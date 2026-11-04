// SPDX-License-Identifier: GPL-3.0-or-later
#include "GenshinLegacyCapture.h"
#include <resource_tracking/DrawObservation_Dx11.h>
#include <utility>
#include "GenshinPsStateCache.h"
#include "GenshinCpuWriterProbe.h"
#include "GenshinCpuStageTiming.h"
#include "GenshinHookCpuTiming.h"
#include "GenshinDepthProvider.h"
#include "GenshinDepthEvidence.h"
#include "GenshinReturnedRaster.h"
#include "GenshinCameraHistory.h"
#include "GenshinProducerTransport.h"
#include "GenshinRuntimeCapture.h"
#include "GenshinInputObserver.h"
#include "GenshinVisibleOutput.h"
#include "GenshinFsrUiObservation.h"
#include "GenshinLoadingProgress.h"
#include "GenshinFsrSubstageProgress.h"
#include <format>
#include <native/OpticalFgNrPolicy.h>
#include <integration/HoYoShade.h>
#include <inputs/FG/GenshinBridge_Present_Dx12.h>
#include <wrapped/wrapped_swapchain.h>
#include "legacy/Il2CppCallSiteHook.h"
#include <Config.h>
#include <genshin/GenshinFgConsumer.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <vector>
#include <list>
#pragma comment(lib,"d3dcompiler.lib")
#include <dxgi1_4.h>

namespace GenericDepthDx11 {void EmitPsCacheInstallReceipt() noexcept;}

namespace GenshinLegacy
{
using Microsoft::WRL::ComPtr;
using Action=GenshinFsr::NativeDrawAction;
InternalSrStateScope::InternalSrStateScope(ID3D11DeviceContext* context,ID3DDeviceContextState* scratch,
    ID3DDeviceContextState* saved,std::shared_ptr<GenshinTransport::ProducerFrame> producer,
    const GenshinFsr::CapturedFrame& frame) noexcept
    :previous_(active_),context_(context),scratch_(scratch),saved_(saved),producer_(std::move(producer)),
     resources_{frame.color,frame.depth,frame.motion,frame.output}
{
    // Shadow any reentrant scope with a refusal rather than lending its proof.
    active_=this;
    if(previous_ || !context_ || !scratch_ || !saved_ || scratch==saved ||
       !producer_ || !producer_->Live() || !frame.guides || producer_->Guides()!=frame.guides ||
       GenshinTransport::ProducerInvocation::Current()!=producer_ ||
       !GenshinTransport::PreparedSrOutput::PureSr())return;
    owner_=producer_->Owner();const auto& p=frame.parameters;
    valid_=owner_.device && owner_.deviceEpoch && owner_.producerEpoch && owner_.configEpoch &&
        owner_.context==reinterpret_cast<uint64_t>(context) && owner_.thread==GetCurrentThreadId() &&
        owner_.instance==p.instance && owner_.historyEpoch==p.epoch && owner_.generation==p.generation &&
        owner_.sourceFrame==p.sourceFrame && Config::Instance()->GenshinControlsCurrent(owner_.configEpoch) &&
        context->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE;
}
InternalSrStateScope::~InternalSrStateScope()
{
    if(active_==this)active_=previous_;
}
bool InternalSrStateScope::Consume(ID3D11DeviceContext* context,const GenshinTransport::Scope& scope,
    const void* parameters) noexcept
{
    auto* proof=active_;
    if(!proof || proof->consumed_)return false;
    proof->consumed_=true;
    return proof->valid_ && proof->context_.Get()==context && scope.parameters==parameters && parameters &&
        GenshinTransport::Scope::Current()==&scope && !scope.lease && scope.resources==proof->resources_ &&
        GenshinGuides::SameOwner(scope.owner,proof->owner_) && scope.owner.thread==GetCurrentThreadId() &&
        proof->producer_->Live() && GenshinTransport::ProducerInvocation::Current()==proof->producer_ &&
        Config::Instance()->GenshinControlsCurrent(scope.owner.configEpoch) &&
        GenshinTransport::PreparedSrOutput::PureSr();
}
// The two real postprocess edges used by FinishedPicture/Present NR.
// Keep their typed views here, independent of the removed diagnostic readbacks.
static constexpr uint64_t SceneShader=0x773B7BD7A2C6971Full,PictureShader=0xB58064BC5041A048ull;
struct PictureView {
    ComPtr<ID3D11Texture2D> texture;
    UINT subresource=0;
    bool valid=false;
};
static PictureView PictureTexture(ID3D11View* view,UINT mip,UINT slice) noexcept
{
    PictureView out;ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    if(!resource || FAILED(resource.As(&out.texture)))return out;
    D3D11_TEXTURE2D_DESC d{};out.texture->GetDesc(&d);
    if(mip>=d.MipLevels || mip>=32 || slice>=d.ArraySize || d.SampleDesc.Count!=1 || d.SampleDesc.Quality)return out;
    out.subresource=D3D11CalcSubresource(mip,slice,d.MipLevels);out.valid=true;return out;
}
static PictureView ReadPictureView(ID3D11ShaderResourceView* view) noexcept
{
    if(!view)return {};
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};view->GetDesc(&d);
    if(d.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D)return PictureTexture(view,d.Texture2D.MostDetailedMip,0);
    if(d.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2DARRAY && d.Texture2DArray.ArraySize==1)
        return PictureTexture(view,d.Texture2DArray.MostDetailedMip,d.Texture2DArray.FirstArraySlice);
    return {};
}
static PictureView ReadPictureView(ID3D11RenderTargetView* view) noexcept
{
    if(!view)return {};
    D3D11_RENDER_TARGET_VIEW_DESC d{};view->GetDesc(&d);
    if(d.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D)return PictureTexture(view,d.Texture2D.MipSlice,0);
    if(d.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2DARRAY && d.Texture2DArray.ArraySize==1)
        return PictureTexture(view,d.Texture2DArray.MipSlice,d.Texture2DArray.FirstArraySlice);
    return {};
}
static CaptureSignalProvider captureSignalProvider=nullptr;
static CapturePresentProvider capturePresentProvider=nullptr;
static void* captureProviderOpaque=nullptr;
static std::atomic<CaptureOnlyMode> captureOnlyMode{CaptureOnlyMode::Disabled};
static std::uint64_t captureMinimumGeneration=0;

// Fixed-slot capture and signed-square decoder come from the pinned legacy bridge.
// The Render method/token gate is mandatory: this layout alone never admits a frame.
struct Bindings
{
    std::array<ComPtr<ID3D11ShaderResourceView>,7> srvs;
    std::array<ComPtr<ID3D11RenderTargetView>,2> rtvs;
    std::array<ComPtr<ID3D11Texture2D>,7> inputs;
    std::array<ComPtr<ID3D11Texture2D>,2> outputs;
    std::array<ComPtr<ID3D11Resource>,2> outputResources;
    // Keep native allocation descriptors distinct from the effective view mip.
    std::array<D3D11_TEXTURE2D_DESC,7> inResourceDesc{},inDesc{};
    std::array<UINT,7> inputSubresource{};
    std::array<UINT,2> outputSubresource{};
    std::array<D3D11_TEXTURE2D_DESC,2> outResourceDesc{},outDesc{};
    std::array<D3D11_SHADER_RESOURCE_VIEW_DESC,7> inView{};
    std::array<D3D11_RENDER_TARGET_VIEW_DESC,2> outView{};
    std::array<uintptr_t,7> inputDevices{};
    std::array<uintptr_t,2> outputDevices{};
    ComPtr<ID3D11RasterizerState> rasterizer;
    bool scissorEnabled=false;
    UINT constantBytes=0;
    D3D11_VIEWPORT viewport{};
    DXGI_FORMAT colorFormat=DXGI_FORMAT_UNKNOWN,outputFormat=DXGI_FORMAT_UNKNOWN;
    unsigned colorSlot=0,depthSlot=2,motionSlot=3,outputSlot=1;
    ComPtr<ID3D11Buffer> cb;
    unsigned refusal=0;bool targetProfile=false,quiet=false;
    uintptr_t contextIdentity=0,deviceIdentity=0;
    uint64_t deviceEpoch=0,ownerEpoch=0,configEpoch=0;
    DWORD ownerThread=0;
};
static PictureView CachedPictureView(ID3D11View* current,ID3D11View* retained,
    ID3D11Texture2D* texture,const D3D11_TEXTURE2D_DESC& desc,
    bool texture2D,UINT mip,UINT slice) noexcept
{
    if(!current || current!=retained || !texture || !texture2D)return {};
    PictureView out;out.texture=texture;
    if(mip>=desc.MipLevels || mip>=32 || slice>=desc.ArraySize || desc.SampleDesc.Count!=1 || desc.SampleDesc.Quality)return {};
    out.subresource=D3D11CalcSubresource(mip,slice,desc.MipLevels);out.valid=true;return out;
}
enum class BindingFailure : unsigned { MissingMrt, ViewportOrCb, SrvView, SrvResource, RtvView, RtvResource,
    Layout, SelectedView, DepthMotionFormat, ColourFormat, OutputFormat, ResourceFormat, Scissor, Count };
// This is reached only at native draw boundaries, never from the menu. Bound the
// descriptor diagnostics by failure class and time, and exclude ordinary single-RTV draws.
static bool RejectBindings(ID3D11DeviceContext* context,Bindings& b,BindingFailure stage,unsigned slot=UINT_MAX)
{
    b.refusal=static_cast<unsigned>(stage)+1;
    if(b.quiet || !b.targetProfile || !il2cpp_callsite::render_call_count())return false;
    static std::array<uint64_t,static_cast<unsigned>(BindingFailure::Count)> last{};
    static unsigned published=0;const auto now=GetTickCount64();auto& tick=last[static_cast<unsigned>(stage)];
    if(published>=40 || (tick && now-tick<5000))return false;tick=now;++published;
    std::ostringstream out;
    out << "pid=" << GetCurrentProcessId() << " stage=" << static_cast<unsigned>(stage) << " slot=" << slot
        << " context=" << reinterpret_cast<uintptr_t>(context) << " vp=" << b.viewport.TopLeftX << "," << b.viewport.TopLeftY
        << "," << b.viewport.Width << "x" << b.viewport.Height;
    D3D11_BUFFER_DESC cb{};if(b.cb)b.cb->GetDesc(&cb);out << " cb0=" << cb.ByteWidth;
    for(unsigned i=0;i<7;++i){const auto& d=b.inResourceDesc[i];const auto& v=b.inView[i];
        out << " srv" << i << "=" << bool(b.srvs[i]) << ":" << d.Width << "x" << d.Height << " fmt=" << d.Format
            << "/" << v.Format << " dim=" << v.ViewDimension << " mips=" << d.MipLevels << ":" << v.Texture2D.MostDetailedMip
            << "+" << v.Texture2D.MipLevels << " array=" << d.ArraySize << " samples=" << d.SampleDesc.Count
            << " usage=" << d.Usage << " cpu=" << d.CPUAccessFlags << " misc=" << d.MiscFlags;}
    for(unsigned i=0;i<2;++i){const auto& d=b.outResourceDesc[i];const auto& v=b.outView[i];
        out << " rtv" << i << "=" << bool(b.rtvs[i]) << ":" << d.Width << "x" << d.Height << " fmt=" << d.Format
            << "/" << v.Format << " dim=" << v.ViewDimension << " mips=" << d.MipLevels << ":" << v.Texture2D.MipSlice
            << " array=" << d.ArraySize << " samples=" << d.SampleDesc.Count << " usage=" << d.Usage << " misc=" << d.MiscFlags;}
    LOG_INFO("Genshin internal FSR binding refused: {}",out.str());return false;
}
// Genshin accumulate only. The upstream recipe selects t0/t2/t3 and RTV1;
// LUT/history bindings belong to the native draw, not the selected SR inputs.
// Strong view references keep immutable metadata reusable across frames. New
// views are inspected before use; context, viewport and scissor stay live.
static bool ReadBindings(ID3D11DeviceContext* context,Bindings& b,const Bindings* retained=nullptr)
{
    ID3D11RenderTargetView* rtvs[2]{};
    context->OMGetRenderTargets(2,rtvs,nullptr);
    for(unsigned i=0;i<2;++i)b.rtvs[i].Attach(rtvs[i]);
    if(!b.rtvs[0] || !b.rtvs[1])return RejectBindings(context,b,BindingFailure::MissingMrt);
    if(retained && retained->refusal)retained=nullptr;

    ID3D11ShaderResourceView* srvs[7]{};
    if(b.targetProfile) {
        context->PSGetShaderResources(0,1,srvs);
        context->PSGetShaderResources(2,2,srvs+2);
    }else context->PSGetShaderResources(0,7,srvs); // Explicit capture-only observation.
    for(unsigned i=0;i<7;++i)b.srvs[i].Attach(srvs[i]);
    context->PSGetConstantBuffers(0,1,&b.cb);
    UINT count=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    context->RSGetViewports(&count,viewports);
    if(count)b.viewport=viewports[0];
    if(count!=1 || !b.cb || b.viewport.TopLeftX!=0 || b.viewport.TopLeftY!=0)
        return RejectBindings(context,b,BindingFailure::ViewportOrCb);
    if(retained && b.cb.Get()==retained->cb.Get())b.constantBytes=retained->constantBytes;
    else {D3D11_BUFFER_DESC d{};b.cb->GetDesc(&d);b.constantBytes=d.ByteWidth;}
    if(b.constantBytes<464)return RejectBindings(context,b,BindingFailure::ViewportOrCb);

    for(unsigned i=0;i<7;++i) {
        if(b.targetProfile && i!=0 && i!=2 && i!=3)continue;
        if(!b.srvs[i])continue;
        if(retained && b.srvs[i].Get()==retained->srvs[i].Get() && retained->inputs[i]) {
            b.inputs[i]=retained->inputs[i];b.inView[i]=retained->inView[i];
            b.inResourceDesc[i]=retained->inResourceDesc[i];b.inDesc[i]=retained->inDesc[i];
            b.inputSubresource[i]=retained->inputSubresource[i];b.inputDevices[i]=retained->inputDevices[i];
            continue;
        }
        auto& view=b.inView[i];b.srvs[i]->GetDesc(&view);
        ComPtr<ID3D11Resource> resource;b.srvs[i]->GetResource(&resource);
        if(!resource || FAILED(resource.As(&b.inputs[i])))
            return RejectBindings(context,b,BindingFailure::SrvResource,i);
        ComPtr<ID3D11Device> device;b.inputs[i]->GetDevice(&device);
        b.inputDevices[i]=reinterpret_cast<uintptr_t>(device.Get());
        auto& d=b.inResourceDesc[i];b.inputs[i]->GetDesc(&d);b.inDesc[i]=d;
        if(view.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D ||
           !NativeMrt::MipExtent(d,view.Texture2D.MostDetailedMip,b.inDesc[i].Width,b.inDesc[i].Height))
            return RejectBindings(context,b,BindingFailure::SrvView,i);
        b.inputSubresource[i]=D3D11CalcSubresource(view.Texture2D.MostDetailedMip,0,d.MipLevels);
        b.inDesc[i].MipLevels=b.inDesc[i].ArraySize=1;
    }
    for(unsigned i=0;i<2;++i) {
        if(retained && b.rtvs[i].Get()==retained->rtvs[i].Get() && retained->outputs[i]) {
            b.outputs[i]=retained->outputs[i];b.outputResources[i]=retained->outputResources[i];
            b.outView[i]=retained->outView[i];b.outResourceDesc[i]=retained->outResourceDesc[i];
            b.outDesc[i]=retained->outDesc[i];b.outputSubresource[i]=retained->outputSubresource[i];
            b.outputDevices[i]=retained->outputDevices[i];continue;
        }
        auto& view=b.outView[i];b.rtvs[i]->GetDesc(&view);
        b.rtvs[i]->GetResource(&b.outputResources[i]);
        if(!b.outputResources[i])return RejectBindings(context,b,BindingFailure::RtvResource,i);
        ComPtr<ID3D11Device> device;b.outputResources[i]->GetDevice(&device);
        b.outputDevices[i]=reinterpret_cast<uintptr_t>(device.Get());
        const bool texture=SUCCEEDED(b.outputResources[i].As(&b.outputs[i]));
        if(texture){b.outputs[i]->GetDesc(&b.outResourceDesc[i]);b.outDesc[i]=b.outResourceDesc[i];}
        // RTV0 is written by the preserved original draw. It is never imported,
        // sampled or copied by SR, and need not satisfy the SR output contract.
        if(i!=b.outputSlot)continue;
        auto& d=b.outResourceDesc[i];
        if(!texture || view.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D ||
           !NativeMrt::MipExtent(d,view.Texture2D.MipSlice,b.outDesc[i].Width,b.outDesc[i].Height))
            return RejectBindings(context,b,BindingFailure::RtvView,i);
        b.outputSubresource[i]=D3D11CalcSubresource(view.Texture2D.MipSlice,0,d.MipLevels);
        b.outDesc[i].MipLevels=b.outDesc[i].ArraySize=1;
    }
    for(const unsigned slot:{0u,2u,3u}) {
        NativeMrt::Surface view;
        view.resource=reinterpret_cast<uintptr_t>(b.inputs[slot].Get());
        view.view=reinterpret_cast<uintptr_t>(b.srvs[slot].Get());view.desc=b.inResourceDesc[slot];
        view.viewDimension=b.inView[slot].ViewDimension;view.firstMip=b.inView[slot].Texture2D.MostDetailedMip;
        view.mipCount=b.inView[slot].Texture2D.MipLevels;
        if(!NativeMrt::Full(view,false))return RejectBindings(context,b,BindingFailure::SelectedView,slot);
    }
    NativeMrt::Surface output;
    output.resource=reinterpret_cast<uintptr_t>(b.outputs[1].Get());output.view=reinterpret_cast<uintptr_t>(b.rtvs[1].Get());
    output.desc=b.outResourceDesc[1];output.viewDimension=b.outView[1].ViewDimension;output.firstMip=b.outView[1].Texture2D.MipSlice;
    if(!NativeMrt::Full(output,true))return RejectBindings(context,b,BindingFailure::RtvView,1);
    const auto& c=b.inDesc[0];const auto& o=b.outDesc[1];
    if(c.Width>o.Width || c.Height>o.Height || b.viewport.Width!=o.Width || b.viewport.Height!=o.Height ||
       b.inDesc[2].Width!=c.Width || b.inDesc[2].Height!=c.Height ||
       b.inDesc[3].Width!=c.Width || b.inDesc[3].Height!=c.Height)
        return RejectBindings(context,b,BindingFailure::SelectedView);
    b.colorFormat=b.inView[0].Format;b.outputFormat=b.outView[1].Format;
    if(!NativeMrt::ReadableDepth(b.inResourceDesc[2].Format,b.inView[2].Format) ||
       b.inView[3].Format!=DXGI_FORMAT_R10G10B10A2_UNORM)
        return RejectBindings(context,b,BindingFailure::DepthMotionFormat);
    if(b.colorFormat!=DXGI_FORMAT_R16G16B16A16_FLOAT && b.colorFormat!=DXGI_FORMAT_R11G11B10_FLOAT &&
       b.colorFormat!=DXGI_FORMAT_R8G8B8A8_UNORM && b.colorFormat!=DXGI_FORMAT_R10G10B10A2_UNORM)
        return RejectBindings(context,b,BindingFailure::ColourFormat);
    if(b.outputFormat!=b.colorFormat)return RejectBindings(context,b,BindingFailure::OutputFormat);
    const auto compatible=[](DXGI_FORMAT raw,DXGI_FORMAT view){return raw==view ||
        (raw==DXGI_FORMAT_R8G8B8A8_TYPELESS && view==DXGI_FORMAT_R8G8B8A8_UNORM) ||
        (raw==DXGI_FORMAT_R16G16B16A16_TYPELESS && view==DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        (raw==DXGI_FORMAT_R10G10B10A2_TYPELESS && view==DXGI_FORMAT_R10G10B10A2_UNORM);};
    if(!compatible(c.Format,b.colorFormat) || !compatible(o.Format,b.outputFormat) ||
       !compatible(b.inResourceDesc[3].Format,b.inView[3].Format))
        return RejectBindings(context,b,BindingFailure::ResourceFormat);
    context->RSGetState(&b.rasterizer);
    if(retained && b.rasterizer.Get()==retained->rasterizer.Get())b.scissorEnabled=retained->scissorEnabled;
    else if(b.rasterizer){D3D11_RASTERIZER_DESC d{};b.rasterizer->GetDesc(&d);b.scissorEnabled=d.ScissorEnable!=FALSE;}
    if(b.scissorEnabled) {
        count=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_RECT rects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        context->RSGetScissorRects(&count,rects);
        if(count!=1 || rects[0].left || rects[0].top || rects[0].right!=o.Width || rects[0].bottom!=o.Height)
            return RejectBindings(context,b,BindingFailure::Scissor);
    }
    return true;
}
// Functional repair proof only: never called just to sample a blocked target.
// Preserve the real native MRT draw. Refuse all extra RTV/DSV/UAV bindings,
// including non-aliasing ones, rather than assume their write/queue semantics.
static bool RepairBindingsSafe(ID3D11DeviceContext* context,const Bindings& b,ID3D11Texture2D* output)
{
    if(!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE || !output)return false;
    const auto privateId=GenshinPresentNr::Identity(output);
    const auto selectedId=GenshinPresentNr::Identity(b.outputResources[b.outputSlot].Get());
    const auto companionId=GenshinPresentNr::Identity(b.outputResources[1-b.outputSlot].Get());
    if(!privateId || !selectedId || !companionId || privateId==selectedId || privateId==companionId || selectedId==companionId)return false;
    // Paired repair inspects all current native inputs for aliasing. Ordinary
    // SR capture only needs t0/t2/t3; auxiliary descriptors stay off its hot path.
    ID3D11ShaderResourceView* inputs[7]{};context->PSGetShaderResources(0,7,inputs);
    bool inputSafe=true;
    for(auto* raw:inputs) {
        ComPtr<ID3D11ShaderResourceView> view;view.Attach(raw);
        if(!view)continue;
        ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
        const auto id=GenshinPresentNr::Identity(resource.Get());
        inputSafe=inputSafe && id && id!=privateId && id!=selectedId;
    }
    if(!inputSafe)return false;
    ComPtr<ID3D11RenderTargetView> rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11RenderTargetView* raw[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};ComPtr<ID3D11DepthStencilView> dsv;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,raw,&dsv);
    bool safe=!dsv;
    for(unsigned i=0;i<D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;++i){rtvs[i].Attach(raw[i]);
        safe=safe && (i<2?rtvs[i].Get()==b.rtvs[i].Get():!rtvs[i]);}
    ComPtr<ID3D11Device> device;context->GetDevice(&device);
    if(!safe)return false; // exactly two RTVs, no DSV proved above
    const UINT slots=device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?D3D11_1_UAV_SLOT_COUNT:D3D11_PS_CS_UAV_REGISTER_COUNT;
    ID3D11UnorderedAccessView* om[D3D11_1_UAV_SLOT_COUNT]{},*cs[D3D11_1_UAV_SLOT_COUNT]{};
    ID3D11RenderTargetView* pairedRtvs[2]{};
    // OM UAV slots 0/1 are occupied by the proved two RTVs. Match the
    // documented RTV-count/UAV-start contract; FL11_1 extends the limit to64.
    context->OMGetRenderTargetsAndUnorderedAccessViews(2,pairedRtvs,nullptr,2,slots-2,om);
    for(unsigned i=0;i<2;++i){ComPtr<ID3D11RenderTargetView> r;r.Attach(pairedRtvs[i]);safe=safe && r.Get()==b.rtvs[i].Get();}
    context->CSGetUnorderedAccessViews(0,slots,cs);
    for(unsigned i=0;i<slots;++i){ComPtr<ID3D11UnorderedAccessView> a,c;if(i<slots-2)a.Attach(om[i]);c.Attach(cs[i]);safe=safe && !a && !c;}
    return safe;
}
static bool SameRepairBindings(const Bindings& a,const Bindings& b) noexcept
{
    if(a.outputSlot!=b.outputSlot || a.cb.Get()!=b.cb.Get() ||
       a.viewport.TopLeftX!=b.viewport.TopLeftX || a.viewport.TopLeftY!=b.viewport.TopLeftY ||
       a.viewport.Width!=b.viewport.Width || a.viewport.Height!=b.viewport.Height ||
       a.viewport.MinDepth!=b.viewport.MinDepth || a.viewport.MaxDepth!=b.viewport.MaxDepth)return false;
    for(unsigned i=0;i<7;++i)if(a.srvs[i].Get()!=b.srvs[i].Get() || a.inputs[i].Get()!=b.inputs[i].Get() ||
        a.inputSubresource[i]!=b.inputSubresource[i])return false;
    for(unsigned i=0;i<2;++i)if(a.rtvs[i].Get()!=b.rtvs[i].Get() || a.outputResources[i].Get()!=b.outputResources[i].Get() ||
        a.outputSubresource[i]!=b.outputSubresource[i])return false;
    return true;
}
static unsigned MatchToken(const Bindings& b,il2cpp_callsite::RenderToken& result,uint64_t associated=0)
{
    return il2cpp_callsite::match_pending_render_token(b.inDesc[0].Width,b.inDesc[0].Height,
        b.outDesc[1].Width,b.outDesc[1].Height,associated,result);
}
// Optional collection is published at the real Present/claim owner boundary.
// Draws only check that owner and immediate consumer demand; resource admission,
// flight retirement and copyback still use their original live checks.
struct DrawObservationSubscription {
    ID3D11DeviceContext* context=nullptr;
    CapturePipeline* owner=nullptr;
    DWORD thread=0;
    uint64_t call=0,configEpoch=0,deviceEpoch=0,ownerEpoch=0;
    HoYoShade::ObservationDemand hoYo;
    bool srOnly=false,captureOff=false;
};
static thread_local DrawObservationSubscription drawObservation;
static thread_local DrawObservationSubscription ownerObservation; // actual owner thread, never cross-owner authority
static bool OptionalConsumersEffectivelyOff() noexcept;
static bool NonHoYoConsumersEffectivelyOff() noexcept;
static bool ObservationOnlyCurrent(ID3D11DeviceContext*,uint64_t) noexcept;
static bool RefreshClaimObservation(ID3D11DeviceContext*,uint64_t) noexcept;
static void RefreshOwnerObservation(ID3D11DeviceContext*,const ControlsSnapshot&) noexcept;
static bool OptionalGuideDemand() noexcept
{
    const auto& cfg=*Config::Instance();
    const bool fg=GenshinFgConsumer::NewWorkRequested(),nr=cfg.DlssNrEnabled.value_or_default();
    const unsigned demand=(fg?1u:0u)|(nr?2u:0u);
    static thread_local unsigned previous=UINT_MAX,reports=0;
    if(demand!=previous){
        previous=demand;
        if(reports++<32)try {
            LOG_INFO("R827 phase33 optional demand: pid={} thread={} fg={} nr={} mapping_requested={} admission_unchanged=true",
                GetCurrentProcessId(),GetCurrentThreadId(),fg,nr,demand!=0);
        }catch(...){}
    }
    return demand!=0;
}
// Carry the immutable demand snapshot on the actual submitted frame without
// copying its parameters/resources or widening the shared capture interface.
struct SrCaptureFrame final : GenshinFsr::CapturedFrame
{
    const bool cameraDemand;
    explicit SrCaptureFrame(bool demand) noexcept : cameraDemand(demand) {}
};
static NativeMrt::Frame NativeFrame(ID3D11Device* device,ID3D11DeviceContext* context,
    const Bindings& b,const il2cpp_callsite::RenderToken& token,const Family::Shader& shader,
    uint64_t deviceEpoch,uint64_t ownerEpoch,uint64_t configEpoch,uint64_t thread,uint64_t drawSerial,const Settings& settings)
{
    NativeMrt::Frame f;const auto& p=token.params;
    f.device=reinterpret_cast<uintptr_t>(device);f.context=reinterpret_cast<uintptr_t>(context);
    f.constantBuffer=reinterpret_cast<uintptr_t>(b.cb.Get());f.deviceEpoch=deviceEpoch;f.ownerEpoch=ownerEpoch;
    f.configEpoch=configEpoch;f.thread=thread;f.drawSerial=drawSerial;f.controls=settings;
    f.instance=p.instance;f.generation=token.generation;f.renderContext=p.context;f.tokenTick=token.capture_tick;
    f.frame=p.frame_index;f.rawJitterX=p.jitter_x;f.rawJitterY=p.jitter_y;
    f.renderWidth=p.render_w;f.renderHeight=p.render_h;f.outputWidth=p.display_w;f.outputHeight=p.display_h;
    f.shader=shader.hash;f.shaderIdentity=shader.identity;f.nrProfile=shader.nrProfile;f.immediate=context->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE;
    f.inputSlots={b.colorSlot,b.depthSlot,b.motionSlot};f.selectedOutput=b.outputSlot;
    for(unsigned i=0;i<3;++i){const auto slot=f.inputSlots[i];auto& v=f.inputs[i];
        v.resource=reinterpret_cast<uintptr_t>(b.inputs[slot].Get());v.view=reinterpret_cast<uintptr_t>(b.srvs[slot].Get());
        v.device=b.inputDevices[slot];
        v.desc=b.inResourceDesc[slot];v.viewFormat=b.inView[slot].Format;v.viewDimension=b.inView[slot].ViewDimension;
        v.firstMip=b.inView[slot].Texture2D.MostDetailedMip;v.mipCount=b.inView[slot].Texture2D.MipLevels;}
    for(unsigned i=0;i<2;++i){auto& v=f.outputs[i];v.resource=reinterpret_cast<uintptr_t>(b.outputResources[i].Get());
        v.view=reinterpret_cast<uintptr_t>(b.rtvs[i].Get());v.device=b.outputDevices[i];
        v.desc=b.outResourceDesc[i];v.viewFormat=b.outView[i].Format;
        v.viewDimension=b.outView[i].ViewDimension;v.firstMip=b.outView[i].Texture2D.MipSlice;}
    // Keep optional native evidence for requested FG (even before provider
    // readiness) or independent NR. Explicit NoFG alone creates no fresh
    // reference; existing GPU owners still retire through their original paths.
    // This observation does not weaken NativeGuideForPresent's numeric gates.
    if(GenshinFgConsumer::NewWorkRequested() || Config::Instance()->DlssNrEnabled.value_or_default())
        f.gameMotionReference=GenshinGameMotion::NativeReference::Observe(context,f);
    return f;
}
static constexpr char PrepareShader[]=R"(
Texture2D<float> InputDepth:register(t0);
Texture2D<float4> InputMotion:register(t1);
RWTexture2D<float> PreparedDepth:register(u0);
RWTexture2D<float2> PreparedMotion:register(u1);
cbuffer Settings:register(b0){uint depthFlipY;uint motionFlipY;uint2 unused;};
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID)
{
    uint w,h;InputMotion.GetDimensions(w,h);if(id.x>=w || id.y>=h)return;
    uint dy=depthFlipY?h-1-id.y:id.y;uint my=motionFlipY?h-1-id.y:id.y;
    PreparedDepth[id.xy]=InputDepth.Load(int3(id.x,dy,0));
    float2 d=InputMotion.Load(int3(id.x,my,0)).xy-(127.0/255.0);
    float2 magnitude=abs(d)*2.0;
    PreparedMotion[id.xy]=-sign(d)*magnitude*magnitude;
})";

struct CapturePipeline::Storage
{
    GenshinSubstage::Key substageKey{};
    uint64_t substageAttempt=0;
    bool substageOutputPending=false;
    GenshinPresentNr::Source presentNrSource{};
    GenshinPresentNr::SelectedContentAuthority presentNrContent;
    GenshinDepthMapping::Capture depthMapping;
    void NativeSelectedContent(uint64_t draw) noexcept {
        // A recognized original accumulate may overwrite either game S. No
        // blocked binding query is needed. Keep the private successful witness
        // for guarded paired repair, and retain all asynchronous GPU owners.
        presentNrContent.NativeAccumulate(draw);presentNrPictures.ClearOwner();depthMapping.Revoke();
    }
    GenshinPresentNr::PictureCadence presentNrPictures;
    // Scalars from the last acquired picture, never a retained/replayable ticket.
    GenshinBridgeFg::Key presentNrConsumedKey{};
    uint64_t presentNrConsumedAtDraw=0;
    uint64_t presentNrConsumedPicture=0,presentNrPairedCopies=0,presentNrPairedDraw=0;
    uintptr_t presentNrPairedOutput=0;
    // OBSERVATION-ONLY: bounded CPU operands from actual target draws; no bypass.
    uint64_t repairTargetDraw=0,repairPollBlocked=0,repairCanonical=0,repairCpuGeneration=0;
    uint32_t repairStage=0,repairMask=0,repairTokenMatches=0,pollStop=0,repairPollStop=0;
    GenshinPresentNrTrace::PendingRepairTrace pendingRepairTrace{};
    // Record only already-owned operands. No COM identity/binding queries here.
    void TraceOuterQuery(uint32_t kind,uint64_t instance,uint64_t generation,uint64_t sourceFrame,
                         ID3D11Texture2D* source,ID3D11Texture2D* destination,ID3D11Resource* resource,UINT subresource) noexcept {
        auto& q=pendingRepairTrace.outer;q={};q.seq=GenshinPresentNrTrace::Sequence();q.active=true;
        q.kind=kind;q.draw=status.observedDraws;q.instance=instance;q.generation=generation;q.sourceFrame=sourceFrame;
        q.ownerEpoch=ownerEpoch;q.deviceEpoch=deviceEpoch;q.configEpoch=configEpoch;q.thread=thread;
        q.query=reinterpret_cast<uintptr_t>(completion.Get());q.context=reinterpret_cast<uintptr_t>(context.Get());
        q.device=reinterpret_cast<uintptr_t>(device.Get());q.sourceTexture=reinterpret_cast<uintptr_t>(source);
        q.destinationTexture=reinterpret_cast<uintptr_t>(destination);q.destinationResource=reinterpret_cast<uintptr_t>(resource);
        q.subresource=subresource;
    }
    void TraceRepairTarget(uint32_t stage) noexcept {
        auto& t=pendingRepairTrace.target;t={};t.seq=GenshinPresentNrTrace::Sequence();t.draw=status.observedDraws;
        t.stage=stage;t.pollStop=pollStop;t.query=pendingRepairTrace.outer;
        t.gateQuerySeq=t.query.seq;t.gatePollSeq=t.query.pollSeq;
    }
    unsigned returnedDrawKind=0;
    static constexpr size_t SrInputSlotCount=GenshinTransport::GpuWorkDomain::RecordingCount;
    static_assert(SrInputSlotCount==3);
    struct PrepareControlSlot
    {
        ComPtr<ID3D11Buffer> constants;
        std::array<UINT,4> uploaded{};
        bool uploadedValid=false;
    };
    struct SrInputSelection
    {
        std::shared_ptr<GenshinTransport::PreparedSrInputs> inputs;
        size_t slot=SrInputSlotCount; // No slot on the ordinary/private path.
    };
    struct History
    {
        uint64_t instance=0,epoch=0,generation=0,canonicalGeneration=0,canonicalHistoryEpoch=0,repairedGeneration=0,completedTick=0;
        GenshinPresentNr::Source canonicalSource{};
        GenshinCameraHistory::History cameraHistory;
        ComPtr<ID3D11Texture2D> color,depth,motion,output,privateOutput;
        std::shared_ptr<GenshinTransport::PreparedSrOutput> srOutput;
        bool srOutputUnavailable=false;
        ComPtr<ID3D11UnorderedAccessView> depthUav,motionUav;
        std::array<std::shared_ptr<GenshinTransport::PreparedSrInputs>,SrInputSlotCount> srInputSlots;
        // Same index and History lifetime as the actual prepared-input owner.
        // This is not the provider commandFrame, nor a scheduling counter.
        std::array<PrepareControlSlot,SrInputSlotCount> prepareControlSlots;
        bool srInputSlotsUnavailable=false;
        std::unique_ptr<GenshinFsr::Dx11Owner> owner;
        std::shared_ptr<GenshinTransport::ProducerFrame> transport;
        bool observationProducerAttempted=false; // Actual construction boundary, not a draw budget.
        std::array<ComPtr<ID3D11Texture2D>,2> gameOutputs;
        std::array<UINT,2> gameOutputSubresources{};
        std::chrono::steady_clock::time_point lastTime{},srCadenceWindow{};
        uint64_t srCadenceInputs=0,srCadenceReturns=0,srCadenceCopies=0;
        float previousJitterX=0,previousJitterY=0;
        bool havePreviousJitter=false,failed=false;
        bool manualExposureUnavailable=false;
        UINT renderWidth=0,renderHeight=0,outputWidth=0,outputHeight=0;
        DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    };
    static uint64_t NextEpoch(){static std::atomic<uint64_t> epochs{1};return epochs.fetch_add(1);}
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3DDeviceContextState> scratch;
    uint64_t nextContextStateAttempt=0; // Missing-state recovery only, never a draw-path clock.
    ComPtr<ID3D11ComputeShader> prepare;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11Query> completion;
    std::array<ComPtr<ID3D11Query>,3> completedSrQueries;
    std::shared_ptr<Bindings> rawLease;
    // Each issued invocation keeps immutable bindings/query/transport owners.
    // completion/rawLease above are aliases of the latest record for diagnostics.
    struct Flight {
        ComPtr<ID3D11Query> query;
        std::shared_ptr<Bindings> bindings;
        std::shared_ptr<GenshinTransport::ProducerFrame> transport;
        std::shared_ptr<GenshinTransport::PreparedSrInputs> preparedInputs;
        std::shared_ptr<GenshinTransport::PreparedSrOutput> preparedOutput;
        ComPtr<ID3D11Buffer> prepareConstants;
        uint64_t instance=0,historyEpoch=0;
        bool queryDone=false;
    };
    std::list<Flight> flights;
    GenshinPresentNr::OrderedCopyLease pairedLease;
    struct PairFlight {GenshinPresentNr::OrderedCopyLease lease;uint64_t serial=0;};
    std::list<PairFlight> retiredPairs;
    bool retirementFailed=false,configurePending=false;
    uint64_t pairedCopySerial=0,repairDraw=0;
    GenshinPresentNrTrace::PictureCandidate pictureCandidate{},sceneCandidate{};
    std::array<std::unique_ptr<History>,8> histories;
    bool cameraDemand=false;
    bool fullDrawObservation=true;
    History* soleHistory=nullptr; // No per-draw scan or multi-owner shortcut.
    void ObserveCameraDemand(bool demand) noexcept
    {
        // Clear every instance once on disable, including ones with no admitted
        // SR frame in this window. Do not retire any resource/transport owner.
        if(cameraDemand && !demand)
            for(auto& h:histories)if(h)h->cameraHistory.Clear();
        // Re-enable starts empty. Commit/Certify still require each sample's
        // own proof and two actually adjacent canonical producer generations.
        cameraDemand=demand;
    }
    Consumer consumer;
    NoSrConsumer::Consumer captureOnly;
    Engine::Producer engineProducer;
    RawObservation::Capture rawObservation;
    RuntimeCapture::InputObserver runtimeInputs;
    NoSrConsumer::Receipt captureReceipt;
    std::shared_ptr<CaptureSnapshot> captureSnapshot;
    ComPtr<ID3D11Query> captureCompletion;
    std::array<ComPtr<ID3D11Texture2D>,3> captureSources;
    bool capturePending=false;
    CaptureOnlyMode captureMode=CaptureOnlyMode::Disabled;
    CaptureSignalProvider signalProvider=nullptr;
    CapturePresentProvider presentProvider=nullptr;
    void* producerOpaque=nullptr;
    std::uint64_t minimumCaptureGeneration=0;
    Presentation::Ledger presentations;
    Presentation::Receipt nativePresentReceipt;
    ResourceProof::Witness presentTarget;
    std::uint64_t presentTargetSwapchain=0,presentTargetGeneration=0;
    Settings settings;
    Status status;
    NativeMrt::Transaction native;
    std::shared_ptr<Bindings> pendingNative,pendingRepair;
    std::shared_ptr<const Bindings> bindingCache; // Metadata only; issued Flights own GPU lifetimes.
    uint64_t repairInstance=0,repairGeneration=0,repairShader=0;
    void ObservePresentPicture(UINT count,bool countKnown) noexcept
    {
        // Only real Scene/Picture consumers need this observation. Do not
        // inspect arbitrary draws to fill diagnostic candidate windows.
        if(!presentNrSource.Known() || !presentNrContent.Matches(presentNrSource) ||
           !(GenshinPresentNr::PictureDrawShape(returnedDrawKind,count,countKnown,true) ||
             GenshinPresentNr::PictureDrawShape(returnedDrawKind,count,countKnown,false)))return;
        const auto shader=Family::Current(context.Get(),device.Get(),deviceEpoch);
        const bool scene=shader.hash==SceneShader;
        const bool picture=shader.hash==PictureShader;
        if(!shader.Known() || (!scene && !picture))return;
        // Keep the existing trace on functional candidates; no additional
        // binding or resource read is issued for its diagnostic metadata.
        auto& candidate=scene?sceneCandidate:pictureCandidate;candidate.seq=GenshinPresentNrTrace::Sequence();
        candidate.draw=status.observedDraws;candidate.shader=shader.hash;candidate.kind=returnedDrawKind;
        candidate.count=count;candidate.countKnown=countKnown;candidate.scene=scene;
        candidate.input=candidate.output=0;++(scene?candidate.sceneSeen:candidate.pictureSeen);
        candidate.reason=1;
        if(!GenshinPresentNr::PictureDrawShape(returnedDrawKind,count,countKnown,scene))return;
        candidate.reason=2;if(!presentNrSource.Known())return;
        candidate.reason=5;if(!presentNrContent.Matches(presentNrSource))return;
        candidate.reason=3;
        // Resource descriptors are read independently of the bounded diagnostic
        // probe windows. Only an actual original draw return advances this clock.
        ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11RenderTargetView> rtv;
        context->PSGetShaderResources(0,1,&srv);context->OMGetRenderTargets(1,&rtv,nullptr);
        const auto* cache=bindingCache.get();
        const bool sameOwner=cache && cache->contextIdentity==reinterpret_cast<uintptr_t>(context.Get()) &&
            cache->deviceIdentity==reinterpret_cast<uintptr_t>(device.Get()) && cache->deviceEpoch==deviceEpoch &&
            cache->ownerEpoch==ownerEpoch && cache->configEpoch==configEpoch &&
            cache->ownerThread==GetCurrentThreadId() && GetCurrentThreadId()==thread &&
            Config::Instance()->GenshinControlsCurrent(configEpoch);
        const auto input=sameOwner && srv.Get()==cache->srvs[0].Get() ?
            CachedPictureView(srv.Get(),cache->srvs[0].Get(),cache->inputs[0].Get(),cache->inResourceDesc[0],
                cache->inView[0].ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D,
                cache->inView[0].Texture2D.MostDetailedMip,0) : ReadPictureView(srv.Get());
        const auto output=sameOwner && rtv.Get()==cache->rtvs[1].Get() ?
            CachedPictureView(rtv.Get(),cache->rtvs[1].Get(),cache->outputs[1].Get(),cache->outResourceDesc[1],
                cache->outView[1].ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D,
                cache->outView[1].Texture2D.MipSlice,0) : ReadPictureView(rtv.Get());
        candidate.input=reinterpret_cast<uintptr_t>(input.texture.Get());candidate.output=reinterpret_cast<uintptr_t>(output.texture.Get());
        if(!input.valid || !output.valid){presentNrPictures.Revoke();return;}
        candidate.reason=4; // chain decision is lastSceneReason/lastPictureReason
        if(scene) {
            auto& p=pendingRepairTrace;p.sceneSeq=GenshinPresentNrTrace::Sequence();p.sceneDraw=status.observedDraws;
            p.sceneTarget=p.target; // retain the target event; refresh only its matching query snapshot
            if(p.sceneTarget.query.seq==p.outer.seq)p.sceneTarget.query=p.outer;
            p.sceneGeneration=presentNrSource.generation;p.sceneSelectedIdentity=presentNrSource.selectedOutput;
            p.sceneInputTexture=reinterpret_cast<uintptr_t>(input.texture.Get());
            p.sceneOutputTexture=reinterpret_cast<uintptr_t>(output.texture.Get());
        }
        if(scene)presentNrPictures.Scene(presentNrSource,input.texture.Get(),input.subresource,
            output.texture.Get(),output.subresource,status.observedDraws);
        else presentNrPictures.Picture(presentNrSource,input.texture.Get(),input.subresource,
            output.texture.Get(),output.subresource,status.observedDraws);
    }
    void ClearCanonical(History& h) noexcept
    {
        h.canonicalGeneration=h.canonicalHistoryEpoch=h.completedTick=0;h.canonicalSource={};
        if(presentNrSource.instance==h.instance){presentNrContent.Revoke();presentNrSource={};
            if(GetCurrentThreadId()==thread)presentNrPictures.ClearOwner();else presentNrPictures.Revoke();}
    }
    VisibleOutput::Pair RepairPair(const Bindings& b,const History& h,bool noToken) const noexcept
    {
        bool knownOutput=false;
        for(unsigned i=0;i<2;++i)knownOutput=knownOutput || (h.gameOutputs[i].Get()==b.outputs[b.outputSlot].Get() &&
            h.gameOutputSubresources[i]==b.outputSubresource[b.outputSlot]);
        const auto& c=h.canonicalSource;const auto& live=presentNrSource;
        const bool sameOwner=!invalidated && GetCurrentThreadId()==thread && c.Known() && live.Known() &&
            h.canonicalHistoryEpoch==h.epoch && c.instance==h.instance && c.generation==h.canonicalGeneration &&
            live.instance==c.instance && live.generation==c.generation && live.privateOutput==c.privateOutput &&
            c.privateOutput==GenshinPresentNr::Identity(h.output.Get()) &&
            c.device==GenshinPresentNr::Identity(device.Get()) && c.context==GenshinPresentNr::Identity(context.Get()) &&
            live.device==c.device && live.context==c.context && c.deviceEpoch==deviceEpoch &&
            live.deviceEpoch==c.deviceEpoch && c.ownerEpoch==ownerEpoch && live.ownerEpoch==c.ownerEpoch &&
            c.configEpoch==configEpoch && live.configEpoch==c.configEpoch && c.thread==thread && live.thread==thread &&
            c.width==live.width && c.height==live.height && c.sourceFrame==live.sourceFrame &&
            c.submitSerial==live.submitSerial && c.copyOrderedSerial==live.copyOrderedSerial &&
            Config::Instance()->GenshinControlsCurrent(configEpoch);
        D3D11_TEXTURE2D_DESC output{};ComPtr<ID3D11Device> privateDevice,gameDevice;
        if(h.output){h.output->GetDesc(&output);h.output->GetDevice(&privateDevice);}
        if(b.outputs[b.outputSlot])b.outputs[b.outputSlot]->GetDevice(&gameDevice);
        // ReadBindings has proved the actual typed RTV/raw-resource family and
        // selected mip. Copy only that mip, never infer pairing from dimensions.
        const auto& selected=b.outDesc[b.outputSlot];
        const bool compatible=h.output && b.outputs[b.outputSlot] &&
            GenshinPresentNr::Identity(privateDevice.Get())==c.device && GenshinPresentNr::Identity(gameDevice.Get())==c.device &&
            output.Width==selected.Width && output.Height==selected.Height && output.Format==b.outputFormat &&
            output.Width==c.width && output.Height==c.height && output.ArraySize==1 && output.MipLevels==1 &&
            output.SampleDesc.Count==1 && !output.SampleDesc.Quality && selected.SampleDesc.Count==1 &&
            !selected.SampleDesc.Quality && selected.ArraySize==1;
        return {h.generation,h.canonicalGeneration,h.repairedGeneration,GetTickCount64(),h.completedTick,
            knownOutput,sameOwner,compatible,noToken,static_cast<bool>(h.output),h.failed};
    }
    bool RepairEligible(const Bindings& b,const History& h,bool noToken) const noexcept
    {return VisibleOutput::CanRepair(RepairPair(b,h,noToken));}
    void RevokeRepair() noexcept
    {
        pendingRepair.reset();repairInstance=repairGeneration=repairShader=0;presentNrContent.Revoke();presentNrPictures.Revoke();
        // Revocation from another thread changes metadata only. Poll on the
        // owning context closes any armed lease before retirement.
        if(GetCurrentThreadId()==thread)pairedLease.End();
        for(auto& h:histories)if(h)ClearCanonical(*h);
    }
    bool CompleteRepair() noexcept
    {
        auto& trace=pendingRepairTrace.target;trace.attempted=true;trace.attemptSeq=GenshinPresentNrTrace::Sequence();
        if(invalidated || !Config::Instance()->GenshinControlsCurrent(configEpoch))
        {trace.stage=8;RevokeRepair();status.reason=CaptureReason::Draining;status.srAccepted=false;return false;}
        auto held=std::move(pendingRepair);if(!held || !pairedLease.Occupied())return false;
        History* h=nullptr;for(auto& candidate:histories)if(candidate && candidate->instance==repairInstance)h=candidate.get();
        Bindings current;current.targetProfile=true;current.quiet=true;il2cpp_callsite::RenderToken token;
        const auto shader=Family::Current(context.Get(),device.Get(),deviceEpoch);
        if(!h || repairDraw!=status.observedDraws || h->generation!=repairGeneration || !shader.Known() || shader.identity!=repairShader ||
            shader.hash!=fsr2_family_takeover::k_accumulate_hash || !ReadBindings(context.Get(),current,held.get()) ||
            !SameRepairBindings(current,*held) || !RepairBindingsSafe(context.Get(),current,h->output.Get()) ||
            !RepairEligible(current,*h,MatchToken(current,token)==0)){pairedLease.End();if(h)ClearCanonical(*h);repairStage=6;trace.stage=6;return false;}
        ComPtr<ID3DDeviceContextState> previous;
        context1->SwapDeviceContextState(scratch.Get(),&previous);
        // A controls edit can arrive while validating bindings or swapping state.
        // Even an aborted copy closes the paired query and retains its lease until Poll.
        if(invalidated || !Config::Instance()->GenshinControlsCurrent(configEpoch))
        {
            pairedLease.End();context1->SwapDeviceContextState(previous.Get(),nullptr);
            trace.stage=8;
            RevokeRepair();status.pending=true;status.reason=CaptureReason::Draining;status.srAccepted=false;
            return false;
        }
        context->CopySubresourceRegion(current.outputs[current.outputSlot].Get(),current.outputSubresource[current.outputSlot],
            0,0,0,h->output.Get(),0,nullptr);
        pairedCopySerial=GenshinPresentNrTrace::Sequence();pairedLease.End();context1->SwapDeviceContextState(previous.Get(),nullptr);
        trace.stage=5;trace.submitted=true;trace.copySeq=pairedCopySerial;
        trace.sourceTexture=reinterpret_cast<uintptr_t>(h->output.Get());
        trace.destinationTexture=reinterpret_cast<uintptr_t>(current.outputs[current.outputSlot].Get());
        trace.destinationResource=reinterpret_cast<uintptr_t>(current.outputResources[current.outputSlot].Get());
        trace.subresource=current.outputSubresource[current.outputSlot];
        // The ordered paired copy changed which actual S feeds the next
        // S -> A draw. Retarget only this unchanged successful source; never
        // mint a generation, refresh its age, or admit a failed/stale history.
        const auto privateIdentity=GenshinPresentNr::Identity(h->output.Get());
        if(presentNrSource.Known() && presentNrSource.instance==h->instance &&
           presentNrSource.generation==h->generation && presentNrSource.privateOutput==privateIdentity &&
           presentNrSource.deviceEpoch==deviceEpoch && presentNrSource.ownerEpoch==ownerEpoch &&
           presentNrSource.configEpoch==configEpoch && presentNrSource.thread==GetCurrentThreadId()) {
            presentNrPictures.ClearOwner();
            presentNrSource.selectedOutput=GenshinPresentNr::Identity(current.outputResources[current.outputSlot].Get());
            presentNrSource.selectedSubresource=current.outputSubresource[current.outputSlot];
            presentNrSource.copyOrderedSerial=pairedCopySerial;presentNrSource.gpuReadySerial=0;
            presentNrContent.Copied(presentNrSource);
            ++presentNrPairedCopies;presentNrPairedDraw=status.observedDraws;
            presentNrPairedOutput=presentNrSource.selectedOutput;
            trace.retargeted=true;trace.selectedIdentity=presentNrSource.selectedOutput;
        }
        h->repairedGeneration=h->generation;repairStage=5;++status.reusedCopies;status.pending=true;
        status.reason=CaptureReason::ReusedOutput;status.srAccepted=true;
        static std::atomic<uint64_t> repairs{0};const auto repairCount=repairs.fetch_add(1,std::memory_order_relaxed)+1;
        if(repairCount<=8 || repairCount%1024==0)LOG_INFO("Genshin SR paired output repair: pid={} instance={} generation={} destination={} subresource={} "
            "native_companion_preserved=true temporal_dispatch=false final_visible=UNKNOWN",
            GetCurrentProcessId(),h->instance,h->generation,reinterpret_cast<uintptr_t>(current.outputResources[current.outputSlot].Get()),
            current.outputSubresource[current.outputSlot]);
        return true;
    }
    bool ArmRepair(std::shared_ptr<Bindings>& bindings,History& h,const Family::Shader& shader) noexcept
    {
        std::shared_ptr<Bindings> held;
        try{
            held=bindings;
            if(pairedLease.Occupied()){
                // Allocate before moving any owner; prior ordered copy remains
                // retained independently of this generation's usable output.
                retiredPairs.emplace_back();auto& prior=retiredPairs.back();
                pairedLease.End();prior.lease=std::move(pairedLease);prior.serial=pairedCopySerial;
                pairedLease=GenshinPresentNr::OrderedCopyLease{};
            }
        }catch(...){return false;}
        if(!pairedLease.Arm(device.Get(),context.Get(),h.output.Get(),bindings->outputResources[bindings->outputSlot].Get(),held))return false;
        pairedCopySerial=0;pendingRepair=std::move(bindings);repairInstance=h.instance;
        repairGeneration=h.generation;repairShader=shader.identity;repairDraw=status.observedDraws;
        repairStage=4;pendingRepairTrace.target.stage=4;return true;
    }
    il2cpp_callsite::RenderToken pendingToken{};
    Family::Shader pendingShader{};
    ComPtr<ID3D11PixelShader> pendingAccumulate;
    bool pendingFamilyAllowed=false;
    Family::State family;
    ComPtr<ID3D11PixelShader> familyAccumulate;
    uint64_t ownerEpoch=NextEpoch(),deviceEpoch=0,configEpoch=0;
    DWORD thread=GetCurrentThreadId();
    bool invalidated=false;
    RawObservation::Owner RawOwner()const noexcept
    {return {reinterpret_cast<uintptr_t>(device.Get()),reinterpret_cast<uintptr_t>(context.Get()),deviceEpoch,ownerEpoch,configEpoch,thread};}
    uint64_t observedRouteDrawSerial=0,observedRouteGeneration=0;bool observedRouteReturnPending=false;
    void ObserveCb(const Bindings& bindings,const il2cpp_callsite::RenderToken& token,const CaptureSource& source,UINT count,bool countKnown) noexcept
    {
        RuntimeCapture::DrawFact d;d.drawCount=count;d.drawCountKnown=countKnown;d.captureTokenMatches=1;d.familyPass=true;d.viewportCount=1;d.observationTick=GetTickCount64();const auto owner=RawOwner();
        d.device=owner.device;d.context=owner.context;d.deviceEpoch=owner.deviceEpoch;
        d.ownerEpoch=owner.producerEpoch;d.configEpoch=owner.configEpoch;d.thread=owner.thread;
        d.drawSerial=status.observedDraws;d.instance=token.params.instance;d.generation=token.generation;
        d.frame=token.params.frame_index;d.captureTick=token.capture_tick;d.shaderHash=source.shaderHash;d.shaderIdentity=source.shaderIdentity;
        d.renderWidth=token.params.render_w;d.renderHeight=token.params.render_h;
        d.outputWidth=token.params.display_w;d.outputHeight=token.params.display_h;
        d.jitterX=token.params.jitter_x;d.jitterY=token.params.jitter_y;d.viewport=bindings.viewport;
        for(unsigned i=0;i<3;++i){const auto slot=source.srvSlots[i];d.resources[i]=reinterpret_cast<uintptr_t>(source.resources[i]);
            d.views[i]=reinterpret_cast<uintptr_t>(bindings.srvs[slot].Get());d.slots[i]=slot;d.formats[i]=source.viewFormats[i];
            d.widths[i]=bindings.inDesc[slot].Width;d.heights[i]=bindings.inDesc[slot].Height;}
        for(unsigned i=0;i<7;++i){d.boundResources[i]=reinterpret_cast<uintptr_t>(bindings.inputs[i].Get());
            d.boundViews[i]=reinterpret_cast<uintptr_t>(bindings.srvs[i].Get());d.boundFormats[i]=bindings.inView[i].Format;}
        for(unsigned i=0;i<2;++i){d.outputResources[i]=reinterpret_cast<uintptr_t>(bindings.outputs[i].Get());
            d.outputViews[i]=reinterpret_cast<uintptr_t>(bindings.rtvs[i].Get());d.outputFormats[i]=bindings.outView[i].Format;
            d.outputWidths[i]=bindings.outDesc[i].Width;d.outputHeights[i]=bindings.outDesc[i].Height;}
        d.selectedOutputSlot=bindings.outputSlot;d.sourceWrites=source.sourceWrites;d.outputWitness=source.outputIdentity;
        observedRouteDrawSerial=d.drawSerial;observedRouteGeneration=d.generation;observedRouteReturnPending=true;
        std::array<ID3D11ShaderResourceView*,3> inputViews{};
        for(unsigned i=0;i<3;++i)inputViews[i]=bindings.srvs[source.srvSlots[i]].Get();
        runtimeInputs.Queue(device.Get(),context.Get(),d,inputViews);
    }
    RuntimeCapture::RouteFact RouteObservation(const Engine::Receipt& e)const noexcept
    {
        RuntimeCapture::RouteFact r;const auto owner=RawOwner();
        r.device=owner.device;r.context=owner.context;r.deviceEpoch=owner.deviceEpoch;r.ownerEpoch=owner.producerEpoch;
        r.configEpoch=owner.configEpoch;r.thread=owner.thread;
        r.drawSerial=observedRouteGeneration==e.sourceGeneration?observedRouteDrawSerial:0;
        r.instance=e.candidate.instance;r.generation=e.sourceGeneration;r.frame=e.candidate.sourceFrame;
        r.shaderIdentity=e.shaderIdentity;r.shaderHash=e.shaderHash;r.originalDrawReturned=e.originalDrawReturned;r.output=e.outputIdentity;
        return r;
    }
    Presentation::Owner PresentOwner()const noexcept
    {return {reinterpret_cast<uintptr_t>(device.Get()),deviceEpoch,reinterpret_cast<uintptr_t>(context.Get()),ownerEpoch,GetCurrentThreadId()};}
    void TryRetirePresented() noexcept
    {
        if(invalidated || capturePending || !captureSnapshot || !captureReceipt.admitted ||
            captureReceipt.state==NoSrCapture::State::Retired || !presentations.IsCurrent(nativePresentReceipt))return;
        const auto& ticket=nativePresentReceipt.ticket;const auto& signal=captureSnapshot->signal;
        if(ticket.owner!=PresentOwner() || ticket.swapchain!=signal.swapchain || ticket.frame.instance!=signal.instance ||
            ticket.frame.generation!=captureSnapshot->renderGeneration || ticket.frame.sourceFrame!=signal.sourceFrame ||
            ticket.frame.producerEpoch!=signal.producerEpoch)return;
        captureReceipt=captureOnly.OnCompletedPresent(signal.instance,captureSnapshot->renderGeneration,nativePresentReceipt.completionSerial);
        if(captureReceipt.state==NoSrCapture::State::Retired)status.reason=CaptureReason::CaptureRetired;
    }

    struct CaptureCopyContext { Storage* storage=nullptr; Bindings* bindings=nullptr; };
    bool CopyCapture(Bindings& b,const NoSrCapture::Signal& signal,
                     const il2cpp_callsite::RenderToken& token) noexcept
    {
        if(!device || !context || !b.inputs[b.colorSlot] || !b.inputs[b.depthSlot] || !b.inputs[b.motionSlot])return false;
        if(!captureCompletion){D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};
            if(FAILED(device->CreateQuery(&q,&captureCompletion)))return false;}
        const unsigned slots[]{b.colorSlot,b.depthSlot,b.motionSlot};
        std::array<D3D11_TEXTURE2D_DESC,3> descriptions{};
        bool reuse=captureSnapshot && captureSnapshot.use_count()==1 && captureSnapshot->gpuCompleted;
        for(unsigned i=0;i<3;++i)
        {
            auto& desc=descriptions[i];desc=b.inDesc[slots[i]];desc.BindFlags=0;desc.MiscFlags=0;
            if(reuse){D3D11_TEXTURE2D_DESC existing{};captureSnapshot->resources[i]->GetDesc(&existing);
                reuse=existing.Width==desc.Width && existing.Height==desc.Height && existing.Format==desc.Format &&
                    existing.SampleDesc.Count==desc.SampleDesc.Count && existing.SampleDesc.Quality==desc.SampleDesc.Quality;}
        }
        if(!reuse)
        {
            // Allocate transactionally before submitting any copy. A published
            // snapshot held by another consumer is never overwritten.
            std::shared_ptr<CaptureSnapshot> next;
            try{next=std::make_shared<CaptureSnapshot>();}catch(...){return false;}
            for(unsigned i=0;i<3;++i){
                if(FAILED(device->CreateTexture2D(&descriptions[i],nullptr,&next->resources[i])))return false;
                ++status.captureTextureCreates;}
            captureSnapshot=std::move(next);
        }
        else status.captureTextureReuses+=3;
        captureSnapshot->signal=signal;captureSnapshot->renderGeneration=token.generation;captureSnapshot->gpuCompleted=false;
        for(unsigned i=0;i<3;++i){captureSources[i]=b.inputs[slots[i]];
            captureSnapshot->sourceIdentity[i]=reinterpret_cast<uintptr_t>(captureSources[i].Get());
            context->CopySubresourceRegion(captureSnapshot->resources[i].Get(),0,0,0,0,captureSources[i].Get(),b.inputSubresource[slots[i]],nullptr);}
        context->End(captureCompletion.Get());capturePending=true;status.pending=true;++status.captureCopies;
        return true;
    }
    static bool CopyCaptureThunk(void* opaque,const NoSrCapture::Signal& signal,const il2cpp_callsite::RenderToken& token) noexcept
    {
        auto* capture=static_cast<CaptureCopyContext*>(opaque);
        return capture && capture->storage && capture->bindings && capture->storage->CopyCapture(*capture->bindings,signal,token);
    }

    static std::vector<Storage*>& Retired(){static auto* v=new std::vector<Storage*>;return *v;}
    static std::mutex& RetiredMutex(){static auto* m=new std::mutex;return *m;}
    static std::atomic<size_t>& RetiredCount(){static std::atomic<size_t> count{0};return count;}
    bool Poll()
    {
        pollStop=0;
        if(GetCurrentThreadId()!=thread){pollStop=1;status.reason=CaptureReason::ThreadMismatch;return false;}
        GenshinDepthEvidence::Poll(context.Get());
        rawObservation.CheckCurrent(RawOwner());
        if(invalidated)rawObservation.Revoke();
        if(invalidated)runtimeInputs.Revoke();
        runtimeInputs.CheckGeneration(&il2cpp_callsite::params_generation_for);
        // Raw diagnostic readbacks retain their own leases; they never gate SR.
        runtimeInputs.Poll(device.Get(),context.Get());
        bool transportPending=!rawObservation.Poll();
        for(auto& h:histories)if(h && h->owner){
            if(h->owner->Poll()==GenshinFsr::Reason::CompletionFailure)retirementFailed=true;
            transportPending=h->owner->Status().pending || transportPending;
        }
        // Generation advancement does not revoke issued work. Only real owner,
        // config/device invalidation revokes it; physical retirement is separate.
        for(auto& flight:flights)if(flight.transport){
            auto current=flight.transport->Owner();
            current.configEpoch=configEpoch;current.producerEpoch=ownerEpoch;current.deviceEpoch=deviceEpoch;
            flight.transport->CheckCurrent(current);
            if(invalidated || (device && FAILED(device->GetDeviceRemovedReason())))flight.transport->Revoke();
            // Poll once in the retirement loop below, after the outer EVENT.
        }
        GenshinTransport::ProducerFrame::CollectRetired();GenshinTransport::CollectRetired();
        if(consumer.poll){PsState::Mutation opaque(context.Get());consumer.poll(consumer.opaque);}
        // A revoked depth mapping still owns its issued GPU work until retired.
        transportPending=(invalidated && depthMapping.Pending(context.Get())) || transportPending;
        if(!device)return true;
        if(FAILED(device->GetDeviceRemovedReason()))
        {pairedLease.DeviceRemoved();
            for(auto& prior:retiredPairs)prior.lease.DeviceRemoved();retiredPairs.clear();flights.clear();
            retirementFailed=false;pairedCopySerial=0;presentNrContent.Revoke();presentNrSource={};presentNrPictures.ClearOwner();
            pendingRepairTrace.outer.active=false;family.Reset();familyAccumulate.Reset();rawLease.reset();completion.Reset();capturePending=false;captureSources={};captureCompletion.Reset();
            captureSnapshot.reset();captureReceipt=captureOnly.Reset();presentations.Reset();nativePresentReceipt={};engineProducer.Reset();
            status.pending=transportPending;status.srAccepted=false;invalidated=true;return !transportPending;}
        if(capturePending)
        {
            BOOL copied=FALSE;auto hr=GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::QueryGetDataDontFlush,[&]{return context->GetData(captureCompletion.Get(),&copied,sizeof(copied),D3D11_ASYNC_GETDATA_DONOTFLUSH);},GenshinHookCpuTiming::ResultKind::Hresult);
            if(FAILED(hr)){pollStop=3;status.reason=CaptureReason::CompletionFailed;return false;}
            if(hr==S_FALSE || !copied){pollStop=4;return false;}
            capturePending=false;captureSources={};captureSnapshot->gpuCompleted=true;
            if(!invalidated){const auto& signal=captureSnapshot->signal;
                captureReceipt=captureOnly.CompleteCopy(signal.device,signal.context,signal.resourceGeneration);
                status.reason=captureReceipt.admitted?CaptureReason::CaptureAdmitted:CaptureReason::CaptureRejected;}
        }
        if(invalidated){captureSnapshot.reset();captureReceipt=captureOnly.Reset();}
        else TryRetirePresented();
        if(!pendingRepair)pairedLease.End(); // unmatched/revoked arming drains without a copy
        auto pollPair=[&](GenshinPresentNr::OrderedCopyLease& lease,uint64_t serial){
            const auto state=lease.Poll();
            if(state==GenshinPresentNr::OrderedCopyLease::State::Failed)retirementFailed=true;
            if(state==GenshinPresentNr::OrderedCopyLease::State::Ready && serial &&
               presentNrSource.copyOrderedSerial==serial)presentNrSource.gpuReadySerial=GenshinPresentNrTrace::Sequence();
        };
        pollPair(pairedLease,pairedCopySerial);
        for(auto it=retiredPairs.begin();it!=retiredPairs.end();){
            pollPair(it->lease,it->serial);
            if(!it->lease.Occupied())it=retiredPairs.erase(it);else ++it;
        }
        for(auto it=flights.begin();it!=flights.end();){
            auto& flight=*it;
            if(!flight.queryDone){
                BOOL done=FALSE;
                const auto hr=GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::QueryGetDataDontFlush,[&]{return context->GetData(flight.query.Get(),&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH);},GenshinHookCpuTiming::ResultKind::Hresult);
                auto& trace=pendingRepairTrace.outer;
                const bool latest=completion.Get()==flight.query.Get();
                if(latest){trace.pollSeq=GenshinPresentNrTrace::Sequence();trace.hr=hr;
                    trace.done=done!=FALSE;trace.polled=true;trace.transportPending=transportPending;}
                if(FAILED(hr))retirementFailed=true;
                else if(hr==S_OK && done){
                    flight.queryDone=true;
                    if(latest){
                        if(substageOutputPending)GenshinSubstage::Event(substageKey,GenshinSubstage::Stage::NativeOutput,
                            GenshinSubstage::State::Succeeded,GenshinSubstage::Resource::NativeOutput);
                        substageOutputPending=false;trace.readySeq=trace.pollSeq;trace.active=false;
                        if(presentNrSource.submitSerial==trace.seq && presentNrSource.copyOrderedSerial &&
                           presentNrSource.copyOrderedSerial!=pairedCopySerial)presentNrSource.gpuReadySerial=trace.readySeq;
                        completion.Reset();rawLease.reset();
                    }
                }
            }
            // D3D11 completion cannot retire an executed but unfenced DX12 lease.
            const bool transportDone=!flight.transport || flight.transport->Poll();
            transportPending=!transportDone || transportPending;
            if(flight.queryDone && transportDone) {
                // Only recycle after both the exact query and the entire
                // transport retire. Never reuse a pending/failed query.
                for(auto& query:completedSrQueries)if(!query){query=std::move(flight.query);break;}
                it=flights.erase(it);
            } else ++it;
        }
        const bool pairedPending=pairedLease.Occupied() || !retiredPairs.empty();
        status.pending=transportPending || !flights.empty() || pairedPending;
        if(retirementFailed){RevokeRepair();pollStop=5;family.Reset();familyAccumulate.Reset();
            status.reason=CaptureReason::CompletionFailed;status.srAccepted=false;return false;}
        pollStop=completion?6:pairedPending?8:status.pending?2:0;
        return !status.pending;
    }
    History* Find(uint64_t instance)
    {
        for(auto& h:histories)if(h && h->instance==instance)return h.get();
        for(auto& h:histories)if(!h){h=std::make_unique<History>();h->instance=instance;h->epoch=NextEpoch();
            ++status.instances;soleHistory=status.instances==1?h.get():nullptr;return h.get();}
        return nullptr;
    }
    History* Associated(const Bindings& b)
    {
        History* result=nullptr;
        for(auto& h:histories)if(h && (h->gameOutputs[0].Get()==b.outputs[b.outputSlot].Get() || h->gameOutputs[1].Get()==b.outputs[b.outputSlot].Get()))
        {if(result)return nullptr;result=h.get();}
        return result;
    }
    bool HistoryRetired(const History& h) const noexcept
    {
        const bool ownerPending=h.owner && h.owner->Status().pending;
        size_t matchingFlights=0,queriesPending=0,transportsPending=0;
        for(const auto& flight:flights)if(flight.instance==h.instance && flight.historyEpoch==h.epoch) {
            ++matchingFlights;queriesPending+=!flight.queryDone;
            if(flight.transport) {
                const auto snapshot=flight.transport->Snapshot();
                transportsPending+=snapshot.stage!=GenshinTransport::Stage::Complete;
            }
        }
        const bool retired=!ownerPending && !matchingFlights && !pairedLease.Occupied() && retiredPairs.empty();
        if(!retired) {
            static std::atomic<unsigned> reports{0};
            if(reports.fetch_add(1,std::memory_order_relaxed)<16)
                LOG_INFO("R21 SR history retirement: pid={} instance={} epoch={} owner_pending={} flights={} queries_pending={} transports_pending={} paired={} retired_pairs={} no_authority=true",
                    GetCurrentProcessId(),h.instance,h.epoch,ownerPending,matchingFlights,queriesPending,transportsPending,
                    pairedLease.Occupied(),retiredPairs.size());
        }
        return retired;
    }
    bool Configure(const Bindings& b,History& h)
    {
        configurePending=false;
        if(h.depth)
        {
            if(h.renderWidth==b.inDesc[0].Width && h.renderHeight==b.inDesc[0].Height &&
               h.outputWidth==b.outDesc[1].Width && h.outputHeight==b.outDesc[1].Height && h.format==b.outputFormat)return true;
            // Reconfiguration is destructive, unlike compatible same-stream reuse.
            if(!HistoryRetired(h)){configurePending=true;return false;}
            if(h.transport)h.transport->Revoke();
            const auto instance=h.instance;h=History{};h.instance=instance;h.epoch=NextEpoch();
        }
        if(!prepare){ComPtr<ID3DBlob> code,error;
            if(FAILED(D3DCompile(PrepareShader,sizeof(PrepareShader)-1,"legacy_genshin_prepare.hlsl",nullptr,nullptr,
                "main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error)) ||
                FAILED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&prepare)))return false;}
        auto create=[&](UINT w,UINT h,DXGI_FORMAT format,ComPtr<ID3D11Texture2D>& texture){
            D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
            d.Format=format;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
            return SUCCEEDED(device->CreateTexture2D(&d,nullptr,&texture));};
        if(!create(b.inDesc[b.colorSlot].Width,b.inDesc[b.colorSlot].Height,b.colorFormat,h.color) ||
           !create(b.inDesc[b.colorSlot].Width,b.inDesc[b.colorSlot].Height,DXGI_FORMAT_R32_FLOAT,h.depth) ||
           !create(b.inDesc[b.colorSlot].Width,b.inDesc[b.colorSlot].Height,DXGI_FORMAT_R16G16_FLOAT,h.motion) ||
           !create(b.outDesc[b.outputSlot].Width,b.outDesc[b.outputSlot].Height,b.outputFormat,h.privateOutput) ||
           FAILED(device->CreateUnorderedAccessView(h.depth.Get(),nullptr,&h.depthUav)) ||
           FAILED(device->CreateUnorderedAccessView(h.motion.Get(),nullptr,&h.motionUav)))return false;
        h.output=h.privateOutput;
        if(!constants){
            D3D11_BUFFER_DESC d{};d.ByteWidth=16;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            const std::array<UINT,4> controls{settings.depthFlipY,settings.motionFlipY,0,0};
            D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=controls.data();
            if(FAILED(device->CreateBuffer(&d,&initial,&constants)))return false;
        }
        // History owns these allocations. Their immutable shape changes only
        // through this configure/drain path, never by re-reading COM per frame.
        h.renderWidth=b.inDesc[0].Width;h.renderHeight=b.inDesc[0].Height;
        h.outputWidth=b.outDesc[1].Width;h.outputHeight=b.outDesc[1].Height;h.format=b.outputFormat;
        return true;
    }
    bool PureSrInputs() const noexcept
    {
        return !consumer.submit && GenshinTransport::PreparedSrOutput::PureSr();
    }
    std::shared_ptr<GenshinTransport::PreparedSrOutput> SelectSrOutput(History& h,
        const GenshinGuides::Owner& owner,const GenshinFsr::Parameters& p,DXGI_FORMAT format) noexcept
    {
        std::shared_ptr<GenshinTransport::PreparedSrOutput> selected;
        ComPtr<ID3D11Texture2D> output=h.privateOutput;
        // Unlike D3D11's ordered private-output overwrites, a D3D12 UAV write
        // must not race any previous capture/diagnostic/repair reader. No new
        // wait is introduced: a busy history takes the original two-copy path.
        if(PureSrInputs() && !h.srOutputUnavailable && HistoryRetired(h)) {
            if(!h.srOutput && h.transport) {
                const auto prior=h.transport->Snapshot();
                if(!prior.revoked && prior.stage==GenshinTransport::Stage::Complete) {
                    h.srOutput=h.transport->PrepareSrOutput();
                    if(!h.srOutput)h.srOutputUnavailable=true; // Unsupported: retain private output.
                }
            }
            if(h.srOutput) {
                if(auto* texture=h.srOutput->TextureFor(owner,context.Get(),p.outputWidth,p.outputHeight,format)) {
                    output=texture;selected=h.srOutput;
                }
            }
        }
        if(h.output.Get()!=output.Get()) {
            // A canonical/paired permit names an exact resource, not whichever
            // output is now selected. Its issued physical leases still retire.
            ClearCanonical(h);h.output=std::move(output);
        }
        return selected;
    }
    SrInputSelection AcquireSrInputs(History& h) noexcept
    {
        // Reuse the same prepare destination for pure SR and the existing
        // authenticated FinishedPicture NR/DLSSG route. Unknown/custom/inline
        // routes keep private inputs. Selection never changes the user's route.
        if(h.srInputSlotsUnavailable || consumer.submit ||
           (!PureSrInputs() && !GenshinTransport::CurrentOutputRoute().batchAtNativeAcquire))return {};
        try {
            for(size_t index=0;index<h.srInputSlots.size();++index) {
                auto& slot=h.srInputSlots[index];
                // Flight, producer, Storage and exported packet lifetimes pin
                // this same slot. Query/transport completion alone cannot retire
                // an NR/FG reader; its existing reader owner must drop the packet.
                // Failed/unfenced work remains pinned in quarantine. The CB
                // shares this slot's authority, never the provider frame index.
                if(slot && slot.use_count()!=1)continue;
                if(!slot) {
                    D3D11_TEXTURE2D_DESC desc{};h.depth->GetDesc(&desc);
                    slot=GenshinTransport::PreparedSrInputs::Create(context.Get(),desc.Width,desc.Height);
                    if(!slot){h.srInputSlotsUnavailable=true;return {};}
                }
                auto& control=h.prepareControlSlots[index];
                if(!control.constants) {
                    D3D11_BUFFER_DESC desc{};desc.ByteWidth=16;desc.Usage=D3D11_USAGE_DEFAULT;
                    desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
                    ComPtr<ID3D11Buffer> constants;
                    if(FAILED(device->CreateBuffer(&desc,nullptr,&constants)) || !constants) {
                        // Optional optimization failure: keep the existing
                        // private inputs/single CB path, without per-frame retry.
                        h.srInputSlotsUnavailable=true;return {};
                    }
                    control.constants=std::move(constants);control.uploadedValid=false;
                }
                return {slot,index};
            }
        }catch(...){} // Optional optimization: preserve private preparation.
        return {}; // Bounded pool full: copy, never wait or overwrite a live slot.
    }
    GenshinFsr::Result Submit(const GenshinFsr::CapturedFrame& frame,History& h)
    {
        if(consumer.submit){PsState::Mutation opaque(context.Get());return consumer.submit(consumer.opaque,frame);}
        if(!h.owner)
        {
            GenshinFsr::Discovery proof;
            proof.captureEntryProven=il2cpp_callsite::active();
            // Legacy preparation and requested controls do not verify current engine semantics.
            proof.semanticsProven=false;proof.srParameterProvenance=GenshinFsr::SrParameterProvenance::LegacyRequestedControls;
            proof.orderedOutputConsumerProven=true;proof.inputLeaseProven=true;proof.sr=true;
            h.owner=std::make_unique<GenshinFsr::Dx11Owner>(context.Get(),proof);
        }
        return h.owner->Submit(frame);
    }
};
CapturePipeline::CapturePipeline(ID3D11DeviceContext* context,Settings settings,Consumer consumer,uint64_t deviceEpoch,uint64_t configEpoch)
    :storage_(std::make_unique<Storage>())
{
    auto& s=*storage_;s.settings=settings;s.consumer=consumer;s.deviceEpoch=deviceEpoch;s.configEpoch=configEpoch;
    s.captureMode=captureOnlyMode;s.signalProvider=captureSignalProvider;s.presentProvider=capturePresentProvider;
    s.producerOpaque=captureProviderOpaque;s.minimumCaptureGeneration=captureMinimumGeneration;
    s.status.ownerEpoch=s.ownerEpoch;s.status.deviceEpoch=deviceEpoch;s.status.requestedFamilySkip=settings.familySkip;
    if(!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    s.context=context;context->GetDevice(&s.device);
    if(!s.device)return;
    s.engineProducer.Bind(s.device.Get(),context,deviceEpoch,s.ownerEpoch,s.thread);
    if(settings.captureOnlyObservation && s.captureMode==CaptureOnlyMode::Disabled){
        s.captureMode=CaptureOnlyMode::ObservationOnly;s.signalProvider=&Engine::Producer::Publish;s.producerOpaque=&s.engineProducer;}
    if(s.captureMode==CaptureOnlyMode::ObservationOnly){s.status.reason=CaptureReason::NoRenderToken;return;}
    s.status.reason=CaptureReason::PreparationFailed;s.status.captureStage=CaptureStage::ContextState;
    s.status.contextStateResult=s.device.As(&s.device1);
    if(FAILED(s.status.contextStateResult))return;
    s.status.contextStateResult=s.context.As(&s.context1);
    if(FAILED(s.status.contextStateResult))return;
    TryInitializeContextState();
}
void CapturePipeline::TryInitializeContextState()
{
    auto& s=*storage_;
    // A failed initial scratch creation owns no SR submission. Retry only at
    // construction or the real owner Present, never in BeforeDraw/Poll. An
    // invalidated owner must still drain; this cannot reopen its authority.
    if(s.scratch || s.invalidated || GetCurrentThreadId()!=s.thread ||
       s.captureMode==CaptureOnlyMode::ObservationOnly || !s.context || !s.device ||
       !s.device1 || !s.context1)return;
    const auto now=GetTickCount64();
    if(now<s.nextContextStateAttempt)return;
    s.nextContextStateAttempt=now+1000; // Persistent refusal must not allocate every Present.
    if(FAILED(s.device->GetDeviceRemovedReason()))return;
    s.status.srAccepted=false;s.status.captureStage=CaptureStage::ContextState;
    s.status.reason=CaptureReason::PreparationFailed;
    auto level=s.device->GetFeatureLevel();D3D_FEATURE_LEVEL chosen{};
    UINT flags=(s.device->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED)?D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED:0;
    ComPtr<ID3DDeviceContextState> scratch;
    s.status.contextStateResult=s.device1->CreateDeviceContextState(flags,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&chosen,&scratch);
    if(FAILED(s.status.contextStateResult) || !scratch)return;
    s.scratch=std::move(scratch);
    // State availability is not SR acceptance. The next real Render/target draw
    // must still construct its producer/provider, Evaluate and copy game RTV.
    s.status.captureStage=CaptureStage::NotInspected;s.status.reason=CaptureReason::ShaderUnavailable;
}
CapturePipeline::~CapturePipeline()
{
    if(!storage_)return;
    Invalidate();
    if(!storage_->Poll() || GetCurrentThreadId()!=storage_->thread)
    {std::lock_guard lock(Storage::RetiredMutex());Storage::Retired().push_back(storage_.release());Storage::RetiredCount().fetch_add(1,std::memory_order_release);}
}
bool CapturePipeline::BeginDrawObservation(UINT count,bool countKnown,unsigned drawKind)
{
    GenshinCpuStageTiming::Scope hostObserve(GenshinCpuStageTiming::Stage::HostObserveDraw);
    auto& s=*storage_;s.returnedDrawKind=drawKind;++s.status.observedDraws;s.status.srAccepted=false;s.status.entryInstalled=il2cpp_callsite::active();
    if(s.fullDrawObservation && GetCurrentThreadId()==s.thread && !s.invalidated && s.context && s.device){
        const bool guideDemand=OptionalGuideDemand();
        s.ObserveCameraDemand(guideDemand);
        if(guideDemand && s.presentNrContent.Matches(s.presentNrSource)){
            const auto shader=Family::Current(s.context.Get(),s.device.Get(),s.deviceEpoch);
            const auto& source=s.presentNrSource;GenshinGuides::Owner owner{};
            owner.device=source.device;owner.context=source.context;owner.deviceEpoch=source.deviceEpoch;
            owner.producerEpoch=source.ownerEpoch;owner.configEpoch=source.configEpoch;owner.instance=source.instance;
            owner.generation=source.generation;owner.historyEpoch=source.historyEpoch;owner.sourceFrame=source.sourceFrame;owner.thread=source.thread;
            s.depthMapping.Before(s.context.Get(),owner,source.selectedOutput,shader.hash,drawKind,count);
        }else s.depthMapping.Revoke();
    }
    // An unmatched previous callback cannot be reused by another native draw.
    s.pendingNative.reset();s.pendingRepair.reset();
    if(GetCurrentThreadId()==s.thread)s.pairedLease.End();
    s.pendingAccumulate.Reset();s.native.Revoke();
    s.status.captureOnlyActive=s.captureMode==CaptureOnlyMode::ObservationOnly;
    s.status.effectiveFamilySkip=false;s.status.familyReason=s.settings.familySkip?Family::Reason::NoReplacement:Family::Reason::Disabled;
    const auto blocked=[&](CaptureStage stage) {s.status.captureStage=stage;++s.status.preShaderBlocked;return false;};
    if(!s.settings.enabled){s.status.reason=CaptureReason::Disabled;return blocked(CaptureStage::Disabled);}
    if(!Config::Instance()->GenshinControlsCurrent(s.configEpoch)){RevokeTransport();return blocked(CaptureStage::ControlsChanged);}
    return true;
}
GenshinFsr::NativeDrawAction CapturePipeline::BeforeNonTargetDraw(UINT count,unsigned drawKind)
{
    // Only positive, known non-three counts use this entry. Unknown/zero draws
    // retain BeforeDraw's original slow path; this entry grants no capture.
    if(!count || count==3)return BeforeDraw(count,true,true,drawKind);
    if(BeginDrawObservation(count,true,drawKind))storage_->status.captureStage=CaptureStage::DrawCount;
    return Action::ContinueOriginal;
}
GenshinFsr::NativeDrawAction CapturePipeline::BeforeDraw(UINT count,bool familyAllowed,bool countKnown,unsigned drawKind)
{
    GenshinCpuStageTiming::Scope hostBefore(GenshinCpuStageTiming::Stage::HostBeforeDraw); GenshinHookCpuTiming::Scope r448BeforeDraw(GenshinHookCpuTiming::Stage::BeforeDraw);
    if(!BeginDrawObservation(count,countKnown,drawKind))return Action::ContinueOriginal;
    auto& s=*storage_;
    const auto blocked=[&](CaptureStage stage) {s.status.captureStage=stage;++s.status.preShaderBlocked;return Action::ContinueOriginal;};
    if(countKnown && count!=3){s.status.captureStage=CaptureStage::DrawCount;return Action::ContinueOriginal;}
    if(countKnown)++s.status.threeElementDraws;
    if(!s.device || !s.context)return blocked(CaptureStage::MissingOwner);
    if(!s.status.captureOnlyActive && (!s.context1 || !s.scratch)){
        s.status.reason=CaptureReason::PreparationFailed;return blocked(CaptureStage::ContextState);}
    if(GetCurrentThreadId()!=s.thread){s.status.reason=CaptureReason::ThreadMismatch;return blocked(CaptureStage::Thread);}
    if(!s.status.entryInstalled){s.status.reason=CaptureReason::EntryNotMatched;return blocked(CaptureStage::Entry);}
    if(s.invalidated){s.status.reason=CaptureReason::Draining;return blocked(CaptureStage::Draining);}
    const auto pollReady=[&](bool accumulate) {
        const bool retired=s.Poll();
        if(!retired && (s.status.captureOnlyActive || s.capturePending || s.retirementFailed || s.invalidated)) {
            if(accumulate)s.NativeSelectedContent(s.status.observedDraws);
            if(!s.retirementFailed && !s.invalidated)s.status.reason=CaptureReason::Pending;
            return false;
        }
        return !s.invalidated;
    };

    // CaptureOnly is evaluated from the same real Render token and bindings before
    // FamilySkip. Missing producer metadata leaves the native draw untouched.
    if(s.status.captureOnlyActive)
    {
        if(!pollReady(false))return blocked(s.invalidated?CaptureStage::Draining:CaptureStage::Pending);
        // One outstanding immutable capture per owner. Neither another Render
        // nor a rejected Present may overwrite a submitted/admitted snapshot.
        if(s.captureReceipt.admitted && s.captureReceipt.state!=NoSrCapture::State::Retired)return Action::ContinueOriginal;
        s.captureReceipt={};s.captureReceipt.hookSeen=true;
        if(!s.signalProvider){s.captureReceipt.reason=NoSrConsumer::Reason::ProducerUnavailable;
            s.status.reason=CaptureReason::CaptureRejected;return Action::ContinueOriginal;}
        Bindings captureBindings;il2cpp_callsite::RenderToken captureToken;
        const auto captureMatches=ReadBindings(s.context.Get(),captureBindings)
            ? il2cpp_callsite::match_capture_render_token(captureBindings.inDesc[captureBindings.colorSlot].Width,
                captureBindings.inDesc[captureBindings.colorSlot].Height,captureBindings.outDesc[captureBindings.outputSlot].Width,
                captureBindings.outDesc[captureBindings.outputSlot].Height,s.minimumCaptureGeneration,captureToken) : 0;
        if(captureMatches==1)
        {
            NoSrCapture::Signal signal;
            signal.device=reinterpret_cast<uintptr_t>(s.device.Get());
            signal.context=reinterpret_cast<uintptr_t>(s.context.Get());
            signal.instance=captureToken.params.instance;
            signal.renderedFrame=captureToken.params.frame_index;
            signal.sourceFrame=captureToken.params.frame_index;
            signal.nativeSize={captureToken.params.render_w,captureToken.params.render_h};
            signal.outputSize={captureToken.params.display_w,captureToken.params.display_h};
            signal.jitterX=captureToken.params.jitter_x;signal.jitterY=captureToken.params.jitter_y;signal.jitterKnown=true;
            CaptureSource source;source.device=s.device.Get();source.context=s.context.Get();source.deviceEpoch=s.deviceEpoch;source.ownerThread=s.thread;
            source.ownerEpoch=s.ownerEpoch;
            if(s.signalProvider==&Engine::Producer::Publish){const auto shader=Family::Current(s.context.Get(),s.device.Get(),s.deviceEpoch);
                source.shaderHash=shader.hash;source.shaderIdentity=shader.identity;source.shaderDeviceEpoch=shader.deviceEpoch;}
            const unsigned slots[]{captureBindings.colorSlot,captureBindings.depthSlot,captureBindings.motionSlot};
            for(unsigned i=0;i<3;++i){source.resources[i]=captureBindings.inputs[slots[i]].Get();source.viewFormats[i]=captureBindings.inView[slots[i]].Format;source.srvSlots[i]=slots[i];}
            if(s.signalProvider==&Engine::Producer::Publish){
                std::array<ID3D11ShaderResourceView*,3> views{};for(unsigned i=0;i<3;++i)views[i]=captureBindings.srvs[slots[i]].Get();
                source.sourceWrites=ResourceProof::Snapshot(s.context.Get(),source.resources,views);
                source.outputIdentity=ResourceProof::Identify(s.context.Get(),captureBindings.outputs[captureBindings.outputSlot].Get(),captureBindings.rtvs[captureBindings.outputSlot].Get());}
            Storage::CaptureCopyContext copy{&s,&captureBindings};
            bool published=false;
            {PsState::Mutation opaque(s.context.Get());published=s.signalProvider(captureToken,source,signal,s.producerOpaque);}
            if(!published)s.captureReceipt.reason=NoSrConsumer::Reason::ProducerUnavailable;
            else if(signal.device!=reinterpret_cast<uintptr_t>(source.device) || signal.context!=reinterpret_cast<uintptr_t>(source.context) ||
                signal.deviceEpoch!=source.deviceEpoch || signal.ownerThread!=source.ownerThread ||
                signal.producer!=NoSrCapture::Producer::EngineMv || signal.depthFormat!=source.viewFormats[1] ||
                signal.motionFormat!=source.viewFormats[2] || !std::isfinite(signal.jitterX) || !std::isfinite(signal.jitterY) ||
                !std::isfinite(signal.previousJitterX) || !std::isfinite(signal.previousJitterY) ||
                signal.jitterX!=captureToken.params.jitter_x || signal.jitterY!=captureToken.params.jitter_y)
                s.captureReceipt.reason=NoSrConsumer::Reason::ProducerMismatch;
            else {
                // Separate raw facts only. The actual default producer must have observed this exact token/pass.
                // This copy does not satisfy or bypass CaptureOnlyAdmission below.
                if(s.signalProvider==&Engine::Producer::Publish){const auto producer=s.engineProducer.GetReceipt();
                    if(producer.actualRenderWitness && producer.knownLegacyPass && producer.sourceGeneration==captureToken.generation &&
                        producer.ownerEpoch==s.ownerEpoch){
                        s.rawObservation.Begin(s.RawOwner(),captureToken,source);
                        // Disk observation has its own session budget; a held in-memory snapshot must not suppress later frames.
                        s.ObserveCb(captureBindings,captureToken,source,count,countKnown);}}
                signal.presentSerial=0; // Unknown until a real successful native call completes.
                s.captureReceipt=s.captureOnly.BeforeNativeDraw(captureToken,signal,{&Storage::CopyCaptureThunk,&copy,true});}
        }
        else s.captureReceipt.reason=NoSrConsumer::Reason::TokenMismatch;
        s.status.pending=s.status.pending || s.rawObservation.GetReceipt().pending;
        s.status.captureStage=CaptureStage::CaptureOnly;
        s.status.reason=s.capturePending?CaptureReason::CaptureQueued:CaptureReason::CaptureRejected;
        // Observation mode owns this branch even when metadata is absent. It
        // never grants FamilySkip, consumes the Render token or invokes SR.
        return Action::ContinueOriginal;
    }

    GenshinCpuStageTiming::Scope hostFamily(GenshinCpuStageTiming::Stage::HostFamilyGate);
    ++s.status.shaderLookups;
    // R289: a single BeforeDraw-local observation, never the earlier TrySkip
    // result. The native owner selected this pure-SR subscription under its
    // existing lock. Ready binding-cache routes and callback consumers keep
    // Family::Current and the original late accumulate getter unchanged.
    const auto& cfg=*Config::Instance();
    const auto reuseDrawShader=[&]() noexcept {
        return !s.fullDrawObservation && !s.consumer.poll && countKnown && count==3 &&
            drawObservation.srOnly && drawObservation.owner==this &&
            drawObservation.context==s.context.Get() && drawObservation.thread==s.thread &&
            drawObservation.deviceEpoch==s.deviceEpoch && drawObservation.configEpoch==s.configEpoch &&
            drawObservation.ownerEpoch==s.ownerEpoch && drawObservation.hoYo.Current() &&
            !GenshinFgConsumer::FullObservationRequired() && !cfg.DlssNrEnabled.value_or_default() &&
            !cfg.HoYoShadeEnabled.value_or_default() && Config::Instance()->GenshinControlsCurrent(s.configEpoch) &&
            PsState::optionalInstall.Phase()!=PsState::OptionalInstallPhase::Ready;
    };
    ComPtr<ID3D11PixelShader> observedPixel;
    const auto shader=[&]() {
        if(!reuseDrawShader())return Family::Current(s.context.Get(),s.device.Get(),s.deviceEpoch,&s.status.shaderObservation);
        // The retained immediate context/device pair was checked before this
        // call; Read still authenticates the actual shader/device/epoch record.
        s.context->PSGetShader(&observedPixel,nullptr,nullptr);
        if(!observedPixel){s.status.shaderObservation={};
            s.status.shaderObservation.state=Family::ReadState::NoPixelShader;
            s.status.shaderObservation.actualDevice=reinterpret_cast<uintptr_t>(s.device.Get());return Family::Shader{};}
        return Family::Read(s.device.Get(),observedPixel.Get(),s.deviceEpoch,&s.status.shaderObservation);
    }();
    if(shader.Known())++s.status.knownShaderDraws;else ++s.status.unknownShaderDraws;
    s.status.shaderHash=shader.hash;s.status.shaderIdentity=shader.identity;
    const bool accumulate=shader.Known() && shader.hash==fsr2_family_takeover::k_accumulate_hash;
    const bool prepass=shader.Known() && s.family.replaced && s.settings.familySkip && familyAllowed && count==3 &&
        (shader.hash==fsr2_family_takeover::k_pre_hash_1 || shader.hash==fsr2_family_takeover::k_pre_hash_2 ||
         shader.hash==fsr2_family_takeover::k_pre_hash_3 || shader.hash==fsr2_family_takeover::k_pre_hash_4);
    if(!accumulate && !prepass) {
        s.status.captureStage=shader.Known()?CaptureStage::ShaderMismatch:CaptureStage::ShaderUnknown;
        if(!s.status.targetShaderDraws)s.status.reason=shader.Known()?CaptureReason::ShaderNotMatched:CaptureReason::ShaderUnavailable;
        return Action::ContinueOriginal;
    }
    // Retire at a real producer/family invocation or Present. Unrelated
    // three-element UI draws never poll all histories, readbacks and leases.
    if(!pollReady(accumulate))return blocked(s.invalidated?CaptureStage::Draining:CaptureStage::Pending);
    // Without a permit the old Read result was unused. A real permit still
    // receives its full current device/epoch/private-data check on every call.
    if(s.family.replaced){
        const auto held=Family::Read(s.device.Get(),s.familyAccumulate.Get(),s.deviceEpoch);
        if(!held.Known() || held.hash!=fsr2_family_takeover::k_accumulate_hash || held.identity!=s.family.shaderIdentity)
        {s.family.Reset();s.familyAccumulate.Reset();}
    }
    // Admit examines time only after these early refusals. Do not read the
    // system clock for disabled, unknown, non-pre or absent-permit draws.
    const auto hash=shader.hash;
    const bool expiryCandidate=s.settings.familySkip && familyAllowed && shader.Known() && count==3 && s.family.replaced &&
        (hash==fsr2_family_takeover::k_pre_hash_1 || hash==fsr2_family_takeover::k_pre_hash_2 ||
         hash==fsr2_family_takeover::k_pre_hash_3 || hash==fsr2_family_takeover::k_pre_hash_4);
    s.status.familyReason=s.family.Admit(shader,reinterpret_cast<uintptr_t>(s.device.Get()),
        reinterpret_cast<uintptr_t>(s.context.Get()),s.ownerEpoch,count,s.settings.familySkip && familyAllowed,
        expiryCandidate?GetTickCount64():0,s.settings.familyExpireMs);
    hostFamily.Finish();
    if(s.status.familyReason==Family::Reason::Ready)
    {
        ++s.status.familySkipped;s.status.effectiveFamilySkip=true;s.status.familyReason=Family::Reason::Skipped;
        return Action::OmitWithReplacement;
    }
    // A fresh known accumulate invalidates the old permit before any fallback or failure.
    ComPtr<ID3D11PixelShader> accumulateShader;
    // Without consumer.poll, this call's intervening Poll/permit checks do
    // not bind a PS or execute the original draw. Recheck live optional demand
    // and controls after Poll; any change keeps the original getter. The
    // existing demand helper uses CPU publications, not GPU/PS queries.
    if(accumulate){
        if(observedPixel && reuseDrawShader() && OptionalConsumersEffectivelyOff()){
            Family::ShaderCacheMetrics::LateObservedReuse(s.context.Get(),s.device.Get(),s.deviceEpoch);accumulateShader=std::move(observedPixel);}
        else if(!PsState::DrawScope::TryShader(s.context.Get(),s.device.Get(),s.deviceEpoch,accumulateShader)){
            Family::ShaderCacheMetrics::ApiCall(Family::ShaderCacheMetrics::Api::LatePs,s.context.Get(),s.device.Get(),s.deviceEpoch);
            s.context->PSGetShader(&accumulateShader,nullptr,nullptr);}
    }
    if(accumulate){s.family.Reset();s.familyAccumulate.Reset();}
    // NativeMrt already requires this exact shader profile. Gate before binding
    // inspection so an unrelated 3-vertex draw cannot overwrite the producer refusal.
    if(!accumulate){s.status.captureStage=shader.Known()?CaptureStage::ShaderMismatch:CaptureStage::ShaderUnknown;
        // No Render token was inspected on this branch.
        if(!s.status.targetShaderDraws)s.status.reason=shader.Known()?CaptureReason::ShaderNotMatched:CaptureReason::ShaderUnavailable;
        return Action::ContinueOriginal;}
    s.status.captureStage=CaptureStage::Bindings;
    // Revoke old selected content before either native fallback or successful
    // before-native replacement; private preparation cannot preserve old colour.
    s.NativeSelectedContent(s.status.observedDraws);
    ++s.status.targetShaderDraws;
    s.repairTargetDraw=s.status.observedDraws;s.repairStage=2;s.repairPollStop=0;
    s.repairMask=s.repairTokenMatches=0;s.repairCanonical=s.repairCpuGeneration=0;
    s.TraceRepairTarget(2);
    s.substageKey=GenshinSubstage::Begin(
        {GetCurrentProcessId(),Ui::ProcessStartIdentity(),s.deviceEpoch,s.configEpoch},s.ownerEpoch,
        reinterpret_cast<uint64_t>(s.context.Get()),Config::Instance()->DlssNrEnabled.value_or_default()
            ?GenshinSubstage::Mode::Requested:GenshinSubstage::Mode::Disabled);
    // Associate runtime attempt once at this actual producer invocation. Never
    // take an arbitrary latest UI/substage key at Present/Return.
    const auto loadingForInvocation=GenshinLoading::Read();
    s.substageAttempt=loadingForInvocation.known &&
        loadingForInvocation.authority==GenshinLoading::Authority::RuntimeOwner &&
        loadingForInvocation.session==s.substageKey.session && s.substageKey.ownerEpoch==s.ownerEpoch &&
        s.substageKey.context==GenshinPresentNr::Identity(s.context.Get())?loadingForInvocation.attempt:0;
    GenshinSubstage::Scope substageScope(s.substageKey);
    GenshinSubstage::Event(GenshinSubstage::Stage::TargetDraw,GenshinSubstage::State::Succeeded);
    GenshinSubstage::Event(GenshinSubstage::Stage::Bindings,GenshinSubstage::State::Running);
    auto bindings=std::make_shared<Bindings>();bindings->targetProfile=true;
    if(!ReadBindings(s.context.Get(),*bindings,s.bindingCache.get())){++s.status.bindingRejected;s.status.bindingRefusal=bindings->refusal;
        s.pendingRepairTrace.target.bindingsRefusal=bindings->refusal;
        GenshinSubstage::Event(GenshinSubstage::Stage::Bindings,GenshinSubstage::State::Failed,
            GenshinSubstage::Resource::Inputs,GenshinSubstage::Domain::Bindings,bindings->refusal,GenshinSubstage::Issue::Rejected);
        s.status.reason=CaptureReason::UnsupportedBindings;return Action::ContinueOriginal;}
    bindings->contextIdentity=reinterpret_cast<uintptr_t>(s.context.Get());
    bindings->deviceIdentity=reinterpret_cast<uintptr_t>(s.device.Get());
    bindings->deviceEpoch=s.deviceEpoch;bindings->ownerEpoch=s.ownerEpoch;bindings->configEpoch=s.configEpoch;
    bindings->ownerThread=s.thread;s.bindingCache=bindings;
    GenshinSubstage::Event(GenshinSubstage::Stage::Bindings,GenshinSubstage::State::Succeeded);
    ++s.status.candidates;s.status.captureStage=CaptureStage::Token;
    il2cpp_callsite::RenderToken token;
    auto* associated=s.Associated(*bindings);
    auto matches=MatchToken(*bindings,token,associated?associated->instance:0);
    if(!matches && associated)matches=MatchToken(*bindings,token);
    s.repairStage=3;s.repairTokenMatches=matches;
    s.repairCanonical=associated?associated->canonicalGeneration:0;
    s.repairCpuGeneration=0; // Diagnostic latest-generation sampling no longer locks the Render producer.
    auto& repairTrace=s.pendingRepairTrace.target;repairTrace.stage=3;repairTrace.matches=matches;
    repairTrace.instance=associated?associated->instance:0;repairTrace.generation=associated?associated->generation:0;
    repairTrace.canonicalGeneration=s.repairCanonical;repairTrace.cpuGeneration=s.repairCpuGeneration;
    repairTrace.sourceTexture=associated?reinterpret_cast<uintptr_t>(associated->output.Get()):0;
    repairTrace.destinationTexture=reinterpret_cast<uintptr_t>(bindings->outputs[bindings->outputSlot].Get());
    repairTrace.destinationResource=reinterpret_cast<uintptr_t>(bindings->outputResources[bindings->outputSlot].Get());
    repairTrace.subresource=bindings->outputSubresource[bindings->outputSlot];
    if(matches>1)
    {
        // Matching dimensions do not identify a producer when several instances render alike.
        s.status.reason=CaptureReason::AmbiguousRenderToken;return Action::ContinueOriginal;
    }
    if(!matches){
        // Reuse only a known output of the latest successful generation. The
        // native draw still writes its companion MRT; AfterOriginalDraw repairs
        // the selected color output, without running DLSS/history a second time.
        s.repairMask=associated?VisibleOutput::RepairRefusal(s.RepairPair(*bindings,*associated,true)):512u;
        repairTrace.refusal=s.repairMask;
        if(associated && !s.repairMask &&
            RepairBindingsSafe(s.context.Get(),*bindings,associated->output.Get())){
            s.ArmRepair(bindings,*associated,shader);}
        s.status.reason=CaptureReason::NoRenderToken;return Action::ContinueOriginal;}
    // A new Render token does not replace the last successfully ordered Present
    // source. The bridge can issue a paired double-buffer draw before the new
    // temporal result is admitted; keep that source until a replacement commits.
    // Failed or incomplete replacements remain rejected by the existing history
    // failure, generation, completion, identity, and age gates below.
    const auto native=NativeFrame(s.device.Get(),s.context.Get(),*bindings,token,shader,
        s.deviceEpoch,s.ownerEpoch,s.configEpoch,s.thread,s.status.observedDraws,s.settings);
    ++s.status.tokenMatches;s.status.captureStage=CaptureStage::Native;
    // Reuse the existing claim-boundary subscription. This accounts for real
    // readers/retirements and rechecks dynamic demand under the native owner lock.
    // It is not equivalent to disabling owned HoYoShade integration alone.
    const bool beforeNative=RefreshClaimObservation(s.context.Get(),s.configEpoch);
    const auto armed=s.native.Arm(native);s.status.nativeRefusal=static_cast<uint32_t>(armed);
    if(armed!=NativeMrt::Reason::Ready){s.status.reason=CaptureReason::UnsupportedBindings;return Action::ContinueOriginal;}
    s.pendingNative=std::move(bindings);s.pendingToken=token;s.pendingShader=shader;
    s.pendingAccumulate=accumulateShader;s.pendingFamilyAllowed=familyAllowed;
    if(beforeNative && s.native.BeginReplacement()==NativeMrt::Reason::Ready) {
        Action action=Action::ContinueOriginal;
        try{action=CompleteNativeReplacement(true);}
        catch(...){s.status.reason=CaptureReason::PreparationFailed;s.status.srAccepted=false;}
        // No AfterOriginalDraw is expected after success. Failure falls through
        // to the real original draw without repeating Evaluate in its After hook.
        s.pendingNative.reset();s.pendingAccumulate.Reset();s.native.Revoke();
        return action;
    }
    // Full consumers keep the real native companion and native-return pairing.
    return Action::ContinueOriginal;
}
GenshinFsr::NativeDrawAction CapturePipeline::CompleteNativeReplacement(bool beforeNative)
{
    GenshinCpuStageTiming::Scope hostReplacement(GenshinCpuStageTiming::Stage::HostReplacement); GenshinHookCpuTiming::Scope r448Replacement(GenshinHookCpuTiming::Stage::Replacement);
    auto& s=*storage_;
    if(s.invalidated || !Config::Instance()->GenshinControlsCurrent(s.configEpoch))
    {RevokeTransport();return Action::ContinueOriginal;}
    GenshinSubstage::Scope substageScope(s.substageKey);
    auto bindings=std::move(s.pendingNative);const auto token=s.pendingToken;const auto shader=s.pendingShader;
    auto accumulateShader=std::move(s.pendingAccumulate);const auto familyAllowed=s.pendingFamilyAllowed;
    const bool accumulate=shader.Known() && shader.hash==fsr2_family_takeover::k_accumulate_hash;
    const auto* source=beforeNative?s.native.ReplacementFrame():s.native.ClaimedFrame();
    if(!bindings || !source || (beforeNative && !ObservationOnlyCurrent(s.context.Get(),s.configEpoch)))
        return Action::ContinueOriginal;
    if(!beforeNative && !GenshinDepthProvider::ReadObservationDemand().Idle())
        GenshinDepthProvider::CaptureClaimed(s.context.Get(),*s.native.ClaimedFrame(),bindings->srvs[bindings->depthSlot].Get());
    // A prior SR flight may have completed in BeforeDraw's ORIGINAL Poll. A
    // fresh claim-boundary demand check can then subscribe this exact invocation
    // without mistaking its not-yet-issued Flight for pre-existing work.
    const bool collectGuides=!beforeNative && !RefreshClaimObservation(s.context.Get(),s.configEpoch);
    const auto returnedRaster=collectGuides?GenshinReturnedRaster::Evidence::Associate(s.context.Get(),s.native):
        std::shared_ptr<const GenshinReturnedRaster::Evidence>{};
    if(collectGuides)GenshinDepthEvidence::ExportClaim(returnedRaster.get(),*s.native.ClaimedFrame());
    const auto& cp=token.params;
    auto* history=s.Find(cp.instance);
    if(!history){s.status.reason=CaptureReason::InstanceCapacity;return Action::ContinueOriginal;}
    auto& h=*history;
    const auto presetSrTicket=GenshinSubstage::Runtime().BeginPresetSr();
    // Unsupported initial exposure is stable native fallback, not a failed
    // epoch. Existing Poll/flight retirement keeps every real GPU owner.
    // Recheck each token so true/auto releases the refusal without restart.
    if(h.manualExposureUnavailable) {
        if(!Config::Instance()->AutoExposure.value_or(true)) {
            s.status.reason=CaptureReason::ConsumerFailed;s.status.srAccepted=false;
            il2cpp_callsite::consume_render_token_for(cp.instance,token.generation);
            return Action::ContinueOriginal;
        }
        h.manualExposureUnavailable=false;
    }
    if(h.failed)
    {
        if(token.generation<=h.generation){s.status.reason=CaptureReason::ConsumerFailed;return Action::ContinueOriginal;}
        if(!s.HistoryRetired(h)){s.status.reason=CaptureReason::Pending;return Action::ContinueOriginal;}
        if(h.transport)h.transport->Revoke();
        const auto instance=h.instance;h=Storage::History{};h.instance=instance;h.epoch=Storage::NextEpoch();
    }
    GenshinSubstage::Event(GenshinSubstage::Stage::PrivateInputs,GenshinSubstage::State::Running);
    bool configured=false;
    {GenshinCpuStageTiming::Scope hostConfigure(GenshinCpuStageTiming::Stage::HostConfigure); GenshinHookCpuTiming::Scope r448Configure(GenshinHookCpuTiming::Stage::Configure);
        configured=s.Configure(*bindings,h);}
    if(!configured){
        if(s.configurePending){s.status.reason=CaptureReason::Pending;return Action::ContinueOriginal;}
        GenshinSubstage::Event(GenshinSubstage::Stage::PrivateInputs,GenshinSubstage::State::Failed,
            GenshinSubstage::Resource::Inputs,GenshinSubstage::Domain::Fsr,0,GenshinSubstage::Issue::Rejected);h.failed=true;h.generation=token.generation;il2cpp_callsite::consume_render_token_for(cp.instance,token.generation);
        s.status.reason=CaptureReason::PreparationFailed;return Action::ContinueOriginal;}
    GenshinFsr::Parameters p;
    const auto now=std::chrono::steady_clock::now();
    const auto deltaMs=h.lastTime.time_since_epoch().count()?std::clamp(std::chrono::duration<float,std::milli>(now-h.lastTime).count(),1.0f,100.0f):16.666667f;
    const auto& controls=source->controls;
    if(!NativeMrt::ApplyProfile(controls,token,h.havePreviousJitter,h.previousJitterX,h.previousJitterY,
           deltaMs,h.epoch,!h.generation,false,p))
    {s.status.reason=CaptureReason::InvalidTemporalInput;return Action::ContinueOriginal;}
    const GenshinGuides::Owner cameraOwner{reinterpret_cast<uint64_t>(s.device.Get()),
        reinterpret_cast<uint64_t>(s.context.Get()),s.deviceEpoch,s.ownerEpoch,s.configEpoch,
        p.instance,p.generation,h.epoch,p.sourceFrame,s.thread};
    // Select before the outer query, immutable receipt, ResourceSet inspection
    // and provider call. No output identity is rewritten after admission.
    const auto srOutput=s.SelectSrOutput(h,cameraOwner,p,bindings->outputFormat);
    const bool cameraDemand=!beforeNative && OptionalGuideDemand();
    s.ObserveCameraDemand(cameraDemand);
    const auto cameraSample=cameraDemand?GenshinCameraHistory::Adapt(token,cameraOwner):std::nullopt;
    D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};
    ComPtr<ID3D11Query> completion;
    for(auto& query:s.completedSrQueries)if(query){completion=std::move(query);break;}
    if(!completion && FAILED(s.device->CreateQuery(&q,&completion))){s.status.reason=CaptureReason::CompletionFailed;return Action::ContinueOriginal;}
    std::shared_ptr<Bindings> held;
    try {held=std::move(bindings);s.flights.emplace_back();}
    catch(...){s.status.reason=CaptureReason::PreparationFailed;return Action::ContinueOriginal;}
    auto& flight=s.flights.back();flight.query=completion;flight.bindings=held;
    flight.instance=h.instance;flight.historyEpoch=h.epoch;flight.preparedOutput=srOutput;
    s.completion=completion;s.rawLease=std::move(held);s.status.pending=true;
    auto& b=*s.rawLease;
    s.TraceOuterQuery(1,p.instance,p.generation,p.sourceFrame,h.output.Get(),b.outputs[b.outputSlot].Get(),
        b.outputResources[b.outputSlot].Get(),b.outputSubresource[b.outputSlot]);
    ComPtr<ID3DDeviceContextState> previous;
    {GenshinCpuStageTiming::Scope hostStateSave(GenshinCpuStageTiming::Stage::HostStateSave); GenshinHookCpuTiming::Scope r448StateSave(GenshinHookCpuTiming::Stage::StateSave);
        s.context1->SwapDeviceContextState(s.scratch.Get(),&previous);}
    Action action=Action::ContinueOriginal;
    bool replacementCopied=false,recoveryRequired=false;
    if(!h.srCadenceWindow.time_since_epoch().count())h.srCadenceWindow=now;
    ++h.srCadenceInputs;
    bool substagePrepared=false;
    try
    {
        GenshinCpuStageTiming::Scope hostPrepare(GenshinCpuStageTiming::Stage::HostPrepare); GenshinHookCpuTiming::Scope r448Prepare(GenshinHookCpuTiming::Stage::Prepare);
        // Revoke the previous content permit BEFORE the first private-input
        // overwrite, including attempts that later fail or skip Submit. Keep
        // its issued transport/fence owners alive for normal GPU retirement.
        if(h.transport && h.transport->Guides())h.transport->Guides()->Retire();
        // Copy-compatible typed storage admits Unity's typeless resources through their concrete views.
        // Pure SR has no immutable downstream resource packet. In that case a
        // typed, complete game colour can feed the existing private transport
        // directly; its own copy and all input ordering/fences are unchanged.
        // Typeless storage, NR/FG, custom consumers and subresources retain the
        // original private colour copy. Do not alias scratch in those paths.
        const auto& colorDesc=b.inResourceDesc[b.colorSlot];
        const bool directColor=!s.consumer.submit &&
            !native::OpticalFgNrEnabled() && !Config::Instance()->DlssNrEnabled.value_or_default() &&
            !GenshinBridgeFg::Requested() &&
            b.inputSubresource[b.colorSlot]==0 && colorDesc.Format==b.colorFormat &&
            (b.colorFormat==DXGI_FORMAT_R16G16B16A16_FLOAT || b.colorFormat==DXGI_FORMAT_R8G8B8A8_UNORM) &&
            colorDesc.MipLevels==1 && colorDesc.ArraySize==1 && colorDesc.SampleDesc.Count==1 &&
            colorDesc.SampleDesc.Quality==0 && !(colorDesc.BindFlags&D3D11_BIND_DEPTH_STENCIL) &&
            colorDesc.Usage==D3D11_USAGE_DEFAULT && colorDesc.CPUAccessFlags==0 && colorDesc.MiscFlags==0;
        if(!directColor)
            s.context->CopySubresourceRegion(h.color.Get(),0,0,0,0,b.inputs[b.colorSlot].Get(),b.inputSubresource[b.colorSlot],nullptr);
        // The original Bridge writes these exact decoded SR inputs straight
        // into shared UAVs. Exported consumers retain the actual prepare slot,
        // so a second depth/MV copy is unnecessary on the known full stack.
        const auto selectedInputs=s.AcquireSrInputs(h);
        const auto& srInputs=selectedInputs.inputs;
        // Pin before any prepare GPU write, even if receipt creation or Submit
        // later fails before a ProducerFrame/transport exists.
        flight.preparedInputs=srInputs;
        Storage::PrepareControlSlot* control=nullptr;
        if(srInputs && selectedInputs.slot<h.srInputSlots.size() &&
           h.srInputSlots[selectedInputs.slot]==srInputs)
            control=&h.prepareControlSlots[selectedInputs.slot];
        const std::array<UINT,4> controls{s.settings.depthFlipY,s.settings.motionFlipY,0,0};
        // Retain the exact CB with the same query/transport Flight as its inputs.
        // Slot authority comes only from AcquireSrInputs, never a frame counter.
        flight.prepareConstants=control?control->constants:s.constants;
        ID3D11Buffer* cb=flight.prepareConstants.Get();
        // The fallback CB was initialized with this pipeline's immutable
        // controls. Each shared-input slot uploads only on its first safe use.
        if(control && (!control->uploadedValid || control->uploaded!=controls)) {
            s.context->UpdateSubresource(cb,0,nullptr,controls.data(),0,0);
            control->uploaded=controls;control->uploadedValid=true;
        }
        // Binding is per dispatch even on a clean slot: context state is shared
        // with the game and restored by the existing context-state guard.
        s.context->CSSetConstantBuffers(0,1,&cb);
        ID3D11ShaderResourceView* views[2]{b.srvs[b.depthSlot].Get(),b.srvs[b.motionSlot].Get()};
        ID3D11UnorderedAccessView* uavs[2]{srInputs?srInputs->Uav(0):h.depthUav.Get(),
            srInputs?srInputs->Uav(1):h.motionUav.Get()};
        s.context->CSSetShader(s.prepare.Get(),nullptr,0);s.context->CSSetShaderResources(0,2,views);
        s.context->CSSetUnorderedAccessViews(0,2,uavs,nullptr);s.context->Dispatch((p.renderWidth+7)/8,(p.renderHeight+7)/8,1);
        ID3D11ShaderResourceView* noViews[2]{};ID3D11UnorderedAccessView* noUavs[2]{};
        s.context->CSSetShaderResources(0,2,noViews);s.context->CSSetUnorderedAccessViews(0,2,noUavs,nullptr);
        SrCaptureFrame frame(cameraDemand);
        frame.parameters=p;frame.color=directColor?b.inputs[b.colorSlot].Get():h.color.Get();
        frame.depth=srInputs?srInputs->Texture(0):h.depth.Get();
        frame.motion=srInputs?srInputs->Texture(1):h.motion.Get();frame.output=h.output.Get();
        // Retire at the actual SR cadence, not once for every unrelated game
        // draw. Present/resize drain and Prepare's own collection remain intact.
        GenshinBridgeFg::Collect();
        GenshinGameMotion::Collect();
        // The backend writes only a capture-owned output (private or exact
        // shared candidate). It can never partially write the game's RTV.
        frame.replaySafe=true;
        substagePrepared=true;
        GenshinSubstage::Event(GenshinSubstage::Stage::PrivateInputs,GenshinSubstage::State::Succeeded);
        // Freeze CPU continuity while the old subscription is still published.
        // This cannot admit the new producer or assert GPU completion.
        DrawObservationSubscription srOnlyHandoff{};
        if(!collectGuides && ObservationOnlyCurrent(s.context.Get(),s.configEpoch))srOnlyHandoff=drawObservation;
        frame.guides=GenshinGuides::Receipt::Observe(s.context.Get(),p,{frame.color,frame.depth,frame.motion,frame.output},s.deviceEpoch,s.ownerEpoch,s.configEpoch,beforeNative?nullptr:&s.native,returnedRaster,frame.cameraDemand && cameraSample?&*cameraSample:nullptr,
            srOnlyHandoff.srOnly?
                GenshinGuides::ObservationSubscription::SrReceiptOnly:GenshinGuides::ObservationSubscription::Full);
        // Producer replacement withdraws the optional subscription. Only the
        // exact new invocation's no-escaped-reader check below can restore it;
        // its issued SR flight is still pending and retires normally.
        ownerObservation.srOnly=false;
        // The previous exact-generation transport is retained by its Flight;
        // advancing history must neither relabel nor revoke that issued lease.
        h.observationProducerAttempted=true;
        h.transport.reset();
        // Missing typed-guide evidence keeps native SR available but cannot open a transport.
        if(frame.guides)h.transport=std::make_shared<GenshinTransport::ProducerFrame>(frame.guides,
            std::array<ID3D11Resource*,4>{frame.color,frame.depth,frame.motion,frame.output},s.rawLease,
            srInputs,srOutput); // Preserve exact input/output provenance through any late control change.
        flight.transport=h.transport;
        GenshinTransport::ProducerInvocation invocation(h.transport);
        // A UI change can arrive while private input copies are being recorded.
        // End the outer query even when no provider submission is admitted.
        const bool submitCurrent=Config::Instance()->GenshinControlsCurrent(s.configEpoch);
        GenshinFsr::Result result;result.reason=GenshinFsr::Reason::Draining;
        hostPrepare.Finish();r448Prepare.Finish();
        if(submitCurrent){
            GenshinCpuStageTiming::Scope hostSubmit(GenshinCpuStageTiming::Stage::HostSubmit); GenshinHookCpuTiming::Scope r448HostSubmit(GenshinHookCpuTiming::Stage::HostSubmit);
            // Mint only inside the actual outer scratch swap and only for our
            // synchronous owner. Custom consumers cannot inherit this shortcut.
            // Dx11Owner nests its own private state swap before Evaluate and
            // restores it before returning here. No game draw runs in this call.
            InternalSrStateScope isolated(s.context.Get(),s.scratch.Get(),previous.Get(),
                s.consumer.submit?nullptr:h.transport,frame);
            result=s.Submit(frame,h);++s.status.submitted;++h.srCadenceReturns;
        }
        // Preserve the provider's existing unsafe-replay disposition. Ordinary
        // private-output failures replay native; uncertain ownership/state does not.
        recoveryRequired=beforeNative && result.DrawAction()==Action::RequireRecovery;
        const bool resultCurrent=submitCurrent && Config::Instance()->GenshinControlsCurrent(s.configEpoch);
        // A new reader before commit gets the original draw. The resource-only
        // SR receipt above contains no native profile, returned raster or camera.
        const auto replacementCurrent=[&]()noexcept {
            return !beforeNative || (srOnlyHandoff.srOnly && srOnlyHandoff.owner==this &&
                srOnlyHandoff.context==s.context.Get() && srOnlyHandoff.thread==GetCurrentThreadId() &&
                srOnlyHandoff.configEpoch==s.configEpoch && srOnlyHandoff.hoYo.Current() &&
                OptionalConsumersEffectivelyOff());
        };
        if(!resultCurrent)RevokeTransport();
        action=resultCurrent && replacementCurrent()?result.DrawAction():Action::ContinueOriginal;
        GenshinSubstage::Event(GenshinSubstage::Stage::ConsumerAcceptance,
            action==Action::OmitWithReplacement?GenshinSubstage::State::Succeeded:
            result.reason==GenshinFsr::Reason::Pending?GenshinSubstage::State::Running:GenshinSubstage::State::Failed,
            GenshinSubstage::Resource::NativeOutput,GenshinSubstage::Domain::Fsr,static_cast<int64_t>(result.reason),
            action==Action::OmitWithReplacement?GenshinSubstage::Issue::None:GenshinSubstage::Issue::Rejected);
        const bool claimAttempted=action==Action::OmitWithReplacement;
        // Dx11Owner's lifecycle receipt carries canonical IUnknown identity, not
        // the Texture2D interface address. Preserve the original RTV resource
        // interface used by NativeFrame for the destination claim as well.
        ComPtr<IUnknown> privateOutputIdentity;
        if(claimAttempted && h.output)h.output.As(&privateOutputIdentity);
        if(claimAttempted)GenshinSubstage::Event(GenshinSubstage::Stage::NativeClaim,GenshinSubstage::State::Running);
        const auto claim=claimAttempted?s.native.ClaimCopyback(result,p,
            reinterpret_cast<uintptr_t>(privateOutputIdentity.Get()),reinterpret_cast<uintptr_t>(b.outputResources[b.outputSlot].Get()),
            reinterpret_cast<uintptr_t>(s.context.Get()),token.generation):NativeMrt::Reason::ProviderRejected;
        if(claimAttempted)GenshinSubstage::Event(GenshinSubstage::Stage::NativeClaim,
            claim==NativeMrt::Reason::Ready?GenshinSubstage::State::Succeeded:GenshinSubstage::State::Failed,
            GenshinSubstage::Resource::NativeOutput,GenshinSubstage::Domain::NativeClaim,static_cast<int64_t>(claim));
        if(claimAttempted && claim==NativeMrt::Reason::Ready &&
           Config::Instance()->GenshinControlsCurrent(s.configEpoch) && replacementCurrent())
        {
            GenshinCpuStageTiming::Scope hostCopyback(GenshinCpuStageTiming::Stage::HostGameCopyback); GenshinHookCpuTiming::Scope r448GameCopyback(GenshinHookCpuTiming::Stage::GameCopyback);
            s.substageOutputPending=true;
            GenshinSubstage::Event(GenshinSubstage::Stage::NativeOutput,GenshinSubstage::State::Running);
            bool resumeObservation=false;
            // Full consumers never enter this handoff. Recheck the actual call,
            // pipeline and new producer, independently of the withdrawn owner
            // subscription; the frozen token itself grants no reader authority.
            if(srOnlyHandoff.srOnly && OptionalConsumersEffectivelyOff() && srOnlyHandoff.hoYo.Current() &&
                !s.invalidated && srOnlyHandoff.owner==this && srOnlyHandoff.context==s.context.Get() &&
                srOnlyHandoff.thread==s.thread && GetCurrentThreadId()==s.thread &&
                srOnlyHandoff.deviceEpoch==s.deviceEpoch && srOnlyHandoff.ownerEpoch==s.ownerEpoch &&
                srOnlyHandoff.configEpoch==s.configEpoch && Config::Instance()->GenshinControlsCurrent(s.configEpoch) &&
                h.transport && GenshinTransport::ProducerInvocation::Current()==h.transport){
                const auto& call=GenshinDepthMapping::CurrentDraw();
                const auto& owner=h.transport->Owner();
                resumeObservation=call.context==s.context.Get() && call.thread==s.thread &&
                    call.observationCall && call.observationCall==srOnlyHandoff.call &&
                    owner.device==reinterpret_cast<uint64_t>(s.device.Get()) &&
                    owner.context==reinterpret_cast<uint64_t>(s.context.Get()) && owner.thread==s.thread &&
                    owner.deviceEpoch==s.deviceEpoch && owner.producerEpoch==s.ownerEpoch && owner.configEpoch==s.configEpoch &&
                    owner.instance==p.instance && owner.historyEpoch==p.epoch && owner.generation==p.generation &&
                    owner.sourceFrame==p.sourceFrame && h.transport->ObservationNoEscapedReaders(owner) &&
                    OptionalConsumersEffectivelyOff() && srOnlyHandoff.hoYo.Current() &&
                    Config::Instance()->GenshinControlsCurrent(s.configEpoch);
            }
            if(resumeObservation){ownerObservation=srOnlyHandoff;ownerObservation.call=0;}
            s.context->CopySubresourceRegion(b.outputs[b.outputSlot].Get(),b.outputSubresource[b.outputSlot],0,0,0,h.output.Get(),0,nullptr);++s.status.outputCopies;
            replacementCopied=true;
            if(beforeNative)++s.status.beforeNativeReplacements;
            s.pendingRepairTrace.outer.copyOrdered=true;
            ++h.srCadenceCopies;
            // The SR copy is ordered on the same immediate context subsequently
            // used to capture the real final backbuffer at Present. No scene guide
            // is claimed to describe game postprocessing or HUD here.
            const GenshinPresentNr::Source copiedSource={GenshinPresentNr::Identity(s.device.Get()),GenshinPresentNr::Identity(s.context.Get()),
                reinterpret_cast<uintptr_t>(privateOutputIdentity.Get()),GenshinPresentNr::Identity(b.outputResources[b.outputSlot].Get()),
                s.deviceEpoch,s.ownerEpoch,s.configEpoch,p.instance,p.generation,p.sourceFrame,s.thread,
                p.outputWidth,p.outputHeight,b.outputSubresource[b.outputSlot],s.substageKey,
                s.pendingRepairTrace.outer.seq,GenshinPresentNrTrace::Sequence(),0};
            h.canonicalSource=copiedSource;
            h.canonicalSource.attempt=s.substageAttempt;
            h.canonicalSource.historyEpoch=h.epoch;
            // This identity records the real SR copy, including for a later
            // original paired draw's existing repair path. It is not a native
            // draw/returned-raster capability and does not qualify typed guides.
            h.generation=h.canonicalGeneration=p.generation;h.canonicalHistoryEpoch=h.epoch;
            if(!beforeNative) {
                s.presentNrSource=h.canonicalSource;
                s.presentNrContent.Copied(s.presentNrSource);
            } else {
                s.presentNrSource={};s.presentNrContent.Revoke();
            }
            h.completedTick=GetTickCount64();h.lastTime=now;
            if(frame.cameraDemand) {
                // Camera history advances only at this actual canonical SR copyback.
                // Optional camera failure cannot prevent successful SR or native NR.
                const auto cameraGrid=frame.guides?frame.guides->OutputGrid():std::nullopt;
                const auto outputCamera=GenshinCameraHistory::ForSrOutput(cameraSample,cameraGrid);
                h.cameraHistory.Commit(outputCamera,p.reset);
                // Seal camera history at the same successful original-Bridge SR
                // output boundary as its queued resource packet. A missed Present
                // must not erase an otherwise real adjacent producer transaction.
                // With FG disabled no packet may escape merely to certify future FG
                // history. The next FG-enabled canonical frame certifies itself again;
                // keeping this SR-only packet local also permits safe surface reuse.
                const bool fgDemand=GenshinFgConsumer::NewWorkRequested();
                const auto bridgePacket=fgDemand && h.transport?h.transport->QueuedFgTransfer():nullptr;
                const auto bridgeGuides=fgDemand && bridgePacket && frame.guides?
                    frame.guides->NativeGuideForPresent():nullptr;
                if(outputCamera && bridgePacket && bridgeGuides &&
                   GenshinGuides::SameOwner(bridgePacket->Get().owner,outputCamera->owner))
                    h.cameraHistory.Certify(outputCamera->owner);
                static unsigned packetReports=0;if(packetReports++<8)
                    LOG_INFO("R840 Bridge packet: pid={} generation={} source_frame={} packet={} native_guides={} camera={} queued_only=true cpu_completion_not_claimed=true",
                        GetCurrentProcessId(),p.generation,p.sourceFrame,bool(bridgePacket),bool(bridgeGuides),bool(outputCamera));
                static unsigned cameraCommitReports=0;if(cameraCommitReports<16){++cameraCommitReports;
                    LOG_INFO("R827 FG camera commit: pid={} generation={} frame={} camera={} grid={} output_camera={} reset={} grid_owner_matches={} no_authority=true",
                        GetCurrentProcessId(),p.generation,p.sourceFrame,bool(cameraSample),bool(cameraGrid),bool(outputCamera),p.reset,
                        cameraSample && cameraGrid && cameraGrid->Matches(cameraSample->owner));
                }
            }
            if(h.gameOutputs[0].Get()!=b.outputs[b.outputSlot].Get() || h.gameOutputSubresources[0]!=b.outputSubresource[b.outputSlot]){
                h.gameOutputs[1]=h.gameOutputs[0];h.gameOutputSubresources[1]=h.gameOutputSubresources[0];
                h.gameOutputs[0]=b.outputs[b.outputSlot];h.gameOutputSubresources[0]=b.outputSubresource[b.outputSlot];}
            h.previousJitterX=cp.jitter_x;h.previousJitterY=cp.jitter_y;h.havePreviousJitter=true;
            il2cpp_callsite::consume_render_token_for(cp.instance,token.generation);
            s.status.reason=CaptureReason::OutputQueued;s.status.srAccepted=true;
            if(accumulate && familyAllowed)
            {
                s.familyAccumulate=accumulateShader;
                s.family.Notify(shader,reinterpret_cast<uintptr_t>(s.device.Get()),reinterpret_cast<uintptr_t>(s.context.Get()),
                    s.ownerEpoch,true,GetTickCount64());++s.status.familyReplacements;
            }
        }
        else{ownerObservation.srOnly=false;const bool retryableTimeout=result.unsubmittedRecordingTimeout &&
            result.reason==GenshinFsr::Reason::Pending && !result.pendingLease;
            const bool unsupportedExposure=result.unsubmittedManualExposure &&
                result.reason==GenshinFsr::Reason::InvalidParameters && !result.pendingLease &&
                !s.consumer.submit;
            const bool backendRebuild=result.transition==GenshinFsr::Transition::UnsubmittedBackendRebuild &&
                result.reason==GenshinFsr::Reason::Pending && result.disposition==GenshinFsr::Disposition::RunNative &&
                result.stateRestored && !result.pendingLease && !s.consumer.submit &&
                (!h.transport || (h.transport->Live() && !h.transport->LeaseCreated()));
            if(h.transport && !backendRebuild)h.transport->Revoke();action=Action::ContinueOriginal;
            if(!Config::Instance()->GenshinControlsCurrent(s.configEpoch))RevokeTransport();
            else if(backendRebuild) {
                // Keep the persistent SR key for construct/init on later frames.
                // Canonical/guide/camera authority is gone; flights, queries and
                // prepared input/output owners still retire through their real gates.
                h.canonicalGeneration=h.canonicalHistoryEpoch=h.completedTick=0;h.canonicalSource={};
                if(s.presentNrSource.instance==h.instance) {
                    s.presentNrContent.Revoke();s.presentNrSource={};s.presentNrPictures.Revoke();
                }
                h.cameraHistory.Clear();
                s.status.reason=CaptureReason::Pending;s.status.srAccepted=false;
            }
            else if(unsupportedExposure) {
                // Preparation commands and their outer flight/query still
                // retire normally; no provider lifecycle submission exists.
                h.manualExposureUnavailable=true;
                s.status.reason=CaptureReason::ConsumerFailed;s.status.srAccepted=false;
            }
            else if(retryableTimeout) {
                s.status.reason=CaptureReason::Pending;
                static std::atomic<unsigned> reports{0};
                if(reports.fetch_add(1,std::memory_order_relaxed)<8)
                    LOG_INFO("R23 SR unsubmitted timeout pending: pid={} instance={} generation={} native_preserved=true retry=true no_authority=true",
                        GetCurrentProcessId(),p.instance,p.generation);
            } else {s.status.reason=CaptureReason::ConsumerFailed;s.status.srAccepted=false;h.failed=true;h.generation=token.generation;}
            il2cpp_callsite::consume_render_token_for(cp.instance,token.generation);}
    }
    catch(...){ownerObservation.srOnly=false;GenshinSubstage::Event(substagePrepared?GenshinSubstage::Stage::ConsumerAcceptance:GenshinSubstage::Stage::PrivateInputs,GenshinSubstage::State::Failed,
        GenshinSubstage::Resource::Inputs,GenshinSubstage::Domain::Fsr,0,GenshinSubstage::Issue::Exception);
        if(h.transport)h.transport->Revoke();s.status.reason=CaptureReason::PreparationFailed;s.status.srAccepted=false;h.failed=true;h.generation=token.generation;
        il2cpp_callsite::consume_render_token_for(cp.instance,token.generation);}
    s.context->End(completion.Get());
    s.pendingRepairTrace.outer.endSeq=GenshinPresentNrTrace::Sequence();
    if(s.pendingRepairTrace.target.draw==s.status.observedDraws)s.pendingRepairTrace.target.query=s.pendingRepairTrace.outer;
    {GenshinCpuStageTiming::Scope hostStateRestore(GenshinCpuStageTiming::Stage::HostStateRestore); GenshinHookCpuTiming::Scope r448StateRestore(GenshinHookCpuTiming::Stage::StateRestore);
        s.context1->SwapDeviceContextState(previous.Get(),nullptr);}
    // One CPU receipt for the first actual result after an explicit request/create.
    // The immutable ticket was captured before SR. No new query, wait or pixel claim.
    GenshinSubstage::PresetSrReturned(presetSrTicket,
        {s.substageKey,p.instance,p.generation,p.sourceFrame,p.epoch},replacementCopied);
    // Reuse the existing attempt timestamp. This is canonical producer cadence,
    // not displayed/base Present FPS and not the reciprocal of an SR GPU time.
    const auto cadenceMs=std::chrono::duration<double,std::milli>(now-h.srCadenceWindow).count();
    if(cadenceMs>=1000.0) {
        const auto& cfg=*Config::Instance();
        try {
            LOG_INFO("R044 SR cadence: pid={} instance={} generation={} source_frame={} window_ms={:.1f} inputs={} returns={} copies={} input_hz={:.2f} return_hz={:.2f} copy_hz={:.2f} fg={} nr={} base_present_fps=UNKNOWN",
                GetCurrentProcessId(),p.instance,p.generation,p.sourceFrame,cadenceMs,h.srCadenceInputs,h.srCadenceReturns,
                h.srCadenceCopies,1000.0*h.srCadenceInputs/cadenceMs,1000.0*h.srCadenceReturns/cadenceMs,
                1000.0*h.srCadenceCopies/cadenceMs,cfg.FGEnabled.value_or_default(),cfg.DlssNrEnabled.value_or_default());
        }catch(...){} // Cadence observation cannot change SR acceptance.
        h.srCadenceWindow=now;h.srCadenceInputs=h.srCadenceReturns=h.srCadenceCopies=0;
    }
    // Only a real ordered game-RTV copy can omit the native terminal draw.
    // The existing context restore and Flight ownership remain mandatory above.
    if(beforeNative)return replacementCopied?Action::OmitWithReplacement:
        recoveryRequired?Action::RequireRecovery:Action::ContinueOriginal;
    return action;
}
bool CapturePipeline::Poll(){return storage_->Poll();}
NoSrConsumer::Receipt CapturePipeline::GetCaptureReceipt() const noexcept{return storage_->captureReceipt;}
Engine::Receipt CapturePipeline::GetEngineProducerReceipt() const noexcept{return storage_->engineProducer.GetReceipt();}
NoSrConsumer::Receipt CapturePipeline::PresentCapture(const CapturePresentReceipt& present) noexcept
{
    // A callback declaration alone is no longer a completion witness.
    auto receipt=storage_->captureReceipt;receipt.reason=NoSrConsumer::Reason::PresentMismatch;return receipt;
}
std::shared_ptr<const CaptureSnapshot> CapturePipeline::GetCaptureSnapshot() const noexcept
{
    const auto& s=*storage_;
    if(GetCurrentThreadId()!=s.thread || s.invalidated || s.capturePending || !s.captureReceipt.admitted ||
        !s.captureSnapshot || !s.captureSnapshot->gpuCompleted)return {};
    return s.captureSnapshot;
}
std::shared_ptr<const RawObservation::Snapshot> CapturePipeline::GetRawObservationSnapshot() const noexcept
{const auto& s=*storage_;return GetCurrentThreadId()==s.thread && !s.invalidated && s.device &&
    SUCCEEDED(s.device->GetDeviceRemovedReason())?s.rawObservation.Get():nullptr;}
RawObservation::Receipt CapturePipeline::GetRawObservationReceipt() const noexcept
{auto result=storage_->rawObservation.GetReceipt();result.current=static_cast<bool>(GetRawObservationSnapshot());return result;}
bool CapturePipeline::Invalidate()
{
    GenshinDepthProvider::RevokePreview();
    auto& s=*storage_;s.invalidated=true;s.status.srAccepted=false;s.status.effectiveFamilySkip=false;
    s.native.Revoke();s.pendingNative.reset();s.RevokeRepair();s.pendingAccumulate.Reset();
    s.status.reason=CaptureReason::Draining;s.family.Reset();s.familyAccumulate.Reset();s.presentations.Reset();s.nativePresentReceipt={};s.engineProducer.Reset();
    s.presentTarget={};s.presentTargetSwapchain=s.presentTargetGeneration=0;
    s.rawObservation.Revoke();
    s.runtimeInputs.Revoke();
    // Revoke before the thread check. Keep every pending GPU lease and provider owner alive.
    for(auto& flight:s.flights)if(flight.transport)flight.transport->Revoke();
    for(auto& h:s.histories)if(h){if(h->transport)h->transport->Revoke();if(h->owner)h->owner->RevokeGuides();}
    if(GetCurrentThreadId()!=s.thread){s.status.reason=CaptureReason::ThreadMismatch;return false;}
    GenshinBridgeFg::Collect();
    GenshinGameMotion::Collect();
    // A D3D11 completion query cannot retire a DX12 submission whose fence failed.
    // Keep the actual provider owner alive until every transport lease is complete.
    if(!s.Poll())return false;
    for(auto& h:s.histories)if(h && h->owner)h->owner->Invalidate();
    if(s.consumer.invalidate){PsState::Mutation opaque(s.context.Get());s.consumer.invalidate(s.consumer.opaque);}
    return s.Poll();
}
void CapturePipeline::RevokeTransport() noexcept
{
    GenshinDepthProvider::RevokePreview();
    auto& s=*storage_;s.invalidated=true;s.status.reason=CaptureReason::Draining;
    s.status.srAccepted=false;s.status.effectiveFamilySkip=false;
    // Revoke permits immediately; pending GPU owners and rawLease remain retained.
    s.native.Revoke();s.pendingNative.reset();s.RevokeRepair();s.pendingAccumulate.Reset();
    s.family.Reset();s.familyAccumulate.Reset();s.presentNrSource={};
    s.rawObservation.Revoke();
    s.runtimeInputs.Revoke();
    // Native callers serialize metadata. Submitted leases/query owners remain retained.
    for(auto& flight:s.flights)if(flight.transport)flight.transport->Revoke();
    for(auto& h:s.histories)if(h && h->transport)h->transport->Revoke();
}
bool CapturePipeline::NeedsDrain() const noexcept{return storage_->invalidated;}
void CapturePipeline::SubscribeDrawObservation(bool full) noexcept {storage_->fullDrawObservation=full;}
bool CapturePipeline::ObservationRetirementsEmpty() noexcept {
    return Storage::RetiredCount().load(std::memory_order_acquire)==0;
}
bool CapturePipeline::ObservationReadersIdle() const noexcept {
    const auto& s=*storage_;
    if(GetCurrentThreadId()!=s.thread || s.invalidated || !s.context || !s.device || !s.deviceEpoch || !s.ownerEpoch || !s.configEpoch ||
        !ObservationRetirementsEmpty() ||
        !GenshinTransport::ProducerFrame::ObservationRetirementsEmpty() ||
        !GenshinTransport::ObservationRetirementsEmpty())return false;
    // Issued SR work needs its real query/fence owners, not observations of
    // unrelated game draws. Account for every retained producer, including
    // previous generations still held by Flights, without requiring GPU idle.
    std::vector<const GenshinTransport::ProducerFrame*> producers;
    const auto account=[&](const std::shared_ptr<GenshinTransport::ProducerFrame>& producer) {
        if(!producer)return true;
        if(std::find(producers.begin(),producers.end(),producer.get())!=producers.end())return true;
        const auto& owner=producer->Owner();
        if(owner.device!=reinterpret_cast<uint64_t>(s.device.Get()) ||
            owner.context!=reinterpret_cast<uint64_t>(s.context.Get()) || owner.thread!=s.thread ||
            owner.deviceEpoch!=s.deviceEpoch || owner.producerEpoch!=s.ownerEpoch ||
            owner.configEpoch!=s.configEpoch || !owner.instance || !owner.generation || !owner.historyEpoch ||
            !producer->ObservationNoEscapedReaders(owner))return false;
        producers.push_back(producer.get());return true;
    };
    try {
        for(const auto& h:s.histories)if(h){
            if(!h->instance || !h->epoch || h->failed ||
                (!h->transport && h->observationProducerAttempted) || !account(h->transport))return false;
        }
        for(const auto& flight:s.flights)if(!account(flight.transport))return false;
    }catch(...){return false;}
    return GenshinTransport::ProducerFrame::ObservationOwnersAccounted(producers.size()) &&
        SUCCEEDED(s.device->GetDeviceRemovedReason());
}
bool CapturePipeline::DrawPairPending() const noexcept {
    const auto& s=*storage_;
    return s.pendingNative || s.pendingRepair || s.pendingAccumulate || s.native.Pending();
}
bool CapturePipeline::OptionalObservationIdle() const noexcept {
    auto& s=*storage_;
    if(GetCurrentThreadId()!=s.thread || !s.context)return false;
    // Present/claim only, with no optional consumer. Retire CPU correspondence;
    // mapping pools and samples keep their original EVENT/reader owners.
    s.ObserveCameraDemand(false);s.depthMapping.Revoke();
    // CPU Before/After pairs and optional observers must drain. Ordinary SR
    // Flights/completion/rawLease remain retained and retire at producer/Present;
    // they cannot require native raster observations when no reader escaped.
    if(s.invalidated || s.configurePending || s.retirementFailed || DrawPairPending() || s.capturePending ||
        s.pairedLease.Occupied() || !s.retiredPairs.empty() ||
        s.captureMode!=CaptureOnlyMode::Disabled || s.settings.captureOnlyObservation ||
        s.consumer.opaque || s.consumer.submit || s.consumer.poll || s.consumer.invalidate ||
        s.depthMapping.Pending(s.context.Get()) || s.rawObservation.GetReceipt().pending)return false;
    // Caller first proves capture has NEVER attempted/armed in this process
    // state. Queue requires Enabled(), and Start publishes attempted before
    // active; ResetForManual publishes manual=true. Never infer idle from Stop
    // or scan InputObserver's 48 slots on each otherwise-unsubscribed draw.
    return ObservationReadersIdle();
}
Presentation::Ticket CapturePipeline::BeginPresent(std::uint64_t swapchain,UINT flags,bool present1) noexcept
{
    auto& s=*storage_;if(GetCurrentThreadId()!=s.thread || s.invalidated || !s.device || !s.context)return {};
    // Present can observe a disabled interval even when no new SR is admitted.
    s.ObserveCameraDemand(OptionalGuideDemand());
    Presentation::Frame frame;
    if(s.captureSnapshot && (s.capturePending || s.captureReceipt.admitted)){
        const auto& signal=s.captureSnapshot->signal;il2cpp_callsite::RenderToken current;
        const auto matches=il2cpp_callsite::match_capture_render_token(signal.nativeSize.width,signal.nativeSize.height,
            signal.outputSize.width,signal.outputSize.height,s.minimumCaptureGeneration,current);
        if(signal.swapchain==swapchain && matches==1 && current.generation==s.captureSnapshot->renderGeneration &&
            current.params.instance==signal.instance && current.params.frame_index==signal.sourceFrame)
            frame={signal.instance,current.generation,signal.sourceFrame,signal.producerEpoch};
    }
    return s.presentations.Begin(s.PresentOwner(),swapchain,frame,flags,present1);
}
Presentation::Receipt CapturePipeline::CompletePresent(const Presentation::Ticket& ticket,HRESULT result) noexcept
{
    auto& s=*storage_;if(GetCurrentThreadId()!=s.thread || s.invalidated)return {};
    auto receipt=s.presentations.Complete(ticket,result,s.PresentOwner());
    if(receipt.recorded){s.nativePresentReceipt=receipt;s.Poll();
        if(s.signalProvider==&Engine::Producer::Publish){const auto engine=s.engineProducer.GetReceipt();
            il2cpp_callsite::RenderToken current;
            const bool same=s.presentTargetSwapchain==ticket.swapchain && s.presentTargetGeneration==engine.sourceGeneration &&
                il2cpp_callsite::match_capture_render_token(engine.candidate.nativeSize.width,engine.candidate.nativeSize.height,
                    engine.candidate.outputSize.width,engine.candidate.outputSize.height,s.minimumCaptureGeneration,current)==1 &&
                current.generation==engine.sourceGeneration && current.params.instance==engine.candidate.instance && current.params.frame_index==engine.candidate.sourceFrame;
            s.engineProducer.NotePresent(receipt,s.presentTarget,same);
            const auto observed=s.engineProducer.GetReceipt();
            if(RuntimeCapture::Enabled() && observed.actualRenderWitness){auto r=s.RouteObservation(observed);
                r.kind=RuntimeCapture::RouteFact::Kind::PresentCompleted;r.swapchain=receipt.ticket.swapchain;
                r.presentSerial=receipt.completionSerial;r.presentResult=static_cast<int32_t>(receipt.rawResult);
                r.presentRecorded=receipt.recorded;r.successfulNonTest=receipt.successfulNonTest;r.sameRender=same;
                r.directIdentityMatch=observed.directOutputMatchesBackbuffer;r.backbuffer=observed.presentedBackbuffer;
                RuntimeCapture::ObserveRoute(r);}}
        s.presentTarget={};s.presentTargetSwapchain=s.presentTargetGeneration=0;}
    return receipt;
}
Presentation::Receipt CapturePipeline::GetPresentReceipt()const noexcept{return storage_->nativePresentReceipt;}
void CapturePipeline::ForgetSwapchain(std::uint64_t swapchain) noexcept
{
    auto& s=*storage_; // Native entry serializes metadata even on a COM-release thread.
    // No authenticated history-to-swapchain mapping exists for ordinary SR.
    // Conservatively retire this pipeline on a real nonzero teardown event.
    if(swapchain){RevokeTransport();if(GetCurrentThreadId()==s.thread)s.presentNrPictures.ClearOwner();}
    s.presentations.Forget(swapchain);
    if(s.nativePresentReceipt.ticket.swapchain==swapchain)s.nativePresentReceipt={};
    if(s.signalProvider==&Engine::Producer::Publish){s.engineProducer.Reset();
        s.invalidated=true;s.status.reason=CaptureReason::Draining;}
    if(s.captureSnapshot && s.captureSnapshot->signal.swapchain==swapchain){
        s.invalidated=true;s.status.reason=CaptureReason::Draining;s.status.srAccepted=false;s.status.effectiveFamilySkip=false;}
    if(s.invalidated)for(auto& h:s.histories)if(h){
        if(h->transport)h->transport->Revoke();if(h->owner)h->owner->RevokeGuides();}
}
// This predicate only selects the CPU PS-cache scope for this return callback.
// It never skips Before/After observation, shader consumers or GPU retirement.
// AfterOriginalDraw reads PS for the three-element SR/repair path, the
// Scene/Picture shapes (3/6), or an active post-copy trace. Keep all of those
// conservative, even when their later source/subscription checks will refuse.
static bool ReturnedDrawMayReadShader(UINT count,bool countKnown) noexcept
{
    const auto* trace=GenshinPresentNrTrace::Current();
    return (countKnown && (count==3 || count==6)) || (trace && trace->copy);
}
void CapturePipeline::AfterOriginalDraw(UINT count,bool countKnown) noexcept
{
    GenshinCpuStageTiming::Scope hostReturn(GenshinCpuStageTiming::Stage::HostNativeReturn);
    auto& s=*storage_;if(s.invalidated || GetCurrentThreadId()!=s.thread || !s.context || !s.device)return;
    if(!Config::Instance()->GenshinControlsCurrent(s.configEpoch)){RevokeTransport();return;}
    if(auto* trace=GenshinPresentNrTrace::Current();trace && trace->copy) {
        const auto shader=Family::Current(s.context.Get(),s.device.Get(),s.deviceEpoch);
        GenshinPresentNrTrace::PostCopyDraw(s.context.Get(),s.status.observedDraws,shader.hash);
    }
    if(s.fullDrawObservation){
        s.depthMapping.Returned(s.context.Get());
        s.ObservePresentPicture(count,countKnown);
    }
    if(!countKnown || count!=3)return;
    if(s.pendingRepair){s.CompleteRepair();
        if(!Config::Instance()->GenshinControlsCurrent(s.configEpoch))RevokeTransport();return;}
    if(s.native.Pending()) {
        Bindings currentBindings;currentBindings.targetProfile=true;il2cpp_callsite::RenderToken currentToken;
        const bool matched=s.pendingNative && ReadBindings(s.context.Get(),currentBindings,s.pendingNative.get()) &&
            MatchToken(currentBindings,currentToken,s.pendingToken.params.instance)==1;
        NativeMrt::Reason reason=NativeMrt::Reason::BindingsChanged;
        if(matched){const auto shader=Family::Current(s.context.Get(),s.device.Get(),s.deviceEpoch);
            const auto current=NativeFrame(s.device.Get(),s.context.Get(),currentBindings,currentToken,shader,
                s.deviceEpoch,s.ownerEpoch,s.configEpoch,s.thread,s.status.observedDraws,s.settings);
            reason=s.native.Returned(current,currentToken.generation);
            if(reason==NativeMrt::Reason::Ready)reason=s.native.ClaimOriginal();}
        s.status.nativeRefusal=static_cast<uint32_t>(reason);
        if(reason==NativeMrt::Reason::Ready) {
            ++s.status.nativeReturns;
            try{CompleteNativeReplacement();}catch(...){s.status.reason=CaptureReason::PreparationFailed;}
        } else s.status.reason=CaptureReason::UnsupportedBindings;
        s.pendingNative.reset();s.pendingRepair.reset();s.pendingAccumulate.Reset();s.native.Revoke();
    }
    if(s.signalProvider!=&Engine::Producer::Publish)return;
    const auto engine=s.engineProducer.GetReceipt();il2cpp_callsite::RenderToken current;
    if(engine.actualRenderWitness && il2cpp_callsite::match_capture_render_token(engine.candidate.nativeSize.width,engine.candidate.nativeSize.height,
        engine.candidate.outputSize.width,engine.candidate.outputSize.height,s.minimumCaptureGeneration,current)==1 && current.generation==engine.sourceGeneration)
    {   s.engineProducer.AfterOriginalDraw(current.generation);
        const bool selected=s.observedRouteReturnPending && s.observedRouteDrawSerial==s.status.observedDraws &&
            s.observedRouteGeneration==current.generation;
        s.observedRouteReturnPending=false;
        if(RuntimeCapture::Enabled() && selected)RuntimeCapture::ObserveRoute(s.RouteObservation(s.engineProducer.GetReceipt()));
    }
}
void CapturePipeline::ObservePresentTarget(IDXGISwapChain* swapchain,std::uint64_t identity) noexcept
{
    auto& s=*storage_;s.presentTarget={};s.presentTargetSwapchain=s.presentTargetGeneration=0;
    if(!swapchain || !identity || s.invalidated || GetCurrentThreadId()!=s.thread || s.signalProvider!=&Engine::Producer::Publish)return;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> three;Microsoft::WRL::ComPtr<ID3D11Texture2D> buffer;
    // Older flip chains have no exact current-index query; never assume buffer zero.
    if(FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&three))) || FAILED(three->GetBuffer(three->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&buffer))))return;
    const auto witness=ResourceProof::Identify(s.context.Get(),buffer.Get());
    if(!witness.resourceLifetime || witness.deviceEpoch!=s.deviceEpoch)return;
    s.presentTarget=witness;s.presentTargetSwapchain=identity;s.presentTargetGeneration=s.engineProducer.GetReceipt().sourceGeneration;
}
GenshinPresentNr::Ticket CapturePipeline::TakePresentNr(IDXGISwapChain* swapchain,uint64_t presentEpoch,
    std::shared_ptr<GenshinTransport::ProducerFrame>* acquiredProducer) noexcept
{
    if(acquiredProducer)acquiredProducer->reset();
    auto& s=*storage_;const auto& source=s.presentNrSource;
    const auto currentGeneration=source.instance?il2cpp_callsite::params_generation_for(source.instance):0;
    if(auto* r=GenshinPresentNrTrace::Current()) {
        r->sourceSeq=GenshinPresentNrTrace::Sequence();r->generation=source.generation;r->currentGeneration=currentGeneration;
        r->sourceFrame=source.sourceFrame;r->ownerEpoch=source.ownerEpoch;r->deviceEpoch=source.deviceEpoch;r->configEpoch=source.configEpoch;
        r->device=source.device;r->context=source.context;r->selected=source.selectedOutput;r->privateOutput=source.privateOutput;
        r->pairedCopies=s.presentNrPairedCopies;r->pairedDraw=s.presentNrPairedDraw;r->pairedOutput=s.presentNrPairedOutput;
        r->repairTargetDraw=s.repairTargetDraw;r->repairPollBlocked=s.repairPollBlocked;
        r->repairCanonical=s.repairCanonical;r->repairCpuGeneration=s.repairCpuGeneration;
        r->repairStage=s.repairStage;r->repairMask=s.repairMask;r->repairTokenMatches=s.repairTokenMatches;r->repairPollStop=s.repairPollStop;
        if(r->thread==s.thread){r->pendingRepair=s.pendingRepairTrace;r->pictureCandidate=s.pictureCandidate;r->sceneCandidate=s.sceneCandidate;}
        r->submitSerial=source.submitSerial;r->sourceCopySerial=source.copyOrderedSerial;r->gpuReadySerial=source.gpuReadySerial;
        r->pictureProbeSamples=0;r->pictureProbeRemaining=0; // Automatic diagnostic probes removed.
        r->selectedContent=s.presentNrContent.Matches(source);r->selectedNativeDraw=s.presentNrContent.NativeDraw();
        r->selectedContentCopy=s.presentNrContent.CopySerial();
        s.presentNrPictures.Trace();
    }
    D3D11_TEXTURE2D_DESC desc{};
    const auto reject=[&](const char* reason)->GenshinPresentNr::Ticket {
        if(auto* r=GenshinPresentNrTrace::Current()){r->ticketSeq=GenshinPresentNrTrace::Sequence();r->reject=reason;}
        if(GetCurrentThreadId()==s.thread) {
            if(strcmp(reason,"no-fresh-native-picture")==0)s.presentNrPictures.RejectUnfresh(source);
            else s.presentNrPictures.ClearOwner();
        }
        static uint64_t calls=0,lastAt=0;static const char* lastReason=nullptr;
        const auto count=++calls,now=GetTickCount64();
        if(count<=3 || count%300==0 || (reason!=lastReason && now-lastAt>=1000)) {
            lastReason=reason;lastAt=now;
            LOG_INFO("Genshin presented NR gate: pid={} stage=ticket reason={} calls={} thread={}/{} generation={}/{} consumedPicture={} sr={}x{} picture={}x{} samples={} array={} mips={} presentEpoch={} observedDraw={} consumedAtDraw={} consumedPresent={}",
                GetCurrentProcessId(),reason,count,GetCurrentThreadId(),s.thread,source.generation,currentGeneration,
                s.presentNrConsumedPicture,source.width,source.height,desc.Width,desc.Height,desc.SampleDesc.Count,
                desc.ArraySize,desc.MipLevels,presentEpoch,s.status.observedDraws,s.presentNrConsumedAtDraw,
                s.presentNrConsumedKey.present);
        }
        return {};
    };
    if(s.invalidated)return reject("owner-invalidated");
    if(!source.Known())return reject("no-successful-sr-source");
    if(!swapchain)return reject("no-swapchain");
    if(GetCurrentThreadId()!=s.thread)return reject("owning-thread");
    s.ObserveCameraDemand(OptionalGuideDemand());
    if(source.ownerEpoch!=s.ownerEpoch || source.deviceEpoch!=s.deviceEpoch || source.configEpoch!=s.configEpoch)
        return reject("owner-epoch");
    if(!s.presentNrContent.Matches(source))return reject("selected-content-unproven");
    // Render entry can run ahead of the draw/Present thread. The Bridge token
    // queue deliberately consumes the oldest valid generation; newer queued CPU
    // params do not invalidate a successful, ordered SR copy already in this picture.
    if(!currentGeneration || source.generation>currentGeneration)return reject("source-generation");
    Storage::History* successful=nullptr;
    for(const auto& history:s.histories)if(history && history->instance==source.instance)successful=history.get();
    const auto now=GetTickCount64();
    if(auto* r=GenshinPresentNrTrace::Current()) {
        r->historyFound=successful!=nullptr;r->historyFailed=successful && successful->failed;
        r->historyGeneration=successful?successful->generation:0;r->historyOutput=successful?GenshinPresentNr::Identity(successful->output.Get()):0;
        r->sourceAge=successful && successful->completedTick && now>=successful->completedTick?now-successful->completedTick:UINT64_MAX;
        // Keep the original combined refusal intact, expose its actual operands.
        r->sourceFailure=(!successful?1u:0u) | (successful && successful->failed?2u:0u) |
            (successful && successful->generation!=source.generation?4u:0u) |
            (successful && GenshinPresentNr::Identity(successful->output.Get())!=source.privateOutput?8u:0u) |
            (successful && !successful->completedTick?16u:0u) | (successful && now<successful->completedTick?32u:0u) |
            (successful && successful->completedTick && now>=successful->completedTick && now-successful->completedTick>250?64u:0u) |
            (successful && successful->canonicalGeneration!=source.generation?128u:0u);
    }
    if(!successful || successful->failed || successful->generation!=source.generation || successful->canonicalGeneration!=source.generation ||
       GenshinPresentNr::Identity(successful->output.Get())!=source.privateOutput ||
       !successful->completedTick || now<successful->completedTick || now-successful->completedTick>250)
        return reject("source-output-not-current");
    if(!s.device || FAILED(s.device->GetDeviceRemovedReason()))return reject("device-removed");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11Texture2D> picture;
    ComPtr<IDXGISwapChain3> indexed;
    UINT index=0;if(SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&indexed))))index=indexed->GetCurrentBackBufferIndex();
    if(FAILED(swapchain->GetDevice(IID_PPV_ARGS(&device))) || GenshinPresentNr::Identity(device.Get())!=source.device)
        return reject("swapchain-device");
    if(FAILED(swapchain->GetBuffer(index,IID_PPV_ARGS(&picture))))return reject("current-backbuffer");
    if(auto* r=GenshinPresentNrTrace::Current()){r->index=index;r->currentPicture=GenshinPresentNr::Identity(picture.Get());}
    picture->GetDesc(&desc);
    if(!desc.Width || !desc.Height || desc.ArraySize!=1 || desc.MipLevels!=1 ||
       desc.SampleDesc.Count!=1 || desc.SampleDesc.Quality)return reject("backbuffer-layout");
    // NR estimates picture-space flow after the game has scaled/composed SR.
    // Its exact final backbuffer extent need not equal the earlier SR output.
    // Same-device identity, lifetime, generation and GPU ordering remain required.
    GenshinPresentNr::Ticket ticket;ticket.source=source;ticket.backbuffer=picture;ticket.context=s.context;
    ticket.swapchain=GenshinPresentNr::Identity(swapchain);ticket.presentEpoch=presentEpoch;ticket.format=desc.Format;
    ticket.pictureWidth=desc.Width;ticket.pictureHeight=desc.Height;
    // One exact returned Scene -> current B, or Scene -> A -> ToPicture -> B,
    // revalidated against this swapchain's actual current buffer/subresource.
    // A fresh final picture may share the previous successful SR generation;
    // duplicate Present/menu-only draws cannot authorize NR a second time.
    ticket.pictureSerial=s.presentNrPictures.Take(source,picture.Get(),presentEpoch);
    if(!ticket.pictureSerial) {
        // No original draw has returned since acquisition. An additional native
        // Present is not a new NR/FG input. Match the actual current buffer and
        // the entire successful source against the prior acquisition; only the
        // real committed FG owner may classify it as an existing visible result.
        // Do not grant a ticket, depth lease, camera or new provider dispatch.
        if(acquiredProducer && s.fullDrawObservation && s.presentNrConsumedKey.Known() &&
           s.presentNrConsumedAtDraw==s.status.observedDraws &&
           presentEpoch>s.presentNrConsumedKey.present) {
            auto repeatedKey=GenshinBridgeFg::MakeKey(ticket);
            repeatedKey.picture=s.presentNrConsumedKey.picture;
            repeatedKey.present=s.presentNrConsumedKey.present;
            if(repeatedKey==s.presentNrConsumedKey)
                if(auto* scope=GenshinBridgeFg::PresentScope::Current())
                    scope->ObserveRepeatedPicture(s.presentNrConsumedKey);
        }
        return reject("no-fresh-native-picture");
    }
    // Reuse only this successful canonical history, never another history
    // with a coincident generation or a still-in-flight newer invocation.
    if(successful->transport && source.historyEpoch && successful->epoch==source.historyEpoch &&
       successful->canonicalHistoryEpoch==source.historyEpoch) {
        // The direct native reader is useful to ordinary FinishedPicture NR
        // too when the optional mapped lease is unavailable. Keep the native
        // queue/fence/owner gates in PreparedDepth and the consumer; requesting
        // this receipt does not itself authorize depth or bypass mapping.
        const auto& config=*Config::Instance();
        const bool presentNrDepth=config.DlssNrEnabled.value_or_default() &&
            config.DlssNrFinishedPicture.value_or_default() &&
            !config.DlssNrRunBeforeSr.value_or_default() && !config.DlssNrDeferredDlss.value_or_default();
        const bool queuedNativeDepth=acquiredProducer!=nullptr ||
            GenshinFgConsumer::NewWorkRequested() || presentNrDepth;
        auto depth=successful->transport->PreparedDepth(ticket.pictureSerial,&s.depthMapping,s.context.Get(),picture.Get(),queuedNativeDepth);
        ticket.depth=std::move(depth);
        const bool depthIdentity=ticket.DepthIdentityMatches();
        // FG has its own fenced native-accumulate receipt. A missing optional
        // world-raster lease must not erase that receipt; FinishedPicture NR
        // still requires its original mapped depth lease downstream.
        const bool depthAccepted=depthIdentity && (ticket.depth.Known() || ticket.depth.NativeIdentityKnown());
        static unsigned depthReports=0;
        if(depthReports<16){++depthReports;const auto& d=ticket.depth;
            const auto* m=d.mappingOwner.get();
            const uint32_t mismatch=(d.device!=source.device?1u:0u)|(d.context!=source.context?2u:0u)|
                (d.deviceEpoch!=source.deviceEpoch?4u:0u)|(d.producerEpoch!=source.ownerEpoch?8u:0u)|
                (d.configEpoch!=source.configEpoch?16u:0u)|(d.instance!=source.instance?32u:0u)|
                (d.generation!=source.generation?64u:0u)|(d.sourceFrame!=source.sourceFrame?128u:0u)|
                (d.pictureSerial!=ticket.pictureSerial?256u:0u)|(d.epoch!=source.historyEpoch?512u:0u)|
                (!m||m->selected!=source.selectedOutput?1024u:0u)|(!m||m->target!=GenshinPresentNr::Identity(picture.Get())?2048u:0u)|
                (d.width!=source.width||d.height!=source.height?4096u:0u)|
                (d.width!=ticket.pictureWidth||d.height!=ticket.pictureHeight?8192u:0u);
            LOG_INFO("R823 depth ticket: pid={} generation={} frame={} picture={} accepted={} ticket_known={} identity_known={} ticket_depth_identity={} lease={} actual_ready_result={} mismatch={} depth_owner={},{},{},{},{} source_owner={},{},{},{},{} depth_size={}x{} sr_size={}x{} picture_size={}x{} transform={} coverage={} producer_ready={} convention={} mapping_serial={} no_authority=true",
                GetCurrentProcessId(),source.generation,source.sourceFrame,ticket.pictureSerial,depthAccepted,ticket.Known(),
                d.IdentityKnown(),depthIdentity,static_cast<bool>(d.lease),depthIdentity?int(depthAccepted):-1,mismatch,
                d.device,d.context,d.instance,d.epoch,d.sourceFrame,source.device,source.context,source.instance,source.historyEpoch,source.sourceFrame,
                d.width,d.height,source.width,source.height,ticket.pictureWidth,ticket.pictureHeight,d.transformKnown,d.coverage,d.producerReady,
                static_cast<unsigned>(d.convention),m?m->serial:0);
        }
        if(!depthAccepted)ticket.depth={};
        else {
            const GenshinGuides::Owner owner{source.device,source.context,source.deviceEpoch,source.ownerEpoch,source.configEpoch,
                source.instance,source.generation,source.historyEpoch,static_cast<uint32_t>(source.sourceFrame),source.thread};
            GenshinGuides::Owner interfaces{};
            if(s.cameraDemand && GenshinGuides::OwnerIdentity::Context(s.context.Get()).ToInterfaces(owner,interfaces))
                successful->cameraHistory.Certify(interfaces);
        }
    }
    if(!ticket.Known())return reject("ticket-incomplete");
    // Retain the exact History already authenticated above, before leaving its
    // owning nativeMutex/thread. An observation/newest lookup is never a sender.
    if(acquiredProducer && ticket.DepthMatches() && successful->transport &&
       successful->epoch==source.historyEpoch && successful->canonicalHistoryEpoch==source.historyEpoch)
        *acquiredProducer=successful->transport;
    s.presentNrConsumedPicture=ticket.pictureSerial;
    s.presentNrConsumedKey=GenshinBridgeFg::MakeKey(ticket);
    s.presentNrConsumedAtDraw=s.status.observedDraws;
    if(auto* r=GenshinPresentNrTrace::Current()) {
        r->ticketSeq=GenshinPresentNrTrace::Sequence();r->ticket=true;r->reject="none";r->pictureSerial=ticket.pictureSerial;
    }
    return ticket;
}
Status CapturePipeline::GetStatus()const noexcept{return storage_->status;}
bool CapturePipeline::CameraForPresent(const GenshinPresentNr::Ticket& ticket,GenshinBridgeFg::CameraObservation& out)const noexcept
{
    out={};const auto& s=*storage_;const auto& source=ticket.source;
    if(s.invalidated || !s.cameraDemand || GetCurrentThreadId()!=s.thread || !ticket.Known() || !ticket.DepthMatches() || source.thread!=s.thread ||
       source.device!=GenshinPresentNr::Identity(s.device.Get()) || source.context!=GenshinPresentNr::Identity(s.context.Get()) ||
       source.deviceEpoch!=s.deviceEpoch || source.ownerEpoch!=s.ownerEpoch || source.configEpoch!=s.configEpoch ||
       !Config::Instance()->GenshinControlsCurrent(s.configEpoch))return false;
    const GenshinGuides::Owner owner{source.device,source.context,source.deviceEpoch,source.ownerEpoch,source.configEpoch,
        source.instance,source.generation,source.historyEpoch,static_cast<uint32_t>(source.sourceFrame),source.thread};
    const GenshinGuides::OwnerIdentity::Context identities(s.context.Get());GenshinGuides::Owner interfaces{};
    if(!identities.ToInterfaces(owner,interfaces))return false;
    for(const auto& h:s.histories)if(h && !h->failed && h->instance==source.instance && h->epoch==source.historyEpoch &&
       h->canonicalHistoryEpoch==source.historyEpoch && h->canonicalGeneration==source.generation &&
       h->generation==source.generation && GenshinPresentNr::Identity(h->output.Get())==source.privateOutput) {
        const auto* constants=h->cameraHistory.For(interfaces);
        if(!constants){
            static unsigned cameraHistoryReports=0;if(cameraHistoryReports<16){++cameraHistoryReports;
                const auto* current=h->cameraHistory.Current();
                LOG_INFO("R827 FG camera history: pid={} generation={} frame={} current={} current_valid={} current_owner_matches={} no_authority=true",
                    GetCurrentProcessId(),source.generation,source.sourceFrame,current!=nullptr,current && current->valid,
                    current && GenshinGuides::SameOwner(current->owner,interfaces));
            }
            return false;
        }
        const auto* predecessor=h->cameraHistory.PredecessorFor(interfaces);if(!predecessor)return false;
        GenshinBridgeFg::CameraObservation camera;camera.owner=owner;
        if(!identities.ToCanonical(*predecessor,camera.predecessor))return false;
        camera.source=GenshinGuides::Source::ActualRenderToken;camera.constants=*constants;
        if(!camera.Matches(ticket))return false;out=camera;return true;
    }
    return false;
}
GenshinTransport::Observation CapturePipeline::GetTransportObservation()const noexcept
{
    GenshinTransport::Observation latest;
    for(const auto& h:storage_->histories)if(h && h->transport){const auto observed=h->transport->Snapshot();
        if(observed.owner.generation>=latest.owner.generation)latest=observed;}
    return latest;
}
void CapturePipeline::CollectRetired()
{
    std::lock_guard lock(Storage::RetiredMutex());auto& retired=Storage::Retired();
    for(auto it=retired.begin();it!=retired.end();)
    {
        auto* value=*it;
        if(GetCurrentThreadId()==value->thread && value->Poll()){delete value;it=retired.erase(it);Storage::RetiredCount().fetch_sub(1,std::memory_order_acq_rel);}
        else ++it;
    }
}
const char* ReasonText(CaptureReason r)noexcept
{
    constexpr const char* text[]{"uninitialized","wrong game","legacy unique method table not matched","legacy Render installation failed",
        "no matching unconsumed Render token","ambiguous Render instance","unsupported full-view bindings",
        "invalid temporal input","GPU lease pending","wrong owning thread","input preparation failed","Opti consumer refused",
        "SR output copy queued","capture owner draining","GPU completion uncertain","eight Render histories occupied","canonical output repair queued","Opti FSR2 inputs disabled",
        "capture copy queued","capture copy completed/admitted","capture signals rejected","capture retired",
        "pixel shader creation record unavailable","pixel shader is not the pinned FSR accumulate"};
    return text[static_cast<unsigned>(r)];
}

static std::mutex nativeMutex;
static std::unique_ptr<CapturePipeline> nativePipeline;
static ComPtr<ID3D11Device> nativeDevice;
static Family::CreationOwner shaderCreationOwner;
static ComPtr<ID3D11DeviceContext> nativeContext;
static uint64_t nativeDeviceEpoch=0;
static bool shaderHookInstalled=false;
// Serialized by nativeMutex and scoped to the native device epoch.
struct ShaderCreations {
    uint64_t callbacks=0,wrongDevice=0,recorded=0,failed=0,beforePublication=0,accumulate=0;
    uint64_t firstTick=0,lastTick=0,lastHash=0,lastBytes=0;
};
static ShaderCreations shaderCreations;
static DWORD nativeThread=0;
static ControlsSnapshot nativeControls{};
// Config notifications are serialized by nativeMutex, including stale callers.
static uint64_t captureNotifiedEpoch=0;
static bool captureNotifiedActive=false;
static Status nativeStatus;
static std::atomic<bool> game{false};
// Published by the draw hook after acquiring the real controls. This gates
// optional observation only; nativeWorkOwned keeps transition drain mandatory.
static std::atomic<bool> nativeObservationActive{true},nativeWorkOwned{false};
// UI-only lifecycle publication. Initialized before the expensive detector, not
// reconstructed afterwards from log text. Writers retain the native owner lock.
static std::atomic<Ui::HookPhase> uiHookPhase { Ui::HookPhase::Unknown };
static std::atomic<uint64_t> uiHookAttempt { 0 }, uiDeviceEpoch { 0 }, uiControlsEpoch { 0 };
static std::atomic<uint64_t> uiHookStartedTick { 0 }, uiHookFinishedTick { 0 }, uiRevision { 0 };
static unsigned attempts=0;
static std::chrono::steady_clock::time_point nextAttempt{};
// Independent optional producer: a successful Render hook must not suppress it.
// nativeMutex serializes this state; config changes/device drain do not reset the budget.
static il2cpp_callsite::Config cameraInstallConfig{};
static uint64_t cameraInstallBase=0;
static unsigned cameraInstallAttempts=0;
static bool cameraInstalled=false, cameraInstallTerminal=false;
static std::chrono::steady_clock::time_point nextCameraAttempt{};
static thread_local bool internal=false;
// Menu requests are CPU-only. GPU sampling is owned by a real native immediate
// context/thread and is independent of feature enablement and guide admission.
static std::mutex manualMutex;
static ManualCapture::Snapshot manualSnapshot;
static std::atomic_bool manualArm{false},manualStop{false};
static std::atomic_uint64_t manualRequestTick{0},manualSessions{0};
static uint64_t manualOwnerEpoch=0;
static std::unique_ptr<RuntimeCapture::InputObserver> manualInputs;
static ComPtr<ID3D11DeviceContext> manualContext;
static DWORD manualThread=0;
static bool manualTerminalPublished=false;

static void PublishManualState()noexcept
{
    try{std::lock_guard lock(manualMutex);auto& out=manualSnapshot;
        out.supported=game.load() && nativeDevice.Get()!=nullptr;
        out.shaderHookInstalled=shaderHookInstalled;out.entryHookInstalled=nativeStatus.entryInstalled;
        const auto& state=RuntimeCapture::Get();
        out.knownShadersCached=RuntimeCapture::CachedShaderCount();
        if(!state.manual)return;
        out.processId=state.pid;out.processCreationFileTime=(uint64_t(state.processCreation.dwHighDateTime)<<32)|state.processCreation.dwLowDateTime;
        out.sessionNumber=manualSessions;out.elapsedMilliseconds=state.startTick?GetTickCount64()-state.startTick:0;
        out.shaderArtifactsWritten=state.shaderArtifactsWritten.load();out.constantArtifactsWritten=state.constantArtifactsWritten.load();
        out.bindingRecordsWritten=state.bindingRecordsWritten.load();out.recordsWritten=state.recordsWritten.load();
        out.diskBytes=state.diskBytes.load();out.queueDrops=state.queueDrops.load()+state.inputDrops.load();
        out.writeFailures=state.writeFailures.load();out.workerFinished=state.workerFinished.load(std::memory_order_acquire);
        wcsncpy_s(out.outputDirectory,state.directory.c_str(),_TRUNCATE);
        if(out.phase==ManualCapture::Phase::Requested)return;
        if(state.active.load(std::memory_order_acquire)){
            out.phase=!state.workerReady.load()?ManualCapture::Phase::Starting:
                state.firstFamilyDrawTick.load()?ManualCapture::Phase::Capturing:ManualCapture::Phase::Armed;
            out.reason=ManualCapture::Reason::None;
        }else{
            out.reason=state.finishReason.load();
            if(state.writeFailures.load())out.reason=ManualCapture::Reason::OutputFailure;
            if(!out.workerFinished || state.gpuPending.load())out.phase=ManualCapture::Phase::Draining;
            else {out.phase=(out.reason==ManualCapture::Reason::OutputFailure ||
                out.reason==ManualCapture::Reason::InternalFailure)?ManualCapture::Phase::Failed:ManualCapture::Phase::Complete;
                manualTerminalPublished=true;}
        }
    }catch(...){}
}
static void StopManualCapture(ManualCapture::Reason reason)noexcept
{
    manualArm=false;
    if(manualInputs)manualInputs->Revoke();
    RuntimeCapture::StopManual(reason);
    PublishManualState();
}
static void PollManualCapture(ID3D11Device* device,ID3D11DeviceContext* context=nullptr)noexcept
{
    if(!manualArm.load() && !manualStop.load() && (!RuntimeCapture::Get().manual || manualTerminalPublished))return;
    if(manualInputs && manualContext && device==nativeDevice.Get() && GetCurrentThreadId()==manualThread &&
        (!context || context==manualContext.Get()))manualInputs->Poll(device,manualContext.Get());
    if(manualStop.exchange(false))StopManualCapture(ManualCapture::Reason::UserStop);
    if(RuntimeCapture::Get().manual)RuntimeCapture::CloseExpiredWindow(RuntimeCapture::Get());
    PublishManualState();
}
static void ObserveManualDraw(ID3D11Device* device,ID3D11DeviceContext* context,UINT count,bool countKnown,uint64_t configEpoch)
{
    if(!manualArm.load() && !manualStop.load() && (!RuntimeCapture::Get().manual || manualTerminalPublished))return;
    PollManualCapture(device,context);
    if(manualArm.exchange(false)){
        ManualCapture::Reason rejected=ManualCapture::Reason::None;
        if(GetTickCount64()-manualRequestTick>10000)rejected=ManualCapture::Reason::RequestTimeout;
        else if(!shaderHookInstalled)rejected=ManualCapture::Reason::ShaderHookUnavailable;
        else if(manualSessions>=8)rejected=ManualCapture::Reason::SessionLimit;
        else if((manualInputs && !manualInputs->Idle()) || !RuntimeCapture::ResetForManual())rejected=ManualCapture::Reason::Busy;
        if(rejected!=ManualCapture::Reason::None){std::lock_guard lock(manualMutex);
            manualSnapshot.phase=ManualCapture::Phase::Failed;manualSnapshot.reason=rejected;return;}
        manualInputs=std::make_unique<RuntimeCapture::InputObserver>();manualContext=context;manualThread=GetCurrentThreadId();
        ++manualOwnerEpoch;++manualSessions;manualTerminalPublished=false;
        {std::lock_guard lock(manualMutex);manualSnapshot.phase=ManualCapture::Phase::Starting;manualSnapshot.reason=ManualCapture::Reason::None;}
        RuntimeCapture::Start(device,true,configEpoch,true);
        if(!RuntimeCapture::Get().active.load()){
            RuntimeCapture::Get().finishReason=ManualCapture::Reason::InternalFailure;PublishManualState();return;}
        RuntimeCapture::ExportCachedShaders(nativeDeviceEpoch);PublishManualState();
    }
    if(!RuntimeCapture::Get().manual || !RuntimeCapture::Enabled() || !manualInputs ||
        manualContext.Get()!=context || manualThread!=GetCurrentThreadId())return;
    if(RuntimeCapture::Get().configEpoch!=configEpoch){StopManualCapture(ManualCapture::Reason::ContextBoundary);return;}
    const auto shader=Family::Current(context,device,nativeDeviceEpoch);
    if(!shader.Known() || !RuntimeCapture::Expected(shader.hash))return;
    auto& state=RuntimeCapture::Get();if(state.passBudgetCount>=RuntimeCapture::MaxPassSamples){++state.passBudgetDrops;return;}
    ++state.passBudgetCount;++state.passOffers;RuntimeCapture::BeginFamilyWindow();
    RuntimeCapture::DrawFact draw;draw.device=reinterpret_cast<uintptr_t>(device);draw.context=reinterpret_cast<uintptr_t>(context);
    draw.deviceEpoch=nativeDeviceEpoch;draw.ownerEpoch=manualOwnerEpoch;draw.configEpoch=configEpoch;draw.thread=manualThread;
    draw.drawSerial=state.passBudgetCount;draw.shaderHash=shader.hash;draw.shaderIdentity=shader.identity;
    draw.observationTick=GetTickCount64();draw.drawCount=count;draw.drawCountKnown=countKnown;draw.familyPass=true;
    // Bindings are facts only. No guessed Render association, camera convention,
    // coordinate system or meaning is promoted into a producer receipt.
    std::array<ComPtr<ID3D11ShaderResourceView>,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> views;
    std::array<ID3D11ShaderResourceView*,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> raw{};
    context->PSGetShaderResources(0,static_cast<UINT>(raw.size()),raw.data());
    for(unsigned i=0;i<raw.size();++i){views[i].Attach(raw[i]);if(!views[i])continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};views[i]->GetDesc(&vd);ComPtr<ID3D11Resource> resource;views[i]->GetResource(&resource);
        draw.boundViews[i]=reinterpret_cast<uintptr_t>(views[i].Get());draw.boundResources[i]=reinterpret_cast<uintptr_t>(resource.Get());
        draw.boundFormats[i]=vd.Format;draw.boundViewDescriptions[i]=vd;draw.boundWrites[i]=ResourceProof::Identify(context,resource.Get(),views[i].Get());
        ComPtr<ID3D11Texture2D> texture;if(SUCCEEDED(resource.As(&texture))){texture->GetDesc(&draw.boundDescriptions[i]);
            draw.boundWidths[i]=draw.boundDescriptions[i].Width;draw.boundHeights[i]=draw.boundDescriptions[i].Height;}}
    std::array<ID3D11RenderTargetView*,D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> outputs{};
    context->OMGetRenderTargets(static_cast<UINT>(outputs.size()),outputs.data(),nullptr);
    for(unsigned i=0;i<outputs.size();++i){ComPtr<ID3D11RenderTargetView> view;view.Attach(outputs[i]);if(!view)continue;
        ComPtr<ID3D11Resource> resource;view->GetResource(&resource);view->GetDesc(&draw.outputViewDescriptions[i]);
        draw.outputViews[i]=reinterpret_cast<uintptr_t>(view.Get());draw.outputResources[i]=reinterpret_cast<uintptr_t>(resource.Get());
        draw.outputFormats[i]=draw.outputViewDescriptions[i].Format;draw.outputWrites[i]=ResourceProof::Identify(context,resource.Get(),view.Get());
        ComPtr<ID3D11Texture2D> texture;if(SUCCEEDED(resource.As(&texture))){texture->GetDesc(&draw.outputDescriptions[i]);
            draw.outputWidths[i]=draw.outputDescriptions[i].Width;draw.outputHeights[i]=draw.outputDescriptions[i].Height;}}
    UINT viewports=0;context->RSGetViewports(&viewports,nullptr);draw.viewportCount=viewports;
    if(viewports==1)context->RSGetViewports(&viewports,&draw.viewport);
    std::array<ID3D11ShaderResourceView*,3> inputs{};const unsigned slots[]{0,2,3};
    for(unsigned i=0;i<3;++i){const auto slot=slots[i];inputs[i]=views[slot].Get();draw.slots[i]=slot;
        draw.views[i]=draw.boundViews[slot];draw.resources[i]=draw.boundResources[slot];draw.formats[i]=draw.boundFormats[slot];
        draw.widths[i]=draw.boundWidths[slot];draw.heights[i]=draw.boundHeights[slot];draw.sourceWrites[i]=draw.boundWrites[slot];}
    RuntimeCapture::Job binding;binding.kind=RuntimeCapture::Job::Kind::Binding;binding.draw=draw;binding.eventTick=draw.observationTick;
    RuntimeCapture::Enqueue(std::move(binding));manualInputs->Queue(device,context,draw,inputs);PublishManualState();
}

bool ManualCapture::RequestArm()noexcept
{
    try{std::lock_guard lock(manualMutex);
        if(!manualSnapshot.supported)return false;
        if(manualSnapshot.phase==Phase::Requested || manualSnapshot.phase==Phase::Starting || manualSnapshot.phase==Phase::Armed ||
            manualSnapshot.phase==Phase::Capturing || manualSnapshot.phase==Phase::Draining){manualSnapshot.reason=Reason::Busy;return false;}
        if(manualSessions>=8){manualSnapshot.reason=Reason::SessionLimit;return false;}
        manualSnapshot.phase=Phase::Requested;manualSnapshot.reason=Reason::AwaitingNativeBoundary;
        manualRequestTick=GetTickCount64();manualStop=false;manualArm.store(true,std::memory_order_release);return true;
    }catch(...){return false;}
}
void ManualCapture::RequestStop()noexcept{manualStop.store(true,std::memory_order_release);}
ManualCapture::Snapshot ManualCapture::GetSnapshot()noexcept
{
    try{std::lock_guard lock(manualMutex);
        if(manualSnapshot.phase==Phase::Requested && GetTickCount64()-manualRequestTick>10000){
            manualArm=false;manualSnapshot.phase=Phase::Failed;manualSnapshot.reason=Reason::RequestTimeout;}
        return manualSnapshot;
    }catch(...){return {};}
}

bool IsInternalCallActive() noexcept { return internal || GenshinPresentNr::Scope::Current()!=nullptr; }
static bool CurrentGame()
{
    wchar_t path[32768]{};DWORD n=GetModuleFileNameW(nullptr,path,32768);
    return n && n<32768 && GenshinFsr::IsGenshinExecutable(std::wstring_view(path,n));
}
static bool RouteActive(const ControlsSnapshot& controls) noexcept
{
    return controls.Active() || (controls.settings.enabled && (controls.settings.captureOnlyObservation || captureOnlyMode==CaptureOnlyMode::ObservationOnly));
}
bool UpdateNativeObservationDemand(const ControlsSnapshot& controls) noexcept
{
    // Entry/lifecycle demand is deliberately broader than the optional draw
    // subscription selected later under nativeMutex. Bridge SR stays observed.
    const bool active=RouteActive(controls) || OptionalGuideDemand() ||
        Config::Instance()->HoYoShadeEnabled.value_or_default() || manualArm.load(std::memory_order_acquire) ||
        RuntimeCapture::Get().active.load(std::memory_order_acquire);
    const auto previous=nativeObservationActive.exchange(active,std::memory_order_acq_rel);
    if(previous!=active){static std::atomic<unsigned> reports{0};if(reports.fetch_add(1,std::memory_order_relaxed)<16)try{
        LOG_INFO("R856 native observation demand: pid={} active={} owned_work={} route={} optional_guides={} runtime_capture={} admission_unchanged=true",
            GetCurrentProcessId(),active,nativeWorkOwned.load(std::memory_order_acquire),RouteActive(controls),OptionalGuideDemand(),
            RuntimeCapture::Get().active.load(std::memory_order_acquire));
    }catch(...){}}
    return active;
}
bool NativeObservationActive() noexcept{return nativeObservationActive.load(std::memory_order_acquire);}
bool NativeObservationDrainNeeded() noexcept{return nativeWorkOwned.load(std::memory_order_acquire);}
// All helpers below run under the already-acquired nativeMutex. Full FG/NR
// exits before optional collector locks. HoYo idle is a boundary-issued token;
// its per-draw Current check never scans selection or acquires another lock.
static bool OptionalConsumersEffectivelyOff() noexcept
{
    // An enabled HoYo stage is idle only after an existing owner boundary
    // proves complete live zero selection and no retained GPU custody.
    return NonHoYoConsumersEffectivelyOff() && ownerObservation.hoYo.Current();
}
static bool NonHoYoConsumersEffectivelyOff() noexcept
{
    const auto& cfg=*Config::Instance();
    if(GenshinFgConsumer::FullObservationRequired() || cfg.DlssNrEnabled.value_or_default())return false;
    // FG demand distinguishes explicit NoFG from an unresolved selection.
    // Re-read FG/NR on every subscription check so a real consumer exits
    // immediately. Idle is only demand: existing publications,
    // pending/retirement and escaped-reader checks still establish idle.
    if(captureOnlyMode.load(std::memory_order_acquire)!=CaptureOnlyMode::Disabled ||
        manualArm.load(std::memory_order_acquire) || manualStop.load(std::memory_order_acquire))return false;
    auto& capture=RuntimeCapture::Get();
    if(capture.attempted || capture.manual || manualInputs ||
        capture.active.load(std::memory_order_acquire) || !capture.workerFinished.load(std::memory_order_acquire) ||
        capture.gpuPending.load(std::memory_order_acquire) || capture.gpuQuarantined.load(std::memory_order_acquire))return false;
    // Full consumers above never pay this query. It reads actual current UI
    // request + actual owner copy/read/lease publication, not UI initialization,
    // visibility, a cached config revision or the presence of cached surfaces.
    return GenshinDepthProvider::ReadObservationDemand().Idle();
}
static bool OptionalCollectorsIdle(const HoYoShade::ObservationDemand& hoYo) noexcept
{
    if(!hoYo.Current())return false;
    // Boundary-only. Pending GPU readbacks/escaped readers remain blockers;
    // inactive LOCATION and completed CPU sample metadata are not GPU owners.
    if(GenshinDepthMapping::Deferred::liveVersions.load(std::memory_order_acquire) ||
        GenshinDepthMapping::Deferred::expectedRaster.context)return false;
    auto& writer=GenshinCpuWriterProbe::state;
    if(writer.pendingRevoke.load(std::memory_order_acquire) || writer.busy.test_and_set(std::memory_order_acquire))return false;
    const bool writerIdle=(!writer.arm.context || (writer.arm.context.Get()==nativeContext.Get() &&
        writer.arm.thread==GetCurrentThreadId())) && !writer.pendingRevoke.load(std::memory_order_acquire);
    if(writerIdle)GenshinCpuWriterProbe::ClearWindow(); // CPU payloads/Map metadata only, no GPU query/lease
    writer.busy.clear(std::memory_order_release);
    if(!writerIdle)return false;
    {
        std::unique_lock samples(GenshinDepthEvidence::SamplesMutex(),std::try_to_lock);
        if(!samples.owns_lock())return false;
        for(const auto& sample:GenshinDepthEvidence::Samples())
            if(sample.pending)return false; // includes quarantined/failed queries
        for(auto& sample:GenshinDepthEvidence::Samples()){
            if(sample.thread && sample.thread!=GetCurrentThreadId())return false;
            for(auto& e:sample.exports)if(e.rawMatched && !e.claimLogged){
                e.claimLogged=true;GenshinDepthEvidence::waitingClaimExports.fetch_sub(1,std::memory_order_relaxed);}
            // Leave the sample/plan and reported flag intact. A metadata-only
            // sample may still make its original readback attempt after wakeup.
        }
    }
    if(GenshinDepthEvidence::waitingClaimExports.load(std::memory_order_acquire))return false;
    auto& expected=GenshinDepthEvidence::Expected();
    if(expected.context && (expected.context.Get()!=nativeContext.Get() || expected.thread!=GetCurrentThreadId()))return false;
    expected={};GenshinDepthEvidence::PendingRasterExport()=0;
    auto& raster=GenshinReturnedRaster::detail::Store();
    std::unique_lock lock(raster.mutex,std::try_to_lock);
    if(!lock.owns_lock() || raster.pendingSlot>=0 || raster.pending.source || raster.clear.resource)return false;
    if(!hoYo.Current())return false;
    // The boundary checked every real producer/lease, mapping EVENT and global
    // export/retirement owner. An immutable CPU Evidence still cached by a
    // completed SR receipt is not itself a GPU reader. Keep its roots untouched;
    // clearing the tracks makes Evidence::Current false before observation stops.
    // Discard them before silence, so enable cannot reuse unobserved contents.
    // No owner, query, fence or lease is polled/released by this transition.
    for(auto& track:raster.tracks)track={};
    return true;
}
static bool OwnerObservationCurrent(ID3D11DeviceContext* context,uint64_t configEpoch) noexcept
{
    // Known Full consumers leave before any new owner/collector query. All
    // remaining operations are scalar/TLS/atomic reads, not locks or scans.
    return OptionalConsumersEffectivelyOff() && ownerObservation.srOnly && nativePipeline &&
        ownerObservation.owner==nativePipeline.get() && ownerObservation.context==context && context==nativeContext.Get() &&
        ownerObservation.thread==GetCurrentThreadId() && GetCurrentThreadId()==nativeThread &&
        ownerObservation.ownerEpoch==nativeStatus.ownerEpoch && ownerObservation.deviceEpoch==nativeDeviceEpoch &&
        ownerObservation.configEpoch==configEpoch && !nativePipeline->NeedsDrain() &&
        Config::Instance()->GenshinControlsCurrent(configEpoch) && ownerObservation.hoYo.Current() &&
        CapturePipeline::ObservationRetirementsEmpty() &&
        GenshinTransport::ProducerFrame::ObservationRetirementsEmpty() && GenshinTransport::ObservationRetirementsEmpty();
}
static bool CaptureDiagnosticsIdle(const ControlsSnapshot& controls) noexcept
{
    const auto& capture=RuntimeCapture::Get();
    return !controls.settings.captureOnlyObservation && captureOnlyMode.load(std::memory_order_acquire)==CaptureOnlyMode::Disabled &&
        !manualArm.load(std::memory_order_acquire) && !manualStop.load(std::memory_order_acquire) &&
        (!capture.manual || manualTerminalPublished) && !capture.active.load(std::memory_order_acquire) &&
        capture.workerFinished.load(std::memory_order_acquire) && !capture.gpuPending.load(std::memory_order_acquire) &&
        !capture.gpuQuarantined.load(std::memory_order_acquire);
}
static bool SameNativeOwner(ID3D11DeviceContext* context,const ControlsSnapshot& controls) noexcept
{
    return NativeObservationActive() && nativePipeline && nativeDevice && nativeDeviceEpoch &&
        context==nativeContext.Get() && GetCurrentThreadId()==nativeThread && !nativePipeline->NeedsDrain() &&
        RouteActive(controls) && shaderHookInstalled && nativeStatus.entryInstalled &&
        nativeControls.epoch==controls.epoch && nativeControls.settings==controls.settings &&
        Config::Instance()->GenshinControlsCurrent(controls.epoch);
}
static bool CaptureObservationCurrent(const ControlsSnapshot& controls) noexcept
{
    // Caller holds nativeMutex and has checked SameNativeOwner for this callback.
    // Only match the boundary publication and live capture demand here.
    return ownerObservation.captureOff && controls.Active() && ownerObservation.owner==nativePipeline.get() &&
        ownerObservation.context==nativeContext.Get() && ownerObservation.thread==nativeThread &&
        ownerObservation.ownerEpoch==nativeStatus.ownerEpoch && ownerObservation.deviceEpoch==nativeDeviceEpoch &&
        ownerObservation.configEpoch==controls.epoch && CaptureDiagnosticsIdle(controls);
}
static void RefreshOwnerObservation(ID3D11DeviceContext* context,const ControlsSnapshot& controls) noexcept
{
    ownerObservation.srOnly=false;ownerObservation.captureOff=false;
    // Independent of FG/NR/HoYo demand. Publish only at the existing Poll/claim
    // boundary; completed capture caches do not become permanent consumers.
    if(context && nativePipeline && nativeDevice && nativeDeviceEpoch && controls.Active() &&
       controls.version==1 && controls.bytes==sizeof(ControlsSnapshot) && ValidSettings(controls.settings) &&
       context==nativeContext.Get() && GetCurrentThreadId()==nativeThread && !nativePipeline->NeedsDrain() &&
       shaderHookInstalled && nativeStatus.entryInstalled && nativeControls.epoch==controls.epoch &&
       nativeControls.settings==controls.settings && Config::Instance()->GenshinControlsCurrent(controls.epoch) &&
       !ResourceProof::Active() && CaptureDiagnosticsIdle(controls) && (!manualInputs || manualInputs->Idle()))
        ownerObservation={context,nativePipeline.get(),GetCurrentThreadId(),0,controls.epoch,nativeDeviceEpoch,nativeStatus.ownerEpoch,{},false,true};
    if(!NonHoYoConsumersEffectivelyOff() || !context || !controls.Active() || controls.settings.captureOnlyObservation ||
        controls.version!=1 || controls.bytes!=sizeof(ControlsSnapshot) || !ValidSettings(controls.settings) ||
        !nativeDevice || !nativeDeviceEpoch || !shaderHookInstalled || !nativeStatus.entryInstalled ||
        nativeControls.epoch!=controls.epoch || nativeControls.settings!=controls.settings ||
        !Config::Instance()->GenshinControlsCurrent(controls.epoch) ||
        (nativePipeline && (context!=nativeContext.Get() || GetCurrentThreadId()!=nativeThread ||
            nativePipeline->NeedsDrain())) || !nativePipeline)return;
    const auto hoYo=HoYoShade::ReadObservationDemand();
    if(!hoYo.Current() || !nativePipeline->OptionalObservationIdle())return;
    if(!OptionalCollectorsIdle(hoYo) || !hoYo.Current() || !NonHoYoConsumersEffectivelyOff() ||
        !Config::Instance()->GenshinControlsCurrent(controls.epoch))return;
    ownerObservation={context,nativePipeline.get(),GetCurrentThreadId(),0,controls.epoch,nativeDeviceEpoch,nativeStatus.ownerEpoch,hoYo,true,true};
}
static bool SelectSrOnlyObservation(ID3D11DeviceContext* context,const ControlsSnapshot& controls) noexcept
{
    drawObservation.srOnly=false;
    if(!OwnerObservationCurrent(context,controls.epoch))return false;
    const auto& call=GenshinDepthMapping::CurrentDraw();
    if(call.context!=context || call.thread!=GetCurrentThreadId() || !call.observationCall ||
        call.kind<1 || call.kind>4 || call.indirectArgumentsUnknown)return false;
    drawObservation=ownerObservation;drawObservation.call=call.observationCall;
    return true;
}
bool TrySkipNonTargetDrawObservation(ID3D11DeviceContext* context,UINT count,bool countKnown,unsigned drawKind) noexcept
{
    // TLS is only an early negative hint. The actual owner stays locked.
    // Known Full-route consumers leave before the try-lock.
    // Direct counts are per-instance counts here, not SR/replay admission.
    // DrawAuto/indirect have no CPU count; only their actual known non-FSR PS
    // can reject them. Never decode/read back their GPU arguments to do so.
    const bool direct=drawKind>=1 && drawKind<=4;
    if(drawKind<1 || drawKind>7 || (direct && (!countKnown || !count)) || !game || internal ||
        !ownerObservation.srOnly || ownerObservation.context!=context ||
        ownerObservation.thread!=GetCurrentThreadId() || GenshinPresentNr::Scope::Current())return false;
    const auto& cfg=*Config::Instance();
    if(GenshinFgConsumer::FullObservationRequired() || cfg.DlssNrEnabled.value_or_default() ||
        !ownerObservation.hoYo.Current())return false;
    try {
        GenshinHookCpuTiming::Scope r448OwnerTry(GenshinHookCpuTiming::Stage::OwnerTryLock);
        std::unique_lock lock(nativeMutex,std::try_to_lock);r448OwnerTry.Returned(lock.owns_lock()?S_OK:S_FALSE,GenshinHookCpuTiming::ResultKind::Hresult);r448OwnerTry.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked,lock.owns_lock());
        // Enable/Use mutations now invalidate the controls epoch checked by
        // OwnerObservationCurrent; do not read their mutable optionals here.
        if(!lock.owns_lock() || !NativeObservationActive() || !nativePipeline || !nativeDevice || !nativeDeviceEpoch ||
            !shaderHookInstalled || !nativeStatus.entryInstalled || !nativeControls.Active() ||
            PsState::optionalInstall.Phase()==PsState::OptionalInstallPhase::PendingPublication ||
            !OwnerObservationCurrent(context,nativeControls.epoch) || nativePipeline->DrawPairPending())return false;
        if(!direct || count==3){
            // Fullscreen and unknown-count draws need a negative shader proof.
            // Unknown/missing records and every FSR-family member stay on the
            // original path, including its original count/replay restrictions.
            ComPtr<ID3D11PixelShader> pixel;context->PSGetShader(&pixel,nullptr,nullptr);
            const auto shader=Family::Read(nativeDevice.Get(),pixel.Get(),nativeDeviceEpoch);
            if(!shader.Known())return false;
            const auto hash=shader.hash;
            if(hash==fsr2_family_takeover::k_accumulate_hash ||
                hash==fsr2_family_takeover::k_pre_hash_1 || hash==fsr2_family_takeover::k_pre_hash_2 ||
                hash==fsr2_family_takeover::k_pre_hash_3 || hash==fsr2_family_takeover::k_pre_hash_4)return false;
        }
        // GPU flights already have their own Poll/Present retirement. They do
        // not subscribe unrelated native draws. Only an unfinished Before/After
        // pair needs the next callback's cleanup; its decision is paired locally.
        nativeStatus.srAccepted=false;nativeStatus.effectiveFamilySkip=false;
        return true;
    }catch(...){return false;}
}
static bool ObservationOnlyCurrent(ID3D11DeviceContext* context,uint64_t configEpoch) noexcept
{
    const auto& call=GenshinDepthMapping::CurrentDraw();
    if(!drawObservation.srOnly || drawObservation.context!=context || drawObservation.thread!=GetCurrentThreadId() ||
        drawObservation.call!=call.observationCall || call.context!=context || call.thread!=GetCurrentThreadId() ||
        drawObservation.configEpoch!=configEpoch || drawObservation.deviceEpoch!=nativeDeviceEpoch ||
        drawObservation.ownerEpoch!=ownerObservation.ownerEpoch ||
        drawObservation.owner!=ownerObservation.owner)return false;
    return OwnerObservationCurrent(context,configEpoch);
}
static bool RefreshClaimObservation(ID3D11DeviceContext* context,uint64_t configEpoch) noexcept
{
    if(ObservationOnlyCurrent(context,configEpoch))return true;
    if(!nativePipeline || configEpoch!=nativeControls.epoch)return false;
    // Original BeforeDraw Poll has already run; no per-draw retry/qualification.
    RefreshOwnerObservation(context,nativeControls);
    return SelectSrOnlyObservation(context,nativeControls);
}
static void RetirePendingRenderTokens()
{
    il2cpp_callsite::retire_render_tokens();
}
// Call under nativeMutex before any GPU drain or runtime artifact offer.
// Stop is terminal for this one-attempt session; it does not cancel GPU ownership drain.
static void ObserveCaptureSessionControls(const ControlsSnapshot& controls) noexcept
{
    // Start/ResetForManual and this observer are serialized by nativeMutex.
    // An inactive session's Stop only locks its mutex and returns: avoid that
    // repeated no-op on the draw path. Active sessions still use locked Stop.
    // Use active, not Enabled(): an expired active session still needs Stop.
    auto& capture=RuntimeCapture::Get();
    if(capture.active.load(std::memory_order_acquire) &&
       (!controls.settings.enabled || !controls.settings.captureOnlyObservation ||
        controls.epoch!=capture.configEpoch))RuntimeCapture::Stop();
}
static void ObserveCaptureControls(const ControlsSnapshot& controls,bool captureOff=false) noexcept
{
    const bool active=controls.Active();
    // A current capture-off owner was notified at its boundary. Retain the last
    // actual notification so an intervening stale caller cannot hide a change.
    if(!captureOff || captureNotifiedEpoch!=controls.epoch || captureNotifiedActive!=active){
        PsState::ObserveConfig(controls.epoch,active);
        GenshinCpuWriterProbe::ObserveConfig(controls.epoch);
        captureNotifiedEpoch=controls.epoch;captureNotifiedActive=active;
    }
    if(!captureOff || RuntimeCapture::Get().active.load(std::memory_order_acquire))ObserveCaptureSessionControls(controls);
}
// Snapshot before nativeMutex to preserve the existing Config-owner -> native order.
// Default-off paths do not acquire the Config owner. An absent snapshot is not
// evidence of current settings and cannot open or refresh a collection session.
static std::optional<ControlsSnapshot> CaptureControlsAtBoundary()
{
    if(!RuntimeCapture::Get().active.load(std::memory_order_acquire))return std::nullopt;
    return Config::Instance()->AcquireGenshinControls();
}
static void UpdateControlsStatus(const ControlsSnapshot& controls)
{
    nativeStatus.controlsEpoch=controls.epoch;nativeStatus.controlsVersion=controls.version;
    nativeStatus.requestedEnabled=controls.settings.enabled;
    nativeStatus.effectiveEnabled=controls.Active() && nativeStatus.entryInstalled && nativeControls.epoch==controls.epoch &&
        nativeStatus.reason!=CaptureReason::Draining && nativeStatus.reason!=CaptureReason::ThreadMismatch;
    nativeStatus.contextIdentity=reinterpret_cast<uint64_t>(nativeContext.Get());nativeStatus.owningThread=nativeThread;
    nativeStatus.requestedFamilySkip=controls.settings.familySkip;nativeStatus.shaderHookInstalled=shaderHookInstalled;
    nativeStatus.deviceEpoch=nativeDeviceEpoch;
    nativeStatus.effectiveFamilySkip=nativeStatus.effectiveFamilySkip && nativeStatus.effectiveEnabled && shaderHookInstalled;
}
static void NoteControls(const ControlsSnapshot& controls)
{
    ObserveCaptureControls(controls);
    UpdateControlsStatus(controls);
}
static bool DrainNativePipeline()
{
    PsState::RetireOwner(&shaderCreationOwner);
    GenshinDepthProvider::RevokePreview();
    GenshinReturnedRaster::Invalidate(nullptr,nullptr,GenshinReturnedRaster::Boundary::Owner);
    if(nativePipeline && !nativePipeline->Invalidate())
    {nativeStatus=nativePipeline->GetStatus();nativeStatus.srAccepted=false;nativeStatus.reason=CaptureReason::Draining;return false;}
    nativePipeline.reset();nativeContext.Reset();nativeThread=0;RetirePendingRenderTokens();
    nativeWorkOwned.store(false,std::memory_order_release);return true;
}
static void TryInstallCamera()
{
    // Called only on the already authenticated Genshin active route under nativeMutex.
    // A pending/failed optional camera never revokes successful SR/ordinary Present NR.
    if(!nativeStatus.entryInstalled || !cameraInstallBase || !cameraInstallConfig.capture_camera ||
       cameraInstalled || cameraInstallTerminal || cameraInstallAttempts>=3 ||
       std::chrono::steady_clock::now()<nextCameraAttempt)return;
    ++cameraInstallAttempts;
    nextCameraAttempt=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    il2cpp_callsite::CameraInstallResult result{};
    cameraInstalled=il2cpp_callsite::install_camera(cameraInstallBase,cameraInstallConfig,&result);
    cameraInstallTerminal=cameraInstalled || !result.retryable || cameraInstallAttempts>=3;
    LOG_INFO("Genshin camera producer install: pid={} device_epoch={} attempt={} camera_rva={:X} requested={} installed={} phase={} domain={} status={:08X} abort_status={:08X} failed_rva={:X} thread={} retry_pending={} semantic=UNPROVED",
        GetCurrentProcessId(),nativeDeviceEpoch,cameraInstallAttempts,cameraInstallConfig.camera_rva,
        cameraInstallConfig.capture_camera,cameraInstalled,result.phase,result.domain,result.status,
        result.abortStatus,result.rva,result.threadId,!cameraInstallTerminal);
}
static void TryInstallEntry()
{
    if(nativeStatus.entryInstalled){TryInstallCamera();return;}
    if(attempts>=3 || std::chrono::steady_clock::now()<nextAttempt)return;
    ++attempts;nextAttempt=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    uiRevision.fetch_add(1, std::memory_order_acq_rel);
    uiHookStartedTick.store(GetTickCount64(), std::memory_order_relaxed);
    uiHookFinishedTick.store(0, std::memory_order_relaxed);
    uiDeviceEpoch.store(nativeDeviceEpoch, std::memory_order_relaxed);
    uiControlsEpoch.store(nativeControls.epoch, std::memory_order_relaxed);
    uiHookAttempt.fetch_add(1, std::memory_order_relaxed);
    uiHookPhase.store(Ui::HookPhase::Initializing, std::memory_order_release);
    uiRevision.fetch_add(1, std::memory_order_release);
    struct UiAttempt
    {
        bool installed = false;
        ~UiAttempt()
        {
            uiRevision.fetch_add(1, std::memory_order_acq_rel);
            uiHookFinishedTick.store(GetTickCount64(), std::memory_order_relaxed);
            uiHookPhase.store(installed ? Ui::HookPhase::Installed : Ui::HookPhase::Failed,
                              std::memory_order_release);
            uiRevision.fetch_add(1, std::memory_order_release);
        }
    } uiAttempt;
    uint32_t render=0,ucb=0,camera=0;uint64_t base=reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    if(!il2cpp_callsite::detect_ffx12_method_rvas(base,render,ucb,camera))
    {nativeStatus.reason=CaptureReason::EntryNotMatched;
        LOG_WARN("Genshin internal FSR entry: pid={} device_epoch={} attempt={} method profile not matched",
            GetCurrentProcessId(),nativeDeviceEpoch,attempts);return;}
    il2cpp_callsite::Config config;config.render_rva=render;config.update_cmd_buffer_rva=ucb;
    // The detector already returns the current build's real
    // ConfigureJitteredProjectionMatrix RVA. Keep this as a read-only,
    // fail-closed observation hook; it does not mint CameraObservation or
    // alter the FSR/FG admission contract until the captured layout is proven.
    config.camera_rva=camera;
    config.skip_render=false;config.capture_camera=camera!=0;config.capture_projection=false;
    const char* reason=nullptr;
    nativeStatus.entryInstalled=il2cpp_callsite::install(base,config,&reason);
    uiAttempt.installed=nativeStatus.entryInstalled;
    LOG_INFO("Genshin internal FSR entry: pid={} device_epoch={} attempt={} render_rva={:X} ucb_rva={:X} installed={} reason={}",
        GetCurrentProcessId(),nativeDeviceEpoch,attempts,render,ucb,nativeStatus.entryInstalled,reason?reason:"none");
    if(nativeStatus.entryInstalled) {
        cameraInstallBase=base;cameraInstallConfig=config;
        TryInstallCamera();
    }
    nativeStatus.reason=nativeStatus.entryInstalled?CaptureReason::NoRenderToken:CaptureReason::EntryInstallFailed;
}

// The camera hook is observation-only. Log a bounded sample when the game
// actually calls the detected writer so the layout and frame relationship can
// be reviewed before any CameraObservation is constructed.
static void ObserveCameraSample() noexcept
{
    static std::uint64_t lastGeneration=0;
    static unsigned logs=0;
    try
    {
        if(!il2cpp_callsite::camera_ready())return;
        const auto generation=il2cpp_callsite::camera_generation();
        if(!generation || generation==lastGeneration)return;
        float values[16]{};
        const auto count=il2cpp_callsite::camera_floats(values,16);
        lastGeneration=generation;
        if(logs>=16)return;
        ++logs;
        LOG_INFO("Genshin camera sample: pid={} generation={} floats={} v0={} v1={} v2={} v3={} v4={} v5={} v6={} v7={} v8={} v9={} v10={} v11={} v12={} v13={} v14={} v15={} semantic=UNPROVED association=UNPROVED",
            GetCurrentProcessId(),generation,count,
            values[0],values[1],values[2],values[3],values[4],values[5],values[6],values[7],
            values[8],values[9],values[10],values[11],values[12],values[13],values[14],values[15]);
    }
    catch(...){ }
}
bool InitializeDevice(ID3D11Device* device)noexcept
{
    try
    {
        if(!CurrentGame()){nativeStatus.reason=CaptureReason::WrongGame;return false;}
        const auto controls=Config::Instance()->AcquireGenshinControls();
        GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);
        ObserveCaptureControls(controls);
        game=true;
        if(!device || (nativeDevice && nativeDevice.Get()!=device)){nativeStatus.reason=CaptureReason::UnsupportedBindings;return false;}
        if(!nativeDevice){static uint64_t epochs=0;nativeDeviceEpoch=++epochs;}
        if(!shaderCreationOwner.Bind(device,nativeDeviceEpoch))return false;
        nativeDevice=device;
        RuntimeCapture::Start(device,controls.settings.enabled && controls.settings.captureOnlyObservation,controls.epoch);
        ResourceProof::Configure(device,nativeDeviceEpoch,controls.settings.enabled && controls.settings.captureOnlyObservation);
        if(!nativePipeline)nativeControls=controls;NoteControls(controls);
        PublishManualState();
        if(RouteActive(controls))TryInstallEntry();else nativeStatus.reason=CaptureReason::Disabled;
        return true;
    }
    catch(...){return false;}
}
void SetShaderHookInstalled(bool installed)noexcept
{
    try{GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);shaderHookInstalled=installed;nativeStatus.shaderHookInstalled=installed;PublishManualState();}
    catch(...){}
}
void NotePixelShaderCreated(ID3D11Device* device,ID3D11PixelShader* shader,const void* bytes,size_t size)noexcept
{
    if(!game || internal)return;
    try{const auto captureControls=CaptureControlsAtBoundary();GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);
        if(captureControls)ObserveCaptureControls(*captureControls);
        ++shaderCreations.callbacks;
        if(device!=nativeDevice.Get()){++shaderCreations.wrongDevice;return;}
        const auto tick=GetTickCount64();if(!shaderCreations.firstTick)shaderCreations.firstTick=tick;shaderCreations.lastTick=tick;
        if(!shaderHookInstalled)++shaderCreations.beforePublication;
        // A creation detour may run between Commit and publication on another
        // thread. Retain its returned object/bytes now: PSGetShader cannot
        // recover lost bytecode later. Existing draw/family gates are unchanged.
        if(shaderCreationOwner.RecordReturned(device,shader,bytes,size)){
            ++shaderCreations.recorded;
            const auto fact=Family::Read(device,shader,nativeDeviceEpoch);
            DrawObservationDx11::RememberShader(shader,fact.hash,size);
            GenshinDepthEvidence::RetainVertex(shader,bytes,size,fact.hash);
            shaderCreations.lastHash=fact.hash;shaderCreations.lastBytes=fact.bytes;
            if(fact.Known() && fact.hash==fsr2_family_takeover::k_accumulate_hash)++shaderCreations.accumulate;
            RuntimeCapture::CacheShader(bytes,size,fact);
            RuntimeCapture::ShaderCreated(shader,bytes,size,fact);
            RuntimeCapture::RawShaderCreated(shader,bytes,size,nativeDeviceEpoch,1);}
        else {++shaderCreations.failed;
            GenshinDepthEvidence::RetainVertex(shader,bytes,size,GenshinDepthEvidence::ByteHash(bytes,size));}}
    catch(...){}
}
void NoteRawShaderCreated(ID3D11Device* device,ID3D11DeviceChild* shader,const void* bytes,size_t size,unsigned stage)noexcept
{
    if(!game || internal)return;
    try{const auto captureControls=CaptureControlsAtBoundary();GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);
        if(captureControls)ObserveCaptureControls(*captureControls);
        if(device==nativeDevice.Get()){
            // The visible CreateVertexShader hook reports stage 0. Retain its
            // actual bytes too: both Scene/Picture replay and depth-writer
            // semantics consume BytesKey from that same returned VS object.
            // RetainVertex keeps its existing creation-time size/budget bounds.
            if(bytes && size){const auto hash=GenshinDepthEvidence::ByteHash(bytes,size);
                GenshinDepthEvidence::RetainVertex(shader,bytes,size,hash);
                DrawObservationDx11::RememberShader(shader,hash,size);}
            if(RuntimeCapture::Enabled())RuntimeCapture::RawShaderCreated(shader,bytes,size,nativeDeviceEpoch,stage);
        }}
    catch(...){}
}
Action BeforeNativeDraw(ID3D11DeviceContext* context,UINT count,bool inputsEnabled,bool countKnown,unsigned drawKind,
    const ControlsSnapshot* acquiredControls,bool knownNonTargetDraw,OptionalDrawObserver* observer)noexcept
{
    bool collected=false;
    const auto collectFull=[&]()noexcept{
        ownerObservation.srOnly=false;
        if(observer){observer->full=true;if(!collected && observer->collect){collected=true;observer->collect(context);}}
    };
    if(observer)observer->full=true;
    if(!game || internal || !context){collectFull();return Action::ContinueOriginal;}
    // Count rejects SR capture only. Full consumer observation remains under the
    // original owner lock; no enabled option is treated as an absent reader.
    const bool nonTargetDraw=knownNonTargetDraw && countKnown && count!=0 && count!=3;
    GenshinCpuStageTiming::Scope hostEntry(GenshinCpuStageTiming::Stage::HostBeforeNativeDraw);
    try
    {
        auto controls=acquiredControls?*acquiredControls:Config::Instance()->AcquireGenshinControls();
        controls.enableFsr2Inputs=controls.enableFsr2Inputs && inputsEnabled;
        GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);
        nativeStatus.srAccepted=false;
        // Classify only target capture before any context/device/shader getter.
        // The retained immediate context/device pair is immutable and protected
        // by nativeMutex. Any unknown owner, transition or drain stays slow.
        const bool sameOwner=SameNativeOwner(context,controls);
        const bool captureOff=countKnown && count && drawKind>=1 && drawKind<=4 && sameOwner &&
            CaptureObservationCurrent(controls);
        ObserveCaptureControls(controls,captureOff);
        bool prefilteredNonTarget=sameOwner && nonTargetDraw;
        // Configure owns another mutex. A stable owner with the same controls
        // already configured ordinary (non-capture) observation at device/entry
        // setup. Do not repeat its disabled-state lock/reset on every non-target
        // draw. Active provenance, capture, unknown owners and transitions still
        // configure before any observer; no consumer subscription is changed.
        if((!prefilteredNonTarget && !captureOff) || controls.settings.captureOnlyObservation || ResourceProof::Active())
            ResourceProof::Configure(nativeDevice.Get(),nativeDeviceEpoch,controls.settings.enabled && controls.settings.captureOnlyObservation);
        ComPtr<ID3D11Device> queriedDevice;
        ID3D11Device* drawDevice=nativeDevice.Get();
        // A retained immediate context cannot change its type or device. Reuse
        // that actual owner for target and non-target draws alike.
        if(!sameOwner){
            if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE){collectFull();return Action::ContinueOriginal;}
            context->GetDevice(&queriedDevice);drawDevice=queriedDevice.Get();
            if(drawDevice!=nativeDevice.Get()){collectFull();return Action::ContinueOriginal;}
        }
        const bool fullObservation=!SelectSrOnlyObservation(context,controls);
        if(fullObservation)ownerObservation.srOnly=false;
        if(observer)observer->full=fullObservation;
        if(fullObservation)collectFull();
        if(nativePipeline)nativePipeline->SubscribeDrawObservation(fullObservation);
        // R162: an explicit internal-Genshin owner/config interval only.
        // The original native draw runs outside this scope.
        PsState::DrawScope psScope(&shaderCreationOwner,context,drawDevice,nativeDeviceEpoch,controls.epoch,
            controls.Active() && shaderHookInstalled && nativeStatus.entryInstalled &&
            (!nativePipeline || (context==nativeContext.Get() && GetCurrentThreadId()==nativeThread &&
                !nativePipeline->NeedsDrain() && nativeControls.epoch==controls.epoch && nativeControls.settings==controls.settings)),
            +[](uint64_t epoch){return Config::Instance()->GenshinControlsCurrent(epoch);});
        // Count never filters observers: non-three draws can be depth writers,
        // manual samples, downstream visible draws or Present picture draws.
        {struct ObserverGuard{ObserverGuard(){internal=true;}~ObserverGuard(){internal=false;}}guard;
            // Requests can arrive after classification, without nativeMutex.
            if(fullObservation && (!captureOff || manualArm.load(std::memory_order_acquire) ||
               manualStop.load(std::memory_order_acquire) || RuntimeCapture::Get().active.load(std::memory_order_acquire) ||
               !Config::Instance()->GenshinControlsCurrent(controls.epoch))){
                ObserveManualDraw(drawDevice,context,count,countKnown,controls.epoch);
                // The same invocation already notified PS/writer controls above.
                // Manual collection can start a session, so recheck its Stop only;
                // keep immediate session revocation without a second config publish.
                ObserveCaptureSessionControls(controls);
            }}
        if(nativePipeline && GetCurrentThreadId()!=nativeThread)
        {nativeStatus.reason=CaptureReason::ThreadMismatch;UpdateControlsStatus(controls);return Action::ContinueOriginal;}
        // Raw shader dumps/CB+IA readbacks are an explicit capture consumer.
        // Real depth writers still enter BeforeNativeDepthRaster/ReturnedRaster;
        // BeginDrawObservation retains the functional Scene/Picture mapping.
        if(fullObservation && RouteActive(controls) && shaderHookInstalled && RuntimeCapture::Enabled()){
            struct ObserverGuard{ObserverGuard(){internal=true;}~ObserverGuard(){internal=false;}}guard;
            const auto shader=Family::Current(context,drawDevice,nativeDeviceEpoch);
            GenshinDepthEvidence::ObserveRaster(context,shader.hash,nativeDeviceEpoch,controls.epoch);
            if(GenshinDepthEvidence::RasterExportPending(context)){
                const auto stamp=GenshinReturnedRaster::ObservationStamp(context);
                GenshinDepthEvidence::MarkSampledCall(context,stamp[0],stamp[1]);}
        }else {
            GenshinDepthEvidence::PendingRasterExport()=0;
            if(!RouteActive(controls) || !shaderHookInstalled)GenshinDepthEvidence::ForgetDepthInput(context);
        }
        if((nativePipeline && nativePipeline->NeedsDrain()) ||
            !RouteActive(controls) || nativeControls.epoch!=controls.epoch || nativeControls.settings!=controls.settings ||
            (nativeContext && nativeContext.Get()!=context))
        {
            prefilteredNonTarget=false;
            if(!DrainNativePipeline()){UpdateControlsStatus(controls);return Action::ContinueOriginal;}
            nativeControls=controls;
        }
        UpdateControlsStatus(controls);
        if(!RouteActive(controls)){nativeStatus.reason=CaptureReason::Disabled;return Action::ContinueOriginal;}
        TryInstallEntry();if(!nativeStatus.entryInstalled)return Action::ContinueOriginal;
        // Creation records may precede hook-status publication. Retention alone
        // never admits SR/family work while the hook is unpublished.
        if(!shaderHookInstalled){nativeStatus.reason=CaptureReason::ShaderUnavailable;return Action::ContinueOriginal;}
        struct Guard{Guard(){internal=true;}~Guard(){internal=false;}}guard;
        if(prefilteredNonTarget){
            const auto result=nativePipeline->BeforeNonTargetDraw(count,drawKind);
            nativeStatus=nativePipeline->GetStatus();UpdateControlsStatus(controls);return result;
        }
        if(!nativePipeline){nativePipeline=std::make_unique<CapturePipeline>(context,controls.settings,Consumer{},nativeDeviceEpoch,controls.epoch);
            nativeContext=context;nativeThread=GetCurrentThreadId();nativeWorkOwned.store(true,std::memory_order_release);}
        nativePipeline->SubscribeDrawObservation(fullObservation);
        auto result=nativePipeline->BeforeDraw(count,countKnown && shaderHookInstalled,countKnown,drawKind);nativeStatus=nativePipeline->GetStatus();UpdateControlsStatus(controls);return result;
    }
    catch(...){collectFull();drawObservation={};return Action::ContinueOriginal;}
}
// Called with nativeMutex held, after resolving the current draw controls.
// No retained diagnostic epoch/LOCATION is an authorization input.
static bool ReturnedRasterCurrent(ID3D11DeviceContext* context,const ControlsSnapshot& controls)noexcept
{
    if(!context || !RouteActive(controls) || !shaderHookInstalled ||
       !Config::Instance()->GenshinControlsCurrent(controls.epoch))return false;
    if(nativePipeline && context==nativeContext.Get() && GetCurrentThreadId()==nativeThread &&
       nativeDevice && nativeDeviceEpoch && nativeControls.epoch==controls.epoch &&
       nativeControls.settings==controls.settings && !nativePipeline->NeedsDrain())return true;
    if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
    ComPtr<ID3D11Device> device;context->GetDevice(&device);
    return device.Get()==nativeDevice.Get() && nativeDeviceEpoch &&
        (!nativePipeline || (context==nativeContext.Get() && GetCurrentThreadId()==nativeThread &&
          nativeControls.epoch==controls.epoch && nativeControls.settings==controls.settings));
}
// This is controls reuse, not a retained draw/owner/resource receipt. Keep the
// fallback Config acquisition BEFORE nativeMutex, with no lock across the draw.
static ControlsSnapshot AcquireDrawControls(ID3D11DeviceContext* context,const DrawControlsSnapshot* drawControls)
{
    const auto& config=*Config::Instance();
    if(drawControls && context && drawControls->context==context && drawControls->thread==GetCurrentThreadId() &&
       drawControls->controls.version==1 && drawControls->controls.bytes==sizeof(ControlsSnapshot) &&
       config.GenshinControlsCurrent(drawControls->controls.epoch))return drawControls->controls;
    // All existing Enable/Use writers now synchronize and publish under Config.
    // Never inspect mutable optionals here. Unknown/stale snapshots reacquire.
    return config.AcquireGenshinControls();
}
void BeforeNativeDepthRaster(ID3D11DeviceContext* context,const DrawControlsSnapshot* drawControls)noexcept
{
    if(!game || internal || GenshinPresentNr::Scope::Current() ||
        (!NativeObservationActive() && !NativeObservationDrainNeeded()))return;
    try{const auto controls=AcquireDrawControls(context,drawControls);GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);
        if(ObservationOnlyCurrent(context,controls.epoch))return;
        if(!ReturnedRasterCurrent(context,controls)){
            GenshinReturnedRaster::Invalidate(context,nullptr,GenshinReturnedRaster::Boundary::Owner);return;}
        ownerObservation.srOnly=false;
        if(nativePipeline)nativePipeline->SubscribeDrawObservation(true);
        if(GenshinDepthEvidence::RasterExportPending(context)){
            const auto stamp=GenshinReturnedRaster::ObservationStamp(context);
            GenshinDepthEvidence::MarkOriginalCall(context,stamp[0],stamp[1]);}
        GenshinReturnedRaster::BeforeRaster(context,{nativeDeviceEpoch,controls.epoch});
    }catch(...){GenshinReturnedRaster::Invalidate(context,nullptr,GenshinReturnedRaster::Boundary::Owner);}
}
static bool BeforeModRasterKind(ID3D11DeviceContext* context,UINT kind,UINT count,UINT first,INT base,
    ModRasterObservation& token,UINT instances=1,UINT firstInstance=0)noexcept
{
    token={};
    if(!game || internal || GenshinPresentNr::Scope::Current() || !context)return false;
    try{
        const auto controls=Config::Instance()->AcquireGenshinControls();
        // Current selected consumers, without a hot-path demand publication/log.
        const bool demanded=RouteActive(controls) || OptionalGuideDemand() ||
            Config::Instance()->HoYoShadeEnabled.value_or_default() || manualArm.load(std::memory_order_acquire) ||
            RuntimeCapture::Get().active.load(std::memory_order_acquire);
        // Preserve the physical API kind and every typed argument. Instanced
        // calls remain kind 4, including zero/one instances and signed base.
        // No SR BeforeDraw/submit path or count-times-instances surrogate.
        GenshinDepthMapping::ObserveDrawArguments(context,kind,count,first,base,instances,firstInstance);
        token.draw={controls,context,GetCurrentThreadId()};token.drawKind=kind;
        token.observationCall=GenshinDepthMapping::CurrentDraw().observationCall;
        if(!token.observationCall){token={};return false;}
        if(!demanded && !NativeObservationDrainNeeded())return true;
        std::lock_guard lock(nativeMutex);
        if(ObservationOnlyCurrent(context,controls.epoch))return true;
        if(!ReturnedRasterCurrent(context,controls)){
            GenshinReturnedRaster::Invalidate(context,nullptr,GenshinReturnedRaster::Boundary::Owner);return true;}
        ownerObservation.srOnly=false;
        if(nativePipeline)nativePipeline->SubscribeDrawObservation(true);
        token.deviceEpoch=nativeDeviceEpoch;
        if(GenshinDepthEvidence::RasterExportPending(context)){
            const auto stamp=GenshinReturnedRaster::ObservationStamp(context);
            GenshinDepthEvidence::MarkOriginalCall(context,stamp[0],stamp[1]);}
        GenshinReturnedRaster::BeforeRaster(context,{nativeDeviceEpoch,controls.epoch});
        token.observed=true;return true;
    }catch(...){token={};GenshinReturnedRaster::Invalidate(context,nullptr,GenshinReturnedRaster::Boundary::Owner);return false;}
}
static bool AfterModRasterKind(ID3D11DeviceContext* context,UINT kind,ModRasterObservation& token,
    bool argumentsMatch=true)noexcept
{
    const auto observation=std::exchange(token,{});
    const auto& actual=GenshinDepthMapping::CurrentDraw();
    if(!argumentsMatch || !context || observation.drawKind!=kind || !observation.observationCall ||
       observation.draw.context!=context || observation.draw.thread!=GetCurrentThreadId() ||
       actual.context!=context || actual.thread!=GetCurrentThreadId() || actual.kind!=kind ||
       actual.observationCall!=observation.observationCall){
        GenshinReturnedRaster::Invalidate(context,nullptr,GenshinReturnedRaster::Boundary::Owner);return false;}
    if(!observation.observed)return true;
    try{
        const auto controls=AcquireDrawControls(context,&observation.draw);
        std::lock_guard lock(nativeMutex);
        if(observation.draw.context!=context || observation.draw.thread!=GetCurrentThreadId() ||
            observation.deviceEpoch!=nativeDeviceEpoch || controls.epoch!=observation.draw.controls.epoch ||
            !ReturnedRasterCurrent(context,controls)){
            GenshinReturnedRaster::Invalidate(context,nullptr,GenshinReturnedRaster::Boundary::Owner);return false;}
        GenshinReturnedRaster::RasterReturned(context,{nativeDeviceEpoch,controls.epoch});
        GenshinDepthEvidence::ExportRasterReturn(context,[&](ID3D11Resource* resource,UINT mip){
            return GenshinReturnedRaster::ObservationLatest(context,resource,mip);});
        // Deliberately no AfterOriginalDraw, SR submit, GPU commands or transport mutation.
        return true;
    }catch(...){GenshinReturnedRaster::Invalidate(context,nullptr,GenshinReturnedRaster::Boundary::Owner);return false;}
}
bool BeforeModRaster(ID3D11DeviceContext* context,UINT count,UINT first,INT base,ModRasterObservation& token)noexcept
{ return BeforeModRasterKind(context,2,count,first,base,token); }
bool AfterModRaster(ID3D11DeviceContext* context,ModRasterObservation& token)noexcept
{ return AfterModRasterKind(context,2,token); }
bool BeforeModDraw(ID3D11DeviceContext* context,UINT vertexCount,UINT firstVertex,ModRasterObservation& token)noexcept
{ return BeforeModRasterKind(context,1,vertexCount,firstVertex,0,token); }
bool AfterModDraw(ID3D11DeviceContext* context,ModRasterObservation& token)noexcept
{ return AfterModRasterKind(context,1,token); }
bool BeforeModIndexedInstanced(ID3D11DeviceContext* context,UINT count,UINT instances,UINT first,INT base,
    UINT firstInstance,bool originalBoundary,ModRasterObservation& token)noexcept
{
    if(!BeforeModRasterKind(context,4,count,first,base,token,instances,firstInstance))return false;
    token.originalBoundary=originalBoundary;return true;
}
bool AfterModIndexedInstanced(ID3D11DeviceContext* context,UINT count,UINT instances,UINT first,INT base,
    UINT firstInstance,bool originalBoundary,ModRasterObservation& token)noexcept
{
    const auto& actual=GenshinDepthMapping::CurrentDraw();
    const bool argumentsMatch=actual.count==count && actual.start==first && actual.base==base &&
        actual.instances==instances && actual.instanceStart==firstInstance &&
        token.originalBoundary==originalBoundary;
    // Consume the real PRE token even on a rejected POST, before any
    // RasterReturned/export. A mismatch must never publish a return receipt.
    return AfterModRasterKind(context,4,token,argumentsMatch);
}
void BeforeNativeDepthClear(ID3D11DeviceContext* context,ID3D11DepthStencilView* view,UINT flags,float depth,UINT8 stencil)noexcept
{
    if(!game || internal || GenshinPresentNr::Scope::Current())return;
    try{const auto controls=Config::Instance()->AcquireGenshinControls();GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);
        if(OwnerObservationCurrent(context,controls.epoch))return;
        if(!ReturnedRasterCurrent(context,controls)){
            GenshinReturnedRaster::Invalidate(context,nullptr,GenshinReturnedRaster::Boundary::Owner);return;}
        ownerObservation.srOnly=false;
        GenshinReturnedRaster::BeforeClear(context,{nativeDeviceEpoch,controls.epoch},view,flags,depth,stencil);
    }catch(...){GenshinReturnedRaster::Invalidate(context,nullptr,GenshinReturnedRaster::Boundary::Owner);}
}
void AfterNativeDepthClear(ID3D11DeviceContext* context,ID3D11DepthStencilView* view,UINT flags,float depth,UINT8 stencil)noexcept
{
    if(!game || internal || GenshinPresentNr::Scope::Current())return;
    try{const auto controls=Config::Instance()->AcquireGenshinControls();GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);
        if(OwnerObservationCurrent(context,controls.epoch))return;
        if(!ReturnedRasterCurrent(context,controls)){
            GenshinReturnedRaster::Invalidate(context,nullptr,GenshinReturnedRaster::Boundary::Owner);return;}
        ownerObservation.srOnly=false;
        GenshinReturnedRaster::ClearReturned(context,{nativeDeviceEpoch,controls.epoch},view,flags,depth,stencil);
    }catch(...){GenshinReturnedRaster::Invalidate(context,nullptr,GenshinReturnedRaster::Boundary::Owner);}
}
void AfterNativeDraw(ID3D11DeviceContext* context,UINT count,bool countKnown,const DrawControlsSnapshot* drawControls)noexcept
{
    if(!game || internal || (!NativeObservationActive() && !NativeObservationDrainNeeded()))return;
    GenshinCpuStageTiming::Scope hostAfter(GenshinCpuStageTiming::Stage::HostAfterNativeDraw); GenshinHookCpuTiming::Scope r448AfterDraw(GenshinHookCpuTiming::Stage::AfterDraw);
    try{const auto controls=AcquireDrawControls(context,drawControls);GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);
        const bool captureOff=countKnown && count && ownerObservation.captureOff &&
            SameNativeOwner(context,controls) && CaptureObservationCurrent(controls);
        ObserveCaptureControls(controls,captureOff);
        const bool srOnly=ObservationOnlyCurrent(context,controls.epoch);
        if(!srOnly){
            ownerObservation.srOnly=false;
            if(ReturnedRasterCurrent(context,controls))
                GenshinReturnedRaster::RasterReturned(context,{nativeDeviceEpoch,controls.epoch});
            else GenshinReturnedRaster::Invalidate(context,nullptr,GenshinReturnedRaster::Boundary::Owner);
            GenshinDepthEvidence::ExportRasterReturn(context,[&](ID3D11Resource* resource,UINT mip){
                return GenshinReturnedRaster::ObservationLatest(context,resource,mip);});
        }
        if(context==nativeContext.Get() && nativePipeline && GetCurrentThreadId()==nativeThread)
        {struct Guard{Guard(){internal=true;}~Guard(){internal=false;}}guard;
            // The original game draw has already executed. A changed owner may
            // drain, but it cannot submit or replace that draw's native result.
            if(!RouteActive(controls) || controls.epoch!=nativeControls.epoch || controls.settings!=nativeControls.settings){
                nativePipeline->RevokeTransport();DrainNativePipeline();NoteControls(controls);return;}
            nativePipeline->SubscribeDrawObservation(!srOnly);
            // R735: the Before scope ended before the original draw. Rebind only
            // after its return; owner/context/thread/controls match above. Keep
            // the remaining Before eligibility and current-controls checks.
            // No shader lookup follows on other counts. Avoid repeating the
            // public binding/coverage qualification just to close an empty scope.
            // The hook's NativeDrawReturnScope still observes the real API return;
            // depth/raster/picture callbacks and their live gates remain below.
            std::optional<PsState::DrawScope> psScope;
            if(ReturnedDrawMayReadShader(count,countKnown))
                psScope.emplace(&shaderCreationOwner,context,nativeDevice.Get(),nativeDeviceEpoch,controls.epoch,
                    controls.Active() && shaderHookInstalled && nativeStatus.entryInstalled && !nativePipeline->NeedsDrain(),
                    +[](uint64_t epoch){return Config::Instance()->GenshinControlsCurrent(epoch);});
            nativePipeline->AfterOriginalDraw(count,countKnown);nativeStatus=nativePipeline->GetStatus();}}catch(...){}
}
void ObserveNativeDepthCompute(ID3D11DeviceContext* context,UINT x,UINT y,UINT z)noexcept
{
    if(!game || internal || GenshinPresentNr::Scope::Current() || !context)return;
    try{
        const auto controls=Config::Instance()->AcquireGenshinControls();
        GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked); // Preserve Config -> native -> samples order.
        if(OwnerObservationCurrent(context,controls.epoch))return;
        ownerObservation.srOnly=false;
        if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
        ComPtr<ID3D11Device> device;context->GetDevice(&device);
        if(device.Get()!=nativeDevice.Get())return;
        if(!RouteActive(controls) || !shaderHookInstalled){GenshinDepthEvidence::ForgetDepthInput(context);return;}
        if(nativePipeline && (context!=nativeContext.Get() || GetCurrentThreadId()!=nativeThread))return;
        // Without an SR pipeline, only the actual native transaction's retained
        // same-thread LOCATION may be inspected. It grants no frame/depth fact.
        if(!GenshinDepthEvidence::LocationCurrent(context,GetCurrentThreadId(),nativeDeviceEpoch,controls.epoch)){
            GenshinDepthEvidence::ForgetDepthInput(context);return;
        }
        struct Guard{bool prior=internal;Guard(){internal=true;}~Guard(){internal=prior;}}guard;
        if(RuntimeCapture::Enabled())GenshinDepthEvidence::ObserveCompute(context,x,y,z,nativeDeviceEpoch,controls.epoch);
    }catch(...){}
}
void AfterContextBoundary(ID3D11DeviceContext* context)noexcept
{
    if(!game || internal || !context)return;
    // Preserve the owner, not its binding, through the opaque callback work.
    // The GPU/query/content invalidation below is intentionally unchanged.
    PsState::Mutation psBoundary(context);
    try{const auto captureControls=CaptureControlsAtBoundary();GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);
        ownerObservation.srOnly=false;
        if(captureControls)ObserveCaptureControls(*captureControls);
        ComPtr<ID3D11Device> device;context->GetDevice(&device);
        if(device.Get()!=nativeDevice.Get())return;
        GenshinReturnedRaster::Invalidate(context,nullptr,GenshinReturnedRaster::Boundary::State);
        GenshinDepthEvidence::RevokeDepthOwner(context);
        // Unknown deferred work can write any native guide. No execution/content proof is fabricated.
        StopManualCapture(ManualCapture::Reason::ContextBoundary);
        ResourceProof::InvalidateContext(context);
        RetirePendingRenderTokens();
        if(nativePipeline){
            struct Guard{Guard(){internal=true;}~Guard(){internal=false;}}guard;
            nativePipeline->Invalidate();nativeStatus=nativePipeline->GetStatus();
        }
        nativeStatus.srAccepted=false;nativeStatus.effectiveFamilySkip=false;NoteControls(nativeControls);
    }catch(...){}
}
void ObserveNativePresentTarget(IDXGISwapChain* swapchain,std::uint64_t identity)noexcept
{
    if(!game || !ResourceProof::Active())return;
    try{const auto captureControls=CaptureControlsAtBoundary();GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);
        if(captureControls)ObserveCaptureControls(*captureControls);
        if(nativePipeline && GetCurrentThreadId()==nativeThread)
        nativePipeline->ObservePresentTarget(swapchain,identity);}catch(...){}
}
GenshinPresentNr::Ticket TakePresentNr(IDXGISwapChain* swapchain,ID3D11Device* device) noexcept
{
    if(!game || !swapchain || !device || GenshinPresentNr::Scope::Current())return {};
    try {
        const auto& cfg=*Config::Instance();auto* fg=State::Instance().currentFG;
        const bool enabled=cfg.DlssNrEnabled.value_or_default(),finished=cfg.DlssNrFinishedPicture.value_or_default();
        const bool before=cfg.DlssNrRunBeforeSr.value_or_default(),deferred=cfg.DlssNrDeferredDlss.value_or_default();
        const bool nativeParent=GenshinNativeParentFrame::OwnsNative(swapchain,device);
        const bool optical=native::OpticalFgNrEnabled(),interop=!nativeParent && State::Instance().swapchainInteropApi!=SwapchainInteropApi::None;
        const bool gameFg=!nativeParent && fg && fg->IsActive() && !fg->IsPaused();
        const auto note=[&](const char* reason) {
            if(auto* r=GenshinPresentNrTrace::Current();r && !r->ticketSeq && strcmp(reason,"ticket-rejected")!=0 && strcmp(reason,"ticket-ready")!=0) {
                r->ticketSeq=GenshinPresentNrTrace::Sequence();r->reject=reason;
            }
            if(!enabled || !finished)return;
            static thread_local uint64_t calls=0,lastAt=0;static thread_local const char* lastReason=nullptr;
            const auto count=++calls,now=GetTickCount64();
            if(count<=3 || count%300==0 || (reason!=lastReason && now-lastAt>=1000)) {
                lastReason=reason;lastAt=now;
                LOG_INFO("Genshin presented NR gate: pid={} stage=caller reason={} calls={} thread={} before={} deferred={} opticalFg={} interop={} gameFg={}",
                    GetCurrentProcessId(),reason,count,GetCurrentThreadId(),before,deferred,optical,interop,gameFg);
            }
        };
        if(!GenshinPresentNr::Route(enabled,finished,before,deferred,optical,interop,gameFg)) {note("mode");return {};}
        const auto controls=cfg.AcquireGenshinControls();GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);
        if(!nativePipeline){note("no-pipeline");return {};}
        if(GetCurrentThreadId()!=nativeThread){note("owning-thread");return {};}
        if(device!=nativeDevice.Get()){note("device");return {};}
        if(!RouteActive(controls)){note("sr-route-disabled");return {};}
        if(controls.epoch!=nativeControls.epoch || controls.settings!=nativeControls.settings){note("controls-epoch");return {};}
        // Optional mapping creation/dispatch is internal work under nativeMutex;
        // its shader/copy hooks must not re-enter that same mutex or self-revoke.
        struct InternalGuard{bool previous=internal;InternalGuard(){internal=true;}~InternalGuard(){internal=previous;}} guard;
        auto ticket=nativePipeline->TakePresentNr(swapchain,State::Instance().frameCount);
        note(ticket.Known()?"ticket-ready":"ticket-rejected");return ticket;
    }catch(...){return {};}
}
GenshinBridgeFg::SenderInput TakePresentFg(IDXGISwapChain* swapchain,ID3D11Device* device) noexcept
{
    GenshinBridgeFg::SenderInput result;
    auto* scope=GenshinBridgeFg::PresentScope::Current();
    if(!game || !scope || !scope->MatchesNative(swapchain,device) || GenshinPresentNr::Scope::Current())return result;
    try {
        const auto& cfg=*Config::Instance();
        result.pictureKind=GenshinBridgeFg::CurrentPictureKind();
        if(result.pictureKind==GenshinPresentNr::FgPictureKind::Unknown)return result;
        const auto controls=cfg.AcquireGenshinControls();GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);
        if(!nativePipeline || GetCurrentThreadId()!=nativeThread || device!=nativeDevice.Get() ||
           !RouteActive(controls) || controls.epoch!=nativeControls.epoch || controls.settings!=nativeControls.settings ||
           !scope->MatchesNative(swapchain,device))return result;
        struct InternalGuard{bool previous=internal;InternalGuard(){internal=true;}~InternalGuard(){internal=previous;}} guard;
        result.ticket=nativePipeline->TakePresentNr(swapchain,State::Instance().frameCount,&result.producer);
        result.provider=reinterpret_cast<uintptr_t>(State::Instance().currentFG);
        result.providerSwapchain=GenshinPresentNr::Identity(State::Instance().currentFGSwapchain);
        if(result.pictureKind!=GenshinBridgeFg::CurrentPictureKind()) {
            result.pictureKind=GenshinPresentNr::FgPictureKind::Unknown;
            result.producer.reset();result.provider=result.providerSwapchain=0;
            return result; // ordinary ticket remains independently authenticated
        }
        // Read only the canonical successful SR History authenticated by the
        // independently acquired ticket; never borrow a newest global camera.
        GenshinBridgeFg::CameraObservation camera;
        if(nativePipeline->CameraForPresent(result.ticket,camera))result.camera=camera;
        // Missing depth/camera still leaves the ordinary ticket intact.
        if(result.camera && result.camera->Matches(result.ticket))result.Seal();
        const unsigned offerStage=!result.ticket.Known()?0u:!result.ticket.DepthMatches()?1u:
            !result.ProducerMatches()?2u:!result.camera?3u:!result.Known()?4u:5u;
        static std::array<unsigned,6> offerReports{};
        // Continue observing real offer transitions after the warmup caps.
        // Frame/generation are provenance, never transition keys.
        static thread_local uint64_t lateCalls=0,lastReport=0,pending=0;
        static thread_local unsigned lastStage=~0u,transitions=0;
        const auto count=++lateCalls;if(count%256==1)transitions=0;
        if(offerStage!=lastStage)++pending;lastStage=offerStage;
        const bool warmup=offerReports[offerStage]<8;
        if(warmup || count%1024==0 || (pending && transitions<4)) {
            if(warmup)++offerReports[offerStage];++transitions;
            LOG_INFO("R827 FG late offer: pid={} calls={} stage={} first_failure={} transitions={} since_report={} generation={} picture={} source_frame={} present_epoch={} owner_epoch={} config_epoch={} kind={} no_authority=true",
                GetCurrentProcessId(),count,offerStage,offerStage<5 && offerReports[offerStage]==1,pending,count-lastReport,
                result.ticket.source.generation,result.ticket.pictureSerial,result.ticket.source.sourceFrame,result.ticket.presentEpoch,
                result.ticket.source.ownerEpoch,result.ticket.source.configEpoch,static_cast<unsigned>(result.pictureKind));
            pending=0;lastReport=count;
            if(warmup)LOG_INFO("R827 FG offer: pid={} generation={} stage={} ticket={} depth={} producer={} producer_matches={} camera={} camera_matches={} sealed={} provider={} swapchain={} no_authority=true",
                GetCurrentProcessId(),result.ticket.source.generation,offerStage,result.ticket.Known(),result.ticket.DepthMatches(),
                bool(result.producer),result.ProducerMatches(),bool(result.camera),result.camera && result.camera->Matches(result.ticket),
                result.Known(),result.provider!=0,result.providerSwapchain!=0);
        }
        // A valid native ticket is retained for ordinary NR fallback even when
        // FG-only depth/camera/producer admission is incomplete. Known() still
        // rejects it for FG; never reacquire/consume this picture twice.
        return result;
    }catch(...){return {};}
}
// R721: output only dirty explicit-event receipts at the existing Present boundary.
// All formatting/logging is here, outside nativeMutex and every draw/provider call.
static std::string PresetSourceText(const GenshinSubstage::PresetSource& s)
{
    if(!s.Known())return "UNKNOWN";
    return std::format("pid={} start={} device_epoch={} controls_epoch={} owner_epoch={} context={} attempt={} instance={} generation={} source_frame={} history_epoch={}",
        s.key.session.processId,s.key.session.processStart,s.key.session.deviceEpoch,s.key.session.controlsEpoch,
        s.key.ownerEpoch,s.key.context,s.key.serial,s.instance,s.generation,s.frame,s.historyEpoch);
}
static const char* PresetSourceRelation(const GenshinSubstage::PresetSource& a,const GenshinSubstage::PresetSource& b) noexcept
{
    if(!a.Known() || !b.Known())return "UNKNOWN";
    return GenshinSubstage::SamePresetSource(a,b)?"SAME_SOURCE":"DIFFERENT_SOURCE_UNPAIRED";
}
static std::string PresetSrText(const GenshinSubstage::PresetSrResult& r)
{
    if(!r.observed)return "UNKNOWN";
    std::string result=std::format("tick={} source=[{}] game_rtv_copy_ordered={} ",r.tick,PresetSourceText(r.source),r.copyOrdered);
    if(!r.progress.known)return result+"stages=UNKNOWN";
    const auto stage=[&](GenshinSubstage::Stage which) {
        const auto& e=r.progress.stages[GenshinSubstage::Index(which)];
        const char* state="UNKNOWN";
        switch(e.state) {
        case GenshinSubstage::State::Waiting:state="WAITING";break;
        case GenshinSubstage::State::Running:state="RUNNING";break;
        case GenshinSubstage::State::Succeeded:state="SUCCEEDED";break;
        case GenshinSubstage::State::Failed:state="FAILED";break;
        default:break;
        }
        return std::format("state={} domain={} code={} issue={}",state,unsigned(e.domain),e.code,unsigned(e.issue));
    };
    return result+std::format("provider=[{}] consumer_acceptance=[{}] native_claim=[{}] native_output=[{}] snapshot_at_sr_return=true",
        stage(GenshinSubstage::Stage::ProviderSuccess),stage(GenshinSubstage::Stage::ConsumerAcceptance),
        stage(GenshinSubstage::Stage::NativeClaim),stage(GenshinSubstage::Stage::NativeOutput));
}
static void PublishPresetReceipt() noexcept
{
    try {
        if(!GenshinSubstage::Runtime().PresetReceiptPending())return;
        GenshinSubstage::PresetReceipt r;
        if(!GenshinSubstage::Runtime().TakePresetReceipt(r))return;
        LOG_INFO("R721 NR preset receipt: pid={} request={} revision={} superseded={} pass={} requested_from={} requested_to={} request_tick={} request_source=UNKNOWN render_rebuild_entered={} render_from={} rebuild_tick={} rebuild_source=[{}] create_return={} create_tick={} nr_create_epoch={} nr_generation=UNKNOWN create_source=[{}] first_sr_after_request=[{}] request_sr_vs_rebuild={} first_sr_after_create=[{}] create_sr_vs_create={} causal_proof=UNKNOWN chronology_not_causality=true gpu_complete=UNKNOWN pixels=UNKNOWN",
            GetCurrentProcessId(),r.request,r.revision,r.superseded,r.pass+1,r.from,r.to,r.requestTick,r.rebuilding?"OBSERVED":"UNKNOWN",
            r.rebuilding?std::to_string(r.renderFrom):"UNKNOWN",r.rebuilding?std::to_string(r.rebuildTick):"UNKNOWN",
            PresetSourceText(r.rebuildSource),r.created?(r.createSucceeded?"CPU_FEATURE_RETURNED":"CPU_CREATE_FAILED"):"UNKNOWN",
            r.created?std::to_string(r.createTick):"UNKNOWN",r.created?std::to_string(r.createEpoch):"UNKNOWN",
            PresetSourceText(r.createSource),PresetSrText(r.firstAfterRequest),
            PresetSourceRelation(r.firstAfterRequest.source,r.rebuildSource),PresetSrText(r.firstAfterCreate),
            PresetSourceRelation(r.firstAfterCreate.source,r.createSource));
    }catch(...){} // Receipt failure cannot change provider, owners or Present.
}

// Existing Present only, before nativeMutex. Never consumes/refreshes cache authority.
static void PublishShaderCacheWindow()
{
    using namespace GenshinHookCpuTiming;
    namespace M=Family::ShaderCacheMetrics;
    using C=ShaderCacheSnapshot;
    struct Emitted {uint64_t end=0,epoch=0,unknownEpoch=UINT64_MAX,lastPollMs=0;bool unknown=false,polled=false;};
    static thread_local Emitted emitted;
    if(!Enabled()){emitted.unknown=false;return;}
    // One bounded snapshot/output opportunity per second, even when window
    // availability or timing epochs alternate on consecutive Presents.
    const auto now=GetTickCount64();
    if(emitted.polled && now-emitted.lastPollMs<1000)return;
    emitted.polled=true;emitted.lastPollMs=now;
    const auto epoch=monitorEpoch.load(std::memory_order_acquire);
    const auto v=ReadShaderCache();
    const bool complete=v[C::Frames] && v[C::ThreadId]==GetCurrentThreadId() && v[C::EndMs]>v[C::BeginMs] && v[C::TimingEpoch]==epoch;
    if(!complete){
        if(!emitted.unknown || emitted.unknownEpoch!=epoch){
            emitted.unknown=true;emitted.unknownEpoch=epoch;
            LOG_INFO("R898 PS cache window: state=UNKNOWN reason=no_complete_current_owner_window thread={} epoch={} hits=UNKNOWN misses=UNKNOWN cost=UNKNOWN no_performance_claim=true",GetCurrentThreadId(),epoch);
        }
        return;
    }
    emitted.unknown=false;
    if(emitted.end==v[C::EndMs] && emitted.epoch==epoch)return;
    emitted.end=v[C::EndMs];emitted.epoch=epoch;
    const auto& d=v.detail;
    std::string costs;
    const auto append=[&](const char* name,const M::Reading& r){
        costs+=std::format(" {}[calls={},samples={},ticks={},max_ticks={},clock_failed={}]",name,r.calls,r.samples,r.ticks,r.max,r.clockFailed);
    };
    append("Current.binding_hit",d.costs[static_cast<unsigned>(M::Cost::CurrentHit)]);
    append("Current.binding_miss",d.costs[static_cast<unsigned>(M::Cost::CurrentMiss)]);
    append("boundary.outside_root",d.costs[static_cast<unsigned>(M::Cost::Boundary)]);
    append("setter.certification",d.costs[static_cast<unsigned>(M::Cost::Setter)]);
    append("Current.self_same_sample",d.currentSelf);
    static constexpr const char* parts[]={"Current.try_binding","Current.context_GetDevice","Current.PSGetShader","Current.fallback_Read","Current.remember_returned"};
    for(unsigned i=0;i<d.parts.size();++i)append(parts[i],d.parts[i]);
    LOG_INFO("R898 PS cache window: state=COMPLETE thread={} epoch={} begin_ms={} end_ms={} frames={} metadata_hit={} metadata_miss={} fallback_Read={} Current_hit={} Current_miss={} Late_hit={} Late_miss={} Late_observed_reuse={}"
        " Current_context_GetDevice={} Family_Read_shader_GetDevice={} Current_PSGetShader={} Late_PSGetShader={} setter_PSGetShader={}"
        " setter_entered={} setter_skipped={} setter_getter_mismatch={} setter_access_busy={} setter_proof_changed={} setter_accepted={}"
        " refresh_unknown_alias={} refresh_publish_dispatch_changed={} refresh_before_attach={} refresh_role_PSSetShader={} refresh_role_ClearState={} refresh_role_ExecuteCommandList={} refresh_role_FinishCommandList={} refresh_role_SwapDeviceContextState={}"
        " pub_ReusedCoverage={} pub_ReplacedCoverage={} pub_PendingRefresh={} pub_Refused={} last_refresh_token={} last_publication_id={}"
        " nested_cost_suppressed={} excluded_observations={} timing_state={} qpc_frequency={} stride=64 costs={}"
        " scope=complete_frames_on_this_owner_thread timing_overhead_included=true direct_children_same_root_sample=true current_self=root_minus_disjoint_direct_children Read_is_inclusive_subset=true inclusive_not_additive=true setter_cost=certification_not_entire_observer no_net_saving_claim=true owner_lock_held=false no_performance_claim=true",
        v[C::ThreadId],v[C::TimingEpoch],v[C::BeginMs],v[C::EndMs],v[C::Frames],v[C::MetadataHits],v[C::MetadataMisses],v[C::FallbackReads],
        d.hits[0],d.misses[0],d.hits[1],d.misses[1],d.lateObservedReuse,
        d.api[0],d.api[1],d.api[2],d.api[3],d.api[4],d.setter[0],d.setter[1],d.setter[2],d.setter[3],d.setter[4],d.setter[5],
        d.refresh[0],d.refresh[1],d.refresh[2],d.refreshRoles[0],d.refreshRoles[1],d.refreshRoles[2],d.refreshRoles[3],d.refreshRoles[4],
        d.publication[0],d.publication[1],d.publication[2],d.publication[3],d.refreshToken,d.publicationId,d.nestedSuppressed,d.excluded,
        v[C::QpcFrequency]?"AVAILABLE":"UNKNOWN",v[C::QpcFrequency],costs);
}

static void PublishHookTiming(const ControlsSnapshot& controls)noexcept
{
    try {
        GenericDepthDx11::EmitPsCacheInstallReceipt(); // before nativeMutex, no installer IO
        // One snapshot at this existing frame boundary. Off/bridge-disabled draws
        // remain measurable without enabling capture, SR, NR or FG.
        GenshinHookCpuTiming::OnPresent(controls.hookCpuTiming,controls.Active(),
            [](DWORD tid,const char* name,const char* path,const GenshinHookCpuTiming::Values& v,
               int64_t frequency,unsigned deferred,uint64_t refusedThreads,bool on) {
                using namespace GenshinHookCpuTiming;
                const auto us=[&](Field field){return frequency>0?double(v[field])*1000000.0/double(frequency):0.0;};
                const auto avg=[&](Field field,uint64_t n){return n?us(field)/double(n):0.0;};
                LOG_INFO("R448 Hook CPU: pid={} thread={} type={} path={} enabled={} calls={} samples={} sampleRatio={} hook_stride=64 event_wait_stride=1 blocking_events_full=true route_snapshot=controls.Active_at_entry total_wall_us={} total_avg_wall_us={} total_sampled_max_wall_us={} original_calls={} original_samples={} original_wall_us={} original_avg_wall_us={} original_sampled_max_wall_us={} wrapper_wall_us={} wrapper_avg_wall_us={} wrapper_sampled_max_wall_us={} before_wall_us={} before_avg_wall_us={} before_sampled_max_wall_us={} after_wall_us={} after_avg_wall_us={} after_sampled_max_wall_us={} nested_calls={} early_refused={} no_original={} result_ok={} result_pending={} result_fail={} last_result={:X} sampled_ge100us={} sampled_ge1ms={} sampled_ge16ms={} clock_failed={} deferred_rows={} refused_threads={} thread_limit=32 cumulative=true calls_published_lower_bound=true unpublished_tail_max_per_thread_bucket=63 wall_not_cpu_busy=true not_gpu_time=true inclusive_not_additive=true timing_overhead_included=true",
                    GetCurrentProcessId(),tid,name,path,on,v[Calls],v[Samples],v[Calls]?double(v[Samples])/double(v[Calls]):0.0,
                    us(Total),avg(Total,v[Samples]),us(TotalMax),v[OriginalCalls],v[OriginalSamples],us(Original),avg(Original,v[OriginalSamples]),us(OriginalMax),
                    us(Wrapper),avg(Wrapper,v[Samples]),us(WrapperMax),us(Before),avg(Before,v[Samples]),us(BeforeMax),
                    us(After),avg(After,v[Samples]),us(AfterMax),v[Nested],v[Refused],v[NoOriginal],v[ResultOk],v[ResultPending],v[ResultFail],v[LastResult],
                    v[SampleOver100us],v[SampleOver1000us],v[SampleOver16000us],v[ClockFailed],deferred,refusedThreads);
            });
        PublishShaderCacheWindow();
    }catch(...){} // Diagnostics never revoke/authorize GPU work or change Present.
}
void OnPresent(ID3D11Device* device)noexcept
{
    if(!game || internal)return;
    GenshinCpuWriterProbe::OnPresent();
    ObserveCameraSample();
    GenshinReturnedRaster::Presented(device);
    try{const auto controls=Config::Instance()->AcquireGenshinControls();
        PublishHookTiming(controls);
        // Setup can succeed before a genuine lazy import changes at runtime.
        // Drain that original immutable refusal through the existing once-only
        // observer at Present, outside nativeMutex and all collector locks.
        // No image reopen, authentication retry or query-state promotion.
        GenshinNativeQuery::detail::NotifyFirstRevocation();
        GenshinSubstage::EnablePresetReceipt(controls.hookCpuTiming);
        PublishPresetReceipt();
        GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);
        ObserveCaptureControls(controls);
        // R606: the existing locked owner boundary closes only this thread's
        // complete Shader observation frame; cache/GPU admission is unchanged.
        shaderCreationOwner.PresentShaderCacheMetrics(nativeContext.Get(),device,nativeDeviceEpoch,
            controls.epoch,nativeStatus.ownerEpoch,
            device==nativeDevice.Get() && nativePipeline && nativeContext && GetCurrentThreadId()==nativeThread &&
            controls.Active() && shaderHookInstalled && nativeStatus.entryInstalled && !nativePipeline->NeedsDrain() &&
            nativeControls.epoch==controls.epoch && nativeControls.settings==controls.settings &&
            Config::Instance()->GenshinControlsCurrent(controls.epoch));
        {struct ObserverGuard{ObserverGuard(){internal=true;}~ObserverGuard(){internal=false;}}guard;
            PollManualCapture(device);}
        if(device==nativeDevice.Get() && RouteActive(controls))TryInstallEntry();
        if(device==nativeDevice.Get() && nativePipeline){
            // Present may be the only boundary after settings change; frozen epochs cannot refresh authority.
            if(!RouteActive(controls) || controls.epoch!=nativeControls.epoch || controls.settings!=nativeControls.settings)
                nativePipeline->RevokeTransport();
            if(nativePipeline->NeedsDrain() && GetCurrentThreadId()==nativeThread && DrainNativePipeline()){
                nativeControls=controls;nativeStatus.srAccepted=false;
                nativeStatus.reason=RouteActive(controls)?CaptureReason::NoRenderToken:CaptureReason::Disabled;}
        }
        if(device==nativeDevice.Get() && nativePipeline){
            const bool accepted=nativeStatus.srAccepted && controls.Active() && controls.epoch==nativeControls.epoch;
            const bool disabled=nativeStatus.reason==CaptureReason::Disabled;
            if(GetCurrentThreadId()==nativeThread && RouteActive(controls) &&
               nativeControls.epoch==controls.epoch && nativeControls.settings==controls.settings &&
               Config::Instance()->GenshinControlsCurrent(controls.epoch)) {
                struct ContextStateGuard {
                    bool prior=internal;
                    ContextStateGuard(){internal=true;}
                    ~ContextStateGuard(){internal=prior;}
                } guard;
                nativePipeline->TryInitializeContextState();
            }
            nativePipeline->Poll();nativeStatus=nativePipeline->GetStatus();nativeStatus.srAccepted=nativeStatus.srAccepted && accepted;
            if(disabled)nativeStatus.reason=CaptureReason::Disabled;}
        NoteControls(controls);
        if(device==nativeDevice.Get() && nativePipeline && GetCurrentThreadId()==nativeThread &&
            !OwnerObservationCurrent(nativeContext.Get(),controls.epoch))
            RefreshOwnerObservation(nativeContext.Get(),controls);
        // R206: only this real owner boundary may consume the optional request.
        // Config is acquired before nativeMutex; the capability cannot escape.
        // A missing first owner waits; changed/stopped owners are terminal.
        if(PsState::optionalInstall.Phase()==PsState::OptionalInstallPhase::PendingPublication ||
           (PsState::optionalInstall.Phase()==PsState::OptionalInstallPhase::Ready && PsState::PublicRefreshRequested())){
            // Keep the barrier through request/COM destruction, not just callback.
            struct InstallReentryGuard {
                bool prior=internal;
                InstallReentryGuard(){internal=true;}
                ~InstallReentryGuard(){internal=prior;}
            } reentry;
            auto pending=PsState::optionalInstall.Pending();
            if(pending){
                const bool changed=pending->owner &&
                    (pending->owner!=nativePipeline.get() || pending->ownerEpoch!=nativeStatus.ownerEpoch || pending->context.Get()!=nativeContext.Get() ||
                     pending->device.Get()!=nativeDevice.Get() || pending->deviceEpoch!=nativeDeviceEpoch ||
                     pending->threadId!=nativeThread);
                const bool closed=!RouteActive(controls) || !Config::Instance()->GenshinControlsCurrent(controls.epoch);
                if(closed || changed){
                    if(PsState::optionalInstall.Cancel())
                        LOG_INFO("R206 optional PS binding cache: state=terminal deferred_attempt=false owner_closed={} owner_changed={} no_performance_claim=true",closed,changed);
                }else if(device==nativeDevice.Get() && nativePipeline && nativeContext && nativeDeviceEpoch &&
                    GetCurrentThreadId()==nativeThread && !nativePipeline->NeedsDrain() && shaderHookInstalled &&
                    nativeStatus.entryInstalled && nativeControls.epoch==controls.epoch && nativeControls.settings==controls.settings){
                    // The epoch is only an ABA discriminator from this live, locked pipeline.
                    // Authority remains the actual pointers/owner thread/controls above.
                    PsState::InstallOwnerScope owner(nativePipeline.get(),&shaderCreationOwner,nativeContext.Get(),nativeDevice.Get(),
                        nativeStatus.ownerEpoch,nativeDeviceEpoch,controls.epoch,+[](uint64_t epoch) noexcept {
                            return Config::Instance()->GenshinControlsCurrent(epoch);
                        });
                    if(auto consume=PsState::optionalOwnerConsumer.load(std::memory_order_acquire))consume(owner);
                }
            }
        }
        // Sparse diagnostics from the real producer owner, independent of optional disk capture.
        // One sample per second records the first concrete refusal and frame counters.
        static uint64_t nextStatusTick=0;const auto statusTick=GetTickCount64();
        if(device==nativeDevice.Get() && statusTick>=nextStatusTick){nextStatusTick=statusTick+1000;
            LOG_INFO("Genshin internal FSR: pid={} epoch={} hook={} shader_hook={} render_calls={} draws={} target_shader_draws={} binding_rejected={} binding_stage={} last_shader={:X} candidates={} token_matches={} native_returned={} submitted={} copyback={} reason={} native_reason={} copyback_counter_scope=selected_game_rtv_ordered final_backbuffer_consumption=UNKNOWN visual=UNKNOWN",
                GetCurrentProcessId(),nativeDeviceEpoch,nativeStatus.entryInstalled,shaderHookInstalled,
                il2cpp_callsite::render_call_count(),nativeStatus.observedDraws,nativeStatus.targetShaderDraws,nativeStatus.bindingRejected,
                nativeStatus.bindingRefusal,nativeStatus.shaderHash,nativeStatus.candidates,nativeStatus.tokenMatches,
                nativeStatus.nativeReturns,nativeStatus.submitted,nativeStatus.outputCopies,ReasonText(nativeStatus.reason),
                NativeMrt::ReasonText(static_cast<NativeMrt::Reason>(nativeStatus.nativeRefusal)));
            LOG_INFO("Genshin FSR capture boundary: pid={} device_epoch={} device={:X} context={:X} owner_thread={} stage={} context_state_hr={:08X} three_element={} pre_shader_blocked={} shader_lookups={} known={} unknown={} shader_read={} shader_object={:X} shader_device={:X} record_epoch={} record_hr={:08X} family_cpu_cached={} family_cpu_generation={} creates={} wrong_device_creates={} recorded={} record_failed={} before_hook_publication={} accumulate_creates={} first_create_tick={} last_create_tick={} last_create_hash={:016X} last_create_bytes={} observation_only=true",
                GetCurrentProcessId(),nativeDeviceEpoch,reinterpret_cast<uintptr_t>(nativeDevice.Get()),reinterpret_cast<uintptr_t>(nativeContext.Get()),nativeThread,
                static_cast<unsigned>(nativeStatus.captureStage),static_cast<unsigned>(nativeStatus.contextStateResult),nativeStatus.threeElementDraws,nativeStatus.preShaderBlocked,
                nativeStatus.shaderLookups,nativeStatus.knownShaderDraws,nativeStatus.unknownShaderDraws,static_cast<unsigned>(nativeStatus.shaderObservation.state),
                nativeStatus.shaderObservation.shader,nativeStatus.shaderObservation.actualDevice,nativeStatus.shaderObservation.recordedEpoch,
                static_cast<unsigned>(nativeStatus.shaderObservation.privateDataResult),nativeStatus.shaderObservation.metadataCached,
                nativeStatus.shaderObservation.metadataGeneration,shaderCreations.callbacks,shaderCreations.wrongDevice,shaderCreations.recorded,
                shaderCreations.failed,shaderCreations.beforePublication,shaderCreations.accumulate,shaderCreations.firstTick,shaderCreations.lastTick,shaderCreations.lastHash,shaderCreations.lastBytes);}
        CapturePipeline::CollectRetired();GenshinFsr::Dx11Owner::CollectRetired();}
    catch(...){}
}
HRESULT BeforeResize(IDXGISwapChain* swapchain)noexcept
{
    PsState::RetireOwner(&shaderCreationOwner);
    if(!game)return S_OK;
    GenshinCpuWriterProbe::Revoke(nullptr);
    try
    {
        const auto captureControls=CaptureControlsAtBoundary();GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);
        if(captureControls)ObserveCaptureControls(*captureControls);
        ComPtr<ID3D11Device> device;if(!swapchain || FAILED(swapchain->GetDevice(IID_PPV_ARGS(&device))) || device.Get()!=nativeDevice.Get())return S_OK;
        GenshinDepthProvider::RevokePreview();
        GenshinReturnedRaster::Invalidate(nullptr,nullptr,GenshinReturnedRaster::Boundary::Owner);
        StopManualCapture(ManualCapture::Reason::ContextBoundary);
        if(!nativePipeline){ResourceProof::Invalidate();return S_OK;}
        if(!nativePipeline->Invalidate()){nativeStatus=nativePipeline->GetStatus();return DXGI_ERROR_WAS_STILL_DRAWING;}
        nativePipeline.reset();nativeContext.Reset();nativeThread=0;RetirePendingRenderTokens();ResourceProof::Invalidate();
        nativeWorkOwned.store(false,std::memory_order_release);
        nativeStatus.srAccepted=false;nativeStatus.reason=CaptureReason::Draining;NoteControls(nativeControls);
        return S_OK;
    }
    catch(...){return DXGI_ERROR_WAS_STILL_DRAWING;}
}
void Shutdown()noexcept
{
    PsState::ForgetCoverage();
    GenshinDepthProvider::RevokePreview();
    GenshinCpuWriterProbe::Revoke(nullptr);
    GenshinReturnedRaster::Invalidate(nullptr,nullptr,GenshinReturnedRaster::Boundary::Owner);
    try{GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);
        uiHookPhase.store(Ui::HookPhase::Unknown, std::memory_order_release);
        StopManualCapture(ManualCapture::Reason::Shutdown);
        RuntimeCapture::Stop();nativePipeline.reset();nativeWorkOwned.store(false,std::memory_order_release);
        nativeObservationActive.store(true,std::memory_order_release);if(game)il2cpp_callsite::shutdown();game=false;shaderCreationOwner.Reset();nativeDevice.Reset();
        nativeContext.Reset();nativeThread=0;nativeDeviceEpoch=0;shaderHookInstalled=false;shaderCreations={};nativeControls={};nativeStatus={};attempts=0;nextAttempt={};
        cameraInstallBase=0;cameraInstallConfig={};cameraInstallAttempts=0;
        cameraInstalled=false;cameraInstallTerminal=true;nextCameraAttempt={};
        ResourceProof::Configure(nullptr,0,false);
        captureSignalProvider=nullptr;capturePresentProvider=nullptr;captureProviderOpaque=nullptr;captureOnlyMode=CaptureOnlyMode::Disabled;
        PublishManualState();}
    catch(...){}
}
GenshinTransport::Observation GetTransportObservation()noexcept
{
    try{GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);return nativePipeline?nativePipeline->GetTransportObservation():GenshinTransport::Observation{};}
    catch(...){return {};}
}
std::uint64_t Ui::ProcessStartIdentity() noexcept
{
    static const auto value = []() -> std::uint64_t {
        FILETIME creation {}, exit {}, kernel {}, user {};
        if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return 0;
        return (std::uint64_t(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
    }();
    return value;
}
Ui::Observation Ui::ReadObservation() noexcept
{
    try
    {
        const auto revision = uiRevision.load(std::memory_order_acquire);
        if (revision & 1) return {};
        Ui::Observation out;
        out.phase = uiHookPhase.load(std::memory_order_acquire);
        out.attempt = uiHookAttempt.load(std::memory_order_acquire);
        out.hookStartedTick = uiHookStartedTick.load(std::memory_order_relaxed);
        out.hookFinishedTick = uiHookFinishedTick.load(std::memory_order_relaxed);
        out.session = { GetCurrentProcessId(), ProcessStartIdentity(),
                        uiDeviceEpoch.load(std::memory_order_relaxed), uiControlsEpoch.load(std::memory_order_relaxed) };
        out.enabled = true;
        if (out.phase != Ui::HookPhase::Initializing)
        {
            std::unique_lock lock(nativeMutex, std::try_to_lock);
            if (!lock.owns_lock() || !game || !nativeDevice) return {};
            out.failure = nativeStatus.reason == CaptureReason::EntryNotMatched ? Ui::HookFailure::EntryNotMatched :
                out.phase == Ui::HookPhase::Failed ? Ui::HookFailure::InstallFailed : Ui::HookFailure::None;
            out.session.deviceEpoch = nativeDeviceEpoch;
            out.session.controlsEpoch = nativeStatus.controlsEpoch;
            out.enabled = nativeControls.Active() && nativeStatus.requestedEnabled &&
                nativeStatus.reason != CaptureReason::Disabled;
            if (!il2cpp_callsite::try_read_ui_render_observation(out.render)) return {};
        }
        if (revision != uiRevision.load(std::memory_order_acquire) ||
            out.phase != uiHookPhase.load(std::memory_order_acquire) ||
            out.attempt != uiHookAttempt.load(std::memory_order_acquire)) return {};
        const auto runtime = GenshinLoading::Read();
        if (runtime.known && runtime.authority == GenshinLoading::Authority::RuntimeOwner)
        {
            if (runtime.session != out.session) return {};
            // R63's actual runtime attempt is authoritative, including resize,
            // control/NR generations. Never expose uiHookAttempt as another
            // generation to a store that already has native owner publications.
            out.attempt = runtime.attempt;
            const auto& hook = runtime.stages[GenshinLoading::Index(GenshinLoading::Stage::Hooking)];
            out.hookStartedTick = hook.startedTick; out.hookFinishedTick = hook.finishedTick;
            switch (hook.state)
            {
            case GenshinLoading::State::Running: out.phase = Ui::HookPhase::Initializing; break;
            case GenshinLoading::State::Complete: out.phase = Ui::HookPhase::Installed; break;
            case GenshinLoading::State::Failed:
                out.phase = Ui::HookPhase::Failed;
                out.failure = hook.reason == GenshinLoading::Reason::HookNotMatched ? Ui::HookFailure::EntryNotMatched :
                    Ui::HookFailure::InstallFailed;
                break;
            default: return {};
            }
        }
        out.observedTick = GetTickCount64();
        return out;
    }
    catch (...) { return {}; }
}
Status GetStatus()noexcept{try{GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);return nativeStatus;}catch(...){return {};}}
Engine::Receipt GetEngineProducerReceipt()noexcept{try{GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);return nativePipeline?nativePipeline->GetEngineProducerReceipt():Engine::Receipt{};}catch(...){return {};}}
bool SetCaptureOnlyProviders(CaptureSignalProvider signalProvider,CapturePresentProvider presentProvider,void* opaque,CaptureOnlyMode mode) noexcept
{
    try{const auto captureControls=CaptureControlsAtBoundary();GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);
        if(captureControls)ObserveCaptureControls(*captureControls);
        if(mode!=CaptureOnlyMode::Disabled && mode!=CaptureOnlyMode::ObservationOnly)return false;
        if(nativePipeline && (GetCurrentThreadId()!=nativeThread || !DrainNativePipeline()))return false;
        captureSignalProvider=signalProvider;capturePresentProvider=presentProvider;captureProviderOpaque=opaque;captureOnlyMode=mode;
        captureMinimumGeneration=il2cpp_callsite::params_generation();
        return true;}
    catch(...){return false;}
}
NoSrConsumer::Receipt GetCaptureOnlyReceipt() noexcept
{
    try{GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);return nativePipeline?nativePipeline->GetCaptureReceipt():NoSrConsumer::Receipt{};}
    catch(...){return {};}
}
std::shared_ptr<const CaptureSnapshot> GetCaptureOnlySnapshot() noexcept
{
    try{GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);return nativePipeline?nativePipeline->GetCaptureSnapshot():nullptr;}
    catch(...){return {};}
}
std::shared_ptr<const RawObservation::Snapshot> RawObservation::GetSnapshot() noexcept
{
    try{const auto controls=Config::Instance()->AcquireGenshinControls();GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);
        ObserveCaptureControls(controls);
        if(!nativePipeline)return {};
        if(!RouteActive(controls) || controls.epoch!=nativeControls.epoch || controls.settings!=nativeControls.settings)
            nativePipeline->RevokeTransport();
        return nativePipeline->GetRawObservationSnapshot();}catch(...){return {};}
}
bool RawObservation::Snapshot::Current() const noexcept
{const auto current=RawObservation::GetSnapshot();return current.get()==this;}
RawObservation::Receipt RawObservation::GetReceipt() noexcept
{
    try{const auto controls=Config::Instance()->AcquireGenshinControls();GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);
        ObserveCaptureControls(controls);
        if(!nativePipeline)return {};
        if(!RouteActive(controls) || controls.epoch!=nativeControls.epoch || controls.settings!=nativeControls.settings)
            nativePipeline->RevokeTransport();
        return nativePipeline->GetRawObservationReceipt();}
    catch(...){return {};}
}
std::uint64_t ResolveNativeSwapchainIdentity(IDXGISwapChain* swapchain) noexcept
{
    if(!game || !swapchain)return 0;
    ComPtr<IUnknown> identity;
    return SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&identity)))?reinterpret_cast<uintptr_t>(identity.Get()):0;
}
Presentation::Ticket BeginNativePresent(ID3D11Device* device,std::uint64_t swapchain,UINT flags,bool present1) noexcept
{
    if(!game || (flags & DXGI_PRESENT_TEST))return {};
    try{const auto captureControls=CaptureControlsAtBoundary();GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);
        if(captureControls)ObserveCaptureControls(*captureControls);
        if(device!=nativeDevice.Get() || !nativePipeline || GetCurrentThreadId()!=nativeThread)return {};
        return nativePipeline->BeginPresent(swapchain,flags,present1);}
    catch(...){return {};}
}
void CompleteNativePresent(const Presentation::Ticket& ticket,HRESULT result) noexcept
{
    if(!game || !ticket.Known())return;
    try{const auto captureControls=CaptureControlsAtBoundary();GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);
        if(captureControls)ObserveCaptureControls(*captureControls);
        if(nativePipeline && ticket.owner.device==reinterpret_cast<uintptr_t>(nativeDevice.Get()) &&
            ticket.owner.deviceEpoch==nativeDeviceEpoch && GetCurrentThreadId()==nativeThread)
            nativePipeline->CompletePresent(ticket,result);}
    catch(...){}
}
void ForgetNativeSwapchain(std::uint64_t swapchain) noexcept
{
    if(!game || !swapchain)return;
    try{const auto captureControls=CaptureControlsAtBoundary();GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);
        if(captureControls)ObserveCaptureControls(*captureControls);
        if(nativePipeline){
        nativePipeline->ForgetSwapchain(swapchain);
        if(GetCurrentThreadId()==nativeThread && nativePipeline->GetStatus().reason==CaptureReason::Draining)DrainNativePipeline();}}
    catch(...){}
}
Presentation::Receipt GetNativePresentReceipt() noexcept
{
    try{GenshinHookCpuTiming::Scope r448OwnerWait(GenshinHookCpuTiming::Stage::OwnerLock);
        std::lock_guard lock(nativeMutex);r448OwnerWait.Finish();
        GenshinHookCpuTiming::Scope r448OwnerHeld(GenshinHookCpuTiming::Stage::OwnerLocked);return nativePipeline?nativePipeline->GetPresentReceipt():Presentation::Receipt{};}
    catch(...){return {};}
}
}
