// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>

// CPU observations only. No resource ownership, graphics calls or completion proof.
namespace FsrHookHudMetrics {
inline constexpr unsigned SampleStride=64;
inline constexpr uint64_t StaleMs=2000;
struct HitRate {
    uint64_t hits=0,misses=0;
    bool Known() const noexcept {return (hits || misses) && hits<=UINT64_MAX-misses;}
    uint64_t Attempts() const noexcept {return Known()?hits+misses:0;}
    double Percent() const noexcept {return Known()?100.0*double(hits)/double(Attempts()):0.0;}
};
struct HostWallEstimate {
    // UI MUST prefix a known value with '~'; this is sampled host wall, not GPU
    // time, frame latency or a measured sum over every hook invocation.
    static constexpr char Prefix='~';
    bool known=false;
    double millisecondsPerSecond=0;
    uint64_t calls=0,samples=0,unconfirmedRoots=0,clockFailures=0;
};
struct Snapshot {
    bool windowKnown=false;
    uint64_t beginMs=0,endMs=0,frames=0,ownerGeneration=0,threadId=0;
    HitRate shaderMetadata{},psBindingCurrent{};
    HostWallEstimate hookHost{};
};
inline Snapshot Fresh(Snapshot s,uint64_t generation,uint64_t now) noexcept {
    if(!s.windowKnown || !generation || s.ownerGeneration!=generation || !s.threadId || !s.frames ||
       s.endMs<=s.beginMs || now<s.endMs || now-s.endMs>StaleMs)return {};
    return s;
}
// One bounded, atomic-payload read; raced publications are unknown, never spun on.
class SnapshotCell final {
    static constexpr unsigned Words=16;
    std::atomic_flag writing_=ATOMIC_FLAG_INIT;
    std::atomic<uint64_t> version_{0};
    std::array<std::atomic<uint64_t>,Words> words_{};
public:
    void Publish(const Snapshot& s) noexcept {
        if(writing_.test_and_set(std::memory_order_acquire))return;
        const std::array<uint64_t,Words> w={s.windowKnown,s.beginMs,s.endMs,s.frames,s.ownerGeneration,s.threadId,
            s.shaderMetadata.hits,s.shaderMetadata.misses,s.psBindingCurrent.hits,s.psBindingCurrent.misses,
            s.hookHost.known,std::bit_cast<uint64_t>(s.hookHost.millisecondsPerSecond),s.hookHost.calls,
            s.hookHost.samples,s.hookHost.unconfirmedRoots,s.hookHost.clockFailures};
        version_.fetch_add(1,std::memory_order_acq_rel);
        for(unsigned i=0;i<Words;++i)words_[i].store(w[i],std::memory_order_relaxed);
        version_.fetch_add(1,std::memory_order_release);writing_.clear(std::memory_order_release);
    }
    Snapshot Read() const noexcept {
        const auto version=version_.load(std::memory_order_acquire);if(!version || (version&1))return {};
        std::array<uint64_t,Words> w{};
        for(unsigned i=0;i<Words;++i)w[i]=words_[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if(version!=version_.load(std::memory_order_acquire))return {};
        Snapshot s;s.windowKnown=w[0]!=0;s.beginMs=w[1];s.endMs=w[2];s.frames=w[3];s.ownerGeneration=w[4];s.threadId=w[5];
        s.shaderMetadata={w[6],w[7]};s.psBindingCurrent={w[8],w[9]};
        s.hookHost={w[10]!=0,std::bit_cast<double>(w[11]),w[12],w[13],w[14],w[15]};return s;
    }
};

// A single capture-owner thread owns this fixed payload. Clock::Tick is called
// only for selected roots and their non-overlapping original-call union.
// Confirmation comes from existing context/device/owner checks, never family
// classification. Unconfirmed roots have their own count and contribute no cost.
template<unsigned Kinds,class Clock> class RootSampler final {
    struct Population {uint64_t calls=0,samples=0,ticks=0,failed=0;};
    std::array<Population,Kinds> populations_{};
    std::array<uint64_t,Kinds> sequences_{};
    uint64_t generation_=0,serial_=0,unconfirmed_=0;
    unsigned depth_=0,originalDepth_=0,kind_=0;
    bool sampled_=false,confirmed_=false,failed_=false,foreign_=false;
    int64_t begin_=0,originalBegin_=0;
    uint64_t originalTicks_=0;
public:
    struct Ticket {uint64_t generation=0,serial=0;bool root=false;};
    struct NativeTicket {uint64_t generation=0,serial=0;bool entered=false;};
    uint64_t Generation() const noexcept {return generation_;}
    void Reset(uint64_t generation) noexcept {
        populations_={};sequences_={};generation_=generation;++serial_;unconfirmed_=0;
        depth_=0;originalDepth_=0;sampled_=false;confirmed_=false;foreign_=false;failed_=false;
    }
    Ticket Enter(unsigned kind,bool collect=true) noexcept {
        if(!generation_)return {};
        const bool root=depth_++==0;
        if(root){
            ++serial_;kind_=kind;confirmed_=false;foreign_=false;failed_=false;originalTicks_=0;originalDepth_=0;
            sampled_=collect && kind<Kinds && ((++sequences_[kind]&(SampleStride-1))==1);
            begin_=sampled_?Clock::Tick():0;
            // Non-collecting/unsupported roots still suppress nested intervals.
            if(!collect || kind>=Kinds)foreign_=true;
        }
        return {generation_,serial_,root};
    }
    void Confirm(bool owned) noexcept {
        // A native callback cannot admit/reject the wrapper that called it.
        // A nested foreign observation outside original invalidates that root.
        if(!depth_ || originalDepth_)return;
        if(owned){if(depth_==1)confirmed_=true;}else foreign_=true;
    }
    NativeTicket BeginOriginal() noexcept {
        if(!depth_)return {};
        if(originalDepth_++==0 && sampled_)originalBegin_=Clock::Tick();
        return {generation_,serial_,true};
    }
    void EndOriginal(NativeTicket t) noexcept {
        if(!t.entered || t.generation!=generation_ || t.serial!=serial_ || !originalDepth_)return;
        if(--originalDepth_ || !sampled_)return;
        const auto end=Clock::Tick();
        if(originalBegin_<=0 || end<originalBegin_)failed_=true;
        else originalTicks_+=uint64_t(end-originalBegin_);
    }
    void Leave(Ticket t) noexcept {
        if(!t.generation || t.generation!=generation_ || t.serial!=serial_ || !depth_)return;
        const auto end=t.root && sampled_?Clock::Tick():0;
        --depth_;if(!t.root)return;
        if(!confirmed_ || foreign_ || kind_>=Kinds){++unconfirmed_;return;}
        auto& p=populations_[kind_];++p.calls;
        if(!sampled_)return;
        if(failed_ || depth_ || originalDepth_ || begin_<=0 || end<begin_ || originalTicks_>uint64_t(end-begin_)){
            ++p.failed;return;
        }
        ++p.samples;p.ticks+=uint64_t(end-begin_)-originalTicks_;
    }
    HostWallEstimate Summarize(int64_t ticksPerSecond,uint64_t durationMs) const noexcept {
        HostWallEstimate out;out.unconfirmedRoots=unconfirmed_;
        bool valid=generation_ && !depth_ && ticksPerSecond>0 && durationMs>0;
        double estimatedTicks=0;
        for(const auto& p:populations_){
            out.calls+=p.calls;out.samples+=p.samples;out.clockFailures+=p.failed;
            // Stratify by hook kind; never use one cheap kind's mean for others.
            if(p.calls && (!p.samples || p.failed))valid=false;
            if(p.samples)estimatedTicks+=double(p.ticks)/double(p.samples)*double(p.calls);
        }
        out.known=valid && out.calls && out.samples && !out.clockFailures;
        if(out.known)out.millisecondsPerSecond=estimatedTicks/double(ticksPerSecond)*1000000.0/double(durationMs);
        return out;
    }
    // Keep sampling phase across adjacent windows, and reject a split root.
    void ClearWindow() noexcept {
        populations_={};unconfirmed_=0;
        if(depth_){Reset(0);}
    }
};
}
