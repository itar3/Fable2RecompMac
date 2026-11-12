# Third-party notices

Fable2RecompMac is released under the MIT License (`LICENSE`). It contains or
builds on the following work, each under its own license. Keep these notices
when you redistribute the source.

| Component | Where | License | Copyright |
|---|---|---|---|
| [Fable2Recomp](https://github.com/Fable2Recomp/Fable2Recomp), the Windows recompilation project this port started from | the project as a whole (`LICENSE`) | MIT | 2025 RyzenDew and the Fable2Recomp contributors |
| [EdgeOfTimeRecomp](https://github.com/goliathret/EdgeOfTimeRecomp) native renderer, adapted to Fable II | `src/gpu`, `src/core`, `src/platform` (see `src/gpu/CREDITS.md`) | BSD 3-Clause, full text in `src/gpu/LICENSE.EdgeOfTimeRecomp` | 2026 goliath-tilers and the authors named in each file |
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) project template | `src/hooks.cpp`, `src/heap.cpp`, `src/heap.h` | BSD 3-Clause (see the file headers) | 2026 Tom Clay |
| [plume](https://github.com/zolaware/plume) | submodule `thirdparty/plume`, Fable II patches in `tools/plume-patches` | MIT | 2024 renderbag and contributors |
| [reeot-XenosRecomp](https://github.com/goliathret/reeot-XenosRecomp), a fork of [hedge-dev/XenosRecomp](https://github.com/hedge-dev/XenosRecomp) | submodule `thirdparty/XenosRecomp`, Fable II patches in `tools/xenosrecomp-patches` | MIT | 2025 hedge-dev and contributors |
| [dlmalloc](https://gee.cs.oswego.edu/dl/html/malloc.html) | `thirdparty/dlmalloc` | MIT-0 | 2023 Doug Lea |
| [RenderDoc](https://renderdoc.org) in-application API header | `thirdparty/renderdoc/renderdoc_app.h` | MIT | 2015-2026 Baldur Karlsson |

## Not included here

- **The ReXGlue SDK** (BSD 3-Clause, Tom Clay; derived from
  [Xenia](https://xenia.jp), BSD 3-Clause, Ben Vanik and Xenia contributors)
  is needed to build and is installed separately. It bundles further libraries
  (MoltenVK, SDL3, FFmpeg, Vulkan loader, fmt, spdlog, imgui, xxHash and
  others) whose licenses are in the SDK's `share/licenses` folder. Anyone who
  distributes a built game must include those notices too, and FFmpeg's LGPL
  has extra conditions for binary distribution.
- **The game.** No game files, recompiled game code or game shaders are part
  of this repository. You need your own copy of Fable II; the build
  recompiles and translates it on your machine.

## Trademarks

Fable, Fable II, Xbox and Xbox 360 are trademarks of Microsoft Corporation.
This project is not affiliated with or endorsed by Microsoft, Xbox Game
Studios or Lionhead Studios.
