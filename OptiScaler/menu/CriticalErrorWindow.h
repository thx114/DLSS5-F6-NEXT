// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "CriticalErrorQueue.h"
#include <genshin/GenshinLoadingProgress.h>
#include "GenshinFsrSubstageUi.h"
#include "UiLocalization.h"
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <algorithm>
#include <cstdio>
#include <cmath>

namespace CriticalUi
{
inline constexpr const char* WindowId = "##OptiScalerCriticalErrors";
inline constexpr const char* FsrHeading = "派蒙设置建议";
inline constexpr const char* FsrStepOne = "请在派蒙设置中将抗锯齿从 SMAA 切换为 FSR";
inline constexpr const char* FsrStepTwo = "请在派蒙设置中将游戏精度调整为 0.6";
inline constexpr const char* HookHeading = "正在Hook原神FSR...";
inline const char* StageText(GenshinLoading::Stage stage) noexcept
{
    constexpr const char* names[] = {
        "检测原神进程", "等待原神FSR", "正在Hook原神FSR...", "准备FSR资源",
        "验证DX12适配器与队列", "初始化DLSS DX12提供器", "等待首次SR执行",
        "等待SR输出回写", "准备NR输入", "初始化NR提供器", "等待首次NR执行",
        "准备FG输入", "初始化FG提供器", "等待首次FG执行"
    };
    const auto index = GenshinLoading::Index(stage);
    return index < GenshinLoading::Index(GenshinLoading::Stage::Count) ? names[index] : "未知阶段";
}
inline const char* StateText(GenshinLoading::State state) noexcept
{
    using GenshinLoading::State;
    switch (state)
    {
    case State::Running: return "进行中";
    case State::Complete: return "已完成";
    case State::Failed: return "失败";
    case State::Skipped: return "已跳过";
    default: return "未知 / 等待观测";
    }
}
inline const char* ReasonText(GenshinLoading::Reason reason) noexcept
{
    using GenshinLoading::Reason;
    switch (reason)
    {
    case Reason::None: return "无";
    case Reason::WaitingObservation: return "等待真实阶段事件";
    case Reason::FsrNotDetected: return "未观测到FSR调用";
    case Reason::HookNotMatched: return "未匹配当前FSR入口";
    case Reason::HookInstallFailed: return "FSR Hook安装失败";
    case Reason::ProducerResourcesUnknown: return "FSR资源尚未确认";
    case Reason::AdapterQueueUnknown: return "适配器/队列关系尚未确认";
    case Reason::ProviderPending: return "提供器尚未就绪";
    case Reason::AdmissionUnknown: return "NR输入尚未获准";
    case Reason::Disabled: return "配置已关闭";
    case Reason::Unsupported: return "当前路径不支持";
    case Reason::NativeFailure: return "运行时调用失败";
    case Reason::Recovery: return "已恢复";
    default: return "会话已重置";
    }
}
struct Rect
{
    float x = 0, y = 0, width = 0, height = 0;
    bool Intersects(const Rect& other) const noexcept
    {
        return width > 0 && height > 0 && other.width > 0 && other.height > 0 &&
            x < other.x + other.width && x + width > other.x &&
            y < other.y + other.height && y + height > other.y;
    }
};
struct Layout
{
    Rect bounds {};
    bool visible = false, compact = false;
};
inline Layout Place(const Rect& viewport, const Rect& mainMenu, bool mainVisible,
                    bool collapsed, float scale, bool paimon = false) noexcept
{
    if (!(scale > 0.0f) || !std::isfinite(scale)) return {};
    const float margin = 12.0f * scale;
    const float availableWidth = viewport.width - 2.0f * margin;
    const float availableHeight = viewport.height - 2.0f * margin;
    if (availableWidth < 180.0f * scale || availableHeight < 64.0f * scale) return {};
    Layout result;
    result.visible = true; result.compact = collapsed;
    result.bounds.width = std::min((paimon ? 960.0f : 464.0f) * scale, availableWidth);
    result.bounds.height = std::min((collapsed ? (paimon ? 160.0f : 64.0f) : paimon ? 560.0f : 270.0f) * scale, availableHeight);
    result.bounds.x = viewport.x + viewport.width - margin - result.bounds.width;
    result.bounds.y = viewport.y + margin;
    if (mainVisible && result.bounds.Intersects(mainMenu))
    {
        // Keep the upper-right anchor; use a compact header if it fits above the
        // settings menu. If neither that nor a readable right-hand strip fits,
        // keep the queued item hidden until the menu moves/closes, rather than
        // covering controls. This changes no saved UI or game configuration.
        result.compact = true; result.bounds.height = std::min((paimon ? 160.0f : 64.0f) * scale, availableHeight);
        if (result.bounds.Intersects(mainMenu))
        {
            const float rightStrip = viewport.x + viewport.width - margin -
                (mainMenu.x + mainMenu.width + margin);
            if (rightStrip < 240.0f * scale) return {};
            result.bounds.width = std::min(result.bounds.width, rightStrip);
            result.bounds.x = viewport.x + viewport.width - margin - result.bounds.width;
        }
    }
    return result;
}
struct Colors { ImVec4 heading, body; };

// Fallback loading rows also retain old successes. Age the source entries,
// never the UI draw/read tick, just as for the detailed substage snapshot.
inline Substage::Freshness ObserveLoading(const GenshinLoading::Snapshot& s,std::uint64_t now) noexcept {
    auto updated=(std::max)(s.startedTick,s.updatedTick);
    for(const auto& e:s.stages) {
        updated=(std::max)(updated,e.startedTick);
        updated=(std::max)(updated,e.updatedTick);
    }
    return Substage::Age(updated,now,s.known && s.session.Known() && s.attempt);
}
inline Layout Render(const Queue& queue, bool& collapsed, const Rect& mainMenu, bool mainVisible,
                     bool highQualityFont, float scale, const Colors& colors,
                     const GenshinLoading::Snapshot& snapshot, std::uint64_t now,
                     std::uint64_t adviceDeadlineTick)
{
    if (!queue.Size()) return {};
    const auto notice = queue.Get(Key::GenshinFsr);
    // User removed the corner progress display. Keep only actionable settings advice;
    // the loading/substage event producers and in-menu diagnostics remain independent.
    if (notice != Notice::GenshinFsrNotDetected) return {};
    // Check every draw, rather than waiting for the sampled hint queue to expire.
    if (!adviceDeadlineTick || now >= adviceDeadlineTick) return {};
    const bool paimon = true;
    const auto* viewport = ImGui::GetMainViewport();
    const auto layout = Place({ viewport->WorkPos.x, viewport->WorkPos.y,
                                viewport->WorkSize.x, viewport->WorkSize.y },
                              mainMenu, mainVisible, false, scale, paimon);
    if (!layout.visible) return layout;
    ImGui::SetNextWindowPos({ layout.bounds.x, layout.bounds.y }, ImGuiCond_Always);
    ImGui::SetNextWindowSize({ layout.bounds.width, layout.bounds.height }, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * scale, 6.0f * scale));
    ImGui::PushStyleColor(ImGuiCol_Text, colors.body);
    const auto flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin(WindowId, nullptr, flags))
    {
        const float windowScale = highQualityFont ? 1.0f : scale;
        ImGui::SetWindowFontScale(windowScale);
        const float normalNoticeSize = ImGui::GetFontSize();
        if (paimon)
        {
            // HQ uses the caller's notification font size; fallback first applies
            // its normal window scale. Advice is exactly 3x either normal path.
            if (ImGui::BeginChild("##PaimonAdvice", ImVec2(0, 0), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                  ImGuiWindowFlags_NoInputs))
            {
                ImGui::SetWindowFontScale(windowScale);
                // PushFontSize takes the size BEFORE ImGui global/window/DPI scaling.
                // Compensate those factors so the final measured text is 3x the
                // normal notice, including fractional fallback scale and rounding.
                const auto* adviceWindow = ImGui::GetCurrentWindow();
                const float fontFactor = ImGui::GetIO().FontGlobalScale *
                    adviceWindow->FontWindowScale * adviceWindow->FontDpiScale;
                ImGui::PushFontSize(normalNoticeSize * 3.0f / fontFactor);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, colors.heading);
                MenuText::TextWrapped("%s", FsrHeading);
                ImGui::PopStyleColor();
                if (!layout.compact)
                {
                    MenuText::TextWrapped("%s", FsrStepOne);
                    MenuText::TextWrapped("%s", FsrStepTwo);
                }
                ImGui::PopTextWrapPos();
                ImGui::PopFontSize();
            }
            ImGui::EndChild();
        }
        else
        {
        GenshinSubstage::Snapshot substages;
        const bool hasSubstages=Substage::Read(snapshot.session,now,substages);
        const auto freshness=hasSubstages?Substage::Observe(substages,now):ObserveLoading(snapshot,now);
        // Keep existing wait/timeout diagnostics. Source age advances each
        // draw independently of the 250ms cache and the attempt timers.
        const auto progress = GenshinLoading::Evaluate(snapshot, now);
        const auto detail=GenshinSubstage::Evaluate(substages,now);
        const auto heading = !freshness.fresh ? Substage::WaitingHeading :
            notice == Notice::GenshinHooking ? HookHeading :
            notice == Notice::GenshinFsrNotDetected ? FsrHeading : hasSubstages?Substage::StageText(detail.current):StageText(progress.stage);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.heading);
        MenuText::TextUnformatted(heading);
        ImGui::PopStyleColor();
        if (mainVisible)
        {
            ImGui::SameLine();
            if (MenuText::SmallButton(collapsed ? "展开" : "折叠")) collapsed = !collapsed;
        }
        std::string progressText;
        if (!freshness.fresh) {
            if(freshness.ageKnown)
                progressText = MenuText::Format("历史记录；源更新距今 %.2fs",double(freshness.age)/1000.0);
            else
                progressText = MenuText::Translate("等待新鲜 producer 事件；源年龄未知");
        }
        else if (hasSubstages)
            progressText = MenuText::Format("观测步骤 %u / %u（可选关闭 %u）",
                     detail.succeeded, detail.applicable, detail.disabled);
        else
            progressText = MenuText::Translate("等待真实FSR链路事件");
        MenuText::ProgressBar(hasSubstages?detail.fraction:0.0f, ImVec2(-1.0f, 0.0f), progressText.c_str());
        if (!layout.compact)
        {
            ImGui::Separator();
            // A fixed child clips/scrolls long wrapped text; repeated errors
            // and phase transitions never grow the top-level window.
            if (ImGui::BeginChild("##CriticalErrorBody", ImVec2(0, 0), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoSavedSettings))
            {
                ImGui::PushTextWrapPos(0.0f);
                if(hasSubstages)Substage::Summary(substages,detail,freshness);
                else {
                Substage::FreshnessText(freshness);
                if(!freshness.fresh)MenuText::TextUnformatted("以下为历史记录，不能证明当前 SR/NR/FG 执行");
                ImGui::PushStyleColor(ImGuiCol_Text, colors.heading);
                MenuText::Text("%s：%s%s", StageText(progress.stage), StateText(progress.state),
                            progress.timedOut ? "（超时）" : "");
                ImGui::PopStyleColor();
                if (snapshot.known)
                {
                    const auto& stage = snapshot.stages[GenshinLoading::Index(progress.stage)];
                    MenuText::Text("本阶段 %.1fs / 超时阈值 %.1fs；总耗时 %.1fs",
                                double(progress.currentMs) / 1000.0,
                                double(GenshinLoading::TimeoutMs(progress.stage)) / 1000.0,
                                double(progress.totalMs) / 1000.0);
                    MenuText::Text("原因：%s；错误码：0x%llX", ReasonText(stage.reason),
                                static_cast<unsigned long long>(stage.errorCode));
                    MenuText::Text("%sSR：%s；NR：%s", freshness.fresh?"近期观测：":"历史记录：",
                                progress.srReady ? "就绪" : "未就绪",
                                progress.nrReady ? "已执行" : snapshot.nrMode == GenshinLoading::NrMode::Disabled
                                    ? "已关闭 / 跳过" : snapshot.nrMode == GenshinLoading::NrMode::Unsupported
                                        ? "不支持 / 跳过" : snapshot.nrMode == GenshinLoading::NrMode::Unknown
                                            ? "未知 / 等待观测" : "未就绪");
                    MenuText::Text("%sFG：%s", freshness.fresh?"近期观测：":"历史记录：", progress.fgReady ? "已执行" :
                                snapshot.fgMode == GenshinLoading::NrMode::Disabled ? "已关闭 / 跳过" :
                                snapshot.fgMode == GenshinLoading::NrMode::Unsupported ? "不支持 / 跳过" :
                                "未知 / 等待观测");
                }
                }
        if (notice == Notice::GenshinFsrNotDetected)
                {
                    ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, colors.heading);
            MenuText::TextUnformatted("派蒙设置提示");
            ImGui::PopStyleColor();
            MenuText::BulletText("%s", FsrStepOne);
            MenuText::BulletText("%s", FsrStepTwo);
                }
                if(hasSubstages) {
                    ImGui::Separator();
                    if(!freshness.fresh)MenuText::TextUnformatted("历史阶段（保留上次源快照）");
                    Substage::Rows(substages,now);
                }
                if (!hasSubstages && snapshot.known && MenuText::TreeNode("关键步骤"))
                {
                    for (std::size_t i = 0; i < snapshot.stages.size(); ++i)
                    {
                        const auto& stage = snapshot.stages[i];
                        const auto duration = stage.startedTick &&
                            (stage.finishedTick ? stage.finishedTick : now) >= stage.startedTick
                            ? (stage.finishedTick ? stage.finishedTick : now) - stage.startedTick : 0;
                        MenuText::Text("%s：%s（%.1fs）", StageText(static_cast<GenshinLoading::Stage>(i)),
                                    StateText(stage.state), double(duration) / 1000.0);
                    }
                    ImGui::TreePop();
                }
                ImGui::PopTextWrapPos();
            }
            ImGui::EndChild();
        }
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    return layout;
}
}
