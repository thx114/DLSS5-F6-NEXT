// Included inside namespace HoYoShade. Source association and execution
// placement only. GPU ownership remains with the owned-stage input lease.
namespace {
struct BridgeSourceEntry
{
    const void* wrapper = nullptr;
    IDXGISwapChain* chain = nullptr; // wrapper withdraws before Release
    uint64_t deviceIdentity = 0, chainDeviceIdentity = 0;
    uint64_t generation = 0, sequence = 0, stream = 0;
    unsigned resizing = 0;
};
struct BridgeSources
{
    std::array<BridgeSourceEntry, 8> entries;
    uint64_t generation = 0;
};
BridgeSources& Sources() { static auto* sources = new BridgeSources; return *sources; }
uint64_t BridgeDeviceIdentity(IUnknown* device)
{
    Microsoft::WRL::ComPtr<IUnknown> identity;
    return device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&identity))) ? Key(identity.Get()) : 0;
}
bool BridgeDeviceMatches(const BridgeSourceEntry& entry, uint64_t identity)
{
    return identity && (identity == entry.deviceIdentity || identity == entry.chainDeviceIdentity);
}
}
void RegisterBridgeSource(const void* wrapper, IDXGISwapChain* chain, ID3D11Device* device) noexcept
{
    if (!wrapper || !chain || !device) return;
    try {
        std::lock_guard lock(Get().executionMutex);
        const auto identity = BridgeDeviceIdentity(device);
        Microsoft::WRL::ComPtr<ID3D11Device> chainDevice;
        if (!identity || FAILED(chain->GetDevice(IID_PPV_ARGS(&chainDevice)))) return;
        const auto chainIdentity = BridgeDeviceIdentity(chainDevice.Get());
        if (!chainIdentity) return;
        auto& sources = Sources();
        for (auto& entry : sources.entries) if (entry.wrapper == wrapper) entry = {};
        if (sources.generation == UINT64_MAX) return;
        for (auto& entry : sources.entries) if (!entry.wrapper) {
            entry.wrapper = wrapper; entry.chain = chain;
            entry.deviceIdentity = identity; entry.chainDeviceIdentity = chainIdentity;
            entry.generation = ++sources.generation; return;
        }
    } catch (...) {}
}
void ForgetBridgeSource(const void* wrapper) noexcept
{
    try { std::lock_guard lock(Get().executionMutex);
        for (auto& entry : Sources().entries) if (entry.wrapper == wrapper) entry = {};
    } catch (...) {}
}
BridgeSourceResize::BridgeSourceResize(const void* owner) noexcept : wrapper(owner)
{
    try { std::lock_guard lock(Get().executionMutex);
        for (auto& entry : Sources().entries) if (entry.wrapper == wrapper) {
            entry.stream = entry.sequence = 0; ++entry.resizing;
            entry.generation = Sources().generation == UINT64_MAX ? 0 : ++Sources().generation;
        }
    } catch (...) {}
}
BridgeSourceResize::~BridgeSourceResize()
{
    try { std::lock_guard lock(Get().executionMutex);
        for (auto& entry : Sources().entries) if (entry.wrapper == wrapper && entry.resizing) --entry.resizing;
    } catch (...) {}
}
void BeginBeforeSrSource(ID3D11DeviceContext* context, uint64_t stream) noexcept
{
    try {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        if (context) context->GetDevice(&device);
        std::lock_guard lock(Get().executionMutex);
        const auto identity = BridgeDeviceIdentity(device.Get());
        for (auto& entry : Sources().entries) if (entry.wrapper &&
            (entry.stream == stream || BridgeDeviceMatches(entry, identity))) {
            entry.stream = stream;
            if (!entry.resizing && entry.generation && entry.sequence != UINT64_MAX) ++entry.sequence;
        }
    } catch (...) {}
}
std::shared_ptr<const NaturalAssociation::Source> CaptureBridgeSource(ID3D11Device* device, uint64_t stream) noexcept
{
    try {
        std::lock_guard lock(Get().executionMutex);
        const auto identity = BridgeDeviceIdentity(device);
        BridgeSourceEntry* found = nullptr;
        for (auto& entry : Sources().entries) if (!entry.resizing && entry.generation &&
            BridgeDeviceMatches(entry, identity) && entry.stream == stream) {
            if (found) return {}; found = &entry;
        }
        if (!found || !found->sequence || found->sequence == UINT64_MAX) return {};
        Microsoft::WRL::ComPtr<IDXGISwapChain> chain = found->chain;
        Microsoft::WRL::ComPtr<IDXGISwapChain3> indexed;
        UINT index = 0;
        if (SUCCEEDED(chain.As(&indexed))) index = indexed->GetCurrentBackBufferIndex();
        Microsoft::WRL::ComPtr<ID3D11Texture2D> buffer;
        if (FAILED(chain->GetBuffer(index, IID_PPV_ARGS(&buffer)))) return {};
        return NaturalAssociation::Source::Capture(chain.Get(), device, buffer.Get(), index,
            stream, found->generation, found->sequence);
    } catch (...) { return {}; }
}
namespace {
bool EnrolledStagedPresent(uint64_t nativeChain, uint64_t buffer)
{
    const auto matches = [&](IDXGISwapChain* enrolled) {
        if (Key(enrolled) == nativeChain) return true;
        Microsoft::WRL::ComPtr<IDXGISwapChain3> indexed;
        UINT index = 0;
        if (SUCCEEDED(enrolled->QueryInterface(IID_PPV_ARGS(&indexed)))) index = indexed->GetCurrentBackBufferIndex();
        Microsoft::WRL::ComPtr<ID3D11Texture2D> b11;
        Microsoft::WRL::ComPtr<ID3D12Resource> b12;
        return (SUCCEEDED(enrolled->GetBuffer(index, IID_PPV_ARGS(&b11))) && Key(b11.Get()) == buffer) ||
            (SUCCEEDED(enrolled->GetBuffer(index, IID_PPV_ARGS(&b12))) && Key(b12.Get()) == buffer);
    };
    for (const auto& source : Sources().entries)
        if (source.wrapper && !source.resizing && source.generation && matches(source.chain)) return true;
    DirectChainMarker marker;
    UINT markerSize = sizeof(marker);
    const bool marked = SUCCEEDED(reinterpret_cast<IDXGISwapChain*>(nativeChain)->GetPrivateData(
        DirectChainMarkerGuid, &markerSize, &marker)) && markerSize == sizeof(marker);
    for (const auto& endpoint : Get().directChains) {
        const auto access = endpoint.access.load(std::memory_order_acquire);
        const auto native = endpoint.chain.load(std::memory_order_acquire);
        if (native && !(access & DirectClosed) && access < DirectResizeStep &&
            (native == nativeChain ||
             (marked && marker.wrapper == endpoint.wrapper.load(std::memory_order_acquire) &&
              marker.lifetime == endpoint.lifetime.load(std::memory_order_acquire)))) return true;
    }
    return false;
}
bool BeforeStageNaturalPresent(reshade::api::command_queue* presenting, reshade::api::device* device,
    uint64_t nativeChain, uint64_t buffer) noexcept
{
    if (!device || !presenting || !nativeChain || !buffer ||
        (!Selected(Stage::BeforeSr) && !Selected(Stage::BeforeNr))) return false;
    try {
        CallbackGuard guard;
        if (!guard.entered || ownedOperation ||
            (device->get_api() != reshade::api::device_api::d3d11 && device->get_api() != reshade::api::device_api::d3d12) ||
            device != presenting->get_device() ||
            !EnrolledStagedPresent(nativeChain, buffer)) return false;
        State::Natural* target = nullptr;
        for (auto& entry : Get().natural) {
            if (!entry.live || entry.id.sdkDevice != Key(device)) continue;
            bool matches = false;
            // Live runtime buffers, including the first Present after resize;
            // stale cached epoch/thread observations cannot authorize or block it.
            const auto count = entry.runtime->get_back_buffer_count();
            if (!count || count > 8) continue;
            for (uint32_t i = 0; i < count; ++i) matches |= entry.runtime->get_back_buffer(i).handle == buffer;
            if (!matches) continue;
            if (target) return false; target = &entry;
        }
        if (!target) return false;
        auto* queue = target->runtime->get_command_queue();
        auto* list = queue ? queue->get_immediate_command_list() : nullptr;
        if (!list || list->get_device() != target->runtime->get_device()) return false;
        // Stage placement covers both the source and physical output, including
        // warmup/refused inputs and SDK worker Presents. Natural effects do not
        // reappear after NR/FG while a pre-consumer stage owns this chain.
        OwnedOperation operation;
        target->thread = GetCurrentThreadId();
        target->frame.armed = true;
        target->runtime->render_effects(list, {0}, {0});
        static unsigned reports = 0;
        if (reports++ < 8) LOG_INFO("HoYoShade staged natural Present disabled runtime={} lifetime={} epoch={} api={} chain={} stage={} fallback=none pixel_source=not-inferred",
            Key(target->runtime), target->lifetime, target->frame.epoch, target->id.api, nativeChain,
            Selected(Stage::BeforeSr) ? "before-sr" : "before-nr");
        return true;
    } catch (...) { ++Get().overflow; }
    return false;
}
void OnBeforeNaturalPresent(reshade::api::command_queue* presenting, reshade::api::swapchain* chain,
    const reshade::api::rect*, const reshade::api::rect*, uint32_t, const reshade::api::rect*) noexcept
{
    if (!chain || !presenting || (!Selected(Stage::BeforeSr) && !Selected(Stage::BeforeNr))) return;
    try {
        const auto index = chain->get_current_back_buffer_index();
        if (index < chain->get_back_buffer_count())
            BeforeStageNaturalPresent(presenting, chain->get_device(), chain->get_native(), chain->get_back_buffer(index).handle);
    } catch (...) { ++Get().overflow; }
}
}
