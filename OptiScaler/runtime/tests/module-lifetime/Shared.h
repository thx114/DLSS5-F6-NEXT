#pragma once
#include <Windows.h>
struct Shared
{
    volatile LONG detaches = 0;
    volatile LONG cleanupCalls = 0;
    volatile LONG loaderCleanupCalls = 0;
    volatile LONG workerCalls = 0;
};
