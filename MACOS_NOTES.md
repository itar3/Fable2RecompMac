# macOS (Apple Silicon) port — status and notes

> Working log of the port, oldest decisions first. For 1.0 the emulated-GPU
> path it started with (the patched SDK GPU plugin, `tools/sdk-patches`,
> `docs/MACOS.md`) was removed; setup is in `docs/SETUP.md`.

Working notes for continuing the macOS arm64 port. Last updated 2026-10-16.

## Current state

- Builds and runs natively on an M4 Pro (macOS 27), Vulkan through MoltenVK.
- Gets in-game (Bowerstone, new game) with sound, textured ground and correct
  shadows.
- **Performance: at the game's 30 fps cap in Bowerstone** (was 11-19 fps) since
  two shader translation fixes in the SDK fork (2026-10-16, see "Shader
  fixes" below).

## Two repositories

| Repo | Branch | What |
|---|---|---|
| `Fable2Recomp` (this one) | `macos-arm64-port` | SDK 0.10 port, macOS presets, codegen config fixes |
| `rexglue-sdk-src` (a local clone of rexglue/rexglue-sdk) | `fable2-mac`, based on `bd833a2` (the commit of the 0.10.0.24-dev SDK release used here) | GPU plugin fixes for MoltenVK |

The game currently runs with the **patched GPU plugin from the SDK fork**:
`out/build/mac-arm64-release/librexgpu-xenos.dylib` is a copy of
`rexglue-sdk-src/out/mac-arm64/Release/librexgpu-xenos.dylib`, the original is
kept as `librexgpu-xenos.dylib.orig`. Relinking `Fable2` copies the SDK's
original plugin back over it (rexglue_configure_target staging), so copy the
patched one again after every relink of the game.

## Setup

- SDK install: the ReXGlue SDK release 0.10.0.24-dev (mac-arm64),
  `REXSDK` points there in `~/.zshrc`.
- Game files: `assets/game` (extracted from the Games on Demand package with
  `tools/sdk_tools/extract`), title update 1 for media ID `05FBECEA`
  (`default.xexp`) next to `default.xex` and in `assets/update`. Codegen logs
  `XEX patch applied successfully ... 0.0.1.33` when it is picked up.
- Build: `rexglue codegen Fable2_manifest.toml`, then
  `cmake --preset mac-arm64-release && cmake --build out/build/mac-arm64-release`.
- SDK fork plugin build (~30 s incremental):
  `cmake --build out/build/mac-arm64 --config Release --target rexgpu-xenos`
  in `rexglue-sdk-src` (configured with `cmake --preset mac-arm64`).

Typical test launch (from `out/build/mac-arm64-release`):

```
MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0 ./Fable2 <repo>/assets --log_file=<log> \
  --readback_memexport=false --vulkan_draw_stats=true \
  --fullscreen=false --window_width=1280 --window_height=720
```

- `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0` moves MoltenVK's Vulkan→Metal encoding
  off the GPU emulation thread.
- `--readback_memexport=false` removes a GPU→CPU stall per memexport draw (no
  visible breakage seen so far).
- `--vulkan_draw_stats=true` (added in the fork) logs per-frame counts once a
  second: fps, draws, quad patch grids, render target transfers, render passes,
  barriers, uploads moved before render passes, shared memory barriers by usage.

## What was fixed (and why)

### This repo
- Ported to SDK 0.10: `rex::runtime` target, `ReXApp`-based `main.cpp`,
  `REX_HOOK`, `Fable2_manifest.toml` + `Fable2_config.toml` include.
- Removed the 22 import stubs — SDK 0.10 implements all of them.
- Native heap (`src/heap.cpp`): the 256 MB dlmalloc arena comes from the 4 KB
  virtual heap (SDK 0.10's system heap is too small). `lh_PhysicalAllocCached`
  is **no longer hooked** — serving it from a virtual arena broke GPU and XMA
  audio (they read by physical address): no sound, missing textures.
- ~115 functions codegen missed (only reachable through pointers — Fable 2's
  own classes have no RTTI, so the SDK's vtable scanner misses them) added to
  `Fable2_config.toml`, found with `tools/ptrscan.py` over a dump of the
  patched image (`tools/sdk_tools/dump`). Remaining misses show up as
  `[FATAL] Call to invalid or unregistered function at guest address 0x...`;
  check the gap between the neighbouring generated functions and add an entry.
- `generated/Fable2_register.cpp` is built with `-O0 -fno-global-isel` and no
  PCH (it took 11 minutes otherwise; GlobalISel's Localizer is quadratic on it).
- MoltenVK per-pipeline "primitive restart" warning spam silenced.
- Texture LOD bias is applied in the shaders (2026-11-08): MoltenVK reports
  `samplerMipLodBias = false` and drops `VkSamplerCreateInfo::mipLodBias`.
  The fetch constant's bias (dword 4 bits 12..21, 1/32 steps) rides in bits
  20..29 of the per-slot sampler index (`SamplerLodBiasBits`) and
  `shader_common.h` (`xeSampler` / `XE_SAMPLE`) applies it with `SampleBias`.
  A texture mirrored above the target's scale (a 1x small target reading a 2x
  mirror) also gets -log2(ratio) so it never reads a coarser mip than the
  console. Symptom it fixed: no distant trees (the tree impostor atlases build
  each mip from the level above with a -1 bias; without it every mip read
  itself, empty). `fable2_fetch_lod_bias=false` switches it off for A/B.
  Runtime shader translations are dropped when `shader_common.h` or the
  translator changes (`shader_runtime/stamp.txt`).
- Resolves into narrow formats (k_1_5_5_5, k_5_6_5, k_4_4_4_4, k_2_10_10_10)
  round to the destination's bits, as the console does (1-bit alpha snaps).
- Less CPU (2026-11-12, `src/cpu_idle.cpp`): the game thread's fixed-step
  loop (`sub_82276D10`, 30 logic ticks/s) waited for its next tick by calling
  SwitchToThread and re-reading the clock; it now sleeps until just before the
  tick, computed from the loop's registers (`fable2_sleep_waits`, default on).
  Card 2 at the 60 cap: CPU 358-364% -> 274-276%, same frame times. Profiled
  remainder (2.9 cores): 3D Engine 0.86, render thread 0.63 (real work: fewer
  idle spins or no sort-fill wait measured no change), game thread 0.43,
  Metal/MoltenVK 0.4, SDK TimerQueue 0.11 (spin_wait_strategy in
  rexglue-sdk-src `src/core/timer_queue.cpp`; blocking_wait_strategy would fix
  it but needs a rebuilt SDK runtime: not done).
- Debug: `fable2_debug_dump_mips_vs=<vs or ps hashes>` +
  `fable2_debug_dump_mips=<n>` dump every mip level (colour and alpha) of the
  textures those draws bind, as logs/mip*.ppm.

### SDK fork (`fable2-mac`)
- **Ground**: Fable 2's ground is tessellated quad patches (continuous for the
  main pass, adaptive for another pass — the missing adaptive layer showed as
  "silhouettes under the ground"). MoltenVK tessellation drew nothing and
  restarted the Metal render pass for every draw. New host vertex shader type
  `kQuadPatchAsTriangleList` draws each patch as an instanced, indexed
  n×n triangle grid (n from VGT_HOS_MAX_TESS_LEVEL + 1, 16 in Bowerstone) and
  runs the guest domain shader as a vertex shader (r0.x patch index, r0.yz
  domain location). cvar `vulkan_expand_quad_patches_in_vs`, default on macOS.
- **Uploads**: every shared memory upload ended the render pass (~230 per
  frame, a full tile store/load each on Apple GPUs). Uploads whose pages the
  current render pass has not accessed are now executed before that render
  pass (deferred command buffer placeholder + per-page render pass access
  stamps). ~330 moved, ~7 still breaking per frame; +50% fps.
- `vulkan_draw_stats` diagnostics; `vulkan_debug_skip_render_target_transfers`
  (debug only, breaks rendering) to measure transfer cost — **not yet tried**.

## Findings that rule things out
- `render_target_path_vulkan=fsi` is badly broken on MoltenVK (garbage bands).
- No effect on fps: occlusion queries off, geometry-shader fallbacks forced,
  `gpu_3d_to_2d_texture`, `gamma_render_target_as_unorm16`, anisotropic
  filtering, `resolution_scale` (already 1), fewer grid vertices (instancing).
- Fable 2 is a 30 fps game; >30 needs the game's frame limiter patched.

## Next steps (performance)
Per frame in Bowerstone streets: ~3200 draws, ~80–110 render target transfers,
~125–170 render passes (~100–130 ended by barriers), ~230–300 image barriers.

1. Run once with `--vulkan_debug_skip_render_target_transfers=true` to measure
   how much of the frame the EDRAM transfers cost.
2. Find the source of the image barriers (texture cache loads via compute /
   render target ↔ texture transitions) and whether texture loads can be
   moved before render passes like the buffer uploads.
3. GPU-side attribution: Metal System Trace works with
   `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xctrace record
   --template 'Metal System Trace' --attach <pid> --time-limit 5s`, but GPU
   intervals could not be matched to encoder labels yet.
4. Consider upstreaming the fork changes to rexglue-sdk.

## Tools
- `tools/sdk_tools/` — built against the SDK (see its CMakeLists.txt):
  `extract <STFS/GOD header file> <out dir>` (unpack packages),
  `dump <game dir> 82000000 1620000 <out>` (decrypted, TU-patched image),
  `flags` (all cvars with defaults; run it from the game's build directory so
  the GPU plugin is found).
- `tools/ptrscan.py <image.bin> <repo>` — candidate missed functions →
  `candidates.toml` (drop ones overlapping hand-written config entries).
- `tools/gputime.py` — per-process GPU time over 3 s (from ioreg).

## Native renderer scoping (2026-10-16)
- `render_target_path_vulkan=fsi`: ~35% faster than host RTs (0 transfers) but
  still corrupt after giving every EDRAM shader a raster order group and after
  `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0`. Parked.
- The statically linked Xbox D3D library is the block **0x82B80000–0x82BD4000**
  (1212 functions; found from the callers of the `Vd*` kernel imports — ring
  buffer init at 0x82BA2830, the `VdSwap` caller at 0x82BA34D8). 308 of its
  functions are called from outside, from ~1000 game functions. 0x82B96E78
  (394 callers) is an empty stripped debug hook. D3DX and the runtime
  HLSL/FXLite compiler are also linked in.
- Call graph script: `tools/callgraph.py <generated> cg.pkl`, then `tools/api.py cg.pkl 82b80000 82bd4000` (parses
  `DEFINE_REX_FUNC` and `sub_XXXXXXXX(ctx` calls in `generated/`).
- Cheaper idea before a full renderer: hook the game's D3D setup to turn off
  4× MSAA and predicated tiling (two screen bands), which cause the 1×↔4×
  transfers and the per-band draw replays.

## Render mode / predicated tiling experiment (2026-10-16)
XDK functions identified (beyond the 80 small state setters that
rexglue-native-kit's `xdk_sigs.py` matched exactly): `D3DDevice_Resolve`
0x82206888, `D3DDevice_BeginTiling` 0x822A61C0 (game wrapper 0x821A1700),
`D3DDevice_EndTiling` 0x8227F3B0, `SetPredication` 0x822655F0 (fuzzy),
DrawVertices 0x8221C3E8 / DrawIndexedVertices 0x8221DFC0 (they emit
SET_BIN_MASK). Scene start 0x822877E8 / scene end 0x822C40C8 read the active
render mode through `obj+4` and tile only when its tile count is > 1.

Fable 2 has a table of 10 render modes at 0x834BE010 (108-byte entries: +4
width, +8 height, +12 EDRAM format, +16 MSAA 0/1/2, +20 tile count, +24 tile
rects, +104 flag), built by sub_83293788. The game uses entry 2, its dynamic
mode: rewritten every frame (seen as 1120x720, 2x MSAA, tiles 0-576 and
576-720). The writer was not found (a host write watch on the page caught
only other variables).

`src/render_mode.cpp`, `--fable2_render_mode=nomsaa`: patches the table and
re-patches entry 2 at scene start. Result: no more tiling (~5700 -> ~4050
draws per frame), but **worse** fps (7-13), the main depth targets stay 4x
MSAA (~115 transfers per frame unchanged) and rows 576-720 are garbage. So
the render surfaces get their sample count and size from elsewhere (set up
with the per-frame mode computation). Off by default. Next step if resumed:
find the code that creates the scene's EDRAM surfaces (it calls the
SetRenderTarget/SetDepthStencil helpers 0x821FC3A0/0x8226B078 at scene start).

## Where the GPU time goes (Metal System Trace, 2026-10-16)
Bowerstone street, host render targets, ~12 fps, GPU 91% busy:
- Fragment 62-72% of the time, vertex 15-21%, compute 5-6%.
- 72% of fragment time is in ~2 render passes per frame of 15-30 ms each
  (the main scene passes); thousands of other passes are tiny.
- The Metal compiler reports register spills in 101-103 of those ~105 big
  passes (spill sizes up to 1104 bytes per thread).
- Skipping transfers by kind (`vulkan_debug_skip_render_target_transfer_kinds`,
  SDK fork) breaks the picture and gives no fps for `msaa` or `colordepth`;
  removing predicated tiling (fable2_render_mode) also gave none. So
  transfers and draw count are not the limit here, the scene's pixel
  shading is.
- Splitting `xe_var_registers` into one variable per register (when there is
  no relative addressing) changed neither the spills nor fps (reverted): the
  spills come from the shaders' size (one loop over a ~100-case switch).
Next: per-draw cost inside the big passes (Xcode GPU frame capture via
MoltenVK, MVK_CONFIG_AUTO_GPU_CAPTURE_*), or split it with debug switches
(host 1x MSAA; trivial pixel shaders) to separate MSAA cost from shading cost.
Trace parsing scripts: tools/gputrace/*.py.

## Shader fixes (2026-10-16) — the real bottleneck
`--vulkan_debug_trivial_pixel_shaders=true` (SDK fork) reached the 30 fps cap
with the GPU 42% busy: the translated pixel shaders were the cost. Two fixes
in the SPIR-V translator (SDK fork `fable2-mac`):
1. `spirv_texture_fetch_implicit_lod` (default on): texture fetches used
   explicit-gradient sampling (coarse derivatives * exp2(lod)); now implicit
   LOD with a bias for 1D/2D/cube. ~12 -> ~17 fps in a busy view.
2. `spirv_pixel_shader_no_contraction` (default off): SPIRV-Cross emits every
   NoContraction op as a `[[clang::optnone]]` spvFMul/spvFAdd call (~500 per
   pixel shader). Dropped in host-RT pixel shaders (vertex shaders and FSI keep
   it). -> 29.1 fps (cap) with 4000+ draws.
3. `spirv_vertex_shader_no_contraction` (default off): same for vertex
   shaders (positions stay consistent via the Invariant position output).
   Vertex work had become 41% of GPU time; gameplay 25-40 -> 35-50 fps.

60 fps: Fable 2 presents every second vblank; `main.cpp` defaults
`video_mode_refresh_rate` to 120 (tested: game speed, animation and audio
normal). `--video_mode_refresh_rate=60` restores 30 fps. The window title
shows the frame rate (`fable2_title_fps`).

Next: frame-time hitches (2-7 fps for a second, likely first-use pipeline
compiles); GPU trace in gameplay to find the next cost (compute was 12%).

Codegen miss fixed: 0x82DE2B58 / 0x82DE2B80 / 0x82DE2BA8 are three bctr
thunks codegen had merged (crash "unregistered function 0x82DE2BA8" after
~15 minutes of play).

## Known issues
- **Barnum's Image Capturing Device** (childhood): hung (Barnum never says
  his line) when continued from a manual save made right before accepting
  it, in any mode (30/60 fps, with or without readback). On a fresh run
  without that save it works. The save captured a state in which the scene
  can't start, so it's not a port bug; avoid saving right before scripted
  scenes when testing.

## Metal GPU frame capture does not work (2026-10-16)
`fable2_gpu_capture_trigger` (src/gpu_capture.cpp) starts an MTLCaptureManager
capture, but `stopCapture` never finishes: while writing the .gputrace the
capture layer reads every Metal buffer, including the guest memory mirror,
whose host pages the SDK keeps protected for write tracking; the reads fault
into the SDK's exception handler (`_sigtramp` under
GTTraceDump_writeUnsortedCapture) and the game hangs. Making it work would
need the SDK's page protection disabled during capture.
Other results this round: plain IEEE multiplication
(`spirv_xenos_zero_multiplication=false`) is only ~5-7% faster per draw and
puts black boxes behind loading screen text, so it stays off; single
unsigned sampling (`spirv_debug_texture_fetch_unsigned_only`) gives ~0%.
Busy Bowerstone views: ~28.7 ms median frame at 5000+ draws (~3.1-3.3 us of
GPU time per draw).

## Native renderer debugging workflow (2026-11-04)

Submodule edits live as patch series the build applies at CMake configure time
(cmake/native_renderer.cmake, `fable2_apply_patch_series`): `tools/xenosrecomp-patches/` (one patch,
the whole XenosRecomp diff) and `tools/plume-patches/`. Since 2026-11-13 plume's is a series:
0001-0006 are self-contained, game-agnostic commits prepared as pull requests for zolaware/plume
(formats, root descriptors, dynamic blend constants/stencil reference, mip-level framebuffer size,
occlusion queries, render pass kept across framebuffer rebinds) and 0007 holds the renderer-specific
rest (lazy clears, the MoltenVK barrier shortcut, pass statistics). The renderer opts in to the
dynamic blend constant and stencil reference in `CreateHostGraphicsPipeline`.
A series is checked as a whole (applied to a scratch copy of the index and compared with the working
tree), so its patches may overlap; a tree with an older patch set is reset to the submodule commit
and patched again. To change one: `git am` the series onto a plume clone at the submodule commit,
edit or add commits, then regenerate with `git format-patch --no-signature -o tools/plume-patches <base>..`.

Everything below runs against the live game (launched from `out/build/mac-arm64-native`); trigger
files are picked up every 30 frames, next to the executable.

- `cvar_now`: `name=value` lines applied at run time (A/B tests in one run, no restart).
- Hide a layer: `fable2_skip_vs=<vshash>[:indexcount]` / `fable2_skip_ps=<pshash>`.
- Constants as the draw uploaded them: `fable2_debug_vs_consts=<vshash>:first-last`
  (`fable2_debug_ps_consts` for pixel shaders). Never read float constants from the device at
  replay: they are outside the packet's device window and show the game thread's live state.
- Bound textures to PPM: `fable2_debug_dump_bound_vs=<vshash,...>` then `touch diag_now`
  (files `logs/bound_<vs>_slot<N>_<va>.ppm`). `fable2_diag_vs=<vshash[:count]>` logs bindings.
- Vertex streams: `fable2_debug_dump_fur=true` (dog fur shells/body, `logs/{fur,body}_s<N>.bin`);
  extend the vs-hash check in CaptureDraw for other meshes.
- One frame of D3D calls: `touch trace_now` (`[d3d-trace]` lines; draws through other paths,
  like the dog's, are not traced).
- Screenshots of the game window: `swift tools/winid.swift` prints the window id,
  then `screencapture -x -o -l<id> out.png`.

Lessons: dump real data early instead of reasoning about swizzles/endianness; hide layers to find
which draw is wrong; guest texture `baseAddress` is a virtual address (0xE0000000 alias is
physical + 0x1000): use `mem::at` (as the texture upload does), not `mem::phys`.
