// SPDX-License-Identifier: GPL-3.0-or-later
// Included in DlssNr, after the shared private-recording lifecycle. The legacy
// placement/optical/cadence dispatcher is deliberately not an entry to this route.

uint64_t BeginGenshinPresentSource()
{
    if (!UsesGenshinPresentPipeline()) return 0;
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    GenshinPresentation::currentMetadata.reset();
    const auto attempt = GenshinPresentation::sourceClock.Begin();
    // New input revokes stale CPU admission. Resources remain pinned by their
    // original capture/private queue signals and recording Reset boundaries.
    for (auto& slot : Late::slots)
        if (slot.genshinAttempt && slot.pending && slot.submitted) slot.pending = false;
    if (!g_nrRetired.empty()) Late::RetireCompletedPrivateRecordings();
    return attempt;
}

void CompleteGenshinPresentSource(uint64_t attempt, GuideSourceMetadata::Ptr metadata,
                                 const void* dx11Output, bool succeeded)
{
    if (!attempt) return;
    std::lock_guard<std::recursive_mutex> lock(g_nrMutex);
    NrPresentation::Source source;
    if (metadata)
        source = {metadata->evaluateFrame, metadata->feature, metadata->context, metadata->generation,
                  reinterpret_cast<uintptr_t>(dx11Output)};
    if (!GenshinPresentation::sourceClock.Complete(attempt, source, succeeded)) return;
    // This is reached only after the actual provider Execute + successful
    // CopyBackOutput. CPU provider success alone does not publish a Present input.
    GenshinPresentation::currentMetadata = std::move(metadata);
}

namespace GenshinPresentation
{
FinishedPictureOutcome Apply(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue,
                             ID3D12CommandQueue* orderedProducerQueue,
                             const GuideSourceMetadata::Ptr& selectedSource)
{
    FinishedPictureOutcome outcome;
    outcome.bridgeEndpoint = true;
    outcome.presentEpoch = State::Instance().frameCount;
    outcome.endpointSerial = ++Late::finishedPictureEndpointSerial;
    outcome.thread = GetCurrentThreadId();
    outcome.reason = FinishedPictureReason::InvalidEndpoint;
    outcome.swapchain = outcome.owner = DlssNrFgTrace::Identity(swapchain);
    outcome.queue = DlssNrFgTrace::Identity(queue);
    outcome.producerQueue = DlssNrFgTrace::Identity(orderedProducerQueue);
    DlssNrCurrentOutput::Capture::PresentScope admission;
    if (!admission || !swapchain || !queue || !orderedProducerQueue ||
        State::Instance().swapchainInteropApi != SwapchainInteropApi::Dx11wDx12 ||
        HoYoShade::ColorConsumers(swapchain) != HoYoShade::ConsumerAccess::Safe)
        return outcome;
    Late::ComPtr<IDXGISwapChain3> sc;
    Late::ComPtr<ID3D12Resource> color;
    Late::ComPtr<ID3D12Device> device;
    if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&sc))) ||
        FAILED(sc->GetBuffer(sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&color))) ||
        FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) return outcome;
    outcome.color = DlssNrFgTrace::Identity(color.Get());
    outcome.device = DlssNrFgTrace::Identity(device.Get());
    const auto attempt = sourceClock.Attempt();
    const auto source = sourceClock.CurrentSource();
    if (!sourceClock.Available(attempt, source) || !currentMetadata ||
        (selectedSource && selectedSource != currentMetadata))
    {
        outcome.reason = FinishedPictureReason::NoCurrentInput;
        Late::Say("Waiting for a new successful DLSS source picture.");
        return outcome;
    }
    outcome.source = currentMetadata;
    const auto& cfg = *Config::Instance();
    if (!cfg.DlssNrEnabled.value_or_default())
    {
        sourceClock.Claim(attempt, source);
        lastWrittenSource = {};
        outcome.reason = FinishedPictureReason::Disabled;
        Late::Say("NR is disabled; presenting the current source picture.");
        return outcome;
    }
    const auto desc = color->GetDesc();
    auto* producer = Late::RealQueue(orderedProducerQueue);
    auto* present = Late::RealQueue(queue);
    Late::Slot* selected = nullptr;
    for (auto& slot : Late::slots)
        if (slot.genshinAttempt == attempt && slot.pending && slot.submitted && !slot.quarantined &&
            slot.dx11Bridge && !slot.residualOnly && slot.bridgeOutputHandoff &&
            slot.producerQueue.Get() == producer && slot.frame.RawSourceMetadata == currentMetadata &&
            slot.frame.OutputWidth == desc.Width && slot.frame.OutputHeight == desc.Height &&
            slot.fence && slot.fence->GetCompletedValue() != UINT64_MAX)
            selected = &slot;
    if (!selected)
    {
        outcome.reason = FinishedPictureReason::NoCurrentInput;
        Late::Say("NR bypass: the current source has no matching guide capture.");
        return outcome;
    }
    auto& slot = *selected;
    // The wrapper already ordered producer -> DX11 -> shared copy -> PRESENT
    // queue before entering this endpoint. It passes the exact producer queue.
    // This is ordering authority only; retirement still uses each real fence.
    if (!DlssNrGpuLifetime::SameDevice(color.Get(), present) ||
        !DlssNrGpuLifetime::SameDevice(slot.motion.Get(), present) ||
        present->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        !sourceClock.Claim(attempt, source)) return outcome;
    slot.pending = false; // claim precedes every recording, including warmup/failure
    outcome.fence = slot.fence;
    outcome.fenceValue = slot.ready;
    outcome.submissionSerial = slot.serial;
    if (FAILED(slot.allocator->Reset()) || FAILED(slot.commands->Reset(slot.allocator.Get(), nullptr)))
    {
        slot.quarantined = true;
        outcome.result = FinishedPictureResult::FailedUncertain;
        outcome.reason = FinishedPictureReason::RecordingFailed;
        lastWrittenSource = {};
        return outcome;
    }
    auto* cmd = slot.commands.Get();
    FinishedPictureResetCommandList(DetailReuse::RealTemporalCommands(cmd));
    slot.backBufferIdentity = reinterpret_cast<uintptr_t>(color.Get());
    if (DetailHistory::PendingRecordings() || !g_nrRetired.empty())
        Late::RetireCompletedPrivateRecordings(&slot);

    const auto failRecording = [&] {
        // No partial private command list is ever executed. Keep uncertain
        // executable owners until the actual Reset/retirement path closes them.
        const bool closed = SUCCEEDED(cmd->Close());
        slot.quarantined = !closed;
        lastWrittenSource = {};
        outcome.reason = FinishedPictureReason::RecordingFailed;
        return outcome;
    };
    auto colorSpace = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ?
        DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT colorSpaceBytes = sizeof(colorSpace);
    swapchain->GetPrivateData(Late::colorSpaceKey, &colorSpaceBytes, &colorSpace);
    uint32_t encoding = cfg.DlssNrColourEncoding.value_or_default();
    static uint32_t queuedEncoding = encoding;
    static uint64_t appliedEncodingGeneration = 0;
    uint32_t nextEncoding = 0; uint64_t nextGeneration = 0;
    if (DlssNrSettingsQueue::ConsumeColourEncoding(nextEncoding, nextGeneration) &&
        nextGeneration > appliedEncodingGeneration)
    { queuedEncoding = nextEncoding; appliedEncodingGeneration = nextGeneration; }
    if (appliedEncodingGeneration) encoding = queuedEncoding;
    using DlssNrColourEncoding::Screen;
    const bool screenPq = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    const bool screenScrgb = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    const bool screenSdr = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    const auto choice = DlssNrColourEncoding::ResolveFinished(encoding, screenPq || screenScrgb || screenSdr,
        screenPq ? Screen::Pq : screenScrgb ? Screen::Scrgb : Screen::Sdr,
        desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
        desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
    if (!choice.supported)
    {
        cmd->Close();
        outcome.reason = FinishedPictureReason::UnsupportedColour;
        return outcome;
    }
    const bool pq = choice.screen == Screen::Pq, scrgb = choice.screen == Screen::Scrgb;
    const auto ensure = [&](Late::ComPtr<ID3D12Resource>& texture, DXGI_FORMAT format) {
        if (texture && (texture->GetDesc().Width != desc.Width || texture->GetDesc().Height != desc.Height ||
                        texture->GetDesc().Format != format)) texture.Reset(); // actual private Reset just succeeded
        if (!texture) texture.Attach(CreateScratch(device.Get(), format, unsigned(desc.Width), desc.Height));
        return texture != nullptr;
    };
    // Raw present stays untouched until the ONE final copy. SDR/scRGB is read
    // directly by encode; PQ is decoded into private work first. Neither route
    // ever starts from the previous source's NR result.
    if (!ensure(slot.presentWork, pq ? DXGI_FORMAT_R16G16B16A16_FLOAT : desc.Format) ||
        (pq && !ensure(slot.encoded, desc.Format))) return failRecording();
    const auto use = NrRecording(device.Get(), cmd);
    if (!DlssNrGpuLifetime::Retain(use, {color.Get(), slot.presentWork.Get(), slot.encoded.Get(),
                                        slot.depth.Get(), slot.motion.Get()})) return failRecording();
    if (!g_compose) g_compose = std::make_unique<DlssNr_Dx12>("Neural Rendering", device.Get());
    DlssNrConstants conversion {};
    conversion.Width = unsigned(desc.Width); conversion.Height = desc.Height;
    if (pq)
    {
        Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const bool converted = g_compose->DispatchResidualPass(cmd, conversion, color.Get(), nullptr, nullptr,
                                                              nullptr, slot.presentWork.Get());
        Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT);
        if (!converted) return failRecording();
    }
    auto frame = slot.frame;
    frame.FinishedPicture = frame.PrivateColorCopy = frame.IndependentCommands = true;
    frame.GenshinPresentPipeline = frame.FullModelRequired = frame.NativeSceneRoute = true;
    frame.BeforeUpscale = false;
    frame.SubmissionEpoch = frame.SourceCaptureSerial = source.ordinal;
    frame.SourceCaptureEpoch = source.generation;
    frame.NativePresentQueueIdentity = present;
    frame.NativeBaseIdentityVerified = true; // exact source + ordered handoff above
    frame.NativeCurrentDepthVerified = frame.NativeFeatureFlagsKnown &&
        Late::NativeRegionFits(slot.depth.Get(), frame.DepthSubrectBaseX, frame.DepthSubrectBaseY,
                              frame.RenderSubrectWidth, frame.RenderSubrectHeight);
    frame.NativeRouteConventionVerified = frame.NativeGuideMetadataKnown;
    frame.ColourIsLinearHdr = pq || scrgb;
    frame.InputEncoding = choice.shaderConversion;
    frame.ColourEncoding = uint32_t(choice.choice.encoding);
    frame.ColourEncodingForced = !choice.choice.automatic;
    frame.WhitePointOverride = pq || scrgb ? 203.0f / 80.0f : 0.0f;
    frame.OutputArrivalState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    frame.InputArrivalState = pq ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_PRESENT;
    frame.Reset |= !NrPresentation::Follows(lastWrittenSource, source) ||
                   lastOutputOwner != outcome.owner || lastOutputQueue != outcome.queue;
    // Save the authenticated scalars for the shared private-lifecycle owner.
    slot.frame.SourceCaptureSerial = frame.SourceCaptureSerial;
    slot.frame.SourceCaptureEpoch = frame.SourceCaptureEpoch;
    if (slot.depth) Barrier(cmd, slot.depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmd, slot.motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    DlssNrNative::SetPrecision(cfg.DlssNrPrecision.value_or_default());
    const auto before = g_nr.successfulDispatches;
    {
        DlssNrCurrentOutput::DeferEndpoint atFinalEndpoint;
        g_compose->Dispatch(cmd, pq ? slot.presentWork.Get() : color.Get(), slot.depth.Get(), slot.motion.Get(),
                            slot.presentWork.Get(), frame, present);
    }
    const bool recorded = g_nr.successfulDispatches > before;
    bool written = recorded && cfg.DlssNrApplyModel.value_or_default();
    ID3D12Resource* finalColor = slot.presentWork.Get();
    if (written && pq)
    {
        conversion.Mode = 1;
        Barrier(cmd, finalColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        written = g_compose->DispatchResidualPass(cmd, conversion, finalColor, nullptr, color.Get(), nullptr, slot.encoded.Get());
        Barrier(cmd, color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT);
        Barrier(cmd, finalColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        finalColor = slot.encoded.Get();
    }
    if (written)
    {
        written = NrPresentation::RecordPublish(cmd, finalColor, color.Get());
    }
    Barrier(cmd, slot.motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (slot.depth) Barrier(cmd, slot.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (FAILED(cmd->Close()))
    {
        slot.quarantined = true;
        outcome.result = FinishedPictureResult::FailedUncertain;
        outcome.reason = FinishedPictureReason::RecordingFailed;
        lastWrittenSource = {};
        return outcome;
    }
    ID3D12CommandList* lists[] {cmd};
    slot.done = slot.ready + 1;
    const auto callbacks = finishedPictureSubmitCallbacks;
    const auto previewReceipt = FinishedPictureSubmitting(present, 1, lists);
    present->ExecuteCommandLists(1, lists);
    if (finishedPictureSubmitCallbacks == callbacks) FinishedPictureSubmitted(present, 1, lists, previewReceipt);
    if (FAILED(present->Signal(slot.fence.Get(), slot.done)))
    {
        slot.quarantined = true;
        outcome.result = FinishedPictureResult::FailedUncertain;
        outcome.reason = FinishedPictureReason::SubmissionFailed;
        lastWrittenSource = {};
        return outcome;
    }
    outcome.fenceValue = slot.done;
    outcome.result = written ? FinishedPictureResult::WrittenOrdered : FinishedPictureResult::NoWrite;
    outcome.reason = written ? FinishedPictureReason::None : FinishedPictureReason::ProviderFailed;
    lastWrittenSource = written ? source : NrPresentation::Source {};
    lastOutputOwner = outcome.owner;
    lastOutputQueue = outcome.queue;
    Late::Say(written ? "Present -> NR -> detail retention -> FG (current source, one write)." :
                        "Preparing NR; presenting the current unmodified source.");
    static uint64_t sources = 0, writes = 0;
    writes += written;
    if (++sources <= 3 || sources % 300 == 0)
        LOG_INFO("F6 Genshin presentation: sources={} writes={} source={} feature={} generation={} attempt={} "
                 "capture={} present={} queue={} fence={} result={} rawInput=current-present privateWork=true "
                 "nrAtEvaluate=false nrAtGeneratedPresent=false historyModelInput=false",
            sources, writes, source.ordinal, source.feature, source.generation, attempt, slot.serial,
            outcome.presentEpoch, outcome.queue, outcome.fenceValue, unsigned(outcome.result));
    return outcome;
}
}
