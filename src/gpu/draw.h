// gpu/draw.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <atomic>

#include <rex/types.h>

#include "gpu/render_thread.h"

namespace eot::gpu {

struct VideoState;

void ReplayDrawLocked(VideoState &s, const DrawPacket &pk);
void ReplayClearLocked(VideoState &s, const ClearPacket &pk);
void ReplayResolveLocked(VideoState &s, const ResolvePacket &pk);

// Xbox 360 Z-pass emulation (render thread): the trigger draw samples a depth
// texture this scene resolves only later; the render thread first replays the
// scene's draws depth-only and that resolve, as the console's Z pass does.
bool DepthPrerunTrigger(const DrawPacket &pk);
bool DepthPrerunFeeds(const DrawPacket &trigger, const ResolvePacket &r);
bool DepthPrerunIncludes(const DrawPacket &pk);
bool DepthPrerunDebugKeeps(u32 index, const DrawPacket &pk);

struct FloatConstantDirty {
  u64 vs = ~0ull;
  u64 ps = ~0ull;
};

// Fable 2: float constants the game loads from its own memory (precompiled
// constant blocks, gpu/fable2_constants.cpp) are copied into the device shadow
// and marked here (one bit per four registers), OR-ed into the next draw's
// pending mask.
inline std::atomic<u64> g_extra_vs_constants_dirty{0};
inline std::atomic<u64> g_extra_ps_constants_dirty{0};
// Fable 2 (fable2_constants.cpp): constant buffer loads waiting for the next draw.
void ApplyDeferredConstantLoads(u32 device);

void DrawGuestPrimitives(u32 device_va, u32 primitive_type, u32 start_vertex,
                         u32 vertex_count, FloatConstantDirty constants);

// Fable 2: shaders loaded with the XDK's GpuLoadShaders (the device's own
// shader pointers stay null); cleared by SetVertexShader / SetPixelShader.
inline std::atomic<u32> g_gpu_owned_vs{0};
inline std::atomic<u32> g_gpu_owned_ps{0};
constexpr u32 kGpuOwnedNone = 1;  // GpuLoadShaders with no pixel shader

void QueueGuestUpDraw(u32 device_va, u32 primitive_type, u32 vertex_count, u32 stride,
                      u32 data_va, FloatConstantDirty constants);
void QueueGuestIndexedUpDraw(u32 device_va, u32 primitive_type, u32 vertex_count, u32 stride,
                             u32 vertex_va, u32 index_count, u32 index_va, bool index32,
                             FloatConstantDirty constants);
void FlushPendingUpDraw();

// Fable 2: a tessellated quad patch draw (patches first_patch.., drawn as grids).
void DrawGuestTessellatedPatches(u32 device_va, u32 first_patch, u32 patch_count, FloatConstantDirty constants);

// Fable 2 (memexport.cpp): runs a memory-export vertex shader's draw on the
// CPU (its output is a vertex stream for later draws). True if it was one.
bool EmulateMemExportDraw(u32 device_va, const u32 *indices, u32 index_count, u32 first, u32 count);

void DrawGuestIndexedPrimitives(u32 device_va, u32 primitive_type, i32 base_vertex,
                                u32 start_index, u32 index_count,
                                FloatConstantDirty constants);
void PrefetchIndexProbes(u32 device_va, u32 start_index, u32 index_count);
u64 DrawSortKey(const DrawPacket &pk, u32 *depth_func = nullptr);

void FlushPendingTransitions(VideoState &s);
void FlushGeometryStaging(VideoState &s);
struct GeometryCacheSizes {
  u64 indexUpload = 0, indexVram = 0, vertexUpload = 0, vertexVram = 0;
  u32 indexChunks = 0, vertexChunks = 0;
};
void GeometryCacheBytes(GeometryCacheSizes *out);
void ClearGuestTargets(u32 device_va, u32 flags, u32 rect_va, u32 color_va, float z, u32 stencil);

void ResolveGuest(u32 device_va, u32 flags, u32 src_rect_va, u32 dest_texture_va,
                  u32 dest_point_va, u32 dest_level, u32 clear_color_va, float clear_z);

}
