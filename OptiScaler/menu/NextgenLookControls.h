#pragma once
#include "UiLocalization.h"
#include <dlssnr/LayerLookProfile.h>
#include <algorithm>
#include <cmath>

// UI-only allowlist. Config remains owned by F5 and the existing Live Look consumer.
// A template lets the headless fixture compile this exact renderer without GPU/provider headers.
namespace NextgenLookControls
{
inline bool EnableCheckbox(const char* label, bool* enabled)
{
    const std::string stable = std::string(label) + "###f5LookEnabled";
    return MenuText::Checkbox(stable.c_str(), enabled);
}

template<class ConfigType> inline void Render(ConfigType* config)
{
    const auto tr = [](const char* cn, const char* en) { return NextgenUi::English() ? en : cn; };
    MenuText::SeparatorText(tr("第一层 Look", "Layer 1 Look"));
    if (!ImGui::CollapsingHeader(tr("第一层 Look 参数###f5Look", "Layer 1 Look parameters###f5Look"),
        ImGuiTreeNodeFlags_DefaultOpen)) return;

    struct Control
    {
        const char* id;
        const char* cn;
        const char* en;
        decltype(config->DlssNrLookStrength)* option;
        float low, high, neutral;
        bool unlimited = false;
    };
    const Control controls[] = {
        { "LookStrength", "效果强度", "Effect strength", &config->DlssNrLookStrength, 0.00f, 2.00f, 1.00f },
        { "LookBrighten", "提亮", "Brightening", &config->DlssNrLookBrighten, 0.00f, 2.00f, 1.00f },
        { "LookDarken", "压暗", "Darkening", &config->DlssNrLookDarken, 0.00f, 2.00f, 1.00f },
        { "LookColour", "色彩", "Colour", &config->DlssNrLookColour, 0.00f, 2.00f, 1.00f },
        { "LookHue", "色相", "Hue", &config->DlssNrLookHue, 0.00f, 2.00f, 1.00f },
        { "LookShadows", "阴影", "Shadows", &config->DlssNrLookShadows, 0.00f, 2.00f, 1.00f },
        { "LookMidtones", "中间调", "Midtones", &config->DlssNrLookMidtones, 0.00f, 2.00f, 1.00f },
        { "LookHighlights", "高光", "Highlights", &config->DlssNrLookHighlights, 0.00f, 2.00f, 1.00f },
        { "LookMaxBrighten", "最大提亮", "Brightening ceiling", &config->DlssNrLookMaxBrighten, 0.00f, 4.00f, 0.00f, true },
        { "LookMaxDarken", "最大压暗", "Darkening floor", &config->DlssNrLookMaxDarken, 0.00f, 4.00f, 0.00f, true },
        { "LookMaxColour", "色彩软限制", "Colour soft limit", &config->DlssNrLookMaxColour, 0.00f, 2.00f, 0.00f, true },
        { "LookTone", "大尺度光照", "Broad lighting", &config->DlssNrLookTone, 0.00f, 2.00f, 1.00f },
        { "LookDetail", "细节", "Detail", &config->DlssNrLookDetail, 0.00f, 2.00f, 1.00f },
        { "LookDetailRadius", "分离半径", "Separation radius", &config->DlssNrLookDetailRadius, 0.25f, 4.00f, 1.00f },
        { "LookHalo", "亮晕抑制", "Bright halo suppression", &config->DlssNrLookHalo, 0.00f, 1.00f, 0.00f },
        { "LookHaloBrightSide", "暗晕抑制", "Dark halo suppression", &config->DlssNrLookHaloBrightSide, 0.00f, 1.00f, 0.00f },
    };
    static_assert(sizeof(controls) / sizeof(controls[0]) == 16, "Keep all existing Look values reachable");
    // Editing global Look values does not enable the effect or change its saved/default mode.
    for (const auto& control : controls)
    {
        ImGui::PushID(control.id);
        float value = control.option->value_or_default();
        // Keep the original ###value identity under each existing control ID.
        const std::string label = std::string(tr(control.cn, control.en)) + "###value";
        if (MenuText::SliderFloat(label.c_str(), &value, control.low, control.high, "%.2f", ImGuiSliderFlags_AlwaysClamp,
                                 control.unlimited ? tr("不限", "Unlimited") : nullptr) && std::isfinite(value))
            *control.option = std::clamp(value, control.low, control.high);
        ImGui::SameLine();
        if (MenuText::SmallButton(tr("重置###reset", "Reset###reset"), !NextgenSliderStyle::IsDefault(value, control.neutral)))
            *control.option = control.neutral;
        ImGui::PopID();
    }
}

// Layer 1 edits the real legacy final Look while per-layer mode is off.
// Independent mode edits only the corresponding pass profile, never a second global editor.
template<class ConfigType> inline void RenderLayer(ConfigType* config, unsigned layer, unsigned activePassCount, bool allowIndependent = true)
{
    if (!config || layer >= std::min(activePassCount, DlssNr::MaxPassCount)) return;
    if (!allowIndependent || !config->DlssNrLayerLookMode.value_or_default())
    {
        if (layer == 0) Render(config);
        return;
    }
    const auto tr = [](const char* cn, const char* en) { return NextgenUi::English() ? en : cn; };
    ImGui::PushID("globalLayerLook"); ImGui::PushID(static_cast<int>(layer));
    const std::string title=(NextgenUi::English()?"Layer "+std::to_string(layer+1)+" Look":
        "第 "+std::to_string(layer+1)+" 层 Look")+"###layerLook";
    if (ImGui::CollapsingHeader(title.c_str(),layer==0?ImGuiTreeNodeFlags_DefaultOpen:0))
    {
        auto& storage=config->DlssNrLayerLooks[layer];
        if (layer==0) MenuText::TextWrapped("%s",tr("第一层 Look 始终启用。","Layer 1 Look is always enabled."));
        else
        {
            bool enabled=storage.DlssNrLookMode.value_or_default();
            if (EnableCheckbox(tr("启用本层 Look", "Enable Look for this layer"),&enabled))
                storage.DlssNrLookMode=enabled;
        }
        unsigned inherit=std::min(storage.inherit.value_or_default(),2u);
        if (layer>0)
        {
            int source=static_cast<int>(inherit);
            if (ImGui::Combo(tr("参数来源","Parameter source"),&source,
                tr("本层独立参数\0继承第一层参数\0继承上一层参数\0","Local\0Inherit layer 1\0Inherit previous layer\0")))
                storage.inherit=inherit=static_cast<unsigned>(source);
        }
        MenuText::SeparatorText(tr("本层 Look 的 UI 遮罩请求", "UI mask request for this Look"));
        unsigned uiMask = DlssNr::LayerLook::BoundUiMask(storage.uiMask.value_or_default());
        const char* maskLabels[] = { tr("不使用遮罩", "No mask"), tr("保护真实 UI", "Protect real UI"),
            tr("继承第一层", "Inherit layer 1"), tr("继承上一层", "Inherit previous layer") };
        ImGui::PushID("lookUiMask");
        for (unsigned choice = 0; choice < (layer == 0 ? 2u : 4u); ++choice)
        {
            if (choice != 0) ImGui::SameLine();
            if (ImGui::RadioButton(maskLabels[choice], uiMask == choice))
                storage.uiMask = uiMask = choice;
        }
        ImGui::PopID();
        const bool protectsUi = DlssNr::LayerLook::ResolveUiMask(*config, layer);
        MenuText::TextWrapped("%s", protectsUi
            ? tr("有效请求：仅保护本层 Look 的真实 UI；不更改 NR 模型的全局遮罩。",
                 "Effective request: protect real UI from this Look only; the NR model's global mask is unchanged.")
            : tr("有效请求：本层 Look 不使用 UI 遮罩。", "Effective request: this Look uses no UI mask."));
        MenuText::TextWrapped("%s", tr("请求不代表保护完成。需要已启用的滤镜遮罩订阅、同帧真实 draw 遮罩与原生像素坐标；缺少资源、shader 支持或兼容契约时拒绝本帧处理链。",
            "A request is not completed protection. It requires the enabled filter-mask subscription, a real same-frame draw mask and native pixel coordinates. Missing resources, shader support or a compatible contract refuse this frame's chain."));
        const bool inherited = layer > 0 && inherit != 0;
        uint32_t overrides = DlssNr::LayerLook::Overrides(storage);
        if (inherited)
            MenuText::TextWrapped("%s", tr("勾选独立调整可覆盖单项，其余跟随来源；重置恢复继承并保留本地值。",
                "Override individual parameters; the rest follow the source. Reset restores inheritance and keeps local values."));
        if (ImGui::Button(tr("重置本层参数","Reset layer parameters")))
        {
            storage.overrides = overrides = 0u;
            if (!inherited)
                DlssNr::LayerLook::Visit(storage, [](unsigned i, auto& option) {
                    option = DlssNr::LayerLook::Parameters[i].neutral;
                });
        }
        auto effective = DlssNr::LayerLook::Resolve(*config, layer);
        DlssNr::LayerLook::Visit(storage, [&](unsigned i, auto& option) {
            const auto& p = DlssNr::LayerLook::Parameters[i];
            const uint32_t bit = uint32_t{1} << i;
            ImGui::PushID(static_cast<int>(i));
            bool local = !inherited || (overrides & bit) != 0;
            if (inherited)
            {
                if (MenuText::Checkbox(tr("独立调整###override", "Override###override"), &local))
                {
                    // Start from the current inherited answer: enabling an override
                    // alone must not jump to a dormant/default local value.
                    if (local) option = effective.values[i];
                    overrides = local ? overrides | bit : overrides & ~bit;
                    storage.overrides = overrides;
                    effective = DlssNr::LayerLook::Resolve(*config, layer);
                }
                ImGui::SameLine();
            }
            ImGui::BeginDisabled(!local);
            float value = effective.values[i];
            if (MenuText::SliderFloat(tr(p.cn,p.en),&value,p.low,p.high,"%.2f",ImGuiSliderFlags_AlwaysClamp) && std::isfinite(value))
                option = DlssNr::LayerLook::Bound(value, p);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("重置","Reset")))
            {
                if (inherited)
                {
                    overrides &= ~bit;
                    storage.overrides = overrides;
                }
                else option = p.neutral;
            }
            ImGui::PopID();
        });
    }
    ImGui::PopID(); ImGui::PopID();
}

template<class ConfigType> inline void RenderGlobal(ConfigType* config,unsigned /*activePassCount*/,bool allowIndependent)
{
    if (!allowIndependent)
    {
        MenuText::TextWrapped("%s", NextgenUi::English()
            ? "Per-layer Look is unavailable on this backend. Layer 1 edits the existing final Look."
            : "此后端不支持逐层 Look；第一层编辑原有最终 Look。");
        return;
    }
    const auto tr=[](const char* cn,const char* en){return NextgenUi::English()?en:cn;};
    MenuText::SeparatorText(tr("Look 设置","Look settings"));
    bool independent=config->DlssNrLayerLookMode.value_or_default();
    if (MenuText::Checkbox(tr("逐层独立 Look###globalLayerMode","Independent per-layer Look###globalLayerMode"),&independent))
        config->DlssNrLayerLookMode=independent;
    MenuText::TextWrapped("%s",tr("第一层始终启用；其他已启用处理层可单独开关或继承参数。此设置不启用 NR 处理层。",
        "Layer 1 is always on. Other active passes have separate enable/inheritance controls; this does not enable NR passes."));
    MenuText::TextWrapped("%s", independent
        ? tr("参数在各已启用层的 Look 中编辑。", "Edit Look on each active layer page.")
        : tr("第一层 Look 使用原有全局参数并作用于最终合成；其他层不叠加独立 Look。",
             "Layer 1 edits the legacy final Look; other passes add no independent Look."));
}
} // namespace NextgenLookControls
