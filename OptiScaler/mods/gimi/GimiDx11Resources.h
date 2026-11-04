#pragma once

#include "GimiResources.h"
#include "GimiDrawCache.h"
#include <d3d11_1.h>
#include <d3d11shader.h>

namespace gimi
{
// Runtime-created game leases own their real COM resources/views. Implementors
// are created outside the hook, not wrappers allocated by ResolveGame per draw.
class Dx11GameLease
{
public:
    virtual ~Dx11GameLease() = default;
};

// R495's Dx11LeaseView is borrowed. Exactly one of these typed owners keeps it
// alive; neither an object ID nor view ID is ever interpreted as an address.
// Owning typed OM operand. It never grants a shader-resource read capability.
struct Dx11OutputBinding
{
    std::shared_ptr<const Dx11GameLease> owner;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11Texture2D* texture = nullptr;
    Binding binding;
    ResourceIdentity identity;
    OutputViewMetadata view;
};
class Dx11OwnedTextureLease;
struct Dx11RetainedBinding
{
    Dx11LeaseView value;
    std::shared_ptr<const Dx11StaticResources> statics;
    std::shared_ptr<const Dx11GameLease> game;
    bool stableReadOnly = false;
    // Separate custom texture ownership; descriptor-only aliases are not readable.
    // Never manufactures a game content hash.
    std::shared_ptr<Dx11OwnedTextureLease> ownedTexture;
    bool textureWritten = false; // packet-local real producer; published only on full submit
    Dx11OutputBinding output; // RTV/DSV alias: ownership only, never stableReadOnly/SRV
};

// Own the creation-time qualified input layout and shader COM objects together
// with the relevant identity revisions. The executor compares their actual
// current bindings, not a previous draw's pointer-only classification.
class Dx11PixelReplacement;
class Dx11VertexReplacement;
class Dx11PipelineOwner
{
public:
    virtual ~Dx11PipelineOwner() = default;
};
// Creation-reflected original VS contract. Never authorizes replacement shader
// ABI or content identity; these slots are preserved, not exposed to commands.
enum class Dx11VertexReadKind : std::uint8_t { None, TypedBuffer, StructuredBuffer, Texture2D };
struct Dx11VertexReadShape
{
    Dx11VertexReadKind kind = Dx11VertexReadKind::None;
    D3D_RESOURCE_RETURN_TYPE returns = D3D_RETURN_TYPE_MIXED;
};
struct Dx11VertexInputs
{
    bool qualified = false;
    std::array<UINT, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> constantBytes{};
    std::array<Dx11VertexReadShape, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> resources{};
    std::uint32_t samplers = 0;
};
// The provider must authenticate exact creation-time COM ownership/generation,
// current unmapped state and complete serial write coverage. No hash/CPU image
// is needed or invented for a game VS input (original or typed added CB).
// A new reader must separately prove its executable range. Never used by aliases.
struct Dx11VertexReadLease
{
    std::shared_ptr<const Dx11GameLease> owner;
    ID3D11Resource* resource = nullptr;
    ID3D11Buffer* buffer = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11Texture2D* texture = nullptr;
    D3D11_BUFFER_DESC bufferDesc{};
    D3D11_TEXTURE2D_DESC textureDesc{};
    std::uint64_t object = 0, generation = 0;
    bool gpuOrdered = false;
};
// Original PS resource interface, reflected at shader creation. A command may
// bind a DDS only to a compatible actually-declared read slot at a raster.
// This is not a replacement ABI, a texture-name classifier, or a sampler map.
struct Dx11PixelReadShape
{
    bool declared = false;
    D3D_SHADER_INPUT_TYPE type = D3D_SIT_TEXTURE;
    D3D_SRV_DIMENSION dimension = D3D_SRV_DIMENSION_UNKNOWN;
    D3D_RESOURCE_RETURN_TYPE returns = D3D_RETURN_TYPE_MIXED;
};
struct Dx11PixelInputs
{
    bool qualified = false;
    std::array<Dx11PixelReadShape, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> resources{};
    std::uint32_t samplers = 0, comparisonSamplers = 0;
};
struct Dx11PipelineBinding
{
    std::shared_ptr<const Dx11PipelineOwner> owner;
    std::shared_ptr<const LayoutContract> layout;
    ID3D11InputLayout* inputLayout = nullptr;
    ID3D11VertexShader* vertexShader = nullptr;
    ID3D11PixelShader* pixelShader = nullptr;
    QualifiedLayout qualified;
    ShaderIdentity vertexIdentity, pixelIdentity;
    DrawEpochs epochs;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    const Dx11VertexInputs* vertexInputs = nullptr; // retained by owner
    ID3D11DeviceContext1* vertexRangeContext = nullptr; // same context, retained by owner
    ID3D11GeometryShader* geometryShader = nullptr;
    ShaderIdentity geometryIdentity;
    const Dx11VertexInputs* geometryInputs = nullptr;
    const Dx11StreamOutputContract* streamContract = nullptr; // retained by owner
    // Always the ORIGINAL creation-reflected VS/PS fact, never overwritten by
    // a replacement capability. Shape/name alone grants no consumer authority.
    bool iniParameterSlotUnused = false;
    // Created/qualified off-draw. The packet copies this owning capability,
    // retaining source/original/replacement/device through real GPU retirement.
    std::shared_ptr<const Dx11PixelReplacement> pixelReplacement;
    std::shared_ptr<const Dx11VertexReplacement> vertexReplacement;
    const Dx11PixelInputs* pixelInputs = nullptr; // original creation contract, retained by owner
};

inline bool OutputSubresources(const D3D11_TEXTURE2D_DESC& t, OutputViewMetadata& o) noexcept
{
    if (!t.MipLevels || !t.ArraySize || !o.sliceCount || o.mip >= t.MipLevels ||
        o.firstSlice >= t.ArraySize || o.sliceCount > t.ArraySize - o.firstSlice ||
        o.format == DXGI_FORMAT_UNKNOWN) return false;
    const auto first = std::uint64_t(o.firstSlice) * t.MipLevels + o.mip;
    if (first > UINT32_MAX) return false;
    o.firstSubresource = static_cast<UINT>(first); return true;
}
inline bool DescribeOutputView(const D3D11_TEXTURE2D_DESC& t, const D3D11_RENDER_TARGET_VIEW_DESC& d,
                              OutputViewMetadata& o) noexcept
{
    o.depth = false; o.dimension = d.ViewDimension; o.format = d.Format;
    o.mip = o.firstSlice = o.flags = 0; o.sliceCount = 1;
    bool multisample = false;
    switch (d.ViewDimension)
    {
    case D3D11_RTV_DIMENSION_TEXTURE2D: o.mip = d.Texture2D.MipSlice; break;
    case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
        o.mip = d.Texture2DArray.MipSlice; o.firstSlice = d.Texture2DArray.FirstArraySlice; o.sliceCount = d.Texture2DArray.ArraySize; break;
    case D3D11_RTV_DIMENSION_TEXTURE2DMS: multisample = true; break;
    case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
        multisample = true; o.firstSlice = d.Texture2DMSArray.FirstArraySlice; o.sliceCount = d.Texture2DMSArray.ArraySize; break;
    default: return false;
    }
    return (multisample ? t.SampleDesc.Count > 1 : t.SampleDesc.Count == 1) && OutputSubresources(t, o);
}
inline bool DescribeOutputView(const D3D11_TEXTURE2D_DESC& t, const D3D11_DEPTH_STENCIL_VIEW_DESC& d,
                              OutputViewMetadata& o) noexcept
{
    o.depth = true; o.dimension = d.ViewDimension; o.format = d.Format; o.flags = d.Flags;
    o.mip = o.firstSlice = 0; o.sliceCount = 1;
    bool multisample = false;
    switch (d.ViewDimension)
    {
    case D3D11_DSV_DIMENSION_TEXTURE2D: o.mip = d.Texture2D.MipSlice; break;
    case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
        o.mip = d.Texture2DArray.MipSlice; o.firstSlice = d.Texture2DArray.FirstArraySlice; o.sliceCount = d.Texture2DArray.ArraySize; break;
    case D3D11_DSV_DIMENSION_TEXTURE2DMS: multisample = true; break;
    case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
        multisample = true; o.firstSlice = d.Texture2DMSArray.FirstArraySlice; o.sliceCount = d.Texture2DMSArray.ArraySize; break;
    default: return false;
    }
    return (multisample ? t.SampleDesc.Count > 1 : t.SampleDesc.Count == 1) && OutputSubresources(t, o);
}

// Exact full-resource copy shape for the original Texture2D<float4> t22
// reader. Array/MSAA/non-base mip require a different reader ABI and stay closed.
inline bool DepthCopyShape(const Dx11OutputBinding& source, D3D11_TEXTURE2D_DESC& out,
                           D3D11_SHADER_RESOURCE_VIEW_DESC& srv, UINT& texelBytes) noexcept
{
    const auto& t = source.identity.texture;
    const auto& v = source.view;
    if (!source.owner || !source.dsv || source.rtv || !source.texture ||
        source.identity.knowledge != Knowledge::Known || source.identity.kind != IdentityKind::Texture2D ||
        !v.depth || v.dimension != D3D11_DSV_DIMENSION_TEXTURE2D || v.mip || v.firstSlice ||
        v.sliceCount != 1 || v.firstSubresource || (v.flags & ~(D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL)) ||
        !t.Width || !t.Height || t.Width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        t.Height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || t.MipLevels != 1 || t.ArraySize != 1 ||
        t.SampleDesc.Count != 1 || t.SampleDesc.Quality || t.Usage != D3D11_USAGE_DEFAULT ||
        t.CPUAccessFlags || t.MiscFlags || !(t.BindFlags & D3D11_BIND_DEPTH_STENCIL) ||
        (t.BindFlags & ~(D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE))) return false;
    out = t; out.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    srv = {}; srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1;
    switch (v.format)
    {
    case DXGI_FORMAT_D16_UNORM:
        if (t.Format != DXGI_FORMAT_D16_UNORM && t.Format != DXGI_FORMAT_R16_TYPELESS) return false;
        out.Format = DXGI_FORMAT_R16_TYPELESS; srv.Format = DXGI_FORMAT_R16_UNORM; texelBytes = 2; break;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        if (t.Format != DXGI_FORMAT_D24_UNORM_S8_UINT && t.Format != DXGI_FORMAT_R24G8_TYPELESS) return false;
        out.Format = DXGI_FORMAT_R24G8_TYPELESS; srv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; texelBytes = 4; break;
    case DXGI_FORMAT_D32_FLOAT:
        if (t.Format != DXGI_FORMAT_D32_FLOAT && t.Format != DXGI_FORMAT_R32_TYPELESS) return false;
        out.Format = DXGI_FORMAT_R32_TYPELESS; srv.Format = DXGI_FORMAT_R32_FLOAT; texelBytes = 4; break;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        if (t.Format != DXGI_FORMAT_D32_FLOAT_S8X24_UINT && t.Format != DXGI_FORMAT_R32G8X24_TYPELESS) return false;
        out.Format = DXGI_FORMAT_R32G8X24_TYPELESS; srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; texelBytes = 8; break;
    default: return false;
    }
    return true;
}
// Shapes are prepared only at serial Present. A free pool slot has no packet/
// alias owner; a descriptor alias can persist without initialized pixel data.
// Real packet fences release readers; quarantine retains every affected owner.
enum class OwnedTexturePurpose : std::uint8_t { DepthCopy, OutputDescription };
struct PrivateTextureShape
{
    D3D11_TEXTURE2D_DESC texture{};
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    D3D11_RENDER_TARGET_VIEW_DESC rtv{};
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
    UINT texelBytes = 0;
};
// Same single-mip/slice/sample ABI as the existing float Texture2D reader.
// The resource family becomes typeless (XXMI MakeTypeless), the actual source
// view selects the typed view. No resize, format conversion or pixel copy.
inline bool DescribePrivateTexture(const Dx11OutputBinding& source, OwnedTexturePurpose purpose,
                                   PrivateTextureShape& shape) noexcept
{
    shape = {};
    if (purpose == OwnedTexturePurpose::DepthCopy || source.view.depth)
    {
        if (!DepthCopyShape(source, shape.texture, shape.srv, shape.texelBytes)) return false;
        if (purpose == OwnedTexturePurpose::OutputDescription)
        {
            shape.texture.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
            shape.dsv.Format = source.view.format; shape.dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            // Fixed XXMI FillOutTex2DDesc creates a new writable DSV (Flags=0).
        }
        return true;
    }
    const auto& t = source.identity.texture; const auto& v = source.view;
    if (!source.owner || !source.rtv || source.dsv || !source.texture ||
        source.identity.knowledge != Knowledge::Known || source.identity.kind != IdentityKind::Texture2D ||
        v.depth || v.dimension != D3D11_RTV_DIMENSION_TEXTURE2D || v.flags || v.mip || v.firstSlice ||
        v.sliceCount != 1 || v.firstSubresource || !t.Width || !t.Height ||
        t.Width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || t.Height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        t.MipLevels != 1 || t.ArraySize != 1 || t.SampleDesc.Count != 1 || t.SampleDesc.Quality ||
        t.Usage != D3D11_USAGE_DEFAULT || t.CPUAccessFlags || t.MiscFlags ||
        !(t.BindFlags & D3D11_BIND_RENDER_TARGET) ||
        (t.BindFlags & ~(D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE))) return false;
    DXGI_FORMAT family = DXGI_FORMAT_UNKNOWN;
    switch (v.format)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: family = DXGI_FORMAT_R32G32B32A32_TYPELESS; shape.texelBytes = 16; break;
    case DXGI_FORMAT_R32G32_FLOAT: family = DXGI_FORMAT_R32G32_TYPELESS; shape.texelBytes = 8; break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_SNORM:
        family = DXGI_FORMAT_R16G16B16A16_TYPELESS; shape.texelBytes = 8; break;
    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_SNORM:
        family = DXGI_FORMAT_R16G16_TYPELESS; shape.texelBytes = 4; break;
    case DXGI_FORMAT_R32_FLOAT: family = DXGI_FORMAT_R32_TYPELESS; shape.texelBytes = 4; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM: family = DXGI_FORMAT_R10G10B10A2_TYPELESS; shape.texelBytes = 4; break;
    case DXGI_FORMAT_R11G11B10_FLOAT: family = DXGI_FORMAT_R11G11B10_FLOAT; shape.texelBytes = 4; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_SNORM:
        family = DXGI_FORMAT_R8G8B8A8_TYPELESS; shape.texelBytes = 4; break;
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM:
        family = DXGI_FORMAT_R8G8_TYPELESS; shape.texelBytes = 2; break;
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_SNORM:
        family = DXGI_FORMAT_R16_TYPELESS; shape.texelBytes = 2; break;
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_SNORM:
        family = DXGI_FORMAT_R8_TYPELESS; shape.texelBytes = 1; break;
    // MakeTypeless leaves the linear BGRA formats as-is in this fixed XXMI.
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM: family = v.format; shape.texelBytes = 4; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: family = DXGI_FORMAT_B8G8R8A8_TYPELESS; shape.texelBytes = 4; break;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: family = DXGI_FORMAT_B8G8R8X8_TYPELESS; shape.texelBytes = 4; break;
    default: return false; // integer/BC/planar/unknown reader ABI stays closed
    }
    if (t.Format != v.format && t.Format != family) return false;
    shape.texture = t; shape.texture.Format = family;
    shape.texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    shape.srv.Format = v.format; shape.srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; shape.srv.Texture2D.MipLevels = 1;
    shape.rtv.Format = v.format; shape.rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    return true;
}
class Dx11OwnedTextureLease final : public Dx11GameLease
{
public:
    Dx11OutputBinding source;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
    Binding binding; // SRV lease
    Binding outputBinding; // distinct real RTV/DSV lease, same texture object
    ResourceIdentity identity;
    OwnedTexturePurpose purpose = OwnedTexturePurpose::DepthCopy;
    Id alias = InvalidId; // destination identity; Fakeo1 and Fakeo6 must never share storage
    std::uint64_t bytes = 0;
    bool ordered = false; // initialized by real Copy/Clear, not descriptor creation
};
// Validate the owned resource AND every cached view, independently of game
// metadata. Shared by raster and native POST preflight; no GPU side effects.
inline bool ValidatePrivateTexture(const Dx11OwnedTextureLease& target, ID3D11Device* device) noexcept
{
    PrivateTextureShape shape;
    if (!target.texture || !target.srv || !DescribePrivateTexture(target.source, target.purpose, shape)) return false;
    D3D11_TEXTURE2D_DESC desc{}; target.texture->GetDesc(&desc);
    const auto& e = shape.texture;
    if (desc.Width != e.Width || desc.Height != e.Height || desc.MipLevels != e.MipLevels || desc.ArraySize != e.ArraySize ||
        desc.Format != e.Format || desc.SampleDesc.Count != e.SampleDesc.Count || desc.SampleDesc.Quality != e.SampleDesc.Quality ||
        desc.Usage != e.Usage || desc.BindFlags != e.BindFlags || desc.CPUAccessFlags != e.CPUAccessFlags || desc.MiscFlags != e.MiscFlags)
        return false;
    Microsoft::WRL::ComPtr<ID3D11Device> actualDevice; target.texture->GetDevice(&actualDevice);
    if (actualDevice.Get() != device) return false;
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    target.srv->GetResource(&resource);
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{}; target.srv->GetDesc(&srv);
    if (resource.Get() != target.texture.Get() || srv.Format != shape.srv.Format || srv.ViewDimension != shape.srv.ViewDimension ||
        srv.Texture2D.MostDetailedMip || srv.Texture2D.MipLevels != 1) return false;
    if (shape.rtv.Format != DXGI_FORMAT_UNKNOWN)
    {
        if (!target.rtv || target.dsv) return false;
        D3D11_RENDER_TARGET_VIEW_DESC v{}; target.rtv->GetDesc(&v); resource.Reset(); target.rtv->GetResource(&resource);
        if (resource.Get() != target.texture.Get() || v.Format != shape.rtv.Format ||
            v.ViewDimension != shape.rtv.ViewDimension || v.Texture2D.MipSlice) return false;
    }
    else if (shape.dsv.Format != DXGI_FORMAT_UNKNOWN)
    {
        if (!target.dsv || target.rtv) return false;
        D3D11_DEPTH_STENCIL_VIEW_DESC v{}; target.dsv->GetDesc(&v); resource.Reset(); target.dsv->GetResource(&resource);
        if (resource.Get() != target.texture.Get() || v.Format != shape.dsv.Format || v.Flags ||
            v.ViewDimension != shape.dsv.ViewDimension || v.Texture2D.MipSlice) return false;
    }
    else if (target.rtv || target.dsv) return false;
    if (target.rtv || target.dsv)
    {
        const auto& output = target.outputBinding;
        const auto format = target.rtv ? shape.rtv.Format : shape.dsv.Format;
        if (output.object != target.binding.object || !output.view || output.view == target.binding.view ||
            output.generation != target.binding.generation || output.format != static_cast<UINT>(format) ||
            output.stride || output.offset || output.byteSize) return false;
    }
    else if (target.outputBinding.object || target.outputBinding.view) return false;
    return target.binding.object && target.binding.view && target.binding.format == srv.Format &&
        target.binding.generation == target.identity.deviceGeneration && target.identity.object == target.binding.object &&
        target.identity.origin == ResourceOrigin::Custom && !target.identity.contentKnown && !target.identity.hashKnown;
}
inline void ClearPrivateTexture(ID3D11DeviceContext* context, const Dx11OwnedTextureLease& target) noexcept
{
    // Caller preflighted the complete tape and reserved its actual GPU owner.
    if (target.rtv) { const FLOAT zero[4]{}; context->ClearRenderTargetView(target.rtv.Get(), zero); }
    else context->ClearDepthStencilView(target.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
}

// Adapter over R495 + the existing serial runtime's retained game registry.
// No registry/hash/decoder is implemented here. Every callback is bounded,
// noexcept, allocation/IO/upload/readback-free, with no GPU side effects.
class Dx11ResourceProvider
{
public:
    virtual ~Dx11ResourceProvider() = default;
    // Certifies ALL inputs queried by PrepareDraw remain stable across PRE,
    // physical raster and POST, not only the resources eventually rebound.
    // Requires the current authenticated immediate owner/thread/epochs and a
    // complete write/opaque-boundary observation. Unknown is false.
    virtual bool StableForInvocation(const Dx11PipelineBinding&) const noexcept = 0;
    // Actual getter-derived COM pointers, NOT opaque Binding IDs. Fixed-table
    // lookup only; false for missed creation, pending Map or unknown writes.
    virtual bool RetainVertexRead(ID3D11Resource*, ID3D11ShaderResourceView*,
                                  Dx11VertexReadLease&) const noexcept = 0;
    virtual bool RetainStreamOutput(Dx11StreamOutputBinding&) const noexcept { return false; }
    virtual bool CurrentBinding(Target target, Binding& out) const noexcept = 0;
    // Called only if the static resource set did not resolve this exact key.
    // Must supply R495 identity, an owning game lease and current read-only
    // stability. A stale generation/descriptor/view must return false.
    virtual bool ResolveGame(Binding binding, Dx11RetainedBinding& out) const noexcept = 0;
    // Native restoration/reference only: descriptor + real COM owner, no
    // content/hash/sample capability. A new shader reader must qualify separately.
    virtual bool RetainNativeReference(Binding, Dx11RetainedBinding&) const noexcept { return false; }
    virtual bool RetainNativeAlias(Id, Binding, Dx11RetainedBinding&) const noexcept { return false; }
    // Exact retained buffer descriptor for the current IA IB query only.
    // stableReadOnly remains false: this capability cannot bind, alias-read,
    // validate indices, or authorize a raster. Missing owner/epoch is unknown.
    virtual bool DescribeGameIndex(Binding, Dx11RetainedBinding&) const noexcept { return false; }
    virtual bool DescribeGameOutput(Target, Binding, Dx11OutputBinding&) const noexcept { return false; }
    virtual bool ResolveOutputOperand(Binding, Dx11OutputBinding&) const noexcept { return false; }
    virtual bool AcquireDepthCopy(const Dx11OutputBinding&, Dx11RetainedBinding&) const noexcept { return false; }
    virtual bool AcquireOutputDescription(const Dx11OutputBinding&, Id, Dx11RetainedBinding&) const noexcept { return false; }
    // Descriptor ownership only. The caller must separately prove initialization
    // at the exact reader position or preflight a real private-target clear.
    virtual bool ResolvePrivateTexture(Binding, Dx11RetainedBinding&) const noexcept { return false; }
    // Return an EXACT previously compiled/cached R495 IndexWindow. Never scan
    // CPU indices or hash bytes here. A range not prepared off-draw is unknown.
    virtual bool Indices(const Dx11LeaseView& index, DXGI_FORMAT format,
                         std::uint32_t offset, std::uint32_t first,
                         std::uint32_t count, IndexWindow& out) const noexcept = 0;
};

struct Dx11AliasChange
{
    Id resource = InvalidId;
    // Both remain owned during the transaction and real GPU retirement. The
    // runtime also retains committed aliases beyond this invocation. Changes
    // occur in tape order; repeated assignments are not collapsed/reinterpreted.
    Dx11RetainedBinding previous, next;
};
} // namespace gimi
