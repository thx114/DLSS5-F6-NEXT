#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <utility>

namespace HoYoShade::OwnedRuntime
{
// A receipt for ONE actual typed Create return, not a device alias/domain.
// NativeCreationDomain still owns producer allocator/list/submission authority.
// SDK get_native/resource handles cannot construct this receipt.
template<class Object> class TypedCreationOwner final
{
    Microsoft::WRL::ComPtr<ID3D12Device> creator_, returnedOwner_;
    Microsoft::WRL::ComPtr<Object> object_;
    HRESULT createHr_ = E_PENDING, ownerHr_ = E_PENDING;
    bool attempted_ = false;
public:
    TypedCreationOwner() = default;
    TypedCreationOwner(const TypedCreationOwner&) = delete;
    TypedCreationOwner& operator=(const TypedCreationOwner&) = delete;
    TypedCreationOwner(TypedCreationOwner&&) = default;
    TypedCreationOwner& operator=(TypedCreationOwner&&) = default;

    template<class Invocation> HRESULT Create(ID3D12Device* creator, Invocation&& invocation)
    {
        if (attempted_ || !creator) return E_UNEXPECTED;
        attempted_ = true;
        creator_ = creator;
        Object* returned = nullptr;
        createHr_ = std::forward<Invocation>(invocation)(&returned);
        // A failed Create may have left an unusable scalar. Never call COM
        // (including Release) through it, even if it is non-null.
        if (FAILED(createHr_) || !returned) return createHr_;
        object_.Attach(returned);
        ID3D12Device* owner = nullptr;
        ownerHr_ = object_->GetDevice(IID_PPV_ARGS(&owner));
        if (SUCCEEDED(ownerHr_) && owner) returnedOwner_.Attach(owner);
        return createHr_;
    }
    Object* Get() const noexcept { return object_.Get(); }
    HRESULT OwnerHr() const noexcept { return ownerHr_; }
    bool Current(ID3D12Device* creator, Object* object, HRESULT& hr) const
    {
        hr = S_OK;
        if (!attempted_ || FAILED(createHr_) || FAILED(ownerHr_) || !creator_ ||
            !returnedOwner_ || !object_ || creator != creator_.Get() || object != object_.Get())
        { if (FAILED(ownerHr_) && ownerHr_ != E_PENDING) hr = ownerHr_; return false; }
        Microsoft::WRL::ComPtr<ID3D12Device> current;
        hr = object_->GetDevice(IID_PPV_ARGS(&current));
        if (FAILED(hr)) return false;
        if (!current || current.Get() != returnedOwner_.Get()) return false;
        hr = creator_->GetDeviceRemovedReason();
        if (FAILED(hr)) return false;
        hr = returnedOwner_->GetDeviceRemovedReason();
        return SUCCEEDED(hr);
    }
};
}
