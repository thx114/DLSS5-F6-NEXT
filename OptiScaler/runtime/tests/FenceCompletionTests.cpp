// SPDX-License-Identifier: GPL-3.0-or-later
#include "../FenceCompletion.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>

int main()
{
    using enum FramePipeline::FenceCompletion;
    struct Case { std::uint64_t completed, target; FramePipeline::FenceCompletion expected; };
    constexpr auto max = (std::numeric_limits<std::uint64_t>::max)();
    constexpr std::array cases {
        Case{0,0,Complete}, Case{0,1,Pending}, Case{11,12,Pending},
        Case{12,12,Complete}, Case{13,12,Complete}, Case{max-1,max-1,Complete},
        Case{max-2,max-1,Pending}, Case{max,0,Invalid}, Case{max,12,Invalid},
        Case{0,max,Invalid}, Case{max-1,max,Invalid}, Case{max,max,Invalid}
    };
    for (const auto& test : cases)
    {
        const auto state = FramePipeline::ObserveFence(test.completed, test.target);
        assert(state == test.expected);
        // Both allocation reuse and retirement use exactly this decision;
        // neither a signalled event nor an unsigned sentinel bypasses it.
        unsigned allocatorResets = 0, releases = 0;
        if (state == Complete) { ++allocatorResets; ++releases; }
        assert((allocatorResets == 1) == (test.expected == Complete));
        assert(releases == allocatorResets);
    }
    std::printf("FenceCompletion: %zu boundary cases passed\n", cases.size());
}
