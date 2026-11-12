# Setting up Fable2RecompMac

From nothing to a running game on an Apple Silicon Mac, building it yourself.
(Just want to play? The ready-made app does all of this by itself: see
"The app" in the [README](../README.md#the-app-easiest).) Once it is built,
day-to-day use (the launcher, options) is in [RUNNING.md](RUNNING.md).

You need your own copy of Fable II for Xbox 360 (a disc image or a Games on
Demand package) and **title update 1** for your disc's media ID. Nothing of
the game is included in this repository.

## 1. Requirements

- An Apple Silicon Mac (M1 or newer), macOS 14 or later.
- Xcode (for the launcher app) and CMake and Ninja:
  `xcode-select --install`, then `brew install cmake ninja`.
- About 20 GB of free disk space (game files, generated code, build).

## 2. Install the ReXGlue SDK

The port needs **ReXGlue SDK 0.10.0.24-dev (commit `bd833a2a`)**, the
`mac-arm64` release.

1. Download the `mac-arm64` package of that release from
   [rexglue-sdk releases](https://github.com/rexglue/rexglue-sdk/releases) and
   extract it, for example to `~/rexglue-sdk`.
2. Point `REXSDK` at it (add this line to `~/.zshrc`, then open a new
   Terminal window):

   ```bash
   export REXSDK=~/rexglue-sdk
   ```

3. Clear the download quarantine so macOS lets the tools run:

   ```bash
   xattr -dr com.apple.quarantine "$REXSDK"
   ```

Check it with `"$REXSDK/bin/rexglue" --version`; it prints
`0.10.0.24-dev.gbd833a2`.

## 3. Get the code

Clone with its submodules:

```bash
git clone --recursive https://github.com/itar3/Fable2RecompMac.git
cd Fable2RecompMac
```

(Already cloned without `--recursive`? Run `git submodule update --init --recursive`.)

## 4. Prepare the game files

The game files go under `assets/` in the repository folder:

```
assets/
  game/      default.xex, default.xexp, data/, ...   (the game + title update 1's patch)
  update/    default.xexp, data/tu1_data.bnk          (title update 1)
```

The build generates its code from `assets/game`, so this comes first. Later
you can reinstall or switch versions from the launcher instead (Install from
ISO in [RUNNING.md](RUNNING.md)).

1. **Build the extractor** (about a minute):

   ```bash
   cmake -S tools/sdk_tools -B out/sdk_tools -G Ninja -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_PREFIX_PATH="$REXSDK"
   cmake --build out/sdk_tools
   ```

2. **Extract the game** from a disc image, or from a Games on Demand package
   (a header file with no extension next to a `<name>.data` folder: pass the
   header file):

   ```bash
   out/sdk_tools/extract "/path/to/Fable 2.iso" assets/game
   ```

3. **Title update 1.** Use the one made for your disc's media ID (title
   update packages are named like `TU_16L61VH_...`). Extract it, then put its
   `default.xexp` next to `assets/game/default.xex` and the whole update into
   `assets/update/`:

   ```bash
   out/sdk_tools/extract "/path/to/<title update package>" /tmp/fable2_tu1
   cp /tmp/fable2_tu1/default.xexp assets/game/
   mkdir -p assets/update && cp -R /tmp/fable2_tu1/default.xexp /tmp/fable2_tu1/data assets/update/
   ```

### Which versions work

The port runs one program: Fable II **0.0.1.33**, a disc's program with title
update 1 applied. These discs become exactly that program with their own title
update 1:

| Disc | Media ID | Disc version |
|---|---|---|
| Game of the Year (Russian) / Games on Demand | `05FBECEA` | 0.0.0.33 |
| Game of the Year (English, Polish, Hungarian, Korean, Czech, Slovak, Chinese) | `716F0A0D` | 0.0.0.26 |

To check your copy before building, print the fingerprint of the program
with the update applied; it must be `97bf3532efcd6bb2281dc1196172056b`:

```bash
out/sdk_tools/extract --image-hash assets/game
```

The launcher does this check itself before every install from a disc image.

## 5. Build

```bash
"$REXSDK/bin/rexglue" codegen Fable2_manifest.toml   # after a fresh clone or a change to Fable2_config.toml
cmake --preset mac-arm64-native                         # once
cmake --build out/build/mac-arm64-native
```

Codegen prints `XEX patch applied successfully ... new version: 0.0.1.33`
when it finds the title update. The first build compiles the whole
recompiled game and takes several minutes; later builds take seconds to a
minute. The build makes:

- `out/build/mac-arm64-native/Fable2`, the game;
- `out/build/mac-arm64-native/Fable 2.app`, the launcher;
- `out/build/mac-arm64-native/fable2-extract`, the extractor the launcher uses.

## 6. Run

Double-click **Fable 2.app** (you can drag it to the Dock), choose your
settings and press **Play**. Everything about running, the launcher and the
options is in [RUNNING.md](RUNNING.md).

**First start:** the game's shaders are translated on first use and Metal
compiles its pipelines, so the first minutes stutter; both are cached for
later starts.

## Troubleshooting

- **"cannot be opened because the developer cannot be verified"**: run the
  `xattr` command from section 2 again.
- **Codegen doesn't print `XEX patch applied successfully`**: the title update
  is missing or not next to `default.xex` (section 4, step 3).
- **`[FATAL] Call to invalid or unregistered function at guest address 0x...`**:
  a game function the recompiler missed. Please open an issue with the address;
  the fix is an entry in `Fable2_config.toml` (see `MACOS_NOTES.md`).
- **The launcher's install waits forever**: macOS is asking whether Fable 2
  may access your Downloads (or another protected) folder; answer the prompt.
