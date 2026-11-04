// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d11_4.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <cstdint>

namespace GenshinDepthContract {

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// ============================================================================
// Layer 2 Semantic Contracts: Bridge between original FSR Bridge producer
// and Present NR consumer. All assertions are UNPROVE until user visual
// acceptance via depth preview or FG output.
// ============================================================================

// Contract 1: FSR Bridge Producer Output Guarantee
//
// The Genshin FSR Bridge produces:
// - Upscaled color output (D3D11 or D3D12 surface)
// - Depth used during SR (original depth or intermediate)
// - Motion vectors (if applicable)
//
// This contract documents what the Bridge guarantees and what it does NOT.
namespace BridgeProducerOutput {

// Depth format that FSR Bridge operates on
// Typical formats: R32_FLOAT (linear 0..1), R16_FLOAT, D32_FLOAT (NDC -1..1)
struct DepthSpecification {
    DXGI_FORMAT format;              // DXGI_FORMAT_R32_FLOAT, etc.
    float near_plane = 0.1f;         // Heuristic; exact value UNPROVE
    float far_plane = 1000.0f;       // Heuristic; exact value UNPROVE
    bool is_ndc_minus_one_to_one = true;  // If true: NDC is -1..1; if false: 0..1
    bool is_inverted = false;        // If true: far=0, near=1
};

// Resource layout that Bridge produces
struct ResourceContract {
    ComPtr<ID3D11Resource> original_depth;    // Original game depth before SR
    ComPtr<ID3D11Resource> upscaled_color;    // FSR output (color)
    ComPtr<ID3D11Resource> motion_vectors;    // Optional MV output

    D3D11_TEXTURE2D_DESC original_depth_desc;
    D3D11_TEXTURE2D_DESC upscaled_color_desc;
};

// What Bridge does NOT guarantee:
// ❌ Depth semantics are NOT guaranteed by format alone
// ❌ Motion vectors are NOT pixel-perfect accurate
// ❌ Depth and color are NOT guaranteed to be in-sync
// ❌ Bridge does NOT validate that depth is actually used by game rendering

} // namespace BridgeProducerOutput

// ============================================================================
// Contract 2: Present NR Consumer Requirements
// ============================================================================

namespace PresentNrConsumer {

// Depth format that Present NR can consume
struct DepthRequirement {
    DXGI_FORMAT accepted_formats[8]; // Formats NR can work with
    unsigned format_count = 0;

    bool width_must_match = false;   // Depth width must equal color width?
    bool height_must_match = false;  // Depth height must equal color height?
    bool same_resource = false;      // Depth must be same resource as color?

    // NR may need jitter applied
    bool needs_jitter = false;
    bool needs_motion_vectors = false;
};

// What Present NR provides:
// ✓ Accepts multiple depth formats
// ✓ Can apply internal jitter correction
// ✓ Uses motion vectors if provided
struct ConsumerCapabilities {
    bool supports_0_to_1_depth = true;
    bool supports_ndc_minus_1_to_1 = true;
    bool supports_linear_depth = true;
    bool supports_depth_from_position = true;  // Can compute from position if needed

    // Temporal coherence requirements
    bool requires_frame_sync = false;
    uint64_t max_frame_lag_frames = 2;
};

} // namespace PresentNrConsumer

// ============================================================================
// Contract 3: Depth Transformation Parameters
// ============================================================================

namespace DepthTransformParams {

// Parameters needed for the transformation shader
struct TransformPacket {
    // View and projection matrices from camera observer (UNPROVE)
    XMFLOAT4X4 view_matrix;
    XMFLOAT4X4 projection_matrix;
    XMFLOAT4X4 inv_projection;
    XMFLOAT4X4 inv_view;

    // Source depth format from Bridge
    DXGI_FORMAT source_format;
    bool source_is_ndc = true;
    bool source_is_inverted = false;
    float source_near = 0.1f;
    float source_far = 1000.0f;

    // Target depth format for Present NR
    DXGI_FORMAT target_format;
    float target_scale = 1.0f;  // Scale factor if output is 0..1

    // Jitter and motion correction
    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    bool apply_jitter = false;

    // Optional: MV scale if motion vectors are transformed
    float motion_scale = 1.0f;

    // Timestamp for debugging
    uint64_t timestamp_ns = 0;
    uint64_t frame_index = 0;
};

} // namespace DepthTransformParams

// ============================================================================
// Contract 4: Semantic State Machine
// ============================================================================

namespace DepthSemanticState {

// State machine for depth semantic extraction and transformation
enum class State : unsigned char {
    Uninitialized,           // Initial state
    AwaitingProducerOutput,  // Waiting for Bridge to produce depth
    CameraExtractionFailed,  // Bridge output exists but camera extraction failed
    CameraExtracted,         // Camera observation obtained (UNPROVE)
    TransformationReady,     // Transformation parameters ready (UNPROVE)
    TransformationApplied,   // Transformation shader executed (UNPROVE)
    ConsumerAccepted,        // Present NR has consumed depth (UNPROVE)
    ConsumerRejected,        // Present NR refused depth
    FrameFinalized           // Frame completed with depth result
};

struct StateTransition {
    State from;
    State to;
    const char* reason;
    uint64_t timestamp_ns = 0;
};

// Transition validation (semantic checks only, not gate enforcement)
// Returns true if transition is plausible
inline bool IsValidTransition(State from, State to) noexcept {
    switch (from) {
        case State::Uninitialized:
            return to == State::AwaitingProducerOutput;

        case State::AwaitingProducerOutput:
            return to == State::CameraExtractionFailed || to == State::CameraExtracted;

        case State::CameraExtractionFailed:
            return to == State::AwaitingProducerOutput; // Retry next frame

        case State::CameraExtracted:
            return to == State::TransformationReady || to == State::AwaitingProducerOutput;

        case State::TransformationReady:
            return to == State::TransformationApplied || to == State::AwaitingProducerOutput;

        case State::TransformationApplied:
            return to == State::ConsumerAccepted || to == State::ConsumerRejected || to == State::FrameFinalized;

        case State::ConsumerAccepted:
        case State::ConsumerRejected:
            return to == State::FrameFinalized || to == State::AwaitingProducerOutput;

        case State::FrameFinalized:
            return to == State::AwaitingProducerOutput; // Next frame

        default:
            return false;
    }
}

} // namespace DepthSemanticState

// ============================================================================
// Contract Assertion Helpers (semantic=UNPROVE marked)
// ============================================================================

// Validate that Bridge output meets minimum expectations
inline bool ValidateBridgeOutput(const BridgeProducerOutput::ResourceContract& contract) noexcept {
    return contract.original_depth != nullptr &&
           contract.upscaled_color != nullptr;
}

// Validate that transformation parameters are complete
inline bool ValidateTransformParams(const DepthTransformParams::TransformPacket& params) noexcept {
    // Check that matrices have finite values (basic sanity check)
    auto is_finite = [](const XMFLOAT4X4& m) {
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                if (!std::isfinite(m.m[i][j])) return false;
            }
        }
        return true;
    };

    return is_finite(params.view_matrix) &&
           is_finite(params.projection_matrix) &&
           params.source_format != DXGI_FORMAT_UNKNOWN &&
           params.target_format != DXGI_FORMAT_UNKNOWN;
}

// Log semantic contract violations (observed facts, not conclusions)
void LogContractViolation(
    const char* contract_name,
    const char* violation_description
) noexcept;

// Log successful contract handoff
void LogContractHandoff(
    const char* from_component,
    const char* to_component,
    const DepthTransformParams::TransformPacket& params
) noexcept;

} // namespace GenshinDepthContract
