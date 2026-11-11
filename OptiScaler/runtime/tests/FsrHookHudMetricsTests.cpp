// SPDX-License-Identifier: GPL-3.0-or-later
#include "../FsrHookHudMetrics.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <thread>

namespace {
struct Clock {
    static inline int64_t now=100;
    static inline unsigned reads=0;
    static int64_t Tick() noexcept {++reads;return now;}
};
using Sampler=FsrHookHudMetrics::RootSampler<3,Clock>;
void Near(double actual,double expected){assert(std::abs(actual-expected)<0.000001);}
void Root(Sampler& s,unsigned kind,int64_t duration,bool confirmed=true) {
    Clock::now=100;auto t=s.Enter(kind);if(confirmed)s.Confirm(true);
    Clock::now+=duration;s.Leave(t);
}
}
int main() {
    using namespace FsrHookHudMetrics;
    unsigned groups=0;
    { // Inactive owner has no clock reads, counters, or allocation.
        Sampler s;Clock::reads=0;Root(s,0,50);assert(!s.Summarize(1000,1000).known);assert(Clock::reads==0);++groups;
    }
    { // Same-owner recursion and native callbacks do not double count.
        Sampler s;s.Reset(1);Clock::reads=0;Clock::now=100;auto root=s.Enter(0);s.Confirm(true);
        Clock::now=110;auto native=s.BeginOriginal();auto nested=s.Enter(1);s.Confirm(false);
        auto nestedNative=s.BeginOriginal();Clock::now=150;s.EndOriginal(nestedNative);s.Leave(nested);
        Clock::now=180;s.EndOriginal(native);Clock::now=200;s.Leave(root);
        const auto out=s.Summarize(1000,1000);assert(out.known && out.calls==1 && out.samples==1);
        Near(out.millisecondsPerSecond,30);assert(Clock::reads==4);++groups;
    }
    { // Nested work outside original is included once, its original excluded once.
        Sampler s;s.Reset(1);Clock::now=100;auto root=s.Enter(0);s.Confirm(true);
        auto nested=s.Enter(1);Clock::now=120;auto native=s.BeginOriginal();
        Clock::now=200;s.EndOriginal(native);s.Leave(nested);Clock::now=300;s.Leave(root);
        auto out=s.Summarize(1000,1000);assert(out.known && out.calls==1);Near(out.millisecondsPerSecond,120);++groups;
    }
    { // Multiple disjoint originals are a union, not just the first interval.
        Sampler s;s.Reset(1);Clock::now=100;auto root=s.Enter(0);s.Confirm(true);
        Clock::now=120;auto a=s.BeginOriginal();Clock::now=150;s.EndOriginal(a);
        Clock::now=200;auto b=s.BeginOriginal();Clock::now=240;s.EndOriginal(b);
        Clock::now=300;s.Leave(root);Near(s.Summarize(1000,1000).millisecondsPerSecond,130);++groups;
    }
    { // Stride and per-kind weighting: 64 cheap roots + one expensive root.
        Sampler s;s.Reset(1);Clock::reads=0;
        for(unsigned i=0;i<64;++i)Root(s,0,10);
        Root(s,1,100);const auto out=s.Summarize(1000,1000);
        assert(out.known && out.calls==65 && out.samples==2 && Clock::reads==4);
        Near(out.millisecondsPerSecond,740);Near(s.Summarize(1000,2000).millisecondsPerSecond,370);++groups;
    }
    { // Missing samples for any participating kind means unknown, never zero.
        Sampler s;s.Reset(1);Root(s,0,10);s.ClearWindow();Root(s,0,10);Root(s,1,50);
        const auto out=s.Summarize(1000,1000);assert(!out.known && out.calls==2 && out.samples==1);
        assert(!s.Summarize(0,1000).known && !s.Summarize(1000,0).known);++groups;
    }
    { // No direct owner proof, foreign nested work, and unsupported roots.
        Sampler s;s.Reset(1);Root(s,0,10,false);
        Clock::now=100;auto root=s.Enter(1);s.Confirm(true);auto child=s.Enter(2);s.Confirm(false);
        s.Leave(child);Clock::now=120;s.Leave(root);
        root=s.Enter(99);s.Confirm(true);s.Leave(root);
        auto out=s.Summarize(1000,1000);assert(!out.known && out.calls==0 && out.unconfirmedRoots==3);++groups;
    }
    { // A nested valid context cannot certify an unproved foreign parent.
        Sampler s;s.Reset(1);auto root=s.Enter(0);auto child=s.Enter(1);s.Confirm(true);
        s.Leave(child);s.Leave(root);assert(!s.Summarize(1000,1000).known);++groups;
    }
    { // Failed QPC, backwards clocks, and invalid native intervals poison cost.
        for(unsigned mode=0;mode<4;++mode){
            Sampler s;s.Reset(1);Clock::now=mode==0?0:100;auto root=s.Enter(0);s.Confirm(true);
            if(mode>=2){Clock::now=mode==2?0:110;auto native=s.BeginOriginal();Clock::now=300;s.EndOriginal(native);}
            Clock::now=mode==1?99:200;s.Leave(root);
            auto out=s.Summarize(1000,1000);assert(!out.known && out.clockFailures==1);
        }++groups;
    }
    { // A real zero duration with valid endpoints is still a valid sample.
        Sampler s;s.Reset(1);Root(s,0,0);auto out=s.Summarize(1000,1000);
        assert(out.known && out.samples==1);Near(out.millisecondsPerSecond,0);++groups;
    }
    { // Old RAII tickets cannot alter a new owner's depth or original union.
        Sampler s;s.Reset(1);auto old=s.Enter(0);auto oldNative=s.BeginOriginal();
        s.Reset(2);Clock::now=100;auto root=s.Enter(1);s.Confirm(true);
        s.EndOriginal(oldNative);s.Leave(old);Clock::now=150;s.Leave(root);
        const auto out=s.Summarize(1000,1000);assert(out.known && out.calls==1);Near(out.millisecondsPerSecond,50);++groups;
    }
    { // An incomplete root at the window cut is unknown and cannot leak tails.
        Sampler s;s.Reset(1);auto root=s.Enter(0);s.Confirm(true);
        assert(!s.Summarize(1000,1000).known);s.ClearWindow();s.Leave(root);assert(s.Generation()==0);++groups;
    }
    {
        HitRate rate{70,30};assert(rate.Known() && rate.Attempts()==100);Near(rate.Percent(),70);
        assert(!HitRate{}.Known());assert((!HitRate{UINT64_MAX,1}.Known()));
        rate={0,100};assert(rate.Known());Near(rate.Percent(),0);
        Snapshot s;s.windowKnown=true;s.beginMs=100;s.endMs=1100;s.frames=60;s.ownerGeneration=5;s.threadId=7;
        assert(Fresh(s,5,3100).windowKnown);assert(!Fresh(s,5,3101).windowKnown);
        assert(!Fresh(s,5,1099).windowKnown && !Fresh(s,6,1100).windowKnown);
        s.beginMs=s.endMs;assert(!Fresh(s,5,1100).windowKnown);++groups;
    }
    { // Cross-thread UI reads are either coherent or explicitly unknown.
        SnapshotCell cell;assert(!cell.Read().windowKnown);std::atomic<bool> done=false;
        std::thread publisher([&]{for(uint64_t i=1;i<=20000;++i){Snapshot s;s.windowKnown=true;
            s.beginMs=i;s.endMs=i+1000;s.frames=i;s.ownerGeneration=9;s.threadId=3;
            s.shaderMetadata={i,i+1};s.psBindingCurrent={i+2,i+3};
            s.hookHost={true,double(i),i+4,i+5,i+6,i+7};cell.Publish(s);}done.store(true,std::memory_order_release);});
        unsigned accepted=0;
        do {const auto s=cell.Read();if(!s.windowKnown)continue;++accepted;
            assert(s.endMs==s.beginMs+1000 && s.frames==s.beginMs && s.ownerGeneration==9);
            assert(s.shaderMetadata.hits==s.beginMs && s.psBindingCurrent.hits==s.beginMs+2);
            assert(s.hookHost.known && s.hookHost.calls==s.beginMs+4 && s.hookHost.clockFailures==s.beginMs+7);
            Near(s.hookHost.millisecondsPerSecond,double(s.beginMs));
        }while(!done.load(std::memory_order_acquire));
        publisher.join();assert(cell.Read().frames==20000);(void)accepted;++groups;
    }
    std::printf("FsrHookHudMetrics: %u CPU groups passed\n",groups);
}
