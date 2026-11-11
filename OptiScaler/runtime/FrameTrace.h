// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <chrono>
#include <cstdint>

// Observations never retain resources, authorize work, or prove display.
namespace FrameTrace {
enum class Api : unsigned { Unknown, D3D11, D3D12 };
enum class Kind : unsigned {
    ConsumerTransition, EvaluateEntered, EvaluateReturned, FinalColourJoined,
    ConstantsAccepted, SdkState, SdkOptions, PhysicalPresentReturned, NrOutcome, GuideSourceObserved
};
inline constexpr const char* KindName(Kind kind) noexcept {
    constexpr const char* names[]={
        "consumer transition","Evaluate entered","Evaluate returned","final colour joined",
        "actual constants accepted","SDK state query","SDK options transaction","physical Present returned","NR outcome",
        "source guides observed in Evaluate"};
    const auto index=static_cast<unsigned>(kind);
    return index<sizeof(names)/sizeof(*names)?names[index]:"unknown";
}
inline uint64_t NowNs() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct Facts {
    Kind kind=Kind::ConsumerTransition;
    Api api=Api::Unknown;
    uint64_t evaluateOrdinal=0,apiFeature=0,epoch=0,feature=0,configuration=0,owner=0;
    uint64_t context=0,colour=0,depth=0,motion=0,queue=0,fenceValue=0;
    uint64_t token=0,querySequence=0,queryBeginTick=0,queryEndTick=0;
    uint32_t presentedInterval=0,sdkStatus=0;
    float scaleX=0,scaleY=0,jitterX=0,jitterY=0;
    bool constantsKnown=false,depthInverted=false,reset=false;
    int nrResult=-1,nrReason=-1;
    bool tokenKnown=false,nrOutcomeKnown=false,queryIntervalKnown=false;
    bool pixelPhaseVerified=false,generatedIdentityKnown=false,displayIdentityKnown=false;
};
struct SourceRate {
    Api api=Api::Unknown;
    uint64_t feature=0,firstNs=0,lastNs=0,samples=0;
    double hz=0;
    bool known=false;
};
// Counts actual SR Evaluate entries, including failed/skipped attempts.
// Observation ordinals deliberately do not impersonate game/SDK frame tokens.
class SourceCadence {
    SourceRate window_{},published_{};
public:
    void Reset() noexcept {window_={};published_={};}
    void Observe(Api api,uint64_t feature,uint64_t tickNs) noexcept {
        if(api==Api::Unknown||!feature||!tickNs)return;
        const bool different=window_.api!=api||window_.feature!=feature;
        const bool gap=window_.lastNs&&(tickNs<window_.lastNs||tickNs-window_.lastNs>3000000000ull);
        if(different||gap){window_={};published_={};}
        if(!window_.samples){window_.api=api;window_.feature=feature;window_.firstNs=tickNs;}
        window_.lastNs=tickNs;++window_.samples;
        const auto elapsed=tickNs-window_.firstNs;
        if(window_.samples>1&&elapsed){
            window_.hz=static_cast<double>(window_.samples-1)*1e9/static_cast<double>(elapsed);
            window_.known=true;
        }
        if(elapsed>=2000000000ull){
            published_=window_;
            window_={api,feature,tickNs,tickNs,1,0,false};
        }
    }
    SourceRate Read(uint64_t nowNs=NowNs()) const noexcept {
        auto result=published_.known?published_:window_;
        if(!result.lastNs||nowNs<result.lastNs||nowNs-result.lastNs>3000000000ull)result.known=false;
        return result;
    }
};
}
