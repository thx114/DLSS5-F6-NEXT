#pragma once

#include <Windows.h>
#include <Config.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

// Monitor metadata only. GPU completion and resource ownership stay with the
// original transport/timer. A marker call brackets the driver's CPU timestamp;
// calibrated GPU timestamps are mapped through that same frame and device.
namespace GpuTimeline
{
enum class Stream : unsigned { Bridge, Nr };
inline bool Enabled() noexcept
{
    const auto* config = Config::Instance();
    return config->ShowFps.value_or_default() &&
        config->FpsOverlayType.value_or_default() >= FpsOverlay_ReflexTimings;
}
inline double QpcUs() noexcept
{
    static const double frequency = [] { LARGE_INTEGER hz{}; QueryPerformanceFrequency(&hz); return double(hz.QuadPart); }();
    LARGE_INTEGER value{};
    return frequency > 0.0 && QueryPerformanceCounter(&value) ? double(value.QuadPart) * 1e6 / frequency : 0.0;
}
inline double QpcTicksToUs(uint64_t ticks) noexcept
{
    static const double frequency = [] { LARGE_INTEGER hz{}; QueryPerformanceFrequency(&hz); return double(hz.QuadPart); }();
    return frequency > 0.0 ? double(ticks) * 1e6 / frequency : 0.0;
}
inline double GpuTicksToQpcUs(uint64_t ticks, uint64_t gpu, uint64_t cpu, uint64_t frequency) noexcept
{
    if (!frequency || !cpu) return 0.0;
    // Subtract before conversion; GPU and QPC may have unrelated epochs.
    const double delta = ticks >= gpu ? double(ticks - gpu) : -double(gpu - ticks);
    return QpcTicksToUs(cpu) + delta * 1e6 / double(frequency);
}
struct Reading
{
    uint64_t frame = 0, stamp = 0, device = 0;
    double beginUs = 0.0, endUs = 0.0;
};
struct Slot
{
    std::atomic_flag writer = ATOMIC_FLAG_INIT;
    std::atomic<uint64_t> version{0}, frame{0}, stamp{0}, device{0};
    std::atomic<double> beginUs{0.0}, endUs{0.0};
    void Store(const Reading& r) noexcept
    {
        if (writer.test_and_set(std::memory_order_acquire)) return;
        version.fetch_add(1, std::memory_order_acq_rel);
        frame.store(r.frame, std::memory_order_relaxed); stamp.store(r.stamp, std::memory_order_relaxed);
        device.store(r.device, std::memory_order_relaxed);
        beginUs.store(r.beginUs, std::memory_order_relaxed); endUs.store(r.endUs, std::memory_order_relaxed);
        version.fetch_add(1, std::memory_order_release);
        writer.clear(std::memory_order_release);
    }
    Reading Read(uint64_t id) const noexcept
    {
        const auto before = version.load(std::memory_order_acquire);
        if (!before || (before & 1)) return {};
        const Reading r{frame.load(std::memory_order_relaxed), stamp.load(std::memory_order_relaxed),
            device.load(std::memory_order_relaxed), beginUs.load(std::memory_order_relaxed), endUs.load(std::memory_order_relaxed)};
        std::atomic_thread_fence(std::memory_order_acquire);
        const auto now = GetTickCount64();
        if (before != version.load(std::memory_order_acquire) || r.frame != id || !id || !r.stamp ||
            now < r.stamp || now - r.stamp > 2000 || !std::isfinite(r.beginUs) ||
            !std::isfinite(r.endUs) || r.beginUs <= 0.0 || r.endUs < r.beginUs) return {};
        return r;
    }
};
inline constexpr size_t Capacity = 256;
inline std::array<std::array<Slot, Capacity>, 2> spans;
inline std::array<Slot, Capacity> markers;
inline void Publish(Stream stream, uint64_t frame, double beginUs, double endUs, uint64_t markerDevice) noexcept
{
    if (Enabled() && frame && markerDevice && std::isfinite(beginUs) && std::isfinite(endUs) &&
        beginUs > 0.0 && endUs >= beginUs && endUs - beginUs <= 2e6)
        spans[unsigned(stream)][frame % Capacity].Store({frame, GetTickCount64(), markerDevice, beginUs, endUs});
}
// Capture the successful marker's identity when recording this logical frame.
// A missing original-driver marker keeps the independent duration path only.
inline Reading Capture(uint64_t frame) noexcept
{
    return Enabled() ? markers[frame % Capacity].Read(frame) : Reading{};
}
inline Reading Read(Stream stream, uint64_t frame) noexcept { return spans[unsigned(stream)][frame % Capacity].Read(frame); }
inline void Marker(uint64_t frame, IUnknown* device, double beginUs, double endUs) noexcept
{
    if (Enabled() && frame && device && beginUs > 0.0 && endUs >= beginUs && endUs - beginUs <= 2000.0)
        markers[frame % Capacity].Store({frame, GetTickCount64(), reinterpret_cast<uintptr_t>(device), beginUs, endUs});
}
// Only RENDERSUBMIT_START markers are recorded. Keep the uncertainty of the
// synchronous call instead of pretending a CPU submission is a GPU start.
inline Reading Anchor(uint64_t frame, IUnknown* device) noexcept
{
    auto r = markers[frame % Capacity].Read(frame);
    return r.device == reinterpret_cast<uintptr_t>(device) ? r : Reading{};
}
} // namespace GpuTimeline
