// Fable2 - Metal GPU frame capture on demand (macOS, debugging only)
//
// With fable2_gpu_capture_trigger set to a file path, the game checks for that
// file once per presented frame. When it appears, the next frame is captured
// with MTLCaptureManager into fable2_gpu_capture_output (a .gputrace document
// for Xcode) and the trigger file is deleted. Requires the environment
// variable METAL_CAPTURE_ENABLED=1 at launch.

#include "gpu_capture.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <filesystem>
#include <system_error>

REXCVAR_DEFINE_STRING(fable2_gpu_capture_trigger, "", "Fable2",
                      "Debug (macOS): capture one frame with Metal when this file appears");
REXCVAR_DEFINE_STRING(fable2_gpu_capture_output, "", "Fable2",
                      "Debug (macOS): .gputrace path for fable2_gpu_capture_trigger");

#if defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>

extern "C" void* MTLCreateSystemDefaultDevice(void);

namespace {

using id = void*;

template <typename R, typename... Args>
R Send(id object, const char* selector, Args... args) {
  return reinterpret_cast<R (*)(id, SEL, Args...)>(objc_msgSend)(object, sel_registerName(selector),
                                                                 args...);
}

id Class(const char* name) { return reinterpret_cast<id>(objc_getClass(name)); }

id NSString(const char* text) {
  return Send<id>(Class("NSString"), "stringWithUTF8String:", text);
}

enum class State { kIdle, kCapturing };
State g_state = State::kIdle;
id g_manager = nullptr;

bool StartCapture(const std::string& output) {
  id device = MTLCreateSystemDefaultDevice();
  g_manager = Send<id>(Class("MTLCaptureManager"), "sharedCaptureManager");
  if (!device || !g_manager) {
    REXLOG_ERROR("GPU capture: no Metal device or capture manager");
    return false;
  }
  // MTLCaptureDestinationGPUTraceDocument = 2.
  if (!Send<bool>(g_manager, "supportsDestination:", long(2))) {
    REXLOG_ERROR("GPU capture: .gputrace output not supported (set METAL_CAPTURE_ENABLED=1)");
    return false;
  }
  std::error_code ec;
  std::filesystem::remove_all(output, ec);
  id descriptor = Send<id>(Send<id>(Class("MTLCaptureDescriptor"), "alloc"), "init");
  Send<void>(descriptor, "setCaptureObject:", device);
  Send<void>(descriptor, "setDestination:", long(2));
  id url = Send<id>(Class("NSURL"), "fileURLWithPath:", NSString(output.c_str()));
  Send<void>(descriptor, "setOutputURL:", url);
  id error = nullptr;
  bool started = Send<bool>(g_manager, "startCaptureWithDescriptor:error:", descriptor, &error);
  if (!started) {
    id description = error ? Send<id>(error, "localizedDescription") : nullptr;
    REXLOG_ERROR("GPU capture: failed to start: {}",
                 description ? Send<const char*>(description, "UTF8String") : "unknown error");
    return false;
  }
  REXLOG_INFO("GPU capture: started, writing {}", output);
  return true;
}

}  // namespace

void Fable2GpuCaptureOnPresent() {
  const std::string& trigger = REXCVAR_GET(fable2_gpu_capture_trigger);
  if (trigger.empty()) {
    return;
  }
  if (g_state == State::kCapturing) {
    // One whole frame (present to present) has been recorded.
    Send<void>(g_manager, "stopCapture");
    g_state = State::kIdle;
    REXLOG_INFO("GPU capture: finished");
    return;
  }
  std::error_code ec;
  if (!std::filesystem::exists(trigger, ec)) {
    return;
  }
  std::filesystem::remove(trigger, ec);
  std::string output = REXCVAR_GET(fable2_gpu_capture_output);
  if (output.empty()) {
    output = trigger + ".gputrace";
  }
  if (StartCapture(output)) {
    g_state = State::kCapturing;
  }
}

#else

void Fable2GpuCaptureOnPresent() {}

#endif
