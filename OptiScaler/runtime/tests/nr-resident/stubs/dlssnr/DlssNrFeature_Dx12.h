#pragma once
#include <string>

namespace DlssNr
{
struct DetailReuseInfo
{
    unsigned long long full = 0, reused = 0, fallback = 0, held = 0;
    std::string why;
    double lightMs = 0, heavyMs = 0, averageMs = 0, baseFps = 0;
    bool active = false, holding = false;
    float dropped = -1;
    unsigned long long modelCalls = 0, modelFailures = 0;
    bool modelCallCountsKnown = false;
};
}
