// gpu/pipeline/pso_precache.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/pipeline/pso_precache.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "core/logging.h"
#include "gpu/device.h"
#include "gpu/pipeline/pso_records.h"

namespace eot::gpu {

namespace {

struct WorkItem {
  PsoRecord rec;
  PsoSource source = PsoSource::Draw;
  PsoLane lane = PsoLane::Background;
  u64 key = 0;
  TokenPtr token;
  TokenPtr screenToken;
};

struct Pool {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<WorkItem> lanes[3];
  std::vector<std::thread> threads;
  bool started = false, stop = false;
  std::atomic<bool> loading{false};
  std::mutex screenMutex;
  TokenPtr screenToken;

  struct Known {
    PsoSource source;
    PsoLane lane;
    bool taken;
  };
  std::mutex dedupMutex;
  std::unordered_map<u64, Known> queuedOrDone;

  std::atomic<u32> queued{0}, built{0}, existing{0}, skipped{0}, failed{0};
};

Pool &pool() {
  static Pool p;
  return p;
}

thread_local TokenPtr t_loadToken;

void SetWorkerPriority(bool loading) {
#if defined(_WIN32)
  ::SetThreadPriority(::GetCurrentThread(),
                      loading ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__)
  static thread_local bool niced = false;
  if (!niced) {
    ::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 5);
    niced = true;
  }
  sched_param param{};
  param.sched_priority = 0;
  ::pthread_setschedparam(::pthread_self(), loading ? SCHED_OTHER : SCHED_IDLE, &param);
#else
  (void)loading;
#endif
}

void ProcessItem(WorkItem &item) {
  auto &p = pool();
  auto &s = state();
  switch (BuildPipelineFromRecord(s, item.rec, item.source)) {
  case PsoBuildResult::Built:
    p.built++;
    break;
  case PsoBuildResult::Existing:
    p.existing++;
    break;
  case PsoBuildResult::Skipped:
    p.skipped++;
    break;
  case PsoBuildResult::Failed:
    p.failed++;
    break;
  }
  if (item.token)
    item.token->ReleasePending();
  if (item.screenToken)
    item.screenToken->ReleasePending();
}

void WorkerLoop() {
  auto &p = pool();
  bool priority_loading = false;
  SetWorkerPriority(priority_loading);
  for (;;) {
    WorkItem item;
    {
      std::unique_lock lock(p.mutex);
      p.cv.wait(lock, [&] {
        return p.stop || !p.lanes[0].empty() || !p.lanes[1].empty() || !p.lanes[2].empty();
      });
      if (p.stop && p.lanes[0].empty() && p.lanes[1].empty() && p.lanes[2].empty())
        return;
      for (auto &lane : p.lanes) {
        if (!lane.empty()) {
          item = std::move(lane.front());
          lane.pop_front();
          break;
        }
      }
    }
    const bool loading = p.loading.load(std::memory_order_relaxed);
    if (loading != priority_loading) {
      priority_loading = loading;
      SetWorkerPriority(loading);
    }
    {
      std::lock_guard lock(p.dedupMutex);
      auto it = p.queuedOrDone.find(item.key);
      if (it != p.queuedOrDone.end()) {
        if (it->second.lane != item.lane) {
          if (item.token)
            item.token->ReleasePending();
          if (item.screenToken)
            item.screenToken->ReleasePending();
          continue;
        }
        it->second.taken = true;
      }
    }
    ProcessItem(item);
  }
}

}

void PsoPrecacheStart() {
  auto &p = pool();
  std::lock_guard lock(p.mutex);
  if (p.started)
    return;
  p.started = true;
  p.stop = false;
  const u32 physical = PhysicalCoreCount();
  const u32 count = std::clamp(physical > 2 ? physical - 2 : 1u, kPsoMinThreads, kPsoMaxThreads);
  for (u32 i = 0; i < count; ++i)
    p.threads.emplace_back(WorkerLoop);
  EOT_INFO("[pso] {} pipeline worker thread(s) for {} physical cores", count, physical);
}

void PsoPrecacheStop() {
  auto &p = pool();
  std::vector<std::thread> threads;
  {
    std::lock_guard lock(p.mutex);
    if (!p.started)
      return;
    p.stop = true;
    for (auto &q : p.lanes) {
      for (auto &item : q) {
        if (item.token)
          item.token->ReleasePending();
        if (item.screenToken)
          item.screenToken->ReleasePending();
      }
      q.clear();
    }
    threads.swap(p.threads);
  }
  p.cv.notify_all();
  for (auto &t : threads)
    if (t.joinable())
      t.join();
  std::lock_guard lock(p.mutex);
  p.started = false;
}

void PsoPrecacheSetLoading(bool loading) {
  auto &p = pool();
  p.loading.store(loading, std::memory_order_relaxed);
  std::lock_guard lock(p.screenMutex);
  if (loading)
    p.screenToken = std::make_shared<CompileToken>();
  else
    p.screenToken.reset();
}

TokenPtr PsoPrecacheScreenToken() {
  auto &p = pool();
  std::lock_guard lock(p.screenMutex);
  return p.screenToken;
}

bool PsoPrecacheEnqueue(const PsoRecord &rec, PsoSource source, PsoLane lane, TokenPtr token) {
  auto &p = pool();
  const u64 key = HashPipelineState(rec.state);
  {
    std::lock_guard lock(p.dedupMutex);
    auto [it, fresh] = p.queuedOrDone.try_emplace(key, Pool::Known{source, lane, false});
    if (!fresh) {
      if (it->second.taken || it->second.lane <= lane)
        return false;
      it->second.lane = lane;
      it->second.source = source;
    }
  }
  PsoPrecacheStart();
  TokenPtr screen;
  if (lane != PsoLane::Background)
    screen = PsoPrecacheScreenToken();
  if (token)
    token->AddPending();
  if (screen)
    screen->AddPending();
  {
    std::lock_guard lock(p.mutex);
    if (p.stop) {
      if (token)
        token->ReleasePending();
      if (screen)
        screen->ReleasePending();
      return false;
    }
    p.lanes[static_cast<u32>(lane)].push_back(
        WorkItem{rec, source, lane, key, std::move(token), std::move(screen)});
  }
  p.queued++;
  p.cv.notify_one();
  return true;
}

void PsoPrecacheBeginLoad() { t_loadToken = std::make_shared<CompileToken>(); }

TokenPtr PsoPrecacheCurrentToken() { return t_loadToken; }

bool PsoPrecacheWaitLoad(u32 max_ms) {
  TokenPtr token = t_loadToken;
  if (!token || token->Pending() == 0)
    return true;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_ms);
  while (token->Pending() != 0) {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

void PsoPrecacheEndLoad() { t_loadToken.reset(); }

bool PsoPrecacheKnown(u64 key, PsoSource *source) {
  auto &p = pool();
  std::lock_guard lock(p.dedupMutex);
  auto it = p.queuedOrDone.find(key);
  if (it == p.queuedOrDone.end())
    return false;
  if (source)
    *source = it->second.source;
  return true;
}

PsoPrecacheStats PsoPrecacheGetStats() {
  auto &p = pool();
  PsoPrecacheStats st;
  st.queued = p.queued.load();
  st.built = p.built.load();
  st.existing = p.existing.load();
  st.skipped = p.skipped.load();
  st.failed = p.failed.load();
  std::lock_guard lock(p.mutex);
  st.recordedPending = static_cast<u32>(p.lanes[0].size());
  st.priorityPending = static_cast<u32>(p.lanes[1].size());
  st.backgroundPending = static_cast<u32>(p.lanes[2].size());
  st.threads = static_cast<u32>(p.threads.size());
  return st;
}

}
