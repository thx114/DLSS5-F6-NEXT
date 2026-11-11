// SPDX-License-Identifier: GPL-3.0-or-later
// Pure production policy. No replacement shader, queue proof or game claims.
#include <shaders/dlssnr/DlssNr_DetailHistoryPolicy.h>
#include <cstdio>
#include <cstdlib>

namespace
{
using namespace NrDetailHistoryPolicy;
unsigned checks = 0;
void Require(bool condition, const char* reason)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}
bool Near(float x, float y) { return std::abs(x - y) < 1e-6f; }
PhaseFacts Raster()
{
    PhaseFacts p;
    p.requested = p.preSr = p.beforeUpscale = true;
    p.baseIdentity = p.currentDepth = p.consumerMappingVerified = p.routeConvention = true;
    p.metadataKnown = p.flagsKnown = p.jitterKnown = p.motionJitterKnown = p.motionLowResolution = true;
    p.renderWidth = p.motionWidth = p.depthWidth = 8;
    p.renderHeight = p.motionHeight = p.depthHeight = 4;
    p.jitterX = .25f; p.jitterY = -.25f;
    p.currentJitterUvX = .25f / 8; p.currentJitterUvY = -.25f / 4;
    p.decodedScaleX = 1; p.decodedScaleY = -1;
    return p;
}
void Phases()
{
    const auto base = Raster();
    Require(ResolvePhase(base).verified, "actual raster guides use phase zero");
    for (auto member : { &PhaseFacts::baseIdentity, &PhaseFacts::currentDepth,
        &PhaseFacts::consumerMappingVerified, &PhaseFacts::routeConvention, &PhaseFacts::metadataKnown,
        &PhaseFacts::flagsKnown, &PhaseFacts::jitterKnown, &PhaseFacts::motionJitterKnown,
        &PhaseFacts::motionLowResolution })
    {
        auto p = base; p.*member = false;
        Require(!ResolvePhase(p).verified, "missing current authority fails closed");
    }
    auto p = base; p.motionJittered = true;
    Require(!ResolvePhase(p).verified, "jittered raw motion unsupported");
    p = base; p.depthWidth = 16;
    Require(!ResolvePhase(p).verified, "wrong depth raster rejected");
    p = base; p.motionPhaseX = p.currentJitterUvX;
    Require(!ResolvePhase(p).verified, "canonical guide phase wrong on raw raster");
    p = base; p.currentJitterUvY = -p.currentJitterUvY;
    Require(!ResolvePhase(p).verified, "vector Y sign cannot flip jitter address");
    p = base; p.cancelX = .01f;
    Require(!ResolvePhase(p).verified, "history offset must not mutate model guide cancellation");
    p = base; p.jitterX = std::numeric_limits<float>::quiet_NaN();
    Require(!ResolvePhase(p).verified, "NaN jitter rejected");
    p = base; p.jitterY = .5001f;
    Require(!ResolvePhase(p).verified, "out of contract raster jitter rejected");
    p = base; p.preSr = p.beforeUpscale = false; p.nativeScene = p.finishedPicture = true;
    p.motionPhaseX = p.depthPhaseX = p.jitterX / p.renderWidth;
    p.motionPhaseY = p.depthPhaseY = p.jitterY / p.renderHeight;
    Require(ResolvePhase(p).verified, "finished scene retains canonical current-J guide phase");
    p.nativeScene = false;
    Require(!ResolvePhase(p).verified, "finished optical-flow route gains no native permission");
}
void KeysAndPublishedPhase()
{
    using K = Key<8>;
    const Settings settings { 1, 1, .85f };
    K previous;
    previous.identity.placement = Placement::PreSrRaster;
    previous.identity.sourceOwner = { 7, 8, 9 };
    previous.identity.sourceEpoch = 9;
    previous.sample = { 10, 5.0, -.25f / 8, .25f / 4, 100 };
    auto current = previous;
    current.sample = { 11, 5.125, .25f / 8, -.25f / 4, 101 };
    Require(previous == current, "per-capture epoch/time/ordinal/J are snapshots, not stable keys");
    SourceSequence source;
    const auto first = source.Observe(10);
    Require(!first.Names(9, 10), "first input cannot invent a predecessor");
    const auto step = source.Observe(11);
    const auto h = Select(current, previous, true, false, step, settings);
    Require(h.usable && Near(h.deltaSeconds, .125f), "adjacent published history accepted");
    Require(Near(h.rasterOffsetUvX, -.5f / 8) && Near(h.rasterOffsetUvY, .5f / 4),
        "offset is PUBLISHED previous J minus current J");
    Require(Near(1.0f / 8 + h.rasterOffsetUvX, .5f / 8) &&
            Near(-1.0f / 4 + h.rasterOffsetUvY, -.5f / 4), "signed production displacement example");
    for (const float sign : { -1.0f, 1.0f })
    {
        auto c = current, old = previous;
        c.sample.jitterUvX *= sign; c.sample.jitterUvY *= sign;
        old.sample.jitterUvX *= sign; old.sample.jitterUvY *= sign;
        const auto phase = Select(c, old, true, false, step, settings);
        Require(phase.usable && Near(phase.rasterOffsetUvX, sign * -.5f / 8) &&
            Near(phase.rasterOffsetUvY, sign * .5f / 4), "positive/negative XY jitter difference");
    }
    auto neverPublished = previous;
    neverPublished.sample.jitterUvX = .375f / 8;
    const auto decoy = Select(current, neverPublished, true, false, step, settings);
    Require(!Near(decoy.rasterOffsetUvX, h.rasterOffsetUvX), "last-evaluated jitter decoy gives wrong phase");
    Require(!Select(current, previous, false, false, step, settings).usable, "unpublished recording grants nothing");
    Require(!Select(current, previous, true, true, step, settings).usable, "reset rejects published history");
    Require(!Select(current, previous, true, false, source.Observe(11), settings).usable, "duplicate source rejected");
    Require(!Select(current, previous, true, false, source.Observe(13), settings).usable, "busy/missing source stays a gap");
    auto canonical = current, canonicalOld = previous;
    canonical.identity.placement = canonicalOld.identity.placement = Placement::FinishedCanonical;
    const auto post = Select(canonical, canonicalOld, true, false, step, settings);
    Require(post.usable && post.rasterOffsetUvX == 0 && post.rasterOffsetUvY == 0,
        "canonical history already incorporates previous guide phase");
    auto changed = current;
    for (size_t n = 0; n < changed.identity.mapping.size(); ++n)
    {
        changed = current; ++changed.identity.mapping[n];
        Require(!(changed == previous), "every mapping bit isolates history");
    }
    for (size_t n = 0; n < changed.identity.settings.size(); ++n)
    {
        changed = current; ++changed.identity.settings[n];
        Require(!(changed == previous), "every effective config bit isolates history");
    }
    for (size_t n = 0; n < changed.identity.sourceOwner.size(); ++n)
    {
        changed = current; ++changed.identity.sourceOwner[n];
        Require(!(changed == previous), "every preSR source owner component isolates history");
    }
    for (size_t n = 0; n < changed.encoding.size(); ++n)
    {
        changed = current; ++changed.encoding[n];
        Require(!(changed == previous), "proxy transform is exact, without struct tail padding");
    }
    changed = current; ++changed.identity.sourceEpoch;
    Require(!(changed == previous), "stable source feature generation isolates history");
    changed = current; changed.identity.timeDomain = TimeDomain::ConsumerSourceEntrySeconds;
    Require(!(changed == previous), "capture-clock and consumer-entry-clock origins never mix");
    changed = current; changed.sample.sourceSeconds = previous.sample.sourceSeconds;
    Require(!Select(changed, previous, true, false, step, settings).usable, "zero elapsed cannot rejuvenate age");
    changed.sample.sourceSeconds = std::numeric_limits<double>::quiet_NaN();
    Require(!Select(changed, previous, true, false, step, settings).usable, "nonfinite elapsed rejected");
    Require(!SourceStep { UINT64_MAX, true }.Names(UINT64_MAX, 0), "serial wrap is not adjacency");
}
void AgeAndBudget()
{
    Settings s { 1, 1, .85f };
    float age = 0, next = -1;
    for (unsigned n = 0; n < 7; ++n)
    {
        Require(CarryAge(age, .125f, s, next), "rolling feedback carries unexpired ancestry");
        age = next; // Mixing any amount of fresh must NOT reset it to zero.
    }
    Require(age == .875f && !CarryAge(age, .125f, s, next), "exact deadline erases old ancestry");
    Require(CarryAge(0, .125f, s, next) && next == .125f, "fresh reseed can contribute again");
    std::array<float, 4> ages { .125f, .25f, .875f, .5f };
    float oldest = 0;
    for (float tap : ages)
    {
        Require(CarryAge(tap, .0625f, s, next), "all bilinear taps individually unexpired");
        oldest = std::max(oldest, next);
    }
    Require(oldest == .9375f && !CarryAge(oldest, .0625f, s, next), "oldest tap, not average age, controls expiry");
    s.durationSeconds = 0;
    Require(!CarryAge(0, .01f, s, next), "duration zero is fresh-only, even for fresh previous samples");
    Key<1> a, b; a.sample = { 2, 2 }; b.sample = { 1, 1 };
    Require(!Select(a, b, true, false, { 1, true }, s).usable, "duration zero cannot inherit legacy 0.75");
    s.stabilitySeconds=.2f;
    Require(Select(a,b,true,false,{1,true},s).usable,"independent stabilizer can run with long detail disabled");
    Require(!Select(a,b,true,true,{1,true},s).usable,"stabilization never bypasses model/scene reset");
    Require(!Select(a,b,true,false,{7,true},s).usable,"stabilization never bypasses exact source adjacency");
    s.stabilitySeconds=std::numeric_limits<float>::quiet_NaN();
    Require(!s.Valid(),"nonfinite stabilization time fails closed");
    s.stabilitySeconds=0;
    s.durationSeconds = 1;
    Require(!CarryAge(-1, .01f, s, next), "invalid age sentinel rejected");
    Require(!CarryAge(0, -1, s, next), "backward time rejected");
    Require(!CarryAge(0, std::numeric_limits<float>::infinity(), s, next), "infinite elapsed rejected");
    Require(FitsBudget(ByteLimit, 0) && FitsBudget(ByteLimit - 1, 1), "exact aligned allocation budget fits");
    Require(!FitsBudget(ByteLimit, 1) && !FitsBudget(UINT64_MAX, 1) && !FitsBudget(1, UINT64_MAX),
        "retired allocations and overflow cannot expand the history pool");
}
void IndependentRequestAndCadence()
{
    Require(RequestsPresentedFeedback(true, false), "temporal feedback is enabled independently on D3D12");
    Require(!RequestsPresentedFeedback(false, false), "disabled temporal feedback remains off");
    Require(!RequestsPresentedFeedback(true, true), "unsupported backend refuses temporal feedback");
    Require(RequiresFullModelEvaluation(false, 1, true),
        "presented feedback forces a full model evaluation with legacy reuse disabled");
    Require(!RequiresFullModelEvaluation(false, 1, false), "disabled modes retain ordinary bottleneck cadence");
    Require(RequiresFullModelEvaluation(true, 3, false), "legacy interval reuse still reserves full evaluation");
}
}
int main()
{
    Phases(); KeysAndPublishedPhase(); AgeAndBudget(); IndependentRequestAndCadence();
    std::printf("NR independent detail history policy PASS checks=%u\n", checks);
}
