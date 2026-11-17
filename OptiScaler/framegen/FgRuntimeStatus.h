#pragma once
#include "FrameOutputRate.h"
#include <atomic>
#include <chrono>
#include <mutex>

namespace framegen
{
enum class RuntimeStatus { External, Off, NoRoute, Initializing, Paused, Waiting, EnabledUnconfirmed, InputGap, OutputObserved, SdkDisabled, SdkError, Stale };
struct RuntimeStatusInputs
{
    bool external = false, requested = false, route = false, provider = false, paused = false, active = false;
};
constexpr RuntimeStatus SelectRuntimeStatus(const RuntimeStatusInputs& in) noexcept
{
    if (in.external) return RuntimeStatus::External;
    if (!in.requested) return RuntimeStatus::Off;
    if (!in.route) return RuntimeStatus::NoRoute;
    if (!in.provider) return RuntimeStatus::Initializing;
    if (in.paused) return RuntimeStatus::Paused;
    if (!in.active) return RuntimeStatus::Waiting;
    return RuntimeStatus::EnabledUnconfirmed;
}

enum class OutputRateSource { None, DlssgSdk, XefgSdk };
struct RuntimeSnapshot
{
    uintptr_t provider = 0;
    uint64_t instance = 0, epoch = 0, tickNs = 0;
    uint64_t sourceFrame = 0;
    RuntimeStatus status = RuntimeStatus::Initializing;
    OutputRateSource rateSource = OutputRateSource::None;
    OutputRate outputRate {};
    uint32_t sdkStatus = 0;
    int32_t presentResult = 0;
};
inline uint64_t RuntimeNowNs() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline uint64_t NextRuntimeInstance() noexcept
{
    static std::atomic<uint64_t> next {0};
    return ++next;
}
// Scalar observations only. Reading never takes a provider/lifecycle lock or
// calls the SDK. No resource/queue completion is implied by this cache.
class RuntimeObservations
{
    std::mutex mutex_;
    RuntimeSnapshot sample_ {};
public:
    void Publish(const RuntimeSnapshot& sample)
    {
        std::lock_guard lock(mutex_);
        sample_ = sample;
    }
    void Invalidate(uintptr_t provider, uint64_t instance)
    {
        std::lock_guard lock(mutex_);
        if (sample_.provider == provider && (!instance || sample_.instance == instance)) sample_ = {};
    }
    bool Read(uintptr_t provider, uint64_t nowNs, RuntimeSnapshot& sample)
    {
        std::lock_guard lock(mutex_);
        sample = sample_;
        if (!provider || sample.provider != provider || !sample.instance) { sample = {}; return false; }
        if (!sample.tickNs || nowNs < sample.tickNs || nowNs - sample.tickNs > 3000000000ull)
        {
            sample.status = RuntimeStatus::Stale;
            sample.outputRate = {};
        }
        if (sample.outputRate.known && (nowNs < sample.outputRate.lastNs || nowNs - sample.outputRate.lastNs > 3000000000ull))
            sample.outputRate = {};
        if (sample.status == RuntimeStatus::OutputObserved && !sample.outputRate.known)
            sample.status = RuntimeStatus::EnabledUnconfirmed;
        return true;
    }
};
inline RuntimeObservations& RuntimeState() { static RuntimeObservations value; return value; }
}
