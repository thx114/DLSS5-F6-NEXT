#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>

namespace dx11 {
// Full API state isolation. This has no GPU lifetime/completion meaning.
class ContextStateGuard final {
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> original_;
public:
    ContextStateGuard(ID3D11DeviceContext1* context,ID3DDeviceContextState* work):context_(context) {
        context_->SwapDeviceContextState(work,&original_);
    }
    ~ContextStateGuard(){context_->ClearState();context_->SwapDeviceContextState(original_.Get(),nullptr);}
    ContextStateGuard(const ContextStateGuard&)=delete;
    ContextStateGuard& operator=(const ContextStateGuard&)=delete;
};
inline HRESULT CreateContextState(ID3D11Device* device,ID3DDeviceContextState** state) noexcept {
    Microsoft::WRL::ComPtr<ID3D11Device1> device1;
    const auto hr=device->QueryInterface(IID_PPV_ARGS(&device1));if(FAILED(hr))return hr;
    const D3D_FEATURE_LEVEL level=device->GetFeatureLevel();D3D_FEATURE_LEVEL chosen{};
    return device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&chosen,state);
}
} // namespace dx11
