// gpu/frame_ring.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include <plume_render_interface.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <format>
#include <vector>

#if defined(EOT_D3D12)
#include <plume_d3d12.h>
#else
#include <plume_vulkan.h>
#endif
#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(fable2_chunk_submit_draws, 600, "Fable2",
                     "Native renderer: submit the frame so far at the first resolve after this many draws "
                     "(up to 4 times a frame; 0 = one submit at Present)");
REXCVAR_DEFINE_BOOL(fable2_destroy_when_queue_idle, true, "Fable2",
                    "Native renderer: destroy retired textures and buffers only while MoltenVK's "
                    "submit thread is idle (async submits lost the device otherwise)");

#include "core/logging.h"
#include "gpu/queries.h"
#include "gpu/writeback.h"
#include "gpu/render_thread.h"
#include "gpu/device.h"
#include "gpu/draw.h"
#include "gpu/gpu_timing.h"
#include "gpu/memory_report.h"
#include "gpu/format.h"

namespace eot::gpu {

void DestroyHostTexture(VideoState &s, HostTexture &host);

namespace {

bool SameTextureShape(const HostTexture &pooled, const HostTexture &want,
                      const plume::RenderTextureDesc &d) {
  const plume::RenderTextureDesc &p = pooled.desc;
  return p.dimension == d.dimension && p.width == d.width && p.height == d.height &&
         p.depth == d.depth && p.mipLevels == d.mipLevels && p.arraySize == d.arraySize &&
         p.format == d.format && p.flags == d.flags &&
         p.multisampling.sampleCount == d.multisampling.sampleCount &&
         pooled.viewDimension == want.viewDimension &&
         pooled.isDepth == want.isDepth;
}

constexpr f64 kHostTexturePoolSeconds = 10.0;
constexpr size_t kHostTexturePoolMax = 192;
constexpr u64 kHostTexturePoolMaxBytes = 256ull << 20;

void TrimHostTexturePool(VideoState &s) {
  while (!s.host_texture_pool.empty() && s.host_texture_pool_bytes > kHostTexturePoolMaxBytes) {
    auto &oldest = s.host_texture_pool.front();
    s.host_texture_pool_bytes -= std::min(s.host_texture_pool_bytes, oldest.bytes);
    DestroyHostTexture(s, oldest.host);
    s.host_texture_pool.erase(s.host_texture_pool.begin());
  }
}

}

namespace {
void BindListStateLocked(VideoState &s) {
  s.command_list->setGraphicsPipelineLayout(s.pipeline_layout.get());
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 0);
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 1);
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 2);
  s.command_list->setGraphicsDescriptorSet(s.sampler_descriptor_set.get(), 3);
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 4);
}
}

void BeginCommandList(VideoState &s) {
  if (s.command_list_open)
    return;
  if (s.shutting_down.load(std::memory_order_acquire) || !s.pipeline_layout)
    return;
  const u32 cur = s.frame.load(std::memory_order_relaxed);
  s.command_list = s.command_lists[cur].get();
  s.command_list->begin();
  QueriesCommandListBegunLocked(s);
  if (!s.null_texture_barriers_submitted) {
    // The null textures read (0, 0, 0, 1) through a view swizzle, which
    // MoltenVK ignores without full image view swizzle support; their texels
    // were never written, so shaders sampled uninitialised memory (different
    // every run). Write (0, 0, 0, 1) into every texel.
    plume::RenderTextureBarrier barriers[kNullTextureDescriptorCount];
    for (u32 i = 0; i < kNullTextureDescriptorCount; ++i)
      barriers[i] = plume::RenderTextureBarrier(s.null_textures[i].get(),
                                                plume::RenderTextureLayout::COPY_DEST);
    s.command_list->barriers(plume::RenderBarrierStage::COPY, barriers, kNullTextureDescriptorCount);
    UploadAlloc texel;
    if (UploadAllocate(kTextureRowPitchAlignment, kTexturePlacementAlignment, &texel)) {
      for (u64 b = 0; b < kTextureRowPitchAlignment; b += 4) {
        texel.cpu[b] = texel.cpu[b + 1] = texel.cpu[b + 2] = 0;
        texel.cpu[b + 3] = 0xFF;
      }
      for (u32 i = 0; i < kNullTextureDescriptorCount; ++i) {
        const u32 layers = i == kNullTextureCubeDescriptorIndex ? 6u : 1u;
        for (u32 layer = 0; layer < layers; ++layer)
          s.command_list->copyTextureRegion(
              plume::RenderTextureCopyLocation::Subresource(s.null_textures[i].get(), 0, layer),
              plume::RenderTextureCopyLocation::PlacedFootprint(
                  texel.buffer, plume::RenderFormat::R8G8B8A8_UNORM, 1, 1, 1,
                  static_cast<u32>(kTextureRowPitchAlignment / 4), texel.offset));
      }
    }
    for (u32 i = 0; i < kNullTextureDescriptorCount; ++i)
      barriers[i] = plume::RenderTextureBarrier(s.null_textures[i].get(),
                                                plume::RenderTextureLayout::SHADER_READ);
    s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, barriers, kNullTextureDescriptorCount);
    s.null_texture_barriers_submitted = true;
  }
  BindListStateLocked(s);
  s.command_list_open = true;
  s.list_draws = 0;
  GpuTimingFrameBegin(s, s.command_list, cur);
#if EOT_GPU_PROFILING
  SetGPUProfilerCommandList(static_cast<plume::D3D12CommandList *>(s.command_list)->d3d);
#endif
  s.bound_framebuffer = nullptr;
  s.bound_pipeline = nullptr;
  s.bound_draw_targets_valid = false;
  for (auto &stream : s.bound_vertex_streams)
    stream.valid = false;
  s.bound_index_stream.valid = false;
  for (u32 i = 0; i < 3; ++i) {
    s.bound_root_buffer[i] = nullptr;
    s.bound_root_offset[i] = 0;
  }
  s.bound_spec = ~0u;
}

void SubmitOpenListLocked(VideoState &s) {
  if (!s.command_list_open)
    return;
  const u32 cur = s.frame.load(std::memory_order_relaxed);
  QueriesBeforeSubmitLocked(s);
  FlushGeometryStaging(s);
  GpuTimingFrameEnd(s.command_list);
  s.command_lists[cur]->end();
  s.command_list_open = false;
  s.bound_framebuffer = nullptr;
  s.bound_pipeline = nullptr;
  s.bound_draw_targets_valid = false;
  for (auto &stream : s.bound_vertex_streams)
    stream.valid = false;
  s.bound_index_stream.valid = false;
  for (u32 i = 0; i < 3; ++i) {
    s.bound_root_buffer[i] = nullptr;
    s.bound_root_offset[i] = 0;
  }
  s.bound_spec = ~0u;
  FlushRetiredLocked(s);
  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  s.queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, s.fences[cur].get());
  s.command_list_submitted[cur] = true;
  NoteQueueSubmittedLocked(s);
}

// Chunked submits: the frame so far goes to the GPU (no fence: the frame's
// last submit signals the slot fence, and the queue runs in order) and the
// frame continues in a fresh command list. MoltenVK encodes and the GPU runs
// this part while the rest of the frame is still being recorded; with one
// submit per frame the GPU started only at Present and the next Present
// waited ~2 ms for it (the game thread waits in Present).
void MaybeSubmitChunkLocked(VideoState &s) {
  const i32 every = REXCVAR_GET(fable2_chunk_submit_draws);
  if (every <= 0 || !s.command_list_open || s.list_draws < static_cast<u32>(every) || s.frame_chunks >= 4 ||
      s.depth_prerun || QueryActiveLocked())
    return;
  const u32 cur = s.frame.load(std::memory_order_relaxed);
  FlushGeometryStaging(s);
  s.command_lists[cur]->end();
  s.command_list_open = false;
  s.bound_framebuffer = nullptr;
  s.bound_pipeline = nullptr;
  s.bound_draw_targets_valid = false;
  for (auto &stream : s.bound_vertex_streams)
    stream.valid = false;
  s.bound_index_stream.valid = false;
  for (u32 i = 0; i < 3; ++i) {
    s.bound_root_buffer[i] = nullptr;
    s.bound_root_offset[i] = 0;
  }
  s.bound_spec = ~0u;
  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  s.queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, nullptr);
  NoteQueueSubmittedLocked(s);
  s.chunk_inflight[cur].push_back(std::move(s.command_lists[cur]));
  if (!s.chunk_spare[cur].empty()) {
    s.command_lists[cur] = std::move(s.chunk_spare[cur].back());
    s.chunk_spare[cur].pop_back();
  } else {
    s.command_lists[cur] = s.queue->createCommandList();
  }
  s.command_list = s.command_lists[cur].get();
  s.command_list->begin();
  BindListStateLocked(s);
  s.command_list_open = true;
  s.list_draws = 0;
  s.frame_chunks++;
  s.perf.chunk_submits++;
}

namespace {

VkFence TailFence(VideoState &s) {
#if defined(EOT_D3D12)
  return nullptr;
#else
  if (!s.tail_fence)
    s.tail_fence = s.device->createCommandFence();
  return s.tail_fence ? static_cast<plume::VulkanCommandFence *>(s.tail_fence.get())->vk : VK_NULL_HANDLE;
#endif
}

// Reclaims the marker fence once it has signalled; true if the queue is idle.
bool QueueIdleLocked(VideoState &s, bool wait) {
  if (s.queue_idle)
    return true;
#if defined(EOT_D3D12)
  (void)wait;
  return true;
#else
  if (!s.tail_fence_inflight)
    return false;
  auto *dev = static_cast<plume::VulkanDevice *>(s.device.get());
  VkFence fence = TailFence(s);
  if (!s.tail_fence_is_last)
    wait = false;
  VkResult res = wait ? vkWaitForFences(dev->vk, 1, &fence, VK_TRUE, UINT64_MAX)
                      : vkGetFenceStatus(dev->vk, fence);
  if (res != VK_SUCCESS)
    return false;
  vkResetFences(dev->vk, 1, &fence);
  s.tail_fence_inflight = false;
  if (s.tail_fence_is_last) {
    s.tail_fence_is_last = false;
    s.queue_idle = true;
  }
  return s.queue_idle;
#endif
}

template <typename T> void MoveAll(std::vector<T> &from, std::vector<T> &to) {
  for (auto &p : from)
    to.push_back(std::move(p));
  from.clear();
}

}

void NoteQueueSubmittedLocked(VideoState &s) {
  s.queue_idle = false;
  s.tail_fence_is_last = false;
}

void NoteQueueDrainedLocked(VideoState &s) {
  s.queue_idle = true;
  FlushRetiredLocked(s);
}

void SubmitQueueMarkerLocked(VideoState &s) {
#if !defined(EOT_D3D12)
  if (!REXCVAR_GET(fable2_destroy_when_queue_idle))
    return;
  QueueIdleLocked(s, false); // reclaim the previous marker if it signalled
  if (s.tail_fence_inflight)
    return;
  VkFence fence = TailFence(s);
  if (fence == VK_NULL_HANDLE)
    return;
  auto *q = static_cast<plume::VulkanCommandQueue *>(s.queue.get())->queue;
  VkResult res;
  {
    const std::scoped_lock queue_lock(*q->mutex);
    res = vkQueueSubmit(q->vk, 0, nullptr, fence);
  }
  if (res != VK_SUCCESS) {
    EOT_WARN("[gpu] fence-only vkQueueSubmit failed ({})", static_cast<int>(res));
    return;
  }
  s.tail_fence_inflight = true;
  s.tail_fence_is_last = true;
#endif
}

void FlushRetiredLocked(VideoState &s) {
  if (s.texture_retired.empty() && s.view_retired.empty() && s.buffer_retired.empty() &&
      s.framebuffer_retired.empty())
    return;
  // Waiting is rare: only when the lists are 8 presents old (GPU bound).
  const bool overdue = s.presented_frames >= s.retired_since + 8;
  if (REXCVAR_GET(fable2_destroy_when_queue_idle) && !QueueIdleLocked(s, overdue))
    return;
  s.perf.retired_flushes++;
  static u64 flushes = 0, overdue_flushes = 0;
  ++flushes;
  if (overdue)
    ++overdue_flushes;
  if (flushes % 200 == 1)
    EOT_INFO("[gpu] retired objects destroyed with the queue idle: {} flushes ({} waited), this one "
             "{} textures {} buffers {} views {} framebuffers, {} presents old",
             flushes, overdue_flushes, s.texture_retired.size(), s.buffer_retired.size(),
             s.view_retired.size(), s.framebuffer_retired.size(), s.presented_frames - s.retired_since);
  s.framebuffer_retired.clear();
  s.view_retired.clear();
  s.texture_retired.clear();
  s.buffer_retired.clear();
}

void AdvanceAndWaitReused(VideoState &s) {
  const u32 slot = s.next_frame;
  s.frame.store(slot, std::memory_order_relaxed);
  s.frame_chunks = 0;
  s.next_frame = (slot + 1) % kNumFrames;
  s.command_list = s.command_lists[slot].get();
  if (s.command_list_submitted[slot]) {
    s.queue->waitForCommandFence(s.fences[slot].get());
    s.command_list_submitted[slot] = false;
    // The slot's chunks ran before its fenced submit.
    for (auto &l : s.chunk_inflight[slot])
      s.chunk_spare[slot].push_back(std::move(l));
    s.chunk_inflight[slot].clear();
    GpuTimingCollect(s, slot);
#if EOT_GPU_PROFILING
    if (auto *ctx = GpuProfilerCtx()) {
      TracyD3D12NewFrame(ctx);
      TracyD3D12Collect(ctx);
    }
#endif
  }
  // Fable 2: the bindless texture set is shared by both frames in flight.
  // Textures released while this slot's frame was recorded were still in the
  // set when the other slot's frame (recorded after them, possibly still on
  // the GPU) bound it, so MoltenVK made them resident for that command buffer.
  // Destroying them here gave "Invalid Resource" GPU faults (device lost) in
  // texture-heavy places such as Lucian's castle. So: null their slots first,
  // and destroy them after the other slot's fence (the next call).
  if (s.texture_retired.empty() && s.view_retired.empty() && s.buffer_retired.empty() &&
      s.framebuffer_retired.empty())
    s.retired_since = s.presented_frames;
  MoveAll(s.view_deferred, s.view_retired);
  MoveAll(s.texture_deferred, s.texture_retired);
  MoveAll(s.buffer_deferred, s.buffer_retired);
  MoveAll(s.framebuffer_graveyard[slot], s.framebuffer_retired);
  if (!REXCVAR_GET(fable2_destroy_when_queue_idle))
    FlushRetiredLocked(s);
  for (auto &b : s.buffer_graveyard[slot])
    s.buffer_deferred.push_back(std::move(b));
  s.buffer_graveyard[slot].clear();
  QueriesSlotRecycledLocked(s, slot);
  WritebacksSlotRecycledLocked(s, slot);
  DrainDescriptorSlotsLocked(s, slot);
  for (auto &v : s.view_graveyard[slot])
    s.view_deferred.push_back(std::move(v));
  s.view_graveyard[slot].clear();
  for (auto &t : s.texture_graveyard[slot])
    s.texture_deferred.push_back(std::move(t));
  s.texture_graveyard[slot].clear();
  UploadRingResetFrame(slot);
}

void ParkTexture(VideoState &s, std::unique_ptr<plume::RenderTexture> t) {
  if (!t)
    return;
  s.perf.host_parked++;
  NoteHostRelease(t.get());
  s.texture_graveyard[s.recording_slot()].push_back(std::move(t));
}

void ParkView(VideoState &s, std::unique_ptr<plume::RenderTextureView> v) {
  if (v)
    s.view_graveyard[s.recording_slot()].push_back(std::move(v));
}

void ParkFramebuffer(VideoState &s, std::unique_ptr<plume::RenderFramebuffer> f) {
  if (f)
    s.framebuffer_graveyard[s.recording_slot()].push_back(std::move(f));
}

void ParkBuffer(VideoState &s, std::unique_ptr<plume::RenderBuffer> b) {
  if (!b)
    return;
  NoteHostRelease(b.get());
  s.buffer_graveyard[s.recording_slot()].push_back(std::move(b));
}

void ParkHostTexture(VideoState &s, HostTexture &host) {
  const u64 bytes = host.texture ? HostTextureBytes(host) : 0;
  if (host.texture && s.host_texture_pool.size() < kHostTexturePoolMax &&
      bytes <= kHostTexturePoolMaxBytes) {
    VideoState::PooledHostTexture entry;
    entry.host = std::move(host);
    entry.freedFrame = s.guest_frames;
    entry.bytes = bytes;
    s.host_texture_pool.push_back(std::move(entry));
    s.host_texture_pool_bytes += bytes;
    host = HostTexture{};
    TrimHostTexturePool(s);
    return;
  }
  DestroyHostTexture(s, host);
}

void DestroyHostTexture(VideoState &s, HostTexture &host) {
  ReleaseTextureSRVLocked(s, host);
  if (host.texture) {
    const auto *dead = host.texture.get();
    for (auto it = s.framebuffers.begin(); it != s.framebuffers.end();) {
      bool names_dead = false;
      for (u32 i = 0; i < it->second.attachmentCount; ++i)
        names_dead = names_dead || it->second.attachments[i] == dead;
      if (!names_dead) {
        ++it;
        continue;
      }
      if (s.bound_framebuffer == it->second.fb.get())
        s.bound_framebuffer = nullptr;
      ParkFramebuffer(s, std::move(it->second.fb));
      it = s.framebuffers.erase(it);
    }
  }
  for (auto &fb : host.mipFramebuffers)
    ParkFramebuffer(s, std::move(fb));
  host.mipFramebuffers.clear();
  for (auto &v : host.mipViews)
    ParkView(s, std::move(v));
  host.mipViews.clear();
  ParkView(s, std::move(host.depthView));
  ParkView(s, std::move(host.srv));
  ParkTexture(s, std::move(host.texture));
  host.layout = plume::RenderTextureLayout::UNKNOWN;
}

bool CreateOrRecycleHostTexture(VideoState &s, HostTexture &host,
                                const plume::RenderTextureDesc &desc, const char *tag,
                                plume::RenderPool *pool) {
  const bool renderable = (desc.flags & plume::RenderTextureFlag::RENDER_TARGET) ||
                          (desc.flags & plume::RenderTextureFlag::DEPTH_TARGET);
  const bool transient = pool != nullptr;
  for (size_t i = 0; i < s.host_texture_pool.size(); ++i) {
    auto &entry = s.host_texture_pool[i];
    if (entry.freedFrame + kNumFrames > s.guest_frames || entry.host.transientPool != transient ||
        !SameTextureShape(entry.host, host, desc))
      continue;
    HostTexture pooled = std::move(entry.host);
    s.host_texture_pool_bytes -= std::min(s.host_texture_pool_bytes, entry.bytes);
    s.host_texture_pool.erase(s.host_texture_pool.begin() + i);
    host.texture = std::move(pooled.texture);
    host.desc = pooled.desc;
    host.srv = std::move(pooled.srv);
    host.descriptorIndex = pooled.descriptorIndex;
    host.swizzledSrvs = std::move(pooled.swizzledSrvs);
    host.mipViews = std::move(pooled.mipViews);
    host.mipFramebuffers = std::move(pooled.mipFramebuffers);
    host.depthView = std::move(pooled.depthView);
    host.transientPool = pooled.transientPool;
    host.layout = pooled.layout;
    host.needsClear = renderable;
    s.perf.host_tex_recycled++;
    RetagHostAllocation(host.texture.get(), tag, desc);
    return true;
  }
  host.texture = CreateHostTexture(s.device.get(), desc, tag, pool);
  host.transientPool = transient && host.texture != nullptr;
  if (!host.texture && transient)
    host.texture = CreateHostTexture(s.device.get(), desc, tag);
  host.desc = desc;
  host.desc.optimizedClearValue = nullptr;
  host.layout = plume::RenderTextureLayout::UNKNOWN;
  host.needsClear = renderable && host.texture != nullptr;
  return host.texture != nullptr;
}

void EvictHostTexturePool(VideoState &s) {
  size_t i = 0;
  while (i < s.host_texture_pool.size()) {
    const bool idle = FrameAgeSeconds(s, s.host_texture_pool[i].freedFrame) > kHostTexturePoolSeconds;
    const bool over = s.host_texture_pool.size() > kHostTexturePoolMax ||
                      s.host_texture_pool_bytes > kHostTexturePoolMaxBytes;
    if (!idle && !over) {
      ++i;
      continue;
    }
    s.host_texture_pool_bytes -= std::min(s.host_texture_pool_bytes, s.host_texture_pool[i].bytes);
    DestroyHostTexture(s, s.host_texture_pool[i].host);
    s.host_texture_pool.erase(s.host_texture_pool.begin() + i);
  }
  s.perf.pool_size = static_cast<u32>(s.host_texture_pool.size());
}

void TransitionLocked(VideoState &s, HostTexture &host, plume::RenderTextureLayout layout) {
  if (!host.texture || host.layout == layout || !s.command_list_open)
    return;
  if (s.defer_shader_read_transitions && layout == plume::RenderTextureLayout::SHADER_READ &&
      s.pending_transition_count < kMaxPendingTransitions) {
    s.pending_transitions[s.pending_transition_count++] = {&host, layout};
    return;
  }
  const HostTextureTransition transition{&host, layout};
  TransitionManyLocked(s, &transition, 1);
}

void TransitionManyLocked(VideoState &s, const HostTextureTransition *transitions, u32 count) {
  if (!transitions || !count || !s.command_list_open)
    return;
  constexpr u32 kBatch = 8;
  for (u32 first = 0; first < count; first += kBatch) {
    plume::RenderTextureBarrier barriers[kBatch];
    u32 barrier_count = 0;
    plume::RenderBarrierStages stages = plume::RenderBarrierStage::NONE;
    const u32 end = std::min(first + kBatch, count);
    for (u32 i = first; i < end; ++i) {
      HostTexture *host = transitions[i].host;
      const plume::RenderTextureLayout layout = transitions[i].layout;
      if (!host || !host->texture || host->layout == layout)
        continue;
      barriers[barrier_count++] = plume::RenderTextureBarrier(host->texture.get(), layout);
      if (layout == plume::RenderTextureLayout::COPY_SOURCE ||
          layout == plume::RenderTextureLayout::COPY_DEST)
        stages |= plume::RenderBarrierStage::COPY;
      else
        stages |= plume::RenderBarrierStage::GRAPHICS;
      host->layout = layout;
    }
    if (!barrier_count)
      continue;
    s.command_list->barriers(stages, barriers, barrier_count);
    s.perf.texture_barrier_calls++;
    s.perf.texture_barrier_resources += barrier_count;
  }
}

}

namespace eot::gpu {

namespace {

constexpr u64 kChunkSize = 32ull * 1024 * 1024;

struct Chunk {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u8 *cpu = nullptr;
  u64 capacity = 0;
  u64 used = 0;
  u64 gpuVa = 0;
};

struct Ring {
  std::vector<Chunk> chunks[kNumFrames];
  u64 frame_bytes[kNumFrames] = {};
};

Ring &ring() {
  static Ring r;
  return r;
}

bool MakeChunk(Chunk &chunk, u64 size) {
  auto &s = state();
  // A 1 MB guard tail that is never handed out: a vertex fetch past the end
  // of a draw's data near the end of a chunk stays inside the buffer (Metal
  // reported "Invalid Resource" and lost the device, e.g. in Lucian's castle
  // and with fable2_geometry_caches off).
  plume::RenderBufferDesc desc = plume::RenderBufferDesc::UploadBuffer(size + (1ull << 20));
  desc.flags = plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::INDEX |
               plume::RenderBufferFlag::CONSTANT;
  chunk.buffer = CreateHostBuffer(s.device.get(), desc, "upload-ring");
  if (!chunk.buffer)
    return false;
  chunk.cpu = static_cast<u8 *>(chunk.buffer->map());
  if (!chunk.cpu) {
    EOT_ERROR("upload ring: map() failed for a {} byte chunk", size);
    chunk.buffer.reset();
    return false;
  }
  chunk.capacity = size;
  chunk.used = 0;
#if defined(EOT_D3D12)
  chunk.gpuVa = static_cast<plume::D3D12Buffer *>(chunk.buffer.get())->d3d->GetGPUVirtualAddress();
#else
  chunk.gpuVa = chunk.buffer->getDeviceAddress();
  if (!chunk.gpuVa) {
    EOT_ERROR("upload ring: the device gave the chunk no buffer device address (VK_KHR_buffer_device_address)");
    chunk.buffer->unmap();
    chunk.buffer.reset();
    return false;
  }
#endif
  return true;
}

}

bool UploadRingInit() {
  auto &r = ring();
  for (u32 i = 0; i < kNumFrames; ++i) {
    r.chunks[i].clear();
    Chunk c;
    if (!MakeChunk(c, kChunkSize))
      return false;
    r.chunks[i].push_back(std::move(c));
  }
  return true;
}

static std::mutex g_ring_mutex;
static std::atomic<u64> g_ring_epoch{0};
u64 UploadRingEpoch() { return g_ring_epoch.load(std::memory_order_acquire); }

void UploadRingResetFrame(u32 slot) {
  std::lock_guard lock(g_ring_mutex);
  auto &r = ring();
  g_ring_epoch.fetch_add(1, std::memory_order_acq_rel);
  auto &chunks = r.chunks[slot];
  for (size_t i = 0; i < chunks.size(); ++i)
    chunks[i].used = 0;
  // Chunks over the usual 4 (a spike, e.g. the first frame in Lucian's
  // castle) go to the buffer graveyard instead of being destroyed here: the
  // other frame slot's command buffer may still be on the GPU, and MoltenVK
  // makes every device-addressable buffer resident for each command buffer,
  // so destroying them now was an "Invalid Resource" GPU fault (device lost).
  while (chunks.size() > 4) {
    chunks.back().buffer->unmap();
    ParkBuffer(state(), std::move(chunks.back().buffer));
    chunks.pop_back();
  }
  if (r.frame_bytes[slot] > 64ull << 20) {
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1) < 40)
      EOT_INFO("[upload-ring] slot {} frame used {:.1f} MB in {} chunks", slot, r.frame_bytes[slot] / 1048576.0,
               chunks.size());
  }
  r.frame_bytes[slot] = 0;
}

bool UploadAllocate(u64 size, u64 alignment, UploadAlloc *out) {
  *out = UploadAlloc{};
  if (size == 0)
    return false;
  auto &s = state();
  auto &r = ring();
  if (alignment == 0)
    alignment = 1;
  std::lock_guard lock(g_ring_mutex);
  const u32 slot = s.recording_slot();
  auto &chunks = r.chunks[slot];
  for (u32 attempt = 0; attempt < 2; ++attempt) {
    for (auto &c : chunks) {
      const u64 start = (c.used + alignment - 1) / alignment * alignment;
      if (start + size <= c.capacity) {
        out->buffer = c.buffer.get();
        out->offset = start;
        out->cpu = c.cpu + start;
        out->size = size;
        out->gpuVa = c.gpuVa ? c.gpuVa + start : 0;
        c.used = start + size;
        r.frame_bytes[slot] += size;
        return true;
      }
    }
    Chunk c;
    const u64 want = size + alignment > kChunkSize ? size + alignment : kChunkSize;
    if (!MakeChunk(c, want))
      return false;
    chunks.push_back(std::move(c));
  }
  return false;
}

bool UploadBytes(const void *src, u64 size, u64 alignment, UploadAlloc *out) {
  if (!UploadAllocate(size, alignment, out))
    return false;
  std::memcpy(out->cpu, src, size);
  return true;
}

static u64 g_ring_peak[kNumFrames];
u64 UploadRingBytesThisFrame() {
  auto &s = state();
  return ring().frame_bytes[s.recording_slot()];
}

u64 UploadRingCapacityBytes(u32 *chunks) {
  std::lock_guard lock(g_ring_mutex);
  u64 bytes = 0;
  u32 n = 0;
  for (const auto &slot : ring().chunks) {
    for (const Chunk &c : slot) {
      bytes += c.capacity;
      n++;
    }
  }
  if (chunks)
    *chunks = n;
  return bytes;
}

}

namespace eot::gpu {

u32 AllocateDescriptorSlot(VideoState &s) {
  for (size_t i = kNullTextureDescriptorCount; i < s.descriptor_slot_used.size(); ++i) {
    if (!s.descriptor_slot_used[i]) {
      s.descriptor_slot_used[i] = true;
      return static_cast<u32>(i);
    }
  }
  return kInvalidDescriptorIndex;
}

void DrainDescriptorSlotsLocked(VideoState &s, u32 slot) {
  for (const auto &d : s.descriptor_graveyard[slot]) {
    if (s.texture_descriptor_set) {
      s.texture_descriptor_set->setTexture(d.slot, s.null_textures[d.null_index].get(),
                                           plume::RenderTextureLayout::SHADER_READ,
                                           s.null_texture_views[d.null_index].get());
    }
    if (d.slot >= kNullTextureDescriptorCount && d.slot < s.descriptor_slot_used.size())
      s.descriptor_slot_used[d.slot] = false;
  }
  s.descriptor_graveyard[slot].clear();
}

namespace {

u32 NullIndexFor(const HostTexture &host) {
  if (host.viewDimension == plume::RenderTextureViewDimension::TEXTURE_3D)
    return kNullTexture3DDescriptorIndex;
  if (host.viewDimension == plume::RenderTextureViewDimension::TEXTURE_CUBE)
    return kNullTextureCubeDescriptorIndex;
  return kNullTexture2DDescriptorIndex;
}

plume::RenderTextureViewDesc SamplingViewDesc(const HostTexture &host) {
  plume::RenderTextureViewDesc view_desc;
  view_desc.format = SampledViewFormat(host.format);
  view_desc.dimension = host.viewDimension;
  view_desc.mipLevels = host.mipLevels ? host.mipLevels : 1;
  return view_desc;
}

plume::RenderSwizzle ToHostSwizzle(u32 source) {
  switch (source & 7) {
  case 0:
    return plume::RenderSwizzle::R;
  case 1:
    return plume::RenderSwizzle::G;
  case 2:
    return plume::RenderSwizzle::B;
  case 3:
    return plume::RenderSwizzle::A;
  case 5:
    return plume::RenderSwizzle::ONE;
  default:
    return plume::RenderSwizzle::ZERO;
  }
}

u32 PublishView(VideoState &s, HostTexture &host, plume::RenderTextureView *view) {
  const u32 slot = AllocateDescriptorSlot(s);
  if (slot == kInvalidDescriptorIndex) {
    EOT_ERROR("bindless texture heap full at {} slots", kBindlessTextureCount);
    return kInvalidDescriptorIndex;
  }
  s.texture_descriptor_set->setTexture(slot, host.texture.get(),
                                       plume::RenderTextureLayout::SHADER_READ, view);
  return slot;
}

}

u32 BindStencilSRVLocked(VideoState &s, HostTexture &host) {
  if (!host.texture || !host.isDepth || !s.texture_descriptor_set)
    return kInvalidDescriptorIndex;
  if (host.stencilDescriptorIndex != kInvalidDescriptorIndex)
    return host.stencilDescriptorIndex;
#if defined(EOT_D3D12)
  if (host.format != plume::RenderFormat::D32_FLOAT_S8_UINT)
    return kInvalidDescriptorIndex;
  const u32 slot = AllocateDescriptorSlot(s);
  if (slot == kInvalidDescriptorIndex) {
    EOT_ERROR("bindless texture heap full at {} slots", kBindlessTextureCount);
    return kInvalidDescriptorIndex;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (host.sampleCount > 1) {
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  } else {
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipLevels = 1;
    desc.Texture2D.PlaneSlice = 1;
  }
  auto *set = static_cast<plume::D3D12DescriptorSet *>(s.texture_descriptor_set.get());
  auto *texture = static_cast<plume::D3D12Texture *>(host.texture.get());
  set->setSRV(slot, texture->d3d, &desc);
  s.perf.host_views++;
  host.stencilDescriptorIndex = slot;
  return slot;
#else
  return kInvalidDescriptorIndex;
#endif
}

void ReleaseTextureSRVLocked(VideoState &s, HostTexture &host) {
  const u32 null_index = NullIndexFor(host);
  s.texture_generation.fetch_add(1, std::memory_order_relaxed);
  if (host.descriptorIndex != kInvalidDescriptorIndex) {
    s.descriptor_graveyard[s.recording_slot()].push_back({host.descriptorIndex, null_index});
    host.descriptorIndex = kInvalidDescriptorIndex;
  }
  if (host.stencilDescriptorIndex != kInvalidDescriptorIndex) {
    s.descriptor_graveyard[s.recording_slot()].push_back({host.stencilDescriptorIndex, null_index});
    host.stencilDescriptorIndex = kInvalidDescriptorIndex;
  }
  for (auto &[swizzle, srv] : host.swizzledSrvs) {
    if (srv.descriptorIndex != kInvalidDescriptorIndex)
      s.descriptor_graveyard[s.recording_slot()].push_back({srv.descriptorIndex, null_index});
    if (srv.view)
      ParkView(s, std::move(srv.view));
  }
  host.swizzledSrvs.clear();
}

u32 BindTextureSRVLocked(VideoState &s, HostTexture &host) {
  if (!host.texture || !s.texture_descriptor_set)
    return kInvalidDescriptorIndex;
  if (host.descriptorIndex != kInvalidDescriptorIndex)
    return host.descriptorIndex;
  if (!host.srv) {
    s.perf.host_views++;
    host.srv = host.texture->createTextureView(SamplingViewDesc(host));
  }
  if (!host.srv)
    return kInvalidDescriptorIndex;
  host.descriptorIndex = PublishView(s, host, host.srv.get());
  return host.descriptorIndex;
}

u32 BindTextureSRVSwizzledLocked(VideoState &s, HostTexture &host, u32 swizzle) {
  swizzle &= 0xFFF;
  if (swizzle == kIdentityFetchSwizzle)
    return BindTextureSRVLocked(s, host);
  if (!host.texture || !s.texture_descriptor_set)
    return kInvalidDescriptorIndex;
  auto &entry = host.swizzledSrvs[swizzle];
  if (entry.descriptorIndex != kInvalidDescriptorIndex)
    return entry.descriptorIndex;
  if (!entry.view) {
    plume::RenderTextureViewDesc view_desc = SamplingViewDesc(host);
    view_desc.componentMapping = plume::RenderComponentMapping(
        ToHostSwizzle(swizzle), ToHostSwizzle(swizzle >> 3), ToHostSwizzle(swizzle >> 6),
        ToHostSwizzle(swizzle >> 9));
    s.perf.host_views++;
    entry.view = host.texture->createTextureView(view_desc);
  }
  if (!entry.view)
    return kInvalidDescriptorIndex;
  entry.descriptorIndex = PublishView(s, host, entry.view.get());
  return entry.descriptorIndex;
}

}
