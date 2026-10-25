# Desktop launcher

The native Windows launcher provides a window picker, FSR 1/bicubic selection,
FP16/FP32 selection, display caching, runtime-file checks, and help/log access.
It is separate from the compositor. The Ctrl+Shift+F6 in-game status panel and all
existing gameplay hotkeys are unchanged.

## Build without changing the working compositor

From the project root, using CMake and Visual Studio C++ build tools:

```powershell
$root = (Get-Location).Path
cmake -S "$root" -B "$root\build-launcher" -A x64 -DAIFRAME_LAUNCHER_ONLY=ON
cmake --build "$root\build-launcher" --config Release
ctest --test-dir "$root\build-launcher" -C Release --output-on-failure
& "$root\build-launcher\Release\AIFrameEngineLauncher.exe" --compositor "$root\build\Release\AIFrameEngineCompositor.exe"
```

The standalone build does not need ImGui, ONNX Runtime, models, or FSR headers.
It does not build, deploy, or overwrite the compositor. Normal full builds also
include the launcher alongside the compositor. By default it looks for the
compositor beside itself; Browse selects an existing build elsewhere.

## Use

1. Start a game in windowed mode and restore its window if minimized.
2. Select its title in the launcher. Refresh updates the list; it never silently
   picks a different game if the previous window disappears.
3. Leave FSR 1, FP16, and caching selected for the usual configuration.
4. Click Launch compositor. The launcher minimizes after successful process creation.
5. Check readiness using Ctrl+Shift+F6, then press F9 to enable frame generation.
6. Ctrl+Shift+F12 closes the compositor. Closing the launcher alone does not stop it.

The launcher reports process lifetime, not model readiness or actual FG state.
Use the existing in-game panel for those. It does not change game resolution,
set the game's FPS cap, or promise a particular output FPS. Native 1080p FG is
still unreliable; 720p remains the best-tested configuration.

Preflight checks for nonempty runtime DLLs, the FP32 flow model, the selected FP16
model, and (for FSR 1) the three shader files. The FP32 file is required even with
FP16 because the current compositor uses it as the flow-path availability gate.
Presence checks do not validate model contents or driver compatibility.

GUI launches remove inherited `AIFRAME_*` values only from the new child's
environment, then set the selected options and pinned window/process identity.
This prevents stale diagnostics or trial overrides from affecting normal use.
No user/machine variables are changed. Use the unchanged PowerShell launcher for
custom diagnostics or adapter overrides. The compositor's normal automatic
adapter selection is retained.

## Validation

`LauncherConfigTest` checks environment isolation, runtime prerequisites, stale
target rejection, launch serialization, and a real harmless child process under a
Unicode path with spaces. `LauncherUiSmokeTest` opens and closes only the launcher;
its smoke mode cannot start the compositor. Neither test launches capture or
inference. Tests require an interactive Windows desktop for window fixtures.

Manual checks before release: window selection and Refresh, keyboard navigation,
display scaling, missing-file messages, launch/exit/relaunch against a real game,
and unchanged Ctrl+Shift+F6 behavior. Offline tests are not gameplay validation.