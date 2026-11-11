// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <limits>

namespace NrPresentation
{
// CPU admission only. A successful source and a single claim do not certify
// queue ordering, completion, or resource retirement. The endpoint proves those.
struct Source
{
    uint64_t ordinal = 0, feature = 0, context = 0, generation = 0, output = 0;
    bool Valid() const noexcept
    {
        return ordinal && ordinal != UINT64_MAX && feature && context && generation && output;
    }
    bool operator==(const Source&) const = default;
};

// Output allocations may rotate. Temporal continuity belongs to the actual
// producer domain, not to a particular member of its output buffer ring.
inline bool Follows(const Source& previous, const Source& current) noexcept
{
    return previous.Valid() && current.Valid() && current.ordinal == previous.ordinal + 1 &&
           previous.feature == current.feature && previous.context == current.context &&
           previous.generation == current.generation;
}

class SourceClock
{
    uint64_t attempt_ = 0, completed_ = 0, claimed_ = 0;
    Source source_ {};
    bool valid_ = false;
public:
    uint64_t Begin() noexcept
    {
        valid_ = false; source_ = {}; completed_ = 0;
        if (attempt_ == UINT64_MAX) return 0;
        return ++attempt_;
    }
    bool Complete(uint64_t attempt, Source source, bool succeeded) noexcept
    {
        if (!attempt || attempt != attempt_) return false; // a nested/newer call owns admission
        valid_ = succeeded && source.Valid();
        source_ = valid_ ? source : Source {};
        completed_ = attempt;
        return valid_;
    }
    bool Current(uint64_t attempt, const Source& source) const noexcept
    {
        return valid_ && attempt && attempt == attempt_ && attempt == completed_ && source == source_;
    }
    bool Claim(uint64_t attempt, const Source& source) noexcept
    {
        if (!Current(attempt, source) || claimed_ == attempt) return false;
        claimed_ = attempt;
        return true;
    }
    bool Available(uint64_t attempt, const Source& source) const noexcept
    { return Current(attempt, source) && claimed_ != attempt; }
    uint64_t Attempt() const noexcept { return attempt_; }
    const Source& CurrentSource() const noexcept { return source_; }
    void Invalidate() noexcept { valid_ = false; } // never recycle ordinals or claims
};
}
