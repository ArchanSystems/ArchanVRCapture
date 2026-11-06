# Archan VR Capture

Records the game's VR picture in 16:9 with OBS. Picture only: sound and mic
are set up in OBS's own audio settings.

Where the picture comes from (first one that has a picture wins):
1. **OpenXR games** on any runtime (Virtual Desktop VDXR, Pimax, SteamVR OpenXR):
   the game's own left eye, through the Archan VR Capture OpenXR layer. PimaxHome is skipped.
   Quad views (foveated rendering, 4 views, e.g. Pimax with eye tracking): the wide left view
   (view 0) with the sharp inner left view (view 2) put into it where it belongs, like the headset
   shows it; with eye-tracked foveation the sharp area moves with the eyes. The combined picture has
   the inner view's sharpness over the whole wide FOV, but no larger than HMD res can use (its 16:9
   crop at most 4096 wide); the OBS log line `quad views: combined picture …` gives its size.
2. **SteamVR games (OpenVR)**: SteamVR's left eye mirror texture, read by OBS as a
   background SteamVR app (never starts SteamVR, no mirror window). This is SteamVR's
   composited image, so SteamVR overlays (dashboard) show in it.

## Parts

| Part | File | What it does |
|---|---|---|
| OpenXR layer | `layer/layer.cpp` → `XR_APILAYER_ARCHAN_vr_capture.dll` | Runs inside the game. On every frame it copies the **left eye** image (view 0 of the projection layer) into a GPU shared texture. It copies only while OBS is showing the source. |
| OBS plugin | `obs-plugin/plugin.cpp` → `archan-vr-capture.dll` (+ `openvr_api.dll`, OpenVR SDK v2.15.6) | Source **"Archan VR Capture"**. Shows the **centre 16:9 crop** of the left eye, filling the canvas. |
| Resolution picker | in the source's Properties | **HMD res (automatic)** (default): canvas and output = the exact 16:9 crop of the left eye, 1:1 pixels (2496×1404 for a 2496×2688 eye), following the eye size by itself, **at most 4096×2304** (user decision 2026-11-06: NVENC H.264 encodes at most 4096 wide, HEVC 8192 — measured with OBS's `obs-nvenc-test` on an RTX 3090; a 5888×3312 quad views picture failed to record). **1440p**: 2560×1440. **1080p**: 1920×1080. Only the canvas and output resolution of the active profile are changed, and only while no recording/stream/replay buffer/virtual camera is running. |

## Stabilization (source Properties → "Stabilization")

Uses the **head pose** the game rendered each frame with (OpenXR: view 0 pose + FOV from the
layer; SteamVR mirror: the HMD pose SteamVR reports for the frame). No image analysis, any GPU.

**Look-ahead:** the video is shown a set delay behind the game, so at each output time the head
poses up to *delay* ahead are known. The camera path is planned on a grid at OBS's frame rate as
the **smoothest path (least angular acceleration) that keeps every frame inside the crop**
(projected FISTA, warm-started). The two cameras just shown are fixed points of each new plan, so
the camera continues smoothly from what the viewer saw: a new plan never jumps, also when OBS
skips a frame. The camera is evaluated at OBS's own frame time and the buffered frame nearest to it
is re-projected to it (one GPU shader pass), so static scenery stays smooth even though the
captured frames' own times jitter (17–52 ms apart in a real DCS recording).

| Option | What it does |
|---|---|
| Stabilization (group checkbox) | On / Off. Default on. Without a head pose the picture is shown unstabilized. |
| Smoothness 0–100 | How freely the camera may leave your head's path: 0 follows the head, 100 = the smoothest path the crop allows. Default 50. |
| Keep horizon level | The camera follows your view direction, not your head tilt (needs a gravity-aligned space: OpenXR LOCAL/STAGE/LOCAL_FLOOR, SteamVR). When the head tilts more than the crop allows, the camera has to tilt too: that tilt is planned like the rest of the path, so it eases in and out smoothly instead of jumping. |
| Crop | Set by you (default 15 %). The room the camera may move in, see the table below. |
| Video delay | Set by you (default 0.20 s, 0.1–2.0 s). GPU memory: only frames a future OBS output frame can still show are kept, about (delay × OBS fps + 3) frames + 2 spare, × eye size × 4 bytes (Quest 3 / VD eye 26.8 MB: 0.20 s at 30 fps ≈ 0.3 GB). The OBS log line `cost (last 10 s)` shows the buffer's frames and MB. |

Room per crop, from the plugin's own fit check (each direction alone, looking straight ahead):

| Crop | DCS on Virtual Desktop, eye 2496×2688 | Pimax, eye 4024×3232 |
|---|---|---|
| 10 % | sideways ±3°, up/down ±8° | tilt ±15°, sideways −2.6/+3.9°, up/down ±7.6° |
| 20 % | ±6–7° / ±16–18° | tilt ±24°, −5.3/+8.0°, ±15° |
| 25 % | ±8–9° / ±20–23° | tilt ±28°, −6.8/+10.3°, ±17° |
| 30 % | ±9–11° / ±24–27° | tilt ±34°, −8.4/+12.7°, ±19° |
| 40 % | | tilt ±50°, −11.9/+17.9°, ±22° |

**Audio sync (automatic):** while stabilization is on, the plugin sets the **Sync Offset of every
audio source** (Desktop Audio, Mic/Aux, application audio, media...) to *your own offset + the video
delay*, and follows the delay setting. An offset you type meanwhile counts as yours (+ the delay).
Stabilization off or the VR source removed: every audio source gets your own offset back. OBS saves
the scene collection while the delay is applied (e.g. at exit), so the plugin saves your own offsets
with it and takes them back on the next load. `uninstall.ps1` puts your own offsets back in every
scene collection (`tools\restore_audio_sync.ps1`, only where the saved value is still the plugin's).
While the delay buffer fills (first *delay* seconds of a game session) the picture waits on the
oldest frame, so the video is already as late as the audio.

The OBS log shows `stabilization running: … video X s behind the newest frame` once it starts, and
one `audio sync: '<source>' … ms -> … ms` line per change.

Head motion recorder: while OBS records, every head pose goes to
`%LOCALAPPDATA%\ArchanVRCapture\logs\poses-*.csv`; `build\stab_replay.exe` replays such a file
through the same stabilizer the plugin runs.

Measured with `stab_replay` on the recorded sessions (what the viewer sees per OBS frame at 30 fps,
smoothness 100). Before = the previous version (each frame re-planned from scratch, horizon
forced level):

| Recording | Settings | Camera angular accel. p99, before → now (plain picture) | Largest horizon tilt step per frame, before → now |
|---|---|---|---|
| Pimax DCS 17:06 | crop 10 %, delay 1.0 s, horizon on | 3327 → 922 °/s² (3922) | 4.65° → 3.01° |
| Pimax DCS 17:04 | crop 10 %, delay 0.2 s, horizon on | 2557 → 1153 (3663) | 3.29° → 2.81° |
| Pimax DCS 17:01 | crop 5 %, delay 0.5 s, horizon on | 1929 → 1055 (2151) | 6.53° → 7.10° (one 30° head-pose jump in 42 ms) |
| Pimax DCS 17:07 | crop 10 %, delay 0.35 s | 1925 → 921 (3601) | |
| Pimax DCS 17:08 | crop 10 %, delay 0.6 s | 698 → 411 (3272) | |
| VD DCS 16:22 | crop 25 %, delay 1.0 s, horizon on | 294 → 296 (4353) | horizon tilt max 0.1° |

No edge in any frame. With 10 % of OBS frames skipped and at 24 fps the results stay in the same
range (no drift). At 60 fps the median stays the same; the 99th percentile is higher (also for the
plain picture: at 60 fps game frames repeat) but stays far below the plain picture. Synthetic (tests): hand-like shake 409× steadier; a 40° glance in 0.6 s
peaks at 158 °/s² (head 548); 10° head tilt → camera tilt 0.0000°. In real OBS (plugin test phase C,
real time, 4 runs): 28× steadier frame to frame, every stabilized frame = steady-camera reference
within worst mean 0.29–0.45 / max 2 colour levels. Phase D: audio offsets 50/0 ms → 550/500 →
850/800 (delay 0.8 s), typed 900 kept as own 100, new source gets +800, saved/loaded, then off →
50/100/0 and VR source removed → 50/100/0.

## Build / install

**Release installer (signed), for testers and users:**

```powershell
powershell -ExecutionPolicy Bypass -File tools\packaging\make_release.ps1 -Version 0.1.0
# -> E:\ArchanVRCapture_Release_Installer\ArchanVRCapture-0.1.0-Setup.exe
```

Build, all tests, explicit stage, sign + prove our two DLLs, payload hashes
(`payload.manifest.txt`), Inno Setup (`tools\packaging\ArchanVRCapture.iss`, signs Setup and the
uninstaller), final check; an installer that fails it is renamed `.UNSIGNED`. Signing is the same
system as Archan XR: `tools\packaging\sign_release.ps1` is Archan XR's script (Microsoft
Artifact Signing, signer Archan Systems AS, SHA-256, RFC 3161 timestamp), except that the Azure
tenant and subscription are read from `tools\packaging\signing_login.local.json` (not in git:
`{ "tenant": "<id>", "subscription": "<id>" }`). Needs Inno Setup 6, the
Artifact Signing Client Tools, `D:\ArchanXR\signing\metadata.json` and `az login`
(contact@archansystems.com). Only Archan Systems can sign; anyone can build and install unsigned
with `build.ps1` and `install.ps1`. Valve's `openvr_api.dll` is shipped as Valve built it.
The installer needs no admin rights, closes OBS (asked first, so it saves), the simulators and
Pimax/OFXR after naming them, and installs to the same places as `install.ps1`. Its uninstaller
gives every audio source its own Sync Offset back, removes the files and the layer registration,
and keeps `logs\`.

**Development install** (unsigned build output, same places):

```powershell
.\build.ps1      # VS 18 + CMake; makes import libs from the installed OBS DLLs
.\install.ps1    # close OBS first; no admin needed
.\uninstall.ps1  # removes everything install.ps1 added; puts your own audio Sync Offsets back (close OBS first)
```

Install locations:
- Layer: `%LOCALAPPDATA%\ArchanVRCapture\layer\`, registered in
  `HKCU\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit`
- Plugin: `%ProgramData%\obs-studio\plugins\archan-vr-capture\`

## Use

1. Start the game in VR (Virtual Desktop, Pimax or SteamVR).
2. In OBS: Sources → + → **Archan VR Capture**.
3. In its Properties, pick **HMD res (automatic)** or **1080p**. OBS switches by itself
   once the game picture shows (it waits while recording/streaming runs).
4. Record with OBS as usual.

## Logs

- Cleanup: on every OBS start and every game start, files in `%LOCALAPPDATA%\ArchanVRCapture\logs`
  last written more than 48 h ago are deleted, and `layer.log` loses its lines older than 48 h
  (`common/log_cleanup.h`; only this folder, OBS's own logs are left to OBS). Both write a
  `log cleanup …` line.
- Layer: `%LOCALAPPDATA%\ArchanVRCapture\logs\layer.log` (app, runtime, eye size, format, FOV).
  Every 10 s while OBS shows the picture: `cost on the game thread …` = the time this layer spends
  inside the game's xrEndFrame on the game's thread (avg / 99 % / max per copied frame, copy and
  flush separately), the game's frame interval and the time of the runtime + layers below.
- Plugin: the OBS log, lines starting with `[archan-vr-capture]`. Every 10 s while a picture is
  shown: `cost (last 10 s): …` = this source's CPU time on OBS's render thread, the stabilization
  planner's share, OBS's average frame render time and lagged frames.

## Limits

- OpenXR layer: D3D11 (e.g. DCS) and D3D12 (e.g. MSFS 2024) games. D3D12 images are copied with
  D3D11On12 on the game's own queue (released colour images are in RENDER_TARGET, OpenXR spec
  khr_d3d12_enable). Vulkan/OpenGL sessions: the layer logs the graphics type and does nothing.
- 64-bit games only.
- MSAA swapchains are not copied (logged).
- HKCU layers are ignored by the OpenXR loader if the game runs as administrator.
- One game at a time is sent to OBS. A running app that holds the channel but sends no
  frame for 2 s while OBS is watching (e.g. PimaxHome) is taken over by the game.
- Tests use their own channel (`ARCHAN_VRCAPTURE_TEST_CHANNEL`, set only inside the test processes), so a
  running game or OBS is never touched. They still refuse (exit 3, "NOT RUN") if that test channel is in use.

## Tests (no headset needed)

```powershell
build\layer_mock_test.exe --crop
build\layer_mock_test.exe --cleanup   # log cleanup on a temporary folder (48 h rule)
build\layer_mock_test.exe build\XR_APILAYER_ARCHAN_vr_capture.dll
build\layer_mock_test.exe build\XR_APILAYER_ARCHAN_vr_capture.dll --cost   # + the cost log line (~10 s at 90 Hz)
build\layer_mock_test.exe build\XR_APILAYER_ARCHAN_vr_capture.dll --d3d12
build\layer_mock_test.exe build\XR_APILAYER_ARCHAN_vr_capture.dll --quad          # quad views (also with --d3d12)
build\stab_test.exe
build\stab_replay.exe <poses.csv> out.csv <crop %> <delay s> <smoothness> <horizon 0|1> [fps]   # env STAB_SKIP=0.1: 10 % of OBS frames skipped
# run from C:\Program Files\obs-studio\bin\64bit:
build\plugin_render_test.exe build\archan-vr-capture.dll obs-plugin\data
```

## Dependencies (in `deps/`, not committed)

| Folder | Source | Version | Commit | Used parts |
|---|---|---|---|---|
| `deps/obs-studio` | https://github.com/obsproject/obs-studio | 32.2.2 | ba2f32bdf791005443988a4955e963663e16b1ed | libobs, libobs-d3d11, frontend/api (headers; import libs are made from the installed OBS DLLs) |
| `deps/openxr-sdk` | https://github.com/KhronosGroup/OpenXR-SDK | release-1.1.65 | 24a1f2b92d212501a2b1b8d30ffd53abf55fcfa8 | include/openxr |
| `deps/openvr` | https://github.com/ValveSoftware/openvr | v2.15.6 | 0924064316de3effbcd1acf1e309182a2deb1c05 | headers, bin/win64/openvr_api.dll |
| `deps/openxr-docs` | https://github.com/KhronosGroup/OpenXR-Docs | release-1.1.65 | 18ebc0374da23c020ef4c949f9b144ffae6160ef | spec text only (khr_d3d12_enable.adoc: D3D12 image states) |

## License

Copyright (C) 2026 Archan Systems AS.

This program is free software; you can redistribute it and/or modify it under the terms of the
GNU General Public License as published by the Free Software Foundation; either version 2 of the
License, or (at your option) any later version. See [LICENSE](LICENSE).

Third-party parts keep their own licenses: OBS Studio (GPL-2.0-or-later), OpenXR SDK headers (Apache-2.0 OR MIT),
OpenVR SDK and its `openvr_api.dll` (BSD-3-Clause, shipped as Valve built it).
