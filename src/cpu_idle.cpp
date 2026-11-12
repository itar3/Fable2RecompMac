// Fable 2: threads that wait by spinning, made to sleep (less CPU and heat,
// same timing).
//
// The game thread's fixed-step loop (sub_82276D10) reads
// QueryPerformanceCounter and, while no new logic tick is due, calls
// SwitchToThread (sub_82CC38E8) and reads the clock again. On the console it
// had a hardware thread to itself; here it kept a host core busy (about half
// of it went to the yield and the clock, profiled at a 60 fps cap). At that
// call the registers hold the loop's tick position f6 (fractional), its tick
// rate f25 (ticks per second) and the last tick it handled r28: the next tick
// is due in (r28 + 1 - f6) / f25 seconds (the logic runs at 30 ticks/s). The
// thread sleeps until shortly before then; the loop's own clock check sees the
// tick as before. Card 2 at the 60 fps cap: CPU 358-364% -> 274-276%, same
// frame times.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "generated/Fable2_init.h"

REXCVAR_DEFINE_BOOL(fable2_sleep_waits, true, "Fable2",
                    "Threads that wait by spinning sleep instead (the game thread's wait for its next "
                    "logic tick); false: spin as on the console");

namespace {
constexpr uint32_t kGameLoopYieldReturn = 0x82277348;  // after "bl SwitchToThread" in sub_82276D10
constexpr double kWakeMargin = 0.0003;                 // wake early; the last stretch yields as before
constexpr double kLongestSleep = 0.05;
}  // namespace

REX_EXTERN(__imp__sub_82CC38E8);
extern "C" REX_FUNC(sub_82CC38E8) {
  if (static_cast<uint32_t>(ctx.lr) == kGameLoopYieldReturn && REXCVAR_GET(fable2_sleep_waits)) {
    const double rate = ctx.f25.f64;
    const double remaining = rate > 0.0 ? (static_cast<double>(ctx.r28.s32) + 1.0 - ctx.f6.f64) / rate : 0.0;
    if (remaining > kWakeMargin && remaining < 0.1) {
      std::this_thread::sleep_for(std::chrono::duration<double>(std::min(remaining - kWakeMargin, kLongestSleep)));
      ctx.r3.u64 = 1;  // TRUE: "switched" (the loop ignores it)
      return;
    }
  }
  __imp__sub_82CC38E8(ctx, base);
}
