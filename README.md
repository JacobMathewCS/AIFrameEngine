# AIFrameEngine

Experimental Windows desktop-capture frame generation and spatial upscaling for games.
The application captures a selected game window, estimates intermediate motion using
RIFE through ONNX Runtime DirectML, reconstructs midpoints from the original-resolution
images, and presents them through a separate D3D11 compositor.

**Status: working personal-project baseline, not a universally validated game utility.**
This source repository does not include model weights or downloaded third-party SDKs.
A fresh clone requires manual dependency provisioning; it is not a one-click installer.

## Features

- Window selection with process-lifetime checks; legacy Witcher lookup remains available.
- RIFE flow estimation at 576 x 320, with full-resolution GPU midpoint reconstruction.
- Optional FP16 inference, GPU preprocessing, and display-image caching.
- Adaptive buffered playback, retry admission, and completion-aware presentation waits.
- Bicubic/bilinear scaling and optional reference FSR 1 EASU + RCAS for SDR images.
- Sharpening controls, read-only status overlay, and optional presentation identity traces.

Upscaling is spatial, not neural or engine-integrated temporal reconstruction. Frame
generation does not increase the game's simulation rate or give native-60-FPS input latency.

## Requirements and build

- Windows x64 with Windows Graphics Capture support and a DirectML-capable GPU/driver.
- Visual Studio C++ build tools, Windows SDK including C++/WinRT, and CMake 3.18 or newer.
- Dear ImGui, AMD FSR 1 headers, ONNX Runtime DirectML SDK/runtime, and compatible RIFE
  models supplied as described in `external/README.md` and `models/README.md`.
- Python is optional for model export and the additional summary-tool tests.

The tested setup is an RTX 3060 Laptop GPU with Witcher 3 in 720p windowed mode,
capped at 30 FPS, composited to 1080p SDR. Other configurations require validation.

From a Developer PowerShell, set the absolute checkout path and build:

```powershell
$root = (Get-Location).Path # Run from the checkout root.
cmake -S $root -B "$root\build" -A x64
cmake --build "$root\build" --config Release
ctest --test-dir "$root\build" -C Release --output-on-failure
```

Repeat with `--config Debug` and `-C Debug` for a debug build. Graphics regression
tests use Windows graphics APIs; passing them is not a gameplay performance benchmark.
Explicit DirectML hardware tests additionally require models and the correct GPU index.

## Run

Start the game first in windowed mode, then run from the checkout root:

```powershell
$root = (Get-Location).Path
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$root\launch_framegen.ps1" -SelectWindow -Fsr1
```

Enter the game's PID from the list. Wait for `RIFE ready`, then press F9 once.
Omit `-Fsr1` for the previous bicubic scaler. Use `-NoFp16` for FP32 or `-NoCache`
to disable display caching. Do not run multiple compositors concurrently.

| Control | Action |
| --- | --- |
| F9 | Toggle frame generation |
| F10 | Toggle selected scaler and bilinear comparison |
| F11 | Cycle sharpening presets |
| Ctrl+Shift+Up / Down | Adjust sharpening by 0.05 |
| Ctrl+Shift+F6 | Show/hide read-only status panel |
| Ctrl+Shift+F7 | Export same-frame upscaler comparison; may briefly hitch |
| F8 | Cycle diagnostic display modes; leave normal mode selected for gameplay |
| Ctrl+Shift+F12 | Clean shutdown |

The application log is `C:\Temp\aiframe_compositor.log`. `-TracePresentations` writes
CSV traces into the checkout's `scratch/presentation_traces` directory. Logs and
screenshots may expose window titles or gameplay content: review before sharing.

## Results and limitations

One recorded Witcher session correlated compositor identity traces with PresentMon:
approximately 30.04 real identities/sec with FG off and 58.36 real-plus-generated
displayed identities/sec with FG on. This is a single-system observation, not a
guarantee of locked 60 FPS, unique pixel content, or consistent performance elsewhere.
Raw gameplay traces are intentionally not published in this source repository.

- Buffering adds latency; recent tested effective delays were commonly around 40–67 ms.
  This is not total input-to-photon latency. Interpolation requires a subsequent source frame.
- Motion artifacts, occasional stalls and uneven pacing remain possible.
- HDR, exclusive fullscreen, protected content, anti-cheat compatibility, multiple monitors,
  target resizing/minimize/restore and broad game support are not comprehensively validated.
- No engine depth/motion/HUD separation; spatial upscaling cannot recreate native detail.
- The experimental D3D12 GPU-tensor I/O path is not end-to-end zero-copy or the default.

## Layout

- `src/`: compositor, GPU helpers, frame scheduling and shaders.
- `tests/`: regression tests, synthetic fixture generator and explicit hardware benchmarks.
- `export_rife_fg.py`, `export_rife_flow.py`: research model exporters, not automatic setup.
- `scratch/`: local evidence and experiments, ignored except three test helper scripts.
- `external/`, `models/`: locally provisioned dependencies; see their setup notes.

## Contributing and licensing

See `docs/PUBLISHING.md` for the first-upload checklist. The GitHub workflow covers
Python summary regressions only; it is not a native build or GPU validation badge.

See `CONTRIBUTING.md` and `THIRD_PARTY_NOTICES.md`. No project-wide open-source license
has been selected yet; do not assume redistribution permission for the project or
model weights. Third-party components retain their own terms. There is no affiliation
with AMD, NVIDIA, Microsoft, or Lossless Scaling.