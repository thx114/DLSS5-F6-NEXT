// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Windows.h>
#include <cstdint>

namespace frame_limit
{
// The cadence in FrameLimit::sleep is thread-local. Its timer must have the
// same owner: rearming an auto-reset timer shared by two callers strands one.
inline int TimerSleep(int64_t hundredNs)
{
    struct Timer final
    {
        HANDLE handle=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
        ~Timer() { if (handle) CloseHandle(handle); }
    };
    static thread_local Timer timer;
    if (!timer.handle) return 1;
    LARGE_INTEGER dueTime;
    dueTime.QuadPart=-hundredNs;
    if (!SetWaitableTimerEx(timer.handle,&dueTime,0,nullptr,nullptr,nullptr,0)) return 2;
    return WaitForSingleObject(timer.handle,1000)==WAIT_OBJECT_0 ? 0 : 3;
}
}
