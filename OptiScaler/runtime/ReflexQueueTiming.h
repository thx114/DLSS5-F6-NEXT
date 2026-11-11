// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>

namespace ReflexQueueTiming
{
// Endpoints from ONE actual report, in microseconds. This separates a nested
// queue interval; it is neither click-to-photon latency nor active GPU time.
struct Parts { double beforeGpuMs, gpuSpanMs, afterGpuMs; };
inline std::optional<Parts> Split(uint64_t frame, bool estimated, uint64_t queueBegin,
    uint64_t queueEnd, uint64_t gpuBegin, uint64_t gpuEnd) noexcept
{
    if (!frame || estimated || !queueBegin || !queueEnd || !gpuBegin || !gpuEnd ||
        queueBegin > gpuBegin || gpuBegin > gpuEnd || gpuEnd > queueEnd) return {};
    return Parts {double(gpuBegin-queueBegin)/1000.0, double(gpuEnd-gpuBegin)/1000.0,
                  double(queueEnd-gpuEnd)/1000.0};
}
}
