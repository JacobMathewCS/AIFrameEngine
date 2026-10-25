# AIFrameEngine

Frame interpolation and spatial upscaling for windowed games on Windows.

AIFrameEngine captures a game window, uses RIFE to estimate motion between frames,
and draws intermediate frames in a separate Direct3D compositor. FSR 1 or bicubic
scaling can enlarge the result to the output resolution. It does not need a game
plugin or access to the game's motion vectors.

**Publication status:** this is the first stage of the public release. This checkout
contains documentation and licensing only; source, build instructions, tests, and
model provisioning will follow. It is not buildable yet.

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
license references. No weights are included in this first stage.

The implementation uses C++17, Windows Graphics Capture, Direct3D 11/12,
ONNX Runtime DirectML, Dear ImGui, and AMD FidelityFX FSR 1. Model export uses
Python, PyTorch, NumPy, ONNX, and onnxsim. See
[third-party notes](THIRD_PARTY_NOTICES.md).

## Maintainer

Maintainer: Jacob Mathew ([JacobMathewCS](https://github.com/JacobMathewCS)).
The complete event team roster still needs to be confirmed before the submission
is finalized.

## AI assistance

Cline was used for code and test assistance, diagnostic analysis, and preparation
of this public release's documentation. AI-produced changes require review and
testing; they are not evidence that a feature works. The complete historical tool
list and contribution breakdown are still being checked with the maintainer, so
this section is not yet the final hackathon disclosure.

## License

Original project code and documentation are released under the [MIT license](LICENSE).
Third-party code and models retain their own licenses and notices.