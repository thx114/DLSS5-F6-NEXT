#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>

// Getters return owned references. Keep them across interop work and restore
// bindings before releasing them, including when the bridge leaves early.
class Dx11BridgeStateGuard
{
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    ID3D11ShaderResourceView* srvs_[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
    ID3D11SamplerState* samplers_[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT]{};
    ID3D11Buffer* buffers_[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT]{};
    ID3D11UnorderedAccessView* uavs_[D3D11_1_UAV_SLOT_COUNT]{};
    ID3D11RenderTargetView* targets_[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* depth_ = nullptr;

    template<class T, unsigned N> static void Release(T* (&objects)[N]) noexcept
    {
        for (auto* object : objects) if (object) object->Release();
    }

public:
    explicit Dx11BridgeStateGuard(ID3D11DeviceContext* context) : context_(context)
    {
        context_->CSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srvs_);
        context_->CSGetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers_);
        context_->CSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, buffers_);
        context_->CSGetUnorderedAccessViews(0, D3D11_1_UAV_SLOT_COUNT, uavs_);
        context_->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets_, &depth_);
        ID3D11RenderTargetView* empty[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        context_->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, empty, nullptr);
    }
    Dx11BridgeStateGuard(const Dx11BridgeStateGuard&) = delete;
    Dx11BridgeStateGuard& operator=(const Dx11BridgeStateGuard&) = delete;
    ~Dx11BridgeStateGuard()
    {
        // Keep the existing bridge restoration order. Null initial counts preserve
        // hidden append/consume UAV counters instead of resetting them.
        context_->CSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srvs_);
        context_->CSSetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers_);
        context_->CSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, buffers_);
        context_->CSSetUnorderedAccessViews(0, D3D11_1_UAV_SLOT_COUNT, uavs_, nullptr);
        context_->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets_, depth_);
        Release(srvs_); Release(samplers_); Release(buffers_); Release(uavs_); Release(targets_);
        if (depth_) depth_->Release();
    }
};
