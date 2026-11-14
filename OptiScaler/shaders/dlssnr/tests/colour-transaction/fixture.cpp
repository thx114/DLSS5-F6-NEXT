// SPDX-License-Identifier: GPL-3.0-or-later
// Actual production branches, synthetic recording failures. GPU pixels and
// ownership are tested separately by NrPresentationOutputWarp/lifetime suites.
#include <cassert>
#include <cstdio>
#include <initializer_list>
using D3D12_RESOURCE_STATES = int;
enum { D3D12_RESOURCE_STATE_PRESENT=0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS=1,
       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE=2, D3D12_RESOURCE_STATE_COPY_SOURCE=3,
       D3D12_RESOURCE_STATE_COPY_DEST=4 };
struct Texture {
    int value=99, state=D3D12_RESOURCE_STATE_UNORDERED_ACCESS, format=1;
    struct Desc {int Format;};
    Desc GetDesc()const{return {format};}
};
struct EncodeParams {unsigned Passthrough=1;operator int()const{return 0;}};
struct Commands { void CopyResource(Texture* to,Texture* from) {to->value=from->value;} } commands;
struct Device { unsigned releases=0;void Release(){++releases;} } deviceState;
struct Timer { unsigned ends=0;void End(Commands*){++ends;} } timer;
struct Nr { bool reset=false;Texture *colorCopy=nullptr,*hdrCopy=nullptr,*colorSmall=nullptr; } g_nr;
Timer* g_gpuTime=&timer;
bool canRecord=true;unsigned skips=0,continuations=0;
namespace DetailHistory { unsigned invalidations=0; void Invalidate(){++invalidations;} }
void ReportSkipOnce(const char*){++skips;}
void Barrier(Commands*,Texture* tex,int before,int after) {assert(tex->state==before);tex->state=after;}
bool DispatchPass(Commands*,int,Texture* source,Texture*,Texture*,Texture*,Texture*,Texture* target,Texture* keep) {
    if(!canRecord) return false;
    target->value=source->value;if(keep)keep->value=source->value;return true;
}
void RunEncode(bool success,bool readOnlyInput,bool share) {
    Texture raw,output,proxy,hdr;raw.state=D3D12_RESOURCE_STATE_PRESENT;
    raw.value=output.value=42;
    auto* cmdList=&commands;auto* device=&deviceState;auto* target=readOnlyInput?&raw:&output;
    struct {int InputArrivalState=D3D12_RESOURCE_STATE_PRESENT;} frame;
    unsigned finishCalls=0;
    auto TransitionTarget=[&](int state){target->state=state;};
    auto FinishColor=[&](bool copy){assert(!copy);++finishCalls;target->state=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;};
    const EncodeParams encodeParams;Texture* exposureTex=nullptr;
    const bool targetSupportsUav=share;
    canRecord=success;g_nr={false,&proxy,&hdr,nullptr};continuations=0;skips=0;
    deviceState.releases=timer.ends=0;
    const auto execute=[&]{
#include "colour-original-routing.inl"
        assert(shareEncodedOriginal==share && encodedOriginal==(share?&proxy:&hdr));
        (void)TransitionKeptOriginal;
        target->state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
#include "colour-encode.inl"
        ++continuations;
    };
    execute();
    assert(continuations==unsigned(success));
    assert(g_nr.reset==!success && deviceState.releases==unsigned(!success) && timer.ends==unsigned(!success));
    assert(proxy.value==(success?42:99) && output.value==42);
    assert(hdr.value==(success && !share?42:99));
    if(!success)assert(target->state==(readOnlyInput?D3D12_RESOURCE_STATE_PRESENT:D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    assert(finishCalls==unsigned(!success && !readOnlyInput));
}
void RunResize(bool success,bool share) {
    Texture proxy,hdr,small;proxy.state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    proxy.value=42;
    hdr.state=share?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto* cmdList=&commands;auto* device=&deviceState;auto* modelInput=&proxy;int down=0;
    unsigned finishCalls=0;auto FinishColor=[&](bool copy){assert(!copy);++finishCalls;};
    canRecord=success;g_nr={false,&proxy,&hdr,&small};continuations=0;
    const EncodeParams encodeParams;const bool targetSupportsUav=share;
#include "colour-original-routing.inl"
    (void)encodedOriginal;
    const auto execute=[&]{
#include "colour-resize.inl"
        ++continuations;
    };
    execute();assert(continuations==unsigned(success) && small.value==(success?42:99));
    assert(finishCalls==unsigned(!success));
    if(!success)assert(proxy.state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS && hdr.state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
void Routing() {
    Texture proxy,hdr;auto* cmdList=&commands;g_nr={false,&proxy,&hdr,nullptr};
    for(unsigned passthrough:{0u,1u})for(bool targetSupportsUav:{false,true})for(int format:{1,2}) {
        hdr.format=format;const EncodeParams encodeParams{passthrough};
#include "colour-original-routing.inl"
        const bool share=passthrough && targetSupportsUav && format==proxy.format;
        assert(shareEncodedOriginal==share && encodedOriginal==(share?&proxy:&hdr));
        proxy.state=hdr.state=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        TransitionKeptOriginal(D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        assert(proxy.state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        assert(hdr.state==(share?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        TransitionKeptOriginal(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
}
void RunResolve(bool success,bool targetSupportsUav) {
    Texture raw,hdr,proxy,answer;
    proxy.value=42;
    raw.state=targetSupportsUav?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto* cmdList=&commands;auto* target=&raw;int targetState=raw.state;
    auto TransitionTarget=[&](int state){raw.state=targetState=state;};
    auto* resolveTarget=targetSupportsUav?&raw:&hdr;auto* resolveOriginal=targetSupportsUav?&hdr:&raw;
    auto* resolveProxy=&proxy;auto* resolveAnswer=&answer;Texture *motionIn=nullptr,*exposureTex=nullptr;
    int resolveParams=0;bool composed=false;canRecord=success;g_nr={false,nullptr,&hdr,nullptr};
    DetailHistory::invalidations=0;
#include "colour-resolve.inl"
    assert(composed==success && raw.value==(success?42:99));
    assert(g_nr.reset==!success && DetailHistory::invalidations==unsigned(!success));
    if(!targetSupportsUav)assert(hdr.state==D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}
void RunEncodeRecovery(bool readOnlyInput,bool share) {
    // Keep the same physical scratch objects through a recorded frame, a
    // failed encode and a different source. A constant stub value would hide
    // accidental reuse of the last successfully encoded picture.
    Texture raw,output,proxy,hdr;raw.state=D3D12_RESOURCE_STATE_PRESENT;
    auto* cmdList=&commands;auto* device=&deviceState;auto* target=readOnlyInput?&raw:&output;
    struct {int InputArrivalState=D3D12_RESOURCE_STATE_PRESENT;} frame;
    unsigned finishCalls=0;
    auto TransitionTarget=[&](int state){target->state=state;};
    auto FinishColor=[&](bool copy){assert(!copy);++finishCalls;target->state=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;};
    const EncodeParams encodeParams;Texture* exposureTex=nullptr;const bool targetSupportsUav=share;
    g_nr={false,&proxy,&hdr,nullptr};continuations=0;skips=0;deviceState.releases=timer.ends=0;
#include "colour-original-routing.inl"
    for(unsigned n=0;n<3;++n) {
        target->value=100+int(n);canRecord=n!=1;
        const auto execute=[&]{
            TransitionTarget(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
#include "colour-encode.inl"
            ++continuations;
            assert(proxy.value==target->value && encodedOriginal->value==target->value);
            Barrier(cmdList,&proxy,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            TransitionKeptOriginal(D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmdList,&proxy,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            TransitionKeptOriginal(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        };
        execute();
        assert(proxy.state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS && hdr.state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        assert(proxy.value==(n==2?102:100) && hdr.value==(share?99:(n==2?102:100)));
        if(n==1)assert(target->state==(readOnlyInput?D3D12_RESOURCE_STATE_PRESENT:D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    }
    assert(continuations==2 && skips==1 && deviceState.releases==1 && timer.ends==1);
    assert(finishCalls==unsigned(!readOnlyInput));
}
int main() {
    Routing();
    for(bool success:{false,true}) {
        for(bool share:{false,true}) {
            RunEncode(success,true,share);RunEncode(success,false,share);RunResize(success,share);
        }
        RunResolve(success,false);RunResolve(success,true);
    }
    for(bool readOnlyInput:{false,true})for(bool share:{false,true})RunEncodeRecovery(readOnlyInput,share);
    std::puts("NR colour transaction: 8 routing + 16 success/failure + 4 same-resource recovery cases passed; synthetic failures, no SDK/game proof");
}
