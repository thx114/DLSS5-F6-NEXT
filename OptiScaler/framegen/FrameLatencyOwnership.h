#pragma once
#include <cstdint>
#include <utility>

namespace framegen::latency
{
enum class Caller : uint8_t { Unknown, Application, Sdk };
enum class Target : uint8_t { None, Native, Provider, Preserve };
struct Flags
{
    uint32_t requested = 0, effectiveNative = 0, effectiveProvider = 0;
    bool requestedKnown = false, nativeKnown = false, providerKnown = false;
};
inline constexpr uint32_t WaitableFlag = 0x40; // DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT

constexpr Target Select(const Flags& flags, Caller caller, bool sdkLoaded,
                        bool hasNative, bool hasProvider, bool nativeOnly) noexcept
{
    // The SDK and unknown callers retain their existing behavior. FG Off is
    // deliberately absent: a loaded plugin may own its wait object when Off.
    if (caller != Caller::Application || !sdkLoaded) return Target::Preserve;
    if (!flags.requestedKnown) return Target::Preserve;
    if (!(flags.requested & WaitableFlag)) return Target::None;
    // An explicit app request keeps a real object, preferably its native one.
    if (hasNative && flags.nativeKnown && (flags.effectiveNative & WaitableFlag)) return Target::Native;
    if (!nativeOnly && hasProvider && flags.providerKnown && (flags.effectiveProvider & WaitableFlag))
        return Target::Provider;
    return Target::Preserve;
}

constexpr Target Resolve(Target selected, Target preserved) noexcept
{
    return selected == Target::Preserve ? preserved : selected;
}

// All three latency APIs must use the same receiver. In particular, an app
// waiting on the native object must never overwrite the provider's queue depth.
// Resolve ownership before forwarding and release bookkeeping locks before DXGI.
template<class Chain, class Result, class Call>
Result Forward(Target target, Chain* native, Chain* provider, Call&& call,
               Result denied, Result missing)
{
    if (target == Target::None) return denied;
    auto* chain = target == Target::Native ? native : target == Target::Provider ? provider : nullptr;
    return chain ? std::forward<Call>(call)(*chain) : missing;
}
}
