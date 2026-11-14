// gpu/render_thread.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <mutex>

#include <rex/types.h>
#include <cstring>
#include <plume_render_interface.h>

#include "gpu/d3d.h"

namespace eot::gpu {

struct VideoState;
constexpr u64 kTexturePlacementAlignment = 512;
constexpr u64 kTextureRowPitchAlignment = 256;
constexpr u64 kConstantBufferAlignment = 256;

struct UploadAlloc {
  plume::RenderBuffer *buffer = nullptr;
  u64 offset = 0;
  u8 *cpu = nullptr;
  u64 size = 0;
  u64 gpuVa = 0;
  explicit operator bool() const { return buffer != nullptr; }
};

bool UploadRingInit();
// Upload ring: frames as the game's thread captures them (see frame_ring.cpp).
// 2: with 3, occlusion query results flickered while standing still (cause not found;
// 2026-11-13), and 2 also suffice for the game's thread running one frame ahead.
constexpr u32 kUploadRingSlots = 2;
void UploadRingNextCapturedFrame();  // game's thread, at its Present
void UploadRingFramePresented();     // render thread, PresentLocked
void UploadRingSlotSubmitted(u32 command_slot);
void UploadRingSlotFinished(u32 command_slot);
u32 UploadRingSlot();
void UploadRingParkExcess(VideoState &s);  // render thread
u64 UploadRingEpoch();

bool UploadAllocate(u64 size, u64 alignment, UploadAlloc *out);
bool UploadBytes(const void *src, u64 size, u64 alignment, UploadAlloc *out);

u64 UploadRingBytesThisFrame();
u64 UploadRingCapacityBytes(u32 *chunks = nullptr);

}

namespace eot::gpu {

struct GuestShader;
struct InputLayout;

struct CachedIndexRange {
  plume::RenderBuffer *buffer = nullptr;
  u64 offset = 0;
  u32 count = 0;
  u32 lo = 0, hi = 0;
  bool is32 = false;
  plume::RenderPrimitiveTopology topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  u64 lastUseFrame = 0;
};

struct TargetWords {
  u32 colorVa[4] = {};
  u32 colorWords[4][5] = {};
  u32 colorInfo[4] = {};
  u32 colorCount = 0;
  u32 depthVa = 0;
  u32 depthWords[5] = {};
};

struct DeviceWindow {
  struct Block {
    u32 base, size;
  };
  static constexpr Block kBlocks[4] = {
      {dev::kFetchConstants, 20u * 24u},  // texture fetch constants 0..19 (16..19: vertex shader)
      {dev::kVsBoolConstants, dev::kPsLoopConstants + 64 - dev::kVsBoolConstants},
      {dev::kSurfaceInfo, dev::kPolyOffsetBackOffset + 4 - dev::kSurfaceInfo},
      {dev::kTextureObject0, 16u * 4u},
  };
  struct Pages {
    bool in[kDeviceSnapshotBytes >> kDevicePageShift] = {};
    constexpr Pages() {
      for (const Block &b : kBlocks)
        for (u32 p = b.base >> kDevicePageShift; p < (b.base + b.size) >> kDevicePageShift; ++p)
          in[p] = true;
    }
  };
  static const bool *PageTable() {
    static constexpr Pages pages{};
    return pages.in;
  }
  alignas(64) u8 image[kDeviceSnapshotBytes];
  template <u32 I> void CopyBlock(const u8 *regs) {
    std::memcpy(image + kBlocks[I].base, regs + kBlocks[I].base, kBlocks[I].size);
  }
  void Capture(const u8 *regs) {
    CopyBlock<0>(regs);
    CopyBlock<1>(regs);
    CopyBlock<2>(regs);
    CopyBlock<3>(regs);
  }
};
static_assert((DeviceWindow::kBlocks[0].base | DeviceWindow::kBlocks[0].size |
               DeviceWindow::kBlocks[1].base | DeviceWindow::kBlocks[1].size |
               DeviceWindow::kBlocks[2].base | DeviceWindow::kBlocks[2].size |
               DeviceWindow::kBlocks[3].base | DeviceWindow::kBlocks[3].size) % 32 == 0);
static_assert(DeviceWindow::kBlocks[3].base + DeviceWindow::kBlocks[3].size <= kDeviceSnapshotBytes);

struct DrawPacket {
  u32 device_va = 0;
  u32 prim = 0;
  bool indexed = false;
  bool rectList = false;
  plume::RenderPrimitiveTopology topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  u32 vertexCount = 0;
  GuestShader *vs = nullptr;
  GuestShader *ps = nullptr;
  u32 vs_va = 0, ps_va = 0;
  const InputLayout *layout = nullptr;
  TargetWords targets;
  bool hasCached = false;
  CachedIndexRange cached;
  UploadAlloc index_alloc;
  u32 index_count = 0;
  i32 host_base_vertex = 0;
  // Host vertex ID minus this is the guest vertex index (r0.x in the shader).
  i32 vertex_id_bias = 0;
  u32 tess_segments = 0, tess_first_patch = 0;  // tessellated patch draw
  // Vertex pulling table (SharedConstants::vertexPullTable), uploaded at capture.
  u64 vertex_pull_table = 0;
  u32 max_slot = 0;
  u32 strides[16] = {};
  plume::RenderVertexBufferView views[16];
  plume::RenderInputSlot slots[16];
  UploadAlloc zero;
  UploadAlloc vs_consts, ps_consts;
  DeviceWindow window;
  bool hasCameraVP = false;
  float cameraVP[16] = {};
  bool taaSkip = false;
  bool velocity = false;
  // The pixel textures' headers when the draw was recorded (bit n of the mask:
  // slot n recorded); the replay looks the textures up by these.
  u32 texHeaderMask = 0;
  u32 texHeaders[16][6] = {};
};

struct ClearPacket {
  u32 device_va = 0;
  u32 flags = 0;
  bool hasRect = false;
  i32 rect[4] = {};
  float rgba[4] = {};
  float z = 0.0f;
  u32 stencil = 0;
  TargetWords targets;
};

struct ResolvePacket {
  u32 device_va = 0;
  u32 flags = 0;
  u32 srcVa = 0;
  u32 srcWords[5] = {};
  u32 destVa = 0;
  u32 destFetch[6] = {};  // the destination's header when the resolve was recorded
  bool hasDestFetch = false;
  u32 destLevel = 0;
  u32 autoMipLevels = 0;  // > 1: build levels 1.. from level 0 after this resolve (fable2_bake_auto_mips)
  bool hasRect = false;
  i32 rect[4] = {};
  bool hasPoint = false;
  i32 point[2] = {};
  bool hasColor = false;
  float rgba[4] = {};
  float clearZ = 0.0f;
  u32 dsVa = 0;
  u32 dsWords[5] = {};
  bool refresh = false;
  bool clearDestOnly = false;
};

}

namespace plume {
struct RenderBuffer;
}

namespace eot::gpu {

enum class RenderCommandType : u32 {
  Draw,
  Clear,
  Resolve,
  Present,
  Unlock,
  Retire,
  QueryBegin, // Fable 2: occlusion queries (gpu/queries.cpp)
  QueryEnd,
};

struct RenderCommand {
  RenderCommandType type = RenderCommandType::Draw;
  u64 seq = 0;
  u32 va = 0;
  u32 aux = 0;
  plume::RenderBuffer *buffer = nullptr;
  ClearPacket clear;
  ResolvePacket resolve;
  DrawPacket draw;
};

void RenderThreadStart();
bool RenderThreadActive();
// The last command committed (call from the producing thread) and the last
// command whose completion the render thread has published (presents and
// resolves publish).
u64 RenderThreadLastCommitted();
u64 RenderThreadExecuted();
void RenderThreadStop();

class RenderEnqueue {
public:
  RenderEnqueue();
  ~RenderEnqueue();
  RenderEnqueue(const RenderEnqueue &) = delete;
  RenderEnqueue &operator=(const RenderEnqueue &) = delete;
  RenderCommand &cmd() { return *cmd_; }
  u64 commit(bool publish = true);

private:
  std::unique_lock<std::mutex> lock_;
  RenderCommand *cmd_ = nullptr;
  bool committed_ = false;
};

void RenderThreadWait(u64 seq);
// Called by PresentLocked once the frame is submitted (render thread).
void RenderThreadPresentSubmitted();

void RenderThreadUnlock(u32 resource_va);
void RenderThreadRetire(plume::RenderBuffer *buffer);

}
