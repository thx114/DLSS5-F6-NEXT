#pragma once
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include "SettingsToggle.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <type_traits>

// Presentation only: the caller continues to own defaults, reset and commit policy.
namespace NextgenSliderStyle
{
inline std::string HiddenLabel(const char* stable)
{
    const std::string_view label(stable);
    if (label.starts_with("##")) return std::string(label);
    const auto reset = label.find("###");
    return reset == std::string_view::npos ? "###" + std::string(label) : std::string(label.substr(reset));
}

struct RowState
{
    ImGuiContext* context = nullptr;
    ImGuiWindow* window = nullptr;
    int frame = -1;
    ImGuiID item = 0;
    float y = 0;
};
inline RowState lastRow;

// Capture native hover while each component is the current item. Never infer a
// target from a screen rectangle or overwrite ImGui's edit/navigation state.
struct ItemHelpTarget
{
    ImGuiContext* context = nullptr;
    ImGuiWindow* window = nullptr;
    int frame = -1;
    ImGuiID item = 0;
    ImRect anchor;
    bool hovered = false;
};
inline ItemHelpTarget itemHelpTarget;
inline bool MatchesHelpTarget()
{
    const auto& current = GImGui->LastItemData;
    const auto& target = itemHelpTarget;
    return target.context == GImGui && target.window == ImGui::GetCurrentWindow() &&
        target.frame == ImGui::GetFrameCount() && target.item == current.ID &&
        target.anchor.Min.x == current.Rect.Min.x && target.anchor.Min.y == current.Rect.Min.y &&
        target.anchor.Max.x == current.Rect.Max.x && target.anchor.Max.y == current.Rect.Max.y;
}
inline void RememberHelpTarget(bool hovered)
{
    itemHelpTarget = { GImGui, ImGui::GetCurrentWindow(), ImGui::GetFrameCount(),
        GImGui->LastItemData.ID, GImGui->LastItemData.Rect, hovered };
}
inline bool ItemHelpHovered()
{
    return MatchesHelpTarget() ? itemHelpTarget.hovered
        : ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
}

// Called only for the caller's existing adjacent reset button. Keep its stable ID.
inline bool IsRowReset(const char* source)
{
    const std::string_view label(source ? source : "");
    const auto visible = label.substr(0, label.find("##"));
    return (visible == "重置" || visible == "Reset") && GImGui &&
        lastRow.context == GImGui && lastRow.window == ImGui::GetCurrentWindow() &&
        lastRow.frame == ImGui::GetFrameCount() && lastRow.item == GImGui->LastItemData.ID &&
        std::abs(ImGui::GetCursorScreenPos().y - lastRow.y) < ImGui::GetFrameHeight();
}

template<class T> inline bool IsDefault(T value, T neutral)
{
    if constexpr (std::is_floating_point_v<T>)
        return std::isfinite(value) && std::abs(value - neutral) <= T(0.00001) * std::max(T(1), std::abs(neutral));
    else
        return value == neutral;
}

inline bool ResetButton(const char* stable, const char* tooltip, bool visible = true)
{
    const bool settingHovered = ItemHelpHovered();
    const auto hidden = HiddenLabel(stable);
    const float side = ImGui::GetFrameHeight();
    if (!visible)
    {
        ImGui::Dummy(ImVec2(side, side));
        RememberHelpTarget(settingHovered);
        lastRow = {};
        return false;
    }
    const bool pressed = ImGui::Button(hidden.c_str(), ImVec2(side, side));
    const auto a = ImGui::GetItemRectMin();
    const ImVec2 centre(a.x + side * .5f, a.y + side * .5f);
    const float radius = side * .24f;
    auto* draw = ImGui::GetWindowDrawList();
    const ImU32 colour = ImGui::GetColorU32(ImGuiCol_Text);
    // A drawn undo arrow avoids depending on a symbol in the selected CN/EN font.
    draw->PathArcTo(centre, radius, -IM_PI * .72f, IM_PI * .72f, 16);
    draw->PathStroke(colour, 0, std::max(1.f, side * .055f));
    const ImVec2 end(centre.x + std::cos(-IM_PI * .72f) * radius,
                     centre.y + std::sin(-IM_PI * .72f) * radius);
    draw->AddTriangleFilled(end, ImVec2(end.x + radius * .85f, end.y - radius * .15f),
                            ImVec2(end.x + radius * .15f, end.y + radius * .75f), colour);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tooltip);
    // The reset keeps its own action tip; setting help stays on label/rail/number.
    RememberHelpTarget(settingHovered);
    lastRow = {};
    return pressed;
}

template<class T> inline float NumberWidth(T value, T low, T high, const char* format, const char* zeroText)
{
    constexpr ImGuiDataType type = std::is_same_v<T, float> ? ImGuiDataType_Float : ImGuiDataType_S32;
    char numericFormat[64] {};
    const char* editFormat = ImParseFormatTrimDecorations(format, numericFormat, IM_ARRAYSIZE(numericFormat));
    if (!*editFormat) editFormat = ImGui::DataTypeGetInfo(type)->PrintFmt;
    float width = zeroText ? ImGui::CalcTextSize(zeroText).x : 0.f;
    // Include range endpoints and the current value, plus the undecorated editor.
    // Measuring the zero label even while nonzero keeps Unlimited from shifting the rail.
    for (const T sample : {low, high, value})
        for (const char* sampleFormat : {format, editFormat})
        {
            char text[128] {};
            ImGui::DataTypeFormatString(text, IM_ARRAYSIZE(text), type, &sample, sampleFormat);
            width = std::max(width, ImGui::CalcTextSize(text).x);
        }
    return std::ceil(width + ImGui::GetStyle().FramePadding.x * 2.f + 1.f);
}

// Shared renderers with native text reset buttons can reserve their measured width.
inline float rowResetWidth = 0.f;
class ScopedResetWidth
{
    float previous;
public:
    explicit ScopedResetWidth(float width) : previous(rowResetWidth) { rowResetWidth = width; }
    ~ScopedResetWidth() { rowResetWidth = previous; }
    ScopedResetWidth(const ScopedResetWidth&) = delete;
    ScopedResetWidth& operator=(const ScopedResetWidth&) = delete;
};

struct RowLayout
{
    float height, gap, width, label, number, rail;
    bool labelAbove, numberBelow;
};
inline RowLayout Layout(bool hasLabel, float numberWidth = 0.f, bool reserveReset = true)
{
    const float font = ImGui::GetFontSize(), height = ImGui::GetFrameHeight();
    const auto& style = ImGui::GetStyle();
    const float available = std::max(1.f, ImGui::GetContentRegionAvail().x);
    const float gap = std::floor(std::min(style.ItemSpacing.x, available * .025f));
    // Only sliders have an adjacent reset. Choices and switches use the full sheet.
    const float reset = reserveReset ? (rowResetWidth > 0.f ? rowResetWidth : height) + style.ItemSpacing.x : 0.f;
    const float width = std::max(1.f, std::floor(available - reset));
    const float number = std::min(std::ceil(numberWidth), width);
    const float minimumRail = font * 4.f;
    const bool numberBelow = number > 0.f && width - gap - number < minimumRail;
    const float labelRoom = width - number - gap - minimumRail - gap;
    const bool labelAbove = hasLabel && (numberBelow || labelRoom < font * 4.f);
    const float label = !hasLabel ? 0.f : labelAbove ? available
        : std::floor(std::min({font * 11.f, available * .30f, labelRoom}));
    const float rail = numberBelow ? available : std::max(1.f,
        std::floor(width - (hasLabel && !labelAbove ? label + gap : 0.f) - (number > 0.f ? gap + number : 0.f)));
    return {height, gap, width, label, number, rail, labelAbove, numberBelow};
}
inline bool RowLabel(std::string_view visible, const RowLayout& row)
{
    if (visible.empty()) return false;
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(row.label, row.height));
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    const ImVec2 textMin(origin.x, origin.y + ImGui::GetStyle().FramePadding.y);
    const ImVec2 textMax(origin.x + row.label, origin.y + row.height);
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), textMin, textMax, textMax.x,
                             visible.data(), visible.data() + visible.size(), nullptr);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        ImGui::CalcTextSize(visible.data(), visible.data() + visible.size()).x > row.label)
        ImGui::SetTooltip("%.*s", int(visible.size()), visible.data());
    if (!row.labelAbove) ImGui::SameLine(0, row.gap);
    return hovered;
}

inline bool BeginCombo(const char* stable, const char* preview, ImGuiComboFlags flags)
{
    const std::string_view label(stable);
    const auto visible = label.substr(0, label.find("##"));
    if (visible.empty()) return ImGui::BeginCombo(stable, preview, flags);
    const auto row = Layout(true, 0.f, false);
    const bool labelHovered = RowLabel(visible, row);
    const auto hidden = HiddenLabel(stable);
    ImGui::SetNextItemWidth(row.rail);
    const bool open = ImGui::BeginCombo(hidden.c_str(), preview, flags);
    // An open combo has already entered its popup. Only bind the closed parent item.
    if (!open) RememberHelpTarget(labelHovered || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled));
    return open;
}

// One header, two explicit non-overlapping hit regions: disclosure/label and enable check.
// The switch is always visible, including while collapsed; toggling it never folds the section.
inline bool EnableHeader(const char* stable, bool* enabled, bool* changed, const char* enableTooltip)
{
    const auto hidden = HiddenLabel(stable);
    const std::string_view label(stable);
    const auto visible = label.substr(0, label.find("##"));
    const auto origin = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetFrameHeight(), width = std::max(height * 2, ImGui::GetContentRegionAvail().x);
    const ImGuiID stateKey = ImGui::GetID(hidden.c_str());
    auto* storage = ImGui::GetStateStorage();
    bool open = storage->GetInt(stateKey, 0) != 0;
    ImGui::GetWindowDrawList()->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
        ImGui::GetColorU32(ImGuiCol_Header), ImGui::GetStyle().FrameRounding);
    ImGui::PushID(hidden.c_str());
    const float disclosureWidth = width - SettingsUi::ToggleSize().x - SettingsUi::ToggleGap();
    if (ImGui::InvisibleButton("##expand", ImVec2(disclosureWidth, height), ImGuiButtonFlags_EnableNav))
    {
        open = !open;
        storage->SetInt(stateKey, open);
    }
    const bool headerHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    auto* draw = ImGui::GetWindowDrawList();
    ImGui::RenderArrow(draw, ImVec2(origin.x + ImGui::GetStyle().FramePadding.x, origin.y + ImGui::GetStyle().FramePadding.y),
                       ImGui::GetColorU32(ImGuiCol_Text), open ? ImGuiDir_Down : ImGuiDir_Right);
    const ImVec2 textMin(origin.x + ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x * 2, origin.y);
    const ImVec2 textMax(origin.x + disclosureWidth, origin.y + height);
    draw->PushClipRect(textMin, textMax, true);
    ImGui::RenderTextClipped(textMin, textMax, visible.data(), visible.data() + visible.size(), nullptr, ImVec2(0, .5f));
    draw->PopClipRect();
    ImGui::SameLine(0, SettingsUi::ToggleGap());
    *changed = SettingsUi::ToggleSwitch("##enabled", enabled);
    if (headerHovered)
        ImGui::SetTooltip("%.*s\n%s", int(visible.size()), visible.data(), enableTooltip);
    else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", enableTooltip);
    ImGui::PopID();
    return open;
}

template<class T, class NativeSlider>
inline bool Render(const char* /*source*/, const char* stable, T* value, T low, T high,
                   const char* format, ImGuiSliderFlags flags, NativeSlider native, const char* zeroText = nullptr)
{
    if (ImGui::GetCurrentWindow()->SkipItems) return false;
    const auto hidden = HiddenLabel(stable);
    const std::string_view label(stable);
    const auto visible = label.substr(0, label.find("##"));
    const float font = ImGui::GetFontSize();
    const auto origin = ImGui::GetCursorScreenPos();
    const auto& style = ImGui::GetStyle();
    const auto row = Layout(!visible.empty(), NumberWidth(*value, low, high, format, zeroText));
    const float gap = row.gap, sliderWidth = row.rail, numberWidth = row.number, height = row.height;
    const bool labelHelpHovered = RowLabel(visible, row);

    const ImGuiID id = ImGui::GetID(hidden.c_str());
    const bool precise = ImGui::GetIO().KeyCtrl && !ImGui::TempInputIsActive(id);
    const bool nativeInput = ImGui::TempInputIsActive(id);
    const bool decorate = !(flags & ImGuiSliderFlags_Logarithmic) && !nativeInput;
    if (!nativeInput) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
    if (decorate)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, font * .48f);
        for (const auto colour : { ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive,
                                  ImGuiCol_SliderGrab, ImGuiCol_SliderGrabActive })
            ImGui::PushStyleColor(colour, ImVec4(0, 0, 0, 0));
    }
    ImGui::SetNextItemWidth(sliderWidth);
    bool changed;
    if (precise)
    {
        const T before = *value;
        // Ctrl is an actual relative fine drag (one pixel/key step), not Ctrl-click text input.
        // Numeric editing remains available in the adjacent hover editor.
        const ImGuiSliderFlags fineFlags = (flags & ~ImGuiSliderFlags_Logarithmic) | ImGuiSliderFlags_NoInput |
                                           ImGuiSliderFlags_NoSpeedTweaks | ImGuiSliderFlags_NoRoundToFormat;
        ImGui::PushID(hidden.c_str());
        const ImGuiID frameKey = ImGui::GetID("fine-frame");
        ImGui::PopID();
        auto* storage = ImGui::GetStateStorage();
        if (storage->GetInt(frameKey, -2) != ImGui::GetFrameCount() - 1 && GImGui->ActiveId == id)
        {
            GImGui->DragCurrentAccum = 0.f;
            GImGui->DragCurrentAccumDirty = false;
        }
        storage->SetInt(frameKey, ImGui::GetFrameCount());
        if constexpr (std::is_same_v<T, float>)
        {
            const bool dragged = ImGui::DragFloat(hidden.c_str(), value, .05f, low, high, "%.6f", fineFlags);
            if (dragged && std::isfinite(*value) && std::isfinite(before))
                *value = std::clamp(before + std::round((*value - before) / .05f) * .05f, std::min(low, high), std::max(low, high));
        }
        else
            ImGui::DragInt(hidden.c_str(), value, 1.f, low, high, format, fineFlags);
        if (GImGui->ActiveId == id && GImGui->ActiveIdSource == ImGuiInputSource_Keyboard &&
            !(GImGui->LastItemData.ItemFlags & (ImGuiItemFlags_Disabled | ImGuiItemFlags_ReadOnly)) &&
            !(flags & ImGuiSliderFlags_ReadOnly))
        {
            // Explicitly define the keyboard contract rather than inheriting range/format speed.
            const float steps = ImGui::GetNavTweakPressedAmount(ImGuiAxis_X);
            const T step = std::is_same_v<T, float> ? T(.05f) : T(1);
            if (steps != 0.f)
                *value = std::clamp(T(before + T(steps) * step), std::min(low, high), std::max(low, high));
        }
        changed = before != *value;
        if (changed) ImGui::MarkItemEdited(id);
    }
    else
        changed = native(hidden.c_str(), value, low, high, format, flags);
    if (decorate) { ImGui::PopStyleColor(5); ImGui::PopStyleVar(2); }
    if (!nativeInput) ImGui::PopStyleColor();
    auto sliderItem = GImGui->LastItemData;
    const bool sliderHelpHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    const bool sliderReleased = ImGui::IsItemDeactivatedAfterEdit();
    if (decorate)
    {
        const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
        const float inset = std::min(font * .6f, sliderWidth * .25f);
        const float left = a.x + inset, right = std::max(left, b.x - inset);
        const double span = double(high) - double(low);
        const float ratio = span > 0 ? float((double(*value) - double(low)) / span) : 0.f;
        const float t = std::isfinite(ratio) ? std::clamp(ratio, 0.f, 1.f) : 0.f;
        const float x = left + (right - left) * t, cy = (a.y + b.y) * .5f;
        const float thickness = font * (active || hovered ? .16f : .12f);
        const auto accent = ImGui::GetStyleColorVec4(ImGuiCol_SliderGrab);
        const auto rail = ImGui::GetStyleColorVec4(ImGuiCol_Border);
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(ImVec2(left, cy - thickness * .5f), ImVec2(x, cy + thickness * .5f), ImGui::GetColorU32(accent), thickness * .5f);
        draw->AddRectFilled(ImVec2(x, cy - thickness * .5f), ImVec2(right, cy + thickness * .5f), ImGui::GetColorU32(rail), thickness * .5f);
        const float radius = std::min(font * (active ? .36f : hovered ? .33f : .30f), sliderWidth * .25f);
        draw->AddCircleFilled(ImVec2(x, cy), radius, ImGui::GetColorU32(active ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab));
        draw->AddCircle(ImVec2(x, cy), radius, ImGui::GetColorU32(ImGuiCol_Border));
    }

    if (row.numberBelow)
    {
        // Keep the number and reset together below the full rail in a narrow window.
        const float y = ImGui::GetCursorScreenPos().y;
        ImGui::SetCursorScreenPos(ImVec2(origin.x + row.width - numberWidth, y));
    }
    else
        ImGui::SameLine(0, gap);
    // Namespace only the added editor. The original rail's ID and caller PushID scopes stay intact.
    ImGui::PushID(hidden.c_str());
    constexpr ImGuiDataType type = std::is_same_v<T, float> ? ImGuiDataType_Float : ImGuiDataType_S32;
    const T previous = *value;
    const ImGuiInputTextFlags inputFlags = (flags & (ImGuiSliderFlags_NoInput | ImGuiSliderFlags_ReadOnly))
        ? ImGuiInputTextFlags_ReadOnly : 0;
    // The numeric editor uses the same undecorated precision as Ctrl+click on
    // the native rail; prefixes such as "dropped %.0f%%" must not enter its parser.
    char numericFormat[64] {};
    const char* editFormat = ImParseFormatTrimDecorations(format, numericFormat, IM_ARRAYSIZE(numericFormat));
    if (!*editFormat) editFormat = ImGui::DataTypeGetInfo(type)->PrintFmt;
    const ImVec2 numberMin = ImGui::GetCursorScreenPos(), numberMax(numberMin.x + numberWidth, numberMin.y + height);
    const ImGuiID numberId = ImGui::GetID("##number");
    const bool editing = GImGui->ActiveId == numberId;
    const bool hovered = ImGui::IsMouseHoveringRect(numberMin, numberMax) &&
                         ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    char numericText[128] {};
    ImGui::DataTypeFormatString(numericText, IM_ARRAYSIZE(numericText), type, value, editFormat);
    const auto* inputState = editing ? ImGui::GetInputTextState(numberId) : nullptr;
    const char* activeText = inputState && inputState->TextA.Size ? inputState->TextA.Data : numericText;
    const float textWidth = ImGui::CalcTextSize(activeText).x;
    const ImVec2 originalPadding = style.FramePadding;
    const float centredPadding = std::max(originalPadding.x, (numberWidth - textWidth) * .5f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(centredPadding, originalPadding.y));
    if (!hovered && !editing)
        for (auto colour : {ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_Border, ImGuiCol_Text})
            ImGui::PushStyleColor(colour, ImVec4(0, 0, 0, 0));
    ImGui::SetNextItemWidth(numberWidth);
    const ImGuiInputTextFlags centredFlags = textWidth < numberWidth - originalPadding.x * 2 ? ImGuiInputTextFlags_NoHorizontalScroll : 0;
    const bool numberChanged = ImGui::InputScalar("##number", type, value, nullptr, nullptr, editFormat, inputFlags | centredFlags);
    if (!hovered && !editing) ImGui::PopStyleColor(5);
    ImGui::PopStyleVar();
    if (!hovered && !editing)
    {
        char display[128] {};
        ImGui::DataTypeFormatString(display, IM_ARRAYSIZE(display), type, value, format);
        const char* text = zeroText && *value == T(0) ? zeroText : display;
        ImGui::RenderTextClipped(numberMin, numberMax, text, nullptr, nullptr, ImVec2(.5f, .5f));
    }
    if (numberChanged && (flags & ImGuiSliderFlags_ClampOnInput))
    {
        const T minimum = std::min(low, high), maximum = std::max(low, high);
        ImGui::DataTypeClamp(type, value, &minimum, &maximum);
    }
    const bool numberReleased = ImGui::IsItemDeactivatedAfterEdit();
    const bool numberOwnsItem = ImGui::IsItemActive() || numberReleased || numberChanged;
    const auto numberItem = GImGui->LastItemData;
    const bool numberHelpHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        char formatted[128] {};
        ImGui::DataTypeFormatString(formatted, IM_ARRAYSIZE(formatted), type, value, format);
        ImGui::SetTooltip("%s", formatted);
    }
    ImGui::PopID();
    changed |= numberChanged && previous != *value;
    // Callers read release/edit state immediately after the wrapper. Keep the original
    // slider release if one happened while focus moved into the number editor.
    GImGui->LastItemData = numberOwnsItem && !sliderReleased ? numberItem : sliderItem;
    lastRow = { GImGui, ImGui::GetCurrentWindow(), ImGui::GetFrameCount(), GImGui->LastItemData.ID, numberMin.y };
    RememberHelpTarget(labelHelpHovered || sliderHelpHovered || numberHelpHovered);
    return changed;
}
} // namespace NextgenSliderStyle
