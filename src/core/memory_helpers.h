// core/memory_helpers.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <atomic>

#include <rex/system/kernel_state.h>
#include <rex/types.h>

namespace eot {

template <typename T> using be = rex::be<T>;

namespace mem {

struct Bases {
  u8 *virtual_base = nullptr;
  u8 *physical_base = nullptr;
  u32 e0_offset = 0;
};
inline Bases g_bases;
inline std::atomic<u8 *> g_virtual_base{nullptr};

inline const Bases *bases() {
  if (g_virtual_base.load(std::memory_order_acquire))
    return &g_bases;
  auto *kernel = REX_KERNEL_STATE();
  auto *memory = kernel ? kernel->memory() : nullptr;
  u8 *virtual_base = memory ? memory->virtual_membase() : nullptr;
  if (!virtual_base)
    return nullptr;
  g_bases.physical_base = memory->template TranslatePhysical<u8 *>(0);
  g_bases.e0_offset = static_cast<u32>(memory->template TranslateVirtual<u8 *>(0xE0000000u) -
                                       (virtual_base + 0xE0000000u));
  g_bases.virtual_base = virtual_base;
  g_virtual_base.store(virtual_base, std::memory_order_release);
  return &g_bases;
}

template <typename T> inline T *at(u32 va) {
  if (va < 0x1000)
    return nullptr;
  const Bases *b = bases();
  if (!b)
    return nullptr;
  return reinterpret_cast<T *>(b->virtual_base + va + (va >= 0xE0000000u ? b->e0_offset : 0u));
}

template <typename T> inline T *phys(u32 gpu_address) {
  const Bases *b = bases();
  if (!b)
    return nullptr;
  return reinterpret_cast<T *>(b->physical_base + (gpu_address & 0x1FFFFFFFu));
}

template <typename T> inline T load(u32 va, T fallback = T{}) {
  auto *p = at<be<T>>(va);
  return p ? static_cast<T>(*p) : fallback;
}

template <typename T> inline bool store(u32 va, T value) {
  auto *p = at<be<T>>(va);
  if (!p)
    return false;
  *p = value;
  return true;
}

inline u32 u32at(u32 va) { return load<u32>(va); }
inline f32 f32at(u32 va) { return load<f32>(va); }

inline bool readable(u32 va, u32 bytes) {
  auto *kernel = REX_KERNEL_STATE();
  auto *memory = kernel ? kernel->memory() : nullptr;
  if (!memory || va < 0x1000 || bytes == 0 || va + bytes < va)
    return false;
  u32 cursor = va;
  const u32 end = va + bytes;
  while (cursor < end) {
    auto *heap = memory->LookupHeap(cursor);
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(cursor, &info))
      return false;
    if (!(info.state & rex::memory::kMemoryAllocationCommit) ||
        info.protect == rex::memory::kMemoryProtectNoAccess)
      return false;
    const u32 region_end = info.base_address + info.region_size;
    if (region_end <= cursor)
      return false;
    cursor = region_end;
  }
  return true;
}

inline const char *str(u32 va) {
  auto *p = at<const char>(va);
  return p ? p : "";
}

}
}
