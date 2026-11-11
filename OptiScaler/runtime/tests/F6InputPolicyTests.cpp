// SPDX-License-Identifier: GPL-3.0-or-later
#include <runtime/F6InputPolicy.h>
#include <upscalers/GameRoutePolicy.h>
#include <genshin/GenshinLegacyControls.h>
#include <cassert>
#include <cstdio>
#include <map>
#include <string>

struct Ini
{
    std::map<std::string, std::string> values;
    const char* GetValue(const char*, const char* key, const char* fallback) const
    {
        const auto found = values.find(key);
        return found == values.end() ? fallback : found->second.c_str();
    }
    void SetValue(const char*, const char* key, const char* value) { values[key] = value; }
};

int main()
{
    static_assert(F6InputPolicy::DirectDlssInputs);
    static_assert(!F6InputPolicy::FsrBridgeEnabled);
    for (const auto game : {GameRouting::Identity::Other, GameRouting::Identity::Genshin,
                           GameRouting::Identity::StarRail, GameRouting::Identity::ZenlessZoneZero})
    {
        assert(GameRouting::UseGenshinPresentPipeline(game,true) == (game == GameRouting::Identity::Genshin));
        assert(!GameRouting::UseGenshinPresentPipeline(game,false));
        // Repeated hot changes must neither latch presentation nor authorize a bridge.
        for (unsigned i=0;i<100;++i) {
            assert(!GameRouting::UseGenshinPresentPipeline(game,false));
            assert(!GameRouting::UseGenshinBridgeTransport(game,true));
        }
    }

    // Old enabled profiles and optional capture/debug demand cannot authorize
    // bridge work. Keep their requested values available for lossless saving.
    for (unsigned mask = 0; mask != 32; ++mask)
    {
        GenshinLegacy::ControlsSnapshot controls;
        controls.settings.enabled = (mask & 1) != 0;
        controls.enableFsr2Inputs = (mask & 2) != 0;
        controls.useFsr2Inputs = (mask & 4) != 0;
        controls.settings.captureOnlyObservation = (mask & 8) != 0;
        controls.hookCpuTiming = (mask & 16) != 0;
        assert(!controls.Active());
    }

    Ini oldProfile;
    oldProfile.values = {{"Enabled", "true"}, {"CaptureOnlyObservation", "true"},
                         {"Fsr2FamilySkip", "true"}, {"Ffx12MotionScale", "1.5"},
                         {"Fsr2SharpnessPercent", "37"}, {"Ffx12JitterMode", "4"}};
    const auto requested = GenshinLegacy::LoadControls(oldProfile);
    assert(requested.enabled && requested.captureOnlyObservation && requested.familySkip);
    GenshinLegacy::ControlsSnapshot controls;
    controls.settings = requested;
    assert(!controls.Active());
    Ini savedProfile;
    GenshinLegacy::SaveControls(savedProfile, requested);
    assert(GenshinLegacy::LoadControls(savedProfile) == requested);
    assert(savedProfile.values.at("Enabled") == "1");
    assert(savedProfile.values.at("CaptureOnlyObservation") == "1");
    std::puts("PASS F6 direct DLSS policy: 32 legacy requests rejected; profile round-trip preserved");
}
