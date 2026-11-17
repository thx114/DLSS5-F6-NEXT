// SPDX-License-Identifier: GPL-3.0-or-later
#include <runtime/F6InputPolicy.h>
#include <upscalers/GameRoutePolicy.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <string_view>

int main()
{
    using GameRouting::Identity;
    static_assert(F6InputPolicy::DirectDlssInputs);
    static_assert(F6InputPolicy::PublicFsrInputs);
    for (const auto game : {Identity::Other, Identity::Genshin, Identity::StarRail, Identity::ZenlessZoneZero})
        for (unsigned iteration = 0; iteration < 100; ++iteration)
            for (bool requested : {false, true})
                assert(GameRouting::UseGenshinSourceOutput(game, requested) ==
                       (game == Identity::Genshin && requested));
    struct Case { std::wstring_view path; Identity expected; };
    for (const auto& test : std::array {
        Case {L"YuanShen.exe", Identity::Genshin},
        Case {L"C:\\Games\\GenshinImpact.exe", Identity::Genshin},
        Case {L"d:/games/GENSHINIMPACT.EXE", Identity::Genshin},
        Case {L"D:\\games\\sTaRrAiL.ExE", Identity::StarRail},
        Case {L"ZenlessZoneZero.exe", Identity::ZenlessZoneZero},
        Case {L"D:/StarRail.exe/unrelated.exe", Identity::Other},
        Case {L"StarRail.exe.bak", Identity::Other}, Case {L"", Identity::Other},
        Case {L"D:/GenshinImpact.exe/", Identity::Other} })
    {
        assert(GameRouting::IdentifyExecutable(test.path) == test.expected);
        assert(GameRouting::IsGenshinExecutable(test.path) == (test.expected == Identity::Genshin));
    }
    // Match the view length rather than reading into a suffix after its boundary.
    const std::wstring_view bounded = L"StarRail.exe.backup";
    assert(GameRouting::IdentifyExecutable(bounded.substr(0, 12)) == Identity::StarRail);
    assert(GameRouting::IdentifyExecutable(nullptr) == Identity::Other);
    std::puts("PASS public input policy, route toggles and executable identity boundaries");
}
