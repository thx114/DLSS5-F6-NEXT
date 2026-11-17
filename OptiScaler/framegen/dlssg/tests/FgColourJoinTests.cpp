// Standalone C++20 CPU policy tests; no device, SDK, compiler or runner is
// invoked by adding this file. Lead owns any later compilation/execution.
#include "../FgColourJoin.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace
{
using namespace dlssg::colour;
void Check(bool result, int line)
{
    if (!result) { std::fprintf(stderr,"colour join failed at line %d\n",line); std::abort(); }
}
#define CHECK(value) Check(static_cast<bool>(value),__LINE__)
}

int main()
{
    using namespace dlssg::colour;
    source_colour::Receipt nr {source_colour::Result::NrWritten, 1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(nr.Matches(1,6,2));
    CHECK(!nr.Matches(2,6,2) && !nr.Matches(1,7,2) && !nr.Matches(1,6,3));
    CHECK(CombinedMutation(nr,Mutation::Original) == Mutation::NrWritten);
    CHECK(CombinedMutation(nr,Mutation::NrWritten) == Mutation::NrWritten);
    CHECK(CombinedMutation(nr,Mutation::Uncertain) == Mutation::Uncertain);
    nr.result = source_colour::Result::Unknown;
    CHECK(!nr.Matches(1,6,2) && CombinedMutation(nr,Mutation::Original) == Mutation::Uncertain);
    nr.result = source_colour::Result::Modified;
    CHECK(nr.Matches(1,6,2));
    CHECK(CombinedMutation(nr,Mutation::Original) == Mutation::Uncertain);
    source_colour::Recording recording;
    CHECK(recording.Outcome() == source_colour::Result::Original);
    recording.Write();
    CHECK(recording.Outcome() == source_colour::Result::Modified && !recording.resolved);
    recording.Resolve();
    CHECK(recording.Outcome() == source_colour::Result::NrWritten);
    nr.result = source_colour::Result::Original;
    CHECK(CombinedMutation(nr,Mutation::Original) == Mutation::Original);
    nr.fenceValue = UINT64_MAX;
    CHECK(!nr.Matches(1,6,2));
    CHECK(CombinedMutation(std::nullopt,Mutation::Original) == Mutation::Original);
    const Source source{1,10,2,3,4,5,0x100,0x200,0x300};
    CHECK(CanUseHudless(source,source,Mutation::Original));
    CHECK(!CanUseHudless(source,source,Mutation::NrWritten));
    CHECK(!CanUseHudless(source,source,Mutation::Uncertain));
    CHECK(!CanUseHudless(source,{},Mutation::Original)); // Missing UI is an optional refusal.
    for (auto member : {&Source::serial,&Source::frame,&Source::generation,&Source::epoch,
                         &Source::feature,&Source::configuration})
    {
        auto stale=source; ++(stale.*member);
        CHECK(!CanUseHudless(source,stale,Mutation::Original));
    }
    for (auto member : {&Source::provider,&Source::swapchain,&Source::output})
    {
        auto other=source; ++(other.*member);
        CHECK(!CanUseHudless(source,other,Mutation::Original));
    }
    auto missing=source;missing.serial=0;
    CHECK(!CanUseHudless(missing,missing,Mutation::Original));
    missing=source;missing.output=0;
    CHECK(!CanUseHudless(missing,missing,Mutation::Original));
    const Endpoint actual{0x100,0x200,0x300,0x400,17};
    CHECK(SameEndpoint(actual,actual));
    for (auto member : {&Endpoint::swapchain,&Endpoint::colour,&Endpoint::queue,&Endpoint::device})
    {
        auto other=actual; ++(other.*member);
        CHECK(!SameEndpoint(actual,other));
        other=actual;other.*member=0;
        CHECK(!SameEndpoint(other,other));
    }
    auto earlier=actual;--earlier.present;
    CHECK(!SameEndpoint(actual,earlier));
    CHECK(!SameEndpoint({},{}));
    std::puts("FG colour join CPU identity policy passed; no GPU/pixel proof");
}
