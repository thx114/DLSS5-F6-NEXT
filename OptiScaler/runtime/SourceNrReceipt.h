#pragma once
#include <cstdint>

namespace source_colour
{
enum class Result { Unknown, Original, Modified, NrWritten };

// Synchronous command-recording facts for one dispatch, never GPU completion.
// LUT/held colour may be committed before a later model/resolve failure.
struct Recording
{
    bool modified = false, resolved = false;
    void Write() noexcept { modified = true; }
    void Resolve() noexcept { modified = resolved = true; }
    Result Outcome() const noexcept { return resolved ? Result::NrWritten : modified ? Result::Modified : Result::Original; }
};

// Published by the actual SR owner only after successful source submission and
// DX11 copyback. Identities/order, not GPU completion or resource ownership.
// The existing source owner retains every recording until its real fence/Reset.
struct Receipt
{
    Result result = Result::Unknown;
    uintptr_t feature = 0, output = 0, queue = 0, list = 0, fence = 0;
    uint64_t handle = 0, attempt = 0, fenceValue = 0;

    bool Matches(uintptr_t currentFeature, uint64_t currentHandle, uintptr_t currentOutput) const noexcept
    {
        return result != Result::Unknown && feature == currentFeature && feature && handle == currentHandle &&
            handle && output == currentOutput && output && attempt && attempt != UINT64_MAX &&
            queue && list && fence && fenceValue && fenceValue != UINT64_MAX;
    }
};
}
