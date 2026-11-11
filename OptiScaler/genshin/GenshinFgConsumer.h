// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Config.h>

namespace GenshinFgConsumer
{
// Demand only, never provider admission or a GPU-completion witness. In
// particular, selected DLSSG must bootstrap without an already-ready provider.
// Missing/unknown selections retain observation rather than guessing NoFG.
inline bool NewWorkRequested() noexcept
{
    const auto& cfg=*Config::Instance();
    if(!cfg.FGEnabled.ObservedValue())return false;
    const auto output=cfg.FGOutput.ReadObserved();
    if(!output || *output!=FGOutput::NoFG)return true;
    const auto selected=cfg.FGInput.ReadObserved();
    if(!selected)return true;
    const auto input=*selected;
    // A native input/replacement can use NoFG output to mean passthrough. Only
    // the explicit bridge/upscaler or fully-disabled input proves no new work.
    return input!=FGInput::Upscaler && input!=FGInput::NoFG;
}

// At existing owner/claim/Present boundaries, retain new demand and a pending
// switch owned by a real provider. A retained provider object alone is not a
// continuing consumer; separate reader/retirement checks still protect old work.
// ResizeBuffers can leave fgChanged set even with NoFG and no provider. That
// ownerless flag must not keep optional observation subscribed indefinitely.
// Re-enabling FG restores demand immediately; existing guide warmup still applies,
// including selected DLSSG bootstrap before its provider is ready.
// Do not call IsActive/IsReady/IsPaused: none proves that old GPU readers retired.
// These are scalar reads, also usable as an immediate subscription invalidator;
// there is no per-draw lock, allocation, provider call or resource scan here.
inline bool HandoffRequired() noexcept
{
    const auto& state=State::Instance();
    return NewWorkRequested() ||
        (state.currentFG && state.fgChanged);
}
// False only permits the existing OptionalObservationIdle/reader/retirement
// checks at the owner boundary. It never releases a lease or skips its Poll.
inline bool FullObservationRequired() noexcept {return HandoffRequired();}
}
