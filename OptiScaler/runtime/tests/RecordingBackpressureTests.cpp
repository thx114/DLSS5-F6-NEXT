// SPDX-License-Identifier: GPL-3.0-or-later
// Exercises the production scalar lifecycle. No GPU completion is simulated
// as a real device receipt, and no SDK/device function is called.
#include "../../inputs/GenshinFsrContract.h"
#include <cassert>
#include <cstdio>

int main()
{
    using namespace GenshinFsr;
    Lifecycle lifecycle;
    assert(lifecycle.Open(7, 9) == Reason::None);
    Parameters frame;
    frame.instance = 7; frame.epoch = 9; frame.generation = 1;
    const auto first = lifecycle.Submitted(frame, 11, true, true, true, true);
    assert(first.disposition == Disposition::QueuedOutput && lifecycle.Pending() && !lifecycle.CanDestroy());
    for (unsigned generation = 2; generation <= 101; ++generation)
    {
        frame.generation = generation;
        // A still-retained no-work EVENT has no ordering proof. Do not relax
        // that check; classify this NEW unsubmitted invocation as retryable.
        auto pending = lifecycle.Check(frame, 11, false);
        assert(pending.reason == Reason::Pending && !pending.unsubmittedRecordingPending);
        pending.NoteUnsubmittedRecordingPending(true, false);
        assert(pending.unsubmittedRecordingPending && !pending.pendingLease);
        assert(pending.DrawAction() == NativeDrawAction::ContinueOriginal);
        assert(!pending.MaySkipOriginalDraw() && lifecycle.Pending() && !lifecycle.CanDestroy());
    }
    frame.generation = 1;
    auto queued = lifecycle.Check(frame, 11, false);
    queued.NoteUnsubmittedRecordingPending(true, false);
    assert(queued.disposition == Disposition::QueuedOutput && !queued.unsubmittedRecordingPending);
    lifecycle.Complete(true); // CPU fixture notification only.
    auto reused = lifecycle.Check(frame, 11, false);
    reused.NoteUnsubmittedRecordingPending(true, false);
    assert(reused.disposition == Disposition::ReusedOutput && !reused.unsubmittedRecordingPending);
    frame.generation = 102;
    const auto ready = lifecycle.Check(frame, 11, false);
    assert(ready.reason == Reason::None && lifecycle.CanDestroy() && !lifecycle.NeedsDrain());

    unsigned rejected = 0;
    for (const auto reason : {Reason::Draining, Reason::CompletionFailure, Reason::BackendFailure,
                              Reason::InvalidIdentity, Reason::StaleGeneration})
    {
        Result result; result.reason = reason;
        result.NoteUnsubmittedRecordingPending(true, false);
        assert(!result.unsubmittedRecordingPending); ++rejected;
    }
    for (unsigned condition = 0; condition < 5; ++condition)
    {
        Result result; result.reason = Reason::Pending;
        if (condition == 0) result.pendingLease = true;
        if (condition == 1) result.stateRestored = false;
        if (condition == 2) result.disposition = Disposition::FailClosed;
        result.NoteUnsubmittedRecordingPending(condition != 3, condition == 4);
        assert(!result.unsubmittedRecordingPending); ++rejected;
    }
    std::printf("RecordingBackpressure: 100 consecutive pending sources and %u refusal cases passed\n", rejected);
}
