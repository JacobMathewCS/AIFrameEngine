# RIFE models

AIFrameEngine uses custom flow/mask exports of the pretrained HD checkpoint linked
by [ECCV2022-RIFE](https://github.com/hzwer/ECCV2022-RIFE#cli-usage).
The exporter loads `train_log.RIFE_HDv3.Model`; these are not newly trained models
and should not be described as a different RIFE release such as v4.9.

| Runtime file | Use | Local file size |
| --- | --- | --- |
| `RIFE_flow_576x320_fp16.onnx` | Default launcher configuration | 5,394,740 bytes |
| `RIFE_flow_576x320.onnx` | FP32 alternative | 10,677,715 bytes |

Both exports estimate two xy flow fields and a blend mask. The runtime contract
uses `img0` and `img1`, RGB NCHW inputs shaped `[1, 3, 320, 576]`, and `flow_mask`
output shaped `[1, 5, 320, 576]`. The FP16 filename describes the converted model;
its exact external tensor types must be checked before publishing it.

The original exporter references the upstream pretrained HD checkpoint with Google
Drive identifier `1APIzVeI-4ZZCEuIRE1m6WYfSCaOsi_7_`. The upstream README distinguishes
this HD checkpoint from the model used for the paper's reported evaluation.

## License and availability

The upstream code carries an [MIT license](https://github.com/hzwer/ECCV2022-RIFE/blob/main/LICENSE)
with copyright attributed to Megvii Inc. Model provenance and the applicable
checkpoint terms will be recorded before redistributing the converted weights;
the root project license is not a replacement for upstream permissions.

No pretrained models are downloadable from this repository at this publication stage. The
model release needs checksums, retained notices, and validated provisioning steps.
Generic RIFE image-output models are not interchangeable with these flow exports.

EDSR, legacy image-output RIFE models, and corrupted backups are not needed for the
normal flow-reconstruction configuration and will not be included in that package.

## Manual setup and exporter limits

Place the two flow files listed above in this directory before building the full
application. Both are required for the default FP16 launcher selection: the current
compositor checks for the FP32 file before enabling the flow path. Choose FP32 in
the launcher if only the FP32 flow export is available.

The included `export_rife_flow.py` and `export_rife_fg.py` are research exporters,
not a pinned, one-command provisioning system. The flow exporter requires Python,
PyTorch, NumPy, ONNX, onnxsim, and an ONNX Runtime Python installation with the
DirectML provider. It downloads upstream source/checkpoint code and executes that
code: inspect it and verify provenance before use. Its default GPU device index is
1; specify `--device` for your actual DirectML adapter. It validates reconstruction
and inference before copying the FP32 result into local model/build folders.

It does not currently provide the FP16 conversion/provisioning stage. Upstream
source is not pinned, export dependencies are not locked, and clean-machine export
has not been validated for this publication. These are release blockers for an
automatic setup or ready-to-run bundle, not requirements silently handled by CMake.