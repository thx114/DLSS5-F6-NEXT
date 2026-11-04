// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d11_4.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <cstdint>
#include <optional>

namespace GenshinCameraObserver {

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// Semantic contract: All returned camera matrices are marked UNPROVE until
// user visual acceptance via depth preview or FG output.
// No gate is weakened, and this observer cannot mint depth/FG admission facts.

struct CameraMatrix {
    XMFLOAT4X4 view;
    XMFLOAT4X4 projection;
    XMFLOAT4 camera_position;
    XMFLOAT4 camera_forward;

    // Metadata: which extraction strategy was used
    enum class Source : unsigned char {
        Unknown,
        ConstantBuffer,      // Tier 1: Direct CB layout recognition
        ShaderDisassembly,   // Tier 2: Hardcoded shader constants
        SrParameterInference // Tier 3: Reverse inference from SR params
    } source = Source::Unknown;

    // Process-lifetime tracking
    uint64_t observation_index = 0;
    uint64_t observation_time_ns = 0;
};

// Observation result: Always carries semantic=UNPROVE marker
struct ObservationResult {
    bool succeeded = false;
    CameraMatrix camera_matrix;
    const char* failure_reason = nullptr; // non-null if succeeded==false
};

// ============================================================================
// Tier 1: Direct Constant Buffer Inspection
// ============================================================================

// Strategy: Scan CB payloads for known view/projection matrix layouts
// Looks for 4x4 float matrices at specific offsets or detectable patterns
class ConstantBufferInspector {
public:
    // Try to extract camera from a D3D11 CB payload
    // Returns nullopt if pattern not recognized
    static std::optional<CameraMatrix> InspectPayload(
        const void* cb_data,
        size_t cb_size,
        uint64_t cb_hash
    ) noexcept;

private:
    // Known Genshin CB layouts (non-exhaustive, collected from shader analysis)
    static constexpr unsigned LAYOUT_MAX_ATTEMPT = 16;

    // CB offset patterns observed in Genshin shaders
    // These are heuristics only; game updates may change them
    static constexpr size_t KNOWN_OFFSETS[] = {
        0,      // Start of buffer
        64,     // Common alignment (4x 4x4 matrices)
        128,    // After first matrix pair
        192,    // Triple matrix offset
        256,    // Page-aligned offset
    };

    // Try to interpret 16 consecutive floats as view+projection+position
    static bool TryParseMatrixTriple(
        const float* data,
        size_t max_floats,
        XMFLOAT4X4& out_view,
        XMFLOAT4X4& out_projection,
        XMFLOAT4& out_camera_pos
    ) noexcept;

    // Validate that extracted matrices are plausible
    // (e.g., non-degenerate, not NaN/Inf, reasonable scale)
    static bool ValidateMatrices(
        const XMFLOAT4X4& view,
        const XMFLOAT4X4& projection
    ) noexcept;
};

// ============================================================================
// Tier 2: Shader Disassembly and Hardcoded Constant Detection
// ============================================================================

// Strategy: Parse FSR Bridge shader bytecode for hardcoded camera parameters
// Looks for patterns like:
//   def cN, <float>, <float>, <float>, <float>  (literal constant definitions)
//   mov r0, cN                                    (camera matrix loads)
class ShaderDisassemblyAnalyzer {
public:
    // Analyze a compiled shader (DXIL or older D3D11 bytecode)
    // Returns nullopt if no camera-like constants found
    static std::optional<CameraMatrix> AnalyzeShader(
        ID3D11DeviceChild* shader,
        const void* shader_bytecode,
        size_t bytecode_size
    ) noexcept;

private:
    // Pattern signatures for common camera matrix patterns
    // (These are signatures of known shader binaries from Genshin)
    static constexpr uint64_t KNOWN_SHADER_HASHES[] = {
        0x778E7E69BB2060F5ull,  // SceneVs hash (from GenshinDepthEvidence.h)
        0x504428132CA63202ull,  // PictureVs hash
    };

    // Try to extract constants from DXIL intermediate representation
    static bool TryExtractFromDxil(
        const void* bytecode,
        size_t size,
        XMFLOAT4X4& out_view,
        XMFLOAT4X4& out_projection,
        XMFLOAT4& out_camera_pos
    ) noexcept;

    // For D3D11 assembly (if bytecode is in older format)
    static bool TryExtractFromAsm(
        const void* bytecode,
        size_t size,
        XMFLOAT4X4& out_view,
        XMFLOAT4X4& out_projection,
        XMFLOAT4& out_camera_pos
    ) noexcept;
};

// ============================================================================
// Tier 3: Inference from SR Parameters
// ============================================================================

// Strategy: Use Opt-known SR parameters and resource bindings to infer
// camera transformation. This is the weakest signal but may succeed
// when CB and shader analysis fail.
class SrParameterInferencer {
public:
    // Try to infer camera from SR context and depth resource metadata
    // This requires the SR ticket, viewport, and depth resource info
    static std::optional<CameraMatrix> InferFromSrContext(
        ID3D11DeviceContext* context,
        ID3D11Resource* depth_resource,
        UINT depth_mip_level,
        UINT depth_format,
        const D3D11_VIEWPORT* viewport
    ) noexcept;

private:
    // Attempt identity or reciprocal matrix inference if all else fails
    static XMFLOAT4X4 BuildIdentityMatrix() noexcept;
};

// ============================================================================
// Main Observer Interface
// ============================================================================

class CameraExtractor {
public:
    CameraExtractor() = default;
    ~CameraExtractor() = default;

    CameraExtractor(const CameraExtractor&) = delete;
    CameraExtractor& operator=(const CameraExtractor&) = delete;

    // Main extraction entry point: try all three strategies in order
    // Returns detailed result (success or failure reason)
    ObservationResult ExtractCamera(
        const void* cb_payload_data,
        size_t cb_payload_size,
        uint64_t cb_payload_hash,
        ID3D11DeviceChild* associated_shader,
        const void* shader_bytecode,
        size_t shader_bytecode_size,
        // Context for Tier 3 inference fallback
        ID3D11DeviceContext* context = nullptr,
        ID3D11Resource* depth_resource = nullptr,
        UINT depth_mip = 0,
        UINT depth_format = 0,
        const D3D11_VIEWPORT* viewport = nullptr
    ) noexcept;

    // Query last successful observation
    const CameraMatrix* LastSuccessful() const noexcept { return last_camera_.get(); }

    // Observation statistics (process-lifetime)
    struct Statistics {
        uint64_t extraction_attempts = 0;
        uint64_t tier1_successes = 0;     // ConstantBuffer
        uint64_t tier2_successes = 0;     // ShaderDisassembly
        uint64_t tier3_successes = 0;     // SrParameterInference
        uint64_t all_failures = 0;
    };

    Statistics GetStatistics() const noexcept { return stats_; }

private:
    std::optional<CameraMatrix> last_camera_;
    Statistics stats_;
    uint64_t observation_index_ = 0;
};

// ============================================================================
// Logging and Diagnostics (semantic=UNPROVE marked)
// ============================================================================

// Log an observation attempt and result
void LogObservation(
    const ObservationResult& result,
    const char* context_name = nullptr
) noexcept;

// Log extraction statistics
void LogStatistics(
    const CameraExtractor::Statistics& stats,
    uint64_t process_id
) noexcept;

} // namespace GenshinCameraObserver
