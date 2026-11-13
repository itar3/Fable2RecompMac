// gpu/draw.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/draw.h"
#include "gpu/queries.h"

#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <xxhash.h>

#include "core/profiling.h"

#include "gpu/settings.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <set>
#include <vector>

#include <rex/graphics/xenos.h>
#include <rex/cvar.h>
#include <rex/memory/utils.h>

#if defined(EOT_D3D12)
#include <plume_d3d12.h>
#else
#include <plume_vulkan.h>
#endif

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/debug/freecam.h"
#include "gpu/render_thread.h"
#include "gpu/backend.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/gpu_timing.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/textures.h"
#include "gpu/writeback.h"
#include "gpu/shaders/guest_shaders.h"
#include "gpu/surfaces.h"
#include "gpu/taa.h"
#include "gpu/trace.h"
#include "gpu/vertex_layout.h"

REXCVAR_DEFINE_BOOL(fable2_geometry_caches, true, "Fable2",
                    "Native renderer: reuse host copies of vertex/index data whose sampled bytes "
                    "look unchanged (dynamic buffers rewritten without Lock went stale)");
REXCVAR_DEFINE_BOOL(fable2_full_constant_upload, false, "Fable2",
                    "Native renderer: compare the whole float constant file on every draw "
                    "(ignore the guest's pending mask)");
REXCVAR_DEFINE_BOOL(fable2_vertex_index_with_base, true, "Fable2",
                    "Native renderer: the vertex shader's index (r0) includes the base vertex");
REXCVAR_DEFINE_INT32(fable2_debug_null_slots, 0, "Fable2",
                     "Native renderer diagnostic: bitmask of texture slots left unbound");
REXCVAR_DEFINE_BOOL(fable2_debug_depth_always, false, "Fable2", "Debug: depth test always passes");
REXCVAR_DEFINE_BOOL(fable2_debug_cull_none, false, "Fable2", "Debug: no face culling");
REXCVAR_DEFINE_BOOL(fable2_pop_counter, false, "Fable2", "Diagnostic: count objects that vanish for 1-3 frames ([pops]; the smoke test turns it on)");
REXCVAR_DEFINE_BOOL(fable2_pull_preswap, false, "Fable2", "Native renderer: vertex-pulled 8-in-32 data is byte-swapped on the CPU while copied (the shader skips it)");
REXCVAR_DEFINE_BOOL(fable2_diag_morph, false, "Fable2",
                    "Diagnostic: count rewrites of morph streams (hashes every such stream per draw)");
REXCVAR_DEFINE_BOOL(fable2_pool_spare, true, "Fable2", "Native renderer: geometry cache pools make their next chunk ahead on a background thread");
REXCVAR_DEFINE_BOOL(fable2_pull_mirrors, true, "Fable2", "Native renderer: vertex-pulled stream buffers come from the mirror cache instead of a copy per draw");
REXCVAR_DEFINE_BOOL(fable2_debug_skip_256_draws, false, "Fable2", "Debug: skip colour draws into 256-pixel-wide targets (cost test)");
REXCVAR_DEFINE_BOOL(fable2_diag_shadow_seq, false, "Fable2", "Diagnostic: log one frame's shadow-pass draws in order with their pixel shaders");
REXCVAR_DEFINE_BOOL(fable2_diag_shadow_mask, false, "Fable2", "Diagnostic: whether the shadow mask samples this frame's shadow maps");
REXCVAR_DEFINE_STRING(fable2_debug_shadow_skip_vs, "", "Fable2", "Debug: vertex shader hashes not drawn into shadow maps");
REXCVAR_DEFINE_BOOL(fable2_debug_shadow_skip_tess, false, "Fable2", "Debug: landscape patches are not drawn into shadow maps");
REXCVAR_DEFINE_INT32(fable2_dump_shadow_maps, 0, "Fable2", "Debug: every N frames, dump the shadow maps the shadow mask samples (with fable2_diag_shadow_mask)");
REXCVAR_DEFINE_BOOL(fable2_debug_7e3_no_alpha, false, "Fable2", "Debug: no alpha writes to k_2_10_10_10_FLOAT targets");
REXCVAR_DEFINE_BOOL(fable2_diag_7e3, false, "Fable2", "Diagnostic: blend use of k_2_10_10_10_FLOAT targets' alpha");
REXCVAR_DEFINE_BOOL(fable2_diag_pull, false, "Fable2", "Diagnostic: log how much vertex-pulling data repeats or changes");
REXCVAR_DEFINE_BOOL(fable2_diag_interp, false, "Fable2", "Diagnostic: log interpolants written by vertex shaders vs read by pixel shaders");
REXCVAR_DEFINE_BOOL(fable2_debug_no_alpha_test, false, "Fable2", "Debug: alpha test always passes");
// Fable 2's vertex shaders read their atmospheric scattering tables (and the
// terrain its height maps) through vertex textures; unbound they read zero and
// distant tree billboards drew black over the sky.
REXCVAR_DEFINE_BOOL(fable2_vertex_textures, true, "Fable2",
                    "Native renderer: bind vertex shader textures (fetch constants 16..19)");
REXCVAR_DEFINE_BOOL(fable2_zero_tile_offset, true, "Fable2",
                    "Native renderer: drop the predicated-tiling offset from g_TileOffsetSize");
REXCVAR_DEFINE_STRING(fable2_tile_offset_exempt, "", "Fable2",
                      "Debug: pixel shader hashes that keep g_TileOffsetSize.xy");
REXCVAR_DEFINE_INT32(fable2_debug_ps_bool_clear, 0, "Fable2",
                     "Debug: bitmask of pixel shader booleans 0..31 forced false");
REXCVAR_DEFINE_STRING(fable2_skip_vs, "", "Fable2",
                      "Debug: comma-separated vertex shader hashes (optionally hash:indexcount) whose draws are skipped");
REXCVAR_DEFINE_STRING(fable2_diag_vs, "", "Fable2",
                      "Debug: vertex shader hashes (optionally hash:indexcount) whose bound textures are logged");
REXCVAR_DEFINE_STRING(fable2_debug_vs_consts, "", "Fable2",
                      "Debug: \"vshash:first-last\" logs those vertex shader constants for its draws");
REXCVAR_DEFINE_STRING(fable2_debug_ps_consts, "", "Fable2",
                      "Debug: \"pshash:first-last\" logs those pixel shader constants for its draws");
REXCVAR_DEFINE_BOOL(fable2_debug_dump_fur, false, "Fable2",
                    "Debug: dump the vertex streams of the dog's fur shells and body once (logs/*.bin)");
REXCVAR_DEFINE_BOOL(fable2_debug_hero_composite, false, "Fable2",
                    "Debug: log the draws into 512x512 colour targets and the resolves into the hero/dog "
                    "composite textures (placement of the coat)");
REXCVAR_DEFINE_STRING(fable2_skip_ps, "", "Fable2",
                      "Debug: comma-separated pixel shader hashes whose draws are skipped");
REXCVAR_DEFINE_INT32(fable2_tess_segments, 16, "Fable2",
                     "Native renderer: grid segments per edge for tessellated patch draws (the landscape)");
REXCVAR_DEFINE_INT32(fable2_debug_null_vs_slots, 0, "Fable2",
                     "Debug: bitmask of vertex samplers (bit 0 = fetch constant 16) left unbound");
REXCVAR_DEFINE_BOOL(fable2_tess_readback, false, "Fable2",
                    "Debug: log the GPU's r0 and position for the first vertices of a landscape patch draw");
REXCVAR_DEFINE_BOOL(fable2_tess_no_blend, false, "Fable2",
                    "Debug: landscape patch draws are drawn opaque (no blending)");
REXCVAR_DEFINE_BOOL(fable2_tess_no_discard, false, "Fable2",
                    "Debug: landscape patch draws ignore their theme weight threshold (c114.x)");
REXCVAR_DEFINE_STRING(fable2_debug_opaque_vs, "", "Fable2", "Debug: vertex shader hashes drawn without blending");
REXCVAR_DEFINE_STRING(fable2_debug_no_depth_vs, "", "Fable2", "Debug: vertex shader hashes drawn without depth test");
REXCVAR_DEFINE_STRING(fable2_readback_vs, "", "Fable2", "Debug: vertex shader hashes whose first vertices' clip positions are read back from the GPU");
REXCVAR_DEFINE_STRING(fable2_debug_depth_scale_vs, "", "Fable2", "Debug: vertex shader hashes whose depth is scaled by fable2_debug_depth_scale");
REXCVAR_DEFINE_DOUBLE(fable2_debug_depth_scale, 1.0, "Fable2", "Debug: depth scale for fable2_debug_depth_scale_vs (> 1 pulls toward the camera with reversed depth)");
REXCVAR_DEFINE_BOOL(fable2_debug_no_cull, false, "Fable2", "Debug: draw every triangle two-sided");
REXCVAR_DEFINE_BOOL(fable2_plane_scene_depth, true, "Fable2",
                    "Fable 2: the half-res effects pass's depth plane takes the scene depth bound with it");
REXCVAR_DEFINE_BOOL(fable2_offsets_guest_texels, true, "Fable2",
                    "Fable 2: pixel shader fetch offsets and weights in guest texels on render-scaled textures "
                    "(false: host texels, the old behaviour; A/B switch)");
REXCVAR_DEFINE_BOOL(fable2_alpha_exact, true, "Fable2",
                    "Fable 2: alpha test GREATER is strict and alpha to mask also covers depth-only draws (tree leaf shadows)");
REXCVAR_DEFINE_BOOL(fable2_fetch_filter_override, true, "Fable2",
                    "Fable 2: fetch instructions' MinFilter/MagFilter replace the sampler's");
// Vertex pulling reads 16-bit pairs as Xenos does (x in the low half after the
// endian swap): right for the skinned characters' bone palettes. The tree
// billboards' instances (half4 position and radius) read that way swapped the
// radius and the height (giant trees covering the clouds); their vertex shaders
// read the pairs in memory order instead.
REXCVAR_DEFINE_STRING(fable2_pull_half_order_vs, "", "Fable2",
                      "Native renderer: vertex shader hashes whose pulled 16-bit pairs are read in memory order");
REXCVAR_DEFINE_STRING(fable2_debug_dump_bound_vs, "", "Fable2",
                      "Debug: on the diag frame, dump the textures bound by draws of these vertex shaders");
REXCVAR_DEFINE_INT32(fable2_debug_log_rt_size, 0, "Fable2",
                     "Debug: full diag lines for draws into NxN colour targets (300 at most; water mask hunt)");
REXCVAR_DEFINE_BOOL(fable2_fetch_lod_bias, true, "Fable2",
                    "Native renderer: apply the fetch constants' LOD bias (and the mirror scale correction) in "
                    "the shaders; Metal samplers have no LOD bias (false: the old behaviour, no bias)");
REXCVAR_DEFINE_STRING(fable2_debug_dump_mips_vs, "", "Fable2",
                      "Debug: vertex shader hashes whose bound textures fable2_debug_dump_mips dumps");
REXCVAR_DEFINE_INT32(fable2_debug_dump_mips, 0, "Fable2",
                     "Debug: each new value dumps every mip level (colour and alpha) of the textures bound "
                     "by the next draw of fable2_debug_dump_mips_vs");
REXCVAR_DEFINE_STRING(fable2_debug_log_ps, "", "Fable2",
                      "Debug: full diag lines for draws with these pixel shader hashes (300 at most)");
REXCVAR_DEFINE_STRING(fable2_debug_log_rt_skip_ps, "", "Fable2",
                      "Debug: pixel shader hashes fable2_debug_log_rt_size leaves out");
REXCVAR_DEFINE_BOOL(fable2_debug_log_rt512, false, "Fable2",
                    "Debug: log every draw into a 512x512 colour target (hero/dog texture composites)");
REXCVAR_DEFINE_STRING(fable2_zpass_debug_range, "", "Fable2",
                      "Debug: \"first,count\": the emulated Z pass runs only those of its draws");
REXCVAR_DEFINE_BOOL(fable2_zpass_skip_a2c, false, "Fable2",
                    "Native renderer: the emulated Z pass leaves out alpha-to-coverage draws");
REXCVAR_DEFINE_BOOL(fable2_zpass_skip_alpha_test, false, "Fable2",
                    "Native renderer: the emulated Z pass leaves out alpha-tested draws");
REXCVAR_DEFINE_BOOL(fable2_zpass_skip_stencil_test, false, "Fable2",
                    "Native renderer: the emulated Z pass leaves out stencil-tested draws");
REXCVAR_DEFINE_BOOL(fable2_zpass_skip_tess, false, "Fable2",
                    "Debug: the emulated Z pass leaves out tessellated (landscape) draws");
REXCVAR_DEFINE_STRING(fable2_zpass_skip_vs, "", "Fable2",
                      "Debug: vertex shader hashes the emulated Z pass leaves out");
REXCVAR_DEFINE_BOOL(fable2_const_full_file, false, "Fable2",
                    "Native renderer: upload all 256 float constant registers per draw (A/B; off: only up to "
                    "the shader's last register)");
REXCVAR_DEFINE_BOOL(fable2_zpass_depth_only, true, "Fable2",
                    "Native renderer: the emulated Z pass draws on a depth-only framebuffer, without pixel "
                    "shaders unless they can drop pixels");
REXCVAR_DEFINE_BOOL(fable2_zpass_restore_depth, true, "Fable2",
                    "Native renderer: the scene depth is put back after the emulated Z pass's early resolve");
REXCVAR_DEFINE_BOOL(fable2_zpass_emulation, true, "Fable2",
                    "Native renderer: emulate the Xbox 360 Z pass, so the screen shadow mask reads this frame's depth");
REXCVAR_DEFINE_STRING(fable2_zpass_trigger_ps, "383793fa8d5a0716", "Fable2",
                      "Native renderer: pixel shaders whose draw starts the emulated Z pass");
REXCVAR_DEFINE_BOOL(fable2_shadow_bias_in_vs, true, "Fable2",
                    "Native renderer: shadow casters' constant polygon offset is applied in the vertex shader");
REXCVAR_DEFINE_STRING(fable2_pull_up_stream_vs, "e6415e8304306082", "Fable2",
                      "Native renderer: vertex shader hashes whose pulled stream 0 is a BeginVertices draw's own data");
// On by default: the game relies on it. The item cards' 3D preview (a warrant
// lit by the card's spot light) multiplied an infinite constant by zero; IEEE
// made every pixel NaN and the 7e3 target scrubbed it to black.
REXCVAR_DEFINE_BOOL(fable2_query_scene_depth, true, "Fable2",
                    "Native renderer: the occlusion query pass (scene / 4) gets the scene depth instead of the plane");
REXCVAR_DEFINE_BOOL(fable2_zero_mul, true, "Fable2",
                    "Native renderer: shaders multiply as Xenos does (0 * anything = 0, also "
                    "infinity and NaN, in mul, mad and dot products)");
REXCVAR_DEFINE_BOOL(fable2_debug_nan_paint, false, "Fable2",
                    "Debug: pixel shader outputs that are NaN or infinite turn magenta");
REXCVAR_DEFINE_BOOL(fable2_texture_as_recorded, true, "Fable2",
                    "Native renderer: draws and resolves use their textures' headers as recorded "
                    "(Fable 2 re-points headers at other render targets before the replay)");
REXCVAR_DEFINE_DOUBLE(fable2_debug_point_scale, 1.0, "Fable2", "Debug: point sprite size multiplier");
REXCVAR_DEFINE_STRING(fable2_point_ps_allow, "ef43abe324f6e039", "Fable2",
                      "Debug: comma-separated pixel shader hashes whose point-list draws are drawn");
REXCVAR_DEFINE_BOOL(fable2_skip_point_lists, true, "Fable2",
                    "Native renderer: skip point-list draws (point sprites not supported yet)");

namespace eot::gpu {
std::atomic<u64> g_pop_events{0}, g_pop_draws{0};
}

namespace eot::gpu {

namespace {

namespace xe = rex::graphics::xenos;

constexpr u32 kPrimTriangleList = 4;
constexpr u32 kPrimTriangleStrip = 6;
constexpr u32 kPrimTriangleFan = 5;
constexpr u32 kPrimRectList = 8;
constexpr u32 kPrimQuadList = 13;
constexpr u32 kPrimLineStrip = 3;

struct Targets {
  GuestSurface *color[4] = {};
  u32 colorCount = 0;
  GuestSurface *depth = nullptr;
  HostTexture *colorImage[4] = {};
  HostTexture *depthImage = nullptr;
  u32 samples = 1;
  bool writesDepthStencil = false;
  bool writesDepth = false;
  bool writesColor = true;
  bool additive = false;
  u32 width = 0;
  u32 height = 0;
  float scale = 1.0f;
  i32 offsetX = 0;
  i32 offsetY = 0;
  velocity::Target *velocity = nullptr;
};

struct ViewportInfo {
  plume::RenderViewport vp;
  plume::RenderRect scissor;
  float posScale[4] = {1, 1, 1, 1};
  float posOffset[4] = {0, 0, 0, 0};
};

void Dropped(const char *why, u64 site) {
  trace::Bump(trace::Counter::DrawDropped);
  u32 n;
  if (DiagShouldLog(site, &n))
    EOT_WARN("[draw] dropped: {} (x{})", why, n + 1);
  auto &s = state();
  if (Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame()))
    EOT_INFO("[diag] dropped draw: {} vs {:#x} ps {:#x}", why, s.current_vs_va, s.current_ps_va);
}

bool ResolveTargets(VideoState &s, DeviceView dev, Targets &t) {
  t = Targets{};
  for (u32 i = 0; i < 4; ++i) {
    const u32 va = dev.U32(dev::kRenderTarget0 + 4 * i);
    if (!va)
      break;
    GuestSurface *surf = GetGuestSurface(s, va);
    if (!surf || !SurfaceHasImage(*surf))
      break;
    const u32 packet = dev.U32(i == 0 ? dev::kColor0Info : dev::kColor1Info + 4 * (i - 1));
    const u32 bias = (packet >> 20) & 0x3F;
    surf->colorExpBias = bias & 0x20 ? static_cast<i32>(bias) - 64 : static_cast<i32>(bias);
    t.color[i] = surf;
    t.colorImage[i] = surf->host.valid() ? &surf->host : &surf->single;
    t.colorCount = i + 1;
  }
  const u32 ds_va = dev.U32(dev::kDepthSurface);
  if (ds_va)
    t.depth = GetGuestSurface(s, ds_va);
  if (t.depth && !SurfaceHasImage(*t.depth))
    t.depth = nullptr;
  if (t.depth)
    t.depthImage = t.depth->host.valid() ? &t.depth->host : &t.depth->single;
  if (t.colorCount) {
    t.width = t.color[0]->width;
    t.height = t.color[0]->height;
  } else if (t.depth) {
    t.width = t.depth->width;
    t.height = t.depth->height;
  }
  if (t.colorCount)
    t.scale = t.color[0]->scale;
  else if (t.depth)
    t.scale = t.depth->scale;
  t.samples = t.colorCount ? t.color[0]->host.sampleCount
                           : (t.depth ? t.depth->host.sampleCount : 1u);
  return t.colorCount || t.depth;
}

bool CaptureTargetWords(DeviceView dev, TargetWords &tw) {
  tw.colorCount = 0;
  for (u32 i = 0; i < 4; ++i) {
    const u32 va = dev.U32(dev::kRenderTarget0 + 4 * i);
    if (!va || !ReadSurfaceHeaderWords(va, tw.colorWords[i]))
      break;
    tw.colorVa[i] = va;
    tw.colorInfo[i] = dev.U32(i == 0 ? dev::kColor0Info : dev::kColor1Info + 4 * (i - 1));
    tw.colorCount = i + 1;
  }
  tw.depthVa = dev.U32(dev::kDepthSurface);
  if (tw.depthVa && !ReadSurfaceHeaderWords(tw.depthVa, tw.depthWords))
    tw.depthVa = 0;
  return tw.colorCount || tw.depthVa;
}

bool ResolveTargetsFromWords(VideoState &s, const TargetWords &tw, Targets &t) {
  t = Targets{};
  for (u32 i = 0; i < tw.colorCount; ++i) {
    GuestSurface *surf = GetGuestSurfaceWords(s, tw.colorVa[i], tw.colorWords[i]);
    if (!surf || !SurfaceHasImage(*surf))
      break;
    const u32 bias = (tw.colorInfo[i] >> 20) & 0x3F;
    surf->colorExpBias = bias & 0x20 ? static_cast<i32>(bias) - 64 : static_cast<i32>(bias);
    t.color[i] = surf;
    t.colorImage[i] = surf->host.valid() ? &surf->host : &surf->single;
    t.colorCount = i + 1;
  }
  if (tw.depthVa)
    t.depth = GetGuestSurfaceWords(s, tw.depthVa, tw.depthWords);
  if (t.depth && !SurfaceHasImage(*t.depth))
    t.depth = nullptr;
  if (t.depth)
    t.depthImage = t.depth->host.valid() ? &t.depth->host : &t.depth->single;
  if (t.colorCount) {
    t.width = t.color[0]->width;
    t.height = t.color[0]->height;
  } else if (t.depth) {
    t.width = t.depth->width;
    t.height = t.depth->height;
  }
  if (t.colorCount)
    t.scale = t.color[0]->scale;
  else if (t.depth)
    t.scale = t.depth->scale;
  t.samples = t.colorCount ? t.color[0]->host.sampleCount
                           : (t.depth ? t.depth->host.sampleCount : 1u);
  return t.colorCount || t.depth;
}

struct DrawClass {
  bool nullPs = false;
  bool depthTest = false;
  bool depthWrite = false;
  bool stencil = false;
  bool stencilWrite = false;
  bool blend = false;
  bool additive = false;
  bool rect = false;
  bool eightBit = false;
};

DrawClass ClassifyDraw(DeviceView dev, const Targets &t, bool has_ps, bool rect_list) {
  DrawClass c;
  c.nullPs = !has_ps;
  c.rect = rect_list;
  const u32 dc = dev.U32(dev::kDepthControl);
  const bool has_ds = t.depth != nullptr;
  c.depthTest = has_ds && (dc & 2);
  c.depthWrite = has_ds && (dc & 4);
  c.stencil = has_ds && (dc & 1);
  if (c.stencil) {
    const u32 front_ops = ((dc >> 11) & 7) | ((dc >> 14) & 7) | ((dc >> 17) & 7);
    const u32 back_ops = (dc & 0x80) ? (((dc >> 23) & 7) | ((dc >> 26) & 7) | ((dc >> 29) & 7)) : 0;
    const u32 write_mask = (dev.U32(dev::kStencilRefMask) >> 16) & 0xFF;
    c.stencilWrite = (front_ops | back_ops) != 0 && write_mask != 0;
  }
  if (t.colorCount) {
    const u32 cc = dev.U32(dev::kColorControl);
    const u32 bc = dev.U32(dev::kBlendControl0);
    const u32 src = bc & 0x1F, op = (bc >> 5) & 7, dst = (bc >> 8) & 0x1F;
    const bool passthrough = src == 1 && dst == 0 && op == 0;
    c.blend = !(cc & (1u << 5)) && !passthrough;
    c.additive = c.blend && op == 0 && dst == 1;
    c.eightBit = t.color[0]->colorFormat <= 1; // k_8_8_8_8, k_8_8_8_8_GAMMA
  }
  return c;
}

u32 SelectPassSamples(const Targets &t, const DrawClass &c, bool stencil_twin) {
  const u32 ms = t.colorCount ? t.color[0]->host.sampleCount
                              : (t.depth ? t.depth->host.sampleCount : 1u);
  if (ms <= 1)
    return 1;
  const bool ms_depth = t.depth && t.depth->host.sampleCount > 1;
  if (ms_depth && c.stencil && !stencil_twin)
    return ms;
  if (!t.colorCount)
    return ms;
  const GuestSurface &c0 = *t.color[0];
  const bool resolved = c0.content == GuestSurface::Content::Borrowed ||
                        (c0.content == GuestSurface::Content::Drawn &&
                         (c0.contentInSingle || c0.imagesAgree || c0.resolvedSinceDraw));
  // The game's own multisampled targets keep their depth-tested draws
  // multisampled after a resolve: Fable 2 draws its scene in two predicated
  // tiles with a resolve and clear between them, and the second tile's world
  // went to the single-sample twin (the lit ground flashed between frames).
  const bool guest_ms = c0.msaaSamples > 1 && ms_depth && c.depthTest;
  if (c.nullPs) {
    if (ms_depth && resolved && !guest_ms)
      return 1;
    return ms_depth && !c.depthWrite && stencil_twin ? 1u : ms;
  }
  if (resolved && !guest_ms)
    return 1;
  if (ms_depth && c.depthWrite && !c.blend)
    return ms;
  if (!c.rect && !c.additive && !c.nullPs && c.depthTest && !c.blend && !c.eightBit)
    return ms;
  return c0.content == GuestSurface::Content::Drawn ? ms : 1u;
}

bool SelectTargetImages(VideoState &s, Targets &t, const DrawClass &c) {
  u32 samples = SelectPassSamples(t, c, s.stencil_ref_supported);
  if (t.depth && t.colorCount && samples > 1 && t.depth->host.sampleCount != samples)
    samples = 1;
  if (samples > 1) {
    for (u32 i = 0; i < t.colorCount && samples > 1; ++i)
      if (!t.color[i]->host.valid() && !SurfaceMakeMultisampled(s, *t.color[i]))
        samples = 1;
    if (samples > 1 && t.depth && t.depth->host.sampleCount == samples && !t.depth->host.valid() &&
        !SurfaceMakeMultisampled(s, *t.depth))
      samples = 1;
  }
  for (u32 i = 0; i < t.colorCount; ++i) {
    t.colorImage[i] = SurfaceImageForDraw(s, *t.color[i], samples, !c.nullPs);
    if (!t.colorImage[i])
      return false;
  }
  if (t.depth) {
    if (samples == t.depth->host.sampleCount) {
      if (samples > 1 && t.depth->singleDirty && !SurfacePropagateDepthSingle(s, *t.depth))
        return false;
      t.depthImage = &t.depth->host;
      if (!t.colorCount && t.depth->redirectMirror && t.depth->redirectMirror->host.valid()) {
        t.depthImage = &t.depth->redirectMirror->host;
        t.offsetX = t.depth->redirectX;
        t.offsetY = t.depth->redirectY;
      }
    } else {
      t.depthImage = SurfaceDepthSingle(s, *t.depth, c.depthTest || c.stencil || c.depthWrite);
      if (!t.depthImage)
        return false;
    }
  }
  t.samples = samples;
  t.writesDepthStencil = c.depthWrite || (c.stencilWrite && s.stencil_ref_supported);
  t.writesDepth = c.depthWrite;
  t.writesColor = !c.nullPs;
  t.additive = c.additive;
  return true;
}

bool BindImages(VideoState &s, HostTexture *const colors[4], u32 color_count, HostTexture *depth,
                bool init_clear) {
  HostTextureTransition transitions[kMaxPendingTransitions + 5];
  u32 transition_count = 0;
  for (u32 i = 0; i < s.pending_transition_count; ++i)
    transitions[transition_count++] = s.pending_transitions[i];
  s.pending_transition_count = 0;
  for (u32 i = 0; i < color_count; ++i)
    transitions[transition_count++] = {colors[i], plume::RenderTextureLayout::COLOR_WRITE};
  if (depth)
    transitions[transition_count++] = {depth, plume::RenderTextureLayout::DEPTH_WRITE};
  TransitionManyLocked(s, transitions, transition_count);
  if (depth && color_count && colors[0]->sampleCount != depth->sampleCount) {
    u32 n;
    if (DiagShouldLog(0x6C20, &n) && n < 8)
      EOT_WARN("[draw] colour {}x{} ({}x samples) bound with depth {}x{} ({}x samples)",
               colors[0]->width, colors[0]->height, colors[0]->sampleCount, depth->width,
               depth->height, depth->sampleCount);
  }
  bool same_targets = s.bound_draw_targets_valid && s.bound_framebuffer &&
                      s.bound_draw_color_count == color_count &&
                      s.bound_draw_depth == (depth ? depth->texture.get() : nullptr);
  for (u32 i = 0; same_targets && i < color_count; ++i)
    same_targets = s.bound_draw_colors[i] == colors[i]->texture.get();
  if (same_targets) {
    s.perf.framebuffer_cache_hits++;
  } else {
    plume::RenderFramebuffer *fb = GetFramebuffer(s, colors, color_count, depth);
    if (!fb)
      return false;
    if (s.bound_framebuffer != fb) {
      s.command_list->setFramebuffer(fb);
      s.bound_framebuffer = fb;
    }
    s.bound_draw_color_count = color_count;
    for (u32 i = 0; i < 4; ++i)
      s.bound_draw_colors[i] = i < color_count ? colors[i]->texture.get() : nullptr;
    s.bound_draw_depth = depth ? depth->texture.get() : nullptr;
    s.bound_draw_targets_valid = true;
  }
  if (!init_clear)
    return true;
  for (u32 i = 0; i < color_count; ++i) {
    if (!colors[i]->needsClear)
      continue;
    s.command_list->clearColor(i, plume::RenderColor(0, 0, 0, 0), nullptr, 0);
    colors[i]->needsClear = false;
  }
  if (depth && depth->needsClear) {
    s.command_list->clearDepthStencil(true, FormatHasStencil(depth->format), 0.0f, 0, nullptr, 0);
    depth->needsClear = false;
  }
  return true;
}

bool BindTargets(VideoState &s, Targets &t) {
  HostTexture *colors[4] = {};
  for (u32 i = 0; i < t.colorCount; ++i) {
    colors[i] = t.colorImage[i] ? t.colorImage[i] : &t.color[i]->host;
    if (t.writesColor)
      NoteSurfaceDrawn(*t.color[i], *colors[i], false);
    t.color[i]->lastUseFrame = s.guest_frames;
  }
  HostTexture *depth = nullptr;
  if (t.depth) {
    depth = t.depthImage ? t.depthImage : &t.depth->host;
    NoteSurfaceDrawn(*t.depth, *depth, t.writesDepthStencil, t.writesDepth);
    t.depth->lastUseFrame = s.guest_frames;
  }
  u32 color_count = t.colorCount;
  if (t.velocity && color_count < 4)
    colors[color_count++] = &t.velocity->image;
  if (!BindImages(s, colors, color_count, depth, true))
    return false;
  if (t.velocity)
    velocity::BeforeDraw(s, *t.velocity, color_count - 1);
  GpuTimingMark(s, s.command_list,
                GpuTargetCategory(t.width == kGuestRenderWidth && t.height == kGuestRenderHeight,
                                  t.depth != nullptr, t.colorCount,
                                  t.colorCount ? static_cast<u32>(colors[0]->format) : 0u,
                                  t.samples, t.additive));
  return true;
}

ViewportInfo ComputeViewport(DeviceView dev, const Targets &t, float jitter_x, float jitter_y) {
  ViewportInfo v;
  const float rt_w = static_cast<float>(std::max(1u, t.width));
  const float rt_h = static_cast<float>(std::max(1u, t.height));
  const u32 vte = dev.U32(dev::kVteControl);
  const float xs = dev.F32(dev::kVportXScale), xo = dev.F32(dev::kVportXOffset);
  const float ys = dev.F32(dev::kVportYScale), yo = dev.F32(dev::kVportYOffset);
  const float zs = dev.F32(dev::kVportZScale), zo = dev.F32(dev::kVportZOffset);
  const bool xs_en = vte & 1, xo_en = vte & 2, ys_en = vte & 4, yo_en = vte & 8;
  const bool zs_en = vte & 16, zo_en = vte & 32;

  float x0, w, y0, h;
  if (xs_en && xs != 0.0f) {
    w = 2.0f * std::fabs(xs);
    x0 = (xo_en ? xo : 0.0f) - std::fabs(xs);
    v.posScale[0] = xs < 0 ? -1.0f : 1.0f;
  } else {
    x0 = 0.0f;
    w = rt_w;
    v.posScale[0] = 2.0f / rt_w;
    v.posOffset[0] = (xo_en ? xo : 0.0f) * 2.0f / rt_w - 1.0f;
  }
  if (ys_en && ys != 0.0f) {
    h = 2.0f * std::fabs(ys);
    y0 = (yo_en ? yo : 0.0f) - std::fabs(ys);
    v.posScale[1] = ys < 0 ? 1.0f : -1.0f;
  } else {
    y0 = 0.0f;
    h = rt_h;
    v.posScale[1] = -2.0f / rt_h;
    v.posOffset[1] = 1.0f - (yo_en ? yo : 0.0f) * 2.0f / rt_h;
  }
  float zmin, zmax;
  if (zs_en) {
    if (zs >= 0.0f) {
      zmin = zo_en ? zo : 0.0f;
      zmax = zmin + zs;
    } else {
      zmax = zo_en ? zo : 0.0f;
      zmin = zmax + zs;
      v.posScale[2] = -1.0f;
      v.posOffset[2] = 1.0f;
    }
  } else {
    zmin = 0.0f;
    zmax = 1.0f;
    v.posOffset[2] = zo_en ? zo : 0.0f;
  }
  zmin = std::clamp(zmin, 0.0f, 1.0f);
  zmax = std::clamp(zmax, 0.0f, 1.0f);
  if (dev.U32(dev::kModeControl) & (1u << 11)) {
    // The vertex shader adds the constant offset exactly; the rasterizer's
    // constant bias on the host's float depth scales with each primitive's
    // depth exponent instead of the guest's fixed 2^-24 steps.
    const bool scene = t.colorCount > 0 || (t.width == kGuestRenderWidth && t.height == kGuestRenderHeight) ||
                       REXCVAR_GET(fable2_shadow_bias_in_vs);
    const float offset = dev.F32(dev::kPolyOffsetFrontOffset);
    if (scene && offset != 0.0f && std::isfinite(offset) && zmax > zmin)
      v.posOffset[2] += offset / (zmax - zmin);
  }
  if ((dev.U32(dev::kVtxControl) & 1) == 0) {
    v.posOffset[0] += 1.0f / std::max(1.0f, w);
    v.posOffset[1] -= 1.0f / std::max(1.0f, h);
  }
  if ((jitter_x != 0.0f || jitter_y != 0.0f) && xs_en && ys_en && t.width == kGuestRenderWidth &&
      t.height == kGuestRenderHeight) {
    v.posOffset[0] += jitter_x * 2.0f / std::max(1.0f, w * t.scale);
    v.posOffset[1] -= jitter_y * 2.0f / std::max(1.0f, h * t.scale);
  }
  const float S = t.scale;
  const float hx0 = std::round(x0 * S), hx1 = std::round((x0 + w) * S);
  const float hy0 = std::round(y0 * S), hy1 = std::round((y0 + h) * S);
  v.vp = plume::RenderViewport(hx0, hy0, std::max(hx1 - hx0, 1.0f), std::max(hy1 - hy0, 1.0f),
                               zmin, zmax);

  auto scissor_of = [](u32 tl, u32 br) {
    return plume::RenderRect(static_cast<i32>(tl & 0x7FFF), static_cast<i32>((tl >> 16) & 0x7FFF),
                             static_cast<i32>(br & 0x7FFF), static_cast<i32>((br >> 16) & 0x7FFF));
  };
  const plume::RenderRect win =
      scissor_of(dev.U32(dev::kWindowScissorTL), dev.U32(dev::kWindowScissorBR));
  const plume::RenderRect scr =
      scissor_of(dev.U32(dev::kScreenScissorTL), dev.U32(dev::kScreenScissorBR));
  plume::RenderRect sc;
  sc.left = std::max({0, win.left, scr.left});
  sc.top = std::max({0, win.top, scr.top});
  sc.right = std::min({static_cast<i32>(t.width), win.right, scr.right});
  sc.bottom = std::min({static_cast<i32>(t.height), win.bottom, scr.bottom});
  if (sc.right <= sc.left || sc.bottom <= sc.top)
    sc = plume::RenderRect(0, 0, static_cast<i32>(t.width), static_cast<i32>(t.height));
  sc = plume::RenderRect(ScalePxBy(sc.left, S), ScalePxBy(sc.top, S),
                         std::max(ScalePxBy(sc.right, S), ScalePxBy(sc.left, S) + 1),
                         std::max(ScalePxBy(sc.bottom, S), ScalePxBy(sc.top, S) + 1));
  if (t.offsetX || t.offsetY) {
    v.vp.x += static_cast<float>(t.offsetX);
    v.vp.y += static_cast<float>(t.offsetY);
    sc.left += t.offsetX;
    sc.right += t.offsetX;
    sc.top += t.offsetY;
    sc.bottom += t.offsetY;
  }
  v.scissor = sc;
  return v;
}

struct ConstFileCache {
  alignas(16) u8 bytes[256 * 16] = {};
  UploadAlloc alloc;
  u64 epoch = ~0ull;
  u32 regs = 0;
};

bool UploadFloatFile(VideoState &s, DeviceView dev, u32 offset, u32 stage, u32 regs,
                     u64 dirty, UploadAlloc *out) {
  if (REXCVAR_GET(fable2_full_constant_upload))
    dirty = ~0ull;
  static ConstFileCache caches[2];
  static const bool verify = Settings::FastSettersVerify();
  if (REXCVAR_GET(fable2_const_full_file))
    regs = 256;  // A/B: the old whole-file uploads
  regs = std::clamp(regs, 16u, 256u);
  const u32 bytes = regs * 16;
  const u8 *src = dev.Bytes(offset, bytes);
  if (!src)
    return false;
  ConstFileCache &c = caches[stage & 1];
  const u32 blocks = (bytes + 63) / 64;
  const u64 in_file = blocks >= 64 ? ~0ull : ((1ull << blocks) - 1);
  auto compare_blocks = [&](u64 mask) {
    u64 changed = 0;
    for (u64 m = mask & in_file; m; m &= m - 1) {
      const u32 b = static_cast<u32>(std::countr_zero(m));
      const u32 n = std::min(64u, bytes - 64 * b);
      if (std::memcmp(c.bytes + 64 * b, src + 64 * b, n) != 0)
        changed |= 1ull << b;
    }
    return changed;
  };
  u64 refresh = ~0ull;
  if (c.epoch == UploadRingEpoch() && c.regs >= regs) {
    if (dirty == 0) {
      *out = c.alloc;
      s.perf.const_file_hits++;
      s.perf.const_file_clean_hits++;
      return true;
    }
    u64 changed = dirty == ~0ull ? (std::memcmp(c.bytes, src, bytes) != 0 ? ~0ull : 0ull)
                                 : compare_blocks(dirty);
    if (verify) {
      const u64 actual = compare_blocks(~0ull);
      if (actual & ~dirty) {
        static u32 logged = 0;
        if (logged++ < 24)
          EOT_WARN("[draw] float file {} changed outside its pending mask: mask {:#018x} changed {:#018x} "
                   "(regs {})",
                   stage, dirty, actual, regs);
        s.perf.const_file_mask_misses++;
        changed |= actual;
      }
    }
    if (!changed) {
      *out = c.alloc;
      c.regs = regs;
      s.perf.const_file_hits++;
      return true;
    }
    refresh = changed;
  }
  if (!UploadAllocate(bytes, kConstantBufferAlignment, out))
    return false;
  rex::memory::copy_and_swap_32_unaligned(out->cpu, reinterpret_cast<const u32 *>(src), regs * 4);
  if (refresh == ~0ull) {
    std::memcpy(c.bytes, src, bytes);
  } else {
    for (u64 m = refresh & in_file; m; m &= m - 1) {
      const u32 b = static_cast<u32>(std::countr_zero(m));
      std::memcpy(c.bytes + 64 * b, src + 64 * b, std::min(64u, bytes - 64 * b));
    }
  }
  c.alloc = *out;
  c.epoch = UploadRingEpoch();
  c.regs = regs;
  s.perf.constant_bytes += bytes;
  return true;
}

void FillLoopConstants(DeviceView dev, u32 offset, i32 (*dst)[4]) {
  for (u32 i = 0; i < 16; ++i) {
    const u32 v = dev.U32(offset + 4 * i);
    dst[i][0] = static_cast<i32>(v & 0xFF);
    dst[i][1] = static_cast<i32>((v >> 8) & 0xFF);
    dst[i][2] = static_cast<i32>(static_cast<i8>((v >> 16) & 0xFF));
    dst[i][3] = 0;
  }
}

struct GeometryPlan {
  const CachedIndexRange *cached = nullptr;
  plume::RenderPrimitiveTopology topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  bool indexed = false;
  std::vector<u32> indices;
  u32 vertexCount = 0;
  u32 startVertex = 0;
  u32 minVertex = 0;
  u32 maxVertex = 0;
  i32 baseVertex = 0;
  bool rectList = false;
  u32 stream0OverrideVa = 0;
  u32 stream0OverrideStride = 0;
  u32 tessSegments = 0, tessFirstPatch = 0;  // patch grid (vertex shader gets patch, u, v)
};

bool ReadGuestIndices(u32 ib_va, u32 start_index, u32 count, std::vector<u32> &out) {
  if (!ib_va || !count)
    return false;
  const u32 common = mem::load<u32>(ib_va + obj::kCommon);
  const u32 base = mem::load<u32>(ib_va + obj::kBufferFetch0);
  const u32 size = mem::load<u32>(ib_va + obj::kBufferFetch1);
  const bool is32 = (common & obj::kIndexBuffer32BitBit) != 0;
  const u32 elem = is32 ? 4 : 2;
  if (!base)
    return false;
  if (size && (u64(start_index) + count) * elem > size) {
    u32 n;
    if (DiagShouldLog(0x6100, &n))
      EOT_WARN("[draw] index range {}+{} exceeds buffer {:#x} size {}", start_index, count, ib_va,
               size);
    count = static_cast<u32>(size / elem) > start_index ? size / elem - start_index : 0;
    if (!count)
      return false;
  }
  out.resize(count);
  if (is32) {
    auto *p = mem::at<be_u32>(base + start_index * 4);
    if (!p)
      return false;
    for (u32 i = 0; i < count; ++i)
      out[i] = p[i];
  } else {
    auto *p = mem::at<be_u16>(base + start_index * 2);
    if (!p)
      return false;
    for (u32 i = 0; i < count; ++i)
      out[i] = p[i];
  }
  return true;
}

void ExpandIndices(u32 prim, std::vector<u32> &idx, bool has_restart) {
  std::vector<u32> out;
  switch (prim) {
  case kPrimTriangleStrip: {
    out.reserve(idx.size() * 3);
    u32 run = 0;
    for (size_t i = 0; i < idx.size(); ++i) {
      if (has_restart && (idx[i] == 0xFFFF || idx[i] == 0xFFFFFFFFu)) {
        run = 0;
        continue;
      }
      ++run;
      if (run >= 3) {
        const u32 a = idx[i - 2], b = idx[i - 1], c = idx[i];
        if ((run & 1) == 1) {
          out.push_back(a);
          out.push_back(b);
          out.push_back(c);
        } else {
          out.push_back(b);
          out.push_back(a);
          out.push_back(c);
        }
      }
    }
    break;
  }
  case kPrimTriangleFan: {
    out.reserve(idx.size() * 3);
    for (size_t i = 2; i < idx.size(); ++i) {
      out.push_back(idx[0]);
      out.push_back(idx[i - 1]);
      out.push_back(idx[i]);
    }
    break;
  }
  case kPrimQuadList: {
    out.reserve(idx.size() / 4 * 6);
    for (size_t i = 0; i + 3 < idx.size(); i += 4) {
      out.push_back(idx[i]);
      out.push_back(idx[i + 1]);
      out.push_back(idx[i + 2]);
      out.push_back(idx[i]);
      out.push_back(idx[i + 2]);
      out.push_back(idx[i + 3]);
    }
    break;
  }
  case kPrimLineStrip: {
    out.reserve(idx.size() * 2);
    for (size_t i = 1; i < idx.size(); ++i) {
      if (has_restart && (idx[i] == 0xFFFF || idx[i - 1] == 0xFFFF))
        continue;
      out.push_back(idx[i - 1]);
      out.push_back(idx[i]);
    }
    break;
  }
  default:
    return;
  }
  idx.swap(out);
}

struct StreamInfo {
  u32 stream = 0;
  u32 stride = 0;
  const u8 *data = nullptr;
  u32 sizeBytes = 0;
  u32 dataVa = 0;
  u32 objectVa = 0;
};

bool ReadStream(DeviceView dev, u32 stream, StreamInfo &out, u32 fallback_stride = 0) {
  out.stream = stream;
  out.stride = dev.U8(dev::kStreamStride0 + stream) * 4u;
  if (!out.stride)
    out.stride = fallback_stride;
  const u32 slot = dev::StreamFetchSlotOffset(stream);
  const u32 d0 = dev.U32(slot);
  const u32 d1 = dev.U32(slot + 4);
  if ((d0 & 3) != 3 || !out.stride)
    return false;
  out.data = mem::phys<u8>(d0 & ~3u);
  out.sizeBytes = ((d1 >> 2) & 0xFFFFFF) * 4u;
  out.dataVa = d0 & ~3u;
  out.objectVa = dev.U32(dev::kStreamObject0 + 4 * stream);
  return out.data != nullptr;
}

// Uploads the memory of each fetch constant a vertex shader pulls from (raw
// guest bytes; the shader applies the fetch constant's endianness) and the
// table the shader finds them through (SharedConstants::vertexPullTable).
// Fable 2: pull sites (XenosRecomp 0x400 layout records, one per pulled
// vfetch): D3D binds the vfetch format, stride, offset and fetch constant into
// a copy of the microcode per declaration, so the shader cache (recompiled
// from unbound microcode) lacks them; the particle billboards read their
// per-particle data with format 0 (all zero) and collapsed onto their
// emitters. The binding comes from the live microcode when it is bound
// (Lionhead's shader-by-index path) or from the declaration by usage.
struct PullSiteEntry {
  u32 slot;  // fetch constant slot | 0x80000000 (valid)
  u32 strideDwords;
  i32 offsetDwords;
  u32 formatFlags;  // format | flags << 8 | exp_adjust << 16
};
static_assert(sizeof(PullSiteEntry) == 16);
constexpr u32 kPullSites = 16;

void BindPullSites(DeviceView dev, const GuestShader &vs, PullSiteEntry (&sites)[kPullSites]) {
  std::memset(sites, 0, sizeof(sites));
  if (!vs.entry)
    return;
  const u32 container = vs.va + obj::kVertexShaderContainer;
  const u32 shader_offset = mem::load<u32>(container + 0x18);
  const u32 physical_offset = mem::load<u32>(container + shader_offset);
  const u32 code_va = mem::load<u32>(vs.va + obj::kVertexShaderPhysical);
  const u8 *code = code_va ? mem::at<u8>(code_va + physical_offset) : nullptr;
  auto word = [&](u32 address, u32 i) { return mem::load<u32>(code_va + physical_offset + address * 12 + i * 4); };
  const u32 decl_va = dev.U32(dev::kVertexDeclaration);
  const u32 decl_count = decl_va ? mem::load<u32>(decl_va + obj::kDeclElementCount) : 0;
  const DeclElement *decl =
      decl_count && decl_count <= 32 ? mem::at<DeclElement>(decl_va + obj::kDeclElements) : nullptr;
  for (u32 i = 0; i < vs.entry->vertexLayoutCount; ++i) {
    const u32 w0 = EntryVertexLayouts(*vs.entry)[i * 2];
    const u32 w1 = EntryVertexLayouts(*vs.entry)[i * 2 + 1];
    if (!(w0 & 0x400))
      continue;
    const u32 site = (w0 >> 16) & 0xFF;
    if (site >= kPullSites)
      continue;
    u32 slot = 0, stride = 0, format = 0, flags = 0;
    i32 offset = 0, exp_adjust = 0;
    if (code) {
      const u32 address = w1 & 0xFFFF, parent = w1 >> 16;
      const u32 d0p = word(parent, 0), d1 = word(address, 1), d2 = word(address, 2), d2p = word(parent, 2);
      format = (d1 >> 16) & 0x3F;
      if (format) {
        slot = ((d0p >> 20) & 0x1F) * 3 + ((d0p >> 25) & 3);
        stride = d2p & 0xFF;
        offset = static_cast<i32>(d2 << 1) >> 9;
        flags = ((d1 >> 12) & 1) | (((d1 >> 13) & 1) << 1) | (((d1 >> 14) & 1) << 2);
        exp_adjust = static_cast<i32>(d1 << 2) >> 26;
      }
    }
    if (!format && decl && (w0 & 0xFF) != 0xFF) {
      for (u32 e = 0; e < decl_count; ++e) {
        const DeclElement &el = decl[e];
        if (el.stream == 0xFF)
          break;
        if (el.usage != (w0 & 0xF) || el.usageIndex != ((w0 >> 4) & 0xF))
          continue;
        const u32 stream = el.stream, type = el.type;
        if (stream >= 16)
          break;
        slot = 95 - stream;
        stride = dev.U8(dev::kStreamStride0 + stream);
        offset = static_cast<i32>(static_cast<u32>(el.offset) / 4);
        format = type & 0x3F;
        flags = ((type >> 8) & 1) | (((type >> 9) & 1) << 1);
        break;
      }
    }
    if (format && slot <= 95)
      sites[site] = {slot | 0x80000000u, stride, offset,
                     format | (flags << 8) | ((static_cast<u32>(exp_adjust) & 0xFF) << 16)};
  }
}

bool DrawMatchesList(const std::string &list, u64 hash, u32 count);

void NotePopCandidate(u64 key, u64 frame) {
  // A pop: an object drawn in at least 8 frames in a row, then missing for
  // 1-3 frames, then drawn again (objects the game draws every second or
  // third frame on purpose never build a streak).
  struct Seen {
    u64 last = 0;
    u32 streak = 0;
  };
  static std::unordered_map<u64, Seen> seen;
  static u64 pruned_at = 0;
  g_pop_draws.fetch_add(1, std::memory_order_relaxed);
  auto [it, inserted] = seen.try_emplace(key, Seen{frame, 1});
  if (!inserted) {
    Seen &e = it->second;
    const u64 gap = frame - e.last;
    if (gap == 1) {
      e.streak++;
    } else if (gap >= 2) {
      if (gap <= 4 && e.streak >= 8)
        g_pop_events.fetch_add(1, std::memory_order_relaxed);
      e.streak = 1;
    }
    e.last = frame;
  }
  if (frame - pruned_at > 600) {
    pruned_at = frame;
    for (auto e = seen.begin(); e != seen.end();)
      e = frame - e->second.last > 300 ? seen.erase(e) : std::next(e);
  }
}

u64 PulledBufferAddress(VideoState &s, const StreamInfo &st, u64 bytes, const u8 *src, bool preswap);
extern u64 pk_vs_hash_for_diag;

bool UploadVertexPullTable(DeviceView dev, const GuestShader &vs, const std::vector<u8> &pulled_slots,
                           u64 *table_va, u32 stream0_va = 0, u32 stream0_bytes = 0) {
  struct Entry {
    u64 address;
    u32 sizeDwords;
    u32 endian;
  };
  static_assert(sizeof(Entry) == 16);
  Entry table[96 + kPullSites] = {};
  PullSiteEntry sites[kPullSites];
  BindPullSites(dev, vs, sites);
  std::memcpy(&table[96], sites, sizeof(sites));
  std::vector<u8> slots = pulled_slots;
  for (const PullSiteEntry &site : sites)
    if ((site.slot & 0x80000000u) &&
        std::find(slots.begin(), slots.end(), static_cast<u8>(site.slot & 0x7F)) == slots.end())
      slots.push_back(static_cast<u8>(site.slot & 0x7F));
  constexpr u32 kMaxDwords = 4u << 20;  // 16 MB
  for (const u8 slot : slots) {
    if (slot >= 96)
      continue;
    if (slot == 95 && stream0_va) {  // BeginVertices: stream 0 is the draw's own data
      const u8 *src = stream0_bytes >= 4 ? mem::at<u8>(stream0_va) : nullptr;
      if (!src)
        continue;
      UploadAlloc data;
      if (!UploadBytes(src, stream0_bytes & ~3u, 16, &data))
        return false;
      table[slot] = {data.gpuVa, stream0_bytes / 4, 2};
      continue;
    }
    const u32 off = dev::kFetchConstants + 8u * slot;
    const u32 d0 = dev.U32(off), d1 = dev.U32(off + 4);
    if ((d0 & 3) != 3)
      continue;
    u32 dwords = (d1 >> 2) & 0xFFFFFF;
    if (dwords > kMaxDwords) {
      static std::atomic<u32> logged{0};
      if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
        EOT_WARN("[draw] vertex pulling: fetch slot {} buffer of {} dwords cut to {}", slot, dwords,
                 kMaxDwords);
      dwords = kMaxDwords;
    }
    const u8 *src = dwords ? mem::phys<u8>(d0 & ~3u) : nullptr;
    if (!src)
      continue;
    // A vertex stream's buffer (fetch slots 80..95): from the mirror cache, as
    // the streams' own buffers are. Other slots (bone palettes, ...) are
    // copied for every draw.
    pk_vs_hash_for_diag = vs.hash;
    if (slot >= 80 && REXCVAR_GET(fable2_pull_mirrors)) {
      StreamInfo st;
      st.stream = 95u - slot;
      st.stride = 16;
      st.data = src;
      st.sizeBytes = dwords * 4;
      st.dataVa = d0 & ~3u;
      st.objectVa = dev.U32(dev::kStreamObject0 + 4 * st.stream);
      // Never unlocked: the game writes it in place (64 KB stream-2 buffers
      // in Bowerstone changed 2 bytes at a time; the sampled key missed it).
      if (st.objectVa && ResourceUnlockSeq(st.objectVa) != 0) {
        const bool preswap = REXCVAR_GET(fable2_pull_preswap) && (d1 & 3) == 2 && !REXCVAR_GET(fable2_diag_pull);
        if (const u64 va = PulledBufferAddress(state(), st, u64(dwords) * 4, src, preswap)) {
          table[slot] = {va, dwords, (d1 & 3) | (preswap ? 0x10u : 0u)};
          state().perf.pull_mirrored_bytes += u64(dwords) * 4;
          continue;
        }
      }
    }
    UploadAlloc data;
    // 8-in-32 data is swapped here, while it is copied anyway: the vertex
    // shader then skips the per-dword swap (skinning reads ~48 dwords a vertex).
    const bool preswap = REXCVAR_GET(fable2_pull_preswap) && (d1 & 3) == 2;
    if (preswap) {
      if (!UploadAllocate(u64(dwords) * 4, 16, &data))
        return false;
      rex::memory::copy_and_swap_32_unaligned(data.cpu, reinterpret_cast<const u32 *>(src), dwords);
    } else if (!UploadBytes(src, u64(dwords) * 4, 16, &data)) {
      return false;
    }
    state().perf.pull_bytes += u64(dwords) * 4;
    if (REXCVAR_GET(fable2_diag_pull)) {
      // Repeats of the same buffer within a frame, and whether its bytes
      // changed since its previous copy (this frame or an earlier one).
      struct Seen {
        u64 hash = 0, frame = ~0ull;
        u32 usesThisFrame = 0;
      };
      static std::unordered_map<u64, Seen> seen;
      static u64 frame = ~0ull, bytes = 0, repeat_same = 0, repeat_changed = 0, new_frame_same = 0,
                 new_frame_changed = 0, fresh = 0, frames = 0;
      const u64 now = state().guest_frames;
      if (now != frame) {
        frame = now;
        if (++frames % 240 == 0) {
          EOT_INFO("[pull] per frame: {} KB; repeats in frame: {} KB same {} KB changed; first use in frame: "
                   "{} KB same as last frame, {} KB changed, {} KB new; {} buffers",
                   bytes / 240 / 1024, repeat_same / 240 / 1024, repeat_changed / 240 / 1024,
                   new_frame_same / 240 / 1024, new_frame_changed / 240 / 1024, fresh / 240 / 1024, seen.size());
          bytes = repeat_same = repeat_changed = new_frame_same = new_frame_changed = fresh = 0;
        }
      }
      const u64 n = u64(dwords) * 4;
      const u64 key = (u64(d0 & ~3u) << 32) | dwords;
      const u64 h = XXH3_64bits(src, n);
      bytes += n;
      auto [it, inserted] = seen.emplace(key, Seen{});
      if (inserted)
        fresh += n;
      else if (it->second.frame == now)
        (it->second.hash == h ? repeat_same : repeat_changed) += n;
      else
        (it->second.hash == h ? new_frame_same : new_frame_changed) += n;
      it->second.hash = h;
      it->second.frame = now;
    }
    table[slot] = {data.gpuVa, dwords, (d1 & 3) | (preswap ? 0x10u : 0u)};
    if (Settings::DiagFrame() > 0 && state().guest_frames + 1 == static_cast<u64>(Settings::DiagFrame())) {
      const u32 *w = reinterpret_cast<const u32 *>(src);
      EOT_INFO("[diag] pull slot {} va {:#x} dwords {} endian {} first {:08x} {:08x} {:08x} {:08x} "
               "{:08x} {:08x}",
               slot, d0 & ~3u, dwords, d1 & 3, w[0], dwords > 1 ? w[1] : 0, dwords > 2 ? w[2] : 0,
               dwords > 3 ? w[3] : 0, dwords > 4 ? w[4] : 0, dwords > 5 ? w[5] : 0);
    }
  }
  UploadAlloc alloc;
  if (!UploadBytes(table, sizeof(table), 16, &alloc))
    return false;
  state().perf.pull_bytes += sizeof(table);
  state().perf.pull_draws++;
  *table_va = alloc.gpuVa;
  return true;
}

std::vector<std::pair<u32, u32>> g_pending_texture_dumps;
// Temporary (dog coat): the textures a listed vertex shader's diag draw had
// bound, kept alive until the dump runs.
std::vector<std::pair<std::shared_ptr<GuestTexture>, std::string>> g_pending_bound_dumps;
struct PendingMipDump {
  std::shared_ptr<GuestTexture> texture;
  std::string path;
  u32 level;
  bool alpha;
};
std::vector<PendingMipDump> g_pending_mip_dumps;

void CopyVertexBytes(u8 *dst, const u8 *src, u32 bytes) {
  const u32 dwords = bytes / 4;
  rex::memory::copy_and_swap_32_unaligned(dst, src, dwords);
  if (bytes & 3)
    std::memcpy(dst + dwords * 4, src + dwords * 4, bytes & 3);
}

struct IndexCacheKey {
  u32 ib_va = 0, data_va = 0, start = 0, count = 0, prim = 0;
  u64 seq = 0, sample = 0;
  bool operator==(const IndexCacheKey &o) const {
    return ib_va == o.ib_va && data_va == o.data_va && start == o.start && count == o.count &&
           prim == o.prim && seq == o.seq && sample == o.sample;
  }
};
struct IndexCacheKeyHash {
  size_t operator()(const IndexCacheKey &k) const {
    u64 h = 0x9E3779B97F4A7C15ull;
    auto mix = [&](u64 v) {
      h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
      h *= 0xFF51AFD7ED558CCDull;
    };
    mix(k.ib_va);
    mix(k.data_va);
    mix((u64(k.start) << 32) | k.count);
    mix(k.prim);
    mix(k.seq);
    mix(k.sample);
    return static_cast<size_t>(h);
  }
};
struct IndexCacheChunk {
  std::unique_ptr<plume::RenderBuffer> buffer;
  std::unique_ptr<plume::RenderBuffer> vram;
  u64 vramValidUpTo = 0;
  u64 sealed = 0;
  u8 *cpu = nullptr;
  u64 capacity = 0, used = 0;
  u64 accounted = 0;
  u64 lastUseFrame = 0;
};
struct IndexCache {
  std::unordered_map<IndexCacheKey, CachedIndexRange, IndexCacheKeyHash> map;
};
IndexCache &index_cache() {
  static IndexCache c;
  return c;
}
constexpr u64 kIndexCacheChunkBytes = 8ull << 20;
constexpr u64 kIndexCacheBudgetBytes = 96ull << 20;

constexpr u64 kChunkGuardBytes = 1ull << 20;

u64 SampleHostBytes(const u8 *p, u32 bytes) {
  if (!p || bytes < 4)
    return 0;
  // Small ranges are hashed whole: the 16 + 16 sampled bytes missed rewrites
  // in the middle of small dynamic buffers (Fable 2's occlusion-query boxes,
  // 576 bytes, rewritten in place each frame), so the caches handed out last
  // frame's boxes and objects popped while the camera moved.
  if (bytes <= 8192)
    return XXH3_64bits(p, bytes) | 1;
  u64 h = 1469598103934665603ull;
  auto mix = [&](u32 off) {
    u32 v;
    std::memcpy(&v, p + off, 4);
    h ^= v;
    h *= 1099511628211ull;
  };
  const u32 span = bytes >= 16 ? bytes - 16 : 0;
  for (u32 k = 0; k < 2; ++k) {
    const u32 base = static_cast<u32>((u64(span) * k) & ~3ull);
    for (u32 i = 0; i < 16 && base + i + 4 <= bytes; i += 4)
      mix(base + i);
  }
  return h;
}
u64 SampleGuestBytes(u32 va, u32 bytes) { return SampleHostBytes(mem::at<u8>(va), bytes); }

void PrefetchSampleProbes(const u8 *p, u64 bytes) {
  if (!p || bytes < 4)
    return;
  eot::cpu::Prefetch(p);
  eot::cpu::Prefetch(p + 15);
  if (bytes >= 16) {
    const u64 span = (bytes - 16) & ~3ull;
    eot::cpu::Prefetch(p + span);
    eot::cpu::Prefetch(p + span + 15);
  }
}

}

void PrefetchIndexProbes(u32 device_va, u32 start_index, u32 index_count) {
  if (!device_va || !index_count)
    return;
  const u8 *dev = mem::at<u8>(device_va);
  if (!dev)
    return;
  const u32 ib_va = rex::memory::load_and_swap<u32>(dev + dev::kIndexBuffer);
  const u8 *ib = ib_va ? mem::at<u8>(ib_va) : nullptr;
  if (!ib)
    return;
  const u32 common = rex::memory::load_and_swap<u32>(ib + obj::kCommon);
  const u32 data_va = rex::memory::load_and_swap<u32>(ib + obj::kBufferFetch0);
  const u32 size = rex::memory::load_and_swap<u32>(ib + obj::kBufferFetch1);
  if (!data_va)
    return;
  const u32 elem = (common & obj::kIndexBuffer32BitBit) ? 4 : 2;
  const u64 start = u64(start_index) * elem;
  u64 bytes = u64(index_count) * elem;
  if (size && start + bytes > size)
    bytes = start < size ? size - start : 0;
  if (start > 0xFFFFFFFFu - data_va)
    return;
  PrefetchSampleProbes(mem::at<u8>(data_va + static_cast<u32>(start)), bytes);
}

u64 DrawSortKey(const DrawPacket &pk, u32 *depth_func) {
  if (pk.rectList || !pk.vs)
    return 0;
  DeviceView dev = Device(pk.device_va);
  dev.snapshot = pk.window.image;
  dev.snapshotSize = kDeviceSnapshotBytes;
  dev.pages = DeviceWindow::PageTable();
  const u32 dc = dev.U32(dev::kDepthControl);
  if (!(dc & 2))
    return 0;
  if (dc & 1) {
    auto constant_write = [](u32 func, u32 fail, u32 zpass, u32 zfail) {
      auto ok = [](u32 op) { return op == 0 || op == 1 || op == 2; };
      return func == 7 && ok(fail) && ok(zpass) && ok(zfail);
    };
    if (!constant_write((dc >> 8) & 7, (dc >> 11) & 7, (dc >> 14) & 7, (dc >> 17) & 7))
      return 0;
    if ((dc & 0x80) &&
        !constant_write((dc >> 20) & 7, (dc >> 23) & 7, (dc >> 26) & 7, (dc >> 29) & 7))
      return 0;
  }
  const u32 zfunc = (dc >> 4) & 7;
  const bool ordered = zfunc == 1 || zfunc == 3 || zfunc == 4 || zfunc == 6;
  if (!(dc & 4) || !ordered)
    return 0;
  if (depth_func)
    *depth_func = zfunc;
  const u32 cc = dev.U32(dev::kColorControl), bc = dev.U32(dev::kBlendControl0);
  const u32 src = bc & 0x1F, op = (bc >> 5) & 7, dst = (bc >> 8) & 0x1F;
  const bool passthrough = src == 1 && dst == 0 && op == 0;
  if (!(cc & (1u << 5)) && !passthrough)
    return 0;
  u64 h = 0x9E3779B97F4A7C15ull;
  auto mix = [&](u64 v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xFF51AFD7ED558CCDull;
  };
  mix(reinterpret_cast<u64>(pk.vs));
  mix(reinterpret_cast<u64>(pk.ps));
  mix(reinterpret_cast<u64>(pk.layout));
  mix(static_cast<u64>(pk.topology));
  for (u32 S = 0; S <= pk.max_slot && S < 16; ++S)
    mix(pk.strides[S]);
  constexpr u32 kRegs[] = {dev::kModeControl,   dev::kDepthControl, dev::kBlendControl0,
                           dev::kBlendControl1, dev::kBlendControl2, dev::kBlendControl3,
                           dev::kColorControl,  dev::kColorMask,     dev::kAlphaRef};
  for (u32 off : kRegs)
    mix(dev.U32(off));
  mix(dev.U32(dev::kStencilRefMask));
  return h ? h : 1;
}

namespace {

struct BufferPool {
  std::mutex mutex;
  std::vector<IndexCacheChunk> chunks;
  u64 totalBytes = 0;
  std::vector<std::unique_ptr<plume::RenderBuffer>> retired;
  std::atomic<bool> hasRetired{false};
  std::atomic<u64> flushEpoch{1};
  // A chunk made ahead on a background thread: made on the game thread, a new
  // 16 MB chunk (upload + VRAM buffers, Metal residency) stalled it 20-50 ms
  // walking into new areas. Guarded by mutex.
  IndexCacheChunk spare;
  u64 spareSize = 0;
  bool spareReady = false, sparePending = false;
};
void PoolDrainRetired(VideoState &s, BufferPool &pool) {
  if (!pool.hasRetired.load(std::memory_order_acquire))
    return;
  std::vector<std::unique_ptr<plume::RenderBuffer>> retired;
  {
    std::lock_guard lock(pool.mutex);
    pool.hasRetired.store(false, std::memory_order_relaxed);
    if (pool.retired.empty())
      return;
    retired.swap(pool.retired);
  }
  for (auto &b : retired) {
    if (!b)
      continue;
    if (RenderThreadActive()) {
      RenderThreadRetire(b.release());
    } else {
      std::lock_guard video_lock(s.mutex);
      ParkBuffer(s, std::move(b));
    }
  }
}
bool MakePoolChunk(VideoState &s, u64 size, plume::RenderBufferFlags flags, const char *name,
                   IndexCacheChunk &ch) {
  // A guard tail that is never handed out: vertex fetches past the end of a
  // draw's range stay inside the buffer (Metal loses the device otherwise).
  plume::RenderBufferDesc desc = plume::RenderBufferDesc::UploadBuffer(size + kChunkGuardBytes);
  desc.flags = flags;
  ch.buffer = CreateHostBuffer(s.device.get(), desc, name);
  if (!ch.buffer)
    return false;
  ch.cpu = static_cast<u8 *>(ch.buffer->map());
  if (!ch.cpu) {
    ch.buffer.reset();
    return false;
  }
  ch.capacity = size;
  plume::RenderBufferDesc vram_desc = plume::RenderBufferDesc::DefaultBuffer(size + kChunkGuardBytes, flags);
  const std::string vram_name = std::string(name) + "-vram";
  ch.vram = CreateHostBuffer(s.device.get(), vram_desc, vram_name.c_str());
  return true;
}

bool PoolAllocate(VideoState &s, BufferPool &pool, u64 budget, u64 chunk_bytes,
                  plume::RenderBufferFlags flags, const char *name, u64 bytes, u64 align,
                  plume::RenderBuffer **buffer, u64 *offset, u8 **cpu, bool *reset) {
  *reset = false;
  if (pool.totalBytes + bytes > budget) {
    for (auto &ch : pool.chunks) {
      pool.retired.push_back(std::move(ch.buffer));
      if (ch.vram)
        pool.retired.push_back(std::move(ch.vram));
    }
    pool.hasRetired.store(true, std::memory_order_release);
    pool.chunks.clear();
    pool.totalBytes = 0;
    *reset = true;
    EOT_DEBUG("[draw] {} pool over budget ({} MB); rebuilt", name, budget >> 20);
  }
  auto align_up = [align](u64 v) { return (v + align - 1) / align * align; };
  if (pool.chunks.empty() ||
      align_up(pool.chunks.back().used) + bytes > pool.chunks.back().capacity) {
    IndexCacheChunk ch;
    const u64 size = std::max(chunk_bytes, bytes + align);
    if (pool.spareReady && pool.spareSize == size) {
      ch = std::move(pool.spare);
      pool.spare = IndexCacheChunk{};
      pool.spareReady = false;
    } else if (!MakePoolChunk(s, size, flags, name, ch)) {
      return false;
    }
    pool.chunks.push_back(std::move(ch));
    // Make the next chunk ahead, off this thread, while there is budget left.
    if (REXCVAR_GET(fable2_pool_spare) && !pool.spareReady && !pool.sparePending &&
        pool.totalBytes + 2 * chunk_bytes <= budget) {
      pool.sparePending = true;
      const std::string tag = name;
      std::thread([&s, &pool, chunk_bytes, flags, tag] {
        IndexCacheChunk spare;
        const bool ok = MakePoolChunk(s, chunk_bytes, flags, tag.c_str(), spare);
        std::lock_guard lock(pool.mutex);
        pool.sparePending = false;
        if (ok) {
          pool.spare = std::move(spare);
          pool.spareSize = chunk_bytes;
          pool.spareReady = true;
        }
      }).detach();
    }
  }
  auto &ch = pool.chunks.back();
  const u64 off = align_up(ch.used);
  *buffer = ch.buffer.get();
  *offset = off;
  *cpu = ch.cpu + off;
  ch.used = off + bytes;
  ch.accounted += bytes;
  pool.totalBytes += bytes;
  return true;
}

BufferPool &index_pool() {
  static BufferPool p;
  return p;
}

void PoolSeal(BufferPool &pool, plume::RenderBuffer *buffer, u64 end) {
  std::lock_guard lock(pool.mutex);
  for (auto &ch : pool.chunks) {
    if (ch.buffer.get() != buffer)
      continue;
    if (end > ch.sealed)
      ch.sealed = end;
    return;
  }
}

plume::RenderBuffer *PoolResidentBuffer(VideoState &s, BufferPool &pool,
                                        plume::RenderBuffer *upload, u64 offset, u64 bytes,
                                        u64 *capacity = nullptr) {
  std::lock_guard lock(pool.mutex);
  for (const auto &ch : pool.chunks) {
    if (ch.buffer.get() != upload)
      continue;
    if (capacity)
      *capacity = ch.capacity;
    if (ch.vram && offset + bytes <= ch.vramValidUpTo) {
      s.perf.geometry_vram_binds++;
      return ch.vram.get();
    }
    break;
  }
  s.perf.geometry_staging_binds++;
  return upload;
}

void PoolFlushToVram(VideoState &s, BufferPool &pool, plume::RenderBufferFlags flags) {
  auto *cmd = s.command_list;
  std::lock_guard lock(pool.mutex);
  for (auto &ch : pool.chunks) {
    if (!ch.vram || ch.sealed <= ch.vramValidUpTo)
      continue;
    const u64 from = ch.vramValidUpTo, bytes = ch.sealed - from;
    const plume::RenderBufferBarrier to_copy(ch.vram.get(), plume::RenderBufferAccess::WRITE);
    cmd->barriers(plume::RenderBarrierStage::COPY, &to_copy, 1, nullptr, 0);
    cmd->copyBufferRegion(plume::RenderBufferReference(ch.vram.get(), from),
                          plume::RenderBufferReference(ch.buffer.get(), from), bytes);
    const plume::RenderBufferBarrier to_read(ch.vram.get(), plume::RenderBufferAccess::READ);
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS, &to_read, 1, nullptr, 0);
    (void)flags;
    ch.vramValidUpTo = ch.sealed;
    pool.flushEpoch.fetch_add(1, std::memory_order_release);
  }
}
bool IndexCacheAllocate(VideoState &s, u64 bytes, plume::RenderBuffer **buffer, u64 *offset,
                        u8 **cpu) {
  auto &pool = index_pool();
  auto &cache = index_cache();
  if (bytes > kIndexCacheBudgetBytes)
    return false;
  std::lock_guard lock(pool.mutex);

  auto capacity = [&] {
    u64 c = 0;
    for (const auto &ch : pool.chunks)
      c += ch.capacity;
    return c;
  };
  auto needs_chunk = [&] {
    return pool.chunks.empty() || ((pool.chunks.back().used + 3) & ~3ull) + bytes > pool.chunks.back().capacity;
  };
  auto over_budget = [&] {
    return pool.totalBytes + bytes > kIndexCacheBudgetBytes ||
           (needs_chunk() && capacity() + std::max(kIndexCacheChunkBytes, bytes) > kIndexCacheBudgetBytes);
  };
  bool recycled_one = false;
  while (over_budget() && !pool.chunks.empty()) {
    auto victim = std::min_element(
        pool.chunks.begin(), pool.chunks.end(),
        [](const IndexCacheChunk &a, const IndexCacheChunk &b) {
          return a.lastUseFrame < b.lastUseFrame;
        });
    plume::RenderBuffer *dead = victim->buffer.get();
    u32 erased = 0;
    for (auto it = cache.map.begin(); it != cache.map.end();) {
      if (it->second.buffer != dead) {
        ++it;
        continue;
      }
      it = cache.map.erase(it);
      ++erased;
    }
    const u64 released = victim->accounted;
    const bool gpu_idle = victim->lastUseFrame + kNumFrames < s.guest_frames;
    IndexCacheChunk recycled;
    if (gpu_idle && bytes <= victim->capacity && !recycled_one) {
      recycled_one = true;
      recycled = std::move(*victim);
      recycled.used = 0;
      recycled.sealed = 0;
      recycled.accounted = 0;
      recycled.vramValidUpTo = 0;
      recycled.lastUseFrame = s.guest_frames;
    } else {
      pool.retired.push_back(std::move(victim->buffer));
      if (victim->vram)
        pool.retired.push_back(std::move(victim->vram));
      pool.hasRetired.store(true, std::memory_order_release);
    }
    pool.chunks.erase(victim);
    pool.totalBytes -= std::min(pool.totalBytes, released);
    if (recycled.buffer)
      pool.chunks.push_back(std::move(recycled));
    s.perf.index_cache_evictions++;
    u32 n;
    if (DiagShouldLog(0x5A11, &n))
      EOT_DEBUG("[draw] index-cache recycled one chunk ({} ranges, {} MB resident)", erased,
                pool.totalBytes >> 20);
  }
  bool reset = false;
  const bool ok = PoolAllocate(s, pool, kIndexCacheBudgetBytes, kIndexCacheChunkBytes,
                               plume::RenderBufferFlag::INDEX, "index-cache", bytes, 4, buffer,
                               offset, cpu, &reset);
  if (reset)
    cache.map.clear();
  if (ok) {
    for (auto &chunk : pool.chunks) {
      if (chunk.buffer.get() == *buffer) {
        chunk.lastUseFrame = s.guest_frames;
        break;
      }
    }
  }
  return ok;
}

void TouchIndexCacheBuffer(plume::RenderBuffer *buffer, u64 frame) {
  auto &pool = index_pool();
  std::lock_guard lock(pool.mutex);
  for (auto &chunk : pool.chunks) {
    if (chunk.buffer.get() == buffer) {
      chunk.lastUseFrame = frame;
      return;
    }
  }
}

struct VertexMirrorKey {
  u32 data_va = 0, size = 0;
  u64 seq = 0, sample = 0;
  bool operator==(const VertexMirrorKey &o) const {
    return data_va == o.data_va && size == o.size && seq == o.seq && sample == o.sample;
  }
};
struct VertexMirrorKeyHash {
  size_t operator()(const VertexMirrorKey &k) const {
    u64 h = 0x9E3779B97F4A7C15ull;
    auto mix = [&](u64 v) {
      h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
      h *= 0xFF51AFD7ED558CCDull;
    };
    mix((u64(k.data_va) << 32) | k.size);
    mix(k.seq);
    mix(k.sample);
    return static_cast<size_t>(h);
  }
};
struct VertexMirror {
  plume::RenderBuffer *buffer = nullptr;
  u64 offset = 0;
  u32 size = 0;
  u64 lastUseFrame = 0;
  u8 *cpu = nullptr;
  mutable u64 residentVa = 0;  // device address of resident (vertex pulling)
  mutable plume::RenderBuffer *resident = nullptr;
  mutable u64 residentCapacity = 0;
  mutable u64 residentEpoch = 0;
};
struct VertexMirrorCache {
  std::unordered_map<VertexMirrorKey, VertexMirror, VertexMirrorKeyHash> map;
  std::unordered_map<VertexMirrorKey, u64, VertexMirrorKeyHash> seen;
  BufferPool pool;
  bool admissionClosed = false;
};
VertexMirrorCache &vertex_mirrors() {
  static VertexMirrorCache c;
  return c;
}
constexpr u64 kVertexMirrorChunkBytes = 16ull << 20;
constexpr u64 kVertexMirrorBudgetBytes = 256ull << 20;
constexpr u32 kVertexMirrorMaxBytes = 32u << 20;
constexpr size_t kVertexMirrorSeenCap = 4096;

// Vertex pulling's whole-buffer mirrors: their own pool (device-addressable,
// and they must not fill the stream mirrors' budget).
VertexMirrorCache &pull_mirrors() {
  static VertexMirrorCache c;
  return c;
}

const VertexMirror *GetMirror(VideoState &s, VertexMirrorCache &c, plume::RenderBufferFlags flags,
                              const char *name, const StreamInfo &st, u64 first, u64 bytes, bool raw = false,
                              u64 salt = 0) {
  if (!REXCVAR_GET(fable2_geometry_caches))
    return nullptr;
  if (!st.dataVa || !bytes || bytes > kVertexMirrorMaxBytes || !st.data ||
      first > UINT32_MAX - st.dataVa)
    return nullptr;
  VertexMirrorKey key;
  key.data_va = st.dataVa + static_cast<u32>(first);
  key.size = static_cast<u32>(bytes);
  key.seq = st.objectVa ? ResourceUnlockSeq(st.objectVa) : 0;
  key.sample = SampleHostBytes(st.data + first, key.size) ^ salt;
  auto it = c.map.find(key);
  if (it != c.map.end()) {
    it->second.lastUseFrame = s.guest_frames;
    s.perf.vertex_cache_hits++;
    return &it->second;
  }
  if (c.admissionClosed)
    return nullptr;
  auto seen = c.seen.find(key);
  if (seen == c.seen.end()) {
    if (c.seen.size() >= kVertexMirrorSeenCap)
      c.seen.clear();
    c.seen.emplace(key, s.guest_frames);
    return nullptr;
  }
  if (seen->second >= s.guest_frames)
    return nullptr;
  c.seen.erase(seen);
  if (c.pool.totalBytes > kVertexMirrorBudgetBytes ||
      bytes > kVertexMirrorBudgetBytes - c.pool.totalBytes) {
    c.admissionClosed = true;
    EOT_DEBUG("[draw] vertex-mirror cache full ({} MB, {} ranges); keeping residents and "
              "using frame uploads for new ranges",
              c.pool.totalBytes >> 20, c.map.size());
    return nullptr;
  }
  s.perf.vertex_cache_misses++;
  plume::RenderBuffer *buffer = nullptr;
  u64 offset = 0;
  u8 *cpu = nullptr;
  bool reset = false;
  bool ok = false;
  {
    std::lock_guard lock(c.pool.mutex);
    ok = PoolAllocate(s, c.pool, kVertexMirrorBudgetBytes, kVertexMirrorChunkBytes, flags, name, bytes,
                      std::max<u64>(4, st.stride), &buffer, &offset, &cpu, &reset);
  }
  if (reset)
    c.map.clear();
  if (!ok)
    return nullptr;
  {
    PerfScope copy_scope(s.perf.vertex_copy_ms);
    // Vertex pulling reads raw guest bytes (the shader applies the fetch
    // constant's endianness); bound streams are swapped here.
    if (raw)
      std::memcpy(cpu, st.data + first, static_cast<size_t>(bytes));
    else
      CopyVertexBytes(cpu, st.data + first, static_cast<u32>(bytes));
  }
  PoolSeal(c.pool, buffer, offset + bytes);
  s.perf.vertex_bytes += bytes;
  VertexMirror m;
  m.buffer = buffer;
  m.offset = offset;
  m.size = static_cast<u32>(bytes);
  m.lastUseFrame = s.guest_frames;
  m.cpu = cpu;
  return &c.map.emplace(key, m).first->second;
}

const VertexMirror *GetVertexMirror(VideoState &s, const StreamInfo &st, u64 first, u64 bytes) {
  return GetMirror(s, vertex_mirrors(), plume::RenderBufferFlag::VERTEX, "vertex-mirror", st, first, bytes);
}

// A pulled fetch constant's buffer from the mirror cache (the same keys as the
// vertex streams': unlock count of its stream object and sampled bytes):
// its device address, or 0 to copy it for this draw.
u64 pk_vs_hash_for_diag = 0;
u64 PulledBufferAddress(VideoState &s, const StreamInfo &st, u64 bytes, const u8 *src, bool preswap) {
  auto &c = pull_mirrors();
  // preswap: the mirror holds the words 8-in-32 swapped (CopyVertexBytes).
  const VertexMirror *m = GetMirror(s, c, plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::CONSTANT,
                                    "pull-mirror", st, 0, bytes, !preswap, preswap ? 0x5A5A5A5A00000000ull : 0);
  if (!m)
    return 0;
  if (REXCVAR_GET(fable2_diag_pull) && m->cpu) {
    static u64 checked = 0, stale = 0, stale_bytes = 0;
    ++checked;
    if (std::memcmp(m->cpu, src, bytes) != 0) {
      ++stale;
      stale_bytes += bytes;
      static std::unordered_map<u32, u32> logged;
      if (logged.size() < 24 && logged[st.dataVa]++ == 0) {
        u32 diff = 0, first = ~0u, last = 0;
        for (u64 i = 0; i < bytes; ++i)
          if (m->cpu[i] != src[i]) {
            ++diff;
            first = std::min<u32>(first, u32(i));
            last = u32(i);
          }
        EOT_INFO("[pull] stale: va {:#x} bytes {} object {:#x} stream {} seq {} differing {} in {}..{} vs {}",
                 st.dataVa, bytes, st.objectVa, st.stream, ResourceUnlockSeq(st.objectVa), diff, first, last,
                 pk_vs_hash_for_diag);
      }
    }
    if (checked % 20000 == 0)
      EOT_INFO("[pull] mirrors: {} uses checked, {} stale ({} KB)", checked, stale, stale_bytes / 1024);
  }
  const u64 epoch = c.pool.flushEpoch.load(std::memory_order_acquire);
  if (m->residentEpoch != epoch || !m->resident) {
    m->resident = PoolResidentBuffer(s, c.pool, m->buffer, m->offset, bytes, &m->residentCapacity);
    m->residentEpoch = epoch;
    m->residentVa = m->resident ? m->resident->getDeviceAddress() : 0;
  }
  return m->residentVa ? m->residentVa + m->offset : 0;
}

void NoteMorphStream(VideoState &s, const StreamInfo &st, u64 first, u64 bytes) {
  struct Seen {
    u64 sample = 0, full = 0;
  };
  static std::unordered_map<u64, Seen> seen;
  static u64 frames = 0, last_frame = ~0ull, draws = 0, changed = 0, stale = 0;
  if (!st.data || !bytes || bytes > kVertexMirrorMaxBytes)
    return;
  const u64 key = (u64(st.dataVa + static_cast<u32>(first)) << 32) ^ bytes;
  const u64 sample = SampleHostBytes(st.data + first, static_cast<u32>(bytes));
  const u64 full = XXH3_64bits(st.data + first, static_cast<size_t>(bytes));
  auto [it, inserted] = seen.emplace(key, Seen{sample, full});
  if (!inserted) {
    if (it->second.full != full) {
      changed++;
      if (it->second.sample == sample)
        stale++;
    }
    it->second = Seen{sample, full};
  }
  draws++;
  if (last_frame != s.guest_frames) {
    last_frame = s.guest_frames;
    if (++frames % 600 == 0) {
      EOT_DEBUG("[morph] {} draws over {} frames: {} ranges, {} rewrites, {} of them under an "
                "unchanged fingerprint",
                draws, frames, seen.size(), changed, stale);
      if (seen.size() > 4096)
        seen.clear();
    }
  }
}

const CachedIndexRange *GetCachedIndexRange(VideoState &s, u32 ib_va, u32 prim, u32 start_index,
                                            u32 count) {
  if (!REXCVAR_GET(fable2_geometry_caches))
    return nullptr;
  if (!ib_va || !count || prim == kPrimRectList)
    return nullptr;
  const u8 *ib = mem::at<u8>(ib_va);
  if (!ib)
    return nullptr;
  const u32 common = rex::memory::load_and_swap<u32>(ib + obj::kCommon);
  const u32 data_va = rex::memory::load_and_swap<u32>(ib + obj::kBufferFetch0);
  const u32 size = rex::memory::load_and_swap<u32>(ib + obj::kBufferFetch1);
  if (!data_va)
    return nullptr;
  const bool is32 = (common & obj::kIndexBuffer32BitBit) != 0;
  const u32 elem = is32 ? 4 : 2;
  const u64 range_start = u64(start_index) * elem;
  u64 range_bytes = u64(count) * elem;
  if (size && range_start + range_bytes > size)
    range_bytes = range_start < size ? size - range_start : 0;
  if (!range_bytes)
    return nullptr;
  IndexCacheKey key;
  key.ib_va = ib_va;
  key.data_va = data_va;
  key.start = start_index;
  key.count = count;
  key.prim = prim;
  key.seq = ResourceUnlockSeq(ib_va);
  key.sample = SampleGuestBytes(data_va + static_cast<u32>(range_start), static_cast<u32>(range_bytes));

  auto &c = index_cache();
  auto it = c.map.find(key);
  if (it != c.map.end()) {
    if (it->second.lastUseFrame != s.guest_frames) {
      it->second.lastUseFrame = s.guest_frames;
      TouchIndexCacheBuffer(it->second.buffer, s.guest_frames);
    }
    s.perf.index_cache_hits++;
    return &it->second;
  }
  s.perf.index_cache_misses++;

  std::vector<u32> idx;
  if (!ReadGuestIndices(ib_va, start_index, count, idx))
    return nullptr;
  bool expand = false;
  plume::RenderPrimitiveTopology topology = ConvertPrimitiveType(prim, &expand);
  if (expand || prim == kPrimTriangleStrip || prim == kPrimLineStrip) {
    ExpandIndices(prim, idx, true);
    topology = prim == kPrimLineStrip ? plume::RenderPrimitiveTopology::LINE_LIST
                                      : plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
  if (idx.empty())
    return nullptr;
  u32 lo = 0xFFFFFFFFu, hi = 0;
  for (u32 v : idx) {
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  const bool host32 = hi > 0xFFFEu;
  const u64 bytes = idx.size() * (host32 ? 4 : 2);
  plume::RenderBuffer *buffer = nullptr;
  u64 offset = 0;
  u8 *cpu = nullptr;
  const bool allocated = s.ready && IndexCacheAllocate(s, bytes, &buffer, &offset, &cpu);
  PoolDrainRetired(s, index_pool());
  if (!allocated)
    return nullptr;
  if (host32) {
    std::memcpy(cpu, idx.data(), bytes);
  } else {
    auto *dst = reinterpret_cast<u16 *>(cpu);
    for (size_t i = 0; i < idx.size(); ++i)
      dst[i] = static_cast<u16>(idx[i]);
  }
  PoolSeal(index_pool(), buffer, offset + bytes);
  CachedIndexRange r;
  r.buffer = buffer;
  r.offset = offset;
  r.count = static_cast<u32>(idx.size());
  r.lo = lo;
  r.hi = hi;
  r.is32 = host32;
  r.topology = topology;
  r.lastUseFrame = s.guest_frames;
  return &(c.map[key] = r);
}

// The landscape's tessellated patch grid: the same indices for every draw with
// this segment and patch count, built once into the index cache (rebuilt and
// uploaded per draw, ~7 MB per frame on the game thread in Bowerstone).
const CachedIndexRange *GetCachedGridIndices(VideoState &s, u32 segments, u32 patch_count) {
  if (!REXCVAR_GET(fable2_geometry_caches) || !segments || !patch_count)
    return nullptr;
  IndexCacheKey key;
  key.data_va = 0xFFFFFFFFu;  // no guest buffer: a synthetic grid
  key.start = segments;
  key.count = patch_count;
  key.prim = kPrimTriangleList;
  key.sample = 0x6E1D6E1D6E1D6E1Dull;
  auto &c = index_cache();
  auto it = c.map.find(key);
  if (it != c.map.end()) {
    if (it->second.lastUseFrame != s.guest_frames) {
      it->second.lastUseFrame = s.guest_frames;
      TouchIndexCacheBuffer(it->second.buffer, s.guest_frames);
    }
    s.perf.index_cache_hits++;
    return &it->second;
  }
  s.perf.index_cache_misses++;
  const u32 row = segments + 1;
  const u64 count = u64(patch_count) * segments * segments * 6;
  const u32 hi = patch_count * row * row - 1;
  const bool host32 = hi > 0xFFFEu;
  const u64 bytes = count * (host32 ? 4 : 2);
  plume::RenderBuffer *buffer = nullptr;
  u64 offset = 0;
  u8 *cpu = nullptr;
  const bool allocated = s.ready && IndexCacheAllocate(s, bytes, &buffer, &offset, &cpu);
  PoolDrainRetired(s, index_pool());
  if (!allocated)
    return nullptr;
  auto emit = [&](auto *dst) {
    for (u32 p = 0; p < patch_count; ++p) {
      const u32 base = p * row * row;
      for (u32 j = 0; j < segments; ++j)
        for (u32 i = 0; i < segments; ++i) {
          const u32 v00 = base + j * row + i, v10 = v00 + 1, v01 = v00 + row, v11 = v01 + 1;
          using T = std::remove_reference_t<decltype(*dst)>;
          *dst++ = T(v00);
          *dst++ = T(v10);
          *dst++ = T(v11);
          *dst++ = T(v00);
          *dst++ = T(v11);
          *dst++ = T(v01);
        }
    }
  };
  if (host32)
    emit(reinterpret_cast<u32 *>(cpu));
  else
    emit(reinterpret_cast<u16 *>(cpu));
  PoolSeal(index_pool(), buffer, offset + bytes);
  CachedIndexRange r;
  r.buffer = buffer;
  r.offset = offset;
  r.count = static_cast<u32>(count);
  r.lo = 0;
  r.hi = hi;
  r.is32 = host32;
  r.topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  r.lastUseFrame = s.guest_frames;
  return &(c.map[key] = r);
}

struct RectExpansion {
  std::vector<u8> vertices[16];
  std::vector<u32> indices;
  u32 hostVertexCount = 0;
};

float LaneToFloat(const u8 *p, plume::RenderFormat f, u32 lane, bool *ok) {
  using F = plume::RenderFormat;
  *ok = true;
  switch (f) {
  case F::R32_FLOAT:
  case F::R32G32_FLOAT:
  case F::R32G32B32_FLOAT:
  case F::R32G32B32A32_FLOAT: {
    float v;
    std::memcpy(&v, p + lane * 4, 4);
    return v;
  }
  case F::R16G16_FLOAT:
  case F::R16G16B16A16_FLOAT:
  case F::R16_FLOAT: {
    u16 h;
    std::memcpy(&h, p + lane * 2, 2);
    const u32 sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    float v;
    if (exp == 0)
      v = std::ldexp(static_cast<float>(man), -24);
    else if (exp == 31)
      v = man ? NAN : INFINITY;
    else
      v = std::ldexp(static_cast<float>(man | 0x400), static_cast<int>(exp) - 25);
    return sign ? -v : v;
  }
  default:
    *ok = false;
    return 0.0f;
  }
}

void SynthesizeLane(u8 *dst, const u8 *vi, const u8 *vj, const u8 *vk, plume::RenderFormat f,
                    u32 bytes) {
  using F = plume::RenderFormat;
  auto lanes_of = [](F fmt, u32 *lane_bytes) -> u32 {
    switch (fmt) {
    case F::R32_FLOAT:
      *lane_bytes = 4;
      return 1;
    case F::R32G32_FLOAT:
      *lane_bytes = 4;
      return 2;
    case F::R32G32B32_FLOAT:
      *lane_bytes = 4;
      return 3;
    case F::R32G32B32A32_FLOAT:
      *lane_bytes = 4;
      return 4;
    case F::R16_FLOAT:
    case F::R16_UNORM:
    case F::R16_SNORM:
      *lane_bytes = 2;
      return 1;
    case F::R16G16_FLOAT:
    case F::R16G16_UNORM:
    case F::R16G16_SNORM:
    case F::R16G16_UINT:
      *lane_bytes = 2;
      return 2;
    case F::R16G16B16A16_FLOAT:
    case F::R16G16B16A16_UNORM:
    case F::R16G16B16A16_SNORM:
    case F::R16G16B16A16_UINT:
      *lane_bytes = 2;
      return 4;
    case F::R8G8B8A8_UNORM:
    case F::B8G8R8A8_UNORM:
    case F::R8G8B8A8_SNORM:
    case F::R8G8B8A8_UINT:
      *lane_bytes = 1;
      return 4;
    case F::R8G8_UNORM:
    case F::R8G8_SNORM:
    case F::R8G8_UINT:
      *lane_bytes = 1;
      return 2;
    case F::R8_UNORM:
    case F::R8_SNORM:
    case F::R8_UINT:
      *lane_bytes = 1;
      return 1;
    default:
      *lane_bytes = 0;
      return 0;
    }
  };
  u32 lane_bytes = 0;
  const u32 lanes = lanes_of(f, &lane_bytes);
  if (!lanes) {
    std::memcpy(dst, vi, bytes);
    return;
  }
  for (u32 l = 0; l < lanes; ++l) {
    bool ok = false;
    if (lane_bytes == 4 || (lane_bytes == 2 && (f == F::R16_FLOAT || f == F::R16G16_FLOAT ||
                                                 f == F::R16G16B16A16_FLOAT))) {
      const float a = LaneToFloat(vi, f, l, &ok), b = LaneToFloat(vj, f, l, &ok),
                  c = LaneToFloat(vk, f, l, &ok);
      const float r = a + b - c;
      if (lane_bytes == 4) {
        std::memcpy(dst + l * 4, &r, 4);
      } else {
        std::memcpy(dst + l * 2, vi + l * 2, 2);
      }
    } else if (lane_bytes == 2) {
      u16 a, b, c;
      std::memcpy(&a, vi + l * 2, 2);
      std::memcpy(&b, vj + l * 2, 2);
      std::memcpy(&c, vk + l * 2, 2);
      const i32 r = std::clamp(static_cast<i32>(a) + b - c, 0, 65535);
      const u16 rv = static_cast<u16>(r);
      std::memcpy(dst + l * 2, &rv, 2);
    } else {
      const i32 r = std::clamp(static_cast<i32>(vi[l]) + vj[l] - vk[l], 0, 255);
      dst[l] = static_cast<u8>(r);
    }
  }
}

bool ExpandRectList(const InputLayout &layout, const StreamInfo streams[16], u32 stream_mask,
                    const std::vector<u32> &guest_vertices, RectExpansion &out) {
  const plume::RenderInputElement *pos = nullptr;
  for (const auto &e : layout.elements) {
    if (std::strcmp(e.semanticName, "POSITION") == 0 && e.semanticIndex == 0 &&
        e.slotIndex != kSyntheticVertexSlot) {
      pos = &e;
      break;
    }
  }
  const u32 rects = static_cast<u32>(guest_vertices.size() / 3);
  out.hostVertexCount = rects * 4;
  out.indices.reserve(rects * 6);
  for (u32 S = 0; S < 16; ++S) {
    if (stream_mask & (1u << S))
      out.vertices[S].resize(size_t(out.hostVertexCount) * streams[S].stride);
  }
  for (u32 r = 0; r < rects; ++r) {
    const u32 g[3] = {guest_vertices[r * 3], guest_vertices[r * 3 + 1],
                      guest_vertices[r * 3 + 2]};
    u32 k = 0;
    if (pos) {
      const StreamInfo &ps = streams[pos->slotIndex];
      float px[3], py[3];
      bool ok = true;
      for (u32 n = 0; n < 3 && ok; ++n) {
        const u8 *v = ps.data + u64(g[n]) * ps.stride + pos->alignedByteOffset;
        if ((u64(g[n]) + 1) * ps.stride > ps.sizeBytes && ps.sizeBytes) {
          ok = false;
          break;
        }
        u8 tmp[16];
        CopyVertexBytes(tmp, v, 16);
        px[n] = LaneToFloat(tmp, pos->format, 0, &ok);
        py[n] = LaneToFloat(tmp, pos->format, 1, &ok);
      }
      if (ok) {
        float best = INFINITY;
        for (u32 c = 0; c < 3; ++c) {
          const u32 i = (c + 1) % 3, j = (c + 2) % 3;
          const float ax = px[i] - px[c], ay = py[i] - py[c];
          const float bx = px[j] - px[c], by = py[j] - py[c];
          const float dot = std::fabs(ax * bx + ay * by);
          const float norm = std::sqrt((ax * ax + ay * ay) * (bx * bx + by * by)) + 1e-20f;
          const float score = dot / norm;
          if (score < best) {
            best = score;
            k = c;
          }
        }
      }
    }
    const u32 i = (k + 1) % 3, j = (k + 2) % 3;
    const u32 base = r * 4;
    for (u32 S = 0; S < 16; ++S) {
      if (!(stream_mask & (1u << S)))
        continue;
      const StreamInfo &st = streams[S];
      u8 *dst = out.vertices[S].data() + size_t(base) * st.stride;
      for (u32 n = 0; n < 3; ++n) {
        const u8 *src = st.data + u64(g[n]) * st.stride;
        CopyVertexBytes(dst + n * st.stride, src, st.stride);
      }
      u8 *v3 = dst + 3 * st.stride;
      std::memcpy(v3, dst + i * st.stride, st.stride);
      for (const auto &e : layout.elements) {
        if (e.slotIndex != S)
          continue;
        const u32 bytes = std::min(16u, st.stride - e.alignedByteOffset);
        SynthesizeLane(v3 + e.alignedByteOffset, dst + i * st.stride + e.alignedByteOffset,
                       dst + j * st.stride + e.alignedByteOffset,
                       dst + k * st.stride + e.alignedByteOffset, e.format, bytes);
      }
    }
    out.indices.push_back(base + 0);
    out.indices.push_back(base + 1);
    out.indices.push_back(base + 2);
    out.indices.push_back(base + i);
    out.indices.push_back(base + 3);
    out.indices.push_back(base + j);
  }
  return true;
}

void FillPipelineState(DeviceView dev, const Targets &t, PipelineState &st,
                       u32 *spec_out, bool *alpha_to_coverage_only) {
  const u32 mode = dev.U32(dev::kModeControl);
  const bool cull_front = mode & 1, cull_back = mode & 2, face_cw = mode & 4;
  if (cull_front && cull_back)
    st.cull = plume::RenderCullMode::BACK;
  else if (cull_front)
    st.cull = plume::RenderCullMode::FRONT;
  else if (cull_back)
    st.cull = plume::RenderCullMode::BACK;
  else
    st.cull = plume::RenderCullMode::NONE;
  if (eot::debug::FreecamDrawTwoSided() || REXCVAR_GET(fable2_debug_no_cull))
    st.cull = plume::RenderCullMode::NONE;
  st.frontFace = face_cw ? plume::RenderFrontFace::CLOCKWISE
                         : plume::RenderFrontFace::COUNTER_CLOCKWISE;
  const u32 clip = dev.U32(dev::kClipControl);
  st.depthClip = (clip & ((1u << 16) | (1u << 26) | (1u << 27))) == 0;
  if (!st.depthClip) {
    u32 n;
    if (DiagShouldLog(0x6C1F, &n) && n < 3)
      EOT_DEBUG("[draw] clip control {:#x}: depth clip off", clip);
  }

  if (mode & (1u << 11)) {
    const float scale = dev.F32(dev::kPolyOffsetFrontScale);
    const float offset = dev.F32(dev::kPolyOffsetFrontOffset);
    st.slopeScaledDepthBias = scale / 16.0f;
    st.depthBias = PolygonOffsetUnits(offset);
    if (!t.colorCount) {  // shadow casters
      const bool scene_sized = t.width == kGuestRenderWidth && t.height == kGuestRenderHeight;
      if (REXCVAR_GET(fable2_shadow_bias_in_vs) && !scene_sized)
        st.depthBias = 0;  // applied in the vertex shader (ComputeViewport)
    }
  }
  const float rs = RenderScaleFactor();
  const float ts = std::fabs(t.scale - rs) < 0.01f ? rs : t.scale;
  st.targetScale = (st.depthBias || st.slopeScaledDepthBias != 0.0f) ? ts : 1.0f;

  const u32 dc = dev.U32(dev::kDepthControl);
  const bool has_ds = t.depth != nullptr;
  const bool z_test = has_ds && (dc & 2);
  st.depthWrite = has_ds && (dc & 4);
  st.depthEnable = z_test || st.depthWrite;
  st.depthFunc = z_test ? ConvertCompareFunc((dc >> 4) & 7)
                        : plume::RenderComparisonFunction::ALWAYS;
  // Debug (missing ground): run-time overrides.
  if (REXCVAR_GET(fable2_debug_depth_always))
    st.depthFunc = plume::RenderComparisonFunction::ALWAYS;
  if (REXCVAR_GET(fable2_debug_cull_none))
    st.cull = plume::RenderCullMode::NONE;
  st.stencilEnable = has_ds && (dc & 1);
  if (st.stencilEnable && !FormatHasStencil(t.depthImage ? t.depthImage->format : t.depth->host.format)) {
    u32 n;
    if (DiagShouldLog(0x6C30, &n) && n == 0)
      EOT_WARN("[draw] a draw into the stencil-less depth target {:#x} ({}x{}) enables the stencil test "
               "(control {:#x}); drawn without it",
               t.depth->va, t.depth->width, t.depth->height, dc);
    st.stencilEnable = false;
  }
  if (st.stencilEnable) {
    const u32 srm = dev.U32(dev::kStencilRefMask);
    st.stencilReadMask = static_cast<u8>((srm >> 8) & 0xFF);
    st.stencilWriteMask = static_cast<u8>((srm >> 16) & 0xFF);
    st.stencilRef = static_cast<u8>(srm & 0xFF);
    st.stencilFront.compareFunction = ConvertCompareFunc((dc >> 8) & 7);
    st.stencilFront.failOp = ConvertStencilOp((dc >> 11) & 7);
    st.stencilFront.passOp = ConvertStencilOp((dc >> 14) & 7);
    st.stencilFront.depthFailOp = ConvertStencilOp((dc >> 17) & 7);
    if (dc & (1u << 7)) {
      st.stencilBack.compareFunction = ConvertCompareFunc((dc >> 20) & 7);
      st.stencilBack.failOp = ConvertStencilOp((dc >> 23) & 7);
      st.stencilBack.passOp = ConvertStencilOp((dc >> 26) & 7);
      st.stencilBack.depthFailOp = ConvertStencilOp((dc >> 29) & 7);
    } else {
      st.stencilBack = st.stencilFront;
    }
  } else {
    st.stencilFront.compareFunction = plume::RenderComparisonFunction::ALWAYS;
    st.stencilBack.compareFunction = plume::RenderComparisonFunction::ALWAYS;
  }

  const u32 cc = dev.U32(dev::kColorControl);
  const bool blend_disable = (cc & (1u << 5)) != 0;
  const u32 color_mask = dev.U32(dev::kColorMask);
  static const u32 kBlendRegs[4] = {dev::kBlendControl0, dev::kBlendControl1,
                                    dev::kBlendControl2, dev::kBlendControl3};
  for (u32 i = 0; i < t.colorCount; ++i) {
    const u32 bc = dev.U32(kBlendRegs[i]);
    const u32 src = bc & 0x1F, op = (bc >> 5) & 7, dst = (bc >> 8) & 0x1F;
    const u32 srca = (bc >> 16) & 0x1F, opa = (bc >> 21) & 7, dsta = (bc >> 24) & 0x1F;
    const bool passthrough = src == 1 && dst == 0 && op == 0 && srca == 1 && dsta == 0 && opa == 0;
    plume::RenderBlendDesc &b = st.blend[i];
    b.blendEnabled = !blend_disable && !passthrough;
    b.srcBlend = ConvertBlendFactor(src);
    b.dstBlend = ConvertBlendFactor(dst);
    b.blendOp = ConvertBlendOp(op);
    b.srcBlendAlpha = ConvertBlendFactor(srca);
    b.dstBlendAlpha = ConvertBlendFactor(dsta);
    b.blendOpAlpha = ConvertBlendOp(opa);
    b.renderTargetWriteMask = static_cast<u8>((color_mask >> (4 * i)) & 0xF);
    st.rtFormats[i] = t.colorImage[i] ? t.colorImage[i]->format : t.color[i]->host.format;
    // 7e3 (k_2_10_10_10_FLOAT) targets: does anything use their 2-bit alpha?
    if (t.color[i] && t.color[i]->colorFormat == 3) {
      if (REXCVAR_GET(fable2_debug_7e3_no_alpha))
        b.renderTargetWriteMask &= 0x7;
      if (REXCVAR_GET(fable2_diag_7e3)) {
        static u64 draws = 0, blended = 0, dest_alpha = 0, alpha_written = 0;
        auto uses_dst_alpha = [](u32 f) { return f == 10 || f == 11 || f == 16; };
        ++draws;
        if (b.blendEnabled) {
          ++blended;
          if (uses_dst_alpha(src) || uses_dst_alpha(dst) || uses_dst_alpha(srca) || uses_dst_alpha(dsta))
            ++dest_alpha;
        }
        if (b.renderTargetWriteMask & 8)
          ++alpha_written;
        if (draws % 20000 == 0)
          EOT_INFO("[7e3] {} draws: {} blended, {} read dest alpha in blending, {} write alpha", draws, blended,
                   dest_alpha, alpha_written);
      }
    }
  }
  st.rtCount = t.colorCount;
  st.dsFormat = has_ds ? (t.depthImage ? t.depthImage->format : t.depth->host.format)
                       : plume::RenderFormat::UNKNOWN;
  st.sampleCount = t.samples;
  // Alpha to coverage is the shader's dither (g_AlphaToMask), not the host's.
  *alpha_to_coverage_only = (cc & (1u << 4)) != 0;
  st.alphaToCoverage = false;

  u32 spec = 0;
  if (cc & (1u << 3)) {
    const u32 func = cc & 7;
    if (func == 4 || func == 6) {
      if (!REXCVAR_GET(fable2_debug_no_alpha_test))
        spec |= kSpecAlphaTest;
    } else if (func == 0) {
      for (u32 i = 0; i < t.colorCount; ++i)
        st.blend[i].renderTargetWriteMask = 0;
    } else if (func != 7) {
      u32 n;
      if (DiagShouldLog(0x6200 + func, &n))
        EOT_WARN("[draw] alpha test function {} not modelled (treated as pass)", func);
    }
  }
  if (REXCVAR_GET(fable2_zero_mul))
    spec |= kSpecZeroMul;
  if (REXCVAR_GET(fable2_debug_nan_paint))
    spec |= kSpecNanPaint;
  if (state().depth_prerun) {  // emulated Z pass: depth only
    for (u32 i = 0; i < t.colorCount; ++i)
      st.blend[i].renderTargetWriteMask = 0;
    st.stencilEnable = false;
  }
  *spec_out = spec;
}

struct SamplerBindings {
  SharedConstants shared;
};

bool RefreshResolvedMirror(VideoState &s, GuestTexture &mirror, const Targets &t) {
  if (!mirror.resolveOwned || mirror.host.isDepth || !mirror.lastResolve)
    return false;
  GuestSurface *surf = nullptr;
  for (u32 i = 0; i < t.colorCount; ++i) {
    if (t.color[i] && t.color[i]->uid == mirror.resolvedSurfaceUid) {
      surf = t.color[i];
      break;
    }
  }
  if (!surf || surf->serial == mirror.resolvedSurfaceSerial ||
      mirror.resolvedSurfaceSerial < surf->wholeClearSerial)
    return false;
  ResolvePacket pk = *mirror.lastResolve;
  pk.refresh = true;
  pk.flags &= ~0x300u;
  const bool defer = s.defer_shader_read_transitions;
  s.defer_shader_read_transitions = false;
  ReplayResolveLocked(s, pk);
  s.defer_shader_read_transitions = defer;
  return s.command_list_open;
}

bool FlushAliasCopyForDraw(VideoState &s, GuestTexture &t) {
  const bool defer = s.defer_shader_read_transitions;
  s.defer_shader_read_transitions = false;
  const bool copied = FlushAliasCopy(s, t);
  s.defer_shader_read_transitions = defer;
  return copied;
}

void BindTexturesAndSamplers(VideoState &s, DeviceView dev, u32 texture_mask,
                             SharedConstants &sc, const Targets *refresh_targets, bool vertex_textures,
                             const DrawPacket &pk, float target_scale) {
  const u32 sampler_policy = static_cast<u32>(Settings::Anisotropy());
  const bool refresh_copies = refresh_targets && Settings::SceneCopyRefresh();
  struct DeferGuard {
    VideoState &s;
    explicit DeferGuard(VideoState &state) : s(state) { s.defer_shader_read_transitions = true; }
    ~DeferGuard() { s.defer_shader_read_transitions = false; }
  } defer_guard(s);
  for (u32 i = 0; i < 16; ++i) {
    sc.texture2DIndices[i] = kNullTexture2DDescriptorIndex;
    sc.texture3DIndices[i] = kNullTexture3DDescriptorIndex;
    sc.textureCubeIndices[i] = kNullTextureCubeDescriptorIndex;
    sc.texture1DIndices[i] = kNullTexture2DDescriptorIndex;
    sc.samplerIndices[i] = kSamplerLinearClamp;
    sc.samplerPointIndices[i] = kSamplerPointClamp;
    sc.samplerLinearIndices[i] = kSamplerLinearClamp;
  }
  const u64 generation = s.texture_generation.load(std::memory_order_relaxed);
  for (u32 slot = 0; slot < 16; ++slot)
    s.draw_bound_textures[slot] = nullptr;
  bool copied_while_binding = false;
  for (u32 slot = 0; slot < 16; ++slot) {
    if (!(texture_mask & (1u << slot)))
      continue;
    const u32 tex_va = dev.U32(dev::kTextureObject0 + 4 * slot);
    if (!tex_va)
      continue;
    if (REXCVAR_GET(fable2_debug_null_slots) & (1 << slot))
      continue;  // diagnostic: leave the slot on the null texture
    const u8 *fc_raw = dev.Bytes(dev::kFetchConstants + 24 * slot, 24);
    if (!fc_raw || (fc_raw[3] & 3) != 2)
      continue;
    s.perf.texture_bind_requests++;
    GuestTexture *gt = nullptr;
    u32 index = kInvalidDescriptorIndex;
    u32 sampler = 0, sampler_point = 0, sampler_linear = 0;
    u8 dimension = 0, biased_bits = 0;
    VideoState::TextureSlotCache *hit = nullptr;
    for (u32 w = 0; w < VideoState::kTextureSlotWays; ++w) {
      VideoState::TextureSlotCache &cs = s.slot_cache[slot][w];
      if (cs.texVa == tex_va && cs.generation == generation && cs.texture && cs.texture->aliasPending)
        copied_while_binding |= FlushAliasCopyForDraw(s, *cs.texture);
      if (cs.texVa == tex_va && cs.generation == generation && cs.texture &&
          cs.resourceGeneration == cs.texture->bindingGeneration &&
          cs.samplerPolicy == sampler_policy && std::memcmp(cs.fc, fc_raw, sizeof(cs.fc)) == 0) {
        hit = &cs;
        break;
      }
    }
    if (hit) {
      gt = hit->texture;
      // Video planes (k_8, written without Lock/Unlock) change every frame:
      // the bind cache must not skip their per-frame content check.
      if (gt->format == xe::TextureFormat::k_8 && !gt->resolveOwned) {
        const u32 fc3 = rex::memory::load_and_swap<u32>(fc_raw + 12);
        PrepareTextureForSampling(s, *gt, (fc3 >> 1) & 0xFFF);
      }
      gt->lastUseFrame = s.guest_frames;
      gt->lastSampledFrame = s.guest_frames;
      if (refresh_copies && gt->lastResolve)
        copied_while_binding |= RefreshResolvedMirror(s, *gt, *refresh_targets);
      TransitionLocked(s, gt->host, plume::RenderTextureLayout::SHADER_READ);
      index = hit->index;
      sampler = hit->sampler;
      sampler_point = hit->samplerPoint;
      sampler_linear = hit->samplerLinear;
      dimension = hit->dimension;
      biased_bits = hit->biasedBits;
      s.perf.texture_bind_hits++;
    } else {
      u32 fc[6];
      for (u32 d = 0; d < 6; ++d)
        fc[d] = rex::memory::load_and_swap<u32>(fc_raw + 4 * d);
      // The header as it was at the draw: the game re-points headers at other
      // render targets later in the frame (a shadow map's at a half-res buffer),
      // and the live one gave the draw that texture when the replay ran behind.
      gt = FindRenderTarget(s, fc);
      if (!gt && s.depth_prerun) {
        // Emulated Z pass: these draws run ahead of the scene, so never create
        // or replace a texture here (a re-pointed header would discard a
        // resolved image the queued draws still sample: white HUD panels).
        const auto it = s.textures.find(tex_va);
        GuestTexture *t = it != s.textures.end() ? it->second.get() : nullptr;
        if (t && t->host.texture && PhysicalBase(t->fetch[1]) == PhysicalBase(fc[1]) &&
            StorageFormat(t->fetch[1] & 0x3F) == StorageFormat(fc[1] & 0x3F))
          gt = t;
        if (!gt)
          continue;
      } else if (!gt) {
        gt = (pk.texHeaderMask & (1u << slot)) && REXCVAR_GET(fable2_texture_as_recorded)
                 ? GetGuestTextureForFetch(s, tex_va, pk.texHeaders[slot])
                 : GetGuestTexture(s, tex_va);
      }
      if (!gt) {
        u32 n;
        if (DiagShouldLog(0x6300 ^ tex_va, &n))
          EOT_WARN("[draw] slot {} texture {:#x} has no host mirror (fetch fmt {}); sampling null",
                   slot, tex_va, (fc[1] >> 0) & 0x3F);
        continue;
      }
      const u32 swizzle = (fc[3] >> 1) & 0xFFF;
      if (gt->aliasPending)
        copied_while_binding |= FlushAliasCopyForDraw(s, *gt);
      gt->lastSampledFrame = s.guest_frames;
      if (refresh_copies && gt->lastResolve)
        copied_while_binding |= RefreshResolvedMirror(s, *gt, *refresh_targets);
      index = PrepareTextureForSampling(s, *gt, swizzle);
      if (index == kInvalidDescriptorIndex) {
        u32 n;
        if (DiagShouldLog(0x6380 ^ tex_va, &n))
          EOT_WARN("[draw] slot {} texture {:#x} (fmt {} {}x{} {}) could not be bound; sampling null",
                   slot, tex_va, static_cast<u32>(gt->format), gt->width, gt->height,
                   gt->host.isDepth ? "depth" : "colour");
        continue;
      }
      {
        const plume::RenderSamplerDesc sd = DecodeSamplerFromFetch(
            fc, !gt->resolveOwned && !gt->host.isDepth && gt->host.mipLevels > 1, gt->synthMips);
        static u32 sampler_logs = 0;
        if (slot <= 2 && Settings::DiagFrame() > 0 &&
            s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame()) && sampler_logs++ < 40)
          EOT_INFO("[diag] sampler slot {} tex {:#x} {}x{} host mips {} fc4 {:#x} -> min {} mag {} mip {} "
                   "minLOD {} maxLOD {} bias {} aniso {}",
                   slot, tex_va, gt->width, gt->height, gt->host.mipLevels, fc[4],
                   static_cast<u32>(sd.minFilter), static_cast<u32>(sd.magFilter),
                   static_cast<u32>(sd.mipmapMode), sd.minLOD, sd.maxLOD, sd.mipLODBias,
                   sd.maxAnisotropy);
      }
      const bool mipmapped_upload = !gt->resolveOwned && !gt->host.isDepth && gt->host.mipLevels > 1;
      sampler = ResolveSamplerSlotLocked(DecodeSamplerFromFetch(fc, mipmapped_upload, gt->synthMips));
      // Fetch instructions may set MinFilter/MagFilter themselves (bits 19..22
      // of dword 3: 0 point, 1 linear); XenosRecomp picks these for them.
      u32 fc_filter[6];
      std::memcpy(fc_filter, fc, sizeof(fc_filter));
      fc_filter[3] = fc[3] & ~(0xFu << 19);
      sampler_point = ResolveSamplerSlotLocked(DecodeSamplerFromFetch(fc_filter, mipmapped_upload, gt->synthMips));
      fc_filter[3] |= 0x5u << 19;
      sampler_linear = ResolveSamplerSlotLocked(DecodeSamplerFromFetch(fc_filter, mipmapped_upload, gt->synthMips));
      sampler |= SamplerLodBiasBits(fc, sampler);
      sampler_point |= SamplerLodBiasBits(fc, sampler_point);
      sampler_linear |= SamplerLodBiasBits(fc, sampler_linear);
      dimension = static_cast<u8>(gt->dimension);
      const u32 sign_x = (fc[0] >> 2) & 3;
      const u32 sign_w = (fc[0] >> 8) & 3;
      if (sign_x == 2)
        biased_bits |= 1;
      if (sign_x == 3) {
        const bool srgb_view = gt->host.format == plume::RenderFormat::BC1_UNORM_SRGB ||
                               gt->host.format == plume::RenderFormat::BC2_UNORM_SRGB ||
                               gt->host.format == plume::RenderFormat::BC3_UNORM_SRGB;
        if (!srgb_view)
          biased_bits |= 2;
      }
      if (sign_w == 2)
        biased_bits |= 4;
      u8 &next = s.slot_cache_next[slot];
      VideoState::TextureSlotCache &cs = s.slot_cache[slot][next];
      next = static_cast<u8>((next + 1) % VideoState::kTextureSlotWays);
      cs.texVa = tex_va;
      std::memcpy(cs.fc, fc_raw, sizeof(cs.fc));
      cs.generation = s.texture_generation.load(std::memory_order_relaxed);
      cs.resourceGeneration = gt->bindingGeneration;
      cs.samplerPolicy = sampler_policy;
      cs.texture = gt;
      cs.index = index;
      cs.sampler = sampler;
      cs.samplerPoint = sampler_point;
      cs.samplerLinear = sampler_linear;
      cs.dimension = dimension;
      cs.biasedBits = biased_bits;
    }
    gt->perfSamples++;
    s.draw_bound_textures[slot] = gt;
    if (const i32 want = REXCVAR_GET(fable2_debug_dump_mips); want != 0 && pk.vs &&
        !REXCVAR_GET(fable2_debug_dump_mips_vs).empty() &&
        (DrawMatchesList(REXCVAR_GET(fable2_debug_dump_mips_vs), pk.vs->hash, 0) ||
         (pk.ps && DrawMatchesList(REXCVAR_GET(fable2_debug_dump_mips_vs), pk.ps->hash, 0)))) {
      static i32 dumped = 0;
      static u64 dumped_frame = ~0ull;
      if (dumped != want) {
        dumped = want;
        dumped_frame = s.guest_frames;
      }
      if (s.guest_frames - dumped_frame < 3) {
        const auto it = s.textures.find(gt->va);
        static u32 lines = 0;
        if (it != s.textures.end() && it->second.get() == gt && lines++ < 80) {
          EOT_INFO("[mipdump] {} frame {} ps {:016x} slot {} tex {:#x} fmt {} {}x{} host {}x{} mips {} resolveOwned {} host {} base {:#x} mip {:#x} fetch [{:08x} {:08x} {:08x} {:08x} {:08x} {:08x}] bound obj {:#x}",
                   want, s.guest_frames, pk.ps ? pk.ps->hash : 0, slot, gt->va, static_cast<u32>(gt->format), gt->width, gt->height, gt->host.width,
                   gt->host.height, gt->host.mipLevels, gt->resolveOwned,
                   static_cast<const void *>(gt->host.texture.get()), gt->baseAddress, gt->mipAddress, gt->fetch[0],
                   gt->fetch[1], gt->fetch[2], gt->fetch[3], gt->fetch[4], gt->fetch[5], tex_va);
          EOT_INFO("[mipdump]   device fetch [{:08x} {:08x} {:08x} {:08x} {:08x} {:08x}] sampler {} point {} linear {}",
                   dev.U32(dev::kFetchConstants + 24 * slot), dev.U32(dev::kFetchConstants + 24 * slot + 4),
                   dev.U32(dev::kFetchConstants + 24 * slot + 8), dev.U32(dev::kFetchConstants + 24 * slot + 12),
                   dev.U32(dev::kFetchConstants + 24 * slot + 16), dev.U32(dev::kFetchConstants + 24 * slot + 20),
                   sampler, sampler_point, sampler_linear);
          for (u32 m = 0; m < gt->host.mipLevels && m < 6 && g_pending_mip_dumps.size() < 32; ++m)
            for (bool a : {false, true})
              g_pending_mip_dumps.push_back(
                  {it->second, std::format("logs/mip{}_s{}_{:08x}_l{}{}.ppm", want, slot, gt->va, m, a ? "_a" : ""),
                   m, a});
        }
      }
    }
    if (REXCVAR_GET(fable2_diag_shadow_mask) && pk.ps && pk.ps->hash == 0x383793fa8d5a0716ull) {
      // The screen shadow mask: are the textures it samples this frame's resolves?
      static u64 frame = ~0ull, draws = 0, stale = 0, fresh = 0, never = 0;
      static std::string last_sig;
      ++draws;
      if (!gt->lastResolvedFrame)
        ++never;
      else if (gt->lastResolvedFrame == s.guest_frames)
        ++fresh;
      else
        ++stale;
      const std::string sig = std::format("s{}:{:x}/{}/{}x{}", slot, tex_va,
                                          static_cast<const void *>(gt->host.texture.get()), gt->host.width,
                                          gt->host.height);
      if (gt->lastResolvedFrame && gt->lastResolvedFrame != s.guest_frames && gt->host.width >= 1024) {
        static u32 logged = 0;
        if (logged++ < 30)
          EOT_INFO("[shadowmask] frame {} slot {} {} resolved at frame {} (stale by {})", s.guest_frames, slot, sig,
                   gt->lastResolvedFrame, s.guest_frames - gt->lastResolvedFrame);
      }
      if (frame != s.guest_frames && (s.guest_frames % 600) == 0)
        EOT_INFO("[shadowmask] samples: {} fresh, {} stale, {} never resolved ({} slot binds)", fresh, stale, never,
                 draws);
      const i32 every = REXCVAR_GET(fable2_dump_shadow_maps);
      if (every > 0 && (s.guest_frames % static_cast<u64>(every)) == 0 && gt->host.width >= 1024 &&
          g_pending_bound_dumps.size() < 8) {
        const auto it = s.textures.find(gt->va);
        if (it != s.textures.end() && it->second.get() == gt)
          g_pending_bound_dumps.push_back(
              {it->second, std::format("logs/smap_f{}_s{}_{}x{}.ppm", s.guest_frames, slot, gt->host.width,
                                       gt->host.height)});
      }
      frame = s.guest_frames;
    }
    switch (static_cast<xe::DataDimension>(dimension)) {
    case xe::DataDimension::k3D:
      sc.texture3DIndices[slot] = index;
      break;
    case xe::DataDimension::kCube:
      sc.textureCubeIndices[slot] = index;
      break;
    default:
      sc.texture2DIndices[slot] = index;
      sc.texture1DIndices[slot] = index;
      break;
    }
    sc.texHostScale[slot] =
        gt->width ? static_cast<float>(gt->host.width) / static_cast<float>(gt->width) : 0.0f;
    // A texture mirrored above the target's scale (a 1x small target reading a
    // 2x mirror) has its implicit LOD raised by log2(texture / target scale):
    // a coarser mip than the console reads. Bias it back. The tree impostor
    // atlases' 64x64 and 32x32 mips are built from the level above that way;
    // the raised LOD read the level being built (empty) instead.
    const bool lod_bias = REXCVAR_GET(fable2_fetch_lod_bias);
    i32 scale_bias = 0;
    if (lod_bias && target_scale > 0.0f && sc.texHostScale[slot] > target_scale * 1.01f)
      scale_bias = -static_cast<i32>(std::lround(std::log2(sc.texHostScale[slot] / target_scale) * 32.0f));
    const auto with_bias = [&](u32 index) {
      return lod_bias ? SamplerAddLodBias(index, scale_bias) : (index >> 30) ? index : index & 0xFFFFFu;
    };
    sc.samplerIndices[slot] = with_bias(sampler);
    const bool filter_override = REXCVAR_GET(fable2_fetch_filter_override);
    sc.samplerPointIndices[slot] = with_bias(filter_override ? sampler_point : sampler);
    sc.samplerLinearIndices[slot] = with_bias(filter_override ? sampler_linear : sampler);
    if (biased_bits & 1)
      sc.biasedTextures |= 1u << slot;
    if (biased_bits & 2)
      sc.biasedTextures |= 1u << (16 + slot);
    if (biased_bits & 4)
      sc.sintTexcoords |= 1u << (16 + slot);
  }
  // Vertex shader texture fetches (fetch constants 16..19, the D3D vertex
  // samplers): Fable 2's terrain and water displace their vertices with them;
  // left unbound they read zero and the ground collapsed out of view.
  sc.vsBiasedTextures = 0;
  for (u32 slot = 16; slot < 20; ++slot) {
    const u32 v = slot - 16;
    sc.vsTexture2DIndices[v] = kNullTexture2DDescriptorIndex;
    sc.vsTexture3DIndices[v] = kNullTexture3DDescriptorIndex;
    sc.vsTextureCubeIndices[v] = kNullTextureCubeDescriptorIndex;
    sc.vsTexture1DIndices[v] = kNullTexture2DDescriptorIndex;
    sc.vsSamplerIndices[v] = kSamplerLinearClamp;
    if (!vertex_textures || !(texture_mask & (1u << slot)) ||
        (REXCVAR_GET(fable2_debug_null_vs_slots) & (1 << v)))
      continue;
    const u8 *fc_raw = dev.Bytes(dev::kFetchConstants + 24 * slot, 24);
    if (!fc_raw || (fc_raw[3] & 3) != 2)
      continue;
    u32 fc[6];
    for (u32 d = 0; d < 6; ++d)
      fc[d] = rex::memory::load_and_swap<u32>(fc_raw + 4 * d);
    // The device's texture pointers cover the pixel samplers only; find the
    // texture by its storage (base address, format, size) instead.
    // Synthetic key: the physical base and format (below the headers' 0x40000000+).
    const u32 tex_va = (fc[1] & 0x1FFFF000u) | (fc[1] & 0x3Fu);
    // A GPU-written image at this address (a resolve, e.g. the 1x1 exposure
    // the intro videos' vertex shader reads) wins over guest memory, which
    // only holds what the guest wrote.
    // The texture the game keeps at this memory: a render target, or one a
    // pixel sampler's header made (refreshed when the CPU rewrites it). Headers
    // hold the 0xE0000000 alias, the fetch constant the physical address.
    GuestTexture *gt = FindRenderTarget(s, fc);
    if (!gt) {
      const u32 base = PhysicalBase(fc[1]), format = StorageFormat(fc[1] & 0x3F);
      for (auto &[other_va, other] : s.textures)
        if (other && other->host.texture && !other->host.isDepth && PhysicalBase(other->fetch[1]) == base &&
            StorageFormat(other->fetch[1] & 0x3F) == format && other->fetch[2] == fc[2] &&
            (!gt || other->lastSampledFrame > gt->lastSampledFrame))
          gt = other.get();
    }
    if (!gt)
      gt = GetGuestTextureForFetch(s, tex_va, fc);
    if (!gt) {
      u32 n;
      if (DiagShouldLog(0x6400 ^ tex_va ^ slot, &n))
        EOT_WARN("[draw] vertex texture slot {} {:#x}: no texture (fetch base {:#x})", slot,
                 tex_va, fc[1] & ~0xFFFu);
      continue;
    }
    const u32 index = PrepareTextureForSampling(s, *gt, (fc[3] >> 1) & 0xFFF);
    if (index == kInvalidDescriptorIndex)
      continue;
    switch (gt->dimension) {
    case xe::DataDimension::k3D:
      sc.vsTexture3DIndices[v] = index;
      break;
    case xe::DataDimension::kCube:
      sc.vsTextureCubeIndices[v] = index;
      break;
    default:
      sc.vsTexture2DIndices[v] = index;
      sc.vsTexture1DIndices[v] = index;
      break;
    }
    const u32 sign_x = (fc[0] >> 2) & 3;  // 1 signed (SNORM host format), 2 biased
    if (sign_x == 2)
      sc.vsBiasedTextures |= 1u << v;
    sc.vsSamplerIndices[v] = ResolveSamplerSlotLocked(
        DecodeSamplerFromFetch(fc, !gt->resolveOwned && !gt->host.isDepth && gt->host.mipLevels > 1,
                               gt->synthMips));
    static bool logged[4] = {};
    if (!logged[v]) {
      logged[v] = true;
      EOT_INFO("[draw] vertex texture fetch: slot {} {:#x} {}x{} fmt {} sign {} host fmt {}", slot, tex_va,
               gt->width, gt->height, static_cast<u32>(gt->format), sign_x, static_cast<u32>(gt->host.format));
    }
  }
  if (copied_while_binding)
    for (u32 slot = 0; slot < 16; ++slot)
      if (GuestTexture *bound = s.draw_bound_textures[slot])
        TransitionLocked(s, bound->host, plume::RenderTextureLayout::SHADER_READ);
}

bool UploadZeroBuffer(VideoState &s, UploadAlloc *out) {
  static UploadAlloc cached[kUploadRingSlots];
  static u64 cached_frame[kUploadRingSlots] = {};
  static bool cached_valid[kUploadRingSlots] = {};
  const u32 slot = UploadRingSlot();
  (void)s;
  if (cached_valid[slot] && cached_frame[slot] == UploadRingEpoch()) {
    *out = cached[slot];
    return true;
  }
  if (!UploadAllocate(4096, kConstantBufferAlignment, out))
    return false;
  std::memset(out->cpu, 0, 4096);
  cached[slot] = *out;
  cached_frame[slot] = UploadRingEpoch();
  cached_valid[slot] = true;
  return true;
}

static_assert(dev::kPsFloatConstants + 256u * 16u == dev::kVsBoolConstants);

bool VelocityCandidate(DeviceView dev, const TargetWords &tw) {
  if (tw.colorCount != 1 || !tw.depthVa || !tw.colorVa[0])
    return false;
  if (!(dev.U32(dev::kDepthControl) & 4))
    return false;
  if ((dev.U32(dev::kVteControl) & 5) != 5)
    return false;
  const float xs = std::fabs(dev.F32(dev::kVportXScale)), ys = std::fabs(dev.F32(dev::kVportYScale));
  if (std::fabs(xs * 2.0f - static_cast<float>(kGuestRenderWidth)) >= 1.0f ||
      std::fabs(ys * 2.0f - static_cast<float>(kGuestRenderHeight)) >= 1.0f)
    return false;
  const u32 *w = tw.colorWords[0];
  const u32 width = (w[3] >> 18) + 1, height = ((w[3] >> 3) & 0x7FFF) + 1;
  if (width != kGuestRenderWidth || height != kGuestRenderHeight)
    return false;
  return ((w[1] >> 16) & 0xF) == 7; // ColorRenderTargetFormat k_16_16_16_16_FLOAT
}

bool ShadowTileWords(const u32 words[5]) {
  const u32 size_bits = words[3];
  return (size_bits >> 18) + 1 == 1024 && ((size_bits >> 3) & 0x7FFF) + 1 == 1024;
}

struct ShadowPassCensus {
  u64 frame = ~0ull;
  u32 pass = 0;
  u64 hash = 0;
  u32 casters = 0;
  u32 skinned = 0;
  u64 staticHash = 0;
  u64 lastHashes[64] = {};
  u64 lastStaticHashes[64] = {};
  u32 lastCasters[64] = {};
  u32 lastPasses = 0;
  u64 hashes[64] = {};
  u64 staticHashes[64] = {};
  u32 castersOf[64] = {};
  u64 sumPasses = 0, sumRepeats = 0, sumCasters = 0, sumRepeatCasters = 0, frames = 0;
  u64 sumSkinned = 0, sumStaticRepeatPasses = 0, sumStaticRepeatCasters = 0;
  u64 lastPrint = 0;
};
ShadowPassCensus g_shadow_census;

void ShadowPassClose(ShadowPassCensus &c) {
  if (!c.casters)
    return;
  if (c.pass < 64) {
    c.hashes[c.pass] = c.hash;
    c.staticHashes[c.pass] = c.staticHash;
    c.castersOf[c.pass] = c.casters;
    const bool repeat = c.skinned == 0 && c.pass < c.lastPasses && c.lastHashes[c.pass] == c.hash;
    const bool static_repeat = c.pass < c.lastPasses && c.lastStaticHashes[c.pass] == c.staticHash;
    c.sumPasses++;
    c.sumCasters += c.casters;
    c.sumSkinned += c.skinned;
    if (repeat) {
      c.sumRepeats++;
      c.sumRepeatCasters += c.casters;
    }
    if (static_repeat) {
      c.sumStaticRepeatPasses++;
      c.sumStaticRepeatCasters += c.casters - c.skinned;
    }
  }
  c.pass++;
  c.hash = 0;
  c.staticHash = 0;
  c.casters = 0;
  c.skinned = 0;
}

void NoteShadowCaster(VideoState &s, DeviceView dev, const GuestShader &vs, const StreamInfo streams[16],
                      u32 lo, u32 hi) {
  if (Settings::DiagVerbosity() < 2)
    return;
  ShadowPassCensus &c = g_shadow_census;
  if (c.frame != s.guest_frames) {
    ShadowPassClose(c);
    std::memcpy(c.lastHashes, c.hashes, sizeof(c.hashes));
    std::memcpy(c.lastStaticHashes, c.staticHashes, sizeof(c.staticHashes));
    std::memcpy(c.lastCasters, c.castersOf, sizeof(c.castersOf));
    c.lastPasses = std::min(c.pass, 64u);
    c.pass = 0;
    c.frames++;
    c.frame = s.guest_frames;
    if (s.guest_frames - c.lastPrint >= 300) {
      c.lastPrint = s.guest_frames;
      if (c.frames) {
        EOT_DEBUG("[shadow] census over {} frames: {:.1f} passes a frame, {:.1f} whole repeats of the previous "
                  "frame ({:.0f}%), {:.1f} static repeats ({:.0f}%); {:.0f} casters a frame, {:.0f} skinned, "
                  "{:.0f} in whole repeats, {:.0f} static casters in static repeats",
                  c.frames, static_cast<f64>(c.sumPasses) / c.frames,
                  static_cast<f64>(c.sumRepeats) / c.frames,
                  c.sumPasses ? 100.0 * c.sumRepeats / c.sumPasses : 0.0,
                  static_cast<f64>(c.sumStaticRepeatPasses) / c.frames,
                  c.sumPasses ? 100.0 * c.sumStaticRepeatPasses / c.sumPasses : 0.0,
                  static_cast<f64>(c.sumCasters) / c.frames, static_cast<f64>(c.sumSkinned) / c.frames,
                  static_cast<f64>(c.sumRepeatCasters) / c.frames,
                  static_cast<f64>(c.sumStaticRepeatCasters) / c.frames);
      }
      c.sumPasses = c.sumRepeats = c.sumCasters = c.sumRepeatCasters = c.frames = 0;
      c.sumSkinned = c.sumStaticRepeatPasses = c.sumStaticRepeatCasters = 0;
    }
  }
  u64 rows = 0x9E3779B97F4A7C15ull;
  u64 world = 0x9E3779B97F4A7C15ull;
  if (const u8 *file = dev.Bytes(dev::kVsFloatConstants, 112)) {
    for (u32 i = 16; i < 28; ++i) {
      u32 raw;
      std::memcpy(&raw, file + 4 * i, 4);
      world ^= raw + 0x9E3779B97F4A7C15ull + (world << 6) + (world >> 2);
    }
    for (u32 i = 0; i < 16; ++i) {
      u32 raw;
      std::memcpy(&raw, file + 4 * i, 4);
      rows ^= raw + 0x9E3779B97F4A7C15ull + (rows << 6) + (rows >> 2);
    }
  }
  static u64 pass_rows = 0;
  if (c.casters && rows != pass_rows)
    ShadowPassClose(c);
  if (!c.casters)
    pass_rows = rows;
  const bool skinned = vs.floatConstantRegs > 16;
  u64 h = c.hash ? c.hash : rows;
  const auto mix = [&h](u64 v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); };
  mix(vs.hash);
  mix(streams[0].dataVa);
  mix((u64(lo) << 32) | hi);
  mix(world);
  if (skinned)
    mix(s.guest_frames);
  c.hash = h;
  if (!skinned) {
    u64 sh = c.staticHash ? c.staticHash : rows;
    sh ^= h + 0x9E3779B97F4A7C15ull + (sh << 6) + (sh >> 2);
    c.staticHash = sh;
  } else {
    c.skinned++;
  }
  c.casters++;
}

bool DrawMatchesList(const std::string &list, u64 hash, u32 count);

bool CaptureDraw(VideoState &s, u32 device_va, u32 prim, GeometryPlan &geom,
                 const u8 *device_image, DrawPacket &pk) {
  static u32 capture_count = 0;
  const bool timed = (++capture_count & 15u) == 0;
  u64 lap_t0 = timed ? PerfNow() : 0;
  auto lap = [&](f64 &acc) {
    if (!timed)
      return;
    const u64 t1 = PerfNow();
    acc += 16.0 * static_cast<f64>(t1 - lap_t0) * PerfMsPerTick();
    lap_t0 = t1;
  };
  pk.device_va = device_va;
  pk.prim = prim;
  pk.indexed = geom.indexed;
  pk.rectList = geom.rectList;
  pk.topology = geom.topology;
  pk.vertexCount = geom.vertexCount;
  pk.hasCached = false;
  pk.index_alloc = UploadAlloc{};
  pk.index_count = 0;
  pk.host_base_vertex = 0;
  pk.tess_segments = geom.tessSegments;
  pk.tess_first_patch = geom.tessFirstPatch;
  pk.zero = UploadAlloc{};
  DeviceView dev = Device(device_va);
  const u8 *regs = nullptr;
  if (device_image) {
    dev.snapshot = device_image;
    dev.snapshotSize = kDeviceSnapshotBytes;
    regs = device_image;
  } else if (const u8 *live = mem::at<u8>(device_va)) {
    dev.snapshot = live;
    dev.snapshotSize = dev::kDeviceSize;
    regs = live;
  }
  if (!regs) {
    Dropped("device unreadable", 0x6000);
    return false;
  }
  pk.window.Capture(regs);
  pk.texHeaderMask = 0;
  for (u32 slot = 0; slot < 16; ++slot) {
    const u32 tex_va = *reinterpret_cast<const be_u32 *>(regs + dev::kTextureObject0 + 4 * slot);
    if (tex_va && ReadTextureHeader(tex_va, pk.texHeaders[slot]))
      pk.texHeaderMask |= 1u << slot;
  }

  // Fable 2: GpuLoadShaders loads shaders without touching the device's
  // pointers (which may still hold an older SetPixelShader's shader); the
  // last of GpuLoadShaders and Set*Shader wins per stage. kGpuOwnedNone marks
  // a GpuLoadShaders with no pixel shader.
  u32 vs_va = dev.U32(dev::kVertexShader);
  u32 ps_va = dev.U32(dev::kPixelShader);
  if (const u32 owned = g_gpu_owned_vs.load(std::memory_order_relaxed))
    vs_va = owned;
  if (const u32 owned = g_gpu_owned_ps.load(std::memory_order_relaxed))
    ps_va = owned == kGpuOwnedNone ? 0 : owned;
  struct ShaderLookupCache {
    u64 generation = 0;
    u32 vs_va = 0, ps_va = 0;
    GuestShader *vs = nullptr, *ps = nullptr;
  };
  static thread_local ShaderLookupCache slc;
  const u64 shader_gen = s.shader_generation.load(std::memory_order_acquire);
  if (slc.generation != shader_gen)
    slc = ShaderLookupCache{shader_gen};
  GuestShader *vs = slc.vs_va == vs_va && slc.vs ? slc.vs : nullptr;
  if (!vs) {
    vs = FindGuestShader(s, vs_va, false);
    if (!vs && vs_va)
      vs = RegisterGuestShader(s, vs_va, false);
    slc.vs_va = vs_va;
    slc.vs = vs;
  }
  GuestShader *ps = slc.ps_va == ps_va && slc.ps ? slc.ps : nullptr;
  if (!ps) {
    ps = FindGuestShader(s, ps_va, true);
    if (!ps && ps_va)
      ps = RegisterGuestShader(s, ps_va, true);
    slc.ps_va = ps_va;
    slc.ps = ps;
  }
  if (!vs) {
    u32 n;
    if (DiagShouldLog(0x6021, &n))
      EOT_WARN("[draw] no VS: vs_va={:#x} ps_va={:#x} prim={} {} n={} rt0={:#x} decl={:#x} "
               "stream0={:#x} frame={}",
               vs_va, ps_va, prim, geom.indexed ? "idx" : "vtx", geom.vertexCount,
               dev.U32(dev::kRenderTarget0), dev.U32(dev::kVertexDeclaration),
               dev.U32(dev::kStreamObject0), s.guest_frames);
    Dropped(vs_va ? "vertex shader object unreadable" : "no vertex shader bound", 0x6001);
    return false;
  }
  if (ps_va && !ps) {
    Dropped("pixel shader object unreadable", 0x6013);
    return false;
  }
  // Fable 2: shaders the built-in cache lacks are translated in the background
  // (runtime_shaders.cpp); their draws drop only until the translation lands.
  if (!vs->entry)
    AttachRuntimeShader(*vs);
  if (ps && !ps->entry)
    AttachRuntimeShader(*ps);
  if (!vs->entry || (ps && !ps->entry)) {
    trace::Bump(trace::Counter::ShaderMiss);
    auto note_miss = [](GuestShader &sh) {
      if (sh.entry || sh.cacheMissLogged)
        return;
      sh.cacheMissLogged = true;
      EOT_WARN("[shaders] {} {:#x} hash {:016x} is not in the shader cache",
               sh.isPixel ? "ps" : "vs", sh.va, sh.hash);
    };
    note_miss(*vs);
    if (ps)
      note_miss(*ps);
    Dropped("shader cache miss", 0x6002);
    return false;
  }
  pk.vs = vs;
  pk.ps = ps;
  pk.hasCameraVP = false;
  if (Settings::Taa() || Settings::DiagVerbosity() >= 2) {
    struct CameraByTarget {
      u32 rt0 = 0;
      float vp[16] = {};
      float prevVp[16] = {};
      bool prevValid = false;
      u64 frame = ~0ull;
      bool skip = false;
    };
    static CameraByTarget cameras[4];
    static u32 camera_next = 0;
    static const CameraByTarget *camera_last = nullptr;
    static u64 capture_frame = 0;
    static u64 last_present_count = ~0ull;
    if (s.captured_presents != last_present_count) {
      last_present_count = s.captured_presents;
      ++capture_frame;
    }
    const u32 rt0 = dev.U32(dev::kRenderTarget0);
    const u32 vte = dev.U32(dev::kVteControl);
    const bool projected = (vte & 5) == 5;
    const float xs = std::fabs(dev.F32(dev::kVportXScale)), ys = std::fabs(dev.F32(dev::kVportYScale));
    const bool frame_sized = std::fabs(xs * 2.0f - static_cast<float>(kGuestRenderWidth)) < 1.0f &&
                             std::fabs(ys * 2.0f - static_cast<float>(kGuestRenderHeight)) < 1.0f;
    if (projected && !geom.rectList && frame_sized && rt0) {
      if (const auto *m = eot::mem::at<eot::be<float>>(taa::kViewProjectionVa)) {
        CameraByTarget *slot = nullptr;
        for (CameraByTarget &c : cameras)
          if (c.rt0 == rt0)
            slot = &c;
        if (!slot)
          slot = &cameras[camera_next++ % 4];
        if (slot->rt0 != rt0) {
          slot->prevValid = false;
          slot->skip = false;
        }
        slot->rt0 = rt0;
        if (slot->frame != capture_frame) {
          if (slot->frame != ~0ull) {
            std::memcpy(slot->prevVp, slot->vp, sizeof(slot->prevVp));
            slot->prevValid = true;
          }
          slot->frame = capture_frame;
          for (u32 i = 0; i < 16; ++i)
            slot->vp[i] = m[i];
          const float motion =
              slot->prevValid
                  ? taa::CameraMotionPixels(slot->prevVp, slot->vp, static_cast<float>(InternalRenderWidth()),
                                            static_cast<float>(InternalRenderHeight()))
                  : 0.0f;
          slot->skip = taa::FastCameraGate(static_cast<u32>(slot - cameras), motion,
                                           static_cast<float>(InternalRenderWidth()));
        }
        camera_last = slot;
      }
    }
    if (camera_last && camera_last->frame == capture_frame)
      pk.taaSkip = camera_last->skip;
    if (ps && taa::IsSceneConsumer(ps->hash)) {
      const CameraByTarget *pick = nullptr;
      for (const CameraByTarget &c : cameras)
        if (c.rt0 && c.rt0 == rt0)
          pick = &c;
      if (!pick)
        pick = camera_last;
      if (pick && pick->rt0) {
        std::memcpy(pk.cameraVP, pick->vp, sizeof(pk.cameraVP));
        pk.hasCameraVP = true;
        pk.taaSkip = pick->skip;
      }
    }
  }
  pk.vs_va = vs_va;
  pk.ps_va = ps_va;

  if (!CaptureTargetWords(dev, pk.targets)) {
    Dropped("no render target or depth surface bound", 0x6003);
    return false;
  }
  if (REXCVAR_GET(fable2_debug_skip_256_draws) && pk.targets.colorCount) {
    // Debug: the per-character 256x256 renders in Bowerstone town (cost test).
    const u32 info = dev.U32(dev::kSurfaceInfo);
    if ((info & 0x3FFF) == 256) {
      Dropped("256-wide colour draw skipped (fable2_debug_skip_256_draws)", 0x6303);
      return false;
    }
  }
  if (REXCVAR_GET(fable2_diag_shadow_seq) && !pk.targets.colorCount && pk.targets.depthVa &&
      ShadowTileWords(pk.targets.depthWords)) {
    // The order of one frame's shadow-pass draws: which pixel shader each has.
    static u64 seq_frame = 0;
    static u32 logged = 0;
    if (s.guest_frames >= 6000 && (seq_frame == 0 || seq_frame == s.guest_frames) && logged < 60) {
      seq_frame = s.guest_frames;
      ++logged;
      const u32 cc = dev.U32(dev::kColorControl);
      EOT_INFO("[shadowseq] f{} #{} vs {:016x} ps {:016x} owned_ps {:#x} dev_ps {:#x} atest {} a2c {} kill {} n {}",
               s.guest_frames, logged, vs->hash, ps ? ps->hash : 0, g_gpu_owned_ps.load(std::memory_order_relaxed),
               dev.U32(dev::kPixelShader), (cc & 8) ? (cc & 7) : 9, (cc >> 4) & 1,
               ps && ps->entry ? (ps->entry->shaderFlags & 1) : 0,
               geom.indexed ? (geom.cached ? geom.cached->count : geom.indices.size()) : geom.vertexCount);
    }
  }

  // Vertex pulling: copy the pulled fetch constants' memory now, while the
  // game's per-frame buffers (bone palettes, ...) still hold this draw's data.
  pk.vertex_pull_table = 0;
  if (const std::vector<u8> &pulled_slots = PulledSlots(*vs); !pulled_slots.empty()) {
    // BeginVertices draws: the listed shaders pull stream 0 from the draw's own
    // data (Fable 2's tree billboards; the device's fetch constant still points
    // wherever SetStreamSource last left it, and they drew phantom trees).
    const bool up_stream = geom.stream0OverrideVa && !REXCVAR_GET(fable2_pull_up_stream_vs).empty() &&
                           DrawMatchesList(REXCVAR_GET(fable2_pull_up_stream_vs), vs->hash, 0);
    if (!UploadVertexPullTable(dev, *vs, pulled_slots, &pk.vertex_pull_table,
                               up_stream ? geom.stream0OverrideVa : 0u,
                               up_stream ? geom.vertexCount * geom.stream0OverrideStride : 0u)) {
      Dropped("vertex pulling upload failed", 0x6030);
      return false;
    }
  }
  const InputLayout *layout = GetInputLayout(s, *vs, dev.U32(dev::kVertexDeclaration));
  if (!layout && ((geom.tessSegments && vs->inputs.empty()) || (vs->entry && vs->entry->vertexLayoutCount == 0))) {
    // Patch shaders compute everything from r0 (patch, u, v), shaders without
    // vertex fetches from r0.x (the vertex index): no vertex input. Dropping
    // the latter lost every draw of vs 6b0779590f1e3de3 in some launches.
    if (!geom.tessSegments) {
      static std::mutex mutex;
      static std::unordered_set<u64> seen;
      std::lock_guard lock(mutex);
      if (seen.size() < 64 && seen.insert(vs->hash).second)
        EOT_INFO("[layout] vs {:016x}: no vertex fetches, drawn without vertex input", vs->hash);
    }
    static const InputLayout empty_layout{};
    layout = &empty_layout;
  }
  if (!layout) {
    {
      static std::mutex mutex;
      static std::unordered_set<u64> seen;
      std::lock_guard lock(mutex);
      if (seen.size() < 64 && seen.insert(vs->hash).second)
        EOT_WARN("[draw] no input layout: vs {:016x} decl {:#x} inputs {} entry {}", vs->hash,
                 dev.U32(dev::kVertexDeclaration), vs->inputs.size(), vs->entry ? "yes" : "no");
    }
    Dropped("no input layout for the bound declaration", 0x6004);
    return false;
  }
  pk.layout = layout;
  lap(s.perf.setup_ms);
  StreamInfo streams[16];
  for (u32 S = 0; S < 16; ++S) {
    if (!(layout->streamMask & (1u << S)))
      continue;
    if (S == 0 && geom.stream0OverrideVa) {
      streams[0].stream = 0;
      streams[0].stride = geom.stream0OverrideStride;
      streams[0].data = mem::at<u8>(geom.stream0OverrideVa);
      streams[0].sizeBytes = geom.vertexCount * geom.stream0OverrideStride;
      if (!streams[0].data || !streams[0].stride) {
        Dropped("BeginVertices data unreadable", 0x6015);
        return false;
      }
      continue;
    }
    if (!ReadStream(dev, S, streams[S], layout->fetchStride[S])) {
      u32 n;
      if (DiagShouldLog(0x6050 + S, &n) && n < 4) {
        const u32 slot = dev::StreamFetchSlotOffset(S);
        EOT_WARN("[draw] stream {} unreadable: fetch {:08x} {:08x} device stride {} vfetch stride {} "
                 "decl {:#x} vs {:016x}",
                 S, dev.U32(slot), dev.U32(slot + 4), dev.U8(dev::kStreamStride0 + S) * 4u,
                 layout->fetchStride[S], dev.U32(dev::kVertexDeclaration), vs->hash);
      }
      Dropped("stream source unreadable", 0x6005);
      return false;
    }
    if (layout->streamExtent[S] > streams[S].stride) {
      u32 n;
      if (DiagShouldLog(0x6006 + S, &n))
        EOT_WARN("[draw] stream {} stride {} smaller than the declaration extent {}", S,
                 streams[S].stride, layout->streamExtent[S]);
      // Fable 2: Metal reads such attributes past each vertex (and faults).
      Dropped("stream stride smaller than the declaration", 0x6008);
      return false;
    }
  }
  for (u32 S = 0; S < 16; ++S)
    pk.strides[S] = streams[S].stride;
  // Debug (dog fur): one-shot dump of the vertex streams of the fur shells
  // (memexport output and fur parameters) and of the dog's body draw.
  if (REXCVAR_GET(fable2_debug_dump_fur)) {
    static u32 dumped = 0;  // bit 0 fur, bit 1 body
    const u32 count = geom.indexed ? static_cast<u32>(geom.indices.size()) : geom.vertexCount;
    const u32 which = vs->hash == 0x4ba0b3646633c5bfull ? 1u
                      : (vs->hash == 0x36f4dd575db99917ull && (count == 20649 || (geom.cached && geom.cached->count == 20649)))
                          ? 2u
                          : 0u;
    if (which && !(dumped & which)) {
      dumped |= which;
      for (u32 S = 0; S < 16; ++S) {
        if (!streams[S].data || !streams[S].sizeBytes)
          continue;
        const std::string path = std::format("logs/{}_s{}.bin", which == 1 ? "fur" : "body", S);
        if (FILE *f = std::fopen(path.c_str(), "wb")) {
          std::fwrite(streams[S].data, 1, streams[S].sizeBytes, f);
          std::fclose(f);
        }
        EOT_INFO("[furdump] {} stream {} stride {} bytes {} va {:#x} -> {}", which == 1 ? "fur" : "body", S,
                 streams[S].stride, streams[S].sizeBytes, streams[S].dataVa, path);
      }
    }
  }

  u32 lo = 0, hi = 0;
  RectExpansion rect;
  if (geom.rectList) {
    std::vector<u32> guest_vertices;
    if (geom.indexed) {
      guest_vertices = geom.indices;
      for (auto &v : guest_vertices)
        v = static_cast<u32>(static_cast<i32>(v) + geom.baseVertex);
    } else {
      guest_vertices.resize(geom.vertexCount);
      for (u32 i = 0; i < geom.vertexCount; ++i)
        guest_vertices[i] = geom.startVertex + i;
    }
    if (!ExpandRectList(*layout, streams, layout->streamMask, guest_vertices, rect) ||
        rect.indices.empty()) {
      Dropped("rect list expansion failed", 0x6009);
      return false;
    }
    if (!UploadBytes(rect.indices.data(), rect.indices.size() * 4, 4, &pk.index_alloc)) {
      Dropped("upload ring exhausted (indices)", 0x600A);
      return false;
    }
    pk.index_count = static_cast<u32>(rect.indices.size());
  } else if (geom.cached) {
    pk.hasCached = true;
    pk.cached = *geom.cached;
    lo = static_cast<u32>(static_cast<i32>(geom.cached->lo) + geom.baseVertex);
    hi = static_cast<u32>(static_cast<i32>(geom.cached->hi) + geom.baseVertex);
    pk.index_count = geom.cached->count;
    pk.host_base_vertex = geom.baseVertex - static_cast<i32>(lo);
  } else if (geom.indexed) {
    if (geom.indices.empty()) {
      Dropped("empty index list", 0x600B);
      return false;
    }
    lo = 0xFFFFFFFFu;
    hi = 0;
    for (u32 v : geom.indices) {
      const u32 gv = static_cast<u32>(static_cast<i32>(v) + geom.baseVertex);
      lo = std::min(lo, gv);
      hi = std::max(hi, gv);
    }
    if (!UploadBytes(geom.indices.data(), geom.indices.size() * 4, 4, &pk.index_alloc)) {
      Dropped("upload ring exhausted (indices)", 0x600A);
      return false;
    }
    pk.index_count = static_cast<u32>(geom.indices.size());
    s.perf.index_bytes += u64(pk.index_count) * 4;
    pk.host_base_vertex = geom.baseVertex - static_cast<i32>(lo);
  } else {
    lo = geom.startVertex;
    hi = geom.startVertex + geom.vertexCount - 1;
  }

  pk.vertex_id_bias = geom.rectList ? 0 : -static_cast<i32>(lo);
  // Debug (dog fur): the vertex range of the logged vertex shader's draws.
  if (const std::string &want = REXCVAR_GET(fable2_debug_vs_consts);
      want.size() >= 16 && want.compare(0, 16, std::format("{:016x}", vs->hash)) == 0) {
    static u32 logged = 0;
    if (logged++ < 12)
      EOT_INFO("[vsrange] vs {:016x} n {} vertices {}..{} base {} start {}", vs->hash,
               geom.indexed ? static_cast<u32>(geom.indices.size()) : geom.vertexCount, lo, hi, geom.baseVertex,
               geom.startVertex);
  }
  // Temporary diagnostics: the world shader's lightmap colour index range.
  if (vs->hash == 0xbcc6e2def4a3e5c2ull && Settings::DiagFrame() > 0 &&
      s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame())) {
    static u32 n = 0;
    if (n++ < 10) {
      const u32 c20 = dev.U32(dev::kVsFloatConstants + 20 * 16);
      const u32 d1 = dev.U32(dev::kFetchConstants + 8 * 92 + 4);
      EOT_INFO("[diag] lightmap idx: offset {} slot92 dwords {} endian {} base {} lo {} hi {} cached {} "
               "cachedlo {} cachedhi {}",
               std::bit_cast<float>(c20), (d1 >> 2) & 0xFFFFFF, d1 & 3, geom.baseVertex, lo, hi,
               geom.cached != nullptr, geom.cached ? geom.cached->lo : 0, geom.cached ? geom.cached->hi : 0);
    }
  }
  if (!geom.rectList) {
    for (u32 S = 0; S < 16; ++S) {
      if (!(layout->streamMask & (1u << S)) || (S == 0 && geom.stream0OverrideVa))
        continue;
      const StreamInfo &st_info = streams[S];
      const u64 first = u64(lo) * st_info.stride;
      u64 bytes = (u64(hi) - lo + 1) * st_info.stride;
      if (st_info.sizeBytes && first + bytes > st_info.sizeBytes)
        bytes = first < st_info.sizeBytes ? st_info.sizeBytes - first : 0;
      if (bytes && bytes <= kVertexMirrorMaxBytes)
        PrefetchSampleProbes(st_info.data + first, bytes);
    }
  }

  if (!pk.targets.colorCount && pk.targets.depthVa && !ps && !geom.rectList &&
      !geom.stream0OverrideVa && ShadowTileWords(pk.targets.depthWords)) {
    if (REXCVAR_GET(fable2_diag_shadow_mask)) {
      // One frame's shadow casters by shader pair and alpha handling.
      static std::unordered_map<std::string, u32> census;
      static u64 census_frame = 0;
      const u32 cc = dev.U32(dev::kColorControl);
      const std::string key = std::format("vs {:016x} ps {:016x} atest {} a2c {} kill {}", vs->hash, ps ? ps->hash : 0,
                                          (cc & 8) ? (cc & 7) : 9, (cc >> 4) & 1,
                                          ps && ps->entry ? (ps->entry->shaderFlags & 1) : 0);
      if (s.guest_frames == 3000 || s.guest_frames == 6000) {
        census_frame = s.guest_frames;
        census[key]++;
      } else if (census_frame && s.guest_frames == census_frame + 1) {
        for (auto &[k, n] : census)
          EOT_INFO("[shadowcasters] frame {} {}: {} draws", census_frame, k, n);
        census.clear();
        census_frame = 0;
      }
    }
    if (!REXCVAR_GET(fable2_debug_shadow_skip_vs).empty() &&
        REXCVAR_GET(fable2_debug_shadow_skip_vs).find(std::format("{:016x}", vs->hash)) != std::string::npos) {
      Dropped("shadow caster skipped (fable2_debug_shadow_skip_vs)", 0x6302);
      return false;
    }
    if (REXCVAR_GET(fable2_debug_shadow_skip_tess) && geom.tessSegments) {
      Dropped("landscape patch in a shadow pass skipped (fable2_debug_shadow_skip_tess)", 0x6301);
      return false;
    }
    NoteShadowCaster(s, dev, *vs, streams, lo, hi);
    if (REXCVAR_GET(fable2_diag_shadow_mask)) {
      // Shadow casters the game submits per frame (capture side), one line a frame.
      static u64 frame = ~0ull;
      static u32 draws = 0;
      static u64 verts = 0;
      if (frame != s.guest_frames) {
        if (frame != ~0ull)
          EOT_INFO("[casters] frame {} draws {} verts {}", frame, draws, verts);
        frame = s.guest_frames;
        draws = 0;
        verts = 0;
      }
      ++draws;
      verts += geom.indexed ? (geom.cached ? geom.cached->count : geom.indices.size()) : geom.vertexCount;
    }
  }

  if (pk.targets.colorCount && !geom.rectList && REXCVAR_GET(fable2_pop_counter)) {
    // Pop counter: a colour draw (same object: stream, index range, shaders)
    // the game left out for 1-3 frames and then drew again. Logged per second
    // with [perf] as [pops]; objects entering or leaving the view do not count.
    const StreamInfo &s0 = streams[0];
    u64 key = 0x51ED270B0E9A4C3Dull;
    auto mix = [&key](u64 v) { key ^= v + 0x9E3779B97F4A7C15ull + (key << 6) + (key >> 2); };
    mix(geom.stream0OverrideVa ? geom.stream0OverrideVa : s0.objectVa);
    mix(s0.dataVa);
    mix((u64(lo) << 32) | hi);
    mix(static_cast<u32>(geom.baseVertex));
    mix((u64(vs_va) << 32) | ps_va);
    mix(pk.indexed ? pk.index_count : pk.vertexCount);
    NotePopCandidate(key, s.guest_frames);
  }

  pk.velocity = false;
  if (Settings::MotionVectors() && ps && !geom.rectList && vs->entry && ps->entry &&
      vs->entry->usesFloatConstants && VelocityCandidate(dev, pk.targets) &&
      VsVariantFor(vs->entry, ps->entry, false, true) == VsVariant::Velocity) {
    const StreamInfo &s0 = streams[0];
    u64 key = 0x51ED270B0E9A4C3Dull;
    auto mix = [&key](u64 v) { key ^= v + 0x9E3779B97F4A7C15ull + (key << 6) + (key >> 2); };
    mix(geom.stream0OverrideVa ? geom.stream0OverrideVa : s0.objectVa);
    mix(s0.dataVa);
    mix((u64(lo) << 32) | hi);
    mix(static_cast<u32>(geom.baseVertex));
    mix((u64(vs_va) << 32) | ps_va);
    mix(pk.indexed ? pk.index_count : pk.vertexCount);
    const u32 regs = std::clamp(vs->floatConstantRegs, 16u, 256u);
    if (const u8 *guest_file = dev.Bytes(dev::kVsFloatConstants, regs * 16)) {
      UploadAlloc block;
      if (velocity::PrepareDraw(s, key, guest_file, regs, &block)) {
        pk.vs_consts = block;
        pk.velocity = true;
      }
    }
    lap(s.perf.const_float_ms);
  }

  if ((!pk.velocity && !UploadFloatFile(s, dev, dev::kVsFloatConstants, 0, vs->floatConstantRegs,
                                        s.vs_float_constants_stale, &pk.vs_consts)) ||
      !UploadFloatFile(s, dev, dev::kPsFloatConstants, 1, ps ? ps->floatConstantRegs : 16u,
                       s.ps_float_constants_stale, &pk.ps_consts)) {
    Dropped("constant upload failed", 0x6010);
    return false;
  }
  if (!pk.velocity)
    s.vs_float_constants_stale = 0;
  s.ps_float_constants_stale = 0;
  // Temporary diagnostics: the main wall material's UV matrices as uploaded.
  if (ps && (ps->hash == 0x9a193febb26723f4ull || ps->hash == 0xb950f34c5012d6f7ull ||
             ps->hash == 0x7c1ef4455de18d9bull) && Settings::DiagFrame() > 0 &&
      s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame()) && pk.ps_consts.cpu) {
    const float *c = reinterpret_cast<const float *>(pk.ps_consts.cpu);
    static u32 n = 0;
    if (n++ < 12)
      EOT_INFO("[diag] wall ps {:016x} c8=({:.4g},{:.4g},{:.4g},{:.4g}) c9=({:.4g},{:.4g},{:.4g},{:.4g}) "
               "c10=({:.4g},{:.4g},{:.4g},{:.4g}) regs {}",
               ps->hash, c[32], c[33], c[34], c[35], c[36], c[37], c[38], c[39], c[40], c[41], c[42],
               c[43], ps->floatConstantRegs);
    if (n <= 12)
      EOT_INFO("[diag] wall ps {:016x} ambient c20=({:.3g},{:.3g},{:.3g},{:.3g}) lightmap c21=({:.3g},{:.3g},{:.3g},{:.3g}) "
               "darken c22=({:.3g},{:.3g},{:.3g}) mat c27=({:.3g},{:.3g},{:.3g},{:.3g}) tint c49=({:.3g},{:.3g},{:.3g}) "
               "sun c80=({:.3g},{:.3g},{:.3g}) c87=({:.3g},{:.3g},{:.3g})",
               ps->hash, c[80], c[81], c[82], c[83], c[84], c[85], c[86], c[87], c[88], c[89], c[90],
               c[108], c[109], c[110], c[111], c[196], c[197], c[198], c[320], c[321], c[322], c[348],
               c[349], c[350]);
  }
  lap(s.perf.const_float_ms);

  u32 max_slot = 0;
  for (u32 S = 0; S < 16; ++S)
    if (layout->streamMask & (1u << S))
      max_slot = S;
  pk.max_slot = max_slot;
  const u32 prefix_mask = (1u << (max_slot + 1)) - 1;
  const bool need_zero = layout->needsSyntheticSlot || layout->streamMask != prefix_mask;
  if (need_zero && !UploadZeroBuffer(s, &pk.zero)) {
    Dropped("upload ring exhausted (zero buffer)", 0x600C);
    return false;
  }
  for (u32 S = 0; S <= max_slot; ++S) {
    pk.slots[S] = plume::RenderInputSlot(S, streams[S].stride);
    if (!(layout->streamMask & (1u << S))) {
      pk.views[S] = plume::RenderVertexBufferView(
          plume::RenderBufferReference(pk.zero.buffer, pk.zero.offset), 4096);
      pk.slots[S] = plume::RenderInputSlot(S, 0);
      continue;
    }
    const StreamInfo &st_info = streams[S];
    UploadAlloc va;
    if (geom.rectList) {
      if (!UploadBytes(rect.vertices[S].data(), rect.vertices[S].size(), 16, &va)) {
        Dropped("upload ring exhausted (vertices)", 0x600D);
        return false;
      }
      pk.views[S] = plume::RenderVertexBufferView(
          plume::RenderBufferReference(va.buffer, va.offset), static_cast<u32>(va.size));
      continue;
    }
    const u64 first = u64(lo) * st_info.stride;
    u64 bytes = (u64(hi) - lo + 1) * st_info.stride;
    if (st_info.sizeBytes && first + bytes > st_info.sizeBytes) {
      if (first >= st_info.sizeBytes) {
        u32 n;
        if (DiagShouldLog(0x6070 + S, &n) && n < 6)
          EOT_WARN("[draw] stream {} range: indices {}..{} stride {} (device {} vfetch {}) size {} "
                   "base {} indexed {} decl {:#x} vs {:016x}",
                   S, lo, hi, st_info.stride, dev.U8(dev::kStreamStride0 + S) * 4u,
                   layout->fetchStride[S], st_info.sizeBytes, geom.baseVertex, geom.indexed,
                   dev.U32(dev::kVertexDeclaration), vs->hash);
      }
      // Fable 2: the GPU fetches past the fetch constant's size (the vertices
      // are in memory); the whole requested range is read, not dropped or cut.
    }
    // Fable 2: Metal faults (device lost) on vertex fetches outside the bound
    // range. Bind past the last vertex by the layout's widest element extent
    // so its whole footprint is covered (guest memory is mapped behind it).
    bytes += std::max<u64>(layout->streamExtent[S], st_info.stride) + 64;
    if (bytes > 64ull * 1024 * 1024) {
      Dropped("vertex range implausibly large", 0x600F);
      return false;
    }
    const bool morph = (layout->morphStreams >> S) & 1u;
    if (morph && REXCVAR_GET(fable2_diag_morph))
      NoteMorphStream(s, st_info, first, bytes);
    if (const VertexMirror *mirror = morph ? nullptr : GetVertexMirror(s, st_info, first, bytes)) {
      auto &pool = vertex_mirrors().pool;
      const u64 epoch = pool.flushEpoch.load(std::memory_order_acquire);
      if (mirror->residentEpoch != epoch || !mirror->resident) {
        mirror->resident = PoolResidentBuffer(s, pool, mirror->buffer, mirror->offset, bytes,
                                              &mirror->residentCapacity);
        mirror->residentEpoch = epoch;
      }
      if (max_slot == 0 && mirror->residentCapacity && mirror->offset % st_info.stride == 0 &&
          mirror->residentCapacity <= 0xFFFFFFFFull) {
        pk.views[S] = plume::RenderVertexBufferView(
            plume::RenderBufferReference(mirror->resident, 0),
            static_cast<u32>(mirror->residentCapacity));
        pk.host_base_vertex += static_cast<i32>(mirror->offset / st_info.stride);
        pk.vertex_id_bias += static_cast<i32>(mirror->offset / st_info.stride);
        continue;
      }
      pk.views[S] = plume::RenderVertexBufferView(
          plume::RenderBufferReference(mirror->resident, mirror->offset),
          static_cast<u32>(bytes));
      continue;
    }
    if (!UploadAllocate(bytes, 16, &va)) {
      Dropped("upload ring exhausted (vertices)", 0x600D);
      return false;
    }
    {
      PerfScope copy_scope(s.perf.vertex_copy_ms);
      CopyVertexBytes(va.cpu, st_info.data + first, static_cast<u32>(bytes));
    }
    s.perf.vertex_bytes += bytes;
    pk.views[S] = plume::RenderVertexBufferView(plume::RenderBufferReference(va.buffer, va.offset),
                                                static_cast<u32>(bytes));
  }
  lap(s.perf.stream_ms);

  if (geom.rectList && streams[0].data && streams[0].stride >= 8 && Settings::DiagFrame() > 0 &&
      s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame())) {
    std::string verts;
    const u32 n_show = std::min<u32>(geom.vertexCount, 12u);
    for (u32 v = 0; v < n_show; ++v) {
      const u8 *p = streams[0].data + u64(v) * streams[0].stride;
      float f[4] = {0, 0, 0, 0};
      const u32 words = std::min<u32>(4u, streams[0].stride / 4u);
      for (u32 c = 0; c < words; ++c) {
        u32 raw;
        std::memcpy(&raw, p + 4 * c, 4);
        raw = (raw >> 24) | ((raw >> 8) & 0xFF00u) | ((raw << 8) & 0xFF0000u) | (raw << 24);
        std::memcpy(&f[c], &raw, 4);
      }
      verts += std::format(" [{:.3f} {:.3f} {:.3f} {:.3f}]", f[0], f[1], f[2], f[3]);
    }
    EOT_INFO("[diag]   rect verts (next draw){}", verts);
  }
  return true;
}

struct SurfaceSignature {
  const void *host = nullptr, *single = nullptr, *redirect = nullptr, *redirectHost = nullptr;
  u32 samples = 0;
  i32 redirectX = 0, redirectY = 0;
  u8 content = 0;
  bool contentInSingle = false, imagesAgree = false, resolvedSinceDraw = false;
  bool singleDirty = false, singleStale = false;
  bool operator==(const SurfaceSignature &o) const {
    return host == o.host && single == o.single && redirect == o.redirect &&
           redirectHost == o.redirectHost && samples == o.samples && redirectX == o.redirectX &&
           redirectY == o.redirectY && content == o.content &&
           contentInSingle == o.contentInSingle && imagesAgree == o.imagesAgree &&
           resolvedSinceDraw == o.resolvedSinceDraw && singleDirty == o.singleDirty &&
           singleStale == o.singleStale;
  }
};

SurfaceSignature SignSurface(const GuestSurface &surf) {
  SurfaceSignature sig{};
  sig.host = surf.host.texture.get();
  sig.single = surf.single.texture.get();
  sig.redirect = surf.redirectMirror.get();
  sig.redirectHost = surf.redirectMirror ? surf.redirectMirror->host.texture.get() : nullptr;
  sig.samples = surf.host.sampleCount;
  sig.redirectX = surf.redirectX;
  sig.redirectY = surf.redirectY;
  sig.content = static_cast<u8>(surf.content);
  sig.contentInSingle = surf.contentInSingle;
  sig.imagesAgree = surf.imagesAgree;
  sig.resolvedSinceDraw = surf.resolvedSinceDraw;
  sig.singleDirty = surf.singleDirty;
  sig.singleStale = surf.singleSerial != surf.writeSerial;
  return sig;
}

// Debug lists "hash[:count],...": does this draw match?
bool DrawMatchesList(const std::string &list, u64 hash, u32 count) {
  for (size_t i = 0; i < list.size();) {
    size_t j = list.find(',', i);
    if (j == std::string::npos)
      j = list.size();
    const std::string item = list.substr(i, j - i);
    const size_t colon = item.find(':');
    const u64 h = std::strtoull(item.substr(0, colon).c_str(), nullptr, 16);
    const u32 n = colon == std::string::npos ? 0u : static_cast<u32>(std::strtoul(item.c_str() + colon + 1, nullptr, 10));
    if (h == hash && (!n || n == count))
      return true;
    i = j + 1;
  }
  return false;
}

void ReplayDraw(VideoState &s, const DrawPacket &pk) {
  static u32 replay_count = 0;
  const bool timed = (++replay_count & 15u) == 0;
  u64 lap_t0 = timed ? PerfNow() : 0;
  auto lap = [&](f64 &acc) {
    if (!timed)
      return;
    const u64 t1 = PerfNow();
    acc += 16.0 * static_cast<f64>(t1 - lap_t0) * PerfMsPerTick();
    lap_t0 = t1;
  };
  DeviceView dev = Device(pk.device_va);
  dev.snapshot = pk.window.image;
  dev.snapshotSize = kDeviceSnapshotBytes;
  dev.pages = DeviceWindow::PageTable();
  GuestShader *vs = pk.vs;
  GuestShader *ps = pk.ps;
  const InputLayout *layout = pk.layout;
  BeginCommandList(s);
  if (!s.command_list_open)
    return;

  constexpr u32 kSharedTextureBytes = offsetof(SharedConstants, booleans);
  static_assert(kSharedTextureBytes == 20 * 16);
  struct ReplayMemo {
    bool valid = false;
    GuestShader *vs = nullptr, *ps = nullptr;
    const InputLayout *layout = nullptr;
    const HostTexture *images[5] = {};
    u32 colorCount = 0, samples = 0;
    i32 offsetX = 0, offsetY = 0;
    plume::RenderPrimitiveTopology topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    bool rectList = false;
    u32 strides[16] = {};
    u8 state[DeviceWindow::kBlocks[2].size];
    u8 consts[DeviceWindow::kBlocks[1].size];
    PipelineState st;
    u32 spec = 0;
    bool a2c = false;
    plume::RenderPipeline *pipeline = nullptr;
    ViewportInfo vp;
    u32 jitterIndex = 0;
    velocity::Target *velocity = nullptr;
    SharedConstants fixed;
    UploadAlloc shared;
    u64 sharedRing = ~0ull;
    u8 sharedTextures[kSharedTextureBytes] = {};
    u32 sharedSint = 0, sharedBiased = 0;
    i32 sharedBaseVertex = 0;
    float sharedPixelScale = 1.0f;
    u32 sharedPointDraw = 0;
    float sharedTexScale[16] = {};
    float sharedTileKeep = 1.0f;
    u32 sharedTessSegments = 0, sharedTessFirst = 0;
    u64 sharedDebug = 0;
    float sharedPointViewport[4] = {};
    u32 sharedSamplerVariants[32] = {};
    u32 sharedPullFlags = 0;
    u8 sharedVsTextures[80] = {};
  };
  constexpr u32 kReplayMemoWays = 8;
  static ReplayMemo memos[kReplayMemoWays];
  static u32 memo_next = 0;
  const u8 *state_block = pk.window.image + DeviceWindow::kBlocks[2].base;
  const u8 *consts_block = pk.window.image + DeviceWindow::kBlocks[1].base;
  const u32 jitter_index = taa::JitterIndex(s, pk.taaSkip, pk.rectList);
  float jitter_x = 0.0f, jitter_y = 0.0f;
  if (!pk.rectList)
    taa::FrameJitter(s, pk.taaSkip, pk.rectList, &jitter_x, &jitter_y);
  ReplayMemo *memo = nullptr;
  // The emulated Z pass draws with colour writes off: it must neither reuse nor
  // leave behind the memoised state (a scene draw that hit a Z-pass memo drew
  // depth only: black buildings).
  for (ReplayMemo &m : memos) {
    if (s.depth_prerun)
      break;
    if (m.valid && m.vs == vs && m.ps == ps && m.layout == layout && m.topology == pk.topology &&
        m.jitterIndex == jitter_index &&
        m.rectList == pk.rectList && std::memcmp(m.strides, pk.strides, sizeof(pk.strides)) == 0 &&
        std::memcmp(m.state, state_block, sizeof(m.state)) == 0 &&
        std::memcmp(m.consts, consts_block, sizeof(m.consts)) == 0) {
      memo = &m;
      break;
    }
  }
  const bool state_hit = memo != nullptr;

  struct ClsKey {
    u32 dc = 0, srm = 0, cc = 0, bc = 0;
    bool hasPs = false, rect = false;
    bool operator==(const ClsKey &o) const {
      return dc == o.dc && srm == o.srm && cc == o.cc && bc == o.bc && hasPs == o.hasPs &&
             rect == o.rect;
    }
  };
  const ClsKey cls_key{dev.U32(dev::kDepthControl), dev.U32(dev::kStencilRefMask),
                       dev.U32(dev::kColorControl), dev.U32(dev::kBlendControl0), ps != nullptr,
                       pk.rectList};
  struct TargetMemo {
    bool valid = false;
    ClsKey key;
    TargetWords words;
    u64 surfaceGeneration = 0;
    SurfaceSignature sigs[5];
    Targets targets;
    DrawClass cls;
  };
  static TargetMemo tmemo;
  Targets targets;
  DrawClass cls;
  bool target_hit = tmemo.valid && tmemo.key == cls_key &&
                    tmemo.surfaceGeneration == s.surface_generation &&
                    std::memcmp(&tmemo.words, &pk.targets, sizeof(TargetWords)) == 0;
  if (target_hit) {
    const Targets &t = tmemo.targets;
    for (u32 i = 0; target_hit && i < t.colorCount; ++i) {
      const SurfaceSignature sig = SignSurface(*t.color[i]);
      target_hit = sig == tmemo.sigs[i];
    }
    if (target_hit && t.depth) {
      const SurfaceSignature sig = SignSurface(*t.depth);
      target_hit = sig == tmemo.sigs[4];
    }
  }
  if (target_hit) {
    targets = tmemo.targets;
    cls = tmemo.cls;
    s.perf.target_memo_hits++;
  } else {
    if (tmemo.valid) {
      if (tmemo.surfaceGeneration != s.surface_generation)
        s.perf.target_memo_miss_gen++;
      else if (!(tmemo.key == cls_key) ||
               std::memcmp(&tmemo.words, &pk.targets, sizeof(TargetWords)) != 0)
        s.perf.target_memo_miss_words++;
      else {
        s.perf.target_memo_miss_sig++;
        static u32 logged = 0;
        if (logged < 12) {
          const Targets &t = tmemo.targets;
          for (u32 i = 0; i < 5; ++i) {
            const GuestSurface *surf = i < 4 ? (i < t.colorCount ? t.color[i] : nullptr) : t.depth;
            if (!surf)
              continue;
            const SurfaceSignature now = SignSurface(*surf), was = tmemo.sigs[i];
            if (now == was)
              continue;
            ++logged;
            EOT_DEBUG("[draw] target memo miss: surface {} ({:#x}) host {}/{} single {}/{} redirect {}/{} "
                      "samples {}/{} content {}/{} cis {}/{} agree {}/{} rsd {}/{} dirty {}/{} stale {}/{}",
                      i, surf->va, was.host, now.host, was.single, now.single, was.redirect,
                      now.redirect, was.samples, now.samples, was.content, now.content,
                      was.contentInSingle, now.contentInSingle, was.imagesAgree, now.imagesAgree,
                      was.resolvedSinceDraw, now.resolvedSinceDraw, was.singleDirty, now.singleDirty,
                      was.singleStale, now.singleStale);
          }
        }
      }
    }
    tmemo.valid = false;
    if (!ResolveTargetsFromWords(s, pk.targets, targets)) {
      Dropped("no render target or depth surface bound", 0x6003);
      return;
    }
    cls = ClassifyDraw(dev, targets, ps != nullptr, pk.rectList);
    // Temporary (missing ground): the memory-export vertex shaders.
    if (vs && (vs->hash == 0x5243696187782727ull || vs->hash == 0xac0c424248a6b1adull)) {
      static u32 memexport_logs = 0;
      if (memexport_logs++ < 40) {
        auto vc = [&](u32 c, u32 i) { return dev.U32(dev::kVsFloatConstants + c * 16 + i * 4); };
        EOT_INFO("[memexport] vs {:016x} prim {} n {} rt0 {:#x} ps {:016x} c8=({:08x} {:08x} {:08x} {:08x}) "
                 "c46=({:08x} {:08x}) c47=({:08x} {:08x})",
                 vs->hash, pk.prim, pk.indexed ? pk.index_count : pk.vertexCount,
                 targets.colorCount ? targets.color[0]->va : 0, ps ? ps->hash : 0, vc(8, 0), vc(8, 1),
                 vc(8, 2), vc(8, 3), vc(46, 0), vc(46, 1), vc(47, 0), vc(47, 1));
      }
    }
    if (!REXCVAR_GET(fable2_skip_vs).empty() &&
        DrawMatchesList(REXCVAR_GET(fable2_skip_vs), vs->hash, pk.indexed ? pk.index_count : pk.vertexCount)) {
      Dropped("vertex shader skipped (fable2_skip_vs)", 0x6033);
      return;
    }
    if (ps && !REXCVAR_GET(fable2_skip_ps).empty()) {
      static std::string parsed_from;
      static std::vector<u64> skip;
      const std::string &list = REXCVAR_GET(fable2_skip_ps);
      if (list != parsed_from) {
        parsed_from = list;
        skip.clear();
        for (size_t i = 0; i < list.size();) {
          size_t j = list.find(',', i);
          if (j == std::string::npos)
            j = list.size();
          skip.push_back(std::strtoull(list.substr(i, j - i).c_str(), nullptr, 16));
          i = j + 1;
        }
      }
      if (std::find(skip.begin(), skip.end(), ps->hash) != skip.end()) {
        Dropped("pixel shader skipped (fable2_skip_ps)", 0x6032);
        return;
      }
    }
    if (REXCVAR_GET(fable2_diag_interp) && ps && !s.depth_prerun && vs->entry && ps->entry) {
      // Interpolants the vertex shader writes vs the ones the pixel shader reads,
      // weighted by vertices.
      static u64 draws = 0, verts = 0, written = 0, read = 0;
      static std::unordered_map<u64, std::pair<u64, u32>> excess;  // vs^ps -> verts, unused bits
      const u32 vm = vs->entry->interpolantMask, pm = ps->entry->interpolantMask & vm;
      const u64 n = pk.indexed ? pk.index_count : pk.vertexCount;
      ++draws;
      verts += n;
      written += n * std::popcount(vm);
      read += n * std::popcount(pm);
      {
        auto &e = excess[vs->hash];
        e.first += n;
        e.second = std::popcount(vm);
      }
      if (draws % 40000 == 0) {
        EOT_INFO("[interp] {} draws {} verts: written {:.2f} read {:.2f} per vertex", draws, verts,
                 double(written) / double(verts), double(read) / double(verts));
        std::vector<std::pair<u64, std::pair<u64, u32>>> v(excess.begin(), excess.end());
        std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second.first > b.second.first; });
        for (size_t i = 0; i < std::min<size_t>(v.size(), 10); ++i)
          EOT_INFO("[interp]   vs {:016x}: {} verts, {} interpolants", v[i].first, v[i].second.first,
                   v[i].second.second);
      }
    }
    // Occlusion queries count exactly these draws (depth-tested, nothing written).
    if (cls.nullPs && !cls.depthWrite && !cls.stencil && !QueryActiveLocked()) {
      if (Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame()))
        EOT_INFO("[diag] skip prim {} n={} dc={:#x} rt0={:#x} ds={:#x} vs={:016x}: no pixel shader, no "
                 "depth or stencil write",
                 pk.prim, pk.indexed ? pk.index_count : pk.vertexCount, dev.U32(dev::kDepthControl),
                 targets.colorCount ? targets.color[0]->va : 0, targets.depth ? targets.depth->va : 0,
                 vs->hash);
      s.perf.draws--;
      s.perf.draws_skipped++;
      return;
    }
    SurfaceSignature sigs[5];
    for (u32 i = 0; i < targets.colorCount; ++i)
      sigs[i] = SignSurface(*targets.color[i]);
    if (targets.depth)
      sigs[4] = SignSurface(*targets.depth);
    if (!SelectTargetImages(s, targets, cls)) {
      Dropped("surface image unavailable", 0x6016);
      return;
    }
    tmemo.key = cls_key;
    tmemo.words = pk.targets;
    tmemo.surfaceGeneration = s.surface_generation;
    std::memcpy(tmemo.sigs, sigs, sizeof(sigs));
    tmemo.targets = targets;
    tmemo.cls = cls;
    tmemo.valid = true;
  }
  lap(s.perf.replay_targets_ms);
  // Emulated Z pass on a depth-only framebuffer: the colour targets were bound
  // only to be masked (loaded and stored for nothing), and the pixel shader
  // runs only where it can drop pixels (kill, depth output, alpha test, alpha
  // to coverage). Same depth image and sample count as before (selected above
  // with the colour targets), so the saved and restored depth stays the same.
  if (s.depth_prerun && targets.depth && targets.depthImage && REXCVAR_GET(fable2_zpass_depth_only)) {
    const u32 cc = dev.U32(dev::kColorControl);
    const bool alpha_test = (cc & (1u << 3)) && (cc & 7) != 7;
    const bool a2c = (cc & (1u << 4)) != 0;
    const bool ps_drops = ps && (!ps->entry || (ps->entry->shaderFlags & 1u) || alpha_test || a2c);
    targets.colorCount = 0;
    for (u32 i = 0; i < 4; ++i) {
      targets.color[i] = nullptr;
      targets.colorImage[i] = nullptr;
    }
    targets.velocity = nullptr;
    targets.writesColor = false;
    if (!ps_drops) {
      ps = nullptr;
      cls.nullPs = true;
    }
  }
  // Fable 2: the half-res effects pass (sub_821CB750 via sub_82278F38) starts
  // with a full-screen quad at depth 0.5 (vs a108, copy shader ps 2ac2, colour
  // writes off) drawn with the scene's resolved depth bound at sampler 13.
  // Drawn as a plane it hid everything past 0.2 m from the half-res effects
  // (the quest trail vanished when it ran into the screen), and without it the
  // trail showed through walls and the hero. Copy the bound depth instead, so
  // the effects test against the scene. Only where the bound depth is twice
  // the target's size (the half-res pass): the occlusion query passes keep
  // the plane.
  if (REXCVAR_GET(fable2_plane_scene_depth) && vs->hash == 0xa108f279e72a396bull && ps &&
      ps->hash == 0x2ac2d68dfe608627ull && !targets.colorCount && targets.depth && targets.depthImage &&
      !targets.offsetX && !targets.offsetY) {
    // The texture as the draw saw it (its captured fetch constant), looked up
    // without creating or replacing anything: the header in guest memory may
    // already describe a later image, and GetGuestTexture would then discard
    // the resolved image queued draws still sample.
    const u32 tex_va = dev.U32(dev::kTextureObject0 + 4 * 13);
    const u8 *fc_raw = dev.Bytes(dev::kFetchConstants + 24 * 13, 24);
    GuestTexture *scene = nullptr;
    if (tex_va && fc_raw && (fc_raw[3] & 3) == 2) {
      u32 fc[6];
      for (u32 d = 0; d < 6; ++d)
        fc[d] = rex::memory::load_and_swap<u32>(fc_raw + 4 * d);
      // Headers hold D3D's virtual base, fetch constants the GPU's physical
      // one (0xE0000000 and up is physical memory 0x1000 higher).
      auto physical = [](u32 base) {
        return base >= 0xE0000000u ? (base & 0x1FFFFFFFu) + 0x1000u : base & 0x1FFFFFFFu;
      };
      auto same_image = [&](const GuestTexture &t) {
        return physical(t.fetch[1] & ~0xFFFu) == (fc[1] & 0x1FFFF000u) &&
               (t.fetch[1] & 0x3F) == (fc[1] & 0x3F) && t.fetch[2] == fc[2];
      };
      auto usable = [&](const GuestTexture &t) {
        return same_image(t) && t.resolveOwned && t.host.valid() && t.host.isDepth;
      };
      // The header the plane binds can be a second one for the resolved image.
      auto it = s.textures.find(tex_va);
      if (it != s.textures.end() && it->second && usable(*it->second))
        scene = it->second.get();
      for (auto i = s.textures.begin(); !scene && i != s.textures.end(); ++i)
        if (i->second && usable(*i->second))
          scene = i->second.get();
    }
    // Half-res effects pass: scene / 2. The 280x180 occlusion query pass
    // (scene / 4) tests its boxes against this depth too; with the flat
    // plane, buildings past it were culled and popped while the camera turned.
    const bool half = scene && scene->width == 2 * targets.width && scene->height == 2 * targets.height;
    const bool quarter = scene && REXCVAR_GET(fable2_query_scene_depth) && scene->width == 4 * targets.width &&
                         scene->height == 4 * targets.height;
    if (scene && !half && !quarter)
      scene = nullptr;
    if (scene && scene->aliasPending)
      FlushAliasCopyForDraw(s, *scene);
    if (scene && SurfaceDepthFromTexture(s, *targets.depth, *targets.depthImage, scene->host)) {
      NoteSurfaceDrawn(*targets.depth, *targets.depthImage, true, true);
      return;
    }
  }

  if (pk.velocity && Settings::MotionVectors() && targets.colorCount == 1 && targets.depth &&
      targets.colorImage[0] && !cls.rect)
    targets.velocity = velocity::CurrentFor(s, *targets.depth, targets.colorImage[0]->width,
                                            targets.colorImage[0]->height, targets.samples);

  bool memo_hit = state_hit && memo->colorCount == targets.colorCount &&
                  memo->samples == targets.samples && memo->offsetX == targets.offsetX &&
                  memo->offsetY == targets.offsetY && memo->images[4] == targets.depthImage &&
                  memo->velocity == targets.velocity;
  for (u32 i = 0; memo_hit && i < targets.colorCount; ++i)
    memo_hit = memo->images[i] == targets.colorImage[i];
  const bool tess_no_blend = pk.tess_segments && REXCVAR_GET(fable2_tess_no_blend);
  memo_hit = memo_hit && !tess_no_blend;

  PipelineState st;
  u32 spec = 0;
  bool a2c = false;
  plume::RenderPipeline *pipeline = nullptr;
  ViewportInfo vp;
  static SharedConstants sc;
  if (memo_hit) {
    st = memo->st;
    spec = memo->spec;
    a2c = memo->a2c;
    pipeline = memo->pipeline;
    vp = memo->vp;
    sc = memo->fixed;
    s.perf.replay_memo_hits++;
  } else {
    sc.biasedTextures = 0;
    ZeroPipelineState(st);
    spec = layout->spec;
    FillPipelineState(dev, targets, st, &spec, &a2c);
    if (tess_no_blend)
      st.blend[0].blendEnabled = false;
    if (!REXCVAR_GET(fable2_debug_opaque_vs).empty() &&
        DrawMatchesList(REXCVAR_GET(fable2_debug_opaque_vs), vs->hash, pk.indexed ? pk.index_count : pk.vertexCount))
      st.blend[0].blendEnabled = false;
    if (!REXCVAR_GET(fable2_debug_no_depth_vs).empty() &&
        DrawMatchesList(REXCVAR_GET(fable2_debug_no_depth_vs), vs->hash, pk.indexed ? pk.index_count : pk.vertexCount)) {
      st.depthEnable = false;
      st.depthWrite = false;
    }
    spec |= layout->spec;
    const VsVariant variant = VsVariantFor(vs->entry, ps ? ps->entry : nullptr, ps == nullptr,
                                           targets.velocity != nullptr);
    if (targets.velocity && variant != VsVariant::Velocity)
      targets.velocity = nullptr;
    st.vs = ResolveHostShader(s, *vs, spec, variant);
    st.ps = ps ? ResolveHostShader(s, *ps, spec,
                                   variant == VsVariant::Velocity ? VsVariant::Velocity
                                                                  : VsVariant::Trimmed)
               : nullptr;
    if (targets.velocity) {
      const u32 v = st.rtCount;
      st.rtFormats[v] = velocity::kFormat;
      st.blend[v] = plume::RenderBlendDesc::Copy();
      st.rtCount = v + 1;
      st.velocity = true;
      static u64 logged_frame = ~0ull;
      if (Settings::DiagVerbosity() >= 2 && logged_frame != s.guest_frames / 600) {
        logged_frame = s.guest_frames / 600;
        EOT_DEBUG("[velocity] draw vs {:016x} ps {:016x} variant {} rt0 fmt {} blend {} mask {:#x} "
                  "rt1 fmt {} samples {} depth write {} spec {:#x}",
                  vs->hash, ps ? ps->hash : 0ull, static_cast<u32>(variant),
                  static_cast<u32>(st.rtFormats[0]), st.blend[0].blendEnabled,
                  st.blend[0].renderTargetWriteMask, static_cast<u32>(st.rtFormats[1]),
                  st.sampleCount, st.depthWrite, spec);
      }
    }
    if (!st.vs || (ps && !st.ps)) {
      Dropped("host shader unavailable (link failure)", 0x6007);
      return;
    }
    st.layout = layout;
    st.vsHash = vs->hash;
    st.psHash = ps ? ps->hash : 0;
    if (!targets.colorCount && targets.depth) {
      struct Snap {
        u32 mode, cache204, cache208, vtx, vte, win;
        float fs, fo, bs, bo, xs, xo, ys, yo;
      };
      static Snap last{};
      static u32 lines = 0;
      const Snap now{dev.U32(dev::kModeControl) & 0x1800u, mem::load<u32>(0x82496F7Cu + 204u * 4u),
                     mem::load<u32>(0x82496F7Cu + 208u * 4u), dev.U32(dev::kVtxControl),
                     dev.U32(dev::kVteControl), dev.U32(dev::kWindowOffset),
                     dev.F32(dev::kPolyOffsetFrontScale), dev.F32(dev::kPolyOffsetFrontOffset),
                     dev.F32(dev::kPolyOffsetBackScale), dev.F32(dev::kPolyOffsetBackOffset),
                     dev.F32(dev::kVportXScale), dev.F32(dev::kVportXOffset),
                     dev.F32(dev::kVportYScale), dev.F32(dev::kVportYOffset)};
      if (std::memcmp(&now, &last, sizeof(now)) != 0 && lines < 64) {
        last = now;
        ++lines;
        EOT_DEBUG("[shadow] caster draw: mode bits {:#x} front scale {:g} offset {:g} back scale {:g} "
                 "offset {:g} | cache 204 {:g} 208 {:g} -> host bias {} slope {:g} (vs {:016x}) | "
                 "vport x {:g}/{:g} y {:g}/{:g} vte {:#x} vtx {:#x} win {:#x} scale {:.4f}",
                 now.mode, now.fs, now.fo, now.bs, now.bo, std::bit_cast<float>(now.cache204),
                 std::bit_cast<float>(now.cache208), st.depthBias,
                 st.slopeScaledDepthBias * st.targetScale, st.vsHash, now.xs, now.xo, now.ys,
                 now.yo, now.vte, now.vtx, now.win, targets.scale);
      }
    }
    st.layoutKey = layout->key;
    st.spec = spec;
    for (u32 S = 0; S < 16; ++S)
      st.strides[S] = pk.strides[S];
    st.topology = pk.topology;
    if (pk.rectList)
      st.cull = plume::RenderCullMode::NONE;
    CanonicalizePipelineState(st,
                              (vs->entry ? vs->entry->specConstantsMask : 0u) |
                                  (ps && ps->entry ? ps->entry->specConstantsMask : 0u),
                              layout->streamMask);
    static PipelineState last_state{};
    static plume::RenderPipeline *last_pipeline = nullptr;
    pipeline = last_pipeline;
    if (!pipeline || std::memcmp(reinterpret_cast<const u8 *>(&st) + kPipelineKeyOffset,
                                 reinterpret_cast<const u8 *>(&last_state) + kPipelineKeyOffset,
                                 sizeof(st) - kPipelineKeyOffset) != 0) {
      pipeline = GetOrCreatePipeline(s, st);
      if (pipeline) {
        last_state = st;
        last_pipeline = pipeline;
      }
    }
    if (!pipeline) {
      Dropped("pipeline not built (async build pending, or creation failed)", 0x6008);
      return;
    }

    vp = ComputeViewport(dev, targets, jitter_x, jitter_y);
    // Debug (dog coat placement): draws into 512x512 colour targets.
    if (REXCVAR_GET(fable2_debug_hero_composite) && targets.colorCount && targets.width == 512 &&
        targets.height == 512) {
      static u32 logged = 0;
      if (logged++ < 400)
        EOT_INFO("[herodraw] vs {:016x} ps {:016x} n {} rt0 {:#x} off {},{} scale {} vport {} {} {} {} vte {:#x} "
                 "win {:#x} mode {:#x} winsc {:#x}-{:#x} scrsc {:#x}-{:#x} vp {},{} {}x{} sc {},{}-{},{}",
                 vs->hash, ps ? ps->hash : 0, pk.indexed ? pk.index_count : pk.vertexCount, targets.color[0]->va,
                 targets.offsetX, targets.offsetY, targets.scale, dev.F32(dev::kVportXScale),
                 dev.F32(dev::kVportXOffset), dev.F32(dev::kVportYScale), dev.F32(dev::kVportYOffset),
                 dev.U32(dev::kVteControl), dev.U32(dev::kWindowOffset), dev.U32(dev::kModeControl),
                 dev.U32(dev::kWindowScissorTL), dev.U32(dev::kWindowScissorBR), dev.U32(dev::kScreenScissorTL),
                 dev.U32(dev::kScreenScissorBR), vp.vp.x, vp.vp.y, vp.vp.width, vp.vp.height, vp.scissor.left,
                 vp.scissor.top, vp.scissor.right, vp.scissor.bottom);
    }
    for (u32 i = 0; i < 4; ++i) {
      sc.booleans[i] = dev.U32(dev::kVsBoolConstants + 4 * i);
      sc.booleans[4 + i] = dev.U32(dev::kPsBoolConstants + 4 * i);
    }
    sc.booleans[4] &= ~static_cast<u32>(REXCVAR_GET(fable2_debug_ps_bool_clear));
    FillLoopConstants(dev, dev::kVsLoopConstants, &sc.loopConstants[0]);
    FillLoopConstants(dev, dev::kPsLoopConstants, &sc.loopConstants[16]);
    sc.swappedTexcoords = layout->swappedTexcoords;
    sc.swappedNormals = layout->swappedNormals;
    sc.swappedBinormals = layout->swappedBinormals;
    sc.swappedTangents = layout->swappedTangents;
    sc.swappedBlendWeights = layout->swappedBlendWeights;
    sc.swappedPositions = layout->swappedPositions;
    // The host copies vertex data with a 32-bit swap, which leaves 16-bit
    // pairs in Xenos order for 8-in-32 streams; only 8-in-16 streams need
    // their halves swapped back.
    for (const InputLayout::SwapElement &e : layout->swapElements) {
      if ((dev.U32(dev::StreamFetchSlotOffset(e.stream) + 4) & 3) == 1)
        continue;
      const u32 clear = ~(1u << (e.index & 31));
      switch (static_cast<DeclUsage>(e.usage)) {
      case DeclUsage::TexCoord: sc.swappedTexcoords &= clear; break;
      case DeclUsage::Normal: sc.swappedNormals &= clear; break;
      case DeclUsage::Binormal: sc.swappedBinormals &= clear; break;
      case DeclUsage::Tangent: sc.swappedTangents &= clear; break;
      case DeclUsage::BlendWeight: sc.swappedBlendWeights &= clear; break;
      case DeclUsage::Position: sc.swappedPositions &= clear; break;
      default: break;
      }
    }
    sc.sintTexcoords = layout->sintTexcoords;
    sc.packedDec3 = layout->packedDec3;
    sc.pullFlags = !REXCVAR_GET(fable2_pull_half_order_vs).empty() &&
                           DrawMatchesList(REXCVAR_GET(fable2_pull_half_order_vs), vs->hash, 0)
                       ? 1u
                       : 0u;
    if (targets.colorCount) {
      const u32 f0 = targets.color[0]->colorFormat;
      if (f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_8_8_8_8_GAMMA))
        sc.packedDec3 |= 1u << 31;
      if (f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_8_8_8_8) ||
          f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) ||
          f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_2_10_10_10) ||
          f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10))
        sc.packedDec3 |= 1u << 30;
    // Fable 2: 7e3 targets (HDR scene) clamp to [0, 31.875] and drop NaN/inf.
    if (f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT) ||
        f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16))
      sc.packedDec3 |= 1u << 29;
    }
    if (!REXCVAR_GET(fable2_offsets_guest_texels))
      sc.packedDec3 |= 1u << 28;
    {
      const u32 cc = dev.U32(dev::kColorControl);
      // The shader's alpha test is clip(alpha - ref), i.e. GREATER_EQUAL. Xenos
      // GREATER (4) also fails alpha == ref: the tree leaf cards' shadow casters
      // test alpha > 0, and their transparent texels (alpha 0) cast solid squares.
      sc.alphaThreshold = dev.F32(dev::kAlphaRef);
      const bool exact = REXCVAR_GET(fable2_alpha_exact);
      if (exact && (cc & (1u << 3)) && (cc & 7) == 4)
        sc.alphaThreshold += 1.0e-6f;
      // Xenos alpha to mask: the console dithers coverage over each 2x2 quad
      // with RB_COLORCONTROL's offsets (bits 24..31); the shader reproduces it
      // (objects in front of the camera fade out instead of vanishing at once).
      // Coverage masks depth writes too: depth-only draws (shadow casters, the
      // emulated Z pass) dither like the colour pass.
      sc.alphaToMask = (cc & (1u << 4)) && (exact || targets.colorCount) ? 0x80000000u | (cc >> 24) : 0u;
    }
    std::memcpy(sc.posScale, vp.posScale, sizeof(sc.posScale));
    std::memcpy(sc.posOffset, vp.posOffset, sizeof(sc.posOffset));

    static ReplayMemo prerun_scratch;
    if (targets.colorCount && !s.depth_prerun &&
        u64(targets.width) * targets.height > u64(s.frame_target_w) * s.frame_target_h) {
      s.frame_target_w = targets.width;
      s.frame_target_h = targets.height;
      s.frame_target_scale = targets.scale;
    }
    ReplayMemo &fill = s.depth_prerun ? prerun_scratch : memo ? *memo : memos[memo_next++ % kReplayMemoWays];
    fill.valid = !s.depth_prerun;
    fill.vs = vs;
    fill.ps = ps;
    fill.layout = layout;
    fill.jitterIndex = jitter_index;
    fill.velocity = targets.velocity;
    for (u32 i = 0; i < 4; ++i)
      fill.images[i] = i < targets.colorCount ? targets.colorImage[i] : nullptr;
    fill.images[4] = targets.depthImage;
    fill.colorCount = targets.colorCount;
    fill.samples = targets.samples;
    fill.offsetX = targets.offsetX;
    fill.offsetY = targets.offsetY;
    fill.topology = pk.topology;
    fill.rectList = pk.rectList;
    std::memcpy(fill.strides, pk.strides, sizeof(fill.strides));
    std::memcpy(fill.state, state_block, sizeof(fill.state));
    std::memcpy(fill.consts, consts_block, sizeof(fill.consts));
    fill.st = st;
    fill.spec = spec;
    fill.a2c = a2c;
    fill.pipeline = pipeline;
    fill.vp = vp;
    fill.fixed = sc;
    fill.sharedRing = ~0ull;
    memo = &fill;
  }
  s.current_vs_va = pk.vs_va;
  s.current_ps_va = pk.ps_va;
  {
    static const char *const kOrigins[2][3] = {{"stream/none", "stream/stream", "stream/bundle"},
                                               {"bundle/none", "bundle/stream", "bundle/bundle"}};
    s.current_origin =
        kOrigins[vs->createdByGuestCall ? 1 : 0][ps ? (ps->createdByGuestCall ? 2 : 1) : 0];
  }
  lap(s.perf.pso_lookup_ms);

  {
    PerfScope bind_scope(s.perf.bind_ms);
    // Vertex shader texture fetches (fetch constants 16..19) from the shader cache.
    const u32 vs_textures = vs->entry ? vs->entry->textureFetchMask & 0xF0000u : 0u;
    const u32 texture_mask = (vs->textureFetchMask | (ps ? ps->textureFetchMask : 0u) | vs_textures) &
                             (0xFFFFu | vs_textures);
    const u32 dc = dev.U32(dev::kDepthControl);
    const u32 zfunc = (dc >> 4) & 7;
    const bool scene_draw = !pk.rectList && (dc & 2) &&
                            (zfunc == 1 || zfunc == 3 || zfunc == 4 || zfunc == 6);
    BindTexturesAndSamplers(s, dev, texture_mask, sc, scene_draw ? &targets : nullptr,
                            REXCVAR_GET(fable2_vertex_textures) || pk.tess_segments, pk, targets.scale);
    // Debug (dog lighting): "hash:first-last" logs that vertex or pixel shader's
    // float constants as the draw uploaded them (the guest's constant memory,
    // outside the packet's device window, has moved on by now) and the
    // booleans, twice per (draw size, render target). A new value starts over.
    for (u32 stage = 0; stage < 2; ++stage) {
      const std::string &want = stage ? REXCVAR_GET(fable2_debug_ps_consts) : REXCVAR_GET(fable2_debug_vs_consts);
      const GuestShader *sh = stage ? ps : vs;
      u32 first = 0, last = 0;
      if (want.empty() || !sh || want.size() < 17 || want[16] != ':' ||
          want.compare(0, 16, std::format("{:016x}", sh->hash)) != 0 ||
          std::sscanf(want.c_str() + 17, "%u-%u", &first, &last) != 2 || first > last || last >= 256)
        continue;
      static std::unordered_map<u64, u32> logged[2];  // render thread only
      static std::string logged_for[2];
      if (logged_for[stage] != want) {
        logged_for[stage] = want;
        logged[stage].clear();
      }
      const u32 count = pk.indexed ? pk.index_count : pk.vertexCount;
      const u32 rt0 = targets.colorCount ? targets.color[0]->va : 0;
      if (logged[stage].size() >= 256 || logged[stage][(u64(count) << 32) ^ rt0]++ >= 2)
        continue;
      const UploadAlloc &file = stage ? pk.ps_consts : pk.vs_consts;
      const float *up = reinterpret_cast<const float *>(file.cpu);
      std::string line;
      for (u32 r = first; r <= last && up && (r + 1) * 16 <= file.size; ++r)
        line += std::format(" c{}=({:.4g},{:.4g},{:.4g},{:.4g})", r, up[r * 4], up[r * 4 + 1], up[r * 4 + 2],
                            up[r * 4 + 3]);
      EOT_INFO("[{}const] vs {:016x} ps {:016x} n {} rt0 {:#x} {}x{} bools {:08x} {:08x}:{}", stage ? "ps" : "vs",
               vs->hash, ps ? ps->hash : 0, count, rt0, targets.width, targets.height, sc.booleans[0],
               sc.booleans[4], line);
    }
    if (!REXCVAR_GET(fable2_diag_vs).empty() &&
        DrawMatchesList(REXCVAR_GET(fable2_diag_vs), vs->hash, pk.indexed ? pk.index_count : pk.vertexCount)) {
      static u32 diag_draws = 0;
      if (diag_draws++ % 200 == 0) {
        std::string line;
        for (u32 slot = 0; slot < 16; ++slot)
          if (const GuestTexture *t = s.draw_bound_textures[slot])
            line += std::format(" | s{} {:#x} fmt{} {}x{} up{} fail{} gam{} host{}:{} f0 {:08x} f1 {:08x} f3 {:08x}",
                                slot, t->va, static_cast<u32>(t->format), t->width, t->height, t->uploaded,
                                t->uploadFailed, t->gammaSigned, static_cast<u32>(t->host.format),
                                t->host.texture ? 1 : 0, t->fetch[0], t->fetch[1], t->fetch[3]);
        // The live guest bytes behind each bound slot (stale uploads of streamed textures).
        line += std::format(" | blend0 {:08x}", dev.U32(dev::kBlendControl0));
        for (u32 slot = 0; slot < 16; ++slot) {
          const GuestTexture *t0 = s.draw_bound_textures[slot];
          if (!t0)
            continue;
          const u32 base = t0->fetch[1] & ~0xFFFu;
          if (const u8 *p = mem::readable(base, 4096) ? mem::at<u8>(base) : nullptr) {
            u32 nz = 0;
            for (u32 k = 0; k < 4096; ++k)
              nz += p[k] != 0;
            std::string head;
            for (u32 k = 0; k < 16; ++k)
              head += std::format("{:02x}", p[k]);
            line += std::format(" | s{} live base {:#x} nz4k {} head {} uploadSeq {} unlockSeq {}", slot, base, nz, head,
                                t0->uploadedUnlockSeq, ResourceUnlockSeq(t0->va));
          }
        }
        line += std::format(" | cc {:08x}", dev.U32(dev::kColorControl));
        EOT_INFO("[diagvs] vs {:016x} ps {:016x} n {} mask {:#x} biased {:#x} psbool {:08x} vsbool {:08x}{}", vs->hash,
                 ps ? ps->hash : 0, pk.indexed ? pk.index_count : pk.vertexCount, texture_mask, sc.biasedTextures,
                 sc.booleans[4], sc.booleans[0], line);
      }
    }
  }
  if (pk.hasCameraVP)
    taa::BeforeSceneConsumerDraw(s, s.draw_bound_textures, pk.cameraVP, ps ? ps->hash : 0, pk.taaSkip);
  // Debug: pull chosen shaders' depth toward the camera (z-fighting tests).
  if (!REXCVAR_GET(fable2_debug_depth_scale_vs).empty() &&
      DrawMatchesList(REXCVAR_GET(fable2_debug_depth_scale_vs), vs->hash,
                      pk.indexed ? pk.index_count : pk.vertexCount)) {
    const float k = static_cast<float>(REXCVAR_GET(fable2_debug_depth_scale));
    sc.posScale[2] = vp.posScale[2] * k;
    sc.posOffset[2] = vp.posOffset[2] * k;
  } else {
    sc.posScale[2] = vp.posScale[2];
    sc.posOffset[2] = vp.posOffset[2];
  }
  // Vertex pulling: the table and data were uploaded at capture time.
  sc.vertexPullTable = pk.vertex_pull_table;
  // Guest r0: the raw index (fable2_vertex_index_with_base false) or the index
  // plus the draw's base vertex.
  sc.hostBaseVertex = REXCVAR_GET(fable2_vertex_index_with_base) ? pk.vertex_id_bias
                                                                 : pk.host_base_vertex;
  sc.pixelPosScale = targets.scale > 0.0f ? 1.0f / targets.scale : 1.0f;
  sc.pointSizeScale = (targets.scale > 0.0f ? targets.scale : 1.0f) *
                      static_cast<float>(REXCVAR_GET(fable2_debug_point_scale));
  sc.isPointDraw = pk.topology == plume::RenderPrimitiveTopology::POINT_LIST ? 1u : 0u;
  sc.pointViewport[0] = vp.vp.x;
  sc.pointViewport[1] = vp.vp.y;
  sc.pointViewport[2] = vp.vp.width;
  sc.pointViewport[3] = vp.vp.height;
  sc.tessSegments = pk.tess_segments;
  sc.tessFirstPatch = pk.tess_first_patch;
  sc.debugAddress = 0;
  {
    // Debug: one patch draw at a time stores its first vertices' r0 and oPos
    // (XenosRecomp xeDebugVertex); logged a few frames later.
    static std::unique_ptr<plume::RenderBuffer> buffer;
    static f32 *cpu = nullptr;
    static u64 gpu_va = 0, armed_frame = 0;
    static bool armed = false;
    if (armed && s.guest_frames > armed_frame + 4) {
      armed = false;
      std::string line;
      for (u32 v = 0; v < 16; ++v)
        line += std::format(" | v{} r0 ({:.3g},{:.3g},{:.3g}) size {:.3g} pos ({:.4g},{:.4g},{:.4g},{:.4g})", v,
                            cpu[v * 8], cpu[v * 8 + 1], cpu[v * 8 + 2], cpu[v * 8 + 3], cpu[v * 8 + 4],
                            cpu[v * 8 + 5], cpu[v * 8 + 6], cpu[v * 8 + 7]);
      EOT_INFO("[tess] readback{}", line);
    }
    const bool readback_vs = !REXCVAR_GET(fable2_readback_vs).empty() &&
                             DrawMatchesList(REXCVAR_GET(fable2_readback_vs), vs->hash,
                                             pk.indexed ? pk.index_count : pk.vertexCount);
    if (!armed && (readback_vs || (pk.topology == plume::RenderPrimitiveTopology::POINT_LIST &&
                                   REXCVAR_GET(fable2_tess_readback)))) {
      if (!buffer) {
        buffer = CreateHostBuffer(s.device.get(), plume::RenderBufferDesc::UploadBuffer(4096), "tess-readback");
        cpu = buffer ? static_cast<f32 *>(buffer->map()) : nullptr;
        gpu_va = buffer ? buffer->getDeviceAddress() : 0;
      }
      if (cpu && gpu_va) {
        std::fill(cpu, cpu + 16 * 8, -12345.0f);
        sc.debugAddress = gpu_va;
        armed = true;
        armed_frame = s.guest_frames;
        EOT_INFO("[tess] readback armed: vs {:016x} first {}", pk.vs ? pk.vs->hash : 0, pk.tess_first_patch);
      }
    }
  }
  {
    // Fable 2 predicated tiling: g_TileOffsetSize.xy is the lower tile's screen
    // offset; the host draws untiled, so world shaders drop it.
    bool zero = REXCVAR_GET(fable2_zero_tile_offset);
    if (zero && pk.ps && !REXCVAR_GET(fable2_tile_offset_exempt).empty()) {
      char hex[17];
      std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(pk.ps->hash));
      zero = REXCVAR_GET(fable2_tile_offset_exempt).find(hex) == std::string::npos;
    }
    sc.tileOffsetKeep = zero ? 0.0f : 1.0f;
  }
  const bool pulled_draw = pk.vertex_pull_table != 0;
  UploadAlloc shared_alloc;
  const u64 ring_epoch = UploadRingEpoch();
  if (!pulled_draw && !s.depth_prerun && memo->sharedRing == ring_epoch &&
      memo->sharedBaseVertex == sc.hostBaseVertex && memo->sharedPixelScale == sc.pixelPosScale &&
      memo->sharedPointDraw == sc.isPointDraw && memo->sharedTileKeep == sc.tileOffsetKeep &&
      memo->sharedTessSegments == sc.tessSegments && memo->sharedTessFirst == sc.tessFirstPatch &&
      memo->sharedDebug == sc.debugAddress &&
      std::memcmp(memo->sharedPointViewport, sc.pointViewport, sizeof(sc.pointViewport)) == 0 &&
      std::memcmp(memo->sharedSamplerVariants, sc.samplerPointIndices, sizeof(memo->sharedSamplerVariants)) == 0 &&
      memo->sharedPullFlags == sc.pullFlags &&
      std::memcmp(memo->sharedVsTextures, sc.vsTexture2DIndices, sizeof(memo->sharedVsTextures)) == 0 &&
      std::memcmp(memo->sharedTexScale, sc.texHostScale, sizeof(sc.texHostScale)) == 0 &&
      std::memcmp(memo->sharedTextures, &sc, kSharedTextureBytes) == 0 &&
      memo->sharedSint == sc.sintTexcoords && memo->sharedBiased == sc.biasedTextures) {
    shared_alloc = memo->shared;
  } else {
    if (!UploadBytes(&sc, sizeof(sc), kConstantBufferAlignment, &shared_alloc)) {
      Dropped("shared constant upload failed", 0x6011);
      return;
    }
    s.perf.constant_bytes += sizeof(SharedConstants);
    memo->shared = shared_alloc;
    memo->sharedRing = ring_epoch;
    std::memcpy(memo->sharedTextures, &sc, kSharedTextureBytes);
    memo->sharedSint = sc.sintTexcoords;
    memo->sharedBiased = sc.biasedTextures;
    memo->sharedBaseVertex = sc.hostBaseVertex;
    memo->sharedPixelScale = sc.pixelPosScale;
    memo->sharedPointDraw = sc.isPointDraw;
    memo->sharedTileKeep = sc.tileOffsetKeep;
    memo->sharedTessSegments = sc.tessSegments;
    memo->sharedTessFirst = sc.tessFirstPatch;
    memo->sharedDebug = sc.debugAddress;
    std::memcpy(memo->sharedPointViewport, sc.pointViewport, sizeof(sc.pointViewport));
    std::memcpy(memo->sharedSamplerVariants, sc.samplerPointIndices, sizeof(memo->sharedSamplerVariants));
    memo->sharedPullFlags = sc.pullFlags;
    std::memcpy(memo->sharedVsTextures, sc.vsTexture2DIndices, sizeof(memo->sharedVsTextures));
    std::memcpy(memo->sharedTexScale, sc.texHostScale, sizeof(sc.texHostScale));
  }
  lap(s.perf.const_ms);

  // Temporary (black dog coat): log the draws into 512x512 colour targets.
  static u32 rt512_logs = 0;
  const bool rt512 = REXCVAR_GET(fable2_debug_log_rt512) && rt512_logs < 300 && targets.colorCount &&
                     targets.width == 512 && targets.height == 512;
  if (rt512)
    ++rt512_logs;
  static u32 rt_sized_logs = 0;
  const i32 rt_size = REXCVAR_GET(fable2_debug_log_rt_size);
  const bool rt_sized = rt_sized_logs < 300 &&
                        ((rt_size > 0 && targets.colorCount && targets.width == static_cast<u32>(rt_size) &&
                          targets.height == static_cast<u32>(rt_size) &&
                          !(ps && !REXCVAR_GET(fable2_debug_log_rt_skip_ps).empty() &&
                            DrawMatchesList(REXCVAR_GET(fable2_debug_log_rt_skip_ps), ps->hash, 0))) ||
                         (ps && !REXCVAR_GET(fable2_debug_log_ps).empty() &&
                          DrawMatchesList(REXCVAR_GET(fable2_debug_log_ps), ps->hash, 0)));
  if (rt_sized) {
    ++rt_sized_logs;
    EOT_INFO("[rtlog] frame {}", s.guest_frames);
  }
  const bool diag_now = (Settings::DiagFrame() > 0 &&
                         s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame())) ||
                        rt512 || rt_sized;
  if (diag_now && Settings::Record()) {
    GpuTimingDiagMark(s, s.command_list,
                      std::format("draw rt0={:#x} {}x{} f{} smp{}{} ps={:016x} vs={:016x}",
                                  targets.colorCount ? targets.color[0]->va : 0, targets.width,
                                  targets.height,
                                  targets.colorCount ? targets.color[0]->colorFormat : 99,
                                  targets.samples,
                                  targets.depth && targets.depthImage == &targets.depth->single
                                      ? " twin"
                                      : "",
                                  ps ? ps->hash : 0, vs->hash));
  } else if (diag_now) {
    static u32 k = 0;
    const u32 vte = dev.U32(dev::kVteControl);
    GpuTimingDiagMark(s, s.command_list, std::format("draw {}", k));
    EOT_INFO("[diag] draw {} prim {} {}{} smp={}{}{}{} n={} pso={} key={:016x} mode={:#x} dc={:#x} srm={:#x} srmbf={:#x} rt0={:#x} {}x{} fmt{} rtexp{} ds={:#x} vs={:016x} ps={:016x} psva={:#x} "
             "vp=({:.0f},{:.0f} {:.0f}x{:.0f} z{:.2f}-{:.2f}) vte={:#x} xs={:.1f} ys={:.1f} zs={:.3f} "
             "zo={:.3f} scis=({},{},{},{}) cull={} z={}{} func{} blend={} mask={:#x} spec={:#x} "
             "streams={:#x} stride0={} tex0={:#x} posScale=({:.3f},{:.3f},{:.3f}) "
             "posOff=({:.3f},{:.3f},{:.3f}) tiles=({},{}) hiz={:#x} cc={:#x} a2c={} aref={:.3f}",
             k++, pk.prim, pk.indexed ? "idx" : "vtx", pk.rectList ? " rect" : "", targets.samples,
             cls.additive ? " add" : "",
             targets.depth && targets.depthImage == &targets.depth->single ? " dtwin" : "",
             targets.offsetX || targets.offsetY ? " atlas" : "",
             pk.indexed ? pk.index_count : pk.vertexCount, static_cast<const void *>(pipeline),
             HashPipelineState(st), dev.U32(dev::kModeControl), dev.U32(dev::kDepthControl),
             dev.U32(dev::kStencilRefMask), dev.U32(dev::kStencilRefMaskBF),
             targets.colorCount ? targets.color[0]->va : 0, targets.width, targets.height,
             targets.colorCount ? targets.color[0]->colorFormat : 99,
             targets.colorCount ? targets.color[0]->colorExpBias : 0,
             targets.depth ? targets.depth->va : 0, vs->hash, ps ? ps->hash : 0, pk.ps_va, vp.vp.x, vp.vp.y,
             vp.vp.width, vp.vp.height, vp.vp.minDepth, vp.vp.maxDepth, vte,
             dev.F32(dev::kVportXScale), dev.F32(dev::kVportYScale), dev.F32(dev::kVportZScale),
             dev.F32(dev::kVportZOffset), vp.scissor.left, vp.scissor.top, vp.scissor.right,
             vp.scissor.bottom, static_cast<u32>(st.cull), st.depthEnable ? "on" : "off",
             st.depthWrite ? "w" : "", static_cast<u32>(st.depthFunc),
             st.blend[0].blendEnabled, st.blend[0].renderTargetWriteMask, spec,
             layout->streamMask, pk.strides[0], dev.U32(dev::kTextureObject0),
             vp.posScale[0], vp.posScale[1], vp.posScale[2], vp.posOffset[0], vp.posOffset[1],
             vp.posOffset[2], targets.colorCount ? static_cast<i32>(targets.color[0]->baseTile) : -1,
             targets.depth ? static_cast<i32>(targets.depth->baseTile) : -1, dev.U32(dev::kHiControl),
             dev.U32(dev::kColorControl), st.alphaToCoverage, dev.F32(dev::kAlphaRef));
    {
      auto pc = [&](u32 c, u32 i) { return dev.F32(dev::kPsFloatConstants + c * 16 + i * 4); };
      EOT_INFO("[diag]   ps c31=({:.4g},{:.4g}) c46=({:.4g},{:.4g}) c47={:.4g} c77=({:.4g},{:.4g}) c78={:.4g}",
               pc(31, 0), pc(31, 1), pc(46, 0), pc(46, 1), pc(47, 0), pc(77, 0), pc(77, 1), pc(78, 0));
      // Temporary: screen shadow mask passes (shadow transforms c36..c43).
      if (ps && (ps->hash == 0x6d4968d5b2bc94c6ull || ps->hash == 0x8ae5b04e31699d07ull))
        for (u32 slot = 0; slot < 16; ++slot)
          if (s.draw_bound_textures[slot])
            g_pending_texture_dumps.push_back({dev.U32(dev::kTextureObject0 + 4 * slot), 1000 + slot});
      if (ps && (ps->hash == 0x6d4968d5b2bc94c6ull || ps->hash == 0x383793fa8d5a0716ull))
        for (u32 c : {12u, 36u, 37u, 38u, 39u, 40u, 41u, 42u, 43u, 46u, 47u, 77u})
          EOT_INFO("[diag]   shadow ps {:016x} c{} = ({:.5g}, {:.5g}, {:.5g}, {:.5g})", ps->hash, c, pc(c, 0),
                   pc(c, 1), pc(c, 2), pc(c, 3));
    }
    // Temporary diagnostics: raw texcoords of the main world shader.
    if (vs->hash == 0x4ba0b3646633c5bfull) {  // Temporary (missing ground): fur vertex data
      static u32 fur_logs = 0;
      if (Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame()) &&
          fur_logs++ < 6) {
        std::string el;
        for (const auto &e : layout->elements)
          el += std::format(" {}{}@s{}+{} fmt{}", e.semanticName ? e.semanticName : "?", e.semanticIndex,
                            e.slotIndex, e.alignedByteOffset, static_cast<u32>(e.format));
        EOT_INFO("[diag] fur layout:{} strides {} {}", el, layout->fetchStride[0], layout->fetchStride[1]);
        {
          const u32 ib = dev.U32(dev::kIndexBuffer);
          EOT_INFO("[diag] fur draw: prim {} indexed {} count {} host base vertex {} ib {:#x}", pk.prim,
                   pk.indexed, pk.indexed ? pk.index_count : pk.vertexCount, pk.host_base_vertex, ib);
          for (u32 st = 0; st < 6; ++st) {
            const u32 a = dev.U32(dev::StreamFetchSlotOffset(st)), b = dev.U32(dev::StreamFetchSlotOffset(st) + 4);
            EOT_INFO("[diag] fur fetch slot {}: base {:#x} size {} type {}", 95 - st, a & ~3u,
                     ((b >> 2) & 0xFFFFFF) * 4, a & 3);
          }
        }
        for (u32 st = 0; st < 2; ++st) {
          const u32 d0 = dev.U32(dev::StreamFetchSlotOffset(st)), d1 = dev.U32(dev::StreamFetchSlotOffset(st) + 4);
          const u8 *vb = mem::phys<u8>(d0 & ~3u);
          std::string raw;
          for (u32 k = 0; vb && k < 15; ++k)
            raw += std::format("{:08x} ", static_cast<u32>(reinterpret_cast<const be_u32 *>(vb)[k]));
          EOT_INFO("[diag] fur stream{}: endian {} base {:#x} size {} raw {}", st, d1 & 3, d0 & ~3u,
                   ((d1 >> 2) & 0xFFFFFF) * 4, raw);
        }
        auto vc = [&](u32 c, u32 i) { return dev.F32(dev::kVsFloatConstants + c * 16 + i * 4); };
        std::string cs;
        for (u32 c = 0; c < 16; ++c)
          cs += std::format(" c{}=({:.3g},{:.3g},{:.3g},{:.3g})", c, vc(c, 0), vc(c, 1), vc(c, 2), vc(c, 3));
        EOT_INFO("[diag] fur vs consts:{}", cs);
      }
    }
    if (vs->hash == 0xbcc6e2def4a3e5c2ull) {
      static u32 uv_logs = 0;
      if (uv_logs++ < 6) {
        const u32 d0 = dev.U32(dev::StreamFetchSlotOffset(0)), d1 = dev.U32(dev::StreamFetchSlotOffset(0) + 4);
        const u32 stride = dev.U8(dev::kStreamStride0) * 4u;
        const u8 *vb = mem::phys<u8>(d0 & ~3u);
        std::string raw;
        for (u32 k = 0; vb && k < 24; ++k)
          raw += std::format("{:08x}{}", static_cast<u32>(reinterpret_cast<const be_u32 *>(vb)[k]),
                             k % 6 == 5 ? " | " : " ");
        EOT_INFO("[diag] uv: stream0 endian {} base {:#x} size {} layout stride {} raw {}", d1 & 3,
                 d0 & ~3u, ((d1 >> 2) & 0xFFFFFF) * 4, layout->fetchStride[0], raw);
      }
    }
    // Temporary diagnostics: dump the main wall material's textures once.
    {
      static u32 material_dumps = 0;
      static u64 dumped_hashes[3] = {};
      const bool wanted = ps && (ps->hash == 0x9a193febb26723f4ull || ps->hash == 0xb950f34c5012d6f7ull ||
                                 ps->hash == 0x7c1ef4455de18d9bull);
      const bool fresh = wanted && dumped_hashes[0] != ps->hash && dumped_hashes[1] != ps->hash &&
                         dumped_hashes[2] != ps->hash;
      if (fresh && material_dumps < 3 && Settings::DiagDump()) {
        dumped_hashes[material_dumps] = ps->hash;
        ++material_dumps;
        for (u32 slot = 0; slot < 4; ++slot)
          if (const u32 tex_va = dev.U32(dev::kTextureObject0 + 4 * slot))
            g_pending_texture_dumps.push_back({tex_va, material_dumps * 16 + slot});
      }
    }
    for (u32 slot = 0; slot < 16 && !Settings::Record(); ++slot) {
      const u32 tex_va = dev.U32(dev::kTextureObject0 + 4 * slot);
      if (!tex_va)
        continue;
      u32 fc[6];
      for (u32 d = 0; d < 6; ++d)
        fc[d] = dev.U32(dev::kFetchConstants + 24 * slot + 4 * d);
      const GuestTexture *gt = s.draw_bound_textures[slot];
      const bool dump_vs = !REXCVAR_GET(fable2_debug_dump_bound_vs).empty() &&
                           DrawMatchesList(REXCVAR_GET(fable2_debug_dump_bound_vs), vs->hash, 0);
      if (gt && dump_vs && g_pending_bound_dumps.size() < 24) {
        const auto it = s.textures.find(gt->va);
        const u64 h = vs->hash;
        if (it != s.textures.end() && it->second.get() == gt)
          g_pending_bound_dumps.push_back(
              {it->second, std::format("logs/bound_{:08x}_slot{}_{:08x}.ppm", static_cast<u32>(h >> 32), slot, gt->va)});
      }
      EOT_INFO("[diag]   tex{} va={:#x} fc=[{:08x} {:08x} {:08x} {:08x} {:08x} {:08x}] fmt={} "
               "dim={} {}x{}x{} {} {} {} biased={:#x} base={:#x} pitch={} obj={:#x} resolved_ago={}",
               slot, tex_va, fc[0], fc[1], fc[2], fc[3], fc[4], fc[5],
               gt ? static_cast<u32>(gt->format) : 0xFFFFu,
               gt ? static_cast<u32>(gt->dimension) : 9u, gt ? gt->width : 0u,
               gt ? gt->height : 0u, gt ? gt->depth : 0u, gt && gt->tiled ? "tiled" : "linear",
               gt && gt->gammaSigned ? "gamma" : "lin",
               gt && gt->resolveOwned ? "resolve" : "upload", sc.biasedTextures,
               gt ? gt->baseAddress : 0u, ((fc[0] >> 22) & 0x1FF) * 32, gt ? gt->va : 0u,
               gt && gt->resolveOwned ? static_cast<i64>(s.guest_frames) - static_cast<i64>(gt->lastResolvedFrame) : -1);
      if (rt512) {  // Temporary (black dog coat): the header as recorded and as it is now
        u32 live[6] = {};
        ReadTextureHeader(tex_va, live);
        const u32 *rec = (pk.texHeaderMask & (1u << slot)) ? pk.texHeaders[slot] : nullptr;
        EOT_INFO("[diag]     tex{} recorded [{:08x} {:08x} {:08x}] live [{:08x} {:08x} {:08x}]", slot,
                 rec ? rec[0] : 0u, rec ? rec[1] : 0u, rec ? rec[2] : 0u, live[0], live[1], live[2]);
      }
      static u32 dumped = 0;
      if (gt && !gt->tiled && gt->format == xe::TextureFormat::k_8 && dumped < 8) {
        const u32 pitch = ((fc[0] >> 22) & 0x1FF) * 32;
        const u8 *src = mem::phys<const u8>(gt->baseAddress);
        if (src && pitch >= gt->width) {
          const std::string path =
              std::format("logs/f{}_tex{}_{:x}_{}x{}_p{}.bin", s.guest_frames + 1, slot, tex_va,
                          gt->width, gt->height, pitch);
          if (FILE *f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(src, 1, static_cast<size_t>(pitch) * gt->height, f);
            std::fclose(f);
            ++dumped;
            EOT_INFO("[diag]   -> wrote {}", path);
          }
        }
      }
    }
  }

  if (!BindTargets(s, targets)) {
    Dropped("framebuffer creation failed", 0x6012);
    return;
  }
  GuestSurface *work_surface = targets.colorCount ? targets.color[0] : targets.depth;
  if (work_surface)
    work_surface->perfDraws++;
  GpuTimingCountDraw(s);
  if (pk.max_slot == 0 && !layout->needsSyntheticSlot)
    s.perf.single_stream_draws++;
  auto *cmd = s.command_list;
  bool dynamic_state_invalid = s.bound_pipeline == nullptr;
  const bool pipeline_changed = s.bound_pipeline != pipeline;
  if (pipeline_changed) {
    cmd->setPipeline(pipeline);
    s.perf.pipeline_bind_calls++;
    if (memo_hit) {
      s.perf.pipeline_bind_on_hit++;
      static u32 logged = 0;
      if (logged++ < 8)
        EOT_DEBUG("[draw] pipeline bind on a memo hit: memo {} bound {} entry {} vs {:016x} ps {:016x} "
                  "samples {} rt0 {:#x}",
                  static_cast<const void *>(pipeline), static_cast<const void *>(s.bound_pipeline),
                  static_cast<const void *>(memo), vs->hash, ps ? ps->hash : 0, targets.samples,
                  targets.colorCount ? targets.color[0]->va : 0);
    }
    s.bound_pipeline = pipeline;
  }
#if defined(EOT_D3D12)
  static u32 last_stencil_ref = ~0u;
  if (dynamic_state_invalid)
    last_stencil_ref = ~0u;
  if (st.stencilEnable && last_stencil_ref != st.stencilRef) {
    static_cast<plume::D3D12CommandList *>(cmd)->d3d->OMSetStencilRef(st.stencilRef);
    last_stencil_ref = st.stencilRef;
    s.perf.stencil_ref_calls++;
  }
#else
  // The stencil reference is dynamic state (the pipeline key leaves it out).
  static u32 last_stencil_ref = ~0u;
  if (dynamic_state_invalid)
    last_stencil_ref = ~0u;
  if (st.stencilEnable) {
    const u32 ref = dev.U32(dev::kStencilRefMask) & 0xFF;
    if (ref != last_stencil_ref) {
      cmd->setStencilReference(ref);
      last_stencil_ref = ref;
      s.perf.stencil_ref_calls++;
    }
  }
#endif
  static plume::RenderViewport last_vp;
  static plume::RenderRect last_sc;
  if (dynamic_state_invalid || std::memcmp(&last_vp, &vp.vp, sizeof(last_vp)) != 0) {
    cmd->setViewports(&vp.vp, 1);
    last_vp = vp.vp;
    s.perf.viewport_bind_calls++;
  }
  if (dynamic_state_invalid || std::memcmp(&last_sc, &vp.scissor, sizeof(last_sc)) != 0) {
    cmd->setScissors(&vp.scissor, 1);
    last_sc = vp.scissor;
    s.perf.scissor_bind_calls++;
  }
  // The blend constant (RB_BLEND_RED..ALPHA): Fable 2 fades objects between
  // the camera and the hero with CONSTANT_COLOR blending; never set, the host's
  // constant stayed 0 and they vanished at once.
  {
    static float last_blend[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    const float blend[4] = {dev.F32(dev::kBlendRed), dev.F32(dev::kBlendRed + 4), dev.F32(dev::kBlendRed + 8),
                            dev.F32(dev::kBlendRed + 12)};
    if (dynamic_state_invalid || std::memcmp(last_blend, blend, sizeof(blend)) != 0) {
      cmd->setBlendConstants(blend);
      std::memcpy(last_blend, blend, sizeof(blend));
    }
  }
  const UploadAlloc *roots[2] = {&pk.vs_consts, &pk.ps_consts};
  auto bind_root_cbv = [&](u32 r, const UploadAlloc &alloc) {
#if defined(EOT_D3D12)
    if (alloc.gpuVa && s.root_cbv_index[r] != ~0u) {
      static_cast<plume::D3D12CommandList *>(cmd)->d3d->SetGraphicsRootConstantBufferView(
          s.root_cbv_index[r], alloc.gpuVa);
      return;
    }
    cmd->setGraphicsRootDescriptor(plume::RenderBufferReference(alloc.buffer, alloc.offset), r);
#elif defined(FABLE2_CONSTANT_BUFFERS)
    cmd->setGraphicsRootDescriptor(plume::RenderBufferReference(alloc.buffer, alloc.offset), r);
#else
    const u64 address = alloc.gpuVa;
    cmd->setGraphicsPushConstants(kGuestPushConstantRangeIndex, &address, r * static_cast<u32>(sizeof(u64)),
                                  static_cast<u32>(sizeof(u64)));
#endif
  };
  for (u32 r = 0; r < 2; ++r) {
    if (s.bound_root_buffer[r] == roots[r]->buffer && s.bound_root_offset[r] == roots[r]->offset)
      continue;
    bind_root_cbv(r, *roots[r]);
    s.bound_root_buffer[r] = roots[r]->buffer;
    s.bound_root_offset[r] = roots[r]->offset;
  }
#if defined(EOT_D3D12)
  if (s.bound_spec != spec) {
    cmd->setGraphicsPushConstants(kSpecPushConstantRangeIndex, &spec, 0, sizeof(spec));
    s.bound_spec = spec;
  }
#endif
  if (s.bound_root_buffer[2] != shared_alloc.buffer ||
      s.bound_root_offset[2] != shared_alloc.offset) {
    bind_root_cbv(2, shared_alloc);
    s.bound_root_buffer[2] = shared_alloc.buffer;
    s.bound_root_offset[2] = shared_alloc.offset;
  }
  lap(s.perf.rec_state_ms);
  auto bind_vertex_views = [&](u32 first, const plume::RenderVertexBufferView *want_views,
                               u32 count, const plume::RenderInputSlot *want_slots) {
    s.perf.vertex_bind_requests++;
    u32 first_dirty = count, last_dirty = 0;
    for (u32 i = 0; i < count; ++i) {
      const u32 slot = first + i;
      const auto &want = want_views[i];
      const auto &input = want_slots[i];
      const auto &have = s.bound_vertex_streams[slot];
      if (!have.valid || have.buffer != want.buffer.ref || have.offset != want.buffer.offset ||
          have.size != want.size || have.stride != input.stride) {
        first_dirty = std::min(first_dirty, i);
        last_dirty = i;
      }
    }
    if (first_dirty == count)
      return;
    const u32 dirty_count = last_dirty - first_dirty + 1;
    cmd->setVertexBuffers(first + first_dirty, want_views + first_dirty, dirty_count,
                          want_slots + first_dirty);
    s.perf.vertex_bind_calls++;
    for (u32 i = first_dirty; i <= last_dirty; ++i) {
      const u32 slot = first + i;
      auto &have = s.bound_vertex_streams[slot];
      have.buffer = want_views[i].buffer.ref;
      have.offset = want_views[i].buffer.offset;
      have.size = want_views[i].size;
      have.stride = want_slots[i].stride;
      have.valid = true;
    }
  };
  bind_vertex_views(0, pk.views, pk.max_slot + 1, pk.slots);
  if (layout->needsSyntheticSlot) {
    plume::RenderVertexBufferView zv(plume::RenderBufferReference(pk.zero.buffer, pk.zero.offset),
                                     4096);
    plume::RenderInputSlot zs(kSyntheticVertexSlot, 0);
    bind_vertex_views(kSyntheticVertexSlot, &zv, 1, &zs);
  }
  lap(s.perf.rec_bind_ms);
  auto bind_index_view = [&](const plume::RenderIndexBufferView &want) {
    s.perf.index_bind_requests++;
    auto &have = s.bound_index_stream;
    if (have.valid && have.buffer == want.buffer.ref && have.offset == want.buffer.offset &&
        have.size == want.size && have.format == want.format)
      return;
    cmd->setIndexBuffer(&want);
    s.perf.index_bind_calls++;
    have.buffer = want.buffer.ref;
    have.offset = want.buffer.offset;
    have.size = want.size;
    have.format = want.format;
    have.valid = true;
  };
  const u32 index_count = pk.index_count;
  if (pk.rectList) {
    plume::RenderIndexBufferView ib(
        plume::RenderBufferReference(pk.index_alloc.buffer, pk.index_alloc.offset), index_count * 4,
        plume::RenderFormat::R32_UINT);
    bind_index_view(ib);
    cmd->drawIndexedInstanced(index_count, 1, 0, 0, 0);
  } else if (pk.hasCached) {
    const u32 elem = pk.cached.is32 ? 4 : 2;
    u64 capacity = 0;
    plume::RenderBuffer *chunk = PoolResidentBuffer(s, index_pool(), pk.cached.buffer,
                                                    pk.cached.offset, u64(index_count) * elem,
                                                    &capacity);
    const u64 view_bytes = capacity ? capacity : pk.cached.offset + u64(index_count) * elem;
    plume::RenderIndexBufferView ib(
        plume::RenderBufferReference(chunk, 0), static_cast<u32>(std::min<u64>(view_bytes, 0xFFFFFFFFu)),
        pk.cached.is32 ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
    bind_index_view(ib);
    cmd->drawIndexedInstanced(index_count, 1, static_cast<u32>(pk.cached.offset / elem),
                              pk.host_base_vertex, 0);
  } else if (pk.indexed) {
    plume::RenderIndexBufferView ib(
        plume::RenderBufferReference(pk.index_alloc.buffer, pk.index_alloc.offset), index_count * 4,
        plume::RenderFormat::R32_UINT);
    bind_index_view(ib);
    cmd->drawIndexedInstanced(index_count, 1, 0, pk.host_base_vertex, 0);
  } else {
    cmd->drawInstanced(pk.vertexCount, 1, static_cast<u32>(pk.host_base_vertex), 0);
  }
  lap(s.perf.record_ms);
  if (Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame()) && ProbeArmed())
    ProbeAfterDrawLocked(s, targets.colorCount ? targets.colorImage[0] : nullptr, targets.depthImage,
                         targets.width, targets.height, targets.scale, vs->hash, ps ? ps->hash : 0, pk.prim,
                         pk.indexed ? pk.index_count : pk.vertexCount, dev.U32(dev::kDepthControl),
                         reinterpret_cast<const float *>(pk.ps_consts.cpu), pk.ps_consts.size);
  DrainHostDebugMessages(s, "draw");
}

void ExecuteDraw(u32 device_va, u32 prim, GeometryPlan &geom,
                 FloatConstantDirty constants, const u8 *device_image = nullptr) {
  auto &s = state();
  s.vs_float_constants_stale |= constants.vs;
  s.ps_float_constants_stale |= constants.ps;
  QueryNoteDraw(prim == kPrimRectList);
  if (EmulateMemExportDraw(device_va, geom.indexed && !geom.indices.empty() ? geom.indices.data() : nullptr,
                           static_cast<u32>(geom.indices.size()), geom.startVertex,
                           geom.indexed ? 0u : geom.vertexCount))
    return;
  // Fable 2: point lists are point sprites (weather particles); nothing writes
  // a point size yet and Metal leaves it undefined (screen-filling garbage).
  if (prim == 1) {
    // Fable 2's point lists: log each distinct (vs, ps) once; draw only those
    // whose pixel shader is listed in fable2_point_ps_allow (or all when
    // fable2_skip_point_lists is off).
    DeviceView dev = Device(device_va);
    u32 vs_va = dev.U32(dev::kVertexShader), ps_va = dev.U32(dev::kPixelShader);
    if (const u32 owned = g_gpu_owned_vs.load(std::memory_order_relaxed))
      vs_va = owned;
    if (const u32 owned = g_gpu_owned_ps.load(std::memory_order_relaxed))
      ps_va = owned == kGpuOwnedNone ? 0 : owned;
    const GuestShader *vs = vs_va ? FindGuestShader(s, vs_va, false) : nullptr;
    if (!vs && vs_va)
      vs = RegisterGuestShader(s, vs_va, false);
    const GuestShader *ps = ps_va ? FindGuestShader(s, ps_va, true) : nullptr;
    if (!ps && ps_va)
      ps = RegisterGuestShader(s, ps_va, true);
    const u64 vs_hash = vs ? vs->hash : 0, ps_hash = ps ? ps->hash : 0;
    {
      static std::mutex mutex;
      static std::unordered_set<u64> seen;
      std::lock_guard lock(mutex);
      if (seen.size() < 64 && seen.insert(vs_hash * 31 + ps_hash).second)
        EOT_INFO("[points] vs {:016x} ps {:016x} n {} indexed {} rt0 {:#x} ds {:#x} blend0 {:#x} cc {:#x} "
                 "dc {:#x} psize-export {}",
                 vs_hash, ps_hash, geom.indexed ? geom.indices.size() : geom.vertexCount, geom.indexed,
                 dev.U32(dev::kRenderTarget0), dev.U32(dev::kDepthSurface), dev.U32(dev::kBlendControl0),
                 dev.U32(dev::kColorControl), dev.U32(dev::kDepthControl), vs && vs->entry ? 1 : 0);
    }
    bool allowed = !REXCVAR_GET(fable2_skip_point_lists);
    if (!allowed && ps && !REXCVAR_GET(fable2_point_ps_allow).empty()) {
      char hex[17];
      std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(ps->hash));
      allowed = REXCVAR_GET(fable2_point_ps_allow).find(hex) != std::string::npos;
    }
    if (!allowed) {
      Dropped("point sprites not supported yet", 0x6031);
      return;
    }
  }
  if (!s.ready)
    return;
  EOT_CPU_ZONE("ExecuteDraw");
  PerfScope perf_scope(s.perf.capture_ms);
  s.perf.draws++;
  if (RenderThreadActive()) {
    {
      RenderEnqueue enqueue;
      RenderCommand &c = enqueue.cmd();
      c.type = RenderCommandType::Draw;
      if (CaptureDraw(s, device_va, prim, geom, device_image, c.draw))
        enqueue.commit(false);
    }
    PoolDrainRetired(s, vertex_mirrors().pool);
    PoolDrainRetired(s, pull_mirrors().pool);
    return;
  }
  static thread_local DrawPacket packet;
  const bool captured = CaptureDraw(s, device_va, prim, geom, device_image, packet);
  PoolDrainRetired(s, vertex_mirrors().pool);
  PoolDrainRetired(s, pull_mirrors().pool);
  if (!captured)
    return;
  std::lock_guard video_lock(s.mutex);
  ReplayDrawLocked(s, packet);
}

}

void ReplayDrawLocked(VideoState &s, const DrawPacket &pk) {
  if (!s.ready)
    return;
  PerfScope perf_scope(s.perf.draw_ms);
  BeginCommandList(s);
  if (!s.command_list_open)
    return;
  ReplayDraw(s, pk);
  s.list_draws++;
}

void FlushGeometryStaging(VideoState &s) {
  if (!s.command_list_open)
    return;
  PoolFlushToVram(s, index_pool(), plume::RenderBufferFlag::INDEX);
  PoolFlushToVram(s, vertex_mirrors().pool, plume::RenderBufferFlag::VERTEX);
  PoolFlushToVram(s, pull_mirrors().pool, plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::CONSTANT);
}

void GeometryCacheBytes(GeometryCacheSizes *out) {
  *out = GeometryCacheSizes{};
  auto sum = [](BufferPool &pool, u64 &upload, u64 &vram, u32 &chunks) {
    std::lock_guard lock(pool.mutex);
    for (const auto &ch : pool.chunks) {
      upload += ch.buffer ? ch.capacity : 0;
      vram += ch.vram ? ch.capacity : 0;
      chunks++;
    }
  };
  sum(index_pool(), out->indexUpload, out->indexVram, out->indexChunks);
  sum(vertex_mirrors().pool, out->vertexUpload, out->vertexVram, out->vertexChunks);
}

void FlushPendingTransitions(VideoState &s) {
  if (!s.pending_transition_count)
    return;
  const u32 n = s.pending_transition_count;
  s.pending_transition_count = 0;
  TransitionManyLocked(s, s.pending_transitions, n);
}
namespace {

}

// Fable 2: tessellated quad patch draws (the landscape). The vertex shader
// computes each vertex from r0 = (patch index, u, v), so every patch is drawn
// as a uniform grid of fable2_tess_segments^2 quads (the guest's adaptive
// per-edge factors are not applied).
void DrawGuestTessellatedPatches(u32 device_va, u32 first_patch, u32 patch_count,
                                 FloatConstantDirty constants) {
  const u32 segments = static_cast<u32>(std::clamp(REXCVAR_GET(fable2_tess_segments), 0, 64));
  const u32 row = segments + 1;
  if (!patch_count || !segments || u64(patch_count) * row * row > (1u << 22)) {
    auto &s = state();
    std::lock_guard lock(s.guest_mutex);
    s.vs_float_constants_stale |= constants.vs;
    s.ps_float_constants_stale |= constants.ps;
    if (patch_count && segments)
      Dropped("tessellated draw too large", 0x6094);
    return;
  }
  GeometryPlan g;
  g.topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  g.indexed = true;
  g.tessSegments = segments;
  g.tessFirstPatch = first_patch;
  g.vertexCount = patch_count * row * row;
  {
    auto &s = state();
    PerfScope index_scope(s.perf.index_ms);
    g.cached = GetCachedGridIndices(s, segments, patch_count);
  }
  // The grid is the same for every draw with this segment and patch count.
  static thread_local std::vector<u32> grid;
  static thread_local u32 grid_segments = 0, grid_patches = 0;
  if (g.cached) {
    // indices stay empty: the draw uses the cached grid
  } else if (grid_segments != segments || grid_patches != patch_count) {
    grid.clear();
    grid.reserve(size_t(patch_count) * segments * segments * 6);
    for (u32 p = 0; p < patch_count; ++p) {
      const u32 base = p * row * row;
      for (u32 j = 0; j < segments; ++j)
        for (u32 i = 0; i < segments; ++i) {
          const u32 v00 = base + j * row + i, v10 = v00 + 1, v01 = v00 + row, v11 = v01 + 1;
          grid.insert(grid.end(), {v00, v10, v11, v00, v11, v01});
        }
    }
    grid_segments = segments;
    grid_patches = patch_count;
  }
  if (!g.cached)
    g.indices = grid;
  // Debug: no vertex is discarded by its theme weight (set fable2_full_constant_upload too).
  u32 *threshold = REXCVAR_GET(fable2_tess_no_discard)
                       ? mem::at<u32>(device_va + dev::kVsFloatConstants + 114 * 16)
                       : nullptr;
  const u32 saved = threshold ? *threshold : 0;
  if (threshold)
    *threshold = std::byteswap(std::bit_cast<u32>(-1.0f));
  ExecuteDraw(device_va, kPrimTriangleList, g, constants);
  if (threshold)
    *threshold = saved;
}

void DrawGuestPrimitives(u32 device_va, u32 prim, u32 start_vertex, u32 vertex_count,
                         FloatConstantDirty constants) {
  if (vertex_count > (1u << 20)) {
    u32 n;
    if (DiagShouldLog(0x6092, &n) && n < 8)
      EOT_WARN("[draw] draw with {} vertices (prim {}) dropped", vertex_count, prim);
    Dropped("vertex count implausibly large", 0x6093);
    vertex_count = 0;
  }
  if (!vertex_count) {
    auto &s = state();
    std::lock_guard lock(s.guest_mutex);
    s.vs_float_constants_stale |= constants.vs;
    s.ps_float_constants_stale |= constants.ps;
    return;
  }
  GeometryPlan g;
  bool expand = false;
  g.topology = ConvertPrimitiveType(prim, &expand);
  g.startVertex = start_vertex;
  g.vertexCount = vertex_count;
  if (prim == kPrimRectList) {
    g.rectList = true;
  } else if (expand) {
    g.indices.resize(vertex_count);
    for (u32 i = 0; i < vertex_count; ++i)
      g.indices[i] = start_vertex + i;
    ExpandIndices(prim, g.indices, false);
    g.indexed = true;
    g.baseVertex = 0;
    g.topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
  ExecuteDraw(device_va, prim, g, constants);
}

void QueueGuestUpDraw(u32 device_va, u32 prim, u32 vertex_count, u32 stride, u32 data_va,
                      FloatConstantDirty constants) {
  FlushPendingUpDraw();
  auto &s = state();
  std::lock_guard lock(s.guest_mutex);
  s.pending_up.valid = data_va != 0 && vertex_count != 0;
  s.pending_up_armed.store(s.pending_up.valid, std::memory_order_release);
  s.pending_up.device_va = device_va;
  s.pending_up.primitive_type = prim;
  s.pending_up.vertex_count = vertex_count;
  s.pending_up.stride = stride;
  s.pending_up.data_va = data_va;
  s.pending_up.index_va = 0;
  s.pending_up.index_count = 0;
  s.pending_up.index32 = false;
  s.pending_up.vs_constants_dirty = constants.vs;
  s.pending_up.ps_constants_dirty = constants.ps;
  if (!s.pending_up.valid) {
    s.vs_float_constants_stale |= constants.vs;
    s.ps_float_constants_stale |= constants.ps;
  }
  s.pending_up.has_image = false;
  if (const u8 *image = mem::at<u8>(device_va)) {
    s.pending_up.device_image.assign(image, image + kDeviceSnapshotBytes);
    s.pending_up.has_image = true;
  }
}

void QueueGuestIndexedUpDraw(u32 device_va, u32 prim, u32 vertex_count, u32 stride, u32 vertex_va,
                             u32 index_count, u32 index_va, bool index32,
                             FloatConstantDirty constants) {
  QueueGuestUpDraw(device_va, prim, vertex_count, stride, vertex_va, constants);
  auto &s = state();
  std::lock_guard lock(s.guest_mutex);
  if (!s.pending_up.valid)
    return;
  if (!index_va || !index_count) {
    s.pending_up.valid = false;
    s.pending_up_armed.store(false, std::memory_order_release);
    s.vs_float_constants_stale |= constants.vs;
    s.ps_float_constants_stale |= constants.ps;
    return;
  }
  s.pending_up.index_va = index_va;
  s.pending_up.index_count = index_count;
  s.pending_up.index32 = index32;
}

void FlushPendingUpDraw() {
  auto &s = state();
  if (!s.pending_up_armed.load(std::memory_order_acquire))
    return;
  VideoState::PendingUpDraw up;
  {
    std::lock_guard lock(s.guest_mutex);
    if (!s.pending_up.valid)
      return;
    up = s.pending_up;
    s.pending_up.valid = false;
    s.pending_up_armed.store(false, std::memory_order_relaxed);
  }
  GeometryPlan g;
  bool expand = false;
  g.topology = ConvertPrimitiveType(up.primitive_type, &expand);
  g.startVertex = 0;
  g.vertexCount = up.vertex_count;
  g.stream0OverrideVa = up.data_va;
  g.stream0OverrideStride = up.stride;
  if (up.index_count) {
    // BeginIndexedVertices: big-endian indices into the vertex data.
    g.indices.resize(up.index_count);
    const u32 index_size = up.index32 ? 4 : 2;
    const u8 *src = mem::at<u8>(up.index_va);
    if (!src)
      return;
    for (u32 i = 0; i < up.index_count; ++i) {
      const u8 *p = src + i * index_size;
      g.indices[i] = up.index32 ? (u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3])
                                : (u32(p[0]) << 8 | p[1]);
    }
    g.indexed = true;
    if (expand) {
      ExpandIndices(up.primitive_type, g.indices, false);
      g.topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    }
  } else if (up.primitive_type == kPrimRectList) {
    g.rectList = true;
  } else if (expand) {
    g.indices.resize(up.vertex_count);
    for (u32 i = 0; i < up.vertex_count; ++i)
      g.indices[i] = i;
    ExpandIndices(up.primitive_type, g.indices, false);
    g.indexed = true;
    g.topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
  trace::Bump(trace::Counter::DrawVertices);
  ExecuteDraw(up.device_va, up.primitive_type, g,
              {up.vs_constants_dirty, up.ps_constants_dirty},
              up.has_image ? up.device_image.data() : nullptr);
}

void DrawGuestIndexedPrimitives(u32 device_va, u32 prim, i32 base_vertex, u32 start_index,
                                u32 index_count, FloatConstantDirty constants) {
  // Fable 2: guard against bogus index counts (reading and expanding them
  // stalled whole frames).
  if (index_count > (1u << 20)) {
    u32 n;
    if (DiagShouldLog(0x6090, &n) && n < 8)
      EOT_WARN("[draw] indexed draw with {} indices (prim {} start {} base {}) dropped",
               index_count, prim, start_index, base_vertex);
    Dropped("index count implausibly large", 0x6091);
    index_count = 0;
  }
  if (!index_count) {
    auto &s = state();
    std::lock_guard lock(s.guest_mutex);
    s.vs_float_constants_stale |= constants.vs;
    s.ps_float_constants_stale |= constants.ps;
    return;
  }
  auto &s = state();
  DeviceView dev = Device(device_va);
  GeometryPlan g;
  bool expand = false;
  g.topology = ConvertPrimitiveType(prim, &expand);
  g.indexed = true;
  g.baseVertex = base_vertex;
  {
    PerfScope index_scope(s.perf.index_ms);
    if (const CachedIndexRange *cached =
            GetCachedIndexRange(s, dev.U32(dev::kIndexBuffer), prim, start_index, index_count)) {
      g.cached = cached;
      g.topology = cached->topology;
    }
  }
  if (g.cached) {
    ExecuteDraw(device_va, prim, g, constants);
    return;
  }
  {
    PerfScope index_scope(s.perf.index_ms);
    if (!ReadGuestIndices(dev.U32(dev::kIndexBuffer), start_index, index_count, g.indices)) {
      std::lock_guard lock(s.guest_mutex);
      s.vs_float_constants_stale |= constants.vs;
      s.ps_float_constants_stale |= constants.ps;
      Dropped("index buffer unreadable", 0x6020);
      return;
    }
    if (prim == kPrimRectList) {
      g.rectList = true;
    } else if (expand || prim == kPrimTriangleStrip || prim == kPrimLineStrip) {
      ExpandIndices(prim, g.indices, true);
      g.topology = prim == kPrimLineStrip ? plume::RenderPrimitiveTopology::LINE_LIST
                                          : plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    }
  }
  ExecuteDraw(device_va, prim, g, constants);
}

namespace {

bool CaptureClear(u32 device_va, u32 flags, u32 rect_va, u32 color_va, float z, u32 stencil,
                  ClearPacket &pk) {
  pk = ClearPacket{};
  pk.device_va = device_va;
  pk.flags = flags;
  pk.z = z;
  pk.stencil = stencil;
  DeviceView dev = Device(device_va);
  if (const u8 *live = mem::at<u8>(device_va)) {
    dev.snapshot = live;
    dev.snapshotSize = dev::kDeviceSize;
  }
  if (!CaptureTargetWords(dev, pk.targets))
    return false;
  if (rect_va) {
    pk.hasRect = true;
    for (u32 i = 0; i < 4; ++i)
      pk.rect[i] = mem::load<i32>(rect_va + 4 * i);
  }
  if (color_va) {
    for (u32 i = 0; i < 4; ++i)
      pk.rgba[i] = mem::f32at(color_va + 4 * i);
  }
  return true;
}

}

void ClearGuestTargets(u32 device_va, u32 flags, u32 rect_va, u32 color_va, float z, u32 stencil) {
  auto &s = state();
  if (!s.ready)
    return;
  if (RenderThreadActive()) {
    RenderEnqueue enqueue;
    RenderCommand &c = enqueue.cmd();
    c.type = RenderCommandType::Clear;
    if (CaptureClear(device_va, flags, rect_va, color_va, z, stencil, c.clear))
      enqueue.commit(false);
    return;
  }
  ClearPacket pk;
  if (!CaptureClear(device_va, flags, rect_va, color_va, z, stencil, pk))
    return;
  std::lock_guard video_lock(s.mutex);
  ReplayClearLocked(s, pk);
}

void ReplayClearLocked(VideoState &s, const ClearPacket &pk) {
  if (!s.ready)
    return;
  BeginCommandList(s);
  if (!s.command_list_open)
    return;
  Targets targets;
  if (!ResolveTargetsFromWords(s, pk.targets, targets))
    return;
  const u32 flags = pk.flags, stencil = pk.stencil;
  const float z = pk.z;
  const float(&rgba)[4] = pk.rgba;
  const plume::RenderColor color(rgba[0], rgba[1], rgba[2], rgba[3]);
  plume::RenderRect rect;
  const plume::RenderRect *rects = nullptr;
  u32 rect_count = 0;
  bool whole = true;
  if (pk.hasRect) {
    const i32 gx0 = pk.rect[0], gy0 = pk.rect[1];
    const i32 gx1 = pk.rect[2], gy1 = pk.rect[3];
    whole = gx0 <= 0 && gy0 <= 0 && gx1 >= static_cast<i32>(targets.width) &&
            gy1 >= static_cast<i32>(targets.height);
    if (!whole) {
      const float k = targets.scale;
      rect = plume::RenderRect(ScalePxBy(gx0, k), ScalePxBy(gy0, k), ScalePxBy(gx1, k),
                               ScalePxBy(gy1, k));
      rects = &rect;
      rect_count = 1;
    }
  }
  const bool clear_depth = (flags & 0x10) != 0, clear_stencil = (flags & 0x20) != 0;
  if (clear_depth && whole && targets.depth)
    velocity::OnDepthCleared(s, *targets.depth);
  auto *cmd = s.command_list;
  if (GpuTimingDiagActive(s)) {
    GpuTimingDiagMark(s, cmd, std::format("clear flags {:#x}", flags));
    EOT_INFO("[diag] clear flags {:#x} {} rt0={:#x} ds={:#x} {}x{} rgba=({:.2f},{:.2f},{:.2f},{:.2f}) z={:.3f} s={} "
             "tiles=({},{})",
             flags, whole ? "whole" : "rect", targets.colorCount ? targets.color[0]->va : 0,
             targets.depth ? targets.depth->va : 0, targets.width, targets.height, rgba[0], rgba[1],
             rgba[2], rgba[3], z, stencil & 0xFF,
             targets.colorCount ? static_cast<i32>(targets.color[0]->baseTile) : -1,
             targets.depth ? static_cast<i32>(targets.depth->baseTile) : -1);
  }
  auto clears_all = [&](const HostTexture &image) {
    return clear_depth && (clear_stencil || !FormatHasStencil(image.format));
  };
  auto clear_depth_image = [&](const HostTexture &image, const plume::RenderRect *r, u32 n) {
    const bool with_stencil = clear_stencil && FormatHasStencil(image.format);
    if (clear_depth || with_stencil)
      cmd->clearDepthStencil(clear_depth, with_stencil, z, stencil & 0xFF, r, n);
  };
  auto clear_image = [&](GuestSurface &surf, HostTexture &image) {
    if (whole && (!surf.isDepth || clears_all(image)))
      image.needsClear = false;
    HostTexture *colors[4] = {surf.isDepth ? nullptr : &image, nullptr, nullptr, nullptr};
    if (!BindImages(s, colors, surf.isDepth ? 0u : 1u, surf.isDepth ? &image : nullptr, true))
      return;
    if (surf.isDepth)
      clear_depth_image(image, rects, rect_count);
    else
      cmd->clearColor(0, color, rects, rect_count);
    surf.perfClears++;
  };
  for (u32 i = 0; i < targets.colorCount; ++i) {
    if (!(flags & (1u << i)))
      continue;
    GuestSurface &surf = *targets.color[i];
    HostTexture &owner = SurfaceContentImage(surf);
    const bool both = SurfaceHasSingle(surf) &&
                      (whole || surf.imagesAgree || surf.content != GuestSurface::Content::Drawn);
    if (both) {
      if (surf.host.valid())
        clear_image(surf, surf.host);
      clear_image(surf, surf.single);
    } else {
      clear_image(surf, owner);
    }
    NoteSurfaceClearedColor(surf, owner, rgba, whole, both);
  }
  if (targets.depth && (flags & 0x30)) {
    GuestSurface &surf = *targets.depth;
    if (whole && !targets.colorCount && clear_depth)
      SurfaceRedirectBegin(s, surf);
    if (surf.redirectMirror && surf.redirectMirror->host.valid()) {
      HostTexture &image = surf.redirectMirror->host;
      plume::RenderRect region(surf.redirectX, surf.redirectY,
                               surf.redirectX + static_cast<i32>(surf.host.width),
                               surf.redirectY + static_cast<i32>(surf.host.height));
      if (!whole)
        region = plume::RenderRect(rect.left + surf.redirectX, rect.top + surf.redirectY,
                                   rect.right + surf.redirectX, rect.bottom + surf.redirectY);
      if (BindImages(s, nullptr, 0, &image, true)) {
        if (GpuTimingDiagActive(s))
          GpuTimingDiagMark(s, cmd, std::format("redirect region clear {}x{}", surf.host.width,
                                                surf.host.height));
        clear_depth_image(image, &region, 1);
        surf.perfClears++;
      }
      NoteSurfaceClearedDepth(surf, z, static_cast<u8>(stencil & 0xFF), whole && clears_all(surf.host), false);
    } else {
      const bool both = SurfaceHasSingle(surf);
      if (surf.host.valid())
        clear_image(surf, surf.host);
      if (both)
        clear_image(surf, surf.single);
      NoteSurfaceClearedDepth(surf, z, static_cast<u8>(stencil & 0xFF), whole && clears_all(surf.host), both);
    }
  }
  s.bound_draw_targets_valid = false;
  DrainHostDebugMessages(s, "clear");
}

// Temporary diagnostics: texture dumps queued by draws, run at a safe point
// (the start of a resolve) because a dump submits the command list.
void RunPendingTextureDumpsLocked(VideoState &s) {
  for (const auto &[t, path] : g_pending_bound_dumps)
    if (t && t->host.texture)
      DumpHostTextureLocked(s, t->host, path.c_str(), 1.0f, kInvalidDescriptorIndex, kIdentityFetchSwizzle);
  g_pending_bound_dumps.clear();
  for (const auto &d : g_pending_mip_dumps)
    if (d.texture && d.texture->host.texture)
      DumpHostTextureLocked(s, d.texture->host, d.path.c_str(), 1.0f, kInvalidDescriptorIndex,
                            kIdentityFetchSwizzle, d.level, d.alpha);
  g_pending_mip_dumps.clear();
  for (const auto &[tex_va, tag] : g_pending_texture_dumps) {
    GuestTexture *gt = GetGuestTexture(s, tex_va, false);
    if (!gt || !gt->host.texture)
      continue;
    char path[256];
    if (tag >= 1000) {  // raw dumps of the shadow mask pass inputs
      std::snprintf(path, sizeof(path), "logs/raw_slot%u_%08x.bin", tag - 1000, tex_va);
      DumpHostRawLocked(s, gt->host, path);
      continue;
    }
    std::snprintf(path, sizeof(path), "logs/mat%u_tex%u_%08x.ppm", tag / 16, tag % 16, tex_va);
    DumpHostTextureLocked(s, gt->host, path, 1.0f, kInvalidDescriptorIndex, kIdentityFetchSwizzle);
    for (u32 level = 1; level < 3 && level < gt->host.mipLevels; ++level) {
      std::snprintf(path, sizeof(path), "logs/mat%u_tex%u_%08x_mip%u.ppm", tag / 16, tag % 16, tex_va, level);
      DumpHostMipBc1Locked(s, gt->host, level, path);
    }
  }
  g_pending_texture_dumps.clear();
}


bool DepthPrerunTrigger(const DrawPacket &pk) {
  return REXCVAR_GET(fable2_zpass_emulation) && pk.ps && !REXCVAR_GET(fable2_zpass_trigger_ps).empty() &&
         DrawMatchesList(REXCVAR_GET(fable2_zpass_trigger_ps), pk.ps->hash, 0);
}

// Which of the scene's draws the emulated Z pass runs: the ones that write
// depth with a non-strict test. The scene's own depth is put back after the
// early resolve (fable2_zpass_restore_depth), so these only shape the depth the
// shadow mask reads; characters' depth draws (alpha- or stencil-tested) must be
// in it, or their bodies get the mask of what stands behind them (see-through
// Rose in the light). The fable2_zpass_skip_* switches are for debugging.
bool DepthPrerunIncludes(const DrawPacket &pk) {
  if (pk.rectList || !pk.vs)
    return false;
  if (REXCVAR_GET(fable2_zpass_skip_tess) && pk.tess_segments)
    return false;
  if (const std::string &skip = REXCVAR_GET(fable2_zpass_skip_vs);
      !skip.empty() && skip.find(std::format("{:016x}", pk.vs->hash)) != std::string::npos)
    return false;
  DeviceView dev = Device(pk.device_va);
  dev.snapshot = pk.window.image;
  dev.snapshotSize = kDeviceSnapshotBytes;
  dev.pages = DeviceWindow::PageTable();
  const u32 dc = dev.U32(dev::kDepthControl), cc = dev.U32(dev::kColorControl);
  if ((dc & 6) != 6)  // depth test and depth write
    return false;
  const u32 func = (dc >> 4) & 7;
  if (func != 3 && func != 6 && func != 7)  // LEQUAL, GEQUAL, ALWAYS
    return false;
  if (REXCVAR_GET(fable2_zpass_skip_a2c) && (cc & (1u << 4)))
    return false;
  if (REXCVAR_GET(fable2_zpass_skip_alpha_test) && (cc & (1u << 3)) && (cc & 7) != 7)
    return false;
  if (REXCVAR_GET(fable2_zpass_skip_stencil_test) && (dc & 1)) {
    const u32 front = (dc >> 8) & 7, back = (dc >> 20) & 7;
    const bool two_sided = (dc & (1u << 7)) != 0;
    if (front != 7 || (two_sided && back != 7))  // anything but ALWAYS rejects pixels
      return false;
  }
  return true;
}

// Debug (black holes): "first,count" limits the emulated Z pass to those of its
// draws, by their order in it; the diag frame lists them.
bool DepthPrerunDebugKeeps(u32 index, const DrawPacket &pk) {
  const std::string &range = REXCVAR_GET(fable2_zpass_debug_range);
  if (Settings::DiagFrame() > 0 && state().guest_frames + 1 == static_cast<u64>(Settings::DiagFrame()))
    EOT_INFO("[zpass] #{} vs {:016x} ps {:016x} n {}", index, pk.vs ? pk.vs->hash : 0, pk.ps ? pk.ps->hash : 0,
             pk.indexed ? pk.index_count : pk.vertexCount);
  if (range.empty())
    return true;
  u32 first = 0, count = 0;
  if (std::sscanf(range.c_str(), "%u,%u", &first, &count) != 2)
    return true;
  return index >= first && index - first < count;
}

bool DepthPrerunFeeds(const DrawPacket &trigger, const ResolvePacket &r) {
  if (!r.hasDestFetch || !r.srcVa || !trigger.ps)
    return false;
  // Only a depth resolve, and only into a texture the trigger's pixel shader
  // reads: the other slots keep stale pooled headers, which can name e.g. the
  // dog's coat composite (run early, it was read back black).
  const u32 format = StorageFormat(r.destFetch[1] & 0x3F);
  if (format != static_cast<u32>(xe::TextureFormat::k_24_8) &&
      format != static_cast<u32>(xe::TextureFormat::k_24_8_FLOAT))
    return false;
  const u32 key = RenderTargetKey(r.destFetch);
  const u32 used = trigger.ps->textureFetchMask & trigger.texHeaderMask & 0xFFFFu;
  for (u32 slot = 0; slot < 16; ++slot)
    if ((used & (1u << slot)) && RenderTargetKey(trigger.texHeaders[slot]) == key)
      return true;
  return false;
}

}
