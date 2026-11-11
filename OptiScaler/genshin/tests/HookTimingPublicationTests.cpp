// SPDX-License-Identifier: GPL-3.0-or-later
#include "../GenshinHookCpuTiming.h"
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
struct Clock {inline static ULONGLONG now=1000;static ULONGLONG Now()noexcept{return now;}};
unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAILED: %s\n",why);std::abort();}}
}
int main() {
    using namespace GenshinHookCpuTiming;
    SetEnvironmentVariableW(L"OPTISCALER_HOOK_CPU_TIMING",nullptr);
    unsigned reports=0,workerReports=0;uint64_t lastCalls=0;bool lastEnabled=false;
    auto emit=[&](DWORD tid,const char*,const char*,const Values& v,int64_t,unsigned,uint64_t,bool on){
        if(tid!=GetCurrentThreadId())++workerReports;
        ++reports;lastCalls=v[Calls];lastEnabled=on;};
    auto drain=[&](bool on){OnPresent<Clock>(on,true,emit);};
    auto work=[] {for(unsigned i=0;i<64;++i){Scope sample(Stage::MapPortable);}};
    drain(true);work();Clock::now=2000;drain(true);
    Check(reports==0,"full text dump repeated after only one second");
    Check(monitorEnd.load()==2000,"live monitor not published at one second");
    const auto firstVersion=monitorVersion.load();
    work();Clock::now=3000;drain(true);
    Check(reports==0 && monitorVersion.load()>firstVersion,"text throttle stopped live monitor updates");
    work();Clock::now=6000;drain(true);
    Check(reports==1 && lastCalls==192 && lastEnabled,"five-second cumulative report lost calls");
    work();Clock::now=7000;drain(true);
    Check(reports==1,"detailed text did not remain throttled");
    // Disable must drain accumulated values even inside the detail interval.
    Clock::now=7001;drain(false);
    Check(reports==2 && lastCalls==256 && !lastEnabled,"disable lost unreported tail");
    Clock::now=8000;drain(false);Check(reports==2,"disabled monitor emitted another report");
    Clock::now=8001;drain(true);
    Check(monitorEnd.load()==8001 && reports==3 && lastEnabled,"new timing epoch not immediately reported");
    std::thread worker(work);worker.join();Clock::now=9001;drain(true);
    Check(workerReports==0,"worker text emitted before detailed interval");
    Clock::now=10001;drain(true);
    Check(workerReports==0,"monitor consumption prematurely emitted worker row");
    Clock::now=13001;drain(true);
    Check(workerReports==1,"finished worker tail lost after earlier monitor-only drain");
    std::printf("Hook timing publication: %u checks; real production publisher with deterministic clock; no GPU\n",checks);
}
