Fable II for Apple Silicon Macs, as a native app. **Bring your own game:** this download contains no game files and no game code. The app installs your copy and builds the game from it on your Mac.

## You need

- An Apple Silicon Mac (M1 or newer) with macOS 14 or later
- Your Fable II disc image (`.iso`) and **title update 1** for your disc's media ID (see the README for the discs that work)
- About 10 GB of free disk space
- Apple's Command Line Tools: the app offers to install them if they're missing

## Install

1. Download **Fable2RecompMac-1.0.zip**, open it and move **Fable 2.app** to Applications.
2. The first time you open it, macOS blocks the app because it isn't signed by Apple (this is a free hobby project without an Apple developer account):
   - **macOS 15 or later:** double-click it, click **Done**, then go to **System Settings → Privacy & Security**, scroll down and click **Open Anyway**.
   - **macOS 14:** right-click the app, choose **Open**, then click **Open**.
3. In the Title update row, click **Choose…** and select the folder with your title update packages.
4. Click **Install from ISO…** and choose your disc image. The app checks it, installs it and then builds the game (2 to 15 minutes, needed once).
5. Click **Play**.

## Good to know

- Your first visit to each area after a new install may show missing or white geometry for a few seconds while its shaders are translated. They're saved for later visits.
- Shadows can pop when the camera moves, much like on the Xbox 360.
- Saves are in `~/.local/share/Fable2`. Everything else the app sets up is in `~/Library/Application Support/Fable2RecompMac`, including `build.log` if a build fails.
