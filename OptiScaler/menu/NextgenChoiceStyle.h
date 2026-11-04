#pragma once
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include "NextgenSliderStyle.h"
#include <algorithm>
#include <string_view>
#include <vector>

// Presentation only. Callers own stored values, unsupported choices and commits.
namespace NextgenChoiceStyle
{
inline bool Button(const char* stable, bool selected, float width)
{
    const auto& style=ImGui::GetStyle();
    const auto surface=style.Colors[ImGuiCol_HeaderActive];
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,ImGui::GetFontSize()*.28f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,selected?1.f:0.f);
    ImGui::PushStyleColor(ImGuiCol_Button,selected?surface:ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,style.Colors[ImGuiCol_HeaderHovered]);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,style.Colors[ImGuiCol_HeaderActive]);
    ImGui::PushStyleColor(ImGuiCol_Border,style.Colors[selected?ImGuiCol_CheckMark:ImGuiCol_Border]);
    ImGui::PushStyleColor(ImGuiCol_Text,style.Colors[selected?ImGuiCol_Text:ImGuiCol_TextDisabled]);
    const bool pressed=ImGui::Button(stable,ImVec2(width,0));
    ImGui::PopStyleColor(5);ImGui::PopStyleVar(2);
    if(pressed)ImGui::MarkItemEdited(ImGui::GetItemID());
    return pressed;
}
inline bool RadioButton(const char* stable,bool selected)
{
    const std::string_view label(stable);
    const auto visible=label.substr(0,label.find("##"));
    const float ideal=std::max(ImGui::GetFrameHeight(),ImGui::CalcTextSize(visible.data(),visible.data()+visible.size()).x+2.f*ImGui::GetStyle().FramePadding.x);
    if(ImGui::GetCurrentWindow()->DC.IsSameLine && ideal>ImGui::GetContentRegionAvail().x)ImGui::NewLine();
    const float width=std::max(1.f,std::min(ideal,ImGui::GetContentRegionAvail().x));
    const bool pressed=Button(stable,selected,width);
    if(width<ideal && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%.*s",int(visible.size()),visible.data());
    return pressed;
}
inline bool RadioButton(const char* stable,int* value,int choice)
{
    const bool pressed=RadioButton(stable,*value==choice);
    if(pressed)*value=choice;
    return pressed;
}
inline bool Segmented(const char* stable, int* selected, const char* const* items, int count,
                      const bool* enabled = nullptr, const bool* visible = nullptr,
                      const char* const* tips = nullptr)
{
    if (!selected || !items || count <= 0 || ImGui::GetCurrentWindow()->SkipItems) return false;
    if (visible && std::none_of(visible, visible + count, [](bool item) { return item; })) return false;
    const std::string_view label(stable);
    const auto text = label.substr(0, label.find("##"));
    ImGui::BeginGroup();
    const auto row = NextgenSliderStyle::Layout(!text.empty(), 0.f, false);
    bool hovered = NextgenSliderStyle::RowLabel(text, row);
    ImGui::PushID(stable);
    const auto origin = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetFrameHeight();
    const float contentWidth = std::max(1.f, ImGui::GetContentRegionAvail().x);
    const float pad = std::min(ImGui::GetFontSize() * .18f, contentWidth * .1f);
    const float gap = std::max(1.f, ImGui::GetFontSize() * .16f);
    const float available = std::max(1.f, contentWidth - 2.f * pad);
    struct Item { int choice; float width, x, y; };
    std::vector<Item> layout;
    layout.reserve(count);
    float x = 0.f, y = 0.f;
    for (int i = 0; i < count; ++i)
    {
        if (visible && !visible[i]) continue;
        const float width = std::min(available, std::max(height,
            ImGui::CalcTextSize(items[i], nullptr, true).x + 2.f * ImGui::GetStyle().FramePadding.x));
        if (x > 0.f && x + width > available) { x = 0.f; y += height + gap; }
        layout.push_back({ i, width, x, y });
        x += width + gap;
    }
    // Fill each wrapped row within one recessed group. Choice indices and ID scopes stay fixed.
    for (size_t first = 0; first < layout.size();)
    {
        size_t end = first + 1;
        while (end < layout.size() && layout[end].y == layout[first].y) ++end;
        const auto& last = layout[end - 1];
        const float extra = std::max(0.f, available - last.x - last.width) / float(end - first);
        float left = 0.f;
        for (size_t i = first; i < end; ++i)
        {
            layout[i].x = left;
            layout[i].width += extra;
            left += layout[i].width + gap;
        }
        first = end;
    }
    const ImVec2 extent(contentWidth, y + height + 2.f * pad);
    auto* draw = ImGui::GetWindowDrawList();
    const auto& style = ImGui::GetStyle();
    const float rounding = ImGui::GetFontSize() * .35f;
    draw->AddRectFilled(origin, ImVec2(origin.x + extent.x, origin.y + extent.y),
                        ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
    draw->AddRect(origin, ImVec2(origin.x + extent.x, origin.y + extent.y),
                  ImGui::GetColorU32(ImGuiCol_Border), rounding);
    bool changed = false;
    for (const auto& item : layout)
    {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + pad + item.x, origin.y + pad + item.y));
        ImGui::PushID(item.choice);
        ImGui::BeginDisabled(enabled && !enabled[item.choice]);
        if (Button(items[item.choice], *selected == item.choice, item.width) && *selected != item.choice)
        {
            *selected = item.choice;
            changed = true;
        }
        hovered |= ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            const char* tip = tips && tips[item.choice] ? tips[item.choice] : nullptr;
            if (tip && *tip)
                ImGui::SetTooltip("%s", tip);
            else if (ImGui::CalcTextSize(items[item.choice], nullptr, true).x + 2.f * style.FramePadding.x > item.width)
            {
                const std::string_view option(items[item.choice]);
                const auto optionText = option.substr(0, option.find("##"));
                ImGui::SetTooltip("%.*s", int(optionText.size()), optionText.data());
            }
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(extent);
    hovered |= ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    ImGui::PopID();
    ImGui::EndGroup();
    NextgenSliderStyle::RememberHelpTarget(hovered);
    return changed;
}
}
