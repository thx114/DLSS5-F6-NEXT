// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinNativeQueryProvenance.h"
#include <Windows.h>
#include <Psapi.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

// Installation diagnostics are opt-in at the D3D11 factory installer. Keep
// their OS mapping queries out of the shared native authentication header.
namespace GenshinNativeQuery::detail {
// R041 diagnostics only. Sample the pre-existing execution edge BEFORE our
// commit overwrites it. No observer result is consumed by native admission,
// normalization, patch indexing, trust, COM ownership, epochs or query birth.
namespace PreexistingBranchObservation {
struct Mapping {
    uintptr_t allocation=0,region=0;size_t size=0;
    DWORD state=0,type=0,protect=0;bool queried=false;
};
struct Node {
    uintptr_t address=0,slot=0,target=0;Mapping mapping{},slotMapping{};
    std::array<unsigned char,16> bytes{};unsigned byteCount=0;
    std::array<wchar_t,260> mappedPath{};DWORD pathLength=0,pathError=0;
    const char* edge="none";bool slotRead=false;
};
struct Chain {
    std::array<Node,4> nodes{};unsigned count=0;
    DWORD pid=0,thread=0;uint64_t tick=0;
    const char* stop="not-captured";bool stableAtEnd=false;
};
inline Mapping Query(uintptr_t address)noexcept {
    MEMORY_BASIC_INFORMATION m{};Mapping r{};
    r.queried=address && VirtualQuery(reinterpret_cast<const void*>(address),&m,sizeof(m))==sizeof(m);
    if(r.queried){r.allocation=reinterpret_cast<uintptr_t>(m.AllocationBase);
        r.region=reinterpret_cast<uintptr_t>(m.BaseAddress);r.size=m.RegionSize;
        r.state=m.State;r.type=m.Type;r.protect=m.Protect;}
    return r;
}
inline bool Readable(const Mapping& m,uintptr_t address,size_t size)noexcept {
    const auto p=m.protect&0xff;
    return m.queried && m.state==MEM_COMMIT && !(m.protect&(PAGE_GUARD|PAGE_NOACCESS)) &&
        (p==PAGE_READONLY || p==PAGE_READWRITE || p==PAGE_WRITECOPY ||
         p==PAGE_EXECUTE_READ || p==PAGE_EXECUTE_READWRITE || p==PAGE_EXECUTE_WRITECOPY) &&
        address>=m.region && address-m.region<=m.size && size<=m.size-(address-m.region);
}
inline bool ExecutableMapping(const Mapping& m)noexcept {
    const auto p=m.protect&0xff;
    return p==PAGE_EXECUTE || p==PAGE_EXECUTE_READ || p==PAGE_EXECUTE_READWRITE || p==PAGE_EXECUTE_WRITECOPY;
}
inline bool SameMapping(const Mapping& a,const Mapping& b)noexcept {
    return a.queried==b.queried && a.allocation==b.allocation && a.region==b.region &&
        a.size==b.size && a.state==b.state && a.type==b.type && a.protect==b.protect;
}
// Checked integer arithmetic only: never form a C++ pointer outside its object.
inline bool Relative(uintptr_t address,unsigned instructionBytes,int64_t displacement,uintptr_t& target)noexcept {
    if(address>UINTPTR_MAX-instructionBytes)return false;
    const auto next=address+instructionBytes;
    if(displacement<0){const auto distance=uint64_t(-(displacement+1))+1;
        if(distance>next)return false;target=next-static_cast<uintptr_t>(distance);
    }else {if(uint64_t(displacement)>UINTPTR_MAX-next)return false;
        target=next+static_cast<uintptr_t>(displacement);}
    return target!=0;
}
inline bool Unchanged(const Chain& chain)noexcept {
    if(!chain.count)return false;
    for(unsigned i=0;i<chain.count;++i){const auto& n=chain.nodes[i];
        if(!n.byteCount || !SameMapping(n.mapping,Query(n.address)) ||
           !Equal(reinterpret_cast<const void*>(n.address),n.bytes.data(),n.byteCount))return false;
        if(n.slot){uintptr_t value=0;
            if(!n.slotRead || !SameMapping(n.slotMapping,Query(n.slot)) ||
               !Memory(reinterpret_cast<const void*>(n.slot),&value,sizeof(value)) || value!=n.target)return false;}
    }return true;
}
inline Chain Capture(const void* entry)noexcept {
    const auto error=GetLastError();Chain r{};
    r.pid=GetCurrentProcessId();r.thread=GetCurrentThreadId();r.tick=GetTickCount64();
    auto address=reinterpret_cast<uintptr_t>(entry);r.stop="null-entry";
    while(address && r.count<r.nodes.size()){
        auto& n=r.nodes[r.count++];n.address=address;n.mapping=Query(address);
        if(!Readable(n.mapping,address,1)){r.stop="unreadable-code";break;}
        n.byteCount=static_cast<unsigned>((std::min)(n.bytes.size(),n.mapping.size-(address-n.mapping.region)));
        if(!Memory(reinterpret_cast<const void*>(address),n.bytes.data(),n.byteCount)){
            n.byteCount=0;r.stop="code-read-failed";break;}
        // Memory-manager mapping name captured now; no loader enumeration,
        // LoadLibrary, file IO, stack walk or late module attribution.
        if(n.mapping.type==MEM_IMAGE){
            n.pathLength=K32GetMappedFileNameW(GetCurrentProcess(),reinterpret_cast<void*>(address),
                n.mappedPath.data(),static_cast<DWORD>(n.mappedPath.size()));
            if(!n.pathLength)n.pathError=GetLastError();
            n.mappedPath.back()=0;
        }
        if(!ExecutableMapping(n.mapping)){r.stop="non-executable-code";break;}
        unsigned instruction=0;int64_t displacement=0;
        if(n.bytes[0]==0xe9){n.edge="E9-rel32";instruction=5;
            if(n.byteCount>=instruction)displacement=Read<int32_t>(n.bytes.data()+1);
        }else if(n.bytes[0]==0xeb){n.edge="EB-rel8";instruction=2;
            if(n.byteCount>=instruction)displacement=static_cast<int8_t>(n.bytes[1]);
        }else if(n.byteCount>=2 && n.bytes[0]==0xff && n.bytes[1]==0x25){
            n.edge="FF25-rip-slot";instruction=6;
            if(n.byteCount>=instruction)displacement=Read<int32_t>(n.bytes.data()+2);
        }else {r.stop="not-supported-unconditional-edge";break;}
        if(n.byteCount<instruction){r.stop="short-instruction";break;}
        uintptr_t next=0;
        if(!Relative(address,instruction,displacement,next)){r.stop="edge-address-overflow";break;}
        if(instruction==6){n.slot=next;n.slotMapping=Query(n.slot);
            n.slotRead=Readable(n.slotMapping,n.slot,sizeof(n.target)) &&
                Memory(reinterpret_cast<const void*>(n.slot),&n.target,sizeof(n.target));
            if(!n.slotRead){r.stop="unreadable-indirect-slot";break;}
        }else n.target=next;
        if(!n.target){r.stop="null-edge-target";break;}
        bool cycle=false;for(unsigned i=0;i<r.count;++i)cycle=cycle || r.nodes[i].address==n.target;
        if(cycle){r.stop="edge-cycle";break;}
        address=n.target;r.stop="node-budget";
    }
    // A bounded end reread is only an observation, not an atomic execution proof.
    r.stableAtEnd=Unchanged(r);SetLastError(error);return r;
}
} // namespace PreexistingBranchObservation

// Product composition is deliberately separate from the diagnostic observer.
// It grants terminal-owner authority, never historical installer attribution.
namespace FactoryComposition {
inline bool Symbol(const char* name)noexcept {
    return name && (!std::strcmp(name,"D3D11CreateDevice") ||
        !std::strcmp(name,"D3D11CreateDeviceAndSwapChain") || !std::strcmp(name,"D3D11On12CreateDevice"));
}
inline FactoryPredecessor::Mapping Freeze(const PreexistingBranchObservation::Mapping& m)noexcept {
    return {m.allocation,m.region,m.size,m.state,m.type,m.protect};
}
inline FactoryPredecessor Capture(HMODULE module,const char* symbol,void* entry)noexcept {
    FactoryPredecessor r{};
    if(!module || !entry || !Symbol(symbol))return r;
    const auto chain=PreexistingBranchObservation::Capture(entry);
    if(chain.count!=3 || !chain.stableAtEnd)return r;
    const auto& first=chain.nodes[0];const auto& relay=chain.nodes[1];const auto& last=chain.nodes[2];
    if(first.byteCount!=16 || first.mapping.type!=MEM_IMAGE || first.mapping.protect!=PAGE_EXECUTE_READ ||
       first.mapping.allocation!=reinterpret_cast<uintptr_t>(module) ||
       first.bytes[0]!=0xe9 || first.target!=relay.address || relay.byteCount!=16 || relay.mapping.type!=MEM_PRIVATE ||
       relay.bytes[0]!=0xff || relay.bytes[1]!=0x25 || !relay.slotRead || relay.slot!=relay.address+6 ||
       relay.target!=last.address || last.byteCount!=16 || last.mapping.type!=MEM_IMAGE || last.target ||
       relay.slotMapping.allocation!=relay.mapping.allocation ||
       !PreexistingBranchObservation::Readable(relay.slotMapping,relay.slot,8))return r;
    auto owner=HoYoShade::AcquireExistingFactoryOwner(reinterpret_cast<void*>(last.address),symbol);
    if(!owner)return r;
    if(!PreexistingBranchObservation::ExecutableMapping(first.mapping) ||
       !PreexistingBranchObservation::ExecutableMapping(relay.mapping) ||
       !PreexistingBranchObservation::ExecutableMapping(last.mapping))return r;
    for(unsigned i=0;i<r.nodes.size();++i){r.nodes[i].address=chain.nodes[i].address;
        r.nodes[i].mapping=Freeze(chain.nodes[i].mapping);r.nodes[i].bytes=chain.nodes[i].bytes;}
    r.slot=relay.slot;r.slotMapping=Freeze(relay.slotMapping);r.terminal=last.address;
    r.owner=std::move(owner);r.module=module;r.symbol=symbol;
    r.thread=GetCurrentThreadId();r.generation=store.generation.load();r.captured=true;
    if(!FactoryPredecessorIntact(r))return {};
    return r;
}
} // namespace FactoryComposition
} // namespace GenshinNativeQuery::detail
