// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinFsrHintPolicy.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace CriticalUi
{
// Typed allow-list: debug logs and arbitrary provider text cannot enter this queue.
enum class Key : unsigned char { GenshinFsr, Count };
class Queue
{
    std::array<Notice, static_cast<std::size_t>(Key::Count)> entries_ {};
    std::uint64_t revision_ = 0;
public:
    void Set(Key key, Notice notice) noexcept
    {
        const auto slot = static_cast<std::size_t>(key);
        if (slot >= entries_.size() || entries_[slot] == notice) return;
        entries_[slot] = notice; ++revision_;
    }
    Notice Get(Key key) const noexcept
    {
        const auto slot = static_cast<std::size_t>(key);
        return slot < entries_.size() ? entries_[slot] : Notice::None;
    }
    std::size_t Size() const noexcept
    {
        std::size_t size = 0;
        for (auto value : entries_) if (value != Notice::None) ++size;
        return size;
    }
    std::uint64_t Revision() const noexcept { return revision_; }
};
}
