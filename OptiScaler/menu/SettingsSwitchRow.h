#pragma once
#include "NextgenSliderStyle.h"
#include "SettingsToggle.h"
#include <imgui/imgui_internal.h>
#include <algorithm>
#include <string>

// Boolean settings only. One row hit target retains keyboard/navigation,
// disabled semantics and a stable label ID; full rows share the form control column.
namespace SettingsUi
{
inline bool SwitchRow(const char* label, bool* value, const char* note = nullptr, bool compact = false)
{
    if (ImGui::GetCurrentWindow()->SkipItems) return false;
    const std::string stable(label);
    const auto visible = stable.substr(0, stable.find("##"));
    const float h = ImGui::GetFrameHeight();
    const auto visual = ToggleSize();
    const float gap = ToggleGap();
    const auto textSize = ImGui::CalcTextSize(visible.c_str());
    const float naturalWidth = visible.empty() ? visual.x : textSize.x + gap + visual.x;
    compact = compact || ImGui::GetCurrentWindow()->DC.IsSameLine || visible.empty();
    // Move an inline label/switch group together before measuring its new row.
    if (ImGui::GetCurrentWindow()->DC.IsSameLine && ImGui::GetContentRegionAvail().x < naturalWidth)
        ImGui::NewLine();
    const float contentWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float w = std::min(visual.x, contentWidth);
    const auto row = NextgenSliderStyle::Layout(!visible.empty(), 0.f, false);
    const bool stacked = !visible.empty() && (compact
        ? contentWidth < w + gap + ImGui::GetFontSize() : row.labelAbove);
    const float labelGap = compact ? gap : row.gap;
    float labelWidth = visible.empty() ? 0.0f : stacked ? contentWidth
        : compact ? std::min(textSize.x, std::max(1.0f, contentWidth - w - labelGap)) : row.label;
    auto wrappedText = ImGui::CalcTextSize(visible.c_str(), nullptr, false,
                                         visible.empty() ? -1.0f : labelWidth);
    // A wrapped label advances the switch by whole, font/DPI-scaled units.
    // Measure each candidate with the actual font; never infer width from bytes
    // or characters. Leave a complete hit target and gap inside this same row.
    const float step = ImGui::GetFontSize();
    while (!stacked && !visible.empty() && wrappedText.y > ImGui::GetTextLineHeight() + 0.5f &&
           labelWidth + labelGap + w + step <= contentWidth)
    {
        labelWidth += step;
        wrappedText = ImGui::CalcTextSize(visible.c_str(), nullptr, false, labelWidth);
    }
    const float labelHeight = visible.empty() ? 0.0f : wrappedText.y;
    const float switchOffset = visible.empty() || stacked ? 0.0f : labelWidth + labelGap;
    const float groupWidth = stacked ? std::max(labelWidth, w) : switchOffset + w;
    const float rowWidth = compact ? groupWidth : contentWidth;
    const float noteWidth = note ? ImGui::CalcTextSize(note).x : 0.0f;
    const bool inlineNote = note && !stacked && groupWidth + gap + noteWidth <= (compact ? contentWidth : rowWidth);
    const float available = compact && note
        ? (inlineNote ? groupWidth + gap + noteWidth : std::max(groupWidth, contentWidth)) : rowWidth;
    const float groupHeight = stacked ? labelHeight + gap + h : std::max(h, labelHeight);
    const float noteHeight = note && !inlineNote
        ? ImGui::CalcTextSize(note, nullptr, false, available).y : 0.0f;
    const float rowHeight = groupHeight + (note && !inlineNote ? gap + noteHeight : 0.0f);
    const ImVec2 low = ImGui::GetCursorScreenPos();
    const bool changed = ImGui::InvisibleButton(stable.c_str(), ImVec2(available, rowHeight), ImGuiButtonFlags_EnableNav);
    if (changed) { *value = !*value; ImGui::MarkItemEdited(ImGui::GetItemID()); }
    auto* draw = ImGui::GetWindowDrawList();
    if (!compact && ImGui::IsItemHovered())
        draw->AddRectFilled(low, ImVec2(low.x + available, low.y + rowHeight),
                            ImGui::GetColorU32(ImGuiCol_HeaderHovered), ImGui::GetStyle().FrameRounding);
    if (ImGui::IsItemFocused())
        draw->AddRect(low, ImVec2(low.x + available, low.y + rowHeight), ImGui::GetColorU32(ImGuiCol_NavCursor), 3.0f);
    const ImVec2 textPos(low.x, low.y + (stacked ? 0.0f : std::max(0.0f, (groupHeight - labelHeight) * 0.5f)));
    if (!visible.empty())
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), textPos, ImGui::GetColorU32(ImGuiCol_Text),
                      visible.c_str(), nullptr, labelWidth);
    const float x = low.x + switchOffset;
    const float y = low.y + (stacked ? labelHeight + gap : (groupHeight - h) * 0.5f);
    if (note)
    {
        const ImVec2 notePos = inlineNote
            ? ImVec2(x + w + gap, y + ImGui::GetStyle().FramePadding.y)
            : ImVec2(low.x, low.y + groupHeight + gap);
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), notePos,
                      ImGui::GetColorU32(ImGuiCol_TextDisabled), note, nullptr, inlineNote ? 0.0f : available);
    }
    DrawToggle(draw, ImVec2(x, y + (h - visual.y) * 0.5f), w, std::min(visual.y, w), *value);
    return changed;
}
}
