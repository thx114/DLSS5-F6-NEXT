// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <cstdint>
#include <wrl/client.h>

namespace GenshinDepthMapping {
// Written by the real hook after its internal-call guard. Replays cannot
// replace these arguments. Indirect observation never grants draw replay.
struct DrawArguments {
    ID3D11DeviceContext* context=nullptr;
    DWORD thread=0;UINT kind=0,count=0,start=0,instances=0,instanceStart=0;INT base=0;
    uint64_t observationCall=0; // diagnostic same-invocation key, never a frame/token
    Microsoft::WRL::ComPtr<ID3D11Buffer> indirectBuffer;
    Microsoft::WRL::ComPtr<IUnknown> indirectIdentity;
    UINT indirectOffset=0;
    bool indirectArgumentsUnknown=false; // scalar zeros are placeholders, not decoded arguments
    bool Matches(ID3D11DeviceContext* c,UINT k,UINT n)const noexcept {
        return context==c && thread==GetCurrentThreadId() && kind==k && count==n && instances==1 &&
            count && (kind==1 || kind==2 || kind==3 || kind==4);
    }
    void Issue()const noexcept {
        if(kind==1)context->Draw(count,start);
        else if(kind==2)context->DrawIndexed(count,start,base);
        else if(kind==3)context->DrawInstanced(count,instances,start,instanceStart);
        else if(kind==4)context->DrawIndexedInstanced(count,instances,start,base,instanceStart);
    }
};
struct Mutation {ID3D11DeviceContext* context=nullptr;uint64_t sequence=0;};
inline Mutation& Mutations(){static thread_local Mutation state;return state;}
inline uint64_t MutationSequence(ID3D11DeviceContext* context)noexcept {
    auto& s=Mutations();if(s.context!=context){s.context=context;++s.sequence;}return s.sequence;
}
inline void ObserveNonRasterWrite(ID3D11DeviceContext* context)noexcept {
    MutationSequence(context);++Mutations().sequence;
}
inline DrawArguments& CurrentDraw(){static thread_local DrawArguments draw;return draw;}
// Release only this completed original call. A reentrant newer observation
// remains intact; captured Draw/Raster owners retain their own references.
inline void FinishDrawObservation(ID3D11DeviceContext* context,uint64_t call)noexcept {
    auto& draw=CurrentDraw();
    if(call && draw.context==context && draw.thread==GetCurrentThreadId() &&
       draw.kind==6 && draw.observationCall==call)draw={};
}
class DrawObservationScope final {
    ID3D11DeviceContext* context_;uint64_t call_;
public:
    DrawObservationScope(ID3D11DeviceContext* context,uint64_t call)noexcept:context_(context),call_(call){}
    ~DrawObservationScope(){FinishDrawObservation(context_,call_);}
    DrawObservationScope(const DrawObservationScope&)=delete;
    DrawObservationScope& operator=(const DrawObservationScope&)=delete;
};
inline void ObserveDrawArguments(ID3D11DeviceContext* context,UINT kind,UINT count,UINT start,
                                INT base,UINT instances,UINT instanceStart,
                                ID3D11Buffer* indirectBuffer=nullptr,UINT indirectOffset=0) noexcept {
    CurrentDraw()={context,GetCurrentThreadId(),kind,count,start,instances,instanceStart,base};
    if(kind==6){
        auto& draw=CurrentDraw();
        draw.count=draw.start=draw.instances=draw.instanceStart=0;draw.base=0;
        draw.indirectArgumentsUnknown=true;draw.indirectBuffer=indirectBuffer;draw.indirectOffset=indirectOffset;
        if(indirectBuffer)indirectBuffer->QueryInterface(IID_PPV_ARGS(&draw.indirectIdentity));
    }
    static thread_local uint64_t call=0;
    if(call!=UINT64_MAX)CurrentDraw().observationCall=++call;
}
}
