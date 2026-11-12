#!/bin/bash
# Makes the release app, out/release/Fable 2.app, and its download,
# out/release/Fable2RecompMac-<version>-mac-native-arm64.dmg (the app next to
# an Applications shortcut, to drag it there).
#
# The app carries no game code: no Fable2 executable, nothing from generated/
# or a shader dump. It is the launcher (tools/launcher) in release mode, with
# a payload (Contents/Resources/payload.tar.xz) of what it needs to build the
# game on the player's Mac from their own copy of the game: this project's
# source at HEAD with its submodules, the ReXGlue SDK (Release libraries only),
# CMake, Ninja and fable2-extract. The launcher unpacks it into
# ~/Library/Application Support/Fable2RecompMac and builds there (see its
# header comment). The app is ad-hoc signed only (no Developer ID): players
# open it the first time with right-click -> Open.
#
# Usage: REXSDK=<ReXGlue SDK> tools/release/make_release.sh [version]
# Needs: Xcode (the launcher is SwiftUI), the game's build in
# out/build/mac-arm64-native (for fable2-extract), and the network the first
# time (CMake and Ninja are downloaded into out/release/downloads).
set -euo pipefail

VERSION="${1:-1.0}"
CMAKE_VERSION=4.4.5
NINJA_VERSION=1.13.2
SDK_VERSION=0.10.0.24-dev.gbd833a2

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/out/release"
BUILD="$ROOT/out/build/mac-arm64-native"
APP="$OUT/Fable 2.app"
PAYLOAD="$OUT/payload"
DOWNLOADS="$OUT/downloads"
: "${REXSDK:?Set REXSDK to the ReXGlue SDK $SDK_VERSION}"
cd "$ROOT"

step() { printf '\n== %s\n' "$*"; }
fail() { echo "make_release: $*" >&2; exit 1; }

"$REXSDK/bin/rexglue" --version 2>&1 | grep -q "$SDK_VERSION" || fail "REXSDK isn't ReXGlue SDK $SDK_VERSION"
# The payload is made from HEAD, or for a test from the working tree (stamped "+changes"). The
# submodules are taken as committed: their working changes are the patches the build applies.
mkdir -p "$OUT"
TREE=HEAD
STAMP="$VERSION ($(git rev-parse --short HEAD))"
if [ -n "$(git status --porcelain --ignore-submodules=dirty)" ]; then
  rm -f "$OUT/index.tmp"
  TREE="$(GIT_INDEX_FILE="$OUT/index.tmp" bash -c 'git read-tree HEAD && git add -A && git write-tree')"
  rm -f "$OUT/index.tmp"
  STAMP="$VERSION ($(git rev-parse --short HEAD)+changes)"
  echo "warning: uncommitted changes are included; commit them before a real release"
fi

step "fable2-extract"
cmake --build "$BUILD" --target fable2_extract
otool -L "$BUILD/fable2-extract" | grep -q "@rpath/librexruntime.dylib" || fail "unexpected fable2-extract linkage"

step "CMake $CMAKE_VERSION and Ninja $NINJA_VERSION"
mkdir -p "$DOWNLOADS"
CMAKE_TGZ="cmake-$CMAKE_VERSION-macos-universal.tar.gz"
CMAKE_URL="https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION"
[ -f "$DOWNLOADS/$CMAKE_TGZ" ] || curl -fsSL --retry 3 -o "$DOWNLOADS/$CMAKE_TGZ" "$CMAKE_URL/$CMAKE_TGZ"
[ -f "$DOWNLOADS/cmake-$CMAKE_VERSION-SHA-256.txt" ] ||
  curl -fsSL --retry 3 -o "$DOWNLOADS/cmake-$CMAKE_VERSION-SHA-256.txt" "$CMAKE_URL/cmake-$CMAKE_VERSION-SHA-256.txt"
(cd "$DOWNLOADS" && grep " $CMAKE_TGZ\$" "cmake-$CMAKE_VERSION-SHA-256.txt" | shasum -a 256 -c -) || fail "CMake checksum"
NINJA_ZIP="ninja-mac-$NINJA_VERSION.zip"
[ -f "$DOWNLOADS/$NINJA_ZIP" ] ||
  curl -fsSL --retry 3 -o "$DOWNLOADS/$NINJA_ZIP" "https://github.com/ninja-build/ninja/releases/download/v$NINJA_VERSION/ninja-mac.zip"

step "Payload"
rm -rf "$PAYLOAD" "$APP"
mkdir -p "$PAYLOAD/source" "$PAYLOAD/sdk" "$PAYLOAD/tools/cmake"

# This project and its submodules, as committed (no .git, assets, generated/, builds).
git archive "$TREE" | tar -x -C "$PAYLOAD/source"
export PAYLOAD_SOURCE="$PAYLOAD/source"
git submodule foreach --quiet --recursive \
  'mkdir -p "$PAYLOAD_SOURCE/$displaypath" && git archive HEAD | tar -x -C "$PAYLOAD_SOURCE/$displaypath"'
# DXC: only the Apple Silicon macOS build is used.
DXC="$PAYLOAD/source/thirdparty/XenosRecomp/thirdparty/dxc-bin"
rm -rf "$DXC/bin/x64" "$DXC/bin/x86" "$DXC/lib/x64" "$DXC/lib/x86"
rm -f "$DXC"/bin/arm64/*.exe "$DXC"/bin/arm64/*.dll "$DXC"/lib/arm64/*.lib
[ -x "$DXC/bin/arm64/dxc-macos" ] && [ -f "$DXC/lib/arm64/libdxcompiler.dylib" ] || fail "DXC is missing"

# The SDK, Release configuration only: the Debug and RelWithDebInfo libraries go with their CMake files.
rsync -a "$REXSDK/" "$PAYLOAD/sdk/"
while IFS= read -r config; do
  grep -o '"${_IMPORT_PREFIX}/[^"]*"' "$config" | sed 's|"${_IMPORT_PREFIX}/||; s|"$||' | sort -u |
    while IFS= read -r file; do rm -rf "$PAYLOAD/sdk/$file"; done
  rm "$config"
done < <(find "$PAYLOAD/sdk/lib/cmake" \( -name '*-debug.cmake' -o -name '*-relwithdebinfo.cmake' \))
while IFS= read -r config; do  # everything the Release configuration names must still be there
  grep -o '"${_IMPORT_PREFIX}/[^"]*"' "$config" | sed 's|"${_IMPORT_PREFIX}/||; s|"$||' | sort -u |
    while IFS= read -r file; do [ -e "$PAYLOAD/sdk/$file" ] || fail "SDK: $file is missing"; done
done < <(find "$PAYLOAD/sdk/lib/cmake" -name '*-release.cmake')

# CMake (bin/cmake and its modules), Ninja, fable2-extract (+ the runtime it links).
tar -x -C "$OUT" -f "$DOWNLOADS/$CMAKE_TGZ"
CMAKE_APP="$OUT/cmake-$CMAKE_VERSION-macos-universal/CMake.app/Contents"
mkdir -p "$PAYLOAD/tools/cmake/bin" "$PAYLOAD/tools/cmake/share"
cp "$CMAKE_APP/bin/cmake" "$PAYLOAD/tools/cmake/bin/"
cp -R "$CMAKE_APP/share/cmake-${CMAKE_VERSION%.*}" "$PAYLOAD/tools/cmake/share/"
rm -rf "$OUT/cmake-$CMAKE_VERSION-macos-universal"
unzip -q -o "$DOWNLOADS/$NINJA_ZIP" ninja -d "$PAYLOAD/tools"
cp "$BUILD/fable2-extract" "$PAYLOAD/sdk/lib/librexruntime.dylib" "$PAYLOAD/tools/"
"$PAYLOAD/tools/cmake/bin/cmake" --version | head -1
"$PAYLOAD/tools/ninja" --version
"$PAYLOAD/tools/fable2-extract" --help >/dev/null 2>&1 || true

step "Payload archive"
COPYFILE_DISABLE=1 tar -c -C "$PAYLOAD" -f - source sdk tools | xz -T0 -6 > "$OUT/payload.tar.xz"
du -sh "$PAYLOAD" "$OUT/payload.tar.xz"

step "Fable 2.app $STAMP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
IMAGE_HASH="$(sed -n 's/^set(FABLE2_LAUNCHER_IMAGE_HASH "\(.*\)")$/\1/p' cmake/launcher.cmake)"
IMAGE_LABEL="$(sed -n 's/^set(FABLE2_LAUNCHER_IMAGE_LABEL "\(.*\)")$/\1/p' cmake/launcher.cmake)"
[ -n "$IMAGE_HASH" ] && [ -n "$IMAGE_LABEL" ] || fail "image hash/label not found in cmake/launcher.cmake"
sed -e "s|@FABLE2_LAUNCHER_GAME_DIR@||; s|@FABLE2_LAUNCHER_ASSETS_DIR@||; s|@FABLE2_LAUNCHER_SHADER_DUMP_DIR@||" \
    -e "s|@FABLE2_LAUNCHER_IMAGE_HASH@|$IMAGE_HASH|; s|@FABLE2_LAUNCHER_IMAGE_LABEL@|$IMAGE_LABEL|" \
    -e "s|@FABLE2_LAUNCHER_PAYLOAD_VERSION@|$STAMP|" \
    -e "/<key>CFBundleShortVersionString<\/key>/{n;s|<string>.*</string>|<string>$VERSION</string>|;}" \
    tools/launcher/Info.plist.in > "$APP/Contents/Info.plist"
plutil -lint "$APP/Contents/Info.plist" >/dev/null
DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}" xcrun --sdk macosx swiftc \
  -O -parse-as-library -target arm64-apple-macos14.0 -o "$APP/Contents/MacOS/Fable 2" tools/launcher/Fable2Launcher.swift
mv "$OUT/payload.tar.xz" "$APP/Contents/Resources/"
codesign --force --deep --sign - "$APP"
codesign --verify --deep "$APP"

step "Disk image"
DMG="$OUT/Fable2RecompMac-$VERSION-mac-native-arm64.dmg"
DMG_ROOT="$OUT/dmg"
rm -rf "$DMG" "$DMG_ROOT"
mkdir -p "$DMG_ROOT"
ditto "$APP" "$DMG_ROOT/Fable 2.app"
ln -s /Applications "$DMG_ROOT/Applications"
hdiutil create -quiet -volname "Fable 2" -srcfolder "$DMG_ROOT" -fs HFS+ -format UDZO -ov "$DMG"
rm -rf "$DMG_ROOT"
hdiutil verify -quiet "$DMG"
ls -lh "$DMG"
echo
echo "Done: $APP ($STAMP)"
