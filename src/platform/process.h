// Fable 2: the part of Edge of Time's platform/process.h the renderer uses.
#pragma once

#include <filesystem>

namespace eot::platform {

// Per-user data folder (pipeline caches and the like): the same folder
// ReXGlue uses for Fable 2's saves, $XDG_DATA_HOME or ~/.local/share, /Fable2.
std::filesystem::path DataDir();

}  // namespace eot::platform
