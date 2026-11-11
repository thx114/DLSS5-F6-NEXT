// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d12.h>

namespace NrPresentation
{
// Caller owns equal-size/format resources, the actual DIRECT recording and its
// queue/fence/Reset lifetime. These helpers neither wait nor retain resources.
inline bool RecordCopy(ID3D12GraphicsCommandList* commands, ID3D12Resource* source,
                       D3D12_RESOURCE_STATES sourceState, ID3D12Resource* target,
                       D3D12_RESOURCE_STATES targetState)
{
    if (!commands || !source || !target || source == target) return false;
    D3D12_RESOURCE_BARRIER barriers[2] {};
    for (auto& barrier : barriers) {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    barriers[0].Transition = {source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE};
    barriers[1].Transition = {target, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, targetState, D3D12_RESOURCE_STATE_COPY_DEST};
    commands->ResourceBarrier(2, barriers);
    commands->CopyResource(target, source);
    for (auto& barrier : barriers) {
        const auto before = barrier.Transition.StateBefore;
        barrier.Transition.StateBefore = barrier.Transition.StateAfter;
        barrier.Transition.StateAfter = before;
    }
    commands->ResourceBarrier(2, barriers);
    return true;
}
inline bool RecordInput(ID3D12GraphicsCommandList* commands, ID3D12Resource* raw, ID3D12Resource* work)
{
    return RecordCopy(commands, raw, D3D12_RESOURCE_STATE_PRESENT, work, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
inline bool RecordPublish(ID3D12GraphicsCommandList* commands, ID3D12Resource* completed, ID3D12Resource* output)
{
    return RecordCopy(commands, completed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, output, D3D12_RESOURCE_STATE_PRESENT);
}

// Distinct input/output is a private, whole-picture transaction. Read-only
// stages sample the actual current input without first copying a full image.
// LUT/hold stages require writable input and use the existing private target.
struct ColourInput
{
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};
inline bool CompatibleColourInput(ID3D12Resource* input, ID3D12Resource* output)
{
    if (!input || !output || input == output) return false;
    const auto a = input->GetDesc(), b = output->GetDesc();
    return a.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && a.Dimension == b.Dimension &&
        a.Width == b.Width && a.Height == b.Height && a.Format == b.Format &&
        a.DepthOrArraySize == 1 && b.DepthOrArraySize == 1 && a.MipLevels == 1 && b.MipLevels == 1 &&
        a.SampleDesc.Count == 1 && b.SampleDesc.Count == 1 &&
        !(a.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
        (b.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
}
inline ColourInput PrepareColourInput(ID3D12GraphicsCommandList* commands, ID3D12Resource* input,
    D3D12_RESOURCE_STATES inputState, ID3D12Resource* output, D3D12_RESOURCE_STATES outputState,
    bool needsWritableInput)
{
    if (!commands || !CompatibleColourInput(input, output)) return {};
    if (!needsWritableInput) return {input, inputState}; // records no copy or transition
    if (!RecordCopy(commands, input, inputState, output, outputState)) return {};
    return {output, outputState};
}
}
