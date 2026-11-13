// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>

namespace dlssg::detail
{
// Process-wide terminal veto: failed real queue retirement must not admit a
// replacement provider over allocations still retained by the old viewport.
inline std::atomic<bool> g_dlssgTeardownQuarantined{false};
}
