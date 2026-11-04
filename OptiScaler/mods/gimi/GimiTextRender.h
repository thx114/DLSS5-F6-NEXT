#pragma once

#include "GimiPlan.h"
#include <d3d11.h>
#include <array>
#include <wrl/client.h>

namespace gimi
{
// Load-time shaders only. A caller must still prove the entire native PRE
// tape, backbuffer, state restoration and fence ownership before any draw.
struct NativeTextStages
{
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
    Microsoft::WRL::ComPtr<ID3D11GeometryShader> gs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
};
class NativeTextShaderLibrary final
{
public:
    std::shared_ptr<const Plan> plan;
    std::vector<NativeTextStages> stages;
};
// Exact installed TextPrinter source and reflected VS/GS/PS t100/t112/
// t113/t114/t120 ABI. No shader bytes from an unverified include/cache.
std::shared_ptr<const NativeTextShaderLibrary> CreateNativeTextShaderLibrary(
    std::shared_ptr<const Plan>, ID3D11Device*, std::vector<Diagnostic>&);

// Per-flight native PRE draw owner. Prepared only for the real swapchain
// backbuffer; all touched state and the backbuffer stay retained until the
// Session fence proves the packet complete. Unknown hazards reject before CS.
class NativeTextDrawState final
{
    using ComRaster = Microsoft::WRL::ComPtr<ID3D11RasterizerState>;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_;
    // Entry roots survive the whole packet; a second preallocated snapshot
    // restores the live tape-position state after EACH text draw. No allocation
    // or independent submission occurs between compute, text and overlay.
    struct Snapshot
    {
        std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthView;
        Microsoft::WRL::ComPtr<ID3D11BlendState> blend;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth;
        ComRaster raster;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
        Microsoft::WRL::ComPtr<ID3D11HullShader> hs;
        Microsoft::WRL::ComPtr<ID3D11DomainShader> ds;
        Microsoft::WRL::ComPtr<ID3D11GeometryShader> gsShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> psShader;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> cs;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav; // exact CS u0, KEEP counter
        std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 5> gs{};
        std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 2> ps{};
        std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
        std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors{};
        std::array<FLOAT, 4> factors{};
        UINT viewportCount = 0, scissorCount = 0, sampleMask = 0, stencilRef = 0;
        D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        bool Capture(ID3D11DeviceContext*) noexcept;
        void Restore(ID3D11DeviceContext*) const noexcept;
        void Reset() noexcept;
    };
    Snapshot entry_, scope_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> overlayBlend_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> overlayDepth_;
    ComRaster overlayRaster_;
    D3D11_VIEWPORT overlayViewport_{};
    bool prepared_ = false;
public:
    bool Initialize(ID3D11Device*) noexcept; // Load only, no Present state-object allocation
    bool Prepare(ID3D11DeviceContext*, IDXGISwapChain*, ID3D11Device*, HWND expectedWindow) noexcept;
    bool Draw(ID3D11DeviceContext*, const NativeTextStages&, ID3D11ShaderResourceView* font,
              ID3D11ShaderResourceView* positions, ID3D11ShaderResourceView* text,
              ID3D11ShaderResourceView* parameters, ID3D11ShaderResourceView* ini) noexcept;
    void Restore(ID3D11DeviceContext*) const noexcept;
    ID3D11Texture2D* Backbuffer() const noexcept { return backbuffer_.Get(); }
    void ReleaseTransient() noexcept; // only after cancellation or actual fence completion
};
} // namespace gimi
