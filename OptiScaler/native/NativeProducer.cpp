// Not built with the precompiled header: no game headers in here, so tests/nr_native_producer_gpu.cpp compiles it alone.
#include "NativeProducer.h"
#include "NativeSubmissionReceipt.h"
#include <ui_mask/DrawUiMask.h>

#include <algorithm>

namespace native
{

NativeProducer::~NativeProducer()
{
    // No completion witness, no release (including failed Signal/device removal).
    // Keep GPU-referenced owners for process lifetime; cancellation is not completion.
    if (!CanRetire()) {
        for(UINT slot=0;slot<kRing;++slot)if(_held[slot])
            DrawUiMask::Observe(_held[slot]->input.uiMask,DrawUiMask::Consumer::Nr,DrawUiMask::Lifecycle::Quarantined,
                reinterpret_cast<uintptr_t>(this),_held[slot]->inputEpoch,0);
        _flow.release(); _trust.release();
        _prepGpu.release(); // the real final fence is unproved; retain query/readback owners too
        _domain.release();
        for (auto& hold : _held) hold.release();
        _queue.Detach(); _device.Detach(); // retain the exact in-flight device/queue as well
        return;
    }
    for (auto& allocator : _allocators)
        if (allocator != nullptr)
            allocator->Release();

    for (auto& allocator : _consumerAllocators)
        if (allocator != nullptr)
            allocator->Release();

    for (auto& readback : _cutReadback)
        if (readback != nullptr)
            readback->Release();

    if (_list != nullptr)
        _list->Release();
    if (_consumerList != nullptr)
        _consumerList->Release();
    if (_fence != nullptr)
        _fence->Release();
    if (_event != nullptr)
        CloseHandle(_event);
}

bool NativeProducer::CanRetire() const
{
    if (_quarantined) return false;
    if (_signalled == 0) return true; // never submitted
    HRESULT hr=S_OK;
    if (!_fence || !_domain->Queue(_queue.Get(),hr) || !_domain->Object(_fence,true,hr)) return false;
    const auto completed = _fence->GetCompletedValue();
    if(completed==UINT64_MAX || completed<_signalled)return false;
    // Existing public retirement queries need not be on the Run owner thread.
    // Read held slots only at its already exclusive observation boundary.
    if(GetCurrentThreadId()==_heldOwnerThread && !_heldAccessActive)
        for(UINT slot=0;slot<kRing;++slot)if(_held[slot] && _values[slot])
            DrawUiMask::Observe(_held[slot]->input.uiMask,DrawUiMask::Consumer::Nr,DrawUiMask::Lifecycle::Completed,
                reinterpret_cast<uintptr_t>(this),_held[slot]->inputEpoch,_values[slot]);
    return true;
}

void NativeProducer::SealHeldFinalSubmission(UINT slot) noexcept
{
    if(slot<kRing && _prepGpu && !_quarantined)
        _prepGpu->Submitted(slot,_queue.Get(),_fence,_values[slot]);
    // Existing successful final exits only; no intermediate producer/filter tail.
    if(slot<kRing && _held[slot] && _values[slot])
        DrawUiMask::Observe(_held[slot]->input.uiMask,DrawUiMask::Consumer::Nr,DrawUiMask::Lifecycle::Submitted,
            reinterpret_cast<uintptr_t>(this),_held[slot]->inputEpoch,_values[slot]);
    // Called only at the two successful final exits, after every optional
    // split/filter/consumer submission. Intermediate signals grant no receipt.
    if (GetCurrentThreadId()!=_heldOwnerThread || !_heldAccessActive || _quarantined ||
        !_initialized || slot>=kRing || !_held[slot] || !_held[slot]->input.gameMotion ||
        !_domain || !_device || !_queue || !_fence || !_signalled || _signalled==UINT64_MAX ||
        _values[slot]!=_signalled || !_domain->Generation() || _domain->Generation()==UINT64_MAX)
        return;
    auto& held=*_held[slot];
    if (!held.inputEpoch || held.inputEpoch==UINT64_MAX || held.finalSubmission.fence) return;
    HRESULT hr=S_OK;
    if (_domain->Creator()!=_device.Get() || !_domain->PublicOwner() ||
        !_domain->Queue(_queue.Get(),hr) || !_domain->Object(_fence,true,hr)) return;
    held.finalSubmission={_device,_domain->PublicOwner(),_queue,_fence,
        _domain->Generation(),held.inputEpoch,held.input.frame,_signalled};
}

unsigned NativeProducer::CollectCompletedMotionReferences() noexcept
{
    // No wait, submission, teardown, pool mutation or use_count exception.
    // The creation/Run owner is the only caller allowed to inspect these slots.
    if (GetCurrentThreadId()!=_heldOwnerThread || _heldAccessActive || _quarantined ||
        !_initialized || !_domain || !_device || !_queue || !_fence) return 0;
    HeldAccessScope access(_heldAccessActive); // ref destruction cannot reenter
    HRESULT hr=S_OK;
    if (_domain->Creator()!=_device.Get() || !_domain->Generation() ||
        _domain->Generation()==UINT64_MAX || !_domain->PublicOwner() ||
        !_domain->Queue(_queue.Get(),hr) || !_domain->Object(_fence,true,hr)) return 0;
    unsigned released=0;
    for (auto& slot:_held) {
        if (!slot || !slot->input.gameMotion) continue;
        const auto& receipt=slot->finalSubmission;
        if (!receipt.generation || receipt.generation!=_domain->Generation() ||
            !receipt.inputEpoch || receipt.inputEpoch==UINT64_MAX || receipt.inputEpoch!=slot->inputEpoch ||
            receipt.frame!=slot->input.frame || receipt.creator.Get()!=_device.Get() ||
            receipt.publicOwner.Get()!=_domain->PublicOwner() || receipt.queue.Get()!=_queue.Get() ||
            receipt.fence.Get()!=_fence || !receipt.value || receipt.value==UINT64_MAX) continue;
        const auto completed=receipt.fence->GetCompletedValue();
        if (completed==UINT64_MAX || completed<receipt.value) continue;
        // Reset only our own copy. Other GPU readers retain their own roots;
        // the motion pool still needs actual completion AND sole ownership.
        slot->input.gameMotion.reset();
        ++released;
    }
    return released;
}

bool NativeProducer::WaitFor(UINT64 value)
{
    if (value == 0) return true;
    HRESULT hr=S_OK;
    if (!_fence || !_event || !_domain->Queue(_queue.Get(),hr) || !_domain->Object(_fence,true,hr)) return false;
    const auto completed = _fence->GetCompletedValue();
    if (completed == UINT64_MAX) return false;
    if (completed >= value) return true;
    if (FAILED(_fence->SetEventOnCompletion(value, _event))) return false;
    if (WaitForSingleObject(_event, 5000) != WAIT_OBJECT_0) return false;
    const auto after = _fence->GetCompletedValue();
    return after != UINT64_MAX && after >= value;
}

bool NativeProducer::EnsureGuides()
{
    if (_guidesReady) return true;
    if (!_flow->Init(_device.Get())) { _error = _flow->Error(); return false; }
    if (!_trust->Init(_device.Get())) { _error = _trust->Error(); return false; }
    _guidesReady = true;
    return true;
}

bool NativeProducer::Init(const NativeWorkDevice& work, bool prepareGuides)
{
    if (_device || !_domain->Bind(work)) { _error="invalid or repeated producer creation binding"; return false; }
    _device=work.device; _queue=work.queue;
    auto* device=_domain->Creator();
    const auto fail=[&]() { _error=_domain->Report(); return false; };
    HRESULT hr=S_OK;
    for (uint32_t i=0;i<kRing;++i) {
        hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&_allocators[i]));
        if (!_domain->Created("producer-allocator",hr,_allocators[i],true,i==0)) return fail();
    }
    for (auto& allocator:_consumerAllocators) {
        hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator));
        if (!_domain->Created("consumer-allocator",hr,allocator,true)) return fail();
    }
    hr=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,_allocators[0],nullptr,IID_PPV_ARGS(&_list));
    if (!_domain->Created("producer-list",hr,_list,false) || !_domain->PairCreated(_list,_allocators[0],hr)) return fail();
    hr=_list->Close();
    if (FAILED(hr)) { _error="closing initial producer list hr="+std::to_string(hr); return false; }
    hr=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,_consumerAllocators[0],nullptr,IID_PPV_ARGS(&_consumerList));
    if (!_domain->Created("consumer-list",hr,_consumerList,false) || !_domain->PairCreated(_consumerList,_consumerAllocators[0],hr)) return fail();
    hr=_consumerList->Close();
    if (FAILED(hr)) { _error="closing initial consumer list hr="+std::to_string(hr); return false; }
    hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&_fence));
    if (!_domain->Created("completion-fence",hr,_fence,true)) return fail();
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width=D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;desc.Height=1;desc.DepthOrArraySize=1;
    desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (auto& readback:_cutReadback) {
        hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback));
        if (!_domain->Created("scene-cut-readback",hr,readback,false)) return fail();
    }
    _flow=std::make_unique<OpticalFlowDx12>(); _trust=std::make_unique<TrustMaskDx12>();
    if (prepareGuides && !EnsureGuides()) return false;
    _event=CreateEvent(nullptr,FALSE,FALSE,nullptr);
    if (!_event) { _error="creating completion event error="+std::to_string(GetLastError()); return false; }
    _initialized=true; return true;
}

HRESULT NativeProducer::SignalNext(ID3D12CommandQueue* queue)
{
    HRESULT hr=S_OK;
    if (!_domain->Queue(queue,hr) || !_domain->Object(_fence,true,hr)) return hr;
    if (_signalled>=UINT64_MAX-1) return E_INVALIDARG;
    if (_fence->GetCompletedValue()==UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
    hr=queue->Signal(_fence,_signalled+1);
    if (SUCCEEDED(hr)) ++_signalled;
    return hr;
}

HRESULT NativeProducer::ExecuteAndSignal(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list, UINT slot) noexcept
{
    try {
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        const auto hr = SignalNext(queue);
        if (SUCCEEDED(hr)) _values[slot] = _signalled;
        return hr;
    } catch (...) { return E_FAIL; } // Execute may already have reached the real queue
}

NativeProducer::FilterResult NativeProducer::FilterSubmitted(const FilterFn& filter, const FrameInput& input,
                                                             ID3D12CommandQueue* queue,
                                                             ID3D12CommandAllocator* anchor)
{
    // Call sites are strictly after successful Signal: real Execute or the
    // actual upload queue Wait. The endpoint cannot construct this capability.
    auto receipt=std::shared_ptr<NativeSubmissionReceipt>(new NativeSubmissionReceipt(
        _device.Get(),_domain->PublicOwner(),anchor ? anchor : _allocators[0],queue,input.picture,_fence,_signalled,
        input.frame,_domain->Generation(),_frame+1));
    try {
        auto result=filter(input,{_fence,_signalled,receipt});
        receipt->active_.store(false,std::memory_order_release);
        return result;
    } catch (...) {
        receipt->active_.store(false,std::memory_order_release);
        throw;
    }
}

bool NativeProducer::SubmitBeforeSrPrefix(ID3D12CommandQueue* queue, UINT slot,
    ID3D12GraphicsCommandList*& list, ID3D12CommandAllocator*& allocator,
    const FrameInput& input, const FilterFn& filter, ID3D12Resource* color, DXGI_FORMAT format,
    D3D12_RESOURCE_STATES state, const std::shared_ptr<void>& resources, Result& result, FrameOutput& output)
{
    const auto fail = [&](const char* step, HRESULT hr = E_FAIL) {
        _quarantined = true; output.uncertain = true;
        result.stoppedAt = step; result.stoppedHr = hr;
        return false;
    };
    HRESULT hr = S_OK;
    if (_quarantined || slot >= kRing || !_held[slot] || !filter || !resources || !color ||
        list != _list || allocator != _allocators[slot] ||
        !NativeWorkDevice::Owns(_device.Get(), color, hr))
        return fail("before-SR prefix owner", FAILED(hr) ? hr : E_INVALIDARG);
    const auto desc = color->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != input.width ||
        desc.Height != input.height || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1)
        return fail("before-SR prefix extent", E_INVALIDARG);

    // This owns the provider's Init recording as well as the prepared color,
    // output, neutral depth and descriptors. A later prefix/filter tail is NOT
    // sufficient to retire the graph: only Run's final tail can do that.
    _held[slot]->beforeSrResources = resources;
    _beforeSrOwned = true;
    hr = list->Close();
    if (FAILED(hr)) return fail("before-SR prefix close", hr);
    if (input.ready.fence && FAILED(hr = queue->Wait(input.ready.fence, input.ready.value)))
        return fail("before-SR prefix input wait", hr);
    if (!_domain->Commands(queue, list, allocator, _fence, hr))
        return fail("before-SR prefix command domain", hr);
    if (FAILED(hr = ExecuteAndSignal(queue, list, slot))) return fail("before-SR prefix execute/signal", hr);

    auto prepared = input;
    prepared.picture = color; prepared.pictureFormat = format; prepared.pictureState = state;
    const auto filtered = FilterSubmitted(filter, prepared, queue, allocator);
    output.filtered |= filtered.filtered;
    if (filtered.uncertain) return fail("before-SR filter uncertain");

    // The ring slot was retired before the original producer Reset. Reuse its
    // separate consumer allocator, never Reset the submitted prefix allocator.
    hr = _domain->Reset(_consumerList, _consumerAllocators[slot]);
    if (FAILED(hr)) return fail("before-SR continuation reset", hr);
    list = _consumerList;
    allocator = _consumerAllocators[slot];
    return true;
}

void NativeProducer::Reset()
{
    _continuityResetPending = true;
    _consumerResetPending = false;
    _guideSceneCutPending = false;
    _scenePolicy.Invalidate();
    ResetGuides();
}

void NativeProducer::ResetGuides()
{
    // Saturation invalidates FG recording; never wrap a history identity.
    if (_guideEpoch != UINT64_MAX) ++_guideEpoch;
    if (_flow)
        _flow->Reset();
    if (_trust)
        _trust->Reset();
}

NativeProducer::Result NativeProducer::Run(ID3D12CommandQueue* queue, const FrameInput& input, const Options& options,
                                           const ApplyFn& apply, FrameOutput& output,
                                           NativeProducerCpuTiming::Frame* timing)
{
    HeldAccessScope heldAccess(_heldAccessActive);
    struct MaskExit {
        NativeProducer& producer;
        ~MaskExit() {
            if(!producer._quarantined)return;
            for(UINT slot=0;slot<kRing;++slot)if(producer._held[slot])
                DrawUiMask::Observe(producer._held[slot]->input.uiMask,DrawUiMask::Consumer::Nr,DrawUiMask::Lifecycle::Quarantined,
                    reinterpret_cast<uintptr_t>(&producer),producer._held[slot]->inputEpoch,0);
        }
    } maskExit{*this};
    Result result;
    result.depthObservation.source = input.depthCount > 0 ? NrInputObservation::SourceDepth::Supplied
                                                        : options.sourceDepth;
    result.depthObservation.acquiredCount = input.depthCount;
    result.depthObservation.paired = input.depthValidityPaired;
    output = FrameOutput {};
    _scenePolicy.Begin(options.sceneCutPolicy ? options.sceneCutMode : DlssNrSceneCut::Mode::Off,
                       input.cutHint || options.explicitConsumerReset);
    if (options.explicitConsumerReset) _continuityResetPending = true;

    if (_quarantined) { output.uncertain=true; result.stoppedAt = "quarantined after incomplete GPU work"; return result; }
    if (!Ready() || queue == nullptr || input.picture == nullptr || _list == nullptr || _consumerList == nullptr)
    {
        result.stoppedAt = !Ready()                   ? "not ready"
                           : queue == nullptr         ? "no queue"
                           : input.picture == nullptr ? "no picture"
                                                      : "no command lists";
        return result;
    }

    // Revalidate actual frame inputs even when the driver reuses this producer.
    // Reject before Reset, Wait, Execute or Signal; no mismatched ready token is
    // translated or replaced with a fabricated completion fence.
    NativeWorkDevice work;
    if (!work.Resolve(input.picture, queue))
    { result.stoppedAt = work.failure; result.stoppedHr = work.hr; return result; }
    HRESULT domainHr = S_OK;
    if (work.device.Get() != _device.Get())
    { result.stoppedAt = "native producer work device mismatch"; return result; }
    if (!_domain->Object(_fence, true, domainHr))
    { result.stoppedAt = "native producer fence device mismatch"; result.stoppedHr = domainHr; return result; }
    if (input.ready.fence && !NativeWorkDevice::Owns(_domain->PublicOwner(), input.ready.fence, domainHr))
    { result.stoppedAt = "native input fence device mismatch"; result.stoppedHr = domainHr; return result; }
    if (input.ready.fence && (input.ready.value==UINT64_MAX || input.ready.fence->GetCompletedValue()==UINT64_MAX))
    { result.stoppedAt="native input completion unknown"; result.stoppedHr=E_INVALIDARG; return result; }
    if (input.depthCount < 0 || input.depthCount > FrameInput::kMaxDepthCopies)
    { result.stoppedAt = "native depth count"; return result; }
    for (int i = 0; i < input.depthCount; ++i)
        if (!NativeWorkDevice::Owns(_device.Get(), input.depth[i], domainHr))
        { result.stoppedAt = "native depth device mismatch"; result.stoppedHr = domainHr; return result; }
    // Switching queues on the same device still needs a completion witness.
    // Retain the old queue until its last signalled work has actually completed.
    if (_queue.Get() != queue)
    {
        if (!CanRetire()) { result.stoppedAt = "native previous queue incomplete"; return result; }
        if (!_domain->RebindQueue(work)) { result.stoppedAt="native queue rebind"; return result; }
        _queue = queue;
    }

    if (!_domain->Queue(queue,domainHr))
    { result.stoppedAt="native bound queue domain"; result.stoppedHr=domainHr; return result; }
    if (_signalled >= UINT64_MAX-4) { result.stoppedAt="native fence value exhausted"; return result; }
    if (_heldInputEpoch>=UINT64_MAX-1) { result.stoppedAt="held input epoch exhausted"; return result; }
    const UINT holdSlot=static_cast<UINT>(_frame%kRing);
    // Existing guide path waits at allocator reuse. Color-only reuse is polled,
    // never given a new CPU wait. Keep every raw input and optional lease alive.
    if (options.colorOnly && _held[holdSlot]) {
        const auto completed=_fence->GetCompletedValue();
        if (completed==UINT64_MAX) {
            _quarantined=true;output.uncertain=true;result.stoppedAt="color completion unknown";return result;
        }
        if (completed<_values[holdSlot]) { result.stoppedAt="color retention slot pending";return result; }
        DrawUiMask::Observe(_held[holdSlot]->input.uiMask,DrawUiMask::Consumer::Nr,DrawUiMask::Lifecycle::Completed,
            reinterpret_cast<uintptr_t>(this),_held[holdSlot]->inputEpoch,_values[holdSlot]);
    }
    if (options.colorOnly)
    {
        // Acquire already submitted the real picture upload. Publish a queue
        // receipt after its GPU Wait, then filter/identity, then final completion.
        // This path needs neither optical-flow warmup nor NR/depth authority.
        _held[holdSlot]=std::make_unique<HeldFrame>(input,++_heldInputEpoch);
        if (input.ready.fence && FAILED(queue->Wait(input.ready.fence, input.ready.value)))
        { _quarantined = true; output.uncertain = true; result.stoppedAt = "color input wait"; return result; }
        if (FAILED(result.stoppedHr=SignalNext(queue)))
        { _quarantined = true; output.uncertain = true; result.stoppedAt = "color producer signal"; return result; }
        try {
            const auto run = [&](const FilterFn& filter) {
                if (!filter) return true;
                const auto filtered = FilterSubmitted(filter,input,queue);
                output.filtered |= filtered.filtered;
                output.uncertain |= filtered.uncertain;
                return !filtered.uncertain;
            };
            if (!run(options.beforeNr) || !run(options.finalOriginal))
            { _quarantined = true; output.uncertain = true; result.stoppedAt = "color filter uncertain"; return result; }
        } catch (...) {
            _quarantined = true; output.uncertain = true; result.stoppedAt = "color filter exception"; return result;
        }
        if (FAILED(result.stoppedHr=SignalNext(queue)))
        { _quarantined = true; output.uncertain = true; result.stoppedAt = "color completion signal"; return result; }
        _values[holdSlot]=_signalled; ++_frame;
        output.done = {_fence, _signalled}; result.submitted = true;
        SealHeldFinalSubmission(holdSlot);
        return result;
    }

    if (!EnsureGuides()) { result.stoppedAt = "guide initialization"; return result; }

    // The previous work may still be reading the textures a new size replaces.
    if (input.width != _width || input.height != _height)
    {
        if (!WaitFor(_signalled)) { _quarantined = true; output.uncertain = true; result.stoppedAt = "resize completion"; return result; }
        _width = input.width;
        _height = input.height;
        Reset();
    }

    // The adapter knows this frame does not continue the last one (a loading screen, a resolution change).
    if (input.cutHint)
        Reset();

    const UINT slot = (UINT) (_frame % kRing);
    bool allocatorReady = false;
    {
        auto waitTiming = NativeProducerCpuTiming::Measure(timing, NativeProducerCpuTiming::Stage::AllocatorWait);
        allocatorReady = WaitFor(_values[slot]);
    }
    if (!allocatorReady) { _quarantined = true; output.uncertain = true; result.stoppedAt = "allocator completion"; return result; }
    if(_prepGpu)_prepGpu->Poll(slot,queue,_fence,result.completedPrepGpu);

    if(_held[slot] && _values[slot])
        DrawUiMask::Observe(_held[slot]->input.uiMask,DrawUiMask::Consumer::Nr,DrawUiMask::Lifecycle::Completed,
            reinterpret_cast<uintptr_t>(this),_held[slot]->inputEpoch,_values[slot]);

    // The flow's own cut flag of the frames the GPU has finished since (usually the last one): a cut found there resets
    // the consumer's history now, a couple of frames before the trust mask's count could say so. Until the consumer has
    // run with the reset it stays asked for.
    for (UINT s = 0; s < kRing; ++s)
    {
        const auto completed = _fence->GetCompletedValue();
        if (completed == UINT64_MAX) { _quarantined = true; output.uncertain = true; result.stoppedAt = "scene-cut device removed"; return result; }
        if (!_cutPending[s] || completed < _values[s])
            continue;

        _cutPending[s] = false;
        const D3D12_RANGE range { 0, sizeof(uint32_t) };
        uint32_t* data = nullptr;

        if (SUCCEEDED(_cutReadback[s]->Map(0, &range, (void**) &data)) && data != nullptr)
        {
            const bool cut = *data != 0;
            const D3D12_RANGE none { 0, 0 };
            _cutReadback[s]->Unmap(0, &none);

            if (cut && _frame >= _cutQuietUntil)
            {
                result.sceneCut = true;
                result.distrustedShare = 1.0f;
                if (options.sceneCutPolicy) _scenePolicy.Observe(_cutStamp[s]);
                else _consumerResetPending = true;
                _cutQuietUntil = _frame + kRing + 2;
            }
        }
        else ++_cutReadbackFailures;
    }

    _held[slot]=std::make_unique<HeldFrame>(input,++_heldInputEpoch);
    const HRESULT listHr = _domain->Reset(_list,_allocators[slot]);

    if (FAILED(listHr))
    {
        result.stoppedAt = "producer allocator/list reset domain";
        result.stoppedHr = (long) listHr;
        result.deviceRemoved = _device != nullptr ? (long) _device->GetDeviceRemovedReason() : 0;
        return result;
    }

    ID3D12GraphicsCommandList* list = _list;
    ID3D12CommandAllocator* activeAllocator = _allocators[slot];
    const bool prepTiming=(_frame&7)==0 && _prepGpu && _prepGpu->Init(_device.Get(),queue,*_domain) &&
        _prepGpu->Begin(slot,queue,{input.frame,input.depthEpoch,input.depthGeneration,input.depthSourceFrame,
            input.depthPictureSerial,reinterpret_cast<uintptr_t>(input.picture)});
    bool beforeSrSplit = false;
    struct BeforeSrExit {
        NativeProducer& owner; bool& split; Result& result; FrameOutput& output;
        ~BeforeSrExit() {
            // Also covers exceptions in a later Close/Wait/domain callback.
            // The prefix/filter tail alone can never retire this frame graph.
            if (split && !result.submitted) { owner._quarantined = true; output.uncertain = true; }
        }
    } beforeSrExit{*this, beforeSrSplit, result, output};

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = input.picture;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = input.pictureState;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const bool needsBarrier = input.pictureState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    if (needsBarrier)
        list->ResourceBarrier(1, &barrier);

    // The flow's match sees where surfaces end through the depth, when it is one copy of the whole scene (several
    // copies each hold part of it, and one alone would show edges that are not there).
    // Depth matching, trust and GuideDepth all sample picture UVs. The mapped
    // RGBA/validity route supplies that transform before this seam; a raw SR
    // producer plane does not. Retain its owners but do not sample it as picture Z.
    const bool pictureDepth = input.depthCoordinates == DepthCoordinates::Picture;
    const bool flowDepth = pictureDepth && !input.depthValidityPaired && input.depthCount == 1 &&
                           input.depth[0] != nullptr && input.depthView != DXGI_FORMAT_UNKNOWN;
    const OpticalFlowDx12::Encoding encoding = input.colorSpace == ColorSpace::ScRgb ? OpticalFlowDx12::Encoding::ScRgb
                                               : input.colorSpace == ColorSpace::Pq  ? OpticalFlowDx12::Encoding::Pq
                                                                                     : OpticalFlowDx12::Encoding::Srgb;
    if(prepTiming)_prepGpu->Stamp(slot,0,list);
    const bool recorded =
        _flow->Dispatch(list, input.picture, input.pictureFormat, flowDepth ? input.depth[0] : nullptr,
                        flowDepth ? input.depthView : DXGI_FORMAT_UNKNOWN, input.depthReversed, encoding);
    if(prepTiming)_prepGpu->Stamp(slot,1,list);

    result.detectorRan = recorded;
    result.detectorFailed = !recorded;
    // This frame's cut flag out to the readback, read once the GPU is done with the frame.
    bool cutCopied = false;

    if (ID3D12Resource* flag = recorded ? _flow->SceneCutFlag() : nullptr)
    {
        D3D12_RESOURCE_BARRIER toCopy {};
        toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy.Transition.pResource = flag;
        toCopy.Transition.Subresource = 0;
        toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &toCopy);

        D3D12_TEXTURE_COPY_LOCATION dst {};
        dst.pResource = _cutReadback[slot];
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT };
        D3D12_TEXTURE_COPY_LOCATION src {};
        src.pResource = flag;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        const D3D12_BOX texel0 { 0, 0, 0, 1, 1, 1 };
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, &texel0);

        std::swap(toCopy.Transition.StateBefore, toCopy.Transition.StateAfter);
        list->ResourceBarrier(1, &toCopy);
        cutCopied = true;
    }

    TrustMaskDx12::Inputs nativeInputs;
    result.depthObservation.guide = NrInputObservation::GuideDepth::FlowUnavailable;
    bool nativeReady = false;
    bool nativeReset = false;

    if (recorded && _flow->FlowValid())
    {
        result.flowValid = true;

        if (options.flowPreview)
            _previewReady = _flow->Visualise(list, options.previewMaxSpeed) || _previewReady;

        // The trust mask uses the scene's depth as the adapter copied it this frame; without any it runs on flow and luma.
        TrustMaskDx12::Inputs in;
        in.flow = _flow->Flow();
        in.flowWidth = _flow->FlowWidth();
        in.flowHeight = _flow->FlowHeight();
        in.fullPerFlow = (float) input.width / (float) _flow->FlowWidth();
        in.lumaNow = _flow->LumaOfLastFrame();
        in.lumaBefore = _flow->LumaOfFrameBefore();
        in.sceneCut = _flow->SceneCutFlag();

        result.depthObservation.guide = input.depthCount <= 0 ? NrInputObservation::GuideDepth::NoSourceDepth
            : !pictureDepth ? NrInputObservation::GuideDepth::UnmappedProducerCoordinates
            : input.depthView == DXGI_FORMAT_UNKNOWN ? NrInputObservation::GuideDepth::UnknownDepthView
            : input.depthValidityPaired && !_trust->SupportsPairedDepth()
                ? NrInputObservation::GuideDepth::PairedPipelineUnavailable
                : NrInputObservation::GuideDepth::GuidesNotRequested;

        if (pictureDepth && input.depthCount > 0 && input.depthView != DXGI_FORMAT_UNKNOWN &&
            (!input.depthValidityPaired || _trust->SupportsPairedDepth()))
        {
            in.depthCount = (std::min)(input.depthCount, (int) TrustMaskDx12::Inputs::kMaxDepths);

            for (int i = 0; i < in.depthCount; ++i)
                in.depths[i] = input.depth[i];

            in.depthFormat = input.depthView;
            in.depthWidth = input.depthWidth;
            in.depthHeight = input.depthHeight;
            in.depthReversed = input.depthReversed;
            in.depthValidityPaired = input.depthValidityPaired;
        }

        result.trustRan = _trust->Dispatch(list, in);
        if (!result.trustRan) result.depthObservation.guide = NrInputObservation::GuideDepth::TrustDispatchFailed;
        nativeInputs = in;
        nativeReady = result.trustRan;

        if (options.sceneCutPolicy && result.trustRan)
            _scenePolicy.TrustDispatched(_trust->SceneCutSeen(), _frame >= _cutQuietUntil);

        // A hard cut seen by the trust mask's count, a few frames after it: the consumer's history is reset, unless the
        // flow's own flag already asked for that for this cut. With the flow's detector on, the flow and the mask carry
        // on (the cut frame had no motion, and the frames since match the new scene) and the consumer runs on this
        // frame too: skipping it showed one frame without it, a flash over the whole picture. Without the detector, as
        // before: the flow and the mask start over.
        if (_trust->SceneCutSeen() && _frame >= _cutQuietUntil)
        {
            result.sceneCut = true;
            result.distrustedShare = _trust->DistrustedShare();
            nativeReset = !options.sceneCutPolicy;

            if (_flow->Tuning().sceneCutDetector)
                _cutQuietUntil = _frame + kRing + 2;
            else
            {
                // The consumer is skipped this frame (and the next, which has no flow yet): the reset waits for it.
                ResetGuides();
                nativeReady = false;
                if (!options.sceneCutPolicy)
                    _consumerResetPending = true;
            }
        }
    }

    if(prepTiming)_prepGpu->Stamp(slot,2,list);
    if (result.sceneCut)
    {
        if (_guideCutSerial != UINT64_MAX) ++_guideCutSerial;
        _guideSceneCutPending = true;
    }
    bool sceneCutResetAccepted = false;
    NativeFrame consumerFrame;
    bool consumerReady = false;
    if (nativeReady && options.apply && apply)
    {
        if (_trust->BuildGuides(list, nativeInputs, input.width, input.height))
        {
            consumerFrame.color = input.picture;
            consumerFrame.colorFormat = input.pictureFormat;
            consumerFrame.pictureState = input.pictureState;
            consumerFrame.space = input.colorSpace;
            consumerFrame.depth = _trust->GuideDepth();
            consumerFrame.motion = _trust->GuideMotion();
            consumerFrame.historyDistrust = _trust->Mask();
            consumerFrame.depthReversed = nativeInputs.depthReversed;
            if (consumerFrame.depth) result.depthObservation.guide = NrInputObservation::GuideDepth::Supplied;
            else if (nativeInputs.depthCount > 0)
                result.depthObservation.guide = NrInputObservation::GuideDepth::GuideDepthMissing;
            consumerFrame.depthObservation = result.depthObservation;
            // BuildGuides samples depth at (pixel+0.5)/pictureSize and enlarges
            // this exact finished-picture flow without sign/row/jitter changes.
            // OpticalFlow reports prev-current in picture pixels; this is not
            // a declaration about raw scene MV or the game camera's jitter.
            consumerFrame.pictureFlowGuides_ = result.flowValid && result.trustRan &&
                nativeInputs.flow && nativeInputs.lumaNow && nativeInputs.lumaBefore &&
                consumerFrame.motion && consumerFrame.historyDistrust;
            consumerFrame.pictureTemporalGuides_ = pictureDepth && nativeInputs.depthCount == 1 &&
                nativeInputs.depths[0] == input.depth[0] && input.depth[0] &&
                consumerFrame.depth && consumerFrame.motion && consumerFrame.historyDistrust;
            consumerFrame.guideSceneCut_ = _guideSceneCutPending;
            consumerFrame.continuityReset = _continuityResetPending;
            consumerFrame.guideEpoch = _guideEpoch; consumerFrame.guideCutSerial = _guideCutSerial;
            consumerFrame.sceneCutReset = options.sceneCutPolicy && _scenePolicy.ResetRequested();
            consumerFrame.sceneCutResetAccepted = &sceneCutResetAccepted;
            consumerFrame.reset = _continuityResetPending || (options.sceneCutPolicy ? consumerFrame.sceneCutReset
                                                                                    : nativeReset || _consumerResetPending);
            consumerReady = true;
        }
        else result.depthObservation.guide = NrInputObservation::GuideDepth::GuideBuildFailed;
    }

    if(prepTiming){_prepGpu->Stamp(slot,3,list);_prepGpu->Resolve(slot,list);}
    // HoYoShade owns a separate ReShade command list and submits it from the
    // consumer callback. Keep the producer's read state alive until that list
    // and the actual NR consumer have both run. Other consumers retain the
    // historical single-list contract and see the adapter's original state.
    const bool splitProducerAndConsumer = options.submitProducerBeforeConsumer;
    const bool filterSplit = static_cast<bool>(options.beforeNr);
    if (options.beforeSr && !splitProducerAndConsumer && consumerReady)
    {
        consumerFrame.beforeSr_ = [&](ID3D12GraphicsCommandList*& recording, ID3D12Resource* color,
            DXGI_FORMAT format, D3D12_RESOURCE_STATES state, const std::shared_ptr<void>& resources) -> NativeFrame::BeforeSrResult {
            if (beforeSrSplit || recording != list) {
                _quarantined = true; output.uncertain = true;
                result.stoppedAt = "before-SR repeated or foreign recording";
                return {};
            }
            beforeSrSplit = true; // one owner transition per synchronous Run
            if (!SubmitBeforeSrPrefix(queue, slot, list, activeAllocator, input, options.beforeSr,
                color, format, state, resources, result, output)) return {};
            recording = list;
            return {true, output.filtered};
        };
    }
    if (splitProducerAndConsumer)
        consumerFrame.pictureState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // Legacy consumers run on the producer list.  The flow/trust recording has
    // left the picture in NON_PIXEL_SHADER_RESOURCE; restore the caller's
    // state before invoking the consumer so its state contract remains the
    // same as the pre-split path.  The list must still be open here.
    if (!splitProducerAndConsumer)
    {
        if (needsBarrier)
        {
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list->ResourceBarrier(1, &barrier);
        }
        try
        {
            if (consumerReady) result.nativeRan = apply(list, consumerFrame);
        }
        catch (...)
        {
            _quarantined = true; output.uncertain = true;
            result.stoppedAt = "consumer callback";
            return result;
        }
    }

    // A failed owner split may already have executed the original list. Do
    // not close/submit it again or manufacture a final-completion receipt.
    if (_quarantined) { output.uncertain = true; return result; }
    const HRESULT closeHr = list->Close();

    if (SUCCEEDED(closeHr))
    {
        if (!beforeSrSplit && input.ready.fence != nullptr)
        {
            const HRESULT waitHr = queue->Wait(input.ready.fence, input.ready.value);
            if (FAILED(waitHr)) { _quarantined = true; output.uncertain = true; result.stoppedAt = "input queue wait"; result.stoppedHr = waitHr; return result; }
        }

        if (!_domain->Commands(queue,list,activeAllocator,_fence,domainHr)) {
            _quarantined=true;output.uncertain=true;result.stoppedAt="producer command domain";result.stoppedHr=domainHr;return result;
        }
        const HRESULT producerSignalHr = ExecuteAndSignal(queue, list, slot);
        if (FAILED(producerSignalHr))
        {
            _quarantined = true; output.uncertain = true;
            result.stoppedAt = "producer signal";
            result.stoppedHr = producerSignalHr;
            return result;
        }
        if (filterSplit)
        {
            auto color = input;
            color.pictureState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            try {
                const auto filtered = FilterSubmitted(options.beforeNr,color,queue);
                output.filtered |= filtered.filtered;
                if (filtered.uncertain) {
                    output.uncertain = true; _quarantined = true; output.uncertain = true;
                    result.stoppedAt = "before-NR filter uncertain"; return result;
                }
            } catch (...) {
                output.uncertain = true; _quarantined = true; output.uncertain = true;
                result.stoppedAt = "before-NR filter exception"; return result;
            }
        }

        // Only the HoYo path needs a second list. Its callback may submit an
        // owned SDK immediate list between these two native submissions.
        if (splitProducerAndConsumer)
        {
            const HRESULT consumerResetHr = _domain->Reset(_consumerList,_consumerAllocators[slot]);
            if (FAILED(consumerResetHr))
            {
                _quarantined = true; output.uncertain = true;
                result.stoppedAt = "consumer allocator/list reset domain";
                result.stoppedHr = consumerResetHr;
                result.deviceRemoved = _device != nullptr ? (long) _device->GetDeviceRemovedReason() : 0;
                return result;
            }

            try
            {
                if (consumerReady) result.nativeRan = apply(_consumerList, consumerFrame);
            }
            catch (...)
            {
                _quarantined = true; output.uncertain = true;
                result.stoppedAt = "consumer callback";
                return result;
            }

            // ApplyNativeInput and the owned HoYo list both leave the picture
            // in the explicit read state supplied to the consumer. Restore the
            // adapter's exact entry state on the native consumer list before
            // the final fence is signalled.
            if (needsBarrier)
            {
                D3D12_RESOURCE_BARRIER restore = barrier;
                restore.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                restore.Transition.StateAfter = input.pictureState;
                _consumerList->ResourceBarrier(1, &restore);
            }

            const HRESULT consumerCloseHr = _consumerList->Close();
            if (FAILED(consumerCloseHr))
            {
                _quarantined = true; output.uncertain = true;
                result.stoppedAt = "consumer close";
                result.stoppedHr = consumerCloseHr;
                result.deviceRemoved = _device != nullptr ? (long) _device->GetDeviceRemovedReason() : 0;
                return result;
            }

            if (!_domain->Commands(queue,_consumerList,_consumerAllocators[slot],_fence,domainHr)) {
                _quarantined=true;output.uncertain=true;result.stoppedAt="consumer command domain";result.stoppedHr=domainHr;return result;
            }
            const HRESULT consumerSignalHr = ExecuteAndSignal(queue, _consumerList, slot);
            if (FAILED(consumerSignalHr))
            {
                _quarantined = true; output.uncertain = true;
                result.stoppedAt = "consumer signal";
                result.stoppedHr = consumerSignalHr;
                return result;
            }
        }
        if (options.finalOriginal)
        {
            try {
                // The NR consumer list is genuinely executed and signalled.
                // Final original runs before Return and its real copyback.
                const auto filtered = FilterSubmitted(options.finalOriginal,input,queue);
                output.filtered |= filtered.filtered;
                if (filtered.uncertain) {
                    output.uncertain = true; _quarantined = true; output.uncertain = true;
                    result.stoppedAt = "final-original filter uncertain"; return result;
                }
            } catch (...) {
                output.uncertain = true; _quarantined = true; output.uncertain = true;
                result.stoppedAt = "final-original filter exception"; return result;
            }
            if (FAILED(result.stoppedHr=SignalNext(queue))) {
                output.uncertain = true; _quarantined = true; output.uncertain = true;
                result.stoppedAt = "final-original completion signal"; return result;
            }
            _values[slot] = _signalled;
        }
        _cutPending[slot] = cutCopied;
        _cutStamp[slot] = _scenePolicy.Current();
        if (options.sceneCutPolicy) _scenePolicy.Accepted(sceneCutResetAccepted);
        if (result.nativeRan) { _consumerResetPending = false; _continuityResetPending = false; _guideSceneCutPending = false; }
        ++_frame;

        output.done = SyncPoint { _fence, _signalled };
        output.processed = result.nativeRan;
        result.submitted = true;
        SealHeldFinalSubmission(slot);
    }

    if (!result.submitted && result.stoppedAt == nullptr)
    {
        result.stoppedAt = "close";
        result.stoppedHr = (long) closeHr;
        result.deviceRemoved = _device != nullptr ? (long) _device->GetDeviceRemovedReason() : 0;

        // Flow/trust CPU readback metadata advanced while this list did not
        // execute. Do not later consume an unsubmitted counter as a fresh cut.
        _quarantined = true; output.uncertain = true;
        // Quarantine retains the exact original list/allocator creation receipts.
        // Never replace/release a list while any submission lacks completion.
    }

    return result;
}

} // namespace native
