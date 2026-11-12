// Temporary diagnostics: the 3D Engine thread sometimes never finishes a
// gameplay frame (sub_82388380). Counts calls into the functions on the stuck
// path and, while a frame runs over 2 s, logs the per-second counts and the
// last arguments once a second.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "core/hooks.h"
#include <rex/logging.h>
#include <fmt/format.h>
#include <string>

namespace {
constexpr int kCount = 20;
const char *kNames[kCount] = {"821C3858", "821C3C60", "821C3670", "821BDC80", "8223CFA0", "82236EA8", "82236F90", "82B28580", "82219A58", "8221BD40", "82222418", "8221CCA8", "8221ED18", "8221B8A0", "8221F040", "8221BB98", "822322C8", "82237020", "8229BA90", "821B7060"};
std::atomic<uint64_t> g_calls[kCount];
std::atomic<uint32_t> g_last_r3[kCount], g_last_r4[kCount], g_last_r5[kCount], g_last_lr[kCount];
std::atomic<int64_t> g_frame_start_ns{0};
std::atomic<bool> g_started{false};

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Watchdog() {
  uint64_t prev[kCount] = {};
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    uint64_t now[kCount];
    for (int i = 0; i < kCount; ++i) now[i] = g_calls[i].load(std::memory_order_relaxed);
    const int64_t start = g_frame_start_ns.load(std::memory_order_relaxed);
    if (start && NowNs() - start > 2'000'000'000LL) {
      std::string line;
      for (int i = 0; i < kCount; ++i)
        if (now[i] != prev[i])
          line += fmt::format(" {}:{}(r3={:08X} r4={:08X} r5={:08X} lr={:08X})", kNames[i], now[i] - prev[i],
                              g_last_r3[i].load(), g_last_r4[i].load(), g_last_r5[i].load(), g_last_lr[i].load());
      REXLOG_WARN("Fable 2 stuck frame ({} s):{}", (NowNs() - start) / 1'000'000'000LL, line);
    }
    for (int i = 0; i < kCount; ++i) prev[i] = now[i];
  }
}
}  // namespace

void Fable2FrameBegin() {
  if (!g_started.exchange(true)) std::thread(Watchdog).detach();
  g_frame_start_ns.store(NowNs(), std::memory_order_relaxed);
}
void Fable2FrameEnd() { g_frame_start_ns.store(0, std::memory_order_relaxed); }

REX_EXTERN(__imp__sub_821C3858);
extern "C" REX_FUNC(sub_821C3858) {
  g_calls[0].fetch_add(1, std::memory_order_relaxed);
  g_last_r3[0].store(ctx.r3.u32, std::memory_order_relaxed);
  g_last_r4[0].store(ctx.r4.u32, std::memory_order_relaxed);
  g_last_r5[0].store(ctx.r5.u32, std::memory_order_relaxed);
  g_last_lr[0].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  __imp__sub_821C3858(ctx, base);
}
REX_EXTERN(__imp__sub_821C3C60);
extern "C" REX_FUNC(sub_821C3C60) {
  g_calls[1].fetch_add(1, std::memory_order_relaxed);
  g_last_r3[1].store(ctx.r3.u32, std::memory_order_relaxed);
  g_last_r4[1].store(ctx.r4.u32, std::memory_order_relaxed);
  g_last_r5[1].store(ctx.r5.u32, std::memory_order_relaxed);
  g_last_lr[1].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  __imp__sub_821C3C60(ctx, base);
}
REX_EXTERN(__imp__sub_821C3670);
extern "C" REX_FUNC(sub_821C3670) {
  g_calls[2].fetch_add(1, std::memory_order_relaxed);
  g_last_r3[2].store(ctx.r3.u32, std::memory_order_relaxed);
  g_last_r4[2].store(ctx.r4.u32, std::memory_order_relaxed);
  g_last_r5[2].store(ctx.r5.u32, std::memory_order_relaxed);
  g_last_lr[2].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  __imp__sub_821C3670(ctx, base);
}
REX_EXTERN(__imp__sub_821BDC80);
extern "C" REX_FUNC(sub_821BDC80) {
  g_calls[3].fetch_add(1, std::memory_order_relaxed);
  g_last_r3[3].store(ctx.r3.u32, std::memory_order_relaxed);
  g_last_r4[3].store(ctx.r4.u32, std::memory_order_relaxed);
  g_last_r5[3].store(ctx.r5.u32, std::memory_order_relaxed);
  g_last_lr[3].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  __imp__sub_821BDC80(ctx, base);
}
REX_EXTERN(__imp__sub_8223CFA0);
extern "C" REX_FUNC(sub_8223CFA0) {
  g_calls[4].fetch_add(1, std::memory_order_relaxed);
  g_last_r3[4].store(ctx.r3.u32, std::memory_order_relaxed);
  g_last_r4[4].store(ctx.r4.u32, std::memory_order_relaxed);
  g_last_r5[4].store(ctx.r5.u32, std::memory_order_relaxed);
  g_last_lr[4].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  __imp__sub_8223CFA0(ctx, base);
}
REX_EXTERN(__imp__sub_82236EA8);
extern "C" REX_FUNC(sub_82236EA8) {
  g_calls[5].fetch_add(1, std::memory_order_relaxed);
  g_last_r3[5].store(ctx.r3.u32, std::memory_order_relaxed);
  g_last_r4[5].store(ctx.r4.u32, std::memory_order_relaxed);
  g_last_r5[5].store(ctx.r5.u32, std::memory_order_relaxed);
  g_last_lr[5].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  __imp__sub_82236EA8(ctx, base);
}
REX_EXTERN(__imp__sub_82236F90);
extern "C" REX_FUNC(sub_82236F90) {
  g_calls[6].fetch_add(1, std::memory_order_relaxed);
  g_last_r3[6].store(ctx.r3.u32, std::memory_order_relaxed);
  g_last_r4[6].store(ctx.r4.u32, std::memory_order_relaxed);
  g_last_r5[6].store(ctx.r5.u32, std::memory_order_relaxed);
  g_last_lr[6].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  __imp__sub_82236F90(ctx, base);
}
REX_EXTERN(__imp__sub_82B28580);
extern "C" REX_FUNC(sub_82B28580) {
  g_calls[7].fetch_add(1, std::memory_order_relaxed);
  g_last_r3[7].store(ctx.r3.u32, std::memory_order_relaxed);
  g_last_r4[7].store(ctx.r4.u32, std::memory_order_relaxed);
  g_last_r5[7].store(ctx.r5.u32, std::memory_order_relaxed);
  g_last_lr[7].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  __imp__sub_82B28580(ctx, base);
}
REX_EXTERN(__imp__sub_82219A58);
extern "C" REX_FUNC(sub_82219A58) {
  g_calls[8].fetch_add(1, std::memory_order_relaxed);
  g_last_r3[8].store(ctx.r3.u32, std::memory_order_relaxed);
  g_last_r4[8].store(ctx.r4.u32, std::memory_order_relaxed);
  g_last_r5[8].store(ctx.r5.u32, std::memory_order_relaxed);
  g_last_lr[8].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  __imp__sub_82219A58(ctx, base);
}
REX_EXTERN(__imp__sub_8221BD40);
extern "C" REX_FUNC(sub_8221BD40) {
  g_calls[9].fetch_add(1, std::memory_order_relaxed);
  g_last_r3[9].store(ctx.r3.u32, std::memory_order_relaxed);
  g_last_r4[9].store(ctx.r4.u32, std::memory_order_relaxed);
  g_last_r5[9].store(ctx.r5.u32, std::memory_order_relaxed);
  g_last_lr[9].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  __imp__sub_8221BD40(ctx, base);
}
REX_EXTERN(__imp__sub_82222418);
extern "C" REX_FUNC(sub_82222418) {
  g_calls[10].fetch_add(1, std::memory_order_relaxed);
  g_last_r3[10].store(ctx.r3.u32, std::memory_order_relaxed);
  g_last_r4[10].store(ctx.r4.u32, std::memory_order_relaxed);
  g_last_r5[10].store(ctx.r5.u32, std::memory_order_relaxed);
  g_last_lr[10].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  __imp__sub_82222418(ctx, base);
}
REX_EXTERN(__imp__sub_8221CCA8);
extern "C" REX_FUNC(sub_8221CCA8) {
  g_calls[11].fetch_add(1, std::memory_order_relaxed);
  g_last_r3[11].store(ctx.r3.u32, std::memory_order_relaxed);
  g_last_r4[11].store(ctx.r4.u32, std::memory_order_relaxed);
  g_last_r5[11].store(ctx.r5.u32, std::memory_order_relaxed);
  g_last_lr[11].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  __imp__sub_8221CCA8(ctx, base);
}
REX_EXTERN(__imp__sub_8221ED18);
extern "C" REX_FUNC(sub_8221ED18) {
  g_calls[12].fetch_add(1, std::memory_order_relaxed);
  g_last_r3[12].store(ctx.r3.u32, std::memory_order_relaxed);
  g_last_r4[12].store(ctx.r4.u32, std::memory_order_relaxed);
  g_last_r5[12].store(ctx.r5.u32, std::memory_order_relaxed);
  g_last_lr[12].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  __imp__sub_8221ED18(ctx, base);
}
REX_EXTERN(__imp__sub_8221B8A0);
extern "C" REX_FUNC(sub_8221B8A0) {
  g_calls[13].fetch_add(1, std::memory_order_relaxed);
  g_last_r3[13].store(ctx.r3.u32, std::memory_order_relaxed);
  g_last_r4[13].store(ctx.r4.u32, std::memory_order_relaxed);
  g_last_r5[13].store(ctx.r5.u32, std::memory_order_relaxed);
  g_last_lr[13].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  __imp__sub_8221B8A0(ctx, base);
}
REX_EXTERN(__imp__sub_8221F040);
extern "C" REX_FUNC(sub_8221F040) {
  g_calls[14].fetch_add(1, std::memory_order_relaxed);
  g_last_r3[14].store(ctx.r3.u32, std::memory_order_relaxed);
  g_last_r4[14].store(ctx.r4.u32, std::memory_order_relaxed);
  g_last_r5[14].store(ctx.r5.u32, std::memory_order_relaxed);
  g_last_lr[14].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  __imp__sub_8221F040(ctx, base);
}
REX_EXTERN(__imp__sub_8221BB98);
extern "C" REX_FUNC(sub_8221BB98) {
  g_calls[15].fetch_add(1, std::memory_order_relaxed);
  g_last_r3[15].store(ctx.r3.u32, std::memory_order_relaxed);
  g_last_r4[15].store(ctx.r4.u32, std::memory_order_relaxed);
  g_last_r5[15].store(ctx.r5.u32, std::memory_order_relaxed);
  g_last_lr[15].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  __imp__sub_8221BB98(ctx, base);
}
REX_EXTERN(__imp__sub_822322C8);
extern "C" REX_FUNC(sub_822322C8) {
  g_calls[16].fetch_add(1, std::memory_order_relaxed);
  g_last_r3[16].store(ctx.r3.u32, std::memory_order_relaxed);
  g_last_r4[16].store(ctx.r4.u32, std::memory_order_relaxed);
  g_last_r5[16].store(ctx.r5.u32, std::memory_order_relaxed);
  g_last_lr[16].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  __imp__sub_822322C8(ctx, base);
}
REX_EXTERN(__imp__sub_82237020);
extern "C" REX_FUNC(sub_82237020) {
  g_calls[17].fetch_add(1, std::memory_order_relaxed);
  g_last_r3[17].store(ctx.r3.u32, std::memory_order_relaxed);
  g_last_r4[17].store(ctx.r4.u32, std::memory_order_relaxed);
  g_last_r5[17].store(ctx.r5.u32, std::memory_order_relaxed);
  g_last_lr[17].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  __imp__sub_82237020(ctx, base);
}
REX_EXTERN(__imp__sub_8229BA90);
extern "C" REX_FUNC(sub_8229BA90) {
  g_calls[18].fetch_add(1, std::memory_order_relaxed);
  g_last_r3[18].store(ctx.r3.u32, std::memory_order_relaxed);
  g_last_r4[18].store(ctx.r4.u32, std::memory_order_relaxed);
  g_last_r5[18].store(ctx.r5.u32, std::memory_order_relaxed);
  g_last_lr[18].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  __imp__sub_8229BA90(ctx, base);
}
REX_EXTERN(__imp__sub_821B7060);
extern "C" REX_FUNC(sub_821B7060) {
  g_calls[19].fetch_add(1, std::memory_order_relaxed);
  g_last_r3[19].store(ctx.r3.u32, std::memory_order_relaxed);
  g_last_r4[19].store(ctx.r4.u32, std::memory_order_relaxed);
  g_last_r5[19].store(ctx.r5.u32, std::memory_order_relaxed);
  g_last_lr[19].store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
  __imp__sub_821B7060(ctx, base);
}
