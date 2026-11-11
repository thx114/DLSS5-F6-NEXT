#include "GimiShaderSource.h"
#include <cstring>

namespace gimi
{
namespace
{
bool ValidIdentity(ShaderIdentity identity) noexcept
{
    return identity.knowledge == Knowledge::Known && identity.object && identity.generation &&
        identity.object != UINT64_MAX && identity.generation != UINT64_MAX;
}
ShaderModel ReflectedModel(UINT version) noexcept
{
    const auto stage = D3D11_SHVER_GET_TYPE(version);
    const auto major = D3D11_SHVER_GET_MAJOR(version), minor = D3D11_SHVER_GET_MINOR(version);
    const bool vs = stage == D3D11_SHVER_VERTEX_SHADER;
    if (!vs && stage != D3D11_SHVER_PIXEL_SHADER) return ShaderModel::Unknown;
    if (major == 4 && minor == 0) return vs ? ShaderModel::VS40 : ShaderModel::PS40;
    if (major == 4 && minor == 1) return vs ? ShaderModel::VS41 : ShaderModel::PS41;
    if (major == 5 && minor == 0) return vs ? ShaderModel::VS50 : ShaderModel::PS50;
    if (major == 5 && minor == 1) return vs ? ShaderModel::VS51 : ShaderModel::PS51;
    return ShaderModel::Unknown;
}
bool ReadParameter(const D3D11_SIGNATURE_PARAMETER_DESC& desc, ShaderSignatureParameter& out) noexcept
{
    if (!desc.SemanticName || !desc.Mask || (desc.Mask & ~15u) || (desc.ReadWriteMask & ~15u)) return false;
    std::size_t n = 0;
    for (; n != out.semantic.size(); ++n) if (!desc.SemanticName[n]) break;
    if (!n || n == out.semantic.size()) return false;
    std::memcpy(out.semantic.data(), desc.SemanticName, n);
    out.semantic[n] = 0;
    out.semanticIndex = desc.SemanticIndex; out.registerIndex = desc.Register;
    out.stream = desc.Stream; out.systemValue = desc.SystemValueType; out.component = desc.ComponentType;
    out.mask = desc.Mask; out.readWriteMask = desc.ReadWriteMask; out.precision = desc.MinPrecision;
    // System outputs (depth/coverage) may legitimately have Register=UINT_MAX.
    // Preserve the real descriptor; never infer its meaning from a slot number.
    return true;
}
bool ReadSignature(ID3D11ShaderReflection& reflection, const D3D11_SHADER_DESC& desc,
                   ShaderSignature& out) noexcept
{
    if (desc.InputParameters > out.inputs.size() || desc.OutputParameters > out.outputs.size() ||
        desc.PatchConstantParameters) return false; // no HS/DS contract in this slice
    for (UINT i = 0; i < desc.InputParameters; ++i)
    {
        D3D11_SIGNATURE_PARAMETER_DESC parameter{};
        if (FAILED(reflection.GetInputParameterDesc(i, &parameter)) || !ReadParameter(parameter, out.inputs[i])) return false;
    }
    for (UINT i = 0; i < desc.OutputParameters; ++i)
    {
        D3D11_SIGNATURE_PARAMETER_DESC parameter{};
        if (FAILED(reflection.GetOutputParameterDesc(i, &parameter)) || !ReadParameter(parameter, out.outputs[i])) return false;
    }
    out.inputCount = desc.InputParameters; out.outputCount = desc.OutputParameters;
    return true;
}
bool SameParameter(const ShaderSignatureParameter& a, const ShaderSignatureParameter& b) noexcept
{
    return a.semantic == b.semantic && a.semanticIndex == b.semanticIndex && a.registerIndex == b.registerIndex &&
        a.stream == b.stream && a.systemValue == b.systemValue && a.component == b.component &&
        a.mask == b.mask && a.readWriteMask == b.readWriteMask && a.precision == b.precision;
}
} // namespace
ShaderSourceResult CaptureShaderSource(ShaderIdentity original, ShaderModel model, const void* bytes, std::size_t size,
    ID3D11ShaderReflection& reflection, const D3D11_SHADER_DESC& desc, std::size_t available) noexcept
{
    if (!ValidIdentity(original)) return {{}, ShaderSourceStatus::InvalidIdentity};
    if (model == ShaderModel::Unknown || ReflectedModel(desc.Version) != model || reflection.GetNumInterfaceSlots())
        return {{}, ShaderSourceStatus::UnsupportedModel};
    if (!bytes || !size || size > GimiShaderByteLimit) return {{}, ShaderSourceStatus::SizeLimit};
    if (available < sizeof(ShaderSource) || size > available - sizeof(ShaderSource))
        return {{}, ShaderSourceStatus::BudgetLimit};
    try
    {
        auto source = std::make_shared<ShaderSource>();
        if (!ReadSignature(reflection, desc, source->signature_)) return {{}, ShaderSourceStatus::InvalidSignature};
        source->original_ = original; source->model_ = model;
        const auto* first = static_cast<const std::uint8_t*>(bytes);
        source->bytes_.assign(first, first + size); // ONE creation-time immutable copy
        if (source->PayloadBytes() > available) return {{}, ShaderSourceStatus::BudgetLimit};
        return {std::move(source), ShaderSourceStatus::Ready};
    }
    catch (...) { return {{}, ShaderSourceStatus::AllocationFailure}; }
}
bool MatchesOriginalShaderSignature(const ShaderSource& source, ID3D11ShaderReflection& reflection) noexcept
{
    D3D11_SHADER_DESC desc{};
    if (FAILED(reflection.GetDesc(&desc)) || ReflectedModel(desc.Version) != source.Model() || reflection.GetNumInterfaceSlots()) return false;
    ShaderSignature actual;
    const auto& expected = source.Signature();
    if (!ReadSignature(reflection, desc, actual) || actual.inputCount != expected.inputCount || actual.outputCount != expected.outputCount) return false;
    for (UINT i = 0; i < actual.inputCount; ++i) if (!SameParameter(actual.inputs[i], expected.inputs[i])) return false;
    for (UINT i = 0; i < actual.outputCount; ++i) if (!SameParameter(actual.outputs[i], expected.outputs[i])) return false;
    return true;
}
bool PreparePixelTarget6Signature(const ShaderSource& source, PixelTarget6Signature& out) noexcept
{
    out = {};
    if (source.Model() != ShaderModel::PS50 || !ValidIdentity(source.Original()) || source.Bytes().empty()) return false;
    const auto& signature = source.Signature();
    if (signature.outputCount >= signature.outputs.size()) return false;
    for (UINT i = 0; i < signature.outputCount; ++i)
    {
        const auto& parameter = signature.outputs[i];
        if (parameter.registerIndex == 6 ||
            (parameter.systemValue == D3D_NAME_TARGET && parameter.semanticIndex == 6) ||
            (!_stricmp(parameter.semantic.data(), "SV_Target") && parameter.semanticIndex == 6)) return false;
    }
    out.original = source.Original(); out.expected = signature;
    auto& added = out.expected.outputs[out.expected.outputCount++];
    added = {};
    std::memcpy(added.semantic.data(), "SV_Target", sizeof("SV_Target"));
    added.semanticIndex = 6; added.registerIndex = 6;
    added.systemValue = D3D_NAME_TARGET; added.component = D3D_REGISTER_COMPONENT_FLOAT32;
    added.mask = 15; added.readWriteMask = 0; // output: no declared component is never-written
    added.precision = D3D_MIN_PRECISION_DEFAULT; added.stream = 0;
    out.valid = true;
    return true;
}
bool MatchesPixelTarget6Signature(const PixelTarget6Signature& contract, ID3D11ShaderReflection& reflection) noexcept
{
    if (!contract.valid || !ValidIdentity(contract.original) || contract.expected.inputCount > GimiSignatureCapacity ||
        !contract.expected.outputCount || contract.expected.outputCount > GimiSignatureCapacity) return false;
    D3D11_SHADER_DESC desc{};
    if (FAILED(reflection.GetDesc(&desc)) || ReflectedModel(desc.Version) != ShaderModel::PS50 || reflection.GetNumInterfaceSlots()) return false;
    ShaderSignature actual;
    if (!ReadSignature(reflection, desc, actual) || actual.inputCount != contract.expected.inputCount ||
        actual.outputCount != contract.expected.outputCount) return false;
    for (UINT i = 0; i < actual.inputCount; ++i)
        if (!SameParameter(actual.inputs[i], contract.expected.inputs[i])) return false;
    for (UINT i = 0; i < actual.outputCount; ++i)
        if (!SameParameter(actual.outputs[i], contract.expected.outputs[i])) return false;
    return true;
}
} // namespace gimi
