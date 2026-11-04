#include "GimiRuntime.h"
#include "GimiUserState.h"
#include <menu/input/input_system.h>
#include "GimiFormatText.h"
#include "GimiShaderSource.h"
#include "GimiShaderTools.h"
#include "GimiRasterObservers.h"
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <Windows.h>
#include <shellscalingapi.h>
#include <chrono>
#include <cmath>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#pragma comment(lib, "Shcore.lib") // documented OS API, no runtime loader or second core

namespace gimi::runtime
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr std::size_t ResourceCapacity = 4096, ViewCapacity = 4096, ShaderCapacity = 2048;
constexpr std::size_t LayoutCapacity = 256, WindowCapacity = 512, PendingWindows = 128;
constexpr std::size_t Flights = 16, RetiredCapacity = 8;
constexpr std::size_t CpuBudget = 128u * 1024u * 1024u;
std::atomic_bool enabled{false}, presentNeeded{false}, mapCaptureLive{false};
std::atomic<std::uint64_t> nextId{1}, contractRevision{1};
std::atomic_bool streamCaptureLive{false};

// XXMI-Libs 065dc746: static Globals construction records steady-clock
// microseconds; TIME is updated at native non-TEST RunFrameActions PRE and
// remains that same frame value through the original return/POST. Never reset
// this origin on MOD enable/reload, and do not substitute process-creation UTC.
std::int64_t NativeTicks() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
const std::int64_t nativeLaunchTicks = NativeTicks();
constexpr auto NativeTime = NativeValueBit(NativeValue::Time);
constexpr auto NativeWindowValues = static_cast<std::uint16_t>(NativeValueBit(NativeValue::WindowWidth) |
    NativeValueBit(NativeValue::WindowHeight) | NativeValueBit(NativeValue::EffectiveDpi));
constexpr auto NativeRenderTargetValues = static_cast<std::uint16_t>(NativeValueBit(NativeValue::RenderTargetWidth) |
    NativeValueBit(NativeValue::RenderTargetHeight));
constexpr auto NativeResolutionValues = static_cast<std::uint16_t>(NativeValueBit(NativeValue::ResolutionWidth) |
    NativeValueBit(NativeValue::ResolutionHeight));
constexpr auto NativePhaseValues = static_cast<std::uint16_t>(NativeWindowValues | NativeRenderTargetValues |
    NativeResolutionValues);
constexpr std::uint16_t NativeCursorValues = NativeValueBit(NativeValue::CursorWindowX) |
    NativeValueBit(NativeValue::CursorWindowY) | NativeValueBit(NativeValue::CursorHotspotX) |
    NativeValueBit(NativeValue::CursorHotspotY) | NativeValueBit(NativeValue::CursorShowing);
void NativeFact(NativePresentValues& out, NativeValue value, float number) noexcept
{
    out.values[static_cast<unsigned>(value)] = number; out.known |= NativeValueBit(value);
}
bool WindowIdentity(HWND window, DWORD owner) noexcept
{
    DWORD process = 0;
    return window && owner && IsWindow(window) &&
        GetWindowThreadProcessId(window, &process) == owner && process == GetCurrentProcessId();
}
bool NativeWindowIdentity(IDXGISwapChain* chain, HWND& window, DWORD& owner) noexcept
{
    DXGI_SWAP_CHAIN_DESC desc{};
    if (!chain || FAILED(chain->GetDesc(&desc)) || !desc.OutputWindow) return false;
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(desc.OutputWindow, &process);
    if (process != GetCurrentProcessId() || !WindowIdentity(desc.OutputWindow, thread)) return false;
    window = desc.OutputWindow; owner = thread; return true;
}
void SampleNativeWindow(HWND window, DWORD owner, std::uint16_t required, NativePresentValues& out) noexcept
{
    if (!(required & NativeWindowValues) || !WindowIdentity(window, owner)) return;
    constexpr auto dimensions = NativeValueBit(NativeValue::WindowWidth) | NativeValueBit(NativeValue::WindowHeight);
    if (required & dimensions)
    {
        // Preserve the calling thread's real client-coordinate DPI context.
        // M12 exposes successful client samples, including small/zero sizes, so
        // the original INI's WINDOW -> RT -> RES -> variable branches decide.
        // Do not apply XXMI's separate heuristic RES/RT/config substitution here.
        RECT client{};
        if (GetClientRect(window, &client) && !client.left && !client.top && client.right >= 0 && client.bottom >= 0)
        {
            if (required & NativeValueBit(NativeValue::WindowWidth))
                NativeFact(out, NativeValue::WindowWidth, static_cast<float>(client.right));
            if (required & NativeValueBit(NativeValue::WindowHeight))
                NativeFact(out, NativeValue::WindowHeight, static_cast<float>(client.bottom));
        }
    }
    if (required & NativeValueBit(NativeValue::EffectiveDpi))
    {
        const auto dpi = [&]() noexcept -> UINT {
            const auto monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
            if (!monitor) return 0;
            // Same successful API path as fixed util.cpp:get_effective_dpi,
            // without a loader, process-awareness change or synthetic 96 DPI.
            const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
            if (!previous) return 0;
            UINT x = 0, y = 0;
            const HRESULT queried = GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y);
            const auto restored = SetThreadDpiAwarenessContext(previous); // restore on query failure too
            return restored && SUCCEEDED(queried) && x >= 96 && y ? x : 0;
        }();
        if (dpi) NativeFact(out, NativeValue::EffectiveDpi, static_cast<float>(dpi));
    }
    // A failed DPI query does not erase a real client sample, or vice versa.
    if (!WindowIdentity(window, owner)) out.known &= static_cast<std::uint16_t>(~NativeWindowValues);
}
void SampleNativeRenderTarget(ID3D11DeviceContext* context, ID3D11Device* device,
                              std::uint16_t required, NativePresentValues& out) noexcept
{
    if (!(required & NativeRenderTargetValues) || !context || !device) return;
    ComPtr<ID3D11RenderTargetView> view;
    context->OMGetRenderTargets(1, view.GetAddressOf(), nullptr); // authentic OM slot 0, not the backbuffer
    D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
    if (view) view->GetDesc(&viewDesc);
    // XXMI CommandListState starts at -1; ProcessParamRTSize leaves it there
    // for known-null OM or non-TEXTURE2D/MS views. This explicit absence can
    // reach the INI's RES branch. Failed resource/device queries stay UNKNOWN.
    if (!view || (viewDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D &&
                  viewDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2DMS))
    {
        NativeFact(out, NativeValue::RenderTargetWidth, -1.0f);
        NativeFact(out, NativeValue::RenderTargetHeight, -1.0f);
        return;
    }
    ComPtr<ID3D11Resource> resource; view->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture)) || !texture) return;
    ComPtr<ID3D11Device> actualDevice; texture->GetDevice(actualDevice.GetAddressOf());
    if (actualDevice.Get() != device) return;
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    if (!desc.Width || !desc.Height || desc.ArraySize != 1 || !desc.MipLevels) return;
    if (viewDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D)
    {
        if (desc.SampleDesc.Count != 1 || viewDesc.Texture2D.MipSlice >= desc.MipLevels) return;
    }
    else if (desc.SampleDesc.Count <= 1 || desc.MipLevels != 1) return;
    // Fixed XXMI CommandList.cpp:3078-3079 returns RESOURCE base dimensions.
    // A nonzero view mip must not shrink these operands; MSAA is not a scale.
    NativeFact(out, NativeValue::RenderTargetWidth, static_cast<float>(desc.Width));
    NativeFact(out, NativeValue::RenderTargetHeight, static_cast<float>(desc.Height));
}
void SampleNativeResolution(IDXGISwapChain* chain, ID3D11Device* device,
                            std::uint16_t required, NativePresentValues& out) noexcept
{
    if (!(required & NativeResolutionValues) || !chain || !device) return;
    // Same chain as the real native call. Its current buffer resolves automatic
    // (zero) creation/ResizeBuffers arguments and cannot borrow another chain's
    // capture-wide last size. No content read; release the temporary COM
    // reference before interpreting/submitting or entering native Present.
    ComPtr<ID3D11Texture2D> buffer;
    if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(buffer.GetAddressOf()))) || !buffer) return;
    ComPtr<ID3D11Device> actualDevice; buffer->GetDevice(actualDevice.GetAddressOf());
    if (actualDevice.Get() != device) return;
    D3D11_TEXTURE2D_DESC desc{}; buffer->GetDesc(&desc);
    if (!desc.Width || !desc.Height) return;
    NativeFact(out, NativeValue::ResolutionWidth, static_cast<float>(desc.Width));
    NativeFact(out, NativeValue::ResolutionHeight, static_cast<float>(desc.Height));
}

std::uint64_t Ids(std::uint64_t count = 1)
{
    count = (std::max)(count, std::uint64_t(1));
    auto id = nextId.load(std::memory_order_relaxed);
    for (unsigned attempt = 0; attempt < 64; ++attempt)
    {
        if (!id || count >= UINT64_MAX - id) throw std::overflow_error("GIMI lease domain exhausted");
        if (nextId.compare_exchange_weak(id, id + count, std::memory_order_relaxed)) return id;
    }
    throw std::runtime_error("GIMI creation identity contention");
}
bool GenshinProcess() noexcept
{
    // Executable identity only, evaluated outside any draw. No routing by DLL
    // name, active feature label or a stale launcher selection.
    static const bool result = [] {
        wchar_t path[32768]{}; const auto n = GetModuleFileNameW(nullptr, path, 32768);
        if (!n || n >= 32768) return false;
        const auto* name = wcsrchr(path, L'\\'); name = name ? name + 1 : path;
        return !_wcsicmp(name, L"YuanShen.exe") || !_wcsicmp(name, L"GenshinImpact.exe");
    }();
    return result;
}
bool Same(Binding a, Binding b) noexcept
{
    return a.object == b.object && a.view == b.view && a.generation == b.generation &&
        a.stride == b.stride && a.offset == b.offset && a.format == b.format && a.byteSize == b.byteSize;
}
std::size_t Bucket(std::uint64_t v, std::size_t capacity) noexcept
{
    v ^= v >> 33; v *= 0xff51afd7ed558ccdULL; v ^= v >> 33;
    return static_cast<std::size_t>(v) & (capacity - 1);
}
// Fixed open-addressed tables. Pointer keys ONLY locate a retained COM object;
// the public Binding/Identity always carries a separately allocated lease ID.
template<class T, std::size_t N> struct Table
{
    struct Slot { std::uint64_t key = 0; std::shared_ptr<T> value; };
    std::array<Slot, N> slots{};
    std::shared_ptr<T> Find(std::uint64_t key) const noexcept
    {
        if (!key) return {};
        auto at = Bucket(key, N);
        for (std::size_t i = 0; i < 32; ++i, at = (at + 1) & (N - 1))
        {
            if (!slots[at].key) return {};
            if (slots[at].key == key) return slots[at].value;
        }
        return {};
    }
    bool Add(std::uint64_t key, std::shared_ptr<T> value) noexcept
    {
        if (!key) return false;
        auto at = Bucket(key, N);
        for (std::size_t i = 0; i < 32; ++i, at = (at + 1) & (N - 1))
            if (!slots[at].key) { slots[at] = {key, std::move(value)}; return true; }
            else if (slots[at].key == key) return slots[at].value == value;
        return false;
    }
};
template<class T> std::uint64_t Pointer(T* p) noexcept { return reinterpret_cast<std::uintptr_t>(p); }

// Non-owning exact-key prefilter for already registered resources. No getter,
// owner lock or allocation for unrelated Maps. Stale keys only cause a bounded
// registry recheck; they NEVER authorize access/lifetime. Process-lifetime keys
// avoid reset races; fixed capacity exhaustion fails the component closed.
constexpr std::size_t MapKeyCapacity = 8192;
std::array<std::atomic<std::uint64_t>, MapKeyCapacity> mapKeys{};
bool RegisterMapResource(ID3D11Resource* resource) noexcept
{
    const auto key = Pointer(resource); auto at = Bucket(key, MapKeyCapacity);
    for (unsigned probe = 0; probe < 32; ++probe, at = (at + 1) & (MapKeyCapacity - 1))
    {
        auto found = mapKeys[at].load(std::memory_order_acquire);
        if (found == key) return true;
        if (!found && mapKeys[at].compare_exchange_strong(found, key, std::memory_order_acq_rel)) return true;
        if (found == key) return true;
    }
    return false;
}
bool MapResourceRegistered(ID3D11Resource* resource) noexcept
{
    if (!Enabled() || !mapCaptureLive.load(std::memory_order_acquire) || !resource) return false;
    const auto key = Pointer(resource); auto at = Bucket(key, MapKeyCapacity);
    for (unsigned probe = 0; probe < 32; ++probe, at = (at + 1) & (MapKeyCapacity - 1))
    {
        const auto found = mapKeys[at].load(std::memory_order_acquire);
        if (!found) return false;
        if (found == key) return true;
    }
    return false;
}

// Dedicated GIMI CPU authority: no Genshin payload/lease/hash authorizes it.
// All storage is reserved at creation, bounded independently of IA snapshots.
constexpr std::size_t ConstantCpuBudget = 8u * 1024u * 1024u;
constexpr UINT ConstantByteLimit = 65536;
std::atomic<unsigned> constantMapCount{0};
struct alignas(16) ConstantBlock { std::array<std::uint8_t, 16> data; };
struct ConstantContent
{
    std::vector<ConstantBlock> bytes, staging;
    std::uint64_t revision = 1, published = 0, generation = 0;
    std::uint64_t mapRevision = 0, mapContract = 0;
    DWORD mapThread = 0;
    ID3D11DeviceContext* context = nullptr; // retained by Capture
    UINT begin = 0, end = 0;
    bool pending = false;
    std::uint8_t* Data() noexcept { return reinterpret_cast<std::uint8_t*>(bytes.data()); }
    std::uint8_t* Staging() noexcept { return reinterpret_cast<std::uint8_t*>(staging.data()); }
    void Revoke() noexcept { begin = end = 0; published = mapRevision = mapContract = 0; mapThread = 0; pending = false; if (revision != UINT64_MAX) ++revision; }
};
// SEH is isolated from C++ owners. Bad CPU input never publishes partial bytes.
bool CopyConstantCpu(void* destination, const void* source, std::size_t bytes) noexcept
{
    if (!destination || !source || !bytes || bytes > ConstantByteLimit) return false;
    __try { std::memcpy(destination, source, bytes); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
struct GameResource final : Dx11GameLease
{
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11Texture2D> texture;
    IdentityRecord record;
    DescriptorMatches descriptorMatches; // plan/object/device/descriptor keyed, prepared off-draw
    std::vector<std::uint8_t> cpu; // creation-time bytes only; never resized in a draw
    std::unique_ptr<ConstantContent> constant;
    // A creation-time CPU identity, immutable storage OR write-observed
    // DEFAULT IA input. Revoked on every later write/map, never rehashed in draw.
    bool capturedInput = false;
    bool gpuBuffer = false, gpuOrdered = false, soCursorKnown = false;
    std::uint64_t soCursorUpper = 0, gpuRevision = 1;
    D3D11_BUFFER_DESC originalBufferDesc{};
    BufferResizeRule resize;
    bool vertexReadOnly = false, mapped = false;
    D3D11_BUFFER_DESC bufferDesc{};
    D3D11_TEXTURE2D_DESC textureDesc{};
    std::uint64_t readObject = 0, readGeneration = 0;
};
struct GameView final : Dx11GameLease
{
    std::shared_ptr<GameResource> resource;
    ComPtr<ID3D11ShaderResourceView> srv;
    std::uint64_t id = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
struct OutputResource
{
    ComPtr<ID3D11Texture2D> texture;
    IdentityRecord descriptor; // actual GetDesc, entirely separate from content/VS leases
};
struct OutputView final : Dx11GameLease
{
    std::shared_ptr<OutputResource> resource;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    Binding binding;
    OutputViewMetadata descriptor;
    DescriptorMatches matches;
};
struct OutputJob
{
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
};
struct GameUnorderedView
{
    std::shared_ptr<GameResource> resource;
    ComPtr<ID3D11UnorderedAccessView> view;
    D3D11_UNORDERED_ACCESS_VIEW_DESC descriptor{};
};
ResourceIdentity GameIdentity(const GameResource& game) noexcept
{
    auto out = game.record.Snapshot();
    if (game.gpuBuffer)
    {
        out.buffer = game.bufferDesc;
        // This SO slice does not certify concurrent CS/PS UAV writers. UAV
        // creation/resize is real, but such storage cannot become a readable
        // identity until exact bound-UAV GPU write tracking is implemented.
        out.gpuOrdered = game.gpuOrdered && !game.mapped && !(game.bufferDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS);
        out.contentKnown = false; out.contentGeneration = game.gpuRevision;
    }
    return out;
}
struct Shader
{
    ComPtr<ID3D11DeviceChild> shader;
    ShaderIdentity identity;
    bool vertex = false, plain = false, iniParameterSlotUnused = false;
    ShaderModel model = ShaderModel::Unknown; // authentic creation reflection only
    std::shared_ptr<const ShaderSource> source; // original bytes/signature, never a replacement
    ShaderSourceStatus sourceStatus = ShaderSourceStatus::Unknown;
    Dx11VertexInputs inputs;
    Dx11PixelInputs pixelInputs;
    Dx11StreamOutputContract stream;
};
struct Layout
{
    ComPtr<ID3D11InputLayout> layout;
    std::uint64_t id = 0;
    std::vector<LayoutElement> elements;
    std::uint32_t slots = 0;
    bool exact = false;
};
struct Capture
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext1> rangeContext;
    std::uint64_t generation = 0, invalidation = 1, vertexReadRevision = 1;
    bool coverage = false, creations = false, exhausted = false, opaque = false;
    std::uint64_t creationAttempt = 0;
    LONG creationResult = NO_ERROR;
    HookServices hooks;
    HashPolicy hashPolicy;
    std::size_t cpuBytes = 0, constantCpuBytes = 0;
    std::size_t shaderSourceBytes = 0; // separate bounded creation payload; no resource-budget regression
    Table<GameResource, ResourceCapacity> resources, resourceIds;
    Table<GameView, ViewCapacity> views, viewIds;
    Table<GameUnorderedView, ViewCapacity> unorderedViews;
    Table<OutputResource, ResourceCapacity> outputResources;
    Table<OutputView, ViewCapacity> outputViews, outputViewIds;
    std::vector<BufferResizeRule> resizeRules; // Present-only publication
    std::uint64_t resizeRevision = 0, streamRevision = 1;
    bool streamCoverage = false;
    std::array<std::shared_ptr<GameResource>, D3D11_SO_BUFFER_SLOT_COUNT> streamTargets{};
    Table<Shader, ShaderCapacity> shaders;
    Table<Layout, LayoutCapacity> layouts;
    void ObserveResolution(UINT width, UINT height) noexcept
    {
        if (!width || !height) return;
        hashPolicy.resolutionKnown = true; hashPolicy.screenWidth = width; hashPolicy.screenHeight = height;
        // Future creation callbacks use this actual swap-chain observation.
        // Earlier texture identities are not rehashed against a later size.
    }
};
// Process-lifetime map owner, independent of active Session/Capture. Once a
// pointer is returned to the app, config-off/reset cannot release its storage
// or suppress the matching original writeback. Fixed lookup, no resource scan.
struct ConstantMap
{
    std::weak_ptr<Capture> capture; // map retains only its exact resource/context, not the whole registry
    std::shared_ptr<GameResource> resource;
    ComPtr<ID3D11DeviceContext> context;
    void* driver = nullptr;
    const void* cpu = nullptr;
    std::uint64_t revision = 0, contract = 0;
    DWORD thread = 0;
    bool writeback = false;
};
struct ConstantMaps
{
    std::mutex mutex;
    std::array<ConstantMap, 32> slots{};
};
ConstantMaps& ConstantMapStore() { static auto* maps = new ConstantMaps; return *maps; }
struct Pipeline final : Dx11PipelineOwner
{
    std::shared_ptr<Layout> input;
    std::shared_ptr<Shader> vs, ps;
    ComPtr<ID3D11DeviceContext1> rangeContext;
    std::shared_ptr<const LayoutContract> contract;
    std::unique_ptr<DrawCache> cache;
    QualifiedLayout qualified;
    std::array<std::uint32_t, GimiVertexSlots> strides{};
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    std::shared_ptr<Shader> gs;
    ShaderEntryLink vertexEntry, pixelEntry;
    std::shared_ptr<const Dx11PixelReplacement> pixelReplacement;
    std::shared_ptr<const Dx11VertexReplacement> vertexReplacement;
    std::array<ProgramInvocation, GimiCachedMatches> programs{};
    std::uint32_t programCount = 0; // VS roots, then PS roots; POST same order
};
struct Witness
{
    bool pending = false;
    std::shared_ptr<Layout> layout;
    std::shared_ptr<Shader> vs, ps;
    std::array<std::uint32_t, GimiVertexSlots> strides{};
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    std::shared_ptr<Shader> gs;
    bool observerOnly = false;
};
struct WindowJob
{
    bool valid = false;
    Dx11LeaseView value;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT offset = 0, first = 0, count = 0;
};
bool SameWindow(const WindowJob& a, const WindowJob& b) noexcept
{
    return a.valid && b.valid && a.value.identity.object == b.value.identity.object &&
        a.value.identity.contentGeneration == b.value.identity.contentGeneration &&
        a.format == b.format && a.offset == b.offset && a.first == b.first && a.count == b.count;
}
struct Session;
struct State
{
    std::mutex requestMutex, ownerMutex;
    Selection selection;
    std::uint64_t revision = 1, loaded = 0;
    std::uint64_t nativePresentTicket = 0; // one serial interpreter; owner lock never spans the original
    std::uint64_t observedRasterTicket = 0; // CPU observation only; owned by the outer original hook
    std::shared_ptr<Capture> capture;
    std::shared_ptr<Session> active;
    std::array<std::shared_ptr<Session>, RetiredCapacity> retired{};
    Observation observation;
    std::atomic_bool coverageLost{false};
    void Message(const char* text) noexcept { std::snprintf(observation.message, sizeof(observation.message), "%s", text); }
    void DiagnosticMessage(const std::vector<Diagnostic>& diagnostics)
    {
        if (diagnostics.empty()) { Message("Unsupported: component preparation failed"); return; }
        const auto& d = diagnostics.front(); const auto file = PathUtf8(d.source.file);
        std::snprintf(observation.message, sizeof(observation.message), "%s:%u: %s", file.c_str(), d.source.line, d.message.c_str());
    }
};
State& Store()
{
    // Intentional process-lifetime owner. On DLL shutdown/no future Present,
    // unresolved/quarantined GPU references are NOT released on CPU teardown.
    // Normal completed batches do release below after an authentic fence value.
    static State* state = new State;
    return *state;
}

struct Session final : Dx11ResourceProvider, Dx11DrawOwner
{
    State& root;
    std::shared_ptr<Capture> capture;
    std::shared_ptr<const Plan> plan;
    RasterObservers observers;
    std::uint64_t observedContract = 0;
    std::shared_ptr<const ShaderRegexLibrary> shaderTools;
    std::shared_ptr<const FormatTextLibrary> formatText;
    std::shared_ptr<const NativeTextShaderLibrary> textRender;
    std::shared_ptr<const Dx11StaticResources> statics;
    std::unique_ptr<CommandState> commands;
    PersistentState userState;
    std::int64_t lastStateSaveAttempt = 0;
    bool stateSaveBlocked = false;
    std::uint64_t lastStateSaveRevision = 0;
    struct InputPacket { OptiInput::KeyboardFrame frame; bool accepting = false; };
    std::array<InputPacket, 64> inputPackets{};
    std::uint32_t inputHead = 0, inputCount = 0;
    std::uint64_t lastInputGeneration = 0;
    HWND inputOwner = nullptr;
    std::array<bool, 4> chordPrevious{};
    std::array<int, 4> cycleCursors{-1, -1, -1, -1};
    std::array<std::uint8_t, 4> cycleChords{}; // compiled once: F6/F12/CtrlF12/F10
    bool rootInputDisabled = false, rootTogglePrevious = false;
    std::unique_ptr<Dx11Executor> executor;
    std::unique_ptr<NativeComputeExecutor> compute;
    Id selected = InvalidId; // first component for diagnostics; single shader eligibility
    std::vector<std::uint8_t> selectionPrograms; // immutable Load-owned callback admission
    std::vector<std::string> selectionLabels; // source locations formatted only at Load
    struct SelectedOverride
    {
        const OverrideSpec* spec = nullptr; // immutable plan-owned metadata
        std::uint32_t ingress = 0;
    };
    // Plan order keeps requirements in the interpreter's canonical order.
    // Each ingress captures its original binding once per physical invocation.
    std::array<SelectedOverride, MaxComponentSelections> selectedOverrides{};
    std::array<Target, MaxOverrideRequirements> ingresses{};
    std::uint32_t selectedCount = 0, ingressCount = 0;
    std::vector<ProgramInvocation> presentPrograms; // immutable, real global roots
    PresentValueRequirements presentValues{}; // all branches, per PHYSICAL native phase
    float drawFrameTime = 0; // not available until this Session's real native PRE
    bool drawFrameTimeKnown = false;
    bool presentCompute = false; // one PRE GPU packet, POST remains CPU-only
    std::uint32_t parameterState = 0;
    bool shaderSelection = false;
    std::uint64_t generation = 0, consumer = 0, invocation = 0, signal = 0, aliasEpoch = 0;
    DWORD thread = 0;
    bool accepting = true, quarantined = false;
    ComPtr<ID3D11Fence> fence;
    ComPtr<ID3D11DeviceContext4> submit;
    enum class PacketState { Empty, Reserved, Unsealed, Fenced, Quarantine };
    struct Packet
    {
        std::shared_ptr<const Dx11ExecutionLease> lease;
        std::shared_ptr<NativeComputePacket> compute;
        PacketState state = PacketState::Empty;
        std::uint64_t ticket = 0, fenceValue = 0;
    };
    std::array<Packet, Flights> packets{};
    std::vector<Dx11RetainedBinding> aliases, aliasScratch;
    std::vector<std::uint64_t> aliasStamp;
    std::array<std::shared_ptr<Pipeline>, 16> pipelines{};
    Witness witness;
    // Negative preparation witnesses are exact objects/topology/strides, never
    // a fabricated shader result. A failed optional observer cannot repeatedly
    // invoke the assembler or close the selected clothing session.
    std::array<Witness, 4> observerRefusals{};
    unsigned observerRefusalCount = 0;
    std::array<OverrideRequirement, MaxOverrideRequirements> observerRequirements{};
    unsigned observerRequirementCount = 0;
    Id observerRequiredRoot = InvalidId;
    std::array<Target, RasterObservers::TargetCapacity> observerReadTargets{};
    unsigned observerReadCount = 0;
    struct WindowEntry { WindowJob key; IndexWindow window; };
    mutable std::array<WindowJob, PendingWindows> jobs{};
    std::array<WindowEntry, WindowCapacity> windows{};
    mutable UINT jobsCount = 0;
    IndexedDrawSnapshot snapshot;
    std::shared_ptr<Pipeline> currentPipeline;
    std::array<Binding, 128> pixelBindings{};
    std::array<bool, 128> pixelKnown{};
    std::array<Binding, 128> vertexTextureBindings{};
    std::array<bool, 128> vertexTextureKnown{};
    std::array<bool, GimiVertexSlots> vertexKnown{};
    bool indexKnown = false;
    // Fixed count and byte budget; recycling requires all packet/alias owners
    // to have retired. Allocation is solely in PreparePrivateTextures at Present.
    static constexpr std::size_t PrivateTextureSlots = PrivateTextureCapacity;
    static constexpr std::uint64_t PrivateTextureBudget = PrivateTextureByteBudget;
    std::array<std::shared_ptr<Dx11OwnedTextureLease>, PrivateTextureSlots> privateTextures{};
    // Rebuilt only at Present; draw resolves at most log2(capacity) numeric keys.
    std::array<std::pair<std::uint64_t, unsigned>, PrivateTextureSlots> privateTextureIndex{};
    unsigned privateTextureIndexCount = 0;
    struct PrivateTextureJob
    {
        Dx11OutputBinding source;
        OwnedTexturePurpose purpose = OwnedTexturePurpose::DepthCopy;
        Id alias = InvalidId;
    };
    mutable std::array<PrivateTextureJob, PrivateTextureSlots> privateTextureJobs{};
    mutable std::array<OutputJob, 128> outputJobs{}; // direct-mapped witnesses; collision stays UNKNOWN
    mutable std::array<Binding, 9> outputBindings{};
    mutable std::array<bool, 9> outputKnown{};
    std::uint64_t drawInvalidation = 0, drawVertexReadRevision = 0;

    explicit Session(State& state, std::shared_ptr<Capture> store) : root(state), capture(std::move(store)) {}
    void PublishUserState() noexcept
    {
        auto& o = root.observation;
        o.stateDirty = userState.Dirty(); o.stateBlocked = stateSaveBlocked;
        o.stateLoadedUtc = userState.LastLoadedUtc(); o.stateSavedUtc = userState.LastSavedUtc();
        o.stateImported = userState.Imported();
    }
    bool SaveUserState(bool force, std::uint64_t requestRevision)
    {
        if (!commands || !commands->CanInitialize()) return false;
        userState.PollDirty(*commands);
        if (stateSaveBlocked && (!force || lastStateSaveRevision == requestRevision))
        { PublishUserState(); return false; }
        const auto now = NativeTicks();
        const auto interval = plan->Configuration()->host.saveIntervalSeconds;
        const bool timed = interval >= 0 && (!lastStateSaveAttempt ||
            now - lastStateSaveAttempt >= static_cast<std::int64_t>(interval) * 1'000'000);
        const bool due = userState.InitialSave() || timed;
        if (userState.Dirty() && (force || due))
        {
            lastStateSaveAttempt = now; lastStateSaveRevision = requestRevision;
            std::string error;
            if (!userState.Save(*commands, error))
            {
                stateSaveBlocked = true; accepting = false;
                root.Message(error.c_str()); PublishUserState(); return false;
            }
            stateSaveBlocked = false;
        }
        PublishUserState(); return true;
    }
    void QueueInput(const OptiInput::KeyboardFrame& frame, bool allow) noexcept
    {
        if (plan->KeyCycles().empty() || !accepting || stateSaveBlocked) return;
        // A snapshot belongs to this interpreter's authentic serial caller and
        // one stable game/input root, not an arbitrary foreground app.
        if (!frame.valid || !frame.generation || frame.thread != thread ||
            (inputOwner && inputOwner != frame.owner))
        {
            accepting = false;
            root.Message("Unsupported: GIMI input frame/owner changed; reload required"); return;
        }
        if (frame.generation <= lastInputGeneration) return;
        if (inputCount == inputPackets.size())
        {
            accepting = false; ++root.observation.inputRejected;
            root.Message("Unsupported: GIMI input queue overflow across interpreter pair; no edge replay"); return;
        }
        inputOwner = frame.owner; lastInputGeneration = frame.generation;
        inputPackets[(inputHead + inputCount) % inputPackets.size()] = {frame, allow}; ++inputCount;
        ++root.observation.inputSnapshots;
    }
    void ConsumeInput() noexcept
    {
        if (!accepting || root.nativePresentTicket || !commands->CanInitialize()) return;
        while (inputCount)
        {
            const auto packet = inputPackets[inputHead];
            inputHead = (inputHead + 1) % static_cast<std::uint32_t>(inputPackets.size()); --inputCount;
            // XXMI focus/disable skips preserve action last_state. F6 uses its
            // own root-HWND focus and suppresses core keys while its menu/key
            // capture is active: explicit product semantics, not shared XXMI.
            if (!packet.frame.focused || !packet.accepting) continue;
            const auto& keys = packet.frame.down;
            const bool ctrl = keys[VK_CONTROL] || keys[VK_LCONTROL] || keys[VK_RCONTROL];
            const bool alt = keys[VK_MENU] || keys[VK_LMENU] || keys[VK_RMENU];
            const bool shift = keys[VK_SHIFT] || keys[VK_LSHIFT] || keys[VK_RSHIFT];
            const bool none = !ctrl && !alt && !shift && !keys[VK_LWIN] && !keys[VK_RWIN];
            const auto& host = plan->Configuration()->host;
            // Root input toggle is a host control, not a Key-cycle action. In
            // upstream mods scope it does not disable Core/GIMI's four cycles.
            // Required MOD-local Key sections still reject at compile time.
            if (host.toggleInput)
            {
                const bool toggle = ctrl && alt && shift && keys[VK_END];
                if (toggle && !rootTogglePrevious) rootInputDisabled = !rootInputDisabled;
                rootTogglePrevious = toggle;
            }
            if (rootInputDisabled && host.disableAllInput) continue;
            std::array<bool, 4> down{};
            for (unsigned i = 0; i < down.size(); ++i)
                switch (cycleChords[i])
                {
                case 0: down[i] = none && keys[VK_F6]; break;
                case 1: down[i] = none && keys[VK_F12]; break;
                case 2: down[i] = ctrl && keys[VK_F12]; break; // other modifiers allowed upstream
                case 3: down[i] = none && keys[VK_F10]; break;
                default: accepting = false; return;
                }
            if (!commands->ApplyKeyCycles(down, chordPrevious, cycleCursors))
            {
                accepting = false; ++root.observation.inputRejected;
                root.Message("Unsupported: fixed GIMI input transaction failed; variables/edges rolled back"); return;
            }
            ++root.observation.inputConsumed;
        }
        userState.PollDirty(*commands); PublishUserState();
    }
    bool HasPackets() const noexcept
    { for (const auto& p : packets) if (p.state != PacketState::Empty) return true; return false; }
    void Quarantine(const char* reason) noexcept
    {
        accepting = false; quarantined = true;
        for (auto& p : packets) if (p.state != PacketState::Empty) p.state = PacketState::Quarantine;
        if (this == root.active.get()) root.Message(reason);
    }
    void RetireAtPresent() noexcept
    {
        // Kept independent of accepting/enabled/plan generation. A disabled or
        // replaced plan still seals and retires already submitted real packets.
        if (!HasPackets() || quarantined) return;
        if (!fence || !submit || FAILED(capture->device->GetDeviceRemovedReason()))
        { Quarantine("Quarantine: MOD device lost; submitted leases remain owned"); return; }
        const auto completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX)
        { Quarantine("Quarantine: MOD fence reports device removal"); return; }
        for (auto& p : packets)
            if (p.state == PacketState::Fenced && p.fenceValue && p.fenceValue <= completed)
            { if (p.compute) p.compute->Retire(); p = {}; }
        bool unsealed = false;
        for (const auto& p : packets) unsealed |= p.state == PacketState::Unsealed;
        if (!unsealed) return;
        // Polling a fence is thread-safe; issuing on a retained immediate
        // context still belongs to its original serial Present thread.
        if (GetCurrentThreadId() != thread) return;
        if (signal >= UINT64_MAX - 1)
        { Quarantine("Quarantine: MOD fence value exhausted"); return; }
        const auto value = signal + 1;
        if (FAILED(submit->Signal(fence.Get(), value)))
        { Quarantine("Quarantine: first/next MOD Signal failed; real submitted owners retained"); return; }
        signal = value;
        for (auto& p : packets) if (p.state == PacketState::Unsealed)
        { p.state = PacketState::Fenced; p.fenceValue = value; }
    }
    bool Current(ID3D11Device* device, ID3D11DeviceContext* context, const DrawEpochs& e) const noexcept override
    {
        return accepting && !quarantined && device == capture->device.Get() && context == capture->context.Get() &&
            GetCurrentThreadId() == thread && e.plan == generation && e.consumer == consumer &&
            e.device == capture->generation && e.context == capture->generation && e.bindings == drawInvalidation &&
            capture->coverage && capture->creations && !capture->opaque && !capture->exhausted &&
            !root.coverageLost.load(std::memory_order_acquire) &&
            drawInvalidation == capture->invalidation && drawVertexReadRevision == capture->vertexReadRevision;
    }
    bool StreamsCurrent(const Dx11StreamOutputBinding& streams) const noexcept
    {
        if (!streams.mask) return true; // ordinary raster has no SO producer
        if (!capture->streamCoverage || streams.revision != capture->streamRevision) return false;
        for (UINT slot = 0; slot < streams.targets.size(); ++slot)
        {
            const auto& target = streams.targets[slot];
            const auto& game = capture->streamTargets[slot];
            if (!(streams.mask & (1u << slot))) { if (game) return false; continue; }
            if (!game || game.get() != target.owner.get() || game->buffer.Get() != target.buffer ||
                game->readObject != target.object || game->readGeneration != target.deviceGeneration ||
                target.deviceGeneration != capture->generation || !game->soCursorKnown || game->mapped ||
                game->soCursorUpper != target.cursorUpper || game->gpuRevision != target.contentGeneration)
                return false;
        }
        return true;
    }
    void RevokeStreamWrites(const Dx11StreamOutputBinding& streams) noexcept
    {
        // Partial/uncertain submission cannot publish a producer receipt. Keep
        // the real packet/COM owners quarantined and invalidate future readers,
        // including a replacement Session on the same Capture after reload.
        for (const auto& target : streams.targets) if (target.buffer)
        {
            const auto game = capture->resources.Find(Pointer(target.buffer));
            if (!game || game.get() != target.owner.get())
            { capture->opaque = true; continue; }
            game->gpuOrdered = false; game->soCursorKnown = false;
            if (game->gpuRevision == UINT64_MAX || capture->invalidation == UINT64_MAX ||
                capture->vertexReadRevision == UINT64_MAX) capture->exhausted = true;
            else { ++game->gpuRevision; ++capture->invalidation; ++capture->vertexReadRevision; }
        }
    }
    bool Reserve(std::shared_ptr<const Dx11ExecutionLease> lease, std::uint64_t& ticket) noexcept override
    {
        if (!accepting || !lease || lease->PlanOwner() != plan || invocation == UINT64_MAX ||
            !StreamsCurrent(lease->StreamOutput())) return false;
        if (plan->Touches().iniParameters && (lease->ParameterOwner() != statics ||
            lease->InitialParameters() != parameterState || !statics->ParameterView(lease->FinalParameters()))) return false;
        Packet* free = nullptr;
        for (auto& p : packets)
        {
            if (p.state != PacketState::Empty && p.ticket == invocation) return false;
            if (p.state == PacketState::Empty && !free) free = &p;
        }
        if (!free) return false;
        // Validate the entire ordered alias journal before publishing a slot.
        // Only aliases touched by this tape are visited; no resource-set scan.
        if (aliasEpoch == UINT64_MAX) return false;
        const auto stamp = ++aliasEpoch;
        for (UINT n = 0; n < lease->AliasCount(); ++n)
        {
            const auto& change = lease->Aliases()[n]; const auto id = change.resource;
            if (id >= aliases.size()) return false;
            const auto& before = aliasStamp[id] == stamp ? aliasScratch[id] : aliases[id];
            if (!Same(before.value.binding, change.previous.value.binding)) return false;
            aliasScratch[id] = change.next; aliasStamp[id] = stamp;
        }
        free->lease = std::move(lease); free->state = PacketState::Reserved;
        free->ticket = ticket = invocation;
        presentNeeded.store(true, std::memory_order_release);
        return true;
    }
    void Finish(std::uint64_t ticket, Dx11ExecutionOutcome result) noexcept override
    {
        for (auto& p : packets) if (p.ticket == ticket && p.state == PacketState::Reserved && p.lease)
        {
            if (result == Dx11ExecutionOutcome::Cancelled) { p = {}; return; }
            const auto& streams = p.lease->StreamOutput();
            const auto& issued = p.lease->StreamBytesIssued();
            if (result == Dx11ExecutionOutcome::Quarantine || !StreamsCurrent(streams))
            {
                RevokeStreamWrites(streams);
                p.state = PacketState::Quarantine;
                Quarantine("Quarantine: partial MOD submission or SO generation lost; output revoked, real owners retained"); return;
            }
            // Validate EVERY destination before publishing any readable output.
            // This is same-immediate-context GPU ordering, never CPU completion.
            for (UINT slot = 0; slot < streams.targets.size(); ++slot) if (issued[slot])
            {
                const auto& game = capture->streamTargets[slot];
                if (!game || game->soCursorUpper > game->bufferDesc.ByteWidth ||
                    issued[slot] > game->bufferDesc.ByteWidth - game->soCursorUpper || game->gpuRevision == UINT64_MAX ||
                    capture->invalidation == UINT64_MAX || capture->vertexReadRevision == UINT64_MAX)
                {
                    RevokeStreamWrites(streams);
                    Quarantine("Quarantine: submitted SO range/generation cannot publish; real owners retained"); return;
                }
            }
            bool wrote = false;
            for (UINT slot = 0; slot < streams.targets.size(); ++slot) if (issued[slot])
            {
                auto& game = *capture->streamTargets[slot];
                game.soCursorUpper += issued[slot]; game.gpuOrdered = true; ++game.gpuRevision; wrote = true;
            }
            if (wrote) { ++capture->invalidation; ++capture->vertexReadRevision; }
            for (UINT n = 0; n < p.lease->AliasCount(); ++n)
            {
                const auto& change = p.lease->Aliases()[n];
                aliases[change.resource] = change.next; // capacity/ordered ownership reserved above
            }
            for (UINT n = 0; n < p.lease->ResourceCount(); ++n)
                if (const auto& kept = p.lease->Resources()[n]; kept.textureWritten && kept.ownedTexture)
                { kept.ownedTexture->ordered = true; kept.ownedTexture->identity.gpuOrdered = true; }
            // Commit only after the complete PRE/original/POST transaction.
            // Quarantine/Cancel above keep the old CPU state and every real GPU owner.
            if (plan->Touches().iniParameters) parameterState = p.lease->FinalParameters();
            p.state = PacketState::Unsealed; return;
        }
        Quarantine("Quarantine: MOD reservation ownership was lost");
    }

    static bool NativeBinding(void* owner, Target target, Binding& out) noexcept
    {
        const auto* self = static_cast<const Session*>(owner); out = {};
        if (!self || !self->capture || target.index < 100 || target.index > 101 ||
            (target.kind != TargetKind::VertexTexture && target.kind != TargetKind::PixelTexture)) return false;
        ComPtr<ID3D11ShaderResourceView> actual;
        if (target.kind == TargetKind::VertexTexture) self->capture->context->VSGetShaderResources(target.index, 1, &actual);
        else self->capture->context->PSGetShaderResources(target.index, 1, &actual);
        if (!actual) return true;
        const auto view = self->capture->views.Find(Pointer(actual.Get()));
        if (!view || view->srv.Get() != actual.Get() || !view->resource || view->resource->mapped) return false;
        const auto identity = GameIdentity(*view->resource);
        if (!identity.object || identity.deviceGeneration != self->capture->generation || !view->id) return false;
        out.object = identity.object; out.view = view->id; out.format = view->format; out.generation = identity.deviceGeneration;
        return true;
    }
    bool RetainNativeReference(Binding binding, Dx11RetainedBinding& out) const noexcept override
    {
        out = {};
        if (!binding.object) return !binding.view && !binding.generation && !binding.stride && !binding.offset && !binding.format && !binding.byteSize;
        if (binding.generation != capture->generation || binding.stride || binding.offset || binding.byteSize || !binding.view) return false;
        const auto view = capture->viewIds.Find(binding.view);
        if (!view || !view->resource || view->resource->mapped || view->format != binding.format) return false;
        const auto identity = GameIdentity(*view->resource);
        if (identity.object != binding.object || identity.deviceGeneration != binding.generation) return false;
        out.game = view;
        out.value = {view->resource->resource.Get(), view->resource->buffer.Get(), view->srv.Get(), binding, identity};
        // No hash/content/stableReadOnly/sampleable claim is granted. This lease
        // can survive alias publication and restore ONLY its exact original slot.
        return true;
    }
    bool RetainNativeAlias(Id alias, Binding expected, Dx11RetainedBinding& out) const noexcept override
    {
        if (alias >= aliases.size() || !Same(aliases[alias].value.binding, expected)) return false;
        out = aliases[alias]; // actual committed owner, even after game registry eviction
        return true; // retention only; never a new sampling/content capability
    }
    static bool DrawNativeValue(void* owner, NativeValue operand, Binding, float& value) noexcept
    {
        const auto& self = *static_cast<const Session*>(owner);
        if (operand != NativeValue::Time || !self.drawFrameTimeKnown || !self.accepting || self.quarantined ||
            GetCurrentThreadId() != self.thread || self.root.coverageLost.load(std::memory_order_acquire))
        {
            self.root.Message("Unsupported: draw TIME requires this Session's authentic native Present PRE snapshot; complete tape rolled back");
            return false;
        }
        value = self.drawFrameTime; return std::isfinite(value);
    }
    static bool ReadConstant(void* context, Id slot, Id scalar, float& value) noexcept
    {
        auto& self = *static_cast<Session*>(context); auto& c = *self.capture;
        const auto fail = [&](const char* why) noexcept {
            std::snprintf(self.root.observation.message, sizeof(self.root.observation.message),
                "Unsupported: store ps-cb%u float[%u] byte[%u..%u]: %s; complete tape rolled back",
                slot, scalar, scalar * 4, scalar * 4 + 3, why); return false;
        };
        if (slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT || scalar >= ConstantByteLimit / 4 ||
            !self.accepting || self.quarantined || !c.coverage || !c.creations || c.exhausted || c.opaque ||
            self.root.coverageLost.load(std::memory_order_acquire) || GetCurrentThreadId() != self.thread)
            return fail("unknown owner/coverage/epoch");
        ComPtr<ID3D11Buffer> buffer; UINT first = 0, count = 0;
        if (c.rangeContext) c.rangeContext->PSGetConstantBuffers1(slot, 1, &buffer, &first, &count);
        else c.context->PSGetConstantBuffers(slot, 1, &buffer);
        if (!buffer) return fail("actual PS slot is null");
        const auto game = c.resources.Find(Pointer(buffer.Get()));
        if (!game || game->buffer.Get() != buffer.Get() || game->readGeneration != c.generation || !game->constant)
            return fail("actual buffer has no GIMI CPU-content owner/budget");
        const auto& cb = *game->constant;
        if (!c.rangeContext) count = game->bufferDesc.ByteWidth / 16;
        const auto relative = std::uint64_t(scalar) * 4;
        const auto offset = std::uint64_t(first) * 16 + relative;
        if (!count || relative + 4 > std::uint64_t(count) * 16 || offset + 4 > game->bufferDesc.ByteWidth)
            return fail("actual 11.1 first/count or buffer byte range excludes scalar");
        if (game->mapped || cb.pending || cb.generation != c.generation || cb.context != c.context.Get() ||
            !cb.published || cb.published != cb.revision || cb.revision == UINT64_MAX || offset < cb.begin || offset + 4 > cb.end)
            return fail("mapped/pending/invalid CPU revision or uncovered byte range");
        if (FAILED(c.device->GetDeviceRemovedReason())) return fail("actual device is removed");
        std::memcpy(&value, reinterpret_cast<const std::uint8_t*>(cb.bytes.data()) + offset, 4);
        if (!std::isfinite(value)) return fail("actual CPU scalar is non-finite");
        return true;
    }
    static bool NativeParameter(void* owner, Id scalar, float& value) noexcept
    {
        const auto* self = static_cast<const Session*>(owner);
        return self && self->statics && self->statics->ReadParameter(self->parameterState, scalar, value);
    }
    static bool NativeFilter(void* owner, const Plan& plan, const MatchQuery& query, float& value) noexcept
    {
        auto* self = static_cast<Session*>(owner);
        if (!self || self->plan.get() != &plan || query.target.kind != TargetKind::Resource) return false;
        Dx11RetainedBinding kept;
        Dx11LeaseView preloaded;
        if (!self->ResolvePrivateTexture(query.binding, kept) && !self->statics->Resolve(query.binding, preloaded)) return false;
        // Existing DrawCache treats an exact custom owner with no GAME hash as
        // bound/no match (+0), distinct from CommandState's null marker (-0).
        value = 0.0f; return true;
    }
    bool SubmitNativePost(const NativePresentValues& values, IDXGISwapChain* chain, HWND window)
    {
        const auto ticket = Ids(); // exhaustion fails before opening the POST journal
        CommandQueries queries; queries.context = this;
        queries.readParameter = NativeParameter; queries.filterIndex = NativeFilter;
        const auto prepared = commands->PrepareNativePost(values, queries);
        if (!prepared || !compute) return false;
        std::uint64_t nesting = 0;
        if (!EnterInternal(capture->context.Get(), nesting)) { commands->DiscardNativePresent(); return false; }
        struct Leave { Session& s; std::uint64_t n; ~Leave() { s.LeaveInternal(s.capture->context.Get(), n); } } leave{*this, nesting};
        if (!InternalActive(capture->context.Get())) { commands->DiscardNativePresent(); return false; }
        const char* failure = nullptr;
        auto packet = compute->Prepare(capture->context.Get(), chain, window, prepared.batch, parameterState, failure, this);
        if (!packet) { commands->DiscardNativePresent(); root.Message(failure ? failure : "Private native POST preflight failed"); return false; }
        // Use a fresh reservation: PRE may still own an unsealed packet under
        // the native-pair token. Neither real owner can be overwritten by POST.
        if (!ReserveCompute(packet, ticket)) { packet->Retire(); commands->DiscardNativePresent(); return false; }
        const auto submitted = compute->Submit(capture->context.Get(), *packet);
        if (!submitted.success || !commands->AcceptNativePresent())
        {
            FinishCompute(ticket, false, submitted.consumed); commands->DiscardNativePresent(); return false;
        }
        return FinishCompute(ticket, true, submitted.consumed);
    }
    // Called only under the same serial owner lock as the raster reservation.
    // The immutable packet and ordered alias journal are retained before the
    // first setter/upload/clear/dispatch. No GPU work occurs in this callback.
    bool ReserveCompute(std::shared_ptr<NativeComputePacket> lease, std::uint64_t ticket) noexcept
    {
        if (!accepting || quarantined || !lease || lease->Commands() != plan.get() ||
            !ticket || ticket == UINT64_MAX || aliasEpoch == UINT64_MAX) return false;
        if (lease->ReaderEpoch() && lease->ReaderEpoch() != ticket) return false;
        Packet* free = nullptr;
        for (auto& p : packets)
        {
            if (p.state != PacketState::Empty && p.ticket == ticket) return false;
            if (p.state == PacketState::Empty && !free) free = &p;
        }
        if (!free) return false;
        const auto stamp = ++aliasEpoch;
        for (UINT n = 0; n < lease->AliasCount(); ++n)
        {
            const auto& change = lease->Aliases()[n]; const auto id = change.resource;
            if (id >= aliases.size()) return false;
            const auto& before = aliasStamp[id] == stamp ? aliasScratch[id] : aliases[id];
            if (!Same(before.value.binding, change.previous.value.binding)) return false;
            aliasScratch[id] = change.next; aliasStamp[id] = stamp;
        }
        free->compute = std::move(lease); free->ticket = ticket;
        free->state = PacketState::Reserved;
        presentNeeded.store(true, std::memory_order_release);
        return true;
    }
    bool FinishCompute(std::uint64_t ticket, bool submitted, bool gpuIssued) noexcept
    {
        for (auto& p : packets) if (p.ticket == ticket && p.state == PacketState::Reserved && p.compute)
        {
            if (!submitted)
            {
                // Even an exception after a GPU setter has an uncertain packet.
                // Keep its full COM owner and all earlier unsealed owners.
                p.state = PacketState::Quarantine;
                Quarantine("Quarantine: native text partial/uncertain submission; packet retained");
                return false;
            }
            for (UINT n = 0; n < p.compute->AliasCount(); ++n)
            {
                const auto& change = p.compute->Aliases()[n];
                aliases[change.resource] = change.next;
            }
            for (UINT n = 0; n < p.compute->ClearCount(); ++n)
            {
                const auto& target = p.compute->Clears()[n].ownedTexture;
                target->ordered = true; target->identity.gpuOrdered = true;
            }
            if (p.compute->ParameterCommit()) parameterState = p.compute->FinalParameters();
            if (gpuIssued) p.state = PacketState::Unsealed;
            else { p.compute->Retire(); p = {}; } // legal null text branch, no GPU operation
            return true;
        }
            Quarantine("Quarantine: native text reservation owner lost");
        return false;
    }
    void CancelCompute(std::uint64_t ticket) noexcept
    {
        for (auto& p : packets) if (p.ticket == ticket && p.state == PacketState::Reserved && p.compute)
        { p.compute->Retire(); p = {}; return; }
        Quarantine("Quarantine: native text cancellation owner lost");
    }
    bool EnterInternal(ID3D11DeviceContext* c, std::uint64_t& token) noexcept override
    { return capture->hooks.enter && capture->hooks.enter(c, token); }
    bool InternalActive(ID3D11DeviceContext* c) const noexcept override
    { return capture->hooks.internal && capture->hooks.internal(c); }
    void LeaveInternal(ID3D11DeviceContext* c, std::uint64_t token) noexcept override
    { capture->hooks.leave(c, token); }
    bool BeforeRaster(ID3D11DeviceContext* c, UINT n, UINT first, INT base, bool original) noexcept override
    { return capture->hooks.before && capture->hooks.before(c, n, first, base, original); }
    bool AfterRaster(ID3D11DeviceContext* c, UINT n, UINT first, INT base, bool original) noexcept override
    { return capture->hooks.after && capture->hooks.after(c, n, first, base, original); }
    bool SupportsIndexedInstanced() const noexcept override
    { return capture->hooks.drawIndexedInstanced && capture->hooks.beforeIndexedInstanced && capture->hooks.afterIndexedInstanced; }
    bool BeforeIndexedInstanced(ID3D11DeviceContext* c, UINT n, UINT instances, UINT first, INT base, UINT firstInstance, bool original) noexcept override
    { return capture->hooks.beforeIndexedInstanced && capture->hooks.beforeIndexedInstanced(c, n, instances, first, base, firstInstance, original); }
    bool AfterIndexedInstanced(ID3D11DeviceContext* c, UINT n, UINT instances, UINT first, INT base, UINT firstInstance, bool original) noexcept override
    { return capture->hooks.afterIndexedInstanced && capture->hooks.afterIndexedInstanced(c, n, instances, first, base, firstInstance, original); }
    bool SupportsRaster(bool indexed) const noexcept override
    { return indexed ? (capture->hooks.before && capture->hooks.after) :
        (capture->hooks.beforeDraw && capture->hooks.afterDraw); }
    bool BeforeDraw(ID3D11DeviceContext* c, UINT count, UINT first, bool original) noexcept override
    { return capture->hooks.beforeDraw && capture->hooks.beforeDraw(c, count, first, original); }
    bool AfterDraw(ID3D11DeviceContext* c, UINT count, UINT first, bool original) noexcept override
    { return capture->hooks.afterDraw && capture->hooks.afterDraw(c, count, first, original); }
    bool StableForInvocation(const Dx11PipelineBinding& pipeline) const noexcept override
    { return capture->coverage && capture->creations && Current(capture->device.Get(), capture->context.Get(), pipeline.epochs); }

    bool RetainStreamOutput(Dx11StreamOutputBinding& out) const noexcept override
    {
        out = {};
        if (!capture->streamCoverage || !Current(capture->device.Get(), capture->context.Get(), snapshot.epochs)) return false;
        out.revision = capture->streamRevision;
        for (UINT slot = 0; slot < capture->streamTargets.size(); ++slot)
        {
            const auto& game = capture->streamTargets[slot];
            if (!game) continue;
            if (!game->gpuBuffer || !game->soCursorKnown || game->mapped ||
                game->readGeneration != capture->generation || game->soCursorUpper > game->bufferDesc.ByteWidth) return false;
            out.mask |= 1u << slot;
            out.targets[slot] = {game, game->buffer.Get(), game->bufferDesc, game->readObject,
                game->readGeneration, game->soCursorUpper, game->gpuRevision};
        }
        return true;
    }
    bool RetainVertexRead(ID3D11Resource* resource, ID3D11ShaderResourceView* view,
                          Dx11VertexReadLease& out) const noexcept override
    {
        out = {};
        if (!resource || !Current(capture->device.Get(), capture->context.Get(), snapshot.epochs)) return false;
        const auto game = capture->resources.Find(Pointer(resource));
        if (!game || game->resource.Get() != resource || !(game->vertexReadOnly || (game->gpuBuffer && GameIdentity(*game).gpuOrdered && capture->streamCoverage)) || game->mapped ||
            !game->readObject || game->readGeneration != capture->generation) return false;
        for (const auto& output : capture->streamTargets) if (output == game) return false;
        out.resource = game->resource.Get(); out.buffer = game->buffer.Get(); out.texture = game->texture.Get();
        out.object = game->readObject; out.generation = game->readGeneration; out.gpuOrdered = game->gpuBuffer && GameIdentity(*game).gpuOrdered;
        out.bufferDesc = game->bufferDesc; out.textureDesc = game->textureDesc;
        if (view)
        {
            const auto actual = capture->views.Find(Pointer(view));
            if (!actual || actual->srv.Get() != view || actual->resource != game) return false;
            out.srv = actual->srv.Get(); out.owner = actual;
        }
        else out.owner = game;
        return true;
    }
    bool ResourceBinding(ID3D11Resource* resource, Binding& out) const noexcept
    {
        out = {};
        if (!resource) return true;
        const auto owner = capture->resources.Find(Pointer(resource));
        if (!owner || owner->mapped) return false; // missed creation/pending map stays UNKNOWN
        const auto r = GameIdentity(*owner);
        if ((!owner->capturedInput || !r.contentKnown) && !(r.gpuOrdered && capture->streamCoverage)) return false;
        if (!r.hashKnown) return false;
        out.object = r.object; out.generation = r.deviceGeneration;
        if (r.kind == IdentityKind::Buffer) out.byteSize = r.buffer.ByteWidth;
        return true;
    }
    bool CurrentBinding(Target target, Binding& out) const noexcept override
    {
        out = {};
        switch (target.kind)
        {
        case TargetKind::Null: return true;
        case TargetKind::VertexBuffer:
        {
            if (target.index >= GimiVertexSlots) return false;
            if (!vertexKnown[target.index])
            {
                ComPtr<ID3D11Buffer> vb; UINT stride = 0, offset = 0;
                capture->context->IAGetVertexBuffers(target.index, 1, &vb, &stride, &offset);
                if (!ResourceBinding(vb.Get(), out)) return false;
                out.stride = stride; out.offset = offset;
                auto& self = *const_cast<Session*>(this);
                Dx11RetainedBinding lease;
                if (!ResolveGame(out, lease)) return false;
                self.snapshot.vertices[target.index] = out;
                self.snapshot.vertexIdentities[target.index] = lease.value.identity;
                self.vertexKnown[target.index] = true;
            }
            out = snapshot.vertices[target.index]; return true;
        }
        case TargetKind::IndexBuffer:
        {
            if (target.index) return false;
            if (!indexKnown)
            {
                ComPtr<ID3D11Buffer> ib; DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; UINT offset = 0;
                capture->context->IAGetIndexBuffer(&ib, &format, &offset);
                // Metadata admission is separate from ResolveGame's strict
                // content/read lease. Always use this invocation's real getter.
                if (ib)
                {
                    const auto owner = capture->resources.Find(Pointer(ib.Get()));
                    if (!owner || owner->buffer.Get() != ib.Get()) return false;
                    const auto identity = GameIdentity(*owner);
                    if (identity.knowledge != Knowledge::Known || identity.kind != IdentityKind::Buffer ||
                        !identity.object || !identity.descriptorGeneration || identity.deviceGeneration != capture->generation)
                        return false; // a real non-null unknown IB must not become KnownNull
                    out.object = identity.object; out.generation = identity.deviceGeneration;
                    out.byteSize = identity.buffer.ByteWidth;
                }
                out.format = format; out.offset = offset;
                Dx11RetainedBinding lease;
                if (!DescribeGameIndex(out, lease)) return false;
                auto& self = *const_cast<Session*>(this);
                self.snapshot.indexBinding = out; self.snapshot.indexIdentity = lease.value.identity;
                self.indexKnown = true;
            }
            out = snapshot.indexBinding; return true;
        }
        case TargetKind::VertexShader:
        case TargetKind::PixelShader:
        {
            const auto& sh = target.kind == TargetKind::VertexShader ? snapshot.vertexShader : snapshot.pixelShader;
            if (sh.knowledge == Knowledge::Unknown) return false;
            out.object = sh.object; out.generation = sh.generation; return true;
        }
        case TargetKind::RenderTarget:
        case TargetKind::DepthTarget:
        {
            const bool depth = target.kind == TargetKind::DepthTarget;
            if (depth ? target.index != 0 : target.index >= 8) return false;
            const auto slot = depth ? 8 : target.index;
            if (!Current(capture->device.Get(), capture->context.Get(), snapshot.epochs)) return false;
            if (outputKnown[slot]) { out = outputBindings[slot]; return true; }
            ComPtr<ID3D11RenderTargetView> rtv;
            ComPtr<ID3D11DepthStencilView> dsv;
            // OM's getter addresses RTVs from slot zero. Release ALL returned refs.
            if (depth) capture->context->OMGetRenderTargets(0, nullptr, &dsv);
            else
            {
                ID3D11RenderTargetView* actual[8]{};
                capture->context->OMGetRenderTargets(target.index + 1, actual, nullptr);
                rtv.Attach(actual[target.index]);
                for (UINT i = 0; i < target.index; ++i) if (actual[i]) actual[i]->Release();
            }
            const auto key = depth ? Pointer(dsv.Get()) : Pointer(rtv.Get());
            if (!key) { outputBindings[slot] = {}; outputKnown[slot] = true; return true; }
            const auto view = capture->outputViews.Find(key);
            if (!view)
            {
                auto& pending = outputJobs[Bucket(key, outputJobs.size())];
                const auto prior = pending.dsv ? Pointer(pending.dsv.Get()) : Pointer(pending.rtv.Get());
                if (!prior || prior == key) { pending.rtv = rtv; pending.dsv = dsv; }
                return false; // non-null unprepared, never KnownNull or a guessed descriptor
            }
            if (view->rtv.Get() != rtv.Get() || view->dsv.Get() != dsv.Get() ||
                view->descriptor.depth != depth || view->binding.generation != capture->generation) return false;
            out = view->binding; outputBindings[slot] = out; outputKnown[slot] = true; return true;
        }
        case TargetKind::PixelTexture: case TargetKind::VertexTexture:
        {
            const bool vertex = target.kind == TargetKind::VertexTexture;
            auto& self = *const_cast<Session*>(this);
            auto& known = vertex ? self.vertexTextureKnown : self.pixelKnown;
            auto& bindings = vertex ? self.vertexTextureBindings : self.pixelBindings;
            if (target.index >= known.size()) return false;
            if (known[target.index]) { out = bindings[target.index]; return true; }
            // At most once per queried stage/slot, no 128-slot resource scan.
            ComPtr<ID3D11ShaderResourceView> actual;
            if (vertex) capture->context->VSGetShaderResources(target.index, 1, &actual);
            else capture->context->PSGetShaderResources(target.index, 1, &actual);
            if (actual)
            {
                const auto view = capture->views.Find(Pointer(actual.Get()));
                if (!view || !ResourceBinding(view->resource->resource.Get(), out)) return false;
                out.view = view->id; out.format = view->format;
            }
            bindings[target.index] = out; known[target.index] = true; return true;
        }
        default: return false; // resource aliases belong to CommandState, not this provider
        }
    }
    bool ResolveGame(Binding binding, Dx11RetainedBinding& out) const noexcept override
    {
        out = {};
        if (!binding.object) { out.value.identity = ResourceIdentity::KnownNull(); return !binding.view; }
        if (Current(capture->device.Get(), capture->context.Get(), snapshot.epochs) &&
            ResolvePrivateTexture(binding, out)) return true;
        const auto owner = capture->resourceIds.Find(binding.object);
        if (!owner || owner->mapped) return false;
        const auto identity = GameIdentity(*owner);
        if (((!owner->capturedInput || !identity.contentKnown) && !(identity.gpuOrdered && capture->streamCoverage && !binding.view)) ||
            !identity.hashKnown || binding.generation != capture->generation ||
            identity.deviceGeneration != capture->generation || identity.object != binding.object) return false;
        for (const auto& output : capture->streamTargets) if (output == owner) return false;
        out.value = {owner->resource.Get(), owner->buffer.Get(), nullptr, binding, identity};
        if (binding.view)
        {
            const auto view = capture->viewIds.Find(binding.view);
            if (!view || view->resource != owner || binding.format != view->format) return false;
            out.value.srv = view->srv.Get(); out.game = view;
        }
        else out.game = owner;
        if (identity.kind == IdentityKind::Buffer && (binding.byteSize != identity.buffer.ByteWidth || binding.offset > binding.byteSize)) return false;
        out.stableReadOnly = true; return true;
    }
    bool DescribeGameIndex(Binding binding, Dx11RetainedBinding& out) const noexcept override
    {
        out = {};
        if (!Current(capture->device.Get(), capture->context.Get(), snapshot.epochs)) return false;
        if (!binding.object)
        { out.value.identity = ResourceIdentity::KnownNull(); out.value.binding = binding; return !binding.view; }
        const auto owner = capture->resourceIds.Find(binding.object);
        if (!owner || !owner->buffer || owner->resource.Get() != owner->buffer.Get()) return false;
        const auto identity = GameIdentity(*owner);
        if (identity.knowledge != Knowledge::Known || identity.kind != IdentityKind::Buffer ||
            identity.origin != ResourceOrigin::Game || !identity.descriptorGeneration ||
            identity.object != binding.object || identity.deviceGeneration != capture->generation ||
            binding.generation != capture->generation || owner->readGeneration != capture->generation ||
            binding.view || binding.stride || binding.byteSize != owner->bufferDesc.ByteWidth ||
            binding.byteSize != identity.buffer.ByteWidth || binding.offset > binding.byteSize ||
            !(identity.buffer.BindFlags & D3D11_BIND_INDEX_BUFFER)) return false;
        out.value = {owner->resource.Get(), owner->buffer.Get(), nullptr, binding, identity};
        out.game = owner; // description-only, intentionally no stableReadOnly
        return true;
    }
    bool DescribeGameOutput(Target target, Binding binding, Dx11OutputBinding& out) const noexcept override
    {
        out = {};
        const bool depth = target.kind == TargetKind::DepthTarget;
        if ((!depth && target.kind != TargetKind::RenderTarget) || (depth ? target.index != 0 : target.index >= 8) ||
            !Current(capture->device.Get(), capture->context.Get(), snapshot.epochs)) return false;
        const auto slot = depth ? 8 : target.index;
        if (!outputKnown[slot] || !Same(binding, outputBindings[slot])) return false;
        out.binding = binding;
        if (!binding.object) { out.identity = ResourceIdentity::KnownNull(); return !binding.view; }
        const auto view = capture->outputViewIds.Find(binding.view);
        if (!view || !Same(view->binding, binding) || view->descriptor.depth != depth ||
            !view->resource || !view->resource->texture) return false;
        out.owner = view; out.rtv = view->rtv.Get(); out.dsv = view->dsv.Get();
        out.texture = view->resource->texture.Get(); out.identity = view->resource->descriptor.Snapshot();
        out.view = view->descriptor;
        return out.identity.knowledge == Knowledge::Known && out.identity.kind == IdentityKind::Texture2D &&
            out.identity.object == binding.object && out.identity.deviceGeneration == capture->generation;
    }
    bool ResolveOutputOperand(Binding binding, Dx11OutputBinding& out) const noexcept override
    {
        out = {};
        if (!Current(capture->device.Get(), capture->context.Get(), snapshot.epochs) || !binding.object || !binding.view) return false;
        const auto view = capture->outputViewIds.Find(binding.view);
        if (!view || !Same(view->binding, binding) || !view->resource || !view->resource->texture) return false;
        out.owner = view; out.rtv = view->rtv.Get(); out.dsv = view->dsv.Get();
        out.texture = view->resource->texture.Get(); out.binding = binding;
        out.identity = view->resource->descriptor.Snapshot(); out.view = view->descriptor;
        return out.identity.knowledge == Knowledge::Known && out.identity.kind == IdentityKind::Texture2D &&
            out.identity.object == binding.object && out.identity.deviceGeneration == capture->generation;
    }
    bool ResolvePrivateTexture(Binding binding, Dx11RetainedBinding& out) const noexcept override
    {
        out = {};
        if (!accepting || quarantined || GetCurrentThreadId() != thread || !binding.object ||
            binding.generation != capture->generation || capture->opaque || capture->exhausted ||
            !capture->coverage || !capture->creations || root.coverageLost.load(std::memory_order_acquire)) return false;
        const auto end = privateTextureIndex.begin() + privateTextureIndexCount;
        const auto found = std::lower_bound(privateTextureIndex.begin(), end, binding.object,
            [](const auto& entry, std::uint64_t key) { return entry.first < key; });
        if (found == end || found->first != binding.object || found->second >= privateTextures.size()) return false;
        const auto& target = privateTextures[found->second];
        if (!target || (!Same(target->binding, binding) && !Same(target->outputBinding, binding)) ||
            (!target->ordered && target->purpose != OwnedTexturePurpose::OutputDescription)) return false;
        if (Same(target->outputBinding, binding))
        {
            if ((!target->rtv && !target->dsv) || !ValidatePrivateTexture(*target, capture->device.Get())) return false;
            auto& output = out.output;
            output.owner = target; output.rtv = target->rtv.Get(); output.dsv = target->dsv.Get();
            output.texture = target->texture.Get(); output.binding = binding; output.identity = target->identity;
            bool good = false;
            if (target->dsv) { D3D11_DEPTH_STENCIL_VIEW_DESC desc{}; target->dsv->GetDesc(&desc); good = DescribeOutputView(output.identity.texture, desc, output.view); }
            else { D3D11_RENDER_TARGET_VIEW_DESC desc{}; target->rtv->GetDesc(&desc); good = DescribeOutputView(output.identity.texture, desc, output.view); }
            if (!good) return false;
            output.view.view = binding.view; output.view.generation = output.identity.descriptorGeneration;
            out.value = {target->texture.Get(), nullptr, nullptr, binding, target->identity};
            out.game = target; out.ownedTexture = target; return true; // typed output only, never sampleable
        }
        out.value = {target->texture.Get(), nullptr, target->srv.Get(), binding, target->identity};
        out.value.sampleableFloat2D = true;
        out.game = target; out.ownedTexture = target; out.stableReadOnly = true; return true;
    }
    bool AcquirePrivateTexture(const Dx11OutputBinding& source, OwnedTexturePurpose purpose,
                               Id alias, Dx11RetainedBinding& out) const noexcept
    {
        out = {};
        if (!source.binding.object || !Current(capture->device.Get(), capture->context.Get(), snapshot.epochs) ||
            (purpose == OwnedTexturePurpose::DepthCopy ? !plan->Touches().depthCopy :
                (!plan->Touches().outputDescription || alias >= aliases.size() || plan->Resources()[alias].kind != ResourceKind::Alias)))
            return false;
        auto matches = [&](const Dx11OwnedTextureLease& target) {
            return target.purpose == purpose && target.alias == alias && target.source.owner == source.owner &&
                Same(target.source.binding, source.binding) && target.source.view == source.view;
        };
        // A repeated copy_desc to the same alias/description retains the original
        // storage and contents, like XXMI's destination custom-resource cache.
        if (purpose == OwnedTexturePurpose::OutputDescription)
        {
            const auto& current = aliases[alias].ownedTexture;
            if (current && matches(*current)) return ResolvePrivateTexture(current->binding, out);
        }
        const auto seat = purpose == OwnedTexturePurpose::OutputDescription ? plan->Resources()[alias].outputSlot : InvalidId;
        if (purpose == OwnedTexturePurpose::OutputDescription && seat >= OutputAliasCapacity) return false;
        const unsigned first = purpose == OwnedTexturePurpose::OutputDescription ? seat * OutputAliasGenerations : OutputTextureCapacity;
        const unsigned count = purpose == OwnedTexturePurpose::OutputDescription ? OutputAliasGenerations : DepthCopyCapacity;
        for (unsigned i = first; i < first + count; ++i)
        {
            const auto& target = privateTextures[i];
            // Availability follows real fence/alias ownership, never CPU return.
            if (!target || target.use_count() != 1 || !matches(*target)) continue;
            target->ordered = false; target->identity.gpuOrdered = false;
            out.value = {target->texture.Get(), nullptr, target->srv.Get(), target->binding, target->identity};
            out.value.sampleableFloat2D = true;
            out.game = target; out.ownedTexture = target; out.stableReadOnly = true; return true;
        }
        const auto key = source.binding.view ^ (std::uint64_t(alias) << 1) ^ static_cast<unsigned>(purpose);
        // Output aliases have distinct immutable seats, including Fakeo1/o6.
        // Depth retains its original bounded direct-map demand behavior.
        const auto jobIndex = purpose == OwnedTexturePurpose::OutputDescription ? first :
            OutputTextureCapacity + Bucket(key, DepthCopyCapacity);
        auto& job = privateTextureJobs[jobIndex];
        if (!job.source.owner || (job.source.owner == source.owner && job.purpose == purpose && job.alias == alias))
            job = {source, purpose, alias};
        presentNeeded.store(true, std::memory_order_release);
        return false; // bounded demand only; no draw-time texture/view creation
    }
    bool AcquireDepthCopy(const Dx11OutputBinding& source, Dx11RetainedBinding& out) const noexcept override
    { return AcquirePrivateTexture(source, OwnedTexturePurpose::DepthCopy, InvalidId, out); }
    bool AcquireOutputDescription(const Dx11OutputBinding& source, Id alias, Dx11RetainedBinding& out) const noexcept override
    { return AcquirePrivateTexture(source, OwnedTexturePurpose::OutputDescription, alias, out); }
    void PreparePrivateTextures()
    {
        if ((!plan->Touches().depthCopy && !plan->Touches().outputDescription) || quarantined || !accepting ||
            GetCurrentThreadId() != thread || !capture->hooks.enter || !capture->hooks.leave || !capture->hooks.internal) return;
        std::uint64_t nesting = 0;
        if (!EnterInternal(capture->context.Get(), nesting)) return;
        struct Leave { Session& s; std::uint64_t n; ~Leave() { s.LeaveInternal(s.capture->context.Get(), n); } } leave{*this, nesting};
        if (!InternalActive(capture->context.Get())) return;
        std::uint64_t used = 0;
        for (const auto& target : privateTextures) if (target) used += target->bytes;
        // One shared 256MiB budget, including both generations and creation's
        // temporary peak. Fixed seats prevent prewarm eviction/starvation.
        // A third live generation or exhausted depth seats remains fail-closed.
        for (auto& pending : privateTextureJobs)
        {
            auto job = std::exchange(pending, {}); const auto& source = job.source;
            if (!source.owner || source.binding.generation != capture->generation) continue;
            PrivateTextureShape shape;
            if (!DescribePrivateTexture(source, job.purpose, shape)) continue;
            const auto bytes = std::uint64_t(shape.texture.Width) * shape.texture.Height * shape.texelBytes;
            if (!bytes || bytes > PrivateTextureBudget / 2) continue;
            UINT support = 0;
            if (FAILED(capture->device->CheckFormatSupport(shape.srv.Format, &support)) ||
                (support & (D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) !=
                (D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) continue;
            const auto outputFormat = shape.rtv.Format != DXGI_FORMAT_UNKNOWN ? shape.rtv.Format : shape.dsv.Format;
            if (outputFormat != DXGI_FORMAT_UNKNOWN)
            {
                const UINT required = shape.rtv.Format != DXGI_FORMAT_UNKNOWN ? D3D11_FORMAT_SUPPORT_RENDER_TARGET : D3D11_FORMAT_SUPPORT_DEPTH_STENCIL;
                if (FAILED(capture->device->CheckFormatSupport(outputFormat, &support)) || !(support & required)) continue;
            }
            auto matches = [&](const Dx11OwnedTextureLease& target) {
                return target.purpose == job.purpose && target.alias == job.alias && target.source.owner == source.owner &&
                    Same(target.source.binding, source.binding) && target.source.view == source.view;
            };
            const auto seat = job.purpose == OwnedTexturePurpose::OutputDescription && job.alias < plan->Resources().size() ?
                plan->Resources()[job.alias].outputSlot : InvalidId;
            if (job.purpose == OwnedTexturePurpose::OutputDescription && seat >= OutputAliasCapacity) continue;
            const unsigned first = job.purpose == OwnedTexturePurpose::OutputDescription ? seat * OutputAliasGenerations : OutputTextureCapacity;
            const unsigned count = job.purpose == OwnedTexturePurpose::OutputDescription ? OutputAliasGenerations : DepthCopyCapacity;
            unsigned ready = 0;
            for (unsigned i = first; i < first + count; ++i)
                if (const auto& target = privateTextures[i]; target && target.use_count() == 1 && matches(*target)) ++ready;
            const unsigned wanted = job.purpose == OwnedTexturePurpose::DepthCopy ? 2u : 1u;
            // Prefer empty seats. Only an exclusively held old generation of
            // THIS alias (or a legacy depth seat) can then be replaced.
            for (unsigned pass = 0; pass < 2 && ready < wanted; ++pass)
            for (unsigned i = first; i < first + count; ++i)
            {
                auto& slot = privateTextures[i];
                if (ready >= wanted) break;
                if ((!pass && slot) || (slot && (slot.use_count() != 1 || matches(*slot)))) continue;
                const auto previous = slot ? slot->bytes : 0;
                if (used > PrivateTextureBudget || bytes > PrivateTextureBudget - used) continue;
                auto target = std::make_shared<Dx11OwnedTextureLease>();
                target->source = source; target->purpose = job.purpose; target->alias = job.alias; target->bytes = bytes;
                if (FAILED(capture->device->CreateTexture2D(&shape.texture, nullptr, &target->texture)) ||
                    FAILED(capture->device->CreateShaderResourceView(target->texture.Get(), &shape.srv, &target->srv))) continue;
                if (shape.rtv.Format != DXGI_FORMAT_UNKNOWN &&
                    FAILED(capture->device->CreateRenderTargetView(target->texture.Get(), &shape.rtv, &target->rtv))) continue;
                if (shape.dsv.Format != DXGI_FORMAT_UNKNOWN &&
                    FAILED(capture->device->CreateDepthStencilView(target->texture.Get(), &shape.dsv, &target->dsv))) continue;
                target->binding.object = Ids(); target->binding.view = Ids();
                target->binding.generation = capture->generation; target->binding.format = shape.srv.Format;
                if (target->rtv || target->dsv)
                {
                    target->outputBinding = target->binding;
                    target->outputBinding.view = Ids(); target->outputBinding.format = outputFormat;
                }
                auto& identity = target->identity;
                identity.knowledge = Knowledge::Known; identity.kind = IdentityKind::Texture2D;
                identity.origin = ResourceOrigin::Custom; identity.object = target->binding.object;
                identity.deviceGeneration = capture->generation;
                identity.descriptorGeneration = identity.contentGeneration = Ids();
                identity.texture = shape.texture; // creation grants no initialized content
                used = used - previous + bytes; slot = std::move(target); ++ready;
            }
        }
        privateTextureIndexCount = 0;
        for (unsigned i = 0; i < privateTextures.size(); ++i)
            if (const auto& target = privateTextures[i])
                privateTextureIndex[privateTextureIndexCount++] = {target->binding.object, i};
        std::sort(privateTextureIndex.begin(), privateTextureIndex.begin() + privateTextureIndexCount);
    }
    void PrepareOutputDescriptors()
    {
        // Sole allocation/descriptor candidate scan: existing serial Present.
        if (!plan->Touches().outputMetadata) return;
        for (auto& pending : outputJobs)
        {
            auto job = std::move(pending); pending = {};
            const auto key = job.dsv ? Pointer(job.dsv.Get()) : Pointer(job.rtv.Get());
            if (!key || capture->outputViews.Find(key)) continue;
            ComPtr<ID3D11Resource> resource;
            ComPtr<ID3D11Device> device;
            if (job.dsv) { job.dsv->GetResource(&resource); job.dsv->GetDevice(&device); }
            else { job.rtv->GetResource(&resource); job.rtv->GetDevice(&device); }
            if (!resource || device.Get() != capture->device.Get()) continue;
            ComPtr<ID3D11Texture2D> texture;
            if (FAILED(resource.As(&texture)) || !texture) continue; // Buffer/3D OM views stay unsupported
            device.Reset(); texture->GetDevice(&device);
            if (device.Get() != capture->device.Get()) continue;
            D3D11_TEXTURE2D_DESC actual{}; texture->GetDesc(&actual);
            auto retained = capture->outputResources.Find(Pointer(texture.Get()));
            if (!retained)
            {
                retained = std::make_shared<OutputResource>(); retained->texture = texture;
                if (!retained->descriptor.CreateTexture2DDescriptor(Ids(), capture->generation, actual)) continue;
                if (!capture->outputResources.Add(Pointer(texture.Get()), retained)) { capture->exhausted = true; return; }
            }
            const auto identity = retained->descriptor.Snapshot();
            auto view = std::make_shared<OutputView>(); view->resource = retained;
            view->rtv = job.rtv; view->dsv = job.dsv;
            view->descriptor.view = view->descriptor.generation = Ids();
            if (job.dsv)
            {
                D3D11_DEPTH_STENCIL_VIEW_DESC desc{}; job.dsv->GetDesc(&desc);
                if (!DescribeOutputView(actual, desc, view->descriptor)) continue;
            }
            else
            {
                D3D11_RENDER_TARGET_VIEW_DESC desc{}; job.rtv->GetDesc(&desc);
                if (!DescribeOutputView(actual, desc, view->descriptor)) continue;
            }
            view->binding.object = identity.object; view->binding.view = view->descriptor.view;
            view->binding.generation = capture->generation; view->binding.format = view->descriptor.format;
            if (!capture->outputViews.Add(key, view) || !capture->outputViewIds.Add(view->binding.view, view))
            { capture->exhausted = true; return; }
        }
        for (const auto& slot : capture->outputViews.slots)
        {
            const auto& view = slot.value;
            if (!view) continue;
            const auto identity = view->resource->descriptor.Snapshot();
            auto& cached = view->matches;
            if (cached.ready && cached.planGeneration == generation && cached.object == identity.object &&
                cached.deviceGeneration == identity.deviceGeneration && cached.descriptorGeneration == identity.descriptorGeneration &&
                cached.output == view->descriptor) continue;
            cached = PrepareTextureDescriptorMatches(*plan, generation, identity, view->descriptor);
        }
    }
    void PrepareDescriptors()
    {
        PrepareOutputDescriptors();
        PreparePrivateTextures();
        // Existing serial Present preparation, fixed retained-owner registry.
        // No resource/INI table is scanned by the draw/query callbacks.
        if (!plan->BufferDescriptorCandidates().second) return;
        for (const auto& slot : capture->resourceIds.slots)
        {
            const auto& owner = slot.value;
            if (!owner || !owner->buffer) continue;
            const auto identity = GameIdentity(*owner);
            auto& cached = owner->descriptorMatches;
            if (cached.ready && cached.planGeneration == generation && cached.object == identity.object &&
                cached.deviceGeneration == identity.deviceGeneration &&
                cached.descriptorGeneration == identity.descriptorGeneration) continue;
            cached = PrepareBufferDescriptorMatches(*plan, generation, identity);
        }
    }
    static const DescriptorMatches* Descriptors(void* self, Binding binding) noexcept
    {
        const auto& s = *static_cast<Session*>(self);
        if (!s.Current(s.capture->device.Get(), s.capture->context.Get(), s.snapshot.epochs)) return nullptr;
        if (binding.view)
        {
            const auto view = s.capture->outputViewIds.Find(binding.view);
            return view && Same(view->binding, binding) ? &view->matches : nullptr;
        }
        const auto owner = s.capture->resourceIds.Find(binding.object);
        if (!owner || !owner->buffer || binding.generation != s.capture->generation) return nullptr;
        return &owner->descriptorMatches;
    }
    bool Indices(const Dx11LeaseView& view, DXGI_FORMAT format, UINT offset, UINT first, UINT count, IndexWindow& out) const noexcept override
    {
        out = {};
        WindowJob key{true, view, format, offset, first, count};
        const auto at = Bucket(view.identity.object ^ (std::uint64_t(first) << 20) ^ count ^ offset ^ format, WindowCapacity);
        if (SameWindow(windows[at].key, key)) { out = windows[at].window; return out.known; }
        for (UINT i = 0; i < jobsCount; ++i) if (SameWindow(jobs[i], key)) return false;
        if (jobsCount < jobs.size()) jobs[jobsCount++] = key;
        // Range capture is numeric only. Bytes are read/scanned at Present.
        return false;
    }
    void PrepareWindows()
    {
        for (UINT i = 0; i < jobsCount; ++i)
        {
            const auto& job = jobs[i]; CpuImage cpu;
            ResourceIdentity identity;
            if (job.value.identity.origin == ResourceOrigin::Game)
            {
                const auto game = capture->resourceIds.Find(job.value.identity.object);
                if (!game) continue;
                identity = game->record.Snapshot(); cpu = {game->cpu.data(), game->cpu.size(), 0};
                if (identity.contentGeneration != job.value.identity.contentGeneration || !game->capturedInput) continue;
            }
            else
            {
                const auto& files = statics->Files()->Resources();
                for (Id id = 0; id < files.size(); ++id)
                {
                    Dx11LeaseView v;
                    if (statics->Get(id, v) && v.binding.object == job.value.identity.object)
                    { identity = v.identity; cpu = {files[id].bytes.data(), files[id].bytes.size(), 0}; break; }
                }
            }
            const auto at = Bucket(identity.object ^ (std::uint64_t(job.first) << 20) ^ job.count ^ job.offset ^ job.format, WindowCapacity);
            IndexWindow window;
            if (CompileIndexWindow(identity, cpu, job.format, job.offset, job.first, job.count, window))
                windows[at] = {job, window};
        }
        jobsCount = 0;
    }
    void PreparePipeline()
    {
        if (!witness.pending) return;
        auto request = std::exchange(witness, {});
        struct ObserverRefusal
        {
            Session& self; const Witness& request; bool prepared = false;
            ~ObserverRefusal()
            {
                if (!prepared && request.observerOnly && self.observerRefusalCount < self.observerRefusals.size())
                    self.observerRefusals[self.observerRefusalCount++] = request;
            }
        } observerRefusal{*this, request};
        if (!request.layout || !request.vs || !request.layout->exact || !request.vs->inputs.qualified ||
            (request.ps && !request.ps->plain) || (request.gs && (!request.gs->stream.qualified || !request.gs->inputs.qualified)))
        { if (!request.observerOnly) accepting = false; root.Message("Unsupported: selected component requires unknown layout or unsupported original VS read-input contract"); return; }
        for (auto& slot : pipelines) if (!slot)
        {
            auto pipeline = std::make_shared<Pipeline>(); pipeline->input = request.layout;
            pipeline->vs = request.vs; pipeline->ps = request.ps; pipeline->gs = request.gs; pipeline->strides = request.strides;
            pipeline->topology = request.topology;
            pipeline->rangeContext = capture->rangeContext;
            // Prepare once for the exact original shader leases. The existing
            // Present caller holds ownerMutex: never call AcquireShaderSource
            // (which takes that lock again) or run tools from a draw.
            // Never prepend PS commands before an explicit draw inside VS PRE.
            PreparedVertexShader vertexEntry;
            ShaderIdentity pixelIdentity; pixelIdentity.knowledge = Knowledge::Null;
            if (request.ps) pixelIdentity = request.ps->identity;
            PreparedPixelShader pixelEntry;
            {
                ComPtr<ID3D11PixelShader> originalPixel;
                ComPtr<ID3D11VertexShader> originalVertex;
                if (!shaderTools || FAILED(request.vs->shader.As(&originalVertex)) ||
                    (request.ps && FAILED(request.ps->shader.As(&originalPixel))))
                { if (!request.observerOnly) accepting = false; root.Message("Unsupported: original VS/PS shader/toolchain owner missing"); return; }
                std::uint64_t nesting = 0;
                if (!capture->hooks.enter || !capture->hooks.leave || !capture->hooks.internal ||
                    !EnterInternal(capture->context.Get(), nesting))
                { if (!request.observerOnly) accepting = false; root.Message("Unsupported: shader preparation requires existing internal suppression"); return; }
                struct Leave { Session& s; std::uint64_t n; ~Leave() { s.LeaveInternal(s.capture->context.Get(), n); } } leave{*this, nesting};
                if (!InternalActive(capture->context.Get()))
                { if (!request.observerOnly) accepting = false; root.Message("Unsupported: shader preparation suppression is inactive"); return; }
                vertexEntry = shaderTools->PrepareVertex(request.vs->source, request.vs->identity,
                    request.vs->model, capture->device.Get(), originalVertex.Get(), generation, Ids());
                pixelEntry = shaderTools->Prepare(request.ps ? request.ps->source : nullptr, pixelIdentity,
                    request.ps ? request.ps->model : ShaderModel::Unknown, capture->device.Get(), originalPixel.Get(), generation, Ids());
            }
            if (!vertexEntry || !pixelEntry)
            {
                if (!request.observerOnly) accepting = false;
                root.DiagnosticMessage(!vertexEntry ? vertexEntry.diagnostics : pixelEntry.diagnostics); return;
            }
            // Existing proven GS/SO passthrough remains available only with no
            // required GS override. HS/DS and unknown GS are refused at Raster.
            if (request.gs && plan->Candidates(request.gs->identity.hash, true).second)
            { if (!request.observerOnly) accepting = false; root.Message("Unsupported: bound GS has a required shader entry; automatic GS scheduling is not implemented"); return; }
            const auto total = vertexEntry.link.count + pixelEntry.link.count;
            if (total > pipeline->programs.size() || total > plan->Limits().matchesPerCheck)
            { if (!request.observerOnly) accepting = false; root.Message("Unsupported: complete VS/PS entry exceeds fixed invocation capacity"); return; }
            pipeline->vertexEntry = std::move(vertexEntry.link);
            pipeline->pixelEntry = std::move(pixelEntry.link);
            pipeline->pixelReplacement = std::move(pixelEntry.replacement);
            pipeline->vertexReplacement = std::move(vertexEntry.replacement);
            std::copy_n(pipeline->vertexEntry.programs.data(), pipeline->vertexEntry.count, pipeline->programs.data());
            std::copy_n(pipeline->pixelEntry.programs.data(), pipeline->pixelEntry.count,
                pipeline->programs.data() + pipeline->vertexEntry.count);
            pipeline->programCount = total;
            LayoutRequest layout;
            layout.contract = Ids(); layout.source = plan->Programs()[selected].source;
            layout.vertexShaderHash = request.vs->identity.hash;
            layout.elements = request.layout->elements; layout.strides = request.strides;
            auto compiled = CompileLayout(std::move(layout));
            if (!compiled) { if (!request.observerOnly) accepting = false; root.DiagnosticMessage(compiled.diagnostics); return; }
            std::vector<D3D11_INPUT_ELEMENT_DESC> actual;
            for (const auto& e : request.layout->elements)
                actual.push_back({e.semantic.c_str(), e.semanticIndex, e.format, e.slot, e.offset, D3D11_INPUT_PER_VERTEX_DATA, 0});
            pipeline->contract = std::move(compiled.layout);
            pipeline->qualified = pipeline->contract->Qualify(request.layout->id, capture->generation, actual.data(),
                static_cast<UINT>(actual.size()), request.vs->identity);
            auto cache = CreateDrawCache(plan, pipeline->contract, request.topology, generation);
            if (!cache || pipeline->qualified.knowledge != Knowledge::Known)
            { if (!request.observerOnly) accepting = false; root.DiagnosticMessage(cache.diagnostics); return; }
            pipeline->cache = std::move(cache.cache);
            const bool hasReplacement = bool(pipeline->pixelReplacement) || bool(pipeline->vertexReplacement);
            slot = std::move(pipeline); observerRefusal.prepared = true;
            root.Message(hasReplacement ?
                "Prepared typed VS/PS5.0 IniParams consumers; original input layout/signatures preserved; render acceptance pending" :
                "Ready for exact Draw/DrawIndexed with original VS inputs; full MOD/render/performance acceptance pending"); return;
        }
        if (!request.observerOnly) accepting = false; root.Message("Unsupported: selected component exceeds fixed exact-pipeline capacity");
    }
    static bool Read(void* self, Target t, Binding& b) noexcept
    { return static_cast<Session*>(self)->CurrentBinding(t, b); }
    static bool Identity(void* self, Binding b, ResourceIdentity& r) noexcept
    {
        auto& s = *static_cast<Session*>(self); Dx11LeaseView v; Dx11RetainedBinding g;
        if (s.statics->Resolve(b, v)) { r = v.identity; return true; }
        if (s.ResolveGame(b, g)) { r = g.value.identity; return true; }
        // Exact getter-derived IB/output keys get metadata fallback only.
        // These IDs remain absent from any bindable/stable-read capability.
        if (s.indexKnown && Same(b, s.snapshot.indexBinding) && s.DescribeGameIndex(b, g))
        { r = g.value.identity; return true; }
        if (b.view && s.Current(s.capture->device.Get(), s.capture->context.Get(), s.snapshot.epochs))
        {
            const auto view = s.capture->outputViewIds.Find(b.view);
            if (view && Same(view->binding, b)) { r = view->resource->descriptor.Snapshot(); return true; }
        }
        return false;
    }
    static bool ShaderQuery(void* self, Target t, Binding b, ShaderIdentity& out) noexcept
    {
        auto& s = *static_cast<Session*>(self);
        out = t.kind == TargetKind::VertexShader ? s.snapshot.vertexShader : s.snapshot.pixelShader;
        return out.knowledge != Knowledge::Unknown && out.object == b.object && out.generation == b.generation;
    }
    static bool ShaderFilter(void* self, Target target, Binding binding, float& value) noexcept
    {
        auto& s = *static_cast<Session*>(self);
        if (!s.currentPipeline || target.index ||
            (target.kind != TargetKind::VertexShader && target.kind != TargetKind::PixelShader)) return false;
        const auto& link = target.kind == TargetKind::VertexShader ?
            s.currentPipeline->vertexEntry : s.currentPipeline->pixelEntry;
        const auto& original = link.original;
        if (!link.valid || original.knowledge == Knowledge::Unknown ||
            original.object != binding.object || original.generation != binding.generation) return false;
        value = link.filterIndex; return true;
    }
    // Exact identity metadata only. CPU observation grants no layout, reader,
    // shader substitution or GPU capability. Complete siblings are returned in
    // original hash/descriptor order; CommandState rejects any unsafe sibling.
    static bool ObservationMatches(void* owner, const Plan& p, const MatchQuery& query,
                                   Id* out, std::uint32_t capacity, std::uint32_t& count) noexcept
    {
        auto& self = *static_cast<Session*>(owner); count = 0;
        if (&p != self.plan.get() || !query.draw ||
            !self.Current(self.capture->device.Get(), self.capture->context.Get(), self.snapshot.epochs)) return false;
        ResourceIdentity identity;
        if (!Identity(&self, query.binding, identity)) return false;
        if (identity.knowledge == Knowledge::Null && !query.binding.object) return true;
        if (identity.knowledge != Knowledge::Known || identity.origin != ResourceOrigin::Game ||
            identity.deviceGeneration != self.capture->generation || !identity.object ||
            identity.object != query.binding.object || query.binding.generation != identity.deviceGeneration ||
            !identity.descriptorGeneration || !identity.hashKnown || !identity.contentKnown) return false;
        constexpr UINT writes = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_STREAM_OUTPUT |
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL;
        constexpr UINT shared = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
            D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        const auto binds = identity.kind == IdentityKind::Buffer ? identity.buffer.BindFlags : identity.texture.BindFlags;
        const auto misc = identity.kind == IdentityKind::Buffer ? identity.buffer.MiscFlags : identity.texture.MiscFlags;
        if ((binds & writes) || (misc & shared) || identity.kind == IdentityKind::Unknown) return false;
        const auto candidates = p.Candidates(identity.upstreamHash, false);
        const DescriptorMatches* descriptors = nullptr;
        // Existing texture descriptor rules apply only to OM output views;
        // this narrow observer admits IB/VB/VS-SRV/PS-SRV metadata, never OM.
        const bool descriptorRules = identity.kind == IdentityKind::Buffer && p.BufferDescriptorCandidates().second != 0;
        if (descriptorRules)
        {
            descriptors = Descriptors(&self, query.binding);
            if (!descriptors || !descriptors->ready || descriptors->overflow ||
                descriptors->planGeneration != self.generation || descriptors->object != identity.object ||
                descriptors->deviceGeneration != identity.deviceGeneration ||
                descriptors->descriptorGeneration != identity.descriptorGeneration) return false;
        }
        const auto append = [&](const OverrideSpec& spec) noexcept {
            if (!query.draw->drawIndexed && (spec.hasFirstIndex || spec.hasIndexCount)) return false;
            if (!spec.MatchesDraw(query.draw->drawIndexed, query.draw->firstIndex, query.draw->indexCount)) return true;
            if (!out || count == capacity) return false;
            out[count++] = spec.program; return true;
        };
        for (std::size_t i = 0; i < candidates.second; ++i)
            if (!append(candidates.first[i])) return false;
        for (unsigned i = 0; descriptors && i < descriptors->count; ++i)
        {
            const auto* spec = p.OverrideForProgram(descriptors->programs[i]);
            if (!spec || spec->shader || !append(*spec)) return false;
        }
        return true;
    }
    static bool ObservationBinding(void* owner, Target target, Binding& binding) noexcept
    {
        auto& self = *static_cast<Session*>(owner);
        const auto end = self.observerReadTargets.begin() + self.observerReadCount;
        if (std::find(self.observerReadTargets.begin(), end, target) == end)
        {
            if (self.observerReadCount == self.observerReadTargets.size()) return false;
            self.observerReadTargets[self.observerReadCount++] = target;
        }
        return self.CurrentBinding(target, binding);
    }
    enum class ObserverIngress { None, CpuPending, Packet };
    ObserverIngress ObserveUnselected(const std::shared_ptr<Shader>& vertex,
                                     const std::shared_ptr<Shader>& pixel, RasterReturnScope* returned) noexcept
    {
        struct Probe { Target target; bool known = false, candidate = false; };
        std::array<Probe, RasterObservers::TargetCapacity> probes{};
        unsigned probeCount = 0;
        observerRequirementCount = 0; observerRequiredRoot = InvalidId; observerReadCount = 0;
        bool requirementOverflow = false;
        const auto resourceCandidate = [&](Target target) noexcept {
            for (unsigned i = 0; i < probeCount; ++i)
                if (probes[i].target == target) return probes[i].known && probes[i].candidate;
            if (probeCount == probes.size()) return false;
            auto& probe = probes[probeCount++]; probe.target = target;
            Binding binding; ResourceIdentity identity;
            if (!ObservationBinding(this, target, binding) || !Identity(this, binding, identity)) return false;
            probe.known = true;
            if (!binding.object || identity.knowledge != Knowledge::Known || !identity.hashKnown ||
                identity.origin != ResourceOrigin::Game || !observers.Resource(identity.upstreamHash)) return false;
            std::array<Id, GimiCachedMatches> matches{}; std::uint32_t count = 0;
            const MatchQuery query{target, binding, &snapshot.draw};
            if (!ObservationMatches(this, *plan, query, matches.data(), static_cast<UINT>(matches.size()), count))
            { probe.known = false; return false; }
            for (UINT i = 0; i < count; ++i)
                if ((observers.Programs()[matches[i]] & (ObserveCpuResource | ObserveVariableWrite)) == (ObserveCpuResource | ObserveVariableWrite))
                {
                    const auto* spec = plan->OverrideForProgram(matches[i]);
                    if (!spec || spec->matchKind != OverrideMatchKind::Hash) continue;
                    probe.candidate = true;
                    const auto found = std::find_if(observerRequirements.begin(),
                        observerRequirements.begin() + observerRequirementCount,
                        [&](const auto& value) { return value.program == matches[i]; });
                    if (found != observerRequirements.begin() + observerRequirementCount) continue;
                    if (observerRequirementCount == observerRequirements.size()) { requirementOverflow = true; continue; }
                    observerRequirements[observerRequirementCount++] = {matches[i], target, binding};
                }
            return probe.candidate;
        };
        std::array<ProgramInvocation, GimiCachedMatches> roots{};
        unsigned rootCount = 0; bool packet = false, overflow = false;
        if (root.loaded != contractRevision.load(std::memory_order_acquire)) return ObserverIngress::None;
        const auto append = [&](const RasterObservers::Root& root, bool exact) noexcept {
            bool hit = exact;
            for (unsigned i = 0; !hit && i < root.targetCount; ++i) hit = resourceCandidate(root.targets[i]);
            if (!hit) return;
            const auto* spec = plan->OverrideForProgram(root.program);
            if (spec && !spec->MatchesDraw(snapshot.drawIndexed, snapshot.draw.firstIndex, snapshot.draw.indexCount)) return;
            if (rootCount == roots.size() || rootCount == plan->Limits().matchesPerCheck) { overflow = true; return; }
            roots[rootCount++] = {root.program, {}}; packet |= root.packet;
        };
        // The same physical stage order as the original automatic link. Only
        // subscriptions actually triggered by an exact shader/resource run.
        for (const auto& shader : {vertex, pixel})
        {
            if (!shader || shader->identity.knowledge != Knowledge::Known || !shader->plain ||
                shader->identity.generation != capture->generation || shader->model == ShaderModel::Unknown) continue;
            const auto routes = observers.For(shader->identity.hash, shader->model);
            if (routes.exact) append(*routes.exact, true);
            if (routes.model) for (const auto& root : *routes.model) append(root, false);
        }
        if (overflow || requirementOverflow) { ++root.observation.unknown; root.Message("Unsupported: complete independent observation exceeds original root capacity"); return ObserverIngress::None; }
        if (!rootCount) return ObserverIngress::None;
        observerRequiredRoot = roots[0].program;
        std::sort(observerRequirements.begin(), observerRequirements.begin() + observerRequirementCount,
            [&](const auto& a, const auto& b) { return plan->OverrideForProgram(a.program) < plan->OverrideForProgram(b.program); });
        if (packet) return ObserverIngress::Packet; // real reader/tape preflight below, never CPU-skip GPU commands
        if (!returned || !commands->CanInitialize() || !Current(capture->device.Get(), capture->context.Get(), snapshot.epochs))
            return ObserverIngress::None;
        CommandQueries queries; queries.context = this; queries.readBinding = &ObservationBinding;
        queries.textureMatches = &ObservationMatches; queries.readPixelConstant = &ReadConstant;
        queries.nativeValue = &DrawNativeValue;
        const auto prepared = commands->PrepareDraw(roots.data(), rootCount, snapshot.draw, queries,
            observerRequiredRoot, observerRequirements.data(), observerRequirementCount, observers.Programs());
        if (!prepared)
        {
            ++root.observation.unknown;
            if (prepared.error == ExecutionError::ObservationEffect)
                root.Message("Unsupported: independent observer reached a non-CPU resource or unknown automatic effect; whole transaction rolled back");
            return ObserverIngress::None;
        }
        // CPU admission is exact: no SetParameter/bind/alias/draw/skip may be
        // discarded just to obtain variable writes. Such work needs a packet.
        if (prepared.batch.count != 1 || !prepared.batch.actions || prepared.batch.skipOriginal ||
            prepared.batch.actions[0].op != ActionOp::OriginalDrawBoundary ||
            prepared.batch.actions[0].skipOriginal || !returned->Arm(capture->context.Get(), invocation))
        {
            commands->Discard(); ++root.observation.unknown;
            root.Message("Unsupported: independent CPU observer requires the unchanged actual original boundary");
            return ObserverIngress::None;
        }
        observedContract = contractRevision.load(std::memory_order_acquire);
        root.observedRasterTicket = invocation;
        ++root.observation.matched;
        // The normal outer hook still performs Before/original/Returned/After.
        // It commits this journal only after its own original actually returns.
        return ObserverIngress::CpuPending;
    }
    bool Raster(ID3D11DeviceContext* c, UINT count, UINT first, INT base, bool indexed,
                Dx11OriginalDrawIndexed original, Dx11OriginalDraw originalDraw, bool instanced = false,
                UINT instances = 1, UINT firstInstance = 0, Dx11OriginalDrawIndexedInstanced originalInstanced = nullptr,
                RasterReturnScope* returned = nullptr) noexcept
    {
        if (!original) original = capture->hooks.drawIndexed;
        if (!originalDraw) originalDraw = capture->hooks.draw;
        if (!originalInstanced) originalInstanced = capture->hooks.drawIndexedInstanced;
        if (instanced && (!indexed || !originalInstanced || !SupportsIndexedInstanced()))
        { root.Message("UNKNOWN: authentic indexed-instanced trampoline/typed observer bridge unavailable; original call preserved"); return false; }
        if (!accepting || !commands || !executor || (instanced ? !originalInstanced : indexed ? !original : !originalDraw) ||
            c != capture->context.Get() || GetCurrentThreadId() != thread) return false;
        if (!selectedCount) return false;
        const bool shaderTarget = shaderSelection;
        const auto eligible = [&](const SelectedOverride& entry) noexcept {
            return entry.spec->MatchesDraw(indexed, first, count);
        };
        const auto selectionEnd = selectedOverrides.begin() + selectedCount;
        const bool selectionRange = std::any_of(selectedOverrides.begin(), selectionEnd, eligible);
        if (!selectionRange && !observers.Active()) return false;
        if (!count || invocation >= UINT64_MAX - 1) return false;
        ++invocation;
        snapshot = {}; snapshot.immediate = true; snapshot.drawIndexed = indexed; snapshot.draw.drawIndexed = indexed;
        snapshot.draw.instanced = instanced; snapshot.draw.instanceCount = instances; snapshot.draw.firstInstance = firstInstance;
        // Stamp the authentic outer boundary once. Nested run/check/replacement
        // commands keep this value even when they issue a different Draw kind.
        snapshot.draw.physicalCall = instanced ? PhysicalDrawCall::DrawIndexedInstanced :
            indexed ? PhysicalDrawCall::DrawIndexed : PhysicalDrawCall::Draw;
        if (indexed) { snapshot.draw.firstIndex = first; snapshot.draw.indexCount = count; snapshot.draw.baseVertex = base; }
        else { snapshot.draw.firstVertex = first; snapshot.draw.vertexCount = count; }
        snapshot.draw.thisTarget = ingresses[0]; // automatic programs supply their own stage target
        drawInvalidation = capture->invalidation;
        drawVertexReadRevision = capture->vertexReadRevision;
        // Cache keys include the actual complete qualified IA/shader/resource
        // IDs. The stability epoch changes on writes/opaque invalidation, NOT
        // on every invocation (which would defeat positive/negative caching).
        snapshot.epochs = {generation, consumer, capture->generation, capture->generation, drawInvalidation};
        pixelKnown.fill(false); vertexTextureKnown.fill(false); vertexKnown.fill(false); indexKnown = false; outputKnown.fill(false);
        struct IngressSnapshot
        {
            MatchSubject subject;
            bool captured = false, candidate = false;
        };
        std::array<IngressSnapshot, MaxOverrideRequirements> inputs{};
        bool hasCandidate = shaderTarget && selectionRange;
        bool selectionUnknown = false;
        if (!shaderTarget && selectionRange)
        {
            // Read only requested targets with a range-eligible selection.
            // Unknown ingress cannot be silently dropped to run a partial tape;
            // known null/nonmatching inputs are normal. Never require a fake IB
            // for a nonindexed skinning Draw at a VB/texture ingress.
            for (auto it = selectedOverrides.begin(); it != selectionEnd; ++it)
            {
                if (!eligible(*it)) continue;
                auto& input = inputs[it->ingress];
                if (!input.captured)
                {
                    input.subject.target = ingresses[it->ingress];
                    if (!CurrentBinding(input.subject.target, input.subject.binding) ||
                        !Identity(this, input.subject.binding, input.subject.resource))
                    { ++root.observation.unknown; selectionUnknown = true; continue; }
                    if (input.subject.resource.kind == IdentityKind::Buffer)
                        input.subject.descriptors = Descriptors(this, input.subject.binding);
                    input.captured = true;
                }
                const auto& identity = input.subject.resource;
                if (identity.knowledge == Knowledge::Known && identity.hashKnown &&
                    identity.origin == ResourceOrigin::Game && identity.upstreamHash == it->spec->hash)
                    input.candidate = hasCandidate = true;
            }
        }
        ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps; ComPtr<ID3D11InputLayout> layout;
        ComPtr<ID3D11GeometryShader> gs;
        ComPtr<ID3D11HullShader> hs; ComPtr<ID3D11DomainShader> ds;
        c->VSGetShader(&vs, nullptr, nullptr); c->PSGetShader(&ps, nullptr, nullptr);
        const auto vertex = capture->shaders.Find(Pointer(vs.Get()));
        const auto pixel = capture->shaders.Find(Pointer(ps.Get()));
        if (vertex) snapshot.vertexShader = vertex->identity;
        if (pixel) snapshot.pixelShader = pixel->identity;
        else if (!ps) snapshot.pixelShader.knowledge = Knowledge::Null;
        if (shaderTarget && hasCandidate)
        {
            auto& subject = inputs[0].subject; subject.target = ingresses[0];
            hasCandidate = CurrentBinding(subject.target, subject.binding) &&
                ShaderQuery(this, subject.target, subject.binding, subject.shader) &&
                subject.shader.hash == selectedOverrides[0].spec->hash;
            inputs[0].captured = inputs[0].candidate = hasCandidate;
        }
        // Preserve the original all-ingress rejection for a selected tape.
        // A known nonselected observation can still be independent of unknown
        // unrelated selection metadata, guarded against all replacement effects.
        if (hasCandidate && selectionUnknown) return false;
        const bool observing = !hasCandidate;
        if (observing && ObserveUnselected(vertex, pixel, returned) != ObserverIngress::Packet) return false;
        // Only an actual resource observation that needs GPU-visible parameter
        // effects reaches the existing whole pipeline/reader/packet path.
        if (indexed)
        {
            Binding binding;
            if (!CurrentBinding({TargetKind::IndexBuffer, 0}, binding)) { ++root.observation.unknown; return false; }
        }
        c->IAGetInputLayout(&layout);
        c->HSGetShader(&hs, nullptr, nullptr); c->DSGetShader(&ds, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr);
        if (hs || ds)
        { ++root.observation.unknown; root.Message("Unsupported: bound HS/DS has no authenticated automatic-stage contract"); return false; }
        const auto input = capture->layouts.Find(Pointer(layout.Get()));
        const auto geometry = capture->shaders.Find(Pointer(gs.Get()));
        if (gs && (!geometry || !geometry->stream.qualified || !capture->streamCoverage))
        { ++root.observation.unknown; root.Message("Unsupported: unknown GS/SO automatic-stage contract"); return false; }
        if (!vertex || (ps && !pixel) || !input) { ++root.observation.unknown; return false; }
        snapshot.vertexShader = vertex->identity;
        if (pixel) snapshot.pixelShader = pixel->identity;
        else snapshot.pixelShader.knowledge = Knowledge::Null;
        ++root.observation.matched;
        c->IAGetPrimitiveTopology(&snapshot.topology);
        if ((snapshot.topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && snapshot.topology != D3D11_PRIMITIVE_TOPOLOGY_POINTLIST) ||
            !vertex->inputs.qualified || (pixel && !pixel->plain) || !input->exact)
        { if (!observing) accepting = false; root.Message("Unsupported: matched component has unqualified original VS inputs, layout or topology"); return false; }
        std::array<UINT, GimiVertexSlots> strides{};
        for (UINT slot = 0; slot < GimiVertexSlots; ++slot) if (input->slots & (1u << slot))
        {
            Binding binding;
            if (!CurrentBinding({TargetKind::VertexBuffer, slot}, binding)) { ++root.observation.unknown; return false; }
            strides[slot] = binding.stride;
        }
        currentPipeline.reset();
        for (const auto& p : pipelines)
            if (p && p->input == input && p->vs == vertex && p->ps == pixel && p->strides == strides &&
                p->topology == snapshot.topology && p->gs == geometry) { currentPipeline = p; break; }
        if (!currentPipeline)
        {
            if (observing)
            {
                for (unsigned i = 0; i < observerRefusalCount; ++i)
                {
                    const auto& refused = observerRefusals[i];
                    if (refused.layout == input && refused.vs == vertex && refused.ps == pixel &&
                        refused.strides == strides && refused.topology == snapshot.topology && refused.gs == geometry) return false;
                }
                if (observerRefusalCount == observerRefusals.size()) return false;
            }
            if (!witness.pending) witness = {true, input, vertex, pixel, strides, snapshot.topology, geometry, observing};
            root.Message("UNKNOWN: exact pipeline queued for Present preparation"); return false;
        }
        snapshot.layout = currentPipeline->qualified;
        if (indexed)
        {
            Dx11RetainedBinding index;
            if (!ResolveGame(snapshot.indexBinding, index) || !Indices(index.value,
                static_cast<DXGI_FORMAT>(snapshot.indexBinding.format), snapshot.indexBinding.offset,
                first, count, snapshot.indices)) return false;
        }
        // The cache is direct-mapped. Validate each entry against its complete
        // sibling result before the next ingress can evict the slot. Retain only
        // fixed numeric requirements, not references into that mutable cache.
        std::array<bool, MaxComponentSelections> selectedMatches{};
        for (std::uint32_t i = 0; !observing && i < ingressCount; ++i) if (inputs[i].candidate)
        {
            const auto& subject = inputs[i].subject;
            const auto& matches = currentPipeline->cache->Lookup(true, snapshot, subject);
            if (matches.status != MatchStatus::Matched)
            { ++root.observation.unknown; return false; }
            const auto hash = shaderTarget ? subject.shader.hash : subject.resource.upstreamHash;
            for (std::uint32_t n = 0; n < selectedCount; ++n)
            {
                const auto& entry = selectedOverrides[n];
                if (entry.ingress != i || !eligible(entry) || entry.spec->hash != hash) continue;
                if (std::find(matches.programs.begin(), matches.programs.begin() + matches.count,
                              entry.spec->program) == matches.programs.begin() + matches.count) return false;
                selectedMatches[n] = true;
            }
        }
        // Compact in original Plan order, independently of ingress ordering.
        // CommandState rejects duplicate/out-of-order programs and requires all
        // of these exact bindings to reach real PRE Check in the SAME tape.
        std::array<OverrideRequirement, MaxOverrideRequirements> required{};
        std::uint32_t requiredCount = 0;
        for (std::uint32_t n = 0; n < selectedCount; ++n) if (selectedMatches[n])
        {
            // Keep the original interpreter/tape bound. Never truncate a
            // package selection to manufacture a partially checked draw.
            if (requiredCount == required.size())
            { ++root.observation.unknown; root.Message("Unsupported: more than 32 exact requirements on one draw; complete MOD tape refused"); return false; }
            const auto& entry = selectedOverrides[n];
            const auto& subject = inputs[entry.ingress].subject;
            required[requiredCount++] = {entry.spec->program, subject.target, subject.binding};
        }
        if (!observing && !requiredCount) return false;
        if (!currentPipeline->programCount)
        { ++root.observation.unknown; root.Message("Unsupported: selected resource has no matched automatic VS/PS entry; TextureOverride is check-only"); return false; }
        DrawQueryAdapter queries(*currentPipeline->cache, *plan, snapshot,
            {this, &Read, &Identity, &ShaderQuery, &ShaderFilter, &Descriptors, &ReadConstant, &DrawNativeValue}, true);
        // Resource Lookup above is eligibility only. Actual TextureOverride
        // commands run solely when the ordered shader entry reaches Check.
        // The prepared fixed list gives VS PRE -> PS PRE -> original/skip ->
        // VS POST -> PS POST through the existing single PrepareDraw tape.
        Dx11ExecutionRequest request;
        request.enabled = true; request.device = capture->device.Get(); request.context = c;
        request.commands = commands.get(); request.programs = currentPipeline->programs.data();
        request.programCount = currentPipeline->programCount;
        if (observing)
        {
            request.observationPrograms = observers.Programs(); request.requiredProgram = observerRequiredRoot;
            request.requiredOverrides = observerRequirements.data(); request.requiredOverrideCount = observerRequirementCount;
        }
        else if (shaderTarget) request.requiredProgram = selected; // existing single ShaderOverride contract
        else { request.requiredOverrides = required.data(); request.requiredOverrideCount = requiredCount; }
        if (!observing) request.selectionPrograms = selectionPrograms.data();
        request.draw = snapshot.draw; request.queries = queries.Commands(); request.statics = statics;
        request.parameterState = parameterState;
        request.pipeline = {currentPipeline, currentPipeline->contract, layout.Get(), vs.Get(), ps.Get(),
            currentPipeline->qualified, snapshot.vertexShader, snapshot.pixelShader, snapshot.epochs, snapshot.topology};
        request.pipeline.iniParameterSlotUnused = vertex->iniParameterSlotUnused && (!pixel || pixel->iniParameterSlotUnused);
        request.pipeline.pixelReplacement = currentPipeline->pixelReplacement;
        request.pipeline.vertexReplacement = currentPipeline->vertexReplacement;
        request.pipeline.vertexInputs = &vertex->inputs;
        request.pipeline.pixelInputs = pixel ? &pixel->pixelInputs : nullptr;
        request.pipeline.vertexRangeContext = currentPipeline->rangeContext.Get();
        if (geometry)
        {
            request.pipeline.geometryShader = gs.Get(); request.pipeline.geometryIdentity = geometry->identity;
            request.pipeline.geometryInputs = &geometry->inputs; request.pipeline.streamContract = &geometry->stream;
        }
        request.resources = this; request.owner = this; request.original = original; request.originalDraw = originalDraw; request.originalInstanced = originalInstanced;
        const auto result = executor->Execute(request);
        if (result.consumed) ++root.observation.executed;
        if (!result && !result.quarantined)
        {
            if (result.error == Dx11ExecutionError::Busy || result.error == Dx11ExecutionError::Retention) ++root.observation.busy;
            else ++root.observation.unknown;
            if (result.error == Dx11ExecutionError::VertexConstant && currentPipeline->vertexReplacement &&
                result.vertexConstantSlot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
            {
                const auto& replacement = *currentPipeline->vertexReplacement;
                const auto slot = result.vertexConstantSlot;
                std::snprintf(root.observation.message, sizeof(root.observation.message),
                    "Unsupported: %s VS cb%u relative bytes[0..%u): %s; full tape discarded",
                    replacement.ConstantLabel(slot).c_str(), slot, replacement.RequiredConstantBytes()[slot],
                    result.vertexConstantReason ? result.vertexConstantReason : "unknown exact reader owner/range");
            }
            else if (result.error == Dx11ExecutionError::VertexShader && currentPipeline->vertexReplacement)
                std::snprintf(root.observation.message, sizeof(root.observation.message),
                    "Unsupported: %s typed VS source/device/generation or immutable t120 capability; full tape discarded",
                    currentPipeline->vertexReplacement->SourceLabel().c_str());
            else if (result.error == Dx11ExecutionError::Unsupported)
                root.Message("Unsupported: original VS input shape or raster state; draw not replaced");
            else if (result.error == Dx11ExecutionError::UnsupportedParameter)
                root.Message("Unsupported: IniParams immutable page/domain or reserved t120 contract; shader consumer ABI not inferred");
            else if (result.error == Dx11ExecutionError::Raster && !SupportsIndexedInstanced())
                root.Message("Unsupported: tape requires an authentic indexed-instanced bridge; no command submitted");
            else if (result.error == Dx11ExecutionError::OutlineMaterial)
                root.Message("Unsupported: exact Outline nonnull t69/t70 material view violates immutable shape/owner contract; full tape discarded");
            else if (result.error == Dx11ExecutionError::OutlineSampler)
                root.Message("Unsupported: exact Outline actual s15 sampler device/filter contract; no sampler synthesized");
            else if (result.error == Dx11ExecutionError::PixelMaterial)
                root.Message("Unsupported: command-bound DDS differs from actual original PS resource shape at raster; full tape discarded");
            else if (result.error == Dx11ExecutionError::PixelSampler)
                root.Message("Unsupported: original PS actual sampler shape for DDS consumer; no sampler synthesized");
            else if (result.prepared.error == ExecutionError::UnselectedOverride)
            {
                const auto id = result.prepared.program;
                const auto* label = id < selectionLabels.size() && !selectionLabels[id].empty() ?
                    selectionLabels[id].c_str() : "unknown source";
                std::snprintf(root.observation.message, sizeof(root.observation.message),
                    "Unsupported: %s unselected MOD callback; complete tape rolled back before GPU work", label);
            }
            else if (result.prepared.error == ExecutionError::ObservationEffect)
                root.Message("Unsupported: independent observation reached an unselected resource replacement or unqualified automatic effect; whole tape discarded");
            else if (result.prepared.error == ExecutionError::RequiredProgramMissing)
                root.Message("Unsupported: automatic tape missed required shader entry or exact original-binding PRE Check; complete tape discarded");
        }
        // Important: no fallback after an actual boundary or partial command.
        return result.consumed;
    }
};

Target ParseTrigger(std::string text)
{
    text = LowerAscii(Trim(text));
    if (text == "ib") return {TargetKind::IndexBuffer, 0};
    if (text == "vs") return {TargetKind::VertexShader, 0};
    if (text == "ps") return {TargetKind::PixelShader, 0};
    const auto parse = [&](std::string_view prefix, UINT maximum, TargetKind kind) -> Target {
        if (!text.starts_with(prefix)) return {};
        const auto suffix = std::string_view(text).substr(prefix.size());
        if (suffix.empty() || suffix.size() > 3) return {};
        UINT value = 0;
        for (const auto ch : suffix) { if (ch < '0' || ch > '9') return {}; value = value * 10 + ch - '0'; }
        return value < maximum ? Target{kind, value} : Target{};
    };
    auto target = parse("vb", GimiVertexSlots, TargetKind::VertexBuffer);
    if (target.kind != TargetKind::Null) return target;
    target = parse("ps-t", 128, TargetKind::PixelTexture);
    if (target.kind != TargetKind::Null) return target;
    return parse("vs-t", 128, TargetKind::VertexTexture);
}
std::shared_ptr<Session> Load(State& state, const Selection& selection)
{
    auto capture = state.capture;
    if (capture && capture->creationAttempt && capture->creationResult != NO_ERROR)
    {
        std::snprintf(state.observation.message, sizeof(state.observation.message),
            "UNKNOWN: creation hook install failed (%ld); no automatic retry for this device/request",
            capture->creationResult);
        return {};
    }
    if (!capture || !capture->coverage || !capture->creations || capture->opaque || capture->exhausted)
    { state.Message("UNKNOWN: waiting for complete authentic device/creation/write coverage; restart may be required"); return {}; }
    // Resolve one bounded declaration. Qualified tokens travel through the
    // existing Component config field; structured callers need no serialization.
    std::vector<ComponentSelection> choices;
    const auto append = [&](std::string_view component, std::string_view trigger) {
        ComponentSelection choice{LowerAscii(Trim(component)), LowerAscii(Trim(trigger))};
        if (choice.component.empty() || choice.component.size() > 255 ||
            choices.size() >= MaxComponentSelections || ParseTrigger(choice.trigger).kind == TargetKind::Null ||
            std::any_of(choices.begin(), choices.end(), [&](const auto& old) { return old.component == choice.component; }))
            return false;
        choices.push_back(std::move(choice)); return true;
    };
    if (!selection.entries.empty())
    {
        if (!selection.component.empty() || selection.entries.size() > MaxComponentSelections)
        { state.Message("Unsupported: use either Components or 1..64 structured entries, never both"); return {}; }
        for (const auto& choice : selection.entries)
            if (!append(choice.component, choice.trigger))
            { state.Message("Unsupported: entries need distinct exact names and explicit resource targets (or one shader)"); return {}; }
    }
    else
    {
        const bool qualified = selection.component.find('@') != std::string::npos;
        if (selection.component.empty() || selection.component.size() > (qualified ? 8192u : 255u))
        { state.Message("Unsupported: Components exceeds its bounded legacy/qualified field or is empty"); return {}; }
        for (std::size_t begin = 0;;)
        {
            const auto end = selection.component.find('|', begin);
            const auto token = std::string_view(selection.component).substr(begin,
                end == std::string::npos ? std::string::npos : end - begin);
            const auto at = token.find('@');
            if ((qualified && (at == std::string_view::npos || token.find('@', at + 1) != std::string_view::npos)) ||
                !append(qualified ? token.substr(at + 1) : token,
                        qualified ? token.substr(0, at) : std::string_view(selection.trigger)))
            { state.Message("Unsupported: use 1..64 distinct exact names, all plain or all target@name, separated by |"); return {}; }
            if (end == std::string::npos) break;
            begin = end + 1;
        }
    }
    const auto target = ParseTrigger(choices.front().trigger);
    if (Trim(selection.ini).empty() || selection.ini.size() > 32768 || selection.namespaceRoot.size() > 32768 ||
        selection.hostRoot.size() > 32768 || selection.importRoot.size() > 32768)
    { state.Message("Unsupported: select a nonempty bounded GIMI INI or MOD directory path"); return {}; }
    const auto path = std::filesystem::absolute(std::filesystem::u8path(selection.ini));
    LoadRequest input;
    if (std::filesystem::is_directory(path))
    {
        // Only the explicitly selected subtree, never its parent Mods/core.
        // The Loader supplies deterministic traversal, existing budgets and
        // full required declarations; exact component eligibility is unchanged.
        input.root = path; input.recursiveRoot = true;
    }
    else { input.root = path.parent_path(); input.files.push_back(path.filename()); }
    if (!selection.namespaceRoot.empty()) input.namespaceRoot = std::filesystem::u8path(selection.namespaceRoot);
    if (!selection.hostRoot.empty()) input.hostRoot = std::filesystem::u8path(selection.hostRoot);
    // Explicit dependency files only; never discover libraries or map namespace
    // strings to guessed files. Windows filenames cannot contain '|'.
    if (!selection.libraries.empty())
    {
        if (selection.libraries.size() > 32768)
        { state.Message("Unsupported: GIMI Libraries exceeds the bounded path-list size"); return {}; }
        std::size_t begin = 0;
        for (;;)
        {
            const auto end = selection.libraries.find('|', begin);
            const auto name = Trim(std::string_view(selection.libraries).substr(begin,
                end == std::string::npos ? std::string::npos : end - begin));
            if (name.empty() || input.libraries.size() >= 16)
            { state.Message("Unsupported: Libraries requires 1..16 nonempty explicit absolute INI paths separated by |"); return {}; }
            const auto dependency = std::filesystem::u8path(name);
            if (!dependency.is_absolute())
            { state.Message("Unsupported: each GIMI library INI path must be absolute"); return {}; }
            input.libraries.push_back(dependency);
            if (end == std::string::npos) break;
            begin = end + 1;
        }
    }
    auto config = LoadConfig(input);
    if (!config) { state.DiagnosticMessage(config.diagnostics); return {}; }
    if (!config.config->host.rootIni.empty() && !selection.importRoot.empty())
    {
        const auto original = std::filesystem::u8path(selection.importRoot);
        if (!original.is_absolute() || LowerAscii(PathUtf8(std::filesystem::weakly_canonical(original))) !=
            LowerAscii(PathUtf8(config.config->host.rootIni)))
        { state.Message("Unsupported: ImportRoot must equal the compiled HostRoot; one original user-state authority only"); return {}; }
    }
    // Arming creation prerequisites does not authorize a partial executable MOD.
    auto resize = CompileBufferResizeRules(*config.config);
    capture->resizeRules.clear(); capture->resizeRevision = 0;
    if (!resize) { state.DiagnosticMessage(resize.diagnostics); return {}; }
    if (!config.config->host.allowBufferResize && !resize.rules.empty())
    { state.Message("Unsupported: original Rendering.allow_buffer_resize disables required MOD creation-time resize; no rules armed"); return {}; }
    capture->resizeRules = std::move(resize.rules);
    capture->resizeRevision = state.loaded;
    CompileRequest compile; compile.config = config.config;
    for (const auto& choice : choices) compile.entries.push_back(choice.component);
    auto plan = CompilePlan(compile);
    if (!plan) { state.DiagnosticMessage(plan.diagnostics); return {}; }
    const auto selected = plan.plan->Entries().front();
    const auto* selectedOverride = plan.plan->OverrideForProgram(selected);
    const bool shaderTarget = target.kind == TargetKind::VertexShader || target.kind == TargetKind::PixelShader;
    for (const auto entry : plan.plan->Entries())
    {
        const auto& program = plan.plan->Programs()[entry];
        const auto choice = std::find_if(choices.begin(), choices.end(),
            [&](const auto& value) { return value.component == program.name; });
        if (choice == choices.end())
        { state.Message("Unsupported: selected entry lost its exact declared target"); return {}; }
        const auto entryTarget = ParseTrigger(choice->trigger);
        const bool entryShader = entryTarget.kind == TargetKind::VertexShader || entryTarget.kind == TargetKind::PixelShader;
        const auto* spec = plan.plan->OverrideForProgram(entry);
        if (!spec || spec->matchKind != OverrideMatchKind::Hash || spec->shader != entryShader || entryShader != shaderTarget ||
            (entryShader && plan.plan->Entries().size() != 1))
        { state.Message("Unsupported: select exact resource entries at explicit targets, or one ShaderOverride at its shader trigger"); return {}; }
        if (plan.plan->Entries().size() > 1)
        {
            // Catchall skip/filter/resize siblings still execute through Check,
            // but cannot alone authorize unrelated parts/LODs in a multi-part set.
            const auto& pre = plan.plan->Programs()[entry].commands[0];
            const bool work = std::any_of(pre.begin(), pre.end(), [](const Command& command) {
                return command.op == CommandOp::Reference || command.op == CommandOp::Draw ||
                    command.op == CommandOp::DrawIndexed || command.op == CommandOp::DrawIndexedInstanced ||
                    command.op == CommandOp::DrawIndexedAuto || command.op == CommandOp::DrawIndexedInstancedAuto ||
                    command.op == CommandOp::DrawFromCaller || command.op == CommandOp::Run;
            });
            if (!work)
            { state.Message("Unsupported: multi-part eligibility needs a real PRE bind/draw/run per selected part; skip/filter/resize-only siblings are check-only"); return {}; }
        }
    }
    bool hasAutomaticEntry = false;
    std::vector<ProgramInvocation> presentPrograms;
    PresentValueRequirements presentValues{};
    bool presentCompute = false;
    for (Id programId = 0; programId < plan.plan->Programs().size(); ++programId)
    {
        const auto& program = plan.plan->Programs()[programId];
        if (program.kind == ProgramKind::Present && (!program.commands[0].empty() || !program.commands[1].empty()))
        {
            Diagnostic failure; PresentValueRequirements values{};
            bool compute = false;
            if (!ValidatePresentVariables(*plan.plan, programId, failure, values, &compute))
            { state.DiagnosticMessage(std::vector<Diagnostic>{std::move(failure)}); return {}; }
            presentValues[0] |= values[0]; presentValues[1] |= values[1];
            presentCompute |= compute;
            presentPrograms.push_back({programId, {}});
        }
    }
    for (const auto& program : plan.plan->Programs())
    {
        const bool hasCommands = !program.commands[0].empty() || !program.commands[1].empty();
        if (!hasCommands) continue;
        hasAutomaticEntry |= program.kind == ProgramKind::ShaderOverride || program.kind == ProgramKind::ShaderRegex;
        // TextureOverride is a query-only program, not an automatic callback.
        // All its commands/dependencies were compiled; every actual Check
        // queries exact live/simulated bindings and ordered hash siblings.
        // Present's full reachable closure was qualified above. Unknown
        // GUI/CS/arbitrary CustomShader still fail there; typed MRT ref/set is separate.
    }
    if (!hasAutomaticEntry)
    {
        const auto source = PathUtf8(selectedOverride->source.file);
        std::snprintf(state.observation.message, sizeof(state.observation.message),
            "%s:%u: Unsupported: explicitly load the real supported ShaderOverride/ShaderRegex entry; TextureOverride cannot execute automatically",
            source.c_str(), selectedOverride->source.line);
        return {};
    }
    if (!plan.plan->KeyCycles().empty() && plan.plan->KeyCycles().size() != 4)
    { state.Message("Unsupported: only the complete fixed four GIMIv8 key cycles are implemented"); return {}; }
    auto files = PreloadResources(plan.plan);
    if (!files) { state.DiagnosticMessage(files.diagnostics); return {}; }
    ComPtr<ID3D11Device5> device5; ComPtr<ID3D11DeviceContext4> context4;
    if (FAILED(capture->device.As(&device5)) || FAILED(capture->context.As(&context4)))
    { state.Message("Unsupported: real D3D11 fence retirement unavailable; no MOD GPU commands issued"); return {}; }
    auto session = std::make_shared<Session>(state, capture);
    session->generation = Ids(); session->consumer = Ids(); session->thread = GetCurrentThreadId();
    session->plan = std::move(plan.plan); session->selected = selected; session->shaderSelection = shaderTarget;
    // Capture exact plan-owned metadata once in upstream hash/priority/name
    // order. Never parse names or scan the full override graph in a draw.
    for (const auto& spec : session->plan->Overrides())
    {
        const auto choice = std::find_if(choices.begin(), choices.end(), [&](const auto& value) {
            return value.component == session->plan->Programs()[spec.program].name;
        });
        if (choice == choices.end()) continue;
        const auto entryTarget = ParseTrigger(choice->trigger);
        auto ingress = std::find(session->ingresses.begin(), session->ingresses.begin() + session->ingressCount, entryTarget);
        if (ingress == session->ingresses.begin() + session->ingressCount)
        {
            if (session->ingressCount == session->ingresses.size())
            { state.Message("Unsupported: exact selection exceeds 32 distinct ingress targets; no session published"); return {}; }
            session->ingresses[session->ingressCount++] = entryTarget;
        }
        session->selectedOverrides[session->selectedCount++] =
            {&spec, static_cast<std::uint32_t>(ingress - session->ingresses.begin())};
    }
    if (session->selectedCount != choices.size())
    { state.Message("Unsupported: incomplete exact ingress selection; no session published"); return {}; }
    // Required declarations/resources stay complete. Callback execution is
    // separately limited to explicit selections within the selected MOD root.
    // Original host/core/library dependencies keep their original semantics.
    session->selectionPrograms.resize(session->plan->Programs().size());
    session->selectionLabels.resize(session->selectionPrograms.size());
    const auto& configuration = *session->plan->Configuration();
    const auto selectedRoot = LowerAscii(PathUtf8(configuration.root));
    for (Id id = 0; id < session->plan->Programs().size(); ++id)
    {
        const auto& program = session->plan->Programs()[id];
        const auto* origin = configuration.RootFor(program.source.file);
        if (!origin)
        { state.Message("Unsupported: executable program has no admitted source root"); return {}; }
        const bool selectedPackage = LowerAscii(PathUtf8(*origin)) == selectedRoot;
        const bool callback = program.kind == ProgramKind::TextureOverride ||
            program.kind == ProgramKind::ShaderOverride || program.kind == ProgramKind::ShaderRegex;
        auto& admission = session->selectionPrograms[id];
        if (selectedPackage && callback)
            session->selectionLabels[id] = PathUtf8(program.source.file) + ":" + std::to_string(program.source.line);
        if (!selectedPackage || !callback) admission = SelectDependency;
        else if (std::find(session->plan->Entries().begin(), session->plan->Entries().end(), id) != session->plan->Entries().end())
            admission = SelectExplicit;
        else if (program.kind != ProgramKind::ShaderRegex && program.commands[0].empty() && program.commands[1].empty())
            admission = SelectPassive; // filter/resize metadata, no executable effects
        else if (program.kind == ProgramKind::TextureOverride && program.commands[1].empty() &&
            !program.commands[0].empty() && std::all_of(program.commands[0].begin(), program.commands[0].end(),
                [](const Command& command) { return command.op == CommandOp::Skip; }))
            admission = SelectSkipCompanion; // still requires this draw's selected hash/target/binding
    }
    Diagnostic observerFailure;
    if (!session->observers.Compile(*session->plan, observerFailure))
    { state.DiagnosticMessage({observerFailure}); return {}; }
    session->rootInputDisabled = !session->plan->Configuration()->host.inputEnabled;
    session->presentPrograms = std::move(presentPrograms); session->presentValues = presentValues;
    session->presentCompute = presentCompute;
    for (unsigned i = 0; i < session->plan->KeyCycles().size(); ++i)
    {
        const auto& chord = session->plan->KeyCycles()[i].chord;
        if (chord == "no_modifiers f6") session->cycleChords[i] = 0;
        else if (chord == "no_modifiers f12") session->cycleChords[i] = 1;
        else if (chord == "ctrl f12") session->cycleChords[i] = 2;
        else if (chord == "no_modifiers f10") session->cycleChords[i] = 3;
        else { state.Message("Unsupported: unknown GIMI cycle chord"); return {}; }
    }
    std::vector<Diagnostic> shaderDiagnostics;
    session->shaderTools = ShaderRegexLibrary::Compile(session->plan, shaderDiagnostics);
    if (!session->shaderTools) { state.DiagnosticMessage(shaderDiagnostics); return {}; }
    if (FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&session->fence))))
    { state.Message("Unsupported: MOD fence creation failed"); return {}; }
    session->submit = std::move(context4);
    std::uint64_t nesting = 0;
    if (!capture->hooks.enter || !capture->hooks.enter(capture->context.Get(), nesting))
    { state.Message("Unsupported: existing internal suppression unavailable"); return {}; }
    struct Leave { Capture& c; std::uint64_t n; ~Leave() { c.hooks.leave(c.context.Get(), n); } } leave{*capture, nesting};
    const auto first = Ids(files.files->Resources().size());
    auto resources = CreateDx11Resources(files.files, capture->device.Get(), capture->generation, first, capture->hashPolicy);
    if (!resources) { state.DiagnosticMessage(resources.diagnostics); return {}; }
    session->statics = std::move(resources.resources);
    session->parameterState = files.files->Parameters().initial;
    session->commands = std::make_unique<CommandState>(session->plan);
    if (!session->statics->Initialize(*session->commands)) throw std::runtime_error("Static resource initialization failed");
    session->executor = std::make_unique<Dx11Executor>(session->plan->Limits().actions, Flights);
    if (session->presentCompute)
    {
        std::vector<Diagnostic> diagnostics;
        session->formatText = CreateFormatTextLibrary(session->plan, capture->device.Get(), diagnostics);
        if (!session->formatText) { state.DiagnosticMessage(diagnostics); return {}; }
        session->textRender = CreateNativeTextShaderLibrary(session->plan, capture->device.Get(), diagnostics);
        if (!session->textRender) { state.DiagnosticMessage(diagnostics); return {}; }
        session->compute = std::make_unique<NativeComputeExecutor>(session->plan->Limits().actions,
            Flights, capture->device.Get(), session->statics, session->formatText, session->textRender);
    }
    session->aliases.resize(session->plan->Resources().size());
    session->aliasScratch.resize(session->aliases.size()); session->aliasStamp.resize(session->aliases.size());
    for (Id id = 0; id < session->aliases.size(); ++id)
    {
        auto& lease = session->aliases[id];
        if (!session->statics->Get(id, lease.value)) throw std::runtime_error("Static alias owner missing");
        if (lease.value.resource) lease.statics = session->statics;
        lease.stableReadOnly = !lease.value.uav;
    }
    // Last semantic initialization, after every declaration/resource setup.
    // This only reads. Initial seed/save occurs on the next safe Present.
    std::string stateError;
    // Host parsing already resolved user_config under its original authority.
    // Initialization remains declarations -> immutable resources -> user state;
    // do not reopen an independently changed root to choose a second authority.
    const auto& importedHost = session->plan->Configuration()->host;
    const auto importRoot = selection.importRoot.empty() && !importedHost.rootIni.empty()
        ? PathUtf8(importedHost.rootIni) : selection.importRoot;
    if (!session->userState.Load(*session->commands, importRoot, stateError, session->plan->Configuration().get()))
    { state.Message(stateError.c_str()); return {}; }
    session->lastStateSaveAttempt = NativeTicks();
    const auto statePath = PathUtf8(session->userState.Path());
    std::snprintf(state.observation.stateFile, sizeof(state.observation.stateFile), "%s", statePath.c_str());
    if (!importedHost.rootIni.empty())
    {
        const auto originalRoot = PathUtf8(importedHost.rootIni);
        std::snprintf(state.observation.hostRoot, sizeof(state.observation.hostRoot), "%s", originalRoot.c_str());
        for (const auto& setting : importedHost.settings)
            switch (setting.kind)
            {
            case HostSettingKind::Native: ++state.observation.hostNativeSettings; break;
            case HostSettingKind::SourceTool: ++state.observation.hostSourceSettings; break;
            case HostSettingKind::Inactive: ++state.observation.hostInactiveSettings; break;
            }
    }
    session->PublishUserState();
    std::snprintf(state.observation.message, sizeof(state.observation.message),
        "Loaded %u exact component(s) at %u ingress(es): waiting for real Check/draw/layout; full MOD/LOD/skeleton not qualified",
        session->selectedCount, session->ingressCount);
    return session;
}
void Park(State& state) noexcept
{
    if (!state.active) return;
    state.active->accepting = false;
    // Park is also called by device creation: never do disk IO there. Present
    // must save all committed CPU values before releasing this semantic owner.
    if (!state.active->commands->CanInitialize() || state.nativePresentTicket ||
        state.active->userState.PollDirty(*state.active->commands))
    {
        presentNeeded.store(true, std::memory_order_release);
        state.Message("F6 state pending: old semantic owner retained until safe Present save"); return;
    }
    if (!state.active->HasPackets()) { state.active.reset(); return; }
    for (auto& slot : state.retired) if (!slot) { slot = std::move(state.active); return; }
    // Never drop an owner to make room. Keep current active object as a closed
    // retirement-only owner and refuse plan replacement until capacity returns.
    state.Message("Retirement capacity full: old owners retained; new plan refused");
}
void LostCoverage(State& state) noexcept
{
    if (state.capture) state.capture->opaque = true;
    if (state.active)
    {
        if (state.observedRasterTicket) { state.active->commands->Discard(); state.observedRasterTicket = 0; }
        state.active->Quarantine("Quarantine: concurrent/opaque MOD coverage lost; submitted owners retained");
    }
}
void CreationCoverageLocked(State& state, Capture& capture, bool complete) noexcept
{
    if (capture.creations == complete) return;
    capture.creations = complete;
    mapCaptureLive.store(complete && capture.coverage, std::memory_order_release);
    if (complete) state.loaded = 0; // a real transition only; never clears lost/opaque history
    else state.coverageLost.store(true, std::memory_order_release);
}
} // namespace

void Configure(Selection selection, bool reload)
{
    auto& state = Store();
    const bool want = selection.enabled && GenshinProcess();
    {
        std::lock_guard lock(state.requestMutex);
        if (!reload && state.selection.enabled == selection.enabled && state.selection.ini == selection.ini &&
            state.selection.component == selection.component && state.selection.trigger == selection.trigger &&
            state.selection.libraries == selection.libraries && state.selection.importRoot == selection.importRoot &&
            state.selection.entries == selection.entries && state.selection.namespaceRoot == selection.namespaceRoot &&
            state.selection.hostRoot == selection.hostRoot) return;
        state.selection = std::move(selection); ++state.revision;
        contractRevision.store(state.revision, std::memory_order_release);
        enabled.store(want, std::memory_order_release);
    }
    OptiInput::ReleaseGameCursor(); // only a real changed selection; outside MOD/request locks
    // Once observation is disabled, a missed Map interval cannot be guessed
    // away on a fast re-enable. Submitted ownership is still retired normally.
    if (!want && mapCaptureLive.exchange(false, std::memory_order_acq_rel))
        state.coverageLost.store(true, std::memory_order_release);
    // A disabled request still needs one Present to detach/drain the old plan.
    presentNeeded.store(true, std::memory_order_release);
}
void InputFrame(const OptiInput::KeyboardFrame& frame, bool accepting) noexcept
{
    if (!Enabled()) return;
    // User32 calls outside the interpreter lock. Reject external-overlay HWNDs.
    DWORD process = 0;
    if (!frame.valid || !frame.owner || !GetWindowThreadProcessId(frame.owner, &process) ||
        process != GetCurrentProcessId()) return;
    try
    {
        auto& s = Store(); std::lock_guard lock(s.ownerMutex);
        if (!Enabled() || !s.active || s.loaded != contractRevision.load(std::memory_order_acquire)) return;
        s.active->QueueInput(frame, accepting);
        // No disk IO or GPU call here, even if a native PRE/POST spans the menu.
        s.active->ConsumeInput();
    }
    catch (...) { Store().coverageLost.store(true, std::memory_order_release); }
}
Selection Selected() { auto& s = Store(); std::lock_guard lock(s.requestMutex); return s.selection; }
bool Enabled() noexcept { return enabled.load(std::memory_order_acquire); }
bool NeedsPresent() noexcept { return presentNeeded.load(std::memory_order_acquire); }
void StopAccepting() noexcept
{
    OptiInput::ReleaseGameCursor();
    enabled.store(false, std::memory_order_release);
    if (mapCaptureLive.exchange(false, std::memory_order_acq_rel)) Store().coverageLost.store(true, std::memory_order_release);
    try { auto& s = Store(); std::lock_guard request(s.requestMutex); ++s.revision; }
    catch (...) {}
    presentNeeded.store(true, std::memory_order_release);
}

void DeviceCreated(ID3D11Device* device, ID3D11DeviceContext* context, HookServices hooks, bool coverage) noexcept
{
    if (!Enabled() || !device || !context) return;
    try
    {
        auto& s = Store(); std::lock_guard lock(s.ownerMutex);
        if (s.capture && s.capture->device.Get() == device && s.capture->context.Get() == context)
        {
            if (s.capture->coverage && !coverage) s.coverageLost.store(true, std::memory_order_release);
            s.capture->coverage = coverage; s.capture->hooks = hooks;
            mapCaptureLive.store(coverage && s.capture->creations, std::memory_order_release); return;
        }
        ComPtr<ID3D11Device> actual; context->GetDevice(&actual);
        if (actual.Get() != device || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        { s.Message("UNKNOWN: unqualified/proxy device-context identity; MOD not installed"); return; }
        mapCaptureLive.store(false, std::memory_order_release);
        streamCaptureLive.store(false, std::memory_order_release);
        // Following a new device must not clear an old lost-coverage latch
        // before its submitted owner has inherited the opaque/quarantine state.
        if (s.coverageLost.load(std::memory_order_acquire)) LostCoverage(s);
        Park(s);
        if (s.active)
        {
            if (s.active->userState.Dirty())
                s.Message("Unsupported: device replacement before F6 state save; old owner retained, new capture refused; restart after safe save");
            return;
        }
        s.capture = std::make_shared<Capture>(); auto& c = *s.capture;
        c.device = device; c.context = context; c.generation = Ids(); c.coverage = coverage; c.hooks = hooks;
        c.context.As(&c.rangeContext); // off-draw only, optional D3D11.1 CB ranges
        s.loaded = 0; s.coverageLost.store(false, std::memory_order_release);
    }
    catch (...) { Store().coverageLost.store(true, std::memory_order_release); }
}
void CreationCoverage(ID3D11Device* device, bool complete) noexcept
{
    if (!Enabled()) return;
    auto& s = Store(); std::lock_guard lock(s.ownerMutex);
    if (s.capture && s.capture->device.Get() == device)
        CreationCoverageLocked(s, *s.capture, complete);
}
CreationHookRequest ClaimCreationHooks(ID3D11Device* device, ID3D11DeviceContext* context) noexcept
{
    if (!Enabled() || !device || !context) return {};
    try
    {
        auto& s = Store(); std::lock_guard owner(s.ownerMutex);
        std::lock_guard request(s.requestMutex);
        if (!Enabled() || !s.selection.enabled || !s.capture || s.capture->device.Get() != device ||
            s.capture->context.Get() != context || s.capture->creationAttempt == s.revision) return {};
        auto& c = *s.capture;
        c.creationAttempt = s.revision;
        c.creationResult = ERROR_IO_PENDING; // claim before any install attempt, including a busy refusal
        return {s.revision, c.generation};
    }
    catch (...) { Store().coverageLost.store(true, std::memory_order_release); return {}; }
}
void CompleteCreationHooks(ID3D11Device* device, ID3D11DeviceContext* context,
                           CreationHookRequest request, LONG result) noexcept
{
    try
    {
        auto& s = Store(); std::lock_guard owner(s.ownerMutex);
        if (!request.revision || !s.capture || s.capture->device.Get() != device || s.capture->context.Get() != context ||
            s.capture->generation != request.generation || s.capture->creationAttempt != request.revision) return;
        auto& c = *s.capture;
        c.creationResult = result;
        CreationCoverageLocked(s, c, result == NO_ERROR);
        if (result != NO_ERROR)
            std::snprintf(s.observation.message, sizeof(s.observation.message),
                "UNKNOWN: creation hook install failed (%ld); no automatic retry for this device/request", result);
        // Success observes future creations only. It does not replace the
        // capture, clear coverageLost/opaque, or recover unobserved old resources.
    }
    catch (...) { Store().coverageLost.store(true, std::memory_order_release); }
}
void StreamOutputCoverage(ID3D11Device* device, ID3D11DeviceContext* context, bool complete) noexcept
{
    if (!Enabled()) return;
    auto& state = Store(); std::lock_guard lock(state.ownerMutex);
    if (!state.capture || state.capture->device.Get() != device || state.capture->context.Get() != context) return;
    auto& c = *state.capture;
    if (c.streamCoverage && !complete) state.coverageLost.store(true, std::memory_order_release);
    c.streamCoverage = complete;
}
void StreamOutputTargets(ID3D11DeviceContext* context, UINT count, ID3D11Buffer* const* targets, const UINT* offsets) noexcept
{
    if (!Enabled()) return;
    auto& state = Store(); std::unique_lock lock(state.ownerMutex, std::try_to_lock);
    if (!lock.owns_lock()) { state.coverageLost.store(true, std::memory_order_release); return; }
    if (!state.capture) return;
    auto& c = *state.capture;
    if (context != c.context.Get() || (state.active && state.active->thread != GetCurrentThreadId()))
    { state.coverageLost.store(true, std::memory_order_release); return; }
    if (c.hooks.internal && c.hooks.internal(context)) return;
    if (c.streamRevision == UINT64_MAX || count > c.streamTargets.size() || (count && !targets))
    { state.coverageLost.store(true, std::memory_order_release); return; }
    ++c.streamRevision;
    c.streamTargets = {};
    ID3D11Buffer* actual[D3D11_SO_BUFFER_SLOT_COUNT]{};
    context->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, actual);
    bool live = false;
    for (UINT slot = 0; slot < c.streamTargets.size(); ++slot)
    {
        auto* target = slot < count ? targets[slot] : nullptr;
        if (actual[slot] != target) { state.coverageLost.store(true, std::memory_order_release); }
        if (actual[slot]) actual[slot]->Release();
        if (!target) continue;
        live = true; const auto game = c.resources.Find(Pointer(target));
        if (!game || !game->gpuBuffer || !(game->bufferDesc.BindFlags & D3D11_BIND_STREAM_OUTPUT)) continue;
        c.streamTargets[slot] = game;
        const auto offset = offsets ? offsets[slot] : UINT(-1);
        if (!offsets) game->soCursorKnown = false;
        else if (offset != UINT(-1))
        { game->soCursorKnown = offset % 4 == 0 && offset <= game->bufferDesc.ByteWidth; game->soCursorUpper = offset; }
        // UINT(-1) preserves ONLY a previously observed conservative bound.
        for (UINT earlier = 0; earlier < slot; ++earlier) if (c.streamTargets[earlier] == game)
            game->soCursorKnown = false;
    }
    streamCaptureLive.store(live, std::memory_order_release);
}
static void AdvanceStreamOutputs(Capture& c, UINT count, UINT instances, bool known) noexcept
{
    if (known && (!count || !instances)) return; // authentic zero-work draw writes nothing
    ComPtr<ID3D11GeometryShader> gs; D3D11_PRIMITIVE_TOPOLOGY topology{};
    if (known)
    {
        c.context->GSGetShader(&gs, nullptr, nullptr); c.context->IAGetPrimitiveTopology(&topology);
    }
    const auto shader = known ? c.shaders.Find(Pointer(gs.Get())) : nullptr;
    known = known && shader && shader->stream.qualified && topology == D3D11_PRIMITIVE_TOPOLOGY_POINTLIST && instances;
    // Multiplication is bounded before combining UINT count/instance domains.
    const auto perInput = known ? std::uint64_t(shader->stream.maxVertices) * shader->stream.instances : 0;
    for (UINT slot = 0; slot < c.streamTargets.size(); ++slot)
    {
        const auto& game = c.streamTargets[slot]; if (!game) continue;
        const auto stride = known ? shader->stream.strides[slot] : 0;
        const auto bytesPerInstance = perInput * stride;
        const auto inputCount = std::uint64_t(count) * instances;
        const bool fits = known && stride && game->soCursorKnown && game->soCursorUpper <= game->bufferDesc.ByteWidth &&
            (!bytesPerInstance || inputCount <= (game->bufferDesc.ByteWidth - game->soCursorUpper) / bytesPerInstance);
        if (fits) game->soCursorUpper += inputCount * bytesPerInstance;
        else game->soCursorKnown = false;
        // A qualified shader with an unknown/overflowed cursor does not prove
        // a bounded producer. Never turn such output into an IA/SRV reader.
        game->gpuOrdered = c.streamCoverage && fits;
        if (game->gpuRevision == UINT64_MAX) c.exhausted = true; else ++game->gpuRevision;
    }
    if (c.invalidation == UINT64_MAX || c.vertexReadRevision == UINT64_MAX) c.exhausted = true;
    else { ++c.invalidation; ++c.vertexReadRevision; }
}
void StreamOutputDrawn(ID3D11DeviceContext* context, UINT count, UINT instances) noexcept
{
    if (!Enabled() || !streamCaptureLive.load(std::memory_order_acquire)) return;
    auto& state = Store(); std::unique_lock lock(state.ownerMutex, std::try_to_lock);
    if (!lock.owns_lock()) { state.coverageLost.store(true, std::memory_order_release); return; }
    if (!state.capture) return;
    auto& c = *state.capture;
    if (context != c.context.Get() || (state.active && state.active->thread != GetCurrentThreadId()))
    { state.coverageLost.store(true, std::memory_order_release); return; }
    if (c.hooks.internal && c.hooks.internal(context)) return; // executor journals physical calls in its packet
    AdvanceStreamOutputs(c, count, instances, true);
}
void StreamOutputUnknownDraw(ID3D11DeviceContext* context) noexcept
{
    if (!Enabled() || !streamCaptureLive.load(std::memory_order_acquire)) return;
    auto& state = Store(); std::unique_lock lock(state.ownerMutex, std::try_to_lock);
    if (!lock.owns_lock()) { state.coverageLost.store(true, std::memory_order_release); return; }
    if (!state.capture) return;
    if (context != state.capture->context.Get() || (state.active && state.active->thread != GetCurrentThreadId()))
    { state.coverageLost.store(true, std::memory_order_release); return; }
    if (state.capture->hooks.internal && state.capture->hooks.internal(context)) return;
    AdvanceStreamOutputs(*state.capture, 0, 0, false);
}
void StreamOutputCleared(ID3D11DeviceContext* context) noexcept
{
    if (!Enabled()) return;
    auto& state = Store(); std::unique_lock lock(state.ownerMutex, std::try_to_lock);
    if (!lock.owns_lock()) { state.coverageLost.store(true, std::memory_order_release); return; }
    if (!state.capture || context != state.capture->context.Get()) return;
    if (state.capture->hooks.internal && state.capture->hooks.internal(context)) return;
    if (state.active && state.active->thread != GetCurrentThreadId())
    { state.coverageLost.store(true, std::memory_order_release); return; }
    auto& c = *state.capture; c.streamTargets = {};
    if (c.streamRevision == UINT64_MAX) c.exhausted = true; else ++c.streamRevision;
    streamCaptureLive.store(false, std::memory_order_release);
}
void PrepareBufferCreation(ID3D11Device* device, const D3D11_BUFFER_DESC* desc,
                           const D3D11_SUBRESOURCE_DATA* data, BufferCreationEdit& edit) noexcept
{
    if (!Enabled() || !device || !desc || !desc->ByteWidth) return;
    try
    {
        auto& state = Store(); std::lock_guard lock(state.ownerMutex);
        if (!state.capture || state.capture->device.Get() != device) return;
        auto& c = *state.capture;
        if (!c.creations || !c.coverage || c.opaque || c.exhausted || c.resizeRules.empty() ||
            c.resizeRevision != contractRevision.load(std::memory_order_acquire) ||
            (c.hooks.internal && c.hooks.internal(c.context.Get()))) return;
        std::uint32_t dataHash = 0, hash = 0;
        CpuImage initial{data ? data->pSysMem : nullptr, data && data->pSysMem ? desc->ByteWidth : 0, 0};
        if (!HashBuffer(*desc, initial, dataHash, hash)) return;
        const auto rule = std::find_if(c.resizeRules.begin(), c.resizeRules.end(), [&](const auto& r) { return r.hash == hash; });
        if (rule == c.resizeRules.end()) return;
        const auto width = (std::max)(desc->ByteWidth, rule->byteWidth);
        const bool shape = desc->Usage == D3D11_USAGE_DEFAULT && !desc->CPUAccessFlags &&
            (desc->BindFlags & (D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_UNORDERED_ACCESS)) &&
            !(desc->BindFlags & ~(D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE)) &&
            !(desc->MiscFlags & ~(D3D11_RESOURCE_MISC_BUFFER_STRUCTURED | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS)) &&
            (!(desc->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) ||
                (desc->StructureByteStride && width % desc->StructureByteStride == 0));
        if (!shape || width > CpuBudget)
        { c.exhausted = true; state.Message("Unsupported: matching resize buffer descriptor; original creation preserved, MOD closed"); return; }
        edit.original = edit.effective = *desc; edit.effective.ByteWidth = width; edit.rule = *rule;
        edit.deviceGeneration = c.generation; edit.revision = c.resizeRevision; edit.creationHash = hash;
        if (data) edit.initial = *data;
        if (width != desc->ByteWidth && data && data->pSysMem)
        {
            edit.padded.resize(width, 0); std::memcpy(edit.padded.data(), data->pSysMem, desc->ByteWidth);
            edit.initial.pSysMem = edit.padded.data();
        }
        edit.changed = width != desc->ByteWidth; edit.qualified = true;
    }
    catch (...) { edit.changed = edit.qualified = false; Store().coverageLost.store(true, std::memory_order_release); }
}
bool PrepareUnorderedView(ID3D11Device* device, ID3D11Resource* resource,
                          const D3D11_UNORDERED_ACCESS_VIEW_DESC* original,
                          D3D11_UNORDERED_ACCESS_VIEW_DESC& effective, bool& changed) noexcept
{
    changed = false;
    if (!Enabled() || !device || !resource) return true;
    try
    {
        auto& state = Store(); std::lock_guard lock(state.ownerMutex);
        if (!state.capture || state.capture->device.Get() != device) return true;
        auto& c = *state.capture;
        if (c.hooks.internal && c.hooks.internal(c.context.Get())) return true;
        const auto game = c.resources.Find(Pointer(resource));
        if (!game || !game->resize.byteWidth) return true;
        const auto& rule = game->resize;
        const auto bad = [&] { c.exhausted = true; state.Message("Unsupported: resized buffer UAV range; original view creation preserved, MOD closed"); return false; };
        D3D11_UNORDERED_ACCESS_VIEW_DESC inferred{};
        const bool inferredView = original == nullptr;
        if (inferredView)
        {
            if (!(game->bufferDesc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) ||
                !game->bufferDesc.StructureByteStride || game->originalBufferDesc.ByteWidth % game->bufferDesc.StructureByteStride) return bad();
            inferred.Format = DXGI_FORMAT_UNKNOWN; inferred.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            inferred.Buffer.NumElements = game->originalBufferDesc.ByteWidth / game->bufferDesc.StructureByteStride;
            original = &inferred;
        }
        effective = *original;
        if (!game->buffer || original->ViewDimension != D3D11_UAV_DIMENSION_BUFFER || original->Buffer.FirstElement) return bad();
        UINT element = 0;
        if (original->Buffer.Flags & D3D11_BUFFER_UAV_FLAG_RAW)
        {
            if (original->Buffer.Flags != D3D11_BUFFER_UAV_FLAG_RAW || original->Format != DXGI_FORMAT_R32_TYPELESS ||
                !(game->bufferDesc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS)) return bad();
            element = 4;
        }
        else if (game->bufferDesc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED)
        {
            if (original->Format != DXGI_FORMAT_UNKNOWN ||
                (original->Buffer.Flags & ~(D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER))) return bad();
            element = game->bufferDesc.StructureByteStride;
        }
        else
        {
            if (original->Buffer.Flags) return bad();
            element = FormatElementBytes(original->Format);
        }
        // Upstream chooses the requested COUNT from explicit metadata, not
        // from the view's declared format/StructureByteStride. uav_byte_stride
        // intentionally supports a different logical stride; retain the actual
        // view format/flags and prove its resulting byte range independently.
        const auto requested = rule.uavStride ? rule.byteWidth / rule.uavStride : rule.vertexCount;
        if (!element || game->originalBufferDesc.ByteWidth % element ||
            original->Buffer.NumElements != game->originalBufferDesc.ByteWidth / element ||
            !requested || std::uint64_t(requested) * element > game->bufferDesc.ByteWidth) return bad();
        effective.Buffer.NumElements = requested;
        changed = inferredView || effective.Buffer.NumElements != original->Buffer.NumElements;
        return true;
    }
    catch (...) { Store().coverageLost.store(true, std::memory_order_release); return false; }
}
void UnorderedViewCreated(ID3D11Device* device, ID3D11Resource* resource, ID3D11UnorderedAccessView* view) noexcept
{
    if (!Enabled() || !device || !resource || !view) return;
    try
    {
        auto& state = Store(); std::lock_guard lock(state.ownerMutex);
        if (!state.capture || state.capture->device.Get() != device) return;
        auto& c = *state.capture;
        if (c.hooks.internal && c.hooks.internal(c.context.Get())) return;
        const auto game = c.resources.Find(Pointer(resource));
        if (!game || !game->gpuBuffer || c.unorderedViews.Find(Pointer(view))) return;
        auto owner = std::make_shared<GameUnorderedView>(); owner->resource = game; owner->view = view;
        view->GetDesc(&owner->descriptor);
        if (!c.unorderedViews.Add(Pointer(view), owner)) c.exhausted = true;
    }
    catch (...) { Store().coverageLost.store(true, std::memory_order_release); }
}
void BufferCreated(ID3D11Device* device, ID3D11Buffer* buffer, const D3D11_BUFFER_DESC* desc,
                   const D3D11_SUBRESOURCE_DATA* data, const BufferCreationEdit* edit) noexcept
{
    if (!Enabled() || !buffer || !desc) return;
    try
    {
        auto& s = Store(); std::lock_guard lock(s.ownerMutex);
        if (!s.capture || s.capture->device.Get() != device) return;
        auto& c = *s.capture;
        if (c.hooks.internal && c.hooks.internal(c.context.Get())) return;
        if (c.resources.Find(Pointer(buffer))) return;
        const bool immutable = desc->Usage == D3D11_USAGE_IMMUTABLE && !desc->CPUAccessFlags &&
            !(desc->BindFlags & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) &&
            !(desc->MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE));
        const bool vertexRead = c.coverage && c.creations && desc->ByteWidth &&
            (desc->Usage == D3D11_USAGE_DEFAULT || desc->Usage == D3D11_USAGE_DYNAMIC || immutable) &&
            !(desc->CPUAccessFlags & ~D3D11_CPU_ACCESS_WRITE) &&
            (desc->BindFlags & (D3D11_BIND_CONSTANT_BUFFER | D3D11_BIND_SHADER_RESOURCE)) &&
            !(desc->BindFlags & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) &&
            !(desc->MiscFlags & ~D3D11_RESOURCE_MISC_BUFFER_STRUCTURED);
        // DEFAULT IA buffers with actual initial bytes are static until a
        // covered write proves otherwise. Do not confuse them with GPU-written
        // SO/UAV storage, dynamic maps, external/shared owners or guessed bytes.
        const bool staticInput = immutable || (c.coverage && c.creations &&
            desc->Usage == D3D11_USAGE_DEFAULT && !desc->CPUAccessFlags &&
            (desc->BindFlags & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER)) &&
            !(desc->BindFlags & ~(D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER | D3D11_BIND_SHADER_RESOURCE)) &&
            !(desc->MiscFlags & ~D3D11_RESOURCE_MISC_BUFFER_STRUCTURED));
        const bool matchable = staticInput && data && data->pSysMem && desc->ByteWidth && desc->ByteWidth <= CpuBudget - c.cpuBytes;
        const bool gpuBuffer = c.coverage && c.creations && desc->ByteWidth &&
            desc->Usage == D3D11_USAGE_DEFAULT && !desc->CPUAccessFlags &&
            (desc->BindFlags & (D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_UNORDERED_ACCESS)) &&
            !(desc->BindFlags & ~(D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE)) &&
            !(desc->MiscFlags & ~(D3D11_RESOURCE_MISC_BUFFER_STRUCTURED | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS));
        // Retain DEFAULT IB metadata even with no readable CPU contents.
        // Additional flags are recorded exactly and cannot match the bare
        // index_buffer rule. No initial bytes are read on this new path.
        const bool descriptorIndex = desc->ByteWidth && desc->Usage == D3D11_USAGE_DEFAULT &&
            (desc->BindFlags & D3D11_BIND_INDEX_BUFFER);
        if (!matchable && !vertexRead && !gpuBuffer && !descriptorIndex) return;
        auto lease = std::make_shared<GameResource>(); lease->resource = buffer; lease->buffer = buffer;
        lease->gpuBuffer = gpuBuffer; lease->originalBufferDesc = *desc;
        if (edit && edit->qualified && edit->deviceGeneration == c.generation)
        { lease->originalBufferDesc = edit->original; lease->resize = edit->rule; }
        lease->capturedInput = matchable; lease->vertexReadOnly = vertexRead;
        lease->bufferDesc = *desc; lease->readObject = Ids(); lease->readGeneration = c.generation;
        // Pure CBs cannot be UAV/SO/RT/shared/external writes. Larger 11.1
        // allocations and unknown descriptors remain unreadable, never clipped.
        const bool constant = c.coverage && c.creations && desc->BindFlags == D3D11_BIND_CONSTANT_BUFFER &&
            !desc->MiscFlags && !desc->StructureByteStride && desc->ByteWidth && !(desc->ByteWidth % 16) &&
            desc->ByteWidth <= ConstantByteLimit &&
            ((desc->Usage == D3D11_USAGE_DEFAULT && !desc->CPUAccessFlags) ||
             (desc->Usage == D3D11_USAGE_DYNAMIC && desc->CPUAccessFlags == D3D11_CPU_ACCESS_WRITE) || immutable);
        if (constant && 2ull * desc->ByteWidth <= ConstantCpuBudget - c.constantCpuBytes)
        {
            (void)ConstantMapStore(); // reserve fixed map owner off-draw at creation
            lease->constant = std::make_unique<ConstantContent>(); auto& cb = *lease->constant;
            cb.bytes.resize(desc->ByteWidth / 16); cb.staging.resize(desc->ByteWidth / 16);
            cb.generation = c.generation; cb.context = c.context.Get(); c.constantCpuBytes += 2ull * desc->ByteWidth;
            if (data && data->pSysMem && CopyConstantCpu(cb.Data(), data->pSysMem, desc->ByteWidth))
            { cb.begin = 0; cb.end = desc->ByteWidth; cb.published = cb.revision; }
        }
        CpuImage initial;
        if (lease->capturedInput && data && data->pSysMem && desc->ByteWidth && desc->ByteWidth <= CpuBudget - c.cpuBytes)
        {
            lease->cpu.resize(desc->ByteWidth); std::memcpy(lease->cpu.data(), data->pSysMem, desc->ByteWidth);
            initial = {lease->cpu.data(), lease->cpu.size(), 0}; c.cpuBytes += lease->cpu.size();
        }
        const auto id = lease->readObject;
        if (matchable) lease->record.CreateBuffer(id, c.generation, *desc, initial);
        else if (gpuBuffer)
        {
            const auto& original = lease->originalBufferDesc;
            CpuImage bytes{data ? data->pSysMem : nullptr, data && data->pSysMem ? original.ByteWidth : 0, 0};
            lease->record.CreateBuffer(id, c.generation, original, bytes);
        }
        else if (descriptorIndex) lease->record.CreateBufferDescriptor(id, c.generation, *desc);
        if (!c.resources.Add(Pointer(buffer), lease) || ((matchable || gpuBuffer || descriptorIndex) && !c.resourceIds.Add(id, lease)) ||
            !RegisterMapResource(buffer)) c.exhausted = true;
    }
    catch (...) { Store().coverageLost.store(true, std::memory_order_release); }
}
void TextureCreated(ID3D11Device* device, ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC* desc,
                    const D3D11_SUBRESOURCE_DATA* data) noexcept
{
    if (!Enabled() || !texture || !desc) return;
    try
    {
        auto& s = Store(); std::lock_guard lock(s.ownerMutex);
        if (!s.capture || s.capture->device.Get() != device) return;
        auto& c = *s.capture;
        if (c.hooks.internal && c.hooks.internal(c.context.Get())) return;
        if (c.resources.Find(Pointer(texture))) return;
        const bool immutable = desc->Usage == D3D11_USAGE_IMMUTABLE && !desc->CPUAccessFlags && desc->ArraySize == 1 &&
            desc->MipLevels == 1 && desc->SampleDesc.Count == 1 && desc->BindFlags == D3D11_BIND_SHADER_RESOURCE && !desc->MiscFlags;
        const bool vertexRead = c.coverage && c.creations && desc->Width && desc->Height && desc->ArraySize == 1 && desc->MipLevels == 1 &&
            desc->SampleDesc.Count == 1 && !desc->SampleDesc.Quality && desc->BindFlags == D3D11_BIND_SHADER_RESOURCE &&
            !desc->MiscFlags && (desc->Usage == D3D11_USAGE_DEFAULT || desc->Usage == D3D11_USAGE_DYNAMIC || immutable) &&
            !(desc->CPUAccessFlags & ~D3D11_CPU_ACCESS_WRITE) && FormatElementBytes(desc->Format);
        const bool matchable = immutable && data && data->pSysMem && c.hashPolicy.resolutionKnown;
        if (!matchable && !vertexRead) return;
        auto lease = std::make_shared<GameResource>(); lease->resource = texture; lease->texture = texture;
        lease->capturedInput = matchable; lease->vertexReadOnly = vertexRead;
        lease->textureDesc = *desc; lease->readObject = Ids(); lease->readGeneration = c.generation;
        CpuImage initial;
        // CPU pitch/size bounds use the resource module's typed format rules.
        SurfaceLayout surface;
        if (lease->capturedInput && data && data->pSysMem && FormatSurface(desc->Format, desc->Width, desc->Height, surface) &&
            data->SysMemPitch >= surface.rowBytes)
        {
            const auto bytes = std::uint64_t(surface.rows - 1) * data->SysMemPitch + surface.rowBytes;
            if (bytes && bytes <= CpuBudget - c.cpuBytes)
            {
                lease->cpu.resize(static_cast<std::size_t>(bytes)); std::memcpy(lease->cpu.data(), data->pSysMem, lease->cpu.size());
                initial = {lease->cpu.data(), lease->cpu.size(), data->SysMemPitch};
                c.cpuBytes += lease->cpu.size();
            }
        }
        lease->capturedInput = matchable && initial.data;
        if (!lease->capturedInput && !vertexRead) return;
        const auto id = lease->readObject;
        if (lease->capturedInput) lease->record.CreateTexture2D(id, c.generation, *desc, initial, c.hashPolicy);
        if (!c.resources.Add(Pointer(texture), lease) || (lease->capturedInput && !c.resourceIds.Add(id, lease)) ||
            !RegisterMapResource(texture)) c.exhausted = true;
    }
    catch (...) { Store().coverageLost.store(true, std::memory_order_release); }
}
void ViewCreated(ID3D11Device* device, ID3D11Resource* resource, ID3D11ShaderResourceView* view) noexcept
{
    if (!Enabled() || !resource || !view) return;
    try
    {
        auto& s = Store(); std::lock_guard lock(s.ownerMutex);
        if (!s.capture || s.capture->device.Get() != device) return;
        auto& c = *s.capture;
        if (c.hooks.internal && c.hooks.internal(c.context.Get())) return;
        if (c.views.Find(Pointer(view))) return;
        auto owner = c.resources.Find(Pointer(resource));
        if (!owner) return;
        auto lease = std::make_shared<GameView>(); lease->resource = std::move(owner); lease->srv = view; lease->id = Ids();
        D3D11_SHADER_RESOURCE_VIEW_DESC desc; view->GetDesc(&desc);
        if (desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D && desc.ViewDimension != D3D11_SRV_DIMENSION_BUFFER) return;
        lease->format = desc.Format;
        if (!c.views.Add(Pointer(view), lease) || !c.viewIds.Add(lease->id, lease)) c.exhausted = true;
    }
    catch (...) { Store().coverageLost.store(true, std::memory_order_release); }
}
// Slot ownership is determined from complete creation reflection, never from
// a previously empty binding or a shader name. Original t120 use stays
// unsupported even when a separate typed replacement capability exists.
static bool IniParameterSlotUnused(ID3D11ShaderReflection& reflection, const D3D11_SHADER_DESC& desc) noexcept
{
    if (desc.BoundResources > 256) return false;
    for (UINT i = 0; i < desc.BoundResources; ++i)
    {
        D3D11_SHADER_INPUT_BIND_DESC input{};
        if (FAILED(reflection.GetResourceBindingDesc(i, &input)) || !input.BindCount) return false;
        const bool srv = input.Type == D3D_SIT_TEXTURE || input.Type == D3D_SIT_TBUFFER ||
            input.Type == D3D_SIT_STRUCTURED || input.Type == D3D_SIT_BYTEADDRESS;
        if (srv && input.BindPoint <= IniParameterRegister && IniParameterRegister - input.BindPoint < input.BindCount) return false;
    }
    return true;
}
static Dx11VertexInputs ReflectVertexInputs(ID3D11ShaderReflection& reflection, const D3D11_SHADER_DESC& desc, bool vertex = true) noexcept
{
    Dx11VertexInputs out;
    constexpr UINT cbSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
    constexpr UINT srvSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
    constexpr UINT samplerSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
    if (desc.BoundResources > cbSlots + srvSlots + samplerSlots ||
        desc.InputParameters > D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT) return out;
    // BLEND/BONE semantics are ordinary qualified per-vertex values. Keep their
    // original VS computation; do not infer a bone ABI from their names.
    for (UINT i = 0; vertex && i < desc.InputParameters; ++i)
    {
        D3D11_SIGNATURE_PARAMETER_DESC input{};
        if (FAILED(reflection.GetInputParameterDesc(i, &input)) || input.SystemValueType != D3D_NAME_UNDEFINED) return out;
    }
    for (UINT i = 0; i < desc.BoundResources; ++i)
    {
        D3D11_SHADER_INPUT_BIND_DESC input{};
        if (FAILED(reflection.GetResourceBindingDesc(i, &input)) || !input.BindCount) return out;
        if (input.Type == D3D_SIT_CBUFFER)
        {
            if (input.BindCount != 1 || input.BindPoint >= cbSlots || out.constantBytes[input.BindPoint] || !input.Name) return out;
            auto* cb = reflection.GetConstantBufferByName(input.Name); D3D11_SHADER_BUFFER_DESC buffer{};
            if (!cb || FAILED(cb->GetDesc(&buffer)) || buffer.Type != D3D_CT_CBUFFER || !buffer.Size ||
                buffer.Size > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16u) return out;
            out.constantBytes[input.BindPoint] = buffer.Size;
        }
        else if (input.Type == D3D_SIT_SAMPLER)
        {
            if (input.BindPoint >= samplerSlots || input.BindCount > samplerSlots - input.BindPoint) return out;
            for (UINT slot = input.BindPoint; slot < input.BindPoint + input.BindCount; ++slot)
            { if (out.samplers & (1u << slot)) return out; out.samplers |= 1u << slot; }
        }
        else
        {
            Dx11VertexReadKind kind = Dx11VertexReadKind::None;
            if (input.Type == D3D_SIT_STRUCTURED && input.Dimension == D3D_SRV_DIMENSION_BUFFER)
                kind = Dx11VertexReadKind::StructuredBuffer;
            else if (input.Type == D3D_SIT_TEXTURE && input.Dimension == D3D_SRV_DIMENSION_BUFFER)
                kind = Dx11VertexReadKind::TypedBuffer;
            else if (input.Type == D3D_SIT_TEXTURE && input.Dimension == D3D_SRV_DIMENSION_TEXTURE2D)
                kind = Dx11VertexReadKind::Texture2D;
            // Raw/byte-address, tbuffer, UAV, interfaces, arrays/MS textures and
            // all unmodelled resource shapes stay explicitly Unsupported.
            if (kind == Dx11VertexReadKind::None || input.BindPoint >= srvSlots || input.BindCount > srvSlots - input.BindPoint) return out;
            if (kind != Dx11VertexReadKind::StructuredBuffer && input.ReturnType != D3D_RETURN_TYPE_FLOAT &&
                input.ReturnType != D3D_RETURN_TYPE_UNORM && input.ReturnType != D3D_RETURN_TYPE_SNORM &&
                input.ReturnType != D3D_RETURN_TYPE_UINT && input.ReturnType != D3D_RETURN_TYPE_SINT) return out;
            for (UINT slot = input.BindPoint; slot < input.BindPoint + input.BindCount; ++slot)
            {
                if (out.resources[slot].kind != Dx11VertexReadKind::None) return out;
                out.resources[slot] = {kind, input.ReturnType};
            }
        }
    }
    out.qualified = true;
    return out;
}
Dx11PixelInputs ReflectPixelInputs(ID3D11ShaderReflection& reflection, const D3D11_SHADER_DESC& desc) noexcept
{
    Dx11PixelInputs out;
    constexpr UINT srvSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
    constexpr UINT samplerSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
    if (desc.BoundResources > srvSlots + samplerSlots + D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) return out;
    for (UINT n = 0; n < desc.BoundResources; ++n)
    {
        D3D11_SHADER_INPUT_BIND_DESC input{};
        if (FAILED(reflection.GetResourceBindingDesc(n, &input)) || !input.BindCount) return out;
        if (input.Type == D3D_SIT_CBUFFER) continue; // unchanged, no new cbuffer ABI
        if (input.Type == D3D_SIT_SAMPLER)
        {
            if (input.BindPoint >= samplerSlots || input.BindCount > samplerSlots - input.BindPoint) return out;
            for (UINT slot = input.BindPoint; slot < input.BindPoint + input.BindCount; ++slot)
            {
                if (out.samplers & (1u << slot)) return out;
                out.samplers |= 1u << slot;
                if (input.uFlags & D3D_SIF_COMPARISON_SAMPLER) out.comparisonSamplers |= 1u << slot;
            }
            continue;
        }
        if ((input.Type != D3D_SIT_TEXTURE && input.Type != D3D_SIT_STRUCTURED && input.Type != D3D_SIT_BYTEADDRESS) ||
            input.BindPoint >= srvSlots || input.BindCount > srvSlots - input.BindPoint) return out;
        for (UINT slot = input.BindPoint; slot < input.BindPoint + input.BindCount; ++slot)
        {
            auto& shape = out.resources[slot];
            if (shape.declared) return out;
            shape = {true, input.Type, input.Dimension, input.ReturnType};
        }
    }
    out.qualified = true;
    return out;
}
void GeometryShaderCreated(ID3D11Device* device, ID3D11GeometryShader* shader, const void* bytes, SIZE_T size,
                           ID3D11ClassLinkage* linkage, const D3D11_SO_DECLARATION_ENTRY* entries, UINT count,
                           const UINT* strides, UINT strideCount, UINT rasterized, bool withOutput) noexcept
{
    if (!Enabled() || !device || !shader || !bytes || !size || size > 16u * 1024u * 1024u) return;
    try
    {
        auto& state = Store(); std::lock_guard lock(state.ownerMutex);
        if (!state.capture || state.capture->device.Get() != device) return;
        auto& c = *state.capture;
        if ((c.hooks.internal && c.hooks.internal(c.context.Get())) || c.shaders.Find(Pointer(shader))) return;
        auto owner = std::make_shared<Shader>(); owner->shader = shader;
        std::uint64_t hash = 0;
        for (SIZE_T i = 0; i < size; ++i) hash = hash * 1099511628211ULL ^ static_cast<const std::uint8_t*>(bytes)[i];
        owner->identity = {Knowledge::Known, hash, Ids(), c.generation};
        // Retain real creation even if reflection/shape is Unsupported. An
        // existing GS pointer or a successful API is not skinning qualification.
        ComPtr<ID3D11ShaderReflection> reflection; D3D11_SHADER_DESC desc{};
        if (!linkage && SUCCEEDED(D3DReflect(bytes, size, IID_PPV_ARGS(&reflection))) &&
            SUCCEEDED(reflection->GetDesc(&desc)) && !reflection->GetNumInterfaceSlots())
        {
            auto& so = owner->stream;
            const auto kind = D3D11_SHVER_GET_TYPE(desc.Version);
            so.vertexPassthrough = withOutput && kind == D3D11_SHVER_VERTEX_SHADER;
            owner->plain = kind == D3D11_SHVER_GEOMETRY_SHADER || so.vertexPassthrough;
            if (so.vertexPassthrough) { owner->inputs.qualified = true; so.maxVertices = so.instances = 1; }
            else if (kind == D3D11_SHVER_GEOMETRY_SHADER)
            {
                owner->inputs = ReflectVertexInputs(*reflection.Get(), desc, false);
                so.maxVertices = desc.GSMaxOutputVertexCount;
                so.instances = (std::max)(1u, desc.cGSInstanceCount);
            }
            so.entryCount = count; so.strideCount = strideCount; so.rasterizedStream = rasterized;
            bool valid = owner->plain && owner->inputs.qualified && withOutput && entries && count && count <= so.elements.size() &&
                strideCount <= so.strides.size() && (!strideCount || strides) && rasterized == D3D11_SO_NO_RASTERIZED_STREAM &&
                so.maxVertices && so.maxVertices <= 1024 && so.instances <= 32 &&
                (so.vertexPassthrough || (desc.InputPrimitive == D3D_PRIMITIVE_POINT && desc.GSOutputTopology == D3D_PRIMITIVE_TOPOLOGY_POINTLIST));
            std::array<UINT, D3D11_SO_BUFFER_SLOT_COUNT> packed{};
            for (UINT i = 0; valid && i < count; ++i)
            {
                const auto& e = entries[i]; auto& out = so.elements[i];
                if (e.Stream || e.OutputSlot >= so.strides.size() || !e.ComponentCount || e.ComponentCount > 4 ||
                    e.StartComponent > 3 || e.StartComponent + e.ComponentCount > 4) { valid = false; break; }
                out.stream = e.Stream; out.output = e.OutputSlot; out.start = e.StartComponent; out.count = e.ComponentCount;
                out.semanticIndex = e.SemanticIndex; out.gap = e.SemanticName == nullptr;
                if (e.SemanticName)
                {
                    const auto length = strnlen_s(e.SemanticName, out.semantic.size());
                    if (!length || length >= out.semantic.size()) { valid = false; break; }
                    std::memcpy(out.semantic.data(), e.SemanticName, length);
                    bool found = false;
                    for (UINT n = 0; n < desc.OutputParameters; ++n)
                    {
                        D3D11_SIGNATURE_PARAMETER_DESC sig{};
                        if (FAILED(reflection->GetOutputParameterDesc(n, &sig))) { valid = false; break; }
                        const UINT mask = ((1u << e.ComponentCount) - 1u) << e.StartComponent;
                        if (sig.SemanticName && !_stricmp(sig.SemanticName, e.SemanticName) && sig.SemanticIndex == e.SemanticIndex &&
                            sig.Stream == e.Stream && (sig.Mask & mask) == mask) found = true;
                    }
                    if (!valid || !found) { valid = false; break; }
                }
                packed[e.OutputSlot] += e.ComponentCount * 4u; so.slots |= 1u << e.OutputSlot;
            }
            for (UINT slot = 0; valid && slot < so.strides.size(); ++slot) if (so.slots & (1u << slot))
            {
                if (strideCount && slot >= strideCount) { valid = false; break; }
                so.strides[slot] = strideCount ? strides[slot] : packed[slot];
                if (so.strides[slot] < packed[slot] || so.strides[slot] % 4 || so.strides[slot] > D3D11_SO_BUFFER_MAX_STRIDE_IN_BYTES) valid = false;
            }
            so.qualified = valid;
        }
        if (!c.shaders.Add(Pointer(shader), owner)) c.exhausted = true;
    }
    catch (...) { Store().coverageLost.store(true, std::memory_order_release); }
}

void ShaderCreated(ID3D11Device* device, ID3D11DeviceChild* shader, const void* bytes, SIZE_T size,
                   bool vertex, bool hasLinkage) noexcept
{
    if (!Enabled() || !shader || !bytes || !size || size > 16u * 1024u * 1024u) return;
    try
    {
        auto& s = Store(); std::lock_guard lock(s.ownerMutex);
        if (!s.capture || s.capture->device.Get() != device) return;
        auto& c = *s.capture;
        if (c.hooks.internal && c.hooks.internal(c.context.Get())) return;
        if (c.shaders.Find(Pointer(shader))) return;
        // GIMI d3dx.ini: shader_hash=3dmigoto, unseeded FNV-1. This is
        // creation bytecode identity, unrelated to the SR classifier's hash.
        std::uint64_t hash = 0;
        for (SIZE_T i = 0; i < size; ++i) hash = (hash * 1099511628211ULL) ^ static_cast<const std::uint8_t*>(bytes)[i];
        auto owner = std::make_shared<Shader>(); owner->shader = shader; owner->vertex = vertex;
        owner->identity = {Knowledge::Known, hash, Ids(), c.generation};
        owner->sourceStatus = hasLinkage ? ShaderSourceStatus::UnsupportedModel : ShaderSourceStatus::ReflectionUnavailable;
        if (!hasLinkage)
        {
            ComPtr<ID3D11ShaderReflection> reflection; D3D11_SHADER_DESC desc{};
            if (SUCCEEDED(D3DReflect(bytes, size, IID_PPV_ARGS(&reflection))) && SUCCEEDED(reflection->GetDesc(&desc)) &&
                D3D11_SHVER_GET_TYPE(desc.Version) == (vertex ? D3D11_SHVER_VERTEX_SHADER : D3D11_SHVER_PIXEL_SHADER) &&
                !reflection->GetNumInterfaceSlots())
            {
                owner->plain = true;
                const auto major = D3D11_SHVER_GET_MAJOR(desc.Version), minor = D3D11_SHVER_GET_MINOR(desc.Version);
                if (major == 4 && minor == 0) owner->model = vertex ? ShaderModel::VS40 : ShaderModel::PS40;
                else if (major == 4 && minor == 1) owner->model = vertex ? ShaderModel::VS41 : ShaderModel::PS41;
                else if (major == 5 && minor == 0) owner->model = vertex ? ShaderModel::VS50 : ShaderModel::PS50;
                else if (major == 5 && minor == 1) owner->model = vertex ? ShaderModel::VS51 : ShaderModel::PS51;
                owner->iniParameterSlotUnused = IniParameterSlotUnused(*reflection.Get(), desc);
                if (vertex) owner->inputs = ReflectVertexInputs(*reflection.Get(), desc);
                else owner->pixelInputs = ReflectPixelInputs(*reflection.Get(), desc);
                const auto available = c.shaderSourceBytes <= GimiShaderSourceBudget ?
                    GimiShaderSourceBudget - c.shaderSourceBytes : 0;
                auto source = CaptureShaderSource(owner->identity, owner->model, bytes, size, *reflection.Get(), desc, available);
                owner->sourceStatus = source.status; owner->source = std::move(source.source);
            }
        }
        if (!c.shaders.Add(Pointer(shader), owner)) c.exhausted = true;
        else if (owner->source) c.shaderSourceBytes += owner->source->PayloadBytes();
    }
    catch (...) { Store().coverageLost.store(true, std::memory_order_release); }
}
ShaderSourceResult AcquireShaderSource(ID3D11Device* device, ID3D11DeviceChild* shader, ShaderIdentity original) noexcept
{
    if (!Enabled() || !device || !shader || original.knowledge != Knowledge::Known ||
        !original.object || !original.generation || original.object == UINT64_MAX || original.generation == UINT64_MAX)
        return {{}, ShaderSourceStatus::InvalidIdentity};
    try
    {
        auto& state = Store(); std::lock_guard lock(state.ownerMutex);
        if (!state.capture || state.capture->device.Get() != device || state.capture->generation != original.generation)
            return {{}, ShaderSourceStatus::InvalidIdentity};
        const auto owner = state.capture->shaders.Find(Pointer(shader));
        if (!owner || owner->identity.knowledge != Knowledge::Known || owner->identity.object != original.object ||
            owner->identity.generation != original.generation || owner->identity.hash != original.hash)
            return {{}, ShaderSourceStatus::InvalidIdentity};
        return {owner->source, owner->sourceStatus}; // independent immutable lifetime; no COM pointer returned
    }
    catch (...) { return {{}, ShaderSourceStatus::Unknown}; }
}
void LayoutCreated(ID3D11Device* device, ID3D11InputLayout* layout, const D3D11_INPUT_ELEMENT_DESC* elements, UINT count) noexcept
{
    if (!Enabled() || !layout || !elements || !count) return;
    try
    {
        auto& s = Store(); std::lock_guard lock(s.ownerMutex);
        if (!s.capture || s.capture->device.Get() != device) return;
        auto& c = *s.capture;
        if (c.hooks.internal && c.hooks.internal(c.context.Get())) return;
        if (c.layouts.Find(Pointer(layout))) return;
        auto owner = std::make_shared<Layout>(); owner->layout = layout; owner->id = Ids(); owner->exact = count <= 32;
        if (owner->exact) for (UINT i = 0; i < count; ++i)
        {
            const auto& e = elements[i];
            if (!e.SemanticName || strlen(e.SemanticName) > 64 || e.InputSlot >= GimiVertexSlots ||
                e.AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT || e.InputSlotClass != D3D11_INPUT_PER_VERTEX_DATA || e.InstanceDataStepRate)
            { owner->exact = false; break; }
            // Semantic spelling is not a private skeleton ABI. Preserve the
            // original qualified VS computation and validate exact descriptors;
            // no replacement shader/guessed blend decoding is introduced.
            owner->elements.push_back({e.SemanticName, e.SemanticIndex, e.InputSlot, e.AlignedByteOffset, e.Format});
            owner->slots |= 1u << e.InputSlot;
        }
        if (!c.layouts.Add(Pointer(layout), owner)) c.exhausted = true;
    }
    catch (...) { Store().coverageLost.store(true, std::memory_order_release); }
}
static void InvalidateReadWrite(Capture& capture, GameResource& resource, bool constantWrite = true) noexcept
{
    if (constantWrite && resource.constant) resource.constant->Revoke();
    // Mutable pass-through values are not content-match/cache identities.
    // Revoke the current invocation without invalidating static match caches.
    if (resource.capturedInput)
    {
        resource.record.OpaqueWrite();
        if (capture.invalidation == UINT64_MAX) capture.exhausted = true; else ++capture.invalidation;
    }
    if (resource.gpuBuffer)
    {
        resource.gpuOrdered = false; resource.soCursorKnown = false;
        if (resource.gpuRevision == UINT64_MAX) capture.exhausted = true; else ++resource.gpuRevision;
        if (capture.invalidation == UINT64_MAX) capture.exhausted = true; else ++capture.invalidation;
    }
    if (resource.vertexReadOnly || resource.gpuBuffer)
    {
        if (capture.vertexReadRevision == UINT64_MAX) capture.exhausted = true;
        else ++capture.vertexReadRevision;
    }
}
std::uint64_t VertexMapStarting(ID3D11DeviceContext* context, ID3D11Resource* resource,
                               UINT sub, D3D11_MAP type) noexcept
{
    if (!MapResourceRegistered(resource)) return 0;
    auto& s = Store(); std::unique_lock lock(s.ownerMutex, std::try_to_lock);
    if (!lock.owns_lock()) { s.coverageLost.store(true, std::memory_order_release); return 0; }
    if (!s.capture) return 0;
    const auto game = s.capture->resources.Find(Pointer(resource));
    if (!game) return 0;
    if (type != D3D11_MAP_READ) InvalidateReadWrite(*s.capture, *game);
    if (!game->vertexReadOnly && !game->gpuBuffer && !game->capturedInput) return 0;
    if (context != s.capture->context.Get() || (s.active && GetCurrentThreadId() != s.active->thread) || sub || game->mapped)
    { s.coverageLost.store(true, std::memory_order_release); return 0; }
    // Latch BEFORE the real API, including its pending interval. Success stays
    // mapped through all CPU writes until the real Unmap returns.
    if (game->constant)
    {
        auto& cb = *game->constant; cb.mapRevision = cb.revision;
        cb.mapContract = contractRevision.load(std::memory_order_acquire); cb.mapThread = GetCurrentThreadId();
    }
    game->mapped = true;
    return game->readGeneration;
}
void VertexMapFailed(ID3D11DeviceContext* context, ID3D11Resource* resource,
                     UINT sub, std::uint64_t generation) noexcept
{
    if (!generation || !MapResourceRegistered(resource)) return;
    auto& s = Store(); std::unique_lock lock(s.ownerMutex, std::try_to_lock);
    if (!lock.owns_lock()) { s.coverageLost.store(true, std::memory_order_release); return; }
    if (!s.capture || s.capture->generation != generation) return;
    const auto game = s.capture->resources.Find(Pointer(resource));
    if (!game || (!game->vertexReadOnly && !game->gpuBuffer && !game->capturedInput)) return;
    if (context != s.capture->context.Get() || (s.active && GetCurrentThreadId() != s.active->thread) || sub || !game->mapped)
    { s.coverageLost.store(true, std::memory_order_release); return; }
    game->mapped = false;
    if (game->constant) game->constant->Revoke(); // failure never revives the old image
}
void VertexUnmapped(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT sub) noexcept
{
    if (!MapResourceRegistered(resource)) return;
    auto& s = Store(); std::unique_lock lock(s.ownerMutex, std::try_to_lock);
    if (!lock.owns_lock()) { s.coverageLost.store(true, std::memory_order_release); return; }
    if (!s.capture) return;
    const auto game = s.capture->resources.Find(Pointer(resource));
    if (!game) return;
    // CpuContentSubmission::Unmap has independently revoked/frozen this CB;
    // do not revoke its exact pending return receipt a second time.
    InvalidateReadWrite(*s.capture, *game, false);
    if (!game->vertexReadOnly && !game->gpuBuffer && !game->capturedInput) return;
    if (context != s.capture->context.Get() || (s.active && GetCurrentThreadId() != s.active->thread) || sub || !game->mapped)
    { s.coverageLost.store(true, std::memory_order_release); return; }
    game->mapped = false;
    if (game->constant)
    { auto& cb = *game->constant; cb.mapRevision = cb.mapContract = 0; cb.mapThread = 0; }
}
void Written(ID3D11DeviceContext* context, ID3D11Resource* resource) noexcept
{
    if (!Enabled() || !resource) return;
    auto& s = Store(); std::unique_lock lock(s.ownerMutex, std::try_to_lock);
    if (!lock.owns_lock()) { s.coverageLost.store(true, std::memory_order_release); return; }
    if (!s.capture) return;
    const auto owner = s.capture->resources.Find(Pointer(resource));
    if (owner)
    {
        if ((owner->vertexReadOnly || owner->gpuBuffer || owner->capturedInput) && (context != s.capture->context.Get() ||
            (s.active && GetCurrentThreadId() != s.active->thread)))
        { s.coverageLost.store(true, std::memory_order_release); return; }
        InvalidateReadWrite(*s.capture, *owner);
    }
}
// Every factory is allocation-free after resource creation. The existing hook
// calls Written before Update/Copy, revoking the old source regardless of whether
// a new legal CPU image can be frozen. Void D3D writes have no success HRESULT:
// publication additionally requires this supported legal descriptor/range and
// the exact unchanged API return, owner, revision and configuration epoch.
bool CpuContentSubmission::HasPendingMaps() noexcept { return constantMapCount.load(std::memory_order_acquire) != 0; }
static bool ConstantUnconditional(ID3D11DeviceContext* context) noexcept
{
    ComPtr<ID3D11Predicate> predicate; BOOL expected = FALSE;
    context->GetPredication(&predicate, &expected);
    return !predicate; // a returned conditional command is not proof of a write
}
CpuContentSubmission CpuContentSubmission::Update(ID3D11DeviceContext* context, ID3D11Resource* resource,
    UINT sub, const D3D11_BOX* box, const void* data, UINT flags) noexcept
{
    CpuContentSubmission out;
    if (!MapResourceRegistered(resource)) return out;
    auto& s = Store(); std::unique_lock lock(s.ownerMutex, std::try_to_lock);
    if (!lock.owns_lock()) { s.coverageLost.store(true, std::memory_order_release); return out; }
    const auto capture = s.capture; if (!capture) return out;
    const auto game = capture->resources.Find(Pointer(resource));
    if (!game || !game->constant) return out;
    auto& cb = *game->constant;
    if (sub || box || flags || !data || context != capture->context.Get() ||
        (s.active && GetCurrentThreadId() != s.active->thread) || game->mapped || cb.pending || cb.revision == UINT64_MAX ||
        game->bufferDesc.Usage != D3D11_USAGE_DEFAULT || !capture->coverage || !capture->creations ||
        s.coverageLost.load(std::memory_order_acquire)) return out;
    if (!ConstantUnconditional(context) || !CopyConstantCpu(cb.Staging(), data, game->bufferDesc.ByteWidth)) return out;
    cb.pending = true; out.capture_ = capture; out.resource_ = game; out.revision_ = cb.revision;
    out.contract_ = contractRevision.load(std::memory_order_acquire); out.thread_ = GetCurrentThreadId();
    out.end_ = game->bufferDesc.ByteWidth; return out;
}
CpuContentSubmission CpuContentSubmission::Copy(ID3D11DeviceContext* context, ID3D11Resource* destination,
    ID3D11Resource* source, UINT sub, UINT x, UINT y, UINT z, UINT sourceSub, const D3D11_BOX* box,
    UINT flags, bool whole) noexcept
{
    CpuContentSubmission out;
    if (!MapResourceRegistered(destination) || !source || source == destination) return out;
    auto& s = Store(); std::unique_lock lock(s.ownerMutex, std::try_to_lock);
    if (!lock.owns_lock()) { s.coverageLost.store(true, std::memory_order_release); return out; }
    const auto capture = s.capture; if (!capture) return out;
    const auto target = capture->resources.Find(Pointer(destination)), from = capture->resources.Find(Pointer(source));
    if (!target || !from || !target->constant || !from->constant) return out;
    auto& cb = *target->constant; const auto& input = *from->constant;
    const auto width = target->bufferDesc.ByteWidth;
    // Constant-buffer copies are whole-subresource only. No GPU readback or
    // inferred source data; a partial known source range remains partial.
    const bool fullBox = !box || (!box->left && box->right == width && !box->top && box->bottom == 1 && !box->front && box->back == 1);
    if (sub || sourceSub || x || y || z || flags || (!whole && !fullBox) || width != from->bufferDesc.ByteWidth ||
        context != capture->context.Get() || (s.active && GetCurrentThreadId() != s.active->thread) ||
        target->mapped || from->mapped || cb.pending || input.pending || cb.revision == UINT64_MAX ||
        target->bufferDesc.Usage != D3D11_USAGE_DEFAULT || !input.published || input.published != input.revision ||
        input.revision == UINT64_MAX || input.begin >= input.end || input.end > width ||
        input.context != context || input.generation != capture->generation ||
        !capture->coverage || !capture->creations || s.coverageLost.load(std::memory_order_acquire)) return out;
    if (!ConstantUnconditional(context) || !CopyConstantCpu(cb.Staging() + input.begin,
        reinterpret_cast<const std::uint8_t*>(input.bytes.data()) + input.begin, input.end - input.begin)) return out;
    cb.pending = true; out.capture_ = capture; out.resource_ = target; out.revision_ = cb.revision;
    out.contract_ = contractRevision.load(std::memory_order_acquire); out.thread_ = GetCurrentThreadId();
    out.begin_ = input.begin; out.end_ = input.end; out.source_ = from; out.sourceRevision_ = input.revision; return out;
}
void ConstantMapped(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT sub, D3D11_MAP type, UINT flags,
    D3D11_MAPPED_SUBRESOURCE* mapped, HRESULT result, std::uint64_t generation, bool knownCpuShadow) noexcept
{
    if (!generation || FAILED(result) || !mapped || !mapped->pData || !MapResourceRegistered(resource)) return;
    auto& s = Store(); std::unique_lock lock(s.ownerMutex, std::try_to_lock);
    if (!lock.owns_lock()) { s.coverageLost.store(true, std::memory_order_release); return; }
    const auto capture = s.capture;
    if (!capture || generation != capture->generation) return;
    const auto game = capture->resources.Find(Pointer(resource));
    if (!game || !game->constant) return;
    auto& cb = *game->constant;
    // NO_OVERWRITE/WRITE with unknown changed bytes is deliberately invalid.
    // Never read write-combined driver RAM or pretend it is a current image.
    if (context != capture->context.Get() || (s.active && GetCurrentThreadId() != s.active->thread) || sub ||
        (flags & ~D3D11_MAP_FLAG_DO_NOT_WAIT) || type != D3D11_MAP_WRITE_DISCARD ||
        game->bufferDesc.Usage != D3D11_USAGE_DYNAMIC || game->bufferDesc.CPUAccessFlags != D3D11_CPU_ACCESS_WRITE ||
        !game->mapped || cb.pending || cb.revision == UINT64_MAX || cb.mapRevision != cb.revision ||
        cb.mapThread != GetCurrentThreadId() || cb.mapContract != contractRevision.load(std::memory_order_acquire) ||
        s.coverageLost.load(std::memory_order_acquire)) return;
    auto& maps = ConstantMapStore(); std::lock_guard mapLock(maps.mutex);
    ConstantMap* free = nullptr;
    for (auto& map : maps.slots)
    {
        if (map.resource == game) return;
        if (!map.resource && !free) free = &map;
    }
    if (!free) return; // leaves the real API pointer untouched, source invalid
    ConstantMap map; map.capture = capture; map.resource = game; map.context = context;
    map.driver = mapped->pData; map.revision = cb.revision;
    map.contract = cb.mapContract; map.thread = cb.mapThread;
    map.writeback = !knownCpuShadow;
    if (map.writeback)
    {
        // DISCARD defines no old contents. These bytes are actually written to
        // the real allocation on Unmap; zero is never a missing-store fallback.
        std::memset(cb.Staging(), 0, game->bufferDesc.ByteWidth);
        map.cpu = cb.Staging();
    }
    else map.cpu = mapped->pData; // borrowed only until the existing owner Unmap
    *free = std::move(map); cb.pending = true; constantMapCount.fetch_add(1, std::memory_order_release);
    if (free->writeback) mapped->pData = cb.Staging();
}
CpuContentSubmission CpuContentSubmission::Unmap(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT sub, bool observe) noexcept
{
    CpuContentSubmission out;
    if (!constantMapCount.load(std::memory_order_acquire))
    { if (observe && Enabled()) Written(context, resource); return out; }
    auto& maps = ConstantMapStore(); ConstantMap map;
    if (constantMapCount.load(std::memory_order_acquire))
    {
        std::lock_guard mapLock(maps.mutex);
        for (auto& candidate : maps.slots)
            if (!sub && candidate.resource && candidate.resource->resource.Get() == resource && candidate.context.Get() == context)
            { map = std::move(candidate); candidate = {}; constantMapCount.fetch_sub(1, std::memory_order_release); break; }
    }
    if (!map.resource) { if (observe && Enabled()) Written(context, resource); return out; }
    auto& cb = *map.resource->constant; const auto width = map.resource->bufferDesc.ByteWidth;
    // Mandatory before any metadata lock/check, including wrong-thread/off/loss.
    // Existing shadow owns its own unchanged writeback; we only freeze its bytes.
    if (map.writeback) std::memcpy(map.driver, cb.Staging(), width);
    const bool frozen = map.writeback || CopyConstantCpu(cb.Staging(), map.cpu, width);
    auto& s = Store(); std::unique_lock lock(s.ownerMutex, std::try_to_lock);
    if (!lock.owns_lock()) { s.coverageLost.store(true, std::memory_order_release); return out; }
    const auto capture = map.capture.lock();
    if (!Enabled() || !capture || s.capture != capture || !frozen || map.thread != GetCurrentThreadId() ||
        map.contract != contractRevision.load(std::memory_order_acquire) || cb.revision != map.revision || !cb.pending ||
        !map.resource->mapped || cb.mapRevision != map.revision || s.coverageLost.load(std::memory_order_acquire)) { cb.Revoke(); return out; }
    out.capture_ = capture; out.resource_ = std::move(map.resource);
    out.revision_ = map.revision; out.contract_ = map.contract; out.thread_ = map.thread; out.end_ = width;
    return out;
}
void CpuContentSubmission::Returned() noexcept
{
    const auto game = std::static_pointer_cast<GameResource>(std::move(resource_));
    const auto capture = std::static_pointer_cast<Capture>(std::move(capture_));
    const auto source = std::static_pointer_cast<GameResource>(std::move(source_));
    if (!game || !capture || !game->constant) return;
    auto& s = Store(); std::unique_lock lock(s.ownerMutex, std::try_to_lock);
    if (!lock.owns_lock()) { s.coverageLost.store(true, std::memory_order_release); return; }
    auto& cb = *game->constant;
    if (cb.revision != revision_) return; // newer exact writer owns its own latch
    if (source && (!source->constant || source->mapped || source->constant->pending ||
        source->constant->revision != sourceRevision_ || source->constant->published != sourceRevision_))
    { cb.Revoke(); return; }
    if (!Enabled() || s.capture != capture || contract_ != contractRevision.load(std::memory_order_acquire) ||
        thread_ != GetCurrentThreadId() || game->mapped || !cb.pending || !capture->coverage || !capture->creations ||
        capture->exhausted || capture->opaque || s.coverageLost.load(std::memory_order_acquire) ||
        begin_ >= end_ || end_ > game->bufferDesc.ByteWidth)
    { cb.Revoke(); return; }
    std::memcpy(cb.Data() + begin_, cb.Staging() + begin_, end_ - begin_);
    cb.begin = begin_; cb.end = end_; cb.published = cb.revision; cb.pending = false;
    cb.context = capture->context.Get(); cb.generation = capture->generation;
}
bool RasterReturnScope::Arm(ID3D11DeviceContext* context, std::uint64_t ticket) noexcept
{
    if (context_ != context || ticket_ || !ticket) return false;
    ticket_ = ticket; return true;
}
RasterReturnScope::~RasterReturnScope() { if (ticket_) Complete(false); }
void RasterReturnScope::Returned() noexcept { if (ticket_) Complete(true); }
void RasterReturnScope::Complete(bool actual) noexcept
{
    const auto ticket = std::exchange(ticket_, 0);
    auto& s = Store(); std::unique_lock lock(s.ownerMutex, std::try_to_lock);
    if (!lock.owns_lock()) { s.coverageLost.store(true, std::memory_order_release); return; }
    auto session = s.active;
    if (!session || s.observedRasterTicket != ticket || context_ != session->capture->context.Get()) return;
    const bool paired = actual && Enabled() && s.capture == session->capture &&
        ticket == session->invocation && s.loaded == session->observedContract &&
        contractRevision.load(std::memory_order_acquire) == session->observedContract &&
        session->Current(session->capture->device.Get(), context_, session->snapshot.epochs) &&
        SUCCEEDED(session->capture->device->GetDeviceRemovedReason());
    if (paired) { session->commands->Accept(); ++s.observation.executed; }
    else { session->commands->Discard(); ++s.observation.unknown; }
    s.observedRasterTicket = 0;
}
void ViewWritten(ID3D11DeviceContext* context, ID3D11View* view) noexcept
{
    if (!Enabled() || !view) return;
    ComPtr<ID3D11Resource> resource; view->GetResource(&resource); Written(context, resource.Get());
}
void OpaqueBoundary(ID3D11DeviceContext*) noexcept
{
    if (!Enabled()) return;
    // Unknown deferred/opaque writes cannot be certified by the immediate
    // context's resource subset. Whole component becomes Unsupported.
    Store().coverageLost.store(true, std::memory_order_release);
}
bool TryDrawIndexed(ID3D11DeviceContext* c, UINT count, UINT first, INT base, Dx11OriginalDrawIndexed original, RasterReturnScope* returned) noexcept
{
    if (!Enabled()) return false; // before Store, owner lock, metadata or getters
    auto& s = Store(); std::unique_lock lock(s.ownerMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    if (s.coverageLost.load(std::memory_order_acquire)) { LostCoverage(s); return false; }
    if (!s.active || !s.capture || s.capture->opaque || s.capture->exhausted) return false;
    if (c == s.capture->context.Get() && GetCurrentThreadId() != s.active->thread)
    {
        // A different real raster issuer revokes the serial immediate-owner
        // assumption. Do not migrate a mutable interpreter between threads.
        s.coverageLost.store(true, std::memory_order_release); LostCoverage(s); return false;
    }
    if (s.nativePresentTicket || s.observedRasterTicket) return false; // preserve wrong-thread revocation
    return s.active->Raster(c, count, first, base, true, original, nullptr, false, 1, 0, nullptr, returned);
}
bool TryDrawIndexedInstanced(ID3D11DeviceContext* c, UINT count, UINT instances, UINT first, INT base,
                            UINT firstInstance, Dx11OriginalDrawIndexedInstanced original, RasterReturnScope* returned) noexcept
{
    if (!Enabled()) return false;
    auto& s = Store(); std::unique_lock lock(s.ownerMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    if (s.coverageLost.load(std::memory_order_acquire)) { LostCoverage(s); return false; }
    if (!s.active || !s.capture || s.capture->opaque || s.capture->exhausted) return false;
    if (c == s.capture->context.Get() && GetCurrentThreadId() != s.active->thread)
    { s.coverageLost.store(true, std::memory_order_release); LostCoverage(s); return false; }
    if (s.nativePresentTicket || s.observedRasterTicket) return false;
    return s.active->Raster(c, count, first, base, true, nullptr, nullptr, true, instances, firstInstance, original, returned);
}
bool TryDraw(ID3D11DeviceContext* c, UINT count, UINT first, Dx11OriginalDraw original, RasterReturnScope* returned) noexcept
{
    if (!Enabled()) return false; // before Store, lock, getters or plan queries
    auto& s = Store(); std::unique_lock lock(s.ownerMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    if (s.coverageLost.load(std::memory_order_acquire)) { LostCoverage(s); return false; }
    if (!s.active || !s.capture || s.capture->opaque || s.capture->exhausted) return false;
    if (c == s.capture->context.Get() && GetCurrentThreadId() != s.active->thread)
    {
        s.coverageLost.store(true, std::memory_order_release); LostCoverage(s); return false;
    }
    if (s.nativePresentTicket || s.observedRasterTicket) return false; // no concurrent interpreter mutation
    return s.active->Raster(c, count, first, 0, false, nullptr, original, false, 1, 0, nullptr, returned);
}
void Present(ID3D11Device* device, ID3D11DeviceContext* context, UINT width, UINT height) noexcept
{
    if (!NeedsPresent()) return;
    try
    {
        auto& s = Store(); std::lock_guard lock(s.ownerMutex);
        // First retire all old plans, independent of current configuration and
        // even of the current Present's device. Each uses its retained original
        // device/context/fence, never a newly selected SR backend's queue.
        for (auto& old : s.retired) if (old)
        {
            old->RetireAtPresent();
            if (!old->HasPackets()) old.reset();
        }
        if (s.active) s.active->RetireAtPresent();
        // Overlay preparation can reenter here after global PRE. Retirement is
        // still real, but plan/alias/variable preparation cannot cross its pair.
        if (s.observedRasterTicket && s.coverageLost.load(std::memory_order_acquire)) LostCoverage(s);
        if (s.nativePresentTicket || s.observedRasterTicket) return;
        if (s.coverageLost.load(std::memory_order_acquire)) LostCoverage(s);
        if (Enabled() && s.capture && device == s.capture->device.Get() && context == s.capture->context.Get())
            s.capture->ObserveResolution(width, height);
        Selection choice; std::uint64_t revision;
        { std::lock_guard request(s.requestMutex); choice = s.selection; revision = s.revision; }
        if (s.active && s.active->commands->CanInitialize())
        {
            // Drain queued real snapshots outside PRE/POST, before saving or
            // replacing the plan. Configure's new revision stops new delivery;
            // already observed input is not replayed into a replacement plan.
            s.active->ConsumeInput();
            if (!s.active->SaveUserState(revision != s.loaded, revision)) return;
        }
        if (revision != s.loaded)
        {
            Park(s);
            if (!s.active)
            {
                s.loaded = revision;
                s.observation.hostRoot[0] = 0;
                s.observation.hostNativeSettings = s.observation.hostSourceSettings = s.observation.hostInactiveSettings = 0;
                if (Enabled() && choice.enabled && s.capture && device == s.capture->device.Get() && context == s.capture->context.Get())
                {
                    s.capture->hashPolicy.resolutionKnown = width && height;
                    s.capture->hashPolicy.screenWidth = width; s.capture->hashPolicy.screenHeight = height;
                    s.active = Load(s, choice);
                }
                else if (!choice.enabled) s.Message("Disabled; submitted packets continue authentic Present retirement");
                else s.Message("UNKNOWN: waiting for captured game device; enable before creation/restart");
            }
        }
        // Old sessions independently own their capture/alias/resource graph.
        // Dropping this idle root handle cannot drop a submitted packet; it
        // also avoids pinning all creation observations after MOD is disabled.
        if (!Enabled() && !s.active)
        { mapCaptureLive.store(false, std::memory_order_release); s.capture.reset(); }
        if (s.active && s.active->accepting && Enabled() && device == s.active->capture->device.Get() &&
            context == s.active->capture->context.Get() && GetCurrentThreadId() == s.active->thread)
        { s.active->PrepareDescriptors(); s.active->PrepareWindows(); s.active->PreparePipeline(); }
        s.observation.pending = s.observation.quarantined = 0;
        const auto count = [&](const std::shared_ptr<Session>& session) {
            if (!session) return;
            for (const auto& p : session->packets)
                if (p.state == Session::PacketState::Quarantine) ++s.observation.quarantined;
                else if (p.state != Session::PacketState::Empty) ++s.observation.pending;
        };
        count(s.active); for (const auto& old : s.retired) count(old);
        presentNeeded.store(Enabled() || s.observation.pending || s.observation.quarantined || s.loaded != revision,
                            std::memory_order_release);
    }
    catch (const std::exception& e)
    {
        auto& s = Store(); std::lock_guard lock(s.ownerMutex); s.Message(e.what());
        if (s.active) s.active->accepting = false;
    }
    catch (...) { Store().coverageLost.store(true, std::memory_order_release); }
}
void NativePresentScope::Begin(IDXGISwapChain* chain, UINT sync, UINT flags, bool present1) noexcept
{
    if (session_) { Finish(false, nullptr, 0, 0, false, S_OK); return; }
    if (!Enabled() || !chain || (flags & DXGI_PRESENT_TEST)) return;
    const auto frameTicks = NativeTicks(); // this real PRE; not a timer advanced at POST
    // COM/creation callbacks are allowed to reenter. This guard only suppresses
    // nested admission while gathering real identities; it never fabricates a
    // native return. In-flight recursion is independently rejected below.
    static thread_local bool gathering = false;
    if (gathering) return;
    gathering = true;
    struct Ungather { bool& value; ~Ungather() { value = false; } } ungather{gathering};
    // Declared before ownerMutex: its user32 restore runs only AFTER that lock
    // is destroyed. A rejected complete PRE must not leave hardware hidden.
    struct CursorAdmission
    {
        bool retained = false;
        ~CursorAdmission() { if (!retained) OptiInput::ReleaseGameCursor(); }
    } cursorAdmission;
    try
    {
        ComPtr<IDXGISwapChain> retainedChain = chain;
        ComPtr<ID3D11Device> device;
        if (FAILED(chain->GetDevice(IID_PPV_ARGS(&device))) || !device) return;
        ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context);
        if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
        ComPtr<ID3D11Device> contextDevice; context->GetDevice(&contextDevice);
        if (contextDevice.Get() != device.Get()) return;
        auto& s = Store(); std::unique_lock lock(s.ownerMutex);
        auto session = s.active;
        if (!session || !s.capture || session->capture != s.capture ||
            device.Get() != s.capture->device.Get() || context.Get() != s.capture->context.Get()) return;
        if (s.nativePresentTicket || s.observedRasterTicket)
        {
            // No second PRE, even for another swap chain on this same context.
            // Preserve the first token until its owner actually returns/cancels.
            session->accepting = false; ++s.observation.nativePresent.rejected;
            s.Message("Unsupported: reentrant/concurrent native Present; global POST refused"); return;
        }
        if (!Enabled() || !session->accepting || session->quarantined || s.capture->opaque || s.capture->exhausted ||
            !s.capture->coverage || !s.capture->creations || s.coverageLost.load(std::memory_order_acquire) ||
            GetCurrentThreadId() != session->thread || s.loaded != contractRevision.load(std::memory_order_acquire)) return;
        // XXMI updates TIME at real RunFrameActions PRE, even if no global
        // command consumes it. Keep that SAME native-frame value for draws;
        // no per-draw clock query or guessed startup time/default is allowed.
        session->drawFrameTimeKnown = false;
        if (frameTicks >= nativeLaunchTicks)
        {
            const float seconds = static_cast<float>(frameTicks - nativeLaunchTicks) / 1'000'000.0f;
            if (std::isfinite(seconds)) { session->drawFrameTime = seconds; session->drawFrameTimeKnown = true; }
        }
        if (session->presentPrograms.empty()) return;
        const auto revision = s.loaded;
        const auto requirements = session->presentValues;
        const auto allValues = static_cast<std::uint16_t>(requirements[0] | requirements[1]);
        const auto ticket = Ids(); // also binds the input owner's immutable snapshot
        lock.unlock(); // user32/shcore calls must not hold the MOD interpreter owner lock
        NativePresentValues values;
        if ((allValues & NativeTime) && frameTicks >= nativeLaunchTicks)
        {
            const float seconds = static_cast<float>(frameTicks - nativeLaunchTicks) / 1'000'000.0f;
            if (std::isfinite(seconds))
            { values.values[static_cast<unsigned>(NativeValue::Time)] = seconds; values.known |= NativeTime; }
        }
        HWND window = nullptr; DWORD windowThread = 0;
        if ((allValues & NativeWindowValues) || session->presentCompute) NativeWindowIdentity(chain, window, windowThread);
        SampleNativeWindow(window, windowThread, requirements[0], values);
        SampleNativeRenderTarget(context.Get(), device.Get(), requirements[0], values);
        SampleNativeResolution(chain, device.Get(), static_cast<std::uint16_t>(requirements[0] |
            ((requirements[0] & NativeRenderTargetValues) ? NativeResolutionValues : 0)), values);
        OptiInput::GameCursorSnapshot cursor;
        if (requirements[0] & NativeCursorValues)
        {
            if (!OptiInput::CaptureGameCursor(window, ticket, session->plan->HideCursor(), cursor)) return;
            // Window size and cursor values use the SAME input owner, native
            // epoch and OS client-coordinate domain; no menu/DPI fallback.
            NativeFact(values, NativeValue::WindowWidth, static_cast<float>(cursor.clientRect.right));
            NativeFact(values, NativeValue::WindowHeight, static_cast<float>(cursor.clientRect.bottom));
            NativeFact(values, NativeValue::EffectiveDpi, static_cast<float>(cursor.dpi));
            NativeFact(values, NativeValue::CursorWindowX, static_cast<float>(cursor.client.x));
            NativeFact(values, NativeValue::CursorWindowY, static_cast<float>(cursor.client.y));
            NativeFact(values, NativeValue::CursorHotspotX, static_cast<float>(cursor.shape->hotspot.x));
            NativeFact(values, NativeValue::CursorHotspotY, static_cast<float>(cursor.shape->hotspot.y));
            NativeFact(values, NativeValue::CursorShowing, cursor.showing ? 1.0f : 0.0f);
        }
        // Requirements are potential reads, not an eager Has(all-branches) gate.
        // The existing interpreter follows the original branches and refuses
        // an unknown operand only if actually evaluated, with full rollback.
        lock.lock();
        // Config/coverage and the serial owner may change while OS queries run.
        // No phase has committed, and no token has been reserved at this point.
        if (!Enabled() || s.active != session || s.capture != session->capture || s.nativePresentTicket || s.observedRasterTicket ||
            !session->accepting || session->quarantined || session->capture->opaque || session->capture->exhausted ||
            !session->capture->coverage || !session->capture->creations ||
            s.coverageLost.load(std::memory_order_acquire) || s.loaded != revision ||
            contractRevision.load(std::memory_order_acquire) != revision) return;
        if (session->presentCompute)
        {
            CommandQueries queries; queries.context = session.get(); queries.readParameter = &Session::NativeParameter;
            queries.filterIndex = &Session::NativeFilter; queries.readBinding = &Session::NativeBinding;
            const auto prepared = session->commands->PrepareNativePresent(session->presentPrograms.data(),
                static_cast<UINT>(session->presentPrograms.size()), values, queries);
            if (!prepared)
            {
                ++s.observation.nativePresent.rejected; session->accepting = false;
                s.Message("Unsupported: complete native text PRE tape failed before GPU work"); return;
            }
            // Preparing the authentic RTV may call D3D creation hooks. Keep
            // the existing internal suppression active through preparation
            // and submission, but reserve the complete packet before GPU work.
            std::uint64_t nesting = 0;
            if (!session->EnterInternal(context.Get(), nesting))
            {
                session->commands->DiscardNativePresent();
                ++s.observation.nativePresent.rejected; session->accepting = false;
                s.Message("Unsupported: native text internal hook suppression unavailable"); return;
            }
            struct LeaveNative { Session& owner; ID3D11DeviceContext* context; std::uint64_t token;
                ~LeaveNative() { owner.LeaveInternal(context, token); } } leaveNative{*session, context.Get(), nesting};
            if (!session->InternalActive(context.Get()))
            {
                session->commands->DiscardNativePresent();
                ++s.observation.nativePresent.rejected; session->accepting = false;
                s.Message("Unsupported: native text internal suppression not active"); return;
            }
            const char* failure = nullptr;
            std::shared_ptr<NativeComputePacket> packet;
            if (session->compute)
                packet = session->compute->Prepare(context.Get(), chain, window, prepared.batch, session->parameterState, failure, session.get(), ticket, cursor.nativeEpoch ? &cursor : nullptr);
            if (!packet)
            {
                session->commands->DiscardNativePresent();
                ++s.observation.nativePresent.rejected; session->accepting = false;
                s.Message(failure ? failure : "Unsupported: native text preflight unavailable"); return;
            }
            if (!session->ReserveCompute(packet, ticket))
            {
                packet->Retire();
                session->commands->DiscardNativePresent();
                ++s.observation.nativePresent.rejected; session->accepting = false;
                s.Message("Unsupported: native text GPU owner/alias reservation unavailable"); return;
            }
            const auto submitted = session->compute->Submit(context.Get(), *packet, ticket);
            if (!submitted.success)
            {
                session->FinishCompute(ticket, false, submitted.consumed);
                session->commands->DiscardNativePresent();
                ++s.observation.nativePresent.rejected;
                s.Message(submitted.reason ? submitted.reason : "Quarantine: native text submission failed"); return;
            }
            if (!session->commands->AcceptNativePresent())
            {
                session->FinishCompute(ticket, false, submitted.consumed);
                session->commands->DiscardNativePresent();
                ++s.observation.nativePresent.rejected;
                s.Message("Quarantine: native text PRE semantic commit failed after submission"); return;
            }
            if (!session->FinishCompute(ticket, true, submitted.consumed))
            {
                session->commands->CompleteVariablePresent(false);
                ++s.observation.nativePresent.rejected; return;
            }
        }
        else
        {
            const auto prepared = session->commands->BeginVariablePresent(session->presentPrograms.data(),
                static_cast<UINT>(session->presentPrograms.size()), values, !(requirements[1] & NativePhaseValues));
            if (!prepared)
            {
                ++s.observation.nativePresent.rejected; session->accepting = false;
                s.Message("Unsupported: native PRE/invariant POST variable preflight failed; no phase committed"); return;
            }
        }
        session_ = session; swapchain_ = std::move(retainedChain); device_ = std::move(device); context_ = std::move(context);
        ticket_ = ticket; deviceGeneration_ = session->capture->generation; planGeneration_ = session->generation;
        revision_ = revision; thread_ = GetCurrentThreadId();
        window_ = window; windowThread_ = windowThread; preValues_ = values; valueRequirements_ = requirements;
        sync_ = sync; flags_ = flags; present1_ = present1;
        s.nativePresentTicket = ticket;
        auto& o = s.observation.nativePresent;
        ++o.pre; o.invocation = ticket; o.deviceGeneration = deviceGeneration_; o.planGeneration = planGeneration_;
        o.swapchain = Pointer(chain); o.device = Pointer(device_.Get()); o.context = Pointer(context_.Get());
        o.thread = thread_; o.sync = sync; o.flags = flags; o.present1 = present1; o.returned = false;
        o.result = S_OK; // result is explicitly unknown until Returned, never a success receipt
        cursorAdmission.retained = cursor.nativeEpoch != 0;
    }
    catch (...) { Store().coverageLost.store(true, std::memory_order_release); }
}
void NativePresentScope::Returned(IDXGISwapChain* chain, UINT sync, UINT flags, bool present1, HRESULT result) noexcept
{ Finish(true, chain, sync, flags, present1, result); }
NativePresentScope::~NativePresentScope() noexcept
{ Finish(false, nullptr, 0, 0, false, S_OK); }
void NativePresentScope::Finish(bool returned, IDXGISwapChain* chain, UINT sync, UINT flags,
                                bool present1, HRESULT result) noexcept
{
    if (!session_) return;
    bool keepCursor = false;
    try
    {
        auto session = std::static_pointer_cast<Session>(session_);
        NativePresentValues postValues;
        // TIME belongs to RunFrameActions PRE, even if Present blocks or fails.
        if (preValues_.Has(NativeTime))
        {
            postValues.values[static_cast<unsigned>(NativeValue::Time)] =
                preValues_.values[static_cast<unsigned>(NativeValue::Time)];
            postValues.known |= NativeTime;
        }
        const bool realReturn = returned && chain == swapchain_.Get() && sync == sync_ && flags == flags_ &&
            present1 == present1_ && GetCurrentThreadId() == thread_;
        if (realReturn)
        {
            if (valueRequirements_[1] & NativeWindowValues)
            {
                HWND window = nullptr; DWORD owner = 0;
                if (NativeWindowIdentity(chain, window, owner) && window == window_ && owner == windowThread_)
                    SampleNativeWindow(window, owner, valueRequirements_[1], postValues);
            }
            // XXMI calls RunCommandList separately after the original return:
            // its fresh CommandListState samples OM/client again, not PRE's RT.
            // Flip Present may unbind slot 0. TIME alone remains the PRE value.
            SampleNativeRenderTarget(context_.Get(), device_.Get(), valueRequirements_[1], postValues);
            SampleNativeResolution(chain, device_.Get(), valueRequirements_[1], postValues);
        }
        auto& s = Store(); std::lock_guard lock(s.ownerMutex);
        const bool paired = returned && ticket_ && s.nativePresentTicket == ticket_ &&
            chain == swapchain_.Get() && sync == sync_ && flags == flags_ && present1 == present1_ &&
            GetCurrentThreadId() == thread_ && session->thread == thread_ &&
            session->capture->device.Get() == device_.Get() && session->capture->context.Get() == context_.Get() &&
            session->capture->generation == deviceGeneration_ && session->generation == planGeneration_;
        const bool publishPost = paired && realReturn && Enabled() && session->accepting && !session->quarantined &&
            s.active == session && s.capture == session->capture && !session->capture->opaque &&
            !session->capture->exhausted && session->capture->coverage && session->capture->creations &&
            !s.coverageLost.load(std::memory_order_acquire) &&
            revision_ == contractRevision.load(std::memory_order_acquire) && s.loaded == revision_;
        // XXMI executes POST after every real original return, including a
        // non-success HRESULT. A clear is a new GPU submission with its own
        // retained packet and later real Signal; this return is not completion.
        const bool completed = publishPost && session->plan->Touches().clearTargets && session->presentCompute ?
            session->SubmitNativePost(postValues, chain, window_) :
            session->commands->CompleteVariablePresent(publishPost, postValues);
        auto& o = s.observation.nativePresent;
        o.returned = returned; if (returned) o.result = result;
        keepCursor = publishPost && completed;
        if (publishPost && completed) ++o.post;
        else
        {
            ++o.cancelled; session->accepting = false;
            s.Message("Unsupported: native Present pair/facts/configuration/coverage or POST evaluation failed; PRE retained, POST cancelled");
        }
        if (s.nativePresentTicket == ticket_) s.nativePresentTicket = 0;
    }
    catch (...) { Store().coverageLost.store(true, std::memory_order_release); }
    if (!keepCursor) OptiInput::ReleaseGameCursor(); // outside ownerMutex, including a cancelled POST
    session_.reset(); swapchain_.Reset(); context_.Reset(); device_.Reset(); ticket_ = 0;
    window_ = nullptr; windowThread_ = 0; preValues_ = {}; valueRequirements_ = {};
}

Observation Observe() noexcept
{
    auto& s = Store(); std::unique_lock lock(s.ownerMutex, std::try_to_lock);
    if (!lock.owns_lock()) { Observation o; std::snprintf(o.message, sizeof(o.message), "MOD owner busy"); return o; }
    return s.observation;
}
} // namespace gimi::runtime
