// Fable2 - render mode table patch
//
// Fable 2 picks its frame setup from a table of render modes built by
// sub_83293788 at 0x834BE010 (10 entries of 108 bytes, see MACOS_NOTES.md):
//   +0   name (string object)
//   +4   width, +8 height
//   +12  EDRAM color format (16 = 2_10_10_10_FLOAT, 2 = 8_8_8_8, 58 = 16_16_16_16_FLOAT)
//   +16  MSAA (0 = 1x, 1 = 2x, 2 = 4x)
//   +20  tile count (predicated tiling passes)
//   +24  tile rectangles, 16 bytes each: left, top, right, bottom
// Entry 0 is "1280x720 No MSAA" with a single tile. The other entries use 2x
// or 4x MSAA split into 2-5 tiles, which the emulated GPU replays per tile and
// converts between sample counts on every pass. With fable2_render_mode=nomsaa
// every entry becomes single-sample and is tiled only if it doesn't fit in
// EDRAM, so the game skips predicated tiling (sub_822C40C8 resolves directly
// when the tile count is 1).

#include "generated/Fable2_init.h"

#include <rex/cvar.h>
#include <rex/exception_handler.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string>

REXCVAR_DEFINE_STRING(fable2_render_mode, "default", "Fable2",
                      "Render mode table override: default (the game's MSAA and tiling) or "
                      "nomsaa (no MSAA, no predicated tiling where the frame fits in EDRAM)");

REXCVAR_DEFINE_BOOL(fable2_debug_find_mode_writer, false, "Fable2",
                    "Debug: log the functions that write the dynamic render mode entry");

namespace {

constexpr u32 kModeTable = 0x834BE010;
constexpr u32 kModeSize = 108;
constexpr u32 kModeCount = 10;
constexpr u32 kMaxTiles = 5;
constexpr u32 kEdramBytes = 10 * 1024 * 1024;

// Returns how many entries needed a change.
u32 PatchModesNoMsaa(uint8_t* base, bool log) {
  u32 patched = 0;
  for (u32 i = 0; i < kModeCount; ++i) {
    u32 e = kModeTable + i * kModeSize;
    u32 width = REX_LOAD_U32(e + 4);
    u32 height = REX_LOAD_U32(e + 8);
    u32 format = REX_LOAD_U32(e + 12);
    u32 msaa = REX_LOAD_U32(e + 16);
    u32 tiles = REX_LOAD_U32(e + 20);
    if (!width || !height || (msaa == 0 && tiles <= 1)) {
      continue;
    }
    // Color plus 32-bit depth, one sample.
    u32 bytes_per_pixel = (format == 58 ? 8 : 4) + 4;
    u64 frame_bytes = u64(width) * height * bytes_per_pixel;
    u32 new_tiles = u32((frame_bytes + kEdramBytes - 1) / kEdramBytes);
    if (new_tiles > kMaxTiles) {
      continue;
    }
    // Horizontal bands, heights aligned to the 16-pixel EDRAM tile rows.
    u32 band = ((height + new_tiles - 1) / new_tiles + 15) & ~15u;
    for (u32 t = 0; t < kMaxTiles; ++t) {
      u32 r = e + 24 + t * 16;
      bool used = t < new_tiles;
      u32 top = used ? std::min(t * band, height) : 0;
      u32 bottom = used ? std::min((t + 1) * band, height) : 0;
      REX_STORE_U32(r + 0, 0);
      REX_STORE_U32(r + 4, top);
      REX_STORE_U32(r + 8, used ? width : 0);
      REX_STORE_U32(r + 12, bottom);
    }
    REX_STORE_U32(e + 16, 0);
    REX_STORE_U32(e + 20, new_tiles);
    ++patched;
    if (!log) {
      continue;
    }
    REXLOG_INFO("Render mode {}: {}x{} format {} MSAA {} with {} tiles -> no MSAA, {} tile(s)", i,
                width, height, format, msaa, tiles, new_tiles);
  }
  return patched;
}

void DumpModes(uint8_t* base, const char* when) {
  for (u32 i = 0; i < kModeCount; ++i) {
    u32 e = kModeTable + i * kModeSize;
    std::string words;
    for (u32 o = 4; o < kModeSize; o += 4) {
      words += fmt::format(" {}", REX_LOAD_U32(e + o));
    }
    REXLOG_INFO("Render mode {} at {}:{}", i, when, words);
  }
}

// Write watch on the dynamic mode entry: its host page is made read-only
// once per frame, the first write to it faults, the handler records the
// host pc, unprotects the page and lets the store retry.
constexpr u32 kDynamicMode = kModeTable + 2 * kModeSize;
uint8_t* g_watch_page = nullptr;
size_t g_watch_page_size = 0;
uint8_t* g_watch_base = nullptr;
std::atomic<bool> g_watch_armed{false};
std::atomic<uint64_t> g_watch_pcs[16];
std::atomic<u32> g_watch_offsets[16];
std::atomic<u32> g_watch_count{0};

bool OnWatchFault(rex::arch::Exception* ex, void*) {
  if (ex->code() != rex::arch::Exception::Code::kAccessViolation || !g_watch_page) {
    return false;
  }
  auto* addr = reinterpret_cast<uint8_t*>(ex->fault_address());
  if (addr < g_watch_page || addr >= g_watch_page + g_watch_page_size) {
    return false;
  }
  mprotect(g_watch_page, g_watch_page_size, PROT_READ | PROT_WRITE);
  g_watch_armed = false;
  // Record each distinct writer once, with the guest address it wrote.
  uint64_t pc = ex->pc();
  u32 n = g_watch_count.load();
  bool seen = false;
  for (u32 i = 0; i < n; ++i) {
    seen |= g_watch_pcs[i] == pc;
  }
  if (!seen && n < 16) {
    g_watch_pcs[n] = pc;
    g_watch_offsets[n] = u32(addr - g_watch_base);
    g_watch_count = n + 1;
  }
  return true;
}

void ArmModeWatch(uint8_t* base) {
  static bool installed = false;
  static u32 logged = 0;
  if (!installed) {
    installed = true;
    g_watch_base = base;
    g_watch_page_size = size_t(getpagesize());
    g_watch_page = reinterpret_cast<uint8_t*>(uintptr_t(base + kDynamicMode) &
                                              ~uintptr_t(g_watch_page_size - 1));
    rex::arch::ExceptionHandler::Install(OnWatchFault, nullptr);
  }
  for (u32 n = g_watch_count; logged < n; ++logged) {
    Dl_info info{};
    uint64_t pc = g_watch_pcs[logged];
    const char* name = dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_sname
                           ? info.dli_sname
                           : "?";
    REXLOG_INFO("Watched page write to {:08X} by {} (+0x{:X})", u32(g_watch_offsets[logged]),
                name, info.dli_saddr ? pc - uint64_t(info.dli_saddr) : 0);
  }
  if (g_watch_count < 16 && !g_watch_armed) {
    g_watch_armed = true;
    mprotect(g_watch_page, g_watch_page_size, PROT_READ);
  }
}

}  // namespace

// Builds the render mode table (static initializer).
REX_HOOK_RAW(sub_83293788) {
  __imp__sub_83293788(ctx, base);
  if (REXCVAR_GET(fable2_render_mode) == "nomsaa") {
    PatchModesNoMsaa(base, true);
  }
  DumpModes(base, "init");
}

// End of the scene: EndTiling when the active mode has several tiles, else
// a plain resolve. r3 + 4 points to the active mode entry. The game rewrites
// entry 2 (its dynamic mode) after startup, so the patch is applied again
// here for the next frame.
REX_HOOK_RAW(sub_822C40C8) {
  static u32 calls = 0;
  static u32 repatches = 0;
  if (calls == 0) {
    DumpModes(base, "first scene");
  }
  if (calls++ % 300 == 0) {
    u32 mode = REX_LOAD_U32(ctx.r3.u32 + 4);
    REXLOG_INFO("Scene render mode pointer {:08X} (entry {}): {}x{} format {} MSAA {} tiles {}",
                mode, mode >= kModeTable ? i64(mode - kModeTable) / i64(kModeSize) : -1,
                REX_LOAD_U32(mode + 4), REX_LOAD_U32(mode + 8), REX_LOAD_U32(mode + 12),
                REX_LOAD_U32(mode + 16), REX_LOAD_U32(mode + 20));
    REXLOG_INFO("Render mode entries patched again so far: {}", repatches);
  }
  __imp__sub_822C40C8(ctx, base);
  if (REXCVAR_GET(fable2_render_mode) == "nomsaa") {
    repatches += PatchModesNoMsaa(base, repatches == 0);
  }
  if (REXCVAR_GET(fable2_debug_find_mode_writer)) {
    ArmModeWatch(base);
  }
}

// Draw entry points (XDK DrawVertices / DrawIndexedVertices): re-arm the
// write watch between draws, since other variables on the same host page are
// written every frame too.
REX_HOOK_RAW(sub_8221C3E8) {
  if (g_watch_page && !g_watch_armed && g_watch_count < 16) {
    g_watch_armed = true;
    mprotect(g_watch_page, g_watch_page_size, PROT_READ);
  }
  __imp__sub_8221C3E8(ctx, base);
}

REX_HOOK_RAW(sub_8221DFC0) {
  if (g_watch_page && !g_watch_armed && g_watch_count < 16) {
    g_watch_armed = true;
    mprotect(g_watch_page, g_watch_page_size, PROT_READ);
  }
  __imp__sub_8221DFC0(ctx, base);
}

// Start of the scene: BeginTiling with the active mode's tiles when it has
// several. The dynamic mode entry is rewritten earlier in each frame, so the
// patch is applied again right before the game reads it.
REX_HOOK_RAW(sub_822877E8) {
  if (REXCVAR_GET(fable2_render_mode) == "nomsaa") {
    static u32 repatches = 0;
    static u32 calls = 0;
    repatches += PatchModesNoMsaa(base, false);
    if (calls++ % 300 == 0) {
      REXLOG_INFO("Scene start {}: entries patched again {}", calls, repatches);
    }
  }
  __imp__sub_822877E8(ctx, base);
}

// Diagnostics: how often the XDK's EndTiling and Resolve run.
REX_HOOK_RAW(sub_8227F3B0) {
  static u32 calls = 0;
  if (calls++ % 300 == 0) {
    REXLOG_INFO("EndTiling call {} flags {:08X}", calls, ctx.r4.u32);
  }
  __imp__sub_8227F3B0(ctx, base);
}
