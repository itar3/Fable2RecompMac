// gpu/vertex_layout.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <vector>

#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/d3d.h"
#include "gpu/resources.h"
#include "gpu/shaders/guest_shaders.h"

namespace eot::gpu {

struct VideoState;

constexpr u32 kSpecLayoutBits = kSpecR11G11B10Normal | kSpecSintTexcoord;

void CanonicalizeDeclElements(DeclElement *elements, u32 count);

constexpr u32 kSyntheticVertexSlot = 15;

struct InputLayout {
  u64 key = 0;
  std::vector<plume::RenderInputElement> elements;
  u32 streamMask = 0;
  bool needsSyntheticSlot = false;
  u32 swappedTexcoords = 0;
  u32 swappedNormals = 0;
  u32 swappedBinormals = 0;
  u32 swappedTangents = 0;
  u32 swappedBlendWeights = 0;
  u32 swappedPositions = 0;
  // Fable 2: the 16-bit elements behind the swapped* bits (usage, index,
  // stream); the swap only applies to streams fetched 8-in-16.
  struct SwapElement {
    u8 usage, index, stream;
  };
  std::vector<SwapElement> swapElements;
  u32 sintTexcoords = 0;
  u32 packedDec3 = 0;
  bool anyPacked111110 = false;
  u32 spec = 0;
  u32 streamExtent[16] = {};
  // Fable 2: strides (bytes) from the vertex shader's vfetch instructions,
  // for draws with no vertex declaration (used when SetStreamSource set none).
  u32 fetchStride[16] = {};
  u32 morphStreams = 0;
  std::vector<u8> declRaw;
};

const InputLayout *GetInputLayout(VideoState &s, GuestShader &vs, u32 declaration_va);

// Fetch slots the vertex shader reads by vertex pulling (empty for most).
const std::vector<u8> &PulledSlots(GuestShader &vs);

const InputLayout *GetInputLayoutFromRaw(u64 vs_hash, const std::vector<VertexInput> &inputs,
                                         const u8 *decl_raw, u32 decl_count);

}
