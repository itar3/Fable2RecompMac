// gpu/shaders/guest_shaders.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <vector>

#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/resources.h"

struct ShaderCacheEntry;

namespace eot::gpu {

struct VideoState;

constexpr u32 kSpecR11G11B10Normal = 1u << 0;
constexpr u32 kSpecAlphaTest = 1u << 1;
constexpr u32 kSpecSintTexcoord = 1u << 2;
constexpr u32 kSpecZeroMul = 1u << 3;   // XenosRecomp SPEC_CONSTANT_ZERO_MUL
constexpr u32 kSpecNanPaint = 1u << 4;  // XenosRecomp SPEC_CONSTANT_NAN_PAINT

bool GuestShadersInit();
u32 GuestShaderCacheCount();

GuestShader *RegisterGuestShader(VideoState &s, u32 object_va, bool is_pixel);
GuestShader *FindGuestShader(VideoState &s, u32 object_va, bool is_pixel);
// Fable 2 loads shader objects into memory that held other shaders of the same
// stage without a registration call; on every bind, a changed object (by a
// cheap fingerprint) is registered again.
void RevalidateGuestShader(VideoState &s, u32 object_va, bool is_pixel);
void DrainShaderGraveyardLocked(VideoState &s);

const ShaderCacheEntry *FindShaderCacheEntry(u64 hash);
// The entry's vertex layout records (w0, w1 pairs), built-in or runtime.
const u32 *EntryVertexLayouts(const ShaderCacheEntry &e);
// Fable 2: picks up a runtime translation for a shader that missed the cache
// (true once it has an entry).
bool AttachRuntimeShader(GuestShader &sh);
u64 CanonicalShaderHash(u64 hash);
void VertexInputsFromEntry(const ShaderCacheEntry &e, std::vector<VertexInput> &out);
enum class VsVariant : u8 { Trimmed = 0, Full = 1, PositionOnly = 2, Velocity = 3 };
VsVariant VsVariantFor(const ShaderCacheEntry *vs, const ShaderCacheEntry *ps, bool null_ps,
                       bool velocity = false);
bool EntryHasVelocity(const ShaderCacheEntry *e);

plume::RenderShader *GetHostShaderByHash(VideoState &s, u64 hash, u32 spec_mask, bool is_pixel,
                                         bool worker = false,
                                         VsVariant variant = VsVariant::Trimmed);

plume::RenderShader *ResolveHostShader(VideoState &s, GuestShader &shader, u32 spec_mask,
                                       VsVariant variant = VsVariant::Trimmed);

}
