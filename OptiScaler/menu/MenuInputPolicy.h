#pragma once

#include <imgui/imgui.h>

namespace MenuInputPolicy
{
// Availability belongs to the platform backend. Visibility only controls which
// input ImGui may consume; it must not invent a connected controller or discard
// unrelated backend/configuration capabilities.
inline void SetVisible(ImGuiIO& io, bool visible)
{
    constexpr ImGuiConfigFlags managed = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad |
        ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoKeyboard;
    const ImGuiConfigFlags mode = visible
        ? ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad
        : ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoKeyboard;
    io.ConfigFlags = (io.ConfigFlags & ~managed) | mode;
}
}
