#pragma once
#include "UiLocalization.h"
#include "MenuPages.h"
#include <imgui/imgui.h>

namespace NextgenNrLayers
{
// Read-only UI query; keeps empty layer tabs out of navigation without changing configuration.
bool IsVisible(unsigned int layer);

inline bool IsVisiblePage(MenuPages::Page page)
{
    using MenuPages::Page;
    switch (MenuPages::NavigationPage(page))
    {
    case Page::NrLayer1: return IsVisible(1u);
    case Page::NrLayer2: return IsVisible(2u);
    case Page::NrLayer3: return IsVisible(3u);
    case Page::NrUnsorted: return IsVisible(4u);
    default: return true;
    }
}

inline void UnavailableDetails()
{
    bool enabled = false;
    ImGui::BeginDisabled();
    MenuText::Checkbox("启用本层独立细节", &enabled);
    ImGui::EndDisabled();
    MenuText::TextWrapped("待实现：当前细节调整仅支持全局最终合成；共享细节设置保留在 NR 未整理，模型分辨率在 NR 全局，共享上采样在第一层。 ");
}

template <typename DrawControls> bool SecondLayerControls(DrawControls drawControls)
{
    const ImGuiID key = ImGui::GetID("nr-layer2-editor-open");
    auto* storage = ImGui::GetStateStorage();
    bool open = storage->GetBool(key, false);
    if (MenuText::Checkbox("启用本层独立控制", &open))
        storage->SetBool(key, open);
    MenuText::TextWrapped("此开关仅展开编辑。关闭时保留已有第二层参数及其运行效果；重置单项可恢复继承。");
    if (open)
        drawControls();
    return open;
}
} // namespace NextgenNrLayers
