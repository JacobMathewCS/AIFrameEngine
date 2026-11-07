#pragma once
#include "gpu_flow_output.h"
#include "gpu_producer_bridge.h"
#include <functional>

// Single-worker, one-shot typed owner. No raw session/OrtValue is exposed.
// Callback resources must be held in the dependency token; borrowed handles must
// not escape. Hooks run on the exact provider queue and are inside retirement.
class GpuResidentRun {
    struct Dependencies {
        std::shared_ptr<GpuBoundSession> session;
        std::shared_ptr<void> hooks;
        std::unique_ptr<GpuProducerBridge> bridge;
    };
public:
    using Before = std::function<void(ID3D12CommandQueue*)>;
    using After = std::function<void(ID3D12Resource*, ID3D12CommandQueue*)>;
    using State = GpuOrtInputs::State;
    static void ValidateResources(const GpuProviderContext* context, const GpuProducerBridge* bridge,
        const GpuFlowOutput* output) {
        if (!context || !bridge || !output || !bridge->BelongsTo(context) || !output->BelongsTo(context))
            throw std::invalid_argument("resident resource context mismatch");
        if (!bridge->HasFlowShape() || bridge->GetStage() != GpuProducerBridge::Stage::Ready ||
            output->GetState() != State::Ready) throw std::invalid_argument("resident resources not ready/fixed-shape");
    }
    GpuResidentRun(std::shared_ptr<GpuProviderContext> context, std::shared_ptr<GpuBoundSession> session,
        std::unique_ptr<GpuProducerBridge>& bridge, std::unique_ptr<GpuFlowOutput>& output,
        std::shared_ptr<void> hookResources) : context_(std::move(context)) {
        ValidateResources(context_.get(), bridge.get(), output.get());
        if (!session || !session->BelongsTo(context_.get()) || !bridge->UsesSession(session.get()) || !hookResources)
            throw std::invalid_argument("resident session/dependencies mismatch");
        session->ValidateFlowContract();
        auto dependencies = std::make_shared<Dependencies>();
        dependencies->session = std::move(session); dependencies->hooks = std::move(hookResources);
        dependencies->bridge = std::move(bridge); // only transfer after all validation/allocation
        dependencies_ = std::move(dependencies); output_ = std::move(output);
    }
    GpuResidentRun(const GpuResidentRun&) = delete;
    GpuResidentRun& operator=(const GpuResidentRun&) = delete;
    GpuResidentRun(GpuResidentRun&&) = delete;
    GpuResidentRun& operator=(GpuResidentRun&&) = delete;
    bool Submitted() const noexcept { return submitted_; }
    void Submit(const Before& before, const After& after) {
        if (submitted_ || !before || !after) throw std::logic_error("resident submit unavailable");
        submitted_ = true;
        auto& bridge = *dependencies_->bridge;
        bridge.SubmitProducer();
        output_->Submit(dependencies_, [&](OrtValue* value, ID3D12Resource* resource, ID3D12CommandQueue* queue) {
            bridge.SubmitConsumer([&](const auto&, const auto& inputs, ID3D12CommandQueue* inputQueue) {
                if (!GpuOrtInputs::SameObject(inputQueue, queue)) throw std::logic_error("resident queue mismatch");
                before(queue); // optional test-only preprocessing oracle
                dependencies_->session->RunResident(inputs, value);
                after(resource, queue); // GPU consumer / explicit test-only output oracle
            });
        });
    }
    // Explicit recovery only fences/drains; it NEVER retries model inference.
    // False retains the whole owner. Failure/device loss requires caller fail-fast
    // or continued retention; no production shutdown recovery is claimed.
    bool Retire(DWORD timeoutMs) {
        if (timeoutMs == INFINITE || !submitted_) throw std::logic_error("bounded submitted retirement required");
        if (retired_) return true;
        auto& bridge = *dependencies_->bridge;
        using Stage = GpuProducerBridge::Stage;
        if (bridge.GetStage() == Stage::ProducerUnfenced) bridge.RetryProducerSignal();
        if (bridge.GetStage() == Stage::ProducerSignaled)
            bridge.SubmitConsumer([](const auto&, const auto&, auto*) {});
        if (bridge.GetStage() == Stage::ConsumerSubmitted && bridge.ConsumerState() == State::Unfenced)
            bridge.RetryRetirement();
        if (output_->GetState() == State::Unfenced) output_->RetrySignal();
        if (output_->GetState() != State::Ready && !output_->Wait(timeoutMs)) return false;
        if (bridge.Retire(timeoutMs) != GpuSubmissionOwner::Retirement::Complete) return false;
        output_.reset(); dependencies_.reset(); context_.reset(); retired_ = true;
        return true;
    }
private:
    std::shared_ptr<GpuProviderContext> context_;
    std::shared_ptr<Dependencies> dependencies_;
    std::unique_ptr<GpuFlowOutput> output_; // destroyed first, fail-fast if pending
    bool submitted_ = false, retired_ = false;
};