#pragma once

namespace motion_flip_policy {
// StarRail needs a vertical texture-row flip for its FFX12 motion surface.
// Other titles keep the shared INI value unchanged.
inline constexpr bool effective_flip_y(bool is_starrail_process, bool configured_value) noexcept {
    return is_starrail_process || configured_value;
}
}
