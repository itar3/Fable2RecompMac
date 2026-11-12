// gpu/shaders/runtime_shaders.h
// Fable 2: shaders the built-in cache lacks, translated while the game runs.
//
// The built-in cache only holds shaders captured before the build. A shader met
// for the first time (a new area) used to drop its draws (black ground in the
// cave). Now the container goes to a background worker that runs XenosRecomp
// --blob on it; the result is loaded into a runtime table and kept on disk
// (fable2_runtime_shader_dir), so later launches have it from the start.

#pragma once

#include <cstdint>

#include "gpu/shaders/shader_cache.h"

namespace eot::gpu {

// Loads the shaders translated in earlier runs (once; later calls do nothing).
void RuntimeShadersInit();

// Queues the translation of a container (exactly the hashed bytes) unless it
// is known, queued, or failed before. Thread safe.
void RuntimeShaderRequest(uint64_t hash, const uint8_t *virtual_bytes, uint32_t virtual_size,
                          const uint8_t *physical_bytes, uint32_t physical_size);

// The translated entry, or nullptr while it is not ready. Thread safe.
const ShaderCacheEntry *RuntimeShaderEntry(uint64_t hash);

// For entries RuntimeShaderEntry returned: the bases their SPIR-V and vertex
// layout offsets count from (nullptr for built-in entries).
const uint8_t *RuntimeShaderBlob(const ShaderCacheEntry *e);
const uint32_t *RuntimeShaderLayouts(const ShaderCacheEntry *e);

}  // namespace eot::gpu
