#include <mods/gimi/GimiCaptureRouting.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace gimi::runtime;
unsigned checks = 0;
void Check(bool ok, const char* reason)
{
    ++checks;
    if (!ok) throw std::runtime_error(reason);
}
std::atomic_bool enabled{true}, presentNeeded{true}, mapCaptureLive{false}, streamCaptureLive{false};
std::atomic<std::uint64_t> contractRevision{1};
std::atomic<unsigned> constantMapCount{0};
CaptureRouting captureRouting;
bool Enabled() noexcept { return enabled.load(); }
bool NeedsPresent() noexcept { return presentNeeded.load(); }
std::uint64_t Ids() noexcept { static std::uint64_t id = 1; return id++; }
constexpr std::size_t MapKeyCapacity = 8192;
constexpr UINT ConstantByteLimit = 65536;
// HEADER: enum class CaptureSource
// HEADER: struct CreationHookRequest
// HEADER: class CpuContentSubmission final
// PRODUCTION: CaptureRoute DeviceEvent(
// PRODUCTION: CaptureRoute ContextEvent(
// PRODUCTION: std::size_t Bucket(
// PRODUCTION: template<class T> std::uint64_t Pointer(
// PRODUCTION: struct MapKey
std::array<MapKey, MapKeyCapacity> mapKeys{};
// PRODUCTION: bool RegisterMapResource(
// PRODUCTION: CaptureRoute ResourceEvent(
// PRODUCTION: struct alignas(16) ConstantBlock
// PRODUCTION: struct ConstantContent
// PRODUCTION: bool CopyConstantCpu(
struct Identity { unsigned writes = 0; void OpaqueWrite() noexcept { ++writes; } };
struct GameResource
{
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Buffer> buffer;
    D3D11_BUFFER_DESC bufferDesc{};
    std::unique_ptr<ConstantContent> constant;
    Identity record;
    bool mapped = false, vertexReadOnly = true, gpuBuffer = false, capturedInput = true;
    bool gpuOrdered = false, soCursorKnown = false;
    std::uint64_t gpuRevision = 1, soCursorUpper = 0, readGeneration = 0;
};
struct HookServices { bool (*internal)(ID3D11DeviceContext*) noexcept = nullptr; };
struct Selection { bool enabled = true; };
struct HashPolicy { bool resolutionKnown = false; UINT screenWidth = 0, screenHeight = 0; };
struct LayoutElement
{
    std::string semantic;
    UINT semanticIndex = 0, slot = 0, offset = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
struct Layout
{
    ComPtr<ID3D11InputLayout> layout;
    std::uint64_t id = 0;
    std::vector<LayoutElement> elements;
    std::uint32_t slots = 0;
    bool exact = false;
};
constexpr unsigned GimiVertexSlots = 8;
template<class T> struct Lookup
{
    std::unordered_map<std::uint64_t, std::shared_ptr<T>> entries;
    std::shared_ptr<T> Find(std::uint64_t key) const noexcept
    { const auto it = entries.find(key); return it == entries.end() ? nullptr : it->second; }
    bool Add(std::uint64_t key, std::shared_ptr<T> value) { return entries.emplace(key, std::move(value)).second; }
};
struct Capture
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext1> rangeContext;
    std::uint64_t generation = 0, creationAttempt = 0, streamRevision = 1, invalidation = 1, vertexReadRevision = 1;
    unsigned routeSlot = 0;
    LONG creationResult = NO_ERROR;
    bool coverage = false, creations = false, opaque = false, exhausted = false, streamCoverage = false;
    HookServices hooks;
    HashPolicy hashPolicy;
    Lookup<GameResource> resources;
    Lookup<Layout> layouts;
    std::array<std::shared_ptr<GameResource>, D3D11_SO_BUFFER_SLOT_COUNT> streamTargets{};
    void ObserveResolution(UINT width, UINT height) noexcept
    { hashPolicy = {width && height, width, height}; }
};
// PRODUCTION: struct ConstantMap
// PRODUCTION: struct ConstantMaps
// PRODUCTION: ConstantMaps& ConstantMapStore(
struct Commands { bool CanInitialize() const { return true; } void Discard() {} };
struct UserState { bool dirty = false; bool Dirty() const { return dirty; } };
struct Session
{
    enum class PacketState { Empty, Fenced, Unsealed, Quarantine };
    struct Packet { PacketState state = PacketState::Empty; };
    std::array<Packet, 1> packets;
    std::shared_ptr<Capture> capture;
    std::unique_ptr<Commands> commands = std::make_unique<Commands>();
    UserState userState;
    DWORD thread = GetCurrentThreadId();
    bool accepting = true;
    unsigned retireCalls = 0, saves = 0, preparations = 0, input = 0;
    void RetireAtPresent() { ++retireCalls; }
    bool HasPackets() const { return false; }
    void ConsumeInput() { ++input; }
    bool saveSucceeds = true;
    bool SaveUserState(bool, std::uint64_t)
    { ++saves; if (!saveSucceeds) return false; userState.dirty = false; return true; }
    void PrepareDescriptors() { ++preparations; }
    void PrepareWindows() { ++preparations; }
    void PreparePipeline() { ++preparations; }
    void Quarantine(const char*) { accepting = false; }
};
struct State
{
    std::mutex ownerMutex, requestMutex;
    std::shared_ptr<Capture> capture;
    std::array<std::shared_ptr<Capture>, CaptureRouting::Capacity> candidates{};
    bool captureConfirmed = false;
    std::shared_ptr<Session> active;
    std::array<std::shared_ptr<Session>, 2> retired{};
    Selection selection;
    std::uint64_t loaded = 0, revision = 1, nativePresentTicket = 0, observedRasterTicket = 0;
    struct Observation
    {
        char message[768]{}, hostRoot[8]{};
        unsigned hostNativeSettings = 0, hostSourceSettings = 0, hostInactiveSettings = 0;
        unsigned pending = 0, quarantined = 0;
    } observation;
    unsigned parks = 0, loads = 0;
    void Message(const char* text) { std::snprintf(observation.message, sizeof(observation.message), "%s", text); }
};
State& Store() { static State state; return state; }
void Park(State& s) { ++s.parks; if (s.active && !s.active->userState.Dirty()) s.active.reset(); }
std::shared_ptr<Session> Load(State& s, const Selection&)
{ ++s.loads; auto session = std::make_shared<Session>(); session->capture = s.capture; return session; }
unsigned advances = 0;
void AdvanceStreamOutputs(Capture&, UINT, UINT, bool) noexcept { ++advances; }
// PRODUCTION: CaptureRoute RouteOf(
// PRODUCTION: std::shared_ptr<Capture> Captured(
// PRODUCTION: void RefreshCaptureFlags(
// PRODUCTION: void LostCoverage(
// PRODUCTION: void CreationCoverageLocked(
// PRODUCTION: void DeviceCreated(
// PRODUCTION: void CreationCoverage(
// PRODUCTION: CreationHookRequest ClaimCreationHooks(
// PRODUCTION: void CompleteCreationHooks(
// PRODUCTION: void LayoutCreated(
// PRODUCTION: void StreamOutputCoverage(
// PRODUCTION: void StreamOutputTargets(
// PRODUCTION: void StreamOutputDrawn(
// PRODUCTION: void StreamOutputUnknownDraw(
// PRODUCTION: void StreamOutputCleared(
// PRODUCTION: static void InvalidateReadWrite(
// PRODUCTION: std::uint64_t VertexMapStarting(
// PRODUCTION: void VertexMapFailed(
// PRODUCTION: void VertexUnmapped(
// PRODUCTION: void Written(
// PRODUCTION: static bool ConstantUnconditional(
// PRODUCTION: CpuContentSubmission CpuContentSubmission::Update(
// PRODUCTION: CpuContentSubmission CpuContentSubmission::Copy(
// PRODUCTION: void ConstantMapped(
// PRODUCTION: CpuContentSubmission CpuContentSubmission::Unmap(
// PRODUCTION: void CpuContentSubmission::Returned(
// PRODUCTION: void OpaqueBoundary(
// PRODUCTION: void Present(

struct Device
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context, deferred;
    Device()
    {
        Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
              D3D11_SDK_VERSION, &device, nullptr, &context)), "create WARP owner");
        Check(SUCCEEDED(device->CreateDeferredContext(0, &deferred)), "create same-device deferred context");
    }
};
void Fresh(Device& device)
{
    auto& s = Store(); s.active.reset(); s.retired = {}; s.capture.reset(); s.candidates = {}; s.captureConfirmed = false; captureRouting.Clear();
    s.loaded = 0; s.revision = contractRevision.fetch_add(1) + 1;
    s.parks = s.loads = 0; s.nativePresentTicket = s.observedRasterTicket = 0;
    s.selection.enabled = true; enabled = presentNeeded = true;
    DeviceCreated(device.device.Get(), device.context.Get(), {}, true, CaptureSource::DeviceCreation);
    CreationCoverage(device.device.Get(), true);
    DeviceCreated(device.device.Get(), device.context.Get(), {}, true, CaptureSource::PresentOwner);
    Check(s.capture && captureRouting.Read().generation == s.capture->generation && !captureRouting.Lost(), "fresh generation");
}
std::shared_ptr<GameResource> Resource(Device& device, const std::shared_ptr<Capture>& capture = Store().capture,
                                       D3D11_USAGE usage = D3D11_USAGE_DEFAULT)
{
    auto game = std::make_shared<GameResource>();
    game->bufferDesc = {16, usage, D3D11_BIND_CONSTANT_BUFFER,
                        usage == D3D11_USAGE_DYNAMIC ? D3D11_CPU_ACCESS_WRITE : 0u, 0, 0};
    Check(SUCCEEDED(device.device->CreateBuffer(&game->bufferDesc, nullptr, &game->buffer)), "create real test buffer");
    Check(SUCCEEDED(game->buffer.As(&game->resource)), "resource interface");
    game->readGeneration = capture->generation;
    game->constant = std::make_unique<ConstantContent>();
    game->constant->bytes.resize(1); game->constant->staging.resize(1);
    capture->resources.entries.emplace(Pointer(game->resource.Get()), game);
    Check(RegisterMapResource(game->resource.Get(), game->readGeneration), "publish resource generation");
    return game;
}
template<class F> void Busy(F&& call)
{
    // Real contention on another thread; never try_lock a mutex held by self.
    std::lock_guard lock(Store().ownerMutex);
    std::thread thread(std::forward<F>(call)); thread.join();
}
void DeviceAndPresent(Device& main, Device& auxiliary)
{
    Fresh(main); auto& s = Store(); auto capture = s.capture;
    s.active = Load(s, s.selection); s.loaded = s.revision;
    auto active = s.active; const auto parks = s.parks;
    DeviceCreated(auxiliary.device.Get(), auxiliary.context.Get(), {}, true, CaptureSource::DeviceCreation);
    Check(s.capture == capture && s.active == active && s.parks == parks && active->accepting, "auxiliary creation cannot park/replace owner");
    Check(!DeviceEvent(auxiliary.device.Get()), "confirmed owner does not register auxiliary candidates");
    const auto oldRevision = s.loaded; ++s.revision; contractRevision = s.revision;
    Present(auxiliary.device.Get(), auxiliary.context.Get(), 16, 16);
    Check(s.loaded == oldRevision && s.loads == 1 && s.parks == parks && s.active == active, "foreign Present cannot consume reload");
    Check(active->retireCalls == 1 && !active->saves && !active->input, "foreign Present only polls retained packet owner");
    Present(main.device.Get(), main.context.Get(), 16, 16);
    Check(s.loaded == s.revision && s.loads == 2 && s.active != active, "primary Present loads pending revision");
    Check(s.active->preparations == 3, "primary Present prepares its actual plan");
    auto migrating = s.active;
    migrating->userState.dirty = true;
    s.nativePresentTicket = 31;
    DeviceCreated(auxiliary.device.Get(), auxiliary.context.Get(), {}, true, CaptureSource::PresentOwner);
    Check(s.capture == capture && s.active == migrating && !migrating->saves && mapCaptureLive, "open native pair blocks migration before save/observation changes");
    s.nativePresentTicket = 0;
    Busy([&] {}); // establish ordinary contention without migrating on it
    std::thread other([&] {
        DeviceCreated(auxiliary.device.Get(), auxiliary.context.Get(), {}, true, CaptureSource::PresentOwner);
    }); other.join();
    Check(s.capture == capture && !migrating->saves && mapCaptureLive, "wrong serial thread cannot save/migrate interpreter");
    migrating->saveSucceeds = false;
    DeviceCreated(auxiliary.device.Get(), auxiliary.context.Get(), {}, true, CaptureSource::PresentOwner);
    Check(s.capture == capture && s.active == migrating && migrating->userState.Dirty() && mapCaptureLive, "failed state save retains old capture and observation");
    migrating->saveSucceeds = true;
    DeviceCreated(auxiliary.device.Get(), auxiliary.context.Get(), {}, true, CaptureSource::PresentOwner);
    Check(s.capture != capture && s.capture->device.Get() == auxiliary.device.Get() && !s.loaded, "explicit selected Present can migrate owner");
    Check(migrating->saves == 2 && !migrating->userState.Dirty(), "selected Present saves dirty semantic owner before migration");
    captureRouting.Lose(RouteOf(*capture));
    Check(!captureRouting.Lost(), "old owner failure cannot poison migrated capture");
    const auto current = s.capture;
    DeviceCreated(auxiliary.device.Get(), main.context.Get(), {}, true, CaptureSource::PresentOwner);
    Check(s.capture == current, "mismatched activation device/context refused");
}
void Events(Device& main, Device& auxiliary)
{
    Fresh(main); auto capture = Store().capture; auto game = Resource(main);
    const auto generation = capture->generation;
    const auto streamRevision = capture->streamRevision;
    streamCaptureLive = true;
    Busy([&] {
        OpaqueBoundary(auxiliary.context.Get());
        StreamOutputTargets(auxiliary.context.Get(), 0, nullptr, nullptr);
        StreamOutputDrawn(auxiliary.context.Get(), 3, 1);
        StreamOutputUnknownDraw(auxiliary.context.Get());
        StreamOutputCleared(auxiliary.context.Get());
        Written(auxiliary.context.Get(), game->resource.Get());
        VertexMapFailed(auxiliary.context.Get(), game->resource.Get(), 0, generation);
    });
    Check(!captureRouting.Lost() && !capture->opaque && capture->streamRevision == streamRevision, "foreign callbacks ignore primary mutex contention and SO state");
    OpaqueBoundary(main.deferred.Get());
    Check(captureRouting.Lost(), "same-device unknown replay remains fatal");
    Fresh(main); game = Resource(main); capture = Store().capture;
    Written(main.deferred.Get(), game->resource.Get());
    Check(captureRouting.Lost(), "same-device foreign-context captured write remains fatal");
    Fresh(main); capture = Store().capture; streamCaptureLive = true;
    StreamOutputTargets(main.deferred.Get(), 0, nullptr, nullptr);
    Check(captureRouting.Lost() && capture->streamRevision == 1, "foreign context cannot overwrite primary SO snapshot");
    Fresh(main); game = Resource(main); capture = Store().capture;
    Written(main.context.Get(), game->resource.Get());
    Check(!captureRouting.Lost() && game->record.writes == 1 && capture->vertexReadRevision == 2, "primary write revokes actual resource identity");
    Busy([&] { Written(main.context.Get(), game->resource.Get()); });
    Check(captureRouting.Lost(), "lost primary write under contention still closes capture");
}
void ProvisionalOrder(Device& main, Device& auxiliary)
{
    Fresh(main);
    auto& s = Store(); s.capture.reset(); s.candidates = {}; s.captureConfirmed = false; captureRouting.Clear();
    DeviceCreated(auxiliary.device.Get(), auxiliary.context.Get(), {}, true, CaptureSource::DeviceCreation);
    CreationCoverage(auxiliary.device.Get(), true);
    const auto first = s.capture;
    DeviceCreated(main.device.Get(), main.context.Get(), {}, true, CaptureSource::DeviceCreation);
    CreationCoverage(main.device.Get(), true);
    const auto route = DeviceEvent(main.device.Get());
    auto provisional = Captured(s, route);
    Check(provisional && provisional != first && !s.captureConfirmed, "second creation registers a separate provisional owner");
    // The selected routes must be the exact same registry used by all production
    // shader/layout/buffer creation callbacks, not a replay of copied metadata.
    Check(ContextEvent(main.context.Get()) == route && DeviceEvent(main.device.Get()) == route, "creation/context route finds main before first Present");
    constexpr char vertex[] = "float4 main(float4 position:POSITION):SV_Position{return position;}";
    ComPtr<ID3DBlob> code, errors;
    Check(SUCCEEDED(D3DCompile(vertex, sizeof(vertex), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &code, &errors)), "compile real layout shader");
    D3D11_INPUT_ELEMENT_DESC element{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    ComPtr<ID3D11InputLayout> layout;
    Check(SUCCEEDED(main.device->CreateInputLayout(&element, 1, code->GetBufferPointer(), code->GetBufferSize(), &layout)), "create real provisional WARP input layout");
    LayoutCreated(main.device.Get(), layout.Get(), &element, 1);
    const auto layoutOwner = provisional->layouts.Find(Pointer(layout.Get()));
    Check(layoutOwner && layoutOwner->exact && layoutOwner->slots == 1 && !first->layouts.Find(Pointer(layout.Get())), "production LayoutCreated records main candidate before its first Present");
    auto game = Resource(main, provisional);
    auto copied = Resource(main, provisional);
    auto dynamic = Resource(main, provisional, D3D11_USAGE_DYNAMIC);
    const auto generation = provisional->generation;
    Written(main.context.Get(), game->resource.Get());
    Check(game->record.writes == 1 && !captureRouting.Lost(route), "provisional writes revoke original bytes before promotion");
    std::array<std::uint8_t, 16> input{}; input.fill(43);
    auto update = CpuContentSubmission::Update(main.context.Get(), game->resource.Get(), 0, nullptr, input.data(), 0);
    Check(bool(update), "prepare non-first provisional constant update");
    main.context->UpdateSubresource(game->resource.Get(), 0, nullptr, input.data(), 0, 0);
    update.Returned();
    Check(game->constant->published == game->constant->revision && !game->constant->pending &&
          !std::memcmp(game->constant->Data(), input.data(), input.size()), "non-first provisional Update publishes authentic return");
    Written(main.context.Get(), copied->resource.Get());
    auto copy = CpuContentSubmission::Copy(main.context.Get(), copied->resource.Get(), game->resource.Get(),
                                           0, 0, 0, 0, 0, nullptr, 0, true);
    Check(bool(copy), "prepare non-first provisional constant copy");
    main.context->CopyResource(copied->resource.Get(), game->resource.Get());
    copy.Returned();
    Check(copied->constant->published == copied->constant->revision && !copied->constant->pending &&
          !std::memcmp(copied->constant->Data(), input.data(), input.size()), "non-first provisional Copy publishes authentic return");
    const auto ticket = VertexMapStarting(main.context.Get(), dynamic->resource.Get(), 0, D3D11_MAP_WRITE_DISCARD);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto mappedResult = main.context->Map(dynamic->resource.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    Check(ticket && SUCCEEDED(mappedResult), "map real provisional dynamic constant buffer");
    ConstantMapped(main.context.Get(), dynamic->resource.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped, mappedResult, ticket, false);
    Check(constantMapCount == 1, "non-first provisional Map retains writeback owner");
    std::memcpy(mapped.pData, input.data(), input.size());
    auto unmap = CpuContentSubmission::Unmap(main.context.Get(), dynamic->resource.Get(), 0, true);
    main.context->Unmap(dynamic->resource.Get(), 0);
    VertexUnmapped(main.context.Get(), dynamic->resource.Get(), 0);
    Check(bool(unmap) && !constantMapCount, "non-first provisional Unmap freezes authentic bytes");
    unmap.Returned();
    Check(dynamic->constant->published == dynamic->constant->revision && !dynamic->constant->pending &&
          !std::memcmp(dynamic->constant->Data(), input.data(), input.size()), "non-first provisional Unmap publishes authentic return");
    OpaqueBoundary(auxiliary.context.Get());
    Check(captureRouting.Lost(RouteOf(*first)) && !captureRouting.Lost(route), "provisional failures belong to their own slot");
    Present(auxiliary.device.Get(), auxiliary.context.Get(), 16, 16);
    Check(!s.loads && !s.captureConfirmed, "unconfirmed auxiliary Present cannot load interpreter");
    DeviceCreated(main.device.Get(), main.context.Get(), {}, true, CaptureSource::PresentOwner);
    Check(s.capture == provisional && s.capture->generation == generation && s.captureConfirmed, "selected Present promotes existing creation history without a new generation");
    Check(s.capture->resources.Find(Pointer(game->resource.Get())) == game && game->record.writes == 1, "promotion preserves resource identity and actual write history");
    Check(s.capture->layouts.Find(Pointer(layout.Get())) == layoutOwner, "promotion retains actual creation observer's layout owner");
    Check(copied->constant->generation == generation && dynamic->constant->generation == generation &&
          !std::memcmp(copied->constant->Data(), input.data(), input.size()) &&
          !std::memcmp(dynamic->constant->Data(), input.data(), input.size()), "promotion preserves completed provisional constant contents");
    Check(!DeviceEvent(auxiliary.device.Get()) && !captureRouting.Lost(), "unselected candidate loss cannot poison promoted main");
    CaptureRouting isolated;
    isolated.Publish({16, 24, 1});
    isolated.Publish({32, 40, 2, 1});
    isolated.Lose(CaptureRoute{32, 40, 2, 1});
    Check(!isolated.Lost(), "newer auxiliary generation failure cannot poison earlier selected capture");
    isolated.LoseAll();
    Check(isolated.Lost(isolated.Read(0)) && isolated.Lost(isolated.Read(1)), "global observation disable invalidates every provisional capture");
}
void StaleMapsAndReturns(Device& main)
{
    Fresh(main); auto game = Resource(main); auto oldCapture = Store().capture;
    std::array<std::uint8_t, 16> input{}; input.fill(73);
    Written(main.context.Get(), game->resource.Get());
    auto pending = CpuContentSubmission::Update(main.context.Get(), game->resource.Get(), 0, nullptr, input.data(), 0);
    Check(bool(pending), "prepare real production CPU-content transaction");
    Fresh(main); auto currentGame = Resource(main);
    const auto generation = Store().capture->generation;
    Check(generation > oldCapture->generation, "replacement uses a fresh generation with identical context keys");
    Busy([&] { VertexMapFailed(main.context.Get(), currentGame->resource.Get(), 0, oldCapture->generation); });
    Check(!captureRouting.Lost(), "stale Map failure rejected before mutex contention");
    Busy([&] { pending.Returned(); });
    Check(!captureRouting.Lost(), "retained old CPU return cannot poison replacement under contention");
    Busy([&] { Written(main.context.Get(), game->resource.Get()); });
    Check(!captureRouting.Lost(), "process-lifetime stale resource key cannot revoke new generation");
    // Mandatory CPU writeback precedes owner/config loss, including mutex busy.
    auto dynamic = currentGame;
    dynamic->bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    dynamic->bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    const auto ticket = VertexMapStarting(main.context.Get(), dynamic->resource.Get(), 0, D3D11_MAP_WRITE_DISCARD);
    std::array<std::uint8_t, 16> driver{};
    D3D11_MAPPED_SUBRESOURCE mapped{driver.data(), 0, 0};
    ConstantMapped(main.context.Get(), dynamic->resource.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped, S_OK, ticket, false);
    Check(mapped.pData != driver.data() && constantMapCount == 1, "production Map owns pending CPU shadow");
    std::memset(mapped.pData, 91, driver.size());
    auto heldCapture = Store().capture;
    Fresh(main);
    Busy([&] { auto returned = CpuContentSubmission::Unmap(main.context.Get(), dynamic->resource.Get(), 0, true); });
    Check(driver.front() == 91 && driver.back() == 91 && constantMapCount == 0, "old Unmap performs mandatory writeback before busy/owner checks");
    Check(!captureRouting.Lost(), "old Unmap contention charged to retained generation");
}
void PublicationRace()
{
    CaptureRouting routes;
    std::atomic_bool go{false}, done{false}, bad{false};
    std::atomic<unsigned> snapshots{0};
    std::thread reader([&] {
        while (!go.load()) std::this_thread::yield();
        while (!done.load()) if (const auto r = routes.Read())
        { ++snapshots; if (r.device != r.generation * 16 || r.context != r.device + 8) bad = true; }
    });
    std::thread failure([&] {
        while (!go.load()) std::this_thread::yield();
        while (!done.load()) routes.Lose(routes.Read());
    });
    routes.Publish({16, 24, 1});
    go = true;
    while (!snapshots.load()) std::this_thread::yield();
    for (std::uint64_t generation = 2; generation <= 100000; ++generation)
        routes.Publish({generation * 16, generation * 16 + 8, generation});
    done = true; reader.join(); failure.join();
    Check(!bad && snapshots > 0, "concurrent ingress sees complete generation/device/context snapshots");
    routes.Publish({1600016, 1600024, 100001});
    routes.Lose(99999); Check(!routes.Lost(), "late failure does not poison later publication");
    routes.Lose(100001); routes.Lose(1); Check(routes.Lost(), "late failure cannot erase current coverage loss");
    routes.Clear(); Check(!routes.Read() && !routes.Lost(), "disabled publication admits no events");
}
int main()
{
    try
    {
        Device main, auxiliary;
        DeviceAndPresent(main, auxiliary); Events(main, auxiliary); ProvisionalOrder(main, auxiliary);
        StaleMapsAndReturns(main); PublicationRace();
        std::printf("PASS: %u production routing/content checks; 100000 concurrent publications\n", checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what()); return 1;
    }
}
