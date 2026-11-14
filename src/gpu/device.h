// gpu/device.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <cstddef>

#include <atomic>
#include <functional>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/types.h>

#include <plume_render_interface.h>

#include "core/cpu.h"
#include "gpu/resources.h"

namespace rex::ui {
class Window;
}

namespace eot::gpu {

constexpr u32 kNumFrames = 2;

constexpr u32 kBindlessTextureCount = 32768;
constexpr u32 kBindlessSamplerCount = 512;

constexpr u32 kSamplerLinearClamp = 0;
constexpr u32 kSamplerPointClamp = 1;
constexpr u32 kReservedSamplerCount = 2;

struct CopyPushConstants {
  u32 resourceDescriptorIndex = 0;
  u32 resourceDescriptorIndex2 = 0;
  float param0 = 1.0f;
  float param1 = 0.0f;
  float rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  float colorAdjust[4] = {0.0f, 1.0f, 1.0f, 0.0f};
  float extra[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(CopyPushConstants) == 64);

struct PerfCounters {
  f64 draw_ms = 0, resolve_ms = 0, upload_ms = 0, link_ms = 0, pso_ms = 0;
  f64 vertex_copy_ms = 0, index_ms = 0, bind_ms = 0;
  f64 setup_ms = 0, pso_lookup_ms = 0, stream_ms = 0, const_ms = 0, record_ms = 0;
  f64 const_float_ms = 0, rec_state_ms = 0, rec_bind_ms = 0;
  f64 guest_d3d_ms = 0;
  u32 guest_d3d_calls = 0;
  f64 capture_ms = 0, present_wait_ms = 0, worker_idle_ms = 0;
  f64 game_thread_ms = 0;  // CPU time of the game's rendering thread (Present to Present)
  u32 index_cache_hits = 0, index_cache_misses = 0;
  u32 index_cache_evictions = 0;
  u32 vertex_cache_hits = 0, vertex_cache_misses = 0;
  u32 geometry_vram_binds = 0, geometry_staging_binds = 0;
  u32 const_file_hits = 0;
  u32 const_file_clean_hits = 0;
  u32 const_file_mask_misses = 0;
  u32 pipeline_hot_hits = 0;
  u32 replay_memo_hits = 0;
  u32 vertex_bind_requests = 0, vertex_bind_calls = 0;
  u32 single_stream_draws = 0, target_memo_hits = 0;
  u32 sorted_draws = 0, sort_runs = 0;
  u32 target_memo_miss_gen = 0, target_memo_miss_words = 0, target_memo_miss_sig = 0;
  u32 index_bind_requests = 0, index_bind_calls = 0;
  u32 framebuffer_cache_hits = 0;
  u32 texture_bind_requests = 0, texture_bind_hits = 0;
  u32 pipeline_bind_calls = 0, viewport_bind_calls = 0, scissor_bind_calls = 0;
  u32 pipeline_bind_on_hit = 0;
  u32 stencil_ref_calls = 0;
  u32 texture_barrier_calls = 0, texture_barrier_resources = 0;
  f64 acquire_ms = 0, submit_ms = 0, fence_ms = 0, frame_ms = 0;
  f64 present_blit_ms = 0, present_house_ms = 0;
  f64 replay_targets_ms = 0;
  f64 hero_resolve_wait_ms = 0;  // game thread waiting for hero/dog texture resolves
  u32 hero_resolve_waits = 0;
  u32 chunk_submits = 0;
  u32 resolves_skipped_debug = 0;
  u32 auto_mip_skips = 0;  // game mip draws + resolves replaced by fable2_bake_auto_mips
  f64 pace_ms = 0;
  u32 draws = 0, resolves = 0, uploads = 0, links = 0, psos = 0, frames = 0;
  u32 uploads_new = 0, uploads_again = 0, uploads_refresh = 0;
  u32 uploads_again_reloaded = 0;
  u64 upload_blocks = 0;
  f64 upload_synth_ms = 0;
  u32 upload_frame_max = 0;
  f64 upload_frame_max_ms = 0;
  u32 upload_lead[5] = {};
  u32 textures_released = 0;
  u32 uploads_preloaded = 0;
  u32 uploads_skipped = 0;
  u32 resolve_copies = 0;
  u32 draws_skipped = 0;
  u32 surface_transfers = 0;
  u32 resolve_transfers = 0;
  u32 resolve_noops = 0;
  u32 resolve_refreshes = 0;
  u32 alias_deferred = 0;
  u32 alias_copies = 0;
  u32 host_textures = 0, host_views = 0, host_framebuffers = 0, host_parked = 0;
  u32 retired_flushes = 0;
  u32 host_tex_surface = 0, host_tex_mirror = 0, host_tex_guest = 0;
  u32 host_tex_recycled = 0;
  u32 live_textures = 0, live_surfaces = 0, alias_scanned = 0;
  u32 textures_evicted = 0, pool_size = 0;
  f64 alias_scan_ms = 0, msaa_scan_ms = 0;
  f64 resolve_mirror_ms = 0, resolve_fb_ms = 0, resolve_bind_ms = 0;
  u64 vertex_bytes = 0, index_bytes = 0, constant_bytes = 0;
  u64 pull_bytes = 0, pull_draws = 0, pull_mirrored_bytes = 0;  // vertex pulling: guest buffers copied per draw
  u32 dead_resolves = 0;
  f64 gpu_ms = 0;
  u32 gpu_frames = 0;
  std::map<u32, std::pair<f64, u32>> gpu_cats;
  std::chrono::steady_clock::time_point last_present{};
};

constexpr u32 kFrameClockRing = 8192;

inline u64 PerfNow() { return eot::cpu::Timestamp(); }
inline f64 g_perf_ms_per_tick = 0.0;
f64 PerfMsPerTickSlow();
bool PinThreadToPhysicalCore(u32 core, const char *what);
bool ThreadSleepsPrecisely();
u32 PhysicalCoreCount();
inline f64 PerfMsPerTick() {
  const f64 v = g_perf_ms_per_tick;
  return v > 0.0 ? v : PerfMsPerTickSlow();
}
struct PerfScopeSampled {
  f64 &acc;
  u64 t0 = 0;
  PerfScopeSampled(f64 &a, u32 &calls) : acc(a) {
    if ((++calls & 15u) == 0)
      t0 = PerfNow();
  }
  ~PerfScopeSampled() {
    if (t0)
      acc += 16.0 * static_cast<f64>(PerfNow() - t0) * PerfMsPerTick();
  }
};

struct PerfScope {
  f64 &acc;
  u64 t0;
  bool stopped = false;
  void stop() {
    if (stopped)
      return;
    acc += static_cast<f64>(PerfNow() - t0) * PerfMsPerTick();
    stopped = true;
  }
  explicit PerfScope(f64 &a) : acc(a), t0(PerfNow()) {}
  ~PerfScope() {
    if (!stopped)
      acc += static_cast<f64>(PerfNow() - t0) * PerfMsPerTick();
  }
};

struct SharedConstants {
  u32 texture2DIndices[16]{};   // c0..c3
  u32 texture3DIndices[16]{};   // c4..c7
  u32 textureCubeIndices[16]{}; // c8..c11
  u32 texture1DIndices[16]{};   // c12..c15
  u32 samplerIndices[16]{};     // c16..c19
  u32 booleans[8]{};            // c20..c21  VS bits 0..127, PS bits 128..255
  u32 swappedTexcoords{};       // c22.x
  float halfPixelOffsetX{};     // c22.y
  float halfPixelOffsetY{};     // c22.z
  float alphaThreshold{};       // c22.w
  u32 swappedNormals{};         // c23.x
  u32 swappedBinormals{};       // c23.y
  u32 swappedTangents{};        // c23.z
  u32 swappedBlendWeights{};    // c23.w
  u32 swappedPositions{};       // c24.x
  u32 sintTexcoords{};
  u32 biasedTextures{};
  u32 packedDec3{};
  i32 loopConstants[32][4]{};   // c25..c56  int4(count, start, step, 0)
  float posScale[4]{1.f, 1.f, 1.f, 1.f};  // c57
  float posOffset[4]{0.f, 0.f, 0.f, 0.f}; // c58
  // Fable 2: vertex shader samplers 16..19 (XenosRecomp patch, byte 944 on).
  // Null descriptors until vertex texture fetch is bound; reading past the
  // block gave garbage descriptor indices and GPU page faults.
  u32 vsTexture2DIndices[4]{kNullTexture2DDescriptorIndex, kNullTexture2DDescriptorIndex,
                            kNullTexture2DDescriptorIndex, kNullTexture2DDescriptorIndex};
  u32 vsTexture3DIndices[4]{kNullTexture3DDescriptorIndex, kNullTexture3DDescriptorIndex,
                            kNullTexture3DDescriptorIndex, kNullTexture3DDescriptorIndex};
  u32 vsTextureCubeIndices[4]{kNullTextureCubeDescriptorIndex, kNullTextureCubeDescriptorIndex,
                              kNullTextureCubeDescriptorIndex, kNullTextureCubeDescriptorIndex};
  u32 vsTexture1DIndices[4]{kNullTexture2DDescriptorIndex, kNullTexture2DDescriptorIndex,
                            kNullTexture2DDescriptorIndex, kNullTexture2DDescriptorIndex};
  u32 vsSamplerIndices[4]{kSamplerLinearClamp, kSamplerLinearClamp, kSamplerLinearClamp,
                          kSamplerLinearClamp};
  // Fable 2: vertex pulling table (byte 1024): 96 entries of { u64 address,
  // u32 size in dwords, u32 endian }, read only by shaders that pull.
  u64 vertexPullTable{};
  i32 hostBaseVertex{};  // byte 1032: guest vertex index = host vertex ID - this
  float pixelPosScale{1.0f};  // byte 1036: guest pixel position = host / render scale
  // Point sprites (byte 1040): diameter scale (render scale), the device's
  // constant diameter, and whether this draw is a point list.
  float pointSizeScale{1.0f};
  float pointSizeConstant{1.0f};
  u32 isPointDraw{};
  float tileOffsetKeep{1.0f};  // byte 1052: g_TileOffsetSize.xy factor (0 = untiled host)
  // byte 1056: host width / guest width of each pixel-shader texture (0 = 1);
  // fetches with texel coordinates divide by the guest size.
  float texHostScale[16]{};
  // byte 1120: tessellated patch draws (XenosRecomp xeVertexRegister): grid
  // segments per patch edge (0 = not tessellated) and the first patch index.
  u32 tessSegments{};
  u32 tessFirstPatch{};
  u64 debugAddress{};  // byte 1128: debug readback of tessellated vertices (0 = off)
  u32 vsBiasedTextures{};  // byte 1136: vertex samplers 16..19 with biased (2x - 1) fetches
  u32 alphaToMask{};  // byte 1140: bit 31 = Xenos alpha to mask, bits 0..7 = its quad dither offsets
  u32 pullFlags{};  // byte 1144: 1 = vertex pulling reads 16-bit pairs in memory order
  u32 pad1148{};
  float pointViewport[4]{};  // byte 1152: host viewport (x, y, w, h) for point sprite coordinates
  // byte 1168: each pixel sampler's state with point / linear MinFilter and
  // MagFilter, for fetches whose instruction overrides the sampler's filters.
  u32 samplerPointIndices[16]{};
  u32 samplerLinearIndices[16]{};
};
static_assert(sizeof(SharedConstants) == 64 * 16 + 32 + 64 + 48 + 128);
static_assert(offsetof(SharedConstants, samplerPointIndices) == 1168);
static_assert(offsetof(SharedConstants, samplerLinearIndices) == 1232);
static_assert(offsetof(SharedConstants, pointViewport) == 1152);
static_assert(offsetof(SharedConstants, tessSegments) == 1120);
static_assert(offsetof(SharedConstants, vertexPullTable) == 1024);
static_assert(offsetof(SharedConstants, vsTexture2DIndices) == 944);

struct HostTextureTransition {
  HostTexture *host = nullptr;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
};
constexpr u32 kMaxPendingTransitions = 24;

struct VideoState {
  std::unique_ptr<plume::RenderInterface> render_iface;
  std::unique_ptr<plume::RenderDevice> device;
  std::unique_ptr<plume::RenderPool> transient_mirror_pool;
  std::unique_ptr<plume::RenderCommandQueue> queue;
  std::unique_ptr<plume::RenderCommandList> command_lists[kNumFrames];
  std::unique_ptr<plume::RenderCommandFence> fences[kNumFrames];
  std::unique_ptr<plume::RenderCommandSemaphore> acquire_semaphores[kNumFrames];
  std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> render_semaphores;
  plume::RenderCommandList *command_list = nullptr;
  std::atomic<u32> frame{0};
  u32 next_frame = 1 % kNumFrames;
  u32 recording_slot() const { return frame.load(std::memory_order_relaxed); }

  std::unique_ptr<plume::RenderSwapChain> swap_chain;
  bool present_wait = false;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> swap_framebuffers;

  std::unique_ptr<plume::RenderPipelineLayout> pipeline_layout;
  std::unique_ptr<plume::RenderDescriptorSet> texture_descriptor_set;
  std::vector<bool> descriptor_slot_used;
  std::unique_ptr<plume::RenderTexture> null_textures[kNullTextureDescriptorCount];
  std::unique_ptr<plume::RenderTextureView> null_texture_views[kNullTextureDescriptorCount];
  bool null_texture_barriers_submitted = false;
  std::unique_ptr<plume::RenderDescriptorSet> sampler_descriptor_set;
  std::vector<bool> sampler_descriptor_used;
  std::unique_ptr<plume::RenderSampler> linear_sampler;
  std::unique_ptr<plume::RenderSampler> point_sampler;

  std::unique_ptr<plume::RenderShader> copy_vs;
  std::unique_ptr<plume::RenderShader> blit_ps;
  std::unique_ptr<plume::RenderShader> copy_depth_ps;
  std::unordered_map<u64, std::unique_ptr<plume::RenderPipeline>> blit_pipelines;
  std::unordered_map<u64, std::unique_ptr<plume::RenderPipeline>> depth_copy_pipelines;
  u32 host_msaa_samples = 1;
  u32 display_refresh_hz = 60;
  std::unique_ptr<plume::RenderShader> resolve_msaa_color_ps[3];
  std::unique_ptr<plume::RenderShader> resolve_msaa_depth_ps[3];
  std::unordered_map<u64, std::unique_ptr<plume::RenderPipeline>> resolve_msaa_pipelines;
  std::unique_ptr<plume::RenderShader> derive_depth_stencil_ps[3];
  std::unordered_map<u64, std::unique_ptr<plume::RenderPipeline>> derive_depth_stencil_pipelines;
  bool stencil_ref_supported = false;

  std::string backend_info;

  std::mutex mutex;
  std::atomic<bool> ready{false};
  std::atomic<bool> shutting_down{false};
  bool quiesced = false;
  std::atomic<bool> resize_requested{false};

  bool command_list_open = false;
  // Predicated tiling resolves clear their tile of the source: while a resolve
  // runs, the next queued command if it is a resolve too (render thread), and
  // the source whose first tile clear was folded into the next tile's.
  const struct ResolvePacket *next_resolve = nullptr;
  u32 tile_clear_deferred_va = 0;
  bool command_list_submitted[kNumFrames] = {};
  // Chunked submits (fable2_chunk_submit_draws): command lists of a frame slot
  // submitted mid-frame (still on the GPU until the slot's fence) and spares.
  std::vector<std::unique_ptr<plume::RenderCommandList>> chunk_inflight[kNumFrames];
  std::vector<std::unique_ptr<plume::RenderCommandList>> chunk_spare[kNumFrames];
  u32 list_draws = 0;       // draws recorded into the open command list
  u32 frame_chunks = 0;     // chunk submits in the frame being recorded
  HostTextureTransition pending_transitions[kMaxPendingTransitions];
  u32 pending_transition_count = 0;
  bool defer_shader_read_transitions = false;

  u64 presented_frames = 0;
  // Window title: the largest colour target drawn since the last present (the
  // game's render resolution) and its host scale.
  u32 frame_target_w = 0, frame_target_h = 0;
  f32 frame_target_scale = 1.0f;
  u64 guest_frames = 0;
  // Xbox 360 Z-pass emulation: the draw being replayed fills depth only.
  bool depth_prerun = false;
  // fable2_scene_keeps_zpass_depth: the scene draws on the emulated Z pass's
  // depth (from it until the scene depth resolve), this surface.
  bool scene_keeps_zpass_depth = false;
  u32 scene_depth_va = 0;
  u64 captured_presents = 0;

  std::vector<std::unique_ptr<plume::RenderTexture>> texture_graveyard[kNumFrames];
  std::vector<std::unique_ptr<plume::RenderTextureView>> view_graveyard[kNumFrames];
  // Textures and views whose bindless slots were just nulled: destroyed only
  // after the next frame slot's fence, because the frame recorded after their
  // release still bound the shared bindless set with them in it (MoltenVK keeps
  // every texture of a bound set resident for that command buffer).
  std::vector<std::unique_ptr<plume::RenderTexture>> texture_deferred;
  std::vector<std::unique_ptr<plume::RenderTextureView>> view_deferred;
  // Same for buffers: MoltenVK makes every device-addressable buffer resident
  // for each command buffer it encodes, whether that frame uses it or not.
  std::vector<std::unique_ptr<plume::RenderBuffer>> buffer_deferred;
  // MoltenVK commits the Metal command buffers on its own thread
  // (MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0), after vkQueueSubmit and
  // vkQueuePresentKHR return. Destroying a texture or buffer takes it out of
  // the queue's residency set and releases it; a command buffer committed
  // meanwhile still listed it, and Metal failed that whole command buffer
  // ("Invalid Resource", first on the present command buffer: castle device
  // lost). So the objects that are old enough wait here until the queue is
  // known idle: a fence-only submit after each present has signalled with
  // nothing submitted since (fable2_destroy_when_queue_idle).
  std::unique_ptr<plume::RenderCommandFence> tail_fence;
  bool tail_fence_inflight = false; // submitted, not yet seen signalled
  bool tail_fence_is_last = false;  // no queue operation after it
  bool queue_idle = false;          // nothing submitted since the queue drained
  u64 retired_since = 0;            // presented_frames when the lists filled
  std::vector<std::unique_ptr<plume::RenderTexture>> texture_retired;
  std::vector<std::unique_ptr<plume::RenderTextureView>> view_retired;
  std::vector<std::unique_ptr<plume::RenderBuffer>> buffer_retired;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffer_retired;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffer_graveyard[kNumFrames];
  std::vector<std::unique_ptr<plume::RenderBuffer>> buffer_graveyard[kNumFrames];
  struct RetiredDescriptorSlot {
    u32 slot;
    u32 null_index;
  };
  std::vector<RetiredDescriptorSlot> descriptor_graveyard[kNumFrames];

  std::unordered_map<u32, std::shared_ptr<GuestTexture>> textures;

  PerfCounters perf;
  PerfCounters perf_prev_frame;
  std::vector<f32> frame_walls;
  f64 frame_clock[kFrameClockRing] = {};

  struct TextureSlotCache {
    u32 texVa = 0;
    u8 fc[24] = {};
    u64 generation = ~0ull;
    u64 resourceGeneration = ~0ull;
    u32 samplerPolicy = ~0u;
    GuestTexture *texture = nullptr;
    u32 index = kInvalidDescriptorIndex;
    u32 sampler = 0;
    u32 samplerPoint = 0;
    u32 samplerLinear = 0;
    u8 dimension = 0;
    u8 biasedBits = 0;
  };
  static constexpr u32 kTextureSlotWays = 4;
  TextureSlotCache slot_cache[16][kTextureSlotWays];
  u8 slot_cache_next[16] = {};
  GuestTexture *draw_bound_textures[16] = {};
  std::atomic<u64> texture_generation{1};
  std::unordered_map<u64, std::unique_ptr<GuestSurface>> surfaces;
  struct SurfaceWorkStats {
    bool isDepth = false;
    u32 format = 0, baseTile = 0;
    u32 minWidth = 0, maxWidth = 0, minHeight = 0, maxHeight = 0;
    u32 allocWidth = 0, allocHeight = 0, hostWidth = 0, hostHeight = 0;
    u32 samples = 1;
    u64 draws = 0, clears = 0, resolves = 0;
  };
  struct TextureWorkStats {
    u32 va = 0, width = 0, height = 0, hostWidth = 0, hostHeight = 0, format = 0;
    u64 samples = 0, resolves = 0, deadResolves = 0;
  };
  std::unordered_map<u64, SurfaceWorkStats> surface_work_window;
  std::unordered_map<u64, TextureWorkStats> texture_work_window;
  std::unordered_map<u64, u64> render_area_window;
  u64 resolve_alias_token = 0;
  std::vector<GuestTexture *> resolve_alias_candidates;
  std::map<u32, u32> resolve_ranges;
  u64 resolve_alias_candidates_generation = 0;
  u64 mirror_generation = 1;
  u32 root_cbv_index[3] = {~0u, ~0u, ~0u};
  u32 bound_spec = ~0u;
  // Keyed by object address << 1 | is_pixel: the game reuses a freed pixel
  // shader's memory for a vertex shader (and back).
  std::unordered_map<u64, std::unique_ptr<GuestShader>> shaders;
  std::mutex shaders_mutex;
  std::vector<std::unique_ptr<GuestShader>> shader_graveyard;
  std::atomic<u64> shader_generation{1};

  struct CachedFramebuffer {
    std::unique_ptr<plume::RenderFramebuffer> fb;
    const plume::RenderTexture *attachments[5] = {};
    u32 attachmentCount = 0;
  };
  std::unordered_map<u64, CachedFramebuffer> framebuffers;
  struct PooledHostTexture {
    HostTexture host;
    u64 freedFrame = 0;
    u64 bytes = 0;
  };
  std::vector<PooledHostTexture> host_texture_pool;
  u64 host_texture_pool_bytes = 0;
  const plume::RenderFramebuffer *bound_framebuffer = nullptr;
  const plume::RenderPipeline *bound_pipeline = nullptr;
  struct BoundVertexStream {
    const plume::RenderBuffer *buffer = nullptr;
    u64 offset = 0;
    u32 size = 0;
    u32 stride = 0;
    bool valid = false;
  } bound_vertex_streams[16];
  struct BoundIndexStream {
    const plume::RenderBuffer *buffer = nullptr;
    u64 offset = 0;
    u32 size = 0;
    plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
    bool valid = false;
  } bound_index_stream;
  const plume::RenderTexture *bound_draw_colors[4] = {};
  const plume::RenderTexture *bound_draw_depth = nullptr;
  u32 bound_draw_color_count = 0;
  bool bound_draw_targets_valid = false;
  plume::RenderBuffer *bound_root_buffer[3] = {};
  u64 bound_root_offset[3] = {};
  u64 perf_resets = 0;
  u64 vs_float_constants_stale = ~0ull;
  u64 ps_float_constants_stale = ~0ull;

  enum class GammaMode : u32 { None = 0, Table = 1, Pwl = 2 };
  GammaMode gamma_mode = GammaMode::None;
  uint16_t gamma_table[3][256] = {};  // D3DGAMMARAMP: red/green/blue, 16-bit
  uint16_t gamma_pwl[3][128][2] = {}; // D3DPWLGAMMA: {base, delta} per channel
  bool gamma_lut_dirty = false;
  HostTexture gamma_lut;
  u32 last_front_buffer_va = 0;

  std::atomic<bool> pending_up_armed{false};
  std::mutex guest_mutex;
  u32 current_vs_va = 0, current_ps_va = 0;
  const char *current_origin = "";
  u64 surface_generation = 0;
  struct PendingUpDraw {
    bool valid = false;
    u32 device_va = 0;
    u32 primitive_type = 0;
    u32 vertex_count = 0;
    u32 stride = 0;
    u32 data_va = 0;
    // BeginIndexedVertices: index data written by the guest after the call.
    u32 index_va = 0;
    u32 index_count = 0;
    bool index32 = false;
    u64 vs_constants_dirty = ~0ull;
    u64 ps_constants_dirty = ~0ull;
    bool has_image = false;
    std::vector<u8> device_image;
  } pending_up;
};

VideoState &state();

class Video {
public:
  static bool CreateHostDevice(rex::ui::Window *window);
  static void RequestShutdown();
  static bool IsShuttingDown();
  static void BeginShutdown();
  static void Shutdown();
  static plume::RenderDevice *HostDevice();
  static u32 OutputWidth();
  static u32 OutputHeight();
  // Window title: frames presented, and the game's render resolution (guest
  // pixels) with the host scale it is drawn at (0 until the first frame).
  static void TitleStats(u64 *frames, u32 *width, u32 *height, f32 *scale);
  static void RequestResize();

  static void Present(u32 front_buffer_texture_va);
  static void PresentOverlayOnly();
};

std::unique_ptr<plume::RenderBuffer> CreateHostBuffer(plume::RenderDevice *device,
                                                      const plume::RenderBufferDesc &desc,
                                                      const char *tag);
std::unique_ptr<plume::RenderTexture> CreateHostTexture(plume::RenderDevice *device,
                                                        const plume::RenderTextureDesc &desc,
                                                        const char *tag,
                                                        plume::RenderPool *pool = nullptr);
std::unique_ptr<plume::RenderPipeline>
CreateHostGraphicsPipeline(plume::RenderDevice *device,
                           const plume::RenderGraphicsPipelineDesc &desc, const char *tag);

bool BuildPipelineLayout(VideoState &s);
bool BuildHelperPipelines(VideoState &s);
bool BuildSwapFramebuffers(VideoState &s);
plume::RenderPipeline *GetBlitPipeline(VideoState &s, plume::RenderFormat rt_format,
                                       u32 samples = 1);
plume::RenderPipeline *GetResolveMsaaPipeline(VideoState &s, plume::RenderFormat dst_format,
                                              u32 src_samples, bool depth);
plume::RenderPipeline *GetDepthCopyPipeline(VideoState &s, plume::RenderFormat ds_format,
                                            u32 samples = 1);
plume::RenderPipeline *GetDeriveDepthStencilPipeline(VideoState &s, plume::RenderFormat ds_format,
                                                     u32 src_samples);
u32 BindStencilSRVLocked(VideoState &s, HostTexture &host);

void BeginCommandList(VideoState &s);
void SubmitOpenListLocked(VideoState &s);
// After a resolve: submits the frame so far when enough draws are recorded.
void MaybeSubmitChunkLocked(VideoState &s);
void AdvanceAndWaitReused(VideoState &s);
// Queue idle tracking for destroying retired objects (see VideoState::tail_fence).
void NoteQueueSubmittedLocked(VideoState &s);
// The fence of the last submission was just waited for: the queue is idle.
void NoteQueueDrainedLocked(VideoState &s);
void SubmitQueueMarkerLocked(VideoState &s);
void FlushRetiredLocked(VideoState &s);
void ParkTexture(VideoState &s, std::unique_ptr<plume::RenderTexture> t);
void ParkView(VideoState &s, std::unique_ptr<plume::RenderTextureView> v);
void ParkFramebuffer(VideoState &s, std::unique_ptr<plume::RenderFramebuffer> f);
void ParkBuffer(VideoState &s, std::unique_ptr<plume::RenderBuffer> b);
void PresentLocked(VideoState &s, u32 front_buffer_texture_va);
bool CreateOrRecycleHostTexture(VideoState &s, HostTexture &host,
                                const plume::RenderTextureDesc &desc, const char *tag,
                                plume::RenderPool *pool = nullptr);
void EvictHostTexturePool(VideoState &s);
void EvictStaleGuestTextures(VideoState &s);

inline f64 FrameAgeSeconds(const VideoState &s, u64 frame) {
  if (frame >= s.guest_frames)
    return 0.0;
  if (s.guest_frames - frame >= kFrameClockRing)
    return 1e9;
  return s.frame_clock[s.guest_frames % kFrameClockRing] - s.frame_clock[frame % kFrameClockRing];
}

void ParkHostTexture(VideoState &s, HostTexture &host);
void DestroyHostTexture(VideoState &s, HostTexture &host);

u32 AllocateDescriptorSlot(VideoState &s);
u32 BindTextureSRVLocked(VideoState &s, HostTexture &host);
u32 BindTextureSRVSwizzledLocked(VideoState &s, HostTexture &host, u32 swizzle);
void ReleaseTextureSRVLocked(VideoState &s, HostTexture &host);
void DrainDescriptorSlotsLocked(VideoState &s, u32 slot);

void TransitionLocked(VideoState &s, HostTexture &host, plume::RenderTextureLayout layout);
void TransitionManyLocked(VideoState &s, const HostTextureTransition *transitions, u32 count);

plume::RenderColor ArgbToRenderColor(u32 argb);

bool DumpHostMipBc1Locked(VideoState &s, HostTexture &host, u32 level, const char *path);
// Temporary diagnostics: mip 0 of a 4-byte-per-texel image (or a depth image's
// depth aspect) as raw texels after a "w h isDepth" text line.
bool DumpHostRawLocked(VideoState &s, HostTexture &host, const char *path);
bool DumpHostTextureLocked(VideoState &s, HostTexture &host, const char *path, float scale,
                           u32 lut_index = kInvalidDescriptorIndex,
                           u32 swizzle = kIdentityFetchSwizzle, u32 level = 0, bool alpha_view = false);

void RenderDocInit();
void RenderDocFrameBoundary(u64 guest_frame_just_presented);

void DrainHostDebugMessages(VideoState &s, const char *phase);

bool DiagShouldLog(u64 site, u32 *n_out);

}

namespace eot::gpu {

void SelectPresentBlitMode(u32 src_w, u32 src_h, float dst_w, float dst_h, float extra[4]);

}
