// SPDX-License-Identifier: GPL-3.0-or-later
// CPU-only policy tests. No GPU, SDK, game or resource lifetime is simulated.
#include "../../runtime/StarRailResidencyPolicy.h"
#include "../../framegen/dlssg/DlssgInputContinuity.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

using namespace StarRailResidency;
namespace
{
unsigned checks = 0;
void Check(bool condition) { assert(condition); ++checks; }
void LiveFgGapPolicyAndSuppression()
{
    // These are the production selection and Off/null-tag orchestration called
    // by DLSSG_Dx12. SDK callbacks are fakes; no GPU completion is claimed.
    for (bool force : {false, true}) for (bool held : {false, true})
    for (bool counted : {false, true}) for (std::uint64_t distance = 0; distance < 9; ++distance)
    {
        const auto action = SelectFgGapAction(force, held, counted, distance);
        if (held) Check(action == FgGapAction::SuppressHeld);
        else if (force) Check(action == FgGapAction::Suspend);
        else if (counted) Check(action == FgGapAction::AlreadyCounted);
        else Check(action == (distance > 3 ? FgGapAction::Suspend : FgGapAction::CountPresent));
    }
    for (unsigned fault = 0; fault < 6; ++fault)
    {
        unsigned offCalls = 0, tagCalls = 0;
        bool current = fault != 1;
        const bool accepted = SuppressFgGap(false, [&] { return current; }, [&] {
            ++offCalls;
            if (fault == 3) current = false; // Vendor callback replaced owner/source.
            return fault != 2;
        }, [&] {
            Check(offCalls == 1); // Off must precede every null-tag attempt.
            ++tagCalls;
            if (fault == 5) current = false;
            return fault != 4;
        });
        Check(accepted == (fault == 0));
        Check(offCalls == (fault == 1 ? 0u : 1u));
        Check(tagCalls == (fault == 0 || fault >= 4 ? 1u : 0u));
    }
    unsigned offCalls = 0, tagCalls = 0;
    for (unsigned i = 0; i < 1000; ++i)
        Check(SuppressFgGap(true, [] { return true; }, [&] { ++offCalls; return true; },
            [&] { ++tagCalls; return true; }));
    Check(offCalls == 0 && tagCalls == 1000); // Cache options Off, NEVER frame-token tags.
    Check(!SuppressFgGap(true, [] { return false; }, [&] { ++offCalls; return true; },
        [&] { ++tagCalls; return true; }));
    Check(offCalls == 0 && tagCalls == 1000); // A cached Off cannot grant a replacement owner.
    Check(!SuppressFgGap(true, [] { return true; }, [] { return true; }, [] { return false; }));
    unsigned frame = 17;
    const auto snapshot = frame;
    Check(!SuppressFgGap(true, [&] { return frame == snapshot; }, [] { return true; }, [&] {
        ++frame; // Reacquired token/tag callback advanced the real source.
        return true;
    }));

    // Exercise the actual guide-provenance type used by the live adapter.
    dlssg::InputContinuity<3> inputs;
    dlssg::InputOwner owner{};
    owner.device = 0x100; owner.queue = 0x200; owner.epoch = 1;
    owner.width = 1920; owner.height = 1080;
    inputs.Observe(1, 10, owner, true, inputs.SourceGeneration());
    inputs.Observe(1, 10, owner, false, inputs.SourceGeneration());
    inputs.Hold(10, owner);
    Check(!inputs.Fresh(1, 10, 10, owner)); // Old same-source tags cannot resume a gap.
    inputs.Observe(2, 11, owner, false, inputs.SourceGeneration());
    Check(!inputs.Fresh(2, 11, 10, owner)); // Current motion without depth still cannot generate.
    inputs.Observe(2, 11, owner, true, inputs.SourceGeneration());
    Check(inputs.Fresh(2, 11, 10, owner));
    auto changed = owner;
    ++changed.queue;
    Check(!inputs.Fresh(2, 11, 10, changed));
    changed = owner; ++changed.epoch;
    Check(!inputs.Fresh(2, 11, 10, changed));
    inputs.Resume();
    Check(!inputs.Held());
    const auto oldGeneration = inputs.SourceGeneration();
    Check(inputs.RestartSource(2, owner));
    inputs.Observe(0, 3, owner, true, oldGeneration);
    inputs.Observe(0, 3, owner, false, oldGeneration);
    Check(!inputs.Fresh(0, 3, 11, owner, false));
    inputs.Observe(0, 3, owner, true, inputs.SourceGeneration());
    inputs.Observe(0, 3, owner, false, inputs.SourceGeneration());
    Check(inputs.Fresh(0, 3, 11, owner, false));
}
}
int main()
{
    LiveFgGapPolicyAndSuppression();
    std::printf("StarRailResidencyPolicy: %u CPU checks passed\n", checks);
}
