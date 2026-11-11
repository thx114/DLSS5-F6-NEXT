#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <atomic>
#include <cstdint>

namespace native {
class NativeProducer;
// Unforgeable in the public endpoint API. Only the actual producer can publish
// one, after successful queue Signal. No caller-supplied device-pair alias.
class NativeSubmissionReceipt final {
    friend class NativeProducer;
    Microsoft::WRL::ComPtr<ID3D12Device> creator_, publicOwner_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> anchor_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12Resource> color_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    uint64_t value_, frame_, generation_, batch_;
    std::atomic<bool> active_{true};
    NativeSubmissionReceipt(ID3D12Device* creator, ID3D12Device* owner, ID3D12CommandAllocator* anchor,
        ID3D12CommandQueue* queue, ID3D12Resource* color, ID3D12Fence* fence, uint64_t value,
        uint64_t frame, uint64_t generation, uint64_t batch)
        : creator_(creator),publicOwner_(owner),anchor_(anchor),queue_(queue),color_(color),fence_(fence),value_(value),frame_(frame),generation_(generation),batch_(batch) {}
public:
    uint64_t Generation() const noexcept { return generation_; }
    uint64_t Batch() const noexcept { return batch_; }
    NativeSubmissionReceipt(const NativeSubmissionReceipt&)=delete;
    NativeSubmissionReceipt& operator=(const NativeSubmissionReceipt&)=delete;
    bool Matches(ID3D12Resource* color, ID3D12CommandQueue* consumerQueue,
        ID3D12CommandQueue* producerQueue, ID3D12Fence* fence, uint64_t value,
        ID3D12Device* colorDevice, ID3D12Device* fenceDevice,
        uint64_t frame, uint64_t generation, uint64_t batch) const noexcept {
        if (!active_.load(std::memory_order_acquire) || !generation_ || !batch_ ||
            frame!=frame_ || generation!=generation_ || batch!=batch_ || !creator_ || !publicOwner_ || !anchor_ || !queue_ || !color_ || !fence_ || !value_ || value_==UINT64_MAX ||
            color!=color_.Get() || consumerQueue!=queue_.Get() || producerQueue!=queue_.Get() ||
            fence!=fence_.Get() || value!=value_ || colorDevice!=creator_.Get() || fenceDevice!=publicOwner_.Get()) return false;
        Microsoft::WRL::ComPtr<ID3D12Device> anchorOwner;
        return SUCCEEDED(anchor_->GetDevice(IID_PPV_ARGS(&anchorOwner))) && anchorOwner.Get()==publicOwner_.Get() &&
            SUCCEEDED(creator_->GetDeviceRemovedReason()) && SUCCEEDED(publicOwner_->GetDeviceRemovedReason());
    }
};
using NativeSubmission = std::shared_ptr<const NativeSubmissionReceipt>;
}
