#include "pch.h"
#include "D3D11_Hooks.h"
#include "SwapchainCreateRule.h"

#include <Util.h>
#include <Config.h>

#include <proxies/KernelBase_Proxy.h>

#include <resource_tracking/GenericDepth_Dx11.h>
#include <mods/gimi/GimiRuntime.h>
#include <genshin/GenshinLegacyCapture.h>
#include <genshin/GenshinQueryScope.h>
#include <genshin/GenshinNativeStartup.h>
#include <genshin/GenshinApplicationProxyHook.h>
#include <genshin/GenshinFactoryPreimageObservation.h>
#include <genshin/GenshinDepthPortableResources.h>
#include <native/NativeLowLatency.h>

#include <wrapped/wrapped_swapchain.h>

#include <detours/detours.h>

#include <d3d11_4.h>
#include <d3d11on12.h>
#include <dxgi1_6.h>

#include "Hook_Utils.h"

#pragma intrinsic(_ReturnAddress)

// Recursion belongs to the creating thread, never an unrelated concurrent call.
static thread_local bool _skipDx11Create = false;
// A combined factory may call the hooked public CreateDevice before handing
// that device to DXGI. Observe THAT inner creation, never certify the exposed
// combined return. No assumed private runtime entry or stack/ABI inspection.
static thread_local bool queryBirthInsideSwap = false;
class ScopedSwapQueryBirth final {
    bool previous_=queryBirthInsideSwap;
public:
    explicit ScopedSwapQueryBirth(bool enabled)noexcept {queryBirthInsideSwap=enabled;}
    ~ScopedSwapQueryBirth(){queryBirthInsideSwap=previous_;}
};

// DirectX
using PFN_CreateSamplerState = rewrite_signature<decltype(&ID3D11Device::CreateSamplerState)>::type;

using PFN_Release = rewrite_signature<decltype(&IUnknown::Release)>::type;

static PFN_D3D11_CREATE_DEVICE o_D3D11CreateDevice = nullptr;
static PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN o_D3D11CreateDeviceAndSwapChain = nullptr;
using PFN_CreatePixelShader = rewrite_signature<decltype(&ID3D11Device::CreatePixelShader)>::type;
static PFN_CreatePixelShader o_CreatePixelShader = nullptr;
static PVOID pixelShaderHookAddress = nullptr;
using PFN_CreateVertexShader = rewrite_signature<decltype(&ID3D11Device::CreateVertexShader)>::type;
static PFN_CreateVertexShader o_VisibleCreateVertexShader = nullptr;
static PVOID visibleVertexHookAddress = nullptr;
using PFN_CreateHullShader = rewrite_signature<decltype(&ID3D11Device::CreateHullShader)>::type;
using PFN_CreateDomainShader = rewrite_signature<decltype(&ID3D11Device::CreateDomainShader)>::type;
static PFN_CreateHullShader o_CreateHullShader=nullptr;
static PFN_CreateDomainShader o_CreateDomainShader=nullptr;
static GenshinNativeQuery::Own::Slot ownHS,ownDS;
static PFN_CreateSamplerState o_CreateSamplerState = nullptr;
static PFN_D3D11ON12_CREATE_DEVICE o_D3D11On12CreateDevice = nullptr;
static PFN_Release o_D3D11DeviceRelease = nullptr;
static GenshinNativeQuery::Own::Slot ownDeviceRelease,ownDeviceSampler;
static GenshinNativeQuery::Own::Slot ownFactory,ownOn12Factory,ownSwapFactory,ownPS,ownVS;

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
static GenshinNativeQuery::Own::Slot ownModUnordered,ownModGeometry,ownModGeometrySO;
static GenshinNativeQuery::Own::Slot ownModBuffer,ownModTexture,ownModView,ownModLayout;
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
        pixelShaderHookAddress==table[15] && visibleVertexHookAddress==table[12];
    gimi::runtime::CreationCoverage(device,complete);
    return complete;
}
VALIDATE_HOOK(hkModCreateBuffer, PFN_ModCreateBuffer)
static HRESULT hkModCreateBuffer(ID3D11Device* device,const D3D11_BUFFER_DESC* desc,
    const D3D11_SUBRESOURCE_DATA* data,ID3D11Buffer** out)
{
    if(!gimi::runtime::Enabled() || GenericDepthDx11::ModInternal()){
        const auto hr=o_ModCreateBuffer(device,desc,data,out);
        if(SUCCEEDED(hr)&&out&&*out&&!GenericDepthDx11::ModInternal())GenshinDepthPortable::Created(*out,data);
        return hr;
    }
    const bool mod=out && ModCreationEnabled(device);
    gimi::BufferCreationEdit edit;
    if(mod)gimi::runtime::PrepareBufferCreation(device,desc,data,edit);
    const auto* actualDesc=edit.changed ? &edit.effective : desc;
    const auto* actualData=edit.changed && data ? &edit.initial : data;
    const auto hr=o_ModCreateBuffer(device,actualDesc,actualData,out);
    if(SUCCEEDED(hr)&&out&&*out)GenshinDepthPortable::Created(*out,actualData);
    if(SUCCEEDED(hr) && out && *out && mod)
        gimi::runtime::BufferCreated(device,*out,actualDesc,actualData,edit.qualified ? &edit : nullptr);
    return hr;
}
VALIDATE_HOOK(hkModCreateTexture, PFN_ModCreateTexture)
static HRESULT hkModCreateTexture(ID3D11Device* device,const D3D11_TEXTURE2D_DESC* desc,
    const D3D11_SUBRESOURCE_DATA* data,ID3D11Texture2D** out)
{
    const auto hr=o_ModCreateTexture(device,desc,data,out);
    if(SUCCEEDED(hr) && out && *out && ModCreationEnabled(device))gimi::runtime::TextureCreated(device,*out,desc,data);
    return hr;
}
VALIDATE_HOOK(hkModCreateView, PFN_ModCreateView)
static HRESULT hkModCreateView(ID3D11Device* device,ID3D11Resource* resource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,ID3D11ShaderResourceView** out)
{
    const auto hr=o_ModCreateView(device,resource,desc,out);
    if(SUCCEEDED(hr) && out && *out && ModCreationEnabled(device))gimi::runtime::ViewCreated(device,resource,*out);
    return hr;
}
VALIDATE_HOOK(hkModCreateLayout, PFN_ModCreateLayout)
static HRESULT hkModCreateLayout(ID3D11Device* device,const D3D11_INPUT_ELEMENT_DESC* elements,UINT count,
    const void* bytes,SIZE_T size,ID3D11InputLayout** out)
{
    const auto hr=o_ModCreateLayout(device,elements,count,bytes,size,out);
    if(SUCCEEDED(hr)&&out&&*out&&!GenericDepthDx11::ModInternal())GenshinDepthPortable::LayoutCreated(*out,elements,count,bytes,size);
    if(SUCCEEDED(hr) && out && *out && ModCreationEnabled(device))gimi::runtime::LayoutCreated(device,*out,elements,count);
    return hr;
}
VALIDATE_HOOK(hkModCreateUnordered, PFN_ModCreateUnordered)
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
VALIDATE_HOOK(hkModCreateGeometry, PFN_ModCreateGeometry)
static HRESULT hkModCreateGeometry(ID3D11Device* device,const void* bytes,SIZE_T size,
    ID3D11ClassLinkage* linkage,ID3D11GeometryShader** out)
{
    const auto hr=o_ModCreateGeometry(device,bytes,size,linkage,out);
    if(SUCCEEDED(hr) && out && *out && ModCreationEnabled(device))
        gimi::runtime::GeometryShaderCreated(device,*out,bytes,size,linkage,nullptr,0,nullptr,0,0,false);
    return hr;
}
VALIDATE_HOOK(hkModCreateGeometrySO, PFN_ModCreateGeometrySO)
static HRESULT hkModCreateGeometrySO(ID3D11Device* device,const void* bytes,SIZE_T size,
    const D3D11_SO_DECLARATION_ENTRY* entries,UINT count,const UINT* strides,UINT strideCount,
    UINT rasterized,ID3D11ClassLinkage* linkage,ID3D11GeometryShader** out)
{
    const auto hr=o_ModCreateGeometrySO(device,bytes,size,entries,count,strides,strideCount,rasterized,linkage,out);
    if(SUCCEEDED(hr) && out && *out && ModCreationEnabled(device))
        gimi::runtime::GeometryShaderCreated(device,*out,bytes,size,linkage,entries,count,strides,strideCount,rasterized,true);
    return hr;
}
LONG D3D11Hooks::InstallGimiCreationHooks(ID3D11Device* device) noexcept
{
    if(!gimi::runtime::Enabled() || !device || GenericDepthDx11::ModInternal())return ERROR_INVALID_OPERATION;
    const auto table=*reinterpret_cast<PVOID**>(device);
    // Shader observation belongs to the existing device initializer. A different
    // ABI/target must not be repaired by rerunning HookToDevice/native setup.
    if(pixelShaderHookAddress!=table[15] || visibleVertexHookAddress!=table[12])return ERROR_NOT_SUPPORTED;
    const std::array<PVOID,7> targets{table[3],table[5],table[7],table[8],table[11],table[13],table[14]};
    if(modCreationsCommitted.load(std::memory_order_acquire))
        return targets==modCreationTargets ? NO_ERROR : ERROR_NOT_SUPPORTED;
    if((o_ModCreateBuffer&&(!ownModBuffer.live||ownModBuffer.entry!=table[3]))||
       (o_ModCreateLayout&&(!ownModLayout.live||ownModLayout.entry!=table[11])))return ERROR_NOT_SUPPORTED;
    GenshinNativeQuery::Own::Batch hooks;
    if(!o_ModCreateBuffer)GenshinApplicationProxyHook::AddMethod(hooks,ownModBuffer,&(PVOID&)o_ModCreateBuffer,table,3,(PVOID)hkModCreateBuffer);
    GenshinApplicationProxyHook::AddMethod(hooks,ownModTexture,&(PVOID&)o_ModCreateTexture,table,5,(PVOID)hkModCreateTexture);
    GenshinApplicationProxyHook::AddMethod(hooks,ownModView,&(PVOID&)o_ModCreateView,table,7,(PVOID)hkModCreateView);
    if(!o_ModCreateLayout)GenshinApplicationProxyHook::AddMethod(hooks,ownModLayout,&(PVOID&)o_ModCreateLayout,table,11,(PVOID)hkModCreateLayout);
    GenshinApplicationProxyHook::AddMethod(hooks,ownModUnordered,&(PVOID&)o_ModCreateUnordered,table,8,(PVOID)hkModCreateUnordered);
    GenshinApplicationProxyHook::AddMethod(hooks,ownModGeometry,&(PVOID&)o_ModCreateGeometry,table,13,(PVOID)hkModCreateGeometry);
    GenshinApplicationProxyHook::AddMethod(hooks,ownModGeometrySO,&(PVOID&)o_ModCreateGeometrySO,table,14,(PVOID)hkModCreateGeometrySO);
    const auto result=hooks.Commit();
    if(result!=NO_ERROR)return result;
    modCreationTargets=targets;modCreationsCommitted.store(true,std::memory_order_release);
    return NO_ERROR;
}
static HRESULT hkCreateSamplerState(ID3D11Device* This, const D3D11_SAMPLER_DESC* pSamplerDesc,
                                    ID3D11SamplerState** ppSamplerState);

VALIDATE_HOOK(hkCreatePixelShader, PFN_CreatePixelShader)
static HRESULT hkCreatePixelShader(ID3D11Device* device,const void* bytes,SIZE_T size,
                                  ID3D11ClassLinkage* linkage,ID3D11PixelShader** shader)
{
    const auto result=o_CreatePixelShader(device,bytes,size,linkage,shader);
    if(SUCCEEDED(result) && shader && *shader){
        GenshinLegacy::NotePixelShaderCreated(device,*shader,bytes,size);
        if(ModCreationEnabled(device))gimi::runtime::ShaderCreated(device,*shader,bytes,size,false,linkage!=nullptr);}
    return result;
}

VALIDATE_HOOK(hkVisibleCreateVertexShader, PFN_CreateVertexShader)
static HRESULT hkVisibleCreateVertexShader(ID3D11Device* device,const void* bytes,SIZE_T size,
                                           ID3D11ClassLinkage* linkage,ID3D11VertexShader** shader)
{
    const auto result=o_VisibleCreateVertexShader(device,bytes,size,linkage,shader);
    if(SUCCEEDED(result) && shader && *shader){
        GenshinLegacy::NoteRawShaderCreated(device,*shader,bytes,size,0);
        if(ModCreationEnabled(device))gimi::runtime::ShaderCreated(device,*shader,bytes,size,true,linkage!=nullptr);}
    return result;
}
VALIDATE_HOOK(hkCreateHullShader, PFN_CreateHullShader)
static HRESULT hkCreateHullShader(ID3D11Device* device,const void* bytes,SIZE_T size,
    ID3D11ClassLinkage* linkage,ID3D11HullShader** shader)
{
    const auto result=o_CreateHullShader(device,bytes,size,linkage,shader);
    if(SUCCEEDED(result) && shader && *shader)GenshinLegacy::NoteRawShaderCreated(device,*shader,bytes,size,3);
    return result;
}
VALIDATE_HOOK(hkCreateDomainShader, PFN_CreateDomainShader)
static HRESULT hkCreateDomainShader(ID3D11Device* device,const void* bytes,SIZE_T size,
    ID3D11ClassLinkage* linkage,ID3D11DomainShader** shader)
{
    const auto result=o_CreateDomainShader(device,bytes,size,linkage,shader);
    if(SUCCEEDED(result) && shader && *shader)GenshinLegacy::NoteRawShaderCreated(device,*shader,bytes,size,4);
    return result;
}
static bool NativeQueryGenshinProcess();
static void HookGenshinRasterCreations(ID3D11Device* device)
{
    if(!NativeQueryGenshinProcess()||!device)return;
    auto** table=*reinterpret_cast<PVOID**>(device);
    if((o_ModCreateBuffer&&(!ownModBuffer.live||ownModBuffer.entry!=table[3]))||
       (o_ModCreateLayout&&(!ownModLayout.live||ownModLayout.entry!=table[11])))return;
    if(o_ModCreateBuffer&&o_ModCreateLayout){GenshinDepthPortable::enabled.store(true);return;}
    GenshinNativeQuery::Own::Batch hooks;
    if(!o_ModCreateBuffer)GenshinApplicationProxyHook::AddMethod(hooks,ownModBuffer,&(PVOID&)o_ModCreateBuffer,table,3,(PVOID)hkModCreateBuffer);
    if(!o_ModCreateLayout)GenshinApplicationProxyHook::AddMethod(hooks,ownModLayout,&(PVOID&)o_ModCreateLayout,table,11,(PVOID)hkModCreateLayout);
    if(hooks.Commit()==NO_ERROR)GenshinDepthPortable::enabled.store(true);
}
static void HookGenshinTessellationShaders(ID3D11Device* device)
{
    if(!NativeQueryGenshinProcess() || (o_CreateHullShader && o_CreateDomainShader))return;
    auto** table=*reinterpret_cast<PVOID**>(device);
    // Public ID3D11Device SDK methods; both slots share the original
    // authenticated AddMethod transaction and teardown ownership mechanism.
    GenshinNativeQuery::Own::Batch hooks;
    if(!o_CreateHullShader)GenshinApplicationProxyHook::AddMethod(hooks,ownHS,&(PVOID&)o_CreateHullShader,table,16,(PVOID)hkCreateHullShader);
    if(!o_CreateDomainShader)GenshinApplicationProxyHook::AddMethod(hooks,ownDS,&(PVOID&)o_CreateDomainShader,table,17,(PVOID)hkCreateDomainShader);
    hooks.Commit(); // Failure rolls back this batch; never bypass its receipt.
}
static void HookVisibleVertexShader(ID3D11Device* device)
{
    PVOID* table=*(PVOID**)device;const auto address=table[12];
    if(o_VisibleCreateVertexShader)return;
    GenshinNativeQuery::Own::Batch hooks;
    GenshinApplicationProxyHook::AddMethod(hooks,ownVS,&(PVOID&)o_VisibleCreateVertexShader,table,12,
        (PVOID)hkVisibleCreateVertexShader);
    if(hooks.Commit()!=NO_ERROR)return;
    visibleVertexHookAddress=address;
}
static void HookGenshinPixelShader(ID3D11Device* device)
{
    // Independent optional observation: failure never revokes the SR pixel hook.
    HookVisibleVertexShader(device);
    HookGenshinTessellationShaders(device);
    HookGenshinRasterCreations(device);
    PVOID* table=*(PVOID**)device;
    // ID3D11Device's SDK-declared CreatePixelShader slot; the fixture checks offsetof(ID3D11DeviceVtbl,...).
    const auto address=table[15];
    if(o_CreatePixelShader)
    {GenshinLegacy::SetShaderHookInstalled(pixelShaderHookAddress==address);GenericDepthDx11::CompleteModCreationHookInstall(device);return;}
    GenshinNativeQuery::Own::Batch hooks;
    GenshinApplicationProxyHook::AddMethod(hooks,ownPS,&(PVOID&)o_CreatePixelShader,table,15,
        (PVOID)hkCreatePixelShader);
    if(hooks.Commit()!=NO_ERROR){GenshinLegacy::SetShaderHookInstalled(false);return;}
    pixelShaderHookAddress=address;GenshinLegacy::SetShaderHookInstalled(true);GenericDepthDx11::CompleteModCreationHookInstall(device);
}

VALIDATE_HOOK(hkD3D11DeviceRelease, PFN_Release)
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

static bool NativeQueryGenshinProcess();
enum class QueryBirthSource:unsigned { DeviceCapture, ExternalHookToDevice, NativeFactory, On12Capture, SwapFactoryCapture,
    DeviceFactoryEntry, SwapFactoryEntry, On12FactoryEntry, NativeNullSwapFactory, NativeNestedDevice,
    DeferredContext, DeferredContext1, DeferredContext2, DeferredContext3, Count };
// Fixed first-entry storage, independent of eligibility and provenance state.
// Only the first entry/decision per factory queues work (six callbacks total).
// No resolver, logger, COM method, ledger lock or wait runs in Record's entry.
struct FactoryEntryObservation {
    std::atomic<uint64_t> entered{0};
    unsigned kind=0;bool nullSwap=false,tlsAtEntry=false;
    DWORD thread=0;uint64_t tick=0;uintptr_t caller=0,executed=0;
    const char* preparation="not-reached";uint64_t waitedMs=0;
    unsigned nativeState=0;uint64_t nativeGeneration=0;
};
static std::array<FactoryEntryObservation,3> factoryEntries{};
static std::atomic<bool> factoryDiagnosticsEnabled{false};
static std::atomic<unsigned> factoryDiagnosticQueueFailures{0};
static const char* FactoryEntryName(unsigned kind)noexcept {
    return kind==0?"CreateDevice":kind==1?"CreateDeviceAndSwapChain":"On12CreateDevice";
}
static void QueueFactoryObservation(PTP_SIMPLE_CALLBACK callback,void* context)noexcept {
    const auto error=GetLastError();
    if(!TrySubmitThreadpoolCallback(callback,context,nullptr))
        factoryDiagnosticQueueFailures.fetch_add(1,std::memory_order_relaxed);
    SetLastError(error);
}
class FactoryFirstEntry final {
    FactoryEntryObservation* first_=nullptr;bool decided_=false;
    const uint64_t tick_=GetTickCount64();
public:
    FactoryFirstEntry(unsigned kind,bool nullSwap,const void* caller,const void* executed)noexcept {
        if(!factoryDiagnosticsEnabled.load(std::memory_order_relaxed))return;
        auto& row=factoryEntries[kind];
        if(row.entered.fetch_add(1,std::memory_order_relaxed))return;
        first_=&row;row.kind=kind;row.nullSwap=nullSwap;row.caller=reinterpret_cast<uintptr_t>(caller);
        row.executed=reinterpret_cast<uintptr_t>(executed);row.thread=GetCurrentThreadId();row.tick=tick_;
        row.tlsAtEntry=_skipDx11Create;
        QueueFactoryObservation(+[](PTP_CALLBACK_INSTANCE,void* context) {
            const auto& r=*static_cast<const FactoryEntryObservation*>(context);
            HMODULE owner=nullptr;
            const bool resolved=GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(r.caller),&owner)!=FALSE;
            try {
                LOG_INFO("R041 factory first entry: pid={} thread={} tick={} type={} nullswap={} tls_sample={} caller={:X} caller_module={:X} caller_rva={:X} caller_resolved={} executed_at_entry={:X} caller_identity=async_resolution_time_sample observation_only=true no_birth_authority=true",
                    GetCurrentProcessId(),r.thread,r.tick,FactoryEntryName(r.kind),r.nullSwap,r.tlsAtEntry,r.caller,
                    reinterpret_cast<uintptr_t>(owner),resolved?r.caller-reinterpret_cast<uintptr_t>(owner):0,resolved,r.executed);
            }catch(...){}
        },&row);
    }
    uint64_t Tick()const noexcept {return tick_;}
    FactoryFirstEntry(const FactoryFirstEntry&)=delete;
    FactoryFirstEntry& operator=(const FactoryFirstEntry&)=delete;
    ~FactoryFirstEntry(){if(!decided_ && first_)Decision(first_->tlsAtEntry?"tls-skip":"returned-before-preparation");}
    void Decision(const char* predicate,uint64_t waitedMs=0)noexcept {
        if(!first_ || decided_)return;
        decided_=true;auto& row=*first_;row.preparation=predicate;row.waitedMs=waitedMs;
        row.nativeState=GenshinNativeQuery::detail::store.initialized.load();
        row.nativeGeneration=GenshinNativeQuery::detail::store.generation.load();
        QueueFactoryObservation(+[](PTP_CALLBACK_INSTANCE,void* context) {
            const auto& r=*static_cast<const FactoryEntryObservation*>(context);
            try {
                LOG_INFO("R041 factory first preparation: pid={} thread={} entry_tick={} type={} nullswap={} first_predicate={} waited_ms={} native_state_sample={} generation_sample={} scheduling_only=true observation_only=true no_birth_authority=true",
                    GetCurrentProcessId(),r.thread,r.tick,FactoryEntryName(r.kind),r.nullSwap,r.preparation,r.waitedMs,r.nativeState,r.nativeGeneration);
            }catch(...){}
        },&row);
    }
};
// One Genshin installation only, fixed storage. Observe all exports before ANY
// Add, then the actual normalized target after each Add but BEFORE Commit.
// These samples never repair/adopt an unknown edge or publish native authority.
class FactoryPreimageObservation final {
    using Chain=GenshinNativeQuery::detail::PreexistingBranchObservation::Chain;
    struct Factory {
        Chain before{},staged{},relocated{};uintptr_t requested=0,entry=0,trampoline=0,executedSlot=0,publishedOriginal=0;
        LONG add=ERROR_PROC_NOT_FOUND;uint64_t installGeneration=0;
        unsigned size=0;bool independent=false,restoreMatches=false,entryComparable=false,entryMatches=false,publishedSlotRead=false;
    };
    struct Snapshot {
        std::atomic_flag claimed=ATOMIC_FLAG_INIT;
        std::array<Factory,3> factories{};uintptr_t caller=0;LONG commit=ERROR_INVALID_OPERATION;
    };
    Snapshot* first_=nullptr;
public:
    FactoryPreimageObservation(const std::array<void*,3>& exports,const void* caller)noexcept {
        if(!factoryDiagnosticsEnabled.load(std::memory_order_relaxed))return;
        static Snapshot row;
        if(row.claimed.test_and_set(std::memory_order_relaxed))return;
        first_=&row;row.caller=reinterpret_cast<uintptr_t>(caller);
        for(unsigned i=0;i<exports.size();++i){row.factories[i].requested=reinterpret_cast<uintptr_t>(exports[i]);
            row.factories[i].before=GenshinNativeQuery::detail::PreexistingBranchObservation::Capture(exports[i]);}
    }
    void Staged(unsigned index,const GenshinNativeQuery::Own::Slot& slot,LONG result)noexcept {
        if(!first_)return;
        auto& r=first_->factories[index];r.add=result;
        r.entry=reinterpret_cast<uintptr_t>(slot.detachReceipt.entry);
        r.trampoline=reinterpret_cast<uintptr_t>(slot.detachReceipt.trampoline);
        r.executedSlot=reinterpret_cast<uintptr_t>(slot.executed);
        r.staged=GenshinNativeQuery::detail::PreexistingBranchObservation::Capture(slot.detachReceipt.entry);
        r.relocated=GenshinNativeQuery::detail::PreexistingBranchObservation::Capture(slot.detachReceipt.trampoline);
        // Copy only the actual same-thread pending receipt; detach rbRestore alone
        // is not an independent preimage, and absent enrollment stays absent.
        const auto& pending=GenshinNativeQuery::detail::pending;
        if(!pending.held)return;
        for(unsigned i=0;i<pending.count;++i){const auto& p=pending.patches[i];
            if(p.original!=slot.executed || p.entry!=slot.detachReceipt.entry ||
               p.trampoline!=slot.detachReceipt.trampoline || p.hook!=slot.hook)continue;
            r.size=p.size;r.independent=p.installPreimageCaptured;r.installGeneration=p.installGeneration;
            r.restoreMatches=r.independent && p.size<=p.installPreimage.size() &&
                !std::memcmp(p.installPreimage.data(),p.code.data()+32,p.size);
            r.entryComparable=r.independent && r.staged.count && p.size<=r.staged.nodes[0].byteCount &&
                p.size<=p.installPreimage.size();
            r.entryMatches=r.entryComparable &&
                !std::memcmp(p.installPreimage.data(),r.staged.nodes[0].bytes.data(),p.size);
            break;
        }
    }
    void Publish(LONG result)noexcept {
        if(!first_)return;
        const auto error=GetLastError();first_->commit=result;
        for(auto& r:first_->factories)r.publishedSlotRead=GenshinNativeQuery::detail::Memory(
            reinterpret_cast<const void*>(r.executedSlot),&r.publishedOriginal,sizeof(r.publishedOriginal));
        QueueFactoryObservation(+[](PTP_CALLBACK_INSTANCE,void* context){
            const auto& snapshot=*static_cast<const Snapshot*>(context);
            for(unsigned i=0;i<snapshot.factories.size();++i){const auto& r=snapshot.factories[i];
                try {
                    LOG_INFO("R041 factory preexisting source: pid={} type={} requested={:X} normalized_entry={:X} trampoline={:X} executed_slot={:X} published_original={:X} published_slot_read={} published_original_matches_trampoline={} add_result={} commit_result={} install_generation={} independent_captured={} restore_matches_independent={} staged_entry_comparable={} staged_entry_matches_independent={} overwritten_size={} own_installer_caller={:X} observer_schema=1 observation_only=true no_callable_authority=true",
                        GetCurrentProcessId(),FactoryEntryName(i),r.requested,r.entry,r.trampoline,r.executedSlot,r.publishedOriginal,
                        r.publishedSlotRead,r.publishedSlotRead && r.publishedOriginal==r.trampoline,r.add,snapshot.commit,
                        r.installGeneration,r.independent,r.restoreMatches,r.entryComparable,r.entryMatches,r.size,snapshot.caller);
                    for(unsigned phase=0;phase<3;++phase){
                        const auto& chain=phase==0?r.before:phase==1?r.staged:r.relocated;
                        const auto* phaseName=phase==0?"before-any-add":phase==1?"staged-before-commit":"trampoline-before-commit";
                        for(unsigned hop=0;hop<chain.count;++hop){const auto& n=chain.nodes[hop];
                            std::array<char,1040> path{};
                            const auto converted=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,n.mappedPath.data(),-1,
                                path.data(),static_cast<int>(path.size()),nullptr,nullptr);
                            LOG_INFO("R041 factory preexisting edge: pid={} thread={} tick={} type={} phase={} node={} nodes={} address={:X} allocation={:X} region={:X} region_size={} state={:X} memory_type={:X} protect={:X} bytes={} prefix_lo_le={:X} prefix_hi_le={:X} edge={} slot={:X} slot_allocation={:X} slot_type={:X} slot_protect={:X} slot_read={} target={:X} mapped_path={} path_chars={} path_error={} path_truncated={} path_utf8_ok={} stop={} end_reread_equal={} evidence=installation_time_memory_sample observation_only=true no_callable_authority=true loaded_image_sha=UNKNOWN installer_identity=UNKNOWN",
                                chain.pid,chain.thread,chain.tick,FactoryEntryName(i),phaseName,
                                hop,chain.count,n.address,n.mapping.allocation,n.mapping.region,n.mapping.size,n.mapping.state,
                                n.mapping.type,n.mapping.protect,n.byteCount,GenshinNativeQuery::detail::Read<uint64_t>(n.bytes.data()),
                                GenshinNativeQuery::detail::Read<uint64_t>(n.bytes.data()+8),n.edge,n.slot,n.slotMapping.allocation,
                                n.slotMapping.type,n.slotMapping.protect,n.slotRead,n.target,path.data(),n.pathLength,n.pathError,
                                n.pathLength>=n.mappedPath.size(),converted!=0,chain.stop,chain.stableAtEnd);
                        }
                        if(!chain.count)LOG_INFO("R041 factory preexisting empty: pid={} type={} phase={} stop={} observation_only=true no_callable_authority=true",
                            chain.pid,FactoryEntryName(i),phaseName,chain.stop);
                    }
                }catch(...){}
            }
        },first_);
        SetLastError(error);
    }
};
// A bounded installation snapshot. It reports actual Add/Commit results and
// executing slot values, never assumes that entering Hook installed anything.
static void ObserveFactoryInstallation(const char* phase,HMODULE module,void* device,void* swap,void* on12,
    LONG deviceAdd,LONG swapAdd,LONG on12Add,LONG commit,bool commitCalled,
    const std::array<bool,3>& previouslyLive={})noexcept
{
    if(!factoryDiagnosticsEnabled.load(std::memory_order_relaxed))return;
    struct SlotSample {uintptr_t address=0,value=0,entry=0,hook=0;bool live=false,detachReady=false;};
    struct Snapshot {
        std::atomic_flag claimed=ATOMIC_FLAG_INIT;
        const char* phase=nullptr;uintptr_t module=0;DWORD thread=0;
        std::array<uintptr_t,3> exports{};std::array<LONG,3> add{};LONG commit=0;bool commitCalled=false;
        std::array<SlotSample,3> slots{};std::array<bool,3> previouslyLive{};bool nativeCommitAttempted=false;
    };
    static std::array<Snapshot,3> snapshots{};
    auto& row=snapshots[phase[0]=='c'?0:phase[0]=='i'?1:2];
    if(row.claimed.test_and_set(std::memory_order_relaxed))return;
    row.phase=phase;row.module=reinterpret_cast<uintptr_t>(module);row.thread=GetCurrentThreadId();
    row.exports={reinterpret_cast<uintptr_t>(device),reinterpret_cast<uintptr_t>(swap),reinterpret_cast<uintptr_t>(on12)};
    row.add={deviceAdd,swapAdd,on12Add};row.commit=commit;row.commitCalled=commitCalled;
    row.previouslyLive=previouslyLive;row.nativeCommitAttempted=commitCalled;
    for(unsigned i=0;i<row.add.size();++i)
        if(row.exports[i] && row.add[i]!=NO_ERROR)row.nativeCommitAttempted=false;
    const std::array<GenshinNativeQuery::Own::Slot*,3> slots{&ownFactory,&ownSwapFactory,&ownOn12Factory};
    if(!commitCalled)for(unsigned i=0;i<slots.size();++i)row.previouslyLive[i]=slots[i]->live;
    const std::array<PVOID*,3> executed{&(PVOID&)o_D3D11CreateDevice,&(PVOID&)o_D3D11CreateDeviceAndSwapChain,&(PVOID&)o_D3D11On12CreateDevice};
    for(unsigned i=0;i<slots.size();++i){const auto& slot=*slots[i];
        row.slots[i]={reinterpret_cast<uintptr_t>(executed[i]),reinterpret_cast<uintptr_t>(*executed[i]),
            reinterpret_cast<uintptr_t>(slot.entry),reinterpret_cast<uintptr_t>(slot.hook),slot.live,slot.detachReady};
    }
    QueueFactoryObservation(+[](PTP_CALLBACK_INSTANCE,void* context) {
        const auto& r=*static_cast<const Snapshot*>(context);
        for(unsigned i=0;i<r.slots.size();++i){const auto& slot=r.slots[i];
            try {
                LOG_INFO("R041 factory installation: pid={} thread={} phase={} type={} module={:X} export={:X} add_result={} attach_staged={} slot_previously_live={} batch_commit_called={} native_commit_attempted={} batch_result={} slot_address={:X} slot_value={:X} registered_entry={:X} registered_hook={:X} live={} detach_receipt_ready={} values=installation_time_sample cached_add_result_is_not_an_attach=true observation_only=true no_birth_authority=true",
                    GetCurrentProcessId(),r.thread,r.phase,FactoryEntryName(i),r.module,r.exports[i],r.add[i],r.exports[i]!=0 && r.add[i]==NO_ERROR && !r.previouslyLive[i],
                    r.previouslyLive[i],r.commitCalled,r.nativeCommitAttempted,r.commit,
                    slot.address,slot.value,slot.entry,slot.hook,slot.live,slot.detachReady);
            }catch(...){}
        }
    },&row);
}

static void ObserveQueryBirth(QueryBirthSource source,const GenshinQueryScope::BirthAttempt& attempt,
    ID3D11Device* device,ID3D11DeviceContext* context,const void* caller,const void* executed,
    const std::source_location& site=std::source_location::current())noexcept
{
    // First occurrence of each source/predicate only: bounded creation-path
    // diagnostics, no draw logging, retry, wait or new retained COM lifetime.
    if(!NativeQueryGenshinProcess())return;
    static std::array<std::atomic<uint64_t>,static_cast<unsigned>(QueryBirthSource::Count)> reported{};
    const auto bit=uint64_t(1)<<static_cast<unsigned>(attempt.reason);
    if(reported[static_cast<unsigned>(source)].fetch_or(bit,std::memory_order_relaxed)&bit)return;
    HMODULE owner=nullptr;
    if(caller)GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(caller),&owner);
    try {
        GenshinQueryScope::ContextBirthObservation birth{};UINT birthSize=sizeof(birth);
        const bool observed=context && SUCCEEDED(context->GetPrivateData(GenshinQueryScope::ContextBirthKey,&birthSize,&birth)) && birthSize==sizeof(birth);
        LOG_INFO("R587 query birth order: pid={} tid={} source={} predicate={} factory_result={:X} entry_tick={} observed_return_tick={} prepare_tick={} refusal_tick={} factory_attach_tick={} context_hooks_tick={} device={:X} context={:X} observed_return={} observed_kind={} observed_entry_tick={} observed_return_tick={} observed_tid={} observed_caller={:X} observed_executed={:X} observation_only=true no_birth_authority=true",
            GetCurrentProcessId(),attempt.entryThread,static_cast<unsigned>(source),GenshinQueryScope::BirthAttempt::Name(attempt.reason),
            static_cast<unsigned>(attempt.factoryResult),attempt.entryTick,attempt.returnTick,attempt.prepareTick,attempt.refusalTick,GenshinQueryScope::factoryAttachTick.load(),
            GenshinQueryScope::contextHooksCommitTick.load(),reinterpret_cast<uintptr_t>(device),reinterpret_cast<uintptr_t>(context),
            observed,static_cast<unsigned>(birth.kind),birth.entryTick,birth.returnTick,birth.thread,birth.caller,birth.executed);
        LOG_INFO("R041 query birth path: pid={} thread={} entry_source={} predicate={} device={:X} context={:X} returned_context={:X} caller={:X} caller_module={:X} caller_rva={:X} executed={:X} witness_matched={} seed_entered={} private_data_result={:X} private_data_size={} store_result={:X} native_call_predicate={} native_generation_begin_sample={} hook_epoch_begin_sample={} refusal_generation_sample={} refusal_hook_epoch_sample={} native_generation_sample={} native_state_sample={} native_reason={} query_epoch_sample={} query_hooks={} coverage_lost={} factory_diagnostics_enabled={} device_factory_entries={} swap_factory_entries={} on12_factory_entries={} diagnostic_queue_failures={} source_file={} source_line={} source_function={} observation_only=true no_birth_authority=true",
            GetCurrentProcessId(),GetCurrentThreadId(),static_cast<unsigned>(source),GenshinQueryScope::BirthAttempt::Name(attempt.reason),
            reinterpret_cast<uintptr_t>(device),reinterpret_cast<uintptr_t>(context),reinterpret_cast<uintptr_t>(attempt.returned),
            reinterpret_cast<uintptr_t>(caller),reinterpret_cast<uintptr_t>(owner),owner?reinterpret_cast<uintptr_t>(caller)-reinterpret_cast<uintptr_t>(owner):0,
            reinterpret_cast<uintptr_t>(executed),attempt.witnessMatched,attempt.seedEntered,static_cast<unsigned>(attempt.privateDataResult),attempt.privateDataSize,
            static_cast<unsigned>(attempt.storeResult),attempt.nativePredicate,attempt.nativeGenerationBegin,attempt.hookEpochBegin,
            attempt.nativeGenerationAtRefusal,attempt.hookEpochAtRefusal,GenshinNativeQuery::detail::store.generation.load(),GenshinNativeQuery::detail::store.initialized.load(),GenshinNativeQuery::Failure(),
            GenshinQueryScope::hooksEpoch.load(),GenshinQueryScope::hooksComplete.load(),GenshinQueryScope::coverageLost.load(),
            factoryDiagnosticsEnabled.load(),factoryEntries[0].entered.load(),factoryEntries[1].entered.load(),factoryEntries[2].entered.load(),factoryDiagnosticQueueFailures.load(),
            site.file_name(),site.line(),site.function_name());
    }catch(...){}
}
// NR context births only. SDK 10.0.26100.0 declares these methods at
// ID3D11Device/1/2/3 slots 27/44/51/62; signatures are taken from those types.
using PFN_QueryDeferred=rewrite_signature<decltype(&ID3D11Device::CreateDeferredContext)>::type;
using PFN_QueryDeferred1=rewrite_signature<decltype(&ID3D11Device1::CreateDeferredContext1)>::type;
using PFN_QueryDeferred2=rewrite_signature<decltype(&ID3D11Device2::CreateDeferredContext2)>::type;
using PFN_QueryDeferred3=rewrite_signature<decltype(&ID3D11Device3::CreateDeferredContext3)>::type;
static PFN_QueryDeferred o_QueryDeferred=nullptr;
static PFN_QueryDeferred1 o_QueryDeferred1=nullptr;
static PFN_QueryDeferred2 o_QueryDeferred2=nullptr;
static PFN_QueryDeferred3 o_QueryDeferred3=nullptr;
static std::array<GenshinNativeQuery::Own::Slot,4> ownQueryDeferred{};
static std::atomic_flag queryCreationInstallBusy=ATOMIC_FLAG_INIT;
static void ObserveDeferredBirth(unsigned api,ID3D11Device* device,ID3D11DeviceContext* context,
    HRESULT hr,uint64_t entryTick,uint64_t returnTick,const void* caller,const void* executed)noexcept
{
    if(!factoryDiagnosticsEnabled.load(std::memory_order_relaxed))return;
    GenshinQueryScope::BirthAttempt attempt;attempt.entryTick=entryTick;attempt.returnTick=returnTick;
    attempt.storeResult=hr;attempt.factoryResult=hr;
    if(SUCCEEDED(hr) && context)
        attempt.storeResult=GenericDepthDx11::NoteDeferredContextBirth(device,context,api,entryTick,returnTick,caller,executed);
    attempt.reason=SUCCEEDED(hr) && context && SUCCEEDED(attempt.storeResult)?
        GenshinQueryScope::BirthAttempt::Reason::DeferredBirthObserved:GenshinQueryScope::BirthAttempt::Reason::DeferredBirthUnproved;
    // No immediate Install/Seed here. Observed creation is not full deferred
    // query/command-list coverage and must not change query admission.
    ObserveQueryBirth(static_cast<QueryBirthSource>(static_cast<unsigned>(QueryBirthSource::DeferredContext)+api),
        attempt,device,context,caller,executed);
}
VALIDATE_HOOK(hkQueryDeferred, PFN_QueryDeferred)
static HRESULT hkQueryDeferred(ID3D11Device* device,UINT flags,ID3D11DeviceContext** output)
{
    const auto tick=GetTickCount64();const auto original=o_QueryDeferred;
    const auto hr=original(device,flags,output);const auto error=GetLastError();const auto returned=GetTickCount64();
    ObserveDeferredBirth(0,device,SUCCEEDED(hr) && output?*output:nullptr,hr,tick,returned,_ReturnAddress(),reinterpret_cast<const void*>(original));SetLastError(error);return hr;
}
VALIDATE_HOOK(hkQueryDeferred1, PFN_QueryDeferred1)
static HRESULT hkQueryDeferred1(ID3D11Device1* device,UINT flags,ID3D11DeviceContext1** output)
{
    const auto tick=GetTickCount64();const auto original=o_QueryDeferred1;
    const auto hr=original(device,flags,output);const auto error=GetLastError();const auto returned=GetTickCount64();
    ObserveDeferredBirth(1,device,SUCCEEDED(hr) && output?*output:nullptr,hr,tick,returned,_ReturnAddress(),reinterpret_cast<const void*>(original));SetLastError(error);return hr;
}
VALIDATE_HOOK(hkQueryDeferred2, PFN_QueryDeferred2)
static HRESULT hkQueryDeferred2(ID3D11Device2* device,UINT flags,ID3D11DeviceContext2** output)
{
    const auto tick=GetTickCount64();const auto original=o_QueryDeferred2;
    const auto hr=original(device,flags,output);const auto error=GetLastError();const auto returned=GetTickCount64();
    ObserveDeferredBirth(2,device,SUCCEEDED(hr) && output?*output:nullptr,hr,tick,returned,_ReturnAddress(),reinterpret_cast<const void*>(original));SetLastError(error);return hr;
}
VALIDATE_HOOK(hkQueryDeferred3, PFN_QueryDeferred3)
static HRESULT hkQueryDeferred3(ID3D11Device3* device,UINT flags,ID3D11DeviceContext3** output)
{
    const auto tick=GetTickCount64();const auto original=o_QueryDeferred3;
    const auto hr=original(device,flags,output);const auto error=GetLastError();const auto returned=GetTickCount64();
    ObserveDeferredBirth(3,device,SUCCEEDED(hr) && output?*output:nullptr,hr,tick,returned,_ReturnAddress(),reinterpret_cast<const void*>(original));SetLastError(error);return hr;
}
static void HookQueryContextCreations(ID3D11Device* device)
{
    static const bool genshin=NativeQueryGenshinProcess();
    if(!device || !genshin)return;
    const auto error=GetLastError();struct RestoreError {DWORD value;~RestoreError(){SetLastError(value);}} restore{error};
    const auto refuse=[&](const char* predicate,unsigned api,void* expected=nullptr,void* actual=nullptr){
        static std::atomic_flag first=ATOMIC_FLAG_INIT;
        if(first.test_and_set())return;
        LOG_WARN("R587 deferred creation hook first refusal: pid={} tid={} tick={} device={:X} predicate={} api={} expected={:X} actual={:X} no_query_authority=true",
            GetCurrentProcessId(),GetCurrentThreadId(),GetTickCount64(),reinterpret_cast<uintptr_t>(device),predicate,api,
            reinterpret_cast<uintptr_t>(expected),reinterpret_cast<uintptr_t>(actual));
    };
    if(queryCreationInstallBusy.test_and_set()){refuse("install-busy",0);return;}
    struct Release {~Release(){queryCreationInstallBusy.clear();}} release;
    Microsoft::WRL::ComPtr<ID3D11Device1> d1;Microsoft::WRL::ComPtr<ID3D11Device2> d2;
    Microsoft::WRL::ComPtr<ID3D11Device3> d3;
    device->QueryInterface(IID_PPV_ARGS(&d1));device->QueryInterface(IID_PPV_ARGS(&d2));device->QueryInterface(IID_PPV_ARGS(&d3));
    const std::array<void**,4> tables{*reinterpret_cast<void***>(device),
        d1?*reinterpret_cast<void***>(d1.Get()):nullptr,d2?*reinterpret_cast<void***>(d2.Get()):nullptr,
        d3?*reinterpret_cast<void***>(d3.Get()):nullptr};
    constexpr std::array<unsigned,4> indices{27,44,51,62};
    const std::array<PVOID*,4> originals{&(PVOID&)o_QueryDeferred,&(PVOID&)o_QueryDeferred1,
        &(PVOID&)o_QueryDeferred2,&(PVOID&)o_QueryDeferred3};
    const std::array<PVOID,4> handlers{(PVOID)hkQueryDeferred,(PVOID)hkQueryDeferred1,(PVOID)hkQueryDeferred2,(PVOID)hkQueryDeferred3};
    bool pending=false;
    for(unsigned i=0;i<tables.size();++i){
        if(!tables[i])continue;
        const auto entry=tables[i][indices[i]];
        // A second, different device implementation cannot overwrite a live
        // original. Do not guess a proxy unwrap or install one ABI over another.
        if(ownQueryDeferred[i].live && ownQueryDeferred[i].entry!=entry){refuse("different-live-target",i,ownQueryDeferred[i].entry,entry);return;}
        for(unsigned j=0;j<i;++j)if(tables[j] && entry==tables[j][indices[j]]){refuse("aliased-distinct-sdk-methods",i,nullptr,entry);return;}
        pending|=!ownQueryDeferred[i].live;
    }
    if(!pending)return;
    // Observation-only installation: a sound rollback must preserve existing
    // native authority. Integrity and rollback failures still revoke normally.
    GenshinNativeQuery::Own::Batch batch(nullptr,0,GenshinNativeQuery::InstallRequirement::Optional);
    std::array<bool,4> staged{};
    for(unsigned i=0;i<tables.size();++i){
        if(!tables[i] || ownQueryDeferred[i].live)continue;
        if(GenshinApplicationProxyHook::AddMethod(batch,ownQueryDeferred[i],originals[i],tables[i],indices[i],handlers[i])!=NO_ERROR)break;
        staged[i]=true;
        // Compare the actual Detours targets, too. Distinct SDK entry pointers
        // can normalize to the same body; abort before publishing either ABI.
        for(unsigned j=0;j<i;++j)
            if((staged[j] || ownQueryDeferred[j].live) && ownQueryDeferred[i].detachReceipt.entry &&
               ownQueryDeferred[i].detachReceipt.entry==ownQueryDeferred[j].detachReceipt.entry){
                refuse("same-normalized-sdk-target",i,ownQueryDeferred[j].entry,ownQueryDeferred[i].entry);return;
            }
    }
    const auto result=batch.Commit();
    LOG_INFO("R587 deferred creation hooks: pid={} tid={} tick={} device={:X} result={} base={} v1={} v2={} v3={} query_certificate_granted=false deferred_replay_supported=false",
        GetCurrentProcessId(),GetCurrentThreadId(),GetTickCount64(),reinterpret_cast<uintptr_t>(device),result,
        ownQueryDeferred[0].live,ownQueryDeferred[1].live,ownQueryDeferred[2].live,ownQueryDeferred[3].live);
}
// Record an observed successful factory return, including skipped/On12/real
// swap paths. This is metadata only; it cannot assert new native creation or
// repair a late attach. A prior authenticated inner birth remains write-once.
static void NoteUnwitnessedFactoryReturn(HRESULT hr,ID3D11Device** output,ID3D11DeviceContext** returned,
    uint64_t entryTick,const void* caller,const void* executed)noexcept
{
    if(hr!=S_OK || !output || !*output || !factoryDiagnosticsEnabled.load(std::memory_order_relaxed))return;
    const auto error=GetLastError();struct RestoreError {DWORD value;~RestoreError(){SetLastError(value);}} restore{error};
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate;
    auto* context=returned?*returned:nullptr;
    if(!context){(*output)->GetImmediateContext(&immediate);context=immediate.Get();}
    GenshinQueryScope::NoteContextBirth(*output,context,GenshinQueryScope::ContextBirthKind::UnwitnessedReturn,
        entryTick,GetTickCount64(),caller,executed);
    if(!GenshinNativeQuery::detail::factoryDepth)HookQueryContextCreations(*output);
}
static void HookToDeviceLocal(ID3D11Device* InDevice, GenshinQueryScope::CreationWitness* birth = nullptr,
    QueryBirthSource source=QueryBirthSource::DeviceCapture,const void* caller=nullptr,
    const std::source_location& site=std::source_location::current())
{
    // The depth finder hooks the immediate context the first time any D3D11 device is captured, whether or not this device's
    // own sampler hooks are already in (a second call with the same device is a no-op inside Install()).
    // Only the normal native factory return can supply this one-use witness.
    // Full context hooks and absent-state seeding precede initializer publication.
    HookQueryContextCreations(InDevice);
    if (birth) GenericDepthDx11::PrepareCreatedContextQueries(InDevice,*birth);
    if(InDevice && !birth && NativeQueryGenshinProcess()){
        // Observe the actual late capture BEFORE ordinary Install. Do not use
        // State(), NativeObject(), Intact() or any helper that creates/adopts a
        // context state. This entry cannot supply a creation witness.
        GenshinQueryScope::BirthAttempt attempt;
        attempt.reason=GenshinQueryScope::BirthAttempt::Reason::NoCreationWitness;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;InDevice->GetImmediateContext(&context);
        ObserveQueryBirth(source,attempt,InDevice,context.Get(),caller,nullptr,site);
    }
    const bool genshin = GenshinLegacy::InitializeDevice(InDevice);
    GenericDepthDx11::Install(InDevice, genshin);
    if(genshin)GenericDepthDx11::BindModRuntime(InDevice);
    if (genshin && GenericDepthDx11::Installed())
        HookGenshinPixelShader(InDevice);

    if (o_CreateSamplerState != nullptr || InDevice == nullptr)
    {
        if(genshin)GenericDepthDx11::CompletePsStateCacheInstall(InDevice);
        return;
    }

    LOG_DEBUG("Dx11");

    // Get the vtable pointer
    PVOID* pVTable = *(PVOID**) InDevice;

    GenshinNativeQuery::Own::Batch hooks;
    // Classify each device method independently: an exact application proxy
    // retains Own transaction/detach ownership without native query enrollment.
    if(pVTable[2])GenshinApplicationProxyHook::AddMethod(hooks,ownDeviceRelease,&(PVOID&)o_D3D11DeviceRelease,
        pVTable,2,(PVOID)hkD3D11DeviceRelease);
    if(pVTable[23])GenshinApplicationProxyHook::AddMethod(hooks,ownDeviceSampler,&(PVOID&)o_CreateSamplerState,
        pVTable,23,(PVOID)hkCreateSamplerState);
    const auto result=hooks.Commit();
    if(result!=NO_ERROR)LOG_ERROR("Failed to hook ID3D11Device: {:X}",result);

    // Publish the optional cache only after every owned device-hook commit:
    // even a successful native install advances the ledger generation.
    if(genshin)GenericDepthDx11::CompletePsStateCacheInstall(InDevice);
}

// Consume the original winning setup refusal only. One bounded creation-path
// observation; no ledger lock, module reopen, COM call or authentication retry.
static void ObserveGenshinStartupPatchRefusal() noexcept
{
    const auto* first=GenshinNativeQuery::FirstRevocation();
    if(!first || !first->setupPatchFailure.captured)return;
    static std::atomic_flag reported=ATOMIC_FLAG_INIT;
    if(reported.test_and_set(std::memory_order_relaxed))return;
    const auto& o=first->setupPatchFailure;const auto& origin=first->origin;
    try {
        LOG_WARN("Genshin startup patch refusal: pid={} thread={} predicate={} attempt={} index={} patches={} images={} owner={} rva={:X} size={} mismatch_offset={} saved_byte={:X} clean_byte={:X} snapshot_generation={} failure_generation={} publication_generation={} busy_at_failure_sample={} entry={:X} requested={:X} declared_module={:X} producer_file={} producer_line={} producer_function={} observation_only=true no_callable_authority=true",
                 GetCurrentProcessId(),first->thread,o.predicate,o.attempt,o.index,o.patchCount,o.imageCount,
                 o.owner,o.rva,o.size,o.mismatchOffset,o.savedByte,o.cleanByte,o.snapshotGeneration,
                 o.failureGeneration,o.publicationGeneration,o.busyAtFailure,
                 reinterpret_cast<uintptr_t>(origin.entry),reinterpret_cast<uintptr_t>(origin.requested),
                 reinterpret_cast<uintptr_t>(origin.declaredModule),origin.enrollment.file_name(),
                 origin.enrollment.line(),origin.enrollment.function_name());
    }catch(...){}
}

// Both real native roots use ONE existing startup budget and loader probe.
// This schedules readiness only; NativeCall still proves the exact executed
// factory, clean/wholecode, thread/reentry, generation and returned identity.
static void AwaitFactoryPreparation(FactoryFirstEntry& entry,bool nativeRoot,IDXGIAdapter* adapter,
    D3D_DRIVER_TYPE type,HMODULE software,UINT flags,UINT sdk,ID3D11Device** device)
{
    const auto fail=[&](const char* predicate){entry.Decision(predicate);};
    if(!nativeRoot){fail("swapchain-exposes-context-before-return");return;}
    if(!NativeQueryGenshinProcess()){fail("non-genshin");return;}
    if(State::Instance().currentD3D12Device!=nullptr){fail("existing-dx12-device");return;}
    auto* config=Config::Instance();
    if(!config->AcquireGenshinControls().Active()){fail("controls-inactive");return;}
    if(!config->OverlayMenu.value_or_default()){fail("overlay-disabled");return;}
    if(!device){fail("no-device-output");return;}
    if(software){fail("software-module");return;}
    if(sdk!=D3D11_SDK_VERSION){fail("sdk-version");return;}
    if(flags&(D3D11_CREATE_DEVICE_DEBUG|D3D11_CREATE_DEVICE_DEBUGGABLE)){fail("debug-device");return;}
    if(adapter?type!=D3D_DRIVER_TYPE_UNKNOWN:type!=D3D_DRIVER_TYPE_HARDWARE){fail("driver-type");return;}
    if(GenshinNativeQuery::detail::draftLedger){fail("setup-draft-thread");return;}
    if(GenshinNativeQuery::detail::factoryDepth){fail("native-factory-reentry");return;}
    static GenshinNativeStartup::Barrier startup;
    const auto joined=startup.Await(
        [](){return GenshinNativeQuery::detail::store.initialized.load()==2;},
        [](){return GenshinNativeQuery::detail::Revoked();},
        GenshinNativeStartup::ProbeLoaderCallout,GetTickCount64,[](DWORD ms){Sleep(ms);});
    entry.Decision(GenshinNativeStartup::Name(joined.status),joined.waitedMs);
    // Existing original-refusal observer only; no additional setup/retry here.
    ObserveGenshinStartupPatchRefusal();
}

// No initializer sees the outputs before this production call core finishes.
// The shared issuer executes the captured original exactly once. Game identity
// selects whether its private birth witness may reach the real hook installer.
template<class Factory> static HRESULT CallNativeFactoryAndPrepare(Factory executed,bool genshin,
    IDXGIAdapter* adapter,D3D_DRIVER_TYPE type,HMODULE software,UINT flags,const D3D_FEATURE_LEVEL* levels,
    UINT count,UINT sdk,ID3D11Device** device,D3D_FEATURE_LEVEL* feature,ID3D11DeviceContext** context,
    uint64_t entryTick=0,const void* caller=nullptr,bool nestedDevice=false)
{
    const auto forward=[&](){
        if constexpr(std::is_same_v<Factory,PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>)
            return executed(adapter,type,software,flags,levels,count,sdk,nullptr,nullptr,device,feature,context);
        else return executed(adapter,type,software,flags,levels,count,sdk,device,feature,context);
    };
    if(!genshin)return forward();
    // R786: one creation receipt, never an alternate admission or a late seed.
    bool ledgerRead=false,patchFound=false,patchStraight=false,patchOwn=false,trampolineMatches=false;
    uint64_t beforeGeneration=0;
    {
        GenshinNativeQuery::detail::Guard guard;
        if(guard.held){
            ledgerRead=true;beforeGeneration=GenshinNativeQuery::detail::store.generation.load();
            const auto* patch=GenshinNativeQuery::detail::Find(
                std::is_same_v<Factory,PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>?
                GenshinNativeQuery::detail::Ledger().swapFactoryEntry:GenshinNativeQuery::detail::Ledger().factoryEntry);
            patchFound=patch!=nullptr;
            if(patch){patchStraight=patch->straight;patchOwn=patch->ownSetup;trampolineMatches=patch->trampoline==reinterpret_cast<void*>(executed);}
        }
    }
    bool callback=false,birthPresent=false,seeded=false;
    GenshinQueryScope::BirthAttempt birthAttempt;
    if(entryTick)birthAttempt.entryTick=entryTick;
    const auto result=GenshinQueryScope::FactoryBirthIssuer::Invoke(executed,adapter,type,software,flags,levels,count,sdk,
        device,feature,context,[&](ID3D11Device* created,GenshinQueryScope::CreationWitness* birth){
            callback=true;birthPresent=birth!=nullptr;
            if(birth){
                birthAttempt.prepareTick=GetTickCount64();
                HookQueryContextCreations(created);
                GenshinQueryScope::NoteContextBirth(created,birthAttempt.context,
                    nestedDevice?GenshinQueryScope::ContextBirthKind::NestedDevice:
                    std::is_same_v<Factory,PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>?GenshinQueryScope::ContextBirthKind::NullSwap:
                    GenshinQueryScope::ContextBirthKind::Device,birthAttempt.entryTick,birthAttempt.returnTick,caller,
                    reinterpret_cast<const void*>(executed));
                seeded=GenericDepthDx11::PrepareCreatedContextQueries(created,*birth);
                // The current preparation implementation only cancels after a
                // successful Matches, but before Seed, when its full installer
                // did not publish g_genshinInstalled. No late seed is attempted.
                if(!seeded && !birthAttempt.refused && !birthAttempt.seedEntered && birthAttempt.witnessMatched)
                    birthAttempt.Reject(GenshinQueryScope::BirthAttempt::Reason::PrepareHooksNotPublished);
            }else NoteUnwitnessedFactoryReturn(S_OK,device,context,birthAttempt.entryTick,caller,reinterpret_cast<const void*>(executed));
        },&birthAttempt);
    ObserveQueryBirth(nestedDevice?QueryBirthSource::NativeNestedDevice:
        std::is_same_v<Factory,PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>?
        QueryBirthSource::NativeNullSwapFactory:QueryBirthSource::NativeFactory,birthAttempt,birthAttempt.device,
        birthAttempt.context?birthAttempt.context:birthAttempt.returned,
        caller,reinterpret_cast<const void*>(executed));
    if(!birthPresent || !seeded){
        // R787: reconstruct only the failed interface predicates after refusal.
        // No AddImage, NativeObject, COM call, state creation or gate consumes it.
        const auto observe=[](const char* role,const void* object,unsigned slots){
            void* table=nullptr;uintptr_t liveOwner=0,firstValue=0,firstExpected=0;
            bool read=false,ledgerRead=false,section=false,equal=false;int known=-1;
            unsigned rva=0,firstMismatch=UINT_MAX,firstUnowned=UINT_MAX;DWORD protection=0;
            read=GenshinNativeQuery::detail::Memory(object,&table,sizeof(table));
            if(read && table){MEMORY_BASIC_INFORMATION info{};
                if(VirtualQuery(table,&info,sizeof(info))==sizeof(info)){liveOwner=reinterpret_cast<uintptr_t>(info.AllocationBase);protection=info.Protect;}}
            {
                GenshinNativeQuery::detail::Guard guard;
                if(guard.held && read && table){
                    ledgerRead=true;known=GenshinNativeQuery::detail::Owner(table,size_t(slots)*sizeof(void*));
                    if(known>=0){const auto& image=GenshinNativeQuery::detail::Ledger().images[known];
                        rva=static_cast<unsigned>(reinterpret_cast<uintptr_t>(table)-reinterpret_cast<uintptr_t>(image.live));
                        section=image.SectionHas(rva,size_t(slots)*sizeof(void*),IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE);
                        equal=GenshinNativeQuery::detail::Equal(table,image.clean.data()+rva,size_t(slots)*sizeof(void*));
                        for(unsigned i=0;i<slots;++i){
                            const auto expected=GenshinNativeQuery::detail::Read<uintptr_t>(image.clean.data()+rva+i*sizeof(void*));
                            uintptr_t value=0;const bool got=GenshinNativeQuery::detail::Memory(static_cast<const unsigned char*>(table)+i*sizeof(void*),&value,sizeof(value));
                            if(firstMismatch==UINT_MAX && (!got || value!=expected)){firstMismatch=i;firstValue=value;firstExpected=expected;}
                            if(firstUnowned==UINT_MAX && !GenshinNativeQuery::detail::Executable(reinterpret_cast<const void*>(expected)))firstUnowned=i;
                        }
                    }
                }
            }
            if(ledgerRead && known<0 && table && slots<=150){
                // R788: after refusal, compare the entire private table to
                // already authenticated read-only image data. Observation only.
                std::array<uintptr_t,150> words{};unsigned matches=0,bestPrefix=0,bestImage=UINT_MAX,bestRva=0;
                bool copied=GenshinNativeQuery::detail::Memory(table,words.data(),size_t(slots)*sizeof(uintptr_t));
                std::array<int,150> owners{};std::array<uint32_t,150> offsets{};owners.fill(-1);
                {
                    GenshinNativeQuery::detail::Guard guard;
                    if(guard.held && copied){
                        for(unsigned i=0;i<slots;++i){owners[i]=GenshinNativeQuery::detail::Owner(reinterpret_cast<void*>(words[i]));
                            if(owners[i]>=0)offsets[i]=static_cast<uint32_t>(words[i]-reinterpret_cast<uintptr_t>(GenshinNativeQuery::detail::Ledger().images[owners[i]].live));}
                        for(unsigned index=0;index<GenshinNativeQuery::detail::Ledger().imageCount;++index){
                            const auto& image=GenshinNativeQuery::detail::Ledger().images[index];
                            for(unsigned sectionIndex=0;sectionIndex<image.sectionCount;++sectionIndex){const auto& s=image.sections[sectionIndex];
                                if(!(s.flags&IMAGE_SCN_MEM_READ) || (s.flags&(IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_DISCARDABLE)) || s.size<size_t(slots)*8)continue;
                                for(uint64_t at=s.rva;at+size_t(slots)*8<=uint64_t(s.rva)+s.size;at+=8){
                                    if(GenshinNativeQuery::detail::Read<uintptr_t>(image.clean.data()+at)!=words[0])continue;
                                    unsigned prefix=0;while(prefix<slots && GenshinNativeQuery::detail::Read<uintptr_t>(image.clean.data()+at+8*prefix)==words[prefix])++prefix;
                                    if(prefix>bestPrefix){bestPrefix=prefix;bestImage=index;bestRva=static_cast<unsigned>(at);}
                                    if(prefix==slots)++matches;
                                }
                            }
                        }
                    }
                }
                LOG_WARN("Genshin private table template observation: pid={} role={} slots={} copied={} exact_matches={} best_prefix={} best_image={} best_rva={:X} post_refusal_only=true no_callable_authority=true",
                         GetCurrentProcessId(),role,slots,copied,matches,bestPrefix,bestImage,bestRva);
                if(copied)for(unsigned i=0;i<slots;++i)
                    LOG_WARN("Genshin private table method observation: pid={} role={} slot={} value={:X} known_owner={} rva={:X} no_callable_authority=true",
                             GetCurrentProcessId(),role,i,words[i],owners[i],offsets[i]);
            }
            LOG_WARN("Genshin interface observation: pid={} role={} object={:X} slots={} read={} table={:X} live_owner={:X} protection={:X} ledger_read={} known_owner={} rva={:X} section={} equal={} first_mismatch={} live_value={:X} expected_value={:X} first_unowned_method={} post_refusal_only=true no_callable_authority=true",
                     GetCurrentProcessId(),role,reinterpret_cast<uintptr_t>(object),slots,read,reinterpret_cast<uintptr_t>(table),
                     liveOwner,protection,ledgerRead,known,rva,section,equal,firstMismatch,firstValue,firstExpected,firstUnowned);
        };
        observe("selected_adapter",adapter,10);
        ID3D11Device* created=nullptr;ID3D11DeviceContext* returned=nullptr;
        if(device && GenshinNativeQuery::detail::Memory(device,&created,sizeof(created)) && created)observe("returned_device",created,43);
        if(context && GenshinNativeQuery::detail::Memory(context,&returned,sizeof(returned)) && returned)observe("returned_context",returned,115);
    }
    const auto owner=[](const void* object){
        void* table=nullptr;MEMORY_BASIC_INFORMATION info{};
        if(!object || !GenshinNativeQuery::detail::Memory(object,&table,sizeof(table)) || !table ||
           VirtualQuery(table,&info,sizeof(info))!=sizeof(info) || info.Type!=MEM_IMAGE)return uintptr_t(0);
        return reinterpret_cast<uintptr_t>(info.AllocationBase);
    };
    LOG_INFO("Genshin native birth observation: pid={} result={:X} callback={} birth={} seeded={} ledger_read={} patch_found={} patch_straight={} patch_own={} trampoline_matches={} generation_before={} generation_after={} native_state={} reason={} query_hooks={} query_epoch={} coverage_lost={} adapter_table_owner={:X} observation_only=true",
             GetCurrentProcessId(),static_cast<unsigned>(result),callback,birthPresent,seeded,ledgerRead,patchFound,
             patchStraight,patchOwn,trampolineMatches,beforeGeneration,GenshinNativeQuery::detail::store.generation.load(),
             GenshinNativeQuery::detail::store.initialized.load(),GenshinNativeQuery::Failure(),
             GenshinQueryScope::hooksComplete.load(),GenshinQueryScope::hooksEpoch.load(),
             GenshinQueryScope::coverageLost.load(),owner(adapter));
    if(const auto* first=GenshinNativeQuery::FirstRevocation())
        LOG_WARN("Genshin native birth first_revocation: pid={} thread={} file={} line={} function={} observation_only=true",
                 GetCurrentProcessId(),first->thread,first->file,first->line,first->function);
    namespace native=GenshinNativeQuery::detail;
    if(native::firstContextFailureState.load(std::memory_order_acquire)==2){
        const auto& f=native::firstContextFailure;
        const auto leaf=[&](const char* role,const native::ContextTableLeaf& value){
            LOG_INFO("R833 context admission: pid={} thread={} object={:X} table={:X} generation={} slots={} role={} scope={} predicate={} status_kind={} status={} slot={} mode0={} mode1={} rva={:X} offset={} actual0={:X} expected0={:X} actual1={:X} expected1={:X} read_bytes={} requested_bytes={} binding_shape={} execution_only={} gpu_context_refused={} native_revoked={} observation_only=true",
                GetCurrentProcessId(),f.thread,f.object,f.table,f.generation,f.slots,role,value.scope,value.predicate,value.statusKind,value.status,value.slot,value.modeMismatch[0],value.modeMismatch[1],value.rva,value.offset,
                value.actual[0],value.expected[0],value.actual[1],value.expected[1],value.readBytes,value.requestedBytes,
                f.bindingShape,f.executionOnly,native::contextExecutionRefused.load(),native::store.revoked.load());
        };
        leaf("native-table",f.tableLeaf);leaf("constructor",f.inlineLeaf);leaf("state-swap",f.swapLeaf);
        leaf("binding-shape",f.bindingLeaf);
    }
    ObserveGenshinStartupPatchRefusal();
    return result;
}
static bool NativeQueryGenshinProcess()
{
    wchar_t path[32768]{};const auto count=GetModuleFileNameW(nullptr,path,32768);
    return count && count<32768 && GenshinFsr::IsGenshinExecutable(std::wstring_view(path,count));
}

VALIDATE_HOOK(hkD3D11On12CreateDevice, PFN_D3D11ON12_CREATE_DEVICE)
static HRESULT hkD3D11On12CreateDevice(IUnknown* pDevice, UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels,
                                       UINT FeatureLevels, IUnknown* const* ppCommandQueues, UINT NumQueues,
                                       UINT NodeMask, ID3D11Device** ppDevice, ID3D11DeviceContext** ppImmediateContext,
                                       D3D_FEATURE_LEVEL* pChosenFeatureLevel)
{
    FactoryFirstEntry firstEntry(2,false,_ReturnAddress(),reinterpret_cast<const void*>(o_D3D11On12CreateDevice));
    GenshinNativeQuery::NoteFactoryEntry();
    firstEntry.Decision("on12-no-native-birth-route");
    LOG_DEBUG("Caller: {}, Device: {:X}", Util::WhoIsTheCaller(_ReturnAddress()), (UINT64) pDevice);

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
        result = o_D3D11On12CreateDevice(pDevice, Flags, pFeatureLevels, FeatureLevels, copyCommandQueues.data(),
                                         static_cast<UINT>(copyCommandQueues.size()), NodeMask, ppDevice,
                                         ppImmediateContext, pChosenFeatureLevel);
    }

    NoteUnwitnessedFactoryReturn(result,ppDevice,ppImmediateContext,firstEntry.Tick(),_ReturnAddress(),reinterpret_cast<const void*>(o_D3D11On12CreateDevice));
    if (result == S_OK && *ppDevice != nullptr && !rtss && State::Instance().currentD3D12Device == nullptr)
    {
        LOG_INFO("Device captured, D3D11Device: {0:X}", (UINT64) *ppDevice);
        HookToDeviceLocal(*ppDevice,nullptr,QueryBirthSource::On12Capture,_ReturnAddress());
    }

    if (result == S_OK && *ppDevice != nullptr)
        State::Instance().d3d11Devices.push_back(*ppDevice);

    LOG_FUNC_RESULT(result);

    return result;
}

VALIDATE_HOOK(hkD3D11CreateDevice, PFN_D3D11_CREATE_DEVICE)
static HRESULT hkD3D11CreateDevice(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
                                   const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
                                   ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel,
                                   ID3D11DeviceContext** ppImmediateContext)
{
    if(GenshinDepthPortable::privateCreation)
        return o_D3D11CreateDevice(pAdapter,DriverType,Software,Flags,pFeatureLevels,FeatureLevels,
                                  SDKVersion,ppDevice,pFeatureLevel,ppImmediateContext);
    FactoryFirstEntry firstEntry(0,false,_ReturnAddress(),reinterpret_cast<const void*>(o_D3D11CreateDevice));
    GenshinNativeQuery::NoteFactoryEntry();
    if(_skipDx11Create && queryBirthInsideSwap && State::Instance().currentD3D12Device==nullptr &&
       !GenshinNativeQuery::detail::factoryDepth){
        // Exact public inner factory, unchanged SDK arguments. Its existing
        // native issuer must authenticate and finish hooks/seed before this
        // return reaches the combined factory's DXGI exposure. Failure simply
        // forwards the same once-only call; no outer-return seed is possible.
        firstEntry.Decision("nested-public-create-before-swap-exposure");
        return CallNativeFactoryAndPrepare(o_D3D11CreateDevice,true,pAdapter,DriverType,Software,Flags,
            pFeatureLevels,FeatureLevels,SDKVersion,ppDevice,pFeatureLevel,ppImmediateContext,firstEntry.Tick(),_ReturnAddress(),true);
    }
    if (_skipDx11Create)
    {
        LOG_DEBUG("Skip");
        const auto hr=o_D3D11CreateDevice(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
                                   ppDevice, pFeatureLevel, ppImmediateContext);
        NoteUnwitnessedFactoryReturn(hr,ppDevice,ppImmediateContext,firstEntry.Tick(),_ReturnAddress(),reinterpret_cast<const void*>(o_D3D11CreateDevice));
        return hr;
    }

    LOG_DEBUG("Caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));

    AwaitFactoryPreparation(firstEntry,true,pAdapter,DriverType,Software,Flags,SDKVersion,ppDevice);

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
                    _skipDx11Create = true;

                    HRESULT result;
                    {
                        ScopedSkipParentWrapping skipParentWrapping {};
                        ScopedCreatingD3DDevice skipCreatingD3DDevice {};
                        result =
                            o_D3D11CreateDevice(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
                                                SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
                    }

                    _skipDx11Create = false;
                    NoteUnwitnessedFactoryReturn(result,ppDevice,ppImmediateContext,firstEntry.Tick(),_ReturnAddress(),reinterpret_cast<const void*>(o_D3D11CreateDevice));
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

    _skipDx11Create = true;

    const auto executedCreateDevice = o_D3D11CreateDevice;
    HRESULT result;
    {
        ScopedSkipParentWrapping skipParentWrapping {};
        result = CallNativeFactoryAndPrepare(executedCreateDevice,NativeQueryGenshinProcess() &&
            State::Instance().currentD3D12Device==nullptr,pAdapter,DriverType,Software,Flags,pFeatureLevels,FeatureLevels,
            SDKVersion,ppDevice,pFeatureLevel,ppImmediateContext,firstEntry.Tick(),_ReturnAddress());
    }

    _skipDx11Create = false;

    if (result == S_OK && ppDevice != nullptr && *ppDevice != nullptr && State::Instance().currentD3D12Device == nullptr)
    {
        LOG_INFO("Device captured");
        // Optional native birth preparation already finished inside the exact
        // call core. Failure still takes this ordinary device/SR initialization.
        HookToDeviceLocal(*ppDevice,nullptr,QueryBirthSource::DeviceCapture,_ReturnAddress());
    }

    LOG_FUNC_RESULT(result);

    if (result == S_OK && ppDevice != nullptr && *ppDevice != nullptr)
        State::Instance().d3d11Devices.push_back(*ppDevice);

    return result;
}

VALIDATE_HOOK(hkD3D11CreateDeviceAndSwapChain, PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)
static HRESULT hkD3D11CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
                                               UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
                                               UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                               IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
                                               D3D_FEATURE_LEVEL* pFeatureLevel,
                                               ID3D11DeviceContext** ppImmediateContext)
{
    FactoryFirstEntry firstEntry(1,pSwapChainDesc==nullptr && ppSwapChain==nullptr,_ReturnAddress(),
        reinterpret_cast<const void*>(o_D3D11CreateDeviceAndSwapChain));
    // Only the authenticated active native CreateDevice may consume this
    // exact null-swapchain child. Handle it before logging or wrapper callbacks.
    HRESULT nestedResult=E_UNEXPECTED;
    if(_skipDx11Create && GenshinNativeQuery::NativeCall::TryNestedSwap(o_D3D11CreateDeviceAndSwapChain,
        _ReturnAddress(),pAdapter,DriverType,Software,Flags,pFeatureLevels,FeatureLevels,SDKVersion,
        pSwapChainDesc,ppSwapChain,ppDevice,pFeatureLevel,ppImmediateContext,nestedResult)){
        firstEntry.Decision("authenticated-nested-swap");return nestedResult;
    }
    GenshinNativeQuery::NoteFactoryEntry();
    if (_skipDx11Create)
    {

        LOG_DEBUG("Skip");
        const auto hr=o_D3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
                                               SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel,
                                               ppImmediateContext);
        NoteUnwitnessedFactoryReturn(hr,ppDevice,ppImmediateContext,firstEntry.Tick(),_ReturnAddress(),reinterpret_cast<const void*>(o_D3D11CreateDeviceAndSwapChain));
        return hr;
    }

    LOG_DEBUG("Caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));
    AwaitFactoryPreparation(firstEntry,pSwapChainDesc==nullptr && ppSwapChain==nullptr,
        pAdapter,DriverType,Software,Flags,SDKVersion,ppDevice);

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
                    _skipDx11Create = true;

                    HRESULT result;
                    {
                        ScopedSkipParentWrapping skipParentWrapping {};
                        ScopedCreatingD3DDevice skipCreatingD3DDevice {};
                        result = o_D3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, pFeatureLevels,
                                                                 FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain,
                                                                 ppDevice, pFeatureLevel, ppImmediateContext);
                    }

                    _skipDx11Create = false;
                    NoteUnwitnessedFactoryReturn(result,ppDevice,ppImmediateContext,firstEntry.Tick(),_ReturnAddress(),reinterpret_cast<const void*>(o_D3D11CreateDeviceAndSwapChain));
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

        _skipDx11Create = true;
        State::Instance().skipParentWrapping = true;

        auto result = o_D3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, pFeatureLevels,
                                                      FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice,
                                                      pFeatureLevel, ppImmediateContext);

        State::Instance().skipParentWrapping = false;
        _skipDx11Create = false;
        NoteUnwitnessedFactoryReturn(result,ppDevice,ppImmediateContext,firstEntry.Tick(),_ReturnAddress(),reinterpret_cast<const void*>(o_D3D11CreateDeviceAndSwapChain));
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

    _skipDx11Create = true;

    // Route only the actual null-swap call through its matching native issuer.
    // A real swapchain can expose the context in DXGI callbacks before return.
    // Only an actually hooked inner public CreateDevice may prepare its own
    // authenticated birth first; the outer return never retrofits history.
    const bool nullSwap=pSwapChainDesc==nullptr && ppSwapChain==nullptr;
    const auto combinedCreate=[&](){
        ScopedSwapQueryBirth innerBirth(NativeQueryGenshinProcess() && State::Instance().currentD3D12Device==nullptr);
        return o_D3D11CreateDeviceAndSwapChain(pAdapter,DriverType,Software,Flags,pFeatureLevels,FeatureLevels,
            SDKVersion,pSwapChainDesc!=nullptr?&localDesc:nullptr,ppSwapChain,ppDevice,pFeatureLevel,ppImmediateContext);
    };
    auto result = nullSwap ? CallNativeFactoryAndPrepare(o_D3D11CreateDeviceAndSwapChain,
        NativeQueryGenshinProcess() && State::Instance().currentD3D12Device==nullptr,
        pAdapter,DriverType,Software,Flags,pFeatureLevels,FeatureLevels,SDKVersion,ppDevice,pFeatureLevel,ppImmediateContext,
        firstEntry.Tick(),_ReturnAddress()) : combinedCreate();
    _skipDx11Create = false;
    NoteUnwitnessedFactoryReturn(result,ppDevice,ppImmediateContext,firstEntry.Tick(),_ReturnAddress(),reinterpret_cast<const void*>(o_D3D11CreateDeviceAndSwapChain));

    if (SUCCEEDED(result) && ppDevice != nullptr && *ppDevice != nullptr && State::Instance().currentD3D12Device == nullptr)
    {
        LOG_INFO("Device captured");
        HookToDeviceLocal(*ppDevice,nullptr,QueryBirthSource::SwapFactoryCapture,_ReturnAddress());
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
                                                      pSwapChainDesc->Flags, false);

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

VALIDATE_HOOK(hkCreateSamplerState, PFN_CreateSamplerState)
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

__declspec(noinline) void D3D11Hooks::HookToDevice(ID3D11Device* InDevice) {
    HookToDeviceLocal(InDevice,nullptr,QueryBirthSource::ExternalHookToDevice,_ReturnAddress());
}

void D3D11Hooks::Hook(HMODULE dx11Module)
{
    // Cache only executable identity samples; a cache hit is not attach proof.
    if(NativeQueryGenshinProcess()){
        HMODULE self=nullptr;
        if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&D3D11Hooks::Hook),&self))factoryDiagnosticsEnabled.store(true);
    }
    if (o_D3D11CreateDevice != nullptr){
        ObserveFactoryInstallation("cached",dx11Module,nullptr,nullptr,nullptr,
            ERROR_ALREADY_EXISTS,ERROR_ALREADY_EXISTS,ERROR_ALREADY_EXISTS,ERROR_ALREADY_EXISTS,false);
        return;
    }

    LOG_DEBUG("");
    const auto factory = (PVOID) KernelBaseProxy::GetProcAddress_()(dx11Module,"D3D11CreateDevice");
    const auto swapFactory = (PVOID) KernelBaseProxy::GetProcAddress_()(dx11Module,"D3D11CreateDeviceAndSwapChain");
    const auto on12Factory = (PVOID) KernelBaseProxy::GetProcAddress_()(dx11Module,"D3D11On12CreateDevice");
    if(factory || swapFactory || on12Factory){
        const std::array<bool,3> previouslyLive{ownFactory.live,ownSwapFactory.live,ownOn12Factory.live};
        FactoryPreimageObservation preexisting({factory,swapFactory,on12Factory},_ReturnAddress());
        // All owner acquisition/IO and temporary module releases precede Begin.
        // Capture all three before any Add; filter enablement is irrelevant.
        std::array<GenshinNativeQuery::detail::FactoryPredecessor,3> predecessor{};
        if(NativeQueryGenshinProcess()){
            using GenshinNativeQuery::detail::FactoryComposition::Capture;
            predecessor={Capture(dx11Module,"D3D11CreateDevice",factory),
                Capture(dx11Module,"D3D11CreateDeviceAndSwapChain",swapFactory),
                Capture(dx11Module,"D3D11On12CreateDevice",on12Factory)};
        }
        GenshinNativeQuery::Own::Batch hooks(predecessor.data(),static_cast<unsigned>(predecessor.size()));
        LONG deviceAttach=ERROR_PROC_NOT_FOUND,swapAttach=ERROR_PROC_NOT_FOUND,on12Attach=ERROR_PROC_NOT_FOUND;
        if(factory)deviceAttach=hooks.Add(ownFactory,&(PVOID&)o_D3D11CreateDevice,nullptr,factory,
            (PVOID)hkD3D11CreateDevice,dx11Module,"D3D11CreateDevice",true,std::source_location::current(),&predecessor[0]);
        preexisting.Staged(0,ownFactory,deviceAttach);
        if(swapFactory)swapAttach=hooks.Add(ownSwapFactory,&(PVOID&)o_D3D11CreateDeviceAndSwapChain,nullptr,swapFactory,
            (PVOID)hkD3D11CreateDeviceAndSwapChain,dx11Module,"D3D11CreateDeviceAndSwapChain",true,std::source_location::current(),&predecessor[1]);
        preexisting.Staged(1,ownSwapFactory,swapAttach);
        if(on12Factory)on12Attach=hooks.Add(ownOn12Factory,&(PVOID&)o_D3D11On12CreateDevice,nullptr,on12Factory,
            (PVOID)hkD3D11On12CreateDevice,dx11Module,"D3D11On12CreateDevice",true,std::source_location::current(),&predecessor[2]);
        preexisting.Staged(2,ownOn12Factory,on12Attach);
        const auto attachBegin=GetTickCount64();
        const auto result=hooks.Commit();const auto attachEnd=GetTickCount64();
        if(result==NO_ERROR)GenshinQueryScope::factoryAttachTick.store(attachEnd);
        if(factoryDiagnosticsEnabled.load())
            LOG_INFO("R587 factory attach order: pid={} tid={} module={:X} begin_tick={} end_tick={} result={} entry_ticks_are_observations=true",
                GetCurrentProcessId(),GetCurrentThreadId(),reinterpret_cast<uintptr_t>(dx11Module),attachBegin,attachEnd,result);
        preexisting.Publish(result);
        if(result!=NO_ERROR)LOG_ERROR("Failed to hook D3D11 factories: {:X}",result);
#if !defined(OPTISCALER_R533_NATIVE_PROVENANCE_VALIDATION)
        else GenshinNativeQuery::ScheduleSetup(dx11Module);
#endif
        ObserveFactoryInstallation("install",dx11Module,factory,swapFactory,on12Factory,
            deviceAttach,swapAttach,on12Attach,result,true,previouslyLive);
    }else ObserveFactoryInstallation("no-exports",dx11Module,factory,swapFactory,on12Factory,
        ERROR_PROC_NOT_FOUND,ERROR_PROC_NOT_FOUND,ERROR_PROC_NOT_FOUND,ERROR_PROC_NOT_FOUND,false);
}

void D3D11Hooks::Unhook()
{
    GenericDepthDx11::RevokeModStreamOutputCoverage();
    gimi::runtime::StopAccepting();
    // Independently retained native transaction receipts also support detach
    // when optional authentication was unavailable. Never erase foreign edits.
    for(auto* slot:{&ownFactory,&ownOn12Factory,&ownSwapFactory,&ownDeviceRelease,&ownDeviceSampler,&ownPS,&ownVS,&ownHS,&ownDS,&ownModBuffer,&ownModTexture,&ownModView,&ownModLayout,&ownModUnordered,&ownModGeometry,&ownModGeometrySO,
        &ownQueryDeferred[0],&ownQueryDeferred[1],&ownQueryDeferred[2],&ownQueryDeferred[3]}){
        if(slot->live && (!slot->detachReady || !GenshinNativeQuery::detail::PatchIntact(slot->detachReceipt))){
            GenshinNativeQuery::Revoke();
            LOG_ERROR("Refusing unverified D3D11 detach; preserving installed chain");return;
        }
    }
    GenshinNativeQuery::Revoke(); // Lifetime ends before any trampoline detach.
    GenshinQueryScope::MappingHooksIncomplete();
    GenshinLegacy::Shutdown();
    if(GenshinNativeQuery::Begin()!=NO_ERROR)return;
    if(DetourUpdateThread(GetCurrentThread())!=NO_ERROR){GenshinNativeQuery::Abort();return;}

    if (o_D3D11CreateDevice != nullptr)
        DetourDetach(&(PVOID&) o_D3D11CreateDevice, hkD3D11CreateDevice);

    if (o_D3D11DeviceRelease != nullptr)
        DetourDetach(&(PVOID&) o_D3D11DeviceRelease, hkD3D11DeviceRelease);

    if (o_D3D11On12CreateDevice != nullptr)
        DetourDetach(&(PVOID&) o_D3D11On12CreateDevice, hkD3D11On12CreateDevice);

    if (o_D3D11CreateDeviceAndSwapChain != nullptr)
        DetourDetach(&(PVOID&) o_D3D11CreateDeviceAndSwapChain, hkD3D11CreateDeviceAndSwapChain);

    if (o_CreateSamplerState != nullptr)
        DetourDetach(&(PVOID&) o_CreateSamplerState, hkCreateSamplerState);

    if (o_CreatePixelShader != nullptr)
        DetourDetach(&(PVOID&) o_CreatePixelShader, hkCreatePixelShader);
    if (o_VisibleCreateVertexShader != nullptr)
        DetourDetach(&(PVOID&) o_VisibleCreateVertexShader, hkVisibleCreateVertexShader);
    if(o_CreateHullShader)DetourDetach(&(PVOID&)o_CreateHullShader,hkCreateHullShader);
    if(o_CreateDomainShader)DetourDetach(&(PVOID&)o_CreateDomainShader,hkCreateDomainShader);

    if(o_ModCreateBuffer)DetourDetach(&(PVOID&)o_ModCreateBuffer,hkModCreateBuffer);
    if(o_ModCreateTexture)DetourDetach(&(PVOID&)o_ModCreateTexture,hkModCreateTexture);
    if(o_ModCreateView)DetourDetach(&(PVOID&)o_ModCreateView,hkModCreateView);
    if(o_ModCreateLayout)DetourDetach(&(PVOID&)o_ModCreateLayout,hkModCreateLayout);
    if(o_ModCreateUnordered)DetourDetach(&(PVOID&)o_ModCreateUnordered,hkModCreateUnordered);
    if(o_ModCreateGeometry)DetourDetach(&(PVOID&)o_ModCreateGeometry,hkModCreateGeometry);
    if(o_ModCreateGeometrySO)DetourDetach(&(PVOID&)o_ModCreateGeometrySO,hkModCreateGeometrySO);
    if(o_QueryDeferred)DetourDetach(&(PVOID&)o_QueryDeferred,hkQueryDeferred);
    if(o_QueryDeferred1)DetourDetach(&(PVOID&)o_QueryDeferred1,hkQueryDeferred1);
    if(o_QueryDeferred2)DetourDetach(&(PVOID&)o_QueryDeferred2,hkQueryDeferred2);
    if(o_QueryDeferred3)DetourDetach(&(PVOID&)o_QueryDeferred3,hkQueryDeferred3);
    auto detourResult = GenshinNativeQuery::Commit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to unhook ID3D11Device: {:X}", detourResult);
    }
    else
    {
        for(auto* slot:{&ownFactory,&ownOn12Factory,&ownSwapFactory,&ownDeviceRelease,&ownDeviceSampler,&ownPS,&ownVS,&ownHS,&ownDS,&ownModBuffer,&ownModTexture,&ownModView,&ownModLayout,&ownModUnordered,&ownModGeometry,&ownModGeometrySO,
        &ownQueryDeferred[0],&ownQueryDeferred[1],&ownQueryDeferred[2],&ownQueryDeferred[3]})
            slot->live=false;
        o_D3D11CreateDevice = nullptr;
        o_CreateSamplerState = nullptr;
        o_CreatePixelShader = nullptr;
        o_ModCreateBuffer=nullptr;o_ModCreateTexture=nullptr;o_ModCreateView=nullptr;o_ModCreateLayout=nullptr;
        o_ModCreateUnordered=nullptr;o_ModCreateGeometry=nullptr;o_ModCreateGeometrySO=nullptr;
        modCreationsCommitted=false;modCreationTargets={};
        GenshinDepthPortable::enabled=false;GenshinDepthPortable::Lost();
        o_QueryDeferred=nullptr;o_QueryDeferred1=nullptr;o_QueryDeferred2=nullptr;o_QueryDeferred3=nullptr;
        pixelShaderHookAddress = nullptr;
        o_VisibleCreateVertexShader = nullptr;visibleVertexHookAddress = nullptr;
        o_CreateHullShader=nullptr;o_CreateDomainShader=nullptr;
        o_D3D11DeviceRelease = nullptr;
        o_D3D11On12CreateDevice = nullptr;
        o_D3D11CreateDeviceAndSwapChain = nullptr;
    }
}

#pragma endregion

// Validation-only ABI; link this ACTUAL TU and its real dependencies into a
// non-game EXE. No replacement issuer/installer, fake Config or fake COM state.
// The first entry exercises the exact production call core. The second calls
// the outer hook too, which must refuse birth for a non-Genshin executable.
#if defined(OPTISCALER_R533_NATIVE_PROVENANCE_VALIDATION)
extern "C" __declspec(dllexport) HRESULT R533NativeFactoryCore(ID3D11Device** device,ID3D11DeviceContext** context)
{
    return CallNativeFactoryAndPrepare(o_D3D11CreateDevice,true,nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,
        nullptr,0,D3D11_SDK_VERSION,device,nullptr,context);
}
extern "C" __declspec(dllexport) HRESULT R560InstalledFactoryCore(ID3D11Device** device,ID3D11DeviceContext** context)
{
    const auto result=R533NativeFactoryCore(device,context);
    if(result==S_OK && device && *device){
        HookToDeviceLocal(*device);
        // Explicitly exercise the exact Genshin PS/VS installers in a non-game
        // harness; this does not grant game identity or numeric shader facts.
        HookGenshinPixelShader(*device);
    }
    return result;
}
extern "C" __declspec(dllexport) HRESULT R533ActualOuterFactory(ID3D11Device** device,ID3D11DeviceContext** context)
{
    return hkD3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
        device,nullptr,context);
}
extern "C" __declspec(dllexport) PFN_D3D11_CREATE_DEVICE R533ExecutedNativeFactory()
{
    return o_D3D11CreateDevice; // Actual captured pointer, never a test issuer.
}
extern "C" __declspec(dllexport) void* R533ObservedNativeOriginal(void* entry)
{
    if(!GenshinNativeQuery::Intact())return nullptr;
    const auto* patch=GenshinNativeQuery::detail::Find(entry);
    return patch?patch->trampoline:nullptr; // Read-only receipt for hidden-range negative.
}
#endif
