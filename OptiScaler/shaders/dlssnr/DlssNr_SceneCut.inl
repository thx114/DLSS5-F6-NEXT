// Scene cuts the game does not flag, on DLSS-NR's game input, D3D12 (menu: NR Options, "Reset NR on scene cuts the game
// does not flag"; ini [DlssNr] SceneCut).
//
// Everything that forgets the last scene -- the model's own history, Reuse detail between frames -- waits for the game's
// reset flag. A game that cuts without it shows the old scene fading out over several frames. The scene-cut detector
// (motion/SceneCut_Dx12.h, the one Optical F5Low's flow runs) looks at every evaluate's encoded frame (g_nr.colorCopy,
// display-referred whatever the game's encoding). Its flag is copied to a readback buffer and read a few evaluates
// later, when the GPU is certainly past it: each slot keeps its own fence, signalled from FinishedPictureSubmitted (the
// real ExecuteCommandLists), as Reuse detail's coverage readback does. What a found cut means is
// dlssnr/DlssNrSceneCut.h:
//   - SceneCut=1, the default: counted only (cuts found, how many the game flagged too), in the log and the menu.
//   - SceneCut=2: on the frame itself, the detector's 1x1 distrust is Reuse detail's history distrust (a reused cut
//     frame drops all the moved detail); and when the answer arrives, the model resets unless the game did.
//   - SceneCut=0: nothing runs.
// Not on Optical F5Low's native input (DlssNrFrameInfo::MotionMatchesPicture): its own flow runs the same detector and
// handles Off/Count/Act through NativeProducer; its trust mask is already reuse's history distrust.
//
// Included inside DlssNr_Dx12.cpp's anonymous namespace, after DlssNr_DetailReuse.inl. Dispatch calls BeginFrame where it
// takes the game's reset flag and Run once the encode has written the colour copy, both under g_nrMutex.
namespace SceneCut
{
constexpr unsigned int kSlots = 8;
constexpr unsigned int kRowBytes = 256; // the 2x1 R32_UINT flag, one row of a texture-to-buffer copy

struct Slot
{
    ID3D12Resource* buffer = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    ID3D12CommandList* commands = nullptr; // identity only
    UINT64 value = 0;
    unsigned long long evaluate = 0; // the evaluate whose frame was judged
    unsigned long long modeEpoch = 0; // mode generation in which the copy was recorded
    bool pending = false;            // a copy was recorded into it
    bool submitted = false;          // and the list it was recorded on was executed
    bool quarantined = false;        // Execute happened but Signal failed: never reusable
};

SceneCutDx12* detector = nullptr; // made on the first evaluate that wants it
ID3D12Device* detectorDevice = nullptr;
bool failed = false; // made and failed on detectorDevice: not tried again until the device changes
bool dispatchFailed = false;
Slot slots[kSlots];
UINT64 fenceValue = 0;
unsigned long long evaluates = 0; // NR evaluates seen here, the clock the policy counts in
unsigned long long modeEpoch = 1; // increments whenever Off/Count/Act changes
bool resetPending = false;        // the next evaluate requests a reset
DlssNrSceneCut::SubmissionReceipts modelReceipts; // pending until a complete model attempt is submitted
bool resetForFrame = false;       // stable across all model passes of the recording
bool nativeFrame = false;
bool detectorUsed = false;        // any recording requires retention on replacement/release
unsigned long long acceptedResets = 0, stale = 0, readbackFailures = 0;
DlssNrSceneCut::Policy policy;
DlssNrSceneCut::Mode lastMode = DlssNrSceneCut::Mode::Count;
bool ranLast = false; // the detector ran on the last evaluate (else its histograms are not the last frame's)
unsigned long long lastLateBy = 0; // how many evaluates after the cut its answer arrived, the last time

// What the menu reads: a copy made under its own lock as each evaluate's work here ends, so the menu never waits for
// g_nrMutex while NR records.
std::mutex publishedLock;
DlssNr::SceneCutInfo published, nativePublished;
bool selectedNative = false;

void SelectNative(bool native)
{
    std::lock_guard<std::mutex> lock(publishedLock);
    selectedNative = native;
}

void PublishNative(const DlssNr::SceneCutInfo& info)
{
    std::lock_guard<std::mutex> lock(publishedLock);
    nativePublished = info;
    // Idle polling of an unused native driver must not steal the shared route.
    if (info.running || info.failed || info.virtualConsumer) selectedNative = true;
}

bool ResetRequested()
{
    return nativeFrame ? DlssNr::NativeSceneCutResetRequested()
                       : resetForFrame && lastMode == DlssNrSceneCut::Mode::Act;
}

DlssNrSceneCut::Mode NormalizedMode(const Config& cfg)
{
    const bool unsupportedOrigin = State::Instance().api == Vulkan || State::Instance().swapchainApi == Vulkan ||
        State::Instance().swapchainInteropApi == SwapchainInteropApi::VkwDx12;
    return DlssNrSceneCut::NormalizeMode(cfg.DlssNrSceneCut.value_or_default(), unsupportedOrigin);
}

DlssNrSceneCut::SubmissionReceipts::Token BeginModelAttempt(unsigned passes, bool proxy)
{
    if (nativeFrame || !resetForFrame || lastMode != DlssNrSceneCut::Mode::Act) return {};
    return modelReceipts.Begin(evaluates, passes, proxy);
}

void ModelRecorded(DlssNrSceneCut::SubmissionReceipts::Token token, ID3D12GraphicsCommandList* cmdList,
                   unsigned pass, uintptr_t model, bool reset, bool success)
{
    if (token.id == 0) return; // ordinary/native frames need no receipt work
    ID3D12CommandList* commands = DetailReuse::RealSubmittedCommands(cmdList);
    if (modelReceipts.Record(token, pass, reinterpret_cast<uintptr_t>(commands), model, reset, success))
        DlssNrWatchedLists::Add(commands); // required even with no detector readback/timer/late work
}

void EndModelAttempt(DlssNrSceneCut::SubmissionReceipts::Token token) { modelReceipts.End(token); }

void Publish()
{
    DlssNr::SceneCutInfo info;
    info.found = policy.found;
    info.flagged = policy.flagged;
    info.silent = policy.silent;
    info.requested = policy.resets;
    info.resets = acceptedResets;
    info.stale = stale;
    info.readbackFailures = readbackFailures;
    info.mode = static_cast<unsigned>(lastMode);
    for (const Slot& slot : slots) info.quarantined = info.quarantined || slot.quarantined;
    info.lateBy = lastLateBy;
    info.running = ranLast;
    info.failed = failed || dispatchFailed || info.quarantined;
    if (failed) info.reason = "shared scene-cut detector initialization";
    else if (dispatchFailed) info.reason = "shared scene-cut detector dispatch";
    else if (info.quarantined) info.reason = "scene-cut readback quarantined after failed Signal";
    std::lock_guard<std::mutex> lock(publishedLock);
    published = info;
}

// A device change/release is not a completion witness. There can also be detector
// recordings with no free readback slot, so slot fences alone cannot retire it.
// Conservatively retain every used detector and its slots for process lifetime.
struct QuarantinedOwners
{
    SceneCutDx12* detector = nullptr;
    Slot slots[kSlots];
    QuarantinedOwners* next = nullptr;
};
QuarantinedOwners* retained = nullptr;

void RetireDetector()
{
    if (detectorUsed)
    {
        auto* owners = new QuarantinedOwners;
        owners->detector = detector;
        for (unsigned i = 0; i < kSlots; ++i)
        {
            owners->slots[i] = std::move(slots[i]);
            slots[i] = Slot {};
        }
        owners->next = retained;
        retained = owners;
    }
    else
    {
        delete detector;
        for (Slot& slot : slots)
        {
            if (slot.buffer) slot.buffer->Release();
            slot = Slot {};
        }
    }
    detector = nullptr;
    detectorUsed = false;
}

void ReleaseSlots()
{
    for (Slot& slot : slots)
    {
        if (slot.buffer != nullptr)
            slot.buffer->Release();
        slot = Slot {};
    }
}

// Built with the detector; without them nothing is read back and only the same-frame distrust works.
bool MakeSlots(ID3D12Device* device)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kRowBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (Slot& slot : slots)
    {
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&slot.buffer))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.fence))))
            return false;
    }

    return true;
}

// The detector on this device, made the first time and again after a device change. Null when it cannot be made.
SceneCutDx12* Detector(ID3D12Device* device)
{
    if (detector != nullptr && detectorDevice == device)
        return detector;

    if (failed && detectorDevice == device)
        return nullptr;

    // Device identity alone says nothing about old GPU completion.
    RetireDetector();
    ++modeEpoch;
    resetPending = resetForFrame = false;
    modelReceipts.Cancel();
    policy.Forget();
    detectorDevice = device;
    failed = false;
    ranLast = false;

    auto* made = new SceneCutDx12();

    if (!made->Init(device) || !MakeSlots(device))
    {
        LOG_WARN("DLSS-NR scene cuts: the detector could not be made ({}); scene cuts are left to the game",
                 made->Error().empty() ? "readback" : made->Error());
        delete made;
        ReleaseSlots();
        failed = true;
        return nullptr;
    }

    detector = made;
    return detector;
}

// Every answer the GPU has finished, before this evaluate records more.
void Collect(DlssNrSceneCut::Mode mode)
{
    for (Slot& slot : slots)
    {
        if (!slot.pending || !slot.submitted || slot.quarantined || slot.buffer == nullptr || slot.fence == nullptr)
            continue;
        const UINT64 completed = slot.fence->GetCompletedValue();
        if (completed == UINT64_MAX || completed < slot.value)
            continue;

        uint32_t words[2] = {};
        void* mapped = nullptr;
        D3D12_RANGE range { 0, sizeof(words) };
        if (SUCCEEDED(slot.buffer->Map(0, &range, &mapped)) && mapped != nullptr)
        {
            std::memcpy(words, mapped, sizeof(words));
            D3D12_RANGE none { 0, 0 };
            slot.buffer->Unmap(0, &none);
        }
        else ++readbackFailures;
        slot.pending = slot.submitted = false;
        slot.commands = nullptr;

        if (words[0] == 0)
            continue;

        float divergence = 0.0f;
        std::memcpy(&divergence, &words[1], sizeof(divergence));
        // A readback can finish after the setting changed. It remains useful for Count diagnostics, but it must not
        // become an Act reset after the user re-enables or changes the detector: that result belongs to the old mode
        // generation and its scene history was discarded in BeginFrame.
        const bool currentGeneration = slot.modeEpoch == modeEpoch;
        if (!currentGeneration) ++stale;
        if (mode == DlssNrSceneCut::Mode::Off) continue;
        const DlssNrSceneCut::Outcome outcome =
            policy.Found(slot.evaluate, evaluates, currentGeneration ? mode : DlssNrSceneCut::Mode::Count);
        lastLateBy = evaluates - slot.evaluate;
        if (currentGeneration)
            resetPending = resetPending || outcome.reset;

        // Every cut for the first twenty, then every fiftieth: enough to see whether a game flags its own.
        if (policy.found <= 20 || policy.found % 50 == 0)
            LOG_INFO("DLSS-NR scene cut found (divergence {:.2f}, read {} evaluates later): {}; {} found so far, {} "
                     "flagged by the game",
                     divergence, lastLateBy,
                     outcome.gameFlagged ? "the game flagged it too"
                     : outcome.reset     ? "the game did not flag it, reset requested (acceptance pending)"
                     : currentGeneration && mode == DlssNrSceneCut::Mode::Act
                         ? "the game did not flag it, another reset request is in the quiet window"
                         : "the game did not flag it (counted only: SceneCut=1)",
                     policy.found, policy.flagged);
    }
}

// Where Dispatch takes the game's reset flag: collects finished answers, notes the game's reset, and says whether this
// evaluate must reset the model for a cut found earlier.
bool BeginFrame(DlssNrSceneCut::Mode mode, bool gameReset, bool isNative)
{
    ++evaluates;
    resetForFrame = false;
    SelectNative(isNative);

    if (mode != lastMode || nativeFrame != isNative)
    {
        // A change of mode starts the bookkeeping over; a reset asked for under Act does not outlive it.
        lastMode = mode;
        ++modeEpoch;
        resetPending = false;
        modelReceipts.Cancel();
        policy.Forget();
        ranLast = false;
    }
    nativeFrame = isNative;
    if (isNative) return false; // no shared pending observation may reset a native consumer

    if (gameReset)
    {
        policy.GameReset(evaluates);
        modelReceipts.Cancel(); // explicit game reset supersedes our unaccepted request
    }
    Collect(mode); // physical slots retire even while Off; observations cannot Act
    if (mode == DlssNrSceneCut::Mode::Off)
    {
        ranLast = false;
        Publish();
        return false;
    }

    const bool reset = resetPending && mode == DlssNrSceneCut::Mode::Act;
    resetPending = false;
    if (reset) modelReceipts.Request();
    resetForFrame = modelReceipts.Pending();
    Publish();
    return reset;
}

// The detector on this evaluate's frame (`color`, readable as `viewFormat`, NON_PIXEL_SHADER_RESOURCE). Returns the 1x1
// distrust for Reuse detail when the Act mode wants it there, else null.
ID3D12Resource* Run(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, DlssNrSceneCut::Mode mode,
                    const DlssNrFrameInfo& frame, ID3D12Resource* color, DXGI_FORMAT viewFormat)
{
    // Optical F5Low's native input has its own detector; and a skipped evaluate breaks the frame-to-frame comparison.
    if (mode == DlssNrSceneCut::Mode::Off || lastMode == DlssNrSceneCut::Mode::Off ||
        frame.MotionMatchesPicture || color == nullptr)
    {
        ranLast = false;
        dispatchFailed = false;
        Publish();
        return nullptr;
    }

    SceneCutDx12* sc = Detector(device);

    if (sc == nullptr)
    {
        Publish();
        return nullptr;
    }

    if (!ranLast)
        sc->Reset();

    // Even a failed recording may reference these owners; retain without proof.
    detectorUsed = true;
    ranLast = sc->DetectColor(cmdList, color, viewFormat, SceneCutDx12::Encoding::Srgb, 203.0f,
                              DlssNrSceneCut::kThreshold);
    dispatchFailed = !ranLast;

    Publish();

    if (!ranLast)
        return nullptr;

    // The flag into a free readback slot. Every slot in flight: this frame is not counted.
    for (Slot& slot : slots)
    {
        if (slot.pending || slot.buffer == nullptr || slot.fence == nullptr)
            continue;

        D3D12_TEXTURE_COPY_LOCATION src {};
        src.pResource = sc->Flag();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

        D3D12_TEXTURE_COPY_LOCATION dst {};
        dst.pResource = slot.buffer;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, 2, 1, 1, kRowBytes };

        Barrier(cmdList, sc->Flag(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(cmdList, sc->Flag(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        // The list the hook will report is the real one, not a Streamline proxy (as DetailReuse::CopyCoverage).
        slot.commands = cmdList;
        ID3D12GraphicsCommandList* real = nullptr;
        if (Util::CheckForRealObject(__FUNCTION__, cmdList, (IUnknown**) &real))
            slot.commands = real;
        DlssNrWatchedLists::Add(slot.commands); // the submit/reset hooks must hear about this list
        slot.evaluate = evaluates;
        slot.modeEpoch = modeEpoch;
        slot.pending = true;
        slot.submitted = false;
        break;
    }

    return mode == DlssNrSceneCut::Mode::Act ? sc->Distrust() : nullptr;
}

// A command list a copy was recorded on was reset without being executed: the slot is free again.
void ResetRecording(ID3D12CommandList* cmdList)
{
    modelReceipts.ResetRecording(reinterpret_cast<uintptr_t>(DetailReuse::RealSubmittedCommands(cmdList)));
    for (Slot& slot : slots)
        if (slot.pending && !slot.submitted && !slot.quarantined && slot.commands == cmdList)
        {
            slot.pending = false;
            slot.commands = nullptr;
        }
}

// The command list a copy was recorded on has been executed: its slot's fence may be signalled.
void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    if (queue == nullptr || lists == nullptr)
        return;
    // FinishedPictureSubmitted already canonicalized these lists and observed ExecuteCommandLists.
    // This acknowledges submission only, independently of readback Signal/completion.
    for (UINT i = 0; i < count; ++i)
        if (modelReceipts.Submitted(reinterpret_cast<uintptr_t>(lists[i])))
        {
            ++acceptedResets;
            Publish();
        }
    for (Slot& slot : slots)
    {
        if (!slot.pending || slot.submitted || slot.fence == nullptr)
            continue;
        for (UINT i = 0; i < count; ++i)
            if (lists[i] == slot.commands)
            {
                slot.value = ++fenceValue;
                slot.submitted = true;
                // Execute already happened. Failed Signal gives no completion proof.
                if (FAILED(queue->Signal(slot.fence.Get(), slot.value)))
                {
                    slot.quarantined = true;
                    ++readbackFailures;
                }
                break;
            }
    }
}

void Release()
{
    RetireDetector();
    detectorDevice = nullptr;
    failed = dispatchFailed = false;
    ranLast = false;
    resetPending = resetForFrame = false;
    modelReceipts.Cancel();
    ++modeEpoch;
    policy.Forget();
}

DlssNr::SceneCutInfo Status()
{
    std::lock_guard<std::mutex> lock(publishedLock);
    return selectedNative ? nativePublished : published;
}
} // namespace SceneCut
