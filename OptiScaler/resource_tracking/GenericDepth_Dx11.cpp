#include "pch.h"
#include <resource_tracking/DrawObservation_Dx11.h>
#include <inputs/taa/TaaInputDx11.h>
#include <inputs/taa/TaaPassDx11.h>
#include <ui_mask/DrawUiMask.h>

#include <native/OpticalFgNrPolicy.h>
#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

#include "GenericDepth_Dx11.h"
#include <mods/gimi/GimiRuntime.h>
#include <hooks/D3D11_Hooks.h>
#include <hooks/RoutedHook.h>


#include <Config.h>
#include <Util.h>
#include <dlssnr/DlssNr_NativeMode.h>

#include <native/DepthCopyDx11.h>
#include <native/DepthFinderCore.h>
#include <native/NativeLowLatency.h>

#include <detours/detours.h>

#include <imgui/imgui.h>
#include <menu/UiLocalization.h>
#include <menu/SettingsSwitchRow.h>

#include <d3d11_4.h>
#include <TlHelp32.h>
#include <intrin.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <format>
#include <string>
#include <cstring>

// See GenericDepth_Dx11.h. Reuses native::DepthFinderCore (resource_tracking/GenericDepth_Dx12.cpp is the D3D12 sibling of this
// file; both observers feed the same core).
#include <hooks/HookOwnership.h>
#include <runtime/HookCpuTiming.h>
namespace
{

// A context can switch method implementations after SwapDeviceContextState.
// Each implementation keeps its own trampoline; no context borrows the first
// device's original. Method-local recursion preserves the existing observers.
void STDMETHODCALLTYPE hkDrawIndexed(ID3D11DeviceContext* c, UINT count, UINT first, INT base);
void STDMETHODCALLTYPE hkDraw(ID3D11DeviceContext* c, UINT count, UINT first);
void STDMETHODCALLTYPE hkDrawIndexedInstanced(ID3D11DeviceContext* c, UINT count, UINT instances, UINT first, INT base, UINT firstInstance);
void STDMETHODCALLTYPE hkDrawInstanced(ID3D11DeviceContext* c, UINT count, UINT instances, UINT first, UINT firstInstance);
void STDMETHODCALLTYPE hkOMSetRenderTargets(ID3D11DeviceContext* This, UINT NumViews,
                                            ID3D11RenderTargetView* const* ppRenderTargetViews,
                                            ID3D11DepthStencilView* pDepthStencilView);
void STDMETHODCALLTYPE hkOMSetRenderTargetsAndUAV(ID3D11DeviceContext* This, UINT NumRTVs,
                                                  ID3D11RenderTargetView* const* ppRenderTargetViews,
                                                  ID3D11DepthStencilView* pDepthStencilView, UINT UAVStartSlot,
                                                  UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
                                                  const UINT* pUAVInitialCounts);
void STDMETHODCALLTYPE hkDrawAuto(ID3D11DeviceContext* c);
void STDMETHODCALLTYPE hkDrawIndexedInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT offset);
void STDMETHODCALLTYPE hkDrawInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT offset);
void STDMETHODCALLTYPE hkDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z);
void STDMETHODCALLTYPE hkRSSetViewports(ID3D11DeviceContext* c,UINT count,const D3D11_VIEWPORT* views);
void STDMETHODCALLTYPE hkClearDepthStencilView(ID3D11DeviceContext* This, ID3D11DepthStencilView* pView,
                                               UINT ClearFlags, FLOAT Depth, UINT8 Stencil);
void STDMETHODCALLTYPE hkDepthDispatchIndirect(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT offset);
HRESULT STDMETHODCALLTYPE hkDepthMap(ID3D11DeviceContext* c,ID3D11Resource* r,UINT sub,D3D11_MAP type,UINT flags,D3D11_MAPPED_SUBRESOURCE* mapped);
void STDMETHODCALLTYPE hkDepthUnmap(ID3D11DeviceContext* c,ID3D11Resource* r,UINT sub);
void STDMETHODCALLTYPE hkDepthCopyRegion(ID3D11DeviceContext* c,ID3D11Resource* d,UINT sub,UINT x,UINT y,UINT z,ID3D11Resource* s,UINT ss,const D3D11_BOX* box);
void STDMETHODCALLTYPE hkDepthCopyResource(ID3D11DeviceContext* c,ID3D11Resource* d,ID3D11Resource* s);
void STDMETHODCALLTYPE hkDepthUpdate(ID3D11DeviceContext* c,ID3D11Resource* d,UINT sub,const D3D11_BOX* box,const void* data,UINT row,UINT slice);
void STDMETHODCALLTYPE hkDepthCopyStructure(ID3D11DeviceContext* c,ID3D11Buffer* d,UINT offset,ID3D11UnorderedAccessView* view);
void STDMETHODCALLTYPE hkClearRtv(ID3D11DeviceContext* c,ID3D11RenderTargetView* view,const FLOAT* values);
void STDMETHODCALLTYPE hkClearUavUint(ID3D11DeviceContext* c,ID3D11UnorderedAccessView* view,const UINT* values);
void STDMETHODCALLTYPE hkClearUavFloat(ID3D11DeviceContext* c,ID3D11UnorderedAccessView* view,const FLOAT* values);
void STDMETHODCALLTYPE hkDepthMips(ID3D11DeviceContext* c,ID3D11ShaderResourceView* view);
void STDMETHODCALLTYPE hkDepthResolve(ID3D11DeviceContext* c,ID3D11Resource* d,UINT sub,ID3D11Resource* s,UINT ss,DXGI_FORMAT format);
void STDMETHODCALLTYPE hkExecuteCommandList(ID3D11DeviceContext* c,ID3D11CommandList* list,BOOL restore);
void STDMETHODCALLTYPE hkClearState(ID3D11DeviceContext* c);
HRESULT STDMETHODCALLTYPE hkFinishCommandList(ID3D11DeviceContext* c,BOOL restore,ID3D11CommandList** list);
void STDMETHODCALLTYPE hkDepthCopyRegion1(ID3D11DeviceContext1* c,ID3D11Resource* d,UINT sub,UINT x,UINT y,UINT z,ID3D11Resource* s,UINT ss,const D3D11_BOX* box,UINT flags);
void STDMETHODCALLTYPE hkDepthUpdate1(ID3D11DeviceContext1* c,ID3D11Resource* d,UINT sub,const D3D11_BOX* box,const void* data,UINT row,UINT slice,UINT flags);
void STDMETHODCALLTYPE hkDiscardResource(ID3D11DeviceContext1* c,ID3D11Resource* resource);
void STDMETHODCALLTYPE hkDiscardView(ID3D11DeviceContext1* c,ID3D11View* view);
void STDMETHODCALLTYPE hkSwapDeviceContextState(ID3D11DeviceContext1* c,ID3DDeviceContextState* state,ID3DDeviceContextState** previous);
void STDMETHODCALLTYPE hkDepthClearView(ID3D11DeviceContext1* c,ID3D11View* view,const FLOAT* values,const D3D11_RECT* rects,UINT count);
void STDMETHODCALLTYPE hkDiscardView1(ID3D11DeviceContext1* c,ID3D11View* view,const D3D11_RECT* rects,UINT count);
void STDMETHODCALLTYPE hkModSOSetTargets(ID3D11DeviceContext* c,UINT count,ID3D11Buffer* const* buffers,const UINT* offsets);
template<size_t Index> struct ContextMethodDomain {};
template<auto Callback,size_t Index> using ContextRoute=HookOwnership::RoutedHook<Callback,Index,ContextMethodDomain<Index>>;
ContextRoute<&hkDrawIndexed,12> o_DrawIndexed;
ContextRoute<&hkDraw,13> o_Draw;
ContextRoute<&hkDrawIndexedInstanced,20> o_DrawIndexedInstanced;
ContextRoute<&hkDrawInstanced,21> o_DrawInstanced;
ContextRoute<&hkOMSetRenderTargets,33> o_OMSetRenderTargets;
ContextRoute<&hkOMSetRenderTargetsAndUAV,34> o_OMSetRenderTargetsAndUAV;
ContextRoute<&hkDrawAuto,38> o_DrawAuto;
ContextRoute<&hkDrawIndexedInstancedIndirect,39> o_DrawIndexedInstancedIndirect;
ContextRoute<&hkDrawInstancedIndirect,40> o_DrawInstancedIndirect;
ContextRoute<&hkDispatch,41> o_Dispatch;
ContextRoute<&hkRSSetViewports,44> o_RSSetViewports;
ContextRoute<&hkClearDepthStencilView,53> o_ClearDepthStencilView;
ContextRoute<&hkDepthDispatchIndirect,42> o_DepthDispatchIndirect;
ContextRoute<&hkDepthMap,14> o_DepthMap;
ContextRoute<&hkDepthUnmap,15> o_DepthUnmap;
ContextRoute<&hkDepthCopyRegion,46> o_DepthCopyRegion;
ContextRoute<&hkDepthCopyResource,47> o_DepthCopyResource;
ContextRoute<&hkDepthUpdate,48> o_DepthUpdate;
ContextRoute<&hkDepthCopyStructure,49> o_DepthCopyStructure;
ContextRoute<&hkClearRtv,50> o_ClearRtv;
ContextRoute<&hkClearUavUint,51> o_ClearUavUint;
ContextRoute<&hkClearUavFloat,52> o_ClearUavFloat;
ContextRoute<&hkDepthMips,54> o_DepthMips;
ContextRoute<&hkDepthResolve,57> o_DepthResolve;
ContextRoute<&hkExecuteCommandList,58> o_ExecuteCommandList;
ContextRoute<&hkClearState,110> o_ClearState;
ContextRoute<&hkFinishCommandList,114> o_FinishCommandList;
ContextRoute<&hkDepthCopyRegion1,115> o_DepthCopyRegion1;
ContextRoute<&hkDepthUpdate1,116> o_DepthUpdate1;
ContextRoute<&hkDiscardResource,117> o_DiscardResource;
ContextRoute<&hkDiscardView,118> o_DiscardView;
ContextRoute<&hkSwapDeviceContextState,131> o_SwapDeviceContextState;
ContextRoute<&hkDepthClearView,132> o_DepthClearView;
ContextRoute<&hkDiscardView1,133> o_DiscardView1;
ContextRoute<&hkModSOSetTargets,37> o_ModSOSetTargets;

std::mutex g_mutex;
native::DepthFinderCore g_core;

// The context the game draws its frames through: its device's immediate context, unwrapped from a wrapping layer's proxy
// (Streamline's), a reference held. The hooks patch the functions themselves, so every context of the same kind runs them (a
// deferred context under the debug layer, another device's immediate context): only this one's calls are counted. Followed to
// the device the game presents with (FollowPresentDevice), so a device the game makes again is watched too. Set at Install and
// on the present thread only.
ID3D11DeviceContext* g_context = nullptr;
std::atomic<uint64_t> g_contextId = 0; // g_context, as the core's id for it; read by the hooks on the game's threads
// The watched context set render targets since the last present (it is still the one the game draws through).
std::atomic<uint64_t> g_watchedActivity = 0;

// The device of g_context, and the real device behind it when that is a proxy; a buffer of either can be copied on g_context.
// Not held (g_context holds its device). Under g_mutex.
ID3D11Device* g_contextDevice = nullptr;
ID3D11Device* g_contextRealDevice = nullptr;

// The copy of the picked buffer for the frame: one context means one copy, unlike the D3D12 finder's several command lists.
// Under g_mutex. Let go at stand-down and when the context changes. Never destroyed (no COM release at process exit).
native::DepthCopyDx11& g_copy = *new native::DepthCopyDx11();

// The finder's own pass is being recorded on this thread: its Dispatch is not the game's.
thread_local bool t_copying = false;

thread_local bool t_modExecuting = false;
bool TrackingInternal() { return t_copying || t_modExecuting || DrawObservationDx11::Internal(); }
std::atomic_bool g_installed{false}, g_installFailed{false};
std::atomic_flag g_installing = ATOMIC_FLAG_INIT;
struct ContextMethod {
    size_t index;
    bool (*covered)(PVOID) noexcept;
    LONG (*add)(HookOwnership::Own::Batch&,PVOID) noexcept;
};
template<auto& Method> constexpr ContextMethod MethodBinding() {
    return {std::remove_cvref_t<decltype(Method)>::method_index,
        +[](PVOID entry) noexcept {return Method.Covers(entry);},
        +[](HookOwnership::Own::Batch& batch,PVOID entry) noexcept {return Method.Add(batch,entry);}};
}
const std::array kCommonMethods {
    MethodBinding<o_DrawIndexed>(),
    MethodBinding<o_Draw>(),
    MethodBinding<o_DrawIndexedInstanced>(),
    MethodBinding<o_DrawInstanced>(),
    MethodBinding<o_OMSetRenderTargets>(),
    MethodBinding<o_OMSetRenderTargetsAndUAV>(),
    MethodBinding<o_DrawAuto>(),
    MethodBinding<o_DrawIndexedInstancedIndirect>(),
    MethodBinding<o_DrawInstancedIndirect>(),
    MethodBinding<o_Dispatch>(),
    MethodBinding<o_RSSetViewports>(),
    MethodBinding<o_ClearDepthStencilView>(),
    MethodBinding<o_DepthDispatchIndirect>()
};
const std::array kWriteMethods {
    MethodBinding<o_DepthMap>(),
    MethodBinding<o_DepthUnmap>(),
    MethodBinding<o_DepthCopyRegion>(),
    MethodBinding<o_DepthCopyResource>(),
    MethodBinding<o_DepthUpdate>(),
    MethodBinding<o_DepthCopyStructure>(),
    MethodBinding<o_ClearRtv>(),
    MethodBinding<o_ClearUavUint>(),
    MethodBinding<o_ClearUavFloat>(),
    MethodBinding<o_DepthMips>(),
    MethodBinding<o_DepthResolve>(),
    MethodBinding<o_ExecuteCommandList>(),
    MethodBinding<o_ClearState>(),
    MethodBinding<o_FinishCommandList>(),
    MethodBinding<o_DepthCopyRegion1>(),
    MethodBinding<o_DepthUpdate1>(),
    MethodBinding<o_DiscardResource>(),
    MethodBinding<o_DiscardView>(),
    MethodBinding<o_SwapDeviceContextState>(),
    MethodBinding<o_DepthClearView>(),
    MethodBinding<o_DiscardView1>()
};
template<size_t N> bool Covered(PVOID* table,const std::array<ContextMethod,N>& methods) noexcept {
    if(!table)return false;
    for(const auto& method:methods)if(!method.covered(table[method.index]))return false;
    return true;
}
bool CommonCovered(ID3D11DeviceContext* c) noexcept {
    return c && Covered(*reinterpret_cast<PVOID**>(c),kCommonMethods);
}
template<size_t N> bool InstallMethods(PVOID* table,const std::array<ContextMethod,N>& methods,const char* group) noexcept {
    if(Covered(table,methods))return true;
    HookOwnership::Own::Batch batch;
    for(const auto& method:methods)method.add(batch,table[method.index]);
    const auto result=batch.Commit();
    if(result!=NO_ERROR) {
        static std::atomic_uint reports{0};
        if(reports.fetch_add(1,std::memory_order_relaxed)<8)
            LOG_WARN("D3D11 context hooks: group={} result={}",group,result);
        return false;
    }
    LOG_INFO("D3D11 context hooks: covered {} implementations for {}",N,group);
    return Covered(table,methods);
}
// All callbacks observe the immediate base pointer. Alternate aliases still
// require their own original routing and are not certified by a successful batch.
bool ModContextCovered(ID3D11DeviceContext* c, bool requireHooks=true) noexcept {
    if(!c || !CommonCovered(c))return false;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> one;
    if(FAILED(c->QueryInterface(IID_PPV_ARGS(&one))) || one.Get()!=c)return false;
    auto* table=*reinterpret_cast<PVOID**>(one.Get());
    if(requireHooks && !Covered(table,kWriteMethods))return false;
    const auto alias=[&]<class T>() {
        Microsoft::WRL::ComPtr<T> value;
        const auto hr=c->QueryInterface(IID_PPV_ARGS(&value));
        return hr==E_NOINTERFACE || (SUCCEEDED(hr) && value.Get()==c && CommonCovered(value.Get()));
    };
    return alias.template operator()<ID3D11DeviceContext2>() &&
           alias.template operator()<ID3D11DeviceContext3>() && alias.template operator()<ID3D11DeviceContext4>();
}
bool EnsureContextWriteHooks(ID3D11DeviceContext* context,bool coveredStateSwitch=false) noexcept;
bool Watched(ID3D11DeviceContext* context)
{
    return !TrackingInternal() && (uint64_t) (size_t) context == g_contextId.load(std::memory_order_relaxed);
}

// Watches `context` (already unwrapped) from now on, and lets go of the copy made for the one before.
void UseContext(ID3D11DeviceContext* context)
{
    if(g_context && g_context!=context && DrawObservationDx11::owner)DrawObservationDx11::Revoke();
    context->AddRef();

    if (g_context != nullptr)
        g_context->Release();

    g_context = context;

    HookCpuTiming::Scope r448StateWait(HookCpuTiming::Stage::StateLock);
    std::lock_guard lock(g_mutex);r448StateWait.Finish();
    HookCpuTiming::Scope r448StateHeld(HookCpuTiming::Stage::StateLocked);

    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    g_contextDevice = device;
    g_contextRealDevice = device;

    IUnknown* real = nullptr;

    if (device != nullptr && Util::CheckForRealObject(__FUNCTION__, device, &real))
        g_contextRealDevice = (ID3D11Device*) real;

    if (device != nullptr)
        device->Release(); // the context holds it

    g_copy.Release();
    g_contextId.store((uint64_t) (size_t) context, std::memory_order_relaxed);
}

// The depth buffer a depth-stencil view points at, as a DepthBuffer (plain data for the core). `*outResource` gets an
// addref'd ID3D11Resource the caller must Release (the copy, if taken, is made from it).
bool Describe(ID3D11DepthStencilView* view, native::DepthBuffer* out, ID3D11Resource** outResource)
{
    if (view == nullptr)
        return false;

    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);

    if (resource == nullptr)
        return false;

    ID3D11Texture2D* tex = nullptr;

    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&tex))) || tex == nullptr)
    {
        resource->Release();
        return false;
    }

    D3D11_TEXTURE2D_DESC desc {};
    tex->GetDesc(&desc);
    tex->Release();

    out->id = (uint64_t) (size_t) resource;
    out->width = desc.Width;
    out->height = desc.Height;
    out->format = (uint32_t) desc.Format;
    out->readOnlyDepth = false; // D3D11 has no read-only depth bind distinct from the view's own flags; not used by the pick

    *outResource = resource; // caller releases
    return true;
}

// Copies `resource` (the picked buffer) into the frame's copy on `context` (g_context, from a hook or Present). A buffer that
// is not copied leaves no copy for the frame: the copy of an earlier one is not offered for it.
void TakeSnapshot(ID3D11DeviceContext* context, ID3D11Resource* resource, const char* where)
{
    HookCpuTiming::Scope r448StateWait(HookCpuTiming::Stage::StateLock);
    std::lock_guard lock(g_mutex);r448StateWait.Finish();
    HookCpuTiming::Scope r448StateHeld(HookCpuTiming::Stage::StateLocked);

    static int calls = 0;

    // The first few at info; the rest at debug level: this runs under g_mutex in the game's bind hooks, and a log line
    // is a synchronous file write
    if (calls < 8)
        LOG_INFO("Depth finder (D3D11): TakeSnapshot call {} from {}, {:X}", calls, where, (size_t) resource);
    else if (calls % 200 == 0)
        LOG_DEBUG("Depth finder (D3D11): TakeSnapshot call {} from {}, {:X}", calls, where, (size_t) resource);

    ++calls;

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    resource->GetDevice(&device);
    const char* failed = nullptr;

    if (device == nullptr || (device.Get() != g_contextDevice && device.Get() != g_contextRealDevice))
    {
        g_copy.Forget();
        failed = "the picked buffer is of another device than the context watched";
    }
    else
    {
        t_copying = true;
        failed = g_copy.Take(context, resource, g_core.CurrentPick().reversed);
        t_copying = false;
    }

    static const char* logged = nullptr;

    if (failed != nullptr && failed != logged)
        LOG_WARN("Depth finder (D3D11): {}; no copy", failed);

    logged = failed;

    static bool loggedFirst = false;

    if (failed == nullptr && !loggedFirst)
    {
        loggedFirst = true;
        LOG_INFO("Depth finder (D3D11): first depth copy taken, {}x{}, {} R32_FLOAT", g_copy.Width(), g_copy.Height(),
                 g_copy.Samples() > 1 ? std::format("{} samples resolved (nearest) into", g_copy.Samples())
                 : g_copy.Converted() ? std::string("converted to")
                                      : std::string("copied straight into"));
    }
}

void OnBound(ID3D11DeviceContext* context, ID3D11DepthStencilView* view)
{
    native::DepthBuffer buffer;
    ID3D11Resource* resource = nullptr;
    const bool hadDepth = view != nullptr;
    const bool have = hadDepth && Describe(view, &buffer, &resource);

    const auto request = g_core.OnDepthBound((uint64_t) (size_t) context, hadDepth, have ? &buffer : nullptr);

    if (request.take)
        TakeSnapshot(context, (ID3D11Resource*) (size_t) request.id, "unbind");

    if (resource != nullptr)
        resource->Release();
}

void STDMETHODCALLTYPE hkOMSetRenderTargets(ID3D11DeviceContext* This, UINT NumViews,
                                            ID3D11RenderTargetView* const* ppRenderTargetViews,
                                            ID3D11DepthStencilView* pDepthStencilView)
{
    HookCpuTiming::HookScope r448Hook(HookCpuTiming::Hook::hkOMSetRenderTargets);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    if (Watched(This))
    {
        g_watchedActivity.fetch_add(1, std::memory_order_relaxed);

        if (g_core.Active())
            OnBound(This, pDepthStencilView);
    }

    r448Hook.Original([&]{return o_OMSetRenderTargets(This, NumViews, ppRenderTargetViews, pDepthStencilView);});
}

void STDMETHODCALLTYPE hkOMSetRenderTargetsAndUAV(ID3D11DeviceContext* This, UINT NumRTVs,
                                                  ID3D11RenderTargetView* const* ppRenderTargetViews,
                                                  ID3D11DepthStencilView* pDepthStencilView, UINT UAVStartSlot,
                                                  UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
                                                  const UINT* pUAVInitialCounts)
{
    HookCpuTiming::HookScope r448Hook(HookCpuTiming::Hook::hkOMSetRenderTargetsAndUAV);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    if (Watched(This))
    {
        g_watchedActivity.fetch_add(1, std::memory_order_relaxed);

        // KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL changes only the UAVs: the depth buffer bound stays bound.
        if (g_core.Active() && NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
            OnBound(This, pDepthStencilView);
    }

    r448Hook.Original([&]{return o_OMSetRenderTargetsAndUAV(This, NumRTVs, ppRenderTargetViews, pDepthStencilView, UAVStartSlot, NumUAVs,
                              ppUnorderedAccessViews, pUAVInitialCounts);});
}

void STDMETHODCALLTYPE hkClearDepthStencilView(ID3D11DeviceContext* This, ID3D11DepthStencilView* pView,
                                               UINT ClearFlags, FLOAT Depth, UINT8 Stencil)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(This);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::ViewWritten(This,pView);
    HookCpuTiming::HookScope r448Hook(HookCpuTiming::Hook::hkClearDepthStencilView);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    if (g_core.Active() && (ClearFlags & D3D11_CLEAR_DEPTH) != 0 && Watched(This))
    {
        native::DepthBuffer buffer;
        ID3D11Resource* resource = nullptr;

        if (Describe(pView, &buffer, &resource))
        {
            const auto request = g_core.OnDepthClear((uint64_t) (size_t) This, buffer, Depth);

            if (request.take)
                TakeSnapshot(This, resource, "clear");

            resource->Release();
        }
    }

    r448Hook.Original([&]{return o_ClearDepthStencilView(This, pView, ClearFlags, Depth, Stencil);});
    if(!TrackingInternal())taa::DepthCleared(This,pView,ClearFlags,Depth);
}


void STDMETHODCALLTYPE hkRSSetViewports(ID3D11DeviceContext* c,UINT count,const D3D11_VIEWPORT* views)
{
    if (g_core.Active() && Watched(c) && count && views) g_core.OnViewport(reinterpret_cast<uint64_t>(c),views[0].Width);
    o_RSSetViewports(c,count,views);
}
void STDMETHODCALLTYPE hkDraw(ID3D11DeviceContext* c, UINT count, UINT first)
{
    taa::Scope taaPass(c,{taa::Stage::Pixel,count},Watched(c));
    DrawObservationDx11::Scope observed(c, {DrawObservationDx11::Kind::Draw,count,first}, []() noexcept {return !TrackingInternal();});
    gimi::runtime::RasterReturnScope returned(c);
    if (gimi::runtime::Enabled() && !TrackingInternal() && gimi::runtime::TryDraw(c,count,first,o_Draw.Forward,&returned))
    { if(!taaPass.Capturing())taa::ConsumerWrite(c,nullptr,"draw consumed by external hook"); observed.Returned(DrawObservationDx11::Completion::Consumed); return; }
    if (Watched(c)) {g_core.OnDraw(reinterpret_cast<uint64_t>(c),count,1); native::lowlatency::OnFirstSubmit();}
    taa::ConsumerScope consumer(c,{false,count,first},Watched(c)&&!taaPass.Capturing());
    o_Draw(c,count,first); returned.Returned(); observed.Returned(); consumer.Returned(); taaPass.Returned();
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::StreamOutputDrawn(c,count);
}
void STDMETHODCALLTYPE hkDrawIndexed(ID3D11DeviceContext* c, UINT count, UINT first, INT base)
{
    taa::Scope taaPass(c,{taa::Stage::Pixel,count},Watched(c));
    DrawObservationDx11::Scope observed(c,{DrawObservationDx11::Kind::Indexed,count,first,base},[]() noexcept {return !TrackingInternal();});
    gimi::runtime::RasterReturnScope returned(c);
    if (gimi::runtime::Enabled() && !TrackingInternal() && gimi::runtime::TryDrawIndexed(c,count,first,base,o_DrawIndexed.Forward,&returned))
    { if(!taaPass.Capturing())taa::ConsumerWrite(c,nullptr,"draw consumed by external hook"); observed.Returned(DrawObservationDx11::Completion::Consumed); return; }
    if (Watched(c)) {g_core.OnDraw(reinterpret_cast<uint64_t>(c),count,1); native::lowlatency::OnFirstSubmit();}
    taa::ConsumerScope consumer(c,{true,count,first,1,0,base},Watched(c)&&!taaPass.Capturing());
    o_DrawIndexed(c,count,first,base); returned.Returned(); observed.Returned(); consumer.Returned(); taaPass.Returned();
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::StreamOutputDrawn(c,count);
}
void STDMETHODCALLTYPE hkDrawInstanced(ID3D11DeviceContext* c, UINT count, UINT instances, UINT first, UINT firstInstance)
{
    taa::Scope taaPass(c,{taa::Stage::Pixel,count,instances},Watched(c));
    DrawObservationDx11::Scope observed(c,{DrawObservationDx11::Kind::Instanced,count,first,0,instances,firstInstance},[]() noexcept {return !TrackingInternal();});
    if (Watched(c)) {g_core.OnDraw(reinterpret_cast<uint64_t>(c),count,instances); native::lowlatency::OnFirstSubmit();}
    taa::ConsumerScope consumer(c,{false,count,first,instances,firstInstance},Watched(c)&&!taaPass.Capturing());
    o_DrawInstanced(c,count,instances,first,firstInstance); observed.Returned(); consumer.Returned(); taaPass.Returned();
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::StreamOutputDrawn(c,count,instances);
}
void STDMETHODCALLTYPE hkDrawIndexedInstanced(ID3D11DeviceContext* c, UINT count, UINT instances, UINT first, INT base, UINT firstInstance)
{
    taa::Scope taaPass(c,{taa::Stage::Pixel,count,instances},Watched(c));
    DrawObservationDx11::Scope observed(c,{DrawObservationDx11::Kind::IndexedInstanced,count,first,base,instances,firstInstance},[]() noexcept {return !TrackingInternal();});
    gimi::runtime::RasterReturnScope returned(c);
    if (gimi::runtime::Enabled() && !TrackingInternal() && gimi::runtime::TryDrawIndexedInstanced(c,count,instances,first,base,firstInstance,o_DrawIndexedInstanced.Forward,&returned))
    { if(!taaPass.Capturing())taa::ConsumerWrite(c,nullptr,"draw consumed by external hook"); observed.Returned(DrawObservationDx11::Completion::Consumed); return; }
    if (Watched(c)) {g_core.OnDraw(reinterpret_cast<uint64_t>(c),count,instances); native::lowlatency::OnFirstSubmit();}
    taa::ConsumerScope consumer(c,{true,count,first,instances,firstInstance,base},Watched(c)&&!taaPass.Capturing());
    o_DrawIndexedInstanced(c,count,instances,first,base,firstInstance); returned.Returned(); observed.Returned(); consumer.Returned(); taaPass.Returned();
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::StreamOutputDrawn(c,count,instances);
}
void STDMETHODCALLTYPE hkDrawAuto(ID3D11DeviceContext* c)
{
    taa::Scope taaPass(c,{taa::Stage::Pixel,0,1,0,0,0,false},Watched(c));
    DrawObservationDx11::Scope observed(c,{DrawObservationDx11::Kind::Auto,0,0,0,1,0,false},[]() noexcept {return !TrackingInternal();});
    if (Watched(c)) g_core.OnIndirect(reinterpret_cast<uint64_t>(c),1);
    taa::ConsumerScope consumer(c,{},Watched(c)&&!taaPass.Capturing());
    o_DrawAuto(c); observed.Returned(); consumer.Returned(); taaPass.Returned();
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::StreamOutputUnknownDraw(c);
}
void STDMETHODCALLTYPE hkDrawIndexedInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT offset)
{
    taa::Scope taaPass(c,{taa::Stage::Pixel,0,1,0,0,0,false},Watched(c));
    DrawObservationDx11::Scope observed(c,{DrawObservationDx11::Kind::IndexedIndirect,0,0,0,1,0,false,args,offset},[]() noexcept {return !TrackingInternal();});
    if (Watched(c)) g_core.OnIndirect(reinterpret_cast<uint64_t>(c),1);
    taa::ConsumerScope consumer(c,{},Watched(c)&&!taaPass.Capturing());
    o_DrawIndexedInstancedIndirect(c,args,offset); observed.Returned(); consumer.Returned(); taaPass.Returned();
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::StreamOutputUnknownDraw(c);
}
void STDMETHODCALLTYPE hkDrawInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT offset)
{
    taa::Scope taaPass(c,{taa::Stage::Pixel,0,1,0,0,0,false},Watched(c));
    DrawObservationDx11::Scope observed(c,{DrawObservationDx11::Kind::Indirect,0,0,0,1,0,false,args,offset},[]() noexcept {return !TrackingInternal();});
    if (Watched(c)) g_core.OnIndirect(reinterpret_cast<uint64_t>(c),1);
    taa::ConsumerScope consumer(c,{},Watched(c)&&!taaPass.Capturing());
    o_DrawInstancedIndirect(c,args,offset); observed.Returned(); consumer.Returned(); taaPass.Returned();
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::StreamOutputUnknownDraw(c);
}
void STDMETHODCALLTYPE hkDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z)
{
    taa::Scope taaPass(c,{taa::Stage::Compute,0,1,x,y,z},Watched(c));
    if (DrawObservationDx11::NeedsDraw() && !TrackingInternal()) DrawObservationDx11::NonDrawWrite(c);
    if (g_core.Active() && Watched(c)) g_core.Counters().dispatches.fetch_add(1,std::memory_order_relaxed);
    if(!taaPass.Capturing()&&!TrackingInternal())taa::ConsumerDispatch(c);
    o_Dispatch(c,x,y,z);
    taaPass.Returned();
}
void STDMETHODCALLTYPE hkDepthDispatchIndirect(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT offset) {
    taa::Scope taaPass(c,{taa::Stage::Compute,0,1,0,0,0,false},Watched(c));
    if (DrawObservationDx11::NeedsDraw() && !TrackingInternal()) DrawObservationDx11::NonDrawWrite(c);
    if(!taaPass.Capturing()&&!TrackingInternal())taa::ConsumerDispatch(c);
    o_DepthDispatchIndirect(c,args,offset);taaPass.Returned();
}

void NoteWrite(ID3D11DeviceContext* c,ID3D11Resource* resource,bool invalidateConstants=true) {
    if (taa::NeedsHooks()) {if(invalidateConstants)taa::constants::Written(resource);else taa::constants::GeometryWrite(resource);}
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(c);
    if(!TrackingInternal())taa::ConsumerWrite(c,resource);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::Written(c,resource);
}
void STDMETHODCALLTYPE hkDepthCopyRegion1(ID3D11DeviceContext1* c,ID3D11Resource* d,UINT sub,UINT x,UINT y,UINT z,ID3D11Resource* s,UINT ss,const D3D11_BOX* box,UINT flags) {
    NoteWrite(c,d);
    auto payload=gimi::runtime::Enabled()&&!TrackingInternal()?gimi::runtime::CpuContentSubmission::Copy(c,d,s,sub,x,y,z,ss,box,flags,false):gimi::runtime::CpuContentSubmission{};
    o_DepthCopyRegion1(c,d,sub,x,y,z,s,ss,box,flags);if(payload)payload.Returned();if(!TrackingInternal())taa::ConsumerCopy(c,d,s,!sub&&!ss&&!x&&!y&&!z&&!box&&!flags);
}
void STDMETHODCALLTYPE hkDepthUpdate1(ID3D11DeviceContext1* c,ID3D11Resource* d,UINT sub,const D3D11_BOX* box,const void* data,UINT row,UINT slice,UINT flags) {
    NoteWrite(c,d,false);
    auto payload=gimi::runtime::Enabled()&&!TrackingInternal()?gimi::runtime::CpuContentSubmission::Update(c,d,sub,box,data,flags):gimi::runtime::CpuContentSubmission{};
    o_DepthUpdate1(c,d,sub,box,data,row,slice,flags);if(payload)payload.Returned();
    if (taa::NeedsHooks()) taa::constants::Updated(c,d,sub,box,data,flags);
}
void STDMETHODCALLTYPE hkDepthCopyResource(ID3D11DeviceContext* c,ID3D11Resource* d,ID3D11Resource* s) {
    NoteWrite(c,d);
    auto payload=gimi::runtime::Enabled()&&!TrackingInternal()?gimi::runtime::CpuContentSubmission::Copy(c,d,s):gimi::runtime::CpuContentSubmission{};
    o_DepthCopyResource(c,d,s);if(payload)payload.Returned();if(!TrackingInternal())taa::ConsumerCopy(c,d,s,true);
}
void STDMETHODCALLTYPE hkDepthCopyRegion(ID3D11DeviceContext* c,ID3D11Resource* d,UINT sub,UINT x,UINT y,UINT z,ID3D11Resource* s,UINT ss,const D3D11_BOX* box) {
    NoteWrite(c,d);
    auto payload=gimi::runtime::Enabled()&&!TrackingInternal()?gimi::runtime::CpuContentSubmission::Copy(c,d,s,sub,x,y,z,ss,box,0,false):gimi::runtime::CpuContentSubmission{};
    o_DepthCopyRegion(c,d,sub,x,y,z,s,ss,box);if(payload)payload.Returned();if(!TrackingInternal())taa::ConsumerCopy(c,d,s,!sub&&!ss&&!x&&!y&&!z&&!box);
}
void STDMETHODCALLTYPE hkDepthUpdate(ID3D11DeviceContext* c,ID3D11Resource* d,UINT sub,const D3D11_BOX* box,const void* data,UINT row,UINT slice) {
    NoteWrite(c,d,false);
    auto payload=gimi::runtime::Enabled()&&!TrackingInternal()?gimi::runtime::CpuContentSubmission::Update(c,d,sub,box,data,0):gimi::runtime::CpuContentSubmission{};
    o_DepthUpdate(c,d,sub,box,data,row,slice);if(payload)payload.Returned();
    if (taa::NeedsHooks()) taa::constants::Updated(c,d,sub,box,data);
}
void STDMETHODCALLTYPE hkDepthCopyStructure(ID3D11DeviceContext* c,ID3D11Buffer* d,UINT offset,ID3D11UnorderedAccessView* view) {
    NoteWrite(c,d);o_DepthCopyStructure(c,d,offset,view);
}
void NoteViewWrite(ID3D11DeviceContext* c,ID3D11View* view) {
    if(TrackingInternal())return;
    if(DrawObservationDx11::NeedsDraw())DrawObservationDx11::NonDrawWrite(c);
    if(view){Microsoft::WRL::ComPtr<ID3D11Resource> resource;view->GetResource(&resource);taa::ConsumerWrite(c,resource.Get());}
    if(gimi::runtime::Enabled())gimi::runtime::ViewWritten(c,view);
}
void STDMETHODCALLTYPE hkDiscardResource(ID3D11DeviceContext1* c,ID3D11Resource* resource) {
    NoteWrite(c,resource);o_DiscardResource(c,resource);
}
void STDMETHODCALLTYPE hkDiscardView(ID3D11DeviceContext1* c,ID3D11View* view) {
    NoteViewWrite(c,view);o_DiscardView(c,view);
}
void STDMETHODCALLTYPE hkDiscardView1(ID3D11DeviceContext1* c,ID3D11View* view,const D3D11_RECT* rects,UINT count) {
    NoteViewWrite(c,view);o_DiscardView1(c,view,rects,count);
}
void STDMETHODCALLTYPE hkDepthClearView(ID3D11DeviceContext1* c,ID3D11View* view,const FLOAT* values,const D3D11_RECT* rects,UINT count) {
    NoteViewWrite(c,view);o_DepthClearView(c,view,values,rects,count);
}

HRESULT STDMETHODCALLTYPE hkDepthMap(ID3D11DeviceContext* c,ID3D11Resource* r,UINT sub,D3D11_MAP type,UINT flags,D3D11_MAPPED_SUBRESOURCE* mapped)
{
    // A lost-device Unity caller can pass a null resource (PID55284). It is not
    // a valid Map request; do not forward it into the runtime or publish a write.
    // pMappedResource may legitimately be null for D3D11.3 default textures.
    if (!r) return E_INVALIDARG;
    const auto generation = gimi::runtime::Enabled() && !TrackingInternal() ? gimi::runtime::VertexMapStarting(c,r,sub,type) : 0;
    if(type!=D3D11_MAP_READ&&!TrackingInternal())taa::ConsumerWrite(c,r);
    if(taa::NeedsHooks()&&type!=D3D11_MAP_READ)taa::constants::GeometryWrite(r);
    const auto result = o_DepthMap(c,r,sub,type,flags,mapped);
    if (taa::NeedsHooks()) taa::constants::Mapped(c,r,sub,type,mapped,result);
    if (generation)
    {
        if (FAILED(result)) gimi::runtime::VertexMapFailed(c,r,sub,generation);
        else gimi::runtime::ConstantMapped(c,r,sub,type,flags,mapped,result,generation,false);
    }
    return result;
}
void STDMETHODCALLTYPE hkDepthUnmap(ID3D11DeviceContext* c,ID3D11Resource* r,UINT sub)
{
    if (DrawObservationDx11::NeedsDraw() && !TrackingInternal()) DrawObservationDx11::NonDrawWrite(c);
    const bool observe = gimi::runtime::Enabled() && !TrackingInternal();
    auto payload = observe || gimi::runtime::CpuContentSubmission::HasPendingMaps() ?
        gimi::runtime::CpuContentSubmission::Unmap(c,r,sub,observe) : gimi::runtime::CpuContentSubmission{};
    // GIMI can redirect Map to CPU staging. Freeze the actual driver bytes
    // after its mandatory writeback and before the real Unmap consumes them.
    if (taa::NeedsHooks()) taa::constants::BeforeUnmap(c,r,sub);
    o_DepthUnmap(c,r,sub);
    if (taa::NeedsHooks()) taa::constants::AfterUnmap(c,r,sub);
    if (payload) payload.Returned();
    if (observe) gimi::runtime::VertexUnmapped(c,r,sub);
}

void STDMETHODCALLTYPE hkDepthResolve(ID3D11DeviceContext* c,ID3D11Resource* d,UINT sub,ID3D11Resource* s,UINT ss,DXGI_FORMAT format) {
    if(!TrackingInternal())taa::ConsumerWrite(c,d);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::Written(c,d);
    o_DepthResolve(c,d,sub,s,ss,format);
}
void STDMETHODCALLTYPE hkDepthMips(ID3D11DeviceContext* c,ID3D11ShaderResourceView* view) {
    if(!TrackingInternal()&&view){Microsoft::WRL::ComPtr<ID3D11Resource> resource;view->GetResource(&resource);taa::ConsumerWrite(c,resource.Get());}
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::ViewWritten(c,view);
    o_DepthMips(c,view);
}
void STDMETHODCALLTYPE hkClearRtv(ID3D11DeviceContext* c,ID3D11RenderTargetView* view,const FLOAT* values) {
    if(!TrackingInternal()&&view){Microsoft::WRL::ComPtr<ID3D11Resource> resource;view->GetResource(&resource);taa::ConsumerWrite(c,resource.Get());}
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::ViewWritten(c,view);
    o_ClearRtv(c,view,values);
}
void STDMETHODCALLTYPE hkClearUavUint(ID3D11DeviceContext* c,ID3D11UnorderedAccessView* view,const UINT* values) {
    if(!TrackingInternal()&&view){Microsoft::WRL::ComPtr<ID3D11Resource> resource;view->GetResource(&resource);taa::ConsumerWrite(c,resource.Get());}
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::ViewWritten(c,view);
    o_ClearUavUint(c,view,values);
}
void STDMETHODCALLTYPE hkClearUavFloat(ID3D11DeviceContext* c,ID3D11UnorderedAccessView* view,const FLOAT* values) {
    if(!TrackingInternal()&&view){Microsoft::WRL::ComPtr<ID3D11Resource> resource;view->GetResource(&resource);taa::ConsumerWrite(c,resource.Get());}
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::ViewWritten(c,view);
    o_ClearUavFloat(c,view,values);
}
bool FollowPresentDevice(IDXGISwapChain* swapChain)
{
    static ID3D11Device* presentDevice = nullptr;         // the device presented with last, a reference held
    static ID3D11DeviceContext* presentContext = nullptr; // its immediate context, unwrapped, a reference held
    static int streak = 0;
    static uint64_t watchedActivity = 0;

    ID3D11Device* device = nullptr;

    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        return false;

    if (device != presentDevice)
    {
        streak = 0;
        watchedActivity = g_watchedActivity.load(std::memory_order_relaxed);
        SAFE_RELEASE(presentContext);
        SAFE_RELEASE(presentDevice);
        presentDevice = device;
        presentDevice->AddRef();

        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);

        if (context != nullptr)
        {
            ID3D11DeviceContext* real = nullptr;

            if (!Util::CheckForRealObject(__FUNCTION__, context, (IUnknown**) &real))
                real = context;

            presentContext = real;
            presentContext->AddRef();
            context->Release();
        }
    }

    device->Release();

    const auto activity = g_watchedActivity.load(std::memory_order_relaxed);
    const bool watchedBusy = activity != watchedActivity;
    watchedActivity = activity;

    if (presentContext == g_context && presentContext) { streak = 0; return true; }
    if (!presentContext || watchedBusy) { streak = 0; return false; }

    if (++streak < 3)
        return false;

    streak = 0;

    // A context whose functions are not the ones hooked is never seen: watching it would see nothing at all.
    if (!CommonCovered(presentContext))
    {
        static bool loggedUnhooked = false;

        if (!loggedUnhooked)
        {
            loggedUnhooked = true;
            LOG_WARN("Depth finder (D3D11): the game presents with another device, whose immediate context runs other "
                     "functions than the ones hooked; it is not watched");
        }

        return false;
    }

    LOG_INFO("Depth finder (D3D11): the game presents with another device now and draws nothing through the one watched; "
             "watching the new one's immediate context");
    UseContext(presentContext);
    return true;
}

void STDMETHODCALLTYPE hkClearState(ID3D11DeviceContext* c) {
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::OpaqueBoundary(c);
    o_ClearState(c);
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::StreamOutputCleared(c);
}
void STDMETHODCALLTYPE hkExecuteCommandList(ID3D11DeviceContext* c,ID3D11CommandList* list,BOOL restore) {
    taa::constants::GeometrySubmission geometrySubmission(taa::NeedsHooks()?c:nullptr);
    if(!TrackingInternal())taa::ConsumerWrite(c,nullptr,"opaque command-list submission");
    if (taa::NeedsHooks()) taa::constants::OpaqueWrite(c);
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::OpaqueBoundary(c);
    o_ExecuteCommandList(c,list,restore);
}
HRESULT STDMETHODCALLTYPE hkFinishCommandList(ID3D11DeviceContext* c,BOOL restore,ID3D11CommandList** list) {
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::OpaqueBoundary(c);
    return o_FinishCommandList(c,restore,list);
}
void STDMETHODCALLTYPE hkSwapDeviceContextState(ID3D11DeviceContext1* c,ID3DDeviceContextState* state,ID3DDeviceContextState** previous) {
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::OpaqueBoundary(c);
    const bool coveredStateSwitch=ModContextCovered(c);
    o_SwapDeviceContextState(c,state,previous);
    // Certify the implementation at the switch itself. A frame can enter an
    // unseen table, write a CB, then return to a known table before Present.
    if (taa::NeedsHooks()) EnsureContextWriteHooks(c,coveredStateSwitch);
    if (gimi::runtime::Enabled() && c==g_context) GenericDepthDx11::BindModRuntime(g_contextDevice);
}
void STDMETHODCALLTYPE hkModSOSetTargets(ID3D11DeviceContext* c,UINT count,ID3D11Buffer* const* buffers,const UINT* offsets) {
    o_ModSOSetTargets(c,count,buffers,offsets);
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::StreamOutputTargets(c,count,buffers,offsets);
}
// Both native and interop Present publish MOD coverage through BindModRuntime.
// Install only missing methods, including after MODs are enabled mid-session.
bool ModStreamOutputCovered(PVOID* table) noexcept {return table && o_ModSOSetTargets.Covers(table[37]);}
bool EnsureContextWriteHooks(ID3D11DeviceContext* context,bool coveredStateSwitch) noexcept
{
    if((!gimi::runtime::Enabled() && !taa::NeedsHooks()) || !context)return false;
    auto* table=*reinterpret_cast<PVOID**>(context);
    const bool writesCovered=ModContextCovered(context,false) && Covered(table,kWriteMethods);
    if(writesCovered && (!gimi::runtime::Enabled() || ModStreamOutputCovered(table)))return true;
    if(g_installing.test_and_set(std::memory_order_acquire))return writesCovered;
    struct Release {~Release(){g_installing.clear(std::memory_order_release);}} release;
    // An observed state switch executes under our already-covered context;
    // only the context's state implementation changes, with no intervening
    // resource command. Install its methods before returning to the caller.
    // Unobserved implementations still revoke this device's earlier mirrors.
    if(!writesCovered&&!coveredStateSwitch&&taa::NeedsHooks())taa::constants::OpaqueWrite(context);
    if(!InstallMethods(table,kCommonMethods,"draw/dispatch") || !ModContextCovered(context,false))return false;
    if(!InstallMethods(table,kWriteMethods,"resource writes"))return false;
    Microsoft::WRL::ComPtr<ID3D11Device> coveredDevice;context->GetDevice(&coveredDevice);
    LOG_INFO("TAA write coverage established: device=0x{:X} context=0x{:X} observed_state_switch={} revoked={}",reinterpret_cast<uintptr_t>(coveredDevice.Get()),reinterpret_cast<uintptr_t>(context),coveredStateSwitch,!writesCovered&&!coveredStateSwitch);
    if(gimi::runtime::Enabled() && !ModStreamOutputCovered(table)) {
        HookOwnership::Own::Batch so(HookOwnership::InstallRequirement::Optional);
        o_ModSOSetTargets.Add(so,table[37]);
        if(so.Commit()!=NO_ERROR)return false;
    }
    return ModContextCovered(context);
}
const gimi::runtime::HookServices kModHooks{
    +[](ID3D11DeviceContext* c,uint64_t& token) noexcept {if(c!=g_context || TrackingInternal())return false; token=t_modExecuting;t_modExecuting=true;return true;},
    +[](ID3D11DeviceContext* c) noexcept {return c==g_context && TrackingInternal();},
    +[](ID3D11DeviceContext*,uint64_t token) noexcept {t_modExecuting=token!=0;},
    +[](ID3D11DeviceContext* c,UINT count,UINT,INT,bool) noexcept {if(c!=g_context)return false;g_core.OnDraw(reinterpret_cast<uint64_t>(c),count,1);native::lowlatency::OnFirstSubmit();return true;},
    +[](ID3D11DeviceContext* c,UINT,UINT,INT,bool) noexcept {return c==g_context;},
    +[](ID3D11DeviceContext* c,UINT count,UINT,bool) noexcept {if(c!=g_context)return false;g_core.OnDraw(reinterpret_cast<uint64_t>(c),count,1);native::lowlatency::OnFirstSubmit();return true;},
    +[](ID3D11DeviceContext* c,UINT,UINT,bool) noexcept {return c==g_context;},
    nullptr,nullptr,nullptr,
    +[](ID3D11DeviceContext* c,UINT count,UINT instances,UINT,INT,UINT,bool) noexcept {if(c!=g_context)return false;g_core.OnDraw(reinterpret_cast<uint64_t>(c),count,instances);native::lowlatency::OnFirstSubmit();return true;},
    +[](ID3D11DeviceContext* c,UINT,UINT,UINT,INT,UINT,bool) noexcept {return c==g_context;}
};
} // namespace

namespace GenericDepthDx11 {
bool ModInternal() noexcept {return TrackingInternal();}
bool EnsureTaaWriteCoverage(ID3D11Device* device) noexcept {
    if(!device || !taa::NeedsHooks())return false;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> raw;
    device->GetImmediateContext(&raw);
    if(!raw)return false;
    ID3D11DeviceContext* real=nullptr;
    if(!Util::CheckForRealObject(__FUNCTION__,raw.Get(),(IUnknown**)&real))real=raw.Get();
    return EnsureContextWriteHooks(real);
}
bool Installed() {return g_installed;}
bool InstallFailed() {return g_installFailed;}
namespace {
void BindModRuntimePair(ID3D11Device* device, ID3D11DeviceContext* context,
                        gimi::runtime::CaptureSource source, const gimi::runtime::NativePresentSourcePtr& nativeSource = {}) noexcept {
    if (!gimi::runtime::Enabled() || !device || !context) return;
    const bool contextCovered = g_installed && EnsureContextWriteHooks(context);
    auto hooks=kModHooks;hooks.draw=o_Draw.Forward;hooks.drawIndexed=o_DrawIndexed.Forward;hooks.drawIndexedInstanced=o_DrawIndexedInstanced.Forward;
    gimi::runtime::DeviceCreated(device,context,hooks,contextCovered,source,nativeSource);
    gimi::runtime::StreamOutputCoverage(device,context,contextCovered &&
        ModStreamOutputCovered(*reinterpret_cast<PVOID**>(context)));
    const auto request=gimi::runtime::ClaimCreationHooks(device,context);
    if (!request.revision) return;
    const auto result=D3D11Hooks::InstallGimiCreationHooks(device);
    gimi::runtime::CompleteCreationHooks(device,context,request,result);
}
}
void BindModRuntime(ID3D11Device* device) noexcept {
    if (!gimi::runtime::Enabled() || !device) return;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context) return;
    ID3D11DeviceContext* real=nullptr;
    if (Util::CheckForRealObject(__FUNCTION__,context.Get(),(IUnknown**)&real)) context=real;
    Microsoft::WRL::ComPtr<ID3D11Device> owner;
    context->GetDevice(&owner);
    BindModRuntimePair(owner.Get(),context.Get(),gimi::runtime::CaptureSource::DeviceCreation);
}
void RevokeModStreamOutputCoverage() noexcept {
    if(gimi::runtime::Enabled() && g_context)gimi::runtime::StreamOutputCoverage(g_contextDevice,g_context,false);
}
void Install(ID3D11Device* device)
{
    const bool mods=gimi::runtime::Enabled();
    if(!device || (!mods && !taa::NeedsHooks() && !DrawObservationDx11::NeedsDraw() &&
        !Config::Instance()->DrawUiMaskEnabled.value_or_default() &&
        !Config::Instance()->DlssNrNativeDepthFinder.value_or_default()))return;
    if(g_installing.test_and_set(std::memory_order_acquire))return;
    struct Release {~Release(){g_installing.clear(std::memory_order_release);}} release;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> raw,context;
    device->GetImmediateContext(&raw);
    if(!raw){g_installFailed=true;return;}
    ID3D11DeviceContext* real=nullptr;
    if(!Util::CheckForRealObject(__FUNCTION__,raw.Get(),(IUnknown**)&real))real=raw.Get();
    context=real;
    auto* table=*reinterpret_cast<PVOID**>(context.Get());
    if(!InstallMethods(table,kCommonMethods,"draw/dispatch")){g_installFailed=true;return;}
    if((mods||taa::NeedsHooks())&&!Covered(table,kWriteMethods)) {
        // Establish write coverage before returning the device to the game.
        // Static geometry created next retains its observed initial contents;
        // implementations first seen later still revoke earlier mirrors.
        if(taa::NeedsHooks())taa::constants::OpaqueWrite(context.Get());
        if(!InstallMethods(table,kWriteMethods,"resource writes")){g_installFailed=true;return;}
    }
    // Device initialization proves function coverage only. The actual Present
    // owner follows a replacement context; auxiliary devices cannot steal it.
    if(!g_context)UseContext(context.Get());
    g_installed=true;g_installFailed=false;
    if(Config::Instance()->DlssNrNativeDepthFinder.value_or_default())
        g_core.Start([](const std::string& line){LOG_INFO("{}",line);});
}

namespace {
IDXGISwapChain* NativeChain(IDXGISwapChain* chain) {
    IDXGISwapChain* native = nullptr;
    return chain && Util::CheckForRealObject(__FUNCTION__,chain,(IUnknown**)&native) ? native : chain;
}
}
bool RegisterModPresentSource(IDXGISwapChain* chain, HWND window) noexcept {
    // Registration comes from the game-facing wrapper's creation window, not
    // whichever device happened to create first. Interop's hidden HWND is never
    // used to identify the game's source. Provider outputs fail the DX11 query.
    if (!chain || !window || window != Util::GetProcessWindow()) return false;
    DWORD pid=0; GetWindowThreadProcessId(window,&pid);
    if (pid != GetCurrentProcessId()) return false;
    try {
        chain=NativeChain(chain);
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        if (FAILED(chain->GetDevice(IID_PPV_ARGS(&device))) || !device) return false;
        device->GetImmediateContext(&context);
        if (!context) return false;
        ID3D11DeviceContext* real=nullptr;
        if(Util::CheckForRealObject(__FUNCTION__,context.Get(),(IUnknown**)&real)) context=real;
        if (context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
        context->GetDevice(device.ReleaseAndGetAddressOf());
        return gimi::runtime::SelectPresentSource(chain,device.Get(),context.Get());
    } catch (...) { gimi::runtime::StopAccepting(); return false; }
}
void ForgetModPresentSource(IDXGISwapChain* chain) noexcept {
    try { gimi::runtime::ForgetPresentSource(NativeChain(chain)); } catch (...) {}
}
gimi::runtime::NativePresentSourcePtr PrepareModPresent(IDXGISwapChain* swapChain) noexcept
{
    if (!gimi::runtime::NeedsPresent() || !swapChain) return {};
    try {
        auto source=gimi::runtime::AcquirePresentSource(NativeChain(swapChain));
        if (!source || !source->live.load(std::memory_order_acquire)) {
            gimi::runtime::Present(nullptr,nullptr,0,0,{}); // auxiliary: retirement only
            return {};
        }
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(swapChain->GetDesc(&desc))) return {};
        Install(source->device.Get());
        if (g_installed.load() && CommonCovered(source->context.Get()) && gimi::runtime::Enabled()) {
            if (g_context!=source->context.Get()) UseContext(source->context.Get());
            BindModRuntimePair(source->device.Get(),source->context.Get(),gimi::runtime::CaptureSource::PresentOwner,source);
        }
        gimi::runtime::Present(source->device.Get(),source->context.Get(),desc.BufferDesc.Width,desc.BufferDesc.Height,source);
        return source->live.load(std::memory_order_acquire) ? source : nullptr;
    } catch (...) { gimi::runtime::StopAccepting(); return {}; }
}

void RevokeSourceObservations() noexcept {
    DrawObservationDx11::Revoke();
    taa::RevokeOwner();
}

void OnPresent(IDXGISwapChain* swapChain)
{
    struct SourceEpoch {
        bool published=false;
        ~SourceEpoch(){if(!published)RevokeSourceObservations();}
    } sourceEpoch;
    if (!g_installed.load() && swapChain && (taa::NeedsHooks() || DrawObservationDx11::NeedsDraw() ||
        Config::Instance()->DrawUiMaskEnabled.value_or_default() || gimi::runtime::Enabled()))
    {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&device)))) Install(device.Get());
    }
    // Drain the existing diagnostics once per frame instead of every draw.
    // CPU binding publication follows the public context owner. Native recipe
    // revocation still disables all of its own GPU/query consumers unchanged.

    DXGI_SWAP_CHAIN_DESC desc {};
    const bool havePresentDevice=g_installed.load() && swapChain && SUCCEEDED(swapChain->GetDesc(&desc));
    // MOD owner selection is explicit per source chain; the depth watcher's
    // device-follow heuristic cannot confirm or discard creation candidates.
    if(gimi::runtime::NeedsPresent()) {
        const auto source=PrepareModPresent(swapChain);
        if (!source && gimi::runtime::Enabled()) return;
    } else if (havePresentDevice) {
        FollowPresentDevice(swapChain);
    }
    if (!havePresentDevice)
        return;

    taa::OnPresent(g_context,taa::NeedsHooks() && EnsureContextWriteHooks(g_context));
    DrawObservationDx11::Present(g_context);
    sourceEpoch.published=true;
    DrawUiMask::Present(swapChain,g_context);

    // The copy taken is NOT forgotten here: the mid-frame "unbind" events that call TakeSnapshot happen while the game renders
    // the frame that is only now finishing (well before this Present call), so forgetting it at the top of this same function
    // would wipe out the copy they just took, before anything downstream (BestSnapshot, the diagnostic below) ever reads it. It
    // is instead let go below when the finder stands down, and kept fresh by every TakeSnapshot (a buffer not copied leaves no
    // copy).

    // The overlay preview (not yet drawn for D3D11) or the motion step needs the depth copy taken.
    g_core.SetSnapshotsWanted((Config::Instance()->DlssNrNativeDepthOverlay.value_or_default() &&
                               Config::Instance()->DlssNrNativeDebugView.value_or_default()) ||
                              Config::Instance()->DlssNrNativeMotion.value_or_default());

    // A game that neither re-clears nor unbinds the picked buffer before Present (it stays bound across many frames) would
    // otherwise never offer a copy: the immediate context has no "list closes" moment the way a D3D12 command list does, so this
    // is its equivalent, once per presented frame.
    if (g_context != nullptr)
    {
        const auto request = g_core.FlushForPresent((uint64_t) (size_t) g_context);

        if (request.take)
            TakeSnapshot(g_context, (ID3D11Resource*) (size_t) request.id, "present");
    }

    g_core.BeginPresent(desc.BufferDesc.Width, desc.BufferDesc.Height);
    const bool stoodDown =
        g_core.EndPresent(desc.BufferDesc.Width, desc.BufferDesc.Height,
                          Config::Instance()->DlssNrNativeDepthWarmupFrames.value_or_default());

    // Self-diagnosis: a pick that never gets a copy despite wanting one is otherwise invisible until someone asks why the trust
    // mask never runs.
    static uint64_t noCopyStreak = 0;
    bool copyTakenNow = false;
    {
    HookCpuTiming::Scope r448StateWait(HookCpuTiming::Stage::StateLock);
    std::lock_guard lock(g_mutex);r448StateWait.Finish();
        copyTakenNow = g_copy.Taken();
    }
    const bool stillNoCopy = g_core.Armed() && g_core.CurrentPick().valid && !copyTakenNow &&
                            (Config::Instance()->DlssNrNativeMotion.value_or_default() ||
                             (Config::Instance()->DlssNrNativeDepthOverlay.value_or_default() &&
                              Config::Instance()->DlssNrNativeDebugView.value_or_default()));

    if (stillNoCopy)
    {
        if (++noCopyStreak % 300 == 0)
        {
            const auto pick = g_core.CurrentPick();
            const auto diag = g_core.Diagnose((uint64_t) (size_t) g_context);
            LOG_WARN("Depth finder (D3D11): {} frames wanting a copy with no copy taken. Picked {:X}. Context: {}, {}, "
                     "bound to {:X}, this stretch {} vertices / {} draws (floor {}), wanted={}",
                     noCopyStreak, pick.id, diag.hasContext ? "known" : "UNKNOWN",
                     diag.hasBoundBuffer ? "has a bound depth buffer" : "NOTHING BOUND", diag.boundResource,
                     diag.currentVertices, diag.currentDrawcalls, g_core.SnapshotFloor(), diag.wanted);
        }
    }
    else
    {
        noCopyStreak = 0;
    }

    // Stood down: nothing is copied while the game calls an upscaler, so what the copies need (and the device it was made on)
    // is let go until the finder wakes.
    if (stoodDown)
    {
    HookCpuTiming::Scope r448StateWait(HookCpuTiming::Stage::StateLock);
    std::lock_guard lock(g_mutex);r448StateWait.Finish();
        g_copy.Release();
    }
}

GenericDepthSelect::Pick CurrentPick() { return g_core.CurrentPick(); }

Snapshot BestSnapshot()
{
    HookCpuTiming::Scope r448StateWait(HookCpuTiming::Stage::StateLock);
    std::lock_guard lock(g_mutex);r448StateWait.Finish();
    const auto pick = g_core.CurrentPick();

    Snapshot snap;

    if (g_copy.Taken() && pick.valid)
    {
        snap.valid = true;
        snap.resource = g_copy.Copy();
        snap.typelessFormat = DXGI_FORMAT_R32_FLOAT;
        snap.viewFormat = DXGI_FORMAT_R32_FLOAT;
        snap.width = g_copy.Width();
        snap.height = g_copy.Height();
        snap.reversed = pick.reversed;
    }

    return snap;
}

void NoteUpscalerCall()
{
    // Noted with no finder installed too: the menu and the frame source must know the game has an upscaler either way.
    native::NoteGameUpscalerCall();

    if (g_installed && !native::OpticalFgNrEnabled())
        g_core.NoteUpscalerCall();
}

bool GameCallsUpscaler() { return native::GameUpscalerCalledRecently() || (g_installed && g_core.GameCallsUpscaler()); }
bool Armed() { return g_installed && g_core.Armed(); }

void DrawStatus()
{
    const bool wanted = Config::Instance()->DlssNrNativeDepthFinder.value_or_default();

    switch (DlssNrNativeMode::FinderFor(wanted, g_installed, g_installFailed))
    {
    case DlssNrNativeMode::Finder::Off:
        MenuText::TextDisabled("深度：关闭。NR 仅使用运动信息，不因游戏超分而让出控制。");
        return;
    case DlssNrNativeMode::Finder::NeedsRestart:
        MenuText::TextDisabled("深度：需要重启后启用搜索。");
        return;
    case DlssNrNativeMode::Finder::CouldNotStart:
        MenuText::TextDisabled("深度：搜索器启动失败，请查看日志；NR 仅使用运动信息。");
        return;
    case DlssNrNativeMode::Finder::Installed:
        break;
    }

    const auto pick = CurrentPick();
    const uint32_t warmup = Config::Instance()->DlssNrNativeDepthWarmupFrames.value_or_default();
    const char* untilRestart = wanted ? "" : " (finder off at next start)";

    if (g_core.GameCallsUpscaler())
        MenuText::TextDisabled("深度：游戏正在调用超分，搜索已暂停；可在游戏设置关闭超分。");
    else if (!g_core.Armed())
        MenuText::TextDisabled("深度：观察中（%llu/%u 帧）…%s",
                            (unsigned long long) (g_core.Presents() - g_core.WarmupStart()), warmup, untilRestart);
    else if (!pick.valid)
        MenuText::TextDisabled("深度：尚未找到，NR 仅使用运动信息%s。", untilRestart);
    else
        MenuText::Text("深度：选中 %ux%u%s%s", pick.width, pick.height, pick.reversed ? ", reversed-Z" : "",
                    untilRestart);

    if (ImGui::IsItemHovered() && pick.valid)
        MenuText::SetTooltip("格式 %u，候选资源见日志的 Depth finder 记录。", pick.format);
    else if (ImGui::IsItemHovered())
        MenuText::SetTooltip("候选资源见日志的 Depth finder 记录。");
}

void DrawAdvancedUi()
{
    auto* config = Config::Instance();
    bool finder = config->DlssNrNativeDepthFinder.value_or_default();

    if (SettingsUi::SwitchRow(MenuText::StableLabel("使用游戏深度（改善质量，需重启）##depthfinder11").c_str(), &finder))
        config->DlssNrNativeDepthFinder = finder;

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "%s", "Watches the game's immediate context (Direct3D 11) and picks the scene's depth buffer, so\n"
                  "NR and the stabiliser get depth as well as motion. Optional: without it they run on motion\n"
                  "only. It is also what notices the game calling its own upscaler: without it, nothing here\n"
                  "stands aside for that. Deferred contexts are not watched yet. Choosing a mode above turns\n"
                  "it on, Off turns it off. Applies at the next start: save the settings and restart the game.");
}
} // namespace GenericDepthDx11
