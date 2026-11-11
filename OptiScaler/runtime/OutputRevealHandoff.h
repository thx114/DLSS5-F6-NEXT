// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace FramePipeline
{
// One observation under the caller's try-acquired display gate. No waiting,
// retry loop, raw hold, lock reacquisition or message dispatch. Pending results
// leave the real output/tails with their existing owners; the caller continues
// its current source. Only the HWND owner can later reveal.
class OutputRevealHandoff final
{
public:
    enum class Step { TailPending, OwnerPending, Revealed, Abandon };
    enum class Result { NotStarted, TailPending, OwnerPending, Revealed, Expired, Cancelled };
    enum class Phase { Idle, Observing };
private:
    Phase phase_=Phase::Idle;
    bool servicing_=false;
public:
    Phase State() const noexcept {return phase_;}
    void Release() noexcept {phase_=Phase::Idle;}
    template<class Clock,class Poll>
    Result Service(uint64_t outputUntil,uint64_t inputUntil,Clock now,Poll poll) noexcept {
        if(servicing_)return Result::NotStarted;
        const auto until=outputUntil<inputUntil ? outputUntil:inputUntil;
        if(now()>=until)return Result::Expired;
        phase_=Phase::Observing;servicing_=true;
        struct Finish {
            OutputRevealHandoff& self;
            ~Finish(){self.Release();self.servicing_=false;}
        } finish{*this};
        // Check immediately BEFORE the sole poll, including a deadline crossed
        // during admission. There is deliberately no post-deadline retry poll.
        if(now()>=until)return Result::Expired;
        const auto step=poll();
        if(phase_==Phase::Idle || step==Step::Abandon)return Result::Cancelled;
        if(step==Step::Revealed)return Result::Revealed;
        return step==Step::TailPending ? Result::TailPending:Result::OwnerPending;
    }
};
}
