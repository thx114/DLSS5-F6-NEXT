#pragma once

#include "NextgenUi.h"

#include <Config.h>
#include <State.h>
#include <dlssnr/DlssNr.h>
#include <dlssnr/DlssNrFeature_Vk.h>
#include <upscalers/IFeature.h>
#include <imgui/imgui.h>

#include <cmath>
#include <optional>
#include <string>

// NEXT presentation only. All readings and the retry action belong to existing F5 APIs.
namespace NextgenF5Panels
{
inline const char* Label(const char* chinese, const char* english)
{
    return NextgenUi::English() ? english : chinese;
}

inline void ValueRow(const char* name)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextWrapped("%s", name);
    ImGui::TableSetColumnIndex(1);
}

inline void TextRow(const char* name, const char* value)
{
    ValueRow(name);
    ImGui::TextWrapped("%s", value);
}

inline bool BeginReadings(const char* id)
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                     ImGuiTableFlags_SizingStretchProp))
        return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.38f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.62f);
    return true;
}

inline bool Measured(const std::optional<double>& value)
{
    return value.has_value() && std::isfinite(*value) && *value >= 0.0;
}

inline void RenderDiagnostics(Config* config)
{
    if (config == nullptr)
        return;

    ImGui::PushID("NextgenF5Diagnostics");
    ImGui::TextUnformatted(Label("诊断与性能", "Diagnostics / performance"));
    ImGui::TextWrapped("%s", Label("显示 F6 最近报告的数据；未提供当前帧采样时间。",
                                  "F6's last reported readings; no current-frame sample timestamp is available."));

    const bool enabled = config->DlssNrEnabled.value_or_default();
    const auto feature = State::Instance().currentFeature;
    const bool runningVk = DlssNr::IsRunningVk();
    const bool nativeVk = feature && feature->Api() == API::Vulkan && !feature->IsWithDx12();
    // Prefer the current feature's route; without one, retain F5's running Vulkan indication.
    const bool vulkan = nativeVk || (!feature && runningVk);
    const bool ready = enabled && (vulkan ? runningVk : DlssNr::IsRunning());
    const char* reportedFailure = vulkan ? DlssNr::FailureReasonVk() : DlssNr::FailureReason();
    const std::string failure = reportedFailure != nullptr ? reportedFailure : "";
    const bool failed = enabled && !ready && !failure.empty();
    const bool reuseEnabled = config->DlssNrDetailReuse.value_or_default();
    const auto reuse = ready && reuseEnabled
                           ? (vulkan ? DlssNr::DetailReuseStatusVk() : DlssNr::DetailReuseStatus())
                           : DlssNr::DetailReuseInfo {};
    const auto passMs = ready ? (vulkan ? DlssNr::LastGpuTimeVk() : DlssNr::LastGpuTime())
                              : std::optional<double> {};
    unsigned int width = 0, height = 0;
    if (ready)
    {
        if (vulkan)
            DlssNr::CurrentModelSizeVk(width, height);
        else
            DlssNr::CurrentModelSize(width, height);
    }
    const char* noData = Label("无数据", "No data");

    if (ImGui::CollapsingHeader(Label("模型与运行状态###model-status-section", "Model / status###model-status-section"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (BeginReadings("model-status"))
        {
            TextRow(Label("NR 状态", "NR status"),
                    !enabled ? Label("已关闭", "Disabled")
                    : failed ? Label("本次会话已停用", "Disabled after a session failure")
                    : ready  ? Label("后端已就绪（最近报告）", "Backend ready (last reported)")
                             : Label("已启用，等待可用输入或初始化", "Enabled; waiting for usable input or initialization"));
            TextRow(Label("读取路径", "Reading route"), vulkan ? "Vulkan" : Label("D3D12 / D3D12 桥接", "D3D12 / D3D12 bridge"));
            if (ready && !vulkan)
            {
                const char* backend = DlssNr::BackendName();
                TextRow(Label("模型后端", "Model backend"), backend && backend[0] ? backend : noData);
            }
            TextRow(Label("失败原因（F6 原文）", "Failure reason (F6 original text)"),
                    !enabled ? Label("NR 已关闭；不显示保留的错误", "NR disabled; retained failure is hidden")
                    : !failure.empty() ? failure.c_str()
                                       : Label("无已报告错误；不代表已执行成功", "No reported failure; this does not prove a successful evaluation"));
            ValueRow(Label("模型工作尺寸", "Model working size"));
            if (width > 0 && height > 0)
                ImGui::Text("%u x %u px", width, height);
            else
                ImGui::TextUnformatted(noData);
            if (ready && !vulkan && width > 0 && height > 0)
            {
                ValueRow(Label("最近应用的工作比例", "Last applied working scale"));
                ImGui::Text("%d%%", DlssNr::CurrentModelResolutionPercent());
            }
            ImGui::EndTable();
        }
        ImGui::TextWrapped("%s", Label("工作尺寸是最近交给模型的尺寸，屏幕边缘压缩启用时包含打包结果。后端就绪不证明当前帧已执行或呈现。",
                                      "Working size is the last size handed to the model, including packing when edge compression is used. Backend readiness does not prove evaluation or presentation of the current frame."));
        if (failed)
        {
            const bool retryable = !vulkan || DlssNr::RetryableVk();
            if (retryable)
            {
                if (ImGui::Button(Label("重试 F6###retry-f5", "Retry F6###retry-f5")))
                {
                    // Match DlssNr_Menu.cpp::RetryNr. Do not create or release GPU resources here.
                    DlssNr::RetryAfterFailure();
                    DlssNr::RequestRetryVk();
                }
                ImGui::TextWrapped("%s", Label("沿用 F6 重试；后续处理需要可用的 NR 输入。",
                                              "Uses F6's retry action; subsequent processing requires usable NR input."));
            }
            else
                ImGui::TextWrapped("%s", Label("此 Vulkan 故障无法通过重试解决，请重启游戏。",
                                              "This Vulkan failure cannot be retried; restart the game."));
        }
    }

    if (ImGui::CollapsingHeader(Label("细节复用###detail-reuse-section", "Detail reuse###detail-reuse-section"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        const bool countsAvailable = ready && reuseEnabled &&
                                     (reuse.full != 0 || reuse.reused != 0 || reuse.fallback != 0);
        if (BeginReadings("detail-reuse"))
        {
            TextRow(Label("最近复用状态", "Last reuse state"),
                    !ready ? noData
                    : !reuseEnabled ? Label("已关闭", "Disabled")
                    : reuse.holding ? Label("因画面运动暂停", "Paused by picture motion")
                    : reuse.active ? Label("最近 NR 帧启用复用", "Reuse active on the last NR frame")
                                   : Label("最近 NR 帧未启用复用", "Reuse inactive on the last NR frame"));
            if (ready && reuseEnabled && !reuse.why.empty())
                TextRow(Label("F6 说明（底层原文）", "F6 detail (original text)"), reuse.why.c_str());
            ValueRow(Label("完整运行 / 复用 / 回退", "Full / reused / fallback"));
            if (countsAvailable)
                ImGui::Text("%llu / %llu / %llu", reuse.full, reuse.reused, reuse.fallback);
            else
                ImGui::TextUnformatted(noData);
            ImGui::EndTable();
        }
        ImGui::TextWrapped("%s", Label("计数来自 F6 已发布的记录侧累计值。回退表示本应复用但改为完整运行；这些计数不是 GPU 完成数或 FG 呈现数。",
                                      "Counts are F6's published cumulative recording-side values. Fallback means a planned reuse became a full run; these are not GPU completion or FG presentation counts."));
    }

    if (ImGui::CollapsingHeader(Label("GPU 时间与范围###gpu-timing-section", "GPU timing / scope###gpu-timing-section"), ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (BeginReadings("gpu-timing"))
        {
            ValueRow(Label("最近 NR pass", "Last NR pass"));
            if (Measured(passMs))
                ImGui::Text("%.2f ms", *passMs);
            else
                ImGui::TextUnformatted(noData);
            ValueRow(Label("最近最多 16 次 NR 计时的均值", "Mean of up to 16 recent NR timings"));
            if (ready && reuseEnabled && std::isfinite(reuse.averageMs) && reuse.averageMs > 0.0)
                ImGui::Text("%.2f ms", reuse.averageMs);
            else
                ImGui::TextUnformatted(noData);
            ValueRow(Label("同一窗口的最小 / 最大值", "Minimum / maximum in that window"));
            if (ready && reuseEnabled && reuse.averageMs > 0.0 &&
                std::isfinite(reuse.lightMs) && std::isfinite(reuse.heavyMs))
                ImGui::Text("%.2f / %.2f ms", reuse.lightMs, reuse.heavyMs);
            else
                ImGui::TextUnformatted(noData);
            TextRow(Label("独立模型时间", "Model-only time"),
                    Label("无数据：现有 F6 公开接口未提供独立读数", "No data: existing F6 public APIs expose no separate reading"));
            TextRow(Label("游戏整帧时间", "Whole game-frame time"),
                    Label("本页无数据", "No reading on this page"));
            ImGui::EndTable();
        }
        ImGui::TextWrapped("%s", Label("NR pass 时间来自已有 GPU 计时，包含该范围内的 GPU 等待和共享工作，可能延后读回。它不是独立模型成本，也不是整帧成本；复用窗口均值沿用同一计时范围。关闭 NR 后隐藏保留读数。",
                                      "NR pass time uses existing GPU timing, includes GPU waits and shared work within that range, and may be read back later. It is neither isolated model cost nor whole-frame cost. Reuse-window means use the same timing scope. Retained readings are hidden when NR is off."));
    }
    ImGui::Separator();
    if (ImGui::Button(Label("打开 F6 输入预览与调试###open-f5-preview", "Open F6 input preview / debug###open-f5-preview")))
        MenuPages::RequestPage(MenuPages::Page::NrDebug);
    ImGui::TextWrapped("%s", Label("进入现有 F6 调试页选择输入预览；此按钮仅切换页面。",
                                  "Select input preview on F6's existing debug page; this button only changes pages."));
    ImGui::PopID();
}
} // namespace NextgenF5Panels
