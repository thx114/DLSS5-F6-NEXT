// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <utility>

class StreamlineHooks;

namespace framegen
{
// Owns only the fence returned by one real SDK query. Present, resource and
// epoch association belong to the caller; this object alone cannot retire an
// input. Inert DlssgStateSample addresses must never be converted into one.
class Dx12InputFence final
{
    friend class ::StreamlineHooks;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    uint64_t value_ = 0;
    HRESULT status_ = E_PENDING;

    void CaptureBorrowed(void* borrowed, uint64_t value) noexcept
    {
        Reset();
        if (!borrowed) { status_ = E_POINTER; return; }
        if (value == UINT64_MAX) { status_ = E_INVALIDARG; return; }
        // QueryInterface acquires OUR reference while the SDK query gate is
        // still held. The SDK's returned pointer is borrowed, never Attach'd.
        auto* object = static_cast<IUnknown*>(borrowed);
        status_ = object->QueryInterface(IID_PPV_ARGS(&fence_));
        if (status_ != S_OK || !fence_)
        {
            fence_.Reset();
            if (SUCCEEDED(status_)) status_ = E_UNEXPECTED;
            return;
        }
        value_ = value;
    }
  public:
    Dx12InputFence() = default;
    Dx12InputFence(const Dx12InputFence&) = delete;
    Dx12InputFence& operator=(const Dx12InputFence&) = delete;
    Dx12InputFence(Dx12InputFence&& other) noexcept
        : fence_(std::move(other.fence_)), value_(std::exchange(other.value_,0)),
          status_(std::exchange(other.status_,E_PENDING)) {}
    Dx12InputFence& operator=(Dx12InputFence&& other) noexcept
    {
        if (this != &other)
        {
            fence_ = std::move(other.fence_);
            value_ = std::exchange(other.value_,0);
            status_ = std::exchange(other.status_,E_PENDING);
        }
        return *this;
    }
    void Reset() noexcept { fence_.Reset(); value_=0; status_=E_PENDING; }
    explicit operator bool() const noexcept { return status_==S_OK && fence_!=nullptr; }
    ID3D12Fence* Get() const noexcept { return fence_.Get(); }
    uint64_t Value() const noexcept { return value_; }
    HRESULT Status() const noexcept { return status_; }
};
}
