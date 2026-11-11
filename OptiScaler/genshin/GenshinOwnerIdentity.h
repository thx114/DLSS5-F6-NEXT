// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinOutputContract.h"
#include <d3d11.h>
#include <wrl/client.h>

namespace GenshinGuides::OwnerIdentity {
// Guide/native receipts retain typed interface addresses. Present picture
// receipts use COM's canonical IUnknown identity. Never reinterpret a stored
// integer as an interface or equate these two representations by address.
class Context final {
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<IUnknown> contextIdentity_,deviceIdentity_;
public:
    explicit Context(ID3D11DeviceContext* context)noexcept {
        if(!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
        context_=context;context_->GetDevice(&device_);
        if(!device_ || FAILED(context_.As(&contextIdentity_)) || FAILED(device_.As(&deviceIdentity_))){
            contextIdentity_.Reset();deviceIdentity_.Reset();
        }
    }
    bool Known()const noexcept {return context_ && device_ && contextIdentity_ && deviceIdentity_;}
    bool ToCanonical(const Owner& interfaces,Owner& canonical)const noexcept {
        canonical={};
        if(!Known() || interfaces.thread!=GetCurrentThreadId() ||
           interfaces.context!=reinterpret_cast<uint64_t>(context_.Get()) ||
           interfaces.device!=reinterpret_cast<uint64_t>(device_.Get()))return false;
        canonical=interfaces;canonical.context=reinterpret_cast<uint64_t>(contextIdentity_.Get());
        canonical.device=reinterpret_cast<uint64_t>(deviceIdentity_.Get());return true;
    }
    bool ToInterfaces(const Owner& canonical,Owner& interfaces)const noexcept {
        interfaces={};
        if(!Known() || canonical.thread!=GetCurrentThreadId() ||
           canonical.context!=reinterpret_cast<uint64_t>(contextIdentity_.Get()) ||
           canonical.device!=reinterpret_cast<uint64_t>(deviceIdentity_.Get()))return false;
        interfaces=canonical;interfaces.context=reinterpret_cast<uint64_t>(context_.Get());
        interfaces.device=reinterpret_cast<uint64_t>(device_.Get());return true;
    }
};
}
