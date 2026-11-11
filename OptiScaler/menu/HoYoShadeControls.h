#pragma once
#include "UiLocalization.h"
#include <integration/HoYoShade.h>
#include <ui_mask/DrawUiMask.h>

// Explicit controls publish live requests; real render owners apply them.
// Saving remains a separate explicit action. No UI SDK/GPU/preset operations.
namespace HoYoShadeControls
{
inline const char* StageLabel(unsigned stage, bool english)
{
    switch (stage)
    {
    case 1: return english ? "Before super resolution" : "超分之前";
    case 2: return english ? "Before neural rendering" : "NR 之前";
    case 3: return english ? "Final output" : "最终输出";
    default: return english ? "Unknown stage" : "未知位置";
    }
}

inline const char* ReasonLabel(HoYoShade::Reason reason, bool english)
{
    using HoYoShade::Reason;
    switch (reason)
    {
    case Reason::Disabled: return english ? "F6 filter processing is off" : "F6 滤镜处理已关闭";
    case Reason::CorePending: return english ? "Waiting for HoYoShade" : "等待 HoYoShade";
    case Reason::CoreMissing: return english ? "HoYoShade core was not loaded" : "未加载 HoYoShade 核心";
    case Reason::NoRuntime: return english ? "Waiting for an effect runtime" : "等待滤镜运行环境";
    case Reason::SelectionUnavailable:
    case Reason::SelectionPending: return english ? "Selected effects are not ready" : "所选滤镜尚未就绪";
    case Reason::UnsupportedApi: return english ? "This rendering path is unavailable" : "此渲染路径暂不可用";
    case Reason::AmbiguousRuntime:
    case Reason::NaturalUnavailable: return english ? "Effect source could not be identified" : "无法确定滤镜来源";
    case Reason::UnsupportedFormat:
    case Reason::ColorSpaceUnknown: return english ? "Current colour format is unavailable" : "当前色彩格式暂不可用";
    case Reason::OtherStage: return english ? "A different placement is selected" : "当前选择了其他位置";
    case Reason::DuplicateFrame: return english ? "This source frame was already processed" : "此来源帧已处理";
    case Reason::ExecutionUnverified: return english ? "Stage execution has not been verified" : "尚未验证此阶段的执行";
    default: return english ? "Processing prerequisites are not satisfied" : "尚未满足处理条件";
    }
}

inline const char* MaskLabel(DrawUiMask::Lifecycle state,bool english)
{
    using DrawUiMask::Lifecycle;
    switch(state) {
    case Lifecycle::Pending:return english?"Pending submission (recorded)":"等待提交（已记录命令）";
    case Lifecycle::Submitted:return english?"Submitted; GPU completion pending":"已提交，等待 GPU 完成";
    case Lifecycle::Completed:return english?"GPU use completed":"GPU 消费已完成";
    case Lifecycle::Quarantined:return english?"Quarantined; completion unknown":"已隔离，完成未知";
    default:return english?"Completion unknown":"完成未知";
    }
}
inline void RenderMaskStatus(bool english)
{
    const auto tr=[english](const char* cn,const char* en){return english?en:cn;};
    const auto status=DrawUiMask::ReadLifecycle();
    MenuText::SeparatorText(tr("真实 UI 遮罩消费","Real UI mask consumers"));
    if(!status.readable) {
        MenuText::TextWrapped("%s",tr("消费状态暂不可读。","Consumer observations are temporarily unavailable."));return;
    }
    if(!status.enabled) {
        MenuText::TextWrapped("%s",tr("遮罩未启用或当前来源不受支持。","Mask disabled or current source unsupported."));return;
    }
    const char* labels[]={"NR",english?"Filters":"滤镜","FG"};
    for(unsigned i=0;i<status.consumers.size();++i) {
        const auto& h=status.consumers[i];const auto& r=h.latest;
        if(!r.capture || r.configuration!=status.configuration) {
            MenuText::TextWrapped("%s: %s",labels[i],tr("暂无当前设置的消费回执","No use receipt for current settings"));continue;
        }
        MenuText::TextWrapped("%s: %s",labels[i],MaskLabel(r.state,english));
        // These scalar IDs describe this observation, never a resource address
        // to inspect or readiness authority. Older completion has its own row.
        MenuText::TextWrapped(tr("捕获 %llu / 帧 %llu / 来源周期 %llu / 提交 %llu",
                                "Capture %llu / frame %llu / source epoch %llu / submission %llu"),
            (unsigned long long)r.capture,(unsigned long long)r.frame,(unsigned long long)r.epoch,(unsigned long long)r.submission);
        if(h.completed.capture && h.completed.configuration==r.configuration &&
           h.completed.context==r.context && h.completed.capture!=r.capture)
            MenuText::TextWrapped(tr("最近确认完成：捕获 %llu / 帧 %llu","Last confirmed complete: capture %llu / frame %llu"),
                (unsigned long long)h.completed.capture,(unsigned long long)h.completed.frame);
        if(h.quarantined.capture && h.quarantined.configuration==r.configuration &&
           h.quarantined.context==r.context && h.quarantined.capture!=r.capture)
            MenuText::TextWrapped(tr("最近隔离：捕获 %llu / 帧 %llu（完成未知）","Last quarantine: capture %llu / frame %llu (completion unknown)"),
                (unsigned long long)h.quarantined.capture,(unsigned long long)h.quarantined.frame);
    }
    MenuText::TextWrapped("%s",tr("回执仅指对应消费者的资源使用完成；画面正确性仍需实测。",
        "Receipts cover that consumer's resource use; image correctness still needs in-game observation."));
}

template<class ConfigType>
inline void Render(ConfigType* config, bool vulkan, bool dx11Bridge)
{
    const bool english = NextgenUi::English();
    const auto tr = [english](const char* cn, const char* en) { return english ? en : cn; };
    if (!ImGui::CollapsingHeader(tr("HoYoShade 滤镜###hoyoFilters", "HoYoShade filters###hoyoFilters"),
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;
    ImGui::PushID("hoyoFilters");
    MenuText::TextWrapped("%s", tr("使用 HoYoShade 当前选择的滤镜。滤镜与参数在 HoYoShade 中调整。",
                                  "Uses the effects selected in HoYoShade. Edit effects and parameters there."));
    MenuText::SeparatorText(tr("实时设置", "Live settings"));
    bool enabled = config->HoYoShadeEnabled.value_or_default();
    unsigned requestedStage = static_cast<unsigned>(config->HoYoShadeStage.value_or_default());
    bool stageChanged = false;
    ImGui::BeginDisabled(vulkan);
    const bool enabledChanged = MenuText::Checkbox(tr("由 F6 安排滤镜位置###enabled", "Let F6 place effects###enabled"), &enabled);
    const char* stages[]={StageLabel(1,english),StageLabel(2,english),StageLabel(3,english)};
    const bool stagesEnabled[]={dx11Bridge,true,true};
    int stageChoice=requestedStage>=1 && requestedStage<=3?int(requestedStage)-1:-1;
    stageChanged=MenuText::Segmented(tr("应用位置###placement","Placement###placement"),&stageChoice,stages,3,stagesEnabled);
    if(stageChanged)requestedStage=unsigned(stageChoice+1);
    ImGui::EndDisabled();
    static bool requestRejected = false;
    if (enabledChanged || stageChanged)
    {
        requestRejected = !HoYoShade::Reconfigure(enabled, requestedStage);
        if (!requestRejected)
        {
            // Preserve untouched auto/optional values. SaveIni stays explicit.
            if (enabledChanged) config->HoYoShadeEnabled = enabled;
            if (stageChanged) config->HoYoShadeStage = static_cast<int>(requestedStage);
        }
    }
    if (requestRejected)
        MenuText::TextWrapped("%s", tr("实时请求未接受；设置未写入。", "Live request was rejected; settings were not changed."));
    if (!config->HoYoShadeEnabled.has_value())
        MenuText::TextWrapped("%s", tr("开关：自动（默认关闭）。", "Enable: automatic (off by default)."));
    if (!config->HoYoShadeStage.has_value())
        MenuText::TextWrapped("%s", tr("位置：自动（默认 NR 之前）。", "Placement: automatic (before neural rendering by default)."));
    if (vulkan)
        MenuText::TextWrapped("%s", tr("Vulkan 暂不支持滤镜位置控制。", "Filter placement is unavailable on Vulkan."));
    else if (!dx11Bridge)
        MenuText::TextWrapped("%s", tr("此路径暂不支持超分之前。", "Before super resolution is unavailable on this path."));
    MenuText::TextWrapped("%s", tr("修改实时请求；跨重启保留仍需点击“保存设置”。HoYoShade 核心需由现有启动器加载。",
                                  "Changes request live application. Use Save settings to keep them across restarts. Load HoYoShade through the existing launcher."));

    MenuText::SeparatorText(tr("本次运行", "This session"));
    const auto live = HoYoShade::ReadLiveSettings();
    MenuText::TextWrapped("%s: %s · %s", tr("请求", "Requested"),
                          live.desiredEnabled ? tr("开启", "On") : tr("关闭", "Off"),
                          StageLabel(live.desiredStage, english));
    if (live.appliedKnown)
        MenuText::TextWrapped("%s: %s · %s", tr("渲染侧设置", "Render settings"),
                              live.appliedEnabled ? tr("开启", "On") : tr("关闭", "Off"),
                              StageLabel(live.appliedStage, english));
    if (!live.initialized)
        MenuText::TextWrapped("%s", tr("尚未初始化。", "Not initialized."));
    else if (live.pending)
        MenuText::TextWrapped("%s", tr("应用中：等待真实渲染边界。", "Applying: waiting for a real render boundary."));
    else if (!live.appliedEnabled)
        MenuText::TextWrapped("%s", tr("F6 滤镜调度已关闭。", "F6 effect scheduling is off."));
    else if (live.connecting)
        MenuText::TextWrapped("%s", tr("连接已有 HoYoShade 核心中；尚未执行滤镜。", "Connecting to the existing HoYoShade core; no effect execution established."));
    else if (!live.accepted)
        MenuText::TextWrapped("%s: %s", tr("连接未就绪", "Connection not ready"), ReasonLabel(live.connectionReason, english));
    else if (!live.readable)
        MenuText::TextWrapped("%s", tr("渲染侧设置已应用；当前来源的准入状态暂不可读。",
                                      "Render settings applied; current source admission is temporarily unreadable."));
    else if (live.waitingForEpoch)
        MenuText::TextWrapped("%s", tr("渲染侧设置已应用；最近滤镜来源仍在等待其旧画面或输出票据退出。",
                                      "Render settings applied; the most recent effect source is waiting for its old picture or output ticket."));
    else
        MenuText::TextWrapped("%s", tr("渲染侧设置已生效；滤镜执行取决于当前来源与启用的 technique。",
                                      "Render settings applied; effect execution depends on the current source and enabled techniques."));
    if(!vulkan)RenderMaskStatus(english);
    const auto status = HoYoShade::ReadStatus();
    if (status.read != HoYoShade::StatusRead::Ready)
    {
        MenuText::TextWrapped("%s", tr("滤镜观测状态暂不可读。", "Effect observations are temporarily unavailable."));
        ImGui::PopID();
        return;
    }
    MenuText::TextWrapped("%s: %s", tr("核心", "Core"),
                          status.accepted ? tr("接口已注册", "Interface registered") : ReasonLabel(status.coreReason, english));
    const auto& natural = status.natural;
    if (natural.status == HoYoShade::NaturalStatus::UniqueLive)
    {
        MenuText::TextWrapped("%s", tr("已观察到一个滤镜来源；尚不代表当前画面已处理。",
                                      "One effect source observed; this does not establish processing of the current picture."));
        if (natural.presetKnown)
            MenuText::TextWrapped("%s: %s", tr("来源预设", "Source preset"), natural.presetPath.data());
        else
            MenuText::TextWrapped("%s", tr("来源预设尚未读取。", "Source preset has not been observed."));
        if (natural.globalEnabledKnown && !natural.globalEnabled)
            MenuText::TextWrapped("%s", tr("HoYoShade 效果总开关已关闭。", "HoYoShade effects are globally disabled."));
        if (natural.countsKnown)
        {
            MenuText::TextWrapped("%s: %llu / %llu", tr("已选滤镜", "Selected effects"),
                                  static_cast<unsigned long long>(natural.enabledTechniques),
                                  static_cast<unsigned long long>(natural.totalTechniques));
            if (natural.enabledTechniques == 0)
                MenuText::TextWrapped("%s", tr("当前没有启用的滤镜，请在 HoYoShade 中选择。",
                                              "No effects are enabled. Select effects in HoYoShade."));
        }
        else
            MenuText::TextWrapped("%s", tr("滤镜数量尚未确定。", "Effect counts are unknown."));
    }
    else if (natural.status == HoYoShade::NaturalStatus::Unobserved)
        MenuText::TextWrapped("%s", tr("尚未观察到滤镜来源。", "No effect source observed."));
    else
        MenuText::TextWrapped("%s", tr("存在多个来源或观测容量不足，无法确定所用预设。",
                                      "Multiple sources or observation capacity prevent identifying a preset."));
    if (status.latest.known)
    {
        MenuText::TextWrapped("%s: %s", tr("最近处理请求", "Last processing request"),
                              StageLabel(static_cast<unsigned>(status.latest.stage), english));
        MenuText::TextWrapped("%s: %llu", tr("最近请求的实际 technique 回调", "Actual technique callbacks for the last request"),
                              static_cast<unsigned long long>(status.latest.techniques));
        if (status.latest.submission == HoYoShade::SubmissionState::Submitted && status.latest.techniques != 0)
            MenuText::TextWrapped("%s", tr("最近请求已提交实际滤镜命令；此历史记录不证明当前 picture 或视觉效果。",
                                          "The last request submitted effect commands; this historical observation does not establish current pixels."));
        else if (status.latest.submission == HoYoShade::SubmissionState::Submitted)
            MenuText::TextWrapped("%s", tr("仅记录到退休/预热提交，实际 technique 为零；未抑制自然滤镜。",
                                          "Only a retirement/warmup submission was recorded, with zero techniques; natural effects were not suppressed."));
        else
            MenuText::TextWrapped("%s", ReasonLabel(status.latest.reason, english));
    }
    else
        MenuText::TextWrapped("%s", tr("尚未观察到处理请求。", "No processing request observed."));
    if (ImGui::TreeNode(tr("诊断详情###details", "Diagnostic details###details")))
    {
        MenuText::TextWrapped("core=%s", HoYoShade::Text(status.coreReason));
        if (status.latest.known)
            MenuText::TextWrapped("stage=%s result=%s submission=%u techniques=%llu",
                                  HoYoShade::Text(status.latest.stage), HoYoShade::Text(status.latest.reason),
                                  static_cast<unsigned>(status.latest.submission),
                                  static_cast<unsigned long long>(status.latest.techniques));
        MenuText::TextWrapped("%s", tr("以上为最近观测，不能代替当前画面的效果或性能验证。",
                                      "These are recent observations, not confirmation of current pixels or performance."));
        ImGui::TreePop();
    }
    ImGui::PopID();
}
}
