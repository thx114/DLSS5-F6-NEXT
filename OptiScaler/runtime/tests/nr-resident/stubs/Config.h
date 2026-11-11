// CPU environment for the REAL DlssNrDetailReuseHost.h. No Gate/cadence logic.
#pragma once
#define LOG_INFO(...) ((void)0)

template<class T> struct TestSetting
{
    T value;
    T value_or_default() const { return value; }
};

struct Config
{
    TestSetting<bool> DlssNrDetailReuse {true}, DlssNrDetailReuseWithFg {true};
    TestSetting<bool> DlssNrDetailReuseDebug {false};
    TestSetting<bool> DlssNrMotionTextureFlipY {false}, DlssNrMotionYInverted {false};
    TestSetting<unsigned> DlssNrDetailReuseInterval {2}, DlssNrDetailReuseSpread {1};
    TestSetting<unsigned> DlssNrPresentedTemporalDurationMs {0}, DlssNrPresentedTemporalMaxAgeMs {250};
    TestSetting<float> DlssNrDetailReuseMinFps {0}, DlssNrDetailReuseMaxDropped {0};
    TestSetting<float> DlssNrDetailReuseSteady {0}, DlssNrDetailReuseFill {0};
    TestSetting<float> DlssNrDetailReuseClipGamma {1}, DlssNrDetailReuseClipFalloff {1};
    TestSetting<float> DlssNrDetailReuseSigmaFloor {0.01f}, DlssNrPresentedTemporalStrength {0.85f};
    static constexpr unsigned kPresentedTemporalDurationMaxMs = 1000;
    static constexpr unsigned kPresentedTemporalMaxAgeMinMs = 1, kPresentedTemporalMaxAgeMaxMs = 1000;
};
