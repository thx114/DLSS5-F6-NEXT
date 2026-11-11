// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

// Observation only. No resources, SDK calls, admission or retirement authority.
// The visible window renews a short lease; closed/hidden means no frame copying.
namespace NrHistoryDebug {
inline uint64_t Now() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Frame {
    uint64_t source=0,historySource=0,owner=0,configuration=0,tick=0;
    unsigned width=0,height=0,motionWidth=0,motionHeight=0,depthWidth=0,depthHeight=0;
    float scaleX=0,scaleY=0,delta=0,duration=0,maxAge=0,strength=0;
    float depthTolerance=0,colourTolerance=0,motionTolerance=0,stabilitySeconds=0,stabilityFloor=0,stabilityClamp=0;
    float motionPhaseX=0,motionPhaseY=0,depthPhaseX=0,depthPhaseY=0;
    bool requested=false,depth=false,motion=false,phase=false,admitted=false,history=false;
    bool recorded=false,submitted=false,completed=false,reset=false,quarantined=false;
    const char* reason="not-observed";
};
struct View {
    std::array<Frame,120> frames{};
    uint64_t revision=0;
    unsigned count=0,next=0;
};
struct State {
    std::atomic<uint64_t> until{0};
    std::mutex mutex;
    View view;
};
inline State& Storage() { static State s;return s; }
inline void Request(bool visible) noexcept {Storage().until.store(visible?Now()+1000:0,std::memory_order_release);}
inline bool Wanted() noexcept {const auto until=Storage().until.load(std::memory_order_acquire);return until&&until>Now();}
inline void Observe(const Frame& value) {
    if(!Wanted()||!value.source)return;
    auto& s=Storage();std::lock_guard lock(s.mutex);
    for(auto& old:s.view.frames) if(old.source==value.source&&old.owner==value.owner&&old.configuration==value.configuration) {
        auto copy=value;
        copy.submitted|=old.submitted;copy.completed|=old.completed;copy.reset|=old.reset;copy.quarantined|=old.quarantined;
        old=copy;++s.view.revision;return;
    }
    s.view.frames[s.view.next]=value;s.view.next=(s.view.next+1)%s.view.frames.size();
    if(s.view.count<s.view.frames.size())++s.view.count;
    ++s.view.revision;
}
inline void Lifecycle(uint64_t source,uint64_t owner,uint64_t configuration,
                      bool submitted,bool completed,bool reset,bool quarantined) {
    if(!Wanted())return;
    auto& s=Storage();std::lock_guard lock(s.mutex);
    for(auto& f:s.view.frames)if(f.source==source&&f.owner==owner&&f.configuration==configuration) {
        const bool changed=(submitted&&!f.submitted)||(completed&&!f.completed)||(reset&&!f.reset)||(quarantined&&!f.quarantined);
        f.submitted|=submitted;f.completed|=completed;f.reset|=reset;f.quarantined|=quarantined;
        if(changed)++s.view.revision;
        return;
    }
}
inline bool Read(View& out) {
    auto& s=Storage();std::unique_lock lock(s.mutex,std::try_to_lock);
    if(!lock.owns_lock())return false;
    if(out.revision!=s.view.revision)out=s.view;
    return true;
}
} // namespace NrHistoryDebug
