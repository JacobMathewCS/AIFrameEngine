#pragma once
#include "interop/gpu_resident_run.h"
#include "texture_handoff.h"
#include "finite_fields.h"

// Scratch correctness path. Single worker, exclusive immediate context. Blocking
// status wait is bounded and is not proposed as the production scheduling design.
struct GuardedTextureWork {
    std::shared_ptr<void> consumerOwner;
    FiniteFields finite;
    std::shared_ptr<PlainTexture> reconstruction;
    TextureHandoff handoff;
    GuardedTextureWork(std::shared_ptr<GpuProviderContext> context, GpuProducerBridge& bridge,
        ID3D11DeviceContext* immediate, std::shared_ptr<void> consumer)
        : consumerOwner(std::move(consumer)), finite(context),
          reconstruction(std::make_shared<PlainTexture>(context, bridge.Original(0), bridge.Original(1), true)),
          handoff(context, reconstruction, immediate) {
        if (!consumerOwner || !bridge.BelongsTo(context.get()) || bridge.GetStage() != GpuProducerBridge::Stage::Ready)
            throw std::invalid_argument("guarded work owner/context/stage");
    }
    template<class Consumer> void Consume(ID3D12Resource* fields, ID3D12CommandQueue* queue, Consumer&& consumer) {
        if (!finite.CheckFinite(fields, queue, 30000))
            throw std::domain_error("nonfinite model output; publication rejected");
        handoff.Submit(fields, queue, std::forward<Consumer>(consumer));
    }
};

class ResidentTextureRun {
public:
    using Consumer = std::function<void(ID3D11DeviceContext4*, ID3D11ShaderResourceView*)>;
    ResidentTextureRun(std::shared_ptr<GpuProviderContext> context, std::shared_ptr<GpuBoundSession> session,
        std::unique_ptr<GpuProducerBridge>& bridge, std::unique_ptr<GpuFlowOutput>& output,
        ID3D11DeviceContext* immediate, std::shared_ptr<void> consumerOwner) {
        GpuResidentRun::ValidateResources(context.get(), bridge.get(), output.get());
        auto work = std::make_shared<GuardedTextureWork>(context, *bridge, immediate, std::move(consumerOwner));
        run_ = std::make_unique<GpuResidentRun>(std::move(context), std::move(session), bridge, output, work);
        work_ = std::move(work);
    }
    ResidentTextureRun(const ResidentTextureRun&) = delete;
    ResidentTextureRun& operator=(const ResidentTextureRun&) = delete;
    void Submit(const Consumer& consumer) {
        if (!consumer || !work_ || run_->Submitted()) throw std::logic_error("resident consumer/submission unavailable");
        run_->Submit([](auto*) {}, [&](auto* fields, auto* queue) { work_->Consume(fields, queue, consumer); });
    }
    bool Submitted() const noexcept { return run_->Submitted(); }
#ifdef AIFRAME_COMBINED_TEST_ORACLES
    // Explicit test-only transfers. Normal Submit has no field/pixel oracle.
    void SubmitForTest(const Consumer& consumer, const GpuResidentRun::Before& before,
        const GpuResidentRun::After& after) {
        if (!consumer || !before || !after || !work_ || run_->Submitted())
            throw std::logic_error("resident test submission unavailable");
        run_->Submit(before, [&](auto* fields, auto* queue) {
            after(fields, queue); work_->Consume(fields, queue, consumer);
        });
    }
    ID3D12Resource* TestOutput() const { return work_->reconstruction->Output(); }
#endif
    bool Retire(DWORD ms) {
        if (!run_->Retire(ms)) return false;
        work_.reset(); return true;
    }
private:
    std::shared_ptr<GuardedTextureWork> work_;
    std::unique_ptr<GpuResidentRun> run_;
};