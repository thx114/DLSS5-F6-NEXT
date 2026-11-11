#pragma once
#include "SysUtils.h"

class FrameLimit
{
    static uint64_t get_timestamp();
    static int timer_sleep(int64_t hundred_ns);
    static int busywait_sleep(int64_t ns);
    static int combined_sleep(int64_t ns);

  public:
    // Count is a scalar snapshot of the provider's applied generated frames,
    // taken by its guarded caller. Default keeps unknown providers' 2X fallback.
    static void sleep(bool fgActive, uint32_t framesToGenerate = 1);
};
