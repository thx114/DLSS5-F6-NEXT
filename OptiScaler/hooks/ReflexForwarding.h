// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <sl_pcl.h>
#include <array>
#include <cstddef>
#include <utility>

namespace reflex::forwarding
{
// Only recursion on the calling thread bypasses SL. Independent game/SDK
// threads must each forward their markers; no lock spans an SDK callback.
inline thread_local std::array<bool, static_cast<size_t>(sl::PCLMarker::eMaximum)> markers {};
inline thread_local bool sleeping = false;

class Scope final
{
    bool& flag_;
    const bool previous_;
    explicit Scope(bool& flag) noexcept : flag_(flag), previous_(std::exchange(flag, true)) {}
public:
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() { flag_ = previous_; }
    static Scope Sleep() noexcept { return Scope(sleeping); }
    static Scope Marker(sl::PCLMarker marker) noexcept { return Scope(markers[static_cast<size_t>(marker)]); }
};

inline bool Active(sl::PCLMarker marker) noexcept
{
    const auto index = static_cast<size_t>(marker);
    return index >= markers.size() || markers[index];
}
}
