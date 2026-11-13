// gpu/format.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/format.h"

#include <atomic>

#include "core/logging.h"
#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(fable2_7e3_32bit, false, "Fable2",
                    "Native renderer: k_2_10_10_10_FLOAT (7e3) render targets as 32-bit 11/11/10 float, not RGBA16F");

namespace eot::gpu {

namespace {
namespace xe = rex::graphics::xenos;

void WarnUnmapped(const char *what, u32 value) {
  static std::atomic<u32> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 16)
    EOT_WARN("{}: no plume mapping for value {}, using the neutral default", what, value);
}
}

plume::RenderFormat ConvertColorRenderTargetFormat(u32 f) {
  using F = plume::RenderFormat;
  switch (static_cast<xe::ColorRenderTargetFormat>(f)) {
  case xe::ColorRenderTargetFormat::k_8_8_8_8:
  case xe::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
    return F::R8G8B8A8_UNORM;
  case xe::ColorRenderTargetFormat::k_2_10_10_10:
  case xe::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
    return F::R16G16B16A16_UNORM;
  case xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
  case xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    // 32 bits like the console's 7e3 (RGBA16F doubled the load/store traffic
    // of the 2x MSAA scene); its 2-bit alpha is written but never read back.
    if (REXCVAR_GET(fable2_7e3_32bit))
      return F::R11G11B10_FLOAT;
    return F::R16G16B16A16_FLOAT;
  case xe::ColorRenderTargetFormat::k_16_16_16_16:
  case xe::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
    return F::R16G16B16A16_FLOAT;
  case xe::ColorRenderTargetFormat::k_16_16:
  case xe::ColorRenderTargetFormat::k_16_16_FLOAT:
    return F::R16G16_FLOAT;
  case xe::ColorRenderTargetFormat::k_32_FLOAT:
    return F::R32_FLOAT;
  case xe::ColorRenderTargetFormat::k_32_32_FLOAT:
    return F::R32G32_FLOAT;
  default:
    WarnUnmapped("ConvertColorRenderTargetFormat", f);
    return F::R8G8B8A8_UNORM;
  }
}

plume::RenderFormat SampledViewFormat(plume::RenderFormat format) {
  if (format == plume::RenderFormat::D32_FLOAT)
    return plume::RenderFormat::R32_FLOAT;
  return format;
}

TextureFormatMapping MapTextureFormat(xe::TextureFormat format) {
  using F = plume::RenderFormat;
  TextureFormatMapping m;
  m.supported = true;
  switch (format) {
  case xe::TextureFormat::k_DXT1:
  case xe::TextureFormat::k_DXT1_AS_16_16_16_16:
    m.format = F::BC1_UNORM;
    m.srgbFormat = F::BC1_UNORM_SRGB;
    m.blockCompressed = true;
    break;
  case xe::TextureFormat::k_DXT2_3:
  case xe::TextureFormat::k_DXT2_3_AS_16_16_16_16:
    m.format = F::BC2_UNORM;
    m.srgbFormat = F::BC2_UNORM_SRGB;
    m.blockCompressed = true;
    break;
  case xe::TextureFormat::k_DXT4_5:
  case xe::TextureFormat::k_DXT4_5_AS_16_16_16_16:
    m.format = F::BC3_UNORM;
    m.srgbFormat = F::BC3_UNORM_SRGB;
    m.blockCompressed = true;
    break;
  case xe::TextureFormat::k_DXN:
    m.format = F::BC5_UNORM;
    m.blockCompressed = true;
    break;
  case xe::TextureFormat::k_DXT5A:
    m.format = F::BC4_UNORM;
    m.blockCompressed = true;
    break;
  case xe::TextureFormat::k_DXT3A:
    // Decoded on upload (4-bit alpha blocks to one byte per texel); Fable 2's
    // landscape blend maps, whose weights cull the terrain's vertices.
    m.format = F::R8_UNORM;
    break;
  case xe::TextureFormat::k_DXT3A_AS_1_1_1_1:
    m.format = F::BC2_UNORM;
    m.blockCompressed = true;
    m.convert = true;
    break;
  case xe::TextureFormat::k_CTX1:
    m.format = F::R8G8_UNORM;
    m.convert = true;
    break;
  case xe::TextureFormat::k_8_8_8_8:
  case xe::TextureFormat::k_8_8_8_8_A:
  case xe::TextureFormat::k_8_8_8_8_AS_16_16_16_16:
  case xe::TextureFormat::k_8_8_8_8_GAMMA_EDRAM:
    m.format = F::R8G8B8A8_UNORM;
    break;
  case xe::TextureFormat::k_8:
  case xe::TextureFormat::k_8_A:
  case xe::TextureFormat::k_8_B:
    m.format = F::R8_UNORM;
    break;
  case xe::TextureFormat::k_8_8:
    m.format = F::R8G8_UNORM;
    break;
  case xe::TextureFormat::k_16:
    m.format = F::R16_UNORM;
    break;
  case xe::TextureFormat::k_16_16:
    m.format = F::R16G16_UNORM;
    break;
  case xe::TextureFormat::k_16_16_16_16:
  case xe::TextureFormat::k_16_16_16_16_EDRAM:
    m.format = F::R16G16B16A16_UNORM;
    break;
  case xe::TextureFormat::k_16_FLOAT:
    m.format = F::R16_FLOAT;
    break;
  case xe::TextureFormat::k_16_16_FLOAT:
    m.format = F::R16G16_FLOAT;
    break;
  case xe::TextureFormat::k_16_16_16_16_FLOAT:
  // Fable 2: the _EXPAND formats are read as half floats (as in Xenia); the
  // game resolves its HDR scene into k_16_16_16_16_EXPAND.
  case xe::TextureFormat::k_16_16_16_16_EXPAND:
    m.format = F::R16G16B16A16_FLOAT;
    break;
  case xe::TextureFormat::k_16_EXPAND:
    m.format = F::R16_FLOAT;
    break;
  case xe::TextureFormat::k_16_16_EXPAND:
    m.format = F::R16G16_FLOAT;
    break;
  case xe::TextureFormat::k_32_FLOAT:
    m.format = F::R32_FLOAT;
    break;
  case xe::TextureFormat::k_32_32_FLOAT:
    m.format = F::R32G32_FLOAT;
    break;
  case xe::TextureFormat::k_32_32_32_32_FLOAT:
    m.format = F::R32G32B32A32_FLOAT;
    break;
  case xe::TextureFormat::k_2_10_10_10:
  case xe::TextureFormat::k_2_10_10_10_AS_16_16_16_16:
  case xe::TextureFormat::k_4_4_4_4:
  case xe::TextureFormat::k_1_5_5_5:
  case xe::TextureFormat::k_5_6_5:
    m.format = (format == xe::TextureFormat::k_2_10_10_10 ||
                format == xe::TextureFormat::k_2_10_10_10_AS_16_16_16_16)
                   ? F::R16G16B16A16_UNORM
                   : F::R8G8B8A8_UNORM;
    m.convert = true;
    break;
  case xe::TextureFormat::k_24_8:
  case xe::TextureFormat::k_24_8_FLOAT:
    m.format = F::R32_FLOAT;
    break;
  default:
    m.supported = false;
    break;
  }
  return m;
}

plume::RenderFormat ResolveDestinationFormat(xe::TextureFormat format, bool depth_source) {
  using F = plume::RenderFormat;
  if (depth_source)
    return F::D32_FLOAT_S8_UINT;
  switch (format) {
  case xe::TextureFormat::k_8_8_8_8:
  case xe::TextureFormat::k_8_8_8_8_A:
  case xe::TextureFormat::k_8_8_8_8_AS_16_16_16_16:
  case xe::TextureFormat::k_8_8_8_8_GAMMA_EDRAM:
    return F::R8G8B8A8_UNORM;
  case xe::TextureFormat::k_8:
  case xe::TextureFormat::k_8_A:
  case xe::TextureFormat::k_8_B:
    return F::R8_UNORM;
  case xe::TextureFormat::k_8_8:
    return F::R8G8_UNORM;
  case xe::TextureFormat::k_2_10_10_10:
  case xe::TextureFormat::k_2_10_10_10_AS_16_16_16_16:
    return F::R16G16B16A16_UNORM;
  case xe::TextureFormat::k_16_16:
  case xe::TextureFormat::k_16_16_FLOAT:
  case xe::TextureFormat::k_16_16_EDRAM:
  case xe::TextureFormat::k_16_16_EXPAND:
    return F::R16G16_FLOAT;
  case xe::TextureFormat::k_16_EXPAND:
    return F::R16_FLOAT;
  case xe::TextureFormat::k_32_FLOAT:
    return F::R32_FLOAT;
  case xe::TextureFormat::k_32_32_FLOAT:
    return F::R32G32_FLOAT;
  default:
    return F::R16G16B16A16_FLOAT;
  }
}

plume::RenderBlend ConvertBlendFactor(u32 value) {
  using B = plume::RenderBlend;
  switch (static_cast<xe::BlendFactor>(value)) {
  case xe::BlendFactor::kZero:
    return B::ZERO;
  case xe::BlendFactor::kOne:
    return B::ONE;
  case xe::BlendFactor::kSrcColor:
    return B::SRC_COLOR;
  case xe::BlendFactor::kOneMinusSrcColor:
    return B::INV_SRC_COLOR;
  case xe::BlendFactor::kSrcAlpha:
    return B::SRC_ALPHA;
  case xe::BlendFactor::kOneMinusSrcAlpha:
    return B::INV_SRC_ALPHA;
  case xe::BlendFactor::kDstColor:
    return B::DEST_COLOR;
  case xe::BlendFactor::kOneMinusDstColor:
    return B::INV_DEST_COLOR;
  case xe::BlendFactor::kDstAlpha:
    return B::DEST_ALPHA;
  case xe::BlendFactor::kOneMinusDstAlpha:
    return B::INV_DEST_ALPHA;
  case xe::BlendFactor::kSrcAlphaSaturate:
    return B::SRC_ALPHA_SAT;
  case xe::BlendFactor::kConstantColor:
  case xe::BlendFactor::kConstantAlpha:
    return B::BLEND_FACTOR;
  case xe::BlendFactor::kOneMinusConstantColor:
  case xe::BlendFactor::kOneMinusConstantAlpha:
    return B::INV_BLEND_FACTOR;
  default:
    WarnUnmapped("ConvertBlendFactor", value);
    return B::ZERO;
  }
}

plume::RenderBlendOperation ConvertBlendOp(u32 value) {
  using O = plume::RenderBlendOperation;
  switch (static_cast<xe::BlendOp>(value)) {
  case xe::BlendOp::kAdd:
    return O::ADD;
  case xe::BlendOp::kSubtract:
    return O::SUBTRACT;
  case xe::BlendOp::kMin:
    return O::MIN;
  case xe::BlendOp::kMax:
    return O::MAX;
  case xe::BlendOp::kRevSubtract:
    return O::REV_SUBTRACT;
  default:
    WarnUnmapped("ConvertBlendOp", value);
    return O::ADD;
  }
}

plume::RenderComparisonFunction ConvertCompareFunc(u32 value) {
  using F = plume::RenderComparisonFunction;
  switch (static_cast<xe::CompareFunction>(value & 7)) {
  case xe::CompareFunction::kNever:
    return F::NEVER;
  case xe::CompareFunction::kLess:
    return F::LESS;
  case xe::CompareFunction::kEqual:
    return F::EQUAL;
  case xe::CompareFunction::kLessEqual:
    return F::LESS_EQUAL;
  case xe::CompareFunction::kGreater:
    return F::GREATER;
  case xe::CompareFunction::kNotEqual:
    return F::NOT_EQUAL;
  case xe::CompareFunction::kGreaterEqual:
    return F::GREATER_EQUAL;
  case xe::CompareFunction::kAlways:
  default:
    return F::ALWAYS;
  }
}

plume::RenderStencilOp ConvertStencilOp(u32 value) {
  using S = plume::RenderStencilOp;
  switch (static_cast<xe::StencilOp>(value & 7)) {
  case xe::StencilOp::kKeep:
    return S::KEEP;
  case xe::StencilOp::kZero:
    return S::ZERO;
  case xe::StencilOp::kReplace:
    return S::REPLACE;
  case xe::StencilOp::kIncrementClamp:
    return S::INCREMENT_AND_CLAMP;
  case xe::StencilOp::kDecrementClamp:
    return S::DECREMENT_AND_CLAMP;
  case xe::StencilOp::kInvert:
    return S::INVERT;
  case xe::StencilOp::kIncrementWrap:
    return S::INCREMENT_AND_WRAP;
  case xe::StencilOp::kDecrementWrap:
    return S::DECREMENT_AND_WRAP;
  default:
    return S::KEEP;
  }
}

plume::RenderPrimitiveTopology ConvertPrimitiveType(u32 prim, bool *expand_needed) {
  using T = plume::RenderPrimitiveTopology;
  if (expand_needed)
    *expand_needed = false;
  switch (static_cast<xe::PrimitiveType>(prim)) {
  case xe::PrimitiveType::kPointList:
    return T::POINT_LIST;
  case xe::PrimitiveType::kLineList:
    return T::LINE_LIST;
  case xe::PrimitiveType::kLineStrip:
    return T::LINE_STRIP;
  case xe::PrimitiveType::kTriangleList:
    return T::TRIANGLE_LIST;
  case xe::PrimitiveType::kTriangleStrip:
    return T::TRIANGLE_STRIP;
  case xe::PrimitiveType::kTriangleFan:
  case xe::PrimitiveType::kRectangleList:
  case xe::PrimitiveType::kQuadList:
    if (expand_needed)
      *expand_needed = true;
    return T::TRIANGLE_LIST;
  default:
    WarnUnmapped("ConvertPrimitiveType", prim);
    return T::TRIANGLE_LIST;
  }
}

DeclTypeInfo DecodeDeclType(u32 t) {
  using F = plume::RenderFormat;
  DeclTypeInfo d;
  d.xenosFormat = t & 0x3F;
  d.signedData = ((t >> 8) & 1) != 0;
  d.integer = ((t >> 9) & 1) != 0;
  const bool bgra = ((t >> 16) & 0xFF) == 0x18; // D3DCOLOR swizzle
  switch (d.xenosFormat) {
  case 36: // k_32_FLOAT
    d.format = F::R32_FLOAT;
    d.byteSize = 4;
    break;
  case 37: // k_32_32_FLOAT
    d.format = F::R32G32_FLOAT;
    d.byteSize = 8;
    break;
  case 57: // k_32_32_32_FLOAT
    d.format = F::R32G32B32_FLOAT;
    d.byteSize = 12;
    break;
  case 38: // k_32_32_32_32_FLOAT
    d.format = F::R32G32B32A32_FLOAT;
    d.byteSize = 16;
    break;
  case 6: // k_8_8_8_8
    d.byteSize = 4;
    if (d.integer)
      d.format = d.signedData ? F::R8G8B8A8_SINT : F::R8G8B8A8_UINT;
    else if (bgra)
      d.format = F::B8G8R8A8_UNORM;
    else
      d.format = d.signedData ? F::R8G8B8A8_SNORM : F::R8G8B8A8_UNORM;
    break;
  case 10: // k_8_8
    d.byteSize = 2;
    d.format = d.integer ? (d.signedData ? F::R8G8_SINT : F::R8G8_UINT)
                         : (d.signedData ? F::R8G8_SNORM : F::R8G8_UNORM);
    break;
  case 2: // k_8
    d.byteSize = 1;
    d.format = d.integer ? (d.signedData ? F::R8_SINT : F::R8_UINT)
                         : (d.signedData ? F::R8_SNORM : F::R8_UNORM);
    break;
  case 25: // k_16_16
    d.byteSize = 4;
    d.sixteenBit = true;
    d.format = d.signedData ? F::R16G16_SNORM : F::R16G16_UNORM;
    break;
  case 26: // k_16_16_16_16
    d.byteSize = 8;
    d.sixteenBit = true;
    d.format = d.signedData ? F::R16G16B16A16_SNORM : F::R16G16B16A16_UNORM;
    break;
  case 24: // k_16
    d.byteSize = 2;
    d.sixteenBit = true;
    d.format = d.signedData ? F::R16_SNORM : F::R16_UNORM;
    break;
  case 31: // k_16_16_FLOAT
    d.byteSize = 4;
    d.sixteenBit = true;
    d.format = F::R16G16_FLOAT;
    break;
  case 32: // k_16_16_16_16_FLOAT
    d.byteSize = 8;
    d.sixteenBit = true;
    d.format = F::R16G16B16A16_FLOAT;
    break;
  case 30: // k_16_FLOAT
    d.byteSize = 2;
    d.sixteenBit = true;
    d.format = F::R16_FLOAT;
    break;
  case 33: // k_32
    d.byteSize = 4;
    d.format = d.signedData ? F::R32_SINT : F::R32_UINT;
    break;
  case 34: // k_32_32
    d.byteSize = 8;
    d.format = d.signedData ? F::R32G32_SINT : F::R32G32_UINT;
    break;
  case 7:
    d.byteSize = 4;
    d.format = F::R32_UINT;
    d.packedDec3 = true;
    break;
  case 16: // k_10_11_11
  case 17: // k_11_11_10: raw bits, decoded in the shader
    d.byteSize = 4;
    d.format = F::R32_UINT;
    d.packed111110 = true;
    break;
  default:
    d.format = F::UNKNOWN;
    break;
  }
  return d;
}

const char *DeclUsageSemantic(u32 usage) {
  static const char *const kNames[] = {"POSITION",   "BLENDWEIGHT", "BLENDINDICES", "NORMAL",
                                       "PSIZE",      "TEXCOORD",    "TANGENT",      "BINORMAL",
                                       "TESSFACTOR", "POSITIONT",   "COLOR",        "FOG",
                                       "DEPTH",      "SAMPLE"};
  return usage < std::size(kNames) ? kNames[usage] : "UNKNOWN";
}

}
