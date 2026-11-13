#include <runtime/LibraryObservation.h>
#include <runtime/LibraryLoadRequest.h>
#include <cassert>
#include <cstdio>

struct State {
    bool isShuttingDown = false;
    static State& Instance() { static State state; return state; }
};
LibraryLoading::Observations observations;
std::atomic<bool> observationWorker {false};
PTP_TIMER observationRetryTimer = nullptr;
HANDLE retryCompleted = nullptr;
std::atomic<unsigned> attempts=0;
class LibraryLoadHooks {
public:
    static void SchedulePending() noexcept;
    static void ProcessPending() {
        observations.Process(true, [] {}, [](HMODULE,bool) {
            assert(!LibraryLoading::InLoaderCallout());
            if (++attempts==1) return false;
            SetEvent(retryCompleted); return true;
        });
    }
};
#include "scheduler.inc"
#include "roles.inc"
int main() {
    assert(OtaRole(L"c:\\sl_dlss_0\\versions\\1\\plugin.dll")==OtaPlugin::Dlss);
    assert(OtaRole(L"c:\\sl_dlss_g_0\\versions\\1\\plugin.dll")==OtaPlugin::Dlssg);
    assert(OtaRole(L"c:\\sl_reflex_0\\versions\\1\\plugin.dll")==OtaPlugin::Reflex);
    assert(OtaRole(L"c:\\sl_pcl_0\\versions\\1\\plugin.dll")==OtaPlugin::Pcl);
    assert(OtaRole(L"c:\\sl_common_0\\versions\\1\\plugin.dll")==OtaPlugin::Common);
    assert(OtaRole(L"c:\\unrelated\\versions\\1\\plugin.dll")==OtaPlugin::None);
    assert(OtaRole(L"c:\\sl_dlss_0\\versions\\1\\plugin.bin")==OtaPlugin::None);
    retryCompleted=CreateEventW(nullptr,TRUE,FALSE,nullptr); assert(retryCompleted);
    observations.Enqueue(GetModuleHandleW(L"kernel32.dll"));
    SetLastError(4242); LibraryLoadHooks::SchedulePending(); assert(GetLastError()==4242);
    // No second load or explicit Process call: the real production worker must
    // wake the retained failed request and complete its own retry.
    assert(WaitForSingleObject(retryCompleted,5000)==WAIT_OBJECT_0);
    for (unsigned n=0; observationWorker.load() && n<500; ++n) Sleep(10);
    assert(attempts==2 && !observationWorker.load() && !observations.HasWork());
    if (observationRetryTimer) {
        SetThreadpoolTimer(observationRetryTimer,nullptr,0,0);
        WaitForThreadpoolTimerCallbacks(observationRetryTimer,TRUE);
        CloseThreadpoolTimer(observationRetryTimer);
    }
    CloseHandle(retryCompleted);
    std::puts("PASS production loader scheduler: automatic failed-install retry, idle convergence and OTA roles");
}
