// gpu/render_thread.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/render_thread.h"
#include "gpu_capture.h"
#include "gpu/queries.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <condition_variable>
#include <memory>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "core/logging.h"
#include "core/profiling.h"
#include "gpu/device.h"
#include "gpu/draw.h"
#include "gpu/settings.h"
#include "gpu/surfaces.h"
#include "gpu/textures.h"

#if !defined(EOT_D3D12)
#include <plume_vulkan.h>
#endif

#include <rex/cvar.h>
REXCVAR_DECLARE(bool, fable2_zpass_restore_depth);
// Off: with it on the menu and the game flickered (user, 2026-11-07): the game
// thread's next frame overlapped the render thread's end-of-frame work (frame
// slot recycling, texture/buffer retirement, bookkeeping), which is not safe
// against concurrent capture yet.
REXCVAR_DEFINE_BOOL(fable2_scene_keeps_zpass_depth, true, "Fable2",
                    "Native renderer: the scene's opaque draws keep the emulated Z pass's depth (hidden "
                    "fragments, alpha-tested ones too, are rejected before shading; strict depth tests of "
                    "depth-writing draws become or-equal)");
REXCVAR_DEFINE_BOOL(fable2_zpass_skip_late_resolve, true, "Fable2",
                    "Native renderer: the game's own scene depth resolve is skipped when the emulated Z pass "
                    "already made it from the same draws (the scene's colour pass then stays one render pass)");
REXCVAR_DEFINE_BOOL(fable2_render_spin, false, "Fable2",
                    "Native renderer: the render thread spins while it waits for work (old behaviour) "
                    "instead of sleeping in short steps");
REXCVAR_DEFINE_BOOL(fable2_present_early_release, false, "Fable2",
                    "Native renderer: the game thread's Present returns once the frame is submitted, not "
                    "after the render thread's GPU fence wait (flickers: off)");
REXCVAR_DEFINE_BOOL(fable2_zpass_reclear, true, "Fable2",
                    "Native renderer: when the scene depth is just cleared, the emulated Z pass records the "
                    "clear again instead of saving and restoring a copy");

namespace eot::gpu {

namespace {

// Holds a whole scene: the emulated Z pass looks from the shadow mask to the
// scene's depth resolve (~1100 commands) before running either.
constexpr u64 kQueueSlots = 4096;
constexpr u64 kQueueMask = kQueueSlots - 1;
constexpr u64 kBatch = 64;
constexpr u64 kPublishEvery = 8;
// Short spin before the render thread sleeps on the queue: longer spins kept a
// core busy doing nothing for ~0.4 of the thread's time (energy, heat).
constexpr u32 kIdleSpins = 200;
constexpr u32 kIdleSpinsOld = 4000;
constexpr u32 kSortWindow = 512;
constexpr f64 kSortFillMs = 0.08;

struct Queue {
  std::unique_ptr<RenderCommand[]> slots;
  alignas(64) std::atomic<u64> head{0};
  alignas(64) std::atomic<u64> tail{0};
  alignas(64) std::atomic<u64> executed{0};
  alignas(64) std::atomic<bool> consumer_sleeping{false};
  std::atomic<bool> producer_sleeping{false};
  std::atomic<u32> producer_waiting{0};  // the game thread is in RenderThreadWait
  std::atomic<bool> active{false};
  std::atomic<bool> stop{false};
  std::mutex produce_mutex;
  std::mutex cv_mutex;
  std::condition_variable cv_work, cv_space, cv_done;
  alignas(64) u64 next_seq = 1;
  u64 tail_local = 0;
  u64 tail_published = 0;
  u64 head_cache = 0;
};

void PublishTail(Queue &q) {
  if (q.tail_published == q.tail_local)
    return;
  q.tail_published = q.tail_local;
  q.tail.store(q.tail_local, std::memory_order_seq_cst);
  if (q.consumer_sleeping.load(std::memory_order_seq_cst)) {
    std::lock_guard cv_lock(q.cv_mutex);
    q.cv_work.notify_one();
  }
}

Queue &queue() {
  static auto *q = new Queue;
  return *q;
}

// Waits that poll a condition (more draws to sort, the scene's depth resolve)
// sleep in short steps instead of spinning: the render thread has slack, and a
// spinning core costs power for nothing.
void PollPause() {
  if (REXCVAR_GET(fable2_render_spin))
    eot::cpu::Relax();
  else
    std::this_thread::sleep_for(std::chrono::microseconds(25));
}

void PublishExecuted(Queue &q, u64 seq) {
  q.executed.store(seq, std::memory_order_release);
  std::lock_guard lock(q.cv_mutex);
  q.cv_done.notify_all();
}

// The Present being executed: published as soon as its frame is submitted
// (RenderThreadPresentSubmitted), so the game thread starts its next frame
// while the render thread waits for the GPU fence and does its bookkeeping.
u64 g_present_seq = 0;
// The depth resolve the emulated Z pass ran early this frame (its queue seq).
u64 g_prerun_resolve_seq = 0;

void Prefetch(const RenderCommand &c) {
  const auto *base = reinterpret_cast<const char *>(&c);
  for (u32 off = 0; off < offsetof(RenderCommand, draw) + offsetof(DrawPacket, window); off += 64)
    eot::cpu::Prefetch(base + off);
  const auto *image = reinterpret_cast<const char *>(c.draw.window.image);
  for (const DeviceWindow::Block &b : DeviceWindow::kBlocks)
    for (u32 off = 0; off < b.size; off += 64)
      eot::cpu::Prefetch(image + b.base + off);
}

void Execute(VideoState &s, Queue &q, RenderCommand &c) {
  switch (c.type) {
  case RenderCommandType::Draw:
    ReplayDrawLocked(s, c.draw);
    break;
  case RenderCommandType::Clear:
    ReplayClearLocked(s, c.clear);
    break;
  case RenderCommandType::Resolve:
    if (c.seq && c.seq == g_prerun_resolve_seq)
      s.scene_keeps_zpass_depth = false;  // the opaque scene ends here
    if (c.seq && c.seq == g_prerun_resolve_seq && REXCVAR_GET(fable2_zpass_skip_late_resolve) &&
        !(c.resolve.flags & 0x300)) {
      // Same draws, same start depth (put back after the Z pass): same result.
      // No chunk submit here either: it is mid-scene, and a submit would end
      // the colour pass (store + reload of the MSAA targets) all the same.
      s.perf.resolve_noops++;
      PublishExecuted(q, c.seq);
      break;
    }
    ReplayResolveLocked(s, c.resolve);
    PublishExecuted(q, c.seq);  // the hero/dog texture resolves are waited for
    MaybeSubmitChunkLocked(s);
    break;
  case RenderCommandType::Present:
    g_present_seq = c.seq;
    PresentLocked(s, c.va);
    g_present_seq = 0;
    Fable2GpuCaptureOnPresent();  // debugging: Metal frame capture between submitted frames
    PublishExecuted(q, c.seq);
    break;
  case RenderCommandType::Unlock:
    NotifyResourceUnlockedLocked(s, c.va);
    break;
  case RenderCommandType::Retire:
    ParkBuffer(s, std::unique_ptr<plume::RenderBuffer>(c.buffer));
    c.buffer = nullptr;
    break;
  case RenderCommandType::QueryBegin:
    QueryBeginLocked(s, c.va, c.aux);
    break;
  case RenderCommandType::QueryEnd:
    QueryEndLocked(s, c.va, c.aux);
    break;
  }
}

u64 WaitForWork(Queue &q, u64 head) {
  const u32 spins = REXCVAR_GET(fable2_render_spin) ? kIdleSpinsOld : kIdleSpins;
  for (u32 i = 0; i < spins; ++i) {
    const u64 t = q.tail.load(std::memory_order_acquire);
    if (t != head || q.stop.load(std::memory_order_relaxed))
      return t;
    eot::cpu::Relax();
  }
  std::unique_lock lock(q.cv_mutex);
  q.consumer_sleeping.store(true, std::memory_order_seq_cst);
  q.cv_work.wait(lock, [&] {
    return q.tail.load(std::memory_order_seq_cst) != head || q.stop.load(std::memory_order_relaxed);
  });
  q.consumer_sleeping.store(false, std::memory_order_seq_cst);
  return q.tail.load(std::memory_order_acquire);
}

u32 GatherSortRun(Queue &q, u64 head, u64 avail, u64 *keys) {
  const RenderCommand &first = q.slots[head & kQueueMask];
  u32 run = 0;
  u32 first_func = 0;
  for (; run < kSortWindow && run < avail; ++run) {
    const RenderCommand &c = q.slots[(head + run) & kQueueMask];
    if (c.type != RenderCommandType::Draw)
      break;
    if (run && std::memcmp(&c.draw.targets, &first.draw.targets, sizeof(TargetWords)) != 0)
      break;
    u32 func = 0;
    const u64 key = DrawSortKey(c.draw, &func);
    if (!key)
      break;
    if (run == 0)
      first_func = func;
    else if (func != first_func)
      break;
    keys[run] = key;
  }
  return run;
}

void PublishHead(Queue &q, u64 head) {
  q.head.store(head, std::memory_order_seq_cst);
  if (q.producer_sleeping.load(std::memory_order_seq_cst)) {
    std::lock_guard cv_lock(q.cv_mutex);
    q.cv_space.notify_all();
  }
}

// Xbox 360 Z pass, emulated: the console runs the scene depth-only first, so
// Fable 2's screen shadow mask (drawn before the scene) reads this frame's
// depth. Here the trigger draw waits until the scene's depth resolve is
// queued, then the scene's draws are replayed depth-only and that resolve is
// run early; the scene itself then runs as captured. Gives up (old behaviour,
// last frame's depth) if the resolve doesn't arrive quickly or the game
// thread waits on the render thread.
void DepthPrerun(VideoState &s, Queue &q, u64 head) {
  static u64 done_frame = ~0ull;
  static u32 runs = 0, failures = 0;
  if (done_frame == s.guest_frames)
    return;
  done_frame = s.guest_frames;
  const DrawPacket &trigger = q.slots[head & kQueueMask].draw;
  // Everything before the trigger has run: say so before waiting, so the GPU
  // callbacks queued before it reach the game (its command buffer allocator
  // spins on them, and would otherwise wait for this wait).
  if (const u64 before = q.slots[head & kQueueMask].seq; before &&
      q.executed.load(std::memory_order_acquire) < before - 1)
    PublishExecuted(q, before - 1);
  const u64 t0 = PerfNow();
  u64 idx = head + 1, found = 0, last_tail = 0, last_growth = t0;
  const char *why = nullptr;
  for (;;) {
    const u64 tail = q.tail.load(std::memory_order_acquire);
    if (tail != last_tail) {
      last_tail = tail;
      last_growth = PerfNow();
    }
    for (; idx < tail; ++idx) {
      const RenderCommand &c = q.slots[idx & kQueueMask];
      if (c.type == RenderCommandType::Present) {
        why = "frame ended first";
        break;
      }
      if (c.type == RenderCommandType::Resolve && DepthPrerunFeeds(trigger, c.resolve)) {
        found = idx;
        break;
      }
    }
    if (found || why)
      break;
    if (idx - head >= kQueueSlots - 16) {
      why = "scene larger than the queue";
      break;
    }
    if (q.producer_sleeping.load(std::memory_order_acquire) ||
        q.producer_waiting.load(std::memory_order_acquire)) {
      why = "game thread waiting";
      break;
    }
    // The game stopped producing: it is waiting for us (a GPU callback, a
    // resource), so this frame keeps last frame's depth instead.
    if (static_cast<f64>(PerfNow() - last_growth) * PerfMsPerTick() > 1.0) {
      why = "game thread stalled";
      break;
    }
    if (static_cast<f64>(PerfNow() - t0) * PerfMsPerTick() > 30.0) {
      why = "timed out";
      break;
    }
    if (q.stop.load(std::memory_order_relaxed))
      return;
    PollPause();
  }
  if (!found) {
    if (failures++ % 200 < 4)
      EOT_INFO("[zpass] skipped ({}): {} commands looked at (skips {}, runs {})", why ? why : "?", idx - head,
               failures, runs);
    return;
  }
  const ResolvePacket &depth = q.slots[found & kQueueMask].resolve;
  u32 draws = 0, left_out = 0, debug_skipped = 0;
  // The emulated Z pass only feeds the depth resolve the shadow mask reads: the
  // scene then draws on the depth it had, which is put back afterwards, so no
  // draw replayed ahead of it can change what the scene draws (characters'
  // bodies test depth EQUAL against their own earlier depth).
  GuestSurface *dsurf = REXCVAR_GET(fable2_zpass_restore_depth)
                            ? GetGuestSurfaceWords(s, depth.srcVa, depth.srcWords)
                            : nullptr;
  HostTexture *dimg = dsurf && dsurf->isDepth ? (dsurf->host.valid() ? &dsurf->host : &dsurf->single) : nullptr;
  plume::RenderTexture *dimg_texture = dimg ? dimg->texture.get() : nullptr;
  const bool pending_clear = dimg && dimg->needsClear;
  // The scene depth is usually just cleared (a pending lazy clear): then no
  // copy is saved, and the same clear is recorded again after the resolve.
  float known_depth = 0.0f;
  uint32_t known_stencil = 0;
  bool known_clear = false;
#if !defined(EOT_D3D12)
  known_clear = dimg && dimg->valid() && !pending_clear && REXCVAR_GET(fable2_zpass_reclear) &&
                plume::VulkanCommandList::pendingWholeDepthClear(dimg->texture.get(), &known_depth, &known_stencil);
#endif
  static HostTexture spare;
  bool saved = false;
  if (dimg && dimg->valid() && !pending_clear && !known_clear) {
    BeginCommandList(s);
    if (s.command_list_open) {
      if (!spare.texture || spare.width != dimg->width || spare.height != dimg->height ||
          spare.format != dimg->format || spare.sampleCount != dimg->sampleCount) {
        ParkHostTexture(s, spare);
        spare = HostTexture{};
        spare.texture = CreateHostTexture(s.device.get(), dimg->desc, "zpass-depth-save");
        spare.desc = dimg->desc;
        spare.format = dimg->format;
        spare.width = dimg->width;
        spare.height = dimg->height;
        spare.sampleCount = dimg->sampleCount;
        spare.isDepth = true;
        spare.layout = plume::RenderTextureLayout::UNKNOWN;
      }
      if (spare.texture) {
        const HostTextureTransition transitions[] = {{dimg, plume::RenderTextureLayout::COPY_SOURCE},
                                                     {&spare, plume::RenderTextureLayout::COPY_DEST}};
        TransitionManyLocked(s, transitions, 2);
        s.command_list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(spare.texture.get(), 0, 0),
                                          plume::RenderTextureCopyLocation::Subresource(dimg->texture.get(), 0, 0),
                                          0, 0, 0, nullptr);
        s.bound_framebuffer = nullptr;
        s.bound_draw_targets_valid = false;
        saved = true;
      }
    }
  }
  s.depth_prerun = true;
  for (u64 k = head + 1; k < found; ++k) {
    const RenderCommand &c = q.slots[k & kQueueMask];
    if (c.type == RenderCommandType::Draw && c.draw.targets.depthVa == depth.srcVa) {
      if (!DepthPrerunIncludes(c.draw) || !DepthPrerunDebugKeeps(draws + debug_skipped, c.draw)) {
        if (DepthPrerunIncludes(c.draw))
          ++debug_skipped;
        else
          ++left_out;
        continue;
      }
      ReplayDrawLocked(s, c.draw);
      ++draws;
    }
  }
  s.depth_prerun = false;
  ReplayResolveLocked(s, depth);
  g_prerun_resolve_seq = q.slots[found & kQueueMask].seq;
  {
    static u32 logged = 0;
    if (logged++ < 2)
      EOT_INFO("[zpass] scene depth resolve flags {:#x} (late copy {})", depth.flags,
               depth.flags & 0x300 ? "kept: it clears" : "skippable");
  }
  const bool keep_depth = REXCVAR_GET(fable2_scene_keeps_zpass_depth);
  s.scene_keeps_zpass_depth = keep_depth;
  s.scene_depth_va = depth.srcVa;
  if (dimg && dimg->texture.get() == dimg_texture && !keep_depth) {
    if (saved && s.command_list_open) {
      const HostTextureTransition transitions[] = {{&spare, plume::RenderTextureLayout::COPY_SOURCE},
                                                   {dimg, plume::RenderTextureLayout::COPY_DEST}};
      TransitionManyLocked(s, transitions, 2);
      s.command_list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(dimg->texture.get(), 0, 0),
                                        plume::RenderTextureCopyLocation::Subresource(spare.texture.get(), 0, 0),
                                        0, 0, 0, nullptr);
      s.bound_framebuffer = nullptr;
      s.bound_draw_targets_valid = false;
    } else if (known_clear && s.command_list_open) {
      SurfaceClearDepthImage(s, *dsurf, *dimg, known_depth, static_cast<u8>(known_stencil));
      s.bound_framebuffer = nullptr;
      s.bound_draw_targets_valid = false;
    } else if (pending_clear) {
      dimg->needsClear = true;  // the scene clears it again, as it would have
    }
  }
  if (runs++ % 600 == 0)
    EOT_INFO("[zpass] scene depth first: {} draws ({} left out), depth {:#x} -> {:#x}, {} commands ahead, waited "
             "{:.2f} ms",
             draws, left_out, depth.srcVa, depth.destVa, found - head,
             static_cast<f64>(PerfNow() - t0) * PerfMsPerTick());
}

void WorkerMain() {
#if defined(_WIN32)
  SetThreadDescription(GetCurrentThread(), L"reeot render");
#elif defined(__APPLE__)
  pthread_setname_np("reeot render");
#else
  pthread_setname_np(pthread_self(), "reeot render");
#endif
  PinThreadToPhysicalCore(2, "the render thread");
#if defined(EOT_PROFILING) && defined(REXGLUE_ENABLE_PROFILING)
  if (TracyIsStarted)
    tracy::SetThreadName("render");
#endif
  auto &q = queue();
  auto &s = state();
  static u64 sort_keys[kSortWindow];
  static u32 sort_order[kSortWindow];
  u64 head = q.head.load(std::memory_order_relaxed);
  for (;;) {
    u64 tail;
    {
      PerfScope idle_scope(s.perf.worker_idle_ms);
      tail = WaitForWork(q, head);
    }
    if (q.stop.load(std::memory_order_acquire) || s.shutting_down.load(std::memory_order_acquire))
      break;
    u64 avail = tail - head;
    u32 run = GatherSortRun(q, head, avail, sort_keys);
    if (run && run == avail && run < kSortWindow) {
      const u64 t0 = PerfNow();
      while (static_cast<f64>(PerfNow() - t0) * PerfMsPerTick() < kSortFillMs) {
        const u64 t = q.tail.load(std::memory_order_acquire);
        if (t - head > avail) {
          avail = t - head;
          run = GatherSortRun(q, head, avail, sort_keys);
          if (run < avail || run == kSortWindow)
            break;
        }
        if (q.stop.load(std::memory_order_relaxed))
          break;
        PollPause();
      }
    }
    EOT_CPU_ZONE("render batch");
    std::lock_guard lock(s.mutex);
    if (run >= 2) {
      for (u32 i = 0; i < run; ++i)
        sort_order[i] = i;
      std::stable_sort(sort_order, sort_order + run,
                       [&](u32 a, u32 b) { return sort_keys[a] < sort_keys[b]; });
      for (u32 i = 0; i < run; ++i) {
        if (i + 1 < run)
          Prefetch(q.slots[(head + sort_order[i + 1]) & kQueueMask]);
        Execute(s, q, q.slots[(head + sort_order[i]) & kQueueMask]);
      }
      head += run;
      s.perf.sorted_draws += run;
      s.perf.sort_runs++;
      PublishHead(q, head);
      continue;
    }
    u64 n = std::min<u64>(avail, kBatch);
    for (u64 i = run ? 1 : 0; i < n; ++i) {
      const RenderCommand &c = q.slots[(head + i) & kQueueMask];
      if (c.type == RenderCommandType::Draw && DrawSortKey(c.draw)) {
        n = i;
        break;
      }
    }
    if (!n)
      n = 1;
    for (u64 i = 0; i < n; ++i) {
      RenderCommand &c = q.slots[head & kQueueMask];
      if (i + 1 < n)
        Prefetch(q.slots[(head + 1) & kQueueMask]);
      if (c.type == RenderCommandType::Draw && DepthPrerunTrigger(c.draw))
        DepthPrerun(s, q, head);
      s.next_resolve = nullptr;
      if (c.type == RenderCommandType::Resolve && head + 1 < q.tail.load(std::memory_order_acquire)) {
        const RenderCommand &next = q.slots[(head + 1) & kQueueMask];
        if (next.type == RenderCommandType::Resolve)
          s.next_resolve = &next.resolve;
      }
      Execute(s, q, c);
      s.next_resolve = nullptr;
      ++head;
      if ((head & (kPublishEvery - 1)) == 0 || i + 1 == n)
        PublishHead(q, head);
    }
  }
  q.active.store(false, std::memory_order_release);
  std::lock_guard lock(q.cv_mutex);
  q.cv_done.notify_all();
  q.cv_space.notify_all();
}

}

void RenderThreadStart() {
  auto &q = queue();
  if (q.active.load(std::memory_order_acquire))
    return;
  q.slots = std::make_unique<RenderCommand[]>(kQueueSlots);
  q.stop.store(false, std::memory_order_release);
  q.active.store(true, std::memory_order_release);
  std::thread(WorkerMain).detach();
  EOT_INFO("[gpu] render thread started: {} command slots ({} MB), batches of {}", kQueueSlots,
           (kQueueSlots * sizeof(RenderCommand)) >> 20, kBatch);
}

bool RenderThreadActive() { return queue().active.load(std::memory_order_acquire); }

u64 RenderThreadLastCommitted() { return queue().next_seq - 1; }

u64 RenderThreadExecuted() { return queue().executed.load(std::memory_order_acquire); }

void RenderThreadStop() {
  auto &q = queue();
  q.stop.store(true, std::memory_order_release);
  std::lock_guard lock(q.cv_mutex);
  q.cv_work.notify_all();
  q.cv_space.notify_all();
  q.cv_done.notify_all();
}

RenderEnqueue::RenderEnqueue() : lock_(queue().produce_mutex) {
  auto &q = queue();
  const u64 t = q.tail_local;
  if (t - q.head_cache >= kQueueSlots) {
    q.head_cache = q.head.load(std::memory_order_acquire);
    if (t - q.head_cache >= kQueueSlots) {
      EOT_CPU_ZONE("render queue full");
      PublishTail(q);
      std::unique_lock cv_lock(q.cv_mutex);
      q.producer_sleeping.store(true, std::memory_order_seq_cst);
      q.cv_space.wait(cv_lock, [&] {
        return t - q.head.load(std::memory_order_seq_cst) < kQueueSlots ||
               q.stop.load(std::memory_order_relaxed);
      });
      q.producer_sleeping.store(false, std::memory_order_seq_cst);
      q.head_cache = q.head.load(std::memory_order_acquire);
    }
  }
  if (q.stop.load(std::memory_order_acquire)) {
    static RenderCommand scratch;
    cmd_ = &scratch;
    return;
  }
  cmd_ = &q.slots[t & kQueueMask];
  cmd_->seq = q.next_seq;
}

u64 RenderEnqueue::commit(bool publish) {
  auto &q = queue();
  const u64 seq = q.next_seq++;
  if (q.stop.load(std::memory_order_acquire)) {
    committed_ = true;
    return seq;
  }
  cmd_->seq = seq;
  ++q.tail_local;
  if (publish || q.tail_local - q.tail_published >= kPublishEvery ||
      q.consumer_sleeping.load(std::memory_order_relaxed))
    PublishTail(q);
  committed_ = true;
  return seq;
}

RenderEnqueue::~RenderEnqueue() = default;

void RenderThreadPresentSubmitted() {
  // Everything the game thread wrote for this frame has been read by now: the
  // GPU works on host copies, so the next frame's writes cannot reach it.
  if (g_present_seq && REXCVAR_GET(fable2_present_early_release)) {
    PublishExecuted(queue(), g_present_seq);
    g_present_seq = 0;
  }
}

void RenderThreadWait(u64 seq) {
  auto &q = queue();
  if (q.executed.load(std::memory_order_acquire) >= seq)
    return;
  q.producer_waiting.fetch_add(1, std::memory_order_acq_rel);
  {
    std::unique_lock lock(q.cv_mutex);
    q.cv_done.wait(lock, [&] {
      return q.executed.load(std::memory_order_acquire) >= seq ||
             q.stop.load(std::memory_order_relaxed) || !q.active.load(std::memory_order_relaxed);
    });
  }
  q.producer_waiting.fetch_sub(1, std::memory_order_acq_rel);
}

void RenderThreadUnlock(u32 resource_va) {
  RenderEnqueue e;
  e.cmd().type = RenderCommandType::Unlock;
  e.cmd().va = resource_va;
  e.commit();
}

void RenderThreadRetire(plume::RenderBuffer *buffer) {
  if (!buffer)
    return;
  RenderEnqueue e;
  e.cmd().type = RenderCommandType::Retire;
  e.cmd().buffer = buffer;
  e.commit();
}

}
