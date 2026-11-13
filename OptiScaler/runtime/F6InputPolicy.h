// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Input policy does not alter persisted user settings. Genshin enters through
// the external bridge's ordinary NGX inputs; public FSR APIs remain available
// to all supported DX11/DX12/Vulkan backends.
namespace F6InputPolicy
{
inline constexpr bool DirectDlssInputs = true;
inline constexpr bool PublicFsrInputs = true;
}
