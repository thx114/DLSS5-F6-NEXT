// Included inside namespace DlssNr. All entry points share g_nrMutex.
// The private helper must declare its integration dependencies at global scope.
} // namespace DlssNr (resumed below)
#include <native/NrConsumerFilter.h>
namespace DlssNr
{
namespace Late
{
using Microsoft::WRL::ComPtr;
struct Slot
{
    ComPtr<ID3D12Resource> depth, motion, linear, encoded, residual, zeroMotionUpload;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    native::nrfilter::PresentTransaction nrFilter; // no source/backbuffer retained across calls
    native::nrfilter::depth::Region filterDepthRegion; // actual depth fields, independent of motion/jitter knownness
    ComPtr<ID3D12CommandQueue> producerQueue; // real queue that signals `ready`; held so its identity stays valid
    ID3D12CommandList* producer = nullptr; // identity only; never dereferenced
    DlssNrFrameInfo frame {};
    // Ordinary ZZZ receipt metadata is independent of Present and model epochs.
    uint64_t zzzAttempt = 0, zzzOrdinal = 0;
    UpscaleSource zzzSource {};
    uintptr_t zzzOutput = 0;
    ComPtr<ID3D12CommandList> zzzProducer;
    uint64_t ready = 0, done = 0, serial = 0, nativeCaptureAttempt = 0, sourceEpoch = 0;
    uintptr_t bridgeHistoryOwner = 0; // history owner at capture; identity only
    uintptr_t backBufferIdentity = 0; // bound by this private recording; identity only, never dereferenced
    bool pending = false, submitted = false, residualOnly = false, sceneLinear = true;
    bool dx11Bridge = false, bridgeOutputHandoff = false, quarantined = false;
};
std::array<Slot, 4> slots;
// Separate from capture slots: no stale capture content or producer fence is borrowed.
std::array<Slot, 4> freshColorSlots;
struct NativeSizing
{
    bool valid = false;
    DlssNrFrameInfo frame {}; // sizing/encoding scalars only; no game resource pointers
    D3D12_RESOURCE_DESC motion {};
    uintptr_t owner = 0;
    DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
};
NativeSizing nativeSizing;
bool freshColorActive = false;
ComPtr<ID3D12Device> device;
uint64_t serial = 0, successes = 0, stillRendering = 0;
uint64_t nativeCaptureAttempt = 0, nativeCaptureAttemptEpoch = 0;
bool loggedCrossQueue = false;
inline constexpr const char* pausedForGameFg = "Paused while the game's own frame generation is on.";

FinishedPictureOutcome finishedPictureOutcome{};
uint64_t finishedPictureEndpointSerial=0;

// Native DLSS-G consumes the finished picture before Present. That evaluate owns
// the status and history while mode 1 is active; Present must not cancel it.
inline bool ComposeOnGameFg()
{
    const Config& cfg = *Config::Instance();
    return cfg.DlssNrEnabled.value_or_default() && cfg.DlssNrFinishedPicture.value_or_default() &&
           !cfg.DlssNrRunBeforeSr.value_or_default() && !cfg.DlssNrDeferredDlss.value_or_default();
}
std::string status = "Waiting for a finished picture.";
bool reset = true;
std::atomic<bool> tracking { false };
inline constexpr GUID colorSpaceKey = { 0x34a31e7b, 0x84c5, 0x44ef, { 0xa7, 0x4d, 0x6b, 0xd3, 0x60, 0x8c, 0xe5, 0x22 } };

void Say(const char* message)
{
    if (status != message)
    {
        status = message;
        LOG_INFO("DLSS-NR finished picture: {}", message);
    }
}

bool Finished(const Slot& slot)
{
    return slot.nrFilter.DepthSourceFinished() && (!slot.fence ||
        (slot.fence->GetCompletedValue() != UINT64_MAX && slot.fence->GetCompletedValue() >= slot.done));
}

// One final-colour history, shared across capture slots. Never sample NR-modified colour.
std::unique_ptr<OpticalFlowDx12> pictureFlow;
std::unique_ptr<TrustMaskDx12> pictureTrust;
ComPtr<ID3D12CommandQueue> pictureQueue;
struct PictureCompletion { ComPtr<ID3D12Fence> fence; uint64_t value = 0; };
std::array<PictureCompletion, 4> pictureCompletion;
uint64_t pictureSequence = 0;
UINT64 pictureWidth = 0;
UINT pictureHeight = 0;
DXGI_FORMAT pictureFormat = DXGI_FORMAT_UNKNOWN;
bool pictureQuarantined = false;
uint64_t bridgePresentSequence = 0, pictureLastBridgePresent = 0;
uintptr_t pictureOwner = 0; // identity only; never retain or dereference a swapchain
DlssNrPresentSubmission::Clock bridgeSubmission;

// A committed StarRail final-picture endpoint covers older capture recordings too.
// Serial AND source epoch must agree: a future/raced epoch is never retired as covered.
// This is metadata only; pending GPU resources retain their original fences.
struct BridgeCoverage
{
    bool valid = false;
    uint64_t serial = 0, epoch = 0;
    uintptr_t owner = 0;
    unsigned width = 0, height = 0;
};
BridgeCoverage bridgeCoverage;
uint64_t coveredRetireEvents = 0, uncoveredRetireEvents = 0, explicitResetEvents = 0, endpointGapEvents = 0;

void TraceResetReason(const char* reason, uint64_t& count, const Slot& slot)
{
    ++count;
    if (count <= 3 || count % 120 == 0)
        LOG_INFO("DLSS-NR Late reason={} count={} captureSerial={} sourceEpoch={} captureOwner={:X} coveredSerial={} coveredEpoch={} coveredOwner={:X}",
                 reason, count, slot.serial, slot.sourceEpoch, slot.bridgeHistoryOwner,
                 bridgeCoverage.serial, bridgeCoverage.epoch, bridgeCoverage.owner);
}

bool CoveredBySuccessfulBridge(const Slot& slot, uint64_t now, uintptr_t owner)
{
    return bridgeCoverage.valid && owner && owner == bridgeCoverage.owner && slot.bridgeHistoryOwner == owner &&
           slot.dx11Bridge && !slot.residualOnly &&
           now >= bridgeCoverage.epoch && slot.serial <= bridgeCoverage.serial &&
           slot.sourceEpoch <= bridgeCoverage.epoch &&
           slot.frame.OutputWidth == bridgeCoverage.width && slot.frame.OutputHeight == bridgeCoverage.height;
}

void RetireCapture(Slot& slot, uint64_t now, uintptr_t owner)
{
    if (CoveredBySuccessfulBridge(slot, now, owner))
        TraceResetReason("covered-capture-retired", coveredRetireEvents, slot);
    else
    {
        TraceResetReason("capture-discard-needs-reset", uncoveredRetireEvents, slot);
        reset = true;
    }
    slot.pending = false;
}

void CommitBridgeCoverage(const Slot& slot, bool persistentBridge, bool ran, bool pictureUpdated, uintptr_t owner)
{
    if (!persistentBridge) return;
    bridgeCoverage = {};
    if (ran && pictureUpdated && owner)
        bridgeCoverage = { true, slot.serial, slot.sourceEpoch, owner,
                           slot.frame.OutputWidth, slot.frame.OutputHeight };
}

bool IsStarRailProcess()
{
    static const bool starRail = [] {
        wchar_t path[32768] {};
        const auto size = GetModuleFileNameW(nullptr, path, (DWORD) std::size(path));
        if (!size || size >= std::size(path)) return false;
        const auto* name = wcsrchr(path, L'\\');
        return _wcsicmp(name ? name + 1 : path, L"StarRail.exe") == 0;
    }();
    return starRail;
}

bool IsZzzProcess()
{
    static const bool zzz = [] {
        wchar_t path[32768] {};
        const auto size = GetModuleFileNameW(nullptr, path, (DWORD) std::size(path));
        if (!size || size >= std::size(path)) return false;
        const auto* name = wcsrchr(path, L'\\');
        return _wcsicmp(name ? name + 1 : path, L"ZenlessZoneZero.exe") == 0;
    }();
    return zzz;
}

// CPU admission only: source callbacks are not GPU completions or loading-screen signals.
// All state below is protected by g_nrMutex, including source and endpoint calls.
struct ZzzPreparation
{
    static constexpr unsigned stableSourceCallbacks = 30;
    UpscaleSource source {};
    uintptr_t endpoint = 0, queue = 0;
    unsigned renderWidth = 0, renderHeight = 0, samples = 0;
    D3D12_RESOURCE_DESC output {}, motion {}, depth {}, target {};
    bool sourceKnown = false, endpointKnown = false, rayReconstruction = false;

    static bool SameShape(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b)
    {
        return a.Dimension == b.Dimension && a.Width == b.Width && a.Height == b.Height &&
               a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels &&
               a.Format == b.Format && a.SampleDesc.Count == b.SampleDesc.Count &&
               a.SampleDesc.Quality == b.SampleDesc.Quality && a.Layout == b.Layout && a.Flags == b.Flags;
    }

    void Restart() { samples = 0; }

    bool Observe(NVSDK_NGX_Parameter* params, ID3D12Resource* color, ID3D12Resource* vectors,
                 ID3D12Resource* guide, bool rr, UpscaleSource currentSource)
    {
        const bool knownSource = currentSource.ngxHandle != 0 ||
            (currentSource.virtualContext && currentSource.virtualGeneration != 0);
        if (!params || !color || !knownSource)
        {
            sourceKnown = false;
            Restart();
            return false;
        }
        const auto colorDesc = color->GetDesc();
        const auto motionDesc = vectors ? vectors->GetDesc() : D3D12_RESOURCE_DESC {};
        const auto depthDesc = guide ? guide->GetDesc() : D3D12_RESOURCE_DESC {};
        if (!colorDesc.Width || !colorDesc.Height ||
            colorDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || colorDesc.SampleDesc.Count != 1)
        {
            sourceKnown = false;
            Restart();
            return false;
        }
        unsigned width = 0, height = 0, gameReset = 0;
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &width);
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &height);
        params->Get(NVSDK_NGX_Parameter_Reset, &gameReset);
        if (!sourceKnown || source != currentSource ||
            renderWidth != width || renderHeight != height || rayReconstruction != rr ||
            !SameShape(output, colorDesc) || !SameShape(motion, motionDesc) || !SameShape(depth, depthDesc))
        {
            source = currentSource;
            output = colorDesc; motion = motionDesc; depth = depthDesc;
            renderWidth = width; renderHeight = height; rayReconstruction = rr;
            sourceKnown = true;
            Restart();
        }
        if (gameReset)
        {
            Restart();
            return false;
        }
        // Count real source calls under the NR lock. Physical/generated Presents
        // cannot increment this counter; no global Present clock or GPU fence is a source ordinal.
        if (samples < stableSourceCallbacks) ++samples;
        return samples >= stableSourceCallbacks;
    }

    bool AtEndpoint(uintptr_t owner, ID3D12CommandQueue* presentQueue, const D3D12_RESOURCE_DESC& desc)
    {
        const auto currentQueue = reinterpret_cast<uintptr_t>(presentQueue);
        if (!endpointKnown || endpoint != owner || queue != currentQueue || !SameShape(target, desc))
        {
            // Initial binding consumes the observed source window. A changed
            // real endpoint/queue restarts only CPU admission, never GPU ownership.
            if (endpointKnown) Restart();
            endpoint = owner; queue = currentQueue; target = desc;
            endpointKnown = true;
        }
        return sourceKnown && samples >= stableSourceCallbacks;
    }
};
ZzzPreparation zzzPreparation;

struct ZzzSourceClock
{
    uint64_t attempt = 0, completedAttempt = 0, ordinal = 0, claimedAttempt = 0;
    bool succeeded = false, routeActive = false;
};
ZzzSourceClock zzzSource;
struct ZzzPictureHistory
{
    UpscaleSource source {};
    uint64_t ordinal = 0, guideEpoch = 1;
    uint32_t encoding = 0;
} zzzPicture;
// Only the immediately enclosing live NGX call can attach the before-SR capture.
// A concurrent or nested newer call invalidates the older endpoint association.
struct ZzzSourceCall
{
    uint64_t attempt = 0;
    ID3D12CommandList* commands = nullptr; // comparison only
    NVSDK_NGX_Parameter* params = nullptr; // comparison only
    UpscaleSource source {};
};
thread_local ZzzSourceCall zzzSourceCall;

void InvalidateZzzSource()
{
    // Revoke CPU admission only. Attempts/ordinals never restart or alias a live
    // call, and no recording/resource/queue/fence owner is released here.
    zzzSource.routeActive = zzzSource.succeeded = false;
    zzzSource.completedAttempt = 0;
    zzzSourceCall = {};
    zzzPicture.ordinal = 0;
}

bool BridgeContinuationEnabled()
{
    return IsStarRailProcess() && !native::OpticalFgNrEnabled();
}

// Physical/generated Presents may advance the global clock on another thread
// between native SR capture and this source picture's bridge handoff. Only the
// actual StarRail bridge endpoint consumes a source epoch. Keep it separate
// from frame.SubmissionEpoch, filter endpoint epochs and real GPU fences.
uint64_t bridgeSourceEpoch = 1;
uint64_t CaptureEpoch(bool dx11Bridge)
{
    if (!dx11Bridge && IsZzzProcess() && ComposeOnGameFg() && zzzSource.routeActive) return zzzSource.ordinal;
    return dx11Bridge && BridgeContinuationEnabled() ? bridgeSourceEpoch : State::Instance().frameCount;
}

bool PictureComplete(const PictureCompletion& c)
{
    if (!c.fence) return true;
    const auto value = c.fence->GetCompletedValue();
    return value != UINT64_MAX && value >= c.value;
}

bool PictureHistoryReady(ID3D12CommandQueue* queue, const D3D12_RESOURCE_DESC& desc)
{
    if (pictureQuarantined) return false;
    const bool changed = pictureQueue.Get() != queue || pictureWidth != desc.Width ||
                         pictureHeight != desc.Height || pictureFormat != desc.Format;
    if (changed)
    {
        // Shape/queue changes cannot release any texture still read by a submitted list.
        for (auto& c : pictureCompletion)
        {
            if (PictureComplete(c)) continue;
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!event) return false;
            const bool done = SUCCEEDED(c.fence->SetEventOnCompletion(c.value, event)) &&
                              WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
            CloseHandle(event);
            if (!done) return false;
        }
        pictureFlow.reset(); pictureTrust.reset();
        pictureQueue = queue; pictureWidth = desc.Width; pictureHeight = desc.Height; pictureFormat = desc.Format;
        pictureCompletion = {}; pictureSequence = 0;
    }
    // Both shader modules use an eight-frame descriptor ring; cap this owner at four completed slots.
    if (!PictureComplete(pictureCompletion[pictureSequence % pictureCompletion.size()])) return false;
    if (!pictureFlow)
    {
        auto flow = std::make_unique<OpticalFlowDx12>();
        auto trust = std::make_unique<TrustMaskDx12>();
        if (!flow->Init(device.Get()) || !trust->Init(device.Get())) return false;
        pictureFlow = std::move(flow); pictureTrust = std::move(trust);
    }
    return true;
}

ID3D12CommandQueue* RealQueue(ID3D12CommandQueue* queue)
{
    ID3D12CommandQueue* real = nullptr;
    return Util::CheckForRealObject(__FUNCTION__, queue, (IUnknown**) &real) ? real : queue;
}

// Both callers already split on OptiScaler's FG (the swapchain Present when it is off, FG_Hooks when it
// runs); the check is repeated so capture, which has no such caller, decides the same way.
bool PausedForGameFrameGeneration()
{
    // Only the exact ordinary parent producer callback can bypass the game-FG
    // pause. Capture/menu/unrelated interop callers have no such capability.
    if (GenshinPresentNr::NativeParentConsumer::Active()) return false;
    auto& state = State::Instance();
    auto* fg = state.currentFG;
    return GameFrameGenerationOn(fg && fg->IsActive() && !fg->IsPaused(),
                                 state.activeFgInput == FGInput::DLSSG, state.activeFgOutput == FGOutput::DLSSG,
                                 state.dlssgLastSetMode != sl::DLSSGMode::eOff,
                                 PresentsSince(state.frameCount, state.dlssgLastEvaluateFrame));
}

bool ZzzGameFrameGenerationStale()
{
    if (!IsZzzProcess()) return false;
    const auto& state = State::Instance();
    // The last SetOptions(On) is not perpetual evidence of produced frames.
    // A new DLSSG Evaluate revokes this exception on the next present.
    return PresentsSince(state.frameCount, state.dlssgLastEvaluateFrame) > DlssgEvaluateWindow;
}

void Cancel(bool resetHistory = true)
{
    if (IsZzzProcess()) InvalidateZzzSource();
    // Submitted copies may still be in flight; their fences still protect reuse.
    for (auto* group : { &slots, &freshColorSlots })
        for (auto& slot : *group)
        {
            slot.nrFilter.CollectDepth(); // poll; Cancel cannot erase either GPU reader
            if (slot.submitted && !slot.quarantined)
                slot.pending = false;
        }
    nativeSizing.valid = false;
    freshColorActive = false;
    if (resetHistory)
    {
        reset = true;
        bridgeCoverage = {};
    }
}

bool FreshColorSafeToRelease()
{
    // Shutdown also owns capture-slot filter snapshots. A signalled unused
    // offer is reclaimable after its actual copy, not a permanent quarantine.
    for (auto* group : { &slots, &freshColorSlots })
        for (auto& slot : *group)
            if (!slot.nrFilter.DepthSafeToRelease()) return false;
    for (const auto& slot : freshColorSlots)
        if (slot.quarantined || (slot.submitted && !Finished(slot))) return false;
    return true;
}

bool Clone(ComPtr<ID3D12Resource>& copy, ID3D12Resource* source)
{
    const auto want = source->GetDesc();
    if (want.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || want.SampleDesc.Count != 1 ||
        want.DepthOrArraySize != 1 || want.MipLevels != 1)
        return false;
    if (copy)
    {
        const auto have = copy->GetDesc();
        if (have.Width != want.Width || have.Height != want.Height ||
            have.Format != TypedGuideFormat(want.Format))
            copy.Reset(); // caller checked the previous GPU fence
    }
    if (!copy)
        copy.Attach(CreateGuideClone(device.Get(), source));
    return copy != nullptr;
}

bool EnsureSlotObjects(Slot& slot)
{
    if (!slot.commands)
    {
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.fence))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                            IID_PPV_ARGS(&slot.commands))) || FAILED(slot.commands->Close()))
        {
            slot.commands.Reset(); slot.allocator.Reset(); slot.fence.Reset();
            Say("Could not prepare the finished-picture option.");
            return false;
        }
    }
    return true;
}

void RememberNativeSizing(const Slot& slot, uintptr_t owner, DXGI_FORMAT colorFormat)
{
    // Reconstruct rather than copying frame: exposure/history pointers and all guide
    // identity/authority fields remain empty. The previous guides are never read.
    auto& saved = nativeSizing;
    saved = {};
    saved.valid = slot.motion != nullptr;
    if (!saved.valid) return;
    saved.owner = owner;
    saved.colorFormat = colorFormat;
    saved.motion = slot.motion->GetDesc();
    auto& f = saved.frame;
    f.RenderSubrectWidth = slot.frame.RenderSubrectWidth;
    f.RenderSubrectHeight = slot.frame.RenderSubrectHeight;
    f.OutputWidth = slot.frame.OutputWidth; f.OutputHeight = slot.frame.OutputHeight;
    f.MotionSubrectBaseX = slot.frame.MotionSubrectBaseX;
    f.MotionSubrectBaseY = slot.frame.MotionSubrectBaseY;
    f.MotionVectorsLowResolution = slot.frame.MotionVectorsLowResolution;
    f.MvScaleX = slot.frame.MvScaleX; f.MvScaleY = slot.frame.MvScaleY;
    f.DepthInverted = slot.frame.DepthInverted;
    f.RayReconstruction = slot.frame.RayReconstruction;
    f.IndependentCommands = true; f.FinishedPicture = true;
    f.OutputArrivalState = D3D12_RESOURCE_STATE_PRESENT;
    f.NativeSceneRoute = true; f.Reset = true; f.FreshColorResetFallback = true;
}

bool EnsureZeroMotion(Slot& slot, D3D12_RESOURCE_DESC want)
{
    // Float zero is unambiguous. In particular UNORM encodings may use 0.5 for
    // stationary motion; all-zero bits would fabricate motion on that route.
    if (want.Format != DXGI_FORMAT_R16G16_FLOAT && want.Format != DXGI_FORMAT_R32G32_FLOAT)
        return false;
    if (want.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || want.SampleDesc.Count != 1 ||
        want.DepthOrArraySize != 1 || want.MipLevels != 1 || !want.Width || !want.Height)
        return false;
    if (slot.motion)
    {
        const auto have = slot.motion->GetDesc();
        if (have.Width != want.Width || have.Height != want.Height || have.Format != want.Format)
        { slot.motion.Reset(); slot.zeroMotionUpload.Reset(); } // completed owner fence checked below
    }
    if (slot.motion && slot.zeroMotionUpload) return true;
    slot.motion.Reset(); slot.zeroMotionUpload.Reset();
    want.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES heap {}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &want,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.motion)))) return false;
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&want, 0, 1, 0, nullptr, nullptr, nullptr, &bytes);
    if (!bytes || bytes == UINT64_MAX || bytes > SIZE_MAX) { slot.motion.Reset(); return false; }
    D3D12_RESOURCE_DESC upload {}; upload.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload.Width = bytes; upload.Height = 1; upload.DepthOrArraySize = 1; upload.MipLevels = 1;
    upload.SampleDesc.Count = 1; upload.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &upload,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&slot.zeroMotionUpload))))
    { slot.motion.Reset(); return false; }
    void* data = nullptr;
    const D3D12_RANGE readNone { 0, 0 };
    if (FAILED(slot.zeroMotionUpload->Map(0, &readNone, &data)))
    { slot.motion.Reset(); slot.zeroMotionUpload.Reset(); return false; }
    memset(data, 0, static_cast<size_t>(bytes));
    slot.zeroMotionUpload->Unmap(0, nullptr);
    return true;
}

Slot* AcquireFreshColor(uintptr_t owner, const D3D12_RESOURCE_DESC& desc, ID3D12CommandQueue* queue, bool dx11Bridge,
                        bool allowStandalone)
{
    const NativeSizing* sizing = &nativeSizing;
    NativeSizing standalone {};
    if (pictureQueue.Get() != queue || !nativeSizing.valid || nativeSizing.owner != owner ||
        nativeSizing.colorFormat != desc.Format || nativeSizing.frame.OutputWidth != desc.Width ||
        nativeSizing.frame.OutputHeight != desc.Height)
    {
        if (!allowStandalone) return nullptr;
        // DLSSG's normal NR path need not have visited the ordinary Present
        // endpoint before dialogue rendering stops submitting guides. Build a
        // self-owned ABI fallback from the real endpoint, not stale DLSS data.
        standalone.valid = true;
        standalone.owner = owner;
        standalone.colorFormat = desc.Format;
        auto& motion = standalone.motion;
        motion.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        motion.Width = desc.Width; motion.Height = desc.Height;
        motion.DepthOrArraySize = 1; motion.MipLevels = 1;
        motion.Format = DXGI_FORMAT_R16G16_FLOAT;
        motion.SampleDesc.Count = 1;
        auto& frame = standalone.frame;
        frame.RenderSubrectWidth = frame.OutputWidth = (unsigned) desc.Width;
        frame.RenderSubrectHeight = frame.OutputHeight = (unsigned) desc.Height;
        frame.MvScaleX = frame.MvScaleY = 1.0f;
        frame.IndependentCommands = true; frame.FinishedPicture = true;
        frame.OutputArrivalState = D3D12_RESOURCE_STATE_PRESENT;
        frame.NativeSceneRoute = true; frame.Reset = true; frame.FreshColorResetFallback = true;
        sizing = &standalone;
    }
    for (auto& slot : freshColorSlots)
    {
        if (slot.pending || slot.quarantined || !Finished(slot)) continue; // poll only: no CPU/GPU wait or allocator race
        if (!EnsureSlotObjects(slot) || !EnsureZeroMotion(slot, sizing->motion)) return nullptr;
        slot.frame = sizing->frame;
        slot.ready = slot.done; // no producer capture or producer wait on this endpoint
        slot.residualOnly = false; slot.dx11Bridge = dx11Bridge;
        return &slot;
    }
    return nullptr;
}

void RecordZeroMotion(Slot& slot)
{
    D3D12_TEXTURE_COPY_LOCATION source {}; source.pResource = slot.zeroMotionUpload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    const auto desc = slot.motion->GetDesc();
    device->GetCopyableFootprints(&desc, 0, 1, 0, &source.PlacedFootprint, nullptr, nullptr, nullptr);
    D3D12_TEXTURE_COPY_LOCATION target {}; target.pResource = slot.motion.Get();
    target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    slot.commands->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
}

Slot* Acquire(ID3D12GraphicsCommandList* cmd, bool dx11Bridge = false)
{
    const auto api = State::Instance().swapchainInteropApi;
    if (!cmd || (dx11Bridge ? api != SwapchainInteropApi::Dx11wDx12 : api != SwapchainInteropApi::None))
    { Say("This option needs a native DirectX 12 game."); return nullptr; }
    if (!dx11Bridge && PausedForGameFrameGeneration() && !ZzzGameFrameGenerationStale())
    {
        if (!ComposeOnGameFg()) Say(pausedForGameFg);
        return nullptr;
    }
    ComPtr<ID3D12Device> currentDevice;
    if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&currentDevice))))
        return nullptr;
    if (device && device != currentDevice)
    {
        if (IsZzzProcess()) InvalidateZzzSource();
        Say("The graphics device changed. Restart the game to use this option.");
        return nullptr;
    }
    device = currentDevice;
    tracking.store(true);
    // Only the lightweight submission hook is needed, including when FG is disabled.
    ResTrack_Dx12::HookLateNrQueue(device.Get());
    Slot* next = nullptr;
    for (auto& slot : slots)
    {
        slot.nrFilter.CollectDepth(); // retire completed Readers; stable GPU pool entries stay cached
        if (!slot.pending && Finished(slot)) { next = &slot; break; }
    }
    if (!next)
    {
        // This frame gets no capture: an older frame's pending one must not be composed onto its picture.
        // Captures of this frame (a game may evaluate more than once) stay.
        const auto now = CaptureEpoch(dx11Bridge);
        for (auto& slot : slots)
            if (slot.submitted && slot.pending && slot.sourceEpoch < now) RetireCapture(slot, now, pictureOwner);
        Say("Waiting for the previous picture to finish.");
        return nullptr;
    }
    auto& slot = *next;
    if (!EnsureSlotObjects(slot)) return nullptr;
    return next;
}

void Arm(Slot& slot, ID3D12GraphicsCommandList* cmd)
{
    static bool loggedArm = false;
    if (!loggedArm)
    {
        loggedArm = true;
        LOG_INFO("DLSS-NR FG path: first capture armed, epoch {} dims {}x{}", slot.frame.SubmissionEpoch,
                 slot.frame.OutputWidth, slot.frame.OutputHeight);
    }
    slot.producer = cmd;
    ID3D12GraphicsCommandList* real = nullptr;
    if (Util::CheckForRealObject(__FUNCTION__, cmd, (IUnknown**) &real)) slot.producer = real;
    DlssNrWatchedLists::Add(slot.producer); // the submit/reset hooks must hear about this list
    slot.serial = ++serial;
    slot.sourceEpoch = slot.dx11Bridge && !slot.residualOnly && BridgeContinuationEnabled()
        ? bridgeSourceEpoch : slot.frame.SubmissionEpoch;
    slot.frame.SourceCaptureSerial = slot.serial;
    slot.frame.SourceCaptureEpoch = slot.frame.SubmissionEpoch;
    slot.bridgeHistoryOwner = slot.dx11Bridge ? pictureOwner : 0;
    if (slot.frame.Reset) TraceResetReason("capture-explicit-reset", explicitResetEvents, slot);
    if (!slot.serial || slot.frame.Reset) bridgeCoverage = {};
    slot.ready = slot.done + 1;
    slot.done = slot.ready;
    slot.submitted = false;
    slot.pending = true;
}

bool NativeRegionFits(ID3D12Resource* resource, unsigned x, unsigned y, unsigned width, unsigned height)
{
    if (!resource || !width || !height) return false;
    const auto desc = resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.SampleDesc.Count == 1 &&
           desc.DepthOrArraySize == 1 && desc.MipLevels == 1 &&
           (uint64_t) x + width <= desc.Width && (uint64_t) y + height <= desc.Height;
}

bool NativeCaptureIdentityMatches(const Slot& slot, uint64_t epoch, bool bridgeOrdered)
{
    return epoch != UINT64_MAX && slot.submitted && slot.dx11Bridge && !slot.residualOnly && slot.serial != 0 &&
           slot.frame.SourceCaptureSerial == slot.serial &&
           slot.frame.SourceCaptureEpoch == slot.frame.SubmissionEpoch &&
           slot.sourceEpoch == epoch && bridgeOrdered && nativeCaptureAttemptEpoch == epoch &&
           nativeCaptureAttempt != 0 && slot.nativeCaptureAttempt == nativeCaptureAttempt;
}

void PopulateNativeSceneContract(DlssNrFrameInfo& frame, const Slot& slot, uint64_t epoch,
                                 const D3D12_RESOURCE_DESC& desc, ID3D12CommandQueue* presentQueue,
                                 ID3D12CommandQueue* orderedProducer)
{
    frame.SourceCaptureSerial = slot.serial;
    frame.SourceCaptureEpoch = slot.frame.SubmissionEpoch;
    frame.NativePresentQueueIdentity = presentQueue;
    frame.NativeBaseIdentityVerified = NativeCaptureIdentityMatches(slot, epoch,
        BridgeProducerOrdered(slot.dx11Bridge, slot.bridgeOutputHandoff, orderedProducer != nullptr,
                              slot.producerQueue.Get() == orderedProducer)) &&
        slot.frame.OutputWidth == desc.Width && slot.frame.OutputHeight == desc.Height;
    frame.NativeCurrentDepthVerified = frame.NativeBaseIdentityVerified && frame.NativeFeatureFlagsKnown &&
        NativeRegionFits(slot.depth.Get(), frame.DepthSubrectBaseX, frame.DepthSubrectBaseY,
                         frame.RenderSubrectWidth, frame.RenderSubrectHeight);
    // NVIDIA DLSS guide 3.6 defines current-to-previous, upper-left, pixel MV
    // after the submitted scales. This is an API scene contract, not a claim
    // that game postprocessing/HUD obey the scene field at every final pixel.
    frame.NativeRouteConventionVerified = frame.NativeBaseIdentityVerified && frame.NativeGuideMetadataKnown;
    // Guide 3.7.3 declares jitter in render pixels in the same canonical upper-left
    // coordinates as the SDK motion convention. Thus native raster p = final p + j:
    // query either low-resolution guide at uv + j/renderSize. MV scales decode raw
    // vector units/direction; they do not scale pixel-space jitter a second time.
    // Depth row normalization happens upstream; it must not negate this canonical
    // phase again. Extra motion row/sign overrides have no proven phase contract.
    const auto& cfg = *Config::Instance();
    frame.NativeGuidePhaseVerified = frame.NativeCurrentJitterKnown && frame.NativeMotionJitterKnown &&
        !frame.NativeMotionVectorsJittered && frame.MotionVectorsLowResolution && frame.NativeGuideMetadataKnown &&
        std::abs(frame.NativeJitterX) <= 0.5f && std::abs(frame.NativeJitterY) <= 0.5f &&
        !cfg.DlssNrMotionTextureFlipY.value_or_default() && !cfg.DlssNrMotionYInverted.value_or_default();
    if (frame.NativeGuidePhaseVerified)
    {
        frame.NativeMotionPhaseUvX = frame.NativeDepthPhaseUvX = frame.NativeJitterX / frame.RenderSubrectWidth;
        frame.NativeMotionPhaseUvY = frame.NativeDepthPhaseUvY = frame.NativeJitterY / frame.RenderSubrectHeight;
        frame.NativeMotionJitterCancelUvX = frame.NativeMotionJitterCancelUvY = 0.0f; // declared nonjittered MV
    }
    frame.NativeGuideContractVerified = frame.NativeBaseIdentityVerified && frame.NativeCurrentDepthVerified &&
        frame.NativeGuideMetadataKnown && frame.NativeMotionJitterKnown && frame.NativeCurrentJitterKnown &&
        frame.NativeRouteConventionVerified && frame.NativeGuidePhaseVerified;
    // Same scene capture is not a claim about HUD/postprocessing motion.
    frame.MotionMatchesPicture = false;
    frame.MotionIsPictureFlow = false;
}

void Capture(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* params, bool rr, bool dx11Bridge = false,
             unsigned retainedFlags = 0, bool retainedFlagsKnown = false, UpscaleSource source = {})
{
    const bool nativeCapture = dx11Bridge && IsStarRailProcess();
    const auto attempt = nativeCapture ? ++nativeCaptureAttempt : 0;
    if (nativeCapture) nativeCaptureAttemptEpoch = CaptureEpoch(dx11Bridge);
    if (!params) return;
    auto* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    auto* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");
    auto* output = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    // Defer before capture-slot allocation, guide copies or any NR recording.
    if (!dx11Bridge && IsZzzProcess() && !zzzPreparation.Observe(params, output, motion, depth, rr, source))
    {
        Say("Deferring ZZZ NR preparation until the SR source is stable.");
        return;
    }
    // Depth is optional: NR runs without it.
    if (!motion || !output)
    { Cancel(); Say("Waiting for the game's movement data."); return; }
    auto* next = Acquire(cmd, dx11Bridge);
    if (!next) return;
    auto& slot = *next;
    slot.nativeCaptureAttempt = attempt;
    if (!depth) slot.depth.Reset(); // no stale depth from an earlier frame
    if ((depth && !Clone(slot.depth, depth)) || !Clone(slot.motion, motion))
    { Say("The game's depth or movement data is not supported."); return; }
    slot.residualOnly = false;
    slot.dx11Bridge = dx11Bridge;
    slot.bridgeOutputHandoff = false;
    // Copy at the NGX seam, where guide states and lifetimes are defined. Keep typed,
    // shader-readable copies until both the producing queue and NR have finished.
    for (auto pair : { std::pair { depth, slot.depth.Get() }, std::pair { motion, slot.motion.Get() } })
    {
        if (!pair.first) continue;
        const auto sourceState = dx11Bridge ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        Barrier(cmd, pair.first, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyResource(pair.second, pair.first);
        Barrier(cmd, pair.first, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
        // The copy returns to COPY_DEST after the late dispatch.
    }
    slot.frame = {};
    auto& frame = slot.frame;
    frame.SceneGuideOwner[0] = source.ngxHandle;
    frame.SceneGuideOwner[1] = reinterpret_cast<uintptr_t>(source.virtualContext);
    frame.SceneGuideOwner[2] = source.virtualGeneration;
    unsigned flags = 0, gameReset = 0;
    int parameterFlags = 0; // same parameter type used by IFeature::SetInitParameters
    // The bridge passes the retained create flags, not an invented evaluate-time zero.
    // The producer separately retains whether its create-time Get succeeded. Evaluate
    // blocks need not carry create flags, and an absent flag must not turn into known zero.
    const bool parameterFlagsKnown = params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
                                                  &parameterFlags) == NVSDK_NGX_Result_Success;
    const bool sourceFlagsKnown = source.featureFlagsKnown;
    flags = dx11Bridge ? retainedFlags : sourceFlagsKnown ? source.featureFlags : (unsigned) parameterFlags;
    frame.NativeFeatureFlags = flags;
    frame.NativeFeatureFlagsKnown = dx11Bridge ? retainedFlagsKnown : (sourceFlagsKnown || parameterFlagsKnown);
    frame.NativeMotionJitterKnown = frame.NativeFeatureFlagsKnown;
    frame.NativeMotionVectorsJittered = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0;
    float jitterX = 0.0f, jitterY = 0.0f;
    const bool jitterXKnown = params->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jitterX) == NVSDK_NGX_Result_Success;
    const bool jitterYKnown = params->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jitterY) == NVSDK_NGX_Result_Success;
    frame.NativeCurrentJitterKnown = jitterXKnown && jitterYKnown && std::isfinite(jitterX) && std::isfinite(jitterY);
    if (frame.NativeCurrentJitterKnown) { frame.NativeJitterX = jitterX; frame.NativeJitterY = jitterY; }
    params->Get(NVSDK_NGX_Parameter_Reset, &gameReset);
    frame.Reset = gameReset != 0;
    frame.DepthInverted = (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    frame.MotionVectorsLowResolution = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
    frame.RayReconstruction = rr;
    frame.OutputWidth = (unsigned) output->GetDesc().Width;
    frame.OutputHeight = output->GetDesc().Height;
    // Read every field independently: an early failure must not skip the other Get calls.
    const bool widthKnown = params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &frame.RenderSubrectWidth) == NVSDK_NGX_Result_Success;
    const bool heightKnown = params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &frame.RenderSubrectHeight) == NVSDK_NGX_Result_Success;
    const bool depthXKnown = params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, &frame.DepthSubrectBaseX) == NVSDK_NGX_Result_Success;
    const bool depthYKnown = params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, &frame.DepthSubrectBaseY) == NVSDK_NGX_Result_Success;
    // Preserve only the Get results already needed by NR. Bounds/descriptor
    // work for the filter remains lazy inside its selected snapshot path.
    const bool depthRegionKnown = widthKnown && heightKnown && depthXKnown && depthYKnown;
    slot.filterDepthRegion = {frame.DepthSubrectBaseX, frame.DepthSubrectBaseY,
                             frame.RenderSubrectWidth, frame.RenderSubrectHeight, depthRegionKnown};
    bool regionsKnown = widthKnown && heightKnown && depthXKnown && depthYKnown;
    regionsKnown &= params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, &frame.MotionSubrectBaseX) == NVSDK_NGX_Result_Success;
    regionsKnown &= params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, &frame.MotionSubrectBaseY) == NVSDK_NGX_Result_Success;
    const bool scaleXKnown = params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.MvScaleX) == NVSDK_NGX_Result_Success;
    const bool scaleYKnown = params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.MvScaleY) == NVSDK_NGX_Result_Success;
    const bool scalesKnown = scaleXKnown && scaleYKnown && std::isfinite(frame.MvScaleX) && std::isfinite(frame.MvScaleY) &&
                             frame.MvScaleX != 0.0f && frame.MvScaleY != 0.0f;
    const auto motionWidth = frame.MotionVectorsLowResolution ? frame.RenderSubrectWidth : frame.OutputWidth;
    const auto motionHeight = frame.MotionVectorsLowResolution ? frame.RenderSubrectHeight : frame.OutputHeight;
    frame.NativeGuideMetadataKnown = frame.NativeFeatureFlagsKnown && regionsKnown && scalesKnown &&
        NativeRegionFits(slot.depth.Get(), frame.DepthSubrectBaseX, frame.DepthSubrectBaseY,
                         frame.RenderSubrectWidth, frame.RenderSubrectHeight) &&
        NativeRegionFits(slot.motion.Get(), frame.MotionSubrectBaseX, frame.MotionSubrectBaseY, motionWidth, motionHeight);
    // Route/phase permission is established only after matching this capture to the real base.
    frame.ColourIsLinearHdr = false;
    frame.IndependentCommands = true;
    frame.FinishedPicture = true;
    frame.FrameGenerationInput = dx11Bridge; // preserve the spatial/FG safety gate
    frame.OutputArrivalState = D3D12_RESOURCE_STATE_PRESENT;
    frame.SubmissionEpoch = State::Instance().frameCount;
    Arm(slot, cmd);
    slot.zzzAttempt = slot.zzzOrdinal = 0;
    slot.zzzSource = {};
    slot.zzzOutput = 0;
    slot.zzzProducer.Reset();
    if (!dx11Bridge && IsZzzProcess() && zzzSourceCall.attempt &&
        zzzSourceCall.attempt == zzzSource.attempt && zzzSourceCall.params == params &&
        zzzSourceCall.source == source && zzzSourceCall.commands == slot.producer)
    {
        slot.zzzAttempt = zzzSourceCall.attempt;
        slot.zzzSource = source;
        slot.zzzOutput = reinterpret_cast<uintptr_t>(output);
        slot.zzzProducer = slot.producer;
        slot.sourceEpoch = 0; // not a successful source until the real SR returns
    }
}

bool CaptureResidual(ID3D12GraphicsCommandList* cmd, ID3D12Resource* clean, ID3D12Resource* residual,
                     float scale, bool sceneLinear)
{
    auto* next = Acquire(cmd);
    if (!next) return false;
    auto& slot = *next;
    if (!Clone(slot.residual, residual))
    { Say("The upscaled changes could not be saved."); return false; }
    Barrier(cmd, residual, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyResource(slot.residual.Get(), residual);
    Barrier(cmd, residual, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    slot.frame = {};
    slot.frame.OutputWidth = (unsigned) clean->GetDesc().Width;
    slot.frame.OutputHeight = clean->GetDesc().Height;
    slot.frame.PreExposure = scale;
    slot.frame.SubmissionEpoch = State::Instance().frameCount;
    slot.residualOnly = true;
    slot.dx11Bridge = false;
    slot.sceneLinear = sceneLinear;
    Arm(slot, cmd);
    return true;
}

} // namespace Late

uint64_t BeginZzzFinishedPictureSource(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* params,
                                      UpscaleSource source)
{
    if (!Late::IsZzzProcess()) return 0;
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    const auto& cfg = *Config::Instance();
    if (!cmd || !params || !source.ngxHandle || !cfg.DlssNrEnabled.value_or_default() ||
        !cfg.DlssNrFinishedPicture.value_or_default() || cfg.DlssNrRunBeforeSr.value_or_default() ||
        cfg.DlssNrDeferredDlss.value_or_default() ||
        State::Instance().swapchainInteropApi != SwapchainInteropApi::None)
    {
        Late::InvalidateZzzSource();
        return 0;
    }
    auto& clock = Late::zzzSource;
    if (clock.attempt == UINT64_MAX || clock.ordinal == UINT64_MAX)
    { Late::InvalidateZzzSource(); return 0; } // never wrap identities
    const auto attempt = ++clock.attempt;
    clock.completedAttempt = 0;
    clock.succeeded = false;
    clock.routeActive = true;
    Late::zzzSourceCall = {attempt, DetailReuse::RealSubmittedCommands(cmd), params, source};
    return attempt;
}

void CompleteZzzFinishedPictureSource(uint64_t attempt, ID3D12GraphicsCommandList* cmd,
                                     NVSDK_NGX_Parameter* params, UpscaleSource source, bool succeeded)
{
    if (!attempt) return;
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    const auto call = Late::zzzSourceCall;
    Late::zzzSourceCall = {};
    auto& clock = Late::zzzSource;
    if (!clock.routeActive) return; // Cancel/resize/config/device revoked the enclosing call
    if (attempt != clock.attempt || call.attempt != attempt || call.params != params ||
        call.commands != DetailReuse::RealSubmittedCommands(cmd) || call.source != source)
    {
        // Overlapping/nested source order is unknown; do not borrow a previous success.
        clock.succeeded = false;
        Late::reset = true;
        return;
    }
    clock.completedAttempt = attempt;
    clock.succeeded = succeeded;
    if (!succeeded) { Late::reset = true; return; }
    ++clock.ordinal; // successful SR only; capture readiness/Present cannot increment it
    auto* output = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    const auto desc = output ? output->GetDesc() : D3D12_RESOURCE_DESC {};
    for (auto& slot : Late::slots)
        if (slot.pending && slot.zzzAttempt == attempt && slot.zzzSource == source &&
            slot.producer == call.commands && slot.zzzOutput == reinterpret_cast<uintptr_t>(output) &&
            slot.frame.OutputWidth == desc.Width && slot.frame.OutputHeight == desc.Height)
        {
            slot.zzzOrdinal = clock.ordinal;
            slot.sourceEpoch = clock.ordinal;
        }
}

void FinishedPictureResetCommandList(ID3D12CommandList* cmd)
{
    // Nearly every reset is of a list NR never recorded on: no lock for those (DlssNr_WatchedLists.h)
    cmd = DetailReuse::RealSubmittedCommands(cmd); // match the canonical recording generation
    const void* list = cmd;
    if (!DlssNrWatchedLists::Any(1, &list) && !DlssNrCurrentOutput::Capture::Instance().Watches(DetailReuse::RealSubmittedCommands(cmd))) return;

    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    DlssNrWatchedLists::Remove(1, &list);
    g_nrGpuWork.ResetRecording(cmd);
    DlssNrCurrentOutput::Capture::Instance().Reset(DetailReuse::RealSubmittedCommands(cmd));
    if (g_gpuTime) g_gpuTime->ResetRecording(cmd);
    if (g_ngxTime) g_ngxTime->ResetRecording(cmd);
    for (auto& timer : g_layerTime) if (timer) timer->ResetRecording(cmd);
    // Reuse's coverage readbacks: a recording thrown away frees its slot, since no fence will ever be signalled for it.
    DetailReuse::ResetRecording(cmd);
    SceneCut::ResetRecording(cmd); // the scene-cut flag's readbacks, the same way
    if (!Late::tracking.load()) return;
    for (auto& slot : Late::slots)
        if (slot.pending && !slot.submitted && slot.producer == cmd)
        {
            Late::RetireCapture(slot, Late::CaptureEpoch(slot.dx11Bridge && !slot.residualOnly), Late::pictureOwner);
            slot.done = slot.ready - 1; // discarded recording: no GPU signal was promised
        }
}

namespace Late
{
// Caller holds g_nrMutex, before selecting/recording a new private endpoint.
// A completed but idle pool slot may never be selected again. Its executable
// generation still pins parked model/scratch snapshots until a real Reset closes it.
// Poll only; borrowed producer/game lists and uncertain submissions stay owned.
// A source rollover excludes the list about to be reset by the normal endpoint.
void RetireCompletedPrivateRecordings(const Slot* nextRecording = nullptr)
{
    const auto uses = g_nrGpuWork.Snapshot();
    unsigned retired = 0;
    for (auto* group : { &slots, &freshColorSlots })
        for (auto& slot : *group)
        {
            if (&slot == nextRecording || slot.pending || slot.quarantined || !slot.submitted || !slot.backBufferIdentity ||
                !slot.commands || !slot.allocator || !slot.fence || !slot.done || !Finished(slot)) continue;
            auto* identity = DetailReuse::RealSubmittedCommands(slot.commands.Get());
            if (!DlssNrGpuLifetime::SameDevice(slot.commands.Get(), slot.allocator.Get())) continue;
            bool open = false, complete = true;
            for (const auto& use : uses)
            {
                if (!use || use->listOwner.Get() != identity) continue;
                open |= !use->closed;
                if (use->quarantined || use->discarded || !use->submitted || !use->GpuComplete())
                { complete = false; break; }
            }
            if (!open || !complete) continue;
            // Both the slot's final signal and every actual NR execution have
            // finished. Reset only this pool's own allocator/list, never a
            // captured producer. GPU completion alone does not close the token.
            if (FAILED(slot.allocator->Reset()) || FAILED(slot.commands->Reset(slot.allocator.Get(), nullptr)))
            {
                slot.quarantined = true;
                LOG_WARN("DLSS-NR private recording Reset failed during resource retirement; retaining GPU owners");
                continue;
            }
            FinishedPictureResetCommandList(identity); // idempotent if the Reset hook already notified
            slot.backBufferIdentity = 0;
            if (g_nrGpuWork.Watches(identity) || FAILED(slot.commands->Close()))
            {
                slot.quarantined = true;
                LOG_WARN("DLSS-NR private recording closure failed during resource retirement; slot quarantined");
            }
            else ++retired;
        }
    // Ordinary source rollover is not a resource-rebuild event or a per-frame log.
    if (retired && nextRecording == nullptr)
        LOG_INFO("DLSS-NR closed {} completed private recordings for NR resource retirement", retired);
    TickNrRetired(); // unchanged Token::Complete release gate
}
} // namespace Late

bool WaitForFinishedPicture()
{
    // The wrapper holds Capture::ResizeScope until the real resize returns.
    // The owned Late endpoint takes its matching PresentScope before GetBuffer.
    // Keep inspecting generations even if NR/tracking has since been disabled.
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    // Resize may replace every buffer even when its dimensions are unchanged.
    // Restart only preparation eligibility; all existing waits/owners remain below.
    if (Late::IsZzzProcess()) { Late::zzzPreparation.Restart(); Late::InvalidateZzzSource(); }
    if (Late::pictureQuarantined) return false;
    constexpr size_t ownedCount = std::tuple_size_v<decltype(Late::slots)> +
                                  std::tuple_size_v<decltype(Late::freshColorSlots)>;
    std::array<Late::Slot*, ownedCount> owned {};
    std::array<bool, ownedCount> discard {};
    size_t next = 0;
    for (auto* group : { &Late::slots, &Late::freshColorSlots })
        for (auto& slot : *group)
        {
            owned[next] = &slot;
            // Do not let Cancel erase an unsubmitted/uncertain producer recording.
            if (slot.quarantined || (slot.pending && !slot.submitted) || !slot.nrFilter.DepthSafeToRelease()) return false;
            if (TouchedBackBuffer(slot.ready, slot.done))
            {
                if (!slot.submitted || !slot.commands || !slot.allocator || !slot.fence) return false;
                discard[next] = slot.backBufferIdentity != 0;
            }
            ++next;
        }

    // Snapshot aliases the real R591 tokens; it does not create or close a use.
    // A borrowed game/native list must be reset by its owner, never here.
    const auto snapshot = g_nrGpuWork.Snapshot();
    DlssNrGpuLifetime::Uses uses;
    for (const auto& use : snapshot)
    {
        if (!use) return false;
        size_t index = owned.size();
        for (size_t i = 0; i < owned.size(); ++i)
            if (owned[i]->commands &&
                DetailReuse::RealSubmittedCommands(owned[i]->commands.Get()) == use->listOwner.Get())
            {
                if (index != owned.size()) return false; // ambiguous ownership
                index = i;
            }
        // A NativeProducer/game generation has its own picture domain. Only a
        // token actually retaining a Late backbuffer is affected by this closure.
        bool affected = index != owned.size();
        if (!affected)
            for (const auto& owner : use->owners)
                for (const auto* slot : owned)
                    if (slot->backBufferIdentity &&
                        reinterpret_cast<uintptr_t>(owner.Get()) == slot->backBufferIdentity) affected = true;
        if (!affected) continue;
        // Affected foreign/unknown ownership, quarantine and pending recording
        // remain refusals. Never Reset/Close the NativeProducer or a game list.
        if (index == owned.size() || !use->listOwner || use->quarantined || !use->submitted ||
            use->discarded || !owned[index]->submitted || !owned[index]->backBufferIdentity) return false;
        uses.push_back(use);
        if (!use->closed) discard[index] = true;
        bool submitted = false;
        for (const auto& execution : use->executions)
        {
            if (!execution.queue) continue;
            submitted = true;
            if (!execution.fence || !execution.target ||
                !DlssNrGpuLifetime::SameDevice(use->listOwner.Get(), execution.queue.Get()) ||
                !DlssNrGpuLifetime::SameDevice(use->listOwner.Get(), execution.fence.Get()) ||
                use->listOwner->GetType() != execution.queue->GetDesc().Type) return false;
        }
        if (!submitted) return false;
    }
    for (size_t i = 0; i < owned.size(); ++i)
        if (discard[i] && (!owned[i]->commands || !owned[i]->allocator ||
            !DlssNrGpuLifetime::SameDevice(owned[i]->commands.Get(), owned[i]->allocator.Get()))) return false;

    // One bounded wait budget, always followed by an exact completed-value check.
    // Timeout, device loss and missing fences are refusals, never completion.
    const auto started = GetTickCount64();
    const auto waitExact = [started](ID3D12Fence* fence, UINT64 target) {
        if (!fence || !target) return false;
        const auto before = fence->GetCompletedValue();
        if (before == UINT64_MAX) return false;
        if (before >= target) return true;
        const auto elapsed = GetTickCount64() - started;
        if (elapsed >= 5000) return false;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return false;
        const bool signalled = SUCCEEDED(fence->SetEventOnCompletion(target, event)) &&
            WaitForSingleObject(event, static_cast<DWORD>(5000 - elapsed)) == WAIT_OBJECT_0;
        CloseHandle(event);
        const auto after = fence->GetCompletedValue();
        return signalled && after != UINT64_MAX && after >= target;
    };
    for (const auto* slot : owned)
        // Preserve the capture-only producer/present wait-cycle exclusion.
        if (TouchedBackBuffer(slot->ready, slot->done) && !waitExact(slot->fence.Get(), slot->done)) return false;
    for (const auto& use : uses)
    {
        for (const auto& execution : use->executions)
            if (execution.queue && !waitExact(execution.fence.Get(), execution.target)) return false;
        if (!use->GpuComplete()) return false;
    }

    // Only our private Late lists reach here, with every exact GPU use finished.
    // Real Reset discards the executable generation; the canonical callback then
    // closes its token and retires owners through R591's unchanged Complete gate.
    for (size_t i = 0; i < owned.size(); ++i)
    {
        if (!discard[i]) continue;
        auto& slot = *owned[i];
        auto* identity = DetailReuse::RealSubmittedCommands(slot.commands.Get());
        if (FAILED(slot.allocator->Reset()) || FAILED(slot.commands->Reset(slot.allocator.Get(), nullptr))) return false;
        FinishedPictureResetCommandList(identity); // idempotent fallback if the real Reset hook already notified
        if (g_nrGpuWork.Watches(identity)) return false;
        slot.backBufferIdentity = 0; // actual Reset discarded this private recording
        if (FAILED(slot.commands->Close()))
        {
            slot.quarantined = true; // the new empty recording has unknown state
            return false;
        }
    }
    for (const auto& use : uses)
        if (!use->Complete()) return false;
    Late::Cancel();
    return true;
}

std::string FinishedPictureStatus()
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    return Late::status;
}

static void FinishedPictureSubmittedReal(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    // Every submit in the process comes here, on any thread: only one with a list NR recorded on takes the lock NR
    // holds while it records (DlssNr_WatchedLists.h)
    const auto watched = reinterpret_cast<const void* const*>(lists);
    bool captureWatched = false;
    for (UINT i = 0; i < count; ++i)
        captureWatched |= DlssNrCurrentOutput::Capture::Instance().Watches(lists[i]);
    if (!DlssNrWatchedLists::Any(count, watched) && !captureWatched) return;

    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    // Never unwatch an executable tracked generation between callbacks: another
    // thread's real Execute must not slip through a transient Remove/Add window.
    for (UINT i = 0; i < count; ++i)
        if (!g_nrGpuWork.Watches(lists[i])) DlssNrWatchedLists::Remove(1, watched + i);
    g_nrGpuWork.Submitted(queue, count, lists);
    // Keep observing this executable generation until an actual successful Reset,
    // even after its first fence completes and even when detail reuse is disabled.
    for (UINT i = 0; i < count; ++i)
        if (g_nrGpuWork.Watches(lists[i])) DlssNrWatchedLists::Add(lists[i]);
    DlssNrCurrentOutput::Capture::Instance().Submitted(Late::RealQueue(queue), count, lists);
    if (g_gpuTime) g_gpuTime->Submitted(queue, count, lists);
    if (g_ngxTime) g_ngxTime->Submitted(queue, count, lists);
    for (auto& timer : g_layerTime) if (timer) timer->Submitted(queue, count, lists);
    // Reuse's coverage readbacks: a copy is safe to read once the list it was recorded on has been executed.
    DetailReuse::Submitted(queue, count, lists);
    SceneCut::Submitted(queue, count, lists); // the scene-cut flag's readbacks, the same way
    if (!Late::tracking.load()) return;
    for (auto& slot : Late::slots)
        if (slot.pending && !slot.submitted)
            for (UINT i = 0; i < count; ++i)
                if (lists[i] == slot.producer)
                {
                    // Signal after ExecuteCommandLists, never when merely recording the copy.
                    slot.submitted = true;
                    slot.producerQueue = Late::RealQueue(queue);
                    if (FAILED(queue->Signal(slot.fence.Get(), slot.ready)))
                    {
                        // The copy already executed. Keep its unsignalled fence protecting
                        // the slot instead of treating this as a discarded recording.
                        slot.pending = false;
                        Late::Say("The graphics queue stopped. Restart the game to retry.");
                    }
                    break;
                }
}

thread_local uint64_t finishedPictureSubmitCallbacks = 0;
uint64_t FinishedPictureSubmissionCallbacks() noexcept { return finishedPictureSubmitCallbacks; }
void FinishedPictureSubmitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    ++finishedPictureSubmitCallbacks;
    if (queue == nullptr || lists == nullptr) return;
    ID3D12CommandQueue* realQueue = nullptr;
    if (Util::CheckForRealObject(__FUNCTION__, queue, (IUnknown**) &realQueue)) queue = realQueue;
    // Canonicalize before the watched-list gate. Recording stores real identities;
    // a Streamline proxy passed to ExecuteCommandLists must reach the same owner.
    // Bounded stack chunks avoid heap allocation on the process-wide submit hook.
    std::array<ID3D12CommandList*, 16> realLists {};
    for (UINT offset = 0; offset < count;)
    {
        const UINT batch = (std::min)(count - offset, static_cast<UINT>(realLists.size()));
        for (UINT i = 0; i < batch; ++i)
            realLists[i] = DetailReuse::RealSubmittedCommands(lists[offset + i]);
        FinishedPictureSubmittedReal(queue, batch, realLists.data());
        offset += batch;
    }
}

void FinishedPictureColorSpace(IDXGISwapChain* swapchain, DXGI_COLOR_SPACE_TYPE colorSpace)
{
    swapchain->SetPrivateData(Late::colorSpaceKey, sizeof(colorSpace), &colorSpace);
}

static void ApplyToFinishedPictureInternal(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue, bool dx11Bridge,
                                          ID3D12CommandQueue* orderedProducerQueue = nullptr,
                                          const GenshinPresentNr::ReturnedPicture* nativeReturned = nullptr)
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    Late::finishedPictureOutcome = {};
    Late::finishedPictureOutcome.presentEpoch = State::Instance().frameCount;
    Late::finishedPictureOutcome.endpointSerial = ++Late::finishedPictureEndpointSerial;
    Late::finishedPictureOutcome.bridgeEndpoint = dx11Bridge;
    Late::finishedPictureOutcome.thread = GetCurrentThreadId();
    Late::finishedPictureOutcome.swapchain = DlssNrFgTrace::Identity(swapchain);
    Late::finishedPictureOutcome.owner = Late::finishedPictureOutcome.swapchain;
    Late::finishedPictureOutcome.queue = DlssNrFgTrace::Identity(queue);
    Late::finishedPictureOutcome.producerQueue = DlssNrFgTrace::Identity(orderedProducerQueue);
    Late::finishedPictureOutcome.reason = FinishedPictureReason::InvalidEndpoint;
    // Physical quarantine outlives stage selection and is checked before any
    // current-color capture, native-return shortcut or later NR read.
    if (HoYoShade::ColorConsumers(swapchain) != HoYoShade::ConsumerAccess::Safe)
    {
        Late::finishedPictureOutcome.result = FinishedPictureResult::FailedUncertain;
        Late::finishedPictureOutcome.reason = FinishedPictureReason::InputNotReady;
        return;
    }
    // Share R600's resize admission for the entire real NR endpoint, including
    // local backbuffer COM owners. The wrapper keeps ResizeScope after our mutex
    // is released; no new owned Late generation may enter that resize window.
    DlssNrCurrentOutput::Capture::PresentScope presentAdmission;
    if (!presentAdmission)
    {
        Late::finishedPictureOutcome.reason = FinishedPictureReason::InputNotReady;
        return;
    }
    // Bind the real current color even for a disabled/no-input return. Failed
    // endpoint queries retain zero identities; no successful result is invented.
    if(swapchain && queue) {
        Late::ComPtr<IDXGISwapChain3> endpoint;Late::ComPtr<ID3D12Resource> currentColor;Late::ComPtr<ID3D12Device> endpointDevice;
        if(SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&endpoint))) &&
           SUCCEEDED(endpoint->GetBuffer(endpoint->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&currentColor))) &&
           SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&endpointDevice)))) {
            Late::finishedPictureOutcome.color = DlssNrFgTrace::Identity(currentColor.Get());
            Late::finishedPictureOutcome.device = DlssNrFgTrace::Identity(endpointDevice.Get());
        }
    }
    if(dx11Bridge && (!queue || queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT))return;
    Late::finishedPictureOutcome.reason = FinishedPictureReason::NoCurrentInput;
    if(dx11Bridge && nativeReturned) {
        const auto& state=State::Instance();
        Late::ComPtr<IDXGISwapChain3> endpoint;Late::ComPtr<ID3D12Resource> color;Late::ComPtr<ID3D12Device> device;
        const auto& cfg=*Config::Instance();
        const auto kind=GenshinPresentNr::SelectFgPicture(cfg.DlssNrEnabled.value_or_default(),cfg.DlssNrFinishedPicture.value_or_default(),
            cfg.DlssNrApplyModel.value_or_default(),cfg.DlssNrRunBeforeSr.value_or_default(),cfg.DlssNrDeferredDlss.value_or_default());
        if(kind==GenshinPresentNr::FgPictureKind::Unknown || kind!=nativeReturned->Kind()) {
            Late::finishedPictureOutcome.reason=FinishedPictureReason::InvalidEndpoint;return;
        }
        if(!swapchain || !queue || !orderedProducerQueue || state.swapchainInteropApi!=SwapchainInteropApi::Dx11wDx12 || state.activeFgInput!=FGInput::Upscaler ||
           state.activeFgOutput!=FGOutput::DLSSG || !Config::Instance()->GenshinControlsCurrent(nativeReturned->ControlsEpoch()) ||
           FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&endpoint))) ||
           FAILED(endpoint->GetBuffer(endpoint->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&color))) ||
           FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) ||
           !nativeReturned->Matches(swapchain,queue,orderedProducerQueue,color.Get(),device.Get(),state.frameCount,
               reinterpret_cast<uintptr_t>(state.currentFG))) {
            Late::finishedPictureOutcome.reason=FinishedPictureReason::InvalidEndpoint;return;
        }
        if(GenshinPresentNr::Original(kind)) {
            // Real same-History guides submitted, original B retained, then B
            // copied to this exact provider color and queue-waited. This is no
            // NR evaluation/write/submission; keep all NR completion fields zero.
            Late::finishedPictureOutcome.result=FinishedPictureResult::NoWrite;
            Late::finishedPictureOutcome.reason=kind==GenshinPresentNr::FgPictureKind::OriginalNrDisabled
                ?FinishedPictureReason::Disabled:FinishedPictureReason::ModelDisabled;
            try {Late::finishedPictureOutcome.original=std::make_shared<const GenshinPresentNr::ReturnedPicture>(*nativeReturned);}
            catch(const std::bad_alloc&) {Late::finishedPictureOutcome.reason=FinishedPictureReason::RecordingFailed;}
            return;
        }
        // NativeDriver actually evaluated NR, Return actually copied to native B,
        // and the wrapper copied that B into THIS color and ordered THIS queue.
        // No second model call and no NoWrite/disabled promotion.
        Late::finishedPictureOutcome.result=FinishedPictureResult::WrittenOrdered;
        Late::finishedPictureOutcome.reason=FinishedPictureReason::None;
        Late::finishedPictureOutcome.fence=nativeReturned->Fence();
        Late::finishedPictureOutcome.fenceValue=nativeReturned->Value();
        Late::finishedPictureOutcome.submissionSerial=nativeReturned->Serial();
        return;
    }
    // ZZZ can stop selecting an old native/fallback slot after a route or
    // cadence change. Close completed private generations independently of
    // reuse selection so they cannot permanently block a model rebuild.
    if (!dx11Bridge && Late::IsZzzProcess() && g_nr.feature == nullptr && !g_nrRetired.empty())
        Late::RetireCompletedPrivateRecordings();
    // A physical/generated interop Present is not a new StarRail source picture.
    // Reject that generic entry before it can erase the bridge's coverage.
    if (!dx11Bridge && Late::BridgeContinuationEnabled() &&
        State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12) {
        Late::finishedPictureOutcome.reason=FinishedPictureReason::InvalidEndpoint;return;
    }
    const auto bridgePresent = dx11Bridge ? ++Late::bridgePresentSequence : 0;
    const bool persistentBridge = dx11Bridge && Late::BridgeContinuationEnabled();
    if (!persistentBridge) Late::bridgeCoverage = {};
    const auto modelEpoch = persistentBridge ? Late::bridgeSubmission.Token() : 0;
    if (persistentBridge && !modelEpoch) {
        Late::finishedPictureOutcome.result=FinishedPictureResult::FailedUncertain;
        Late::finishedPictureOutcome.reason=FinishedPictureReason::SubmissionFailed;
        return; // failed submission resources stay pinned
    }
    if (!Config::Instance()->DlssNrFinishedPicture.value_or_default() ||
        !Config::Instance()->DlssNrEnabled.value_or_default())
    {
        Late::finishedPictureOutcome.reason=FinishedPictureReason::Disabled;
        if (!dx11Bridge && Late::IsZzzProcess()) Late::zzzPreparation.Restart();
        Late::Cancel();
        return;
    }
    const auto api = State::Instance().swapchainInteropApi;
    if (!swapchain || !queue || (dx11Bridge ? api != SwapchainInteropApi::Dx11wDx12 : api != SwapchainInteropApi::None)) {
        Late::finishedPictureOutcome.reason=FinishedPictureReason::InvalidEndpoint;return;
    }
    if (!dx11Bridge && Late::PausedForGameFrameGeneration() && !Late::ZzzGameFrameGenerationStale())
    {
        Late::Cancel(!Late::ComposeOnGameFg());
        Late::stillRendering = 0;
        if (!Late::ComposeOnGameFg()) Late::Say(Late::pausedForGameFg);
        return;
    }
    Late::ComPtr<IDXGISwapChain3> sc;
    Late::ComPtr<ID3D12Resource> color;
    Late::ComPtr<ID3D12Device> currentDevice;
    if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&sc))) ||
        FAILED(sc->GetBuffer(sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&color))) ||
        FAILED(queue->GetDevice(IID_PPV_ARGS(&currentDevice))) || currentDevice != Late::device) {
        Late::finishedPictureOutcome.reason=FinishedPictureReason::InvalidEndpoint;return;
    }
    Late::finishedPictureOutcome.swapchain = DlssNrFgTrace::Identity(swapchain);
    Late::finishedPictureOutcome.color = DlssNrFgTrace::Identity(color.Get());
    Late::finishedPictureOutcome.queue = DlssNrFgTrace::Identity(queue);
    const auto desc = color->GetDesc();
    auto colorSpace = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
        ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT colorSpaceSize = sizeof(colorSpace);
    swapchain->GetPrivateData(Late::colorSpaceKey, &colorSpaceSize, &colorSpace);
    const bool screenPq = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    const bool screenScrgb = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    const bool screenSdr = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    // [DlssNr] ColourEncoding: Auto keeps the colour-space rule; a forced choice picks the path, which is what lets
    // an FP16 buffer that holds gamma values through as SDR (DlssNr_ColourEncoding.h).
    using DlssNrColourEncoding::Screen;
    const auto screenChoice = DlssNrColourEncoding::ResolveFinished(
        Config::Instance()->DlssNrColourEncoding.value_or_default(), screenPq || screenScrgb || screenSdr,
        screenPq ? Screen::Pq : screenScrgb ? Screen::Scrgb : Screen::Sdr,
        desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
        desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
    DlssNr::ReportColourEncoding(screenChoice.choice, DiagFormatName(desc.Format), "finished picture");
    if (!screenChoice.supported)
    {
        Late::finishedPictureOutcome.reason=FinishedPictureReason::UnsupportedColour;
        Late::Cancel();
        Late::Say("This screen colour format is not supported.");
        return;
    }
    const bool pq = screenChoice.screen == Screen::Pq;
    const bool scrgb = screenChoice.screen == Screen::Scrgb;
    const auto epoch = State::Instance().frameCount;
    const bool residualOnly = Config::Instance()->DlssNrRunBeforeSr.value_or_default() ||
                              Config::Instance()->DlssNrDeferredDlss.value_or_default();
    const bool nativeStarRail = dx11Bridge && !residualOnly && Late::IsStarRailProcess();
    const bool nativeZzz = !dx11Bridge && !residualOnly && Late::IsZzzProcess();
    const bool zzzReceiptRoute = nativeZzz && Late::zzzSource.routeActive;
    const auto captureEpoch = zzzReceiptRoute ? Late::zzzSource.ordinal :
        persistentBridge && !residualOnly ? Late::bridgeSourceEpoch : epoch;
    const bool nativeFreshColorRoute = nativeStarRail || nativeZzz;
    auto* presentQueue = Late::RealQueue(queue);
    auto* orderedProducer = orderedProducerQueue ? Late::RealQueue(orderedProducerQueue) : nullptr;
    if (zzzReceiptRoute && State::Instance().currentFG != nullptr &&
        (State::Instance().externalFrameGeneration || State::Instance().activeFgInput != FGInput::Upscaler ||
         State::Instance().activeFgOutput != FGOutput::DLSSG ||
         State::Instance().currentFGSwapchain != swapchain))
    {
        // This receipt has no authority on an unrelated endpoint or provider route.
        Late::finishedPictureOutcome.reason = FinishedPictureReason::InvalidEndpoint;
        return;
    }
    // StarRail can leave a completed fresh-color list idle after native guides
    // return. Its executable recording still pins old model/reuse allocations,
    // even while the current model remains active. Close only owned,
    // completed private lists before selecting/recording this source picture;
    // captured producer/game lists and incomplete uses keep their owners.
    if (nativeStarRail && persistentBridge && orderedProducer != nullptr &&
        !g_nrRetired.empty() && (g_nr.feature == nullptr || DetailReuse::HasParkedAllocations()))
        Late::RetireCompletedPrivateRecordings();
    // No fresh-slot allocation, allocator Reset, filter work, conversion or model
    // creation has happened at this point. A defer is an ordinary NR NoWrite; the
    // caller continues the original SR/FG inputs and its normal Present sequence.
    if (nativeZzz && !Late::zzzPreparation.AtEndpoint(DlssNrFgTrace::Identity(swapchain), presentQueue, desc))
    {
        Late::finishedPictureOutcome.reason = FinishedPictureReason::InputNotReady;
        Late::reset = true;
        Late::Say("Deferring ZZZ NR preparation until the SR source is stable.");
        return;
    }
    if (zzzReceiptRoute && (Late::zzzSource.completedAttempt != Late::zzzSource.attempt ||
                            Late::zzzSource.claimedAttempt == Late::zzzSource.attempt))
    {
        // A physical/generated Present is not a new successful source. No second
        // NR/flow recording on the same source, and no guess during an active SR.
        Late::finishedPictureOutcome.reason = FinishedPictureReason::NoCurrentInput;
        return;
    }
    if (nativeFreshColorRoute && Late::nativeSizing.valid &&
        (Late::nativeSizing.owner != DlssNrFgTrace::Identity(swapchain) ||
         Late::pictureQueue.Get() != presentQueue || Late::nativeSizing.colorFormat != desc.Format ||
         Late::nativeSizing.frame.OutputWidth != desc.Width || Late::nativeSizing.frame.OutputHeight != desc.Height))
        Late::nativeSizing.valid = false; // scalar invalidation; in-flight slot resources retain their fences
    // Coverage is local to the successful final-picture owner, shape and presentation queue.
    if (persistentBridge && Late::bridgeCoverage.valid &&
        (Late::bridgeCoverage.owner != DlssNrFgTrace::Identity(swapchain) ||
         Late::pictureQueue.Get() != presentQueue || Late::pictureFormat != desc.Format ||
         Late::bridgeCoverage.width != desc.Width || Late::bridgeCoverage.height != desc.Height ||
         captureEpoch < Late::bridgeCoverage.epoch))
        Late::bridgeCoverage = {};
    std::array<FinishedInput, std::tuple_size_v<decltype(Late::slots)>> inputs {};
    uint64_t latestNativeSerial = 0;
    if (nativeStarRail)
        for (const auto& current : Late::slots)
            if (current.pending && current.dx11Bridge && !current.residualOnly &&
                current.sourceEpoch == captureEpoch)
                latestNativeSerial = std::max(latestNativeSerial, current.serial);
    for (size_t i = 0; i < Late::slots.size(); ++i)
    {
        auto& slot = Late::slots[i];
        if (!slot.pending || !slot.submitted)
            continue;
        // A delayed recording already covered by this owner cannot own the newer picture.
        if (persistentBridge && Late::CoveredBySuccessfulBridge(slot, captureEpoch, DlssNrFgTrace::Identity(swapchain)))
        {
            Late::RetireCapture(slot, captureEpoch, DlssNrFgTrace::Identity(swapchain));
            continue;
        }
        const bool zzzIdentity = zzzReceiptRoute && Late::zzzSource.succeeded &&
            slot.zzzAttempt == Late::zzzSource.completedAttempt && slot.zzzOrdinal != 0 &&
            slot.zzzOrdinal == captureEpoch && slot.zzzProducer.Get() == slot.producer;
        if ((zzzReceiptRoute && !zzzIdentity) || (!zzzReceiptRoute &&
            (captureEpoch < slot.sourceEpoch || captureEpoch - slot.sourceEpoch > 1 ||
             (dx11Bridge && captureEpoch != slot.sourceEpoch))))
        {
            Late::RetireCapture(slot, captureEpoch, DlssNrFgTrace::Identity(swapchain));
            continue;
        }
        const bool bridgeOrdered = BridgeProducerOrdered(slot.dx11Bridge, slot.bridgeOutputHandoff,
                                                         dx11Bridge && orderedProducer != nullptr,
                                                         slot.producerQueue.Get() == orderedProducer);
        const bool nativeIdentity = Late::NativeCaptureIdentityMatches(slot, captureEpoch, bridgeOrdered);
        inputs[i] = { (!nativeStarRail || (nativeIdentity && slot.serial == latestNativeSerial)) && slot.dx11Bridge == dx11Bridge && slot.residualOnly == residualOnly && slot.frame.OutputWidth == desc.Width &&
                          slot.frame.OutputHeight == desc.Height,
                      slot.serial, slot.producerQueue.Get() == presentQueue || bridgeOrdered, slot.fence->GetCompletedValue(),
                      slot.ready };
    }
    // Native presents cannot wait on pending work from another queue. The explicit DX11 bridge
    // handoff is already ordered by its producer->DX11->copy->present chain.
    size_t index = 0;
    const auto pick = PickFinishedInput(inputs.data(), inputs.size(), index);
    // Per-present counters distinguish missing captures from completion skips and model bypass.
    // Log once per 300 bridge presents; no GPU readback or waiting is introduced.
    static uint64_t bridgePresents = 0, bridgeMissing = 0, bridgeNotReady = 0, bridgeRan = 0, bridgeBypass = 0;
    if (dx11Bridge)
    {
        ++bridgePresents;
        bridgeMissing += pick == FinishedPick::None;
        bridgeNotReady += pick == FinishedPick::NotReady;
        if (bridgePresents % 300 == 0)
            LOG_INFO("DLSS-NR bridge continuity: presents={} missing={} notReady={} applied={} bypass={} epoch={}",
                     bridgePresents, bridgeMissing, bridgeNotReady, bridgeRan, bridgeBypass, epoch);
    }
    const bool freshColorFallback =
        (pick == FinishedPick::None || (pick == FinishedPick::NotReady && nativeZzz)) &&
        ((nativeStarRail && persistentBridge && orderedProducer != nullptr) || nativeZzz);
    Late::Slot* selected = nullptr;
    if (pick == FinishedPick::None || (pick == FinishedPick::NotReady && freshColorFallback))
    {
        if (nativeFreshColorRoute) { Late::reset = true; Late::bridgeCoverage = {}; }
        if (freshColorFallback)
            selected = Late::AcquireFreshColor(DlssNrFgTrace::Identity(swapchain), desc, presentQueue, dx11Bridge, nativeZzz);
        if (!selected)
        {
            if (nativeFreshColorRoute) Late::Say("NR bypass: no current capture and no safe fresh-color slot/sizing.");
            if (nativeZzz)
            {
                static uint64_t refusals = 0;
                if (++refusals <= 16 || refusals % 300 == 0)
                    LOG_INFO("DLSS-NR ZZZ fresh-color refusal: count={} pick={} endpoint={} nativeSizing={} freshActive={}",
                             refusals, static_cast<unsigned>(pick), Late::finishedPictureOutcome.submissionSerial,
                             Late::nativeSizing.valid, Late::freshColorActive);
            }
            if (pick == FinishedPick::None) return;
        }
    }
    if (pick == FinishedPick::NotReady && !selected)
    {
        Late::finishedPictureOutcome.reason=FinishedPictureReason::InputNotReady;
        // Stays pending in case another present of this frame follows; history skips a frame.
        Late::reset = true;
        const auto& input = inputs[index];
        if (!input.sameQueue && input.completed != UINT64_MAX && !Late::loggedCrossQueue)
        {
            Late::loggedCrossQueue = true;
            LOG_INFO("DLSS-NR finished picture: the game renders on a different queue than it presents on; "
                     "frames still rendering at present are skipped instead of waited for");
        }
        // Say changes are logged: only after a sustained run, not on every alternation.
        if (++Late::stillRendering == 120)
            Late::Say("Skipping frames: the game's picture is still rendering when it is presented.");
        return;
    }
    Late::stillRendering = 0;
    auto& slot = selected ? *selected : Late::slots[index];
    Late::finishedPictureOutcome.submissionSerial=slot.serial;
    Late::finishedPictureOutcome.fence=slot.fence;
    Late::finishedPictureOutcome.fenceValue=slot.ready; // captured work only; not a fabricated new completion
    if (nativeFreshColorRoute && !freshColorFallback)
        Late::RememberNativeSizing(slot, DlssNrFgTrace::Identity(swapchain), desc.Format);
    // Drop other submitted evaluates only when an actual capture owns this picture.
    if (!freshColorFallback)
        for (auto& other : Late::slots)
            if (other.submitted && other.serial <= slot.serial)
                other.pending = false;
    // ZZZ source gaps can leave the last history writer on a different, now
    // idle private list. Its GPU fence may be complete while the executable
    // generation is still open: adjacent-source Advance must reject the gap,
    // and Collect must retain the real owners until a successful Reset.
    // Close only completed old private lists through the existing lifecycle.
    // The selected list is reset normally below; producer/game lists are never
    // reset here. History compatibility/adjacency and every GPU gate stay intact.
    if (nativeZzz && !freshColorFallback && DetailReuse::PendingRecordings() != 0)
        Late::RetireCompletedPrivateRecordings(&slot);
    // Claim before any private recording/uncertain queue work. NotReady returns
    // above may retry; a recorded/failing source may not run twice on child Presents.
    if (zzzReceiptRoute) Late::zzzSource.claimedAttempt = Late::zzzSource.completedAttempt;
    // Same queue: orders after the producer's signal. Another queue: already complete, so this never blocks.
    if ((!freshColorFallback && FAILED(queue->Wait(slot.fence.Get(), slot.ready))) ||
        // This seam precedes the new private command list: the queue already
        // orders genuine current-color work (bridge upload + Wait included).
        // Only full FinishedPicture NR consumes this color; residual mode
        // already evaluated NR elsewhere and keeps its previous contract.
        (!slot.residualOnly && !slot.nrFilter.BeforeNr(swapchain, queue, color.Get(),
            screenSdr, epoch, dx11Bridge, orderedProducerQueue,
            {(!freshColorFallback && slot.frame.SourceCaptureSerial == slot.serial &&
              slot.frame.SourceCaptureEpoch == slot.frame.SubmissionEpoch) ? slot.depth.Get() : nullptr,
             slot.producerQueue.Get(), slot.fence.Get(), slot.ready, slot.serial, slot.frame.SourceCaptureEpoch,
             slot.filterDepthRegion, slot.frame.NativeFeatureFlagsKnown, slot.frame.DepthInverted})) ||
        FAILED(slot.allocator->Reset()) ||
        FAILED(slot.commands->Reset(slot.allocator.Get(), nullptr)))
    {
        Late::finishedPictureOutcome.result=FinishedPictureResult::FailedUncertain;
        Late::finishedPictureOutcome.reason=FinishedPictureReason::RecordingFailed;
        Late::finishedPictureOutcome.submissionSerial=slot.serial;
        Late::finishedPictureOutcome.fence=slot.fence;
        Late::finishedPictureOutcome.fenceValue=slot.ready;
        Late::reset = true;
        Late::Say("Could not prepare the finished picture.");
        return;
    }
    // This identity names only the real buffer bound by the new private Late
    // recording. R591 retains it when dispatched; resize never dereferences it.
    slot.backBufferIdentity = reinterpret_cast<uintptr_t>(static_cast<IUnknown*>(color.Get()));
    auto* cmd = slot.commands.Get();
    if (freshColorFallback) Late::RecordZeroMotion(slot);
    if (!g_compose)
        g_compose = std::make_unique<DlssNr_Dx12>("Neural Rendering", Late::device.Get());
    const auto before = g_nr.successfulDispatches;
    bool appliedResidual = false;
    bool pictureRecorded = false;
    bool pictureUpdated = false;
    bool pictureFlowValid = false, pictureGuidesBuilt = false;
    bool effectiveModelReset = false, effectiveNativeAuthority = false;
    // This is the source picture before fg->Present(), not an SDK child-frame
    // callback. The successful source capture, exact endpoint/shape and actual
    // producer readiness were checked above; feedback only reads THIS raw color.
    const bool zzzExactPicture = zzzReceiptRoute && !freshColorFallback &&
        slot.zzzOrdinal == captureEpoch && slot.zzzAttempt == Late::zzzSource.completedAttempt;
    const auto zzzInterval = Config::Instance()->DlssNrDetailReuseInterval.value_or_default();
    const bool zzzFlowReuseRequested = zzzExactPicture &&
        Config::Instance()->DlssNrDetailReuse.value_or_default() && zzzInterval > 1 && zzzInterval <= 8;
    const bool zzzFlowRequested = zzzExactPicture &&
        ((Config::Instance()->DlssNrPresentedTemporalDetail.value_or_default() && zzzInterval == 1) ||
         zzzFlowReuseRequested) &&
        !(desc.Width & 1) && !(desc.Height & 1); // half-size flow has an exact 2x pixel pitch
    if (slot.residualOnly)
    {
        if (slot.encoded && (slot.encoded->GetDesc().Width != desc.Width ||
            slot.encoded->GetDesc().Height != desc.Height || slot.encoded->GetDesc().Format != desc.Format))
            slot.encoded.Reset();
        if (!slot.encoded)
            slot.encoded.Attach(CreateScratch(Late::device.Get(), desc.Format, (unsigned) desc.Width, desc.Height));
        if (slot.encoded && Config::Instance()->DlssNrApplyModel.value_or_default())
        {
            Barrier(cmd, slot.residual.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            DlssNrConstants apply {};
            apply.Mode = pq ? 4 : scrgb ? 3 : 2;
            apply.Width = (unsigned) desc.Width; apply.Height = desc.Height;
            apply.WhitePoint = slot.frame.PreExposure;
            apply.TransferStrength = slot.sceneLinear ? 1.0f : 0.0f;
            apply.MaxRatio = std::clamp(Config::Instance()->DlssNrMaxRatio.value_or_default(), 1.0f, 8.0f);
            appliedResidual = g_compose->DispatchResidualPass(cmd, apply, color.Get(), nullptr,
                slot.residual.Get(), nullptr, slot.encoded.Get());
            if (appliedResidual)
            {
                Barrier(cmd, slot.encoded.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                cmd->CopyResource(color.Get(), slot.encoded.Get());
                Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cmd, slot.encoded.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT);
            Barrier(cmd, slot.residual.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        }
    }
    else
    {
        auto frame = slot.frame;
        frame.ColourIsLinearHdr = pq || scrgb;
        frame.InputEncoding = screenChoice.shaderConversion; // gamma 2.2 re-encoded as sRGB by the shared pass
        frame.ColourEncoding = (uint32_t) screenChoice.choice.encoding;
        frame.ColourEncodingForced = !screenChoice.choice.automatic;
        // Absolute display encodings: 203-nit reference white, in 80-nit scRGB units.
        frame.WhitePointOverride = (pq || scrgb) ? 203.0f / 80.0f : 0.0f;
        frame.Reset |= Late::reset;
        // Slot selection uses the source-picture epoch above. Model creation and
        // cadence use this endpoint's committed lists even while SR is stopped.
        frame.SubmissionEpoch = persistentBridge ? modelEpoch : epoch;
        {
            if (slot.depth) Barrier(cmd, slot.depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd, slot.motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        DlssNrNative::SetPrecision(Config::Instance()->DlssNrPrecision.value_or_default());
        ID3D12Resource* nrColor = color.Get();
        bool colorReady = true;
        DlssNrConstants conversion {};
        conversion.Width = (unsigned) desc.Width;
        conversion.Height = desc.Height;
        if (pq)
        {
            auto ensure = [&](Late::ComPtr<ID3D12Resource>& resource, DXGI_FORMAT format) {
                if (resource && (resource->GetDesc().Width != desc.Width || resource->GetDesc().Height != desc.Height ||
                                 resource->GetDesc().Format != format)) resource.Reset();
                if (!resource) resource.Attach(CreateScratch(Late::device.Get(), format, (unsigned) desc.Width, desc.Height));
                return resource != nullptr;
            };
            colorReady = ensure(slot.linear, DXGI_FORMAT_R16G16B16A16_FLOAT) && ensure(slot.encoded, desc.Format);
            if (colorReady)
            {
                Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                colorReady = g_compose->DispatchResidualPass(cmd, conversion, color.Get(), nullptr, nullptr, nullptr,
                                                           slot.linear.Get());
                Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT);
                // Dispatch reads the converted colour; its transition out of UAV orders the conversion.
                nrColor = slot.linear.Get();
                frame.OutputArrivalState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
        }
        ID3D12Resource* modelMotion = slot.motion.Get();
        ID3D12Resource* modelDepth = slot.depth.Get();
        if (nativeZzz && zzzExactPicture)
        {
            // Save the real source BEFORE final-picture flow replaces its motion fields.
            frame.SceneGuideSourceVerified = frame.NativeGuideMetadataKnown &&
                slot.frame.OutputWidth == desc.Width && slot.frame.OutputHeight == desc.Height;
            frame.SceneGuideMotion = slot.motion.Get();
            frame.SceneMotionWidth = frame.MotionVectorsLowResolution ? frame.RenderSubrectWidth : frame.OutputWidth;
            frame.SceneMotionHeight = frame.MotionVectorsLowResolution ? frame.RenderSubrectHeight : frame.OutputHeight;
            frame.SceneMotionBaseX = frame.MotionSubrectBaseX; frame.SceneMotionBaseY = frame.MotionSubrectBaseY;
            frame.SceneMvScaleX = frame.MvScaleX; frame.SceneMvScaleY = frame.MvScaleY;
            frame.SourceCaptureSerial = captureEpoch;
            frame.SourceCaptureEpoch = captureEpoch;
        }
        if (nativeStarRail)
        {
            const auto owner = DlssNrFgTrace::Identity(swapchain);
            if (Late::pictureOwner != owner || Late::pictureQueue.Get() != presentQueue ||
                Late::pictureWidth != desc.Width || Late::pictureHeight != desc.Height ||
                Late::pictureFormat != desc.Format || Late::pictureLastBridgePresent + 1 != bridgePresent)
                frame.Reset = true;
            // This is the selected StarRail native presented base endpoint. Keep route identity
            // independent from guide/metadata validity so caller-v5 can choose the native core
            // fail-closed path even when the scene guide contract is unknown.
            frame.NativeSceneRoute = true;
            if (!freshColorFallback)
                Late::PopulateNativeSceneContract(frame, slot, captureEpoch, desc, presentQueue, orderedProducer);
            // Fallback keeps NativeSceneRoute but no identity/guide authority. Reset is
            // unconditional; zeros satisfy the model ABI, not a claim of static motion.
        }
        const bool zzzHistoryChanged = nativeZzz && (Late::pictureQueue.Get() != presentQueue ||
            Late::pictureWidth != desc.Width || Late::pictureHeight != desc.Height ||
            Late::pictureFormat != desc.Format);
        // ZZZ measures the actual final color here, before NR mutates it. Scene
        // depth keeps its independent SR subrect and encoding for the model; it
        // never qualifies postprocess/HUD depth; any geometry veto is separate.
        if (!nativeStarRail && (slot.dx11Bridge || zzzFlowRequested) && colorReady &&
            Late::PictureHistoryReady(presentQueue, desc))
        {
            const auto owner = DlssNrFgTrace::Identity(swapchain);
            const bool zzzGap = nativeZzz && (zzzHistoryChanged || !Late::zzzPicture.ordinal ||
                Late::zzzPicture.ordinal + 1 != captureEpoch || Late::zzzPicture.source != slot.zzzSource ||
                Late::zzzPicture.encoding != frame.ColourEncoding || slot.frame.Reset);
            if (Late::pictureOwner != owner || (nativeZzz ? zzzGap :
                Late::pictureLastBridgePresent + 1 != bridgePresent))
            {
                Late::TraceResetReason(Late::pictureOwner != owner ? "picture-owner-changed" : "picture-endpoint-gap",
                                       Late::endpointGapEvents, slot);
                Late::pictureFlow->Reset();
                Late::pictureTrust->Reset();
                if (nativeZzz && Late::zzzPicture.guideEpoch != UINT64_MAX) ++Late::zzzPicture.guideEpoch;
                frame.Reset = true;
            }
            // A real source reset restarts ZZZ flow above. Model-only warm-up/reset
            // does not discard this independently measured raw-color pair; its
            // feedback history still resets via frame.Reset and the model owner.
            const auto state = (D3D12_RESOURCE_STATES) frame.OutputArrivalState;
            Barrier(cmd, nrColor, state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            const auto encoding = pq || scrgb ? OpticalFlowDx12::Encoding::ScRgb : OpticalFlowDx12::Encoding::Srgb;
            pictureRecorded = true; // even a failed recording may reference history resources
            const bool flowRan = Late::pictureFlow->Dispatch(cmd, nrColor, nrColor->GetDesc().Format,
                                                          nullptr, DXGI_FORMAT_UNKNOWN, frame.DepthInverted, encoding);
            pictureUpdated = flowRan; // a successful warm-up also updates the current raw picture
            pictureFlowValid = flowRan && Late::pictureFlow->FlowValid();
            if (flowRan && Late::pictureFlow->FlowValid() &&
                (!nativeZzz || Late::pictureFlow->SceneCutFlag()))
            {
                TrustMaskDx12::Inputs in {};
                in.flow = Late::pictureFlow->Flow();
                in.flowWidth = Late::pictureFlow->FlowWidth(); in.flowHeight = Late::pictureFlow->FlowHeight();
                in.fullPerFlow = nativeZzz ? 2.0f : (float) desc.Width / (float) in.flowWidth;
                in.lumaNow = Late::pictureFlow->LumaOfLastFrame();
                in.lumaBefore = Late::pictureFlow->LumaOfFrameBefore();
                in.sceneCut = Late::pictureFlow->SceneCutFlag();
                if (Late::pictureTrust->Dispatch(cmd, in) &&
                    Late::pictureTrust->BuildGuides(cmd, in, (unsigned) desc.Width, desc.Height))
                {
                    modelMotion = Late::pictureTrust->GuideMotion();
                    frame.MotionIsPictureFlow = true; // canonical final-picture flow; depth retains its capture provenance
                    frame.MotionMatchesPicture = true;
                    frame.MotionVectorsLowResolution = false;
                    frame.MotionSubrectBaseX = 0; frame.MotionSubrectBaseY = 0;
                    frame.MvScaleX = 1.0f; frame.MvScaleY = 1.0f;
                    frame.HistoryDistrust = Late::pictureTrust->Mask();
                    pictureGuidesBuilt = true;
                    if (nativeZzz)
                    {
                        // BuildGuides replaces motion only. Keep the current source's
                        // captured scene depth, render extent, subrect and reversed-Z
                        // for the model; picture flow has its own output-sized rect.
                        // The consumer keeps model scene-depth independent from
                        // its conditional geometry veto; it never calls it picture-depth.
                        frame.PictureFlowModelSceneDepth = modelDepth != nullptr;
                        frame.DepthIsPictureAligned = false;
                        frame.NativeSceneRoute = false;
                        frame.NativeBaseIdentityVerified = frame.NativeCurrentDepthVerified = false;
                        frame.NativeGuideContractVerified = frame.NativeGuidePhaseVerified = false;
                        frame.OrderedPictureReuseQueue = true;
                        frame.OrderedPictureReuseOwner[0] = owner;
                        frame.OrderedPictureReuseOwner[1] = reinterpret_cast<uintptr_t>(Late::device.Get());
                        frame.OrderedPictureReuseOwner[2] = slot.zzzSource.ngxHandle;
                        frame.OrderedPictureReuseOwner[3] = reinterpret_cast<uintptr_t>(slot.zzzSource.virtualContext);
                        frame.OrderedPictureReuseOwner[4] = slot.zzzSource.virtualGeneration;
                        frame.OrderedPictureReuseOwner[5] = Late::zzzPicture.guideEpoch;
                        frame.PictureTemporalRoute = true;
                        frame.PictureTemporalGuidesVerified = false; // no mapped final-picture depth
                        const bool measuredFlow = Late::zzzPicture.guideEpoch != UINT64_MAX &&
                            modelMotion && frame.HistoryDistrust && Late::pictureFlow->SceneCutFlag();
                        frame.PictureFlowGuidesVerified = measuredFlow && zzzInterval == 1;
                        frame.PictureFlowReuseGuidesVerified = measuredFlow && zzzFlowReuseRequested;
                        frame.PictureGuideEpoch = Late::zzzPicture.guideEpoch;
                        frame.PictureTemporalReset = frame.Reset;
                        // Use the real source clock, never physical/generated Present counts.
                        frame.SubmissionEpoch = captureEpoch;
                    }
                }
            }
            Barrier(cmd, nrColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, state);
        }
        effectiveModelReset = frame.Reset;
        effectiveNativeAuthority = frame.NativeGuideContractVerified;
        if (colorReady && modelMotion)
        {
            DlssNrCurrentOutput::DeferEndpoint captureAtFinalEndpoint;
            g_compose->Dispatch(cmd, nrColor, modelDepth, modelMotion, nrColor, frame, queue);
        }
        if (pq && colorReady && g_nr.successfulDispatches > before && Config::Instance()->DlssNrApplyModel.value_or_default())
        {
            conversion.Mode = 1;
            Barrier(cmd, slot.linear.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (g_compose->DispatchResidualPass(cmd, conversion, slot.linear.Get(), nullptr, color.Get(), nullptr,
                                               slot.encoded.Get()))
            {
                Barrier(cmd, slot.encoded.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                cmd->CopyResource(color.Get(), slot.encoded.Get());
                Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cmd, slot.encoded.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT);
            Barrier(cmd, slot.linear.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        {
            Barrier(cmd, slot.motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            if (slot.depth) Barrier(cmd, slot.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        }
    }
    if (State::Instance().swapchainApi != DX12 && State::Instance().api != Vulkan &&
        State::Instance().swapchainApi != Vulkan && State::Instance().swapchainInteropApi != SwapchainInteropApi::VkwDx12 &&
        DlssNrCurrentOutput::Capture::Instance().Requested())
    {
        DlssNrCurrentOutput::Metadata metadata;
        metadata.epoch = epoch; metadata.dispatch = g_frames;
        metadata.encoding = pq ? 4u : scrgb ? 1u : 3u;
        metadata.hdr = pq || scrgb; metadata.debugView = Config::Instance()->DlssNrDebugView.value_or_default();
        metadata.guidePreview = Config::Instance()->DlssNrGuidePreview.value_or_default();
        metadata.modelApplied = g_nr.successfulDispatches > before && Config::Instance()->DlssNrApplyModel.value_or_default();
        metadata.selectionKnown = false;
        metadata.stage = "finished-picture-after-PQ-and-final-writeback-before-Opt-UI";
        DlssNrCurrentOutput::Capture::Instance().Record(cmd, Late::device.Get(), color.Get(),
            D3D12_RESOURCE_STATE_PRESENT, 0, 0, unsigned(desc.Width), desc.Height,
            metadata, NrRecording(Late::device.Get(), cmd), Late::RealQueue(queue), nullptr, false,
            DetailReuse::RealTemporalCommands(cmd));
    }
    if (FAILED(cmd->Close()))
    {
        Late::finishedPictureOutcome.result = FinishedPictureResult::FailedUncertain;
        Late::finishedPictureOutcome.reason = FinishedPictureReason::RecordingFailed;
        if (persistentBridge) Late::bridgeSubmission.Quarantine();
        Late::Say("Could not finish the picture. Restart the game to retry.");
        if (pictureRecorded) Late::pictureQuarantined = true;
        slot.pending = true; // quarantine the slot; do not reuse possibly recorded NR resources
        slot.submitted = false;
        if (freshColorFallback) slot.quarantined = true;
        return;
    }
    ID3D12CommandList* lists[] = { cmd };
    slot.done = slot.ready + 1;
    Late::finishedPictureOutcome.submissionSerial=slot.serial;
    Late::finishedPictureOutcome.fence=slot.fence;
    Late::finishedPictureOutcome.fenceValue=slot.done;
    const auto observedBeforeSubmit = finishedPictureSubmitCallbacks;
    queue->ExecuteCommandLists(1, lists);
    slot.submitted = true; // fresh-colour lists have no capture submission hook to mark them
    // The global hook may already have delivered this exact execution. Calling
    // twice would turn one producer into a replay and discard its publication.
    if (finishedPictureSubmitCallbacks == observedBeforeSubmit)
        FinishedPictureSubmitted(queue, 1, lists); // queues without the global submit hook
    if (FAILED(queue->Signal(slot.fence.Get(), slot.done)))
    {
        Late::finishedPictureOutcome.result = FinishedPictureResult::FailedUncertain;
        Late::finishedPictureOutcome.reason = FinishedPictureReason::SubmissionFailed;
        if (persistentBridge) Late::bridgeSubmission.Quarantine();
        Late::Say("The graphics queue stopped. Restart the game to retry.");
        if (pictureRecorded) Late::pictureQuarantined = true;
        if (freshColorFallback) { slot.pending = true; slot.quarantined = true; }
        return;
    }
    if (persistentBridge && !Late::bridgeSubmission.Commit(modelEpoch))
    {
        Late::finishedPictureOutcome.result = FinishedPictureResult::FailedUncertain;
        Late::finishedPictureOutcome.reason = FinishedPictureReason::SubmissionFailed;
        return;
    }
    if (pictureRecorded)
    {
        auto& completion = Late::pictureCompletion[Late::pictureSequence++ % Late::pictureCompletion.size()];
        completion.fence = slot.fence; completion.value = slot.done;
        if (pictureUpdated)
        {
            Late::pictureLastBridgePresent = bridgePresent;
            Late::pictureOwner = DlssNrFgTrace::Identity(swapchain);
            if (nativeZzz)
            {
                Late::zzzPicture.ordinal = captureEpoch;
                Late::zzzPicture.source = slot.zzzSource;
                Late::zzzPicture.encoding = (uint32_t) screenChoice.choice.encoding;
            }
        }
    }
    // Fallback must not overwrite the flow owner's actual allocation/queue shape:
    // the next measured picture must drain/recreate it through PictureHistoryReady.
    if (nativeFreshColorRoute && (!nativeZzz || !Late::pictureFlow))
    {
        Late::pictureOwner = DlssNrFgTrace::Identity(swapchain);
        Late::pictureQueue = presentQueue;
        Late::pictureWidth = desc.Width; Late::pictureHeight = desc.Height; Late::pictureFormat = desc.Format;
        Late::pictureLastBridgePresent = bridgePresent;
    }
    const bool ran = slot.residualOnly ? appliedResidual : g_nr.successfulDispatches > before;
    Late::finishedPictureOutcome.submissionSerial = slot.serial;
    Late::finishedPictureOutcome.fenceValue = slot.done;
    if (ran)
    {
        Late::finishedPictureOutcome.result = FinishedPictureResult::WrittenOrdered;
        Late::finishedPictureOutcome.reason = FinishedPictureReason::None;
    }
    else
    {
        Late::finishedPictureOutcome.result = FinishedPictureResult::NoWrite;
        Late::finishedPictureOutcome.reason = FinishedPictureReason::ProviderFailed;
    }
    if (dx11Bridge)
    {
        bridgeRan += ran;
        bridgeBypass += !ran;
        DlssNrFgTrace::NrSubmitted(epoch, color.Get(), cmd, queue, slot.fence.Get(), slot.done, ran);

    }
    if (freshColorFallback) Late::bridgeCoverage = {};
    else Late::CommitBridgeCoverage(slot, persistentBridge, ran, pictureUpdated || nativeStarRail, DlssNrFgTrace::Identity(swapchain));
    // Native recovery must not inherit a model history evaluated without current guides.
    Late::reset = !ran || freshColorFallback;
    static uint64_t freshColorRuns = 0, nativeRuns = 0, continuityBypasses = 0;
    freshColorRuns += ran && freshColorFallback;
    nativeRuns += ran && nativeStarRail && !freshColorFallback;
    continuityBypasses += !ran;
    static uint64_t lastRouteLog = 0;
    if (nativeStarRail && (!lastRouteLog || bridgePresents % 300 == 0 ||
        (Late::freshColorActive != freshColorFallback && bridgePresents - lastRouteLog >= 60)))
    {
        lastRouteLog = bridgePresents;
        LOG_INFO("DLSS-NR StarRail continuity route={} native={} freshColorResetFallback={} bypass={} modelEpoch={} "
                 "reason={} temporalAuthority={} modelReset={}",
                 freshColorFallback ? "fresh-current-color" : "native-capture", nativeRuns, freshColorRuns,
                 continuityBypasses, modelEpoch, freshColorFallback ? "no-current-native-capture" : "current-native-capture",
                 effectiveNativeAuthority, effectiveModelReset);
    }
    if (freshColorFallback && ran && (freshColorRuns == 1 || freshColorRuns % 300 == 0))
        LOG_INFO("DLSS-NR fresh-color continuity: game={} runs={} source=current-backbuffer depth=none motion=explicit-zero temporalReset=true",
                 nativeZzz ? "ZZZ" : "StarRail", freshColorRuns);
    Late::freshColorActive = freshColorFallback;
    const char* endpointStatus = !Config::Instance()->DlssNrApplyModel.value_or_default() ? "NR changes are hidden." :
        ran ? (slot.residualOnly ? "Applying the pre-SR changes to the finished picture." :
        freshColorFallback ? "Applying NR to current color; temporal history reset (native guides unavailable)." :
        "Applying NR to the finished picture.") : "Preparing NR for the finished picture.";
    // Route oscillation is counted above with a bounded log rate, not one line per present.
    if (nativeStarRail) Late::status = endpointStatus;
    else Late::Say(endpointStatus);
    if (ran && (++Late::successes == 1 || Late::successes % 300 == 0))
        LOG_INFO("DLSS-NR finished picture: {} frames, {}x{}, FG {}", Late::successes,
                 desc.Width, desc.Height, State::Instance().currentFG &&
                 State::Instance().currentFG->IsActive() && !State::Instance().currentFG->IsPaused());
}

FinishedPictureOutcome ApplyToFinishedPicture(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue)
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    ApplyToFinishedPictureInternal(swapchain, queue, false);
    return Late::finishedPictureOutcome;
}

bool CaptureFinishedPictureDx11Bridge(ID3D12GraphicsCommandList* cmd, ID3D12CommandQueue* queue,
                                     NVSDK_NGX_Parameter* params, bool rr, unsigned retainedFlags,
                                     bool retainedFlagsKnown, UpscaleSource source)
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    const auto rejectCapture = [] {
        if (Late::IsStarRailProcess())
        {
            // A newer failed offer invalidates an older same-epoch capture without
            // releasing or recycling any recorded/submitted resource.
            ++Late::nativeCaptureAttempt;
            Late::nativeCaptureAttemptEpoch = Late::CaptureEpoch(true);
            Late::reset = true;
        }
        return false;
    };
    const auto& cfg = *Config::Instance();
    if (!cfg.DlssNrEnabled.value_or_default() || !cfg.DlssNrFinishedPicture.value_or_default() ||
        cfg.DlssNrRunBeforeSr.value_or_default() || cfg.DlssNrDeferredDlss.value_or_default() ||
        State::Instance().swapchainInteropApi != SwapchainInteropApi::Dx11wDx12 || !cmd || !queue || !params ||
        cmd->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return rejectCapture();
    Late::ComPtr<ID3D12Device> listDevice, queueDevice;
    if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&listDevice))) || FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) ||
        listDevice != queueDevice)
        return rejectCapture();
    // Before SR: DX11 shared guides arrive in COMMON after the interop wait and return to COMMON.
    // Native/FG child-frame callers retain their guards.
    Late::Capture(cmd, params, rr, true, retainedFlags, retainedFlagsKnown, source);
    return true;
}

void FinishedPictureDx11BridgeOutputHandoff(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* cmd)
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    if (!queue || !cmd) return;
    ID3D12GraphicsCommandList* real = nullptr;
    ID3D12CommandList* producer = cmd;
    if (Util::CheckForRealObject(__FUNCTION__, cmd, (IUnknown**) &real)) producer = real;
    auto* producerQueue = Late::RealQueue(queue);
    for (auto& slot : Late::slots)
        if (slot.dx11Bridge && slot.pending && slot.submitted && slot.producer == producer &&
            slot.producerQueue.Get() == producerQueue)
            slot.bridgeOutputHandoff = true;
}

FinishedPictureOutcome ApplyToFinishedPictureDx11Bridge(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue,
                                     ID3D12CommandQueue* orderedProducerQueue,
                                     const GenshinPresentNr::ReturnedPicture* nativeReturned)
{
    // Return this exact invocation under the owning lock, including invalid,
    // disabled, no-input and uncertain paths. Never reuse a later generic result.
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    ApplyToFinishedPictureInternal(swapchain, queue, true, orderedProducerQueue, nativeReturned);
    // This entry follows the actual source copy/order, before hidden, physical
    // or generated Presents. A skipped NR picture also consumes its boundary.
    if (Late::BridgeContinuationEnabled() && Late::bridgeSourceEpoch != UINT64_MAX)
        ++Late::bridgeSourceEpoch;
    auto outcome=Late::finishedPictureOutcome;
    // The receipt is consumed on this Present stack. The global diagnostic
    // outcome must not retain native B across Resize/Release on Original.
    Late::finishedPictureOutcome.original.reset();
    return outcome;
}

DXGI_COLOR_SPACE_TYPE NativeInputColourSpace(IDXGISwapChain* swapchain, DXGI_FORMAT format)
{
    auto colorSpace = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                                                : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT colorSpaceSize = sizeof(colorSpace);

    if (swapchain != nullptr)
        swapchain->GetPrivateData(Late::colorSpaceKey, &colorSpaceSize, &colorSpace);

    return colorSpace;
}

bool NativeInputBlockedBySwapChainInterop()
{
    return State::Instance().swapchainInteropApi != SwapchainInteropApi::None;
}

bool ApplyNativeInput(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* cmd, ID3D12Resource* color,
                      ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* historyDistrust,
                      bool depthReversed, bool reset,
                      DXGI_COLOR_SPACE_TYPE colorSpace, D3D12_RESOURCE_STATES pictureState, unsigned long long epoch, bool opticalFgNr, const GenshinPresentNr::Scope* genshinSender)
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    if (!Config::Instance()->DlssNrFinishedPicture.value_or_default() ||
        !Config::Instance()->DlssNrEnabled.value_or_default())
    {
        Late::Say("Native input needs Finished picture and Enable Neural Rendering on.");
        return false;
    }
    // Depth is optional: NR runs without it.
    const bool opticalBridge = opticalFgNr && native::OpticalFgNrEnabled() &&
        State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12 &&
        cmd && cmd->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT &&
        queue && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT;
    const auto& state=State::Instance();
    const bool genshinBridge=genshinSender && genshinSender==GenshinPresentNr::Scope::Current() &&
        state.activeFgInput==FGInput::Upscaler && state.activeFgOutput==FGOutput::DLSSG &&
        state.swapchainInteropApi==SwapchainInteropApi::Dx11wDx12 &&
        Config::Instance()->GenshinControlsCurrent(genshinSender->ticket.source.configEpoch) &&
        genshinSender->MatchesFgInput(reinterpret_cast<uintptr_t>(state.currentFG),
            GenshinPresentNr::Identity(state.currentFGSwapchain),cmd,queue,color,depth,motion,historyDistrust);
    // Ordinary NR is authorized by the exact real parent + producer callback,
    // with current selected controls. This grants no FG or global interop right.
    const auto* nativeScope=GenshinPresentNr::Scope::Current();
    const bool genshinNative=!opticalFgNr && nativeScope &&
        state.swapchainInteropApi==SwapchainInteropApi::Dx11wDx12 &&
        state.api!=Vulkan && state.swapchainApi!=Vulkan &&
        !Config::Instance()->DlssNrRunBeforeSr.value_or_default() &&
        !Config::Instance()->DlssNrDeferredDlss.value_or_default() && !native::OpticalFgNrEnabled() &&
        Config::Instance()->GenshinControlsCurrent(nativeScope->ticket.source.configEpoch) &&
        GenshinPresentNr::NativeParentConsumer::Matches(cmd,queue,color,depth,motion,historyDistrust,epoch);
    // A stale FG sender is never upgraded into a bridge receipt. The private
    // ordinary parent capability may still run NR on this exact native picture.
    if (genshinSender && !genshinBridge && !genshinNative)return false;
    if (!queue || !cmd || !color || !motion || (NativeInputBlockedBySwapChainInterop() && !opticalBridge && !genshinBridge && !genshinNative))
        return false;
    if (!genshinBridge && Late::PausedForGameFrameGeneration() && !Late::ZzzGameFrameGenerationStale())
    {
        if (!Late::ComposeOnGameFg()) Late::Say(Late::pausedForGameFg);
        return false;
    }
    Late::ComPtr<ID3D12Device> currentDevice;
    if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&currentDevice))))
        return false;
    if (Late::device && Late::device != currentDevice)
    {
        Late::Say("The graphics device changed. Restart the game to use this option.");
        return false;
    }
    if (opticalBridge || genshinBridge || genshinNative)
    {
        Late::ComPtr<ID3D12Device> queueDevice;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) || queueDevice != currentDevice) return false;
    }
    Late::device = currentDevice;
    const auto desc = color->GetDesc();
    const bool screenPq = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    const bool screenScrgb = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    const bool screenSdr = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    using DlssNrColourEncoding::Screen;
    const auto screenChoice = DlssNrColourEncoding::ResolveFinished(
        Config::Instance()->DlssNrColourEncoding.value_or_default(), screenPq || screenScrgb || screenSdr,
        screenPq ? Screen::Pq : screenScrgb ? Screen::Scrgb : Screen::Sdr,
        desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
        // BGRA too: a Vulkan swapchain is usually B8G8R8A8 (DOOM Eternal's), and Optical F5Low's shared picture keeps
        // it, since swapping the channels on the game's queue needs a blit and the game may present from a compute queue
        // (a blit there is a GPU fault). The pass reads it through views, which order the channels, and its scratch
        // copies take the picture's own format.
        desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
            desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
    DlssNr::ReportColourEncoding(screenChoice.choice, DiagFormatName(desc.Format), "native input");
    if (!screenChoice.supported)
    {
        Late::Say("This screen colour format is not supported.");
        return false;
    }
    // HDR10 (PQ) needs the conversion passes the game-input path records; not done for native input yet.
    if (screenChoice.screen == Screen::Pq)
    {
        Late::Say("Native input does not support HDR10 output yet.");
        return false;
    }
    const bool scrgb = screenChoice.screen == Screen::Scrgb;
    if (!g_compose)
        g_compose = std::make_unique<DlssNr_Dx12>("Neural Rendering", Late::device.Get());

    DlssNrFrameInfo frame {};
    frame.DepthInverted = depthReversed;
    frame.MvScaleX = 1.0f;
    frame.MvScaleY = 1.0f;
    frame.ColourIsLinearHdr = scrgb;
    frame.InputEncoding = screenChoice.shaderConversion;
    frame.ColourEncoding = (uint32_t) screenChoice.choice.encoding;
    frame.ColourEncodingForced = !screenChoice.choice.automatic;
    frame.WhitePointOverride = scrgb ? 203.0f / 80.0f : 0.0f;
    frame.Reset = reset || Late::reset;
    if(auto* r=GenshinPresentNrTrace::Current()){r->lateReset=Late::reset;r->dispatchReset=frame.Reset;}
    frame.OutputWidth = (unsigned) desc.Width;
    frame.OutputHeight = desc.Height;
    frame.FinishedPicture = true;
    frame.FrameGenerationInput = opticalBridge || genshinBridge;
    // Optical F5Low measured the motion on this picture, so detail reuse may move detail with it.
    frame.MotionMatchesPicture = true;
    // NativeProducer supplies TrustMask picture-space guides to both callers:
    // unmapped Genshin producer depth is retained but excluded before flow/trust
    // sampling, and BuildGuides returns null instead of a stale depth texture.
    // Only the actual mapped source may therefore mint a non-null aligned guide.
    frame.MotionIsPictureFlow = true;
    frame.DepthIsPictureAligned = depth != nullptr;
    frame.HistoryDistrust = historyDistrust;
    frame.IndependentCommands = true;
    // R583: scope authenticates game/picture, thread, exact queue, list device and measured guides.
    // NativeDriver's callback is inside NativeProducer::Run; Run closes this list and executes it
    // on that same queue. Do not extend this capability to arbitrary native/FG callers or hints.
    if (const auto* presented = GenshinPresentNr::Scope::Current())
    {
        native::NativeFrame witness {};
        witness.color = color; witness.depth = depth; witness.motion = motion;
        witness.historyDistrust = historyDistrust;
        frame.OrderedPictureReuseQueue = !opticalBridge &&
            Config::Instance()->GenshinControlsCurrent(presented->ticket.source.configEpoch) &&
            epoch == presented->ticket.presentEpoch && presented->MatchesConsumer(cmd, queue, witness);
        if (frame.OrderedPictureReuseQueue)
        {
            const auto& source = presented->ticket.source;
            frame.OrderedPictureReuseOwner[0] = presented->ticket.swapchain;
            frame.OrderedPictureReuseOwner[1] = source.deviceEpoch;
            frame.OrderedPictureReuseOwner[2] = source.ownerEpoch;
            frame.OrderedPictureReuseOwner[3] = source.instance;
            frame.OrderedPictureReuseOwner[4] = source.historyEpoch;
            frame.OrderedPictureReuseOwner[5] = source.configEpoch;
        }
    }
    // R628: ordinary authenticated Genshin picture feedback is independent of the native
    // scene route and FG. Missing mapped depth keeps the original full-model fallback.
    frame.PictureTemporalRoute = genshinNative && frame.OrderedPictureReuseQueue;
    GenshinPresentNr::TemporalGuideFacts temporalGuides {};
    frame.PictureTemporalGuidesVerified = frame.PictureTemporalRoute &&
        GenshinPresentNr::NativeParentConsumer::TemporalGuides(cmd, queue, color, depth, motion,
            historyDistrust, epoch, temporalGuides);
    // At interval 1 only, the same real producer can supply flow/trust without depth.
    // This never upgrades the depth receipt or authorizes model-skipping reuse.
    frame.PictureFlowGuidesVerified = frame.PictureTemporalRoute && depth == nullptr &&
        Config::Instance()->DlssNrPresentedTemporalDetail.value_or_default() &&
        Config::Instance()->DlssNrDetailReuseInterval.value_or_default() == 1 &&
        GenshinPresentNr::NativeParentConsumer::PictureFlowGuides(cmd, queue, color, depth, motion,
            historyDistrust, epoch, temporalGuides);
    if (frame.PictureTemporalGuidesVerified || frame.PictureFlowGuidesVerified)
    {
        frame.PictureGuideEpoch = temporalGuides.guideEpoch;
        frame.PictureGuideCutSerial = temporalGuides.guideCutSerial;
        frame.PictureTemporalReset = temporalGuides.reset;
    }
    frame.OutputArrivalState = pictureState;
    // The first evaluate waits for the epoch to move past the frame the model was made on, and detail reuse numbers
    // frames by it (a step over 1 is a skipped frame). The caller's present count (native::PresentEpoch): each comes
    // after the frame source executed the previous frame's list.
    frame.SubmissionEpoch = epoch;

    DlssNrNative::SetPrecision(Config::Instance()->DlssNrPrecision.value_or_default());
    const auto before = g_nr.successfulDispatches;
    const auto modelCallsBefore=DetailReuse::ledger.modelCalls;
    g_compose->Dispatch(cmd, color, depth, motion, color, frame, queue);
    if(auto* r=GenshinPresentNrTrace::Current()) {r->modelSeq=GenshinPresentNrTrace::Sequence();r->modelObserved=true;
        r->modelCalls=DetailReuse::ledger.modelCalls-modelCallsBefore;}
    const bool ran = g_nr.successfulDispatches > before;
    const auto* inputObservation = native::NrInputObservation::Current(queue, cmd, color, depth, motion,
                                                                      historyDistrust, epoch);
    const auto depthObservation = inputObservation ? inputObservation->depth : native::NrInputObservation::Depth{};
    static thread_local native::NrInputObservation::Reporter inputReporter;
    if (inputReporter.Sample(depthObservation, depth != nullptr, ran))
        LOG_INFO("R849 NR actual input: pid={} epoch={} generation={} sourceFrame={} picture={} "
                 "depth={} motion={} source_depth={} guide_depth={} acquired_count={} paired={} "
                 "matched_driver={} model_composed={} model_calls={} integration_depth_contract=optional "
                 "input_scope=ApplyNativeInput raw_or_fg_preview=false submission=UNKNOWN",
                 GetCurrentProcessId(), epoch, inputObservation ? inputObservation->generation : 0,
                 inputObservation ? inputObservation->sourceFrame : 0,
                 inputObservation ? inputObservation->pictureSerial : 0, depth != nullptr, motion != nullptr,
                 native::NrInputObservation::Text(depthObservation.source),
                 native::NrInputObservation::Text(depthObservation.guide), depthObservation.acquiredCount,
                 depthObservation.paired, inputObservation != nullptr, ran,
                 DetailReuse::ledger.modelCalls - modelCallsBefore);
    Late::bridgeCoverage = {};
    Late::reset = !ran;
    Late::Say(!Config::Instance()->DlssNrApplyModel.value_or_default() ? "NR changes are hidden." :
        ran ? "Applying NR to the finished picture (native input)." : "Preparing NR for the finished picture (native input).");
    if (ran && (++Late::successes == 1 || Late::successes % 300 == 0))
        LOG_INFO("DLSS-NR native input: {} frames, {}x{}", Late::successes, desc.Width, desc.Height);
    return ran;
}

namespace
{
// All private textures return to UAV after each evaluate, including model warm-up.
Microsoft::WRL::ComPtr<ID3D12Resource> fgComposeScratch;
Microsoft::WRL::ComPtr<ID3D12Resource> fgComposeClean;
Microsoft::WRL::ComPtr<ID3D12Resource> fgComposePresent;
Microsoft::WRL::ComPtr<ID3D12Resource> fgComposeOutput;
Microsoft::WRL::ComPtr<ID3D12Resource> fgComposeGain;
Microsoft::WRL::ComPtr<ID3D12Resource> fgComposeDepth;
Microsoft::WRL::ComPtr<ID3D12Resource> fgComposeMotion;
bool fgComposeLogged = false;

void RetireFgTexture(Microsoft::WRL::ComPtr<ID3D12Resource>& texture)
{
    auto* resource = texture.Detach();
    ParkNrResource(resource);
}

bool EnsureFgTexture(Microsoft::WRL::ComPtr<ID3D12Resource>& texture, ID3D12Device* device,
                     DXGI_FORMAT format, unsigned width, unsigned height)
{
    if (texture)
    {
        const auto desc = texture->GetDesc();
        if (desc.Format != format || desc.Width != width || desc.Height != height)
            RetireFgTexture(texture);
    }
    if (!texture) texture.Attach(CreateScratch(device, format, width, height));
    return texture != nullptr;
}
} // namespace

// Use the DLSS-G keys, never the SR keys left in a reused NGX parameter block.
// Run the model once on HUDLess, then carry its edit to Backbuffer while retaining
// HUD/alpha. Write both real inputs back before FG, without replacing parameters.
bool ApplyToFgInput(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params)
{
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    if (!Late::ComposeOnGameFg() || !Late::PausedForGameFrameGeneration() || !cmdList || !params ||
        NativeInputBlockedBySwapChainInterop())
        return false;

    unsigned subframe = 1;
    params->Get("DLSSG.MultiFrameIndex", &subframe);
    if (subframe > 1) return false; // already edited the real inputs on subframe 1

    auto* present = GetResource(params, "DLSSG.Backbuffer", "DLSSG.Backbuffer");
    auto* hudless = GetResource(params, "DLSSG.HUDLess", "DLSSG.HUDLess");
    auto* depth = GetResource(params, "DLSSG.Depth", "DLSSG.Depth");
    auto* motion = GetResource(params, "DLSSG.MVecs", "DLSSG.MVecs");
    auto* color = hudless ? hudless : present;
    if (color == nullptr || present == nullptr || motion == nullptr)
    {
        Late::Say("Waiting for DLSS-G finished-picture inputs.");
        return false;
    }

    Late::ComPtr<ID3D12Device> currentDevice;
    if (FAILED(cmdList->GetDevice(IID_PPV_ARGS(&currentDevice)))) return false;
    if (Late::device && Late::device != currentDevice)
    {
        Late::Say("The graphics device changed. Restart the game to use this option.");
        return false;
    }
    Late::device = currentDevice;
    const auto desc = color->GetDesc();
    const auto presentDesc = present->GetDesc();
    const auto format = TypedGuideFormat(desc.Format);
    const auto presentFormat = TypedGuideFormat(presentDesc.Format);
    const auto valid = [](const D3D12_RESOURCE_DESC& d) {
        return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.SampleDesc.Count == 1 &&
               d.DepthOrArraySize == 1 && d.MipLevels == 1;
    };
    if (!valid(desc) || !valid(presentDesc) || desc.Width != presentDesc.Width || desc.Height != presentDesc.Height)
    {
        Late::Say("DLSS-G finished-picture sizes do not match.");
        return false;
    }

    if (!fgComposeLogged)
    {
        fgComposeLogged = true;
        LOG_INFO("DLSS-NR FG v3: {} {}x{} {} -> {}; Backbuffer {}; depth {}; motion {}x{}",
                 hudless ? "DLSSG.HUDLess" : "DLSSG.Backbuffer", desc.Width, desc.Height,
                 DiagFormatName(desc.Format), DiagFormatName(format), DiagFormatName(presentDesc.Format),
                 depth != nullptr, motion->GetDesc().Width, motion->GetDesc().Height);
    }

    using DlssNrColourEncoding::Screen;
    const bool fp16 = format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    const bool unorm = format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R10G10B10A2_UNORM;
    unsigned hdr = 0;
    params->Get("DLSSG.ColorBuffersHDR", &hdr);
    const auto screenChoice = DlssNrColourEncoding::ResolveFinished(
        Config::Instance()->DlssNrColourEncoding.value_or_default(), true,
        fp16 ? Screen::Scrgb : hdr ? Screen::Pq : Screen::Sdr,
        fp16, unorm);
    const bool presentSupported = presentFormat == DXGI_FORMAT_R8G8B8A8_UNORM ||
                                 presentFormat == DXGI_FORMAT_R10G10B10A2_UNORM ||
                                 presentFormat == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!screenChoice.supported || !presentSupported || presentFormat != format || screenChoice.screen == Screen::Pq)
    {
        Late::Say("The frame-generation input colour encoding is not supported (SDR/scRGB required).");
        return false;
    }
    ReportColourEncoding(screenChoice.choice, DiagFormatName(format), "DLSS-G finished picture");
    const bool scrgb = screenChoice.screen == Screen::Scrgb;
    const bool separateHud = color != present;
    const unsigned width = (unsigned) desc.Width, height = desc.Height;
    auto ensure = [&](Late::ComPtr<ID3D12Resource>& texture, DXGI_FORMAT f) {
        return EnsureFgTexture(texture, currentDevice.Get(), f, width, height);
    };
    if (!ensure(fgComposeScratch, format) ||
        (separateHud && (!ensure(fgComposeClean, format) || !ensure(fgComposePresent, presentFormat) ||
                         !ensure(fgComposeOutput, presentFormat) ||
                         !ensure(fgComposeGain, DXGI_FORMAT_R16G16B16A16_FLOAT))))
    {
        Late::Say("Could not allocate the DLSS-G finished-picture textures.");
        return false;
    }
    const auto clone = [&](Late::ComPtr<ID3D12Resource>& target, ID3D12Resource* source) {
        if (target)
        {
            const auto have = target->GetDesc(), want = source->GetDesc();
            if (have.Width != want.Width || have.Height != want.Height || have.Format != TypedGuideFormat(want.Format))
                RetireFgTexture(target);
        }
        return Late::Clone(target, source);
    };
    if (!clone(fgComposeMotion, motion) || (depth && !clone(fgComposeDepth, depth)))
    {
        Late::Say("The game's depth or movement data is not supported.");
        return false;
    }

    // The existing native FG adapter uses COPY_DEST for HUDLess and shader-read
    // for Backbuffer/guides at this seam. Restore those states before calling FG.
    const auto colorArrival = hudless ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto capture = [&](ID3D12Resource* source, ID3D12Resource* target, D3D12_RESOURCE_STATES arrival) {
        Barrier(cmdList, source, arrival, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->CopyResource(target, source);
        Barrier(cmdList, target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, arrival);
    };
    capture(color, fgComposeScratch.Get(), colorArrival);
    if (separateHud)
    {
        capture(color, fgComposeClean.Get(), colorArrival);
        capture(present, fgComposePresent.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    for (auto pair : { std::pair { depth, fgComposeDepth.Get() }, std::pair { motion, fgComposeMotion.Get() } })
    {
        if (pair.first == nullptr)
            continue;
        Barrier(cmdList, pair.first, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmdList->CopyResource(pair.second, pair.first);
        Barrier(cmdList, pair.first, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    DlssNrFrameInfo frame {};
    unsigned inverted = 0, gameReset = 0;
    params->Get("DLSSG.DepthInverted", &inverted);
    params->Get("DLSSG.Reset", &gameReset);
    frame.Reset = gameReset != 0 || Late::reset;
    frame.DepthInverted = inverted != 0;
    frame.MotionVectorsLowResolution = motion->GetDesc().Width < width || motion->GetDesc().Height < height;
    frame.FinishedPicture = true;
    frame.FrameGenerationInput = true;
    // Explicit experimental opt-in: scene-only (HUDLess) input, never the UI backbuffer.
    // This permits game-vector detail transport; it does not certify postprocess motion.
    frame.MotionMatchesPicture = hudless != nullptr && Config::Instance()->DlssNrGameMotionReuse.value_or_default();
    frame.IndependentCommands = false; // this is the game's command list
    frame.ColourIsLinearHdr = scrgb;
    frame.InputEncoding = screenChoice.shaderConversion;
    frame.ColourEncoding = (uint32_t) screenChoice.choice.encoding;
    frame.ColourEncodingForced = !screenChoice.choice.automatic;
    frame.WhitePointOverride = scrgb ? 203.0f / 80.0f : 0.0f;
    frame.OutputWidth = width;
    frame.OutputHeight = height;
    frame.RenderSubrectWidth = (unsigned) motion->GetDesc().Width;
    frame.RenderSubrectHeight = motion->GetDesc().Height;
    params->Get("DLSSG.MvecScaleX", &frame.MvScaleX);
    params->Get("DLSSG.MvecScaleY", &frame.MvScaleY);
    params->Get("DLSSG.MVecsSubrectWidth", &frame.RenderSubrectWidth);
    params->Get("DLSSG.MVecsSubrectHeight", &frame.RenderSubrectHeight);
    params->Get("DLSSG.DepthSubrectBaseX", &frame.DepthSubrectBaseX);
    params->Get("DLSSG.DepthSubrectBaseY", &frame.DepthSubrectBaseY);
    params->Get("DLSSG.MVecsSubrectBaseX", &frame.MotionSubrectBaseX);
    params->Get("DLSSG.MVecsSubrectBaseY", &frame.MotionSubrectBaseY);
    frame.OutputArrivalState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    frame.SubmissionEpoch = State::Instance().frameCount;

    if (depth)
        Barrier(cmdList, fgComposeDepth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmdList, fgComposeMotion.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    DlssNrNative::SetPrecision(Config::Instance()->DlssNrPrecision.value_or_default());

    if (!g_compose)
        g_compose = std::make_unique<DlssNr_Dx12>("Neural Rendering", Late::device.Get());
    const auto before = g_nr.successfulDispatches;
    {
        DlssNrCurrentOutput::DeferEndpoint captureAtFinalEndpoint;
        g_compose->Dispatch(cmdList, fgComposeScratch.Get(), depth ? fgComposeDepth.Get() : nullptr,
                            fgComposeMotion.Get(), fgComposeScratch.Get(), frame, nullptr);
    }

    Barrier(cmdList, fgComposeMotion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    if (depth)
        Barrier(cmdList, fgComposeDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

    bool ran = g_nr.successfulDispatches > before;
    const bool applyModel = Config::Instance()->DlssNrApplyModel.value_or_default();
    if (ran && applyModel && separateHud)
    {
        ScopedNrStateEnvelope stateEnvelope(cmdList);
        for (auto* texture : { fgComposeClean.Get(), fgComposeScratch.Get(), fgComposePresent.Get() })
            Barrier(cmdList, texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        DlssNrConstants transfer {};
        transfer.Mode = 5; // existing finished-colour shader: bounded log-gain
        transfer.Width = width; transfer.Height = height;
        transfer.WhitePoint = 1.0f;
        transfer.TransferStrength = scrgb ? 1.0f : 0.0f;
        transfer.MaxRatio = std::clamp(Config::Instance()->DlssNrMaxRatio.value_or_default(), 1.0f, 8.0f);
        ran = g_compose->DispatchResidualPass(cmdList, transfer, fgComposeClean.Get(), fgComposeScratch.Get(),
                                              nullptr, nullptr, fgComposeGain.Get());
        Barrier(cmdList, fgComposeGain.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transfer.Mode = scrgb ? 3 : 2;
        if (ran)
            ran = g_compose->DispatchResidualPass(cmdList, transfer, fgComposePresent.Get(), nullptr,
                                                  fgComposeGain.Get(), nullptr, fgComposeOutput.Get());
        Barrier(cmdList, fgComposeGain.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (auto* texture : { fgComposeClean.Get(), fgComposeScratch.Get(), fgComposePresent.Get() })
            Barrier(cmdList, texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    const auto writeBack = [&](ID3D12Resource* target, ID3D12Resource* edited, D3D12_RESOURCE_STATES arrival) {
        Barrier(cmdList, edited, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmdList, target, arrival, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->CopyResource(target, edited);
        Barrier(cmdList, target, D3D12_RESOURCE_STATE_COPY_DEST, arrival);
        Barrier(cmdList, edited, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    };
    if (ran && applyModel)
    {
        writeBack(color, fgComposeScratch.Get(), colorArrival);
        if (separateHud)
            writeBack(present, fgComposeOutput.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    Late::bridgeCoverage = {};
    Late::reset = !ran;
    Late::Say(!Config::Instance()->DlssNrApplyModel.value_or_default()
                  ? "NR changes are hidden."
                  : ran ? "Applying NR to DLSS-G finished-picture inputs (6X compatible)."
                        : "Preparing NR for the frame-generation input.");
    if (ran && (++Late::successes == 1 || Late::successes % 300 == 0))
        LOG_INFO("DLSS-NR finished picture (FG v3): {} frames, {}x{}, HUD preserved {}", Late::successes, width, height, separateHud);
    return ran;
}
