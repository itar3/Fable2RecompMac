// Fable 2: Edge of Time's XDK function names -> Fable 2 addresses.
//
// The renderer hooks (gpu/hooks) use the XDK function names Edge of Time gives
// them in its codegen config. Here each name maps to the recompiled Fable 2
// function (docs/NATIVE_RENDERER.md, D3D hooks). Names with no Fable 2
// function yet map to placeholders (gpu/fable2_d3d_names.cpp) that are never
// called, so the hooks still compile.
#pragma once

#define D3DDevice_SetRenderTarget sub_822869A0
#define __imp__D3DDevice_SetRenderTarget __imp__sub_822869A0
#define D3DDevice_SetDepthStencilSurface sub_82264590
#define __imp__D3DDevice_SetDepthStencilSurface __imp__sub_82264590
#define D3DDevice_SetViewport sub_821F9F58
#define __imp__D3DDevice_SetViewport __imp__sub_821F9F58
#define D3DDevice_SetTexture sub_821B6EE0
#define __imp__D3DDevice_SetTexture __imp__sub_821B6EE0
#define D3DDevice_SetVertexShader sub_82232510
#define __imp__D3DDevice_SetVertexShader __imp__sub_82232510
#define D3DDevice_SetPixelShader sub_82208BB0
#define __imp__D3DDevice_SetPixelShader __imp__sub_82208BB0
#define D3DDevice_SetStreamSource sub_821B6C60
#define __imp__D3DDevice_SetStreamSource __imp__sub_821B6C60
#define D3DDevice_SetIndices sub_8219CC30
#define __imp__D3DDevice_SetIndices __imp__sub_8219CC30
#define D3DDevice_Swap sub_82BA34D8
#define __imp__D3DDevice_Swap __imp__sub_82BA34D8
#define D3DDevice_DrawVertices sub_8221C3E8
#define __imp__D3DDevice_DrawVertices __imp__sub_8221C3E8
#define D3DDevice_DrawIndexedVertices sub_8221DFC0
#define __imp__D3DDevice_DrawIndexedVertices __imp__sub_8221DFC0
#define D3DDevice_ClearF sub_821E2FE0
#define __imp__D3DDevice_ClearF __imp__sub_821E2FE0
#define D3DDevice_Resolve sub_82206888
#define __imp__D3DDevice_Resolve __imp__sub_82206888
#define D3DDevice_BeginVertices sub_82205F68
#define __imp__D3DDevice_BeginVertices __imp__sub_82205F68
#define D3DDevice_CreateVertexShader sub_82B992B0
#define __imp__D3DDevice_CreateVertexShader __imp__sub_82B992B0
#define D3DDevice_CreatePixelShader sub_82B994B8
#define __imp__D3DDevice_CreatePixelShader __imp__sub_82B994B8
#define D3D_UnlockResource sub_822C2630
#define __imp__D3D_UnlockResource __imp__sub_822C2630
#define eot_D3D_CDevice_BlockOnFence sub_82242668
#define __imp__eot_D3D_CDevice_BlockOnFence __imp__sub_82242668
#define eot_D3D_CDevice_BlockOnSecondaryPosition sub_82206400
#define __imp__eot_D3D_CDevice_BlockOnSecondaryPosition __imp__sub_82206400
#define eot_D3D_CDevice_WaitPrimaryRingSpace sub_82286618
#define __imp__eot_D3D_CDevice_WaitPrimaryRingSpace __imp__sub_82286618

// BeginIndexedVertices(device, prim, base vertex, vertex count, index count,
// index format, stride, &index data, &vertex data on the stack).
#define D3DDevice_BeginIndexedVertices sub_82217DB8
#define __imp__D3DDevice_BeginIndexedVertices __imp__sub_82217DB8

// Not found in Fable 2 yet.
#define D3DDevice_SetGammaRamp fable2_unmapped_D3DDevice_SetGammaRamp
#define __imp__D3DDevice_SetGammaRamp __imp__fable2_unmapped_D3DDevice_SetGammaRamp
#define D3DDevice_SetPWLGamma fable2_unmapped_D3DDevice_SetPWLGamma
#define __imp__D3DDevice_SetPWLGamma __imp__fable2_unmapped_D3DDevice_SetPWLGamma
#define XGRegisterVertexShader fable2_unmapped_XGRegisterVertexShader
#define __imp__XGRegisterVertexShader __imp__fable2_unmapped_XGRegisterVertexShader
#define XGRegisterPixelShader fable2_unmapped_XGRegisterPixelShader
#define __imp__XGRegisterPixelShader __imp__fable2_unmapped_XGRegisterPixelShader
#define D3DDevice_IsFencePending fable2_unmapped_D3DDevice_IsFencePending
#define __imp__D3DDevice_IsFencePending __imp__fable2_unmapped_D3DDevice_IsFencePending
#define D3DDevice_BlockUntilIdle fable2_unmapped_D3DDevice_BlockUntilIdle
#define __imp__D3DDevice_BlockUntilIdle __imp__fable2_unmapped_D3DDevice_BlockUntilIdle

// Temporary (missing ground): unhooked draw functions (flush dirty state and
// emit DRAW_INDX like DrawVertices); tessellated-draw candidates.
#define Fable2_DrawCandidateA sub_8221C898
#define __imp__Fable2_DrawCandidateA __imp__sub_8221C898
#define Fable2_DrawCandidateB sub_82207AF8
#define __imp__Fable2_DrawCandidateB __imp__sub_82207AF8
