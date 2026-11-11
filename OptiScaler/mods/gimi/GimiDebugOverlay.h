#pragma once

#include "GimiPlan.h"
#include <wrl/client.h>

namespace gimi
{
struct DebugOverlayStages
{
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
};
class DebugOverlayLibrary final
{
public:
    std::shared_ptr<const Plan> plan;
    std::vector<DebugOverlayStages> stages, mouseStages;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> mouseRaster;
    std::array<Microsoft::WRL::ComPtr<ID3D11BlendState>, 2> mouseBlend;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend;
    // No sampler or depth override: original CustomShader inherits both.
};
// Existing Load/serial Present resource preparation only. Source equality,
// compilation/reflection and all device creation finish before publication.
std::shared_ptr<const DebugOverlayLibrary> CreateDebugOverlayLibrary(
    std::shared_ptr<const Plan>, ID3D11Device*, std::vector<Diagnostic>&);
} // namespace gimi
