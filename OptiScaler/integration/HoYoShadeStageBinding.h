#pragma once
#include <cstdint>

namespace HoYoShade::StageBinding
{
// Private effect-runtime allocation identity only. This key never authenticates
// color/depth contents, recording ownership, producer readiness or completion.
// Those capabilities must still be checked for every actual stage invocation.
enum class Source : unsigned char { None, GenshinBeforeSr, GenshinBeforeNr };
struct Key
{
    Source source = Source::None;
    uint64_t stream = 0, generation = 0;
    uint64_t device = 0, context = 0, deviceEpoch = 0, producerEpoch = 0;
    uint64_t configEpoch = 0, instance = 0, historyEpoch = 0;
    uint32_t thread = 0;
    bool operator==(const Key&) const = default;
    bool Valid() const noexcept
    {
        if (!stream) return false;
        if (source == Source::GenshinBeforeNr) return generation != 0;
        return source == Source::GenshinBeforeSr && device && context &&
            deviceEpoch && producerEpoch && configEpoch && instance && historyEpoch && thread;
    }
};

// The authenticated Bridge increments Owner::generation on EVERY Render entry.
// It is an invocation identity, not the lifetime of the private effect runtime.
// Keep every actual lifecycle epoch; omit only generation and sourceFrame.
template<class Owner>
inline Key BeforeSr(uint64_t stream, const Owner& owner) noexcept
{
    Key key;
    key.source = Source::GenshinBeforeSr;
    key.stream = stream;
    key.device = owner.device; key.context = owner.context;
    key.deviceEpoch = owner.deviceEpoch; key.producerEpoch = owner.producerEpoch;
    key.configEpoch = owner.configEpoch; key.instance = owner.instance;
    key.historyEpoch = owner.historyEpoch; key.thread = owner.thread;
    return key;
}

// Present supplies an adapter generation rather than a Render-token serial.
// Preserve its existing stream/generation replacement condition exactly.
inline Key BeforeNr(uint64_t stream, uint64_t generation) noexcept
{
    Key key;
    key.source = Source::GenshinBeforeNr;
    key.stream = stream; key.generation = generation;
    return key;
}
}
