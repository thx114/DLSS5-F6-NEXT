#include "GimiDx11Executor.h"
#include "GimiShaderTools.h"

#include <limits>
#include <stdexcept>
#include <vector>

namespace gimi
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr std::uint32_t NoResource = UINT32_MAX;
constexpr unsigned VertexSlots = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
constexpr unsigned PixelSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
constexpr unsigned UavSlots11_1 = 64;
constexpr UINT WriteBindings = D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL |
                               D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_STREAM_OUTPUT;
constexpr UINT SharedResources = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
                                 D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

bool SameBinding(Binding a, Binding b) noexcept
{
    return a.object == b.object && a.view == b.view && a.generation == b.generation &&
        a.stride == b.stride && a.offset == b.offset && a.format == b.format && a.byteSize == b.byteSize;
}
bool BufferDescriptor(const D3D11_BUFFER_DESC& a, const D3D11_BUFFER_DESC& b) noexcept
{
    return a.ByteWidth == b.ByteWidth && a.Usage == b.Usage && a.BindFlags == b.BindFlags &&
        a.CPUAccessFlags == b.CPUAccessFlags && a.MiscFlags == b.MiscFlags && a.StructureByteStride == b.StructureByteStride;
}
bool TextureDescriptor(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b) noexcept
{
    return a.Width == b.Width && a.Height == b.Height && a.MipLevels == b.MipLevels && a.ArraySize == b.ArraySize &&
        a.Format == b.Format && a.SampleDesc.Count == b.SampleDesc.Count && a.SampleDesc.Quality == b.SampleDesc.Quality &&
        a.Usage == b.Usage && a.BindFlags == b.BindFlags && a.CPUAccessFlags == b.CPUAccessFlags && a.MiscFlags == b.MiscFlags;
}
std::uint64_t BindingKey(Binding b) noexcept
{
    // Numeric lease key only. No resource bytes, shader bytecode or file IO.
    std::uint64_t h = b.object ^ (b.view * 0x9e3779b97f4a7c15ull);
    const auto mix = [&h](std::uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
    mix(b.generation); mix(std::uint64_t(b.stride) | (std::uint64_t(b.offset) << 32));
    mix(std::uint64_t(b.format) | (std::uint64_t(b.byteSize) << 32));
    return h;
}
template<class T> bool OnDevice(T* object, ID3D11Device* device) noexcept
{
    if (!object) return false;
    ComPtr<ID3D11Device> actual;
    object->GetDevice(actual.GetAddressOf());
    return actual.Get() == device;
}
bool Bit(const std::array<std::uint64_t, 2>& mask, unsigned slot) noexcept
{
    return (mask[slot / 64] & (std::uint64_t(1) << (slot % 64))) != 0;
}
void SetBit(std::array<std::uint64_t, 2>& mask, unsigned slot) noexcept
{
    mask[slot / 64] |= std::uint64_t(1) << (slot % 64);
}
struct SavedState
{
    std::array<ComPtr<ID3D11Buffer>, VertexSlots> vertices;
    std::array<UINT, VertexSlots> strides{}, offsets{};
    ComPtr<ID3D11Buffer> index;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
    UINT indexOffset = 0;
    std::array<ComPtr<ID3D11ShaderResourceView>, PixelSlots> pixels, vertexTextures;
    std::array<ComPtr<ID3D11Buffer>, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> vertexConstants;
    std::array<UINT, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> constantFirst{}, constantCount{};
    ComPtr<ID3D11DeviceContext1> constantRanges;
    std::uint32_t constantCaptured = 0, constantChanged = 0;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11GeometryShader> geometryShader;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    std::uint32_t vertexCaptured = 0, vertexChanged = 0;
    std::array<std::uint64_t, 2> pixelCaptured{}, pixelChanged{};
    std::array<std::uint64_t, 2> vertexTextureCaptured{}, vertexTextureChanged{};
    bool indexCaptured = false, indexChanged = false;
    ComPtr<ID3D11ShaderResourceView> iniVertex;
    bool iniVertexChanged = false, pixelShaderChanged = false, vertexShaderChanged = false;
    ComPtr<ID3D11SamplerState> outlineSampler;
    D3D11_SAMPLER_DESC outlineSamplerDesc{};
    bool outlineSamplerChanged = false;
    ComPtr<ID3D11RasterizerState> raster;
    bool rasterChanged = false;

    void Clear() noexcept
    {
        for (unsigned n = 0; n < VertexSlots; ++n) if (vertexCaptured & (1u << n)) vertices[n].Reset();
        for (unsigned n = 0; n < PixelSlots; ++n)
        {
            if (Bit(pixelCaptured, n)) pixels[n].Reset();
            if (Bit(vertexTextureCaptured, n)) vertexTextures[n].Reset();
        }
        for (unsigned n = 0; n < vertexConstants.size(); ++n)
            if (constantCaptured & (1u << n)) vertexConstants[n].Reset();
        constantRanges.Reset(); constantFirst = {}; constantCount = {}; constantCaptured = constantChanged = 0;
        index.Reset(); layout.Reset(); vertexShader.Reset(); pixelShader.Reset(); geometryShader.Reset();
        vertexCaptured = vertexChanged = 0; pixelCaptured = {}; pixelChanged = {};
        vertexTextureCaptured = {}; vertexTextureChanged = {};
        indexCaptured = indexChanged = false;
        iniVertex.Reset(); iniVertexChanged = pixelShaderChanged = vertexShaderChanged = false;
        outlineSampler.Reset(); outlineSamplerDesc = {}; outlineSamplerChanged = false;
        raster.Reset(); rasterChanged = false;
    }
    void Restore(ID3D11DeviceContext* context) const noexcept
    {
        // Only captured setters are restored. Original PS has no class
        // instances (preflight proved it). Storage restores any changed OM
        // snapshot before read slots; the typed VS is paired back to the exact
        // original shader. Original layout/topology remain the same objects.
        // No broad ClearState or hidden UAV conflict repair erases game state.
        for (unsigned n = 0; n < vertexConstants.size(); ++n) if (constantChanged & (1u << n))
        {
            auto* buffer = vertexConstants[n].Get();
            // Base API restoration would discard an actual D3D11.1 subrange.
            if (constantRanges && buffer)
                constantRanges->VSSetConstantBuffers1(n, 1, &buffer, &constantFirst[n], &constantCount[n]);
            else context->VSSetConstantBuffers(n, 1, &buffer);
        }
        if (rasterChanged) context->RSSetState(raster.Get());
        if (pixelShaderChanged) context->PSSetShader(pixelShader.Get(), nullptr, 0);
        if (vertexShaderChanged) context->VSSetShader(vertexShader.Get(), nullptr, 0);
        if (outlineSamplerChanged)
        {
            auto* sampler = outlineSampler.Get(); context->PSSetSamplers(15, 1, &sampler);
        }
        for (unsigned n = 0; n < VertexSlots; ++n) if (vertexChanged & (1u << n))
        {
            auto* buffer = vertices[n].Get();
            context->IASetVertexBuffers(n, 1, &buffer, &strides[n], &offsets[n]);
        }
        if (indexChanged) context->IASetIndexBuffer(index.Get(), indexFormat, indexOffset);
        if (iniVertexChanged)
        {
            auto* srv = iniVertex.Get(); context->VSSetShaderResources(IniParameterRegister, 1, &srv);
        }
        for (unsigned n = 0; n < PixelSlots; ++n)
        {
            if (Bit(vertexTextureChanged, n))
            {
                auto* srv = vertexTextures[n].Get(); context->VSSetShaderResources(n, 1, &srv);
            }
            if (Bit(pixelChanged, n))
            {
                auto* srv = pixels[n].Get(); context->PSSetShaderResources(n, 1, &srv);
            }
        }
    }
};
// Original reflected VS inputs plus the final typed rewrite's exact CB
// readers are captured. Reserved IniParams is separate and originally unused.
// This capture retains ownership, never copies dynamic game input contents.
struct VertexInputRetention
{
    std::array<ComPtr<ID3D11Buffer>, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> constants;
    std::array<ComPtr<ID3D11ShaderResourceView>, PixelSlots> views;
    std::array<ComPtr<ID3D11SamplerState>, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplers;
    std::array<UINT, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> first{}, count{};
    std::array<Dx11VertexReadLease, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT + PixelSlots> leases;
    UINT constantMask = 0, samplerMask = 0;
    std::array<std::uint64_t, 2> viewMask{};
    void Clear() noexcept
    {
        for (UINT slot = 0; slot < constants.size(); ++slot) if (constantMask & (1u << slot))
        { constants[slot].Reset(); leases[slot] = {}; }
        for (UINT slot = 0; slot < views.size(); ++slot) if (Bit(viewMask, slot))
        { views[slot].Reset(); leases[constants.size() + slot] = {}; }
        for (UINT slot = 0; slot < samplers.size(); ++slot) if (samplerMask & (1u << slot)) samplers[slot].Reset();
        constantMask = samplerMask = 0; viewMask = {};
    }
};
// This slice accepts only explicitly typed uncompressed return families. All
// other formats remain Unsupported instead of inferring their shader ABI.
bool VertexReturnFormat(DXGI_FORMAT format, D3D_RESOURCE_RETURN_TYPE returns) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8_UINT:
        return returns == D3D_RETURN_TYPE_UINT;
    case DXGI_FORMAT_R32G32B32A32_SINT: case DXGI_FORMAT_R32G32B32_SINT:
    case DXGI_FORMAT_R32G32_SINT: case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_R8G8B8A8_SINT: case DXGI_FORMAT_R8G8_SINT: case DXGI_FORMAT_R8_SINT:
        return returns == D3D_RETURN_TYPE_SINT;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16_FLOAT:
        return returns == D3D_RETURN_TYPE_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16_UNORM:
        return returns == D3D_RETURN_TYPE_FLOAT || returns == D3D_RETURN_TYPE_UNORM;
    case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16_SNORM:
        return returns == D3D_RETURN_TYPE_FLOAT || returns == D3D_RETURN_TYPE_SNORM;
    default: return false;
    }
}
bool StandardSampleFilter(D3D11_FILTER filter) noexcept
{
    switch (filter)
    {
    case D3D11_FILTER_MIN_MAG_MIP_POINT: case D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR:
    case D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT: case D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR:
    case D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT: case D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
    case D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT: case D3D11_FILTER_MIN_MAG_MIP_LINEAR:
    case D3D11_FILTER_ANISOTROPIC: return true;
    default: return false; // mode_default cannot infer a comparison/reduction ABI
    }
}
struct PreparedAction
{
    Action action;
    bool skipRaster = false; // XXMI auto with an empty IB emits no physical draw
    std::uint32_t resource = NoResource;
    std::uint32_t parameters = 0;
    std::array<std::uint32_t, 4> overlayResources{}; // owning packet indices, including known-null
    std::array<std::uint64_t, D3D11_SO_BUFFER_SLOT_COUNT> streamBytes{};
    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> outputs{};
    ID3D11RasterizerState* raster = nullptr; // owned by static resources or saved entry
    ID3D11DepthStencilView* depth = nullptr; // all views owned by original snapshot or packet resources
};
struct ResolutionCache
{
    Binding binding;
    std::uint64_t stamp = 0;
    std::uint32_t resource = NoResource;
};
} // namespace

struct Dx11ExecutionLease::Storage
{
    std::shared_ptr<const Plan> plan;
    std::shared_ptr<const Dx11StaticResources> parameters, rasterOwner;
    std::uint32_t initialParameters = 0, finalParameters = 0;
    Dx11PipelineBinding pipeline;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    SavedState saved;
    VertexInputRetention vertexInputs, geometryInputs;
    // Actual call-site state retained from whole-tape preflight through this
    // packet's final GPU reader fence. No synthetic s0/default sampler object.
    std::shared_ptr<const DebugOverlayLibrary> overlayLibrary;
    ComPtr<ID3D11SamplerState> overlaySampler;
    ComPtr<ID3D11ComputeShader> overlayCompute;
    ComPtr<ID3D11BlendState> overlayBlend;
    std::array<FLOAT, 4> overlayBlendFactor{};
    UINT overlaySampleMask = 0;
    bool overlayCaptured = false;
    Dx11RetainedBinding queriedIndex; // descriptor-only owner, never in bindable resources
    std::array<Dx11OutputBinding, 9> queriedOutputs{};
    std::array<ComPtr<ID3D11RenderTargetView>, 8> originalOutputs;
    ComPtr<ID3D11DepthStencilView> originalDepth;
    std::array<Dx11OutputBinding, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT + 1> simulatedOutputs{};
    bool outputsChanged = false, outputTape = false;
    struct RasterScope { std::array<Dx11OutputBinding, 9> outputs; ID3D11RasterizerState* raster = nullptr; };
    std::vector<RasterScope> rasterScopes; // constructor allocated, bounded by tape capacity
    unsigned rasterDepth = 0;
    ID3D11RasterizerState* simulatedRaster = nullptr;
    std::uint32_t simulatedConstantNulls = 0;
    Id failedVertexConstant = InvalidId;
    const char* vertexConstantReason = nullptr;
    // Real original PS samplers used by a command-bound DDS consumer. Commands
    // cannot alter them; retain without redundant per-raster sampler setters.
    std::array<ComPtr<ID3D11SamplerState>, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> pixelSamplers;
    std::uint32_t pixelSamplerMask = 0;
    bool pixelSamplersCaptured = false;
    Dx11StreamOutputBinding streams;
    std::array<std::uint64_t, D3D11_SO_BUFFER_SLOT_COUNT> streamPlanned{}, streamIssued{};
    std::vector<PreparedAction> tape;
    std::vector<Dx11RetainedBinding> resources;
    std::vector<Dx11AliasChange> aliases;
    std::vector<ResolutionCache> lookup;
    std::array<std::uint32_t, DepthCopyCapacity> preparedDepth{};
    std::array<bool, DepthCopyCapacity> copiedDepth{};
    unsigned depthCount = 0;
    std::array<Binding, PrivateTextureCapacity> textureWrites{};
    unsigned textureWriteCount = 0;
    bool TextureReady(std::uint32_t index) const noexcept
    {
        const auto& retained = resources[index];
        if (!retained.ownedTexture || retained.ownedTexture->ordered) return true;
        for (unsigned i = 0; i < textureWriteCount; ++i)
            if (SameBinding(textureWrites[i], retained.ownedTexture->binding)) return true;
        return false;
    }
    bool WroteTexture(std::uint32_t index) noexcept
    {
        auto& retained = resources[index];
        for (unsigned i = 0; i < textureWriteCount; ++i)
            if (SameBinding(textureWrites[i], retained.value.binding)) return true;
        if (textureWriteCount == textureWrites.size()) return Fail(Dx11ExecutionError::Capacity);
        textureWrites[textureWriteCount++] = retained.value.binding; retained.textureWritten = true; return true;
    }
    std::array<ComPtr<ID3D11Resource>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT + 1 + UavSlots11_1 + D3D11_SO_BUFFER_SLOT_COUNT> writes;
    std::array<std::uint32_t, VertexSlots> simulatedVertices{};
    std::uint32_t simulatedIndex = NoResource;
    std::array<std::uint32_t, 2> simulatedOutline{NoResource, NoResource};
    std::array<std::uint32_t, PixelSlots> simulatedPixels{}, qualifiedPixels{};
    std::array<std::uint32_t, PixelSlots> simulatedVertexTextures{}, qualifiedVertexTextures{};
    std::array<unsigned, PixelSlots> materialSlots{};
    unsigned materialSlotCount = 0;
    std::array<unsigned, PixelSlots> vertexTextureSlots{};
    unsigned vertexTextureSlotCount = 0;
    std::uint32_t resourceCount = 0, aliasCount = 0, writeCount = 0, tapeCount = 0;
    std::uint64_t stamp = 0;
    Dx11ExecutionError error = Dx11ExecutionError::None;
    unsigned failedAction = 0;

    explicit Storage(std::uint32_t capacity)
    {
        tape.resize(capacity); aliases.resize(capacity); rasterScopes.resize(capacity);
        // At most two resolutions per action (old/new alias), every initial
        // queried slot, plus the lazy IA bounds witnesses. Direct-map collisions
        // can make those lazy witnesses distinct retained entries.
        resources.resize(std::size_t(capacity) * 2 + VertexSlots + PixelSlots * 2 + 1 + GimiVertexSlots + 1 + D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT + 1);
        std::size_t slots = 1;
        while (slots < resources.size() * 2) slots *= 2;
        lookup.resize(slots);
    }
    bool Fail(Dx11ExecutionError e) noexcept { error = e; return false; }
    bool Reset(const Dx11ExecutionRequest& r) noexcept
    {
        // Called only when no external retirement owner holds this flight.
        // Retained COM references from earlier submitted work can therefore be
        // released; resetting a CPU frame/ordinal alone never reaches here.
        saved.Clear(); vertexInputs.Clear(); geometryInputs.Clear(); queriedIndex = {};
        overlayLibrary.reset(); overlaySampler.Reset(); overlayCompute.Reset(); overlayBlend.Reset();
        overlayBlendFactor = {}; overlaySampleMask = 0; overlayCaptured = false;
        for (auto& query : queriedOutputs) query = {};
        for (auto& output : originalOutputs) output.Reset();
        originalDepth.Reset(); simulatedOutputs = {}; outputsChanged = outputTape = false;
        streams = {}; streamPlanned = {}; streamIssued = {};
        for (unsigned slot = 0; slot < pixelSamplers.size(); ++slot)
            if (pixelSamplerMask & (1u << slot)) pixelSamplers[slot].Reset();
        pixelSamplerMask = 0; pixelSamplersCaptured = false;
        simulatedPixels.fill(NoResource); qualifiedPixels.fill(NoResource); materialSlotCount = 0;
        simulatedVertexTextures.fill(NoResource); qualifiedVertexTextures.fill(NoResource); vertexTextureSlotCount = 0;
        for (unsigned n = 0; n < resourceCount; ++n) resources[n] = {};
        for (unsigned n = 0; n < aliasCount; ++n) aliases[n] = {};
        for (unsigned n = 0; n < writeCount; ++n) writes[n].Reset();
        resourceCount = aliasCount = writeCount = tapeCount = failedAction = 0;
        depthCount = textureWriteCount = 0; copiedDepth = {};
        for (unsigned n = 0; n < rasterDepth; ++n) rasterScopes[n] = {};
        rasterDepth = 0; simulatedRaster = nullptr; simulatedConstantNulls = 0;
        failedVertexConstant = InvalidId; vertexConstantReason = nullptr;
        simulatedVertices.fill(NoResource); simulatedIndex = NoResource;
        simulatedOutline.fill(NoResource);
        error = Dx11ExecutionError::None;
        if (stamp == (std::numeric_limits<std::uint64_t>::max)()) return Fail(Dx11ExecutionError::Capacity);
        ++stamp;
        plan = r.commands->PlanOwner(); pipeline = r.pipeline;
        device = r.device; context = r.context;
        parameters.reset(); rasterOwner = plan->Touches().rasterCustom ? r.statics : nullptr;
        if (rasterOwner && (rasterOwner->DeviceGeneration() != pipeline.epochs.device ||
            !OnDevice(rasterOwner->Raster(1), r.device) || !OnDevice(rasterOwner->Raster(2), r.device)))
            return Fail(Dx11ExecutionError::WrongOwner);
        initialParameters = finalParameters = r.parameterState;
        if (plan->Touches().iniParameters)
        {
            if (r.statics->DeviceGeneration() != pipeline.epochs.device ||
                !r.statics->ParameterView(initialParameters) ||
                !OnDevice(r.statics->ParameterView(initialParameters), r.device))
                return Fail(Dx11ExecutionError::UnsupportedParameter);
            parameters = r.statics;
        }
        return true;
    }
    template<class T> bool WriteView(T* view) noexcept
    {
        if (!view) return true;
        if (writeCount == writes.size()) return Fail(Dx11ExecutionError::Capacity);
        view->GetResource(writes[writeCount++].GetAddressOf());
        return true;
    }
    bool CapturePipeline(const Dx11ExecutionRequest& r) noexcept
    {
        const auto& p = r.pipeline;
        if (parameters && (!p.iniParameterSlotUnused || p.geometryShader))
            return Fail(Dx11ExecutionError::UnsupportedParameter);
        if (p.vertexReplacement && (!parameters || !p.iniParameterSlotUnused || p.geometryShader ||
            !p.vertexReplacement->ValidFor(r.device, p.vertexShader, p.vertexIdentity, p.epochs.plan, p.epochs.device) ||
            !OnDevice(p.vertexReplacement->Shader(), r.device)))
            return Fail(Dx11ExecutionError::VertexShader);
        if (p.pixelReplacement && (!parameters || !p.iniParameterSlotUnused || p.geometryShader ||
            !p.pixelReplacement->ValidFor(r.device, p.pixelShader, p.pixelIdentity, p.epochs.plan, p.epochs.device) ||
            !OnDevice(p.pixelReplacement->Shader(), r.device)))
            return Fail(Dx11ExecutionError::UnsupportedParameter);
        if (!p.owner || !p.layout || !p.inputLayout || !p.vertexShader ||
            !p.epochs.plan || !p.epochs.consumer || !p.epochs.device || !p.epochs.context || !p.epochs.bindings ||
            p.qualified.knowledge != Knowledge::Known || !p.qualified.object ||
            p.qualified.contract != p.layout->IdValue() || p.qualified.deviceGeneration != p.epochs.device ||
            p.vertexIdentity.knowledge != Knowledge::Known || !p.vertexIdentity.object ||
            !p.vertexIdentity.generation || p.vertexIdentity.hash != p.layout->VertexShaderHash() ||
            (p.pixelShader ? (p.pixelIdentity.knowledge != Knowledge::Known || !p.pixelIdentity.object || !p.pixelIdentity.generation) :
                (p.pixelIdentity.knowledge != Knowledge::Null || p.pixelIdentity.object || p.pixelIdentity.generation)) ||
            (p.topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && p.topology != D3D11_PRIMITIVE_TOPOLOGY_POINTLIST) ||
            !p.layout->Slots())
            return Fail(Dx11ExecutionError::Layout);
        if (!OnDevice(p.inputLayout, r.device) || !OnDevice(p.vertexShader, r.device) ||
            (p.pixelShader && !OnDevice(p.pixelShader, r.device)))
            return Fail(Dx11ExecutionError::WrongOwner);
        r.context->IAGetInputLayout(saved.layout.GetAddressOf());
        UINT vertexClasses = 0;
        r.context->VSGetShader(saved.vertexShader.GetAddressOf(), nullptr, &vertexClasses);
        if (vertexClasses) return Fail(Dx11ExecutionError::Unsupported);
        UINT pixelClasses = 0;
        r.context->PSGetShader(saved.pixelShader.GetAddressOf(), nullptr, &pixelClasses);
        if (pixelClasses) return Fail(Dx11ExecutionError::Unsupported);
        r.context->IAGetPrimitiveTopology(&saved.topology);
        if (saved.layout.Get() != p.inputLayout || saved.vertexShader.Get() != p.vertexShader ||
            saved.pixelShader.Get() != p.pixelShader || saved.topology != p.topology)
            return Fail(Dx11ExecutionError::Layout);

        ComPtr<ID3D11HullShader> hs; ComPtr<ID3D11DomainShader> ds; ComPtr<ID3D11GeometryShader> gs;
        r.context->HSGetShader(hs.GetAddressOf(), nullptr, nullptr);
        r.context->DSGetShader(ds.GetAddressOf(), nullptr, nullptr);
        UINT geometryClasses = 0;
        r.context->GSGetShader(gs.GetAddressOf(), nullptr, &geometryClasses);
        if (hs || ds || geometryClasses || gs.Get() != p.geometryShader) return Fail(Dx11ExecutionError::Unsupported);
        saved.geometryShader = gs;
        const auto* contract = p.streamContract;
        if (gs && (!OnDevice(gs.Get(), r.device) || !contract || !contract->qualified ||
            !p.geometryInputs || !p.geometryInputs->qualified || !p.geometryIdentity.object ||
            p.geometryIdentity.knowledge != Knowledge::Known || p.geometryIdentity.generation != p.epochs.device ||
            p.topology != D3D11_PRIMITIVE_TOPOLOGY_POINTLIST ||
            contract->rasterizedStream != D3D11_SO_NO_RASTERIZED_STREAM ||
            !r.resources->RetainStreamOutput(streams) || !streams.revision || streams.mask != contract->slots))
            return Fail(Dx11ExecutionError::Unsupported);
        ID3D11Buffer* stream[D3D11_SO_BUFFER_SLOT_COUNT]{};
        r.context->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, stream);
        bool streamGood = true;
        for (UINT slot = 0; slot < streams.targets.size(); ++slot)
        {
            auto* actual = stream[slot]; const auto& target = streams.targets[slot];
            if (actual != target.buffer) streamGood = false;
            if (actual)
            {
                D3D11_BUFFER_DESC desc{}; actual->GetDesc(&desc);
                if (!gs || !target.owner || !target.object || target.deviceGeneration != p.epochs.device ||
                    !BufferDescriptor(desc, target.descriptor) || !(desc.BindFlags & D3D11_BIND_STREAM_OUTPUT) ||
                    desc.Usage != D3D11_USAGE_DEFAULT || desc.CPUAccessFlags || (desc.MiscFlags & SharedResources) ||
                    !target.contentGeneration || target.cursorUpper > desc.ByteWidth || writeCount == writes.size()) streamGood = false;
                else writes[writeCount++] = actual;
                actual->Release();
            }
        }
        if (!streamGood) return Fail(Dx11ExecutionError::Unsupported);

        // FL11.1 has 64 UAV slots, FL11.0 has eight. Reject ALL OM UAVs:
        // PrepareDraw requires a stable query set across the original raster,
        // and this first slice does not model arbitrary shader storage writes.
        const UINT uavCount = r.device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? UavSlots11_1 : D3D11_PS_CS_UAV_REGISTER_COUNT;
        ID3D11RenderTargetView* rt[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* depth = nullptr;
        r.context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rt, &depth);
        bool good = true;
        for (unsigned slot = 0; slot < originalOutputs.size(); ++slot)
        {
            originalOutputs[slot].Attach(rt[slot]);
            if (rt[slot] && !WriteView(rt[slot])) good = false;
        }
        originalDepth.Attach(depth);
        if (depth && !WriteView(depth)) good = false;
        if (!good) return false;
        ID3D11UnorderedAccessView* unordered[UavSlots11_1]{};
        // NumRTVs=0 only queries the UAV range; it does not change current RTs.
        r.context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, uavCount, unordered);
        bool omWrites = false;
        for (unsigned n = 0; n < uavCount; ++n) if (unordered[n]) { omWrites = true; unordered[n]->Release(); unordered[n] = nullptr; }
        if (omWrites) return Fail(Dx11ExecutionError::Unsupported);
        r.context->CSGetUnorderedAccessViews(0, uavCount, unordered);
        for (unsigned n = 0; n < uavCount; ++n) if (unordered[n])
        {
            if (!WriteView(unordered[n])) good = false;
            unordered[n]->Release();
        }
        if (p.pixelReplacement && p.pixelReplacement->OutlineResources())
        {
            // Retain the ACTUAL current sampler through this packet's existing
            // real GPU retirement. Null is the observed API state, never a
            // synthesized sampler. No sampler command/creation is introduced.
            r.context->PSGetSamplers(15, 1, saved.outlineSampler.GetAddressOf());
            if (saved.outlineSampler)
            {
                if (!OnDevice(saved.outlineSampler.Get(), r.device)) return Fail(Dx11ExecutionError::OutlineSampler);
                saved.outlineSampler->GetDesc(&saved.outlineSamplerDesc);
            }
        }
        return good && CaptureShaderInputs(r, false) && (!p.geometryShader || CaptureShaderInputs(r, true));
    }
    bool VertexRead(const Dx11ExecutionRequest& r, ID3D11Resource* resource,
                    ID3D11ShaderResourceView* view, Dx11VertexReadLease& out) noexcept
    {
        if (!resource || !r.resources->RetainVertexRead(resource, view, out) || !out.owner || !out.object ||
            out.generation != r.pipeline.epochs.device || out.resource != resource || out.srv != view ||
            !OnDevice(resource, r.device)) return Fail(Dx11ExecutionError::Unsupported);
        // Creation ownership and map/write qualification came from the provider,
        // independently of these actual descriptor/shape/hazard checks.
        for (UINT n = 0; n < writeCount; ++n)
            if (writes[n].Get() == resource) return Fail(Dx11ExecutionError::Hazard);
        return true;
    }
    bool CaptureShaderInputs(const Dx11ExecutionRequest& r, bool geometry) noexcept
    {
        const auto* contract = geometry ? r.pipeline.geometryInputs : r.pipeline.vertexInputs;
        if (!contract || !contract->qualified) return Fail(Dx11ExecutionError::Unsupported);
        auto& kept = geometry ? geometryInputs : vertexInputs;
        for (UINT slot = 0; slot < contract->constantBytes.size(); ++slot)
        {
            const UINT added = !geometry && r.pipeline.vertexReplacement ?
                r.pipeline.vertexReplacement->AddedConstantBytes()[slot] : 0;
            const UINT extra = !geometry && r.pipeline.vertexReplacement ?
                r.pipeline.vertexReplacement->RequiredConstantBytes()[slot] : 0;
            if (!contract->constantBytes[slot] && !extra) continue;
            // Added slots are executable-code authority, never stale original
            // RDEF. A nonnull buffer needs the same exact game read owner and
            // range as original inputs; actual null is a distinct zero-read
            // API binding, not a fabricated resource/content/default page.
            const auto refuse = [&](const char* reason) noexcept {
                if (!extra) return Fail(Dx11ExecutionError::Unsupported);
                failedVertexConstant = slot; vertexConstantReason = reason;
                return Fail(Dx11ExecutionError::VertexConstant);
            };
            if (added && contract->constantBytes[slot]) return refuse("original reflected slot conflicts with added reader");
            const auto required = (std::max)(extra, contract->constantBytes[slot]);
            kept.constantMask |= 1u << slot;
            if (auto* range = r.pipeline.vertexRangeContext)
            {
                if (geometry) range->GSGetConstantBuffers1(slot, 1, kept.constants[slot].GetAddressOf(), &kept.first[slot], &kept.count[slot]);
                else range->VSGetConstantBuffers1(slot, 1, kept.constants[slot].GetAddressOf(), &kept.first[slot], &kept.count[slot]);
            }
            else
            {
                if (geometry) r.context->GSGetConstantBuffers(slot, 1, kept.constants[slot].GetAddressOf());
                else r.context->VSGetConstantBuffers(slot, 1, kept.constants[slot].GetAddressOf());
                kept.first[slot] = 0; kept.count[slot] = D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
            }
            auto* buffer = kept.constants[slot].Get(); auto& lease = kept.leases[slot];
            if (added && !buffer) continue; // authentic getter null, no COM reader owner exists
            if (!kept.count[slot] || kept.count[slot] > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT)
                return refuse("actual constant binding range is empty or exceeds 4096 vectors");
            if (!VertexRead(r, buffer, nullptr, lease))
                return extra ? refuse("nonnull CB lacks current exact game read owner/device/coverage or has write hazard") : false;
            D3D11_BUFFER_DESC desc{}; buffer->GetDesc(&desc);
            const auto offset = std::uint64_t(kept.first[slot]) * 16;
            if (lease.buffer != buffer || !BufferDescriptor(desc, lease.bufferDesc) ||
                desc.BindFlags != D3D11_BIND_CONSTANT_BUFFER || desc.MiscFlags || !desc.ByteWidth || (desc.ByteWidth % 16) ||
                offset > desc.ByteWidth || required > desc.ByteWidth - offset ||
                required > std::uint64_t(kept.count[slot]) * 16)
                return refuse("actual descriptor/first/count does not cover exact added CB byte range");
        }
        for (UINT slot = 0; slot < contract->resources.size(); ++slot)
        {
            const auto shape = contract->resources[slot];
            if (shape.kind == Dx11VertexReadKind::None) continue;
            SetBit(kept.viewMask, slot);
            if (geometry) r.context->GSGetShaderResources(slot, 1, kept.views[slot].GetAddressOf());
            else r.context->VSGetShaderResources(slot, 1, kept.views[slot].GetAddressOf());
            auto* view = kept.views[slot].Get();
            if (!view || !OnDevice(view, r.device)) return Fail(Dx11ExecutionError::Binding);
            ComPtr<ID3D11Resource> resource; view->GetResource(resource.GetAddressOf());
            auto& lease = kept.leases[kept.constants.size() + slot];
            if (!VertexRead(r, resource.Get(), view, lease)) return false;
            D3D11_SHADER_RESOURCE_VIEW_DESC desc{}; view->GetDesc(&desc);
            D3D11_RESOURCE_DIMENSION dimension{}; resource->GetType(&dimension);
            if (shape.kind == Dx11VertexReadKind::TypedBuffer || shape.kind == Dx11VertexReadKind::StructuredBuffer)
            {
                if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER || !lease.buffer ||
                    static_cast<ID3D11Resource*>(lease.buffer) != resource.Get() || desc.ViewDimension != D3D11_SRV_DIMENSION_BUFFER)
                    return Fail(Dx11ExecutionError::Unsupported);
                D3D11_BUFFER_DESC buffer{}; lease.buffer->GetDesc(&buffer);
                if (!BufferDescriptor(buffer, lease.bufferDesc) || !(buffer.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
                    (buffer.BindFlags & (lease.gpuOrdered ? (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL) : WriteBindings)) || (buffer.MiscFlags & ~D3D11_RESOURCE_MISC_BUFFER_STRUCTURED))
                    return Fail(Dx11ExecutionError::Unsupported);
                UINT stride = 0;
                if (shape.kind == Dx11VertexReadKind::StructuredBuffer)
                {
                    if (desc.Format != DXGI_FORMAT_UNKNOWN || buffer.MiscFlags != D3D11_RESOURCE_MISC_BUFFER_STRUCTURED ||
                        !buffer.StructureByteStride || buffer.StructureByteStride > D3D11_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES ||
                        (buffer.StructureByteStride % 4)) return Fail(Dx11ExecutionError::Unsupported);
                    stride = buffer.StructureByteStride;
                }
                else
                {
                    if (buffer.MiscFlags || buffer.StructureByteStride || !VertexReturnFormat(desc.Format, shape.returns))
                        return Fail(Dx11ExecutionError::Unsupported);
                    stride = FormatElementBytes(desc.Format);
                }
                if (!stride || !desc.Buffer.NumElements || (std::uint64_t(desc.Buffer.FirstElement) + desc.Buffer.NumElements) * stride > buffer.ByteWidth)
                    return Fail(Dx11ExecutionError::Unsupported);
            }
            else if (shape.kind == Dx11VertexReadKind::Texture2D)
            {
                if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D || lease.buffer ||
                    desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || !VertexReturnFormat(desc.Format, shape.returns))
                    return Fail(Dx11ExecutionError::Unsupported);
                if (!lease.texture || static_cast<ID3D11Resource*>(lease.texture) != resource.Get())
                    return Fail(Dx11ExecutionError::Unsupported);
                D3D11_TEXTURE2D_DESC actual{}; lease.texture->GetDesc(&actual);
                if (!TextureDescriptor(actual, lease.textureDesc) || actual.Format != desc.Format ||
                    actual.MipLevels != 1 || actual.ArraySize != 1 || actual.SampleDesc.Count != 1 || actual.SampleDesc.Quality ||
                    actual.BindFlags != D3D11_BIND_SHADER_RESOURCE || actual.MiscFlags || desc.Texture2D.MostDetailedMip ||
                    (desc.Texture2D.MipLevels != 1 && desc.Texture2D.MipLevels != UINT(-1)))
                    return Fail(Dx11ExecutionError::Unsupported);
            }
            else return Fail(Dx11ExecutionError::Unsupported);
        }
        for (UINT slot = 0; slot < kept.samplers.size(); ++slot) if (contract->samplers & (1u << slot))
        {
            kept.samplerMask |= 1u << slot;
            if (geometry) r.context->GSGetSamplers(slot, 1, kept.samplers[slot].GetAddressOf());
            else r.context->VSGetSamplers(slot, 1, kept.samplers[slot].GetAddressOf());
            if (!OnDevice(kept.samplers[slot].Get(), r.device)) return Fail(Dx11ExecutionError::Binding);
        }
        // Original CB read contracts/samplers stay unchanged. Explicit POST
        // CB nulls and touched VS SRVs have separate exact save/restore cursors;
        // all original reader COM owners remain in
        // this SAME packet through reserve/partial failure/Present retirement.
        return true;
    }
    void CaptureVertex(ID3D11DeviceContext* c, unsigned slot) noexcept
    {
        if (saved.vertexCaptured & (1u << slot)) return;
        c->IAGetVertexBuffers(slot, 1, saved.vertices[slot].GetAddressOf(), &saved.strides[slot], &saved.offsets[slot]);
        saved.vertexCaptured |= 1u << slot;
    }
    void CaptureIndex(ID3D11DeviceContext* c) noexcept
    {
        if (saved.indexCaptured) return;
        c->IAGetIndexBuffer(saved.index.GetAddressOf(), &saved.indexFormat, &saved.indexOffset);
        saved.indexCaptured = true;
    }
    void CapturePixel(ID3D11DeviceContext* c, unsigned slot) noexcept
    {
        if (Bit(saved.pixelCaptured, slot)) return;
        c->PSGetShaderResources(slot, 1, saved.pixels[slot].GetAddressOf()); SetBit(saved.pixelCaptured, slot);
    }
    void CaptureVertexTexture(ID3D11DeviceContext* c, unsigned slot) noexcept
    {
        if (Bit(saved.vertexTextureCaptured, slot)) return;
        c->VSGetShaderResources(slot, 1, saved.vertexTextures[slot].GetAddressOf());
        SetBit(saved.vertexTextureCaptured, slot);
    }
    bool Resolve(const Dx11ExecutionRequest& r, Binding binding, std::uint32_t& out,
                 const Dx11RetainedBinding* acquired = nullptr, bool allowOutput = false) noexcept
    {
        // Pending copy destinations are visible ONLY inside this packet. A
        // direct-map collision cannot expose an unsubmitted pool slot globally.
        for (unsigned n = 0; n < depthCount; ++n)
            if (SameBinding(resources[preparedDepth[n]].value.binding, binding))
            { out = preparedDepth[n]; return true; }
        auto& cached = lookup[static_cast<std::size_t>(BindingKey(binding)) & (lookup.size() - 1)];
        if (cached.stamp == stamp && SameBinding(cached.binding, binding))
        {
            if (!allowOutput && resources[cached.resource].output.owner) return Fail(Dx11ExecutionError::Binding);
            out = cached.resource; return true;
        }
        if (resourceCount == resources.size()) return Fail(Dx11ExecutionError::Capacity);
        Dx11RetainedBinding resolved;
        if (!binding.object)
        {
            if (binding.view) return Fail(Dx11ExecutionError::Binding);
            resolved.value.binding = binding;
            resolved.value.identity = ResourceIdentity::KnownNull();
            resolved.stableReadOnly = true;
        }
        else
        {
            if (acquired) resolved = *acquired;
            else if (r.statics->Resolve(binding, resolved.value))
            {
                resolved.statics = r.statics; resolved.stableReadOnly = true;
            }
            else if (allowOutput && r.resources->ResolveOutputOperand(binding, resolved.output))
            {
                resolved.value = {resolved.output.texture, nullptr, nullptr, binding, resolved.output.identity};
                resolved.game = resolved.output.owner; // typed ownership, no content/read authority
            }
            else if (!r.resources->ResolveGame(binding, resolved)) return Fail(Dx11ExecutionError::Binding);
            const auto& v = resolved.value;
            const auto& identity = v.identity;
            if (resolved.output.owner)
            {
                if (!allowOutput || resolved.statics || resolved.game != resolved.output.owner || resolved.stableReadOnly ||
                    v.srv || v.buffer || v.resource != resolved.output.texture || !SameBinding(v.binding, binding) ||
                    !SameBinding(resolved.output.binding, binding) || !ValidateOutput(r, resolved.output) ||
                    (identity.origin == ResourceOrigin::Custom && (!resolved.ownedTexture ||
                        resolved.game.get() != resolved.ownedTexture.get() ||
                        !SameBinding(binding, resolved.ownedTexture->outputBinding) ||
                        !ValidatePrivateTexture(*resolved.ownedTexture, r.device))) ||
                    (identity.origin == ResourceOrigin::Game && resolved.ownedTexture))
                    return Fail(Dx11ExecutionError::Binding);
            }
            else
            {
                if (!resolved.stableReadOnly || (!resolved.statics && !resolved.game) ||
                    (resolved.statics && resolved.game) || !v.resource || !SameBinding(v.binding, binding) ||
                    identity.knowledge != Knowledge::Known ||
                    (!identity.contentKnown && !identity.gpuOrdered && !(resolved.ownedTexture &&
                        (acquired || resolved.ownedTexture->purpose == OwnedTexturePurpose::OutputDescription))) ||
                    // Static custom resources deliberately have no GAME trigger
                    // hash. Their exact static owner/lease resolves above; retain
                    // the unchanged hash requirement for game-owned bindings.
                    (!resolved.statics && !identity.hashKnown && !resolved.ownedTexture) ||
                    identity.object != binding.object || !identity.contentGeneration || !identity.descriptorGeneration ||
                    identity.deviceGeneration != r.pipeline.epochs.device || binding.generation != r.pipeline.epochs.device)
                    return Fail(Dx11ExecutionError::Generation);
                if (resolved.ownedTexture)
                {
                    const auto& depth = *resolved.ownedTexture;
                    if (resolved.game.get() != &depth || resolved.statics || identity.origin != ResourceOrigin::Custom ||
                        (!identity.gpuOrdered && !acquired && depth.purpose != OwnedTexturePurpose::OutputDescription) || identity.contentKnown || identity.hashKnown ||
                        v.resource != depth.texture.Get() || v.srv != depth.srv.Get() ||
                        !SameBinding(binding, depth.binding) || (!acquired && !depth.ordered && depth.purpose != OwnedTexturePurpose::OutputDescription) ||
                        !ValidatePrivateTexture(depth, r.device))
                        return Fail(Dx11ExecutionError::Generation);
                }
                if (!OnDevice(v.resource, r.device)) return Fail(Dx11ExecutionError::WrongOwner);
                D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
                v.resource->GetType(&dimension);
                if (identity.kind == IdentityKind::Buffer)
                {
                    if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER || !v.buffer ||
                        static_cast<ID3D11Resource*>(v.buffer) != v.resource) return Fail(Dx11ExecutionError::Binding);
                    D3D11_BUFFER_DESC desc{}; v.buffer->GetDesc(&desc);
                    if (!BufferDescriptor(desc, identity.buffer) || desc.ByteWidth != binding.byteSize ||
                        binding.offset > desc.ByteWidth || (desc.BindFlags & (identity.gpuOrdered ? (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL) : WriteBindings)) || (desc.MiscFlags & SharedResources))
                        return Fail(Dx11ExecutionError::Binding);
                }
                else if (identity.kind == IdentityKind::Texture2D)
                {
                    if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D || v.buffer || binding.byteSize || binding.offset || binding.stride)
                        return Fail(Dx11ExecutionError::Binding);
                    // The dimension was queried from the actual retained object.
                    ComPtr<ID3D11Texture2D> texture;
                    if (FAILED(v.resource->QueryInterface(IID_PPV_ARGS(texture.GetAddressOf())))) return Fail(Dx11ExecutionError::Binding);
                    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
                    if (!TextureDescriptor(desc, identity.texture) || ((desc.BindFlags & WriteBindings) &&
                        (!resolved.ownedTexture || resolved.ownedTexture->purpose != OwnedTexturePurpose::OutputDescription)) ||
                        (desc.MiscFlags & SharedResources)) return Fail(Dx11ExecutionError::Binding);
                }
                else return Fail(Dx11ExecutionError::Binding);
                if (binding.view)
                {
                    if (!v.srv || !OnDevice(v.srv, r.device)) return Fail(Dx11ExecutionError::Binding);
                    ComPtr<ID3D11Resource> actual; v.srv->GetResource(actual.GetAddressOf());
                    D3D11_SHADER_RESOURCE_VIEW_DESC desc{}; v.srv->GetDesc(&desc);
                    if (actual.Get() != v.resource || static_cast<UINT>(desc.Format) != binding.format)
                        return Fail(Dx11ExecutionError::Binding);
                }
                else if (v.srv) return Fail(Dx11ExecutionError::Binding);
                for (unsigned n = 0; n < writeCount; ++n)
                    if (writes[n].Get() == v.resource) return Fail(Dx11ExecutionError::Hazard);
            }
        }
        out = resourceCount++;
        resources[out] = std::move(resolved);
        cached = {binding, stamp, out};
        return true;
    }
    bool ValidateOutput(const Dx11ExecutionRequest& r, const Dx11OutputBinding& o) noexcept
    {
        if (!o.owner || !o.texture || (bool(o.rtv) == bool(o.dsv)) || !o.binding.object || !o.binding.view ||
            o.binding.stride || o.binding.offset || o.binding.byteSize ||
            o.identity.knowledge != Knowledge::Known || o.identity.kind != IdentityKind::Texture2D ||
            o.identity.object != o.binding.object || o.identity.deviceGeneration != pipeline.epochs.device ||
            o.binding.generation != pipeline.epochs.device || !o.identity.descriptorGeneration ||
            o.view.view != o.binding.view || !o.view.generation || static_cast<UINT>(o.view.format) != o.binding.format ||
            o.identity.hashKnown || o.identity.creationHashKnown || o.identity.contentKnown ||
            !OnDevice(o.texture, r.device)) return false;
        if (o.identity.origin != ResourceOrigin::Game && o.identity.origin != ResourceOrigin::Custom) return false;
        if (o.identity.origin == ResourceOrigin::Game && o.identity.gpuOrdered) return false;
        D3D11_TEXTURE2D_DESC texture{}; o.texture->GetDesc(&texture);
        if (!TextureDescriptor(texture, o.identity.texture) || o.view.mip >= 32) return false;
        ComPtr<ID3D11Resource> actual; auto view = o.view;
        if (o.dsv)
        {
            if (!o.view.depth || !OnDevice(o.dsv, r.device)) return false;
            D3D11_DEPTH_STENCIL_VIEW_DESC desc{}; o.dsv->GetDesc(&desc); o.dsv->GetResource(&actual);
            if (!DescribeOutputView(texture, desc, view)) return false;
        }
        else
        {
            if (o.view.depth || !OnDevice(o.rtv, r.device)) return false;
            D3D11_RENDER_TARGET_VIEW_DESC desc{}; o.rtv->GetDesc(&desc); o.rtv->GetResource(&actual);
            if (!DescribeOutputView(texture, desc, view)) return false;
        }
        return actual.Get() == o.texture && view == o.view;
    }
    bool OutputOf(const Dx11ExecutionRequest& r, std::uint32_t index, Dx11OutputBinding& out) noexcept
    {
        const auto& kept = resources[index];
        if (!kept.value.binding.object) { out = {}; out.identity = ResourceIdentity::KnownNull(); return true; }
        if (kept.output.owner) { out = kept.output; return ValidateOutput(r, out); }
        const auto& target = kept.ownedTexture;
        if (!target || target->purpose != OwnedTexturePurpose::OutputDescription ||
            !ValidatePrivateTexture(*target, r.device) || !target->outputBinding.object) return Fail(Dx11ExecutionError::Binding);
        out = {}; out.owner = target; out.texture = target->texture.Get();
        out.rtv = target->rtv.Get(); out.dsv = target->dsv.Get();
        out.binding = target->outputBinding; out.identity = target->identity;
        bool good = false;
        if (out.dsv) { D3D11_DEPTH_STENCIL_VIEW_DESC d{}; out.dsv->GetDesc(&d); good = DescribeOutputView(out.identity.texture, d, out.view); }
        else if (out.rtv) { D3D11_RENDER_TARGET_VIEW_DESC d{}; out.rtv->GetDesc(&d); good = DescribeOutputView(out.identity.texture, d, out.view); }
        out.view.view = out.binding.view; out.view.generation = out.identity.descriptorGeneration;
        return good && ValidateOutput(r, out);
    }
    bool InitialOutputs(const Dx11ExecutionRequest& r) noexcept
    {
        if (!outputTape) return true;
        for (unsigned slot = 0; slot < simulatedOutputs.size(); ++slot)
        {
            auto& o = simulatedOutputs[slot];
            o.rtv = slot < originalOutputs.size() ? originalOutputs[slot].Get() : nullptr;
            o.dsv = slot == originalOutputs.size() ? originalDepth.Get() : nullptr;
            if (!o.rtv && !o.dsv) continue;
            ComPtr<ID3D11Resource> resource; ComPtr<ID3D11Texture2D> texture;
            if (o.dsv) o.dsv->GetResource(&resource); else o.rtv->GetResource(&resource);
            if (!resource || FAILED(resource.As(&texture)) || !OnDevice(texture.Get(), r.device)) return Fail(Dx11ExecutionError::Binding);
            o.texture = texture.Get(); texture->GetDesc(&o.identity.texture);
            bool good = false;
            if (o.dsv) { D3D11_DEPTH_STENCIL_VIEW_DESC d{}; o.dsv->GetDesc(&d); good = DescribeOutputView(o.identity.texture, d, o.view); }
            else { D3D11_RENDER_TARGET_VIEW_DESC d{}; o.rtv->GetDesc(&d); good = DescribeOutputView(o.identity.texture, d, o.view); }
            if (!good || o.view.mip >= 32) return Fail(Dx11ExecutionError::Unsupported);
        }
        return true;
    }
    bool OutputConflict(ID3D11Resource* resource) const noexcept
    {
        if (!outputTape) return false;
        for (const auto& output : simulatedOutputs) if (output.texture == resource && resource) return true;
        return false;
    }
    bool CommandReadConflict(ID3D11Resource* resource) const noexcept
    {
        if (!resource) return false;
        for (unsigned i = 0; i < materialSlotCount; ++i)
        {
            const auto index = simulatedPixels[materialSlots[i]];
            if (index != NoResource && resources[index].value.resource == resource) return true;
        }
        for (unsigned i = 0; i < vertexTextureSlotCount; ++i)
        {
            const auto index = simulatedVertexTextures[vertexTextureSlots[i]];
            if (index != NoResource && resources[index].value.resource == resource) return true;
        }
        return false;
    }
    bool PrepareOutput(const Dx11ExecutionRequest& r, PreparedAction& prepared) noexcept
    {
        const auto& a = prepared.action; const bool depth = a.destination.kind == TargetKind::DepthTarget;
        if ((!depth && a.destination.kind != TargetKind::RenderTarget) || (depth ? a.destination.index != 0 : a.destination.index >= 8) ||
            !Resolve(r, a.binding, prepared.resource, nullptr, true)) return Fail(Dx11ExecutionError::Binding);
        Dx11OutputBinding output;
        if (!OutputOf(r, prepared.resource, output) || (output.texture && output.view.depth != depth)) return Fail(Dx11ExecutionError::Binding);
        if (output.texture && output.identity.origin == ResourceOrigin::Game)
        {
            // Backup/Restore may rebind an exact view from THIS invocation's
            // authentic OM snapshot. Persistent aliases cannot redirect output
            // to a different game target whose active reader set is unknown.
            bool original = depth ? output.dsv == originalDepth.Get() : false;
            if (!depth) for (const auto& view : originalOutputs) original |= view.Get() == output.rtv;
            if (!original) return Fail(Dx11ExecutionError::Hazard);
        }
        // Do not rely on D3D's implicit read-slot unbinding. Private outputs may
        // only conflict with command-touched VS/PS slots; original game outputs
        // cannot be promoted into read leases. Explicit null/rebind is required.
        if (CommandReadConflict(output.texture)) return Fail(Dx11ExecutionError::Hazard);
        simulatedOutputs[depth ? 8 : a.destination.index] = output;
        return PackOutputs(prepared);
    }
    bool PackOutputs(PreparedAction& prepared) noexcept
    {
        const Dx11OutputBinding* first = nullptr;
        for (unsigned slot = 0; slot < simulatedOutputs.size(); ++slot)
        {
            const auto& o = simulatedOutputs[slot];
            if (slot < prepared.outputs.size()) prepared.outputs[slot] = o.rtv; else prepared.depth = o.dsv;
            if (!o.texture) continue;
            if (first)
            {
                const auto& x = first->identity.texture; const auto& y = o.identity.texture;
                if ((std::max)(1u, x.Width >> first->view.mip) != (std::max)(1u, y.Width >> o.view.mip) ||
                    (std::max)(1u, x.Height >> first->view.mip) != (std::max)(1u, y.Height >> o.view.mip) ||
                    first->view.sliceCount != o.view.sliceCount || x.SampleDesc.Count != y.SampleDesc.Count ||
                    x.SampleDesc.Quality != y.SampleDesc.Quality) return Fail(Dx11ExecutionError::Binding);
            }
            else first = &o;
            for (unsigned previous = 0; previous < slot; ++previous)
                if (simulatedOutputs[previous].texture == o.texture) return Fail(Dx11ExecutionError::Hazard);
        }
        return true; // binding a destination grants NO initialized-content receipt
    }
    void Restore(ID3D11DeviceContext* context) const noexcept
    {
        if (outputsChanged)
        {
            // Release only modified read slots before rebinding the exact game
            // OM snapshot, then restore their exact original SRVs below.
            ID3D11ShaderResourceView* nullView = nullptr;
            for (unsigned n = 0; n < PixelSlots; ++n)
            {
                if (Bit(saved.pixelChanged, n)) context->PSSetShaderResources(n, 1, &nullView);
                if (Bit(saved.vertexTextureChanged, n)) context->VSSetShaderResources(n, 1, &nullView);
            }
            std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> original{};
            for (unsigned n = 0; n < original.size(); ++n) original[n] = originalOutputs[n].Get();
            context->OMSetRenderTargets(static_cast<UINT>(original.size()), original.data(), originalDepth.Get());
        }
        saved.Restore(context);
    }
    struct CheckedQueries
    {
        Storage& work;
        const Dx11ExecutionRequest& request;
        static bool Read(void* raw, Target target, Binding& binding) noexcept
        {
            auto& q = *static_cast<CheckedQueries*>(raw);
            const auto& r = q.request; auto& w = q.work;
            if (w.parameters && (target.kind == TargetKind::PixelTexture || target.kind == TargetKind::VertexTexture) &&
                target.index == IniParameterRegister) return false;
            if (!r.queries.readBinding || !r.queries.readBinding(r.queries.context, target, binding)) return false;
            // Shader IDs have their own identity revision, not a device epoch.
            // CapturePipeline already compared the ACTUAL current COM shaders.
            if (target.kind == TargetKind::VertexShader || target.kind == TargetKind::PixelShader)
            {
                if (target.index) return false;
                const auto& shader = target.kind == TargetKind::VertexShader ? w.pipeline.vertexIdentity : w.pipeline.pixelIdentity;
                return binding.object == shader.object && binding.generation == shader.generation && !binding.view;
            }
            if (target.kind == TargetKind::RenderTarget || target.kind == TargetKind::DepthTarget)
            {
                const bool depth = target.kind == TargetKind::DepthTarget;
                if (depth ? target.index != 0 : target.index >= 8) return false;
                Dx11OutputBinding described;
                if (!r.resources->DescribeGameOutput(target, binding, described) ||
                    !SameBinding(described.binding, binding) ||
                    described.rtv != (depth ? nullptr : w.originalOutputs[target.index].Get()) ||
                    described.dsv != (depth ? w.originalDepth.Get() : nullptr)) return false;
                if (!binding.object)
                {
                    if (binding.view || described.texture || described.rtv || described.dsv || described.identity.knowledge != Knowledge::Null) return false;
                    w.queriedOutputs[depth ? 8 : target.index] = std::move(described); return true;
                }
                const auto& identity = described.identity;
                if (!described.owner || !described.texture || !binding.view || binding.stride || binding.offset || binding.byteSize ||
                    !OnDevice(described.texture, r.device) || identity.knowledge != Knowledge::Known ||
                    identity.kind != IdentityKind::Texture2D || identity.origin != ResourceOrigin::Game ||
                    identity.object != binding.object || !identity.descriptorGeneration || !described.view.generation ||
                    identity.deviceGeneration != w.pipeline.epochs.device || binding.generation != identity.deviceGeneration ||
                    described.view.view != binding.view || described.view.depth != depth ||
                    static_cast<UINT>(described.view.format) != binding.format ||
                    identity.hashKnown || identity.creationHashKnown || identity.contentKnown || identity.gpuOrdered) return false;
                ComPtr<ID3D11Resource> actual;
                D3D11_TEXTURE2D_DESC texture{}; described.texture->GetDesc(&texture);
                auto view = described.view;
                if (depth)
                {
                    if (!described.dsv || !OnDevice(described.dsv, r.device)) return false;
                    described.dsv->GetResource(&actual);
                    D3D11_DEPTH_STENCIL_VIEW_DESC desc{}; described.dsv->GetDesc(&desc);
                    if (!DescribeOutputView(texture, desc, view)) return false;
                }
                else
                {
                    if (!described.rtv || !OnDevice(described.rtv, r.device)) return false;
                    described.rtv->GetResource(&actual);
                    D3D11_RENDER_TARGET_VIEW_DESC desc{}; described.rtv->GetDesc(&desc);
                    if (!DescribeOutputView(texture, desc, view)) return false;
                }
                if (actual.Get() != described.texture || !TextureDescriptor(texture, identity.texture) ||
                    !(view == described.view)) return false;
                w.queriedOutputs[depth ? 8 : target.index] = std::move(described);
                return true; // metadata does not enter Resolve/Bindable or the GPU read set
            }
            if (!((target.kind == TargetKind::VertexBuffer && target.index < VertexSlots) ||
                  (target.kind == TargetKind::IndexBuffer && !target.index) ||
                  ((target.kind == TargetKind::PixelTexture || target.kind == TargetKind::VertexTexture) && target.index < PixelSlots))) return false;
            Dx11RetainedBinding described;
            if (target.kind == TargetKind::IndexBuffer && r.resources->DescribeGameIndex(binding, described))
            {
                // Check/Filter read only metadata. Do not promote this retained
                // descriptor to a stableReadOnly/bindable resource capability.
                w.CaptureIndex(r.context);
                const auto& v = described.value;
                const auto& identity = v.identity;
                if (!SameBinding(v.binding, binding) || v.srv ||
                    v.buffer != w.saved.index.Get() || v.resource != v.buffer ||
                    binding.format != static_cast<UINT>(w.saved.indexFormat) || binding.offset != w.saved.indexOffset)
                    return false;
                if (!binding.object && (v.resource || v.buffer || identity.knowledge != Knowledge::Null)) return false;
                if (binding.object && (!described.game || !v.buffer || !OnDevice(v.buffer, r.device) ||
                    identity.knowledge != Knowledge::Known || identity.kind != IdentityKind::Buffer ||
                    identity.origin != ResourceOrigin::Game || identity.object != binding.object ||
                    identity.deviceGeneration != w.pipeline.epochs.device || binding.generation != identity.deviceGeneration ||
                    !identity.descriptorGeneration || binding.byteSize != identity.buffer.ByteWidth)) return false;
                if (binding.object)
                {
                    D3D11_BUFFER_DESC actual{}; v.buffer->GetDesc(&actual);
                    if (!BufferDescriptor(actual, identity.buffer) || !(actual.BindFlags & D3D11_BIND_INDEX_BUFFER)) return false;
                }
                w.queriedIndex = std::move(described);
                return true;
            }
            std::uint32_t resolved;
            if (!w.Resolve(r, binding, resolved) || !w.Bindable(resolved, target)) return false;
            const auto& v = w.resources[resolved].value;
            if (target.kind == TargetKind::VertexBuffer)
            {
                w.CaptureVertex(r.context, target.index);
                return v.buffer == w.saved.vertices[target.index].Get() && binding.stride == w.saved.strides[target.index] &&
                    binding.offset == w.saved.offsets[target.index];
            }
            if (target.kind == TargetKind::IndexBuffer)
            {
                w.CaptureIndex(r.context);
                return v.buffer == w.saved.index.Get() && binding.format == static_cast<UINT>(w.saved.indexFormat) &&
                    binding.offset == w.saved.indexOffset;
            }
            if (target.kind == TargetKind::VertexTexture)
            {
                w.CaptureVertexTexture(r.context, target.index);
                return v.srv == w.saved.vertexTextures[target.index].Get();
            }
            w.CapturePixel(r.context, target.index);
            return v.srv == w.saved.pixels[target.index].Get();
        }
        static bool CopyDepth(void* raw, Binding source, Binding& destination) noexcept
        {
            auto& q = *static_cast<CheckedQueries*>(raw); auto& w = q.work;
            const auto& r = q.request;
            const auto& output = w.queriedOutputs[8];
            if (!source.object || !SameBinding(output.binding, source) || w.depthCount == w.preparedDepth.size()) return false;
            Dx11RetainedBinding acquired;
            if (!r.resources->AcquireDepthCopy(output, acquired) || !acquired.ownedTexture) return false;
            const auto& depth = *acquired.ownedTexture;
            if (depth.purpose != OwnedTexturePurpose::DepthCopy || depth.source.owner != output.owner || !SameBinding(depth.source.binding, source) ||
                !(depth.source.view == output.view) || depth.texture.Get() == output.texture) return false;
            std::uint32_t resource;
            if (!w.Resolve(r, acquired.value.binding, resource, &acquired)) return false;
            destination = acquired.value.binding;
            w.preparedDepth[w.depthCount++] = resource;
            return true;
        }
        static bool CopyDescription(void* raw, Target sourceTarget, Binding source, Id alias, Binding& destination) noexcept
        {
            auto& q = *static_cast<CheckedQueries*>(raw); auto& w = q.work;
            const bool depth = sourceTarget.kind == TargetKind::DepthTarget;
            if ((!depth && sourceTarget.kind != TargetKind::RenderTarget) || (depth ? sourceTarget.index != 0 : sourceTarget.index >= 8)) return false;
            const auto& output = w.queriedOutputs[depth ? 8 : sourceTarget.index];
            // This factory slice intentionally derives descriptions only from
            // authenticated game outputs, not a changed/custom OM operand.
            if (!source.object || !SameBinding(output.binding, source)) return false;
            Dx11RetainedBinding acquired;
            if (!q.request.resources->AcquireOutputDescription(output, alias, acquired) || !acquired.ownedTexture) return false;
            const auto& target = *acquired.ownedTexture;
            if (target.purpose != OwnedTexturePurpose::OutputDescription || target.alias != alias ||
                target.source.owner != output.owner || !SameBinding(target.source.binding, source) ||
                !(target.source.view == output.view) || target.texture.Get() == output.texture) return false;
            std::uint32_t index;
            if (!w.Resolve(q.request, acquired.value.binding, index, &acquired)) return false;
            destination = acquired.value.binding; return true; // storage only, no producer receipt
        }
        static bool OutputReference(void* raw, Binding binding, bool depth, Binding& typed) noexcept
        {
            auto& q = *static_cast<CheckedQueries*>(raw); std::uint32_t index; Dx11OutputBinding output;
            if (!q.work.Resolve(q.request, binding, index, nullptr, true) || !q.work.OutputOf(q.request, index, output) ||
                !output.texture || output.view.depth != depth) return false;
            typed = output.binding; return true;
        }
        static bool Native(void* raw, NativeValue operand, Binding binding, float& value) noexcept
        {
            auto& q = *static_cast<CheckedQueries*>(raw); auto& w = q.work;
            if (operand == NativeValue::Time)
                return q.request.queries.nativeValue &&
                    q.request.queries.nativeValue(q.request.queries.context, operand, binding, value);
            if (operand != NativeValue::RenderTargetWidth && operand != NativeValue::RenderTargetHeight) return false;
            // CommandState supplies the exact tape-position o0 operand; initial
            // game operands were authenticated by Read, changed ones by OutputReference.
            std::uint32_t index; Dx11OutputBinding output;
            if (!w.Resolve(q.request, binding, index, nullptr, true) || !w.OutputOf(q.request, index, output)) return false;
            const auto dimension = output.view.dimension;
            value = !binding.object || (dimension != D3D11_RTV_DIMENSION_TEXTURE2D && dimension != D3D11_RTV_DIMENSION_TEXTURE2DMS) ?
                -1.0f : static_cast<float>(operand == NativeValue::RenderTargetWidth ? output.identity.texture.Width : output.identity.texture.Height);
            return true;
        }
        static bool Filter(void* raw, const Plan& p, const MatchQuery& query, float& value) noexcept
        {
            const auto& q = *static_cast<CheckedQueries*>(raw);
            return q.request.queries.filterIndex && q.request.queries.filterIndex(q.request.queries.context, p, query, value);
        }
        static bool Matches(void* raw, const Plan& p, const MatchQuery& query, Id* ids,
                            std::uint32_t capacity, std::uint32_t& count) noexcept
        {
            const auto& q = *static_cast<CheckedQueries*>(raw);
            return q.request.queries.textureMatches &&
                q.request.queries.textureMatches(q.request.queries.context, p, query, ids, capacity, count);
        }
        static bool Parameter(void* raw, Id scalar, float& value) noexcept
        {
            const auto& q = *static_cast<CheckedQueries*>(raw);
            return q.work.parameters && q.work.parameters->ReadParameter(q.work.initialParameters, scalar, value);
        }
        static bool Constant(void* raw, Id slot, Id scalar, float& value) noexcept
        {
            const auto& q = *static_cast<CheckedQueries*>(raw);
            const auto& r = q.request;
            return r.owner->Current(r.device, r.context, r.pipeline.epochs) && r.queries.readPixelConstant &&
                r.queries.readPixelConstant(r.queries.context, slot, scalar, value);
        }
        CommandQueries Commands() noexcept { return {this, Read, Filter, Parameter, Matches, CopyDepth, CopyDescription, Native, OutputReference, Constant}; }
    };
    bool Bindable(std::uint32_t resource, Target target) noexcept
    {
        const auto& v = resources[resource].value;
        if (resources[resource].output.owner) return Fail(Dx11ExecutionError::Binding);
        if (!v.binding.object) return true; // explicit null bind
        if ((target.kind == TargetKind::PixelTexture || target.kind == TargetKind::VertexTexture) && OutputConflict(v.resource)) return Fail(Dx11ExecutionError::Hazard);
        if (!TextureReady(resource)) return Fail(Dx11ExecutionError::Binding); // descriptor alone cannot be sampled
        switch (target.kind)
        {
        case TargetKind::VertexBuffer:
            return v.buffer && (v.identity.buffer.BindFlags & D3D11_BIND_VERTEX_BUFFER) &&
                v.binding.stride && v.binding.stride <= D3D11_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES
                ? true : Fail(Dx11ExecutionError::Layout);
        case TargetKind::IndexBuffer:
        {
            const auto format = static_cast<DXGI_FORMAT>(v.binding.format);
            const auto element = format == DXGI_FORMAT_R16_UINT ? 2u : format == DXGI_FORMAT_R32_UINT ? 4u : 0u;
            return v.buffer && (v.identity.buffer.BindFlags & D3D11_BIND_INDEX_BUFFER) && element && !(v.binding.offset % element)
                ? true : Fail(Dx11ExecutionError::IndexBounds);
        }
        case TargetKind::PixelTexture: case TargetKind::VertexTexture:
            return v.srv && v.binding.view ? true : Fail(Dx11ExecutionError::Binding);
        default: return Fail(Dx11ExecutionError::Unsupported);
        }
    }
    bool RasterAvailable(const Dx11ExecutionRequest& r, bool indexed, bool instanced = false) noexcept
    {
        if (instanced)
            return indexed && r.originalInstanced && r.owner->SupportsIndexedInstanced() ? true : Fail(Dx11ExecutionError::Raster);
        if ((indexed ? !r.original : !r.originalDraw) || !r.owner->SupportsRaster(indexed))
            return Fail(Dx11ExecutionError::Raster);
        return true;
    }
    bool CaptureBindings(const Dx11ExecutionRequest& r, const BatchView& batch) noexcept
    {
        std::uint32_t vertexMask = r.pipeline.layout->Slots();
        std::array<std::uint64_t, 2> pixelMask{}, materialMask{}, vertexTextureMask{};
        bool index = false;
        std::uint32_t constantMask = 0;
        unsigned boundaries = 0;
        for (unsigned n = 0; n < batch.count; ++n)
        {
            failedAction = n;
            const auto& a = batch.actions[n];
            switch (a.op)
            {
            case ActionOp::Bind:
                if (parameters && (a.destination.kind == TargetKind::PixelTexture || a.destination.kind == TargetKind::VertexTexture) &&
                    a.destination.index == IniParameterRegister)
                    return Fail(Dx11ExecutionError::UnsupportedParameter);
                if (a.destination.kind == TargetKind::VertexBuffer && a.destination.index < VertexSlots)
                    vertexMask |= 1u << a.destination.index;
                else if (a.destination.kind == TargetKind::IndexBuffer && a.destination.index == 0) index = true;
                else if (a.destination.kind == TargetKind::VertexTexture && a.destination.index < PixelSlots)
                {
                    if (!Bit(vertexTextureMask, a.destination.index)) vertexTextureSlots[vertexTextureSlotCount++] = a.destination.index;
                    SetBit(vertexTextureMask, a.destination.index);
                }
                else if (a.destination.kind == TargetKind::PixelTexture && a.destination.index < PixelSlots)
                {
                    if (!Bit(materialMask, a.destination.index)) materialSlots[materialSlotCount++] = a.destination.index;
                    SetBit(materialMask, a.destination.index); SetBit(pixelMask, a.destination.index);
                }
                else return Fail(Dx11ExecutionError::Unsupported);
                break;
            case ActionOp::UnbindVertexConstant:
                // The whole tape is checked before the first setter. Nested
                // post-run callee PRE has a logical PRE tag but occurs after
                // this authentic boundary, which is what owns the cleanup.
                if (boundaries != 1 || a.constantSlot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT ||
                    !(plan->Touches().vertexConstantNulls & (std::uint32_t{1} << a.constantSlot)))
                    return Fail(Dx11ExecutionError::Tape);
                constantMask |= std::uint32_t{1} << a.constantSlot;
                break;
            case ActionOp::OriginalDrawBoundary:
                if (++boundaries != 1 || a.skipOriginal != batch.skipOriginal) return Fail(Dx11ExecutionError::Tape);
                if (!a.skipOriginal)
                {
                    if (!RasterAvailable(r, r.draw.drawIndexed, r.draw.instanced)) return false;
                    index |= r.draw.drawIndexed;
                }
                break;
            case ActionOp::DrawIndexedInstanced:
                if (!RasterAvailable(r, true, true)) return false;
                index = true; break;
            case ActionOp::DrawIndexed:
                if (!RasterAvailable(r, true)) return false;
                index = true; break;
            case ActionOp::Draw:
                if (!RasterAvailable(r, false)) return false;
                break;
            case ActionOp::DebugOverlay:
                if (!plan->Touches().debugOverlay || !RasterAvailable(r, false)) return Fail(Dx11ExecutionError::Tape);
                // Preserve every reader slot, including if a future caller
                // reaches the action with a known-null command binding.
                for (unsigned slot = 20; slot < 24; ++slot) SetBit(pixelMask, slot);
                outputTape = true; break;
            case ActionOp::BeginRasterCustom:
            case ActionOp::EndRasterCustom:
                if (!plan->Touches().rasterCustom || !rasterOwner || !rasterOwner->Raster(a.raster)) return Fail(Dx11ExecutionError::Tape);
                outputTape = true; break;
            case ActionOp::BindOutput:
                if (!plan->Touches().outputSetters) return Fail(Dx11ExecutionError::Tape);
                outputTape = true; break;
            case ActionOp::CopyDescription:
                if (!plan->Touches().outputDescription || a.destination.kind != TargetKind::Resource ||
                    a.destination.index >= plan->Resources().size() || plan->Resources()[a.destination.index].kind != ResourceKind::Alias)
                    return Fail(Dx11ExecutionError::Tape);
                break;
            case ActionOp::ClearTarget:
                if (!plan->Touches().clearTargets) return Fail(Dx11ExecutionError::Tape);
                break;
            case ActionOp::CopyDepth:
                if (!plan->Touches().depthCopy || !SameBinding(a.previous, queriedOutputs[8].binding))
                    return Fail(Dx11ExecutionError::Tape);
                break;
            case ActionOp::SetResource:
                if (a.destination.kind != TargetKind::Resource || a.destination.index >= plan->Resources().size())
                    return Fail(Dx11ExecutionError::Tape);
                break;
            case ActionOp::SetParameter:
                if (!parameters || a.parameter >= IniParameterScalars) return Fail(Dx11ExecutionError::UnsupportedParameter);
                break;
            default: return Fail(Dx11ExecutionError::Tape);
            }
        }
        if (boundaries != 1) return Fail(Dx11ExecutionError::Tape);
        if (pipeline.pixelReplacement && pipeline.pixelReplacement->OutlineResources())
        {
            SetBit(pixelMask, 69); SetBit(pixelMask, 70);
        }
        if (pipeline.pixelReplacement && pipeline.pixelReplacement->DiffuseResources()) SetBit(pixelMask, 69);
        if (plan->Touches().rasterCustom || plan->Touches().debugOverlay)
        {
            r.context->RSGetState(saved.raster.GetAddressOf());
            if (saved.raster && !OnDevice(saved.raster.Get(), r.device)) return Fail(Dx11ExecutionError::WrongOwner);
            simulatedRaster = saved.raster.Get();
        }
        if (parameters)
        {
            SetBit(pixelMask, IniParameterRegister);
            r.context->VSGetShaderResources(IniParameterRegister, 1, saved.iniVertex.GetAddressOf());
        }
        if (constantMask)
        {
            saved.constantRanges = r.pipeline.vertexRangeContext;
            if (saved.constantRanges && !OnDevice(saved.constantRanges.Get(), r.device))
                return Fail(Dx11ExecutionError::WrongOwner);
            for (unsigned n = 0; n < saved.vertexConstants.size(); ++n) if (constantMask & (1u << n))
            {
                saved.constantCaptured |= 1u << n;
                if (saved.constantRanges)
                    saved.constantRanges->VSGetConstantBuffers1(n, 1, saved.vertexConstants[n].GetAddressOf(),
                        &saved.constantFirst[n], &saved.constantCount[n]);
                else r.context->VSGetConstantBuffers(n, 1, saved.vertexConstants[n].GetAddressOf());
                if (saved.vertexConstants[n] && !OnDevice(saved.vertexConstants[n].Get(), r.device))
                    return Fail(Dx11ExecutionError::WrongOwner);
            }
        }
        // Capture every actually touched IA/VS/PS slot BEFORE any setter, plus
        // layout-required initial VBs and the IB needed for bounds validation.
        for (unsigned n = 0; n < VertexSlots; ++n) if (vertexMask & (1u << n))
            CaptureVertex(r.context, n);
        if (index) CaptureIndex(r.context);
        for (unsigned n = 0; n < PixelSlots; ++n)
        {
            if (Bit(pixelMask, n)) CapturePixel(r.context, n);
            if (Bit(vertexTextureMask, n)) CaptureVertexTexture(r.context, n);
        }
        return true;
    }
    bool CaptureOverlay(const Dx11ExecutionRequest& r) noexcept
    {
        if (overlayCaptured) return true;
        overlayLibrary = r.statics->DebugOverlay();
        if (!parameters || !overlayLibrary || overlayLibrary->plan != plan ||
            !OnDevice(overlayLibrary->raster.Get(), r.device) || !OnDevice(overlayLibrary->blend.Get(), r.device) ||
            streams.mask || pipeline.geometryShader) return Fail(Dx11ExecutionError::Unsupported);
        UINT classes = 0;
        r.context->CSGetShader(overlayCompute.GetAddressOf(), nullptr, &classes);
        if (classes || (overlayCompute && !OnDevice(overlayCompute.Get(), r.device))) return Fail(Dx11ExecutionError::Unsupported);
        const UINT count = r.device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? UavSlots11_1 : D3D11_PS_CS_UAV_REGISTER_COUNT;
        std::array<ID3D11UnorderedAccessView*, UavSlots11_1> unordered{};
        r.context->CSGetUnorderedAccessViews(0, count, unordered.data());
        bool empty = true;
        for (UINT i = 0; i < count; ++i) if (unordered[i]) { empty = false; unordered[i]->Release(); }
        if (!empty) return Fail(Dx11ExecutionError::Unsupported); // no UAV/counter ABI added
        r.context->PSGetSamplers(0, 1, overlaySampler.GetAddressOf());
        if (overlaySampler)
        {
            if (!OnDevice(overlaySampler.Get(), r.device)) return Fail(Dx11ExecutionError::PixelSampler);
            D3D11_SAMPLER_DESC desc{}; overlaySampler->GetDesc(&desc);
            if (!StandardSampleFilter(desc.Filter)) return Fail(Dx11ExecutionError::PixelSampler);
        }
        // A captured API null stays null; never create or bind a default.
        r.context->OMGetBlendState(overlayBlend.GetAddressOf(), overlayBlendFactor.data(), &overlaySampleMask);
        if (overlayBlend && !OnDevice(overlayBlend.Get(), r.device)) return Fail(Dx11ExecutionError::WrongOwner);
        overlayCaptured = true; return true;
    }
    bool PrepareOverlay(const Dx11ExecutionRequest& r, PreparedAction& prepared) noexcept
    {
        const auto& a = prepared.action;
        if (!CaptureOverlay(r) || a.debugOverlay >= overlayLibrary->stages.size()) return Fail(Dx11ExecutionError::Tape);
        const auto& stages = overlayLibrary->stages[a.debugOverlay];
        if (!OnDevice(stages.vs.Get(), r.device) || !OnDevice(stages.ps.Get(), r.device)) return Fail(Dx11ExecutionError::WrongOwner);
        float pass = 0;
        if (!parameters->ReadParameter(finalParameters, 70 * 4, pass) || (pass != 1.0f && pass != 2.0f))
            return Fail(Dx11ExecutionError::UnsupportedParameter);
        prepared.parameters = finalParameters; // exact preloaded t120 page at this reader
        prepared.raster = simulatedRaster; // restore CALL-entry scope, not outer frame RS
        if (!parameters->ParameterView(prepared.parameters) || !simulatedOutputs[0].rtv)
            return Fail(Dx11ExecutionError::Binding);
        for (unsigned i = 0; i < a.overlayBindings.size(); ++i)
        {
            auto& index = prepared.overlayResources[i];
            if (!Resolve(r, a.overlayBindings[i], index) || !Bindable(index, {TargetKind::PixelTexture, 20 + i})) return false;
            // Shader reads exactly the same four leases that the command tape
            // physically binds. No snapshot from a producer/previous pass.
            if (simulatedPixels[20 + i] == NoResource ||
                !SameBinding(resources[simulatedPixels[20 + i]].value.binding, a.overlayBindings[i]))
                return Fail(Dx11ExecutionError::Tape);
            const auto& kept = resources[index];
            if (!kept.value.binding.object) continue; // original null-SRV reads remain legal
            const auto& target = kept.ownedTexture;
            if (!target || kept.game.get() != target.get() || !kept.value.sampleableFloat2D ||
                !ValidatePrivateTexture(*target, r.device) || !TextureReady(index) || OutputConflict(kept.value.resource) ||
                (i == 2 ? target->purpose != OwnedTexturePurpose::DepthCopy :
                    target->purpose != OwnedTexturePurpose::OutputDescription) ||
                (i < 2 ? target->source.view.depth : !target->source.view.depth))
                return Fail(Dx11ExecutionError::PixelMaterial);
            D3D11_SHADER_RESOURCE_VIEW_DESC view{}; kept.value.srv->GetDesc(&view);
            if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || view.Texture2D.MostDetailedMip || view.Texture2D.MipLevels != 1)
                return Fail(Dx11ExecutionError::Binding);
        }
        const auto& color = resources[prepared.overlayResources[0]];
        const auto& rt6 = resources[prepared.overlayResources[1]];
        if (color.ownedTexture && rt6.ownedTexture &&
            (color.value.resource == rt6.value.resource || color.ownedTexture->alias == rt6.ownedTexture->alias))
            return Fail(Dx11ExecutionError::Hazard); // Fakeo1 and Fakeo6 are independent
        return true;
    }
    void BeginOverlay(ID3D11DeviceContext* c, const PreparedAction& prepared) const noexcept
    {
        const auto& stages = overlayLibrary->stages[prepared.action.debugOverlay];
        c->VSSetShader(stages.vs.Get(), nullptr, 0);
        c->HSSetShader(nullptr, nullptr, 0); c->DSSetShader(nullptr, nullptr, 0);
        c->GSSetShader(nullptr, nullptr, 0); c->CSSetShader(nullptr, nullptr, 0);
        c->PSSetShader(stages.ps.Get(), nullptr, 0);
        c->IASetInputLayout(nullptr); // VS consumes only SV_VertexID
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const FLOAT factor[4]{};
        c->OMSetBlendState(overlayLibrary->blend.Get(), factor, 0xffffffffu);
        c->RSSetState(overlayLibrary->raster.Get());
        // t20..23/t120 already physically bound by the prevalidated tape.
        // No s0/depth/viewport/CB/VB/IB setter: original inheritance is exact.
    }
    void EndOverlay(ID3D11DeviceContext* c, const PreparedAction& prepared) const noexcept
    {
        c->VSSetShader(saved.vertexShaderChanged ? pipeline.vertexReplacement->Shader() : saved.vertexShader.Get(), nullptr, 0);
        c->HSSetShader(nullptr, nullptr, 0); c->DSSetShader(nullptr, nullptr, 0);
        c->GSSetShader(saved.geometryShader.Get(), nullptr, 0);
        c->CSSetShader(overlayCompute.Get(), nullptr, 0);
        c->PSSetShader(saved.pixelShaderChanged ? pipeline.pixelReplacement->Shader() : saved.pixelShader.Get(), nullptr, 0);
        c->IASetInputLayout(saved.layout.Get()); c->IASetPrimitiveTopology(saved.topology);
        c->OMSetBlendState(overlayBlend.Get(), overlayBlendFactor.data(), overlaySampleMask);
        c->RSSetState(prepared.raster);
    }
    bool InitialVertex(const Dx11ExecutionRequest& r, unsigned slot) noexcept
    {
        if (simulatedVertices[slot] != NoResource) return true;
        Binding binding;
        if (!r.resources->CurrentBinding({TargetKind::VertexBuffer, slot}, binding) ||
            !Resolve(r, binding, simulatedVertices[slot])) return Fail(Dx11ExecutionError::Binding);
        const auto& view = resources[simulatedVertices[slot]].value;
        if (view.buffer != saved.vertices[slot].Get() || binding.stride != saved.strides[slot] || binding.offset != saved.offsets[slot])
            return Fail(Dx11ExecutionError::Generation);
        return Bindable(simulatedVertices[slot], {TargetKind::VertexBuffer, slot});
    }
    bool InitialIndex(const Dx11ExecutionRequest& r) noexcept
    {
        if (simulatedIndex != NoResource) return true;
        Binding binding;
        if (!r.resources->CurrentBinding({TargetKind::IndexBuffer, 0}, binding) ||
            !Resolve(r, binding, simulatedIndex)) return Fail(Dx11ExecutionError::Binding);
        const auto& view = resources[simulatedIndex].value;
        if (view.buffer != saved.index.Get() || binding.format != static_cast<UINT>(saved.indexFormat) || binding.offset != saved.indexOffset)
            return Fail(Dx11ExecutionError::Generation);
        return Bindable(simulatedIndex, {TargetKind::IndexBuffer, 0});
    }
    bool Indexed(const Dx11ExecutionRequest& r, UINT count, UINT first, INT baseVertex) noexcept
    {
        if (!count) return true; // legal no-op still issued at its real boundary
        if (!InitialIndex(r)) return false;
        const auto& ib = resources[simulatedIndex].value;
        const auto& binding = ib.binding;
        if (!ib.buffer) return Fail(Dx11ExecutionError::IndexBounds);
        const auto format = static_cast<DXGI_FORMAT>(binding.format);
        const auto element = format == DXGI_FORMAT_R16_UINT ? 2u : format == DXGI_FORMAT_R32_UINT ? 4u : 0u;
        if (!element || (std::uint64_t(first) + count) * element + binding.offset > binding.byteSize)
            return Fail(Dx11ExecutionError::IndexBounds);
        IndexWindow window;
        if (!r.resources->Indices(ib, format, binding.offset, first, count, window) || !window.known ||
            window.object != binding.object || window.contentGeneration != ib.identity.contentGeneration ||
            window.format != format || window.offset != binding.offset || window.first != first || window.count != count ||
            window.maximum < window.minimum || (element == 2 && window.maximum > UINT16_MAX))
            return Fail(Dx11ExecutionError::IndexBounds);
        const auto minimum = std::int64_t(window.minimum) + baseVertex;
        const auto maximum = std::int64_t(window.maximum) + baseVertex;
        if (minimum < 0 || maximum < minimum || maximum > UINT32_MAX) return Fail(Dx11ExecutionError::VertexBounds);
        return Vertices(r, static_cast<std::uint64_t>(maximum));
    }
    bool AutoIndices(const Dx11ExecutionRequest& r, Action& a, bool& skipRaster) noexcept
    {
        if (!a.autoIndices) return true;
        if (!InitialIndex(r) || simulatedIndex == NoResource) return false;
        const auto& v = resources[simulatedIndex].value;
        const unsigned element = v.binding.format == DXGI_FORMAT_R16_UINT ? 2u : v.binding.format == DXGI_FORMAT_R32_UINT ? 4u : 0u;
        if (!v.buffer || !element || v.binding.offset > v.binding.byteSize || v.binding.offset % element)
            return Fail(Dx11ExecutionError::IndexBounds);
        a.count = (v.binding.byteSize - v.binding.offset) / element; a.first = 0; a.baseVertex = 0;
        a.autoIndices = false; skipRaster = !a.count;
        return true; // Indexed below still needs the exact CPU-preloaded min/max range.
    }
    bool NonIndexed(const Dx11ExecutionRequest& r, UINT count, UINT first) noexcept
    {
        if (!count) return true; // no IB exists/needed for this physical call
        const auto maximum = std::uint64_t(first) + count - 1;
        if (maximum > UINT32_MAX) return Fail(Dx11ExecutionError::VertexBounds);
        return Vertices(r, maximum);
    }
    bool Vertices(const Dx11ExecutionRequest& r, std::uint64_t maximum) noexcept
    {
        const auto& layout = *r.pipeline.layout;
        for (unsigned slot = 0; slot < GimiVertexSlots; ++slot) if (layout.Slots() & (1u << slot))
        {
            if (!InitialVertex(r, slot)) return false;
            const auto& vb = resources[simulatedVertices[slot]].value;
            if (!vb.buffer || vb.binding.stride != layout.Strides()[slot] ||
                std::uint64_t(vb.binding.offset) + std::uint64_t(maximum) * vb.binding.stride + layout.Ends()[slot] > vb.binding.byteSize)
                return Fail(Dx11ExecutionError::VertexBounds);
        }
        return true;
    }
    bool ImmutableSampledDds(const Dx11ExecutionRequest& r, std::uint32_t index) const noexcept
    {
        const auto& retained = resources[index]; const auto& view = retained.value;
        if (!retained.statics || retained.statics.get() != r.statics.get() || retained.game ||
            !retained.stableReadOnly || !view.sampleableFloat2D || !view.srv || !view.binding.view ||
            view.identity.kind != IdentityKind::Texture2D || !view.identity.contentKnown ||
            view.identity.origin != ResourceOrigin::Custom) return false;
        const auto& desc = view.identity.texture;
        if (desc.Usage != D3D11_USAGE_IMMUTABLE || desc.BindFlags != D3D11_BIND_SHADER_RESOURCE ||
            desc.CPUAccessFlags || desc.MiscFlags || !desc.Width || !desc.Height || !desc.MipLevels ||
            desc.ArraySize != 1 || desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{}; view.srv->GetDesc(&srv);
        // Resolve already compared the actual Texture2D descriptor, view->resource,
        // device/lease generations and write hazards; its packet entry owns them.
        return srv.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && srv.Format == desc.Format &&
            static_cast<UINT>(srv.Format) == view.binding.format && !srv.Texture2D.MostDetailedMip &&
            srv.Texture2D.MipLevels == desc.MipLevels;
    }
    bool CapturePixelSamplers(const Dx11ExecutionRequest& r, const Dx11PixelInputs& inputs) noexcept
    {
        if (pixelSamplersCaptured) return true;
        // Reflection provides no texture-to-sampler pairing. Retain every real
        // original PS sampler and refuse comparison/reduction in this narrow
        // color-DDS path rather than invent a relationship or sampler preset.
        if (inputs.comparisonSamplers) return Fail(Dx11ExecutionError::PixelSampler);
        for (unsigned slot = 0; slot < pixelSamplers.size(); ++slot) if (inputs.samplers & (1u << slot))
        {
            pixelSamplerMask |= 1u << slot;
            auto& sampler = pixelSamplers[slot];
            r.context->PSGetSamplers(slot, 1, sampler.GetAddressOf());
            if (!sampler) continue; // authentic API null is retained as null
            if (!OnDevice(sampler.Get(), r.device)) return Fail(Dx11ExecutionError::PixelSampler);
            D3D11_SAMPLER_DESC desc{}; sampler->GetDesc(&desc);
            if (!StandardSampleFilter(desc.Filter)) return Fail(Dx11ExecutionError::PixelSampler);
        }
        pixelSamplersCaptured = true;
        return true;
    }
    bool VertexTextureMaterials(const Dx11ExecutionRequest& r) noexcept
    {
        // The ORIGINAL shader reflection remains authoritative. A stage-qualified
        // reference grants transport/ownership, not an invented shader ABI.
        const auto* inputs = pipeline.vertexInputs;
        for (unsigned n = 0; n < vertexTextureSlotCount; ++n)
        {
            const auto slot = vertexTextureSlots[n]; const auto index = simulatedVertexTextures[slot];
            if (index == NoResource || index == qualifiedVertexTextures[slot]) continue;
            const auto& retained = resources[index]; const auto& value = retained.value;
            if (!value.binding.object) continue; // exact API null, no fabricated image
            if (!inputs || !inputs->qualified) return Fail(Dx11ExecutionError::Binding);
            const auto shape = inputs->resources[slot];
            if (shape.kind == Dx11VertexReadKind::None) continue; // transport only
            if (value.identity.origin == ResourceOrigin::Game)
            {
                // The earlier CaptureShaderInputs retained/proved this exact
                // original reader. Backup/restore must not promote a different
                // game resource or unknown contents into a new VS consumer.
                if (value.srv != saved.vertexTextures[slot].Get()) return Fail(Dx11ExecutionError::Binding);
            }
            else
            {
                if (shape.kind != Dx11VertexReadKind::Texture2D || shape.returns != D3D_RETURN_TYPE_FLOAT ||
                    !(retained.ownedTexture ? value.sampleableFloat2D : ImmutableSampledDds(r, index)))
                    return Fail(Dx11ExecutionError::Binding);
                // Reflection has no texture-to-sampler pairing: retain all
                // originals and reject comparison/reduction filters, as for PS.
                for (unsigned s = 0; s < vertexInputs.samplers.size(); ++s)
                    if (inputs->samplers & (1u << s))
                    {
                        auto* sampler = vertexInputs.samplers[s].Get();
                        if (!sampler) return Fail(Dx11ExecutionError::Binding);
                        D3D11_SAMPLER_DESC desc{}; sampler->GetDesc(&desc);
                        if (!StandardSampleFilter(desc.Filter)) return Fail(Dx11ExecutionError::Unsupported);
                    }
                // No sampler, upload, shader or cursor texture is synthesized.
            }
            qualifiedVertexTextures[slot] = index;
        }
        return true;
    }
    bool PixelMaterials(const Dx11ExecutionRequest& r) noexcept
    {
        // Iterate only command-touched slots, in their actual state at THIS
        // physical draw. An overwritten PRE bind or POST-only alias is not a
        // material consumer. Original PS resources are not guessed from names.
        for (unsigned n = 0; n < materialSlotCount; ++n)
        {
            const auto slot = materialSlots[n]; const auto index = simulatedPixels[slot];
            if (index == NoResource || index == qualifiedPixels[slot]) continue;
            const auto& retained = resources[index];
            if (!retained.value.binding.object || (!retained.statics && !retained.ownedTexture)) continue;
            // Added Outline slots have their own exact code contract, not a
            // fabricated entry in the ORIGINAL PS reflection resource table.
            if (pipeline.pixelReplacement && ((pipeline.pixelReplacement->OutlineResources() && (slot == 69 || slot == 70)) ||
                (pipeline.pixelReplacement->DiffuseResources() && slot == 69))) continue;
            const auto* inputs = pipeline.pixelInputs;
            if (!pipeline.pixelShader) continue; // no physical PS consumer
            if (!inputs || !inputs->qualified) return Fail(Dx11ExecutionError::PixelMaterial);
            const auto& shape = inputs->resources[slot];
            if (!shape.declared) continue; // binding transport only, no consumer claim
            if (shape.type != D3D_SIT_TEXTURE || shape.dimension != D3D_SRV_DIMENSION_TEXTURE2D ||
                shape.returns != D3D_RETURN_TYPE_FLOAT ||
                !(retained.ownedTexture ? retained.value.sampleableFloat2D : ImmutableSampledDds(r, index)))
                return Fail(Dx11ExecutionError::PixelMaterial);
            if (!CapturePixelSamplers(r, *inputs)) return false;
            qualifiedPixels[slot] = index; // exact immutable lease; no repeated descriptor work
        }
        return true;
    }
    bool OutlineMaterial(const Dx11ExecutionRequest& r) noexcept
    {
        if (!pipeline.pixelReplacement || !pipeline.pixelReplacement->OutlineResources()) return true;
        for (unsigned n = 0; n < simulatedOutline.size(); ++n)
        {
            const unsigned slot = 69 + n;
            auto& index = simulatedOutline[n];
            if (index == NoResource)
            {
                Binding binding;
                if (!r.resources->CurrentBinding({TargetKind::PixelTexture, slot}, binding) ||
                    !Resolve(r, binding, index) || resources[index].value.srv != saved.pixels[slot].Get())
                    return Fail(Dx11ExecutionError::OutlineMaterial);
            }
            const auto& retained = resources[index]; const auto& view = retained.value;
            // D3D11 resinfo on an unbound t# returns zero dimensions. The
            // exact template then skips sampling and retains its t120-driven
            // threshold/discard and ini69.w fallback behavior. Null is valid;
            // it does not certify optional material sampling.
            if (!view.binding.object) continue;
            // Game-produced/dynamic views still need a separate reader contract.
            if (!ImmutableSampledDds(r, index)) return Fail(Dx11ExecutionError::OutlineMaterial);
            // Only a nonnull material can reach the template's sample blocks.
            // Do not impose sampler-filter qualification on its all-null path.
            if (saved.outlineSampler && !StandardSampleFilter(saved.outlineSamplerDesc.Filter))
                return Fail(Dx11ExecutionError::OutlineSampler);
        }
        return true;
    }
    bool DiffuseMaterial(const Dx11ExecutionRequest& r) noexcept
    {
        if (!pipeline.pixelReplacement || !pipeline.pixelReplacement->DiffuseResources()) return true;
        auto& index = simulatedOutline[0];
        if (index == NoResource)
        {
            Binding binding;
            if (!r.resources->CurrentBinding({TargetKind::PixelTexture, 69}, binding) || !Resolve(r, binding, index) ||
                resources[index].value.srv != saved.pixels[69].Get()) return Fail(Dx11ExecutionError::PixelMaterial);
        }
        const auto& retained = resources[index];
        if (!retained.value.binding.object) return true; // real resinfo guard, no sample claim
        if (OutputConflict(retained.value.resource) || !TextureReady(index) ||
            !(retained.ownedTexture ? retained.value.sampleableFloat2D : ImmutableSampledDds(r, index)) ||
            !pipeline.pixelInputs || !pipeline.pixelInputs->qualified || !CapturePixelSamplers(r, *pipeline.pixelInputs))
            return Fail(Dx11ExecutionError::PixelMaterial);
        return true; // sampler is the captured ORIGINAL declared slot, not s15 guessed from Outline
    }
    bool PrepareStreams(const Dx11ExecutionRequest& r, UINT count, bool indexed,
                        std::array<std::uint64_t, D3D11_SO_BUFFER_SLOT_COUNT>& bytes) noexcept
    {
        if (!streams.mask || !count) return true;
        const auto* contract = r.pipeline.streamContract;
        if (indexed || !contract || !contract->qualified || !contract->maxVertices || !contract->instances)
            return Fail(Dx11ExecutionError::Unsupported);
        const auto maximum = std::uint64_t(count) * contract->maxVertices * contract->instances;
        for (UINT slot = 0; slot < streams.targets.size(); ++slot) if (streams.mask & (1u << slot))
        {
            const auto& target = streams.targets[slot]; const auto stride = contract->strides[slot];
            if (!stride || target.cursorUpper > target.descriptor.ByteWidth ||
                streamPlanned[slot] > target.descriptor.ByteWidth - target.cursorUpper ||
                maximum > (target.descriptor.ByteWidth - target.cursorUpper - streamPlanned[slot]) / stride)
                return Fail(Dx11ExecutionError::VertexBounds);
            bytes[slot] = maximum * stride; streamPlanned[slot] += bytes[slot];
        }
        return true;
    }
    bool PrepareTape(const Dx11ExecutionRequest& r, const BatchView& batch) noexcept
    {
        if (!batch.includesDrawBoundary || batch.phase != Phase::Pre || batch.plan != plan.get() || !batch.actions ||
            !batch.count || batch.count > tape.size()) return Fail(Dx11ExecutionError::Tape);
        if (!CaptureBindings(r, batch) || !InitialOutputs(r)) return false;
        if (outputTape || plan->Touches().clearTargets || plan->Touches().depthCopy)
        {
            ComPtr<ID3D11Predicate> predicate; BOOL predicateValue = FALSE;
            r.context->GetPredication(&predicate, &predicateValue);
            if (predicate) return Fail(Dx11ExecutionError::Unsupported); // no conditional initialization receipt
        }
        tapeCount = batch.count;
        for (unsigned n = 0; n < batch.count; ++n)
        {
            failedAction = n;
            auto& prepared = tape[n]; prepared = {}; prepared.resource = NoResource; prepared.action = batch.actions[n];
            auto& a = prepared.action;
            switch (a.op)
            {
            case ActionOp::DebugOverlay:
                if (!PrepareOverlay(r, prepared)) return false;
                break;
            case ActionOp::BeginRasterCustom:
                if (rasterDepth == rasterScopes.size()) return Fail(Dx11ExecutionError::Capacity);
                rasterScopes[rasterDepth++] = {simulatedOutputs, simulatedRaster};
                simulatedRaster = rasterOwner->Raster(a.raster); prepared.raster = simulatedRaster;
                break;
            case ActionOp::EndRasterCustom:
                if (!rasterDepth) return Fail(Dx11ExecutionError::Tape);
                --rasterDepth;
                simulatedOutputs = rasterScopes[rasterDepth].outputs;
                simulatedRaster = rasterScopes[rasterDepth].raster; rasterScopes[rasterDepth] = {};
                prepared.raster = simulatedRaster;
                for (const auto& output : simulatedOutputs)
                    if (CommandReadConflict(output.texture)) return Fail(Dx11ExecutionError::Hazard);
                if (!PackOutputs(prepared)) return false;
                break;
            case ActionOp::UnbindVertexConstant:
                if (!(saved.constantCaptured & (std::uint32_t{1} << a.constantSlot)))
                    return Fail(Dx11ExecutionError::Tape);
                simulatedConstantNulls |= std::uint32_t{1} << a.constantSlot;
                break;
            case ActionOp::Bind:
                if (!Resolve(r, a.binding, prepared.resource) || !Bindable(prepared.resource, a.destination)) return false;
                for (unsigned i = 0; i < depthCount; ++i)
                    if (preparedDepth[i] == prepared.resource && !copiedDepth[i]) return Fail(Dx11ExecutionError::Tape);
                if (a.destination.kind == TargetKind::VertexBuffer) simulatedVertices[a.destination.index] = prepared.resource;
                else if (a.destination.kind == TargetKind::IndexBuffer) simulatedIndex = prepared.resource;
                else if (a.destination.kind == TargetKind::VertexTexture)
                    simulatedVertexTextures[a.destination.index] = prepared.resource;
                else if (a.destination.kind == TargetKind::PixelTexture)
                {
                    simulatedPixels[a.destination.index] = prepared.resource;
                    if (a.destination.index == 69 || a.destination.index == 70)
                        simulatedOutline[a.destination.index - 69] = prepared.resource;
                }
                break;
            case ActionOp::BindOutput:
                if (!PrepareOutput(r, prepared)) return false;
                break;
            case ActionOp::CopyDescription:
            {
                if (!Resolve(r, a.binding, prepared.resource)) return false;
                const auto& target = resources[prepared.resource].ownedTexture;
                if (!target || target->purpose != OwnedTexturePurpose::OutputDescription || target->alias != a.destination.index ||
                    !SameBinding(target->source.binding, a.previous) || !ValidatePrivateTexture(*target, r.device))
                    return Fail(Dx11ExecutionError::Binding);
                // No GPU copy and no initialized-content claim. SetResource
                // below owns this descriptor through the existing journal.
                break;
            }
            case ActionOp::ClearTarget:
            {
                if (!Resolve(r, a.binding, prepared.resource)) return false;
                const auto& target = resources[prepared.resource].ownedTexture;
                if (!target || target->purpose != OwnedTexturePurpose::OutputDescription ||
                    (!target->rtv && !target->dsv) || !ValidatePrivateTexture(*target, r.device))
                    return Fail(Dx11ExecutionError::Binding);
                // Private objects remain within this owning packet. At this
                // tape position a clear cannot race an active simulated VS/PS reader.
                if (CommandReadConflict(target->texture.Get())) return Fail(Dx11ExecutionError::Hazard);
                if (!WroteTexture(prepared.resource)) return false;
                break;
            }
            case ActionOp::CopyDepth:
            {
                if (!Resolve(r, a.binding, prepared.resource)) return false;
                const auto& retained = resources[prepared.resource];
                const auto& depth = retained.ownedTexture;
                D3D11_TEXTURE2D_DESC expected{}; D3D11_SHADER_RESOURCE_VIEW_DESC view{}; UINT texel = 0;
                if (!depth || !SameBinding(depth->source.binding, a.previous) ||
                    depth->source.owner != queriedOutputs[8].owner ||
                    !DepthCopyShape(queriedOutputs[8], expected, view, texel)) return Fail(Dx11ExecutionError::Binding);
                D3D11_TEXTURE2D_DESC actual{}; depth->texture->GetDesc(&actual);
                D3D11_SHADER_RESOURCE_VIEW_DESC srv{}; depth->srv->GetDesc(&srv);
                if (!TextureDescriptor(actual, expected) || srv.Format != view.Format ||
                    srv.ViewDimension != view.ViewDimension || srv.Texture2D.MostDetailedMip || srv.Texture2D.MipLevels != 1)
                    return Fail(Dx11ExecutionError::Binding);
                bool found = false;
                for (unsigned i = 0; i < depthCount; ++i) if (preparedDepth[i] == prepared.resource)
                { if (copiedDepth[i]) return Fail(Dx11ExecutionError::Tape); copiedDepth[i] = found = true; }
                if (!found || !WroteTexture(prepared.resource)) return Fail(Dx11ExecutionError::Tape);
                break;
            }
            case ActionOp::SetParameter:
                if (!parameters || !parameters->WriteParameter(finalParameters, a.parameter, a.value, prepared.parameters) ||
                    !parameters->ParameterView(prepared.parameters)) return Fail(Dx11ExecutionError::UnsupportedParameter);
                finalParameters = prepared.parameters;
                break;
            case ActionOp::SetResource:
            {
                std::uint32_t previous, next;
                if (!Resolve(r, a.previous, previous, nullptr, true) || !Resolve(r, a.binding, next, nullptr, true)) return false;
                for (unsigned i = 0; i < depthCount; ++i)
                    if (preparedDepth[i] == next && !copiedDepth[i]) return Fail(Dx11ExecutionError::Tape);
                if (aliasCount == aliases.size()) return Fail(Dx11ExecutionError::Capacity);
                // CommandState already applied alias semantics transactionally.
                // This is ownership journaling, never a second alias interpreter.
                aliases[aliasCount++] = {a.destination.index, resources[previous], resources[next]};
                break;
            }
            case ActionOp::DrawIndexedInstanced:
                // Per-instance IA layouts are still rejected by the genuine
                // captured layout contract. This path never fabricates one.
                if (std::uint64_t(a.firstInstance) + a.instances > std::uint64_t(UINT32_MAX) + 1 || streams.mask)
                    return Fail(Dx11ExecutionError::Unsupported);
                [[fallthrough]];
            case ActionOp::DrawIndexed:
                if (!AutoIndices(r, a, prepared.skipRaster) || !Indexed(r, a.count, a.first, a.baseVertex)) return false;
                break;
            case ActionOp::Draw:
                if (!NonIndexed(r, a.count, a.first)) return false;
                break;
            case ActionOp::OriginalDrawBoundary:
                if (!a.skipOriginal && !(r.draw.drawIndexed ?
                    Indexed(r, r.draw.indexCount, r.draw.firstIndex, r.draw.baseVertex) :
                    NonIndexed(r, r.draw.vertexCount, r.draw.firstVertex))) return false;
                break;
            default: return Fail(Dx11ExecutionError::UnsupportedParameter); // caught in full first pass
            }
            const bool original = a.op == ActionOp::OriginalDrawBoundary;
            if (!prepared.skipRaster && (a.op == ActionOp::Draw || a.op == ActionOp::DrawIndexed || a.op == ActionOp::DrawIndexedInstanced || (original && !a.skipOriginal)))
            {
                // Original nonnull CB input qualification cannot prove a
                // subsequent reader of a slot explicitly nulled by this tape.
                // The actual cleanup path has no such reader; unknown custom
                // post draws stay rejected before any GPU effect.
                if (simulatedConstantNulls)
                {
                    if (!pipeline.vertexInputs) return Fail(Dx11ExecutionError::Unsupported);
                    for (unsigned slot = 0; slot < pipeline.vertexInputs->constantBytes.size(); ++slot)
                        if ((simulatedConstantNulls & (1u << slot)) && (pipeline.vertexInputs->constantBytes[slot] ||
                            (pipeline.vertexReplacement && pipeline.vertexReplacement->RequiredConstantBytes()[slot] &&
                                !pipeline.vertexReplacement->AddedConstantBytes()[slot])))
                            return Fail(Dx11ExecutionError::Unsupported);
                }
                const bool indexed = original ? r.draw.drawIndexed : a.op != ActionOp::Draw;
                const auto count = original ? (indexed ? r.draw.indexCount : r.draw.vertexCount) : a.count;
                if (!VertexTextureMaterials(r) || !PixelMaterials(r) || !OutlineMaterial(r) || !DiffuseMaterial(r) || !PrepareStreams(r, count, indexed, prepared.streamBytes)) return false;
            }
        }
        if (rasterDepth) return Fail(Dx11ExecutionError::Tape);
        for (unsigned i = 0; i < depthCount; ++i)
            if (!copiedDepth[i]) return Fail(Dx11ExecutionError::Tape);
        // Actual t120 loads qualify the parameter consumer independently of
        // optional t69/t70 material presence. Full-null resource state is legal
        // under the exact template; it is never reported as sampled material.
        return true;
    }
};

Dx11ExecutionLease::Dx11ExecutionLease(std::uint32_t capacity) : state_(std::make_unique<Storage>(capacity)) {}
Dx11ExecutionLease::~Dx11ExecutionLease() = default;
const Dx11AliasChange* Dx11ExecutionLease::Aliases() const noexcept { return state_->aliases.data(); }
std::uint32_t Dx11ExecutionLease::AliasCount() const noexcept { return state_->aliasCount; }
const Dx11RetainedBinding* Dx11ExecutionLease::Resources() const noexcept { return state_->resources.data(); }
std::uint32_t Dx11ExecutionLease::ResourceCount() const noexcept { return state_->resourceCount; }
const Dx11PipelineBinding& Dx11ExecutionLease::Pipeline() const noexcept { return state_->pipeline; }
const std::shared_ptr<const Plan>& Dx11ExecutionLease::PlanOwner() const noexcept { return state_->plan; }
const std::shared_ptr<const Dx11StaticResources>& Dx11ExecutionLease::ParameterOwner() const noexcept { return state_->parameters; }
std::uint32_t Dx11ExecutionLease::InitialParameters() const noexcept { return state_->initialParameters; }
std::uint32_t Dx11ExecutionLease::FinalParameters() const noexcept { return state_->finalParameters; }
const Dx11StreamOutputBinding& Dx11ExecutionLease::StreamOutput() const noexcept { return state_->streams; }
const std::array<std::uint64_t, D3D11_SO_BUFFER_SLOT_COUNT>& Dx11ExecutionLease::StreamBytesIssued() const noexcept { return state_->streamIssued; }

struct Dx11Executor::Storage
{
    std::vector<std::shared_ptr<Dx11ExecutionLease>> flights;
    std::uint32_t capacity;
    bool executing = false; // only the authenticated serial context owner calls
    explicit Storage(std::uint32_t actions) : capacity(actions) {}
};
Dx11Executor::Dx11Executor(std::uint32_t actions, std::uint32_t flights)
{
    if (!actions || actions > 4096 || !flights || flights > 16)
        throw std::invalid_argument("GIMI DX11 executor requires bounded action/retirement capacities");
    state_ = std::make_unique<Storage>(actions);
    state_->flights.reserve(flights);
    for (unsigned n = 0; n < flights; ++n)
        state_->flights.emplace_back(new Dx11ExecutionLease(actions));
}
Dx11Executor::~Dx11Executor() = default;

Dx11ExecutionResult Dx11Executor::Execute(const Dx11ExecutionRequest& r) noexcept
{
    Dx11ExecutionResult result;
    if (!r.enabled) { result.error = Dx11ExecutionError::Disabled; return result; }
    if (!r.device || !r.context || !r.commands || !r.programs || !r.programCount || !r.statics ||
        !r.resources || !r.owner || (r.draw.instanced ? !r.originalInstanced : r.draw.drawIndexed ? !r.original : !r.originalDraw) ||
        !r.commands->PlanOwner() || !r.statics->Files() ||
        r.commands->PlanOwner().get() != r.statics->Files()->PlanOwner().get())
    { result.error = Dx11ExecutionError::InvalidRequest; return result; }
    if (state_->executing) { result.error = Dx11ExecutionError::Busy; return result; }
    struct Executing
    {
        bool& flag;
        explicit Executing(bool& f) noexcept : flag(f) { flag = true; }
        ~Executing() { flag = false; }
    } executing(state_->executing);
    if (r.context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || !OnDevice(r.context, r.device))
    { result.error = Dx11ExecutionError::WrongContext; return result; }
    if ((!r.draw.instanced && (r.draw.instanceCount != 1 || r.draw.firstInstance)) ||
        (r.draw.instanced && (!r.draw.drawIndexed || !r.owner->SupportsIndexedInstanced())) ||
        r.commands->PlanOwner()->Limits().actions > state_->capacity)
    { result.error = Dx11ExecutionError::Unsupported; return result; }
    // The immutable preload owns a proven finite parameter domain. All page
    // transitions are still checked before the first GPU effect below.
    if (!r.owner->Current(r.device, r.context, r.pipeline.epochs) || !r.resources->StableForInvocation(r.pipeline))
    { result.error = Dx11ExecutionError::WrongOwner; return result; }

    std::shared_ptr<Dx11ExecutionLease> packet;
    for (const auto& flight : state_->flights)
        if (flight.use_count() == 1) { packet = flight; break; }
    // use_count is an AVAILABILITY test, not a fence completion query. Only the
    // caller's authenticated retirement owner is allowed to drop its reference.
    if (!packet) { result.error = Dx11ExecutionError::Busy; return result; }
    auto& work = *packet->state_;
    if (!work.Reset(r)) { result.error = work.error; return result; }
    if (!work.CapturePipeline(r))
    {
        result.error = work.error; result.vertexConstantSlot = work.failedVertexConstant;
        result.vertexConstantReason = work.vertexConstantReason; return result;
    }
    Dx11ExecutionLease::Storage::CheckedQueries queries{work, r};
    result.prepared = r.commands->PrepareDraw(r.programs, r.programCount, r.draw, queries.Commands(), r.requiredProgram,
        r.requiredOverrides, r.requiredOverrideCount, r.observationPrograms, r.selectionPrograms);
    if (!result.prepared) { result.error = Dx11ExecutionError::Prepare; result.prepared.batch = {}; return result; }
    const auto failure = [&](Dx11ExecutionError error, unsigned action) noexcept
    {
        result.error = error; result.action = action;
        const auto& batch = result.prepared.batch;
        if (batch.actions && action < batch.count)
        {
            result.prepared.program = batch.actions[action].program;
            result.prepared.command = batch.actions[action].command;
            result.prepared.phase = batch.actions[action].phase;
        }
        r.commands->Discard(); result.prepared.batch = {};
    };
    if (!work.PrepareTape(r, result.prepared.batch))
    { failure(work.error, work.failedAction); return result; }
    if (!r.owner->Current(r.device, r.context, work.pipeline.epochs) || !r.resources->StableForInvocation(work.pipeline))
    { failure(Dx11ExecutionError::OwnerChanged, 0); return result; }

    std::uint64_t reservation = 0;
    if (!r.owner->Reserve(packet, reservation))
    { failure(Dx11ExecutionError::Retention, 0); return result; }
    std::uint64_t nesting = 0;
    const bool entered = r.owner->EnterInternal(r.context, nesting);
    if (!entered || !r.owner->InternalActive(r.context))
    {
        if (entered) r.owner->LeaveInternal(r.context, nesting);
        r.owner->Finish(reservation, Dx11ExecutionOutcome::Cancelled);
        failure(Dx11ExecutionError::Suppression, 0); return result;
    }

    bool gpuIssued = false, successful = true, parametersBound = false;
    std::uint32_t boundParameters = work.initialParameters;
    const auto bindParameters = [&](std::uint32_t state) noexcept {
        if (parametersBound && state == boundParameters) return;
        auto* srv = work.parameters->ParameterView(state); // whole tape prevalidated
        boundParameters = state; parametersBound = true;
        gpuIssued = true; work.saved.iniVertexChanged = true;
        SetBit(work.saved.pixelChanged, IniParameterRegister);
        r.context->VSSetShaderResources(IniParameterRegister, 1, &srv);
        r.context->PSSetShaderResources(IniParameterRegister, 1, &srv);
    };
    Dx11ExecutionError executionError = Dx11ExecutionError::None;
    unsigned action = 0;
    for (; action < work.tapeCount; ++action)
    {
        const auto& prepared = work.tape[action]; const auto& a = prepared.action;
        if (!r.owner->Current(r.device, r.context, work.pipeline.epochs) || !r.owner->InternalActive(r.context))
        { successful = false; executionError = Dx11ExecutionError::OwnerChanged; break; }
        if (work.parameters && !parametersBound)
            bindParameters(work.initialParameters);
        if (a.op == ActionOp::SetParameter) bindParameters(prepared.parameters);
        else if (a.op == ActionOp::ClearTarget)
        {
            gpuIssued = true; // before the void clear: partial submission retains real owner
            ClearPrivateTexture(r.context, *work.resources[prepared.resource].ownedTexture);
        }
        else if (a.op == ActionOp::CopyDepth)
        {
            const auto& depth = work.resources[prepared.resource].ownedTexture;
            // Same authenticated ORIGINAL immediate context, at this exact
            // tape position. No ref, readback, staging upload or implicit resolve.
            // CopyResource has no HRESULT: potential emission owns quarantine.
            gpuIssued = true;
            r.context->CopyResource(depth->texture.Get(), depth->source.texture);
        }
        else if (a.op == ActionOp::UnbindVertexConstant)
        {
            // Saved COM/range owners already belong to the reserved packet.
            // Mark potential emission before the void setter; the unchanged
            // restoration/Signal/quarantine path owns every failure afterwards.
            gpuIssued = true; work.saved.constantChanged |= std::uint32_t{1} << a.constantSlot;
            ID3D11Buffer* empty = nullptr;
            r.context->VSSetConstantBuffers(a.constantSlot, 1, &empty);
        }
        else if (a.op == ActionOp::BindOutput)
        {
            gpuIssued = true; work.outputsChanged = true; // before the void setter
            r.context->OMSetRenderTargets(static_cast<UINT>(prepared.outputs.size()), prepared.outputs.data(), prepared.depth);
        }
        else if (a.op == ActionOp::BeginRasterCustom || a.op == ActionOp::EndRasterCustom)
        {
            gpuIssued = true; work.saved.rasterChanged = true;
            r.context->RSSetState(prepared.raster);
            if (a.op == ActionOp::EndRasterCustom)
            {
                work.outputsChanged = true;
                r.context->OMSetRenderTargets(static_cast<UINT>(prepared.outputs.size()), prepared.outputs.data(), prepared.depth);
            }
        }
        else if (a.op == ActionOp::Bind)
        {
            const auto& v = work.resources[prepared.resource].value;
            // Mark potential command emission BEFORE the void D3D call. There
            // is no HRESULT proving that no command reached the driver.
            gpuIssued = true;
            if (a.destination.kind == TargetKind::VertexBuffer)
            {
                auto* buffer = v.buffer;
                work.saved.vertexChanged |= 1u << a.destination.index;
                r.context->IASetVertexBuffers(a.destination.index, 1, &buffer, &v.binding.stride, &v.binding.offset);
            }
            else if (a.destination.kind == TargetKind::IndexBuffer)
            {
                work.saved.indexChanged = true;
                const auto format = v.buffer ? static_cast<DXGI_FORMAT>(v.binding.format) : DXGI_FORMAT_UNKNOWN;
                r.context->IASetIndexBuffer(v.buffer, format, v.binding.offset);
            }
            else if (a.destination.kind == TargetKind::VertexTexture)
            {
                auto* srv = v.srv; SetBit(work.saved.vertexTextureChanged, a.destination.index);
                r.context->VSSetShaderResources(a.destination.index, 1, &srv);
            }
            else
            {
                auto* srv = v.srv; SetBit(work.saved.pixelChanged, a.destination.index);
                r.context->PSSetShaderResources(a.destination.index, 1, &srv);
            }
        }
        else if (a.op == ActionOp::DebugOverlay)
        {
            // This packet is already Reserved with all four SRVs, s0, immutable
            // t120 pages, shaders, OM owners and alias before/after images.
            // Any setter may have reached the driver: no fallback redraw.
            gpuIssued = true;
            work.BeginOverlay(r.context, prepared);
            const bool before = r.owner->BeforeDraw(r.context, 3, 0, false);
            bool after = false;
            if (before)
            {
                r.originalDraw(r.context, 3, 0); ++result.rasterCalls;
                after = r.owner->AfterDraw(r.context, 3, 0, false);
            }
            work.EndOverlay(r.context, prepared); // also restore on observer refusal
            if (!before || !after)
            { successful = false; executionError = Dx11ExecutionError::Raster; break; }
        }
        else if (a.op == ActionOp::DrawIndexed || a.op == ActionOp::Draw || a.op == ActionOp::DrawIndexedInstanced ||
                 (a.op == ActionOp::OriginalDrawBoundary && !a.skipOriginal))
        {
            if (prepared.skipRaster) continue;
            const bool original = a.op == ActionOp::OriginalDrawBoundary;
            const bool indexed = original ? r.draw.drawIndexed : a.op != ActionOp::Draw;
            const bool instanced = original ? r.draw.instanced : a.op == ActionOp::DrawIndexedInstanced;
            const auto instances = original ? r.draw.instanceCount : a.instances;
            const auto firstInstance = original ? r.draw.firstInstance : a.firstInstance;
            const UINT count = original ? (indexed ? r.draw.indexCount : r.draw.vertexCount) : a.count;
            const UINT first = original ? (indexed ? r.draw.firstIndex : r.draw.firstVertex) : a.first;
            const INT base = original ? r.draw.baseVertex : a.baseVertex;
            if (work.pipeline.pixelReplacement && work.pipeline.pixelReplacement->OutlineResources() &&
                !work.saved.outlineSamplerChanged)
            {
                // Whole tape preflight and Reserve precede this real setter.
                // Rebind EXACTLY the captured object (including real null).
                auto* sampler = work.saved.outlineSampler.Get();
                gpuIssued = true; work.saved.outlineSamplerChanged = true;
                r.context->PSSetSamplers(15, 1, &sampler);
            }
            if (work.pipeline.vertexReplacement && !work.saved.vertexShaderChanged)
            {
                // Whole-tape qualification and Reserve own the authentic
                // source/signature, replacement, immutable pages and exact
                // extra CB ranges. BeforeRaster sees the REAL bound shader.
                gpuIssued = true; work.saved.vertexShaderChanged = true;
                r.context->VSSetShader(work.pipeline.vertexReplacement->Shader(), nullptr, 0);
            }
            if (work.pipeline.pixelReplacement && !work.saved.pixelShaderChanged)
            {
                // Reserve already retained the exact capability and parameter
                // pages. Mark potential emission BEFORE the void setter. The
                // observer sees the ACTUAL shader; never spoof original identity.
                gpuIssued = true; work.saved.pixelShaderChanged = true;
                r.context->PSSetShader(work.pipeline.pixelReplacement->Shader(), nullptr, 0);
            }
            // Before=false opens no observer. Every true Before is paired with
            // exactly one real typed trampoline and its matching After, even
            // when After reports failure. An outer scope must use the hook's
            // Consumed disposition, never emit another physical-raster Before.
            const bool before = instanced ? r.owner->BeforeIndexedInstanced(r.context, count, instances, first, base, firstInstance, original) :
                indexed ? r.owner->BeforeRaster(r.context, count, first, base, original) :
                r.owner->BeforeDraw(r.context, count, first, original);
            if (!before) { successful = false; executionError = Dx11ExecutionError::Raster; break; }
            gpuIssued = true; ++result.rasterCalls;
            for (UINT slot = 0; slot < work.streamIssued.size(); ++slot) work.streamIssued[slot] += prepared.streamBytes[slot];
            if (original) result.originalCalled = true;
            if (instanced) r.originalInstanced(r.context, count, instances, first, base, firstInstance);
            else if (indexed) r.original(r.context, count, first, base);
            else r.originalDraw(r.context, count, first);
            const bool after = instanced ? r.owner->AfterIndexedInstanced(r.context, count, instances, first, base, firstInstance, original) :
                indexed ? r.owner->AfterRaster(r.context, count, first, base, original) :
                r.owner->AfterDraw(r.context, count, first, original);
            if (!after) { successful = false; executionError = Dx11ExecutionError::Raster; break; }
        }
        // SetResource was simulated/journaled by CommandState and retained in
        // Reserve. It emits no setter and is NOT silently ignored: only the
        // successful Accept/Finish pair commits those exact aliases.
    }
    if (successful && (!r.owner->Current(r.device, r.context, work.pipeline.epochs) || !r.owner->InternalActive(r.context)))
    { successful = false; executionError = Dx11ExecutionError::OwnerChanged; }
    work.Restore(r.context);
    r.owner->LeaveInternal(r.context, nesting);
    if (!successful)
    {
        result.consumed = gpuIssued; result.quarantined = gpuIssued;
        failure(executionError, action);
        r.owner->Finish(reservation, gpuIssued ? Dx11ExecutionOutcome::Quarantine : Dx11ExecutionOutcome::Cancelled);
        return result;
    }
    r.commands->Accept(); // combined PrepareDraw already closes its invocation
    result.originalSkipped = result.prepared.batch.skipOriginal;
    result.prepared.batch = {}; result.consumed = true;
    r.owner->Finish(reservation, Dx11ExecutionOutcome::Submitted);
    return result;
}
} // namespace gimi
