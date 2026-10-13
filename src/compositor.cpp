#include <windows.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <windows.graphics.capture.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <onnxruntime_cxx_api.h>
#include "rife_io_buffers.h"
#include "dml_gpu_io.h"
#include "display_cache.h"
#include "fsr_upscaler.h"
#include "upscale_comparison.h"
#include "status_overlay.h"
#include "frame_identity.h"
#include "sharpness_control.h"
#include <dml_provider_factory.h>

#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <array>
#include <stdexcept>
#include <fstream>
#include <filesystem>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "frame_timeline.h"
#include "frame_job_admission.h"
#include "latest_frame_request.h"
#include "rife_reconstructor.h"
#include "rife_preprocessor.h"
#include "presentation_guard.h"
#include "frame_clock.h"
#include "presentation_wait.h"
#include "capture_crop.h"
#include "capture_target.h"
#include "inference_slot.h"
#include "adaptive_latency.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowsapp.lib")

using Microsoft::WRL::ComPtr;
using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

static void LogCapture(const std::string& message) {
    static std::mutex logMutex;
    std::lock_guard<std::mutex> lock(logMutex);
    CreateDirectoryW(L"C:\\Temp", nullptr);
    std::ofstream log("C:\\Temp\\aiframe_compositor.log", std::ios::app);
    if (log.is_open()) log << message << std::endl;
}

// One-shot diagnostic so repeated startup/geometry notes don't flood the log.
static void LogOnce(const std::string& message) {
    static std::once_flag flag;
    std::call_once(flag, [&]() { LogCapture(message); });
}

// Must run before any window or monitor is queried. If the process is
// DPI-unaware, Windows virtualizes coordinates: GetMonitorInfo reports a
// scaled rectangle (e.g. 1536x864 on a 1920x1080 panel at 125%), so the swap
// chain is built at that size and DWM stretches our carefully upscaled/sharpened
// frame up to the real panel with plain bilinear - undoing the whole scaler.
static void EnableNativeDpi() {
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto setContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        // (HANDLE)-4 == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        if (setContext && setContext(reinterpret_cast<HANDLE>(-4))) return;
    }
    SetProcessDPIAware();
}

static int64_t GetCurrentTime100ns() {
    LARGE_INTEGER qpc;
    static const int64_t frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    QueryPerformanceCounter(&qpc);
    return QpcTo100ns(qpc.QuadPart, frequency);
}

// ONNX inference must run on the fastest adapter in the machine. On hybrid
// laptops DirectML device 0 is usually the integrated GPU, which is many times
// slower than the discrete GPU for RIFE. Pick the adapter with the most
// dedicated VRAM, and allow an explicit override via AIFRAME_DML_DEVICE.
static int SelectBestAdapterIndex() {
    char override[16] = {};
    const DWORD overrideLength = GetEnvironmentVariableA("AIFRAME_DML_DEVICE", override, sizeof(override));
    if (overrideLength > 0 && overrideLength < sizeof(override)) {
        const int forced = std::atoi(override);
        if (forced >= 0) return forced;
    }

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 0;
    int bestIndex = 0;
    long long bestScore = -1;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc = {};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        const long long memory = static_cast<long long>(desc.DedicatedVideoMemory);
        const long long score = (memory > 0) ? memory : 1;
        if (score > bestScore) {
            bestScore = score;
            bestIndex = static_cast<int>(index);
        }
    }
    return bestIndex;
}

static std::string DescribeAdapter(int index) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return "unknown";
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(static_cast<UINT>(index), &adapter))) return "unknown";
    DXGI_ADAPTER_DESC1 desc = {};
    if (FAILED(adapter->GetDesc1(&desc))) return "unknown";
    char name[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
    return std::string(name);
}

struct ScheduledFrame {
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame captureFrame{ nullptr };
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    int64_t captureTime = 0;
};

struct Vertex { float position[2]; float uv[2]; };

struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) IDirect3DDxgiInterfaceAccess : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID id, void** object) = 0;
};

// RIFE runs at a fixed internal resolution so its ONNX graph is fully static
// (no Shape/Gather/Unsqueeze chains) and therefore DirectML-safe. Each axis
// must stay divisible by 64 for RIFE's multi-scale flow pyramid.
static constexpr UINT kRifeWidth = 576;
static constexpr UINT kRifeHeight = 320;

// Upscaling uses GPU shaders; cost must be measured with frame generation.
// The EDSR CPU path is a downsample->upsample round trip with
// no net upscale and costs ~98 ms per run (411 ms if run at native capture), so
// it is off by default. Set to true to restore it for comparison.
static constexpr bool kEnableEdsrUpscale = false;

// Displayed destination size + scaler mode, uploaded to the pixel shader.
// Toggled at runtime with F10 so bicubic vs bilinear can be compared live.
struct ScalerConstants {
    float dstSize[2];
    float bicubicEnabled;
    float sharpness;   // 0 disables RCAS
};


class Compositor {
public:
    bool Initialize(HINSTANCE instance) {
        instance_ = instance;
        completionEvent_.attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!completionEvent_) { startupError_ = L"inference completion event"; return false; }
        init_apartment(apartment_type::multi_threaded);
        wchar_t handleText[32] = {}, pidText[32] = {}, creationText[32] = {};
        const DWORD nh = GetEnvironmentVariableW(L"AIFRAME_TARGET_HWND", handleText, 32);
        const DWORD np = GetEnvironmentVariableW(L"AIFRAME_TARGET_PID", pidText, 32);
        const DWORD nc = GetEnvironmentVariableW(L"AIFRAME_TARGET_CREATED", creationText, 32);
        if (nh || np || nc) {
            uint64_t handle = 0, pid = 0, creation = 0;
            if (nh >= 32 || np >= 32 || nc >= 32 || !ParseTargetNumber(handleText, handle) ||
                !ParseTargetNumber(pidText, pid) || !ParseTargetNumber(creationText, creation) ||
                pid > MAXDWORD || handle > (std::numeric_limits<uintptr_t>::max)() ||
                !captureTarget_.Bind(reinterpret_cast<HWND>(static_cast<uintptr_t>(handle)),
                    static_cast<DWORD>(pid), creation)) {
                startupError_ = L"selected target invalid or closed; reselect a window";
                return false;
            }
            targetLabel_ = to_string(captureTarget_.Title());
            LogCapture("Capture target selected: " + targetLabel_ + " (PID " + std::to_string(pid) + ").");
        }
        if (!CreateOutputWindow()) {
            startupError_ = L"output window";
            return false;
        }
        if (!CreateGraphics()) {
            if (startupError_.empty()) startupError_ = L"D3D11 graphics or swapchain";
            return false;
        }
        return true;
    }

    int Run() {
        LogCapture("Compositor render loop started.");
        identities_.BeginSegment(GetCurrentTime100ns());
        wchar_t tracePath[32768] = {};
        const DWORD traceLength = GetEnvironmentVariableW(L"AIFRAME_PRESENT_TRACE", tracePath, 32768);
        if (traceLength && traceLength < 32768) {
            presentTrace_.open(std::filesystem::path(tracePath), std::ios::out | std::ios::trunc);
            if (presentTrace_) {
                presentTrace_ << "sequence,pid,segment,fg_enabled,display_mode,pair,content_100ns,generated,has_image,qpc_start,qpc_end,qpc_frequency,hresult\n";
                LogCapture("Presentation identity trace enabled (submission only): " + to_string(tracePath));
            } else LogCapture("Presentation identity trace could not be opened; continuing without CSV.");
        }
        if (!statusOverlay_.Initialize(device_.Get(), context_.Get()))
            LogCapture("Status panel renderer unavailable; gameplay continues without overlay.");
        if (!RegisterHotKey(window_, 7, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F6))
            LogCapture("Ctrl+Shift+F6 registration failed; status panel hotkey unavailable.");
        LogCapture("Status panel: hidden; Ctrl+Shift+F6 toggles (read-only)." );
        modelThread_ = std::thread([this]() {
            modelReady_ = InitializeModel();
        });
        RegisterHotKey(window_, 1, MOD_CONTROL | MOD_SHIFT, VK_F12);
        admission_.SetLatencyController(&adaptiveLatency_);
        RegisterHotKey(window_, 2, MOD_NOREPEAT, VK_F9);
        RegisterHotKey(window_, 3, 0, VK_F10);
        RegisterHotKey(window_, 4, MOD_NOREPEAT, VK_F11);
        if (!RegisterHotKey(window_, 8, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_UP))
            LogCapture("Ctrl+Shift+Up registration failed; fine sharpen increase unavailable.");
        if (!RegisterHotKey(window_, 9, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_DOWN))
            LogCapture("Ctrl+Shift+Down registration failed; fine sharpen decrease unavailable.");
        if (!RegisterHotKey(window_, 6, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F7))
            LogCapture("Ctrl+Shift+F7 registration failed; upscale export unavailable.");
        if (!RegisterHotKey(window_, 5, MOD_NOREPEAT, VK_F8))
            LogCapture("F8 registration failed; FG comparison hotkey unavailable.");
        MSG message = {};
        while (running_) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT || (message.message == WM_HOTKEY && message.wParam == 1)) running_ = false;
                if (message.message == WM_HOTKEY && message.wParam == 2) {
                    statusRate_.Reset();
                    identities_.BeginSegment(GetCurrentTime100ns());
                    frameGenerationEnabled_ = !frameGenerationEnabled_ && modelReady_.load() && rifeAvailable_.load();
                    timeline_.Clear();
                    latestRequest_.Clear();
                    ++pairId_; // Never reuse identities across an enable/disable boundary.
                    admission_.Reset();
                    timeline_.SetDelay(adaptiveLatency_.GetDelay());
                    firstEnabledPairId_ = pairId_ + 1;
                    statsTime_ = GetCurrentTime100ns();
                    statsPresented_ = presentedRifeFrames_;
                    LogCapture(frameGenerationEnabled_
                        ? "RIFE frame generation enabled (576x320; buffered 30->60 test; adaptive added delay)."
                        : "RIFE frame generation disabled or model not ready.");
                    if (frameGenerationEnabled_) LogCapture(std::string("FG display mode: ") + FgDisplayModeName());
                }
                if (message.message == WM_HOTKEY && message.wParam == 5) {
                    statusRate_.Reset();
                    identities_.BeginSegment(GetCurrentTime100ns());
                    fgDisplayMode_ = (fgDisplayMode_ + 1) % 3;
                    if (fgDisplayMode_ == 2 && !comparisonReady_) {
                        std::string error;
                        comparisonReady_ = comparisonResize_.Initialize(device_.Get(), kRifeWidth, kRifeHeight, error);
                        if (!comparisonReady_) {
                            LogCapture("FG matched-resolution diagnostic unavailable: " + error);
                            fgDisplayMode_ = 0;
                        }
                    }
                    // A mode change starts at a real image, not a retained
                    // generated image. Hold it until the delayed clock catches up.
                    presentation_.Commit({frameB_.view, pairId_, false, frameB_.captureTime});
                    statsTime_ = GetCurrentTime100ns();
                    statsPresented_ = presentedRifeFrames_;
                    LogCapture(std::string("FG display mode: ") + FgDisplayModeName());
                }
                if (message.message == WM_HOTKEY && message.wParam == 3) {
                    scalerBicubic_ = !scalerBicubic_;
                    UpdateScalerConstants();
                    LogCapture(fsrEnabled_ ? (scalerBicubic_ ? "Upscaling: FSR1 EASU + optional RCAS." : "Upscaling: bilinear + optional reference RCAS.") : scalerBicubic_
                        ? "Upscaling: Catmull-Rom bicubic (GPU)."
                        : "Upscaling: bilinear (A/B compare).");
                }
                if (message.message == WM_HOTKEY &&
                    (message.wParam == 4 || message.wParam == 8 || message.wParam == 9)) {
                    scalerSharpness_ = message.wParam == 4
                        ? SharpnessControl::NextPreset(scalerSharpness_)
                        : SharpnessControl::Adjust(scalerSharpness_, message.wParam == 8);
                    UpdateScalerConstants();
                    LogCapture(scalerSharpness_ > 0.0f
                        ? "Upscale sharpen: strength " + std::to_string(scalerSharpness_) + " (F11 presets; Ctrl+Shift+Up/Down fine adjustment)."
                        : std::string("Upscale sharpen: off (F11 presets; Ctrl+Shift+Up/Down fine adjustment)."));
                }
                TranslateMessage(&message);
                if (message.message == WM_HOTKEY && message.wParam == 7) {
                    statusVisible_ = !statusVisible_;
                    LogCapture(statusVisible_ ? "Status panel: visible." : "Status panel: hidden.");
                }
                if (message.message == WM_HOTKEY && message.wParam == 6) ExportUpscaleComparison();
                DispatchMessageW(&message);
            }
            if (!running_) break;
            if (captureTarget_.Bound() && !captureTarget_.Valid()) {
                LogCapture("Selected target closed; exiting without selecting another window.");
                break;
            }
            if (modelReady_ && !inferenceStarted_) {
                inferenceStarted_ = true;
                inferenceThread_ = std::thread([this]() { InferenceLoop(); });
            }
            try {
                // Consume before a new job can finish and replace the single
                // result slot, then again in Render after capture/readback.
                ConsumeProcessedFrame();
                EnsureCapture();
                CaptureLatestFrame();
                // Retry a still-useful pair even on a tick with no new capture.
                // Consume again before submission so the single result slot is
                // never intentionally overwritten by a subsequent worker job.
                ConsumeProcessedFrame();
                if (frameGenerationEnabled_ && latestRequest_.Pending(pairId_))
                    QueueLatestFrameForInference();
            } catch (const winrt::hresult_error& error) {
                LogCapture("Windows Graphics Capture error: " + std::string(to_string(error.message())));
                if (captureSession_) captureSession_.Close();
                if (framePool_) framePool_.Close();
                captureItem_ = nullptr;
                targetWindow_ = nullptr;
                latestRequest_.Clear();
                running_ = false; // Explicit relaunch; never silently change target.
            } catch (...) {
                LogCapture("Unknown capture error.");
                if (captureSession_) captureSession_.Close();
                if (framePool_) framePool_.Close();
                captureItem_ = nullptr;
                targetWindow_ = nullptr;
                latestRequest_.Clear();
                running_ = false;
            }
            if (!running_) break;
            if (!frameLatencyEvent_ || displayReady_) {
                Render();
                displayReady_ = false;
            }
            if (frameLatencyEvent_) {
                switch (WaitForPresentation(completionEvent_.get(), frameLatencyEvent_.get())) {
                case PresentationWake::Display: displayReady_ = true; break;
                case PresentationWake::Completion: ++completionWakes_; break;
                case PresentationWake::Failed:
                    LogCapture("Presentation wait failed; using legacy vsync pacing.");
                    frameLatencyEvent_.close();
                    break;
                default: break; // messages or bounded capture poll
                }
            } else Sleep(1);
        }
        UnregisterHotKey(window_, 1);
        UnregisterHotKey(window_, 2);
        UnregisterHotKey(window_, 3);
        UnregisterHotKey(window_, 4);
        UnregisterHotKey(window_, 5);
        UnregisterHotKey(window_, 6);
        UnregisterHotKey(window_, 7);
        UnregisterHotKey(window_, 8);
        UnregisterHotKey(window_, 9);
        statusOverlay_.Shutdown();
        if (systemCursorHidden_) {
            while (ShowCursor(TRUE) < 0) {}
            systemCursorHidden_ = false;
        }
        DestroyWindow(window_);
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            stopInference_ = true;
        }
        queueCv_.notify_one();
        if (inferenceThread_.joinable()) inferenceThread_.join();
        if (modelThread_.joinable()) modelThread_.join();
        if (rifeProfileEnabled_ && rifeSession_) {
            try {
                Ort::AllocatorWithDefaultOptions allocator;
                auto path = rifeSession_->EndProfilingAllocated(allocator);
                LogCapture(std::string("RIFE runtime profile saved: ") + path.get());
            } catch (const Ort::Exception& error) {
                LogCapture(std::string("RIFE profile finalization failed: ") + error.what());
            }
        }
        return 0;
    }

private:
    void ForwardMouseMessage(UINT message, WPARAM wParam) {
        if (!targetWindow_ || !IsWindow(targetWindow_)) return;
        POINT cursor = {};
        GetCursorPos(&cursor);
        ScreenToClient(targetWindow_, &cursor);
        LPARAM coordinates = MAKELPARAM(cursor.x, cursor.y);
        PostMessageW(targetWindow_, message, wParam, coordinates);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto compositor = reinterpret_cast<Compositor*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        if (message == WM_NCHITTEST) return HTTRANSPARENT;
        if (message == WM_SETCURSOR) {
            SetCursor(nullptr);
            return TRUE;
        }
        if (message == WM_DESTROY) PostQuitMessage(0);
        return DefWindowProcW(window, message, wParam, lParam);
    }

    bool CreateOutputWindow() {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.hInstance = instance_;
        wc.lpfnWndProc = WindowProc;
        wc.lpszClassName = L"AIFrameEngineCompositorWindow";
        RegisterClassExW(&wc);

        MONITORINFO monitor = { sizeof(monitor) };
        GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &monitor);
        monitorRect_ = monitor.rcMonitor;
        window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            wc.lpszClassName, L"AIFrameEngine Compositor", WS_POPUP,
            monitorRect_.left, monitorRect_.top,
            monitorRect_.right - monitorRect_.left,
            monitorRect_.bottom - monitorRect_.top,
            nullptr, nullptr, instance_, this);
        if (!window_) return false;
        ShowWindow(window_, SW_SHOW);
        SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        return true;
    }

    bool CreateGraphics() {
        LogCapture("CreateGraphics: starting");
        const D3D_FEATURE_LEVEL requestedLevels[] = { D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL createdLevel = D3D_FEATURE_LEVEL_11_0;
        dmlDevice_ = SelectBestAdapterIndex();
        ComPtr<IDXGIFactory1> adapterFactory;
        HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&adapterFactory));
        if (FAILED(result)) return GraphicsFailure(L"Create adapter factory", result);
        ComPtr<IDXGIAdapter1> selectedAdapter;
        result = adapterFactory->EnumAdapters1(static_cast<UINT>(dmlDevice_), &selectedAdapter);
        if (FAILED(result)) return GraphicsFailure(L"Select graphics/DirectML adapter", result);
        result = D3D11CreateDevice(selectedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, requestedLevels, ARRAYSIZE(requestedLevels), D3D11_SDK_VERSION,
            &device_, &createdLevel, &context_);
        if (FAILED(result)) return GraphicsFailure(L"D3D11CreateDevice", result);
        LogCapture("CreateGraphics: device created");

        ComPtr<IDXGIDevice> dxgiDevice;
        result = device_.As(&dxgiDevice);
        if (FAILED(result)) return GraphicsFailure(L"Query IDXGIDevice", result);
        ComPtr<IDXGIAdapter> adapter;
        result = dxgiDevice->GetAdapter(&adapter);
        if (FAILED(result)) return GraphicsFailure(L"GetAdapter", result);
        DXGI_ADAPTER_DESC actualDesc = {};
        adapter->GetDesc(&actualDesc);
        LogCapture("Graphics/capture adapter " + std::to_string(dmlDevice_) + ": " + DescribeAdapter(dmlDevice_)
            + " LUID=" + std::to_string(actualDesc.AdapterLuid.HighPart) + ":"
            + std::to_string(actualDesc.AdapterLuid.LowPart) + "; same index selected for DirectML.");
        ComPtr<IDXGIFactory2> factory;
        result = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(result)) return GraphicsFailure(L"GetParent IDXGIFactory2", result);

        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferCount = 2;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        char legacyPacing[8] = {};
        GetEnvironmentVariableA("AIFRAME_LEGACY_PACING", legacyPacing, sizeof(legacyPacing));
        if (std::strcmp(legacyPacing, "1") != 0)
            desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        result = factory->CreateSwapChainForHwnd(device_.Get(), window_, &desc, nullptr, nullptr, &swapChain_);
        if (FAILED(result) && desc.Flags) {
            desc.Flags = 0;
            result = factory->CreateSwapChainForHwnd(device_.Get(), window_, &desc, nullptr, nullptr, &swapChain_);
        }
        if (FAILED(result)) return GraphicsFailure(L"CreateSwapChainForHwnd", result);
        if (desc.Flags) {
            ComPtr<IDXGISwapChain2> latencySwapChain;
            if (SUCCEEDED(swapChain_.As(&latencySwapChain)) && SUCCEEDED(latencySwapChain->SetMaximumFrameLatency(1)))
                frameLatencyEvent_.attach(latencySwapChain->GetFrameLatencyWaitableObject());
        }
        LogCapture(frameLatencyEvent_ ? "Presentation pacing: waitable swapchain, latency=1; completion-aware wait, vsync retained."
                                     : "Presentation pacing: legacy vsync loop.");
        LogCapture("CreateGraphics: swapchain created");

        ComPtr<ID3D11Texture2D> backBuffer;
        result = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(result)) return GraphicsFailure(L"Get swapchain back buffer", result);
        result = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget_);
        if (FAILED(result)) return GraphicsFailure(L"CreateRenderTargetView", result);
        LogCapture("CreateGraphics: render target created");
        displayBackBuffer_ = backBuffer;
        char cacheFlag[8] = {};
        GetEnvironmentVariableA("AIFRAME_DISPLAY_CACHE", cacheFlag, sizeof(cacheFlag));
        displayCacheEnabled_ = std::strcmp(cacheFlag, "1") == 0;
        LogCapture(displayCacheEnabled_ ? "Display cache: enabled (exact final-pixel copy)."
                                       : "Display cache: disabled (baseline).");

        if (!CreatePipeline()) return GraphicsFailure(L"CreatePipeline", E_FAIL);
        char cpuPreprocess[8] = {};
        GetEnvironmentVariableA("AIFRAME_CPU_PREPROCESS", cpuPreprocess, sizeof(cpuPreprocess));
        gpuPreprocessing_ = std::strcmp(cpuPreprocess, "1") != 0;
        if (gpuPreprocessing_) {
            std::string error;
            if (!rifePreprocessor_.Initialize(device_.Get(), kRifeWidth, kRifeHeight, error)) {
                LogCapture("RIFE GPU preprocessing unavailable, using CPU path: " + error);
                gpuPreprocessing_ = false;
            }
        }
        LogCapture(gpuPreprocessing_ ? "RIFE preprocessing: GPU bilinear 576x320 + compact readback/cache."
                                    : "RIFE preprocessing: legacy full-resolution CPU path.");
        LogCapture("CreateGraphics: pipeline created");
        return true;
    }

    bool GraphicsFailure(const wchar_t* stage, HRESULT result) {
        wchar_t message[256] = {};
        swprintf_s(message, L"%ls (HRESULT 0x%08lX)", stage, static_cast<unsigned long>(result));
        startupError_ = message;
        return false;
    }

    bool InitializeModel() {
        LogCapture(std::string("ONNX Runtime version: ") + OrtGetApiBase()->GetVersionString());
        char profile[8] = {}, allocating[8] = {};
        GetEnvironmentVariableA("AIFRAME_ORT_PROFILE", profile, sizeof(profile));
        GetEnvironmentVariableA("AIFRAME_ALLOCATING_IO", allocating, sizeof(allocating));
        rifeProfileEnabled_ = std::strcmp(profile, "1") == 0;
        reuseRifeIo_ = std::strcmp(allocating, "1") != 0;
        LogCapture(reuseRifeIo_ ? "RIFE IO: reusable CPU tensors (transfers still required)."
                               : "RIFE IO: allocating baseline.");
        std::wstring modulePath(MAX_PATH, L'\0');
        DWORD length = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
        if (length == 0) return false;
        modulePath.resize(length);
        const size_t separator = modulePath.find_last_of(L"\\/");
        const std::wstring modelDirectory = modulePath.substr(0, separator) + L"\\models\\";
        const std::wstring modelPath = modelDirectory + L"EDSR_x2.onnx";
        char legacy[8] = {};
        GetEnvironmentVariableA("AIFRAME_LEGACY_RIFE", legacy, sizeof(legacy));
        const std::wstring flowPath = modelDirectory + L"RIFE_flow_576x320.onnx";
        flowReconstruction_ = legacy[0] != '1' && GetFileAttributesW(flowPath.c_str()) != INVALID_FILE_ATTRIBUTES;
        // FP16 is an opt-in performance experiment. Synthetic parity checks
        // do not certify visual quality or live-game throughput. Validate the
        // model against the same contract the
        // FP32 path enforces so a stale or mismatched file cannot slip through.
        char fp16Flag[8] = {};
        GetEnvironmentVariableA("AIFRAME_RIFE_FP16", fp16Flag, sizeof(fp16Flag));
        const bool wantFp16 = fp16Flag[0] == '1';
        const std::wstring fp16Path = modelDirectory + L"RIFE_flow_576x320_fp16.onnx";
        const bool haveFp16 = wantFp16 &&
            flowReconstruction_ &&
            GetFileAttributesW(fp16Path.c_str()) != INVALID_FILE_ATTRIBUTES;
        const std::wstring rifePath = haveFp16 ? fp16Path
            : (flowReconstruction_ ? flowPath : modelDirectory + L"RIFE_trained_576x320.onnx");
        rifePrecision_ = haveFp16 ? 16 : 32;
        LogCapture(flowReconstruction_ ? "RIFE output: flow/mask for original-resolution GPU reconstruction."
                                      : "RIFE output: legacy low-resolution image.");
        if (wantFp16 && !haveFp16)
            LogCapture(flowReconstruction_
                ? "AIFRAME_RIFE_FP16 requested but RIFE_flow_576x320_fp16.onnx is missing; using FP32."
                : "AIFRAME_RIFE_FP16 ignored: flow model not in use.");

        LogCapture("DirectML adapter " + std::to_string(dmlDevice_) + ": " + DescribeAdapter(dmlDevice_) + ".");

        try {
            ortEnvironment_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "AIFrameEngineCompositor");
            ortOptions_ = std::make_unique<Ort::SessionOptions>();
            ortOptions_->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            ortOptions_->DisableMemPattern();
            ortOptions_->SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
            if (kEnableEdsrUpscale) {
            try {
                Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(*ortOptions_, dmlDevice_));
                edsrSession_ = std::make_unique<Ort::Session>(*ortEnvironment_, modelPath.c_str(), *ortOptions_);
                LogCapture("EDSR ONNX session loaded (DirectML).");
            } catch (const Ort::Exception& error) {
                LogCapture(std::string("EDSR DirectML failed, falling back to CPU: ") + error.what());
                ortOptions_ = std::make_unique<Ort::SessionOptions>();
                ortOptions_->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
                edsrSession_ = std::make_unique<Ort::Session>(*ortEnvironment_, modelPath.c_str(), *ortOptions_);
                LogCapture("EDSR ONNX session loaded (CPU).");
            }
            } else {
                LogCapture("EDSR disabled; GPU shader handles upscaling.");
            }
        } catch (const Ort::Exception& error) {
            LogCapture(std::string("EDSR session load failed: ") + error.what());
            return false;
        }

        // RIFE is optional: a failure here must never disable the EDSR path.
        if (GetFileAttributesW(rifePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            LogCapture("Trained RIFE ONNX model missing; frame generation unavailable.");
            return true;
        }
        try {
            char gpuIoFlag[8] = {};
            GetEnvironmentVariableA("AIFRAME_DML_GPU_IO", gpuIoFlag, sizeof(gpuIoFlag));
            if (std::strcmp(gpuIoFlag, "1") == 0) {
                try {
                    rifeGpuIo_ = std::make_unique<DmlGpuIo>(dmlDevice_);
                    Ort::SessionOptions options;
                    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
                    rifeGpuIo_->AppendProvider(options);
                    if (rifeProfileEnabled_) options.EnableProfiling(L"C:\\Temp\\aiframe_ort_gpu_io");
                    rifeSession_ = std::make_unique<Ort::Session>(*ortEnvironment_, rifePath.c_str(), options);
                    rifeGpuIo_->Configure(kRifeWidth, kRifeHeight, flowReconstruction_ ? 5 : 3);
                    rifeProvider_ = "DirectML";
                    reuseRifeIo_ = true;
                    LogCapture("RIFE IO: persistent D3D12 tensors, explicit upload/readback; not zero-copy.");
                } catch (const std::exception& error) {
                    LogCapture(std::string("GPU IO initialization failed; retrying existing provider: ") + error.what());
                    rifeSession_.reset();
                    rifeGpuIo_.reset();
                }
            }
            auto rifeOptions = std::make_unique<Ort::SessionOptions>();
            rifeOptions->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            rifeOptions->DisableMemPattern();
            rifeOptions->SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
            if (rifeProfileEnabled_) rifeOptions->EnableProfiling(L"C:\\Temp\\aiframe_ort");
            if (!rifeSession_) {
            try {
                Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(*rifeOptions, dmlDevice_));
                rifeSession_ = std::make_unique<Ort::Session>(*ortEnvironment_, rifePath.c_str(), *rifeOptions);
                rifeProvider_ = "DirectML";
                LogCapture("Trained RIFE ONNX session loaded (DirectML).");
            } catch (const Ort::Exception& error) {
                LogCapture(std::string("RIFE DirectML failed, falling back to CPU: ") + error.what());
                rifeOptions = std::make_unique<Ort::SessionOptions>();
                rifeOptions->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
                if (rifeProfileEnabled_) rifeOptions->EnableProfiling(L"C:\\Temp\\aiframe_ort_cpu");
                rifeSession_ = std::make_unique<Ort::Session>(*ortEnvironment_, rifePath.c_str(), *rifeOptions);
                rifeProvider_ = "CPU";
                LogCapture("Trained RIFE ONNX session loaded (CPU fallback).");
            }
            }
            const std::vector<int64_t> expected = {1, 3, kRifeHeight, kRifeWidth};
            if (rifeSession_->GetInputCount() != 2 || rifeSession_->GetOutputCount() != 1)
                throw std::runtime_error("RIFE must have two inputs and one output");
            for (size_t i = 0; i < 2; ++i) {
                auto type = rifeSession_->GetInputTypeInfo(i);
                auto info = type.GetTensorTypeAndShapeInfo();
                if (info.GetShape() != expected || info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
                    throw std::runtime_error("RIFE input contract differs from float32 [1,3,320,576]");
            }
            auto outputType = rifeSession_->GetOutputTypeInfo(0);
            auto outputInfo = outputType.GetTensorTypeAndShapeInfo();
            const std::vector<int64_t> expectedOutput = {1, flowReconstruction_ ? 5 : 3, kRifeHeight, kRifeWidth};
            if (outputInfo.GetShape() != expectedOutput || outputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
                throw std::runtime_error("RIFE output contract differs from selected image/flow mode");
            // Compile/warm up before F9 can enable FG; do not use a cold run to
            // permanently disable inference. Session access remains serialized.
            std::vector<float> zeros(static_cast<size_t>(kRifeWidth) * kRifeHeight * 3, 0.0f);
            auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            const char* names[] = {"img0", "img1"};
            const char* outputs[] = {flowReconstruction_ ? "flow_mask" : "output"};
            for (int run = 0; run < 3; ++run) {
                std::array<Ort::Value, 2> inputs = {
                    Ort::Value::CreateTensor<float>(memory, zeros.data(), zeros.size(), expected.data(), expected.size()),
                    Ort::Value::CreateTensor<float>(memory, zeros.data(), zeros.size(), expected.data(), expected.size())};
                if (rifeGpuIo_) rifeGpuIo_->Run(*rifeSession_, Ort::RunOptions{nullptr}, zeros, zeros, outputs[0]);
                else rifeSession_->Run(Ort::RunOptions{nullptr}, names, inputs.data(), 2, outputs, 1);
            }
            rifeAvailable_ = true;
            LogCapture(std::string("RIFE ready: 576x320 precision=") + (haveFp16 ? "fp16" : "fp32")
                + "; warmup complete; F9 enables buffered frame generation.");
        } catch (const std::exception& error) {
            LogCapture(std::string("RIFE session load failed; frame generation unavailable: ") + error.what());
            rifeSession_.reset();
            rifeGpuIo_.reset();
            // A bad FP16 file must never cost the user frame generation: retry
            // the known-good FP32 flow model before giving up entirely.
            if (haveFp16) {
                LogCapture("FP16 RIFE rejected; retrying the FP32 flow model.");
                try {
                    auto retry = std::make_unique<Ort::SessionOptions>();
                    retry->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
                    retry->DisableMemPattern();
                    retry->SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
                    if (rifeProfileEnabled_) retry->EnableProfiling(L"C:\\Temp\\aiframe_ort_fp32_retry");
                    Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(*retry, dmlDevice_));
                    rifeSession_ = std::make_unique<Ort::Session>(*ortEnvironment_, flowPath.c_str(), *retry);
                    const char* names[] = {"img0", "img1"};
                    const char* outputs[] = {"flow_mask"};
                    const std::vector<int64_t> inputShape = {1, 3, kRifeHeight, kRifeWidth};
                    std::vector<float> zeros(static_cast<size_t>(kRifeWidth) * kRifeHeight * 3, 0.0f);
                    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
                    for (int run = 0; run < 3; ++run) {
                        std::array<Ort::Value, 2> inputs = {
                            Ort::Value::CreateTensor<float>(memory, zeros.data(), zeros.size(), inputShape.data(), inputShape.size()),
                            Ort::Value::CreateTensor<float>(memory, zeros.data(), zeros.size(), inputShape.data(), inputShape.size())};
                        rifeSession_->Run(Ort::RunOptions{nullptr}, names, inputs.data(), 2, outputs, 1);
                    }
                    rifeAvailable_ = true;
                    rifePrecision_ = 32;
                    LogCapture("RIFE ready: 576x320 precision=fp32 (FP16 fallback); warmup complete.");
                } catch (const std::exception& retryError) {
                    LogCapture(std::string("FP32 RIFE fallback also failed: ") + retryError.what());
                    rifeSession_.reset();
                }
            }
        }
        return true;
    }

    bool CreatePipeline() {
        LogCapture("CreatePipeline: compiling shader");
        char fsrFlag[8] = {};
        GetEnvironmentVariableA("AIFRAME_FSR1", fsrFlag, sizeof(fsrFlag));
        if (std::strcmp(fsrFlag, "1") == 0) {
            wchar_t module[32768] = {};
            DWORD length = GetModuleFileNameW(nullptr, module, 32768);
            std::string error;
            if (length && length < 32768) {
                std::wstring path(module, length);
                path = path.substr(0, path.find_last_of(L"\\/")) + L"\\shaders\\fsr1.hlsl";
                fsrEnabled_ = fsrUpscaler_.Initialize(device_.Get(), path, error);
            } else error = "executable path unavailable";
            LogCapture(fsrEnabled_ ? "Upscaler: reference FSR1 EASU/RCAS enabled (SDR)."
                : "FSR initialization failed; using legacy scaler: " + error);
        }
        const char* source = R"(
            struct VSIn { float2 position : POSITION; float2 uv : TEXCOORD0; };
            struct VSOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
            VSOut VSMain(VSIn i) { VSOut o; o.position=float4(i.position,0,1); o.uv=i.uv; return o; }

            Texture2D frameTexture : register(t0);
            SamplerState frameSampler : register(s0);
            cbuffer ScalerConstants : register(b0) { float2 dstSize; float bicubicEnabled; float sharpness; };

            float3 SampleSource(float2 uv) { return frameTexture.Sample(frameSampler, uv).rgb; }

            // Real FSR 1.0 RCAS (MIT, AMD FidelityFX, ported from ffx_fsr1.h
            // via the Magpie FSR_RCAS reference). Robust contrast-adaptive
            // sharpening (peak lobe limited to 0.1875) that skips noise.
            float3 FsrRcasF(float3 b, float3 d, float3 e, float3 f, float3 h, float sharpness) {
                float bR = b.r; float bG = b.g; float bB = b.b;
                float dR = d.r; float dG = d.g; float dB = d.b;
                float eR = e.r; float eG = e.g; float eB = e.b;
                float fR = f.r; float fG = f.g; float fB = f.b;
                float hR = h.r; float hG = h.g; float hB = h.b;
                float bL = bB * 0.5 + (bR * 0.5 + bG);
                float dL = dB * 0.5 + (dR * 0.5 + dG);
                float eL = eB * 0.5 + (eR * 0.5 + eG);
                float fL = fB * 0.5 + (fR * 0.5 + fG);
                float hL = hB * 0.5 + (hR * 0.5 + hG);
                float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
                float mn = min(min(min(bL, dL), eL), fL);
                mn = min(mn, hL);
                float mx = max(max(max(bL, dL), eL), fL);
                mx = max(mx, hL);
                nz = saturate(abs(nz) * rcp(mx - mn + 1e-6));
                nz = -0.5 * nz + 1.0;
                float mn4R = min(min(min(bR, dR), fR), hR);
                float mn4G = min(min(min(bG, dG), fG), hG);
                float mn4B = min(min(min(bB, dB), fB), hB);
                float mx4R = max(max(max(bR, dR), fR), hR);
                float mx4G = max(max(max(bG, dG), fG), hG);
                float mx4B = max(max(max(bB, dB), fB), hB);
                float peakX = 1.0, peakY = -4.0;
                float hitMinR = min(mn4R, eR) * rcp(max(4.0 * mx4R, 1e-6));
                float hitMinG = min(mn4G, eG) * rcp(max(4.0 * mx4G, 1e-6));
                float hitMinB = min(mn4B, eB) * rcp(max(4.0 * mx4B, 1e-6));
                float hitMaxR = (peakX - max(mx4R, eR)) * rcp(4.0 * mn4R + peakY);
                float hitMaxG = (peakX - max(mx4G, eG)) * rcp(4.0 * mn4G + peakY);
                float hitMaxB = (peakX - max(mx4B, eB)) * rcp(4.0 * mn4B + peakY);
                float lobeR = max(-hitMinR, hitMaxR);
                float lobeG = max(-hitMinG, hitMaxG);
                float lobeB = max(-hitMinB, hitMaxB);
                float lobe = max(-(0.25 - (1.0 / 16.0)), min(max(max(lobeR, lobeG), lobeB), 0.0)) * sharpness;
                lobe *= nz;
                float rcpL = rcp(4.0 * lobe + 1.0);
                return float3(
                    (lobe * bR + lobe * dR + lobe * hR + lobe * fR + eR) * rcpL,
                    (lobe * bG + lobe * dG + lobe * hG + lobe * fG + eG) * rcpL,
                    (lobe * bB + lobe * dB + lobe * hB + lobe * fB + eB) * rcpL);
            }

            // Catmull-Rom bicubic: 9 taps. Not FSR EASU or NVIDIA NIS;
            // cost depends on output resolution and sharpening sample count.
            float3 SampleCatmullRom(float2 uv, float2 texSize) {
                float2 sp = uv * texSize;
                float2 base = floor(sp - 0.5) + 0.5;
                float2 f = sp - base;
                float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
                float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
                float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
                float2 w3 = f * f * (-0.5 + 0.5 * f);
                float2 w12 = w1 + w2;
                float2 o12 = w2 / max(w12, float2(1e-5, 1e-5));
                float2 uv0 = (base - 1.0) / texSize;
                float2 uv12 = (base + o12) / texSize;
                float2 uv3 = (base + 2.0) / texSize;
                float3 c = float3(0, 0, 0);
                c += SampleSource(float2(uv0.x, uv0.y)) * (w0.x * w0.y);
                c += SampleSource(float2(uv12.x, uv0.y)) * (w12.x * w0.y);
                c += SampleSource(float2(uv3.x, uv0.y)) * (w3.x * w0.y);
                c += SampleSource(float2(uv0.x, uv12.y)) * (w0.x * w12.y);
                c += SampleSource(float2(uv12.x, uv12.y)) * (w12.x * w12.y);
                c += SampleSource(float2(uv3.x, uv12.y)) * (w3.x * w12.y);
                c += SampleSource(float2(uv0.x, uv3.y)) * (w0.x * w3.y);
                c += SampleSource(float2(uv12.x, uv3.y)) * (w12.x * w3.y);
                c += SampleSource(float2(uv3.x, uv3.y)) * (w3.x * w3.y);
                float wsum = (w0.x + w12.x + w3.x) * (w0.y + w12.y + w3.y);
                return c / max(wsum, 1e-5);
            }

            float4 PSMain(VSOut i) : SV_TARGET {
                uint texW = 0, texH = 0;
                frameTexture.GetDimensions(texW, texH);
                float2 texSize = float2(texW, texH);
                // F10 picks the spatial filter, F11 independently toggles RCAS.
                // Both only apply when genuinely upscaling; bicubic on a
                // downscale would alias.
                bool isUpscale = (dstSize.x >= texSize.x) && (dstSize.y >= texSize.y);
                bool useBicubic = (bicubicEnabled > 0.5) && isUpscale;
                float4 center = frameTexture.Sample(frameSampler, i.uv);
                float3 rgb = useBicubic ? SampleCatmullRom(i.uv, texSize) : center.rgb;
                if (sharpness > 0.001 && isUpscale) {
                    // FSR RCAS must read the NEIGHBOURS OF THE OUTPUT IMAGE,
                    // one output pixel away - not source texels. Sampling the
                    // source grid gives neighbours 1.5 output pixels wide at
                    // 1.5x, which corrupts the ring and cancels the sharpening.
                    float2 outPixel = 1.0 / dstSize;
                    float2 buv = i.uv - float2(0, outPixel.y), duv = i.uv - float2(outPixel.x, 0);
                    float2 fuv = i.uv + float2(outPixel.x, 0), huv = i.uv + float2(0, outPixel.y);
                    float3 bx = useBicubic ? SampleCatmullRom(buv, texSize) : SampleSource(buv);
                    float3 dx = useBicubic ? SampleCatmullRom(duv, texSize) : SampleSource(duv);
                    float3 fx = useBicubic ? SampleCatmullRom(fuv, texSize) : SampleSource(fuv);
                    float3 hx = useBicubic ? SampleCatmullRom(huv, texSize) : SampleSource(huv);
                    rgb = FsrRcasF(bx, dx, rgb, fx, hx, sharpness);
                }
                return float4(rgb, center.a);
            }
        )";
        ComPtr<ID3DBlob> vs, ps, errors;
        LogCapture("CreatePipeline: compiling VSMain");
        HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vs, &errors);
        if (FAILED(hr)) {
            std::string errMsg = errors ? static_cast<char*>(errors->GetBufferPointer()) : "unknown";
            LogCapture("CreatePipeline: VSMain compile failed: " + errMsg);
            return false;
        }
        LogCapture("CreatePipeline: compiling PSMain");
        hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &ps, &errors);
        if (FAILED(hr)) {
            std::string errMsg = errors ? static_cast<char*>(errors->GetBufferPointer()) : "unknown";
            LogCapture("CreatePipeline: PSMain compile failed: " + errMsg);
            return false;
        }
        LogCapture("CreatePipeline: shaders compiled successfully");
        if (FAILED(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertexShader_)) ||
            FAILED(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixelShader_))) return false;

        D3D11_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 }
        };
        if (FAILED(device_->CreateInputLayout(layout, 2, vs->GetBufferPointer(), vs->GetBufferSize(), &inputLayout_))) return false;
        Vertex vertices[] = { {{-1,1},{0,0}}, {{1,1},{1,0}}, {{-1,-1},{0,1}}, {{1,-1},{1,1}} };
        D3D11_BUFFER_DESC bd = { sizeof(vertices), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
        D3D11_SUBRESOURCE_DATA data = { vertices };
        if (FAILED(device_->CreateBuffer(&bd, &data, &vertexBuffer_))) return false;
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device_->CreateSamplerState(&sd, &sampler_))) return false;

        ScalerConstants constants = {};
        constants.dstSize[0] = static_cast<float>(monitorRect_.right - monitorRect_.left);
        constants.dstSize[1] = static_cast<float>(monitorRect_.bottom - monitorRect_.top);
        constants.bicubicEnabled = scalerBicubic_ ? 1.0f : 0.0f;
        constants.sharpness = scalerSharpness_;
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = sizeof(ScalerConstants);
        cbDesc.Usage = D3D11_USAGE_DEFAULT;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA cbData = { &constants };
        return SUCCEEDED(device_->CreateBuffer(&cbDesc, &cbData, &scalerConstants_));
    }

    void DrawLegacyComparison(ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target,
        UINT width, UINT height, bool bicubic, float gain) {
        ScalerConstants constants = {};
        constants.dstSize[0] = float(width); constants.dstSize[1] = float(height);
        constants.bicubicEnabled = bicubic ? 1.f : 0.f; constants.sharpness = gain;
        context_->UpdateSubresource(scalerConstants_.Get(), 0, nullptr, &constants, 0, 0);
        context_->OMSetRenderTargets(1, &target, nullptr);
        D3D11_VIEWPORT vp = {0, 0, float(width), float(height), 0, 1};
        context_->RSSetViewports(1, &vp); context_->RSSetState(nullptr);
        context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context_->OMSetDepthStencilState(nullptr, 0);
        UINT stride = sizeof(Vertex), offset = 0;
        context_->IASetInputLayout(inputLayout_.Get());
        context_->IASetVertexBuffers(0, 1, vertexBuffer_.GetAddressOf(), &stride, &offset);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->GSSetShader(nullptr, nullptr, 0);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        context_->PSSetConstantBuffers(0, 1, scalerConstants_.GetAddressOf());
        context_->PSSetShaderResources(0, 1, &source);
        context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        context_->Draw(4, 0);
        ID3D11ShaderResourceView* empty = nullptr;
        context_->PSSetShaderResources(0, 1, &empty);
        context_->OMSetRenderTargets(0, nullptr, nullptr);
    }

    void ExportUpscaleComparison() {
        if (frameGenerationEnabled_) {
            LogCapture("UPSCALE EXPORT: turn F9 off first; blocking export is not a performance test.");
            return;
        }
        if (!frameB_.view) { LogCapture("UPSCALE EXPORT: no captured client frame available."); return; }
        try {
            wchar_t exe[32768] = {};
            const DWORD length = GetModuleFileNameW(nullptr, exe, 32768);
            if (!length || length >= 32768) throw std::runtime_error("executable path unavailable");
            const auto base = std::filesystem::path(exe).parent_path();
            const auto root = base / "comparisons";
            std::filesystem::create_directories(root);
            const auto output = root / ("frame-" + std::to_string(GetCurrentTime100ns()));
            std::string error;
            LogCapture("UPSCALE EXPORT: beginning blocking same-frame diagnostic.");
            const bool ok = UpscaleComparison::Export(device_.Get(), context_.Get(), frameB_.view.Get(),
                monitorRect_.right - monitorRect_.left, monitorRect_.bottom - monitorRect_.top,
                scalerSharpness_, pairId_, frameB_.captureTime, base / "shaders" / "fsr1.hlsl", output,
                [this](auto* source, auto* target, UINT w, UINT h, bool cubic, float gain) {
                    DrawLegacyComparison(source, target, w, h, cubic, gain);
                }, error);
            LogCapture(ok ? "UPSCALE EXPORT: saved " + (output / "compare.html").string()
                : "UPSCALE EXPORT failed: " + error);
        } catch (const std::exception& e) { LogCapture(std::string("UPSCALE EXPORT failed: ") + e.what()); }
        UpdateScalerConstants();
        displayCache_.Invalidate();
    }

    DWORD FindWitcherProcess() {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return 0;
        PROCESSENTRY32W entry = { sizeof(entry) };
        DWORD result = 0;
        if (Process32FirstW(snapshot, &entry)) do {
            if (_wcsicmp(entry.szExeFile, L"witcher3.exe") == 0) { result = entry.th32ProcessID; break; }
        } while (Process32NextW(snapshot, &entry));
        CloseHandle(snapshot);
        return result;
    }

    static BOOL CALLBACK FindWindowCallback(HWND window, LPARAM parameter) {
        auto state = reinterpret_cast<std::pair<DWORD, HWND*>*>(parameter);
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId == state->first && IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
            *state->second = window;
            return FALSE;
        }
        return TRUE;
    }

    HWND FindWitcherWindow() {
        DWORD processId = FindWitcherProcess();
        if (!processId) return nullptr;
        HWND result = nullptr;
        std::pair<DWORD, HWND*> state = { processId, &result };
        EnumWindows(FindWindowCallback, reinterpret_cast<LPARAM>(&state));
        return result;
    }

    void EnsureCapture() {
        if (targetWindow_ && captureSession_) return;

        if (!loggedCaptureLookup_) {
            LogCapture(captureTarget_.Bound() ? "Opening explicitly selected capture target." : "Beginning default Witcher process lookup.");
            loggedCaptureLookup_ = true;
        }
        HWND target = captureTarget_.Bound() ? captureTarget_.Window() : FindWitcherWindow();
        if (!target) {
            if (!loggedMissingWindow_) {
                LogCapture("Witcher window not found.");
                loggedMissingWindow_ = true;
            }
            return;
        }
        loggedMissingWindow_ = false;
        if (!captureTarget_.Bound()) {
            if (!captureTarget_.Bind(target)) { LogCapture("Cannot pin default target identity."); running_ = false; return; }
            targetLabel_ = to_string(captureTarget_.Title());
        }
        if (!captureTarget_.Valid()) { running_ = false; return; }
        LogCapture("Target window resolved: " + targetLabel_);
        if (captureSession_) {
            LogCapture("Closing previous capture session.");
            captureSession_.Close();
        }
        if (framePool_) {
            LogCapture("Closing previous frame pool.");
            framePool_.Close();
        }
        captureItem_ = nullptr;
        targetWindow_ = target;
        if (!targetWindow_) return;
        LogCapture("Target window found; creating GraphicsCaptureItem.");

        LogCapture("Before RoGetActivationFactory.");
        winrt::hstring runtimeClassName = L"Windows.Graphics.Capture.GraphicsCaptureItem";
        winrt::com_ptr<IGraphicsCaptureItemInterop> factory;
        check_hresult(RoGetActivationFactory(
            reinterpret_cast<HSTRING>(get_abi(runtimeClassName)),
            __uuidof(IGraphicsCaptureItemInterop),
            factory.put_void()));
        LogCapture("GraphicsCaptureItem interop factory acquired.");
        GraphicsCaptureItem item{ nullptr };
        check_hresult(factory->CreateForWindow(targetWindow_, guid_of<GraphicsCaptureItem>(), put_abi(item)));
        LogCapture("GraphicsCaptureItem created.");
        captureItem_ = item;

        ComPtr<IDXGIDevice> dxgiDevice;
        device_.As(&dxgiDevice);
        winrt::com_ptr<IInspectable> inspectable;
        check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()));
        LogCapture("Direct3D11 WinRT device created.");
        direct3dDevice_ = inspectable.as<IDirect3DDevice>();
        auto size = captureItem_.Size();
        framePool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
            direct3dDevice_, DirectXPixelFormat::B8G8R8A8UIntNormalized, 4, size);
        captureSession_ = framePool_.CreateCaptureSession(captureItem_);
        LogCapture("Capture session object created.");
        captureSession_.IsCursorCaptureEnabled(false);
        captureSession_.StartCapture();
        LogCapture("Windows Graphics Capture session started.");
        ShowWindow(targetWindow_, SW_RESTORE);
        ActivateTargetWindow();
        if (!systemCursorHidden_) {
            while (ShowCursor(FALSE) >= 0) {}
            systemCursorHidden_ = true;
        }
        LogCapture("Selected target returned to foreground.");
    }

    void ActivateTargetWindow() {
        if (!targetWindow_ || !captureTarget_.Valid()) return;
        DWORD targetThread = GetWindowThreadProcessId(targetWindow_, nullptr);
        DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        if (targetThread != foregroundThread) AttachThreadInput(foregroundThread, targetThread, TRUE);
        SetForegroundWindow(targetWindow_);
        SetFocus(targetWindow_);
        if (targetThread != foregroundThread) AttachThreadInput(foregroundThread, targetThread, FALSE);
    }

    // Windows Graphics Capture returns the entire window including the title
    // bar and resize borders (e.g. 1282x759 for a 1280x720 game). Left alone
    // that skews the aspect ratio (1.689 vs 16:9) and stretches chrome across
    // the display. Crop every capture down to the game's client area.
    bool CropToClientArea(const ComPtr<ID3D11Texture2D>& source, ComPtr<ID3D11Texture2D>& cropped) {
        if (!targetWindow_ || !IsWindow(targetWindow_)) { LogOnce("CROP: no target window"); return false; }
        RECT windowRect = {};
        RECT clientRect = {};
        if (!GetWindowRect(targetWindow_, &windowRect)) { LogOnce("CROP: GetWindowRect failed"); return false; }
        if (!GetClientRect(targetWindow_, &clientRect)) { LogOnce("CROP: GetClientRect failed"); return false; }
        POINT origin = { clientRect.left, clientRect.top };
        if (!ClientToScreen(targetWindow_, &origin)) { LogOnce("CROP: ClientToScreen failed"); return false; }
        D3D11_TEXTURE2D_DESC desc = {};
        source->GetDesc(&desc);
        RECT clientScreen = {origin.x, origin.y,
            origin.x + clientRect.right - clientRect.left,
            origin.y + clientRect.bottom - clientRect.top};
        RECT visible = {}, crop = {};
        const bool haveVisible = SUCCEEDED(DwmGetWindowAttribute(targetWindow_,
            DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible)));
        const bool resolved = (haveVisible && ResolveClientCrop(visible, clientScreen, desc.Width, desc.Height, crop)) ||
            ResolveClientCrop(windowRect, clientScreen, desc.Width, desc.Height, crop) ||
            ResolveClientCrop(clientScreen, clientScreen, desc.Width, desc.Height, crop);
        if (!resolved) {
            LogOnce("CROP: unknown capture geometry; preserving texture instead of clipping client pixels.");
            return false;
        }
        const int relX = crop.left, relY = crop.top;
        const UINT cropW = crop.right - crop.left, cropH = crop.bottom - crop.top;
        LogOnce("CROP: window=" + std::to_string(windowRect.right - windowRect.left) + "x" + std::to_string(windowRect.bottom - windowRect.top) +
                " client=" + std::to_string(cropW) + "x" + std::to_string(cropH) +
                " rel=(" + std::to_string(relX) + "," + std::to_string(relY) + ")" +
                " texture=" + std::to_string(desc.Width) + "x" + std::to_string(desc.Height) +
                " right=" + std::to_string(static_cast<UINT>(relX) + cropW) + " bottom=" + std::to_string(static_cast<UINT>(relY) + cropH));

        if (cropW < 2 || cropH < 2) return false;
        if (relX == 0 && relY == 0 && cropW == desc.Width && cropH == desc.Height) { LogOnce("CROP: nothing to crop"); return false; }

        D3D11_TEXTURE2D_DESC dstDesc = desc;
        dstDesc.Width = cropW;
        dstDesc.Height = cropH;
        dstDesc.Usage = D3D11_USAGE_DEFAULT;
        dstDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        dstDesc.CPUAccessFlags = 0;
        dstDesc.MiscFlags = 0;
        if (FAILED(device_->CreateTexture2D(&dstDesc, nullptr, &cropped))) return false;
        const D3D11_BOX box = {
            static_cast<UINT>(relX), static_cast<UINT>(relY), 0,
            static_cast<UINT>(relX) + cropW, static_cast<UINT>(relY) + cropH, 1
        };
        context_->CopySubresourceRegion(cropped.Get(), 0, 0, 0, 0, source.Get(), 0, &box);
        return true;
    }

    void CaptureLatestFrame() {
        if (!framePool_) return;
        auto frame = framePool_.TryGetNextFrame();
        if (!frame) return;
        while (auto newer = framePool_.TryGetNextFrame()) frame = newer;
        const int64_t captureTime = frame.SystemRelativeTime().count();
        captureClock_.Observe(captureTime, GetCurrentTime100ns());
        if (captureTime <= frameB_.captureTime) return;
        auto access = frame.Surface().as<IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(access->GetInterface(IID_PPV_ARGS(&texture)))) return;

        // Drop the window chrome so the display, EDSR and RIFE all see only
        // the game's client area (exactly 16:9 for a 1280x720 window).
        ComPtr<ID3D11Texture2D> source = texture;
        ComPtr<ID3D11Texture2D> cropped;
        ComPtr<ID3D11ShaderResourceView> view;
        if (CropToClientArea(texture, cropped)) {
            if (SUCCEEDED(device_->CreateShaderResourceView(cropped.Get(), nullptr, &view))) {
                source = cropped;
            } else {
                LogOnce("CROP: SRV creation failed for cropped texture, falling back to original");
                view.Reset();
            }
        }
        if (!view) {
            // Own the image even when cropping is unavailable: WGC pool textures
            // can be reused after their capture frame is released.
            D3D11_TEXTURE2D_DESC desc = {};
            source->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = 0;
            desc.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> owned;
            if (FAILED(device_->CreateTexture2D(&desc, nullptr, &owned))) return;
            context_->CopyResource(owned.Get(), source.Get());
            source = owned;
            if (FAILED(device_->CreateShaderResourceView(source.Get(), nullptr, &view))) return;
        }

        frameA_ = std::move(frameB_);
        frameB_.captureFrame = frame;
        frameB_.texture = source;
        frameB_.view = view;
        frameB_.captureTime = captureTime;
        hasFrame_ = true;
        ++capturedFrameCount_;
        if (capturedFrameCount_ % 120u == 0u)
            LogCapture("Captured frame heartbeat: " + std::to_string(capturedFrameCount_) + ".");
        if (!loggedFirstFrame_) {
            LogCapture("First captured target frame received.");
            loggedFirstFrame_ = true;
        }
        if (frameA_.texture && frameB_.texture) {
            ++pairId_;
            const int64_t frameDelta = frameB_.captureTime - frameA_.captureTime;
            captureInterval100ns_ = frameDelta;
            if (frameGenerationEnabled_ && modelReady_ && rifeAvailable_) {
                if (frameDelta > 1000000) {
                    timeline_.Clear(); // pause/resize discontinuity
                    latestRequest_.Clear();
                }
                else {
                    // Retain EVERY pair even if its inference is skipped. The
                    // presentation clock must not jump forward/back on jitter.
                    timeline_.Add(pairId_, frameA_.captureTime, frameB_.captureTime, frameA_.view, frameB_.view);
                    if (latestRequest_.Offer(pairId_)) ++supersededRequests_;
                }
            }
        }

        if (kEnableEdsrUpscale && modelReady_ && !frameGenerationEnabled_ && ++processingCounter_ % 4u == 0u) {
            QueueLatestFrameForInference();
        }

    }
    void QueueLatestFrameForInference() {
        if (!frameB_.texture) return;
        const bool useRife = frameGenerationEnabled_ && rifeAvailable_.load();
        if (!useRife && !kEnableEdsrUpscale) return;
        if (useRife && !latestRequest_.Pending(pairId_)) return;
        admission_.SetPresentationDelay(timeline_.EffectiveDelayAt(GetCurrentTime100ns()));
        if (useRife && GetCurrentTime100ns() >= frameB_.captureTime + admission_.GetBudget()) {
            latestRequest_.Clear();
            ++expiredRequests_;
            return;
        }
        if (useRife && !admission_.ShouldSubmit(GetCurrentTime100ns(), frameB_.captureTime)) {
            if (latestRequest_.MarkDenied()) ++autoSkippedRifeFrames_;
            return;
        }
        {
            std::scoped_lock lock(queueMutex_, resultMutex_);
            if (!CanSubmitInference(inferenceBusy_, resultReady_)) {
                if (useRife && latestRequest_.MarkBusy()) ++droppedRifeFrames_;
                return; // No blocking GPU readback for jobs we cannot accept.
            }
        }
        const int64_t readbackStart = GetCurrentTime100ns();
        D3D11_TEXTURE2D_DESC sourceDesc = {};
        frameB_.texture->GetDesc(&sourceDesc);
        if (sourceDesc.Width < 2 || sourceDesc.Height < 2) return;
        if (useRife) {
            if (!frameA_.texture) return;
            D3D11_TEXTURE2D_DESC previousDesc = {};
            frameA_.texture->GetDesc(&previousDesc);
            if (previousDesc.Width != sourceDesc.Width || previousDesc.Height != sourceDesc.Height) {
                timeline_.Clear();
                latestRequest_.Clear();
                return;
            }
        }

        UINT processingWidth = sourceDesc.Width;
        UINT processingHeight = sourceDesc.Height;
        std::vector<uint8_t> frame, previous;
        if (useRife && gpuPreprocessing_) {
            processingWidth = kRifeWidth;
            processingHeight = kRifeHeight;
            if (!rifePreprocessor_.Read(context_.Get(), frameA_.view.Get(), frameA_.captureTime, previous) ||
                !rifePreprocessor_.Read(context_.Get(), frameB_.view.Get(), frameB_.captureTime, frame)) {
                LogCapture("RIFE compact readback failed; reverting to CPU preprocessing.");
                gpuPreprocessing_ = false;
                rifePreprocessor_.ResetCache();
                return;
            }
        } else {
        if (!stagingTexture_ || stagingWidth_ != sourceDesc.Width || stagingHeight_ != sourceDesc.Height) {
            stagingTexture_.Reset();
            D3D11_TEXTURE2D_DESC stagingDesc = sourceDesc;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.BindFlags = 0;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            stagingDesc.MiscFlags = 0;
            if (FAILED(device_->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture_))) return;
            stagingWidth_ = sourceDesc.Width;
            stagingHeight_ = sourceDesc.Height;
        }

        auto readback = [&](ID3D11Texture2D* texture, std::vector<uint8_t>& pixels) {
            context_->CopyResource(stagingTexture_.Get(), texture);
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (FAILED(context_->Map(stagingTexture_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
            pixels.resize(static_cast<size_t>(processingWidth) * processingHeight * 4ULL);
            const auto* source = static_cast<const uint8_t*>(mapped.pData);
            for (UINT y = 0; y < processingHeight; ++y)
                memcpy(pixels.data() + static_cast<size_t>(y) * processingWidth * 4ULL,
                    source + static_cast<size_t>(y) * mapped.RowPitch, processingWidth * 4ULL);
            context_->Unmap(stagingTexture_.Get(), 0);
            return true;
        };
        if (useRife && !readback(frameA_.texture.Get(), previous)) return;
        if (!readback(frameB_.texture.Get(), frame)) return;
        }
        readbackTime100ns_ = GetCurrentTime100ns() - readbackStart;
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (!inferenceBusy_) {
            pendingPreviousFrame_ = std::move(previous);
            pendingFrame_ = std::move(frame);
            pendingWidth_ = processingWidth;
            pendingHeight_ = processingHeight;
            pendingUseRife_ = useRife;
            pendingPairId_ = pairId_;
            pendingQueueTime_ = GetCurrentTime100ns();
            pendingServiceStart_ = readbackStart;
            pendingCaptureTime_ = frameB_.captureTime;
            if (useRife) {
                admission_.Submitted(pendingQueueTime_);
                if (latestRequest_.Retried()) ++retriedSubmissions_;
                latestRequest_.Clear();
            }
            inferenceBusy_ = true;
            if (frameGenerationEnabled_) totalRifeFrames_++;
            queueCv_.notify_one();
        } else if (frameGenerationEnabled_) {
            droppedRifeFrames_++;
        }
    }
    void ConsumeProcessedFrame() {
        std::vector<uint8_t> pixels;
        std::vector<float> fields;
        UINT width = 0;
        UINT height = 0;
        int64_t serviceTime = 0;
        int64_t completedAt = 0;
        int64_t captureTime = 0;
        {
            std::lock_guard<std::mutex> lock(resultMutex_);
            if (!resultReady_) return;
            pixels = std::move(processedPixels_);
            fields = std::move(processedFields_);
            width = processedWidth_;
            height = processedHeight_;
            interpolatedFrameReady_ = resultIsRife_;
            interpolatedPairId_ = resultPairId_;
            serviceTime = resultServiceTime_;
            completedAt = resultCompletedAt_;
            captureTime = resultCaptureTime_;
            resultReady_ = false;
        }
        processedView_.Reset();
        resultWait100ns_ = GetCurrentTime100ns() - completedAt;
        reconstructionSubmit100ns_ = 0;
        if (!fields.empty()) {
            const int64_t start = GetCurrentTime100ns();
            ComPtr<ID3D11ShaderResourceView> a, b;
            if (frameGenerationEnabled_ && timeline_.Sources(interpolatedPairId_, start, a, b)) {
                if (!reconstructorReady_) {
                    std::string error;
                    reconstructorReady_ = reconstructor_.Initialize(device_.Get(), error);
                    if (!reconstructorReady_) LogCapture("RIFE reconstruction initialization failed: " + error);
                }
                if (reconstructorReady_ && !reconstructor_.Reconstruct(device_.Get(), context_.Get(),
                    a.Get(), b.Get(), fields, kRifeWidth, kRifeHeight, processedView_))
                    LogCapture("RIFE reconstruction failed; retaining real frames.");
                if (processedView_ && !loggedReconstruction_) {
                    ComPtr<ID3D11Resource> resource;
                    ComPtr<ID3D11Texture2D> texture;
                    processedView_->GetResource(&resource);
                    if (SUCCEEDED(resource.As(&texture))) {
                        D3D11_TEXTURE2D_DESC desc = {}; texture->GetDesc(&desc);
                        LogCapture("RIFE reconstructed midpoint: " + std::to_string(desc.Width) + "x" +
                            std::to_string(desc.Height) + " from original A/B; flow grid 576x320.");
                        loggedReconstruction_ = true;
                    }
                }
            }
            reconstructionSubmit100ns_ = GetCurrentTime100ns() - start;
            serviceTime += reconstructionSubmit100ns_;
        } else if (!pixels.empty()) UploadProcessedFrame(pixels, width, height);
        if (interpolatedFrameReady_) {
            ++completedRifeFrames_;
            if (frameGenerationEnabled_ && interpolatedPairId_ >= firstEnabledPairId_) {
                const bool wasCooling = admission_.CoolingDown();
                admission_.SetPresentationDelay(timeline_.EffectiveDelayAt(GetCurrentTime100ns()));
                admission_.Observe(serviceTime);
                // Presentation delay follows measured capture-to-completion
                // time (submit age + readback + queue + inference + recon).
                // The attach deadline runs from capture, not from submission,
                // so service time alone under-counts the required delay.
                if (captureTime > 0)
                    adaptiveLatency_.Observe(GetCurrentTime100ns() - captureTime);
                timeline_.SetDelay(adaptiveLatency_.GetDelay());
                if (wasCooling != admission_.CoolingDown())
                    LogCapture(admission_.CoolingDown()
                        ? "FG admission: sustained deadline overruns; probing every 2 seconds, buffered presentation retained."
                        : "FG admission: probe recovered; normal submission resumed, buffered presentation retained.");
            }
            if (!frameGenerationEnabled_ || !processedView_ ||
                !timeline_.Attach(interpolatedPairId_, processedView_, GetCurrentTime100ns()))
                ++staleRifeFrames_;
        }
    }

    void InferenceLoop() {
        for (;;) {
            std::vector<uint8_t> frame;
            std::vector<uint8_t> previousFrame;
            UINT width = 0;
            UINT height = 0;
            bool useRife = false;
            int64_t qTime = 0;
            int64_t pairId = 0;
            int64_t serviceStart = 0;
            int64_t captureTime = 0;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueCv_.wait(lock, [this]() { return stopInference_ || inferenceBusy_; });
                if (stopInference_) return;
                frame = std::move(pendingFrame_);
                previousFrame = std::move(pendingPreviousFrame_);
                width = pendingWidth_;
                height = pendingHeight_;
                useRife = pendingUseRife_;
                qTime = pendingQueueTime_;
                pairId = pendingPairId_;
                serviceStart = pendingServiceStart_;
                captureTime = pendingCaptureTime_;
            }

            int64_t startInference = GetCurrentTime100ns();
            int64_t latency = startInference - qTime;

            std::vector<uint8_t> result;
            std::vector<float> fields;
            UINT resultWidth = 0;
            UINT resultHeight = 0;
            if (useRife) {
                RunRifeInference(previousFrame, frame, width, height, result, resultWidth, resultHeight, fields);
            } else if (kEnableEdsrUpscale) {
                RunEdsrInference(frame, width, height, result, resultWidth, resultHeight);
            }
            int64_t endInference = GetCurrentTime100ns();

            if (!result.empty() || !fields.empty()) {
                std::lock_guard<std::mutex> resultLock(resultMutex_);
                processedPixels_ = std::move(result);
                processedFields_ = std::move(fields);
                processedWidth_ = resultWidth;
                processedHeight_ = resultHeight;
                resultIsRife_ = useRife;
                resultPairId_ = pairId;
                resultServiceTime_ = endInference - serviceStart;
                resultCompletedAt_ = endInference;
                resultCaptureTime_ = captureTime;
                inferenceTime100ns_ = endInference - startInference;
                queueLatency100ns_ = latency;
                resultReady_ = true;
            }
            {
                std::lock_guard<std::mutex> queueLock(queueMutex_);
                inferenceBusy_ = false;
            }
            SetEvent(completionEvent_.get()); // Publish result AND idle state before waking owner.
        }
    }
    void RunEdsrInference(const std::vector<uint8_t>& frame, UINT width, UINT height, std::vector<uint8_t>& result, UINT& resultWidth, UINT& resultHeight) {
        const UINT inputWidth = width / 2u;
        const UINT inputHeight = height / 2u;
        const size_t planeSize = static_cast<size_t>(inputWidth) * inputHeight;
        std::vector<float> input(planeSize * 3ULL);
        for (UINT y = 0; y < inputHeight; ++y) {
            for (UINT x = 0; x < inputWidth; ++x) {
                float channels[3] = {};
                for (UINT sy = 0; sy < 2; ++sy) for (UINT sx = 0; sx < 2; ++sx) {
                    const size_t index = (static_cast<size_t>(y * 2u + sy) * width + x * 2u + sx) * 4ULL;
                    channels[0] += frame[index + 2] / 255.0f;
                    channels[1] += frame[index + 1] / 255.0f;
                    channels[2] += frame[index + 0] / 255.0f;
                }
                const size_t pixel = static_cast<size_t>(y) * inputWidth + x;
                input[pixel] = channels[0] / 4.0f;
                input[planeSize + pixel] = channels[1] / 4.0f;
                input[planeSize * 2ULL + pixel] = channels[2] / 4.0f;
            }
        }
        try {
            Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            std::array<int64_t, 4> shape = { 1, 3, static_cast<int64_t>(inputHeight), static_cast<int64_t>(inputWidth) };
            auto tensor = Ort::Value::CreateTensor<float>(memoryInfo, input.data(), input.size(), shape.data(), shape.size());
            const char* inputNames[] = { "input" };
            const char* outputNames[] = { "output" };
            auto output = edsrSession_->Run(Ort::RunOptions{ nullptr }, inputNames, &tensor, 1, outputNames, 1);
            auto outputShape = output[0].GetTensorTypeAndShapeInfo().GetShape();
            resultWidth = static_cast<UINT>(outputShape[3]);
            resultHeight = static_cast<UINT>(outputShape[2]);
            if (resultWidth != width || resultHeight != height) {
                LogCapture("EDSR output dimensions: " + std::to_string(resultWidth) + "x" + std::to_string(resultHeight) + "; input frame: " + std::to_string(width) + "x" + std::to_string(height) + ".");
                result.clear();
                return;
            }
            LogCapture("EDSR frame completed at " + std::to_string(width) + "x" + std::to_string(height) + ".");
            const size_t outputPlane = static_cast<size_t>(resultWidth) * resultHeight;
            const float* values = output[0].GetTensorData<float>();
            result.resize(outputPlane * 4ULL);
            for (size_t pixel = 0; pixel < outputPlane; ++pixel) {
                result[pixel * 4ULL + 0] = static_cast<uint8_t>(std::clamp(values[outputPlane * 2ULL + pixel], 0.0f, 1.0f) * 255.0f);
                result[pixel * 4ULL + 1] = static_cast<uint8_t>(std::clamp(values[outputPlane + pixel], 0.0f, 1.0f) * 255.0f);
                result[pixel * 4ULL + 2] = static_cast<uint8_t>(std::clamp(values[pixel], 0.0f, 1.0f) * 255.0f);
                result[pixel * 4ULL + 3] = 255;
            }
        } catch (const Ort::Exception& error) {
            LogCapture(std::string("EDSR inference error: ") + error.what());
            result.clear();
        }
    }

    void RunRifeInference(const std::vector<uint8_t>& previousFrame, const std::vector<uint8_t>& currentFrame, UINT width, UINT height, std::vector<uint8_t>& result, UINT& resultWidth, UINT& resultHeight, std::vector<float>& fields) {
        if (!rifeSession_) {
            LogCapture("RIFE inference skipped: session unavailable.");
            return;
        }
        if (!width || !height || previousFrame.size() != currentFrame.size() ||
            currentFrame.size() != static_cast<size_t>(width) * height * 4ULL) {
            LogCapture("RIFE inference skipped: frame sizes differ.");
            return;
        }

        // RIFE is exported at a fixed size, so resample the captured BGRA
        // frames into that static tensor shape (bilinear).
        const int64_t resampleStart = GetCurrentTime100ns();
        const UINT modelWidth = kRifeWidth;
        const UINT modelHeight = kRifeHeight;
        const size_t modelPlaneSize = static_cast<size_t>(modelWidth) * modelHeight;
        if (!rifeIo_) rifeIo_ = std::make_unique<RifeIoBuffers>(modelWidth, modelHeight, flowReconstruction_ ? 5 : 3);
        std::vector<float> allocatedFirst, allocatedSecond;
        if (!reuseRifeIo_) {
            allocatedFirst.resize(modelPlaneSize * 3ULL);
            allocatedSecond.resize(modelPlaneSize * 3ULL);
        }
        auto& first = reuseRifeIo_ ? rifeIo_->First() : allocatedFirst;
        auto& second = reuseRifeIo_ ? rifeIo_->Second() : allocatedSecond;
        if (width == modelWidth && height == modelHeight) {
            RifePreprocessor::PackRgb(previousFrame, first);
            RifePreprocessor::PackRgb(currentFrame, second);
        } else {
        const float scaleX = static_cast<float>(width) / static_cast<float>(modelWidth);
        const float scaleY = static_cast<float>(height) / static_cast<float>(modelHeight);
        for (UINT y = 0; y < modelHeight; ++y) {
            const float sourceYf = std::clamp((static_cast<float>(y) + 0.5f) * scaleY - 0.5f, 0.0f, static_cast<float>(height - 1u));
            const UINT y0 = static_cast<UINT>(sourceYf);
            const UINT y1 = (y0 + 1u < height) ? y0 + 1u : y0;
            const float wy = sourceYf - static_cast<float>(y0);
            for (UINT x = 0; x < modelWidth; ++x) {
                const float sourceXf = std::clamp((static_cast<float>(x) + 0.5f) * scaleX - 0.5f, 0.0f, static_cast<float>(width - 1u));
                const UINT x0 = static_cast<UINT>(sourceXf);
                const UINT x1 = (x0 + 1u < width) ? x0 + 1u : x0;
                const float wx = sourceXf - static_cast<float>(x0);
                const size_t i00 = (static_cast<size_t>(y0) * width + x0) * 4ULL;
                const size_t i01 = (static_cast<size_t>(y0) * width + x1) * 4ULL;
                const size_t i10 = (static_cast<size_t>(y1) * width + x0) * 4ULL;
                const size_t i11 = (static_cast<size_t>(y1) * width + x1) * 4ULL;
                const size_t pixel = static_cast<size_t>(y) * modelWidth + x;
                for (int channel = 0; channel < 3; ++channel) {
                    const int bgra = 2 - channel; // RGB planes out of a BGRA buffer
                    const float topA = previousFrame[i00 + bgra] * (1.0f - wx) + previousFrame[i01 + bgra] * wx;
                    const float bottomA = previousFrame[i10 + bgra] * (1.0f - wx) + previousFrame[i11 + bgra] * wx;
                    first[static_cast<size_t>(channel) * modelPlaneSize + pixel] = (topA * (1.0f - wy) + bottomA * wy) / 255.0f;
                    const float topB = currentFrame[i00 + bgra] * (1.0f - wx) + currentFrame[i01 + bgra] * wx;
                    const float bottomB = currentFrame[i10 + bgra] * (1.0f - wx) + currentFrame[i11 + bgra] * wx;
                    second[static_cast<size_t>(channel) * modelPlaneSize + pixel] = (topB * (1.0f - wy) + bottomB * wy) / 255.0f;
                }
            }
        }

        }
        const int64_t resampleEnd = GetCurrentTime100ns();

        try {
            Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            std::array<int64_t, 4> shape = { 1, 3, static_cast<int64_t>(modelHeight), static_cast<int64_t>(modelWidth) };
            auto firstTensor = Ort::Value::CreateTensor<float>(memoryInfo, first.data(), first.size(), shape.data(), shape.size());
            auto secondTensor = Ort::Value::CreateTensor<float>(memoryInfo, second.data(), second.size(), shape.data(), shape.size());
            std::array<Ort::Value, 2> inputs = { std::move(firstTensor), std::move(secondTensor) };
            const char* inputNames[] = { "img0", "img1" };
            const char* outputNames[] = { flowReconstruction_ ? "flow_mask" : "output" };
            Ort::RunOptions runOptions;
            const auto runId = ++rifeRunId_;
            const std::string tag = "rife_" + std::to_string(runId);
            runOptions.SetRunTag(tag.c_str());
            const int64_t runStart = GetCurrentTime100ns();
            std::vector<Ort::Value> output;
            if (rifeGpuIo_) rifeGpuIo_->Run(*rifeSession_, runOptions, first, second, outputNames[0]);
            else if (reuseRifeIo_) rifeIo_->Run(*rifeSession_, runOptions, outputNames[0]);
            else output = rifeSession_->Run(runOptions, inputNames, inputs.data(), 2, outputNames, 1);
            const int64_t inferenceEnd = GetCurrentTime100ns();
            auto outputShape = reuseRifeIo_ ? std::vector<int64_t>{1, flowReconstruction_ ? 5 : 3, kRifeHeight, kRifeWidth}
                                          : output[0].GetTensorTypeAndShapeInfo().GetShape();
            if (outputShape != std::vector<int64_t>{1, flowReconstruction_ ? 5 : 3, kRifeHeight, kRifeWidth}) {
                LogCapture("RIFE output shape differs from selected image/flow contract.");
                return;
            }
            const UINT modelOutputWidth = static_cast<UINT>(outputShape[3]);
            const UINT modelOutputHeight = static_cast<UINT>(outputShape[2]);
            const size_t outputPlane = static_cast<size_t>(modelOutputWidth) * modelOutputHeight;
            const float* values = rifeGpuIo_ ? rifeGpuIo_->Output().data()
                : (reuseRifeIo_ ? rifeIo_->Output().data() : output[0].GetTensorData<float>());
            resultWidth = modelOutputWidth;
            resultHeight = modelOutputHeight;
            if (flowReconstruction_) {
                fields.assign(values, values + outputPlane * 5);
            } else {
            result.resize(outputPlane * 4ULL);
            for (size_t pixel = 0; pixel < outputPlane; ++pixel) {
                result[pixel * 4ULL + 0] = static_cast<uint8_t>(std::clamp(values[outputPlane * 2ULL + pixel], 0.0f, 1.0f) * 255.0f);
                result[pixel * 4ULL + 1] = static_cast<uint8_t>(std::clamp(values[outputPlane + pixel], 0.0f, 1.0f) * 255.0f);
                result[pixel * 4ULL + 2] = static_cast<uint8_t>(std::clamp(values[pixel], 0.0f, 1.0f) * 255.0f);
                result[pixel * 4ULL + 3] = 255;
            }
            }
            const int64_t convertEnd = GetCurrentTime100ns();
            // Sample normal calls, but preserve every >50ms inference outlier.
            // QPC endpoints permit alignment without treating log receipt as execution time.
            const bool slowCall = inferenceEnd - resampleEnd >= 500000;
            if (slowCall || convertEnd - lastRifeTimingLog_ >= 10000000) {
            lastRifeTimingLog_ = convertEnd;
            LogCapture(std::string(flowReconstruction_ ? "RIFE flow completed at " : "RIFE frame completed at ") + std::to_string(resultWidth) + "x" + std::to_string(resultHeight) + " (" + rifeProvider_ + "). CPU prepare "
                + std::to_string((resampleEnd - resampleStart) / 10000.0) + " ms, inference "
                + std::to_string((inferenceEnd - resampleEnd) / 10000.0) + " ms, convert "
                + std::to_string((convertEnd - inferenceEnd) / 10000.0) + " ms. run_start_100ns="
                + std::to_string(runStart) + " run_end_100ns=" + std::to_string(inferenceEnd)
                + " run_id=" + std::to_string(runId)
                + " ort_run_ms=" + std::to_string((inferenceEnd - runStart) / 10000.0)
                + " setup_ms=" + std::to_string((runStart - resampleEnd) / 10000.0)
                + " slow_call=" + (slowCall ? "1" : "0"));
            if (rifeGpuIo_) {
                const auto& t = rifeGpuIo_->LastTiming();
                LogCapture("GPU IO: run_id=" + std::to_string(runId)
                    + " upload_submit_ms=" + std::to_string(t.uploadSubmitMs)
                    + " run_host_ms=" + std::to_string(t.runCpuMs)
                    + " finish_wait_ms=" + std::to_string(t.finishWaitMs)
                    + " readback_cpu_ms=" + std::to_string(t.readbackCpuMs)
                    + " upload_queue_ms=" + std::to_string(t.uploadQueueMs)
                    + " inference_queue_elapsed_ms=" + std::to_string(t.inferenceQueueMs)
                    + " readback_queue_ms=" + std::to_string(t.readbackQueueMs));
            }
            }
        } catch (const std::exception& error) {
            LogCapture(std::string("RIFE inference error: ") + error.what());
            result.clear();
            fields.clear();
        }
    }


    void UploadProcessedFrame(const std::vector<uint8_t>& pixels, UINT width, UINT height) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data = { pixels.data(), width * 4u, 0 };
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        if (SUCCEEDED(device_->CreateTexture2D(&desc, &data, &texture)) &&
            SUCCEEDED(device_->CreateShaderResourceView(texture.Get(), nullptr, &view))) {
            processedTexture_ = texture;
            processedView_ = view;
            hasProcessedFrame_ = true;
        }
    }

    const char* FgDisplayModeName() const {
        switch (fgDisplayMode_) {
        case 1: return "buffered-real-only (inference continues; generated images hidden)";
        case 2: return "matched-576x320 (DIAGNOSTIC: real images deliberately softened)";
        default: return "normal-rife (original real-frame resolution)";
        }
    }

    void Render() {
        reconstructor_.PollGpuTiming(context_.Get());
        // Poll every render tick, including ticks without a new capture.
        ConsumeProcessedFrame();
        if (++renderCounter_ % 120u == 0u) {
            LogCapture("DISPLAY CACHE: enabled=" + std::to_string(displayCacheEnabled_)
                + " hits=" + std::to_string(displayCacheHits_) + " draws=" + std::to_string(displayDraws_));
            SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            if (targetWindow_ && GetForegroundWindow() != targetWindow_) {
                ActivateTargetWindow();
            }
            // Log diagnostics periodically
            {
                const int dstW = monitorRect_.right - monitorRect_.left;
                const int dstH = monitorRect_.bottom - monitorRect_.top;
                UINT srcW = 0, srcH = 0;
                if (frameB_.texture) {
                    D3D11_TEXTURE2D_DESC d = {};
                    frameB_.texture->GetDesc(&d);
                    srcW = d.Width;
                    srcH = d.Height;
                }
                const bool isUpscale = srcW > 0 && srcH > 0 && dstW >= static_cast<int>(srcW) && dstH >= static_cast<int>(srcH);
                const double scale = (srcW > 0) ? static_cast<double>(dstW) / srcW : 0.0;
                LogCapture("UPSCALE CHECK: src " + std::to_string(srcW) + "x" + std::to_string(srcH) +
                           " -> dst " + std::to_string(dstW) + "x" + std::to_string(dstH) +
                           " | isUpscale=" + std::string(isUpscale ? "TRUE" : "FALSE") +
                           " | scale=" + std::to_string(scale) + "x" +
                           " | bicubic=" + std::string(scalerBicubic_ ? "on" : "off") +
                           " | sharpness=" + std::to_string(scalerSharpness_));
            }
            if (frameGenerationEnabled_) {
                double infTimeMs = inferenceTime100ns_.load() / 10000.0;
                double qLatencyMs = queueLatency100ns_.load() / 10000.0;
                int64_t age = 0;
                if (frameB_.captureTime > 0) age = GetCurrentTime100ns() - frameB_.captureTime;
                double ageMs = age / 10000.0;
                const int64_t statsNow = GetCurrentTime100ns();
                const double seconds = (statsNow - statsTime_) / 10000000.0;
                const double generatedRate = seconds > 0 ? (presentedRifeFrames_ - statsPresented_) / seconds : 0;
                statsTime_ = statsNow;
                statsPresented_ = presentedRifeFrames_;
                LogCapture("FG STATS: worker_ms=" + std::to_string(infTimeMs)
                    + " readback_ms=" + std::to_string(readbackTime100ns_ / 10000.0)
                    + " preprocess=" + std::string(gpuPreprocessing_ ? "gpu" : "cpu")
                    + " compact_reads=" + std::to_string(rifePreprocessor_.Readbacks())
                    + " cache_hits=" + std::to_string(rifePreprocessor_.CacheHits())
                    + " queue_ms=" + std::to_string(qLatencyMs) + " capture_age_ms=" + std::to_string(ageMs)
                    + " source_interval_ms=" + std::to_string(captureInterval100ns_ / 10000.0)
                    + " service_ema_ms=" + std::to_string(admission_.Estimate() / 10000.0)
                    + " admission=" + std::string(admission_.CoolingDown() ? "probe" : "normal")
                    + " delay_ms=" + std::to_string(adaptiveLatency_.GetDelay() / 10000.0)
                    + " playback_delay_ms=" + std::to_string(timeline_.EffectiveDelayAt(statsNow) / 10000.0)
                    + " e2e_worst_ms=" + std::to_string(adaptiveLatency_.GetWorstService() / 10000.0)
                    + " e2e_ema_ms=" + std::to_string(adaptiveLatency_.GetServiceEma() / 10000.0)
                    + " latency_outlier=" + std::to_string(adaptiveLatency_.IsThermalSpike() ? 1 : 0)
                    + " present_return_avg_ms=" + std::to_string(scanout_.avgPresentInterval / 10000.0)
                    + " present_return_min_ms=" + std::to_string(scanout_.minPresentInterval / 10000.0)
                    + " present_return_max_ms=" + std::to_string(scanout_.maxPresentInterval / 10000.0)
                    + " present_return_intervals=" + std::to_string(scanout_.presentCount)
                    + " dxgi_stats_available=" + std::to_string(scanout_.frameStatsAvailable)
                    + " dxgi_present_count=" + std::to_string(scanout_.frameStatsPresentCount)
                    + " dxgi_present_refresh=" + std::to_string(scanout_.frameStatsPresentRefreshCount)
                    + " dxgi_sync_refresh=" + std::to_string(scanout_.frameStatsSyncRefreshCount)
                    + " generated_per_sec=" + std::to_string(generatedRate)
                    + " display_mode=" + std::to_string(fgDisplayMode_)
                    + " reconstruction=" + std::string(flowReconstruction_ ? "full-res-flow" : "legacy-image")
                    + " reconstruction_submit_ms=" + std::to_string(reconstructionSubmit100ns_ / 10000.0)
                    + " result_wait_ms=" + std::to_string(resultWait100ns_ / 10000.0)
                    + " recon_pack_ms=" + std::to_string(reconstructor_.LastTiming().packMs)
                    + " recon_upload_api_ms=" + std::to_string(reconstructor_.LastTiming().uploadMs)
                    + " recon_alloc_ms=" + std::to_string(reconstructor_.LastTiming().allocationMs)
                    + " recon_draw_api_ms=" + std::to_string(reconstructor_.LastTiming().drawMs)
                    + " recon_gpu_ms=" + std::to_string(reconstructor_.GpuMs())
                    + " recon_gpu_samples=" + std::to_string(reconstructor_.GpuSamples())
                    + " present_cpu_ms=" + std::to_string(presentTime100ns_ / 10000.0)
                    + " pacing=" + std::string(frameLatencyEvent_ ? "waitable" : "legacy")
                    + " completion_wakes=" + std::to_string(completionWakes_)
                    + " capture_receive_age_ms=" + std::to_string(captureClock_.lastAge / 10000.0)
                    + " capture_min_age_ms=" + std::to_string(captureClock_.minAge / 10000.0)
                    + " capture_max_age_ms=" + std::to_string(captureClock_.maxAge / 10000.0)
                    + " capture_future_samples=" + std::to_string(captureClock_.futureSamples)
                    + " submitted=" + std::to_string(totalRifeFrames_)
                    + " completed=" + std::to_string(completedRifeFrames_)
                    + " generated_presented=" + std::to_string(presentedRifeFrames_)
                    + " stale=" + std::to_string(staleRifeFrames_)
                    + " busy_skips=" + std::to_string(droppedRifeFrames_)
                    + " auto_skips=" + std::to_string(autoSkippedRifeFrames_)
                    + " retried=" + std::to_string(retriedSubmissions_)
                    + " superseded=" + std::to_string(supersededRequests_)
                    + " expired=" + std::to_string(expiredRequests_)
                    + " precision=" + std::to_string(rifePrecision_));
            }
        }
        int64_t now = GetCurrentTime100ns();
        ID3D11ShaderResourceView* viewToShow = nullptr;
        FrameTimeline<ComPtr<ID3D11ShaderResourceView>>::Selection selected;

        if (frameGenerationEnabled_) {
            selected = presentation_.Choose(timeline_.Select(now, fgDisplayMode_ != 1));
            if (!selected.frame) selected = {frameB_.view, pairId_, false, frameB_.captureTime};
            viewToShow = selected.frame ? selected.frame.Get() : frameB_.view.Get();
        } else {
            if (kEnableEdsrUpscale && hasProcessedFrame_ && !interpolatedFrameReady_) {
                viewToShow = processedView_.Get();
            } else {
                viewToShow = frameB_.view ? frameB_.view.Get() : nullptr;
            }
            selected = {viewToShow, pairId_, false, frameB_.captureTime};
        }

        if (frameGenerationEnabled_ && fgDisplayMode_ == 2 && viewToShow && (flowReconstruction_ || !selected.generated)) {
            if (comparisonResize_.Resize(context_.Get(), viewToShow)) {
                viewToShow = comparisonResize_.OutputView();
            } else {
                LogCapture("FG comparison resize failed; returning to normal display.");
                fgDisplayMode_ = 0;
            }
        }
        // Diagnostic resize reuses a mutable texture; never key its content by SRV.
        const bool cacheable = displayCacheEnabled_ && viewToShow &&
            !(frameGenerationEnabled_ && fgDisplayMode_ == 2);
        if (!cacheable) displayCache_.Invalidate();
        const bool cached = cacheable && displayCache_.Restore(context_.Get(), displayBackBuffer_.Get(),
            viewToShow, scalerRevision_);
        if (cached) ++displayCacheHits_;
        if (!cached) {
        // Resize changes render state. Always bind display target/viewport AFTER it.
        float clear[] = { 0, 0, 0, 1 };
        context_->OMSetRenderTargets(1, renderTarget_.GetAddressOf(), nullptr);
        D3D11_VIEWPORT viewport = { 0, 0, static_cast<float>(monitorRect_.right - monitorRect_.left), static_cast<float>(monitorRect_.bottom - monitorRect_.top), 0, 1 };
        context_->RSSetViewports(1, &viewport);
        context_->ClearRenderTargetView(renderTarget_.Get(), clear);

        bool fsrDrawn = false;
        if (viewToShow && fsrEnabled_) {
            fsrDrawn = fsrUpscaler_.Draw(context_.Get(), viewToShow, renderTarget_.Get(),
                monitorRect_.right - monitorRect_.left, monitorRect_.bottom - monitorRect_.top,
                scalerBicubic_, scalerSharpness_);
            if (!fsrDrawn) {
                fsrEnabled_ = false;
                LogCapture("FSR draw failed; using legacy scaler.");
            } else ++displayDraws_;
        }
        if (viewToShow && !fsrDrawn) {
            UINT stride = sizeof(Vertex), offset = 0;
            ID3D11Buffer* buffers[] = { vertexBuffer_.Get() };
            ID3D11ShaderResourceView* views[] = { viewToShow };
            ID3D11SamplerState* samplers[] = { sampler_.Get() };
            context_->IASetInputLayout(inputLayout_.Get());
            context_->IASetVertexBuffers(0, 1, buffers, &stride, &offset);
            context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
            context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
            context_->PSSetConstantBuffers(0, 1, scalerConstants_.GetAddressOf());
            context_->PSSetShaderResources(0, 1, views);
            context_->PSSetSamplers(0, 1, samplers);
            context_->Draw(4, 0);
            ++displayDraws_;

        }
        if (cacheable && !displayCache_.Store(device_.Get(), context_.Get(), displayBackBuffer_.Get(),
            viewToShow, scalerRevision_)) {
            displayCacheEnabled_ = false;
            LogCapture("Display cache allocation failed; using baseline rendering.");
        }
        }
        // Cache contains game pixels ONLY. Restore/redraw before adding the UI
        // every frame so changing text and hiding the panel cannot leave trails.
        statusRate_.Observe(now, presentedRifeFrames_);
        if (statusVisible_) {
            StatusSnapshot status;
            status.target = captureTarget_.Bound() ? targetLabel_ : "Waiting for Witcher 3 (default)";
            if (frameB_.texture) {
                D3D11_TEXTURE2D_DESC desc = {}; frameB_.texture->GetDesc(&desc);
                status.sourceWidth = desc.Width; status.sourceHeight = desc.Height;
            }
            status.scaler = scalerBicubic_ ? (fsrEnabled_ ? "FSR1 EASU + RCAS" : "Bicubic + legacy sharpen")
                : (fsrEnabled_ ? "Bilinear + RCAS" : "Bilinear + legacy sharpen");
            status.sharpness = scalerSharpness_;
            // modelReady_ is the publication barrier for initialization data.
            status.fgState = !modelReady_.load() ? "Loading / unavailable" : !rifeAvailable_.load() ? "Unavailable"
                : !frameGenerationEnabled_ ? "OFF (ready)" : admission_.CoolingDown() ? "ON (cooldown / probing)" : "ON";
            status.displayMode = FgDisplayModeName();
            status.generatedRate = frameGenerationEnabled_ ? statusRate_.Value() : 0;
            status.bufferMs = frameGenerationEnabled_ ? timeline_.EffectiveDelayAt(now) / 10000.0 : 0;
            status.workerMs = inferenceTime100ns_.load() / 10000.0;
            status.realIdentityRate = identities_.Rate().real;
            status.generatedIdentityRate = identities_.Rate().generated;
            status.repeatRate = identities_.Rate().repeats;
            if (statusOverlay_.Draw(context_.Get(), renderTarget_.Get(),
                monitorRect_.right - monitorRect_.left, monitorRect_.bottom - monitorRect_.top, now, status)
                && !statusDrawLogged_) {
                statusDrawLogged_ = true;
                LogCapture("Status panel: draw submitted after game-image cache.");
            }
        }
        const int64_t presentStart = GetCurrentTime100ns();
        LARGE_INTEGER traceStart = {}, traceEnd = {};
        if (presentTrace_.is_open()) QueryPerformanceCounter(&traceStart);
        HRESULT presentResult = swapChain_->Present(1, 0);
        if (presentTrace_.is_open()) QueryPerformanceCounter(&traceEnd);
        presentTime100ns_ = GetCurrentTime100ns() - presentStart;
        const int64_t identityNow = presentStart + presentTime100ns_;
        identities_.Observe(presentResult == S_OK, viewToShow != nullptr,
            selected.contentTime, selected.generated, identityNow);
        if (presentTrace_.is_open()) {
            LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
            presentTrace_ << ++traceSequence_ << ',' << GetCurrentProcessId() << ',' << identities_.Segment()
                << ',' << frameGenerationEnabled_ << ',' << fgDisplayMode_ << ',' << selected.pair
                << ',' << selected.contentTime << ',' << selected.generated << ',' << (viewToShow != nullptr)
                << ',' << traceStart.QuadPart << ',' << traceEnd.QuadPart << ',' << frequency.QuadPart
                << ',' << static_cast<int64_t>(presentResult) << '\n';
            // Bound diagnostics to about 15 minutes at 144 Hz; never grow without limit.
            if (!presentTrace_ || traceSequence_ >= 150000) {
                presentTrace_.close();
                LogCapture("Presentation identity CSV stopped (row limit or write failure).");
            }
        }
        if (identities_.Sample(identityNow)) {
            const auto& r = identities_.Rate();
            LogCapture("FRAME IDENTITIES: segment=" + std::to_string(identities_.Segment())
                + " fg=" + std::to_string(frameGenerationEnabled_) + " mode=" + std::to_string(fgDisplayMode_)
                + " real_per_sec=" + std::to_string(r.real)
                + " generated_per_sec=" + std::to_string(r.generated)
                + " novel_per_sec=" + std::to_string(r.real + r.generated)
                + " repeats_per_sec=" + std::to_string(r.repeats)
                + " change_return_min_ms=" + std::to_string(r.changeMinMs)
                + " change_return_max_ms=" + std::to_string(r.changeMaxMs)
                + " regressions=" + std::to_string(identities_.Total().regressions)
                + " unsuccessful=" + std::to_string(identities_.Total().unsuccessful)
                + " measurement=submitted_identities_not_display_fps");
        }
        if (presentResult == S_OK) {
            // CPU Present-return intervals, NOT physical scanout or content FPS.
            const int64_t presentAt = presentStart + presentTime100ns_;
            if (scanout_.lastPresentQpc > 0) {
                const int64_t interval = presentAt - scanout_.lastPresentQpc;
                if (interval > 0) {
                    if (scanout_.presentCount == 0) {
                        scanout_.avgPresentInterval = interval;
                        scanout_.minPresentInterval = interval;
                        scanout_.maxPresentInterval = interval;
                    } else {
                        scanout_.avgPresentInterval += (interval - scanout_.avgPresentInterval) / (scanout_.presentCount + 1);
                        if (interval < scanout_.minPresentInterval) scanout_.minPresentInterval = interval;
                        if (interval > scanout_.maxPresentInterval) scanout_.maxPresentInterval = interval;
                    }
                    ++scanout_.presentCount;
                }
            }
            scanout_.lastPresentQpc = presentAt;
            ++scanout_.presentedFrames;
            if (selected.generated) ++scanout_.generatedFramesPresented;
            DXGI_FRAME_STATISTICS frameStats = {};
            if (SUCCEEDED(swapChain_->GetFrameStatistics(&frameStats))) {
                scanout_.frameStatsAvailable = true;
                scanout_.frameStatsPresentCount = frameStats.PresentCount;
                scanout_.frameStatsSyncRefreshCount = frameStats.SyncRefreshCount;
                scanout_.frameStatsPresentRefreshCount = frameStats.PresentRefreshCount;
            } else {
                scanout_.frameStatsAvailable = false;
                scanout_.frameStatsPresentCount = 0;
                scanout_.frameStatsSyncRefreshCount = 0;
                scanout_.frameStatsPresentRefreshCount = 0;
            }
        }
        if (presentResult == S_OK) presentation_.Commit(selected);
        if (presentResult == S_OK && selected.generated && selected.pair != lastPresentedRifePair_) {
            lastPresentedRifePair_ = selected.pair;
            ++presentedRifeFrames_;
        }
        if (FAILED(presentResult) && presentResult != DXGI_ERROR_WAS_STILL_DRAWING) {
            LogCapture("Compositor Present failed.");
        }
    }
    HINSTANCE instance_ = nullptr;
    StatusOverlay statusOverlay_;
    StatusRate statusRate_;
    FrameIdentityStats identities_;
    std::ofstream presentTrace_;
    uint64_t traceSequence_ = 0;
    bool statusVisible_ = false; // preserve baseline rendering cost until requested
    bool statusDrawLogged_ = false;
    std::atomic<bool> flowReconstruction_{false};
    bool reconstructorReady_ = false;
    bool loggedReconstruction_ = false;
    RifeReconstructor reconstructor_;
    std::vector<float> processedFields_; // Protected by resultMutex_.
    int64_t reconstructionSubmit100ns_ = 0; // CPU submission time, NOT GPU execution time.
    CaptureClockAudit captureClock_;
    int64_t resultCompletedAt_ = 0; // Protected by resultMutex_.
    int64_t resultWait100ns_ = 0;
    int64_t presentTime100ns_ = 0;
    winrt::handle completionEvent_;
    winrt::handle frameLatencyEvent_;
    bool displayReady_ = false;
    int64_t completionWakes_ = 0;
    HWND window_ = nullptr;
    HWND targetWindow_ = nullptr;
    RECT monitorRect_ = {};
    bool running_ = true;
    std::wstring startupError_;
    bool hasFrame_ = false;
    bool hasProcessedFrame_ = false;
    bool loggedFirstFrame_ = false;
    bool loggedMissingWindow_ = false;
    bool loggedCaptureLookup_ = false;
    bool systemCursorHidden_ = false;
    UINT processingCounter_ = 0;
    UINT capturedFrameCount_ = 0;
    UINT renderCounter_ = 0;
    UINT stagingWidth_ = 0;
    UINT stagingHeight_ = 0;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID3D11RenderTargetView> renderTarget_;
    ComPtr<ID3D11Texture2D> displayBackBuffer_;
    DisplayCache displayCache_;
    FsrUpscaler fsrUpscaler_;
    bool fsrEnabled_ = false;
    bool displayCacheEnabled_ = false;
    uint64_t scalerRevision_ = 0, displayCacheHits_ = 0, displayDraws_ = 0;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11Buffer> vertexBuffer_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> scalerConstants_;
    bool scalerBicubic_ = true;
    float scalerSharpness_ = 0.6f;

    void UpdateScalerConstants() {
        ++scalerRevision_;
        displayCache_.Invalidate();
        if (!context_ || !scalerConstants_) return;
        ScalerConstants constants = {};
        constants.dstSize[0] = static_cast<float>(monitorRect_.right - monitorRect_.left);
        constants.dstSize[1] = static_cast<float>(monitorRect_.bottom - monitorRect_.top);
        constants.bicubicEnabled = scalerBicubic_ ? 1.0f : 0.0f;
        constants.sharpness = scalerSharpness_;
        context_->UpdateSubresource(scalerConstants_.Get(), 0, nullptr, &constants, 0, 0);
    }
    ScheduledFrame frameA_;
    CaptureTarget captureTarget_;
    std::string targetLabel_;
    ScheduledFrame frameB_;
    FrameTimeline<ComPtr<ID3D11ShaderResourceView>> timeline_;
    PresentationGuard<ComPtr<ID3D11ShaderResourceView>> presentation_;
    int fgDisplayMode_ = 0; // F8: normal, buffered real-only, matched resolution
    RifePreprocessor comparisonResize_;
    bool comparisonReady_ = false;
    std::atomic<bool> rifeAvailable_ = false;
    int64_t captureInterval100ns_ = 0;
    int64_t readbackTime100ns_ = 0;
    FrameJobAdmission admission_;
    AdaptiveLatency adaptiveLatency_;
    ScanoutStats scanout_;
    int64_t firstEnabledPairId_ = 0;
    int64_t pendingServiceStart_ = 0;
    int64_t pendingCaptureTime_ = 0;
    int64_t resultServiceTime_ = 0;
    int64_t resultCaptureTime_ = 0;
    int64_t statsTime_ = 0;
    int64_t statsPresented_ = 0;
    int64_t completedRifeFrames_ = 0;
    int64_t presentedRifeFrames_ = 0;
    int64_t staleRifeFrames_ = 0;
    int64_t lastPresentedRifePair_ = -1;
    int64_t pairId_ = 0;
    int64_t pendingPairId_ = 0;
    int64_t resultPairId_ = 0;
    int64_t interpolatedPairId_ = 0;
    std::string rifeProvider_ = "none";
    int dmlDevice_ = 0;

    int64_t pendingQueueTime_ = 0;
    std::atomic<int64_t> inferenceTime100ns_ = 0;
    std::atomic<int64_t> queueLatency100ns_ = 0;
    int64_t droppedRifeFrames_ = 0;
    int64_t totalRifeFrames_ = 0;
    int64_t autoSkippedRifeFrames_ = 0;
    LatestFrameRequest latestRequest_;
    int64_t retriedSubmissions_ = 0;
    int64_t supersededRequests_ = 0;
    int64_t expiredRequests_ = 0;
    int rifePrecision_ = 32;
    ComPtr<ID3D11Texture2D> stagingTexture_;
    RifePreprocessor rifePreprocessor_;
    bool gpuPreprocessing_ = false;
    int64_t lastRifeTimingLog_ = 0; // inference thread only
    ComPtr<ID3D11Texture2D> processedTexture_;
    ComPtr<ID3D11ShaderResourceView> processedView_;
    GraphicsCaptureItem captureItem_{ nullptr };
    Direct3D11CaptureFramePool framePool_{ nullptr };
    GraphicsCaptureSession captureSession_{ nullptr };
    IDirect3DDevice direct3dDevice_{ nullptr };
    std::unique_ptr<Ort::Env> ortEnvironment_;
    std::unique_ptr<Ort::SessionOptions> ortOptions_;
    std::unique_ptr<Ort::Session> edsrSession_;
    std::unique_ptr<DmlGpuIo> rifeGpuIo_; // destroyed after session; worker joined first
    std::unique_ptr<Ort::Session> rifeSession_;
    std::unique_ptr<RifeIoBuffers> rifeIo_; // only accessed by inference thread
    bool reuseRifeIo_ = true;
    bool rifeProfileEnabled_ = false;
    uint64_t rifeRunId_ = 0;
    std::atomic<bool> modelReady_ = false;
    std::thread modelThread_;
    std::thread inferenceThread_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::vector<uint8_t> pendingFrame_;
    std::vector<uint8_t> pendingPreviousFrame_;
    UINT pendingWidth_ = 0;
    UINT pendingHeight_ = 0;
    bool pendingUseRife_ = false;
    bool inferenceBusy_ = false;
    bool stopInference_ = false;
    std::mutex resultMutex_;
    std::vector<uint8_t> processedPixels_;
    UINT processedWidth_ = 0;
    UINT processedHeight_ = 0;
    bool resultReady_ = false;
    bool resultIsRife_ = false;
    bool interpolatedFrameReady_ = false;
    bool inferenceStarted_ = false;
    bool frameGenerationEnabled_ = false;

public:
    const std::wstring& StartupError() const { return startupError_; }
};

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    EnableNativeDpi();
    LogCapture("Compositor process started.");
    Compositor compositor;
    if (!compositor.Initialize(instance)) {
        std::wstring message = L"AIFrameEngine compositor initialization failed at: " + compositor.StartupError();
        MessageBoxW(nullptr, message.c_str(), L"AIFrameEngine", MB_ICONERROR);
        return 1;
    }
    return compositor.Run();
}
