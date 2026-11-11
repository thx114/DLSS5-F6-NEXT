#pragma once
#include "HoYoShadeSchedule.h"
#include <array>
#include <cstdint>

namespace HoYoShade
{
// Only Ready makes the remaining fields consumable. Busy/Failed mean UNKNOWN,
// never a disabled core, an empty selection or a zero-technique observation.
enum class StatusRead { Ready, Busy, Failed };
enum class NaturalStatus { Unobserved, UniqueLive, Ambiguous, CapacityExceeded };

struct NaturalSelectionStatus
{
    NaturalStatus status = NaturalStatus::Unobserved;
    // Tracked live sources only, not SDK enumeration. On CapacityExceeded this
    // is a lower bound; completeness remains unknown until process restart.
    uint32_t liveRuntimeCount = 0;
    uint64_t runtime = 0, lifetime = 0;
    unsigned api = 0;
    bool captureObserved = false;
    bool captureReady = false, selectionReady = false;
    bool presetKnown = false;
    std::array<char, 32768> presetPath{}; // UTF-8, NUL terminated; valid iff presetKnown.
    bool globalEnabledKnown = false, globalEnabled = false;
    bool countsKnown = false;
    uint64_t totalTechniques = 0, enabledTechniques = 0;
};

struct LatestStageStatus
{
    // Historical last admitted RenderStage exit, across all sources/stages.
    // Not a per-frame result, GPU completion, current source or pixel proof.
    bool known = false;
    Stage stage = Stage::BeforeNr;
    Reason reason = Reason::Disabled;
    SubmissionState submission = SubmissionState::Untouched;
    uint64_t techniques = 0;
    uint64_t naturalRuntime = 0, naturalLifetime = 0;
    uint64_t stream = 0, generation = 0, frame = 0;
    OutputKind output = OutputKind::Original;
    bool naturalSuppressed = false;
};

struct StatusSnapshot
{
    StatusRead read = StatusRead::Failed;
    // Existing startup atomics: initialized means Initialize was entered, not
    // registration completed. Independent observations, not a startup barrier.
    // These are process-latched state; requested next-start settings come from Config.
    bool initialized = false, accepted = false, enabled = false;
    unsigned stage = 2;
    Reason coreReason = Reason::Disabled;
    NaturalSelectionStatus natural;
    LatestStageStatus latest;
};

// Nonblocking, bounded, by value. No SDK/COM/GPU calls, IO, logging or mutation.
// Selection rule: exactly one tracked live natural runtime, no capacity loss.
// UniqueLive identifies that source only; it does NOT establish an association
// with the game's active color/endpoint. Multiple sources expose Ambiguous and
// leave source fields UNKNOWN; technique totals are never summed globally.
// Capture observations can precede loading completion. Unknown numeric/bool
// slots have no meaning; honor their known flags. Loading is not exposed by API20.
StatusSnapshot ReadStatus() noexcept;
}
