#pragma once
#include <cstdint>

namespace framegen
{
enum class PacingRole : uint8_t { Unknown, Source, GeneratedOutput };
struct SourcePacingInput
{
    PacingRole role = PacingRole::Unknown;
    bool realPresent = false, directDlss = false, ownedDlssgProvider = false;
    bool fgActive = false, ownMarkers = false, tokenLeaseValid = false;
    bool reflexOwnsCap = false, nativeOwnsCap = false, xellOwnsCap = false;
    bool hasFgOutput = false, dxvkExcluded = false;
    unsigned generatedFrames = 0;
};
struct SourcePacingDecision
{
    bool source = false, fgActive = false, reflexSleep = false, frameLimit = false;
    unsigned generatedFrames = 0;
};
class SourcePacingScope final
{
    inline static thread_local bool active_ = false;
    bool owns_ = false;
public:
    explicit SourcePacingScope(bool source) noexcept : owns_(source && !active_)
    { if (owns_) active_ = true; }
    ~SourcePacingScope() { if (owns_) active_ = false; }
    SourcePacingScope(const SourcePacingScope&) = delete;
    bool Owns() const noexcept { return owns_; }
};
constexpr SourcePacingDecision SelectSourcePacing(const SourcePacingInput& input) noexcept
{
    const bool source = input.realPresent && input.role == PacingRole::Source &&
        input.directDlss && input.ownedDlssgProvider;
    // Reflex can reduce latency without owning the cap. Keep that legal
    // combination with an explicit FrameLimit; only cap owners exclude it.
    return { source, source && input.fgActive,
        source && input.fgActive && input.ownMarkers && input.tokenLeaseValid,
        source && input.hasFgOutput && !input.reflexOwnsCap && !input.nativeOwnsCap &&
            !input.xellOwnsCap && !input.dxvkExcluded,
        source && input.fgActive ? input.generatedFrames : 0 };
}
}
