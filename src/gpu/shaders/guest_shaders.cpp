// gpu/shaders/guest_shaders.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/shaders/guest_shaders.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include <xxhash.h>
#include <zstd.h>

#include <rex/cvar.h>
#include "core/logging.h"
#include "core/profiling.h"

#include "core/memory_helpers.h"
#include "gpu/backend.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/shaders/shader_cache.h"
#include "gpu/shaders/runtime_shaders.h"

#if !defined(EOT_D3D12)
#include <smolv.h>
#endif

void Fable2WriteShaderContainer(const unsigned char *virtual_bytes, unsigned virtual_size,
                                const unsigned char *physical_bytes, unsigned physical_size,
                                unsigned long long hash);

REXCVAR_DEFINE_STRING(fable2_debug_ignore_builtin_shaders, "", "Fable2",
                      "Debug: shader hashes whose built-in cache entries are ignored (runtime translation test)");

namespace eot::gpu {
static bool IgnoredBuiltin(u64 hash);

namespace {

std::string GuestEntryPoint(const u8 *bytes, size_t size) {
#if defined(EOT_D3D12)
  (void)bytes;
  (void)size;
  return "main";
#else
  const auto *words = reinterpret_cast<const u32 *>(bytes);
  const size_t count = size / 4;
  constexpr u32 kOpEntryPoint = 15;
  for (size_t i = 5; i < count;) {
    const u32 opcode = words[i] & 0xFFFFu;
    const u32 length = words[i] >> 16;
    if (length == 0)
      break;
    if (opcode == kOpEntryPoint && i + 3 < count) {
      const char *name = reinterpret_cast<const char *>(&words[i + 3]);
      const size_t max = (std::min(i + length, count) - (i + 3)) * 4;
      return std::string(name, strnlen(name, max));
    }
    i += length;
  }
  return "shaderMain";
#endif
}

// The execution model of a SPIR-V module's entry point (0 vertex, 4
// fragment), or ~0u when it has none.
u32 GuestEntryModel(const u8 *bytes, size_t size) {
  const auto *words = reinterpret_cast<const u32 *>(bytes);
  const size_t count = size / 4;
  constexpr u32 kOpEntryPoint = 15;
  for (size_t i = 5; i < count;) {
    const u32 opcode = words[i] & 0xFFFFu;
    const u32 length = words[i] >> 16;
    if (length == 0)
      break;
    if (opcode == kOpEntryPoint && i + 1 < count)
      return words[i + 1];
    i += length;
  }
  return ~0u;
}

#if defined(EOT_MVK)
void StrictMathForMetal(std::vector<u8> &spirv) {
  constexpr u32 kOpExtension = 10;
  constexpr u32 kOpEntryPoint = 15;
  constexpr u32 kOpExecutionMode = 16;
  constexpr u32 kOpCapability = 17;
  constexpr u32 kOpDecorate = 71;
  constexpr u32 kDecorationNoContraction = 42;
  constexpr u32 kCapabilitySignedZeroInfNanPreserve = 4466;
  constexpr u32 kExecutionModeSignedZeroInfNanPreserve = 4461;
  constexpr u32 kSpirv14 = 0x00010400;
  static constexpr char kExtension[] = "SPV_KHR_float_controls";

  if (spirv.size() < 20 || spirv.size() % 4)
    return;
  std::vector<u32> in(spirv.size() / 4);
  std::memcpy(in.data(), spirv.data(), spirv.size());
  const auto is_no_contraction = [&](size_t i, u32 opcode, u32 length) {
    return opcode == kOpDecorate && length == 3 && in[i + 2] == kDecorationNoContraction;
  };

  bool no_contraction = false;
  bool has_capability = false;
  std::vector<u32> entries;
  for (size_t i = 5; i < in.size();) {
    const u32 opcode = in[i] & 0xFFFFu;
    const u32 length = in[i] >> 16;
    if (length == 0 || i + length > in.size())
      return;
    if (is_no_contraction(i, opcode, length))
      no_contraction = true;
    else if (opcode == kOpCapability && in[i + 1] == kCapabilitySignedZeroInfNanPreserve)
      has_capability = true;
    else if (opcode == kOpEntryPoint && length >= 3)
      entries.push_back(in[i + 2]);
    else if (opcode == kOpExecutionMode && length >= 3 && in[i + 2] == kExecutionModeSignedZeroInfNanPreserve)
      return;
    i += length;
  }
  if (!no_contraction || entries.empty())
    return;

  std::vector<u32> out(in.begin(), in.begin() + 5);
  out.reserve(in.size() + 16);
  bool past_capabilities = false;
  bool past_entries = false;
  bool seen_entry = false;
  for (size_t i = 5; i < in.size();) {
    const u32 opcode = in[i] & 0xFFFFu;
    const u32 length = in[i] >> 16;
    if (!past_capabilities && opcode != kOpCapability) {
      past_capabilities = true;
      if (!has_capability) {
        out.push_back((2u << 16) | kOpCapability);
        out.push_back(kCapabilitySignedZeroInfNanPreserve);
      }
      if (in[1] < kSpirv14) {
        u32 name[(sizeof(kExtension) + 3) / 4] = {};
        std::memcpy(name, kExtension, sizeof(kExtension));
        out.push_back((u32(1 + std::size(name)) << 16) | kOpExtension);
        out.insert(out.end(), std::begin(name), std::end(name));
      }
    }
    if (seen_entry && !past_entries && opcode != kOpEntryPoint) {
      past_entries = true;
      for (const u32 entry : entries) {
        out.push_back((4u << 16) | kOpExecutionMode);
        out.push_back(entry);
        out.push_back(kExecutionModeSignedZeroInfNanPreserve);
        out.push_back(32);
      }
    }
    if (opcode == kOpEntryPoint)
      seen_entry = true;
    if (!is_no_contraction(i, opcode, length))
      out.insert(out.end(), in.begin() + i, in.begin() + i + length);
    i += length;
  }
  spirv.resize(out.size() * 4);
  std::memcpy(spirv.data(), out.data(), spirv.size());
}
#endif

struct CacheState {
  std::vector<u8> blob;
  std::unordered_map<u64, const ShaderCacheEntry *> by_hash;
  std::unordered_map<u64, u64> canonical;
  std::mutex host_mutex;
  std::unordered_map<u64, std::unique_ptr<plume::RenderShader>> host;
  bool ready = false;
};

CacheState &cache() {
  static CacheState c;
  return c;
}

const u8 *EntryBytes(const ShaderCacheEntry &e, u32 *size,
                     VsVariant variant = VsVariant::Trimmed) {
  auto &c = cache();
  // Fable 2: entries translated at run time keep their SPIR-V in their own blob.
  const u8 *base = RuntimeShaderBlob(&e);
  if (!base)
    base = c.blob.empty() ? nullptr : c.blob.data();
  if (!base) {
    *size = 0;
    return nullptr;
  }
  u32 offset = 0;
#if defined(EOT_D3D12)
  offset = e.dxilOffset;
  *size = e.dxilSize;
  if (variant == VsVariant::Full && e.fullDxilSize) {
    offset = e.fullDxilOffset;
    *size = e.fullDxilSize;
  } else if (variant == VsVariant::PositionOnly && e.posDxilSize) {
    offset = e.posDxilOffset;
    *size = e.posDxilSize;
  } else if (variant == VsVariant::Velocity && e.velDxilSize) {
    offset = e.velDxilOffset;
    *size = e.velDxilSize;
  }
#else
  offset = e.spirvOffset;
  *size = e.spirvSize;
  if (variant == VsVariant::Full && e.fullSpirvSize) {
    offset = e.fullSpirvOffset;
    *size = e.fullSpirvSize;
  } else if (variant == VsVariant::PositionOnly && e.posSpirvSize) {
    offset = e.posSpirvOffset;
    *size = e.posSpirvSize;
  } else if (variant == VsVariant::Velocity && e.velSpirvSize) {
    offset = e.velSpirvOffset;
    *size = e.velSpirvSize;
  }
#endif
  return base + offset;
}

}

bool GuestShadersInit() {
  auto &c = cache();
  if (c.ready)
    return !c.blob.empty();
  c.ready = true;
  RuntimeShadersInit();
#if defined(EOT_D3D12)
  const u8 *compressed = g_compressedDxilCache;
  const size_t compressed_size = g_dxilCacheCompressedSize;
  const size_t decompressed_size = g_dxilCacheDecompressedSize;
#else
  const u8 *compressed = g_compressedSpirvCache;
  const size_t compressed_size = g_spirvCacheCompressedSize;
  const size_t decompressed_size = g_spirvCacheDecompressedSize;
#endif
  if (g_shaderCacheEntryCount == 0 || decompressed_size == 0) {
    EOT_INFO("[shaders] no built-in shader cache (no shaders were captured before the build): "
             "every shader is translated while the game runs");
    return false;
  }
  c.blob.resize(decompressed_size);
  const size_t got =
      ZSTD_decompress(c.blob.data(), decompressed_size, compressed, compressed_size);
  if (ZSTD_isError(got) || got != decompressed_size) {
    EOT_ERROR("[shaders] shader cache decompression failed: {}",
              ZSTD_isError(got) ? ZSTD_getErrorName(got) : "short read");
    c.blob.clear();
    return false;
  }
  c.by_hash.reserve(g_shaderCacheEntryCount);
  std::unordered_map<u64, u64> by_bytes;
  std::string canon_rows;
  for (size_t i = 0; i < g_shaderCacheEntryCount; ++i) {
    const ShaderCacheEntry &e = g_shaderCacheEntries[i];
    c.by_hash.emplace(e.hash, &e);
    u32 size = 0;
    const u8 *bytes = EntryBytes(e, &size);
    const u64 bh = bytes && size ? XXH3_64bits(bytes, size) : e.hash;
    const u64 canon = by_bytes.emplace(bh, e.hash).first->second;
    c.canonical.emplace(e.hash, canon);
    canon_rows += std::format("{:016x},{:016x}\n", e.hash, canon);
  }
  EOT_DEBUG("[shaders] {} entries, {} unique DXIL: {} streamed duplicates share a host shader",
            g_shaderCacheEntryCount, by_bytes.size(), g_shaderCacheEntryCount - by_bytes.size());
  if (FILE *f = std::fopen("pso/shader_canon.csv", "wb")) {
    std::fputs("hash,canonical\n", f);
    std::fwrite(canon_rows.data(), 1, canon_rows.size(), f);
    std::fclose(f);
  }
  EOT_INFO("[shaders] cache ready: {} shaders, {} MB decompressed", g_shaderCacheEntryCount,
           decompressed_size / (1024 * 1024));
  return true;
}

u32 GuestShaderCacheCount() { return static_cast<u32>(g_shaderCacheEntryCount); }

static u64 ShaderKey(u32 object_va, bool is_pixel) { return (u64(object_va) << 1) | (is_pixel ? 1u : 0u); }

GuestShader *FindGuestShader(VideoState &s, u32 object_va, bool is_pixel) {
  std::lock_guard lock(s.shaders_mutex);
  auto it = s.shaders.find(ShaderKey(object_va, is_pixel));
  return it == s.shaders.end() ? nullptr : it->second.get();
}

void DrainShaderGraveyardLocked(VideoState &s) {
  std::vector<std::unique_ptr<GuestShader>> dead;
  {
    std::lock_guard lock(s.shaders_mutex);
    dead.swap(s.shader_graveyard);
  }
}

u32 FloatConstantRegisters(const u8 *bytes, u32 size, u32 table_off, bool is_pixel) {
  constexpr u32 kFull = 256;
  constexpr u32 kTableBytes = 4 + 28;
  constexpr u32 kInfoBytes = 20;      // D3DXSHADER_CONSTANTINFO
  if (!table_off || table_off + kTableBytes > size)
    return kFull;
  const u8 *table = bytes + table_off + 4;
  const u32 constants = rex::memory::load_and_swap<u32>(table + 12);
  const u32 info_off = rex::memory::load_and_swap<u32>(table + 16);
  if (!constants || constants > 1024)
    return kFull;
  const u64 info_end = u64(table_off) + 4 + info_off + u64(constants) * kInfoBytes;
  if (info_end > size)
    return kFull;
  u32 end = 0;
  for (u32 i = 0; i < constants; ++i) {
    const u8 *ci = table + info_off + i * kInfoBytes;
    const u32 set = rex::memory::load_and_swap<u16>(ci + 4);
    const u32 index = rex::memory::load_and_swap<u16>(ci + 6);
    const u32 count = rex::memory::load_and_swap<u16>(ci + 8);
    if (set != 2) // D3DXRS_FLOAT4
      continue;
    // Fable 2: an array or matrix occupies index..index+count-1 (the
    // translated shaders read constants only through these table entries, and
    // arrays bounds-checked). Any count > 1 used to mean all 256 registers:
    // ~4 KB per stage per draw, 11-16 MB of constants per frame.
    if (index >= kFull || count == 0)
      return kFull;
    end = std::max(end, std::min(index + count, kFull));
  }
  (void)is_pixel;
  return std::max(end, 16u);
}

u32 TextureFetchMask(const u8 *physical, u32 physical_size, const ShaderRecord *shader) {
  constexpr u32 kAllSlots = 0xFFFFu;
  constexpr u32 kInstructionBytes = 12;
  if (!physical || !shader)
    return kAllSlots;
  const u32 code_offset = shader->physicalOffset;
  const u32 code_size = shader->size;
  if (code_size < kInstructionBytes || code_offset > physical_size ||
      code_size > physical_size - code_offset)
    return kAllSlots;

  const u8 *code = physical + code_offset;
  u32 control_end = code_size;
  u32 mask = 0;
  bool found_clause = false;
  for (u32 control_offset = 0; control_offset + kInstructionBytes <= control_end;
       control_offset += kInstructionBytes) {
    const u32 w0 = rex::memory::load_and_swap<u32>(code + control_offset);
    const u32 w1 = rex::memory::load_and_swap<u32>(code + control_offset + 4);
    const u32 w2 = rex::memory::load_and_swap<u32>(code + control_offset + 8);
    const u64 controls[2] = {
        u64(w0) | (u64(w1 & 0xFFFFu) << 32),
        u64(w1 >> 16) | (u64(w2) << 16),
    };
    for (u64 control : controls) {
      const u32 opcode = static_cast<u32>((control >> 44) & 0xFu);
      const bool exec = (opcode >= 1 && opcode <= 6) || opcode == 13 || opcode == 14;
      if (!exec)
        continue;
      found_clause = true;
      const u32 address = static_cast<u32>(control & 0xFFFu);
      const u32 count = static_cast<u32>((control >> 12) & 7u);
      u32 sequence = static_cast<u32>((control >> 16) & 0xFFFu);
      if (address && address * kInstructionBytes < control_end)
        control_end = address * kInstructionBytes;
      if (address > code_size / kInstructionBytes ||
          count > code_size / kInstructionBytes - address)
        return kAllSlots;
      for (u32 i = 0; i < count; ++i, sequence >>= 2) {
        if (!(sequence & 1u))
          continue;
        const u32 fetch = rex::memory::load_and_swap<u32>(
            code + (address + i) * kInstructionBytes);
        const u32 fetch_opcode = fetch & 0x1Fu;
        if (fetch_opcode != 1u && fetch_opcode != 19u)
          continue;
        const u32 slot = (fetch >> 20) & 0x1Fu;
        if (slot >= 16)
          return kAllSlots;
        mask |= 1u << slot;
      }
    }
  }
  return found_clause && control_end < code_size ? mask : kAllSlots;
}

// Cheap identity of a shader object's current contents: container header,
// microcode pointer, and the first bytes of both parts.
static u64 ShaderFingerprint(u32 object_va, bool is_pixel) {
  const u32 container_va = object_va + (is_pixel ? obj::kPixelShaderContainer : obj::kVertexShaderContainer);
  const u32 physical_va = mem::load<u32>(object_va + (is_pixel ? obj::kPixelShaderPhysical : obj::kVertexShaderPhysical));
  const auto *header = mem::at<ShaderContainerHeader>(container_va);
  if (!header)
    return 0;
  const u32 virtual_size = header->virtualSize, physical_size = header->physicalSize;
  u8 buf[4 + 128 + 128];
  std::memcpy(buf, &physical_va, 4);
  u32 n = 4;
  if (const u8 *v = mem::at<u8>(container_va)) {
    const u32 k = std::min<u32>(virtual_size, 128);
    std::memcpy(buf + n, v, k);
    n += k;
  }
  if (const u8 *p = physical_va && physical_size ? mem::at<u8>(physical_va) : nullptr) {
    const u32 k = std::min<u32>(physical_size, 128);
    std::memcpy(buf + n, p, k);
    n += k;
  }
  return XXH3_64bits(buf, n) | 1;
}

void RevalidateGuestShader(VideoState &s, u32 object_va, bool is_pixel) {
  if (!object_va)
    return;
  u64 known = 0;
  {
    std::lock_guard lock(s.shaders_mutex);
    auto it = s.shaders.find(ShaderKey(object_va, is_pixel));
    if (it == s.shaders.end() || !it->second)
      return;  // registered at its first draw
    known = it->second->fingerprint;
  }
  const u64 now = ShaderFingerprint(object_va, is_pixel);
  if (!now || now == known)
    return;
  const GuestShader *before = FindGuestShader(s, object_va, is_pixel);
  const u64 old_hash = before ? before->hash : 0;
  const GuestShader *after = RegisterGuestShader(s, object_va, is_pixel);
  if (after && after->hash != old_hash) {
    static u32 logged = 0;
    if (logged++ < 64)
      EOT_INFO("[shaders] {:#x} {} rewritten in place: {:016x} -> {:016x}", object_va, is_pixel ? "ps" : "vs",
               old_hash, after->hash);
  }
}

GuestShader *RegisterGuestShader(VideoState &s, u32 object_va, bool is_pixel) {
  if (!object_va)
    return nullptr;
  const u32 container_va =
      object_va + (is_pixel ? obj::kPixelShaderContainer : obj::kVertexShaderContainer);
  const u32 physical_va = mem::load<u32>(
      object_va + (is_pixel ? obj::kPixelShaderPhysical : obj::kVertexShaderPhysical));
  auto *header = mem::at<ShaderContainerHeader>(container_va);
  if (!header) {
    EOT_WARN("[shaders] register {:#x}: container unreadable", object_va);
    return nullptr;
  }
  const u32 flags = header->flags;
  if ((flags & 0xFFFFFF00u) != 0x102A1100u) {
    u32 n;
    if (DiagShouldLog(0x5B00, &n))
      EOT_WARN("[shaders] register {:#x}: bad container magic {:#x}", object_va, flags);
    return nullptr;
  }
  if (((flags & 0xFFu) == 0) != is_pixel) {
    // A pixel shader container in the vertex shader slot or the reverse.
    static std::mutex mutex;
    static std::unordered_set<u32> seen;
    std::lock_guard lock(mutex);
    if (seen.size() < 64 && seen.insert(object_va).second)
      EOT_WARN("[shaders] {:#x} bound as a {} shader holds a {} shader container (flags {:#x})", object_va,
               is_pixel ? "pixel" : "vertex", is_pixel ? "vertex" : "pixel", flags);
  }
  const u32 virtual_size = header->virtualSize;
  const u32 physical_size = header->physicalSize;
  const u8 *virtual_bytes = mem::at<u8>(container_va);
  const u8 *physical_bytes = physical_va ? mem::at<u8>(physical_va) : nullptr;
  if (!virtual_bytes || (physical_size && !physical_bytes) ||
      virtual_size < sizeof(*header)) {
    EOT_WARN("[shaders] register {:#x}: parts unreadable (virt {} phys {} @ {:#x})",
             object_va, virtual_size, physical_size, physical_va);
    return nullptr;
  }

  XXH3_state_t *xs = XXH3_createState();
  XXH3_64bits_reset(xs);
  XXH3_64bits_update(xs, virtual_bytes, virtual_size);
  if (physical_size)
    XXH3_64bits_update(xs, physical_bytes, physical_size);
  const u64 hash = XXH3_64bits_digest(xs);
  XXH3_freeState(xs);

  {
    std::lock_guard lock(s.shaders_mutex);
    auto it = s.shaders.find(ShaderKey(object_va, is_pixel));
    if (it != s.shaders.end() && it->second && it->second->hash == hash) {
      it->second->fingerprint = ShaderFingerprint(object_va, is_pixel);
      return it->second.get();
    }
  }
  auto sh = std::make_unique<GuestShader>();
  sh->va = object_va;
  sh->hash = hash;
  sh->fingerprint = ShaderFingerprint(object_va, is_pixel);
  sh->isPixel = is_pixel;
  auto &c = cache();
  auto it = c.by_hash.find(hash);
  sh->entry = it == c.by_hash.end() || IgnoredBuiltin(hash) ? RuntimeShaderEntry(hash) : it->second;
  if (!sh->entry)
    RuntimeShaderRequest(hash, virtual_bytes, virtual_size, physical_bytes, physical_size);
  // Fable 2: save shaders missing from the cache (fable2_shader_dump_dir) with
  // exactly the bytes hashed here, for the next shader cache build.
  if (!sh->entry)
    Fable2WriteShaderContainer(virtual_bytes, virtual_size, physical_bytes, physical_size, hash);
  if (sh->entry)
    sh->usesFloatConstants = sh->entry->usesFloatConstants != 0;
  sh->floatConstantRegs =
      FloatConstantRegisters(virtual_bytes, virtual_size, header->constantTableOffset, is_pixel);
  const u32 shader_off = header->shaderOffset;
  const ShaderRecord *shader_record =
      shader_off <= virtual_size && sizeof(ShaderRecord) <= virtual_size - shader_off
          ? reinterpret_cast<const ShaderRecord *>(virtual_bytes + shader_off)
          : nullptr;
  sh->textureFetchMask = TextureFetchMask(physical_bytes, physical_size, shader_record);

  if (!is_pixel) {
    auto *rec = reinterpret_cast<const VertexShaderRecord *>(shader_record);
    if (rec) {
      const u32 first = rec->field18;
      const u32 count = rec->vertexElementCount;
      if (count <= 32) {
        for (u32 i = 0; i < count; ++i) {
          const u32 v = rec->vertexElementsAndInterpolators[first + i];
          VertexInput in;
          in.usage = static_cast<u8>((v >> 12) & 0xF);
          in.usageIndex = static_cast<u8>((v >> 16) & 0xF);
          sh->inputs.push_back(in);
        }
      }
    }
    if (sh->inputs.empty() && sh->entry) {
      for (u32 i = 0; i < sh->entry->vertexLayoutCount; ++i) {
        const u32 w0 = EntryVertexLayouts(*sh->entry)[i * 2];
        if (w0 & 0x400)
          continue;  // pull site record
        VertexInput in;
        in.usage = static_cast<u8>(w0 & 0xF);
        in.usageIndex = static_cast<u8>((w0 >> 4) & 0xF);
        sh->inputs.push_back(in);
      }
    }
  }

  EOT_DEBUG("[shaders] {} {:#x} hash {:016x} {} inputs={} spec={:#x} regs={} texmask={:#06x}",
            is_pixel ? "ps" : "vs", object_va, hash, sh->entry ? "hit" : "MISS",
            sh->inputs.size(), sh->entry ? sh->entry->specConstantsMask : 0,
            sh->floatConstantRegs, sh->textureFetchMask);
  std::lock_guard lock(s.shaders_mutex);
  if (auto other = s.shaders.find(ShaderKey(object_va, !is_pixel)); other != s.shaders.end() && other->second) {
    // The address held the other stage's shader before (the game reuses freed
    // shader memory). Keyed by address alone, the stale entry was returned.
    static u32 logged = 0;
    if (logged++ < 64)
      EOT_INFO("[shaders] {:#x} reused: was {} {:016x}, now {} {:016x}", object_va, is_pixel ? "vs" : "ps",
               other->second->hash, is_pixel ? "ps" : "vs", hash);
  }
  auto &slot = s.shaders[ShaderKey(object_va, is_pixel)];
  if (slot && slot->hash == hash)
    return slot.get();
  if (slot)
    s.shader_graveyard.push_back(std::move(slot));
  slot = std::move(sh);
  s.shader_generation.fetch_add(1, std::memory_order_release);
  return slot.get();
}

u64 CanonicalShaderHash(u64 hash) {
  auto &c = cache();
  auto it = c.canonical.find(hash);
  return it == c.canonical.end() ? hash : it->second;
}

// Debug: built-in entries treated as missing (tests runtime translation).
static bool IgnoredBuiltin(u64 hash) {
  static const std::unordered_set<u64> ignored = [] {
    std::unordered_set<u64> out;
    const std::string &list = REXCVAR_GET(fable2_debug_ignore_builtin_shaders);
    for (size_t i = 0; i < list.size();) {
      size_t j = list.find(',', i);
      if (j == std::string::npos)
        j = list.size();
      if (j > i)
        out.insert(std::strtoull(list.substr(i, j - i).c_str(), nullptr, 16));
      i = j + 1;
    }
    return out;
  }();
  return !ignored.empty() && ignored.count(hash);
}

const ShaderCacheEntry *FindShaderCacheEntry(u64 hash) {
  auto &c = cache();
  auto it = c.by_hash.find(hash);
  return it != c.by_hash.end() && !IgnoredBuiltin(hash) ? it->second : RuntimeShaderEntry(hash);
}

const u32 *EntryVertexLayouts(const ShaderCacheEntry &e) {
  const u32 *runtime = RuntimeShaderLayouts(&e);
  return (runtime ? runtime : g_shaderVertexLayouts) + e.vertexLayoutOffset;
}

static void FillFromEntry(GuestShader &sh) {
  sh.usesFloatConstants = sh.entry->usesFloatConstants != 0;
  if (!sh.isPixel && sh.inputs.empty())
    VertexInputsFromEntry(*sh.entry, sh.inputs);
}

bool AttachRuntimeShader(GuestShader &sh) {
  if (sh.entry)
    return true;
  sh.entry = RuntimeShaderEntry(sh.hash);
  if (!sh.entry)
    return false;
  FillFromEntry(sh);
  EOT_INFO("[shaders] {} {:#x} hash {:016x}: runtime translation attached", sh.isPixel ? "ps" : "vs", sh.va, sh.hash);
  return true;
}

void VertexInputsFromEntry(const ShaderCacheEntry &e, std::vector<VertexInput> &out) {
  out.clear();
  for (u32 i = 0; i < e.vertexLayoutCount; ++i) {
    const u32 w0 = EntryVertexLayouts(e)[i * 2];
    if (w0 & 0x400)
      continue;  // pull site record
    VertexInput in;
    in.usage = static_cast<u8>(w0 & 0xF);
    in.usageIndex = static_cast<u8>((w0 >> 4) & 0xF);
    out.push_back(in);
  }
}

bool EntryHasVelocity(const ShaderCacheEntry *e) {
  if (!e)
    return false;
#if defined(EOT_D3D12)
  return e->velDxilSize != 0;
#else
  return e->velSpirvSize != 0;
#endif
}

VsVariant VsVariantFor(const ShaderCacheEntry *vs, const ShaderCacheEntry *ps, bool null_ps,
                       bool velocity) {
  if (null_ps)
    return VsVariant::PositionOnly;
  if (!vs || !ps)
    return VsVariant::Trimmed;
  if (velocity && EntryHasVelocity(vs) && EntryHasVelocity(ps) &&
      !(ps->interpolantMask & ~vs->interpolantMask))
    return VsVariant::Velocity;
  return (ps->interpolantMask & ~vs->interpolantMask) ? VsVariant::Full : VsVariant::Trimmed;
}

plume::RenderShader *GetHostShaderByHash(VideoState &s, u64 hash, u32 spec_mask, bool is_pixel,
                                         bool worker, VsVariant variant) {
  auto &c = cache();
  const ShaderCacheEntry *entry = FindShaderCacheEntry(hash);
  if (!entry || !s.device)
    return nullptr;
  if (is_pixel && variant != VsVariant::Velocity)
    variant = VsVariant::Trimmed;
  const u32 effective = 0;
  (void)spec_mask;
  const u64 salt = (u64(effective) << 3) | (u64(variant) << 1) | 1u;
  const u64 key = hash ^ (salt * 0x9E3779B97F4A7C15ull) ^ (is_pixel ? 1u : 0u);
  {
    std::lock_guard lock(c.host_mutex);
    auto it = c.host.find(key);
    if (it != c.host.end())
      return it->second.get();
  }
  u32 size = 0;
  const u8 *bytes = EntryBytes(*entry, &size, variant);
#if !defined(EOT_D3D12)
  std::vector<u8> decoded;
  if (bytes && size) {
    decoded.resize(smolv::GetDecodedBufferSize(bytes, size));
    if (decoded.empty() || !smolv::Decode(bytes, size, decoded.data(), decoded.size())) {
      EOT_ERROR("[shaders] SMOL-V decode failed for {:016x} ({} bytes)", hash, size);
      decoded.clear();
    }
#if defined(EOT_MVK)
    StrictMathForMetal(decoded);
#endif
    bytes = decoded.data();
    size = static_cast<u32>(decoded.size());
  }
#endif
  std::unique_ptr<plume::RenderShader> host;
#if !defined(EOT_D3D12)
  // MoltenVK aborts building a pipeline whose stage module has no entry point
  // of that stage; such a shader is left without a host shader (its draws drop).
  if (bytes && size && GuestEntryModel(bytes, size) != (is_pixel ? 4u : 0u)) {
    EOT_ERROR("[shaders] {:016x} ({}, variant {}): SPIR-V entry point model {} does not match", hash,
              is_pixel ? "pixel" : "vertex", static_cast<u32>(variant), GuestEntryModel(bytes, size));
    bytes = nullptr;
  }
#endif
  if (bytes && size) {
    (void)worker;
    host = s.device->createShader(bytes, size, GuestEntryPoint(bytes, size).c_str(), kHostShaderFormat);
    if (host)
      host->setName(std::format("{:016x}", hash));
    if (!host)
      EOT_ERROR("[shaders] createShader failed for {:016x}", hash);
  }
  std::lock_guard lock(c.host_mutex);
  auto [it, inserted] = c.host.emplace(key, std::move(host));
  return it->second.get();
}

plume::RenderShader *ResolveHostShader(VideoState &s, GuestShader &shader, u32 spec_mask,
                                       VsVariant variant) {
  if (!shader.entry) {
    if (!shader.cacheMissLogged) {
      shader.cacheMissLogged = true;
      EOT_WARN("[shaders] {} {:#x} hash {:016x} is not in the shader cache",
               shader.isPixel ? "ps" : "vs", shader.va, shader.hash);
    }
    return nullptr;
  }
  if (shader.isPixel && variant != VsVariant::Velocity)
    variant = VsVariant::Trimmed;
  if (shader.lastHost && shader.lastSpecMask == spec_mask && static_cast<VsVariant>(shader.lastVariant) == variant)
    return shader.lastHost;
  plume::RenderShader *host =
      GetHostShaderByHash(s, shader.hash, spec_mask, shader.isPixel, false, variant);
  if (host) {
    shader.lastSpecMask = spec_mask;
    shader.lastVariant = static_cast<u8>(variant);
    shader.lastHost = host;
  }
  return host;
}

}
