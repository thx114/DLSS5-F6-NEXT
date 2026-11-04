#pragma once
#include <cstdint>
#include <limits>

namespace Dx11BridgeInit
{
enum class Phase { NotStarted, Recording, FailedUnsubmitted, SubmittedUntracked, SubmittedFenced, DeviceRemoved, Complete };
class State
{
    Phase phase_ = Phase::NotStarted;
    std::uint64_t fenceValue_ = 0;
public:
    Phase Get() const { return phase_; }
    bool CanStart() const { return phase_ == Phase::NotStarted; }
    bool Begin() { if (!CanStart()) return false; phase_ = Phase::Recording; return true; }
    void UnsubmittedFailure() { if (phase_ == Phase::Recording) phase_ = Phase::FailedUnsubmitted; }
    void Submitted() { if (phase_ == Phase::Recording) phase_ = Phase::SubmittedUntracked; }
    void Signalled(std::uint64_t value, bool success)
    {
        if (phase_ != Phase::SubmittedUntracked || !success || value == 0) return;
        fenceValue_ = value; phase_ = Phase::SubmittedFenced;
    }
    bool Observe(std::uint64_t completed, bool removed)
    {
        if (phase_ != Phase::SubmittedFenced && phase_ != Phase::SubmittedUntracked) return Ready();
        if (removed || completed == std::numeric_limits<std::uint64_t>::max())
        { phase_ = Phase::DeviceRemoved; return false; }
        if (phase_ == Phase::SubmittedFenced && completed >= fenceValue_)
            phase_ = Phase::Complete;
        return Ready();
    }
    bool Ready() const { return phase_ == Phase::Complete; }
    bool RequiresRetention() const
    {
        return phase_ == Phase::SubmittedFenced || phase_ == Phase::SubmittedUntracked || phase_ == Phase::DeviceRemoved;
    }
};
}
