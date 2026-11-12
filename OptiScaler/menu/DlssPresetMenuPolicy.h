// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <OptiTypes.h>

namespace DlssPresetMenu
{
enum class Page { Hidden, SuperResolution, RayReconstruction };

// A DX11/Vulkan compatibility layer does not change the underlying SR
// preset contract. RR retains its own settings and existing visibility.
constexpr Page SettingsFor(Upscaler active, bool enabled, int major) noexcept
{
    if (active == Upscaler::DLSSD) return Page::RayReconstruction;
    if (enabled && major > 2 && (active == Upscaler::DLSS || active == Upscaler::DLSS_on12))
        return Page::SuperResolution;
    return Page::Hidden;
}
}
