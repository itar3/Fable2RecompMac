Fable II for Apple Silicon Macs, as a native app. **You need your own Fable II disc image (`.iso`) and its Title Update 1** (title updates aren't on the disc; you need the one for your disc's media ID). This download contains no game files and no game code: the app installs your copy and builds the game from it on your Mac.

## New in 1.2

- **Faster in busy places.** The game now skips drawing what it knows is hidden behind walls and buildings, as it did on the Xbox 360. In busy Bowerstone a frame takes about 10-15% less time, and other areas gain too.
- **Fixed: "Set Nickname" froze the game.** Choosing Set Nickname on a villager's card opened a text box that never appeared, and the game stopped responding. The box now shows: type the name on your Mac's keyboard and press Enter (or click OK). It can't be typed into with a controller yet. Other system messages the game shows now appear too.
- **Less CPU use and heat.** The renderer now waits without keeping a processor core busy.
- **40 fps cap** in the launcher, for 120 Hz screens: steady frame pacing with less power than 60.

Updating from 1.1: replace the app in Applications. On first start it unpacks its tools again and asks you to click **Build** once (a few minutes). Your game files and saves stay.

## Also in 1.1: keyboard and mouse

- Play without a controller: WASD moves, the mouse moves the camera, left click is melee, right click is ranged, Space or E interacts. **Controls** and **Key bindings…** in the launcher set it up; a controller still works alongside.

## You need

- An Apple Silicon Mac (M1 or newer) with macOS 14 or later
- Your Fable II disc image (`.iso`) and **title update 1** for your disc's media ID (see the README for the discs that work)
- About 10 GB of free disk space
- Apple's Command Line Tools: the app offers to install them if they're missing

## Install

1. Download **Fable2RecompMac-1.2-mac-native-arm64.dmg**, open it and drag **Fable 2.app** to Applications.
2. The first time you open it, macOS blocks the app because it isn't signed by Apple (this is a free hobby project without an Apple developer account):
   - **macOS 15 or later:** double-click it, click **Done**, then go to **System Settings → Privacy & Security**, scroll down and click **Open Anyway**.
   - **macOS 14:** right-click the app, choose **Open**, then click **Open**.
3. In the Title update row, click **Choose…** and select the folder with your title update packages.
4. Click **Install from ISO…** and choose your disc image. The app checks it, installs it and then builds the game (2 to 15 minutes, needed once).
5. Click **Play**.

## Good to know

- Your first visit to each area after a new install may show missing or white geometry for a few seconds while its shaders are translated. They're saved for later visits.
- Shadows can pop when the camera moves, much like on the Xbox 360.
- While playing with the mouse, the pointer is locked to the game: switch apps (⌘-Tab) to get it back.
- Saves are in `~/.local/share/Fable2`. Everything else the app sets up is in `~/Library/Application Support/Fable2RecompMac`, including `build.log` if a build fails.
