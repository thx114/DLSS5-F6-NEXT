// SPDX-License-Identifier: GPL-3.0-or-later
// Portable CPU fixtures. Lead owns compilation/execution and runner integration.
#include "../GuideCalibrationImages.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>

using namespace GuideCalibration;
namespace {
unsigned checks = 0;
void Check(bool condition, const char* description) {
    ++checks;
    if (!condition) { std::fprintf(stderr,"GuideCalibration failure: %s\n",description); std::exit(1); }
}
struct Texel { Pixel colour; float depth; };
Texel Scene(int x, int y) {
    const bool foreground = (x+2*y > 41 && x-2*y < 17);
    const float step = foreground ? .18f : 0.f;
    return {{.38f+.19f*std::sin(.37f*x+.13f*y)+step,
             .35f+.17f*std::sin(.16f*x-.41f*y)+step,
             .32f+.16f*std::sin(.39f*x+.31f*y)+step},
            .25f+.0013f*x+.0017f*y+(foreground ? .24f : 0.f)};
}
// Independent numerical interpolation used to build real translated RGB/depth,
// not production scoring helpers or hand-authored candidate scores.
template<class Fetch> Texel Interpolate(double x, double y, Fetch fetch) {
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    const double tx = x-ix, ty = y-iy;
    const double w[4] = {(1-tx)*(1-ty),tx*(1-ty),(1-tx)*ty,tx*ty};
    const int xs[4] = {ix,ix+1,ix,ix+1}, ys[4] = {iy,iy,iy+1,iy+1};
    Texel out{};
    for (unsigned i = 0; i < 4; ++i) {
        if (w[i] <= 1e-10) continue;
        const auto t = fetch(xs[i],ys[i]);
        out.colour.r += static_cast<float>(w[i]*t.colour.r);
        out.colour.g += static_cast<float>(w[i]*t.colour.g);
        out.colour.b += static_cast<float>(w[i]*t.colour.b);
        out.depth += static_cast<float>(w[i]*t.depth);
    }
    return out;
}
size_t RawIndex(const Frame& f, unsigned x, unsigned y, bool flip) {
    return size_t(flip ? f.height-1-y : y)*f.width+x;
}
void PutDepth(Frame& f, unsigned x, unsigned y, CandidateFlags flags, float canonical) {
    f.depth[RawIndex(f,x,y,flags.depthRows)] =
        f.metadata.depthConvention == DepthConvention::NearOneFarZero ? 1-canonical : canonical;
}
Texel At(const Frame& f, int x, int y, CandidateFlags flags) {
    float depth = f.depth[RawIndex(f,unsigned(x),unsigned(y),flags.depthRows)];
    if (f.metadata.depthConvention == DepthConvention::NearOneFarZero) depth = 1-depth;
    return {f.colour[size_t(y)*f.width+size_t(x)],depth};
}
Frame Base(CandidateFlags flags, unsigned w = 47, unsigned h = 35, bool flatDepth = false) {
    Frame f; f.width = w; f.height = h;
    const size_t n = size_t(w)*h;
    f.colour.resize(n); f.depth.resize(n); f.motion.resize(n);
    auto& m = f.metadata;
    m.source = 1; m.epoch = 71; m.feature = 3; m.configuration = 4; m.domain = 5;
    m.capturedAtMs = 1000; m.observedAtMs = 1010;
    m.renderWidth = 1393; m.renderHeight = 991;
    m.colourRegion = {191,137,{7,9,139,101}};
    m.depthRegion = {313,271,{11,13,177,133}};
    m.motionRegion = {173,127,{13,17,141,105}};
    m.motionToUvX = 1.f/m.renderWidth; m.motionToUvY = 1.f/m.renderHeight;
    m.motionUnits = MotionUnits::RenderPixels;
    m.depthConvention = DepthConvention::NearZeroFarOne;
    m.motionJitter = MotionJitter::Excluded;
    m.addressSemantics = AddressSemantics::CanonicalUnjittered;
    m.known = m.rawSceneBeforeNr = m.rawGuides = true;
    m.currentJitterKnown = m.previousJitterKnown = true;
    m.sourceContinuous = m.epochContinuous = true;
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
        const auto t = Scene(int(x),int(y)); f.colour[size_t(y)*w+x] = t.colour;
        PutDepth(f,x,y,flags,flatDepth ? .5f : t.depth);
    }
    return f;
}
Frame Next(const Frame& previous, CandidateFlags flags, Stage axis,
           bool constantMotion = false, bool staticMotion = false, Vec2 currentJitter = {}) {
    Frame current = previous;
    auto& m = current.metadata;
    ++m.source; ++m.epoch; m.capturedAtMs += 16; m.observedAtMs = m.capturedAtMs+10;
    m.previousJitterUv = previous.metadata.jitterUv; m.jitterUv = currentJitter;
    for (unsigned y = 0; y < current.height; ++y) for (unsigned x = 0; x < current.width; ++x) {
        const double row = double(y)-(current.height-1)*.5;
        double dx = axis == Stage::Horizontal ? 2.25+.06*row : .70+.025*row;
        double dy = axis == Stage::Horizontal ? .70+.020*row : 2.40+.06*row;
        if (constantMotion) { dx = axis == Stage::Horizontal ? 2.25 : .7; dy = axis == Stage::Horizontal ? .7 : 2.4; }
        if (staticMotion) dx = dy = 0;
        double addressX = 0, addressY = 0;
        if (m.addressSemantics == AddressSemantics::CanonicalSampleOffsets) {
            addressX = double(m.jitterUv.x-m.previousJitterUv.x)*current.width;
            addressY = double(m.jitterUv.y-m.previousJitterUv.y)*current.height;
        }
        const double px = x+dx+addressX, py = y+dy+addressY;
        const auto sample = Interpolate(px,py,[&](int sx,int sy) {
            if (sx >= 0 && sy >= 0 && sx < int(previous.width) && sy < int(previous.height))
                return At(previous,sx,sy,flags);
            // Newly revealed boundary content; it is outside reprojection overlap.
            // Clamp the scene's coordinates to keep the fixture's declared depth.
            return Scene((std::max)(0,(std::min)(sx,70)),(std::max)(0,(std::min)(sy,55)));
        });
        current.colour[size_t(y)*current.width+x] = sample.colour;
        PutDepth(current,x,y,flags,sample.depth);
        const double encodedX = dx+(m.motionJitter == MotionJitter::Included ? addressX : 0);
        const double encodedY = dy+(m.motionJitter == MotionJitter::Included ? addressY : 0);
        Vec2 raw{static_cast<float>(encodedX/current.width/m.motionToUvX),
                 static_cast<float>(encodedY/current.height/m.motionToUvY)};
        if (flags.vectorY) raw.y = -raw.y;
        current.motion[RawIndex(current,x,y,flags.motionRows)] = raw;
    }
    return current;
}
Frame NextRotation(const Frame& previous, CandidateFlags flags, Stage stage) {
    Frame current = previous;
    ++current.metadata.source; ++current.metadata.epoch;
    current.metadata.capturedAtMs += 16; current.metadata.observedAtMs = current.metadata.capturedAtMs+10;
    current.metadata.previousJitterUv = previous.metadata.jitterUv;
    // Perspective camera turn, with a small oblique component as the guide UI
    // instructs. Yaw has real nonuniform Y motion; pitch has nonuniform X motion.
    const double yaw = stage == Stage::Horizontal ? .09 : .03;
    const double pitch = stage == Stage::Horizontal ? .035 : .09;
    const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    for (unsigned y = 0; y < current.height; ++y) for (unsigned x = 0; x < current.width; ++x) {
        const double rx = ((x+.5)/current.width-.5)/.8, ry = ((y+.5)/current.height-.5)/.8;
        const double pitchY = cp*ry+sp, pitchZ = -sp*ry+cp;
        const double rotatedX = cy*rx+sy*pitchZ, rotatedZ = -sy*rx+cy*pitchZ;
        const double px = (.8*rotatedX/rotatedZ+.5)*current.width-.5;
        const double py = (.8*pitchY/rotatedZ+.5)*current.height-.5;
        const auto sample = Interpolate(px,py,[&](int sx,int syi) {
            if (sx >= 0 && syi >= 0 && sx < int(previous.width) && syi < int(previous.height)) return At(previous,sx,syi,flags);
            return Scene((std::max)(0,(std::min)(sx,70)),(std::max)(0,(std::min)(syi,55)));
        });
        current.colour[size_t(y)*current.width+x] = sample.colour;
        PutDepth(current,x,y,flags,sample.depth);
        Vec2 raw{static_cast<float>((px-x)/current.width/current.metadata.motionToUvX),
                 static_cast<float>((py-y)/current.height/current.metadata.motionToUvY)};
        if (flags.vectorY) raw.y = -raw.y;
        current.motion[RawIndex(current,x,y,flags.motionRows)] = raw;
    }
    return current;
}
unsigned Best(const PairResult& r) {
    unsigned best = CandidateCount;
    for (unsigned i = 0; i < CandidateCount; ++i)
        if (r.candidates[i].valid && (best == CandidateCount || r.candidates[i].metrics.score < r.candidates[best].metrics.score)) best = i;
    return best;
}
Recommendation Collect(unsigned index, const Options& options = {}, bool flat = false,
                       bool constant = false, bool oppositeVertical = false, unsigned pairs = 24) {
    Session s(options); s.Start(); auto f = Base(Candidate(index),47,35,flat);
    auto initial = s.Observe(f); Check(initial.rejection == Rejection::NeedPrevious,"first snapshot cannot vote");
    for (auto axis : {Stage::Horizontal,Stage::Vertical}) {
        s.SetStage(axis);
        for (unsigned n = 0; n < pairs; ++n) {
            auto next = Next(f,Candidate(index),axis,constant);
            // Next reveals new boundary texels from Scene, whose depth varies.
            // Keep the ENTIRE flat-wall fixture flat across every frame rather
            // than weakening the scorer's evidence/ambiguity requirements.
            if(flat)std::fill(next.depth.begin(),next.depth.end(),.5f);
            if (oppositeVertical && axis == Stage::Vertical)
                for (auto& motion : next.motion) motion.y = -motion.y;
            const auto r = s.Observe(next);
            Check(r.valid,"numeric moving pair accepted for scoring");
            Check(r.rejection == Rejection::None,"requested axis has evidence");
            f = std::move(next);
        }
    }
    return s.Result();
}
void Orientations() {
    for (unsigned index = 0; index < CandidateCount; ++index) for (auto axis : {Stage::Horizontal,Stage::Vertical}) {
        auto p = Base(Candidate(index)); auto c = Next(p,Candidate(index),axis);
        const auto result = ScorePair(p,c,c.metadata.observedAtMs);
        Check(result.valid,"translated pair produces evidence");
        Check(result.candidates[index].valid,"correct independent orientation valid");
        Check(Best(result) == index,"all eight raw orientations select their actual numeric transform");
        const auto& metrics = result.candidates[index].metrics;
        Check(metrics.reprojection < 2e-6,"bilinear RGB residual for known translation near zero");
        Check(metrics.depthConsistency < 2e-5,"numeric depth warp matches on correct orientation");
        Check(metrics.coverage > .7 && metrics.coverage < 1,"coverage counts translated boundaries");
        Check(metrics.score < result.candidates[index ^ 4].metrics.score,"wrong Y sign loses on real RGB/depth");
        Check(metrics.score < result.candidates[index ^ 2].metrics.score,"wrong motion rows lose on row-varying translation");
        Check(metrics.score < result.candidates[index ^ 1].metrics.score,"independent wrong depth rows lose");
        Check(metrics.depthEdgeEvidence > 0,"depth edge evidence measured");
        const auto images = MakeImages(p,c,Candidate(index),c.metadata.observedAtMs);
        Check(images.rejection == Rejection::None,"numeric images available");
        Check(images.warped.pixels.size() == size_t(c.width)*c.height,"bounded RGB image dimensions");
        const size_t center = size_t(c.height/2)*c.width+c.width/2;
        Check(images.warped.mask[center] != 0,"center warp observed");
        Check(images.residual.pixels[center].r < 2e-6,"residual image is actual colour error");
        Check(images.depthContribution.pixels[center].r < 2e-5,"depth image is actual depth error");
        Check(images.warped.mask.back() == 0,"out-of-bounds pixels explicitly masked");
    }
    for (unsigned index = 0; index < CandidateCount; ++index) {
        const auto r = Collect(index);
        Check(r.horizontalPairs == 24 && r.verticalPairs == 24,"24 distinct valid pairs per axis");
        Check(r.kind == RecommendationKind::Full,"numeric evidence recommends full orientation");
        Check(r.flags.Index() == index && r.depthCertain,"session agrees with all three raw transforms");
        Check(r.relativeGap >= .15 && r.horizontalGap >= .15 && r.verticalGap >= .15,"combined and each-axis margin enforced");
    }
}
void UnitsSubrectAndJitter() {
    for (auto units : {MotionUnits::CanonicalUv,MotionUnits::RenderPixels,MotionUnits::MotionTexturePixels}) {
        auto p = Base(Candidate(7),53,37);
        p.metadata.motionUnits = units;
        // Effective conversions deliberately use unequal odd subrect dimensions,
        // never the 160x100 maximum, full allocation, or reduced width as a guess.
        p.metadata.motionToUvX = units == MotionUnits::CanonicalUv ? .4f :
            units == MotionUnits::RenderPixels ? 1.f/1393 : 1.f/141;
        p.metadata.motionToUvY = units == MotionUnits::CanonicalUv ? .7f :
            units == MotionUnits::RenderPixels ? 1.f/991 : 1.f/105;
        auto c = Next(p,Candidate(7),Stage::Vertical);
        auto r = ScorePair(p,c,c.metadata.observedAtMs);
        Check(Best(r) == 7,"declared effective units and odd subrect scaling");
        Check(r.candidates[7].metrics.reprojection < 2e-6,"explicit conversion numerically exact");
        p.metadata.motionToUvY *= 2; c.metadata.motionToUvY *= 2;
        const auto wrong = ScorePair(p,c,c.metadata.observedAtMs);
        Check(wrong.candidates[7].metrics.reprojection > .005,"wrong effective scale cannot masquerade as matching pixels");
    }
    for (auto address : {AddressSemantics::CanonicalUnjittered,AddressSemantics::CanonicalSampleOffsets})
        for (auto jitter : {MotionJitter::Excluded,MotionJitter::Included}) {
            if (address == AddressSemantics::CanonicalUnjittered && jitter == MotionJitter::Included) continue;
            auto p = Base(Candidate(5)); p.metadata.addressSemantics = address; p.metadata.motionJitter = jitter;
            p.metadata.jitterUv = {.35f/p.metadata.renderWidth,-.4f/p.metadata.renderHeight};
            auto c = Next(p,Candidate(5),Stage::Vertical,false,false,
                          {-.45f/p.metadata.renderWidth,.3f/p.metadata.renderHeight});
            const auto r = ScorePair(p,c,c.metadata.observedAtMs);
            Check(Best(r) == 5,"declared sample addressing/jitter preserves orientation");
            Check(r.candidates[5].metrics.reprojection < 2e-6,"jitter handled once after vector sign");
            c.metadata.previousJitterUv.y += .00001f;
            Check(ScorePair(p,c,c.metadata.observedAtMs).rejection == Rejection::BadMetadata,"mismatched previous jitter rejected");
        }
    auto p = Base(Candidate(0)); auto c = Next(p,Candidate(0),Stage::Horizontal);
    c.metadata.currentJitterKnown = false;
    Check(ScorePair(p,c,c.metadata.observedAtMs).rejection == Rejection::BadMetadata,"unknown jitter never gets a zero default");
    c = Next(p,Candidate(0),Stage::Horizontal); c.metadata.jitterUv.x = std::numeric_limits<float>::quiet_NaN();
    Check(ScorePair(p,c,c.metadata.observedAtMs).rejection == Rejection::BadMetadata,"NaN jitter rejected");
    c = Next(p,Candidate(0),Stage::Horizontal); c.metadata.addressSemantics = AddressSemantics::Unknown;
    Check(ScorePair(p,c,c.metadata.observedAtMs).rejection == Rejection::BadMetadata,"unknown canonical address semantics rejected");
    // A real near/far inversion has to be declared on both snapshots.
    p = Base(Candidate(3));
    for (auto& d : p.depth) d = 1-d;
    p.metadata.depthConvention = DepthConvention::NearOneFarZero;
    c = Next(p,Candidate(3),Stage::Vertical);
    Check(Best(ScorePair(p,c,c.metadata.observedAtMs)) == 3,"declared near/far inversion retained");
    p.metadata.depthConvention = c.metadata.depthConvention = DepthConvention::Unknown;
    Check(ScorePair(p,c,c.metadata.observedAtMs).rejection == Rejection::BadMetadata,"never guess near/far depth convention");
}
// Analytic continuous fields, independent of Next/production addressing. A
// bilinear colour field and affine depth/vector fields remain exact under the
// CPU bilinear sampler, so phase/sign errors cannot hide behind fixture blur.
Pixel ContinuousColour(double x,double y) {
    return {static_cast<float>(.15+.006*x+.002*y+.00004*x*y),
            static_cast<float>(.20+.001*x+.004*y+.00003*x*y),
            static_cast<float>(.25+.003*x+.001*y+.00002*x*y)};
}
float ContinuousDepth(double x,double y) {return static_cast<float>(.22+.002*x+.0035*y);}
Vec2 ContinuousWarp(double x,double y,Stage stage,unsigned w,unsigned h) {
    const double row=y-(h-1)*.5, column=x-(w-1)*.5;
    return {static_cast<float>((stage==Stage::Horizontal?1.4:.4)+.015*row+.004*column),
            static_cast<float>((stage==Stage::Horizontal?.4:1.6)+.025*row-.003*column)};
}
void FillMixed(Frame& f,CandidateFlags flags,Stage stage,Vec2 jitter,bool current) {
    auto& m=f.metadata;
    m.addressSemantics=AddressSemantics::CanonicalColourRasterGuides;
    m.renderWidth=f.width;m.renderHeight=f.height;m.jitterUv=jitter;
    m.motionToUvX=1.f/137;m.motionToUvY=-1.f/97;
    m.depthRegion={f.width+23,f.height+29,{11,13,f.width,f.height}};
    m.motionRegion={f.width+31,f.height+37,{17,19,f.width,f.height}};
    const double jx=double(jitter.x)*f.width,jy=double(jitter.y)*f.height;
    for(unsigned y=0;y<f.height;++y)for(unsigned x=0;x<f.width;++x){
        const auto scene=ContinuousWarp(x,y,stage,f.width,f.height);
        // Canonical final-SR colour never carries raster sample jitter.
        f.colour[size_t(y)*f.width+x]=ContinuousColour(x+(current?scene.x:0),y+(current?scene.y:0));
        // Raw raster samples at r describe canonical scene position r-j. Encode
        // rows independently only AFTER assigning that physical sample position.
        const double cx=x-jx,cy=y-jy;
        const auto rawScene=ContinuousWarp(cx,cy,stage,f.width,f.height);
        PutDepth(f,x,y,flags,ContinuousDepth(cx+(current?rawScene.x:0),cy+(current?rawScene.y:0)));
        Vec2 raw{rawScene.x/f.width/m.motionToUvX,rawScene.y/f.height/m.motionToUvY};
        if(flags.vectorY)raw.y=-raw.y;
        f.motion[RawIndex(f,x,y,flags.motionRows)]=raw;
    }
}
void MixedRasterPhase() {
    for(unsigned index=0;index<CandidateCount;++index)for(auto axis:{Stage::Horizontal,Stage::Vertical})
        for(unsigned phase=0;phase<2;++phase){
            const auto flags=Candidate(index);auto p=Base(flags,61,45),c=p;
            const Vec2 jp{(phase?.29f:.43f)/p.width,(phase?-.46f:-.37f)/p.height};
            const Vec2 jc{(phase?-.41f:-.31f)/p.width,(phase?.33f:.47f)/p.height};
            FillMixed(p,flags,axis,jp,false);FillMixed(c,flags,axis,jc,true);
            ++c.metadata.source;++c.metadata.epoch;c.metadata.capturedAtMs+=16;c.metadata.observedAtMs+=16;
            c.metadata.previousJitterUv=jp;
            const auto result=ScorePair(p,c,c.metadata.observedAtMs);const auto& good=result.candidates[index];
            Check(result.valid&&good.valid,"mixed canonical/raster pair has valid complete support");
            Check(good.metrics.reprojection<5e-7,"mixed final-SR colour has NO previous/current jitter delta");
            Check(good.metrics.depthConsistency<2e-5,"independent previous/current raw depth phases align");
            for(unsigned y=5;y<c.height-5;y+=9)for(unsigned x=7;x<c.width-7;x+=10){
                const auto expected=ContinuousWarp(x,y,axis,c.width,c.height);Vec2 mv;float depth;
                Check(Detail::Motion(p,c,x,y,flags,mv),"fractional raw motion bilinear support exists");
                Check(std::abs(double(mv.x)*c.width-expected.x)<2e-6&&
                      std::abs(double(mv.y)*c.height-expected.y)<2e-6,"raw phase then rows then scaled vector sign exactly once");
                Check(Detail::Depth(c,x,y,flags.depthRows,depth)&&
                      std::abs(depth-ContinuousDepth(x+expected.x,y+expected.y))<2e-7,"depth rows reflect AFTER canonical jitter address");
            }
            const auto images=MakeImages(p,c,flags,c.metadata.observedAtMs);
            const auto center=size_t(c.height/2)*c.width+c.width/2;
            Check(images.warped.mask[center]&&images.residual.pixels[center].r<5e-7,"phase-correct diagnostic image shares scorer coordinates");
            auto a=p,b=c;a.metadata.addressSemantics=b.metadata.addressSemantics=AddressSemantics::CanonicalUnjittered;
            const auto ignored=ScorePair(a,b,b.metadata.observedAtMs);
            Check(ignored.candidates[index].metrics.depthConsistency>good.metrics.depthConsistency+.001,"ignoring raw raster phase changes real depth error");
            Check(result.candidates[index^4].metrics.reprojection>good.metrics.reprojection+1e-4,"mixed guide Y sign remains independent of phase");
            if(flags.depthRows){
                // Deliberately wrong order: reflect canonical y, THEN add J.
                Detail::BilinearSupport support;const unsigned x=17,y=13;
                Check(Detail::Support(c,x+double(jc.x)*c.width,c.height-1-y+double(jc.y)*c.height,support),"wrong-order negative control has available samples");
                double wrong=0;for(unsigned j=0;j<4;++j)wrong+=support.weight[j]*c.depth[Detail::Index(c,support.x[j],support.y[j])];
                float correct=0;Check(Detail::Depth(c,x,y,true,correct)&&std::abs(wrong-correct)>.001,"reflect-then-jitter fails numeric depth control");
            }
            b=c;b.metadata.addressSemantics=AddressSemantics::Unknown;
            Check(ScorePair(p,b,b.metadata.observedAtMs).rejection==Rejection::BadMetadata,"unknown mixed phase cannot gain authority from score");
            a=p;b=c;a.metadata.motionJitter=b.metadata.motionJitter=MotionJitter::Included;
            Check(ScorePair(a,b,b.metadata.observedAtMs).rejection==Rejection::BadMetadata,"mixed canonical colour does not guess jittered-MV cancellation");
            // Every positive-weight raw tap must be finite and unmasked, including
            // the fractional neighbour after reflection; zero-weight taps need
            // not exist as evidence. Do not renormalize around an invalid guide.
            const unsigned x=17,y=13;Detail::BilinearSupport ds,ms;
            Check(Detail::GuideSupport(c,x,y,flags.depthRows,ds)&&Detail::GuideSupport(c,x,y,flags.motionRows,ms),"mixed mask fixture full support");
            b=c;b.depthMask.assign(b.depth.size(),1);b.depthMask[Detail::Index(b,ds.x[3],ds.y[3])]=0;
            float d=0;Check(!Detail::Depth(b,x,y,flags.depthRows,d),"masked fractional depth tap rejects rather than imputes");
            b=c;b.depth[Detail::Index(b,ds.x[3],ds.y[3])]=std::numeric_limits<float>::quiet_NaN();
            Check(!Detail::Depth(b,x,y,flags.depthRows,d),"nonfinite fractional depth tap rejects");
            b=c;b.motionMask.assign(b.motion.size(),1);b.motionMask[Detail::Index(b,ms.x[3],ms.y[3])]=0;Vec2 mv;
            Check(!Detail::Motion(p,b,x,y,flags,mv),"masked fractional motion tap rejects");
            b=c;b.motion[Detail::Index(b,ms.x[3],ms.y[3])].y=std::numeric_limits<float>::infinity();
            Check(!Detail::Motion(p,b,x,y,flags,mv),"nonfinite fractional motion tap rejects");
        }
}
void SampleOffsetAnalyticSign() {
    // Independently construct Cj(x)=C0(x+j). Current C0 is translated by scene
    // displacement d: equality with previous requires x+d+CURRENT-PREV jitter.
    for(auto jitter:{MotionJitter::Excluded,MotionJitter::Included})for(bool still:{false,true}){
        auto p=Base(Candidate(0),61,45),c=p;
        p.metadata.renderWidth=c.metadata.renderWidth=p.width;
        p.metadata.renderHeight=c.metadata.renderHeight=p.height;
        p.metadata.addressSemantics=c.metadata.addressSemantics=AddressSemantics::CanonicalSampleOffsets;
        p.metadata.motionJitter=c.metadata.motionJitter=jitter;
        p.metadata.jitterUv={.37f/p.width,-.41f/p.height};
        c.metadata.jitterUv={-.31f/p.width,.43f/p.height};c.metadata.previousJitterUv=p.metadata.jitterUv;
        ++c.metadata.source;++c.metadata.epoch;c.metadata.capturedAtMs+=16;c.metadata.observedAtMs+=16;
        const double pxj=double(p.metadata.jitterUv.x)*p.width,pyj=double(p.metadata.jitterUv.y)*p.height;
        const double cxj=double(c.metadata.jitterUv.x)*c.width,cyj=double(c.metadata.jitterUv.y)*c.height;
        const double dx=still?0:1.3,dy=still?0:1.7;
        for(unsigned y=0;y<p.height;++y)for(unsigned x=0;x<p.width;++x){
            const auto i=size_t(y)*p.width+x;
            p.colour[i]=ContinuousColour(x+pxj,y+pyj);p.depth[i]=ContinuousDepth(x+pxj,y+pyj);
            c.colour[i]=ContinuousColour(x+dx+cxj,y+dy+cyj);c.depth[i]=ContinuousDepth(x+dx+cxj,y+dy+cyj);
            c.motion[i]={static_cast<float>((dx+(jitter==MotionJitter::Included?cxj-pxj:0))/c.width/c.metadata.motionToUvX),
                         static_cast<float>((dy+(jitter==MotionJitter::Included?cyj-pyj:0))/c.height/c.metadata.motionToUvY)};
        }
        Vec2 motion;Check(Detail::Motion(p,c,17,13,Candidate(0),motion),"analytic sample-offset motion available");
        Check(std::abs(double(motion.x)*c.width-(dx+cxj-pxj))<2e-6&&
              std::abs(double(motion.y)*c.height-(dy+cyj-pyj))<2e-6,"current-minus-previous jitter follows independent Cj identity");
        const auto r=ScorePair(p,c,c.metadata.observedAtMs);
        Check(r.candidates[0].metrics.reprojection<5e-7&&r.candidates[0].metrics.depthConsistency<2e-5,"analytic jittered colour/depth matches actual pixels");
        if(still)Check(r.candidates[0].rejection==Rejection::StaticMotion&&
            r.candidates[0].metrics.motionX<2e-6&&r.candidates[0].metrics.motionY<2e-6,
            "sample jitter alone gives no scene-axis excitation under the actual MV contract");
        else if(jitter==MotionJitter::Excluded){
            auto a=p,b=c;a.metadata.jitterUv.x=-a.metadata.jitterUv.x;a.metadata.jitterUv.y=-a.metadata.jitterUv.y;
            b.metadata.jitterUv.x=-b.metadata.jitterUv.x;b.metadata.jitterUv.y=-b.metadata.jitterUv.y;
            b.metadata.previousJitterUv=a.metadata.jitterUv;
            Check(ScorePair(a,b,b.metadata.observedAtMs).candidates[0].metrics.reprojection>.001,"opposite jitter sign loses on independently constructed colour");
        }
    }
}
void MissingComparisonPenalty() {
    auto p=Base(Candidate(0)),c=Next(p,Candidate(0),Stage::Vertical);
    std::fill(p.colour.begin(),p.colour.end(),Pixel{});std::fill(c.colour.begin(),c.colour.end(),Pixel{});
    std::fill(p.depth.begin(),p.depth.end(),.5f);std::fill(c.depth.begin(),c.depth.end(),.5f);
    c.colourMask.assign(c.colour.size(),0);const auto mid=p.height/2;
    for(unsigned x=0;x<p.width;++x)p.colour[size_t(mid)*p.width+x]={.15f,.15f,.15f};
    for(unsigned y=0;y<c.height;++y)for(unsigned x=0;x<c.width;++x){
        const auto i=size_t(y)*c.width+x;c.colourMask[i]=y>=3&&y<c.height-3?1:0;
        c.motion[i]={0,static_cast<float>((double(mid)-y)/c.height/c.metadata.motionToUvY)};
    }
    const auto r=ScorePair(p,c,c.metadata.observedAtMs);
    const auto& full=r.candidates[0];const auto& cropped=r.candidates[2];
    Check(!r.valid&&r.rejection==Rejection::UnequalCandidateCoverage,"adversarial crop cannot authorize orientation evidence");
    Check(full.metrics.coverage>.999&&cropped.metrics.coverage>.55&&cropped.metrics.coverage<.65,"wrong-row candidate discards almost half the canonical pixels");
    const auto oldScore=[](const Metrics& m){return .60*m.reprojection+.20*m.depthConsistency+.15*m.edgeAlignment+.05*(1-m.coverage);};
    Check(oldScore(cropped.metrics)<oldScore(full.metrics),"fixture reproduces surviving-pixel-average bias");
    Check(cropped.metrics.coverage+.05<full.metrics.coverage,"large coverage deficit remains explicit despite common-support residual");
    Session session;session.Start();session.SetStage(Stage::Vertical);
    Check(!session.ObservePair(p,c,c.metadata.observedAtMs).valid&&session.Result().kind==RecommendationKind::None,
          "low-error crop supplies no calibration recommendation");
}
void WrongSignsAndImages() {
    auto p = Base(Candidate(0)); auto c = Next(p,Candidate(0),Stage::Vertical);
    // A negative effective scale plus already negated raw data cancels. Applying
    // another candidate sign is a double-correction and must lose on pixel error.
    p.metadata.motionToUvY = -p.metadata.motionToUvY;
    c.metadata.motionToUvY = -c.metadata.motionToUvY;
    for (auto& m : c.motion) m.y = -m.y;
    const auto doubleNegation = ScorePair(p,c,c.metadata.observedAtMs);
    Check(Best(doubleNegation) == 0,"raw/scale double-negation cancels rather than demanding another flip");
    Check(doubleNegation.candidates[4].metrics.reprojection > .01,"a third sign application measurably wrong");
    for (auto& m : c.motion) m.y = -m.y;
    Check(Best(ScorePair(p,c,c.metadata.observedAtMs)) == 4,"negative effective scale participates exactly once");
    const auto wrongImage = MakeImages(p,c,Candidate(0),c.metadata.observedAtMs);
    const auto goodImage = MakeImages(p,c,Candidate(4),c.metadata.observedAtMs);
    const size_t center = size_t(c.height/2)*c.width+c.width/2;
    Check(wrongImage.residual.pixels[center].r > goodImage.residual.pixels[center].r+.005f,"image residual comes from selected wrong transform");
    // A whole-depth temporal shift produces actual depth and occlusion pixels.
    p = Base(Candidate(0)); c = Next(p,Candidate(0),Stage::Horizontal);
    for (auto& d : c.depth) d += .08f;
    const auto depth = MakeImages(p,c,Candidate(0),c.metadata.observedAtMs);
    Check(depth.depthContribution.pixels[center].r > .9f,"depth mismatch visualized numerically");
    Check(depth.depthContribution.pixels[center].b == 1,"nearer previous depth reports occlusion");
}
void Rejections() {
    const auto originalP = Base(Candidate(0)); const auto originalC = Next(originalP,Candidate(0),Stage::Horizontal);
    auto reject = [&](auto change,Rejection expected,const char* label) {
        auto p = originalP, c = originalC; change(p,c);
        const auto r = ScorePair(p,c,originalC.metadata.observedAtMs);
        Check(!r.valid && r.rejection == expected,label);
    };
    reject([](Frame&,Frame& c){c.width = 161;},Rejection::BadDimensions,"oversized grid rejected before indexing");
    reject([](Frame&,Frame& c){c.motion.pop_back();},Rejection::BadArrays,"truncated raw motion rejected");
    reject([](Frame&,Frame& c){c.depth.clear();},Rejection::NoDepth,"missing depth rejected");
    reject([](Frame&,Frame& c){c.metadata.known = false;},Rejection::BadMetadata,"unknown metadata rejected");
    reject([](Frame&,Frame& c){c.metadata.rawSceneBeforeNr = false;},Rejection::BadMetadata,"post-NR colour forbidden");
    reject([](Frame&,Frame& c){c.metadata.rawGuides = false;},Rejection::BadMetadata,"transformed guides cannot pose as raw");
    reject([](Frame&,Frame& c){c.metadata.motionToUvX = 0;},Rejection::BadMetadata,"zero/missing scale rejected");
    reject([](Frame&,Frame& c){c.metadata.motionToUvX = std::numeric_limits<float>::infinity();},Rejection::BadMetadata,"infinite effective scale rejected");
    reject([](Frame&,Frame& c){c.metadata.source += 2;},Rejection::SourceGap,"source serial gap rejected");
    reject([](Frame& p,Frame& c){c.metadata.source = p.metadata.source;},Rejection::SourceGap,"replayed source rejected");
    reject([](Frame&,Frame& c){c.metadata.sourceContinuous = false;},Rejection::SourceGap,"missing source continuity rejected");
    reject([](Frame&,Frame& c){c.metadata.epoch += 2;},Rejection::EpochGap,"epoch gap rejected");
    reject([](Frame&,Frame& c){c.metadata.epochContinuous = false;},Rejection::EpochGap,"missing epoch continuity rejected");
    reject([](Frame&,Frame& c){++c.metadata.domain;},Rejection::DomainChanged,"domain change rejected");
    reject([](Frame&,Frame& c){++c.metadata.feature;},Rejection::FeatureChanged,"feature change rejected");
    reject([](Frame&,Frame& c){++c.metadata.configuration;},Rejection::ConfigurationChanged,"config change rejected");
    reject([](Frame& p,Frame&){p.metadata.capturedAtMs = 1;},Rejection::Stale,"excess pair interval rejected");
    reject([](Frame& p,Frame& c){c.metadata.capturedAtMs = p.metadata.capturedAtMs;},Rejection::Stale,"unordered captures rejected");
    reject([](Frame&,Frame& c){++c.metadata.depthRegion.rect.y;},Rejection::MappingChanged,"changed source subrect rejected");
    reject([](Frame&,Frame& c){c.metadata.motionRegion.rect.x = 173;},Rejection::BadMetadata,"out-of-range source region rejected");
    reject([](Frame&,Frame& c){c.depthMask.assign(c.depth.size(),0);},Rejection::NoDepth,"all masked depth rejected");
    reject([](Frame&,Frame& c){for(auto& d:c.depth)d=std::numeric_limits<float>::quiet_NaN();},Rejection::NoDepth,"nonfinite depth rejected");
    reject([](Frame&,Frame& c){for(auto& m:c.motion)m={100000,100000};},Rejection::OutOfBounds,"unbounded reprojection rejected");
    reject([](Frame& p,Frame& c){for(auto& v:p.colour)v={0,0,0};for(auto& v:c.colour)v={1,1,1};},Rejection::SceneCut,"no candidate explains scene cut");
    reject([](Frame&,Frame& c){c.colourMask.assign(c.colour.size(),0);},Rejection::NoDepth,"no eligible scene pixels rejected");
    auto stale = ScorePair(originalP,originalC,originalC.metadata.capturedAtMs+2000);
    Check(stale.rejection == Rejection::Stale,"readback freshness explicit");
    Options invalid; invalid.minPairsPerAxis = 23;
    Check(ScorePair(originalP,originalC,originalC.metadata.observedAtMs,invalid).rejection == Rejection::BadOptions,"cannot lower 24-pair safety threshold");
    invalid = {}; invalid.minRelativeGap = .14f;
    Check(ScorePair(originalP,originalC,originalC.metadata.observedAtMs,invalid).rejection == Rejection::BadOptions,"cannot lower 15-percent margin");
    auto p = Base(Candidate(0)); auto c = Next(p,Candidate(0),Stage::Still,false,true);
    Check(ScorePair(p,c,c.metadata.observedAtMs).rejection == Rejection::StaticMotion,"static scene provides no orientation evidence");
    auto jitterP = Base(Candidate(0)); jitterP.metadata.addressSemantics = AddressSemantics::CanonicalSampleOffsets;
    jitterP.metadata.jitterUv = {.35f/jitterP.metadata.renderWidth,-.4f/jitterP.metadata.renderHeight};
    auto jitterC = Next(jitterP,Candidate(0),Stage::Still,false,true,
                        {-.45f/jitterP.metadata.renderWidth,.3f/jitterP.metadata.renderHeight});
    Check(ScorePair(jitterP,jitterC,jitterC.metadata.observedAtMs).rejection == Rejection::StaticMotion,"jitter-only image change does not establish camera motion");
    for (auto& colour : p.colour) colour = {0,0,0};
    for (auto& colour : c.colour) colour = {1,1,1};
    Check(ScorePair(p,c,c.metadata.observedAtMs).rejection == Rejection::SceneCut,"static motion does not hide a scene cut");
}
void CoverageAndExclusions() {
    auto p = Base(Candidate(6)); auto c = Next(p,Candidate(6),Stage::Horizontal);
    // Damage a manual character ROI in current raw colour.
    const Rect roi{12,9,11,13};
    for (unsigned y = roi.y; y < roi.y+roi.height; ++y) for (unsigned x = roi.x; x < roi.x+roi.width; ++x)
        c.colour[size_t(y)*c.width+x] = {1,0,1};
    const auto before = ScorePair(p,c,c.metadata.observedAtMs);
    c.exclusions.push_back(roi);
    const auto after = ScorePair(p,c,c.metadata.observedAtMs);
    Check(after.valid,"manual ROI still leaves usable scene");
    Check(after.candidates[6].metrics.reprojection < before.candidates[6].metrics.reprojection*.1,"manual ROI removes actual damaged pixels");
    const auto images = MakeImages(p,c,Candidate(6),c.metadata.observedAtMs);
    Check(images.residual.mask[size_t(roi.y+1)*c.width+roi.x+1] == 0,"manual exclusions respected by visual masks");
    c = Next(p,Candidate(6),Stage::Horizontal);
    c.depthMask.assign(c.depth.size(),0);
    // Sparse valid depth must not turn a tiny island into 100-percent coverage.
    for (unsigned y = 8; y < 13; ++y) for (unsigned x = 8; x < 18; ++x)
        c.depthMask[RawIndex(c,x,y,true)] = 1;
    auto sparse = ScorePair(p,c,c.metadata.observedAtMs);
    Check(!sparse.valid && sparse.rejection == Rejection::InsufficientCoverage,"coverage denominator retains colour lacking depth");
    c = Next(p,Candidate(6),Stage::Horizontal); c.exclusions = {{0,0,c.width,c.height}};
    Check(!ScorePair(p,c,c.metadata.observedAtMs).valid,"whole-image exclusion cannot grant evidence");
    c = Next(p,Candidate(6),Stage::Horizontal); p.exclusions = {{0,0,p.width,p.height}};
    Check(!ScorePair(p,c,c.metadata.observedAtMs).valid,"previous-image ROI applies to bilinear support");
    p = Base(Candidate(6)); c = Next(p,Candidate(6),Stage::Horizontal);
    c.motionMask.assign(c.motion.size(),1);
    for (unsigned y = 0; y < c.height/2; ++y) for (unsigned x = 0; x < c.width; ++x)
        c.motionMask[RawIndex(c,x,y,true)] = 0;
    Check(!ScorePair(p,c,c.metadata.observedAtMs).valid,"raw motion row masks follow candidate transform");
}
void SessionsAndMargins() {
    auto r = Collect(7,{},false,false,false,23);
    Check(r.kind == RecommendationKind::None && r.rejection == Rejection::InsufficientPairs,"23 pairs never enough");
    r = Collect(7,{},false,true);
    Check(r.kind == RecommendationKind::None,"motion-row ties never yield recommendation");
    r = Collect(7,{},true);
    Check(r.kind == RecommendationKind::MotionOnly && !r.depthCertain,"flat depth leaves explicit individual depth uncertainty");
    Check(r.flags.motionRows && r.flags.vectorY,"uncertain depth does not hide proven motion orientation");
    r = Collect(7,{},false,false,true);
    Check(r.kind == RecommendationKind::None && r.rejection == Rejection::AxisDisagreement,"axes must agree on vector sign");
    Options strict; strict.minRelativeGap = 1.f;
    r = Collect(7,strict);
    Check(r.kind == RecommendationKind::None,"numeric fixture below requested gap threshold has no recommendation");
    Check(RelativeGap(.851,1) < .15 && RelativeGap(.849,1) > .15,"relative ratio threshold both sides");
    Check(std::abs(RelativeGap(.85,1)-.15) < 1e-9,"relative margin denominator is runner score");
    Check(RelativeGap(0,0) == 0 && RelativeGap(1,1) == 0,"zero and nonzero ties have no margin");
    Check(RelativeGap(2.4e-8,3.6e-8) == 0,"float sampling noise cannot establish depth orientation");
    Session session; session.Start(); session.SetStage(Stage::Horizontal);
    auto frame = Base(Candidate(0)); session.Observe(frame);
    for (unsigned n = 0; n < 24; ++n) { auto next = Next(frame,Candidate(0),Stage::Horizontal); session.Observe(next); frame = std::move(next); }
    Check(session.Result().horizontalPairs == 24,"horizontal stage collected");
    session.SetStage(Stage::Still);
    auto still = Next(frame,Candidate(0),Stage::Still,false,true); session.Observe(still); frame = std::move(still);
    Check(session.Result().horizontalPairs == 24,"static manual stage preserves prior valid evidence");
    session.SetStage(Stage::Character);
    auto character = Next(frame,Candidate(0),Stage::Horizontal);
    Check(session.Observe(character).rejection == Rejection::ManualStage,"moving Character stage display only");
    Check(session.Result().horizontalPairs == 24 && session.Result().verticalPairs == 0,"manual stage never adds axis votes");
    session.SetStage(Stage::Vertical); frame = std::move(character);
    auto gap = Next(frame,Candidate(0),Stage::Vertical); gap.metadata.source += 4;
    Check(session.Observe(gap).rejection == Rejection::SourceGap,"session rejects source discontinuity");
    Check(session.Result().horizontalPairs == 0,"source gap invalidates prior recommendation evidence");
    session.Start(); session.SetStage(Stage::Vertical); frame = Base(Candidate(0)); session.Observe(frame);
    auto horizontal = Next(frame,Candidate(0),Stage::Horizontal);
    Check(session.Observe(horizontal).rejection == Rejection::StageMotionMismatch,"wrong guided camera axis not counted");
    Check(session.Result().verticalPairs == 0,"axis mismatch contributes no vote");
    Options bounded; bounded.maxPairsPerAxis = 24;
    Session capped(bounded); capped.Start(); capped.SetStage(Stage::Horizontal); frame = Base(Candidate(0)); capped.Observe(frame);
    for (unsigned n = 0; n < 24; ++n) { auto next = Next(frame,Candidate(0),Stage::Horizontal); capped.Observe(next); frame = std::move(next); }
    auto extra = Next(frame,Candidate(0),Stage::Horizontal);
    Check(capped.Observe(extra).rejection == Rejection::SessionFull,"bounded memory evidence cap explicit");
    Check(capped.Result().horizontalPairs == 24,"full stage cannot silently keep accumulating");
    capped.Start(); Check(capped.Result().horizontalPairs == 0 && capped.CurrentStage() == Stage::Still,"explicit new session clears evidence");
    // The worker can receive disjoint adjacent pairs from a small capture ring.
    // No pair may bridge the sampling gap, and no previous CPU image is reused
    // to fabricate the missing source frame. Every image pair below is complete.
    Session disjoint; disjoint.Start(); uint64_t serial = 1;
    for (auto stage : {Stage::Horizontal,Stage::Vertical}) {
        disjoint.SetStage(stage);
        for (unsigned i = 0; i < 24; ++i) {
            auto p = Base(Candidate(7),79,57);
            p.metadata.source = serial; p.metadata.epoch = serial+100;
            p.metadata.capturedAtMs = 1000+serial*16;
            p.metadata.observedAtMs = p.metadata.capturedAtMs+10;
            auto c = NextRotation(p,Candidate(7),stage);
            const auto scored = disjoint.ObservePair(p,c,c.metadata.observedAtMs);
            Check(scored.valid && scored.rejection == Rejection::None,"perspective oblique yaw/pitch pair scored");
            Check(scored.candidates[7].metrics.reprojection < 2e-6,"projected camera RGB warp numerically matches");
            serial += 5;
        }
    }
    const auto rotations = disjoint.Result();
    Check(rotations.horizontalPairs == 24 && rotations.verticalPairs == 24,"sampling gaps between complete pairs do not erase evidence");
    Check(rotations.kind == RecommendationKind::Full && rotations.flags.Index() == 7,"perspective camera turns can recommend full orientation");
    auto brokenP = Base(Candidate(7),79,57); brokenP.metadata.source = serial; brokenP.metadata.epoch = serial+100;
    brokenP.metadata.capturedAtMs = 1000+serial*16;
    auto brokenC = NextRotation(brokenP,Candidate(7),Stage::Vertical); ++brokenC.metadata.source;
    Check(disjoint.ObservePair(brokenP,brokenC,brokenC.metadata.observedAtMs).rejection == Rejection::SourceGap,"independent pair API still rejects a within-pair source gap");
    Check(disjoint.Result().horizontalPairs == 0,"invalid within-pair identity invalidates accumulated evidence");
    // Horizontal image translation has no Y signal but can identify motion rows
    // from row-dependent X. Vertical motion then establishes the sign separately.
    Session pureHorizontal; pureHorizontal.Start(); serial = 1;
    for (auto stage : {Stage::Horizontal,Stage::Vertical}) {
        pureHorizontal.SetStage(stage);
        for (unsigned n = 0; n < 24; ++n) {
            auto p = Base(Candidate(7)); p.metadata.source = serial; p.metadata.epoch = serial+100;
            p.metadata.capturedAtMs = 1000+serial*16;
            auto c = Next(p,Candidate(7),stage);
            if (stage == Stage::Horizontal) {
                // Rebuild the current pixels from a row-varying horizontal warp.
                for (unsigned y = 0; y < c.height; ++y) for (unsigned x = 0; x < c.width; ++x) {
                    auto& mv = c.motion[RawIndex(c,x,y,true)]; mv.y = 0;
                    const double px = x+double(mv.x)*c.metadata.motionToUvX*c.width;
                    const auto t = Interpolate(px,y,[&](int sx,int syi) {
                        return sx >= 0 && sx < int(p.width) ? At(p,sx,syi,Candidate(7)) : Scene((std::min)(sx,70),syi);
                    });
                    c.colour[size_t(y)*c.width+x] = t.colour; PutDepth(c,x,y,Candidate(7),t.depth);
                }
            }
            const auto observation = pureHorizontal.ObservePair(p,c,c.metadata.observedAtMs);
            Check(observation.valid,"pure horizontal/vertical pair numeric");
            serial += 3;
        }
    }
    const auto yawTie = pureHorizontal.Result();
    Check(yawTie.kind == RecommendationKind::Full && yawTie.flags.Index() == 7,"unidentifiable horizontal Y sign can be proven by vertical axis");
}
} // namespace
int main() {
    Orientations(); UnitsSubrectAndJitter(); MixedRasterPhase(); SampleOffsetAnalyticSign(); MissingComparisonPenalty();
    WrongSignsAndImages(); Rejections(); CoverageAndExclusions(); SessionsAndMargins();
    std::printf("GuideCalibration: %u numeric CPU checks passed\n",checks);
    return 0;
}
