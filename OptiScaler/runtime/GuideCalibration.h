// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

// CPU observations only. Call on the diagnostics worker with immutable, genuinely
// completed readbacks. This code cannot authorize runtime input, GPU retirement,
// config changes, or history feedback. No graphics/platform headers or threads.
namespace GuideCalibration {
inline constexpr unsigned MaxWidth = 160, MaxHeight = 100, CandidateCount = 8;
struct Vec2 { float x = 0, y = 0; };
struct Pixel { float r = 0, g = 0, b = 0; };
struct Rect { uint32_t x = 0, y = 0, width = 0, height = 0; };
struct Region { uint32_t textureWidth = 0, textureHeight = 0; Rect rect; };
enum class DepthConvention { Unknown, NearZeroFarOne, NearOneFarZero };
enum class MotionUnits { Unknown, CanonicalUv, RenderPixels, MotionTexturePixels };
enum class MotionJitter { Unknown, Excluded, Included };
// Explicit contract for where the CPU image pixels are addressed. In the first
// mode jitter is provenance only. The second explicitly defines jitterUv as
// canonical sample-position offsets: Cj(x)=C0(x+j), hence a current->previous
// colour lookup adds CURRENT-minus-PREVIOUS jitter. The mixed mode keeps colour
// canonical but addresses raw raster guides at canonicalUV + CURRENT jitter,
// before reflecting their rows. These are different coordinate contracts.
enum class AddressSemantics { Unknown, CanonicalUnjittered, CanonicalSampleOffsets, CanonicalColourRasterGuides };
enum class Stage { Still, Horizontal, Vertical, Character };
struct Metadata {
    uint64_t source = 0, epoch = 0, feature = 0, configuration = 0, domain = 0;
    uint64_t capturedAtMs = 0, observedAtMs = 0;
    uint32_t renderWidth = 0, renderHeight = 0;
    Region colourRegion, depthRegion, motionRegion;
    // Raw*motionToUv is explicitly normalized current->previous canonical UV.
    // No hidden division by texture size: the sampler declares effective scaling
    // (including a source subrect), while motionUnits records the raw units.
    // Never synthesize 1/width defaults for missing NGX scale/provenance.
    float motionToUvX = 0, motionToUvY = 0;
    Vec2 jitterUv, previousJitterUv;
    DepthConvention depthConvention = DepthConvention::Unknown;
    MotionUnits motionUnits = MotionUnits::Unknown;
    MotionJitter motionJitter = MotionJitter::Unknown;
    AddressSemantics addressSemantics = AddressSemantics::Unknown;
    bool known = false, rawSceneBeforeNr = false, rawGuides = false;
    bool currentJitterKnown = false, previousJitterKnown = false;
    bool sourceContinuous = false, epochContinuous = false;
};
struct Frame {
    uint32_t width = 0, height = 0;
    // All arrays are the same reduced grid. Colour has canonical top-down rows;
    // depth/MV retain independently raw rows within their own source subrects.
    // RGB must use the same finite [0,1] linear normalization in both frames.
    // Depth must already be declared [0,1] device depth, not guessed/linearized.
    std::vector<Pixel> colour;
    std::vector<float> depth;
    std::vector<Vec2> motion;
    // Empty mask means every array element was actually supplied. Zero excludes
    // that element; masks follow their respective array's raw row ordering.
    std::vector<uint8_t> colourMask, depthMask, motionMask;
    Metadata metadata;
    // Canonical grid pixels, e.g. HUD/character exclusions; maximum 32 per frame.
    std::vector<Rect> exclusions;
};
struct CandidateFlags {
    bool depthRows = false, motionRows = false, vectorY = false;
    constexpr unsigned Index() const noexcept {
        return (depthRows ? 1u : 0u) | (motionRows ? 2u : 0u) | (vectorY ? 4u : 0u);
    }
};
inline constexpr CandidateFlags Candidate(unsigned index) noexcept {
    return {(index & 1u) != 0, (index & 2u) != 0, (index & 4u) != 0};
}
enum class Rejection {
    None, BadOptions, BadDimensions, BadArrays, BadMetadata, NoDepth,
    Stale, SourceGap, EpochGap, DomainChanged, FeatureChanged, ConfigurationChanged,
    MappingChanged, InsufficientCoverage, UnequalCandidateCoverage, OutOfBounds, StaticMotion, SceneCut,
    ManualStage, StageMotionMismatch, SessionFull, NeedPrevious, InsufficientPairs, Tied,
    AxisDisagreement, RatioBelowThreshold
};
inline const char* Reason(Rejection r) noexcept {
    switch (r) {
    case Rejection::None: return "numeric evidence available";
    case Rejection::BadOptions: return "invalid calibration options";
    case Rejection::BadDimensions: return "invalid or oversized reduced grid";
    case Rejection::BadArrays: return "missing or mismatched pixel arrays/masks";
    case Rejection::BadMetadata: return "unknown or invalid raw guide metadata";
    case Rejection::NoDepth: return "no valid declared depth";
    case Rejection::Stale: return "stale or unordered readback timestamps";
    case Rejection::SourceGap: return "nonconsecutive source frames";
    case Rejection::EpochGap: return "nonconsecutive source epochs";
    case Rejection::DomainChanged: return "source domain changed";
    case Rejection::FeatureChanged: return "feature changed";
    case Rejection::ConfigurationChanged: return "configuration changed";
    case Rejection::MappingChanged: return "grid/region/scale/semantics changed";
    case Rejection::InsufficientCoverage: return "insufficient valid overlap";
    case Rejection::UnequalCandidateCoverage: return "best candidate loses more than 15 percent coverage versus alternatives";
    case Rejection::OutOfBounds: return "motion leaves image bounds";
    case Rejection::StaticMotion: return "insufficient scene motion";
    case Rejection::SceneCut: return "no candidate explains scene transition";
    case Rejection::ManualStage: return "display/manual verification stage";
    case Rejection::StageMotionMismatch: return "motion does not establish requested axis";
    case Rejection::SessionFull: return "bounded stage evidence full";
    case Rejection::NeedPrevious: return "waiting for preceding immutable snapshot";
    case Rejection::InsufficientPairs: return "need 24 valid pairs per axis";
    case Rejection::Tied: return "candidate scores tied";
    case Rejection::AxisDisagreement: return "horizontal/vertical evidence disagrees";
    case Rejection::RatioBelowThreshold: return "best/runner gap below 15 percent";
    }
    return "unknown rejection";
}
struct Options {
    uint64_t maxAgeMs = 1500, maxPairIntervalMs = 250;
    float minCoverage = .55f, minMotionPixels = .35f, axisDominance = 1.25f;
    float depthTolerance = .025f, edgeThreshold = .015f, sceneCutError = .25f;
    float minRelativeGap = .15f;
    unsigned minPairsPerAxis = 24, maxPairsPerAxis = 96, minSamples = 32;
};
struct Metrics {
    double score = 0, reprojection = 0, depthConsistency = 0, edgeAlignment = 0;
    double coverage = 0, occlusionFraction = 0, outOfBoundsFraction = 0;
    double motionX = 0, motionY = 0, depthEdgeEvidence = 0;
    uint32_t eligible = 0, depthSamples = 0, motionSamples = 0, compared = 0, occluded = 0, outOfBounds = 0;
};
struct CandidateResult {
    CandidateFlags flags;
    Metrics metrics;
    Rejection rejection = Rejection::InsufficientCoverage;
    bool valid = false;
};
struct PairResult {
    std::array<CandidateResult, CandidateCount> candidates{};
    Rejection rejection = Rejection::BadMetadata;
    uint64_t source = 0, epoch = 0;
    bool valid = false;
};

namespace Detail {
inline bool Finite(float f) noexcept { return std::isfinite(f); }
inline bool Unit(float f) noexcept { return Finite(f) && f >= 0 && f <= 1; }
inline bool Good(Pixel p) noexcept { return Unit(p.r) && Unit(p.g) && Unit(p.b); }
inline bool Good(Vec2 v) noexcept { return Finite(v.x) && Finite(v.y); }
inline double Difference(Pixel a, Pixel b) noexcept {
    return (std::abs(double(a.r) - b.r) + std::abs(double(a.g) - b.g) + std::abs(double(a.b) - b.b)) / 3;
}
inline bool Same(Rect a, Rect b) noexcept {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
inline bool Good(Rect r, uint32_t w, uint32_t h) noexcept {
    return r.width && r.height && r.x <= w && r.y <= h && r.width <= w-r.x && r.height <= h-r.y;
}
inline bool Good(Region r) noexcept { return Good(r.rect, r.textureWidth, r.textureHeight); }
inline bool Same(Region a, Region b) noexcept {
    return a.textureWidth == b.textureWidth && a.textureHeight == b.textureHeight && Same(a.rect,b.rect);
}
inline bool GoodOptions(const Options& o) noexcept {
    return o.maxAgeMs && o.maxPairIntervalMs && Unit(o.minCoverage) && o.minCoverage > 0 &&
        Finite(o.minMotionPixels) && o.minMotionPixels > 0 && Finite(o.axisDominance) && o.axisDominance > 1 &&
        Unit(o.depthTolerance) && o.depthTolerance > 0 && Unit(o.edgeThreshold) && o.edgeThreshold > 0 &&
        Unit(o.sceneCutError) && o.sceneCutError > 0 && Unit(o.minRelativeGap) && o.minRelativeGap >= .15f &&
        o.minPairsPerAxis >= 24 && o.maxPairsPerAxis >= o.minPairsPerAxis && o.maxPairsPerAxis <= 128 &&
        o.minSamples >= 4 && o.minSamples <= MaxWidth * MaxHeight;
}
inline Rejection ValidateFrame(const Frame& f, uint64_t now, const Options& o) noexcept {
    if (f.width < 3 || f.height < 3 || f.width > MaxWidth || f.height > MaxHeight)
        return Rejection::BadDimensions;
    const size_t n = size_t(f.width) * f.height;
    if (f.depth.empty()) return Rejection::NoDepth;
    if (f.colour.size() != n || f.depth.size() != n || f.motion.size() != n ||
        (!f.colourMask.empty() && f.colourMask.size() != n) ||
        (!f.depthMask.empty() && f.depthMask.size() != n) ||
        (!f.motionMask.empty() && f.motionMask.size() != n)) return Rejection::BadArrays;
    const auto& m = f.metadata;
    if (!m.known || !m.rawSceneBeforeNr || !m.rawGuides || !m.source || !m.epoch ||
        !m.feature || !m.configuration || !m.domain || !m.renderWidth || !m.renderHeight ||
        !Good(m.colourRegion) || !Good(m.depthRegion) || !Good(m.motionRegion) ||
        !Finite(m.motionToUvX) || !Finite(m.motionToUvY) || m.motionToUvX == 0 || m.motionToUvY == 0 ||
        !m.currentJitterKnown || !Good(m.jitterUv) || std::abs(m.jitterUv.x) > 2.0 / m.renderWidth ||
        std::abs(m.jitterUv.y) > 2.0 / m.renderHeight ||
        (m.depthConvention != DepthConvention::NearZeroFarOne && m.depthConvention != DepthConvention::NearOneFarZero) ||
        (m.motionUnits != MotionUnits::CanonicalUv && m.motionUnits != MotionUnits::RenderPixels &&
         m.motionUnits != MotionUnits::MotionTexturePixels) ||
        (m.motionJitter != MotionJitter::Included && m.motionJitter != MotionJitter::Excluded) ||
        (m.addressSemantics != AddressSemantics::CanonicalUnjittered &&
         m.addressSemantics != AddressSemantics::CanonicalSampleOffsets &&
         m.addressSemantics != AddressSemantics::CanonicalColourRasterGuides) ||
        (m.addressSemantics != AddressSemantics::CanonicalSampleOffsets && m.motionJitter != MotionJitter::Excluded) ||
        f.exclusions.size() > 32)
        return Rejection::BadMetadata;
    for (const auto r : f.exclusions) if (!Good(r, f.width, f.height)) return Rejection::BadMetadata;
    if (!m.capturedAtMs || now < m.capturedAtMs || now-m.capturedAtMs > o.maxAgeMs) return Rejection::Stale;
    return Rejection::None;
}
inline Rejection ValidatePair(const Frame& p, const Frame& c, uint64_t now, const Options& o) noexcept {
    if (!GoodOptions(o)) return Rejection::BadOptions;
    auto r = ValidateFrame(p, now, o); if (r != Rejection::None) return r;
    r = ValidateFrame(c, now, o); if (r != Rejection::None) return r;
    const auto& a = p.metadata; const auto& b = c.metadata;
    if (!b.sourceContinuous || b.source <= a.source || b.source-a.source != 1) return Rejection::SourceGap;
    if (!b.epochContinuous || b.epoch <= a.epoch || b.epoch-a.epoch != 1) return Rejection::EpochGap;
    if (a.domain != b.domain) return Rejection::DomainChanged;
    if (a.feature != b.feature) return Rejection::FeatureChanged;
    if (a.configuration != b.configuration) return Rejection::ConfigurationChanged;
    if (!b.previousJitterKnown || !Good(b.previousJitterUv) ||
        b.previousJitterUv.x != a.jitterUv.x || b.previousJitterUv.y != a.jitterUv.y) return Rejection::BadMetadata;
    if (b.capturedAtMs <= a.capturedAtMs || b.capturedAtMs-a.capturedAtMs > o.maxPairIntervalMs) return Rejection::Stale;
    if (p.width != c.width || p.height != c.height || a.renderWidth != b.renderWidth || a.renderHeight != b.renderHeight ||
        !Same(a.colourRegion,b.colourRegion) || !Same(a.depthRegion,b.depthRegion) || !Same(a.motionRegion,b.motionRegion) ||
        a.depthConvention != b.depthConvention || a.motionUnits != b.motionUnits || a.motionJitter != b.motionJitter ||
        a.addressSemantics != b.addressSemantics ||
        a.motionToUvX != b.motionToUvX || a.motionToUvY != b.motionToUvY)
        return Rejection::MappingChanged;
    return Rejection::None;
}
inline bool Mask(const std::vector<uint8_t>& mask, size_t i) noexcept { return mask.empty() || mask[i] != 0; }
inline bool Excluded(const Frame& f, unsigned x, unsigned y) noexcept {
    for (const auto r : f.exclusions)
        if (x >= r.x && y >= r.y && x-r.x < r.width && y-r.y < r.height) return true;
    return false;
}
inline size_t Index(const Frame& f, unsigned x, unsigned y, bool flip = false) noexcept {
    return size_t(flip ? f.height-1-y : y)*f.width+x;
}
inline bool Colour(const Frame& f, unsigned x, unsigned y, Pixel& p) noexcept {
    const auto i = Index(f,x,y); p = f.colour[i];
    return Mask(f.colourMask,i) && !Excluded(f,x,y) && Good(p);
}
struct BilinearSupport {
    std::array<unsigned,4> x{}, y{};
    std::array<double,4> weight{};
};
inline bool Support(const Frame& f, double x, double y, BilinearSupport& s) noexcept {
    // Never clamp a missing border sample or renormalize surviving taps: either
    // would turn unavailable/invalid support into apparently valid evidence.
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x > f.width-1 || y > f.height-1) return false;
    const auto x0=static_cast<unsigned>(x), y0=static_cast<unsigned>(y);
    const auto x1=(std::min)(x0+1,f.width-1), y1=(std::min)(y0+1,f.height-1);
    const double tx=x-x0, ty=y-y0;
    s.x={x0,x1,x0,x1}; s.y={y0,y0,y1,y1};
    s.weight={(1-tx)*(1-ty),tx*(1-ty),(1-tx)*ty,tx*ty};
    return true;
}
inline bool GuideSupport(const Frame& f, double x, double y, bool flip, BilinearSupport& s) noexcept {
    if (f.metadata.addressSemantics == AddressSemantics::CanonicalColourRasterGuides) {
        x += double(f.metadata.jitterUv.x)*f.width;
        y += double(f.metadata.jitterUv.y)*f.height;
    }
    // Phase is in canonical coordinates; a raw row reflection changes its sign
    // implicitly through the address, never through vector-Y negation.
    if (flip) y=f.height-1-y;
    return Support(f,x,y,s);
}
inline bool Depth(const Frame& f, double x, double y, bool flip, float& d) noexcept {
    BilinearSupport s; if (!GuideSupport(f,x,y,flip,s)) return false;
    d=0;
    for (unsigned j=0;j<4;++j) {
        if (s.weight[j]<=1e-10) continue;
        const auto i=Index(f,s.x[j],s.y[j]);
        if (!Mask(f.depthMask,i) || !Unit(f.depth[i])) return false;
        d+=static_cast<float>(s.weight[j]*f.depth[i]);
    }
    // Only declared inversion; do not estimate the near/far convention.
    if (f.metadata.depthConvention == DepthConvention::NearOneFarZero) d = 1-d;
    return true;
}
inline bool Motion(const Frame& p, const Frame& c, unsigned x, unsigned y, CandidateFlags flags, Vec2& v) noexcept {
    BilinearSupport s; if (!GuideSupport(c,x,y,flags.motionRows,s)) return false;
    v={};
    for (unsigned j=0;j<4;++j) {
        if (s.weight[j]<=1e-10) continue;
        const auto i=Index(c,s.x[j],s.y[j]); const auto raw=c.motion[i];
        if (!Mask(c.motionMask,i) || !Good(raw)) return false;
        v.x+=static_cast<float>(s.weight[j]*raw.x); v.y+=static_cast<float>(s.weight[j]*raw.y);
    }
    const auto& m = c.metadata;
    v.x *= m.motionToUvX; v.y *= m.motionToUvY;
    if (flags.vectorY) v.y = -v.y;
    // jitterUv is the canonical rendered sample offset, not raw NGX units.
    // Jitter-excluded MV becomes current->previous sample UV exactly once.
    if (m.addressSemantics == AddressSemantics::CanonicalSampleOffsets && m.motionJitter == MotionJitter::Excluded) {
        v.x += m.jitterUv.x-p.metadata.jitterUv.x;
        v.y += m.jitterUv.y-p.metadata.jitterUv.y;
    }
    return Good(v);
}
struct Sample { Pixel colour; float depth = 0; };
inline bool SamplePrevious(const Frame& f, double x, double y, bool flip, Sample& sample) noexcept {
    BilinearSupport s; if (!Support(f,x,y,s)) return false;
    sample = {};
    for (unsigned j = 0; j < 4; ++j) {
        if (s.weight[j] <= 1e-10) continue;
        Pixel p; if (!Colour(f,s.x[j],s.y[j],p)) return false;
        sample.colour.r += static_cast<float>(s.weight[j]*p.r);
        sample.colour.g += static_cast<float>(s.weight[j]*p.g);
        sample.colour.b += static_cast<float>(s.weight[j]*p.b);
    }
    // Read raw previous depth directly at the subpixel canonical lookup plus
    // previous jitter. Interpolating already resampled integer guides here would
    // filter twice and smear depth/occlusion evidence.
    return Depth(f,x,y,flip,sample.depth);
}
struct Observation {
    Pixel warped;
    double residual = 0, depthError = 0, edgeError = 0, depthEdge = 0;
    double motionX = 0, motionY = 0;
    bool eligible = false, depthValid = false, motionValid = false;
    bool compared = false, occluded = false, outOfBounds = false;
};
inline Observation Observe(const Frame& p, const Frame& c, unsigned x, unsigned y,
                           CandidateFlags flags, const Options& o) noexcept {
    Observation v; Pixel colour; float depth;
    if (!Colour(c,x,y,colour)) return v;
    v.eligible = true;
    if (!Depth(c,x,y,flags.depthRows,depth)) return v;
    v.depthValid = true;
    Vec2 motion;
    if (!Motion(p,c,x,y,flags,motion)) return v;
    v.motionValid = true;
    // Ignore jitter for axis/motion excitation: jitter alone is not camera evidence.
    Vec2 scene = motion;
    if (c.metadata.addressSemantics == AddressSemantics::CanonicalSampleOffsets) {
        scene.x -= c.metadata.jitterUv.x-p.metadata.jitterUv.x;
        scene.y -= c.metadata.jitterUv.y-p.metadata.jitterUv.y;
    }
    v.motionX = std::abs(double(scene.x)*c.width); v.motionY = std::abs(double(scene.y)*c.height);
    const double px = x+double(motion.x)*c.width, py = y+double(motion.y)*c.height;
    if (px < 0 || py < 0 || px > p.width-1 || py > p.height-1) { v.outOfBounds = true; return v; }
    Sample previous;
    if (!SamplePrevious(p,px,py,flags.depthRows,previous)) return v;
    v.warped = previous.colour; v.compared = true;
    v.residual = Difference(colour,previous.colour);
    v.depthError = (std::min)(1.0,std::abs(double(depth)-previous.depth)/o.depthTolerance);
    // Conservative visible-depth consistency diagnostic, not a disocclusion grant.
    // All compared pixels retain their RGB residual, including occluded pixels.
    v.occluded = previous.depth+o.depthTolerance < depth;
    double edgeError = 0, evidence = 0; unsigned neighbours = 0;
    const unsigned nx[2] = {(std::min)(x+1,c.width-1),x};
    const unsigned ny[2] = {y,(std::min)(y+1,c.height-1)};
    for (unsigned j = 0; j < 2; ++j) {
        if (nx[j] == x && ny[j] == y) continue;
        Pixel other; float od;
        if (!Colour(c,nx[j],ny[j],other) || !Depth(c,nx[j],ny[j],flags.depthRows,od)) continue;
        const double de = (std::min)(1.0,std::abs(double(depth)-od)/o.edgeThreshold);
        const double ce = (std::min)(1.0,Difference(colour,other)/o.edgeThreshold);
        // Penalize unaligned depth edges, without demanding a depth discontinuity
        // for every albedo edge. Flat depth remains explicitly ambiguous.
        edgeError += de*(1-ce); evidence += de; ++neighbours;
    }
    if (neighbours) { v.edgeError = edgeError/neighbours; v.depthEdge = evidence/neighbours; }
    return v;
}
inline CandidateResult ScoreCandidate(const Frame& p, const Frame& c, CandidateFlags flags, const Options& o,
                                     const std::vector<uint8_t>* commonSupport=nullptr) noexcept {
    CandidateResult result; result.flags = flags; auto& m = result.metrics;
    double commonLoss=0;unsigned commonCount=0;
    for (unsigned y = 0; y < c.height; ++y) for (unsigned x = 0; x < c.width; ++x) {
        const auto v = Observe(p,c,x,y,flags,o);
        if (!v.eligible) continue;
        ++m.eligible; m.motionX += v.motionX; m.motionY += v.motionY;
        if (v.depthValid) ++m.depthSamples;
        if (v.motionValid) ++m.motionSamples;
        if (v.outOfBounds) ++m.outOfBounds;
        if (!v.compared) continue;
        ++m.compared; if (v.occluded) ++m.occluded;
        if(commonSupport&&(*commonSupport)[size_t(y)*c.width+x]){
            commonLoss+=.60*v.residual+.20*v.depthError+.15*v.edgeError;++commonCount;
        }
        m.reprojection += v.residual; m.depthConsistency += v.depthError;
        m.edgeAlignment += v.edgeError; m.depthEdgeEvidence += v.depthEdge;
    }
    if (!m.depthSamples) { result.rejection = Rejection::NoDepth; return result; }
    m.coverage = double(m.compared)/m.eligible;
    m.outOfBoundsFraction = double(m.outOfBounds)/m.eligible;
    m.motionX /= m.eligible; m.motionY /= m.eligible;
    if (m.compared) {
        m.reprojection /= m.compared; m.depthConsistency /= m.compared;
        m.edgeAlignment /= m.compared; m.depthEdgeEvidence /= m.compared;
        m.occlusionFraction = double(m.occluded)/m.compared;
    }
    // Every eligible canonical colour pixel has the SAME denominator for every
    // candidate. Missing comparisons (guide masks, bad taps, OOB, exclusions)
    // incur worst loss 1, not a tiny surcharge on a surviving-pixel average.
    // Observed metrics remain conditional means for display/scene-cut checks.
    m.score = m.coverage*(.60*m.reprojection + .20*m.depthConsistency + .15*m.edgeAlignment) + (1-m.coverage);
    // Pair ranking uses exactly the same observed pixels for all candidates.
    // Unavoidable borders cannot reward a wrong direction with more overlap,
    // and a candidate cannot hide a difficult pixel solely from its own score.
    if (m.compared < o.minSamples || m.coverage < o.minCoverage) {
        result.rejection = m.outOfBoundsFraction > 1-o.minCoverage ? Rejection::OutOfBounds : Rejection::InsufficientCoverage;
        return result;
    }
    if(commonSupport){
        if(commonCount<o.minSamples||double(commonCount)/m.eligible<o.minCoverage){result.rejection=Rejection::InsufficientCoverage;return result;}
        m.score=commonLoss/commonCount;
    }
    if ((std::max)(m.motionX,m.motionY) < o.minMotionPixels) { result.rejection = Rejection::StaticMotion; return result; }
    result.valid = true; result.rejection = Rejection::None; return result;
}
} // namespace Detail

inline PairResult ScorePair(const Frame& previous, const Frame& current, uint64_t nowMs, const Options& options = {}) {
    PairResult result; result.source = current.metadata.source; result.epoch = current.metadata.epoch;
    for (unsigned i = 0; i < CandidateCount; ++i) result.candidates[i].flags = Candidate(i);
    result.rejection = Detail::ValidatePair(previous,current,nowMs,options);
    if (result.rejection != Rejection::None) {
        for (auto& c : result.candidates) c.rejection = result.rejection;
        return result;
    }
    std::vector<uint8_t> commonSupport(size_t(current.width)*current.height,1);
    for(unsigned y=0;y<current.height;++y)for(unsigned x=0;x<current.width;++x){
        for(unsigned candidate=0;candidate<CandidateCount;++candidate){
            if(!Detail::Observe(previous,current,x,y,Candidate(candidate),options).compared){commonSupport[size_t(y)*current.width+x]=0;break;}
        }
    }
    double bestResidual = (std::numeric_limits<double>::max)();
    bool depth = false, bounds = false, moving = false;
    for (unsigned i = 0; i < CandidateCount; ++i) {
        auto& c = result.candidates[i] = Detail::ScoreCandidate(previous,current,Candidate(i),options,&commonSupport);
        depth |= c.metrics.depthSamples != 0;
        bounds |= c.rejection == Rejection::OutOfBounds;
        moving |= c.rejection != Rejection::StaticMotion;
        if (c.metrics.compared >= options.minSamples && c.metrics.coverage >= options.minCoverage)
            bestResidual = (std::min)(bestResidual,c.metrics.reprojection);
        if (c.valid) result.valid = true;
    }
    if (bestResidual != (std::numeric_limits<double>::max)() && bestResidual > options.sceneCutError) {
        result.valid = false; result.rejection = Rejection::SceneCut;
        for (auto& c : result.candidates) { c.valid = false; c.rejection = Rejection::SceneCut; }
        return result;
    }
    if (!result.valid) {
        result.rejection = !depth ? Rejection::NoDepth : !moving ? Rejection::StaticMotion :
            bounds ? Rejection::OutOfBounds : Rejection::InsufficientCoverage;
        return result;
    }
    if(result.valid){
        const CandidateResult* best=nullptr;double maximumCoverage=0;
        for(const auto& c:result.candidates)if(c.valid){
            maximumCoverage=(std::max)(maximumCoverage,c.metrics.coverage);
            if(!best||c.metrics.score<best->metrics.score)best=&c;
        }
        // Common support makes the errors comparable. It cannot explain away
        // a large amount of missing evidence: abstain if the apparent winner
        // discards materially more image than a competing interpretation.
        if(best&&maximumCoverage-best->metrics.coverage>.15){
            result.valid=false;result.rejection=Rejection::UnequalCandidateCoverage;
            for(auto& c:result.candidates){c.valid=false;c.rejection=result.rejection;}
        }
    }
    return result;
}

struct Aggregate {
    CandidateFlags flags;
    unsigned pairs = 0;
    Metrics mean;
};
enum class RecommendationKind { None, MotionOnly, Full };
struct Recommendation {
    RecommendationKind kind = RecommendationKind::None;
    Rejection rejection = Rejection::InsufficientPairs;
    CandidateFlags flags;
    bool depthCertain = false;
    double relativeGap = 0, horizontalGap = 0, verticalGap = 0;
    unsigned horizontalPairs = 0, verticalPairs = 0;
    std::array<Aggregate,CandidateCount> horizontal{}, vertical{};
};
inline double RelativeGap(double best, double runner) noexcept {
    if (!std::isfinite(best) || !std::isfinite(runner) || best < 0 || runner <= best) return 0;
    // Bilinear float sampling noise on a flat field is not direction evidence.
    constexpr double scoreNoiseFloor = 1e-6;
    if (runner-best <= scoreNoiseFloor) return 0;
    return (runner-best)/(std::max)(runner,1e-9);
}

// Single-worker session; bounded accumulators and at most one preceding frame.
// Invalid pair identity discards prior recommendation evidence. ObservePair may
// collect disjoint, individually consecutive pairs: no pixel history is joined
// across their sampling gap. Observe(Frame) requires a continuous input stream.
// Still and Character are display/manual stages and never contribute votes.
class Session {
public:
    explicit Session(Options options = {}) : options_(options) { Reset(); }
    void Start() noexcept { Reset(); stage_ = Stage::Still; }
    void SetStage(Stage stage) noexcept { stage_ = stage; }
    Stage CurrentStage() const noexcept { return stage_; }
    PairResult ObservePair(const Frame& previous, const Frame& current, uint64_t nowMs) {
        return Observe(previous,current,stage_,nowMs);
    }
    void Reset() noexcept {
        horizontal_ = {}; vertical_ = {}; horizontalPairs_ = verticalPairs_ = 0;
        lastSource_ = lastEpoch_ = domain_ = feature_ = configuration_ = lastCapture_ = 0;
        lastMetadata_ = {}; lastWidth_ = lastHeight_ = 0;
        previous_ = {}; hasPrevious_ = false;
        for (unsigned i = 0; i < CandidateCount; ++i) {
            horizontal_[i].flags = Candidate(i); vertical_[i].flags = Candidate(i);
        }
    }
    // Owns at most one previous reduced snapshot. Pass std::move(frame) from the
    // Lead worker to avoid a copy. Freshness uses caller-provided monotonic times,
    // never a platform clock or global context. The first frame cannot vote.
    PairResult Observe(Frame frame) {
        const uint64_t now = frame.metadata.observedAtMs;
        const auto invalid = !Detail::GoodOptions(options_) ? Rejection::BadOptions :
            Detail::ValidateFrame(frame,now,options_);
        if (invalid != Rejection::None) { Reset(); PairResult r; r.rejection = invalid; return r; }
        PairResult result;
        if (hasPrevious_) result = Observe(previous_,frame,stage_,now);
        else { result.source = frame.metadata.source; result.epoch = frame.metadata.epoch; result.rejection = Rejection::NeedPrevious; }
        previous_ = std::move(frame); hasPrevious_ = true;
        return result;
    }
    PairResult Observe(const Frame& previous, const Frame& current, Stage stage, uint64_t nowMs) {
        auto result = ScorePair(previous,current,nowMs,options_);
        // Ordinary low-information frames (notably the Still/Character stages)
        // neither contribute nor destroy already collected calibration evidence.
        // Invalid identity, timestamps, mappings, and scene cuts invalidate it.
        if (!result.valid && result.rejection != Rejection::StaticMotion &&
            result.rejection != Rejection::InsufficientCoverage && result.rejection != Rejection::OutOfBounds &&
            result.rejection != Rejection::NoDepth) { Reset(); return result; }
        const auto& m = current.metadata;
        const auto& pm = previous.metadata;
        const bool replay = lastSource_ && (pm.source < lastSource_ || pm.epoch < lastEpoch_ ||
            ((pm.source == lastSource_) != (pm.epoch == lastEpoch_)) || pm.capturedAtMs < lastCapture_);
        const bool mappingChanged = lastSource_ && (previous.width != lastWidth_ || previous.height != lastHeight_ ||
            pm.renderWidth != lastMetadata_.renderWidth || pm.renderHeight != lastMetadata_.renderHeight ||
            !Detail::Same(pm.colourRegion,lastMetadata_.colourRegion) || !Detail::Same(pm.depthRegion,lastMetadata_.depthRegion) ||
            !Detail::Same(pm.motionRegion,lastMetadata_.motionRegion) || pm.motionToUvX != lastMetadata_.motionToUvX ||
            pm.motionToUvY != lastMetadata_.motionToUvY || pm.motionJitter != lastMetadata_.motionJitter ||
            pm.addressSemantics != lastMetadata_.addressSemantics || pm.motionUnits != lastMetadata_.motionUnits ||
            pm.depthConvention != lastMetadata_.depthConvention);
        if (lastSource_ && (replay || mappingChanged ||
            domain_ != m.domain || feature_ != m.feature || configuration_ != m.configuration)) {
            const auto why = domain_ != m.domain ? Rejection::DomainChanged : feature_ != m.feature ? Rejection::FeatureChanged :
                configuration_ != m.configuration ? Rejection::ConfigurationChanged :
                mappingChanged ? Rejection::MappingChanged : Rejection::SourceGap;
            Reset(); result.valid = false; result.rejection = why; return result;
        }
        lastSource_ = m.source; lastEpoch_ = m.epoch; domain_ = m.domain;
        feature_ = m.feature; configuration_ = m.configuration;
        lastCapture_ = m.capturedAtMs; lastMetadata_ = m; lastWidth_ = current.width; lastHeight_ = current.height;
        if (!result.valid) return result;
        if (stage == Stage::Still || stage == Stage::Character) {
            result.rejection = Rejection::ManualStage; return result;
        }
        auto& totals = stage == Stage::Horizontal ? horizontal_ : vertical_;
        auto& count = stage == Stage::Horizontal ? horizontalPairs_ : verticalPairs_;
        if (count >= options_.maxPairsPerAxis) { result.rejection = Rejection::SessionFull; return result; }
        // Use the same pairs for all eight candidates. Invalid candidates get a
        // bounded worst loss rather than silently removing difficult evidence.
        bool axisEvidence = false;
        for (const auto& c : result.candidates) if (c.valid) {
            const auto& v = c.metrics;
            const double main = stage == Stage::Horizontal ? v.motionX : v.motionY;
            const double cross = stage == Stage::Horizontal ? v.motionY : v.motionX;
            if (main >= options_.minMotionPixels && main >= options_.axisDominance*cross) axisEvidence = true;
        }
        if (!axisEvidence) { result.rejection = Rejection::StageMotionMismatch; return result; }
        ++count;
        for (unsigned i = 0; i < CandidateCount; ++i) Accumulate(totals[i],result.candidates[i]);
        return result;
    }
    Recommendation Result() const noexcept {
        Recommendation r; r.horizontalPairs = horizontalPairs_; r.verticalPairs = verticalPairs_;
        r.horizontal = horizontal_; r.vertical = vertical_;
        if (!Detail::GoodOptions(options_)) { r.rejection = Rejection::BadOptions; return r; }
        if (horizontalPairs_ < options_.minPairsPerAxis || verticalPairs_ < options_.minPairsPerAxis) return r;
        std::array<double,CandidateCount> h{},v{},all{};
        for (unsigned i = 0; i < CandidateCount; ++i) {
            h[i] = horizontal_[i].mean.score; v[i] = vertical_[i].mean.score; all[i] = (h[i]+v[i])/2;
        }
        const auto order = Rank(all); const unsigned best = order[0];
        r.flags = Candidate(best); r.relativeGap = RelativeGap(all[best],all[order[1]]);
        // Pure horizontal camera motion may not identify vector-Y sign. Collapse
        // only that bit for horizontal agreement if there is insufficient Y
        // excitation; vertical evidence and the combined 8-way margin still
        // establish it. Never collapse contradictory identifiable sign evidence.
        const bool horizontalSignUnknown = horizontal_[best].mean.motionY < options_.minMotionPixels;
        const auto ha = AxisEvidence(h,best,horizontalSignUnknown ? 4u : 0u);
        const auto va = AxisEvidence(v,best,0);
        r.horizontalGap = ha.gap; r.verticalGap = va.gap;
        const bool bothAgree = ha.agrees && va.agrees;
        if (bothAgree && r.relativeGap >= options_.minRelativeGap &&
            r.horizontalGap >= options_.minRelativeGap && r.verticalGap >= options_.minRelativeGap) {
            r.kind = RecommendationKind::Full; r.depthCertain = true; r.rejection = Rejection::None; return r;
        }
        // Depth can be ambiguous (e.g. a flat wall). Collapse ONLY the depth bit;
        // require strong matching evidence for both motion bits on both axes.
        std::array<double,4> hm{},vm{},am{};
        for (unsigned i = 0; i < 4; ++i) {
            hm[i] = (std::min)(h[2*i],h[2*i+1]); vm[i] = (std::min)(v[2*i],v[2*i+1]);
            am[i] = (hm[i]+vm[i])/2;
        }
        const auto mo = Rank(am); const unsigned mb = mo[0];
        const auto hma = AxisEvidence(hm,mb,horizontalSignUnknown ? 2u : 0u);
        const auto vma = AxisEvidence(vm,mb,0);
        const double mg = RelativeGap(am[mb],am[mo[1]]), hg = hma.gap, vg = vma.gap;
        // MotionOnly is valid only when the full runner differs solely in depth.
        if ((best/2 == order[1]/2) && mb == best/2 && hma.agrees && vma.agrees &&
            mg >= options_.minRelativeGap && hg >= options_.minRelativeGap && vg >= options_.minRelativeGap) {
            r.kind = RecommendationKind::MotionOnly; r.flags = Candidate(2*mb);
            r.depthCertain = false; r.relativeGap = mg; r.horizontalGap = hg; r.verticalGap = vg;
            r.rejection = Rejection::None; return r;
        }
        r.rejection = !bothAgree ? Rejection::AxisDisagreement :
            r.relativeGap <= 1e-9 ? Rejection::Tied : Rejection::RatioBelowThreshold;
        return r;
    }
private:
    template<size_t N> static std::array<unsigned,N> Rank(const std::array<double,N>& a) noexcept {
        std::array<unsigned,N> order{}; for (unsigned i = 0; i < N; ++i) order[i] = i;
        std::sort(order.begin(),order.end(),[&](unsigned x,unsigned y) { return a[x] < a[y]; }); return order;
    }
    struct AxisAssessment { double gap = 0; bool agrees = false; };
    template<size_t N> static AxisAssessment AxisEvidence(const std::array<double,N>& values, unsigned best, unsigned ignoreBit) noexcept {
        const double score = ignoreBit ? (std::min)(values[best],values[best ^ ignoreBit]) : values[best];
        double runner = (std::numeric_limits<double>::max)();
        for (unsigned i = 0; i < N; ++i) if (i != best && (!ignoreBit || i != (best ^ ignoreBit)))
            runner = (std::min)(runner,values[i]);
        return {RelativeGap(score,runner),score <= runner+1e-9};
    }
    static void Accumulate(Aggregate& a, const CandidateResult& c) noexcept {
        const double n = ++a.pairs;
        auto add = [n](double& mean,double value) { mean += (value-mean)/n; };
        add(a.mean.score,c.valid ? c.metrics.score : 1.0);
        add(a.mean.reprojection,c.valid ? c.metrics.reprojection : 1.0);
        add(a.mean.depthConsistency,c.valid ? c.metrics.depthConsistency : 1.0);
        add(a.mean.edgeAlignment,c.valid ? c.metrics.edgeAlignment : 1.0);
        add(a.mean.coverage,c.metrics.coverage); add(a.mean.occlusionFraction,c.metrics.occlusionFraction);
        add(a.mean.outOfBoundsFraction,c.metrics.outOfBoundsFraction);
        add(a.mean.motionX,c.metrics.motionX); add(a.mean.motionY,c.metrics.motionY);
        add(a.mean.depthEdgeEvidence,c.metrics.depthEdgeEvidence);
        a.mean.eligible += c.metrics.eligible; a.mean.compared += c.metrics.compared;
        a.mean.depthSamples += c.metrics.depthSamples; a.mean.motionSamples += c.metrics.motionSamples;
        a.mean.occluded += c.metrics.occluded; a.mean.outOfBounds += c.metrics.outOfBounds;
    }
    Options options_;
    std::array<Aggregate,CandidateCount> horizontal_{},vertical_{};
    unsigned horizontalPairs_ = 0, verticalPairs_ = 0;
    uint64_t lastSource_ = 0, lastEpoch_ = 0, domain_ = 0, feature_ = 0, configuration_ = 0;
    uint64_t lastCapture_ = 0;
    Metadata lastMetadata_;
    unsigned lastWidth_ = 0, lastHeight_ = 0;
    Stage stage_ = Stage::Still;
    Frame previous_;
    bool hasPrevious_ = false;
};
} // namespace GuideCalibration
