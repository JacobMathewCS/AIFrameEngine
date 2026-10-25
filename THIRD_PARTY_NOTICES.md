# Third-party components

This source publication does not bundle downloaded third-party SDK source,
runtime libraries, or pretrained model weights. The project MIT license applies to
original AIFrameEngine work, not as a replacement for upstream licenses.

| Component | Role | Upstream |
| --- | --- | --- |
| RIFE HDv3-derived flow model | Motion estimation and blend mask | [ECCV2022-RIFE](https://github.com/hzwer/ECCV2022-RIFE), [code license](https://github.com/hzwer/ECCV2022-RIFE/blob/main/LICENSE) |
| ONNX Runtime DirectML | ONNX inference | [microsoft/onnxruntime](https://github.com/microsoft/onnxruntime) |
| Microsoft DirectML | GPU machine-learning runtime | [microsoft/DirectML](https://github.com/microsoft/DirectML) |
| Dear ImGui | Status overlay | [ocornut/imgui](https://github.com/ocornut/imgui) |
| AMD FidelityFX FSR 1 | EASU and RCAS shaders | [GPUOpen-Effects/FidelityFX-FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR) |
| PyTorch | Loading and exporting the pretrained model | [pytorch/pytorch](https://github.com/pytorch/pytorch) |
| NumPy | Export validation arrays | [numpy/numpy](https://github.com/numpy/numpy) |
| ONNX | Exported model representation and checking | [onnx/onnx](https://github.com/onnx/onnx) |
| onnxsim | Simplifying exported graphs | [onnxsim](https://github.com/daquexian/onnx-simplifier) |

Windows SDK APIs provide capture, graphics, and window management. The application
does not use a hosted AI API in its normal frame-generation path.

## Release preparation

The working FSR 1 headers were recorded at revision
`a21ffb8f6c13233ba336352bdff293894c706575`. The local ImGui header reports
`1.93.0 WIP (19297)`; its exact source revision still needs identification.
The local ONNX Runtime DLL reports `1.24.20260316.9.2d92497`; this file version alone
is not a pinned package specification.

Before dependency or binary bundling, retain the notices for the actual dependency
versions, document reproducible acquisition steps, and verify model distribution
terms separately. No training dataset or private gameplay capture is included.