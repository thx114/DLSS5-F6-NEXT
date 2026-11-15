// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace ReflexMarkerTiming
{
enum class Api : uint8_t { D3D, Vulkan };
enum class Source : uint8_t { Unknown, Application, NativeBoundary, FirstSubmit, PresentFallback, Synthetic };
enum class Point : uint8_t { SimStart, SimEnd, SubmitStart, SubmitEnd, PresentStart, PresentEnd, Count };

struct Device
{
    Api api = Api::D3D;
    uintptr_t address = 0;
    uint64_t generation = 0;
    bool operator==(const Device&) const = default;
};

struct Marks
{
    std::array<Source, static_cast<size_t>(Point::Count)> points {};

    Source Span(Point first, Point last) const noexcept
    {
        const auto a = points[static_cast<size_t>(first)], b = points[static_cast<size_t>(last)];
        for (auto detail : { Source::PresentFallback, Source::FirstSubmit, Source::Synthetic })
            if (a == detail || b == detail) return detail;
        return a == b ? a : Source::Unknown;
    }
};

inline constexpr const char* Name(Source source) noexcept
{
    switch (source)
    {
    case Source::Application: return "application";
    case Source::NativeBoundary: return "native-boundary";
    case Source::FirstSubmit: return "first-submit";
    case Source::PresentFallback: return "present-fallback";
    case Source::Synthetic: return "synthetic";
    default: return "unknown";
    }
}

// Callers synchronize the ledger; never hold that lock across provider calls.
// Bounded storage covers the driver's 64 report slots. Missing/evicted data
// means unknown; a delayed response never borrows the currently running frame.
class Ledger
{
    struct Sample
    {
        uint64_t frame = 0;
        Marks marks;
        uint8_t observed = 0;
        bool ambiguous = false;
    };
    struct Owner
    {
        Device device;
        std::array<Sample, 128> frames {};
    };
    std::array<Owner, 8> owners_ {};
    size_t nextOwner_ = 0;
    uint64_t nextGeneration_ = 0;

    Owner* Find(Api api, uintptr_t address) noexcept
    {
        for (auto& owner : owners_)
            if (owner.device.api == api && owner.device.address == address) return &owner;
        return nullptr;
    }

public:
    Device Get(Api api, uintptr_t address) noexcept
    {
        if (auto* owner = address ? Find(api, address) : nullptr) return owner->device;
        return { api, address, 0 };
    }

    Device Begin(Api api, uintptr_t address) noexcept
    {
        if (!address) return { api, 0, 0 };
        if (auto* owner = Find(api, address)) return owner->device;
        auto& owner = owners_[nextOwner_];
        nextOwner_ = (nextOwner_ + 1) % owners_.size();
        owner = {};
        return owner.device = { api, address, ++nextGeneration_ };
    }

    void Forget(Api api, uintptr_t address) noexcept
    {
        if (auto* owner = address ? Find(api, address) : nullptr) *owner = {};
    }

    void Accept(Device device, uint64_t frame, Point point, Source source) noexcept
    {
        const auto idx = static_cast<size_t>(point);
        if (!device.generation || !frame || idx >= static_cast<size_t>(Point::Count)) return;
        auto* owner = Find(device.api, device.address);
        if (!owner || owner->device != device) return;
        auto& sample = owner->frames[frame % owner->frames.size()];
        if (sample.frame != frame) sample = { frame, {}, 0 };
        const auto mask = static_cast<uint8_t>(1u << idx);
        // A duplicated simulation start makes the entire sample ambiguous. In
        // other cases the report cannot identify which duplicate supplied the endpoint.
        if (point == Point::SimStart && (sample.observed & mask)) sample.ambiguous = true;
        if (sample.observed & mask) sample.marks.points[idx] = Source::Unknown;
        else sample.marks.points[idx] = source;
        sample.observed |= mask;
    }

    Marks Read(Device device, uint64_t frame) noexcept
    {
        if (!device.generation || !frame) return {};
        const auto* owner = Find(device.api, device.address);
        if (!owner || owner->device != device) return {};
        const auto& sample = owner->frames[frame % owner->frames.size()];
        return sample.frame == frame && !sample.ambiguous ? sample.marks : Marks {};
    }
};
} // namespace ReflexMarkerTiming
