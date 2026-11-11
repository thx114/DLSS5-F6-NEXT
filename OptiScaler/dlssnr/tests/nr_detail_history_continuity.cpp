// CPU-only product policy regression. Lead owns compilation/execution.
// No queue submission, GPU completion, guide provenance or game pixel proof.
#include "../DlssNrDetailReuse.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace
{
using namespace DlssNrDetailReuse;
unsigned checks = 0;
void Require(bool condition, const char* why)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

void LateCapture(unsigned interval, bool sourceClock, unsigned long long step)
{
    InputContinuity inputs;
    Cadence cadence;
    FrameFacts f;
    f.enabled = true;
    f.reuseRun = interval - 1;
    f.spread = interval;
    f.maxStep = sourceClock ? 1 : MaxStepFor(true, step);
    f.frame = 100;
    inputs.Observe(f.frame);
    cadence.Next(f); // capture remains physically pending

    f.frame += step;
    inputs.Observe(f.frame); // BeforeModel's GPU-busy early return
    // Mirrors the existing busy path: end the frame without advancing cadence.
    // The original capture subsequently completes and becomes logically usable.
    cadence.Captured(true);

    f.frame += step;
    auto previous = inputs.Observe(f.frame);
    Require(!previous.Matches(100, f.frame, sourceClock), "late capture cannot skip the intervening input");
    // The backend drops only logical reuse, leaving the physical owner intact.
    cadence.Drop();
    auto d = cadence.Next(f);
    Require(d.kind == Kind::Full && !d.historyUsable && !d.composeMotion && !d.saveShown && d.spreadStep == -1,
        "late history cannot feed Reproject, Steady, Spread or composed model motion");
    const auto freshOrdinal = f.frame;
    cadence.Captured(true); // new complete answer; fixture supplies completion separately

    f.frame += step;
    previous = inputs.Observe(f.frame);
    Require(previous.Matches(freshOrdinal, f.frame, sourceClock), "fresh adjacent capture restores correspondence");
    d = cadence.Next(f);
    Require(d.historyUsable && d.kind == (interval == 1 ? Kind::Full : Kind::Reuse),
        "interval-1 feedback and skipped-model cadence both recover");
}
}

int main()
{
    for (unsigned interval = 1; interval <= 8; ++interval)
    {
        LateCapture(interval, true, 1);
        LateCapture(interval, false, 1);
        LateCapture(interval, false, 6); // legitimate physical FG Present clock
    }
    InputContinuity inputs;
    Require(!inputs.Observe(10).Matches(9, 10, true), "no fabricated previous endpoint on first input");
    Require(inputs.Observe(11).Matches(10, 11, true), "adjacent source positive control");
    Require(!inputs.Observe(11).Matches(10, 11, true), "duplicate input cannot read older detail");
    Require(!inputs.Observe(8).Matches(7, 8, true), "rollback cannot read earlier lifetime");
    auto previous = inputs.Observe(14);
    Require(!previous.Matches(8, 14, true), "source gap remains rejected with no intermediate observation");
    Require(previous.Matches(8, 14, false), "physical clock may advance across generated presents");
    inputs.Reset();
    Require(!inputs.Observe(15).Matches(14, 15, false), "release forgets observed endpoint");
    inputs.Observe((std::numeric_limits<unsigned long long>::max)());
    Require(!inputs.Observe(0).Matches((std::numeric_limits<unsigned long long>::max)(), 0, true),
        "counter wrap is not adjacency");
    std::printf("NR detail history continuity CPU PASS checks=%u\n", checks);
}
