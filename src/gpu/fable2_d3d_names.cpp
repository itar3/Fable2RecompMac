// Fable 2: placeholders for XDK functions not found in Fable 2 yet (see
// gpu/fable2_d3d_names.h). The hooks that wrap them are never reached.

#include <rex/hook.h>

#include "gpu/fable2_d3d_names.h"

extern "C" REX_FUNC(__imp__D3DDevice_SetGammaRamp) {
  (void)ctx;
  (void)base;
}

extern "C" REX_FUNC(__imp__D3DDevice_SetPWLGamma) {
  (void)ctx;
  (void)base;
}

extern "C" REX_FUNC(__imp__XGRegisterVertexShader) {
  (void)ctx;
  (void)base;
}

extern "C" REX_FUNC(__imp__XGRegisterPixelShader) {
  (void)ctx;
  (void)base;
}

extern "C" REX_FUNC(__imp__D3DDevice_IsFencePending) {
  (void)ctx;
  (void)base;
}

extern "C" REX_FUNC(__imp__D3DDevice_BlockUntilIdle) {
  (void)ctx;
  (void)base;
}
