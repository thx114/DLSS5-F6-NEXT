// Only the external overlay demand is stubbed; the timer and timeline are real.
#pragma once
template<class T> struct TimerSetting {
    T value;
    T value_or_default() const { return value; }
};
inline constexpr int FpsOverlay_ReflexTimings = 2;
struct Config {
    TimerSetting<bool> ShowFps {false};
    TimerSetting<int> FpsOverlayType {0};
    static Config* Instance() { static Config value; return &value; }
};
