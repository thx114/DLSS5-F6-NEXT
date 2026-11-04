// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <d3d11.h>
#include <wrl/client.h>

namespace GenshinLegacy::VisibleOutput
{
using Microsoft::WRL::ComPtr;
// Original Bridge's paired-output repair: a consumed generation is reusable
// once within 50 ms, only for an unambiguous, already associated native output.
// Opti additionally proves the same live storage/owner and legal selected
// subresource copy. CPU queued Render params and SRV/CB pointer rotation do not
// replace canonical consumed-generation identity. No temporal history/token is advanced.
struct Pair
{
    uint64_t generation=0,canonicalGeneration=0,repairedGeneration=0,now=0,completedTick=0;
    bool associated=false,sameOwner=false,copyCompatible=false,noPendingToken=false,outputPresent=false,failed=false;
};
inline uint32_t RepairRefusal(const Pair& p) noexcept
{
    return (!p.generation || p.canonicalGeneration!=p.generation?1u:0u) |
        (p.repairedGeneration==p.generation?2u:0u) |
        (!p.completedTick || p.now<p.completedTick || p.now-p.completedTick>50?4u:0u) |
        (!p.associated?8u:0u) | (!p.sameOwner?16u:0u) | (!p.copyCompatible?32u:0u) |
        (!p.noPendingToken?64u:0u) | (!p.outputPresent?128u:0u) | (p.failed?256u:0u);
}
inline bool CanRepair(const Pair& p) noexcept {return RepairRefusal(p)==0;}
inline uint64_t Hash(const void* bytes,size_t count,uint64_t seed=14695981039346656037ull) noexcept
{
    const auto* p=static_cast<const uint8_t*>(bytes);
    for(size_t i=0;i<count;++i){seed^=p[i];seed*=1099511628211ull;}
    return seed;
}
struct Difference
{
    uint64_t instance=0,generation=0,nativeHash=0,privateHash=0,copyHash=0;
    uint32_t bytes=0,nativeDifferenceBytes=0,copyDifferenceBytes=0;
    HRESULT result=S_OK;
};
// Three small center samples per pipeline, no flush/wait, no full-frame staging.
// native != private proves only sampled value differences; private == copied
// proves only those selected-MRT bytes. Neither proves final visible consumption.
class DifferenceProbe
{
    std::array<ComPtr<ID3D11Texture2D>,3> sample_;
    ComPtr<ID3D11Query> query_;
    Difference identity_{};
    D3D11_BOX box_{};
    uint32_t width_=0,height_=0,pixelBytes_=0,attempts_=0;
    bool begun_=false,pending_=false,revoked_=false;
    void Clear() noexcept {sample_={};query_.Reset();begun_=pending_=false;}
public:
    // Logical revocation only. Owner-thread Poll fences and retires samples;
    // a foreign-thread invalidation must never release in-flight GPU resources.
    void Revoke() noexcept {revoked_=true;identity_={};}
    bool Pending() const noexcept {return begun_ || pending_;}
    bool Begin(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* native,
               UINT subresource,ID3D11Texture2D* output,UINT width,UINT height,
               DXGI_FORMAT format,uint64_t instance,uint64_t generation) noexcept
    {
        if(revoked_ || pending_ || begun_ || attempts_>=3 || !device || !context || !native || !output || !width || !height)return false;
        ++attempts_;
        pixelBytes_=format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:
            (format==DXGI_FORMAT_R11G11B10_FLOAT || format==DXGI_FORMAT_R8G8B8A8_UNORM ||
             format==DXGI_FORMAT_R10G10B10A2_UNORM)?4:0;
        if(!pixelBytes_)return false;
        width_=width<8?width:8;height_=height<8?height:8;
        D3D11_TEXTURE2D_DESC nativeDesc{},outputDesc{};native->GetDesc(&nativeDesc);output->GetDesc(&outputDesc);
        for(unsigned i=0;i<3;++i){auto desc=i==1?outputDesc:nativeDesc;
            desc.Width=width_;desc.Height=height_;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
            desc.SampleDesc.Quality=0;desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            if(FAILED(device->CreateTexture2D(&desc,nullptr,&sample_[i]))){Clear();return false;}}
        const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};
        if(FAILED(device->CreateQuery(&query,&query_))){Clear();return false;}
        const UINT x=(width-width_)/2,y=(height-height_)/2;
        box_={x,y,0,x+width_,y+height_,1};identity_={};identity_.instance=instance;identity_.generation=generation;
        context->CopySubresourceRegion(sample_[0].Get(),0,0,0,0,native,subresource,&box_);
        context->CopySubresourceRegion(sample_[1].Get(),0,0,0,0,output,0,&box_);begun_=true;return true;
    }
    void Finish(ID3D11DeviceContext* context,ID3D11Texture2D* native,UINT subresource) noexcept
    {
        if(!begun_ || !context || !native)return;
        context->CopySubresourceRegion(sample_[2].Get(),0,0,0,0,native,subresource,&box_);
        context->End(query_.Get());begun_=false;pending_=true;
    }
    bool Poll(ID3D11DeviceContext* context,Difference& result) noexcept
    {
        if(!context)return false;
        // Begin may have recorded samples without Finish. Seal that abandoned
        // window here, on its owning immediate context, without flushing/waiting.
        if(revoked_ && begun_){context->End(query_.Get());begun_=false;pending_=true;}
        if(!pending_)return false;
        BOOL done=FALSE;const auto status=context->GetData(query_.Get(),&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if(status==S_FALSE || (SUCCEEDED(status) && !done))return false;
        if(revoked_){Clear();return false;}
        result=identity_;result.result=status;
        if(FAILED(status)){Clear();return true;}
        std::array<D3D11_MAPPED_SUBRESOURCE,3> mapped{};unsigned acquired=0;
        for(;acquired<3;++acquired){result.result=context->Map(sample_[acquired].Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped[acquired]);
            if(FAILED(result.result))break;}
        if(acquired==3){result.nativeHash=result.privateHash=result.copyHash=14695981039346656037ull;
            const size_t rowBytes=static_cast<size_t>(width_)*pixelBytes_;
            for(UINT y=0;y<height_;++y){
                std::array<const uint8_t*,3> row{};
                for(unsigned i=0;i<3;++i)row[i]=static_cast<const uint8_t*>(mapped[i].pData)+static_cast<size_t>(y)*mapped[i].RowPitch;
                result.nativeHash=Hash(row[0],rowBytes,result.nativeHash);result.privateHash=Hash(row[1],rowBytes,result.privateHash);
                result.copyHash=Hash(row[2],rowBytes,result.copyHash);
                for(size_t x=0;x<rowBytes;++x){result.nativeDifferenceBytes+=row[0][x]!=row[1][x];result.copyDifferenceBytes+=row[2][x]!=row[1][x];}
            }
            result.bytes=width_*height_*pixelBytes_;
        }
        for(unsigned i=0;i<acquired;++i)context->Unmap(sample_[i].Get(),0);
        Clear();return true;
    }
};
struct BindingEvent
{
    uint64_t instance=0,generation=0,step=0;
    uint32_t directSrvs=0,relatedSrvs=0,directRtvs=0,relatedRtvs=0;
    std::array<uintptr_t,8> outputs{};
};
// Bounded binding provenance, not a shader dataflow proof. In particular a bound
// SRV can be unused by a shader; a bound RTV can have a zero write mask. Copies,
// compute and untracked descendants remain UNKNOWN. Never suppress from this.
class BindingProbe
{
    std::array<ComPtr<IUnknown>,8> resources_;
    uint64_t instance_=0,generation_=0,step_=0;
    unsigned count_=0,windows_=0,budget_=0,logBudget_=24;
    static ComPtr<IUnknown> Identity(ID3D11View* view) noexcept
    {
        ComPtr<ID3D11Resource> resource;ComPtr<IUnknown> identity;
        if(view){view->GetResource(&resource);if(resource)resource.As(&identity);}return identity;
    }
    unsigned Match(IUnknown* value)const noexcept
    {if(!value)return 0;for(unsigned i=0;i<count_;++i)if(resources_[i].Get()==value)return i+1;return 0;}
public:
    // Retain lifetime-wide window/log budgets; only the stale provenance clears.
    void Revoke() noexcept {resources_={};count_=0;instance_=generation_=step_=0;budget_=0;}
    void Close() noexcept {resources_={};count_=budget_=0;}
    void Begin(ID3D11Resource* target,uint64_t instance,uint64_t generation) noexcept
    {
        if(windows_>=3 || !target)return;
        resources_={};count_=0;ComPtr<IUnknown> identity;if(FAILED(target->QueryInterface(IID_PPV_ARGS(&identity))))return;
        resources_[count_++]=identity;instance_=instance;generation_=generation;step_=0;budget_=64;++windows_;
    }
    bool ObserveReturnedDraw(ID3D11DeviceContext* context,BindingEvent& event) noexcept
    {
        if(!budget_ || !logBudget_){resources_={};count_=0;return false;}
        if(!context)return false;
        --budget_;++step_;event={};event.instance=instance_;event.generation=generation_;event.step=step_;
        std::array<ComPtr<ID3D11ShaderResourceView>,16> srvs;
        std::array<ID3D11ShaderResourceView*,16> rawSrvs{};context->PSGetShaderResources(0,16,rawSrvs.data());
        for(unsigned i=0;i<16;++i){srvs[i].Attach(rawSrvs[i]);const auto identity=Identity(srvs[i].Get());const auto match=Match(identity.Get());
            if(match==1)event.directSrvs|=1u<<i;else if(match)event.relatedSrvs|=1u<<i;}
        std::array<ComPtr<ID3D11RenderTargetView>,8> rtvs;
        std::array<ID3D11RenderTargetView*,8> rawRtvs{};context->OMGetRenderTargets(8,rawRtvs.data(),nullptr);
        std::array<ComPtr<IUnknown>,8> outputIdentities;
        for(unsigned i=0;i<8;++i){rtvs[i].Attach(rawRtvs[i]);outputIdentities[i]=Identity(rtvs[i].Get());const auto match=Match(outputIdentities[i].Get());
            event.outputs[i]=reinterpret_cast<uintptr_t>(outputIdentities[i].Get());
            if(match==1)event.directRtvs|=1u<<i;else if(match)event.relatedRtvs|=1u<<i;}
        if(!(event.directSrvs|event.relatedSrvs|event.directRtvs|event.relatedRtvs))return false;
        if(event.directSrvs || event.relatedSrvs)for(const auto& output:outputIdentities)
            if(output && !Match(output.Get()) && count_<resources_.size())resources_[count_++]=output;
        --logBudget_;return true;
    }
};
}
