#pragma once
#include "gpu_submission_owner.h"
#include "baseline/gpu_input_pack.h"

// Joined, single-use RGB input preparation. Texture sources must be completed on
// entry, BGRA8/COMMON, immutable through retirement. D3D11 fence bridging remains
// upstream; this class does NOT infer producer completion or resource states.
class GpuJoinedPacking {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
public:
    struct Source {
        ID3D12Resource* texture = nullptr;
        std::shared_ptr<void> producerOwner;
    };
private:
    struct Packet {
        std::array<Source, 2> sources;
        std::array<std::shared_ptr<GpuInputPack>, 2> jobs;
        Ptr<ID3D12CommandAllocator> allocator;
        Ptr<ID3D12GraphicsCommandList> list;
        int64_t width = 0, height = 0;
        Packet(ID3D12Device* device, std::array<Source, 2> in) : sources(std::move(in)) {
            for (const auto& s : sources)
                if (!s.texture || !s.producerOwner) throw std::invalid_argument("missing texture/producer owner");
            const auto first = sources[0].texture->GetDesc(), second = sources[1].texture->GetDesc();
            if (first.Width != second.Width || first.Height != second.Height)
                throw std::invalid_argument("input texture dimensions differ");
            if (GpuOrtInputs::SameObject(sources[0].texture, sources[1].texture))
                throw std::invalid_argument("distinct immutable texture sources required");
            for (size_t i = 0; i < jobs.size(); ++i)
                jobs[i] = std::make_shared<GpuInputPack>(device, sources[i].texture);
            width = static_cast<int64_t>(first.Width); height = first.Height;
            GpuOrtInputs::Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&allocator)), "joined allocator");
            GpuOrtInputs::Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "joined list");
            for (const auto& job : jobs) job->Record(list.Get());
            GpuOrtInputs::Check(list->Close(), "close joined packing");
            // Recording only: nothing was submitted, safe to unwind construction.
        }
    };
public:
    GpuJoinedPacking(std::shared_ptr<GpuProviderContext> context, std::array<Source, 2> sources,
        std::shared_ptr<GpuBoundSession> session = {}) {
        if (!context) throw std::invalid_argument("missing joined provider");
        if (session && !session->BelongsTo(context.get())) throw std::invalid_argument("joined session mismatch");
        auto packet = std::make_shared<Packet>(context->Device(), std::move(sources));
        std::array<GpuOrtInputs::Input, 2> in{{
            {packet->jobs[0]->Output(), packet->jobs[0]}, {packet->jobs[1]->Output(), packet->jobs[1]}}};
        auto inputs = std::make_unique<GpuOrtInputs>(context->Api(), context->Device(), context->Queue(),
            in, packet->width, packet->height);
        owner_.Adopt(std::move(context), inputs, packet, std::move(session));
        packet_ = packet.get(); weakPacket_ = packet;
    }
    GpuJoinedPacking(const GpuJoinedPacking&) = delete;
    GpuJoinedPacking& operator=(const GpuJoinedPacking&) = delete;
    // Inspection only: weak handle cannot replace the owner's completion contract.
    std::weak_ptr<void> Lifetime() const noexcept { return weakPacket_; }
    ID3D12Resource* Output(size_t index) const {
        if (!owner_.Occupied() || index >= 2) throw std::out_of_range("joined output unavailable");
        return packet_->jobs[index]->Output(); // borrowed until retirement; not CPU data
    }
    template<class Consumer> void Submit(Consumer&& consume, GpuOrtInputs::Signal signal = GpuOrtInputs::QueueSignal) {
        // The owner rejects repeated calls before invoking this callback.
        owner_.Submit([&](const auto& names, const auto& values, ID3D12CommandQueue* queue) {
            ID3D12CommandList* lists[]{packet_->list.Get()};
            queue->ExecuteCommandLists(1, lists);
            consume(names, values, queue);
        }, signal);
    }
    void RetrySignal(GpuOrtInputs::Signal signal = GpuOrtInputs::QueueSignal) { owner_.RetrySignal(signal); }
    GpuSubmissionOwner::Retirement Retire(DWORD ms) { return owner_.Retire(ms); }
    bool Occupied() const noexcept { return owner_.Occupied(); }
    GpuOrtInputs::State State() const { return owner_.State(); }
private:
    Packet* packet_ = nullptr; // never dereference after retirement
    std::weak_ptr<Packet> weakPacket_;
    GpuSubmissionOwner owner_; // destroyed first; fails fast on unretired GPU work
};