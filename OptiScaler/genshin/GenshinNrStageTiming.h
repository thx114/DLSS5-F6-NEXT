#pragma once
#include "../dlssnr/DlssNrCadenceLedger.h"

// Observation only. Consume the existing scalar ledger under g_nrMutex. No new
// clocks, queries, waits, allocations or authority over the rendering path.
namespace GenshinNrStageTiming {
struct Totals {
    uint64_t cpuCount=0,cpuInclusiveNs=0,cpuExclusiveNs=0,cpuMaxNs=0,gpuCount=0;
    long double gpuSumNs=0,gpuMaxNs=0;
    DlssNrCadence::StageSample lastCpu{},lastGpu{};
};
class Accounting {
public:
    static constexpr size_t Count=7;
    std::array<Totals,Count> stages{};
    DlssNrCadence::Identity epoch{};
    uint64_t cursor=0,generation=0,dropped=0,invalid=0,firstDispatch=0;
    bool nativeScene=false,active=false;

    void Suspend() noexcept {active=false;}
    void Observe(const DlssNrCadence::NrCadenceLedger& ledger,bool scene,uint64_t dispatch) noexcept {
        if(!active || epoch.owner!=ledger.epoch.owner || epoch.config!=ledger.epoch.config ||
           scene!=nativeScene || ledger.stageCount<cursor) {
            stages={};epoch=ledger.epoch;cursor=0;dropped=invalid=0;
            nativeScene=scene;firstDispatch=dispatch;active=true;++generation;
        }
        // A frozen/overrun recorder is visible as lost coverage, never zero cost.
        const auto available=(std::min)(ledger.stageCount,uint64_t(ledger.Capacity));
        const auto oldest=ledger.stageCount-available;
        if(cursor<oldest){dropped+=oldest-cursor;cursor=oldest;}
        for(;cursor<ledger.stageCount;++cursor) {
            const auto& s=ledger.stages[cursor%ledger.Capacity];
            const auto index=static_cast<size_t>(s.stage);
            if(index>=Count || s.identity.owner!=epoch.owner || s.identity.config!=epoch.config) {
                ++invalid;continue;
            }
            if(s.identity.dispatch<firstDispatch)continue;
            auto& t=stages[index];
            if(s.cpuBeginNs && s.cpuEndNs>=s.cpuBeginNs &&
               s.cpuInclusiveNs==s.cpuEndNs-s.cpuBeginNs && s.cpuExclusiveNs<=s.cpuInclusiveNs) {
                ++t.cpuCount;t.cpuInclusiveNs+=s.cpuInclusiveNs;t.cpuExclusiveNs+=s.cpuExclusiveNs;
                t.cpuMaxNs=(std::max)(t.cpuMaxNs,s.cpuInclusiveNs);t.lastCpu=s;
            }
            if(s.gpuObserved) {
                if(!s.submitted || !s.completed || !s.queueHz || !s.fenceTarget ||
                   s.fenceCompleted<s.fenceTarget || s.gpuEndTicks<s.gpuBeginTicks) {
                    ++invalid;continue;
                }
                // Normalize each exact queue interval independently. Never subtract
                // model and total means or convert CPU wall into GPU duration.
                const long double ns=static_cast<long double>(s.gpuEndTicks-s.gpuBeginTicks)*
                    1000000000.0L/static_cast<long double>(s.queueHz);
                ++t.gpuCount;t.gpuSumNs+=ns;t.gpuMaxNs=(std::max)(t.gpuMaxNs,ns);t.lastGpu=s;
            }
        }
    }
};
} // namespace GenshinNrStageTiming
