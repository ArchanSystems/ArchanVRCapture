# Archan VR Capture

Record your VR game in OBS as a steady 16:9 video.

## How to use

1. Install `ArchanVRCapture-<version>-Setup.exe` from
   [Releases](https://github.com/ArchanSystems/ArchanVRCapture/releases) (close OBS and the game first).
2. Start the game in VR.
3. In OBS: **Sources → + → Archan VR Capture**.
4. In its Properties pick the resolution: **HMD res** (automatic, up to 4096×2304), **1440p** or **1080p**.
5. Record as usual.

Stabilization settings are in the same Properties: **Smoothness** (default 100), **Crop** (default
15 %) is how much room the camera has to move, **Video delay** (default 0.20 s) how far ahead it can
look. While the source is live in your Program scene, every audio source gets this delay added to
its own Sync Offset; when the source is not live, stabilization is off, or the source is removed,
they get their own offset back.

## How it works

- **OpenXR games** (DCS, MSFS 2024, … on Virtual Desktop, Pimax or SteamVR): a small OpenXR layer
  copies the game's left eye to OBS. Pimax quad views (eye-tracked foveated rendering) are combined
  like the headset shows them.
- **SteamVR games (OpenVR):** OBS reads SteamVR's left eye mirror.
- OBS shows the centre 16:9 of that eye. The camera is smoothed using your head movement, a short
  delay behind the game, so the video stays steady and the horizon level.
- The audio is delayed by the same amount, so sound and picture stay in sync.

Works with D3D11 and D3D12 games (not Vulkan/OpenGL). Logs: `%LOCALAPPDATA%\ArchanVRCapture\logs`
and the OBS log.

## License

GPL-2.0-or-later, Copyright (C) 2026 Archan Systems AS. See [LICENSE](LICENSE).
