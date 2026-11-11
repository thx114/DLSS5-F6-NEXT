// SPDX-License-Identifier: GPL-3.0-or-later
#include <runtime/GuideDiagnostics.h>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <set>
using namespace GuideDiagnostics;
int main(){try{
    unsigned checks=0;const auto check=[&](bool b){++checks;if(!b)throw std::runtime_error("observation invariant");};
    unsigned constructed=0;
    const auto expensiveSnapshot=[&] { ++constructed; };
    ObserveConsumers(expensiveSnapshot);check(constructed==0);
    SetConsumerObservationDemand(true);ObserveConsumers(expensiveSnapshot);check(constructed==1);
    SetConsumerObservationDemand(false);ObserveConsumers(expensiveSnapshot);check(constructed==1);
    // Disabling full snapshots must not disable source cadence/SDK statistics.
    Snapshot a;a.source=10;a.epoch=10;a.feature=1;a.configuration=1;a.tick=100;a.stage=Stage::Received;
    a.Set(Field::DepthX,0,true,Origin::ApiDefault);a.Set(Field::ScaleX,0,false,Origin::Evaluate);
    Publish(Consumer::NrModel,a);View view;check(Read(view));check(view.lanes[0].latest.source==10);
    check(view.lanes[0].latest.fields[static_cast<unsigned>(Field::DepthX)].valid);
    check(!view.lanes[0].latest.fields[static_cast<unsigned>(Field::ScaleX)].valid);
    a.stage=Stage::Recorded;Publish(Consumer::NrModel,a);check(Read(view));check(view.lanes[0].completed.tick==0);
    a.stage=Stage::Submitted;Publish(Consumer::NrModel,a);check(Read(view));check(view.lanes[0].completed.tick==0);
    auto b=a;b.source=11;b.epoch=11;b.tick=110;b.stage=Stage::Received;Publish(Consumer::NrModel,b);
    a.stage=Stage::Completed;a.tick=120;Publish(Consumer::NrModel,a);check(Read(view));
    check(view.lanes[0].latest.source==11);check(view.lanes[0].completed.source==10);
    b.stage=Stage::Completed;b.tick=130;Publish(Consumer::NrModel,b);check(Read(view));check(view.lanes[0].completed.source==11);
    a.tick=140;Publish(Consumer::NrModel,a);check(Read(view));check(view.lanes[0].completed.source==11);
    check(view.lanes[1].observations==0&&view.lanes[2].observations==0);
    check(!Stale(b,1500)&&Stale(b,1800));check(Stale(Snapshot{},1800));
    for(unsigned i=0;i<200;++i){b.source=12+i;b.tick=200+i;Publish(Consumer::NrModel,b);}
    check(Read(view));check(view.eventCount<=512);check(view.lanes[0].latest.generatedIdentityKnown==false);
    check(view.lanes[0].latest.contributionKnown==false);
    Snapshot measured;measured.source=25;measured.epoch=25;measured.feature=8;measured.configuration=2;
    measured.tick=500;measured.stage=Stage::Completed;measured.contributionKnown=true;measured.contribution=.25f;measured.coverage=.9f;
    Publish(Consumer::NrHistory,measured);auto unmeasured=measured;unmeasured.contributionKnown=false;unmeasured.contribution=-1;
    Publish(Consumer::NrHistory,unmeasured);check(Read(view));check(view.lanes[1].completed.contributionKnown);
    check(view.lanes[1].completed.contribution==.25f&&view.lanes[1].completed.coverage==.9f);
    unmeasured.stage=Stage::Recorded;Publish(Consumer::NrHistory,unmeasured);check(Read(view));
    check(view.lanes[1].latest.stage==Stage::Completed);
    unmeasured.source=26;unmeasured.epoch=26;unmeasured.tick=510;Publish(Consumer::NrHistory,unmeasured);
    check(Read(view));check(!view.lanes[1].latest.contributionKnown);check(view.lanes[1].completed.source==25);
    const auto observations=view.lanes[2].observations;const auto completion=view.lanes[2].completed.tick;
    PresentReturned(0);PresentReturned(-2005270523);check(Read(view));
    check(view.lanes[2].observations==observations&&view.lanes[2].completed.tick==completion);
    check(!view.lanes[2].latest.generatedIdentityKnown);
    unsigned returns=0;for(const auto& e:view.events)if(e.presentReturned){
        ++returns;check(e.source==0&&e.picture==0&&e.providerFrame==0&&e.stage==Stage::Unknown&&e.resultKnown);}
    check(returns==2);
    const auto before=Now();
    Snapshot timed;timed.source=900;timed.epoch=900;timed.feature=90;timed.configuration=9;
    timed.tick=1;timed.stage=Stage::Received;Publish(Consumer::Fg,timed);
    const auto after=Now();check(Read(view));bool foundTimed=false;
    for(const auto& e:view.events)if(e.consumer==Consumer::Fg&&e.source==900){
        foundTimed=true;check(e.sourceTick==1&&e.tick>=before&&e.tick<=after);
    }
    check(foundTimed);check(view.lanes[2].latest.tick==1&&Stale(view.lanes[2].latest));
    check(!ConsumerObservationDemand().load());
    const auto ordinal=EvaluateEntered(FrameTrace::Api::D3D11,89,900);
    check(ordinal!=0);EvaluateReturned(FrameTrace::Api::D3D11,89,ordinal,-4);
    {EvaluationScope scope(FrameTrace::Api::D3D12,90,901);scope.Result(1);}
    {EvaluationScope scope(FrameTrace::Api::D3D12,90,901);}
    const auto countBefore=view.eventCount;
    {EvaluationScope scope(FrameTrace::Api::D3D12,91,902,false);}
    check(Read(view));check(view.eventCount==countBefore+6);
    unsigned entries=0,exits=0,unseen=0;std::set<uint64_t> sequences;
    for(const auto& e:view.events)if(e.sequence){
        check(sequences.insert(e.sequence).second);
        check(e.tickNs>0);
        check(!e.facts.generatedIdentityKnown&&!e.facts.displayIdentityKnown);
        if(e.facts.kind==FrameTrace::Kind::EvaluateEntered){++entries;check(!e.source&&e.facts.evaluateOrdinal>0);}
        if(e.facts.kind==FrameTrace::Kind::EvaluateReturned){++exits;if(!e.resultKnown)++unseen;}
    }
    check(entries==3&&exits==3&&unseen==1);
    // Contended diagnostics never block the source thread and explicitly make
    // the rate unknown rather than silently reporting an undercount as Hz.
    const auto drops=view.droppedEvents;
    {
        std::lock_guard held(Storage().mutex);
        std::thread writer([]{
            EvaluateEntered(FrameTrace::Api::D3D11,89,900);
            Snapshot s;Publish(Consumer::Fg,s);PresentReturned(0);
        });writer.join();
    }
    check(Read(view));check(view.droppedEvents==drops+3);check(!view.sourceRate.known);
    for(unsigned i=0;i<600;++i)PresentReturned(0);
    check(Read(view));check(view.eventCount==512);
    sequences.clear();uint64_t newest=0,oldest=UINT64_MAX;
    for(const auto& e:view.events){
        check(sequences.insert(e.sequence).second);
        if(e.sequence>newest)newest=e.sequence;if(e.sequence<oldest)oldest=e.sequence;
    }
    check(newest-oldest==511);
    // FPS readers use a numeric cache and cannot consume SDK counters or copy
    // the 512-entry string timeline on every overlay callback.
    Statistics statistics;
    FrameTrace::Facts sdk;sdk.kind=FrameTrace::Kind::SdkState;sdk.querySequence=100;
    sdk.queryBeginTick=1000;sdk.queryEndTick=2000;sdk.presentedInterval=7;sdk.queryIntervalKnown=true;
    Observe(Consumer::Fg,{},sdk,"actual query",true,0);
    check(ReadStatistics(statistics));
    check(statistics.querySequence==100&&statistics.beginNs==1000&&statistics.endNs==2000);
    check(statistics.presented==7&&statistics.intervalKnown&&statistics.sdkSucceeded);
    sdk.querySequence=99;sdk.presentedInterval=400;Observe(Consumer::Fg,{},sdk,"late older query",true,0);
    check(ReadStatistics(statistics)&&statistics.querySequence==100&&statistics.presented==7);
    const auto eventSequence=Storage().sequence;
    for(unsigned n=0;n<20;++n)check(ReadStatistics(statistics));
    check(Storage().sequence==eventSequence);
    sdk.querySequence=101;Observe(Consumer::Fg,{},sdk,"failed query",true,-1);
    check(ReadStatistics(statistics)&&!statistics.sdkSucceeded);
    check(CurrentEvaluation().ordinal==0);
    {EvaluationScope outer(FrameTrace::Api::D3D11,321,456);
        const auto parent=CurrentEvaluation();check(parent.ordinal!=0&&parent.feature==321);
        {EvaluationScope nested(FrameTrace::Api::D3D12,654,987,false);check(CurrentEvaluation().ordinal==0);}
        check(CurrentEvaluation().ordinal==parent.ordinal&&CurrentEvaluation().context==456);
    }
    check(CurrentEvaluation().ordinal==0);
    FrameTrace::SourceCadence cadence;
    cadence.Observe(FrameTrace::Api::D3D11,1,1000000000);
    check(!cadence.Read(1000000000).known);
    cadence.Observe(FrameTrace::Api::D3D11,1,1050000000);
    check(cadence.Read(1050000000).known&&cadence.Read(1050000000).hz==20);
    check(!cadence.Read(5000000000).known);
    cadence.Observe(FrameTrace::Api::D3D11,2,1100000000);
    check(!cadence.Read(1100000000).known);
    cadence.Observe(FrameTrace::Api::D3D11,2,1200000000);
    check(cadence.Read(1200000000).hz==10);
    cadence.Observe(FrameTrace::Api::D3D12,2,1300000000);
    check(!cadence.Read(1300000000).known);
    cadence.Observe(FrameTrace::Api::D3D12,2,1250000000);
    check(!cadence.Read(1250000000).known);
    cadence.Reset();check(!cadence.Read(1300000000).known);
    std::printf("GuideDiagnostics: %u checks passed\n",checks);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"GuideDiagnostics: %s\n",e.what());return 1;}}
