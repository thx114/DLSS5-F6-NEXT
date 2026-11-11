// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GuideCapture.h"
#include "GuideCalibrationImages.h"
#include "GuideSourceMetadata.h"
#include <iomanip>
#include <locale>
#include <ostream>
#include <sstream>
#include <type_traits>

// CPU-only serialization/comparison of immutable completed samples. No capture,
// config, wizard, GPU API, queue wait, or retirement decisions occur here.
namespace GuidePairExport {
struct ExportIdentity {
    uint32_t pid=0;
    std::string diskDllSHA, configuration, capturedUtc;
    // Caller-supplied independently verified evidence, NEVER derived from disk.
    bool loadedMemoryKnown=false;
    std::string loadedMemorySHA;
};
inline bool Sha256(const std::string& s) {
    return s.size()==64 && std::all_of(s.begin(),s.end(),[](unsigned char c){
        return (c>='0'&&c<='9')||(c>='A'&&c<='F')||(c>='a'&&c<='f');
    });
}
// nullptr means exportable, not scoreable or GPU-safe. Epoch is the unchanged
// source generation; source (not epoch) advances by one between FG samples.
inline const char* Validate(const std::vector<GuideCapture::Image>& images,const ExportIdentity& id) {
    if (!id.pid || (!id.diskDllSHA.empty()&&!Sha256(id.diskDllSHA)) ||
        (id.loadedMemoryKnown&&!Sha256(id.loadedMemorySHA))) return "invalid process/disk/memory identity";
    if (images.size()!=2) return "exactly two ordered images required";
    for (const auto& im:images) {
        if (!im.valid || !im.completedTick || im.stamp.stage!=GuideDiagnostics::Stage::Completed)
            return "image is not a completed CPU observation";
        if (im.width<3 || im.height<3 || im.width>GuideCalibration::MaxWidth || im.height>GuideCalibration::MaxHeight)
            return "invalid reduced dimensions";
        const size_t n=size_t(im.width)*im.height;
        if (!im.colourPresent||!im.depthPresent||!im.motionPresent||im.colour.size()!=n||
            im.depth.size()!=n||im.motion.size()!=n ||
            (!im.contribution.empty()&&im.contribution.size()!=n) ||
            (im.contributionPresent&&im.contribution.size()!=n)) return "missing/mismatched colour/depth/motion pixels";
        if (!std::isfinite(im.motionToUvX)||!std::isfinite(im.motionToUvY)||!im.motionToUvX||!im.motionToUvY)
            return "unknown/nonfinite/zero motion scale";
        if (!std::isfinite(im.stamp.scaleX)||!std::isfinite(im.stamp.scaleY)||!im.stamp.scaleX||!im.stamp.scaleY)
            return "unknown/nonfinite/zero SDK scale";
        if (!im.stamp.source||!im.stamp.feature||!im.stamp.epoch||!im.stamp.configuration||!im.stamp.owner)
            return "missing source/feature/epoch/configuration/owner identity";
        if (static_cast<unsigned>(im.stream)>=static_cast<unsigned>(GuideCapture::Stream::Count)) return "unknown stream";
        if (!im.stamp.tick || im.completedTick<im.stamp.tick) return "unordered completion stamp";
    }
    const auto& a=images[0];const auto& b=images[1];
    if (a.stream!=b.stream||a.stamp.feature!=b.stamp.feature||a.stamp.epoch!=b.stamp.epoch||
        a.stamp.configuration!=b.stamp.configuration||a.stamp.owner!=b.stamp.owner) return "stream/feature/epoch/configuration/owner changed";
    if (b.stamp.source<=a.stamp.source||b.stamp.source-a.stamp.source!=1) return "sources are not adjacent";
    if (a.width!=b.width||a.height!=b.height) return "reduced dimensions changed";
    if (b.stamp.tick<=a.stamp.tick) return "capture stamps are not ordered";
    return nullptr;
}
namespace Detail {
inline void Value(std::ostream& o,const std::string& s) {
    o.put('\"');
    constexpr char hex[]="0123456789abcdef";
    for (unsigned char c:s) {
        if(c=='\"'||c=='\\') {o.put('\\');o.put(static_cast<char>(c));}
        else if(c<32){o<<"\\u00";o.put(hex[c>>4]);o.put(hex[c&15]);}
        else o.put(static_cast<char>(c));
    }
    o.put('\"');
}
inline void Value(std::ostream& o,const char* s){Value(o,std::string(s));}
inline void Value(std::ostream& o,bool b){o<<(b?"true":"false");}
template<class T,std::enable_if_t<std::is_integral_v<T>,int> =0>
inline void Value(std::ostream& o,T n){o<<n;}
template<class T,std::enable_if_t<std::is_floating_point_v<T>,int> =0>
inline void Value(std::ostream& o,T n){
    // Explicit tokens preserve nonfinite raw samples without invalid JSON or
    // replacing an unobserved pixel with a plausible black/zero observation.
    if(std::isfinite(n))o<<n;else Value(o,std::isnan(n)?"NaN":n>0?"+Infinity":"-Infinity");
}
struct Object {
    std::ostream& o;bool first=true;
    explicit Object(std::ostream& out):o(out){o<<'{';}
    ~Object(){o<<'}';}
    void Key(const char* name){if(!first)o<<',';first=false;Value(o,name);o<<':';}
    template<class T>void Put(const char* key,const T& value){Key(key);Value(o,value);}
};
template<class T,class F>inline void Array(std::ostream& o,const std::vector<T>& values,F write){
    o<<'[';bool first=true;for(const auto& v:values){if(!first)o<<',';first=false;write(o,v);}o<<']';
}
inline void Rect(std::ostream& o,const GuideDiagnostics::Rect& r){
    Object j(o);j.Put("x",r.x);j.Put("y",r.y);j.Put("width",r.width);j.Put("height",r.height);
}
inline void Resource(std::ostream& o,const GuideDiagnostics::Resource& r){
    Object j(o);j.Put("present",r.present);j.Put("format",r.format);j.Put("width",r.width);j.Put("height",r.height);
    j.Key("region");Rect(o,r.region);
}
template<class Fields>inline void FieldsJson(std::ostream& o,const Fields& fields){
    o<<'[';bool first=true;
    for(const auto& f:fields){if(!first)o<<',';first=false;Object j(o);
        j.Put("value",f.value);j.Put("origin",static_cast<unsigned>(f.origin));j.Put("valid",f.valid);}
    o<<']';
}
inline void RawSource(std::ostream& o,const GuideSourceMetadata::Ptr& p){
    if(!p){o<<"null";return;}const auto& s=*p;Object j(o);
#define F6_PAIR_FIELD(name) j.Put(#name,s.name)
    F6_PAIR_FIELD(feature);F6_PAIR_FIELD(context);F6_PAIR_FIELD(generation);F6_PAIR_FIELD(tick);
    F6_PAIR_FIELD(inputDepth);F6_PAIR_FIELD(inputMotion);F6_PAIR_FIELD(translatedDepth);F6_PAIR_FIELD(translatedMotion);
    F6_PAIR_FIELD(evaluateFrame);F6_PAIR_FIELD(flags);F6_PAIR_FIELD(renderWidth);F6_PAIR_FIELD(renderHeight);
    F6_PAIR_FIELD(outputWidth);F6_PAIR_FIELD(outputHeight);F6_PAIR_FIELD(scaleX);F6_PAIR_FIELD(scaleY);
    F6_PAIR_FIELD(jitterX);F6_PAIR_FIELD(jitterY);F6_PAIR_FIELD(flagsKnown);F6_PAIR_FIELD(jitterKnown);
    F6_PAIR_FIELD(lowRes);F6_PAIR_FIELD(jittered);F6_PAIR_FIELD(depthInverted);F6_PAIR_FIELD(rr);
#undef F6_PAIR_FIELD
    j.Key("depth");Resource(o,s.depth);j.Key("motion");Resource(o,s.motion);
    j.Key("provenance");FieldsJson(o,s.provenance.fields);
}
inline void Stamp(std::ostream& o,const GuideDiagnostics::Snapshot& s){
    Object j(o);
#define F6_PAIR_FIELD(name) j.Put(#name,s.name)
    F6_PAIR_FIELD(source);F6_PAIR_FIELD(epoch);F6_PAIR_FIELD(feature);F6_PAIR_FIELD(configuration);
    F6_PAIR_FIELD(picture);F6_PAIR_FIELD(providerFrame);F6_PAIR_FIELD(owner);F6_PAIR_FIELD(submission);
    F6_PAIR_FIELD(historySource);F6_PAIR_FIELD(tick);F6_PAIR_FIELD(rawSdkUvX);F6_PAIR_FIELD(rawSdkUvY);
    F6_PAIR_FIELD(rawMappingKnown);F6_PAIR_FIELD(scaleX);F6_PAIR_FIELD(scaleY);F6_PAIR_FIELD(jitterX);F6_PAIR_FIELD(jitterY);
    F6_PAIR_FIELD(depthFlip);F6_PAIR_FIELD(motionFlip);F6_PAIR_FIELD(yNegated);F6_PAIR_FIELD(depthInverted);
    F6_PAIR_FIELD(opticalFlow);F6_PAIR_FIELD(phaseVerified);F6_PAIR_FIELD(accepted);F6_PAIR_FIELD(modelSuccess);
    F6_PAIR_FIELD(addressPhaseKnown);F6_PAIR_FIELD(motionJitterKnown);F6_PAIR_FIELD(motionJittered);
    F6_PAIR_FIELD(historyAvailable);F6_PAIR_FIELD(fusionRecorded);F6_PAIR_FIELD(contributionKnown);
    F6_PAIR_FIELD(contribution);F6_PAIR_FIELD(coverage);F6_PAIR_FIELD(uiProtected);F6_PAIR_FIELD(sdkEnabled);
    F6_PAIR_FIELD(sdkStateKnown);F6_PAIR_FIELD(generatedIdentityKnown);F6_PAIR_FIELD(sdkQueryObserved);
    F6_PAIR_FIELD(sdkStatusKnown);F6_PAIR_FIELD(sdkOptionsKnown);F6_PAIR_FIELD(sdkOptionsCalled);
    F6_PAIR_FIELD(sdkQueryResult);F6_PAIR_FIELD(sdkOptionsResult);F6_PAIR_FIELD(sdkOptionsMode);
    F6_PAIR_FIELD(sdkStatus);F6_PAIR_FIELD(sdkPresentedInterval);F6_PAIR_FIELD(sdkQueryTick);
    F6_PAIR_FIELD(reason);F6_PAIR_FIELD(colourChoice);F6_PAIR_FIELD(resetReason);
#undef F6_PAIR_FIELD
    j.Put("stage",static_cast<unsigned>(s.stage));
    j.Key("colour");Resource(o,s.colour);j.Key("depth");Resource(o,s.depth);j.Key("motion");Resource(o,s.motion);
    j.Key("rawDepth");Resource(o,s.rawDepth);j.Key("rawMotion");Resource(o,s.rawMotion);
    j.Key("fields");FieldsJson(o,s.fields);j.Key("rawSource");RawSource(o,s.rawSource);
}
inline bool Known(const GuideDiagnostics::Snapshot& s,GuideDiagnostics::Field field){
    const auto& v=s.fields[static_cast<unsigned>(field)];
    return v.valid&&v.origin!=GuideDiagnostics::Origin::Unknown&&std::isfinite(v.value);
}
// Mirrors the existing calibration adapter's normalization and address contract.
// Final-colour phase is an explicit observation, never inferred by a low score.
inline GuideCalibration::Frame Convert(const GuideCapture::Image& im,const GuideCalibration::Frame* previous,uint64_t now){
    using namespace GuideCalibration;using F=GuideDiagnostics::Field;
    Frame f;f.width=im.width;f.height=im.height;auto& m=f.metadata;const auto& s=im.stamp;
    m.source=s.source;m.epoch=s.epoch;m.feature=s.feature;m.configuration=s.configuration;m.domain=s.owner;
    m.capturedAtMs=s.tick;m.observedAtMs=now;
    const auto extent=[&](F field){const auto v=s.fields[static_cast<unsigned>(field)].value;
        return Known(s,field)&&v>0&&v<=UINT32_MAX&&std::floor(v)==v?static_cast<unsigned>(v):0u;};
    m.renderWidth=extent(F::RenderWidth);m.renderHeight=extent(F::RenderHeight);
    const auto region=[](const GuideDiagnostics::Resource& r){return Region{r.width<=UINT32_MAX?static_cast<unsigned>(r.width):0u,
        r.height,{r.region.x,r.region.y,r.region.width,r.region.height}};};
    m.colourRegion=region(s.colour);m.depthRegion=region(s.depth);m.motionRegion=region(s.motion);
    m.motionToUvX=im.motionToUvX;m.motionToUvY=im.motionToUvY;
    m.depthConvention=s.depthInverted?DepthConvention::NearOneFarZero:DepthConvention::NearZeroFarOne;
    m.motionUnits=MotionUnits::RenderPixels;
    m.motionJitter=s.motionJitterKnown?(s.motionJittered?MotionJitter::Included:MotionJitter::Excluded):MotionJitter::Unknown;
    m.addressSemantics=s.addressPhaseKnown?AddressSemantics::CanonicalColourRasterGuides:AddressSemantics::Unknown;
    m.currentJitterKnown=Known(s,F::JitterX)&&Known(s,F::JitterY)&&m.renderWidth&&m.renderHeight;
    if(m.currentJitterKnown)m.jitterUv={s.jitterX/m.renderWidth,s.jitterY/m.renderHeight};
    if(previous){m.previousJitterKnown=previous->metadata.currentJitterKnown;m.previousJitterUv=previous->metadata.jitterUv;
        m.sourceContinuous=m.source>previous->metadata.source&&m.source-previous->metadata.source==1;
        m.epochContinuous=m.epoch>previous->metadata.epoch&&m.epoch-previous->metadata.epoch==1;}
    m.known=im.valid; // Numeric provenance and final colour phase are separate.
    for(auto field:{F::CreateFlags,F::ScaleX,F::ScaleY,F::DepthX,F::DepthY,F::MotionX,F::MotionY,F::DepthBounds,F::MotionBounds})
        m.known=m.known&&Known(s,field);
    m.known=m.known&&s.fields[static_cast<unsigned>(F::DepthBounds)].value==1&&
        s.fields[static_cast<unsigned>(F::MotionBounds)].value==1&&m.renderWidth==s.depth.region.width&&
        m.renderHeight==s.depth.region.height&&m.renderWidth==s.motion.region.width&&m.renderHeight==s.motion.region.height;
    m.rawSceneBeforeNr=im.stream==GuideCapture::Stream::Source;m.rawGuides=m.rawSceneBeforeNr&&!s.opticalFlow;
    f.colour.reserve(im.colour.size());f.colourMask.reserve(im.colour.size());
    const auto tone=[](float v){return std::isfinite(v)&&v>=0?v/(1+v):0.f;};
    for(const auto& p:im.colour){f.colour.push_back({tone(p.r),tone(p.g),tone(p.b)});
        f.colourMask.push_back(std::isfinite(p.r)&&std::isfinite(p.g)&&std::isfinite(p.b)&&p.r>=0&&p.g>=0&&p.b>=0?1:0);}
    f.depth=im.depth;f.motion.reserve(im.motion.size());for(const auto v:im.motion)f.motion.push_back({v.x,v.y});
    return f;
}
struct Comparison {
    GuideCalibration::PairResult pair;
    bool numericAvailable=false,phaseKnown=false;
    std::string mode="rejected",reason="insufficient mapping metadata",assumptions;
};
// Offline admission uses a SAME generation, not calibration's per-source epoch.
// No live-age rejection: immutable saved observations remain analyzable later.
// Unknown address phase may be explored under a named coordinate hypothesis;
// it NEVER changes the exported stamp or turns phaseVerified/valid on.
inline const char* Mapping(const GuideCalibration::Frame& f){
    using namespace GuideCalibration;const auto& m=f.metadata;
    if(!m.known||!m.renderWidth||!m.renderHeight||!GuideCalibration::Detail::Good(m.colourRegion)||
        !GuideCalibration::Detail::Good(m.depthRegion)||!GuideCalibration::Detail::Good(m.motionRegion))
        return "missing numeric metadata or invalid texture regions";
    if(!m.currentJitterKnown||!GuideCalibration::Detail::Good(m.jitterUv)||
        std::abs(m.jitterUv.x)>2.0/m.renderWidth||std::abs(m.jitterUv.y)>2.0/m.renderHeight)
        return "unknown or invalid jitter";
    if(m.motionJitter!=MotionJitter::Excluded)return "jittered/unknown motion contract unsupported by raster-guide comparison";
    if(!std::isfinite(m.motionToUvX)||!std::isfinite(m.motionToUvY)||!m.motionToUvX||!m.motionToUvY)
        return "invalid effective motion scale";
    return nullptr;
}
inline Comparison Compare(const std::vector<GuideCapture::Image>& images,const GuideCalibration::Frame& p,
                          const GuideCalibration::Frame& c){
    using namespace GuideCalibration;Comparison result;auto& pair=result.pair;
    pair.source=c.metadata.source;pair.epoch=c.metadata.epoch;
    for(unsigned i=0;i<CandidateCount;++i){pair.candidates[i].flags=Candidate(i);pair.candidates[i].rejection=Rejection::BadMetadata;}
    const auto refuse=[&](const char* reason){result.reason=reason;return result;};
    for(const auto& im:images)if((im.stream!=GuideCapture::Stream::Source&&im.stream!=GuideCapture::Stream::Fg)||im.stamp.opticalFlow)
        return refuse("only Source/FG scene-guide pairs supported; picture-flow is not scene motion");
    if(const auto why=Mapping(p))return refuse(why);
    if(const auto why=Mapping(c))return refuse(why);
    const auto& a=p.metadata;const auto& b=c.metadata;
    if(a.epoch!=b.epoch||a.feature!=b.feature||a.configuration!=b.configuration||a.domain!=b.domain||
        b.source<=a.source||b.source-a.source!=1)return refuse("source generation/owner changed or nonadjacent sources");
    if(b.capturedAtMs<=a.capturedAtMs||b.capturedAtMs-a.capturedAtMs>Options{}.maxPairIntervalMs)
        return refuse("unordered or excessive pair interval");
    if(p.width!=c.width||p.height!=c.height||a.renderWidth!=b.renderWidth||a.renderHeight!=b.renderHeight||
        !GuideCalibration::Detail::Same(a.colourRegion,b.colourRegion)||
        !GuideCalibration::Detail::Same(a.depthRegion,b.depthRegion)||!GuideCalibration::Detail::Same(a.motionRegion,b.motionRegion)||
        a.depthConvention!=b.depthConvention||a.motionUnits!=b.motionUnits||a.motionJitter!=b.motionJitter||
        a.addressSemantics!=b.addressSemantics||a.motionToUvX!=b.motionToUvX||a.motionToUvY!=b.motionToUvY)
        return refuse("region/scale/coordinate contract changed");
    result.phaseKnown=images[0].stamp.phaseVerified&&images[1].stamp.phaseVerified&&
        images[0].stamp.addressPhaseKnown&&images[1].stamp.addressPhaseKnown;
    result.mode=result.phaseKnown?"phase-verified observation":"exploratory";
    result.assumptions=result.phaseKnown?"observed canonical colour/raster-guide address contract":
        "HYPOTHESIS ONLY: supplied colour is canonical and matches these scene guides; raw guide addresses use canonical UV plus observed current jitter; final colour phase is unverified";
    // These local coordinate hypotheses do not rewrite actual metadata. The
    // published analysisMetadata below continues to contain Unknown if unknown.
    auto hp=p,hc=c;hp.metadata.addressSemantics=hc.metadata.addressSemantics=AddressSemantics::CanonicalColourRasterGuides;
    const Options options;std::vector<uint8_t> common(size_t(c.width)*c.height,1);
    for(unsigned y=0;y<c.height;++y)for(unsigned x=0;x<c.width;++x)
        for(unsigned i=0;i<CandidateCount;++i)if(!GuideCalibration::Detail::Observe(hp,hc,x,y,Candidate(i),options).compared){
            common[size_t(y)*c.width+x]=0;break;}
    double bestResidual=(std::numeric_limits<double>::max)();const CandidateResult* best=nullptr;double maxCoverage=0;
    for(unsigned i=0;i<CandidateCount;++i){
        auto& r=pair.candidates[i]=GuideCalibration::Detail::ScoreCandidate(hp,hc,Candidate(i),options,&common);
        result.numericAvailable|=r.metrics.compared>0;
        if(r.metrics.compared>=options.minSamples&&r.metrics.coverage>=options.minCoverage)
            bestResidual=(std::min)(bestResidual,r.metrics.reprojection);
        if(r.valid){pair.valid=true;maxCoverage=(std::max)(maxCoverage,r.metrics.coverage);if(!best||r.metrics.score<best->metrics.score)best=&r;}
    }
    pair.rejection=pair.valid?Rejection::None:Rejection::InsufficientCoverage;
    if(bestResidual!=(std::numeric_limits<double>::max)()&&bestResidual>options.sceneCutError){
        pair.valid=false;pair.rejection=Rejection::SceneCut;
    }else if(best&&maxCoverage-best->metrics.coverage>.15){
        pair.valid=false;pair.rejection=Rejection::UnequalCandidateCoverage;
    }
    if(!result.phaseKnown){pair.valid=false;pair.rejection=Rejection::BadMetadata;}
    if(!pair.valid)for(auto& r:pair.candidates){r.valid=false;if(result.phaseKnown||r.rejection==Rejection::None)r.rejection=pair.rejection;}
    result.reason=!result.phaseKnown?"unverified colour/address phase; errors are exploratory hypotheses, never direction evidence":
        GuideCalibration::Reason(pair.rejection);
    return result;
}
// Same Observe pixel decomposition as GuideCalibrationImages::MakeImages,
// with the explicit offline generation/phase admission above.
inline GuideCalibration::Images Views(const GuideCalibration::Frame& p,const GuideCalibration::Frame& c,
                                     GuideCalibration::CandidateFlags flags,bool numeric){
    using namespace GuideCalibration;Images images;images.flags=flags;if(!numeric)return images;
    auto hp=p,hc=c;hp.metadata.addressSemantics=hc.metadata.addressSemantics=AddressSemantics::CanonicalColourRasterGuides;
    for(auto* image:{&images.warped,&images.residual,&images.depthContribution}){
        image->width=c.width;image->height=c.height;image->pixels.resize(size_t(c.width)*c.height);image->mask.resize(image->pixels.size());}
    for(unsigned y=0;y<c.height;++y)for(unsigned x=0;x<c.width;++x){
        const auto v=GuideCalibration::Detail::Observe(hp,hc,x,y,flags,Options{});if(!v.compared)continue;
        const auto i=size_t(y)*c.width+x;const auto e=static_cast<float>(v.residual);
        images.warped.pixels[i]=v.warped;images.residual.pixels[i]={e,e,e};
        images.depthContribution.pixels[i]={static_cast<float>(v.depthError),static_cast<float>(v.edgeError),v.occluded?1.f:0.f};
        images.warped.mask[i]=images.residual.mask[i]=images.depthContribution.mask[i]=1;
    }return images;
}
inline void Pixel(std::ostream& o,GuideCalibration::Pixel p){o<<'[';Value(o,p.r);o<<',';Value(o,p.g);o<<',';Value(o,p.b);o<<']';}
inline void Image(std::ostream& o,const GuideCalibration::Image& image){
    Object j(o);j.Put("width",image.width);j.Put("height",image.height);j.Key("pixels");Array(o,image.pixels,Pixel);
    j.Key("mask");Array(o,image.mask,[](auto& out,uint8_t v){Value(out,static_cast<unsigned>(v));});
}
inline void AnalysisMetadata(std::ostream& o,const GuideCalibration::Frame& f){
    const auto& s=f.metadata;Object j(o);
#define F6_PAIR_FIELD(name) j.Put(#name,s.name)
    F6_PAIR_FIELD(source);F6_PAIR_FIELD(epoch);F6_PAIR_FIELD(feature);F6_PAIR_FIELD(configuration);F6_PAIR_FIELD(domain);
    F6_PAIR_FIELD(capturedAtMs);F6_PAIR_FIELD(observedAtMs);F6_PAIR_FIELD(renderWidth);F6_PAIR_FIELD(renderHeight);
    F6_PAIR_FIELD(motionToUvX);F6_PAIR_FIELD(motionToUvY);F6_PAIR_FIELD(known);F6_PAIR_FIELD(rawSceneBeforeNr);
    F6_PAIR_FIELD(rawGuides);F6_PAIR_FIELD(currentJitterKnown);F6_PAIR_FIELD(previousJitterKnown);
    F6_PAIR_FIELD(sourceContinuous);F6_PAIR_FIELD(epochContinuous);
#undef F6_PAIR_FIELD
    j.Put("depthConvention",static_cast<unsigned>(s.depthConvention));j.Put("motionUnits",static_cast<unsigned>(s.motionUnits));
    j.Put("motionJitter",static_cast<unsigned>(s.motionJitter));j.Put("addressSemantics",static_cast<unsigned>(s.addressSemantics));
    j.Key("jitterUv");o<<'[';Value(o,s.jitterUv.x);o<<',';Value(o,s.jitterUv.y);o<<']';
    j.Key("previousJitterUv");o<<'[';Value(o,s.previousJitterUv.x);o<<',';Value(o,s.previousJitterUv.y);o<<']';
}
inline void Captured(std::ostream& o,const GuideCapture::Image& im,const GuideCalibration::Frame& analysis){
    Object j(o);j.Put("width",im.width);j.Put("height",im.height);j.Put("stream",static_cast<unsigned>(im.stream));
    j.Put("completedTick",im.completedTick);j.Put("valid",im.valid);
    j.Put("colourPresent",im.colourPresent);j.Put("depthPresent",im.depthPresent);j.Put("motionPresent",im.motionPresent);
    j.Put("contributionPresent",im.contributionPresent);j.Put("motionToUvX",im.motionToUvX);j.Put("motionToUvY",im.motionToUvY);
    j.Key("stamp");Stamp(o,im.stamp);j.Key("analysisMetadata");AnalysisMetadata(o,analysis);
    j.Key("colour");Array(o,im.colour,[](auto& out,const auto& p){out<<'[';Value(out,p.r);out<<',';Value(out,p.g);out<<',';Value(out,p.b);out<<',';Value(out,p.a);out<<']';});
    j.Key("depth");Array(o,im.depth,[](auto& out,float v){Value(out,v);});
    j.Key("motion");Array(o,im.motion,[](auto& out,const auto& v){out<<'[';Value(out,v.x);out<<',';Value(out,v.y);out<<']';});
    j.Key("contribution");Array(o,im.contribution,[](auto& out,float v){Value(out,v);});
}
inline void Metrics(std::ostream& o,const GuideCalibration::Metrics& s){
    Object j(o);
#define F6_PAIR_FIELD(name) j.Put(#name,s.name)
    F6_PAIR_FIELD(score);F6_PAIR_FIELD(reprojection);F6_PAIR_FIELD(depthConsistency);F6_PAIR_FIELD(edgeAlignment);
    F6_PAIR_FIELD(coverage);F6_PAIR_FIELD(occlusionFraction);F6_PAIR_FIELD(outOfBoundsFraction);
    F6_PAIR_FIELD(motionX);F6_PAIR_FIELD(motionY);F6_PAIR_FIELD(depthEdgeEvidence);
    F6_PAIR_FIELD(eligible);F6_PAIR_FIELD(depthSamples);F6_PAIR_FIELD(motionSamples);F6_PAIR_FIELD(compared);
    F6_PAIR_FIELD(occluded);F6_PAIR_FIELD(outOfBounds);
#undef F6_PAIR_FIELD
}
} // namespace Detail
// Exactly one pair per invocation, called by the caller's async CPU export worker.
// false: invalid structural inputs or stream failure; input rejection emits no bytes.
// true: serialized observation, even if comparison.valid=false. No recommendation
// is authorized by a single pair. Preserve unknown phase and all raw identities.
inline bool Write(std::ostream& out,const std::vector<GuideCapture::Image>& images,const ExportIdentity& identity){
    if(Validate(images,identity))return false;
    try {
        const auto now=GuideDiagnostics::Now();
        const auto previous=Detail::Convert(images[0],nullptr,now);
        const auto current=Detail::Convert(images[1],&previous,now);
        const auto comparison=Detail::Compare(images,previous,current);const auto& score=comparison.pair;
        std::ostringstream o;o.imbue(std::locale::classic());o<<std::setprecision(std::numeric_limits<double>::max_digits10);
        {Detail::Object j(o);j.Put("schema","f6.guide-pair.v1");j.Put("observedAtMs",now);
        j.Put("authority","observation only; no configuration or GPU retirement authority");
        j.Put("epochContract","epoch is source generation: equal across adjacent source frames; no renumbering");
        j.Put("normalization","finite nonnegative RGB: v/(1+v); invalid RGB masked; raw arrays unchanged");
        j.Key("identity");{Detail::Object id(o);id.Put("pid",identity.pid);id.Put("diskDllSHA",identity.diskDllSHA);
            id.Put("configuration",identity.configuration);id.Put("capturedUtc",identity.capturedUtc);
            id.Put("loadedMemoryKnown",identity.loadedMemoryKnown);id.Key("loadedMemorySHA");
            if(identity.loadedMemoryKnown)Detail::Value(o,identity.loadedMemorySHA);else o<<"null";}
        j.Key("frames");o<<'[';Detail::Captured(o,images[0],previous);o<<',';Detail::Captured(o,images[1],current);o<<']';
        j.Key("comparison");{Detail::Object c(o);c.Put("algorithm","GuidePairExport::Compare/GuideCalibration::Detail::ScoreCandidate/Observe");
            c.Put("mode",comparison.mode);c.Put("numericAvailable",comparison.numericAvailable);
            c.Put("phaseKnown",comparison.phaseKnown);c.Put("assumptions",comparison.assumptions);
            c.Put("valid",score.valid);c.Put("rejectionCode",static_cast<unsigned>(score.rejection));
            c.Put("reason",comparison.reason);c.Put("autoApply",false);
            c.Put("recommendation","none: one pair is observational only");c.Key("candidates");o<<'[';
            for(unsigned i=0;i<GuideCalibration::CandidateCount;++i){if(i)o<<',';const auto& r=score.candidates[i];
                Detail::Object candidate(o);candidate.Put("index",i);candidate.Put("depthRows",r.flags.depthRows);
                candidate.Put("motionRows",r.flags.motionRows);candidate.Put("vectorY",r.flags.vectorY);
                candidate.Put("valid",r.valid);candidate.Put("reason",GuideCalibration::Reason(r.rejection));
                candidate.Put("rejectionCode",static_cast<unsigned>(r.rejection));candidate.Key("metrics");Detail::Metrics(o,r.metrics);
                candidate.Key("images");const auto views=Detail::Views(previous,current,r.flags,comparison.numericAvailable);
                {Detail::Object v(o);v.Key("warped");Detail::Image(o,views.warped);v.Key("residual");Detail::Image(o,views.residual);
                    v.Key("depthContribution");Detail::Image(o,views.depthContribution);}
            }o<<']';}
        }
        out<<o.str();return bool(out);
    }catch(...){return false;}
}
} // namespace GuidePairExport
