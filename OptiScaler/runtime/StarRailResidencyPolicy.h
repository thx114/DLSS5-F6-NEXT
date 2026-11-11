// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

// FG input-gap policy only. Call under the existing provider owner's lock.
// This header owns no resources, outputs, SDK tokens or fences.
// The caller must still prove source identity, input ordering, recording reuse
// and final presentation through its existing real queue/fence contracts.
namespace StarRailResidency
{
// Called only at the real final SDK Present admission, after the adapter has
// failed to admit a fresh source. Default mode preserves the F5-DS threshold;
// forced residency suppresses generation immediately, including if Present()
// already counted this boundary. Neither mode authorizes no-depth generation.
enum class FgGapAction { SuppressHeld, Suspend, CountPresent, AlreadyCounted };
constexpr FgGapAction SelectFgGapAction(bool forced, bool held, bool counted,
                                      std::uint64_t presentsSinceDispatch) noexcept
{
    if (held) return FgGapAction::SuppressHeld;
    if (forced) return FgGapAction::Suspend;
    if (counted) return FgGapAction::AlreadyCounted;
    return presentsSinceDispatch > 3 ? FgGapAction::Suspend : FgGapAction::CountPresent;
}

// Real adapters supply the actual SDK calls. Check the exact owner/source
// snapshot around EACH fallible/reentrant call; publish Off acknowledgement
// only after this returns true. None of these callbacks proves GPU retirement.
template<class Current, class Off, class ClearTags>
bool SuppressFgGap(bool acknowledged, Current&& current, Off&& off, ClearTags&& clearTags)
{
    if (!current()) return false;
    // Options Off is viewport/session state. Null tags are FRAME based: an Off
    // ACK cannot stand in for clearing tags on a newly acquired frame token,
    // even when the source ordinal is unchanged across multiple Presents.
    if (!acknowledged && (!off() || !current())) return false;
    return clearTags() && current();
}

}
