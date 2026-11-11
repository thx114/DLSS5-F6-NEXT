#pragma once

#include "FgPacingLease.h"
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <utility>

namespace framegen
{
// CPU/SDK admission only. Idle never substitutes for GPU queue/fence retirement.
// Start closed. The provider opens only after its actual SDK lifecycle is ready.
class DlssgPacingLifetime final
{
    std::atomic<uint64_t> state_ {0}; // low bit: open, remaining bits: admitted leases
public:
    bool OpenAdmission() noexcept {
        uint64_t expected=0;
        return state_.compare_exchange_strong(expected,1,std::memory_order_acq_rel) || expected==1;
    }
    void CloseAdmission() noexcept {state_.fetch_and(~uint64_t{1},std::memory_order_acq_rel);}
    bool Idle() const noexcept {return (state_.load(std::memory_order_acquire)>>1)==0;}
    bool TryEnter() noexcept {
        auto value=state_.load(std::memory_order_acquire);
        while(value&1) {
            if(state_.compare_exchange_weak(value,value+2,std::memory_order_acq_rel))return true;
        }
        return false;
    }
    void Leave() noexcept {state_.fetch_sub(2,std::memory_order_release);}
};

// A single process-level instance must cover ALL real GetNewFrameToken calls.
// Reservation is established BEFORE the original token is requested. No mutex
// is held across source Present. While reserved, only this source thread's same
// explicit frame/API can reuse the ORIGINAL pointer (without calling the SDK).
// Other requests fail closed rather than block an SDK worker needed by Present.
class DlssgTokenDomain final
{
public:
    using GetToken=decltype(&slGetNewFrameToken);
    using SleepApi=decltype(&slReflexSleep);
    using MarkerApi=decltype(&slPCLSetMarker);
    using Pin=std::shared_ptr<void>(*)(const void*) noexcept;
private:
    std::mutex mutex_;
    uint64_t sequence_=0,reservation_=0;
    unsigned unreservedCalls_=0;
    GetToken api_=nullptr;
    sl::FrameToken* token_=nullptr;
    uint32_t frame_=0;
    std::thread::id thread_;
    inline static thread_local bool inSdk_=false;
    struct SdkScope {SdkScope(){inSdk_=true;}~SdkScope(){inSdk_=false;}};
    struct Admission {
        std::shared_ptr<DlssgPacingLifetime> lifetime;
        explicit Admission(const std::shared_ptr<DlssgPacingLifetime>& owner) {
            if(owner && owner->TryEnter())lifetime=owner;
        }
        ~Admission(){if(lifetime)lifetime->Leave();}
    };
    class Lease final : public FGPacingLease {
        DlssgTokenDomain& domain_;
        std::shared_ptr<Admission> admission_;
        std::array<std::shared_ptr<void>,4> pins_;
        uint64_t reservation_;
        uint32_t frame_;
        SleepApi sleep_;
        std::atomic<bool> finished_{false};
    public:
        Lease(DlssgTokenDomain& domain,std::shared_ptr<Admission> admission,
              std::array<std::shared_ptr<void>,4> pins,uint64_t reservation,uint32_t frame,SleepApi sleep)
            :domain_(domain),admission_(std::move(admission)),pins_(std::move(pins)),
             reservation_(reservation),frame_(frame),sleep_(sleep){}
        ~Lease() override {Release();}
        bool Valid() const noexcept override {return !finished_.load(std::memory_order_acquire);}
        uint64_t Frame() const noexcept override {return frame_;}
        void Release() noexcept {
            if(finished_.exchange(true,std::memory_order_acq_rel))return;
            domain_.Cancel(reservation_);
            admission_.reset();
        }
        sl::Result Sleep() noexcept override {
            if(finished_.exchange(true,std::memory_order_acq_rel))return sl::Result::eErrorInvalidState;
            const auto result=domain_.Sleep(reservation_,frame_,sleep_);
            // Module references remain alive until the actual call has returned.
            domain_.Cancel(reservation_);
            admission_.reset();
            return result;
        }
    };
    void Cancel(uint64_t reservation) noexcept {
        std::lock_guard lock(mutex_);
        if(reservation_==reservation) {reservation_=0;api_=nullptr;token_=nullptr;}
    }
    sl::Result Sleep(uint64_t reservation,uint32_t frame,SleepApi sleep) noexcept {
        if(inSdk_)return sl::Result::eErrorInvalidState;
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock() || reservation_!=reservation || thread_!=std::this_thread::get_id() ||
           !token_ || static_cast<uint32_t>(*token_)!=frame || !sleep)return sl::Result::eErrorInvalidState;
        auto* retainedToken=token_;
        lock.unlock(); // Reservation, not this mutex, excludes token recycling.
        SdkScope call;
        try{return sleep(*retainedToken);}catch(...){return sl::Result::eErrorInvalidState;}
    }
public:
    std::unique_lock<std::mutex> TryChangeApi() noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(lock.owns_lock()&&(reservation_||unreservedCalls_))lock.unlock();
        return lock;
    }
    sl::Result Get(GetToken api,sl::FrameToken*& token,const uint32_t* frame) {
        return GetResolved([=]{return api;},token,frame);
    }
    // Resolve mutable detour targets while holding the SAME mutex that admits
    // API replacement. Establish call ownership before releasing this mutex.
    template<class Resolve> sl::Result GetResolved(Resolve&& resolve,sl::FrameToken*& token,const uint32_t* frame) {
        token=nullptr;
        if(inSdk_)return sl::Result::eErrorInvalidState;
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock())return sl::Result::eErrorInvalidState;
        const auto api=resolve();
        if(!api)return sl::Result::eErrorInvalidState;
        if(reservation_ && api==api_) {
            if(thread_!=std::this_thread::get_id() || api!=api_ || !frame || *frame!=frame_ ||
               !token_ || static_cast<uint32_t>(*token_)!=frame_)return sl::Result::eErrorInvalidState;
            token=token_;return sl::Result::eOk;
        }
        // Actual SDK calls never hold this bookkeeping mutex. Independent API
        // instances can progress while a source is requesting/sleeping on its
        // own token; API replacement cannot race any admitted getter.
        ++unreservedCalls_;
        lock.unlock();
        struct CallDone {DlssgTokenDomain& domain;~CallDone(){
            std::lock_guard held(domain.mutex_);--domain.unreservedCalls_;
        }} done{*this};
        SdkScope call;
        return api(token,frame);
    }
    FGPacingLeasePtr Begin(const std::shared_ptr<DlssgPacingLifetime>& lifetime,
        GetToken api,SleepApi sleep,MarkerApi marker,sl::FrameToken*& token,const uint32_t* frame,Pin pin)
    {
        return BeginResolved(lifetime,[=]{return Apis{api,sleep,marker};},token,frame,pin);
    }
    struct Apis {GetToken get;SleepApi sleep;MarkerApi marker;};
    template<class Resolve> FGPacingLeasePtr BeginResolved(const std::shared_ptr<DlssgPacingLifetime>& lifetime,
        Resolve&& resolve,sl::FrameToken*& token,const uint32_t* frame,Pin pin)
    {
        token=nullptr;
        if(!lifetime || !frame || !pin || inSdk_)return {};
        auto admission=std::make_shared<Admission>(lifetime);
        if(!admission->lifetime)return {};
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock() || reservation_ || unreservedCalls_)return {};
        const auto resolved=resolve();
        const auto api=resolved.get;const auto sleep=resolved.sleep;const auto marker=resolved.marker;
        if(!api || !sleep || !marker)return {};
        auto serial=++sequence_;if(!serial)serial=++sequence_;
        reservation_=serial;api_=api;token_=nullptr;frame_=*frame;thread_=std::this_thread::get_id();
        lock.unlock();
        struct Rollback {DlssgTokenDomain& domain;uint64_t serial;bool active=true;
            ~Rollback(){if(active)domain.Cancel(serial);}
        } rollback{*this,serial};
        std::array<std::shared_ptr<void>,4> pins {pin(reinterpret_cast<const void*>(api)),
            pin(reinterpret_cast<const void*>(sleep)),pin(reinterpret_cast<const void*>(marker)),{}};
        if(!pins[0] || !pins[1] || !pins[2])return {};
        SdkScope call;
        if(api(token,frame)!=sl::Result::eOk || !token) {token=nullptr;return {};}
        // Token storage is SDK-owned, often in another plugin than the exported
        // API. Pin its actual vtable module, not its heap allocation address.
        const auto vtable=*reinterpret_cast<const void* const*>(token);
        pins[3]=pin(vtable);
        if(!pins[3] || static_cast<uint32_t>(*token)!=*frame) {token=nullptr;return {};}
        auto lease=std::make_shared<Lease>(*this,std::move(admission),std::move(pins),serial,*frame,sleep);
        {std::lock_guard held(mutex_);token_=token;}
        rollback.active=false;
        return lease;
    }
};
}
