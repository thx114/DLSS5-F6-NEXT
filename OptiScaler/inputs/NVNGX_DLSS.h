#pragma once

#include <memory>
#include <utility>

struct ID3D12Device;
struct ID3D11Device;
struct NVSDK_NGX_Handle;
namespace interop { struct BeforeSrInput; }
namespace neo::ngx { struct F6EvalReceiptV1; }
namespace NgxInput
{
// Attach only the real command-list device. This does not reconstruct or call
// NGX Init, and does not invent application/project/SDK metadata.
bool CanAttachNativeDx12Input(ID3D12Device* device);
bool AttachNativeDx12Input(ID3D12Device* device);
bool DetachNativeDx12Input();
// Internal TAA source: attach a real device without fabricating an NGX Init
// call or replacing the game's app metadata. Release bypasses external ASI
// feature caches, after the caller's D3D11 tail and the provider's own fences.
bool AttachTaaDx11Input(ID3D11Device* device);
bool HasTaaDx11Context(unsigned int id);
bool ReleaseTaaDx11Feature(unsigned int id);
void DetachTaaDx11Input(ID3D11Device* device);
// Internal synchronous ingress: actual inputs do not depend on an external bridge.
NVSDK_NGX_Result EvaluateDx11Input(ID3D11DeviceContext* context, const NVSDK_NGX_Handle* handle,
    NVSDK_NGX_Parameter* parameters, uint64_t requestId, neo::ngx::F6EvalReceiptV1* receipt,
    const interop::BeforeSrInput& input);
}

template <typename FeatureType> struct ContextData
{
    using ParameterOwner = std::unique_ptr<NVSDK_NGX_Parameter, void (*)(NVSDK_NGX_Parameter*)>;
    // Only a table allocated for this transition is owned here. A caller's custom
    // OptiScaler table is still borrowed, regardless of its allocation tag.
    ParameterOwner createParamsOwner { nullptr, nullptr };
    std::unique_ptr<FeatureType> feature;
    NVSDK_NGX_Parameter* createParams = nullptr;
    int changeBackendCounter = 0;

    ContextData() = default;
    ContextData(const ContextData&) = delete;
    ContextData& operator=(const ContextData&) = delete;
    ContextData(ContextData&& other) noexcept { *this = std::move(other); }
    ContextData& operator=(ContextData&& other) noexcept
    {
        if (this != &other)
        {
            // Dense-map erase/assignment can replace a live entry. Its feature
            // must be destroyed before the parameters used during initialization.
            feature.reset();
            ClearCreateParams();
            createParamsOwner = std::move(other.createParamsOwner);
            feature = std::move(other.feature);
            createParams = std::exchange(other.createParams, nullptr);
            changeBackendCounter = std::exchange(other.changeBackendCounter, 0);
        }
        return *this;
    }

    void ClearCreateParams()
    {
        createParams = nullptr;
        createParamsOwner.reset();
    }

    void BorrowCreateParams(NVSDK_NGX_Parameter* parameters)
    {
        ClearCreateParams();
        createParams = parameters;
    }

    void OwnCreateParams(ParameterOwner parameters)
    {
        ClearCreateParams();
        createParamsOwner = std::move(parameters);
        createParams = createParamsOwner.get();
    }
};
