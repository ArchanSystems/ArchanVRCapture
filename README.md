# Archan VR Capture

Records the game's VR picture in 16:9 with OBS. Picture only: sound and mic
are set up in OBS's own audio settings.

Where the picture comes from (first one that has a picture wins):
1. **OpenXR games** on any runtime (Virtual Desktop VDXR, Pimax, SteamVR OpenXR):
   the game's own left eye, through the Archan VR Capture OpenXR layer. PimaxHome is skipped.
   Pimax quad views: view 0 = the wide left view.
2. **SteamVR games (OpenVR)**: SteamVR's left eye mirror texture, read by OBS as a
   background SteamVR app (never starts SteamVR, no mirror window). This is SteamVR's
   composited image, so SteamVR overlays (dashboard) show in it.

## Parts

| Part | File | What it does |
|---|---|---|
| OpenXR layer | `layer/layer.cpp` → `XR_APILAYER_ARCHAN_vr_capture.dll` | Runs inside the game. On every frame it copies the **left eye** image (view 0 of the projection layer) into a GPU shared texture. It copies only while OBS is showing the source. |
| OBS plugin | `obs-plugin/plugin.cpp` → `archan-vr-capture.dll` (+ `openvr_api.dll`, OpenVR SDK v2.15.6) | Source **"Archan VR Capture"**. Shows the **centre 16:9 crop** of the left eye, filling the canvas. |
| Resolution picker | in the source's Properties | **HMD res (automatic)** (default): canvas and output = the exact 16:9 crop of the left eye, 1:1 pixels (2496×1404 for a 2496×2688 eye), following the eye size by itself. **1080p**: 1920×1080. Only the canvas and output resolution of the active profile are changed, and only while no recording/stream/replay buffer/virtual camera is running. |

## Build / install

```powershell
.\build.ps1      # VS 18 + CMake; makes import libs from the installed OBS DLLs
.\install.ps1    # close OBS first; no admin needed
.\uninstall.ps1  # removes everything install.ps1 added
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

- Layer: `%LOCALAPPDATA%\ArchanVRCapture\logs\layer.log` (app, runtime, eye size, format, FOV)
- Plugin: the OBS log, lines starting with `[archan-vr-capture]`

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
build\layer_mock_test.exe build\XR_APILAYER_ARCHAN_vr_capture.dll
build\layer_mock_test.exe build\XR_APILAYER_ARCHAN_vr_capture.dll --d3d12
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
