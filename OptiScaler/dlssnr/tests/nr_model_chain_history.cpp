// CPU regression for the DX12 model-chain receipt used before AfterModel.
// Exercises cadence recovery after a displayable prefix and a failed selected
// Look. No submission, GPU completion, real input mapping or pixel proof.
#include "../DlssNrDetailReuse.h"

#include <cstdio>
#include <cstdlib>

namespace
{
using namespace DlssNrDetailReuse;
unsigned checks = 0;

void Require(bool condition, const char* why)
{
    ++checks;
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", why);
        std::exit(1);
    }
}

void CheckRecovery(unsigned reuseRun, bool dropOnFailure)
{
    Cadence cadence;
    FrameFacts frame;
    frame.enabled = true;
    frame.revision = 123;
    frame.reuseRun = reuseRun;
    frame.frame = 10;
    auto decision = cadence.Next(frame);
    Require(decision.kind == Kind::Full && !decision.historyUsable, "initial full answer");
    cadence.Captured(HistoryAnswerComplete(true, false, 3, 3));

    ++frame.frame;
    frame.hold = true; // force a model run even on a would-be reuse frame
    decision = cadence.Next(frame);
    Require(decision.kind == Kind::Full && decision.historyUsable, "adjacent model refresh");
    // The first Evaluate succeeds but its interpass clamp cannot be recorded.
    // The existing display prefix remains successful, with only one of three passes.
    const bool prefixComplete = HistoryAnswerComplete(true, false, 3, 1);
    Require(!prefixComplete, "displayable prefix is not complete-chain history");
    if (dropOnFailure) cadence.Drop(); // temporal feedback failure path
    else cadence.Captured(prefixComplete); // ordinary capture failure path

    ++frame.frame;
    frame.hold = false;
    decision = cadence.Next(frame);
    Require(decision.kind == Kind::Full && !decision.historyUsable,
            "next source cannot reuse or fuse the shortened answer");
    cadence.Captured(HistoryAnswerComplete(true, false, 3, 3));

    ++frame.frame;
    decision = cadence.Next(frame);
    Require(decision.historyUsable, "a genuine complete refresh restores adjacent history");
    Require(decision.kind == (reuseRun ? Kind::Reuse : Kind::Full), "configured cadence resumes");
    Require(HistoryAnswerComplete(true, decision.kind == Kind::Reuse, 3, reuseRun ? 0 : 3),
            "complete-model and authenticated reuse answers remain eligible");
}
}

int main()
{
    using namespace DlssNrDetailReuse;
    for (unsigned planned = 1; planned <= 6; ++planned)
    {
        for (unsigned completed = 0; completed < planned; ++completed)
            Require(!HistoryAnswerComplete(true, false, planned, completed), "reject every shortened chain");
        Require(HistoryAnswerComplete(true, false, planned, planned), "accept the completed plan");
        Require(!HistoryAnswerComplete(false, false, planned, planned), "failed selected Look cannot seed history");
        Require(!HistoryAnswerComplete(false, true, planned, 0), "failed reuse cannot seed history");
        Require(HistoryAnswerComplete(true, true, planned, 0), "authenticated reuse does not call the model");
        Require(!HistoryAnswerComplete(true, false, planned, planned + 1), "reject a different chain length");
    }
    Require(!HistoryAnswerComplete(true, false, 0, 0), "an empty plan cannot seed history");
    Require(!HistoryAnswerComplete(true, true, 0, 0), "reuse cannot manufacture an empty plan");
    CheckRecovery(0, true);  // interval-1 full-model feedback
    CheckRecovery(1, true); // temporal skipped-model cadence
    CheckRecovery(1, false); // ordinary detail capture
    std::printf("NR model-chain history CPU PASS checks=%u\n", checks);
}
