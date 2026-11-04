#pragma once

#include "../dlssnr/DlssNrSceneCut.h"

namespace native
{
// CPU policy only. GPU ownership is independent of invalidating an observation.
// Flow readbacks carry exact stamps; TrustMask exposes only a cached level, not
// the frame/epoch it read. After invalidation, drain its four slots plus three
// dispatches of readback latency AND require a low level before a new high edge.
// This deliberately refuses an unproven held-high result instead of reviving it.
class NativeSceneCutPolicy
{
  public:
    using Mode = DlssNrSceneCut::Mode;
    struct Stamp { unsigned long long tick = 0, epoch = 0; };

    void Begin(Mode mode, bool continuityReset)
    {
        ++_tick;
        SetMode(mode);
        if (continuityReset) Invalidate();
        if (continuityReset) _policy.GameReset(_tick);
    }

    void SetMode(Mode mode)
    {
        if (mode != _mode) { _mode = mode; Invalidate(); }
    }

    void Invalidate()
    {
        ++_epoch;
        _pending = false;
        _policy.Forget();
        _trustDispatches = 0;
        _trustArmed = false;
    }

    Stamp Current() const { return { _tick, _epoch }; }
    Mode GetMode() const { return _mode; }
    bool ResetRequested() const { return _pending && _mode == Mode::Act; }

    void Observe(Stamp source, bool lateKnown = true)
    {
        const bool current = source.epoch == _epoch;
        if (!current) ++stale;
        if (_mode == Mode::Off) return;
        lateBy = lateKnown && _tick >= source.tick ? _tick - source.tick : 0;
        lateByKnown = lateKnown;
        const auto outcome = _policy.Found(source.tick, _tick, current ? _mode : Mode::Count);
        if (current) _pending = _pending || outcome.reset;
    }

    void TrustDispatched(bool high, bool allowCut)
    {
        ++_trustDispatches;
        if (_trustDispatches <= 7) return;
        if (!high)
        {
            _trustArmed = true;
            _trustLow = Current();
        }
        else if (_trustArmed && allowCut)
        {
            _trustArmed = false;
            // The low observation is a conservative lower bound on cut origin.
            // No exact TrustMask readback age is claimed by the status API.
            Observe(_trustLow, false);
        }
    }

    void Accepted(bool accepted)
    {
        // Called only after a successful producer submission, with the actual
        // model Evaluate(reset=1) success receipt, never the ApplyFn boolean.
        if (accepted && ResetRequested()) { ++resets; _pending = false; }
    }

    const DlssNrSceneCut::Policy& Counts() const { return _policy; }
    unsigned long long resets = 0, stale = 0, lateBy = 0;
    bool lateByKnown = true;

  private:
    Mode _mode = Mode::Count;
    unsigned long long _tick = 0, _epoch = 1, _trustDispatches = 0;
    bool _pending = false, _trustArmed = false;
    Stamp _trustLow;
    DlssNrSceneCut::Policy _policy;
};
} // namespace native
