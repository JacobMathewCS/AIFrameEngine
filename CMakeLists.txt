cmake_minimum_required(VERSION 3.18)
project(AIFrameEngine LANGUAGES CXX)
include(CTest)

if(NOT WIN32)
    message(FATAL_ERROR "AIFrameEngine requires Windows and the Windows SDK.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Standalone GUI: build without downloading ML/graphics dependencies or touching
# an existing compositor. Normal builds also include this target.
option(AIFRAME_LAUNCHER_ONLY "Build only the launcher and its CPU/UI tests" OFF)
add_executable(AIFrameEngineLauncher WIN32 src/launcher.cpp)
target_link_libraries(AIFrameEngineLauncher PRIVATE user32.lib gdi32.lib comctl32.lib
    comdlg32.lib shell32.lib dwmapi.lib)
target_compile_definitions(AIFrameEngineLauncher PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(AIFrameEngineLauncher PRIVATE /W4 /utf-8)
    target_link_options(AIFrameEngineLauncher PRIVATE
        "/MANIFEST:EMBED" "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/launcher.manifest")
endif()
if(BUILD_TESTING)
    add_executable(LauncherConfigTest tests/launcher_config_test.cpp)
    target_link_libraries(LauncherConfigTest PRIVATE user32.lib)
    target_compile_definitions(LauncherConfigTest PRIVATE UNICODE _UNICODE NOMINMAX)
    if(MSVC)
        target_compile_options(LauncherConfigTest PRIVATE /W4 /utf-8)
    endif()
    add_test(NAME LauncherConfigTest COMMAND LauncherConfigTest)
    add_test(NAME LauncherUiSmokeTest COMMAND AIFrameEngineLauncher --smoke-test)
    set_tests_properties(LauncherConfigTest LauncherUiSmokeTest PROPERTIES TIMEOUT 20)
endif()
if(AIFRAME_LAUNCHER_ONLY)
    return()
endif()

if(BUILD_TESTING)
    add_executable(SharpnessControlTest tests/sharpness_control_test.cpp)
    add_test(NAME SharpnessControlTest COMMAND SharpnessControlTest)
    add_executable(FrameIdentityTest tests/frame_identity_test.cpp)
    add_test(NAME FrameIdentityTest COMMAND FrameIdentityTest)
    add_executable(CaptureTargetTest tests/capture_target_test.cpp)
    target_link_libraries(CaptureTargetTest PRIVATE user32.lib)
    add_test(NAME CaptureTargetTest COMMAND CaptureTargetTest)
    add_executable(FsrUpscalerTest tests/fsr_upscaler_test.cpp)
    target_link_libraries(FsrUpscalerTest PRIVATE d3d11.lib d3dcompiler.lib)
    add_test(NAME FsrUpscalerTest COMMAND FsrUpscalerTest)
    add_executable(PlaybackClockTest tests/playback_clock_test.cpp)
    add_test(NAME PlaybackClockTest COMMAND PlaybackClockTest)
    add_executable(DisplayCacheTest tests/display_cache_test.cpp)
    target_link_libraries(DisplayCacheTest PRIVATE d3d11.lib)
    add_test(NAME DisplayCacheTest COMMAND DisplayCacheTest)
    add_executable(RepeatCopyProbeTest tests/repeat_copy_probe_test.cpp)
    add_test(NAME RepeatCopyProbeTest COMMAND RepeatCopyProbeTest)
    add_executable(WorkerCpuProbeTest tests/worker_cpu_probe_test.cpp)
    add_test(NAME WorkerCpuProbeTest COMMAND WorkerCpuProbeTest)
    add_executable(CaptureCropTest tests/capture_crop_test.cpp)
    add_test(NAME CaptureCropTest COMMAND CaptureCropTest)
    add_executable(PresentationWaitTest tests/presentation_wait_test.cpp)
    target_link_libraries(PresentationWaitTest PRIVATE user32.lib)
    add_test(NAME PresentationWaitTest COMMAND PresentationWaitTest)
    add_executable(FrameTimelineTest tests/frame_timeline_test.cpp)
    add_test(NAME FrameTimelineTest COMMAND FrameTimelineTest)
    add_executable(FgDiagnosticsTest tests/fg_diagnostics_test.cpp)
    add_test(NAME FgDiagnosticsTest COMMAND FgDiagnosticsTest)
    add_executable(RifeAttemptTraceTest tests/rife_attempt_trace_test.cpp)
    add_test(NAME RifeAttemptTraceTest COMMAND RifeAttemptTraceTest)
    add_executable(RifePreprocessorTest tests/rife_preprocessor_test.cpp)
    target_link_libraries(RifePreprocessorTest PRIVATE d3d11.lib d3dcompiler.lib)
    add_test(NAME RifePreprocessorTest COMMAND RifePreprocessorTest)
    add_executable(RifeReconstructorTest tests/rife_reconstructor_test.cpp)
    target_link_libraries(RifeReconstructorTest PRIVATE d3d11.lib d3dcompiler.lib)
    add_test(NAME RifeReconstructorTest COMMAND RifeReconstructorTest)
endif()

if(MSVC)
    add_compile_options(/W3 /utf-8 /D_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS)
    # Use the C++/WinRT headers supplied by the selected Windows SDK.
endif()

set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external")

add_library(StatusOverlayUi STATIC
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/imgui_impl_dx11.cpp)
target_include_directories(StatusOverlayUi PUBLIC "${EXTERNAL_DIR}/imgui")
target_link_libraries(StatusOverlayUi PUBLIC d3d11.lib d3dcompiler.lib)
if(BUILD_TESTING)
    add_executable(StatusOverlayTest tests/status_overlay_test.cpp)
    target_link_libraries(StatusOverlayTest PRIVATE StatusOverlayUi)
    add_test(NAME StatusOverlayTest COMMAND StatusOverlayTest)
endif()

include_directories(
    "${EXTERNAL_DIR}/onnxruntime-directml"
    "${EXTERNAL_DIR}/onnxruntime-directml/include"
    "${EXTERNAL_DIR}/onnxruntime-directml/include/onnxruntime"
)

add_executable(AIFrameEngineCompositor WIN32
    ${SRC_DIR}/compositor.cpp
)

target_link_libraries(AIFrameEngineCompositor PRIVATE
    StatusOverlayUi
    d3d12.lib
    d3d11.lib
    dxgi.lib
    dwmapi.lib
    d3dcompiler.lib
    windowsapp.lib
    "${EXTERNAL_DIR}/onnxruntime-directml/lib/onnxruntime.lib"
)

set_target_properties(AIFrameEngineCompositor PROPERTIES
    OUTPUT_NAME "AIFrameEngineCompositor"
)

# Deploy shaders at configure time as well as after app builds (test-only builds).
foreach(config Debug Release RelWithDebInfo MinSizeRel)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/${config}/shaders")
    configure_file(src/shaders/fsr1.hlsl "${CMAKE_BINARY_DIR}/${config}/shaders/fsr1.hlsl" COPYONLY)
    configure_file(external/fsr1/ffx_a.h "${CMAKE_BINARY_DIR}/${config}/shaders/ffx_a.h" COPYONLY)
    configure_file(external/fsr1/ffx_fsr1.h "${CMAKE_BINARY_DIR}/${config}/shaders/ffx_fsr1.h" COPYONLY)
endforeach()

add_custom_command(TARGET AIFrameEngineCompositor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/external/fsr1" "$<TARGET_FILE_DIR:AIFrameEngineCompositor>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SRC_DIR}/shaders/fsr1.hlsl" "$<TARGET_FILE_DIR:AIFrameEngineCompositor>/shaders/fsr1.hlsl"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${EXTERNAL_DIR}/onnxruntime-directml/lib/onnxruntime.dll"
        "$<TARGET_FILE_DIR:AIFrameEngineCompositor>/onnxruntime.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${EXTERNAL_DIR}/onnxruntime-directml/lib/DirectML.dll"
        "$<TARGET_FILE_DIR:AIFrameEngineCompositor>/DirectML.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/models"
        "$<TARGET_FILE_DIR:AIFrameEngineCompositor>/models"
)

if(BUILD_TESTING)
    add_executable(RifeIoBuffersTest tests/rife_io_buffers_test.cpp)
    add_executable(DmlGpuIoTest tests/dml_gpu_io_test.cpp)
    add_executable(DmlQueueBench tests/dml_queue_bench.cpp)
    target_link_libraries(DmlQueueBench PRIVATE d3d12.lib dxgi.lib
        "${EXTERNAL_DIR}/onnxruntime-directml/lib/onnxruntime.lib")
    add_dependencies(DmlQueueBench RifeIoBuffersTest)
    target_link_libraries(DmlGpuIoTest PRIVATE d3d12.lib dxgi.lib
        "${EXTERNAL_DIR}/onnxruntime-directml/lib/onnxruntime.lib")
    add_dependencies(DmlGpuIoTest RifeIoBuffersTest)
    # Hardware validation is explicit: pass fixture/model and the desired DXGI index.
    target_link_libraries(RifeIoBuffersTest PRIVATE "${EXTERNAL_DIR}/onnxruntime-directml/lib/onnxruntime.lib")
    add_custom_command(TARGET RifeIoBuffersTest POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${EXTERNAL_DIR}/onnxruntime-directml/lib/onnxruntime.dll"
            "$<TARGET_FILE_DIR:RifeIoBuffersTest>/onnxruntime.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${EXTERNAL_DIR}/onnxruntime-directml/lib/DirectML.dll"
            "$<TARGET_FILE_DIR:RifeIoBuffersTest>/DirectML.dll")
    add_test(NAME RifeIoBuffersTest COMMAND RifeIoBuffersTest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/rife_io_add.onnx")
endif()
