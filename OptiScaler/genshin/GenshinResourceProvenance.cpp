// SPDX-License-Identifier: GPL-3.0-or-later
#include "GenshinResourceProvenance.h"
#include <wrl/client.h>
#include <atomic>
#include <mutex>
namespace GenshinLegacy::ResourceProof
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr GUID Key{0x97537632,0x72ca,0x45fb,{0x8c,0x6a,0x2c,0x43,0xd5,0x14,0xb6,0x3d}};
constexpr std::uint64_t Magic=0x5232303257524954ull;
struct Record {std::uint64_t magic=Magic,lifetime=0,deviceEpoch=0;Witness write{};};
constexpr GUID MapKey{0x97537633,0x72ca,0x45fb,{0x8c,0x6a,0x2c,0x43,0xd5,0x14,0xb6,0x3d}};
struct Mapping {std::uint64_t context=0,thread=0,epoch=0;UINT subresource=0;D3D11_MAP type{};};
struct MapRecord {std::uint64_t magic=Magic,lifetime=0,deviceEpoch=0;std::array<Mapping,8> entries{};};
std::mutex mutex;
ComPtr<ID3D11Device> owner;
std::uint64_t deviceEpoch=0,observationEpoch=1,nextLifetime=1,nextSerial=1;
std::atomic<bool> enabled=false,hooked=false;
bool OwnContext(ID3D11DeviceContext* context) noexcept
{
    if(!context || !enabled || !hooked)return false;
    ComPtr<ID3D11Device> device;context->GetDevice(&device);
    return device.Get()==owner.Get();
}
Record Touch(ID3D11DeviceChild* child) noexcept
{
    if(!child)return {};
    ComPtr<ID3D11Device> device;child->GetDevice(&device);if(device.Get()!=owner.Get())return {};
    Record record;UINT size=sizeof(record);
    if(SUCCEEDED(child->GetPrivateData(Key,&size,&record)) && size==sizeof(record) && record.magic==Magic && record.deviceEpoch==deviceEpoch && record.lifetime)return record;
    record={};record.lifetime=nextLifetime++;record.deviceEpoch=deviceEpoch;
    if(FAILED(child->SetPrivateData(Key,sizeof(record),&record)))return {};
    return record;
}
Witness Read(ID3D11DeviceContext* context,ID3D11Resource* resource,ID3D11View* view,bool validatedContext=false) noexcept
{
    Witness result;
    if(!validatedContext){if(!OwnContext(context)){result.reason=Reason::WrongOwner;return result;}
        if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE){result.reason=Reason::Deferred;return result;}}
    auto record=Touch(resource);if(!record.lifetime){result.reason=Reason::UnknownAlias;return result;}
    result=record.write;result.resourceLifetime=record.lifetime;result.deviceEpoch=deviceEpoch;
    result.observationEpoch=observationEpoch;
    if(view){ComPtr<ID3D11Resource> viewed;view->GetResource(&viewed);
        // GetResource returns the same resource interface for the usual alias: avoid a second
        // GetDevice/GetPrivateData round trip while retaining lifetime checks for other aliases.
        auto referenced=viewed.Get()==resource?record:Touch(viewed.Get());
        if(referenced.lifetime!=record.lifetime){result.reason=Reason::UnknownAlias;result.observedMutationCall=false;return result;}
        auto v=Touch(view);result.viewLifetime=v.lifetime;if(!v.lifetime)result.reason=Reason::UnknownAlias;}
    ComPtr<ID3D11Texture2D> texture;
    if(FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))){result.reason=Reason::UnknownAlias;result.observedMutationCall=false;return result;}
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    result.subresourceKnown=desc.ArraySize==1 && desc.MipLevels==1 && desc.SampleDesc.Count==1 &&
        (!record.write.lastObservedSerial || record.write.subresource==0);
    if(!result.subresourceKnown){result.reason=Reason::UnknownAlias;result.observedMutationCall=false;}
    else if(record.write.lastObservedSerial && record.write.observationEpoch!=observationEpoch){result.reason=Reason::Invalidated;result.observedMutationCall=false;}
    MapRecord maps;UINT size=sizeof(maps);
    if(SUCCEEDED(resource->GetPrivateData(MapKey,&size,&maps)) && size==sizeof(maps) && maps.magic==Magic &&
        maps.lifetime==record.lifetime && maps.deviceEpoch==deviceEpoch){
        for(const auto& entry:maps.entries)if(entry.context && entry.epoch==observationEpoch && entry.type!=D3D11_MAP_READ){
            result.reason=Reason::MappedWrite;result.observedMutationCall=false;break;}}
    return result;
}
bool LoadMappings(ID3D11Resource* resource,MapRecord& maps) noexcept
{
    const auto record=Touch(resource);if(!record.lifetime)return false;
    UINT size=sizeof(maps);
    if(FAILED(resource->GetPrivateData(MapKey,&size,&maps)) || size!=sizeof(maps) || maps.magic!=Magic ||
        maps.lifetime!=record.lifetime || maps.deviceEpoch!=deviceEpoch){maps={};maps.lifetime=record.lifetime;maps.deviceEpoch=deviceEpoch;}
    for(auto& entry:maps.entries)if(entry.context && entry.epoch!=observationEpoch)entry={};
    return true;
}
bool ValidMapSubresource(ID3D11Resource* resource,UINT subresource) noexcept
{
    D3D11_RESOURCE_DIMENSION dimension{};resource->GetType(&dimension);
    if(dimension==D3D11_RESOURCE_DIMENSION_BUFFER)return subresource==0;
    ComPtr<ID3D11Texture2D> texture;
    if(FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))))return false;
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    return desc.MipLevels && desc.ArraySize && static_cast<std::uint64_t>(subresource)<static_cast<std::uint64_t>(desc.MipLevels)*desc.ArraySize;
}
void RecordWrite(ID3D11DeviceContext* context,ID3D11Resource* resource,Kind kind,UINT subresource,bool whole,UINT mapType=0) noexcept
{
    auto record=Touch(resource);if(!record.lifetime)return;
    Witness write;write.resourceLifetime=record.lifetime;write.deviceEpoch=deviceEpoch;write.observationEpoch=observationEpoch;
    write.lastObservedSerial=nextSerial++;write.lastObservedEpoch=observationEpoch;write.writerContext=reinterpret_cast<uintptr_t>(context);write.writerThread=GetCurrentThreadId();
    write.kind=kind;write.subresource=subresource;write.wholeResource=whole;write.mapType=mapType;
    write.observedMutationCall=kind!=Kind::DrawPotential && kind!=Kind::DispatchPotential && kind!=Kind::None;
    write.reason=Reason::Observed;record.write=write;
    if(FAILED(resource->SetPrivateData(Key,sizeof(record),&record)))++observationEpoch;
}
}
void Configure(ID3D11Device* device,std::uint64_t epoch,bool active) noexcept
{
    try{std::lock_guard lock(mutex);if(owner.Get()!=device || deviceEpoch!=epoch || enabled!=active){++observationEpoch;owner=device;deviceEpoch=epoch;}
        enabled=active && device && epoch;if(!enabled)owner.Reset();}catch(...){enabled=false;}
}
void HooksInstalled(bool value) noexcept
{try{std::lock_guard lock(mutex);hooked=value;if(!value)++observationEpoch;}catch(...){hooked=false;enabled=false;}}
bool Active() noexcept{return enabled && hooked;}
void Invalidate() noexcept{try{std::lock_guard lock(mutex);++observationEpoch;}catch(...){enabled=false;}}
void InvalidateContext(ID3D11DeviceContext* context) noexcept
{try{std::lock_guard lock(mutex);if(OwnContext(context))++observationEpoch;}catch(...){enabled=false;}}
void NoteWrite(ID3D11DeviceContext* context,ID3D11Resource* resource,Kind kind,UINT subresource,bool whole) noexcept
{
    if(!Active() || !resource)return;
    try{std::lock_guard lock(mutex);if(!OwnContext(context))return;
        if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE){++observationEpoch;return;}
        RecordWrite(context,resource,kind,subresource,whole);
    }catch(...){enabled=false;}
}
void NoteViewWrite(ID3D11DeviceContext* context,ID3D11View* view,Kind kind,bool whole) noexcept
{
    if(!Active() || !view)return;
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    // First supported source contract is one mip/slice; all other aliases stay unknown.
    NoteWrite(context,resource.Get(),kind,0,whole);
}
void AfterMap(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT subresource,D3D11_MAP type,HRESULT result) noexcept
{
    if(!Active() || !resource || FAILED(result))return;
    try{std::lock_guard lock(mutex);if(!OwnContext(context))return;
        if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE){++observationEpoch;return;}
        MapRecord maps;if(!LoadMappings(resource,maps))return;
        if(!ValidMapSubresource(resource,subresource)){++observationEpoch;return;}
        // Success is paired even for READ so its Unmap cannot fabricate a write.
        if(type<D3D11_MAP_READ || type>D3D11_MAP_WRITE_NO_OVERWRITE){++observationEpoch;return;}
        Mapping* free=nullptr;
        for(auto& entry:maps.entries){if(entry.context && entry.subresource==subresource){++observationEpoch;return;}if(!entry.context && !free)free=&entry;}
        if(!free){++observationEpoch;return;}
        *free={reinterpret_cast<uintptr_t>(context),GetCurrentThreadId(),observationEpoch,subresource,type};
        if(FAILED(resource->SetPrivateData(MapKey,sizeof(maps),&maps)))++observationEpoch;
    }catch(...){enabled=false;}
}
void AfterUnmap(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT subresource) noexcept
{
    if(!Active() || !resource)return;
    try{std::lock_guard lock(mutex);if(!OwnContext(context))return;
        if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE){++observationEpoch;return;}
        MapRecord maps;if(!LoadMappings(resource,maps))return;
        for(auto& entry:maps.entries)if(entry.context && entry.subresource==subresource){
            const auto paired=entry;entry={};
            if(FAILED(resource->SetPrivateData(MapKey,sizeof(maps),&maps))){++observationEpoch;return;}
            if(paired.context!=reinterpret_cast<uintptr_t>(context) || paired.thread!=GetCurrentThreadId() || paired.epoch!=observationEpoch){++observationEpoch;return;}
            if(paired.type!=D3D11_MAP_READ)RecordWrite(context,resource,Kind::WriteUnmap,subresource,false,paired.type);
            return;
        }
        // Includes unobserved/failed/overflow/old-epoch pairing; no definite writer is invented.
        ++observationEpoch;
    }catch(...){enabled=false;}
}
void AfterDiscard(ID3D11DeviceContext* context,ID3D11Resource* resource) noexcept
{
    if(!Active() || !resource)return;
    try{std::lock_guard lock(mutex);if(!OwnContext(context))return;
        if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE){++observationEpoch;return;}
        auto record=Touch(resource);if(!record.lifetime)return;
        // Every discard request conservatively retires old witnesses, including partial ranges.
        // Reuse the single validated lifetime record; no second owner/private-data query is needed.
        ++observationEpoch;record.write.reason=Reason::Discarded;record.write.observedMutationCall=false;
        record.write.observationEpoch=observationEpoch;record.write.wholeResource=false;
        record.write.lastWriteProven=record.write.completeMutationCoverage=false;
        MapRecord maps;maps.lifetime=record.lifetime;maps.deviceEpoch=deviceEpoch;
        const auto writeResult=resource->SetPrivateData(Key,sizeof(record),&record);
        const auto mapResult=resource->SetPrivateData(MapKey,sizeof(maps),&maps);
        if(FAILED(writeResult) || FAILED(mapResult))++observationEpoch;
    }catch(...){enabled=false;}
}
void AfterDiscardView(ID3D11DeviceContext* context,ID3D11View* view) noexcept
{
    if(!Active() || !view)return;
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);AfterDiscard(context,resource.Get());
}
bool CurrentIdentityPair(const Witness& a,const Witness& b) noexcept
{
    if(!Active())return false;
    try{std::lock_guard lock(mutex);if(!enabled || !hooked)return false;
        const auto current=[](const Witness& witness){return witness.resourceLifetime && witness.subresourceKnown &&
            witness.deviceEpoch==deviceEpoch && witness.observationEpoch==observationEpoch &&
            witness.reason!=Reason::Discarded && witness.reason!=Reason::Invalidated && witness.reason!=Reason::MappedWrite &&
            witness.reason!=Reason::UnknownAlias && witness.reason!=Reason::WrongOwner && witness.reason!=Reason::Deferred && witness.reason!=Reason::Disabled;};
        // One locked epoch/gate snapshot for both identities; this grants no content/write authority.
        return current(a) && current(b);
    }catch(...){return false;}
}
void AfterGraphics(ID3D11DeviceContext* context) noexcept
{
    if(!Active() || !context)return;
    if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE){InvalidateContext(context);return;}
    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};ID3D11DepthStencilView* depth=nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,targets,&depth);
    for(auto target:targets)if(target){NoteViewWrite(context,target,Kind::DrawPotential,false);target->Release();}
    if(depth){NoteViewWrite(context,depth,Kind::DrawPotential,false);depth->Release();}
}
void AfterCompute(ID3D11DeviceContext* context) noexcept
{
    if(!Active() || !context)return;
    if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE){InvalidateContext(context);return;}
    ID3D11UnorderedAccessView* views[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
    context->CSGetUnorderedAccessViews(0,D3D11_PS_CS_UAV_REGISTER_COUNT,views);
    for(auto view:views)if(view){NoteViewWrite(context,view,Kind::DispatchPotential,false);view->Release();}
}
std::array<Witness,3> Snapshot(ID3D11DeviceContext* context,const std::array<ID3D11Texture2D*,3>& resources,
                             const std::array<ID3D11ShaderResourceView*,3>& views) noexcept
{
    std::array<Witness,3> result{};if(!Active())return result;
    try{std::lock_guard lock(mutex);
        // One owner/type check under the same lock replaces three identical COM queries.
        if(!OwnContext(context)){for(auto& witness:result)witness.reason=Reason::WrongOwner;return result;}
        if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE){for(auto& witness:result)witness.reason=Reason::Deferred;return result;}
        for(unsigned i=0;i<3;++i)result[i]=Read(context,resources[i],views[i],true);
    }catch(...){}
    return result;
}
Witness Identify(ID3D11DeviceContext* context,ID3D11Resource* resource,ID3D11View* view) noexcept
{
    if(!Active())return {};
    try{std::lock_guard lock(mutex);return Read(context,resource,view);}catch(...){return {};}
}
}
