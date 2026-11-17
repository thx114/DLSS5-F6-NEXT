#include "pch.h"
#include "D3D11_Hooks.h"
#include "Dxgi_Hooks.h"
#include "D3D11On12FactoryRoutes.h"
#include "SwapchainCreateRule.h"

#include <Util.h>
#include <Config.h>

#include <proxies/KernelBase_Proxy.h>

#include <resource_tracking/GenericDepth_Dx11.h>
#include <resource_tracking/DrawObservation_Dx11.h>
#include <mods/gimi/GimiRuntime.h>
#include <native/NativeLowLatency.h>
#include <inputs/taa/TaaInputDx11.h>
#include <inputs/taa/TaaFullscreenDx11.h>

#include <wrapped/wrapped_swapchain.h>

#include <detours/detours.h>

#include <d3d11_4.h>
#include <mutex>
#include <type_traits>
#include <d3d11on12.h>
#include <dxgi1_6.h>

#include "Hook_Utils.h"

#pragma intrinsic(_ReturnAddress)

// Factory recursion is local to the calling thread and restores nested state.
static std::recursive_mutex hookLifecycle;
static thread_local bool _skipDx11Create = false;
struct FactoryRecursionScope {
    bool previous = _skipDx11Create;
    FactoryRecursionScope() { _skipDx11Create = true; }
    ~FactoryRecursionScope() { _skipDx11Create = previous; }
};
#include <hooks/HookOwnership.h>
#include <hooks/MethodHooks.h>
#include <hooks/RoutedHook.h>
#include <hooks/HookTransaction.h>
// DirectX
using PFN_CreateSamplerState = rewrite_signature<decltype(&ID3D11Device::CreateSamplerState)>::type;

using PFN_Release = rewrite_signature<decltype(&IUnknown::Release)>::type;

static HRESULT hkD3D11CreateDevice(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
                                   const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
                                   ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel,
                                   ID3D11DeviceContext** ppImmediateContext);
static HRESULT hkD3D11CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
                                               UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
                                               UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                               IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
                                               D3D_FEATURE_LEVEL* pFeatureLevel,
                                               ID3D11DeviceContext** ppImmediateContext);
static HRESULT hkD3D11On12CreateDevice(IUnknown* pDevice, UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels,
                                       UINT FeatureLevels, IUnknown* const* ppCommandQueues, UINT NumQueues,
                                       UINT NodeMask, ID3D11Device** ppDevice, ID3D11DeviceContext** ppImmediateContext,
                                       D3D_FEATURE_LEVEL* pChosenFeatureLevel);
static void HookToDeviceLocal(ID3D11Device* device);
struct D3D11FactoryDomain {
    template<class... Args> static void Returned(HRESULT result,Args... args) {
        if ((!gimi::runtime::Enabled() && !taa::NeedsHooks()) || FAILED(result)) return;
        const auto arguments=std::tuple<Args...>(args...);
        constexpr size_t deviceIndex=sizeof...(Args)==12 ? 9 : 7;
        auto** device=std::get<deviceIndex>(arguments);
        if (device && *device) HookToDeviceLocal(*device);
    }
};
static HookOwnership::RoutedHook<&hkD3D11CreateDevice, SIZE_MAX, D3D11FactoryDomain> o_D3D11CreateDevice;
static HookOwnership::RoutedHook<&hkD3D11CreateDeviceAndSwapChain, SIZE_MAX, D3D11FactoryDomain> o_D3D11CreateDeviceAndSwapChain;
using D3D11On12FactoryDomain = D3D11On12FactoryBoundary::Domain<D3D11FactoryDomain>;
static HookOwnership::RoutedHook<&hkD3D11On12CreateDevice, SIZE_MAX, D3D11On12FactoryDomain> o_D3D11On12CreateDevice;
using PFN_CreatePixelShader = rewrite_signature<decltype(&ID3D11Device::CreatePixelShader)>::type;
using PFN_CreateVertexShader = rewrite_signature<decltype(&ID3D11Device::CreateVertexShader)>::type;
using PFN_CreateComputeShader = rewrite_signature<decltype(&ID3D11Device::CreateComputeShader)>::type;
static HookOwnership::MethodHooks<PFN_CreatePixelShader> pixelHooks;
static HookOwnership::MethodHooks<PFN_CreateVertexShader> vertexHooks;
static HookOwnership::MethodHooks<PFN_CreateComputeShader> computeHooks;
static PFN_CreateSamplerState o_CreateSamplerState = nullptr;
static PFN_Release o_D3D11DeviceRelease = nullptr;
static HookOwnership::Own::Slot ownDeviceRelease,ownDeviceSampler;
using PFN_ModCreateBuffer = rewrite_signature<decltype(&ID3D11Device::CreateBuffer)>::type;
using PFN_ModCreateTexture = rewrite_signature<decltype(&ID3D11Device::CreateTexture2D)>::type;
using PFN_ModCreateView = rewrite_signature<decltype(&ID3D11Device::CreateShaderResourceView)>::type;
using PFN_ModCreateLayout = rewrite_signature<decltype(&ID3D11Device::CreateInputLayout)>::type;
static PFN_ModCreateBuffer o_ModCreateBuffer=nullptr;
static PFN_ModCreateTexture o_ModCreateTexture=nullptr;
static PFN_ModCreateView o_ModCreateView=nullptr;
static PFN_ModCreateLayout o_ModCreateLayout=nullptr;
using PFN_ModCreateUnordered = rewrite_signature<decltype(&ID3D11Device::CreateUnorderedAccessView)>::type;
using PFN_ModCreateGeometry = rewrite_signature<decltype(&ID3D11Device::CreateGeometryShader)>::type;
using PFN_ModCreateGeometrySO = rewrite_signature<decltype(&ID3D11Device::CreateGeometryShaderWithStreamOutput)>::type;
static PFN_ModCreateUnordered o_ModCreateUnordered=nullptr;
static PFN_ModCreateGeometry o_ModCreateGeometry=nullptr;
static PFN_ModCreateGeometrySO o_ModCreateGeometrySO=nullptr;
static HookOwnership::Own::Slot ownModUnordered,ownModGeometry,ownModGeometrySO;
static HookOwnership::Own::Slot ownModBuffer,ownModTexture,ownModView,ownModLayout;
static std::array<PVOID,7> modCreationTargets{};
static std::atomic_bool modCreationsCommitted{false};
static bool ModCreationEnabled(ID3D11Device* device)noexcept
{
    if(!gimi::runtime::Enabled() || GenericDepthDx11::ModInternal())return false;
    // Creation-time only; never weaken the exact target coverage published at
    // installation to a transaction-committed flag on the next creation call.
    const auto table=*reinterpret_cast<PVOID**>(device);
    const bool complete=modCreationsCommitted.load(std::memory_order_acquire) &&
        modCreationTargets==std::array<PVOID,7>{table[3],table[5],table[7],table[8],table[11],table[13],table[14]} &&
        pixelHooks.Covers(table[15]) && vertexHooks.Covers(table[12]) &&
        ownModBuffer.live && ownModBuffer.entry==table[3] && ownModBuffer.transaction==(PVOID)o_ModCreateBuffer &&
        ownModTexture.live && ownModTexture.entry==table[5] && ownModTexture.transaction==(PVOID)o_ModCreateTexture &&
        ownModView.live && ownModView.entry==table[7] && ownModView.transaction==(PVOID)o_ModCreateView &&
        ownModUnordered.live && ownModUnordered.entry==table[8] && ownModUnordered.transaction==(PVOID)o_ModCreateUnordered &&
        ownModLayout.live && ownModLayout.entry==table[11] && ownModLayout.transaction==(PVOID)o_ModCreateLayout &&
        ownModGeometry.live && ownModGeometry.entry==table[13] && ownModGeometry.transaction==(PVOID)o_ModCreateGeometry &&
        ownModGeometrySO.live && ownModGeometrySO.entry==table[14] && ownModGeometrySO.transaction==(PVOID)o_ModCreateGeometrySO;
    gimi::runtime::CreationCoverage(device,complete);
    return complete;
}
static void RememberTaaGeometry(ID3D11Device* device,ID3D11Buffer* buffer,
    const D3D11_BUFFER_DESC* desc,const D3D11_SUBRESOURCE_DATA* data)
{
    if(!buffer || !desc || GenericDepthDx11::ModInternal() ||
       !(desc->BindFlags&(D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_INDEX_BUFFER)) || desc->ByteWidth>4096)return;
    // Swapchain creation can change the immediate context implementation after
    // device initialization. Cover its writes before admitting these new bytes.
    if(!GenericDepthDx11::EnsureTaaWriteCoverage(device))return;
    taa::constants::Observe(buffer);taa::constants::Initialized(buffer,data);
}
static HRESULT hkModCreateBuffer(ID3D11Device* device,const D3D11_BUFFER_DESC* desc,
    const D3D11_SUBRESOURCE_DATA* data,ID3D11Buffer** out)
{
    if(!gimi::runtime::Enabled() || GenericDepthDx11::ModInternal()){
        const auto hr=o_ModCreateBuffer(device,desc,data,out);
        if(SUCCEEDED(hr)&&out&&*out)RememberTaaGeometry(device,*out,desc,data);
        return hr;
    }
    const bool mod=out && ModCreationEnabled(device);
    gimi::BufferCreationEdit edit;
    if(mod)gimi::runtime::PrepareBufferCreation(device,desc,data,edit);
    const auto* actualDesc=edit.changed ? &edit.effective : desc;
    const auto* actualData=edit.changed && data ? &edit.initial : data;
    const auto hr=o_ModCreateBuffer(device,actualDesc,actualData,out);
    if(SUCCEEDED(hr)&&out&&*out)RememberTaaGeometry(device,*out,actualDesc,actualData);
    if(SUCCEEDED(hr) && out && *out && mod)
        gimi::runtime::BufferCreated(device,*out,actualDesc,actualData,edit.qualified ? &edit : nullptr);
    return hr;
}
static HRESULT hkModCreateTexture(ID3D11Device* device,const D3D11_TEXTURE2D_DESC* desc,
    const D3D11_SUBRESOURCE_DATA* data,ID3D11Texture2D** out)
{
    const auto hr=o_ModCreateTexture(device,desc,data,out);
    if(SUCCEEDED(hr) && out && *out && ModCreationEnabled(device))gimi::runtime::TextureCreated(device,*out,desc,data);
    return hr;
}
static HRESULT hkModCreateView(ID3D11Device* device,ID3D11Resource* resource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,ID3D11ShaderResourceView** out)
{
    const auto hr=o_ModCreateView(device,resource,desc,out);
    if(SUCCEEDED(hr) && out && *out && ModCreationEnabled(device))gimi::runtime::ViewCreated(device,resource,*out);
    return hr;
}
static HRESULT hkModCreateLayout(ID3D11Device* device,const D3D11_INPUT_ELEMENT_DESC* elements,UINT count,
    const void* bytes,SIZE_T size,ID3D11InputLayout** out)
{
    const auto hr=o_ModCreateLayout(device,elements,count,bytes,size,out);
    if(SUCCEEDED(hr)&&out&&*out&&!GenericDepthDx11::ModInternal())taa::fullscreen::Remember(*out,elements,count);
    if(SUCCEEDED(hr) && out && *out && ModCreationEnabled(device))gimi::runtime::LayoutCreated(device,*out,elements,count);
    return hr;
}
static HRESULT hkModCreateUnordered(ID3D11Device* device,ID3D11Resource* resource,
    const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc,ID3D11UnorderedAccessView** out)
{
    const bool mod=out && ModCreationEnabled(device);
    D3D11_UNORDERED_ACCESS_VIEW_DESC effective{};bool changed=false;
    if(mod)gimi::runtime::PrepareUnorderedView(device,resource,desc,effective,changed);
    const auto hr=o_ModCreateUnordered(device,resource,changed ? &effective : desc,out);
    if(SUCCEEDED(hr) && out && *out && mod)gimi::runtime::UnorderedViewCreated(device,resource,*out);
    return hr;
}
static HRESULT hkModCreateGeometry(ID3D11Device* device,const void* bytes,SIZE_T size,
    ID3D11ClassLinkage* linkage,ID3D11GeometryShader** out)
{
    const auto hr=o_ModCreateGeometry(device,bytes,size,linkage,out);
    if(SUCCEEDED(hr) && out && *out && ModCreationEnabled(device))
        gimi::runtime::GeometryShaderCreated(device,*out,bytes,size,linkage,nullptr,0,nullptr,0,0,false);
    return hr;
}
static HRESULT hkModCreateGeometrySO(ID3D11Device* device,const void* bytes,SIZE_T size,
    const D3D11_SO_DECLARATION_ENTRY* entries,UINT count,const UINT* strides,UINT strideCount,
    UINT rasterized,ID3D11ClassLinkage* linkage,ID3D11GeometryShader** out)
{
    const auto hr=o_ModCreateGeometrySO(device,bytes,size,entries,count,strides,strideCount,rasterized,linkage,out);
    if(SUCCEEDED(hr) && out && *out && ModCreationEnabled(device))
        gimi::runtime::GeometryShaderCreated(device,*out,bytes,size,linkage,entries,count,strides,strideCount,rasterized,true);
    return hr;
}
static LONG EnsureShaderHooks(ID3D11Device* device) noexcept;
LONG D3D11Hooks::InstallGimiCreationHooks(ID3D11Device* device) noexcept
{
    std::lock_guard lifecycle(hookLifecycle);
    if(!gimi::runtime::Enabled() || !device || GenericDepthDx11::ModInternal())return ERROR_INVALID_OPERATION;
    const auto table=*reinterpret_cast<PVOID**>(device);
    const auto shaderResult=EnsureShaderHooks(device);
    if(shaderResult!=NO_ERROR)return shaderResult;
    if(!pixelHooks.Covers(table[15]) || !vertexHooks.Covers(table[12]))return ERROR_NOT_SUPPORTED;
    const std::array<PVOID,7> targets{table[3],table[5],table[7],table[8],table[11],table[13],table[14]};
    HookOwnership::Own::Batch hooks;
    HookOwnership::AddMethod(hooks,ownModBuffer,&(PVOID&)o_ModCreateBuffer,table,3,(PVOID)hkModCreateBuffer);
    HookOwnership::AddMethod(hooks,ownModTexture,&(PVOID&)o_ModCreateTexture,table,5,(PVOID)hkModCreateTexture);
    HookOwnership::AddMethod(hooks,ownModView,&(PVOID&)o_ModCreateView,table,7,(PVOID)hkModCreateView);
    HookOwnership::AddMethod(hooks,ownModLayout,&(PVOID&)o_ModCreateLayout,table,11,(PVOID)hkModCreateLayout);
    HookOwnership::AddMethod(hooks,ownModUnordered,&(PVOID&)o_ModCreateUnordered,table,8,(PVOID)hkModCreateUnordered);
    HookOwnership::AddMethod(hooks,ownModGeometry,&(PVOID&)o_ModCreateGeometry,table,13,(PVOID)hkModCreateGeometry);
    HookOwnership::AddMethod(hooks,ownModGeometrySO,&(PVOID&)o_ModCreateGeometrySO,table,14,(PVOID)hkModCreateGeometrySO);
    const auto result=hooks.Commit();
    if(result!=NO_ERROR)return result;
    modCreationTargets=targets;modCreationsCommitted.store(true,std::memory_order_release);
    return NO_ERROR;
}
static HRESULT hkCreateSamplerState(ID3D11Device* This, const D3D11_SAMPLER_DESC* pSamplerDesc,
                                    ID3D11SamplerState** ppSamplerState);

static thread_local unsigned pixelCreationDepth = 0, vertexCreationDepth = 0, computeCreationDepth = 0;
struct ShaderCreationScope {
    unsigned& depth;
    bool outer;
    explicit ShaderCreationScope(unsigned& value) : depth(value), outer(value++ == 0) {}
    ~ShaderCreationScope() { --depth; }
};
template<size_t Index> static HRESULT hkCreatePixelShader(ID3D11Device* device,const void* bytes,SIZE_T size,
                                  ID3D11ClassLinkage* linkage,ID3D11PixelShader** shader)
{
    ShaderCreationScope scope(pixelCreationDepth);
    auto call = pixelHooks.Acquire(Index);
    const auto result=call.Get()(device,bytes,size,linkage,shader);
    if(SUCCEEDED(result) && shader && *shader){
        if(scope.outer) { DrawObservationDx11::RememberShader(*shader,DrawObservationDx11::ShaderByteHash(bytes,size),size);
            taa::RememberShader(*shader,bytes,size); }
        if(ModCreationEnabled(device))gimi::runtime::ShaderCreated(device,*shader,bytes,size,false,linkage!=nullptr);}
    return result;
}

template<size_t Index> static HRESULT hkVisibleCreateVertexShader(ID3D11Device* device,const void* bytes,SIZE_T size,
                                           ID3D11ClassLinkage* linkage,ID3D11VertexShader** shader)
{
    ShaderCreationScope scope(vertexCreationDepth);
    auto call = vertexHooks.Acquire(Index);
    const auto result=call.Get()(device,bytes,size,linkage,shader);
    if(SUCCEEDED(result) && shader && *shader){
        if(scope.outer) { DrawObservationDx11::RememberShader(*shader,DrawObservationDx11::ShaderByteHash(bytes,size),size);
            taa::RememberShader(*shader,bytes,size); }
        if(ModCreationEnabled(device))gimi::runtime::ShaderCreated(device,*shader,bytes,size,true,linkage!=nullptr);}
    return result;
}

template<size_t Index> static HRESULT hkVisibleCreateComputeShader(ID3D11Device* device,const void* bytes,SIZE_T size,
                                           ID3D11ClassLinkage* linkage,ID3D11ComputeShader** shader)
{
    ShaderCreationScope scope(computeCreationDepth);
    auto call=computeHooks.Acquire(Index);
    const auto result=call.Get()(device,bytes,size,linkage,shader);
    if (SUCCEEDED(result) && shader && *shader && scope.outer) {
        DrawObservationDx11::RememberShader(*shader,DrawObservationDx11::ShaderByteHash(bytes,size),size);
        taa::RememberShader(*shader,bytes,size);
    }
    return result;
}

static LONG EnsureShaderHooks(ID3D11Device* device) noexcept
{
    std::lock_guard lifecycle(hookLifecycle);
    if (!device) return ERROR_INVALID_PARAMETER;
    auto table = *reinterpret_cast<PVOID**>(device);
    static const auto pixelThunks = []<size_t... I>(std::index_sequence<I...>) {
        return std::array<PFN_CreatePixelShader, sizeof...(I)>{&hkCreatePixelShader<I>...};
    }(std::make_index_sequence<decltype(pixelHooks)::capacity>{});
    static const auto vertexThunks = []<size_t... I>(std::index_sequence<I...>) {
        return std::array<PFN_CreateVertexShader, sizeof...(I)>{&hkVisibleCreateVertexShader<I>...};
    }(std::make_index_sequence<decltype(vertexHooks)::capacity>{});
    static const auto computeThunks = []<size_t... I>(std::index_sequence<I...>) {
        return std::array<PFN_CreateComputeShader, sizeof...(I)>{&hkVisibleCreateComputeShader<I>...};
    }(std::make_index_sequence<decltype(computeHooks)::capacity>{});
    HookOwnership::Own::Batch hooks;
    const auto pixel = pixelHooks.Add(hooks, table[15], pixelThunks);
    const auto vertex = pixel == NO_ERROR ? vertexHooks.Add(hooks, table[12], vertexThunks) : pixel;
    if (vertex != NO_ERROR) return vertex;
    const auto compute=computeHooks.Add(hooks,table[18],computeThunks);
    if (compute!=NO_ERROR) return compute;
    HookOwnership::AddMethod(hooks,ownModBuffer,&(PVOID&)o_ModCreateBuffer,table,3,(PVOID)hkModCreateBuffer);
    HookOwnership::AddMethod(hooks,ownModLayout,&(PVOID&)o_ModCreateLayout,table,11,(PVOID)hkModCreateLayout);
    const auto result=hooks.Commit();
    if (result != NO_ERROR) LOG_WARN("D3D11 shader hook transaction refused hr=0x{:X} device={} pixel={} vertex={}",
        result, static_cast<void*>(device), table[15], table[12]);
    return result;
}
static ULONG hkD3D11DeviceRelease(IUnknown* device)
{
    if (State::Instance().currentD3D11Device == device)
    {
        device->AddRef();
        auto refCount = o_D3D11DeviceRelease(device);

        if (refCount == 1)
        {
            LOG_DEBUG("Set State::Instance().currentD3D11Device = nullptr, was: {:X}", (size_t) device);
            State::Instance().currentD3D11Device = nullptr;
            native::lowlatency::OnDeviceReleased(device);
        }
    }

    auto result = o_D3D11DeviceRelease(device);
    return result;
}

// The ini setting wins; unset, the game's quirk decides.
static bool ElevateD3D11FeatureLevel()
{
    if (Config::Instance()->D3D11FeatureLevelElevation.has_value())
        return Config::Instance()->D3D11FeatureLevelElevation.value();

    return !(State::Instance().gameQuirks & GameQuirk::SkipD3D11FeatureLevelElevation);
}

static inline D3D11_FILTER UpgradeToAF(D3D11_FILTER f)
{
    if (Config::Instance()->AnisotropySkipPointFilter.value_or_default() &&
        (f == D3D11_FILTER_MIN_MAG_MIP_POINT || f == D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT ||
         f == D3D11_FILTER_MINIMUM_MIN_MAG_MIP_POINT || f == D3D11_FILTER_MAXIMUM_MIN_MAG_MIP_POINT ||
         f == D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT || f == D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT ||
         f == D3D11_FILTER_MINIMUM_MIN_MAG_LINEAR_MIP_POINT || f == D3D11_FILTER_MAXIMUM_MIN_MAG_LINEAR_MIP_POINT ||
         f == D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT ||
         f == D3D11_FILTER_COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT ||
         f == D3D11_FILTER_MINIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT ||
         f == D3D11_FILTER_MAXIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT))
    {
        return f;
    }

    if (f >= D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT && f <= D3D11_FILTER_COMPARISON_ANISOTROPIC)
    {
        return Config::Instance()->AnisotropyModifyComp.value_or_default() ? D3D11_FILTER_COMPARISON_ANISOTROPIC : f;
    }

    if (f >= D3D11_FILTER_MINIMUM_MIN_MAG_MIP_POINT && f <= D3D11_FILTER_MINIMUM_ANISOTROPIC)
    {
        return Config::Instance()->AnisotropyModifyMinMax.value_or_default() ? D3D11_FILTER_MINIMUM_ANISOTROPIC : f;
    }

    if (f >= D3D11_FILTER_MAXIMUM_MIN_MAG_MIP_POINT && f <= D3D11_FILTER_MAXIMUM_ANISOTROPIC)
    {
        return Config::Instance()->AnisotropyModifyMinMax.value_or_default() ? D3D11_FILTER_MAXIMUM_ANISOTROPIC : f;
    }

    return D3D11_FILTER_ANISOTROPIC;
}

static void HookToDeviceLocal(ID3D11Device* device)
{
    std::lock_guard lifecycle(hookLifecycle);
    if (!device) return;
    GenericDepthDx11::Install(device);
    const auto shaderResult=EnsureShaderHooks(device);
    if(shaderResult!=NO_ERROR)LOG_WARN("D3D11 shader observation installation refused: {:X}",shaderResult);
    if (!o_CreateSamplerState)
    {
        auto table = *reinterpret_cast<PVOID**>(device);
        HookOwnership::Own::Batch hooks;
        HookOwnership::AddMethod(hooks, ownDeviceRelease, &(PVOID&)o_D3D11DeviceRelease, table, 2, (PVOID)hkD3D11DeviceRelease);
        HookOwnership::AddMethod(hooks, ownDeviceSampler, &(PVOID&)o_CreateSamplerState, table, 23, (PVOID)hkCreateSamplerState);
        const auto result = hooks.Commit();
        if (result != NO_ERROR) LOG_ERROR("Failed to hook ID3D11Device: {:X}", result);
    }
    GenericDepthDx11::BindModRuntime(device);
}
static HRESULT hkD3D11On12CreateDevice(IUnknown* pDevice, UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels,
                                       UINT FeatureLevels, IUnknown* const* ppCommandQueues, UINT NumQueues,
                                       UINT NodeMask, ID3D11Device** ppDevice, ID3D11DeviceContext** ppImmediateContext,
                                       D3D_FEATURE_LEVEL* pChosenFeatureLevel)
{
    LOG_DEBUG("Caller: {}, Device: {:X}", Util::WhoIsTheCaller(_ReturnAddress()), (UINT64) pDevice);

    if (!DxgiHooks::PrepareD3D11On12Boundary())
    {
        if (ppDevice) *ppDevice = nullptr;
        if (ppImmediateContext) *ppImmediateContext = nullptr;
        return E_FAIL;
    }

#ifdef ENABLE_DEBUG_LAYER_DX11
    Flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    bool rtss = false;

    std::vector<IUnknown*> copyCommandQueues;

    if (ppCommandQueues && NumQueues > 0)
        copyCommandQueues.assign(ppCommandQueues, ppCommandQueues + NumQueues);

    // Assuming RTSS is creating a D3D11on12 device, not sure why but sometimes RTSS tries to create
    // it's D3D11on12 device with old CommandQueue which results crash
    // I am changing it's CommandQueue with current swapchain's command queue
    if (!copyCommandQueues.empty() && State::Instance().currentCommandQueue != nullptr &&
        copyCommandQueues[0] != State::Instance().currentCommandQueue &&
        GetModuleHandle(L"RTSSHooks64.dll") != nullptr && pDevice == State::Instance().currentD3D12Device)
    {
        LOG_INFO("Replaced RTSS CommandQueue with correct one {0:X} -> {1:X}", (UINT64) *ppCommandQueues,
                 (UINT64) State::Instance().currentCommandQueue);

        copyCommandQueues[0] = State::Instance().currentCommandQueue;

        rtss = true;
    }

    HRESULT result = E_FAIL;
    {
        ScopedCreatingD3DDevice skipCreatingD3DDevice {};
        D3D11On12FactoryBoundary::Scope internalFactories;
        result = o_D3D11On12CreateDevice(pDevice, Flags, pFeatureLevels, FeatureLevels, rtss ? copyCommandQueues.data() : ppCommandQueues,
                                         rtss ? static_cast<UINT>(copyCommandQueues.size()) : NumQueues, NodeMask, ppDevice,
                                         ppImmediateContext, pChosenFeatureLevel);
    }

    if (result == S_OK && ppDevice && *ppDevice != nullptr && !rtss && (gimi::runtime::Enabled() || State::Instance().currentD3D12Device == nullptr))
    {
        LOG_INFO("Device captured, D3D11Device: {0:X}", (UINT64) *ppDevice);
        HookToDeviceLocal(*ppDevice);
    }

    if (result == S_OK && ppDevice && *ppDevice != nullptr)
        State::Instance().d3d11Devices.push_back(*ppDevice);

    LOG_FUNC_RESULT(result);

    return result;
}

static HRESULT hkD3D11CreateDevice(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
                                   const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
                                   ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel,
                                   ID3D11DeviceContext** ppImmediateContext)
{
    if (_skipDx11Create)
    {
        LOG_DEBUG("Skip");
        const auto hr=o_D3D11CreateDevice(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
                                   ppDevice, pFeatureLevel, ppImmediateContext);
        return hr;
    }

    LOG_DEBUG("Caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));


#ifdef ENABLE_DEBUG_LAYER_DX11
    Flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    DXGI_ADAPTER_DESC desc {};
    std::wstring szName;
    if (pAdapter != nullptr)
    {
        {
            ScopedSkipSpoofingGlobal skipSpoofingGlobal {};

            if (pAdapter->GetDesc(&desc) == S_OK)
            {
                szName = desc.Description;
                LOG_INFO("Adapter Desc: {}", wstring_to_string(szName));

                if (desc.VendorId == VendorId::Microsoft)
                {
                    FactoryRecursionScope factoryRecursion;

                    HRESULT result;
                    {
                        ScopedSkipParentWrapping skipParentWrapping {};
                        ScopedCreatingD3DDevice skipCreatingD3DDevice {};
                        result =
                            o_D3D11CreateDevice(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
                                                SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
                    }


                    return result;
                }
            }
        }
    }

    if (ElevateD3D11FeatureLevel())
    {
        static const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
        };

        D3D_FEATURE_LEVEL maxLevel = D3D_FEATURE_LEVEL_1_0_CORE;

        for (UINT i = 0; i < FeatureLevels; ++i)
        {
            maxLevel = std::max(maxLevel, pFeatureLevels[i]);
        }

        if (maxLevel == D3D_FEATURE_LEVEL_11_0)
        {
            LOG_INFO("Overriding D3D_FEATURE_LEVEL, "
                     "Game requested D3D_FEATURE_LEVEL_11_0, "
                     "we need D3D_FEATURE_LEVEL_11_1!");

            pFeatureLevels = levels;
            FeatureLevels = ARRAYSIZE(levels);
        }
    }

    const auto executedCreateDevice = o_D3D11CreateDevice;
    HRESULT result;
    {
        ScopedSkipParentWrapping skipParentWrapping {};
        FactoryRecursionScope factoryRecursion;
        ScopedCreatingD3DDevice creatingDevice;
        result = executedCreateDevice(pAdapter,DriverType,Software,Flags,pFeatureLevels,FeatureLevels,
            SDKVersion,ppDevice,pFeatureLevel,ppImmediateContext);
    }



    if (result == S_OK && ppDevice != nullptr && *ppDevice != nullptr && (gimi::runtime::Enabled() || State::Instance().currentD3D12Device == nullptr))
    {
        LOG_INFO("Device captured");
        HookToDeviceLocal(*ppDevice);
    }

    LOG_FUNC_RESULT(result);

    if (result == S_OK && ppDevice != nullptr && *ppDevice != nullptr)
        State::Instance().d3d11Devices.push_back(*ppDevice);

    return result;
}

static HRESULT hkD3D11CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
                                               UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
                                               UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                               IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
                                               D3D_FEATURE_LEVEL* pFeatureLevel,
                                               ID3D11DeviceContext** ppImmediateContext)
{
    const std::optional<UINT> applicationFlags = pSwapChainDesc ? std::optional<UINT>{pSwapChainDesc->Flags} : std::nullopt;
    if (_skipDx11Create)
    {

        LOG_DEBUG("Skip");
        const auto hr=o_D3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
                                               SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel,
                                               ppImmediateContext);
        return hr;
    }

    LOG_DEBUG("Caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));

#ifdef ENABLE_DEBUG_LAYER_DX11
    Flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    DXGI_ADAPTER_DESC desc {};
    std::wstring szName;
    if (pAdapter != nullptr)
    {
        {
            ScopedSkipSpoofingGlobal skipSpoofingGlobal {};

            if (pAdapter->GetDesc(&desc) == S_OK)
            {
                szName = desc.Description;
                LOG_INFO("Adapter Desc: {}", wstring_to_string(szName));

                if (desc.VendorId == VendorId::Microsoft)
                {
                    FactoryRecursionScope factoryRecursion;

                    HRESULT result;
                    {
                        ScopedSkipParentWrapping skipParentWrapping {};
                        ScopedCreatingD3DDevice skipCreatingD3DDevice {};
                        result = o_D3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, pFeatureLevels,
                                                                 FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain,
                                                                 ppDevice, pFeatureLevel, ppImmediateContext);
                    }


                    return result;
                }
            }
        }
    }

    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1 };

    if (ElevateD3D11FeatureLevel())
    {
        D3D_FEATURE_LEVEL maxLevel = D3D_FEATURE_LEVEL_1_0_CORE;

        for (UINT i = 0; i < FeatureLevels; ++i)
        {
            maxLevel = std::max(maxLevel, pFeatureLevels[i]);
        }

        if (maxLevel == D3D_FEATURE_LEVEL_11_0)
        {
            LOG_INFO("Overriding D3D_FEATURE_LEVEL, "
                     "Game requested D3D_FEATURE_LEVEL_11_0, "
                     "we need D3D_FEATURE_LEVEL_11_1!");

            pFeatureLevels = levels;
            FeatureLevels = ARRAYSIZE(levels);
        }
    }

    if (pSwapChainDesc != nullptr && pSwapChainDesc->BufferDesc.Height == 2 && pSwapChainDesc->BufferDesc.Width == 2)
    {
        LOG_WARN("Overlay call!");

        FactoryRecursionScope factoryRecursion;
        ScopedSkipParentWrapping skipParentWrapping;
        ScopedCreatingD3DDevice creatingDevice;

        auto result = o_D3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, pFeatureLevels,
                                                      FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice,
                                                      pFeatureLevel, ppImmediateContext);



        return result;
    }

    DXGI_SWAP_CHAIN_DESC localDesc {};

    // For vsync override
    if (pSwapChainDesc != nullptr)
    {
        memcpy(&localDesc, pSwapChainDesc, sizeof(DXGI_SWAP_CHAIN_DESC));

        if (!pSwapChainDesc->Windowed)
        {
            LOG_INFO("Game is creating fullscreen swapchain, disabled V-Sync overrides");
            Config::Instance()->OverrideVsync.set_volatile_value(false);
        }

        if (Config::Instance()->OverrideVsync.value_or_default())
        {
            localDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            localDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

            if (localDesc.BufferCount < 2)
                localDesc.BufferCount = 2;
        }
    }

    HRESULT result;
    {
        FactoryRecursionScope factoryRecursion;
        ScopedCreatingD3DDevice creatingDevice;
    result = o_D3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags,
        pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc ? &localDesc : nullptr,
        ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
    }

    if (SUCCEEDED(result) && ppDevice != nullptr && *ppDevice != nullptr && (gimi::runtime::Enabled() || State::Instance().currentD3D12Device == nullptr))
    {
        LOG_INFO("Device captured");
        HookToDeviceLocal(*ppDevice);
    }

    if (SwapchainCreate::Created(result, ppSwapChain) && pSwapChainDesc != nullptr &&
        ppDevice != nullptr && *ppDevice != nullptr)
    {
        // check for SL proxy
        IDXGISwapChain* realSC = nullptr;
        if (!Util::CheckForRealObject(__FUNCTION__, *ppSwapChain, (IUnknown**) &realSC))
            realSC = *ppSwapChain;

        State::Instance().currentRealSwapchain = realSC;

        IUnknown* realDevice = nullptr;
        if (!Util::CheckForRealObject(__FUNCTION__, *ppDevice, (IUnknown**) &realDevice))
            realDevice = *ppDevice;

        State::Instance().screenWidth = static_cast<float>(pSwapChainDesc->BufferDesc.Width);
        State::Instance().screenHeight = static_cast<float>(pSwapChainDesc->BufferDesc.Height);

        LOG_DEBUG("Created new swapchain: {0:X}, hWnd: {1:X}", (UINT64) *ppSwapChain,
                  (UINT64) pSwapChainDesc->OutputWindow);

        if (State::Instance().currentWrappedSwapchain == *ppSwapChain)
        {
            LOG_DEBUG("Same as current wrapped swapchain, skipping wrapping");
        }
        else
        {
            *ppSwapChain = new WrappedIDXGISwapChain4(realSC, realDevice, pSwapChainDesc->OutputWindow,
                                                      pSwapChainDesc->Flags, false, false, applicationFlags);

            State::Instance().currentSwapchain = *ppSwapChain;
            State::Instance().currentWrappedSwapchain = *ppSwapChain;

            LOG_DEBUG("Created new WrappedIDXGISwapChain4: {0:X}, pDevice: {1:X}", (UINT64) *ppSwapChain,
                      (UINT64) *ppDevice);
        }
    }

    if (SUCCEEDED(result) && ppDevice != nullptr && *ppDevice != nullptr)
        State::Instance().d3d11Devices.push_back(*ppDevice);

    LOG_FUNC_RESULT(result);
    return result;
}
static HRESULT hkCreateSamplerState(ID3D11Device* This, const D3D11_SAMPLER_DESC* pSamplerDesc,
                                    ID3D11SamplerState** ppSamplerState)
{
    if (pSamplerDesc == nullptr || This == nullptr)
        return E_INVALIDARG;

    LOG_FUNC();

    D3D11_SAMPLER_DESC newDesc {};

    newDesc.AddressU = pSamplerDesc->AddressU;
    newDesc.AddressV = pSamplerDesc->AddressV;
    newDesc.AddressW = pSamplerDesc->AddressW;
    newDesc.ComparisonFunc = pSamplerDesc->ComparisonFunc;
    newDesc.BorderColor[0] = pSamplerDesc->BorderColor[0];
    newDesc.BorderColor[1] = pSamplerDesc->BorderColor[1];
    newDesc.BorderColor[2] = pSamplerDesc->BorderColor[2];
    newDesc.BorderColor[3] = pSamplerDesc->BorderColor[3];
    newDesc.MinLOD = pSamplerDesc->MinLOD;
    newDesc.MaxLOD = pSamplerDesc->MaxLOD;

    if (Config::Instance()->AnisotropyOverride.has_value())
    {
        LOG_DEBUG("Overriding {2:X} to anisotropic filtering {0} -> {1}", pSamplerDesc->MaxAnisotropy,
                  Config::Instance()->AnisotropyOverride.value(), (UINT) newDesc.Filter);

        newDesc.Filter = UpgradeToAF(pSamplerDesc->Filter);
        newDesc.MaxAnisotropy = Config::Instance()->AnisotropyOverride.value();
    }
    else
    {
        newDesc.Filter = pSamplerDesc->Filter;
        newDesc.MaxAnisotropy = pSamplerDesc->MaxAnisotropy;
    }

    newDesc.MipLODBias = pSamplerDesc->MipLODBias;

    if ((newDesc.MipLODBias < 0.0f && newDesc.MinLOD != newDesc.MaxLOD) ||
        Config::Instance()->MipmapBiasOverrideAll.value_or_default())
    {
        if (Config::Instance()->MipmapBiasOverride.has_value())
        {
            LOG_DEBUG("Overriding mipmap bias {0} -> {1}", pSamplerDesc->MipLODBias,
                      Config::Instance()->MipmapBiasOverride.value());

            if (Config::Instance()->MipmapBiasFixedOverride.value_or_default())
                newDesc.MipLODBias = Config::Instance()->MipmapBiasOverride.value();
            else if (Config::Instance()->MipmapBiasScaleOverride.value_or_default())
                newDesc.MipLODBias = newDesc.MipLODBias * Config::Instance()->MipmapBiasOverride.value();
            else
                newDesc.MipLODBias = newDesc.MipLODBias + Config::Instance()->MipmapBiasOverride.value();

            newDesc.MipLODBias = std::clamp(newDesc.MipLODBias, -16.0f, 15.99f);
        }

        if (State::Instance().lastMipBiasMax < newDesc.MipLODBias)
            State::Instance().lastMipBiasMax = newDesc.MipLODBias;

        if (State::Instance().lastMipBias > newDesc.MipLODBias)
            State::Instance().lastMipBias = newDesc.MipLODBias;
    }

    return o_CreateSamplerState(This, &newDesc, ppSamplerState);
}

void D3D11Hooks::HookToDevice(ID3D11Device* device) { HookToDeviceLocal(device); }

bool D3D11Hooks::Hook(HMODULE module)
{
    std::lock_guard lifecycle(hookLifecycle);
    if (!module) return false;
    const auto factory = (PVOID)KernelBaseProxy::GetProcAddress_()(module, "D3D11CreateDevice");
    const auto combined = (PVOID)KernelBaseProxy::GetProcAddress_()(module, "D3D11CreateDeviceAndSwapChain");
    const auto on12 = (PVOID)KernelBaseProxy::GetProcAddress_()(module, "D3D11On12CreateDevice");
    HookOwnership::Own::Batch hooks;
    if (factory) o_D3D11CreateDevice.Add(hooks, factory);
    if (combined) o_D3D11CreateDeviceAndSwapChain.Add(hooks, combined);
    if (on12) o_D3D11On12CreateDevice.Add(hooks, on12);
    const auto result = hooks.Commit();
    if (result != NO_ERROR) LOG_ERROR("Failed to hook D3D11 factories: {:X}", result);
    return result == NO_ERROR;
}

void D3D11Hooks::Unhook()
{
    std::lock_guard lifecycle(hookLifecycle);
    GenericDepthDx11::RevokeModStreamOutputCoverage();
    gimi::runtime::StopAccepting();
    std::vector<HookOwnership::Own::Slot*> slots{
        &ownDeviceRelease, &ownDeviceSampler,
        &ownModBuffer, &ownModTexture, &ownModView, &ownModLayout,
        &ownModUnordered, &ownModGeometry, &ownModGeometrySO};
    o_D3D11CreateDevice.AppendSlots(slots);
    o_D3D11CreateDeviceAndSwapChain.AppendSlots(slots);
    o_D3D11On12CreateDevice.AppendSlots(slots);
    D3D11On12FactoryRoutes::routes.AppendSlots(slots);
    pixelHooks.AppendSlots(slots); vertexHooks.AppendSlots(slots); computeHooks.AppendSlots(slots);
    const auto result = HookOwnership::Own::DetachAll(std::span<HookOwnership::Own::Slot* const>(slots));
    if (result != NO_ERROR)
    {
        LOG_ERROR("Failed to detach D3D11 hooks: {:X}", result);
        return;
    }
    o_D3D11DeviceRelease = nullptr;
    o_CreateSamplerState = nullptr;
    o_ModCreateBuffer = nullptr;
    o_ModCreateTexture = nullptr;
    o_ModCreateView = nullptr;
    o_ModCreateLayout = nullptr;
    o_ModCreateUnordered = nullptr;
    o_ModCreateGeometry = nullptr;
    o_ModCreateGeometrySO = nullptr;
    modCreationsCommitted = false;
    modCreationTargets = {};
}

static_assert(std::is_same_v<decltype(&hkModCreateBuffer), PFN_ModCreateBuffer>);
static_assert(std::is_same_v<decltype(&hkModCreateTexture), PFN_ModCreateTexture>);
static_assert(std::is_same_v<decltype(&hkModCreateView), PFN_ModCreateView>);
static_assert(std::is_same_v<decltype(&hkModCreateLayout), PFN_ModCreateLayout>);
static_assert(std::is_same_v<decltype(&hkModCreateUnordered), PFN_ModCreateUnordered>);
static_assert(std::is_same_v<decltype(&hkModCreateGeometry), PFN_ModCreateGeometry>);
static_assert(std::is_same_v<decltype(&hkModCreateGeometrySO), PFN_ModCreateGeometrySO>);
static_assert(std::is_same_v<decltype(&hkCreatePixelShader<0>), PFN_CreatePixelShader>);
static_assert(std::is_same_v<decltype(&hkVisibleCreateVertexShader<0>), PFN_CreateVertexShader>);
static_assert(std::is_same_v<decltype(&hkVisibleCreateComputeShader<0>), PFN_CreateComputeShader>);
static_assert(std::is_same_v<decltype(&hkD3D11On12CreateDevice), PFN_D3D11ON12_CREATE_DEVICE>);
static_assert(std::is_same_v<decltype(&hkD3D11CreateDevice), PFN_D3D11_CREATE_DEVICE>);
static_assert(std::is_same_v<decltype(&hkD3D11CreateDeviceAndSwapChain), PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>);
static_assert(std::is_same_v<decltype(&hkCreateSamplerState), PFN_CreateSamplerState>);
