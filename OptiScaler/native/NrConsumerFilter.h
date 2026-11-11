#pragma once
#include "NrConsumerFilterPolicy.h"
#include "NrConsumerDepth.h"
#include <integration/HoYoShade.h>
#include <integration/HoYoShadeNaturalAssociation.h>
#include <wrl/client.h>
#include <memory>

class Dx11wDx12SC;
namespace native::nrfilter
{
// Construct ONLY in Dx11wDx12SC::Present after its real upload/copy and
// _WaitForInteropCopyOnPresentQueue succeed, around the synchronous Late call.
// No global currentReal/currentWrapped inference, no retained buffer, no expired
// Source/Binding handoff. Capture below creates a new proof on the actual caller.
class BridgeScope final
{
    friend class ::Dx11wDx12SC;
    inline static thread_local BridgeScope* current_ = nullptr;
    BridgeScope* previous_ = nullptr;
    IDXGISwapChain* original_ = nullptr;
    ID3D11Device* device_ = nullptr;
    UINT index_ = 0;
    ID3D12CommandQueue* copyQueue_ = nullptr;
    ID3D12Fence* copyFence_ = nullptr;
    uint64_t copyValue_ = 0;
    Key key_;
    BridgeScope(IDXGISwapChain* original, ID3D11Device* device,
        UINT index, IDXGISwapChain* child, ID3D12CommandQueue* producer,
        ID3D12CommandQueue* consumer, uint64_t epoch, ID3D12CommandQueue* copyQueue,
        ID3D12Fence* copyFence, uint64_t copyValue) noexcept
        : original_(original), device_(device), index_(index),
          copyQueue_(copyQueue), copyFence_(copyFence), copyValue_(copyValue),
          key_{reinterpret_cast<uintptr_t>(child), reinterpret_cast<uintptr_t>(producer),
              reinterpret_cast<uintptr_t>(consumer), epoch, GetCurrentThreadId()}
    {
        previous_ = current_;
        current_ = this;
    }
    ~BridgeScope() { current_ = previous_; }
    BridgeScope(const BridgeScope&) = delete;
    BridgeScope& operator=(const BridgeScope&) = delete;

public:
    static std::shared_ptr<const HoYoShade::NaturalAssociation::Source> Capture(
        IDXGISwapChain* child, ID3D12Resource* color, ID3D12CommandQueue* producer,
        ID3D12CommandQueue* consumer, uint64_t epoch, HoYoShade::ProducerReady& submitted) noexcept
    {
        auto* active = current_;
        const Key caller{reinterpret_cast<uintptr_t>(child), reinterpret_cast<uintptr_t>(producer),
            reinterpret_cast<uintptr_t>(consumer), epoch, GetCurrentThreadId()};
        if (!active || !Matches(active->key_, caller) || !active->original_ || !active->device_ ||
            !active->copyQueue_ || !active->copyFence_ ||
            !ReadyValueValid(active->copyValue_, active->copyFence_->GetCompletedValue())) return {};
        Microsoft::WRL::ComPtr<IDXGISwapChain3> indexed;
        Microsoft::WRL::ComPtr<ID3D12Resource> currentColor;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> buffer;
        if (FAILED(child->QueryInterface(IID_PPV_ARGS(&indexed))) ||
            FAILED(child->GetBuffer(indexed->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&currentColor))) ||
            currentColor.Get() != color) return {};
        // Original index comes from this wrapper's actual source-copy recipe,
        // including nonflip/private-ring selection, not a guessed index zero.
        if (FAILED(active->original_->GetBuffer(active->index_, IID_PPV_ARGS(&buffer)))) return {};
        auto proof = HoYoShade::NaturalAssociation::Source::Capture(active->original_, active->device_,
            buffer.Get(), active->index_, reinterpret_cast<uintptr_t>(child), 1, epoch);
        // This is the actual successful copy Execute/Signal followed by the
        // wrapper's successful present-queue Wait, not the guide producer queue.
        submitted = {active->copyQueue_, active->copyFence_, active->copyValue_};
        return proof;
    }
};

// Only the actual generic wrapper may expose this synchronous optical-input
// parent. FrameInput scalars are equality checks, never cast back to objects.
// No global currentReal guess, no buffer/Binding retained from an earlier call.
class OpticalInputScope final
{
    friend class ::Dx11wDx12SC;
    inline static thread_local OpticalInputScope* current_ = nullptr;
    OpticalInputScope* previous_ = nullptr;
    IDXGISwapChain* parent_;
    ID3D11Device* device_;
    DWORD thread_ = GetCurrentThreadId();
    OpticalInputScope(IDXGISwapChain* parent, ID3D11Device* device) noexcept
        : parent_(parent), device_(device) { previous_ = current_; current_ = this; }
    ~OpticalInputScope() { current_ = previous_; }
    OpticalInputScope(const OpticalInputScope&) = delete;
    OpticalInputScope& operator=(const OpticalInputScope&) = delete;
public:
    static std::shared_ptr<const HoYoShade::NaturalAssociation::Source> Capture(
        uint64_t authenticDevice, uint64_t authenticBuffer, uint64_t stream, uint64_t frame) noexcept
    {
        auto* active = current_;
        if (!active || active->thread_ != GetCurrentThreadId() || !active->parent_ || !active->device_ ||
            reinterpret_cast<uintptr_t>(active->device_) != authenticDevice || !authenticBuffer ||
            !stream || !frame || frame == UINT64_MAX) return {};
        // Exact Dx11FrameSource::Acquire recipe: current IDXGISwapChain3 index,
        // otherwise zero. This is independent of the wrapper's fake copy index.
        UINT index = 0;
        Microsoft::WRL::ComPtr<IDXGISwapChain3> indexed;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> buffer;
        if (SUCCEEDED(active->parent_->QueryInterface(IID_PPV_ARGS(&indexed))))
            index = indexed->GetCurrentBackBufferIndex();
        if (FAILED(active->parent_->GetBuffer(index, IID_PPV_ARGS(&buffer))) ||
            reinterpret_cast<uintptr_t>(buffer.Get()) != authenticBuffer) return {};
        return HoYoShade::NaturalAssociation::Source::Capture(active->parent_, active->device_,
            buffer.Get(), index, stream, 1, frame);
    }
};

// One instance per Late slot, serialized by its existing g_nrMutex. Holds no
// backbuffer or natural Source across calls/resizes. The core owns submitted or
// uncertain color/reader batches; quarantine remains independent of selection.
class PresentTransaction final
{
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    uint64_t value_ = 0;
    depth::Owner depth_;
public:
    // Polled by Late's existing slot reuse, Cancel and cleanup paths. Source
    // copy and snapshot reader have different, independently real completions.
    // Ordinary collection keeps idle GPU entries cached. Cleanup retires the
    // domain (including same-size resize) and still tests actual active readers.
    bool DepthSourceFinished() const noexcept { return depth_.SourceFinished(); }
    bool DepthSafeToRelease() noexcept { return depth_.SafeToRelease(); }
    void CollectDepth() noexcept { depth_.Collect(); }
    bool BeforeNr(IDXGISwapChain* chain, ID3D12CommandQueue* queue,
        ID3D12Resource* color, bool sdr, uint64_t epoch, bool bridge,
        ID3D12CommandQueue* producer, const depth::Capture& capturedDepth) noexcept
    {
        return BeforeRead(
            [&] { return depth_.SourceHealthy() && HoYoShade::ColorConsumers(chain) == HoYoShade::ConsumerAccess::Safe; },
            [] { return HoYoShade::Selected(HoYoShade::Stage::BeforeNr); },
            [&] { return SelectedBeforeNr(chain, queue, color, sdr, epoch, bridge, producer, capturedDepth); });
    }
private:
    bool SelectedBeforeNr(IDXGISwapChain* chain, ID3D12CommandQueue* queue,
        ID3D12Resource* color, bool sdr, uint64_t epoch, bool bridge,
        ID3D12CommandQueue* producer, const depth::Capture& capturedDepth) noexcept
    {
        if (!chain || !queue || !color || !sdr || epoch == UINT64_MAX) return true;
        try
        {
            using Microsoft::WRL::ComPtr;
            ComPtr<ID3D12Device> owner, queueOwner, fenceOwner;
            if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
                FAILED(color->GetDevice(IID_PPV_ARGS(&owner))) ||
                FAILED(queue->GetDevice(IID_PPV_ARGS(&queueOwner))) ||
                owner != queueOwner || FAILED(owner->GetDeviceRemovedReason())) return true;

            HoYoShade::ColorEndpoint endpoint;
            endpoint.stage = HoYoShade::Stage::BeforeNr;
            endpoint.color = color; endpoint.queue = queue;
            endpoint.stream = reinterpret_cast<uintptr_t>(chain); endpoint.generation = 1; endpoint.frame = epoch;
            const auto desc = color->GetDesc();
            endpoint.width = static_cast<uint32_t>(desc.Width); endpoint.height = desc.Height;
            endpoint.state = D3D12_RESOURCE_STATE_PRESENT; endpoint.stateKnown = true; endpoint.sdr = sdr;

            std::shared_ptr<const HoYoShade::NaturalAssociation::Source> proof;
            if (bridge) proof = BridgeScope::Capture(chain, color, producer, queue, epoch, endpoint.ready);
            else
            {
                ComPtr<IDXGISwapChain3> indexed;
                ComPtr<ID3D12Device> chainDevice;
                if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&indexed))) ||
                    FAILED(chain->GetDevice(IID_PPV_ARGS(&chainDevice)))) return true;
                proof = HoYoShade::NaturalAssociation::Source::Capture(chain, chainDevice.Get(), color,
                    indexed->GetCurrentBackBufferIndex(), endpoint.stream, endpoint.generation, epoch);
            }
            HoYoShade::NaturalAssociation::Source::Scope source(std::move(proof));
            endpoint.naturalSource = source.Get();
            if (!endpoint.naturalSource) return true; // untouched refusal, not coverage
            const auto& observation = endpoint.naturalSource->Observation();
            endpoint.naturalDevice = observation.sourceDevice;
            endpoint.naturalBackbuffer = observation.buffer;
            HoYoShade::BindNaturalSource(endpoint.naturalSource);

            if (!bridge)
            {
                if (!fence_ || device_ != owner || queue_.Get() != queue)
                {
                    ComPtr<ID3D12Fence> replacement;
                    if (FAILED(owner->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&replacement)))) return true;
                    device_ = owner; queue_ = queue; fence_ = replacement; value_ = 0;
                }
                if (FAILED(fence_->GetDevice(IID_PPV_ARGS(&fenceOwner))) || fenceOwner != owner ||
                    fence_->GetCompletedValue() == UINT64_MAX || value_ >= UINT64_MAX - 1) return true;

                // API12 Present callback follows submitted color producers;
                // this seam is before Late opens any private list. Signal the
                // actual queue tail, never a still-recording application list.
                if (FAILED(queue->Signal(fence_.Get(), ++value_))) return true;
                endpoint.ready = {queue, fence_.Get(), value_};
            }
            HoYoShade::ColorWriteScope writer(true, owner.Get(), queue, color, chain);
            // true means only that untouched refusal permits the old NR path;
            // it never reports a filter execution or successful color change.
            if (!writer.Acquired()) return HoYoShade::ColorConsumers(chain) == HoYoShade::ConsumerAccess::Safe;
            // Late has already validated this exact capture and queued its ready
            // Wait. Seal on the consumer queue, independently of the color ready
            // witness (which can come from the bridge copy queue). No open game
            // list is submitted and no synthetic NativeSubmission is needed.
            endpoint.depth = depth_.Acquire(capturedDepth, endpoint);
            if (!depth_.SourceHealthy()) {
                writer.FinishAfterReturn(false); // depth-only uncertainty; no color write occurred
                return false; // COPY_SOURCE/restore submission is unknown: do not let NR read the slot
            }
            const auto result = HoYoShade::RenderStage(endpoint);
            depth_.Return(result); // Complete itself is called only by the real owned filter
            if (result.Uncertain()) HoYoShade::QuarantineColor(chain, queue, owner.Get(), color, epoch ? epoch : 1);
            writer.FinishAfterReturn(result.Uncertain());
            return AfterAttempt(result.Uncertain()) == Decision::Continue;
        }
        catch (...) { return true; } // no exceptions escape owned recording (RenderStage is noexcept)
    }
};
}
