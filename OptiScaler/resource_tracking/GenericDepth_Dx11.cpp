#include "pch.h"
#include <resource_tracking/DrawObservation_Dx11.h>
#include <ui_mask/DrawUiMask.h>

#include <native/OpticalFgNrPolicy.h>
#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

#include "GenericDepth_Dx11.h"
#include <mods/gimi/GimiRuntime.h>
#include <hooks/D3D11_Hooks.h>


#include <Config.h>
#include <Util.h>
#include <dlssnr/DlssNr_NativeMode.h>

#include <native/DepthCopyDx11.h>
#include <native/DepthFinderCore.h>
#include <native/NativeLowLatency.h>

#include <detours/detours.h>

#include <imgui/imgui.h>
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

typedef void(STDMETHODCALLTYPE* PFN_OMSetRenderTargets)(ID3D11DeviceContext* This, UINT NumViews,
                                                        ID3D11RenderTargetView* const* ppRenderTargetViews,
                                                        ID3D11DepthStencilView* pDepthStencilView);
typedef void(STDMETHODCALLTYPE* PFN_OMSetRenderTargetsAndUnorderedAccessViews)(
    ID3D11DeviceContext* This, UINT NumRTVs, ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView, UINT UAVStartSlot, UINT NumUAVs,
    ID3D11UnorderedAccessView* const* ppUnorderedAccessViews, const UINT* pUAVInitialCounts);
typedef void(STDMETHODCALLTYPE* PFN_ClearDepthStencilView)(ID3D11DeviceContext* This, ID3D11DepthStencilView* pView,
                                                           UINT ClearFlags, FLOAT Depth, UINT8 Stencil);
typedef void(STDMETHODCALLTYPE* PFN_RSSetViewports)(ID3D11DeviceContext* This, UINT NumViewports,
                                                    const D3D11_VIEWPORT* pViewports);
typedef void(STDMETHODCALLTYPE* PFN_Draw)(ID3D11DeviceContext* This, UINT VertexCount, UINT StartVertexLocation);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexed)(ID3D11DeviceContext* This, UINT IndexCount, UINT StartIndexLocation,
                                                 INT BaseVertexLocation);
typedef void(STDMETHODCALLTYPE* PFN_DrawInstanced)(ID3D11DeviceContext* This, UINT VertexCountPerInstance,
                                                   UINT InstanceCount, UINT StartVertexLocation,
                                                   UINT StartInstanceLocation);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(ID3D11DeviceContext* This, UINT IndexCountPerInstance,
                                                          UINT InstanceCount, UINT StartIndexLocation,
                                                          INT BaseVertexLocation, UINT StartInstanceLocation);
typedef void(STDMETHODCALLTYPE* PFN_DrawAuto)(ID3D11DeviceContext* This);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndirect)(ID3D11DeviceContext* This, ID3D11Buffer* pBufferForArgs,
                                                  UINT AlignedByteOffsetForArgs);
typedef void(STDMETHODCALLTYPE* PFN_Dispatch)(ID3D11DeviceContext* This, UINT X, UINT Y, UINT Z);
typedef void(STDMETHODCALLTYPE* PFN_SetConstantBuffers)(ID3D11DeviceContext* This, UINT StartSlot,
                                                         UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers);

PFN_OMSetRenderTargets o_OMSetRenderTargets = nullptr;
PFN_OMSetRenderTargetsAndUnorderedAccessViews o_OMSetRenderTargetsAndUAV = nullptr;
PFN_ClearDepthStencilView o_ClearDepthStencilView = nullptr;
PFN_RSSetViewports o_RSSetViewports = nullptr;
using PFN_ClearRtv=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11RenderTargetView*,const FLOAT*);
using PFN_ClearUavUint=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11UnorderedAccessView*,const UINT*);
using PFN_ClearUavFloat=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11UnorderedAccessView*,const FLOAT*);
PFN_ClearRtv o_ClearRtv=nullptr;PFN_ClearUavUint o_ClearUavUint=nullptr;PFN_ClearUavFloat o_ClearUavFloat=nullptr;
using PFN_CreateDepthCs=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11ComputeShader**);
PFN_CreateDepthCs o_CreateDepthCs=nullptr;
using PFN_CopyStructure=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Buffer*,UINT,ID3D11UnorderedAccessView*);
PFN_CopyStructure o_DepthCopyStructure=nullptr;
using PFN_CopyResource=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,ID3D11Resource*);
using PFN_CopyRegion=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,UINT,UINT,UINT,ID3D11Resource*,UINT,const D3D11_BOX*);
using PFN_Update=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,const D3D11_BOX*,const void*,UINT,UINT);
using PFN_Map=HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*);
using PFN_Unmap=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT);
using PFN_Resolve=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,ID3D11Resource*,UINT,DXGI_FORMAT);
using PFN_Mips=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11ShaderResourceView*);
using PFN_CopyRegion1=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*,ID3D11Resource*,UINT,UINT,UINT,UINT,ID3D11Resource*,UINT,const D3D11_BOX*,UINT);
using PFN_Update1=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*,ID3D11Resource*,UINT,const D3D11_BOX*,const void*,UINT,UINT,UINT);
using PFN_ClearView=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*,ID3D11View*,const FLOAT*,const D3D11_RECT*,UINT);
PFN_CopyRegion1 o_DepthCopyRegion1=nullptr;PFN_Update1 o_DepthUpdate1=nullptr;PFN_ClearView o_DepthClearView=nullptr;
PFN_DrawIndirect o_DepthDispatchIndirect=nullptr;
PFN_CopyResource o_DepthCopyResource=nullptr;PFN_CopyRegion o_DepthCopyRegion=nullptr;PFN_Update o_DepthUpdate=nullptr;
PFN_Map o_DepthMap=nullptr;PFN_Unmap o_DepthUnmap=nullptr;PFN_Resolve o_DepthResolve=nullptr;PFN_Mips o_DepthMips=nullptr;
PFN_Draw o_Draw = nullptr;
PFN_DrawIndexed o_DrawIndexed = nullptr;
PFN_DrawInstanced o_DrawInstanced = nullptr;
PFN_DrawIndexedInstanced o_DrawIndexedInstanced = nullptr;
PFN_DrawAuto o_DrawAuto = nullptr;
PFN_DrawIndirect o_DrawIndexedInstancedIndirect = nullptr;
PFN_DrawIndirect o_DrawInstancedIndirect = nullptr;
PFN_Dispatch o_Dispatch = nullptr;
PFN_SetConstantBuffers o_VSSetConstantBuffers = nullptr;
PFN_SetConstantBuffers o_PSSetConstantBuffers = nullptr;
PFN_SetConstantBuffers o_CSSetConstantBuffers = nullptr;
using PFN_ClearState = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using PFN_ExecuteCommandList = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using PFN_FinishCommandList = HRESULT (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
using PFN_DiscardResource = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11Resource*);
using PFN_DiscardView = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11View*);
using PFN_DiscardView1 = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11View*, const D3D11_RECT*, UINT);
using PFN_SwapDeviceContextState = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3DDeviceContextState*, ID3DDeviceContextState**);
PFN_ClearState o_ClearState = nullptr;
PFN_ExecuteCommandList o_ExecuteCommandList = nullptr;
PFN_FinishCommandList o_FinishCommandList = nullptr;
PFN_DiscardResource o_DiscardResource = nullptr;
PFN_DiscardView o_DiscardView = nullptr;
PFN_DiscardView1 o_DiscardView1 = nullptr;
PFN_SwapDeviceContextState o_SwapDeviceContextState = nullptr;
// ID3D11DeviceContext vtable indices (checked against the SDK header with offsetof; see tests/ -- a host-only sanity program,
// not shipped): DrawIndexed 12, Draw 13, DrawIndexedInstanced 20, DrawInstanced 21, OMSetRenderTargets 33,
// OMSetRenderTargetsAndUnorderedAccessViews 34, SOSetTargets 37, DrawAuto 38, DrawIndexedInstancedIndirect 39, DrawInstancedIndirect 40,
// Dispatch 41, RSSetViewports 44, ClearDepthStencilView 53. The constant-buffer
// setter indices below are pinned to the same Windows SDK ID3D11DeviceContext
// declaration: VS 7, PS 16 and CS 71.
constexpr int kDrawIndexed = 12, kDraw = 13, kDrawIndexedInstanced = 20, kDrawInstanced = 21, kOMSetRenderTargets = 33,
             kOMSetRenderTargetsAndUAV = 34, kSOSetTargets = 37, kDrawAuto = 38, kDrawIndexedInstancedIndirect = 39,
             kDrawInstancedIndirect = 40, kDispatch = 41, kRSSetViewports = 44, kClearDepthStencilView = 53,
             kVSSetConstantBuffers = 7, kPSSetConstantBuffers = 16, kCSSetConstantBuffers = 71;

// These process-wide function targets are immutable after the first common
// installation. A later bridge upgrade must use the same complete hook set.
constexpr std::array<int,12> kCommonHookSlots {kDrawIndexed,kDraw,kDrawIndexedInstanced,kDrawInstanced,
    kOMSetRenderTargets,kOMSetRenderTargetsAndUAV,kDrawAuto,kDrawIndexedInstancedIndirect,
    kDrawInstancedIndirect,kDispatch,kRSSetViewports,kClearDepthStencilView};
std::array<PVOID,kCommonHookSlots.size()> g_commonHookTargets {};

std::mutex g_mutex;
native::DepthFinderCore g_core;

// The context the game draws its frames through: its device's immediate context, unwrapped from a wrapping layer's proxy
// (Streamline's), a reference held. The hooks patch the functions themselves, so every context of the same kind runs them (a
// deferred context under the debug layer, another device's immediate context): only this one's calls are counted. Followed to
// the device the game presents with (FollowPresentDevice), so a device the game makes again is watched too. Set at Install and
// on the present thread only.
ID3D11DeviceContext* g_context = nullptr;
std::atomic<uint64_t> g_contextId = 0; // g_context, as the core's id for it; read by the hooks on the game's threads
PVOID g_hookedOMSetRenderTargets = nullptr; // the function hooked, to tell whether a context runs the hooks at all
// The watched context set render targets since the last present (it is still the one the game draws through).
std::atomic<bool> g_watchedBound = false;

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
std::atomic_bool g_installed{false}, g_installFailed{false}, g_modWriteHooksComplete{false};
std::atomic_flag g_installing = ATOMIC_FLAG_INIT;
std::array<HookOwnership::Own::Slot,12> commonOwners;
std::array<HookOwnership::Own::Slot,21> writeOwners;
HookOwnership::Own::Slot soOwner;
constexpr std::array<int,21> writeSlots{14,15,46,47,48,49,50,51,52,54,57,58,110,114,115,116,117,118,131,132,133};
std::atomic_bool soComplete{false};
template<size_t N> bool Covered(PVOID* table, const std::array<int,N>& slots,
    const std::array<HookOwnership::Own::Slot,N>& owners) noexcept
{
    if(!table)return false;
    for(size_t i=0;i<N;++i) {
        const auto& own=owners[i];
        if(!own.live || own.entry!=table[slots[i]] || !own.executed || *own.executed!=own.transaction)return false;
    }
    return true;
}
bool CommonCovered(ID3D11DeviceContext* c) noexcept {
    return c && Covered(*reinterpret_cast<PVOID**>(c),kCommonHookSlots,commonOwners);
}
// All callbacks observe the immediate base pointer. Reject an alternate alias
// or dispatch table rather than certifying methods these callbacks cannot see.
bool ModContextCovered(ID3D11DeviceContext* c, bool requireHooks=true) noexcept {
    if(!c || !CommonCovered(c))return false;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> one;
    if(FAILED(c->QueryInterface(IID_PPV_ARGS(&one))) || one.Get()!=c)return false;
    auto* table=*reinterpret_cast<PVOID**>(one.Get());
    if(requireHooks && !Covered(table,writeSlots,writeOwners))return false;
    const auto alias=[&]<class T>() {
        Microsoft::WRL::ComPtr<T> value;
        const auto hr=c->QueryInterface(IID_PPV_ARGS(&value));
        return hr==E_NOINTERFACE || (SUCCEEDED(hr) && value.Get()==c && CommonCovered(value.Get()));
    };
    return alias.template operator()<ID3D11DeviceContext2>() &&
           alias.template operator()<ID3D11DeviceContext3>() && alias.template operator()<ID3D11DeviceContext4>();
}
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
        g_watchedBound.store(true, std::memory_order_relaxed);

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
        g_watchedBound.store(true, std::memory_order_relaxed);

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
}


void STDMETHODCALLTYPE hkRSSetViewports(ID3D11DeviceContext* c,UINT count,const D3D11_VIEWPORT* views)
{
    if (g_core.Active() && Watched(c) && count && views) g_core.OnViewport(reinterpret_cast<uint64_t>(c),views[0].Width);
    o_RSSetViewports(c,count,views);
}
void STDMETHODCALLTYPE hkDraw(ID3D11DeviceContext* c, UINT count, UINT first)
{
    DrawObservationDx11::Scope observed(c, {DrawObservationDx11::Kind::Draw,count,first}, []() noexcept {return !TrackingInternal();});
    gimi::runtime::RasterReturnScope returned(c);
    if (gimi::runtime::Enabled() && !TrackingInternal() && gimi::runtime::TryDraw(c,count,first,o_Draw,&returned))
    { observed.Returned(DrawObservationDx11::Completion::Consumed); return; }
    if (Watched(c)) {g_core.OnDraw(reinterpret_cast<uint64_t>(c),count,1); native::lowlatency::OnFirstSubmit();}
    o_Draw(c,count,first); returned.Returned(); observed.Returned();
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::StreamOutputDrawn(c,count);
}
void STDMETHODCALLTYPE hkDrawIndexed(ID3D11DeviceContext* c, UINT count, UINT first, INT base)
{
    DrawObservationDx11::Scope observed(c,{DrawObservationDx11::Kind::Indexed,count,first,base},[]() noexcept {return !TrackingInternal();});
    gimi::runtime::RasterReturnScope returned(c);
    if (gimi::runtime::Enabled() && !TrackingInternal() && gimi::runtime::TryDrawIndexed(c,count,first,base,o_DrawIndexed,&returned))
    { observed.Returned(DrawObservationDx11::Completion::Consumed); return; }
    if (Watched(c)) {g_core.OnDraw(reinterpret_cast<uint64_t>(c),count,1); native::lowlatency::OnFirstSubmit();}
    o_DrawIndexed(c,count,first,base); returned.Returned(); observed.Returned();
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::StreamOutputDrawn(c,count);
}
void STDMETHODCALLTYPE hkDrawInstanced(ID3D11DeviceContext* c, UINT count, UINT instances, UINT first, UINT firstInstance)
{
    DrawObservationDx11::Scope observed(c,{DrawObservationDx11::Kind::Instanced,count,first,0,instances,firstInstance},[]() noexcept {return !TrackingInternal();});
    if (Watched(c)) {g_core.OnDraw(reinterpret_cast<uint64_t>(c),count,instances); native::lowlatency::OnFirstSubmit();}
    o_DrawInstanced(c,count,instances,first,firstInstance); observed.Returned();
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::StreamOutputDrawn(c,count,instances);
}
void STDMETHODCALLTYPE hkDrawIndexedInstanced(ID3D11DeviceContext* c, UINT count, UINT instances, UINT first, INT base, UINT firstInstance)
{
    DrawObservationDx11::Scope observed(c,{DrawObservationDx11::Kind::IndexedInstanced,count,first,base,instances,firstInstance},[]() noexcept {return !TrackingInternal();});
    gimi::runtime::RasterReturnScope returned(c);
    if (gimi::runtime::Enabled() && !TrackingInternal() && gimi::runtime::TryDrawIndexedInstanced(c,count,instances,first,base,firstInstance,o_DrawIndexedInstanced,&returned))
    { observed.Returned(DrawObservationDx11::Completion::Consumed); return; }
    if (Watched(c)) {g_core.OnDraw(reinterpret_cast<uint64_t>(c),count,instances); native::lowlatency::OnFirstSubmit();}
    o_DrawIndexedInstanced(c,count,instances,first,base,firstInstance); returned.Returned(); observed.Returned();
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::StreamOutputDrawn(c,count,instances);
}
void STDMETHODCALLTYPE hkDrawAuto(ID3D11DeviceContext* c)
{
    DrawObservationDx11::Scope observed(c,{DrawObservationDx11::Kind::Auto,0,0,0,1,0,false},[]() noexcept {return !TrackingInternal();});
    if (Watched(c)) g_core.OnIndirect(reinterpret_cast<uint64_t>(c),1);
    o_DrawAuto(c); observed.Returned();
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::StreamOutputUnknownDraw(c);
}
void STDMETHODCALLTYPE hkDrawIndexedInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT offset)
{
    DrawObservationDx11::Scope observed(c,{DrawObservationDx11::Kind::IndexedIndirect,0,0,0,1,0,false,args,offset},[]() noexcept {return !TrackingInternal();});
    if (Watched(c)) g_core.OnIndirect(reinterpret_cast<uint64_t>(c),1);
    o_DrawIndexedInstancedIndirect(c,args,offset); observed.Returned();
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::StreamOutputUnknownDraw(c);
}
void STDMETHODCALLTYPE hkDrawInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT offset)
{
    DrawObservationDx11::Scope observed(c,{DrawObservationDx11::Kind::Indirect,0,0,0,1,0,false,args,offset},[]() noexcept {return !TrackingInternal();});
    if (Watched(c)) g_core.OnIndirect(reinterpret_cast<uint64_t>(c),1);
    o_DrawInstancedIndirect(c,args,offset); observed.Returned();
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::StreamOutputUnknownDraw(c);
}
void STDMETHODCALLTYPE hkDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z)
{
    if (DrawObservationDx11::NeedsDraw() && !TrackingInternal()) DrawObservationDx11::NonDrawWrite(c);
    if (g_core.Active() && Watched(c)) g_core.Counters().dispatches.fetch_add(1,std::memory_order_relaxed);
    o_Dispatch(c,x,y,z);
}

void NoteWrite(ID3D11DeviceContext* c,ID3D11Resource* resource) {
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(c);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::Written(c,resource);
}
void STDMETHODCALLTYPE hkDepthCopyRegion1(ID3D11DeviceContext1* c,ID3D11Resource* d,UINT sub,UINT x,UINT y,UINT z,ID3D11Resource* s,UINT ss,const D3D11_BOX* box,UINT flags) {
    NoteWrite(c,d);
    auto payload=gimi::runtime::Enabled()&&!TrackingInternal()?gimi::runtime::CpuContentSubmission::Copy(c,d,s,sub,x,y,z,ss,box,flags,false):gimi::runtime::CpuContentSubmission{};
    o_DepthCopyRegion1(c,d,sub,x,y,z,s,ss,box,flags);if(payload)payload.Returned();
}
void STDMETHODCALLTYPE hkDepthUpdate1(ID3D11DeviceContext1* c,ID3D11Resource* d,UINT sub,const D3D11_BOX* box,const void* data,UINT row,UINT slice,UINT flags) {
    NoteWrite(c,d);
    auto payload=gimi::runtime::Enabled()&&!TrackingInternal()?gimi::runtime::CpuContentSubmission::Update(c,d,sub,box,data,flags):gimi::runtime::CpuContentSubmission{};
    o_DepthUpdate1(c,d,sub,box,data,row,slice,flags);if(payload)payload.Returned();
}
void STDMETHODCALLTYPE hkDepthCopyResource(ID3D11DeviceContext* c,ID3D11Resource* d,ID3D11Resource* s) {
    NoteWrite(c,d);
    auto payload=gimi::runtime::Enabled()&&!TrackingInternal()?gimi::runtime::CpuContentSubmission::Copy(c,d,s):gimi::runtime::CpuContentSubmission{};
    o_DepthCopyResource(c,d,s);if(payload)payload.Returned();
}
void STDMETHODCALLTYPE hkDepthCopyRegion(ID3D11DeviceContext* c,ID3D11Resource* d,UINT sub,UINT x,UINT y,UINT z,ID3D11Resource* s,UINT ss,const D3D11_BOX* box) {
    NoteWrite(c,d);
    auto payload=gimi::runtime::Enabled()&&!TrackingInternal()?gimi::runtime::CpuContentSubmission::Copy(c,d,s,sub,x,y,z,ss,box,0,false):gimi::runtime::CpuContentSubmission{};
    o_DepthCopyRegion(c,d,sub,x,y,z,s,ss,box);if(payload)payload.Returned();
}
void STDMETHODCALLTYPE hkDepthUpdate(ID3D11DeviceContext* c,ID3D11Resource* d,UINT sub,const D3D11_BOX* box,const void* data,UINT row,UINT slice) {
    NoteWrite(c,d);
    auto payload=gimi::runtime::Enabled()&&!TrackingInternal()?gimi::runtime::CpuContentSubmission::Update(c,d,sub,box,data,0):gimi::runtime::CpuContentSubmission{};
    o_DepthUpdate(c,d,sub,box,data,row,slice);if(payload)payload.Returned();
}
void STDMETHODCALLTYPE hkDepthCopyStructure(ID3D11DeviceContext* c,ID3D11Buffer* d,UINT offset,ID3D11UnorderedAccessView* view) {
    NoteWrite(c,d);o_DepthCopyStructure(c,d,offset,view);
}
void NoteViewWrite(ID3D11DeviceContext* c,ID3D11View* view) {
    if(TrackingInternal())return;
    if(DrawObservationDx11::NeedsDraw())DrawObservationDx11::NonDrawWrite(c);
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
    const auto generation = gimi::runtime::Enabled() && !TrackingInternal() ? gimi::runtime::VertexMapStarting(c,r,sub,type) : 0;
    const auto result = o_DepthMap(c,r,sub,type,flags,mapped);
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
    o_DepthUnmap(c,r,sub);
    if (payload) payload.Returned();
    if (observe) gimi::runtime::VertexUnmapped(c,r,sub);
}

void STDMETHODCALLTYPE hkDepthResolve(ID3D11DeviceContext* c,ID3D11Resource* d,UINT sub,ID3D11Resource* s,UINT ss,DXGI_FORMAT format) {
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::Written(c,d);
    o_DepthResolve(c,d,sub,s,ss,format);
}
void STDMETHODCALLTYPE hkDepthMips(ID3D11DeviceContext* c,ID3D11ShaderResourceView* view) {
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::ViewWritten(c,view);
    o_DepthMips(c,view);
}
void STDMETHODCALLTYPE hkClearRtv(ID3D11DeviceContext* c,ID3D11RenderTargetView* view,const FLOAT* values) {
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::ViewWritten(c,view);
    o_ClearRtv(c,view,values);
}
void STDMETHODCALLTYPE hkClearUavUint(ID3D11DeviceContext* c,ID3D11UnorderedAccessView* view,const UINT* values) {
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::ViewWritten(c,view);
    o_ClearUavUint(c,view,values);
}
void STDMETHODCALLTYPE hkClearUavFloat(ID3D11DeviceContext* c,ID3D11UnorderedAccessView* view,const FLOAT* values) {
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::ViewWritten(c,view);
    o_ClearUavFloat(c,view,values);
}
void FollowPresentDevice(IDXGISwapChain* swapChain)
{
    static ID3D11Device* presentDevice = nullptr;         // the device presented with last, a reference held
    static ID3D11DeviceContext* presentContext = nullptr; // its immediate context, unwrapped, a reference held
    static int streak = 0;

    ID3D11Device* device = nullptr;

    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        return;

    if (device != presentDevice)
    {
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

    const bool watchedBusy = g_watchedBound.exchange(false, std::memory_order_relaxed);

    if (presentContext == nullptr || presentContext == g_context || watchedBusy)
    {
        streak = 0;
        return;
    }

    if (++streak < 3)
        return;

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

        return;
    }

    LOG_INFO("Depth finder (D3D11): the game presents with another device now and draws nothing through the one watched; "
             "watching the new one's immediate context");
    UseContext(presentContext);
}

void STDMETHODCALLTYPE hkClearState(ID3D11DeviceContext* c) {
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::OpaqueBoundary(c);
    o_ClearState(c);
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::StreamOutputCleared(c);
}
void STDMETHODCALLTYPE hkExecuteCommandList(ID3D11DeviceContext* c,ID3D11CommandList* list,BOOL restore) {
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::OpaqueBoundary(c);
    o_ExecuteCommandList(c,list,restore);
}
HRESULT STDMETHODCALLTYPE hkFinishCommandList(ID3D11DeviceContext* c,BOOL restore,ID3D11CommandList** list) {
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::OpaqueBoundary(c);
    return o_FinishCommandList(c,restore,list);
}
void STDMETHODCALLTYPE hkSwapDeviceContextState(ID3D11DeviceContext1* c,ID3DDeviceContextState* state,ID3DDeviceContextState** previous) {
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::OpaqueBoundary(c);
    o_SwapDeviceContextState(c,state,previous);
    if (gimi::runtime::Enabled() && c==g_context) GenericDepthDx11::BindModRuntime(g_contextDevice);
}
using PFN_ModSOSetTargets = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,ID3D11Buffer* const*,const UINT*);
PFN_ModSOSetTargets o_ModSOSetTargets = nullptr;
void STDMETHODCALLTYPE hkModSOSetTargets(ID3D11DeviceContext* c,UINT count,ID3D11Buffer* const* buffers,const UINT* offsets) {
    o_ModSOSetTargets(c,count,buffers,offsets);
    if (gimi::runtime::Enabled() && !TrackingInternal()) gimi::runtime::StreamOutputTargets(c,count,buffers,offsets);
}
// Both native and interop Present publish MOD coverage through BindModRuntime.
// Install only missing methods, including after MODs are enabled mid-session.
bool ModStreamOutputCovered(PVOID* table) noexcept
{
    return soComplete && soOwner.live && soOwner.entry==table[37] &&
        soOwner.executed && *soOwner.executed==soOwner.transaction;
}
bool EnsureModContextHooks(ID3D11DeviceContext* context) noexcept
{
    if(!gimi::runtime::Enabled() || !ModContextCovered(context,false))return false;
    auto* table=*reinterpret_cast<PVOID**>(context);
    const bool writesCovered=g_modWriteHooksComplete && Covered(table,writeSlots,writeOwners);
    if(writesCovered && ModStreamOutputCovered(table))return true;
    if(g_installing.test_and_set(std::memory_order_acquire))return writesCovered;
    struct Release {~Release(){g_installing.clear(std::memory_order_release);}} release;
    if(!writesCovered)
    {
        HookOwnership::Own::Batch writes;
        size_t next=0;
        const auto write=[&](auto& original,unsigned index,auto hook) {
            writes.Add(writeOwners[next++],reinterpret_cast<PVOID*>(&original),nullptr,table[index],reinterpret_cast<PVOID>(hook));
        };
        write(o_DepthMap,14,hkDepthMap);write(o_DepthUnmap,15,hkDepthUnmap);
        write(o_DepthCopyRegion,46,hkDepthCopyRegion);write(o_DepthCopyResource,47,hkDepthCopyResource);
        write(o_DepthUpdate,48,hkDepthUpdate);write(o_DepthCopyStructure,49,hkDepthCopyStructure);
        write(o_ClearRtv,50,hkClearRtv);write(o_ClearUavUint,51,hkClearUavUint);write(o_ClearUavFloat,52,hkClearUavFloat);
        write(o_DepthMips,54,hkDepthMips);write(o_DepthResolve,57,hkDepthResolve);
        write(o_ExecuteCommandList,58,hkExecuteCommandList);write(o_ClearState,110,hkClearState);write(o_FinishCommandList,114,hkFinishCommandList);
        write(o_DepthCopyRegion1,115,hkDepthCopyRegion1);write(o_DepthUpdate1,116,hkDepthUpdate1);
        write(o_DiscardResource,117,hkDiscardResource);write(o_DiscardView,118,hkDiscardView);
        write(o_SwapDeviceContextState,131,hkSwapDeviceContextState);write(o_DepthClearView,132,hkDepthClearView);
        write(o_DiscardView1,133,hkDiscardView1);
        g_modWriteHooksComplete=writes.Commit()==NO_ERROR;
    }
    if(!g_modWriteHooksComplete || !Covered(table,writeSlots,writeOwners))return false;
    if(!ModStreamOutputCovered(table))
    {
        HookOwnership::Own::Batch so(HookOwnership::InstallRequirement::Optional);
        so.Add(soOwner,reinterpret_cast<PVOID*>(&o_ModSOSetTargets),nullptr,table[37],reinterpret_cast<PVOID>(hkModSOSetTargets));
        soComplete=so.Commit()==NO_ERROR;
    }
    return true;
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
bool Installed() {return g_installed;}
bool InstallFailed() {return g_installFailed;}
void BindModRuntime(ID3D11Device* device) noexcept {
    if(!gimi::runtime::Enabled() || device!=g_contextDevice || !g_context)return;
    const bool contextCovered=g_installed && EnsureModContextHooks(g_context);
    auto hooks=kModHooks;hooks.draw=o_Draw;hooks.drawIndexed=o_DrawIndexed;hooks.drawIndexedInstanced=o_DrawIndexedInstanced;
    gimi::runtime::DeviceCreated(device,g_context,hooks,contextCovered);
    gimi::runtime::StreamOutputCoverage(device,g_context,contextCovered &&
        ModStreamOutputCovered(*reinterpret_cast<PVOID**>(g_context)));
}
void CompleteModCreationHookInstall(ID3D11Device* device) noexcept {
    if(!gimi::runtime::Enabled() || device!=g_contextDevice || !g_context)return;
    const auto request=gimi::runtime::ClaimCreationHooks(device,g_context);
    if(!request.revision)return;
    const auto result=D3D11Hooks::InstallGimiCreationHooks(device);
    gimi::runtime::CompleteCreationHooks(device,g_context,request,result);
}
void RevokeModStreamOutputCoverage() noexcept {
    if(gimi::runtime::Enabled() && g_context)gimi::runtime::StreamOutputCoverage(g_contextDevice,g_context,false);
}
void Install(ID3D11Device* device)
{
    const bool mods=gimi::runtime::Enabled();
    if(!device || (!mods && !DrawObservationDx11::NeedsDraw() &&
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
    {
        HookOwnership::Own::Batch common;
        size_t next=0;
        const auto add=[&](auto& original,unsigned index,auto hook) {
            common.Add(commonOwners[next++],reinterpret_cast<PVOID*>(&original),nullptr,table[index],reinterpret_cast<PVOID>(hook));
        };
        add(o_DrawIndexed,12,hkDrawIndexed);add(o_Draw,13,hkDraw);add(o_DrawIndexedInstanced,20,hkDrawIndexedInstanced);
        add(o_DrawInstanced,21,hkDrawInstanced);add(o_OMSetRenderTargets,33,hkOMSetRenderTargets);
        add(o_OMSetRenderTargetsAndUAV,34,hkOMSetRenderTargetsAndUAV);add(o_DrawAuto,38,hkDrawAuto);
        add(o_DrawIndexedInstancedIndirect,39,hkDrawIndexedInstancedIndirect);add(o_DrawInstancedIndirect,40,hkDrawInstancedIndirect);
        add(o_Dispatch,41,hkDispatch);add(o_RSSetViewports,44,hkRSSetViewports);add(o_ClearDepthStencilView,53,hkClearDepthStencilView);
        if(common.Commit()!=NO_ERROR){g_installFailed=true;return;}
    }
    if(!CommonCovered(context.Get())){g_installFailed=true;return;}
    for(size_t i=0;i<kCommonHookSlots.size();++i)g_commonHookTargets[i]=table[kCommonHookSlots[i]];
    g_hookedOMSetRenderTargets=table[33];
    // Device initialization proves function coverage only. The actual Present
    // owner follows a replacement context; auxiliary devices cannot steal it.
    if(!g_context)UseContext(context.Get());
    g_installed=true;g_installFailed=false;
    if(Config::Instance()->DlssNrNativeDepthFinder.value_or_default())
        g_core.Start([](const std::string& line){LOG_INFO("{}",line);});
}

void PrepareModPresent(IDXGISwapChain* swapChain) noexcept
{
    if (!gimi::runtime::NeedsPresent() || !swapChain) return;
    try
    {
        // Use this exact native chain's owners, never State's backend device or
        // the previously watched context. OnPresent keeps its existing device
        // following/depth accounting; it is not called early a second time.
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))) || !device ||
            FAILED(swapChain->GetDesc(&desc))) return;
        device->GetImmediateContext(&context);
        if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
        ID3D11DeviceContext* real=nullptr;
        if(Util::CheckForRealObject(__FUNCTION__,context.Get(),(IUnknown**)&real))context=real;
        Install(device.Get());
        if (g_installed.load() && gimi::runtime::Enabled() &&
            device.Get() == g_contextDevice && context.Get() == g_context)
        {
            BindModRuntime(device.Get());
            CompleteModCreationHookInstall(device.Get());
        }
        gimi::runtime::Present(device.Get(), context.Get(), desc.BufferDesc.Width, desc.BufferDesc.Height);
    }
    catch (...) { gimi::runtime::StopAccepting(); }
}

void OnPresent(IDXGISwapChain* swapChain)
{
    if (!g_installed.load() && swapChain && (DrawObservationDx11::NeedsDraw() ||
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
    // Follow first: binding/installing against the previous frame's device
    // would publish creation coverage for an owner this Present just replaced.
    if(havePresentDevice)FollowPresentDevice(swapChain);
    if(gimi::runtime::NeedsPresent()){
        if(havePresentDevice && gimi::runtime::Enabled() && g_contextDevice && g_context){
            BindModRuntime(g_contextDevice);
            CompleteModCreationHookInstall(g_contextDevice);
        }
        // Invalid/null Present still retires retained submitted owners, without
        // loading a plan or installing against a stale watched device.
        gimi::runtime::Present(havePresentDevice?g_contextDevice:nullptr,havePresentDevice?g_context:nullptr,
                               desc.BufferDesc.Width,desc.BufferDesc.Height);
    }
    if (!havePresentDevice)
        return;

    DrawObservationDx11::Present(g_context, State::Instance().frameCount);
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
        ImGui::TextDisabled("深度：关闭。NR 仅使用运动信息，不因游戏超分而让出控制。");
        return;
    case DlssNrNativeMode::Finder::NeedsRestart:
        ImGui::TextDisabled("深度：需要重启后启用搜索。");
        return;
    case DlssNrNativeMode::Finder::CouldNotStart:
        ImGui::TextDisabled("深度：搜索器启动失败，请查看日志；NR 仅使用运动信息。");
        return;
    case DlssNrNativeMode::Finder::Installed:
        break;
    }

    const auto pick = CurrentPick();
    const uint32_t warmup = Config::Instance()->DlssNrNativeDepthWarmupFrames.value_or_default();
    const char* untilRestart = wanted ? "" : " (finder off at next start)";

    if (g_core.GameCallsUpscaler())
        ImGui::TextDisabled("深度：游戏正在调用超分，搜索已暂停；可在游戏设置关闭超分。");
    else if (!g_core.Armed())
        ImGui::TextDisabled("深度：观察中（%llu/%u 帧）…%s",
                            (unsigned long long) (g_core.Presents() - g_core.WarmupStart()), warmup, untilRestart);
    else if (!pick.valid)
        ImGui::TextDisabled("深度：尚未找到，NR 仅使用运动信息%s。", untilRestart);
    else
        ImGui::Text("深度：选中 %ux%u%s%s", pick.width, pick.height, pick.reversed ? ", reversed-Z" : "",
                    untilRestart);

    if (ImGui::IsItemHovered() && pick.valid)
        ImGui::SetTooltip("格式 %u，候选资源见日志的 Depth finder 记录。", pick.format);
    else if (ImGui::IsItemHovered())
        ImGui::SetTooltip("候选资源见日志的 Depth finder 记录。");
}

void DrawAdvancedUi()
{
    auto* config = Config::Instance();
    bool finder = config->DlssNrNativeDepthFinder.value_or_default();

    if (SettingsUi::SwitchRow("使用游戏深度（改善质量，需重启）##depthfinder11", &finder))
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
