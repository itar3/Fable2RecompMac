# Fable2RecompMac

A native macOS port of **Fable II** for Apple Silicon Macs. The game's Xbox 360
program is statically recompiled to native ARM64 code with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), and a native renderer
draws its graphics through Metal (via MoltenVK) instead of emulating the Xbox
GPU.

**You need your own copy of the game.** This repository contains no game
files, no recompiled game code and no game shaders: the build recompiles and
translates your copy on your Mac.

## Status: 1.0

The whole game is playable on Apple Silicon (tested on an M4 Pro, macOS 27):

- Native renderer at the window's full Retina resolution (or a chosen
  full-screen resolution), with the game's 2x MSAA kept.
- Stable 60 fps nearly everywhere with the 60 fps cap; the busiest Bowerstone
  views dip to about 55-58 fps.
- **Fable 2.app**, a launcher: windowed or full screen, window size or
  resolution, render scale, frame rate cap, VSync, power saving, and
  **Install from ISO** that sets the game up from your disc image, finds the
  matching title update and checks that the result is exactly the program
  this port runs before installing anything.
- Power saving: idle game threads sleep instead of spinning (about one CPU
  core less at the same frame rate).

## What you need

- An Apple Silicon Mac, macOS 14 or later.
- Xcode (for the launcher; its command line tools for the rest), CMake and
  Ninja.
- [ReXGlue SDK 0.10.0.24-dev](https://github.com/rexglue/rexglue-sdk/releases).
- Your own Fable II: a disc image (`.iso`) or a Games on Demand package, and
  **title update 1** for your disc's media ID.

### Which game versions work

The port runs one program: Fable II **0.0.1.33**, a disc's program with title
update 1 applied. The discs checked so far all hold the same base program and
become exactly that program with their own title update 1:

| Disc | Media ID | Disc version | With title update 1 |
|---|---|---|---|
| Game of the Year (Russian) / Games on Demand | `05FBECEA` | 0.0.0.33 | works |
| Game of the Year (English, Polish, Hungarian, Korean, Czech, Slovak, Chinese) | `716F0A0D` | 0.0.0.26 | works |

Other discs very likely work too; the launcher checks any disc and title
update you give it before installing.

## Getting started

1. Install the SDK, get the code, prepare your game files and build:
   [docs/SETUP.md](docs/SETUP.md).
2. Play with the launcher (or from Terminal), options and known problems:
   [docs/RUNNING.md](docs/RUNNING.md).

The native renderer's design and the Fable II specifics are in
[docs/NATIVE_RENDERER.md](docs/NATIVE_RENDERER.md); `MACOS_NOTES.md` is the
port's working log (findings, fixes and debugging switches).

## Known issues

- Shadows can pop when the camera moves; the Xbox 360 version shows similar
  popping.
- The first launch after a shader change stutters while Metal compiles the new
  pipelines; later launches are smooth.

## Support

If you'd like to support my work on this port, you can buy me a coffee on
[Ko-fi](https://ko-fi.com/itar3). It's entirely optional: donations support
the time spent on the port, don't unlock anything, and the port stays free.
The game itself is never sold or included.

## Credits

- [Fable2Recomp](https://github.com/Fable2Recomp/Fable2Recomp) by RyzenDew and
  contributors, the Windows recompilation project this port started from.
- [EdgeOfTimeRecomp](https://github.com/goliathret/EdgeOfTimeRecomp), whose
  native renderer this one is adapted from.
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (based on
  [Xenia](https://xenia.jp)), [plume](https://github.com/zolaware/plume) and
  [XenosRecomp](https://github.com/hedge-dev/XenosRecomp).

Licenses: [LICENSE](LICENSE) (MIT) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Disclaimer

Fable, Fable II, Xbox and Xbox 360 are trademarks of Microsoft Corporation.
This project is not affiliated with or endorsed by Microsoft, Xbox Game
Studios or Lionhead Studios. It does not include or distribute any part of
the game.
