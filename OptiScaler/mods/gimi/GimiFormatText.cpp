#include "GimiFormatText.h"
#include "third_party/textprinter/TextPrinterSource.h"
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <stdexcept>

namespace gimi
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr UINT Slots[] = {100, 113, 114, IniParameterRegister};
constexpr UINT UavSlots = 64; // FL11.1; FL11.0 uses the actual first eight below
bool OnDevice(ID3D11DeviceChild* child, ID3D11Device* expected) noexcept
{
    if (!child) return false;
    ComPtr<ID3D11Device> actual; child->GetDevice(actual.GetAddressOf()); return actual.Get() == expected;
}
bool ShaderContract(ID3DBlob& bytes) noexcept
{
    ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(bytes.GetBufferPointer(), bytes.GetBufferSize(), IID_PPV_ARGS(reflection.GetAddressOf())))) return false;
    D3D11_SHADER_DESC desc{};
    if (FAILED(reflection->GetDesc(&desc)) || D3D11_SHVER_GET_TYPE(desc.Version) != D3D11_SHVER_COMPUTE_SHADER ||
        D3D11_SHVER_GET_MAJOR(desc.Version) != 5 || D3D11_SHVER_GET_MINOR(desc.Version) || desc.BoundResources != 5) return false;
    UINT x = 0, y = 0, z = 0;
    if (reflection->GetThreadGroupSize(&x, &y, &z) != 1 || x != 1 || y != 1 || z != 1) return false;
    unsigned mask = 0;
    for (UINT i = 0; i < desc.BoundResources; ++i)
    {
        D3D11_SHADER_INPUT_BIND_DESC binding{};
        if (FAILED(reflection->GetResourceBindingDesc(i, &binding)) || binding.BindCount != 1) return false;
        unsigned bit = 0;
        if (binding.Type == D3D_SIT_TEXTURE && binding.BindPoint == 100 && binding.Dimension == D3D_SRV_DIMENSION_TEXTURE2D && binding.ReturnType == D3D_RETURN_TYPE_FLOAT) bit = 1;
        if (binding.Type == D3D_SIT_TEXTURE && binding.BindPoint == 113 && binding.Dimension == D3D_SRV_DIMENSION_BUFFER && binding.ReturnType == D3D_RETURN_TYPE_UINT) bit = 2;
        if (binding.Type == D3D_SIT_STRUCTURED && binding.BindPoint == 114 && binding.Dimension == D3D_SRV_DIMENSION_BUFFER) bit = 4;
        if (binding.Type == D3D_SIT_TEXTURE && binding.BindPoint == IniParameterRegister && binding.Dimension == D3D_SRV_DIMENSION_TEXTURE1D && binding.ReturnType == D3D_RETURN_TYPE_FLOAT) bit = 8;
        if (binding.Type == D3D_SIT_UAV_RWSTRUCTURED && binding.BindPoint == 0 && binding.Dimension == D3D_SRV_DIMENSION_BUFFER) bit = 16;
        if (!bit || (mask & bit)) return false;
        mask |= bit;
    }
    return mask == 31;
}
bool SameBinding(Binding a, Binding b) noexcept
{
    return a.object == b.object && a.view == b.view && a.generation == b.generation && a.stride == b.stride &&
        a.offset == b.offset && a.format == b.format && a.byteSize == b.byteSize;
}
struct SavedState
{
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ComputeShader> cs;
    std::array<ComPtr<ID3D11ShaderResourceView>, 4> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    void Reset() noexcept
    {
        vs.Reset(); hs.Reset(); ds.Reset(); gs.Reset(); ps.Reset(); cs.Reset(); uav.Reset();
        for (auto& view : srv) view.Reset();
    }
    bool Capture(ID3D11DeviceContext* context) noexcept
    {
        Reset();
        UINT classes = 0;
        context->VSGetShader(vs.GetAddressOf(), nullptr, &classes); if (classes) return false;
        context->HSGetShader(hs.GetAddressOf(), nullptr, &classes); if (classes) return false;
        context->DSGetShader(ds.GetAddressOf(), nullptr, &classes); if (classes) return false;
        context->GSGetShader(gs.GetAddressOf(), nullptr, &classes); if (classes) return false;
        context->PSGetShader(ps.GetAddressOf(), nullptr, &classes); if (classes) return false;
        context->CSGetShader(cs.GetAddressOf(), nullptr, &classes); if (classes) return false;
        for (unsigned i = 0; i < std::size(Slots); ++i) context->CSGetShaderResources(Slots[i], 1, srv[i].GetAddressOf());
        context->CSGetUnorderedAccessViews(0, 1, uav.GetAddressOf());
        ComPtr<ID3D11Predicate> predicate; BOOL value = FALSE;
        context->GetPredication(predicate.GetAddressOf(), &value);
        return !predicate; // unknown conditional dispatch is outside this first consumer
    }
    void Restore(ID3D11DeviceContext* context) const noexcept
    {
        ID3D11UnorderedAccessView* nullUav = nullptr; const UINT keep = ~UINT{0};
        context->CSSetUnorderedAccessViews(0, 1, &nullUav, &keep);
        for (unsigned i = 0; i < std::size(Slots); ++i)
        { auto* view = srv[i].Get(); context->CSSetShaderResources(Slots[i], 1, &view); }
        auto* previous = uav.Get(); context->CSSetUnorderedAccessViews(0, 1, &previous, &keep);
        context->VSSetShader(vs.Get(), nullptr, 0); context->HSSetShader(hs.Get(), nullptr, 0);
        context->DSSetShader(ds.Get(), nullptr, 0); context->GSSetShader(gs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0); context->CSSetShader(cs.Get(), nullptr, 0);
    }
};
} // namespace

std::shared_ptr<const FormatTextLibrary> CreateFormatTextLibrary(
    std::shared_ptr<const Plan> plan, ID3D11Device* device, std::vector<Diagnostic>& diagnostics)
{
    try
    {
        if (!plan || !device || device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) throw std::runtime_error("FormatText requires actual FL11 compute");
        auto library = std::make_shared<FormatTextLibrary>(); library->plan = plan;
        library->shaders.resize(plan->FormatTexts().size());
        std::size_t total = 0;
        for (Id id = 0; id < library->shaders.size(); ++id)
        {
            const auto& spec = plan->FormatTexts()[id];
            auto fail = [&](const char* reason) { diagnostics.push_back({DiagnosticCode::Unsupported, spec.source, reason}); };
            std::ifstream input(spec.shader, std::ios::binary | std::ios::ate);
            if (!input) { fail("Cannot read explicitly declared FormatText HLSL"); return {}; }
            const auto end = input.tellg();
            if (end <= 0 || end > 1024 * 1024 || total > 8u * 1024u * 1024u - static_cast<std::size_t>(end))
            { fail("FormatText static source budget exceeded"); return {}; }
            std::vector<char> source(static_cast<std::size_t>(end)); input.seekg(0);
            if (!input.read(source.data(), static_cast<std::streamsize>(source.size())) || input.peek() != std::char_traits<char>::eof())
            { fail("FormatText HLSL changed/incomplete at preload"); return {}; }
            std::string normalized; normalized.reserve(source.size());
            for (std::size_t i = 0; i < source.size(); ++i)
            {
                if (source[i] == '\r')
                {
                    if (++i == source.size() || source[i] != '\n')
                    { fail("FormatText HLSL contains unsupported bare CR"); return {}; }
                }
                normalized.push_back(source[i]);
            }
            if (std::string_view(normalized.data(), normalized.size()) != detail::FixedTextPrinter)
            { fail("Declared FormatText source differs from fixed GIMI TextPrinter HLSL; unknown compute semantics refused"); return {}; }
            total += source.size();
            // Fixed CustomShader::compile contract. No cache output, file include
            // handler, foreign DLL, draw-time compilation or generated callback.
            const D3D_SHADER_MACRO macros[] = {{"COMPUTE_SHADER", ""}, {nullptr, nullptr}};
            ComPtr<ID3DBlob> bytecode, errors;
            const auto filename = PathUtf8(spec.shader);
            if (FAILED(D3DCompile(detail::FixedTextPrinter.data(), detail::FixedTextPrinter.size(), filename.c_str(), macros, nullptr, "main", "cs_5_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, bytecode.GetAddressOf(), errors.GetAddressOf())) ||
                !bytecode || !ShaderContract(*bytecode.Get()))
            { fail("FormatText main/cs_5_0 source compile or exact reflected t100/t113/t114/t120/u0 ABI refused"); return {}; }
            if (FAILED(device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr,
                library->shaders[id].object.GetAddressOf()))) { fail("Real FormatText compute shader creation failed"); return {}; }
        }
        return library;
    }
    catch (const std::exception& e) { diagnostics.push_back({DiagnosticCode::Unsupported, {}, e.what()}); return {}; }
}

struct NativeComputePacket::Storage
{
    struct Step { ActionOp op = ActionOp::FormatText; UINT index = 0; };
    struct Dispatch
    {
        Id shader = InvalidId;
        std::array<Dx11LeaseView, 4> resources{};
        std::vector<float> image;
        ComPtr<ID3D11Texture1D> parameters;
        ComPtr<ID3D11ShaderResourceView> parameterView;
    };
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::shared_ptr<const Dx11StaticResources> statics;
    std::shared_ptr<const FormatTextLibrary> library;
    std::shared_ptr<const NativeTextShaderLibrary> textLibrary;
    SavedState saved, dispatchScope;
    NativeTextDrawState textState;
    std::unique_ptr<NativeOverlayPacket> overlay;
    bool overlayPrepared = false;
    std::array<Dispatch, NativeFormatTextDispatches> dispatches;
    std::array<Id, NativeFormatTextDispatches> drawShaders{};
    std::vector<Step> steps;
    std::vector<Dx11AliasChange> aliases;
    std::vector<Dx11RetainedBinding> clears;
    UINT clearCount = 0, finalParameters = 0;
    bool parameterCommit = false, postPhase = false;
    std::vector<float> working;
    UINT aliasCount = 0, dispatchCount = 0, drawCount = 0, stepCount = 0;
    bool prepared = false, submitted = false;
    void ReleaseTransient() noexcept
    {
        if (overlay) overlay->Retire();
        overlayPrepared = false;
        textState.ReleaseTransient(); saved.Reset(); dispatchScope.Reset(); context.Reset();
        for (UINT i = 0; i < aliasCount; ++i) aliases[i] = {};
        for (UINT i = 0; i < dispatchCount; ++i) dispatches[i].resources = {};
        for (UINT i = 0; i < clearCount; ++i) clears[i] = {};
        clearCount = 0; parameterCommit = postPhase = false;
        aliasCount = dispatchCount = drawCount = stepCount = 0;
        prepared = submitted = false;
    }
    bool Retain(Binding binding, Dx11ResourceProvider* provider, Dx11RetainedBinding& out, Id alias = InvalidId) const noexcept
    {
        out = {};
        if (!binding.object && !SameBinding(binding, {})) return false;
        if (statics->Resolve(binding, out.value))
        {
            if (out.value.resource) out.statics = statics;
            out.stableReadOnly = !out.value.uav;
            return true;
        }
        if (provider && alias != InvalidId && provider->RetainNativeAlias(alias, binding, out)) return true;
        // Earlier assignments within THIS ordered packet own both ends of
        // their journal even if an alias was overwritten again later.
        for (UINT n = aliasCount; n-- > 0;)
        {
            if (SameBinding(aliases[n].next.value.binding, binding) &&
                (aliases[n].next.game || aliases[n].next.statics || !binding.object))
            { out = aliases[n].next; return true; }
            if (SameBinding(aliases[n].previous.value.binding, binding) &&
                (aliases[n].previous.game || aliases[n].previous.statics || !binding.object))
            { out = aliases[n].previous; return true; }
        }
        return provider && (provider->ResolvePrivateTexture(binding, out) || provider->RetainNativeReference(binding, out)) &&
            SameBinding(binding, out.value.binding);
    }
    bool ImmutableParameters(UINT initial, UINT& page) const noexcept
    {
        // The WHOLE tape-position image is the key, including restored text
        // x/y/w and every unwritten default. Resolve it atomically: walking one
        // scalar at a time would demand unreachable intermediate overlay pages.
        // No upload, allocation, guessed defaults or partial-image fallback.
        if (!statics->ParameterView(initial)) return false;
        return statics->Files()->Parameters().Find(working, page) && statics->ParameterView(page) != nullptr;
    }
    bool Views(Dispatch& dispatch) const noexcept
    {
        const auto& font = dispatch.resources[0]; const auto& text = dispatch.resources[1];
        const auto& params = dispatch.resources[2]; const auto& positions = dispatch.resources[3];
        for (const auto& view : dispatch.resources)
        {
            if (!view.resource || !view.srv || !OnDevice(view.resource, device.Get()) || !OnDevice(view.srv, device.Get()) ||
                view.binding.generation != statics->DeviceGeneration() || view.binding.offset) return false;
            ComPtr<ID3D11Resource> actual; view.srv->GetResource(actual.GetAddressOf());
            if (actual.Get() != view.resource) return false;
        }
        for (unsigned i = 0; i < 3; ++i) if (dispatch.resources[i].resource == positions.resource) return false;
        if (!font.sampleableFloat2D || font.buffer || !text.buffer || !params.buffer || !positions.buffer || !positions.uav) return false;
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(font.resource->QueryInterface(IID_PPV_ARGS(texture.GetAddressOf())))) return false;
        D3D11_TEXTURE2D_DESC fontDesc{}; texture->GetDesc(&fontDesc);
        D3D11_SHADER_RESOURCE_VIEW_DESC fontView{}; font.srv->GetDesc(&fontView);
        // TextPrinter COMPUTE_SHADER get_meta uses these fixed atlas dimensions.
        if (fontDesc.Width != 256 || fontDesc.Height != 96 || fontDesc.ArraySize != 1 || fontDesc.SampleDesc.Count != 1 ||
            fontDesc.Usage != D3D11_USAGE_IMMUTABLE || fontView.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
            fontView.Texture2D.MostDetailedMip != 0) return false;
        D3D11_BUFFER_DESC textDesc{}, paramsDesc{}, positionsDesc{};
        text.buffer->GetDesc(&textDesc); params.buffer->GetDesc(&paramsDesc); positions.buffer->GetDesc(&positionsDesc);
        D3D11_SHADER_RESOURCE_VIEW_DESC textView{}, paramsView{}, positionView{};
        text.srv->GetDesc(&textView); params.srv->GetDesc(&paramsView); positions.srv->GetDesc(&positionView);
        D3D11_UNORDERED_ACCESS_VIEW_DESC output{}; positions.uav->GetDesc(&output);
        ComPtr<ID3D11Resource> outputResource; positions.uav->GetResource(outputResource.GetAddressOf());
        if (!OnDevice(positions.uav, device.Get()) || outputResource.Get() != positions.resource ||
            textDesc.Usage != D3D11_USAGE_IMMUTABLE || textDesc.CPUAccessFlags || textDesc.MiscFlags || !textDesc.ByteWidth || textDesc.ByteWidth > 4093 ||
            textView.ViewDimension != D3D11_SRV_DIMENSION_BUFFER || textView.Format != DXGI_FORMAT_R8_UINT ||
            textView.Buffer.FirstElement || textView.Buffer.NumElements != textDesc.ByteWidth ||
            paramsDesc.Usage != D3D11_USAGE_IMMUTABLE || paramsDesc.CPUAccessFlags || paramsDesc.ByteWidth != 72 || paramsDesc.StructureByteStride != 72 ||
            paramsDesc.MiscFlags != D3D11_RESOURCE_MISC_BUFFER_STRUCTURED || paramsView.ViewDimension != D3D11_SRV_DIMENSION_BUFFER ||
            paramsView.Format != DXGI_FORMAT_UNKNOWN || paramsView.Buffer.FirstElement || paramsView.Buffer.NumElements != 1 ||
            positionsDesc.Usage != D3D11_USAGE_DEFAULT || positionsDesc.CPUAccessFlags || positionsDesc.ByteWidth != 16 * 4096 ||
            positionsDesc.StructureByteStride != 16 || positionsDesc.MiscFlags != D3D11_RESOURCE_MISC_BUFFER_STRUCTURED ||
            output.ViewDimension != D3D11_UAV_DIMENSION_BUFFER || output.Format != DXGI_FORMAT_UNKNOWN ||
            output.Buffer.FirstElement || output.Buffer.NumElements != 4096 || output.Buffer.Flags ||
            positionView.ViewDimension != D3D11_SRV_DIMENSION_BUFFER || positionView.Format != DXGI_FORMAT_UNKNOWN ||
            positionView.Buffer.FirstElement || positionView.Buffer.NumElements != 4096) return false;
        auto payload = [&](const Dx11LeaseView& view) -> const StaticResource* {
            // Never scan user/game resources. The already-preloaded fixed set is
            // indexed through its opaque binding's declared resource table.
            const auto& files = statics->Files()->Resources();
            for (Id id = 0; id < files.size(); ++id)
            {
                // Initialized aliases share the source lease, not its CPU
                // payload. Select the canonical file/inline resource once.
                if (files[id].kind == ResourceKind::Alias) continue;
                Dx11LeaseView candidate;
                if (statics->Get(id, candidate) && SameBinding(candidate.binding, view.binding)) return &files[id];
            }
            return nullptr;
        };
        const auto* textData = payload(text); const auto* parameterData = payload(params);
        if (!textData || !parameterData || textData->bytes.size() != textDesc.ByteWidth || parameterData->bytes.size() != 72) return false;
        unsigned length = 0;
        for (const auto c : textData->bytes)
        {
            if (!c) break;
            if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c >= 127) return false;
            ++length;
        }
        // Fixed shader reads text[pos-1]; empty text is not an actual formatter
        // consumer. Out-of-view typed reads provide the source's terminating 0.
        if (!length || length > 4093) return false;
        std::array<float, 18> paramsCpu{}; std::memcpy(paramsCpu.data(), parameterData->bytes.data(), 72);
        for (const auto value : paramsCpu) if (!std::isfinite(value)) return false;
        if (!(paramsCpu[2] > paramsCpu[0]) || !(paramsCpu[17] > 0) ||
            paramsCpu[16] < 0 || paramsCpu[16] > 2 || std::trunc(paramsCpu[16]) != paramsCpu[16]) return false;
        const auto& image = dispatch.image;
        if (!(image[0] > 0) || !(image[1] > 0) || !(image[3] > 0)) return false;
        const float scale = image[3] > 120 ? image[3] / 120 : 1;
        return std::isfinite(16 / image[0] * 2 * paramsCpu[17] * scale) &&
            std::isfinite(16 / image[1] * 2 * paramsCpu[17] * scale);
    }
    bool OutputResource(ID3D11Resource* resource) const noexcept
    {
        for (UINT i = 0; i < dispatchCount; ++i) if (dispatches[i].resources[3].resource == resource) return true;
        return false;
    }
    bool InputResource(ID3D11Resource* resource) const noexcept
    {
        for (UINT i = 0; i < dispatchCount; ++i)
            for (unsigned n = 0; n < 3; ++n) if (dispatches[i].resources[n].resource == resource) return true;
        return false;
    }
    bool Conflict(ID3D11View* view, bool write, bool replacing = false) const noexcept
    {
        if (!view) return false;
        ComPtr<ID3D11Resource> resource; view->GetResource(resource.GetAddressOf());
        return !resource || (!replacing && OutputResource(resource.Get())) || (write && InputResource(resource.Get()));
    }
    bool Hazards(ID3D11DeviceContext* c) const noexcept
    {
        // Binding our UAV must never silently clear a game's PS/OM/other-stage
        // view. Reject actual overlap before setters; those states stay intact.
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
            bool good = true;
            for (UINT slot = 0; slot < views.size(); ++slot) if (views[slot])
            {
                const bool replaced = stage == 5 && std::find(std::begin(Slots), std::end(Slots), slot) != std::end(Slots);
                if (Conflict(views[slot], false, replaced)) good = false;
                views[slot]->Release(); views[slot] = nullptr;
            }
            if (!good) return false;
        }
        ID3D11RenderTargetView* rt[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* depth = nullptr;
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rt, &depth);
        bool good = true;
        for (auto* view : rt) if (view) { if (Conflict(view, true)) good = false; view->Release(); }
        if (depth) { if (Conflict(depth, true)) good = false; depth->Release(); }
        if (!good) return false;
        const UINT count = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? UavSlots : D3D11_PS_CS_UAV_REGISTER_COUNT;
        std::array<ID3D11UnorderedAccessView*, UavSlots> unordered{};
        c->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, count, unordered.data());
        for (UINT slot = 0; slot < count; ++slot) if (unordered[slot])
        { if (Conflict(unordered[slot], true)) good = false; unordered[slot]->Release(); unordered[slot] = nullptr; }
        c->CSGetUnorderedAccessViews(0, count, unordered.data());
        for (UINT slot = 0; slot < count; ++slot) if (unordered[slot])
        { if (Conflict(unordered[slot], true, slot == 0)) good = false; unordered[slot]->Release(); }
        return good;
    }
};

NativeComputePacket::NativeComputePacket(UINT actions, ID3D11Device* device,
    std::shared_ptr<const Dx11StaticResources> statics, std::shared_ptr<const FormatTextLibrary> library,
    std::shared_ptr<const NativeTextShaderLibrary> textLibrary) : state_(new Storage)
{
    auto& s = *state_; s.device = device; s.statics = std::move(statics); s.library = std::move(library);
    s.textLibrary = std::move(textLibrary);
    if (s.statics->Files()->Commands().Touches().backbufferViewport)
        s.overlay = std::make_unique<NativeOverlayPacket>(actions, device, s.statics);
    s.aliases.resize(actions); s.clears.resize(actions); s.steps.resize(actions); s.working.resize(IniParameterScalars);
    if (!s.textLibrary->stages.empty() && !s.textState.Initialize(device))
        throw std::runtime_error("Native text raster/blend/depth state creation failed");
    D3D11_TEXTURE1D_DESC desc{};
    desc.Width = IniParameterVectors; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = desc.Format; view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D; view.Texture1D.MipLevels = 1;
    // A clear-only native plan does not need any formatter GPU parameters.
    if (!s.library->shaders.empty()) for (auto& dispatch : s.dispatches)
    {
        dispatch.image.resize(IniParameterScalars);
        if (FAILED(device->CreateTexture1D(&desc, nullptr, dispatch.parameters.GetAddressOf())) ||
            FAILED(device->CreateShaderResourceView(dispatch.parameters.Get(), &view, dispatch.parameterView.GetAddressOf())))
            throw std::runtime_error("Native FormatText packet parameter Texture1D/SRV creation failed");
    }
}
NativeComputePacket::~NativeComputePacket() = default;
const Plan* NativeComputePacket::Commands() const noexcept { return state_->library->plan.get(); }
const Dx11AliasChange* NativeComputePacket::Aliases() const noexcept
{ return state_->aliases.data(); }
UINT NativeComputePacket::AliasCount() const noexcept
{ return state_->aliasCount; }
std::uint64_t NativeComputePacket::ReaderEpoch() const noexcept
{ return state_->overlayPrepared ? state_->overlay->Epoch() : 0; }
UINT NativeComputePacket::DispatchCount() const noexcept { return state_->dispatchCount; }
const Dx11RetainedBinding* NativeComputePacket::Clears() const noexcept { return state_->clears.data(); }
UINT NativeComputePacket::ClearCount() const noexcept { return state_->clearCount; }
bool NativeComputePacket::ParameterCommit() const noexcept { return state_->parameterCommit; }
UINT NativeComputePacket::FinalParameters() const noexcept { return state_->finalParameters; }
void NativeComputePacket::Retire() noexcept { state_->ReleaseTransient(); }
NativeComputeExecutor::NativeComputeExecutor(UINT actions, unsigned flights, ID3D11Device* device,
    std::shared_ptr<const Dx11StaticResources> statics, std::shared_ptr<const FormatTextLibrary> library,
    std::shared_ptr<const NativeTextShaderLibrary> textLibrary) : device_(device)
{
    if (!actions || actions > 4096 || !flights || flights > 16 || !device || !statics || !library || !textLibrary ||
        statics->Files()->PlanOwner() != library->plan || textLibrary->plan != library->plan)
        throw std::invalid_argument("Invalid native text preparation owner/capacity");
    flights_.reserve(flights);
    for (unsigned i = 0; i < flights; ++i)
        flights_.push_back(std::shared_ptr<NativeComputePacket>(new NativeComputePacket(actions, device, statics, library, textLibrary)));
}
std::shared_ptr<NativeComputePacket> NativeComputeExecutor::Prepare(ID3D11DeviceContext* context,
    IDXGISwapChain* chain, HWND window, const BatchView& batch, UINT parameterState, const char*& failure, Dx11ResourceProvider* privateTargets, std::uint64_t nativeEpoch, const OptiInput::GameCursorSnapshot* cursor) noexcept
{
    failure = "Native PRE/private-clear POST complete tape preflight failed";
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || !OnDevice(context, device_) ||
        !batch.plan || !batch.actions || batch.includesDrawBoundary) return {};
    std::shared_ptr<NativeComputePacket> packet;
    for (const auto& flight : flights_) if (flight.use_count() == 1) { packet = flight; break; }
    if (!packet) { failure = "Native compute packet capacity busy; no wait/upload/dispatch"; return {}; }
    auto& s = *packet->state_; s.ReleaseTransient();
    struct Abort { NativeComputePacket& packet; bool committed = false;
        ~Abort() { if (!committed) packet.Retire(); } } abort{*packet};
    if (batch.plan != s.library->plan.get() || batch.count > s.aliases.size()) return {};
    s.context = context;
    s.finalParameters = parameterState;
    if (batch.phase == Phase::Post)
    {
        s.postPhase = true;
        Microsoft::WRL::ComPtr<ID3D11Predicate> predicate; BOOL predicateValue = FALSE;
        context->GetPredication(&predicate, &predicateValue);
        if (predicate) { failure = "Conditional private clear cannot certify initialized contents"; return {}; }
        // Actual native return only. Every effect is a private clear; parameter
        // actions select already-preloaded pages and publish after full submit.
        for (UINT i = 0; i < batch.count; ++i)
        {
            const auto& action = batch.actions[i];
            if (action.op == ActionOp::SetParameter)
            {
                UINT next = 0;
                if (!s.statics->WriteParameter(s.finalParameters, action.parameter, action.value, next) ||
                    !s.statics->ParameterView(next)) return {};
                s.finalParameters = next;
            }
            else if (action.op == ActionOp::ClearTarget)
            {
                if (!batch.plan->Touches().clearTargets || !privateTargets || s.clearCount == s.clears.size()) return {};
                auto& kept = s.clears[s.clearCount];
                if (!privateTargets->ResolvePrivateTexture(action.binding, kept) || !kept.ownedTexture ||
                    !SameBinding(kept.value.binding, action.binding) ||
                    kept.game.get() != kept.ownedTexture.get() ||
                    kept.ownedTexture->purpose != OwnedTexturePurpose::OutputDescription ||
                    action.binding.generation != s.statics->DeviceGeneration() ||
                    !ValidatePrivateTexture(*kept.ownedTexture, device_)) return {};
                ++s.clearCount;
            }
            else return {}; // no text/draw/dispatch/alias setter GPU POST extension
        }
        if (FAILED(device_->GetDeviceRemovedReason())) return {};
        s.parameterCommit = true; s.prepared = true; abort.committed = true; failure = nullptr; return packet;
    }
    if (batch.phase != Phase::Pre) return {};
    bool overlayTape = false;
    for (UINT i = 0; i < batch.count; ++i)
        overlayTape |= batch.actions[i].op == ActionOp::DebugOverlay || batch.actions[i].op == ActionOp::BackbufferViewport ||
            batch.actions[i].op == ActionOp::BindOutput || batch.actions[i].op == ActionOp::Bind ||
            batch.actions[i].op == ActionOp::BeginMouse || batch.actions[i].op == ActionOp::EndMouse || batch.actions[i].op == ActionOp::MouseDraw;
    if (overlayTape)
    {
        if (!s.overlay || !s.overlay->BeginPrepare(context, chain, window, nativeEpoch, batch, failure)) return {};
        s.overlayPrepared = true;
        // No early overlay submission/preflight return. The following ONE walk
        // owns aliases, dynamic parameters, text pairs AND every overlay action.
        // The caller cannot reserve or submit any prefix of a rejected tape.
    }
    bool awaitingRender = false;
    if (batch.plan->Touches().iniParameters) for (Id scalar = 0; scalar < IniParameterScalars; ++scalar)
        if (!s.statics->ReadParameter(parameterState, scalar, s.working[scalar])) return {};
    for (UINT i = 0; i < batch.count; ++i)
    {
        const auto& action = batch.actions[i];
        // The fixed original PrintText executes CS then VS/GS/PS with no
        // intervening action. A second effect or alias mutation is not a pair.
        if (awaitingRender && action.op != ActionOp::RenderText)
        {
            if (s.textLibrary && !s.textLibrary->stages.empty()) return {};
            awaitingRender = false; // existing compute-only slice remains admitted
        }
        if (action.op == ActionOp::SetParameter)
        {
            const bool textScalar = !s.library->shaders.empty() &&
                (action.parameter == 0 || action.parameter == 1 || action.parameter == 3);
            const bool overlayScalar = s.overlayPrepared &&
                (action.parameter == 2 || action.parameter == 3 || action.parameter == 70 * 4);
            const bool mouseScalar = s.overlayPrepared && cursor && !batch.plan->SoftwareMouse().empty() && action.parameter < 8;
            const bool finiteScalar = action.parameter >= 8 && action.parameter < IniParameterScalars;
            if ((!finiteScalar && !textScalar && !overlayScalar && !mouseScalar) || !std::isfinite(action.value)) return {};
            if (finiteScalar)
            {
                // This is CPU page selection only. Retain ALL tape-position
                // values, including intermediate writes and the final restore.
                UINT next = 0;
                if (!s.statics->WriteParameter(s.finalParameters, action.parameter, action.value, next) ||
                    !s.statics->ParameterView(next))
                { failure = "Native finite parameter transition is absent from immutable preloaded pages"; return {}; }
                s.finalParameters = next;
            }
            s.working[action.parameter] = action.value;
        }
        else if (action.op == ActionOp::SetResource)
        {
            if (s.aliasCount == s.aliases.size()) return {};
            if (action.destination.kind != TargetKind::Resource || action.destination.index >= s.statics->Files()->Resources().size()) return {};
            auto& alias = s.aliases[s.aliasCount++]; alias = {}; alias.resource = action.destination.index;
            if (!s.Retain(action.previous, privateTargets, alias.previous, action.destination.index) || !s.Retain(action.binding, privateTargets, alias.next)) return {};
        }
        else if (action.op == ActionOp::FormatText)
        {
            if (action.formatText >= s.library->shaders.size() || s.dispatchCount == s.dispatches.size()) return {};
            auto& dispatch = s.dispatches[s.dispatchCount]; dispatch.shader = action.formatText;
            if (!s.library->shaders[dispatch.shader].object || !OnDevice(s.library->shaders[dispatch.shader].object.Get(), device_)) return {};
            for (unsigned n = 0; n < 4; ++n)
                if (!s.statics->Resolve(action.formatBindings[n], dispatch.resources[n])) return {};
            std::copy(s.working.begin(), s.working.end(), dispatch.image.begin());
            if (!s.Views(dispatch)) { failure = "FormatText real view/shape/data/device/parameter contract unavailable"; return {}; }
            s.steps[s.stepCount++] = {ActionOp::FormatText, s.dispatchCount};
            ++s.dispatchCount; awaitingRender = true;
        }
        else if (action.op == ActionOp::RenderText)
        {
            if (!awaitingRender || !s.dispatchCount || s.drawCount == s.dispatches.size() ||
                !s.textLibrary || action.renderText >= s.textLibrary->stages.size()) return {};
            const auto& stage = s.textLibrary->stages[action.renderText];
            if (!stage.vs || !stage.gs || !stage.ps || !OnDevice(stage.vs.Get(), device_) ||
                !OnDevice(stage.gs.Get(), device_) || !OnDevice(stage.ps.Get(), device_)) return {};
            const auto& dispatch = s.dispatches[s.dispatchCount - 1];
            for (unsigned n = 0; n < 4; ++n)
                if (!SameBinding(action.renderBindings[n], dispatch.resources[n].binding)) return {};
            s.drawShaders[s.dispatchCount - 1] = action.renderText;
            s.steps[s.stepCount++] = {ActionOp::RenderText, s.dispatchCount - 1};
            ++s.drawCount; awaitingRender = false;
        }
        else if (s.overlayPrepared && (action.op == ActionOp::BeginMouse || action.op == ActionOp::EndMouse ||
            action.op == ActionOp::MouseDraw || (action.op == ActionOp::Bind &&
            (action.destination.kind == TargetKind::VertexTexture || action.destination.kind == TargetKind::PixelTexture) &&
            (action.destination.index == 100 || action.destination.index == 101))))
        {
            UINT index = 0;
            if (!cursor || !s.overlay->PrepareMouse(action, s.working.data(), *cursor, privateTargets, index, failure)) return {};
            s.steps[s.stepCount++] = {action.op, index};
        }
        else if (s.overlayPrepared && (action.op == ActionOp::BindOutput || action.op == ActionOp::BackbufferViewport ||
            action.op == ActionOp::Bind || action.op == ActionOp::DebugOverlay))
        {
            UINT page = parameterState, index = 0;
            if (action.op == ActionOp::DebugOverlay && !s.ImmutableParameters(parameterState, page))
            { failure = "Native overlay t120 must exactly match the complete tape-position image in immutable pages"; return {}; }
            if (!s.overlay->PrepareAction(action, page, privateTargets, index, failure)) return {};
            s.steps[s.stepCount++] = {action.op, index};
        }
        else return {};
    }
    if ((s.drawCount && (awaitingRender || s.drawCount != s.dispatchCount)) ||
        (awaitingRender && s.textLibrary && !s.textLibrary->stages.empty())) return {};
    // Dynamic text/Mouse/overlay coordinates must still restore. Persistent
    // finite core flags instead publish the exact final full image, after the
    // same complete tape submission and semantic commit as its alias journal.
    if (batch.plan->Touches().iniParameters)
    {
        for (Id scalar = 0; scalar < 8; ++scalar)
        {
            float initial;
            if (!s.statics->ReadParameter(parameterState, scalar, initial) ||
                IniParameterBits(initial) != IniParameterBits(s.working[scalar]))
            { failure = "Native PRE must restore its dynamic IniParams before any GPU effect"; return {}; }
        }
        UINT page = 0;
        if (!s.ImmutableParameters(parameterState, page) || page != s.finalParameters)
        { failure = "Native PRE final full parameter image differs from its finite transaction"; return {}; }
        s.parameterCommit = true;
    }
    if (s.overlayPrepared && !s.overlay->EndPrepare(context, failure)) return {};
    // Whole-entry hazards plus each component's exact resource roles prove
    // cross-segment safety: text's UAV is a fixed structured buffer; overlay
    // inputs are ordered private Texture2D leases and output is the real bb.
    // Text scopes restore OM/PS/CS/GS/UAV/viewport to their tape-position entry.
    if (s.dispatchCount && (!s.saved.Capture(context) || !s.Hazards(context) || FAILED(device_->GetDeviceRemovedReason())))
    { failure = "Native compute real pipeline/hazard/conditional owner unavailable"; return {}; }
    if (s.drawCount && (!s.textState.Prepare(context, chain, device_, window) ||
        (s.overlayPrepared && s.textState.Backbuffer() != s.overlay->Backbuffer())))
    { failure = "Native RenderText real backbuffer/OM/RS/IA/SRV/UAV owner unavailable or differs from overlay"; return {}; }
    s.prepared = true; abort.committed = true; failure = nullptr; return packet;
}
NativeComputeResult NativeComputeExecutor::Submit(ID3D11DeviceContext* context, NativeComputePacket& packet, std::uint64_t nativeEpoch) noexcept
{
    auto& s = *packet.state_;
    if (!s.prepared || s.submitted || context != s.context.Get() || !context || FAILED(device_->GetDeviceRemovedReason()))
        return {false, false, "Native compute submission owner/device changed"};
    s.submitted = true;
    if (s.overlayPrepared && !s.overlay->BeginSubmit(context, nativeEpoch))
        return {false, false, "Native combined tape frame/context/window/backbuffer owner changed"};
    if (s.postPhase)
    {
        bool consumed = false;
        for (UINT i = 0; i < s.clearCount; ++i)
        {
            consumed = true; // BEFORE the void GPU call; never publish on partial failure
            ClearPrivateTexture(context, *s.clears[i].ownedTexture);
            if (FAILED(device_->GetDeviceRemovedReason()))
                return {false, consumed, "Private target clear device loss; submitted owners require quarantine"};
        }
        return {true, consumed, nullptr};
    }
    if (!s.stepCount) return {true, false, nullptr}; // legal null branch, no GPU effects
    bool consumed = false;
    auto restore = [&]() noexcept {
        s.textState.Restore(context);
        if (s.dispatchCount) s.saved.Restore(context);
        if (s.overlayPrepared) s.overlay->Restore(context);
    };
    try
    {
        const UINT keep = ~UINT{0}; const UINT zero[4]{};
        ID3D11UnorderedAccessView* none = nullptr;
        for (UINT i = 0; i < s.stepCount; ++i)
        {
            const auto step = s.steps[i];
            if (step.op == ActionOp::FormatText)
            {
                auto& dispatch = s.dispatches[step.index];
                if (!s.dispatchScope.Capture(context))
                { if (consumed) restore(); return {false, consumed, "Native text CS scope changed; full packet must quarantine"}; }
                struct RestoreDispatch { SavedState& state; ID3D11DeviceContext* context;
                    ~RestoreDispatch() { state.Restore(context); } } scoped{s.dispatchScope, context};
                consumed = true; // BEFORE any void setter/upload/clear/dispatch
                context->VSSetShader(nullptr, nullptr, 0); context->HSSetShader(nullptr, nullptr, 0);
                context->DSSetShader(nullptr, nullptr, 0); context->GSSetShader(nullptr, nullptr, 0);
                context->PSSetShader(nullptr, nullptr, 0);
                context->CSSetUnorderedAccessViews(0, 1, &none, &keep);
                ID3D11ShaderResourceView* views[] = {dispatch.resources[0].srv, dispatch.resources[1].srv,
                    dispatch.resources[2].srv, dispatch.parameterView.Get()};
                for (unsigned n = 0; n < std::size(Slots); ++n) context->CSSetShaderResources(Slots[n], 1, &views[n]);
                context->CSSetShader(s.library->shaders[dispatch.shader].object.Get(), nullptr, 0);
                context->UpdateSubresource(dispatch.parameters.Get(), 0, nullptr, dispatch.image.data(), 0, 0);
                context->ClearUnorderedAccessViewUint(dispatch.resources[3].uav, zero);
                auto* output = dispatch.resources[3].uav;
                context->CSSetUnorderedAccessViews(0, 1, &output, &keep);
                context->Dispatch(1, 1, 1);
                // SAME buffer's actual write precedes GS t112 in the next step.
                // No queue, Wait or old producer fence substitutes for this read.
                context->CSSetUnorderedAccessViews(0, 1, &none, &keep);
            }
            else if (step.op == ActionOp::RenderText)
            {
                const auto& dispatch = s.dispatches[step.index];
                const auto& shaders = s.textLibrary->stages[s.drawShaders[step.index]];
                consumed = true;
                if (!s.textState.Draw(context, shaders, dispatch.resources[0].srv, dispatch.resources[3].srv,
                    dispatch.resources[1].srv, dispatch.resources[2].srv, dispatch.parameterView.Get()))
                { restore(); return {false, consumed, "Native text GS scope changed; full packet must quarantine"}; }
            }
            else
            {
                consumed = true; // overlay Execute can issue its first void setter
                const auto result = s.overlay->Execute(context, step.index);
                if (!result.success) { restore(); return {false, consumed, result.reason}; }
            }
            if (FAILED(device_->GetDeviceRemovedReason()))
            { restore(); return {false, consumed, "Native combined tape device loss; full reader must quarantine"}; }
        }
        if (consumed) restore();
        if (FAILED(device_->GetDeviceRemovedReason()))
            return {false, consumed, "Native combined tape restore device loss; full reader must quarantine"};
        // Only now may the existing Session mark this ONE reservation Unsealed.
        // Its next actual Signal is after the FINAL text/overlay reader/restores.
        return {true, consumed, nullptr};
    }
    catch (...)
    {
        if (consumed) restore();
        return {false, consumed, "Native combined tape partial failure; full reservation must quarantine"};
    }
}
} // namespace gimi
