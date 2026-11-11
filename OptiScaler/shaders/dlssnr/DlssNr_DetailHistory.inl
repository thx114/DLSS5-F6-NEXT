// SPDX-License-Identifier: GPL-3.0-or-later
// Included AFTER DlssNr_DetailReuse.inl, inside DlssNr_Dx12.cpp's anonymous
// namespace. All entry points require g_nrMutex. No cadence/model skipping,
// SDK state, diagnostics input, waits, or game-specific capture lives here.
#include "DlssNr_DetailHistoryPolicy.h"
#include "DlssNr_OrderedLifetime.h"
#include <mutex>

namespace DetailHistory
{
using Frame = DetailReuse::Frame;
using Key = NrDetailHistoryPolicy::Key<DetailReuse::kEncodedContractBytes>;
using Recording = DlssNrGpuLifetime::HistoryRecord<Key>;
using Clock = std::chrono::steady_clock;

struct Plan { bool active = false, historyUsable = false; };
struct Snapshot
{
    bool requested = false, active = false, historyUsable = false;
    bool recorded = false, published = false, queueOrdered = false;
    const char* reason = "not-requested";
    uint64_t historySource = 0, recordedCount = 0, publishedCount = 0;
    uint64_t pending = 0, bytes = 0;
    uint64_t source = 0, configuration = 0, owner = 0;
    uint64_t full = 0, modelCalls = 0, modelFailures = 0;
    bool modelCallCountsKnown = false;
};

DlssNrGpuLifetime::OrderedHistory<Key> recordings;
NrDetailHistoryPolicy::SourceSequence sources;
ID3D12Resource* detail[2] {};
ID3D12Resource* colourDepth[2] {};
ID3D12Resource* stability[2] {};
ID3D12Resource* fused = nullptr;
ID3D12Device* resourceDevice = nullptr; // identity, resources retain their device
unsigned width = 0, height = 0, publishedWrite = 0, activeWrite = 0;
uint64_t generation = 0, publishedGeneration = 0, allocatedBytes = 0;
Key publishedKey, activeKey;
DlssNrDetailReuseConstants constants {};
bool historyPublished = false, prepared = false, fusedReadable = false;
ID3D12CommandList* activeList = nullptr;
Snapshot snapshot;
NrHistoryDebug::Frame debugFrame;
std::mutex publishedMutex;
Snapshot publishedSnapshot;

Snapshot Status();
void PublishObservation()
{
    // This ledger already counts actual SDK calls/layers/failures. Observation
    // only: no model/history decision may read either published snapshot.
    snapshot.modelCalls = DetailReuse::ledger.modelCalls;
    snapshot.modelFailures = DetailReuse::ledger.modelFailures;
    snapshot.modelCallCountsKnown = snapshot.modelCalls != 0;
    const auto value = Status();
    if (NrHistoryDebug::Wanted() && debugFrame.source==value.source &&
        debugFrame.owner==value.owner && debugFrame.configuration==value.configuration)
    {
        debugFrame.source=value.source;debugFrame.owner=value.owner;debugFrame.configuration=value.configuration;
        debugFrame.requested=value.requested;
        debugFrame.admitted=value.active;debugFrame.history=value.historyUsable;debugFrame.recorded=value.recorded;
        debugFrame.reason=value.reason;
        NrHistoryDebug::Observe(debugFrame);
    }
    std::lock_guard<std::mutex> lock(publishedMutex);
    publishedSnapshot = value;
}
struct PublishOnExit { ~PublishOnExit() { PublishObservation(); } };

// Only accounting identities are kept here. ParkNrResource/CollectRetired own
// release and require the real registry tokens' completion AND Reset closure.
struct ParkedAllocation { ID3D12Resource* resource; uint64_t bytes; };
std::vector<ParkedAllocation> parked;
uint64_t AllocationBytes(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc)
{
    if (!device) return UINT64_MAX;
    const auto info = device->GetResourceAllocationInfo(0, 1, &desc);
    return info.SizeInBytes && info.Alignment ? info.SizeInBytes : UINT64_MAX;
}
uint64_t AddBytes(uint64_t a, uint64_t b)
{
    return a == UINT64_MAX || b >= UINT64_MAX - a ? UINT64_MAX : a + b;
}
uint64_t ParkedBytes()
{
    uint64_t bytes = 0;
    for (auto it = parked.begin(); it != parked.end();)
    {
        const bool retained = std::any_of(g_nrRetired.begin(), g_nrRetired.end(),
            [&](const NrRetired& r) { return r.resource == it->resource; });
        if (!retained) it = parked.erase(it);
        else { bytes = AddBytes(bytes, it->bytes); ++it; }
    }
    return bytes;
}
void Park(ID3D12Resource*& resource)
{
    if (!resource) return;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    const auto bytes = SUCCEEDED(resource->GetDevice(IID_PPV_ARGS(&device)))
        ? AllocationBytes(device.Get(), resource->GetDesc()) : UINT64_MAX;
    parked.push_back({ resource, bytes });
    ParkNrResource(resource);
}
ID3D12CommandList* RealList(ID3D12CommandList* list)
{
    ID3D12CommandList* real = nullptr;
    return list && Util::CheckForRealObject(__FUNCTION__, list, (IUnknown**)&real) ? real : list;
}
ID3D12CommandQueue* RealQueue(ID3D12CommandQueue* queue)
{
    ID3D12CommandQueue* real = nullptr;
    return queue && Util::CheckForRealObject(__FUNCTION__, queue, (IUnknown**)&real) ? real : queue;
}
void Invalidate()
{
    historyPublished = false;
    recordings.Each([](Recording& r) { r.publish = false; });
    snapshot.published = snapshot.historyUsable = snapshot.queueOrdered = false;
    snapshot.historySource = 0;
}
void Publish(Recording& record, bool ordered)
{
    // OrderedHistory supplies real post-Execute/signal proof; only an explicitly
    // authenticated producer may use it. Borrowed lists/queue hints grant none.
    if (ordered && !record.ordered) return;
    // false is reached only from Collect after this record's actual queue fence
    // AND resource use report GPU completion. Queue-order publication isn't it.
    NrHistoryDebug::Lifecycle(record.ordinal,record.contract.identity.owner,record.contract.identity.revision,
                              true,!ordered,false,record.quarantined);
    DlssNrGpuLifetime::PublishHistoryRecord(record, ordered, publishedGeneration,
        [](const Recording& accepted, bool queueOrdered)
        {
            publishedWrite = accepted.write;
            publishedKey = accepted.contract; // exact captured ordinal/time/J
            historyPublished = true;
            ++snapshot.publishedCount;
            snapshot.published = true;
            snapshot.historySource = accepted.ordinal;
            snapshot.queueOrdered = queueOrdered;
            snapshot.reason = queueOrdered ? "history-published-queue-ordered" : "history-published-completed";
        });
}
void Collect() { recordings.Collect(Publish, Invalidate); }
bool OwnersBusy()
{
    bool busy = false;
    recordings.Each([&](const Recording& r)
    {
        // Reset can clear pending for an unsubmitted record before the shared
        // Registry callback closes its token. Pending alone is insufficient.
        busy = busy || r.pending || (r.resourceUse && !r.resourceUse->Complete());
    });
    return busy;
}
void ParkAll()
{
    Invalidate();
    for (auto** p : { &detail[0], &detail[1], &colourDepth[0], &colourDepth[1], &stability[0], &stability[1], &fused }) Park(*p);
    resourceDevice = nullptr; width = height = 0; allocatedBytes = 0;
}
D3D12_RESOURCE_DESC ScratchDesc(DXGI_FORMAT format, unsigned w, unsigned h)
{
    D3D12_RESOURCE_DESC d {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = format; d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return d;
}
bool PrepareResources(const Frame& f)
{
    if (resourceDevice == f.device && width == f.workWidth && height == f.workHeight &&
        detail[0] && detail[1] && colourDepth[0] && colourDepth[1] && fused &&
        ((f.cfg->DlssNrPresentedTemporalStabilityMs.value_or_default() != 0) == (stability[0] != nullptr && stability[1] != nullptr))) return true;
    // An unchanged allocation can advance in authenticated queue order. A new
    // size/device reaches here only after the old executable users have closed.
    ParkAll();
    const auto d = ScratchDesc(DXGI_FORMAT_R16G16B16A16_FLOAT, f.workWidth, f.workHeight);
    const auto c = ScratchDesc(DXGI_FORMAT_R32G32B32A32_FLOAT, f.workWidth, f.workHeight);
    const auto db = AllocationBytes(f.device, d), cb = AllocationBytes(f.device, c);
    const bool stabilize = f.cfg->DlssNrPresentedTemporalStabilityMs.value_or_default() != 0;
    const auto baseBytes = AddBytes(AddBytes(AddBytes(db, db), db), AddBytes(cb, cb));
    const auto required = stabilize ? AddBytes(baseBytes, AddBytes(db, db)) : baseBytes;
    const auto retired = ParkedBytes();
    snapshot.bytes = AddBytes(required, retired);
    if (!NrDetailHistoryPolicy::FitsBudget(required, retired))
    { snapshot.reason = "history-512-MiB-budget"; return false; }
    for (unsigned i = 0; i < 2; ++i)
    {
        detail[i] = CreateScratch(f.device, d.Format, f.workWidth, f.workHeight);
        colourDepth[i] = CreateScratch(f.device, c.Format, f.workWidth, f.workHeight);
        if (stabilize) stability[i] = CreateScratch(f.device, d.Format, f.workWidth, f.workHeight);
    }
    fused = CreateScratch(f.device, d.Format, f.workWidth, f.workHeight);
    if (!detail[0] || !detail[1] || !colourDepth[0] || !colourDepth[1] || !fused ||
        (stabilize && (!stability[0] || !stability[1])))
    { ParkAll(); snapshot.reason = "history-allocation-failed"; return false; }
    resourceDevice = f.device; width = f.workWidth; height = f.workHeight;
    allocatedBytes = required;
    return true;
}
NrDetailHistoryPolicy::Settings Settings(const Frame& f)
{
    return {
        std::min(f.cfg->DlssNrPresentedTemporalDurationMs.value_or_default(),
                 Config::kPresentedTemporalDurationMaxMs) / 1000.0f,
        std::clamp(f.cfg->DlssNrPresentedTemporalMaxAgeMs.value_or_default(),
                   Config::kPresentedTemporalMaxAgeMinMs, Config::kPresentedTemporalMaxAgeMaxMs) / 1000.0f,
        std::clamp(f.cfg->DlssNrPresentedTemporalStrength.value_or_default(), 0.0f, 0.95f),
        std::min(f.cfg->DlssNrPresentedTemporalStabilityMs.value_or_default(), 2000u) / 1000.0f
    };
}
NrDetailHistoryPolicy::Phase Phase(const Frame& f)
{
    NrDetailHistoryPolicy::PhaseFacts p;
    p.requested = f.independentFeedbackRequested;
    p.preSr = f.nativePreSrRoute; p.nativeScene = f.info->NativeSceneRoute;
    p.beforeUpscale = f.beforeUpscale; p.finishedPicture = f.finishedPicture;
    p.baseIdentity = f.baseIdentityVerified; p.currentDepth = f.currentDepthVerified;
    p.consumerMappingVerified = f.nativeGuideContractVerified && f.nativeGuidePhaseVerified;
    p.routeConvention = f.info->NativeRouteConventionVerified;
    p.metadataKnown = f.info->NativeGuideMetadataKnown; p.flagsKnown = f.info->NativeFeatureFlagsKnown;
    p.jitterKnown = f.info->NativeCurrentJitterKnown; p.motionJitterKnown = f.info->NativeMotionJitterKnown;
    p.motionJittered = f.info->NativeMotionVectorsJittered; p.motionLowResolution = f.info->MotionVectorsLowResolution;
    p.unsupported = f.vulkan || f.unsupportedVulkanGameOrigin || f.edgeCompression || f.blocked ||
        f.info->FreshColorResetFallback || f.info->MotionIsPictureFlow;
    p.renderWidth = f.info->RenderSubrectWidth; p.renderHeight = f.info->RenderSubrectHeight;
    p.motionWidth = f.motionWidth; p.motionHeight = f.motionHeight;
    p.depthWidth = f.depthWidth; p.depthHeight = f.depthHeight;
    p.jitterX = f.info->NativeJitterX; p.jitterY = f.info->NativeJitterY;
    p.currentJitterUvX = f.currentRasterJitterUvX; p.currentJitterUvY = f.currentRasterJitterUvY;
    p.motionPhaseX = f.nativeMotionPhaseUvX; p.motionPhaseY = f.nativeMotionPhaseUvY;
    p.depthPhaseX = f.nativeDepthPhaseUvX; p.depthPhaseY = f.nativeDepthPhaseUvY;
    p.cancelX = f.nativeMotionJitterCancelUvX; p.cancelY = f.nativeMotionJitterCancelUvY;
    p.decodedScaleX = f.mvScaleX; p.decodedScaleY = f.mvScaleY;
    return NrDetailHistoryPolicy::ResolvePhase(p);
}
Key Contract(const Frame& f, const NrDetailHistoryPolicy::Phase& phase,
             const NrDetailHistoryPolicy::Settings& settings, double sourceEntrySeconds)
{
    Key k;
    auto& i = k.identity;
    i.owner = f.owner; i.device = reinterpret_cast<uintptr_t>(f.device);
    i.queue = reinterpret_cast<uintptr_t>(RealQueue(f.temporalQueue));
    i.revision = f.revision;
    // Late final captureEpoch follows SubmissionEpoch and changes every source.
    // Its actual source feature creation generation is SceneGuideOwner[2].
    i.sourceEpoch = f.nativePreSrRoute ? f.info->SourceCaptureEpoch : f.info->SceneGuideOwner[2];
    i.placement = phase.placement;
    i.timeDomain = f.info->GuideObservationTick ? NrDetailHistoryPolicy::TimeDomain::SourceCaptureMilliseconds :
        NrDetailHistoryPolicy::TimeDomain::ConsumerSourceEntrySeconds;
    for (unsigned n = 0; n < 3; ++n)
        i.sourceOwner[n] = f.nativePreSrRoute ? f.preSrOwner[n] : f.info->SceneGuideOwner[n];
    i.exposure = f.exposureContract; i.scene = f.sceneGuideContract;
    const auto depthDesc = f.depth->GetDesc(), motionDesc = f.motion->GetDesc();
    const auto fb = NrDetailHistoryPolicy::FloatBits;
    const uint64_t coordinateControls = (f.cfg->DlssNrDepthTextureFlipY.value_or_default() ? 1ull : 0ull) |
        (f.cfg->DlssNrMotionTextureFlipY.value_or_default() ? 2ull : 0ull) |
        (f.cfg->DlssNrMotionYInverted.value_or_default() ? 4ull : 0ull);
    i.mapping = { f.workWidth, f.workHeight, static_cast<uint64_t>(f.answerFormat),
        f.motionWidth, f.motionHeight, f.motionBaseX, f.motionBaseY,
        motionDesc.Width, motionDesc.Height, static_cast<uint64_t>(motionDesc.Format),
        f.depthWidth, f.depthHeight, f.depthBaseX, f.depthBaseY,
        depthDesc.Width, depthDesc.Height, static_cast<uint64_t>(depthDesc.Format),
        f.depthInverted ? 1ull : 0ull, fb(f.mvScaleX), fb(f.mvScaleY),
        f.info->RenderSubrectWidth, f.info->RenderSubrectHeight,
        f.info->OutputWidth, f.info->OutputHeight,
        f.info->DepthSubrectBaseX, f.info->DepthSubrectBaseY,
        f.info->MotionSubrectBaseX, f.info->MotionSubrectBaseY,
        fb(f.info->MvScaleX), fb(f.info->MvScaleY), f.info->NativeFeatureFlags,
        f.info->RayReconstruction ? 1ull : 0ull, coordinateControls,
        static_cast<uint64_t>(f.modelInput->GetDesc().Format), f.reversibleMode,
        f.passthrough ? 1ull : 0ull, f.beforeUpscale ? 1ull : 0ull, f.finishedPicture ? 1ull : 0ull };
    i.settings = { fb(settings.durationSeconds), fb(settings.maxAgeSeconds), fb(settings.strength),
        fb(f.cfg->DlssNrPresentedTemporalDepthTolerance.value_or_default()),
        fb(f.cfg->DlssNrPresentedTemporalColourTolerance.value_or_default()),
        fb(f.cfg->DlssNrPresentedTemporalMotionTolerance.value_or_default()),
        fb(f.cfg->DlssNrPresentedTemporalNoiseFloor.value_or_default()),
        fb(settings.stabilitySeconds),
        fb(f.cfg->DlssNrPresentedTemporalStabilityFloor.value_or_default()),
        fb(f.cfg->DlssNrPresentedTemporalStabilityClamp.value_or_default()),
        f.independentFeedbackRequested ? 1ull : 0ull };
    k.encoding = f.encodedTransform;
    // Prefer the immutable actual Evaluate/capture tick. The fallback is taken
    // once at THIS source's Before entry, never in After/Submitted/Collect or at
    // completion. Different origins are isolated by the stable time-domain key.
    const double sourceSeconds = f.info->GuideObservationTick ?
        static_cast<double>(f.info->GuideObservationTick) / 1000.0 : sourceEntrySeconds;
    k.sample = { f.frameNumber, sourceSeconds,
                 phase.jitterUvX, phase.jitterUvY };
    k.sample.captureEpoch = f.info->SourceCaptureEpoch;
    return k;
}
bool ViewFits(ID3D12Resource* resource, unsigned x, unsigned y, unsigned w, unsigned h)
{
    if (!resource || !w || !h) return false;
    const auto d = resource->GetDesc();
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize == 1 &&
        d.MipLevels == 1 && d.SampleDesc.Count == 1 &&
        !(d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
        x < d.Width && y < d.Height && w <= d.Width - x && h <= d.Height - y;
}

Plan Before(const Frame& f)
{
    const double sourceEntrySeconds = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    const auto step = sources.Observe(f.frameNumber); // includes EVERY refused/busy source
    prepared = false;
    Collect();
    snapshot.requested = f.independentFeedbackRequested;
    snapshot.source = f.frameNumber; snapshot.configuration = f.revision; snapshot.owner = f.owner;
    snapshot.active = snapshot.historyUsable = snapshot.recorded = false;
    snapshot.reason = "not-requested";
    debugFrame.source=0; // enabling the inspector mid-recording cannot relabel old metadata
    if(NrHistoryDebug::Wanted())
    {
        debugFrame={};debugFrame.tick=NrHistoryDebug::Now();
        debugFrame.source=f.frameNumber;debugFrame.owner=f.owner;debugFrame.configuration=f.revision;
        debugFrame.depth=f.depth!=nullptr;debugFrame.motion=f.motion!=nullptr;
        debugFrame.width=f.workWidth;debugFrame.height=f.workHeight;
        debugFrame.motionWidth=f.motionWidth;debugFrame.motionHeight=f.motionHeight;
        debugFrame.depthWidth=f.depthWidth;debugFrame.depthHeight=f.depthHeight;
        debugFrame.scaleX=f.mvScaleX;debugFrame.scaleY=f.mvScaleY;
        debugFrame.motionPhaseX=f.nativeMotionPhaseUvX;debugFrame.motionPhaseY=f.nativeMotionPhaseUvY;
        debugFrame.depthPhaseX=f.nativeDepthPhaseUvX;debugFrame.depthPhaseY=f.nativeDepthPhaseUvY;
    }
    const auto reject = [&](const char* reason)
    { snapshot.reason = reason; return Plan {}; };
    if (!f.independentFeedbackRequested)
    {
        Invalidate();
        if (!OwnersBusy() && !fusedReadable) ParkAll();
        return {};
    }
    if (!f.cfg || !f.info || !f.pass || !f.cmdList || !f.device || !f.temporalQueue ||
        !f.modelInput || !f.depth || !f.motion || !f.exposureContractKnown || !f.owner)
    { Invalidate(); return reject("missing-current-frame-contract"); }
    if (f.beforeUpscale != f.info->BeforeUpscale || f.finishedPicture != f.info->FinishedPicture)
    { Invalidate(); return reject("inconsistent-frame-placement"); }
    const auto phase = Phase(f);
    const auto settings = Settings(f);
    if(NrHistoryDebug::Wanted())
    {debugFrame.phase=phase.verified;debugFrame.duration=settings.durationSeconds;
     debugFrame.maxAge=settings.maxAgeSeconds;debugFrame.strength=settings.strength;
     debugFrame.depthTolerance=f.cfg->DlssNrPresentedTemporalDepthTolerance.value_or_default();
     debugFrame.colourTolerance=f.cfg->DlssNrPresentedTemporalColourTolerance.value_or_default();
     debugFrame.motionTolerance=f.cfg->DlssNrPresentedTemporalMotionTolerance.value_or_default();
     debugFrame.stabilitySeconds=settings.stabilitySeconds;
     debugFrame.stabilityFloor=f.cfg->DlssNrPresentedTemporalStabilityFloor.value_or_default();
     debugFrame.stabilityClamp=f.cfg->DlssNrPresentedTemporalStabilityClamp.value_or_default();}
    if (!phase.verified || !settings.Valid())
    { Invalidate(); return reject(!phase.verified ? phase.reason : "invalid-feedback-settings"); }
    if (!NrDetailHistoryPolicy::ValidSerial(f.frameNumber) || f.info->SourceCaptureSerial != f.frameNumber ||
        !NrDetailHistoryPolicy::ValidSerial(f.info->SourceCaptureEpoch) ||
        (!f.nativePreSrRoute && !NrDetailHistoryPolicy::ValidSerial(f.info->SceneGuideOwner[2])) ||
        (f.nativePreSrRoute && std::none_of(f.preSrOwner.begin(), f.preSrOwner.end(),
            [](uint64_t n) { return NrDetailHistoryPolicy::ValidSerial(n); })))
    { Invalidate(); return reject("unknown-source-identity"); }
    if (!ViewFits(f.modelInput, 0, 0, f.workWidth, f.workHeight) ||
        !ViewFits(f.motion, f.motionBaseX, f.motionBaseY, f.motionWidth, f.motionHeight) ||
        !ViewFits(f.depth, f.depthBaseX, f.depthBaseY, f.depthWidth, f.depthHeight))
    { Invalidate(); return reject("current-resource-view"); }
    auto* list = RealList(f.cmdList);
    auto* queue = RealQueue(f.temporalQueue);
    if (!DlssNrGpuLifetime::SameDevice(list, queue) || !DlssNrGpuLifetime::SameDevice(f.depth, queue) ||
        !DlssNrGpuLifetime::SameDevice(f.motion, queue) || !DlssNrGpuLifetime::SameDevice(f.modelInput, queue) ||
        queue->GetDesc().Type != list->GetType() || list->GetType() == D3D12_COMMAND_LIST_TYPE_COPY)
    { Invalidate(); return reject("queue-device-or-list-type"); }
    activeKey = Contract(f, phase, settings, sourceEntrySeconds);
    const bool authenticatedProducer = f.nativePreSrRoute ? f.preSrProducerQueueVerified : f.orderedNativeQueue;
    bool orderedAdvance = recordings.Advance(authenticatedProducer, list, queue, activeKey, f.frameNumber);
    if (!orderedAdvance && !f.nativePreSrRoute && f.orderedNativeQueue && recordings.Pending())
    {
        // This existing endpoint knows its private native allocator/list owners
        // and performs the real successful Reset. Never reset a borrowed list.
        DetailReuse::RetireCompletedNativeRecordings(f);
        Collect();
        orderedAdvance = recordings.Advance(authenticatedProducer, list, queue, activeKey, f.frameNumber);
    }
    // Advance retains every executable record/use/fence/owner; same-queue reads
    // and writes may proceed in order, physical reuse/retirement still requires
    // completion+Reset. No hint queue can enter this path, and no CPU wait occurs.
    if (fusedReadable || (!orderedAdvance && OwnersBusy()))
        return reject("recording-awaits-completion-and-reset");
    if (!settings.Enabled())
    {
        Invalidate();
        if (!OwnersBusy()) ParkAll();
        return reject("fresh-only-settings");
    }
    if (!f.workWidth || !f.workHeight || f.workWidth > 16384 || f.workHeight > 16384)
        return reject("invalid-working-extent");
    if (!f.pass->DetailReuseReady()) return reject("feedback-shader-unavailable");
    if (!PrepareResources(f)) return {};
    auto& slot = recordings.current;
    if (!slot.fence || resourceDevice != f.device)
    {
        slot.fence.Reset();
        if (FAILED(f.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.fence))))
            return reject("feedback-fence-unavailable");
    }
    // Fence objects are device-specific even across a resize/rebuild.
    if (!DlssNrGpuLifetime::SameDevice(slot.fence.Get(), queue))
    {
        slot.fence.Reset();
        if (FAILED(f.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.fence))))
            return reject("feedback-fence-unavailable");
    }
    const auto use = NrRecording(f.device, f.cmdList);
    if (!DlssNrGpuLifetime::Retain(use, { detail[0], detail[1], colourDepth[0], colourDepth[1],
                                         fused, stability[0], stability[1], f.modelInput, f.motion, f.depth }))
        return reject("feedback-recording-owner-unavailable");
    const auto history = NrDetailHistoryPolicy::Select(activeKey, publishedKey, historyPublished, f.modelReset, step, settings);
    if(NrHistoryDebug::Wanted())
    {debugFrame.delta=history.deltaSeconds;debugFrame.historySource=history.usable?publishedKey.sample.ordinal:0;}
    activeWrite = publishedWrite ^ 1u;
    auto fence = slot.fence;
    slot = {};
    slot.fence = std::move(fence);
    slot.commands = list; slot.commandOwner = list; slot.queue = queue; slot.resourceUse = use;
    slot.contract = activeKey; slot.ordinal = f.frameNumber; slot.generation = ++generation;
    slot.bytes = AddBytes(allocatedBytes, ParkedBytes()); slot.write = activeWrite;
    slot.pending = true; slot.ordered = authenticatedProducer;
    DlssNrWatchedLists::Add(list);

    constants = {};
    constants.WorkWidth = f.workWidth; constants.WorkHeight = f.workHeight;
    constants.MotionWidth = f.motionWidth; constants.MotionHeight = f.motionHeight;
    constants.MotionBaseX = f.motionBaseX; constants.MotionBaseY = f.motionBaseY;
    constants.DepthWidth = f.depthWidth; constants.DepthHeight = f.depthHeight;
    constants.DepthBaseX = f.depthBaseX; constants.DepthBaseY = f.depthBaseY;
    constants.DepthInverted = f.depthInverted ? 1u : 0u;
    constants.MvScaleX = f.mvScaleX; constants.MvScaleY = f.mvScaleY;
    constants.NativeMotionPhaseUvX = f.nativeMotionPhaseUvX;
    constants.NativeMotionPhaseUvY = f.nativeMotionPhaseUvY;
    constants.NativeDepthPhaseUvX = f.nativeDepthPhaseUvX;
    constants.NativeDepthPhaseUvY = f.nativeDepthPhaseUvY;
    constants.DepthTolerance = f.cfg->DlssNrPresentedTemporalDepthTolerance.value_or_default();
    constants.ClipGamma = f.cfg->DlssNrPresentedTemporalColourTolerance.value_or_default();
    constants.MotionReject = f.cfg->DlssNrPresentedTemporalMotionTolerance.value_or_default();
    constants.SigmaFloor = f.cfg->DlssNrPresentedTemporalNoiseFloor.value_or_default();
    constants.FeedbackStabilitySeconds = settings.stabilitySeconds;
    constants.FeedbackStabilityFloor = f.cfg->DlssNrPresentedTemporalStabilityFloor.value_or_default();
    constants.FeedbackStabilityClamp = f.cfg->DlssNrPresentedTemporalStabilityClamp.value_or_default();
    // The independent full-model history always carries an original residual
    // with its birth fingerprint. Legacy cadence modes keep their own ABI.
    constants.TemporalMode = 2u;
    constants.FeedbackDeltaSeconds = history.deltaSeconds;
    constants.FeedbackDurationSeconds = settings.durationSeconds;
    constants.FeedbackMaxAgeSeconds = settings.maxAgeSeconds;
    constants.FeedbackStrength = settings.strength;
    constants.FeedbackHistoryValid = history.usable ? 1u : 0u;
    constants.FeedbackRasterOffsetUvX = history.rasterOffsetUvX;
    constants.FeedbackRasterOffsetUvY = history.rasterOffsetUvY;
    activeList = list; prepared = true;
    snapshot.active = true; snapshot.historyUsable = history.usable;
    snapshot.reason = history.reason;
    snapshot.bytes = slot.bytes;
    return { true, history.usable };
}

using Inspector = void (*)(const Frame&, const DlssNrDetailReuseConstants&, const Recording&,
                          ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*);
void After(const Frame& f, bool complete, ID3D12Resource*& finalAnswer, Inspector inspect = nullptr)
{
    PublishOnExit publishObservation;
    if (f.independentFeedbackRequested && complete && finalAnswer) ++snapshot.full;
    if (!prepared || RealList(f.cmdList) != activeList || f.frameNumber != activeKey.sample.ordinal) return;
    prepared = false;
    auto& slot = recordings.current;
    if (!complete || !finalAnswer || !slot.pending || slot.submitted || slot.quarantined ||
        !DlssNrGpuLifetime::Retain(slot.resourceUse, { finalAnswer }))
    { Invalidate(); snapshot.reason = "full-model-incomplete"; return; }
    // Current proxies/depth and fresh answer are readable at this seam. Old
    // history is never sampled when FeedbackHistoryValid==0 (shader contract).
    D3D12_RESOURCE_BARRIER order {};
    order.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    f.cmdList->ResourceBarrier(1, &order);
    Barrier(f.cmdList, detail[publishedWrite], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(f.cmdList, colourDepth[publishedWrite], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (stability[publishedWrite]) Barrier(f.cmdList, stability[publishedWrite], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    constants.Mode = NrDetailHistoryPolicy::ResolveMode;
    const bool resolved = f.pass->DispatchDetailReuse(f.cmdList, constants, f.workWidth, f.workHeight,
        f.modelInput, finalAnswer, detail[publishedWrite], colourDepth[publishedWrite], f.motion,
        fused, detail[activeWrite], f.depth, colourDepth[activeWrite], stability[publishedWrite], stability[activeWrite]);
    if(resolved&&inspect)inspect(f,constants,slot,finalAnswer,detail[publishedWrite],colourDepth[publishedWrite],stability[publishedWrite]);
    if (stability[publishedWrite]) Barrier(f.cmdList, stability[publishedWrite], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(f.cmdList, colourDepth[publishedWrite], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(f.cmdList, detail[publishedWrite], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!resolved)
    {
        // Partial GPU writes remain owned by this recording. Neither output nor
        // publication can use an incomplete set; no record/token is cancelled.
        Invalidate(); snapshot.reason = "feedback-dispatch-failed"; return;
    }
    slot.publish = true;
    ++snapshot.recordedCount; snapshot.recorded = true;
    snapshot.reason = "feedback-recorded-awaiting-publication";
    Barrier(f.cmdList, fused, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    fusedReadable = true;
    finalAnswer = fused;
}
void AfterResolve(ID3D12GraphicsCommandList* list)
{
    if (!fusedReadable || RealList(list) != activeList) return;
    Barrier(list, fused, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    fusedReadable = false;
}
void ResetRecording(ID3D12CommandList* list)
{
    PublishOnExit publishObservation;
    const auto* real = RealList(list);
    if (real == activeList) prepared = false;
    recordings.Each([&](const Recording& r){if(r.pending&&r.commands==real)
        NrHistoryDebug::Lifecycle(r.ordinal,r.contract.identity.owner,r.contract.identity.revision,false,false,true,r.quarantined);});
    recordings.Reset(RealList(list), Publish, Invalidate);
    Collect();
}
void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    PublishOnExit publishObservation;
    if (!queue || !lists) return;
    // The outer hook normally supplies real objects; normalize proxy identities
    // before matching, without allocating an unbounded list copy.
    auto* realQueue = RealQueue(queue);
    for (UINT i = 0; i < count; ++i)
    {
        ID3D12CommandList* realList = RealList(lists[i]);
        recordings.Submitted(realQueue, 1, &realList, Publish, Invalidate,
            [](Recording& slot) {
                NrHistoryDebug::Lifecycle(slot.ordinal,slot.contract.identity.owner,slot.contract.identity.revision,
                                          slot.submitted,false,false,slot.quarantined);
                DlssNrWatchedLists::Add(slot.commands);
            });
    }
    Collect();
}
bool SafeToRelease()
{
    Collect();
    return !fusedReadable && !OwnersBusy();
}
unsigned PendingRecordings() { return recordings.Pending(); }
Snapshot Status()
{
    Snapshot result = snapshot;
    result.pending = recordings.Pending();
    result.published = historyPublished;
    result.historySource = historyPublished ? publishedKey.sample.ordinal : 0;
    return result; // fixed-size observation; never read by Before/After admission
}
Snapshot Published()
{
    // Menu readers take neither NR nor Config locks. Every field, including the
    // static reason pointer, belongs to this one fixed-size coherent snapshot.
    std::lock_guard<std::mutex> lock(publishedMutex);
    return publishedSnapshot;
}
void Release()
{
    PublishOnExit publishObservation;
    // Be safe even if an integration caller forgets the separate query. A
    // completed fence without executable-generation closure is NOT sufficient.
    if (!SafeToRelease()) { Invalidate(); return; }
    ParkAll();
    recordings = {}; sources.Reset();
    activeKey = {}; publishedKey = {}; constants = {};
    generation = publishedGeneration = 0;
    publishedWrite = activeWrite = 0; activeList = nullptr;
    prepared = fusedReadable = false;
    snapshot = {};
    // Keep accounting for parked allocations until the existing retirement
    // collector actually releases them, including across an off/on transition.
}
} // namespace DetailHistory
