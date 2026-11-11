// SPDX-License-Identifier: GPL-3.0-or-later
// CPU configuration contracts only; never loads or executes an SM86 runtime.
#include "../../framegen/dlssg/AmpereMfgLoader.h"
#include "../ManagedIni.h"
#include <cassert>
#include <climits>
#include <cstdio>

int main()
{
    using namespace AmpereMfgLoader;
    unsigned checks = 0;
    for (int requested : {-1, 0, 1, 2, 3, 4, 5, 6, INT_MAX})
    {
        const auto content = FormatIniContent(requested, "Auto");
        CSimpleIniA ini;
        assert(ini.LoadData(content) >= 0);
        const int expected = requested > 0 && requested <= MaximumGeneratedRequest ? requested : DefaultGeneratedRequest;
        assert(ini.GetLongValue("FrameGeneration", "MaxGeneratedFrames", -1) == expected);
        ++checks;
    }
    const std::string original =
        "; user-owned tuning\n[General]\nEnabled=1\n"
        "[FrameGeneration]\nOptimized=2\nMaxGeneratedFrames=3\n"
        "[Compatibility]\nPreset=B\nKernelImage=PTX\n"
        "[Runtime]\nMode=Bundled\nCacheDirectory=custom-cache\n"
        "[Logging]\nLevel=3\nDirectory=custom-logs\n";
    std::string merged;
    const auto settings = FormatIniContent(5, "Cubin", 1, "SM86", 1);
    assert(FramePipeline::MergeManagedIni(original, settings, merged));
    assert(merged.find("user-owned tuning") != std::string::npos);
    CSimpleIniA parsed;
    assert(parsed.LoadData(merged) >= 0);
    assert(parsed.GetLongValue("FrameGeneration", "MaxGeneratedFrames") == 5);
    assert(parsed.GetLongValue("FrameGeneration", "Optimized") == 2);
    assert(parsed.GetLongValue("General", "Enabled") == 1);
    assert(std::string(parsed.GetValue("Compatibility", "Preset")) == "B");
    assert(std::string(parsed.GetValue("Runtime", "Mode")) == "Bundled");
    assert(std::string(parsed.GetValue("Runtime", "CacheDirectory")) == "custom-cache");
    assert(std::string(parsed.GetValue("Logging", "Directory")) == "custom-logs");
    assert(parsed.GetLongValue("Logging", "Level") == 1);
    assert(std::string(parsed.GetValue("Compatibility", "KernelImage")) == "Cubin");
    assert(parsed.GetLongValue("Compatibility", "HardwareBilinear") == 1);
    ++checks;
    std::string repeated;
    assert(FramePipeline::MergeManagedIni(merged, settings, repeated) && repeated == merged);
    std::string fresh;
    assert(FramePipeline::MergeManagedIni({}, FormatIniContent(0, "invalid", 8, "invalid", 17), fresh));
    CSimpleIniA defaults;
    assert(defaults.LoadData(fresh) >= 0);
    assert(defaults.GetLongValue("FrameGeneration", "MaxGeneratedFrames") == DefaultGeneratedRequest);
    assert(std::string(defaults.GetValue("Compatibility", "KernelImage")) == "Auto");
    assert(std::string(defaults.GetValue("Compatibility", "Router")) == "SM86");
    assert(defaults.GetLongValue("Compatibility", "HardwareBilinear", -1) == 0);
    assert(defaults.GetLongValue("Logging", "Level", -1) == 1);
    ++checks;
    for (const auto& unsupported : {std::string("[General]\nEnabled=1") + std::string(1, '\0') + "[Runtime]\nMode=External",
                                    std::string("\xff\xfe[General]"), std::string("\xfe\xff[General]")})
    {
        std::string untouched = "unchanged";
        assert(!FramePipeline::MergeManagedIni(unsupported, settings, untouched));
        assert(untouched == "unchanged");
        assert(!FramePipeline::MergeManagedIni(original, unsupported, untouched));
        assert(untouched == "unchanged");
        ++checks;
    }
    std::printf("Sm86Settings: %u request and preservation groups passed; runtime capability untested\n", checks);
}
