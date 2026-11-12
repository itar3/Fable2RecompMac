// gpu/writeback.cpp
// Fable 2: resolves the CPU reads back. The game measures scene luminance on
// the CPU from a small k_32_FLOAT resolve (a 280x180 luminance map no draw
// samples). On the console the resolve writes guest memory; the native
// renderer keeps resolves on the host GPU, so the CPU read stale bytes and the
// game set the tonemap's brightness and saturation to 0 (only bloom showed).
//
// After such a resolve the host image is copied into a readback buffer in the
// same command list; when the frame slot's fence is reached, the texels are
// averaged down to the guest size and stored at the guest texture's address in
// its tiled layout and endianness (a frame or two late, like the queries).

#include "gpu/writeback.h"

#include <algorithm>
#include <string>
#include <format>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>
#include <plume_vulkan.h>
#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device.h"

REXCVAR_DEFINE_BOOL(fable2_resolve_writeback, true, "Fable2",
                    "Native renderer: write small k_32_FLOAT resolves back to guest memory "
                    "(the CPU reads them for auto-exposure)");
REXCVAR_DEFINE_STRING(fable2_debug_probe, "", "Fable2",
                      "Debug: \"fx,fy\": log every frame-sized draw's colour and depth at that point of "
                      "its target in the next diag frame");
REXCVAR_DEFINE_BOOL(fable2_hero_texture_writeback, true, "Fable2",
                    "Native renderer: write the hero's and dog's composited k_8_8_8_8 texture "
                    "resolves back to guest memory (the CPU compresses them; black dog otherwise)");

namespace eot::gpu {

namespace xe = rex::graphics::xenos;
namespace tu = rex::graphics::texture_util;

namespace {

constexpr u32 kMaxGuestTexels = 512 * 512;
// Fable 2 composites the hero's and the dog's textures on the GPU, resolves
// them as k_8_8_8_8 to this physical address (the copy destination register;
// the texture's base is the page below) and compresses them on the CPU
// (unofficial-xenia-femtofork-for-fable-ii reads back the same resolves).
constexpr u32 kHeroTextureBase = 0x12704000;
constexpr u32 kMaxHeroTexels = 2048 * 2048;

struct Pending {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u32 slot = 0;
  u32 hostWidth = 0, hostHeight = 0, rowBytes = 0;
  u32 guestWidth = 0, guestHeight = 0, pitch = 0;
  u32 baseAddress = 0;
  u32 endian = 0;
  bool tiled = false;
  bool rgba8 = false;
  bool swapRB = false;  // guest storage has red and blue exchanged (see below)
  u32 va = 0;
};

std::vector<Pending> g_pending;

// Temporary diagnostics (black sky): one pixel per draw, see ProbeAfterDrawLocked.
struct ProbeEntry {
  u64 vs = 0, ps = 0;
  u32 prim = 0, count = 0, depthControl = 0;
  u32 x = 0, y = 0;
  plume::RenderFormat colourFormat = plume::RenderFormat::UNKNOWN;
  bool colour = false, depth = false;
  float c19[4] = {}, c29[4] = {};  // pixel shader constants as uploaded (eye position, stipple)
};
constexpr u32 kProbeMaxDraws = 4096;
constexpr u32 kProbeStride = 16;  // colour (up to 8 bytes) at 0, depth (4 bytes) at 8
struct ProbeFrame {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u32 slot = ~0u;
  u64 frame = ~0ull;
  std::vector<ProbeEntry> entries;
};
ProbeFrame g_probe;

float HalfToFloat(u16 h) {
  const u32 sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
  float v;
  if (exp == 0)
    v = std::ldexp(static_cast<float>(mant), -24);
  else if (exp == 31)
    v = mant ? NAN : INFINITY;
  else
    v = std::ldexp(static_cast<float>(mant | 0x400), static_cast<i32>(exp) - 25);
  return sign ? -v : v;
}

void LogProbe(VideoState &s) {
  const auto *data = g_probe.buffer ? static_cast<const u8 *>(g_probe.buffer->map()) : nullptr;
  if (!data)
    return;
  EOT_INFO("[probe] frame {}: {} frame-sized draws", g_probe.frame, g_probe.entries.size());
  float last_depth = -1.0f;
  for (size_t i = 0; i < g_probe.entries.size(); ++i) {
    const ProbeEntry &e = g_probe.entries[i];
    const u8 *p = data + i * kProbeStride;
    std::string colour = "-";
    if (e.colour) {
      if (e.colourFormat == plume::RenderFormat::R16G16B16A16_FLOAT) {
        u16 h[4];
        std::memcpy(h, p, 8);
        colour = std::format("({:.3f},{:.3f},{:.3f},{:.3f})", HalfToFloat(h[0]), HalfToFloat(h[1]),
                             HalfToFloat(h[2]), HalfToFloat(h[3]));
      } else {
        colour = std::format("({},{},{},{}) fmt {}", p[0], p[1], p[2], p[3], static_cast<u32>(e.colourFormat));
      }
    }
    float depth = -1.0f;
    if (e.depth)
      std::memcpy(&depth, p + 8, 4);
    EOT_INFO("[probe] #{} at ({},{}) vs {:016x} ps {:016x} prim {} n {} dc {:#x} colour {} depth {:.7g}{} "
             "c19=({:.4g},{:.4g},{:.4g}) c29=({:.4g},{:.4g},{:.4g},{:.4g})",
             i, e.x, e.y, e.vs, e.ps, e.prim, e.count, e.depthControl, colour, depth,
             e.depth && depth != last_depth ? " <- depth changed" : "", e.c19[0], e.c19[1], e.c19[2], e.c29[0],
             e.c29[1], e.c29[2], e.c29[3]);
    if (e.depth)
      last_depth = depth;
  }
  g_probe.buffer->unmap();
  (void)s;
}

u32 Swap(u32 v, u32 endian) {
  switch (endian) {
  case 1: // 8-in-16
    return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
  case 2: // 8-in-32
    return __builtin_bswap32(v);
  case 3: // 16-in-32
    return (v << 16) | (v >> 16);
  default:
    return v;
  }
}

}

bool IsHeroTextureFetch(const u32 fetch[6]) {
  const u32 base = fetch[1] & 0x1FFFF000u;
  return REXCVAR_GET(fable2_hero_texture_writeback) &&
         static_cast<xe::TextureFormat>(fetch[1] & 0x3F) == xe::TextureFormat::k_8_8_8_8 &&
         base >= kHeroTextureBase - 0x1000 && base <= kHeroTextureBase;
}

// Readback buffers of finished writebacks, reused (their frame's fence has
// passed). Created and retired per resolve, they kept the retired list
// non-empty: every 8 presents FlushRetiredLocked waited for the whole GPU
// queue (30-45 ms stalls every few seconds walking after the prologue).
std::vector<std::unique_ptr<plume::RenderBuffer>> g_free_buffers;
std::vector<u64> g_free_sizes;
constexpr size_t kMaxFreeBuffers = 16;

std::unique_ptr<plume::RenderBuffer> TakeReadbackBuffer(VideoState &s, u64 bytes) {
  for (size_t i = 0; i < g_free_buffers.size(); ++i) {
    if (g_free_sizes[i] == bytes) {
      auto b = std::move(g_free_buffers[i]);
      g_free_buffers.erase(g_free_buffers.begin() + static_cast<std::ptrdiff_t>(i));
      g_free_sizes.erase(g_free_sizes.begin() + static_cast<std::ptrdiff_t>(i));
      return b;
    }
  }
  return CreateHostBuffer(s.device.get(), plume::RenderBufferDesc::ReadbackBuffer(bytes), "resolve-writeback");
}

void ReturnReadbackBuffer(VideoState &s, std::unique_ptr<plume::RenderBuffer> b, u64 bytes) {
  if (g_free_buffers.size() >= kMaxFreeBuffers) {
    ParkBuffer(s, std::move(b));
    return;
  }
  g_free_buffers.push_back(std::move(b));
  g_free_sizes.push_back(bytes);
}

bool QueueResolveWritebackLocked(VideoState &s, GuestTexture &d) {
  if (!REXCVAR_GET(fable2_resolve_writeback) || !s.command_list)
    return false;
  const bool rgba8 = IsHeroTextureFetch(d.fetch) && d.width * d.height <= kMaxHeroTexels;
  if (!rgba8 && (d.format != xe::TextureFormat::k_32_FLOAT || d.width * d.height > kMaxGuestTexels ||
                 !d.baseAddress))
    return false;
  HostTexture &h = d.host;
  const plume::RenderFormat host_format =
      rgba8 ? plume::RenderFormat::R8G8B8A8_UNORM : plume::RenderFormat::R32_FLOAT;
  if (!h.valid() || h.format != host_format || h.sampleCount > 1 || h.width < d.width ||
      h.height < d.height)
    return false;
  const u32 pitch = ((d.fetch[0] >> 22) & 0x1FF) * 32;
  if (!pitch)
    return false;
  const u32 row = (h.width * 4 + 255) & ~255u;
  auto buffer = TakeReadbackBuffer(s, u64(row) * h.height);
  if (!buffer)
    return false;

  TransitionLocked(s, h, plume::RenderTextureLayout::COPY_SOURCE);
  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = row / 4;
  region.bufferImageHeight = h.height;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {h.width, h.height, 1};
  vkCmdCopyImageToBuffer(static_cast<plume::VulkanCommandList *>(s.command_list)->vk,
                         static_cast<plume::VulkanTexture *>(h.texture.get())->vk,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         static_cast<plume::VulkanBuffer *>(buffer.get())->vk, 1, &region);
  s.bound_framebuffer = nullptr;

  Pending p;
  p.buffer = std::move(buffer);
  p.slot = s.recording_slot();
  p.hostWidth = h.width;
  p.hostHeight = h.height;
  p.rowBytes = row;
  p.guestWidth = d.width;
  p.guestHeight = d.height;
  p.pitch = pitch;
  p.baseAddress = d.baseAddress;
  p.endian = (d.fetch[1] >> 6) & 3;
  p.tiled = d.tiled;
  p.rgba8 = rgba8;
  // An ARGB destination: the host copy keeps the render target's RGBA order
  // (sampling swaps the fetch swizzle back), while the console's resolve
  // stored red and blue exchanged, which is what the CPU reads (the dog's
  // coat compresses brown, not blue).
  p.swapRB = rgba8 && d.storeSwapRB;
  p.va = d.va;
  static u32 logged = 0;
  if (logged < 2 && (logged == 0 || rgba8)) {
    logged += rgba8 ? 2 : 1;
    EOT_INFO("[writeback] resolve {:#x}: {}x{} {} (host {}x{}, pitch {}, {} endian {}) "
             "written back to guest memory {:#x}",
             d.va, d.width, d.height, rgba8 ? "k_8_8_8_8" : "k_32_FLOAT", h.width, h.height, pitch,
             d.tiled ? "tiled" : "linear", p.endian, d.baseAddress);
  }
  g_pending.push_back(std::move(p));
  return rgba8;
}

void CompleteWritebacksNowLocked(VideoState &s) {
  if (!s.command_list_open)
    return;
  const u32 cur = s.frame.load(std::memory_order_relaxed);
  const u64 t0 = PerfNow();
  SubmitOpenListLocked(s);
  const u64 t1 = PerfNow();
  s.queue->waitForCommandFence(s.fences[cur].get());
  const u64 t2 = PerfNow();
  NoteQueueDrainedLocked(s);
  const size_t pending = g_pending.size();
  WritebacksSlotRecycledLocked(s, cur);
  const u64 t3 = PerfNow();
  static u64 last_frame = ~0ull;
  static u32 in_frame = 0;
  in_frame = last_frame == s.guest_frames ? in_frame + 1 : 1;
  last_frame = s.guest_frames;
  const f64 ms = PerfMsPerTick();
  EOT_DEBUG("[writeback] sync #{} in frame {}: submit {:.2f} gpu wait {:.2f} copy-out {:.2f} ms ({} pending)",
            in_frame, s.guest_frames, static_cast<f64>(t1 - t0) * ms, static_cast<f64>(t2 - t1) * ms,
            static_cast<f64>(t3 - t2) * ms, pending);
}

bool ProbeArmed() { return !REXCVAR_GET(fable2_debug_probe).empty(); }

void ProbeAfterDrawLocked(VideoState &s, HostTexture *colour, HostTexture *depth, u32 width, u32 height,
                          float scale, u64 vs, u64 ps, u32 prim, u32 count, u32 depth_control,
                          const float *ps_constants, u64 ps_constant_bytes) {
  if (width < 1024 || !s.command_list)
    return;
  float fx = 0.0f, fy = 0.0f;
  if (std::sscanf(REXCVAR_GET(fable2_debug_probe).c_str(), "%f,%f", &fx, &fy) != 2)
    return;
  if (!g_probe.buffer || g_probe.frame != s.guest_frames) {
    if (g_probe.buffer && g_probe.slot != ~0u)
      return;  // the previous probe frame is not logged yet
    g_probe.buffer = CreateHostBuffer(s.device.get(),
                                      plume::RenderBufferDesc::ReadbackBuffer(u64(kProbeMaxDraws) * kProbeStride),
                                      "probe");
    if (!g_probe.buffer)
      return;
    g_probe.entries.clear();
    g_probe.frame = s.guest_frames;
    g_probe.slot = s.recording_slot();
  }
  if (g_probe.entries.size() >= kProbeMaxDraws)
    return;
  ProbeEntry e;
  e.vs = vs;
  e.ps = ps;
  e.prim = prim;
  e.count = count;
  e.depthControl = depth_control;
  if (ps_constants && ps_constant_bytes >= 30 * 16) {
    std::memcpy(e.c19, ps_constants + 19 * 4, 16);
    std::memcpy(e.c29, ps_constants + 29 * 4, 16);
  }
  e.x = std::min(static_cast<u32>(fx * static_cast<float>(width) * scale), 1u << 14);
  e.y = std::min(static_cast<u32>(fy * static_cast<float>(height) * scale), 1u << 14);
  const u64 offset = u64(g_probe.entries.size()) * kProbeStride;
  auto *cmd = static_cast<plume::VulkanCommandList *>(s.command_list)->vk;
  auto *buffer = static_cast<plume::VulkanBuffer *>(g_probe.buffer.get())->vk;
  auto copy = [&](HostTexture &h, VkImageAspectFlags aspect, u64 at) {
    TransitionLocked(s, h, plume::RenderTextureLayout::COPY_SOURCE);
    VkBufferImageCopy region{};
    region.bufferOffset = at;
    region.imageSubresource = {aspect, 0, 0, 1};
    region.imageOffset = {static_cast<i32>(e.x), static_cast<i32>(e.y), 0};
    region.imageExtent = {1, 1, 1};
    vkCmdCopyImageToBuffer(cmd, static_cast<plume::VulkanTexture *>(h.texture.get())->vk,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
  };
  if (colour && colour->texture && colour->sampleCount <= 1 && e.x < colour->width && e.y < colour->height &&
      (colour->format == plume::RenderFormat::R16G16B16A16_FLOAT ||
       colour->format == plume::RenderFormat::R8G8B8A8_UNORM || colour->format == plume::RenderFormat::B8G8R8A8_UNORM)) {
    copy(*colour, VK_IMAGE_ASPECT_COLOR_BIT, offset);
    e.colour = true;
    e.colourFormat = colour->format;
  }
  if (depth && depth->texture && depth->sampleCount <= 1 && e.x < depth->width && e.y < depth->height) {
    copy(*depth, VK_IMAGE_ASPECT_DEPTH_BIT, offset + 8);
    e.depth = true;
  }
  s.bound_framebuffer = nullptr;
  s.bound_draw_targets_valid = false;
  g_probe.entries.push_back(e);
}

void WritebacksSlotRecycledLocked(VideoState &s, u32 slot) {
  if (g_probe.buffer && g_probe.slot == slot && g_probe.frame != s.guest_frames) {
    LogProbe(s);
    g_probe.slot = ~0u;
    g_probe.buffer.reset();
    g_probe.entries.clear();
  }
  for (auto it = g_pending.begin(); it != g_pending.end();) {
    if (it->slot != slot) {
      ++it;
      continue;
    }
    Pending &p = *it;
    const auto *src = static_cast<const u8 *>(p.buffer->map());
    // The guest address as the texture upload reads it: virtual first, the
    // physical view only when unmapped. 0xE0000000-range memory sits 4 KB
    // above its number in the physical view, so storing through it put the
    // hero/dog coat one 32x32 tile early; the game's CPU compressed the coat
    // shifted a tile left (the dog wore the neighbouring pieces of its coat).
    const u32 extent = p.pitch * ((p.guestHeight + 31) & ~31u) * 4;
    u8 *dst = mem::readable(p.baseAddress, extent) ? mem::at<u8>(p.baseAddress) : mem::phys<u8>(p.baseAddress);
    if (src && dst && p.rgba8) {
      const u32 kx = std::max(1u, p.hostWidth / p.guestWidth);
      const u32 ky = std::max(1u, p.hostHeight / p.guestHeight);
      const u32 n = kx * ky;
      u64 total[4] = {};  // the average says whether the game composited a real coat
      for (u32 gy = 0; gy < p.guestHeight; ++gy) {
        for (u32 gx = 0; gx < p.guestWidth; ++gx) {
          u32 sum[4] = {};
          for (u32 y = 0; y < ky; ++y) {
            const u8 *row = src + u64(gy * ky + y) * p.rowBytes + u64(gx * kx) * 4;
            for (u32 x = 0; x < kx; ++x)
              for (u32 c = 0; c < 4; ++c)
                sum[c] += row[x * 4 + c];
          }
          u32 bits = 0;
          for (u32 c = 0; c < 4; ++c) {
            const u32 v = (sum[c] + n / 2) / n;
            bits |= v << ((p.swapRB && c != 1 && c != 3 ? 2 - c : c) * 8);
            total[c] += v;
          }
          bits = Swap(bits, p.endian);
          const u32 offset = p.tiled
                                 ? static_cast<u32>(tu::GetTiledOffset2D(static_cast<i32>(gx),
                                                                         static_cast<i32>(gy),
                                                                         p.pitch, 2))
                                 : (gy * p.pitch + gx) * 4;
          std::memcpy(dst + offset, &bits, 4);
        }
      }
      const double texels = double(p.guestWidth) * p.guestHeight;
      EOT_INFO("[writeback] hero {:#x} {}x{} -> {:#x}: average rgba ({:.1f}, {:.1f}, {:.1f}, {:.1f}){}", p.va,
               p.guestWidth, p.guestHeight, p.baseAddress, total[0] / texels, total[1] / texels,
               total[2] / texels, total[3] / texels, p.swapRB ? ", stored as argb" : "");
    } else if (src && dst) {
      const u32 kx = std::max(1u, p.hostWidth / p.guestWidth);
      const u32 ky = std::max(1u, p.hostHeight / p.guestHeight);
      const float inv = 1.0f / static_cast<float>(kx * ky);
      float vmin = 1e30f, vmax = -1e30f;
      double vsum = 0.0;
      for (u32 gy = 0; gy < p.guestHeight; ++gy) {
        for (u32 gx = 0; gx < p.guestWidth; ++gx) {
          float sum = 0.0f;
          for (u32 y = 0; y < ky; ++y) {
            const float *row =
                reinterpret_cast<const float *>(src + u64(gy * ky + y) * p.rowBytes) + gx * kx;
            for (u32 x = 0; x < kx; ++x)
              sum += row[x];
          }
          const float v = sum * inv;
          vmin = std::min(vmin, v);
          vmax = std::max(vmax, v);
          vsum += v;
          u32 bits;
          std::memcpy(&bits, &v, 4);
          bits = Swap(bits, p.endian);
          const u32 offset = p.tiled
                                 ? static_cast<u32>(tu::GetTiledOffset2D(static_cast<i32>(gx),
                                                                         static_cast<i32>(gy),
                                                                         p.pitch, 2))
                                 : (gy * p.pitch + gx) * 4;
          std::memcpy(dst + offset, &bits, 4);
        }
      }
      // Per target: the game alternates two exposure targets every frame, and
      // one shared counter always landed on the same one.
      static std::unordered_map<u32, u32> logged;
      if (logged[p.va]++ % 300 == 0)
        EOT_INFO("[writeback] {:#x}: min {:.4g} max {:.4g} avg {:.4g}", p.va, vmin, vmax,
                 vsum / (double(p.guestWidth) * p.guestHeight));
    }
    if (src)
      p.buffer->unmap();
    ReturnReadbackBuffer(s, std::move(p.buffer), u64(p.rowBytes) * p.hostHeight);
    it = g_pending.erase(it);
  }
}

}
