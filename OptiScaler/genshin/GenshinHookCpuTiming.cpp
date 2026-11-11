// SPDX-License-Identifier: GPL-3.0-or-later
#include "GenshinHookCpuTiming.h"

namespace GenshinHookCpuTiming {
// These user-provided definitions are deliberately outside their first
// declarations and non-constexpr. Each constructor initializes only one level:
// 25 atomic fields per Cell, 402 Cells per ThreadSlot, then 32 ThreadSlots.
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
