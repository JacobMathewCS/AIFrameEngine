# AIFrameEngine

Frame interpolation and spatial upscaling for windowed games on Windows.

AIFrameEngine captures a game window, uses RIFE to estimate motion between frames,
and draws intermediate frames in a separate Direct3D compositor. FSR 1 or bicubic
scaling can enlarge the result to the output resolution. It does not need a game
plugin or access to the game's motion vectors.

**Publication status:** source, the native desktop launcher, shaders, and native
regression tests are available. The launcher builds independently with the Windows
SDK. A full compositor build still needs manually provisioned dependencies and
compatible models; this is not a one-click installer. See
[release status](docs/RELEASE_STATUS.md) for remaining release gates.

## How it works

- Windows Graphics Capture supplies the selected window's frames.
- A RIFE HDv3-derived ONNX model estimates flow and a blend mask at 576 x 320.
- ONNX Runtime DirectML runs inference on the selected GPU.
- A D3D11 shader reconstructs intermediate frames at the capture resolution.
- Buffered presentation schedules real and generated frames. Optional FSR 1
  EASU/RCAS performs spatial upscaling and sharpening.

RIFE is the AI component. FSR 1 is a spatial shader-based upscaler, not a neural
model. Generated frames improve visual smoothness; they do not increase the game's
simulation rate or provide the input latency of a higher native frame rate.

## Tested configurations

Development testing used The Witcher 3 with a 30 FPS cap on an RTX 3060 Laptop GPU,
with 1920 x 1080 SDR output.

| Capture resolution | Current evidence |
| --- | --- |
| 1024 x 768 | Working frame generation reported in user testing |
| 1280 x 720 | Best-tested configuration; good sessions approach 60 real-plus-generated frame submissions per second |
| 1366 x 768 | Working frame generation reported in user testing |
| 1680 x 1050 | Working frame generation reported in user testing |
| 1920 x 1080 | Capture works, but frame generation remains unreliable and can stop producing intermediate frames |

720p-to-1080p upscaling has also been visually tested. These results are not a
guarantee for every game, resolution, or GPU. Performance depends on available GPU
headroom. Interpolation artifacts, added buffering latency, and occasional
cooldown/probing are current limitations. HDR and exclusive fullscreen are not
validated.

## Models and dependencies

The normal configuration uses the FP16 RIFE flow export, with an FP32 alternative.
See [model notes](models/README.md) for the exact filenames, upstream project, and
license references. No pretrained weights are included.

Model: **RIFE HDv3-derived flow/mask export**, from
[ECCV2022-RIFE](https://github.com/hzwer/ECCV2022-RIFE), whose
[upstream code license is MIT](https://github.com/hzwer/ECCV2022-RIFE/blob/main/LICENSE).
Checkpoint redistribution terms still need separate confirmation; the repository
does not claim that the project MIT license licenses those weights.

The implementation uses C++17, Windows Graphics Capture, Direct3D 11/12,
ONNX Runtime DirectML, Dear ImGui, and AMD FidelityFX FSR 1. Model export uses
Python, PyTorch, NumPy, ONNX, and onnxsim. See
[third-party notes](THIRD_PARTY_NOTICES.md).

Windows SDK APIs provide capture and graphics. No hosted inference API is used in
the normal runtime. No training dataset is bundled or newly trained model claimed;
the native I/O test uses a small synthetic ONNX fixture, not pretrained weights.
The project does not use Gemma or the Gemini API.

## Build and run

Requirements: Windows x64, Visual Studio C++ build tools, Windows SDK with
C++/WinRT, and CMake 3.18 or newer. Runtime inference requires a compatible
DirectML GPU and driver.

From the repository root in PowerShell, build just the launcher without ML SDKs:

```powershell
$root = (Get-Location).Path
cmake -S "$root" -B "$root\build-launcher" -A x64 -DAIFRAME_LAUNCHER_ONLY=ON
cmake --build "$root\build-launcher" --config Release
ctest --test-dir "$root\build-launcher" -C Release --output-on-failure
```

For the compositor, first provision the exact directory layouts described in
[dependency setup](external/README.md) and [model setup](models/README.md), then:

```powershell
cmake -S "$root" -B "$root\build" -A x64
cmake --build "$root\build" --config Release
ctest --test-dir "$root\build" -C Release --output-on-failure
& "$root\build\Release\AIFrameEngineLauncher.exe"
```

Start your game in windowed mode, select its restored window in the launcher,
choose the scaler and precision, and click **Launch compositor**. Check readiness
with **Ctrl+Shift+F6**, then press **F9** to toggle frame generation.
**Ctrl+Shift+F12** exits the compositor. Closing the launcher does not stop it.
The launcher attaches capture to an already-running game; it does not start the game.
See [launcher guide](docs/LAUNCHER.md) for an existing compositor in another folder.

The maintainer reports a successful real-game launch with frame generation and
upscaling through the GUI. This is user-reported gameplay validation, not a new
benchmark or confirmation that the native-1080p limitation is fixed.

CI builds and tests the standalone launcher only. It does not validate DirectML,
model inference, gameplay, or full compositor dependencies.

## Maintainer

Maintainer: Jacob Mathew ([JacobMathewCS](https://github.com/JacobMathewCS)).
The complete event team roster still needs to be confirmed before the submission
is finalized.

## AI assistance

Cline was used for code/test assistance, diagnostic analysis, the native launcher,
and public-release documentation and validation. The maintainer also reports using
another AI model for repository changes; its name and contribution scope have not
yet been confirmed. The full historical tool/model list and contribution breakdown
remain incomplete. This section must not be treated as a complete event disclosure.
AI-produced changes require review and testing; they are not evidence that a
feature works. Team members must be able to explain the code they submit.

## License

Original project code and documentation are released under the [MIT license](LICENSE).
Third-party code and models retain their own licenses and notices.