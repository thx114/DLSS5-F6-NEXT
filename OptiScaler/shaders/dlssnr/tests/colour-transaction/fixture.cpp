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
struct Texture { int value=99, state=D3D12_RESOURCE_STATE_UNORDERED_ACCESS; };
struct Commands { void CopyResource(Texture* to,Texture* from) {to->value=from->value;} } commands;
struct Device { unsigned releases=0;void Release(){++releases;} } deviceState;
struct Timer { unsigned ends=0;void End(Commands*){++ends;} } timer;
struct Nr { bool reset=false;Texture *colorCopy=nullptr,*hdrCopy=nullptr,*colorSmall=nullptr; } g_nr;
Timer* g_gpuTime=&timer;
bool canRecord=true;unsigned skips=0,continuations=0;
namespace DetailHistory { unsigned invalidations=0; void Invalidate(){++invalidations;} }
void ReportSkipOnce(const char*){++skips;}
void Barrier(Commands*,Texture* tex,int before,int after) {assert(tex->state==before);tex->state=after;}
bool DispatchPass(Commands*,int,Texture*,Texture*,Texture*,Texture*,Texture*,Texture* target,Texture* keep) {
    if(!canRecord) return false;
    target->value=42;if(keep)keep->value=42;return true;
}
void RunEncode(bool success,bool readOnlyInput) {
    Texture raw,output,proxy,hdr;raw.state=D3D12_RESOURCE_STATE_PRESENT;
    auto* cmdList=&commands;auto* device=&deviceState;auto* target=readOnlyInput?&raw:&output;
    struct {int InputArrivalState=D3D12_RESOURCE_STATE_PRESENT;} frame;
    unsigned finishCalls=0;
    auto TransitionTarget=[&](int state){target->state=state;};
    auto FinishColor=[&](bool copy){assert(!copy);++finishCalls;target->state=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;};
    const int encodeParams=0;Texture* exposureTex=nullptr;
    canRecord=success;g_nr={false,&proxy,&hdr,nullptr};continuations=0;skips=0;
    deviceState.releases=timer.ends=0;
    const auto execute=[&]{
        target->state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
#include "colour-encode.inl"
        ++continuations;
    };
    execute();
    assert(continuations==unsigned(success));
    assert(g_nr.reset==!success && deviceState.releases==unsigned(!success) && timer.ends==unsigned(!success));
    assert(proxy.value==(success?42:99) && output.value==99);
    if(!success)assert(target->state==(readOnlyInput?D3D12_RESOURCE_STATE_PRESENT:D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    assert(finishCalls==unsigned(!success && !readOnlyInput));
}
void RunResize(bool success) {
    Texture proxy,hdr,small;proxy.state=hdr.state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto* cmdList=&commands;auto* device=&deviceState;auto* modelInput=&proxy;int down=0;
    unsigned finishCalls=0;auto FinishColor=[&](bool copy){assert(!copy);++finishCalls;};
    canRecord=success;g_nr={false,&proxy,&hdr,&small};continuations=0;
    const auto execute=[&]{
#include "colour-resize.inl"
        ++continuations;
    };
    execute();assert(continuations==unsigned(success) && small.value==(success?42:99));
    assert(finishCalls==unsigned(!success));
    if(!success)assert(proxy.state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS && hdr.state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
void RunResolve(bool success,bool targetSupportsUav) {
    Texture raw,hdr,proxy,answer;
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
int main() {
    for(bool success:{false,true}) {
        RunEncode(success,true);RunEncode(success,false);
        RunResize(success);RunResolve(success,false);RunResolve(success,true);
    }
    std::puts("NR colour transaction: 10 production-branch cases passed; synthetic failures, no SDK/game proof");
}
