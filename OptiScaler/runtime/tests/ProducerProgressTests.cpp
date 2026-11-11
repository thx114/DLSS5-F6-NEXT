// SPDX-License-Identifier: GPL-3.0-or-later
// Executes the actual CPU policy. No SDK/GPU completion or draw is simulated
// as a real receipt. Lead owns compilation and runner integration.
#include "../ProducerProgress.h"
#include <cstdio>
#include <cstdlib>
using Progress=FramePipeline::ProducerProgress;
using Phase=Progress::Phase;
namespace {
unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::abort();}}
Progress::Owner Owner(){return {1,2,3,4,5,6,7,8};}
void HealthyAndIdle(){
    Progress p;const auto owner=Owner();Progress::Production target{owner,1};
    for(uint64_t g=1;g!=378;++g){target.generation=g;p.Observe(owner,g,target);
        Check(p.State()==Phase::Tracking && p.Produced()==g && !p.OfferProbe(owner),"adjacent target progress needs no getter probes");}
    for(unsigned i=0;i!=335;++i){p.Observe(owner,377,target);
        Check(p.State()==Phase::Idle && !p.OfferProbe(owner),"repeated Presents without Render progress are idle");}
}
void SceneGapAndRecovery(){
    Progress p;const auto owner=Owner();Progress::Production target{owner,379};p.Observe(owner,379,target);
    unsigned probes=0;
    for(uint64_t g=380;g!=715;++g){p.Observe(owner,g,target);
        if(g>=383)Check(p.State()==Phase::Stalled,"advancing Render with no matched target stalls");
        for(unsigned draw=0;draw!=512;++draw)if(p.OfferProbe(owner)){
            Check(draw==Progress::DrawOffsets[probes],"stalled probes cover fixed distinct draw offsets");++probes;}
        Check(!p.OfferProbe(owner),"same Render interval cannot consume another probe");}
    Check(probes==Progress::ProbeBudget,"335 missing targets use exactly the fixed probe budget");
    target.generation=715;p.Observe(owner,715,target);
    Check(p.State()==Phase::Recovering && !p.OfferProbe(owner),"only a new exact target marks recovery");
    target.generation=716;p.Observe(owner,716,target);Check(p.State()==Phase::Tracking,"subsequent target tracks normally");
    p.Observe(owner,800,target);Check(!p.OfferProbe(owner),"healthy recovery does not renew exhausted owner budget");
}
void IdentityAndCooldown(){
    const auto owner=Owner();
    for(unsigned field=0;field!=8;++field){
        Progress p;p.Observe(owner,1,{});p.Observe(owner,5,{});Check(p.OfferProbe(owner),"initial advancing gap admits one probe");
        auto other=owner;
        switch(field){case 0:++other.pipeline;break;case 1:++other.device;break;case 2:++other.context;break;
            case 3:++other.deviceEpoch;break;case 4:++other.ownerEpoch;break;case 5:++other.configEpoch;break;
            case 6:++other.instance;break;default:++other.thread;break;}
        p.Observe(other,100,{owner,100});Check(p.Produced()==0 && p.State()==Phase::Idle,"old owner target cannot certify replacement");
        p.Observe(other,104,{owner,104});Check(p.State()==Phase::Stalled,"replacement has its own stalled interval");
        Check(!p.OfferProbe(owner),"foreign native owner cannot take replacement probe");
        Check(p.OfferProbe(other),"replacement starts its independent bounded observation");
        p.Observe(other,105,{other,106});Check(p.Produced()==0,"future target cannot certify current Render");
        p.Observe(other,119,{});Check(!p.OfferProbe(other),"cooldown rejects fifteen generation distance");
        p.Observe(other,120,{});
        for(unsigned draw=0;draw!=Progress::DrawOffsets[1];++draw)
            Check(!p.OfferProbe(other),"second probe skips earlier eligible calls");
        Check(p.OfferProbe(other),"cooldown and draw offset must both pass");
    }
    Progress p;p.Observe(owner,10,{});p.Observe(owner,14,{});Check(p.OfferProbe(owner),"probe before regression");
    p.Observe(owner,1,{});Check(p.State()==Phase::Idle && p.Probes()==1 && !p.OfferProbe(owner),"regression grants no wraparound or budget reset");
    p.Observe(owner,UINT64_MAX,{});Check(p.State()==Phase::Idle && p.Probes()==1 && !p.OfferProbe(owner),"sentinel Render disarms without renewing budget");
    auto invalid=owner;invalid.instance=0;p.Observe(invalid,4,{});Check(p.State()==Phase::Idle && !p.OfferProbe(invalid),"unknown instance never arms observation");
}
void ShortFramesAndOwnerBoundary(){
    Progress p;const auto owner=Owner();p.Observe(owner,379,{owner,379});
    unsigned probes=0,nonFirst=0,eligibleSinceProbe=0;
    uint64_t previousGeneration=0;
    // One eligible draw per Render: offsets accumulate rather than waiting
    // forever for a long frame. This exercises the 255-call offset as well.
    for(uint64_t g=380;g!=1800;++g){p.Observe(owner,g,{owner,379});
        if(p.ProbeDue(owner))++eligibleSinceProbe;
        if(p.OfferProbe(owner)){
            Check(eligibleSinceProbe==Progress::DrawOffsets[probes]+1,"short frames preserve dispersed call offset");
            Check(!previousGeneration || g-previousGeneration>=Progress::ProbeCooldown,"short frames retain generation cooldown");
            if(eligibleSinceProbe>1)++nonFirst;
            previousGeneration=g;eligibleSinceProbe=0;++probes;
        }
    }
    Check(probes==8 && nonFirst==7,"bounded probes eventually cover later calls even in short frames");
    auto next=owner;++next.instance;p.Observe(next,1800,{owner,1799});p.Observe(next,1804,{owner,1804});
    Check(p.Produced()==0 && !p.OfferProbe(owner) && p.OfferProbe(next),"old instance neither produces nor consumes replacement observation");
    p.Observe(next,1805,{next,1805});Check(p.State()==Phase::Recovering,"only matching new instance target recovers");
    p.Observe({},0,{});p.Observe(next,1820,{next,1805});
    Check(p.Probes()==1,"temporary missing observation does not renew owner budget");
}
}
int main(){HealthyAndIdle();SceneGapAndRecovery();IdentityAndCooldown();ShortFramesAndOwnerBoundary();
    std::printf("PASS %u producer progress checks; SDK/GPU/cache adapter unexecuted\n",checks);}
