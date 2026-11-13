// gpu/queries.cpp
// Fable 2: XDK occlusion queries (Query type 9) on host occlusion query
// pools. The game builds its auto-exposure luminance histogram from occlusion
// query sample counts (fullscreen passes that let only pixels over a threshold
// through), so the counts have to be real: answering every query with "all
// samples visible" made it set the scene brightness to zero.
//
// Guest thread: Issue(Begin/End) enqueues QueryBegin/QueryEnd render commands
// in order with the draws; End bumps the query object's generation. Render
// thread: one host pool per frame slot, reset at its first use in a frame;
// queries still open at submit are closed and dropped (begin and end must be
// in one command buffer). When the slot's fence is reached the counts are
// read, scaled to guest samples (render scale squared) and published per
// (query, generation). GetData returns the latest generation's count once
// published and "not ready" until then.

#include "gpu/queries.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/logging.h"
#include <rex/cvar.h>
#include "gpu/device.h"
#include "gpu/render_thread.h"
#include "gpu/settings.h"
#if !defined(EOT_D3D12)
#include <plume_vulkan.h>
#endif
#include "gpu/trace.h"

REXCVAR_DECLARE(bool, fable2_query_force_visible);
REXCVAR_DECLARE(bool, fable2_query_force_exposure);
REXCVAR_DEFINE_BOOL(fable2_query_break_pass, false, "Fable2",
                    "Native renderer: begin and end occlusion queries outside render passes (old behaviour: "
                    "each query ended the pass, an extra store and load of its attachments)");
REXCVAR_DEFINE_BOOL(fable2_query_binary, false, "Fable2",
                    "Native renderer: occlusion queries answer 0 (no host sample) or fully visible");
REXCVAR_DEFINE_BOOL(fable2_query_stale_visible, true, "Fable2",
                    "Native renderer: an occlusion query whose latest count is not in yet reports fully visible");
REXCVAR_DEFINE_INT32(fable2_query_hold, 1, "Fable2",
                     "Native renderer: occlusion queries report the largest of their last N host counts (1-4)");
REXCVAR_DEFINE_BOOL(fable2_query_skip_forced, true, "Fable2",
                    "Native renderer: no host occlusion query for tests reported fully visible anyway");

namespace eot::gpu {

namespace {

constexpr u32 kQueriesPerSlot = 4096;

struct Entry {
  u32 va = 0;
  u32 index = 0;
  u32 generation = 0;
};

struct Slot {
  std::unique_ptr<plume::RenderQueryPool> pool;
  u32 used = 0;
  bool needsReset = true;
  std::vector<Entry> done;
};

struct Active {
  u32 slot = 0;
  u32 index = 0;
  u32 generation = 0;
};

struct Result {
  u32 generation = 0;
  u64 samples = 0;
  // The last counts published for this query, newest first: GetData reports
  // their maximum (fable2_query_hold) so a one-frame zero hides nothing.
  u64 recent[4] = {};
  u32 recentCount = 0;
};

struct State {
  Slot slots[kNumFrames];
  std::unordered_map<u32, Active> active; // render thread
  bool unsupported = false;
  std::mutex mutex; // guest-visible maps
  std::unordered_map<u32, u32> issued;   // query -> generation of its last End
  std::unordered_map<u32, Result> results;
  std::unordered_map<u32, bool> exposure;  // query -> wraps fullscreen passes
  // Sun visibility queries (QueryMarkSun): read every frame before that frame's
  // count can be in, so a stale read gets the previous count of the same query.
  std::unordered_map<u32, bool> sun;
};

// Guest thread: the query between Begin and End, if any.
std::atomic<u32> g_open_query{0};

State &st() {
  static State s;
  return s;
}

bool DiagNow(const VideoState &s) {
  return Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame());
}

// Temporary diagnostics: counters logged every few seconds.
std::atomic<u64> g_issued_begin{0}, g_issued_end{0}, g_host_begin{0}, g_host_end{0},
    g_dropped_open{0}, g_published{0}, g_get_hit{0}, g_get_stale{0}, g_get_miss{0};
std::atomic<u64> g_query_flicker{0}, g_query_zero{0};
std::atomic<u64> g_sample_sum{0}, g_orig_ready{0}, g_orig_notready{0}, g_nonocc{0};

void MaybeLogStats() {
  using clock = std::chrono::steady_clock;
  static std::atomic<i64> last{0};
  const i64 now = std::chrono::duration_cast<std::chrono::seconds>(
                      clock::now().time_since_epoch()).count();
  i64 prev = last.load();
  if (now - prev < 5 || !last.compare_exchange_strong(prev, now))
    return;
  EOT_INFO("[queries] issued b{} e{} host b{} e{} dropped-open {} published {} (sum {}) "
           "get hit {} stale {} miss {} | GetData orig ready {} not-ready {} non-occlusion {}",
           g_issued_begin.load(), g_issued_end.load(), g_host_begin.load(), g_host_end.load(),
           g_dropped_open.load(), g_published.load(), g_sample_sum.load(), g_get_hit.load(),
           g_get_stale.load(), g_get_miss.load(), g_orig_ready.load(), g_orig_notready.load(),
           g_nonocc.load());
}

}

namespace {
// Queries stay inside the render pass unless fable2_query_break_pass; the
// pass is then left bound (no restart around each query).
bool QueriesBreakPass(VideoState &s) {
  const bool brk = REXCVAR_GET(fable2_query_break_pass);
#if !defined(EOT_D3D12)
  static_cast<plume::VulkanCommandList *>(s.command_list)->queriesEndRenderPass = brk;
#endif
  return brk;
}
}  // namespace

// A new command list: reset the frame's query slot now, while no render pass
// is open (a reset inside one would end it).
void QueriesCommandListBegunLocked(VideoState &s) {
  auto &q = st();
  if (q.unsupported || !s.command_list)
    return;
  Slot &slot = q.slots[s.recording_slot()];
  if (slot.pool && slot.needsReset) {
    s.command_list->resetQueryPool(slot.pool.get(), 0, kQueriesPerSlot);
    slot.needsReset = false;
  }
}

void QueryBeginLocked(VideoState &s, u32 query_va, u32 generation) {
  auto &q = st();
  if (q.unsupported || !s.command_list)
    return;
  // With fable2_query_force_visible only exposure tests read host counts
  // (fable2_vblank.cpp); the others need no host query, and each one ended
  // the render pass twice (begin, end): ~45 restarts per frame in Bowerstone.
  // A query's exposure flag is known from its first frame on.
  if (REXCVAR_GET(fable2_query_skip_forced) && REXCVAR_GET(fable2_query_force_visible) &&
      !REXCVAR_GET(fable2_query_force_exposure) && !QueryIsExposure(query_va) && !QueryIsSun(query_va))
    return;
  const u32 slot_index = s.recording_slot();
  Slot &slot = q.slots[slot_index];
  if (!slot.pool) {
    slot.pool = s.device->createOcclusionQueryPool(kQueriesPerSlot);
    if (!slot.pool) {
      q.unsupported = true;
      EOT_WARN("[queries] no occlusion query pools; queries report not ready");
      return;
    }
  }
  if (slot.needsReset) {
    s.command_list->resetQueryPool(slot.pool.get(), 0, kQueriesPerSlot);
    slot.needsReset = false;
    s.bound_framebuffer = nullptr;
  }
  if (slot.used >= kQueriesPerSlot)
    return;
  if (q.active.count(query_va))
    return; // begun twice: keep the first
  const u32 index = slot.used++;
  if (QueriesBreakPass(s))
    s.bound_framebuffer = nullptr;
  s.command_list->beginQuery(slot.pool.get(), index);
  ++g_host_begin;
  q.active[query_va] = {slot_index, index, generation};
  if (DiagNow(s))
    EOT_INFO("[diag] query {:#x} begin (slot {} index {})", query_va, slot_index, index);
}

void QueryEndLocked(VideoState &s, u32 query_va, u32 generation) {
  auto &q = st();
  auto it = q.active.find(query_va);
  if (it == q.active.end())
    return;
  const Active a = it->second;
  q.active.erase(it);
  Slot &slot = q.slots[a.slot];
  if (QueriesBreakPass(s))
    s.bound_framebuffer = nullptr;
  s.command_list->endQuery(slot.pool.get(), a.index);
  ++g_host_end;
  if (DiagNow(s))
    EOT_INFO("[diag] query {:#x} end", query_va);
  if (a.slot == s.recording_slot())
    slot.done.push_back({query_va, a.index, generation});
}

bool QueryActiveLocked() { return !st().active.empty(); }

void QueriesBeforeSubmitLocked(VideoState &s) {
  auto &q = st();
  if (q.active.empty())
    return;
  for (auto &[va, a] : q.active) {
    s.command_list->endQuery(q.slots[a.slot].pool.get(), a.index);
    ++g_dropped_open;
  }
  q.active.clear();
}

void QueriesSlotRecycledLocked(VideoState &s, u32 slot_index) {
  (void)s;
  auto &q = st();
  Slot &slot = q.slots[slot_index];
  if (slot.pool && !slot.done.empty()) {
    std::vector<u64> raw(slot.used, 0);
    if (slot.pool->getRawResults(0, slot.used, raw.data())) {
      const float k = RenderScaleFactor();
      const double scale = k > 0.0f ? 1.0 / (double(k) * double(k)) : 1.0;
      std::lock_guard lock(q.mutex);
      for (const Entry &e : slot.done) {
        // Host samples at the render scale, rounded; any visible sample is at
        // least 1 (rounding a sliver to 0 hid distant buildings).
        const u64 n = raw[e.index] ? std::max<u64>(1, static_cast<u64>(double(raw[e.index]) * scale + 0.5)) : 0;
        auto &r = q.results[e.va];
        if (e.generation >= r.generation) {
          // Flicker statistics: a 0 between two nonzero counts of the same query.
          if (r.recentCount >= 2 && n != 0 && r.recent[0] == 0 && r.recent[1] != 0)
            ++g_query_flicker;
          if (n == 0)
            ++g_query_zero;
          r.generation = e.generation;
          r.samples = n;
          for (u32 i = 3; i > 0; --i)
            r.recent[i] = r.recent[i - 1];
          r.recent[0] = n;
          r.recentCount = std::min(r.recentCount + 1, 4u);
        }
        ++g_published;
        g_sample_sum += n;
      }
    }
  }
  slot.done.clear();
  slot.used = 0;
  slot.needsReset = true;
}

void QueryFlickerStats(u64 *flicker, u64 *zero, u64 *published) {
  *flicker = g_query_flicker.exchange(0);
  *zero = g_query_zero.exchange(0);
  static u64 last_published = 0;
  const u64 now = g_published.load();
  *published = now - last_published;
  last_published = now;
}

u32 QueryOpenVa() { return g_open_query.load(std::memory_order_relaxed); }

void QueryNoteDraw(bool rect_list) {
  const u32 open = g_open_query.load(std::memory_order_relaxed);
  if (!open || !rect_list)
    return;
  auto &q = st();
  std::lock_guard lock(q.mutex);
  q.exposure[open] = true;
}

void QueryMarkSun(u32 query_va) {
  auto &q = st();
  std::lock_guard lock(q.mutex);
  if (!q.sun[query_va])
    EOT_INFO("[queries] sun query {:#x}", query_va);
  q.sun[query_va] = true;
}

bool QueryIsSun(u32 query_va) {
  auto &q = st();
  std::lock_guard lock(q.mutex);
  auto it = q.sun.find(query_va);
  return it != q.sun.end() && it->second;
}

bool QueryIsExposure(u32 query_va) {
  auto &q = st();
  std::lock_guard lock(q.mutex);
  auto it = q.exposure.find(query_va);
  return it != q.exposure.end() && it->second;
}

void QueryIssued(u32 query_va, bool begin, bool end) {
  if (begin && !end)
    g_open_query.store(query_va, std::memory_order_relaxed);
  else if (end && g_open_query.load(std::memory_order_relaxed) == query_va)
    g_open_query.store(0, std::memory_order_relaxed);
  auto &q = st();
  MaybeLogStats();
  EOT_TRACE_CALL("Query 0x{:x}{}{}", query_va, begin ? " begin" : "", end ? " end" : "");
  if (begin)
    ++g_issued_begin;
  if (end)
    ++g_issued_end;

  u32 generation = 0;
  {
    std::lock_guard lock(q.mutex);
    u32 &g = q.issued[query_va];
    if (end)
      ++g;
    generation = g + (begin && !end ? 1 : 0);
  }
  auto push = [&](RenderCommandType type) {
    RenderEnqueue enqueue;
    enqueue.cmd().type = type;
    enqueue.cmd().va = query_va;
    enqueue.cmd().aux = generation;
    enqueue.commit();
  };
  if (begin)
    push(RenderCommandType::QueryBegin);
  if (end)
    push(RenderCommandType::QueryEnd);
}

void QueryGetDataCalled(bool orig_ready, bool occlusion) {
  if (!occlusion)
    ++g_nonocc;
  else if (orig_ready)
    ++g_orig_ready;
  else
    ++g_orig_notready;
  MaybeLogStats();
}

bool QueryResult(u32 query_va, u64 *samples) {
  auto &q = st();
  std::lock_guard lock(q.mutex);
  auto issued = q.issued.find(query_va);
  auto result = q.results.find(query_va);
  const auto sun = q.sun.find(query_va);
  const bool is_sun = sun != q.sun.end() && sun->second;
  if (issued == q.issued.end() || result == q.results.end()) {
    ++g_get_miss;
    // A sun query's first uses were not counted on the host (forced visible
    // before it was known as one), and the game reissues it only once it is
    // answered: answer "hidden" until its first count is in.
    if (is_sun && issued != q.issued.end()) {
      *samples = 0;
      return true;
    }
    // No count yet for this query object: its host query may never come
    // (skipped while every test was forced visible, then
    // fable2_query_force_visible switched off: the game polled GetData
    // forever). Same answer as a stale read: fully visible.
    if (REXCVAR_GET(fable2_query_stale_visible) && !is_sun) {
      *samples = 1280 * 720;
      return true;
    }
    return false;
  }
  // The game reissues its queries every frame, usually before the host GPU has
  // produced the previous frame's counts; the newest published count (a frame
  // or two old) stands in for the current one.
  if (result->second.generation != issued->second) {
    ++g_get_stale;
    // Fable 2 reuses query objects for different objects; the count of an
    // earlier use may be another object's. Until this use's count is in,
    // report fully visible (never hides anything, never stalls the game).
    // Not for the sun queries: the game reads them every frame before their
    // count is in, so "fully visible" was their only answer (the sun glare
    // over Lucian's castle hall).
    if (REXCVAR_GET(fable2_query_stale_visible) && !is_sun) {
      *samples = 1280 * 720;
      return true;
    }
  } else {
    ++g_get_hit;
  }
  // Hold: report the largest of the last fable2_query_hold counts. Real
  // counts hide light glows behind walls (the white haze in Lucian's castle
  // with every test forced visible), but a single frame's zero made objects
  // and textures pop in and out while the camera moved.
  const u32 hold = static_cast<u32>(std::clamp(REXCVAR_GET(fable2_query_hold), 1, 4));
  u64 best = result->second.samples;
  for (u32 i = 0; i < std::min(hold, result->second.recentCount); ++i)
    best = std::max(best, result->second.recent[i]);
  // Fable 2's query boxes test depth ALWAYS: they ask whether the box is on
  // screen at all. Binary mode answers that (0 off screen, fully visible
  // otherwise), so small distant boxes are never hidden by their size.
  if (REXCVAR_GET(fable2_query_binary))
    best = best ? 1280u * 720u : 0u;
  *samples = best;
  return true;
}

}
