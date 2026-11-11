#pragma once
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

// Shared switch visuals and the compact header hit target. This header has no
// slider/row dependency so disclosure headers and full rows can share it.
namespace SettingsUi
{
// Visual size follows the current font/DPI; the keyboard/click row keeps its
// full frame height. Shared by standalone rows and disclosure headers.
inline ImVec2 ToggleSize()
{
    const float height = ImGui::GetFontSize() * 0.82f;
    return ImVec2(height * 1.85f, height);
}
inline float ToggleGap()
{
    return ImMax(ImGui::GetFontSize() * 0.6f, ImGui::GetStyle().ItemSpacing.x);
}
inline void DrawToggle(ImDrawList* draw, const ImVec2& origin, float width, float height, bool enabled)
{
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const ImGuiCol track = enabled ? (active || hovered ? ImGuiCol_SliderGrabActive : ImGuiCol_CheckMark)
        : active ? ImGuiCol_FrameBgActive : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg;
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
        ImGui::GetColorU32(track), height * 0.5f);
    draw->AddRect(origin, ImVec2(origin.x + width, origin.y + height),
        ImGui::GetColorU32(ImGuiCol_Border), height * 0.5f);
    draw->AddCircleFilled(ImVec2(enabled ? origin.x + width - height * 0.5f : origin.x + height * 0.5f,
                                origin.y + height * 0.5f),
                         height * 0.36f, ImGui::GetColorU32(enabled ? ImGuiCol_WindowBg : ImGuiCol_TextDisabled));
}

inline bool ToggleSwitch(const char* stable, bool* value)
{
    if (ImGui::GetCurrentWindow()->SkipItems) return false;
    const float height = ImGui::GetFrameHeight();
    const auto visual = ToggleSize();
    const float width = visual.x;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool changed = ImGui::InvisibleButton(stable, ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    if (changed)
    {
        *value = !*value;
        ImGui::MarkItemEdited(ImGui::GetItemID());
    }
    auto* draw = ImGui::GetWindowDrawList();
    if (ImGui::IsItemFocused())
        draw->AddRect(origin, ImVec2(origin.x + width, origin.y + height),
                      ImGui::GetColorU32(ImGuiCol_NavCursor), 3.0f);
    DrawToggle(draw, ImVec2(origin.x, origin.y + (height - visual.y) * 0.5f), width, visual.y, *value);
    return changed;
}
}
