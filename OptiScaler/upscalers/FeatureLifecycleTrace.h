#pragma once
#include <windows.h>
#include <Logger.h>
// Caller CPU time only: includes vendor waits, not an independent GPU duration.
class FeatureLifecycleTrace
{
    const char* phase;
    unsigned int handle;
    LARGE_INTEGER start {}, frequency {};
public:
    FeatureLifecycleTrace(const char* p, unsigned int h = 0) : phase(p), handle(h)
    {
        QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
        LOG_INFO("F5 lifecycle begin: {} handle {} qpc {} frequency {}", phase, handle, start.QuadPart, frequency.QuadPart);
    }
    ~FeatureLifecycleTrace()
    {
        LARGE_INTEGER end {}; QueryPerformanceCounter(&end);
        const double ms = frequency.QuadPart > 0 ? 1000.0 * (end.QuadPart - start.QuadPart) / frequency.QuadPart : -1.0;
        LOG_INFO("F5 lifecycle end: {} handle {} cpuMs {:.3f} qpc {} frequency {}", phase, handle, ms, end.QuadPart, frequency.QuadPart);
    }
};
