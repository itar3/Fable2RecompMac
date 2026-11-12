// Fable 2: CPU emulation of the GPU skinning pass (memory export).
//
// Vertex shader 5243696187782727 skins a mesh (4 bones, 3 float4 rows each)
// and writes the result with memexport instead of rasterising: per vertex i,
// position (3 dwords) at element 5i through g_ExportAddressPosition (c8),
// texcoord at 5i+3 (c47) and the packed normal * 511 at 5i+4 (c46). Later
// draws (the street's fur shells) read that buffer as an ordinary vertex
// stream. The native renderer has no memexport, so the stream stayed zero.
//
// The pass is emulated on the guest thread when its draw is issued, so the
// data is in guest memory before the draws that read it are captured. The
// vfetch formats, strides, offsets and fetch slots come from the live
// microcode: D3D patches them into the vertex shader when it binds it to a
// declaration (the captured shader dumps are unbound and say format 0).

#include "gpu/draw.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <utility>

#include <rex/cvar.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/resources.h"
#include "gpu/shaders/guest_shaders.h"

REXCVAR_DEFINE_BOOL(fable2_memexport_unnormalized_float, true, "Fable2",
                    "Native renderer: memexport fixed-point fields of float-format streams are not scaled "
                    "(the skinned normals)");
REXCVAR_DEFINE_BOOL(fable2_emulate_skinning, true, "Fable2",
                    "Native renderer: run the GPU skinning memexport pass on the CPU (the dog's fur "
                    "shells; without it they read stale memory and draw as screen-wide shards)");

namespace eot::gpu {

namespace {

constexpr u64 kSkinningShader = 0x5243696187782727ull;

struct Vec4 {
  f32 v[4] = {0, 0, 0, 0};
  f32 &operator[](u32 i) { return v[i]; }
  f32 operator[](u32 i) const { return v[i]; }
};

struct Fetch {
  u32 slot = 0, src = 0, srcComp = 0, dst = 0, dstSwizzle = 0;
  u32 format = 0, stride = 0, flags = 0;
  i32 offset = 0, expAdjust = 0;
  bool mini = false, rounded = false, bgra = false;
};

// Microcode instruction indices of the shader's vfetches (identical in the
// bound and unbound microcode; checked against the expected registers).
constexpr u32 kFirstFetch = 11, kFetchCount = 29;
// The declaration usage each vfetch binds to (usage, index): the vertex's
// inputs, then the bone rows POSITION1..3 (4 bones for the position, again
// for the normal).
constexpr u8 kFetchUsage[kFetchCount][2] = {
    {0, 0}, {3, 0}, {2, 0}, {1, 0}, {5, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 1}, {0, 2}, {0, 3}, {0, 1}, {0, 2}, {0, 3}, {0, 1},
    {0, 2}, {0, 3}, {0, 1}, {0, 2}, {0, 3}, {0, 1}, {0, 2}, {0, 3}, {0, 1}, {0, 2}, {0, 3}, {0, 1}, {0, 2}, {0, 3}};
constexpr u8 kExpectedDst[kFetchCount] = {9,  16, 19, 3,  2,  4,  8,  1,  27, 15, 12, 26, 14, 11, 25,
                                          13, 10, 18, 24, 21, 7,  5,  6,  17, 23, 20, 0,  22, 19};

f32 HalfToFloat(u32 h) {
  const u32 sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
  if (!exp)
    return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<f32>(man), -24);
  if (exp == 31)
    return std::bit_cast<f32>(sign | 0x7F800000u | (man << 13));
  return std::bit_cast<f32>(sign | ((exp + 112) << 23) | (man << 13));
}

u32 FloatToHalf(f32 f) {
  const u32 b = std::bit_cast<u32>(f);
  const u32 sign = (b >> 16) & 0x8000u;
  const i32 exp = static_cast<i32>((b >> 23) & 0xFF) - 112;
  u32 man = b & 0x7FFFFF;
  if (exp <= 0)
    return sign;
  if (exp >= 31)
    return sign | 0x7BFF;  // Xenos 16-bit floats have no infinity
  man += 0x1000;  // round to nearest
  if (man & 0x800000) {
    man = 0;
    if (exp + 1 >= 31)
      return sign | 0x7BFF;
    return sign | (static_cast<u32>(exp + 1) << 10);
  }
  return sign | (static_cast<u32>(exp) << 10) | (man >> 13);
}

// The fetch constant's endian mode applied to a dword read as little-endian
// raw guest bytes (as XenosRecomp's vfetchPullWord does).
u32 Swap(u32 v, u32 endian) {
  if (endian == 1 || endian == 2)
    v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
  if (endian == 2 || endian == 3)
    v = (v << 16) | (v >> 16);
  return v;
}

f32 Packed(u32 bits, u32 width, u32 flags) {
  const bool is_signed = flags & 1, integer = flags & 2;
  if (is_signed) {
    const i32 sv = static_cast<i32>(bits << (32 - width)) >> (32 - width);
    if (integer)
      return static_cast<f32>(sv);
    return std::max(static_cast<f32>(sv) / static_cast<f32>((1u << (width - 1)) - 1), -1.0f);
  }
  if (integer)
    return static_cast<f32>(bits);
  return static_cast<f32>(bits) / static_cast<f32>((1u << width) - 1);
}

struct Stream {
  const u8 *data = nullptr;
  u32 dwords = 0, endian = 0;
  u32 Word(i64 dword) const {
    if (!data || dword < 0 || dword >= dwords)
      return 0;
    u32 v;
    std::memcpy(&v, data + dword * 4, 4);
    return Swap(v, endian);
  }
};

// Returns false for formats this emulation doesn't know.
bool Decode(const Stream &st, i64 element, u32 format, u32 flags, f32 scale, Vec4 &r) {
  r = Vec4{};
  r[3] = 1.0f;
  const u32 w0 = st.Word(element);
  switch (format) {
  case 6:  // k_8_8_8_8
    for (u32 c = 0; c < 4; ++c)
      r[c] = Packed((w0 >> (8 * c)) & 0xFF, 8, flags);
    break;
  case 7:  // k_2_10_10_10
    r[0] = Packed(w0 & 0x3FF, 10, flags);
    r[1] = Packed((w0 >> 10) & 0x3FF, 10, flags);
    r[2] = Packed((w0 >> 20) & 0x3FF, 10, flags);
    r[3] = Packed(w0 >> 30, 2, flags);
    break;
  case 16:  // k_10_11_11
    r[0] = Packed(w0 & 0x7FF, 11, flags);
    r[1] = Packed((w0 >> 11) & 0x7FF, 11, flags);
    r[2] = Packed(w0 >> 22, 10, flags);
    break;
  case 17:  // k_11_11_10
    r[0] = Packed(w0 & 0x3FF, 10, flags);
    r[1] = Packed((w0 >> 10) & 0x7FF, 11, flags);
    r[2] = Packed(w0 >> 21, 11, flags);
    break;
  case 25:  // k_16_16 (16-bit pairs: x is the high half, as the host layout's swap)
    r[0] = Packed(w0 >> 16, 16, flags);
    r[1] = Packed(w0 & 0xFFFF, 16, flags);
    break;
  case 26: {  // k_16_16_16_16
    const u32 w1 = st.Word(element + 1);
    r[0] = Packed(w0 >> 16, 16, flags);
    r[1] = Packed(w0 & 0xFFFF, 16, flags);
    r[2] = Packed(w1 >> 16, 16, flags);
    r[3] = Packed(w1 & 0xFFFF, 16, flags);
    break;
  }
  case 31:  // k_16_16_FLOAT
    r[0] = HalfToFloat(w0 >> 16);
    r[1] = HalfToFloat(w0 & 0xFFFF);
    break;
  case 32: {  // k_16_16_16_16_FLOAT
    const u32 w1 = st.Word(element + 1);
    r[0] = HalfToFloat(w0 >> 16);
    r[1] = HalfToFloat(w0 & 0xFFFF);
    r[2] = HalfToFloat(w1 >> 16);
    r[3] = HalfToFloat(w1 & 0xFFFF);
    break;
  }
  case 36:  // k_32_FLOAT
  case 37:  // k_32_32_FLOAT
  case 38:  // k_32_32_32_32_FLOAT
  case 57: {  // k_32_32_32_FLOAT
    const u32 n = format == 36 ? 1 : format == 37 ? 2 : format == 38 ? 4 : 3;
    for (u32 c = 0; c < n; ++c)
      r[c] = std::bit_cast<f32>(st.Word(element + c));
    return true;  // floats ignore exp_adjust
  }
  default:
    return false;
  }
  if (scale != 1.0f)
    for (u32 c = 0; c < 4; ++c)
      r[c] *= scale;
  return true;
}

void WriteDst(Vec4 &dst, const Vec4 &val, u32 swizzle) {
  for (u32 c = 0; c < 4; ++c) {
    const u32 sel = (swizzle >> (3 * c)) & 7;
    if (sel < 4)
      dst[c] = val[sel];
    else if (sel == 4)
      dst[c] = 0.0f;
    else if (sel == 5)
      dst[c] = 1.0f;
  }
}

f32 Dot3(const Vec4 &a, const Vec4 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
f32 Dot4(const Vec4 &a, const Vec4 &b) { return Dot3(a, b) + a[3] * b[3]; }
// dotP(a.wzxy, b.wxzy): the weights' x and z cross over (as the shader does).
f32 DotWzxy(const Vec4 &a, const Vec4 &w) { return a[3] * w[3] + a[2] * w[0] + a[0] * w[2] + a[1] * w[1]; }

// A memexport stream constant: x = base address in dwords (bits 0-29),
// y = 2^23 (the element index is added to it), z = format word.
struct ExportStream {
  u32 base_dwords = 0, format = 0, endian = 0;
  bool is_signed = false, integer = false;
  bool valid() const { return base_dwords != 0; }
};

ExportStream ReadExportStream(DeviceView dev, u32 reg) {
  const u32 x = dev.U32(dev::kVsFloatConstants + reg * 16);
  const u32 z = dev.U32(dev::kVsFloatConstants + reg * 16 + 8);
  ExportStream e;
  e.base_dwords = x & 0x3FFFFFFFu;
  e.endian = z & 7;
  e.format = (z >> 8) & 0x3F;
  // Bits 16-18: the number format (0 unsigned and 1 signed normalized, 4
  // unsigned and 5 signed integer, 7 float). Only the normalized ones scale a
  // fixed-point field: the skinning shaders scale their normals by 511
  // themselves and export them as float, and scaled again every normal
  // saturated to a cube corner (streaky lighting, patchy dog fur).
  const u32 num_format = (z >> 16) & 7;
  e.is_signed = num_format & 1;
  e.integer = REXCVAR_GET(fable2_memexport_unnormalized_float) ? num_format != 0 && num_format != 1
                                                                : !((z >> 17) & 1);
  return e;
}

bool PackExport(const ExportStream &e, const Vec4 &v, u32 &out) {
  auto clamp_int = [](f32 f, i32 lo, i32 hi) {
    return static_cast<i32>(std::clamp(std::nearbyint(f), static_cast<f32>(lo), static_cast<f32>(hi)));
  };
  auto field = [&](f32 f, u32 width) -> u32 {
    if (e.is_signed) {
      const i32 hi = (1 << (width - 1)) - 1;
      const f32 x = e.integer ? f : f * static_cast<f32>(hi);
      return static_cast<u32>(clamp_int(x, -hi - 1, hi)) & ((1u << width) - 1);
    }
    const i32 hi = static_cast<i32>((1u << width) - 1);
    const f32 x = e.integer ? f : f * static_cast<f32>(hi);
    return static_cast<u32>(clamp_int(x, 0, hi));
  };
  switch (e.format) {
  case 6:
    out = field(v[0], 8) | field(v[1], 8) << 8 | field(v[2], 8) << 16 | field(v[3], 8) << 24;
    return true;
  case 7:
    out = field(v[0], 10) | field(v[1], 10) << 10 | field(v[2], 10) << 20 | field(v[3], 2) << 30;
    return true;
  case 24:  // k_16_16 (color format numbering)
    out = field(v[0], 16) << 16 | field(v[1], 16);
    return true;
  case 31:
    out = FloatToHalf(v[0]) << 16 | FloatToHalf(v[1]);
    return true;
  case 36:
    out = std::bit_cast<u32>(v[0]);
    return true;
  default:
    return false;
  }
}

void StoreExport(const ExportStream &e, u32 element, u32 value) {
  u8 *p = mem::phys<u8>((e.base_dwords + element) * 4u);
  if (!p)
    return;
  // Endian128 modes 1..3 as for fetches (memory is big-endian dwords).
  const u32 raw = Swap(value, e.endian ? e.endian : 0);
  std::memcpy(p, &raw, 4);
}

bool ReadFetches(const GuestShader &vs, Fetch (&fetches)[kFetchCount]) {
  const u32 container = vs.va + obj::kVertexShaderContainer;
  const u32 shader_offset = mem::load<u32>(container + 0x18);
  const u32 physical_offset = mem::load<u32>(container + shader_offset);
  const u32 code_va = mem::load<u32>(vs.va + obj::kVertexShaderPhysical);
  const u8 *code = code_va ? mem::at<u8>(code_va + physical_offset) : nullptr;
  if (!code)
    return false;
  u32 parent_stride = 0;
  for (u32 i = 0; i < kFetchCount; ++i) {
    const u8 *p = code + (kFirstFetch + i) * 12;
    auto word = [&](u32 k) { return rex::memory::load_and_swap<u32>(p + 4 * k); };
    const u32 w0 = word(0), w1 = word(1), w2 = word(2);
    Fetch &f = fetches[i];
    f.src = (w0 >> 5) & 0x3F;
    f.dst = (w0 >> 12) & 0x3F;
    if ((w0 & 0x1F) != 0 || !((w0 >> 19) & 1) || f.dst != kExpectedDst[i])
      return false;
    f.slot = ((w0 >> 20) & 0x1F) * 3 + ((w0 >> 25) & 3);
    f.srcComp = (w0 >> 30) & 3;
    f.dstSwizzle = w1 & 0xFFF;
    f.flags = ((w1 >> 12) & 1) | (((w1 >> 13) & 1) << 1);
    f.rounded = (w1 >> 15) & 1;
    f.format = (w1 >> 16) & 0x3F;
    f.expAdjust = static_cast<i32>(w1 << 2) >> 26;
    f.mini = (w1 >> 30) & 1;
    f.stride = w2 & 0xFF;
    f.offset = static_cast<i32>(w2 << 1) >> 9;
    if (!f.mini)
      parent_stride = f.stride;
    else
      f.stride = parent_stride;
  }
  return true;
}

// The microcode's vfetches are unbound (D3D binds a copy per declaration), so
// the stream, offset, stride and format come from the bound declaration.
bool BindFetches(DeviceView dev, Fetch (&fetches)[kFetchCount], std::string *desc) {
  const u32 decl_va = dev.U32(dev::kVertexDeclaration);
  const u32 count = decl_va ? mem::load<u32>(decl_va + obj::kDeclElementCount) : 0;
  const DeclElement *elements = count && count <= 32 ? mem::at<DeclElement>(decl_va + obj::kDeclElements) : nullptr;
  if (!elements)
    return false;
  if (desc)
    for (u32 e = 0; e < count; ++e)
      *desc += std::format(" u{}.{}@s{}+{} t{:#x}", elements[e].usage, elements[e].usageIndex,
                           static_cast<u32>(elements[e].stream), static_cast<u32>(elements[e].offset),
                           static_cast<u32>(elements[e].type));
  for (u32 i = 0; i < kFetchCount; ++i) {
    const DeclElement *match = nullptr;
    for (u32 e = 0; e < count && !match; ++e)
      if (elements[e].usage == kFetchUsage[i][0] && elements[e].usageIndex == kFetchUsage[i][1])
        match = &elements[e];
    if (!match)
      return false;
    Fetch &f = fetches[i];
    const u32 stream = match->stream, type = match->type;
    f.slot = 95 - stream;
    f.stride = dev.U8(dev::kStreamStride0 + stream);
    f.offset = static_cast<i32>(static_cast<u32>(match->offset) / 4);
    f.format = type & 0x3F;
    f.flags = ((type >> 8) & 1) | (((type >> 9) & 1) << 1);
    f.bgra = ((type >> 16) & 0xFF) == 0x18;
    f.expAdjust = 0;
    f.mini = false;
  }
  return true;
}

}  // namespace

bool EmulateMemExportDraw(u32 device_va, const u32 *indices, u32 index_count, u32 first, u32 count) {
  if (!device_va || !REXCVAR_GET(fable2_emulate_skinning))
    return false;
  DeviceView dev = Device(device_va);
  u32 vs_va = dev.U32(dev::kVertexShader);
  if (const u32 owned = g_gpu_owned_vs.load(std::memory_order_relaxed))
    vs_va = owned;
  auto &s = state();
  // Point-list draws are dropped before capture, so their shaders may not be
  // registered yet.
  const GuestShader *vs = vs_va ? FindGuestShader(s, vs_va, false) : nullptr;
  if (!vs && vs_va)
    vs = RegisterGuestShader(s, vs_va, false);
  if (!vs || vs->hash != kSkinningShader)
    return false;

  static u64 frame = ~0ull;
  static u32 frame_vertices = 0, frame_draws = 0, logs = 0;
  if (frame != s.guest_frames) {
    if (frame != ~0ull && (s.guest_frames % 600) == 0)
      EOT_INFO("[memexport] skinning: {} draws, {} vertices last frame", frame_draws, frame_vertices);
    frame = s.guest_frames;
    frame_vertices = frame_draws = 0;
  }
  const u32 n = indices ? index_count : count;
  frame_draws++;
  frame_vertices += n;

  Fetch fetches[kFetchCount];
  if (!ReadFetches(*vs, fetches)) {
    if (logs++ < 4)
      EOT_WARN("[memexport] skinning vs {:#x}: unexpected microcode", vs_va);
    return true;
  }
  std::string decl_desc;
  if (!BindFetches(dev, fetches, logs < 4 ? &decl_desc : nullptr)) {
    if (logs++ < 4)
      EOT_WARN("[memexport] skinning: declaration lacks an input:{}", decl_desc);
    return true;
  }
  const ExportStream position = ReadExportStream(dev, 8), normal = ReadExportStream(dev, 46),
                     texcoord = ReadExportStream(dev, 47);
  Stream streams[96];
  bool bad_format = false;
  for (const Fetch &f : fetches) {
    Stream &st = streams[f.slot];
    if (st.data)
      continue;
    const u32 a = dev.U32(dev::kFetchConstants + 8 * f.slot), b = dev.U32(dev::kFetchConstants + 8 * f.slot + 4);
    if ((a & 3) == 3) {
      st.data = mem::phys<u8>(a & ~3u);
      st.dwords = (b >> 2) & 0xFFFFFF;
      st.endian = b & 3;
    }
  }
  if (logs < 4) {
    logs++;
    std::string desc;
    for (const Fetch &f : fetches)
      desc += std::format(" r{}<-s{}[r{}.{}*{}{:+}]f{}{}", f.dst, f.slot, f.src, "xyzw"[f.srcComp], f.stride,
                          f.offset, f.format, f.mini ? "m" : "");
    EOT_INFO("[memexport] skinning vs {:#x} n {} pos base {:#x} fmt {} endian {} | nrm {:#x} fmt {} s{} i{} | "
             "tc {:#x} fmt {} | fetches{}",
             vs_va, n, position.base_dwords * 4, position.format, position.endian, normal.base_dwords * 4,
             normal.format, normal.is_signed, normal.integer, texcoord.base_dwords * 4, texcoord.format, desc);
    EOT_INFO("[memexport] skinning declaration:{}", decl_desc);
  }
  if (!position.valid())
    return true;
  {
    // Debug (dog fur): the vertices this skinning draw exports.
    static u32 range_logs = 0;
    if (range_logs < 6 && (s.guest_frames % 300) == 0) {
      range_logs++;
      u32 vmin = ~0u, vmax = 0;
      for (u32 k = 0; k < n; ++k) {
        const u32 v = indices ? indices[k] : first + k;
        vmin = std::min(vmin, v);
        vmax = std::max(vmax, v);
      }
      EOT_INFO("[memexport] skinning exports {} vertices {}..{} ({}) to {:#x}", n, vmin, vmax,
               indices ? "indexed" : "linear", position.base_dwords * 4);
    }
  }

  for (u32 k = 0; k < n; ++k) {
    const u32 vertex = indices ? indices[k] : first + k;
    Vec4 r[32];
    r[0][0] = static_cast<f32>(vertex);
    for (const Fetch &f : fetches) {
      const f32 idx = r[f.src][f.srcComp];
      const i64 element = static_cast<i64>(std::floor(f.rounded ? idx + 0.5f : idx)) * f.stride + f.offset;
      Vec4 val;
      if (!Decode(streams[f.slot], element, f.format, f.flags, std::ldexp(1.0f, f.expAdjust), val))
        bad_format = true;
      if (f.bgra)
        std::swap(val[0], val[2]);
      WriteDst(r[f.dst], val, f.dstSwizzle);
    }
    // The shader's math (XenosRecomp output of 5243696187782727).
    const Vec4 pos{{r[9][0], r[9][1], r[9][2], 1.0f}};
    const Vec4 &nrm = r[16], &w = r[3];
    auto xyz = [](const Vec4 &v, u32 first_comp) {
      return Vec4{{v[first_comp], v[first_comp + 1], v[first_comp + 2], 0.0f}};
    };
    const Vec4 a{{Dot4(r[4], pos), Dot4(r[27], pos), Dot4(r[26], pos), Dot4(r[25], pos)}};
    const Vec4 b{{Dot4(r[8], pos), Dot4(r[15], pos), Dot4(r[14], pos), Dot4(r[13], pos)}};
    const Vec4 c{{Dot4(r[1], pos), Dot4(r[12], pos), Dot4(r[11], pos), Dot4(r[10], pos)}};
    const Vec4 n5{{Dot3(nrm, r[24]), Dot3(nrm, xyz(r[5], 1)), Dot3(nrm, r[23]), Dot3(nrm, r[22])}};
    const Vec4 n6{{Dot3(nrm, r[21]), Dot3(nrm, xyz(r[6], 1)), Dot3(nrm, r[20]), Dot3(nrm, r[19])}};
    const Vec4 n7{{Dot3(nrm, r[18]), Dot3(nrm, xyz(r[7], 1)), Dot3(nrm, r[17]), Dot3(nrm, xyz(r[0], 1))}};
    const f32 px = DotWzxy(a, w), py = DotWzxy(b, w), pz = DotWzxy(c, w);
    f32 ny = DotWzxy(n7, w), nz = DotWzxy(n6, w), nw = DotWzxy(n5, w);
    const f32 len2 = nz * nz + ny * ny + nw * nw;
    const f32 inv = len2 > 0.0f ? 1.0f / std::sqrt(len2) : 0.0f;
    ny *= inv, nz *= inv, nw *= inv;
    // r1.yzw = r1.wzy * 511, exported as r1.wyz.
    const Vec4 nexp{{ny * 511.0f, nw * 511.0f, nz * 511.0f, ny * 511.0f}};
    // The texture coordinate passes through: stored in the source's half
    // order, as the fur shells read it like the body reads its own (exported
    // r2.yx, these halves came out exchanged and the fur sampled its prelit
    // coat mirrored across the diagonal: the dog's black and white back).
    const Vec4 tc{{r[2][0], r[2][1], r[2][0], r[2][0]}};
    const u32 base = 5 * vertex;
    StoreExport(position, base + 0, std::bit_cast<u32>(px));
    StoreExport(position, base + 1, std::bit_cast<u32>(py));
    StoreExport(position, base + 2, std::bit_cast<u32>(pz));
    u32 packed;
    if (texcoord.valid() && PackExport(texcoord, tc, packed))
      StoreExport(texcoord, base + 3, packed);
    else
      bad_format = true;
    if (normal.valid() && PackExport(normal, nexp, packed))
      StoreExport(normal, base + 4, packed);
    else
      bad_format = true;
  }
  if (bad_format) {
    static u32 bad_logs = 0;
    if (bad_logs++ < 4)
      EOT_WARN("[memexport] skinning: unsupported fetch or export format (see the fetch list above)");
  }
  return true;
}

}  // namespace eot::gpu
