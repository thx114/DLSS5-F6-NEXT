#include <runtime/ComIdentity.h>
#include "pch.h"
#include <runtime/SwapchainColorSpace.h>
#include <integration/HoYoShade.h>
#include <integration/HoYoShadeNaturalAssociation.h>
#include <native/OpticalFgNrPolicy.h>

#include "Dx11FrameSource.h"
#include <runtime/NativeProducerCpuTiming.h>
#include <ui_mask/DrawUiMask.h>
#include "BridgeDepthReader.h"
#include <runtime/GameIdentity.h>

#include <Util.h>
#include <dlssnr/DlssNrFeature_Dx12.h>
#include <resource_tracking/GenericDepth_Dx11.h>
#include <with_dx12/with_dx12.h>

namespace native
{

namespace
{
bool RequireExactSharedOwner() { return false; }
ColorSpace ToColorSpace(DXGI_COLOR_SPACE_TYPE type)
{
    switch (type)
    {
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        return ColorSpace::ScRgb;
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        return ColorSpace::Pq;
    default:
        return ColorSpace::Srgb;
    }
}
} // namespace

void Dx11FrameSource::StampDelivery(DeliveryObservation::Boundary boundary) noexcept
{
    if (_delivery.capturing && _deliveryThread == GetCurrentThreadId())
        _delivery.ticks[boundary] = NativeProducerCpuTiming::Qpc();
}

void Dx11FrameSource::ReportDeliveryObservation()
{
    const auto now = GetTickCount64();
    if (!_delivery.active || (_deliveryLastReport && now - _deliveryLastReport < 1000)) return;
    const auto& d = _delivery;
    const auto frequency = NativeProducerCpuTiming::QpcFrequency();
    const auto elapsed = [&](DeliveryObservation::Boundary from, DeliveryObservation::Boundary to) {
        return frequency && d.ticks[from] > 0 && d.ticks[to] >= d.ticks[from]
            ? double(d.ticks[to] - d.ticks[from]) * 1000.0 / double(frequency) : -1.0;
    };
    const auto sharedNow = d.sharedFence ? d.sharedFence->GetCompletedValue() : UINT64_MAX;
    const auto doneNow = d.doneFence ? d.doneFence->GetCompletedValue() : UINT64_MAX;
    LOG_INFO("Native DX11 delivery: pid={} tid={} frame={} generation={} source_frame={} present_epoch={} picture_serial={} "
        "context={:X} queue={:X} picture={:X} backbuffer={:X} acquired={} returned={} copy_back={} processed={} uncertain={} "
        "previous_shared_use={} shared_fence={:X} previous_copy={} shared_before={} ready={} shared_after_upload={} "
        "done_fence={:X} done_value={} done_on_return={} return_value={} copy_value={} shared_after_copy={} "
        "shared_observed_later={} done_observed_later={} qpc_hz={} begin_qpc={} acquire_qpc={} return_qpc={} end_qpc={} "
        "acquire_ms={:.4f} setup_previous_order_ms={:.4f} upload_copy_ms={:.4f} depth_ms={:.4f} mask_ms={:.4f} "
        "upload_signal_ms={:.4f} upload_flush_ms={:.4f} run_gap_ms={:.4f} return_signal_ms={:.4f} "
        "return_wait_ms={:.4f} copyback_ms={:.4f} copy_signal_ms={:.4f} copy_flush_ms={:.4f} "
        "cpu_wall_inclusive=true missing_ms=-1 removed_or_absent_fence=UINT64_MAX "
        "observed_qpc={} completion_time=UNKNOWN gpu_wait_duration=UNKNOWN observation_only=true no_added_wait=true",
        GetCurrentProcessId(), _deliveryThread, d.frame, d.generation, d.sourceFrame, d.presentEpoch, d.pictureSerial,
        d.context, d.queue, d.picture, d.backbuffer, d.acquired, d.returned, d.copyBack, d.processed, d.uncertain,
        d.previousSharedUse, reinterpret_cast<uintptr_t>(d.sharedFence.Get()), d.previousCopy, d.sharedBefore,
        d.ready, d.sharedAfterUpload, reinterpret_cast<uintptr_t>(d.doneFence.Get()), d.doneValue, d.doneOnReturn,
        d.returnValue, d.copyValue, d.sharedAfterCopy, sharedNow, doneNow, frequency,
        d.ticks[DeliveryObservation::Begin], d.ticks[DeliveryObservation::Acquired],
        d.ticks[DeliveryObservation::Returned], d.ticks[DeliveryObservation::End],
        elapsed(DeliveryObservation::Begin, DeliveryObservation::Acquired),
        elapsed(DeliveryObservation::Begin, DeliveryObservation::PreviousOrder),
        elapsed(DeliveryObservation::PreviousOrder, DeliveryObservation::UploadCopy),
        elapsed(DeliveryObservation::UploadCopy, DeliveryObservation::Depth),
        elapsed(DeliveryObservation::Depth, DeliveryObservation::Mask),
        elapsed(DeliveryObservation::Mask, DeliveryObservation::UploadSignal),
        elapsed(DeliveryObservation::UploadSignal, DeliveryObservation::UploadFlush),
        elapsed(DeliveryObservation::Acquired, DeliveryObservation::Returned),
        elapsed(DeliveryObservation::Returned, DeliveryObservation::ReturnSignal),
        elapsed(DeliveryObservation::ReturnSignal, DeliveryObservation::ReturnWait),
        elapsed(DeliveryObservation::ReturnWait, DeliveryObservation::Copyback),
        elapsed(DeliveryObservation::Copyback, DeliveryObservation::CopySignal),
        elapsed(DeliveryObservation::CopySignal, DeliveryObservation::CopyFlush), NativeProducerCpuTiming::Qpc());
    _deliveryLastReport = now;
    _delivery = {};
}

void Dx11FrameSource::BeginDeliveryObservation()
{
    const auto thread = GetCurrentThreadId();
    if (!_deliveryThread) _deliveryThread = thread;
    if (_deliveryThread != thread) return;
    _delivery.capturing = false;
    ReportDeliveryObservation(); // outside the next sampled Acquire interval
    const auto now = GetTickCount64();
    if (_delivery.active || (_deliveryLastSample && now - _deliveryLastSample < 1000)) return;
    _deliveryLastSample = now;
    _delivery.active = _delivery.capturing = true;
    _delivery.previousSharedUse = _sharedInUse;
    _delivery.copyBack = _copyBack;
    _delivery.previousCopy = _copybackCompletion;
    StampDelivery(DeliveryObservation::Begin);
}

void Dx11FrameSource::SetPresent(IDXGISwapChain* swapChain, ID3D11Device* device11, bool copyBack)
{
    _swapChain = swapChain;
    _copyBack = copyBack;

    if (!_quarantined && CanReleaseShared() && device11 != _device11)
    {
        _device11 = device11;
        _realDevice11 = device11;

        IUnknown* real = nullptr;

        if (device11 != nullptr && Util::CheckForRealObject(__FUNCTION__, device11, &real))
            _realDevice11 = (ID3D11Device*) real;

        _context11.Reset();
        _context4.Reset();
        _device12 = nullptr;
        _queue12 = nullptr;
        _fence.Reset();
        _fenceCreateAttempted = false;
        _copybackCompletion = 0;
        _picture.Reset();
        _depth.Reset();
        _msaa.Release();
    }
}

bool Dx11FrameSource::EnsureDevices(ID3D11Device* device11)
{
    if (_context11 == nullptr)
    {
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        device11->GetImmediateContext(&context);

        if (context == nullptr || FAILED(context.As(&_context4)))
        {
            _error = "the D3D11 device's immediate context does not support ID3D11DeviceContext4 (needs Windows 10 "
                     "1703 or later)";
            return false;
        }

        _context11 = context;
    }

    if (_device12 == nullptr || _queue12 == nullptr)
    {
        if (!WithDx12::PrepareD3D12ForD3D11(device11, D3D_FEATURE_LEVEL_11_0))
        {
            _error = "could not make the D3D12 device paired with this D3D11 device";
            return false;
        }

        _device12 = WithDx12::GetD3D12Device();
        _queue12 = WithDx12::GetD3D12CommandQueue();

        if (_device12 == nullptr || _queue12 == nullptr)
        {
            _error = "the paired D3D12 device or queue is not ready";
            return false;
        }
    }

    if (!_fence.Ready())
    {
        // A rejected public import remains rejected for this device binding.
        // Do not reopen handles or repeat creation diagnostics every Present.
        if (_fenceCreateAttempted) return false;
        _fenceCreateAttempted = true;
        const bool ready = _fence.Create(_device12.Get(), device11);
        LOG_INFO("Present shared fence domain: ready={} creator=0x{:x} queue=0x{:x} {} error={}",
            ready, reinterpret_cast<uintptr_t>(_device12.Get()), reinterpret_cast<uintptr_t>(_queue12.Get()),
            _fence.Diagnostic(), _fence.Error());
        if (!ready)
        {
            _error = _fence.Error();
            return false;
        }
    }

    return true;
}

AcquireStatus Dx11FrameSource::Acquire(FrameInput& input)
{
    _backBuffer.Reset();
    struct CaptureReference {
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& buffer;
        bool ready=false;
        ~CaptureReference() { if (!ready) buffer.Reset(); }
    } capture{_backBuffer};

    // A failed sampled Return can quarantine this source. Preserve its final
    // observation even when no later input is eligible for sampling.
    if (_deliveryThread == GetCurrentThreadId() && (_quarantined || !_delivery.capturing)) {
        if (_quarantined && _delivery.active) {
            StampDelivery(DeliveryObservation::End);
            _delivery.uncertain=true;
            _delivery.capturing=false;
        }
        ReportDeliveryObservation();
    }
    if (_quarantined || _swapChain == nullptr || _device11 == nullptr)
        return AcquireStatus::Unavailable;

    BeginDeliveryObservation();
    const bool colorStage = _copyBack && (HoYoShade::Selected(HoYoShade::Stage::BeforeNr) ||
        HoYoShade::Selected(HoYoShade::Stage::FinalOutput));
    const bool gameUpscaler = !native::OpticalFgNrEnabled() && GenericDepthDx11::GameCallsUpscaler();
    if (gameUpscaler && !colorStage)
        return AcquireStatus::WaitingForUpscaler;

    if (!EnsureDevices(_device11))
        return AcquireStatus::Unavailable;
    if (_delivery.capturing && _deliveryThread == GetCurrentThreadId()) {
        _delivery.sharedFence=_fence.Fence12();
        _delivery.previousCopy=_copybackCompletion;
        _delivery.sharedBefore=_fence.Fence12()->GetCompletedValue();
        _delivery.context=reinterpret_cast<uintptr_t>(_context11.Get());
        _delivery.queue=reinterpret_cast<uintptr_t>(_queue12.Get());
    }

    UINT backbufferIndex=0;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> indexed;
    if(SUCCEEDED(_swapChain->QueryInterface(IID_PPV_ARGS(&indexed))))backbufferIndex=indexed->GetCurrentBackBufferIndex();
    if (FAILED(_swapChain->GetBuffer(backbufferIndex, IID_PPV_ARGS(&_backBuffer))))
    {
        _error = "could not get the swap chain's buffer";
        return AcquireStatus::Unavailable;
    }

    D3D11_TEXTURE2D_DESC desc {};
    _backBuffer->GetDesc(&desc);

    const DXGI_FORMAT pictureFormat = SharedPictureFormat(desc.Format);
    const bool multisampled = desc.SampleDesc.Count > 1;

    if (!_picture.Matches(desc.Width, desc.Height, pictureFormat))
    {
        if (!CanReleaseShared()) return AcquireStatus::Unavailable;
        // The multisampled path holds the picture's read view
        _msaa.Release();

        // DLSS-NR's compose pass writes its result straight back into `color` through a UAV (the same in-place
        // pattern as the D3D12 finished-picture path): shader-resource-only here left the D3D12 side unable to
        // create that view, so the model ran (GPU time, "successful" dispatch) but its write never landed anywhere.
        if (!_picture.Create(_device11, desc.Width, desc.Height, pictureFormat,
                             D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS) ||
            !_picture.Open(_device12.Get(), RequireExactSharedOwner()))
        {
            _error = _picture.Error();
            static bool loggedPictureFail = false;

            if (!loggedPictureFail)
            {
                loggedPictureFail = true;
                LOG_WARN("Native motion (D3D11): sharing the picture failed: {}", _error);
            }

            return AcquireStatus::Unavailable;
        }
    }

    // Under frame generation the last frame's D3D12 work (the producer, then the bridge's copy of the result into frame
    // generation's back buffer, both on _queue12) is not waited for at its present: it is here, a frame later, just
    // before the shared textures it reads are overwritten. A signal now is ordered after all of it on that queue.
    if (_sharedInUse)
    {
        const uint64_t releasedValue = _fence.Next();
        if(FAILED(_queue12->Signal(_fence.Fence12(), releasedValue)) ||
           FAILED(_context4->Wait(_fence.Fence11(), releasedValue))) {
            _quarantined=true;_uncertainBackBuffer=_backBuffer;
            _error="previous shared-picture ordering failed";return AcquireStatus::Unavailable;
        }
        _sharedInUse = false;
    }

    StampDelivery(DeliveryObservation::PreviousOrder);
    if (_delivery.capturing && _deliveryThread == GetCurrentThreadId()) {
        _delivery.picture=reinterpret_cast<uintptr_t>(_picture.Res12());
        _delivery.backbuffer=reinterpret_cast<uintptr_t>(_backBuffer.Get());
    }
    if (!multisampled)
    {
        _context11->CopyResource(_picture.Tex11(), _backBuffer.Get());
    }
    else
    {
        // A copy between a multisampled and a single-sample texture is dropped by D3D11 without a word: NR would cost GPU time
        // and show nothing. Resolve the samples into the picture instead (Return draws the result back into every sample).
        // Under frame generation's swap chain (Dx11wDx12SC) the back buffer is the hidden real one; the bridge copies the
        // same buffer out on its own (resolving it too) before this runs and takes NR's result from the D3D12 side, so
        // nothing here depends on the order of the two.
        static bool loggedMultisampled = false;

        if (!loggedMultisampled)
        {
            loggedMultisampled = true;
            LOG_INFO("Native motion (D3D11): the game's swap chain is multisampled ({} samples): resolved in, drawn back",
                     desc.SampleDesc.Count);
        }

        if (const char* why = _msaa.ResolveIn(_context11.Get(), _backBuffer.Get(), _picture.Tex11()))
        {
            _error = why;
            static const char* loggedReason = nullptr;

            if (loggedReason != why)
            {
                loggedReason = why;
                LOG_WARN("Native motion (D3D11): resolving the multisampled back buffer failed: {}", why);
            }

            return AcquireStatus::Unavailable;
        }
    }

    StampDelivery(DeliveryObservation::UploadCopy);
    // Present NR measures the final picture itself. Raw scene depth is not
    // silently promoted to postprocessing/HUD depth authority.
    auto depthSnap = GenericDepthDx11::BestSnapshot();
    const char* genericDepthStage = depthSnap.valid ? "snapshot" : "no-snapshot";
    if(colorStage && (gameUpscaler || !Config::Instance()->DlssNrEnabled.value_or_default()))
    {
        genericDepthStage = "suppressed-color-stage";
        depthSnap.valid=false;
    }
    bool depthReady = false;

    // The finder follows the device the game presents with, a frame or more after a new one appears: until then its copy can be
    // of the old device, which this device's context cannot copy from (the runtime drops the copy).
    Microsoft::WRL::ComPtr<ID3D11Device> snapDevice;

    if (depthSnap.valid)
        depthSnap.resource->GetDevice(&snapDevice);

    if (depthSnap.valid && snapDevice.Get() != _device11 && snapDevice.Get() != _realDevice11)
    {
        genericDepthStage = "snapshot-device-mismatch";
    }
    else if (depthSnap.valid)
    {
        bool haveTexture = _depth.Matches(depthSnap.width, depthSnap.height, depthSnap.typelessFormat);

        if (!haveTexture)
            haveTexture = _depth.Create(_device11, depthSnap.width, depthSnap.height, depthSnap.typelessFormat,
                                        D3D11_BIND_SHADER_RESOURCE);

        if (haveTexture && _depth.Open(_device12.Get(), RequireExactSharedOwner()))
        {
            depthReady = true;
            genericDepthStage = "copy-ordered";
            _context11->CopyResource(_depth.Tex11(), depthSnap.resource.Get());
        }
        else
        {
            genericDepthStage = haveTexture ? "shared-open-failed" : "shared-create-failed";
        }
    }

    // This same-context copy needs a COMPLETED producer receipt, an exact
    // picture relation, and ownership armed before touching the D3D11 source.
    const uint64_t signalValue = _fence.Next();
    // Mask commands were recorded at THIS Present, after all selected UI.
    // This upload signal/Flush orders them, not the earlier SR producer fence.
    StampDelivery(DeliveryObservation::Depth);
    auto uiMask=DrawUiMask::Acquire(_context11.Get(),_backBuffer.Get(),_device12.Get());
    StampDelivery(DeliveryObservation::Mask);
    if(FAILED(_context4->Signal(_fence.Fence11(), signalValue))) {
        _quarantined=true;_uncertainBackBuffer=_backBuffer;
        _error="D3D11 picture-ready signal failed";return AcquireStatus::Unavailable;
    }
    StampDelivery(DeliveryObservation::UploadSignal);
    _context11->Flush();
    StampDelivery(DeliveryObservation::UploadFlush);
    if (_delivery.capturing && _deliveryThread == GetCurrentThreadId()) {
        _delivery.ready=signalValue;
        _delivery.sharedAfterUpload=_fence.Fence12()->GetCompletedValue();
    }

    input = FrameInput {};
    input.uiMask=std::move(uiMask);
    input.api = Api::D3D11;
    input.frame = ++_frame;
    input.naturalDevice = reinterpret_cast<uint64_t>(_device11);
    input.naturalBackbuffer = reinterpret_cast<uint64_t>(_backBuffer.Get());
    if (colorStage)
        input.naturalSource = HoYoShade::NaturalAssociation::Source::Capture(_swapChain, _device11,
            _backBuffer.Get(), backbufferIndex, reinterpret_cast<uint64_t>(static_cast<IFrameSource*>(this)), 1, input.frame);
    input.finalOriginal = _copyBack;
    input.colorOnly = colorStage && (gameUpscaler ||
        (!false && !Config::Instance()->DlssNrNativeMotion.value_or_default()) ||
        !Config::Instance()->DlssNrEnabled.value_or_default());
    input.picture = _picture.Res12();
    input.pictureFormat = pictureFormat;
    input.pictureState = D3D12_RESOURCE_STATE_COMMON;
    // Preserve the helper's exact raw observation without changing NR's
    // mapping/default/lookup. Its private-data HRESULT is not exposed.
    const auto rawNativeColorSpace = DlssNr::NativeInputColourSpace(_swapChain, desc.Format);
    input.colorSpace = ToColorSpace(rawNativeColorSpace);
    input.nativePictureColor.sourceFormat = desc.Format;
    input.nativePictureColor.sharedFormat = _picture.Format();
    input.nativePictureColor.rawColorSpace = rawNativeColorSpace;
    input.nativePictureColor.origin = NativePictureColorOrigin::SwapchainHelperDefaultOrPrivateUnknown;
    const auto& outputColor = State::Instance().outputColorSpace;
    input.nativePictureColor.knownHdr = State::Instance().hdrOutputActive ||
        (outputColor.valid && (outputColor.transfer == ColorTransfer::PQ || outputColor.transfer == ColorTransfer::HLG ||
            outputColor.dxgiColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 ||
            outputColor.dxgiColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020));
    input.filterSdrAllowed = SwapchainColorSpace::ExplicitSdr(_swapChain);
    input.width = desc.Width;
    input.height = desc.Height;

    if (depthReady && !false)
    {
        input.depth[0] = _depth.Res12();
        input.depthCount = 1;
        input.depthView = depthSnap.viewFormat;
        input.depthWidth = depthSnap.width;
        input.depthHeight = depthSnap.height;
        input.depthReversed = depthSnap.reversed;
    }

    input.ready = SyncPoint { _fence.Fence12(), signalValue };
    // Diagnose the final admitted input, not the intentionally suppressed
    // generic snapshot. Bridge/native attempts and optional BindInput refusal
    // have all finished; a successful alternative never emits a missing-depth warning.
    static thread_local uint64_t noDepthStreak = 0;
    static thread_local uint64_t lastNoDepthReport = 0;
    if (input.depthCount > 0)
        noDepthStreak = 0;
    else
    {
        ++noDepthStreak;
        const auto now = GetTickCount64();
        if (!lastNoDepthReport || now - lastNoDepthReport >= 30000)
        {
            lastNoDepthReport = now;
        }
    }
    if (_delivery.capturing && _deliveryThread == GetCurrentThreadId()) {
        _delivery.frame=input.frame;
        _delivery.acquired=true;
        StampDelivery(DeliveryObservation::Acquired);
    }
    capture.ready=true;
    return AcquireStatus::Ready;
}

void Dx11FrameSource::Return(const FrameInput& input, const FrameOutput& output)
{
    if (_delivery.capturing && _deliveryThread == GetCurrentThreadId() &&
        _delivery.acquired && _delivery.frame==input.frame) {
        StampDelivery(DeliveryObservation::Returned);
        _delivery.returned=true;
        _delivery.processed=output.processed;
        _delivery.uncertain=output.uncertain;
        _delivery.doneFence=output.done.fence;
        _delivery.doneValue=output.done.value;
        _delivery.doneOnReturn=output.done.fence?output.done.fence->GetCompletedValue():UINT64_MAX;
    } else if (_deliveryThread == GetCurrentThreadId()) _delivery.capturing=false;
    struct DeliveryExit {
        Dx11FrameSource& source;
        const bool sampled;
        ~DeliveryExit() {
            if (!sampled) return;
            source._delivery.uncertain |= source._quarantined;
            source.StampDelivery(DeliveryObservation::End);
            source._delivery.capturing=false;
        }
    } deliveryExit{*this, _delivery.capturing && _deliveryThread == GetCurrentThreadId()};
    if (output.uncertain) {
        _quarantined = true; _uncertainBackBuffer = _backBuffer; return;
    }
    if (_backBuffer == nullptr)
        return;

    // Under frame generation's swap chain nothing shows the D3D11 back buffer: the bridge copies ProcessedPicture() out
    // on the D3D12 side. Waiting here would hold the game's next D3D11 frame on the GPU until all of this frame's D3D12
    // work ran, every frame; the next Acquire waits instead, only before it overwrites the shared textures.
    if (!_copyBack)
    {
        _sharedInUse = true;
        _backBuffer.Reset();
        return;
    }

    // output.done names NativeProducer's own internal fence (shared with no one -- it only tells the D3D12 adapter, which
    // reads nothing back across an API boundary, that a frame was submitted). Waiting on it directly here was a bug: it
    // checked output.done.fence for non-null but then waited on OUR shared fence for OUR value, two unrelated counters
    // that happened to track closely enough to let the wait return immediately without ever actually waiting for the
    // producer's GPU work -- so the D3D11 copy-back could run before DLSS-NR's write ever landed, and did.
    // A fresh signal on the private queue, right after the producer's ExecuteCommandLists (already submitted when Run()
    // returned, so this Signal is ordered after it on the GPU regardless of CPU timing), gives the D3D11 context
    // something real to wait on.
    if (output.done.fence != nullptr && _queue12 != nullptr)
    {
        const uint64_t doneValue = _fence.Next();
        const auto signalHr=_queue12->Signal(_fence.Fence12(), doneValue);
        StampDelivery(DeliveryObservation::ReturnSignal);
        if (_delivery.capturing && _deliveryThread == GetCurrentThreadId()) _delivery.returnValue=doneValue;
        const auto waitHr=SUCCEEDED(signalHr)?_context4->Wait(_fence.Fence11(), doneValue):signalHr;
        StampDelivery(DeliveryObservation::ReturnWait);
        if(FAILED(signalHr) || FAILED(waitHr)) {
            _quarantined=true;_uncertainBackBuffer=_backBuffer;
            _error="D3D12 picture-return ordering failed";_backBuffer.Reset();return;
        }
    }

    D3D11_TEXTURE2D_DESC backDesc {};
    _backBuffer->GetDesc(&backDesc);

    // Warmup/failed consumers only ran flow preparation. Keep the actual
    // native backbuffer; still order/signal its shared-picture GPU lifetime.
    const bool copyPicture=true;
    bool pictureCopyIssued=false;
    if(copyPicture) {
        if (backDesc.SampleDesc.Count <= 1)
        {
            _context11->CopyResource(_backBuffer.Get(), _picture.Tex11());
            pictureCopyIssued=true;
            }
        else if (const char* why = _msaa.WriteBack(_context11.Get(), _picture.Tex11(), _backBuffer.Get()))
        {
            // The frame goes out as the game drew it
            _error = why;
            static const char* loggedReason = nullptr;

            if (loggedReason != why)
            {
                loggedReason = why;
                LOG_WARN("Native motion (D3D11): drawing the result into the multisampled back buffer failed: {}", why);
            }
        }
        else pictureCopyIssued=true;
    }

    // Every cross-API color return needs its actual D3D11 retirement, including
    // NR-off HoYo color-only frames without a GenshinPresentNr scope. The owned
    // or NativeProducer D3D12 fence cannot retire this later D3D11 texture read.
    // Optional FG snapshot still belongs to the same existing return signal.
    StampDelivery(DeliveryObservation::Copyback);
    const auto copied=_fence.Next();
    if(FAILED(_context4->Signal(_fence.Fence11(),copied))) {
        _quarantined=true;_uncertainBackBuffer=_backBuffer;
        _error="D3D11 copyback completion signal failed";_backBuffer.Reset();return;
    }
    StampDelivery(DeliveryObservation::CopySignal);
    _copybackCompletion=copied;_context11->Flush();
    StampDelivery(DeliveryObservation::CopyFlush);
    if (_delivery.capturing && _deliveryThread == GetCurrentThreadId()) {
        _delivery.copyValue=copied;
        _delivery.sharedAfterCopy=_fence.Fence12()->GetCompletedValue();
    }
    _backBuffer.Reset();
}

void Dx11FrameSource::OnResize()
{
    if (_quarantined || !CanReleaseShared()) return; // retain resources until their actual last use completes
    _picture.Reset();
    _depth.Reset();
    _backBuffer.Reset();
    _msaa.Release();
}

} // namespace native
