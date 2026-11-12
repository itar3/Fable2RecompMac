// Fable2 - capture of the game's compiled shaders (for XenosRecomp)
//
// Fable 2's Lionhead shader bank (data/Shaders/*.sbk) is packed, and its loader
// builds the XDK shader objects itself; the XDK's shader creation functions are
// never called. An XDK shader object still embeds the virtual part of the
// standard shader container (header, constant table, definitions) and points
// to the microcode (the physical part). With fable2_shader_dump_dir set, every
// shader object passed to SetVertexShader / SetPixelShader is written once to
// that folder as a standard container file (virtual part followed by the
// physical part), named by a hash of its bytes.
//
// The container offset is found at run time by its magic (logged once per
// shader type); the microcode pointer offsets are Edge of Time's XDK layout.

#include "generated/Fable2_init.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

REXCVAR_DEFINE_STRING(fable2_shader_dump_dir, "", "Fable2",
                      "Write every shader the game uses to this folder as a standard XDK "
                      "shader container (for XenosRecomp)");

namespace {

constexpr uint32_t kContainerScanBytes = 0x800;
constexpr uint32_t kMaxPartBytes = 256 * 1024;

std::mutex g_mutex;
std::unordered_set<uint32_t> g_seen_objects;
std::unordered_set<uint64_t> g_written;

uint64_t Fnv1a(const uint8_t* data, size_t size, uint64_t hash = 0xCBF29CE484222325ull) {
  for (size_t i = 0; i < size; ++i) {
    hash = (hash ^ data[i]) * 0x100000001B3ull;
  }
  return hash;
}

// Microcode pointer offsets in the XDK shader objects (same as Edge of Time's
// XDK). The pointer can be physical or a virtual address in the executable's
// data.
constexpr uint32_t kVertexMicrocodePointer = 0x20;
constexpr uint32_t kPixelMicrocodePointer = 0x18;

struct Layout {
  bool known = false;
  uint32_t container = 0;
};
Layout g_layout[2];  // [0] vertex, [1] pixel

bool FindLayout(uint8_t* base, uint32_t object, bool is_pixel, Layout& layout) {
  for (uint32_t off = 0; off < kContainerScanBytes; off += 4) {
    uint32_t flags = REX_LOAD_U32(object + off);
    if ((flags & 0xFFFFFF00u) != 0x102A1100u) {
      continue;
    }
    uint32_t virtual_size = REX_LOAD_U32(object + off + 4);
    uint32_t physical_size = REX_LOAD_U32(object + off + 8);
    if (virtual_size < 0x24 || virtual_size > kMaxPartBytes || physical_size > kMaxPartBytes) {
      continue;
    }
    layout.known = true;
    layout.container = off;
    REXLOG_INFO("Shader capture: {} shader object layout: container at +{:#x}",
                is_pixel ? "pixel" : "vertex", off);
    return true;
  }
  return false;
}

void Capture(uint8_t* base, uint32_t object, bool is_pixel) {
  const std::string& dir = REXCVAR_GET(fable2_shader_dump_dir);
  if (dir.empty() || !object) {
    return;
  }
  std::lock_guard lock(g_mutex);
  if (!g_seen_objects.insert(object).second) {
    return;
  }
  Layout& layout = g_layout[is_pixel ? 1 : 0];
  if (!layout.known && !FindLayout(base, object, is_pixel, layout)) {
    REXLOG_WARN("Shader capture: no container found in {} shader object {:08X}",
                is_pixel ? "pixel" : "vertex", object);
    return;
  }
  uint32_t container = object + layout.container;
  uint32_t flags = REX_LOAD_U32(container);
  uint32_t virtual_size = REX_LOAD_U32(container + 4);
  uint32_t physical_size = REX_LOAD_U32(container + 8);
  uint32_t microcode =
      REX_LOAD_U32(object + (is_pixel ? kPixelMicrocodePointer : kVertexMicrocodePointer));
  if ((flags & 0xFFFFFF00u) != 0x102A1100u || virtual_size > kMaxPartBytes ||
      physical_size > kMaxPartBytes || (physical_size && !microcode)) {
    REXLOG_WARN("Shader capture: {} shader object {:08X} doesn't match the layout",
                is_pixel ? "pixel" : "vertex", object);
    return;
  }
  const uint8_t* virtual_bytes = REX_RAW_ADDR(container);
  const uint8_t* physical_bytes = REX_RAW_ADDR(microcode);
  bool any_nonzero = false;
  for (uint32_t i = 0; i < physical_size && !any_nonzero; ++i) {
    any_nonzero = physical_bytes[i] != 0;
  }
  if (physical_size && !any_nonzero) {
    REXLOG_WARN("Shader capture: {} shader object {:08X}: microcode at {:08X} is all zeros",
                is_pixel ? "pixel" : "vertex", object, microcode);
    return;
  }
  uint64_t hash = Fnv1a(physical_bytes, physical_size, Fnv1a(virtual_bytes, virtual_size));
  if (!g_written.insert(hash).second) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::filesystem::path path = std::filesystem::path(dir) /
                               fmt::format("{}_{:016x}.bin", (flags & 1) ? "vs" : "ps", hash);
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(virtual_bytes), std::streamsize(virtual_size));
  out.write(reinterpret_cast<const char*>(physical_bytes), std::streamsize(physical_size));
  if (g_written.size() % 50 == 1) {
    REXLOG_INFO("Shader capture: {} shaders written to {}", g_written.size(), dir);
  }
}

}  // namespace

// Called by the native renderer when a shader is not in its cache; the hash is
// the renderer's (XXH3 of the container, as XenosRecomp keys its output).
void Fable2WriteShaderContainer(const unsigned char* virtual_bytes, unsigned virtual_size,
                                const unsigned char* physical_bytes, unsigned physical_size,
                                unsigned long long hash) {
  const std::string& dir = REXCVAR_GET(fable2_shader_dump_dir);
  if (dir.empty() || !virtual_bytes || virtual_size < 4) {
    return;
  }
  std::lock_guard lock(g_mutex);
  if (!g_written.insert(hash).second) {
    return;
  }
  const bool vertex = (virtual_bytes[3] & 1) != 0;  // big-endian flags, bit 0
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::filesystem::path path = std::filesystem::path(dir) /
                               fmt::format("{}_x{:016x}.bin", vertex ? "vs" : "ps", hash);
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(virtual_bytes), std::streamsize(virtual_size));
  if (physical_bytes && physical_size) {
    out.write(reinterpret_cast<const char*>(physical_bytes), std::streamsize(physical_size));
  }
  REXLOG_INFO("Shader capture: {} shader {:016x} missing from the cache, written to {}",
              vertex ? "vertex" : "pixel", hash, dir);
}

// Called by the native renderer's SetVertexShader / SetPixelShader hooks.
void Fable2CaptureShader(uint8_t* base, uint32_t object, bool is_pixel) {
  Capture(base, object, is_pixel);
}

#if !defined(FABLE2_NATIVE_RENDERER)
// D3DDevice_SetPixelShader(device, shader) / SetVertexShader(device, shader).
REX_HOOK_RAW(sub_82208BB0) {
  Capture(base, ctx.r4.u32, true);
  __imp__sub_82208BB0(ctx, base);
}

REX_HOOK_RAW(sub_82232510) {
  Capture(base, ctx.r4.u32, false);
  __imp__sub_82232510(ctx, base);
}
#endif
