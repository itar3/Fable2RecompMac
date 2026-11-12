// gpu/writeback.h
// Fable 2: resolves the CPU reads back (see writeback.cpp).
#pragma once

#include <rex/types.h>

namespace eot::gpu {

struct VideoState;
struct GuestTexture;

// Render thread, after a resolve into d: queue its readback when the CPU needs it.
// True when a hero/dog texture writeback was queued (see CompleteWritebacksNowLocked).
bool QueueResolveWritebackLocked(VideoState &s, GuestTexture &d);
// Submits the open command list, waits for it and stores its writebacks now.
void CompleteWritebacksNowLocked(VideoState &s);
// Whether a resolve destination (texture fetch dwords) is the hero/dog texture area.
bool IsHeroTextureFetch(const u32 fetch[6]);
// Render thread, when a frame slot's fence has been reached: store the texels.
void WritebacksSlotRecycledLocked(VideoState &s, u32 slot);

struct HostTexture;
// Temporary diagnostics (black sky): with fable2_debug_probe = "fx,fy", every
// frame-sized draw of the diag frame copies the pixel there (fractions of the
// target) from its colour and depth images; logged as [probe] when the frame's
// fence is reached (single-sampled images only: launch with fable2_guest_msaa off).
bool ProbeArmed();
void ProbeAfterDrawLocked(VideoState &s, HostTexture *colour, HostTexture *depth, u32 width, u32 height,
                          float scale, u64 vs, u64 ps, u32 prim, u32 count, u32 depth_control,
                          const float *ps_constants, u64 ps_constant_bytes);

}
