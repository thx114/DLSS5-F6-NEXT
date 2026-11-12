#include "pch.h"
#include <menu/GuideDiagnosticsMenu.h>
#include <dlssnr/ModelScalarPolicy.h>
#include <runtime/F6InputPolicy.h>
#include <runtime/GuideCapture.h>

#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif
#include "DlssNrFeature_Vk.h"

#include "DlssNr.h"
#include "DlssNr_ExposureScan.h"
#include "DlssNrNative.h"


#include <Config.h>
#include <State.h>
#include <Util.h>
#include <menu/menu_common.h>
#include <menu/MenuPages.h>
#include <menu/UiLocalization.h>
#include <menu/HoYoShadeControls.h>
#include <genshin/GenshinManualCapture.h>
#include <genshin/GenshinLegacyCapture.h>
#include <genshin/GenshinFsrSubstageProgress.h>
#include <genshin/GenshinDepthProvider.h>
#include <genshin/GenshinFgPreview.h>
#include "DlssNrFgDetailReuse.h"
#include <resource_tracking/GenericDepth_Dx12.h>
#include <resource_tracking/GenericDepth_Dx11.h>
#include <native/NativeDriverDx12.h>
#include <native/NativeDriverDx11.h>
#include <native/NativeDriverVk.h>
#include <native/VkPresentBridge.h>
#include <resource_tracking/GenericDepth_Vk.h>
#include <framegen/IFGFeature.h>
#include <hooks/FG_Hooks.h>
#include <native/NativeLowLatency.h>
#include <misc/IdentifyGpu.h>
#include <upscalers/FeatureProvider_Vk.h>
#include <nvapi/fakenvapi.h>

#include <imgui/imgui.h>
#include <imgui/ImGuiNotify.hpp>
#include <shaders/dlssnr/DlssNr_GameScale.h>
#include <shaders/dlssnr/DlssNr_GuidePreview.h>
#include <shaders/dlssnr/DlssNr_Spatial.h>
#include <shaders/dlssnr/DlssNr_TrimAnchors.h>
#include <shaders/dlssnr/DlssNr_AutoTrimDefault.h>
#include <shaders/dlssnr/DlssNr_FollowGame.h>
#include <shaders/dlssnr/DlssNr_ExposureCalibrate.h>
#include <shaders/dlssnr/DlssNr_ExposureAdapt.h>
#include <shaders/dlssnr/DlssNr_ExposureMeter.h>
#include <shaders/dlssnr/DlssNr_ColourEncoding.h>
#include <shaders/dlssnr/DlssNr_ProxyCurve.h>
#include "DlssNr_ColourEncodingStatus.h"
#include "DlssNr_SettingsQueue.h"
#include "DlssNr_GameDefaults.h"
#include "DlssNr_NativeMode.h"
#include "DlssNr_LutStatus.h"
#include "PassProfiles.h"
#include <menu/NextgenLookControls.h>
#include <menu/NextgenNrLayers.h>

#include <string>
#include <array>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>

namespace DlssNr
{

// Bounded UI-only stage markers for a matching PID/frame menu crash audit.
// No exception recovery and no provider/configuration operations occur here.
static bool nrMenuTraceActive = false;
static unsigned int nrMenuTraceFramesRemaining = 48;
static MenuPages::Page nrMenuTracePage = MenuPages::Page::Upscaler;

static void NrMenuBreadcrumb(const char* stage)
{
    if (!nrMenuTraceActive)
        return;

    LOG_INFO("NrMenuTrace pid={} tid={} frame={} page={} stage={}", GetCurrentProcessId(), GetCurrentThreadId(),
             ImGui::GetFrameCount(), MenuPages::Name(nrMenuTracePage), stage);
    spdlog::default_logger()->flush();
}

// Highlight guard's own ceiling -- independent of the model pass limit, which is an
// unrelated setting that happens to share this file.
static constexpr float MaxHighlightGuard = 8.0f;

// Whether this game has ever offered a Game-exposure value. Shared by the White-point-source
// panel's own "No game exposure available" readout and the Optimized Defaults preset, so the
// two stay in agreement if this definition ever changes (e.g. gains a staleness check).
static bool HaveGameExposure()
{
    return DlssNr::IsRunningVk() ? DlssNr::ExposureOfferedVk()
                                  : DlssNr::GameExposureStatus().everOffered;
}

static void HelpMarker(const char* tip);

// Retry after a failure, on whichever backend failed: Direct3D 12 at once, native Vulkan on its next frame.
static void RetryNr()
{
    DlssNr::RetryAfterFailure();
    DlssNr::RequestRetryVk();
}

// A stretch of the menu that shows status text which comes and goes, or re-wraps as it changes (a warning that appears, a
// line that only shows while something is happening). Without a slot everything below it moves when that happens, and a click
// aimed at a button lands on the next control. The slot takes `lines` lines of height whatever is in it: text drawn between
// Begin and End that needs less is padded to that, more simply grows (rare, and then it is the text that decided).
static float StatusSlotBegin() { return ImGui::GetCursorPosY(); }

static void StatusSlotEnd(float begin, int lines)
{
    const float target = begin + (float) lines * ImGui::GetTextLineHeightWithSpacing();
    const float gap = target - ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y;

    if (gap > 0.5f)
        ImGui::Dummy(ImVec2(0.0f, gap));
}

// `*.cube` files in `LUTs` inside the OptiScaler folder (MainDllPath: `OptiScaler` beside the game exe, or
// [Libraries] OptiDllPath -- where the bundled streamline/plugins folders live too), for the LUT combo below.
// Scanned once (first menu render) and on demand (the Rescan button) rather than every frame -- a directory
// listing is not worth paying for on every one of a menu's many redraws, and the folder only changes when the
// user drops a new file in.
static std::filesystem::path LutFolder()
{
    return std::filesystem::path(Config::Instance()->MainDllPath.value_or(Util::DllPath().parent_path().wstring())) /
           L"LUTs";
}

static std::vector<std::filesystem::path> ScanLutFolder()
{
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    const std::filesystem::path lutsDir = LutFolder();

    if (!std::filesystem::exists(lutsDir, ec) || ec)
        return found;

    for (const auto& entry : std::filesystem::directory_iterator(lutsDir, ec))
    {
        if (ec)
            break;
        if (!entry.is_regular_file())
            continue;

        std::wstring ext = entry.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return std::towlower(c); });
        if (ext == L".cube")
            found.push_back(entry.path());
    }

    std::sort(found.begin(), found.end());
    return found;
}

// Trim multiplies the white point, so a larger Trim darkens the picture NR is shown. The menu shows it in stops
// instead, the other way round (+ = brighter) and centred on each source's own default, which reads as 0 EV.
// The conversions are Tune for this scene's own (DlssNrExposureCalibrate), so the slider and a tuned value can never
// disagree; Tidy shows anything that rounds to zero as +0.0 (a tuned value lands exactly on 0 EV).
static float TrimToEv(float trim, float neutral)
{
    return DlssNrExposureCalibrate::Tidy(DlssNrExposureCalibrate::EvForTrim(trim, neutral));
}

static float EvToTrim(float ev, float neutral) { return DlssNrExposureCalibrate::TrimForEv(ev, neutral); }

// The one "Model input brightness" slider (and its Reset) for an exposure source's Trim. `anchorCount` is how
// many Trim anchors the ini holds for that source: they are ini-only now and take over from the slider, so
// with any present the slider is shown disabled and says why. `detectedDefault` is set for a source whose
// default is decided per game (Automatic, see DlssNr_AutoTrimDefault.h): the slider then shows it while the
// user has set nothing, and Reset goes back to it (the ini key back to auto) rather than to `neutral`.
static void RenderTrimEvSlider(CustomOptional<float>& trim, float neutral, size_t anchorCount, const char* idSuffix,
                               const char* tip, std::optional<float> detectedDefault = std::nullopt)
{
    const float minEv = TrimToEv(DlssNrTrim::kMaxTrim, neutral);
    const float maxEv = TrimToEv(DlssNrTrim::kMinTrim, neutral);
    const float shown = trim.has_value() ? trim.value() : detectedDefault.value_or(trim.value_or_default());
    float ev = std::clamp(TrimToEv(shown, neutral), minEv, maxEv);

    ImGui::BeginDisabled(anchorCount > 0);
    const std::string sliderLabel = std::string("模型输入亮度##") + idSuffix;
    if (MenuText::SliderFloat(sliderLabel.c_str(), &ev, minEv, maxEv, "%+.1f EV"))
        trim = EvToTrim(ev, neutral);

    ImGui::SameLine();

    // Keep the reset slot fixed; an explicit automatic override remains resettable even at the same value.
    const std::string resetLabel = std::string("重置##") + idSuffix;
    if (MenuText::SmallButton(resetLabel.c_str(), detectedDefault.has_value() ? trim.has_value() :
                              !NextgenSliderStyle::IsDefault(trim.value_or_default(), neutral)))
    {
        if (detectedDefault.has_value())
            trim = std::nullopt;
        else
            trim = neutral;
    }
    ImGui::EndDisabled();

    HelpMarker(tip);

    if (anchorCount > 0)
        MenuText::TextDisabled("正在使用下方 %u 个亮度校准点；存在校准点时，滑块不生效。",
                            (unsigned int) anchorCount);
}

// The brightness points of one exposure source (the ini's Trim anchors, "base white point:Trim;"): where Tune's results are
// saved. Each is the Model input brightness to use when the scene is as bright as it was at the Tune, blended in between
// and held beyond the first and last. A row per point with its own delete, and one button for them all.
static void RenderBrightnessPoints(CustomOptional<std::string>& table, float neutral, const char* idSuffix)
{
    std::string text = table.value_or_default();
    const auto points = DlssNrTrim::Parse(text);

    if (points.empty())
        return;

    ImGui::Indent();

    const bool pointsOpen = MenuText::TreeNode((std::string("亮度校准点##") + idSuffix).c_str());
    HelpMarker("自动调优为不同亮度场景保存的设置。自动模式在最近两个点之间插值。分别在明亮和昏暗场景调优；相同亮度下再次调优会替换旧点，最多 8 个点。更改测光方式后需重新调优。");
    if (pointsOpen)
    {
        size_t remove = points.size();

        for (size_t i = 0; i < points.size(); ++i)
        {
            MenuText::Text("场景亮度 %.4g：%+.1f EV", points[i].key, DlssNrExposureCalibrate::Tidy(TrimToEv(points[i].trim, neutral)));
            ImGui::SameLine();

            if (MenuText::SmallButton((std::string("删除##") + idSuffix + std::to_string(i)).c_str()))
                remove = i;
        }

        if (MenuText::SmallButton((std::string("清空所有点##") + idSuffix).c_str()))
            table = std::string();
        else if (remove < points.size())
        {
            DlssNrTrim::RemovePoint(text, remove);
            table = text;
        }

        ImGui::TreePop();
    }

    ImGui::Unindent();
}

// "Tune for this scene" (shaders/dlssnr/DlssNr_ExposureCalibrate.h), under a brightness slider: sweeps it over the scene
// on screen and offers the step where the model's output had the most detail without flicker or clipping. Indented under
// the slider it sets, a SmallButton like the other actions here. D3D12 and Vulkan. `source` is the panel's white point source
// (3 Automatic, 1 Game exposure), `trim` / `neutral` its slider. Named apart from Follow-game's "Re-learn", which is a
// different calibration.
static void RenderTuneForThisScene(uint32_t source, CustomOptional<float>& trim, float neutral,
                                   CustomOptional<std::string>& table)
{
    const auto cal = DlssNr::ExposureCalibration();
    // A result belongs to the panel it was tuned in: its EVs are in that slider's units. A Measure detail run is shown
    // under Compare instead.
    const bool mine = cal.source == source && !cal.measure;
    const bool havePoints = !DlssNrTrim::Parse(table.value_or_default()).empty();
    // Where a result goes: with brightness points the slider is not in force, so it is saved as a point; without any it is
    // applied to the slider, as before, and "Save as point" starts a table.
    static std::string pointNote;
    const auto savePoint = [&](float ev)
    {
        std::string text = table.value_or_default();
        pointNote.clear();

        if (DlssNrTrim::AddPoint(text, cal.anchorKey, EvToTrim(ev, neutral)))
            table = text;
        else
            pointNote = "校准点已满（8 个），请先删除一个。";
    };
    ImGui::Indent();
    // Running and result are different heights, and a run ends under the cursor: three lines are reserved from the moment
    // one starts so the buttons below do not jump when it ends. Idle reserves nothing, so there is no blank stretch.
    const bool tuneBusy = mine && (cal.running || cal.starting || cal.finished);
    const float tuneSlot = StatusSlotBegin();

    if ((cal.running || cal.starting) && mine)
    {
        char text[96];

        if (cal.starting)
            snprintf(text, sizeof(text), MenuText::Translate("正在启动…").c_str());
        else if (cal.stepIndex < cal.stepCount && cal.passes > 1)
            snprintf(text, sizeof(text), MenuText::Translate("调优 %+.1f EV（%u/%u，第 %u/%u 次处理）：请保持镜头静止").c_str(), cal.stepEv,
                     cal.stepIndex + 1, cal.stepCount, cal.pass + 1, cal.passes);
        else if (cal.stepIndex < cal.stepCount)
            snprintf(text, sizeof(text), MenuText::Translate("调优 %+.1f EV（%u/%u）：请保持镜头静止").c_str(), cal.stepEv,
                     cal.stepIndex + 1, cal.stepCount);
        else
            snprintf(text, sizeof(text), MenuText::Translate("正在读取结果…").c_str());

        MenuText::ProgressBar(cal.progress, ImVec2(-FLT_MIN, 0.0f), text);

        if (MenuText::SmallButton("取消##tune"))
            DlssNr::CancelExposureCalibration();
    }
    else if (cal.finished && mine)
    {
        // The long results wrap, and their buttons go on the next line: on one unwrapped line OK ran off the panel's
        // right edge, and without it the result could not be dismissed, so Tune could not be run again.
        const auto warning = [](const char* text)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, NextgenUi::ReadableTextColor(ImVec4(0.9f, 0.6f, 0.25f, 1.0f)));
            MenuText::TextWrapped("%s", text);
            ImGui::PopStyleColor();
        };
        bool ownLine = false;

        if (cal.unsure)
        {
            warning("不同亮度的细节差异没有超过测量自身波动，无法确定更优设置。保留当前值。");
            ownLine = true;
        }
        else if (cal.unrepeated)
        {
            char text[192];
            snprintf(text, sizeof(text), MenuText::Translate("两次结果不一致（%+.1f EV 与 %+.1f EV），不够可靠。保留当前值，请保持镜头静止后重试。").c_str(),
                     cal.firstPassEv, cal.lastPassEv);
            warning(text);
            ownLine = true;
        }
        else if (cal.changed)
        {
            MenuText::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f), "当前场景最佳：%+.1f EV（当前 %+.1f EV）",
                               cal.resultEv, cal.currentEv);
            ImGui::SameLine();

            if (havePoints)
            {
                if (MenuText::SmallButton("保存为校准点##tune"))
                {
                    savePoint(cal.resultEv);
                    if (pointNote.empty())
                        DlssNr::DismissExposureCalibration();
                }
            }
            else
            {
                if (MenuText::SmallButton("应用##tune"))
                {
                    trim = EvToTrim(cal.resultEv, neutral);
                    DlssNr::DismissExposureCalibration();
                }

                ImGui::SameLine();

                if (MenuText::SmallButton("保存为校准点##tune"))
                {
                    savePoint(cal.resultEv);
                    if (pointNote.empty())
                        DlssNr::DismissExposureCalibration();
                }
            }

            if (ImGui::IsItemHovered())
                MenuText::SetTooltip("%s", "保存此亮度场景的结果，自动模式在已保存点之间插值。");
        }
        else
        {
            MenuText::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f), "当前场景最佳：%+.1f EV，已与当前设置一致。",
                               cal.currentEv);
        }

        if (!pointNote.empty())
            warning(pointNote.c_str());

        if (!ownLine)
            ImGui::SameLine();

        if (MenuText::SmallButton(cal.changed ? "保留##tune" : "确定##tune"))
            DlssNr::DismissExposureCalibration();

        ImGui::SameLine();
        ImGui::BeginDisabled(!cal.available);

        if (MenuText::SmallButton("重新调优##tune"))
            DlssNr::StartExposureCalibration(source); // clears this result itself

        ImGui::EndDisabled();

        // The curves and the raw measure are for checking the tuning itself, not for choosing.
        if (!cal.ev.empty() && MenuText::TreeNode("详细信息##tune"))
        {
            const int n = (int) cal.ev.size();
            const auto bandLabel = MenuText::Translate("带通##tune");
            const auto rawLabel = MenuText::Translate("原始##tune");
            ImGui::PlotLines(bandLabel.c_str(), cal.scoreBand.data(), n, 0, nullptr, FLT_MAX, FLT_MAX,
                             ImVec2(0.0f, 50.0f));
            ImGui::PlotLines(rawLabel.c_str(), cal.scoreRaw.data(), n, 0, nullptr, FLT_MAX, FLT_MAX,
                             ImVec2(0.0f, 50.0f));
            MenuText::TextDisabled("评分范围：左侧 %+.1f EV 至右侧 %+.1f EV。最优：带通 %+.1f，原始 %+.1f。",
                                cal.ev.front(), cal.ev.back(), cal.bestBandEv, cal.bestRawEv);
            // What the model does to the game's colour and shadows there (output against the game's frame).
            MenuText::TextDisabled("%+.1f EV 时：饱和度 %+.0f%%，暖色 %+.3f，阴影 %.0f%% %s，暗部截断 %.1f%%。",
                                cal.resultStepEv, 100.0f * cal.resultSaturation, cal.resultWarmth,
                                std::fabs(100.0f * cal.resultShadowDarkening),
                                cal.resultShadowDarkening >= 0.0f ? "变暗" : "提亮", 100.0f * cal.resultCrushed);

            // Not when the run was unsure or its passes disagreed: those keep the current value for either measure.
            // Nor when the passes picked different raw bests -- the number shown is the last pass's, and offering it
            // would be a single-pass answer from a run that promised two.
            ImGui::BeginDisabled(cal.unsure || cal.unrepeated || !cal.rawAgreed);

            if (MenuText::SmallButton(havePoints ? "改为保存原始结果##tune" : "改为应用原始结果##tune"))
            {
                if (havePoints)
                    savePoint(cal.bestRawEv);
                else
                    trim = EvToTrim(cal.bestRawEv, neutral);

                if (pointNote.empty())
                    DlssNr::DismissExposureCalibration();
            }

            ImGui::EndDisabled();

            ImGui::TreePop();
        }
    }
    else
    {
        ImGui::BeginDisabled(!cal.available);

        if (MenuText::SmallButton("为当前场景自动调优"))
            DlssNr::StartExposureCalibration(source);

        ImGui::EndDisabled();
        HelpMarker("寻找当前场景细节最多的模型输入亮度。保持镜头静止几秒，画面会在测试时变亮或变暗。点击应用或保存为校准点后才会改变设置。");

        if (!cal.available && !cal.unavailable.empty())
            MenuText::TextDisabled("不可用：%s", cal.unavailable.c_str());

        if (mine && !cal.startError.empty())
            MenuText::TextDisabled("无法启动：%s", cal.startError.c_str());
        else if (mine && !cal.aborted.empty())
        {
            // Wrapped: a stop now says what it measured, and an unwrapped line runs off the panel.
            ImGui::PushStyleColor(ImGuiCol_Text, NextgenUi::ReadableTextColor(ImVec4(0.9f, 0.6f, 0.25f, 1.0f)));
            MenuText::TextWrapped("已停止：%s", cal.aborted.c_str());
            ImGui::PopStyleColor();
        }
    }

    if (tuneBusy)
        StatusSlotEnd(tuneSlot, 3);
    ImGui::Unindent();
}

// "Measure detail" (Story 1 of the input canvas epic): measures NR's output on the scene on screen at the current
// settings -- detail added over the game's frame, and flicker beyond the input's -- for about a second, and shows it
// beside the previous measurement, so any setting can be A/B'd by number. Shares Tune's run (DlssNr_ExposureCalibrate.h,
// MeasureSettings); nothing is dispatched until the button is pressed.
static void RenderMeasureDetail()
{
    const auto cal = DlssNr::ExposureCalibration();

    if (cal.measure && (cal.running || cal.starting))
    {
        MenuText::ProgressBar(cal.progress, ImVec2(-FLT_MIN, 0.0f),
                           cal.starting ? "正在启动…" : "正在测量，请保持镜头静止。");

        if (MenuText::SmallButton("取消##measure"))
            DlssNr::CancelExposureCalibration();

        return;
    }

    ImGui::BeginDisabled(!cal.measureAvailable);

    if (MenuText::Button("测量细节"))
        DlssNr::StartMeasureDetail();

    ImGui::EndDisabled();
    HelpMarker("用约一秒对比 NR 和游戏原画：新增细节、闪烁（1 倍表示未增加）、颜色和阴影。\n在静止场景测量，修改一个设置后再次测量。几个百分点的差异可能只是噪声。");

    if (!cal.measureAvailable && !cal.measureUnavailable.empty())
        MenuText::TextDisabled("不可用：%s", cal.measureUnavailable.c_str());

    if (cal.measure && !cal.startError.empty())
        MenuText::TextDisabled("无法启动：%s", cal.startError.c_str());
    else if (cal.measure && !cal.aborted.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, NextgenUi::ReadableTextColor(ImVec4(0.9f, 0.6f, 0.25f, 1.0f)));
        MenuText::TextWrapped("已停止：%s", cal.aborted.c_str());
        ImGui::PopStyleColor();
    }

    if (cal.measurements == 0)
        return;

    const auto& m = cal.latest;
    MenuText::Text("第 %u 次测量", cal.measurements);
    MenuText::TextWrapped("%s", cal.detailWords.c_str());
    MenuText::TextWrapped("%s", cal.flickerWords.c_str());
    MenuText::TextWrapped("%s", cal.colourWords.c_str());
    MenuText::TextWrapped("%s", cal.shadowWords.c_str());
    MenuText::TextDisabled("新增细节 %.5f（输出 %.5f，输入 %.5f），原始 %.5f | 闪烁 %.5f（输出变化 %.5f，输入变化 %.5f）| %u 帧", m.detail, m.detailOut, m.detailIn, m.raw, m.flicker, m.flickerOut, m.flickerIn,
                        m.frames);

    if (cal.hasPrevious && !cal.comparable)
    {
        MenuText::TextDisabled("与第 %u 次测量不可比：曝光不同，请等待稳定；也可能白点来源已改变。", cal.measurements - 1);
    }
    else if (cal.hasPrevious)
    {
        MenuText::TextWrapped("对比第 %u 次：%s", cal.measurements - 1, cal.compareWords.c_str());
    }
}

// Help belongs to the setting text and control, with no extra layout item.
static void HelpMarker(const char* tip)
{
    MenuText::ItemHelp(tip);
}

static void ModelScalarHelp(const char* label)
{
    if (std::strcmp(label, "整体强度") == 0)
        HelpMarker("本次处理的强度。越高效果越强，越低越接近游戏原画。默认 1；超过 1 为实验范围。");
    else if (std::strcmp(label, "局部结构") == 0)
        HelpMarker("细节和局部对比度。越高纹理越清晰，越低越平滑。默认 1；超过 1 为实验范围。");
    else if (std::strcmp(label, "局部色调") == 0)
        HelpMarker("大范围光照变化。越高重新布光越强，越低越保留原光照。默认 1；预设 B 上限为 1，其他预设上限为 2。");
    else if (std::strcmp(label, "皮肤结构") == 0)
        HelpMarker("皮肤细节强度，默认 1。0 为平滑皮肤，旧版 -1 跟随局部结构。预设 B 上限为 1，其他预设上限为 2。");
}

// Geometry controls commit on release because they change feature resources.
template <typename Option>
static bool DeferredSlider(const char* label, Option* opt, float mn, float mx,
                           float def, const char* fmt = "%.2f", bool inheritReset = false)
{
    static std::unordered_map<ImGuiID, float> pending;
    const ImGuiID id = ImGui::GetID(label);

    auto it = pending.find(id);
    float value = it != pending.end() ? it->second : (opt->has_value() ? opt->value() : def);
    bool changed = false;

    if (MenuText::SliderFloat(label, &value, mn, mx, fmt))
        pending[id] = value;

    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        auto committed = pending.find(id);

        if (committed != pending.end())
        {
            *opt = std::clamp(committed->second, mn, mx);
            pending.erase(committed);
            changed = true;
        }
    }

    ImGui::SameLine();

    const std::string resetId = std::string("重置##") + label;
    if (MenuText::SmallButton(resetId.c_str(), inheritReset ? (opt->has_value() || pending.contains(id)) :
                              !NextgenSliderStyle::IsDefault(value, def)))
    {
        if (inheritReset)
            *opt = std::optional<float> {};
        else
            *opt = def;
        pending.erase(id);
        changed = true;
    }

    ModelScalarHelp(label);
    return changed;
}

static bool DriverProxyProfilesLocked(const Config* config);

// The proven evaluator consumes these scalars every frame. Unknown providers keep release-to-commit
// controls, avoiding a feature recreation on every drag step. Reset preserves later-pass inheritance.
template <typename Option>
static bool ModelScalarSlider(const char* label, Option* opt, float mn, float mx,
                              float def, const char* fmt = "%.2f", bool inheritReset = false)
{
    def = std::isfinite(def) ? std::clamp(def, mn, mx) : 1.0f;
    if (opt->has_value())
    {
        const float bounded = std::isfinite(opt->value()) ? std::clamp(opt->value(), mn, mx) : 1.0f;
        if (opt->value() != bounded)
            *opt = bounded;
    }
    const auto consumer = DlssNr::IsRunningVk() ? DlssNr::ModelScalars::Consumer::Vulkan
                                               : DlssNr::ModelScalars::Consumer::Dx12;
    if (DriverProxyProfilesLocked(Config::Instance()) || !DlssNr::ModelScalars::Live(consumer))
        return DeferredSlider(label, opt, mn, mx, def, fmt, inheritReset);

    float value = opt->has_value() ? opt->value() : def;
    bool changed = MenuText::SliderFloat(label, &value, mn, mx, fmt);
    if (changed)
        *opt = std::clamp(value, mn, mx);

    ImGui::SameLine();
    const std::string resetId = std::string("重置##") + label;
    if (MenuText::SmallButton(resetId.c_str(), inheritReset ? opt->has_value() :
                              !NextgenSliderStyle::IsDefault(value, def)))
    {
        if (inheritReset)
            *opt = std::optional<float> {};
        else
            *opt = def;
        changed = true;
    }
    ModelScalarHelp(label);
    return changed;
}

// Only a deliberate selection writes config. Unknown/legacy values stay visible without being
// clamped into one of the supported names merely because the menu was rendered.
static bool ProfileChoiceCombo(const char* label, uint32_t* value, bool* inherit,
                               const char* const* names, int nameCount)
{
    NrMenuBreadcrumb("profile.preview.begin");
    const bool inherited = inherit != nullptr && *inherit;
    std::string unknown;
    const char* preview = "自动（继承第 1 次）";
    if (!inherited)
    {
        if (*value < (uint32_t) nameCount)
            preview = names[*value];
        else
        {
            // Translation is UTF-8 display data, not an MSVC runtime format string.
            // Preserve the existing translation key and replace only its known value token.
            unknown = MenuText::Translate("未知／旧版值 {}（保持）");
            if (const auto token = unknown.find("{}"); token != std::string::npos)
                unknown.replace(token, 2, std::to_string(*value));
            preview = unknown.c_str();
        }
    }
    NrMenuBreadcrumb("profile.preview.end");
    std::vector<const char*> choices;
    choices.reserve(nameCount+(inherit?1:0));
    if(inherit)choices.push_back("自动（继承第 1 次）");
    for(int i=0;i<nameCount;++i)choices.push_back(names[i]);
    int selected=inherited?0:(*value<(uint32_t)nameCount?int(*value)+(inherit?1:0):-1);
    const bool changed=MenuText::Segmented(label,&selected,choices.data(),int(choices.size()));
    if(selected<0)MenuText::TextDisabled("%s",preview);
    if(changed){
        if(inherit)*inherit=selected==0;
        if(!inherit || selected>0)*value=uint32_t(selected-(inherit?1:0));
    }
    return changed;
}

// An absent later-pass setting inherits pass 1; supported values remain zero-based.
static bool InheritedProfileCombo(const char* label, CustomOptional<uint32_t, NoDefault>* opt,
                                  const char* const* names, int nameCount)
{
    bool inherit = !opt->has_value();
    uint32_t value = opt->value_or(0u);
    if (!ProfileChoiceCombo(label, &value, &inherit, names, nameCount))
        return false;

    if (inherit)
        *opt = std::optional<uint32_t> {};
    else
        *opt = value;

    return true;
}

static bool DriverProxyProfilesLocked(const Config* config)
{
    // Published Proxy is positive runtime evidence, but spatial status remains Off when edge
    // compression is disabled. Also guard the requested proxy route until it publishes a frame.
    return !DlssNr::IsRunningVk() &&
           (DlssNr::EdgeCompressionStatus().status == DlssNr::Spatial::Status::Proxy ||
            config->DlssNrUseProxy.value_or_default());
}

// UI visibility follows the configured backend pass limit without changing saved layer values.
static unsigned int VisiblePassCount(const Config* config)
{
    if (DriverProxyProfilesLocked(config))
        return 1u;
    return ClampPassCount(config->DlssNrPasses.value_or_default());
}

// The effective value is captured only at an actual UI commit. No render/GPU
// owner is queried here; same-value/inheritance-only edits emit no A/B event.
static void InheritedPresetCombo(Config* config, unsigned pass,
                                 CustomOptional<uint32_t, NoDefault>* opt,
                                 const char* const* names, int nameCount)
{
    const auto before = Profiles::PassPreset(*config, pass);
    if (InheritedProfileCombo("模型预设", opt, names, nameCount))
    {
        Profiles::ClampConfiguredModelScalars(*config);
        if constexpr (F6InputPolicy::FsrBridgeEnabled)
            GenshinSubstage::RequestPreset(pass, before, Profiles::PassPreset(*config, pass));
    }
}

// Per-pass profile values the pass presets write. One table shared by ApplyPassPreset and
// PassPresetActive (the button highlight), so what a preset sets and what counts as "that preset is
// in effect" can't drift apart.
struct PassProfile
{
    uint32_t style; // DLSSNR.Style: 0 Model A, 1 Model B, 2 Model C
    float intensity;
    float structure;
    float tone;
    float skin;
};

static constexpr PassProfile PresetPass1 { 1u, 1.8f, 1.8f, 1.8f, -1.0f };    // Model B
static constexpr PassProfile PresetPass2 { 1u, 1.0f, 1.0f, 1.0f, -1.0f };    // Model B
static constexpr PassProfile PresetPass3 { 1u, 0.75f, 1.5f, 0.46f, 1.0f };   // Model B

// This fork's recommended starting points for the "Model passes" slider, one per pass count.
// Each button touches only the settings named below (including Pass 2/3 overrides once the
// preset's pass count reaches them); anything else in the panel (skin/environment sliders,
// Compare, Debug view, Hold frame, Downscaler, exposure-scan settings, etc.) is left exactly as
// the user had it.
static void ApplyPassPreset(Config* config, unsigned int passes)
{
    config->DlssNrEnabled = true;
    config->DlssNrPrecision = 0u; // NVIDIA (FP8)
    config->DlssNrApplyModel = true;
    config->DlssNrPasses = ClampPassCount(passes);

    // Upscale Method, Upscale Mode and Final Image Composition are deliberately not set here: the Pre-SR/Post-SR
    // tiers set them, and writing them from a pass preset would clear that tier's highlight.
    // Reuse bottleneck is not set here either: it is a speed/quality choice per kernel set, not part of a look, so a
    // preset leaves both checkboxes as the user had them.
    config->DlssNrTransferStrength = 1.0f;      // Detail strength
    config->DlssNrColourStrength = 1.0f;
    config->DlssNrStyle = PresetPass1.style;
    config->DlssNrIntensity = PresetPass1.intensity;
    config->DlssNrLocalStructure = PresetPass1.structure;
    config->DlssNrLocalTone = PresetPass1.tone;
    config->DlssNrSkinStructure = PresetPass1.skin;
    config->DlssNrAutoMask = true;
    // Automatic exposure at its default brightness (DlssNr_AutoTrimDefault.h). Game exposure at 1x, which
    // this used to set, gave NBA 2K27 a model input with a median of 0.07-0.32 (measured 2026-09-25).
    config->DlssNrWhitePointSource = 3u;        // Automatic exposure
    config->DlssNrAutoExposureTrim = std::nullopt;
    config->DlssNrMaxRatio = 2.0f;               // Highlight guard

    if (passes >= 2u)
    {
        config->DlssNrPass2Style = PresetPass2.style;
        config->DlssNrPass2Intensity = PresetPass2.intensity;
        config->DlssNrPass2LocalStructure = PresetPass2.structure;
        config->DlssNrPass2LocalTone = PresetPass2.tone;
        config->DlssNrPass2SkinStructure = PresetPass2.skin;
        config->DlssNrPass2AutoMask = true;
    }

    if (passes >= 3u)
    {
        config->DlssNrPass3Style = PresetPass3.style;
        config->DlssNrPass3Intensity = PresetPass3.intensity;
        config->DlssNrPass3LocalStructure = PresetPass3.structure;
        config->DlssNrPass3LocalTone = PresetPass3.tone;
        config->DlssNrPass3SkinStructure = PresetPass3.skin;
        config->DlssNrPass3AutoMask = true;
    }
}

static bool NearlyEqual(float a, float b)
{
    return std::fabs(a - b) < 0.005f;
}

// Pass 2/3 settings are optional (absent = inherit pass 1), so an absent one never matches a preset.
template <typename FloatOpt>
static bool OptionalIs(FloatOpt& opt, float value)
{
    return opt.has_value() && NearlyEqual(opt.value(), value);
}

static bool Pass1Is(Config* config, const PassProfile& p)
{
    return config->DlssNrStyle.value_or_default() == p.style &&
           NearlyEqual(config->DlssNrIntensity.value_or_default(), p.intensity) &&
           NearlyEqual(config->DlssNrLocalStructure.value_or_default(), p.structure) &&
           NearlyEqual(config->DlssNrLocalTone.value_or_default(), p.tone) &&
           NearlyEqual(config->DlssNrSkinStructure.value_or_default(), p.skin);
}

static bool Pass2Is(Config* config, const PassProfile& p)
{
    return config->DlssNrPass2Style.has_value() && config->DlssNrPass2Style.value() == p.style &&
           OptionalIs(config->DlssNrPass2Intensity, p.intensity) &&
           OptionalIs(config->DlssNrPass2LocalStructure, p.structure) &&
           OptionalIs(config->DlssNrPass2LocalTone, p.tone) &&
           OptionalIs(config->DlssNrPass2SkinStructure, p.skin);
}

static bool Pass3Is(Config* config, const PassProfile& p)
{
    return config->DlssNrPass3Style.has_value() && config->DlssNrPass3Style.value() == p.style &&
           OptionalIs(config->DlssNrPass3Intensity, p.intensity) &&
           OptionalIs(config->DlssNrPass3LocalStructure, p.structure) &&
           OptionalIs(config->DlssNrPass3LocalTone, p.tone) &&
           OptionalIs(config->DlssNrPass3SkinStructure, p.skin);
}

// Whether a pass preset is what is currently in effect, for highlighting its button. Derived from
// the config (not remembered), like ResolutionTierActive: the pass count plus each pass's own
// profile must match. Detail/Colour strength, white point and the like are left out on purpose so
// tuning them doesn't drop the highlight.
static bool PassPresetActive(Config* config, unsigned int passes)
{
    return config->DlssNrPasses.value_or_default() == passes && Pass1Is(config, PresetPass1) &&
           (passes < 2u || Pass2Is(config, PresetPass2)) &&
           (passes < 3u || Pass3Is(config, PresetPass3));
}

// Success highlights follow light/dark surfaces while preserving the preset action and ID.
static bool PresetButton(const char* label, bool active)
{
    if (active)
    {
        const bool light = Config::Instance()->LightTheme.value_or_default();
        ImGui::PushStyleColor(ImGuiCol_Button, light ? ImVec4(0.78f, 0.90f, 0.80f, 1.0f) : ImVec4(0.20f, 0.55f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, light ? ImVec4(0.67f, 0.84f, 0.70f, 1.0f) : ImVec4(0.26f, 0.65f, 0.31f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, light ? ImVec4(0.56f, 0.76f, 0.60f, 1.0f) : ImVec4(0.16f, 0.45f, 0.20f, 1.0f));
    }

    const bool pressed = MenuText::Button(label);

    if (active)
        ImGui::PopStyleColor(3);

    return pressed;
}

// Quality tiers (High/Medium/Low/Potato) for running NR at a reduced model resolution, offered as
// two preset rows that differ only in where NR runs: Pre-SR (Before Super Resolution) and Post-SR
// (After Super Resolution). Each touches only Upscale Mode, Upscale Method, Model resolution,
// Final Image Composition and (where the tier lists one) Restore Sharpness, plus Auto model
// resolution -- turned off because Auto overrides the Model resolution slider when NR runs
// post-SR, which would make the tier's resolution a silent no-op. Each also sets "NR 处理位置：",
// which includes turning off Generate-before-SR/apply-after-SR (DLSS): that mode generates pre-SR
// and disables the placement choice, so leaving it on would contradict the row's name.
struct ResolutionTier
{
    const char* name;
    uint32_t upscaleMethod;      // 0 Bilinear, 1 SGSR1
    float workingScale;          // model resolution as a fraction (1.0 = 100%)
    uint32_t composition;        // 1 Reversible curve + composed, 2 Reversible curve + replace
    float restoreSharpness;      // < 0: leave the current value (composed mode hides the slider)
    float detailStrength;        // < 0: leave the current value
    float colourStrength;        // < 0: leave the current value
};

static constexpr ResolutionTier ResolutionTiers[] = {
    { "高质量",   1u, 1.00f, 1u, -1.0f, 1.0f, 1.0f },
    { "均衡", 1u, 0.80f, 2u,  1.50f, -1.0f, -1.0f },
    { "低开销",    0u, 0.65f, 2u,  1.50f, -1.0f, -1.0f },
    { "极低开销", 0u, 0.50f, 2u,  1.70f, -1.0f, -1.0f },
};

static void ApplyResolutionTier(Config* config, int& pendingScale, const ResolutionTier& tier,
                                bool beforeSuperResolution)
{
    // Same rule as the "NR 处理位置：" combo: leaving Finished Picture clears a session failure.
    if (config->DlssNrFinishedPicture.value_or_default())
        RetryNr();
    config->DlssNrFinishedPicture = false;
    config->DlssNrRunBeforeSr = beforeSuperResolution;
    config->DlssNrDeferredDlss = false;

    config->DlssNrTransfer = 2u; // Upscale Mode: NVIDIA residual
    config->DlssNrReducedUpscaleMethod = tier.upscaleMethod;
    config->DlssNrModelResolutionAuto = false;
    config->DlssNrWorkingScale = tier.workingScale;
    pendingScale = -1; // clear any in-flight drag
    config->DlssNrReversibleMode = tier.composition;

    if (tier.restoreSharpness >= 0.0f)
        config->DlssNrReplaceDetailStrength = tier.restoreSharpness;

    if (tier.detailStrength >= 0.0f)
        config->DlssNrTransferStrength = tier.detailStrength;

    if (tier.colourStrength >= 0.0f)
        config->DlssNrColourStrength = tier.colourStrength;
}

// Whether a tier's settings are what is currently in effect, for highlighting its button. Derived
// from the config rather than remembered, so it can never claim a preset the settings have since
// drifted from, and it survives a restart. Restore Sharpness is left out of the comparison on
// purpose: fine-tuning it after picking a tier shouldn't drop the highlight. At most one button
// can match -- tiers have distinct resolutions and the two rows differ in placement.
static bool ResolutionTierActive(Config* config, const ResolutionTier& tier, bool beforeSuperResolution)
{
    return !config->DlssNrFinishedPicture.value_or_default() &&
           config->DlssNrRunBeforeSr.value_or_default() == beforeSuperResolution &&
           config->DlssNrTransfer.value_or_default() == 2u &&
           config->DlssNrReducedUpscaleMethod.value_or_default() == tier.upscaleMethod &&
           !config->DlssNrModelResolutionAuto.value_or_default() &&
           std::fabs(config->DlssNrWorkingScale.value_or_default() - tier.workingScale) < 0.005f &&
           config->DlssNrReversibleMode.value_or_default() == tier.composition;
}

// Which API's Optical F5Low drivers and depth finder this game uses: a game presents with one of them. A dxvk game
// presents its D3D frames through Vulkan; by default its D3D drivers are still the ones that run (NativeDriverDx11.h),
// unless the experimental DlssNrNativeDxvkVulkan key asks for dxvk's own Vulkan calls instead.
enum class NativeApi
{
    Dx12,
    Dx11,
    Vulkan
};

static NativeApi CurrentNativeApi()
{
    const auto& state = State::Instance();

    if (VkPresentBridge::DxvkThroughVulkan())
        return NativeApi::Vulkan;

    const auto present = state.swapchainApi == API::Vulkan  ? DlssNrNativeMode::PresentApi::Vulkan
                         : state.swapchainApi == API::DX12 ? DlssNrNativeMode::PresentApi::Dx12
                         : state.swapchainApi == API::DX11 ? DlssNrNativeMode::PresentApi::Dx11
                                                           : DlssNrNativeMode::PresentApi::NotSelected;
    const auto interop = state.swapchainInteropApi == SwapchainInteropApi::VkwDx12
                             ? DlssNrNativeMode::Interop::VkwDx12
                         : state.swapchainInteropApi == SwapchainInteropApi::Dx11wDx12
                             ? DlssNrNativeMode::Interop::Dx11wDx12
                             : DlssNrNativeMode::Interop::None;

    switch (DlssNrNativeMode::GameApiFor(present, interop, state.currentD3D11Device != nullptr))
    {
    case DlssNrNativeMode::GameApi::Vulkan:
        return NativeApi::Vulkan;
    case DlssNrNativeMode::GameApi::Dx11:
        return NativeApi::Dx11;
    default:
        return NativeApi::Dx12;
    }
}

// Include Vulkan games using the D3D12 bridge, even before native NR starts.
static bool DetailReuseUnavailableOnVulkan()
{
    return DlssNr::IsRunningVk() || State::Instance().api == Vulkan ||
           CurrentNativeApi() == NativeApi::Vulkan;
}

static bool NativeGameCallsUpscaler(NativeApi api)
{
    switch (api)
    {
    case NativeApi::Dx11:
        return GenericDepthDx11::GameCallsUpscaler();
    case NativeApi::Vulkan:
        return GenericDepthVk::GameCallsUpscaler();
    default:
        return GenericDepthDx12::GameCallsUpscaler();
    }
}

static DlssNrNativeMode::Finder NativeFinder(NativeApi api, bool depthWanted)
{
    using DlssNrNativeMode::FinderFor;

    switch (api)
    {
    case NativeApi::Dx11:
        return FinderFor(depthWanted, GenericDepthDx11::Installed(), GenericDepthDx11::InstallFailed());
    case NativeApi::Vulkan:
        return FinderFor(depthWanted, GenericDepthVk::Installed(), GenericDepthVk::InstallFailed());
    default:
        return FinderFor(depthWanted, GenericDepthDx12::Installed(), GenericDepthDx12::InstallFailed());
    }
}

static void NativeDepthStatus(NativeApi api)
{
    switch (api)
    {
    case NativeApi::Dx11:
        GenericDepthDx11::DrawStatus();
        break;
    case NativeApi::Vulkan:
        GenericDepthVk::DrawStatus();
        break;
    default:
        GenericDepthDx12::DrawStatus();
        break;
    }
}

static void NativeMotionStatus(NativeApi api)
{
    switch (api)
    {
    case NativeApi::Dx11:
        NativeMotionDx11::DrawStatus();
        break;
    case NativeApi::Vulkan:
        NativeMotionVk::DrawStatus();
        break;
    default:
        NativeMotionDx12::DrawStatus();
        break;
    }
}

static bool NativeNrOnlyRunning(NativeApi api)
{
    switch (api)
    {
    case NativeApi::Dx11:
        return NativeMotionDx11::NrOnlyRunning();
    case NativeApi::Vulkan:
        return NativeMotionVk::NrOnlyRunning();
    default:
        return NativeMotionDx12::NrOnlyRunning();
    }
}

// "FG only (game's upscaler)": frame generation is fed from a Vulkan-on-D3D12 backend's evaluate
// (upscalers/IFeature_VkwDx12.cpp), so the game's Vulkan upscaler call has to run through one. (Such a feature reports
// API::DX12: its upscaler type is what says so.)
static bool VulkanFeatureIsOn12()
{
    const auto feature = State::Instance().currentFeature;
    return feature != nullptr && FeatureProvider_Vk::IsOn12(feature->GetUpscalerType());
}

// The backend frame generation only runs the game's upscaler on, and the one to go back to when the mode is left.
static std::optional<Upscaler> g_backendBeforeFrameGenerationOnly;

static void SwitchVulkanBackend(Config* config, Upscaler backend)
{
    auto& state = State::Instance();
    config->VulkanUpscaler = backend;
    state.newBackend = backend;

    // A live feature is rebuilt on the game's next evaluate (inputs/NVNGX_DLSS_Vk.cpp); a later one is made with it.
    for (auto& changeBackend : state.changeBackend)
        changeBackend.second = true;
}

static void EnterFrameGenerationOnly(Config* config)
{
    const Upscaler current = config->VulkanUpscaler.value_or_default();

    // The same DLSS on D3D12 where the game's DLSS could run, FSR otherwise (FeatureProvider_Vk::On12For). Already a
    // D3D12 one: rebuilt all the same, so it is made on the bridge's device (IFeature_VkwDx12::CreateDx12Device).
    const Upscaler target = FeatureProvider_Vk::On12For(current);

    if (target != current)
        g_backendBeforeFrameGenerationOnly = current;

    SwitchVulkanBackend(config, target);
}

static void LeaveFrameGenerationOnly(Config* config)
{
    if (!g_backendBeforeFrameGenerationOnly.has_value())
        return;

    SwitchVulkanBackend(config, g_backendBeforeFrameGenerationOnly.value());
    g_backendBeforeFrameGenerationOnly.reset();
}

static void RenderNativeMode(Config* config, bool nrEnabled, bool& finishedPicture)
{
    using namespace DlssNrNativeMode;

    const NativeApi api = CurrentNativeApi();
    const bool gameUpscaler = NativeGameCallsUpscaler(api);
    const Shown shown =
        FromKeys({ config->DlssNrNativeDepthFinder.value_or_default(), config->DlssNrNativeMotion.value_or_default(),
                   config->DlssNrNativeInput.value_or_default(), config->DlssNrNativeUpscaler.value_or_default(),
          config->DlssNrNativeFrameGenerationOnly.value_or_default() });

    struct ModeChoice
    {
        Mode mode;
        Shown shown;
        const char* label;
    };

    static constexpr ModeChoice kModes[] = {
        { Mode::Off, Shown::Off, "关闭##nativemode" },
        { Mode::NrOnly, Shown::NrOnly, "仅 NR##nativemode" },
        { Mode::NrAndFrameGeneration, Shown::NrAndFrameGeneration, "NR＋超分与插帧##nativemode" },
        { Mode::FrameGenerationOnly, Shown::FrameGenerationOnly, "仅插帧（游戏超分）##nativemode" },
    };

    // FG only feeds frame generation from the game's Vulkan upscaler call through a Vulkan-on-D3D12 backend; a dxvk
    // game's upscaler calls are D3D11 ones, so it is not offered there.
    const bool vulkan = api == NativeApi::Vulkan && !VkPresentBridge::DxvkGame();
    const char* modeHelp = "游戏没有调用超分接口时，Optical F5Low 自动估计运动，让 NR 能运行。\n仅 NR：处理最终画面，不插帧，开销最低。\nNR＋超分与插帧：通过 OptiScaler 超分稳定画面，为插帧提供输入，开销较高。\n仅插帧（Vulkan）：为自己调用 DLSS、FSR 或 XeSS 的 Vulkan 游戏插帧。\n需要启用神经渲染；Vulkan 插帧须在游戏启动前配置。";
    MenuText::TextUnformatted("模式：");
    HelpMarker(modeHelp);
    const float rowRight = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    bool firstChoice = true;

    for (const auto& choice : kModes)
    {
        if (!Offered(choice.mode, vulkan))
            continue;

        const float width = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
                            MenuText::CalcTextSize(choice.label, nullptr, true).x +
                            ImGui::GetStyle().ItemSpacing.x;

        if (firstChoice || ImGui::GetCursorScreenPos().x + width <= rowRight)
            ImGui::SameLine();
        else
            ImGui::NewLine();
        firstChoice = false;

        const bool selectable = Selectable(choice.mode, gameUpscaler, vulkan);
        ImGui::BeginDisabled(!selectable);
        const bool clicked = MenuText::RadioButton(choice.label, shown == choice.shown);
        ImGui::EndDisabled();
        HelpMarker(modeHelp);

        if (clicked && selectable)
        {
            const Change change = ForClick(choice.mode, shown, finishedPicture);

            if (change.keys.has_value())
            {
                config->DlssNrNativeDepthFinder = change.keys->depthFinder;
                config->DlssNrNativeMotion = change.keys->motion;
                config->DlssNrNativeInput = change.keys->input;
                config->DlssNrNativeUpscaler = change.keys->upscaler;
                config->DlssNrNativeFrameGenerationOnly = change.keys->frameGenerationOnly;

                if (change.keys->frameGenerationOnly)
                    EnterFrameGenerationOnly(config);
                else if (shown == Shown::FrameGenerationOnly)
                    LeaveFrameGenerationOnly(config);
            }

            if (change.retryAfterFailure)
                RetryNr();

            if (change.finishedPicture.has_value())
            {
                finishedPicture = change.finishedPicture.value();
                config->DlssNrFinishedPicture = finishedPicture;
            }
        }
    }


    if (shown == Shown::Off)
    {
        if (gameUpscaler)
            MenuText::TextWrapped(vulkan ? "游戏已调用超分，NR 使用该接口运行。插帧请选择仅插帧（游戏超分）。"
                                      : "游戏已调用超分，NR 使用该接口运行，无需此模式。");

        return;
    }

    const bool depthWanted = config->DlssNrNativeDepthFinder.value_or_default();
    const Finder finder = NativeFinder(api, depthWanted);
    const bool depthRestart = DepthRestartWarning(shown, finder);
    // A dxvk game reporting NativeApi::Vulkan (NativeDxvkVulkan) gets its frame generation from the same present bridge as
    // a native Vulkan game, on dxvk's own Vulkan swapchain.
    const bool vulkanNoBridge = api == NativeApi::Vulkan && !VkPresentBridge::IsUp();
    const Warning warning =
        WarningFor(shown, nrEnabled, finishedPicture, DlssNr::NativeInputBlockedBySwapChainInterop(), gameUpscaler,
                   vulkanNoBridge, VulkanFeatureIsOn12());
    const char* warningText = nullptr;

    switch (warning)
    {
    case Warning::GameUpscaler:
        warningText = "游戏已调用超分接口，此模式让出控制，NR 使用该调用运行。请选择关闭，或关闭游戏超分后使用此模式。";
        break;
    case Warning::Dx11FrameGeneration:
        warningText = "此 D3D11 游戏的交换链已被 OptiScaler 插帧替换，仅 NR 模式不生效。请选择 NR＋超分与插帧。";
        break;
    case Warning::VulkanNeedsRestart:
        warningText = "等待游戏为插帧创建新交换链。若无变化，可切换一次窗口模式或分辨率；仍未启动时，请配置插帧输入与输出，保存后重启游戏。";
        break;
    case Warning::NeedsGameUpscaler:
        warningText = "F6 当前未接入游戏超分调用。若游戏已启用 DLSS、FSR 或 XeSS，请检查输入接入；菜单或加载画面也可能暂停调用。";
        break;
    case Warning::NeedsOn12Backend:
        warningText = "正在切换为游戏超分的 D3D12 副本，以提供插帧输入；下次超分调用时重建。";
        break;
    case Warning::NrDisabled:
        warningText = shown == Shown::NrOnly ? "上方神经渲染开关关闭，NR 不运行。"
                                             : "神经渲染关闭，插帧仍可使用此输入，但 NR 不运行。";
        break;
    case Warning::NeedsFinishedPicture:
        warningText = "仅 NR 模式需要最终画面处理位置。再次点击仅 NR 可自动设置。";
        break;
    case Warning::None:
        break;
    }

    // Enough for a wrapped warning of each kind; the usual two status lines leave the rest blank.
    const float slot = StatusSlotBegin();
    ImGui::PushStyleColor(ImGuiCol_Text, NextgenUi::ReadableTextColor(ImVec4(0.9f, 0.6f, 0.25f, 1.0f)));

    if (warningText != nullptr)
        MenuText::TextWrapped("%s", warningText);

    if (depthRestart)
        MenuText::TextWrapped("保存设置并重启游戏后才能使用深度；在此之前 NR 仅使用运动信息。");

    ImGui::PopStyleColor();

    if (shown == Shown::FrameGenerationOnly)
    {
        if (warning == Warning::None)
        {
            const auto feature = State::Instance().currentFeature;
            MenuText::TextDisabled("使用游戏超分接口的插帧（D3D12 上为 %s）。",
                                feature != nullptr ? feature->Name().c_str() : "超分后端");
        }

        StatusSlotEnd(slot, 4);
        return;
    }

    if (!depthRestart)
        NativeDepthStatus(api);

    if (shown == Shown::MotionOnly)
        MenuText::TextDisabled("仅估计运动，目前没有功能使用该结果。");
    else if (shown == Shown::NrAndFrameGeneration || warning == Warning::None)
        NativeMotionStatus(api);

    StatusSlotEnd(slot, 4);
}

// Model-resolution drag in flight, shared by the tier presets (which clear it) and the Model resolution slider.
static int pendingScale = -1;

// What every page needs. Read after the Enable checkbox so a toggle shows up the same frame.
// Translate runtime diagnostics only for display; backend messages remain stable in logs.
static std::string NrOptionsStatusChinese(std::string value)
{
    if (NextgenUi::English()) return value;
    const std::pair<const char*, const char*> words[] = {
        { "Waiting for a new successful DLSS source picture.", "等待新的成功 DLSS 源帧。" },
        { "NR is disabled; presenting the current source picture.", "NR 已关闭；显示当前源帧。" },
        { "NR bypass: the current source has no matching guide capture.", "当前源帧缺少匹配的深度／运动；保留当前原图。" },
        { "NR bypass: the presentation endpoint is unavailable or changing.", "NR 暂停：呈现端点不可用或正在切换。" },
        { "NR bypass: the current presentation buffer could not be acquired.", "NR 暂停：无法取得当前呈现缓冲区。" },
        { "NR bypass: the FG input and current source picture do not match.", "NR 暂停：插帧输入与当前源帧不匹配；保留当前原图。" },
        { "NR bypass: the current source queue or single-write claim is unavailable.", "NR 暂停：当前源帧的队列或单次写入凭据不可用。" },
        { "NR bypass: the current picture recording could not be reset.", "NR 暂停：当前画面录制无法安全重置。" },
        { "NR bypass: the current picture could not be recorded completely.", "NR 暂停：当前画面未能完整录制；保留当前原图。" },
        { "NR bypass: the current picture recording could not be closed.", "NR 暂停：当前画面录制无法封闭。" },
        { "Present -> NR -> detail retention -> FG (current source, one write).", "呈现输入 → NR → 细节保留 → FG；当前源帧已处理并发布一次。" },
        { "Preparing NR; presenting the current unmodified source.", "正在准备 NR；显示当前未修改的源帧。" },
        { "feedback-recorded-awaiting-publication", "细节已录制；等待真实提交／完成回执" },
        { "adjacent-published-source", "上一源帧历史已接受" },
        { "no-published-history", "当前帧重新播种；暂无已发布历史" },
        { "recording-awaits-completion-and-reset", "历史资源仍在使用；本帧使用新 NR 结果" },
        { "missing-current-frame-contract", "当前深度、运动、曝光或实际队列未获证明" },
        { "current-source-or-consumer-contract", "当前源帧或指南映射尚未验证" },
        { "unknown-native-metadata", "当前指南元数据不完整" },
        { "unsupported-or-blocked-route", "当前路线无法证明深度与运动；本帧使用新 NR 结果" },
        { "source-not-adjacent", "源帧不连续；丢弃旧细节" },
        { "contract-changed", "模型、Look 或输入映射变化；重新播种" },
        { "fresh-only-settings", "持续时长或强度为零；只使用本帧细节" },
        { "history-512-MiB-budget", "历史资源达到预算；本帧使用新 NR 结果" },
        { "Waiting for DLSS-G finished-picture inputs.", "等待 DLSS-G 的最终画面输入。" },
        { "This option needs a native DirectX 12 game.", "此选项需要原生 DirectX 12 游戏。" },
        { "The graphics device changed. Restart the game to use this option.", "图形设备已更换，请重启游戏后使用此选项。" },
        { "Waiting for the previous picture to finish.", "等待上一幅画面完成。" },
        { "Could not prepare the finished-picture option.", "无法初始化最终画面处理。" },
        { "Waiting for the game's movement data.", "等待游戏运动数据。" },
        { "The game's depth or movement data is not supported.", "不支持游戏提供的深度或运动数据。" },
        { "The upscaled changes could not be saved.", "无法保存超分后的画面修改。" },
        { "The graphics queue stopped. Restart the game to retry.", "图形队列已停止，请重启游戏重试。" },
        { "This screen colour format is not supported.", "不支持此画面颜色格式。" },
        { "Skipping frames: the game's picture is still rendering when it is presented.", "已跳过：呈现时游戏画面仍在渲染。" },
        { "Could not prepare the finished picture.", "无法准备最终画面。" },
        { "Could not finish the picture. Restart the game to retry.", "无法完成画面处理，请重启游戏重试。" },
        { "NR bypass: no current capture and no safe fresh-color slot/sizing.", "NR 暂停：缺少当前捕获，且没有可安全使用的当前画面槽位或尺寸信息。" },
        { "Applying NR to current color; temporal history reset (native guides unavailable).", "正在对当前画面运行 NR；原生引导缺失，已重置跨帧历史。" },
        { "Native input needs Finished picture and Enable Neural Rendering on.", "原生输入需要同时开启最终画面模式和神经渲染。" },
        { "Native input does not support HDR10 output yet.", "原生输入暂不支持 HDR10 输出。" },
        { "DLSS-G finished-picture sizes do not match.", "DLSS-G 最终画面尺寸不匹配。" },
        { "The frame-generation input colour encoding is not supported (SDR/scRGB required).", "不支持此插帧输入颜色编码（需要 SDR 或 scRGB）。" },
        { "Could not allocate the DLSS-G finished-picture textures.", "无法分配 DLSS-G 最终画面纹理。" },
        { "waiting for GPU completion slots; clean SR frame retained", "等待 GPU 完成槽位，保留未修改的超分画面" },
        { "waiting for retired GPU work; clean SR frame retained", "等待旧 GPU 工作完成，保留未修改的超分画面" },
        { "allocation failed; clean SR frame retained", "分配失败，保留未修改的超分画面" },
        { "waiting for NR evaluation; clean SR frame retained", "等待 NR 运算，保留未修改的超分画面" },
        { "composition failed; clean frame retained", "合成失败，保留未修改画面" },
        { "private DLSS creation failed: ", "内部 DLSS 创建失败：" },
        { "private DLSS evaluation failed: ", "内部 DLSS 运算失败：" },
        { "private DLSS created; waiting for a later submission epoch", "内部 DLSS 已创建，等待下一次提交" },
        { "private exposure initialization failed", "内部曝光初始化失败" },
        { "asynchronous residual DLSS evaluation failed", "异步残差 DLSS 运算失败" },
        { "sample-and-hold guide initialization failed", "采样保持引导初始化失败" },
        { "waiting for a same-device direct queue identity", "等待同设备的直接队列标识" },
        { "GPU completion tracking is unavailable or full", "GPU 完成追踪不可用或已满" },
        { "GPU descriptor slots are still in flight; bypassing this frame", "GPU 描述符槽位仍在使用，已旁路此帧" },
        { "measuring...", "正在测量…" },
        { "disabled", "已关闭" },
        { "warming up", "正在预热" },
        { "waiting", "等待中" },

        { "native scene reuse: corresponding base frame is not verified", "原生场景复用暂停：尚未验证对应的真实基帧" },
        { "native scene reuse: current depth is not verified", "原生场景复用暂停：尚未验证当前帧深度" },
        { "native scene reuse: guide coordinate contract is not verified", "原生场景复用暂停：尚未验证运动与深度坐标约定" },
        { "native scene reuse: manual motion mapping has no verified phase", "原生场景复用暂停：手动运动翻转或反向后的采样相位尚未验证" },
        { "native scene reuse: guide jitter phase is not verified", "原生场景复用暂停：尚未验证引导纹理的抖动与采样相位" },
        { "native scene reuse: guide phase contains non-finite values", "原生场景复用暂停：引导采样相位数值无效" },
        { "unavailable in Finished Picture without Optical F5Low", "最终画面复用暂停：场景引导与当前成品画面的对应关系尚未认证（不代表模型缺少运动输入）" },
        { "unavailable while NR runs before SR", "超分之前的 NR 不支持细节复用" },
        { "off below the minimum frame rate", "真实帧率低于复用最低帧率，正在逐帧计算" },
        { "available, kept on with frame generation", "复用可用，插帧时保持开启" },
        { "available", "复用可用" },
        { "Waiting for a finished picture.", "等待最终画面输入。" },
        { "Applying NR to DLSS-G finished-picture inputs (6X compatible).", "正在处理 DLSS-G 的最终画面输入（兼容 6X，保留 HUD）。" },
        { "Preparing NR for the frame-generation input.", "正在为插帧输入准备 NR 模型。" },
        { "NR changes are hidden.", "NR 正在计算，但画面效果已隐藏。" },
        { "NR bypass: the final picture output could not be written.", "NR 已计算，但当前画面输出失败。" },
        { "not seen yet", "尚未检测到" },
        { "plain FP16", "普通 FP16" },
        { "off for this session (unexpected NR launch order)", "本次会话已关闭（NR 内核调用顺序异常）" },
        { "computed ", "重新计算 " },
        { ", reused ", " 次，复用 " },
    };
    for (const auto& word : words)
    {
        size_t pos = 0;
        while ((pos = value.find(word.first, pos)) != std::string::npos)
        {
            value.replace(pos, std::strlen(word.first), word.second);
            pos += std::strlen(word.second);
        }
    }
    return value;
}


struct NrCommon
{
    bool enabled;
    // Read early (checkbox itself is drawn down in NR Options) so the running-status block
    // can already report finished-picture-specific text.
    bool finishedPicture;
    // Either backend. The two keep separate state, and on a native Vulkan game the D3D12 side
    // is never touched -- so asking only that one reports "waiting for the upscaler" over a pass
    // that is demonstrably running.
    bool vulkan;
};

static NrCommon ReadNrCommon(Config* config)
{
    return { config->DlssNrEnabled.value_or_default(),
             DlssNr::UsesGenshinPresentPipeline() || config->DlssNrFinishedPicture.value_or_default(),
             DlssNr::IsRunningVk() };
}

static void RenderEnableToggle(Config* config)
{
    bool enabled = config->DlssNrEnabled.value_or_default();
    const auto keyName = MenuStyle::ShortcutName(config->DlssNrToggleKey.value_or_default());
    const auto keyHint = MenuText::Format("快捷键：%s", keyName.c_str());
    if (MenuText::Checkbox("启用神经渲染", &enabled, keyHint.c_str()))
        config->DlssNrEnabled = enabled;

    HelpMarker("开启 DLSS 神经渲染。需要 nvngx_dlssnr.dll 以及随附的 nvngx.dll_dlssnr.dll。");
    if (config->IsStarRailInputProfile())
    {
        const bool placement = config->DlssNrFinishedPicture.value_or_default() &&
            !config->DlssNrRunBeforeSr.value_or_default() && !config->DlssNrDeferredDlss.value_or_default();
        bool resident = config->StarRailForceNRResident.ObservedValue();
        ImGui::BeginDisabled(!placement);
        if (MenuText::Checkbox("强制 NR 常驻（崩铁）", &resident))
            config->StarRailForceNRResident = resident;
        ImGui::EndDisabled();
        HelpMarker("仅用于成品画面 NR。DLSS 引导缺失时继续处理当前画面；不改变 SR、FG 或 NR 注入位置。");
        if (!placement) MenuText::TextDisabled("常驻需要先选择成品画面 NR。");
    }
}

// Legacy bridge UI must not constrain genuine DLSS inputs, even in Genshin.
// Executable identity affects labels only when the bridge is enabled.
static bool GenshinBridgeMenuProcess()
{
    if constexpr (!F6InputPolicy::FsrBridgeEnabled) return false;
    static const bool value = [] {
        const auto executable = Util::ExePath().filename().wstring();
        return _wcsicmp(executable.c_str(), L"YuanShen.exe") == 0 ||
               _wcsicmp(executable.c_str(), L"GenshinImpact.exe") == 0;
    }();
    return value;
}

// A loaded model is not proof of current Genshin final-picture consumption or visible output.
static bool GenshinPresentedStatus(const NrCommon& nr)
{
    return F6InputPolicy::FsrBridgeEnabled && nr.finishedPicture && !nr.vulkan &&
           GenshinLegacy::GetStatus().effectiveEnabled &&
           State::Instance().swapchainInteropApi == SwapchainInteropApi::None;
}

// Both the current present pipeline and the legacy bridge report publication.
static bool CurrentPresentedStatus(const NrCommon& nr)
{
    return (nr.finishedPicture && !nr.vulkan && DlssNr::UsesGenshinPresentPipeline()) ||
           GenshinPresentedStatus(nr);
}

static void RenderFinishedPictureStatus(const NrCommon& nr, const char* fallback = "")
{
    if (!nr.finishedPicture || nr.vulkan) return;
    const auto status = DlssNr::FinishedPictureStatus();
    if (!status.empty()) MenuText::TextWrapped("%s", NrOptionsStatusChinese(status).c_str());
    else if (fallback[0]) MenuText::TextWrapped("%s", fallback);
}

// Both menu layouts consume the same bounded completed-sample publication.
// Model residency and an old local timer are not current processing evidence.
static std::optional<double> MenuNrGpuTime(bool vulkan)
{
    if (vulkan) return DlssNr::LastGpuTimeVk();
    const auto completed = DlssNr::ReadGpuTiming();
    return completed.publishedMs != 0 ? std::optional<double>(completed.milliseconds) : std::nullopt;
}

// The full running-status block, for the Status & Presets page.
static void RenderRunningStatus(Config* config, const NrCommon& nr)
{
    const bool enabled = nr.enabled;
    const bool finishedPicture = nr.finishedPicture;
    const bool vulkan = nr.vulkan;
    const auto ms = MenuNrGpuTime(vulkan);

    // Turning the pass off does not release the model, so the feature handle stays alive and
    // IsRunning keeps answering yes. Reporting a cost from that was wrong in the way that matters
    // most: the toggle is how anyone A/Bs this, so the one moment the number is read is the one
    // moment it describes the frame before last.
    if (!enabled)
    {
        MenuText::TextDisabled("NR 已关闭。");
    }
    else if (!DlssNr::IsRunning() && !vulkan)
    {
        const auto feature = State::Instance().currentFeature;
        const bool nativeVk = feature && feature->Api() == API::Vulkan && !feature->IsWithDx12();
        const char* reason = nativeVk ? DlssNr::FailureReasonVk() : DlssNr::FailureReason();

        if (reason[0] != 0)
        {
            MenuText::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "本次会话已停用：%s。", NrOptionsStatusChinese(reason).c_str());
            ImGui::SameLine();

            if (nativeVk && !DlssNr::RetryableVk())
                MenuText::TextUnformatted("重试无法解决，请重启游戏。");
            else if (MenuText::SmallButton("重试"))
                RetryNr();
        }
        else if (feature && feature->Api() == API::DX11 && !feature->IsWithDx12())
        {
            MenuText::TextWrapped("NR 在 D3D11 上需要 D3D12 桥接，请选择标注 w/Dx12 的超分器并重启游戏。");
        }
        else if (nativeVk && config->DlssNrDeferredDlss.value_or_default())
        {
            MenuText::TextWrapped("若要使用原生 Vulkan NR，请关闭“SR 前生成、SR 后应用（DLSS）”。");
        }
        else if (enabled && finishedPicture)
        {
            // On the finished picture (Optical F5Low's NR only among them) there is no upscaler to wait for: what NR
            // says about the picture is the reason it has not started (only the log had it).
            RenderFinishedPictureStatus(nr, "等待成品画面。");
        }
        else if (enabled)
            MenuText::TextUnformatted("等待超分器运行。");
    }
    else if (!vulkan && !ms.has_value())
    {
        // Residency is not activity. The completed-sample publication expires
        // even if no later Dispatch arrives to clear the old local timer.
        MenuText::TextDisabled("模型已就绪；最近 5 秒没有可用的 NR GPU 完成样本。");
        RenderFinishedPictureStatus(nr);
    }
    else if (CurrentPresentedStatus(nr))
    {
        // A completed model timer can outlive a refused final endpoint. Show
        // the current publication status even while the model remains resident.
        RenderFinishedPictureStatus(nr);
        MenuText::TextDisabled("最近 NR 总 GPU 区间：%.2f ms。[呈现模式]", *ms);
        const char* reason = DlssNr::FailureReason();
        if (reason[0] != 0)
            MenuText::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "本次会话已停用：%s。", NrOptionsStatusChinese(reason).c_str());
    }
    else
    {
        // The elapsed time belongs here rather than only in the upscaler's breakdown: that tooltip needs
        // OptiScaler's own upscaler to have run, and with native DLSS passing through there is
        // nothing in it to hang this off.
        // Either backend's timer. They measure the same thing by different means, and only one
        // of them is running.

        // With "应用模型效果" off the pass STILL RUNS (so Hold-frame A/B can toggle its edit on
        // a frozen frame) -- it only outputs the clean frame.
        // Enable Neural Rendering off stops the work.
        const char* runSuffix =
            !config->DlssNrApplyModel.value_or_default() ? "（模型运行中，编辑已隐藏）" : "";

        // With Reuse detail between frames, full and reused frames alternate, so one reading is either the heavy or
        // the light one; the average over the recent frames is the real per-frame cost.
        const bool detailStatusRequested = config->DlssNrDetailReuse.value_or_default() ||
            (!vulkan && config->DlssNrPresentedTemporalDetail.value_or_default());
        const auto detailReuse = !detailStatusRequested ? DlssNr::DetailReuseInfo {}
                                 : vulkan               ? DlssNr::DetailReuseStatusVk()
                                                        : DlssNr::DetailReuseStatus();
        {
            ScopedMonoFont monoFont {};

            if (detailReuse.active && detailReuse.averageMs > 0.0)
                MenuText::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f),
                                   "运行中 - 平均每帧 %.2f ms（%.2f ~ %.2f）%s",
                                   detailReuse.averageMs, detailReuse.lightMs, detailReuse.heavyMs, runSuffix);
            else if (ms.has_value())
                MenuText::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "运行中%s - 耗时 %.2f ms%s",
                                   vulkan ? "（Vulkan 原生）" : "", ms.value(), runSuffix);
            else if (vulkan)
                // Measured but not yet read: the first few frames are still in the query ring.
                MenuText::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "原生运行于 Vulkan - 已运行 %llu 帧%s",
                                   DlssNr::FramesVk(), runSuffix);
            else
                MenuText::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "运行中。%s", runSuffix);
        }

        HelpMarker("NR 每帧 GPU 时间，包含等待其他 GPU 工作的时间。真实开销请查看 FPS。");
        if (finishedPicture)
            MenuText::TextDisabled("包含与其他 GPU 工作共享的时间。");

        if (!vulkan && DlssNr::BackendName()[0] != 0)
            MenuText::TextDisabled("模型后端：%s", DlssNr::BackendName());
    }
}

// The same current status and completed timing, for the other pages.
static void RenderStatusLine(const NrCommon& nr)
{
    const auto ms = MenuNrGpuTime(nr.vulkan);
    ScopedMonoFont monoFont {};

    if (!nr.enabled)
        MenuText::TextDisabled("NR 已关闭。");
    else if (!DlssNr::IsRunning() && !nr.vulkan)
    {
        MenuText::TextDisabled("NR 未运行，请见“状态与预设”页。");
        RenderFinishedPictureStatus(nr);
    }
    else if (!nr.vulkan && !ms.has_value())
    {
        MenuText::TextDisabled("模型已就绪；最近 5 秒没有可用的 NR GPU 完成样本。");
        RenderFinishedPictureStatus(nr);
    }
    else if (CurrentPresentedStatus(nr))
    {
        RenderFinishedPictureStatus(nr);
        if (ms && std::isfinite(*ms) && *ms >= 0.0)
            MenuText::TextDisabled("最近 NR 总 GPU 区间：%.2f ms。[呈现模式]", *ms);
        else
            MenuText::TextDisabled("最近 NR 总 GPU 区间：暂无测量。[呈现模式]");
    }
    else if (ms.has_value())
        MenuText::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "运行中%s - 耗时 %.2f ms",
                           nr.vulkan ? "（Vulkan 原生）" : "", ms.value());
    else
        MenuText::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "运行中。");
}

// The mode in effect, as the Optical F5Low page and the summary line name it.
static const char* NativeModeName(DlssNrNativeMode::Shown shown)
{
    switch (shown)
    {
    case DlssNrNativeMode::Shown::NrOnly:
        return "仅 NR";
    case DlssNrNativeMode::Shown::NrAndFrameGeneration:
        return "NR＋超分与插帧";
    case DlssNrNativeMode::Shown::MotionOnly:
        return "仅运动估计";
    case DlssNrNativeMode::Shown::FrameGenerationOnly:
        return "仅插帧（游戏超分）";
    default:
        return "关闭";
    }
}

// Compress screen edges (shaders/dlssnr/DlssNr_Spatial.h): what the backend that is running last published, and the
// words for it. The runtime only publishes a status code; every word is here. Grey is "not running / not available",
// green is "on", amber is "not applied" or "turned itself off" -- never red, because NR itself is still running.
static DlssNr::Spatial::Published EdgeCompressionNow(const NrCommon& nr)
{
    return nr.vulkan ? DlssNr::EdgeCompressionStatusVk() : DlssNr::EdgeCompressionStatus();
}

// The model resolution NR applies, as a fraction: what the layout was built for when a frame has run (Auto included),
// else the slider.
static float EdgeCompressionScale(const Config* config, const NrCommon& nr)
{
    const auto published = EdgeCompressionNow(nr);

    if (published.nativeW != 0 && std::isfinite(published.scale) && published.scale > 0.0f)
        return published.scale;

    return config->DlssNrWorkingScale.value_or_default();
}

struct EdgeCompressionLine
{
    enum class Tone { Grey, Green, Amber } tone = Tone::Grey;
    std::string text;
};

static EdgeCompressionLine EdgeCompressionText(const Config* config, const NrCommon& nr)
{
    using DlssNr::Spatial::Status;
    EdgeCompressionLine line;

    if (!nr.enabled)
    {
        line.text = "NR 已关闭。";
        return line;
    }

    const auto published = EdgeCompressionNow(nr);
    char buffer[256];

    switch (published.status)
    {
    case Status::Off:
        line.text = "等待 NR 处理画面。";
        break;
    case Status::Proxy:
        line.text = "驱动代理后端不支持此功能。";
        break;
    case Status::GameFgInput:
        line.tone = EdgeCompressionLine::Tone::Amber;
        line.text = "原生 DLSS-G 输入暂时禁用边缘压缩，以规避已报告的 GPU 挂起。NR 和插帧继续运行。";
        break;
    case Status::Active:
    {
        const double share = 100.0 * published.modelW * published.modelH /
                             std::max(1.0, (double) published.ordinaryW * (double) published.ordinaryH);
        std::snprintf(buffer, sizeof(buffer), MenuText::Translate("NR 处理 %ux%u，原始 %ux%u（%.0f%% 像素）。").c_str(),
                      published.modelW, published.modelH, published.ordinaryW, published.ordinaryH, share);
        line.tone = EdgeCompressionLine::Tone::Green;
        line.text = buffer;
        break;
    }
    case Status::BadSettings:
        line.tone = EdgeCompressionLine::Tone::Amber;
        line.text = "未应用：布局参数超出范围。请选择布局预设或重置高级值。";
        break;
    case Status::TooSmall:
        line.tone = EdgeCompressionLine::Tone::Amber;
        line.text = "未应用：模型某一方向小于画面的四分之一。请提高模型分辨率或 NR 处理范围。";
        break;
    case Status::NothingToCompress:
        line.tone = EdgeCompressionLine::Tone::Amber;
        line.text = "未应用：处理范围已覆盖完整画面，无需压缩。";
        break;
    case Status::ThinEdge:
        line.tone = EdgeCompressionLine::Tone::Amber;
        line.text = "未应用：边缘不足一个像素。请缩小清晰中央区域或扩大 NR 处理范围。";
        break;
    case Status::TurnedOffResources:
        line.tone = EdgeCompressionLine::Tone::Amber;
        line.text = "自动关闭：无法创建着色器或纹理。NR 继续运行。";
        break;
    case Status::TurnedOffDispatch:
        line.tone = EdgeCompressionLine::Tone::Amber;
        line.text = "自动关闭：GPU 上某个压缩步骤失败。NR 继续运行，更改布局可重试。";
        break;
    case Status::TurnedOffModel:
        line.tone = EdgeCompressionLine::Tone::Amber;
        line.text = "自动关闭：NR 拒绝压缩后的画面。NR 继续运行，更改布局可重试。";
        break;
    }

    // Asked for and nothing published yet is "waiting"; anything published while it is off in the settings is stale.
    if (!config->DlssNrSpatialCompression.value_or_default())
    {
        line.tone = EdgeCompressionLine::Tone::Grey;
        line.text = "关闭。";
    }

    return line;
}

static void EdgeCompressionDrawText(const EdgeCompressionLine& line)
{
    using Tone = EdgeCompressionLine::Tone;
    const ImVec4 colour = line.tone == Tone::Green   ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f)
                          : line.tone == Tone::Amber ? ImVec4(1.0f, 0.85f, 0.3f, 1.0f)
                                                     : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    MenuText::TextWrapped("%s", line.text.c_str());
    ImGui::PopStyleColor();
}

// The "Reuse detail between frames" checkbox and its help, on the multi-frame NR reuse page; the Optical F5Low page shows its
// state and links here. Returns whether it is on.
static bool RenderDetailReuseToggle(Config* config)
{
    bool detailReuse = config->DlssNrDetailReuse.value_or_default();
    if (MenuText::Checkbox("跨帧复用细节（实验功能）", &detailReuse))
        config->DlssNrDetailReuse = detailReuse;
    HelpMarker("隔帧运行完整模型，并用运动信息把上一次的细节移到中间帧，降低开销。运动或新出现的区域可能发生细节跳变。\n建议使用一次处理；不支持超分之前的 NR。可展开调试项微调。");
    return detailReuse;
}

// Scene cuts the game does not flag (shaders/dlssnr/DlssNr_SceneCut.inl), on the NR Options page. The detector runs from
// SceneCut=1, where it only counts; the checkbox is 2, where it also acts. 0 (nothing runs) is set in the ini.
static void RenderSceneCutToggle(Config* config)
{
    if (DlssNr::IsRunningVk())
    {
        MenuText::TextDisabled("补充检测镜头切换：仅支持 D3D12 游戏");
        return;
    }

    const uint32_t setting = config->DlssNrSceneCut.value_or_default();
    bool act = setting >= 2;

    ImGui::BeginDisabled(setting == 0);
    if (MenuText::Checkbox("游戏未报告镜头切换时重置NR###游戏未报告镜头切换时重置 NR（实验功能）", &act))
        config->DlssNrSceneCut = act ? 2u : 1u;
    ImGui::EndDisabled();

    HelpMarker("检测游戏未报告的镜头切换并重置 NR 历史，避免旧场景残留。\n关闭此开关时仍统计切换次数；仅支持 D3D12。");
}

// Optical F5Low's own Reflex calls (native/NativeLowLatency.h): the switch, the method in use and, where the driver
// reports it, the measured latency. Never touches the fakenvapi settings; it only points to them.
static void RenderLowLatencySection(Config* config)
{
    if (!MenuText::TreeNodeEx("低延迟##nativelowlatency", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    using native::lowlatency::CapOwner;
    using native::lowlatency::Decision;
    using native::lowlatency::Setting;

    // Off / On / On + Boost. On is the ini's auto or true (any game without Reflex, not only Optical F5Low): the last
    // one seen comes back after Off. Off keeps the Boost choice for the next On.
    static const char* modeNames[] = { "关闭", "开启", "开启＋Boost" };
    static Setting lastOn = Setting::Auto;
    const auto setting = config->DlssNrNativeLowLatency.value_or_default();

    if (setting != Setting::Off)
        lastOn = setting;

    int mode = setting == Setting::Off ? 0 : (config->DlssNrNativeLowLatencyBoost.value_or_default() ? 2 : 1);

    if (MenuText::Segmented("降低游戏延迟##nativelowlatency", &mode, modeNames, IM_ARRAYSIZE(modeNames)))
    {
        if (mode == 0)
        {
            config->DlssNrNativeLowLatency = Setting::Off;
        }
        else
        {
            if (setting == Setting::Off)
                config->DlssNrNativeLowLatency = lastOn;

            config->DlssNrNativeLowLatencyBoost = mode == 2;
        }
    }

    HelpMarker("补充游戏缺失的低延迟调用：NVIDIA 使用 Reflex，其他显卡使用 Anti-Lag 2、XeLL 或 LatencyFlex；游戏已有 Reflex 时让出控制。\nXeFG 始终使用 XeLL，因此无需 Optical F5Low 模式也会运行。此时游戏的 Reflex 调用转交 XeLL；可在 fakenvapi 中强制关闭。\n开启＋Boost 让 NVIDIA GPU 保持较高频率，略降低延迟并增加功耗；其他方案忽略 Boost。");

    const auto status = native::lowlatency::GetStatus();

    // Run with no path yet: the first frame, before the first Reflex call went out
    const char* reason = status.decision == Decision::Run
                             ? (status.path[0] != '\0' ? status.path : "正在启动")
                             : native::lowlatency::DecisionText(status.decision, status.gameReflexFeedsXell,
                                                                status.forceReflexDisabled);

    MenuText::TextWrapped("状态：[%s]：%s", NrOptionsStatusChinese(native::lowlatency::StatusLabel(status.decision)).c_str(), NrOptionsStatusChinese(reason).c_str());

    if (status.decision == Decision::Run && status.hasLatency)
    {
        // Through fakenvapi there are no GPU times: the frame's simulation start to its present's end
        if (status.latencyEstimated)
            MenuText::Text("模拟至呈现延迟（按帧标记估算）：%.1f ms", status.latencyMs);
        else
            MenuText::Text("实测延迟：%.1f ms", status.latencyMs);
    }

    // On NVIDIA and AMD XeLL does nothing while XeFG generates no frames (paused, off, exclusive full screen, or
    // fakenvapi's own XeLL with no XeFG)
    if (status.decision == Decision::Run && status.xellIdle)
        MenuText::TextDisabled("此 GPU 上 XeFG 未生成帧时，XeLL 不生效。");

    // Who keeps the frame cap while we run: one wait per frame (NativeLowLatencyRule.h PlanCap)
    if (const float cap = config->FramerateLimit.value_or_default();
        status.decision == Decision::Run && status.path[0] != '\0' && cap > 0.0f)
    {
        MenuText::TextDisabled("帧率上限 %.0f fps：由 %s 控制", cap,
                            status.capOwner == CapOwner::Limiter ? "OptiScaler 限帧器" : status.path);
    }

    // XeFG cannot run without XeLL (Intel), so a card that has real Reflex gets XeLL with it: say what gives Reflex.
    // Read only the GPU data discovered before the menu frame. Do not trigger DXGI/NvAPI discovery from ImGui.
    const auto gpu = IdentifyGpu::getCachedPrimaryGpu();

    if (gpu.vendorId == VendorId::Nvidia && !fakenvapi::isUsingAsMainNvapi() &&
        State::Instance().activeFgOutput == FGOutput::XeFG)
    {
        // DLSS frame generation: RTX 40 (Ada) and later
        const bool dlssFrameGeneration = gpu.nvidiaArchInfo.architecture_id >= NV_GPU_ARCHITECTURE_AD100;
        MenuText::TextWrapped(dlssFrameGeneration ? "XeFG 始终使用 XeLL；NVIDIA Reflex 请搭配 DLSS 或 FSR 插帧。"
                                               : "XeFG 始终使用 XeLL；NVIDIA Reflex 请搭配 FSR 插帧。");
    }
    else if (fakenvapi::isUsingAsMainNvapi() || State::Instance().activeFgOutput == FGOutput::XeFG ||
             status.decision == Decision::ForceReflexDisabled || status.decision == Decision::ForceXell)
    {
        MenuText::TextWrapped("低延迟方案可在 fakenvapi 设置中选择。");
    }

    ImGui::TreePop();
}

// Optical F5Low: NR for a game that makes no upscaler call of its own (the native modes). The mode selector sits at the
// top, then the Advanced settings (depth, flow tuning).
static void RenderF5LowPage(Config* config, const NrCommon& nr)
{
    MenuText::SeparatorText("Optical F5Low（实验功能）");
    MenuText::TextWrapped("在没有调用超分接口的游戏中运行 NR。");
    MenuText::TextWrapped(
        "Optical F5Low 比较前后帧来估计运动，尽量使用游戏深度、保持静止 HUD 并检测镜头切换。NR 使用这些运动信息；第二种模式还运行 OptiScaler 超分和插帧。支持 D3D11、D3D12 和 Vulkan。");

    bool finishedPicture = nr.finishedPicture;
    RenderNativeMode(config, nr.enabled, finishedPicture);

    RenderLowLatencySection(config);

    // Detail reuse on Optical F5Low's motion: shown here because this is where its saving matters most (NR on the whole
    // finished picture). Only its state: the checkbox and its fine-tuning live on the multi-frame NR reuse page.
    MenuText::SeparatorText("降低 NR 开销");
    const bool reuseVk = DetailReuseUnavailableOnVulkan();
    if (reuseVk)
        MenuText::TextWrapped(NextgenUi::English()
            ? "Multi-frame NR reuse: unsupported on Vulkan; saved settings are retained."
            : "多帧 NR 复用：Vulkan 暂未支持；已保存设置保持不变。");
    else if (!config->DlssNrDetailReuse.value_or_default() &&
             !config->DlssNrPresentedTemporalDetail.value_or_default())
        MenuText::TextUnformatted("跨帧复用与细节持续反馈：关闭");
    else
    {
        const auto status = DlssNr::DetailReuseStatus();
        if (!status.why.empty())
            MenuText::TextWrapped("细节历史：%s", NrOptionsStatusChinese(status.why).c_str());
        else if (status.active)
            MenuText::Text("细节历史：已运行（完整 %llu | 复用 %llu%s）", status.full, status.reused,
                        status.holding ? " | 快速运动时暂停" : "");
        else
            MenuText::TextUnformatted("细节历史：等待 NR 输入");
    }
    HelpMarker(reuseVk
        ? (NextgenUi::English() ? "Multi-frame NR reuse is unsupported on Vulkan; its controls are hidden."
                               : "Vulkan 暂不支持多帧 NR 复用；相关控件已隐藏。")
        : "隔帧运行模型，约降低一半 NR 开销；Optical F5Low 会丢弃运动不可靠区域的复用细节。在多帧NR复用页面设置。");
    if (MenuText::SmallButton("前往多帧NR复用##f5lowdetailreuse"))
        MenuPages::RequestPage(MenuPages::Page::NrReuse);

    // Compress screen edges is another way to lighten NR; it works on the finished picture too. Only its state here.
    {
        const bool compress = config->DlssNrSpatialCompression.value_or_default();
        const EdgeCompressionLine line = EdgeCompressionText(config, nr);
        const std::string text = std::string("边缘压缩：") + (compress ? line.text : std::string("关闭"));
        EdgeCompressionDrawText({ compress ? line.tone : EdgeCompressionLine::Tone::Grey, text });
        HelpMarker("降低 NR 处理像素数，保持中央清晰并压缩边缘。节省 GPU 时间，边缘可能稍软。在 NR 全局页面设置。");
        if (MenuText::SmallButton("前往 NR 全局##f5lowcompress"))
            MenuPages::RequestPage(MenuPages::Page::NrStatus);
    }

    // The page is Optical F5Low's own, so every control shows and the section starts open.
    if (MenuText::TreeNodeEx("高级设置##nativeinputadvanced", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool pictures = config->DlssNrNativeDebugView.value_or_default();

        if (MenuText::Checkbox("显示运动与可信度图##nativedebugview", &pictures))
            config->DlssNrNativeDebugView = pictures;

        if (ImGui::IsItemHovered())
            MenuText::SetTooltip("%s", "在此页显示运动估计与可信度遮罩，有少量 D3D12 GPU 开销。显示深度还需开启选中深度预览并重启。");

        switch (CurrentNativeApi())
        {
        case NativeApi::Dx11:
            GenericDepthDx11::DrawAdvancedUi();
            NativeMotionDx11::DrawAdvancedUi();
            break;
        case NativeApi::Vulkan:
            GenericDepthVk::DrawAdvancedUi();
            NativeMotionVk::DrawAdvancedUi();
            break;
        default:
            GenericDepthDx12::DrawAdvancedUi();
            NativeMotionDx12::DrawAdvancedUi();
            break;
        }

        ImGui::TreePop();
    }
}

static void RenderPassCount(Config* config, float menuResScale)
{
    ImGui::PushItemWidth(220.0f * menuResScale);
    constexpr unsigned int passLimit = MaxPassCount;

    {
        int passes = (int) std::clamp(config->DlssNrPasses.value_or_default(), 1u,
                                      passLimit);
        const ImVec4 colour = passes <= 1   ? ImVec4(0.35f, 0.88f, 0.38f, 1.0f)
                              : passes == 2 ? ImVec4(0.95f, 0.70f, 0.20f, 1.0f)
                                            : ImVec4(0.92f, 0.30f, 0.25f, 1.0f);

        ImGui::PushStyleColor(ImGuiCol_Text, colour);
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, colour);

        if (MenuText::SliderInt("模型处理次数", &passes, 1, (int) passLimit,
                             passes == 1 ? "%d（正常）" : "%dx 模型开销"))
            config->DlssNrPasses = (uint32_t) std::clamp(passes, 1, (int) passLimit);

        ImGui::PopStyleColor(2);

        // Reset's own label stays plain text -- placed after PopStyleColor so the
        // passes-based warning colour above doesn't tint it too.
        ImGui::SameLine();
        if (MenuText::SmallButton("重置##modelpasses", !NextgenSliderStyle::IsDefault(passes, 1)))
            config->DlssNrPasses = 1u;

        HelpMarker("每个真实帧运行模型的次数。越高效果可能更强，GPU 开销越高。默认 1。");
    }

    ImGui::PopItemWidth();
}

// Feedback belongs to the reuse page; preset application helpers remain available to the product.
static void RenderPassFeedback(Config* config, float menuResScale)
{
    ImGui::PushItemWidth(220.0f * menuResScale);

    {
        // Disabled rather than hidden at Passes == 1: the control exists, it just has nothing to
        // do yet (there is no boundary between passes to damp), which is a clearer statement
        // than making it vanish and reappear as Passes changes.
        const bool noBoundary = VisiblePassCount(config) <= 1u;
        ImGui::BeginDisabled(noBoundary);
        float feedback = config->DlssNrPassFeedback.value_or_default();
        if (MenuText::SliderFloat("后续处理反馈", &feedback, 0.0f, 1.0f,
                               feedback >= 1.0f ? "%.2f（完整效果）" : "%.2f"))
            config->DlssNrPassFeedback = std::clamp(feedback, 0.0f, 1.0f);
        ImGui::SameLine();
        if (MenuText::SmallButton("重置##passfeedback", !NextgenSliderStyle::IsDefault(feedback, 1.0f)))
            config->DlssNrPassFeedback = 1.0f;
        ImGui::EndDisabled();

        HelpMarker("每次额外处理有多少变化传给下一次。1 表示完整效果，较低时变化更温和稳定。默认 1；一次处理时不生效。");
    }

    ImGui::PopItemWidth();

}

// NR Exposure, drawn at the end of the NR Input page.
static void RenderExposureSection(Config* config, float menuResScale)
{
    MenuText::SeparatorText("NR 曝光");
    MenuText::TextDisabled("HDR 输入设置：调整交给 NR 的亮度范围。");

    {
    // Logarithmic, because the useful range is not linear. A quarter to 240: the low end because
    // a frame the game already tone mapped wants roughly 1, the high end because there is no
    // principled ceiling -- this is a divisor on an open-ended linear buffer, and how far up a
    // given game needs to go is a property of that game's exposure, not of anything we can bound.
    // One tester was still improving at 100. A linear slider over that span would spend nine
    // tenths of its travel on values nobody needs and never reach the ones they do.
    // One dropdown, because there is one answer.
    //
    // This was two checkboxes that could both be on, and every attempt to stop that was a patch
    // on a shape that should not have existed. Greying deadlocked -- each disabled the other, so
    // once both were set the only way out was a button the notice never mentioned. Clearing
    // worked but silently undid a setting somebody had made. Both were ways to stop an illegal
    // state being REACHED; a single choice cannot reach it, because there is only one value to
    // be in.
    //
    // Each option also says whether it can actually do anything in THIS game, in colour, so the
    // choice is made on what is available rather than on what sounds best.
    {
        const bool vk = DlssNr::IsRunningVk();
        const auto ex = vk ? DlssNr::GameExposureStatusVk() : DlssNr::GameExposureStatus();
        const bool haveExposure = HaveGameExposure();

        const float anchorNow = DlssNr::ExposureScan::BestValue();
        const bool haveAnchor = !DlssNr::ExposureScan::Anchors().empty();

        static const char* sourceNames[] = { "手动纸白亮度", "游戏曝光",
                                             "扫描曝光（实验功能）",
                                             "从 HDR 画面自动曝光" };

        int source = (int) config->DlssNrWhitePointSource.value_or_default();

        if (source < 0 || source > 3)
            source = 0;

        if (MenuText::Segmented("白点来源", &source, sourceNames, IM_ARRAYSIZE(sourceNames)))
        {
            config->DlssNrWhitePointSource = (uint32_t) source;

            // Nothing else to set. The scan asks the source whether it is wanted, so choosing
            // it here is the whole of switching it on -- there is no second flag to keep in
            // step, and so no way for the two to disagree.
        }

        HelpMarker("NR 亮度参考的来源。\n手动：纸白亮度。\n游戏曝光：游戏提供的数值。\n扫描曝光：从游戏缓冲区读取，需要校准。\n自动曝光：OptiScaler 自行测量画面。");

        // Availability, in colour, for the option currently chosen. Two lines at most, whichever source.
        if (MenuText::TreeNode("曝光读数##autoexposure"))
        {
            const float exposureSlot = StatusSlotBegin();
            if (source == 1)
            {
                if (!vk && ex.seenFrames == 0)
                    MenuText::TextDisabled("等待画面…");
                else if (!haveExposure)
                    MenuText::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                       "游戏未提供曝光，使用手动纸白亮度。");
                else if (ex.exposure > 1e-6f)
                {
                    // Brightness points are keyed by the game's exposure in both scales; the scale only changes what
                    // the Trim multiplies. Both whites are shown so the scale can be chosen by eye.
                    const float anchorKey = DlssNrGameScale::AnchorKey(ex.preExposure, ex.exposure);
                    const auto trimAnchors =
                        DlssNrTrim::Parse(config->DlssNrGameExposureTrimAnchors.value_or_default());
                    const float trim = DlssNrTrim::TrimForKey(
                        anchorKey, config->DlssNrWhitePointTrim.value_or_default(), trimAnchors, false);
                    const bool asIs = config->DlssNrGameExposureScale.value_or_default() == DlssNrGameScale::kAsIs;
                    const float whiteAsIs =
                        DlssNrGameScale::WhiteBase(DlssNrGameScale::kAsIs, ex.preExposure, ex.exposure) * trim;
                    const float whiteWithExposure =
                        DlssNrGameScale::WhiteBase(DlssNrGameScale::kWithExposure, ex.preExposure, ex.exposure) * trim;
                    MenuText::TextColored(
                        ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                        asIs ? "游戏值 %.2f → NR 亮度 %.2f（原生外观），游戏设置模式为 %.2f%s"
                             : "游戏值 %.2f → NR 亮度 %.2f（游戏设置），原生外观模式为 %.2f%s",
                        ex.exposure, asIs ? whiteAsIs : whiteWithExposure, asIs ? whiteWithExposure : whiteAsIs,
                        ex.offeredNow ? "" : "（保持上次值：本帧缺失）");
                }
                else
                    MenuText::TextDisabled("正在读取曝光…");
            }
            else if (source == 3)
            {
                const auto autoEx = vk ? DlssNr::AutoExposureStatusVk() : DlssNr::AutoExposureStatus();

                if (autoEx.exposure > 1e-8f)
                {
                    const float baseWhitePoint = autoEx.preExposure / autoEx.exposure;
                    const auto trimAnchors =
                        DlssNrTrim::Parse(config->DlssNrAutoExposureTrimAnchors.value_or_default());
                    const float trim = DlssNrTrim::TrimForKey(
                        baseWhitePoint, DlssNr::AutoTrimEffective(*config), trimAnchors, false);
                    // Middle-grey metering, mode 13 in dlssnr.hlsl: exposure = 0.18 / (0.82 * average scene brightness).
                    MenuText::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                       "场景亮度 %.3f → 模型白点 %.2f",
                                       0.18f / (0.82f * autoEx.exposure), baseWhitePoint * trim);
                    HelpMarker("测量 NR 处理前的画面。模型白点表示被当作纯白的亮度。");
                    MenuText::TextDisabled("自动曝光 %.4f", autoEx.exposure);
                }
                else
                    MenuText::TextDisabled("正在计算自动曝光…");
            }
            else if (source == 2)
            {
                // "Nothing found" and "found several, none of them moving" are different states,
                // and this said the first for both. In GTA V the log carried eight candidates while
                // the panel claimed there were none, which reads as the scan being broken when what
                // it actually needs is for the light to change.
                if (anchorNow <= 0.0f)
                {
                    const unsigned int watching = (unsigned int) DlssNr::ExposureScan::Report().size();

                    if (watching == 0)
                        MenuText::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                           "未找到候选曝光。");
                    else
                        MenuText::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                           "%u 个候选值；在亮暗区域间移动以测试。",
                                           watching);
                }
                else if (!haveAnchor)
                    MenuText::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                       "已找到候选曝光。调整纸白亮度后点击在此锚定。");
                // Once anchored, the scan -> white point readout sits above the sliders below; it is
                // not repeated up here.
            }
            else if (haveExposure)
            {
                MenuText::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                   "游戏曝光可用。");
            }
            StatusSlotEnd(exposureSlot, 2);
            ImGui::TreePop();
        }
    }






    // A measured suggestion for paper white used to sit here and has been withdrawn.
    //
    // It took the 90th percentile of per-tile peak luminance from the untouched frame, which is a
    // statement about scene content rather than about the buffer's scale. In Nioh 3, where the
    // right answer is about 240, it offered 8 -- because most tiles are shadow and the percentile
    // sits wherever most tiles are. The guard meant to catch that compared each tile against the
    // frame's own brightest, which is scale-free and therefore passes on a black screen: the same
    // relative-threshold mistake the white point meter was removed for, made a second time.
    //
    // A wrong number offered confidently is worse than no number, so nothing is offered. What
    // replaces it has to be a measurement of the game's own exposure rather than of its scenery:
    // the exposure texture where a game supplies one, and otherwise the ratio between the
    // scene-referred buffer and the finished frame, which is that exposure by definition.

    // Two controls, not one control with two meanings.
    //
    // These are different quantities. The manual path wants an absolute divisor on an open-ended
    // linear buffer -- Nioh 3 needs about 240 -- and the exposure path wants a multiplier on a
    // number the game already supplied, where 1 is correct and anything far from it says the read
    // is wrong rather than that somebody prefers it.
    //
    // They used to share one stored value, narrowed to 0.25..4 when the toggle was on. That kept
    // a ruinous value unreachable but left two worse problems: moving the slider in one mode
    // silently destroyed the number found in the other, and there was no way back to "just take
    // the game's answer" short of knowing that the number for it was 1. Separate values fix both.
    // Switching modes is now non-destructive in both directions.
    // The trim belongs to both automatic sources, since both end in "the game's number times a
    // little". Only the manual source gets the absolute slider.
    // One slider per source, each remembering its own number.
    //
    // A trim on the game's exposure and a trim on a buffer the scan found are trims on different
    // things, and a value found against one means nothing against the other. Sharing them meant
    // changing source silently carried a number across, so a picture that had been tuned came
    // back wrong for a reason nothing on screen explained.
    //
    // The scan before it is anchored is the exception, and it has to be: anchoring captures an
    // absolute white point, so there must be an absolute slider to set. Showing a trim there
    // asked people to "set paper white below" next to a control that was not paper white.
    const int wpSource = (int) config->DlssNrWhitePointSource.value_or_default();

    // Which anchor row the paper-white slider edits, or -1 for the live unanchored point. Menu-
    // local and not persisted; the anchor block below sets it when a row is clicked. Declared
    // here because both the slider (this block) and the table (below) read it in the same frame.
    static int selectedAnchor = -1;
    auto anchors = DlssNr::ExposureScan::Anchors();
    if (selectedAnchor >= (int) anchors.size())
        selectedAnchor = -1;

    if (wpSource == 2)
    {
        const bool editingRow = selectedAnchor >= 0 && selectedAnchor < (int) anchors.size();

        // The single scan -> white point readout, above the sliders it explains.
        if (!anchors.empty())
        {
            const float liveScan = DlssNr::ExposureScan::BestValue();

            if (liveScan > 0.0f)
            {
                const float w = DlssNr::ExposureScan::AnchoredWhitePoint(
                    liveScan, config->DlssNrScanInverted.value_or_default(),
                    config->DlssNrScanTrim.value_or_default());

                MenuText::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                   "扫描值 %.5f → 白点 %.2f（%u 点%s）", liveScan, w,
                                   (unsigned) anchors.size(), anchors.size() == 1 ? "" : "s");
            }
        }

        // Paper white shows only when there is a point to set: before the first anchor, or when a
        // row is selected to edit. Once points exist and none is selected, the white point is fixed
        // by the anchors and only the trim adjusts the live picture -- so the trim takes the
        // slider's place, the same shape as the game-exposure source.
        const bool showPaperWhite = anchors.empty() || editingRow;

        if (showPaperWhite)
        {
            float pw = editingRow ? anchors[selectedAnchor].white
                                  : config->DlssNrWhitePointScale.value_or_default();

            char lbl[48];
            if (editingRow)
                snprintf(lbl, sizeof(lbl), MenuText::Translate("纸白亮度（编辑第 %d 点）").c_str(), selectedAnchor + 1);
            else
                snprintf(lbl, sizeof(lbl), MenuText::Translate("纸白亮度").c_str());

            if (MenuText::SliderFloat((std::string(lbl) + "###scan-paper-white").c_str(), &pw, 0.25f, 2000.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
            {
                if (editingRow)
                {
                    DlssNr::ExposureScan::AnchorSetWhite(selectedAnchor, pw);
                    config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                }
                else
                    config->DlssNrWhitePointScale = pw;
            }

            HelpMarker("调整选中的校准点或下一个校准点。点击在此锚定可保存当前光照。");
        }

        // The trim multiplies the interpolated result, and in the steady state it is the control
        // that stands in for paper white: adjust it until the picture looks right in the current
        // light, then Anchor bakes that trimmed value into a new point and resets the trim to 1.
        if (!anchors.empty())
        {
            float trim = config->DlssNrScanTrim.value_or_default();

            if (MenuText::SliderFloat("微调（乘以扫描值）", &trim, 0.25f, 4.0f, "%.2fx",
                                   ImGuiSliderFlags_Logarithmic))
                config->DlssNrScanTrim = std::clamp(trim, 0.25f, 4.0f);

            ImGui::SameLine();

            if (MenuText::SmallButton("重置##scantrim", !NextgenSliderStyle::IsDefault(trim, 1.0f)))
                config->DlssNrScanTrim = 1.0f;

            HelpMarker("乘以校准白点。越高 NR 输入越暗，越低越亮。在此锚定会保存并将此值重置为 1。");
        }
    }
    else if (wpSource == 1)
    {
        // Up to 50x under the hood: a game's reported exposure scale can sit well below what the picture wants
        // (Marvel's Spider-Man Remastered with XeSS swapped to DLSS is one), so 4x was too tight. Shown as
        // stops around 1x, which is why the slider runs further towards darker than towards brighter.
        // Index = DlssNrGameScale value: 0 with the game's exposure, 1 as is.
        static const char* scaleNames[] = { "游戏设置", "原生外观" };
        int scale = (int) config->DlssNrGameExposureScale.value_or_default();

        if (scale < 0 || scale > 1)
            scale = 0;

        if (MenuText::Segmented("起始外观##gameexposure", &scale, scaleNames, IM_ARRAYSIZE(scaleNames)))
        {
            config->DlssNrGameExposureScale = (uint32_t) scale;
            // A Trim set on one scale means something else on the other, so the slider starts again at 0 EV.
            config->DlssNrWhitePointTrim = DlssNrExposureCalibrate::kGameExposureNeutralTrim;
        }

        HelpMarker("原生外观：类似内置 NR 的游戏，建议先试。\n游戏设置：使用游戏亮度值；原生外观过暗时可试。\n切换后输入亮度滑块重置为 0。");

        RenderTrimEvSlider(config->DlssNrWhitePointTrim, DlssNrExposureCalibrate::kGameExposureNeutralTrim,
                           DlssNrTrim::Parse(config->DlssNrGameExposureTrimAnchors.value_or_default()).size(),
                           "gameexposure",
                           "模型输入亮度，0 为上方选择的起点。值越高越亮，越低越暗。过亮会丢失高光细节，过暗会丢失阴影细节。");
        RenderTuneForThisScene(1, config->DlssNrWhitePointTrim, DlssNrExposureCalibrate::kGameExposureNeutralTrim,
                               config->DlssNrGameExposureTrimAnchors);
        RenderBrightnessPoints(config->DlssNrGameExposureTrimAnchors, DlssNrExposureCalibrate::kGameExposureNeutralTrim,
                               "gameexposure");
    }
    else if (wpSource == 3)
    {
        // The scale stays centred on a 5x Trim (0 EV), the old default from the PR this came from. The default is
        // +1.5 EV for every game; see DlssNr_AutoTrimDefault.h for the measurements. It is independent of the Game exposure Trim.
        RenderTrimEvSlider(config->DlssNrAutoExposureTrim, DlssNrExposureCalibrate::kNeutralTrim,
                           DlssNrTrim::Parse(config->DlssNrAutoExposureTrimAnchors.value_or_default()).size(),
                           "autoexposure",
                           "模型输入亮度，正值更亮，负值更暗。未调整时默认 +1.5 EV，重置恢复此值。过亮可能截断高光或改变阴影颜色，过暗会隐藏阴影细节。OptiScaler 自行测量 NR 前的线性 HDR 画面，支持 D3D12 和 Vulkan。",
                           DlssNrAutoTrim::kDefaultTrim);

        RenderTuneForThisScene(3, config->DlssNrAutoExposureTrim, DlssNrExposureCalibrate::kNeutralTrim,
                               config->DlssNrAutoExposureTrimAnchors);
        RenderBrightnessPoints(config->DlssNrAutoExposureTrimAnchors, DlssNrExposureCalibrate::kNeutralTrim,
                               "autoexposure");

        // Following the game's own exposure (DlssNr_FollowGame.h): on by default for a known unexposed game
        // (DlssNr_GameDefaults.h). Vulkan follows from the host value, a few frames behind the game.
        {
            const bool followVk = DlssNr::IsRunningVk();
            bool follow = DlssNr::FollowGameOn(*config);

            if (MenuText::Checkbox("跟随游戏曝光", &follow))
                config->DlssNrAutoExposureFollowGame = follow;

            HelpMarker("让自动曝光跟随游戏本身的曝光变化，如过场和淡入淡出。适合曝光应用前的输入，RDR2 默认开启。大多数游戏应关闭，避免重复应用曝光。");

            // Status and Re-learn only while following: off, the checkbox already says so, and a second
            // "calibrate" button next to Tune for this scene read as the same thing.
            if (follow)
            {
                const auto followStatus =
                    followVk ? DlssNr::FollowGameExposureStatusVk() : DlssNr::FollowGameExposureStatus();
                const auto& calibration = DlssNrFollowGame::Instance();
                ImGui::Indent();
                if (MenuText::TreeNode("跟随状态##autoexposure"))
                {
                    const float followSlot = StatusSlotBegin();

                    if (!followStatus.gameExposureSeen)
                        MenuText::TextDisabled("尚不可用：游戏没有提供曝光");
                    else if (!calibration.Locked())
                        MenuText::TextDisabled("正在学习校准…（%u/%u）", calibration.Readings(),
                                            DlssNrFollowGame::kWindow);
                    else
                        MenuText::TextDisabled("相对游戏曝光的校准：%+.2f EV%s", calibration.OffsetEv(),
                                            followStatus.following ? "；跟随中" : "；未跟随");

                    // The calibration eases toward Automatic when the two stay apart (DlssNr_FollowGame.h Track), so a
                    // large disagreement is usually on its way out; say which, wrapped (it ran off the panel before).
                    // The warning shows above the limit and stays until the disagreement is well under it: it changes every
                    // frame, and hovering at the limit made the warning (and everything below it) flicker.
                    static bool disagreementShown = false;
                    const float disagreement = std::fabs(followStatus.disagreementEv);
                    const float limit = DlssNrExposureCalibrate::kFollowDisagreementLimitEv;
                    disagreementShown = disagreement > limit || (disagreementShown && disagreement > 0.75f * limit);

                    if (followStatus.following && calibration.Locked() && calibration.Easing())
                    {
                        ImGui::PushTextWrapPos(0.0f);
                        MenuText::TextDisabled("正在向自动曝光自身测量值平滑校准（相差 %+.1f EV）。",
                                            followStatus.disagreementEv);
                        ImGui::PopTextWrapPos();
                    }
                    else if (disagreementShown)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, NextgenUi::ReadableTextColor(ImVec4(0.9f, 0.6f, 0.25f, 1.0f)));
                        // Easing is off in a game whose own exposure moves (DlssNr_FollowGame.h): then only Re-learn.
                        if (calibration.GameMoves())
                            MenuText::TextWrapped("与自动曝光测量值相差 %+.1f EV。画面过亮或过暗时，请在正常场景重新学习。",
                                               followStatus.disagreementEv);
                        else
                            MenuText::TextWrapped("与自动曝光测量值相差 %+.1f EV。持续偏差时会自行恢复，也可在正常场景重新学习。",
                                               followStatus.disagreementEv);
                        ImGui::PopStyleColor();
                    }

                    StatusSlotEnd(followSlot, 4);
                    ImGui::TreePop();
                }

                // Learns the calibration again from scratch.
                if (MenuText::SmallButton("重新学习##autoexposure"))
                {
                    DlssNrFollowGame::Instance().Reset();
                    LOG_INFO("DLSS-NR automatic exposure: re-learning the calibration against the game's exposure");
                }

                HelpMarker("重新学习自动曝光与游戏曝光的关系，约需 2 秒。请在正常日光场景进行，避免使用过场画面。");
                ImGui::Unindent();
            }
        }

        static const char* const meterNames[] = { "平均", "百分位（对数）" };
        int meter = std::min<int>((int) config->DlssNrAutoExposureMeter.value_or_default(), 1);

        if (MenuText::Segmented("测光方式", &meter, meterNames, IM_ARRAYSIZE(meterNames)))
            config->DlssNrAutoExposureMeter = (uint32_t) meter;

        HelpMarker("场景亮度的测量方式。\n平均：测量全画面并降低亮区权重。\n百分位：取中间亮度范围，减少灯光和天空影响。\n切换后请重新检查模型输入亮度。");

        if (meter == (int) DlssNrExposureMeter::kPercentile)
        {
            float low = config->DlssNrAutoExposureMeterLowPercent.value_or_default();
            float high = config->DlssNrAutoExposureMeterHighPercent.value_or_default();

            if (MenuText::SliderFloat("忽略最暗区域", &low, 0.0f, 50.0f, "%.0f%%"))
                config->DlssNrAutoExposureMeterLowPercent = std::clamp(low, 0.0f, 50.0f);

            HelpMarker("排除画面最暗部分的比例。越高忽略阴影越多，默认 10%。");

            if (MenuText::SliderFloat("忽略最亮区域", &high, 50.0f, 100.0f, "%.0f%%"))
                config->DlssNrAutoExposureMeterHighPercent = std::clamp(high, 50.0f, 100.0f);

            HelpMarker("排除高于该亮度的读数。越低忽略高光越多，默认 90%。");
        }
        else
        {
            float protection = config->DlssNrAutoExposureShadowProtection.value_or_default();
            if (MenuText::SliderFloat("降低高光权重", &protection, 0.0f, 100.0f, "%.0f%%"))
                config->DlssNrAutoExposureShadowProtection = std::clamp(protection, 0.0f, 100.0f);

            HelpMarker("避免天空、灯光和反射导致其余画面变暗。越高高光权重越低；0% 时同等计权。");
        }

        // DlssNr_ExposureAdapt.h: a pass of its own on D3D12 and Vulkan.
        float adaptBrighter = DlssNrExposureAdapt::Seconds(
            config->DlssNrAutoExposureAdaptBrighterSeconds.value_or_default(), DlssNrExposureAdapt::kDefaultBrighterSeconds);
        float adaptDarker = DlssNrExposureAdapt::Seconds(config->DlssNrAutoExposureAdaptDarkerSeconds.value_or_default());

        if (MenuText::SliderFloat("适应变亮的速度", &adaptBrighter, 0.0f, DlssNrExposureAdapt::kMaxSeconds,
                               adaptBrighter > 0.0f ? "%.2f s" : "关闭"))
            config->DlssNrAutoExposureAdaptBrighterSeconds =
                DlssNrExposureAdapt::Seconds(adaptBrighter, DlssNrExposureAdapt::kDefaultBrighterSeconds);

        HelpMarker("跟随场景变亮的速度。越高越慢越稳定，越低越快；关闭为立即变化。跟随游戏曝光时不使用。");

        if (MenuText::SliderFloat("适应变暗的速度", &adaptDarker, 0.0f, DlssNrExposureAdapt::kMaxSeconds,
                               adaptDarker > 0.0f ? "%.2f s" : "关闭"))
            config->DlssNrAutoExposureAdaptDarkerSeconds = DlssNrExposureAdapt::Seconds(adaptDarker);

        HelpMarker("跟随场景变暗的速度。默认较慢，避免短暂闪光令画面长时间变暗。");
    }
    else
    {
        // Logarithmic, because the useful range is not linear. A quarter to 2000: the low end
        // because a frame the game already tone mapped wants roughly 1, the high end because
        // there is no principled ceiling -- this is a divisor on an open-ended linear buffer, and
        // how far up a given game needs to go is a property of that game's exposure rather than
        // of anything that can be bounded here. One tester was still improving at 100.
        float wpScale = config->DlssNrWhitePointScale.value_or_default();

        if (MenuText::SliderFloat("纸白亮度", &wpScale, 0.25f, 2000.0f, "%.2fx",
                               ImGuiSliderFlags_Logarithmic))
            config->DlssNrWhitePointScale = wpScale;

        ImGui::SameLine();
        if (MenuText::SmallButton("重置##paperwhite", !NextgenSliderStyle::IsDefault(wpScale, 1.0f)))
            config->DlssNrWhitePointScale = 1.0f;

    HelpMarker("NR 输入的亮度参考。越高输入越暗，越低越亮。细节丢失或偏色时可调整。");
    }

    // Highlight guard, directly under the white point / trim -- it bounds the model's edit and
    // belongs with the exposure controls it works alongside.
    float maxRatio = config->DlssNrMaxRatio.value_or_default();
    if (MenuText::SliderFloat("高光保护", &maxRatio, 1.0f, MaxHighlightGuard, "%.1fx"))
        config->DlssNrMaxRatio = maxRatio;

    ImGui::SameLine();
    if (MenuText::SmallButton("重置##guard", !NextgenSliderStyle::IsDefault(maxRatio, 2.0f)))
        config->DlssNrMaxRatio = 2.0f;

    HelpMarker("限制 NR 将单个像素变亮的幅度。越低高光变化越少；替换模式中也限制变暗幅度。");

    // Directly under the white point, because that is the number it moves and the number the
    // anchor captures. It used to sit under Inspect, a whole section away from the slider it
    // reads, which left "Anchor here" looking like a control for something else entirely.
    {
        // No checkbox here any more.
        //
        // The dropdown above says whether the scan is the white point's source, and that is
        // the only reason anybody using this would want it running. A second control could
        // only agree with the dropdown or contradict it, and both were on offer: it began as
        // a redundant question and became a way to switch off the thing the chosen source
        // depended on.
        //
        // The ini key survives as a developer override for the one case a user has no reason
        // to want -- running the scan in a game that supplies a REAL exposure, so the log can
        // compare the two. That is validation, and validation does not need a widget.
        //
        // Worth keeping written down, since the panel no longer says it: the scan matches
        // buffers by SHAPE, and shape is a weak filter. In GTA V -- a game that supplies a
        // real exposure, so the right answer sat visible beside it -- the best candidate was
        // a 1x1 R32_FLOAT that climbed in a straight line for seventeen minutes while the
        // true exposure held still. Their ratio moved 14x. That is an accumulator, not an
        // eye adaptation.

            // Only where it means something. The lamp reads the scan, so offering it beside a
            // white point that comes from the game's own exposure is offering a control that
            // cannot light up.
            bool meter = config->DlssNrScanMeter.value_or_default();

            if (config->DlssNrWhitePointSource.value_or_default() == 2 &&
                MenuText::Checkbox("显示曝光仪表", &meter))
                config->DlssNrScanMeter = meter;

            HelpMarker("显示扫描曝光与颜色指示，仅用于观察。");

        // Shown when the scan is actually running, whichever way it got switched on.
        if (DlssNr::ExposureScan::Scanning())
        {
            // Anchoring: one press, then it never needs touching again.
            //
            // The absolute white point cannot come out of a buffer whose units are unknown.
            // Every value AFTER the first can: only the ratio against the anchor is used, so
            // whatever the number means, it cancels. That is why this is a button and not a
            // measurement -- the one thing a person can supply that no amount of cleverness
            // can is "this looks right to me".
            int which = 0;
            float low = 0.0f, high = 0.0f;
            const float live = DlssNr::ExposureScan::BestValue(&which, &low, &high);

            const bool isSource = config->DlssNrWhitePointSource.value_or_default() == 2;

            // Anchor captures (currentScan, currentPaperWhite) and ADDS a row -- it does not
            // replace. One row is the old single-anchor ratio law; add a second in different
            // light and the white point is interpolated between the points, so it holds across
            // the whole range instead of only near one anchor. Greyed unless the scan is the
            // chosen source and it currently has a value to capture.
            ImGui::BeginDisabled(live <= 0.0f || !isSource);

            if (MenuText::Button("在此锚定"))
            {
                // What to capture. Before the first point, the paper white above (an absolute value
                // with the wide range a fresh game needs). After that, the EFFECTIVE white point the
                // picture is showing right now -- the interpolated value times the Trim the user just
                // dialed in -- so a second point in different light captures the trimmed look, not a
                // frozen paper white (which would make two equal whites and a flat, non-tracking
                // curve). The trim is reset afterwards: the new point, which the picture now passes
                // through exactly, must not be multiplied by it a second time.
                const float captureWhite =
                    anchors.empty()
                        ? std::max(0.01f, config->DlssNrWhitePointScale.value_or_default())
                        : std::max(0.01f, DlssNr::ExposureScan::AnchoredWhitePoint(
                                              live, config->DlssNrScanInverted.value_or_default(),
                                              config->DlssNrScanTrim.value_or_default()));

                if (DlssNr::ExposureScan::AnchorAdd(live, captureWhite))
                {
                    config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                    config->DlssNrScanTrim = 1.0f;
                    selectedAnchor = -1;
                }
            }

            ImGui::EndDisabled();

            HelpMarker("将当前曝光和白点保存为校准点，最多 8 个。首个点调整纸白亮度，后续点调整微调值。");

            if (!isSource)
                MenuText::TextDisabled("当前白点来源不是扫描曝光。");

            if (!anchors.empty())
            {
                // The row nearest the live scan value (in log space) is the one driving the
                // picture right now; mark it so the user can see which calibration is in effect.
                int active = 0;
                float bestDist = 1e30f;
                const float liveLog = std::log(std::max(live, 1e-6f));

                for (size_t i = 0; i < anchors.size(); ++i)
                {
                    const float d =
                        std::fabs(std::log(std::max(anchors[i].scan, 1e-6f)) - liveLog);
                    if (d < bestDist)
                    {
                        bestDist = d;
                        active = (int) i;
                    }
                }

                for (size_t i = 0; i < anchors.size(); ++i)
                {
                    ImGui::PushID((int) i);

                    // Delete first, so its click is never swallowed by the row-wide Selectable.
                    if (MenuText::SmallButton("x"))
                    {
                        DlssNr::ExposureScan::AnchorRemove((int) i);
                        config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                        if (selectedAnchor == (int) i)
                            selectedAnchor = -1;
                        else if (selectedAnchor > (int) i)
                            --selectedAnchor;
                        ImGui::PopID();
                        continue;
                    }

                    ImGui::SameLine();

                    const bool sel = (int) i == selectedAnchor;
                    char row[96];
                    snprintf(row, sizeof(row), MenuText::Translate("%s 扫描值 %.4f → 白点 %.2f%s").c_str(),
                             ((int) i == active && isSource) ? ">" : "  ", anchors[i].scan,
                             anchors[i].white, sel ? "[正在编辑]" : "");

                    // Click selects the row (slider edits it); click again deselects (slider
                    // returns to the live unanchored point).
                    if (MenuText::Selectable((std::string(row) + "###scan-anchor").c_str(), sel))
                        selectedAnchor = sel ? -1 : (int) i;

                    ImGui::PopID();
                }

                MenuText::TextDisabled("点击一行选中编辑，再次点击取消选择。> 表示当前激活点。");
            }

            // The direction flag only means anything with a single point; with two or more the
            // direction the white point moves is already fixed by the data.
            if (anchors.size() == 1)
            {
                bool inverted = config->DlssNrScanInverted.value_or_default();
                if (MenuText::Checkbox("反转曝光跟踪", &inverted))
                    config->DlssNrScanInverted = inverted;

                HelpMarker("反转扫描曝光对白点的影响方向，仅在只有一个校准点时可用。");
            }

            // The scan -> white point readout is shown above the sliders now, not here.

            // Everything below is read-out rather than control: what the scan is looking at and
            // how to tell whether it found the right thing. Folded away because the two decisions
            // that matter -- anchor, and which way the number runs -- are above it.
            if (MenuText::TreeNode("高级设置"))
            {

                const auto found = DlssNr::ExposureScan::Report();
                const char* why = DlssNr::ExposureScan::Status();

                if (found.empty())
                {
                    MenuText::TextDisabled("%s", why != nullptr && why[0] != 0
                                                  ? why
                                                  : "未找到候选曝光。");
                }
                else
                {
                    for (size_t i = 0; i < found.size(); ++i)
                    {
                        const auto& c = found[i];

                        if (c.reads == 0)
                        {
                            MenuText::TextDisabled("%zu. %s —— 尚未读取", i + 1, c.shape.c_str());
                            continue;
                        }

                        // Moving is the whole signal, so it is the thing that is coloured.
                        MenuText::TextColored(c.moves ? ImVec4(0.45f, 0.8f, 0.45f, 1.0f)
                                                   : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                                           "%zu. %s = %.5f（范围 %.5f..%.5f）%s", i + 1,
                                           c.shape.c_str(), c.latest, c.lowest, c.highest,
                                           c.moves ? "有变化" : "尚无变化");
                    }

                    MenuText::TextDisabled("在明亮和昏暗区域间移动，检查曝光跟踪。");
                    MenuText::TextDisabled("只增长的数值可能是计数器。");
                }

                ImGui::TreePop();
            }
        }
    }


    }
}

// NR Options.
static void RenderSharedDlssUpsampling(Config* config, const NrCommon& nr)
{
    const bool finishedPicture = nr.finishedPicture;
    const bool genshin = GenshinBridgeMenuProcess();
    if (genshin && !config->MenuShowUnfinished.value_or_default()) return;
    ImGui::BeginDisabled(genshin);
    bool beforeSr = config->DlssNrRunBeforeSr.value_or_default() ||
                    (finishedPicture && config->DlssNrDeferredDlss.value_or_default());
    const auto activeFeature = State::Instance().currentFeature;
    const bool rayReconstruction = activeFeature && activeFeature->GetUpscalerType() == Upscaler::DLSSD;
    // Nested under Finished Picture: a second, independent axis (generate the changes at the
    // smaller pre-SR size vs. at the finished picture's own size), not a fourth top-level
    // placement -- progressive disclosure, same as every other mode-gated control in this file.
    if (finishedPicture)
    {
        if (MenuText::Checkbox(genshin ? "BeforeSR（未完成）" : "在超分辨率之前运行模型", &beforeSr))
        {
            config->DlssNrRunBeforeSr = beforeSr;
            config->DlssNrDeferredDlss = false;
        }
        HelpMarker("先在超分前的小图上计算模型效果，再随 DLSS 放大并应用到最终画面。实验功能，颜色可能不同；需要 DLSS 超分，不支持光线重建。");
    }

    bool deferredDlss = config->DlssNrDeferredDlss.value_or_default();
    if (!finishedPicture)
    {
        if (MenuText::Checkbox(genshin ? "Auto-afterSR（未完成）" : "超分前计算、超分后应用（DLSS）", &deferredDlss))
            config->DlssNrDeferredDlss = deferredDlss;
        HelpMarker("在小图计算 NR 修改，用 DLSS 放大并在超分后应用。实验功能，可能闪烁且开销较高；需要 D3D12 DLSS，不支持光线重建。关闭冻结帧、对比和调试视图。");
        if (deferredDlss && rayReconstruction)
            MenuText::TextWrapped("光线重建不支持先计算后应用；由超分前应用选项控制 NR 位置。");
        else if (deferredDlss)
            MenuText::TextWrapped("残差 DLSS：%s", NrOptionsStatusChinese(DlssNr::DeferredDlssStatus()).c_str());
    }
    else if (beforeSr)
        MenuText::TextWrapped("超分前修改：%s", NrOptionsStatusChinese(DlssNr::DeferredDlssStatus()).c_str());
    ImGui::EndDisabled();

}

static void RenderOptionsPage(Config* config, float menuResScale, const NrCommon& nr)
{
    const bool enabled = nr.enabled;
    bool finishedPicture = nr.finishedPicture;

    MenuText::SeparatorText("NR 选项");

    bool beforeSr = config->DlssNrRunBeforeSr.value_or_default() ||
                    (finishedPicture && config->DlssNrDeferredDlss.value_or_default());
    const auto activeFeature = State::Instance().currentFeature;
    const bool rayReconstruction = activeFeature && activeFeature->GetUpscalerType() == Upscaler::DLSSD;
    const bool deferredActive = !finishedPicture && config->DlssNrDeferredDlss.value_or_default() && !rayReconstruction;

    const bool genshin = GenshinBridgeMenuProcess();
    const char* placementNames[] = { genshin ? "Auto-afterSR（未完成）" : "超分辨率之后",
                                    genshin ? "BeforeSR（未完成）" : "超分辨率之前",
                                    "最终画面（呈现模式）" };
    const bool fixedPresent = DlssNr::UsesGenshinPresentPipeline();
    int placement = fixedPresent || finishedPicture ? 2 : (beforeSr ? 1 : 0);
    const bool showUnfinished = config->MenuShowUnfinished.value_or_default();
    bool placementChanged = false;
    ImGui::BeginDisabled(deferredActive);
    const bool placementEnabled[]={!genshin,!genshin,true};
    const bool placementVisible[]={!genshin || showUnfinished,!genshin || showUnfinished,true};
    placementChanged=MenuText::Segmented("NR 处理位置：",&placement,placementNames,3,placementEnabled,placementVisible);
    ImGui::EndDisabled();
    if (fixedPresent)
        MenuText::TextDisabled("当前入口：呈现输入 → NR → 细节保留 → FG；每个真实源帧一次。");
    if (placementChanged)
    {
        // One selector owns both the normal Evaluate seams and the Genshin
        // presentation adapter. Never let the latter silently override a choice.
        config->DlssNrGenshinPresentPipeline = placement == 2;
        if (placement == 0)
        {
            if (finishedPicture) RetryNr();
            finishedPicture = false;
            beforeSr = false;
            config->DlssNrFinishedPicture = false;
            config->DlssNrRunBeforeSr = false;
        }
        else if (placement == 1)
        {
            if (finishedPicture) RetryNr();
            finishedPicture = false;
            beforeSr = true;
            config->DlssNrFinishedPicture = false;
            config->DlssNrRunBeforeSr = true;
        }
        else
        {
            if (!finishedPicture) RetryNr();
            finishedPicture = true;
            beforeSr = false;
            config->DlssNrFinishedPicture = true;
            config->DlssNrRunBeforeSr = false;
        }
    }

    HelpMarker("NR 的处理位置。\n超分辨率之后：处理超分后的画面。\n超分辨率之前：处理较小输入，开销较低。\n最终画面：处理游戏后期效果后的画面，在插帧前运行。\n模型分辨率是独立设置；切换入口不调整模型尺寸、层数或效果。\n启用光线重建时，NR 总是在其后运行。");

    if (fixedPresent && enabled)
        MenuText::TextWrapped("%s", NrOptionsStatusChinese(DlssNr::FinishedPictureStatus()).c_str());
    else if (finishedPicture && enabled)
    {
        const auto feature = State::Instance().currentFeature;
        const bool bridge = feature && feature->Api() == API::DX11 && feature->IsWithDx12() &&
                            State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12;
        const bool genshinPresent = F6InputPolicy::FsrBridgeEnabled &&
            GenshinLegacy::GetStatus().effectiveEnabled &&
            State::Instance().swapchainInteropApi == SwapchainInteropApi::None;
        if (bridge && (config->DlssNrRunBeforeSr.value_or_default() || config->DlssNrDeferredDlss.value_or_default()))
            MenuText::TextWrapped("DX11 桥接的呈现模式暂不支持在超分前运行模型，请关闭下方选项。");
        else if (feature && !bridge && !genshinPresent && (feature->Api() != API::DX12 || feature->IsWithDx12()))
            MenuText::TextWrapped("此选项需要原生 DirectX 12 游戏。");
        else if (genshinPresent)
        {
            const auto status = DlssNr::FinishedPictureStatus();
            // The generic game-input slot may leave this message behind before
            // the separate native picture consumer has been reached. Show a wait,
            // never a successful/model-running state, for this supported route.
            if (status.empty() || status == "This option needs a native DirectX 12 game.")
                MenuText::TextWrapped("等待原神当前成品画面；呈现入口拒绝原因见日志。");
            else
                MenuText::TextWrapped("%s", NrOptionsStatusChinese(status).c_str());
        }
        else
            MenuText::TextWrapped("%s", NrOptionsStatusChinese(DlssNr::FinishedPictureStatus()).c_str());
    }

    // Placement and deferred DLSS application are Global controls, independent of layer editors.
    if (!fixedPresent) RenderSharedDlssUpsampling(config, nr);

    int precisionChoice = config->DlssNrPrecision.value_or_default() == 4 ? 1 : 0;
    const char* precisions[] = { "NVIDIA 原版（FP8）", "实验混合精度（FP8+NVFP4）" };
    if (MenuText::Segmented("模型精度", &precisionChoice, precisions, IM_ARRAYSIZE(precisions)))
        config->DlssNrPrecision = precisionChoice == 1 ? 4u : 0u;
    HelpMarker("NVIDIA：原版 FP8 模型，默认选项。\nFP8+NVFP4：面向 RTX 50 的实验混合精度模型，输出可能稍有不同；仅支持 D3D12。");
    const bool starRailProfile = config->IsStarRailInputProfile();
    if (starRailProfile)
        MenuText::TextDisabled("Star Rail 输入配置：三项首次默认开启，修改保存到 SR_input");
    bool invertMotionY = starRailProfile ? config->StarRailInputMotionY() : config->DlssNrMotionYInverted.value_or_default();
    if (MenuText::Checkbox("NR运动向量Y反向###NR 运动向量 Y 反向（方向修正）", &invertMotionY))
    {
        if (starRailProfile)
            config->SetStarRailInputMotionY(invertMotionY);
        else
            config->DlssNrMotionYInverted = invertMotionY;
    }
    HelpMarker("仅将 NR 使用的运动向量 Y 分量反号，供方向约定相反的游戏测试。不是把整张运动纹理上下翻转，也不会改变游戏插帧。默认关闭；不能仅凭细节跳变判断需要开启。当前原生呈现路径会暂停未验证此映射采样相位的跨帧复用，完整 NR 模型仍可运行。");
    if (!DlssNr::IsRunningVk())
    {
        bool depthFlip = starRailProfile ? config->StarRailInputDepthFlipY() : config->DlssNrDepthTextureFlipY.value_or_default();
        bool motionFlip = starRailProfile ? config->StarRailInputMotionFlipY() : config->DlssNrMotionTextureFlipY.value_or_default();
        if (MenuText::Checkbox("NR深度上下翻转###NR 深度纹理上下翻转（实际输入）", &depthFlip))
        {
            if (starRailProfile)
                config->SetStarRailInputDepthFlipY(depthFlip);
            else
                config->DlssNrDepthTextureFlipY = depthFlip;
        }
        if (MenuText::Checkbox("NR运动上下翻转###NR 运动纹理上下翻转（实际输入）", &motionFlip))
        {
            if (starRailProfile)
                config->SetStarRailInputMotionFlipY(motionFlip);
            else
                config->DlssNrMotionTextureFlipY = motionFlip;
        }
        HelpMarker("在有效区域内翻转实际 NR 输入的纹理行，供深度或运动图与当前画面上下相反时使用。独立于向量 Y 分量反号、倒置 Z、仅预览翻转；不改变颜色或历史画面的方向。切换实际映射仅清空一次 NR 历史。当前原生呈现路径会暂停未验证运动翻转采样相位的跨帧复用；深度翻转是独立选项。默认关闭。D3D12 路径专用。");
    }
    RenderSceneCutToggle(config);
    if (precisionChoice > 0 && DlssNr::IsRunningVk())
    {
        // The hybrid rewrites the model's kernels through NvAPI's D3D12 entry points; on Vulkan the model runs
        // unchanged.
        MenuText::TextUnformatted("混合精度：仅支持 D3D12，Vulkan 不应用");
    }
    else if (precisionChoice > 0)
    {
        MenuText::TextUnformatted(enabled && DlssNrNative::IsActive() ? "混合精度：运行中" : "混合精度：未运行");
        MenuText::TextWrapped("加载时游戏可能暂时停顿，请等待。");
    }
    // Keep failure details in the log without displaying changing kernel counters in the menu.
    auto hybridStatus = DlssNrNative::Status();
    hybridStatus = hybridStatus.substr(0, hybridStatus.find(" |"));
    static std::string lastHybridWarning;
    if (hybridStatus.rfind("Restart required:", 0) == 0 || hybridStatus.find("fallback") != std::string::npos)
    {
        if (hybridStatus != lastHybridWarning)
            LOG_WARN("Hybrid: {}", hybridStatus);
        lastHybridWarning = hybridStatus;
    }
    else
        lastHybridWarning.clear();

    bool applyModel = config->DlssNrApplyModel.value_or_default();
    if (MenuText::Checkbox("应用模型效果", &applyModel))
        config->DlssNrApplyModel = applyModel;

    HelpMarker("显示或隐藏 NR 对画面的修改。取消勾选后模型仍会运行，便于对比。\n要停止计算开销，请关闭“启用神经渲染”。");


    ImGui::Spacing();
}

// A 3D LUT (.cube) graded onto the NR input image before the model sees it (dlssnr-lut-apply epic,
// Story 4). Scanned from the LUTs folder inside the OptiScaler folder; LutFile also accepts any path typed
// into the ini directly, so the combo's preview shows the current selection's filename even when it
// is not one of the scanned entries.
// Cross-frame NR reuse controls retain their exact configuration fields and backend gates.
static void RenderReusePage(Config* config, float menuResScale)
{
    if(MenuText::Button("打开每帧细节保留调试"))GuideDiagnosticsMenu::OpenHistoryWindow();
    ImGui::PushItemWidth(220.0f * menuResScale);
    MenuText::SeparatorText(NextgenUi::English() ? "Multi-frame NR reuse" : "多帧NR复用");
    RenderPassFeedback(config, menuResScale);
    if (DetailReuseUnavailableOnVulkan())
    {
        MenuText::TextWrapped(NextgenUi::English()
            ? "Multi-frame NR reuse is not supported on Vulkan in this round. Its controls are hidden; saved settings are retained."
            : "Vulkan 本轮暂不支持多帧 NR 复用。相关控件已隐藏；已保存设置保持不变。");
        MenuText::TextWrapped(NextgenUi::English()
            ? "Basic NR and the existing global Look remain available where supported."
            : "基础 NR 与原有全局 Look 仍按现有能力提供。");
        ImGui::PopItemWidth();
        return;
    }
    // One setting per kernel set: the fp8 kernels (NVIDIA's DLL and fp8-based builds) and the plain FP16 kernels (used by some modified DLSS-NR DLLs).
    // Only the one for the kernels actually running is used.
    const char* kernelSet = DlssNrNative::VitKernelSet();
    // Forced off for Vulkan games, natively and through the D3D12 bridge (DlssNrFeature_Vk.cpp, DlssNr_Dx12.cpp): shown
    // off and greyed out; the settings stay as they are for D3D12 games.
    const bool vitReuseVk = DlssNr::IsRunningVk() || State::Instance().api == Vulkan;
    ImGui::BeginDisabled(vitReuseVk);
    bool vitReuse = !vitReuseVk && config->DlssNrVitEvery.value_or_default() > 1;
    if (MenuText::Checkbox("复用瓶颈计算：FP8 内核", &vitReuse))
        config->DlssNrVitEvery = vitReuse ? 2u : 1u;
    HelpMarker("隔帧复用模型中变化较慢的瓶颈阶段，降低计算开销；快速运动时可能稍软。\n关闭后每帧重新计算该阶段。它与下方的跨帧细节复用是不同功能。");
    bool vitReusePlain = !vitReuseVk && config->DlssNrVitEveryPlain.value_or_default() > 1;
    if (MenuText::Checkbox("复用瓶颈计算：普通 FP16 内核", &vitReusePlain))
        config->DlssNrVitEveryPlain = vitReusePlain ? 2u : 1u;
    HelpMarker("作用同上，但仅在实际使用普通 FP16 内核时生效；部分修改版 NR 模型使用此内核。\nFP8 和 FP16 两个开关分别控制对应内核，并不会同时叠加。");
    ImGui::EndDisabled();
    MenuText::Text("当前计算内核：%s", NrOptionsStatusChinese(kernelSet).c_str());
    bool detailReuse = config->DlssNrDetailReuse.value_or_default();
    const bool presentedFeedback = config->DlssNrPresentedTemporalDetail.value_or_default();
    const auto detailReuseStatus = !(detailReuse || presentedFeedback) ? DlssNr::DetailReuseInfo {}
                                   : DlssNr::IsRunningVk()              ? DlssNr::DetailReuseStatusVk()
                                                                        : DlssNr::DetailReuseStatus();
    const bool detailReuseOwnsBottleneck = presentedFeedback ||
        (detailReuse && config->DlssNrDetailReuseInterval.value_or_default() > 1);
    if (vitReuseVk)
        MenuText::TextWrapped("瓶颈复用：Vulkan 游戏中关闭，避免暗处闪烁");
    else if (detailReuseOwnsBottleneck)
        MenuText::TextDisabled("瓶颈复用：跨帧细节模式下每帧计算");
    else if (DlssNrNative::VitPlainKernels() ? vitReusePlain : vitReuse)
        MenuText::TextUnformatted(("瓶颈复用：" + NrOptionsStatusChinese(DlssNrNative::VitStatus())).c_str());
    detailReuse = RenderDetailReuseToggle(config);
    bool temporal = config->DlssNrPresentedTemporalDetail.value_or_default();
    if (MenuText::Checkbox("细节持续保留（每源帧完整计算）", &temporal))
        config->DlssNrPresentedTemporalDetail = temporal;
    const bool legacyDetailReuseActive = detailReuse && !temporal;
    HelpMarker("独立于上方的跳帧复用：每个真实源帧完整计算 NR，只搬运细节残差。超分前使用同次 DLSS Evaluate 的深度、运动及真实前后 jitter 差；最终画面需要已验证的场景指南映射。缺少当前输入或完成凭据时使用本帧 NR 结果，不拿旧帧补齐。");
    ImGui::BeginDisabled(!temporal || vitReuseVk);
    int durationMs = static_cast<int>(std::min(config->DlssNrPresentedTemporalDurationMs.value_or_default(),
                                              Config::kPresentedTemporalDurationMaxMs));
    if (MenuText::SliderInt("细节反馈持续时长（毫秒）", &durationMs, 0,
                            static_cast<int>(Config::kPresentedTemporalDurationMaxMs), "%d ms"))
        config->DlssNrPresentedTemporalDurationMs = static_cast<uint32_t>(std::clamp(durationMs, 0,
            static_cast<int>(Config::kPresentedTemporalDurationMaxMs)));
    HelpMarker("正值控制旧残差按源帧实际时间衰减的速度；0 不携带旧细节。强度仍有独立上限，逐像素历史不会因融合而续期。不计生成帧。");
    int maxAgeMs = static_cast<int>(std::clamp(config->DlssNrPresentedTemporalMaxAgeMs.value_or_default(),
        Config::kPresentedTemporalMaxAgeMinMs, Config::kPresentedTemporalMaxAgeMaxMs));
    if (MenuText::SliderInt("反馈历史最长寿命（毫秒）", &maxAgeMs,
        static_cast<int>(Config::kPresentedTemporalMaxAgeMinMs),
        static_cast<int>(Config::kPresentedTemporalMaxAgeMaxMs), "%d ms"))
        config->DlssNrPresentedTemporalMaxAgeMs = static_cast<uint32_t>(maxAgeMs);
    HelpMarker("每个像素保留最老贡献的年龄，达到最长寿命后重新播种当前细节。融合不能刷新旧贡献的年龄；没有按时钟定期更新整幅画面的计时器。");
    float strength = config->DlssNrPresentedTemporalStrength.value_or_default();
    strength = std::isfinite(strength) ? std::clamp(strength, 0.0f, 0.95f) : 0.85f;
    if (MenuText::SliderFloat("历史反馈强度上限", &strength, 0.0f, 0.95f, "%.2f"))
        config->DlssNrPresentedTemporalStrength = strength;
    HelpMarker("完整模型帧融合旧细节的最大比例，默认 0.85，最高 0.95；实际比例还受衰减、剩余寿命、运动和深度信任约束。0 停止旧细节融合。");
    ImGui::EndDisabled();
    if (temporal)
    {
        const auto status = DlssNr::DetailReuseStatus();
        ImGui::BeginDisabled(vitReuseVk);
        GuideDiagnosticsMenu::RenderHistoryTuning(config);
        ImGui::EndDisabled();
        // Measurement is now explicitly owned by the separate debug window.
        // Merely visiting this settings page must not start pixel readbacks.
        if (status.active)
        {
            MenuText::TextUnformatted(status.feedbackHistoryUsable
                ? "细节持续反馈：融合已录制（不代表实际贡献）"
                : "细节持续反馈：已记录当前细节，等待相邻历史");
        }
        else
            MenuText::TextWrapped("细节持续反馈：%s", status.why.empty()
                ? "等待当前 NR 输入" : NrOptionsStatusChinese(status.why).c_str());
        static GuideCapture::ContributionSample measured;
        const auto now = GuideDiagnostics::Now();
        GuideCapture::ReadHistoryMeasurement(measured);
        if (!vitReuseVk &&
            measured.Matches(status.feedbackOwner, status.feedbackConfiguration, status.feedbackSource, now))
            MenuText::Text("历史贡献：%.1f%% 采样点 | 源帧 %llu | %llu ms 前",
                100.0f * measured.changed, measured.source, now - measured.tick);
        else
            MenuText::TextDisabled("历史贡献：等待本配置的 GPU 实测");
        if (status.modelCallCountsKnown)
            MenuText::Text("模型调用：%llu 次 | 失败 %llu 次", status.modelCalls, status.modelFailures);
        else
            MenuText::TextDisabled("模型调用：暂无当前统计");
        if (detailReuse)
            MenuText::TextDisabled("间隔复用：持续反馈期间未运行");
    }
    if (legacyDetailReuseActive)
    {
        bool gameMotion = config->DlssNrGameMotionReuse.value_or_default();
        if (MenuText::Checkbox("允许原生 DLSS-G 场景运动复用（实验功能）", &gameMotion))
            config->DlssNrGameMotionReuse = gameMotion;
        HelpMarker("仅在游戏提供无 HUD 的 DLSS-G 场景输入时使用游戏运动图保持细节。游戏后处理可能与运动数据不完全匹配；出现拖尾或细节错位时关闭。不会改变游戏插帧输入。");
        int interval = static_cast<int>(std::clamp(config->DlssNrDetailReuseInterval.value_or_default(), 1u, 8u));
        if (MenuText::SliderInt("NR 间隔", &interval, 1, 8, "%d"))
            config->DlssNrDetailReuseInterval = static_cast<unsigned>(std::clamp(interval, 1, 8));
        HelpMarker("每隔多少个真实渲染帧运行一次完整 NR。持续保留开启时每帧完整计算，此间隔只在持续保留关闭时生效。关闭持续保留后，1 表示每帧完整计算；2 到 8 表示在完整计算之间复用运动搬运的细节。快速运动或门禁触发时仍会强制完整计算。Vulkan 暂不支持跨帧细节复用。");
        int fgInterval = static_cast<int>(std::clamp(config->DlssNrDetailReuseFgInterval.value_or_default(), 1u, 2u));
        const char* fgIntervalFormat = fgInterval == 1 ? "每个 FG 帧" : "每 2 个 FG 帧";
        const auto fgReuse = DlssNr::FgDetailReuseStatus();
        ImGui::BeginDisabled(!fgReuse.supported);
        if (MenuText::SliderInt("FG 细节复用频率", &fgInterval, 1, 2, fgIntervalFormat))
            config->DlssNrDetailReuseFgInterval = static_cast<unsigned>(std::clamp(fgInterval, 1, 2));
        ImGui::EndDisabled();
        HelpMarker("此频率仅控制生成帧细节复用。当前没有已验证的生成输出消费者，因此控件禁用，保存值保持不变。累计元数据观测不证明当前插帧正在工作，也不证明已复用生成帧。真实输入帧的 NR 间隔独立生效。");
        MenuText::TextWrapped("%s", fgReuse.why);
        MenuText::TextDisabled("已观察 NGX 调用：%llu | 累计生成输出元数据：%llu | 实际生成帧复用提交：%llu",
            static_cast<unsigned long long>(fgReuse.observedNgxEvaluations),
            static_cast<unsigned long long>(fgReuse.evaluationsWithGeneratedOutput),
            static_cast<unsigned long long>(fgReuse.consumedOutputs));
        int spread = std::min(config->DlssNrDetailReuseSpread.value_or_default(), 3u);
        const char* smoothing[] = { "自动", "关闭", "分 2 帧平滑", "分 3 帧平滑" };
        if (MenuText::Segmented("平滑模型帧变化", &spread, smoothing, IM_ARRAYSIZE(smoothing)))
            config->DlssNrDetailReuseSpread = static_cast<unsigned>(spread);
        HelpMarker("将新模型细节的变化分散到数帧，减少突然跳变。自动模式在每 3 帧计算时启用；运动时可能增加拖尾。");
        const auto actual = DlssNr::IsRunningVk() ? DlssNr::DetailReuseStatusVk() : DlssNr::DetailReuseStatus();
        if (actual.modelCallCountsKnown)
            MenuText::Text("模型调用：%llu 次 | 失败 %llu 次", actual.modelCalls, actual.modelFailures);
        else
            MenuText::TextDisabled("模型调用：暂无当前统计");
        HelpMarker("模型调用按各层真实调用累计，含失败；不是显示帧数或 GPU 完成数。统计随模型或配置变化重置，发布值截至本次 NR 调度开始前，可能晚一轮。下方完整/复用/回退仅计复用调度，复用未激活时可保持为零；不代表模型未运行。调度速率不是模型 GPU 耗时的倒数。");
        MenuText::Text("复用调度：完整 %llu | 复用 %llu | 回退 %llu", actual.full, actual.reused, actual.fallback);
        if (actual.baseFps > 0.0)
            MenuText::TextDisabled("NR 调度速率：%.0f 次/秒", actual.baseFps);
        else
            MenuText::TextDisabled("NR 调度速率：暂无测量");
        if (!actual.why.empty())
            MenuText::TextWrapped("细节状态：%s", NrOptionsStatusChinese(actual.why).c_str());

    }

    if (legacyDetailReuseActive)
    {
        // Debugging and A/B testing only; the defaults are the tuned values.
        if (MenuText::TreeNode("调试##detailReuse"))
        {
            bool detailReuseDebug = config->DlssNrDetailReuseDebug.value_or_default();
            if (MenuText::Checkbox("显示丢弃的细节", &detailReuseDebug))
                config->DlssNrDetailReuseDebug = detailReuseDebug;
            HelpMarker("复用帧中，洋红色表示搬运细节被丢弃，青色表示由填补功能替代。\n用于测试。");
            float fill = config->DlssNrDetailReuseFill.value_or_default();
            if (MenuText::SliderFloat("填补丢弃的细节", &fill, 0.0f, 1.0f, "%.2f"))
                config->DlssNrDetailReuseFill = std::clamp(fill, 0.0f, 1.0f);
            HelpMarker("用同一表面邻近像素的 NR 细节填补丢弃区域。越高填补越多；0 时显示游戏原画。默认 1。");
            float steady = config->DlssNrDetailReuseSteady.value_or_default();
            if (MenuText::SliderFloat("稳定完整计算帧", &steady, 0.0f, 1.0f, "%.2f"))
                config->DlssNrDetailReuseSteady = std::clamp(steady, 0.0f, 1.0f);
            HelpMarker("让完整计算帧更接近复用细节，减少两者变化。越高越稳定，但运动可能稍滞后。默认 0，即关闭。");
            MenuText::SeparatorText("搬运采样的可信范围");
            float depthTolerance = DlssNr::DetailReuseDepthToleranceEffective(*config);
            if (MenuText::SliderFloat("深度容差", &depthTolerance, 0.0f, 1.0f, "%.3f"))
                config->DlssNrDetailReuseDepthTolerance = std::clamp(depthTolerance, 0.0f, 1.0f);
            HelpMarker("允许的深度差异。越高越保留水面和玻璃细节，但可能拖影；越低丢弃越多。默认 0.051，RDR2 为 0.267。");
            float clipGamma = config->DlssNrDetailReuseClipGamma.value_or_default();
            if (MenuText::SliderFloat("颜色范围", &clipGamma, 0.0f, 10.0f, "%.2f sigma"))
                config->DlssNrDetailReuseClipGamma = std::clamp(clipGamma, 0.0f, 10.0f);
            HelpMarker("丢弃搬运细节前允许的颜色差异。越高保留复杂噪声区域的细节越多，越低丢弃越多。默认 1.25。");
            float clipFalloff = config->DlssNrDetailReuseClipFalloff.value_or_default();
            if (MenuText::SliderFloat("颜色衰减", &clipFalloff, 0.01f, 10.0f, "%.2f sigma"))
                config->DlssNrDetailReuseClipFalloff = std::clamp(clipFalloff, 0.01f, 10.0f);
            HelpMarker("超过颜色限制后可信度的衰减速度。越高越平滑，越低越接近直接丢弃，可能产生斑点。不可为 0，默认 1。");
            float sigmaFloor = config->DlssNrDetailReuseSigmaFloor.value_or_default();
            if (MenuText::SliderFloat("平坦区域下限", &sigmaFloor, 0.0f, 1.0f, "%.3f"))
                config->DlssNrDetailReuseSigmaFloor = std::clamp(sigmaFloor, 0.0f, 1.0f);
            HelpMarker("平坦区域使用的最小颜色范围，避免噪声令细节被丢弃。越高保留越多，默认 0.010。");
            bool withFg = config->DlssNrDetailReuseWithFg.value_or_default();
            if (MenuText::Checkbox("插帧时保持启用", &withFg))
                config->DlssNrDetailReuseWithFg = withFg;
            HelpMarker("插帧时继续细节复用。关闭则在插帧期间暂停复用；默认开启。");
            float minFps = config->DlssNrDetailReuseMinFps.value_or_default();
            if (MenuText::SliderFloat("最低真实帧率", &minFps, 0.0f, 120.0f, "%.0f fps"))
                config->DlssNrDetailReuseMinFps = std::clamp(minFps, 0.0f, 240.0f);
            HelpMarker("真实渲染帧率高于此值才复用；低于此值时每帧运行模型。越高越早停止复用。0 为不限，默认 0。");
            float maxDropped = config->DlssNrDetailReuseMaxDropped.value_or_default();
            if (MenuText::SliderFloat("快速运动时暂停复用", &maxDropped, 0.0f, 50.0f, "丢弃 %.0f%%"))
                config->DlssNrDetailReuseMaxDropped = std::clamp(maxDropped, 0.0f, 50.0f);
            HelpMarker("新出现画面占比超过此值时，每帧运行模型，减少快速转动造成的边缘闪烁。越低越早暂停。0 为从不暂停，默认 14%。");
            ImGui::TreePop();
        }
        const auto& status = detailReuseStatus;
        // Counters for tuning and bug reports, folded away by default so the menu is not left with a blank
        // stretch while they are hidden. The slot inside keeps the lines below still while it is open.
        if (MenuPages::Show(MenuPages::Category::Nonessential) && MenuText::TreeNode("统计##detailReuse"))
        {
            // One line while it is not running, up to four while it is, and the last three come and go with the picture's
            // motion: always four lines, so nothing below moves with the camera.
            const float reuseSlot = StatusSlotBegin();
            if (!status.why.empty())
                MenuText::TextDisabled("细节复用：%s（NR 调度 %.0f 次/秒）", NrOptionsStatusChinese(status.why).c_str(), status.baseFps);
            else
            {
                MenuText::Text("复用调度：完整 %llu | 复用 %llu | 回退 %llu | %.0f 次/秒", status.full,
                            status.reused, status.fallback, status.baseFps);
                if (status.held > 0 || status.holding)
                {
                    if (status.holding)
                        MenuText::TextUnformatted("复用：快速运动期间暂停");
                    if (status.dropped >= 0.0f)
                        MenuText::TextDisabled("上次测量画面的 %.0f%% 无可搬运细节；累计暂停 %llu 帧",
                                            100.0f * status.dropped, status.held);
                    else
                        MenuText::TextDisabled("累计暂停 %llu 帧", status.held);
                }
                if (status.heavyMs > 0.0)
                {
                    MenuText::Text("NR GPU：平均 %.2f ms（%.2f 至 %.2f）", status.averageMs,
                                status.lightMs, status.heavyMs);
                    HelpMarker("完整帧与复用帧耗时不同。运动不顺畅时，可将帧率上限设置为略低于平均值。");
                }
            }
            StatusSlotEnd(reuseSlot, 4);
            ImGui::TreePop();
        }
    }
    ImGui::PopItemWidth();
}

static void RenderLutSection(Config* config)
{
    static std::vector<std::filesystem::path> lutFiles = ScanLutFolder();

    const std::string current = config->DlssNrLutFile.value_or_default();
    const std::string preview = current.empty() ? "无" : std::filesystem::path(current).filename().string();

    if (MenuText::BeginCombo("LUT", preview.c_str()))
    {
        const bool noneSelected = current.empty();
        if (MenuText::Selectable("无", noneSelected))
            config->DlssNrLutFile = std::string();
        if (noneSelected)
            ImGui::SetItemDefaultFocus();

        for (const auto& path : lutFiles)
        {
            const std::string pathStr = path.string();
            const bool selected = pathStr == current;
            if (MenuText::Selectable(path.filename().string().c_str(), selected))
                config->DlssNrLutFile = pathStr;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    HelpMarker("用 .cube LUT 对 NR 输入调色。将文件放入 OptiScaler\\LUTs 后重新扫描。强烈调色也会影响曝光、肤色检测和复用。");
    ImGui::SameLine();
    if (MenuText::SmallButton("重新扫描##lut"))
        lutFiles = ScanLutFolder();
    HelpMarker("重新扫描 LUT 文件夹中的 .cube 文件。");


    if (!current.empty())
    {
        float strength = config->DlssNrLutStrength.value_or_default();
        if (MenuText::SliderFloat("LUT 强度", &strength, 0.0f, 1.0f, "%.2f"))
            config->DlssNrLutStrength = strength;

        ImGui::SameLine();
        if (MenuText::SmallButton("重置##lutstrength", !NextgenSliderStyle::IsDefault(strength, 1.0f)))
            config->DlssNrLutStrength = 1.0f;

        HelpMarker("LUT 调色的混合强度。0 为无效果，1 为完整应用。");
    }

    const auto lutStatus = DlssNr::ReadLutStatus();
    if (MenuText::TreeNode("状态##lut"))
    {
        const float lutSlot = StatusSlotBegin();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (!lutStatus.seen)
            MenuText::TextWrapped("等待画面。");
        else if (!lutStatus.wanted)
            MenuText::TextWrapped("未加载 LUT。");
        else if (lutStatus.loaded)
            MenuText::TextWrapped("已加载：%s（%d^3）",
                               std::filesystem::path(lutStatus.loadedPath).filename().string().c_str(), lutStatus.size);
        else
            MenuText::TextWrapped("未加载 LUT。");
        ImGui::PopStyleColor();

        if (lutStatus.failed)
        {
            // Wrapped: a parse error can be longer than the menu is wide. A failed attempt does not
            // necessarily mean nothing is loaded -- a working LUT stays active if a later, different
            // path fails (DlssNr_Lut.h's documented contract) -- so this is shown alongside, not
            // instead of, the line above.
            ImGui::PushStyleColor(ImGuiCol_Text, NextgenUi::ReadableTextColor(ImVec4(1.0f, 0.85f, 0.3f, 1.0f)));
            MenuText::TextWrapped("%s: %s", std::filesystem::path(lutStatus.attemptedPath).filename().string().c_str(),
                               lutStatus.error.c_str());
            ImGui::PopStyleColor();
        }
        StatusSlotEnd(lutSlot, 5);
        ImGui::TreePop();
    }
}

// Compress screen edges, in the NR Input "Size" group after the Downscaler. The checkbox, the layout, what is happening,
// the on-screen layout, and the advanced values. The runtime rebuilds on any change (a layout change resets NR's
// history once); every edit goes through Spatial::Constrain, the same limits the runtime checks against.
static void StoreSpatialSettings(Config* config, const DlssNr::Spatial::Settings& s)
{
    config->DlssNrSpatialCenterX = s.centerX;
    config->DlssNrSpatialCenterY = s.centerY;
    config->DlssNrSpatialWorkX = s.workX;
    config->DlssNrSpatialWorkY = s.workY;
    config->DlssNrSpatialOffsetX = s.offsetX;
    config->DlssNrSpatialOffsetY = s.offsetY;
    config->DlssNrSpatialShiftX = s.shiftX;
    config->DlssNrSpatialShiftY = s.shiftY;
}

// Draws exactly the boxes the runtime published, as fractions of the frame: cyan for the sharp middle, orange for the
// part of the frame NR works on. Behind the menu, never in what the model is shown. Not saved; only while the menu is
// open (this runs from the menu).
static void DrawEdgeCompressionBoxes(const DlssNr::Spatial::Published& published, float menuResScale)
{
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    const float thickness = std::max(1.0f, 2.0f * menuResScale);

    const auto box = [&](const DlssNr::Spatial::Rect& r, ImU32 colour)
    {
        list->AddRect(ImVec2(r.left * size.x, r.top * size.y), ImVec2(r.right * size.x, r.bottom * size.y), colour, 0.0f,
                      0, thickness);
    };

    box(published.work, IM_COL32(255, 150, 0, 230));
    box(published.center, IM_COL32(0, 230, 255, 230));
}

static void RenderEdgeCompression(Config* config, float menuResScale, const NrCommon& nr)
{
    namespace Sp = DlssNr::Spatial;

    bool on = config->DlssNrSpatialCompression.value_or_default();

    if (MenuText::Checkbox("压缩画面边缘（实验功能）", &on))
        config->DlssNrSpatialCompression = on;

    HelpMarker("减少模型处理像素：中央保持清晰，边缘压缩后再展开。降低开销，但边缘可能变软或闪烁；不能与细节复用同时运行。");

    if (!on)
        return;

    Sp::Settings settings = Sp::ReadSettings(*config);
    const float scale = EdgeCompressionScale(config, nr);
    const int matched = Sp::MatchedLayout(settings);

    // Three named layouts are the only pickable values; "自定义" is a status, not a fourth choice.
    char layoutItems[3][48];
    const char* layoutNames[3];
    for (int i = 0; i != 3; ++i)
    {
        std::snprintf(layoutItems[i], sizeof(layoutItems[i]), "%s (%.0f / %.0f)", Sp::kNamedLayouts[i].name,
                      Sp::kNamedLayouts[i].center, Sp::kNamedLayouts[i].work);
        layoutNames[i] = layoutItems[i];
    }

    // Keep -1 when the current values match no named layout so the group shows no active selection.
    int layoutChoice = matched;
    if (MenuText::Segmented("布局", &layoutChoice, layoutNames, IM_ARRAYSIZE(layoutNames)) && matched != layoutChoice)
    {
        // Picking a layout is also the reset: it puts every advanced value back to that layout's own.
        Sp::ApplyNamedLayout(settings, layoutChoice);
        Sp::Constrain(settings, scale);
        StoreSpatialSettings(config, settings);
    }

    // What the advanced values add up to when they are none of the above; shown, not picked.
    if (matched < 0)
        MenuText::TextDisabled("自定义（已手动更改）");

    HelpMarker("选择中央清晰范围与模型处理范围。轻度约 90% 像素，均衡约 81%，强力约 64%。选择预设会重置高级值；自定义表示已手动更改。");

    // What is happening. A slot of two lines, so the controls below do not move when this changes length.
    const float slot = StatusSlotBegin();
    EdgeCompressionDrawText(EdgeCompressionText(config, nr));

    // Only while compression is applied: when it is not (too small, a thin edge, turned itself off) Reuse detail runs.
    if (config->DlssNrDetailReuse.value_or_default() && nr.enabled &&
        EdgeCompressionNow(nr).status == Sp::Status::Active)
        MenuText::TextDisabled("应用边缘压缩时，不运行跨帧细节复用。");

    StatusSlotEnd(slot, 2);

    // The layout on screen: not saved, and only while the menu is open.
    static bool showLayout = false;
    MenuText::Checkbox("在画面显示布局##edgecompression", &showLayout);
    HelpMarker("菜单打开时显示两条轮廓：青色为清晰中央区域，橙色为模型处理区域。橙线外不进入模型。仅用于观察，不保存，也不会进入模型输入。");

    if (showLayout)
    {
        const auto published = EdgeCompressionNow(nr);

        if (published.status == Sp::Status::Active)
            DrawEdgeCompressionBoxes(published, menuResScale);
        else
            MenuText::TextDisabled("无可显示内容：压缩未运行。");
    }

    // The eight values behind the layout, each committed on release (a change rebuilds NR's model) with its own Reset.
    if (MenuText::TreeNode("高级布局##edgecompression"))
    {
        const float minimumWork = Sp::MinimumWorkPercent(scale);
        bool changed = false;

        // Limits from the values as they stand; Constrain below keeps every pair consistent after an edit.
        const auto xShift = Sp::WorkShiftLimits(settings, false);
        const auto yShift = Sp::WorkShiftLimits(settings, true);
        const float xOffset = Sp::MaxCenterOffset(settings.centerX);
        const float yOffset = Sp::MaxCenterOffset(settings.centerY);
        const auto range = [](float low, float high) { return std::pair<float, float> { low, std::max(low, high) }; };

        {
            const auto r = range(1.0f, settings.workX - 0.5f);
            changed |= DeferredSlider("清晰中央宽度", &config->DlssNrSpatialCenterX, r.first, r.second, 80.0f,
                                      "%.1f%%");
            HelpMarker("完全清晰的中央宽度。越高保留越多、节省越少，越低节省越多。");
        }
        {
            const auto r = range(1.0f, settings.workY - 0.5f);
            changed |= DeferredSlider("清晰中央高度", &config->DlssNrSpatialCenterY, r.first, r.second, 80.0f,
                                      "%.1f%%");
            HelpMarker("完全清晰的中央高度。越高保留越多、节省越少，越低节省越多。");
        }
        {
            const auto r = range(std::max(minimumWork, settings.centerX + 0.5f), 100.0f);
            changed |= DeferredSlider("NR 处理宽度", &config->DlssNrSpatialWorkX, r.first, r.second, 90.0f,
                                      "%.1f%%");
            HelpMarker("NR 处理的宽度比例。越低节省越多、边缘压缩越强，越高越温和。");
        }
        {
            const auto r = range(std::max(minimumWork, settings.centerY + 0.5f), 100.0f);
            changed |= DeferredSlider("NR 处理高度", &config->DlssNrSpatialWorkY, r.first, r.second, 90.0f,
                                      "%.1f%%");
            HelpMarker("NR 处理的高度比例。越低节省越多、边缘压缩越强，越高越温和。");
        }
        {
            const auto r = range(-xOffset, xOffset);
            changed |= DeferredSlider("中央左右位置", &config->DlssNrSpatialOffsetX, r.first, r.second,
                                      0.0f, "%+.1f%%");
            HelpMarker("按画面比例水平移动清晰中央区域，负值向左、正值向右；适合主体不在中央的画面。");
        }
        {
            const auto r = range(-yOffset, yOffset);
            changed |= DeferredSlider("中央上下位置", &config->DlssNrSpatialOffsetY, r.first, r.second, 0.0f,
                                      "%+.1f%%");
            HelpMarker("按画面比例垂直移动清晰中央区域，负值向上、正值向下。");
        }
        {
            const auto r = range(xShift.first, xShift.second);
            changed |= DeferredSlider("左右边缘像素分配", &config->DlssNrSpatialShiftX, r.first, r.second, 0.0f,
                                      "%+.1f%%");
            HelpMarker("在左右边缘间分配模型像素。正值令右边更清晰、左边更软，负值相反；极端值会关闭压缩。");
        }
        {
            const auto r = range(yShift.first, yShift.second);
            changed |= DeferredSlider("上下边缘像素分配", &config->DlssNrSpatialShiftY, r.first, r.second, 0.0f,
                                      "%+.1f%%");
            HelpMarker("在上下边缘间分配模型像素。正值给下边更多像素、上边更少，负值相反。");
        }

        if (changed)
        {
            Sp::Settings edited = Sp::ReadSettings(*config);
            Sp::Constrain(edited, scale);
            StoreSpatialSettings(config, edited);
        }

        ImGui::TreePop();
    }
}

// NR Input.
static void RenderSharedResolution(Config* config, float menuResScale, const NrCommon& nr)
{
    ImGui::PushItemWidth(220.0f * menuResScale);

    // Same definitions as at the top of NR Options.
    const bool finishedPicture = nr.finishedPicture;
    const bool beforeSr = config->DlssNrRunBeforeSr.value_or_default() ||
                          (finishedPicture && config->DlssNrDeferredDlss.value_or_default());
    const auto activeFeature = State::Instance().currentFeature;
    const bool rayReconstruction = activeFeature && activeFeature->GetUpscalerType() == Upscaler::DLSSD;


    // Any percentage, rather than a handful of steps somebody chose in advance. The lower bound
    // is 25%: below that the model is working on so little of the picture that its answer no
    // longer survives being enlarged onto it.
    // Applied when the handle is let go, not while it is moving.
    //
    // Every distinct value here is a different working size, and a different working size tears
    // down the scratch textures and rebuilds the model. Writing it on each pixel of a drag meant
    // dozens of rebuilds in a second, which is felt as the whole frame hitching. The slider still
    // reads live; only the commit waits.

    bool resolutionAuto = config->DlssNrModelResolutionAuto.value_or_default();
    const bool autoActive = resolutionAuto && (!beforeSr || rayReconstruction);

    int scalePercent = autoActive ? DlssNr::CurrentModelResolutionPercent()
                      : pendingScale >= 0
                           ? pendingScale
                           : (int) lroundf(config->DlssNrWorkingScale.value_or_default() * 100.0f);

    ImGui::BeginDisabled(autoActive);
    if (MenuText::SliderInt("模型分辨率", &scalePercent, 25, 200, "%d%%"))
        pendingScale = scalePercent;

    // Captured right here, before the Reset button below becomes the new "last item" --
    // IsItemDeactivatedAfterEdit() only ever reports on whatever was most recently
    // submitted, so checking it after the button would report the button's state, not
    // the slider's release, and the commit below would never fire.
    const bool sliderReleased = ImGui::IsItemDeactivatedAfterEdit();

    ImGui::SameLine();
    if (MenuText::SmallButton("重置##modelresolution", !NextgenSliderStyle::IsDefault(scalePercent, 100)))
    {
        config->DlssNrWorkingScale = 1.0f;
        pendingScale = -1;
    }
    ImGui::EndDisabled();

    if (!autoActive && sliderReleased && pendingScale >= 0)
    {
        config->DlssNrWorkingScale = std::clamp(pendingScale, 25, 200) / 100.0f;
        pendingScale = -1;
        // The saved layout is not rewritten for a Model resolution: Strong at 30% would become a permanent Custom. When the
        // NR picture would be too small the status line says so and compression is not applied (Constrain is for edits to
        // the layout's own sliders).
    }

    HelpMarker("相对输入画面的模型处理尺寸。越低越快，但细节更少；超过 100% 会提高开销。游戏输出分辨率不变。");

    {
        unsigned int modelWidth = 0;
        unsigned int modelHeight = 0;

        // Native Vulkan keeps its own; the network pools 2x2 in the same model on both.
        if (nr.vulkan)
            DlssNr::CurrentModelSizeVk(modelWidth, modelHeight);
        else
            DlssNr::CurrentModelSize(modelWidth, modelHeight);

        if (modelWidth != 0 && modelHeight != 0)
            MenuText::TextDisabled("模型输入 %ux%u；主网络在 %ux%u 上运行。", modelWidth, modelHeight,
                                (modelWidth + 1) / 2, (modelHeight + 1) / 2);
    }

    if (MenuText::Checkbox("自动（仅超分后）", &resolutionAuto))
        config->DlssNrModelResolutionAuto = resolutionAuto;
    HelpMarker("超分后运行 NR 时，模型分辨率跟随游戏超分的输入与输出比例。超分前 NR 忽略此选项。");

    if (autoActive)
        MenuText::TextDisabled("NR 缩放：%.2fx，由超分输入输出比例决定。",
                            scalePercent / 100.0f);
    else if (scalePercent > 100)
        MenuText::TextDisabled("NR 缩放：%.2fx；提高分辨率会增加 GPU 开销。",
                            scalePercent / 100.0f);

    if (scalePercent > 100)
    {
        static const char* dsNames[] = { "FSR1", "双三次", "Catmull-Rom", "Lanczos2",
                                         "Lanczos3", "Kaiser2", "Kaiser3", "MAGIC" };
        int ds = (int) config->DlssNrScalingDownscaler.value_or_default();
        if (ds < 0 || ds >= IM_ARRAYSIZE(dsNames))
            ds = (int) Scaler::Lanczos3;

        if (MenuText::Combo("缩小滤镜（NR）", &ds, dsNames, IM_ARRAYSIZE(dsNames)))
            config->DlssNrScalingDownscaler = (Scaler) ds;

        HelpMarker("模型分辨率超过 100% 时缩小结果的滤镜。\n较锐利滤镜可能在边缘产生振铃。");
    }

    ImGui::PopItemWidth();
}

static void RenderInputExtras(Config* config, float menuResScale, const NrCommon& nr)
{
    ImGui::PushItemWidth(220.0f * menuResScale);
    RenderEdgeCompression(config, menuResScale, nr);
    ImGui::PopItemWidth();
}

// HDR owns exposure interpretation and LUT presentation. The controls retain their existing
// Config fields and runtime consumers; this is only a navigation reclassification.
static void RenderHdrPage(Config* config, float menuResScale)
{
    ImGui::PushItemWidth(220.0f * menuResScale);
    MenuText::SeparatorText("HDR");
    RenderLutSection(config);
    ImGui::PopItemWidth();

    // Exposure controls keep the default width they always had.
    RenderExposureSection(config, menuResScale);
}

// The model resolution NR applies, as a percentage: the slider, or with Auto (post-SR only) the
// ratio the runtime derived from render:output. The same rule the Input page uses for its slider.
static float AppliedModelScale(Config* config, const NrCommon& nr)
{
    const bool beforeSr = config->DlssNrRunBeforeSr.value_or_default() ||
                          (nr.finishedPicture && config->DlssNrDeferredDlss.value_or_default());
    const auto activeFeature = State::Instance().currentFeature;
    const bool rayReconstruction = activeFeature && activeFeature->GetUpscalerType() == Upscaler::DLSSD;
    const bool autoActive = config->DlssNrModelResolutionAuto.value_or_default() && (!beforeSr || rayReconstruction);

    return autoActive ? DlssNr::CurrentModelResolutionPercent() / 100.0f : config->DlssNrWorkingScale.value_or_default();
}

static void RenderSharedUpsampling(Config* config, float menuResScale, const NrCommon& nr)
{
    ImGui::PushItemWidth(220.0f * menuResScale);
    // Meaningful only when the model runs BELOW the frame's size. At 100% -- and above, where
    // supersampling composites its down-legged answer at native -- the residual collapses to the
    // model's own picture and the two modes are identical, so the control says so by going grey.
    {
        // The applied scale, not the slider: with Auto after SR the slider can say 100% while NR runs smaller.
        const bool reduced = AppliedModelScale(config, nr) < 0.999f;

        if (!reduced)
            ImGui::BeginDisabled();

        static const char* enlargeNames[] = { "传统", "匹配残差", "NVIDIA 残差" };
        int enlarge = (int) std::min(config->DlssNrTransfer.value_or_default(), 2u);

        if (MenuText::Segmented("放大模式", &enlarge, enlargeNames, IM_ARRAYSIZE(enlargeNames)))
            config->DlssNrTransfer = (uint32_t) enlarge;

        if (!reduced)
            ImGui::EndDisabled();

        HelpMarker("模型分辨率低于 100% 时如何放大结果。\n传统：放大整个输出。\n匹配残差：仅放大 NR 修改，减少模糊和偏色。\nNVIDIA 残差：按 NVIDIA 方式放大修改。");

        if (!reduced)
            ImGui::BeginDisabled();

        static const char* upscaleMethodNames[] = { "双线性（快速）", "SGSR1" };
        int upscaleMethod = (int) std::min(config->DlssNrReducedUpscaleMethod.value_or_default(), 1u);

        if (MenuText::Segmented("放大方法", &upscaleMethod, upscaleMethodNames, IM_ARRAYSIZE(upscaleMethodNames)))
            config->DlssNrReducedUpscaleMethod = (uint32_t) upscaleMethod;

        if (!reduced)
            ImGui::EndDisabled();

        HelpMarker("低于 100% 模型分辨率时的放大滤镜。双线性开销最低但较软；SGSR1 感知边缘，较锐利。");
    }

    ImGui::PopItemWidth();
}

// NR Output, including Effect strength.
static void RenderOutputPage(Config* config, float menuResScale, const NrCommon& nr)
{
    ImGui::PushItemWidth(220.0f * menuResScale);


    // How the game's colour is decoded (DlssNr_ColourEncoding.h). Auto trusts the game; a forced choice is applied
    // even when the format looks wrong for it, and the line under the combo says so.
    {
        static const char* encodingNames[] = { "自动", "线性 HDR", "色调映射后的 sRGB", "色调映射后的伽马 2.2",
                                               "PQ（HDR10）" };
        static_assert(IM_ARRAYSIZE(encodingNames) == DlssNrColourEncoding::kSettingCount);
        static_assert(DlssNrSettingsQueue::kColourEncodingCount == DlssNrColourEncoding::kSettingCount);
        int encoding = (int) config->DlssNrColourEncoding.value_or_default();
        if (encoding < 0 || encoding >= (int) DlssNrColourEncoding::kSettingCount)
            encoding = 0;
        if (MenuText::Segmented("颜色编码", &encoding, encodingNames, IM_ARRAYSIZE(encodingNames)))
        {
            if (DlssNrSettingsQueue::SubmitColourEncoding((uint32_t) encoding))
            {
                config->DlssNrColourEncoding = (uint32_t) encoding;
                LOG_INFO("DLSS-NR colour encoding queued: {}", encodingNames[encoding]);
            }
        }
        HelpMarker("NR 如何读取游戏颜色。自动默认信任游戏信息；仅在褪色、过暗或出现色带时尝试其他编码。");


    }

    // Experimental. 0 off (soft knee), 1 Reversible curve + our composition, 2 Reversible curve +
    // pure-inverse replace, 3 Balanced+composed, 4 Balanced+replace (identity midtones + unclipped
    // highlights), 5 HLG+composed, 6 PQ+composed (BT.2100 / ST 2084, white per BT.2408), 7 Linear+composed (what
    // game integrations hand the model). Always shown.
    static const char* reversibleNames[] = { "关闭（柔和高光）",          "可逆曲线＋合成",
                                             "可逆曲线＋替换", "均衡曲线＋合成",
                                             "均衡曲线＋替换",   "HLG 曲线＋合成",
                                             "PQ 曲线＋合成",        "线性＋合成" };
    static_assert(IM_ARRAYSIZE(reversibleNames) == DlssNrProxyCurve::kCount, "one name per proxy curve");
    const uint32_t reversibleValue = config->DlssNrReversibleMode.value_or_default();
    const int reversible = DlssNrProxyCurve::Valid(reversibleValue) ? (int) reversibleValue : 0;
    if (MenuText::BeginCombo("最终画面合成（实验功能）", reversibleNames[reversible]))
    {
        for (int i = 0; i < (int) DlssNrProxyCurve::kPickable; ++i)
        {
            const bool selected = i == reversible;
            if (MenuText::Selectable(reversibleNames[i], selected))
                config->DlssNrReversibleMode = (uint32_t) i;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    HelpMarker("控制亮度映射和结果合成。\n柔和／均衡：压缩高光，保留中间调。\n可逆曲线：可准确反向映射。\nHLG／PQ：保留更多高光，但模型看到的中间调不同。\n线性：NVIDIA 原生外观，配合游戏曝光和原生外观起点。\n合成：按强度混合；替换：直接使用模型结果，闪烁时可降低高光保护。\n切换后请重新自动调优。");

    if (DlssNrProxyCurve::IsReplace((uint32_t) reversible))
    {
        float replaceDetail = config->DlssNrReplaceDetailStrength.value_or_default();
        if (MenuText::SliderFloat("恢复锐度", &replaceDetail, 0.0f, 2.0f, "%.2f"))
            config->DlssNrReplaceDetailStrength = replaceDetail;

        ImGui::SameLine();
        if (MenuText::SmallButton("重置##replacedetail", !NextgenSliderStyle::IsDefault(replaceDetail, 0.5f)))
            config->DlssNrReplaceDetailStrength = 0.5f;

        HelpMarker("利用原画锐化细小边缘。越高越锐利，0 为关闭。仅在模型分辨率低于 100% 时生效，替换模式中最有用。");
    }


    float transfer = config->DlssNrTransferStrength.value_or_default();
    if (MenuText::SliderFloat("细节强度", &transfer, 0.0f, 2.0f, "%.2f"))
        config->DlssNrTransferStrength = transfer;

    ImGui::SameLine();
    if (MenuText::SmallButton("重置##detail", !NextgenSliderStyle::IsDefault(transfer, 1.0f)))
        config->DlssNrTransferStrength = 1.0f;

    HelpMarker("越高 NR 细节越强，超过 1 为增强；越低越弱，0 为无效果。默认 1。");

    float colour = config->DlssNrColourStrength.value_or_default();
    if (MenuText::SliderFloat("颜色强度", &colour, 0.0f, 4.0f, "%.2f"))
        config->DlssNrColourStrength = colour;

    ImGui::SameLine();
    if (MenuText::SmallButton("重置##colour", !NextgenSliderStyle::IsDefault(colour, 1.0f)))
        config->DlssNrColourStrength = 1.0f;

    HelpMarker("越高 NR 颜色变化越强，超过 1 可能更饱和；0 保留游戏颜色。默认 1。");

    ImGui::PopItemWidth();
}

// Keep presentation choices local to the layer page; the shared Look renderer owns its controls.
static void RenderLayerLook(Config* config, unsigned layer)
{
    const bool allowIndependent = CurrentNativeApi() != NativeApi::Vulkan;
    const bool independent = allowIndependent && config && config->DlssNrLayerLookMode.value_or_default();
    const char* resetText = NextgenUi::English() ? "Reset" : "重置";
    const float resetWidth = independent
        ? ImGui::CalcTextSize(resetText).x + ImGui::GetStyle().FramePadding.x * 2.f : ImGui::GetFrameHeight();
    const NextgenSliderStyle::ScopedResetWidth reset(resetWidth);
    const MenuText::ScopedSeparatorOmission heading(layer == 0u
        ? (NextgenUi::English() ? "Layer 1 Look" : "第一层 Look") : "");
    NextgenLookControls::RenderLayer(config, layer, VisiblePassCount(config), allowIndependent);
}

// Mode policy stays on NR Global; actual parameter editors belong to their layer pages.
static void RenderGlobalLookPage(Config* config)
{
    NextgenLookControls::RenderGlobal(config,VisiblePassCount(config),CurrentNativeApi()!=NativeApi::Vulkan);
}

// Model passes and Colour.
static void RenderFirstLayerParameters(Config* config, float menuResScale)
{
    ImGui::PushItemWidth(220.0f * menuResScale);

    static const char* styles[] = { "标准", "自然", "电影感" };
    static const char* presets[] = { "默认", "预设 A", "预设 B" };
    const bool profilesLocked = DriverProxyProfilesLocked(config);
    if (profilesLocked)
        MenuText::TextWrapped("驱动代理模式暂不支持在游戏中切换模型和预设，已锁定相关选项与多次处理预设。");

    {
        // Preserve the former TreeNode's ID scope without its title or indentation.
        ImGui::PushID(MenuText::StableLabel("第 1 次处理").c_str());
        ImGui::BeginDisabled(profilesLocked);
        uint32_t style = config->DlssNrStyle.value_or_default();
        if (ProfileChoiceCombo("风格", &style, nullptr, styles, IM_ARRAYSIZE(styles)))
            config->DlssNrStyle = style;
        ImGui::EndDisabled();
        HelpMarker("这是画面风格选择，不会切换模型文件。模型预设单独控制模型渲染提示。");
        ImGui::BeginDisabled(profilesLocked);
        uint32_t preset = config->DlssNrPreset.value_or_default();
        if (ProfileChoiceCombo("模型预设", &preset, nullptr, presets, IM_ARRAYSIZE(presets)))
        {
            const auto before = config->DlssNrPreset.value_or_default();
            config->DlssNrPreset = preset;
            Profiles::ClampConfiguredModelScalars(*config);
            if constexpr (F6InputPolicy::FsrBridgeEnabled)
                GenshinSubstage::RequestPreset(0u, before, preset);
        }
        ImGui::EndDisabled();
        HelpMarker("与模型选择独立的预设提示：默认、预设 A、预设 B。切换时将强度收敛到当前预设上限：B 为 1，其他为 2；默认均为 1。未知／旧版值会保留，直到主动选择其他选项。");
        const auto effective = Profiles::EffectivePassProfileFor(*config, 0u);
        const float scalarMax = Profiles::ScalarMaximum(effective.preset);
        ModelScalarSlider("整体强度", &config->DlssNrIntensity, 0.0f, scalarMax, 1.0f);
        ModelScalarSlider("局部结构", &config->DlssNrLocalStructure, 0.0f, scalarMax, 1.0f);
        ModelScalarSlider("局部色调", &config->DlssNrLocalTone, 0.0f, scalarMax, 1.0f);
        ModelScalarSlider("皮肤结构", &config->DlssNrSkinStructure, -1.0f, scalarMax, 1.0f);
        bool mask = config->DlssNrAutoMask.value_or_default();
        if (MenuText::Checkbox("自动皮肤遮罩", &mask))
            config->DlssNrAutoMask = mask;
        HelpMarker("使用模型自身皮肤检测控制皮肤结构，准确度因场景而异；与下方按颜色选择的遮罩不同。");
        ImGui::PopID();
    }

    ImGui::PopItemWidth();
}

static void RenderSecondLayerParameters(Config* config, float menuResScale)
{
    ImGui::PushItemWidth(220.0f * menuResScale);

    static const char* styles[] = { "标准", "自然", "电影感" };
    static const char* presets[] = { "默认", "预设 A", "预设 B" };
    const bool profilesLocked = DriverProxyProfilesLocked(config);
    if (profilesLocked)
        MenuText::TextWrapped("驱动代理模式暂不支持在游戏中切换模型和预设，已锁定相关选项与多次处理预设。");

    {
        ImGui::PushID(MenuText::StableLabel("第 2 次处理").c_str());
        MenuText::TextWrapped("默认继承第 1 次处理的四项强度，初始均为 1。重置恢复继承。");
        ImGui::BeginDisabled(profilesLocked);
        InheritedProfileCombo("风格", &config->DlssNrPass2Style, styles, IM_ARRAYSIZE(styles));
        HelpMarker("模型与预设可分别继承第 1 次处理，或为本次处理指定选项。未启用本次处理时，选择会在之后启用时使用。");
        InheritedPresetCombo(config, 1u, &config->DlssNrPass2Preset, presets, IM_ARRAYSIZE(presets));
        ImGui::EndDisabled();
        HelpMarker("模型与预设可分别继承第 1 次处理，或为本次处理指定选项。未启用本次处理时，选择会在之后启用时使用。");
        const auto effective = Profiles::EffectivePassProfileFor(*config, 1u);
        const float scalarMax = Profiles::ScalarMaximum(effective.preset);
        ModelScalarSlider("整体强度", &config->DlssNrPass2Intensity, 0.0f, scalarMax, effective.tuning.intensity, "%.2f", true);
        ModelScalarSlider("局部结构", &config->DlssNrPass2LocalStructure, 0.0f, scalarMax, effective.tuning.structure, "%.2f", true);
        ModelScalarSlider("局部色调", &config->DlssNrPass2LocalTone, 0.0f, scalarMax, effective.tuning.tone, "%.2f", true);
        ModelScalarSlider("皮肤结构", &config->DlssNrPass2SkinStructure, -1.0f, scalarMax, effective.tuning.skin, "%.2f", true);
        bool mask = config->DlssNrPass2AutoMask.has_value() ? config->DlssNrPass2AutoMask.value() : config->DlssNrAutoMask.value_or_default();
        if (MenuText::Checkbox("自动皮肤遮罩", &mask))
            config->DlssNrPass2AutoMask = mask;
        ImGui::SameLine();
        if (MenuText::SmallButton("重置##mask"))
            config->DlssNrPass2AutoMask = std::optional<bool> {};
        ImGui::PopID();
    }

    ImGui::PopItemWidth();
}

static void RenderRemainingPasses(Config* config, float menuResScale)
{
    ImGui::PushItemWidth(220.0f * menuResScale);

    const unsigned int visiblePasses = VisiblePassCount(config);

    if (visiblePasses >= 3u)
    {
        MenuText::SeparatorText("模型与处理参数");
    }
    static const char* styles[] = { "标准", "自然", "电影感" };
    static const char* presets[] = { "默认", "预设 A", "预设 B" };
    const bool profilesLocked = DriverProxyProfilesLocked(config);
    if (visiblePasses >= 3u && profilesLocked)
        MenuText::TextWrapped("驱动代理模式暂不支持在游戏中切换模型和预设，已锁定相关选项与多次处理预设。");

    for (unsigned int pass = 3; pass < visiblePasses; ++pass)
    {
        auto& settings = config->DlssNrExtraPasses[pass - 3];
        if (!MenuText::TreeNode(std::format("Pass {}", pass + 1).c_str()))
            continue;
        MenuText::TextWrapped("默认继承第 1 次处理的四项强度，默认均为 1。");
        ImGui::BeginDisabled(profilesLocked);
        InheritedProfileCombo("风格", &settings.style, styles, IM_ARRAYSIZE(styles));
        ImGui::EndDisabled();
        ImGui::BeginDisabled(profilesLocked);
        InheritedPresetCombo(config, pass, &settings.preset, presets, IM_ARRAYSIZE(presets));
        ImGui::EndDisabled();
        const auto effective = Profiles::EffectivePassProfileFor(*config, pass);
        const float scalarMax = Profiles::ScalarMaximum(effective.preset);
        ModelScalarSlider("整体强度", &settings.intensity, 0.0f, scalarMax, effective.tuning.intensity, "%.2f", true);
        ModelScalarSlider("局部结构", &settings.structure, 0.0f, scalarMax, effective.tuning.structure, "%.2f", true);
        ModelScalarSlider("局部色调", &settings.tone, 0.0f, scalarMax, effective.tuning.tone, "%.2f", true);
        ModelScalarSlider("皮肤结构", &settings.skin, -1.0f, scalarMax, effective.tuning.skin, "%.2f", true);
        bool mask = settings.autoMask.value_or(config->DlssNrAutoMask.value_or_default());
        if (MenuText::Checkbox("自动皮肤遮罩", &mask))
            settings.autoMask = mask;
        ImGui::SameLine();
        if (MenuText::SmallButton("重置##mask"))
            settings.autoMask = std::optional<bool> {};
        ImGui::TreePop();
    }

    MenuText::TextWrapped("处理设置适用于 DX12 和原生 Vulkan 的超分及光线重建；驱动代理后端仅支持一次处理。");

    ImGui::PopItemWidth();
}


static void RenderThirdLayerParameters(Config* config, float menuResScale)
{
    ImGui::PushItemWidth(220.0f * menuResScale);

    const unsigned int visiblePasses = VisiblePassCount(config);
    static const char* styles[] = { "标准", "自然", "电影感" };
    static const char* presets[] = { "默认", "预设 A", "预设 B" };
    const bool profilesLocked = DriverProxyProfilesLocked(config);
    if (visiblePasses >= 3u && profilesLocked)
        MenuText::TextWrapped("驱动代理模式暂不支持在游戏中切换模型和预设，已锁定相关选项与多次处理预设。");

    if (visiblePasses >= 3u)
    {
        ImGui::PushID(MenuText::StableLabel("第 3 次处理").c_str());
        MenuText::TextWrapped("默认继承第 1 次处理的四项强度，初始均为 1。重置恢复继承。");
        ImGui::BeginDisabled(profilesLocked);
        InheritedProfileCombo("风格", &config->DlssNrPass3Style, styles, IM_ARRAYSIZE(styles));
        HelpMarker("模型与预设可分别继承第 1 次处理，或为本次处理指定选项。未启用本次处理时，选择会在之后启用时使用。");
        InheritedPresetCombo(config, 2u, &config->DlssNrPass3Preset, presets, IM_ARRAYSIZE(presets));
        ImGui::EndDisabled();
        HelpMarker("模型与预设可分别继承第 1 次处理，或为本次处理指定选项。未启用本次处理时，选择会在之后启用时使用。");
        const auto effective = Profiles::EffectivePassProfileFor(*config, 2u);
        const float scalarMax = Profiles::ScalarMaximum(effective.preset);
        ModelScalarSlider("整体强度", &config->DlssNrPass3Intensity, 0.0f, scalarMax, effective.tuning.intensity, "%.2f", true);
        ModelScalarSlider("局部结构", &config->DlssNrPass3LocalStructure, 0.0f, scalarMax, effective.tuning.structure, "%.2f", true);
        ModelScalarSlider("局部色调", &config->DlssNrPass3LocalTone, 0.0f, scalarMax, effective.tuning.tone, "%.2f", true);
        ModelScalarSlider("皮肤结构", &config->DlssNrPass3SkinStructure, -1.0f, scalarMax, effective.tuning.skin, "%.2f", true);
        bool mask = config->DlssNrPass3AutoMask.has_value() ? config->DlssNrPass3AutoMask.value() : config->DlssNrAutoMask.value_or_default();
        if (MenuText::Checkbox("自动皮肤遮罩", &mask))
            config->DlssNrPass3AutoMask = mask;
        ImGui::SameLine();
        if (MenuText::SmallButton("重置##mask"))
            config->DlssNrPass3AutoMask = std::optional<bool> {};
        ImGui::PopID();
    }


    MenuText::TextWrapped("处理设置适用于 DX12 和原生 Vulkan 的超分及光线重建；驱动代理后端仅支持一次处理。");

    ImGui::PopItemWidth();
}

static void RenderColourAndLighting(Config* config, float menuResScale)
{
    ImGui::PushItemWidth(220.0f * menuResScale);

    NrMenuBreadcrumb("passes.colour.begin");
    MenuText::SeparatorText("颜色");

    if (MenuText::TreeNode("皮肤与环境（最终修改）"))
    {
        MenuText::TextWrapped("按颜色选择皮肤，分别调整皮肤和环境的最终 NR 效果。选择可能不准确，请检查预览。");
        bool filter = config->DlssNrSkinProtection.value_or_default();
        if (MenuText::Checkbox("分别控制皮肤与环境", &filter))
            config->DlssNrSkinProtection = filter;
        ImGui::BeginDisabled(!filter);
        bool tone = config->DlssNrSkinToneEnabled.value_or_default();
        if (MenuText::Checkbox("允许肤色变化", &tone))
            config->DlssNrSkinToneEnabled = tone;
        HelpMarker("允许 NR 修改选中的皮肤颜色。关闭可保留原色，同时仍可改变细节。");
        const auto slider = [](const char* label, auto& option) {
            float v = option.value_or_default();
            if (MenuText::SliderFloat(label, &v, 0.0f, 1.0f, "%.2f"))
                option = v;
            ImGui::SameLine();
            const std::string resetId = std::string("重置##") + label;
            if (MenuText::SmallButton(resetId.c_str(), !NextgenSliderStyle::IsDefault(v, 1.0f)))
                option = 1.0f;
            HelpMarker("此区域的 NR 强度。越高越强，0 为无效果，默认 1 为完整效果。");
        };
        slider("皮肤细节／光照", config->DlssNrSkinDetail);
        ImGui::BeginDisabled(!tone);
        slider("皮肤颜色", config->DlssNrSkinColour);
        ImGui::EndDisabled();
        slider("环境细节／光照", config->DlssNrEnvironmentDetail);
        slider("环境颜色", config->DlssNrEnvironmentColour);
        bool preview = config->DlssNrShowSkinMask.value_or_default();
        if (MenuText::Checkbox("预览颜色遮罩", &preview))
            config->DlssNrShowSkinMask = preview;
        ImGui::EndDisabled();
        ImGui::TreePop();
    }

    ImGui::PopItemWidth();
}

// Process-local diagnostic capture. Only the capture API owns collection and file IO.
static void RenderGenshinManualCapture()
{
    if constexpr (!F6InputPolicy::FsrBridgeEnabled) return;
    namespace Capture = GenshinLegacy::ManualCapture;
    using Phase = Capture::Phase;
    using Reason = Capture::Reason;
    const auto tr = [](const char* cn, const char* en) { return NextgenUi::English() ? en : cn; };
    const auto phaseName = [&](Phase phase) -> const char* {
        switch (phase)
        {
        case Phase::Off: return tr("关闭（默认）", "Off (default)");
        case Phase::Requested: return tr("已请求，等待原生绘制边界", "Requested; waiting for a native draw boundary");
        case Phase::Starting: return tr("正在启动采集", "Starting capture");
        case Phase::Armed: return tr("已待命，等待已知着色器绘制", "Armed; waiting for a known shader draw");
        case Phase::Capturing: return tr("采集中", "Capturing");
        case Phase::Draining: return tr("正在停止并等待在途读回与写出", "Stopping; waiting for pending readback and export");
        case Phase::Complete: return tr("本轮已结束", "Session complete");
        case Phase::Failed: return tr("失败", "Failed");
        }
        return tr("未知阶段", "Unknown phase");
    };
    const auto reasonName = [&](Reason reason) -> const char* {
        switch (reason)
        {
        case Reason::None: return tr("无", "None");
        case Reason::AwaitingNativeBoundary: return tr("等待原生绘制边界", "Waiting for a native draw boundary");
        case Reason::UserStop: return tr("用户停止", "Stopped by user");
        case Reason::DurationLimit: return tr("达到时间上限", "Duration limit reached");
        case Reason::CountLimit: return tr("达到样本数量上限", "Sample count limit reached");
        case Reason::DiskLimit: return tr("达到尝试写入字节上限", "Attempted write byte limit reached");
        case Reason::RequestTimeout: return tr("请求超时：未遇到原生绘制边界", "Request timed out before a native draw boundary");
        case Reason::ShaderHookUnavailable: return tr("着色器创建观测不可用", "Shader creation observation unavailable");
        case Reason::OutputFailure: return tr("输出文件写入失败", "Output file write failed");
        case Reason::DeviceChanged: return tr("设备改变或重建", "Device changed or recreated");
        case Reason::ContextBoundary: return tr("上下文边界使待处理观测失效", "Context boundary invalidated pending observations");
        case Reason::Shutdown: return tr("模块正在关闭", "Module is shutting down");
        case Reason::Busy: return tr("上一轮仍在处理，暂不能重新待命", "Previous session is still busy; cannot rearm yet");
        case Reason::SessionLimit: return tr("达到本进程轮次上限", "Process session limit reached");
        case Reason::InternalFailure: return tr("采集内部错误", "Internal capture error");
        }
        return tr("未知原因", "Unknown reason");
    };

    MenuText::SeparatorText(tr("原神：手动证据采集", "Genshin: manual evidence capture"));
    MenuText::TextWrapped("%s", tr("仅原神 D3D11。默认关闭，仅本进程生效；按钮不写入 INI。待命或请求被接受均不代表已经采到数据。",
        "Genshin D3D11 only. Off by default and local to this process; buttons do not write the INI. Arming or accepting a request does not mean data was captured."));
    ImGui::PushID("genshin-manual-capture");
    auto snapshot = Capture::GetSnapshot();
    enum class Action { None, ArmRequested, ArmRejected, StopRequested };
    static Action lastAction = Action::None;
    constexpr uint32_t compiledApi = Capture::Snapshot {}.apiVersion;
    const bool knownApi = (compiledApi == 1 || compiledApi == 2) && snapshot.apiVersion == compiledApi;
    const bool active = snapshot.phase == Phase::Requested || snapshot.phase == Phase::Starting ||
        snapshot.phase == Phase::Armed || snapshot.phase == Phase::Capturing || snapshot.phase == Phase::Draining;
    const bool canArm = knownApi && snapshot.supported && snapshot.sessionNumber < snapshot.maxSessions &&
        (snapshot.phase == Phase::Off || snapshot.phase == Phase::Complete || snapshot.phase == Phase::Failed);
    const bool canStop = knownApi && active && snapshot.phase != Phase::Draining;
    const char* startLabel = tr("开始采集###arm", "Arm capture###arm");
    const char* stopLabel = tr("停止采集###stop", "Stop capture###stop");
    const float buttonsWidth = ImGui::CalcTextSize(startLabel, nullptr, true).x +
        ImGui::CalcTextSize(stopLabel, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 4.f +
        ImGui::GetStyle().ItemSpacing.x;
    const bool sameLine = buttonsWidth <= ImGui::GetContentRegionAvail().x;
    ImGui::BeginDisabled(!canArm);
    if (MenuText::Button(startLabel))
    {
        lastAction = Capture::RequestArm() ? Action::ArmRequested : Action::ArmRejected;
        snapshot = Capture::GetSnapshot();
    }
    ImGui::EndDisabled();
    if (sameLine) ImGui::SameLine();
    ImGui::BeginDisabled(!canStop);
    if (MenuText::Button(stopLabel))
    {
        Capture::RequestStop();
        lastAction = Action::StopRequested;
        snapshot = Capture::GetSnapshot();
    }
    ImGui::EndDisabled();

    if (!knownApi)
        MenuText::TextWrapped("%s", tr("捕获接口版本不受支持，操作已停用。", "Unsupported capture API version; actions are disabled."));
    if (!snapshot.supported)
        MenuText::TextWrapped("%s", tr("当前进程不支持或原神 D3D11 设备尚未就绪；无法开始采集。",
            "This process is unsupported or its Genshin D3D11 device is not ready; capture cannot be armed."));
    MenuText::TextWrapped(tr("状态：%s | 原因：%s", "State: %s | Reason: %s"),
                          phaseName(snapshot.phase), reasonName(snapshot.reason));
    if (lastAction == Action::ArmRequested && snapshot.phase == Phase::Requested)
        MenuText::TextWrapped("%s", tr("待命请求已接收；仍需等待绘制与实际写出。", "Arm request acknowledged; drawing and actual export are still pending."));
    else if (lastAction == Action::ArmRejected)
        MenuText::TextWrapped("%s", tr("最近一次待命请求未被接受，请查看原因和轮次上限。", "The last arm request was rejected; check the reason and session limit."));
    else if (lastAction == Action::StopRequested && snapshot.phase == Phase::Draining)
        MenuText::TextWrapped("%s", tr("已请求停止；在途读回和写出完成前不能重新待命。", "Stop requested; rearming must wait for pending readback and export."));

    // v2 adds actual object/consumer exports; compile against either reviewed header,
    // never treat a mismatched binary layout as a compatible snapshot.
    const auto extraExports = []<class Snapshot>(const Snapshot& value) {
        std::array<unsigned long long, 2> counts {};
        if constexpr (requires { value.shaderObjectsWritten; value.consumerLedgerWritten; })
            counts = { static_cast<unsigned long long>(value.shaderObjectsWritten),
                       static_cast<unsigned long long>(value.consumerLedgerWritten) };
        return counts;
    }(snapshot);
    const bool hasData = snapshot.shaderArtifactsWritten != 0 || snapshot.constantArtifactsWritten != 0 ||
        snapshot.bindingRecordsWritten != 0 || snapshot.recordsWritten != 0 || extraExports[0] != 0 || extraExports[1] != 0;
    MenuText::TextWrapped("%s", hasData
        ? tr("已有实际写出数据；完整证据与语义仍需审查。", "Some data has been written; evidence completeness and semantics still require review.")
        : tr("尚无实际写出数据（待命、结束或路径已创建均不等于采集成功）。", "No actual data has been written (arming, completing or creating a path does not prove capture succeeded)."));
    MenuText::TextWrapped(tr("实际写出：着色器 %llu | CB %llu | 绑定 %llu | 记录 %llu",
                            "Actual exports: shaders %llu | CB %llu | bindings %llu | records %llu"),
        static_cast<unsigned long long>(snapshot.shaderArtifactsWritten), static_cast<unsigned long long>(snapshot.constantArtifactsWritten),
        static_cast<unsigned long long>(snapshot.bindingRecordsWritten), static_cast<unsigned long long>(snapshot.recordsWritten));
    if (knownApi && compiledApi == 2)
        MenuText::TextWrapped(tr("实际观测：着色器对象 %llu | 消费记录 %llu", "Actual observations: shader objects %llu | consumer records %llu"),
                              extraExports[0], extraExports[1]);
    MenuText::TextWrapped(tr("PID %u | 本轮 %llu / %u | 已运行 %.1f / %.1f 秒",
                            "PID %u | Session %llu / %u | Elapsed %.1f / %.1f s"),
        snapshot.processId, static_cast<unsigned long long>(snapshot.sessionNumber), snapshot.maxSessions,
        snapshot.elapsedMilliseconds / 1000.0, snapshot.maxSessionMilliseconds / 1000.0);
    FILETIME created { static_cast<DWORD>(snapshot.processCreationFileTime),
                       static_cast<DWORD>(snapshot.processCreationFileTime >> 32) };
    SYSTEMTIME utc {};
    if (snapshot.processCreationFileTime != 0 && FileTimeToSystemTime(&created, &utc))
        MenuText::TextWrapped(tr("进程创建：%04u-%02u-%02u %02u:%02u:%02u.%03u UTC",
                          "Process created: %04u-%02u-%02u %02u:%02u:%02u.%03u UTC"),
            unsigned(utc.wYear), unsigned(utc.wMonth), unsigned(utc.wDay), unsigned(utc.wHour),
            unsigned(utc.wMinute), unsigned(utc.wSecond), unsigned(utc.wMilliseconds));
    else
        MenuText::TextWrapped("%s", tr("进程创建时间：未知", "Process creation time: unknown"));
    MenuText::TextWrapped(tr("尝试写入 %llu / %llu 字节 | 队列丢弃 %llu | 写入失败 %llu",
                            "Attempted writes %llu / %llu bytes | Queue drops %llu | Write failures %llu"),
        static_cast<unsigned long long>(snapshot.diskBytes), static_cast<unsigned long long>(snapshot.maxDiskBytes),
        static_cast<unsigned long long>(snapshot.queueDrops), static_cast<unsigned long long>(snapshot.writeFailures));
    MenuText::TextWrapped(tr("本轮上限：着色器 %u | CB %u | 绑定 %u | 队列 %u 项 / %llu 字节",
                            "Session limits: shaders %u | CB %u | bindings %u | queue %u jobs / %llu bytes"),
        snapshot.maxShaderArtifacts, snapshot.maxConstantSamples, snapshot.maxBindingSamples, snapshot.maxQueueJobs,
        static_cast<unsigned long long>(snapshot.maxQueuePayloadBytes));
    MenuText::TextWrapped(tr("采集入口：%s | 着色器创建观测：%s | 写出线程结束：%s",
                            "Capture entry: %s | Shader creation observation: %s | Export worker finished: %s"),
        snapshot.entryHookInstalled ? tr("可用", "ready") : tr("未就绪", "not ready"),
        snapshot.shaderHookInstalled ? tr("可用", "ready") : tr("未就绪", "not ready"),
        snapshot.workerFinished ? tr("是", "yes") : tr("否", "no"));

    const auto pathEnd = std::find(std::begin(snapshot.outputDirectory), std::end(snapshot.outputDirectory), L'\0');
    const int pathLength = static_cast<int>(pathEnd - std::begin(snapshot.outputDirectory));
    const int utf8Bytes = pathLength > 0
        ? WideCharToMultiByte(CP_UTF8, 0, snapshot.outputDirectory, pathLength, nullptr, 0, nullptr, nullptr) : 0;
    std::string outputPath(utf8Bytes > 0 ? static_cast<size_t>(utf8Bytes) : 0, '\0');
    if (utf8Bytes > 0)
        WideCharToMultiByte(CP_UTF8, 0, snapshot.outputDirectory, pathLength, outputPath.data(), utf8Bytes, nullptr, nullptr);
    MenuText::TextUnformatted(tr("输出目录：", "Output directory:"));
    if (!outputPath.empty()) ImGui::TextWrapped("%s", outputPath.c_str());
    else MenuText::TextWrapped("%s", tr("尚未创建（或路径不可用）。", "Not created yet (or path unavailable)."));
    ImGui::BeginDisabled(outputPath.empty());
    if (MenuText::SmallButton(tr("复制输出路径###copyPath", "Copy output path###copyPath")))
        ImGui::SetClipboardText(outputPath.c_str());
    ImGui::EndDisabled();
    MenuText::TextWrapped("%s", tr("这只采集诊断证据，不会启用超分、NR 或 FG，也不证明输入语义或输出消费已经成立。",
        "This collects diagnostic evidence only. It does not enable SR, NR or FG, or prove input semantics or output consumption."));
    ImGui::PopID();
}

static void RenderDetectedColourFormat()
{
        const auto status = DlssNr::ReadColourEncodingStatus();
        // The line and the warning are wrapped and the warning comes and goes with the detected format: a fixed slot.
        if (MenuText::TreeNode("检测到的格式##colourEncoding"))
        {
            const float encodingSlot = StatusSlotBegin();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            MenuText::TextWrapped("%s", status.line.c_str());
            ImGui::PopStyleColor();
            if (!status.warning.empty())
            {
                // Wrapped: the sentence is longer than the menu is wide.
                ImGui::PushStyleColor(ImGuiCol_Text, NextgenUi::ReadableTextColor(ImVec4(1.0f, 0.85f, 0.3f, 1.0f)));
                MenuText::TextWrapped("%s", status.warning.c_str());
                ImGui::PopStyleColor();
            }
            StatusSlotEnd(encodingSlot, 5);
            ImGui::TreePop();
        }
}

// R844 diagnostic-only preview drawing: no backend textures or GPU uploads.
struct GenshinFgPreviewRuns {
    struct Run { unsigned short first,end,y; ImU32 colour; };
    std::array<Run,GenshinFgPreview::N> runs{};
    GenshinFgPreview::Stamp stamp{};
    unsigned count=0;
    bool contrast=false;
    float range=0;
    void Update(const GenshinFgPreview::Snapshot& image,bool motion,bool depthContrast,float motionRange) {
        if (count && stamp.tick==image.stamp.tick && stamp.generation==image.stamp.generation &&
            stamp.providerFrame==image.stamp.providerFrame && stamp.sourceFrame==image.stamp.sourceFrame &&
            stamp.picture==image.stamp.picture && (motion ? range==motionRange : contrast==depthContrast)) return;
        count=0;stamp=image.stamp;contrast=depthContrast;range=motionRange;
        const auto byte = [](float v) { return (unsigned)(std::clamp(v,0.0f,1.0f)*255.0f+0.5f); };
        for (unsigned y=0;y<GenshinFgPreview::H;++y) for (unsigned x=0;x<GenshinFgPreview::W;++x) {
            const auto i=y*GenshinFgPreview::W+x;
            ImU32 colour=IM_COL32(255,0,255,255);
            if (motion) {
                const auto vx=image.motion[i][0],vy=image.motion[i][1];
                if (GenshinFgPreview::ValidMotion(vx,vy))
                    colour=IM_COL32(byte(0.5f+vx/(2*motionRange)),byte(0.5f+vy/(2*motionRange)),byte(std::sqrt(vx*vx+vy*vy)/motionRange),255);
            } else {
                const auto z=image.depth[i];
                if (std::isfinite(z) && z>=0 && z<=1) {
                    const float span=image.maxDepth-image.minDepth;
                    const auto value=byte(depthContrast && span>0 ? (z-image.minDepth)/span : z);
                    colour=IM_COL32(value,value,value,255);
                }
            }
            if (x && runs[count-1].colour==colour) runs[count-1].end=static_cast<unsigned short>(x+1);
            else runs[count++]={static_cast<unsigned short>(x),static_cast<unsigned short>(x+1),static_cast<unsigned short>(y),colour};
        }
    }
};

// EndGroup reports the entire preview section, rather than the switch's last
// item. Scrolling the switch offscreen must not turn off a visible image.
struct GenshinFgPreviewDemandScope {
    bool& enabled;
    const char* unavailable;
    GenshinFgPreviewDemandScope(bool& value,const char* reason):enabled(value),unavailable(reason) { ImGui::BeginGroup(); }
    ~GenshinFgPreviewDemandScope() {
        ImGui::EndGroup();
        const bool visible=ImGui::IsItemVisible();
        const bool wanted=enabled && !unavailable && visible;
        GenshinFgPreview::Trace(GenshinFgPreview::TraceStage::Control,
            unavailable?unavailable:"debug-control-reached",(enabled?1u:0u)|(visible?2u:0u)|(!unavailable?4u:0u));
        GenshinFgPreview::Request(wanted,unavailable?unavailable:!enabled?"switch-disabled":!visible?"section-clipped":"section-visible",
            []() noexcept { return MenuCommon::IsVisible(); });
    }
};

static void RenderGenshinFgGuidePreview()
{
    if constexpr (!F6InputPolicy::FsrBridgeEnabled) return;
    if (!GenshinBridgeMenuProcess()) return;
    const auto& state = State::Instance();
    const char* unavailable=state.api==Vulkan || state.swapchainApi==Vulkan ? "Vulkan-disabled" :
        state.swapchainInteropApi!=SwapchainInteropApi::Dx11wDx12 ? "Dx11wDx12-route-unavailable" : nullptr;
    static bool enabled = false, depthContrast = true;
    static float motionRange = 16.0f;
    GenshinFgPreviewDemandScope demand(enabled,unavailable);
    MenuText::SeparatorText("FG 接收图调试（独立于 NR）");
    ImGui::BeginDisabled(unavailable!=nullptr);
    MenuText::Checkbox("显示 FG 接收的深度和运动图", &enabled);
    ImGui::EndDisabled();
    if (unavailable) {
        MenuText::TextWrapped("FG 接收图不可用：%s", state.api==Vulkan || state.swapchainApi==Vulkan
            ? "Vulkan 暂不支持此预览。" : "当前未检测到原神 DX11 到 DX12 的 FG 交换链。请确认所选 FG 已开启。追踪日志记录当前路由；此控件不会切换后端。");
        return;
    }
    MenuText::TextWrapped("本开关在此显示 FG 已接收的深度/运动快照，不替换游戏画面；NR 调试视图和 FG 后端调试开关不会开启本预览。");
    if (!enabled) return;
    if (!state.currentFG || !state.currentFG->IsActive())
        MenuText::TextWrapped("预览已请求，等待 FG 后端运行并接收输入；目前没有可显示的新 FG 输入。");
    else if (state.currentFG->IsPaused())
        MenuText::TextWrapped("预览已请求，但 FG 后端已暂停；恢复并接收输入后才会更新。");
    MenuText::TextWrapped("低带宽采样 DLSSG 实际接收的深度和运动纹理，每秒最多两次、每次一个 192×120 以内区域；九个位置轮换，不拼接、不冒充全帧。紫色表示无效值；预览不会修改输入。");
    MenuText::Checkbox("深度显示自动对比度", &depthContrast);
    MenuText::SliderFloat("运动显示范围（像素）", &motionRange, 0.1f, 512.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    motionRange = std::clamp(motionRange, 0.1f, 512.0f);
    GenshinFgPreview::Snapshot image;
    const char* status = nullptr;
    if (!GenshinFgPreview::Read(image, status)) {
        MenuText::TextWrapped("等待 FG 输入：%s", status ? status : "pending");
        return;
    }
    MenuText::TextWrapped("FG 接收图状态：%s", status ? status : "pending");
    const auto age = image.ageMs;
    MenuText::TextWrapped("最近接收：游戏帧 %llu / FG 帧 %llu，源画面 %ux%u，%llu ms 前；本帧派发%s。",
        (unsigned long long) image.stamp.sourceFrame, (unsigned long long) image.stamp.providerFrame,
        image.width, image.height, (unsigned long long) age, image.stamp.submitted ? "已接受" : "未接受");
    MenuText::TextWrapped("%s：区域 %u/%u，源坐标 (%u,%u)，%ux%u；复制 %.1f KiB。区域图只检查空间和数值，不证明动态历史正确。",
        image.fullFrame ? "完整小画面" : "局部同帧采样",image.region.index+1,image.region.count,
        image.region.x,image.region.y,image.region.width,image.region.height,image.readbackBytes/1024.0);
    if (image.stale) MenuText::TextWrapped("最近未收到新的 FG 输入；下方保留上次区域快照。");
    MenuText::TextWrapped("深度 R32_FLOAT：Z %.7g .. %.7g，有效 %u/%u。运动 RG16_FLOAT：最大 %.3f 像素，无效 %u/%u，近零 %u/%u。",
        image.minDepth, image.maxDepth, image.validDepth, GenshinFgPreview::N,
        image.maxMotion, GenshinFgPreview::N-image.validMotion, GenshinFgPreview::N,
        image.zeroMotion, GenshinFgPreview::N);
    const float available = ImGui::GetContentRegionAvail().x;
    const bool sideBySide = available >= 600.0f;
    const float width = (std::min)(400.0f, sideBySide ? (available-ImGui::GetStyle().ItemSpacing.x)*0.5f : available);
    if (width <= 0) return;
    const float height = image.region.width ? width*image.region.height/image.region.width : 0.0f;
    bool drew=false;
    const auto draw = [&](bool motion) {
        ImGui::BeginGroup();
        MenuText::TextUnformatted(motion ? "FG 接收：区域运动矢量（像素）" : "FG 接收：区域深度（原始 Z）");
        const auto origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(motion ? "##fgReceivedMotion" : "##fgReceivedDepth", ImVec2(width,height));
        if (!ImGui::IsItemVisible()) { ImGui::EndGroup(); return; }
        drew=true;
        // CPU-only bounded caches. Rebuild colour/run data only for a fresh
        // snapshot or display-control change; numeric samples remain untouched.
        static GenshinFgPreviewRuns caches[2];
        auto& cache=caches[motion ? 1 : 0];
        cache.Update(image,motion,depthContrast,motionRange);
        auto* list = ImGui::GetWindowDrawList();
        list->PushClipRect(origin, ImVec2(origin.x+width,origin.y+height), true);
        // One reservation per visible image, stock PrimRect geometry per same-
        // colour horizontal run. PrimReserve preserves ImGui's VtxOffset path.
        list->PrimReserve(int(cache.count*6),int(cache.count*4));
        for (unsigned i=0;i<cache.count;++i) {
            const auto& run=cache.runs[i];
            list->PrimRect(ImVec2(origin.x+width*run.first/GenshinFgPreview::W,origin.y+height*run.y/GenshinFgPreview::H),
                ImVec2(origin.x+width*run.end/GenshinFgPreview::W,origin.y+height*(run.y+1)/GenshinFgPreview::H),run.colour);
        }
        list->PopClipRect();
        if (ImGui::IsItemHovered()) {
            const auto mouse=ImGui::GetMousePos();
            const auto x=(std::min)(GenshinFgPreview::W-1,unsigned((std::max)(0.0f,(mouse.x-origin.x)/width)*GenshinFgPreview::W));
            const auto y=(std::min)(GenshinFgPreview::H-1,unsigned((std::max)(0.0f,(mouse.y-origin.y)/height)*GenshinFgPreview::H));
            const auto i=y*GenshinFgPreview::W+x;
            const auto sx=image.region.x+unsigned((uint64_t(2*x+1)*image.region.width)/(2*GenshinFgPreview::W));
            const auto sy=image.region.y+unsigned((uint64_t(2*y+1)*image.region.height)/(2*GenshinFgPreview::H));
            MenuText::SetTooltip("源像素 (%u,%u)\nZ=%.9g  MV=(%.6g, %.6g) px",sx,sy,image.depth[i],image.motion[i][0],image.motion[i][1]);
        }
        ImGui::EndGroup();
    };
    draw(false);
    if (sideBySide) ImGui::SameLine();
    draw(true);
    MenuText::TextWrapped("运动颜色：R=X，G=Y（0.5 为零），B=长度；65504 哨兵为紫色。深度对比度只影响显示，不线性化 Z，不翻转方向。");
    if (drew) GenshinFgPreview::Trace(GenshinFgPreview::TraceStage::Draw,"visible-accepted-tag-snapshot",
        image.stamp.submitted?1:0,image.stamp);
}

// Compare and Debug.
static void RenderDebugPage(Config* config, float menuResScale)
{
    ImGui::PushItemWidth(220.0f * menuResScale);

    // Keep the FG control above the long manual-capture panel and the NR-only
    // debug controls. It is a core Debug control, independent of NR enablement.
    if constexpr (F6InputPolicy::FsrBridgeEnabled)
    {
        RenderGenshinFgGuidePreview();
        RenderGenshinManualCapture();
    }
    RenderDetectedColourFormat();

    MenuText::SeparatorText("对比");

    // Freeze the frame the model works on, so a setting change re-renders it in place -- the only
    // clean way to A/B our own settings (a moving scene confounds every other comparison). See
    // design/frame-hold.md.
    bool held = config->DlssNrHoldFrame.value_or_default();
    if (MenuText::Checkbox("冻结输入帧", &held))
        config->DlssNrHoldFrame = held;

    HelpMarker("冻结 NR 输入以对比设置。HUD 可能继续更新，超分或光线重建变化不会显示。");

    if (MenuPages::Show(MenuPages::Category::Nonessential))
    {
        bool frameStats = config->DlssNrFrameStats.value_or_default();
        if (MenuText::Checkbox("记录画面亮度统计", &frameStats))
            config->DlssNrFrameStats = frameStats;

        HelpMarker("约每 2 秒将亮度统计写入 OptiScaler.log，用于诊断。");

        NrMenuBreadcrumb("debug.measure.begin");
        RenderMeasureDetail();
        NrMenuBreadcrumb("debug.measure.end");

        bool kernelProfile = config->DlssNrKernelProfile.value_or_default();
        if (MenuText::Checkbox("记录 NR 内核耗时", &kernelProfile))
            config->DlssNrKernelProfile = kernelProfile;

        HelpMarker("约每 4 秒将 NR GPU 耗时分布写入 OptiScaler.log，用于诊断。");

    }
    NrMenuBreadcrumb("debug.compare.begin");
    static const char* compareNames[] = { "关闭", "并排", "划线" };
    int compare = (int) config->DlssNrCompare.value_or_default();
    if (MenuText::Segmented("对比", &compare, compareNames, IM_ARRAYSIZE(compareNames)))
        config->DlssNrCompare = (uint32_t) compare;

    HelpMarker("比较原画与 NR 结果。并排模式容纳两张图；划线模式分隔一张全尺寸画面。");

    if (compare != 0)
    {
        bool swap = config->DlssNrCompareSwap.value_or_default();
        if (MenuText::Checkbox("交换左右", &swap))
            config->DlssNrCompareSwap = swap;
        HelpMarker("交换原画与 NR 的左右位置。");

        bool tags = config->DlssNrCompareTags.value_or_default();
        if (MenuText::Checkbox("显示两侧标签", &tags))
            config->DlssNrCompareTags = tags;

        HelpMarker("显示原画和 NR 的识别标签。");

        if (tags)
        {
            float tagScale = config->DlssNrTagScale.value_or_default();
            if (MenuText::SliderFloat("标签大小", &tagScale, 0.5f, 5.0f, "%.1fx"))
                config->DlssNrTagScale = std::clamp(tagScale, 0.5f, 5.0f);
        }

    }

    if (compare == 1)
    {
        float zoom = config->DlssNrCompareZoom.value_or_default();
        if (MenuText::SliderFloat("缩放", &zoom, 1.0f, 2.0f, "%.2f"))
            config->DlssNrCompareZoom = std::clamp(zoom, 1.0f, 2.0f);

        HelpMarker("仅并排模式使用。1 为显示完整画面，2 为填满两侧并裁剪边缘。");
    }

    if (compare == 2)
    {
        float split = config->DlssNrCompareSplit.value_or_default();
        if (MenuText::SliderFloat("分割位置", &split, 0.0f, 1.0f, "%.2f"))
            config->DlssNrCompareSplit = std::clamp(split, 0.0f, 1.0f);

        HelpMarker("分割线的位置。");
    }

    NrMenuBreadcrumb("debug.view.begin");
    static const char* debugNames[] = { "关闭", "代理输入（模型所见）", "模型原始输出",
                                        "放大的差异" };
    int debugView = (int) config->DlssNrDebugView.value_or_default();
    if (MenuText::Segmented(GenshinBridgeMenuProcess() ? "NR 调试视图" : "调试视图", &debugView, debugNames,
                            IM_ARRAYSIZE(debugNames)))
        config->DlssNrDebugView = (uint32_t) debugView;

    HelpMarker(GenshinBridgeMenuProcess() ? "仅切换 NR 模型画面，需要 NR 实际执行。FG 接收的深度/运动请使用本页顶部的独立开关；灰色表示 NR 无差异。" : "显示模型输入、原始输出或放大 20 倍的差异；灰色表示无变化。");

    GuideDiagnosticsMenu::RenderPreviewStrip(config);

    // Independent, process-local raw preview demand on the ordinary Compare/Debug page.
    if constexpr (F6InputPolicy::FsrBridgeEnabled)
        GenshinDepthProvider::DrawPreview();

    ImGui::PopItemWidth();
}

static HeaderBanner::Inputs HeaderInputs(Config* config, HeaderBanner::Feature feature, bool upscalerFiles)
{
    using namespace HeaderBanner;

    const auto& state = State::Instance();
    const NativeApi api = CurrentNativeApi();

    Inputs in;
    in.feature = feature;
    in.upscalerFiles = upscalerFiles;
    in.gameCallsUpscaler = NativeGameCallsUpscaler(api);
    in.mode = DlssNrNativeMode::FromKeys(
        { config->DlssNrNativeDepthFinder.value_or_default(), config->DlssNrNativeMotion.value_or_default(),
          config->DlssNrNativeInput.value_or_default(), config->DlssNrNativeUpscaler.value_or_default(),
          config->DlssNrNativeFrameGenerationOnly.value_or_default() });
    // Optical F5Low is for D3D11, D3D12 and Vulkan games, and only helps where NR is switched on and has not failed
    // this session.
    in.nrAvailable = config->DlssNrEnabled.value_or_default() && FailureReason()[0] == 0;
    in.f5lowNrOnlyRunning = NativeNrOnlyRunning(api);
    in.nrEnabled = config->DlssNrEnabled.value_or_default() && FailureReason()[0] == 0;
    in.frameGeneration = state.currentFG != nullptr && state.currentFG->IsActive() && !state.currentFG->IsPaused() &&
        (state.externalFrameGeneration || FGHooks::SwapchainHookStatus() == S_OK);
    return in;
}

bool RenderHeaderBanner(Config* config, HeaderBanner::Feature feature, bool upscalerFiles,
                        const std::string& upscalerNames, const std::string& backendName, const ImVec4& offerColour,
                        void (*upscalerFileChecks)())
{
    using namespace HeaderBanner;

    const NativeApi api = CurrentNativeApi();
    const Banner banner = Decide(HeaderInputs(config, feature, upscalerFiles));

    const auto hookStatus = FGHooks::SwapchainHookStatus();
    if (!State::Instance().externalFrameGeneration && config->FGEnabled.value_or_default() &&
        hookStatus != S_OK && hookStatus != HRESULT_FROM_WIN32(ERROR_NOT_READY))
        MenuText::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
            "FG 接入失败（0x%08X），需要重启后重试；NR 可继续使用。", static_cast<unsigned>(hookStatus));

    switch (banner.line)
    {
    case Line::Blank:
    case Line::Frozen:
    case Line::SelectUpscaler:
    case Line::NoFiles:
        return false; // the menu's own lines
    case Line::OfferWithFiles:
        MenuText::TextColored(
            offerColour,
            "F6 当前未接入游戏超分调用。已启用 %s 时请检查输入接入，或使用 Optical F5Low。",
            upscalerNames.c_str());
        break;
    case Line::OfferNoFiles:
        MenuText::TextColored(offerColour, "F6 尚未检测到可用的超分输入，Optical F5Low 仍可独立运行 NR。");
        break;
    case Line::F5LowNrAndFrameGen:
        MenuText::TextDisabled("Optical F5Low：对最终画面运行 NR 与插帧（%s 稳定画面）。",
                            backendName.c_str());
        break;
    case Line::F5LowNrNoFrameGen:
        MenuText::TextDisabled("Optical F5Low：对最终画面运行 NR（%s 稳定画面），插帧关闭。",
                            backendName.c_str());
        break;
    case Line::F5LowFrameGenNoNr:
        MenuText::TextDisabled("Optical F5Low：对最终画面插帧（%s 稳定画面），NR 关闭。",
                            backendName.c_str());
        break;
    case Line::F5LowStabiliser:
        MenuText::TextDisabled("Optical F5Low：%s 稳定画面，NR 与插帧均关闭。",
                            backendName.c_str());
        break;
    case Line::F5LowNrOnly:
        MenuText::TextDisabled("Optical F5Low：对最终画面运行 NR。");
        break;
    case Line::F5LowStandsAside:
        MenuText::TextDisabled("游戏已调用超分接口，Optical F5Low 让出控制。");
        break;
    case Line::F5LowStatus:
        MenuText::TextDisabled("Optical F5Low：");
        ImGui::SameLine();
        NativeMotionStatus(api);
        break;
    }

    if (banner.line == Line::OfferWithFiles || banner.line == Line::OfferNoFiles)
    {
        // File checks stay lazy and attach to the offer text itself.
        if (MenuText::ItemHelpHovered() && ImGui::BeginTooltip())
        {
            MenuText::TextUnformatted("菜单和加载画面也可能不调用超分接口，因此此状态可在那里出现。\nOptical F5Low 在最终画面上运行 NR，可与游戏超分配合，也可独立运行。");

            if (banner.line == Line::OfferWithFiles && upscalerFileChecks != nullptr)
            {
                ImGui::Spacing();
                upscalerFileChecks();
            }

            ImGui::EndTooltip();
        }
    }

    if (banner.action != Action::None)
    {
        ImGui::SameLine();

        if (MenuText::SmallButton(banner.action == Action::UseF5Low ? "启用 Optical F5Low##headerf5low"
                                                                 : "Optical F5Low 设置##headerf5low"))
            MenuPages::RequestPage(MenuPages::Page::NrF5Low);
    }

    return true;
}

void RenderF5LowUpscalerNote()
{
    MenuText::TextWrapped(
        "由 Optical F5Low 驱动：游戏未调用超分接口，此超分以相同尺寸运行来稳定画面。");

    if (MenuText::SmallButton("Optical F5Low 设置##upscalernote"))
        MenuPages::RequestPage(MenuPages::Page::NrF5Low);

    ImGui::Spacing();
}

void RenderF5LowFrameGenHint(Config* config, bool noUpscalerFeature)
{
    using namespace HeaderBanner;

    // The same offer as the header's: only where Optical F5Low is off, NR can run and the game makes no upscaler call.
    if (!noUpscalerFeature || Decide(HeaderInputs(config, Feature::None, false)).action != Action::UseF5Low)
        return;

    MenuText::TextWrapped("游戏没有调用超分接口？Optical F5Low 的 NR＋超分与插帧模式可提供插帧输入。");

    if (MenuText::SmallButton("启用 Optical F5Low##framegenhint"))
        MenuPages::RequestPage(MenuPages::Page::NrF5Low);

    ImGui::Spacing();
}

void UpdateF5LowHint(Config* config)
{
    // Star Rail input-profile state belongs in its settings page. Do not draw
    // a permanent top-right overlay; profile activation and direction flags are unchanged.

    // Once per session, and never again after it has been shown or once a game upscaler is in play.
    static bool shown = false;
    static double quietMs = 0.0;
    static double lastCallMs = 0.0;

    if (shown || !config->F5LowHint.value_or_default())
        return;

    // Real time between presents (ImGui's DeltaTime stands still while no menu frame is drawn)
    const double now = Util::MillisecondsNow();
    const double stepMs = lastCallMs > 0.0 ? now - lastCallMs : 0.0;
    lastCallMs = now;

    const auto feature = State::Instance().currentFeature;
    const bool noFeature = feature == nullptr || !feature->IsInited();

    // The header's offer: Optical F5Low off, NR available, no upscaler call from the game, and no upscaler running.
    const bool offerStands =
        noFeature && HeaderBanner::Decide(HeaderInputs(config, HeaderBanner::Feature::None, false)).action ==
                         HeaderBanner::Action::UseF5Low;

    // A minute of the game running with the offer standing (a loading stall counts little): see HeaderBanner.h
    quietMs = HeaderBanner::HintQuietAfter(quietMs, offerStands, stepMs);

    if (!HeaderBanner::HintDue(quietMs))
        return;

    shown = true;
    ImGui::InsertNotification({ ImGuiToastType::Info, 15000,
                                "F6 当前未接入游戏超分调用。\n若游戏已启用超分，请检查输入接入。光学 F5Low 也可独立运行 NR：打开菜单，神经渲染，光学 F5Low。" });
}

static void RenderUnsortedPage(Config* config, float menuResScale)
{
    const auto passes = VisiblePassCount(config);
    if (passes >= 4u)
    {
        RenderRemainingPasses(config, menuResScale);
        for (unsigned pass = 3; pass < passes; ++pass)
            RenderLayerLook(config, pass);
    }
}

void RenderMenu(Config* config, float menuResScale, MenuPages::Page page)
{
    using MenuPages::Page;
    if (MenuPages::NavigationPage(page) == Page::ProFilters)
    {
        if (GenshinBridgeMenuProcess()) GenshinFgPreview::Request(false,"debug-page-hidden");
        HoYoShadeControls::Render(config, CurrentNativeApi() == NativeApi::Vulkan,
                                  CurrentNativeApi() == NativeApi::Dx11 &&
                                  State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12);
        return;
    }
    Profiles::ClampConfiguredModelScalars(*config);
    if (GenshinBridgeMenuProcess() && MenuPages::NavigationPage(page)!=Page::Debug)
        GenshinFgPreview::Request(false,"debug-page-hidden");

    static int previousFrame = -1;
    const int frame = ImGui::GetFrameCount();
    nrMenuTraceActive = false;
    if (nrMenuTraceFramesRemaining > 0 &&
        (previousFrame < 0 || frame != previousFrame + 1 || nrMenuTracePage != page ||
         ImGui::IsMouseClicked(ImGuiMouseButton_Left)))
    {
        --nrMenuTraceFramesRemaining;
        nrMenuTraceActive = true;
    }
    previousFrame = frame;
    nrMenuTracePage = page;

    if (MenuPages::NavigationPage(page) == Page::NrStatus)
        RenderEnableToggle(config);
    NrMenuBreadcrumb("common.begin");
    const NrCommon nr = ReadNrCommon(config);
    NrMenuBreadcrumb("common.end");

    switch (MenuPages::NavigationPage(page))
    {
    case Page::NrStatus:
        RenderRunningStatus(config, nr);
        RenderPassCount(config, menuResScale);
        RenderSharedResolution(config, menuResScale, nr);
        RenderOutputPage(config, menuResScale, nr);
        RenderOptionsPage(config, menuResScale, nr);
        RenderColourAndLighting(config, menuResScale);
        RenderGlobalLookPage(config);
        RenderInputExtras(config, menuResScale, nr);
        break;
    case Page::NrReuse:
    {
        const MenuText::ScopedSeparatorOmission heading(NextgenUi::English() ? "Multi-frame NR reuse" : "多帧NR复用");
        RenderReusePage(config, menuResScale);
        break;
    }
    case Page::NrF5Low:
        RenderF5LowPage(config, nr);
        break;
    case Page::NrLayer1:
        RenderFirstLayerParameters(config, menuResScale);
        RenderLayerLook(config, 0u);
        if (MenuPages::Show(MenuPages::Category::Unfinished)) MenuText::SeparatorText("本层细节");
        if (MenuPages::Show(MenuPages::Category::Unfinished)) NextgenNrLayers::UnavailableDetails();
        MenuText::SeparatorText("上采样（所有层共享）");
        MenuText::TextWrapped("以下设置影响所有处理层；模型分辨率在 NR 全局，逐层独立分辨率、放大与 DLSS 尚不可用。");
        RenderSharedUpsampling(config, menuResScale, nr);
        break;
    case Page::NrLayer2:
        if (VisiblePassCount(config) < 2u)
            break;
        RenderLayerLook(config, 1u);
        if (NextgenNrLayers::SecondLayerControls([&] { RenderSecondLayerParameters(config, menuResScale); }))
        {
        }
        if (MenuPages::Show(MenuPages::Category::Unfinished)) MenuText::SeparatorText("本层细节");
        if (MenuPages::Show(MenuPages::Category::Unfinished)) NextgenNrLayers::UnavailableDetails();
        if (MenuPages::Show(MenuPages::Category::Unfinished))
            MenuText::TextWrapped("待实现：第二层独立分辨率、放大与 DLSS。模型分辨率在 NR 全局，上采样使用第一层页面标明的共享设置。");
        break;
    case Page::NrLayer3:
        if (VisiblePassCount(config) >= 3u)
        {
            RenderThirdLayerParameters(config,menuResScale);
            RenderLayerLook(config, 2u);
        }
        break;
    case Page::NrUnsorted:
        RenderUnsortedPage(config, menuResScale);
        break;
    case Page::Hdr:
    {
        const MenuText::ScopedSeparatorOmission heading("HDR");
        RenderHdrPage(config, menuResScale);
        break;
    }
    case Page::Debug:
        RenderDebugPage(config, menuResScale);
        break;
    default:
        IM_ASSERT(false && "not a Neural Rendering page");
        break;
    }
    nrMenuTraceActive = false;
}

} // namespace DlssNr

bool NextgenNrLayers::IsVisible(unsigned int layer)
{
    const Config* config = Config::Instance();
    return config != nullptr && layer >= 1u && layer <= DlssNr::VisiblePassCount(config);
}
