// Fable 2: non-volatile vector registers shared across a call.
//
// With non_volatile_as_local every recompiled function keeps the non-volatile
// registers in local variables. Two functions preload v64 and up and call
// helpers that read them and leave results in them (the game's renderer:
// sub_822E16A8 -> sub_822340C8; and sub_82DED5E0 -> sub_82DF48D8 /
// sub_82DF4BB8 / sub_82DF5010); the code generator only hands general-purpose
// registers to such callees. The [[midasm_hook]] entries in Fable2_config.toml
// call these functions with the registers in a fixed order: "Out" copies them
// to a per-thread stash (the caller before the call, the helper before it
// returns), "In" copies them back (the helper at its entry, the caller after
// the call). Without them the helpers read zeros: the world drew black.

#include "generated/Fable2_init.h"

namespace {
thread_local PPCVRegister g_stash[24];

template <typename... R> inline void Out(const R &...r) {
  PPCVRegister *p = g_stash;
  ((*p++ = r), ...);
}
template <typename... R> inline void In(R &...r) {
  const PPCVRegister *p = g_stash;
  ((r = *p++), ...);
}
}  // namespace

using V = PPCVRegister &;

void Fable2VecOut9(V a, V b, V c, V d, V e, V f, V g, V h, V i) { Out(a, b, c, d, e, f, g, h, i); }
void Fable2VecIn9(V a, V b, V c, V d, V e, V f, V g, V h, V i) { In(a, b, c, d, e, f, g, h, i); }

void Fable2VecOut16(V a, V b, V c, V d, V e, V f, V g, V h, V i, V j, V k, V l, V m, V n, V o, V p) {
  Out(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p);
}
void Fable2VecIn16(V a, V b, V c, V d, V e, V f, V g, V h, V i, V j, V k, V l, V m, V n, V o, V p) {
  In(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p);
}

void Fable2VecOut19(V a, V b, V c, V d, V e, V f, V g, V h, V i, V j, V k, V l, V m, V n, V o, V p, V q, V r, V s) {
  Out(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s);
}
void Fable2VecIn19(V a, V b, V c, V d, V e, V f, V g, V h, V i, V j, V k, V l, V m, V n, V o, V p, V q, V r, V s) {
  In(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s);
}
