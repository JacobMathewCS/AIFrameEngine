#pragma once
#include <d3d11.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <cstdint>
#include <string>

// Render-thread-only sampling, independent of the diagnostic log cadence.
class StatusRate {
public:
    void Reset() { started_ = false; rate_ = 0; }
    void Observe(int64_t now, int64_t count) {
        if (!started_ || now < time_ || count < count_) {
            started_ = true; time_ = now; count_ = count; rate_ = 0; return;
        }
        if (now - time_ >= 10000000) {
            rate_ = double(count - count_) * 10000000.0 / double(now - time_);
            time_ = now; count_ = count;
        }
    }
    double Value() const { return rate_; }
private:
    bool started_ = false;
    int64_t time_ = 0, count_ = 0;
    double rate_ = 0;
};

struct StatusSnapshot {
    std::string target, scaler, fgState, displayMode;
    unsigned sourceWidth = 0, sourceHeight = 0;
    float sharpness = 0;
    double generatedRate = 0, bufferMs = 0, workerMs = 0;
    double realIdentityRate = 0, generatedIdentityRate = 0, repeatRate = 0;
};

// Intentionally no platform/input backend: this informational overlay never
// consumes game input or changes the cursor. Caller supplies size and time.
class StatusOverlay {
public:
    StatusOverlay() = default;
    StatusOverlay(const StatusOverlay&) = delete;
    StatusOverlay& operator=(const StatusOverlay&) = delete;
    ~StatusOverlay() { Shutdown(); }
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
        Shutdown();
        IMGUI_CHECKVERSION();
        ui_ = ImGui::CreateContext();
        ImGui::SetCurrentContext(ui_);
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().LogFilename = nullptr;
        ImGui::StyleColorsDark();
        ready_ = ImGui_ImplDX11_Init(device, context);
        if (!ready_) Shutdown();
        return ready_;
    }
    void Shutdown() {
        if (!ui_) return;
        ImGui::SetCurrentContext(ui_);
        if (ready_) ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext(ui_);
        ui_ = nullptr; ready_ = false; lastTime_ = 0;
    }
    bool Draw(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
              unsigned width, unsigned height, int64_t now, const StatusSnapshot& s) {
        if (!ready_ || !target || !width || !height) return false;
        ImGui::SetCurrentContext(ui_);
        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(float(width), float(height));
        io.DeltaTime = lastTime_ && now > lastTime_ ? float(now - lastTime_) / 10000000.f : 1.f / 60.f;
        lastTime_ = now;
        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.86f);
        ImGui::Begin("AIFrameEngine status", nullptr, ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::Text("Target: %s", s.target.c_str());
        ImGui::Text("Source %ux%u -> output %ux%u", s.sourceWidth, s.sourceHeight, width, height);
        ImGui::Text("Scaler: %s | strength %.2f", s.scaler.c_str(), s.sharpness);
        ImGui::Text("Frame generation: %s", s.fgState.c_str());
        ImGui::Text("Mode: %s", s.displayMode.c_str());
        ImGui::Text("Generated submissions: %.1f/sec (not display FPS)", s.generatedRate);
        ImGui::Text("New identities/sec: real %.1f + generated %.1f = %.1f",
            s.realIdentityRate, s.generatedIdentityRate, s.realIdentityRate + s.generatedIdentityRate);
        ImGui::Text("Repeated identities/sec: %.1f (not pixel/display verification)", s.repeatRate);
        ImGui::Text("Buffer: %.1f ms (not total input latency)", s.bufferMs);
        ImGui::Text("Last worker: %.1f ms", s.workerMs);
        ImGui::Separator();
        ImGui::TextUnformatted("F9: frame gen | F10: scaler | F11: sharpen");
        ImGui::TextUnformatted("Ctrl+Shift+Up/Down: sharpen +/-0.05 (0 to 1)");
        ImGui::TextUnformatted("F8: diagnostic mode | Ctrl+Shift+F6: hide/show");
        ImGui::TextUnformatted("Ctrl+Shift+F7: export comparison | Ctrl+Shift+F12: quit");
        ImGui::End();
        ImGui::Render();
        context->OMSetRenderTargets(1, &target, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        return true;
    }
private:
    ImGuiContext* ui_ = nullptr;
    bool ready_ = false;
    int64_t lastTime_ = 0;
};