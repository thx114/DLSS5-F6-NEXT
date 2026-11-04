#pragma once

// Which temporal fix the configuration selects for the built-in RTX 40 unlock. Header-only and free of
// OptiScaler headers so tests/mfg_method_smoke.cpp can compile it alone.

#include <optional>
#include <string>

namespace MfgUnlock
{
// How generated frames are given their own time between the two real ones. Above 2X the Ada build of the
// interpolation kernel puts every one of them at the midpoint.
//   Retarget: the module's Blackwell image is relabelled to answer for Ada (RewriteBlackwellKernels).
//   Ptx:      the Ada kernel's PTX is rewritten to blend at each frame's own time (MfgUnlockPtx.h).
// One per session: both edit the same fatbin.
enum class TemporalMethod
{
    None,
    Retarget,
    Ptx,
};

// `fix` is [DLSSG] AdaTemporalFix as read ("Auto", "Retarget" or "Ptx"), `legacy` the older
// AdaBlackwellKernels on/off. A named method wins. "Auto", or nothing, leaves it to the older key (false
// leaves the unlock unapplied), then to the default, Retarget.
inline TemporalMethod ResolveTemporalMethod(const std::optional<std::string>& fix, const std::optional<bool>& legacy)
{
    if (fix.has_value())
    {
        if (*fix == "Retarget")
            return TemporalMethod::Retarget;

        if (*fix == "Ptx")
            return TemporalMethod::Ptx;
    }

    if (legacy.has_value() && !*legacy)
        return TemporalMethod::None;

    return TemporalMethod::Retarget;
}
} // namespace MfgUnlock
