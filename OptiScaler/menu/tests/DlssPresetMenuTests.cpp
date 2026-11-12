// SPDX-License-Identifier: GPL-3.0-or-later
// OptiTypes expects the application's logger; this fixture only uses its enum.
#define LOG_WARN(...) ((void)0)
#include <menu/DlssPresetMenuPolicy.h>
#undef LOG_WARN
#include <array>
#include <cstdio>
#include <cstdlib>

int main()
{
    using enum Upscaler;
    using DlssPresetMenu::Page;
    using DlssPresetMenu::SettingsFor;
    unsigned checks = 0;
    const auto expect = [&](Upscaler route, bool enabled, int version, Page expected) {
        ++checks;
        if (SettingsFor(route, enabled, version) != expected)
        {
            std::fprintf(stderr, "Unexpected preset page for route %d, enabled=%d, major=%d\n",
                         static_cast<int>(route), enabled, version);
            std::exit(1);
        }
    };
    for (auto sr : { DLSS, DLSS_on12 })
    {
        expect(sr, true, 3, Page::SuperResolution);
        expect(sr, true, 4, Page::SuperResolution);
        expect(sr, true, 2, Page::Hidden);
        expect(sr, true, 0, Page::Hidden);
        expect(sr, false, 4, Page::Hidden);
    }
    expect(DLSSD, true, 4, Page::RayReconstruction);
    expect(DLSSD, false, 0, Page::RayReconstruction);
    for (auto other : { FSR31, FFX_on12, XeSS, XeSS_on12, Reset, Optical })
        expect(other, true, 4, Page::Hidden);
    std::printf("DLSS preset menu: %u route checks passed\n", checks);
}
