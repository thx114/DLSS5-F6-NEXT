// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <cstring>

namespace FramePipeline {
namespace detail {
// D3D getters already return one owned reference per populated slot. A single
// array owns those results, including all slots after an early mismatch.
template<class Interface,size_t N>
class Dx11GetterReferences final {
    std::array<Interface*,N> values_{};
public:
    Dx11GetterReferences()=default;
    Dx11GetterReferences(const Dx11GetterReferences&)=delete;
    Dx11GetterReferences& operator=(const Dx11GetterReferences&)=delete;
    ~Dx11GetterReferences(){for(auto* value:values_)if(value)value->Release();}
    Interface** data()noexcept {return values_.data();}
    size_t size()const noexcept {return N;}
    Interface* operator[](size_t index)const noexcept {return values_[index];}
};
}
// Re-read public D3D11 bindings against a retained snapshot. The snapshot owns
// every compared interface for the duration of this call. This verifies binding
// identity only: draw arguments, owner/config epochs, query coverage and mutable
// resource contents still require their original independent checks.
// Immutable shader/view/state descriptors cannot change while their retained
// interface stays identical. No second numerical capture or budget lease is
// needed to compare those same objects after the original API returns.
template<class Snapshot>
bool MatchesDx11RasterBindings(ID3D11DeviceContext* context, const Snapshot& expected,
                               ID3D11DeviceContext1* constantContext=nullptr) noexcept
{
    using Microsoft::WRL::ComPtr;
    if (!context || expected.viewportCount > expected.viewports.size() ||
        expected.scissorCount > expected.scissors.size()) return false;

    ComPtr<ID3D11DepthStencilView> view;
    context->OMGetRenderTargets(0, nullptr, &view);
    if (view.Get() != expected.view.Get()) return false;
    ComPtr<ID3D11DepthStencilState> depth;
    UINT reference = 0;
    context->OMGetDepthStencilState(&depth, &reference);
    if (depth.Get() != expected.depth.Get() || reference != expected.reference) return false;
    ComPtr<ID3D11Predicate> predicate;
    BOOL predicateValue = FALSE;
    context->GetPredication(&predicate, &predicateValue);
    if (predicate) return false;

    ComPtr<ID3D11GeometryShader> geometry;
    context->GSGetShader(&geometry, nullptr, nullptr);
    if (geometry) return false;
    ComPtr<ID3D11HullShader> hull;
    ComPtr<ID3D11DomainShader> domain;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    UINT classes = 0;
    context->HSGetShader(&hull, nullptr, &classes);
    if (classes || hull.Get() != expected.hs.Get()) return false;
    classes = 0;
    context->DSGetShader(&domain, nullptr, &classes);
    if (classes || domain.Get() != expected.ds.Get()) return false;
    classes = 0;
    context->VSGetShader(&vertex, nullptr, &classes);
    if (classes || vertex.Get() != expected.vs.Get()) return false;
    classes = 0;
    context->PSGetShader(&pixel, nullptr, &classes);
    if (classes || pixel.Get() != expected.ps.Get()) return false;

    ComPtr<ID3D11InputLayout> layout;
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    context->IAGetInputLayout(&layout);
    if (layout.Get() != expected.layout.Get()) return false;
    context->IAGetPrimitiveTopology(&topology);
    if (topology != expected.topology) return false;
    ComPtr<ID3D11RasterizerState> raster;
    context->RSGetState(&raster);
    if (raster.Get() != expected.raster.Get()) return false;

    detail::Dx11GetterReferences<ID3D11Buffer,32> vertices;
    std::array<UINT, 32> strides{}, offsets{};
    context->IAGetVertexBuffers(0, 32, vertices.data(), strides.data(), offsets.data());
    if (strides != expected.strides || offsets != expected.offsets) return false;
    for (size_t i = 0; i < vertices.size(); ++i)
        if (vertices[i] != expected.vertices[i].Get()) return false;
    ComPtr<ID3D11Buffer> indices;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT indexOffset = 0;
    context->IAGetIndexBuffer(&indices, &format, &indexOffset);
    if (indices.Get() != expected.indices.Get() || format != expected.indexFormat ||
        indexOffset != expected.indexOffset) return false;

    std::array<D3D11_VIEWPORT, 16> viewports{};
    std::array<D3D11_RECT, 16> scissors{};
    UINT viewportCount = 16, scissorCount = 16;
    context->RSGetViewports(&viewportCount, viewports.data());
    if (viewportCount != expected.viewportCount ||
        std::memcmp(viewports.data(), expected.viewports.data(), viewportCount * sizeof(D3D11_VIEWPORT))) return false;
    context->RSGetScissorRects(&scissorCount, scissors.data());
    if (scissorCount != expected.scissorCount ||
        std::memcmp(scissors.data(), expected.scissors.data(), scissorCount * sizeof(D3D11_RECT))) return false;

    ComPtr<ID3D11DeviceContext1> context1;
    // A successful interface acquired from this same retained context during
    // capture remains valid. A failed/unknown query keeps the original fallback.
    if (!constantContext) {
        context->QueryInterface(IID_PPV_ARGS(&context1));
        constantContext=context1.Get();
    }
    for (UINT stage = 0; stage < (expected.ds ? 4u : 2u); ++stage)
    {
        detail::Dx11GetterReferences<ID3D11Buffer,14> buffers;
        std::array<UINT, 14> first{}, count{};
        count.fill(4096);
        if (constantContext) switch (stage) {
            case 0: constantContext->VSGetConstantBuffers1(0, 14, buffers.data(), first.data(), count.data()); break;
            case 1: constantContext->PSGetConstantBuffers1(0, 14, buffers.data(), first.data(), count.data()); break;
            case 2: constantContext->HSGetConstantBuffers1(0, 14, buffers.data(), first.data(), count.data()); break;
            case 3: constantContext->DSGetConstantBuffers1(0, 14, buffers.data(), first.data(), count.data()); break;
        } else switch (stage) {
            case 0: context->VSGetConstantBuffers(0, 14, buffers.data()); break;
            case 1: context->PSGetConstantBuffers(0, 14, buffers.data()); break;
            case 2: context->HSGetConstantBuffers(0, 14, buffers.data()); break;
            case 3: context->DSGetConstantBuffers(0, 14, buffers.data()); break;
        }
        for (size_t i = 0; i < buffers.size(); ++i) {
            const auto& binding = expected.constants[stage * 14 + i];
            if (buffers[i] != binding.buffer.Get() || first[i] != binding.first || count[i] != binding.count)
                return false;
        }
    }
    return true;
}
}
