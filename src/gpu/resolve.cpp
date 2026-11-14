// gpu/resolve.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

#include "core/logging.h"
#include <rex/cvar.h>
#include "core/memory_helpers.h"
#include "gpu/backend.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/gpu_timing.h"
#include "gpu/render_thread.h"

#include "gpu/draw.h"
#include "gpu/format.h"
#include "gpu/settings.h"
#include "gpu/patches/aspect_ratio.h"
#include "gpu/surfaces.h"
#include "gpu/textures.h"
#include "gpu/trace.h"
#include "gpu/writeback.h"
#include "gpu/taa.h"

REXCVAR_DECLARE(bool, fable2_debug_hero_composite);
REXCVAR_DECLARE(bool, fable2_texture_as_recorded);
REXCVAR_DECLARE(bool, fable2_render_targets_by_storage);
REXCVAR_DECLARE(bool, fable2_debug_skip_bake_chains);
REXCVAR_DEFINE_BOOL(fable2_debug_skip_auto_mip_build, false, "Fable2",
                    "Debug: fable2_bake_auto_mips drops the game's mip passes but builds none (cost test)");
REXCVAR_DEFINE_BOOL(fable2_bake_auto_mips, true, "Fable2",
                    "Native renderer: the per-character 256x256 textures get their mip levels built on the GPU "
                    "right after level 0 is resolved, instead of replaying the game's draw + resolve per level "
                    "(6 fewer render passes per texture)");
REXCVAR_DECLARE(bool, fable2_diag_bake);

namespace eot::gpu {
void NoteBakeSurface(u32 va);

// fable2_bake_auto_mips. Fable 2 renders ~12 characters a frame into 256x256
// k_8_8_8_8 textures with 4 levels, then builds levels 1-3 itself: per level, a
// bilinear quad (ps 2cb8bcd53db53c93) sampling the level above into a scratch
// surface and a resolve into the level. On the Xbox that is cheap; here every
// draw and every resolve is a render pass (~26 us each on the GPU). The
// capture side drops that sequence and the level-0 resolve's replay builds the
// levels on the GPU from level 0 instead (a 2:1 bilinear sample = the game's).
// Game thread only (draw and resolve capture).
namespace {
constexpr u64 kBakeMipPs = 0x2cb8bcd53db53c93ull;
struct AutoMipChain {
  u32 destVa = 0;     // 0 = no chain open
  u32 nextLevel = 0;  // the level whose draw + resolve come next
  u32 levels = 0;
  bool drawTaken = false;  // that level's draw was dropped; its resolve is due
} g_auto_mip;
u64 g_auto_mip_breaks = 0;

void AutoMipChainBreak(const char *why) {
  if (g_auto_mip.drawTaken && (g_auto_mip_breaks++ & 1023) == 0)
    EOT_WARN("[auto-mips] chain into {:#x} broken at level {} ({}); a mip draw was already dropped",
             g_auto_mip.destVa, g_auto_mip.nextLevel, why);
  g_auto_mip = AutoMipChain{};
}
}  // namespace

// Called for every captured draw while a chain is open: true = drop the draw.
bool AutoMipChainTakesDraw(u64 ps_hash) {
  if (!g_auto_mip.destVa)
    return false;
  if (ps_hash == kBakeMipPs && !g_auto_mip.drawTaken) {
    g_auto_mip.drawTaken = true;
    state().perf.auto_mip_skips++;
    return true;
  }
  AutoMipChainBreak("another draw");
  return false;
}
void AutoMipChainOtherCommand() {
  if (g_auto_mip.destVa)
    AutoMipChainBreak("another command");
}

namespace xe = rex::graphics::xenos;
namespace tu = rex::graphics::texture_util;

namespace {

bool MirrorAtScale(const GuestTexture &t, float k) {
  return t.host.width == ScaleDimBy(t.width, k) && t.host.height == ScaleDimBy(t.height, k);
}

bool LocateAlias(const GuestTexture &d, const GuestTexture &t, i32 &tx, i32 &ty) {
  if (t.format != d.format || t.tiled != d.tiled)
    return false;
  const u32 pitch = ((d.fetch[0] >> 22) & 0x1FF) * 32;
  if (!pitch || pitch != ((t.fetch[0] >> 22) & 0x1FF) * 32)
    return false;
  const rex::graphics::FormatInfo *fi =
      rex::graphics::FormatInfo::Get(static_cast<u32>(d.format));
  if (!fi || fi->block_width != 1 || fi->block_height != 1)
    return false;
  const u32 bpb = fi->bytes_per_block();
  if (bpb < 4)
    return false;
  u32 bpb_log2 = 0;
  while ((1u << bpb_log2) < bpb)
    ++bpb_log2;
  const bool t_after = t.baseAddress >= d.baseAddress;
  const u32 delta = t_after ? t.baseAddress - d.baseAddress : d.baseAddress - t.baseAddress;
  i32 ox = 0, oy = 0;
  if (d.tiled) {
    const u32 tile_bytes = 32 * 32 * bpb;
    const u32 tiles_per_row = pitch / 32;
    if (!tiles_per_row || delta % tile_bytes)
      return false;
    const u32 tile = delta / tile_bytes;
    ox = static_cast<i32>((tile % tiles_per_row) * 32);
    oy = static_cast<i32>((tile / tiles_per_row) * 32);
    if (tu::GetTiledOffset2D(ox, oy, pitch, bpb_log2) != static_cast<i32>(delta))
      return false;
  } else {
    const u32 row_bytes = pitch * bpb;
    oy = static_cast<i32>(delta / row_bytes);
    ox = static_cast<i32>((delta % row_bytes) / bpb);
  }
  tx = t_after ? ox : -ox;
  ty = t_after ? oy : -oy;
  return true;
}

i32 Signed6(u32 v) {
  v &= 0x3F;
  return v & 0x20 ? static_cast<i32>(v) - 64 : static_cast<i32>(v);
}

// Bits of each stored component (x | y << 4 | z << 8 | w << 12) for resolve
// destination formats narrower than their host format (0: none). The console's
// resolve rounds to the destination's bits: a 1-bit alpha snaps to 0 or 1. Tree
// impostor atlases (k_1_5_5_5) build their mips by downsampling the level above
// and resolving it; without the rounding, alpha thinned at every level and the
// alpha test dropped distant trees.
u32 ResolveQuantizeBits(xe::TextureFormat format) {
  switch (format) {
  case xe::TextureFormat::k_1_5_5_5:
    return 5u | 5u << 4 | 5u << 8 | 1u << 12;
  case xe::TextureFormat::k_5_6_5:
    return 5u | 6u << 4 | 5u << 8;
  case xe::TextureFormat::k_4_4_4_4:
    return 4u | 4u << 4 | 4u << 8 | 4u << 12;
  case xe::TextureFormat::k_2_10_10_10:
  case xe::TextureFormat::k_2_10_10_10_AS_16_16_16_16:
    return 10u | 10u << 4 | 10u << 8 | 2u << 12;
  default:
    return 0u;
  }
}

u32 ResolveStoreSwizzle(u32 fetch3) {
  const u32 sw = (fetch3 >> 1) & 0xFFF;
  constexpr u32 kSwapRedBlue = 0x60A;
  return (sw & 7) == 2 ? (0x80000000u | kSwapRedBlue) : 0u;
}

void ClearSource(VideoState &s, GuestSurface &surf, const float *clear_rgba, float clear_z,
                 const i32 *guest_rect = nullptr) {
  surf.redirectMirror.reset();
  float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  if (!surf.isDepth && clear_rgba) {
    for (u32 i = 0; i < 4; ++i)
      rgba[i] = clear_rgba[i];
  }
  auto clear_image = [&](HostTexture &image) {
    HostTexture *colors[4] = {surf.isDepth ? nullptr : &image, nullptr, nullptr, nullptr};
    plume::RenderFramebuffer *fb =
        GetFramebuffer(s, colors, surf.isDepth ? 0u : 1u, surf.isDepth ? &image : nullptr);
    if (!fb)
      return;
    TransitionLocked(s, image,
                     surf.isDepth ? plume::RenderTextureLayout::DEPTH_WRITE
                                  : plume::RenderTextureLayout::COLOR_WRITE);
    if (s.bound_framebuffer != fb)
      s.command_list->setFramebuffer(fb);
    s.bound_framebuffer = fb;
    s.bound_draw_targets_valid = false;
    // Fable 2: a resolve clears only the rectangle it resolved (predicated
    // tiling resolves and clears one tile at a time; clearing the whole surface
    // erased the tiles not resolved yet).
    plume::RenderRect rect;
    const plume::RenderRect *rects = nullptr;
    if (guest_rect && surf.allocWidth && surf.allocHeight) {
      const float kx = static_cast<float>(image.width) / static_cast<float>(surf.allocWidth);
      const float ky = static_cast<float>(image.height) / static_cast<float>(surf.allocHeight);
      rect = plume::RenderRect(static_cast<i32>(guest_rect[0] * kx), static_cast<i32>(guest_rect[1] * ky),
                               static_cast<i32>(guest_rect[2] * kx), static_cast<i32>(guest_rect[3] * ky));
      rects = &rect;
    }
    if (surf.isDepth)
      s.command_list->clearDepthStencil(true, FormatHasStencil(image.format), clear_z, 0, rects,
                                        rects ? 1 : 0);
    else
      s.command_list->clearColor(0, plume::RenderColor(rgba[0], rgba[1], rgba[2], rgba[3]), rects,
                                 rects ? 1 : 0);
    if (!rects)
      image.needsClear = false;
  };
  if (surf.host.valid())
    clear_image(surf.host);
  if (SurfaceHasSingle(surf))
    clear_image(surf.single);
  if (surf.isDepth)
    NoteSurfaceClearedDepth(surf, clear_z, 0, true, SurfaceHasSingle(surf));
  else
    NoteSurfaceClearedColor(surf, surf.host, rgba, true, true);
  surf.perfClears++;
}

bool CaptureResolve(u32 device_va, u32 flags, u32 src_rect_va, u32 dest_texture_va,
                    u32 dest_point_va, u32 dest_level, u32 clear_color_va, float clear_z,
                    ResolvePacket &pk) {
  pk = ResolvePacket{};
  pk.device_va = device_va;
  pk.flags = flags;
  pk.destVa = dest_texture_va;
  pk.hasDestFetch = dest_texture_va && ReadTextureHeader(dest_texture_va, pk.destFetch);
  pk.destLevel = dest_level;
  pk.clearZ = clear_z;
  const DeviceView dev = Device(device_va);
  const u32 source = flags & 7;
  const bool depth_source = source == 4;
  pk.srcVa = depth_source ? dev.U32(dev::kDepthSurface)
                          : dev.U32(dev::kRenderTarget0 + 4 * (source & 3));
  if (pk.srcVa && !ReadSurfaceHeaderWords(pk.srcVa, pk.srcWords))
    pk.srcVa = 0;
  if (src_rect_va) {
    pk.hasRect = true;
    for (u32 i = 0; i < 4; ++i)
      pk.rect[i] = mem::load<i32>(src_rect_va + 4 * i);
  }
  if (dest_point_va) {
    pk.hasPoint = true;
    pk.point[0] = mem::load<i32>(dest_point_va);
    pk.point[1] = mem::load<i32>(dest_point_va + 4);
  }
  if (clear_color_va) {
    pk.hasColor = true;
    for (u32 i = 0; i < 4; ++i)
      pk.rgba[i] = mem::f32at(clear_color_va + 4 * i);
  }
  if (REXCVAR_GET(fable2_diag_bake) && pk.destVa) {
    static u32 last_dest = 0, last_src = 0;
    if (pk.destLevel == 0) {
      last_dest = pk.destVa;
      last_src = pk.srcVa;
    } else if (pk.destLevel == 1 && pk.destVa == last_dest && last_src) {
      NoteBakeSurface(last_src);
    }
    static u32 lines = 0;
    if (lines < 3000) {
      ++lines;
      EOT_INFO("[bake] f {} resolve src {:#x} dest {:#x} level {} rect {}", state().guest_frames, pk.srcVa, pk.destVa,
               pk.destLevel, pk.hasRect ? pk.rect[2] : -1);
    }
  }
  if (g_auto_mip.destVa) {
    if (!depth_source && pk.destVa == g_auto_mip.destVa && dest_level == g_auto_mip.nextLevel &&
        g_auto_mip.drawTaken) {
      state().perf.auto_mip_skips++;
      g_auto_mip.drawTaken = false;
      if (++g_auto_mip.nextLevel >= g_auto_mip.levels)
        g_auto_mip = AutoMipChain{};
      return false;  // built from level 0 already
    }
    AutoMipChainBreak("another resolve");
  }
  if (REXCVAR_GET(fable2_bake_auto_mips) && !depth_source && dest_level == 0 && pk.hasDestFetch &&
      !pk.hasRect && !pk.hasPoint) {
    rex::graphics::xenos::xe_gpu_texture_fetch_t f;
    std::memcpy(&f, pk.destFetch, sizeof(f));
    rex::graphics::TextureInfo info{};
    const bool prepared = rex::graphics::TextureInfo::Prepare(f, &info);
    if (prepared && info.width + 1 == 256 && REXCVAR_GET(fable2_diag_bake)) {
      static u32 n = 0;
      if (n++ < 40)
        EOT_INFO("[auto-mips] candidate {:#x}: {}x{} depth {} dim {} mips {}-{} fmt {} rect {} point {}", pk.destVa,
                 info.width + 1, info.height + 1, info.depth + 1, static_cast<u32>(info.dimension),
                 info.mip_min_level, info.mip_max_level, static_cast<u32>(info.format), pk.hasRect, pk.hasPoint);
    }
    if (prepared && info.width + 1 == 256 && info.height + 1 == 256 &&
        info.dimension == xe::DataDimension::k2DOrStacked && info.depth == 0 && info.mip_min_level == 0 &&
        info.mip_max_level >= 1 &&
        (info.format == xe::TextureFormat::k_1_5_5_5 || info.format == xe::TextureFormat::k_8_8_8_8)) {
      pk.autoMipLevels = info.mip_max_level + 1;
      g_auto_mip = AutoMipChain{pk.destVa, 1, pk.autoMipLevels, false};
    }
  }
  if ((flags & 0x200) && !depth_source) {
    pk.dsVa = dev.U32(dev::kDepthSurface);
    if (pk.dsVa && !ReadSurfaceHeaderWords(pk.dsVa, pk.dsWords))
      pk.dsVa = 0;
  }
  return true;
}

}

void ResolveGuest(u32 device_va, u32 flags, u32 src_rect_va, u32 dest_texture_va,
                  u32 dest_point_va, u32 dest_level, u32 clear_color_va, float clear_z) {
  auto &s = state();
  if (!s.ready)
    return;
  const bool clear_only = TakeMovieResolveSkip();
  if (RenderThreadActive()) {
    // The hero's and dog's textures are read by the CPU straight after the
    // resolve: wait until the render thread has stored them in guest memory.
    u32 fetch[6] = {};
    if (dest_texture_va)
      for (u32 i = 0; i < 6; ++i)
        fetch[i] = mem::load<u32>(dest_texture_va + obj::kTextureFetch + 4 * i);
    const bool hero = !clear_only && dest_texture_va && IsHeroTextureFetch(fetch);
    u64 seq = 0;
    bool queued = false;
    {
      RenderEnqueue enqueue;
      RenderCommand &c = enqueue.cmd();
      c.type = RenderCommandType::Resolve;
      if (CaptureResolve(device_va, flags, src_rect_va, dest_texture_va, dest_point_va, dest_level,
                         clear_color_va, clear_z, c.resolve)) {
        c.resolve.clearDestOnly = clear_only;
        seq = enqueue.commit(hero);
        queued = true;
      }
    }
    if (hero && queued) {
      PerfScope wait_scope(s.perf.hero_resolve_wait_ms);
      s.perf.hero_resolve_waits++;
      RenderThreadWait(seq);
    }
    return;
  }
  ResolvePacket pk;
  if (!CaptureResolve(device_va, flags, src_rect_va, dest_texture_va, dest_point_va, dest_level,
                      clear_color_va, clear_z, pk))
    return;
  pk.clearDestOnly = clear_only;
  std::lock_guard video_lock(s.mutex);
  ReplayResolveLocked(s, pk);
}

// The destination as its header was when the game resolved: the game re-points
// headers at other render targets later in the frame, and the replay runs behind.
static GuestTexture *ResolveDestination(VideoState &s, const ResolvePacket &pk) {
  if (pk.hasDestFetch && REXCVAR_GET(fable2_render_targets_by_storage)) {
    const u32 key = RenderTargetKey(pk.destFetch);
    return GetGuestTextureForFetch(s, key, pk.destFetch, false);
  }
  return pk.hasDestFetch && REXCVAR_GET(fable2_texture_as_recorded)
             ? GetGuestTextureForFetch(s, pk.destVa, pk.destFetch, false)
             : GetGuestTexture(s, pk.destVa, false);
}

static void ClearResolveDestination(VideoState &s, const ResolvePacket &pk) {
  GuestSurface *surf = pk.srcVa ? GetGuestSurfaceWords(s, pk.srcVa, pk.srcWords) : nullptr;
  GuestTexture *dest = pk.destVa ? ResolveDestination(s, pk) : nullptr;
  if (!surf || !dest || surf->isDepth || !EnsureResolveMirror(s, *dest, false, surf->scale) ||
      !dest->host.renderable)
    return;
  for (u32 m = 0; m < dest->host.mipLevels; ++m) {
    plume::RenderFramebuffer *fb = GetMipFramebuffer(s, *dest, m);
    if (!fb)
      continue;
    TransitionLocked(s, dest->host, plume::RenderTextureLayout::COLOR_WRITE);
    s.command_list->setFramebuffer(fb);
    s.command_list->clearColor(0, plume::RenderColor(0, 0, 0, 0), nullptr, 0);
  }
  s.bound_framebuffer = nullptr;
  s.bound_pipeline = nullptr;
  s.bound_draw_targets_valid = false;
  dest->host.needsClear = false;
  dest->uploaded = true;
  dest->uploadedUnlockSeq = ResourceUnlockSeq(dest->va);
  dest->contentSerial++;
  dest->lastUseFrame = s.guest_frames;
}

REXCVAR_DEFINE_BOOL(fable2_debug_skip_small_resolves, false, "Fable2", "Debug: skip 256x256 colour resolves (cost measurement)");
REXCVAR_DEFINE_BOOL(fable2_merge_tile_clears, true, "Fable2",
                    "Native renderer: predicated tiling's per-tile clears after its resolves become one whole "
                    "clear at the last tile (a pass start's load action) instead of a pass per tile");
REXCVAR_DEFINE_BOOL(fable2_diag_dead_resolves, false, "Fable2", "Diagnostic: resolves by kind, how many are never sampled");
REXCVAR_DEFINE_BOOL(fable2_diag_resolve_copies, false, "Fable2", "Diagnostic: log which resolve copies move the most pixels");
static void ReplayResolveImplLocked(VideoState &s, const ResolvePacket &pk);

void RunPendingTextureDumpsLocked(VideoState &s);

void ReplayResolveLocked(VideoState &s, const ResolvePacket &pk) {
  if (s.ready && s.command_list_open)
    RunPendingTextureDumpsLocked(s);
  ReplayResolveImplLocked(s, pk);
  if (pk.autoMipLevels > 1 && s.ready && !pk.clearDestOnly && s.command_list_open &&
      !REXCVAR_GET(fable2_debug_skip_auto_mip_build))
    if (GuestTexture *dest = ResolveDestination(s, pk))
      GenerateMipsLocked(s, *dest, pk.autoMipLevels, ResolveQuantizeBits(dest->format));
  if (s.ready && !pk.clearDestOnly && pk.destVa && s.command_list_open)
    if (GuestTexture *dest = ResolveDestination(s, pk))
      if (QueueResolveWritebackLocked(s, *dest))
        CompleteWritebacksNowLocked(s);  // the CPU reads it right after the resolve
}

static void ReplayResolveImplLocked(VideoState &s, const ResolvePacket &pk) {
  if (!s.ready)
    return;
  const u32 flags = pk.flags, dest_texture_va = pk.destVa, dest_level = pk.destLevel;
  const float clear_z = pk.clearZ;
  BeginCommandList(s);
  if (!s.command_list_open)
    return;
  if (pk.clearDestOnly) {
    ClearResolveDestination(s, pk);
    return;
  }
  EOT_CPU_ZONE("ResolveGuest");
  PerfScope perf_scope(s.perf.resolve_ms);
  s.perf.resolves++;
  if (pk.refresh)
    s.perf.resolve_refreshes++;
  FlushPendingTransitions(s);
  GpuTimingMark(s, s.command_list, kGpuCatResolve);

  const u32 source = flags & 7;
  const bool depth_source = source == 4;
  const u32 src_va = pk.srcVa;
  GuestSurface *surf = src_va ? GetGuestSurfaceWords(s, src_va, pk.srcWords) : nullptr;
  if (!surf || !SurfaceHasImage(*surf)) {
    u32 n;
    if (DiagShouldLog(0x7001, &n))
      EOT_WARN("[resolve] source {} has no bound surface (flags {:#x})", source, flags);
    return;
  }
  surf->perfResolves++;
  // Debug: skip the small colour resolves (256x256 k_8_8_8_8, per character in
  // Bowerstone town, most never sampled) to measure what they cost.
  if (REXCVAR_GET(fable2_debug_skip_small_resolves) && !depth_source && surf->width == 256 && surf->height == 256) {
    s.perf.resolves_skipped_debug++;
    return;
  }

  if (!depth_source && surf->content == GuestSurface::Content::Borrowed)
    SurfaceTakeBack(s, *surf);
  HostTexture *src_host = depth_source ? &surf->host : &SurfaceContentImage(*surf);
  GuestSurface *alias_src = nullptr;
  {
    PerfScope msaa_scope(s.perf.msaa_scan_ms);
    alias_src = FindMultisampleAliasSource(s, *surf);
  }
  if (alias_src) {
    src_host = depth_source ? &alias_src->host : SurfaceContentPeek(s, *alias_src);
    if (!src_host)
      return;
    u32 n;
    if (DiagShouldLog(0x7400 ^ src_va, &n) && n == 0)
      EOT_DEBUG("[resolve] {:#x}: {}x{} {}x-msaa alias -> sampling {}x{} 1x surface {:#x}", src_va,
               surf->width, surf->height, surf->msaaSamples, alias_src->width, alias_src->height,
               alias_src->va);
  }
  if (src_host->sampleCount > 1) {
    GuestSurface &src_surf = alias_src ? *alias_src : *surf;
    if (HostTexture *single = SurfaceColorSingle(s, src_surf))
      src_host = single;
  }
  if (!src_host->valid()) {
    u32 n;
    if (DiagShouldLog(0x7B00 ^ src_va, &n))
      EOT_WARN("[resolve] {:#x}: no image to read (the {}x image not made, no twin)", src_va,
               surf->host.sampleCount);
    return;
  }

  GuestTexture *dest = nullptr;
  {
    EOT_CPU_ZONE("resolve destination mirror");
    PerfScope mirror_scope(s.perf.resolve_mirror_ms);
    dest = dest_texture_va ? ResolveDestination(s, pk) : nullptr;
    if (dest && !depth_source && REXCVAR_GET(fable2_debug_skip_bake_chains) && dest->width == 256 &&
        dest->height == 256 && dest->mipLevels > 1) {
      s.perf.resolves_skipped_debug++;
      return;
    }
    if (dest && !EnsureResolveMirror(s, *dest, depth_source, surf->scale,
                                     depth_source ? surf->host.format : plume::RenderFormat::UNKNOWN))
      dest = nullptr;
  }

  i32 x0 = 0, y0 = 0, x1 = static_cast<i32>(surf->width), y1 = static_cast<i32>(surf->height);
  if (pk.hasRect) {
    x0 = pk.rect[0];
    y0 = pk.rect[1];
    x1 = pk.rect[2];
    y1 = pk.rect[3];
  }
  i32 dx = 0, dy = 0;
  if (pk.hasPoint) {
    dx = pk.point[0];
    dy = pk.point[1];
  }
  x0 = std::max(0, x0);
  y0 = std::max(0, y0);
  x1 = std::min(static_cast<i32>(surf->width), x1);
  y1 = std::min(static_cast<i32>(surf->height), y1);
  const i32 rw = x1 - x0, rh = y1 - y0;

  const bool whole_src = x0 == 0 && y0 == 0 && rw == static_cast<i32>(surf->width) &&
                         rh == static_cast<i32>(surf->height);
  bool redirect_hit = false;
  if (depth_source && surf->redirectMirror) {
    const i32 hx = ScalePxBy(dx, surf->scale), hy = ScalePxBy(dy, surf->scale);
    if (dest == surf->redirectMirror.get() && whole_src && dest_level == 0 &&
        hx == surf->redirectX && hy == surf->redirectY) {
      redirect_hit = true;
      src_host = &surf->redirectMirror->host;
    } else if (!SurfaceRedirectEnd(s, *surf)) {
      return;
    }
  }
  const bool src_is_region = redirect_hit;
  const i32 src_origin_x = src_is_region ? surf->redirectX : 0;
  const i32 src_origin_y = src_is_region ? surf->redirectY : 0;

  if (dest && rw > 0 && rh > 0 && dest_level < dest->host.mipLevels) {
    const i32 copy_exp = Signed6(flags >> 26);
    const i32 tex_exp = Signed6(dest->fetch[3] >> 13);
    const i32 rt_exp = surf->colorExpBias;
    const i32 net_exp = rt_exp + copy_exp + tex_exp;
    const float scale = 1.0f;
    {
      u32 n;
      const bool diag_now =
          Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame());
      if ((DiagShouldLog(0x7100 ^ dest_texture_va ^ (dest_level << 8), &n) && n == 0) || diag_now) {
        EOT_DEBUG("[resolve] {} {:#x} -> tex {:#x} mip {} rect {},{}-{},{} at {},{} exp rt{} "
                  "copy{} tex{} => x{} ({} -> host fmt {}) fetch=[{:08x} {:08x} {:08x} {:08x}] "
                  "base {:#x} pitch {} {}x{} {}",
                  depth_source ? "depth" : "color", src_va, dest_texture_va, dest_level, x0, y0,
                  x1, y1, dx, dy, rt_exp, copy_exp, tex_exp, std::ldexp(1.0f, net_exp),
                  static_cast<u32>(dest->format), static_cast<u32>(dest->host.format),
                  dest->fetch[0], dest->fetch[1], dest->fetch[2], dest->fetch[3],
                  dest->baseAddress, ((dest->fetch[0] >> 22) & 0x1FF) * 32, dest->width,
                  dest->height, dest->tiled ? "tiled" : "linear");
      }
    }
    std::shared_ptr<GuestTexture> dest_ref;
    if (auto it = s.textures.find(dest_texture_va); it != s.textures.end())
      dest_ref = it->second;
    auto mark = [&](GuestTexture &target, u32 level) {
      if (!target.resolveOwned)
        NoteResolveDestination(s, target);
      target.resolveOwned = true;
      target.resolveProvisional = false;
      target.uploaded = true;
      target.uploadedUnlockSeq = ResourceUnlockSeq(target.va);
      target.resolvedMipMask |= 1u << level;
      target.lastUseFrame = s.guest_frames;
      const bool dead = target.lastResolvedFrame && target.lastSampledFrame < target.lastResolvedFrame;
      if (dead) {
        s.perf.dead_resolves++;
        target.perfDeadResolves++;
      }
      if (REXCVAR_GET(fable2_diag_dead_resolves)) {
        // Per kind of resolve: how many, how many dead (the destination was not
        // sampled since its previous resolve), and the pixels moved.
        struct Kind {
          u64 n = 0, dead = 0, px = 0, written = 0;
        };
        static std::unordered_map<std::string, Kind> kinds;
        static u64 frames0 = s.guest_frames;
        const std::string key = std::format("{} {}x{} lvl{} fmt{} from {}x{}{}", depth_source ? "depth" : "colour",
                                            target.width, target.height, level, static_cast<u32>(target.format),
                                            surf->width, surf->height, surf->msaaSamples > 1 ? " ms" : "");
        Kind &k = kinds[key];
        ++k.n;
        if (dead)
          ++k.dead;
        k.px += u64(std::max(1u, target.width >> level)) * std::max(1u, target.height >> level);
        if (s.guest_frames - frames0 >= 600) {
          std::vector<std::pair<std::string, Kind>> v(kinds.begin(), kinds.end());
          std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second.n > b.second.n; });
          for (size_t i = 0; i < std::min<size_t>(v.size(), 12); ++i)
            EOT_INFO("[deadres] {:.1f}/frame ({:.1f} dead) {:.2f} Mpx/frame: {}", double(v[i].second.n) / 600,
                     double(v[i].second.dead) / 600, double(v[i].second.px) / 600e6, v[i].first);
          kinds.clear();
          frames0 = s.guest_frames;
        }
      }
      target.lastResolvedFrame = s.guest_frames;
      target.perfResolves++;
    };
    plume::RenderViewport last_resolve_vp;
    plume::RenderRect last_resolve_sc;
    bool resolve_dynamic_valid = false;
    auto blit = [&](GuestTexture &target, u32 level, i32 vx, i32 vy, i32 vw, i32 vh, i32 sx0,
                    i32 sy0) -> bool {
      const i32 mip_w = static_cast<i32>(std::max(1u, target.width >> level));
      const i32 mip_h = static_cast<i32>(std::max(1u, target.height >> level));
      const bool whole = sx0 == 0 && sy0 == 0 && vx == 0 && vy == 0 &&
                         vw == static_cast<i32>(surf->width) &&
                         vh == static_cast<i32>(surf->height) && vw == mip_w && vh == mip_h;
      const bool same_format = src_host->format == target.host.format;
      const bool reorder = !depth_source && ResolveStoreSwizzle(target.fetch[3]) != 0;
      const bool covers_image = whole && target.host.mipLevels == 1;
      const bool ms_src = src_host->sampleCount > 1;
      const GuestSurface &content_surf = alias_src ? *alias_src : *surf;
      const i32 rect_now[6] = {vx, vy, vw, vh, sx0, sy0};
      if (s.guest_frames - target.handoffRegretResetFrame >= 128) {
        target.handoffRegretResetFrame = s.guest_frames;
        target.handoffRegretMask = 0;
      }
      if (target.resolveOrdinalFrame != s.guest_frames) {
        target.resolveOrdinalFrame = s.guest_frames;
        target.resolveOrdinal = 0;
      }
      const u32 ordinal = std::min(target.resolveOrdinal, 31u);
      const bool regretted = pk.refresh || ((target.handoffRegretMask >> ordinal) & 1u);
      if (!pk.refresh)
        target.resolveOrdinal++;
      const bool noop_now = target.resolvedSurfaceUid == content_surf.uid &&
          target.resolvedSurfaceSerial == content_surf.serial &&
          target.resolvedOwnSerial == target.contentSerial && target.resolvedLevel == level &&
          std::memcmp(target.resolvedRect, rect_now, sizeof(rect_now)) == 0 &&
          !target.host.needsClear && !reorder == !target.storeSwapRB;
      if (noop_now) {
        s.perf.resolve_noops++;
        mark(target, level);
        return true;
      }
      if (Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame()))
        EOT_INFO("[diag] resolve {:#x} -> {:#x} not a no-op: uid {}/{} serial {}/{} own {}/{} level {}/{} "
                 "rect {},{},{},{},{},{}/{},{},{},{},{},{} needsClear {} reorder {} swap {}",
                 src_va, dest_texture_va, target.resolvedSurfaceUid, content_surf.uid,
                 target.resolvedSurfaceSerial, content_surf.serial, target.resolvedOwnSerial,
                 target.contentSerial, target.resolvedLevel, level, target.resolvedRect[0],
                 target.resolvedRect[1], target.resolvedRect[2], target.resolvedRect[3],
                 target.resolvedRect[4], target.resolvedRect[5], vx, vy, vw, vh, sx0, sy0,
                 target.host.needsClear, reorder, target.storeSwapRB);
      TextureReleaseBorrower(s, target);
      if (target.aliasPending) {
        if (covers_image) {
          target.aliasPending = false;
          target.aliasSource.reset();
        } else {
          FlushAliasCopy(s, target);
          resolve_dynamic_valid = false;
        }
      }
      target.contentSerial++;
      target.resolvedSurfaceUid = content_surf.uid;
      target.resolvedSurfaceSerial = content_surf.serial;
      target.velocity = content_surf.isDepth ? velocity::HandleFor(content_surf, s.guest_frames)
                                             : VelocityHandle{};
      target.resolvedOwnSerial = target.contentSerial;
      target.resolvedLevel = level;
      std::memcpy(target.resolvedRect, rect_now, sizeof(rect_now));
      const float k = surf->scale;
      const i32 mip_w_host = static_cast<i32>(std::max(1u, target.host.width >> level));
      const i32 mip_h_host = static_cast<i32>(std::max(1u, target.host.height >> level));
      const bool at_k = MirrorAtScale(target, k);
      const float kx = at_k ? k : static_cast<float>(mip_w_host) / static_cast<float>(mip_w);
      const float ky = at_k ? k : static_cast<float>(mip_h_host) / static_cast<float>(mip_h);
      const i32 hx0 = ScalePxBy(vx, kx), hy0 = ScalePxBy(vy, ky), hx1 = ScalePxBy(vx + vw, kx),
                hy1 = ScalePxBy(vy + vh, ky);
      if (Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame()))
        EOT_INFO("[diag] resolve {:#x} dest rect v=({},{} {}x{}) mip {}x{} host {}x{} k {} kx {} ky {} "
                 "-> ({},{})-({},{}) src ({},{})",
                 dest_texture_va, vx, vy, vw, vh, mip_w, mip_h, mip_w_host, mip_h_host, k, kx, ky,
                 hx0, hy0, hx1, hy1, sx0, sy0);
      if (hx1 <= hx0 || hy1 <= hy0)
        return true;
      const i32 sx0h = ScalePxBy(sx0, k) + src_origin_x, sy0h = ScalePxBy(sy0, k) + src_origin_y;
      const i32 sw = at_k ? hx1 - hx0 : ScalePxBy(sx0 + vw, k) - ScalePxBy(sx0, k);
      const i32 sh = at_k ? hy1 - hy0 : ScalePxBy(sy0 + vh, k) - ScalePxBy(sy0, k);
      if (!at_k) {
        u32 n;
        if (DiagShouldLog(0x7A00 ^ target.va ^ static_cast<u32>(k * 1000.0f), &n) && n == 0)
          EOT_DEBUG("[resolve] {:#x} {}x{} at x{:.3f} (tile {}) into {:#x} {}x{} fmt {}, a mirror made at "
                    "x{:.3f} ({}x{} host): drawn scaled, {}x{} -> {}x{} host",
                    src_va, surf->width, surf->height, k, surf->baseTile, target.va, target.width,
                    target.height, static_cast<u32>(target.format), kx, target.host.width,
                    target.host.height, sw, sh, hx1 - hx0, hy1 - hy0);
      }
      const bool copy_fits = at_k && sx0h + (hx1 - hx0) <= static_cast<i32>(src_host->width) &&
                             sy0h + (hy1 - hy0) <= static_cast<i32>(src_host->height) &&
                             hx1 <= mip_w_host && hy1 <= mip_h_host;
      const bool whole_host = whole && src_host->width == static_cast<u32>(mip_w_host) &&
                              src_host->height == static_cast<u32>(mip_h_host);
      const bool own_image = src_host == &surf->single || src_host == &surf->host;
      if (Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame()) &&
          !depth_source && own_image)
        EOT_INFO("[diag] resolve {:#x} -> {:#x} hand-off gate: ordinal {} regretted {} (mask {:#x}) dest {} "
                 "ms {} alias {} fmt {} scale {} level {} whole_host {} covers {} array {} depth {} "
                 "samples {} flags {}/{} dim {}/{} committed {}/{}",
                 src_va, dest_texture_va, ordinal, regretted, target.handoffRegretMask, &target == dest && dest_ref,
                 ms_src, alias_src != nullptr, same_format, scale, level, whole_host, covers_image,
                 target.host.arraySize,
                 target.host.depth, target.host.sampleCount, static_cast<u32>(target.host.desc.flags),
                 static_cast<u32>(src_host->desc.flags), static_cast<u32>(target.host.desc.dimension),
                 static_cast<u32>(src_host->desc.dimension), target.host.desc.committed,
                 src_host->desc.committed);
      if (!regretted && &target == dest && dest_ref && !depth_source &&
          !ms_src && !alias_src && own_image && same_format && scale == 1.0f && level == 0 &&
          at_k && whole_host && covers_image && target.host.arraySize == 1 && target.host.depth == 1 &&
          target.host.sampleCount == 1 &&
          target.host.desc.flags == src_host->desc.flags &&
          target.host.desc.dimension == src_host->desc.dimension &&
          target.host.desc.committed == src_host->desc.committed &&
          target.host.transientPool == src_host->transientPool) {
        if (target.storeSwapRB != reorder) {
          target.storeSwapRB = reorder;
          target.bindingGeneration++;
        }
        target.host.needsClear = false;
        surf->handoffMirrorOrdinal = ordinal;
        GpuTimingMark(s, s.command_list, kGpuCatResolveMirror);
        SurfaceTransferToMirror(s, *surf, *src_host, target, dest_ref);
        src_host = &target.host;
        mark(target, level);
        return true;
      }
      if (!ms_src && !alias_src && same_format && scale == 1.0f && copy_fits &&
          (depth_source || ResolveQuantizeBits(target.format) == 0) &&
          (!depth_source || whole_host) && (covers_image || !target.host.needsClear) &&
          (!reorder || covers_image)) {
        if (covers_image)
          target.host.needsClear = false;
        if (target.storeSwapRB != reorder) {
          target.storeSwapRB = reorder;
          target.bindingGeneration++;
        }
        auto *cmd = s.command_list;
        const HostTextureTransition transitions[] = {
            {src_host, plume::RenderTextureLayout::COPY_SOURCE},
            {&target.host, plume::RenderTextureLayout::COPY_DEST}};
        TransitionManyLocked(s, transitions, 2);
        GpuTimingMark(s, s.command_list, kGpuCatResolveCopy);
        const plume::RenderBox box(sx0h, sy0h, sx0h + (hx1 - hx0), sy0h + (hy1 - hy0));
        if (GpuTimingDiagActive(s))
          GpuTimingDiagMark(s, cmd,
                            std::format("resolve copy {:#x} {}x{} {}", dest_texture_va, hx1 - hx0,
                                        hy1 - hy0, depth_source ? "depth" : "colour"));
        if (Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame()))
          EOT_INFO("[diag] resolve {:#x} copy to ({},{})-({},{}) of {}x{} from ({},{}) whole_host {}",
                   dest_texture_va, hx0, hy0, hx1, hy1, target.host.width, target.host.height, sx0h,
                   sy0h, whole_host);
        cmd->copyTextureRegion(
            plume::RenderTextureCopyLocation::Subresource(target.host.texture.get(), level, 0),
            plume::RenderTextureCopyLocation::Subresource(src_host->texture.get(), 0, 0),
            static_cast<u32>(hx0), static_cast<u32>(hy0), 0, whole_host ? nullptr : &box);
        s.perf.resolve_copies++;
        if (REXCVAR_GET(fable2_diag_resolve_copies)) {
          // Which copies cost: host pixels per frame by size, format and why not handed off.
          static std::unordered_map<std::string, u64> px;
          static u64 frames0 = s.guest_frames;
          const std::string key = std::format("{}x{} fmt{} {} whole_host {} covers {} ms {} dest_ref {}",
                                              hx1 - hx0, hy1 - hy0, static_cast<u32>(src_host->format),
                                              depth_source ? "depth" : "colour", whole_host, covers_image,
                                              src_host->sampleCount, dest_ref != nullptr);
          px[key] += u64(hx1 - hx0) * u64(hy1 - hy0);
          if (s.guest_frames - frames0 >= 600) {
            std::vector<std::pair<std::string, u64>> v(px.begin(), px.end());
            std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
            for (size_t i = 0; i < std::min<size_t>(v.size(), 8); ++i)
              EOT_INFO("[rcopy] {:.2f} Mpx/frame {}", double(v[i].second) / 600e6, v[i].first);
            px.clear();
            frames0 = s.guest_frames;
          }
        }
        mark(target, level);
        return true;
      }
      {
        u32 n;
        GpuTimingMark(s, s.command_list, kGpuCatResolveBlit);
        const u32 key = 0x7500 ^ (static_cast<u32>(src_host->format) << 8) ^
                        static_cast<u32>(target.host.format) ^ (reorder ? 0x40000 : 0) ^
                        (alias_src ? 0x80000 : 0) ^ (depth_source ? 0x100000 : 0) ^
                        (ms_src ? 0x200000 : 0);
        if (DiagShouldLog(key, &n) && n == 0)
          EOT_DEBUG("[resolve] blit kept: host fmt {} -> {}{}{}{}{}", static_cast<u32>(src_host->format),
                   static_cast<u32>(target.host.format), reorder ? " reorder" : "",
                   alias_src ? " msaa-alias" : "", depth_source && !whole ? " depth-subrect" : "",
                   ms_src ? " multisampled-source" : "");
      }
      if (Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame()))
        EOT_INFO("[diag] resolve {:#x} blit to ({},{})-({},{}) of {}x{} from ({},{}) needsClear {} at_k {}",
                 dest_texture_va, hx0, hy0, hx1, hy1, target.host.width, target.host.height, sx0h,
                 sy0h, target.host.needsClear, at_k);
      plume::RenderFramebuffer *fb = nullptr;
      {
        EOT_CPU_ZONE("resolve framebuffer");
        PerfScope fb_scope(s.perf.resolve_fb_ms);
        fb = GetMipFramebuffer(s, target, level);
      }
      if (!fb)
        return false;
      auto *cmd = s.command_list;
      PerfScope bind_scope(s.perf.resolve_bind_ms);
      const HostTextureTransition transitions[] = {
          {src_host, plume::RenderTextureLayout::SHADER_READ},
          {&target.host, depth_source ? plume::RenderTextureLayout::DEPTH_WRITE
                                      : plume::RenderTextureLayout::COLOR_WRITE}};
      TransitionManyLocked(s, transitions, 2);
      if (target.host.needsClear) {
        for (u32 m = 0; m < target.host.mipLevels; ++m) {
          plume::RenderFramebuffer *mfb = m == level ? fb : GetMipFramebuffer(s, target, m);
          if (!mfb)
            continue;
          if (s.bound_framebuffer != mfb) {
            cmd->setFramebuffer(mfb);
            s.bound_framebuffer = mfb;
          }
          s.bound_draw_targets_valid = false;
          if (target.host.isDepth)
            cmd->clearDepthStencil(true, FormatHasStencil(target.host.format), 0.0f, 0, nullptr, 0);
          else
            cmd->clearColor(0, plume::RenderColor(0, 0, 0, 0), nullptr, 0);
        }
        target.host.needsClear = false;
      }
      GpuTimingMark(s, cmd, depth_source ? kGpuCatResolveDepth : kGpuCatResolve);
      if (GpuTimingDiagActive(s))
        GpuTimingDiagMark(s, cmd,
                          std::format("resolve blit {:#x} {}x{} {}{}", dest_texture_va, hx1 - hx0,
                                      hy1 - hy0, depth_source ? "depth" : "colour",
                                      ms_src ? " ms" : ""));
      if (target.storeSwapRB) {
        target.storeSwapRB = false;
        target.bindingGeneration++;
      }
      if (s.bound_framebuffer != fb) {
        cmd->setFramebuffer(fb);
        s.bound_framebuffer = fb;
      }
      s.bound_draw_targets_valid = false;
      const plume::RenderFormat color_fmt = target.host.format;
      plume::RenderPipeline *pso =
          ms_src ? GetResolveMsaaPipeline(s, depth_source ? target.host.format : color_fmt,
                                          src_host->sampleCount, depth_source)
          : depth_source ? GetDepthCopyPipeline(s, target.host.format)
                         : GetBlitPipeline(s, color_fmt);
      if (!pso)
        return false;
      if (s.bound_pipeline != pso) {
        cmd->setPipeline(pso);
        s.bound_pipeline = pso;
      }
      plume::RenderViewport vp(static_cast<float>(hx0), static_cast<float>(hy0),
                               static_cast<float>(hx1 - hx0), static_cast<float>(hy1 - hy0), 0.0f,
                               1.0f);
      plume::RenderRect sc(hx0, hy0, hx1, hy1);
      if (!resolve_dynamic_valid ||
          std::memcmp(&last_resolve_vp, &vp, sizeof(last_resolve_vp)) != 0) {
        cmd->setViewports(&vp, 1);
        last_resolve_vp = vp;
      }
      if (!resolve_dynamic_valid ||
          std::memcmp(&last_resolve_sc, &sc, sizeof(last_resolve_sc)) != 0) {
        cmd->setScissors(&sc, 1);
        last_resolve_sc = sc;
      }
      resolve_dynamic_valid = true;
      CopyPushConstants pc;
      pc.resourceDescriptorIndex = BindTextureSRVLocked(s, *src_host);
      pc.resourceDescriptorIndex2 = depth_source ? 0u : ResolveStoreSwizzle(target.fetch[3]);
      pc.param0 = scale;
      pc.param1 = 0.0f;
      pc.extra[2] = depth_source ? 0.0f : static_cast<float>(ResolveQuantizeBits(target.format));
      const GuestSurface &src_surf = alias_src ? *alias_src : *surf;
      if (src_is_region) {
        pc.rect[0] = static_cast<float>(sx0h) / static_cast<float>(src_host->width);
        pc.rect[1] = static_cast<float>(sy0h) / static_cast<float>(src_host->height);
        pc.rect[2] = static_cast<float>(sx0h + sw) / static_cast<float>(src_host->width);
        pc.rect[3] = static_cast<float>(sy0h + sh) / static_cast<float>(src_host->height);
      } else {
        const float rx = static_cast<float>(src_surf.width) / static_cast<float>(surf->width);
        const float ry = static_cast<float>(src_surf.height) / static_cast<float>(surf->height);
        pc.rect[0] = static_cast<float>(sx0) * rx / static_cast<float>(src_surf.allocWidth);
        pc.rect[1] = static_cast<float>(sy0) * ry / static_cast<float>(src_surf.allocHeight);
        pc.rect[2] = static_cast<float>(sx0 + vw) * rx / static_cast<float>(src_surf.allocWidth);
        pc.rect[3] = static_cast<float>(sy0 + vh) * ry / static_cast<float>(src_surf.allocHeight);
      }
      bind_scope.stop();
      cmd->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &pc,
                                    kCopyPushConstantByteOffset, sizeof(pc));
      {
        EOT_GPU_ZONE("resolve blit");
        cmd->drawInstanced(3, 1, 0, 0);
      }
      mark(target, level);
      return true;
    };

    const u32 mip_w = std::max(1u, dest->width >> dest_level);
    const u32 mip_h = std::max(1u, dest->height >> dest_level);
    const i32 vx = std::clamp(dx, 0, static_cast<i32>(mip_w));
    const i32 vy = std::clamp(dy, 0, static_cast<i32>(mip_h));
    const i32 vw = std::min(rw, static_cast<i32>(mip_w) - vx);
    const i32 vh = std::min(rh, static_cast<i32>(mip_h) - vy);
    if (REXCVAR_GET(fable2_debug_hero_composite) && IsHeroTextureFetch(dest->fetch)) {
      static u32 logged = 0;
      if (logged++ < 40)
        EOT_INFO("[heroresolve] {:#x} -> {:#x} level {}: surf {}x{} alloc {}x{} scale {} tile {} rect {},{}-{},{} "
                 "(has {}) point {},{} (has {}) -> dest {},{} {}x{} of {}x{} host {}x{} flags {:#x} swapRB {} "
                 "redirect {}",
                 src_va, dest_texture_va, dest_level, surf->width, surf->height, surf->allocWidth,
                 surf->allocHeight, surf->scale, surf->baseTile, x0, y0, x1, y1, pk.hasRect, dx, dy, pk.hasPoint,
                 vx, vy, vw, vh, dest->width, dest->height, dest->host.width, dest->host.height, flags,
                 dest->storeSwapRB, redirect_hit);
    }
    if (whole_src && dest_level == 0 && !depth_source && vw > 0 && vh > 0 &&
        (vx != 0 || vy != 0 || vw < static_cast<i32>(mip_w) || vh < static_cast<i32>(mip_h))) {
      u32 n;
      if (DiagShouldLog(0x7900 ^ dest_texture_va ^ (static_cast<u32>(vw) << 12) ^ static_cast<u32>(vh), &n) && n == 0)
        EOT_DEBUG("[resolve] {:#x} {}x{} (alloc {}x{}, x{:.3f}, tile {}) covers part of texture {:#x} {}x{} fmt {} "
                  "(host {}x{}): {}x{} at {},{}, {}x{} host",
                  src_va, surf->width, surf->height, surf->allocWidth, surf->allocHeight, surf->scale, surf->baseTile,
                  dest_texture_va, dest->width, dest->height, static_cast<u32>(dest->format), dest->host.width,
                  dest->host.height, vw, vh, vx, vy, ScalePxBy(vw, surf->scale), ScalePxBy(vh, surf->scale));
    }
    if (vw > 0 && vh > 0 && redirect_hit) {
      dest->contentSerial++;
      dest->resolvedSurfaceUid = surf->uid;
      dest->resolvedSurfaceSerial = surf->serial;
      dest->velocity = surf->isDepth ? velocity::HandleFor(*surf, s.guest_frames) : VelocityHandle{};
      dest->resolvedOwnSerial = dest->contentSerial;
      dest->resolvedLevel = dest_level;
      const i32 rect_now[6] = {vx, vy, vw, vh, x0, y0};
      std::memcpy(dest->resolvedRect, rect_now, sizeof(rect_now));
      dest->host.needsClear = false;
      s.perf.resolve_noops++;
      mark(*dest, dest_level);
    } else if (vw > 0 && vh > 0) {
      blit(*dest, dest_level, vx, vy, vw, vh, x0, y0);
    }
    if (!depth_source && !pk.refresh)
      dest->lastResolve = std::make_shared<ResolvePacket>(pk);
    if (depth_source && whole_src && dest_level == 0 && vw == rw && vh == rh &&
        surf->host.sampleCount == 1 && dest_ref && dest->host.isDepth && dest->host.renderable &&
        dest->host.sampleCount == 1 && dest->host.mipLevels == 1 && dest->host.arraySize == 1 &&
        dest->host.format == surf->host.format && surf->redirectPassFrame == s.guest_frames &&
        surf->redirectPasses >= 1 && surf->redirectPasses <= GuestSurface::kRedirectPasses &&
        MirrorAtScale(*dest, surf->scale) &&
        (dest->host.width > surf->host.width || dest->host.height > surf->host.height)) {
      const i32 hx = ScalePxBy(vx, surf->scale), hy = ScalePxBy(vy, surf->scale);
      if (hx >= 0 && hy >= 0 && hx + surf->host.width <= dest->host.width &&
          hy + surf->host.height <= dest->host.height) {
        GuestSurface::RedirectPrediction &p = surf->redirectPredictions[surf->redirectPasses - 1];
        const bool same = p.recordedFrame + 1 == s.guest_frames && p.mirror.lock() == dest_ref &&
                          p.texture == dest->host.texture.get() && p.x == hx && p.y == hy;
        p.streak = same ? p.streak + 1 : 0;
        p.recordedFrame = s.guest_frames;
        p.mirror = dest_ref;
        p.texture = dest->host.texture.get();
        p.x = hx;
        p.y = hy;
      }
    }

    if (dest_level == 0 && vw > 0 && vh > 0) {
      PerfScope alias_scope(s.perf.alias_scan_ms);
      const u64 gen = (s.texture_generation.load(std::memory_order_relaxed) << 20) ^
                      s.mirror_generation;
      if (s.resolve_alias_candidates_generation != gen) {
        s.resolve_alias_candidates_generation = gen;
        s.resolve_alias_candidates.clear();
        for (auto &[va, tex] : s.textures) {
          if (tex && tex->host.texture && tex->host.renderable)
            s.resolve_alias_candidates.push_back(tex.get());
        }
      }
      s.perf.alias_scanned += static_cast<u32>(s.resolve_alias_candidates.size());
      const u64 visit_token = ++s.resolve_alias_token;
      for (GuestTexture *t : s.resolve_alias_candidates) {
        const u32 alias_va = t->va;
        if (t == dest || !t->host.texture || !t->host.renderable ||
            t->host.isDepth != depth_source)
          continue;
        if (t->uploaded && !t->resolveOwned)
          continue;
        if (t->aliasVisitToken == visit_token)
          continue;
        t->aliasVisitToken = visit_token;
        i32 tx, ty;
        if (!LocateAlias(*dest, *t, tx, ty))
          continue;
        const i32 ax0 = vx - tx, ay0 = vy - ty;
        const i32 cx0 = std::max(ax0, 0), cy0 = std::max(ay0, 0);
        const i32 cx1 = std::min(ax0 + vw, static_cast<i32>(t->width));
        const i32 cy1 = std::min(ay0 + vh, static_cast<i32>(t->height));
        if (cx1 <= cx0 || cy1 <= cy0)
          continue;
        if (t->resolveProvisional &&
            (!EnsureResolveMirror(s, *t, depth_source, surf->scale,
                                  depth_source ? surf->host.format : plume::RenderFormat::UNKNOWN) ||
             !t->host.texture || !t->host.renderable || t->host.isDepth != depth_source))
          continue;
        {
          u32 n;
          if (DiagShouldLog(0x7300 ^ alias_va ^ dest_texture_va, &n) && n == 0)
            EOT_DEBUG("[resolve] {:#x} also lands in {:#x} ({}x{} base {:#x}) at {},{} ({}x{})",
                     dest_texture_va, alias_va, t->width, t->height, t->baseAddress, cx0, cy0,
                     cx1 - cx0, cy1 - cy0);
        }
        const bool deferrable =
            dest_ref.get() == dest && tx == 0 && ty == 0 && vx == 0 && vy == 0 &&
            vw == static_cast<i32>(dest->width) && vh == static_cast<i32>(dest->height) &&
            cx1 == static_cast<i32>(t->width) && cy1 == static_cast<i32>(t->height) &&
            MirrorAtScale(*dest, surf->scale) && MirrorAtScale(*t, surf->scale) &&
            dest->host.valid() && t->host.mipLevels == 1 && t->host.arraySize == 1 &&
            t->host.depth == 1 && t->host.sampleCount == 1 && dest->host.sampleCount == 1 &&
            t->host.format == dest->host.format && t->host.width <= dest->host.width &&
            t->host.height <= dest->host.height &&
            (depth_source || ResolveStoreSwizzle(t->fetch[3]) == ResolveStoreSwizzle(dest->fetch[3]));
        std::shared_ptr<GuestTexture> t_ref;
        if (deferrable)
          if (auto it = s.textures.find(alias_va); it != s.textures.end() && it->second.get() == t)
            t_ref = it->second;
        if (t_ref) {
          TextureReleaseBorrower(s, *t);
          t->contentSerial++;
          t->aliasPending = true;
          t->aliasSource = dest_ref;
          t->aliasSourceImage = dest->host.texture.get();
          t->velocity = dest->velocity;
          auto &dependents = dest->aliasDependents;
          bool noted = false;
          for (const auto &weak : dependents)
            noted |= weak.lock() == t_ref;
          if (!noted) {
            std::erase_if(dependents, [](const std::weak_ptr<GuestTexture> &w) { return w.expired(); });
            dependents.push_back(t_ref);
          }
          s.perf.alias_deferred++;
          mark(*t, 0);
          continue;
        }
        blit(*t, 0, cx0, cy0, cx1 - cx0, cy1 - cy0, x0 + (cx0 - ax0), y0 + (cy0 - ay0));
      }
    }
    s.bound_pipeline = nullptr;
    s.bound_framebuffer = nullptr;
  } else if (!dest) {
    u32 n;
    if (DiagShouldLog(0x7002 ^ dest_texture_va, &n))
      EOT_WARN("[resolve] destination {:#x} has no usable host mirror", dest_texture_va);
  }

  if (Settings::DiagDump() && Settings::DiagFrame() > 0 &&
      s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame())) {
    static u32 k = 0;
    const std::string path = std::format("logs/f{}_r{}_{}_{:x}.ppm", s.guest_frames + 1, k++,
                                         depth_source ? "depth" : "color", src_va);
    if (surf->host.sampleCount == 1)
      DumpHostTextureLocked(s, surf->host, path.c_str(), depth_source ? 1.0f : 1.0f);
    if (s.command_list_open && dest && dest->host.texture && dest->host.sampleCount == 1 &&
        !depth_source) {
      const std::string dpath = std::format("logs/f{}_r{}_dest_{:x}.ppm", s.guest_frames + 1,
                                            k - 1, dest_texture_va);
      DumpHostTextureLocked(s, dest->host, dpath.c_str(), 1.0f);
      // HDR range: the same image scaled by 1/32.
      const std::string hpath = std::format("logs/f{}_r{}_hdr_{:x}.ppm", s.guest_frames + 1, k - 1,
                                            dest_texture_va);
      DumpHostTextureLocked(s, dest->host, hpath.c_str(), 1.0f / 32.0f);
    }
    if (!s.command_list_open)
      return;
  }
  const i32 resolved_rect[4] = {x0, y0, x1, y1};
  const i32 *clear_rect = whole_src ? nullptr : resolved_rect;
  // Predicated tiling resolves and clears one horizontal band at a time. When
  // the next queued command resolves and clears the band below with the same
  // values, this band's clear waits and that one clears the whole surface
  // (plume makes a whole clear the next pass's load action; nothing reads this
  // band in between: the next resolve copies only its own band).
  if (clear_rect && (flags & 0x300) && REXCVAR_GET(fable2_merge_tile_clears)) {
    if (s.tile_clear_deferred_va == pk.srcVa) {
      clear_rect = nullptr;
    } else if (const ResolvePacket *n = s.next_resolve;
               n && n->srcVa == pk.srcVa && (n->flags & 0x3FF) == (flags & 0x3FF) && n->hasRect && pk.hasRect &&
               n->dsVa == pk.dsVa && n->hasColor == pk.hasColor && n->clearZ == pk.clearZ &&
               (!pk.hasColor || std::memcmp(n->rgba, pk.rgba, sizeof(pk.rgba)) == 0) && x0 == 0 && y0 == 0 &&
               x1 == static_cast<i32>(surf->width) && n->rect[0] <= 0 && n->rect[2] >= x1 && n->rect[1] == y1 &&
               n->rect[3] >= static_cast<i32>(surf->height)) {
      s.tile_clear_deferred_va = pk.srcVa;
      s.bound_framebuffer = nullptr;
      DrainHostDebugMessages(s, "resolve");
      return;
    }
  }
  s.tile_clear_deferred_va = 0;
  if (flags & 0x100)
    ClearSource(s, *surf, pk.hasColor ? pk.rgba : nullptr, clear_z, clear_rect);
  if ((flags & 0x200) && depth_source)
    ClearSource(s, *surf, nullptr, clear_z, clear_rect);
  else if (flags & 0x200) {
    if (GuestSurface *ds = pk.dsVa ? GetGuestSurfaceWords(s, pk.dsVa, pk.dsWords) : nullptr)
      ClearSource(s, *ds, nullptr, clear_z, clear_rect);
  }
  s.bound_framebuffer = nullptr;
  DrainHostDebugMessages(s, "resolve");
}

}
