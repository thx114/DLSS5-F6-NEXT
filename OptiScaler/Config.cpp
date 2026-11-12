#include "pch.h"

#include "Config.h"
#include <mods/gimi/GimiRuntime.h>
#include <ui_mask/DrawUiMask.h>
#include <genshin/GenshinHookCpuTiming.h>
#include <dlssnr/LayerLookConfig.h>

#include "Util.h"

#include "nvapi/fakenvapi.h"
#include <hooks/Streamline_Hooks.h>
#include <misc/IdentifyGpu.h>
#include <shaders/dlssnr/DlssNr_ProxyCurve.h>

#include <SimpleIni.h>

static CSimpleIniA ini;

// R040: retain the values from the existing parser, without rereading the file.
// Only Reload and the startup worker touch this diagnostic; no render-path IO.
namespace HoYoShadeStartupTrace
{
struct Snapshot
{
    std::string path;
    bool loaded = false;
    std::optional<std::string> rawEnabled, rawStage;
    std::optional<bool> parsedEnabled;
    std::optional<int> parsedStage;
};
static Snapshot snapshot;
static std::mutex mutex;

static std::string JsonString(const std::optional<std::string>& value)
{
    if (!value) return "null";
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : *value)
    {
        if (c == '"' || c == '\\') { result += '\\'; result += c; }
        else if (c < 0x20 || c >= 0x7f)
        {
            // Byte escapes also keep malformed INI text on one parseable log line.
            result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15];
        }
        else result += c;
    }
    return result + '"';
}
} // namespace HoYoShadeStartupTrace

// Private cross-TU entrypoint; called once after Initialize on the existing worker.
// These are Initialize's latched inputs, not evidence of core/pixel execution.
void LogHoYoShadeStartupConfig(bool enabled, unsigned stage) noexcept
{
    static std::atomic<bool> emitted { false };
    if (emitted.exchange(true)) return;
    try
    {
        using namespace HoYoShadeStartupTrace;
        Snapshot captured;
        { std::lock_guard lock(mutex); captured = snapshot; }
        FILETIME created {}, exited {}, kernel {}, user {};
        const bool haveStart = GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE;
        const uint64_t start = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        std::ostringstream line;
        line << std::boolalpha
             << "{\"schema\":1,\"pid\":" << GetCurrentProcessId()
             << ",\"processStartFileTime\":" << JsonString(haveStart ? std::optional(std::to_string(start)) : std::nullopt)
             << ",\"iniPath\":" << JsonString(captured.path)
             << ",\"loaded\":" << captured.loaded
             << ",\"rawEnabled\":" << JsonString(captured.rawEnabled)
             << ",\"rawStage\":" << JsonString(captured.rawStage)
             << ",\"parsedEnabled\":" << (captured.parsedEnabled ? (*captured.parsedEnabled ? "true" : "false") : "null")
             << ",\"parsedStage\":" << (captured.parsedStage ? std::to_string(*captured.parsedStage) : "null")
             << ",\"latchedEnabled\":" << enabled
             << ",\"latchedStage\":" << stage
             << ",\"iniSHA256\":null,\"hashSource\":\"harness-disk-snapshot-only\""
                ",\"rawEncoding\":\"byte-escaped\",\"configLatchedUntilRestart\":true,\"pixels\":\"UNKNOWN\"}";
        LOG_INFO("R040 HoYoShade startup {}", line.str());
    }
    catch (...) { LOG_WARN("R040 HoYoShade startup diagnostic unavailable pid={}", GetCurrentProcessId()); }
}

static inline int64_t GetTicks()
{
    LARGE_INTEGER ticks;

    if (!QueryPerformanceCounter(&ticks))
        return 0;

    return ticks.QuadPart;
}

static inline bool isInteger(const std::string& str, int& value)
{
    std::istringstream iss(str);
    return (iss >> value) && iss.eof();
}

static inline bool isUInt(const std::string& str, uint32_t& value)
{
    std::istringstream iss(str);
    return (iss >> value) && iss.eof();
}

static inline bool isFloat(const std::string& str, float& value)
{
    std::istringstream iss(str);
    return (iss >> value) && iss.eof();
}

Config::Config()
{
    absoluteFileName = Util::DllPath().parent_path() / fileName;
    _starRailInputProfile = _wcsicmp(Util::ExePath().filename().c_str(), L"StarRail.exe") == 0;
    if (!Reload(absoluteFileName))
        ReloadStarRailInputProfile(absoluteFileName);
    if (!savedSettings_) CaptureSavedSettings();
}

Config::Config(MemoryOnly)
{
    CaptureSavedSettings();
}

bool Config::Reload(std::filesystem::path iniPath)
{
    {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(iniPath, error);
        std::lock_guard lock(HoYoShadeStartupTrace::mutex);
        HoYoShadeStartupTrace::snapshot = {};
        HoYoShadeStartupTrace::snapshot.path = wstring_to_string((error ? iniPath : absolute).wstring());
    }
    auto pathWStr = iniPath.wstring();

    LOG_INFO("Trying to load ini from: {0}", wstring_to_string(pathWStr));
    if (ini.LoadFile(iniPath.c_str()) == SI_OK)
    {
        // Reload invalidates the display/comparison cache, never the saved baseline.
        // The next poll still samples every persisted value through the one schema.
        dirtySampleKnown_ = false;
        dirtyComparisonKnown_ = false;
        State::Instance().nvngxIniDetected = exists(iniPath.parent_path() / "nvngx.ini");
        _log.clear();

        // Upscalers
        {
            // transform converts only when optional has a value
            Dx11Upscaler.set_from_config(readString("Upscalers", "Dx11Upscaler", true).transform(CodeToUpscaler));
            Dx12Upscaler.set_from_config(readString("Upscalers", "Dx12Upscaler", true).transform(CodeToUpscalerFfx));
            VulkanUpscaler.set_from_config(
                readString("Upscalers", "VulkanUpscaler", true).transform(CodeToUpscalerFfx));
        }

        // Frame Generation
        {
            FGEnabled.set_from_config(readBool("FrameGen", "Enabled"));
            StarRailForceFGResident.set_from_config(readBool("FrameGen", "StarRailForceResident"));
            ExternalFrameGeneration.set_from_config(readBool("FrameGen", "External"));
            FGDLSSGAdaMfgUnlock.set_from_config(readBool("DLSSG", "AdaMfgUnlock"));
            FGDLSSGAdaBlackwellKernels.set_from_config(readBool("DLSSG", "AdaBlackwellKernels"));

            if (auto adaFix = readString("DLSSG", "AdaTemporalFix"); adaFix.has_value())
            {
                if (lstrcmpiA(adaFix.value().c_str(), "retarget") == 0)
                    FGDLSSGAdaTemporalFix.set_from_config("Retarget");
                else if (lstrcmpiA(adaFix.value().c_str(), "ptx") == 0)
                    FGDLSSGAdaTemporalFix.set_from_config("Ptx");
                else
                    FGDLSSGAdaTemporalFix.set_from_config("Auto");
            }

            FGDLSSGAdaFlipMeteringPatch.set_from_config(readBool("DLSSG", "AdaFlipMeteringPatch"));
            FGDLSSGAmpereMfgUnlock.set_from_config(readBool("DLSSG", "AmpereMfgUnlock"));
            FGDebugView.set_from_config(readBool("FrameGen", "DebugView"));

            if (auto FGInputString = readString("FrameGen", "FGInput"); FGInputString.has_value())
            {
                if (lstrcmpiA(FGInputString.value().c_str(), "nofg") == 0)
                    FGInput.set_from_config(FGInput::NoFG);
                else if (lstrcmpiA(FGInputString.value().c_str(), "upscaler") == 0)
                    FGInput.set_from_config(FGInput::Upscaler);
                else if (lstrcmpiA(FGInputString.value().c_str(), "nvngxfg") == 0)
                    FGInput.set_from_config(FGInput::NvngxFG);
                else if (lstrcmpiA(FGInputString.value().c_str(), "dlssg") == 0)
                    FGInput.set_from_config(FGInput::DLSSG);
                else if (lstrcmpiA(FGInputString.value().c_str(), "fsrfg") == 0)
                    FGInput.set_from_config(FGInput::FSRFG);
                else if (lstrcmpiA(FGInputString.value().c_str(), "fsrfg30") == 0)
                    FGInput.set_from_config(FGInput::FSRFG30);

                if (lstrcmpiA(FGInputString.value().c_str(), "nukems") == 0)
                {
                    FGInput.set_from_config(FGInput::NvngxFG);
                    ini.SetValue("FrameGen", "FGNvngxReplacement", "nukems");
                }
            }

            if (auto FGOutputString = readString("FrameGen", "FGOutput");
                FGInput.value_or_default() != FGInput::NvngxFG && FGOutputString.has_value())
            {
                if (lstrcmpiA(FGOutputString.value().c_str(), "nofg") == 0)
                    FGOutput.set_from_config(FGOutput::NoFG);
                else if (lstrcmpiA(FGOutputString.value().c_str(), "fsrfg") == 0)
                    FGOutput.set_from_config(FGOutput::FSRFG);
                else if (lstrcmpiA(FGOutputString.value().c_str(), "xefg") == 0)
                    FGOutput.set_from_config(FGOutput::XeFG);
                else if (lstrcmpiA(FGOutputString.value().c_str(), "dlssg") == 0)
                    FGOutput.set_from_config(FGOutput::DLSSG);
            }

            FGStreamlineIgnoreOTA.set_from_config(readBool("FrameGen", "StreamlineIgnoreOTA"));

            const bool canUseNvngxReplacement =
                FGInput.value_or_default() == FGInput::NvngxFG || FGOutput.value_or_default() == FGOutput::DLSSG;

            if (auto FGNvngxReplacementString = readString("FrameGen", "FGNvngxReplacement");
                canUseNvngxReplacement && FGNvngxReplacementString.has_value())
            {
                if (lstrcmpiA(FGNvngxReplacementString.value().c_str(), "none") == 0)
                    FGNvngxReplacement.set_from_config(FGNvngxReplacement::None);
                else if (lstrcmpiA(FGNvngxReplacementString.value().c_str(), "nukems") == 0)
                    FGNvngxReplacement.set_from_config(FGNvngxReplacement::Nukems);
                else if (lstrcmpiA(FGNvngxReplacementString.value().c_str(), "arturs") == 0)
                    FGNvngxReplacement.set_from_config(FGNvngxReplacement::Arturs);
                else if (lstrcmpiA(FGNvngxReplacementString.value().c_str(), "ffx") == 0)
                    FGNvngxReplacement.set_from_config(FGNvngxReplacement::FFX);
                else if (lstrcmpiA(FGNvngxReplacementString.value().c_str(), "combo") == 0)
                    FGNvngxReplacement.set_from_config(FGNvngxReplacement::Combo);
            }

            if (auto forceXell = readBool("fakenvapi", "ForceXeLL"); forceXell.has_value() && forceXell.value())
            {
                FGInput.set_volatile_value(FGInput::ForceXeLL);
                FGOutput.set_volatile_value(FGOutput::XeFG);
            }

            auto ftInput = readInt("FrameGen", "FTInput");
            if (ftInput.has_value() && ftInput.value() >= 0 &&
                ftInput.value() <= (FGOutput.value_or_default() == FGOutput::XeFG ? 2 : 1))
            {
                FTInput.set_from_config(static_cast<FrameTimeSource>(ftInput.value()));
            }

            FGDrawUIOverFG.set_from_config(readBool("FrameGen", "DrawUIOverFG"));
            FGUIPremultipliedAlpha.set_from_config(readBool("FrameGen", "UIPremultipliedAlpha"));
            FGDisableHudless.set_from_config(readBool("FrameGen", "DisableHudless"));
            FGDisableUI.set_from_config(readBool("FrameGen", "DisableUI"));
            FGSkipReset.set_from_config(readBool("FrameGen", "SkipReset"));
            FGRectLeft.set_from_config(readInt("FrameGen", "RectLeft"));
            FGRectTop.set_from_config(readInt("FrameGen", "RectTop"));
            FGRectWidth.set_from_config(readInt("FrameGen", "RectWidth"));
            FGRectHeight.set_from_config(readInt("FrameGen", "RectHeight"));

            FGAllowedFrameAhead.set_from_config(readInt("FrameGen", "AllowedFrameAhead"));
            if (FGAllowedFrameAhead.has_value() && (FGAllowedFrameAhead.value() < 1 || FGAllowedFrameAhead.value() > 3))
                FGAllowedFrameAhead.reset();

            FGDepthValidNow.set_from_config(readBool("FrameGen", "DepthValidNow"));
            FGVelocityValidNow.set_from_config(readBool("FrameGen", "VelocityValidNow"));
            FGHudlessValidNow.set_from_config(readBool("FrameGen", "HudlessValidNow"));
            FGOnlyAcceptFirstHudless.set_from_config(readBool("FrameGen", "OnlyAcceptFirstHudless"));
            FGPreserveSwapChain.set_from_config(readBool("FrameGen", "PreserveSwapChain"));
            FGSkipResizeBuffers.set_from_config(readBool("FrameGen", "SkipResizeBuffers"));
            FGModifyBufferState.set_from_config(readBool("FrameGen", "ModifyBufferState"));
            FGModifySCIndex.set_from_config(readBool("FrameGen", "ModifySCIndex"));
            FGHudCutoff.set_from_config(readFloat("FrameGen", "HudCutoff"));
        }

        // FSR FG
        {
            FGDebugTearLines.set_from_config(readBool("FSRFG", "DebugTearLines"));
            FGDebugResetLines.set_from_config(readBool("FSRFG", "DebugResetLines"));
            FGDebugPacingLines.set_from_config(readBool("FSRFG", "DebugPacingLines"));
            FGAsync.set_from_config(readBool("FSRFG", "AllowAsync"));
            FGUseMutexForSwapchain.set_from_config(readBool("FSRFG", "UseMutexForSwapchain"));
            FGFramePacingTuning.set_from_config(readBool("FSRFG", "FramePacingTuning"));
            FGFPTSafetyMarginInMs.set_from_config(readFloat("FSRFG", "FPTSafetyMarginInMs"));
            FGFPTVarianceFactor.set_from_config(readFloat("FSRFG", "FPTVarianceFactor"));
            FGFPTAllowHybridSpin.set_from_config(readBool("FSRFG", "FPTHybridSpin"));
            FGFPTHybridSpinTime.set_from_config(readInt("FSRFG", "FPTHybridSpinTime"));
            FGFPTAllowWaitForSingleObjectOnFence.set_from_config(readBool("FSRFG", "FPTWaitForSingleObjectOnFence"));
            FSRFGEnableWatermark.set_from_config(readBool("FSRFG", "EnableWatermark"));
        }

        // OptiFG
        {
            FGDisableHUDFix.set_from_config(readBool("OptiFG", "DisableHUDFix"));
            FGHUDFix.set_from_config(readBool("OptiFG", "HUDFix"));
            FGHUDLimit.set_from_config(readInt("OptiFG", "HUDLimit"));
            FGHUDFixExtended.set_from_config(readBool("OptiFG", "HUDFixExtended"));
            FGImmediateCapture.set_from_config(readBool("OptiFG", "HUDFixImmediate"));
            FGHudfixPersistentBindings.set_from_config(readBool("OptiFG", "HUDFixPersistentBindings"));
            FGUseShards.set_from_config(readBool("OptiFG", "UseShards"));
            FGAlwaysTrackHeaps.set_from_config(readBool("OptiFG", "AlwaysTrackHeaps"));
            FGResourceBlocking.set_from_config(readBool("OptiFG", "ResourceBlocking"));
            FGMakeDepthCopy.set_from_config(readBool("OptiFG", "MakeDepthCopy"));
            FGMakeMVCopy.set_from_config(readBool("OptiFG", "MakeMVCopy"));
            FGHudfixDisableRTV.set_from_config(readBool("OptiFG", "HudfixDisableRTV"));
            FGHudfixDisableSRV.set_from_config(readBool("OptiFG", "HudfixDisableSRV"));
            FGHudfixDisableUAV.set_from_config(readBool("OptiFG", "HudfixDisableUAV"));
            FGHudfixDisableOM.set_from_config(readBool("OptiFG", "HudfixDisableOM"));
            FGHudfixDisableDispatch.set_from_config(readBool("OptiFG", "HudfixDisableDispatch"));
            FGHudfixDisableDI.set_from_config(readBool("OptiFG", "HudfixDisableDI"));
            FGHudfixDisableDII.set_from_config(readBool("OptiFG", "HudfixDisableDII"));
            FGHudfixDisableSCR.set_from_config(readBool("OptiFG", "HudfixDisableSCR"));
            FGHudfixDisableSGR.set_from_config(readBool("OptiFG", "HudfixDisableSGR"));

            FGEnableDepthScale.set_from_config(readBool("OptiFG", "EnableDepthScale"));
            FGDepthScaleMax.set_from_config(readFloat("OptiFG", "DepthScaleMax"));

            FGDontUseSwapchainBuffers.set_from_config(readBool("OptiFG", "HUDFixDontUseSwapchainBuffers"));
            FGRelaxedResolutionCheck.set_from_config(readBool("OptiFG", "HUDFixRelaxedResolutionCheck"));

            FGResourceFlip.set_from_config(readBool("OptiFG", "ResourceFlip"));
            FGResourceFlipOffset.set_from_config(readBool("OptiFG", "ResourceFlipOffset"));
            FGDepthTextureFlipY.set_from_config(readBool("OptiFG", "DepthTextureFlipY"));
            FGMotionTextureFlipY.set_from_config(readBool("OptiFG", "MotionTextureFlipY"));
            FGMotionYInverted.set_from_config(readBool("OptiFG", "MotionYInverted"));

            FGAlwaysCaptureFSRFGSwapchain.set_from_config(readBool("OptiFG", "AlwaysCaptureFSRFGSwapchain"));
        }

        {
            FGXeFGInterpolationCount.set_from_config(readInt("XeFG", "InterpolationCount"));
            if (FGXeFGInterpolationCount.has_value() && FGXeFGInterpolationCount.value() < 1)
                FGXeFGInterpolationCount.reset();

            FGXeFGIgnoreInitChecks.set_from_config(readBool("XeFG", "IgnoreInitChecks"));
            FGXeFGUIComposition.set_from_config(readBool("XeFG", "UIComposition"));
            FGXeFGDepthInverted.set_from_config(readBool("XeFG", "DepthInverted"));
            FGXeFGJitteredMV.set_from_config(readBool("XeFG", "JitteredMV"));
            FGXeFGHighResMV.set_from_config(readBool("XeFG", "HighResMV"));
            FGXeFGDebugView.set_from_config(readBool("XeFG", "DebugView"));
            FGXeFGForceBorderless.set_from_config(readBool("XeFG", "ForceBorderless"));
        }

        {
            FGDLSSGInterpolationCount.set_from_config(readInt("DLSSG", "InterpolationCount"));
            if (FGDLSSGInterpolationCount.has_value() &&
                (FGDLSSGInterpolationCount.value() < 1 || FGDLSSGInterpolationCount.value() > 6))
                FGDLSSGInterpolationCount.reset();

            FGDLSSGModelPreset.set_from_config(readUInt("DLSSG", "ModelPreset"));
            if (FGDLSSGModelPreset.has_value() && FGDLSSGModelPreset.value() > 3)
                FGDLSSGModelPreset.reset();

            FGDLSSGUseGamesReflexMarkers.set_from_config(readBool("DLSSG", "UseGamesReflexMarkers"));

            FGDLSSGOverrideInterpolationCount.set_from_config(readInt("DLSSG", "OverrideInterpolationCount"));
            if (FGDLSSGOverrideInterpolationCount.has_value() &&
                (FGDLSSGOverrideInterpolationCount.value() < 0 || FGDLSSGOverrideInterpolationCount.value() > 6))
                FGDLSSGOverrideInterpolationCount.reset();

            FGDLSSGFramerateTargetDMFG.set_from_config(readFloat("DLSSG", "FramerateTargetDMFG"));
            FGDLSSGOverrideForceDMFG.set_from_config(readBool("DLSSG", "OverrideForceDMFG"));
            FGDLSSGForceDMFG.set_from_config(readBool("DLSSG", "ForceDMFG"));
        }

        // FSR FG Inputs
        {
            FSRFGSkipConfigForHudless.set_from_config(readBool("FSRFGInputs", "SkipConfigForHudless"));
            FSRFGSkipDispatchForHudless.set_from_config(readBool("FSRFGInputs", "SkipDispatchForHudless"));
        }

        // Framerate
        {
            FramerateLimit.set_from_config(readFloat("Framerate", "FramerateLimit"));
        }

        // FSR Common
        {
            FsrVerticalFov.set_from_config(readFloat("FSR", "VerticalFov"));
            FsrHorizontalFov.set_from_config(readFloat("FSR", "HorizontalFov"));
            FsrCameraNear.set_from_config(readFloat("FSR", "CameraNear"));
            FsrCameraFar.set_from_config(readFloat("FSR", "CameraFar"));
            FsrUseFsrInputValues.set_from_config(readBool("FSR", "UseFsrInputValues"));
        }

        // FSR
        {
            FsrVelocity.set_from_config(readFloat("FSR", "VelocityFactor"));
            FsrReactiveScale.set_from_config(readFloat("FSR", "ReactiveScale"));
            FsrShadingScale.set_from_config(readFloat("FSR", "ShadingScale"));
            FsrAccAddPerFrame.set_from_config(readFloat("FSR", "AccAddPerFrame"));
            FsrMinDisOccAcc.set_from_config(readFloat("FSR", "MinDisOccAcc"));
            FsrDebugView.set_from_config(readBool("FSR", "DebugView"));
            FfxUpscalerIndex.set_from_config(readInt("FSR", "UpscalerIndex"));
            FfxFGIndex.set_from_config(readInt("FSR", "FGIndex"));
            FsrUseMaskForTransparency.set_from_config(readBool("FSR", "UseReactiveMaskForTransparency"));
            DlssReactiveMaskBias.set_from_config(readFloat("FSR", "DlssReactiveMaskBias"));

            if (auto v = readEnum<FSR4Support>("FSR", "Fsr4ForceModel"))
                Fsr4ForceModel.set_from_config(*v);
            else
                Fsr4ForceModel.reset();

            Fsr4EnableWatermark.set_from_config(readBool("FSR", "Fsr4EnableWatermark"));
            Fsr4DoNotLoadAmdxc64.set_from_config(readBool("FSR", "Fsr4DoNotLoadAmdxc64"));

            if (auto setting = readInt("FSR", "Fsr4Preset"); setting.has_value() && setting >= 0 && setting <= 5)
                Fsr4Preset.set_from_config(setting);

            FsrNonLinearColorSpace.set_from_config(readBool("FSR", "FsrNonLinearColorSpace"));
            FsrNonLinearPQ.set_from_config(readBool("FSR", "FsrNonLinearPQ"));
            FsrNonLinearSRGB.set_from_config(readBool("FSR", "FsrNonLinearSRGB"));
            FsrAgilitySDKUpgrade.set_from_config(readBool("FSR", "FsrAgilitySDKUpgrade"));

            // Only sRGB or PQ should be enabled
            if (FsrNonLinearPQ.has_value() && FsrNonLinearPQ.value())
                FsrNonLinearSRGB.reset();
            else if (FsrNonLinearSRGB.has_value() && FsrNonLinearSRGB.value())
                FsrNonLinearPQ.reset();

            if (FsrNonLinearPQ.has_value() || FsrNonLinearSRGB.has_value())
                FsrNonLinearColorSpace.set_volatile_value(true);
        }

        // XeSS
        {
            BuildPipelines.set_from_config(readBool("XeSS", "BuildPipelines"));
            NetworkModel.set_from_config(readInt("XeSS", "NetworkModel"));
            CreateHeaps.set_from_config(readBool("XeSS", "CreateHeaps"));
        }

        // DLSS
        {
            // Don't enable again if set false because of no nvngx found
            DLSSEnabled.set_from_config(readBool("DLSS", "Enabled"));

            // --- DLSS 5 Neural Rendering (OptiScaler/dlssnr) ---
            DlssNrEnabled.set_from_config(readBool("DlssNr", "Enabled"));
            StarRailForceNRResident.set_from_config(readBool("DlssNr", "StarRailForceResident"));
            DlssNrRunBeforeSr.set_from_config(readBool("DlssNr", "RunBeforeSR"));
            DlssNrFinishedPicture.set_from_config(readBool("DlssNr", "FinishedPicture"));
            DlssNrGenshinPresentPipeline.set_from_config(readBool("DlssNr", "GenshinPresentPipeline"));
            DlssNrDeferredDlss.set_from_config(readBool("DlssNr", "DeferredDLSS"));
            // ResidualFG and ResidualFGApproxCamera are no longer read: NR every second frame through NVIDIA Frame
            // Generation is off for good, whatever an old ini says.
            DlssNrPrecision.set_from_config(readUInt("DlssNr", "Precision"));
            if (DlssNrPrecision.value_or_default() != 4) DlssNrPrecision = 0u;
            DlssNrVitEvery.set_from_config(readUInt("DlssNr", "VitEvery"));
            if (DlssNrVitEvery.value_or_default() < 1u || DlssNrVitEvery.value_or_default() > 2u)
                DlssNrVitEvery = std::clamp<uint32_t>(DlssNrVitEvery.value_or_default(), 1u, 2u);
            DlssNrVitEveryPlain.set_from_config(readUInt("DlssNr", "VitEveryPlain"));
            if (DlssNrVitEveryPlain.value_or_default() < 1u || DlssNrVitEveryPlain.value_or_default() > 2u)
                DlssNrVitEveryPlain = std::clamp<uint32_t>(DlssNrVitEveryPlain.value_or_default(), 1u, 2u);
            DlssNrDetailReuse.set_from_config(readBool("DlssNr", "DetailReuse"));
            DlssNrDetailReuseDebug.set_from_config(readBool("DlssNr", "DetailReuseDebug"));
            DlssNrDetailReuseSteady.set_from_config(readFloat("DlssNr", "DetailReuseSteady"));
            DlssNrPresentedTemporalDetail.set_from_config(readBool("DlssNr", "PresentedTemporalDetail"));
            DlssNrPresentedTemporalDurationMs.set_from_config(readUInt("DlssNr", "PresentedTemporalDurationMs"));
            if (DlssNrPresentedTemporalDurationMs.has_value())
                DlssNrPresentedTemporalDurationMs = std::min(DlssNrPresentedTemporalDurationMs.value(),
                                                            kPresentedTemporalDurationMaxMs);
            DlssNrPresentedTemporalMaxAgeMs.set_from_config(readUInt("DlssNr", "PresentedTemporalMaxAgeMs"));
            if (DlssNrPresentedTemporalMaxAgeMs.has_value())
                DlssNrPresentedTemporalMaxAgeMs = std::clamp(DlssNrPresentedTemporalMaxAgeMs.value(),
                    kPresentedTemporalMaxAgeMinMs, kPresentedTemporalMaxAgeMaxMs);
            DlssNrPresentedTemporalStrength.set_from_config(readFloat("DlssNr", "PresentedTemporalStrength"));
            if (DlssNrPresentedTemporalStrength.has_value())
                DlssNrPresentedTemporalStrength = std::isfinite(DlssNrPresentedTemporalStrength.value())
                    ? std::clamp(DlssNrPresentedTemporalStrength.value(), 0.0f, 0.95f) : 0.85f;
            DlssNrPresentedTemporalDepthTolerance.set_from_config(readFloat("DlssNr", "PresentedTemporalDepthTolerance"));
            if (DlssNrPresentedTemporalDepthTolerance.has_value())
                DlssNrPresentedTemporalDepthTolerance = std::isfinite(DlssNrPresentedTemporalDepthTolerance.value())
                    ? std::clamp(DlssNrPresentedTemporalDepthTolerance.value(), 0.0f, 1.0f) : 0.08f;
            DlssNrPresentedTemporalColourTolerance.set_from_config(readFloat("DlssNr", "PresentedTemporalColourTolerance"));
            if (DlssNrPresentedTemporalColourTolerance.has_value())
                DlssNrPresentedTemporalColourTolerance = std::isfinite(DlssNrPresentedTemporalColourTolerance.value())
                    ? std::clamp(DlssNrPresentedTemporalColourTolerance.value(), 0.005f, 0.5f) : 0.08f;
            DlssNrPresentedTemporalMotionTolerance.set_from_config(readFloat("DlssNr", "PresentedTemporalMotionTolerance"));
            if (DlssNrPresentedTemporalMotionTolerance.has_value())
                DlssNrPresentedTemporalMotionTolerance = std::isfinite(DlssNrPresentedTemporalMotionTolerance.value())
                    ? std::clamp(DlssNrPresentedTemporalMotionTolerance.value(), 0.05f, 32.0f) : 2.0f;
            DlssNrPresentedTemporalNoiseFloor.set_from_config(readFloat("DlssNr", "PresentedTemporalNoiseFloor"));
            if (DlssNrPresentedTemporalNoiseFloor.has_value())
                DlssNrPresentedTemporalNoiseFloor = std::isfinite(DlssNrPresentedTemporalNoiseFloor.value())
                    ? std::clamp(DlssNrPresentedTemporalNoiseFloor.value(), 0.0f, 0.1f) : 0.01f;
            DlssNrPresentedTemporalStabilityMs.set_from_config(readUInt("DlssNr", "PresentedTemporalStabilityMs"));
            if (DlssNrPresentedTemporalStabilityMs.has_value())
                DlssNrPresentedTemporalStabilityMs = std::clamp(DlssNrPresentedTemporalStabilityMs.value(), 0u, 2000u);
            DlssNrPresentedTemporalStabilityFloor.set_from_config(readFloat("DlssNr", "PresentedTemporalStabilityFloor"));
            if (DlssNrPresentedTemporalStabilityFloor.has_value())
                DlssNrPresentedTemporalStabilityFloor = std::isfinite(DlssNrPresentedTemporalStabilityFloor.value())
                    ? std::clamp(DlssNrPresentedTemporalStabilityFloor.value(), 0.0f, 1.0f) : 0.65f;
            DlssNrPresentedTemporalStabilityClamp.set_from_config(readFloat("DlssNr", "PresentedTemporalStabilityClamp"));
            if (DlssNrPresentedTemporalStabilityClamp.has_value())
                DlssNrPresentedTemporalStabilityClamp = std::isfinite(DlssNrPresentedTemporalStabilityClamp.value())
                    ? std::clamp(DlssNrPresentedTemporalStabilityClamp.value(), 0.001f, 0.5f) : 0.08f;
            DlssNrMotionYInverted.set_from_config(readBool("DlssNr", "MotionYInverted"));
            DlssNrDepthTextureFlipY.set_from_config(readBool("DlssNr", "DepthTextureFlipY"));
            DlssNrMotionTextureFlipY.set_from_config(readBool("DlssNr", "MotionTextureFlipY"));
            DlssNrGameMotionReuse.set_from_config(readBool("DlssNr", "GameMotionReuse"));
            DlssNrDetailReuseInterval.set_from_config(readUInt("DlssNr", "DetailReuseInterval"));
            if (DlssNrDetailReuseInterval.has_value())
            {
                DlssNrDetailReuseInterval = std::clamp(DlssNrDetailReuseInterval.value(), 1u, 8u);
            }
            DlssNrDetailReuseFgInterval.set_from_config(readUInt("DlssNr", "DetailReuseFgInterval"));
            if (DlssNrDetailReuseFgInterval.has_value())
                DlssNrDetailReuseFgInterval = std::clamp(DlssNrDetailReuseFgInterval.value(), 1u, 2u);
            DlssNrDetailReuseSpread.set_from_config(readUInt("DlssNr", "DetailReuseSpread"));
            if (DlssNrDetailReuseSpread.has_value()) DlssNrDetailReuseSpread = std::min(DlssNrDetailReuseSpread.value(), 3u);
            DlssNrDetailReuseFill.set_from_config(readFloat("DlssNr", "DetailReuseFill"));
            DlssNrDetailReuseDepthTolerance.set_from_config(readFloat("DlssNr", "DetailReuseDepthTolerance"));
            DlssNrDetailReuseClipGamma.set_from_config(readFloat("DlssNr", "DetailReuseClipGamma"));
            DlssNrDetailReuseClipFalloff.set_from_config(readFloat("DlssNr", "DetailReuseClipFalloff"));
            DlssNrDetailReuseSigmaFloor.set_from_config(readFloat("DlssNr", "DetailReuseSigmaFloor"));
            DlssNrDetailReuseWithFg.set_from_config(readBool("DlssNr", "DetailReuseWithFG"));
            DlssNrDetailReuseMinFps.set_from_config(readFloat("DlssNr", "DetailReuseMinFps"));
            DlssNrSceneCut.set_from_config(readUInt("DlssNr", "SceneCut"));
            if (DlssNrSceneCut.value_or_default() > 2u)
                DlssNrSceneCut = 2u;
            DlssNrDetailReuseMaxDropped.set_from_config(readFloat("DlssNr", "DetailReuseMaxDropped"));
            if (DlssNrDetailReuseMaxDropped.has_value() &&
                (!std::isfinite(DlssNrDetailReuseMaxDropped.value()) || DlssNrDetailReuseMaxDropped.value() < 0.0f ||
                 DlssNrDetailReuseMaxDropped.value() > 50.0f))
                DlssNrDetailReuseMaxDropped = std::isfinite(DlssNrDetailReuseMaxDropped.value())
                                                  ? std::clamp(DlssNrDetailReuseMaxDropped.value(), 0.0f, 50.0f)
                                                  : 10.0f;
            if (DlssNrDetailReuseMinFps.has_value() &&
                (!std::isfinite(DlssNrDetailReuseMinFps.value()) || DlssNrDetailReuseMinFps.value() < 0.0f ||
                 DlssNrDetailReuseMinFps.value() > 240.0f))
                DlssNrDetailReuseMinFps = std::isfinite(DlssNrDetailReuseMinFps.value())
                                              ? std::clamp(DlssNrDetailReuseMinFps.value(), 0.0f, 240.0f)
                                              : 25.0f;
            if (DlssNrDetailReuseFill.has_value() &&
                (!std::isfinite(DlssNrDetailReuseFill.value()) || DlssNrDetailReuseFill.value() < 0.0f ||
                 DlssNrDetailReuseFill.value() > 1.0f))
                DlssNrDetailReuseFill = std::isfinite(DlssNrDetailReuseFill.value())
                                            ? std::clamp(DlssNrDetailReuseFill.value(), 0.0f, 1.0f)
                                            : 1.0f;
            if (DlssNrDetailReuseSteady.has_value() &&
                (!std::isfinite(DlssNrDetailReuseSteady.value()) || DlssNrDetailReuseSteady.value() < 0.0f ||
                 DlssNrDetailReuseSteady.value() > 1.0f))
                DlssNrDetailReuseSteady = std::isfinite(DlssNrDetailReuseSteady.value())
                                              ? std::clamp(DlssNrDetailReuseSteady.value(), 0.0f, 1.0f)
                                              : 0.0f;
            if (DlssNrDetailReuseDepthTolerance.has_value() &&
                (!std::isfinite(DlssNrDetailReuseDepthTolerance.value()) ||
                 DlssNrDetailReuseDepthTolerance.value() < 0.0f || DlssNrDetailReuseDepthTolerance.value() > 1.0f))
            {
                // Out of range is clamped, but a value that is not a number is CLEARED rather than set to a
                // literal: the default lives in one place (kDlssNrDetailReuseDepthTolerance) and a copy here would
                // silently go stale the next time it moves, with SaveIni then writing the stale value back.
                if (std::isfinite(DlssNrDetailReuseDepthTolerance.value()))
                    DlssNrDetailReuseDepthTolerance = std::clamp(DlssNrDetailReuseDepthTolerance.value(), 0.0f, 1.0f);
                else
                    DlssNrDetailReuseDepthTolerance = std::optional<float> {};
            }
            if (DlssNrDetailReuseClipGamma.has_value() &&
                (!std::isfinite(DlssNrDetailReuseClipGamma.value()) || DlssNrDetailReuseClipGamma.value() < 0.0f ||
                 DlssNrDetailReuseClipGamma.value() > 10.0f))
                DlssNrDetailReuseClipGamma = std::isfinite(DlssNrDetailReuseClipGamma.value())
                                                 ? std::clamp(DlssNrDetailReuseClipGamma.value(), 0.0f, 10.0f)
                                                 : 1.25f;
            // Never 0: the shader divides by max(clipFalloff, 1e-3), so at 0 trust would stop fading and go binary.
            if (DlssNrDetailReuseClipFalloff.has_value() &&
                (!std::isfinite(DlssNrDetailReuseClipFalloff.value()) || DlssNrDetailReuseClipFalloff.value() < 0.01f ||
                 DlssNrDetailReuseClipFalloff.value() > 10.0f))
                DlssNrDetailReuseClipFalloff = std::isfinite(DlssNrDetailReuseClipFalloff.value())
                                                   ? std::clamp(DlssNrDetailReuseClipFalloff.value(), 0.01f, 10.0f)
                                                   : 1.0f;
            if (DlssNrDetailReuseSigmaFloor.has_value() &&
                (!std::isfinite(DlssNrDetailReuseSigmaFloor.value()) || DlssNrDetailReuseSigmaFloor.value() < 0.0f ||
                 DlssNrDetailReuseSigmaFloor.value() > 1.0f))
                DlssNrDetailReuseSigmaFloor = std::isfinite(DlssNrDetailReuseSigmaFloor.value())
                                                  ? std::clamp(DlssNrDetailReuseSigmaFloor.value(), 0.0f, 1.0f)
                                                  : 0.01f;
            DlssNrToggleKey.set_from_config(readInt("DlssNr", "ToggleKey"));
            CurrentOutputCaptureKey.set_from_config(readInt("Menu", "CurrentOutputCaptureKey"));
            DlssNrTransferStrength.set_from_config(readFloat("DlssNr", "TransferStrength"));
            DlssNrColourStrength.set_from_config(readFloat("DlssNr", "ColourStrength"));
            DlssNrLookMode.set_from_config(readBool("DlssNr", "LookMode"));
            // Retain the legacy INI value while enforcing the current always-on runtime policy.
            DlssNr::LayerLook::EnforceGlobalEnable(*this);
            DlssNrLookStrength.set_from_config(readFloat("DlssNr", "LookStrength"));
            if (DlssNrLookStrength.has_value())
                DlssNrLookStrength = std::isfinite(DlssNrLookStrength.value()) ? std::clamp(DlssNrLookStrength.value(), 0.00f, 2.00f) : 1.00f;
            DlssNrLookBrighten.set_from_config(readFloat("DlssNr", "LookBrighten"));
            if (DlssNrLookBrighten.has_value())
                DlssNrLookBrighten = std::isfinite(DlssNrLookBrighten.value()) ? std::clamp(DlssNrLookBrighten.value(), 0.00f, 2.00f) : 1.00f;
            DlssNrLookDarken.set_from_config(readFloat("DlssNr", "LookDarken"));
            if (DlssNrLookDarken.has_value())
                DlssNrLookDarken = std::isfinite(DlssNrLookDarken.value()) ? std::clamp(DlssNrLookDarken.value(), 0.00f, 2.00f) : 1.00f;
            DlssNrLookColour.set_from_config(readFloat("DlssNr", "LookColour"));
            if (DlssNrLookColour.has_value())
                DlssNrLookColour = std::isfinite(DlssNrLookColour.value()) ? std::clamp(DlssNrLookColour.value(), 0.00f, 2.00f) : 1.00f;
            DlssNrLookHue.set_from_config(readFloat("DlssNr", "LookHue"));
            if (DlssNrLookHue.has_value())
                DlssNrLookHue = std::isfinite(DlssNrLookHue.value()) ? std::clamp(DlssNrLookHue.value(), 0.00f, 2.00f) : 1.00f;
            DlssNrLookShadows.set_from_config(readFloat("DlssNr", "LookShadows"));
            if (DlssNrLookShadows.has_value())
                DlssNrLookShadows = std::isfinite(DlssNrLookShadows.value()) ? std::clamp(DlssNrLookShadows.value(), 0.00f, 2.00f) : 1.00f;
            DlssNrLookMidtones.set_from_config(readFloat("DlssNr", "LookMidtones"));
            if (DlssNrLookMidtones.has_value())
                DlssNrLookMidtones = std::isfinite(DlssNrLookMidtones.value()) ? std::clamp(DlssNrLookMidtones.value(), 0.00f, 2.00f) : 1.00f;
            DlssNrLookHighlights.set_from_config(readFloat("DlssNr", "LookHighlights"));
            if (DlssNrLookHighlights.has_value())
                DlssNrLookHighlights = std::isfinite(DlssNrLookHighlights.value()) ? std::clamp(DlssNrLookHighlights.value(), 0.00f, 2.00f) : 1.00f;
            DlssNrLookMaxBrighten.set_from_config(readFloat("DlssNr", "LookMaxBrighten"));
            if (DlssNrLookMaxBrighten.has_value())
                DlssNrLookMaxBrighten = std::isfinite(DlssNrLookMaxBrighten.value()) ? std::clamp(DlssNrLookMaxBrighten.value(), 0.00f, 4.00f) : 0.00f;
            DlssNrLookMaxDarken.set_from_config(readFloat("DlssNr", "LookMaxDarken"));
            if (DlssNrLookMaxDarken.has_value())
                DlssNrLookMaxDarken = std::isfinite(DlssNrLookMaxDarken.value()) ? std::clamp(DlssNrLookMaxDarken.value(), 0.00f, 4.00f) : 0.00f;
            DlssNrLookMaxColour.set_from_config(readFloat("DlssNr", "LookMaxColour"));
            if (DlssNrLookMaxColour.has_value())
                DlssNrLookMaxColour = std::isfinite(DlssNrLookMaxColour.value()) ? std::clamp(DlssNrLookMaxColour.value(), 0.00f, 2.00f) : 0.00f;
            DlssNrLookTone.set_from_config(readFloat("DlssNr", "LookTone"));
            if (DlssNrLookTone.has_value())
                DlssNrLookTone = std::isfinite(DlssNrLookTone.value()) ? std::clamp(DlssNrLookTone.value(), 0.00f, 2.00f) : 1.00f;
            DlssNrLookDetail.set_from_config(readFloat("DlssNr", "LookDetail"));
            if (DlssNrLookDetail.has_value())
                DlssNrLookDetail = std::isfinite(DlssNrLookDetail.value()) ? std::clamp(DlssNrLookDetail.value(), 0.00f, 2.00f) : 1.00f;
            DlssNrLookDetailRadius.set_from_config(readFloat("DlssNr", "LookDetailRadius"));
            if (DlssNrLookDetailRadius.has_value())
                DlssNrLookDetailRadius = std::isfinite(DlssNrLookDetailRadius.value()) ? std::clamp(DlssNrLookDetailRadius.value(), 0.25f, 4.00f) : 1.00f;
            DlssNrLookHalo.set_from_config(readFloat("DlssNr", "LookHalo"));
            if (DlssNrLookHalo.has_value())
                DlssNrLookHalo = std::isfinite(DlssNrLookHalo.value()) ? std::clamp(DlssNrLookHalo.value(), 0.00f, 1.00f) : 0.00f;
            DlssNrLookHaloBrightSide.set_from_config(readFloat("DlssNr", "LookHaloBrightSide"));
            if (DlssNrLookHaloBrightSide.has_value())
                DlssNrLookHaloBrightSide = std::isfinite(DlssNrLookHaloBrightSide.value()) ? std::clamp(DlssNrLookHaloBrightSide.value(), 0.00f, 1.00f) : 0.00f;

            DlssNr::LayerLook::Load(*this,
                [&](const std::string& key) { return readBool("DlssNr", key); },
                [&](const std::string& key) { return readUInt("DlssNr", key); },
                [&](const std::string& key) { return readFloat("DlssNr", key); });

            DlssNrReplaceDetailStrength.set_from_config(readFloat("DlssNr", "ReplaceDetailStrength"));
            DlssNrMaxRatio.set_from_config(readFloat("DlssNr", "MaxRatio"));
            DlssNrTransfer.set_from_config(readUInt("DlssNr", "Transfer"));

            DlssNrWhitePointFromExposure.set_from_config(readBool("DlssNr", "WhitePointFromExposure"));
            DlssNrProbeD3D11.set_from_config(readBool("DlssNr", "ProbeD3D11"));
            DlssNrFrameStats.set_from_config(readBool("DlssNr", "FrameStats"));
            DlssNrKernelProfile.set_from_config(readBool("DlssNr", "KernelProfile"));
            DlssNrDebugView.set_from_config(readUInt("DlssNr", "DebugView"));
            DlssNrGuidePreview.set_from_config(readUInt("DlssNr", "GuidePreview"));
            DlssNrGuidePreviewRaw.set_from_config(readBool("DlssNr", "GuidePreviewRaw"));
            DlssNrGuidePreviewFlipY.set_from_config(readBool("DlssNr", "GuidePreviewFlipY"));
            DlssNrGuidePreviewMotionGain.set_from_config(readFloat("DlssNr", "GuidePreviewMotionGain"));
            DlssNrCompare.set_from_config(readUInt("DlssNr", "Compare"));
            DlssNrCompareSplit.set_from_config(readFloat("DlssNr", "CompareSplit"));
            DlssNrCompareZoom.set_from_config(readFloat("DlssNr", "CompareZoom"));
            DlssNrCompareSwap.set_from_config(readBool("DlssNr", "CompareSwap"));
            DlssNrCompareTags.set_from_config(readBool("DlssNr", "CompareTags"));
            DlssNrTagScale.set_from_config(readFloat("DlssNr", "TagScale"));
            DlssNrWorkingScale.set_from_config(readFloat("DlssNr", "WorkingScale"));
            DlssNrReducedUpscaleMethod.set_from_config(readUInt("DlssNr", "ReducedUpscaleMethod"));
            // Only 0 (Bilinear) and 1 (SGSR1) exist -- an old config or hand-edited ini past that
            // range falls back to Bilinear rather than being read raw by the enlarge-filter checks.
            if (DlssNrReducedUpscaleMethod.has_value() && DlssNrReducedUpscaleMethod.value() > 1u)
                DlssNrReducedUpscaleMethod = 0u;
            DlssNrModelResolutionAuto.set_from_config(readBool("DlssNr", "ModelResolutionAuto"));
            DlssNrSpatialCompression.set_from_config(readBool("DlssNr", "SpatialCompression"));
            DlssNrSpatialCenterX.set_from_config(readFloat("DlssNr", "SpatialCenterX"));
            DlssNrSpatialCenterY.set_from_config(readFloat("DlssNr", "SpatialCenterY"));
            DlssNrSpatialWorkX.set_from_config(readFloat("DlssNr", "SpatialWorkX"));
            DlssNrSpatialWorkY.set_from_config(readFloat("DlssNr", "SpatialWorkY"));
            DlssNrSpatialOffsetX.set_from_config(readFloat("DlssNr", "SpatialOffsetX"));
            DlssNrSpatialOffsetY.set_from_config(readFloat("DlssNr", "SpatialOffsetY"));
            DlssNrSpatialShiftX.set_from_config(readFloat("DlssNr", "SpatialShiftX"));
            DlssNrSpatialShiftY.set_from_config(readFloat("DlssNr", "SpatialShiftY"));

            if (auto v = readEnum<Scaler>("DlssNr", "ScalingDownscaler"))
                DlssNrScalingDownscaler.set_from_config(*v);
            else
                DlssNrScalingDownscaler.reset();
            DlssNrProxyProbe.set_from_config(readBool("DlssNr", "ProxyProbe"));
            DlssNrUseProxy.set_from_config(readBool("DlssNr", "UseProxy"));
            DlssNrScanExposure.set_from_config(readBool("DlssNr", "ScanExposure"));
            DlssNrWhitePointSource.set_from_config(readUInt("DlssNr", "WhitePointSource"));
            DlssNrGameExposureScale.set_from_config(readUInt("DlssNr", "GameExposureScale"));
            if (DlssNrGameExposureScale.has_value() && DlssNrGameExposureScale.value() > 1)
                DlssNrGameExposureScale.reset();
            DlssNrAutoExposureTrim.set_from_config(readFloat("DlssNr", "AutoExposureTrim"));
            DlssNrAutoExposureShadowProtection.set_from_config(readFloat("DlssNr", "AutoExposureShadowProtection"));
            DlssNrAutoExposureMeter.set_from_config(readUInt("DlssNr", "AutoExposureMeter"));
            DlssNrAutoExposureMeterLowPercent.set_from_config(readFloat("DlssNr", "AutoExposureMeterLowPercent"));
            DlssNrAutoExposureMeterHighPercent.set_from_config(readFloat("DlssNr", "AutoExposureMeterHighPercent"));
            DlssNrNativeDepthFinder.set_from_config(readBool("DlssNr", "NativeDepthFinder"));
            DlssNrNativeDepthVkCopyUsage.set_from_config(readBool("DlssNr", "NativeDepthVkCopyUsage"));
            DlssNrNativeDxvkVulkan.set_from_config(readBool("DlssNr", "NativeDxvkVulkan"));
            DlssNrNativeDepthWarmupFrames.set_from_config(readUInt("DlssNr", "NativeDepthWarmupFrames"));
            DlssNrNativeDepthOverlay.set_from_config(readBool("DlssNr", "NativeDepthOverlay"));
            DlssNrNativeDebugView.set_from_config(readBool("DlssNr", "NativeDebugView"));
            DlssNrNativeMotion.set_from_config(readBool("DlssNr", "NativeMotion"));
            DlssNrOpticalFgNr.set_from_config(readBool("DlssNr", "OpticalFgNr"));
            DlssNrNativeInput.set_from_config(readBool("DlssNr", "NativeInput"));
            DlssNrNativeUpscaler.set_from_config(readBool("DlssNr", "NativeUpscaler"));
            DlssNrNativeFrameGenerationOnly.set_from_config(readBool("DlssNr", "NativeFrameGenerationOnly"));
            if (auto lowLatency = readString("DlssNr", "NativeLowLatency", true); lowLatency.has_value())
            {
                if (lowLatency.value() == "auto")
                    DlssNrNativeLowLatency.set_from_config(native::lowlatency::Setting::Auto);
                else if (lowLatency.value() == "true")
                    DlssNrNativeLowLatency.set_from_config(native::lowlatency::Setting::On);
                else if (lowLatency.value() == "false")
                    DlssNrNativeLowLatency.set_from_config(native::lowlatency::Setting::Off);
            }
            DlssNrNativeLowLatencyBoost.set_from_config(readBool("DlssNr", "NativeLowLatencyBoost"));
            DlssNrAutoExposureAdaptBrighterSeconds.set_from_config(readFloat("DlssNr", "AutoExposureAdaptBrighterSeconds"));
            DlssNrAutoExposureAdaptDarkerSeconds.set_from_config(readFloat("DlssNr", "AutoExposureAdaptDarkerSeconds"));
            DlssNrAutoExposureAdaptSeconds.set_from_config(readFloat("DlssNr", "AutoExposureAdaptSeconds"));
            // An ini from before the two speeds were split keeps its one speed both ways.
            if (DlssNrAutoExposureAdaptSeconds.has_value())
            {
                if (!DlssNrAutoExposureAdaptBrighterSeconds.has_value())
                    DlssNrAutoExposureAdaptBrighterSeconds = DlssNrAutoExposureAdaptSeconds.value();
                if (!DlssNrAutoExposureAdaptDarkerSeconds.has_value())
                    DlssNrAutoExposureAdaptDarkerSeconds = DlssNrAutoExposureAdaptSeconds.value();
            }
            DlssNrAutoExposureFollowGame.set_from_config(readBool("DlssNr", "AutoExposureFollowGame"));
            DlssNrGameExposureTrimAnchors.set_from_config(readString("DlssNr", "GameExposureTrimAnchors"));
            DlssNrAutoExposureTrimAnchors.set_from_config(readString("DlssNr", "AutoExposureTrimAnchors"));

            // Migrate the retired flag only when the new key was genuinely absent, which is why this
            // has to run AFTER the read above -- set_from_config assigns only into an empty optional,
            // so an ini that carries both keys keeps its explicit WhitePointSource.
            if (!DlssNrWhitePointSource.has_value() && DlssNrWhitePointFromExposure.has_value())
                DlssNrWhitePointSource = DlssNrWhitePointFromExposure.value() ? 1u : 0u;
            DlssNrScanMeter.set_from_config(readBool("DlssNr", "ScanMeter"));
            DlssNrScanTrim.set_from_config(readFloat("DlssNr", "ScanTrim"));
            if (const auto passes = readUInt("DlssNr", "Passes"); passes.has_value())
                DlssNrPasses.set_from_config(DlssNr::ClampPassCount(*passes));
            else
                DlssNrPasses.set_from_config(std::nullopt);
            DlssNrPassFeedback.set_from_config(readFloat("DlssNr", "PassFeedback"));
            DlssNrScanAnchorValue.set_from_config(readFloat("DlssNr", "ScanAnchorValue"));
            DlssNrScanAnchorWhitePoint.set_from_config(readFloat("DlssNr", "ScanAnchorWhitePoint"));
            DlssNrScanAnchors.set_from_config(readString("DlssNr", "ScanAnchors"));
            DlssNrScanInverted.set_from_config(readBool("DlssNr", "ScanInverted"));
            DlssNrWhitePointTrim.set_from_config(readFloat("DlssNr", "WhitePointTrim"));
            DlssNrAutoCapture.set_from_config(readBool("DlssNr", "AutoCapture"));
            DlssNrWhitePointScale.set_from_config(readFloat("DlssNr", "WhitePointScale"));
            DlssNrPreset.set_from_config(readUInt("DlssNr", "Preset"));
            DlssNrIntensity.set_from_config(readFloat("DlssNr", "Intensity"));
            DlssNrStyle.set_from_config(readUInt("DlssNr", "Style"));
            DlssNrPass2Preset.set_from_config(readUInt("DlssNr", "Pass2Preset"));
            DlssNrPass2Style.set_from_config(readUInt("DlssNr", "Pass2Style"));
            DlssNrPass3Preset.set_from_config(readUInt("DlssNr", "Pass3Preset"));
            DlssNrPass3Style.set_from_config(readUInt("DlssNr", "Pass3Style"));
            DlssNrLocalStructure.set_from_config(readFloat("DlssNr", "LocalStructure"));
            DlssNrLocalTone.set_from_config(readFloat("DlssNr", "LocalTone"));
            DlssNrSkinStructure.set_from_config(readFloat("DlssNr", "SkinStructure"));
            DlssNrAutoMask.set_from_config(readBool("DlssNr", "AutoMask"));
            DlssNrSkinProtection.set_from_config(readBool("DlssNr", "SkinProtection"));
            DlssNrSkinToneEnabled.set_from_config(readBool("DlssNr", "SkinToneEnabled"));
            DlssNrSkinDetail.set_from_config(readFloat("DlssNr", "SkinDetail"));
            DlssNrSkinColour.set_from_config(readFloat("DlssNr", "SkinColour"));
            DlssNrEnvironmentDetail.set_from_config(readFloat("DlssNr", "EnvironmentDetail"));
            DlssNrEnvironmentColour.set_from_config(readFloat("DlssNr", "EnvironmentColour"));
            DlssNrShowSkinMask.set_from_config(readBool("DlssNr", "ShowSkinMask"));
            DlssNrPass2Intensity.set_from_config(readFloat("DlssNr", "Pass2Intensity"));
            DlssNrPass2LocalStructure.set_from_config(readFloat("DlssNr", "Pass2LocalStructure"));
            DlssNrPass2LocalTone.set_from_config(readFloat("DlssNr", "Pass2LocalTone"));
            DlssNrPass2SkinStructure.set_from_config(readFloat("DlssNr", "Pass2SkinStructure"));
            DlssNrPass2AutoMask.set_from_config(readBool("DlssNr", "Pass2AutoMask"));
            DlssNrPass3Intensity.set_from_config(readFloat("DlssNr", "Pass3Intensity"));
            DlssNrPass3LocalStructure.set_from_config(readFloat("DlssNr", "Pass3LocalStructure"));
            DlssNrPass3LocalTone.set_from_config(readFloat("DlssNr", "Pass3LocalTone"));
            DlssNrPass3SkinStructure.set_from_config(readFloat("DlssNr", "Pass3SkinStructure"));
            DlssNrPass3AutoMask.set_from_config(readBool("DlssNr", "Pass3AutoMask"));
            for (unsigned int i = 0; i < DlssNr::ExtraPassCount; ++i)
            {
                auto& pass = DlssNrExtraPasses[i];
                pass.preset.set_from_config(readUInt("DlssNr", std::format("Pass{}Preset", i + 4).c_str()));
                pass.style.set_from_config(readUInt("DlssNr", std::format("Pass{}Style", i + 4).c_str()));
                pass.intensity.set_from_config(readFloat("DlssNr", std::format("Pass{}Intensity", i + 4).c_str()));
                pass.structure.set_from_config(readFloat("DlssNr", std::format("Pass{}LocalStructure", i + 4).c_str()));
                pass.tone.set_from_config(readFloat("DlssNr", std::format("Pass{}LocalTone", i + 4).c_str()));
                pass.skin.set_from_config(readFloat("DlssNr", std::format("Pass{}SkinStructure", i + 4).c_str()));
                pass.autoMask.set_from_config(readBool("DlssNr", std::format("Pass{}AutoMask", i + 4).c_str()));
            }
            DlssNrReversibleMode.set_from_config(readUInt("DlssNr", "ReversibleMode"));
            if (!DlssNrProxyCurve::Valid(DlssNrReversibleMode.value_or_default())) // not a curve: soft knee, saved as auto
                DlssNrReversibleMode.reset();
            DlssNrColourEncoding.set_from_config(readUInt("DlssNr", "ColourEncoding"));
            if (DlssNrColourEncoding.value_or_default() > 4u) // not a Colour encoding: back to Auto, saved as auto
                DlssNrColourEncoding.reset();
            DlssNrLutFile.set_from_config(readString("DlssNr", "LutFile"));
            DlssNrLutStrength.set_from_config(readFloat("DlssNr", "LutStrength"));
            if (DlssNrLutStrength.has_value() && (!std::isfinite(DlssNrLutStrength.value()) ||
                                                  DlssNrLutStrength.value() < 0.0f || DlssNrLutStrength.value() > 1.0f))
                DlssNrLutStrength =
                    std::isfinite(DlssNrLutStrength.value()) ? std::clamp(DlssNrLutStrength.value(), 0.0f, 1.0f) : 1.0f;
            DlssNrApplyModel.set_from_config(readBool("DlssNr", "ApplyModel"));
            DlssNrHoldFrame.set_from_config(readBool("DlssNr", "HoldFrame"));
            UseGenericAppIdWithDlss.set_from_config(readBool("DLSS", "UseGenericAppIdWithDlss"));

            RenderPresetOverride.set_from_config(readBool("DLSS", "RenderPresetOverride"));

            constexpr size_t presetCount = 17;

            if (auto setting = readInt("DLSS", "RenderPresetForAll");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                RenderPresetForAll.set_from_config(setting);

            if (auto setting = readInt("DLSS", "RenderPresetDLAA");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                RenderPresetDLAA.set_from_config(setting);

            if (auto setting = readInt("DLSS", "RenderPresetUltraQuality");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                RenderPresetUltraQuality.set_from_config(setting);

            if (auto setting = readInt("DLSS", "RenderPresetQuality");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                RenderPresetQuality.set_from_config(setting);

            if (auto setting = readInt("DLSS", "RenderPresetBalanced");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                RenderPresetBalanced.set_from_config(setting);

            if (auto setting = readInt("DLSS", "RenderPresetPerformance");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                RenderPresetPerformance.set_from_config(setting);

            if (auto setting = readInt("DLSS", "RenderPresetUltraPerformance");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                RenderPresetUltraPerformance.set_from_config(setting);
        }
        // DLSSD
        {
            // Don't enable again if set false because of no nvngx found
            DLSSDRenderPresetOverride.set_from_config(readBool("DLSSD", "RenderPresetOverride"));

            constexpr size_t presetCount = 7;

            if (auto setting = readInt("DLSSD", "RenderPresetForAll");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                DLSSDRenderPresetForAll.set_from_config(setting);

            if (auto setting = readInt("DLSSD", "RenderPresetDLAA");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                DLSSDRenderPresetDLAA.set_from_config(setting);

            if (auto setting = readInt("DLSSD", "RenderPresetUltraQuality");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                DLSSDRenderPresetUltraQuality.set_from_config(setting);

            if (auto setting = readInt("DLSSD", "RenderPresetQuality");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                DLSSDRenderPresetQuality.set_from_config(setting);

            if (auto setting = readInt("DLSSD", "RenderPresetBalanced");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                DLSSDRenderPresetBalanced.set_from_config(setting);

            if (auto setting = readInt("DLSSD", "RenderPresetPerformance");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                DLSSDRenderPresetPerformance.set_from_config(setting);

            if (auto setting = readInt("DLSSD", "RenderPresetUltraPerformance");
                setting.has_value() && setting >= 0 && (setting < presetCount || setting == NV_PRESET_LATEST))
                DLSSDRenderPresetUltraPerformance.set_from_config(setting);
        }

        // NvngxFG
        {
            if (auto setting = readBool("Nukems", "MakeDepthCopy"); setting.has_value() && setting.value())
                NvngxFGMakeDepthCopy.set_from_config(setting); // For compat with older config
            else
                NvngxFGMakeDepthCopy.set_from_config(readBool("NvngxFG", "MakeDepthCopy"));

            NvngxFGDispatchFlags.set_from_config(readUInt("NvngxFG", "DispatchFlags"));
            NvngxFGShowDebug.set_from_config(readBool("NvngxFG", "ShowDebug"));
            NvngxFGDisableHudless.set_from_config(readBool("NvngxFG", "DisableHudless"));
        }

        // Logging
        {
            LogToFile.set_from_config(readBool("Log", "LogToFile"));
            LogLevel.set_from_config(readInt("Log", "LogLevel"));
            LogToConsole.set_from_config(readBool("Log", "LogToConsole"));
            LogToDebug.set_from_config(readBool("Log", "LogToDebug"));
            LogToNGX.set_from_config(readBool("Log", "LogToNGX"));
            OpenConsole.set_from_config(readBool("Log", "OpenConsole"));
            DebugWait.set_from_config(readBool("Log", "DebugWait"));
            LogSingleFile.set_from_config(readBool("Log", "SingleFile"));
            LogAsync.set_from_config(readBool("Log", "LogAsync"));
            LogAsyncThreads.set_from_config(readInt("Log", "LogAsyncThreads"));

            {
                auto setting = readString("Log", "LogFileName", false);

                if (setting.has_value() && setting.value().empty())
                    setting = std::nullopt;

                auto path = std::filesystem::path(setting.value_or(wstring_to_string(LogFileName.value_or_default())));
                auto filenameStem = path.stem();

                auto filename =
                    std::filesystem::path(LogSingleFile.value_or_default()
                                              ? filenameStem.wstring() + L".log"
                                              : filenameStem.wstring() + L"_" + std::to_wstring(GetTicks()) + L".log");

                if (setting.has_value())
                {
                    if (path.has_root_path())
                        LogFileName.set_from_config((path.parent_path() / filename).wstring());
                    else
                        LogFileName.set_from_config((Util::DllPath().parent_path() / filename).wstring());
                }
                else
                {
                    if (path.has_root_path())
                        LogFileName.set_volatile_value((path.parent_path() / filename).wstring());
                    else
                        LogFileName.set_volatile_value((Util::DllPath().parent_path() / filename).wstring());
                }
            }
        }

        // Sharpness
        {
            SharpnessShader.set_from_config(readString("Sharpness", "Shader", true).transform(CodeToSharpnessShader));
            OverrideSharpness.set_from_config(readBool("Sharpness", "OverrideSharpness"));

            if (auto setting = readFloat("Sharpness", "Sharpness"); setting.has_value())
                Sharpness.set_from_config(std::clamp(setting.value(), 0.0f, 1.3f));
        }

        // Menu
        {
            if (auto setting = readFloat("Menu", "Scale"); setting.has_value())
                MenuScale.set_from_config(std::clamp(setting.value(), 0.5f, 2.0f));

            if (auto setting = readFloat("Menu", "MenuWidth"); setting.has_value())
                MenuWidth.set_from_config(std::clamp(setting.value(), 100.0f, 8000.0f));

            if (auto setting = readFloat("Menu", "MenuHeight"); setting.has_value())
                MenuHeight.set_from_config(std::clamp(setting.value(), 100.0f, 8000.0f));

            MenuPage.set_from_config(readString("Menu", "MenuPage", true));
            auto menuLanguage = readString("Menu", "Language", true);
            if (menuLanguage == "en" || menuLanguage == "zh")
                MenuLanguage.set_from_config(menuLanguage);
            else if (auto legacyLanguage = readUInt("DlssNr", "Language"); !menuLanguage.has_value() && legacyLanguage.has_value())
                MenuLanguage.set_from_config(std::string(legacyLanguage.value() == 1 ? "en" : "zh"));
            else
                MenuLanguage.set_from_config(std::nullopt);

            // Don't enable again if set false because of Linux issue
            OverlayMenu.set_from_config(readBool("Menu", "OverlayMenu"));
            ShortcutKey.set_from_config(readInt("Menu", "ShortcutKey"));
            ExtendedLimits.set_from_config(readBool("Menu", "ExtendedLimits"));
            ShowFps.set_from_config(readBool("Menu", "ShowFps"));
            F5LowHint.set_from_config(readBool("Menu", "F5LowHint"));
            MenuShowUnfinished.set_from_config(readBool("Menu", "ShowUnfinished"));
            MenuShowNonessential.set_from_config(readBool("Menu", "ShowNonessential"));
            UseHQFont.set_from_config(readBool("Menu", "UseHQFont"));
            DisableSplash.set_from_config(readBool("Menu", "DisableSplash"));

            if (auto setting = readUInt("Menu", "FpsOverlayPos"); setting.has_value())
            {
                FpsOverlayPosition.set_from_config(
                    (FpsOverlayPos) std::clamp(setting.value(), 0U, FpsOverlayPos_COUNT - 1));
            }

            if (auto setting = readUInt("Menu", "FpsOverlayType"); setting.has_value())
            {
                FpsOverlayType.set_from_config((FpsOverlay) std::clamp(setting.value(), 0U, FpsOverlay_COUNT - 1));
            }

            FpsShortcutKey.set_from_config(readInt("Menu", "FpsShortcutKey"));
            FpsCycleShortcutKey.set_from_config(readInt("Menu", "FpsCycleShortcutKey"));
            FpsOverlayHorizontal.set_from_config(readBool("Menu", "FpsOverlayHorizontal"));

            if (auto setting = readFloat("Menu", "FpsOverlayAlpha"); setting.has_value())
                FpsOverlayAlpha.set_from_config(std::clamp(setting.value(), 0.0f, 1.0f));

            if (auto setting = readFloat("Menu", "FpsScale"); setting.has_value())
                FpsScale.set_from_config(std::clamp(setting.value(), 0.5f, 2.0f));

            FontSize.set_from_config(readFloat("Menu", "FontSize"));
            TTFFontPath.set_from_config(readWString("Menu", "TTFFontPath"));

            FGShortcutEnabled.set_from_config(readBool("Menu", "FGShortcutEnabled"));
            FGShortcutKey.set_from_config(readInt("Menu", "FGShortcutKey"));

            LightTheme.set_from_config(readBool("Menu", "LightTheme"));
            ModernTheme.set_from_config(readBool("Menu", "ModernTheme"));
            OverlaysUseTheme.set_from_config(readBool("Menu", "OverlaysUseTheme"));
            MenuAccentColorR.set_from_config(readFloat("Menu", "AccentColorR"));
            MenuAccentColorG.set_from_config(readFloat("Menu", "AccentColorG"));
            MenuAccentColorB.set_from_config(readFloat("Menu", "AccentColorB"));
            MenuBGColorR.set_from_config(readFloat("Menu", "BGColorR"));
            MenuBGColorG.set_from_config(readFloat("Menu", "BGColorG"));
            MenuBGColorB.set_from_config(readFloat("Menu", "BGColorB"));
            MenuBGColorA.set_from_config(readFloat("Menu", "BGColorA"));
        }

        // Hooks
        {
            HookOriginalNvngxOnly.set_from_config(readBool("Hooks", "HookOriginalNvngxOnly"));
            EarlyHooking.set_from_config(readBool("Hooks", "EarlyHooking"));
            D3D11FeatureLevelElevation.set_from_config(readBool("Hooks", "D3D11FeatureLevelElevation"));
            UseNtdllHooks.set_from_config(readBool("Hooks", "UseNtdllHooks"));
        }

        // RCAS
        {
            RcasEnabled.set_from_config(readBool("CAS", "Enabled"));

            MotionSharpnessEnabled.set_from_config(readBool("CAS", "MotionSharpnessEnabled"));

            if (auto setting = readFloat("CAS", "MotionSharpness"); setting.has_value())
                MotionSharpness.set_from_config(std::clamp(setting.value(), -1.3f, 1.3f));

            if (auto setting = readFloat("CAS", "MotionThreshold"); setting.has_value())
                MotionThreshold.set_from_config(std::clamp(setting.value(), 0.0f, 100.0f));

            if (auto setting = readFloat("CAS", "MotionScaleLimit"); setting.has_value())
                MotionScaleLimit.set_from_config(std::clamp(setting.value(), 0.01f, 100.0f));

            ContrastEnabled.set_from_config(readBool("CAS", "ContrastEnabled"));
            if (auto setting = readFloat("CAS", "Contrast"); setting.has_value())
                Contrast.set_from_config(std::clamp(setting.value(), -2.0f, 2.0f));

            DADepthScale.set_from_config(readFloat("CAS", "DADepthScale"));
            DADepthBias.set_from_config(readFloat("CAS", "DADepthBias"));
            DAClampOutput.set_from_config(readBool("CAS", "DAClampOutput"));

            MotionSharpnessDebug.set_from_config(readBool("CAS", "SharpenerDebug"));
        }

        // Magnifier
        {
            MagnifierEnabled.set_from_config(readBool("Magnifier", "Enabled"));

            if (auto setting = readFloat("Magnifier", "Size"); setting.has_value())
                MagnifierSize.set_from_config(std::clamp(setting.value(), 0.0f, 100.0f));

            if (auto setting = readInt("Magnifier", "ZoomFactor"); setting.has_value())
                MagnifierZoomFactor.set_from_config(std::clamp(setting.value(), 2, 20));

            if (auto setting = readFloat("Magnifier", "BorderSize"); setting.has_value())
                MagnifierBorderSize.set_from_config(std::clamp(setting.value(), 0.0f, 2.0f));

            if (auto setting = readFloat("Magnifier", "CursorOffsetX"); setting.has_value())
                MagnifierCursorOffsetX.set_from_config(std::clamp(setting.value(), -1000.f, 1000.f));
            if (auto setting = readFloat("Magnifier", "CursorOffsetY"); setting.has_value())
                MagnifierCursorOffsetY.set_from_config(std::clamp(setting.value(), -1000.f, 1000.f));

            if (auto setting = readFloat("Magnifier", "StaticPosX"); setting.has_value())
                MagnifierStaticPosX.set_from_config(std::clamp(setting.value(), 0.0f, 100.0f));
            if (auto setting = readFloat("Magnifier", "StaticPosY"); setting.has_value())
                MagnifierStaticPosY.set_from_config(std::clamp(setting.value(), 0.0f, 100.0f));
        }

        // Output Scaling
        {
            OutputScalingEnabled.set_from_config(readBool("OutputScaling", "Enabled"));

            if (auto v = readEnum<Scaler>("OutputScaling", "Downscaler"))
                OutputScalingDownscaler.set_from_config(*v);
            else
                OutputScalingDownscaler.reset();

            if (auto setting = readFloat("OutputScaling", "Multiplier"); setting.has_value())
                OutputScalingMultiplier.set_from_config(std::clamp(setting.value(), 0.5f, 3.0f));
        }

        // Init Flags
        {
            AutoExposure.set_from_config(readBool("InitFlags", "AutoExposure"));
            HDR.set_from_config(readBool("InitFlags", "HDR"));
            DepthInverted.set_from_config(readBool("InitFlags", "DepthInverted"));
            JitterCancellation.set_from_config(readBool("InitFlags", "JitterCancellation"));
            DisplayResolution.set_from_config(readBool("InitFlags", "DisplayResolution"));
            DisableReactiveMask.set_from_config(readBool("InitFlags", "DisableReactiveMask"));
        }

        // DRS
        {
            DrsMinOverrideEnabled.set_from_config(readBool("DRS", "DrsMinOverrideEnabled"));
            DrsMaxOverrideEnabled.set_from_config(readBool("DRS", "DrsMaxOverrideEnabled"));
        }

        // Upscale Ratio Override
        {
            UpscaleRatioOverrideEnabled.set_from_config(readBool("UpscaleRatio", "UpscaleRatioOverrideEnabled"));
            UpscaleRatioOverrideValue.set_from_config(readFloat("UpscaleRatio", "UpscaleRatioOverrideValue"));
        }

        // Quality Overrides
        {
            QualityRatioOverrideEnabled.set_from_config(readBool("QualityOverrides", "QualityRatioOverrideEnabled"));
            QualityRatio_DLAA.set_from_config(readFloat("QualityOverrides", "QualityRatioDLAA"));
            QualityRatio_UltraQuality.set_from_config(readFloat("QualityOverrides", "QualityRatioUltraQuality"));
            QualityRatio_Quality.set_from_config(readFloat("QualityOverrides", "QualityRatioQuality"));
            QualityRatio_Balanced.set_from_config(readFloat("QualityOverrides", "QualityRatioBalanced"));
            QualityRatio_Performance.set_from_config(readFloat("QualityOverrides", "QualityRatioPerformance"));
            QualityRatio_UltraPerformance.set_from_config(
                readFloat("QualityOverrides", "QualityRatioUltraPerformance"));
        }

        // Anisotropy
        {
            if (auto setting = readInt("Anisotropy", "AnisotropyOverride");
                setting.has_value() && setting.value() <= 16 && setting.value() >= 1)
                AnisotropyOverride.set_from_config(setting);

            if (AnisotropyOverride.has_value() && (AnisotropyOverride.value() > 16 || AnisotropyOverride.value() < 1))
                AnisotropyOverride.reset();

            AnisotropySkipPointFilter.set_from_config(readBool("Anisotropy", "SkipPointFilter"));
            AnisotropyModifyComp.set_from_config(readBool("Anisotropy", "AFModifyComparison"));
            AnisotropyModifyMinMax.set_from_config(readBool("Anisotropy", "AFModifyMinMax"));
        }

        // Mipmap
        {
            if (auto setting = readFloat("Mipmap", "MipmapBiasOverride");
                setting.has_value() && setting.value() <= 15.0 && setting.value() >= -15.0)
                MipmapBiasOverride.set_from_config(setting);

            // Unsure if that's needed but it resets invalid MipmapBiasOverride on config reload
            // Unexpected place for it but could be playing a role
            if (MipmapBiasOverride.has_value() &&
                (MipmapBiasOverride.value() > 15.0 || MipmapBiasOverride.value() < -15.0))
                MipmapBiasOverride.reset();

            MipmapBiasFixedOverride.set_from_config(readBool("Mipmap", "MipmapBiasFixedOverride"));
            MipmapBiasScaleOverride.set_from_config(readBool("Mipmap", "MipmapBiasScaleOverride"));
            MipmapBiasOverrideAll.set_from_config(readBool("Mipmap", "MipmapBiasOverrideAll"));
        }

        // Process Filter
        {
            ProcessExclusionList.set_from_config(readWString("ProcessFilter", "ProcessExclusionList", true));
            TargetProcess.set_from_config(readWString("ProcessFilter", "TargetProcessName", true));
        }

        // Hotfixes
        {
            CheckForUpdate.set_from_config(readBool("Hotfix", "CheckForUpdate"));
            DisableOverlays.set_from_config(readBool("Hotfix", "DisableOverlays"));
            REFrameworkCompat.set_from_config(readBool("Hotfix", "REFrameworkCompat"));

            SimulateWaitableObject.set_from_config(readBool("Hotfix", "SimulateWaitableObject"));

            RoundInternalResolution.set_from_config(readInt("Hotfix", "RoundInternalResolution"));

            RestoreComputeSignature.set_from_config(readBool("Hotfix", "RestoreComputeSignature"));
            RestoreGraphicSignature.set_from_config(readBool("Hotfix", "RestoreGraphicSignature"));
            ExtendedStateRestore.set_from_config(readBool("Hotfix", "ExtendedStateRestore"));
            PreferDedicatedGpu.set_from_config(readBool("Hotfix", "PreferDedicatedGpu"));
            PreferFirstDedicatedGpu.set_from_config(readBool("Hotfix", "PreferFirstDedicatedGpu"));
            SkipFirstFrames.set_from_config(readInt("Hotfix", "SkipFirstFrames"));
            UsePrecompiledShaders.set_from_config(readBool("Hotfix", "UsePrecompiledShaders"));
            ColorResourceBarrier.set_from_config(readInt("Hotfix", "ColorResourceBarrier"));
            MVResourceBarrier.set_from_config(readInt("Hotfix", "MotionVectorResourceBarrier"));
            DepthResourceBarrier.set_from_config(readInt("Hotfix", "DepthResourceBarrier"));
            MaskResourceBarrier.set_from_config(readInt("Hotfix", "ColorMaskResourceBarrier"));
            ExposureResourceBarrier.set_from_config(readInt("Hotfix", "ExposureResourceBarrier"));
            OutputResourceBarrier.set_from_config(readInt("Hotfix", "OutputResourceBarrier"));
            CreateD3D12DeviceForLuma.set_from_config(readBool("Hotfix", "CreateD3D12DeviceForLuma"));
        }

        // Dx11 with Dx12
        {
            Dx11DelayedInit.set_from_config(readInt("Dx11withDx12", "UseDelayedInit"));
            DontUseNTShared.set_from_config(readBool("Dx11withDx12", "DontUseNTShared"));
        }

        // NvApi
        {
            DisableFlipMetering.set_from_config(readBool("NvApi", "DisableFlipMetering"));
        }

        // Spoofing
        {
            DxgiSpoofing.set_from_config(readBool("Spoofing", "Dxgi"));
            DxgiFactoryWrapping.set_from_config(readBool("Spoofing", "DxgiFactoryWrapping"));
            DxgiBlacklist.set_from_config(readString("Spoofing", "DxgiBlacklist"));
            DxgiVRAM.set_from_config(readInt("Spoofing", "DxgiVRAM"));
            VulkanSpoofing.set_from_config(readBool("Spoofing", "Vulkan"));
            VulkanExtensionSpoofing.set_from_config(readBool("Spoofing", "VulkanExtensionSpoofing"));
            VulkanVRAM.set_from_config(readInt("Spoofing", "VulkanVRAM"));
            SpoofedGPUName.set_from_config(readWString("Spoofing", "SpoofedGPUName"));
            StreamlineSpoofing.set_from_config(readBool("Spoofing", "StreamlineSpoofing"));
            SpoofHAGS.set_from_config(readBool("Spoofing", "SpoofHAGS"));
            SpoofFeatureLevel.set_from_config(readBool("Spoofing", "D3DFeatureLevel"));
            SpoofedVendorId.set_from_config(readUInt("Spoofing", "SpoofedVendorId"));
            SpoofedDeviceId.set_from_config(readUInt("Spoofing", "SpoofedDeviceId"));
            TargetVendorId.set_from_config(readUInt("Spoofing", "TargetVendorId"));
            TargetDeviceId.set_from_config(readUInt("Spoofing", "TargetDeviceId"));
            UESpoofIntelAtomics64.set_from_config(readBool("Spoofing", "UEIntelAtomics"));
            SpoofRegistry.set_from_config(readBool("Spoofing", "Registry"));
            SpoofedDriver.set_from_config(readWString("Spoofing", "RegistryDriver"));
            SpoofUser32.set_from_config(readBool("Spoofing", "User32"));

            // Enable HAGS when DLSS-G will be used
            if (!SpoofHAGS.has_value())
            {
                SpoofHAGS.set_volatile_value(FGInput.value_or_default() == FGInput::NvngxFG ||
                                             FGInput.value_or_default() == FGInput::DLSSG);
            }
        }

        // fakenvapi
        {
            UseFakenvapi.set_from_config(readBool("fakenvapi", "UseFakenvapi"));
            ForceXeLL.set_from_config(readBool("fakenvapi", "ForceXeLL"));
            FN_ForceLatencyFlex.set_from_config(readBool("fakenvapi", "ForceLatencyFlex"));

            if (auto v = readEnum<LFXMode>("fakenvapi", "LatencyFlexMode"))
                FN_LatencyFlexMode.set_from_config(*v);
            else
                FN_LatencyFlexMode.reset();

            if (auto v = readEnum<ForceReflex>("fakenvapi", "ForceReflex"))
                FN_ForceReflex.set_from_config(*v);
            else
                FN_ForceReflex.reset();

            // DMFG is a mess with our reflex implementations, disable by default
            if (FGDLSSGOverrideForceDMFG.value_or_default() && !FN_ForceReflex.has_value())
                FN_ForceReflex.set_volatile_value(ForceReflex::ForceDisable);
        }

        // Inputs
        {
            EnableDlssInputs.set_from_config(readBool("Inputs", "EnableDlssInputs"));
            EnableXeSSInputs.set_from_config(readBool("Inputs", "EnableXeSSInputs"));

            {
                std::lock_guard<std::recursive_mutex> guard(genshinControlsMutex_);
                EnableFsr2Inputs.set_from_config(readBool("Inputs", "EnableFsr2Inputs"));
                UseFsr2Inputs.set_from_config(readBool("Inputs", "UseFsr2Inputs"));
                SyncGenshinInputControlsLocked();
            }
            UseFsr2Dx11Inputs.set_from_config(readBool("Inputs", "UseFsr2Dx11Inputs"));
            UseFsr2VulkanInputs.set_from_config(readBool("Inputs", "UseFsr2VulkanInputs"));
            Fsr2Pattern.set_from_config(readBool("Inputs", "Fsr2Pattern"));

            EnableFsr3Inputs.set_from_config(readBool("Inputs", "EnableFsr3Inputs"));
            UseFsr3Inputs.set_from_config(readBool("Inputs", "UseFsr3Inputs"));
            Fsr3Pattern.set_from_config(readBool("Inputs", "Fsr3Pattern"));

            EnableFfxInputs.set_from_config(readBool("Inputs", "EnableFfxInputs"));
            UseFfxInputs.set_from_config(readBool("Inputs", "UseFfxInputs"));
            EnableHotSwapping.set_from_config(readBool("Inputs", "EnableHotSwapping"));
        }

        // Plugins
        {
            PluginPath.set_from_config(readWString("Plugins", "Path"));
            LoadSpecialK.set_from_config(readBool("Plugins", "LoadSpecialK"));
            LoadReShade.set_from_config(readBool("Plugins", "LoadReShade"));
            const auto hoYoEnabled = readBool("HoYoShade", "Enabled");
            const auto hoYoStage = readInt("HoYoShade", "Stage");
            DrawUiMaskEnabled.set_from_config(readBool("DrawUiMask", "Enabled"));
            DrawUiMaskFg.set_from_config(readBool("DrawUiMask", "FG"));
            DrawUiMaskNr.set_from_config(readBool("DrawUiMask", "NR"));
            DrawUiMaskFilters.set_from_config(readBool("DrawUiMask", "Filters"));
            DrawUiMaskRules.set_from_config(readString("DrawUiMask", "Rules"));
            DrawUiMask::Configure({DrawUiMaskEnabled.value_or_default(),DrawUiMaskFg.value_or_default(),
                DrawUiMaskNr.value_or_default(),DrawUiMaskFilters.value_or_default(),DrawUiMaskRules.value_or_default()});
            GimiEnabled.set_from_config(readBool("GIMI", "Enabled"));
            GimiIni.set_from_config(readString("GIMI", "Ini"));
            GimiComponent.set_from_config(readString("GIMI", "Component"));
            GimiTrigger.set_from_config(readString("GIMI", "Trigger"));
            GimiLibraries.set_from_config(readString("GIMI", "Libraries"));
            GimiHostRoot.set_from_config(readString("GIMI", "HostRoot"));
            GimiNamespaceRoot.set_from_config(readString("GIMI", "NamespaceRoot"));
            GimiImportRoot.set_from_config(readString("GIMI", "ImportRoot"));
            gimi::runtime::Configure({GimiEnabled.value_or_default(),GimiIni.value_or_default(),
                GimiComponent.value_or_default(),GimiTrigger.value_or_default(),GimiLibraries.value_or_default(),
                GimiImportRoot.value_or_default(),{},GimiNamespaceRoot.value_or_default(),GimiHostRoot.value_or_default()});
            HoYoShadeEnabled.set_from_config(hoYoEnabled);
            HoYoShadeStage.set_from_config(hoYoStage);
            // R040 snapshot of SimpleIni's selected raw values and the same parsed optionals.
            {
                std::lock_guard lock(HoYoShadeStartupTrace::mutex);
                auto& captured = HoYoShadeStartupTrace::snapshot;
                captured.loaded = true;
                if (const char* raw = ini.GetValue("HoYoShade", "Enabled", nullptr)) captured.rawEnabled = raw;
                if (const char* raw = ini.GetValue("HoYoShade", "Stage", nullptr)) captured.rawStage = raw;
                captured.parsedEnabled = hoYoEnabled;
                captured.parsedStage = hoYoStage;
            }
            LoadCustomAmdxc64OnRdna2.set_from_config(readBool("Plugins", "LoadCustomAmdxc64OnRdna2"));
            LoadAsiPlugins.set_from_config(readBool("Plugins", "LoadAsiPlugins"));
            LateAsiPluginsDelay.set_from_config(readInt("Plugins", "LateAsiPluginsDelay"));
        }

        // HDR
        {
            ForceHDR.set_from_config(readBool("HDR", "ForceHDR"));
            UseHDR10.set_from_config(readBool("HDR", "UseHDR10"));
            SkipColorSpace.set_from_config(readBool("HDR", "SkipColorSpace"));
        }

        // V-Sync
        {
            OverrideVsync.set_from_config(readBool("V-Sync", "OverrideVsync"));
            ForceVsync.set_from_config(readBool("V-Sync", "ForceVsync"));
            VsyncInterval.set_from_config(readInt("V-Sync", "SyncInterval"));
        }

        // Libraries
        {
            MainDllPath.set_from_config(readWString("Libraries", "OptiDllPath"));

            NvngxPath.set_from_config(readWString("Libraries", "NvngxPath"));
            NVNGX_DLSS_Library.set_from_config(readWString("Libraries", "NvngxDlssPath"));
            DLSSFeaturePath.set_from_config(readWString("Libraries", "NvngxFeaturePath"));
            NvapiDllPath.set_from_config(readWString("Libraries", "NvapiPath"));

            FfxDx12Path.set_from_config(readWString("Libraries", "FfxDx12Path"));
            FfxDx12SRPath.set_from_config(readWString("Libraries", "FfxDx12SRPath"));
            FfxDx12FGPath.set_from_config(readWString("Libraries", "FfxDx12FGPath"));
            FfxDx12RRPath.set_from_config(readWString("Libraries", "FfxDx12RRPath"));
            FfxDx12RCPath.set_from_config(readWString("Libraries", "FfxDx12RCPath"));
            FfxVkPath.set_from_config(readWString("Libraries", "FfxVkPath"));

            XeSSLibrary.set_from_config(readWString("Libraries", "XeSSPath"));
            XeSSLibrary.set_from_config(readWString("Libraries", "XeFGPath"));
            XeSSLibrary.set_from_config(readWString("Libraries", "XeLLPath"));
            XeSSDx11Library.set_from_config(readWString("Libraries", "XeSSDx11Path"));
        }

        LoadGenshinControls();

        // Reading old configs for compatibility reasons
        {
            _DONTUSE_Fsr4ForceEnableInt8.set_from_config(readBool("FSR", "Fsr4ForceEnableInt8"));
        }

        ReloadStarRailInputProfile(iniPath);
        // Establish the initial load baseline only. Reload must not bless edits
        // that set_from_config intentionally retains; only SaveIni advances it.
        if (!savedSettings_) CaptureSavedSettings();

        return true;
    }

    return false;
}

void Config::SyncGenshinInputControlsLocked() const
{
    const bool enable=EnableFsr2Inputs.value_or_default(),use=UseFsr2Inputs.value_or_default();
    if(genshinControls_.enableFsr2Inputs!=enable || genshinControls_.useFsr2Inputs!=use)
    {genshinControls_.enableFsr2Inputs=enable;genshinControls_.useFsr2Inputs=use;++genshinControls_.epoch;}
    genshinControlsRevision_.Publish(genshinControls_.epoch);
}
GenshinLegacy::ControlsSnapshot Config::AcquireGenshinControls() const
{
    GenshinHookCpuTiming::Scope wait(GenshinHookCpuTiming::Stage::ControlsLock);
    std::lock_guard<std::recursive_mutex> guard(genshinControlsMutex_);
    // Scope ends immediately after actual lock acquisition; original mutex stays intact.
    wait.Finish();
    GenshinHookCpuTiming::Scope held(GenshinHookCpuTiming::Stage::ControlsLocked);
    SyncGenshinInputControlsLocked();
    return genshinControls_;
}
void Config::SetGenshinHookCpuTiming(bool enabled)
{
    std::lock_guard<std::recursive_mutex> guard(genshinControlsMutex_);
    genshinControls_.hookCpuTiming=enabled;
    // AcquireGenshinControls / PublishHookTiming carries this to the sampler at
    // the next existing Present boundary. Preserve the route/admission epoch.
}
void Config::SetUseFsr2Inputs(bool value)
{
    std::lock_guard<std::recursive_mutex> guard(genshinControlsMutex_);
    UseFsr2Inputs=value; // Preserve CustomOptional assignment/persistence semantics.
    SyncGenshinInputControlsLocked();
}
bool Config::ApplyFsr2InputDisableQuirk()
{
    std::lock_guard<std::recursive_mutex> guard(genshinControlsMutex_);
    if(EnableFsr2Inputs.has_value())return false;
    EnableFsr2Inputs.set_volatile_value(false);
    SyncGenshinInputControlsLocked();
    return true;
}
bool Config::EditGenshinControls(uint64_t observedEpoch,const std::function<void(GenshinLegacy::Settings&)>& edit)
{
    std::lock_guard<std::recursive_mutex> guard(genshinControlsMutex_);
    if(!edit || observedEpoch!=AcquireGenshinControls().epoch)return false;
    auto candidate=genshinControls_.settings;
    try{edit(candidate);}catch(...){return false;}
    if(observedEpoch!=AcquireGenshinControls().epoch)return false;
    if(!GenshinLegacy::ValidSettings(candidate))return false;
    if(candidate!=genshinControls_.settings){genshinControls_.settings=candidate;++genshinControls_.epoch;
        genshinControlsRevision_.Publish(genshinControls_.epoch);}
    return true;
}

void Config::LoadGenshinControls()
{
    std::lock_guard<std::recursive_mutex> guard(genshinControlsMutex_);
    genshinControls_.settings=GenshinLegacy::LoadControls(ini);
    // Detailed diagnostics belong to this process, not the rendering profile.
    // Reload must also preserve the user's current session toggle. Older INIs
    // may contain HookCpuTiming=true; leave those files intact but do not resume
    // heavy diagnostics silently at the next game launch.
    ++genshinControls_.epoch;genshinControlsRevision_.Publish(genshinControls_.epoch);
}

bool Config::LoadFromPath(const wchar_t* InPath)
{
    std::filesystem::path iniPath(InPath);
    auto newPath = iniPath / fileName;

    if (Reload(newPath))
    {
        absoluteFileName = newPath;
        return true;
    }

    return false;
}

std::string GetBoolValue(std::optional<bool> value)
{
    if (!value.has_value())
        return "auto";

    return value.value() ? "true" : "false";
}

template <typename T> std::string GetIntValue(std::optional<T> value, bool getHex = false)
{
    if (!value.has_value())
        return "auto";

    if constexpr (std::is_enum_v<T>)
    {
        using Underlying = std::underlying_type_t<T>;
        Underlying v = static_cast<Underlying>(value.value());

        if (getHex)
            return std::format("{:#x}", v);

        return std::to_string(v);
    }
    else
    {
        if (getHex)
            return std::format("{:#x}", value.value());

        return std::to_string(value.value());
    }
}

std::string GetFloatValue(std::optional<float> value)
{
    if (!value.has_value())
        return "auto";

    return std::to_string(value.value());
}

void Config::SettingsSample::Begin()
{
    cursor = 0;
    changed = false;
}

void Config::SettingsSample::Observe(Operation operation, std::string_view section, std::string_view key,
    std::string_view value, bool removeEmpty)
{
    const size_t index = cursor++;
    if (index < writes.size())
    {
        const auto& previous = writes[index];
        if (previous.operation == operation && previous.section == section && previous.key == key &&
            previous.value == value && previous.removeEmpty == removeEmpty)
            return;
    }

    // Invalidate BEFORE a vector/string allocation: an interrupted pass must
    // never leave an old successful snapshot attached to partly changed writes.
    serialized.reset();
    changed = true;
    if (index == writes.size())
        writes.push_back({ operation, std::string(section), std::string(key), std::string(value), removeEmpty });
    else
    {
        auto& write = writes[index];
        write.operation = operation;
        write.section = section;
        write.key = key;
        write.value = value;
        write.removeEmpty = removeEmpty;
    }
}

void Config::SettingsSample::SetValue(const char* section, const char* key, const char* value)
{
    // All current schema writes use the three-argument, non-null form. Copy
    // c-string views, not pointers: controls/formatters may use temporary storage.
    Observe(Operation::Set, section, key, value, false);
}

void Config::SettingsSample::Delete(const char* section, const char* key, bool removeEmpty)
{
    Observe(Operation::Erase, section, key, {}, removeEmpty);
}

bool Config::SettingsSample::Finish()
{
    if (cursor != writes.size())
    {
        // Conditional schema output can disappear (e.g. the Star Rail block).
        // Replaying into a FRESH tree also drops keys/sections no longer emitted.
        serialized.reset();
        changed = true;
        writes.resize(cursor);
    }
    if (!changed && serialized)
        return false;

    // Do not patch the loaded/global ini or accumulate edits in an old tree.
    // Fresh replay preserves SimpleIni's case handling, duplicate-key writes,
    // deletions, empty-section behavior and first-insertion output ordering.
    // Its immutable base is the original default/EMPTY CSimpleIniA, not ini.
    // Loaded-INI unknown keys/comments are outside this projection, as before.
    CSimpleIniA memoryIni;
    for (const auto& write : writes)
    {
        if (write.operation == Operation::Set)
        {
            if (memoryIni.SetValue(write.section.c_str(), write.key.c_str(), write.value.c_str()) < 0)
                return true;
        }
        else
            memoryIni.Delete(write.section.c_str(), write.key.c_str(), write.removeEmpty);
    }
    std::string current;
    if (memoryIni.Save(current) >= 0)
        serialized = std::move(current);
    // An unavailable snapshot stays unavailable and retries on the next poll.
    return true;
}

template <typename Sink>
void Config::WriteSettings(Sink& target, const GenshinLegacy::Settings& genshinSettings)
{
    // Upscalers
    {
        auto SaveUpscaler = [&](const char* key, auto& upscalerSetting)
        {
            std::string value = upscalerSetting.value_for_config()
                                    .transform(UpscalerToCode) // Turn enum into string
                                    .value_or("auto");

            target.SetValue("Upscalers", key, value.c_str());
        };

        SaveUpscaler("Dx11Upscaler", this->Dx11Upscaler);
        SaveUpscaler("Dx12Upscaler", this->Dx12Upscaler);
        SaveUpscaler("VulkanUpscaler", this->VulkanUpscaler);
    }

    // Frame Generation
    {
        target.SetValue("FrameGen", "Enabled", GetBoolValue(this->FGEnabled.value_for_config()).c_str());
        target.SetValue("FrameGen", "StarRailForceResident", GetBoolValue(this->StarRailForceFGResident.value_for_config()).c_str());
        target.SetValue("FrameGen", "External", GetBoolValue(this->ExternalFrameGeneration.value_for_config_or(false)).c_str());
        target.SetValue("DLSSG", "AdaMfgUnlock", GetBoolValue(this->FGDLSSGAdaMfgUnlock.value_for_config()).c_str());
        target.SetValue("DLSSG", "AdaBlackwellKernels", GetBoolValue(this->FGDLSSGAdaBlackwellKernels.value_for_config()).c_str());
        target.SetValue("DLSSG", "AdaTemporalFix", this->FGDLSSGAdaTemporalFix.value_for_config_or("auto").c_str());
        target.SetValue("DLSSG", "AdaFlipMeteringPatch", GetBoolValue(this->FGDLSSGAdaFlipMeteringPatch.value_for_config()).c_str());
        target.SetValue("DLSSG", "AmpereMfgUnlock", GetBoolValue(this->FGDLSSGAmpereMfgUnlock.value_for_config()).c_str());
        target.SetValue("FrameGen", "DebugView", GetBoolValue(this->FGDebugView.value_for_config()).c_str());
        std::string FGInputString = "auto";
        if (auto FGInputHeld = this->FGInput.value_for_config(); FGInputHeld.has_value())
        {
            if (FGInputHeld.value() == FGInput::NoFG)
                FGInputString = "NoFG";
            else if (FGInputHeld.value() == FGInput::Upscaler)
                FGInputString = "Upscaler";
            else if (FGInputHeld.value() == FGInput::NvngxFG)
                FGInputString = "NvngxFG";
            else if (FGInputHeld.value() == FGInput::DLSSG)
                FGInputString = "DLSSG";
            else if (FGInputHeld.value() == FGInput::FSRFG)
                FGInputString = "FSRFG";
            else if (FGInputHeld.value() == FGInput::FSRFG30)
                FGInputString = "FSRFG30";
        }
        target.SetValue("FrameGen", "FGInput", FGInputString.c_str());

        std::string FGOutputString = "auto";
        if (auto FGOutputHeld = this->FGOutput.value_for_config(); FGOutputHeld.has_value())
        {
            if (FGOutputHeld.value() == FGOutput::NoFG)
                FGOutputString = "NoFG";
            else if (FGOutputHeld.value() == FGOutput::FSRFG)
                FGOutputString = "FSRFG";
            else if (FGOutputHeld.value() == FGOutput::XeFG)
                FGOutputString = "XeFG";
            else if (FGOutputHeld.value() == FGOutput::DLSSG)
                FGOutputString = "DLSSG";
        }
        target.SetValue("FrameGen", "FGOutput", FGOutputString.c_str());

        target.SetValue("FrameGen", "StreamlineIgnoreOTA",
                     GetBoolValue(this->FGStreamlineIgnoreOTA.value_for_config()).c_str());

        std::string FGNvngxReplacementString = "auto";
        if (auto FGNvngxReplacementHeld = this->FGNvngxReplacement.value_for_config();
            FGNvngxReplacementHeld.has_value())
        {
            if (FGNvngxReplacementHeld.value() == FGNvngxReplacement::None)
                FGNvngxReplacementString = "None";
            else if (FGNvngxReplacementHeld.value() == FGNvngxReplacement::Nukems)
                FGNvngxReplacementString = "Nukems";
            else if (FGNvngxReplacementHeld.value() == FGNvngxReplacement::Arturs)
                FGNvngxReplacementString = "Arturs";
            else if (FGNvngxReplacementHeld.value() == FGNvngxReplacement::FFX)
                FGNvngxReplacementString = "FFX";
            else if (FGNvngxReplacementHeld.value() == FGNvngxReplacement::Combo)
                FGNvngxReplacementString = "Combo";
        }
        target.SetValue("FrameGen", "FGNvngxReplacement", FGNvngxReplacementString.c_str());

        std::optional<int> ftInput;
        if (const auto persisted = this->FTInput.value_for_config())
            ftInput = (int) *persisted;

        target.SetValue("FrameGen", "FTInput", GetIntValue(ftInput).c_str());
        target.SetValue("FrameGen", "DrawUIOverFG", GetBoolValue(this->FGDrawUIOverFG.value_for_config()).c_str());
        target.SetValue("FrameGen", "UIPremultipliedAlpha",
                     GetBoolValue(this->FGUIPremultipliedAlpha.value_for_config()).c_str());
        target.SetValue("FrameGen", "DisableHudless",
                     GetBoolValue(this->FGDisableHudless.value_for_config()).c_str());
        target.SetValue("FrameGen", "DisableUI", GetBoolValue(this->FGDisableUI.value_for_config()).c_str());
        target.SetValue("FrameGen", "SkipReset", GetBoolValue(this->FGSkipReset.value_for_config()).c_str());
        target.SetValue("FrameGen", "RectLeft", GetIntValue(this->FGRectLeft.value_for_config()).c_str());
        target.SetValue("FrameGen", "RectTop", GetIntValue(this->FGRectTop.value_for_config()).c_str());
        target.SetValue("FrameGen", "RectWidth", GetIntValue(this->FGRectWidth.value_for_config()).c_str());
        target.SetValue("FrameGen", "RectHeight", GetIntValue(this->FGRectHeight.value_for_config()).c_str());
        target.SetValue("FrameGen", "AllowedFrameAhead",
                     GetIntValue(this->FGAllowedFrameAhead.value_for_config()).c_str());
        target.SetValue("FrameGen", "DepthValidNow", GetBoolValue(this->FGDepthValidNow.value_for_config()).c_str());
        target.SetValue("FrameGen", "VelocityValidNow",
                     GetBoolValue(this->FGVelocityValidNow.value_for_config()).c_str());
        target.SetValue("FrameGen", "HudlessValidNow",
                     GetBoolValue(this->FGHudlessValidNow.value_for_config()).c_str());
        target.SetValue("FrameGen", "OnlyAcceptFirstHudless",
                     GetBoolValue(this->FGOnlyAcceptFirstHudless.value_for_config()).c_str());
        target.SetValue("FrameGen", "PreserveSwapChain",
                     GetBoolValue(this->FGPreserveSwapChain.value_for_config()).c_str());
        target.SetValue("FrameGen", "SkipResizeBuffers",
                     GetBoolValue(this->FGSkipResizeBuffers.value_for_config()).c_str());
        target.SetValue("FrameGen", "ModifyBufferState",
                     GetBoolValue(this->FGModifyBufferState.value_for_config()).c_str());
        target.SetValue("FrameGen", "ModifySCIndex", GetBoolValue(this->FGModifySCIndex.value_for_config()).c_str());
        target.SetValue("FrameGen", "HudCutoff", GetFloatValue(this->FGHudCutoff.value_for_config()).c_str());
    }

    // FSR FG output
    {
        target.SetValue("FSRFG", "DebugTearLines", GetBoolValue(this->FGDebugTearLines.value_for_config()).c_str());
        target.SetValue("FSRFG", "DebugResetLines",
                     GetBoolValue(this->FGDebugResetLines.value_for_config()).c_str());
        target.SetValue("FSRFG", "DebugPacingLines",
                     GetBoolValue(this->FGDebugPacingLines.value_for_config()).c_str());
        target.SetValue("FSRFG", "AllowAsync", GetBoolValue(this->FGAsync.value_for_config()).c_str());
        target.SetValue("FSRFG", "UseMutexForSwapchain",
                     GetBoolValue(this->FGUseMutexForSwapchain.value_for_config()).c_str());
        target.SetValue("FSRFG", "FramePacingTuning",
                     GetBoolValue(this->FGFramePacingTuning.value_for_config()).c_str());
        target.SetValue("FSRFG", "FPTSafetyMarginInMs",
                     GetFloatValue(this->FGFPTSafetyMarginInMs.value_for_config()).c_str());
        target.SetValue("FSRFG", "FPTVarianceFactor",
                     GetFloatValue(this->FGFPTVarianceFactor.value_for_config()).c_str());
        target.SetValue("FSRFG", "FPTHybridSpin",
                     GetBoolValue(this->FGFPTAllowHybridSpin.value_for_config()).c_str());
        target.SetValue("FSRFG", "FPTHybridSpinTime",
                     GetIntValue(this->FGFPTHybridSpinTime.value_for_config()).c_str());
        target.SetValue("FSRFG", "FPTWaitForSingleObjectOnFence",
                     GetBoolValue(this->FGFPTAllowWaitForSingleObjectOnFence.value_for_config()).c_str());
        target.SetValue("FSRFG", "EnableWatermark",
                     GetBoolValue(this->FSRFGEnableWatermark.value_for_config()).c_str());
    }

    // XeFG output
    {
        target.SetValue("XeFG", "InterpolationCount",
                     GetIntValue(this->FGXeFGInterpolationCount.value_for_config()).c_str());
        target.SetValue("XeFG", "IgnoreInitChecks",
                     GetBoolValue(this->FGXeFGIgnoreInitChecks.value_for_config()).c_str());
        target.SetValue("XeFG", "UIComposition", GetBoolValue(this->FGXeFGUIComposition.value_for_config()).c_str());
        target.SetValue("XeFG", "DepthInverted", GetBoolValue(this->FGXeFGDepthInverted.value_for_config()).c_str());
        target.SetValue("XeFG", "JitteredMV", GetBoolValue(this->FGXeFGJitteredMV.value_for_config()).c_str());
        target.SetValue("XeFG", "HighResMV", GetBoolValue(this->FGXeFGHighResMV.value_for_config()).c_str());
        target.SetValue("XeFG", "DebugView", GetBoolValue(this->FGXeFGDebugView.value_for_config()).c_str());
        target.SetValue("XeFG", "ForceBorderless",
                     GetBoolValue(this->FGXeFGForceBorderless.value_for_config()).c_str());
    }

    {
        target.SetValue("DLSSG", "InterpolationCount",
                     GetIntValue(this->FGDLSSGInterpolationCount.value_for_config()).c_str());
        target.SetValue("DLSSG", "ModelPreset",
                     GetIntValue(this->FGDLSSGModelPreset.value_for_config()).c_str());
        target.SetValue("DLSSG", "UseGamesReflexMarkers",
                     GetBoolValue(this->FGDLSSGUseGamesReflexMarkers.value_for_config()).c_str());
        target.SetValue("DLSSG", "OverrideInterpolationCount",
                     GetIntValue(this->FGDLSSGOverrideInterpolationCount.value_for_config()).c_str());
        target.SetValue("DLSSG", "FramerateTargetDMFG",
                     GetFloatValue(this->FGDLSSGFramerateTargetDMFG.value_for_config()).c_str());
        target.SetValue("DLSSG", "OverrideForceDMFG",
                     GetBoolValue(this->FGDLSSGOverrideForceDMFG.value_for_config()).c_str());
        target.SetValue("DLSSG", "ForceDMFG", GetBoolValue(this->FGDLSSGForceDMFG.value_for_config()).c_str());
    }

    // OptiFG
    {
        target.SetValue("OptiFG", "DisableHUDFix", GetBoolValue(this->FGDisableHUDFix.value_for_config()).c_str());
        target.SetValue("OptiFG", "HUDFix", GetBoolValue(this->FGHUDFix.value_for_config()).c_str());
        target.SetValue("OptiFG", "HUDLimit", GetIntValue(this->FGHUDLimit.value_for_config()).c_str());
        target.SetValue("OptiFG", "HUDFixExtended", GetBoolValue(this->FGHUDFixExtended.value_for_config()).c_str());
        target.SetValue("OptiFG", "HUDFixImmediate",
                     GetBoolValue(this->FGImmediateCapture.value_for_config()).c_str());
        target.SetValue("OptiFG", "HUDFixPersistentBindings",
                     GetBoolValue(this->FGHudfixPersistentBindings.value_for_config()).c_str());
        target.SetValue("OptiFG", "UseShards", GetBoolValue(this->FGUseShards.value_for_config()).c_str());
        target.SetValue("OptiFG", "AlwaysTrackHeaps",
                     GetBoolValue(this->FGAlwaysTrackHeaps.value_for_config()).c_str());
        target.SetValue("OptiFG", "ResourceBlocking",
                     GetBoolValue(this->FGResourceBlocking.value_for_config()).c_str());
        target.SetValue("OptiFG", "MakeDepthCopy", GetBoolValue(this->FGMakeDepthCopy.value_for_config()).c_str());
        target.SetValue("OptiFG", "MakeMVCopy", GetBoolValue(this->FGMakeMVCopy.value_for_config()).c_str());

        target.SetValue("OptiFG", "HudfixDisableRTV",
                     GetBoolValue(this->FGHudfixDisableRTV.value_for_config()).c_str());
        target.SetValue("OptiFG", "HudfixDisableSRV",
                     GetBoolValue(this->FGHudfixDisableSRV.value_for_config()).c_str());
        target.SetValue("OptiFG", "HudfixDisableUAV",
                     GetBoolValue(this->FGHudfixDisableUAV.value_for_config()).c_str());
        target.SetValue("OptiFG", "HudfixDisableOM",
                     GetBoolValue(this->FGHudfixDisableOM.value_for_config()).c_str());
        target.SetValue("OptiFG", "HudfixDisableDispatch",
                     GetBoolValue(this->FGHudfixDisableDispatch.value_for_config()).c_str());
        target.SetValue("OptiFG", "HudfixDisableDI",
                     GetBoolValue(this->FGHudfixDisableDI.value_for_config()).c_str());
        target.SetValue("OptiFG", "HudfixDisableDII",
                     GetBoolValue(this->FGHudfixDisableDII.value_for_config()).c_str());
        target.SetValue("OptiFG", "HudfixDisableSCR",
                     GetBoolValue(this->FGHudfixDisableSCR.value_for_config()).c_str());
        target.SetValue("OptiFG", "HudfixDisableSGR",
                     GetBoolValue(this->FGHudfixDisableSGR.value_for_config()).c_str());

        target.SetValue("OptiFG", "EnableDepthScale",
                     GetBoolValue(this->FGEnableDepthScale.value_for_config()).c_str());
        target.SetValue("OptiFG", "DepthScaleMax", GetFloatValue(this->FGDepthScaleMax.value_for_config()).c_str());

        target.SetValue("OptiFG", "HUDFixDontUseSwapchainBuffers",
                     GetBoolValue(this->FGDontUseSwapchainBuffers.value_for_config()).c_str());
        target.SetValue("OptiFG", "HUDFixRelaxedResolutionCheck",
                     GetBoolValue(this->FGRelaxedResolutionCheck.value_for_config()).c_str());
        target.SetValue("OptiFG", "ResourceFlip", GetBoolValue(this->FGResourceFlip.value_for_config()).c_str());
        target.SetValue("OptiFG", "ResourceFlipOffset",
                     GetBoolValue(this->FGResourceFlipOffset.value_for_config()).c_str());
        target.SetValue("OptiFG", "DepthTextureFlipY", GetBoolValue(this->FGDepthTextureFlipY.value_for_config()).c_str());
        target.SetValue("OptiFG", "MotionTextureFlipY", GetBoolValue(this->FGMotionTextureFlipY.value_for_config()).c_str());
        target.SetValue("OptiFG", "MotionYInverted", GetBoolValue(this->FGMotionYInverted.value_for_config()).c_str());

        target.SetValue("OptiFG", "AlwaysCaptureFSRFGSwapchain",
                     GetBoolValue(this->FGAlwaysCaptureFSRFGSwapchain.value_for_config()).c_str());
    }

    // FSR FG Inputs
    {
        target.SetValue("FSRFGInputs", "SkipConfigForHudless",
                     GetBoolValue(this->FSRFGSkipConfigForHudless.value_for_config()).c_str());
        target.SetValue("FSRFGInputs", "SkipDispatchForHudless",
                     GetBoolValue(this->FSRFGSkipDispatchForHudless.value_for_config()).c_str());
    }

    // Framerate
    {
        target.SetValue("Framerate", "FramerateLimit",
                     GetFloatValue(this->FramerateLimit.value_for_config()).c_str());
    }

    // Output Scaling
    {
        target.SetValue("OutputScaling", "Enabled",
                     GetBoolValue(this->OutputScalingEnabled.value_for_config()).c_str());
        target.SetValue("OutputScaling", "Multiplier",
                     GetFloatValue(this->OutputScalingMultiplier.value_for_config()).c_str());
        target.SetValue("OutputScaling", "Downscaler", GetIntValue(this->OutputScalingDownscaler.value_for_config()).c_str());
    }

    // FSR common
    {
        target.SetValue("FSR", "VerticalFov", GetFloatValue(this->FsrVerticalFov.value_for_config()).c_str());
        target.SetValue("FSR", "HorizontalFov", GetFloatValue(this->FsrHorizontalFov.value_for_config()).c_str());
        target.SetValue("FSR", "CameraNear", GetFloatValue(this->FsrCameraNear.value_for_config()).c_str());
        target.SetValue("FSR", "CameraFar", GetFloatValue(this->FsrCameraFar.value_for_config()).c_str());
        target.SetValue("FSR", "UseFsrInputValues",
                     GetBoolValue(this->FsrUseFsrInputValues.value_for_config()).c_str());
    }

    // FSR
    {
        target.SetValue("FSR", "VelocityFactor", GetFloatValue(this->FsrVelocity.value_for_config()).c_str());
        target.SetValue("FSR", "ReactiveScale", GetFloatValue(this->FsrReactiveScale.value_for_config()).c_str());
        target.SetValue("FSR", "ShadingScale", GetFloatValue(this->FsrShadingScale.value_for_config()).c_str());
        target.SetValue("FSR", "AccAddPerFrame", GetFloatValue(this->FsrAccAddPerFrame.value_for_config()).c_str());
        target.SetValue("FSR", "MinDisOccAcc", GetFloatValue(this->FsrMinDisOccAcc.value_for_config()).c_str());
        target.SetValue("FSR", "DebugView", GetBoolValue(this->FsrDebugView.value_for_config()).c_str());
        target.SetValue("FSR", "UpscalerIndex", GetIntValue(this->FfxUpscalerIndex.value_for_config()).c_str());
        target.SetValue("FSR", "FGIndex", GetIntValue(this->FfxFGIndex.value_for_config()).c_str());
        target.SetValue("FSR", "UseReactiveMaskForTransparency",
                     GetBoolValue(this->FsrUseMaskForTransparency.value_for_config()).c_str());
        target.SetValue("FSR", "DlssReactiveMaskBias",
                     GetFloatValue(this->DlssReactiveMaskBias.value_for_config()).c_str());
        target.SetValue("FSR", "Fsr4ForceModel", GetIntValue(this->Fsr4ForceModel.value_for_config()).c_str());
        target.SetValue("FSR", "Fsr4Preset", GetIntValue(this->Fsr4Preset.value_for_config()).c_str());
        target.SetValue("FSR", "Fsr4EnableWatermark",
                     GetBoolValue(this->Fsr4EnableWatermark.value_for_config()).c_str());
        target.SetValue("FSR", "Fsr4DoNotLoadAmdxc64",
                     GetBoolValue(this->Fsr4DoNotLoadAmdxc64.value_for_config()).c_str());
        target.SetValue("FSR", "FsrNonLinearColorSpace",
                     GetBoolValue(this->FsrNonLinearColorSpace.value_for_config()).c_str());
        target.SetValue("FSR", "FsrNonLinearPQ", GetBoolValue(this->FsrNonLinearPQ.value_for_config()).c_str());
        target.SetValue("FSR", "FsrNonLinearSRGB", GetBoolValue(this->FsrNonLinearSRGB.value_for_config()).c_str());
        target.SetValue("FSR", "FsrAgilitySDKUpgrade",
                     GetBoolValue(this->FsrAgilitySDKUpgrade.value_for_config()).c_str());
    }

    // XeSS
    {
        target.SetValue("XeSS", "BuildPipelines", GetBoolValue(this->BuildPipelines.value_for_config()).c_str());
        target.SetValue("XeSS", "CreateHeaps", GetBoolValue(this->CreateHeaps.value_for_config()).c_str());
        target.SetValue("XeSS", "NetworkModel", GetIntValue(this->NetworkModel.value_for_config()).c_str());
    }

    // DLSS
    {
        target.SetValue("DLSS", "Enabled", GetBoolValue(this->DLSSEnabled.value_for_config()).c_str());

    // --- DLSS 5 Neural Rendering (OptiScaler/dlssnr) ---
    target.SetValue("DlssNr", "Enabled", GetBoolValue(this->DlssNrEnabled.value_for_config()).c_str());
    target.SetValue("DlssNr", "StarRailForceResident", GetBoolValue(this->StarRailForceNRResident.value_for_config()).c_str());
    target.SetValue("DlssNr", "FinishedPicture", GetBoolValue(this->DlssNrFinishedPicture.value_for_config()).c_str());
    target.SetValue("DlssNr", "GenshinPresentPipeline", GetBoolValue(this->DlssNrGenshinPresentPipeline.value_for_config()).c_str());
    target.SetValue("DlssNr", "RunBeforeSR",
                 GetBoolValue(this->DlssNrRunBeforeSr.value_for_config()).c_str());
    target.SetValue("DlssNr", "DeferredDLSS",
                 GetBoolValue(this->DlssNrDeferredDlss.value_for_config()).c_str());
    target.SetValue("DlssNr", "Precision", GetIntValue(this->DlssNrPrecision.value_for_config()).c_str());
    target.SetValue("DlssNr", "VitEvery", GetIntValue(this->DlssNrVitEvery.value_for_config()).c_str());
    target.SetValue("DlssNr", "VitEveryPlain", GetIntValue(this->DlssNrVitEveryPlain.value_for_config()).c_str());
    target.SetValue("DlssNr", "DetailReuse", GetBoolValue(this->DlssNrDetailReuse.value_for_config()).c_str());
    target.SetValue("DlssNr", "DetailReuseDebug", GetBoolValue(this->DlssNrDetailReuseDebug.value_for_config()).c_str());
    target.SetValue("DlssNr", "DetailReuseSteady", GetFloatValue(this->DlssNrDetailReuseSteady.value_for_config()).c_str());
    target.SetValue("DlssNr", "PresentedTemporalDetail", GetBoolValue(this->DlssNrPresentedTemporalDetail.value_for_config()).c_str());
    target.SetValue("DlssNr", "PresentedTemporalDurationMs", GetIntValue(this->DlssNrPresentedTemporalDurationMs.value_for_config()).c_str());
    target.SetValue("DlssNr", "PresentedTemporalMaxAgeMs", GetIntValue(this->DlssNrPresentedTemporalMaxAgeMs.value_for_config()).c_str());
    target.SetValue("DlssNr", "PresentedTemporalStrength", GetFloatValue(this->DlssNrPresentedTemporalStrength.value_for_config()).c_str());
    target.SetValue("DlssNr", "PresentedTemporalDepthTolerance", GetFloatValue(this->DlssNrPresentedTemporalDepthTolerance.value_for_config()).c_str());
    target.SetValue("DlssNr", "PresentedTemporalColourTolerance", GetFloatValue(this->DlssNrPresentedTemporalColourTolerance.value_for_config()).c_str());
    target.SetValue("DlssNr", "PresentedTemporalMotionTolerance", GetFloatValue(this->DlssNrPresentedTemporalMotionTolerance.value_for_config()).c_str());
    target.SetValue("DlssNr", "PresentedTemporalNoiseFloor", GetFloatValue(this->DlssNrPresentedTemporalNoiseFloor.value_for_config()).c_str());
    target.SetValue("DlssNr", "PresentedTemporalStabilityMs", GetIntValue(this->DlssNrPresentedTemporalStabilityMs.value_for_config()).c_str());
    target.SetValue("DlssNr", "PresentedTemporalStabilityFloor", GetFloatValue(this->DlssNrPresentedTemporalStabilityFloor.value_for_config()).c_str());
    target.SetValue("DlssNr", "PresentedTemporalStabilityClamp", GetFloatValue(this->DlssNrPresentedTemporalStabilityClamp.value_for_config()).c_str());
    target.SetValue("DlssNr", "MotionYInverted", GetBoolValue(this->DlssNrMotionYInverted.value_for_config()).c_str());
    target.SetValue("DlssNr", "DepthTextureFlipY", GetBoolValue(this->DlssNrDepthTextureFlipY.value_for_config()).c_str());
    target.SetValue("DlssNr", "MotionTextureFlipY", GetBoolValue(this->DlssNrMotionTextureFlipY.value_for_config()).c_str());
    target.SetValue("DlssNr", "GameMotionReuse", GetBoolValue(this->DlssNrGameMotionReuse.value_for_config()).c_str());
    target.SetValue("DlssNr", "DetailReuseInterval", GetIntValue(this->DlssNrDetailReuseInterval.value_for_config()).c_str());
    target.SetValue("DlssNr", "DetailReuseFgInterval", GetIntValue(this->DlssNrDetailReuseFgInterval.value_for_config()).c_str());

    if (this->IsStarRailInputProfile())
    {
        target.SetValue("SR_input", "MotionYInverted", GetBoolValue(this->StarRailInputMotionYInverted.value_for_config_or(true)).c_str());
        target.SetValue("SR_input", "DepthTextureFlipY", GetBoolValue(this->StarRailInputDepthTextureFlipY.value_for_config_or(true)).c_str());
        target.SetValue("SR_input", "MotionTextureFlipY", GetBoolValue(this->StarRailInputMotionTextureFlipY.value_for_config_or(true)).c_str());
        target.SetValue("SR_input", "OriginalMotionYInverted", GetBoolValue(this->_starRailOriginalMotionY).c_str());
        target.SetValue("SR_input", "OriginalDepthTextureFlipY", GetBoolValue(this->_starRailOriginalDepthFlipY).c_str());
        target.SetValue("SR_input", "OriginalMotionTextureFlipY", GetBoolValue(this->_starRailOriginalMotionFlipY).c_str());
    }
    target.SetValue("DlssNr", "DetailReuseSpread", GetIntValue(this->DlssNrDetailReuseSpread.value_for_config()).c_str());
    target.SetValue("DlssNr", "DetailReuseFill", GetFloatValue(this->DlssNrDetailReuseFill.value_for_config()).c_str());
    target.SetValue("DlssNr", "DetailReuseDepthTolerance", GetFloatValue(this->DlssNrDetailReuseDepthTolerance.value_for_config()).c_str());
    target.SetValue("DlssNr", "DetailReuseClipGamma", GetFloatValue(this->DlssNrDetailReuseClipGamma.value_for_config()).c_str());
    target.SetValue("DlssNr", "DetailReuseClipFalloff", GetFloatValue(this->DlssNrDetailReuseClipFalloff.value_for_config()).c_str());
    target.SetValue("DlssNr", "DetailReuseSigmaFloor", GetFloatValue(this->DlssNrDetailReuseSigmaFloor.value_for_config()).c_str());
    target.SetValue("DlssNr", "DetailReuseWithFG", GetBoolValue(this->DlssNrDetailReuseWithFg.value_for_config()).c_str());
    target.SetValue("DlssNr", "SceneCut", GetIntValue(this->DlssNrSceneCut.value_for_config()).c_str());
    target.SetValue("DlssNr", "DetailReuseMinFps", GetFloatValue(this->DlssNrDetailReuseMinFps.value_for_config()).c_str());
    target.SetValue("DlssNr", "DetailReuseMaxDropped", GetFloatValue(this->DlssNrDetailReuseMaxDropped.value_for_config()).c_str());
    {
        auto toggle = this->DlssNrToggleKey.value_for_config();
        target.SetValue("DlssNr", "ToggleKey", GetIntValue(toggle, toggle > 0).c_str());
    }
    target.SetValue("DlssNr", "TransferStrength",
                 GetFloatValue(this->DlssNrTransferStrength.value_for_config()).c_str());
    target.SetValue("DlssNr", "ColourStrength",
                 GetFloatValue(this->DlssNrColourStrength.value_for_config()).c_str());
    // Preserve legacy enable provenance; the consumer no longer uses this key as a gate.
    target.SetValue("DlssNr", "LookMode", GetBoolValue(this->DlssNrLookMode.value_for_config()).c_str());
    target.SetValue("DlssNr", "LookStrength", GetFloatValue(this->DlssNrLookStrength.value_for_config()).c_str());
    target.SetValue("DlssNr", "LookBrighten", GetFloatValue(this->DlssNrLookBrighten.value_for_config()).c_str());
    target.SetValue("DlssNr", "LookDarken", GetFloatValue(this->DlssNrLookDarken.value_for_config()).c_str());
    target.SetValue("DlssNr", "LookColour", GetFloatValue(this->DlssNrLookColour.value_for_config()).c_str());
    target.SetValue("DlssNr", "LookHue", GetFloatValue(this->DlssNrLookHue.value_for_config()).c_str());
    target.SetValue("DlssNr", "LookShadows", GetFloatValue(this->DlssNrLookShadows.value_for_config()).c_str());
    target.SetValue("DlssNr", "LookMidtones", GetFloatValue(this->DlssNrLookMidtones.value_for_config()).c_str());
    target.SetValue("DlssNr", "LookHighlights", GetFloatValue(this->DlssNrLookHighlights.value_for_config()).c_str());
    target.SetValue("DlssNr", "LookMaxBrighten", GetFloatValue(this->DlssNrLookMaxBrighten.value_for_config()).c_str());
    target.SetValue("DlssNr", "LookMaxDarken", GetFloatValue(this->DlssNrLookMaxDarken.value_for_config()).c_str());
    target.SetValue("DlssNr", "LookMaxColour", GetFloatValue(this->DlssNrLookMaxColour.value_for_config()).c_str());
    target.SetValue("DlssNr", "LookTone", GetFloatValue(this->DlssNrLookTone.value_for_config()).c_str());
    target.SetValue("DlssNr", "LookDetail", GetFloatValue(this->DlssNrLookDetail.value_for_config()).c_str());
    target.SetValue("DlssNr", "LookDetailRadius", GetFloatValue(this->DlssNrLookDetailRadius.value_for_config()).c_str());
    target.SetValue("DlssNr", "LookHalo", GetFloatValue(this->DlssNrLookHalo.value_for_config()).c_str());
    target.SetValue("DlssNr", "LookHaloBrightSide", GetFloatValue(this->DlssNrLookHaloBrightSide.value_for_config()).c_str());
    DlssNr::LayerLook::Save(*this,
        [&](const std::string& key, std::optional<bool> value) { target.SetValue("DlssNr", key.c_str(), GetBoolValue(value).c_str()); },
        [&](const std::string& key, std::optional<uint32_t> value) { target.SetValue("DlssNr", key.c_str(), GetIntValue(value).c_str()); },
        [&](const std::string& key, std::optional<float> value) { target.SetValue("DlssNr", key.c_str(), GetFloatValue(value).c_str()); });
    target.SetValue("DlssNr", "ReplaceDetailStrength",
                 GetFloatValue(this->DlssNrReplaceDetailStrength.value_for_config()).c_str());
    target.SetValue("DlssNr", "MaxRatio", GetFloatValue(this->DlssNrMaxRatio.value_for_config()).c_str());
    target.SetValue("DlssNr", "Transfer", GetIntValue(this->DlssNrTransfer.value_for_config()).c_str());

    target.SetValue("DlssNr", "ProbeD3D11",
                 GetBoolValue(this->DlssNrProbeD3D11.value_for_config()).c_str());
    target.SetValue("DlssNr", "FrameStats",
                 GetBoolValue(this->DlssNrFrameStats.value_for_config()).c_str());
    target.SetValue("DlssNr", "KernelProfile",
                 GetBoolValue(this->DlssNrKernelProfile.value_for_config()).c_str());
    target.SetValue("DlssNr", "WhitePointFromExposure",
                 GetBoolValue(this->DlssNrWhitePointFromExposure.value_for_config()).c_str());
    target.SetValue("DlssNr", "DebugView", GetIntValue(this->DlssNrDebugView.value_for_config()).c_str());
    target.SetValue("DlssNr", "GuidePreview", GetIntValue(this->DlssNrGuidePreview.value_for_config()).c_str());
    target.SetValue("DlssNr", "GuidePreviewRaw", GetBoolValue(this->DlssNrGuidePreviewRaw.value_for_config()).c_str());
    target.SetValue("DlssNr", "GuidePreviewFlipY", GetBoolValue(this->DlssNrGuidePreviewFlipY.value_for_config()).c_str());
    target.SetValue("DlssNr", "GuidePreviewMotionGain", GetFloatValue(this->DlssNrGuidePreviewMotionGain.value_for_config()).c_str());
    target.SetValue("DlssNr", "Compare", GetIntValue(this->DlssNrCompare.value_for_config()).c_str());
    target.SetValue("DlssNr", "CompareSplit",
                 GetFloatValue(this->DlssNrCompareSplit.value_for_config()).c_str());
    target.SetValue("DlssNr", "CompareZoom",
                 GetFloatValue(this->DlssNrCompareZoom.value_for_config()).c_str());
    target.SetValue("DlssNr", "CompareSwap",
                 GetBoolValue(this->DlssNrCompareSwap.value_for_config()).c_str());
    target.SetValue("DlssNr", "CompareTags",
                 GetBoolValue(this->DlssNrCompareTags.value_for_config()).c_str());
    target.SetValue("DlssNr", "TagScale",
                 GetFloatValue(this->DlssNrTagScale.value_for_config()).c_str());
    target.SetValue("DlssNr", "WorkingScale", GetFloatValue(this->DlssNrWorkingScale.value_for_config()).c_str());
    target.SetValue("DlssNr", "ReducedUpscaleMethod",
                 GetIntValue(this->DlssNrReducedUpscaleMethod.value_for_config()).c_str());
    target.SetValue("DlssNr", "ModelResolutionAuto",
                 GetBoolValue(this->DlssNrModelResolutionAuto.value_for_config()).c_str());
    target.SetValue("DlssNr", "SpatialCompression",
                 GetBoolValue(this->DlssNrSpatialCompression.value_for_config()).c_str());
    target.SetValue("DlssNr", "SpatialCenterX", GetFloatValue(this->DlssNrSpatialCenterX.value_for_config()).c_str());
    target.SetValue("DlssNr", "SpatialCenterY", GetFloatValue(this->DlssNrSpatialCenterY.value_for_config()).c_str());
    target.SetValue("DlssNr", "SpatialWorkX", GetFloatValue(this->DlssNrSpatialWorkX.value_for_config()).c_str());
    target.SetValue("DlssNr", "SpatialWorkY", GetFloatValue(this->DlssNrSpatialWorkY.value_for_config()).c_str());
    target.SetValue("DlssNr", "SpatialOffsetX", GetFloatValue(this->DlssNrSpatialOffsetX.value_for_config()).c_str());
    target.SetValue("DlssNr", "SpatialOffsetY", GetFloatValue(this->DlssNrSpatialOffsetY.value_for_config()).c_str());
    target.SetValue("DlssNr", "SpatialShiftX", GetFloatValue(this->DlssNrSpatialShiftX.value_for_config()).c_str());
    target.SetValue("DlssNr", "SpatialShiftY", GetFloatValue(this->DlssNrSpatialShiftY.value_for_config()).c_str());
    target.SetValue("DlssNr", "ScalingDownscaler", GetIntValue(this->DlssNrScalingDownscaler.value_for_config()).c_str());
    target.SetValue("DlssNr", "AutoCapture", GetBoolValue(this->DlssNrAutoCapture.value_for_config()).c_str());

    // These were read every launch but never written, so nothing set through the menu survived a
    // restart -- the white-point source, both trims, the anchor, the pass count and the rest all
    // reset to default on the next run.
    target.SetValue("DlssNr", "WhitePointSource", GetIntValue(this->DlssNrWhitePointSource.value_for_config()).c_str());
    target.SetValue("DlssNr", "GameExposureScale",
                 GetIntValue(this->DlssNrGameExposureScale.value_for_config()).c_str());
    target.SetValue("DlssNr", "WhitePointTrim", GetFloatValue(this->DlssNrWhitePointTrim.value_for_config()).c_str());
    target.SetValue("DlssNr", "AutoExposureTrim", GetFloatValue(this->DlssNrAutoExposureTrim.value_for_config()).c_str());
    target.SetValue("DlssNr", "AutoExposureShadowProtection",
                 GetFloatValue(this->DlssNrAutoExposureShadowProtection.value_for_config()).c_str());
    // The one speed both used to share is only read (and copied into both); drop it so a choice equal to a default is not
    // overridden by it on the next load.
    target.Delete("DlssNr", "AutoExposureAdaptSeconds");
    target.SetValue("DlssNr", "AutoExposureMeter", GetIntValue(this->DlssNrAutoExposureMeter.value_for_config()).c_str());
    target.SetValue("DlssNr", "AutoExposureMeterLowPercent",
                 GetFloatValue(this->DlssNrAutoExposureMeterLowPercent.value_for_config()).c_str());
    target.SetValue("DlssNr", "AutoExposureMeterHighPercent",
                 GetFloatValue(this->DlssNrAutoExposureMeterHighPercent.value_for_config()).c_str());
    target.SetValue("DlssNr", "NativeDepthFinder",
                 GetBoolValue(this->DlssNrNativeDepthFinder.value_for_config()).c_str());
    target.SetValue("DlssNr", "NativeDepthVkCopyUsage",
                 GetBoolValue(this->DlssNrNativeDepthVkCopyUsage.value_for_config()).c_str());
    target.SetValue("DlssNr", "NativeDxvkVulkan", GetBoolValue(this->DlssNrNativeDxvkVulkan.value_for_config()).c_str());
    target.SetValue("DlssNr", "NativeDepthWarmupFrames",
                 GetIntValue(this->DlssNrNativeDepthWarmupFrames.value_for_config()).c_str());
    target.SetValue("DlssNr", "NativeDepthOverlay",
                 GetBoolValue(this->DlssNrNativeDepthOverlay.value_for_config()).c_str());
    target.SetValue("DlssNr", "NativeDebugView",
                 GetBoolValue(this->DlssNrNativeDebugView.value_for_config()).c_str());
    target.SetValue("DlssNr", "NativeMotion", GetBoolValue(this->DlssNrNativeMotion.value_for_config()).c_str());
    target.SetValue("DlssNr", "OpticalFgNr", GetBoolValue(this->DlssNrOpticalFgNr.value_for_config()).c_str());
    target.SetValue("DlssNr", "NativeInput", GetBoolValue(this->DlssNrNativeInput.value_for_config()).c_str());
    target.SetValue("DlssNr", "NativeUpscaler",
                 GetBoolValue(this->DlssNrNativeUpscaler.value_for_config()).c_str());
    target.SetValue("DlssNr", "NativeFrameGenerationOnly",
                 GetBoolValue(this->DlssNrNativeFrameGenerationOnly.value_for_config()).c_str());
    {
        const char* lowLatency = "auto";

        if (auto held = this->DlssNrNativeLowLatency.value_for_config(); held.has_value())
            lowLatency = held.value() == native::lowlatency::Setting::On    ? "true"
                         : held.value() == native::lowlatency::Setting::Off ? "false"
                                                                            : "auto";

        target.SetValue("DlssNr", "NativeLowLatency", lowLatency);
    }
    target.SetValue("DlssNr", "NativeLowLatencyBoost",
                 GetBoolValue(this->DlssNrNativeLowLatencyBoost.value_for_config()).c_str());
    target.SetValue("DlssNr", "AutoExposureAdaptBrighterSeconds",
                 GetFloatValue(this->DlssNrAutoExposureAdaptBrighterSeconds.value_for_config()).c_str());
    target.SetValue("DlssNr", "AutoExposureAdaptDarkerSeconds",
                 GetFloatValue(this->DlssNrAutoExposureAdaptDarkerSeconds.value_for_config()).c_str());
    target.SetValue("DlssNr", "AutoExposureFollowGame",
                 GetBoolValue(this->DlssNrAutoExposureFollowGame.value_for_config()).c_str());
    target.SetValue("DlssNr", "GameExposureTrimAnchors",
                 this->DlssNrGameExposureTrimAnchors.value_for_config_or("").c_str());
    target.SetValue("DlssNr", "AutoExposureTrimAnchors",
                 this->DlssNrAutoExposureTrimAnchors.value_for_config_or("").c_str());
    target.SetValue("DlssNr", "ScanTrim", GetFloatValue(this->DlssNrScanTrim.value_for_config()).c_str());
    target.SetValue("DlssNr", "ScanAnchorValue", GetFloatValue(this->DlssNrScanAnchorValue.value_for_config()).c_str());
    target.SetValue("DlssNr", "ScanAnchorWhitePoint", GetFloatValue(this->DlssNrScanAnchorWhitePoint.value_for_config()).c_str());
    target.SetValue("DlssNr", "ScanAnchors", this->DlssNrScanAnchors.value_for_config_or("").c_str());
    target.SetValue("DlssNr", "ScanInverted", GetBoolValue(this->DlssNrScanInverted.value_for_config()).c_str());
    target.SetValue("DlssNr", "ScanMeter", GetBoolValue(this->DlssNrScanMeter.value_for_config()).c_str());
    target.SetValue("DlssNr", "Passes", GetIntValue(this->DlssNrPasses.value_for_config()).c_str());
    target.SetValue("DlssNr", "PassFeedback", GetFloatValue(this->DlssNrPassFeedback.value_for_config()).c_str());
    target.SetValue("DlssNr", "UseProxy", GetBoolValue(this->DlssNrUseProxy.value_for_config()).c_str());
    target.SetValue("DlssNr", "ProxyProbe", GetBoolValue(this->DlssNrProxyProbe.value_for_config()).c_str());
    // ScanExposure is a developer override with no menu control; persist it so a set ini keeps it.
    target.SetValue("DlssNr", "ScanExposure", GetBoolValue(this->DlssNrScanExposure.value_for_config()).c_str());
    target.SetValue("DlssNr", "WhitePointScale",
                 GetFloatValue(this->DlssNrWhitePointScale.value_for_config()).c_str());
    target.SetValue("DlssNr", "Preset", GetIntValue(this->DlssNrPreset.value_for_config()).c_str());
    target.SetValue("DlssNr", "Intensity", GetFloatValue(this->DlssNrIntensity.value_for_config()).c_str());
    target.SetValue("DlssNr", "Style", GetIntValue(this->DlssNrStyle.value_for_config()).c_str());
    target.SetValue("DlssNr", "Pass2Preset",
                 GetIntValue(this->DlssNrPass2Preset.value_for_config()).c_str());
    target.SetValue("DlssNr", "Pass2Style",
                 GetIntValue(this->DlssNrPass2Style.value_for_config()).c_str());
    target.SetValue("DlssNr", "Pass3Preset",
                 GetIntValue(this->DlssNrPass3Preset.value_for_config()).c_str());
    target.SetValue("DlssNr", "Pass3Style",
                 GetIntValue(this->DlssNrPass3Style.value_for_config()).c_str());
    target.SetValue("DlssNr", "LocalStructure",
                 GetFloatValue(this->DlssNrLocalStructure.value_for_config()).c_str());
    target.SetValue("DlssNr", "LocalTone", GetFloatValue(this->DlssNrLocalTone.value_for_config()).c_str());
    target.SetValue("DlssNr", "SkinStructure",
                 GetFloatValue(this->DlssNrSkinStructure.value_for_config()).c_str());
    target.SetValue("DlssNr", "AutoMask", GetBoolValue(this->DlssNrAutoMask.value_for_config()).c_str());
    target.SetValue("DlssNr", "SkinProtection", GetBoolValue(this->DlssNrSkinProtection.value_for_config()).c_str());
    target.SetValue("DlssNr", "SkinToneEnabled", GetBoolValue(this->DlssNrSkinToneEnabled.value_for_config()).c_str());
    target.SetValue("DlssNr", "SkinDetail", GetFloatValue(this->DlssNrSkinDetail.value_for_config()).c_str());
    target.SetValue("DlssNr", "SkinColour", GetFloatValue(this->DlssNrSkinColour.value_for_config()).c_str());
    target.SetValue("DlssNr", "EnvironmentDetail", GetFloatValue(this->DlssNrEnvironmentDetail.value_for_config()).c_str());
    target.SetValue("DlssNr", "EnvironmentColour", GetFloatValue(this->DlssNrEnvironmentColour.value_for_config()).c_str());
    target.SetValue("DlssNr", "ShowSkinMask", GetBoolValue(this->DlssNrShowSkinMask.value_for_config()).c_str());
    target.SetValue("DlssNr", "Pass2Intensity", GetFloatValue(this->DlssNrPass2Intensity.value_for_config()).c_str());
    target.SetValue("DlssNr", "Pass2LocalStructure", GetFloatValue(this->DlssNrPass2LocalStructure.value_for_config()).c_str());
    target.SetValue("DlssNr", "Pass2LocalTone", GetFloatValue(this->DlssNrPass2LocalTone.value_for_config()).c_str());
    target.SetValue("DlssNr", "Pass2SkinStructure", GetFloatValue(this->DlssNrPass2SkinStructure.value_for_config()).c_str());
    target.SetValue("DlssNr", "Pass2AutoMask", GetBoolValue(this->DlssNrPass2AutoMask.value_for_config()).c_str());
    target.SetValue("DlssNr", "Pass3Intensity", GetFloatValue(this->DlssNrPass3Intensity.value_for_config()).c_str());
    target.SetValue("DlssNr", "Pass3LocalStructure", GetFloatValue(this->DlssNrPass3LocalStructure.value_for_config()).c_str());
    target.SetValue("DlssNr", "Pass3LocalTone", GetFloatValue(this->DlssNrPass3LocalTone.value_for_config()).c_str());
    target.SetValue("DlssNr", "Pass3SkinStructure", GetFloatValue(this->DlssNrPass3SkinStructure.value_for_config()).c_str());
    target.SetValue("DlssNr", "Pass3AutoMask", GetBoolValue(this->DlssNrPass3AutoMask.value_for_config()).c_str());
    for (unsigned int i = 0; i < DlssNr::ExtraPassCount; ++i)
    {
        auto& pass = this->DlssNrExtraPasses[i];
        target.SetValue("DlssNr", std::format("Pass{}Preset", i + 4).c_str(), GetIntValue(pass.preset.value_for_config()).c_str());
        target.SetValue("DlssNr", std::format("Pass{}Style", i + 4).c_str(), GetIntValue(pass.style.value_for_config()).c_str());
        target.SetValue("DlssNr", std::format("Pass{}Intensity", i + 4).c_str(), GetFloatValue(pass.intensity.value_for_config()).c_str());
        target.SetValue("DlssNr", std::format("Pass{}LocalStructure", i + 4).c_str(), GetFloatValue(pass.structure.value_for_config()).c_str());
        target.SetValue("DlssNr", std::format("Pass{}LocalTone", i + 4).c_str(), GetFloatValue(pass.tone.value_for_config()).c_str());
        target.SetValue("DlssNr", std::format("Pass{}SkinStructure", i + 4).c_str(), GetFloatValue(pass.skin.value_for_config()).c_str());
        target.SetValue("DlssNr", std::format("Pass{}AutoMask", i + 4).c_str(), GetBoolValue(pass.autoMask.value_for_config()).c_str());
    }
    target.SetValue("DlssNr", "ReversibleMode", GetIntValue(this->DlssNrReversibleMode.value_for_config()).c_str());
    target.SetValue("DlssNr", "ColourEncoding", GetIntValue(this->DlssNrColourEncoding.value_for_config()).c_str());
    target.SetValue("DlssNr", "LutFile", this->DlssNrLutFile.value_for_config_or("").c_str());
    target.SetValue("DlssNr", "LutStrength", GetFloatValue(this->DlssNrLutStrength.value_for_config()).c_str());
    target.SetValue("DlssNr", "ApplyModel", GetBoolValue(this->DlssNrApplyModel.value_for_config()).c_str());
    target.SetValue("DlssNr", "HoldFrame", GetBoolValue(this->DlssNrHoldFrame.value_for_config()).c_str());
        target.SetValue("DLSS", "RenderPresetOverride",
                     GetBoolValue(this->RenderPresetOverride.value_for_config()).c_str());
        target.SetValue("DLSS", "RenderPresetForAll",
                     GetIntValue(this->RenderPresetForAll.value_for_config()).c_str());
        target.SetValue("DLSS", "RenderPresetDLAA", GetIntValue(this->RenderPresetDLAA.value_for_config()).c_str());
        target.SetValue("DLSS", "RenderPresetUltraQuality",
                     GetIntValue(this->RenderPresetUltraQuality.value_for_config()).c_str());
        target.SetValue("DLSS", "RenderPresetQuality",
                     GetIntValue(this->RenderPresetQuality.value_for_config()).c_str());
        target.SetValue("DLSS", "RenderPresetBalanced",
                     GetIntValue(this->RenderPresetBalanced.value_for_config()).c_str());
        target.SetValue("DLSS", "RenderPresetPerformance",
                     GetIntValue(this->RenderPresetPerformance.value_for_config()).c_str());
        target.SetValue("DLSS", "RenderPresetUltraPerformance",
                     GetIntValue(this->RenderPresetUltraPerformance.value_for_config()).c_str());
        target.SetValue("DLSS", "UseGenericAppIdWithDlss",
                     GetBoolValue(this->UseGenericAppIdWithDlss.value_for_config()).c_str());
    }

    // DLSSD
    {
        target.SetValue("DLSSD", "RenderPresetOverride",
                     GetBoolValue(this->DLSSDRenderPresetOverride.value_for_config()).c_str());
        target.SetValue("DLSSD", "RenderPresetForAll",
                     GetIntValue(this->DLSSDRenderPresetForAll.value_for_config()).c_str());
        target.SetValue("DLSSD", "RenderPresetDLAA",
                     GetIntValue(this->DLSSDRenderPresetDLAA.value_for_config()).c_str());
        target.SetValue("DLSSD", "RenderPresetUltraQuality",
                     GetIntValue(this->DLSSDRenderPresetUltraQuality.value_for_config()).c_str());
        target.SetValue("DLSSD", "RenderPresetQuality",
                     GetIntValue(this->DLSSDRenderPresetQuality.value_for_config()).c_str());
        target.SetValue("DLSSD", "RenderPresetBalanced",
                     GetIntValue(this->DLSSDRenderPresetBalanced.value_for_config()).c_str());
        target.SetValue("DLSSD", "RenderPresetPerformance",
                     GetIntValue(this->DLSSDRenderPresetPerformance.value_for_config()).c_str());
        target.SetValue("DLSSD", "RenderPresetUltraPerformance",
                     GetIntValue(this->DLSSDRenderPresetUltraPerformance.value_for_config()).c_str());
    }

    // NvngxFG
    {
        target.SetValue("NvngxFG", "MakeDepthCopy",
                     GetBoolValue(this->NvngxFGMakeDepthCopy.value_for_config()).c_str());
        target.SetValue("NvngxFG", "DispatchFlags",
                     GetIntValue(this->NvngxFGDispatchFlags.value_for_config(), true).c_str());
        target.SetValue("NvngxFG", "ShowDebug", GetBoolValue(this->NvngxFGShowDebug.value_for_config()).c_str());
        target.SetValue("NvngxFG", "DisableHudless",
                     GetBoolValue(this->NvngxFGDisableHudless.value_for_config()).c_str());
    }

    // Sharpness
    {
        std::string shader = SharpnessShader.value_for_config()
                                 .transform(SharpnessShaderToCode) // Turn enum into string
                                 .value_or("auto");

        target.SetValue("Sharpness", "Shader", shader.c_str());

        target.SetValue("Sharpness", "OverrideSharpness",
                     GetBoolValue(this->OverrideSharpness.value_for_config()).c_str());
        target.SetValue("Sharpness", "Sharpness", GetFloatValue(this->Sharpness.value_for_config()).c_str());
    }

    // CAS
    {
        target.SetValue("CAS", "Enabled", GetBoolValue(this->RcasEnabled.value_for_config()).c_str());

        target.SetValue("CAS", "MotionSharpnessEnabled",
                     GetBoolValue(this->MotionSharpnessEnabled.value_for_config()).c_str());
        target.SetValue("CAS", "MotionSharpness", GetFloatValue(this->MotionSharpness.value_for_config()).c_str());
        target.SetValue("CAS", "MotionThreshold", GetFloatValue(this->MotionThreshold.value_for_config()).c_str());
        target.SetValue("CAS", "MotionScaleLimit", GetFloatValue(this->MotionScaleLimit.value_for_config()).c_str());

        target.SetValue("CAS", "ContrastEnabled", GetBoolValue(this->ContrastEnabled.value_for_config()).c_str());
        target.SetValue("CAS", "Contrast", GetFloatValue(this->Contrast.value_for_config()).c_str());

        target.SetValue("CAS", "DADepthScale", GetFloatValue(this->DADepthScale.value_for_config()).c_str());
        target.SetValue("CAS", "DADepthBias", GetFloatValue(this->DADepthBias.value_for_config()).c_str());
        target.SetValue("CAS", "DAClampOutput", GetBoolValue(this->DAClampOutput.value_for_config()).c_str());

        target.SetValue("CAS", "SharpenerDebug",
                     GetBoolValue(this->MotionSharpnessDebug.value_for_config()).c_str());
    }

    // Magnifier
    {
        target.SetValue("Magnifier", "Enabled", GetBoolValue(this->MagnifierEnabled.value_for_config()).c_str());

        target.SetValue("Magnifier", "Size", GetFloatValue(this->MagnifierSize.value_for_config()).c_str());
        target.SetValue("Magnifier", "ZoomFactor",
                     GetIntValue(this->MagnifierZoomFactor.value_for_config()).c_str());

        target.SetValue("Magnifier", "BorderSize",
                     GetFloatValue(this->MagnifierBorderSize.value_for_config()).c_str());
        target.SetValue("Magnifier", "CursorOffsetX",
                     GetFloatValue(this->MagnifierCursorOffsetX.value_for_config()).c_str());
        target.SetValue("Magnifier", "CursorOffsetY",
                     GetFloatValue(this->MagnifierCursorOffsetY.value_for_config()).c_str());

        target.SetValue("Magnifier", "StaticPosX",
                     GetFloatValue(this->MagnifierStaticPosX.value_for_config()).c_str());
        target.SetValue("Magnifier", "StaticPosY",
                     GetFloatValue(this->MagnifierStaticPosY.value_for_config()).c_str());
    }

    // Menu
    {
        target.SetValue("Menu", "Scale", GetFloatValue(this->MenuScale.value_for_config()).c_str());
        target.SetValue("Menu", "MenuWidth", GetFloatValue(this->MenuWidth.value_for_config()).c_str());
        target.SetValue("Menu", "MenuHeight", GetFloatValue(this->MenuHeight.value_for_config()).c_str());
        target.SetValue("Menu", "MenuPage", this->MenuPage.value_for_config_or("auto").c_str());
        target.SetValue("Menu", "Language", this->MenuLanguage.value_for_config_or("zh").c_str());
        target.SetValue("Menu", "OverlayMenu", GetBoolValue(this->OverlayMenu.value_for_config()).c_str());

        auto setting = this->ShortcutKey.value_for_config();
        target.SetValue("Menu", "ShortcutKey",
                     GetIntValue(this->ShortcutKey.value_for_config(), setting > 0).c_str());

        target.SetValue("Menu", "ExtendedLimits", GetBoolValue(this->ExtendedLimits.value_for_config()).c_str());
        target.SetValue("Menu", "ShowFps", GetBoolValue(this->ShowFps.value_for_config()).c_str());
        target.SetValue("Menu", "F5LowHint", GetBoolValue(this->F5LowHint.value_for_config()).c_str());
        target.SetValue("Menu", "ShowUnfinished", GetBoolValue(this->MenuShowUnfinished.value_for_config()).c_str());
        target.SetValue("Menu", "ShowNonessential", GetBoolValue(this->MenuShowNonessential.value_for_config()).c_str());
        target.SetValue("Menu", "UseHQFont", GetBoolValue(this->UseHQFont.value_for_config()).c_str());
        target.SetValue("Menu", "DisableSplash", GetBoolValue(this->DisableSplash.value_for_config()).c_str());

        setting = this->CurrentOutputCaptureKey.value_for_config();
        target.SetValue("Menu", "CurrentOutputCaptureKey",
                       GetIntValue(this->CurrentOutputCaptureKey.value_for_config(), setting > 0).c_str());

        // Persist an explicit opt-in/off value rather than an ambiguous legacy auto binding.
        target.SetValue("Menu", "FGShortcutEnabled",
                        this->FGShortcutEnabled.value_for_config_or(false) ? "true" : "false");

        setting = this->FGShortcutKey.value_for_config();
        target.SetValue("Menu", "FGShortcutKey",
                     GetIntValue(this->FGShortcutKey.value_for_config(), setting > 0).c_str());

        setting = this->FpsShortcutKey.value_for_config();
        target.SetValue("Menu", "FpsShortcutKey",
                     GetIntValue(this->FpsShortcutKey.value_for_config(), setting > 0).c_str());

        setting = this->FpsCycleShortcutKey.value_for_config();
        target.SetValue("Menu", "FpsCycleShortcutKey",
                     GetIntValue(this->FpsCycleShortcutKey.value_for_config(), setting > 0).c_str());

        target.SetValue("Menu", "FpsOverlayPos", GetIntValue(this->FpsOverlayPosition.value_for_config()).c_str());
        target.SetValue("Menu", "FpsOverlayType", GetIntValue(this->FpsOverlayType.value_for_config()).c_str());
        target.SetValue("Menu", "FpsOverlayHorizontal",
                     GetBoolValue(this->FpsOverlayHorizontal.value_for_config()).c_str());
        target.SetValue("Menu", "FpsOverlayAlpha", GetFloatValue(this->FpsOverlayAlpha.value_for_config()).c_str());
        target.SetValue("Menu", "FpsScale", GetFloatValue(this->FpsScale.value_for_config()).c_str());
        target.SetValue("Menu", "FontSize", GetFloatValue(this->FontSize.value_for_config()).c_str());
        target.SetValue("Menu", "TTFFontPath",
                     wstring_to_string(this->TTFFontPath.value_for_config_or(L"auto")).c_str());

        target.SetValue("Menu", "LightTheme", GetBoolValue(this->LightTheme.value_for_config()).c_str());
        target.SetValue("Menu", "ModernTheme", GetBoolValue(this->ModernTheme.value_for_config()).c_str());
        target.SetValue("Menu", "OverlaysUseTheme", GetBoolValue(this->OverlaysUseTheme.value_for_config()).c_str());
        target.SetValue("Menu", "AccentColorR", GetFloatValue(this->MenuAccentColorR.value_for_config()).c_str());
        target.SetValue("Menu", "AccentColorG", GetFloatValue(this->MenuAccentColorG.value_for_config()).c_str());
        target.SetValue("Menu", "AccentColorB", GetFloatValue(this->MenuAccentColorB.value_for_config()).c_str());
        target.SetValue("Menu", "BGColorR", GetFloatValue(this->MenuBGColorR.value_for_config()).c_str());
        target.SetValue("Menu", "BGColorG", GetFloatValue(this->MenuBGColorG.value_for_config()).c_str());
        target.SetValue("Menu", "BGColorB", GetFloatValue(this->MenuBGColorB.value_for_config()).c_str());
        target.SetValue("Menu", "BGColorA", GetFloatValue(this->MenuBGColorA.value_for_config()).c_str());
    }

    // Hooks
    {
        target.SetValue("Hooks", "HookOriginalNvngxOnly",
                     GetBoolValue(this->HookOriginalNvngxOnly.value_for_config()).c_str());
        target.SetValue("Hooks", "EarlyHooking", GetBoolValue(this->EarlyHooking.value_for_config()).c_str());
        target.SetValue("Hooks", "D3D11FeatureLevelElevation",
                     GetBoolValue(this->D3D11FeatureLevelElevation.value_for_config()).c_str());
        target.SetValue("Hooks", "UseNtdllHooks", GetBoolValue(this->UseNtdllHooks.value_for_config()).c_str());
    }

    // InitFlags
    {
        target.SetValue("InitFlags", "AutoExposure", GetBoolValue(this->AutoExposure.value_for_config()).c_str());
        target.SetValue("InitFlags", "HDR", GetBoolValue(this->HDR.value_for_config()).c_str());
        target.SetValue("InitFlags", "DepthInverted", GetBoolValue(this->DepthInverted.value_for_config()).c_str());
        target.SetValue("InitFlags", "JitterCancellation",
                     GetBoolValue(this->JitterCancellation.value_for_config()).c_str());
        target.SetValue("InitFlags", "DisplayResolution",
                     GetBoolValue(this->DisplayResolution.value_for_config()).c_str());
        target.SetValue("InitFlags", "DisableReactiveMask",
                     GetBoolValue(this->DisableReactiveMask.value_for_config()).c_str());
    }

    // Upscale Ratio Override
    {
        target.SetValue("UpscaleRatio", "UpscaleRatioOverrideEnabled",
                     GetBoolValue(this->UpscaleRatioOverrideEnabled.value_for_config()).c_str());
        target.SetValue("UpscaleRatio", "UpscaleRatioOverrideValue",
                     GetFloatValue(this->UpscaleRatioOverrideValue.value_for_config()).c_str());
    }

    // Quality Overrides
    {
        target.SetValue("QualityOverrides", "QualityRatioOverrideEnabled",
                     GetBoolValue(this->QualityRatioOverrideEnabled.value_for_config()).c_str());
        target.SetValue("QualityOverrides", "QualityRatioDLAA",
                     GetFloatValue(this->QualityRatio_DLAA.value_for_config()).c_str());
        target.SetValue("QualityOverrides", "QualityRatioUltraQuality",
                     GetFloatValue(this->QualityRatio_UltraQuality.value_for_config()).c_str());
        target.SetValue("QualityOverrides", "QualityRatioQuality",
                     GetFloatValue(this->QualityRatio_Quality.value_for_config()).c_str());
        target.SetValue("QualityOverrides", "QualityRatioBalanced",
                     GetFloatValue(this->QualityRatio_Balanced.value_for_config()).c_str());
        target.SetValue("QualityOverrides", "QualityRatioPerformance",
                     GetFloatValue(this->QualityRatio_Performance.value_for_config()).c_str());
        target.SetValue("QualityOverrides", "QualityRatioUltraPerformance",
                     GetFloatValue(this->QualityRatio_UltraPerformance.value_for_config()).c_str());
    }

    // Anisotropy
    {
        target.SetValue("Anisotropy", "AnisotropyOverride",
                     GetIntValue(this->AnisotropyOverride.value_for_config()).c_str());
        target.SetValue("Anisotropy", "ModifyComparison",
                     GetBoolValue(this->AnisotropyModifyComp.value_for_config()).c_str());
        target.SetValue("Anisotropy", "ModifyMinMax",
                     GetBoolValue(this->AnisotropyModifyMinMax.value_for_config()).c_str());
        target.SetValue("Anisotropy", "SkipPointFilter",
                     GetBoolValue(this->AnisotropySkipPointFilter.value_for_config()).c_str());
    }

    // Mipmap
    {
        target.SetValue("Mipmap", "MipmapBiasOverride",
                     GetFloatValue(this->MipmapBiasOverride.value_for_config()).c_str());
        target.SetValue("Mipmap", "MipmapBiasOverrideAll",
                     GetBoolValue(this->MipmapBiasOverrideAll.value_for_config()).c_str());
        target.SetValue("Mipmap", "MipmapBiasFixedOverride",
                     GetBoolValue(this->MipmapBiasFixedOverride.value_for_config()).c_str());
        target.SetValue("Mipmap", "MipmapBiasScaleOverride",
                     GetBoolValue(this->MipmapBiasScaleOverride.value_for_config()).c_str());
    }

    // Process Filter
    {
        target.SetValue("ProcessFilter", "TargetProcessName",
                     wstring_to_string(this->TargetProcess.value_for_config_or(L"auto")).c_str());
        target.SetValue("ProcessFilter", "ProcessExclusionList",
                     wstring_to_string(this->ProcessExclusionList.value_for_config_or(L"auto")).c_str());
    }

    // Hotfixes
    {
        target.SetValue("Hotfix", "CreateD3D12DeviceForLuma",
                     GetBoolValue(this->CreateD3D12DeviceForLuma.value_for_config()).c_str());
        target.SetValue("Hotfix", "CheckForUpdate", GetBoolValue(this->CheckForUpdate.value_for_config()).c_str());
        target.SetValue("Hotfix", "SimulateWaitableObject",
                     GetBoolValue(this->SimulateWaitableObject.value_for_config()).c_str());
        target.SetValue("Hotfix", "DisableOverlays", GetBoolValue(this->DisableOverlays.value_for_config()).c_str());
        target.SetValue("Hotfix", "REFrameworkCompat",
                     GetBoolValue(this->REFrameworkCompat.value_for_config()).c_str());

        target.SetValue("Hotfix", "RoundInternalResolution",
                     GetIntValue(this->RoundInternalResolution.value_for_config()).c_str());

        target.SetValue("Hotfix", "RestoreComputeSignature",
                     GetBoolValue(this->RestoreComputeSignature.value_for_config()).c_str());
        target.SetValue("Hotfix", "RestoreGraphicSignature",
                     GetBoolValue(this->RestoreGraphicSignature.value_for_config()).c_str());
        target.SetValue("Hotfix", "ExtendedStateRestore",
                     GetBoolValue(this->ExtendedStateRestore.value_for_config()).c_str());
        target.SetValue("Hotfix", "SkipFirstFrames", GetIntValue(this->SkipFirstFrames.value_for_config()).c_str());

        target.SetValue("Hotfix", "UsePrecompiledShaders",
                     GetBoolValue(this->UsePrecompiledShaders.value_for_config()).c_str());
        target.SetValue("Hotfix", "PreferDedicatedGpu",
                     GetBoolValue(this->PreferDedicatedGpu.value_for_config()).c_str());
        target.SetValue("Hotfix", "PreferFirstDedicatedGpu",
                     GetBoolValue(this->PreferFirstDedicatedGpu.value_for_config()).c_str());

        target.SetValue("Hotfix", "ColorResourceBarrier",
                     GetIntValue(this->ColorResourceBarrier.value_for_config()).c_str());
        target.SetValue("Hotfix", "MotionVectorResourceBarrier",
                     GetIntValue(this->MVResourceBarrier.value_for_config()).c_str());
        target.SetValue("Hotfix", "DepthResourceBarrier",
                     GetIntValue(this->DepthResourceBarrier.value_for_config()).c_str());
        target.SetValue("Hotfix", "ColorMaskResourceBarrier",
                     GetIntValue(this->MaskResourceBarrier.value_for_config()).c_str());
        target.SetValue("Hotfix", "ExposureResourceBarrier",
                     GetIntValue(this->ExposureResourceBarrier.value_for_config()).c_str());
        target.SetValue("Hotfix", "OutputResourceBarrier",
                     GetIntValue(this->OutputResourceBarrier.value_for_config()).c_str());
    }

    // Dx11 with Dx12
    {
        target.SetValue("Dx11withDx12", "DontUseNTShared",
                     GetBoolValue(this->DontUseNTShared.value_for_config()).c_str());
    }

    // Logging
    {
        target.SetValue("Log", "LogToFile", GetBoolValue(this->LogToFile.value_for_config()).c_str());
        target.SetValue("Log", "LogLevel", GetIntValue(this->LogLevel.value_for_config()).c_str());
        target.SetValue("Log", "LogToConsole", GetBoolValue(this->LogToConsole.value_for_config()).c_str());
        target.SetValue("Log", "LogToDebug", GetBoolValue(this->LogToDebug.value_for_config()).c_str());
        target.SetValue("Log", "LogToNGX", GetBoolValue(this->LogToNGX.value_for_config()).c_str());
        target.SetValue("Log", "OpenConsole", GetBoolValue(this->OpenConsole.value_for_config()).c_str());
        target.SetValue("Log", "SingleFile", GetBoolValue(this->LogSingleFile.value_for_config()).c_str());
        target.SetValue("Log", "LogFileName",
                     wstring_to_string(this->LogFileName.value_for_config_or(L"auto")).c_str());
        target.SetValue("Log", "LogAsync", GetBoolValue(this->LogAsync.value_for_config()).c_str());
        target.SetValue("Log", "LogAsyncThreads", GetIntValue(this->LogAsyncThreads.value_for_config()).c_str());
    }

    // NvApi
    {
        target.SetValue("NvApi", "DisableFlipMetering",
                     GetBoolValue(this->DisableFlipMetering.value_for_config()).c_str());
    }

    // DRS
    {
        target.SetValue("DRS", "DrsMinOverrideEnabled",
                     GetBoolValue(this->DrsMinOverrideEnabled.value_for_config()).c_str());
        target.SetValue("DRS", "DrsMaxOverrideEnabled",
                     GetBoolValue(this->DrsMaxOverrideEnabled.value_for_config()).c_str());
    }

    // Spoofing
    {
        target.SetValue("Spoofing", "Dxgi", GetBoolValue(this->DxgiSpoofing.value_for_config()).c_str());
        target.SetValue("Spoofing", "DxgiFactoryWrapping",
                     GetBoolValue(this->DxgiFactoryWrapping.value_for_config()).c_str());
        target.SetValue("Spoofing", "DxgiBlacklist", this->DxgiBlacklist.value_for_config_or("auto").c_str());
        target.SetValue("Spoofing", "Vulkan", GetBoolValue(this->VulkanSpoofing.value_for_config()).c_str());
        target.SetValue("Spoofing", "VulkanExtensionSpoofing",
                     GetBoolValue(this->VulkanExtensionSpoofing.value_for_config()).c_str());
        target.SetValue("Spoofing", "VulkanVRAM", GetIntValue(this->VulkanVRAM.value_for_config()).c_str());
        target.SetValue("Spoofing", "DxgiVRAM", GetIntValue(this->DxgiVRAM.value_for_config()).c_str());
        target.SetValue("Spoofing", "SpoofedGPUName",
                     wstring_to_string(this->SpoofedGPUName.value_for_config_or(L"auto")).c_str());
        target.SetValue("Spoofing", "StreamlineSpoofing",
                     GetBoolValue(this->StreamlineSpoofing.value_for_config()).c_str());
        target.SetValue("Spoofing", "SpoofHAGS", GetBoolValue(this->SpoofHAGS.value_for_config()).c_str());
        target.SetValue("Spoofing", "D3DFeatureLevel",
                     GetBoolValue(this->SpoofFeatureLevel.value_for_config()).c_str());
        target.SetValue("Spoofing", "UEIntelAtomics",
                     GetBoolValue(this->UESpoofIntelAtomics64.value_for_config()).c_str());
        target.SetValue("Spoofing", "SpoofedVendorId",
                     GetIntValue(this->SpoofedVendorId.value_for_config(), true).c_str());
        target.SetValue("Spoofing", "SpoofedDeviceId",
                     GetIntValue(this->SpoofedDeviceId.value_for_config(), true).c_str());
        target.SetValue("Spoofing", "TargetVendorId",
                     GetIntValue(this->TargetVendorId.value_for_config(), true).c_str());
        target.SetValue("Spoofing", "TargetDeviceId",
                     GetIntValue(this->TargetDeviceId.value_for_config(), true).c_str());
        target.SetValue("Spoofing", "Registry", GetBoolValue(this->SpoofRegistry.value_for_config()).c_str());
        target.SetValue("Spoofing", "RegistryDriver",
                     wstring_to_string(this->SpoofedDriver.value_for_config_or(L"auto")).c_str());
        target.SetValue("Spoofing", "User32", GetBoolValue(this->SpoofUser32.value_for_config()).c_str());
    }

    // Plugins
    {

        target.SetValue("Plugins", "Path", wstring_to_string(this->PluginPath.value_for_config_or(L"auto")).c_str());
        target.SetValue("Plugins", "LoadSpecialK", GetBoolValue(this->LoadSpecialK.value_for_config()).c_str());
        target.SetValue("Plugins", "LoadReShade", GetBoolValue(this->LoadReShade.value_for_config()).c_str());
        target.SetValue("DrawUiMask", "Enabled", GetBoolValue(this->DrawUiMaskEnabled.value_for_config()).c_str());
        target.SetValue("DrawUiMask", "FG", GetBoolValue(this->DrawUiMaskFg.value_for_config()).c_str());
        target.SetValue("DrawUiMask", "NR", GetBoolValue(this->DrawUiMaskNr.value_for_config()).c_str());
        target.SetValue("DrawUiMask", "Filters", GetBoolValue(this->DrawUiMaskFilters.value_for_config()).c_str());
        target.SetValue("DrawUiMask", "Rules", this->DrawUiMaskRules.value_for_config_or("").c_str());
        target.SetValue("GIMI", "Enabled", GetBoolValue(this->GimiEnabled.value_for_config()).c_str());
        target.SetValue("GIMI", "Ini", this->GimiIni.value_for_config_or("").c_str());
        target.SetValue("GIMI", "Component", this->GimiComponent.value_for_config_or("").c_str());
        target.SetValue("GIMI", "Trigger", this->GimiTrigger.value_for_config_or("ib").c_str());
        target.SetValue("GIMI", "Libraries", this->GimiLibraries.value_for_config_or("").c_str());
        target.SetValue("GIMI", "HostRoot", this->GimiHostRoot.value_for_config_or("").c_str());
        target.SetValue("GIMI", "NamespaceRoot", this->GimiNamespaceRoot.value_for_config_or("").c_str());
        target.SetValue("GIMI", "ImportRoot", this->GimiImportRoot.value_for_config_or("").c_str());
        target.SetValue("HoYoShade", "Enabled", GetBoolValue(this->HoYoShadeEnabled.value_for_config()).c_str());
        target.SetValue("HoYoShade", "Stage", GetIntValue(this->HoYoShadeStage.value_for_config()).c_str());
        target.SetValue("Plugins", "LoadCustomAmdxc64OnRdna2",
                     GetBoolValue(this->LoadCustomAmdxc64OnRdna2.value_for_config()).c_str());
        target.SetValue("Plugins", "LoadAsiPlugins", GetBoolValue(this->LoadAsiPlugins.value_for_config()).c_str());
        target.SetValue("Plugins", "LateAsiPluginsDelay",
                     GetIntValue(this->LateAsiPluginsDelay.value_for_config()).c_str());
    }

    // fakenvapi
    {
        target.SetValue("fakenvapi", "UseFakenvapi", GetBoolValue(this->UseFakenvapi.value_for_config()).c_str());
        target.SetValue("fakenvapi", "ForceXeLL", GetBoolValue(this->ForceXeLL.value_for_config()).c_str());
        target.SetValue("fakenvapi", "ForceLatencyFlex",
                     GetBoolValue(this->FN_ForceLatencyFlex.value_for_config()).c_str());
        target.SetValue("fakenvapi", "LatencyFlexMode",
                     GetIntValue(this->FN_LatencyFlexMode.value_for_config()).c_str());
        target.SetValue("fakenvapi", "ForceReflex", GetIntValue(this->FN_ForceReflex.value_for_config()).c_str());
    }

    // inputs
    {
        target.SetValue("Inputs", "EnableDlssInputs",
                     GetBoolValue(this->EnableDlssInputs.value_for_config()).c_str());
        target.SetValue("Inputs", "EnableXeSSInputs",
                     GetBoolValue(this->EnableXeSSInputs.value_for_config()).c_str());
        target.SetValue("Inputs", "UseFsr2Inputs", GetBoolValue(this->UseFsr2Inputs.value_for_config()).c_str());
        target.SetValue("Inputs", "UseFsr2Dx11Inputs",
                     GetBoolValue(this->UseFsr2Dx11Inputs.value_for_config()).c_str());
        target.SetValue("Inputs", "UseFsr2VulkanInputs",
                     GetBoolValue(this->UseFsr2VulkanInputs.value_for_config()).c_str());
        target.SetValue("Inputs", "Fsr2Pattern", GetBoolValue(this->Fsr2Pattern.value_for_config()).c_str());
        target.SetValue("Inputs", "UseFsr3Inputs", GetBoolValue(this->UseFsr3Inputs.value_for_config()).c_str());
        target.SetValue("Inputs", "Fsr3Pattern", GetBoolValue(this->Fsr3Pattern.value_for_config()).c_str());
        target.SetValue("Inputs", "UseFfxInputs", GetBoolValue(this->UseFfxInputs.value_for_config()).c_str());
        target.SetValue("Inputs", "EnableHotSwapping",
                     GetBoolValue(this->EnableHotSwapping.value_for_config()).c_str());

        target.SetValue("Inputs", "EnableFsr2Inputs",
                     GetBoolValue(this->EnableFsr2Inputs.value_for_config()).c_str());
        target.SetValue("Inputs", "EnableFsr3Inputs",
                     GetBoolValue(this->EnableFsr3Inputs.value_for_config()).c_str());
        target.SetValue("Inputs", "EnableFfxInputs", GetBoolValue(this->EnableFfxInputs.value_for_config()).c_str());
    }

    // V-Sync
    {
        target.SetValue("V-Sync", "OverrideVsync", GetBoolValue(this->OverrideVsync.value_for_config()).c_str());
        target.SetValue("V-Sync", "ForceVsync", GetBoolValue(this->ForceVsync.value_for_config()).c_str());
        const auto interval = this->VsyncInterval.value_for_config();
        // Normalize the persisted candidate without mutating live/runtime state.
        target.SetValue("V-Sync", "SyncInterval",
            GetIntValue(interval && *interval > 3 ? std::optional<UINT>{} : interval).c_str());
    }

    // Libraries
    {
        target.SetValue("Libraries", "OptiDllPath",
                     wstring_to_string(this->MainDllPath.value_for_config_or(L"auto")).c_str());

        target.SetValue("Libraries", "NvngxPath",
                     wstring_to_string(this->NvngxPath.value_for_config_or(L"auto")).c_str());
        target.SetValue("Libraries", "NvngxFeaturePath",
                     wstring_to_string(this->DLSSFeaturePath.value_for_config_or(L"auto")).c_str());
        target.SetValue("Libraries", "NvngxDlssPath",
                     wstring_to_string(this->NVNGX_DLSS_Library.value_for_config_or(L"auto")).c_str());
        target.SetValue("Libraries", "NvapiPath",
                     wstring_to_string(this->NvapiDllPath.value_for_config_or(L"auto")).c_str());

        target.SetValue("Libraries", "FfxDx12Path",
                     wstring_to_string(this->FfxDx12Path.value_for_config_or(L"auto")).c_str());
        target.SetValue("Libraries", "FfxDx12SRPath",
                     wstring_to_string(this->FfxDx12SRPath.value_for_config_or(L"auto")).c_str());
        target.SetValue("Libraries", "FfxDx12FGPath",
                     wstring_to_string(this->FfxDx12FGPath.value_for_config_or(L"auto")).c_str());
        target.SetValue("Libraries", "FfxDx12RRPath",
                     wstring_to_string(this->FfxDx12RRPath.value_for_config_or(L"auto")).c_str());
        target.SetValue("Libraries", "FfxDx12RCPath",
                     wstring_to_string(this->FfxDx12RCPath.value_for_config_or(L"auto")).c_str());
        target.SetValue("Libraries", "FfxVkPath",
                     wstring_to_string(this->FfxVkPath.value_for_config_or(L"auto")).c_str());

        target.SetValue("Libraries", "XeSSPath",
                     wstring_to_string(this->XeSSLibrary.value_for_config_or(L"auto")).c_str());
        target.SetValue("Libraries", "XeFGPath",
                     wstring_to_string(this->XeFGLibrary.value_for_config_or(L"auto")).c_str());
        target.SetValue("Libraries", "XeLLPath",
                     wstring_to_string(this->XeLLLibrary.value_for_config_or(L"auto")).c_str());
        target.SetValue("Libraries", "XeSSDx11Path",
                     wstring_to_string(this->XeSSDx11Library.value_for_config_or(L"auto")).c_str());
    }

    GenshinLegacy::SaveControls(target, genshinSettings);
    // Hook CPU diagnostics are session-only; do not persist or remove legacy keys.

    // Old configs, just delete them
    {
        target.Delete("FSR", "Fsr4ForceEnableInt8");
        target.Delete("Nukems", "MakeDepthCopy", true);
    }

}

GenshinLegacy::Settings Config::SnapshotGenshinSettings() const
{
    // Same existing mutex and one short acquisition per snapshot as before.
    // Never call AcquireGenshinControls here: it may publish an admission epoch.
    std::lock_guard<std::recursive_mutex> guard(genshinControlsMutex_);
    return genshinControls_.settings;
}

void Config::RefreshSettingsSample(const GenshinLegacy::Settings* genshinSnapshot)
{
    // Retain the existing controls snapshot/lock boundary. Poll actual persisted
    // values, not an incomplete edit generation or the controls admission epoch.
    const auto genshinSettings = genshinSnapshot ? *genshinSnapshot : SnapshotGenshinSettings();
    settingsSample_.Begin();
    WriteSettings(settingsSample_, genshinSettings);
    if (settingsSample_.Finish())
        dirtyComparisonKnown_ = false;
}

std::optional<std::string> Config::SerializeSettings(bool updateLoadedIni,
    const GenshinLegacy::Settings* genshinSnapshot)
{
    // Preserve the original serializer API for explicit save/baseline callers.
    // Menu polling uses the owned snapshot directly, without a whole-string copy.
    if (!updateLoadedIni)
    {
        RefreshSettingsSample(genshinSnapshot);
        return settingsSample_.serialized;
    }
    const auto genshinSettings = genshinSnapshot ? *genshinSnapshot : SnapshotGenshinSettings();
    WriteSettings(ini, genshinSettings);
    std::string serialized;
    if (ini.Save(serialized) < 0) return std::nullopt;
    return serialized;
}

void Config::CaptureSavedSettings()
{
    savedSettings_ = SerializeSettings();
    dirtySampleKnown_ = false;
    dirtyComparisonKnown_ = false;
    cachedDirty_ = !savedSettings_.has_value();
}

bool Config::HasUnsavedSettings(uint64_t now)
{
    // Poll the same complete persistence schema at the original 250 ms cadence.
    // Stable writes only compare existing strings: no INI tree, Save, snapshot
    // copy or baseline string comparison. Public/raw edits remain observable.
    if (dirtySampleKnown_ && now >= dirtySampleTick_ && now - dirtySampleTick_ < 250)
        return cachedDirty_;
    dirtySampleKnown_ = true;
    dirtySampleTick_ = now;
    RefreshSettingsSample();
    if (!dirtyComparisonKnown_)
    {
        const auto& current = settingsSample_.serialized;
        cachedDirty_ = !current || !savedSettings_ || *current != *savedSettings_;
        dirtyComparisonKnown_ = true;
    }
    return cachedDirty_;
}

bool Config::SaveIni()
{
    LOG_INFO("Trying to save ini to: {0}", wstring_to_string(absoluteFileName.wstring()));
    return PersistSettings([&] { return ini.SaveFile(absoluteFileName.wstring().c_str()) >= 0; });
}

bool Config::PersistSettings(const std::function<bool()>& writer)
{
    // Keep the native controls lock short, as before: never hold it during
    // whole-schema serialization or file IO. Both save forms use this one copy.
    const auto genshinSettings = SnapshotGenshinSettings();
    const auto candidate = SerializeSettings(false, &genshinSettings);
    if (!candidate || !SerializeSettings(true, &genshinSettings))
    {
        lastSaveFailed_ = true;
        dirtySampleKnown_ = false;
        dirtyComparisonKnown_ = false;
        return false;
    }
    if (!writer || !writer())
    {
        lastSaveFailed_ = true;
        dirtySampleKnown_ = false;
        dirtyComparisonKnown_ = false;
        return false; // Preserve the last successful snapshot and dirty edits.
    }
    savedSettings_ = candidate;
    dirtyComparisonKnown_ = false;
    cachedDirty_ = false;
    dirtySampleKnown_ = false;
    lastSaveFailed_ = false;
    return true;
}

bool Config::SaveXeFG()
{
    ini.SetValue("XeFG", "DepthInverted", GetBoolValue(Instance()->FGXeFGDepthInverted.value_for_config()).c_str());
    ini.SetValue("XeFG", "JitteredMV", GetBoolValue(Instance()->FGXeFGJitteredMV.value_for_config()).c_str());
    ini.SetValue("XeFG", "HighResMV", GetBoolValue(Instance()->FGXeFGHighResMV.value_for_config()).c_str());

    auto pathWStr = absoluteFileName.wstring();
    LOG_INFO("Trying to save ini to: {0}", wstring_to_string(pathWStr));

    return ini.SaveFile(absoluteFileName.wstring().c_str()) >= 0;
}

void Config::CheckUpscalerFiles()
{
    if (!State::Instance().nvngxExists)
        State::Instance().nvngxExists = std::filesystem::exists(Util::ExePath().parent_path() / L"nvngx.dll");

    if (!State::Instance().nvngxExists)
        State::Instance().nvngxExists = std::filesystem::exists(Util::ExePath().parent_path() / L"_nvngx.dll");

    if (!State::Instance().nvngxExists)
    {
        State::Instance().nvngxExists = GetModuleHandle(L"nvngx.dll") != nullptr;

        if (!State::Instance().nvngxExists)
            State::Instance().nvngxExists = GetModuleHandle(L"_nvngx.dll") != nullptr;

        if (State::Instance().nvngxExists)
            LOG_INFO("nvngx.dll found in memory");
        else
            LOG_WARN("nvngx.dll not found!");
    }
    else
    {
        LOG_INFO("nvngx.dll found in game folder");
    }

    if (auto nvngxReplacement = Util::FindFilePath(Util::DllPath().remove_filename(), "nvngx_dlss.dll");
        nvngxReplacement.has_value())
    {
        State::Instance().nvngxReplacement = nvngxReplacement.value().wstring();
    }

    State::Instance().libxessExists = std::filesystem::exists(Util::ExePath().parent_path() / L"libxess.dll");
    if (!State::Instance().libxessExists)
    {
        State::Instance().libxessExists = GetModuleHandle(L"libxess.dll") != nullptr;

        if (State::Instance().libxessExists)
            LOG_INFO("libxess.dll found in memory");
        else
            LOG_WARN("libxess.dll not found!");
    }
    else
    {
        LOG_INFO("libxess.dll found in game folder");
    }
}

std::vector<std::string> Config::GetConfigLog() { return _log; }

std::optional<std::string> Config::readString(std::string section, std::string key, bool lowercase)
{
    std::string value = ini.GetValue(section.c_str(), key.c_str(), "auto");

    std::string lower = value;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return std::tolower(c); });

    if (lower == "auto")
        return std::nullopt;

    _log.push_back(std::format("{}.{}: {}", section, key, value));

    return lowercase ? lower : value;
}

std::optional<std::wstring> Config::readWString(std::string section, std::string key, bool lowercase)
{
    std::string value = ini.GetValue(section.c_str(), key.c_str(), "auto");

    std::string lower = value;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return std::tolower(c); });

    if (lower == "auto")
        return std::nullopt;

    _log.push_back(std::format("{}.{}: {}", section, key, value));

    return lowercase ? string_to_wstring(lower) : string_to_wstring(value);
}

std::optional<float> Config::readFloat(std::string section, std::string key)
{
    auto value = readString(section, key);

    try
    {
        float result;

        if (value.has_value() && isFloat(value.value(), result))
            return result;

        return std::nullopt;
    }
    catch (const std::bad_optional_access&) // missing or auto value
    {
        return std::nullopt;
    }
    catch (const std::invalid_argument&) // invalid float string for std::stof
    {
        return std::nullopt;
    }
    catch (const std::out_of_range&) // out of range for 32 bit float
    {
        return std::nullopt;
    }
}

std::optional<int> Config::readInt(std::string section, std::string key)
{
    auto value = readString(section, key);
    if (!value.has_value())
        return std::nullopt;

    const auto& s = *value;
    try
    {
        size_t idx = 0;
        int result;

        // detect hex prefix
        if (s.size() > 2 && (s[0] == '0') && (s[1] == 'x' || s[1] == 'X'))
        {
            result = std::stoi(s, &idx, 16);
        }
        else
        {
            result = std::stoi(s, &idx, 10);
        }

        // ensure we consumed the whole string
        if (idx == s.size())
            return result;
        else
            return std::nullopt;
    }
    catch (const std::bad_optional_access&) // missing or auto value
    {
        return std::nullopt;
    }
    catch (const std::invalid_argument&) // invalid float string for std::stof
    {
        return std::nullopt;
    }
    catch (const std::out_of_range&) // out// out of range for 32 bit float
    {
        return std::nullopt;
    }
}

std::optional<uint32_t> Config::readUInt(std::string section, std::string key)
{
    auto value = readString(section, key);
    if (!value.has_value())
        return std::nullopt;

    const auto& s = *value;
    try
    {
        size_t idx = 0;
        int result;

        // detect hex prefix
        if (s.size() > 2 && (s[0] == '0') && (s[1] == 'x' || s[1] == 'X'))
        {
            result = std::stoi(s, &idx, 16);
        }
        else
        {
            result = std::stoi(s, &idx, 10);
        }

        // ensure we consumed the whole string
        if (idx == s.size())
            return result;
        else
            return std::nullopt;
    }
    catch (const std::bad_optional_access&) // missing or auto value
    {
        return std::nullopt;
    }
    catch (const std::invalid_argument&) // invalid float string for std::stof
    {
        return std::nullopt;
    }
    catch (const std::out_of_range&) // out// out of range for 32 bit float
    {
        return std::nullopt;
    }
}

std::optional<bool> Config::readBool(std::string section, std::string key)
{
    auto value = readString(section, key, true);
    if (value == "true")
        return true;
    else if (value == "false")
        return false;

    return std::nullopt;
}

// Only use for unsigned enums that have Enum::Count as the last entry
template <typename Enum> std::optional<Enum> Config::readEnum(std::string section, std::string key)
{
    static_assert(std::is_enum_v<Enum>, "Enum type required");

    auto value = readUInt(section, key);
    if (!value.has_value())
        return std::nullopt;

    using Underlying = std::underlying_type_t<Enum>;

    if (*value < static_cast<Underlying>(Enum::Count))
        return static_cast<Enum>(*value);

    return std::nullopt;
}

Config* Config::Instance()
{
    if (!_config)
        _config = new Config();

    return _config;
}

bool Config::IsStarRailInputProfile() const
{
    return _starRailInputProfile;
}

bool Config::StarRailInputMotionY() const
{
    return StarRailInputMotionYInverted.value_or_default();
}

bool Config::StarRailInputDepthFlipY() const
{
    return StarRailInputDepthTextureFlipY.value_or_default();
}

bool Config::StarRailInputMotionFlipY() const
{
    return StarRailInputMotionTextureFlipY.value_or_default();
}

void Config::SetStarRailInputMotionY(bool value)
{
    StarRailInputMotionYInverted = value;
    if (_starRailInputProfile)
        DlssNrMotionYInverted.set_volatile_value(value);
}

void Config::SetStarRailInputDepthFlipY(bool value)
{
    StarRailInputDepthTextureFlipY = value;
    if (_starRailInputProfile)
        DlssNrDepthTextureFlipY.set_volatile_value(value);
}

void Config::SetStarRailInputMotionFlipY(bool value)
{
    StarRailInputMotionTextureFlipY = value;
    if (_starRailInputProfile)
        DlssNrMotionTextureFlipY.set_volatile_value(value);
}

void Config::ApplyStarRailInputProfile()
{
    if (!_starRailInputProfile)
        return;

    if (!_starRailInputCaptured)
    {
        _starRailOriginalMotionY = DlssNrMotionYInverted.value_or_default();
        _starRailOriginalDepthFlipY = DlssNrDepthTextureFlipY.value_or_default();
        _starRailOriginalMotionFlipY = DlssNrMotionTextureFlipY.value_or_default();
        _starRailInputCaptured = true;
    }

    DlssNrMotionYInverted.set_volatile_value(StarRailInputMotionYInverted.value_or_default());
    DlssNrDepthTextureFlipY.set_volatile_value(StarRailInputDepthTextureFlipY.value_or_default());
    DlssNrMotionTextureFlipY.set_volatile_value(StarRailInputMotionTextureFlipY.value_or_default());
}

void Config::ReloadStarRailInputProfile(const std::filesystem::path& iniPath)
{
    if (!_starRailInputProfile)
        return;

    // Use a fresh reader: the global SimpleIni reader merges files, and CustomOptional::set_from_config
    // intentionally ignores already populated values. Profile reloads must respect new explicit false values.
    CSimpleIniA profileIni;
    profileIni.LoadFile(iniPath.c_str());
    auto readProfileBool = [&](const char* section, const char* key) -> std::optional<bool> {
        const auto* value = profileIni.GetValue(section, key, nullptr);
        if (value != nullptr && _stricmp(value, "true") == 0)
            return true;
        if (value != nullptr && _stricmp(value, "false") == 0)
            return false;
        return std::nullopt;
    };

    // Restore the generic config snapshot before applying the effective per-game values.
    DlssNrMotionYInverted = readProfileBool("DlssNr", "MotionYInverted");
    DlssNrDepthTextureFlipY = readProfileBool("DlssNr", "DepthTextureFlipY");
    DlssNrMotionTextureFlipY = readProfileBool("DlssNr", "MotionTextureFlipY");
    StarRailInputMotionYInverted = readProfileBool("SR_input", "MotionYInverted").value_or(true);
    StarRailInputDepthTextureFlipY = readProfileBool("SR_input", "DepthTextureFlipY").value_or(true);
    StarRailInputMotionTextureFlipY = readProfileBool("SR_input", "MotionTextureFlipY").value_or(true);

    _starRailOriginalMotionY = readProfileBool("SR_input", "OriginalMotionYInverted")
                                 .value_or(DlssNrMotionYInverted.value_or_default());
    _starRailOriginalDepthFlipY = readProfileBool("SR_input", "OriginalDepthTextureFlipY")
                                   .value_or(DlssNrDepthTextureFlipY.value_or_default());
    _starRailOriginalMotionFlipY = readProfileBool("SR_input", "OriginalMotionTextureFlipY")
                                    .value_or(DlssNrMotionTextureFlipY.value_or_default());
    _starRailInputCaptured = true;
    ApplyStarRailInputProfile();
}
