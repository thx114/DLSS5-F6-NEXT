#include "pch.h"
#include "HoYoShadeDepth.h"
#include <native/NativeSubmissionReceipt.h>
#include <genshin/GenshinDx12Transport.h>
#include <cstring>
#include <stdexcept>

namespace HoYoShade::Depth
{
namespace
{
template<class A, class B> bool Same(A* a, B* b) noexcept
{
    Microsoft::WRL::ComPtr<IUnknown> x, y;
    return a && b && SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x))) &&
        SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y))) && x.Get() == y.Get();
}
bool FenceValid(ID3D12Fence* fence, uint64_t value) noexcept
{
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    return fence && value && value != UINT64_MAX &&
        SUCCEEDED(fence->GetDevice(IID_PPV_ARGS(&device))) && device &&
        SUCCEEDED(device->GetDeviceRemovedReason()) && fence->GetCompletedValue() != UINT64_MAX;
}
// Source annotations are private runtime parameters, never compile definitions
// or edits to the natural runtime's global preset. bufready_depth is standard.
void Uniforms(reshade::api::effect_runtime* runtime, bool available, const Access* input)
{
    runtime->enumerate_uniform_variables(nullptr, [=](auto* r, auto variable) {
        char source[64]{};
        if (!r->get_annotation_string_from_uniform_variable(variable, "source", source)) return;
        bool value = false;
        if (!std::strcmp(source, "bufready_depth")) value = available;
        else if (!std::strcmp(source, "hoyoshade_depth_reversed")) value = available && input && input->reversed;
        else if (!std::strcmp(source, "hoyoshade_depth_y_flipped")) value = available && input && input->yFlipped;
        else if (!std::strcmp(source, "hoyoshade_depth_validity_paired")) value = available && input && input->validityPaired;
        else return;
        r->set_uniform_value_bool(variable, &value, 1);
    });
}
// Retains the entire content owner, not a standalone ComPtr. An uncertain read
// intentionally retains this bounded admission for process lifetime. There is
// no registry and no SDK/GPU work in destruction or quarantine.
class RetainedReader : public Reader, public std::enable_shared_from_this<RetainedReader>
{
    std::shared_ptr<RetainedReader> quarantine_;
protected:
    Microsoft::WRL::ComPtr<ID3D12Fence> done_;
    uint64_t doneValue_ = 0;
public:
    void Quarantine() noexcept override { quarantine_ = weak_from_this().lock(); }
    bool BindCompletion(ID3D12Fence* fence, uint64_t value) noexcept override
    {
        if (done_ || !FenceValid(fence, value)) { Quarantine(); return false; }
        done_ = fence; doneValue_ = value;
        return true;
    }
};
class NativeReader final : public RetainedReader
{
public:
    native::BridgeDepthLeasePtr lease;
    native::NativeSubmission producer;
    Microsoft::WRL::ComPtr<ID3D12Resource> color;
    Microsoft::WRL::ComPtr<ID3D12Fence> inputFence, producerFence;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    uint64_t inputValue = 0, producerValue = 0, frame = 0, generation = 0, batch = 0;
    uint64_t stream = 0, streamGeneration = 0, epoch = 0;
    ID3D12Resource* depth = nullptr;
    DWORD thread = GetCurrentThreadId();
    bool Validate(const Access& a) const noexcept override
    {
        if (GetCurrentThreadId() != thread || !producer || !lease || !lease->Ready() ||
            a.resource != depth || a.queue != queue.Get() || a.readyFence != producerFence.Get() ||
            a.readyValue != producerValue || a.owner != stream || a.generation != streamGeneration ||
            a.frame != frame || a.epoch != epoch ||
            !lease->MatchesReader(depth, inputFence.Get(), inputValue)) return false;
        Microsoft::WRL::ComPtr<ID3D12Device> colorDevice, fenceDevice;
        return SUCCEEDED(color->GetDevice(IID_PPV_ARGS(&colorDevice))) &&
            SUCCEEDED(producerFence->GetDevice(IID_PPV_ARGS(&fenceDevice))) &&
            producer->Matches(color.Get(), queue.Get(), queue.Get(), producerFence.Get(), producerValue,
                colorDevice.Get(), fenceDevice.Get(), frame, generation, batch);
    }
    bool BindCompletion(ID3D12Fence* fence, uint64_t value) noexcept override
    {
        Microsoft::WRL::ComPtr<ID3D12Device> producerOwner, consumerOwner;
        if (!fence || FAILED(producerFence->GetDevice(IID_PPV_ARGS(&producerOwner))) ||
            FAILED(fence->GetDevice(IID_PPV_ARGS(&consumerOwner))) ||
            !Same(producerOwner.Get(), consumerOwner.Get())) { Quarantine(); return false; }
        // The parent FrameInput reader is already begun. Do NOT consume its
        // one-shot BindReaderCompletion: source.Return binds the final native
        // consumer fence, ordered after this owned batch on the same queue.
        // Its failure path retains the parent rather than minting completion.
        return RetainedReader::BindCompletion(fence, value);
    }
};
class BeforeSrReader final : public RetainedReader
{
public:
    std::shared_ptr<GenshinTransport::Lease> lease;
    GenshinGuides::Owner owner;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> ready;
    uint64_t value = 0, stream = 0, generation = 0;
    bool Validate(const Access& a) const noexcept override
    {
        if (!lease || owner.thread != GetCurrentThreadId() || !owner.device || !owner.context ||
            !owner.deviceEpoch || !owner.producerEpoch || !owner.configEpoch || !owner.instance ||
            !owner.generation || !owner.historyEpoch || a.frame != owner.sourceFrame ||
            a.owner != stream || a.generation != generation || a.epoch != owner.historyEpoch ||
            a.queue != queue.Get() || a.readyFence != ready.Get() || a.readyValue != value) return false;
        const auto observation = lease->Snapshot();
        return !observation.revoked && observation.stage == GenshinTransport::Stage::InputsOrdered &&
            GenshinGuides::SameOwner(owner, observation.owner) && observation.queue == reinterpret_cast<uint64_t>(queue.Get()) &&
            lease->Resource(owner, 1) == a.resource;
    }
    bool BindCompletion(ID3D12Fence* fence, uint64_t value) noexcept override
    {
        Microsoft::WRL::ComPtr<ID3D12Device> readyOwner, doneOwner;
        if (!fence || FAILED(ready->GetDevice(IID_PPV_ARGS(&readyOwner))) ||
            FAILED(fence->GetDevice(IID_PPV_ARGS(&doneOwner))) || !Same(readyOwner.Get(), doneOwner.Get()))
        { Quarantine(); return false; }
        // The full transport stays in this reader until the OWNED completion,
        // even if SR later refuses/never submits. Its surface pool cannot run
        // until the lease's last owner disappears. Unknown work self-retains.
        return RetainedReader::BindCompletion(fence, value);
    }
};

}


void Clear(reshade::api::effect_runtime* runtime)
{
    if (!runtime) return;
    // Clear the readiness flag first, including partial/throwing SDK updates.
    Uniforms(runtime, false, nullptr);
    runtime->update_texture_bindings("DEPTH", {0}, {0});
}
void Cache::Destroy(reshade::api::device* device) noexcept
{
    if (srv.handle && this->device != device) return; // never destroy a foreign SDK view
    try { if (device && srv.handle) device->destroy_resource_view(srv); } catch (...) {}
    srv = {0}; resource.Reset(); typedFormat = DXGI_FORMAT_UNKNOWN; this->device = nullptr;
}
Prepared::Prepared(const Access& input, reshade::api::device* device, reshade::api::resource_view view, Cache& cache)
    : input_(input), device_(device), srv_(view), cache_(&cache), lifetime_(input.reader) {}
Prepared::~Prepared()
{
    if (lifetime_.Began() && (!lifetime_.Completed() || lifetime_.Quarantined())) cache_->quarantined = true;
}
void Prepared::Transition(ID3D12GraphicsCommandList* list, bool restore)
{
    if (input_.entryState == ShaderRead) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = input_.resource;
    barrier.Transition.Subresource = input_.subresource;
    barrier.Transition.StateBefore = restore ? ShaderRead : input_.entryState;
    barrier.Transition.StateAfter = restore ? input_.entryState : ShaderRead;
    list->ResourceBarrier(1, &barrier);
}
bool Prepared::Begin(reshade::api::effect_runtime* runtime, reshade::api::command_list* sdkList,
                     ID3D12GraphicsCommandList* nativeList)
{
    Clear(runtime);
    if (cache_->quarantined || lifetime_.Began() || !runtime || runtime->get_device() != device_ || !sdkList || sdkList->get_device() != device_ ||
        !runtime->get_command_queue() || runtime->get_command_queue()->get_native() != reinterpret_cast<uint64_t>(input_.queue) ||
        !nativeList || sdkList->get_native() != reinterpret_cast<uint64_t>(nativeList) ||
        nativeList->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || !input_.reader->Validate(input_)) return false;
    Microsoft::WRL::ComPtr<ID3D12Device> listOwner, queueOwner;
    if (FAILED(nativeList->GetDevice(IID_PPV_ARGS(&listOwner))) ||
        FAILED(input_.queue->GetDevice(IID_PPV_ARGS(&queueOwner))) || !Same(listOwner.Get(), queueOwner.Get())) return false;
    runtime_ = runtime; list_ = sdkList; nativeList_ = nativeList;
    if (!lifetime_.Begin()) return false; // BEFORE any potentially recorded GPU work
    Transition(nativeList, false);
    runtime->update_texture_bindings("DEPTH", srv_, srv_); // depth has no sRGB interpretation
    bool bound = false;
    runtime->enumerate_texture_variables(nullptr, [&](auto* r, auto variable) {
        reshade::api::resource_view linear{0}, srgb{0};
        r->get_texture_binding(variable, &linear, &srgb);
        if (linear.handle == srv_.handle && srgb.handle == srv_.handle) bound = true;
    });
    // update_texture_bindings is void. Only actual texture binding reflection,
    // never an SDK declaration, descriptor, UI or preview, publishes availability.
    if (!bound) runtime->update_texture_bindings("DEPTH", {0}, {0});
    Uniforms(runtime, bound, &input_);
    return bound;
}
void Prepared::End(reshade::api::effect_runtime* runtime, reshade::api::command_list* sdkList,
                   ID3D12GraphicsCommandList* nativeList)
{
    if (!lifetime_.Began()) { Clear(runtime); return; }
    if (restored_ || runtime != runtime_ || sdkList != list_ || nativeList != nativeList_ ||
        !sdkList || sdkList->get_device() != device_ || !nativeList ||
        sdkList->get_native() != reinterpret_cast<uint64_t>(nativeList)) {
        cache_->quarantined = true; lifetime_.Quarantine(); throw std::logic_error("HoYoShade depth reader list changed");
    }
    Clear(runtime);
    Transition(nativeList, true);
    restored_ = true;
    lifetime_.Restored();
}
bool Prepared::Complete(ID3D12Fence* fence, uint64_t value) noexcept
{
    const bool completed = lifetime_.Complete(fence, value);
    if (!completed) cache_->quarantined = true;
    return completed;
}
std::shared_ptr<Prepared> Prepare(const Access& a, ID3D12Device* nativeDevice,
                                 reshade::api::device* sdkDevice, Cache& cache)
{
    if (cache.quarantined || !a.resource || !a.reader || !nativeDevice || !sdkDevice || !a.owner || !a.generation || !a.epoch ||
        !a.queue || a.queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        !FenceValid(a.readyFence, a.readyValue) || !Compatible(a.resource->GetDesc(), a) ||
        !a.reader->Validate(a) || sdkDevice->get_api() != reshade::api::device_api::d3d12) return {};
    Microsoft::WRL::ComPtr<ID3D12Device> resourceDevice;
    if (FAILED(a.resource->GetDevice(IID_PPV_ARGS(&resourceDevice))) ||
        !Same(resourceDevice.Get(), nativeDevice) || FAILED(nativeDevice->GetDeviceRemovedReason())) return {};
    // Reader validates queue/ready in its actual producer domain (which may
    // expose a separately receipted public device). No native SDK-pointer cast.
    const reshade::api::resource resource{reinterpret_cast<uint64_t>(a.resource)};
    const auto desc = sdkDevice->get_resource_desc(resource);
    if (desc.type != reshade::api::resource_type::texture_2d || desc.texture.width != a.width ||
        desc.texture.height != a.height || desc.texture.depth_or_layers != 1 || desc.texture.levels != 1 ||
        desc.texture.samples != 1 || static_cast<DXGI_FORMAT>(desc.texture.format) != a.resource->GetDesc().Format) return {};
    if (cache.srv.handle && cache.device != sdkDevice) return {};
    if (!cache.srv.handle || cache.resource.Get() != a.resource || cache.typedFormat != a.typedFormat) {
        cache.Destroy(sdkDevice); // caller has cleared the semantic and fenced the previous use
        reshade::api::resource_view view{0};
        const reshade::api::resource_view_desc viewDesc(static_cast<reshade::api::format>(a.typedFormat), 0, 1, 0, 1);
        if (!sdkDevice->create_resource_view(resource, reshade::api::resource_usage::shader_resource, viewDesc, &view)) {
            if (view.handle) sdkDevice->destroy_resource_view(view);
            return {};
        }
        cache.device = sdkDevice; cache.srv = view; cache.resource = a.resource; cache.typedFormat = a.typedFormat;
    }
    const auto viewDesc = sdkDevice->get_resource_view_desc(cache.srv);
    if (sdkDevice->get_resource_from_view(cache.srv).handle != resource.handle ||
        viewDesc.type != reshade::api::resource_view_type::texture_2d ||
        static_cast<DXGI_FORMAT>(viewDesc.format) != a.typedFormat || viewDesc.texture.first_level != 0 ||
        (viewDesc.texture.levels != 1 && viewDesc.texture.levels != UINT32_MAX) || viewDesc.texture.first_layer != 0 ||
        (viewDesc.texture.layers != 1 && viewDesc.texture.layers != UINT32_MAX)) return {};
    return std::make_shared<Prepared>(a, sdkDevice, cache.srv, cache);
}
Input FromNative(const native::FrameInput& frame, ID3D12CommandQueue* queue, const native::SyncPoint& ready,
                 uint64_t stream, uint64_t generation) noexcept
{
    try {
        // GenericDepth's short snapshot has no content-owning reader capability.
        // Never manufacture one from its raw COM pointer or pick one split copy.
        // BeforeNR/final effects address the final picture. A retained raw SR
        // plane is not a Scene/Picture UV mapping, even at matching extents.
        // Preserve its parent lease; only the optional effect binding is refused.
        if (frame.depthCoordinates != native::DepthCoordinates::Picture ||
            frame.depthCount != 1 || !frame.depth[0] || !frame.depthLease || !frame.depthProducerReady ||
            !frame.depthCoverage || !frame.depthEpoch || !frame.depthGeneration || !frame.depthPictureSerial ||
            frame.depthResource != frame.depth[0] || frame.depthOriginX || frame.depthOriginY ||
            !ready.submission || !queue || !ready.fence || !ready.value) return {};
        const auto desc = frame.depth[0]->GetDesc();
        // The bridge shares simultaneous-access textures. Producer Execute has
        // completed recording; these textures decay to COMMON at the batch end.
        // Unknown non-simultaneous-access entry state must remain unavailable.
        if (!(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)) return {};
        auto reader = std::make_shared<NativeReader>();
        reader->lease = frame.depthLease; reader->producer = ready.submission; reader->color = frame.picture;
        reader->depth = frame.depth[0]; reader->inputFence = frame.ready.fence; reader->inputValue = frame.ready.value;
        reader->producerFence = ready.fence; reader->producerValue = ready.value; reader->queue = queue;
        reader->generation = ready.submission->Generation(); reader->batch = ready.submission->Batch();
        reader->frame = frame.frame; reader->stream = stream; reader->streamGeneration = generation; reader->epoch = frame.depthEpoch;
        Access a;
        a.resource = frame.depth[0]; a.typedFormat = frame.depthView; a.width = frame.depthWidth; a.height = frame.depthHeight;
        a.stateKnown = a.directionKnown = true; a.entryState = D3D12_RESOURCE_STATE_COMMON;
        a.reversed = frame.depthReversed; a.yFlipped = frame.depthYFlip; a.validityPaired = frame.depthValidityPaired;
        a.queue = queue; a.readyFence = ready.fence; a.readyValue = ready.value;
        a.owner = stream; a.generation = generation; a.frame = frame.frame; a.epoch = frame.depthEpoch; a.reader = reader;
        return reader->Validate(a) && Compatible(desc, a) ? a : Access{};
    } catch (...) { return {}; }
}
Input FromBeforeSr(const std::shared_ptr<GenshinTransport::Lease>& lease, const GenshinGuides::Owner& owner,
    ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value, uint64_t stream, uint64_t generation,
    uint64_t frame, uint32_t width, uint32_t height, bool directionKnown, bool reversed) noexcept
{
    try {
        if (!lease || !directionKnown || frame != owner.sourceFrame) return {};
        auto reader = std::make_shared<BeforeSrReader>();
        reader->lease = lease; reader->owner = owner; reader->queue = queue; reader->ready = fence;
        reader->value = value; reader->stream = stream; reader->generation = generation;
        Access a;
        a.resource = lease->Resource(owner, 1); a.typedFormat = DXGI_FORMAT_R32_FLOAT;
        a.width = width; a.height = height; a.stateKnown = a.directionKnown = true;
        a.entryState = D3D12_RESOURCE_STATE_COMMON; a.reversed = reversed;
        // This is the already prepared SR plane. Preserve its actual orientation;
        // do not apply the original game's extraction Y transform a second time.
        a.queue = queue; a.readyFence = fence; a.readyValue = value;
        a.owner = stream; a.generation = generation; a.frame = frame; a.epoch = owner.historyEpoch; a.reader = reader;
        return a.resource && reader->Validate(a) && Compatible(a.resource->GetDesc(), a) ? a : Access{};
    } catch (...) { return {}; }
}
}
