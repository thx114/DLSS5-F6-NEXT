// Runs the product's actual owner wait with two CPU threads. This exercises
// lock release, notifications and expiry, not GPU completion or HWND behavior.
#include "../../../inputs/FG/GenshinFgTicket.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>

using namespace GenshinBridgeFg;
using namespace std::chrono_literals;
namespace {
unsigned checks=0;
void Check(bool ok,const char* why) {
    ++checks;
    if(!ok){std::fprintf(stderr,"FAILED: %s\n",why);std::abort();}
}
uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void RevealHandoff() {
    std::recursive_timed_mutex mutex;
    OutputRevealWindow window;
    bool realTailComplete=false,shown=false;
    auto now=Now();
    Check(window.Hold(7,now,now+1000,now+1000),"real pending observation arms input yield");
    std::promise<void> entered;
    auto worker=std::async(std::launch::async,[&] {
        std::unique_lock lock(mutex);
        entered.set_value();
        return window.AwaitOwner(lock,1000,[&]{return !window.BlocksRaw(Now());});
    });
    entered.get_future().wait();
    {
        // This lock acquisition proves the worker released the display lock.
        std::lock_guard lock(mutex);
        Check(!realTailComplete && !shown,"CPU waiting has not completed or revealed anything");
        window.OwnerChanged(); // unrelated publication cannot authorize a raw
    }
    Check(worker.wait_for(10ms)==std::future_status::timeout,"spurious owner publication does not end reveal yield");
    {
        std::lock_guard lock(mutex);
        realTailComplete=true;shown=true;window.Release();
    }
    Check(worker.wait_for(1s)==std::future_status::ready && worker.get(),"owner completion resumes the already queued worker without SDK error");
    Check(realTailComplete && shown,"only the owner changed its own state");
}
void HideAcknowledgement() {
    std::recursive_timed_mutex mutex;
    OutputRevealWindow window;
    bool hidden=false,live=true;
    std::promise<void> entered;
    auto worker=std::async(std::launch::async,[&] {
        std::unique_lock lock(mutex);entered.set_value();
        return window.AwaitOwner(lock,1000,[&]{return !live || hidden;});
    });
    entered.get_future().wait();
    {
        std::lock_guard lock(mutex);
        hidden=true;window.OwnerChanged();
    }
    Check(worker.wait_for(1s)==std::future_status::ready && worker.get(),"actual hidden publication wakes source-less raw bootstrap");
    hidden=false;
    std::promise<void> waiting;
    auto revoked=std::async(std::launch::async,[&] {
        std::unique_lock lock(mutex);waiting.set_value();
        return window.AwaitOwner(lock,1000,[&]{return !live || hidden;});
    });
    waiting.get_future().wait();
    {
        std::lock_guard lock(mutex);live=false;window.Release();
    }
    Check(revoked.wait_for(1s)==std::future_status::ready && revoked.get(),"retirement wakes waiters for revalidation without claiming hidden or completion");
    Check(!live && !hidden,"wake does not make a retired endpoint admissible");
}
void OwnerDeadline() {
    std::recursive_timed_mutex mutex;
    OutputRevealWindow window;
    auto now=Now();
    Check(window.Hold(9,now,now+30,now+100),"fresh output sets the original wait deadline");
    std::unique_lock lock(mutex);
    const auto start=std::chrono::steady_clock::now();
    window.AwaitOwner(lock,window.Remaining(now),[&]{return !window.BlocksRaw(Now());});
    Check(std::chrono::steady_clock::now()-start<1s,"missing owner cannot wait indefinitely");
    Check(!window.BlocksRaw(Now()),"expiry resumes admission for original identity revalidation");
    Check(!window.Hold(9,Now(),Now()+1000,Now()+1000),"retry cannot extend the old observation deadline");
    bool hidden=false;
    Check(!window.AwaitOwner(lock,10,[&]{return hidden;}),"deadline cannot fabricate hidden acknowledgement");
}
void EndpointHeldHandoff() {
    std::mutex endpoint,snapshot;
    std::recursive_timed_mutex display;
    OutputRevealWindow window;
    bool hidden=false;
    std::promise<void> entered;
    auto worker=std::async(std::launch::async,[&] {
        std::lock_guard endpointLock(endpoint);
        std::unique_lock displayLock(display);
        // RequestHide's recursive display/snapshot scopes return before wait.
        {std::lock_guard recursive(display);std::lock_guard data(snapshot);}
        entered.set_value();
        return window.AwaitOwner(displayLock,1000,[&]{return hidden;});
    });
    entered.get_future().wait();
    // A real owner publication needs display -> snapshot only. Keeping the
    // endpoint busy must not prevent this callback from releasing the waiter.
    auto owner=std::async(std::launch::async,[&] {
        std::unique_lock displayLock(display,std::defer_lock);
        if(!displayLock.try_lock_for(500ms))return false;
        std::lock_guard data(snapshot);
        hidden=true;window.OwnerChanged();return true;
    });
    Check(owner.wait_for(1s)==std::future_status::ready && owner.get(),"owner acquires display after recursive hide helper returned while endpoint remains held");
    Check(worker.wait_for(1s)==std::future_status::ready && worker.get(),"depth-one unlock lets endpoint-held waiter complete without lock cycle");
}
void SdkOwnerWaitsForWorker() {
    std::recursive_timed_mutex display;
    OutputRevealWindow window;
    SdkOwnerActivity activity;
    SdkStreamLease lease;
    const int call=1,foreignCall=2;
    Check(activity.MayWaitForOwner(false) && !activity.MayWaitForOwner(true),
          "idle owner may receive worker messages but cannot wait on itself");
    Check(!activity.Enter(nullptr) && activity.Enter(&call),"only a real call identity marks SDK owner activity");
    Check(!activity.Enter(&foreignCall),"nested activity cannot replace the active owner call");
    lease.Begin(Now(),250);lease.Revoke();
    Check(!activity.MayWaitForOwner(false),"stream revocation cannot erase a host still waiting in SDK Present");
    const auto now=Now();
    Check(window.Hold(10,now,now+1000,now+1000),"pending reveal has a real scheduling deadline");
    bool hidden=false,authorized=false,rawCalled=false;
    auto worker=std::async(std::launch::async,[&] {
        std::unique_lock lock(display);
        if(!activity.MayWaitForOwner(false))window.Release();
        // The revoked stream cannot take a raw shortcut merely because its
        // owner is waiting for this worker. Hidden acknowledgement is real.
        return !window.BlocksRaw(Now()) && !activity.MayWaitForOwner(false) &&
            !hidden && !authorized && !rawCalled;
    });
    Check(worker.wait_for(500ms)==std::future_status::ready && worker.get(),
          "SDK owner can await worker completion without worker awaiting an owner message");
    Check(!hidden && !authorized && !rawCalled,"cancelled CPU yield grants no hidden state, authorization or raw result");
    activity.Leave(&foreignCall);
    Check(!activity.MayWaitForOwner(false),"foreign cleanup cannot end another owner call");
    activity.Leave(&call);
    Check(activity.MayWaitForOwner(false),"exact SDK return restores message-based handoff");

    std::promise<void> waiting;
    auto alreadyWaiting=std::async(std::launch::async,[&] {
        std::unique_lock lock(display);waiting.set_value();
        return window.AwaitOwner(lock,1000,[&] {
            return hidden || !activity.MayWaitForOwner(false);
        });
    });
    waiting.get_future().wait();
    {
        std::lock_guard lock(display);
        Check(activity.Enter(&call),"owner may enter SDK after a worker started waiting");
        window.OwnerChanged();
    }
    Check(alreadyWaiting.wait_for(500ms)==std::future_status::ready && alreadyWaiting.get(),
          "entry notification also breaks a previously started owner-message wait");
    Check(!hidden,"wake on SDK entry does not acknowledge a hidden window");
    activity.Leave(&call);
}
}
int main() {
    RevealHandoff();HideAcknowledgement();OwnerDeadline();EndpointHeldHandoff();SdkOwnerWaitsForWorker();
    std::printf("async-output threaded handoff: %u checks passed (no GPU/display claim)\n",checks);
}
