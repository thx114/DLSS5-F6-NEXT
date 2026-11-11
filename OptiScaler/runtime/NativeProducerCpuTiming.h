// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Windows.h>
#include <array>
#include <cstdint>

namespace NativeProducerCpuTiming {

inline constexpr uint64_t SampleStride = 8;
inline constexpr uint64_t WindowMs = 1000;

enum class Stage : unsigned { Acquire, Run, Return, AllocatorWait, Count };
inline constexpr unsigned StageCount = static_cast<unsigned>(Stage::Count);

struct Reading {
    uint64_t calls = 0;
    uint64_t samples = 0;
    uint64_t ticks = 0;
    uint64_t maxTicks = 0;
    uint64_t clockFailures = 0;
    uint64_t lastSampleFrame = 0;
    bool estimateKnown = false;
    double estimatedMsPerSecond = 0;
};

struct Snapshot {
    uint64_t sequence = 0;
    uint64_t beginMs = 0;
    uint64_t endMs = 0;
    uint64_t firstFrame = 0;
    uint64_t lastFrame = 0;
    uint64_t frames = 0;
    uint64_t qpcFrequency = 0;
    std::array<Reading, StageCount> stages{};
};

class WindowAccumulator final {
    bool active_ = false;
    uint64_t beginMs_ = 0;
    uint64_t firstFrame_ = 0;
    uint64_t lastFrame_ = 0;
    uint64_t frames_ = 0;
    std::array<Reading, StageCount> stages_{};

public:
    bool BeginFrame(uint64_t frame, uint64_t nowMs) noexcept {
        if (!frame) return false;
        if (!active_) {
            active_ = true;
            beginMs_ = nowMs;
            firstFrame_ = frame;
        }
        lastFrame_ = frame;
        ++frames_;
        return ((frame - 1) & (SampleStride - 1)) == 0;
    }

    void RecordCall(Stage stage) noexcept {
        const auto i = static_cast<unsigned>(stage);
        if (i < StageCount) ++stages_[i].calls;
    }

    void RecordDuration(Stage stage, uint64_t frame, uint64_t ticks) noexcept {
        const auto i = static_cast<unsigned>(stage);
        if (i >= StageCount) return;
        auto& reading = stages_[i];
        ++reading.samples;
        reading.ticks += ticks;
        reading.lastSampleFrame = frame;
        if (reading.maxTicks < ticks) reading.maxTicks = ticks;
    }

    void RecordClockFailure(Stage stage) noexcept {
        const auto i = static_cast<unsigned>(stage);
        if (i < StageCount) ++stages_[i].clockFailures;
    }

    bool EndFrame(uint64_t nowMs, uint64_t qpcFrequency, uint64_t sequence, Snapshot& out) noexcept {
        if (!active_ || nowMs < beginMs_ || nowMs - beginMs_ < WindowMs) return false;

        out = {};
        out.sequence = sequence;
        out.beginMs = beginMs_;
        out.endMs = nowMs;
        out.firstFrame = firstFrame_;
        out.lastFrame = lastFrame_;
        out.frames = frames_;
        out.qpcFrequency = qpcFrequency;
        out.stages = stages_;

        const auto durationMs = nowMs - beginMs_;
        for (auto& reading : out.stages) {
            reading.estimateKnown = reading.calls && reading.samples && !reading.clockFailures && qpcFrequency;
            if (reading.estimateKnown) {
                const double meanTicks = static_cast<double>(reading.ticks) / reading.samples;
                reading.estimatedMsPerSecond = meanTicks * reading.calls * 1000000.0 /
                    (static_cast<double>(qpcFrequency) * durationMs);
            }
        }

        active_ = false;
        beginMs_ = firstFrame_ = lastFrame_ = frames_ = 0;
        stages_ = {};
        return true;
    }
};

inline int64_t Qpc() noexcept {
    const auto lastError = GetLastError();
    LARGE_INTEGER value{};
    const bool okay = QueryPerformanceCounter(&value) != FALSE;
    SetLastError(lastError);
    return okay ? value.QuadPart : 0;
}

inline uint64_t QpcFrequency() noexcept {
    static const uint64_t frequency = []() noexcept {
        const auto lastError = GetLastError();
        LARGE_INTEGER value{};
        const bool okay = QueryPerformanceFrequency(&value) != FALSE;
        SetLastError(lastError);
        return okay && value.QuadPart > 0 ? static_cast<uint64_t>(value.QuadPart) : 0;
    }();
    return frequency;
}

class Frame;

class Recorder final {
    friend class Frame;
    WindowAccumulator window_;
    uint64_t nextFrame_ = 0;
    uint64_t publishedSequence_ = 0;
    uint64_t consumedSequence_ = 0;
    Snapshot pending_{};

    void FinishFrame(uint64_t nowMs) noexcept {
        Snapshot completed;
        if (window_.EndFrame(nowMs, QpcFrequency(), publishedSequence_ + 1, completed)) {
            pending_ = completed;
            publishedSequence_ = completed.sequence;
        }
    }

    uint64_t NextFrame() noexcept { return ++nextFrame_; }

public:
    Frame BeginFrame() noexcept;

    bool TakePublished(Snapshot& out) noexcept {
        if (consumedSequence_ == publishedSequence_) return false;
        out = pending_;
        consumedSequence_ = publishedSequence_;
        return true;
    }
};

class Frame final {
    friend class Recorder;
    Recorder* recorder_ = nullptr;
    uint64_t id_ = 0;
    bool sampled_ = false;

    Frame(Recorder& recorder, uint64_t id) noexcept
        : recorder_(&recorder), id_(id), sampled_(recorder.window_.BeginFrame(id, GetTickCount64())) {}

public:
    class Scope final {
        Frame* frame_ = nullptr;
        Stage stage_ = Stage::Count;
        int64_t begin_ = 0;

    public:
        Scope(Frame* frame, Stage stage) noexcept : frame_(frame), stage_(stage) {
            if (!frame_) return;
            frame_->recorder_->window_.RecordCall(stage_);
            if (frame_->sampled_) begin_ = Qpc();
        }

        ~Scope() noexcept {
            if (!frame_ || !frame_->sampled_) return;
            const auto end = Qpc();
            if (begin_ > 0 && end >= begin_) {
                frame_->recorder_->window_.RecordDuration(stage_, frame_->id_, static_cast<uint64_t>(end - begin_));
            } else {
                frame_->recorder_->window_.RecordClockFailure(stage_);
            }
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

    ~Frame() noexcept {
        if (recorder_) recorder_->FinishFrame(GetTickCount64());
    }

    Scope Measure(Stage stage) noexcept { return Scope(this, stage); }
    uint64_t Id() const noexcept { return id_; }
    bool Sampled() const noexcept { return sampled_; }

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    Frame(Frame&&) = delete;
    Frame& operator=(Frame&&) = delete;
};

inline Frame Recorder::BeginFrame() noexcept { return Frame(*this, NextFrame()); }
inline Frame::Scope Measure(Frame* frame, Stage stage) noexcept { return Frame::Scope(frame, stage); }

} // namespace NativeProducerCpuTiming
