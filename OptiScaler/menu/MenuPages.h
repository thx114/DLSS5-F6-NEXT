#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

// NEXT: page labels are user-facing; simplified Chinese needs UTF-8 narrow literals.
#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

// Pages of the main menu's navigation pane. Pure data, no ImGui, so it can be unit tested on the host.
namespace MenuPages
{
// Keep historical page values and saved names stable. NavigationPage resolves their new owner.
enum class Page : unsigned char
{
    Upscaler,
    FrameGeneration,
    Image,
    Framerate,
    OverlayLook,
    Input,
    Misc,
    NrF5Low, // stable historical identity; displayed last under Neural Rendering
    NrStatus,
    NrOptions,
    NrInput,
    NrOutput,
    NrPasses,
    NrDebug,
    NrLayer1,
    NrLayer2,
    NrUnsorted,
    Hdr,
    Debug,
    Diagnostics, // append without changing existing saved identities
    NrReuse, // append; preserve every existing enum and saved page identity
    Developer, // append; menu-only developer visibility controls
    NrLayer3, // append; formal third-layer page
    ProGimi, // append; preserve existing page values and saved names
    ProUidAuthorization,
    ProFilters, // append; retain all historical Page values and INI names
    FrameGenerationAdvanced,
};

struct PageInfo
{
    const char* name;  // stable ini name, stored in [Menu] MenuPage
    const char* label; // shown in the nav pane
    const char* english;
};

// Same order as Page.
inline constexpr PageInfo kPages[] = {
    { "upscaler", "超分", "Upscaler" },
    { "framegen", "帧生成", "Frame generation" },
    { "image", "图像", "Image" },
    { "framerate", "帧率", "Framerate" },
    { "overlay", "界面与外观", "Interface & appearance" },
    { "input", "快捷键", "Keybindings" },
    { "misc", "杂项", "Miscellaneous" },
    { "nr.f5low", "光学 F5Low", "Optical F5Low" },
    { "nr.status", "全局", "Global" },
    { "nr.options", "NR 选项", "NR options" },
    { "nr.input", "NR 输入", "NR input" },
    { "nr.output", "NR 输出", "NR output" },
    { "nr.passes", "NR 通道与色彩", "Passes & color" },
    { "nr.debug", "对比与调试", "Compare & debug" },
    { "nr.layer1", "第一层", "Layer 1" },
    { "nr.layer2", "第二层", "Layer 2" },
    { "nr.unsorted", "NR 未整理", "NR unsorted" },
    { "hdr", "HDR", "HDR" },
    { "debug", "调试", "Debug" },
    { "diagnostics", "诊断与性能", "Diagnostics / performance" },
    { "nr.reuse", "多帧NR复用", "Multi-frame NR reuse" },
    { "developer", "开发者", "Developer" },
    { "nr.layer3", "第三层", "Layer 3" },
    { "pro.gimi", "GIMI", "GIMI" },
    { "pro.uid", "UID与授权", "UID & authorization" },
    { "pro.filters", "HoYoShade 滤镜", "HoYoShade filters" },
    { "framegen.advanced", "高级", "Advanced" },
};

inline constexpr size_t kPageCount = sizeof(kPages) / sizeof(kPages[0]);

inline constexpr bool IsNeuralRendering(Page page) { return (page >= Page::NrF5Low && page <= Page::NrUnsorted) || page == Page::NrReuse || page == Page::Hdr || page == Page::NrLayer3; }

// Preserve enum values and INI names; legacy requests resolve to their new owning tab.
inline constexpr Page NavigationPage(Page page)
{
    switch (page)
    {
    case Page::NrOutput: return Page::NrStatus;
    case Page::NrInput: return Page::NrStatus;
    case Page::NrPasses: return Page::NrLayer1;
    case Page::NrOptions: return Page::NrStatus;
    case Page::NrDebug: return Page::Debug;
    default: return page;
    }
}
// Explicit allowlist only; unclassified/core controls remain visible.
enum class Category : unsigned char { Core, Unfinished, Nonessential };
struct Visibility { bool unfinished = false, nonessential = false; };
inline Visibility& MenuVisibility() { static Visibility value; return value; }
inline bool Show(Category category)
{
    return category == Category::Core || (category == Category::Unfinished
        ? MenuVisibility().unfinished : MenuVisibility().nonessential);
}
inline constexpr bool IsVisiblePage(Page page, bool nonessential = MenuVisibility().nonessential)
{
    return NavigationPage(page) == page && (page != Page::Diagnostics || nonessential);
}

enum class Group : unsigned char { Sr, Nr, Fg, Pro, Settings };
inline constexpr size_t kGroupCount = 5;

// Display order is independent of stable enum/INI identities.
inline constexpr Page kNavigationPages[] = {
    Page::Upscaler, Page::Image,
    Page::NrStatus, Page::NrLayer1, Page::NrLayer2, Page::NrLayer3, Page::NrUnsorted, Page::NrReuse, Page::NrF5Low, Page::Hdr,
    Page::FrameGeneration, Page::FrameGenerationAdvanced, Page::Framerate,
    Page::ProGimi, Page::ProUidAuthorization, Page::ProFilters,
    Page::OverlayLook, Page::Input, Page::Misc, Page::Debug, Page::Diagnostics, Page::Developer,
};

inline constexpr Group GroupOf(Page page)
{
    page = NavigationPage(page);
    if (page == Page::ProGimi || page == Page::ProUidAuthorization || page == Page::ProFilters) return Group::Pro;
    if (IsNeuralRendering(page))
        return Group::Nr;
    if (page == Page::FrameGeneration || page == Page::FrameGenerationAdvanced || page == Page::Framerate)
        return Group::Fg;
    if (page == Page::Upscaler || page == Page::Image)
        return Group::Sr;
    return Group::Settings;
}

inline constexpr Page DefaultPage(Group group)
{
    switch (group)
    {
    case Group::Pro: return Page::ProGimi;
    case Group::Nr: return Page::NrStatus;
    case Group::Fg: return Page::FrameGeneration;
    case Group::Settings: return Page::OverlayLook;
    default: return Page::Upscaler;
    }
}

inline constexpr const PageInfo& Info(Page page)
{
    const size_t index = static_cast<size_t>(page);
    return kPages[index < kPageCount ? index : 0];
}

inline constexpr const char* Name(Page page) { return Info(page).name; }
inline constexpr const char* Label(Page page) { return Info(page).label; }

// Missing or unknown names fall back to Upscaler.
inline constexpr Page PageFromName(std::string_view name)
{
    if (name == "nr.unsorted") return Page::NrLayer3;
    for (size_t i = 0; i < kPageCount; ++i)
    {
        if (name == kPages[i].name)
            return static_cast<Page>(i);
    }

    return Page::Upscaler;
}

// A page another part of the menu wants shown (the header's Optical F5Low button, cross-links). The page choice itself
// is a static inside MenuCommon::RenderMainMenuPages, so the request waits here until that function takes it, once, on
// its next draw.
inline std::optional<Page>& PendingRequest()
{
    static std::optional<Page> request;
    return request;
}

inline void RequestPage(Page page) { PendingRequest() = page; }

// The requested page, once: a second call returns nothing until the next RequestPage.
inline std::optional<Page> ConsumeRequest()
{
    const std::optional<Page> request = PendingRequest();
    PendingRequest().reset();
    return request;
}
} // namespace MenuPages
