# Local models (not distributed)

Keep production ONNX files here locally. They are excluded from Git and copied beside
the executable by CMake. Do not commit gameplay inputs, checkpoints or unreviewed weights.

The full-resolution reconstruction path uses:

- `RIFE_flow_576x320.onnx`: FP32 flow/mask model.
- `RIFE_flow_576x320_fp16.onnx`: opt-in FP16 variant selected by the launcher by default.

Inputs are `img0`, `img1`, RGB NCHW float32 tensors shaped `[1,3,320,576]`.
The flow export produces `flow_mask`, `[1,5,320,576]` (two xy flow fields and a mask).
Do not rename a generic RIFE interpolation model to these names: the reconstruction
shader depends on this specific output contract. Validate precision and tensor types
against the runtime before publishing a conversion.

`export_rife_flow.py` uses `export_rife_fg.py` to obtain the HDv3 source/checkpoint and
validates reconstruction before copying an FP32 export into the model directories.
It requires PyTorch, NumPy, ONNX, onnxsim and a DirectML-enabled ONNX Runtime Python
environment. Its `--device` argument is a DXGI adapter index and must match your system.
The default index of 1 is specific to the original development setup.

The historical FP16 conversion/validation experiments remain local in `scratch`; there
is not yet a self-contained, pinned FP16 production exporter in this repository. Preserve
the already validated local model, or use FP32 with the launcher's `-NoFp16` option after
generating the flow model. Rebuilding weights from moving upstream downloads is not
guaranteed reproducible. Review checkpoint provenance, licensing and download safety first.

Legacy image models and EDSR are not required for the normal full-resolution flow path.
Existing local files are retained for rollback rather than deleted during repository cleanup.