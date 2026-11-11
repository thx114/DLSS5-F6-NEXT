// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Product input policy, independent of persisted profiles and runtime toggles.
// NR/FG consume the current DLSS upscaler evaluation. Legacy FSR interception,
// shader discovery and its private Present producer cannot be re-enabled by an
// old INI, a debug/capture request or a late-loaded FSR module.
namespace F6InputPolicy
{
inline constexpr bool DirectDlssInputs = true;
inline constexpr bool FsrBridgeEnabled = false;
}
