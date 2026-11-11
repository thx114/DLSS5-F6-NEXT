// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace FramePipeline {
// Requests for NEW numerical inputs only. Retained readers, maps and GPU
// flights have independent owners and must drain without keeping this active.
struct InputObservationDemand final {
    bool nr=false,fg=false,effects=false,preview=false,capture=false,unknown=false;
    bool Requested() const noexcept {return nr || fg || effects || preview || capture || unknown;}
};
class InputObservationSession final {
    // Unknown until the first real demand boundary: preserve observation.
    bool requested_=true;
public:
    bool Observe(InputObservationDemand demand) noexcept {
        const bool next=demand.Requested();
        const bool changed=next!=requested_;requested_=next;return changed;
    }
    bool Requested() const noexcept {return requested_;}
};
}
