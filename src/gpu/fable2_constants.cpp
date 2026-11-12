// gpu/fable2_constants.cpp
// Fable 2: float constants loaded from the game's own memory.
//
// Two D3D routines replay precompiled constant blocks: sub_82221B90 (state
// block with a record index) emits LOAD_ALU_CONSTANT packets that point the GPU
// at the data, sub_8222BFA0 copies the data into SET_CONSTANT packets. Neither
// writes the device's constant shadow, which the native renderer reads, so
// those constants kept stale values; the final tonemap's
// SaturationBrightnessBaseAndOffset (c77) stayed (0, 0) and only bloom showed.
//
// Block layout (from the code): +20 offset of the record list (per index, 8
// bytes apart in sub_82221B90), list at base + offset + 20, list size at
// base + offset + 16; records { u16 first float4 register, u16 dword count,
// u32 data offset } until a count of 0: both routines address register
// 0x4000 + first * 4, so the first dword is first * 4 (reading it as a dword
// put the snow shader's literal constants c244..c255 over c61..c72, its
// g_CamPos). The ALU constant file is 512 float4s, vertex
// shader c0 at dword 0 and pixel shader c0 at dword 1024; the device shadow
// holds the same layout at +0x780.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/draw.h"

REXCVAR_DEFINE_BOOL(fable2_block_fetch_constants, true, "Fable2",
                    "Native renderer: copy fetch constants (vertex streams, textures) from constant blocks");
REXCVAR_DEFINE_BOOL(fable2_constant_blocks, true, "Fable2",
                    "Native renderer: copy constant blocks loaded from game memory into the "
                    "device constant shadow");

namespace {

using eot::gpu::g_extra_ps_constants_dirty;
using eot::gpu::g_extra_vs_constants_dirty;

constexpr uint32_t kConstantShadow = 0x780;
constexpr uint32_t kConstantDwords = 2048;
constexpr uint32_t kFetchShadow = 0x480;
constexpr uint32_t kFetchDwords = 192;

uint32_t Ld32(uint32_t va) { return eot::mem::load<uint32_t>(va); }
uint16_t Ld16(uint32_t va) { return eot::mem::load<uint16_t>(va); }

void CopyBlock(uint32_t device, uint32_t base, uint32_t list_offset_va, uint32_t data_base) {
  if (!device || !base || !REXCVAR_GET(fable2_constant_blocks))
    return;
  const uint32_t offset = Ld32(list_offset_va);
  if (!offset)
    return;
  const uint32_t header = base + offset;
  uint32_t p = header + 20;
  const uint32_t end = p + Ld32(header + 16);
  uint64_t vs_dirty = 0, ps_dirty = 0;
  while (p < end) {
    const uint32_t first = Ld16(p) * 4u, count = Ld16(p + 2);
    p += 4;
    if (!count)
      break;
    const uint32_t data = Ld32(p) + data_base;
    p += 4;
    if (first >= kConstantDwords) {
      // Fetch constants (textures, vertex streams) follow the ALU constants:
      // dwords 2048..2239, held by the device at +0x480 (24 bytes per texture
      // slot, the vertex streams from 95 down).
      static std::atomic<uint32_t> fetch_logs{0};
      const uint32_t f = first - kConstantDwords;
      if (REXCVAR_GET(fable2_block_fetch_constants) && f < kFetchDwords) {
        const uint32_t n = std::min(count, kFetchDwords - f);
        uint8_t *dst = eot::mem::at<uint8_t>(device + kFetchShadow + f * 4);
        const uint8_t *src = eot::mem::at<uint8_t>(data);
        if (dst && src)
          std::memmove(dst, src, n * 4);
        if (fetch_logs.fetch_add(1) < 8)
          REXLOG_INFO("Fable 2 constant block: fetch constants dwords {}..{} copied", f, f + n - 1);
      }
      continue;
    }
    const uint32_t n = std::min(count, kConstantDwords - first);
    uint8_t *dst = eot::mem::at<uint8_t>(device + kConstantShadow + first * 4);
    const uint8_t *src = eot::mem::at<uint8_t>(data);
    if (!dst || !src)
      continue;
    std::memmove(dst, src, n * 4);  // both big-endian guest memory
    for (uint32_t reg = first / 4; reg <= (first + n - 1) / 4; ++reg) {
      if (reg < 256)
        vs_dirty |= 1ull << (reg / 4);
      else
        ps_dirty |= 1ull << ((reg - 256) / 4);
    }
  }
  if (vs_dirty)
    g_extra_vs_constants_dirty.fetch_or(vs_dirty, std::memory_order_relaxed);
  if (ps_dirty)
    g_extra_ps_constants_dirty.fetch_or(ps_dirty, std::memory_order_relaxed);
  static bool logged = false;
  if (!logged && (vs_dirty | ps_dirty)) {
    logged = true;
    REXLOG_INFO("Fable 2 constant blocks copied into the device shadow (first: vs {:016x} ps {:016x})",
                vs_dirty, ps_dirty);
  }
}

}

// sub_8227D150(device, stage, start, vec4 count, buffer, vec4 offset): loads
// constants from a constant buffer object (data pointer at +24) with a
// LOAD_ALU_CONSTANT packet; stage 1 is the pixel shader (registers 256 on).
// Fable 2's global light parameters (sun direction and colour, PS c80..c87)
// arrive this way; without them the scene lost its sunlight.
//
// The GPU reads the buffer when it executes the packet, and the game often
// fills it after the call: copied at call time, the screen shadow pass got the
// previous user's g_ScreenSize (1/512, 1/256), clamped its depth lookups and
// smeared shadows into screen-high columns. The copy waits for the next draw.
REXCVAR_DEFINE_BOOL(fable2_deferred_constant_buffers, true, "Fable2",
                    "Native renderer: copy constant buffer loads at the next draw instead of at the "
                    "call (the game fills the buffer after the call)");

namespace {

struct DeferredLoad {
  uint32_t device, first_reg, count, buffer, offset;
};
std::mutex g_deferred_mutex;
std::vector<DeferredLoad> g_deferred;

void CopyConstantBuffer(uint32_t device, uint32_t first_reg, uint32_t count, uint32_t buffer,
                        uint32_t offset) {
  const uint32_t data = Ld32(buffer + 24) + offset * 16;
  if (!data || first_reg >= 512)
    return;
  const uint32_t n = std::min(count, 512u - first_reg);
  uint8_t *dst = eot::mem::at<uint8_t>(device + kConstantShadow + first_reg * 16);
  const uint8_t *src = eot::mem::at<uint8_t>(data);
  if (!dst || !src)
    return;
  std::memmove(dst, src, n * 16);
  uint64_t vs_dirty = 0, ps_dirty = 0;
  for (uint32_t reg = first_reg; reg < first_reg + n; ++reg) {
    if (reg < 256)
      vs_dirty |= 1ull << (reg / 4);
    else
      ps_dirty |= 1ull << ((reg - 256) / 4);
  }
  g_extra_vs_constants_dirty.fetch_or(vs_dirty, std::memory_order_relaxed);
  g_extra_ps_constants_dirty.fetch_or(ps_dirty, std::memory_order_relaxed);
}

}

namespace eot::gpu {

void ApplyDeferredConstantLoads(uint32_t device) {
  std::vector<DeferredLoad> loads;
  {
    std::lock_guard lock(g_deferred_mutex);
    if (g_deferred.empty())
      return;
    loads.swap(g_deferred);
  }
  for (const DeferredLoad &l : loads)
    if (l.device == device)
      CopyConstantBuffer(l.device, l.first_reg, l.count, l.buffer, l.offset);
}

}

REX_EXTERN(__imp__sub_8227D150);
extern "C" REX_FUNC(sub_8227D150) {
  const uint32_t device = ctx.r3.u32, stage = ctx.r4.u32 & 0xFF, start = ctx.r5.u32;
  const uint32_t count = ctx.r6.u32, buffer = ctx.r7.u32, offset = ctx.r8.u32;
  if (device && buffer && count && REXCVAR_GET(fable2_constant_blocks)) {
    const uint32_t first_reg = (stage << 8) + start;
    if (REXCVAR_GET(fable2_deferred_constant_buffers)) {
      std::lock_guard lock(g_deferred_mutex);
      g_deferred.push_back({device, first_reg, count, buffer, offset});
    } else {
      CopyConstantBuffer(device, first_reg, count, buffer, offset);
    }
  }
  __imp__sub_8227D150(ctx, base);
}

// sub_82221B90(device, block, data, ?, index): state block record `index`.
REX_EXTERN(__imp__sub_82221B90);
extern "C" REX_FUNC(sub_82221B90) {
  CopyBlock(ctx.r3.u32, ctx.r4.u32, ctx.r4.u32 + ctx.r7.u32 * 8 + 20, ctx.r5.u32);
  __imp__sub_82221B90(ctx, base);
}

// sub_8222BFA0(device, block, data).
REX_EXTERN(__imp__sub_8222BFA0);
extern "C" REX_FUNC(sub_8222BFA0) {
  CopyBlock(ctx.r3.u32, ctx.r4.u32, ctx.r4.u32 + 20, ctx.r5.u32);
  __imp__sub_8222BFA0(ctx, base);
}

// Fable 2: D3D functions that write float constants straight into the command
// ring (SET_CONSTANT / LOAD_ALU_CONSTANT packets) without the device's constant
// shadow, which the native renderer reads. Each wrapped function decodes the
// packets it wrote (between the ring write position before and after it ran)
// and copies ALU constants into the shadow when its bit is set in
// fable2_mirror_constants; every function logs the registers it writes.
REXCVAR_DEFINE_INT32(fable2_mirror_constants, 0, "Fable2",
                     "Native renderer: bitmask of ring constant writers mirrored into the device "
                     "shadow (bit 0 821F9918, 1 822192E8, 2 823A4740, 3 8273CFB8, 4 82BA77D0)");

namespace {

void NoteRingConstants(uint32_t bit, const char *name, uint32_t first, uint32_t count) {
  static std::mutex mutex;
  static std::set<std::pair<uint32_t, uint32_t>> seen;
  std::lock_guard lock(mutex);
  if (seen.size() < 200 && seen.insert({bit << 16 | first, count}).second)
    REXLOG_INFO("[ringconst] {} writes ALU dwords {}..{} ({} c{}..c{})", name, first, first + count - 1,
                first < kConstantDwords / 2 ? "vs" : "ps", (first % 1024) / 4,
                ((first + count - 1) % 1024) / 4);
}

void ApplyAluConstants(uint32_t device, uint32_t first, uint32_t count, const uint8_t *src) {
  if (first >= kConstantDwords || !src)
    return;
  count = std::min(count, kConstantDwords - first);
  uint8_t *dst = eot::mem::at<uint8_t>(device + kConstantShadow + first * 4);
  if (!dst)
    return;
  std::memmove(dst, src, count * 4);
  uint64_t vs_dirty = 0, ps_dirty = 0;
  for (uint32_t reg = first / 4; reg <= (first + count - 1) / 4; ++reg) {
    if (reg < 256)
      vs_dirty |= 1ull << (reg / 4);
    else
      ps_dirty |= 1ull << ((reg - 256) / 4);
  }
  g_extra_vs_constants_dirty.fetch_or(vs_dirty, std::memory_order_relaxed);
  g_extra_ps_constants_dirty.fetch_or(ps_dirty, std::memory_order_relaxed);
}

void MirrorRingConstants(uint32_t bit, const char *name, uint32_t device, uint32_t before) {
  if (!device)
    return;
  const uint32_t after = Ld32(device + 48);
  if (after <= before || after - before > 0x10000)
    return;  // nothing written, or the ring wrapped to a new segment
  const bool apply = (REXCVAR_GET(fable2_mirror_constants) >> bit) & 1;
  for (uint32_t p = before + 4; p <= after;) {
    const uint32_t h = Ld32(p);
    const uint32_t type = h >> 30, n = ((h >> 16) & 0x3FFF) + 1;
    if (type == 3) {
      const uint32_t op = (h >> 8) & 0x7F;
      if (op == 0x2D && n >= 2) {  // SET_CONSTANT
        const uint32_t ot = Ld32(p + 4);
        if (((ot >> 16) & 0xFF) == 0) {
          NoteRingConstants(bit, name, ot & 0x7FF, n - 1);
          if (apply)
            ApplyAluConstants(device, ot & 0x7FF, n - 1, eot::mem::at<uint8_t>(p + 8));
        }
      } else if (op == 0x2F && n >= 3) {  // LOAD_ALU_CONSTANT
        const uint32_t addr = Ld32(p + 4) & ~3u, ot = Ld32(p + 8), size = Ld32(p + 12) & 0xFFF;
        if (((ot >> 16) & 0xFF) == 0) {
          NoteRingConstants(bit, name, ot & 0x7FF, size);
          if (apply)
            ApplyAluConstants(device, ot & 0x7FF, size, eot::mem::phys<uint8_t>(addr));
        }
      }
      p += 4 * (n + 1);
    } else if (type == 0) {
      p += 4 * (n + 1);
    } else {
      p += 4;
    }
  }
}

}  // namespace

#define FABLE2_RING_CONSTANT_WRITER(addr, bit)                         \
  REX_EXTERN(__imp__sub_##addr);                                       \
  extern "C" REX_FUNC(sub_##addr) {                                    \
    const uint32_t device =                                            \
        eot::mem::readable(ctx.r3.u32 + 48, 4) ? ctx.r3.u32 : 0u;      \
    const uint32_t before = device ? Ld32(device + 48) : 0;            \
    __imp__sub_##addr(ctx, base);                                      \
    MirrorRingConstants(bit, "sub_" #addr, device, before);            \
  }

FABLE2_RING_CONSTANT_WRITER(821F9918, 0)
FABLE2_RING_CONSTANT_WRITER(822192E8, 1)
FABLE2_RING_CONSTANT_WRITER(823A4740, 2)
FABLE2_RING_CONSTANT_WRITER(8273CFB8, 3)
FABLE2_RING_CONSTANT_WRITER(82BA77D0, 4)
