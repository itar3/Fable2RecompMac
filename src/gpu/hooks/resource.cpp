/**
 * @file    gpu/hooks/resource.cpp
 * @brief   Guest hooks that create and refcount D3D texture/surface
 *          resources.
 *
 * Narrow-slice port of re:Blue's gpu/hooks/resource.cpp, corrected against
 * this binary's own IDA database rather than assumed from re:Blue's: only
 * CreateTexture/CreateSurface/Release/AddRef/GetType/GetSurfaceLevel/
 * D3D_DestroyResource are here. Buffer creation/lock, LockRect, GetDesc and
 * the native-mirror fallback path aren't ported yet - see gpu/device/device.h
 * and gpu/guest/resources.h for what's deliberately missing underneath these.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */

// Fable 2: XDK names -> Fable 2 functions; before any use of the names.
#include "gpu/fable2_d3d_names.h"

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device.h"
#include "gpu/shaders/guest_shaders.h"
#include "gpu/textures.h"
#include "gpu/draw.h"
#include "gpu/trace.h"

using namespace eot;
using namespace eot::gpu;

REX_EXTERN(__imp__XGRegisterVertexShader);
REX_EXTERN(__imp__XGRegisterPixelShader);
REX_EXTERN(__imp__D3DDevice_CreateVertexShader);
REX_EXTERN(__imp__D3DDevice_CreatePixelShader);
REX_EXTERN(__imp__D3D_UnlockResource);

extern "C" REX_FUNC(XGRegisterVertexShader) {
  FlushPendingUpDraw();
  const u32 shader = ctx.r3.u32, physical = ctx.r4.u32;
  __imp__XGRegisterVertexShader(ctx, base);
  EOT_TRACE_CALL("XGRegisterVertexShader obj={:#x} phys={:#x}", shader, physical);
  auto &s = state();
  if (GuestShader *g = RegisterGuestShader(s, shader, false)) {
    if (physical >= 0x82000000u && physical < 0x83000000u) {
      g->createdByGuestCall = true;
      EOT_DEBUG("[shaders] xex-data shader vs {:#x} hash {:016x} microcode {:#x}", shader, g->hash, physical);
    }
  }
}

extern "C" REX_FUNC(XGRegisterPixelShader) {
  FlushPendingUpDraw();
  const u32 shader = ctx.r3.u32, physical = ctx.r4.u32;
  __imp__XGRegisterPixelShader(ctx, base);
  EOT_TRACE_CALL("XGRegisterPixelShader obj={:#x} phys={:#x}", shader, physical);
  auto &s = state();
  if (GuestShader *g = RegisterGuestShader(s, shader, true)) {
    if (physical >= 0x82000000u && physical < 0x83000000u) {
      g->createdByGuestCall = true;
      EOT_DEBUG("[shaders] xex-data shader ps {:#x} hash {:016x} microcode {:#x}", shader, g->hash, physical);
    }
  }
}

extern "C" REX_FUNC(D3DDevice_CreateVertexShader) {
  const u32 function = ctx.r3.u32;
  __imp__D3DDevice_CreateVertexShader(ctx, base);
  EOT_DEBUG("[shaders] CreateVertexShader blob={:#x} -> obj={:#x} (hdr {:#x} {:#x} {:#x})",
           function, ctx.r3.u32, mem::load<u32>(function), mem::load<u32>(function + 4),
           mem::load<u32>(function + 8));
}

extern "C" REX_FUNC(D3DDevice_CreatePixelShader) {
  const u32 function = ctx.r3.u32;
  __imp__D3DDevice_CreatePixelShader(ctx, base);
  EOT_DEBUG("[shaders] CreatePixelShader blob={:#x} -> obj={:#x}", function, ctx.r3.u32);
}

extern "C" REX_FUNC(D3D_UnlockResource) {
  FlushPendingUpDraw();
  const u32 resource = ctx.r3.u32;
  __imp__D3D_UnlockResource(ctx, base);
  NotifyResourceUnlocked(resource);
}
