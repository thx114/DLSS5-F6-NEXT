// SPDX-License-Identifier: GPL-3.0-or-later
#include "GuideCalibrationRuntime.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace GuideCalibrationRuntime {
namespace {
using namespace GuideCalibration;
struct Store {
    std::mutex mutex;
    std::atomic<bool> active{false},busy{false};std::atomic<uint64_t> generation{0};
    std::atomic<unsigned> stage{0},candidate{0};
    std::atomic<uint64_t> stageChangedAt{0};
    uint64_t workerGeneration=0;
    Stage previousStage=Stage::Still;
    Session session;std::optional<Frame> previous;
    std::vector<Rect> exclusions;
    Status status;
};
Store& Storage(){static auto* s=new Store;return *s;}
struct Job {std::vector<GuideCapture::Image> images;uint64_t generation=0;HMODULE module=nullptr;};
bool Known(const GuideDiagnostics::Snapshot& s,GuideDiagnostics::Field f){
    const auto& v=s.fields[static_cast<unsigned>(f)];
    return v.valid&&v.origin!=GuideDiagnostics::Origin::Unknown&&std::isfinite(v.value);
}
Frame Convert(const GuideCapture::Image& im,const std::optional<Frame>& previous,const std::vector<Rect>& exclusions){
    Frame f;f.width=im.width;f.height=im.height;const auto& stamp=im.stamp;auto& m=f.metadata;
    m.source=stamp.source;m.epoch=stamp.epoch;m.feature=stamp.feature;m.configuration=stamp.configuration;
    m.domain=stamp.owner?stamp.owner:stamp.feature;m.capturedAtMs=stamp.tick;m.observedAtMs=GuideDiagnostics::Now();
    const auto extent=[&](GuideDiagnostics::Field field){
        const auto value=stamp.fields[static_cast<unsigned>(field)].value;
        return Known(stamp,field)&&value>0&&value<=UINT32_MAX&&std::floor(value)==value?static_cast<unsigned>(value):0u;
    };
    m.renderWidth=extent(GuideDiagnostics::Field::RenderWidth);m.renderHeight=extent(GuideDiagnostics::Field::RenderHeight);
    const auto region=[](const GuideDiagnostics::Resource& r){return Region{static_cast<unsigned>(r.width),r.height,{r.region.x,r.region.y,r.region.width,r.region.height}};};
    m.colourRegion=region(stamp.colour);m.depthRegion=region(stamp.depth);m.motionRegion=region(stamp.motion);
    m.motionToUvX=im.motionToUvX;m.motionToUvY=im.motionToUvY;
    m.depthConvention=stamp.depthInverted?DepthConvention::NearOneFarZero:DepthConvention::NearZeroFarOne;
    m.motionUnits=MotionUnits::RenderPixels; // explicit scaling below, never inferred from dimensions
    m.motionJitter=stamp.motionJitterKnown?(stamp.motionJittered?MotionJitter::Included:MotionJitter::Excluded):MotionJitter::Unknown;
    // Final SR colour is canonical; only depth/MV addresses carry current raster
    // jitter. The source seam declares this contract independently of a score.
    m.addressSemantics=stamp.addressPhaseKnown?AddressSemantics::CanonicalColourRasterGuides:AddressSemantics::Unknown;
    m.currentJitterKnown=Known(stamp,GuideDiagnostics::Field::JitterX)&&Known(stamp,GuideDiagnostics::Field::JitterY)&&m.renderWidth&&m.renderHeight;
    if(m.currentJitterKnown)m.jitterUv={stamp.jitterX/m.renderWidth,stamp.jitterY/m.renderHeight};
    if(previous){m.previousJitterKnown=previous->metadata.currentJitterKnown;m.previousJitterUv=previous->metadata.jitterUv;
        m.sourceContinuous=m.source==previous->metadata.source+1;m.epochContinuous=m.epoch==previous->metadata.epoch+1;}
    m.known=im.valid&&im.colourPresent&&im.depthPresent&&im.motionPresent&&
        Known(stamp,GuideDiagnostics::Field::CreateFlags)&&Known(stamp,GuideDiagnostics::Field::ScaleX)&&
        Known(stamp,GuideDiagnostics::Field::ScaleY)&&Known(stamp,GuideDiagnostics::Field::DepthX)&&
        Known(stamp,GuideDiagnostics::Field::DepthY)&&Known(stamp,GuideDiagnostics::Field::MotionX)&&
        Known(stamp,GuideDiagnostics::Field::MotionY)&&Known(stamp,GuideDiagnostics::Field::DepthBounds)&&
        Known(stamp,GuideDiagnostics::Field::MotionBounds)&&
        stamp.fields[static_cast<unsigned>(GuideDiagnostics::Field::DepthBounds)].value==1&&
        stamp.fields[static_cast<unsigned>(GuideDiagnostics::Field::MotionBounds)].value==1&&
        m.renderWidth==stamp.depth.region.width&&m.renderHeight==stamp.depth.region.height&&
        m.renderWidth==stamp.motion.region.width&&m.renderHeight==stamp.motion.region.height;
    m.rawSceneBeforeNr=im.stream==GuideCapture::Stream::Source;m.rawGuides=m.rawSceneBeforeNr&&!stamp.opticalFlow;
    const auto n=im.colour.size();f.colour.resize(n);f.colourMask.resize(n);f.depth=im.depth;f.motion.resize(im.motion.size());
    for(size_t i=0;i<n;++i){const auto& p=im.colour[i];
        const bool finite=std::isfinite(p.r)&&std::isfinite(p.g)&&std::isfinite(p.b);
        // Fixed, identical bounded normalization in both frames. No automatic
        // per-frame exposure fitting that could hide mismatched scene colours.
        const auto tone=[](float v){return v>=0&&std::isfinite(v)?v/(1+v):0.0f;};
        f.colour[i]={tone(p.r),tone(p.g),tone(p.b)};f.colourMask[i]=finite&&p.r>=0&&p.g>=0&&p.b>=0?1:0;
    }
    for(size_t i=0;i<im.motion.size();++i)f.motion[i]={im.motion[i].x,im.motion[i].y};
    f.exclusions=exclusions;return f;
}
DWORD WINAPI Work(void* data){
    std::unique_ptr<Job> job(static_cast<Job*>(data));auto& s=Storage();
    try{
        std::lock_guard lock(s.mutex);
        if(s.workerGeneration!=job->generation){s.session.Start();s.previous.reset();s.workerGeneration=job->generation;}
        if(s.active.load(std::memory_order_acquire)&&s.generation.load(std::memory_order_acquire)==job->generation){
            std::stable_sort(job->images.begin(),job->images.end(),[](const auto& a,const auto& b){
                return a.stamp.tick!=b.stamp.tick?a.stamp.tick<b.stamp.tick:a.stamp.source<b.stamp.source;
            });
            for(auto& image:job->images){
                if(!s.active.load(std::memory_order_acquire)||s.generation.load(std::memory_order_acquire)!=job->generation)break;
                const auto stage=static_cast<Stage>(s.stage.load(std::memory_order_acquire));
                // Millisecond stamps at the transition are ambiguous; wait for
                // a strictly later capture rather than attributing it to an axis.
                if(image.stamp.tick<=s.stageChangedAt.load(std::memory_order_acquire))continue;
                if(s.previousStage!=stage){s.previous.reset();s.status.images={};s.previousStage=stage;}
                Frame current=Convert(image,s.previous,s.exclusions);const auto now=GuideDiagnostics::Now();
                PairResult pair;
                const bool sameDomain=s.previous && s.previous->metadata.feature==current.metadata.feature &&
                    s.previous->metadata.configuration==current.metadata.configuration && s.previous->metadata.domain==current.metadata.domain;
                const bool adjacent=sameDomain && current.metadata.source==s.previous->metadata.source+1 && current.metadata.epoch==s.previous->metadata.epoch+1;
                // An out-of-order completion must not replace the seed with an
                // older source and later manufacture a replayed adjacent pair.
                if(s.previous&&(current.metadata.capturedAtMs<s.previous->metadata.capturedAtMs||
                    (sameDomain&&(current.metadata.source<=s.previous->metadata.source||current.metadata.epoch<=s.previous->metadata.epoch))))continue;
                const auto invalid=Detail::ValidateFrame(current,now,Options{});
                if((s.previous&&!sameDomain)||invalid!=Rejection::None)s.session.Start();
                s.session.SetStage(stage); // Start resets its stage; set AFTER any reset.
                if(invalid!=Rejection::None){pair.rejection=invalid;pair.source=current.metadata.source;s.status.images={};}
                else if(adjacent){
                    pair=s.session.ObservePair(*s.previous,current,now);
                    if(!s.active.load(std::memory_order_acquire)||s.generation.load(std::memory_order_acquire)!=job->generation)break;
                    s.status.images=MakeImages(*s.previous,current,Candidate(s.candidate.load(std::memory_order_acquire)%8),now);
                }else{pair.rejection=s.previous?Rejection::SourceGap:Rejection::NeedPrevious;pair.source=current.metadata.source;s.status.images={};}
                if(!s.active.load(std::memory_order_acquire)||s.generation.load(std::memory_order_acquire)!=job->generation)break;
                s.status.pair=std::move(pair);s.status.recommendation=s.session.Result();s.status.stage=stage;
                s.status.source=current.metadata.source;s.status.epoch=current.metadata.epoch;s.status.feature=current.metadata.feature;
                s.status.configuration=current.metadata.configuration;s.status.tick=now;s.status.generation=job->generation;
                s.status.reason=Reason(s.status.pair.rejection);
                if(invalid==Rejection::None)s.previous=std::move(current);else s.previous.reset();
            }
        }
    }catch(...){std::lock_guard lock(s.mutex);s.status.reason="Calibration worker failed; no recommendation applied";}
    const auto module=job->module;job.reset();s.busy.store(false,std::memory_order_release);
    if(module)FreeLibraryAndExitThread(module,0);return 0;
}
}
void Start() noexcept {
    auto& s=Storage();s.stageChangedAt.store(GuideDiagnostics::Now());s.stage.store(0);
    s.generation.fetch_add(1,std::memory_order_acq_rel);s.active.store(true,std::memory_order_release);
    GuideCapture::DrainSourcePairs();GuideCapture::Request(true,true);
}
void Stop() noexcept {
    auto& s=Storage();
    // Cancel queued/in-flight batches without waiting for their scoring mutex.
    // A pair already being scored may finish, but cannot publish or score more.
    if(s.active.exchange(false,std::memory_order_acq_rel))s.generation.fetch_add(1,std::memory_order_acq_rel);
    GuideCapture::Request(false);
}
bool Active() noexcept {return Storage().active.load(std::memory_order_acquire);}
void SetStage(Stage stage) noexcept {
    auto& s=Storage();const auto value=static_cast<unsigned>(stage);
    if(s.stage.load(std::memory_order_acquire)==value)return;
    s.stageChangedAt.store(GuideDiagnostics::Now(),std::memory_order_release);
    s.stage.store(value,std::memory_order_release);
}
void SelectCandidate(unsigned candidate) noexcept {Storage().candidate.store(candidate%8,std::memory_order_release);}
void SetExclusions(const std::vector<Rect>& exclusions) noexcept {
    try{auto& s=Storage();std::unique_lock lock(s.mutex,std::try_to_lock);if(!lock.owns_lock())return;
        s.exclusions.assign(exclusions.begin(),exclusions.begin()+(std::min)(exclusions.size(),size_t(32)));
        s.generation.fetch_add(1,std::memory_order_acq_rel);
    }catch(...){}
}
void Tick() noexcept {
    auto& s=Storage();if(!s.active.load(std::memory_order_acquire))return;
    bool expected=false;if(!s.busy.compare_exchange_strong(expected,true,std::memory_order_acq_rel))return;
    try{
        auto frames=GuideCapture::DrainSourcePairs();if(frames.empty()){s.busy.store(false);return;}
        auto job=std::make_unique<Job>();job->images=std::move(frames);job->generation=s.generation.load(std::memory_order_acquire);
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&Work),&job->module)){s.busy.store(false);return;}
        HANDLE thread=CreateThread(nullptr,0,Work,job.get(),0,nullptr);
        if(!thread){FreeLibrary(job->module);s.busy.store(false);return;}job.release();CloseHandle(thread);
    }catch(...){s.busy.store(false,std::memory_order_release);}
}
bool Read(Status& status) noexcept {
    try{auto& s=Storage();std::unique_lock lock(s.mutex,std::try_to_lock);if(!lock.owns_lock())return false;
        status=s.status;status.active=s.active.load();status.busy=s.busy.load();
        if(status.generation!=s.generation.load()){status={};status.active=s.active.load();status.busy=s.busy.load();status.reason="Waiting for new calibration source frames";}
        return true;
    }catch(...){return false;}
}
}
