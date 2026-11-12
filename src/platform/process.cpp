// Fable 2: the part of Edge of Time's platform/process.cpp the renderer uses.

#include "platform/process.h"

#include <cstdlib>

namespace eot::platform {

std::filesystem::path DataDir() {
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
    return std::filesystem::path(xdg) / "Fable2";
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::filesystem::path(home) / ".local" / "share" / "Fable2";
  }
  return std::filesystem::current_path();
}

}  // namespace eot::platform
