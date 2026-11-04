#pragma once

// Local, bounded NR accounting. The caller holds g_nrMutex. CPU recording time,
// native GPU timestamps and queue wait are different domains; an unobserved wait
// stays unknown. No I/O, allocation, provider hook or GPU wait is performed here.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

namespace DlssNrCadence
{
enum class Stage : uint8_t { ReuseBefore, ModelLoop, Layer, ProviderCall, ReuseAfter, TotalGpu, ModelGpu };
enum class CacheReason : uint8_t
{
    Disabled, Accepted, NoHistory, ContractChanged, Reset, SlotInFlight,
    QueueUnknown, FenceUnknown, ExposureUnknown, ByteCap, RecordFailed, Count
};
inline const char* CacheReasonName(CacheReason reason)
{
    switch (reason)
    {
    case CacheReason::Accepted: return "Accepted";
    case CacheReason::NoHistory: return "NoHistory";
    case CacheReason::ContractChanged: return "ContractChanged";
    case CacheReason::Reset: return "Reset";
    case CacheReason::SlotInFlight: return "SlotInFlight";
    case CacheReason::QueueUnknown: return "QueueUnknown";
    case CacheReason::FenceUnknown: return "FenceUnknown";
    case CacheReason::ExposureUnknown: return "ExposureUnknown";
    case CacheReason::ByteCap: return "ByteCap";
    case CacheReason::RecordFailed: return "RecordFailed";
    default: return "DisabledOrFullDue";
    }
}
struct Identity
{
    uintptr_t owner = 0, queue = 0, list = 0;
    uint64_t config = 0, listGeneration = 0, pictureOrdinal = 0, dispatch = 0;
};
struct StageSample
{
    Identity identity {};
    Stage stage = Stage::Layer;
    unsigned layer = 0;
    uint64_t cpuBeginNs = 0, cpuEndNs = 0, cpuInclusiveNs = 0, cpuExclusiveNs = 0;
    uint64_t gpuBeginTicks = 0, gpuEndTicks = 0, queueHz = 0;
    uint64_t fenceTarget = 0, fenceCompleted = 0;
    double gpuInclusiveMs = 0, gpuExclusiveMs = 0, queueWaitMs = 0;
    bool gpuObserved = false, gpuExclusiveKnown = false, queueWaitObserved = false;
    bool submitted = false, completed = false, presented = false;
};
struct ReuseDecision
{
    Identity identity {};
    uint64_t sourceOrdinal = 0, sourceFence = 0, sourceFenceCompleted = 0, targetFence = 0, bytes = 0;
    unsigned intervalRequested = 1, intervalUsed = 1, slotInFlight = 0, pendingRecordings = 0;
    bool sourceQueueOrdered = false;
    bool completedRetirementsOnly = false; // GPU done; executable owners still retained, no history authority
    CacheReason cacheReason = CacheReason::Disabled;
    bool accepted = false;
};
class NrCadenceLedger
{
  public:
    static constexpr size_t Capacity = 256;
    std::array<StageSample, Capacity> stages {};
    std::array<ReuseDecision, Capacity> decisions {};
    std::array<Identity, Capacity> frames {};
    uint64_t stageCount = 0, decisionCount = 0, stageOverwritten = 0, decisionOverwritten = 0;
    uint64_t modelCalls = 0, modelFailures = 0, reuseFull = 0, reuseAccepted = 0, reuseRefused = 0;
    std::array<uint64_t, 32> layerModelCalls {}, layerModelFailures {};
    uint64_t gpuCompletionObserved = 0, presentObserved = 0;
    std::array<uint64_t, static_cast<size_t>(CacheReason::Count)> cacheReasons {};
    Identity epoch {};

    void BeginEpoch(const Identity& identity)
    {
        if (epoch.owner != identity.owner || epoch.config != identity.config)
        {
            // Resource ownership is external; only scalar observations are reset.
            *this = NrCadenceLedger {};
            epoch = identity;
        }
    }
    void Record(const StageSample& sample)
    {
        if (stageCount >= Capacity) ++stageOverwritten;
        stages[stageCount++ % Capacity] = sample;
        if (sample.completed && sample.gpuObserved) ++gpuCompletionObserved;
    }
    void Record(const ReuseDecision& decision)
    {
        if (decisionCount >= Capacity) ++decisionOverwritten;
        decisions[decisionCount++ % Capacity] = decision;
        ++cacheReasons[static_cast<size_t>(decision.cacheReason)];
        if (decision.accepted) ++reuseAccepted;
        else if (decision.intervalRequested > 1 && decision.cacheReason != CacheReason::Disabled) ++reuseRefused;
    }
    void Remember(const Identity& identity) { frames[identity.dispatch % Capacity] = identity; }
    template <class Evidence> void RecordGpu(Stage stage, unsigned layer, const Evidence& evidence)
    {
        const auto& identity = frames[evidence.dispatch % Capacity];
        if (!evidence.dispatch || identity.dispatch != evidence.dispatch ||
            identity.owner != epoch.owner || identity.config != epoch.config ||
            identity.list != evidence.list || !evidence.queueHz ||
            !evidence.fenceTarget || evidence.fenceCompleted < evidence.fenceTarget ||
            evidence.endTicks < evidence.beginTicks) return;
        StageSample sample;
        sample.identity = identity; sample.identity.queue = evidence.queue;
        sample.stage = stage; sample.layer = layer;
        sample.gpuBeginTicks = evidence.beginTicks; sample.gpuEndTicks = evidence.endTicks;
        sample.queueHz = evidence.queueHz; sample.fenceTarget = evidence.fenceTarget;
        sample.fenceCompleted = evidence.fenceCompleted; sample.gpuInclusiveMs = evidence.milliseconds;
        sample.gpuObserved = sample.submitted = sample.completed = true;
        // Exclusive GPU cost is unknown until sibling/child timestamps from this
        // exact submission are available. CPU duration is never substituted.
        Record(sample);
    }
};
// Inclusive parent time is never added to child time. A scope's exclusive CPU
// duration removes only its direct children, all measured by the same clock.
class StageScope
{
    using Clock = std::chrono::steady_clock;
    NrCadenceLedger& ledger;
    StageSample sample;
    StageScope* parent;
    Clock::time_point begin = Clock::now();
    uint64_t childrenNs = 0;
    bool stopped = false;
  public:
    StageScope(NrCadenceLedger& sink, Stage stage, const Identity& identity,
               unsigned layer = 0, StageScope* enclosing = nullptr)
        : ledger(sink), parent(enclosing)
    {
        sample.identity = identity; sample.stage = stage; sample.layer = layer;
        sample.cpuBeginNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            begin.time_since_epoch()).count());
    }
    void Stop()
    {
        if (stopped) return;
        stopped = true;
        sample.cpuEndNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count());
        sample.cpuInclusiveNs = sample.cpuEndNs - sample.cpuBeginNs;
        sample.cpuExclusiveNs = sample.cpuInclusiveNs - (std::min)(childrenNs, sample.cpuInclusiveNs);
        if (parent) parent->childrenNs += sample.cpuInclusiveNs;
        ledger.Record(sample);
    }
    ~StageScope() { Stop(); }
    StageScope(const StageScope&) = delete;
    StageScope& operator=(const StageScope&) = delete;
};
} // namespace DlssNrCadence
