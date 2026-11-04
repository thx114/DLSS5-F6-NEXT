#pragma once
#include <cstdint>

namespace HoYoShade::AssociationDiagnostics
{
// Cached scalar observations only. None of these types or predicates authorize
// execution, retry, aliasing, suppression, resource access or source-frame claims.
enum class Event : unsigned { Init, Present, Begin, Finish, Technique, Reload, Destroy, Count };
inline const char* Text(Event event) noexcept
{
    constexpr const char* names[]{"init", "present", "begin", "finish", "technique", "reload", "destroy"};
    return static_cast<unsigned>(event) < static_cast<unsigned>(Event::Count) ? names[static_cast<unsigned>(event)] : "unknown";
}
inline void Increment(uint64_t& value) noexcept { if (value != UINT64_MAX) ++value; }
struct Callbacks
{
    uint64_t counts[static_cast<unsigned>(Event::Count)]{};
    void Observe(Event event, bool owned) noexcept
    { if (!owned && static_cast<unsigned>(event) < static_cast<unsigned>(Event::Count)) Increment(counts[static_cast<unsigned>(event)]); }
    uint64_t Count(Event event) const noexcept { return counts[static_cast<unsigned>(event)]; }
};
struct Candidate
{
    uint64_t runtime = 0, lifetime = 0, device = 0, queue = 0, backbuffer0 = 0;
    unsigned api = 0;
    bool operator==(const Candidate&) const = default;
};
enum class Match : unsigned { NotReached, NoLiveRuntimeObserved, DeviceUnspecified, BackbufferUnspecified, DeviceUnmapped,
    BackbufferUnmapped, TicketRuntimeUnmapped, TicketLifetimeUnmapped, TicketSerialUnmapped,
    Ambiguous, Unique };
inline const char* Text(Match match) noexcept
{
    constexpr const char* names[]{"not-reached", "no-live-runtime-observed", "natural-device-unspecified", "natural-backbuffer-unspecified",
        "natural-device-unmapped", "natural-backbuffer-unmapped", "physical-source-runtime-unmapped",
        "physical-source-lifetime-unmapped", "physical-source-serial-unmapped", "ambiguous-exact-candidates", "unique-exact-candidate"};
    return static_cast<unsigned>(match) < sizeof(names) / sizeof(*names) ? names[static_cast<unsigned>(match)] : "unknown";
}
struct Snapshot
{
    Match match = Match::NotReached;
    unsigned live = 0, tombstones = 0, devices = 0, backbuffers = 0, exact = 0;
    unsigned ticketRuntime = 0, ticketLifetime = 0, independentBuffers = 0, apis = 0;
    Candidate firstLive, firstDevice;
    bool operator==(const Snapshot&) const = default;
};
// Observe the entire bounded table, including candidates after the execution
// loop's ambiguous early return. Never dereference cached SDK/native handles.
template<class Entries, class PointerKey>
Snapshot Inspect(const Entries& entries, PointerKey key, uint64_t device, uint64_t backbuffer,
                 bool physical, uint64_t runtime, uint64_t lifetime, uint64_t serial, unsigned api = 0) noexcept
{
    Snapshot result;
    for (const auto& entry : entries) {
        if (!entry.runtime) continue;
        if (!entry.live) { ++result.tombstones; continue; }
        ++result.live;
        Candidate candidate{key(entry.runtime), entry.lifetime, entry.id.nativeDevice,
            entry.id.nativeQueue, entry.backbuffers[0], entry.id.api};
        if (!result.firstLive.runtime) result.firstLive = candidate;
        const bool apiMatch = !api || entry.id.api == api;
        if (apiMatch) ++result.apis;
        if (apiMatch && entry.id.nativeDevice == device) {
            ++result.devices;
            if (!result.firstDevice.runtime) result.firstDevice = candidate;
        }
        // An absent color cannot prove a resource association. Unlike the
        // existing execution selector's optional buffer, diagnostics stay unknown.
        bool bufferMatch = false;
        if (backbuffer) for (auto value : entry.backbuffers) if (value == backbuffer) bufferMatch = true;
        if (bufferMatch) ++result.independentBuffers;
        if (apiMatch && bufferMatch && entry.id.nativeDevice == device) ++result.backbuffers;
        if (physical) {
            if (key(entry.runtime) != runtime) continue;
            ++result.ticketRuntime;
            if (entry.lifetime != lifetime) continue;
            ++result.ticketLifetime;
            if (entry.sourceSerial == serial) ++result.exact;
        } else if (apiMatch && entry.id.nativeDevice == device && bufferMatch) ++result.exact;
    }
    if (!result.live) result.match = Match::NoLiveRuntimeObserved;
    else if (physical && !result.ticketRuntime) result.match = Match::TicketRuntimeUnmapped;
    else if (physical && !result.ticketLifetime) result.match = Match::TicketLifetimeUnmapped;
    else if (physical && !result.exact) result.match = Match::TicketSerialUnmapped;
    else if (!physical && !device) result.match = Match::DeviceUnspecified;
    else if (!physical && !backbuffer) result.match = Match::BackbufferUnspecified;
    else if (!physical && !result.devices) result.match = Match::DeviceUnmapped;
    else if (!physical && !result.exact) result.match = Match::BackbufferUnmapped;
    else result.match = result.exact == 1 ? Match::Unique : Match::Ambiguous;
    return result;
}
struct CallbackSnapshot
{
    Candidate candidate;
    uint64_t enrolledLifetime = 0;
    bool identityAccepted = false, enrolled = false, live = false;
    bool operator==(const CallbackSnapshot&) const = default;
};
}
