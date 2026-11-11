#include "GimiDebugOverlay.h"
#include "third_party/texfx-overlay/DebugShaderOutputSource.h"
#include "third_party/mouse/MouseSource.h"
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <fstream>
#include <stdexcept>

namespace gimi
{
namespace
{
using Microsoft::WRL::ComPtr;
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
            if (++i == source.size() || source[i] != char(10)) return false;
        normalized.push_back(source[i]);
    }
    if (std::string_view(normalized) != detail::FixedDebugShaderOutput) return false;
    total += source.size(); return true;
}
bool ReflectedStage(ID3DBlob& bytes, bool pixel) noexcept
{
    ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(bytes.GetBufferPointer(), bytes.GetBufferSize(), IID_PPV_ARGS(&reflection)))) return false;
    D3D11_SHADER_DESC desc{};
    if (FAILED(reflection->GetDesc(&desc)) || reflection->GetNumInterfaceSlots() ||
        D3D11_SHVER_GET_TYPE(desc.Version) != (pixel ? D3D11_SHVER_PIXEL_SHADER : D3D11_SHVER_VERTEX_SHADER) ||
        D3D11_SHVER_GET_MAJOR(desc.Version) != 5 || D3D11_SHVER_GET_MINOR(desc.Version) || desc.ConstantBuffers) return false;
    if (!pixel)
    {
        D3D11_SIGNATURE_PARAMETER_DESC input{};
        return !desc.BoundResources && desc.InputParameters == 1 &&
            SUCCEEDED(reflection->GetInputParameterDesc(0, &input)) && input.SystemValueType == D3D_NAME_VERTEX_ID &&
            input.ComponentType == D3D_REGISTER_COMPONENT_UINT32 && input.Mask == 1;
    }
    unsigned seen = 0;
    for (UINT i = 0; i < desc.BoundResources; ++i)
    {
        D3D11_SHADER_INPUT_BIND_DESC binding{};
        if (FAILED(reflection->GetResourceBindingDesc(i, &binding)) || binding.BindCount != 1) return false;
        unsigned bit = 0;
        if (binding.Type == D3D_SIT_SAMPLER && binding.BindPoint == 0) bit = 1;
        if (binding.Type == D3D_SIT_TEXTURE && binding.Dimension == D3D_SRV_DIMENSION_TEXTURE2D &&
            binding.ReturnType == D3D_RETURN_TYPE_FLOAT && binding.BindPoint >= 20 && binding.BindPoint <= 23)
            bit = 2u << (binding.BindPoint - 20);
        if (binding.Type == D3D_SIT_TEXTURE && binding.BindPoint == IniParameterRegister &&
            binding.Dimension == D3D_SRV_DIMENSION_TEXTURE1D && binding.ReturnType == D3D_RETURN_TYPE_FLOAT) bit = 32;
        if (!bit || (seen & bit)) return false;
        seen |= bit;
    }
    D3D11_SIGNATURE_PARAMETER_DESC output{};
    return seen == 63 && desc.OutputParameters == 1 &&
        SUCCEEDED(reflection->GetOutputParameterDesc(0, &output)) && output.SystemValueType == D3D_NAME_TARGET &&
        output.SemanticIndex == 0 && output.Register == 0 && output.Mask == 15 &&
        output.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32;
}
bool CompileStage(const std::string& filename, bool pixel, ComPtr<ID3DBlob>& bytes) noexcept
{
    const D3D_SHADER_MACRO macros[] = {{pixel ? "PIXEL_SHADER" : "VERTEX_SHADER", ""}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> errors;
    return SUCCEEDED(D3DCompile(detail::FixedDebugShaderOutput.data(), detail::FixedDebugShaderOutput.size(),
        filename.c_str(), macros, nullptr, "main", pixel ? "ps_5_0" : "vs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, bytes.GetAddressOf(), errors.GetAddressOf())) && bytes &&
        ReflectedStage(*bytes.Get(), pixel);
}

bool MouseSource(const std::filesystem::path& path, std::size_t& total)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto bytes = file.tellg();
    if (bytes <= 0 || bytes > 1024 * 1024 || total > 8u * 1024u * 1024u - static_cast<std::size_t>(bytes)) return false;
    std::string text(static_cast<std::size_t>(bytes), char{}); file.seekg(0);
    if (!file.read(text.data(), static_cast<std::streamsize>(text.size())) || file.peek() != std::char_traits<char>::eof()) return false;
    std::string normalized; normalized.reserve(text.size());
    for (std::size_t n = 0; n < text.size(); ++n)
    {
        if (text[n] == '\r') if (++n == text.size() || text[n] != '\n') return false;
        normalized.push_back(text[n]);
    }
    // Raw literal has a single framing newline, not part of the source.
    if (std::string_view(normalized) != detail::FixedMouseSource.substr(1)) return false;
    total += text.size(); return true;
}
bool MouseReflection(ID3DBlob& bytes, bool pixel) noexcept
{
    ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(bytes.GetBufferPointer(), bytes.GetBufferSize(), IID_PPV_ARGS(&reflection)))) return false;
    D3D11_SHADER_DESC desc{};
    if (FAILED(reflection->GetDesc(&desc)) || reflection->GetNumInterfaceSlots() || desc.ConstantBuffers ||
        D3D11_SHVER_GET_TYPE(desc.Version) != (pixel ? D3D11_SHVER_PIXEL_SHADER : D3D11_SHVER_VERTEX_SHADER) ||
        D3D11_SHVER_GET_MAJOR(desc.Version) != 5 || D3D11_SHVER_GET_MINOR(desc.Version)) return false;
    unsigned seen = 0;
    for (UINT n = 0; n < desc.BoundResources; ++n)
    {
        D3D11_SHADER_INPUT_BIND_DESC input{};
        if (FAILED(reflection->GetResourceBindingDesc(n, &input)) || input.Type != D3D_SIT_TEXTURE ||
            input.ReturnType != D3D_RETURN_TYPE_FLOAT || input.BindCount != 1) return false;
        unsigned bit = 0;
        if (input.Dimension == D3D_SRV_DIMENSION_TEXTURE2D && input.BindPoint >= 100 && input.BindPoint <= 101)
            bit = 1u << (input.BindPoint - 100);
        if (!pixel && input.BindPoint == IniParameterRegister && input.Dimension == D3D_SRV_DIMENSION_TEXTURE1D) bit = 4;
        if (!bit || (seen & bit)) return false;
        seen |= bit;
    }
    if (seen != (pixel ? 3u : 7u)) return false;
    if (!pixel)
    {
        D3D11_SIGNATURE_PARAMETER_DESC input{};
        if (desc.InputParameters != 1 || desc.OutputParameters != 2 ||
            FAILED(reflection->GetInputParameterDesc(0, &input)) || input.SystemValueType != D3D_NAME_VERTEX_ID ||
            input.ComponentType != D3D_REGISTER_COMPONENT_UINT32 || input.Mask != 1) return false;
    }
    else
    {
        D3D11_SIGNATURE_PARAMETER_DESC output{};
        if (desc.InputParameters < 1 || desc.InputParameters > 2 || desc.OutputParameters != 1 ||
            FAILED(reflection->GetOutputParameterDesc(0, &output)) || output.SystemValueType != D3D_NAME_TARGET ||
            output.SemanticIndex || output.Register || output.Mask != 15 || output.ComponentType != D3D_REGISTER_COMPONENT_FLOAT32) return false;
    }
    // FXC can omit the unused PS SV_Position input. The texcoord link stays
    // exact; source authentication already fixes its computation in the VS.
    bool texcoord = false;
    for (UINT n = 0; n < (pixel ? desc.InputParameters : desc.OutputParameters); ++n)
    {
        D3D11_SIGNATURE_PARAMETER_DESC signature{};
        if (FAILED(pixel ? reflection->GetInputParameterDesc(n, &signature) : reflection->GetOutputParameterDesc(n, &signature)) ||
            signature.ComponentType != D3D_REGISTER_COMPONENT_FLOAT32 || signature.SemanticIndex) return false;
        if (signature.SystemValueType == D3D_NAME_POSITION && signature.Mask == 15) continue;
        if (signature.SystemValueType != D3D_NAME_UNDEFINED || signature.Mask != 3 ||
            !signature.SemanticName || std::string_view(signature.SemanticName) != "TEXCOORD" || texcoord) return false;
        texcoord = true;
    }
    return texcoord;
}
bool MouseStage(const std::string& filename, bool pixel, ComPtr<ID3DBlob>& bytes) noexcept
{
    const D3D_SHADER_MACRO macros[] = {{pixel ? "PIXEL_SHADER" : "VERTEX_SHADER", ""}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> errors; const auto text = detail::FixedMouseSource.substr(1);
    return SUCCEEDED(D3DCompile(text.data(), text.size(), filename.c_str(), macros, nullptr, "main",
        pixel ? "ps_5_0" : "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytes, &errors)) && bytes && MouseReflection(*bytes.Get(), pixel);
}
} // namespace

std::shared_ptr<const DebugOverlayLibrary> CreateDebugOverlayLibrary(
    std::shared_ptr<const Plan> plan, ID3D11Device* device, std::vector<Diagnostic>& diagnostics)
{
    try
    {
        if (!plan || !device || device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
            throw std::runtime_error("DebugShaderOutput needs a real FL11 device");
        auto library = std::make_shared<DebugOverlayLibrary>(); library->plan = std::move(plan);
        library->stages.resize(library->plan->DebugOverlays().size());
        std::size_t total = 0;
        for (Id id = 0; id < library->stages.size(); ++id)
        {
            const auto& spec = library->plan->DebugOverlays()[id];
            auto fail = [&](const char* reason) { diagnostics.push_back({DiagnosticCode::Unsupported, spec.source, reason}); };
            if (!OriginalSource(spec.shader, total))
            { fail("DebugShaderOutput source differs from the fixed original or exceeds static budget"); return {}; }
            ComPtr<ID3DBlob> vs, ps; const auto filename = PathUtf8(spec.shader);
            if (!CompileStage(filename, false, vs) || !CompileStage(filename, true, ps))
            { fail("DebugShaderOutput original VS/PS compile or reflected slot ABI refused"); return {}; }
            auto& stage = library->stages[id];
            if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &stage.vs)) ||
                FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &stage.ps)))
            { fail("DebugShaderOutput real VS/PS creation failed"); return {}; }
        }
        library->mouseStages.resize(library->plan->SoftwareMouse().size());
        for (Id id = 0; id < library->mouseStages.size(); ++id)
        {
            const auto& spec = library->plan->SoftwareMouse()[id];
            if (!MouseSource(spec.shader, total))
            { diagnostics.push_back({DiagnosticCode::Unsupported, spec.source, "Mouse source differs from original or exceeds static budget"}); return {}; }
            ComPtr<ID3DBlob> vs, ps; const auto filename = PathUtf8(spec.shader);
            if (!MouseStage(filename, false, vs) || !MouseStage(filename, true, ps))
            { diagnostics.push_back({DiagnosticCode::Unsupported, spec.source, "Mouse original VS/PS compile/reflection failed"}); return {}; }
            auto& stages = library->mouseStages[id];
            if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &stages.vs)) ||
                FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &stages.ps)))
                throw std::runtime_error("Mouse real shader creation failed");
        }
        if (!library->mouseStages.empty())
        {
            D3D11_RASTERIZER_DESC raster{}; raster.FillMode = D3D11_FILL_SOLID;
            raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
            if (FAILED(device->CreateRasterizerState(&raster, &library->mouseRaster)))
                throw std::runtime_error("Mouse raster creation failed");
            for (unsigned n = 0; n < 2; ++n)
            {
                D3D11_BLEND_DESC blend{}; auto& rt = blend.RenderTarget[0]; rt.BlendEnable = TRUE;
                rt.SrcBlend = n ? D3D11_BLEND_INV_DEST_COLOR : D3D11_BLEND_SRC_ALPHA;
                rt.DestBlend = n ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_INV_SRC_ALPHA;
                rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
                rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_ZERO;
                rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
                for (unsigned i = 1; i < 8; ++i) blend.RenderTarget[i] = rt;
                if (FAILED(device->CreateBlendState(&blend, &library->mouseBlend[n])))
                    throw std::runtime_error("Mouse blend creation failed");
            }
        }
        if (!library->stages.empty())
        {
            // Fixed XXMI ParseRSState defaults; no state_merge property.
            D3D11_RASTERIZER_DESC raster{}; raster.FillMode = D3D11_FILL_SOLID;
            raster.CullMode = D3D11_CULL_BACK; raster.DepthClipEnable = TRUE;
            if (FAILED(device->CreateRasterizerState(&raster, &library->raster)))
                throw std::runtime_error("DebugShaderOutput raster state creation failed");
            D3D11_BLEND_DESC blend{};
            auto& rt = blend.RenderTarget[0]; rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D11_BLEND_SRC_ALPHA; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_ZERO; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            for (unsigned i = 1; i < 8; ++i) blend.RenderTarget[i] = rt;
            if (FAILED(device->CreateBlendState(&blend, &library->blend)))
                throw std::runtime_error("DebugShaderOutput blend state creation failed");
        }
        return library;
    }
    catch (const std::exception& e)
    { diagnostics.push_back({DiagnosticCode::Unsupported, {}, e.what()}); return {}; }
}
} // namespace gimi
