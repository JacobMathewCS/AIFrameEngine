# Third-party provenance and release checklist

This repository does not grant rights to downloaded dependencies or model weights.
The dependency directories and production models are excluded from the initial source commit.

| Component | Role | Upstream identifier / local observation |
| --- | --- | --- |
| Dear ImGui | Status overlay | ocornut/imgui; local header reports 1.93.0 WIP (19297), exact commit not recorded |
| FidelityFX FSR 1 | EASU / RCAS shader headers | GPUOpen-Effects/FidelityFX-FSR, revision a21ffb8f6c13233ba336352bdff293894c706575 |
| ONNX Runtime DirectML | Inference runtime | Microsoft.ML.OnnxRuntime.DirectML; local DLL reports 1.24.20260316.9.2d92497 |
| Microsoft DirectML | D3D12 ML runtime | supplied with the local runtime package; retain its distribution notices |
| RIFE / HDv3 checkpoint | Flow/mask inference | hzwer/ECCV2022-RIFE plus the checkpoint referenced by export_rife_fg.py |

Before a binary or model release, obtain and retain the exact upstream license/notice files,
record pinned source/package revisions and hashes, and verify model/checkpoint redistribution
terms separately from code terms. The current exporter downloads a moving source branch;
its reproducibility and download integrity need hardening before promising automated setup.

The project has not selected its own license yet. Do not add an MIT (or other) license
on behalf of the owner or apply it to third-party weights. Existing upstream copyright
notices embedded in shader/source code must remain intact.