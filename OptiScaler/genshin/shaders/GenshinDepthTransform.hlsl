// SPDX-License-Identifier: GPL-3.0-or-later
// Genshin Depth Transformation Shader
//
// Purpose: Convert original FSR Bridge depth (NDC or linear) to Present NR format
//
// Semantic Mark: All outputs are UNPROVE until user visual acceptance.
// This shader performs mathematical transformations only; it does not validate
// depth semantics or prove camera correctness.
//
// Input:  Bridge-produced depth (possibly NDC -1..1 or 0..1 linear)
// Output: Present NR-compatible depth format

// ============================================================================
// Constant Buffer: Transformation Parameters
// ============================================================================

cbuffer TransformParams : register(b0)
{
    // View and projection matrices (column-major)
    float4x4 view_matrix;
    float4x4 projection_matrix;
    float4x4 inv_projection;

    // Source depth specification
    float source_near;
    float source_far;
    uint source_is_ndc;           // 1 if NDC, 0 if linear 0..1
    uint source_is_inverted;      // 1 if far=0 near=1, 0 otherwise

    // Target depth specification
    float target_scale;           // Output scale factor
    float jitter_x;
    float jitter_y;
    uint apply_jitter;

    // Metadata
    float timestamp_ms;
    uint frame_index;
    uint2 _pad;
};

// ============================================================================
// Textures and UAVs
// ============================================================================

// Source: Original depth from Bridge
Texture2D<float> src_depth : register(t0);

// Target: Output for Present NR consumption
RWTexture2D<float> dst_depth : register(u0);

// Optional: Texture dimensions (if needed)
// In HLSL compute shader, can query directly via GetDimensions()

// ============================================================================
// Helper Functions
// ============================================================================

// Clamp value to [min, max]
float clamp_value(float val, float minval, float maxval)
{
    return max(minval, min(val, maxval));
}

// Convert NDC depth to linear depth
// Input NDC: -1 (near) to 1 (far)
// Output: linear 0 (near) to 1 (far)
float ndc_to_linear(float ndc_depth, float n, float f)
{
    // Standard NDC to linear formula for perspective projection:
    // linear = (2 * n * f) / (f + n - (2*z - 1) * (f - n))
    // Simplified: linear = (2 * n) / (f + n - ndc_depth * (f - n))

    float denominator = (f + n) - ndc_depth * (f - n);
    if (abs(denominator) < 1e-6) return 0.5; // Avoid division by zero

    return (2.0 * n * f) / denominator;
}

// Convert linear depth (0..1) to NDC depth
// Input: linear 0 (near) to 1 (far)
// Output: NDC -1 (near) to 1 (far)
float linear_to_ndc(float linear_depth, float n, float f)
{
    // Reverse formula:
    // ndc = (n + f) / (n - f) + (2*n*f) / ((n - f) * z)

    if (linear_depth <= 0) return -1.0;
    if (linear_depth >= 1.0) return 1.0;

    float z = linear_depth;
    return (n + f) / (n - f) + (2.0 * n * f) / ((n - f) * z);
}

// Normalize inverted depth (far=0, near=1) to standard (near=0, far=1)
float denormalize_inverted(float inverted)
{
    return 1.0 - inverted;
}

// Apply temporal jitter offset (if needed for NR coherence)
float2 apply_jitter(float2 uv, float2 jitter_offset)
{
    return uv + jitter_offset;
}

// ============================================================================
// Main Compute Shader
// ============================================================================

[numthreads(8, 8, 1)]
void DepthConvert(uint3 dispatch_id : SV_DispatchThreadID)
{
    uint3 pixel_coord = dispatch_id;

    // Get texture dimensions
    uint width, height;
    src_depth.GetDimensions(width, height);

    // Bounds check
    if (pixel_coord.x >= width || pixel_coord.y >= height)
        return;

    // ========================================================================
    // Step 1: Read source depth
    // ========================================================================

    float src_depth_raw = src_depth[pixel_coord.xy];

    // Clamp to valid range (handle potential artifacts)
    src_depth_raw = clamp_value(src_depth_raw, -2.0, 2.0);

    // ========================================================================
    // Step 2: Normalize source depth
    // ========================================================================

    float normalized_depth = src_depth_raw;

    // If source is inverted (far=0, near=1), flip it
    if (source_is_inverted)
    {
        normalized_depth = denormalize_inverted(normalized_depth);
    }

    // Convert NDC to linear if needed
    if (source_is_ndc)
    {
        // NDC is -1..1, convert to linear 0..1
        normalized_depth = ndc_to_linear(normalized_depth, source_near, source_far);
    }

    // Now normalized_depth is in linear 0..1 space
    normalized_depth = clamp_value(normalized_depth, 0.0, 1.0);

    // ========================================================================
    // Step 3: Apply jitter (if NR requires it)
    // ========================================================================

    float2 uv = float2(pixel_coord.xy) / float2(width, height);

    if (apply_jitter)
    {
        float2 jitter = float2(jitter_x, jitter_y);
        uv = apply_jitter(uv, jitter / float2(width, height));

        // Clamp UV to valid texel space
        uv = clamp_value(uv, 0.0, 1.0);
    }

    // ========================================================================
    // Step 4: Transform to target format
    // ========================================================================

    // Target scale: if target is 0..1, use target_scale as-is
    // If target is different format, apply transformation
    float output_depth = normalized_depth * target_scale;

    // Clamp output to reasonable range
    output_depth = clamp_value(output_depth, 0.0, 1.0);

    // ========================================================================
    // Step 5: Write output
    // ========================================================================

    dst_depth[pixel_coord.xy] = output_depth;

    // Semantic mark: Output is UNPROVE
    // No certification of correctness, accuracy, or semantic validity.
    // User visual inspection is the only validation.
}

// ============================================================================
// Alternative Compute Shader: Linearization only (simplified)
// ============================================================================

[numthreads(8, 8, 1)]
void DepthLinearize(uint3 dispatch_id : SV_DispatchThreadID)
{
    // Simpler version: Only linearize NDC depth, no jitter/transform

    uint width, height;
    src_depth.GetDimensions(width, height);

    if (dispatch_id.x >= width || dispatch_id.y >= height)
        return;

    float src = src_depth[dispatch_id.xy];

    // Simple linearization: assume NDC -1..1 to linear 0..1
    float linear = (src + 1.0) * 0.5; // Rough conversion
    linear = clamp_value(linear, 0.0, 1.0);

    dst_depth[dispatch_id.xy] = linear;
}

// ============================================================================
// Diagnostic Compute Shader: Depth visualization
// ============================================================================

[numthreads(8, 8, 1)]
void DepthVisualize(uint3 dispatch_id : SV_DispatchThreadID)
{
    // Output depth as grayscale for debugging
    // Black = near, White = far

    uint width, height;
    src_depth.GetDimensions(width, height);

    if (dispatch_id.x >= width || dispatch_id.y >= height)
        return;

    float src = src_depth[dispatch_id.xy];

    // Normalize to 0..1 for visualization
    float normalized = src;

    // If NDC, convert to 0..1
    if (source_is_ndc)
    {
        normalized = (src + 1.0) * 0.5;
    }

    normalized = clamp_value(normalized, 0.0, 1.0);

    // Output as grayscale
    dst_depth[dispatch_id.xy] = normalized;
}
