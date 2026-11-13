// SPDX-License-Identifier: GPL-3.0-or-later
#include "HookCpuTiming.h"

namespace HookCpuTiming {
// These user-provided definitions are deliberately outside their first
// declarations and non-constexpr. Each constructor initializes only one level:
// FieldCount atomic fields per Cell, BucketCount Cells per ThreadSlot, then ThreadLimit slots.
// Member zero initializers and real atomic construction remain intact.
Cell::Cell() noexcept = default;
StarRailCell::StarRailCell() noexcept = default;
ThreadSlot::ThreadSlot() noexcept = default;

// One eager CRT startup initialization; no aggregate constinit expansion,
// function-local lazy guard, heap allocation, resizing or hook-time locking.
std::array<ThreadSlot,ThreadLimit> slots;

// TLS keeps constant initialization (slot=-1, all counters zero), also visible
// in the extern declaration so callers do not need dynamic TLS initialization.
constinit thread_local Local local;
constinit thread_local StarRailInvocation starRailInvocation;
}
