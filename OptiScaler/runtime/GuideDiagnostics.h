// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "FrameTrace.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <memory>
#include <string>
#include <utility>

namespace GuideSourceMetadata { struct Descriptor; }

// Passive observations only. Nothing in this file grants work admission, queue
// ordering, resource lifetime or retirement authority.
namespace GuideDiagnostics {
inline uint64_t Now() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
enum class Consumer : unsigned { NrModel, NrHistory, Fg, Count };
// Full consumer snapshots contain strings and resource descriptors. The menu
// requests them explicitly; source/SDK numeric statistics stay independent.
inline std::atomic<bool>& ConsumerObservationDemand() {
    static std::atomic<bool> value {false}; return value;
}
inline void SetConsumerObservationDemand(bool wanted) noexcept {
    ConsumerObservationDemand().store(wanted, std::memory_order_release);
}
template<class Read> inline void ObserveConsumers(Read&& read) {
    if (ConsumerObservationDemand().load(std::memory_order_acquire)) std::forward<Read>(read)();
}
enum class Stage : unsigned { Unknown, Received, Rejected, Recorded, Submitted, Completed };
enum class Origin : unsigned { Unknown, Evaluate, RetainedFeature, ApiDefault, ActualCall };
enum class Field : unsigned {
    CreateFlags, RenderWidth, RenderHeight, DepthX, DepthY, MotionX, MotionY,
    ScaleX, ScaleY, JitterX, JitterY, DepthBounds, MotionBounds, Phase, Count
};
inline constexpr const char* FieldNames[] = {"createFlags", "renderWidth", "renderHeight",
    "depthBaseX", "depthBaseY", "motionBaseX", "motionBaseY", "motionScaleX", "motionScaleY",
    "jitterX", "jitterY", "depthBounds", "motionBounds", "phase"};
inline constexpr const char* StageName(Stage s) noexcept {
    constexpr const char* names[]={"unknown","received","rejected","recorded","submitted","GPU completed"};
    return names[static_cast<unsigned>(s)];
}
inline constexpr const char* OriginName(Origin s) noexcept {
    constexpr const char* names[]={"unknown","Evaluate","retained feature","API default","actual call"};
    return names[static_cast<unsigned>(s)];
}
struct Value { double value=0; Origin origin=Origin::Unknown; bool valid=false; };
struct Rect { uint32_t x=0,y=0,width=0,height=0; };
struct Resource {
    bool present=false; uint32_t format=0; uint64_t width=0; uint32_t height=0;
    Rect region;
};
struct Snapshot {
    uint64_t source=0,epoch=0,feature=0,configuration=0,picture=0,providerFrame=0;
    uint64_t owner=0,submission=0,historySource=0,tick=0;
    Stage stage=Stage::Unknown;
    Resource colour,depth,motion;
    // Immutable raw producer facts and accepted pre-transform mapping. These
    // never equate independent source clocks or certify final colour phase.
    std::shared_ptr<const GuideSourceMetadata::Descriptor> rawSource;
    Resource rawDepth,rawMotion;
    float rawSdkUvX=0,rawSdkUvY=0;
    bool rawMappingKnown=false;
    std::array<Value,static_cast<unsigned>(Field::Count)> fields{};
    float scaleX=0,scaleY=0,jitterX=0,jitterY=0;
    bool depthFlip=false,motionFlip=false,yNegated=false,depthInverted=false;
    bool opticalFlow=false,phaseVerified=false,accepted=false,modelSuccess=false;
    bool addressPhaseKnown=false,motionJitterKnown=false,motionJittered=false;
    bool historyAvailable=false,fusionRecorded=false,contributionKnown=false;
    float contribution=-1,coverage=-1;
    bool uiProtected=false,sdkEnabled=false,sdkStateKnown=false,generatedIdentityKnown=false;
    // Raw existing SDK calls/acknowledgements. None proves enabled interpolation
    // or identifies a generated frame; the presented count is query-interval data.
    bool sdkQueryObserved=false,sdkStatusKnown=false,sdkOptionsKnown=false,sdkOptionsCalled=false;
    int sdkQueryResult=0,sdkOptionsResult=0,sdkOptionsMode=0;
    uint32_t sdkStatus=0,sdkPresentedInterval=0;uint64_t sdkQueryTick=0;
    uint64_t sdkQuerySequence=0,sdkPreviousQueryTick=0;bool sdkIntervalKnown=false;
    uint64_t sdkQueryBeginNs=0,sdkQueryEndNs=0,sdkPreviousQueryEndNs=0;
    std::string reason="not observed",colourChoice="unknown",resetReason;
    void Set(Field f,double v,bool valid,Origin origin) {
        fields[static_cast<unsigned>(f)]={v,valid?origin:Origin::Unknown,valid};
    }
};
struct Event {
    Consumer consumer=Consumer::NrModel; Stage stage=Stage::Unknown;
    uint64_t source=0,picture=0,providerFrame=0,tick=0; std::string reason;
    bool presentReturned=false,resultKnown=false;int64_t apiResult=0;
    uint64_t sourceTick=0;
    uint64_t sequence=0,tickNs=0;
    FrameTrace::Facts facts;
};
struct Lane { Snapshot latest,completed; uint64_t observations=0; };
struct View {
    std::array<Lane,static_cast<unsigned>(Consumer::Count)> lanes;
    std::array<Event,512> events{}; uint64_t revision=0; unsigned eventCount=0;
    FrameTrace::SourceRate sourceRate;uint64_t droppedEvents=0;
};
struct Statistics {
    FrameTrace::SourceRate source;
    uint64_t querySequence=0,beginNs=0,endNs=0;
    uint32_t presented=0;
    bool intervalKnown=false,sdkSucceeded=false;
};
struct Store { std::mutex mutex; View view; unsigned eventWrite=0;uint64_t sequence=0,evaluateOrdinal=0;
    FrameTrace::SourceCadence cadence;Statistics statistics;
    std::atomic<uint64_t> dropped{0};std::atomic<bool> cadenceLost{false}; };
inline Store& Storage(){ static auto* s=new Store;return *s; }
struct EvaluationIdentity {
    FrameTrace::Api api=FrameTrace::Api::Unknown;
    uint64_t feature=0,context=0,ordinal=0;
};
inline thread_local EvaluationIdentity activeEvaluation;
inline EvaluationIdentity CurrentEvaluation() noexcept {return activeEvaluation;}
inline void AppendEvent(Store& s,Event event) {
    event.sequence=++s.sequence;if(!event.tick)event.tick=Now();
    if(!event.tickNs)event.tickNs=FrameTrace::NowNs();
    s.view.events[s.eventWrite]=std::move(event);
    s.eventWrite=(s.eventWrite+1)%static_cast<unsigned>(s.view.events.size());
    if(s.view.eventCount<s.view.events.size())++s.view.eventCount;
}
inline FrameTrace::Facts TraceFacts(const Snapshot& snapshot,FrameTrace::Kind kind) noexcept {
    FrameTrace::Facts facts;facts.kind=kind;facts.epoch=snapshot.epoch;facts.feature=snapshot.feature;
    facts.configuration=snapshot.configuration;facts.owner=snapshot.owner;
    facts.querySequence=snapshot.sdkQuerySequence;facts.queryBeginTick=snapshot.sdkPreviousQueryEndNs;
    facts.queryEndTick=snapshot.sdkQueryEndNs;facts.queryIntervalKnown=snapshot.sdkIntervalKnown;
    facts.presentedInterval=snapshot.sdkPresentedInterval;facts.sdkStatus=snapshot.sdkStatus;
    facts.pixelPhaseVerified=snapshot.phaseVerified;facts.generatedIdentityKnown=snapshot.generatedIdentityKnown;
    return facts;
}
inline void Observe(Consumer consumer,const Snapshot& snapshot,FrameTrace::Facts facts,const char* reason,
                    bool resultKnown=false,int64_t result=0) noexcept {
    try {auto& s=Storage();std::unique_lock lock(s.mutex,std::try_to_lock);
        if(!lock.owns_lock()){s.dropped.fetch_add(1,std::memory_order_relaxed);return;}
        if(facts.kind==FrameTrace::Kind::SdkState &&
           facts.querySequence>s.statistics.querySequence)
            s.statistics={ {},facts.querySequence,facts.queryBeginTick,facts.queryEndTick,
                facts.presentedInterval,facts.queryIntervalKnown,resultKnown&&result==0 };
        Event event;event.consumer=consumer;event.stage=snapshot.stage;event.source=snapshot.source;
        event.picture=snapshot.picture;event.providerFrame=snapshot.providerFrame;event.sourceTick=snapshot.tick;
        event.facts=facts;event.reason=reason?reason:"observation";event.resultKnown=resultKnown;event.apiResult=result;
        AppendEvent(s,std::move(event));++s.view.revision;
    }catch(...){}
}
inline uint64_t EvaluateEntered(FrameTrace::Api api,uint64_t feature,uint64_t context) noexcept {
    try {auto& s=Storage();std::unique_lock lock(s.mutex,std::try_to_lock);
        if(!lock.owns_lock()){s.dropped.fetch_add(1,std::memory_order_relaxed);
            s.cadenceLost.store(true,std::memory_order_relaxed);return 0;}
        Event event;event.consumer=Consumer::Fg;event.tickNs=FrameTrace::NowNs();
        event.facts.kind=FrameTrace::Kind::EvaluateEntered;event.facts.api=api;
        event.facts.feature=feature;event.facts.apiFeature=feature;
        event.facts.context=context;event.facts.evaluateOrdinal=++s.evaluateOrdinal;
        event.reason="SR API entry";
        if(s.cadenceLost.exchange(false,std::memory_order_relaxed))s.cadence.Reset();
        const auto ordinal=event.facts.evaluateOrdinal;s.cadence.Observe(api,feature,event.tickNs);
        AppendEvent(s,std::move(event));++s.view.revision;return ordinal;
    }catch(...){return 0;}
}
inline void EvaluateReturned(FrameTrace::Api api,uint64_t feature,uint64_t ordinal,int64_t result) noexcept {
    if(!ordinal)return;Snapshot snapshot;FrameTrace::Facts facts;facts.kind=FrameTrace::Kind::EvaluateReturned;
    facts.api=api;facts.feature=feature;facts.apiFeature=feature;facts.evaluateOrdinal=ordinal;
    Observe(Consumer::Fg,snapshot,facts,"SR API returned",true,result);
}
class EvaluationScope final {
    FrameTrace::Api api_;uint64_t feature_,ordinal_;bool known_=false;int64_t result_=0;
    EvaluationIdentity previous_;
public:
    EvaluationScope(FrameTrace::Api api,uint64_t feature,uint64_t context,bool enabled=true) noexcept
        :api_(api),feature_(feature),ordinal_(enabled&&activeEvaluation.api==FrameTrace::Api::Unknown?
            EvaluateEntered(api,feature,context):0),
         previous_(activeEvaluation){activeEvaluation={api,feature,context,ordinal_};}
    void Result(int64_t value) noexcept {result_=value;known_=true;}
    ~EvaluationScope(){
        activeEvaluation=previous_;if(!ordinal_)return;
        Snapshot snapshot;FrameTrace::Facts facts;facts.kind=FrameTrace::Kind::EvaluateReturned;
        facts.api=api_;facts.feature=feature_;facts.apiFeature=feature_;facts.evaluateOrdinal=ordinal_;
        Observe(Consumer::Fg,snapshot,facts,known_?"SR API returned":"API exit unseen",known_,result_);
    }
    EvaluationScope(const EvaluationScope&)=delete;
    EvaluationScope& operator=(const EvaluationScope&)=delete;
};
// Observation contention drops a status update, never blocks Evaluate/Present.
inline void Publish(Consumer consumer,Snapshot snapshot) noexcept {
    try {
        auto& s=Storage();std::unique_lock lock(s.mutex,std::try_to_lock);
        if(!lock.owns_lock()){s.dropped.fetch_add(1,std::memory_order_relaxed);return;}
        if(!snapshot.tick)snapshot.tick=Now();
        auto& lane=s.view.lanes[static_cast<unsigned>(consumer)];
        // A late completion may update completed, but cannot replace newer input.
        const bool same=snapshot.source==lane.latest.source && snapshot.epoch==lane.latest.epoch &&
            snapshot.configuration==lane.latest.configuration && snapshot.feature==lane.latest.feature;
        const bool sameDomain=snapshot.feature==lane.latest.feature && snapshot.configuration==lane.latest.configuration;
        const bool older=sameDomain && snapshot.source && lane.latest.source && snapshot.source<lane.latest.source;
        // The lifecycle callback may arrive after the optional pixel readback.
        // Keep that measurement only for this exact source/consumer contract.
        const auto& measured=lane.completed;
        if(!snapshot.contributionKnown && measured.contributionKnown && snapshot.source==measured.source &&
           snapshot.epoch==measured.epoch && snapshot.feature==measured.feature && snapshot.configuration==measured.configuration){
            snapshot.contributionKnown=true;snapshot.contribution=measured.contribution;snapshot.coverage=measured.coverage;
        }
        if(snapshot.stage==Stage::Completed && (!lane.completed.tick ||
           (snapshot.feature==lane.completed.feature && snapshot.configuration==lane.completed.configuration &&
            snapshot.source>=lane.completed.source) ||
           (!older&&same&&snapshot.tick>=lane.completed.tick)))lane.completed=snapshot;
        const bool transition=!same || snapshot.stage!=lane.latest.stage || snapshot.reason!=lane.latest.reason;
        const bool regresses=same && lane.latest.stage==Stage::Completed && snapshot.stage!=Stage::Completed;
        if(!older&&!regresses&&snapshot.tick>=lane.latest.tick &&
           (snapshot.stage!=Stage::Completed||same||!lane.latest.tick))lane.latest=snapshot;
        ++lane.observations;++s.view.revision;
        if(transition){
            Event event{consumer,snapshot.stage,snapshot.source,snapshot.picture,
                snapshot.providerFrame,Now(),snapshot.reason};
            event.sourceTick=snapshot.tick; // source age and observation time are different clocks
            event.facts=TraceFacts(snapshot,FrameTrace::Kind::ConsumerTransition);
            AppendEvent(s,std::move(event));
        }
    }catch(...){}
}
inline bool Read(View& view) noexcept {
    try {auto& s=Storage();std::unique_lock lock(s.mutex,std::try_to_lock);
        if(!lock.owns_lock())return false;view=s.view;view.sourceRate=s.cadence.Read();
        if(s.cadenceLost.load(std::memory_order_relaxed))view.sourceRate.known=false;
        view.droppedEvents=s.dropped.load(std::memory_order_relaxed);return true;}catch(...){return false;}
}
inline bool ReadStatistics(Statistics& value) noexcept {
    try {auto& s=Storage();std::unique_lock lock(s.mutex,std::try_to_lock);
        if(!lock.owns_lock())return false;
        value=s.statistics;value.source=s.cadence.Read();
        if(s.cadenceLost.load(std::memory_order_relaxed))value.source.known=false;
        return true;
    }catch(...){return false;}
}
// Output API events have no authenticated source/generated-frame identity.
// In particular, HRESULT success is not a GPU completion or visible-frame receipt.
inline void PresentReturned(int64_t result) noexcept {
    try {
        auto& s=Storage();std::unique_lock lock(s.mutex,std::try_to_lock);
        if(!lock.owns_lock()){s.dropped.fetch_add(1,std::memory_order_relaxed);return;}
        Event e;e.consumer=Consumer::Fg;e.tick=Now();e.presentReturned=e.resultKnown=true;e.apiResult=result;
        e.facts.kind=FrameTrace::Kind::PhysicalPresentReturned;
        e.reason="SDK Present returned; source/generated/display identity UNKNOWN";
        AppendEvent(s,std::move(e));
        ++s.view.revision;
    }catch(...){}
}
inline std::atomic<bool>& OnlyGenerated(){static std::atomic<bool> value{false};return value;}
inline uint64_t Age(const Snapshot& s,uint64_t now=Now()) noexcept {return s.tick && now>=s.tick?now-s.tick:UINT64_MAX;}
inline bool Stale(const Snapshot& s,uint64_t now=Now()) noexcept {return Age(s,now)>1500;}
} // namespace GuideDiagnostics
