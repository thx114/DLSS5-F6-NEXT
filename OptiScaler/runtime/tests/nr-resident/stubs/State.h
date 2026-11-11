#pragma once
#include <atomic>
#include <cstdint>

struct TestFg
{
    bool IsActive() const { return true; }
    bool IsPaused() const { return false; }
};

struct State
{
    static State& Instance() { static State state; return state; }
    TestFg* currentFG = nullptr;
    bool dlssgLastEvaluateGenerates = false, externalFrameGeneration = false;
    std::atomic<bool> dlssgLastEvaluateGeneratesVk {false};
    uint64_t dlssgLastEvaluateFrame = 0;
    std::atomic<uint64_t> dlssgLastEvaluateFrameVk {0};
};
