#pragma once
#include "HoYoShadePolicy.h"

namespace HoYoShade
{
// A core frame, not a game submission counter. Only the real reshade_present
// callback advances it. Suppression/failure consumes this frame until then.
struct NaturalFrame
{
    uint64_t epoch = 1;
    bool armed = false, rendered = false, invalid = false;
    bool CanArm() const noexcept { return !armed && !rendered && !invalid; }
    bool Arm() noexcept { if (!CanArm()) return false; armed = true; return true; }
    void Presented() noexcept
    {
        if (epoch == UINT64_MAX) { invalid = true; return; }
        ++epoch; armed = rendered = false;
    }
    void Invalidate() noexcept { invalid = true; }
};

// Type and capability are separate. A raw physical Present does not identify
// whether the vendor generated this particular picture.
enum class OutputKind : unsigned { Original, GeneratedUnavailable, UnknownPhysical };
struct PhysicalSource
{
    uint64_t runtime = 0, lifetime = 0, epoch = 0, serial = 0;
    // True only for a private color copied BEFORE this DX11 source's natural
    // effects. Parent effects stay enabled; this ticket never suppresses parent.
    bool copiedBeforeNatural = false;
    uint64_t copyOwner = 0, copyGeneration = 0, copySerial = 0;
    explicit operator bool() const noexcept { return runtime && lifetime && epoch && serial; }
};
struct OutputToken
{
    uint64_t owner = 0, generation = 0, sequence = 0;
    explicit operator bool() const noexcept { return owner && generation && sequence; }
};
enum class SubmissionState : unsigned { Untouched, RecordedOrUncertain, Submitted };
// This state is shared with the executable CPU admission tests. No retry after
// natural suppression or any owned recording, even when Signal fails.
struct Attempt
{
    SubmissionState state = SubmissionState::Untouched;
    bool naturalSuppressed = false;
    bool CanRetry() const noexcept
    { return !naturalSuppressed && state == SubmissionState::Untouched; }
};
}
