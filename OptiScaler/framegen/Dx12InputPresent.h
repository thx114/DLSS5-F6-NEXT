// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "DlssgInputPresent.h"
#include "DlssgStateSample.h"
#include "Dx12InputFence.h"
#include <runtime/ComIdentity.h>
#include <runtime/D3D12DeviceIdentity.h>
#include <sl_dlss_g.h>
#include <dxgi1_6.h>

namespace framegen::input_present
{
class Completion;
struct Result;
template<class Present,class Current,class Query>
Result Invoke(Domain&,const Binding&,ID3D12Resource*,Present&&,Current&&,Query&&);

// SDK reader receipt only. Final colour writers remain separate dependencies.
// The receipt pins this exact buffer and SDK fence, never the swapchain itself.
class Completion final
{
    template<class Present,class Current,class Query>
    friend Result Invoke(Domain&,const Binding&,ID3D12Resource*,Present&&,Current&&,Query&&);
    Binding binding_ {};
    uint64_t serial_=0,query_=0;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer_;
    Dx12InputFence fence_;
  public:
    enum class Retirement {Invalid,Pending,Complete,DeviceLost};
    Completion() = default;
    Completion(const Completion&) = delete;
    Completion& operator=(const Completion&) = delete;
    Completion(Completion&& other) noexcept
        : binding_(other.binding_),serial_(std::exchange(other.serial_,0)),query_(std::exchange(other.query_,0)),
          buffer_(std::move(other.buffer_)),fence_(std::move(other.fence_)) {}
    Completion& operator=(Completion&& other) noexcept
    {
        if (this!=&other)
        {
            binding_=other.binding_;serial_=std::exchange(other.serial_,0);query_=std::exchange(other.query_,0);
            buffer_=std::move(other.buffer_);fence_=std::move(other.fence_);
        }
        return *this;
    }
    explicit operator bool() const noexcept {return serial_ && query_ && buffer_ && fence_;}
    const Binding& Owner() const noexcept {return binding_;}
    uint64_t PresentSerial() const noexcept {return serial_;}
    uint64_t QuerySequence() const noexcept {return query_;}
    ID3D12Resource* Buffer() const noexcept {return buffer_.Get();}
    ID3D12Fence* Fence() const noexcept {return fence_.Get();}
    uint64_t FenceValue() const noexcept {return fence_.Value();}
    // This orders the SDK reader only. The caller must also order the final
    // colour writer before granting a new write on this actual buffer.
    HRESULT OrderBeforeWrite(const Binding& current,ID3D12Resource* buffer,ID3D12CommandQueue* producer) const noexcept
    {
        if (!*this || !binding_.SameInput(current) || ComIdentity::Get(buffer)!=binding_.buffer || !producer)
            return E_INVALIDARG;
        Microsoft::WRL::ComPtr<ID3D12Device> queueDevice,fenceDevice;
        if (producer->GetDevice(IID_PPV_ARGS(&queueDevice))!=S_OK ||
            fence_.Get()->GetDevice(IID_PPV_ARGS(&fenceDevice))!=S_OK ||
            !D3D12DeviceIdentity::Same(queueDevice.Get(),fenceDevice.Get())) return E_INVALIDARG;
        return producer->Wait(fence_.Get(),fence_.Value());
    }
    Retirement Poll() const noexcept
    {
        if (!*this) return Retirement::Invalid;
        const auto completed=fence_.Get()->GetCompletedValue();
        if (completed==UINT64_MAX) return Retirement::DeviceLost;
        return completed>=fence_.Value()?Retirement::Complete:Retirement::Pending;
    }
};

struct Result
{
    HRESULT presentResult=DXGI_ERROR_WAS_STILL_DRAWING;
    HRESULT fenceStatus=E_PENDING;
    bool forwarded=false;
    Completion completion;
};

// Call under the provider's Present execution guard. The only query is after
// the real SDK Present has returned and before any SDK teardown is permitted.
// Query obtains its own COM reference inside the unified query gate.
template<class Present,class Current,class Query>
Result Invoke(Domain& domain,const Binding& binding,ID3D12Resource* buffer,
              Present&& present,Current&& current,Query&& query)
{
    Result result;
    const auto entryError=GetLastError();
    DWORD boundaryError=entryError;
    struct RestoreError {DWORD& error;~RestoreError(){SetLastError(error);}} restoreError {boundaryError};
    Microsoft::WRL::ComPtr<ID3D12Resource> heldBuffer=buffer;
    if (!heldBuffer || ComIdentity::Get(buffer)!=binding.buffer ||
        !(current()==binding)) return result;
    const auto attempt=domain.Begin(binding);
    if (!attempt) return result;
    struct Finish {Domain& domain;const Domain::Attempt& attempt;~Finish(){domain.Cancel(attempt);}} finish {domain,attempt};
    SetLastError(boundaryError); // Identity queries must not change the SDK API's entry state.
    try {result.presentResult=std::forward<Present>(present)();}
    catch (...) {boundaryError=GetLastError();throw;}
    boundaryError=GetLastError();result.forwarded=true;
    // Non-S_OK (including a successful DXGI status such as OCCLUDED) cannot
    // be relabelled as a new consumed input by querying a preceding target.
    if (result.presentResult!=S_OK || !domain.Current(attempt,current())) return result;
    Dx12InputFence fence;
    DlssgStateSample sample;
    bool querySucceeded=false;
    try
    {
        const auto queryResult=std::forward<Query>(query)(fence,sample);
        result.fenceStatus=fence.Status();
        Microsoft::WRL::ComPtr<ID3D12Device> inputDevice,fenceDevice;
        querySucceeded=queryResult==sl::Result::eOk && sample.succeeded && sample.inputCompletionFieldsObserved &&
            sample.api==binding.api && sample.viewport==binding.viewport &&
            sample.reason==DlssgStateQueryReason::CompletionFence &&
            sample.requestVersion>=3 && sample.requestVersion<=4 &&
            sample.stateVersion>=3 && sample.stateVersion<=4 && fence &&
            fence.Value()!=0 && sample.inputCompletionFenceValue==fence.Value() &&
            heldBuffer->GetDevice(IID_PPV_ARGS(&inputDevice))==S_OK &&
            fence.Get()->GetDevice(IID_PPV_ARGS(&fenceDevice))==S_OK &&
            D3D12DeviceIdentity::Same(inputDevice.Get(),fenceDevice.Get());
    }
    catch (...) {querySucceeded=false;} // Preserve Present, but never publish a partially validated receipt.
    if (domain.Complete(attempt,current(),true,querySucceeded,sample.querySequence,sample.queryThreadId))
    {
        result.completion.binding_=binding;result.completion.serial_=attempt.serial;
        result.completion.query_=sample.querySequence;result.completion.buffer_=std::move(heldBuffer);
        result.completion.fence_=std::move(fence);
    }
    return result;
}
}
