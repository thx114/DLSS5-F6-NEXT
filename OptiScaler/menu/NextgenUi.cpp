#include "pch.h"
#include "NextgenUi.h"
#include "NextgenNrLayers.h"

#include <Config.h>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include <algorithm>
#include <array>
#include <string>

// NEXT nav labels are simplified Chinese; keep narrow literals UTF-8 on any system codepage.
#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

namespace NextgenUi
{
namespace
{
constexpr int kStyleColorCount = 35;
constexpr int kStyleVarCount = 10;

ImVec4 Alpha(ImVec4 color, float alpha)
{
    color.w = alpha;
    return color;
}

const char* Label(const char* chinese, const char* english)
{
    if (English())
        return english;
    for (const char* p = chinese; *p;)
    {
        unsigned int code = 0;
        const int length = ImTextCharFromUtf8(&code, p, nullptr);
        if (length <= 0 || !ImGui::GetFont()->IsGlyphInFont(static_cast<ImWchar>(code)))
            return english;
        p += length;
    }
    return chinese;
}

bool Tab(const char* id, const char* label, bool selected, float width, float height, bool primary = false)
{
    const ImVec2 low = ImGui::GetCursorScreenPos();
    const ImVec2 high(low.x + width, low.y + height);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    auto* draw = ImGui::GetWindowDrawList();
    const float rounding = std::min({ImGui::GetFontSize() * .28f, width * .25f, height * .25f});
    if (selected || ImGui::IsItemHovered())
        draw->AddRectFilled(low, high,
            ImGui::GetColorU32(selected ? (primary ? ImGuiCol_Header : ImGuiCol_WindowBg) : ImGuiCol_HeaderHovered),
            rounding);
    if (selected && !primary)
        draw->AddRect(low, high, ImGui::GetColorU32(ImGuiCol_Border), rounding);
    if (selected && primary)
        draw->AddLine(ImVec2(low.x + rounding, high.y - 1.0f), ImVec2(high.x - rounding, high.y - 1.0f),
                      ImGui::GetColorU32(ImGuiCol_CheckMark), std::max(1.0f, ImGui::GetFontSize() * .11f));
    if (ImGui::IsItemFocused())
        draw->AddRect(low, high, ImGui::GetColorU32(ImGuiCol_NavCursor), rounding);

    const ImVec2 extent = ImGui::CalcTextSize(label);
    // Large fonts and very narrow windows must not paint over neighboring tabs.
    draw->PushClipRect(low, high, true);
    draw->AddText(ImVec2(low.x + std::max(2.0f, (width - extent.x) * 0.5f),
                         low.y + (height - extent.y) * 0.5f),
                  ImGui::GetColorU32(selected ? ImGuiCol_Text : ImGuiCol_TextDisabled), label);
    draw->PopClipRect();
    if (extent.x > width - 4.0f && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", label);
    return clicked;
}

void SecondaryTabs(MenuPages::Page currentPage, MenuPages::Group group, bool revealCurrent)
{
    std::array<MenuPages::Page, MenuPages::kPageCount> pages {};
    std::array<float, MenuPages::kPageCount> widths {};
    size_t count = 0;
    float textWidth = 0.0f;
    for (const auto page : MenuPages::kNavigationPages)
    {
        if (MenuPages::GroupOf(page) != group || !MenuPages::IsVisiblePage(page))
            continue;
        if (!NextgenNrLayers::IsVisiblePage(page))
            continue;
        pages[count] = page;
        widths[count] = ImGui::CalcTextSize(Label(MenuPages::Label(page), MenuPages::Info(page).english)).x;
        textWidth += widths[count++];
    }

    if (count == 0) return;
    const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float font = ImGui::GetFontSize(), height = ImGui::GetFrameHeight(), gap = std::max(1.0f, font * .16f);
    const float padding = std::clamp((available - gap * (count - 1) - textWidth) / count,
                                     font * 0.35f, font * 1.2f);
    float totalWidth = gap * (count - 1);
    for (size_t i = 0; i < count; ++i)
    {
        widths[i] = std::min(available, widths[i] + padding);
        totalWidth += widths[i];
    }
    const bool overflow = totalWidth > available + 0.1f;
    if (!overflow)
    {
        const float extra = std::max(0.0f, available - totalWidth) / count;
        for (size_t i = 0; i < count; ++i) widths[i] += extra;
        totalWidth = available;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 0));
    ImGui::SetNextWindowContentSize(ImVec2(totalWidth, height));
    ImGui::BeginChild("##secondary-strip", ImVec2(available, height + (overflow ? ImGui::GetStyle().ScrollbarSize : 0.0f)),
                       ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoBackground);
    const auto strip = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(strip, ImVec2(strip.x + totalWidth, strip.y + height),
        ImGui::GetColorU32(ImGuiCol_FrameBg), font * .35f);
    for (size_t i = 0; i < count; ++i)
    {
        if (i)
            ImGui::SameLine();
        ImGui::PushID(static_cast<int>(pages[i]));
        if (Tab("##secondary-tab", Label(MenuPages::Label(pages[i]), MenuPages::Info(pages[i]).english),
                pages[i] == currentPage, widths[i], height) && pages[i] != currentPage)
            MenuPages::RequestPage(pages[i]);
        if (revealCurrent && pages[i] == currentPage)
            ImGui::SetScrollHereX(0.5f);
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}
} // namespace

StyleScope::StyleScope(bool lightTheme)
{
    // Font-relative rhythm applies to the real pages while retaining the user's light/dark choice.
    const ImVec4 accent = lightTheme ? ImVec4(0.56f, 0.38f, 0.14f, 1.0f)
                                    : ImVec4(0.91f, 0.67f, 0.30f, 1.0f);
    const ImVec4 accentHover = lightTheme ? ImVec4(0.48f, 0.31f, 0.10f, 1.0f)
                                         : ImVec4(0.98f, 0.75f, 0.38f, 1.0f);
    const ImVec4 button = lightTheme ? ImVec4(0.86f, 0.84f, 0.78f, 1.0f)
                                    : ImVec4(0.18f, 0.19f, 0.18f, 1.0f);

    const auto base = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const auto text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    const ImVec4 field(base.x + (text.x - base.x) * .055f, base.y + (text.y - base.y) * .055f,
                       base.z + (text.z - base.z) * .055f, base.w);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, field);
    ImGui::PushStyleColor(ImGuiCol_Border, Alpha(text, lightTheme ? .28f : .20f));
    ImGui::PushStyleColor(ImGuiCol_Header, Alpha(accent, lightTheme ? 0.12f : 0.16f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Alpha(accentHover, lightTheme ? 0.10f : 0.14f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, Alpha(accent, lightTheme ? 0.22f : 0.26f));
    ImGui::PushStyleColor(ImGuiCol_Button, Alpha(button, 0.72f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Alpha(accentHover, lightTheme ? 0.24f : 0.34f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Alpha(accent, lightTheme ? 0.38f : 0.48f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Alpha(accentHover, lightTheme ? 0.15f : 0.22f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Alpha(accent, lightTheme ? 0.24f : 0.32f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, accent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, Alpha(accent, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, accentHover);
    ImGui::PushStyleColor(ImGuiCol_Separator, Alpha(accent, lightTheme ? 0.24f : 0.32f));
    // Native widgets nested inside custom sheets must use the same palette:
    // combo menus, tooltips, tab bars, table headers and keyboard selection.
    ImGui::PushStyleColor(ImGuiCol_Tab, field);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, Alpha(accentHover, lightTheme ? .20f : .28f));
    ImGui::PushStyleColor(ImGuiCol_TabSelected, Alpha(accent, lightTheme ? .16f : .24f));
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, accent);
    ImGui::PushStyleColor(ImGuiCol_TabDimmed, field);
    ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, Alpha(accent, lightTheme ? .10f : .16f));
    ImGui::PushStyleColor(ImGuiCol_TabDimmedSelectedOverline, Alpha(accent, .55f));
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, field);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, Alpha(accent, lightTheme ? .22f : .32f));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, accentHover);
    ImGui::PushStyleColor(ImGuiCol_InputTextCursor, accentHover);
    ImGui::PushStyleColor(ImGuiCol_TextLink, accentHover);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, Alpha(accent, lightTheme ? .40f : .45f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, Alpha(accentHover, .65f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, accentHover);
    ImGui::PushStyleColor(ImGuiCol_SeparatorHovered, Alpha(accentHover, .65f));
    ImGui::PushStyleColor(ImGuiCol_SeparatorActive, accentHover);
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip, Alpha(accent, .25f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, Alpha(accentHover, .65f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, accentHover);
    ImGui::PushStyleColor(ImGuiCol_TreeLines, Alpha(text, lightTheme ? .20f : .25f));

    const float font = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, font * .4f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, font * .3f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, font * .28f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, font * .28f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(font * .45f, font * .22f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(font * .5f, font * .3f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(font * .75f, font * .55f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(font * .35f, font * .22f));
    ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, font);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, font * .65f);
}

StyleScope::~StyleScope()
{
    ImGui::PopStyleVar(kStyleVarCount);
    ImGui::PopStyleColor(kStyleColorCount);
}

void RenderHeader(MenuPages::Page currentPage)
{
    using MenuPages::Group;
    using MenuPages::Page;
    currentPage = MenuPages::NavigationPage(currentPage);
    if (!NextgenNrLayers::IsVisiblePage(currentPage))
    {
        currentPage = Page::NrStatus;
        MenuPages::RequestPage(currentPage);
    }
    const float available = ImGui::GetContentRegionAvail().x;
    const Group selected = MenuPages::GroupOf(currentPage);
    ImGui::PushID("nextgen-navigation");
    // Window-owned state avoids sharing remembered pages across ImGui contexts/windows.
    auto* storage = ImGui::GetStateStorage();
    const char* keys[] = { "last-sr", "last-nr", "last-fg", "last-pro", "last-settings" };
    static_assert(IM_ARRAYSIZE(keys) == MenuPages::kGroupCount);
    std::array<ImGuiID, MenuPages::kGroupCount> rememberedKeys {};
    for (size_t i = 0; i < rememberedKeys.size(); ++i)
        rememberedKeys[i] = ImGui::GetID(keys[i]);
    storage->SetInt(rememberedKeys[static_cast<size_t>(selected)], static_cast<int>(currentPage));
    const ImGuiID pageKey = ImGui::GetID("last-page");
    const ImGuiID widthKey = ImGui::GetID("last-width");
    const ImGuiID fontKey = ImGui::GetID("last-font");
    const ImGuiID languageKey = ImGui::GetID("last-language");
    const bool revealCurrent = storage->GetInt(pageKey, -1) != static_cast<int>(currentPage) ||
                               storage->GetFloat(widthKey, -1.0f) != available ||
                               storage->GetFloat(fontKey, -1.0f) != ImGui::GetFontSize() ||
                               storage->GetInt(languageKey, -1) != static_cast<int>(English());
    storage->SetInt(pageKey, static_cast<int>(currentPage));
    storage->SetFloat(widthKey, available);
    storage->SetFloat(fontKey, ImGui::GetFontSize());
    storage->SetInt(languageKey, static_cast<int>(English()));

    const char* labels[] = { "SR", "NR", "FG", "Pro", Label("设置", "Settings") };
    static_assert(IM_ARRAYSIZE(labels) == MenuPages::kGroupCount);
    const float gap = std::max(1.0f, ImGui::GetFontSize() * .22f);
    const float width = std::max(1.0f, (available - gap * (MenuPages::kGroupCount - 1)) / MenuPages::kGroupCount);
    for (size_t i = 0; i < MenuPages::kGroupCount; ++i)
    {
        if (i)
            ImGui::SameLine(0, gap);
        ImGui::PushID(static_cast<int>(i));
        if (Tab("##primary-tab", labels[i], selected == static_cast<Group>(i), width, ImGui::GetFontSize() * 1.75f, true) &&
            selected != static_cast<Group>(i))
        {
            const Group group = static_cast<Group>(i);
            Page target = static_cast<Page>(storage->GetInt(rememberedKeys[i], static_cast<int>(MenuPages::DefaultPage(group))));
            if (static_cast<size_t>(target) >= MenuPages::kPageCount || MenuPages::GroupOf(target) != group ||
                !MenuPages::IsVisiblePage(target) || !NextgenNrLayers::IsVisiblePage(target))
                target = MenuPages::DefaultPage(group);
            MenuPages::RequestPage(MenuPages::NavigationPage(target));
        }
        ImGui::PopID();
    }
    SecondaryTabs(currentPage, selected, revealCurrent);
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * .12f));
}

void FooterSameLineIfFits(float nextWidth, float spacing)
{
    const float gap = spacing < 0.0f ? ImGui::GetStyle().ItemSpacing.x : spacing;
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (ImGui::GetItemRectMax().x + gap + nextWidth <= right)
        ImGui::SameLine(0.0f, gap);
}

float FooterLanguageWidth()
{
    return ImGui::CalcTextSize("F6").x + ImGui::CalcTextSize("CN").x + ImGui::CalcTextSize("EN").x +
           ImGui::CalcTextSize(Label("深色", "Dark")).x + ImGui::CalcTextSize(Label("浅色", "Light")).x +
           8.0f * ImGui::GetStyle().FramePadding.x + 4.0f * ImGui::GetStyle().ItemSpacing.x;
}

bool RenderFooterLanguage()
{
    // Compact language names keep both theme choices inside the second footer
    // row at the supported minimum width. Original zh/en identities are retained.
    const char* labels[] = { "CN###zh", "EN###en" };
    bool themeChanged = false;
    ImGui::PushID("nextgen-footer-language");
    ImGui::BeginGroup();
    // Keep the variant visible with the menu open, without changing any window/key IDs.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("F6");
    for (int i = 0; i < IM_ARRAYSIZE(labels); ++i)
    {
        const float width = ImGui::CalcTextSize(labels[i], nullptr, true).x +
                            2.0f * ImGui::GetStyle().FramePadding.x;
        FooterSameLineIfFits(width);
        const bool selected = English() == (i == 1);
        const char* text = i == 0 ? "CN" : "EN";
        if (Tab(labels[i], text, selected, width, ImGui::GetFrameHeight()) && !selected)
            SetEnglish(i == 1);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Label("切换整个 F6 的显示语言；点击保存设置后记住选择。",
                                        "Change the F6 display language. Use Save Settings to remember the choice."));
    }
    auto* config = Config::Instance();
    for (int i = 0; i < 2; ++i)
    {
        const char* text = i == 0 ? Label("深色", "Dark") : Label("浅色", "Light");
        const float width = ImGui::CalcTextSize(text).x + 2.0f * ImGui::GetStyle().FramePadding.x;
        FooterSameLineIfFits(width);
        const bool selected = config->LightTheme.value_or_default() == (i == 1);
        if (Tab(i == 0 ? "##dark-theme" : "##light-theme", text, selected, width, ImGui::GetFrameHeight()) && !selected)
        {
            config->LightTheme = i == 1;
            themeChanged = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Label("立即切换浅色／深色；点击保存设置后记住选择。",
                                        "Change light/dark appearance now. Use Save Settings to remember the choice."));
    }
    ImGui::EndGroup();
    ImGui::PopID();
    return themeChanged;
}

ImVec4 ReadableTextColor(const ImVec4& colour)
{
    if (!Config::Instance()->LightTheme.value_or_default()) return colour;
    const auto background = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    if (background.x * .2126f + background.y * .7152f + background.z * .0722f <= .5f)
        return colour; // Explicitly dark overlays retain their bright status text.
    // Existing status hues were chosen for dark backgrounds. Lower their
    // luminance for light cards, popups and disabled text without changing hue
    // or caller alpha; dark mode and non-text graphics keep their own palette.
    const float luminance = colour.x * .2126f + colour.y * .7152f + colour.z * .0722f;
    const float scale = luminance > .28f ? .28f / luminance : 1.0f;
    return ImVec4(colour.x * scale, colour.y * scale, colour.z * scale, colour.w);
}

bool English()
{
    return Config::Instance()->MenuLanguage.value_or_default() == "en";
}

void SetEnglish(bool english)
{
    Config::Instance()->MenuLanguage = std::string(english ? "en" : "zh");
}
} // namespace NextgenUi
