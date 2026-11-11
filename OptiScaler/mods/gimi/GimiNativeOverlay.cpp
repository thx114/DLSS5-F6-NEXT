#include "GimiNativeOverlay.h"

#include <array>
#include <cmath>
#include <iterator>
#include <stdexcept>

namespace gimi
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr UINT UavCapacity = D3D11_1_UAV_SLOT_COUNT;
constexpr UINT PassCapacity = 2; // original B/F sequence, never an unbounded draw list
constexpr UINT PixelSlots[] = {20, 21, 22, 23, IniParameterRegister, 100, 101};
constexpr UINT MouseSlots[] = {100, 101, IniParameterRegister};
template<class T> bool OnDevice(T* object, ID3D11Device* device) noexcept
{
    if (!object) return false;
    ComPtr<ID3D11Device> actual; object->GetDevice(&actual);
    return actual.Get() == device;
}
bool Same(Binding a, Binding b) noexcept
{
    return a.object == b.object && a.view == b.view && a.generation == b.generation &&
        a.stride == b.stride && a.offset == b.offset && a.format == b.format && a.byteSize == b.byteSize;
}
bool StandardSampler(ID3D11SamplerState* sampler) noexcept
{
    if (!sampler) return true; // genuine inherited API null, never a fabricated default
    D3D11_SAMPLER_DESC desc{}; sampler->GetDesc(&desc);
    switch (desc.Filter)
    {
    case D3D11_FILTER_MIN_MAG_MIP_POINT: case D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR:
    case D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT: case D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR:
    case D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT: case D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
    case D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT: case D3D11_FILTER_MIN_MAG_MIP_LINEAR:
    case D3D11_FILTER_ANISOTROPIC: return true;
    default: return false;
    }
}
bool WindowOwner(HWND window) noexcept
{
    DWORD process = 0;
    return window && IsWindow(window) && GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId();
}
} // namespace

struct NativeOverlayPacket::Storage
{

    struct MouseScope
    {
        // Native PRE shadow at this CALL, including a nested parent's cursor.
        std::array<bool, 9> outputNull{};
        std::array<Binding, 4> pixelBindings{}, mouseBindings{};
        std::array<bool, 4> known{}, mouseKnown{};
        bool backbufferBound = false;
        UINT role = 0, draws = 0;
        Id shader = InvalidId;
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11HullShader> hs;
        ComPtr<ID3D11DomainShader> ds;
        ComPtr<ID3D11GeometryShader> gs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11InputLayout> layout;
        ComPtr<ID3D11BlendState> blend;
        ComPtr<ID3D11RasterizerState> raster;
        std::array<ComPtr<ID3D11RenderTargetView>, 8> targets{};
        ComPtr<ID3D11DepthStencilView> depth;
        std::array<ComPtr<ID3D11ShaderResourceView>, 3> vertex{}, pixel{};
        std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
        UINT viewportCount = 0, sampleMask = 0;
        FLOAT factors[4]{};
        D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        bool Capture(ID3D11DeviceContext* c) noexcept
        {
            UINT classes = 0;
            c->VSGetShader(&vs, nullptr, &classes); if (classes) return false;
            c->HSGetShader(&hs, nullptr, &classes); if (classes) return false;
            c->DSGetShader(&ds, nullptr, &classes); if (classes) return false;
            c->GSGetShader(&gs, nullptr, &classes); if (classes) return false;
            c->PSGetShader(&ps, nullptr, &classes); if (classes) return false;
            c->IAGetInputLayout(&layout); c->IAGetPrimitiveTopology(&topology);
            c->OMGetBlendState(&blend, factors, &sampleMask); c->RSGetState(&raster);
            ID3D11RenderTargetView* views[8]{}; c->OMGetRenderTargets(8, views, &depth);
            for (unsigned n = 0; n < 8; ++n) targets[n].Attach(views[n]);
            viewportCount = static_cast<UINT>(viewports.size()); c->RSGetViewports(&viewportCount, viewports.data());
            if (viewportCount > viewports.size()) return false;
            for (unsigned n = 0; n < 3; ++n)
            { c->VSGetShaderResources(MouseSlots[n], 1, &vertex[n]); c->PSGetShaderResources(MouseSlots[n], 1, &pixel[n]); }
            return true;
        }
        void Restore(ID3D11DeviceContext* c) const noexcept
        {
            ID3D11ShaderResourceView* none = nullptr;
            for (auto slot : MouseSlots) { c->VSSetShaderResources(slot, 1, &none); c->PSSetShaderResources(slot, 1, &none); }
            c->OMSetRenderTargets(0, nullptr, nullptr);
            ID3D11RenderTargetView* rt[8]{}; for (unsigned n = 0; n < 8; ++n) rt[n] = targets[n].Get();
            c->OMSetRenderTargets(8, rt, depth.Get());
            for (unsigned n = 0; n < 3; ++n)
            { auto* v = vertex[n].Get(); auto* p = pixel[n].Get(); c->VSSetShaderResources(MouseSlots[n], 1, &v); c->PSSetShaderResources(MouseSlots[n], 1, &p); }
            c->VSSetShader(vs.Get(), nullptr, 0); c->HSSetShader(hs.Get(), nullptr, 0);
            c->DSSetShader(ds.Get(), nullptr, 0); c->GSSetShader(gs.Get(), nullptr, 0); c->PSSetShader(ps.Get(), nullptr, 0);
            c->IASetInputLayout(layout.Get()); c->IASetPrimitiveTopology(topology);
            c->OMSetBlendState(blend.Get(), factors, sampleMask); c->RSSetState(raster.Get());
            c->RSSetViewports(viewportCount, viewportCount ? viewports.data() : nullptr);
        }
    };
    struct Step
    {
        ActionOp op = ActionOp::Bind;
        UINT slot = 0, parameters = 0;
        Id shader = InvalidId;
        bool vertex = false;
        UINT scope = 0;
        ComPtr<ID3D11Texture1D> mouseParameters;
        ComPtr<ID3D11ShaderResourceView> mouseParameterView;
        Dx11RetainedBinding kept;
        std::array<Dx11RetainedBinding, 4> inputs{};
    };
    ComPtr<ID3D11Device> device;
    std::shared_ptr<const Dx11StaticResources> statics;
    std::shared_ptr<const DebugOverlayLibrary> library;
    std::vector<Step> steps;
    std::vector<MouseScope> mouseScopes;
    UINT mouseScopeCount = 0, mouseDepth = 0, dynamicPages = 0;
    std::array<UINT, 2> mouseStack{};
    std::array<Binding, 4> mouseBindings{};
    std::array<bool, 4> mouseKnown{};
    std::array<ComPtr<ID3D11ShaderResourceView>, 3> mouseVertex{};
    OptiInput::GameCursorSnapshot cursor;
    UINT stepCount = 0, nextStep = 0, passes = 0;
    // Complete tape-position shadow. Text CS/draw scopes are net-zero for
    // these bindings; they cannot replace an explicit null output or bb action.
    std::array<bool, 9> outputNull{};
    std::array<Binding, 4> pixelBindings{};
    std::array<bool, 4> known{};
    bool backbufferBound = false;
    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> liveTargets{};
    ID3D11DepthStencilView* liveDepth = nullptr;
    std::uint64_t epoch = 0;
    DWORD thread = 0;
    HWND window = nullptr;
    bool prepared = false, submitted = false;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> chain;
    ComPtr<ID3D11Texture2D> backbuffer;
    ComPtr<ID3D11RenderTargetView> target;
    D3D11_VIEWPORT viewport{};
    // Getters return strong COM roots; each view also retains its actual resource.
    std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
    ComPtr<ID3D11DepthStencilView> depthView;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11SamplerState> sampler;
    std::array<ComPtr<ID3D11ShaderResourceView>, std::size(PixelSlots)> pixels{};
    std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
    UINT viewportCount = 0, sampleMask = 0, stencilRef = 0;
    std::array<FLOAT, 4> factors{};
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;

    void Release() noexcept
    {
        for (UINT n = 0; n < stepCount; ++n) steps[n] = {};
        for (UINT n = 0; n < mouseScopeCount; ++n) mouseScopes[n] = {};
        for (auto& view : mouseVertex) view.Reset();
        statics->ReleaseNativeParameterPages(dynamicPages);
        dynamicPages = mouseScopeCount = mouseDepth = 0; mouseBindings = {}; mouseKnown = {}; cursor = {};
        stepCount = nextStep = passes = 0; prepared = submitted = false; epoch = 0;
        outputNull = {}; pixelBindings = {}; known = {}; backbufferBound = false;
        liveTargets = {}; liveDepth = nullptr;
        context.Reset(); chain.Reset(); backbuffer.Reset(); target.Reset();
        for (auto& view : targets) view.Reset();
        for (auto& view : pixels) view.Reset();
        depthView.Reset(); depth.Reset(); blend.Reset(); raster.Reset(); layout.Reset();
        vs.Reset(); hs.Reset(); ds.Reset(); gs.Reset(); ps.Reset(); cs.Reset(); sampler.Reset();
        thread = 0; window = nullptr; viewportCount = 0;
    }
    bool Retain(Binding binding, Dx11ResourceProvider* provider, Dx11RetainedBinding& out) const noexcept
    {
        out = {};
        if (!binding.object && !Same(binding, {})) return false;
        if (statics->Resolve(binding, out.value))
        {
            if (out.value.resource) out.statics = statics;
            return true;
        }
        return provider && (provider->ResolvePrivateTexture(binding, out) || provider->RetainNativeReference(binding, out)) &&
            Same(out.value.binding, binding);
    }
    bool Readable(const Dx11RetainedBinding& kept, unsigned slot) const noexcept
    {
        if (!kept.value.binding.object) return Same(kept.value.binding, {});
        const auto& owned = kept.ownedTexture;
        if (!owned || kept.game.get() != owned.get() || !owned->source.owner || !owned->ordered ||
            !owned->identity.gpuOrdered || !kept.stableReadOnly || !kept.value.sampleableFloat2D ||
            !Same(kept.value.binding, owned->binding) || kept.value.binding.generation != statics->DeviceGeneration() ||
            kept.value.resource != owned->texture.Get() || kept.value.srv != owned->srv.Get() ||
            !ValidatePrivateTexture(*owned, device.Get()) || kept.value.resource == backbuffer.Get() ||
            (slot == 2 ? owned->purpose != OwnedTexturePurpose::DepthCopy : owned->purpose != OwnedTexturePurpose::OutputDescription) ||
            (slot < 2 ? owned->source.view.depth : !owned->source.view.depth)) return false;
        return true;
    }
    bool ReadResource(ID3D11Resource* resource) const noexcept
    {
        for (UINT i = 0; i < stepCount; ++i)
        {
            const auto& step = steps[i];
            if (step.op == ActionOp::Bind && step.kept.value.resource == resource) return true;
            if (step.op == ActionOp::DebugOverlay || step.op == ActionOp::MouseDraw)
                for (const auto& input : step.inputs) if (input.value.resource == resource) return true;
        }
        return false;
    }
    bool Hazards(ID3D11DeviceContext* c) const noexcept
    {
        // OM is explicitly unbound by the tape before any SRV binding. Other
        // writable bindings have no counter/restoration ABI here: reject all.
        const UINT count = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? UavCapacity : D3D11_PS_CS_UAV_REGISTER_COUNT;
        std::array<ID3D11UnorderedAccessView*, UavCapacity> uavs{};
        c->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, count, uavs.data());
        bool safe = true;
        for (UINT i = 0; i < count; ++i) if (uavs[i]) { safe = false; uavs[i]->Release(); uavs[i] = nullptr; }
        c->CSGetUnorderedAccessViews(0, count, uavs.data());
        for (UINT i = 0; i < count; ++i) if (uavs[i]) { safe = false; uavs[i]->Release(); }
        if (!safe) return false;
        // Binding the backbuffer as RTV must not silently null any stage's SRV.
        // Also forbid an original OM writer of a private input: restoring OM
        // could otherwise silently erase an untouched original SRV slot.
        std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> views{};
        for (unsigned stage = 0; stage < 6; ++stage)
        {
            switch (stage)
            {
            case 0: c->VSGetShaderResources(0, static_cast<UINT>(views.size()), views.data()); break;
            case 1: c->HSGetShaderResources(0, static_cast<UINT>(views.size()), views.data()); break;
            case 2: c->DSGetShaderResources(0, static_cast<UINT>(views.size()), views.data()); break;
            case 3: c->GSGetShaderResources(0, static_cast<UINT>(views.size()), views.data()); break;
            case 4: c->PSGetShaderResources(0, static_cast<UINT>(views.size()), views.data()); break;
            case 5: c->CSGetShaderResources(0, static_cast<UINT>(views.size()), views.data()); break;
            }
            for (auto*& view : views) if (view)
            {
                ComPtr<ID3D11Resource> resource; view->GetResource(&resource);
                if (!resource || resource.Get() == backbuffer.Get()) safe = false;
                view->Release(); view = nullptr;
            }
        }
        for (const auto& rt : targets) if (rt)
        {
            ComPtr<ID3D11Resource> resource; rt->GetResource(&resource);
            if (!resource || ReadResource(resource.Get())) safe = false;
        }
        if (depthView)
        {
            ComPtr<ID3D11Resource> resource; depthView->GetResource(&resource);
            if (!resource || ReadResource(resource.Get())) safe = false;
        }
        return safe;
    }
    bool Capture(ID3D11DeviceContext* c, IDXGISwapChain* swapchain, HWND expectedWindow) noexcept
    {
        if (!c || !swapchain || !OnDevice(c, device.Get()) || c->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
            !WindowOwner(expectedWindow)) return false;
        DXGI_SWAP_CHAIN_DESC swap{};
        ComPtr<ID3D11Device> actual;
        if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&actual))) || actual.Get() != device.Get() ||
            FAILED(swapchain->GetDesc(&swap)) || swap.OutputWindow != expectedWindow || !swap.BufferCount ||
            !(swap.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT)) return false;
        if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer))) || !OnDevice(backbuffer.Get(), device.Get())) return false;
        D3D11_TEXTURE2D_DESC desc{}; backbuffer->GetDesc(&desc);
        if (!desc.Width || !desc.Height || desc.Width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            desc.Height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || desc.ArraySize != 1 || desc.MipLevels != 1 ||
            desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality || desc.Usage != D3D11_USAGE_DEFAULT ||
            !(desc.BindFlags & D3D11_BIND_RENDER_TARGET) || desc.CPUAccessFlags || desc.MiscFlags ||
            desc.Format == DXGI_FORMAT_UNKNOWN || desc.Format != swap.BufferDesc.Format ||
            (swap.BufferDesc.Width && swap.BufferDesc.Width != desc.Width) ||
            (swap.BufferDesc.Height && swap.BufferDesc.Height != desc.Height)) return false;
        if (FAILED(device->CreateRenderTargetView(backbuffer.Get(), nullptr, &target)) || !OnDevice(target.Get(), device.Get())) return false;
        ComPtr<ID3D11Resource> targetResource; target->GetResource(&targetResource);
        D3D11_RENDER_TARGET_VIEW_DESC targetDesc{}; target->GetDesc(&targetDesc);
        if (targetResource.Get() != backbuffer.Get() || targetDesc.Format != desc.Format ||
            targetDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || targetDesc.Texture2D.MipSlice) return false;
        ComPtr<ID3D11Predicate> predicate; BOOL value = FALSE;
        c->GetPredication(&predicate, &value); if (predicate) return false;
        ID3D11Buffer* so[D3D11_SO_BUFFER_SLOT_COUNT]{};
        c->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, so);
        bool noSo = true;
        for (auto* buffer : so) if (buffer) { noSo = false; buffer->Release(); }
        if (!noSo) return false;
        UINT classes = 0;
        c->VSGetShader(&vs, nullptr, &classes); if (classes) return false;
        c->HSGetShader(&hs, nullptr, &classes); if (classes) return false;
        c->DSGetShader(&ds, nullptr, &classes); if (classes) return false;
        c->GSGetShader(&gs, nullptr, &classes); if (classes) return false;
        c->PSGetShader(&ps, nullptr, &classes); if (classes) return false;
        c->CSGetShader(&cs, nullptr, &classes); if (classes) return false;
        ID3D11RenderTargetView* rt[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rt, &depthView);
        for (UINT i = 0; i < targets.size(); ++i) targets[i].Attach(rt[i]);
        c->OMGetDepthStencilState(&depth, &stencilRef);
        c->OMGetBlendState(&blend, factors.data(), &sampleMask);
        c->RSGetState(&raster); c->IAGetInputLayout(&layout); c->IAGetPrimitiveTopology(&topology);
        viewportCount = static_cast<UINT>(viewports.size()); c->RSGetViewports(&viewportCount, viewports.data());
        if (viewportCount > viewports.size()) return false;
        for (unsigned n = 0; n < std::size(PixelSlots); ++n) c->PSGetShaderResources(PixelSlots[n], 1, &pixels[n]);
        for (unsigned n = 0; n < 3; ++n) c->VSGetShaderResources(MouseSlots[n], 1, &mouseVertex[n]);
        c->PSGetSamplers(0, 1, &sampler);
        if (!library->stages.empty() && ((sampler && !OnDevice(sampler.Get(), device.Get())) || !StandardSampler(sampler.Get()))) return false;
        viewport = {0, 0, static_cast<FLOAT>(desc.Width), static_cast<FLOAT>(desc.Height), 0, 1};
        context = c; chain = swapchain; window = expectedWindow; thread = GetCurrentThreadId();
        return true;
    }
    void Restore(ID3D11DeviceContext* c) const noexcept
    {
        ID3D11ShaderResourceView* none = nullptr;
        for (const auto slot : PixelSlots) c->PSSetShaderResources(slot, 1, &none);
        for (const auto slot : MouseSlots) c->VSSetShaderResources(slot, 1, &none);
        c->OMSetRenderTargets(0, nullptr, nullptr);
        ID3D11RenderTargetView* rt[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        for (UINT i = 0; i < targets.size(); ++i) rt[i] = targets[i].Get();
        c->OMSetRenderTargets(static_cast<UINT>(targets.size()), rt, depthView.Get());
        for (unsigned i = 0; i < std::size(PixelSlots); ++i)
        { auto* view = pixels[i].Get(); c->PSSetShaderResources(PixelSlots[i], 1, &view); }
        for (unsigned n = 0; n < 3; ++n)
        { auto* view = mouseVertex[n].Get(); c->VSSetShaderResources(MouseSlots[n], 1, &view); }
        c->VSSetShader(vs.Get(), nullptr, 0); c->HSSetShader(hs.Get(), nullptr, 0);
        c->DSSetShader(ds.Get(), nullptr, 0); c->GSSetShader(gs.Get(), nullptr, 0);
        c->PSSetShader(ps.Get(), nullptr, 0); c->CSSetShader(cs.Get(), nullptr, 0);
        c->IASetInputLayout(layout.Get()); c->IASetPrimitiveTopology(topology);
        c->RSSetState(raster.Get()); c->RSSetViewports(viewportCount, viewportCount ? viewports.data() : nullptr);
        c->OMSetBlendState(blend.Get(), factors.data(), sampleMask);
        c->OMSetDepthStencilState(depth.Get(), stencilRef);
        // s0, scissor, CB and VB/IB are inherited. Text restores its own scope
        // before this final entry-state restore, including its separate GS/PS slots.
    }
};

NativeOverlayPacket::NativeOverlayPacket(UINT actions, ID3D11Device* device,
    std::shared_ptr<const Dx11StaticResources> statics) : state_(new Storage)
{
    auto& s = *state_; s.device = device; s.statics = std::move(statics);
    if (!actions || !s.device || !s.statics || !(s.library = s.statics->DebugOverlay()) ||
        s.library->plan != s.statics->Files()->PlanOwner() ||
        (!s.library->stages.empty() && (!OnDevice(s.library->raster.Get(), device) || !OnDevice(s.library->blend.Get(), device))) ||
        (!s.library->mouseStages.empty() && (!OnDevice(s.library->mouseRaster.Get(), device) ||
          !OnDevice(s.library->mouseBlend[0].Get(), device) || !OnDevice(s.library->mouseBlend[1].Get(), device))))
        throw std::runtime_error("Native overlay requires its own authenticated device library");
    s.steps.resize(actions); s.mouseScopes.resize(actions);
}
NativeOverlayPacket::~NativeOverlayPacket() { state_->Release(); }
std::uint64_t NativeOverlayPacket::Epoch() const noexcept { return state_->epoch; }
ID3D11Texture2D* NativeOverlayPacket::Backbuffer() const noexcept { return state_->backbuffer.Get(); }
void NativeOverlayPacket::Retire() noexcept { state_->Release(); }
void NativeOverlayPacket::Restore(ID3D11DeviceContext* c) const noexcept { state_->Restore(c); }

bool NativeOverlayPacket::BeginPrepare(ID3D11DeviceContext* c, IDXGISwapChain* chain, HWND window,
    std::uint64_t nativeEpoch, const BatchView& batch, const char*& reason) noexcept
{
    auto& s = *state_; s.Release(); reason = "Native overlay complete PRE tape/owner unavailable";
    if (!nativeEpoch || nativeEpoch == UINT64_MAX || !batch.plan || batch.plan != s.library->plan.get() ||
        !batch.plan->Touches().backbufferViewport || batch.phase != Phase::Pre || !batch.actions ||
        batch.count > s.steps.size() || batch.includesDrawBoundary || !s.Capture(c, chain, window)) return false;
    s.epoch = nativeEpoch; return true;
}


bool NativeOverlayPacket::PrepareMouse(const Action& a, const float* image,
    const OptiInput::GameCursorSnapshot& snapshot, Dx11ResourceProvider* provider, UINT& index, const char*& reason) noexcept
{
    auto& s = *state_; reason = "SoftwareMouse complete scope/input/resource/parameter preflight failed";
    if (!s.epoch || s.prepared || s.submitted || s.stepCount == s.steps.size() ||
        snapshot.nativeEpoch != s.epoch || snapshot.window != s.window || !snapshot.shape ||
        !OptiInput::ValidateGameCursor(snapshot)) return false;
    if (s.cursor.nativeEpoch && (s.cursor.windowEpoch != snapshot.windowEpoch || s.cursor.cursorRevision != snapshot.cursorRevision)) return false;
    s.cursor = snapshot;
    index = s.stepCount++; auto& step = s.steps[index]; step.op = a.op;
    if (a.op == ActionOp::BeginMouse)
    {
        if (s.mouseScopeCount == s.mouseScopes.size() || s.mouseDepth >= s.mouseStack.size() ||
            a.raster != (s.mouseDepth ? 2u : 1u)) return false;
        const Id shader = s.mouseDepth ? s.mouseScopes[s.mouseStack[0]].shader : a.debugOverlay;
        if (shader >= s.library->mouseStages.size()) return false;
        step.scope = s.mouseScopeCount++; auto& scope = s.mouseScopes[step.scope];
        scope.outputNull = s.outputNull; scope.backbufferBound = s.backbufferBound;
        scope.pixelBindings = s.pixelBindings; scope.known = s.known;
        scope.mouseBindings = s.mouseBindings; scope.mouseKnown = s.mouseKnown;
        scope.role = a.raster; scope.shader = shader;
        s.mouseStack[s.mouseDepth++] = step.scope;
    }
    else if (a.op == ActionOp::EndMouse)
    {
        if (!s.mouseDepth) return false;
        step.scope = s.mouseStack[--s.mouseDepth]; const auto& scope = s.mouseScopes[step.scope];
        if (scope.role != a.raster || scope.draws != 1) return false;
        s.outputNull = scope.outputNull; s.backbufferBound = scope.backbufferBound;
        s.pixelBindings = scope.pixelBindings; s.known = scope.known;
        s.mouseBindings = scope.mouseBindings; s.mouseKnown = scope.mouseKnown;
    }
    else if (a.op == ActionOp::Bind)
    {
        if (!s.mouseDepth || !s.backbufferBound ||
            (a.destination.kind != TargetKind::VertexTexture && a.destination.kind != TargetKind::PixelTexture) ||
            (a.destination.index != 100 && a.destination.index != 101) || !s.Retain(a.binding, provider, step.kept)) return false;
        step.vertex = a.destination.kind == TargetKind::VertexTexture; step.slot = a.destination.index;
        const auto n = (step.vertex ? 0u : 2u) + step.slot - 100;
        // An original view is legal ONLY as a restore to that exact entry slot.
        // New readers below require a real immutable/ordered private resource.
        const auto* original = step.vertex ? s.mouseVertex[step.slot - 100].Get() : s.pixels[5 + step.slot - 100].Get();
        if (step.kept.value.srv != original && step.kept.value.binding.object &&
            (!step.kept.statics || !step.kept.value.sampleableFloat2D)) return false;
        s.mouseBindings[n] = a.binding; s.mouseKnown[n] = true;
    }
    else if (a.op == ActionOp::MouseDraw)
    {
        if (!image || !s.mouseDepth || !s.backbufferBound) return false;
        auto& scope = s.mouseScopes[s.mouseStack[s.mouseDepth - 1]];
        if (a.raster != scope.role || scope.draws || image[7] != static_cast<float>(scope.role) ||
            image[4] <= 0 || image[5] <= 0 || image[6] != (snapshot.showing ? 1.0f : 0.0f) ||
            image[0] != static_cast<float>(snapshot.client.x) || image[1] != static_cast<float>(snapshot.client.y) ||
            image[2] != static_cast<float>(snapshot.shape->hotspot.x) || image[3] != static_cast<float>(snapshot.shape->hotspot.y)) return false;
        step.scope = s.mouseStack[s.mouseDepth - 1];
        for (unsigned n = 0; n < 4; ++n)
        {
            if (!s.mouseKnown[n] || !Same(s.mouseBindings[n], a.overlayBindings[n]) ||
                !s.Retain(a.overlayBindings[n], provider, step.inputs[n])) return false;
            const auto& kept = step.inputs[n];
            if (!kept.value.binding.object) { if (!Same(a.overlayBindings[n], {})) return false; continue; }
            // Mask/color source is the ORIGINAL explicit Resource alias. No OS
            // screenshot/cursor-art replacement or game-view promotion.
            if (!kept.statics || !kept.value.sampleableFloat2D || !kept.value.srv || !kept.value.resource ||
                kept.value.binding.generation != s.statics->DeviceGeneration()) return false;
            ComPtr<ID3D11Texture2D> texture;
            if (FAILED(kept.value.resource->QueryInterface(IID_PPV_ARGS(&texture)))) return false;
            D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
            D3D11_SHADER_RESOURCE_VIEW_DESC view{}; kept.value.srv->GetDesc(&view);
            if (desc.Usage != D3D11_USAGE_IMMUTABLE || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
                !desc.Width || !desc.Height || view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
                view.Texture2D.MostDetailedMip || !OnDevice(texture.Get(), s.device.Get())) return false;
        }
        if (!Same(a.overlayBindings[0], a.overlayBindings[2]) || !Same(a.overlayBindings[1], a.overlayBindings[3])) return false;
        // Genuine known-null aliases keep the original D3D null Load /
        // GetDimensions semantics. Colour uses its own dimensions, BW uses
        // mask.height/2 exactly in the authenticated HLSL. Never substitute
        // an OS texture, force dimensions or promote an unknown game view.
        for (Id n = 0; n < IniParameterScalars; ++n) if (!std::isfinite(image[n])) return false;
        if (!s.statics->ClaimNativeParameterPage()) return false;
        ++s.dynamicPages; // reclaimed ONLY by pre-submit abort or real packet retirement
        D3D11_TEXTURE1D_DESC desc{}; desc.Width = IniParameterVectors; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA initial{image, static_cast<UINT>(IniParameterScalars * sizeof(float)), 0};
        if (FAILED(s.device->CreateTexture1D(&desc, &initial, &step.mouseParameters)) ||
            FAILED(s.device->CreateShaderResourceView(step.mouseParameters.Get(), nullptr, &step.mouseParameterView))) return false;
        ++scope.draws;
    }
    else return false;
    reason = nullptr; return true;
}

bool NativeOverlayPacket::PrepareAction(const Action& a, UINT immutableParameters,
    Dx11ResourceProvider* provider, UINT& index, const char*& reason) noexcept
{
    auto& s = *state_; reason = "Native overlay ordered OM/PS/viewport action unavailable";
    if (!s.epoch || s.prepared || s.submitted || s.stepCount == s.steps.size()) return false;
    index = s.stepCount++; auto& step = s.steps[index]; step.op = a.op;
    if (a.op == ActionOp::BindOutput)
    {
        const bool depth = a.destination.kind == TargetKind::DepthTarget;
        if ((!depth && a.destination.kind != TargetKind::RenderTarget) || (depth ? a.destination.index != 0 : a.destination.index >= 8) ||
            !Same(a.binding, {})) return false;
        step.slot = depth ? 8 : a.destination.index; s.outputNull[step.slot] = true;
        if (!step.slot) s.backbufferBound = false;
    }
    else if (a.op == ActionOp::BackbufferViewport)
    {
        if (!(a.destination == Target{TargetKind::RenderTarget, 0})) return false;
        for (const bool empty : s.outputNull) if (!empty) return false;
        s.outputNull[0] = false; s.backbufferBound = true;
    }
    else if (a.op == ActionOp::Bind)
    {
        if (a.destination.kind != TargetKind::PixelTexture || a.destination.index < 20 || a.destination.index > 23 ||
            !s.backbufferBound || !s.Retain(a.binding, provider, step.kept) || !s.Readable(step.kept, a.destination.index - 20)) return false;
        step.slot = a.destination.index; s.pixelBindings[step.slot - 20] = a.binding; s.known[step.slot - 20] = true;
    }
    else if (a.op == ActionOp::DebugOverlay)
    {
        if (!s.backbufferBound || s.passes == PassCapacity || a.debugOverlay >= s.library->stages.size()) return false;
        const auto& shaders = s.library->stages[a.debugOverlay];
        if (!OnDevice(shaders.vs.Get(), s.device.Get()) || !OnDevice(shaders.ps.Get(), s.device.Get())) return false;
        float pass = 0, z = 0, w = 0;
        if (!s.statics->ReadParameter(immutableParameters, 70 * 4, pass) || (pass != 1 && pass != 2) ||
            !s.statics->ReadParameter(immutableParameters, 2, z) || z != 0 ||
            !s.statics->ReadParameter(immutableParameters, 3, w) || w != 1 || !s.statics->ParameterView(immutableParameters)) return false;
        for (unsigned n = 0; n < s.known.size(); ++n)
            if (!s.known[n] || !Same(s.pixelBindings[n], a.overlayBindings[n]) ||
                !s.Retain(a.overlayBindings[n], provider, step.inputs[n]) || !s.Readable(step.inputs[n], n)) return false;
        const auto& color = step.inputs[0]; const auto& rt6 = step.inputs[1];
        if (color.ownedTexture && rt6.ownedTexture && (color.value.resource == rt6.value.resource ||
            color.ownedTexture->alias == rt6.ownedTexture->alias)) return false;
        step.parameters = immutableParameters; step.shader = a.debugOverlay; ++s.passes;
    }
    else return false; // the enclosing executor owns text, aliases and parameters
    reason = nullptr; return true;
}

bool NativeOverlayPacket::EndPrepare(ID3D11DeviceContext* c, const char*& reason) noexcept
{
    auto& s = *state_;
    if (!s.epoch || s.prepared || s.mouseDepth || c != s.context.Get() || !s.Hazards(c) || FAILED(s.device->GetDeviceRemovedReason()))
    { reason = "Native combined tape overlay resource/state hazards unavailable"; return false; }
    s.prepared = true; reason = nullptr; return true;
}

bool NativeOverlayPacket::BeginSubmit(ID3D11DeviceContext* c, std::uint64_t nativeEpoch) noexcept
{
    auto& s = *state_;
    if (!s.prepared || s.submitted || !nativeEpoch || nativeEpoch != s.epoch || c != s.context.Get() ||
        GetCurrentThreadId() != s.thread || !WindowOwner(s.window) || FAILED(s.device->GetDeviceRemovedReason())) return false;
    if (s.cursor.nativeEpoch && (s.cursor.nativeEpoch != nativeEpoch || !OptiInput::ValidateGameCursor(s.cursor))) return false;
    ComPtr<ID3D11Texture2D> actual; DXGI_SWAP_CHAIN_DESC swap{};
    if (FAILED(s.chain->GetBuffer(0, IID_PPV_ARGS(&actual))) || actual.Get() != s.backbuffer.Get() ||
        FAILED(s.chain->GetDesc(&swap)) || swap.OutputWindow != s.window) return false;
    for (UINT n = 0; n < s.liveTargets.size(); ++n) s.liveTargets[n] = s.targets[n].Get();
    s.liveDepth = s.depthView.Get(); s.nextStep = 0; s.submitted = true; return true;
}

NativeOverlayResult NativeOverlayPacket::Execute(ID3D11DeviceContext* c, UINT index) noexcept
{
    auto& s = *state_;
    if (!s.prepared || !s.submitted || c != s.context.Get() || index != s.nextStep || index >= s.stepCount)
        return {false, false, "Native combined tape overlay cursor changed"};
    const auto& step = s.steps[s.nextStep++];
    if ((step.op == ActionOp::BeginMouse || step.op == ActionOp::MouseDraw) && !OptiInput::ValidateGameCursor(s.cursor))
        return {false, false, "Mouse window/input epoch changed inside reserved native tape"};
    // The enclosing independent reservation owns text AND overlay roots before
    // this first setter. A failure anywhere quarantines that entire packet.
    if (step.op == ActionOp::BeginMouse)
    {
        auto& scope = s.mouseScopes[step.scope];
        if (!scope.Capture(c)) return {false, false, "Mouse nested state changed after whole-tape preflight"};
        if (scope.role == 1)
        {
            const auto& shaders = s.library->mouseStages[scope.shader];
            c->VSSetShader(shaders.vs.Get(), nullptr, 0); c->PSSetShader(shaders.ps.Get(), nullptr, 0);
            c->HSSetShader(nullptr, nullptr, 0); c->DSSetShader(nullptr, nullptr, 0); c->GSSetShader(nullptr, nullptr, 0);
            c->IASetInputLayout(nullptr); c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            c->RSSetState(s.library->mouseRaster.Get());
        }
        const FLOAT zero[4]{}; c->OMSetBlendState(s.library->mouseBlend[scope.role - 1].Get(), zero, 0xffffffffu);
    }
    else if (step.op == ActionOp::EndMouse)
    {
        const auto& scope = s.mouseScopes[step.scope]; scope.Restore(c);
        for (unsigned n = 0; n < 8; ++n) s.liveTargets[n] = scope.targets[n].Get();
        s.liveDepth = scope.depth.Get();
    }
    else if (step.op == ActionOp::MouseDraw)
    {
        auto* parameters = step.mouseParameterView.Get();
        c->VSSetShaderResources(IniParameterRegister, 1, &parameters);
        // Original Mouse PS has no t120 reader. Preserve its entry binding.
        c->Draw(4, 0);
    }
    else if (step.op == ActionOp::BindOutput)
    {
        if (step.slot == 8) s.liveDepth = nullptr; else s.liveTargets[step.slot] = nullptr;
        c->OMSetRenderTargets(static_cast<UINT>(s.liveTargets.size()), s.liveTargets.data(), s.liveDepth);
    }
    else if (step.op == ActionOp::BackbufferViewport)
    {
        s.liveTargets[0] = s.target.Get();
        c->OMSetRenderTargets(static_cast<UINT>(s.liveTargets.size()), s.liveTargets.data(), s.liveDepth);
        c->RSSetViewports(1, &s.viewport);
    }
    else if (step.op == ActionOp::Bind)
    {
        auto* srv = step.kept.value.srv;
        if (step.vertex) c->VSSetShaderResources(step.slot, 1, &srv);
        else c->PSSetShaderResources(step.slot, 1, &srv);
    }
    else
    {
        const auto& shaders = s.library->stages[step.shader];
        c->VSSetShader(shaders.vs.Get(), nullptr, 0); c->PSSetShader(shaders.ps.Get(), nullptr, 0);
        c->HSSetShader(nullptr, nullptr, 0); c->DSSetShader(nullptr, nullptr, 0);
        c->GSSetShader(nullptr, nullptr, 0); c->CSSetShader(nullptr, nullptr, 0);
        c->IASetInputLayout(nullptr); c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->RSSetState(s.library->raster.Get()); const FLOAT zero[4]{};
        c->OMSetBlendState(s.library->blend.Get(), zero, 0xffffffffu);
        auto* parameters = s.statics->ParameterView(step.parameters);
        c->PSSetShaderResources(IniParameterRegister, 1, &parameters);
        c->Draw(3, 0); // exact immutable snapshot at THIS command, never text's dynamic view
        auto* previousParameters = s.pixels[4].Get();
        c->PSSetShaderResources(IniParameterRegister, 1, &previousParameters);
        c->VSSetShader(s.vs.Get(), nullptr, 0); c->HSSetShader(s.hs.Get(), nullptr, 0);
        c->DSSetShader(s.ds.Get(), nullptr, 0); c->GSSetShader(s.gs.Get(), nullptr, 0);
        c->PSSetShader(s.ps.Get(), nullptr, 0); c->CSSetShader(s.cs.Get(), nullptr, 0);
        c->IASetInputLayout(s.layout.Get()); c->IASetPrimitiveTopology(s.topology);
        c->RSSetState(s.raster.Get()); c->OMSetBlendState(s.blend.Get(), s.factors.data(), s.sampleMask);
    }
    if (FAILED(s.device->GetDeviceRemovedReason()))
        return {false, true, "Native combined tape overlay device loss; full packet must quarantine"};
    return {true, true, nullptr};
}
} // namespace gimi
