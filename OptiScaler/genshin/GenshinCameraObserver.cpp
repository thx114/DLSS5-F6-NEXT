// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

#include "GenshinCameraObserver.h"
#include <SysUtils.h>
#include <cmath>
#include <cstring>

namespace GenshinCameraObserver {

// ============================================================================
// Tier 1: ConstantBufferInspector Implementation
// ============================================================================

std::optional<CameraMatrix> ConstantBufferInspector::InspectPayload(
    const void* cb_data,
    size_t cb_size,
    uint64_t cb_hash
) noexcept {
    if (!cb_data || cb_size < 256) {
        return std::nullopt; // Buffer too small for view+proj+position
    }

    const auto* float_data = static_cast<const float*>(cb_data);
    const size_t max_floats = cb_size / sizeof(float);

    // Try known offsets
    for (size_t offset : KNOWN_OFFSETS) {
        if (offset + 32 > cb_size) continue; // 32 floats = view(16) + proj(16)

        const float* matrix_start = float_data + (offset / sizeof(float));
        const size_t remaining_floats = max_floats - (offset / sizeof(float));

        XMFLOAT4X4 view, projection;
        XMFLOAT4 camera_pos;

        if (TryParseMatrixTriple(matrix_start, remaining_floats, view, projection, camera_pos)) {
            CameraMatrix result;
            result.view = view;
            result.projection = projection;
            result.camera_position = camera_pos;
            result.camera_forward = {0, 0, 1, 0}; // Will be computed from view matrix
            result.source = CameraMatrix::Source::ConstantBuffer;
            return result;
        }
    }

    // If known offsets fail, try scanning through the entire buffer
    // with a sliding window approach (heuristic)
    if (cb_size >= 256 && cb_size <= 4096) {
        // Scan every 16-byte aligned offset for matrix patterns
        for (size_t offset = 0; offset + 128 <= cb_size; offset += 16) {
            const float* candidate = float_data + (offset / sizeof(float));
            const size_t remaining_floats = max_floats - (offset / sizeof(float));

            XMFLOAT4X4 view, projection;
            XMFLOAT4 camera_pos;

            if (TryParseMatrixTriple(candidate, remaining_floats, view, projection, camera_pos)) {
                CameraMatrix result;
                result.view = view;
                result.projection = projection;
                result.camera_position = camera_pos;
                result.camera_forward = {0, 0, 1, 0};
                result.source = CameraMatrix::Source::ConstantBuffer;
                return result;
            }
        }
    }

    return std::nullopt;
}

bool ConstantBufferInspector::TryParseMatrixTriple(
    const float* data,
    size_t max_floats,
    XMFLOAT4X4& out_view,
    XMFLOAT4X4& out_projection,
    XMFLOAT4& out_camera_pos
) noexcept {
    if (max_floats < 36) { // 16 (view) + 16 (proj) + 4 (pos)
        return false;
    }

    // Parse view matrix (4x4 = 16 floats)
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(data[i])) return false;
        out_view.m[i / 4][i % 4] = data[i];
    }

    // Parse projection matrix (4x4 = 16 floats)
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(data[16 + i])) return false;
        out_projection.m[i / 4][i % 4] = data[16 + i];
    }

    // Parse camera position (4 floats)
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(data[32 + i])) return false;
        (&out_camera_pos.x)[i] = data[32 + i];
    }

    // Validate extracted matrices
    if (!ValidateMatrices(out_view, out_projection)) {
        return false;
    }

    return true;
}

bool ConstantBufferInspector::ValidateMatrices(
    const XMFLOAT4X4& view,
    const XMFLOAT4X4& projection
) noexcept {
    // Check that matrices have reasonable scales and determinants
    // Avoid NaN, Inf, and zero-scale matrices

    // Simple heuristic: all elements should be in a reasonable range
    constexpr float MIN_VAL = -1e4f;
    constexpr float MAX_VAL = 1e4f;

    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float v = view.m[i][j];
            float p = projection.m[i][j];

            if (!std::isfinite(v) || !std::isfinite(p)) return false;
            if (v < MIN_VAL || v > MAX_VAL || p < MIN_VAL || p > MAX_VAL) return false;
        }
    }

    // Check that projection has recognizable structure
    // Typical projection matrix has non-zero diagonal elements
    float proj_diag_sum = std::abs(projection.m[0][0]) + std::abs(projection.m[1][1]) +
                           std::abs(projection.m[2][2]) + std::abs(projection.m[3][3]);
    if (proj_diag_sum < 0.1f) return false; // Likely not a real projection

    // Check that view matrix is not degenerate
    float view_diag_sum = std::abs(view.m[0][0]) + std::abs(view.m[1][1]) + std::abs(view.m[2][2]);
    if (view_diag_sum < 0.1f) return false;

    return true;
}

// ============================================================================
// Tier 2: ShaderDisassemblyAnalyzer Implementation
// ============================================================================

std::optional<CameraMatrix> ShaderDisassemblyAnalyzer::AnalyzeShader(
    ID3D11DeviceChild* shader,
    const void* shader_bytecode,
    size_t bytecode_size
) noexcept {
    if (!shader || !shader_bytecode || bytecode_size < 20) {
        return std::nullopt;
    }

    XMFLOAT4X4 view, projection;
    XMFLOAT4 camera_pos;

    // Try DXIL first (modern bytecode format)
    if (TryExtractFromDxil(shader_bytecode, bytecode_size, view, projection, camera_pos)) {
        CameraMatrix result;
        result.view = view;
        result.projection = projection;
        result.camera_position = camera_pos;
        result.camera_forward = {0, 0, 1, 0};
        result.source = CameraMatrix::Source::ShaderDisassembly;
        return result;
    }

    // Fallback to older D3D11 assembly format
    if (TryExtractFromAsm(shader_bytecode, bytecode_size, view, projection, camera_pos)) {
        CameraMatrix result;
        result.view = view;
        result.projection = projection;
        result.camera_position = camera_pos;
        result.camera_forward = {0, 0, 1, 0};
        result.source = CameraMatrix::Source::ShaderDisassembly;
        return result;
    }

    return std::nullopt;
}

bool ShaderDisassemblyAnalyzer::TryExtractFromDxil(
    const void* bytecode,
    size_t size,
    XMFLOAT4X4& out_view,
    XMFLOAT4X4& out_projection,
    XMFLOAT4& out_camera_pos
) noexcept {
    // DXIL format: Complex IL representation
    // For now, this is a stub. Full DXIL parsing would require
    // the DXIL container library or custom binary analysis.
    //
    // The signature for DXIL is:
    //   Magic: 0x4C495844 ('DXIL')
    //   Version: Follows version rules
    //
    // Real implementation would decode container, parse modules,
    // and search for constant definitions matching camera patterns.

    if (size < 4) return false;

    const uint32_t* magic = static_cast<const uint32_t*>(bytecode);
    if (*magic != 0x4C495844) return false; // Not DXIL ('DXIL')

    // Stub: Cannot extract without full DXIL parser
    // For Genshin specifically, camera constants may be baked during
    // shader compilation or accessed via runtime CB injection.
    // This tier remains UNPROVE pending actual implementation.

    return false; // Not yet implemented
}

bool ShaderDisassemblyAnalyzer::TryExtractFromAsm(
    const void* bytecode,
    size_t size,
    XMFLOAT4X4& out_view,
    XMFLOAT4X4& out_projection,
    XMFLOAT4& out_camera_pos
) noexcept {
    // D3D11 assembly format analysis
    // Looking for patterns like:
    //   def cN, f1, f2, f3, f4
    //   mov r0, cN
    //
    // This is a simplified pattern matcher. Real shader disassembly
    // would use D3DDisassemble or equivalent.

    if (size < 32) return false;

    // Check for D3D11 shader header signature
    // (Version and type info in first DWORD)
    const uint32_t header = *static_cast<const uint32_t*>(bytecode);
    const uint32_t version = (header >> 0) & 0xFF;
    const uint32_t shader_type = (header >> 16) & 0xFF;

    // Type 0 = PixelShader, 1 = VertexShader, 2 = GeometryShader, etc.
    if (shader_type > 10) return false; // Likely not valid

    // Stub: Full assembly parsing would scan for constant definitions
    // and extract floating-point literals that match matrix patterns.
    // For now, return false (not implemented).

    return false; // Not yet implemented
}

// ============================================================================
// Tier 3: SrParameterInferencer Implementation
// ============================================================================

std::optional<CameraMatrix> SrParameterInferencer::InferFromSrContext(
    ID3D11DeviceContext* context,
    ID3D11Resource* depth_resource,
    UINT depth_mip_level,
    UINT depth_format,
    const D3D11_VIEWPORT* viewport
) noexcept {
    if (!context || !depth_resource || !viewport) {
        return std::nullopt;
    }

    // Tier 3 inference: Use viewport and resource metadata to guess
    // reasonable camera parameters as a fallback.
    //
    // This is very weak evidence and should only be used when
    // Tier 1 and Tier 2 have failed completely.

    // Extract viewport dimensions
    float viewport_width = viewport->Width;
    float viewport_height = viewport->Height;
    float near_plane = viewport->MinDepth;
    float far_plane = viewport->MaxDepth;

    if (viewport_width <= 0 || viewport_height <= 0) {
        return std::nullopt;
    }

    // Build a reasonable default projection matrix from viewport
    // Using a symmetric perspective projection
    float aspect = viewport_width / viewport_height;
    float fov_y = 3.14159265f / 4.0f; // 45 degrees

    float f = 1.0f / std::tan(fov_y / 2.0f);
    float range = near_plane - far_plane;

    CameraMatrix result;

    // Projection matrix (column-major for DirectX)
    result.projection = {
        f / aspect, 0, 0, 0,
        0, f, 0, 0,
        0, 0, (near_plane + far_plane) / range, -1,
        0, 0, (2.0f * near_plane * far_plane) / range, 0
    };

    // View matrix: identity (assumes camera at origin looking down +Z)
    result.view = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };

    // Camera position: origin
    result.camera_position = {0, 0, 0, 1};
    result.camera_forward = {0, 0, 1, 0};
    result.source = CameraMatrix::Source::SrParameterInference;

    return result;
}

XMFLOAT4X4 SrParameterInferencer::BuildIdentityMatrix() noexcept {
    return {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
}

// ============================================================================
// CameraExtractor Implementation
// ============================================================================

ObservationResult CameraExtractor::ExtractCamera(
    const void* cb_payload_data,
    size_t cb_payload_size,
    uint64_t cb_payload_hash,
    ID3D11DeviceChild* associated_shader,
    const void* shader_bytecode,
    size_t shader_bytecode_size,
    ID3D11DeviceContext* context,
    ID3D11Resource* depth_resource,
    UINT depth_mip,
    UINT depth_format,
    const D3D11_VIEWPORT* viewport
) noexcept {
    ObservationResult result;
    result.observation_index = observation_index_++;
    result.observation_time_ns = GetPerformanceCounterNs();

    ++stats_.extraction_attempts;

    // Tier 1: Try constant buffer inspection
    if (auto cam = ConstantBufferInspector::InspectPayload(cb_payload_data, cb_payload_size, cb_payload_hash)) {
        ++stats_.tier1_successes;
        result.succeeded = true;
        result.camera_matrix = cam.value();
        result.camera_matrix.observation_index = result.observation_index;
        result.camera_matrix.observation_time_ns = result.observation_time_ns;
        last_camera_ = result.camera_matrix;
        return result;
    }

    // Tier 2: Try shader disassembly analysis
    if (auto cam = ShaderDisassemblyAnalyzer::AnalyzeShader(associated_shader, shader_bytecode, shader_bytecode_size)) {
        ++stats_.tier2_successes;
        result.succeeded = true;
        result.camera_matrix = cam.value();
        result.camera_matrix.observation_index = result.observation_index;
        result.camera_matrix.observation_time_ns = result.observation_time_ns;
        last_camera_ = result.camera_matrix;
        return result;
    }

    // Tier 3: Try SR parameter inference (fallback)
    if (auto cam = SrParameterInferencer::InferFromSrContext(context, depth_resource, depth_mip, depth_format, viewport)) {
        ++stats_.tier3_successes;
        result.succeeded = true;
        result.camera_matrix = cam.value();
        result.camera_matrix.observation_index = result.observation_index;
        result.camera_matrix.observation_time_ns = result.observation_time_ns;
        last_camera_ = result.camera_matrix;
        return result;
    }

    // All tiers failed
    ++stats_.all_failures;
    result.succeeded = false;
    result.failure_reason = "All extraction tiers exhausted: CB pattern not found, "
                            "shader analysis failed, SR inference unavailable";
    return result;
}

// ============================================================================
// Logging and Diagnostics
// ============================================================================

void LogObservation(
    const ObservationResult& result,
    const char* context_name
) noexcept {
    if (result.succeeded) {
        const char* source_str = "UNKNOWN";
        switch (result.camera_matrix.source) {
            case CameraMatrix::Source::ConstantBuffer:      source_str = "CONSTANT_BUFFER"; break;
            case CameraMatrix::Source::ShaderDisassembly:   source_str = "SHADER_DISASSEMBLY"; break;
            case CameraMatrix::Source::SrParameterInference: source_str = "SR_INFERENCE"; break;
            default: break;
        }

        LOG_INFO(
            "Genshin camera observation extracted: "
            "context={} index={} source={} "
            "view_m00={:.4f} proj_m00={:.4f} camera_pos=({:.2f},{:.2f},{:.2f}) "
            "semantic=UNPROVE association=UNPROVE",
            context_name ? context_name : "default",
            result.camera_matrix.observation_index,
            source_str,
            result.camera_matrix.view.m[0][0],
            result.camera_matrix.projection.m[0][0],
            result.camera_matrix.camera_position.x,
            result.camera_matrix.camera_position.y,
            result.camera_matrix.camera_position.z
        );
    } else {
        LOG_INFO(
            "Genshin camera observation failed: "
            "context={} reason={} semantic=UNPROVE",
            context_name ? context_name : "default",
            result.failure_reason
        );
    }
}

void LogStatistics(
    const CameraExtractor::Statistics& stats,
    uint64_t process_id
) noexcept {
    LOG_INFO(
        "Genshin camera extractor statistics: "
        "pid={} attempts={} tier1_pass={} tier2_pass={} tier3_pass={} all_fail={} "
        "semantic=UNPROVE",
        process_id,
        stats.extraction_attempts,
        stats.tier1_successes,
        stats.tier2_successes,
        stats.tier3_successes,
        stats.all_failures
    );
}

} // namespace GenshinCameraObserver
