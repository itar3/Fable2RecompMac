# Native renderer: plan and state

Goal: replace ReXGlue's emulated Xbox 360 GPU (xenos plugin) with a native
renderer that hooks the game's D3D calls, like Unleashed Recompiled and
EdgeOfTimeRecomp do. That removes EDRAM emulation, render target transfers,
resolve round trips through guest memory and on-the-fly shader translation,
which is where the frame time goes now (see `MACOS_NOTES.md`). Target: 60 fps
at high resolutions on Apple Silicon.

Branch: `macos-native-renderer`. The playable emulated version stays on
`macos-emulated-gpu`.

## Template: EdgeOfTimeRecomp

[goliathret/EdgeOfTimeRecomp](https://github.com/goliathret/EdgeOfTimeRecomp)
(BSD-3, Spider-Man: Edge of Time) is a ReXGlue port with a native renderer
that runs on Apple Silicon through MoltenVK at up to 2160p. Its parts:

| Part | Where | License |
|---|---|---|
| Renderer (~15k lines): device, draw, textures, surfaces, resolve, present, pipeline cache | `src/gpu/` | BSD-3 |
| Graphics layer (Vulkan/Metal/D3D12 abstraction) | [zolaware/plume](https://github.com/zolaware/plume) | MIT |
| Ahead-of-time shader translator | [goliathret/reeot-XenosRecomp](https://github.com/goliathret/reeot-XenosRecomp) | MIT |

Integration with ReXGlue: ReXGlue's detached mode. `OnPreSetup` sets
`config.graphics = nullptr` (no GPU plugin), `OnCreateImmediateDrawer`
returns the renderer's ImGui drawer. The renderer hooks about 22 XDK D3D
functions by name (`extern "C" REX_FUNC(D3DDevice_...)`, names assigned in the
codegen config) and reads render states and shader constants straight from the
guest D3D device struct at draw time.

A local clone for reference is at `../EdgeOfTimeRecomp`.

## D3D hooks: Edge of Time names → Fable 2 addresses

Status: **known** = confirmed from the code; **candidate** = from the
rexglue-native-kit signature match or call structure, still to confirm.

| Hook (EdgeOfTimeRecomp) | Fable 2 | Status |
|---|---|---|
| D3DDevice_Resolve | 0x82206888 | known (args match the XDK Resolve, called per tile by EndTiling) |
| D3DDevice_DrawVertices | 0x8221C3E8 | known (emits SET_BIN_MASK, draw helpers) |
| D3DDevice_DrawIndexedVertices | 0x8221DFC0 | known (same structure) |
| D3DDevice_Swap | 0x82BA34D8 | known (device, front buffer texture in r4; calls VdSwap) |
| D3DDevice_SetVertexDeclaration | 0x822224A8 | known (exact signature match) |
| D3DDevice_BeginTiling / EndTiling | 0x822A61C0 / 0x8227F3B0 | known (Fable 2 tiles; Edge of Time hooks none, handle here) |
| D3DDevice_SetPredication | 0x822655F0 | candidate (fuzzy 0.91) |
| D3DDevice_SetRenderTarget | 0x822869A0 | known (stores at device + (index + 3110) * 4 = 0x3098 + 4i) |
| D3DDevice_SetDepthStencilSurface | 0x82264590 | known (stores 0x30A8, writes DEPTH_INFO 0x2888, HI control 0x2940) |
| D3DDevice_SetIndices | 0x8219CC30 | known (stores 0x3094) |
| D3DDevice_SetStreamSource | 0x821B6C60 | known (dev, stream, vb, offset, stride; vb +24 address / +28 size; stores 0x30AC + 4s; vertex fetch constant 95 - s at 0x480) |
| D3DDevice_SetVertexShader | 0x82232510 | known (stores 0x3198, clears a flag byte at 0x2ABE; shader object: container at +0x368, microcode pointer at +0x14) |
| D3DDevice_SetPixelShader | 0x82208BB0 | known (stores 0x3194; shader object: container at +0x28, microcode pointer at +0x14) |
| D3DDevice_SetTexture | 0x821B6EE0 | known (dev, sampler, tex; stores 0x3100 + 4s; copies tex +28..+48 to fetch constants 0x480 + 24s) |
| D3DDevice_SetViewport | 0x821F9F58 | known (device, D3DVIEWPORT9*: x, y, w, h ints, min/max Z floats; register helper 0x821F9D00) |
| D3DDevice_ClearF | 0x821E2FE0 | known (device, flags, rect, color*, z f1, stencil r8; D3DDevice_Clear 0x821E2B48 converts the color and calls it) |
| D3DDevice_BeginVertices / BeginIndexedVertices | 0x82205F68 / ? | BeginVertices known (device, prim, count, stride; returns the vertex pointer; DrawVerticesUP 0x82265CD8 wraps it); BeginIndexedVertices to find |
| D3DDevice_CreateVertexShader / CreatePixelShader | 0x82B992B0 / 0x82B994B8 area | candidate; container checks at 0x82B9E008 (callers 0x82B9DCB8, 0x82B9DD48) |
| XGRegisterVertexShader / PixelShader | ? | to find |
| D3D_UnlockResource | 0x822C2630 | known (resource in r3, atomically drops the lock count; D3DVertexBuffer_Unlock 0x82B9EEC0 / D3DIndexBuffer_Unlock 0x82B9EED0 tail-call it; 52 callers) |
| D3DDevice_SetGammaRamp / SetPWLGamma | ? | to find |

GPU waits (Edge of Time no-ops them, `src/gpu/hooks/stubs.cpp`). All read the
GPU write-back block whose pointer is at device +10896 (completed fence at +0,
secondary position at +4, primary ring read pointer at +60); the device's
current fence is at +10908:

| Wait | Fable 2 |
|---|---|
| BlockOnFence(device, fence, ...) | 0x82242668 |
| BlockOnSecondaryPosition | 0x82206400 |
| WaitPrimaryRingSpace | 0x82286618 |
| device-level waits (BlockUntilIdle, vsync) | 0x82BA4D80, 0x82BA5818 (to tell apart) |
| spin helpers (hang watchdog, delay) | 0x82BA1EE0, 0x82BA1FA8 |

Alternative that also covers waits inlined into game code: keep the
write-back block reporting "done" (fence = current fence, read pointer =
write pointer) after every kick-off.

Fable 2 specifics with no Edge of Time counterpart:
- **Tessellated quad patches** (the ground; continuous and adaptive modes).
- **Predicated tiling with 2x MSAA**, dynamic render mode table at
  0x834BE010 (`src/render_mode.cpp`).
- Many D3D functions are LTCG copies inside game code (0x8217..0x822C), not
  only in the D3D block 0x82B80000..0x82BD4000.

## Device struct

Fable 2 offsets confirmed from its setters (EdgeOfTimeRecomp value in brackets):

| Field | Fable 2 | Edge of Time |
|---|---|---|
| Fetch constants (texture 24 B each; vertex 8 B, stream s = slot 95 - s) | 0x480 | 0x480 |
| GPU register shadow (DEPTH_INFO 0x2888, RB_DEPTHCONTROL area, HI control 0x2940) | same | same |
| Vertex declaration | 0x2E2C | 0x2FB8 |
| Index buffer | 0x3094 | 0x31F4 |
| Render targets 0..3 | 0x3098 | 0x31F8 |
| Depth surface | 0x30A8 | 0x3208 |
| Stream sources 0..15 | 0x30AC | 0x320C |
| Textures 0..? | 0x3100 | 0x3260 |
| Pixel / vertex shader | 0x3194 / 0x3198 | ? |

The register shadow at the start of the device is identical between the two
XDKs; the object pointers after it moved (mostly by 0x160, not uniformly).
Vertex buffer (+24 address, +28 size) and texture (+28 fetch constant) object
layouts match Edge of Time's.

Edge of Time reads the guest D3D device directly (`src/gpu/d3d.h`: device
size 0x6080, constant area at 1152, sampler/declaration/shader fields at
11070..12358). Those offsets belong to its 2011 XDK; Fable 2 (2008) needs its
own. Derive them from the setters (SetVertexShader, SetTexture, SetStreamSource
write their device fields) once those are confirmed.

## Shaders

Fable 2 keeps its shaders in `data/Shaders/ShadersRelease.sbk` (Lionhead
`ShaderBankFile`). **It does not use standard XDK shader containers:**
- the bank holds no `0x102A11xx` containers (mostly compressed or encrypted
  data, plus Lionhead parameter names such as `m_BetaRayleigh`);
- the XDK shader creation functions (0x82B992B0 / 0x82B994B8, container
  pointer in r3) are never called (`fable2_shader_dump_dir` capture: 0 files
  after reaching the main menu);
- no function that parses containers is reachable from the bank loader
  `sub_82B83378` (which checks the `ShaderBankFile` header and calls the
  SetVertexShader / SetPixelShader wrappers 0x82220880 / 0x82232498).

**Capture works** (`fable2_shader_dump_dir`, `src/shader_capture.cpp`): the
shader objects embed the container's virtual part (vertex at +0x368, pixel at
+0x28) and point to the microcode (vertex +0x20, pixel +0x18; often a virtual
address, not physical), the same layout as Edge of Time's XDK. 65 shaders by
the main menu, all with microcode. reeot-XenosRecomp (built from
`../EdgeOfTimeRecomp/thirdparty/XenosRecomp`, branch `fable2`, patches in
`tools/xenosrecomp-patches/` on top of commit `a031503`) **translates and
compiles all 65 to SPIR-V with no errors** after these Fable 2 fixes:
- vertex shaders that fetch the same usage several times (`POSITION0` x4 ...):
  each usage declared once; these shaders need **vertex pulling** in the
  renderer (stderr note "repeated vertex fetch usages");
- vertex shader exports to point size (register 63) and **memory export**
  (eA/eM, 32..37) go to an unused local; memexport shaders need renderer
  support (stderr note "uses memory export");
- vertex texture samplers 16..19: descriptor indices at shared constant bytes
  **944.. (4 dimensions x 4 samplers, then 4 sampler indices)**, appended after
  Edge of Time's 944-byte `SharedConstants`; the renderer's struct must get
  the same fields;
- vertex shaders sample with `SampleLevel(..., 0)` (`#define Sample` for VS).

Usage: `XenosRecomp <shader_dump> <out shader_cache.cpp> <shader_common.h>`
(`--hlsl` as 4th argument writes the HLSL instead).

So the shader objects are built by Lionhead's loader directly (microcode in
physical memory plus their own parameter tables). XenosRecomp, which reads
containers, needs one of:
1. **Runtime capture of shader objects** at SetVertexShader / SetPixelShader
   (0x82208BB0 / 0x82232510): read microcode address/size and the literal
   (definition) table from the XDK shader object, write a synthetic container,
   and let XenosRecomp treat constants as a plain `c[0..255]` array (its README
   describes this fallback when reflection data is missing). Needs the XDK
   shader object layout (Edge of Time reads e.g. the pixel shader literal table
   at +60).
2. **Decode the bank format** (loader 0x82B83378, large helper 0x82B82310 is
   a decompression candidate) and convert offline.

Option 1 is more direct; it only captures shaders that get used, so the
capture run has to visit the game's areas.

## Steps and progress

| Step | Weight | Done |
|---|---|---|
| 1. Study EdgeOfTimeRecomp | 5% | 100% |
| 2. D3D hook map + device layout | 10% | ~95% (all needed hooks, waits, device fields; gamma optional; XGRegister not needed: shaders register at Set*Shader) |
| 3. Shaders: capture + XenosRecomp adaptation | 20% | ~40% (all 65 menu shaders compile; full-game capture, memexport and vertex pulling left) |
| 4. Renderer + plume in, main menu on screen | 20% | 0% |
| 5. In-game rendering (textures, targets, resolves, tiling, patches) | 25% | 0% |
| 6. Effects and full-game testing | 20% | 0% |
| **Total** | | **~22.5%** |

### Step details

1. ~~Study EdgeOfTimeRecomp~~ (this document).
2. Confirm the hook addresses above; derive the device struct layout.
3. Capture Fable 2's shaders (see Shaders: synthetic containers from shader
   objects) and adapt reeot-XenosRecomp to them.
4. Bring in plume and the renderer from EdgeOfTimeRecomp; detached mode in
   `main.cpp`; get the main menu on screen.
5. In-game rendering: textures, render targets, resolves, tiling, patches.
6. Effects and full-game testing.

## Performance work (2026-11-05)

Goal set by the user: run faster **with the same settings**: same render scale
(2x in a Retina window), guest 2x MSAA, Xenos zero multiplication and the Z-pass
emulation all stay on. Measurements are in busy Bowerstone, standing still, from
the `[perf]` log line (`--eot_perf_frames=240`), which is now logged at info level.

### Result so far

| | Before | After |
|---|---|---|
| Frame time, busy view | ~21.5 ms (~46 fps) | ~19.2 ms (~52 fps) |
| GPU time per frame | ~21.5 ms | ~19.2 ms |
| Render thread submit | 3.7 ms | ~0 ms |
| Render pass begins per frame | ~290 | ~70 |

The game is now limited by the GPU only: the game thread waits ~8 ms per frame
for the render thread, which waits for the GPU.

### Changes (all uncommitted, each with a cvar for A/B)

1. **Render passes end only when a draw needs another framebuffer**
   (plume `setFramebuffer` no longer ends the active pass; `checkActiveRenderPass`
   switches lazily). Rebinding the same framebuffer, or binding one that got no
   draws before switching back, used to store and reload every attachment
   (expensive on tile GPUs). ~0.8 ms. cvar `fable2_lazy_render_pass` (default on).
2. **No host occlusion query for tests reported fully visible anyway**
   (`fable2_query_force_visible` answers them; only exposure tests read host
   counts). Each query ended the render pass twice (~45 restarts per frame).
   cvar `fable2_query_skip_forced` (default on), `src/gpu/queries.cpp`.
3. **Layout barriers on textures that aren't attachments of the active pass keep
   the pass** (plume `barriers`): Metal ignores image layouts and orders writes
   in earlier encoders before later reads. ~1.5 ms (main pass 14.5 -> 12.4 ms).
   cvar `fable2_barrier_keeps_pass` (default on). User: "all seems fine".
4. **MoltenVK encodes Metal commands on its own thread**
   (`MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0`, `src/platform/moltenvk.cpp`; the env
   variable still overrides it). Render thread submit 3.7 ms -> 0.
   Not yet confirmed: the first long session after this change ended in a GPU
   "device lost" (see Open below).

plume: the three plume patches were folded into one,
`tools/plume-patches/0001-Fable-2-plume-framebuffers-queries-blend-constants-render-passes.patch`
(the new hunks overlapped the old 0001 patch, which breaks the configure-time
reverse check).

Temporary diagnostics added (to remove when the perf work is done): `[passes]`
counters in plume (`g_passStats`: pass begins, restarts on the same framebuffer
by cause, attachment MB per frame), `fable2_debug_pass_log=true` (one frame of
render passes on >= 2 Mpx targets: draws, first clears, what ended it,
attachments), `fable2_debug_quarter_scissor` (quarter of the pixels, same
vertex work), `fable2_debug_skip_clears` (1 = resolve clears, 2 = game clears).

### Findings

- **GPU split of the main scene pass** (quarter-scissor test): ~57% pixel work
  (~7.5 ms), ~43% fixed (~5.6 ms) that doesn't shrink with fewer pixels: mostly
  loading/storing the 2x-scaled MSAA attachments (105 MB per main-pass begin).
- **Vertices are cheap**: skipping the background landscape (1.69 M vertices)
  saves only 0.8 ms; skipping the continuous landscape (0.29 M vertices) saves
  2.8 ms, i.e. the ground's cost is its pixel shader.
- **Zero multiplication** (`fable2_zero_mul`) costs ~13% of GPU time (~2.7 ms).
  A variant that computes the plain product and redoes it the Xenos way only
  when the result is NaN (a branch per multiply) was 3x more expensive (zero
  multiplication then cost 36%) and made the first launch compile for minutes;
  reverted. The translator already clamps rcp/rsq/log to +-FLT_MAX, so the
  infinity behind the black item previews comes from elsewhere.
- **Precise math is not slow here**: the dumped Metal shaders
  (`MVK_CONFIG_SHADER_DUMP_DIR`) have no `spvFMul`/optnone helpers (unlike the old
  emulated build). The largest shaders are big because of the Xenos control flow
  state machine (a loop with a program-counter switch and copies of all
  registers per block: 116k lines of MSL for one pixel shader, 72k for a
  skinned vertex shader with 15 pulled fetches).
- **Render passes on big targets**: 35 per frame, ~1.3 GB of attachments.
  12 of them contain no draws: clears of the MSAA and single-sample copies of the
  scene colour and depth (frame start, and per predicated tile on resolve).
  Skipping all clears (diagnostic) saves ~2 ms, all of it from the game's own
  clears; the resolve clears cost almost nothing.
- The Z-pass pre-run (943 draws) runs on the colour + depth framebuffer
  (105 MB per begin); a depth-only framebuffer would halve its load/store.
- CPU: game thread ~11 ms of work per frame, render thread ~5 ms of replay
  (3 ms of that binding constants).
- Xcode is not installed, so there are no Metal System Traces; per-draw GPU
  timing isn't possible either (timestamps end the render pass on Apple GPUs).

### Open

- **GPU device lost** (2026-11-05 02:21, ~9 min into a session with all four
  changes): Vulkan error -4, the renderer disables itself, the game keeps running
  without presenting. Seen twice before (Oct 23, Oct 25), so possibly not new.
  MoltenVK's message wasn't kept; launch with output to a file (docs/RUNNING.md)
  so the next one says GPU timeout or bad access. Suspects to switch off one at
  a time: `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=1`, `fable2_barrier_keeps_pass=false`.

- **Two MoltenVK copies in one process** (found 2026-11-05 from the saved
  stderr: objc "Class MVKBlockObserver is implemented in both ... may cause
  spurious casting failures and mysterious crashes"). `PrepareMoltenVK` looked
  for the staged ICD file in `~/.local/share/Fable2/vulkan`, but the build stages
  it next to the executable; so it loaded the bundled MoltenVK (1.4.3) directly
  AND the loader searched the system ICDs (`/usr/local/share/vulkan/icd.d`: the
  Vulkan SDK's MoltenVK 1.4.2 plus KosmicKrisp) and rendered with the system
  copy. Fixed: the bundled ICD next to the executable is used first (only the
  bundled loader + MoltenVK are mapped now; game renders, 58 fps in the night
  street). `FABLE2_SYSTEM_VULKAN=1` restores the old system search. A candidate
  cause of the device-lost freezes; all testing before this used MoltenVK 1.4.2.
- **White screen at Lucian's castle (intro)** (2026-11-05, user): the frame is
  drawn (38 fps, ~2150 draws) but all white; still white with the query skip,
  lazy pass and barrier changes switched off live, no NaN/inf (NaN painter shows
  no magenta). Not known whether it is new: next test is a launch with all of
  today's changes off from the start (`--fable2_query_skip_forced=false
  --fable2_lazy_render_pass=false --fable2_barrier_keeps_pass=false`,
  `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=1 FABLE2_SYSTEM_VULKAN=1`).

- **Castle = GPU fault, older than today**: with all of today's changes off and
  the system MoltenVK (morning-equivalent), Lucian's castle went black at 0 fps:
  MoltenVK "Lost VkDevice after MTLCommandBuffer ... execution failed (code 9):
  Invalid Resource (kIOGPUCommandBufferCallbackErrorInvalidResource)", i.e. the
  GPU touched an invalid resource or address. Probably also the cause of the
  earlier device-lost freezes. Metal GPU/API validation works without Xcode
  (`MTL_SHADER_VALIDATION=1 MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_ERROR_MODE=nslog`):
  the first run compiles every pipeline instrumented (minutes of 0 fps) and
  reports many harmless "INF or NAN detected in interpolant" lines.
- **Castle findings (2026-11-05 07:40)**: two symptoms of one problem. Either the
  GPU faults ("Invalid Resource"), or the picture goes black/white because the
  game's exposure constant c47 (`g_ActualFramebufferBrightness`) becomes 0/inf.
  The exposure input is a 280x180 k_32_FLOAT resolve the CPU reads back
  (`[writeback] 0x1a34fda4` / `0x1a385da4`); in bad runs its values freeze
  (identical every sample), in good runs they vary (avg ~0.5). Ruled out: today's
  perf changes, vertex textures, the hero coat writeback (the NPC outline was fine
  without it, so that one may be separate), nil Metal objects (checked with
  `vkGetMTLTextureMVK`/`vkGetMTLBufferMVK`). Skipping the billboard shader
  e6415e83 from launch avoided it once, as did slowing its capture path once:
  timing-dependent. Lifetime fixes kept: retired textures/views/buffers destroyed
  one fence later (bindless slots nulled first), upload ring trimming parks its
  chunks instead of destroying them.
- **Window resize bug** (found by API validation): after a resize the present
  render pass still uses the old size (renderTargetWidth 2560 > attachment
  width 2354). Harmless without validation, but should be fixed.

### Next steps (in order)

1. Find the device-lost cause and the castle white screen (above).
2. Full clears as render pass load action CLEAR (plume render pass variants)
   instead of LOAD + vkCmdClearAttachments; then lazy clears (a pending clear
   applied when the image is next used) to remove clear-only passes. Up to ~2 ms.
3. Z-pass pre-run on a depth-only framebuffer.
4. Cheaper exact zero multiplication, measured per shader before rebuilding all.
5. Translator code quality: the control-flow state machine.
6. Later, if load/store traffic still dominates: a native Metal backend (plume
   has one) for memoryless MSAA attachments and explicit load/store actions.

## 2026-11-06: occlusion queries, GPU faults, test automation

**Castle white haze = occlusion queries.** The blob is a light glow quad
(ps a8eaa0d5, vs b1b160f8, 6 vertices, depth test off) whose strength the game
takes from an occlusion query. Every query is answered "fully visible"
(`fable2_query_force_visible`, default on), so glows of lights behind the camera
or walls draw at full strength. With real counts the haze is gone, but buildings
in the distance pop and the ground flickers dark (lights switched off) while the
camera moves.

Findings on the queries (all of Fable 2's query draws are the same kind: vs
8f1c881c, no pixel shader, 48 vertices, depth func ALWAYS, back-face culling,
drawn into the 280x180 and 256x256 query passes, ~104 per frame):
- Stale answers (the result of a query object's previous use; the game reuses
  query objects) now report fully visible (`fable2_query_stale_visible`), and
  any visible host sample counts at least 1 after the render-scale division.
  Pops with real counts went from bursts of 21-33/s to mostly ~1/s.
- Still popping: on-screen boxes return 0 samples in some frames while the
  camera moves (`[pops] ... queries: N flickers/s`: 0 with a still camera,
  100-180/s while panning). Ruled out: culling (cull none: same), stale
  constants (`fable2_full_constant_upload`: same), the geometry caches (off: same;
  their 16+16-byte sampled change check now hashes ranges <= 8 KB fully anyway),
  draw sorting (runs stop at query commands), no-op draw skipping (not while a
  query is active), size thresholds (`fable2_query_binary`: same).
  `[qflip]` traces: the zero use had 1 draw into the same 280x180/256x256 target
  and viewport as the previous, nonzero use. Next: where the box lands
  (vertex positions / VS constants of the zero use vs the previous).
- Default stays forced visible (no popping; the castle haze remains) until the
  flicker is understood.

**GPU faults ("Invalid Resource", device lost).** MoltenVK records command
buffers without retaining their resources, and makes every bindless texture and
every device-addressable buffer resident for every command buffer. Fixed:
- retired textures, views and buffers are destroyed one fence later (the other
  in-flight frame may have them resident), bindless slots nulled first;
- upload-ring chunks trimmed after a spike are parked instead of destroyed;
- upload-ring and geometry-pool chunks get a 1 MB guard tail never handed out:
  vertex fetches past a draw's range near a chunk's end stayed outside the
  buffer. Proof: `fable2_geometry_caches=false` (20 MB/frame through the ring)
  lost the device within 6 s before, ran 30 s+ after.
- only the bundled MoltenVK is loaded (it loaded two copies before).
Still open: the castle (first heavy frames after the load) still loses the
device with all of these. Metal API + GPU validation report no bad access there
(the picture goes black instead).

**Test automation.** `--mnk_mode=true` maps the keyboard to the controller (A =
Space, Start = Return, left stick WASD, right stick = arrow keys). A small
CGEvent tool (`tools/keys.swift`: `keys <pid> <keycode:holdMs[:pauseMs]> ...`,
macOS key codes: Space 49, Return 36, W 13, S 1, A 0, D 2, arrows 123-126)
drives menus and camera pans, so tests can repeat the same path. Title screen:
A; main menu: Continue is the 2nd item; the user's save is the 4th card (Hero 3,
Old Town Bowerstone, at the guards that take you to Castle Fairfax).
**Smoke test** (`tools/smoke_test.py`, 2026-11-06): launches the build, presses
through title, Continue and the 4th save card, measures a 10 s still view and a
16 s camera pan in Bowerstone, optionally `--soak N` seconds of pans and `--route`
(castle: `"A:5500:1500"`, travel prompts need A held ~5 s), screenshots the window,
kills the game and restores the save folder (every run starts from the same save).
Input is a scripted controller inside the game (`src/scripted_input.cpp`: the test
writes `input_now` next to the exe, items `NAME:holdMs[:pauseMs]`), and the game is
launched with `SDL_MAC_BACKGROUND_APP=1`, so the window never needs focus and the
user can keep working. Output per run: `logs/smoke/<label>-<time>/runN/`
(summary.json, jpg shots, stdio.txt) and one line: frame/GPU ms, draws, pops/s,
query flickers/s, exposure readback (`run` = longest streak of identical values),
dog coat, flip check of the world/castle shots against `logs/smoke/ref/*.jpg`
(correlation with the reference / the upside-down reference), shaders drawn without
vertex fetches, stage mismatches, device lost / exit. `--diag` / `--diag-world` take
a per-draw diag frame; `--stop-on-nofetch` ends a batch at the first such launch.
Extra game flags go last: `--args --fable2_query_force_visible=false`; environment
with `--env K=V`. ~2.3 min per castle run.

Temporary diagnostics removed (2026-11-06 cleanup): `[res]`, `[k32]`, `[qdraw]`,
`[qflip]`, `[vstex]`/`[vspull]` (`fable2_debug_vs_tex`), `[passes]`/`fable2_debug_pass_log`,
plume `[mvk-check]`, `fable2_debug_quarter_scissor`, `fable2_debug_skip_clears`,
`fable2_debug_dump_bound_ps`. Kept as test metrics: the `[pops]` line (pops/s and
query flickers/s, every `[perf]` interval) and the `[writeback]` exposure min/max/avg.

## 2026-11-06 (session 2): shader identity, occlusion pass, brown castle

**Shader objects are reused.** The game loads shader objects into memory that
held other shaders, often without a registration call. Keyed by address, the
renderer kept returning the old translation. Two forms, both random per launch:
- a pixel shader's memory reused for a vertex shader: vs slot got the UI pixel
  shader 6b0779590f1e3de3 (fragment entry point, pipeline never built), seen with
  the upside-down castle / device lost runs. Fixed 7850e93e (keys = address << 1 |
  stage).
- same stage: the castle's exposure draw (ps 8f29906f498ea8e4 into the 280x180
  k_32_FLOAT targets the CPU reads back) ran with vs a108 instead of its own
  shader, exposure read 0 and the castle was one flat brown/blue colour (~1 in 3
  launches). Fixed bcda5dee: SetVertexShader / SetPixelShader / GpuLoadShaders
  compare a fingerprint (container header, microcode pointer, first 128 bytes of
  both parts) with the registered one and register again when it changed
  (`[shaders] ... rewritten in place`, ~20 per castle run). Shaders the stale
  entries had hidden get dumped as cache misses on the first run and need a
  rebuild (vs 96681647db886f48, ps 05e3bdd0a3b46ca5 so far). Castle 0 of 6 brown
  after (was 8 of ~23).
- Likely the same cause behind other "random per launch" bugs (dog coat).

**Occlusion queries.** The 280x180 query pass draws the a108/2ac2 quad (s13 =
scene depth) and tests boxes (vs 8f1c881c: per-box AABB at fetch slot 93, 48
vertices per box, box = vertex id / c48.x, WVP c0-c3, depth GEQUAL) against it.
Kept as a flat plane, everything past the plane failed: buildings popped and the
ground went dark while the camera turned. `fable2_query_scene_depth=true` copies
the scene depth (4x down) instead; user: no popping with real counts. Default off
while real counts stay off. Probes ruled out: stale box data, wrong box
positions/constants (CPU projection agrees with the GPU), pending BeginVertices
draws, Total/ZFail fields (game ignores them), query hold (hold 3 made pops worse).
Still open: the castle white glow stays with real counts + scene depth.

**Exposure readback log** is per target now (c0b43c00): the game alternates two
targets every frame, and the shared counter always logged the same one (the old
"one target per area" reading was an artifact).

**Smoke test additions:** `--soak`, `--route-cvar name=value` (cvar_now at the
castle shot), `--stop-on-frozen` (castle exposure 0 / frozen readback), flip check,
`--disconnect`, per-run CASTLE-EXPOSURE-0 flag.

**Castle device lost = race with MoltenVK's asynchronous encoding.** After
bcda5dee it is deterministic with `--fable2_query_force_visible=false
--fable2_query_scene_depth=true`: 4 of 4 runs lose the device ("Invalid Resource")
12.5 s after the castle prompt (first castle frames). Each setting alone: 1 of 1
fine; defaults: 6 of 6 fine. With `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=1`: 2 of 2
fine; with `MVK_CONFIG_DEBUG=1`: 4 of 4 fine (correction, session 3: in this
MoltenVK debug mode does not retain resources, it only adds encoder error status
and logging; that result was a timing change). So something the render thread changes or frees after
`vkQueueSubmit` returns, but before MoltenVK's thread has encoded that command
buffer, is gone by then. The fence-delayed destruction does not cover that window.
Suspects, in order: the shared bindless descriptor set (slots nulled / republished
while an earlier submit is still being encoded), helper-blit framebuffers/views
(`HelperBlit`: the scene-depth copy into the query pass), query pools. Next:
make the descriptor updates wait for the previous submit's encode (or keep a
per-frame copy of the slots that changed), then re-run
`tools/smoke_test.py --runs 4 --route "A:5500:1500" --args
--fable2_query_force_visible=false --fable2_query_scene_depth=true`.

Open as well: castle white glow (real counts + scene depth don't remove it; not
ZFail/Total, not hold); the upside-down picture during the lost-controller pause
in mnk mode (user: skip).

## 2026-11-06 (session 3): castle device lost fixed, castle haze = sun glare

**Castle device lost (fixed, be755ec2).** MoltenVK 1.4.3 (pinned 701747d)
uses a Metal residency set: every texture and buffer is added on creation and
removed + committed on destruction, and the set is attached to the queue, so it
covers every command buffer. With `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0`,
MoltenVK commits the Metal command buffers on its own thread after
`vkQueueSubmit` / `vkQueuePresentKHR` return; the render thread meanwhile
destroyed retired objects. A command buffer committed in that window listed a
freed allocation and Metal failed it ("Invalid Resource"; the first to fail was
MoltenVK's present command buffer, which uses none of our resources). Fix: a
fence-only `vkQueueSubmit` after each present marks the queue tail; retired
textures, buffers, views and framebuffers (after the existing deferral) are
destroyed just before the next submit once that fence has signalled with
nothing submitted since, or after a mid-frame readback wait; a forced wait if
they are 8 presents old. cvar `fable2_destroy_when_queue_idle` (default on).
Repro (real counts + scene depth): 4 of 4 lost before, 4 of 4 fine after; the
3.7 ms async-submit gain stays. If it ever comes back: Xcode is installed, so
MoltenVK itself could be patched (commit command buffers under the residency
set lock, release removed allocations after the next command buffer completes).

**Castle white haze = the sun glare (fixed, 3f8ed1ad; user confirmed).** The
two additive quads (vs b1b160f8, ps a8eaa0d5 = `g_SunGlareTexture *
g_SunColour`, depth off) take their strength from two queries the game reads
once per frame: callers 0x82279A74 (`sub_82279410`) and 0x821D8DA4
(`sub_821D8658`) compute samples / (r^2 * k), the visible part of the sun disc;
not ready keeps the old value. The game reads them before that frame's count is
in, so with forced visible (default) or the stale rule they always got
1280x720; the real count in the castle hall is 0 (night: the sun is below the
floor, which is why the glare sat low on the screen). Now: GetData from those
callers marks a sun query (`fable2_query_sun_counts`, default on): host counts
even with `fable2_query_force_visible`, a stale read gets the same query's
previous count, and 0 until its first count is in (its first uses were skipped
on the host, and the game reissues a query only once it is answered: without
that the sun queries never reached the host in default mode).
`fable2_query_scene_depth` now defaults to on (the sun box needs the real scene
depth; the flat plane would hide the sun outdoors). Castle frame time unchanged;
castle exposure readback ~0.41 -> ~0.27 (no glare light). To check outdoors:
the sun glare should now fade behind buildings instead of always drawing.

Tools: `XenosRecomp <dump file or dir> <out> <shader_common.h> --hlsl` turns
captured shaders into readable HLSL (constant names included), e.g. to see what
a draw is (the glare shader's names gave it away).

**Real counts play test (user, 2026-11-06, `--fable2_query_force_visible=false`):**
night street: no popping, no dark ground flicker; day: sun glare fades correctly.
But one in-engine cutscene in Old Town (Rose: "for five gold we could eat for a
week") showed a black picture with subtitles at 58 fps, back to normal after
the scene. Exposure readback stayed normal (avg 0.2-0.4) and 2000-3000 draws ran,
so the scene was drawn and something late turned it black. Not the sun formula
(the game clamps it, NaN included). Not known yet whether defaults show it too.
Next time: in that scene, `diag_now` for a per-draw frame, then cvar_now
`fable2_query_force_visible=true` live (picture back = real counts). Parked by
the user; real counts stay off by default until then.
**2026-11-13: real counts are the default** (`fable2_query_force_visible=false`,
`fable2_query_hold=2`). Queries no longer end render passes (a2c79cda), so
culling is now a net win: town (card 9) 2504 -> 2230 draws, frame 15.7 -> 14.3 ms
standing and 17.1 -> 15.1 ms panning, GPU up to -2.8 ms. The user played the
prologue to Bowerstone: no popping or blinking, no black frames (the Old Town
cutscene above included).

**Full clears as render pass load actions: tried, no gain, reverted** (2026-11-06).
plume held a full clear back until its framebuffer's pass began and began it
with a LOAD_OP_CLEAR variant of the render pass (colour and depth merged).
Findings: (1) on MoltenVK a render pass with a CLEAR load action and no draws
left the colour target uncleared: fresh textures kept garbage, the intro videos,
menu and world broke (magenta/green blocks); passes followed by draws were fine,
so clear-only passes have to stay LOAD + vkCmdClearAttachments. (2) With that,
~30-50 clears per frame in the world became load actions, but frame times did
not move (Bowerstone still 20.85 vs 20.9 ms, castle 24.3 vs 24.2, 2 runs each):
the "up to ~2 ms" estimate does not hold on the M4 Pro. Not worth the plume
complexity; the next perf candidates are the Z-pass pre-run and the zero
multiplication cost.

**Zero multiplication cost (2026-11-06).** Separate runs differ by up to ~2 ms
for the same settings, so A/B now happens inside one session: a toggler writes
`cvar_now` every 10 s, and each `[perf]` line (every 120 frames) is matched to
the state before it (first line after a switch dropped). In-session:
`fable2_zero_mul` off = GPU 18.77 vs 20.74 ms (main pass 11.00 vs 12.71, same
draw counts). But cheaper exact or near-exact forms saved nothing: NaN-select
products (2 ops instead of 5) 20.82 vs 20.86; dot products that apply the zero
pairing only to non-finite results 21.03 vs 21.04. So the 2 ms is not the
select overhead; most likely, without the zero rule, 0 * infinity products
turn into NaN and drop geometry or fail depth tests (less work because things
are missing). Kept as is; not a perf lever.

**Shaders with more than 32 registers** (2026-11-06): vs db2acd98 (dumped in
the castle runs) uses r32-r34; the translator declared only r0-r31, so it did
not compile and its draws were dropped. Now r0-r63 are declared (unused ones
cost nothing).

## 2026-11-07: toward stable 60 fps (user goal: 60 at all times, current resolution or better, better draw distance)

**Render passes are the main GPU cost.** Tracer (`[passes]`, diag `[pass]` lines):
~60-80 render passes per frame, 22-36 on 2+ Mpixel targets; each big begin/end
loads and stores the whole attachments (2240x1536 RGBA16F 2x MSAA + depth ~80
MB each way). Fixed: lazy clears (eccf1964), -1.1 ms GPU in-session.

Still split (card 3 / card 2): the scene is drawn twice per frame (Xenos
predicated tiling, 2 tiles, ~1900 draws each); copies/resolves and sampling
after drawing end passes (real dependencies).

**MSAA off** (`--fable2_guest_msaa=false`): -3.6 ms GPU (Bowerstone,
alternating runs), 19.5 -> ~16-17.7 ms on card 3. Blocked by rectangles of
wrong lighting on the ground (user saw them; reproduced on card 3). Facts:
MSAA off + `fable2_zpass_emulation=false` -> no rectangles; MSAA off +
`fable2_zpass_restore_depth=false` -> no rectangles; MSAA on -> none. Not the
cause: depth redirect to atlas (surf.msaaSamples guard tried), blended draws in
the Z pre-pass (skip tried), the half/quarter scene-depth copy (not used on
card 3). Next: find what the restore (copy of the saved depth back into the
scene depth after the pre-pass resolve) changes that the lighting reads when
the scene depth is single-sampled (does anything sample the scene depth surface
image itself, not the resolved texture?).

**Other levers:** real occlusion counts (castle draws halve, -2 ms; blocked by
the black cutscene); drawing the scene once instead of per predicated tile
(vertex work + a big pass per tile).

Test saves: cards 2 and 3 (`tools/smoke_test.py --card 2` default, `--card 3`);
logs/smoke/ref/world.jpg is now the card 2 view.

**2026-11-07 later: results so far** (all pictures unchanged; in-session A/B
with tools/ab.py unless noted):
- Z pass depth-only (a345734b) -0.7 ms GPU; re-record the scene depth clear
  instead of save/restore copies (ad268428) -1.1 ms.
- Per-pass GPU timing (`fable2_diag_pass_timing`): the scene colour pass and a
  blended pass (terrain layers / lights) dominate.
- XenosRecomp forward-only control flow without the loop/switch state machine
  (f8446496): terrain layer shaders 3.95 -> 1.78 ms.
- tfetch2D skips the offset math for zero offsets (fbb4432b): ~-1 ms.
- Card 2 went from ~15 to ~11.8 ms per frame; card 3 GPU ~19 -> ~16.6 ms.
- Early Present release (game thread resumes after the frame's submit): -1.1 ms
  but flicker (menu, game) -> off by default (83599c49). Needs the render
  thread's end-of-frame work made safe against concurrent capture.
- Pipeline precache (269ccd81): never started in Fable 2; EoT records skipped;
  real sample counts recorded; dynamic stencil reference. Walking soak (second
  session): worst in-world frame 774 -> 82 ms, pipeline builds in slow frames
  179 -> 43.
Open (stutters while walking, card 3): ~15 frames per 90 s of ~28 ms where the
game thread's capture takes 13-25 ms instead of ~5; a few 44-60 ms frames with
long render-thread draw time or a GPU spike and no pipeline builds; first
frames after a load (uploads + pipelines).

**2026-11-07 afternoon: vertex work** (Metal System Trace via
`/Applications/Xcode.app/.../xctrace record --template "Metal System Trace"
--attach <pid>`, table `metal-gpu-intervals`: Vertex/Fragment time per encoder).
- Half the GPU time on card 3 was resolution-independent (`--eot_render_scale=0.5`
  only took the scene pass 9.6 -> 7.5 ms). The scene colour pass spent 3.4 ms in
  its vertex phase vs 1.1 ms for the same draws in the position-only Z pass.
- Cause: `xeDebugVertex` (a RawBufferStore behind a runtime branch) was emitted
  into every vertex shader. A vertex function that can write memory loses the
  vertex fast paths on Apple GPUs. Now only with `XE_DEBUG_VERTEX` (7b95b809):
  colour pass vertex 3.4 -> 0.75 ms, vertex time -40% overall, GPU ~16.5 -> ~13
  ms per frame, picture unchanged.
- Not levers (measured): real occlusion counts (draws -10% but GPU +1.6 ms: the
  host queries split render passes); keeping the Z pass depth for the scene
  (-0.3 ms, Apple's hidden surface removal already does it); trimming
  interpolants (`fable2_diag_interp`: 4.46 written, 4.46 read per vertex).
- Hottest vertex shader: the landscape grid (ffdbabd9eaa1c48c, 26% of vertices,
  4 vertex texture fetches per vertex).

**2026-11-07 afternoon: CPU critical path** (xctrace "Time Profiler" attached,
table `time-profile`, aggregated per thread). With the GPU at ~13 ms the frame
is set by the game's own "3D Engine" thread (100% busy: the game's renderer plus
our CaptureDraw, ~35% of it); the game thread waits (swtch_pri 74%).
- Vertex pulling copied each pulled draw's whole guest buffer: ~100 MB/frame,
  2/3 repeats of an unchanged buffer. Now a whole-buffer mirror cache for pulled
  stream slots with an unlocked stream object (f4b32a9d); never-unlocked
  buffers (written in place) and bone palettes are still copied per draw.
  `fable2_diag_pull` checks every mirrored use against guest memory (0 stale in
  a 60 s walk). Wall 19.1 -> 17.7 ms in-session, capture 5.2 -> 3.9 ms.
- Landscape patch grid indices cached (3cabe81b): 7.3 MB/frame -> 20 KB, small
  CPU gain.
- Remaining capture cost (profile): CaptureDraw itself ~7%, GetInputLayout 4%,
  GetVertexMirror 3.8%, ReadFetch 2.6%. The Audio Worker thread spins on a full
  core (trylock/WaitMultiple), harmless while cores are free.

**Open: shadows switch on/off with the camera yaw (after the prologue,
rightmost save card = `--card 9`).** User: shadows appear/disappear when panning
left/right; differs from the Bowerstone Z-pass bug. Seen in a pan video: at the
same camera angle on two sweeps (looking at the lake, dog in front) the shadow
over the foreground grass switches off in one frame and stays off.
Ruled out: Z-pass emulation (runs every frame there: 3 skips in 3641), shader
cache misses (0), dropped draws (only point sprites), occlusion queries (sun and
exposure queries forced visible: same shadows at matched angles; note all host
query results there are 0, "sum 0"). Next: count the shadow-map draws (2048x2048
depth atlas) per frame at the moment of the switch - if the casters change it is
the game's caster/cascade selection, if not it is our shadow-map or mask replay
(atlas viewports/scissors, cascade constants).
Tools: window-only bursts (`screencapture -l <winid>` in a loop, never a
full-screen recording: it captures the user's other windows), frames matched
across runs by the sky/mountain band, ground-brightness jump scan.

**2026-11-07 evening: stalls while walking fixed.** Walks (90 s, `--walk`):
card 3 p50 11.7 / p99 13.9 ms, rightmost save (`--card 9`) p50 12.0 / p99 13.2
ms, no in-world frame over 25 ms on either (was 19 on the rightmost save).
- Async pipeline builds (ce728fdf): first-seen pipelines of depth-tested draws
  build on a background thread (a 2.2 s freeze after the prologue).
- Synthesized input layouts cached per shader (c7a81a27): capture 3.9 -> 3.2 ms.
- Chunked submits (75cbdd1a, `fable2_chunk_submit_draws` 600): present-wait
  3.6 -> 1.9 ms, fence 1.9 -> 0.5 ms (same view).
- Realtime budget of the rendering threads 5 -> 14 ms (c86ddc51,
  `fable2_rt_budget_ms`): the kernel demoted the game's rendering thread.
- Writeback readback buffers pooled (6e2c1278): retired per resolve, they made
  FlushRetiredLocked wait for the whole GPU queue every 8 presents (30-70 ms
  every ~3.7 s). Found by elimination (`fable2_resolve_writeback=false` -> 0
  spikes), then the `[gpu] retired objects destroyed ... (N waited)` line.
- Still open: vertex-mirror pool growth (16 MB chunks) on the game thread in new
  areas (one 49 ms capture seen); loading-screen hitches; shadows switching
  with camera yaw (above).

**Card 2 (Bowerstone, day) is GPU-bound in pans** (walk: p50 13.9, p99 17.0,
42 of 75 two-second intervals with a frame over 16.7 ms, none over 25; GPU 14.6
ms average, up to 16.7). Metal System Trace (`--still 5 --soak 60 --card 2`;
wait for "1xxx draws", its standing view has ~1770): pixel work ~12.8 ms a frame
in ~220 encoders: 3 big passes ~6 ms, ~50 medium/small ~6 ms, ~170 tiny ~0.7 ms.
GPU timing on/off (`fable2_gpu_timing`, new) makes no difference.
Next GPU lever: the scene target is guest k_2_10_10_10_FLOAT (7e3, 32 bpp) hosted
as RGBA16F 2x MSAA (64 bpp) at 2240x1536; every big pass loads/stores it. A 32-bit
host format needs plume formats (no R11G11B10/RGB10A2 in RenderFormat), resolve
conversions, and the 2-bit alpha: draws into it write all channels (mask 0xf);
whether anything reads that alpha is not known yet (the diag line lacks blend
factors).
Diag frame (`--diag-world` / eot_diag_frame) loses the device on card 2 at the
diag frame (also with chunked submits off; pass timing alone is fine): per-draw
GPU timestamps split render passes on Apple GPUs; suspect GPU watchdog. Use
`fable2_diag_pass_timing` without the diag frame, or Metal System Trace.

**Card 2 resolve copies** (`fable2_diag_resolve_copies`, `[rcopy]` lines; GPU
timing now splits resolve-copy / resolve-blit / resolve-mirror): resolve copies
~1.5 ms/frame. Biggest: two 2048x2048 depth copies a frame (the shadow maps,
8.4 Mpx). Neither zero-copy path takes them: the hand-off excludes depth, and the
depth redirect (render the pass into the destination) needs (a) a destination
larger than the surface (atlas) and (b) the same GuestTexture object 16 frames in
a row, but Fable 2 makes a new texture object for the shadow map every frame
over the same host image (`p.mirror == dest_ref` false, texture pointer equal),
and the resolve's redirect hit also compares objects. Fixing it means matching by
host image in both places; not done (shadow path, ~0.5-0.75 ms).
Also measured on card 2 and left as they are: zero-multiply emulation off -0.3 ms
GPU (kept on: correctness), 32-bit 7e3 scene -0.34 ms (`fable2_7e3_32bit`, off).
Shadow investigation tools (2026-11-07): `fable2_diag_shadow_mask` (the mask,
ps 383793fa8d5a0716, always samples this frame's shadow maps: 0 stale in a pan;
per-frame `[casters]` counts: 80-160 with camera yaw; `[shadowcasters]` census),
`fable2_dump_shadow_maps=N` (logs/smap_f*_s7_2048x2048.ppm, s13 = Z-pass scene
depth), `fable2_debug_shadow_skip_vs`, `fable2_debug_shadow_skip_tess` (no
landscape patches are drawn into shadow maps). The game draws all shadow casters
with GpuLoadShaders(vs, null): no pixel shader (vertex-only load, type 5), so
leaf cards cast solid quads on the console too. Waiting for user screenshots of
a patch (same angle, with and without).
A one-off boot fault (guest read of 0x10 at +0.75 s, pink window, 39M log lines,
6 GB log) did not reproduce in two launches.

**2026-11-07 late:** user decision: keep 2x MSAA (no TAA / MSAA-off default).
The MSAA-off ground rectangles no longer reproduce on card 3. Geometry pools
make their next chunk ahead in the background (26b5bc94): no capture stalls
walking into new areas. Depth hand-off for the shadow maps (~0.5 ms on card 2)
not done: the depth paths (draw targets, partial clears, depth resolves) do not
handle Content::Borrowed (only colour paths take back), so a depth hand-off needs
take-back in those places. Run-to-run variation is ~1 ms (machine state), so
compare in-session (tools/ab.py) or several runs.

**User's Xbox 360 comparison (2026-11-07, rightmost save start):** the original's
tree shadows show branches (cut-out); ours are solid rectangles -> our bug. The
shadows popping in and out with camera moves happens on the console too (the sun's
shadow region follows the camera) -> original. Fix plan (after the 60 fps work):
after GpuLoadShaders(vs, NULL), depth-only draws keep the last loaded pixel shader
when it kills pixels (texkill / alpha test), as Xenos runs it in depth-only mode.
**FIXED 2026-11-07 late** (see "Visual fixes" below): not the missing pixel shader;
the alpha test and alpha to mask of the leaf cards' casters.
Depth hand-off for the shadow maps (tried, reverted): the 2048x2048 shadow
surface is D32_FLOAT_S8_UINT (stencil) and its destination texture is not a
committed allocation while the surface is (`committed false/true`), so the
hand-off gate (same flags/dimension/committed/pool) can never pass; the
GuestTexture is also often not in s.textures at the resolve (`dest_ref` false).
Needs the shadow textures allocated like the surface first.

**Known bug (parked): pixelated light effects.** Sun rays and light glows look
low-res with stair-stepped edges (user, Bowerstone town at sunset). Likely the
low-res light buffer magnified with point sampling: check XenosRecomp's fetch
filter override (SAMPLER_POINT when the instruction's mag==min filter is point;
`fable2_fetch_filter_override`) and the buffer's sampler. Live test:
`fable2_fetch_filter_override=false` via cvar_now.
**FIXED 2026-11-07 late** (see "Visual fixes" below): blur tap offsets, not the filters.

**Bowerstone town (rightmost save, user re-saved there): ~17.2 ms standing.**
A mix of limits; each lever alone moves it little (separate runs, ~0.5 ms
variance): half render scale -1 ms; PGO (FABLE2_PGO=gen/use in CMakeLists, %c
continuous profile; 3 training runs) -0.3..0.5 ms, left off; 3 frames in flight
(kNumFrames=3) no gain, reverted; GPU timing off 0; skipping the 256x256 colour
resolves (35/frame, 29 never sampled: `fable2_diag_dead_resolves`,
`fable2_debug_skip_small_resolves`) 0. The game's 3D Engine thread is ~97% busy
(our CaptureDraw 26%: GetMirror 5.8%, pull copies 4.6%, constants 2%); the game
thread spins on it. 3.2% of that thread is rex's BaseHeap::QueryRegionInfo under
Lionhead's command-buffer writer (sub_822192E8): SDK memory path, not ours.
Metal System Trace caveat: it induces the GPU's minimum performance state and
adds ~190 "Record GPU Counter Sample" blit encoders a frame (its own), so its
idle gaps and durations are not representative.
Small fixes: the pop counter is off unless fable2_pop_counter (smoke test turns
it on); DiagShouldLog reads the clock every 64th call of a site.
Shadow pass draw order (`fable2_diag_shadow_seq`, card 3): every shadow-pass
draw is vs e4d8abb13de22341 with no pixel shader: the GpuLoadShaders state is
"none" (owned 0x1) and the device's pixel shader pointer is 0 too. So the game
does not load a cut-out shader for the shadow pass; "keep the previous pixel
shader" is not the fix. Next: in a tree-heavy spot, find which draws put the
canopy quads into the shadow map (skip by vertex shader, dump the map) and
compare with what the console shows (branches only?).

**What limits Bowerstone town (in-session A/Bs, card 9 standing):**
- +2 ms busy-wait on the game's rendering thread (`fable2_debug_present_spin_us`):
  wall +0.8 ms -> that thread has ~1.2 ms of slack.
- All shadow casters skipped (`fable2_debug_shadow_skip_vs`, the 4 caster VS):
  shadow GPU -0.73 ms, wall -0.47 ms -> partly GPU-bound.
- 256-wide colour draws skipped (`fable2_debug_skip_256_draws`): -0.14 (noise).
- Ring hook: the CP_RB_WPTR read now goes through MMIOHandler::CheckLoad (no
  faulting load; it reads 0 either way in this renderer), the read-pointer
  write-back is stored only when it changes, the ring opcode scan is
  `fable2_ring_opcode_log` (off): no measurable change.
So town is balanced: GPU (vertex/draw-heavy, half resolution only -1 ms) and the
game's rendering thread (game code ~12 ms + our capture ~4.3 ms) are both near
17-18 ms. A locked 60 there needs ~1-1.5 ms off both at once.
- Pulled data byte-swapped on the CPU while copied (`fable2_pull_preswap`,
  table endian | 0x10 = already swapped; shader unchanged otherwise): town
  18.16 vs 18.11 ms, capture +0.26 ms -> off. Skinning is not ALU-bound on the
  swaps. Town pulled copies (`fable2_diag_pull`): 36 MB/frame, 20 MB of it
  buffers the game rewrites between draws (per-character scratch data), so no
  dedupe.

## 2026-11-07 (late): visual fixes (60 fps paused by the user)

**Pixelated sun rays / light glows: FIXED.** The bloom and sun-ray chain runs
at 280x180 guest (560x360 host at render scale 2): sun glare + sun disc
(acb18ee7, 7d59afca), downsample (2ac2d68d), radial blur x2 (1bcfdf20, 14
point taps), composite (6b9e2c3d: bloom + radial blur), then a separable
13-tap Gaussian (4ac75548 horizontal, 83303fe7 vertical) whose taps are
**fetch-instruction offsets** -5.5..6.5 texels with point filtering; the
tonemap (27890441) magnifies the result bilinearly to the screen. XenosRecomp
divided instruction offsets by the HOST texture size, so on scaled render
targets the blur covered half its kernel and skipped every other texel: the
bloom buffer kept hard, blocky edges (dump `f*_r24_dest_*`). Fix: pixel
shader 2D fetches with offsets call `tfetch2DOffset(..., slot)` and
`getWeights` calls `getWeights2DGuest(..., slot)`, both in guest texels via
`g_TexHostScale(slot)` (no change for unscaled textures). After the fix the
bloom buffer is smooth and the glows have no stair-steps. Light test spot:
rightmost card (5), turn the camera ~180 degrees and slightly up.

**Tree shadows as solid rectangles: FIXED.** In the shadow pass (1024x1024
guest, depth only) the leaf cards are vs 3f5adb0f + ps b72ab2df with
RB_COLORCONTROL 0x...1c: alpha test GREATER, ref 0, plus alpha to mask. The
shader's alpha test is `clip(alpha - ref)` (GREATER_EQUAL), so alpha 0
passed, and alpha to mask was only enabled with colour targets, so nothing
cut the transparent texels. Fix (`fable2_alpha_exact`, default true, live
toggle): GREATER adds 1e-6 to the threshold; alpha to mask also applies to
depth-only draws (shadow casters, emulated Z pass). Verified on card 4 (the
Gypsy camp bridge, tree-heavy) with `fable2_dump_shadow_maps` +
`fable2_diag_shadow_mask` in an off/on/off sequence: solid leaf polygons ->
leaves and branches -> solid again. The other tree caster (vs bd71193f, no
pixel shader, back-face culled) is solid trunk/branch geometry, correct.


**Status 2026-11-08 (user):** both visual fixes confirmed in game. Closed: the
random black/patchy dog coat at load and the pink window at boot no longer
happen; performance is done ("we've done enough"): 60 fps everywhere except
the busiest card 2 views and busy Bowerstone town (~55-58 fps, no stutters);
the rework options above are not planned. On hold: shadow popping with camera
moves (original behaviour too); resume plan: capture both positions (diag
frame + shadow map dump), then depth clamp for shadow casters if the draws are
issued but clipped, or the game's culling if they are not issued.

**Cave water stripe / squarish water: FIXED 2026-11-08.** Each water body has
a small k_8 mask (16x4 .. 64x64; 1 = water) that the water pixel shaders
(e33859f0, b9b8beca, 3360a2eb, e9b723e0; slot 1, clamp to edge) use to kill
pixels outside the pool. At area load the game blurs each mask 3x in place:
screen rects (ps 05e3bdd0, sample offsets/weights from constants) into a
small surface (0x406f8610, k_8_8_8_8), resolved back into the mask. The
rects' edges sit on guest pixel centres (the D3D9 half pixel: posOff
1/64 for 64x64), which covers every pixel on the console but at 2x left the
first host row and column uncovered (black); the blurs spread that black
inward, so the masks faded on the left/top and stayed at full water on the
right/bottom, and the water ran past the pool as a straight stripe. Fix:
`fable2_native_small_surfaces` (default 64): colour targets <= 64x64 render
at 1x. Verified on card 4 (cave save): mask edges clean, stripe gone.
Debug tools added: `fable2_debug_log_rt_size=N` (+ `fable2_debug_log_rt_skip_ps`)
logs full diag lines for draws into NxN targets; `fable2_offsets_guest_texels`
(default true) switches the light fix's guest-texel offsets back for A/B.

**Water slab after reloading a save: FIXED 2026-11-08.** Repro (user): pause
-> Save -> Quit without saving -> confirm -> load the same save; from the 2nd
or 3rd load the cave water covers whole tile rectangles. Each load the CPU
writes the masks (Lock/Unlock) at new or reused addresses and blurs them;
`resolve_ranges` remember every range any resolve ever wrote, so a fresh mask
texture there was taken over as a black render target
(`TakeOverAsResolveTarget`) and the blurs blurred black (mask all 0).
Fix: `fable2_unlocked_no_takeover` (default true): a texture the CPU unlocked
is uploaded even in resolve memory; the resolves adopt it as usual. Verified
with a scripted 3-load run (tools: scratchpad reload_drive.py; menu path START,
DUP, A, DDOWN x2, A, A): without the fix the slab shows on load 2 and the
mask is empty on load 3; with it, loads 2 and 3 are clean.

**Missing shaders (black / missing geometry in new areas): FIXED for good
2026-11-08.** The built-in shader cache only holds shaders captured before the
build (`shader_dump`); a shader met for the first time dropped its draws
("shader cache miss": the black cave floor, 297 drops in one session). Now
`src/gpu/shaders/runtime_shaders.cpp` translates them while the game runs: on a
cache miss at registration the container goes to background workers
(`fable2_runtime_shader_workers`, 3) that run XenosRecomp in its new `--blob`
mode (one binary file: entries, vertex layouts, SMOL-V; 'F2SC' v1); the result
is published to a locked runtime table (`FindShaderCacheEntry` falls back to
it; `EntryBytes` / `EntryVertexLayouts` use its blob) and attached to the
shader at its next draw. Files are kept in `shader_runtime/` next to the
executable (`fable2_runtime_shader_dir`) and loaded at startup. Translator and
header default to the ones the build used (`FABLE2_XENOSRECOMP_EXE`,
`FABLE2_SHADER_COMMON_H`; cvars `fable2_xenosrecomp`, `fable2_shader_common`;
for a packaged build put XenosRecomp and shader_common.h next to the
executable). `fable2_runtime_shaders=false` turns it off;
`fable2_debug_ignore_builtin_shaders=<hashes>` makes built-in entries count as
missing (test). Verified on card 4 (the user's cave save) with the 7 cave
shaders ignored: off -> 117 drops, black floor; on -> 7 translations in
0.25-0.77 s each, floor drawn; second launch -> 7 loaded from disk, 0 drops.

## 2026-11-13 evening: towards 100 fps in the Gypsy Camp (card 9)

The user's target: at least 100 fps (10 ms) in the heaviest place on an M4
Pro. Card 9 was re-saved in the Gypsy Camp (busier than Bowerstone: more NPCs).
`[perf]` now reports the game's rendering thread CPU time per frame (`game`),
and `tools/ab.py` averages it, so CPU-side changes show even when the GPU sets
the frame time. `tools/gputime.py` gives real GPU utilisation (ioreg).

**Where it stands** (pipelined Present on; standing still):
- Full resolution: 18.2 ms a frame, GPU 98% busy: GPU-bound. GPU split: main
  scene pass 7.8 ms, depth-only (shadow maps 2x, prepass, query boxes) 2.4-2.8,
  resolves ~3-3.5, 8-bit targets ~2.8.
- Half resolution: 16.1 ms with the GPU 74% busy and the game's rendering
  thread at 12.7 ms of CPU: neither saturated. The rest is waiting between the
  game's logic thread, its rendering thread, our render thread and the GPU.
  Next wall after GPU savings: ~16 ms.

**Done today**: pipelined Present with the upload ring per captured frame
(town 14.4 -> 13.4 ms); skinning emulation decodes each bone row once (-0.3 ms
CPU); ring constant writer hooks skip their memory query and packet walk
unless mirroring (-0.45 ms game thread CPU); morph stream statistics only with
`fable2_diag_morph`.

**Measured and dropped** (in-session A/B unless noted):
- Reusing pulled-buffer copies within a frame: the Gypsy Camp pulls two 64 KB
  stream-2 buffers ~600 times a frame (instancing: element = (vertex + c255.x)
  * k; 37 MB of copies), changed by a few bytes between some uses. Comparing the
  bytes costs as much as copying (no gain); page write watches
  (EnablePhysicalMemoryAccessCallbacks) fault 180-400 times a frame (16 KB host
  pages). The real fix is copying only the slice a draw reads: the translator
  would have to give the index formula's range (~0.8 ms CPU).
- Skipping the scene's depth prepass: -0.16 ms GPU only (depth-only is cheap
  on Apple GPUs); not worth its correctness risks.
- `fable2_zero_mul` off: -1.4 ms, but as found on 2026-11-06 that is geometry
  lost to NaNs, not a lever.
- 3 upload ring slots: occlusion query counts flickered while standing still
  (cause not found); 2 are kept and suffice for pipelining.
- Control flow: only 10 vertex shaders still use the loop/switch state machine.

**Options that change the picture** (user's decision): shadow maps at 1x (the
Xbox's 1024, now 2x): -1.3 ms (-0.9 panning), mostly their resolve copies;
temporal upscaling (TAA reconstructing to full resolution from ~70-75%): the
TAA keeps its history at scene size today, so this is new work.

**Large projects that remain**: plume's Metal backend instead of MoltenVK
(memoryless MSAA attachments and explicit load/store: the main pass's fixed
MSAA load/store cost); moving capture work off the game's rendering thread
(our render thread idles ~4 ms a frame); hand-off instead of copies for the
2048x2048 shadow maps (blocked before by per-frame texture objects).

## 2026-11-14: recompiler options (registers as locals)

The game's rendering thread spends ~9 ms a frame in recompiled code in the
Gypsy Camp, every PowerPC register a memory access. `Fable2_config.toml` now
turns on `skip_msr`, `ctr_as_local`, `xer_as_local`, `cr_as_local`,
`reserved_as_local` and `non_volatile_as_local`. Rendering thread CPU per frame
in the camp (card 9, standing still): 12.71 -> 11.66 (all but cr) -> 11.02 ms
(with cr); our capture work inside it stays 3.65. Cards 2, 3 and 9 pass with
recordings, 0 errors. The frame time at full resolution does not move yet: the
GPU sets it there.

What the options needed (each found as a crash or a black world first):
- SEH funclets that read or change their owner's non-volatile registers need
  `share_registers`: 40 entries, listed by `tools/nvshare.py` (the SDK's own
  detection misses funclets outside scope tables). `share_registers` on a
  normal function is wrong: the save/restore helper calls are gone, so the
  funclet syncs of other functions overwrite what it keeps in the context.
- The game loop hook read registers from the context: now a mid-asm hook that
  gets them as arguments (`Fable2GameLoopYield`, src/cpu_idle.cpp).
- Two functions preload v64+ and call four helpers that read them and leave
  results there: 38 mid-asm hooks hand the registers over
  (src/vector_share.cpp; found by `tools/nvshare_fv.py`). Symptom: world black,
  13,000 draws.
- Condition flags are never shared across functions (`tools/crshare.py`: 0 at
  entry; the only reads after a call follow an error raise that does not
  return).
- The analysis scripts split blocks at every branch (a first version did not
  and could miss reads at a branch target; the result was the same set).

Left off: `non_argument_as_local` (r0, r2, r11, r12, f0, v32-63). ~120 funclets
get their owner's frame in r12, and the option has no per-function exception;
it needs a hook pair per funclet call or an SDK change. `skip_lr` stays off:
hooks tell call sites apart by the link register.

## 2026-11-14: Gypsy Camp pass list (next GPU lever, not started)

`out/diag/run_card.sh tile_c9 9 0 -- --diag-world` + `out/diag/passes.py LOG`:
the camp frame has **140 render passes**. The scene is not drawn per tile
(Z pre-pass 755 draws, colour 755 + 596). 80 of the passes are ten identical
8-pass chains at the start of the frame: a 256x256 texture bake (1-2 draws
from three 1024x1024 sources into surface 0x407bea00/0x407be9a0 with depth),
a resolve to mip 0 of a texture, then three single-quad mip passes
(vs a108f279e72a396b, ps 2cb8bcd53db53c93) each followed by a resolve to mips
1-3 of the same texture. Every resolve is its own render pass (hand-off gate:
`whole_host false covers false`). Likely per-character work (ten NPCs).
To do: (1) do the same ten textures get baked every frame (then cache or
throttle)? (2) draw the mip quads straight into the destination mip (redirect)
or hand the surface image off: 80 passes -> 40 or fewer; (3) per-pass GPU cost
is ~50 us, so this is ~2-4 ms of the 18.
Also seen: **123 dropped draws a frame, "point sprites not supported yet"**
(vs 0x4cc113b0 / 0x4c139590), inside those chains: something the game draws
is missing from the picture; find out what.

## 2026-11-14: character texture mips on the GPU (camp -1.9 ms)

The ten (in fact ~12) 8-pass chains of the camp's pass list: each frame the
game renders ~12 characters into 256x256 `k_1_5_5_5` textures with 4 levels
(probably per-character shadow textures) and builds levels 1-3 itself. The
dest textures rotate through a pool; `fable2_diag_bake` showed every bake's
constants change every frame (animation), so caching is out.
`fable2_debug_skip_bake_chains` (cost test, picture wrong): skipping the mip
draws and the 4 resolves per chain = -2.2 ms GPU, about 26 us per small pass.
`fable2_bake_auto_mips` (default on, e22ff475): the capture side drops the
game's per-level draw (ps 2cb8bcd53db53c93) + resolve while the sequence
follows the exact pattern (anything else breaks the chain, logged as
`[auto-mips]`); the level-0 resolve's replay builds levels 1-3 with
`GenerateMipsLocked` (blit_ps mode 4: one bilinear sample of the level above,
rounded to 5-5-5-1 bits like the resolves). The game made levels 2-3 at 1x
scale and stretched them; ours are full resolution. Camp A/B 19.19 -> 17.32 ms;
cards 2, 3, 9: 0 errors, pictures unchanged.
Still per chain: the bake pass, the level-0 resolve (a pass: rounding to 5 bits
needs a shader) and our 3 mip passes. A compute pass building all levels would
save 2 more passes per chain (~0.6 ms); rendering the bake straight into level
0 would need look-ahead (the replay streams commands).
The "123 dropped point sprite draws" are the memexport passes (never visible)
and the night sky's stars (vs 867a1ee73bc31ea7 / ps e14feb99dd4adc46, 512
points, g_StarParams): stars are missing at night, not a cost.
