// SPDX-License-Identifier: GPL-3.0-or-later
#include "../NativeProducerCpuTiming.h"

#include <cstdio>
#include <cstdlib>

namespace {
unsigned checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::abort();
    }
}

using NativeProducerCpuTiming::Stage;
using NativeProducerCpuTiming::WindowAccumulator;

void FirstWindowUsesOneSharedSampleFrame() {
    WindowAccumulator window;
    NativeProducerCpuTiming::Snapshot snapshot;
    for (uint64_t frame = 1; frame <= 65; ++frame) {
        const bool sampled = window.BeginFrame(frame, frame == 1 ? 100 : 100);
        Check(sampled == (((frame - 1) & 7) == 0), "sampling follows stable 1/8 frame stride");
        window.RecordCall(Stage::Acquire);
        window.RecordCall(Stage::Run);
        window.RecordCall(Stage::AllocatorWait);
        window.RecordCall(Stage::Return);
        if (sampled) {
            window.RecordDuration(Stage::Acquire, frame, 10);
            window.RecordDuration(Stage::Run, frame, 5);
            window.RecordDuration(Stage::AllocatorWait, frame, 2);
        }
        if (frame == 65)
            Check(window.EndFrame(1100, 1000, 1, snapshot), "one-second window publishes at frame boundary");
    }

    const auto& acquire = snapshot.stages[static_cast<unsigned>(Stage::Acquire)];
    const auto& run = snapshot.stages[static_cast<unsigned>(Stage::Run)];
    const auto& allocator = snapshot.stages[static_cast<unsigned>(Stage::AllocatorWait)];
    const auto& returned = snapshot.stages[static_cast<unsigned>(Stage::Return)];
    Check(snapshot.sequence == 1 && snapshot.frames == 65 && snapshot.firstFrame == 1 && snapshot.lastFrame == 65,
          "snapshot identifies exact frame window");
    Check(acquire.calls == 65 && acquire.samples == 9 && acquire.lastSampleFrame == 65 && acquire.estimateKnown,
          "acquire calls and sampled duration are retained");
    Check(acquire.estimatedMsPerSecond == 650.0, "acquire estimate scales sampled mean by calls and window length");
    Check(run.samples == acquire.samples && run.lastSampleFrame == acquire.lastSampleFrame &&
          run.estimatedMsPerSecond == 325.0, "run uses the same sampled frame IDs");
    Check(allocator.samples == acquire.samples && allocator.lastSampleFrame == acquire.lastSampleFrame &&
          allocator.estimatedMsPerSecond == 130.0, "allocator wait uses the same sampled frame IDs");
    Check(returned.calls == 65 && returned.samples == 0 && !returned.estimateKnown,
          "missing sampled stage durations remain unknown");
    Check(!window.EndFrame(1100, 1000, 2, snapshot), "completed window is not republished");
}

void NextWindowStartsAtItsOwnFirstFrame() {
    WindowAccumulator window;
    NativeProducerCpuTiming::Snapshot snapshot;
    for (uint64_t frame = 66; frame <= 129; ++frame) {
        const bool sampled = window.BeginFrame(frame, frame == 66 ? 1100 : 1100);
        Check(sampled == (((frame - 1) & 7) == 0), "sampling phase remains stable across windows");
        window.RecordCall(Stage::Acquire);
        if (sampled) window.RecordDuration(Stage::Acquire, frame, 20);
        if (frame == 129)
            Check(window.EndFrame(2100, 1000, 2, snapshot), "second one-second window publishes");
    }
    const auto& acquire = snapshot.stages[static_cast<unsigned>(Stage::Acquire)];
    Check(snapshot.sequence == 2 && snapshot.frames == 64 && snapshot.firstFrame == 66 && snapshot.lastFrame == 129,
          "subsequent window records its real first frame");
    Check(acquire.calls == 64 && acquire.samples == 8 && acquire.lastSampleFrame == 129 &&
          acquire.estimatedMsPerSecond == 1280.0, "sparse sampled window estimate stays explicit");
}

void ClockAndFrequencyFailuresStayUnknown() {
    WindowAccumulator window;
    NativeProducerCpuTiming::Snapshot snapshot;
    for (uint64_t frame = 1; frame <= 2; ++frame) {
        const bool sampled = window.BeginFrame(frame, 500);
        window.RecordCall(Stage::Return);
        if (sampled) {
            window.RecordDuration(Stage::Return, frame, 4);
            window.RecordClockFailure(Stage::Return);
        }
    }
    Check(window.EndFrame(1500, 1000, 1, snapshot), "failure fixture publishes its window");
    Check(!snapshot.stages[static_cast<unsigned>(Stage::Return)].estimateKnown,
          "any sampled clock failure invalidates estimate");

    WindowAccumulator noFrequency;
    noFrequency.BeginFrame(1, 700);
    noFrequency.RecordCall(Stage::Run);
    noFrequency.RecordDuration(Stage::Run, 1, 4);
    Check(noFrequency.EndFrame(1700, 0, 1, snapshot), "zero-frequency fixture publishes");
    Check(!snapshot.stages[static_cast<unsigned>(Stage::Run)].estimateKnown,
          "missing QPC frequency leaves estimate unknown");
}
} // namespace

int main() {
    FirstWindowUsesOneSharedSampleFrame();
    NextWindowStartsAtItsOwnFirstFrame();
    ClockAndFrequencyFailuresStayUnknown();
    std::printf("NativeProducerCpuTiming: %u checks passed; sampled CPU wall windows only\n", checks);
}
