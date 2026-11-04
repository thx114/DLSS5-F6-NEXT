// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <d3d11.h>
#include "legacy/Fsr2FamilyTakeover.h"
#include "GenshinNrProfile.h"
#include "GenshinShaderCacheMetrics.h"

namespace GenshinLegacy::Family
{
inline uint64_t Hash(const void* data,size_t size)noexcept
{
    if(!data || !size)return 0;
    uint64_t hash=14695981039346656037ull;
    const auto* bytes=static_cast<const uint8_t*>(data);
    for(size_t i=0;i<size;++i){hash^=bytes[i];hash*=1099511628211ull;}
    return hash;
}
struct Shader
{
    uint64_t hash=0,bytes=0,deviceEpoch=0,identity=0;
    std::shared_ptr<const GenshinNr::ShaderProfile> nrProfile;
    bool Known()const noexcept{return hash && bytes && deviceEpoch && identity;}
};
// Stored on the actual COM child: destruction removes the record, so a reused address has no old entry.
inline constexpr GUID ShaderKey={0x6e2a1a99,0xf537,0x4ba4,{0x89,0x18,0x12,0x4b,0x2b,0x47,0xb3,0x01}};
// Strict same-device entry for callers without a successful creation-return witness.
bool Record(ID3D11Device* device,ID3D11PixelShader* shader,const void* bytes,size_t size,uint64_t deviceEpoch)noexcept;
// One initialized creator/epoch, serialized by the capture owner's nativeMutex.
// This is a returned-object relation, not an arbitrary COM unwrap or a device alias map.
// Both endpoints have bounded owner lifetime; Reset revokes the weak publication.
// Shader private data contains only values, never its own COM reference.
struct CreationBinding;
class CreationOwner final
{
    std::shared_ptr<CreationBinding> binding_;
public:
    CreationOwner()=default;
    ~CreationOwner();
    CreationOwner(const CreationOwner&)=delete;
    CreationOwner& operator=(const CreationOwner&)=delete;
    bool Bind(ID3D11Device* creator,uint64_t epoch)noexcept;
    void Reset()noexcept;
    // Existing locked capture Present only, after HookCpuTiming publishes its
    // control epoch. current proves the live capture owner/device/thread/config;
    // it does not authorize a cache hit or GPU work. First/changed frames baseline.
    void PresentShaderCacheMetrics(ID3D11DeviceContext* context,ID3D11Device* device,
        uint64_t deviceEpoch,uint64_t configEpoch,uint64_t ownerEpoch,bool current)const noexcept;
    // Only the successful CreatePixelShader return callback supplies this evidence.
    // Different callback creators or returned ownership domains cannot join this owner.
    bool RecordReturned(ID3D11Device* creator,ID3D11PixelShader* shader,
                        const void* bytes,size_t size)noexcept;
};
// Producer-internal observations at the already executed PS-cache branches.
// Fixed TLS only on ordinary draws; Revoke is an exceptional lifecycle event.
namespace ShaderCacheMetrics {
uint64_t Draw(const void* owner,ID3D11DeviceContext* context,ID3D11Device* device,
    uint64_t deviceEpoch,uint64_t configEpoch)noexcept;
void BindingResult(uint64_t frame,Consumer consumer,bool hit,unsigned availability)noexcept;
void Controls(uint64_t configEpoch,bool enabled)noexcept;
void Revoke()noexcept;
}
Shader Read(ID3D11Device* device,ID3D11PixelShader* shader,uint64_t deviceEpoch)noexcept;
// Actual bound-object observations only: never synthesize missing bytecode.
enum class ReadState : uint32_t { NotInspected, Ready, MissingOwner, WrongContextDevice,
    NoPixelShader, WrongShaderDevice, MissingRecord, InvalidRecord, WrongEpoch, ProfileUnavailable };
struct ReadObservation
{
    ReadState state=ReadState::NotInspected;
    uintptr_t shader=0,actualDevice=0;
    uint64_t recordedEpoch=0;
    HRESULT privateDataResult=S_OK;
    // On a CPU cache hit, the fields above describe the last successful full
    // validation, not fresh GetDevice/GetPrivateData calls for this draw.
    bool metadataCached=false;
    uint64_t metadataGeneration=0;
};
// The first successful full Read of our StoreReturned record locks its CPU
// classification to the strongly held shader/device/owner identities. There
// is no frame/time expiry or getter revalidation on a hit. Mark attempts
// invalidate their slot; Reset/destruction revoke the whole owner. The 2048
// direct-mapped slots can evict on collision/capacity, so residency is bounded,
// not permanent. Unknown/contended/reentrant reads keep the original getters
// and checks without expiring an otherwise resident fact. External writes
// to our private GUID are outside this observable project-mark lifecycle;
// neither such mutations nor GPU ownership are certified by this cache.
// The legacy entry point delegates here with no observer.
Shader Read(ID3D11Device* device,ID3D11PixelShader* shader,uint64_t deviceEpoch,ReadObservation* observation)noexcept;
Shader Current(ID3D11DeviceContext* context,ID3D11Device* device,uint64_t deviceEpoch,
               ReadObservation* observation=nullptr)noexcept;
enum class Reason { Disabled,UnknownShader,NotPre,NoReplacement,WrongOwner,Expired,Ready,Skipped };
struct State
{
    uintptr_t device=0,context=0;
    uint64_t ownerEpoch=0,deviceEpoch=0,shaderIdentity=0,tick=0;
    bool replaced=false;
    void Reset()noexcept{*this={};}
    void Notify(const Shader& shader,uintptr_t d,uintptr_t c,uint64_t epoch,bool success,uint64_t now)noexcept
    {
        Reset();
        if(success && shader.Known() && shader.hash==fsr2_family_takeover::k_accumulate_hash && d && c && epoch && now)
        {device=d;context=c;ownerEpoch=epoch;deviceEpoch=shader.deviceEpoch;shaderIdentity=shader.identity;tick=now;replaced=true;}
    }
    Reason Admit(const Shader& shader,uintptr_t d,uintptr_t c,uint64_t epoch,uint32_t count,
                 bool enabled,uint64_t now,uint32_t expire)const noexcept
    {
        if(!enabled)return Reason::Disabled;
        if(!shader.Known())return Reason::UnknownShader;
        const auto hash=shader.hash;
        if(count!=3 || (hash!=fsr2_family_takeover::k_pre_hash_1 && hash!=fsr2_family_takeover::k_pre_hash_2 &&
            hash!=fsr2_family_takeover::k_pre_hash_3 && hash!=fsr2_family_takeover::k_pre_hash_4))return Reason::NotPre;
        if(!replaced)return Reason::NoReplacement;
        if(!epoch || device!=d || context!=c || ownerEpoch!=epoch || deviceEpoch!=shader.deviceEpoch)return Reason::WrongOwner;
        if(!tick || now<tick || now-tick>expire)return Reason::Expired;
        return Reason::Ready;
    }
};
}
