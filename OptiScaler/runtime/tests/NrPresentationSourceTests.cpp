// SPDX-License-Identifier: GPL-3.0-or-later
#include <runtime/NrPresentationSource.h>
#include <cstdio>
#include <cstdlib>
namespace {
unsigned checks = 0;
void Check(bool ok, const char* why) { ++checks; if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
}
int main()
{
    using namespace NrPresentation;
    SourceClock clock;
    const Source source {1, 2, 3, 4, 5};
    Check(!clock.Claim(0, source), "no fabricated initial source");
    auto a = clock.Begin();
    Check(!clock.Claim(a, source), "Evaluate entry is not success");
    Check(clock.Complete(a, source, true), "real successful source accepted");
    Check(clock.Claim(a, source), "first source Present may record");
    Check(!clock.Claim(a, source), "duplicate and generated Presents may not rerun NR");
    auto b = clock.Begin();
    Check(!clock.Current(a, source), "new attempt immediately revokes previous success");
    Check(!clock.Complete(a, source, true), "late return cannot publish old source");
    Check(!clock.Complete(b, source, false), "failed SR never publishes old colour");
    Check(!clock.Available(b, source), "failure may not reuse last NR output");
    b = clock.Begin();
    const auto nested = clock.Begin();
    Check(!clock.Complete(b, source, true), "nested newer Evaluate owns transaction");
    Source newer {2, 2, 3, 4, 5};
    Check(Follows(source, newer), "adjacent source in the same producer domain");
    auto rotating = newer; ++rotating.output;
    Check(Follows(source, rotating), "output-ring rotation preserves temporal continuity");
    for (unsigned field = 0; field < 4; ++field) {
        auto changed = newer;
        switch (field) {
        case 0: ++changed.ordinal; break; case 1: ++changed.feature; break;
        case 2: ++changed.context; break; case 3: ++changed.generation; break;
        }
        Check(!Follows(source, changed), "frame gap and producer/config changes reset history");
    }
    Check(!Follows({}, newer), "no completed source means a history reset");
    Check(clock.Complete(nested, newer, true), "newer source completes");
    for (unsigned field = 0; field < 5; ++field) {
        auto wrong = newer;
        switch (field) {
        case 0: ++wrong.ordinal; break; case 1: ++wrong.feature; break;
        case 2: ++wrong.context; break; case 3: ++wrong.generation; break;
        case 4: ++wrong.output; break;
        }
        Check(!clock.Claim(nested, wrong), "cross-source/feature/config/output cannot claim");
    }
    Check(clock.Claim(nested, newer), "unrelated identity cannot consume valid source");
    clock.Invalidate();
    Check(!clock.Current(nested, newer), "resize revokes admission without completing resources");
    for (unsigned field = 0; field < 5; ++field) {
        auto invalid = source;
        switch (field) {
        case 0: invalid.ordinal=0; break; case 1: invalid.feature=0; break;
        case 2: invalid.context=0; break; case 3: invalid.generation=0; break;
        case 4: invalid.output=0; break;
        }
        a=clock.Begin();
        Check(!clock.Complete(a, invalid, true), "missing identity is never inferred");
    }
    for (uint64_t i = 3; i < 10003; ++i) {
        Source s {i,2,3,4,5}; a=clock.Begin();
        Check(clock.Complete(a,s,true) && clock.Claim(a,s), "continuous source admission");
        for (unsigned generated=0; generated<8; ++generated)
            Check(!clock.Claim(a,s), "eight output callbacks remain one model source");
    }
    std::printf("NrPresentationSource: %u checks passed\n", checks);
}
