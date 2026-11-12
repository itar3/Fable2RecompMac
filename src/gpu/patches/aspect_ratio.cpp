// Fable 2: stand-ins for Edge of Time's aspect ratio and movie patches.

#include "gpu/patches/aspect_ratio.h"

namespace eot::gpu {

float ConfiguredAspectRatio() { return 16.0f / 9.0f; }
void ApplyAspectRatio() {}
bool TakeMovieDrawnFlag() { return false; }
bool TakeMovieResolveSkip() { return false; }

}  // namespace eot::gpu
