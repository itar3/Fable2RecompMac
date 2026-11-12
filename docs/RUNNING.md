# Running Fable2RecompMac

Day-to-day use once the game is built: the launcher, Terminal, options and
known problems. Building it the first time is in [SETUP.md](SETUP.md).

## 1. Run

### With the launcher (easiest)

The build also makes **`out/build/mac-arm64-native/Fable 2.app`**. Double-click
it: a settings window opens (windowed or full screen, window size or
resolution, render scale, frame rate cap, VSync, power saving, frame rate in
the title). Press **Play** and the launcher starts the game with those choices
and closes; the choices are remembered for next time. You can drag the app to
the Dock or `/Applications`: it knows where the game and its files are.
`open "Fable 2.app" --args --play` starts the game at once with the saved
choices. The game's own output goes to `logs/stdio_launcher.txt`.

**Install from ISO…** sets up the game files from a Fable II disc image, and
the **Title update** row installs title update 1, which isn't on any disc:

1. The launcher reads the disc's game program (title ID `4D5307F1`, its media
   ID and version).
2. A base disc (version 0.0.0.x) needs title update 1. Click **Choose…** in
   the Title update row once and pick the folder with your title updates (for
   example `FableII_TU1` with one update per media ID) or a single update file;
   the launcher takes the one made for the disc's media ID.
3. Before extracting anything it loads the disc's program with that update
   and compares the result with the program this build was recompiled from
   (`fable2-extract --image-hash`, XXH3-128 of the guest image). Only an exact
   match is installed; otherwise nothing is changed. Every disc checked so far
   holds the same base program and title update 1 brings it to this build's
   program: the Russian GOTY / Games on Demand disc (media ID `05FBECEA`) and
   the multi-language GOTY disc with English (`716F0A0D`).
4. It extracts the disc into `assets/game.new` (about 7 GB, under a minute),
   adds the update (`default.xexp` next to the program, the whole update in
   `assets/update`), moves the old files to the Trash and puts the new ones in
   place.

If the game is installed without its update, the Title update row shows
**Install update**. The first time the launcher reads from your Downloads
folder, macOS may ask whether Fable 2 may access it: click Allow.
`open "Fable 2.app" --args --update-location <path> --install-iso <iso>` does
the same without dialogs.

Building the launcher needs Xcode (SwiftUI); without `xcrun` it is skipped.

### From Terminal

Always start the game from the build folder, and only one copy at a time:

```bash
cd <repo>/out/build/mac-arm64-native
pkill -x Fable2   # close a copy that is still running, if any
./Fable2 <repo>/assets \
    --fullscreen=false --window_width=1280 --window_height=720 \
    --fable2_shader_dump_dir=<repo>/shader_dump
```

- Drop the three window options to play fullscreen.
- After a white intro fade, the main menu appears about 50 seconds after
  launch. That's normal.
- **First launch after a shader change:** loading takes much longer and the game
  freezes for seconds at a time while Metal compiles the new pipelines. The
  next launch is smooth again (Metal keeps them cached).
- The window title shows the frame rate and the resolution the game renders at
  (`N fps - WxH (drawn at WxH)`). A Retina window renders at 2x.
- F8 toggles the on-screen overlay.

To also keep MoltenVK's own messages (useful after a freeze or crash), send the
output to a file:

```bash
./Fable2 <repo>/assets --fullscreen=false \
    --window_width=1280 --window_height=720 > logs/stdio_$(date +%H%M).txt 2>&1
```

### Where things are

| What | Where |
|---|---|
| Logs | `out/build/mac-arm64-native/logs/Fable2_<N>.log` (a new one per launch) |
| Saves | `~/.local/share/Fable2/B13EBABEBABEBABE/4D5307F1/` |
| Settings and caches | `~/.local/share/Fable2/` |

## 2. Useful options

| Option | Default | Meaning |
|---|---|---|
| `--fullscreen` | `true` | Fullscreen or windowed |
| `--window_width`, `--window_height` | | Window size |
| `--video_mode_refresh_rate` | `120` | `120` = up to 60 fps, `60` = the original 30 fps |
| `--eot_fps_limit` | `0` | Frame rate cap (`60`: steady 60 fps, less GPU work and heat; `0` = none) |
| `--eot_resolution` | `1080p` | Render resolution in fullscreen (`720p`, `1080p`, `1440p`, `2160p`, `display`, `native`) |
| `--eot_render_scale` | `1.0` | Extra scale on top of the render resolution |
| `--fable2_shader_dump_dir=<dir>` | | Save shaders the build doesn't have yet into `<dir>` |
| `--eot_perf_frames=<N>` | `600` | Write a `[perf]` timing line (CPU and GPU per frame) to the log every N frames |
| `--log_level=debug` | `info` | More detailed log |

## 3. Changing settings while the game runs

Write `name=value` lines to a file called `cvar_now` next to the executable;
the game applies them within a second and logs `[cvar_now] name=value -> set`:

```bash
echo "fable2_zero_mul=false" > out/build/mac-arm64-native/cvar_now
```

Other trigger files in the same folder: `touch diag_now` logs one frame of
per-draw detail, and `touch trace_now` logs one frame of D3D calls. More
debugging switches are listed in `MACOS_NOTES.md` ("Native renderer debugging
workflow").

## 4. Known problems

- Shadows can pop when the camera moves; the Xbox 360 version shows similar
  popping.
- The first start after a build stutters while the game's shaders are
  translated and Metal compiles its pipelines; both are cached for later
  starts.
- Avoid saving right before a scripted scene: a save made just before
  accepting Barnum's Image Capturing Device made that scene hang.
- If the picture ever freezes while the game keeps running, the log shows
  `renderer disabled` (the GPU reported "device lost"). Close the game and
  start again; your last save is safe. Starting from Terminal with the output
  kept (section 1) records MoltenVK's reason.
