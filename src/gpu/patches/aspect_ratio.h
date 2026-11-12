// Fable 2: stand-ins for Edge of Time's aspect ratio and movie patches
// (gpu/patches/aspect_ratio.cpp there), which are Spider-Man specific.
#pragma once

namespace eot::gpu {

// Output aspect ratio the frame is fitted to.
float ConfiguredAspectRatio();
void ApplyAspectRatio();

// Edge of Time's movie player hooks; Fable 2 has none of them yet.
bool TakeMovieDrawnFlag();
bool TakeMovieResolveSkip();

}  // namespace eot::gpu
