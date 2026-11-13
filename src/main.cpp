// Fable2 - ReXGlue Recompiled Project
// ReXApp subclass (custom content paths + native heap)

#if defined(FABLE2_NATIVE_RENDERER)
#include "gpu/pipeline/pipeline_cache.h"
#endif
#include "generated/Fable2_init.h"
#include "gpu_capture.h"
#include "heap.h"

#if defined(FABLE2_NATIVE_RENDERER)
#include "gpu/device.h"
#include "gpu/imgui_overlay.h"
#include "gpu/shaders/guest_shaders.h"
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include <fmt/format.h>

REXCVAR_DEFINE_BOOL(fable2_title_fps, true, "Fable2", "Show the frame rate in the window title");
REXCVAR_DEFINE_BOOL(fable2_pso_precache, true, "Fable2",
                    "Native renderer: record the pipelines drawn and build the recorded ones on worker "
                    "threads from boot");

namespace {
// Frames presented by the game, counted in its D3D present function.
std::atomic<uint32_t> g_presented_frames{0};
}  // namespace

#if !defined(FABLE2_NATIVE_RENDERER)
// The game's present (calls VdSwap), once per frame on the 3D Engine thread.
// (The native renderer replaces this function: gpu/hooks/state.cpp.)
REX_HOOK_RAW(sub_82BA34D8) {
  g_presented_frames.fetch_add(1, std::memory_order_relaxed);
  Fable2GpuCaptureOnPresent();
  __imp__sub_82BA34D8(ctx, base);
}
#endif

class Fable2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    auto app = std::unique_ptr<Fable2App>(
        new Fable2App(ctx, "Fable2", PPCImageConfig, "[game_directory]"));
    app->AddPositionalOption("game_directory");
    return app;
  }

 protected:
  // Assets root: arg or default to exe_dir/assets, containing game/ and
  // update/. The --game_data_root / --update_data_root cvars still win.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    std::filesystem::path assets_root;
    if (auto arg = GetArgument("game_directory")) {
      assets_root = *arg;
    } else {
      assets_root = rex::filesystem::GetExecutableFolder() / "assets";
    }
    if (paths.game_data_root.empty()) {
      paths.game_data_root = assets_root / "game";
    }
    if (paths.update_data_root.empty()) {
      paths.update_data_root = assets_root / "update";
    }
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
#if defined(FABLE2_NATIVE_RENDERER)
    // Detached mode: no GPU emulation plugin; the native renderer (src/gpu)
    // hooks the game's D3D calls and draws the frames itself.
    config.gpu_plugin.clear();
    config.graphics = nullptr;
#else
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
#endif
    // Fable 2 presents every second vblank, so a 120 Hz guest video mode
    // lifts its 30 fps cap to 60. Game speed, animation and audio follow
    // real time (checked in play). --video_mode_refresh_rate=60 restores 30.
    for (const auto& flag : rex::cvar::GetRegistry()) {
      if (flag.name == "video_mode_refresh_rate" &&
          flag.source == rex::cvar::Source::kDefault) {
        rex::cvar::SetFlagByName(flag.name, "120");
      }
    }
#if defined(__APPLE__)
    // MoltenVK warns on every pipeline that Metal can't disable primitive
    // restart; logging that tens of thousands of times costs frame time.
    // Errors only, unless the user set their own level.
    setenv("MVK_CONFIG_LOG_LEVEL", "1", 0);
#if !defined(FABLE2_NATIVE_RENDERER)
    // Encode Vulkan commands into Metal on MoltenVK's own thread instead of
    // the GPU emulation thread. (The native renderer sets its own MoltenVK
    // options: platform/moltenvk.cpp.)
    setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "0", 0);
#endif
    for (const auto& flag : rex::cvar::GetRegistry()) {
      if (flag.source != rex::cvar::Source::kDefault) {
        continue;
      }
      // The same warning also reaches the log through the Vulkan debug
      // messenger; keep it off unless a config file or flag turned it on.
      if (flag.name == "vulkan_log_debug_messages") {
        rex::cvar::SetFlagByName(flag.name, "false");
      }
      // Reading memexport results back stalls the GPU every such draw; Fable
      // 2 plays correctly without it.
      if (flag.name == "readback_memexport") {
        rex::cvar::SetFlagByName(flag.name, "false");
      }
    }
#endif
  }

#if defined(FABLE2_NATIVE_RENDERER)
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    return eot::gpu::CreateOverlayDrawer();
  }

  void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override {
    (void)pixel_width;
    (void)pixel_height;
    eot::gpu::Video::RequestResize();
  }
#endif

  // The window and its title exist by now (SetupPresentation runs first).
  void OnPreLaunchModule() override {
#if defined(FABLE2_NATIVE_RENDERER)
    if (!eot::gpu::Video::CreateHostDevice(window())) {
      REXLOG_ERROR("Native renderer: failed to create the host device");
      app_context().QuitFromUIThread();
      return;
    }
    eot::gpu::GuestShadersInit();
    // The runtime's own dialogs (on-screen keyboard, message boxes) are ImGui
    // windows drawn into the presented frame. Without this hook they were never
    // drawn: XamShowKeyboardUI (a villager's "Set Nickname") then waited for a
    // dialog nobody could see while it held all controller input, and the game
    // looked frozen. (As in EdgeOfTimeRecomp's ReeotApp::InstallOverlayHook.)
    eot::gpu::SetOverlayDrawHook([this](plume::RenderCommandList *cmd,
                                        plume::RenderFramebuffer *framebuffer, uint32_t width,
                                        uint32_t height) {
      if (!imgui_drawer() || !imgui_drawer()->HasDialogs() || eot::gpu::Video::IsShuttingDown())
        return;
      app_context().CallInUIThreadSynchronous([&] {
        eot::gpu::OverlayDrawContext ctx(width, height, cmd, framebuffer);
        imgui_drawer()->Draw(ctx);
      });
    });
    // Pipelines drawn are recorded (<cache root>/pso/pso_drawn_*.csv) and the
    // recorded ones are built on worker threads from boot on: without it every
    // new pipeline was built on the render thread in the frame that first drew
    // it (10-30 ms each: the drops while panning, 0.8 s at a level's first frame).
    if (REXCVAR_GET(fable2_pso_precache))
      eot::gpu::PsoCachePrecache();
#endif
    if (!REXCVAR_GET(fable2_title_fps) || !window()) {
      return;
    }
    // Window title: frame rate, and (native renderer) the game's render
    // resolution with the resolution it is drawn at on the host.
    auto presented = []() -> uint64_t {
#if defined(FABLE2_NATIVE_RENDERER)
      uint64_t frames = 0;
      uint32_t w = 0, h = 0;
      float scale = 1.0f;
      eot::gpu::Video::TitleStats(&frames, &w, &h, &scale);
      return frames;
#else
      return g_presented_frames.load(std::memory_order_relaxed);
#endif
    };
    std::string base_title = window()->GetTitle();
    title_thread_ = std::jthread([this, base_title, presented](std::stop_token stop) {
      using clock = std::chrono::steady_clock;
      uint64_t last_frames = presented();
      auto last_time = clock::now();
      while (!stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        uint64_t frames = presented();
        auto now = clock::now();
        double seconds = std::chrono::duration<double>(now - last_time).count();
        double fps = seconds > 0.0 ? static_cast<double>(frames - last_frames) / seconds : 0.0;
        last_frames = frames;
        last_time = now;
        std::string title = fmt::format("{} - {:.0f} fps", base_title, fps);
#if defined(FABLE2_NATIVE_RENDERER)
        {
          uint64_t unused = 0;
          uint32_t w = 0, h = 0;
          float scale = 1.0f;
          eot::gpu::Video::TitleStats(&unused, &w, &h, &scale);
          if (w && h)
            title += fmt::format(" - {}x{} (drawn at {}x{})", w, h,
                                 static_cast<uint32_t>(std::lround(w * scale)),
                                 static_cast<uint32_t>(std::lround(h * scale)));
        }
#endif
        app_context().CallInUIThread([this, title]() {
          if (window()) {
            window()->SetTitle(title);
          }
        });
      }
    });
  }

  void OnShutdown() override {
#if defined(FABLE2_NATIVE_RENDERER)
    eot::gpu::Video::BeginShutdown();
    eot::gpu::Video::Shutdown();
#endif
    title_thread_.request_stop();
    if (title_thread_.joinable()) {
      title_thread_.join();
    }
  }

  // Runs after Runtime::Setup() and before the XEX is loaded, which is where
  // the native heap has to come up (serves the game's Lua allocator,
  // lhHeapRealloc, from dlmalloc).
  void OnLoadXexImage(std::string& xex_image) override {
    if (!lh::InitNativeHeap()) {
      REXLOG_ERROR("Failed to initialize native heap");
      // No failure return from this hook; an unresolvable image path makes
      // ConstructRuntime report the error and abort startup.
      xex_image.clear();
    }
  }

 private:
  // Updates the window title with the frame rate.
  std::jthread title_thread_;
};

REX_DEFINE_APP(Fable2, Fable2App::Create)
