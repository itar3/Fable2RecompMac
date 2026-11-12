// platform/moltenvk.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#if defined(__APPLE__)

#include "platform/moltenvk.h"

#include <dlfcn.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#include "core/logging.h"
#include "platform/process.h"

#include <rex/filesystem.h>

namespace eot::platform {

namespace {

namespace fs = std::filesystem;

void SetenvIfUnset(const char *name, const char *value) {
  if (const char *cur = std::getenv(name); !cur || !cur[0])
    ::setenv(name, value, 1);
}

void *LoadFirst(const fs::path *roots, size_t root_count, const char *leaf, fs::path *loaded_from) {
  std::error_code ec;
  for (size_t i = 0; i < root_count; ++i) {
    const fs::path candidate = roots[i] / leaf;
    if (!fs::exists(candidate, ec))
      continue;
    if (void *handle = ::dlopen(candidate.c_str(), RTLD_NOW | RTLD_GLOBAL)) {
      *loaded_from = candidate;
      return handle;
    }
    EOT_WARN("[mvk] {} did not load: {}", candidate.string(), ::dlerror());
  }
  return nullptr;
}

}

bool PrepareMoltenVK() {
  static bool prepared = false;
  static bool ok = false;
  if (prepared)
    return ok;
  prepared = true;

  // Fable 2: MoltenVK encodes the Metal commands on its own thread (3-4 ms
  // per frame off the render thread in busy Bowerstone).
  SetenvIfUnset("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "0");
  SetenvIfUnset("MVK_CONFIG_SEMAPHORE_SUPPORT_STYLE", "3");
  SetenvIfUnset("MVK_CONFIG_PRESENT_WITH_COMMAND_BUFFER", "1");

  const fs::path staged = DataDir() / "vulkan";
  // Fable 2: the ReXGlue build stages the Vulkan loader and MoltenVK in
  // vulkan/lib next to the executable.
  const fs::path roots[] = {
      rex::filesystem::GetExecutableFolder() / "vulkan" / "lib",
      staged / "lib",
      fs::path("/opt/homebrew/lib"),
      fs::path("/usr/local/lib"),
  };

  std::error_code ec;
  // Fable 2: the build stages the loader, MoltenVK and its ICD file in
  // vulkan/ next to the executable. Without pointing the loader at that ICD it
  // searched the system (/usr/local/share/vulkan/icd.d: the Vulkan SDK's
  // MoltenVK and KosmicKrisp) while the bundled MoltenVK was also loaded
  // directly: two MoltenVK copies in one process (objc warns of "spurious
  // casting failures and mysterious crashes"). FABLE2_SYSTEM_VULKAN=1 keeps the
  // old system search.
  fs::path icd = staged / "share" / "vulkan" / "icd.d" / "MoltenVK_icd.json";
  const char *system_vulkan = std::getenv("FABLE2_SYSTEM_VULKAN");
  const fs::path bundled_icd =
      rex::filesystem::GetExecutableFolder() / "vulkan" / "share" / "vulkan" / "icd.d" / "MoltenVK_icd.json";
  if (!(system_vulkan && *system_vulkan == '1') && fs::exists(bundled_icd, ec))
    icd = bundled_icd;
  if (fs::exists(icd, ec)) {
    SetenvIfUnset("VK_DRIVER_FILES", icd.c_str());
    SetenvIfUnset("VK_ICD_FILENAMES", icd.c_str());
  }

  fs::path from;
  void *handle = nullptr;
  // Without a MoltenVK ICD file the loader finds no driver; MoltenVK exports
  // the Vulkan entry points itself, so load it directly then.
  if (!fs::exists(icd, ec))
    handle = LoadFirst(roots, std::size(roots), "libMoltenVK.dylib", &from);
  if (!handle)
    handle = LoadFirst(roots, std::size(roots), "libvulkan.1.dylib", &from);
  if (!handle)
    handle = LoadFirst(roots, std::size(roots), "libvulkan.dylib", &from);
  if (!handle)
    handle = LoadFirst(roots, std::size(roots), "libMoltenVK.dylib", &from);
  if (!handle) {
    EOT_ERROR("[mvk] no Vulkan runtime: neither {}/lib nor /opt/homebrew/lib nor /usr/local/lib holds "
              "libvulkan.1.dylib or libMoltenVK.dylib (brew install molten-vk, or build with the SDK's "
              "staging)",
              staged.string());
    return false;
  }
  EOT_INFO("[mvk] Vulkan runtime {} ({})", from.string(),
           fs::exists(icd, ec) ? "MoltenVK ICD " + icd.string() : std::string("system ICD search"));
  ok = true;
  return true;
}

bool GetMetalRenderWindow(plume::RenderWindow &out) {
  int count = 0;
  SDL_Window **windows = SDL_GetWindows(&count);
  SDL_Window *sdl_window = (windows && count > 0) ? windows[0] : nullptr;
  SDL_free(windows);
  if (!sdl_window) {
    EOT_ERROR("No SDL window exists yet");
    return false;
  }

  out.window = SDL_GetPointerProperty(SDL_GetWindowProperties(sdl_window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
                                      nullptr);

  static SDL_Window *view_owner = nullptr;
  static SDL_MetalView metal_view = nullptr;
  if (!metal_view || view_owner != sdl_window) {
    metal_view = SDL_Metal_CreateView(sdl_window);
    view_owner = sdl_window;
  }
  if (!metal_view) {
    EOT_ERROR("SDL_Metal_CreateView failed: {}", SDL_GetError());
    return false;
  }
  out.view = SDL_Metal_GetLayer(metal_view);
  if (!out.window || !out.view) {
    EOT_ERROR("The SDL window exposed no NSWindow/CAMetalLayer for the Metal surface");
    return false;
  }
  return true;
}

}

#endif
