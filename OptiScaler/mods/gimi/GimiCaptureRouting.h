#pragma once

#include <atomic>
#include <array>
#include <cstdint>

namespace gimi::runtime
{
// Non-owning ingress identity. Only compare these keys; the owner mutex and
// retained Capture still authorize every registry/GPU access. Generations are
// unique, increasing IDs, including when a device/context pointer is reused.
struct CaptureRoute
{
    std::uintptr_t device = 0, context = 0;
    std::uint64_t generation = 0;
    unsigned slot = 0;
    explicit operator bool() const noexcept { return generation != 0; }
    bool operator==(const CaptureRoute&) const = default;
};

class CaptureRouting final
{
public:
    // Creation observations are provisional until the selected native Present
    // confirms a device. This is a CPU capture budget, not a GPU packet pool.
    static constexpr unsigned Capacity = 4;
private:
    struct Slot
    {
        std::atomic<std::uint64_t> generation{0}, lost{0};
        std::atomic<std::uintptr_t> device{0}, context{0};
    };
    std::array<Slot, Capacity> slots_{};
    std::atomic<unsigned> selected_{0};
public:
    // Publication/clear have one writer under the existing owner mutex. Zero
    // closes the short publication interval. Acquire pointer loads ensure that
    // observing a new key also observes that preceding zero generation; the
    // second generation load therefore cannot certify a mixed snapshot.
    void Publish(CaptureRoute route) noexcept
    {
        auto& slot = slots_[route.slot];
        slot.generation.store(0, std::memory_order_release);
        slot.device.store(route.device, std::memory_order_release);
        slot.context.store(route.context, std::memory_order_release);
        slot.generation.store(route.generation, std::memory_order_release);
    }
    void Clear() noexcept
    {
        for (unsigned index = 0; index < Capacity; ++index) slots_[index].generation.store(0, std::memory_order_release);
    }
    void Select(CaptureRoute route) noexcept
    {
        Nominate(route);
        for (unsigned index = 0; index < Capacity; ++index)
            if (index != route.slot) slots_[index].generation.store(0, std::memory_order_release);
    }
    void Nominate(CaptureRoute route) noexcept { selected_.store(route.slot, std::memory_order_release); }
    CaptureRoute Read(unsigned index) const noexcept
    {
        const auto& slot = slots_[index];
        const auto generation = slot.generation.load(std::memory_order_acquire);
        if (!generation) return {};
        CaptureRoute route{slot.device.load(std::memory_order_acquire),
                           slot.context.load(std::memory_order_acquire), generation, index};
        return slot.generation.load(std::memory_order_acquire) == generation ? route : CaptureRoute{};
    }
    CaptureRoute Read() const noexcept { return Read(selected_.load(std::memory_order_acquire)); }
    CaptureRoute Device(std::uintptr_t key) const noexcept
    {
        const auto selected = Read();
        if (selected && selected.device == key) return selected;
        for (unsigned index = 0; index < Capacity; ++index)
            if (const auto route = Read(index); route && route.device == key) return route;
        return {};
    }
    CaptureRoute Context(std::uintptr_t key) const noexcept
    {
        const auto selected = Read();
        if (selected && selected.context == key) return selected;
        for (unsigned index = 0; index < Capacity; ++index)
            if (const auto route = Read(index); route && route.context == key) return route;
        return {};
    }
    bool Current(CaptureRoute route) const noexcept { return route && Read(route.slot) == route; }
    // A delayed failure from an old callback cannot poison a replacement or
    // overwrite a newer failure. No retry loop/owner lock on ordinary reads.
    void Lose(std::uint64_t generation, unsigned index) noexcept
    {
        auto& slot = slots_[index];
        auto lost = slot.lost.load(std::memory_order_relaxed);
        while (lost < generation && !slot.lost.compare_exchange_weak(
            lost, generation, std::memory_order_release, std::memory_order_relaxed)) {}
    }
    void Lose(std::uint64_t generation) noexcept { Lose(generation, selected_.load(std::memory_order_acquire)); }
    void Lose(CaptureRoute route) noexcept { Lose(route.generation, route.slot); }
    void Lose() noexcept { Lose(Read()); }
    void LoseAll() noexcept
    { for (unsigned index = 0; index < Capacity; ++index) Lose(Read(index)); }
    // Callers already holding a Capture use its immutable generation. In the
    // raster/constant-reader path this is one atomic load, with no COM query.
    bool Lost(std::uint64_t generation, unsigned index) const noexcept
    { return generation && slots_[index].lost.load(std::memory_order_acquire) >= generation; }
    bool Lost(CaptureRoute route) const noexcept { return Lost(route.generation, route.slot); }
    bool Lost() const noexcept
    {
        const auto route = Read();
        return Lost(route);
    }
};
} // namespace gimi::runtime
