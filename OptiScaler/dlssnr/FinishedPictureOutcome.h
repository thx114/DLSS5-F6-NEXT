// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <new>
#include <d3d12.h>
#include <wrl/client.h>

namespace GenshinPresentNr { class ReturnedPicture; }

// Shared observation contract; no model, shader, UI, or Config dependencies.
namespace DlssNr
{
enum class FinishedPictureResult : unsigned char
{
    NoWrite,
    WrittenOrdered,
    FailedUncertain
};

enum class FinishedPictureReason : unsigned char
{
    None,
    Disabled,
    InvalidEndpoint,
    UnsupportedColour,
    NoCurrentInput,
    InputNotReady,
    RecordingFailed,
    SubmissionFailed,
    ProviderFailed,
    ModelDisabled
};

// A result is an observation of the actual finished-picture submission. It is
// not a provider-success claim and carries no authority over another route.
struct FinishedPictureOutcome
{
    FinishedPictureResult result = FinishedPictureResult::NoWrite;
    FinishedPictureReason reason = FinishedPictureReason::None;
    uint64_t presentEpoch = 0;
    uint64_t swapchain = 0;
    uint64_t color = 0;
    uint64_t queue = 0;
    uint64_t submissionSerial = 0;
    uint64_t fenceValue = 0;
    // Endpoint identity is independent of whether any commands were submitted.
    uint64_t endpointSerial = 0, owner = 0, device = 0, producerQueue = 0;
    uint32_t thread = 0;
    bool bridgeEndpoint = false;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence; // actual slot completion owner, never a cast of fenceValue

    // Only the real Bridge endpoint forwards this wrapper-issued original
    // receipt. NoWrite alone, including failures/disabled generic calls, has no
    // FG authority. fence/submissionSerial above remain NR-write evidence only.
    std::shared_ptr<const GenshinPresentNr::ReturnedPicture> original;

    bool Written() const noexcept { return result == FinishedPictureResult::WrittenOrdered; }
};

}
