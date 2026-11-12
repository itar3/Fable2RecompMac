// gpu/textures.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/resources.h"

namespace eot::gpu {

struct VideoState;

GuestTexture *GetGuestTexture(VideoState &s, u32 header_va, bool create_host_image = true);
// Debug: the header's memory uploaded from guest memory now (no resolve image).
GuestTexture *GuestMemoryCopy(VideoState &s, u32 header_va);
// The same, with the fetch constant given (textures no header points at, e.g.
// Fable 2's vertex textures: header_va is then a synthetic key).
GuestTexture *GetGuestTextureForFetch(VideoState &s, u32 header_va, const u32 fetch[6],
                                      bool create_host_image = true);
// A texture header's fetch constant as the guest holds it now (false if unset).
// Draws and resolves record their textures' headers with this: Fable 2 re-points
// headers at other render targets later in the frame, before the replay.
bool ReadTextureHeader(u32 header_va, u32 out[6]);
u32 StorageFormat(u32 format);
u32 PhysicalBase(u32 word1);
// Fable 2 re-points the header objects of its render targets at other targets
// right after binding them (the shadow map's at a bloom buffer), while the GPU
// samples what the device's fetch constant names. Resolve destinations are kept
// under a key made from their memory (physical base, size, format), which a
// header and the device's copy of its fetch constant both map to; draws find
// them by their fetch constant (null: not a render target, or switched off).
u32 RenderTargetKey(const u32 fetch[6]);
GuestTexture *FindRenderTarget(VideoState &s, const u32 fetch[6]);

u32 PrepareTextureForSampling(VideoState &s, GuestTexture &t,
                              u32 swizzle = kIdentityFetchSwizzle);
u32 SamplingSwizzle(const GuestTexture &t, u32 fetch_swizzle);

bool EnsureResolveMirror(VideoState &s, GuestTexture &t, bool depth_source, float scale = 0.0f,
                         plume::RenderFormat depth_format = plume::RenderFormat::UNKNOWN);

void NoteResolveDestination(VideoState &s, const GuestTexture &t);

void PreloadAnnouncedTextures(VideoState &s, f64 budget_ms);

plume::RenderFramebuffer *GetMipFramebuffer(VideoState &s, GuestTexture &t, u32 mip);

void NotifyResourceUnlocked(u32 resource_va);
void NotifyResourceUnlockedLocked(VideoState &s, u32 resource_va);
u64 ResourceUnlockSeq(u32 resource_va);

}

namespace eot::gpu {

// The fetch constant's LOD bias as bits to OR into a pixel sampler index.
u32 SamplerLodBiasBits(const u32 fc[6], u32 sampler_index);
// Adds bias_32nds (1/32 steps) to the LOD bias packed in a sampler index.
u32 SamplerAddLodBias(u32 sampler_index, i32 bias_32nds);
plume::RenderSamplerDesc DecodeSamplerFromFetch(const u32 fc[6], bool mipmapped_upload = true,
                                                bool synthesized_mips = false);

u32 ResolveSamplerSlotLocked(const plume::RenderSamplerDesc &desc);

}
