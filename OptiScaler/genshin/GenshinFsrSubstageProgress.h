// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <genshin/GenshinFsrUiObservation.h>
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

// Observation only. No callbacks, provider/GPU operations, allocations, logging,
// timeout transitions or admission decisions occur in this store.
namespace GenshinSubstage
{
enum class Stage : unsigned char {
    TargetDraw, Bindings, PrivateInputs, SrAdmission, ContextRegistration,
    ResourceMapping, ParameterCheck, ProviderAttempt, ProviderSuccess,
    QueueExecute, OutputCopyback, ConsumerAcceptance, NativeClaim, NativeOutput,
    NrGuides, NrExecute, FgInputs, FgExecute, Count
};
enum class State : unsigned char { Unknown, Waiting, Running, Succeeded, Failed };
enum class Mode : unsigned char { Unknown, Requested, Disabled };
enum class Resource : unsigned char { Unknown, Color, Depth, Motion, Output, Inputs, Context, Queue, NativeOutput, Nr, Fg };
enum class Domain : unsigned char { Unknown, Bindings, Fsr, Mapping, Parameters, Ngx, Hresult, NativeClaim, Nr, Fg };
enum class Issue : unsigned char { None, Rejected, NonTex2D, Pending, NoProvider, Exception };
constexpr std::size_t Index(Stage s) noexcept { return static_cast<std::size_t>(s); }
constexpr bool IsNr(Stage s) noexcept { return s == Stage::NrGuides || s == Stage::NrExecute; }
constexpr bool IsFg(Stage s) noexcept { return s == Stage::FgInputs || s == Stage::FgExecute; }
struct Key {
    GenshinLegacy::Ui::Session session{};
    std::uint64_t ownerEpoch=0, context=0, serial=0;
    bool Known() const noexcept { return session.Known() && ownerEpoch && context && serial; }
    bool operator==(const Key&) const noexcept = default;
};
struct Entry {
    State state=State::Unknown;
    Resource resource=Resource::Unknown;
    Domain domain=Domain::Unknown;
    Issue issue=Issue::None;
    std::int64_t code=0;
    std::uint64_t started=0, updated=0, finished=0;
};
struct Failure {
    Key key{};
    Stage stage=Stage::TargetDraw;
    Entry event{};
    bool known=false, recovered=false;
};
struct Snapshot {
    Key key{};
    std::uint64_t started=0, revision=0;
    Mode nr=Mode::Unknown, fg=Mode::Unknown;
    std::array<Entry,Index(Stage::Count)> stages{};
    Failure firstFailure{};
    bool known=false;
};
constexpr bool Applicable(const Snapshot& s,Stage stage) noexcept {
    return !(IsNr(stage) && s.nr==Mode::Disabled) && !(IsFg(stage) && s.fg==Mode::Disabled);
}
// R721: fixed CPU-only receipt in the existing progress store. A request has no
// source. Only the render callsites below supply source identities; neither
// temporal proximity nor a global frame counter establishes SameSource.
struct PresetSource {
    Key key{};
    std::uint64_t instance=0, generation=0, frame=0, historyEpoch=0;
    bool Known() const noexcept { return key.Known() && instance && generation && historyEpoch; }
    bool operator==(const PresetSource&) const noexcept = default;
};
inline bool SamePresetSource(const PresetSource& a,const PresetSource& b) noexcept {
    return a.Known() && b.Known() && a==b;
}
struct PresetSrTicket { std::uint64_t request=0; bool afterCreate=false; };
struct PresetSrResult {
    PresetSource source{};
    Snapshot progress{};
    bool observed=false, copyOrdered=false;
    std::uint64_t tick=0;
};
struct PresetReceipt {
    std::uint64_t request=0, revision=0, superseded=0, createEpoch=0;
    std::uint64_t requestTick=0, rebuildTick=0, createTick=0;
    unsigned pass=0, from=0, to=0, renderFrom=0;
    bool rebuilding=false, created=false, createSucceeded=false;
    PresetSource rebuildSource{}, createSource{};
    PresetSrResult firstAfterRequest{}, firstAfterCreate{};
};
class Store final {
    std::mutex mutex_;
    Snapshot value_{};
    PresetReceipt preset_{};
    std::uint64_t nextPreset_=0;
    std::atomic<bool> presetEnabled_{false}, presetDirty_{false};
    std::atomic<std::uint64_t> presetRequest_{0}, presetCreated_{0};
    std::atomic<unsigned> presetSrPending_{0}; // one result per explicit request/create boundary
    void PresetChanged() noexcept { ++preset_.revision; presetDirty_.store(true,std::memory_order_release); }
public:
    // Published with the existing Hook CPU runtime switch at Present; no new config.
    void EnablePresetReceipt(bool enabled) {
        if(presetEnabled_.load(std::memory_order_relaxed)==enabled)return;
        std::lock_guard lock(mutex_);
        presetEnabled_.store(enabled,std::memory_order_release);
        presetRequest_.store(0,std::memory_order_release);
        presetCreated_.store(0,std::memory_order_release);
        presetSrPending_.store(0,std::memory_order_relaxed);
        presetDirty_.store(false,std::memory_order_relaxed);preset_={};
    }
    void RequestPreset(unsigned pass,unsigned from,unsigned to) {
        if(from==to || !presetEnabled_.load(std::memory_order_relaxed))return;
        std::lock_guard lock(mutex_);
        if(!presetEnabled_.load(std::memory_order_relaxed) || nextPreset_==UINT64_MAX)return;
        const auto superseded=preset_.superseded+(presetRequest_.load(std::memory_order_relaxed)?1u:0u);
        preset_={};preset_.request=++nextPreset_;preset_.pass=pass;
        preset_.from=from;preset_.to=to;preset_.superseded=superseded;preset_.requestTick=GetTickCount64();
        presetCreated_.store(0,std::memory_order_release);
        presetSrPending_.store(1,std::memory_order_release);
        presetRequest_.store(preset_.request,std::memory_order_release);PresetChanged();
    }
    void PresetRebuilding(unsigned pass,unsigned from,unsigned to,const PresetSource& source) {
        if(from==to || !presetRequest_.load(std::memory_order_acquire))return;
        std::lock_guard lock(mutex_);
        if(!presetEnabled_.load(std::memory_order_relaxed) || !preset_.request ||
           preset_.pass!=pass || preset_.to!=to || preset_.rebuilding)return;
        preset_.rebuilding=true;preset_.renderFrom=from;preset_.rebuildSource=source;
        preset_.rebuildTick=GetTickCount64();PresetChanged();
    }
    // Capture BEFORE the real create. A request published during the call cannot
    // adopt that call's result, even when its selected value happens to match.
    std::uint64_t PresetCreateRequest(unsigned pass,unsigned preset) {
        if(!presetRequest_.load(std::memory_order_acquire))return 0;
        std::lock_guard lock(mutex_);
        return presetEnabled_.load(std::memory_order_relaxed) && preset_.rebuilding && !preset_.created &&
            preset_.pass==pass && preset_.to==preset?preset_.request:0;
    }
    void PresetCreated(std::uint64_t request,unsigned pass,unsigned preset,std::uint64_t epoch,
                       bool succeeded,const PresetSource& source) {
        if(!request || request!=presetRequest_.load(std::memory_order_acquire))return;
        std::lock_guard lock(mutex_);
        if(!presetEnabled_.load(std::memory_order_relaxed) || request!=preset_.request ||
           preset_.pass!=pass || preset_.to!=preset || preset_.created)return;
        preset_.created=true;preset_.createSucceeded=succeeded;
        preset_.createEpoch=epoch;preset_.createSource=source;preset_.createTick=GetTickCount64();
        presetSrPending_.fetch_or(2,std::memory_order_release);
        presetCreated_.store(request,std::memory_order_release);PresetChanged();
    }
    PresetSrTicket BeginPresetSr() const noexcept {
        if(!presetEnabled_.load(std::memory_order_relaxed) || !presetSrPending_.load(std::memory_order_relaxed))return {};
        const auto request=presetRequest_.load(std::memory_order_acquire);
        return {request,request && presetCreated_.load(std::memory_order_acquire)==request};
    }
    void PresetSrReturned(PresetSrTicket ticket,const PresetSource& source,bool copyOrdered) {
        const unsigned mask=ticket.afterCreate?3u:1u;
        if(!ticket.request || ticket.request!=presetRequest_.load(std::memory_order_acquire) ||
           !(presetSrPending_.load(std::memory_order_relaxed)&mask))return;
        std::lock_guard lock(mutex_);
        if(!presetEnabled_.load(std::memory_order_relaxed) || ticket.request!=preset_.request)return;
        PresetSrResult result;result.observed=true;result.source=source;result.copyOrdered=copyOrdered;result.tick=GetTickCount64();
        // A different current progress snapshot is UNKNOWN, never borrowed.
        if(value_.known && value_.key==source.key)result.progress=value_;
        bool changed=false;
        if(!preset_.firstAfterRequest.observed) {preset_.firstAfterRequest=result;changed=true;}
        if(ticket.afterCreate && preset_.created && !preset_.firstAfterCreate.observed)
            {preset_.firstAfterCreate=result;changed=true;}
        presetSrPending_.fetch_and(~mask,std::memory_order_relaxed);
        if(preset_.firstAfterCreate.observed) {
            presetRequest_.store(0,std::memory_order_release);
            presetCreated_.store(0,std::memory_order_release);
        }
        if(changed)PresetChanged();
    }
    bool PresetRequestPending() const noexcept {
        return presetRequest_.load(std::memory_order_acquire)!=0;
    }
    bool PresetReceiptPending() const noexcept {
        return presetEnabled_.load(std::memory_order_relaxed) && presetDirty_.load(std::memory_order_acquire);
    }
    bool TakePresetReceipt(PresetReceipt& receipt) {
        if(!presetEnabled_.load(std::memory_order_relaxed) || !presetDirty_.load(std::memory_order_acquire))return false;
        // Called only at the existing Present publisher. Never wait on the UI.
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock() || !presetEnabled_.load(std::memory_order_relaxed) || !presetDirty_.load(std::memory_order_relaxed))return false;
        receipt=preset_;presetDirty_.store(false,std::memory_order_release);return receipt.request!=0;
    }
    bool Begin(const Key& key,Mode nr,Mode fg,std::uint64_t now) noexcept {
        if(!key.Known() || !now)return false;
        std::lock_guard lock(mutex_);
        if(value_.known && value_.key==key)return true;
        if(value_.known && value_.key.session==key.session && value_.key.ownerEpoch==key.ownerEpoch &&
           value_.key.context==key.context && key.serial<=value_.key.serial)return false;
        Failure first;
        if(value_.known && value_.key.session==key.session && value_.key.ownerEpoch==key.ownerEpoch &&
           value_.key.context==key.context)first=value_.firstFailure;
        value_={};value_.key=key;value_.known=true;value_.nr=nr;value_.fg=fg;
        value_.started=now;value_.revision=1;value_.firstFailure=first;
        // Only an observed start authorizes WAITING. Unwired NR/FG stay UNKNOWN.
        for(std::size_t i=0;i<Index(Stage::NrGuides);++i)value_.stages[i].state=State::Waiting;
        return true;
    }
    bool Publish(const Key& key,Stage stage,State state,std::uint64_t now,
                 Resource resource=Resource::Unknown,Domain domain=Domain::Unknown,
                 std::int64_t code=0,Issue issue=Issue::None) noexcept {
        if(!key.Known() || Index(stage)>=Index(Stage::Count) || !now)return false;
        std::lock_guard lock(mutex_);
        if(!value_.known || value_.key!=key || now<value_.started || !Applicable(value_,stage) ||
           (IsNr(stage) && value_.nr!=Mode::Requested) || (IsFg(stage) && value_.fg!=Mode::Requested))return false;
        auto& e=value_.stages[Index(stage)];
        if(now<e.updated)return false;
        // Pending/unobserved notifications cannot erase real failure or success.
        if((e.state==State::Failed || e.state==State::Succeeded) &&
           (state==State::Unknown || state==State::Waiting || state==State::Running))return false;
        if(e.state==state && e.resource==resource && e.domain==domain && e.code==code && e.issue==issue)return true;
        if(!e.started)e.started=now;
        e.state=state;e.resource=resource;e.domain=domain;e.code=code;e.issue=issue;e.updated=now;
        e.finished=(state==State::Succeeded || state==State::Failed)?now:0;
        ++value_.revision;
        if(state==State::Failed && !value_.firstFailure.known)
            value_.firstFailure={key,stage,e,true,false};
        if(state==State::Succeeded && value_.firstFailure.known && value_.firstFailure.stage==stage)
            value_.firstFailure.recovered=true;
        return true;
    }
    bool Read(Snapshot& result) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock())return false;
        result=value_;return true;
    }
};
inline Store& Runtime() noexcept { static Store store;return store; }

// Event-only locks are contained here; no observation failure changes routing.
inline void EnablePresetReceipt(bool enabled) noexcept {
    try { Runtime().EnablePresetReceipt(enabled); } catch(...) {}
}
inline void RequestPreset(unsigned pass,unsigned from,unsigned to) noexcept {
    try { Runtime().RequestPreset(pass,from,to); } catch(...) {}
}
inline void PresetRebuilding(unsigned pass,unsigned from,unsigned to,const PresetSource& source) noexcept {
    try { Runtime().PresetRebuilding(pass,from,to,source); } catch(...) {}
}
inline std::uint64_t PresetCreateRequest(unsigned pass,unsigned preset) noexcept {
    try { return Runtime().PresetCreateRequest(pass,preset); } catch(...) { return 0; }
}
inline void PresetCreated(std::uint64_t request,unsigned pass,unsigned preset,std::uint64_t epoch,
                          bool succeeded,const PresetSource& source) noexcept {
    try { Runtime().PresetCreated(request,pass,preset,epoch,succeeded,source); } catch(...) {}
}
inline void PresetSrReturned(PresetSrTicket ticket,const PresetSource& source,bool copyOrdered) noexcept {
    try { Runtime().PresetSrReturned(ticket,source,copyOrdered); } catch(...) {}
}

inline thread_local Key current{};
inline std::atomic<std::uint64_t> nextSerial{1};
inline Key Begin(const GenshinLegacy::Ui::Session& session,std::uint64_t ownerEpoch,std::uint64_t context,
                 Mode nr,Mode fg=Mode::Unknown) noexcept {
    Key key{session,ownerEpoch,context,nextSerial.fetch_add(1,std::memory_order_relaxed)};
    return Runtime().Begin(key,nr,fg,GetTickCount64())?key:Key{};
}
class Scope final {
    Key previous_;
public:
    explicit Scope(const Key& key) noexcept : previous_(current) { current=key; }
    ~Scope() { current=previous_; }
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
};
inline bool Event(const Key& key,Stage stage,State state,Resource resource=Resource::Unknown,
                  Domain domain=Domain::Unknown,std::int64_t code=0,Issue issue=Issue::None) noexcept {
    return Runtime().Publish(key,stage,state,GetTickCount64(),resource,domain,code,issue);
}
inline bool Event(Stage stage,State state,Resource resource=Resource::Unknown,
                  Domain domain=Domain::Unknown,std::int64_t code=0,Issue issue=Issue::None) noexcept {
    return current.Known() && Event(current,stage,state,resource,domain,code,issue);
}
inline void ProviderResult(const Key& key,std::uint32_t result,bool succeeded) noexcept {
    if(!key.Known())return;
    Snapshot s;
    if(!Runtime().Read(s) || s.key!=key)return;
    const auto& p=s.stages[Index(Stage::ParameterCheck)];
    // BAD00005 alone identifies invalid parameters, never invents "Color".
    if(succeeded || result==0xBAD00005u || p.issue==Issue::NonTex2D)
        Event(key,Stage::ParameterCheck,succeeded?State::Succeeded:State::Failed,
              p.resource,Domain::Ngx,result,succeeded?Issue::None:p.issue);
    Event(key,Stage::ProviderSuccess,succeeded?State::Succeeded:State::Failed,
          Resource::Unknown,Domain::Ngx,result,succeeded?Issue::None:Issue::Rejected);
}
// Captures the actual invocation key. Unwinding reports failure without catching
// exceptions or changing provider behavior; a normal NGX error is still a return.
class ActualProviderCall final {
    Key key_;
    bool returned_=false;
public:
    ActualProviderCall() noexcept : key_(current) {
        Event(key_,Stage::ProviderAttempt,State::Running);
        Event(key_,Stage::ProviderSuccess,State::Running);
    }
    void Returned(std::uint32_t result,bool succeeded) noexcept {
        returned_=true;
        Event(key_,Stage::ProviderAttempt,State::Succeeded);
        ProviderResult(key_,result,succeeded);
    }
    ~ActualProviderCall() {
        if(returned_)return;
        Event(key_,Stage::ProviderAttempt,State::Failed,Resource::Unknown,Domain::Ngx,0,Issue::Exception);
        Event(key_,Stage::ProviderSuccess,State::Failed,Resource::Unknown,Domain::Ngx,0,Issue::Exception);
    }
    ActualProviderCall(const ActualProviderCall&)=delete;
    ActualProviderCall& operator=(const ActualProviderCall&)=delete;
};
constexpr std::uint64_t TimeoutMs(Stage stage) noexcept {
    return stage==Stage::ContextRegistration || stage==Stage::ProviderSuccess ||
           stage==Stage::NrExecute || stage==Stage::FgExecute ? 30000 : 15000;
}
struct Progress {
    unsigned succeeded=0,applicable=0,disabled=0;
    Stage current=Stage::TargetDraw;
    State state=State::Unknown;
    bool srReady=false,nrReady=false,fgReady=false,timedOut=false;
    std::uint64_t elapsed=0,total=0;
    float fraction=0;
};
inline Progress Evaluate(const Snapshot& s,std::uint64_t now) noexcept {
    Progress p;
    if(!s.known || !s.key.Known() || now<s.started)return p;
    p.total=now-s.started;
    bool selected=false;
    for(std::size_t i=0;i<s.stages.size();++i) {
        const auto stage=static_cast<Stage>(i);const auto& e=s.stages[i];
        if(!Applicable(s,stage))continue;
        if(e.state==State::Failed || (!selected && e.state!=State::Succeeded)) {
            if(!selected || e.state==State::Failed) {p.current=stage;p.state=e.state;selected=true;}
            if(e.state==State::Failed)break;
        }
    }
    // Count/readiness include all rows, independently of the first failure.
    p.srReady=true;p.nrReady=s.nr==Mode::Requested;p.fgReady=s.fg==Mode::Requested;
    for(std::size_t i=0;i<s.stages.size();++i) {
        const auto stage=static_cast<Stage>(i);const auto success=s.stages[i].state==State::Succeeded;
        if(!Applicable(s,stage)){++p.disabled;continue;}
        ++p.applicable;p.succeeded+=success?1:0;
        if(i<Index(Stage::NrGuides))p.srReady=p.srReady && success;
        if(IsNr(stage))p.nrReady=p.nrReady && success;
        if(IsFg(stage))p.fgReady=p.fgReady && success;
    }
    if(!selected){p.current=Stage::NativeOutput;p.state=State::Succeeded;}
    if(p.applicable)p.fraction=float(p.succeeded)/p.applicable;
    const auto& e=s.stages[Index(p.current)];
    const auto began=e.started?e.started:s.started;
    const auto end=e.finished?e.finished:now;
    p.elapsed=end>=began?end-began:0;
    p.timedOut=(p.state==State::Waiting || p.state==State::Running || p.state==State::Unknown) &&
               now>=began && now-began>=TimeoutMs(p.current);
    return p;
}
}
