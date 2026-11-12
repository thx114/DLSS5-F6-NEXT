// Reuse detail between frames, D3D12 (menu: "Reuse detail between frames"; ini DetailReuse*).
//
// Every other frame (or two of every three, DetailReuseInterval 3) skips the model -- all passes -- and moves the
// previous frame's detail (model answer minus model input, in the proxy domain at the working size) onto this frame's
// input instead; the resolve then composes it like any model answer. The spread (DetailReuseSpread) blends what was
// shown before a full frame into it and the reused frames after it, on the display only. The cadence is
// dlssnr/DlssNrDetailReuse.h, the GPU work precompile/dlssnr_detail_reuse.hlsl, and what does not depend on the API
// (when it may run, the constants, the status) dlssnr/DlssNrDetailReuseHost.h.
// NR after SR only; frames that must see the real model (reset, settings change, Tune, frame hold) run it, and none of
// it runs while frame generation makes frames unless asked to (A/B testing): generated frames are built from real
// ones, and alternating full and reused frames can flicker under it.
//
// Included inside DlssNr_Dx12.cpp's anonymous namespace, after CreateScratch / ParkNrResource / Barrier. Dispatch calls
// BeforeModel and AfterModel around its pass loop and AfterResolve after the resolve, all under g_nrMutex. Its own
// state lives here; the textures exist only while it is on.
#include <dlssnr/DlssNrReuseBudget.h>
#include <dlssnr/DlssNrOrderedReuse.h>

namespace DetailReuse
{
// The shader constants are 4-byte scalars followed by alignment padding. Never
// compare the unspecified tail padding as part of an otherwise exact contract.
constexpr size_t kEncodedContractBytes = offsetof(DlssNrConstants, OriginalIsGameColour) + sizeof(uint32_t);
// What Dispatch knows about this frame: the shared facts, and the D3D12 objects.
struct Frame : DlssNrDetailReuse::HostFrame
{
    DlssNr_Dx12* pass = nullptr;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* temporalQueue = nullptr; // real ordered present queue identity, never dereferenced here
    const DlssNrFrameInfo* info = nullptr;
    DXGI_FORMAT answerFormat = DXGI_FORMAT_UNKNOWN; // the model answers' format
    ID3D12Resource* modelInput = nullptr;           // readable
    ID3D12Resource* motion = nullptr;               // readable
    ID3D12Resource* depth = nullptr;                // readable
    ID3D12Resource* output = nullptr;               // the first model answer's texture, at rest (UAV)
    bool orderedPictureQueue = false; // authenticated local final-picture producer, never a queue hint
    bool orderedNativeQueue = false;  // exact private native endpoint + verified guides + actual submission queue
    std::array<uint64_t, 6> pictureProducer {}; // exact stable scope owner, NOT this picture's ordinal
    uintptr_t owner = 0; // local feature owner; never an ABI/provider hook
    std::array<uint64_t, 8> exposureContract {};
    std::array<unsigned char, kEncodedContractBytes> encodedTransform {};
    bool exposureContractKnown = false;
    // Model-only scene-depth layout belongs to the model answer contract.
    // The separate versioned sceneGuideContract can admit a conditional geometry
    // veto; neither contract promotes scene depth to general picture-depth.
    std::array<uint64_t, 10> pictureModelDepthContract {};
    std::array<uint64_t, 23> sceneGuideContract {};
};

// What BeforeModel decided.
struct Plan
{
    bool active = false;     // reuse runs on this route now (the bottleneck reuse is then off)
    bool temporal = false;   // full-refresh feedback can use depth or explicit picture-flow storage
    bool reused = false;     // the model is skipped: the answer is in Frame::output, still writable
    bool resetModel = false; // start the model's history over (it last ran two frames ago, without composed vectors)
    ID3D12Resource* motion = nullptr; // the vectors the model evaluates with, and their subrect origin
    unsigned int motionBaseX = 0, motionBaseY = 0;
};

DlssNrDetailReuse::Host host { "DLSS-NR detail reuse" };
DlssNrDetailReuse::Cadence& cadence = host.cadence;
DlssNrDetailReuse::Decision& decision = host.decision;
DlssNrDetailReuseConstants& params = host.params;
DlssNrDetailReuse::InputContinuity observedInputs;

// Two sets alternate except authenticated interval-1 picture feedback: all old reads finish before capture.
ID3D12Resource* detail[2] = {};      // work size, RGBA16F: answer - input, a = valid
ID3D12Resource* colourDepth[2] = {}; // work size, RGBA32F: that frame's input colour, a = far-is-zero depth
                                     // (32-bit: far reversed-Z values fall below half precision's range)
unsigned int cur = 0;
bool singleHistory = false; // never used by skipped-model intervals or Spread
// work size: the reused frames' vectors as uv displacement, back to the last full frame. Two at every 3rd frame, so the
// second reused frame in a row can add its own to what the first kept (savedMotion is the one holding them); the
// second exists only then.
ID3D12Resource* prevMotion[2] = {};
unsigned int savedMotion = 0;
// work size, RGBA16F: the detail a spread frame showed (answer - input, a = valid), for the spread's next frame to move
ID3D12Resource* shownDetail = nullptr;
bool shownValid = false;
ID3D12Resource* composed = nullptr;   // the motion texture's allocation size: raw vectors over two frames
ID3D12Resource* estimate = nullptr;   // work size: moved detail with its trust (Fill and Steady)
ID3D12Resource* steadied = nullptr;   // work size, the answers' format: the steadied answer (Steady)
DXGI_FORMAT steadiedFormat = DXGI_FORMAT_UNKNOWN;
ID3D12Resource* temporalFused = nullptr; // independent from display Spread's steadied buffer
DXGI_FORMAT temporalFusedFormat = DXGI_FORMAT_UNKNOWN;
bool temporalFusedReadable = false, temporalEstimateReady = false;
bool temporalFusionRecorded = false; // successful recording, not GPU completion/pixel proof
unsigned int workWidth = 0, workHeight = 0, composedWidth = 0, composedHeight = 0;
bool composedReadable = false, steadiedReadable = false; // this frame, between the calls
bool measureWithSteady = false; // a held frame whose steadiness pass carries the coverage measurement
// Shared reuse scratch stays on one authenticated queue while a bounded chain is active.
// GPU reads/writes are queue ordered; CPU descriptors/constants retain their existing exact-fence
// slots. Physical retirement records below are never replaced by logical history publication.
// Unauthenticated routes keep the original completion-only admission; Present never waits.
struct HistoryContract
{
    std::array<uint64_t, 66> values {};
    // Native guide convention/flags and source geometry, independent of the current sample's J.
    std::array<uint64_t, 16> nativeGuideContract {};
    std::array<uint64_t, 10> pictureModelDepthContract {};
    std::array<uint64_t, 23> sceneGuideContract {};
    std::array<unsigned char, kEncodedContractBytes> encodedTransform {};
    bool operator==(const HistoryContract& other) const
    { return values == other.values && nativeGuideContract == other.nativeGuideContract &&
        pictureModelDepthContract == other.pictureModelDepthContract && sceneGuideContract == other.sceneGuideContract &&
        encodedTransform == other.encodedTransform; }
};
uint64_t FloatBits(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
// This selects a history-key equivalence, not route admission. NativeGuidePhase::Resolve and
// Host::Gate still authenticate every frame, including actual normalization, depth and vector decoding.
// TemporalCapture stores H_n[u] on canonical W: colour/latent at u, depth D_n(u + J_n/R).
// Next frame reads nonjittered MV at u + J_(n+1)/R, then H_n[u + delta]. The saved depth
// already contains J_n's address conversion, so neither a previous-J offset nor equal J is needed.
// Only that exact convention may vary J without changing the key. Unknown/other conventions
// keep the original exact sample bits; in particular this does not admit jittered MV/cancellation.
bool CanonicalNativeHistoryPhase(const Frame& f)
{
    if (!f.nativeTemporalRouteRequested || !DlssNrDetailReuse::NativeGuideAuthority(f) || f.info == nullptr)
        return false;
    const auto& native = *f.info;
    if (!native.NativeSceneRoute || !native.FinishedPicture || native.BeforeUpscale ||
        !native.NativeRouteConventionVerified || !native.NativeGuideMetadataKnown ||
        !native.NativeFeatureFlagsKnown || !native.NativeCurrentJitterKnown || !native.NativeMotionJitterKnown ||
        native.NativeMotionVectorsJittered || !native.MotionVectorsLowResolution ||
        !native.RenderSubrectWidth || !native.RenderSubrectHeight ||
        f.motionWidth != native.RenderSubrectWidth || f.motionHeight != native.RenderSubrectHeight ||
        f.depthWidth != native.RenderSubrectWidth || f.depthHeight != native.RenderSubrectHeight ||
        !std::isfinite(native.NativeJitterX) || !std::isfinite(native.NativeJitterY) ||
        std::abs(native.NativeJitterX) > 0.5f || std::abs(native.NativeJitterY) > 0.5f)
        return false;
    const float phaseX = native.NativeJitterX / native.RenderSubrectWidth;
    const float phaseY = native.NativeJitterY / native.RenderSubrectHeight;
    return f.nativeMotionPhaseUvX == phaseX && f.nativeMotionPhaseUvY == phaseY &&
        f.nativeDepthPhaseUvX == phaseX && f.nativeDepthPhaseUvY == phaseY &&
        f.nativeMotionJitterCancelUvX == 0.0f && f.nativeMotionJitterCancelUvY == 0.0f;
}
HistoryContract Contract(const Frame& f)
{
    HistoryContract key;
    key.encodedTransform = f.encodedTransform;
    key.pictureModelDepthContract = f.pictureModelDepthContract;
    key.sceneGuideContract = f.sceneGuideContract;
    const bool canonicalPhase = CanonicalNativeHistoryPhase(f);
    if (f.nativeTemporalRouteRequested && f.info != nullptr)
    {
        const auto& native = *f.info;
        // Convention 1: canonical render-pixel phase, nonjittered MV, zero cancel, W-grid history.
        // Keep original source rects even when row normalization rebases the effective guides to zero.
        const uint64_t coordinateControls = (f.cfg->DlssNrDepthTextureFlipY.value_or_default() ? 1ull : 0ull) |
            (f.cfg->DlssNrMotionTextureFlipY.value_or_default() ? 2ull : 0ull) |
            (f.cfg->DlssNrMotionYInverted.value_or_default() ? 4ull : 0ull);
        key.nativeGuideContract = { canonicalPhase ? 1ull : 0ull, native.NativeFeatureFlags,
            native.RenderSubrectWidth, native.RenderSubrectHeight, native.OutputWidth, native.OutputHeight,
            native.DepthSubrectBaseX, native.DepthSubrectBaseY, native.MotionSubrectBaseX, native.MotionSubrectBaseY,
            FloatBits(native.MvScaleX), FloatBits(native.MvScaleY), native.RayReconstruction ? 1ull : 0ull,
            native.MotionIsPictureFlow ? 1ull : 0ull, native.DepthIsPictureAligned ? 1ull : 0ull, coordinateControls };
    }
    key.values = { f.owner, reinterpret_cast<uintptr_t>(f.device), reinterpret_cast<uintptr_t>(f.temporalQueue),
        f.revision, f.workWidth, f.workHeight, static_cast<uint64_t>(f.answerFormat),
        f.motionWidth, f.motionHeight, f.motionBaseX, f.motionBaseY, f.motionAllocWidth, f.motionAllocHeight,
        f.depthWidth, f.depthHeight, f.depthBaseX, f.depthBaseY, f.depthInverted ? 1ull : 0ull,
        FloatBits(f.mvScaleX), FloatBits(f.mvScaleY), f.reversibleMode, f.passthrough ? 1ull : 0ull,
        f.nativeTemporalRouteRequested ? 1ull : 0ull, f.temporalRequested ? 1ull : 0ull,
        // Per-frame addressing remains live in Host::Decide -> HLSL. Only the certified
        // canonical convention above omits J from cross-frame compatibility, never from sampling.
        canonicalPhase ? 0ull : FloatBits(f.nativeMotionPhaseUvX),
        canonicalPhase ? 0ull : FloatBits(f.nativeMotionPhaseUvY),
        canonicalPhase ? 0ull : FloatBits(f.nativeDepthPhaseUvX),
        canonicalPhase ? 0ull : FloatBits(f.nativeDepthPhaseUvY),
        FloatBits(f.nativeMotionJitterCancelUvX), FloatBits(f.nativeMotionJitterCancelUvY),
        f.exposureContract[0], f.exposureContract[1], f.exposureContract[2], f.exposureContract[3],
        f.exposureContract[4], f.exposureContract[5], f.exposureContract[6], f.exposureContract[7],
        (f.orderedPictureQueue ? 1ull : 0ull) | (f.orderedNativeQueue ? 2ull : 0ull),
        f.cfg->DlssNrDetailReuse.value_or_default() ? 1ull : 0ull,
        f.cfg->DlssNrDetailReuseInterval.value_or_default(), f.cfg->DlssNrDetailReuseSpread.value_or_default(),
        f.cfg->DlssNrDetailReuseDebug.value_or_default() ? 1ull : 0ull,
        FloatBits(f.cfg->DlssNrDetailReuseMaxDropped.value_or_default()),
        f.cfg->DlssNrDetailReuseWithFg.value_or_default() ? 1ull : 0ull,
        FloatBits(f.cfg->DlssNrDetailReuseMinFps.value_or_default()),
        f.pictureProducer[0], f.pictureProducer[1], f.pictureProducer[2], f.pictureProducer[3],
        f.pictureProducer[4], f.pictureProducer[5],
        std::min(f.cfg->DlssNrPresentedTemporalDurationMs.value_or_default(), Config::kPresentedTemporalDurationMaxMs),
        f.pictureTemporalRouteRequested ? 1ull : 0ull, f.pictureGuideContractVerified ? 1ull : 0ull,
        f.pictureGuideEpoch, f.pictureGuideCutSerial,
        (f.pictureFlowFeedbackVerified ? 1ull : 0ull) | (f.pictureFlowReuseVerified ? 2ull : 0ull) |
            (f.lightEditFeedback ? 4ull : 0ull) | (f.sceneGeometryVerified ? 8ull : 0ull),
        std::clamp(f.cfg->DlssNrPresentedTemporalMaxAgeMs.value_or_default(),
            Config::kPresentedTemporalMaxAgeMinMs, Config::kPresentedTemporalMaxAgeMaxMs),
        FloatBits(f.cfg->DlssNrPresentedTemporalStrength.value_or_default()) };
    return key;
}
// The fixture calls this same product lifecycle with real D3D12 objects.
using TemporalRecording = DlssNrGpuLifetime::HistoryRecord<HistoryContract>;
DlssNrGpuLifetime::OrderedHistory<HistoryContract> orderedHistory;
TemporalRecording& temporalRecording = orderedHistory.current;
auto& orderedRetirements = orderedHistory.retained;
UINT64 publishedGeneration = 0;
bool publishedQueueOrdered = false;
template <class F> void EachRecording(F&& visit) { orderedHistory.Each(std::forward<F>(visit)); }
unsigned PendingRecordings() { return orderedHistory.Pending(); }
HistoryContract publishedContract;
UINT64 publishedOrdinal = 0, publishedFence = 0, recordingGeneration = 0;
// A fence value belongs to its published writer, not the next recording's fence.
Microsoft::WRL::ComPtr<ID3D12Fence> publishedFenceOwner;
uintptr_t sourceFenceIdentity = 0; // same observation as reuseEvidence.sourceFence/Completed
const char* historyReadiness = "no-history";
const char* recordingReadiness = "no-recording";
// Defined beside Late's private slots in this TU; never resets a borrowed list.
void RetireCompletedNativeRecordings(const Frame& f);
bool historyPublished = false;
DlssNrCadence::NrCadenceLedger ledger;
DlssNrCadence::ReuseDecision reuseEvidence;
constexpr uint64_t kHistoryByteCap = DlssNrReuseBudget::Limit;

// A parked resource is still allocated until the existing GPU retirement list
// actually releases it. Track identity/bytes only; ownership and fence proof
// remain exclusively with ParkNrResource / TickNrRetired.
struct RetiredBudget { ID3D12Resource* identity; uint64_t bytes; };
std::vector<RetiredBudget> retiredBudget;
struct AllocationBudget
{
    ID3D12Device* device;
    D3D12_RESOURCE_DESC desc;
    uint64_t bytes;
};
std::vector<AllocationBudget> allocationBudget;
ID3D12Device* budgetDevice = nullptr;
uint64_t AllocationBytes(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc)
{
    if (device == nullptr) return DlssNrReuseBudget::Invalid;
    for (const auto& entry : allocationBudget)
        if (entry.device == device && entry.desc.Dimension == desc.Dimension &&
            entry.desc.Width == desc.Width && entry.desc.Height == desc.Height &&
            entry.desc.Format == desc.Format && entry.desc.Flags == desc.Flags &&
            entry.desc.Layout == desc.Layout && entry.desc.Alignment == desc.Alignment &&
            entry.desc.DepthOrArraySize == desc.DepthOrArraySize && entry.desc.MipLevels == desc.MipLevels &&
            entry.desc.SampleDesc.Count == desc.SampleDesc.Count && entry.desc.SampleDesc.Quality == desc.SampleDesc.Quality)
            return entry.bytes;
    const auto info = device->GetResourceAllocationInfo(0, 1, &desc);
    const uint64_t bytes = info.SizeInBytes && info.Alignment &&
        info.SizeInBytes != UINT64_MAX ? info.SizeInBytes : DlssNrReuseBudget::Invalid;
    if (allocationBudget.size() >= 64) allocationBudget.clear();
    allocationBudget.push_back({ device, desc, bytes });
    return bytes;
}
void ParkReuseResource(ID3D12Resource*& resource)
{
    if (resource == nullptr) return;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    const auto bytes = SUCCEEDED(resource->GetDevice(IID_PPV_ARGS(&device)))
        ? AllocationBytes(device.Get(), resource->GetDesc()) : DlssNrReuseBudget::Invalid;
    retiredBudget.push_back({ resource, bytes });
    ParkNrResource(resource);
}
// Only a cheap pending marker at the Present endpoint. RetiredBytes prunes
// identities during the existing budget decision; GPU completion stays there.
bool HasParkedAllocations() { return !retiredBudget.empty(); }
uint64_t RetiredBytes()
{
    uint64_t bytes = 0;
    for (auto it = retiredBudget.begin(); it != retiredBudget.end();)
    {
        const bool retained = std::any_of(g_nrRetired.begin(), g_nrRetired.end(),
            [&](const NrRetired& r) { return r.resource == it->identity; });
        if (!retained) it = retiredBudget.erase(it);
        else { bytes = DlssNrReuseBudget::Add(bytes, it->bytes); ++it; }
    }
    return bytes;
}
ID3D12CommandQueue* temporalHistoryQueue = nullptr;
ID3D12CommandList* RealTemporalCommands(ID3D12GraphicsCommandList* cmd)
{
    ID3D12GraphicsCommandList* real=nullptr;
    return Util::CheckForRealObject(__FUNCTION__, cmd, (IUnknown**)&real) ? real : cmd;
}
// Retain exact COM objects: an address reused by a new list is not an owner witness.
struct ProducerOwner
{
    Microsoft::WRL::ComPtr<ID3D12CommandList> commands;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
};
std::array<ProducerOwner, 8> producerOwners {};
unsigned producerOwnerNext = 0;
ID3D12CommandList* RealSubmittedCommands(ID3D12CommandList* cmd)
{
    ID3D12CommandList* real = nullptr;
    return cmd != nullptr && Util::CheckForRealObject(__FUNCTION__, cmd, (IUnknown**)&real) ? real : cmd;
}
bool SameDevice(ID3D12DeviceChild* a, ID3D12DeviceChild* b)
{
    if (a == nullptr || b == nullptr) return false;
    Microsoft::WRL::ComPtr<ID3D12Device> da, db;
    if (FAILED(a->GetDevice(IID_PPV_ARGS(&da))) || FAILED(b->GetDevice(IID_PPV_ARGS(&db)))) return false;
    ID3D12Device* ra = da.Get(); ID3D12Device* rb = db.Get();
    ID3D12Device* unwrapped = nullptr;
    if (Util::CheckForRealObject(__FUNCTION__, ra, (IUnknown**)&unwrapped)) ra = unwrapped;
    if (Util::CheckForRealObject(__FUNCTION__, rb, (IUnknown**)&unwrapped)) rb = unwrapped;
    Microsoft::WRL::ComPtr<IUnknown> ia, ib;
    return SUCCEEDED(ra->QueryInterface(IID_PPV_ARGS(&ia))) &&
        SUCCEEDED(rb->QueryInterface(IID_PPV_ARGS(&ib))) && ia.Get() == ib.Get();
}
ID3D12CommandQueue* ProvenProducerQueue(ID3D12GraphicsCommandList* cmd, ID3D12CommandQueue* hint)
{
    const auto* real = RealTemporalCommands(cmd);
    for (const auto& owner : producerOwners)
        if (owner.commands.Get() == real && SameDevice(owner.commands.Get(), owner.queue.Get()))
            return owner.queue.Get();
    return hint;
}
void RememberProducer(ID3D12CommandList* cmd, ID3D12CommandQueue* queue)
{
    for (auto& owner : producerOwners)
        if (owner.commands.Get() == cmd) { owner.queue = queue; return; }
    auto& owner = producerOwners[producerOwnerNext++ % producerOwners.size()];
    owner.commands = cmd; owner.queue = queue;
}
void DiscardTemporalRecording()
{
    host.DiscardTemporalRecording(); shownValid=false; temporalHistoryQueue=nullptr;
    historyPublished = false;
    publishedFenceOwner.Reset();
    EachRecording([](TemporalRecording& slot) { slot.publish = false; });
}

void PublishHistory(TemporalRecording& slot, bool queueOrdered)
{
    DlssNrGpuLifetime::PublishHistoryRecord(slot, queueOrdered, publishedGeneration,
        [](const TemporalRecording& accepted, bool ordered)
        {
            cur = accepted.write; shownValid = accepted.shown;
            publishedContract = accepted.contract; publishedOrdinal = accepted.ordinal;
            publishedFence = accepted.value; publishedQueueOrdered = ordered;
            publishedFenceOwner = accepted.fence;
            historyPublished = true; temporalHistoryQueue = accepted.queue;
            cadence.Captured(true); // logical publication; never resource retirement
        });
}

void CollectHistory()
{
    orderedHistory.Collect(PublishHistory, DiscardTemporalRecording);
}

bool AdvanceOrderedRecording(const Frame& f)
{
    return orderedHistory.Advance(f.orderedPictureQueue || f.orderedNativeQueue, RealTemporalCommands(f.cmdList),
                                  f.temporalQueue, Contract(f), f.frameNumber);
}

// A newer current record can finish/close before an older idle private list is
// reset. Collect then leaves current empty while retained still pins that list's
// executable generation. These are GPU-completed owners, not an active writer.
// Authenticate this recovery separately: it grants recording space only, never
// adjacent-source history, publication, generation closure or owner retirement.
bool CompletedPictureRetirements(const Frame& f)
{
    if (temporalRecording.pending || !f.pictureFlowReuseVerified || !f.orderedPictureQueue ||
        f.nativeTemporalRouteRequested || f.vulkan || f.unsupportedVulkanGameOrigin ||
        f.temporalQueue == nullptr)
        return false;
    auto* nextList = RealTemporalCommands(f.cmdList);
    if (nextList == nullptr || !SameDevice(nextList, f.temporalQueue) ||
        nextList->GetType() != f.temporalQueue->GetDesc().Type)
        return false;
    bool retained = false;
    for (const auto& slot : orderedRetirements)
    {
        if (!slot.pending) continue;
        retained = true;
        if (!slot.ordered || !slot.submitted || !slot.retirementProven ||
            !DlssNrGpuLifetime::OrderedHistory<HistoryContract>::Healthy(slot) ||
            !slot.fence || !slot.value || slot.value == UINT64_MAX ||
            slot.queue != f.temporalQueue || slot.retirementQueue.Get() != f.temporalQueue ||
            slot.commands == nextList || !SameDevice(slot.commandOwner.Get(), f.temporalQueue) ||
            slot.commandOwner->GetType() != nextList->GetType() || slot.ordinal >= f.frameNumber)
            return false;
        const auto completed = slot.fence->GetCompletedValue();
        if (completed == UINT64_MAX || completed < slot.value ||
            slot.resourceUse->DeviceLost() || !slot.resourceUse->GpuComplete())
            return false;
    }
    // Leave every Record/Use/fence/owner untouched. Submit/reset hooks continue
    // watching executable generations; unknown or repeated Execute quarantines.
    // A retired record's old key is not borrowed as new history authority:
    // matchingHistory below still rejects source gaps, reset and contract changes.
    return retained;
}

// Display-only output: debug colours must never replace the answer captured into temporal history.
// At rest this is UAV, like steadied; only the resolve reads it, between AfterModel and AfterResolve.
ID3D12Resource* debugAnswer = nullptr;
DXGI_FORMAT debugFormat = DXGI_FORMAT_UNKNOWN;
bool debugReady = false, debugReadable = false, debugFailed = false;

void ParkDebug()
{
    ParkReuseResource(debugAnswer);
    debugFormat = DXGI_FORMAT_UNKNOWN;
    debugReady = debugReadable = debugFailed = false;
}

// How much of a frame arrived with no detail to move, for DlssNrDetailReuse::MotionGuard: the Coverage pass sums the
// trust over a small grid, which is copied to a readback buffer and read a few frames later, when the GPU is certainly
// past it. Each slot keeps its own fence, signalled from FinishedPictureSubmitted (the real ExecuteCommandLists) like
// DlssNrGpuTime's samples: recording a copy is not executing it, and two queues could signal one shared fence out of
// order.
constexpr unsigned int kCoverageTiles = kDlssNrDetailReuseCoverageTiles;
constexpr unsigned int kCoverageRowBytes = kCoverageTiles * 2 * sizeof(float); // 32 texels of RG32F = 256 bytes exactly
constexpr unsigned int kCoverageBytes = kCoverageRowBytes * kCoverageTiles;
constexpr unsigned int kCoverageSlots = 8;
static_assert(kCoverageRowBytes % 256 == 0, "a texture-to-buffer copy needs rows a multiple of 256 bytes");

ID3D12Resource* coverage = nullptr; // kCoverageTiles^2, RG32F: (sum of 1 - trust, pixels measured) per tile

struct CoverageSlot
{
    ID3D12Resource* buffer = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    ID3D12CommandList* commands = nullptr; // identity only
    UINT64 value = 0;
    bool pending = false;   // a copy was recorded into it
    bool submitted = false; // and the list it was recorded on was executed
};

CoverageSlot coverageSlots[kCoverageSlots];
UINT64 coverageValue = 0;
// Coverage's own allocation failure: not host.AllocationFailed, which also turns off Fill and steadiness (they share
// the estimate texture, and losing fill is a visible loss for a measurement nobody asked for).
bool coverageFailed = false;

void ParkExtras()
{
    ParkReuseResource(estimate);
    ParkReuseResource(steadied);
    ParkReuseResource(temporalFused);
    temporalFusedFormat = DXGI_FORMAT_UNKNOWN;
    temporalFusedReadable = temporalEstimateReady = false;
    steadiedFormat = DXGI_FORMAT_UNKNOWN;
}

// The coverage grid and its readback slots. A slot with a copy in flight is parked, not released; its fence is kept,
// since a Signal recorded against it may still be outstanding (DlssNrGpuTime and Late::slots keep theirs for their
// object's lifetime too). Only Release lets the fences go.
void ParkCoverage()
{
    ParkReuseResource(coverage);
    for (CoverageSlot& slot : coverageSlots)
    {
        ParkReuseResource(slot.buffer);
        slot.commands = nullptr;
        slot.pending = slot.submitted = false;
    }
}

// Built the first frame the measurement is wanted; without them reuse simply never pauses.
bool MakeCoverage(ID3D12Device* device)
{
    coverage = CreateScratch(device, DXGI_FORMAT_R32G32_FLOAT, kCoverageTiles, kCoverageTiles);

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kCoverageBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    bool ok = coverage != nullptr;
    for (CoverageSlot& slot : coverageSlots)
    {
        ok = ok && SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                            IID_PPV_ARGS(&slot.buffer)));
        // A fence a park kept is reused; its value only ever goes up, so an old Signal cannot read as a new one.
        if (ok && slot.fence == nullptr)
            ok = SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.fence)));
    }
    if (!ok)
    {
        ParkCoverage();
        coverageFailed = true;
        LOG_WARN("DLSS-NR detail reuse: could not allocate the coverage grid; it will not pause on fast motion");
    }
    return ok;
}

// Every finished copy: the share of that frame with no detail to move, to the guard. Called once a frame, before a new
// copy is recorded, so a slot is never read while the GPU may still be writing it.
void CollectCoverage()
{
    for (CoverageSlot& slot : coverageSlots)
    {
        if (!slot.pending || !slot.submitted || slot.buffer == nullptr || slot.fence == nullptr)
            continue;
        const UINT64 completed = slot.fence->GetCompletedValue();
        if (completed == UINT64_MAX || completed < slot.value)
            continue;

        void* mapped = nullptr;
        D3D12_RANGE range { 0, kCoverageBytes };
        if (SUCCEEDED(slot.buffer->Map(0, &range, &mapped)) && mapped != nullptr)
        {
            double dropped = 0.0, pixels = 0.0;
            const auto* row = static_cast<const char*>(mapped);
            for (unsigned int y = 0; y < kCoverageTiles; ++y, row += kCoverageRowBytes)
            {
                const auto* tile = reinterpret_cast<const float*>(row);
                for (unsigned int x = 0; x < kCoverageTiles; ++x)
                {
                    // A tile whose numbers are not finite is left out rather than poisoning the frame's share.
                    if (std::isfinite(tile[2 * x]) && std::isfinite(tile[2 * x + 1]))
                    {
                        dropped += tile[2 * x];
                        pixels += tile[2 * x + 1];
                    }
                }
            }
            D3D12_RANGE none { 0, 0 };
            slot.buffer->Unmap(0, &none);
            if (pixels > 0.0)
                host.RecordDropped((float) std::clamp(dropped / pixels, 0.0, 1.0));
        }
        slot.pending = slot.submitted = false;
        slot.commands = nullptr;
    }
}

// The grid into a free readback slot. Recorded right after a Coverage dispatch, with the grid still a UAV.
void CopyCoverage(ID3D12GraphicsCommandList* cmdList)
{
    CoverageSlot* free = nullptr;
    for (CoverageSlot& slot : coverageSlots)
        if (!slot.pending && slot.buffer != nullptr && slot.fence != nullptr)
        {
            free = &slot;
            break;
        }
    if (free == nullptr) // every slot in flight: this frame is not measured, which the guard reads as no reading
        return;

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = coverage;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = free->buffer;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kCoverageTiles;
    dst.PlacedFootprint.Footprint.Height = kCoverageTiles;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kCoverageRowBytes;

    Barrier(cmdList, coverage, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(cmdList, coverage, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // The list the hook will report is the real one, not a Streamline proxy: DlssNrGpuTime::Start and Late::Arm both
    // translate it the same way, and without it no slot ever matches and the measurement quietly dies.
    free->commands = cmdList;
    ID3D12GraphicsCommandList* real = nullptr;
    if (Util::CheckForRealObject(__FUNCTION__, cmdList, (IUnknown**) &real))
        free->commands = real;
    DlssNrWatchedLists::Add(free->commands); // the submit/reset hooks must hear about this list
    free->pending = true;
    free->submitted = false;
}

// A command list a copy was recorded on was reset without being executed: the slot is free again, and no fence was
// ever signalled for it. Called from FinishedPictureResetCommandList, as DlssNrGpuTime::ResetRecording is.
void ResetRecording(ID3D12CommandList* cmdList)
{
    orderedHistory.Reset(cmdList, PublishHistory, DiscardTemporalRecording);
    for (CoverageSlot& slot : coverageSlots)
        if (slot.pending && !slot.submitted && slot.commands == cmdList)
        {
            slot.pending = false;
            slot.commands = nullptr;
        }
}

// The command list a copy was recorded on has been executed: its slot's fence may be signalled. Called from
// FinishedPictureSubmitted, under g_nrMutex, after the game's own ExecuteCommandLists.
void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    if (queue == nullptr || lists == nullptr)
        return;
    ID3D12CommandQueue* realQueue = queue;
    ID3D12CommandQueue* unwrappedQueue = nullptr;
    if (Util::CheckForRealObject(__FUNCTION__, queue, (IUnknown**) &unwrappedQueue))
        realQueue = unwrappedQueue;
    orderedHistory.Submitted(realQueue, count, lists, PublishHistory, DiscardTemporalRecording,
        [&](TemporalRecording& slot)
        {
            if (!slot.quarantined && slot.retirementProven) RememberProducer(slot.commands, realQueue);
            for (auto& evidence : ledger.decisions)
                if (evidence.identity.listGeneration == slot.generation &&
                    evidence.identity.list == reinterpret_cast<uintptr_t>(slot.commands))
                    evidence.targetFence = slot.value;
            DlssNrWatchedLists::Add(slot.commands);
        });
    for (CoverageSlot& slot : coverageSlots)
    {
        if (!slot.pending || slot.submitted || slot.fence == nullptr)
            continue;
        for (UINT i = 0; i < count; ++i)
            if (lists[i] == slot.commands)
            {
                slot.value = ++coverageValue;
                slot.submitted = true;
                // A failed signal would leave the slot waiting for ever: give it up instead, unread.
                if (FAILED(queue->Signal(slot.fence.Get(), slot.value)))
                    slot.pending = slot.submitted = false;
                break;
            }
    }
}

void ParkComposed()
{
    ParkReuseResource(composed);
    composedWidth = composedHeight = 0;
}

void ParkAll()
{
    // BeforeModel refuses to enter Prepare while the slot is pending. Retain its
    // fence and monotonic target across ordinary resize/off cycles.
    DiscardTemporalRecording();
    for (unsigned int i = 0; i < 2; ++i)
    {
        ParkReuseResource(detail[i]);
        ParkReuseResource(colourDepth[i]);
    }
    ParkReuseResource(prevMotion[0]);
    ParkReuseResource(prevMotion[1]);
    savedMotion = 0;
    ParkReuseResource(shownDetail);
    shownValid = false;
    ParkComposed();
    ParkExtras();
    ParkCoverage();
    ParkDebug();
    workWidth = workHeight = 0;
    singleHistory = false;
    cadence.Drop();
}

// Match CreateScratch and MakeCoverage descriptors exactly. Committed buffers
// include device allocation alignment, rather than their logical copy byte count.
uint64_t SelectedBytes(const Frame& f, const DlssNrReuseBudget::Layout& layout)
{
    return DlssNrReuseBudget::Required(layout, [&](DlssNrReuseBudget::Kind kind, unsigned w, unsigned h)
    {
        D3D12_RESOURCE_DESC desc {};
        const bool readback = kind == DlssNrReuseBudget::Kind::Readback;
        desc.Dimension = readback ? D3D12_RESOURCE_DIMENSION_BUFFER : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w; desc.Height = h;
        desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1;
        desc.Layout = readback ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR : D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = readback ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        switch (kind)
        {
        case DlssNrReuseBudget::Kind::Detail: desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
        case DlssNrReuseBudget::Kind::ColourDepth: desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
        case DlssNrReuseBudget::Kind::Answer: desc.Format = f.answerFormat; break;
        case DlssNrReuseBudget::Kind::Readback: desc.Format = DXGI_FORMAT_UNKNOWN; break;
        default: desc.Format = DXGI_FORMAT_R32G32_FLOAT; break;
        }
        return AllocationBytes(f.device, desc);
    });
}

// Before allocating, park everything not in the selected recipe. All survivors
// are a subset of SelectedBytes; parked old allocations are counted separately.
// No immediate release, waits, new fence authority or in-flight slot edits.
void TrimForBudget(const Frame& f, const DlssNrReuseBudget::Layout& layout)
{
    if (budgetDevice != f.device || workWidth != f.workWidth || workHeight != f.workHeight ||
        detail[0] == nullptr || singleHistory != layout.singleHistory)
    {
        ParkAll();
        allocationBudget.clear();
        budgetDevice = f.device;
    }
    if (layout.singleHistory)
    {
        ParkReuseResource(prevMotion[0]);
        ParkComposed();
    }
    else if (composedWidth != layout.motionWidth || composedHeight != layout.motionHeight) ParkComposed();
    if (!layout.secondMotion && prevMotion[1] != nullptr)
    {
        ParkReuseResource(prevMotion[1]); savedMotion = 0; cadence.Drop();
    }
    if (!layout.estimate) ParkReuseResource(estimate);
    if (!layout.steadied || steadiedFormat != f.answerFormat)
    { ParkReuseResource(steadied); steadiedFormat = DXGI_FORMAT_UNKNOWN; }
    const auto fusedFormat = layout.fusedRgba16 ? DXGI_FORMAT_R16G16B16A16_FLOAT : f.answerFormat;
    if (!layout.fused || temporalFusedFormat != fusedFormat)
    { ParkReuseResource(temporalFused); temporalFusedFormat = DXGI_FORMAT_UNKNOWN; }
    if (!layout.shown) { ParkReuseResource(shownDetail); shownValid = false; }
    if (!layout.debug || debugFormat != f.answerFormat) ParkDebug();
    if (!layout.measure) ParkCoverage();
}

// Allocates or parks the textures for this frame. Returns whether reuse can run. keep: reuse is held off for now
// (frame generation, the frame rate), so the textures stay.
bool Prepare(const Frame& f, bool wanted, bool keep, float steady, float fill, bool measure, unsigned int spread,
             unsigned int interval, bool temporal, bool useSingleHistory)
{
    // Everything that follows the working size, rebuilt only when that changes.
    if (wanted && (detail[0] == nullptr || workWidth != f.workWidth || workHeight != f.workHeight))
    {
        ParkAll();
        bool allocated = true;
        for (unsigned int i = 0; i < (useSingleHistory ? 1u : 2u); ++i)
        {
            detail[i] = CreateScratch(f.device, DXGI_FORMAT_R16G16B16A16_FLOAT, f.workWidth, f.workHeight);
            colourDepth[i] = CreateScratch(f.device, DXGI_FORMAT_R32G32B32A32_FLOAT, f.workWidth, f.workHeight);
            allocated = allocated && detail[i] != nullptr && colourDepth[i] != nullptr;
        }
        if (!useSingleHistory)
        {
            prevMotion[0] = CreateScratch(f.device, DXGI_FORMAT_R32G32_FLOAT, f.workWidth, f.workHeight);
            allocated = allocated && prevMotion[0] != nullptr;
        }
        savedMotion = 0;
        if (!allocated)
        {
            ParkAll();
            host.AllocationFailed(f, false);
            LOG_ERROR("DLSS-NR detail reuse: could not allocate its history textures; every frame runs the model");
            return false;
        }
        workWidth = f.workWidth;
        workHeight = f.workHeight;
        cur = 0;
        singleHistory = useSingleHistory;
        host.TexturesRebuilt();
        LOG_INFO("DLSS-NR detail reuse: on, model {}x{}", f.workWidth, f.workHeight);
    }
    else if (!wanted)
    {
        if (detail[0] != nullptr && !keep)
            ParkAll();
        return false;
    }

    // The second saved-vector texture, every 3rd frame only. Without it the cadence runs every other frame
    // (BeforeModel).
    if (interval >= 3 && prevMotion[1] == nullptr && !host.ExtrasFailed())
    {
        prevMotion[1] = CreateScratch(f.device, DXGI_FORMAT_R32G32_FLOAT, f.workWidth, f.workHeight);
        if (prevMotion[1] == nullptr)
        {
            host.AllocationFailed(f, true);
            LOG_WARN("DLSS-NR detail reuse: could not allocate its second motion texture; every other frame instead");
        }
    }
    else if (interval < 3 && prevMotion[1] != nullptr)
    {
        ParkReuseResource(prevMotion[1]);
        savedMotion = 0;
        cadence.Drop(); // what was saved may have been in the one just parked
    }

    // The composed vectors go to the model in the motion subrect's place, so they are sized like the motion texture's
    // allocation: a render-size change moves the subrect inside it without a rebuild.
    const unsigned int wantWidth = std::max(f.motionAllocWidth, f.motionWidth);
    const unsigned int wantHeight = std::max(f.motionAllocHeight, f.motionHeight);
    if (!useSingleHistory && (composed == nullptr || composedWidth != wantWidth || composedHeight != wantHeight))
    {
        ParkComposed();
        composed = CreateScratch(f.device, DXGI_FORMAT_R32G32_FLOAT, wantWidth, wantHeight);
        if (composed == nullptr)
        {
            ParkAll();
            host.AllocationFailed(f, false);
            LOG_ERROR("DLSS-NR detail reuse: could not allocate its motion texture; every frame runs the model");
            return false;
        }
        composedWidth = wantWidth;
        composedHeight = wantHeight;
    }

    // The coverage grid, only while the pause on fast motion is on. A failure here disables the measurement alone.
    if (measure && coverage == nullptr && !coverageFailed)
        MakeCoverage(f.device);
    else if (!measure && coverage != nullptr)
        ParkCoverage();
    const bool measuring = measure && coverage != nullptr;

    // The moved detail with its trust, for Fill on reused frames, Steady on full ones and the coverage measurement;
    // the steadied answer for Steady and the spread (which also keeps what it showed).
    const bool spreading = spread > 1;
    const bool needEstimate = steady > 0.0f || fill > 0.0f || measuring || spreading || temporal;
    if (needEstimate && estimate == nullptr && !host.ExtrasFailed())
    {
        estimate = CreateScratch(f.device, DXGI_FORMAT_R16G16B16A16_FLOAT, f.workWidth, f.workHeight);
        if (estimate == nullptr)
        {
            host.AllocationFailed(f, true);
            LOG_WARN("DLSS-NR detail reuse: could not allocate the estimate texture; no fill and no steadiness");
        }
    }
    else if (!needEstimate && estimate != nullptr)
    {
        ParkReuseResource(estimate);
    }
    // Interval-1 flow feedback writes temporalFused directly; no display-Spread target is used.
    const bool needSteadied = (steady > 0.0f && !f.pictureFlowFeedbackVerified) || spreading;
    if (needSteadied && estimate != nullptr && !host.ExtrasFailed() &&
        (steadied == nullptr || steadiedFormat != f.answerFormat))
    {
        ParkReuseResource(steadied);
        steadied = CreateScratch(f.device, f.answerFormat, f.workWidth, f.workHeight);
        steadiedFormat = steadied != nullptr ? f.answerFormat : DXGI_FORMAT_UNKNOWN;
        if (steadied == nullptr)
        {
            host.AllocationFailed(f, true);
            LOG_WARN("DLSS-NR detail reuse: could not allocate the steadiness texture; full frames stay as the model made them");
        }
    }
    else if (!needSteadied && steadied != nullptr)
    {
        ParkReuseResource(steadied);
        steadiedFormat = DXGI_FORMAT_UNKNOWN;
    }
    const auto fusedFormat = temporal && f.temporalRequested && interval == 1 && spread == 1 &&
        f.answerFormat != DXGI_FORMAT_R32G32B32A32_FLOAT
        ? DXGI_FORMAT_R16G16B16A16_FLOAT : f.answerFormat;
    if (temporal && f.temporalRequested && (temporalFused == nullptr || temporalFusedFormat != fusedFormat))
    {
        ParkReuseResource(temporalFused);
        temporalFused = CreateScratch(f.device, fusedFormat, f.workWidth, f.workHeight);
        temporalFusedFormat = fusedFormat;
        if (temporalFused == nullptr || estimate == nullptr)
        {
            host.AllocationFailed(f, true);
            return false; // never mix temporal history with legacy semantic storage
        }
    }
    else if ((!temporal || !f.temporalRequested) && temporalFused != nullptr)
    {
        ParkReuseResource(temporalFused);
        temporalFusedFormat = DXGI_FORMAT_UNKNOWN;
    }
    // Feedback needs both resources as a pair.  A retained fused allocation can
    // outlive an estimate allocation failure; do not report an active plan with
    // incomplete temporal storage in that case.
    if (temporal && estimate == nullptr &&
        ((f.temporalRequested && temporalFused != nullptr) || f.pictureFlowReuseVerified))
    {
        LOG_WARN("DLSS-NR detail reuse: temporal feedback estimate is unavailable; native route stays inactive");
        return false;
    }
    if (spreading && shownDetail == nullptr && !host.ExtrasFailed())
    {
        shownDetail = CreateScratch(f.device, DXGI_FORMAT_R16G16B16A16_FLOAT, f.workWidth, f.workHeight);
        shownValid = false;
        if (shownDetail == nullptr)
        {
            host.AllocationFailed(f, true);
            LOG_WARN("DLSS-NR detail reuse: could not allocate the spread's texture; the model's frames are not "
                     "spread");
        }
    }
    else if (!spreading && shownDetail != nullptr)
    {
        ParkReuseResource(shownDetail);
        shownValid = false;
    }
    return true;
}

void MakeHistoryReadable(ID3D12GraphicsCommandList* cmdList, bool readable)
{
    const auto from = readable ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto to = readable ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Barrier(cmdList, detail[cur], from, to);
    Barrier(cmdList, colourDepth[cur], from, to);
}

// How much of the frame the estimate has no detail for, into the grid and on towards the CPU. The estimate must be
// readable; it is left as it was found.
void MeasureCoverage(const Frame& f, ID3D12Resource* movedEstimate = nullptr)
{
    if (coverage == nullptr)
        return;
    if (movedEstimate == nullptr) movedEstimate = estimate;
    params.Mode = DlssNrDetailReuse_Coverage;
    // One 8x8 thread group per tile of the grid, whatever the working size.
    const unsigned int threads = kCoverageTiles * 8;
    if (f.pass->DispatchDetailReuse(f.cmdList, params, threads, threads, f.modelInput, movedEstimate, nullptr, nullptr,
                                    nullptr, coverage, nullptr))
        CopyCoverage(f.cmdList);
}

// The outside opinion of each pixel's history for the moving passes (DlssNrFrameInfo::HistoryDistrust), or null.
ID3D12Resource* HistoryDistrust(const Frame& f) { return static_cast<ID3D12Resource*>(f.info->HistoryDistrust); }

// The moved detail with its trust into `estimate`, then `mode` (Fill or Steady) from it into `target`. measure: also
// read off how much of the frame had no detail to move, while the estimate is readable anyway.
// moved: the detail to move, last frame's saved detail by default (the spread moves what it showed instead; the
// colour it is tested with is last frame's either way).
bool EstimateThen(const Frame& f, DlssNrDetailReuseMode mode, ID3D12Resource* second, ID3D12Resource* target,
                  bool measure, ID3D12Resource* moved = nullptr)
{
    if (moved == nullptr)
        moved = detail[cur];
    MakeHistoryReadable(f.cmdList, true);
    if (moved != detail[cur])
        Barrier(f.cmdList, moved, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    params.Mode = DlssNrDetailReuse_Estimate;
    bool ok = f.pass->DispatchDetailReuse(f.cmdList, params, f.workWidth, f.workHeight, f.modelInput, moved,
                                          colourDepth[cur], f.motion, f.depth, estimate, nullptr, HistoryDistrust(f));
    if (moved != detail[cur])
        Barrier(f.cmdList, moved, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    MakeHistoryReadable(f.cmdList, false);
    if (!ok)
        return false;
    if (params.TemporalMode != 0u || f.pictureFlowFeedbackVerified) temporalEstimateReady = true;
    Barrier(f.cmdList, estimate, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (measure)
        MeasureCoverage(f);
    params.Mode = mode;
    ok = f.pass->DispatchDetailReuse(f.cmdList, params, f.workWidth, f.workHeight, f.modelInput, second, estimate,
                                     nullptr, f.depth, target, nullptr);
    Barrier(f.cmdList, estimate, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return ok;
}

// A full frame while reuse is paused: nothing is reused, but the measurement carries on so the guard sees the picture
// calm down. Estimate into `estimate`, then the grid; the answer is untouched.
void MeasureOnly(const Frame& f)
{
    if (coverage == nullptr || estimate == nullptr)
        return;
    MakeHistoryReadable(f.cmdList, true);
    params.Mode = DlssNrDetailReuse_Estimate;
    const bool ok =
        f.pass->DispatchDetailReuse(f.cmdList, params, f.workWidth, f.workHeight, f.modelInput, detail[cur],
                                    colourDepth[cur], f.motion, f.depth, estimate, nullptr, HistoryDistrust(f));
    MakeHistoryReadable(f.cmdList, false);
    if (!ok)
        return;
    Barrier(f.cmdList, estimate, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    MeasureCoverage(f);
    Barrier(f.cmdList, estimate, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

// Record an independent display answer from the same history/estimate as the normal reuse. No capture, coverage,
// motion accumulation, host failure latch or cadence transition is allowed here. A failed debug pass leaves the
// normal answer intact. This must run before AfterModel advances cur or overwrites estimate for the spread.
void RecordDebug(const Frame& f, bool filled)
{
    if (debugAnswer != nullptr && debugFormat != f.answerFormat)
        ParkDebug();
    if (debugAnswer == nullptr && !debugFailed)
    {
        debugAnswer = CreateScratch(f.device, f.answerFormat, f.workWidth, f.workHeight);
        debugFormat = debugAnswer != nullptr ? f.answerFormat : DXGI_FORMAT_UNKNOWN;
        debugFailed = debugAnswer == nullptr;
        if (debugFailed)
            LOG_WARN("DLSS-NR detail reuse: debug output could not be allocated; normal reuse remains active");
    }
    if (debugAnswer == nullptr)
        return;

    auto debugParams = params;
    debugParams.DebugView = 1;
    if (filled)
    {
        // EstimateThen left the normal estimate at rest, UAV. Fill reads it; it does not change its trust or detail.
        Barrier(f.cmdList, estimate, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        debugParams.Mode = DlssNrDetailReuse_Fill;
        debugReady = f.pass->DispatchDetailReuse(f.cmdList, debugParams, f.workWidth, f.workHeight, f.modelInput,
                                                estimate, estimate, nullptr, f.depth, debugAnswer, nullptr);
        Barrier(f.cmdList, estimate, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    else
    {
        MakeHistoryReadable(f.cmdList, true);
        debugParams.Mode = DlssNrDetailReuse_Reproject;
        debugReady = f.pass->DispatchDetailReuse(f.cmdList, debugParams, f.workWidth, f.workHeight, f.modelInput,
                                                detail[cur], colourDepth[cur], f.motion, f.depth, debugAnswer,
                                                nullptr, HistoryDistrust(f));
        MakeHistoryReadable(f.cmdList, false);
    }
}

Plan BeforeModel(const Frame& f)
{
    // Keep the raw input endpoint through ALL early returns. A late completed
    // capture cannot replace the intervening input that current one-step MV names.
    const auto previousInput = observedInputs.Observe(f.frameNumber);
    Plan plan;
    plan.motion = f.motion;
    plan.motionBaseX = f.motionBaseX;
    plan.motionBaseY = f.motionBaseY;
    composedReadable = steadiedReadable = false;
    temporalFusedReadable = temporalEstimateReady = false;
    temporalFusionRecorded = false;
    debugReady = debugReadable = false;
    CollectHistory();
    if (f.independentFeedbackRequested)
    {
        // Explicit persistent feedback owns an independent, full-model history.
        // Genshin's presentation route alone does not select it; validated guides
        // may still use the configured legacy reuse path when feedback is off.
        DiscardTemporalRecording();
        // Once the old writer has retired, release its texture set through the
        // existing real recording registry. Keeping both algorithms resident
        // after an option switch needlessly doubles the history allocation.
        if (PendingRecordings() == 0 && (detail[0] || detail[1] || colourDepth[0] ||
            colourDepth[1] || temporalFused || estimate || steadied || composed))
            ParkAll();
        cadence.Drop(); decision = {}; params = {};
        plan.resetModel = host.Finish(f, false, false, false);
        return plan;
    }
    bool orderedAdvance = AdvanceOrderedRecording(f);
    // Adjacency/contract changes must not be bypassed to move an old writer.
    // If that private native generation really finished, its owner can instead
    // Reset it through the normal hook, then collect the exact completed record.
    // In-flight or uncertain generations keep their original owners and fences.
    if (!orderedAdvance && f.orderedNativeQueue && PendingRecordings() != 0)
    {
        RetireCompletedNativeRecordings(f);
        CollectHistory();
        orderedAdvance = AdvanceOrderedRecording(f);
    }
    const bool completedRetirements = !orderedAdvance && CompletedPictureRetirements(f);
    const bool recordingAvailable = orderedAdvance || completedRetirements;
    reuseEvidence = {};
    reuseEvidence.completedRetirementsOnly = completedRetirements;
    reuseEvidence.identity = { f.owner, reinterpret_cast<uintptr_t>(f.temporalQueue),
        reinterpret_cast<uintptr_t>(RealTemporalCommands(f.cmdList)), f.revision,
        temporalRecording.pending ? 0 : recordingGeneration + 1, f.frameNumber, g_frames };
    ledger.BeginEpoch(reuseEvidence.identity);
    DlssNrCadence::StageScope beforeCpu(ledger, DlssNrCadence::Stage::ReuseBefore, reuseEvidence.identity);
    DlssNrDetailReuse::HostFrame facts = f;
    facts.beforeUpscale = f.info->BeforeUpscale;
    facts.finishedPicture = f.info->FinishedPicture;
    facts.motionMatchesPicture = f.info->MotionMatchesPicture;
    facts.present = State::Instance().frameCount;
    // Reuse may be unavailable while the real model still runs every frame.
    // Publish existing actual-call counts through the same status lock, before
    // this decision's model loop. No new counter, lock or GPU proof is created.
    facts.modelCalls = ledger.modelCalls;
    facts.modelFailures = ledger.modelFailures;
    facts.modelCallCountsKnown = ledger.modelCalls != 0;
    reuseEvidence.intervalRequested = f.cfg->DlssNrDetailReuse.value_or_default()
        ? std::clamp(f.cfg->DlssNrDetailReuseInterval.value_or_default(), 1u, 8u) : 1u;
    reuseEvidence.sourceOrdinal = publishedOrdinal;
    reuseEvidence.sourceFence = publishedFence; // target, not a claimed completion
    reuseEvidence.sourceQueueOrdered = historyPublished && publishedQueueOrdered;
    reuseEvidence.pendingRecordings = PendingRecordings();
    sourceFenceIdentity = historyPublished ? reinterpret_cast<uintptr_t>(publishedFenceOwner.Get()) : 0;
    if (historyPublished && publishedFenceOwner != nullptr)
    {
        const auto completed = publishedFenceOwner->GetCompletedValue();
        if (completed != UINT64_MAX) reuseEvidence.sourceFenceCompleted = completed;
    }
    historyReadiness = "not-evaluated";
    recordingReadiness = orderedAdvance ? "ordered-adjacent" :
        completedRetirements ? "completed-picture-retained" : "available";
    reuseEvidence.slotInFlight = !recordingAvailable && PendingRecordings() != 0 ? 1u : 0u;
    if (!recordingAvailable && PendingRecordings() != 0)
    {
        bool unknownFence = false, unsubmitted = false, gpuPending = false;
        EachRecording([&](const TemporalRecording& slot)
        {
            if (!slot.pending) return;
            reuseEvidence.bytes = (std::max)(reuseEvidence.bytes, slot.bytes);
            unknownFence = unknownFence || slot.quarantined || slot.retirementAmbiguous;
            unsubmitted = unsubmitted || !slot.submitted;
            gpuPending = gpuPending || (slot.resourceUse && !slot.resourceUse->GpuComplete());
        });
        recordingReadiness = unknownFence ? "uncertain-owner" : unsubmitted ? "unsubmitted" :
            gpuPending ? "gpu-pending" : "completed-executable-retained";
        historyReadiness = "recording-blocked";
        // No shared auxiliary resource is touched while a prior recording is
        // unsubmitted, in flight or quarantined. The ordinary full-model path
        // remains available and uses its existing lifetime management.
        reuseEvidence.cacheReason = unknownFence
            ? DlssNrCadence::CacheReason::FenceUnknown : DlssNrCadence::CacheReason::SlotInFlight;
        // Keep the producer's contract and publication intent intact. This
        // frame uses fresh NR; a later frame may collect the exact completed
        // producer. The normal cadence gap/config checks still reject stale
        // motion endpoints. No in-flight recording is cancelled or overwritten.
        decision = {}; params = {};
        plan.resetModel = host.Finish(facts, false, false, false);
        ledger.Record(reuseEvidence);
        return plan;
    }
    if (recordingAvailable)
    {
        // The previous Execute is already observed on the exact same local producer queue.
        // Order all shared UAV scratch (including coverage WAW), not only history's SRV transitions.
        D3D12_RESOURCE_BARRIER order {};
        order.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        f.cmdList->ResourceBarrier(1, &order);
    }
    const bool debugRequested = f.cfg->DlssNrDetailReuseDebug.value_or_default();
    if (!debugRequested)
        ParkDebug();
    // Anything the GPU has finished measuring, before this frame records more: the guard reads it in Gate.
    CollectCoverage();

    DlssNrDetailReuse::Wanted want = host.Gate(
        facts, [&]() -> const char* { return f.pass->DetailReuseReady() ? nullptr : "its shader could not be built"; });
    if (want.wanted && (f.temporalQueue == nullptr || !f.exposureContractKnown))
    {
        reuseEvidence.cacheReason = f.temporalQueue == nullptr
            ? DlssNrCadence::CacheReason::QueueUnknown : DlssNrCadence::CacheReason::ExposureUnknown;
        want.wanted = false; want.keep = true;
    }
    // Keep F6 source-flow latent/confidence and every skipped-model interval on two sets.
    const bool useSingleHistory = want.temporal && f.temporalRequested && f.pictureFlowFeedbackVerified &&
        want.interval == 1 && want.spread == 1;
    if (want.wanted)
    {
        DlssNrReuseBudget::Layout layout;
        layout.singleHistory = useSingleHistory;
        layout.width = f.workWidth; layout.height = f.workHeight;
        layout.motionWidth = (std::max)(f.motionAllocWidth, f.motionWidth);
        layout.motionHeight = (std::max)(f.motionAllocHeight, f.motionHeight);
        layout.coverageTiles = kCoverageTiles; layout.coverageSlots = kCoverageSlots;
        layout.coverageBytes = kCoverageBytes;
        layout.secondMotion = want.interval >= 3;
        layout.estimate = want.steady > 0.0f || want.fill > 0.0f || want.measure || want.spread > 1 || want.temporal;
        layout.steadied = (want.steady > 0.0f && !f.pictureFlowFeedbackVerified) || want.spread > 1;
        layout.fused = want.temporal && f.temporalRequested;
        layout.fusedRgba16 = layout.fused && want.interval == 1 && want.spread == 1 &&
            f.answerFormat != DXGI_FORMAT_R32G32B32A32_FLOAT;
        layout.shown = want.spread > 1; layout.debug = debugRequested; layout.measure = want.measure;
        TrimForBudget(f, layout);
        const uint64_t required = SelectedBytes(f, layout);
        reuseEvidence.bytes = DlssNrReuseBudget::Add(required, RetiredBytes());
        if (reuseEvidence.bytes > kHistoryByteCap)
        {
            // Retire old storage once, then use fresh model frames until the
            // existing retirement list releases it. Never churn new allocations
            // while an older size/option set still consumes the cap.
            ParkAll();
            reuseEvidence.cacheReason = DlssNrCadence::CacheReason::ByteCap;
            want.wanted = false; want.keep = true;
        }
    }
    const auto resourceUse = NrRecording(f.device, f.cmdList);
    if (!resourceUse || resourceUse->quarantined) { want.wanted = false; want.keep = true; }
    plan.active = Prepare(f, want.wanted, want.keep, want.steady, want.fill, want.measure, want.spread,
                          want.interval, want.temporal, useSingleHistory);
    plan.temporal = want.temporal;
    if (want.interval >= 3 && prevMotion[1] == nullptr)
    {
        want.interval = 2;
        want.spread = DlssNrDetailReuse::SpreadFor(2, f.cfg->DlssNrDetailReuseSpread.value_or_default());
    }
    // Authenticated picture motion and StarRail native scene motion each span one source picture.
    // A GPU-busy NR call may leave completed history two source captures behind; physical FG
    // Presents do not turn that one-step field into multistep motion. Refuse stale detail,
    // including full-frame feedback/Spread/Steady, and capture a fresh model answer instead.
    // SourceCaptureSerial is the native source clock, independent from physical FG Presents.
    const bool singleSourceStep = f.orderedPictureQueue || f.nativeTemporalRouteRequested;
    const bool matchingOrdinal = previousInput.Matches(publishedOrdinal, f.frameNumber, singleSourceStep);
    const bool matchingContract = historyPublished && publishedContract == Contract(f);
    const bool matchingHistory = historyPublished && matchingOrdinal && matchingContract && !f.modelReset;
    // Observation only: the existing CacheReason also covers ordinal failures.
    // Keep a genuine source gap distinct from a changed exposure/Look/config key.
    historyReadiness = f.modelReset ? "reset" : !historyPublished ? "no-history" :
        !matchingOrdinal ? (publishedOrdinal >= f.frameNumber ? "non-forward-source" : "source-gap") :
        !matchingContract ? "contract-changed" :
        publishedQueueOrdered && reuseEvidence.sourceFenceCompleted < publishedFence
            ? "queue-ordered-gpu-pending" : "completed";
    if (!matchingHistory)
    {
        if (plan.active) reuseEvidence.cacheReason = f.modelReset ? DlssNrCadence::CacheReason::Reset :
            historyPublished ? DlssNrCadence::CacheReason::ContractChanged : DlssNrCadence::CacheReason::NoHistory;
        cadence.Drop();
        shownValid = false;
    }
    host.Decide(facts, plan.active, want);
    if (plan.active)
    {
        auto& slot = temporalRecording;
        slot.commands = RealTemporalCommands(f.cmdList); slot.queue = f.temporalQueue;
        slot.commandOwner = slot.commands;
        slot.resourceUse = resourceUse;
        slot.ordered = f.orderedPictureQueue || f.orderedNativeQueue;
        slot.logicalPublished = false;
        slot.contract = Contract(f); slot.ordinal = f.frameNumber; slot.generation = ++recordingGeneration;
        slot.bytes = reuseEvidence.bytes;
        slot.pending = true; slot.submitted = slot.publish = slot.shown = slot.quarantined = slot.retirementProven = slot.retirementAmbiguous = false;
        slot.fence.Reset(); slot.value = 0; // assigned only by the actual submission receipt
        slot.retirementQueue.Reset();
        DlssNrWatchedLists::Add(slot.commands);
    }
    params.HistoryDistrust = HistoryDistrust(f) != nullptr ? 1u : 0u;
    // Normal output, spread and capture always use the same constants whether debug is displayed or not.
    params.DebugView = 0;
    // Fill samples t4 as depth even in the legacy shader mode. On the Genshin
    // picture route a null depth binds input colour as the descriptor stand-in;
    // that colour must not become geometric support for borrowing neighbour detail.
    if ((want.temporal || f.orderedPictureQueue) && !(f.depthWidth && f.depthHeight))
        params.FillStrength = 0.0f;

    if (decision.kind == DlssNrDetailReuse::Kind::Reuse)
    {
        const bool filled = (params.FillStrength > 0.0f || want.temporal) && estimate != nullptr;
        if (filled)
        {
            // Moved detail with its trust first; Fill composes it and fills where it was dropped.
            plan.reused = EstimateThen(f, DlssNrDetailReuse_Fill, estimate, f.output, want.measure);
        }
        else
        {
            // Fill is off. The frame is still measured when asked for, in a pass of its own, so the reuse below stays
            // exactly what it was: Fill at no strength is nearly the same but not bit for bit (it reads the detail back
            // through a half-float texture and lacks Reproject's overflow guard).
            if (want.measure)
                MeasureOnly(f);

            MakeHistoryReadable(f.cmdList, true);
            params.Mode = DlssNrDetailReuse_Reproject;
            plan.reused =
                f.pass->DispatchDetailReuse(f.cmdList, params, f.workWidth, f.workHeight, f.modelInput, detail[cur],
                                            colourDepth[cur], f.motion, f.depth, f.output, nullptr, HistoryDistrust(f));
            MakeHistoryReadable(f.cmdList, false);
        }

        if (plan.reused)
        {
            if (debugRequested)
                RecordDebug(f, filled && params.FillStrength > 0.0f);
            // Kept for the next full frame, which composes it with its own vectors for the model's history. Without
            // it that frame cannot compose, so the cadence starts over and the model's history is reset instead. The
            // second reused frame in a row adds its own to what the first kept, into the other texture.
            // Source-flow history advances one source at a time. The model has a
            // different endpoint after skips: use Finish's real reset fallback,
            // not a partial/off-screen accumulated vector as a multistep proof.
            bool saved = f.pictureFlowReuseVerified;
            if (!saved && decision.accumulateMotion)
            {
                Barrier(f.cmdList, prevMotion[savedMotion], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                params.Mode = DlssNrDetailReuse_AccumulateMotion;
                saved = f.pass->DispatchDetailReuse(f.cmdList, params, f.workWidth, f.workHeight, f.motion, nullptr,
                                                    nullptr, f.motion, prevMotion[savedMotion],
                                                    prevMotion[savedMotion ^ 1u], nullptr);
                Barrier(f.cmdList, prevMotion[savedMotion], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (saved)
                    savedMotion ^= 1u;
            }
            else if (!saved)
            {
                params.Mode = DlssNrDetailReuse_SaveMotion;
                saved = f.pass->DispatchDetailReuse(f.cmdList, params, f.workWidth, f.workHeight, f.motion, nullptr,
                                                    nullptr, f.motion, nullptr, prevMotion[savedMotion], nullptr);
            }
            if (!saved)
                cadence.Drop();
        }
        else
        {
            // Nothing was written: run the model this frame and start the cadence over.
            cadence.ReuseFailed();
            static bool warnedReuse = false;
            if (!warnedReuse)
            {
                warnedReuse = true;
                LOG_WARN("DLSS-NR detail reuse: the reuse pass could not be recorded; running the model");
            }
        }
    }

    // A full frame while reuse is paused: keep measuring, so the guard knows when the picture has calmed down. With
    // steadiness on, AfterModel's own Estimate carries the measurement instead of running a second identical one.
    measureWithSteady = false;
    if (!plan.reused && plan.active && want.measure && want.hold && decision.historyUsable)
    {
        const bool feedback = want.temporal && f.temporalRequested;
        // The cohort path owns estimate persistently; never measure into it here.
        if (params.TemporalHandoff != 0) measureWithSteady = true;
        else if (params.Steady > 0.0f && estimate != nullptr &&
            (feedback ? temporalFused != nullptr : steadied != nullptr) &&
            (!feedback || params.TemporalHistoryWeight > 0.0f))
            measureWithSteady = true;
        else
            MeasureOnly(f);
    }

    // A full frame after reused frames: the model last ran two (or three) frames ago, so it gets the vectors over all.
    // Native final-picture guide cancellation is per-base-frame. Until the model's own multistep jitter
    // endpoint contract is proved, use Finish's existing model-reset fallback after a skipped model.
    if (params.TemporalMode == 0u && !plan.reused && decision.kind == DlssNrDetailReuse::Kind::Full && decision.composeMotion)
    {
        ID3D12Resource* saved = prevMotion[savedMotion];
        Barrier(f.cmdList, saved, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        params.Mode = DlssNrDetailReuse_Compose;
        const bool ok = f.pass->DispatchDetailReuse(f.cmdList, params, f.motionWidth, f.motionHeight, f.motion,
                                                    nullptr, nullptr, f.motion, saved, composed, nullptr);
        Barrier(f.cmdList, saved, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (ok)
        {
            Barrier(f.cmdList, composed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            composedReadable = true;
            plan.motion = composed;
            plan.motionBaseX = plan.motionBaseY = 0;
        }
    }

    plan.resetModel = host.Finish(facts, plan.active, plan.reused, composedReadable);
    reuseEvidence.accepted = plan.reused;
    reuseEvidence.pendingRecordings = PendingRecordings();
    reuseEvidence.intervalUsed = plan.active && matchingHistory ? want.interval : 1u;
    if (plan.reused) reuseEvidence.cacheReason = DlssNrCadence::CacheReason::Accepted;
    else if (decision.kind == DlssNrDetailReuse::Kind::Reuse)
        reuseEvidence.cacheReason = DlssNrCadence::CacheReason::RecordFailed;
    ledger.Record(reuseEvidence);
    return plan;
}

// Interval-1 only: detail is cohort A; estimate is cohort B, not transient
// motion scratch. temporalFused (signed float) is reused for each moved
// estimate, then for the display. Neither cohort reads the displayed blend.
void CaptureContribution(const Frame& f, ID3D12Resource* fresh, ID3D12Resource* fused,
                         uint64_t source = publishedOrdinal, bool available = decision.historyUsable)
{
    GuideCapture::SampleToken previewToken;
    const bool preview=GuideCapture::HistoryDebugView()==0&&GuideCapture::TryBeginSample(GuideCapture::Stream::History,previewToken);
    const bool offline=GuideCapture::OfflineSampleDue(GuideCapture::Stream::History);
    struct Cancel {GuideCapture::SampleToken& token;~Cancel(){GuideCapture::CancelSample(token);}} cancel{previewToken};
    if ((!preview&&!offline) || !f.info || !f.info->IndependentCommands || !fresh || !fused || fresh==fused)
        return;
    GuideCapture::Inputs input;
    input.independentRecording=true;
    input.colour=fused; input.contribution=fresh; input.compareFreshAnswer=true;
    input.colourRect={0,0,f.workWidth,f.workHeight};
    input.colourState=input.contributionState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto stamp=DlssNrGuideObservation::Source(*f.info);
    stamp.configuration=f.revision;stamp.owner=f.owner;stamp.feature=reinterpret_cast<uintptr_t>(g_nr.feature);
    stamp.historySource=source;stamp.historyAvailable=available;
    stamp.fusionRecorded=true;stamp.accepted=true;stamp.stage=GuideDiagnostics::Stage::Recorded;
    stamp.colour=DlssNrGuideObservation::Resource(fused,input.colourRect);
    stamp.reason="history fusion recorded; awaiting measured fresh/fused RGB difference";
    stamp.colourChoice="actual fused model answer versus same-frame fresh model answer";
    auto* list=static_cast<ID3D12GraphicsCommandList*>(RealTemporalCommands(f.cmdList));
    const bool recorded=preview&&GuideCapture::TryRecord(previewToken,list,input,stamp);
    const bool captured=offline&&GuideCapture::RecordOffline(GuideCapture::Stream::History,list,input,stamp);
    if(recorded||captured)DlssNrWatchedLists::Add(list);
}

void AfterModelHandoff(const Frame& f, ID3D12Resource*& finalAnswer)
{
    if (!decision.captureHistory) return;
    const unsigned write = singleHistory ? cur : cur ^ 1u;
    const float savedSteady = params.Steady;
    const uint32_t savedKind = params.TemporalFrameKind;
    bool ok = true;
    Barrier(f.cmdList, colourDepth[cur], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (unsigned cohort = 0; cohort < 2 && ok; ++cohort)
    {
        ID3D12Resource* previous = cohort == 0 ? detail[cur] : estimate;
        ID3D12Resource* target = cohort == 0 ? detail[write] : estimate;
        const bool carry = decision.historyUsable && (params.TemporalHandoffResetMask & (1u << cohort)) == 0;
        if (carry)
        {
            Barrier(f.cmdList, previous, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            params.Mode = DlssNrDetailReuse_Estimate;
            ok = f.pass->DispatchDetailReuse(f.cmdList, params, f.workWidth, f.workHeight,
                f.modelInput, previous, colourDepth[cur], f.motion, f.depth, temporalFused, nullptr,
                HistoryDistrust(f));
            Barrier(f.cmdList, previous, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            // The read has ended before same-resource cohort capture can write.
            Barrier(f.cmdList, temporalFused, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (ok && cohort == params.TemporalHandoffIncoming && measureWithSteady)
                MeasureCoverage(f, temporalFused);
        }
        if (ok)
        {
            // Also order seed/reset writes, where no preceding SRV read exists.
            D3D12_RESOURCE_BARRIER order {};
            order.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            order.UAV.pResource = target;
            f.cmdList->ResourceBarrier(1, &order);
            params.Steady = params.TemporalHistoryWeight;
            params.TemporalFrameKind = carry ? 0u : 2u;
            params.Mode = DlssNrDetailReuse_HandoffCapture;
            ok = f.pass->DispatchDetailReuse(f.cmdList, params, f.workWidth, f.workHeight,
                f.modelInput, finalAnswer, carry ? temporalFused : nullptr,
                params.TemporalContent != 0u ? f.motion : nullptr, f.depth, target, nullptr,
                params.TemporalContent != 0u ? colourDepth[cur] : HistoryDistrust(f));
        }
        if (carry)
            Barrier(f.cmdList, temporalFused, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    params.Steady = savedSteady;
    params.TemporalFrameKind = savedKind;
    Barrier(f.cmdList, colourDepth[cur], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (ok)
    {
        // Both old-guide reads have ended. Compose also captures their common
        // current-frame guides, avoiding another full-screen dispatch.
        D3D12_RESOURCE_BARRIER guideOrder {};
        guideOrder.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        guideOrder.UAV.pResource = colourDepth[write];
        f.cmdList->ResourceBarrier(1, &guideOrder);
        Barrier(f.cmdList, detail[write], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(f.cmdList, estimate, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        params.Mode = DlssNrDetailReuse_HandoffCompose;
        ok = f.pass->DispatchDetailReuse(f.cmdList, params, f.workWidth, f.workHeight,
            f.modelInput, detail[write], estimate, finalAnswer, f.depth, temporalFused, colourDepth[write]);
        Barrier(f.cmdList, estimate, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(f.cmdList, detail[write], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    shownValid = false;
    if (!ok)
    {
        // A partially overwritten cohort is never published. Keep the genuine
        // full-model answer and the recording's real retained GPU owners.
        cadence.Captured(false);
        return;
    }
    host.TemporalCaptured(true);
    temporalRecording.write = write;
    temporalRecording.publish = true;
    temporalRecording.shown = false;
    Barrier(f.cmdList, temporalFused, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    temporalFusedReadable = true;
    temporalFusionRecorded = decision.historyUsable && params.TemporalHistoryWeight > 0.0f &&
        params.TemporalHandoffResetMask != 3u;
    if(temporalFusionRecorded)CaptureContribution(f,finalAnswer,temporalFused);
    finalAnswer = temporalFused;
}


// Multi-interval flow/native latent and display Spread retain their existing path.
// Logical publication still follows the real submitted/completed recording.
void AfterModelTemporal(const Frame& f, const Plan& plan, bool succeeded, ID3D12Resource*& finalAnswer)
{
    if (!succeeded || finalAnswer == nullptr)
    {
        cadence.Drop();
        shownValid = false;
        return;
    }
    if (params.TemporalHandoff != 0)
    {
        AfterModelHandoff(f, finalAnswer);
        return;
    }
    const bool canCarry = decision.historyUsable;
    if (!plan.reused && canCarry && params.TemporalHistoryWeight > 0.0f && temporalFused != nullptr)
    {
        const float savedSteady = params.Steady;
        const float savedDeadZone = params.SteadyDeadZone;
        params.Steady = params.TemporalHistoryWeight;
        // Feedback always obeys lifetime and strength, including duration 0.
        // The display dead zone must not promote small historical differences to weight 1.
        params.SteadyDeadZone = 0.0f;
        const bool fused = EstimateThen(f, DlssNrDetailReuse_FreshHistory, finalAnswer, temporalFused, measureWithSteady);
        params.Steady = savedSteady;
        params.SteadyDeadZone = savedDeadZone;
        if (fused)
        {
            Barrier(f.cmdList, temporalFused, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            temporalFusedReadable = true;
            temporalFusionRecorded = true;
            CaptureContribution(f,finalAnswer,temporalFused);
            finalAnswer = temporalFused;
        }
        else temporalEstimateReady = false;
    }

    // Failed estimate/full reset cannot read an old estimate or manufacture a reusable sample.
    if (!temporalEstimateReady && plan.reused)
    {
        cadence.Drop();
        shownValid = false;
        return;
    }
    const unsigned write = singleHistory ? cur : cur ^ 1u;
    bool captured = false;
    if (decision.captureHistory)
    {
        // Flow-only history is the existing colour/motion layout; depth stays absent.
        if (singleHistory)
        {
            D3D12_RESOURCE_BARRIER order[2] {};
            order[0].Type = order[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            order[0].UAV.pResource = detail[write];
            order[1].UAV.pResource = colourDepth[write];
            f.cmdList->ResourceBarrier(2, order);
        }
        params.Mode = f.sceneGeometryVerified ? DlssNrDetailReuse_TemporalCapture :
            f.pictureFlowReuseVerified ? DlssNrDetailReuse_FlowCapture :
            f.pictureFlowFeedbackVerified ? DlssNrDetailReuse_Capture : DlssNrDetailReuse_TemporalCapture;
        params.TemporalFrameKind = plan.reused ? 1u : !canCarry || !temporalEstimateReady ? 2u : 0u;
        if (temporalEstimateReady)
            Barrier(f.cmdList, estimate, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        captured = f.pass->DispatchDetailReuse(f.cmdList, params, f.workWidth, f.workHeight, f.modelInput,
            finalAnswer, f.depth, temporalEstimateReady ? estimate : nullptr, nullptr,
            detail[write], colourDepth[write], HistoryDistrust(f));
        if (temporalEstimateReady)
            Barrier(f.cmdList, estimate, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    ID3D12Resource* displayed = nullptr;
    const auto spread = DlssNrDetailReuse::PlanSpread(decision, plan.reused,
        captured && estimate != nullptr && steadied != nullptr, shownValid && shownDetail != nullptr, false);
    if (spread.run)
    {
        const float savedSteady = params.Steady;
        params.Steady = spread.amount;
        const bool ok = EstimateThen(f, DlssNrDetailReuse_Steady, finalAnswer, steadied, false,
                                     spread.moveShown ? shownDetail : nullptr);
        params.Steady = savedSteady;
        if (ok)
        {
            Barrier(f.cmdList, steadied, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            steadiedReadable = true;
            displayed = steadied;
        }
    }
    shownValid = false;
    if (displayed == nullptr && decision.spreadStep >= 0) cadence.EndSpread();
    if (captured)
    {
        // CaptureShown reads the just-recorded write slot explicitly; publication waits for Submitted.
        if (displayed != nullptr && spread.saveShown)
        {
            // Display storage is not a fresh model sample. A source-chain Spread
            // must not turn lost confidence back into valid history either.
            if (f.pictureFlowReuseVerified)
            {
                Barrier(f.cmdList, detail[write], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(f.cmdList, estimate, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            params.Mode = f.pictureFlowReuseVerified ? DlssNrDetailReuse_FlowCaptureShown :
                DlssNrDetailReuse_CaptureShown;
            shownValid = shownDetail != nullptr && f.pass->DispatchDetailReuse(f.cmdList, params,
                f.workWidth, f.workHeight, f.modelInput, displayed, nullptr,
                f.pictureFlowReuseVerified ? detail[write] : nullptr,
                f.pictureFlowReuseVerified ? estimate : nullptr, shownDetail, nullptr);
            if (f.pictureFlowReuseVerified)
            {
                Barrier(f.cmdList, estimate, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                Barrier(f.cmdList, detail[write], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            if (!shownValid) cadence.EndSpread();
        }
    }
    if (captured)
    {
        host.TemporalCaptured(!plan.reused);
        temporalRecording.write = write;
        temporalRecording.publish = true;
        temporalRecording.shown = shownValid;
        shownValid=false;
    }
    else if (decision.captureHistory) cadence.Captured(false);
    if (displayed != nullptr) finalAnswer = displayed;
    if (plan.reused && debugReady)
    {
        Barrier(f.cmdList, debugAnswer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        debugReadable = true;
        finalAnswer = debugAnswer;
    }
}

// After the model passes, or the reuse. succeeded: finalAnswer holds a valid, readable answer. May replace it with
// the steadied one.
void AfterModel(const Frame& f, const Plan& plan, bool succeeded, ID3D12Resource*& finalAnswer)
{
    if (!plan.active) return;
    if (composedReadable)
        Barrier(f.cmdList, composed, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    composedReadable = false;
    if (plan.temporal)
    {
        AfterModelTemporal(f, plan, succeeded, finalAnswer);
        return;
    }

    // The spread (dlssnr/DlssNrDetailReuse.h): this frame shows Steady's blend toward what was shown before, moved here
    // -- last frame's saved detail on the full frame, what the spread showed last frame after it -- while the history
    // below keeps the unblended answer. displayed: what the resolve shows instead, once the history is saved.
    ID3D12Resource* displayed = nullptr;
    const DlssNrDetailReuse::SpreadPlan spread = DlssNrDetailReuse::PlanSpread(
        decision, plan.reused, succeeded && finalAnswer != nullptr && estimate != nullptr && steadied != nullptr,
        shownValid && shownDetail != nullptr, params.DebugView != 0);
    if (spread.run)
    {
        const float steady = params.Steady;
        params.Steady = spread.amount;
        const bool ok = EstimateThen(f, DlssNrDetailReuse_Steady, finalAnswer, steadied,
                                     !spread.moveShown && measureWithSteady, spread.moveShown ? shownDetail : nullptr);
        params.Steady = steady;
        if (ok)
        {
            Barrier(f.cmdList, steadied, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            steadiedReadable = true;
            displayed = steadied;
        }
    }
    shownValid = false;
    if (displayed == nullptr && decision.spreadStep >= 0)
        cadence.EndSpread();

    // A full frame with last frame's history: pull the model's new detail toward the moved previous detail, as far as
    // that is trusted, so this frame and the reused one next to it differ less. Wherever the spread did not run.
    if (displayed == nullptr && !plan.reused && succeeded && finalAnswer != nullptr &&
        decision.historyUsable && params.Steady > 0.0f && estimate != nullptr && steadied != nullptr &&
        EstimateThen(f, DlssNrDetailReuse_Steady, finalAnswer, steadied, measureWithSteady))
    {
        Barrier(f.cmdList, steadied, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        steadiedReadable = true;
        finalAnswer = steadied;
    }

    // Every processed frame keeps the normal detail (answer - input), input colour and depth. The independent debug
    // output is selected only after both the normal history and the spread's shown detail have been captured.
    if (decision.captureHistory)
    {
        bool captured = false;
        if (succeeded && finalAnswer != nullptr)
        {
            const unsigned int write = 1u - cur;
            params.Mode = DlssNrDetailReuse_Capture;
            captured = f.pass->DispatchDetailReuse(f.cmdList, params, f.workWidth, f.workHeight, f.modelInput,
                                                   finalAnswer, f.depth, nullptr, nullptr, detail[write],
                                                   colourDepth[write]);
            if (captured)
            {
                temporalRecording.write = write;
                temporalRecording.publish = true;
            }
        }
        if (!captured) cadence.Captured(false);

        // The spread's next frame moves what this one showed (tested with this frame's colour, just saved)
        if (displayed != nullptr && spread.saveShown)
        {
            params.Mode = DlssNrDetailReuse_CaptureShown;
            shownValid = captured && shownDetail != nullptr &&
                         f.pass->DispatchDetailReuse(f.cmdList, params, f.workWidth, f.workHeight, f.modelInput,
                                                     displayed, nullptr, nullptr, nullptr, shownDetail, nullptr);
            if (!shownValid)
                cadence.EndSpread();
        }
        temporalRecording.shown = shownValid;
        shownValid = false;
    }
    if (displayed != nullptr)
        finalAnswer = displayed;
    if (succeeded && plan.reused && debugReady)
    {
        Barrier(f.cmdList, debugAnswer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        debugReadable = true;
        finalAnswer = debugAnswer;
    }
}

// After the resolve has read the answer: the steadied one goes back to rest.
void AfterResolve(ID3D12GraphicsCommandList* cmdList)
{
    if (temporalFusedReadable)
        Barrier(cmdList, temporalFused, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    temporalFusedReadable = false;
    if (steadiedReadable)
        Barrier(cmdList, steadied, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    steadiedReadable = false;
    if (debugReadable)
        Barrier(cmdList, debugAnswer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    debugReadable = debugReady = false;
}

void RecordGpuTime(double ms) { host.RecordGpuTime(ms); }

bool SafeToRelease()
{
    CollectHistory();
    return PendingRecordings() == 0;
}

DlssNr::DetailReuseInfo Status() { return host.Status(); }

// For the menu, without g_nrMutex.
DlssNr::DetailReuseInfo Published() { return host.Published(State::Instance().frameCount); }

// Shutdown: the GPU is idle, so everything is released outright.
void Release()
{
    for (ID3D12Resource** res : { &detail[0], &detail[1], &colourDepth[0], &colourDepth[1], &prevMotion[0],
                                  &prevMotion[1], &shownDetail, &composed, &estimate, &steadied, &temporalFused,
                                  &coverage, &debugAnswer })
    {
        if (*res != nullptr)
        {
            (*res)->Release();
            *res = nullptr;
        }
    }
    for (CoverageSlot& slot : coverageSlots)
    {
        if (slot.buffer != nullptr)
        {
            slot.buffer->Release();
            slot.buffer = nullptr;
        }
        slot.fence.Reset();
        slot.commands = nullptr;
        slot.value = 0;
        slot.pending = slot.submitted = false;
    }
    coverageValue = 0;
    coverageFailed = false;
    measureWithSteady = false;
    workWidth = workHeight = composedWidth = composedHeight = 0;
    singleHistory = false;
    steadiedFormat = DXGI_FORMAT_UNKNOWN;
    composedReadable = steadiedReadable = false;
    cur = 0;
    savedMotion = 0;
    shownValid = false;
    debugFormat = DXGI_FORMAT_UNKNOWN;
    debugReady = debugReadable = debugFailed = false;
    temporalRecording = {};
    orderedRetirements = {};
    publishedGeneration = 0; publishedQueueOrdered = false;
    producerOwners = {}; producerOwnerNext = 0;
    historyPublished = false; publishedOrdinal = publishedFence = 0;
    observedInputs.Reset();
    publishedFenceOwner.Reset();
    sourceFenceIdentity = 0;
    historyReadiness = "no-history"; recordingReadiness = "no-recording";
    publishedContract = {}; temporalHistoryQueue = nullptr;
    allocationBudget.clear(); budgetDevice = nullptr;
    temporalFusedFormat = DXGI_FORMAT_UNKNOWN;
    temporalFusedReadable = temporalEstimateReady = false;
    host.Reset();
}
} // namespace DetailReuse
