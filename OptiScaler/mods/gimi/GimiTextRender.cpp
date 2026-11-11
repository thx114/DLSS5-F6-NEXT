#include "GimiTextRender.h"
#include "third_party/textprinter/TextPrinterSource.h"
#include <d3dcompiler.h>
#include <d3d11_1.h>
#include <d3d11shader.h>
#include <array>
#include <fstream>
#include <stdexcept>
#include <string>

namespace gimi
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr unsigned Font = 1, Position = 2, Text = 4, Parameters = 8, Ini = 16;

bool ReflectedStage(ID3DBlob& bytes, unsigned kind, unsigned expected) noexcept
{
    ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(bytes.GetBufferPointer(), bytes.GetBufferSize(),
        IID_PPV_ARGS(reflection.GetAddressOf())))) return false;
    D3D11_SHADER_DESC desc{};
    if (FAILED(reflection->GetDesc(&desc)) || D3D11_SHVER_GET_TYPE(desc.Version) != kind ||
        D3D11_SHVER_GET_MAJOR(desc.Version) != 5 || D3D11_SHVER_GET_MINOR(desc.Version)) return false;
    unsigned seen = 0;
    for (UINT i = 0; i < desc.BoundResources; ++i)
    {
        D3D11_SHADER_INPUT_BIND_DESC binding{};
        if (FAILED(reflection->GetResourceBindingDesc(i, &binding)) || binding.BindCount != 1) return false;
        unsigned bit = 0;
        if (binding.Type == D3D_SIT_TEXTURE && binding.BindPoint == 100 &&
            binding.Dimension == D3D_SRV_DIMENSION_TEXTURE2D && binding.ReturnType == D3D_RETURN_TYPE_FLOAT) bit = Font;
        if (binding.Type == D3D_SIT_STRUCTURED && binding.BindPoint == 112 &&
            binding.Dimension == D3D_SRV_DIMENSION_BUFFER) bit = Position;
        if (binding.Type == D3D_SIT_TEXTURE && binding.BindPoint == 113 &&
            binding.Dimension == D3D_SRV_DIMENSION_BUFFER && binding.ReturnType == D3D_RETURN_TYPE_UINT) bit = Text;
        if (binding.Type == D3D_SIT_STRUCTURED && binding.BindPoint == 114 &&
            binding.Dimension == D3D_SRV_DIMENSION_BUFFER) bit = Parameters;
        if (binding.Type == D3D_SIT_TEXTURE && binding.BindPoint == IniParameterRegister &&
            binding.Dimension == D3D_SRV_DIMENSION_TEXTURE1D && binding.ReturnType == D3D_RETURN_TYPE_FLOAT) bit = Ini;
        if (!(expected & bit) || (seen & bit)) return false;
        seen |= bit;
    }
    return seen == expected;
}

bool CompileStage(const std::string& filename, const char* macro, const char* profile,
    unsigned kind, unsigned bindings, ComPtr<ID3DBlob>& result) noexcept
{
    const D3D_SHADER_MACRO macros[] = {{macro, ""}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> errors;
    return SUCCEEDED(D3DCompile(detail::FixedTextPrinter.data(), detail::FixedTextPrinter.size(),
        filename.c_str(), macros, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        result.GetAddressOf(), errors.GetAddressOf())) && result && ReflectedStage(*result.Get(), kind, bindings);
}

bool OriginalSource(const std::filesystem::path& file, std::size_t& total)
{
    std::ifstream input(file, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const auto end = input.tellg();
    if (end <= 0 || end > 1024 * 1024 || total > 8u * 1024u * 1024u - static_cast<std::size_t>(end)) return false;
    std::string source(static_cast<std::size_t>(end), char{});
    input.seekg(0);
    if (!input.read(source.data(), static_cast<std::streamsize>(source.size())) ||
        input.peek() != std::char_traits<char>::eof()) return false;
    std::string normalized; normalized.reserve(source.size());
    for (std::size_t i = 0; i < source.size(); ++i)
    {
        if (source[i] == char(13))
        {
            if (++i == source.size() || source[i] != char(10)) return false;
        }
        normalized.push_back(source[i]);
    }
    if (std::string_view(normalized.data(), normalized.size()) != detail::FixedTextPrinter) return false;
    total += source.size();
    return true;
}
} // namespace

std::shared_ptr<const NativeTextShaderLibrary> CreateNativeTextShaderLibrary(
    std::shared_ptr<const Plan> plan, ID3D11Device* device, std::vector<Diagnostic>& diagnostics)
{
    try
    {
        if (!plan || !device || device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
            throw std::runtime_error("Native RenderText needs a real FL11 D3D11 device");
        auto library = std::make_shared<NativeTextShaderLibrary>();
        library->plan = std::move(plan);
        library->stages.resize(library->plan->RenderTexts().size());
        std::size_t total = 0;
        for (Id id = 0; id < library->stages.size(); ++id)
        {
            const auto& spec = library->plan->RenderTexts()[id];
            auto fail = [&](const char* message) { diagnostics.push_back({DiagnosticCode::Unsupported, spec.source, message}); };
            if (!OriginalSource(spec.shader, total))
            { fail("Declared RenderText source differs from fixed TextPrinter or exceeds static budget"); return {}; }
            const auto filename = PathUtf8(spec.shader);
            ComPtr<ID3DBlob> vs, gs, ps;
            if (!CompileStage(filename, "VERTEX_SHADER", "vs_5_0", D3D11_SHVER_VERTEX_SHADER, 0, vs) ||
                !CompileStage(filename, "GEOMETRY_SHADER", "gs_5_0", D3D11_SHVER_GEOMETRY_SHADER,
                    Font | Position | Text | Parameters | Ini, gs) ||
                !CompileStage(filename, "PIXEL_SHADER", "ps_5_0", D3D11_SHVER_PIXEL_SHADER,
                    Font | Parameters, ps))
            { fail("RenderText VS/GS/PS compile or exact reflected resource ABI refused"); return {}; }
            auto& stage = library->stages[id];
            if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr,
                    stage.vs.GetAddressOf())) ||
                FAILED(device->CreateGeometryShader(gs->GetBufferPointer(), gs->GetBufferSize(), nullptr,
                    stage.gs.GetAddressOf())) ||
                FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr,
                    stage.ps.GetAddressOf())))
            { fail("Real RenderText VS/GS/PS creation failed"); return {}; }
        }
        return library;
    }
    catch (const std::exception& e)
    { diagnostics.push_back({DiagnosticCode::Unsupported, {}, e.what()}); return {}; }
}

namespace
{
constexpr UINT GsSlots[] = {100, 112, 113, 114, IniParameterRegister};
constexpr UINT PsSlots[] = {100, 114};
bool OnDevice(ID3D11DeviceChild* child, ID3D11Device* expected) noexcept
{
    if (!child || !expected) return false;
    ComPtr<ID3D11Device> actual; child->GetDevice(actual.GetAddressOf());
    return actual.Get() == expected;
}
bool IsBackbufferView(ID3D11View* view, ID3D11Resource* backbuffer) noexcept
{
    if (!view) return false;
    ComPtr<ID3D11Resource> resource; view->GetResource(resource.GetAddressOf());
    return !resource || resource.Get() == backbuffer;
}
bool NoBackbufferReadOrUav(ID3D11DeviceContext* c, ID3D11Device* device, ID3D11Resource* backbuffer) noexcept
{
    // RTV binding can silently null a conflicting SRV/UAV in ANY stage. The
    // original pipeline must be intact when the wrapper returns. Reject even
    // a slot that our text draw would replace rather than relying on D3D fixup.
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
        bool safe = true;
        for (auto*& view : views) if (view)
        {
            if (IsBackbufferView(view, backbuffer)) safe = false;
            view->Release(); view = nullptr;
        }
        if (!safe) return false;
    }
    const UINT count = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ?
        D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT;
    std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> unordered{};
    // OMSetRenderTargets changes OM UAV slots. We have no permission to
    // reinterpret an existing OM UAV counter/slot layout: any live one rejects.
    c->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, count, unordered.data());
    bool safe = true;
    for (UINT i = 0; i < count; ++i) if (unordered[i])
    { safe = false; unordered[i]->Release(); unordered[i] = nullptr; }
    c->CSGetUnorderedAccessViews(0, count, unordered.data());
    for (UINT i = 0; i < count; ++i) if (unordered[i])
    {
        if (IsBackbufferView(unordered[i], backbuffer)) safe = false;
        unordered[i]->Release(); unordered[i] = nullptr;
    }
    return safe;
}
} // namespace

bool NativeTextDrawState::Snapshot::Capture(ID3D11DeviceContext* c) noexcept
{
    Reset();
    UINT classes = 0;
    c->VSGetShader(&vs, nullptr, &classes); if (classes) return false;
    c->HSGetShader(&hs, nullptr, &classes); if (classes) return false;
    c->DSGetShader(&ds, nullptr, &classes); if (classes) return false;
    c->GSGetShader(&gsShader, nullptr, &classes); if (classes) return false;
    c->PSGetShader(&psShader, nullptr, &classes); if (classes) return false;
    c->CSGetShader(&cs, nullptr, &classes); if (classes) return false;
    c->CSGetUnorderedAccessViews(0, 1, &uav);
    ID3D11RenderTargetView* rt[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rt, depthView.GetAddressOf());
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) targets[i].Attach(rt[i]);
    c->OMGetBlendState(blend.GetAddressOf(), factors.data(), &sampleMask);
    c->OMGetDepthStencilState(depth.GetAddressOf(), &stencilRef);
    c->RSGetState(raster.GetAddressOf());
    viewportCount = scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    c->RSGetViewports(&viewportCount, viewports.data());
    c->RSGetScissorRects(&scissorCount, scissors.data());
    if (viewportCount > viewports.size() || scissorCount > scissors.size()) return false;
    c->IAGetInputLayout(layout.GetAddressOf()); c->IAGetPrimitiveTopology(&topology);
    for (unsigned i = 0; i < std::size(GsSlots); ++i) c->GSGetShaderResources(GsSlots[i], 1, gs[i].GetAddressOf());
    for (unsigned i = 0; i < std::size(PsSlots); ++i) c->PSGetShaderResources(PsSlots[i], 1, ps[i].GetAddressOf());
    return true;
}
void NativeTextDrawState::Snapshot::Restore(ID3D11DeviceContext* c) const noexcept
{
    ID3D11RenderTargetView* rt[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) rt[i] = targets[i].Get();
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rt, depthView.Get());
    c->OMSetBlendState(blend.Get(), factors.data(), sampleMask);
    c->OMSetDepthStencilState(depth.Get(), stencilRef);
    c->RSSetState(raster.Get()); c->RSSetViewports(viewportCount, viewportCount ? viewports.data() : nullptr);
    c->RSSetScissorRects(scissorCount, scissorCount ? scissors.data() : nullptr);
    c->IASetInputLayout(layout.Get()); c->IASetPrimitiveTopology(topology);
    for (unsigned i = 0; i < std::size(GsSlots); ++i)
    { auto* view = gs[i].Get(); c->GSSetShaderResources(GsSlots[i], 1, &view); }
    for (unsigned i = 0; i < std::size(PsSlots); ++i)
    { auto* view = ps[i].Get(); c->PSSetShaderResources(PsSlots[i], 1, &view); }
    auto* previousUav = uav.Get(); const UINT keep = ~UINT{0};
    c->CSSetUnorderedAccessViews(0, 1, &previousUav, &keep);
    c->VSSetShader(vs.Get(), nullptr, 0); c->HSSetShader(hs.Get(), nullptr, 0);
    c->DSSetShader(ds.Get(), nullptr, 0); c->GSSetShader(gsShader.Get(), nullptr, 0);
    c->PSSetShader(psShader.Get(), nullptr, 0); c->CSSetShader(cs.Get(), nullptr, 0);
}
void NativeTextDrawState::Snapshot::Reset() noexcept
{
    depthView.Reset(); blend.Reset(); depth.Reset(); raster.Reset(); layout.Reset();
    vs.Reset(); hs.Reset(); ds.Reset(); gsShader.Reset(); psShader.Reset(); cs.Reset(); uav.Reset();
    for (auto& view : targets) view.Reset();
    for (auto& view : gs) view.Reset();
    for (auto& view : ps) view.Reset();
    viewportCount = scissorCount = 0;
}

bool NativeTextDrawState::Initialize(ID3D11Device* device) noexcept
{
    if (!device || device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) return false;
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = FALSE; // original TextPrinter packs a texcoord in z
    if (FAILED(device->CreateRasterizerState(&raster, overlayRaster_.GetAddressOf()))) return false;
    D3D11_BLEND_DESC blend{};
    auto& rt = blend.RenderTarget[0]; rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_ZERO; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blend, overlayBlend_.GetAddressOf()))) return false;
    D3D11_DEPTH_STENCIL_DESC depth{}; depth.DepthEnable = FALSE; depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(device->CreateDepthStencilState(&depth, overlayDepth_.GetAddressOf()))) return false;
    return true;
}

bool NativeTextDrawState::Prepare(ID3D11DeviceContext* c, IDXGISwapChain* chain,
    ID3D11Device* device, HWND expectedWindow) noexcept
{
    ReleaseTransient();
    if (!c || !chain || !device || !expectedWindow || !overlayRaster_ || !overlayBlend_ || !overlayDepth_ ||
        c->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || !OnDevice(c, device)) return false;
    DWORD process = 0;
    if (!IsWindow(expectedWindow) || !GetWindowThreadProcessId(expectedWindow, &process) ||
        process != GetCurrentProcessId()) return false;
    DXGI_SWAP_CHAIN_DESC swap{};
    if (FAILED(chain->GetDesc(&swap)) || swap.OutputWindow != expectedWindow ||
        !(swap.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT) || !swap.BufferCount) return false;
    if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(backbuffer_.GetAddressOf()))) ||
        !OnDevice(backbuffer_.Get(), device)) return false;
    D3D11_TEXTURE2D_DESC desc{}; backbuffer_->GetDesc(&desc);
    if (!desc.Width || !desc.Height || desc.ArraySize != 1 || desc.MipLevels != 1 ||
        desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality || desc.Usage != D3D11_USAGE_DEFAULT ||
        !(desc.BindFlags & D3D11_BIND_RENDER_TARGET) || desc.CPUAccessFlags || desc.MiscFlags ||
        desc.Format == DXGI_FORMAT_UNKNOWN || desc.Format != swap.BufferDesc.Format ||
        (swap.BufferDesc.Width && swap.BufferDesc.Width != desc.Width) ||
        (swap.BufferDesc.Height && swap.BufferDesc.Height != desc.Height) ||
        !NoBackbufferReadOrUav(c, device, backbuffer_.Get())) return false;
    if (FAILED(device->CreateRenderTargetView(backbuffer_.Get(), nullptr, target_.GetAddressOf())) ||
        !OnDevice(target_.Get(), device)) return false;
    ComPtr<ID3D11Resource> targetResource; target_->GetResource(targetResource.GetAddressOf());
    D3D11_RENDER_TARGET_VIEW_DESC targetDesc{}; target_->GetDesc(&targetDesc);
    if (targetResource.Get() != backbuffer_.Get() || targetDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D ||
        targetDesc.Format != desc.Format || targetDesc.Texture2D.MipSlice) return false;
    ID3D11Buffer* so[D3D11_SO_BUFFER_SLOT_COUNT]{};
    c->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, so);
    bool noSo = true;
    for (auto*& buffer : so) if (buffer) { noSo = false; buffer->Release(); buffer = nullptr; }
    if (!noSo) return false; // GS text output must reach raster, never an active SO target
    if (!entry_.Capture(c)) return false;
    overlayViewport_ = {0, 0, static_cast<FLOAT>(desc.Width), static_cast<FLOAT>(desc.Height), 0, 1};
    prepared_ = true; return true;
}

bool NativeTextDrawState::Draw(ID3D11DeviceContext* c, const NativeTextStages& shaders,
    ID3D11ShaderResourceView* font, ID3D11ShaderResourceView* positions, ID3D11ShaderResourceView* text,
    ID3D11ShaderResourceView* parameters, ID3D11ShaderResourceView* ini) noexcept
{
    if (!prepared_ || !scope_.Capture(c)) return false;
    struct RestoreScope { Snapshot& saved; ID3D11DeviceContext* c;
        ~RestoreScope() { saved.Restore(c); } } restore{scope_, c};
    ID3D11RenderTargetView* target = target_.Get();
    const FLOAT factor[] = {1, 1, 1, 1};
    // A text-only caller can inherit the very Position buffer at CS u0. The
    // scoped formatter restored that original UAV; physically unbind it before
    // the GS SRV reader, then restore the exact view/counter after the draw.
    ID3D11UnorderedAccessView* none = nullptr; const UINT keep = ~UINT{0};
    c->CSSetUnorderedAccessViews(0, 1, &none, &keep);
    c->CSSetShader(nullptr, nullptr, 0); c->HSSetShader(nullptr, nullptr, 0); c->DSSetShader(nullptr, nullptr, 0);
    c->VSSetShader(shaders.vs.Get(), nullptr, 0); c->GSSetShader(shaders.gs.Get(), nullptr, 0);
    c->PSSetShader(shaders.ps.Get(), nullptr, 0);
    c->IASetInputLayout(nullptr); c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    c->RSSetState(overlayRaster_.Get()); c->RSSetViewports(1, &overlayViewport_);
    c->OMSetRenderTargets(1, &target, nullptr);
    c->OMSetBlendState(overlayBlend_.Get(), factor, 0xffffffffu);
    c->OMSetDepthStencilState(overlayDepth_.Get(), 0);
    ID3D11ShaderResourceView* gsViews[] = {font, positions, text, parameters, ini};
    ID3D11ShaderResourceView* psViews[] = {font, parameters};
    for (unsigned i = 0; i < std::size(GsSlots); ++i) c->GSSetShaderResources(GsSlots[i], 1, &gsViews[i]);
    for (unsigned i = 0; i < std::size(PsSlots); ++i) c->PSSetShaderResources(PsSlots[i], 1, &psViews[i]);
    c->Draw(4096, 0);
    ID3D11ShaderResourceView* nullView = nullptr;
    for (const auto slot : GsSlots) c->GSSetShaderResources(slot, 1, &nullView);
    for (const auto slot : PsSlots) c->PSSetShaderResources(slot, 1, &nullView);
    return true;
}

void NativeTextDrawState::Restore(ID3D11DeviceContext* c) const noexcept
{
    if (!prepared_) return;
    entry_.Restore(c);
}

void NativeTextDrawState::ReleaseTransient() noexcept
{
    prepared_ = false; backbuffer_.Reset(); target_.Reset();
    entry_.Reset(); scope_.Reset();
}
} // namespace gimi
