#pragma once

#include "FrameContract.h"
#include <wrl/client.h>
#include <vector>
#include <string>
#include <cstdio>
#include <atomic>

namespace native
{
// Creation entry is recovered from the actual acquired picture AND queue.
// source.Device(), IUnknown identity and adapter identity grant no authority.
struct NativeWorkDevice
{
    Microsoft::WRL::ComPtr<ID3D12Device> device, queueDevice;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Resource> color;
    const char* failure = nullptr;
    HRESULT hr = S_OK, colorHr = E_PENDING, queueHr = E_PENDING;

    bool Resolve(ID3D12Resource* picture, ID3D12CommandQueue* submissionQueue)
    {
        device.Reset(); queueDevice.Reset(); queue.Reset(); color.Reset(); failure = nullptr; hr = S_OK;
        if (!picture || !submissionQueue) { failure = "native color/queue missing"; hr = E_POINTER; return false; }
        hr = colorHr = picture->GetDevice(IID_PPV_ARGS(&device));
        if (FAILED(hr) || !device) { failure = "native color GetDevice"; return false; }
        hr = queueHr = submissionQueue->GetDevice(IID_PPV_ARGS(&queueDevice));
        if (FAILED(hr) || !queueDevice) { failure = "native queue GetDevice"; return false; }
        if (device.Get() != queueDevice.Get()) { failure = "native color/queue device mismatch"; hr = E_INVALIDARG; return false; }
        if (submissionQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        { failure = "native queue is not direct"; hr = E_INVALIDARG; return false; }
        hr = device->GetDeviceRemovedReason();
        if (FAILED(hr)) { failure = "native work device removed"; return false; }
        queue = submissionQueue; color = picture;
        return true;
    }

    static bool Owns(ID3D12Device* expected, ID3D12DeviceChild* child, HRESULT& hr)
    {
        if (!expected || !child) { hr = E_POINTER; return false; }
        Microsoft::WRL::ComPtr<ID3D12Device> actual;
        hr = child->GetDevice(IID_PPV_ARGS(&actual));
        if (FAILED(hr)) return false;
        if (!actual || actual.Get() != expected) { hr = E_INVALIDARG; return false; }
        return true;
    }
};

// Receipts for objects actually used by this producer. The first real allocator
// establishes PublicOwner; it never replaces Creator as a creation entry.
// Queue/list/resource require Creator; allocators/fence require PublicOwner.
// This per-object pattern is validated, never inferred from a private core ABI.
class NativeCreationDomain
{
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    struct Receipt {
        const char* kind;
        HRESULT createHr, deviceHr;
        ID3D12DeviceChild* returned; // scalar only until Create succeeded
        Ptr<ID3D12DeviceChild> object;
        Ptr<ID3D12Device> owner;
        bool forwarded;
    };
    struct Pair { ID3D12GraphicsCommandList* list; ID3D12CommandAllocator* allocator; };
    Ptr<ID3D12Device> creator_, publicOwner_;
    Ptr<ID3D12CommandQueue> queue_;
    std::vector<Receipt> receipts_;
    std::vector<Pair> pairs_;
    std::string report_;
    uint64_t generation_=0;
    inline static std::atomic<uint64_t> nextGeneration_{0};
    void Describe(const Receipt& r, ID3D12Device* expected, const char* predicate) {
        char line[384];
        std::snprintf(line,sizeof(line),"%s predicate=%s create=%08lX GetDevice=%08lX object=%p creator=%p expected=%p actual=%p; ",
            r.kind,predicate,static_cast<unsigned long>(r.createHr),static_cast<unsigned long>(r.deviceHr),
            static_cast<void*>(r.returned),static_cast<void*>(Creator()),static_cast<void*>(expected),static_cast<void*>(r.owner.Get()));
        report_ += line;
    }
    bool Known(ID3D12DeviceChild* child, bool forwarded) const {
        for (const auto& r : receipts_)
            if (r.object.Get() == child && r.forwarded == forwarded && SUCCEEDED(r.createHr) && SUCCEEDED(r.deviceHr)) return true;
        return false;
    }
public:
    uint64_t Generation() const { return generation_; }
    ID3D12Device* Creator() const { return creator_.Get(); }
    ID3D12Device* PublicOwner() const { return publicOwner_.Get(); }
    const std::string& Report() const { return report_; }
    bool Bind(const NativeWorkDevice& work) {
        if (creator_ || work.failure || !work.device || !work.queue || !work.color) return false;
        NativeWorkDevice current;
        if (!current.Resolve(work.color.Get(),work.queue.Get()) || current.device != work.device) return false;
        auto previous=nextGeneration_.load(std::memory_order_relaxed);
        do { if (previous==UINT64_MAX) return false; }
        while (!nextGeneration_.compare_exchange_weak(previous,previous+1,std::memory_order_relaxed));
        generation_=previous+1;
        creator_ = current.device; queue_ = current.queue;
        char binding[320];
        std::snprintf(binding,sizeof(binding),"binding generation=%llu color=%p queue=%p creator=%p color.GetDevice=%08lX queue.GetDevice=%08lX queue.Create=not-issued-here; ",
            static_cast<unsigned long long>(generation_),static_cast<void*>(current.color.Get()),static_cast<void*>(current.queue.Get()),
            static_cast<void*>(Creator()),static_cast<unsigned long>(current.colorHr),static_cast<unsigned long>(current.queueHr));
        report_+=binding;
        return true;
    }
    bool Created(const char* kind, HRESULT createHr, ID3D12DeviceChild* object, bool forwarded, bool anchor = false) {
        Receipt r{kind,createHr,E_PENDING,object,{},{},forwarded};
        if (FAILED(createHr) || !object) { Describe(r,nullptr,FAILED(createHr)?"Create-failed":"Create-null"); return false; }
        r.object=object; // no COM call at all after failed/null Create
        r.deviceHr = object->GetDevice(IID_PPV_ARGS(&r.owner));
        if (FAILED(r.deviceHr) || !r.owner) { Describe(r,nullptr,"GetDevice-failed-or-null"); return false; }
        if (anchor) {
            if (!forwarded || publicOwner_) return false;
            publicOwner_ = r.owner;
        }
        auto* expected = forwarded ? PublicOwner() : Creator();
        const bool matches = expected && expected == r.owner.Get();
        Describe(r,expected,matches?"exact-owner":"owner-mismatch");
        if (!matches) return false;
        if (FAILED(expected->GetDeviceRemovedReason())) { report_ += "object-owner-removed; "; return false; }
        receipts_.push_back(std::move(r));
        return true;
    }
    bool Queue(ID3D12CommandQueue* queue, HRESULT& hr) const {
        if (!queue || queue != queue_.Get()) { hr=E_INVALIDARG; return false; }
        if (!NativeWorkDevice::Owns(Creator(),queue,hr)) return false;
        if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) { hr=E_INVALIDARG; return false; }
        hr=Creator()->GetDeviceRemovedReason();
        if (FAILED(hr)) return false;
        hr=PublicOwner()?PublicOwner()->GetDeviceRemovedReason():E_POINTER;
        return SUCCEEDED(hr);
    }
    bool RebindQueue(const NativeWorkDevice& work) {
        // Caller must first prove completion of the previous queue.
        if (work.failure || work.device.Get()!=Creator() || !work.queue) return false;
        queue_=work.queue; return true;
    }
    bool Object(ID3D12DeviceChild* object, bool forwarded, HRESULT& hr) const {
        if (!Known(object,forwarded)) { hr=E_INVALIDARG; return false; }
        return NativeWorkDevice::Owns(forwarded?PublicOwner():Creator(),object,hr);
    }
    bool PairCreated(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator, HRESULT& hr) {
        if (!Object(list,false,hr) || !Object(allocator,true,hr)) return false;
        if (list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT) { report_+="list-type-mismatch; "; hr=E_INVALIDARG; return false; }
        pairs_.push_back({list,allocator});
        char pair[160];
        std::snprintf(pair,sizeof(pair),"created-pair list=%p allocator=%p; ",static_cast<void*>(list),static_cast<void*>(allocator));
        report_+=pair;
        return true;
    }
    HRESULT Reset(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator) {
        HRESULT hr=S_OK;
        if (!Object(list,false,hr) || !Object(allocator,true,hr)) return hr;
        auto found=pairs_.end();
        for (auto it=pairs_.begin();it!=pairs_.end();++it) if (it->list==list) {found=it;break;}
        if (found==pairs_.end() || list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT) return E_INVALIDARG;
        hr=allocator->Reset(); if (FAILED(hr)) return hr;
        hr=list->Reset(allocator,nullptr);
        if (SUCCEEDED(hr)) found->allocator=allocator; // exact successful real Reset, not an alias admission
        return hr;
    }
    bool Commands(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list,
                  ID3D12CommandAllocator* allocator, ID3D12Fence* fence, HRESULT& hr) const {
        if (!Queue(queue,hr) || !Object(list,false,hr) || !Object(allocator,true,hr) || !Object(fence,true,hr)) return false;
        if (list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT) { hr=E_INVALIDARG; return false; }
        for (const auto& pair:pairs_) if (pair.list==list && pair.allocator==allocator) return true;
        hr=E_INVALIDARG; return false;
    }
};
} // namespace native
