// gpu/vertex_layout.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/vertex_layout.h"

#include <array>
#include <cstddef>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/memory/utils.h>
#include <xxhash.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/backend.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/shaders/guest_shaders.h"

REXCVAR_DEFINE_BOOL(fable2_layout_from_shader, false, "Fable2",
                    "Native renderer: build vertex layouts from the shader's vfetch instructions, not the declaration");
REXCVAR_DEFINE_BOOL(fable2_relabel_vertex_fetches, true, "Fable2",
                    "Native renderer: vertex inputs take the declaration elements their fetch instructions read "
                    "(XenosRecomp's usages can be attached to the wrong fetches)");
REXCVAR_DEFINE_INT32(fable2_relabel_keep_usages, 0, "Fable2",
                     "Debug: bitmask of declaration usages (bit = D3DDECLUSAGE, 1024 = colour) the fetch "
                     "relabel leaves as declared, with the inputs they would trade with");
REXCVAR_DEFINE_BOOL(fable2_scaled_vertex_formats, true, "Fable2",
                    "Native renderer: read integer vertex data as floats (SCALED formats)");

namespace eot::gpu {

#include <cstring>

namespace {

struct LayoutCache {
  std::mutex mutex;
  std::unordered_map<u64, std::unique_ptr<InputLayout>> map;
};

LayoutCache &cache() {
  static LayoutCache c;
  return c;
}

struct DeclCacheEntry {
  u32 count = 0;
  u64 hash = 0;
  u8 raw[32 * sizeof(DeclElement)] = {};
};
std::unordered_map<u32, DeclCacheEntry> &decl_cache() {
  static std::unordered_map<u32, DeclCacheEntry> c;
  return c;
}

struct DecodedElement {
  u32 stream;
  u32 offset;
  u32 type;
  u32 usage;
  u32 usageIndex;
};

bool DecodeElements(const DeclElement *elements, u32 count, std::vector<DecodedElement> &out,
                    u64 *hash) {
  if (!elements || count == 0 || count > 32)
    return false;
  DeclElement canon[32];
  std::memcpy(canon, elements, count * sizeof(DeclElement));
  CanonicalizeDeclElements(canon, count);
  elements = canon;
  out.clear();
  out.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    const DeclElement &e = elements[i];
    DecodedElement d;
    d.stream = e.stream;
    d.offset = e.offset;
    d.type = e.type;
    d.usage = e.usage;
    d.usageIndex = e.usageIndex;
    if (d.stream == 0xFF)
      break;
    out.push_back(d);
  }
  *hash = XXH3_64bits(elements, count * sizeof(DeclElement));
  // An end marker alone: a vertex shader that pulls all its fetches.
  return !out.empty() || (count == 1 && elements[0].stream == 0xFF);
}

bool ReadDeclaration(u32 decl_va, std::vector<DecodedElement> &out, u64 *hash) {
  const u32 count = mem::load<u32>(decl_va + obj::kDeclElementCount);
  if (count == 0 || count > 32)
    return false;
  return DecodeElements(mem::at<DeclElement>(decl_va + obj::kDeclElements), count, out, hash);
}

const InputLayout *FindLayout(u64 key) {
  auto &c = cache();
  std::lock_guard lock(c.mutex);
  auto it = c.map.find(key);
  return it == c.map.end() ? nullptr : it->second.get();
}

const InputLayout *BuildInputLayout(u64 vs_hash, const std::vector<VertexInput> &inputs,
                                    const std::vector<DecodedElement> &decl, u64 key,
                                    const DeclElement *raw, u32 count);

void SetSwapBit(InputLayout &l, u32 usage, u32 index) {
  const u32 bit = 1u << (index & 31);
  switch (static_cast<DeclUsage>(usage)) {
  case DeclUsage::TexCoord:
    l.swappedTexcoords |= bit;
    break;
  case DeclUsage::Normal:
    l.swappedNormals |= bit;
    break;
  case DeclUsage::Binormal:
    l.swappedBinormals |= bit;
    break;
  case DeclUsage::Tangent:
    l.swappedTangents |= bit;
    break;
  case DeclUsage::BlendWeight:
    l.swappedBlendWeights |= bit;
    break;
  case DeclUsage::Position:
    l.swappedPositions |= bit;
    break;
  default:
    break;
  }
}

}

static const InputLayout *GetInputLayoutSlow(VideoState &s, GuestShader &vs, u32 declaration_va,
                                             u32 raw_count, const DeclElement *raw_elements,
                                             bool raw_ok);

// Fable 2: draws with no vertex declaration (Lionhead's shader-by-index
// path stores null) fetch with the formats bound into the vertex shader's
// vfetch instructions. Build the declaration from them: the shader cache
// records, per input, the vfetch instruction (and its parent full fetch).
static bool SynthesizeDeclaration(const GuestShader &vs, std::vector<DeclElement> &out) {
  out.clear();
  if (!vs.entry || !vs.entry->vertexLayoutCount)
    return false;
  // The code starts at the shader record's physicalOffset into the physical part.
  const u32 container = vs.va + obj::kVertexShaderContainer;
  const u32 shader_offset = mem::load<u32>(container + offsetof(ShaderContainerHeader, shaderOffset));
  const u32 physical_offset = mem::load<u32>(container + shader_offset);
  const u32 code_va = mem::load<u32>(vs.va + obj::kVertexShaderPhysical);
  const u8 *code = code_va ? mem::at<u8>(code_va + physical_offset) : nullptr;
  if (!code)
    return false;
  auto word = [&](u32 address, u32 i) {
    return rex::memory::load_and_swap<u32>(code + address * 12 + i * 4);
  };
  for (u32 i = 0; i < vs.entry->vertexLayoutCount && out.size() < 31; ++i) {
    const u32 w0 = EntryVertexLayouts(*vs.entry)[i * 2];
    const u32 w1 = EntryVertexLayouts(*vs.entry)[i * 2 + 1];
    if (w0 & 0x600)
      continue;  // vertex pulling and pull site records
    const u32 address = w1 & 0xFFFF, parent = w1 >> 16;
    const u32 d0p = word(parent, 0), d1 = word(address, 1), d2 = word(address, 2);
    const u32 d2p = word(parent, 2);
    const u32 slot = ((d0p >> 20) & 0x1F) * 3 + ((d0p >> 25) & 3);
    if (slot > 95)
      continue;
    DeclElement e{};
    e.stream = static_cast<u16>(95 - slot);
    e.offset = static_cast<u16>(((static_cast<i32>(d2 << 1) >> 9) * 4) & 0xFFFF);
    // d1: bit 12 format_comp_all (signed), bit 13 num_format_all (integer,
    // i.e. unnormalized), bits 16-21 format, bits 24-29 exp_adjust.
    e.type = ((d1 >> 16) & 0x3F) | (((d1 >> 12) & 1) << 8) | (((d1 >> 13) & 1) << 9);
    if (const i32 exp_adjust = static_cast<i32>(d1 << 2) >> 26) {
      static std::mutex mutex;
      static std::unordered_set<u32> seen;
      std::lock_guard lock(mutex);
      if (seen.insert(e.type | (static_cast<u32>(exp_adjust) << 16)).second)
        EOT_WARN("[layout] vs {:016x}: vfetch exp_adjust {} (type {:#x} usage {}{}) not applied",
                 vs.hash, exp_adjust, static_cast<u32>(e.type), w0 & 0xF, (w0 >> 4) & 0xF);
    }
    e.method = 0;
    e.pad = static_cast<u8>(d2p & 0xFF);  // stride in dwords (the parent full fetch's)
    e.usage = static_cast<u8>(w0 & 0xF);
    e.usageIndex = static_cast<u8>((w0 >> 4) & 0xF);
    out.push_back(e);
  }
  // Shaders that pull all their fetches have no regular vertex inputs left.
  bool pulls = false;
  for (u32 i = 0; i < vs.entry->vertexLayoutCount; ++i)
    pulls |= (EntryVertexLayouts(*vs.entry)[i * 2] & 0x200) != 0;
  if (out.empty() && !pulls)
    return false;
  DeclElement end{};
  end.stream = 0xFF;
  out.push_back(end);
  return true;
}

const std::vector<u8> &PulledSlots(GuestShader &vs) {
  if (!vs.pulledSlotsKnown) {
    vs.pulledSlotsKnown = true;
    vs.pulledSlots.clear();
    if (!vs.isPixel && vs.entry) {
      for (u32 i = 0; i < vs.entry->vertexLayoutCount; ++i) {
        const u32 w0 = EntryVertexLayouts(*vs.entry)[i * 2];
        if (w0 & 0x200)
          vs.pulledSlots.push_back(static_cast<u8>((w0 >> 16) & 0x7F));
      }
    }
  }
  return vs.pulledSlots;
}

const InputLayout *GetInputLayout(VideoState &s, GuestShader &vs, u32 declaration_va) {
  // Fable 2: the vfetch instructions name the fetch constants the GPU really
  // reads; the bound declaration can disagree (the fur shells fetch from
  // streams 2 and 3 while their declaration says 0 and 1).
  if (declaration_va && !vs.isPixel && REXCVAR_GET(fable2_layout_from_shader))
    declaration_va = 0;
  if (!declaration_va && !vs.isPixel && vs.synthLayout && vs.synthHash == vs.hash)
    return vs.synthLayout;
  if (!declaration_va && !vs.isPixel) {
    static std::mutex mutex;
    static std::unordered_map<u64, std::vector<DeclElement>> synthesized;
    std::vector<DeclElement> decl;
    {
      std::lock_guard lock(mutex);
      auto it = synthesized.find(vs.hash);
      if (it == synthesized.end()) {
        std::vector<DeclElement> built;
        SynthesizeDeclaration(vs, built);
        it = synthesized.emplace(vs.hash, std::move(built)).first;
      }
      decl = it->second;
    }
    if (decl.empty())
      return nullptr;
    const InputLayout *layout = GetInputLayoutFromRaw(
        vs.hash, vs.inputs, reinterpret_cast<const u8 *>(decl.data()), static_cast<u32>(decl.size()));
    if (layout) {
      auto *mut = const_cast<InputLayout *>(layout);
      for (const DeclElement &e : decl)
        if (e.stream < 16 && e.pad)
          mut->fetchStride[e.stream] = e.pad * 4u;
      vs.synthLayout = layout;
      vs.synthHash = vs.hash;
    }
    return layout;
  }
  if (!declaration_va || vs.isPixel)
    return nullptr;
  const u8 *decl_bytes = mem::at<u8>(declaration_va);
  const u32 raw_count =
      decl_bytes ? rex::memory::load_and_swap<u32>(decl_bytes + obj::kDeclElementCount) : 0;
  const auto *raw_elements =
      decl_bytes ? reinterpret_cast<const DeclElement *>(decl_bytes + obj::kDeclElements) : nullptr;
  const bool raw_ok = raw_count != 0 && raw_count <= 32 && raw_elements != nullptr;
  static_assert(sizeof(vs.lastDeclRaw) >= 32 * sizeof(DeclElement));
  if (raw_ok && vs.lastLayout && vs.lastDeclVa == declaration_va &&
      vs.lastDeclCount == raw_count &&
      std::memcmp(vs.lastDeclRaw, raw_elements, raw_count * sizeof(DeclElement)) == 0)
    return vs.lastLayout;
  const InputLayout *layout = GetInputLayoutSlow(s, vs, declaration_va, raw_count, raw_elements, raw_ok);
  if (layout && raw_ok) {
    vs.lastDeclVa = declaration_va;
    vs.lastDeclCount = raw_count;
    vs.lastLayout = layout;
    std::memcpy(vs.lastDeclRaw, raw_elements, raw_count * sizeof(DeclElement));
  }
  return layout;
}

// Fable 2: XenosRecomp can attach a shader's vertex fetch usages to the wrong
// fetch instructions (the dog's fur shells: COLOR0, NORMAL0, TEXCOORD0 and
// TEXCOORD2 in reverse order), so the input named NORMAL0 holds what the shader
// fetches as its texture coordinate. Each fetch in memory reads the declaration
// element the XDK bound it to (stream, offset, format): when every input whose
// usage-matched element disagrees with its fetch has exactly one element that
// matches the fetch, those elements take the inputs' usages.
static bool RelabelByFetches(const GuestShader &vs, std::vector<DecodedElement> &decl) {
  if (!vs.entry || !vs.entry->vertexLayoutCount || decl.empty())
    return false;
  const u32 container = vs.va + obj::kVertexShaderContainer;
  const u32 shader_offset = mem::load<u32>(container + offsetof(ShaderContainerHeader, shaderOffset));
  const u32 physical_offset = mem::load<u32>(container + shader_offset);
  const u32 code_va = mem::load<u32>(vs.va + obj::kVertexShaderPhysical);
  const u8 *code = code_va ? mem::at<u8>(code_va + physical_offset) : nullptr;
  if (!code)
    return false;
  auto word = [&](u32 address, u32 i) { return rex::memory::load_and_swap<u32>(code + address * 12 + i * 4); };
  struct Fetch {
    u32 usage, usageIndex, stream, offset, format, count;
  };
  std::vector<Fetch> fetches;
  for (u32 i = 0; i < vs.entry->vertexLayoutCount; ++i) {
    const u32 w0 = EntryVertexLayouts(*vs.entry)[i * 2];
    const u32 w1 = EntryVertexLayouts(*vs.entry)[i * 2 + 1];
    if (w0 & 0x600)
      continue;  // vertex pulling and pull site records
    const u32 address = w1 & 0xFFFF, parent = w1 >> 16;
    const u32 d0p = word(parent, 0), d1 = word(address, 1), d2 = word(address, 2);
    const u32 slot = ((d0p >> 20) & 0x1F) * 3 + ((d0p >> 25) & 3);
    if (slot > 95)
      continue;
    const Fetch f{w0 & 0xF, (w0 >> 4) & 0xF, 95 - slot, static_cast<u32>((static_cast<i32>(d2 << 1) >> 9) * 4),
                  (d1 >> 16) & 0x3F, 1};
    bool repeated = false;
    for (Fetch &g : fetches)
      if (g.usage == f.usage && g.usageIndex == f.usageIndex)
        repeated = true, g.count++;
    if (!repeated)
      fetches.push_back(f);
  }
  auto reads = [](const DecodedElement &d, const Fetch &f) {
    return d.stream == f.stream && d.offset == f.offset && (d.type & 0x3F) == f.format;
  };
  // Debug: every fetch next to the declaration element of its usage, once per
  // shader with a mismatch (what it reads, what the usage names).
  auto report = [&](const char *verdict) {
    static std::mutex mutex;
    static std::unordered_set<u64> seen;
    std::lock_guard lock(mutex);
    if (seen.size() >= 64 || !seen.insert(vs.hash).second)
      return;
    std::string line;
    for (const Fetch &f : fetches) {
      const DecodedElement *named = nullptr;
      for (const DecodedElement &d : decl)
        if (d.usage == f.usage && d.usageIndex == f.usageIndex)
          named = &d;
      line += std::format(" | {}{}: fetch s{}+{} fmt {}{}", DeclUsageSemantic(f.usage), f.usageIndex, f.stream,
                          f.offset, f.format, f.count > 1 ? std::format(" x{}", f.count) : "");
      if (named)
        line += std::format(", decl s{}+{} fmt {}", named->stream, named->offset, named->type & 0x3F);
      else
        line += ", not in decl";
    }
    std::string elems;
    for (const DecodedElement &d : decl)
      elems += std::format(" {}{}=s{}+{}/{}", DeclUsageSemantic(d.usage), d.usageIndex, d.stream, d.offset, d.type & 0x3F);
    EOT_INFO("[layout] vs {:016x}: fetches vs declaration ({}){} || decl:{}", vs.hash, verdict, line, elems);
  };
  // (element index, usage, usage index) for each input to move.
  std::vector<std::array<u32, 3>> moves;
  for (const Fetch &f : fetches) {
    if (f.count != 1)
      continue;  // several fetches share the usage: keep it
    const DecodedElement *named = nullptr;
    for (const DecodedElement &d : decl)
      if (d.usage == f.usage && d.usageIndex == f.usageIndex)
        named = &d;
    if (!named || reads(*named, f))
      continue;
    u32 found = ~0u, matches = 0;
    for (u32 k = 0; k < decl.size(); ++k)
      if (reads(decl[k], f))
        found = k, matches++;
    if (matches != 1) {
      report(matches ? "ambiguous: kept" : "a fetch matches no element: kept");
      return false;  // the fetch was not bound to this declaration: trust the usages
    }
    if (const u32 keep = static_cast<u32>(REXCVAR_GET(fable2_relabel_keep_usages));
        ((keep >> (f.usage & 31)) & 1) || ((keep >> (decl[found].usage & 31)) & 1))
      continue;  // debug: leave this pair as declared
    moves.push_back({found, f.usage, f.usageIndex});
  }
  if (moves.empty())
    return false;
  for (u32 a = 0; a < moves.size(); ++a)
    for (u32 b = a + 1; b < moves.size(); ++b)
      if (moves[a][0] == moves[b][0]) {
        report("two inputs want one element: kept");
        return false;
      }
  report("relabelled");
  std::string what;
  for (const auto &m : moves) {
    what += std::format(" {}{}<-{}{}", DeclUsageSemantic(m[1]), m[2], DeclUsageSemantic(decl[m[0]].usage),
                        decl[m[0]].usageIndex);
    decl[m[0]].usage = m[1];
    decl[m[0]].usageIndex = m[2];
  }
  {
    static std::mutex mutex;
    static std::unordered_set<u64> seen;
    std::lock_guard lock(mutex);
    if (seen.size() < 64 && seen.insert(vs.hash).second)
      EOT_INFO("[layout] vs {:016x}: inputs take the elements their fetches read:{}", vs.hash, what);
  }
  return true;
}

static const InputLayout *GetInputLayoutSlow(VideoState &s, GuestShader &vs, u32 declaration_va,
                                             u32 raw_count, const DeclElement *raw_elements,
                                             bool raw_ok) {
  (void)s;
  const u64 relabel_salt =
      REXCVAR_GET(fable2_relabel_vertex_fetches)
          ? 0x5BD1E9955BD1E995ull ^ (u64(static_cast<u32>(REXCVAR_GET(fable2_relabel_keep_usages))) * 0x9E3779B97F4A7C15ull)
          : 0;
  if (raw_ok) {
    auto dit = decl_cache().find(declaration_va);
    if (dit != decl_cache().end() && dit->second.count == raw_count &&
        std::memcmp(dit->second.raw, raw_elements, raw_count * sizeof(DeclElement)) == 0) {
      if (const InputLayout *hit = FindLayout(dit->second.hash ^ (vs.hash * 0x9E3779B97F4A7C15ull) ^ relabel_salt))
        return hit;
    }
  }
  std::vector<DecodedElement> decl;
  u64 decl_hash = 0;
  if (!ReadDeclaration(declaration_va, decl, &decl_hash)) {
    u32 n;
    if (DiagShouldLog(0x5D00 ^ declaration_va, &n))
      EOT_WARN("[layout] declaration {:#x} unreadable", declaration_va);
    return nullptr;
  }
  if (raw_ok) {
    DeclCacheEntry &e = decl_cache()[declaration_va];
    e.count = raw_count;
    e.hash = decl_hash;
    std::memcpy(e.raw, raw_elements, raw_count * sizeof(DeclElement));
  }
  const u64 key = decl_hash ^ (vs.hash * 0x9E3779B97F4A7C15ull) ^ relabel_salt;
  if (const InputLayout *hit = FindLayout(key))
    return hit;
  if (relabel_salt)
    RelabelByFetches(vs, decl);
  return BuildInputLayout(vs.hash, vs.inputs, decl, key, raw_elements, raw_count);
}

const InputLayout *GetInputLayoutFromRaw(u64 vs_hash, const std::vector<VertexInput> &inputs,
                                         const u8 *decl_raw, u32 decl_count) {
  std::vector<DecodedElement> decl;
  u64 decl_hash = 0;
  const auto *elements = reinterpret_cast<const DeclElement *>(decl_raw);
  if (!DecodeElements(elements, decl_count, decl, &decl_hash))
    return nullptr;
  const u64 key = decl_hash ^ (vs_hash * 0x9E3779B97F4A7C15ull);
  if (const InputLayout *hit = FindLayout(key))
    return hit;
  return BuildInputLayout(vs_hash, inputs, decl, key, elements, decl_count);
}

namespace {

// Xenos "integer" vertex fetches are unnormalized values read as floats
// (Vulkan's SCALED formats). The shaders declare every input but BLENDINDICES
// as float4, and MoltenVK rejects an unsigned integer format feeding a float
// input ("Vertex attribute type mismatch between host and shader"), so those
// pipelines failed and their draws were dropped.
plume::RenderFormat ScaledForFloatInput(const DeclTypeInfo &t) {
  using F = plume::RenderFormat;
  if (!t.integer)
    return t.format;
  switch (t.format) {
  case F::R8G8B8A8_UINT: return F::R8G8B8A8_USCALED;
  case F::R8G8B8A8_SINT: return F::R8G8B8A8_SSCALED;
  case F::R8G8_UINT: return F::R8G8_USCALED;
  case F::R8G8_SINT: return F::R8G8_SSCALED;
  case F::R8_UINT: return F::R8_USCALED;
  case F::R8_SINT: return F::R8_SSCALED;
  case F::R16G16B16A16_UNORM: return F::R16G16B16A16_USCALED;
  case F::R16G16B16A16_SNORM: return F::R16G16B16A16_SSCALED;
  case F::R16G16_UNORM: return F::R16G16_USCALED;
  case F::R16G16_SNORM: return F::R16G16_SSCALED;
  case F::R16_UNORM: return F::R16_USCALED;
  case F::R16_SNORM: return F::R16_SSCALED;
  default: return t.format;
  }
}

const InputLayout *BuildInputLayout(u64 vs_hash, const std::vector<VertexInput> &inputs,
                                    const std::vector<DecodedElement> &decl, u64 key,
                                    const DeclElement *raw, u32 count) {
  auto layout = std::make_unique<InputLayout>();
  layout->key = key;
  u32 location = 0;
  bool ok = true;
  for (const VertexInput &in : inputs) {
    const DecodedElement *match = nullptr;
    for (const auto &d : decl) {
      if (d.usage == in.usage && d.usageIndex == in.usageIndex) {
        match = &d;
        break;
      }
    }
    const char *semantic = DeclUsageSemantic(in.usage);
    if (!match) {
      layout->elements.emplace_back(semantic, in.usageIndex, location++,
                                    plume::RenderFormat::R32G32B32A32_FLOAT,
                                    kSyntheticVertexSlot, 0);
      layout->needsSyntheticSlot = true;
      continue;
    }
    DeclTypeInfo t = DecodeDeclType(match->type);
    if (t.format == plume::RenderFormat::UNKNOWN) {
      EOT_WARN("[layout] vs {:016x}: {}{} decl type {:#x} (xenos fmt {}) has no host format",
               vs_hash, semantic, in.usageIndex, match->type, t.xenosFormat);
      ok = false;
      break;
    }
    // Blend indices are a float4 input (XenosRecomp patch) and keep their
    // declared format: Fable 2's skinning shaders read bone weights through
    // BLENDINDICES (normalized bytes); reading them as 0..255 integers made
    // skinned meshes explode across the screen. Integer data goes through
    // the SCALED rule below like any other input.
    if (REXCVAR_GET(fable2_scaled_vertex_formats)) {
      const plume::RenderFormat scaled = ScaledForFloatInput(t);
      if (scaled != t.format)
        EOT_INFO("[layout] vs {:016x}: {}{} decl type {:#x} -> scaled format {}", vs_hash, semantic,
                 in.usageIndex, match->type, static_cast<u32>(scaled));
      t.format = scaled;
    }
    // Half-float texcoords (k_16_16_FLOAT, k_16_16_16_16_FLOAT) may carry the
    // integer flag too; read as integers they became huge UVs (swirl moire on
    // every wall).
    const u32 vformat = match->type & 0x3F;
    if (in.usage == static_cast<u32>(DeclUsage::TexCoord) && t.integer && t.sixteenBit &&
        vformat != 31 && vformat != 32) {
      if (g_mvk) {
        t.format = t.byteSize == 4 ? plume::RenderFormat::R16G16_SINT
                                   : plume::RenderFormat::R16G16B16A16_SINT;
      } else {
        t.format = t.byteSize == 4 ? plume::RenderFormat::R16G16_UINT
                                   : plume::RenderFormat::R16G16B16A16_UINT;
        layout->sintTexcoords |= 1u << in.usageIndex;
      }
    }
    // k_2_10_10_10 on inputs the shader doesn't unpack (everything but
    // normal, tangent and binormal, e.g. Fable 2's tangent in TEXCOORD1):
    // Metal reads it natively (x in the low bits, as on Xenos after the
    // host's 32-bit swap). The raw bits read as a float made garbage tangent
    // frames (swirl patterns on normal-mapped walls).
    const bool shader_unpacks = in.usage == static_cast<u32>(DeclUsage::Normal) ||
                                in.usage == static_cast<u32>(DeclUsage::Tangent) ||
                                in.usage == static_cast<u32>(DeclUsage::Binormal);
    if (t.packedDec3 && !shader_unpacks && !t.integer) {
      t.format = t.signedData ? plume::RenderFormat::A2B10G10R10_SNORM_PACK32
                              : plume::RenderFormat::A2B10G10R10_UNORM_PACK32;
      t.packedDec3 = false;
    }
    if (g_mvk && (t.packedDec3 || t.packed111110))
      t.format = plume::RenderFormat::R32_FLOAT;
    if (t.sixteenBit) {
      SetSwapBit(*layout, in.usage, in.usageIndex);
      layout->swapElements.push_back({static_cast<u8>(in.usage), static_cast<u8>(in.usageIndex),
                                      static_cast<u8>(match->stream)});
    }
    if (t.packedDec3) {
      switch (static_cast<DeclUsage>(in.usage)) {
      case DeclUsage::Normal:
        layout->packedDec3 |= 1u << in.usageIndex;
        break;
      case DeclUsage::Tangent:
        layout->packedDec3 |= 1u << (8 + in.usageIndex);
        break;
      case DeclUsage::Binormal:
        layout->packedDec3 |= 1u << (16 + in.usageIndex);
        break;
      default:
        break;
      }
      layout->anyPacked111110 = true;
    }
    if (t.packed111110)
      layout->anyPacked111110 = true;
    if (match->stream >= 16) {
      EOT_WARN("[layout] vs {:016x}: stream {} out of range", vs_hash, match->stream);
      ok = false;
      break;
    }
    layout->elements.emplace_back(semantic, in.usageIndex, location++, t.format, match->stream,
                                  match->offset);
    layout->streamMask |= 1u << match->stream;
    if (in.usage == static_cast<u32>(DeclUsage::Position) && in.usageIndex == 1)
      layout->morphStreams |= 1u << match->stream;
    const u32 extent = match->offset + t.byteSize;
    if (extent > layout->streamExtent[match->stream])
      layout->streamExtent[match->stream] = extent;
  }
  if (!ok)
    return nullptr;
  if (layout->anyPacked111110)
    layout->spec |= kSpecR11G11B10Normal;
  if (layout->sintTexcoords)
    layout->spec |= kSpecSintTexcoord;
  if (raw && count) {
    layout->declRaw.assign(reinterpret_cast<const u8 *>(raw),
                           reinterpret_cast<const u8 *>(raw) + count * sizeof(DeclElement));
    CanonicalizeDeclElements(reinterpret_cast<DeclElement *>(layout->declRaw.data()), count);
  }
  auto &c = cache();
  std::lock_guard lock(c.mutex);
  auto it = c.map.find(key);
  if (it != c.map.end())
    return it->second.get();
  auto *result = layout.get();
  c.map.emplace(key, std::move(layout));
  return result;
}

}

void CanonicalizeDeclElements(DeclElement *elements, u32 count) {
  for (u32 i = 0; i < count; ++i)
    elements[i].pad = 0;
}

}
