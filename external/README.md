# Local dependency layout

Downloaded dependencies are not included in Git. Preserve an existing working installation
when preparing the repository. Do not substitute unrelated runtime DLLs for matching headers.

CMake currently expects this layout under the checkout's `external` directory:

```text
imgui/
  imgui.cpp, imgui_draw.cpp, imgui_tables.cpp, imgui_widgets.cpp
  imgui.h, imgui_internal.h, imconfig.h, imstb_*.h
  imgui_impl_dx11.cpp, imgui_impl_dx11.h
fsr1/
  ffx_a.h, ffx_fsr1.h
onnxruntime-directml/
  include/   # matching ONNX Runtime C/C++ headers and dml_provider_factory.h
  lib/
    onnxruntime.lib
    onnxruntime.dll
    DirectML.dll
```

Dear ImGui DX11 backend files normally live in upstream's `backends` directory; this
project expects them alongside the other ImGui files. Retain upstream licenses locally.
The current local ImGui snapshot is 1.93.0 WIP (19297); its exact commit is not recorded.
FSR 1 headers came from GPUOpen-Effects/FidelityFX-FSR revision
`a21ffb8f6c13233ba336352bdff293894c706575`.

Obtain the Windows x64 ONNX Runtime DirectML SDK from the official
`Microsoft.ML.OnnxRuntime.DirectML` NuGet distribution, preserving its matching include,
import-library and runtime files in the layout above. Consult the official ONNX Runtime
DirectML installation documentation. The locally tested DLL reports
`1.24.20260316.9.2d92497`; a different package version must be rebuilt and tested, not assumed
equivalent. This project does not yet provide a pinned dependency bootstrap script.

These manual provisioning requirements are a known fresh-clone limitation, not an assertion
that the repository builds without dependencies. See the root third-party notice checklist
before including these files in any release.