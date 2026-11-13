// Fable 2: guest vblank interrupts for the native renderer.
//
// Without the GPU emulation plugin the kernel ignores
// VdSetGraphicsInterruptCallback, so the game's vblank callback never runs and
// its menu animations crawl. This replaces the import in the executable,
// keeps the callback and calls it from a guest thread at the guest refresh
// rate, as the plugin's vsync worker does (GraphicsSystem::MarkVblank), plus
// the command processor interrupt for the XDK's inserted callbacks.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mmio_handler.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include "gpu/queries.h"
#include "gpu/render_thread.h"
#include "gpu/settings.h"

REXCVAR_DEFINE_BOOL(fable2_deliver_gpu_callbacks, true, "Fable2",
                    "Native renderer: deliver each InsertCallback to the guest interrupt handler once "
                    "the render thread has passed it (the GPU's copy of the callback into the device's "
                    "callback block)");
REXCVAR_DEFINE_BOOL(fable2_query_force_exposure, false, "Fable2",
                    "Debug: exposure (rect-list) occlusion queries also report fully visible");
REXCVAR_DEFINE_BOOL(fable2_ring_opcode_log, false, "Fable2",
                    "Diagnostic: scan the command ring and log PM4 opcode counts");
REXCVAR_DEFINE_BOOL(fable2_ring_wptr_direct, true, "Fable2",
                    "Native renderer: read the ring write pointer through the MMIO handler directly, not by a "
                    "faulting load of the register address");
REXCVAR_DEFINE_BOOL(fable2_query_sun_counts, true, "Fable2",
                    "Native renderer: the sun glare's occlusion queries read real counts (also with "
                    "fable2_query_force_visible)");
// Off by default since 2026-11-13: with queries kept inside render passes,
// real counts let the game skip hidden objects (town: -11% draws, frames
// ~1.4-2 ms shorter); play-tested through the prologue without popping.
REXCVAR_DEFINE_BOOL(fable2_query_force_visible, false, "Fable2",
                    "Native renderer: report every occlusion test as fully visible (no occlusion culling)");
REXCVAR_DEFINE_BOOL(fable2_query_visible, true, "Fable2",
                    "Native renderer: answer occlusion queries from host occlusion queries");

void Fable2RetireCommandRing();

namespace {

constexpr uint32_t kVblankStatus = 0x7FC86544;

std::atomic<uint32_t> g_callback{0};
std::atomic<uint32_t> g_user_data{0};
rex::system::object_ref<rex::system::XHostThread> g_thread;

// The XDK compares fences with the completed fence the GPU writes to the
// first word of the write-back block (device +10896; the next fence is at
// device +10908, InsertFence adds 2). The native renderer takes in the work as
// it is issued, so every inserted fence is reported complete.
constexpr uint32_t kDeviceWriteBack = 10896;
constexpr uint32_t kDeviceNextFence = 10908;

void RetireFences() {
  const uint32_t device = g_user_data.load(std::memory_order_relaxed);
  if (!device) {
    return;
  }
  auto* memory = REX_KERNEL_MEMORY();
  const uint32_t write_back = rex::memory::load_and_swap<uint32_t>(
      memory->TranslateVirtual<uint8_t*>(device + kDeviceWriteBack));
  if (!write_back) {
    return;
  }
  const uint32_t next = rex::memory::load_and_swap<uint32_t>(
      memory->TranslateVirtual<uint8_t*>(device + kDeviceNextFence));
  rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(write_back), next - 2);
}

void Dispatch(uint32_t source) {
  const uint32_t callback = g_callback.load(std::memory_order_acquire);
  if (!callback) {
    return;
  }
  // The XDK handler runs its vblank work (vblank count, swap bookkeeping) only
  // when D1MODE_VBLANK_STATUS (GPU register 0x1951, 0x7FC86544) reads 1, as
  // the plugin's register read returns; without the plugin it is plain memory.
  if (auto* status = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(kVblankStatus)) {
    status[0] = status[1] = status[2] = 0;
    status[3] = 1;
  }
  auto* thread = rex::system::XThread::GetCurrentThread();
  thread->SetActiveCpu(2);
  uint64_t args[] = {source, g_user_data.load(std::memory_order_relaxed)};
  REX_KERNEL_STATE()->function_dispatcher()->ExecuteInterrupt(thread->thread_state(), callback,
                                                              args, 2);
}

// Fable 2: InsertCallback (sub_821F6050) writes the callback and its argument
// into GPU scratch registers and has the GPU copy them into the device's
// callback block (device +10900: +16 function, +20 argument) before raising
// interrupt source 1; the guest handler calls that one function (D3D's thunk
// queues the argument for its callback thread, 4 entries per CPU). Without the
// GPU the block kept stale values, so the game's callbacks (they free its
// dynamic command buffer) were not delivered and its allocations stalled for
// hundreds of milliseconds. The native renderer has copied what the commands
// before a callback use once its render thread has executed past them.
struct PendingCallback {
  uint32_t function, argument;
  uint64_t seq;
};
std::mutex g_callback_mutex;
std::deque<PendingCallback> g_pending_callbacks;
std::atomic<uint64_t> g_callbacks_queued{0}, g_callbacks_delivered{0};

// Delivers at most one due callback; true when one was delivered.
bool DeliverOneCallback() {
  const uint32_t device = g_user_data.load(std::memory_order_relaxed);
  if (!device || !g_callback.load(std::memory_order_acquire)) {
    return false;
  }
  PendingCallback cb;
  {
    std::lock_guard lock(g_callback_mutex);
    if (g_pending_callbacks.empty()) {
      return false;
    }
    cb = g_pending_callbacks.front();
    if (eot::gpu::RenderThreadActive() && eot::gpu::RenderThreadExecuted() < cb.seq) {
      return false;
    }
    g_pending_callbacks.pop_front();
  }
  auto* memory = REX_KERNEL_MEMORY();
  const uint32_t block =
      rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(device + 10900));
  if (!block) {
    return false;
  }
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("Fable 2 GPU callbacks: block {:08X} held function {:08X} argument {:08X}; first {:08X}({:08X})",
                block,
                rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(block + 16)),
                rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(block + 20)),
                cb.function, cb.argument);
  }
  rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(block + 16), cb.function);
  rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(block + 20), cb.argument);
  Dispatch(1);
  rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(block + 16), 0);
  g_callbacks_delivered.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void StartThread() {
  auto* kernel_state = REX_KERNEL_STATE();
  g_thread = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(kernel_state, 128 * 1024, 0, [] {
        rex::system::X_VIDEO_MODE video_mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
        const double refresh_hz = std::max(1.0, double(float(video_mode.refresh_rate)));
        const uint64_t frequency = rex::chrono::Clock::guest_tick_frequency();
        const uint64_t interval = std::max(uint64_t(1), uint64_t(double(frequency) / refresh_hz));
        REXLOG_INFO("Fable 2 vblank: {} Hz guest interrupts", refresh_hz);
        uint64_t last = rex::chrono::Clock::QueryGuestTickCount();
        for (;;) {
          const uint64_t now = rex::chrono::Clock::QueryGuestTickCount();
          while (now - last >= interval) {
            // Source 1 is the command processor interrupt the GPU raises at the
            // callbacks the XDK inserts in the command stream (InsertCallback);
            // Fable 2 retires its dynamic ring buffers in them. The native
            // renderer has consumed the work by now, so run them every vblank.
            RetireFences();
            Fable2RetireCommandRing();
            Dispatch(1);
            Dispatch(0);
            last += interval;
          }
          if (REXCVAR_GET(fable2_deliver_gpu_callbacks)) {
            DeliverOneCallback();
            static uint64_t logged_at = 0;
            const uint64_t delivered = g_callbacks_delivered.load(std::memory_order_relaxed);
            if (delivered >= logged_at + 2000) {
              logged_at = delivered;
              std::lock_guard lock(g_callback_mutex);
              REXLOG_INFO("Fable 2 GPU callbacks: {} queued, {} delivered, {} pending",
                          g_callbacks_queued.load(std::memory_order_relaxed), delivered,
                          g_pending_callbacks.size());
            }
          }
          rex::thread::Sleep(std::chrono::milliseconds(1));
        }
        return 0;
      }));
  g_thread->set_name("Fable2 VBlank");
  g_thread->Create();
}

}  // namespace

// VdSetGraphicsInterruptCallback(callback, user_data).
extern "C" REX_FUNC(__imp__VdSetGraphicsInterruptCallback) {
  const uint32_t callback = ctx.r3.u32;
  const uint32_t user_data = ctx.r4.u32;
  REXLOG_INFO("Fable 2 vblank: interrupt callback {:08X} data {:08X}", callback, user_data);
  g_user_data.store(user_data, std::memory_order_relaxed);
  const bool first = g_callback.exchange(callback, std::memory_order_acq_rel) == 0 && !g_thread;
  if (first && callback) {
    StartThread();
  }
  (void)base;
}

namespace {

// The D3D command ring. The XDK still writes its commands into it (the
// renderer's hooks call the original functions) and kicks them by writing
// CP_RB_WPTR; the GPU reports how far it has read through the read pointer
// write-back. Without the plugin both kernel calls are ignored and nothing
// reports progress, so the ring fills and the game waits (the menu freezes).
// The native renderer has taken in everything kicked, so the read pointer is
// reported equal to the write pointer.
constexpr uint32_t kCpRbWptr = 0x7FC80000 + 0x01C5 * 4;
std::atomic<uint32_t> g_rptr_write_back{0};  // physical address
std::atomic<uint32_t> g_ring_base{0}, g_ring_dwords{0};

// Temporary (missing ground): PM4 type-3 opcodes passing through the ring,
// to see whether the game runs command buffers (INDIRECT_BUFFER 0x3F) or
// draws (DRAW_INDX 0x22, DRAW_INDX_2 0x36) the hooks never see.
void ScanRing(uint32_t from, uint32_t to) {
  static uint64_t counts[128] = {};
  static uint32_t calls = 0;
  const uint32_t base = g_ring_base.load(std::memory_order_relaxed);
  const uint32_t n = g_ring_dwords.load(std::memory_order_relaxed);
  if (!base || !n)
    return;
  auto* memory = REX_KERNEL_MEMORY();
  auto rd = [&](uint32_t i) {
    return rex::memory::load_and_swap<uint32_t>(memory->TranslatePhysical<uint8_t*>(base + (i % n) * 4));
  };
  uint32_t i = from;
  for (uint32_t guard = 0; i != to && guard < n; ++guard) {
    const uint32_t h = rd(i);
    const uint32_t type = h >> 30;
    if (type == 3) {
      counts[(h >> 8) & 0x7F]++;
      i = (i + 2 + ((h >> 16) & 0x3FFF)) % n;
    } else if (type == 0) {
      // Register writes: count the draw initiator (a draw without a packet).
      const uint32_t reg = h & 0x7FFF, cnt = ((h >> 16) & 0x3FFF) + 1;
      if (reg <= 0x21FC && 0x21FC < reg + cnt)
        counts[127]++;
      i = (i + 2 + ((h >> 16) & 0x3FFF)) % n;
    } else if (type == 1) {
      i = (i + 3) % n;
    } else {
      i = (i + 1) % n;
    }
  }
  if (++calls % 120 == 0) {
    std::string line;
    for (uint32_t op = 0; op < 128; ++op)
      if (counts[op])
        line += std::format(" {:02X}:{}", op, counts[op]);
    REXLOG_INFO("Fable 2 ring opcodes:{}", line);
  }
}

}  // namespace

void Fable2RetireCommandRing() {
  const uint32_t write_back = g_rptr_write_back.load(std::memory_order_relaxed);
  if (!write_back) {
    return;
  }
  auto* memory = REX_KERNEL_MEMORY();
  // The register page is MMIO: a plain load faults into the exception handler
  // (heap lookups, decode) on every "make space" call (~3% of the game's
  // rendering thread in Bowerstone town). The handler's own lookup is direct.
  uint32_t wptr = 0;
  auto* mmio = rex::runtime::MMIOHandler::global_handler();
  if (!REXCVAR_GET(fable2_ring_wptr_direct) || !mmio || !mmio->CheckLoad(kCpRbWptr, &wptr))
    wptr = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(kCpRbWptr));
  static uint32_t checks = 0;
  if (checks < 3 && mmio && wptr) {
    ++checks;
    uint32_t direct = 0;
    mmio->CheckLoad(kCpRbWptr, &direct);
    const uint32_t faulting =
        rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(kCpRbWptr));
    REXLOG_INFO("Fable 2 ring write pointer: direct {:#x}, faulting load {:#x}{}", direct, faulting,
                direct == faulting ? "" : " (DIFFER)");
  }
  static uint32_t last_wptr = 0;
  if (wptr != last_wptr) {
    if (REXCVAR_GET(fable2_ring_opcode_log))
      ScanRing(last_wptr, wptr);
    last_wptr = wptr;
  }
  // A store to a write-watched physical page faults too: store only changes.
  auto *wb = memory->TranslatePhysical<uint8_t*>(write_back);
  if (!REXCVAR_GET(fable2_ring_wptr_direct) || rex::memory::load_and_swap<uint32_t>(wb) != wptr)
    rex::memory::store_and_swap<uint32_t>(wb, wptr);
}

// VdInitializeRingBuffer(physical base, log2 size).
extern "C" REX_FUNC(__imp__VdInitializeRingBuffer) {
  REXLOG_INFO("Fable 2 command ring: base {:08X}, {} bytes", ctx.r3.u32, 1u << ctx.r4.u32);
  g_ring_base.store(ctx.r3.u32, std::memory_order_relaxed);
  g_ring_dwords.store((1u << ctx.r4.u32) / 4, std::memory_order_relaxed);
  (void)base;
}

// VdEnableRingBufferRPtrWriteBack(physical address, log2 block size).
extern "C" REX_FUNC(__imp__VdEnableRingBufferRPtrWriteBack) {
  REXLOG_INFO("Fable 2 command ring: read pointer write-back at {:08X}", ctx.r3.u32);
  g_rptr_write_back.store(ctx.r3.u32, std::memory_order_relaxed);
  (void)base;
}

// The XDK's "make space in the command ring" (waits for the read pointer).
REX_EXTERN(__imp__sub_821E8EC0);
extern "C" REX_FUNC(sub_821E8EC0) {
  Fable2RetireCommandRing();
  __imp__sub_821E8EC0(ctx, base);
}

// The game's ring buffer allocators refresh their GPU-consumed position here
// before waiting for space; report the GPU's progress first.
REX_EXTERN(__imp__sub_821A5BD0);
extern "C" REX_FUNC(sub_821A5BD0) {
  RetireFences();
  Fable2RetireCommandRing();
  __imp__sub_821A5BD0(ctx, base);
}

// Query::GetData(query, data, size, flags). Occlusion queries (type 9) read
// sample counts the GPU writes over 0xFFFFFEED markers; without the GPU they
// stay "not ready" (S_FALSE) forever. Report them ready and visible.
REX_EXTERN(__imp__sub_8220BD40);
extern "C" REX_FUNC(sub_8220BD40) {
  const uint32_t query = ctx.r3.u32, data = ctx.r4.u32, size = ctx.r5.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_8220BD40(ctx, base);
  if (!REXCVAR_GET(fable2_query_visible) || !query || !data) {
    return;
  }
  auto* memory = REX_KERNEL_MEMORY();
  const uint32_t type =
      rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(query + 4));
  eot::gpu::QueryGetDataCalled(ctx.r3.u32 != 1, type == 9);
  if (ctx.r3.u32 != 1 || type != 9) {
    return;
  }
  // Real sample counts from the host occlusion queries (gpu/queries.cpp);
  // "not ready" (S_FALSE) until the GPU has produced them. With
  // fable2_query_force_visible every test reports fully visible (counts a
  // frame or two late made objects pop in and out).
  // The sun glare's visibility (sub_82279410, sub_821D8658: samples over the
  // sun disc's expected area) needs real counts: answered "fully visible", the
  // glare covered indoor scenes such as Lucian's castle hall.
  const bool sun = REXCVAR_GET(fable2_query_sun_counts) && (caller == 0x82279A74u || caller == 0x821D8DA4u);
  if (sun)
    eot::gpu::QueryMarkSun(query);
  uint64_t samples = 0;
  if (REXCVAR_GET(fable2_query_force_visible) && !sun &&
      (REXCVAR_GET(fable2_query_force_exposure) || !eot::gpu::QueryIsExposure(query))) {
    samples = 1280 * 720;
  } else if (!eot::gpu::QueryResult(query, &samples)) {
    return;
  }
  const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(samples, 0xFFFFFFFFu));
  rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(data), count);
  if (size >= 16) {
    // The larger result layout carries per-quadrant counts; report the total
    // in the first and zero in the others.
    for (uint32_t i = 1; i < 4; ++i) {
      rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(data + i * 4), 0);
    }
  }
  ctx.r3.u64 = 0;  // S_OK
}

// Diagnostics: log ring allocations that wait for space for over 50 ms.
REX_EXTERN(__imp__sub_8222ABE0);
extern "C" REX_FUNC(sub_8222ABE0) {
  const uint32_t allocator = ctx.r3.u32, size = ctx.r4.u32;
  const uint64_t lr = ctx.lr;
  const auto start = std::chrono::steady_clock::now();
  __imp__sub_8222ABE0(ctx, base);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
  if (ms > 50) {
    auto* memory = REX_KERNEL_MEMORY();
    auto ld = [&](uint32_t off) {
      return rex::memory::load_and_swap<uint32_t>(
          memory->TranslateVirtual<uint8_t*>(allocator + off));
    };
    REXLOG_INFO("Fable 2 ring wait {} ms: allocator {:08X} size {:X} lr {:08X} | base {:X} cap {:X} "
                "+12 {:X} +16 {:X} +20 {:X} +24 {:X} +28 {:X} write {:X} consumed {:X} +52 {:X} "
                "+64 {:X}",
                ms, allocator, size, uint32_t(lr), ld(4), ld(8), ld(12), ld(16), ld(20), ld(24),
                ld(28), ld(32), ld(36), ld(52), ld(64));
  }
}

// The 3D Engine thread's frame (sub_82388380) holds the render object's lock
// for the whole frame, Swap included. On the console Swap returns at once; the
// native renderer's Present blocks for vsync and the render thread, and the
// thread takes the lock again right after releasing it, before a waiting thread
// wakes (host locks are not fair). The game thread, which needs the lock for a
// handshake on every video frame, starved: the intro videos barely advanced.
// A short pause after each frame lets waiters in.
//
// The frame rate cap (eot_fps_limit) is applied here too: the thread waits
// after each frame until the next slot of the cap. Capping at Present instead
// let the game produce frames faster than they were shown, filling the queue
// to the render thread (~1.5 s input lag, multi-second freezes).
REX_EXTERN(__imp__sub_82388380);
void Fable2FrameBegin();
void Fable2FrameEnd();
namespace {
void Fable2FramePause() {
  using clock = std::chrono::steady_clock;
  constexpr auto kLockPause = std::chrono::microseconds(500);
  static clock::time_point deadline{};
  static int32_t cadence_fps = 0;
  const int32_t fps = eot::gpu::Settings::FpsLimit();
  const auto now = clock::now();
  if (fps <= 0) {
    deadline = clock::time_point{};
    cadence_fps = 0;
    rex::thread::Sleep(kLockPause);
    return;
  }
  const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / fps));
  if (cadence_fps != fps || deadline == clock::time_point{}) {
    cadence_fps = fps;
    deadline = now;
  }
  deadline += period;
  if (deadline < now + kLockPause)
    deadline = now + kLockPause;  // a late frame: no catch-up burst
  std::this_thread::sleep_until(deadline);  // sleeps (no spinning): the point is to save power
}
}  // namespace
extern "C" REX_FUNC(sub_82388380) {
  Fable2FrameBegin();
  __imp__sub_82388380(ctx, base);
  Fable2FrameEnd();
  Fable2FramePause();
}

// Query::Issue(query, flags): bit 1 begin, bit 0 end; occlusion queries (type 9)
// are recorded on the host GPU too.
REX_EXTERN(__imp__sub_822194B8);
extern "C" REX_FUNC(sub_822194B8) {
  const uint32_t query = ctx.r3.u32, flags = ctx.r4.u32;
  __imp__sub_822194B8(ctx, base);
  if (!query || !REXCVAR_GET(fable2_query_visible)) {
    return;
  }
  const uint32_t type = rex::memory::load_and_swap<uint32_t>(
      REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(query + 4));
  if (type == 9) {
    eot::gpu::QueryIssued(query, (flags & 2) != 0, (flags & 1) != 0);
  }
}

// InsertCallback(device, command cursor, flags, function, argument).
REX_EXTERN(__imp__sub_821F6050);
extern "C" REX_FUNC(sub_821F6050) {
  const uint32_t function = ctx.r6.u32, argument = ctx.r7.u32;
  __imp__sub_821F6050(ctx, base);
  if (!REXCVAR_GET(fable2_deliver_gpu_callbacks) || !function) {
    return;
  }
  const uint64_t seq = eot::gpu::RenderThreadActive() ? eot::gpu::RenderThreadLastCommitted() : 0;
  std::lock_guard lock(g_callback_mutex);
  if (g_pending_callbacks.size() < 4096) {
    g_pending_callbacks.push_back({function, argument, seq});
    g_callbacks_queued.fetch_add(1, std::memory_order_relaxed);
  }
}
