#include "pch.h"

#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

#include "menu_common.h"
#include "MenuHotpathContext.h"
#include "MenuPages.h"
#include "NextgenUi.h"
#include "NextgenF5Panels.h"
#include "UiLocalization.h"
#include "CriticalErrorWindow.h"
#include "GenshinFsrHintController.h"
#include <genshin/GenshinHookHintPresentation.h>
#include <genshin/GenshinLegacyCapture.h>
#include <mods/gimi/GimiRuntime.h>
#include <uid/DrawUidPanel.h>
#include <inputs/GenshinFsrContract.h>
#include <framegen/dlssg/MfgUnlock.h>
#include <framegen/dlssg/AmpereMfgLoader.h>
#include <framegen/dlssg/DLSSG_Dx12.h>
#include <dlssnr/DlssNr_ExposureScan.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <optional>
#include <utility>

#include <dlssnr/DlssNr.h>
#include <native/VirtualUpscalerDriver.h>
#include <native/NativeLowLatency.h>
#include <genshin/GenshinHookCpuTiming.h>
#include <native/VkPresentBridge.h>

#include "input/input_system.h"
#include <wrapped/wrapped_swapchain.h>

#include "font/Hack_Compressed.h"
#include "font/Inter_Compressed.h"

#include <proxies/XeSS_Proxy.h>
#include <proxies/XeFG_Proxy.h>
#include <proxies/FfxApi_Proxy.h>
#include <proxies/Streamline_Proxy.h>

#include <framegen/nvngx/Nvngx_FG.h>

#include <nvapi/fakenvapi.h>
#include <hooks/Reflex_Hooks.h>

#include <version_check.h>

#include <upscaler_time/UpscalerTime_Vk.h>

#include <imgui/imgui_internal.h>
#include <imgui/ImGuiNotify.hpp>
#include <imgui/imgui_impl_win32.h>
#include <imgui/imgui_impl_uwp.h>

#include <mutex>
#include <cstdarg>

#include <array>
#include <chrono>
#include <memory>
#include <type_traits>
#include <misc/IdentifyGpu.h>
#include <hooks/Xell_Hooks.h>
#include <low_latency/input/input_common.h>

enum class UiTargetMode
{
    SDR,
    LinearHDR,
    ScRGB,
    PQ,
    HLG
};

#define MARK_ALL_BACKENDS_CHANGED()                                                                                    \
    for (auto& singleChangeBackend : State::Instance().changeBackend)                                                  \
        singleChangeBackend.second = true;

static float fontSize = 14.0f; // just changing this doesn't make other elements scale ideally
static ImVec2 overlaySize(0.0f, 0.0f);
static ImVec2 overlayPosition(-1000.0f, -1000.0f);
static bool _hdrTonemapApplied = false;
static bool nextgenThemePending = false;
static bool nextgenStyleActive = false;
static bool nextgenRebuildPending = false;
static float nextgenPendingScale = 0.0f;
static ImVec4 SdrColors[ImGuiCol_COUNT];

// Modern style: colours that are drawn outside style.Colors (already tone-mapped for HDR output).
struct ModernPalette
{
    ImVec4 card;
    ImVec4 cardHeader;
    ImVec4 cardHeaderHover;
    ImVec4 cardHeaderActive;
    ImVec4 navBar;
    ImVec4 navSelected;
    ImVec4 navHover;
    ImVec4 navActive;
};

static bool gModernActive = false;
static ModernPalette gModernPalette {};
static ImFont* gHackFont = nullptr;
static ImFont* gInterFont = nullptr;
static ImFont* gClassicFont = nullptr;

bool MenuStyle::IsModern() { return gModernActive; }
bool MenuStyle::IsNextgen() { return nextgenStyleActive; }
ImFont* MenuStyle::MonoFont() { return gHackFont; }
const ImVec4& MenuStyle::CardColor() { return gModernPalette.card; }
const ImVec4& MenuStyle::CardHeaderColor() { return gModernPalette.cardHeader; }
const ImVec4& MenuStyle::CardHeaderHoverColor() { return gModernPalette.cardHeaderHover; }
const ImVec4& MenuStyle::CardHeaderActiveColor() { return gModernPalette.cardHeaderActive; }

static bool inputMenu = false;
static bool inputFG = false;
static bool inputFps = false;
static bool inputFpsCycle = false;
static uint64_t lastInputTick = 0;
static uint64_t lastShortcutFireTick = 0;
constexpr uint64_t debounceThreshold = 1000;

static bool hasGamepad = false;
static bool ffxInitTried = false;
static bool xefgInitTried = false;
static std::string windowTitle;
static std::string selectedUpscalerName = "";
static Upscaler currentBackend = Upscaler::Reset;
static std::string currentBackendName = "";
static int refreshRate = 0;
static ImVec2 lastPosition(-1000.0f, -1000.0f);
static CriticalUi::GenshinFsrHintController genshinCriticalHints;
static bool criticalErrorsCollapsed = false;
static CriticalUi::Rect criticalMainMenuBounds {};
static std::uint64_t criticalLastSampleTick = 0;

// One deadline for this process, independent of menus, devices, controls or
// advice transitions. Reuse the cached process identity, not the first UI draw.
// Convert UTC creation age once; later clock adjustments cannot reopen it.
static const std::uint64_t paimonAdviceDeadlineTick = []() noexcept -> std::uint64_t {
    constexpr std::uint64_t lifetime100ns = 120ULL * 1000 * 10000;
    const auto created = GenshinLegacy::Ui::ProcessStartIdentity();
    if (!created) return 0; // Unknown startup age must not open a fresh window.
    const auto tick = GetTickCount64();
    FILETIME current {};
    GetSystemTimeAsFileTime(&current);
    const auto utc = (std::uint64_t(current.dwHighDateTime) << 32) | current.dwLowDateTime;
    if (utc < created || utc - created >= lifetime100ns) return 0;
    // Round remaining time down, so the advice never gets an extra millisecond.
    return tick + (lifetime100ns - (utc - created)) / 10000;
}();

static void UpdateGenshinCriticalHints(const State& state, Config* config)
{
    const auto now = GetTickCount64(); // Same monotonic clock as captured Render tokens.
    if (criticalLastSampleTick && now >= criticalLastSampleTick &&
        now - criticalLastSampleTick < CriticalUi::GenshinFsrHintPolicy::SampleIntervalMs) return;
    criticalLastSampleTick = now;
    const auto controls = config->AcquireGenshinControls();
    const auto observation = GenshinLegacy::Ui::ReadObservation();
    // The native snapshot must describe the current controls, never a previous config.
    const bool currentRoute = controls.Active() && observation.session.controlsEpoch == controls.epoch;
    genshinCriticalHints.Update(observation, GetCurrentProcessId(), GenshinLegacy::Ui::ProcessStartIdentity(),
        GetTickCount64(), state.frameCount, static_cast<std::uint32_t>((std::max)(state.screenWidth, 0.0f)),
        static_cast<std::uint32_t>((std::max)(state.screenHeight, 0.0f)), currentRoute,
        config->DlssNrEnabled.value_or_default() ? GenshinLoading::NrMode::Requested : GenshinLoading::NrMode::Disabled);
}

// How many frames DLSS frame generation adds per real frame (0: off). A game's own DLSS frame generation reports it on
// each evaluate; OptiScaler's own DLSSG output (OptiFG input, e.g. Optical F5Low's upscaler) gets no such report back,
// so there it is the count OptiScaler asked for while its frame generation runs.
static int DlssgInterpolationCount(const State& state)
{
    // Genshin's provider activity/requested multiplier is not a measured
    // generated-frame count. Do not inflate its FPS/ON indicators from it.
    static const bool genshinProcess = GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring());
    if (genshinProcess && state.activeFgInput == FGInput::Upscaler && state.activeFgOutput == FGOutput::DLSSG)
        return 0;
    if (const int reported = state.dlssgDetectedInterpolationCount; reported > 0)
        return reported;

    const auto fg = state.currentFG;

    if (state.activeFgOutput == FGOutput::DLSSG && state.activeFgInput == FGInput::Upscaler && fg != nullptr &&
        fg->IsActive() && !fg->IsPaused())
    {
        return (int) fg->GetInterpolatedFrameCount();
    }

    return 0;
}

static ImVec2 splashPosition(-1000.0f, -1000.0f);
static ImVec2 splashSize(0.0f, 0.0f);
static double splashStart = 0.0;
static double splashLimit = 0.0;
static std::vector<std::string> splashText = { "巧用超分，轻松畅玩",
                                               "超分之力与你同在…",
                                               "精彩从这里开始…",
                                               "还有更多超分方案吗？",
                                               "生成像素，生成帧…",
                                               "生成帧准备好了…",
                                               "为像素和帧数而来…",
                                               "别忘了试试超采样。",
                                               "逐帧提升分辨率！",
                                               "你的像素即将升级。",
                                               "低分辨率不是问题。",
                                               "DLSS，我们更上一层！",
                                               "再找找合适的分辨率。",
                                               "向更高帧率出发…",
                                               "检查一下帧调度吧。",
                                               "带上这个超分工具再出发。",
                                               "超分之后，焕然一新。",
                                               "相信流程，也留意闪烁。",
                                               "生成帧，已就绪。",
                                               "性能的魔法",
                                               "经典超分，值得收藏。",
                                               "原生渲染也有替代选择。",
                                               "超分越多，开销越少。",
                                               "升级显卡永远不晚。",
                                               "让生成像素助力画面。",
                                               "Intel 已为更多显卡开放 XeFG。",
                                               "Nukem 多倍插帧，100%% 认真推荐。",
                                               "这里也有原生像素！",
                                               "别只盯着一个像素看。",
                                               "也支持软件 XeSS！",
                                               "画面模糊？试试 RCAS。",
                                               "感谢 nitec！",
                                               "由 By-U 测试认可。",
                                               "0.8 版本的小秘密。",
                                               "期待 AMD FSR4 DP4a。",
                                               "超分爱好者集合！",
                                               "让画面以超分方式呈现。",
                                               "愿今天游戏稳定运行。",
                                               "扩展与增强",
                                               "今天已经崩溃五次了。",
                                               "插帧延迟与网络延迟不同。",
                                               "更多 PC 画面设置。",
                                               "放松眼睛，享受游戏。",
                                               "大胆的超分比例。",
                                               "输入延迟似乎更低了。",
                                               "向 60 FPS 出发。",
                                               "一起提升分辨率。",
                                               "为超分爱好者而作。",
                                               "精彩尽在采样中。",
                                               "你的世界，我们来超分。",
                                               "让像素焕发新生。",
                                               "让更多玩家使用超分。",
                                               "自 2023 年起探索插帧。",
                                               "自 2023 年起支持 DLSS。",
                                               "[已隐去] 从未如此好看。",
                                               "免费，并始终免费。",
                                               "正在拓展显卡支持…",
                                               "Nukem 是谁？",
                                               "正在编译着色器…预计 05h:49m。",
                                               "这款游戏花了 70 欧元吗？",
                                               "又漏了空指针检查。",
                                               "AI 也追不上这个点子。",
                                               "现在我们是早期演示版了。",
                                               "新应用登场：TH。",
                                               "再卡一下就要受不了了。",
                                               "大体稳定，驱动另说。",
                                               "Vulkan 是什么？——AMD",
                                               "我的 8 位是浮点。",
                                               "我只在 -128 到 127 之间。",
                                               "先生成，再完善。",
                                               "实在不行，关掉再开启。",
                                               "几何层面的生成修正模式。",
                                               "深度学习采样 5",
                                               "2D AI 滤镜，搭配两张 5090。",
                                               "DLSS5 神经采样。",
                                               "DLSS5 的生成画面。",
                                               "刚想退出，又被超分吸引。",
                                               "高速路上别只用一档。",
                                               "Nitec 的奇妙超分。",
                                               "插帧吸引着各路玩家。",
                                               "想关闭这些玩笑提示吗？",
                                               "<在这里填写你的提示>" };

static float lastMenuScale = 0.0f;
static CustomOptional<uint32_t> comboPreset { 0 };
static int lastKey = 0;
static bool inputDlssNr = false;
static bool inputCurrentOutputCapture = false;
static bool capturingKey = false;

// Keep an explicit open request across renderer/window recreation. Final shutdown and
// an invalid HWND revoke it; transient menu rendering failures leave it recoverable.
static bool menuOpenRequested = false;
static bool menuIntentEligibleForRebuild = false;

static bool IsAuthenticatedGenshinRebuildWindow(HWND hwnd) noexcept
{
    return GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring()) &&
        hwnd != nullptr && GenshinNativeParentFrame::IsRetainedChildWindow(hwnd);
}

template <typename T, size_t N> struct RingBuffer
{
    std::array<T, N> data {};
    size_t head { 0 };
    size_t count { N };
    double sum { 0.0 };

    RingBuffer() { data.fill(static_cast<T>(0)); }

    void Push(T v)
    {
        if (count == N)
        {
            sum -= data[head];
        }
        else
        {
            ++count;
        }
        data[head] = v;
        sum += v;
        head = (head + 1) % N;
    }

    size_t Size() const { return N; }

    T At(size_t i) const
    {
        size_t start = head;
        return data[(start + i) % N];
    }

    float Average() const { return static_cast<float>(sum / static_cast<double>(N)); }
};

const int plotWidth = 360;
static RingBuffer<float, plotWidth> gFrameTimes;
static RingBuffer<float, plotWidth> gUpscalerTimes;

struct FsExistsCache
{
    std::wstring lastPath;
    bool cached { false };
    std::chrono::steady_clock::time_point nextRefresh { std::chrono::steady_clock::time_point::min() };
    std::chrono::milliseconds interval { 2000 };

    bool Get(const std::filesystem::path& path)
    {
        auto now = std::chrono::steady_clock::now();
        if (path != lastPath || now >= nextRefresh)
        {
            lastPath = path;
            cached = std::filesystem::exists(path);
            nextRefresh = now + interval;
        }
        return cached;
    }
};

static FsExistsCache nukemsExists;
static FsExistsCache enablerExists;

struct FlagDefinition
{
    std::string name;
    uint32_t mask;
    std::string description;
};

inline std::string StrFmt(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    const auto translatedFormat = MenuText::Translate(fmt);
    int len = std::vsnprintf(nullptr, 0, translatedFormat.c_str(), args);
    va_end(args);
    std::string out(len, '\0');
    va_start(args, fmt);
    std::vsnprintf(out.data(), len + 1, translatedFormat.c_str(), args);
    va_end(args);
    return MenuText::Translate(out);
}

// The authenticated Genshin FG child is an output window. Keep its input
// on the same native owner as native Presents, including backend re-init.
// Switching away from a foreign subclass chain can otherwise lose the saved
// original and reinstall OptiInput above a chain that still calls OptiInput.
static HWND ResolveMenuInputOwner(HWND hwnd) noexcept
{
    if (!hwnd || !GenshinNativeParentFrame::IsRetainedChildWindow(hwnd)) return hwnd;
    const auto root = GetAncestor(hwnd, GA_ROOT);
    DWORD pid = 0;
    if (!root || root == hwnd || !IsWindow(root) ||
        !GetWindowThreadProcessId(root, &pid) || pid != GetCurrentProcessId()) return hwnd;
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
        LOG_INFO("R827 phase35 input owner: pid={} child={:X} native={:X} retained_child=true",
            GetCurrentProcessId(), reinterpret_cast<uintptr_t>(hwnd), reinterpret_cast<uintptr_t>(root));
    return root;
}

// FG alone requires an explicit opt-in and a new press/release after every interruption.
// Keep its arm separate: another shortcut can share this VK (including the menu key).
struct FgShortcutState
{
    int key = 0;
    HWND owner = nullptr;
    bool accepting = false;
    bool waitForRelease = true;
    bool armed = false;
};
static FgShortcutState fgShortcut;

static void ResetFgShortcut()
{
    fgShortcut.accepting = false;
    fgShortcut.waitForRelease = true;
    fgShortcut.armed = false;
    inputFG = false;
}

static bool CanAcceptFgShortcut(Config* config)
{
    const int key = config->FGShortcutKey.value_or_default();
    if (!config->FGShortcutEnabled.value_or_default() || key <= 0 || key >= 256 ||
        capturingKey || GetTickCount64() - lastInputTick <= debounceThreshold ||
        !OptiInput::IsFocused() || ImGui::GetCurrentContext() == nullptr)
        return false;

    const auto& io = ImGui::GetIO();
    return !io.AppFocusLost && !io.WantTextInput && !io.WantCaptureKeyboard && !ImGui::IsAnyItemActive();
}

static void LogFgShortcutPolicy(Config* config, const char* source)
{
    LOG_INFO("FG shortcut policy: source={} Menu.FGShortcutEnabled={} Menu.FGShortcutKey={} "
             "disabled_clears_pending=true fresh_press_release_required=true input_source=UNKNOWN",
             source, config->FGShortcutEnabled.value_or_default(), config->FGShortcutKey.value_or_default());
}

void MenuCommon::UpdateManualInput(HWND targetHwnd)
{
    const auto inputOwner = ResolveMenuInputOwner(targetHwnd);
    OptiInput::BeginFrame(inputOwner);
    if (gimi::runtime::Enabled())
        gimi::runtime::InputFrame(OptiInput::CaptureKeyboardFrame(), !_isVisible && !capturingKey);

    const auto config = Config::Instance();

    const auto currentTick = GetTickCount64();

    // A shortcut fires on key release. Under some input stacks (notably Assetto Corsa + CSP) the
    // same physical tap is observed by more than one acquisition path (WndProc hook, message-queue
    // hook, GetAsyncKeyState poll) on different frames, which produces a second spurious release
    // edge and makes the menu "close then immediately re-open". Require a fresh press edge between
    // fires, plus a short cooldown, so one tap can only toggle once.
    static bool shortcutArmed[256] = {};
    constexpr uint64_t shortcutCooldown = 250;

    auto CheckShortcut = [&](int vk, bool& inputFlag, const char* logMessage)
    {
        if (inputFlag)
            return;

        if (vk <= 0 || vk >= 256)
            return;

        if (OptiInput::IsKeyPressed(vk))
            shortcutArmed[vk] = true;

        if (!shortcutArmed[vk])
            return;

        if (currentTick - lastShortcutFireTick < shortcutCooldown)
            return;

        if (OptiInput::IsKeyReleased(vk))
        {
            shortcutArmed[vk] = false;
            lastShortcutFireTick = currentTick;
            lastKey = vk;
            // receivingWmInputs = false;
            inputFlag = true;
            LOG_DEBUG("{}", logMessage);
        }
    };

    const bool canAcceptInputs = lastInputTick + debounceThreshold < currentTick;

    if (!capturingKey && canAcceptInputs)
    {
        CheckShortcut(config->ShortcutKey.value_or_default(), inputMenu, "Menu key pressed, will be switching menu");
        CheckShortcut(config->FpsShortcutKey.value_or_default(), inputFps, "Menu key pressed, will be switching FPS");
        CheckShortcut(config->FpsCycleShortcutKey.value_or_default(), inputFpsCycle,
                      "Menu key pressed, will be switching FPS mode");
        CheckShortcut(config->DlssNrToggleKey.value_or_default(), inputDlssNr,
                      "Neural Rendering key pressed, will be toggling the pass");
        CheckShortcut(config->CurrentOutputCaptureKey.value_or_default(), inputCurrentOutputCapture,
                      "Current output capture key released");
    }
    else if (capturingKey)
    {
        lastInputTick = currentTick;
    }

    // The common shortcut path intentionally keeps the existing menu-open behavior.
    // FG is sampled separately even while disabled so old pending/armed state is revoked.
    const int fgKey = config->FGShortcutKey.value_or_default();
    if (fgShortcut.key != fgKey || fgShortcut.owner != inputOwner)
    {
        ResetFgShortcut();
        fgShortcut.key = fgKey;
        fgShortcut.owner = inputOwner;
    }

    if (!CanAcceptFgShortcut(config))
    {
        ResetFgShortcut();
    }
    else
    {
        if (!fgShortcut.accepting)
        {
            fgShortcut.accepting = true;
            fgShortcut.waitForRelease = true;
            fgShortcut.armed = false;
            inputFG = false;
        }

        if (fgShortcut.waitForRelease)
        {
            // Discard initial held/synthetic edges after enable, focus, capture or key changes.
            // Observe a quiet up sample before a subsequent press can arm this shortcut.
            if (!OptiInput::IsKeyDown(fgKey) && !OptiInput::IsKeyPressed(fgKey) &&
                !OptiInput::IsKeyReleased(fgKey))
                fgShortcut.waitForRelease = false;
        }
        else if (!inputFG)
        {
            if (OptiInput::IsKeyPressed(fgKey))
                fgShortcut.armed = true;

            if (OptiInput::IsKeyReleased(fgKey))
            {
                const bool fire = fgShortcut.armed && currentTick - lastShortcutFireTick >= shortcutCooldown;
                fgShortcut.armed = false;
                fgShortcut.waitForRelease = true;
                if (fire)
                {
                    inputFG = true;
                    lastShortcutFireTick = currentTick;
                    lastKey = fgKey;
                    LOG_DEBUG("Opted-in FG shortcut released: vk={} input_source=UNKNOWN", fgKey);
                }
            }
        }
    }

    lastKey = OptiInput::GetLastPressedKey();
}

void MenuCommon::ShowTooltip(const char* tip)
{
    MenuText::ItemHelp(tip);
}

void MenuCommon::ShowHelpMarker(const char* tip)
{
    ShowTooltip(tip);
}

void MenuCommon::ShowResetButton(CustomOptional<bool, NoDefault>* initFlag, std::string buttonName)
{
    ImGui::SameLine();

    ImGui::BeginDisabled(!initFlag->has_value());

    if (MenuText::Button(buttonName.c_str()))
    {
        initFlag->reset();
        ReInitUpscaler();
    }

    ImGui::EndDisabled();
}

inline void MenuCommon::ReInitUpscaler()
{
    if (!State::Instance().currentFeature)
        return;

    if (State::Instance().currentFeature->GetUpscalerType() == Upscaler::DLSSD)
        State::Instance().newBackend = Upscaler::DLSSD;
    else
        State::Instance().newBackend = currentBackend;

    MARK_ALL_BACKENDS_CHANGED();
}

void MenuCommon::SeparatorWithHelpMarker(const char* label, const char* tip)
{
    MenuText::SeparatorText(label);
    ShowHelpMarker(tip);
}

class Keybind
{
    std::string name;
    int id;
    bool waitingForKey = false;

  public:
    Keybind(std::string name, int id) : name(name), id(id) {}

    static std::string KeyNameFromVirtualKeyCode(USHORT virtualKey)
    {
        if (virtualKey == (USHORT) UnboundKey)
            return "未绑定";

        UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);

        // Keys like Home would display as Num 0 without this fix
        switch (virtualKey)
        {
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_NUMLOCK:
        case VK_DIVIDE:
        case VK_RCONTROL:
        case VK_RMENU:
            scanCode |= 0xE000;
            break;
        }

        LONG lParam = (scanCode & 0xFF) << 16;
        if (scanCode & 0xE000)
            lParam |= 1 << 24;

        wchar_t buf[64] = {};
        if (GetKeyNameTextW(lParam, buf, static_cast<int>(std::size(buf))) != 0)
            return wstring_to_string(buf);

        return "未知";
    }

    void CancelCapture()
    {
        if (!waitingForKey)
            return;
        waitingForKey = false;
        capturingKey = false;
        lastInputTick = GetTickCount64();
        lastKey = 0;
        ResetFgShortcut();
    }

    void Render(CustomOptional<int>& configKey)
    {
        ImGui::PushID(id);
        if (MenuText::Button(name.c_str()))
        {
            waitingForKey = true;
            capturingKey = true;
            lastKey = 0;
            ResetFgShortcut();
        }
        ImGui::PopID();

        if (waitingForKey)
        {
            ImGui::SameLine();
            MenuText::Text("请按任意键…");

            if (lastKey == 0 || lastKey == VK_LBUTTON || lastKey == VK_RBUTTON || lastKey == VK_MBUTTON)
                return;

            if (lastKey == VK_ESCAPE)
            {
                waitingForKey = false;
                capturingKey = false;
                return;
            }

            if (lastKey == VK_BACK)
                lastKey = UnboundKey;

            configKey = lastKey;
            waitingForKey = false;
            capturingKey = false;
            return;
        }

        ImGui::SameLine();
        MenuText::Text(KeyNameFromVirtualKeyCode(configKey.value_or_default()).c_str());

        ImGui::SameLine();
        ImGui::PushID(id);
        if (MenuText::Button("R"))
        {
            configKey.reset();
        }
        ImGui::PopID();
    }
};

std::string MenuStyle::ShortcutName(int virtualKey)
{
    return Keybind::KeyNameFromVirtualKeyCode(static_cast<USHORT>(virtualKey));
}

Upscaler MenuCommon::GetBackendCode(const API api)
{
    if (auto feature = State::Instance().currentFeature)
        return feature->GetUpscalerType();

    Upscaler upscaler;

    if (api == DX11)
        upscaler = Config::Instance()->Dx11Upscaler.value_or_default();
    else if (api == DX12)
        upscaler = Config::Instance()->Dx12Upscaler.value_or_default();
    else
        upscaler = Config::Instance()->VulkanUpscaler.value_or_default();

    return upscaler;
}

void MenuCommon::GetCurrentBackendInfo(const API api, Upscaler& upscaler, std::string* name)
{
    upscaler = GetBackendCode(api);
    *name = UpscalerDisplayName(upscaler, api);
}

// Init/render already belong to the menu owner. Lifetime distinguishes context
// address reuse; TLS caches store only scalar identities, never an ImGui context.
static std::uint64_t g_menuGpuLifetime = 0;
static thread_local const GpuInformation* g_framePrimaryGpu = nullptr;

struct MenuGpuSnapshotKey
{
    std::uint64_t MenuLifetime;
    std::uint64_t BackendGeneration;
    std::uint64_t OutputGeneration;
    MenuHotpaths::Backend Path;
    std::uintptr_t DeviceIdentity;
    std::uintptr_t RendererIdentity;
    std::uintptr_t FeatureIdentity;
    std::uintptr_t Dx11Identity;
    std::uintptr_t Dx12Identity;
    std::uintptr_t VkIdentity;
    API GameApi;
    Upscaler ActiveBackend;
    Upscaler RequestedBackend;
    Upscaler Dx11Backend;
    Upscaler Dx12Backend;
    Upscaler VulkanBackend;
    bool PreferDedicated;
    bool PreferFirstDedicated;
    bool DoNotLoadAmdxc64;
    bool operator==(const MenuGpuSnapshotKey&) const = default;
};

static const GpuInformation& MenuPrimaryGpu()
{
    // Even a combo changing the requested backend halfway through this frame
    // must not replace a snapshot still borrowed by other section helpers.
    if (g_framePrimaryGpu != nullptr)
        return *g_framePrimaryGpu;

    const auto& state = State::Instance();
    const auto config = Config::Instance();
    const auto* frame = MenuHotpaths::Current;
    const MenuGpuSnapshotKey key {
        g_menuGpuLifetime, frame ? frame->BackendGeneration : 0, frame ? frame->OutputGeneration : 0,
        frame ? frame->Path : MenuHotpaths::Backend::Other, frame ? frame->DeviceIdentity : 0,
        reinterpret_cast<std::uintptr_t>(ImGui::GetIO().BackendRendererUserData),
        reinterpret_cast<std::uintptr_t>(state.currentFeature),
        reinterpret_cast<std::uintptr_t>(state.currentD3D11Device),
        reinterpret_cast<std::uintptr_t>(state.currentD3D12Device),
        reinterpret_cast<std::uintptr_t>(state.currentVkDevice), state.api,
        state.currentFeature ? state.currentFeature->GetUpscalerType() : Upscaler::Reset, state.newBackend,
        config->Dx11Upscaler.value_or_default(), config->Dx12Upscaler.value_or_default(),
        config->VulkanUpscaler.value_or_default(), config->PreferDedicatedGpu.value_or_default(),
        config->PreferFirstDedicatedGpu.value_or_default(), config->Fsr4DoNotLoadAmdxc64.value_or_default()
    };
    struct Cache
    {
        std::optional<MenuGpuSnapshotKey> Key;
        std::optional<GpuInformation> Snapshot;
        ULONGLONG NextRetry = 0;
    };
    static thread_local Cache cache;
    static const GpuInformation unavailable {}; // Unknown, never invented capabilities.
    if (!cache.Key || !(*cache.Key == key))
    {
        cache.Key = key;
        cache.Snapshot.reset();
        cache.NextRetry = 0;
    }
    const ULONGLONG now = GetTickCount64();
    if (now >= cache.NextRetry)
    {
        // Refresh ready capabilities too: identity keys do not track their publication.
        // The frame-borrow guard above defers all replacement until no frame uses it.
        // Between attempts there is no IdentifyGpu lock or GPU-information copy.
        cache.NextRetry = now + 1000;
        try
        {
            auto gpu = IdentifyGpu::getCachedPrimaryGpu();
            if (gpu.vendorId == VendorId::Invalid)
            {
                // Preserve DXVK recursion protection and the caller's original value.
                struct SkipVulkanHooks
                {
                    bool Previous = State::Instance().vulkanSkipHooks;
                    SkipVulkanHooks() { State::Instance().vulkanSkipHooks = true; }
                    ~SkipVulkanHooks() { State::Instance().vulkanSkipHooks = Previous; }
                } skip;
                gpu = IdentifyGpu::getPrimaryGpu();
            }
            if (gpu.vendorId != VendorId::Invalid)
                cache.Snapshot.emplace(std::move(gpu));
            else
                cache.Snapshot.reset(); // Failed refresh is unknown, not stale capabilities.
        }
        catch (...)
        {
            cache.Snapshot.reset(); // Discovery/copy failure must also remain recoverable.
        }
        // Bound both ready refreshes and failed retries after slow calls complete.
        cache.NextRetry = GetTickCount64() + 1000;
    }
    return cache.Snapshot ? *cache.Snapshot : unavailable;
}

class MenuGpuFrameScope final
{
    const GpuInformation* Previous;

  public:
    explicit MenuGpuFrameScope(const GpuInformation* gpu) noexcept : Previous(g_framePrimaryGpu)
    { g_framePrimaryGpu = gpu; }
    ~MenuGpuFrameScope() { g_framePrimaryGpu = Previous; }
    MenuGpuFrameScope(const MenuGpuFrameScope&) = delete;
    MenuGpuFrameScope& operator=(const MenuGpuFrameScope&) = delete;
};

void MenuCommon::RenderUpscalerCombo(const API api, Upscaler currentUpscaler, const std::vector<Upscaler>& options)
{
    const auto& primaryGpu = MenuPrimaryGpu();

    // Determine display name
    Upscaler targetBackend = State::Instance().newBackend;
    if (targetBackend == Upscaler::Reset)
        targetBackend = currentUpscaler;

    std::string selectedName = UpscalerDisplayName(targetBackend, api);

    // Apply the same GPU/API filter the combo used, then fold a short list into the house
    // single-select group. Longer filtered lists (five or more) keep the combo unchanged.
    std::vector<Upscaler> available;
    available.reserve(options.size());
    for (auto opt : options)
    {
        // Check if GPU is capable of a given backend
        if (opt == Upscaler::DLSS && !primaryGpu.dlssCapable)
            continue;

        // DLSS on D3D12 for a Vulkan game needs the GPU and the DLSS library just the same
        if (opt == Upscaler::DLSS_on12 && api == API::Vulkan &&
            (!primaryGpu.dlssCapable || !State::Instance().NVNGX_DLSS_Path.has_value()))
            continue;

        // Not all Intel GPUs support native DX11 XeSS but don't think we have a good way to check exactly
        if (opt == Upscaler::XeSS && api == API::DX11 && primaryGpu.vendorId != VendorId::Intel)
            continue;

        available.push_back(opt);
    }

    if (!available.empty() && available.size() < 5)
    {
        std::vector<std::string> labels;
        std::vector<const char*> labelPointers;
        labels.reserve(available.size());
        labelPointers.reserve(available.size());
        // -1 keeps the group unselected when the active backend is filtered out; picking a
        // button then sets the new backend explicitly.
        int selected = -1;
        for (size_t i = 0; i < available.size(); ++i)
        {
            labels.push_back(UpscalerDisplayName(available[i], api));
            if (targetBackend == available[i])
                selected = (int) i;
        }
        for (const auto& label : labels)
            labelPointers.push_back(label.c_str());

        int choice = selected;
        if (MenuText::Segmented("##UpscalerCombo", &choice, labelPointers.data(), (int) available.size()) &&
            choice >= 0 && choice < (int) available.size())
            State::Instance().newBackend = available[(size_t) choice];
        return;
    }

    if (MenuText::BeginCombo("##UpscalerCombo", selectedName.c_str()))
    {
        for (auto opt : available)
        {
            bool isSelected = (currentUpscaler == opt);
            if (MenuText::Selectable(UpscalerDisplayName(opt, api).c_str(), isSelected))
            {
                State::Instance().newBackend = opt;
            }
        }
        ImGui::EndCombo();
    }
}

void MenuCommon::AddDx11Backends(Upscaler upscaler)
{
    RenderUpscalerCombo(API::DX11, upscaler,
                        { Upscaler::XeSS, Upscaler::FSR22, Upscaler::FSR31, Upscaler::XeSS_on12, Upscaler::FSR21_on12,
                          Upscaler::FSR22_on12, Upscaler::FFX_on12, Upscaler::DLSS, Upscaler::DLSS_on12 });
}

void MenuCommon::AddDx12Backends(Upscaler upscaler)
{
    RenderUpscalerCombo(API::DX12, upscaler,
                        { Upscaler::XeSS, Upscaler::FSR21, Upscaler::FSR22, Upscaler::FFX, Upscaler::DLSS });
}

void MenuCommon::AddVulkanBackends(Upscaler upscaler)
{
    // "FG only (game's upscaler)" feeds frame generation from a Vulkan-on-D3D12 backend only: the others would leave it
    // without inputs (upscalers/FeatureProvider_Vk.h)
    if (Config::Instance()->DlssNrNativeFrameGenerationOnly.value_or_default())
    {
        RenderUpscalerCombo(API::Vulkan, upscaler, { Upscaler::DLSS_on12, Upscaler::FFX_on12, Upscaler::FSR21_on12 });
        ShowHelpMarker("已启用 Optical F5Low 仅插帧模式，只列出可提供插帧输入的 D3D12 超分副本。退出此模式后可选其他后端。");
        return;
    }

    RenderUpscalerCombo(API::Vulkan, upscaler,
                        { Upscaler::XeSS, Upscaler::FSR21, Upscaler::FSR22, Upscaler::FFX, Upscaler::FSR21_on12,
                          Upscaler::FFX_on12, Upscaler::DLSS });
}

template <HasDefaultValue B> void MenuCommon::AddResourceBarrier(std::string name, CustomOptional<int32_t, B>* value)
{
    const char* states[] = { "自动",
                             "COMMON",
                             "VERTEX_AND_CONSTANT_BUFFER",
                             "INDEX_BUFFER",
                             "RENDER_TARGET",
                             "UNORDERED_ACCESS",
                             "DEPTH_WRITE",
                             "DEPTH_READ",
                             "NON_PIXEL_SHADER_RESOURCE",
                             "PIXEL_SHADER_RESOURCE",
                             "STREAM_OUT",
                             "INDIRECT_ARGUMENT",
                             "COPY_DEST",
                             "COPY_SOURCE",
                             "RESOLVE_DEST",
                             "RESOLVE_SOURCE",
                             "RAYTRACING_ACCELERATION_STRUCTURE",
                             "SHADING_RATE_SOURCE",
                             "GENERIC_READ",
                             "ALL_SHADER_RESOURCE",
                             "PRESENT",
                             "PREDICATION",
                             "VIDEO_DECODE_READ",
                             "VIDEO_DECODE_WRITE",
                             "VIDEO_PROCESS_READ",
                             "VIDEO_PROCESS_WRITE",
                             "VIDEO_ENCODE_READ",
                             "VIDEO_ENCODE_WRITE" };
    const int values[] = { -1,  0,   1,     2,      4,      8,      16,      32,       64,   128,
                           256, 512, 1024,  2048,   4096,   8192,   4194304, 16777216, 2755, 192,
                           0,   310, 65536, 131072, 262144, 524288, 2097152, 8388608 };

    int selected = value->value_or(-1);

    const char* selectedName = "";

    for (int n = 0; n < 28; n++)
    {
        if (values[n] == selected)
        {
            selectedName = states[n];
            break;
        }
    }

    if (MenuText::BeginCombo(name.c_str(), selectedName))
    {
        if (MenuText::Selectable(states[0], !value->has_value()))
            value->reset();

        for (int n = 1; n < 28; n++)
        {
            if (MenuText::Selectable(states[n], selected == values[n]))
                *value = values[n];
        }

        ImGui::EndCombo();
    }
}

static uint32_t GetPresetIndex(IFeature* feature, bool dlssd = false)
{
    auto ratio = (float) feature->TargetWidth() / (float) feature->RenderWidth();

    if (!dlssd)
    {
        if (State::Instance().dlssPresetsOverridenByOpti)
        {
            LOG_DEBUG("DLSS Presets overridden by Opti, using Opti preset indices with ratio: {}", ratio);

            if (ratio <= (Config::Instance()->QualityRatio_UltraPerformance.value_or_default() + 0.01f))
            {
                return Config::Instance()->RenderPresetForAll.value_or(
                    Config::Instance()->RenderPresetUltraPerformance.value_or_default());
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Performance.value_or_default() + 0.01f))
            {
                return Config::Instance()->RenderPresetForAll.value_or(
                    Config::Instance()->RenderPresetPerformance.value_or_default());
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Balanced.value_or_default() + 0.01f))
            {
                return Config::Instance()->RenderPresetForAll.value_or(
                    Config::Instance()->RenderPresetBalanced.value_or_default());
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Quality.value_or_default() + 0.01f))
            {
                return Config::Instance()->RenderPresetForAll.value_or(
                    Config::Instance()->RenderPresetQuality.value_or_default());
            }
            else if (ratio <= (Config::Instance()->QualityRatio_UltraQuality.value_or_default() + 0.01f))
            {
                return Config::Instance()->RenderPresetForAll.value_or(
                    Config::Instance()->RenderPresetUltraQuality.value_or_default());
            }
            else
            {
                return Config::Instance()->RenderPresetForAll.value_or(
                    Config::Instance()->RenderPresetDLAA.value_or_default());
            }
        }
        else if (State::Instance().dlssPresetsOverriddenExternally)
        {
            LOG_DEBUG("DLSS Presets overridden externally, using external preset index: {}",
                      State::Instance().dlssRenderPresetExternal);

            return State::Instance().dlssRenderPresetExternal;
        }
        else
        {
            if (ratio <= (Config::Instance()->QualityRatio_UltraPerformance.value_or_default() + 0.01f))
            {
                return State::Instance().dlssRenderPresetUltraPerformance;
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Performance.value_or_default() + 0.01f))
            {
                return State::Instance().dlssRenderPresetPerformance;
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Balanced.value_or_default() + 0.01f))
            {
                return State::Instance().dlssRenderPresetBalanced;
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Quality.value_or_default() + 0.01f))
            {
                return State::Instance().dlssRenderPresetQuality;
            }
            else if (ratio <= (Config::Instance()->QualityRatio_UltraQuality.value_or_default() + 0.01f))
            {
                return State::Instance().dlssRenderPresetUltraQuality;
            }
            else
            {
                return State::Instance().dlssRenderPresetDLAA;
            }
        }
    }
    else
    {
        if (State::Instance().dlssdPresetsOverridenByOpti)
        {
            if (ratio <= (Config::Instance()->QualityRatio_UltraPerformance.value_or_default() + 0.01f))
            {
                return Config::Instance()->DLSSDRenderPresetForAll.value_or(
                    Config::Instance()->DLSSDRenderPresetUltraPerformance.value_or_default());
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Performance.value_or_default() + 0.01f))
            {
                return Config::Instance()->DLSSDRenderPresetForAll.value_or(
                    Config::Instance()->DLSSDRenderPresetPerformance.value_or_default());
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Balanced.value_or_default() + 0.01f))
            {
                return Config::Instance()->DLSSDRenderPresetForAll.value_or(
                    Config::Instance()->DLSSDRenderPresetBalanced.value_or_default());
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Quality.value_or_default() + 0.01f))
            {
                return Config::Instance()->DLSSDRenderPresetForAll.value_or(
                    Config::Instance()->DLSSDRenderPresetQuality.value_or_default());
            }
            else if (ratio <= (Config::Instance()->QualityRatio_UltraQuality.value_or_default() + 0.01f))
            {
                return Config::Instance()->DLSSDRenderPresetForAll.value_or(
                    Config::Instance()->DLSSDRenderPresetUltraQuality.value_or_default());
            }
            else
            {
                return Config::Instance()->DLSSDRenderPresetForAll.value_or(
                    Config::Instance()->DLSSDRenderPresetDLAA.value_or_default());
            }
        }
        else if (State::Instance().dlssdPresetsOverriddenExternally)
        {
            return State::Instance().dlssdRenderPresetExternal;
        }
        else
        {
            if (ratio <= (Config::Instance()->QualityRatio_UltraPerformance.value_or_default() + 0.01f))
            {
                return State::Instance().dlssdRenderPresetUltraPerformance;
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Performance.value_or_default() + 0.01f))
            {
                return State::Instance().dlssdRenderPresetPerformance;
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Balanced.value_or_default() + 0.01f))
            {
                return State::Instance().dlssdRenderPresetBalanced;
            }
            else if (ratio <= (Config::Instance()->QualityRatio_Quality.value_or_default() + 0.01f))
            {
                return State::Instance().dlssdRenderPresetQuality;
            }
            else if (ratio <= (Config::Instance()->QualityRatio_UltraQuality.value_or_default() + 0.01f))
            {
                return State::Instance().dlssdRenderPresetUltraQuality;
            }
            else
            {
                return State::Instance().dlssdRenderPresetDLAA;
            }
        }
    }

    return 0;
}

// TODO: disable presets based on the detected DLSS version
template <HasDefaultValue B> void MenuCommon::AddDLSSRenderPreset(std::string name, CustomOptional<uint32_t, B>* value)
{
    // clang-format off
    static const std::vector<MenuOption<uint32_t>> presets = {
        { NVSDK_NGX_DLSS_Hint_Render_Preset_Default, "默认", 
            "跟随游戏设置" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_A, "预设 A",
            "适合性能、均衡和质量模式的旧变体，用于减少拖影。近期版本已移除。" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_B, "预设 B",
            "适合极致性能模式，与预设 A 类似。近期版本已移除。" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_C, "预设 C",
            "适合性能、均衡和质量模式，更偏重当前帧信息。近期版本已移除。" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_D, "预设 D",
            "性能、均衡和质量模式的旧默认值，偏重画面稳定。近期版本已移除。" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_E, "预设 E",
            "DLSS 3.7 以上的改进 D 预设。近期版本已移除。" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_F, "预设 F",
            "极致性能与 DLAA 的旧默认预设，近期版本已移除。" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_G, "预设 G",
            "未使用" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_H_Reserved, "预设 H",
            "未使用" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_I_Reserved, "预设 I",
            "未使用" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_J, "预设 J",
            "类似 K，可能拖影略少。第一代 Transformer。" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_K, "预设 K",
            "DLAA、均衡和质量的默认预设，第一代 Transformer。" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_L, "预设 L",
            "极致性能默认预设，第二代 Transformer。" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_M, "预设 M",
            "性能默认预设，第二代 Transformer。" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_N, "预设 N",
            "未使用" },
        { NVSDK_NGX_DLSS_Hint_Render_Preset_O, "预设 O",
            "未使用" },
        { NV_PRESET_LATEST, "最新",
            "DLL 支持的最新预设。" }
    };
    // clang-format on

    PopulateCombo(name, *value, presets);
}

template <HasDefaultValue B> void MenuCommon::AddDLSSDRenderPreset(std::string name, CustomOptional<uint32_t, B>* value)
{
    // We don't have DLSSD definitions so using raw values
    static const std::vector<MenuOption<uint32_t>> presets = {
        { 0, "默认", "跟随游戏设置" },
        { 1, "预设 A", "预设 A，近期版本已移除。" },
        { 2, "预设 B", "预设 B，近期版本已移除。" },
        { 3, "预设 C", "预设 C，近期版本已移除。" },
        { 4, "预设 D", "默认 Transformer 模型。" },
        { 5, "预设 E", "最新 Transformer 模型，需要景深引导时须使用。" },
        { 6, "预设 F", "最新 Transformer 模型，需要景深引导时须使用。" },
        { NV_PRESET_LATEST, "最新", "DLL 支持的最新预设。" }
    };

    PopulateCombo(name, *value, presets);
}

template <typename TStorage, typename T>
void MenuCommon::PopulateCombo(const std::string& name, TStorage& currentValue,
                               const std::vector<MenuOption<T>>& options)
{
    if (options.empty())
        return;

    // Assumes that different types mean that TStorage is std::optional
    T currentVal;
    if constexpr (std::is_same_v<TStorage, T>)
        currentVal = currentValue;
    else
        currentVal = currentValue.value_or(options[0].value);

    // Find the label for the currently selected item
    std::string preview = "未知";
    for (const auto& opt : options)
    {
        if (opt.value == currentVal)
        {
            preview = opt.label;
            break;
        }
    }

    if (MenuText::BeginCombo(name.c_str(), preview.c_str()))
    {
        for (const auto& opt : options)
        {
            if (opt.hidden)
                continue;

            if (opt.disabled)
                ImGui::BeginDisabled();

            bool isSelected = (currentVal == opt.value);
            if (MenuText::Selectable(opt.label.c_str(), isSelected))
                currentValue = opt.value;

            // Show tooltip for the individual item if it exists
            if (!opt.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                MenuText::SetTooltip("%s", opt.tooltip.c_str());

            if (opt.disabled)
                ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
}

// Fold a short MenuOption list (1..4 visible entries) into the house single-select group.
// Longer or unproven lists keep the caller's existing combo by returning false here, so the
// stored value contract, disabled states and per-option tooltips survive either presentation.
template <typename TStorage, typename T>
bool TryPopulateComboGroup(const std::string& name, TStorage& currentValue,
                           const std::vector<MenuOption<T>>& options)
{
    int visibleCount = 0;
    for (const auto& opt : options)
        if (!opt.hidden)
            ++visibleCount;

    if (options.empty() || visibleCount == 0 || visibleCount > 4)
        return false;

    T currentVal;
    if constexpr (std::is_same_v<TStorage, T>)
        currentVal = currentValue;
    else
        currentVal = currentValue.value_or(options[0].value);

    std::vector<const char*> names;
    std::vector<const char*> tips;
    std::vector<T> values;
    bool enabled[4] = { true, true, true, true };
    names.reserve((size_t) visibleCount);
    tips.reserve((size_t) visibleCount);
    values.reserve((size_t) visibleCount);

    // -1 keeps the group unselected when the stored value is not one of the visible
    // entries (for example a hidden None); picking a button then commits explicitly.
    int selected = -1;
    for (const auto& opt : options)
    {
        if (opt.hidden)
            continue;
        const int index = (int) values.size();
        if (opt.value == currentVal)
            selected = index;
        names.push_back(opt.label.c_str());
        enabled[index] = !opt.disabled;
        tips.push_back(opt.tooltip.empty() ? nullptr : opt.tooltip.c_str());
        values.push_back(opt.value);
    }

    if (MenuText::Segmented(name.c_str(), &selected, names.data(), visibleCount, enabled, nullptr,
                            tips.data()))
        currentValue = values[(size_t) selected];
    return true;
}

static UiTargetMode getUiTargetMode()
{
    const auto& state = State::Instance();

    const bool fallback = !Config::Instance()->OverlayMenu.value_or_default();

    if (fallback)
    {
        // We have no reliable swapchain/output color-space information here.
        // Only classify the upscaled working image.
        if (state.currentFeature && state.currentFeature->IsHdr())
            return UiTargetMode::LinearHDR;

        return UiTargetMode::SDR;
    }

    const auto& output = state.outputColorSpace;

    // If SetColorSpace1 has not provided a known/valid color space,
    // fall back conservatively.
    if (!output.valid)
        return UiTargetMode::SDR;

    switch (output.transfer)
    {
    case ColorTransfer::Linear:
        // scRGB: linear Rec.709 RGB.
        //
        // hdrOutputActive is intentionally NOT required here.
        // A scRGB swapchain is still linear even when the physical output
        // is currently SDR. hdrOutputActive only affects the desired
        // reference-white scaling in toneMapColor().
        if (output.model == ColorModel::RGB && output.primaries == ColorPrimaries::Rec709)
        {
            return UiTargetMode::ScRGB;
        }

        break;

    case ColorTransfer::PQ:
        // Direct PQ UI rendering currently assumes RGB PQ / Rec.2020.
        //
        // Do not treat YCbCr PQ as an RGB render target.
        if (output.model == ColorModel::RGB && output.primaries == ColorPrimaries::Rec2020)
        {
            return UiTargetMode::PQ;
        }

        break;

    case ColorTransfer::HLG:
        // Current HLG UI path assumes an RGB render target.
        //
        // DXGI HLG modes are commonly YCbCr, so reject unsupported
        // combinations rather than applying an RGB HLG transform blindly.
        if (output.model == ColorModel::RGB && output.primaries == ColorPrimaries::Rec2020)
        {
            return UiTargetMode::HLG;
        }

        break;

    case ColorTransfer::SRGB:
        return UiTargetMode::SDR;

    case ColorTransfer::Unknown:
    default:
        break;
    }

    // Unsupported model / primaries / transfer combination.
    return UiTargetMode::SDR;
}

static float srgbToLinear(float x)
{
    x = std::clamp(x, 0.0f, 1.0f);

    if (x <= 0.04045f)
        return x / 12.92f;

    return std::pow((x + 0.055f) / 1.055f, 2.4f);
}

static float linearToPQ(float nits)
{
    // SMPTE ST.2084
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 32.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 128.0f;
    constexpr float c3 = 2392.0f / 128.0f;

    const float y = std::clamp(nits / 10000.0f, 0.0f, 1.0f);
    const float ym1 = std::pow(y, m1);

    return std::pow((c1 + c2 * ym1) / (1.0f + c3 * ym1), m2);
}

static float linearToHLG(float x)
{
    // BT.2100 HLG OETF
    constexpr float a = 0.17883277f;
    constexpr float b = 0.28466892f;
    constexpr float c = 0.55991073f;

    x = std::max(x, 0.0f);

    if (x <= (1.0f / 12.0f))
        return std::sqrt(3.0f * x);

    return a * std::log(12.0f * x - b) + c;
}

static ImVec4 linear709To2020(float r, float g, float b)
{
    return ImVec4(0.6274040f * r + 0.3292820f * g + 0.0433136f * b, 0.0690970f * r + 0.9195400f * g + 0.0113612f * b,
                  0.0163916f * r + 0.0880132f * g + 0.8955950f * b, 0.0f);
}

static ImVec4 legacyHdrToneMap(const ImVec4& color)
{
    constexpr float exposure = 1.0f;
    constexpr float strength = 1.0f;

    const float peak = std::max(color.x, std::max(color.y, color.z));

    if (peak <= 0.0f)
        return color;

    const float exposedPeak = peak * exposure;
    const float mappedPeak = exposedPeak / (1.0f + exposedPeak);

    const float reinhardScale = mappedPeak / peak;
    const float scale = 1.0f + (reinhardScale - 1.0f) * strength;

    return ImVec4(color.x * scale, color.y * scale, color.z * scale, color.w);
}

static ImVec4 toneMapColor(const ImVec4& color)
{
    const auto mode = getUiTargetMode();

    switch (mode)
    {
    case UiTargetMode::SDR:
        return color;

    case UiTargetMode::LinearHDR:
        return ImVec4(srgbToLinear(color.x), srgbToLinear(color.y), srgbToLinear(color.z), color.w);

    case UiTargetMode::ScRGB:
    {
        constexpr float scRgbReferenceWhiteNits = 80.0f;
        constexpr float hdrUiWhiteNits = 203.0f;

        const float uiWhiteNits = State::Instance().hdrOutputActive ? hdrUiWhiteNits : scRgbReferenceWhiteNits;

        const float scale = uiWhiteNits / scRgbReferenceWhiteNits;

        return ImVec4(srgbToLinear(color.x) * scale, srgbToLinear(color.y) * scale, srgbToLinear(color.z) * scale,
                      color.w);
    }

    case UiTargetMode::PQ:
        // Direct ImGui rendering into a nonlinear PQ target.
        //
        // Proper PQ encoding of vertex colors produces incorrect results
        // with the standard ImGui alpha blend state because blending then
        // happens in PQ space.
        //
        // Keep the known-good legacy compression until PQ rendering is
        // moved to a linear intermediate/composite pass.
        return legacyHdrToneMap(color);

    case UiTargetMode::HLG:
        // Same fundamental nonlinear-blending problem as PQ.
        // Conservative compatibility behavior for now.
        return legacyHdrToneMap(color);

    default:
        return color;
    }
}

static void MenuHdrCheck(ImGuiIO io)
{
    if (!_hdrTonemapApplied)
    {
        ImGuiStyle& style = ImGui::GetStyle();
        const auto mode = getUiTargetMode();

        LOG_INFO("Output HDR: {}, UI Mode: {}", State::Instance().hdrOutputActive, magic_enum::enum_name(mode));

        CopyMemory(SdrColors, style.Colors, sizeof(style.Colors));

        // Apply tone mapping to the ImGui style
        for (int i = 0; i < ImGuiCol_COUNT; ++i)
        {
            ImVec4 color = style.Colors[i];
            style.Colors[i] = toneMapColor(color);
        }

        _hdrTonemapApplied = true;
    }
}

static float MenuResolutionScale(ImGuiIO io)
{
    if (Config::Instance()->MenuScale.has_value())
        return Config::Instance()->MenuScale.value();

    // Calculate menu scale according to display resolution
    float y = State::Instance().screenHeight;

    if (io.DisplaySize.y != 0)
        y = (float) io.DisplaySize.y;

    // 1000p is minimum for 1.0 menu ratio
    float result = (float) ((int) (y / 108.0f)) / 10.0f;

    result = std::round(result * 10.0f) / 10.0f;

    if (result < 0.5f)
        result = 0.5f;

    if (result > 2.0f)
        result = 2.0f;

    return result;
}

inline static std::string GetSourceString(UINT source)
{
    switch (source)
    {
    case 1:
        return "RTV";
    case 2:
        return "SRV";
    case 4:
        return "UAV";
    case 8:
        return "OM";
    case 16:
        return "Ups";
    case 32:
        return "SCR";
    case 64:
        return "SGR";
    default:
        return std::format("{}", source);
    }
}

inline static std::string GetDispatchString(UINT source)
{
    switch (source)
    {
    case 512:
        return "DI";
    case 1024:
        return "DII";
    case 256:
        return "Disp";
    default:
        return std::format("{}", source);
    }
}

void MenuCommon::ApplyThemeStyle()
{
    if (ImGui::GetCurrentContext() == nullptr)
        return;

    ImGuiStyle& style = ImGui::GetStyle();

    auto conf = Config::Instance();
    bool lightTheme = conf->LightTheme.value_or_default();

    const bool modern = conf->ModernTheme.value_or_default();
    gModernActive = modern;

    // Base (unscaled) values; RebuildStyle scales them with Menu Scale.
    if (modern)
    {
        style.WindowRounding = 8.0f;
        style.ChildRounding = 6.0f;
        style.FrameRounding = 5.0f;
        style.PopupRounding = 6.0f;
        style.ScrollbarRounding = 6.0f;
        style.GrabRounding = 4.0f;
        style.TabRounding = 5.0f;

        style.WindowPadding = ImVec2(12.0f, 10.0f);
        style.FramePadding = ImVec2(8.0f, 5.0f);
        style.ItemSpacing = ImVec2(8.0f, 6.0f);
        style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);

        style.ChildBorderSize = 0.0f;
        style.FrameBorderSize = 0.0f;
        style.TabBorderSize = 0.0f;
        style.ScrollbarSize = 8.0f;
    }
    else
    {
        style.WindowRounding = 2.0f;
        style.ChildRounding = 1.0f;
        style.FrameRounding = 2.0f;
        style.PopupRounding = 2.0f;
        style.ScrollbarRounding = 2.0f;
        style.GrabRounding = 2.0f;
        style.TabRounding = 2.0f;

        style.WindowPadding = ImVec2(8.0f, 8.0f);
        style.FramePadding = ImVec2(4.0f, 3.0f);
        style.ItemSpacing = ImVec2(8.0f, 4.0f);
        style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);

        style.ChildBorderSize = 1.0f;
        style.FrameBorderSize = lightTheme ? 1.0f : 0.0f;
        style.TabBorderSize = lightTheme ? 1.0f : 0.0f;
        style.ScrollbarSize = 10.0f;
    }

    style.WindowBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.GrabMinSize = 10.0f;

    auto Clamp01 = [](float v) { return std::max(0.0f, std::min(v, 1.0f)); };

    auto Mix = [](const ImVec4& a, const ImVec4& b, float t, float alpha = 1.0f)
    { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, alpha); };

    auto Luminance = [](const ImVec4& c) { return c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f; };

    auto Saturate = [&](const ImVec4& color, float amount)
    {
        float lum = Luminance(color);

        return ImVec4(Clamp01(lum + (color.x - lum) * amount), Clamp01(lum + (color.y - lum) * amount),
                      Clamp01(lum + (color.z - lum) * amount), color.w);
    };

    ImVec4 accent = ImVec4(conf->MenuAccentColorR.value_or_default(), conf->MenuAccentColorG.value_or_default(),
                           conf->MenuAccentColorB.value_or_default(), 1.0f);

    ImVec4 bgAccent = ImVec4(conf->MenuBGColorR.value_or_default(), conf->MenuBGColorG.value_or_default(),
                             conf->MenuBGColorB.value_or_default(), 1.0f);

    float luminance = Luminance(accent);

    const ImVec4 bgDark = lightTheme ? ImVec4(0.80f, 0.82f, 0.86f, 1.00f) : ImVec4(0.09f, 0.09f, 0.10f, 1.00f);
    const ImVec4 bgMid = lightTheme ? ImVec4(0.89f, 0.91f, 0.95f, 1.00f) : ImVec4(0.11f, 0.11f, 0.12f, 1.00f);
    const ImVec4 bgLight = lightTheme ? ImVec4(0.96f, 0.97f, 0.99f, 1.00f) : ImVec4(0.14f, 0.14f, 0.15f, 1.00f);

    const ImVec4 textPrimary = lightTheme ? ImVec4(0.05f, 0.06f, 0.08f, 1.00f) : ImVec4(0.90f, 0.93f, 0.95f, 1.00f);
    const ImVec4 textDim = lightTheme ? ImVec4(0.22f, 0.25f, 0.31f, 1.00f) : ImVec4(0.54f, 0.58f, 0.62f, 1.00f);

    const ImVec4 borderCol = lightTheme ? ImVec4(0.35f, 0.40f, 0.50f, 1.00f) : ImVec4(0.24f, 0.24f, 0.26f, 1.00f);
    const ImVec4 dimBg = lightTheme ? ImVec4(0.30f, 0.33f, 0.38f, 0.20f) : ImVec4(0.09f, 0.10f, 0.13f, 0.20f);
    const ImVec4 modalDimBg = lightTheme ? ImVec4(0.22f, 0.24f, 0.28f, 0.55f) : ImVec4(0.04f, 0.04f, 0.07f, 0.55f);

    // MenuBGColor: only background/surface tint.
    auto BgTint = [&](const ImVec4& base, float strength = 1.0f, float alpha = 1.0f)
    {
        float t = lightTheme ? (0.180f * strength) : (0.120f * strength);
        return Mix(base, bgAccent, t, alpha);
    };

    // MenuAccentColor: all visible interactive accent colors.
    auto AccentSoft = [&](float alpha = 1.0f)
    { return lightTheme ? Mix(bgLight, accent, 0.14f, alpha) : Mix(bgDark, accent, 0.32f, alpha); };

    auto AccentMed = [&](float alpha = 1.0f)
    { return lightTheme ? Mix(bgLight, accent, 0.42f, alpha) : Mix(bgDark, accent, 0.55f, alpha); };

    auto AccentStrong = [&](float alpha = 1.0f) { return ImVec4(accent.x, accent.y, accent.z, alpha); };

    const ImVec4 bgTitle = AccentSoft();

    auto SurfaceHover = [&](float alpha = 1.0f)
    { return lightTheme ? Mix(bgLight, accent, 0.12f, alpha) : Mix(bgLight, accent, 0.18f, alpha); };

    auto SurfaceActive = [&](float alpha = 1.0f)
    { return lightTheme ? Mix(bgLight, accent, 0.20f, alpha) : Mix(bgLight, accent, 0.28f, alpha); };

    auto TitleActive = [&](float alpha = 1.0f)
    { return lightTheme ? Mix(bgTitle, accent, 0.18f, alpha) : Mix(bgTitle, accent, 0.16f, alpha); };

    auto PlotAccent = [&](float alpha = 1.0f)
    {
        if (lightTheme)
        {
            // Darken slightly for contrast on light bg — no channel floors
            return Mix(accent, ImVec4(0.00f, 0.00f, 0.00f, 1.00f), 0.20f, alpha);
        }

        // Brighten slightly for visibility on dark bg — no channel floors
        return Mix(accent, ImVec4(1.00f, 1.00f, 1.00f, 1.00f), 0.35f, alpha);
    };

    auto PlotAccentHovered = [&](float alpha = 1.0f)
    {
        if (lightTheme)
        {
            return Mix(PlotAccent(alpha), ImVec4(0.00f, 0.00f, 0.00f, 1.00f), 0.15f, alpha);
        }

        return Mix(PlotAccent(alpha), ImVec4(1.00f, 1.00f, 1.00f, 1.00f), 0.25f, alpha);
    };

    auto AccentReadable = [&](float alpha = 1.0f)
    {
        // Apply saturation boost and luminance correction only here,
        // so AccentStrong / AccentMed / AccentSoft stay true to the user's pick.
        ImVec4 a = Saturate(accent, lightTheme ? 1.35f : 1.25f);
        float lum = Luminance(a);

        if (lightTheme && lum > 0.72f)
            a = Mix(a, ImVec4(0.0f, 0.0f, 0.0f, 1.0f), 0.35f, 1.0f);

        if (!lightTheme && lum < 0.25f)
            a = Mix(a, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.30f, 1.0f);

        return ImVec4(a.x, a.y, a.z, alpha);
    };

    ImVec4* c = ImGui::GetStyle().Colors;

    float minAlpha = Config::Instance()->MenuBGColorA.value_or_default() >= 0.5f
                         ? Config::Instance()->MenuBGColorA.value_or_default()
                         : 0.5f;

    c[ImGuiCol_Text] = textPrimary;
    c[ImGuiCol_TextDisabled] = textDim;
    c[ImGuiCol_TextLink] = AccentReadable();

    // MenuBGColor only.
    c[ImGuiCol_WindowBg] = BgTint(bgDark, 1.00f, Config::Instance()->MenuBGColorA.value_or_default());
    c[ImGuiCol_ChildBg] = BgTint(bgMid, 1.10f, minAlpha + 0.1f);
    c[ImGuiCol_PopupBg] =
        lightTheme ? BgTint(bgLight, 0.90f) : BgTint(ImVec4(0.09f, 0.10f, 0.13f, 0.97f), 0.90f, 0.97f);
    c[ImGuiCol_MenuBarBg] = BgTint(bgDark, 0.85f);
    c[ImGuiCol_DockingEmptyBg] = BgTint(bgDark, 0.75f);

    c[ImGuiCol_Border] = borderCol;
    c[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    // Neutral background, not MenuBGColor.
    c[ImGuiCol_FrameBg] = BgTint(bgLight, 0.50f, minAlpha + 0.15f);
    c[ImGuiCol_FrameBgHovered] = SurfaceHover();
    c[ImGuiCol_FrameBgActive] = SurfaceActive();

    c[ImGuiCol_TitleBg] = BgTint(bgTitle, 0.40f);
    c[ImGuiCol_TitleBgActive] = TitleActive();
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(bgTitle.x, bgTitle.y, bgTitle.z, 0.75f);

    c[ImGuiCol_ScrollbarBg] = BgTint(bgDark, 0.60f, minAlpha + 0.2f);
    c[ImGuiCol_ScrollbarGrab] = AccentSoft();
    c[ImGuiCol_ScrollbarGrabHovered] = AccentMed();
    c[ImGuiCol_ScrollbarGrabActive] = AccentStrong();

    c[ImGuiCol_CheckMark] = AccentReadable();
    c[ImGuiCol_SliderGrab] = AccentMed();
    c[ImGuiCol_SliderGrabActive] = AccentReadable();
    c[ImGuiCol_InputTextCursor] = AccentReadable();

    c[ImGuiCol_Button] = AccentSoft();
    c[ImGuiCol_ButtonHovered] = AccentMed();
    c[ImGuiCol_ButtonActive] = AccentStrong();

    c[ImGuiCol_Header] = AccentSoft(0.90f);
    c[ImGuiCol_HeaderHovered] = AccentMed(0.95f);
    c[ImGuiCol_HeaderActive] = AccentStrong();

    c[ImGuiCol_Separator] = borderCol;
    c[ImGuiCol_SeparatorHovered] = AccentMed(0.85f);
    c[ImGuiCol_SeparatorActive] = AccentStrong();

    c[ImGuiCol_ResizeGrip] = AccentSoft(0.30f);
    c[ImGuiCol_ResizeGripHovered] = AccentStrong(0.70f);
    c[ImGuiCol_ResizeGripActive] = AccentStrong(0.95f);

    c[ImGuiCol_Tab] = AccentSoft();
    c[ImGuiCol_TabHovered] = AccentMed();
    c[ImGuiCol_TabSelected] = AccentSoft();
    c[ImGuiCol_TabSelectedOverline] = AccentStrong();
    c[ImGuiCol_TabDimmed] = BgTint(bgDark, 0.60f);
    c[ImGuiCol_TabDimmedSelected] = AccentSoft(0.75f);
    c[ImGuiCol_TabDimmedSelectedOverline] = borderCol;

    c[ImGuiCol_DockingPreview] = AccentStrong(0.70f);

    c[ImGuiCol_PlotLines] = PlotAccent();
    c[ImGuiCol_PlotLinesHovered] = PlotAccentHovered();
    c[ImGuiCol_PlotHistogram] = PlotAccent(0.85f);
    c[ImGuiCol_PlotHistogramHovered] = PlotAccentHovered();

    c[ImGuiCol_TableHeaderBg] = BgTint(bgMid, 0.80f, minAlpha + 0.25f);
    c[ImGuiCol_TableBorderStrong] = borderCol;
    c[ImGuiCol_TableBorderLight] = lightTheme ? ImVec4(0.68f, 0.72f, 0.80f, 1.00f) : AccentSoft();
    c[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = lightTheme ? ImVec4(0.00f, 0.00f, 0.00f, 0.045f) : ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

    c[ImGuiCol_TreeLines] = borderCol;
    c[ImGuiCol_TextSelectedBg] = AccentMed(0.38f);
    c[ImGuiCol_DragDropTarget] = AccentStrong(0.90f);
    c[ImGuiCol_NavCursor] = AccentReadable();
    c[ImGuiCol_NavWindowingHighlight] = AccentStrong(0.70f);
    c[ImGuiCol_NavWindowingDimBg] = dimBg;
    c[ImGuiCol_ModalWindowDimBg] = modalDimBg;

    if (modern)
    {
        // Layers: window (bgDark) < panes (bgMid) < section cards (bgLight); inputs sit inset in the cards.
        // Same alpha as ChildBg, so Background Alpha reaches the cards too.
        const ImVec4 card =
            Mix(BgTint(bgLight, 0.80f), textPrimary, lightTheme ? 0.00f : 0.03f, std::min(minAlpha + 0.1f, 1.0f));
        const ImVec4 inset = BgTint(bgDark, 0.85f);
        const float insetAlpha = std::min(minAlpha + 0.15f, 1.0f);

        c[ImGuiCol_Border] = Mix(bgDark, textPrimary, 0.16f);
        c[ImGuiCol_Separator] = Mix(card, textPrimary, 0.14f);
        c[ImGuiCol_TableBorderStrong] = c[ImGuiCol_Separator];
        c[ImGuiCol_TableBorderLight] = Mix(card, textPrimary, 0.10f);
        c[ImGuiCol_ScrollbarBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

        c[ImGuiCol_FrameBg] = ImVec4(inset.x, inset.y, inset.z, insetAlpha);
        c[ImGuiCol_FrameBgHovered] = Mix(inset, accent, lightTheme ? 0.12f : 0.18f);
        c[ImGuiCol_FrameBgActive] = Mix(inset, accent, lightTheme ? 0.20f : 0.28f);

        c[ImGuiCol_Header] = Mix(card, accent, lightTheme ? 0.14f : 0.18f, 0.90f);
        c[ImGuiCol_HeaderHovered] = Mix(card, accent, lightTheme ? 0.24f : 0.30f, 0.95f);
        c[ImGuiCol_HeaderActive] = Mix(card, accent, lightTheme ? 0.36f : 0.42f);

        gModernPalette.card = toneMapColor(card);
        gModernPalette.cardHeader = toneMapColor(ImVec4(card.x, card.y, card.z, 0.0f));
        gModernPalette.cardHeaderHover = toneMapColor(Mix(card, accent, lightTheme ? 0.12f : 0.18f));
        gModernPalette.cardHeaderActive = toneMapColor(Mix(card, accent, lightTheme ? 0.20f : 0.28f));
        gModernPalette.navBar = toneMapColor(AccentReadable());
        gModernPalette.navSelected = toneMapColor(AccentReadable(lightTheme ? 0.18f : 0.22f));
        gModernPalette.navHover = toneMapColor(AccentReadable(0.26f));
        gModernPalette.navActive = toneMapColor(AccentReadable(0.34f));
    }

    _hdrTonemapApplied = false;
    MenuHdrCheck(ImGui::GetIO());
}

// Every theme, font-affecting and Menu Scale change goes through here so none of them drops the scale.
void MenuCommon::RebuildStyle(float menuScale)
{
    // Settings remain authoritative; rebuild only after temporary NEXT styles are gone.
    if (nextgenStyleActive)
    {
        nextgenRebuildPending = true;
        nextgenPendingScale = menuScale;
        return;
    }

    if (ImGui::GetCurrentContext() == nullptr)
        return;

    if (menuScale <= 0.0f)
        menuScale = lastMenuScale > 0.0f ? lastMenuScale : 1.0f;

    ImGuiStyle& style = ImGui::GetStyle();

    // ScaleAllSizes is lossy and changes the stored sizes, so start from a fresh style.
    style = ImGuiStyle();

    ApplyThemeStyle();

    style.ScaleAllSizes(menuScale);
    style.MouseCursorScale = 1.0f;
}

void MenuCommon::ApplyThemeFont()
{
    if (ImGui::GetCurrentContext() == nullptr || gClassicFont == nullptr)
        return;

    const bool useInter = Config::Instance()->ModernTheme.value_or_default() && gInterFont != nullptr;
    ImGui::GetIO().FontDefault = useInter ? gInterFont : gClassicFont;
}

// Live numbers keep the fixed-width font so digits don't jitter when the default is proportional.
static void PushMonoFontSize(float size)
{
    if (gHackFont != nullptr)
        ImGui::PushFont(gHackFont, size);
    else
        ImGui::PushFontSize(size);
}

static double lastTime = 0.0;
static double lastFrameTime = 0.0;
static UINT64 uwpTargetFrame = 0;

void MenuCommon::Present()
{
    _frameCount++;

    auto now = Util::MillisecondsNow();

    if (lastTime > 0.0)
        lastFrameTime = now - lastTime;

    lastTime = now;

    if (_handle != nullptr)
        UpdateManualInput(_handle);
}

struct VersionCheckStatus
{
    bool completed = false;
    bool updateAvailable = false;
    std::string latestTag;
    std::string latestUrl;
    std::string error;
};

struct MenuCommon::RenderMenuContext
{
    State& state;
    decltype(Config::Instance()) config;
    ImGuiIO& io;
    IFeature* currentFeature = nullptr;

    double now = 0.0;
    double frameTime = 0.0;
    double frameRate = 0.0;
    float menuResScale = 1.0f;
    float fpsScale = 1.0f;
    float averageFrameTime = 0.0f;
    float averageUpscalerFT = 0.0f;

    bool frameTimesCalculated = false;
    bool newFrame = false;

    VersionCheckStatus versionStatus;
    std::string currentVersionText;

    // Borrowed only for this frame from the menu owner's immutable snapshot.
    const GpuInformation* primaryGpu = nullptr;
};

static std::string splashMessage;

void MenuCommon::UpdateRenderTiming(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& now = ctx.now;
    auto& frameTime = ctx.frameTime;
    auto& frameRate = ctx.frameRate;

    if (config->OverlayMenu.value_or_default())
    {
        _frameCount++;

        // FPS & frame time calculation
        if (lastTime > 0.0)
        {
            frameTime = now - lastTime;
            frameRate = 1000.0 / frameTime;
        }

        lastTime = now;

        if (_handle != nullptr)
            UpdateManualInput(_handle);
    }
    else
    {
        if (state.activeFgInput == FGInput::NoFG || state.activeFgOutput == FGOutput::NoFG)
            MenuCommon::Present();

        frameTime = lastFrameTime;
        frameRate = 1000.0 / frameTime;
    }

    // A dxvk game on the Vulkan bridge: the menu is drawn on the game's presents only, frame generation's swapchain
    // presents the generated frames past our hooks. The shown frames come that many times as often.
    if (auto fg = state.currentFG; VkPresentBridge::MenuOnGamePresents() && fg != nullptr && fg->IsActive() &&
                                   !fg->IsPaused() && frameTime > 0.0)
    {
        frameTime /= (double) (fg->GetInterpolatedFrameCount() + 1);
        frameRate = 1000.0 / frameTime;
    }

    state.frameTimes.pop_front();
    state.frameTimes.push_back(frameTime);
}

void MenuCommon::UpdateMenuInputMode(RenderMenuContext& ctx)
{
    auto& io = ctx.io;

    // Moved here to prevent gamepad key replay
    if (_isVisible)
    {
        if (hasGamepad)
            io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

        io.ConfigFlags = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    }
    else
    {
        capturingKey = false;
        hasGamepad = (io.BackendFlags & ImGuiBackendFlags_HasGamepad) != 0;
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
        io.ConfigFlags = ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoKeyboard;
    }
}

void MenuCommon::HandleMenuShortcuts(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& io = ctx.io;

    // Handle Inputs
    {
        if (inputCurrentOutputCapture)
        {
            inputCurrentOutputCapture = false;
            DlssNr::RequestCurrentOutputCapture();
        }
        // Worker publishes plain data only; ImGui notifications stay on the menu thread.
        static unsigned long long captureStatusRevision = 0;
        const auto capture = DlssNr::CurrentOutputCaptureStatus();
        if (capture.revision != captureStatusRevision)
        {
            captureStatusRevision = capture.revision;
            ImGuiToast toast { capture.failed ? ImGuiToastType::Warning : ImGuiToastType::Info, 6000 };
            toast.setTitle("Current output capture");
            toast.setContent("%s%s%s", capture.message.c_str(), capture.directory.empty() ? "" : "\n",
                             capture.directory.c_str());
            ImGui::InsertNotification(toast);
        }
        // Recheck at consumption: no queued event can bypass opt-in, focus or UI capture.
        if (!CanAcceptFgShortcut(config) || !fgShortcut.accepting ||
            fgShortcut.key != config->FGShortcutKey.value_or_default())
            ResetFgShortcut();

        if (inputFG)
        {
            inputFG = false;

            const bool genshinDlssgRequest = GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring()) &&
                !state.externalFrameGeneration && state.activeFgInput == FGInput::Upscaler &&
                state.activeFgOutput == FGOutput::DLSSG;
            if (genshinDlssgRequest ||
                (state.activeFgInput != FGInput::NoFG && state.activeFgOutput != FGOutput::NoFG &&
                 (state.currentFGSwapchain != nullptr || state.activeFgInput == FGInput::NvngxFG)))
            {
                config->FGEnabled = !config->FGEnabled.value_or_default();
                LOG_DEBUG("Opted-in FG shortcut setting FGEnabled to {} input_source=UNKNOWN",
                          config->FGEnabled.value_or_default());

                if (config->FGEnabled.value_or_default())
                {
                    state.fgChanged = true;
                    if (genshinDlssgRequest)
                        if (auto* provider = dynamic_cast<DLSSG_Dx12*>(state.currentFG))
                            provider->RequestGenshinResume();
                }
            }
        }

        if (inputFps)
        {
            inputFps = false;
            config->ShowFps = !config->ShowFps.value_or_default();
        }

        if (inputDlssNr)
        {
            inputDlssNr = false;
            config->DlssNrEnabled = !config->DlssNrEnabled.value_or_default();
            LOG_DEBUG("Neural Rendering toggle key pressed, setting DlssNrEnabled to {}",
                      config->DlssNrEnabled.value_or_default());

            ImGuiToast toast { ImGuiToastType::Info, 2000 };
            toast.setTitle(MenuText::Translate("DLSS 神经渲染").c_str());
            toast.setContent("%s", MenuText::Translate(config->DlssNrEnabled.value_or_default() ? "On" : "关闭").c_str());
            ImGui::InsertNotification(toast);
        }

        if (inputFpsCycle && config->ShowFps.value_or_default())
            config->FpsOverlayType = (FpsOverlay) ((config->FpsOverlayType.value_or_default() + 1) % FpsOverlay_COUNT);

        if (inputMenu)
        {
            inputMenu = false;
            ResetFgShortcut();
            _isVisible = !_isVisible;
            menuOpenRequested = _isVisible;
            if (!_isVisible)
                menuIntentEligibleForRebuild = false;

            LOG_DEBUG("Menu key pressed, {0}", _isVisible ? "opening ImGui" : "closing ImGui");

            if (_isVisible)
            {
                io.ClearEventsQueue();
                io.ClearInputKeys();
                io.ClearInputMouse();

                OptiInput::ResetMenuInputTransientState();

                RebuildStyle();

                refreshRate = Util::GetActiveRefreshRate(_handle);

                auto optiPath = std::filesystem::path(Config::Instance()->MainDllPath.value());
                state.artursFgFileAvailable = enablerExists.Get(optiPath / L"dlss-enabler-headless.dll");
                state.nukemsFgFileAvailable = nukemsExists.Get(optiPath / L"dlssg_to_fsr3_amd_is_better.dll");

                if (State::Instance().currentFeature != nullptr)
                {
                    if (State::Instance().currentFeature->GetUpscalerType() == Upscaler::DLSSD)
                        comboPreset = config->DLSSDRenderPresetForAll.value_or_default();
                    else if (State::Instance().currentFeature->GetUpscalerType() == Upscaler::DLSS)
                        comboPreset = config->RenderPresetForAll.value_or_default();
                }
            }
            else
            {
                ImGui::CloseCurrentPopup();

                _showMipmapCalcWindow = false;
                _showHudlessWindow = false;
            }

            io.MouseDrawCursor = _isVisible;
            io.WantCaptureKeyboard = _isVisible;
            io.WantCaptureMouse = _isVisible;
        }

        inputFpsCycle = false;
    }
}

void MenuCommon::UpdateVersionAndStartupNotifications(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& now = ctx.now;

    constexpr double splashTime = 7000.0;
    // One-shot startup warning notifications.
    if (!state.postDone)
    {
        if (state.postCodes & PostCode::SlPluginsAlreadyInMemory)
        {
            auto filename = Util::DllPath().filename().string();
            to_lower_in_place(filename);

            ImGuiToast notification { ImGuiToastType::Warning, 10000 };
            notification.setTitle(MenuText::Translate("检测到延迟加载的 Streamline 钩子").c_str());
            notification.setContent(
                "建议将 OptiScaler 从 %s 改为其他支持的加载文件名，否则可能出现异常。",
                filename.c_str());
            ImGui::InsertNotification(notification);
        }

        if (state.postCodes & PostCode::TryingFsr4Fp8OnUnsupported)
        {
            ImGuiToast notification { ImGuiToastType::Warning, 10000 };
            notification.setTitle(MenuText::Translate("配置可能不匹配").c_str());
            notification.setContent(MenuText::Translate("FSR4 FP8 仅支持 AMD 显卡").c_str());
            ImGui::InsertNotification(notification);
        }

        state.postDone = true;
    }

    // Initialize splash timing and select the splash text once per process.
    if (splashLimit < 1.0f)
    {
        splashStart = now + 100.0;
        splashLimit = splashStart + splashTime;

        std::srand(static_cast<unsigned>(std::time(nullptr)));
        splashMessage = splashText[std::rand() % splashText.size()];
    }
}

void MenuCommon::BeginMenuFrameIfNeeded(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& io = ctx.io;
    auto& now = ctx.now;
    auto& newFrame = ctx.newFrame;

    // New frame check
    // The lamp is drawn while the menu is closed, which is the whole point of it. Tied to its own
    // setting and nothing else: an overlay that appears because a scan is running, rather than
    // because someone asked for it, is an overlay nobody asked for.
    const bool scanIndicator = config->DlssNrScanMeter.value_or_default() &&
                               DlssNr::ExposureScan::Where() != DlssNr::ExposureScan::Verdict::Off;

    // These are demand categories, not proof of visible pixels. A toast may
    // expire while being built; StarRail's existing notice path can be empty.
    const bool notificationDemand = config->IsStarRailInputProfile() || ImGui::notifications.size() > 0 ||
                                    genshinCriticalHints.Notices().Size() > 0;
    const bool otherOverlayDemand =
        (!config->DisableSplash.value_or_default() && now > splashStart && now < splashLimit) || scanIndicator ||
        (config->DlssNrCompare.value_or_default() != 0 && config->DlssNrCompareTags.value_or_default());
    if (auto* frame = MenuHotpaths::Current)
    {
        frame->MenuVisible = _isVisible;
        frame->FpsOverlay = config->ShowFps.value_or_default();
        frame->NotificationDemand = notificationDemand;
        frame->OtherOverlayDemand = otherOverlayDemand;
    }

    // UpdateF5LowHint's closed-menu notice still needs an initialized frame.
    if (otherOverlayDemand || notificationDemand || config->ShowFps.value_or_default() || _isVisible)
    {
        if (!_isUWP)
        {
            ImGui_ImplWin32_NewFrame();
        }
        else
        {
            ImVec2 displaySize { state.screenWidth, state.screenHeight };
            ImGui_ImplUwp_NewFrame(displaySize);
        }

        OptiInput::FeedImGui(_isVisible);

        MenuHdrCheck(io);
        ImGui::NewFrame();

        newFrame = true;
    }
}

void MenuCommon::RenderSplashWindow(RenderMenuContext& ctx)
{
    auto config = ctx.config;
    auto& io = ctx.io;
    auto& now = ctx.now;

    constexpr double fadeTime = 1000.0;

    // Splash screen
    if (!config->DisableSplash.value_or_default())
    {
        if (now > splashStart && now < splashLimit)
        {

            ImGui::SetNextWindowSize({ 0.0f, 0.0f });
            ImGui::SetNextWindowBgAlpha(config->FpsOverlayAlpha.value_or_default());
            ImGui::SetNextWindowPos(splashPosition, ImGuiCond_Always);

            float windowAlpha = 1.0f;
            if (auto diff = now - splashStart; diff < fadeTime)
                windowAlpha = static_cast<float>(diff / fadeTime);
            else if (auto diff = splashLimit - now; diff < fadeTime)
                windowAlpha = static_cast<float>(diff / fadeTime);

            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, windowAlpha);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));

            if (!config->OverlaysUseTheme.value_or_default())
            {
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, toneMapColor(ImVec4(1.0f, 1.0f, 1.0f, 1.0f)));
            }

            if (MenuText::Begin("启动提示", nullptr,
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration |
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav))
            {
                float splashScale = 1.0f;
                float baseScaleHeight = 720.0f;

                if (io.DisplaySize.y > baseScaleHeight)
                    splashScale = io.DisplaySize.y / baseScaleHeight;

                if (config->UseHQFont.value_or_default())
                    ImGui::PushFontSize(std::round(splashScale * fontSize));
                else
                    ImGui::SetWindowFontScale(splashScale);

                MenuText::Text("F6 —— 按 %s 打开菜单",
                            Keybind::KeyNameFromVirtualKeyCode(config->ShortcutKey.value_or_default()).c_str());
                MenuText::TextColored(toneMapColor(ImVec4(1.0f, 1.0f, 1.0f, 0.7f)), splashMessage.c_str());

                splashSize = ImGui::GetWindowSize();

                if (config->UseHQFont.value_or_default())
                    ImGui::PopFontSize();

                ImGui::End();

                splashPosition.x = 0.0f; // io.DisplaySize.x - splashWinSize.x;
                splashPosition.y = io.DisplaySize.y - splashSize.y;
            }

            if (!config->OverlaysUseTheme.value_or_default())
                ImGui::PopStyleColor(4);
            else
                ImGui::PopStyleColor(2);

            ImGui::PopStyleVar(2);
        }
    }
}

void MenuCommon::RenderNotifications(RenderMenuContext& ctx)
{
    auto config = ctx.config;
    auto& io = ctx.io;

    // Notifications
    const UiTargetMode uiTargetMode = getUiTargetMode();
    const bool tonemapRequired = uiTargetMode != UiTargetMode::SDR;

    float screenHeight = State::Instance().screenHeight;
    if (io.DisplaySize.y != 0)
        screenHeight = io.DisplaySize.y;

    // Map resolution height to scale, 0.5 for 480p, 2.0 for 1440p
    constexpr float slope = (2.0f - 0.5f) / (1440.f - 480.f);
    float notificationScale = 0.5f + slope * (screenHeight - 480.f);
    notificationScale = std::clamp(notificationScale, 0.5f, 2.0f);

    if (config->UseHQFont.value_or_default())
        ImGui::PushFontSize(std::round(notificationScale * fontSize));

    // No fallback font, SetWindowFontScale needs to be called after Begin()

    // Non-StarRail only updates the hint timer/enqueues its toast before frame demand is checked.
    // Keep StarRail's existing window draw inside the initialized frame and scaled font scope.
    if (config->IsStarRailInputProfile())
        DlssNr::UpdateF5LowHint(config);

    ImGui::RenderNotifications(ImGuiToastPos::TopCenter, notificationScale, tonemapRequired);


    if (config->UseHQFont.value_or_default())
        ImGui::PopFontSize();
}

void MenuCommon::UpdateFrameTimeAverages(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& frameTime = ctx.frameTime;
    auto& frameRate = ctx.frameRate;
    auto& frameTimesCalculated = ctx.frameTimesCalculated;
    auto& menuResScale = ctx.menuResScale;
    auto& fpsScale = ctx.fpsScale;
    auto& averageFrameTime = ctx.averageFrameTime;
    auto& averageUpscalerFT = ctx.averageUpscalerFT;

    // FPS Overlay font
    fpsScale = config->FpsScale.value_or(menuResScale);

    // Update frame time & upscaler time averages
    averageFrameTime = 0.0f;
    averageUpscalerFT = 0.0f;

    if (config->ShowFps.value_or_default() || _isVisible)
    {
        float frameCnt = 0;
        frameTime = 0;
        for (size_t i = 299; i > 199; i--)
        {
            if (state.frameTimes[i] > 0.0)
            {
                frameTime += state.frameTimes[i];
                frameCnt++;
            }
        }

        frameTime /= frameCnt;
        frameRate = 1000.0 / frameTime;
        frameTimesCalculated = true;

        float lastFT = static_cast<float>(state.frameTimes.empty() ? 0.0f : state.frameTimes.back());
        float lastUT = static_cast<float>(state.upscaleTimes.empty() ? 0.0f : state.upscaleTimes.back());
        gFrameTimes.Push(lastFT);
        gUpscalerTimes.Push(lastUT);

        averageFrameTime = gFrameTimes.Average();
        averageUpscalerFT = gUpscalerTimes.Average();
    }
}

// Labels for the comparison views.
//
// Drawn straight onto the foreground draw list, not as ImGui windows -- the last attempt made them
// draggable windows and the clamping fought the split. Here each label is clipped to its own side of
// the comparison, so in the wipe the moving split reveals and hides it exactly as it does the
// pictures, and there is nothing to drag. Both wipe labels sit in the same top-left corner, each
// clipped to its side, so whichever picture currently owns that corner is the one whose label shows.
void MenuCommon::RenderNrCompareTags()
{
    auto* config = Config::Instance();

    const uint32_t mode = config->DlssNrCompare.value_or_default();

    if (mode == 0 || !config->DlssNrCompareTags.value_or_default())
        return;

    const ImVec2 screen = ImGui::GetIO().DisplaySize;

    if (screen.x < 1.0f || screen.y < 1.0f)
        return;

    const bool swap = config->DlssNrCompareSwap.value_or_default();
    const float split = mode == 1 ? 0.5f
                                  : std::clamp(config->DlssNrCompareSplit.value_or_default(), 0.0f, 1.0f);
    const float splitX = split * screen.x;

    const float scale = std::clamp(config->DlssNrTagScale.value_or_default(), 0.5f, 5.0f);

    // The left side is the untouched frame unless swapped -- matching the shader's
    // showOriginal = (uv.x < split) != swap.
    const std::string leftText = MenuText::Translate(swap ? "DLSS NR：开启" : "DLSS NR：关闭");
    const std::string rightText = MenuText::Translate(swap ? "DLSS NR：关闭" : "DLSS NR：开启");

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize() * scale;
    const float margin = 10.0f * scale;

    // Both labels flank the divider along the top: the left picture's label is right-aligned just
    // left of the split, the right picture's is left-aligned just right of it. Each is clipped to its
    // own side, so in the wipe the split reveals and hides them along with the images.
    auto drawTag = [&](const char* text, float x, ImVec2 clipMin, ImVec2 clipMax)
    {
        const ImVec2 size = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);

        // Never let a label run off the visible frame as it grows.
        x = std::min(std::max(x, 0.0f), screen.x - size.x);
        float y = std::min(margin, screen.y - size.y - margin);
        y = std::max(y, 0.0f);

        dl->PushClipRect(clipMin, clipMax, true);
        dl->AddText(font, fontSize, ImVec2(x + 2.0f, y + 2.0f), IM_COL32(0, 0, 0, 210), text);
        dl->AddText(font, fontSize, ImVec2(x, y), IM_COL32(255, 255, 255, 255), text);
        dl->PopClipRect();
    };

    const ImVec2 leftSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, leftText.c_str());

    // Left picture's label: right edge a margin in from the split. Right picture's: left edge a margin
    // out from the split.
    drawTag(leftText.c_str(), splitX - margin - leftSize.x, ImVec2(0.0f, 0.0f), ImVec2(splitX, screen.y));
    drawTag(rightText.c_str(), splitX + margin, ImVec2(splitX, 0.0f), ImVec2(screen.x, screen.y));
}

// Read completed durations on the Reflex UI polling boundary. Absolute GPU
// placement needs a shared frame/clock receipt; a duration is never an offset.
struct OverlayGpuTimings
{
    GenshinHookCpuTiming::BridgeGpuReading fsr {};
    DlssNr::GpuTimingSample nr {};
    uint64_t capturedMs = 0, fgFrame = 0, lowLatencyFrame = 0;
    bool fsrEnabled = false, nrEnabled = false;
    void Capture(bool genshin, bool nrRequested, uint64_t fg, uint64_t lowLatency)
    {
        fsr = genshin ? GenshinHookCpuTiming::ReadBridgeGpu() : GenshinHookCpuTiming::BridgeGpuReading {};
        nr = nrRequested ? DlssNr::ReadGpuTiming() : DlssNr::GpuTimingSample {};
        capturedMs = GetTickCount64(); fgFrame = fg; lowLatencyFrame = lowLatency;
        fsrEnabled = genshin; nrEnabled = nrRequested;
    }
};

// Completed duration and its scope; schematic placement never changes the sample.
struct OverlayGpuDuration
{
    std::optional<double> milliseconds;
    bool bridgeOnly = false;
};

static OverlayGpuDuration ReadOverlayGpuDuration(const OverlayGpuTimings& sample, bool fsr)
{
    if (fsr ? !sample.fsrEnabled : !sample.nrEnabled)
        return {};

    const auto stamp = fsr ? sample.fsr.sampleMs : sample.nr.publishedMs;
    const auto ms = fsr ? sample.fsr.milliseconds : sample.nr.milliseconds;
    const bool fresh = stamp && sample.capturedMs >= stamp && sample.capturedMs - stamp <= 2000;
    if (!fresh || !std::isfinite(ms) || ms < 0.0 || (fsr && !sample.fsr.samples))
        return {};
    return { ms, false };
}

// All rows share one scale, including the schematic NR endpoint. Reflex positions
// remain relative to their measured report; the GPU duration rows use UI anchors.
struct OverlayTimingPlot
{
    double rangeMs = 1.0;
    double presentEndMs = 0.0;
    float width, labelWidth;

    OverlayTimingPlot(float plotWidth, float fpsScale, double reportMs,
                      const std::optional<TimingEntry>& present)
        : rangeMs(std::max(1.0, reportMs)), width(plotWidth), labelWidth(155.0f * fpsScale)
    {
        if (Valid(present) && reportMs > 0.0)
            presentEndMs = (present->position + present->length) * reportMs;
    }

    static bool Valid(const std::optional<TimingEntry>& timing)
    {
        return timing && std::isfinite(timing->position) && std::isfinite(timing->length) &&
            timing->position >= 0.0 && timing->length >= 0.0 &&
            std::isfinite(timing->position + timing->length);
    }

    void Include(const std::optional<TimingEntry>& timing, double reportMs)
    {
        if (Valid(timing))
            rangeMs = std::max(rangeMs, (timing->position + timing->length) * reportMs);
    }

    void IncludeGpu(const OverlayGpuDuration& fsr, const OverlayGpuDuration& nr)
    {
        rangeMs = std::max(rangeMs, fsr.milliseconds.value_or(0.0));
        rangeMs = std::max(rangeMs, presentEndMs + nr.milliseconds.value_or(0.0));
        // Keep translated GPU labels and their exact values clear of every bar.
        const auto includeLabel = [&](const OverlayGpuDuration& duration, const char* label)
        {
            if (!duration.milliseconds)
                return;
            const auto text = MenuText::Format("%-12s %4.1fms", label, *duration.milliseconds);
            labelWidth = std::max(labelWidth,
                ImGui::CalcTextSize(text.c_str()).x + ImGui::GetStyle().ItemInnerSpacing.x);
        };
        includeLabel(fsr, fsr.bridgeOnly ? "FSR 桥 DX12 GPU" : "FSR 全链 GPU");
        includeLabel(nr, "NR GPU");
    }

    void Draw(double startMs, double durationMs, ImVec4 color) const
    {
        const auto min = ImGui::GetItemRectMin();
        const float left = min.x + labelWidth;
        const float rowWidth = std::min(width, ImGui::GetContentRegionAvail().x);
        const float barWidth = std::max(0.0f, rowWidth - labelWidth);
        const float start = left + barWidth * static_cast<float>(startMs / rangeMs);
        const float end = left + barWidth * static_cast<float>((startMs + durationMs) / rangeMs);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(start, min.y),
            ImVec2(end, ImGui::GetItemRectMax().y), ImGui::ColorConvertFloat4ToU32(color));
    }

    void DrawTiming(const std::optional<TimingEntry>& timing, const char* desc, ImVec4 color,
                    double reportMs) const
    {
        if (!Valid(timing))
            return;
        const auto mappedColor = toneMapColor(color);
        const double durationMs = timing->length * reportMs;
        MenuText::TextColored(mappedColor, "%-12s %4.1fms", desc, durationMs);
        Draw(timing->position * reportMs, durationMs, mappedColor);
    }
};

static void RenderOverlayGpuTiming(const OverlayGpuTimings& sample, bool fsr,
                                   const OverlayGpuDuration& duration, const OverlayTimingPlot& plot)
{
    if (fsr ? !sample.fsrEnabled : !sample.nrEnabled)
        return;

    const char* label = fsr ? (duration.bridgeOnly ? "FSR 桥 DX12 GPU" : "FSR 全链 GPU") : "NR GPU";
    if (!duration.milliseconds)
    {
        MenuText::TextDisabled("%s：暂无数据", label);
        return;
    }

    const auto color = toneMapColor(fsr ? ImVec4(0.10f, 0.70f, 0.78f, 1.0f) : ImVec4(0.38f, 0.82f, 0.44f, 1.0f));
    MenuText::TextColored(color, "%-12s %4.1fms", label, *duration.milliseconds);
    plot.Draw(fsr ? 0.0 : plot.presentEndMs, *duration.milliseconds, color);

    if (!MenuText::ItemHelpHovered())
        return;

    const auto tip = fsr
        ? (duration.bridgeOnly
            ? MenuText::Translate("真实已完成 D3D12 桥段 GPU 耗时；仅包含桥段，不代表 D3D11 全链。与 Reflex 同批刷新。\n条从时间轴左端起画，长度为实测耗时；横向锚点仅为示意，不代表真实 GPU 开始时间或同帧时序。")
            : MenuText::Format("D3D11 全链独立 GPU 耗时：输入、传输、超分及回写，包含 GPU 等待。监控开启时最多每 100 毫秒采样，与 Reflex 同批更新。样本 CPU 年龄 %.1f 秒；查询异步完成。\n条从时间轴左端起画，长度为实测耗时；横向锚点仅为示意，不代表真实 GPU 开始时间或同帧时序。不与 D3D12 桥段合并。",
                double(sample.capturedMs - sample.fsr.sampleMs) / 1000.0))
        : MenuText::Translate("真实已完成 NR GPU 耗时：准备、模型或复用及合成回写，包含 GPU 等待，与 Reflex 同批刷新。\n条从当前呈现条右端向右延伸，长度为实测耗时；无呈现样本时从左端示意。横向锚点不代表真实 GPU 开始时间或同帧时序，不证明串行或异步并行；各计时不能相加。");
    MenuText::ItemHelp(true, tip.c_str());
}

void MenuCommon::RenderPerformanceOverlay(RenderMenuContext& ctx)
{
    RenderNrCompareTags();


    auto& state = ctx.state;
    auto config = ctx.config;
    auto& io = ctx.io;
    auto& currentFeature = ctx.currentFeature;
    auto& now = ctx.now;
    auto& frameTime = ctx.frameTime;
    auto& frameRate = ctx.frameRate;
    auto& menuResScale = ctx.menuResScale;
    auto& fpsScale = ctx.fpsScale;
    auto& averageFrameTime = ctx.averageFrameTime;
    auto& averageUpscalerFT = ctx.averageUpscalerFT;

    const float criticalHeight=io.DisplaySize.y?io.DisplaySize.y:state.screenHeight;
    const float notificationScale=std::clamp(0.5f+(2.0f-0.5f)/(1440.0f-480.0f)*(criticalHeight-480.0f),0.5f,2.0f);
    if(config->UseHQFont.value_or_default())ImGui::PushFontSize(std::round(notificationScale*fontSize));
    // The main window renders last. Its current stored bounds prevent the
    // separate critical queue from covering its controls while it is open.
    auto menuBounds = criticalMainMenuBounds;
    if (_isVisible)
        if (const auto* menu = ImGui::FindWindowByName(windowTitle.c_str()))
            menuBounds = { menu->Pos.x, menu->Pos.y, menu->Size.x, menu->Size.y };
    const CriticalUi::Colors criticalColors {
        toneMapColor(ImVec4(1.0f, 0.58f, 0.26f, 1.0f)), toneMapColor(ImVec4(1.0f, 1.0f, 1.0f, 1.0f))
    };
    const auto criticalLayout = CriticalUi::Render(genshinCriticalHints.Notices(), criticalErrorsCollapsed, menuBounds,
        _isVisible, config->UseHQFont.value_or_default(), notificationScale, criticalColors,
        genshinCriticalHints.ProgressSnapshot(), GetTickCount64(), paimonAdviceDeadlineTick);
    if (criticalLayout.visible && genshinCriticalHints.Notices().Get(CriticalUi::Key::GenshinFsr) ==
        CriticalUi::Notice::GenshinHooking)
    {
        const auto& progress = genshinCriticalHints.ProgressSnapshot();
        GenshinLegacy::Ui::HookHintPresentation().DrawListBuilt({ progress.session, progress.attempt },
                                                               GetCurrentThreadId());
    }
    // Draw-list creation is not a Present acknowledgement. The actual overlay
    // submission/presenter owner must publish Submitted/CompletePresent before
    // a pre-hook scheduling gate can truthfully consume Ready().

    if(config->UseHQFont.value_or_default())ImGui::PopFontSize();

    // If Fps overlay is visible
    if (config->ShowFps.value_or_default())
    {
        bool stylePushed = false;

        const static auto defaultStyle = ImGuiStyle();

        // Rescale the fps overlay every frame because it shares style with the main menu
        if (config->FpsScale.has_value() && config->FpsScale.value() != menuResScale)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, defaultStyle.WindowPadding * fpsScale);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, defaultStyle.FramePadding * fpsScale);
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, defaultStyle.CellPadding * fpsScale);
            ImGui::PushStyleVar(ImGuiStyleVar_SeparatorTextPadding, defaultStyle.SeparatorTextPadding * fpsScale);

            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, defaultStyle.ItemSpacing * fpsScale);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, defaultStyle.ItemInnerSpacing * fpsScale);
            ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, defaultStyle.IndentSpacing * fpsScale);

            stylePushed = true;
        }

        // Set overlay position
        ImGui::SetNextWindowPos(overlayPosition, ImGuiCond_Always);

        // Set overlay window properties
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));  // Transparent border
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0)); // Transparent frame background

        if (!config->OverlaysUseTheme.value_or_default())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, toneMapColor(ImVec4(1.0f, 1.0f, 1.0f, 1.0f)));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
        }

        ImGui::SetNextWindowBgAlpha(config->FpsOverlayAlpha.value_or_default()); // Transparent background

        if (!config->OverlaysUseTheme.value_or_default())
        {
            ImVec4 green(0.0f, 1.0f, 0.0f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_PlotLines, toneMapColor(green));
        }

        if (MenuText::Begin("性能叠加层", nullptr,
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav))
        {
            std::string api;
            if (MenuPrimaryGpu().usesDxvk && state.api == DX11)
            {
                if (state.swapchainInteropApi == SwapchainInteropApi::None)
                    api = "DXVK";
                else
                    api = "DXVK w/Dx12";
            }
            else if (MenuPrimaryGpu().usesVkd3dProton && state.api == DX12)
            {
                api = "VKD3D";
            }
            else
            {
                switch (state.swapchainApi)
                {
                case Vulkan:
                    api = state.swapchainInteropApi == SwapchainInteropApi::VkwDx12 ? "VLK w/DX12" : "VLK";
                    break;

                case DX11:
                    api = "D3D11";
                    break;

                case DX12:
                    if (state.swapchainInteropApi == SwapchainInteropApi::Dx11wDx12)
                        api = "D3D11 w/DX12";
                    else
                        api = "D3D12";

                    break;

                default:
                    switch (state.api)
                    {
                    case Vulkan:
                        api = "VLK";
                        break;

                    case DX11:
                        api = "D3D11";
                        break;

                    case DX12:
                        api = "D3D12";
                        break;

                    default:
                        api = "???";
                        break;
                    }

                    break;
                }
            }

            if (config->UseHQFont.value_or_default())
                PushMonoFontSize(std::round(fpsScale * fontSize));
            else
                ImGui::SetWindowFontScale(fpsScale);

            std::string firstLine = "";
            std::string secondLine = "";
            std::string thirdLine = "";

            static const bool genshinProcess = GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring());
            const bool unmeasuredGenshinFg = genshinProcess && state.activeFgInput == FGInput::Upscaler &&
                state.activeFgOutput == FGOutput::DLSSG;

            auto fg = state.currentFG;
            auto fgText = (fg != nullptr && fg->IsActive() && !fg->IsPaused()) ? (" (" + std::string(fg->Name()) + ")")
                                                                               : std::string();

            const int fakeFramesCount = DlssgInterpolationCount(state);
            auto formatFg = [&](std::string_view name, int maxFakeFrames)
            {
                if (fakeFramesCount > maxFakeFrames)
                    return MenuText::Format("（%s 不支持超过 %dx）", std::string(name), maxFakeFrames);

                else if (fakeFramesCount == 0)
                    return MenuText::Format("（%s 已关闭）", std::string(name));

                return std::format(" ({} x{})", name, fakeFramesCount + 1);
            };

            const FGNvngxReplacement activeNvngxFg = state.activeFgNvngx;
            if (activeNvngxFg == FGNvngxReplacement::Arturs)
            {
                fgText = formatFg("Enabler", Nvngx_FG::getMaxFakeFramesCount());
            }
            else if (activeNvngxFg == FGNvngxReplacement::Nukems)
            {
                fgText = formatFg("Nukems", Nvngx_FG::getMaxFakeFramesCount());
            }
            else if (activeNvngxFg == FGNvngxReplacement::FFX)
            {
                fgText = formatFg("FFX", Nvngx_FG::getMaxFakeFramesCount());
            }
            else if (activeNvngxFg == FGNvngxReplacement::Combo)
            {
                fgText = formatFg("Combo", Nvngx_FG::getMaxFakeFramesCount());
            }
            else if (state.activeFgOutput == FGOutput::DLSSG && fg)
            {
                // Genshin's requested multiplier is not a measured generated FPS.
                // Unknown output must not be presented as "DLSSG off".
                fgText = unmeasuredGenshinFg && fg->IsActive() && !fg->IsPaused()
                    ? MenuText::Translate("（DLSSG 已运行；实际生成帧率未知）")
                    : formatFg("DLSSG", fg->GetMaxInterpolationCount());
            }

            const auto overlayType = config->FpsOverlayType.value_or_default();
            const bool hasFeature = currentFeature && !currentFeature->IsFrozen();

            // Prepare Line 1
            std::string featurePart;
            std::string fpsPart;

            if (hasFeature)
            {
                const bool usesDx12CompatLayer = currentFeature->IsWithDx12();

                featurePart = StrFmt(" | %s -> %s %u.%u.%u%s", ApiUpscalerInputName(state.currentInputApiName).c_str(),
                                     currentFeature->ShortName().c_str(), currentFeature->Version().major,
                                     currentFeature->Version().minor, currentFeature->Version().patch,
                                     usesDx12CompatLayer ? " w/Dx12" : "");
            }


            if (!unmeasuredGenshinFg && fg != nullptr && fg->IsActive() && !fg->IsPaused())
            {
                const double baseFps = frameRate / (double) (fg->GetInterpolatedFrameCount() + 1);

                switch (overlayType)
                {
                case FpsOverlay_JustFPS:
                    fpsPart = StrFmt("%6.1f/%5.1f ", frameRate, baseFps);
                    break;

                case FpsOverlay_Simple:
                    fpsPart = StrFmt("FPS：%6.1f/%5.1f，%7.2f ms", frameRate, baseFps, frameTime);
                    break;

                default:
                    fpsPart = StrFmt("FPS：%6.1f/%5.1f，平均：%6.1f", frameRate, baseFps, 1000.0f / averageFrameTime);
                    break;
                }
            }
            else
            {
                switch (overlayType)
                {
                case FpsOverlay_JustFPS:
                    fpsPart = StrFmt("%6.1f ", frameRate);
                    break;

                case FpsOverlay_Simple:
                    fpsPart = StrFmt("FPS：%6.1f，%7.2f ms", frameRate, frameTime);
                    break;

                default:
                    fpsPart = StrFmt("FPS：%6.1f，平均：%6.1f", frameRate, 1000.0f / averageFrameTime);
                    break;
                }
            }

            if (overlayType == FpsOverlay_JustFPS)
                firstLine = StrFmt("%s", fpsPart.c_str());
            else
                firstLine = StrFmt("%s | %s%s%s", api.c_str(), fpsPart.c_str(), fgText.c_str(), featurePart.c_str());

            // Prepare Line 2
            if (config->FpsOverlayType.value_or_default() >= FpsOverlay_Detailed)
            {
                if (config->FpsOverlayHorizontal.value_or_default())
                {
                    ImGui::SameLine(0.0f, 0.0f);
                    MenuText::Text(" | ");
                    ImGui::SameLine(0.0f, 0.0f);
                }
                else
                {
                    ImGui::Spacing();
                }

                secondLine = StrFmt("帧耗时：%7.2f ms，平均：%7.2f ms", state.frameTimes.back(), averageFrameTime);
            }

            // Prepare Line 3
            if (config->FpsOverlayType.value_or_default() >= FpsOverlay_Full)
            {
                thirdLine =
                    StrFmt("超分耗时：%7.2f ms，平均：%7.2f ms", state.upscaleTimes.back(), averageUpscalerFT);
            }

            ImVec2 plotSize;
            if (config->FpsOverlayHorizontal.value_or_default())
            {
                plotSize = { fpsScale * 150, fpsScale * 16 };
            }
            else
            {
                // Find the widest text width
                auto firstSize = MenuText::CalcTextSize(firstLine.c_str());
                auto secondSize = MenuText::CalcTextSize(secondLine.c_str());
                auto thirdSize = MenuText::CalcTextSize(thirdLine.c_str());
                auto textWidth = 0.0f;

                if (firstSize.x > secondSize.x)
                    textWidth = firstSize.x > thirdSize.x ? firstSize.x : thirdSize.x;
                else
                    textWidth = secondSize.x > thirdSize.x ? secondSize.x : thirdSize.x;

                auto minWidth = fpsScale * 300.0f;
                auto plotWidth = textWidth < minWidth ? minWidth : textWidth;

                plotSize = { plotWidth, fpsScale * 30 };
            }

            // Draw the overlay
            MenuText::Text(firstLine.c_str());

            if (config->FpsOverlayType.value_or_default() >= FpsOverlay_Detailed)
            {
                if (config->FpsOverlayHorizontal.value_or_default())
                {
                    ImGui::SameLine(0.0f, 0.0f);
                    MenuText::Text(" | ");
                    ImGui::SameLine(0.0f, 0.0f);
                }
                else
                {
                    ImGui::Spacing();
                }

                MenuText::Text(secondLine.c_str());
            }

            if (config->FpsOverlayType.value_or_default() >= FpsOverlay_DetailedGraph)
            {
                if (config->FpsOverlayHorizontal.value_or_default())
                    ImGui::SameLine(0.0f, 0.0f);

                // Graph of frame times
                ImGui::PlotLines(
                    "##FrameTimeGraph",
                    [](void* rb, int idx) -> float { return static_cast<RingBuffer<float, plotWidth>*>(rb)->At(idx); },
                    &gFrameTimes, plotWidth, 0, nullptr, 0.0f, 66.6f, plotSize);
            }

            if (config->FpsOverlayType.value_or_default() >= FpsOverlay_Full)
            {
                if (config->FpsOverlayHorizontal.value_or_default())
                {
                    ImGui::SameLine(0.0f, 0.0f);
                    MenuText::Text(" | ");
                    ImGui::SameLine(0.0f, 0.0f);
                }
                else
                {
                    ImGui::Spacing();
                }

                MenuText::Text(thirdLine.c_str());
            }

            if (config->FpsOverlayType.value_or_default() >= FpsOverlay_FullGraph)
            {
                if (config->FpsOverlayHorizontal.value_or_default())
                    ImGui::SameLine(0.0f, 0.0f);

                // Graph of upscaler times
                ImGui::PlotLines(
                    "##UpscalerFrameTimeGraph",
                    [](void* rb, int idx) -> float { return static_cast<RingBuffer<float, plotWidth>*>(rb)->At(idx); },
                    &gUpscalerTimes, plotWidth, 0, nullptr, 0.0f, 20.0f, plotSize);
            }

            if (config->FpsOverlayType.value_or_default() >= FpsOverlay_ReflexTimings)
            {
                constexpr auto delayBetweenPollsMs = 100;
                static auto previousPoll = 0.0;
                static bool gotData = false;
                static OverlayGpuTimings gpuTimings {};
                const bool pollTimings = previousPoll <= 0.001 || now < previousPoll ||
                    previousPoll + delayBetweenPollsMs < now ||
                    gpuTimings.fsrEnabled != genshinProcess ||
                    gpuTimings.nrEnabled != config->DlssNrEnabled.value_or_default();

#ifdef LOW_LATENCY_INPUTS
                if (genshinProcess || config->DlssNrEnabled.value_or_default())
                    MenuText::TextDisabled("实心条：FSR 左端、NR 接呈现右端；横向锚点仅示意");
                static TimingData timingData {};

                if (pollTimings)
                {
                    gotData = InputCommon::get_timing_data(timingData);
                    gpuTimings.Capture(genshinProcess, config->DlssNrEnabled.value_or_default(),
                        fg != nullptr ? fg->FrameCount() : 0, state.reflexFrameId);
                    previousPoll = now;
                }

                const auto fsrDuration = ReadOverlayGpuDuration(gpuTimings, true);
                const auto nrDuration = ReadOverlayGpuDuration(gpuTimings, false);
                const bool hasTimeline = gotData && timingData.timeRange &&
                    std::isfinite(timingData.timeRange->length) && timingData.timeRange->length > 0.0;
                const double reportMs = hasTimeline ? timingData.timeRange->length / 1000.0 : 0.0;
                const float maxWidth = config->FpsOverlayHorizontal.value_or_default()
                    ? ImGui::GetWindowWidth() : plotSize.x;
                OverlayTimingPlot timingPlot(maxWidth, fpsScale, reportMs,
                    hasTimeline ? timingData.present : std::optional<TimingEntry> {});
                if (hasTimeline)
                {
                    timingPlot.Include(timingData.simulation, reportMs);
                    timingPlot.Include(timingData.renderSubmit, reportMs);
                    timingPlot.Include(timingData.present, reportMs);
                    timingPlot.Include(timingData.driver, reportMs);
                    timingPlot.Include(timingData.osRenderQueue, reportMs);
                    timingPlot.Include(timingData.gpuRender, reportMs);
                }
                timingPlot.IncludeGpu(fsrDuration, nrDuration);
                const auto drawTiming = [&](const auto& timing, const char* desc, ImVec4 color)
                {
                    timingPlot.DrawTiming(timing, desc, color, reportMs);
                };

                if (hasTimeline)
                {
                    MenuText::Text("插帧编号：%llu，低延迟编号：%llu", gpuTimings.fgFrame, gpuTimings.lowLatencyFrame);
                    MenuText::Text("整帧低延迟计时：%.1fms", reportMs);
                    drawTiming(timingData.simulation, "模拟", ImVec4(0.768f, 0.169f, 0.169f, 1.0f));
                    drawTiming(timingData.renderSubmit, "渲染提交", ImVec4(0.235f, 0.705f, 0.294f, 1.0f));
                    RenderOverlayGpuTiming(gpuTimings, true, fsrDuration, timingPlot);
                    drawTiming(timingData.present, "呈现", ImVec4(1.0f, 0.88f, 0.098f, 1.0f));
                    RenderOverlayGpuTiming(gpuTimings, false, nrDuration, timingPlot);
                    drawTiming(timingData.driver, "驱动", ImVec4(0.263f, 0.388f, 0.847f, 1.0f));
                    drawTiming(timingData.osRenderQueue, "渲染队列", ImVec4(0.76f, 0.51f, 0.188f, 1.0f));
                    drawTiming(timingData.gpuRender, "GPU 渲染", ImVec4(0.569f, 0.117f, 0.705f, 1.0f));
                }
                else
                {
                    MenuText::TextDisabled("Reflex 时间线：暂无数据（不要求开启 FG）");
                    RenderOverlayGpuTiming(gpuTimings, true, fsrDuration, timingPlot);
                    RenderOverlayGpuTiming(gpuTimings, false, nrDuration, timingPlot);
                }
#else
                if (genshinProcess || config->DlssNrEnabled.value_or_default())
                    MenuText::TextDisabled("实心条：FSR 左端、NR 接呈现右端；横向锚点仅示意");
                static ReflexHooks::TimingReportSnapshot timingReport {};
                if (pollTimings)
                {
                    gotData = ReflexHooks::updateTimingData(timingReport);
                    gpuTimings.Capture(genshinProcess, config->DlssNrEnabled.value_or_default(),
                        fg != nullptr ? fg->FrameCount() : 0, state.reflexFrameId);
                    previousPoll = now;
                }

                const auto& timingData = timingReport.timingData;
                auto fsrDuration = ReadOverlayGpuDuration(gpuTimings, true);
                auto nrDuration = ReadOverlayGpuDuration(gpuTimings, false);
                const auto useReportDuration = [&](OverlayGpuDuration& duration, bool fsr)
                {
                    if (duration.milliseconds || !gotData ||
                        (fsr ? !gpuTimings.fsrEnabled : !gpuTimings.nrEnabled))
                        return;
                    const auto& ms = fsr ? timingReport.bridgeGpuMs : timingReport.nrGpuMs;
                    if (ms && std::isfinite(*ms) && *ms >= 0.0)
                        duration = { ms, fsr };
                };
                // Prefer the full D3D11 chain; the DX12 bridge is a labelled fallback,
                // never a second FSR row or a substitute scope for the full chain.
                useReportDuration(fsrDuration, true);
                useReportDuration(nrDuration, false);
                const bool hasTimeline = gotData && timingData[TimingType::TimeRange] &&
                    std::isfinite(timingData[TimingType::TimeRange]->length) &&
                    timingData[TimingType::TimeRange]->length > 0.0;
                const double reportMs = hasTimeline ? timingData[TimingType::TimeRange]->length / 1000.0 : 0.0;
                const float maxWidth = config->FpsOverlayHorizontal.value_or_default()
                    ? ImGui::GetWindowWidth() : plotSize.x;
                OverlayTimingPlot timingPlot(maxWidth, fpsScale, reportMs,
                    hasTimeline ? timingData[TimingType::Present] : std::optional<TimingEntry> {});
                if (hasTimeline)
                {
                    timingPlot.Include(timingData[TimingType::Simulation], reportMs);
                    timingPlot.Include(timingData[TimingType::RenderSubmit], reportMs);
                    timingPlot.Include(timingData[TimingType::Present], reportMs);
                    if (!timingReport.estimated)
                    {
                        timingPlot.Include(timingData[TimingType::Driver], reportMs);
                        timingPlot.Include(timingData[TimingType::OsRenderQueue], reportMs);
                        timingPlot.Include(timingData[TimingType::GpuRender], reportMs);
                    }
                }
                timingPlot.IncludeGpu(fsrDuration, nrDuration);

                if (hasTimeline)
                {
                    const auto& rangeInUs = timingData[TimingType::TimeRange].value().length;

                    const UINT64 localFrameCount = gpuTimings.fgFrame;

                    MenuText::Text("计时报告编号：%llu", timingReport.frameId);
                    MenuText::Text("本批 FG 编号：%llu（独立计数，非计时报告绑定）", localFrameCount);
                    // fakenvapi (XeFG's XeLL, Anti-Lag 2, LatencyFlex) has only the markers' times: its driver, queue
                    // and GPU entries are made up, so they are not drawn
                    const bool estimated = timingReport.estimated;

                    if (estimated)
                    {
                        MenuText::Text("模拟至呈现延迟：%.1fms", rangeInUs / 1000.0);
                        MenuText::TextDisabled("按帧标记计算（fakenvapi 不提供 GPU 计时）");
                    }
                    else
                    {
                        MenuText::Text("整帧 Reflex 计时：%.1fms", rangeInUs / 1000.0);
                    }

                    const auto drawTiming = [&](const auto& timing, const char* desc, ImVec4 color)
                    {
                        timingPlot.DrawTiming(timing, desc, color, reportMs);
                    };

                    drawTiming(timingData[TimingType::Simulation], "模拟", ImVec4(0.768f, 0.169f, 0.169f, 1.0f));
                    drawTiming(timingData[TimingType::RenderSubmit], "渲染提交", ImVec4(0.235f, 0.705f, 0.294f, 1.0f));
                    RenderOverlayGpuTiming(gpuTimings, true, fsrDuration, timingPlot);
                    drawTiming(timingData[TimingType::Present], "呈现", ImVec4(1.0f, 0.88f, 0.098f, 1.0f));
                    RenderOverlayGpuTiming(gpuTimings, false, nrDuration, timingPlot);

                    if (!estimated)
                    {
                        drawTiming(timingData[TimingType::Driver], "驱动", ImVec4(0.263f, 0.388f, 0.847f, 1.0f));
                        drawTiming(timingData[TimingType::OsRenderQueue], "渲染队列", ImVec4(0.76f, 0.51f, 0.188f, 1.0f));
                        drawTiming(timingData[TimingType::GpuRender], "GPU 起止跨度", ImVec4(0.569f, 0.117f, 0.705f, 1.0f));
                    }

                    if (timingReport.gpuActiveRenderTimeUs.has_value())
                        MenuText::Text("GPU 活跃时间：%.1fms",
                                       static_cast<double>(*timingReport.gpuActiveRenderTimeUs) / 1000.0);
                    else
                        MenuText::TextDisabled("GPU 活跃时间：不可用");

                    MenuText::TextDisabled("各计时窗口可重叠，不能相加；GPU 起止跨度不等于忙时");
                }
                else
                {
                    MenuText::TextDisabled("Reflex 时间线：暂无数据（不要求开启 FG）");
                    RenderOverlayGpuTiming(gpuTimings, true, fsrDuration, timingPlot);
                    RenderOverlayGpuTiming(gpuTimings, false, nrDuration, timingPlot);
                    MenuText::TextDisabled("GPU 活跃时间：不可用");
                }
#endif
            }

            ImGui::Spacing();
            MenuText::TextDisabled("使用 PageUp 或在 设置>界面 中关闭此面板");
        }

        // Restore the style
        if (!config->OverlaysUseTheme.value_or_default())
            ImGui::PopStyleColor(5);
        else
            ImGui::PopStyleColor(2);

        // Get size for postioning
        overlaySize = ImGui::GetWindowSize();

        if (config->UseHQFont.value_or_default())
            ImGui::PopFontSize();

        ImGui::End();

        if (stylePushed)
            ImGui::PopStyleVar(7);

        // Left / Right
        if (config->FpsOverlayPosition.value_or_default() == FpsOverlayPos_TopLeft ||
            config->FpsOverlayPosition.value_or_default() == FpsOverlayPos_BottomLeft)
        {
            overlayPosition.x = 0;
        }
        else
        {
            overlayPosition.x = io.DisplaySize.x - overlaySize.x;
        }

        // Top / Bottom
        if (config->FpsOverlayPosition.value_or_default() == FpsOverlayPos_TopLeft ||
            config->FpsOverlayPosition.value_or_default() == FpsOverlayPos_TopRight)
        {
            overlayPosition.y = 0;
        }
        else
        {
            // Prevent overlapping with splash message
            if (!config->DisableSplash.value_or_default() && now > splashStart && now < splashLimit)
                overlayPosition.y = io.DisplaySize.y - overlaySize.y - splashSize.y;
            else
                overlayPosition.y = io.DisplaySize.y - overlaySize.y;
        }
    }
}

// Which upscaler files and hooks were found: for troubleshooting a game whose upscaler call does not arrive. Drawn
// inside a tooltip, by the header's lines (here and DlssNr::RenderHeaderBanner's offer).
static void UpscalerFileChecks()
{
    const auto& state = State::Instance();
    const auto& primaryGpu = MenuPrimaryGpu();

    if (primaryGpu.dlssCapable)
    {
        MenuText::Text("nvngx_dlss : %s", state.NVNGX_DLSS_Path.has_value() ? "存在" : "不存在");
        ImGui::SameLine(0.0f, 16.0f);
        MenuText::Text("nvngx_dlssd : %s", state.NVNGX_DLSSD_Path.has_value() ? "存在" : "不存在");
    }
    else
    {
        MenuText::Text("nvngx.dll: %s", state.nvngxExists ? "存在" : "不存在");
        ImGui::SameLine(0.0f, 16.0f);
        MenuText::Text("nvngx 替代模块：%s", state.nvngxReplacement.has_value() ? "存在" : "不存在");
    }

    MenuText::Text("libxess: %s", (state.libxessExists || XeSSProxy::Module() != nullptr) ? "存在" : "不存在");

    MenuText::Text("FSR 钩子：%s", state.fsrHooks ? "存在" : "不存在");
    ImGui::SameLine(0.0f, 16.0f);
    MenuText::Text("FSR 3.1: %s", FfxApiProxy::Dx12Module() != nullptr ? "存在" : "不存在");
    ImGui::SameLine(0.0f, 16.0f);
    MenuText::Text("FSR 3.1 SR: %s", FfxApiProxy::Dx12Module_SR() != nullptr ? "存在" : "不存在");
    ImGui::SameLine(0.0f, 16.0f);
    MenuText::Text("FSR 3.1 FG: %s", FfxApiProxy::Dx12Module_FG() != nullptr ? "存在" : "不存在");
}

void MenuCommon::RenderMainMenuHeaderMessages(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& currentFeature = ctx.currentFeature;
    auto& menuResScale = ctx.menuResScale;
    auto& currentVersionText = ctx.currentVersionText;
    auto& primaryGpu = *ctx.primaryGpu;

    if (!_showMipmapCalcWindow && !_showHudlessWindow && !ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
        ImGui::SetWindowFocus();

    if (config->MenuScale.has_value())
    {
        _selectedScale = ((int) (menuResScale * 10.0f)) - 4;
    }
    else
    {
        _selectedScale = 0;
    }

    // Upscaler state is one line in every case, so the panes below never move when it changes.
    const bool upscalerFiles = state.nvngxExists || state.nvngxReplacement.has_value() ||
                               (state.libxessExists || XeSSProxy::Module() != nullptr);

    std::vector<std::string> upscalers;

    if (state.fsrHooks)
        upscalers.push_back("FSR");

    if (state.nvngxExists || state.nvngxReplacement.has_value() || primaryGpu.dlssCapable)
        upscalers.push_back("DLSS");

    if (state.libxessExists || XeSSProxy::Module() != nullptr)
        upscalers.push_back("XeSS");

    auto joined = upscalers | std::views::join_with(std::string { " or " });

    std::string joinedUpscalers(joined.begin(), joined.end());

    const auto genshinStatus = GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring())
        ? GenshinLegacy::GetStatus() : GenshinLegacy::Status {};
    const bool genshinBridgeActive = genshinStatus.srAccepted &&
        (genshinStatus.submitted != 0 || genshinStatus.outputCopies != 0);

    // Optical F5Low's lines (the native modes, menu/HeaderBanner.h) come first; the rule leaves a game's own feature,
    // and the cases with no offer to make, to the lines below.
    const bool f5lowLine = DlssNr::RenderHeaderBanner(
        config,
        currentFeature == nullptr || !currentFeature->IsInited() ? HeaderBanner::Feature::None
        : native::IsVirtualUpscalerFeature(currentFeature)       ? HeaderBanner::Feature::F5Low
        : currentFeature->IsFrozen() && !genshinBridgeActive     ? HeaderBanner::Feature::Frozen
                                                                 : HeaderBanner::Feature::Game,
        upscalerFiles, joinedUpscalers, currentFeature != nullptr ? currentFeature->Name() : std::string(),
        toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)), UpscalerFileChecks);

    if (f5lowLine)
    {
        // Drawn by DlssNr::RenderHeaderBanner.
    }
    else if (genshinBridgeActive)
    {
        // Bridge status/errors have their own UI; suppress generic inactive/frozen hints here.
    }
    else if (currentFeature == nullptr || !currentFeature->IsInited())
    {
        if (upscalerFiles)
        {
            MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)),
                               "请在游戏选择 %s 超分，并进入游戏场景以启用这些设置。",
                               joinedUpscalers.c_str());
            // Troubleshooting reads remain lazy, while hovering the status text.
            if (MenuText::ItemHelpHovered() && ImGui::BeginTooltip())
            {
                MenuText::TextUnformatted("超分功能在菜单中不一定运行。");
                ImGui::Spacing();

                UpscalerFileChecks();

                ImGui::EndTooltip();
            }
        }
        else
        {
            MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.4f, 0.35f, 1.f)),
                               "未找到 nvngx.dll、libxess.dll 或 FSR 输入，超分支持不可用。");
        }
    }
    else if (currentFeature->IsFrozen())
    {
        MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)),
                           "%s 已启用，但游戏尚未使用，请进入游戏场景。",
                           currentFeature->Name().c_str());
    }
}

void MenuCommon::RenderGimiSettings(RenderMenuContext& ctx)
{
    auto config = ctx.config;
    static const bool genshinProcess = [] {
        wchar_t path[32768]{};
        const auto length = GetModuleFileNameW(nullptr, path, 32768);
        return length && length < 32768 && GenshinFsr::IsGenshinExecutable(std::wstring_view(path, length));
    }();
    if (!genshinProcess)
    {
        MenuText::TextWrapped("GIMI 当前仅支持原神，其他游戏不可启用此面板。");
        return;
    }
    const auto text = [](const char* chinese, const char* english) {
        return NextgenUi::English() ? english : chinese;
    };
    if (auto ch = ScopedCollapsingHeader(text("GIMI 多入口 MOD###GIMI 单组件 MOD",
                                             "GIMI MOD entries###GIMI 单组件 MOD")); ch.IsHeaderOpen())
    {
        // Keep the complete applied value separate from an unsubmitted editor draft.
        struct Editor
        {
            gimi::runtime::Selection baseline;
            std::string ini, component, trigger, libraries, hostRoot, namespaceRoot;
            bool initialized = false, dirty = false, componentEdited = false, active = false;
            int error = 0;
        };
        static Editor editor;
        const gimi::runtime::Selection current{config->GimiEnabled.value_or_default(),
            config->GimiIni.value_or_default(), config->GimiComponent.value_or_default(),
            config->GimiTrigger.value_or_default(), config->GimiLibraries.value_or_default(),
            config->GimiImportRoot.value_or_default(), {}, config->GimiNamespaceRoot.value_or_default(),
            config->GimiHostRoot.value_or_default()};
        const auto load = [&] {
            editor.baseline = current;
            editor.ini = current.ini;
            editor.component = current.component;
            std::replace(editor.component.begin(), editor.component.end(), '|', '\n');
            editor.component.reserve(8192); // runtime qualified limit, plus std::string's terminator
            editor.trigger = current.trigger;
            editor.libraries = current.libraries;
            editor.hostRoot = current.hostRoot;
            editor.namespaceRoot = current.namespaceRoot;
            editor.initialized = true;
            editor.dirty = editor.componentEdited = editor.active = false;
            editor.error = 0;
        };
        if (!editor.initialized) load();
        bool conflict = current.ini != editor.baseline.ini || current.component != editor.baseline.component ||
            current.trigger != editor.baseline.trigger || current.libraries != editor.baseline.libraries ||
            current.hostRoot != editor.baseline.hostRoot || current.namespaceRoot != editor.baseline.namespaceRoot ||
            current.importRoot != editor.baseline.importRoot;
        if (conflict && !editor.dirty && !editor.active) { load(); conflict = false; }

        bool enabled = current.enabled;
        if (MenuText::Checkbox(text("启用所选入口###启用所选单组件", "Enable selected entries###启用所选单组件"), &enabled))
        {
            config->GimiEnabled = enabled;
            // Toggling only uses the complete applied Config strings, never the draft.
            auto applied = current;
            applied.enabled = enabled;
            gimi::runtime::Configure(std::move(applied));
        }
        MenuText::ItemHelp(text("默认关闭；只启用明确选择的入口，不代表整包 MOD 已兼容。",
                               "Off by default. Enables only explicitly selected entries; full MOD compatibility is not established."));

        const auto resize = [](ImGuiInputTextCallbackData* data) -> int {
            if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
            {
                auto& value = *static_cast<std::string*>(data->UserData);
                value.resize(static_cast<std::size_t>(data->BufTextLen));
                data->Buf = value.data();
            }
            return 0;
        };
        editor.active = false;
        const auto input = [&](const char* caption, const char* identity, std::string& value,
                               const char* help, bool multiline = false) {
            MenuText::TextUnformatted(caption);
            MenuText::ItemHelp(help);
            // Use the original field identity independently of its visible caption.
            const std::string label = std::string("###") + identity;
            const bool changed = multiline
                ? ImGui::InputTextMultiline(label.c_str(), value.data(), value.capacity() + 1,
                    ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * 6.0f + ImGui::GetStyle().FramePadding.y * 2),
                    ImGuiInputTextFlags_CallbackResize, resize, &value)
                : (ImGui::SetNextItemWidth(-FLT_MIN), ImGui::InputText(label.c_str(), value.data(), value.capacity() + 1,
                    ImGuiInputTextFlags_CallbackResize, resize, &value));
            editor.active |= ImGui::IsItemActive();
            MenuText::ItemHelp(help);
            if (changed) { editor.dirty = true; editor.error = 0; }
            return changed;
        };
        input(text("MOD 文件或目录", "MOD file or directory"), "INI 绝对路径##gimi", editor.ini,
              text("填写一个 INI 或一个明确 MOD 目录的完整路径；目录会有界读取全部子 INI。填写原宿主根后，遵守其排除规则，绝不扩到其他 MOD。",
                   "Enter one INI or one explicit MOD directory. Directory import reads its descendant INIs within fixed budgets. With an original host root, its exclusions apply; other MODs are never scanned."));
        input(text("原宿主根 INI（可选）", "Original host root INI (optional)"), "gimi.hostRoot", editor.hostRoot,
              text("整包 GIMI 填写原 d3dx.ini 绝对路径。读取真实根变量与 core Include，并仅导入上方所选 MOD；不会自动启用。原加载器、调试快捷键和磁盘工具仍由 F6 既有入口管理，不执行原外部程序。未知运行设置会明确拒绝。",
                   "For a GIMI package, enter its original d3dx.ini. Imports authentic root globals, core Includes and only the selected MOD; never enables it automatically. Original launcher/debug keys/disk tools are source metadata; F6 uses its existing controls. Unsupported runtime settings are refused at their source."));
        if (input(text("组件入口（每行一个）", "Component entries (one per line)"), "组件节名##gimi", editor.component,
                  text("1–64 个不同的规范节名。多入口格式：vb0@canonicalName、ib@canonicalName、ps-t0@canonicalName，每行一个，也可用 | 分隔。旧式单入口仍可只填节名并使用下方绑定。不可混用两种格式；整包兼容性仍未确认。",
                       "1–64 distinct canonical section names. Use vb0@canonicalName, ib@canonicalName or ps-t0@canonicalName, one per line (| also works). Legacy names use the binding below. Do not mix plain and qualified entries. Full MOD compatibility remains unconfirmed."), true))
            editor.componentEdited = true;
        input(text("旧式入口绑定", "Legacy entry binding"), "精确绑定##gimi", editor.trigger,
              text("仅用于未带 @ 的旧式入口：ib、vb0..7、ps-t0..127、vs 或 ps。带 @ 的入口各用自己的绑定。",
                   "For plain legacy entries only: ib, vb0..7, ps-t0..127, vs or ps. Qualified entries each use their own binding."));
        input(text("依赖 INI 路径", "Dependency INI paths"), "依赖 INI 路径##gimi", editor.libraries,
              text("可留空；最多 16 个明确的绝对路径，以 | 分隔。原宿主已 Include 的 core 无需重复填写。",
                   "Optional: up to 16 explicit absolute paths separated by |. The host's real core Include does not need to be repeated here."));
        input(text("原目录基准（可选）", "Original directory base (optional)"), "gimi.namespaceRoot", editor.namespaceRoot,
              text("通常留空。填写原宿主根时自动使用它的父目录；这里若填写必须相同。仅用于保留原规范节名，不扩大 MOD 的文件权限。",
                   "Usually leave empty. Host import uses the root INI's parent; an explicit base must match it. Preserves original canonical section names without expanding MOD file authority."));

        if (conflict)
            MenuText::TextWrapped("%s", text("配置已变化；草稿已保留，请重新载入后编辑。",
                                            "Configuration changed. Draft retained; reload before editing."));
        ImGui::BeginDisabled(conflict);
        const bool apply = MenuText::Button(text("应用所选入口###应用所选组件##gimi", "Apply selected entries###应用所选组件##gimi"));
        ImGui::EndDisabled();
        MenuText::ItemHelp(text("应用到现有设置；实际加载或拒绝结果见下方运行状态。保存设置仍使用原有按钮。",
                               "Apply to the existing settings. Actual loading or refusal is shown below. Use the existing Save Settings button to persist."));
        if (apply)
        {
            std::string serialized = editor.baseline.component;
            if (editor.componentEdited)
            {
                const auto draft = gimi::Trim(editor.component);
                serialized.clear();
                for (std::size_t i = 0; i < draft.size(); ++i)
                {
                    const char c = draft[i];
                    if (c == '\r' && i + 1 < draft.size() && draft[i + 1] == '\n') ++i;
                    serialized += c == '\r' || c == '\n' ? '|' : c;
                }
            }
            // Mirror only M9's declaration bounds here; runtime owns all actual admission.
            const auto validTarget = [](std::string_view source) {
                const auto target = gimi::LowerAscii(gimi::Trim(source));
                if (target == "ib" || target == "vs" || target == "ps") return true;
                const auto slot = [&](std::string_view prefix, unsigned limit) {
                    if (!target.starts_with(prefix)) return false;
                    const auto suffix = std::string_view(target).substr(prefix.size());
                    if (suffix.empty() || suffix.size() > 3) return false;
                    unsigned value = 0;
                    for (char c : suffix) { if (c < '0' || c > '9') return false; value = value * 10 + c - '0'; }
                    return value < limit;
                };
                return slot("vb", 8) || slot("ps-t", 128);
            };
            const bool qualified = serialized.find('@') != std::string::npos;
            editor.error = serialized.empty() ? 1 : serialized.size() > (qualified ? 8192u : 255u) ? 2 : 0;
            std::vector<std::string> names;
            for (std::size_t begin = 0; !editor.error;)
            {
                const auto end = serialized.find('|', begin);
                const auto token = std::string_view(serialized).substr(begin,
                    end == std::string::npos ? std::string::npos : end - begin);
                const auto at = token.find('@');
                if (qualified && (at == std::string_view::npos || token.find('@', at + 1) != std::string_view::npos))
                { editor.error = 3; break; }
                const auto name = gimi::LowerAscii(gimi::Trim(qualified ? token.substr(at + 1) : token));
                if (name.empty() || name.size() > 255 || names.size() >= gimi::runtime::MaxComponentSelections) { editor.error = 1; break; }
                if (!validTarget(qualified ? token.substr(0, at) : std::string_view(editor.trigger)))
                { editor.error = 4; break; }
                if (std::find(names.begin(), names.end(), name) != names.end()) { editor.error = 5; break; }
                names.push_back(name);
                if (end == std::string::npos) break;
                begin = end + 1;
            }
            if (!editor.error)
            {
                config->GimiIni = editor.ini;
                config->GimiComponent = serialized;
                config->GimiTrigger = editor.trigger;
                config->GimiLibraries = editor.libraries;
                config->GimiHostRoot = editor.hostRoot;
                config->GimiNamespaceRoot = editor.namespaceRoot;
                auto applied = current;
                applied.enabled = enabled; applied.ini = editor.ini; applied.component = serialized;
                applied.trigger = editor.trigger; applied.libraries = editor.libraries;
                applied.hostRoot = editor.hostRoot; applied.namespaceRoot = editor.namespaceRoot;
                gimi::runtime::Configure(applied, true);
                editor.baseline = std::move(applied);
                editor.dirty = editor.componentEdited = false;
            }
        }
        if (editor.error)
        {
            const char* errors[] = {"",
                text("需要 1–64 个非空入口；每个规范节名最多 255 字节。", "Use 1–64 nonempty entries; each canonical name is limited to 255 bytes."),
                text("入口串过长：带绑定的格式最多 8192 字节，旧式格式最多 255 字节。", "Entry list too long: qualified format allows 8192 bytes; legacy format allows 255 bytes."),
                text("请统一使用节名或 target@canonicalName；每项只能有一个 @。", "Use all plain names or all target@canonicalName; exactly one @ per qualified entry."),
                text("绑定范围：ib、vb0..7、ps-t0..127、vs 或 ps。", "Binding range: ib, vb0..7, ps-t0..127, vs or ps."),
                text("规范组件节名不能重复。", "Canonical component names must be distinct.")};
            MenuText::TextWrapped("%s", errors[editor.error]);
        }
        if (editor.dirty || conflict || editor.error)
        {
            if (MenuText::Button(text("重新载入已应用值###gimi.reload", "Reload applied values###gimi.reload"))) load();
            MenuText::ItemHelp(text("丢弃本面板未提交的编辑，重新读取当前设置；不写配置。",
                                   "Discard this panel's unsubmitted edits and read the current settings. Does not write configuration."));
        }
        const auto mod = gimi::runtime::Observe();
        MenuText::TextWrapped("%s", mod.message[0] ? mod.message : text("默认关闭，尚未选择入口", "Off by default; no entry selected"));
        if (mod.hostRoot[0] && MenuText::ItemHelpHovered() && ImGui::BeginTooltip())
        {
            MenuText::TextWrapped("%s", mod.hostRoot);
            MenuText::Text(text("宿主设置：映射 %u，原工具来源 %u，未激活 %u",
                                "Host settings: mapped %u, original-tool metadata %u, inactive %u"),
                mod.hostNativeSettings, mod.hostSourceSettings, mod.hostInactiveSettings);
            MenuText::TextWrapped("%s", text("原加载器/调试快捷键/磁盘工具不执行；沿 F6 既有入口管理。此状态不代表实际 Check、骨骼或像素已验收。",
                "Original launcher/debug keys/disk tools are not executed; use the existing F6 controls. This status does not qualify actual Checks, skeletons or pixels."));
            ImGui::EndTooltip();
        }
        MenuText::Text(text("执行 %llu / 匹配 %llu；退休中 %u，隔离 %u",
                            "Executed %llu / matched %llu; retiring %u, quarantined %u"),
            static_cast<unsigned long long>(mod.executed), static_cast<unsigned long long>(mod.matched), mod.pending, mod.quarantined);
        MenuText::ItemHelp(text("仅为真实运行计数，不代表整包加载、像素正确或游戏兼容验收。",
                               "Actual runtime counts only; they do not establish full-package loading, correct pixels or game compatibility."));
    }
}

void MenuCommon::RenderActiveUpscalerSettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& currentFeature = ctx.currentFeature;
    auto& menuResScale = ctx.menuResScale;
    auto& primaryGpu = *ctx.primaryGpu;

    static const bool genshinProcess = [] {
        wchar_t path[32768]{};
        const auto length = GetModuleFileNameW(nullptr, path, 32768);
        return length && length < 32768 && GenshinFsr::IsGenshinExecutable(std::wstring_view(path, length));
    }();
    if (genshinProcess)
    {
        // The existing bridge Enabled owner also persists this choice in Save Settings.
        // Keep this outside the current-feature gate so native FSR can be re-enabled.
        const auto controls = config->AcquireGenshinControls();
        bool enabled = controls.settings.enabled;
        if (MenuText::Checkbox("替换原神 FSR 为所选超分", &enabled))
            config->EditGenshinControls(controls.epoch, [enabled](GenshinLegacy::Settings& settings) { settings.enabled = enabled; });
        ShowHelpMarker("关闭后使用游戏原生 FSR；重新启用无需重新注入。依赖此接入的 NR/FG 暂停接收新帧，其他设置不变。点击保存设置后记住选择。");
    }

    if (currentFeature != nullptr && !currentFeature->IsFrozen())
    {
        if (native::IsVirtualUpscalerFeature(currentFeature))
            DlssNr::RenderF5LowUpscalerNote();

        // UPSCALERS -----------------------------
        MenuText::SeparatorText("超分辨率");
        ShowTooltip("选择超分方案");

        GetCurrentBackendInfo(state.api, currentBackend, &currentBackendName);

        std::string spoofingText;

        ImGui::PushItemWidth(180.0f * menuResScale);

        const bool usesDlssd = currentFeature->GetUpscalerType() == Upscaler::DLSSD;
        const bool usesDx12CompatLayer = currentFeature->IsWithDx12();

        switch (state.api)
        {
        case DX11:
            MenuText::Text(primaryGpu.name.c_str());

            MenuText::Text("D3D11 %s| %s %d.%d.%d%s", primaryGpu.usesDxvk ? "(DXVK) " : "",
                        currentFeature->ShortName().c_str(), currentFeature->Version().major,
                        currentFeature->Version().minor, currentFeature->Version().patch,
                        usesDx12CompatLayer ? " w/Dx12" : "");
            ImGui::SameLine(0.0f, 6.0f);
            MenuText::Text("| 输入：%s", ApiUpscalerInputName(state.currentInputApiName).c_str());

            ImGui::SameLine(0.0f, 6.0f);
            spoofingText = config->DxgiSpoofing.value_or_default() ? "On" : "关闭";
            MenuText::Text("| 伪装：%s", spoofingText.c_str());

            if (!usesDlssd)
                AddDx11Backends(currentBackend);

            break;

        case DX12:
            MenuText::Text(primaryGpu.name.c_str());

            MenuText::Text("D3D12 %s| %s %d.%d.%d", primaryGpu.usesDxvk ? "(DXVK) " : "",
                        currentFeature->ShortName().c_str(), currentFeature->Version().major,
                        currentFeature->Version().minor, currentFeature->Version().patch);
            ImGui::SameLine(0.0f, 6.0f);
            MenuText::Text("| 输入：%s", ApiUpscalerInputName(state.currentInputApiName).c_str());

            ImGui::SameLine(0.0f, 6.0f);
            spoofingText = config->DxgiSpoofing.value_or_default() ? "On" : "关闭";
            MenuText::Text("| 伪装：%s", spoofingText.c_str());

            if (!usesDlssd)
                AddDx12Backends(currentBackend);

            break;

        default:
            MenuText::Text(primaryGpu.name.c_str());

            MenuText::Text("Vulkan %s| %s %d.%d.%d%s", primaryGpu.usesDxvk ? "(DXVK) " : "",
                        currentFeature->ShortName().c_str(), currentFeature->Version().major,
                        currentFeature->Version().minor, currentFeature->Version().patch,
                        usesDx12CompatLayer ? " w/Dx12" : "");
            ImGui::SameLine(0.0f, 6.0f);
            MenuText::Text("| 输入：%s", ApiUpscalerInputName(state.currentInputApiName).c_str());

            auto vlkSpoof = config->VulkanSpoofing.value_or_default();
            auto vlkExtSpoof = config->VulkanExtensionSpoofing.value_or_default();

            if (vlkSpoof && vlkExtSpoof)
                spoofingText = "开启＋扩展";
            else if (vlkSpoof)
                spoofingText = "On";
            else if (vlkExtSpoof)
                spoofingText = "仅扩展";
            else
                spoofingText = "关闭";

            ImGui::SameLine(0.0f, 6.0f);
            MenuText::Text("| 伪装：%s", spoofingText.c_str());

            if (!usesDlssd)
                AddVulkanBackends(currentBackend);
        }

        ImGui::PopItemWidth();

        if (!usesDlssd)
        {
            ImGui::SameLine(0.0f, 6.0f);

            if (MenuText::Button("切换超分##2") && state.newBackend != Upscaler::Reset &&
                state.newBackend != currentBackend)
            {
                if (state.newBackend == Upscaler::XeSS)
                {
                    // Reseting them for xess
                    config->DisableReactiveMask.reset();
                    config->DlssReactiveMaskBias.reset();
                }

                MARK_ALL_BACKENDS_CHANGED();
            }
        }

        if (currentFeature->AccessToReactiveMask())
        {
            ImGui::BeginDisabled(config->DisableReactiveMask.value_or(false));

            auto useAsTransparency = config->FsrUseMaskForTransparency.value_or_default();
            if (MenuText::Checkbox("将反应遮罩用作透明度遮罩", &useAsTransparency))
                config->FsrUseMaskForTransparency = useAsTransparency;

            ImGui::EndDisabled();
        }

        if (primaryGpu.dlssCapable && !state.NVNGX_DLSS_Path.has_value())
        {
            ImGui::Spacing();
            MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)), "未找到 nvngx_dlss.dll，DLSS 已关闭。");
        }
    }

    if (currentFeature != nullptr && !currentFeature->IsFrozen())
    {
        const bool usesDlssd = currentFeature->GetUpscalerType() == Upscaler::DLSSD;

        // Dx11 with Dx12
        if (state.api == DX11 && currentFeature->IsWithDx12())
        {
            ImGui::Spacing();
            if (auto ch = ScopedCollapsingHeader("DX11 转 DX12 设置"); ch.IsHeaderOpen())
            {
                ScopedIndent indent {};
                ImGui::Spacing();

                if (bool dontUseNTShared = config->DontUseNTShared.value_or_default();
                    MenuText::Checkbox("不使用 NTShared", &dontUseNTShared))
                    config->DontUseNTShared = dontUseNTShared;

                ImGui::Spacing();
                ImGui::Spacing();
            }
        }

        if (state.api == Vulkan && currentFeature->IsWithDx12())
        {
            ImGui::Spacing();
            if (auto ch = ScopedCollapsingHeader("Vulkan 转 DX12 设置"); ch.IsHeaderOpen())
            {
                ScopedIndent indent {};
                ImGui::Spacing();

                if (bool inputsUseCopy = config->VulkanUseCopyForInputs.value_or_default();
                    MenuText::Checkbox("用 CopyResource 复制输入", &inputsUseCopy))
                    config->VulkanUseCopyForInputs = inputsUseCopy;

                if (bool outputUseCopy = config->VulkanUseCopyForOutput.value_or_default();
                    MenuText::Checkbox("用 CopyResource 复制输出", &outputUseCopy))
                    config->VulkanUseCopyForOutput = outputUseCopy;

                ImGui::Spacing();
                ImGui::Spacing();
            }
        }

        // UPSCALER SPECIFIC -----------------------------

        // XeSS -----------------------------
        if (currentBackend == Upscaler::XeSS && !usesDlssd)
        {
            ImGui::Spacing();
            if (auto ch = ScopedCollapsingHeader("XeSS 设置"); ch.IsHeaderOpen())
            {
                ScopedIndent indent {};
                ImGui::Spacing();

                const char* models[] = { "KPSS", "SPLAT", "MODEL_3", "MODEL_4", "MODEL_5", "MODEL_6" };
                auto configModes = config->NetworkModel.value_or_default();

                if (configModes < 0 || configModes > 5)
                    configModes = 0;

                const char* selectedModel = models[configModes];

                if (MenuText::BeginCombo("网络模型", selectedModel))
                {
                    for (int n = 0; n < 6; n++)
                    {
                        if (MenuText::Selectable(models[n], (config->NetworkModel.value_or_default() == n)))
                        {
                            config->NetworkModel = n;
                            state.newBackend = currentBackend;
                            MARK_ALL_BACKENDS_CHANGED();
                        }
                    }

                    ImGui::EndCombo();
                }
                ShowHelpMarker("可能没有明显作用");

                if (bool dbg = state.xessDebug; MenuText::CheckboxInline("转储（Shift+Del）", &dbg))
                    state.xessDebug = dbg;

                ImGui::SameLine(0.0f, 6.0f);
                int dbgCount = state.xessDebugFrames;

                ImGui::PushItemWidth(95.0f * menuResScale);
                if (MenuText::InputInt("帧", &dbgCount))
                {
                    if (dbgCount < 4)
                        dbgCount = 4;
                    else if (dbgCount > 999)
                        dbgCount = 999;

                    state.xessDebugFrames = dbgCount;
                }

                ImGui::PopItemWidth();

                ImGui::Spacing();
                ImGui::Spacing();
            }
        }

        // FFX -----------------
        if (!usesDlssd && (currentBackend == Upscaler::FFX || currentBackend == Upscaler::FFX_on12))
        {
            MenuText::SeparatorText("FFX 设置");

            if (_ffxUpscalerIndex < 0)
                _ffxUpscalerIndex = config->FfxUpscalerIndex.value_or_default();

            if (currentBackend == Upscaler::FFX ||
                currentBackend == Upscaler::FFX_on12 && state.ffxUpscalerVersionNames.size() > 0)
            {
                const int ffxUpscalerCount = (int) state.ffxUpscalerVersionIds.size();
                if (ffxUpscalerCount > 0 && ffxUpscalerCount < 5)
                {
                    // The SDK list is short enough to fold into a single-select group; the
                    // pending choice still only commits through the "切换超分" button below.
                    std::vector<std::string> labels;
                    std::vector<const char*> labelPointers;
                    labels.reserve((size_t) ffxUpscalerCount);
                    for (int n = 0; n < ffxUpscalerCount; ++n)
                        labels.push_back(StrFmt("FSR %s", state.ffxUpscalerVersionNames[n]));
                    for (const auto& label : labels)
                        labelPointers.push_back(label.c_str());

                    int choice = _ffxUpscalerIndex;
                    if (MenuText::Segmented("FFX 超分", &choice, labelPointers.data(), ffxUpscalerCount))
                        _ffxUpscalerIndex = choice;
                }
                else
                {
                    ImGui::PushItemWidth(135.0f * menuResScale);

                    auto currentName = StrFmt("FSR %s", state.ffxUpscalerVersionNames[_ffxUpscalerIndex]);
                    if (MenuText::BeginCombo("FFX 超分", currentName.c_str()))
                    {
                        for (int n = 0; n < state.ffxUpscalerVersionIds.size(); n++)
                        {
                            auto name = StrFmt("FSR %s##%d", state.ffxUpscalerVersionNames[n], n);
                            if (MenuText::Selectable(name.c_str(), config->FfxUpscalerIndex.value_or_default() == n))
                                _ffxUpscalerIndex = n;
                        }

                        ImGui::EndCombo();
                    }
                    ImGui::PopItemWidth();
                }

                ShowHelpMarker("FFX SDK 报告的超分后端列表");

                ImGui::SameLine(0.0f, 6.0f);

                if (MenuText::Button("切换超分") &&
                    _ffxUpscalerIndex != config->FfxUpscalerIndex.value_or_default())
                {
                    config->FfxUpscalerIndex = _ffxUpscalerIndex;
                    state.newBackend = currentBackend;
                    MARK_ALL_BACKENDS_CHANGED();
                }

                auto majorFsrVersion = currentFeature->Version().major;

                if (majorFsrVersion >= 4)
                {
                    ImGui::Spacing();

                    // Colorspaces
                    const char* colorSpaces[] = { "线性（默认）", "非线性", "非线性 sRGB",
                                                  "非线性 PQ" };
                    int currentColorSpace = 0;
                    if (config->FsrNonLinearPQ.value_or_default())
                        currentColorSpace = 3;
                    else if (config->FsrNonLinearSRGB.value_or_default())
                        currentColorSpace = 2;
                    else if (config->FsrNonLinearColorSpace.value_or_default())
                        currentColorSpace = 1;

                    if (MenuText::Segmented("输入色彩空间", &currentColorSpace, colorSpaces,
                                            IM_ARRAYSIZE(colorSpaces)))
                    {
                        bool isSrgb = (currentColorSpace == 2);
                        bool isPq = (currentColorSpace == 3);

                        config->FsrNonLinearSRGB = isSrgb;
                        config->FsrNonLinearPQ = isPq;

                        if (isSrgb || isPq)
                        {
                            config->FsrNonLinearColorSpace.set_volatile_value(true);
                        }
                        else if (currentColorSpace == 1) // Just non-Linear
                        {
                            config->FsrNonLinearColorSpace = true;
                        }
                        else // Linear
                        {
                            config->FsrNonLinearColorSpace = false;
                        }

                        state.newBackend = currentBackend;
                        MARK_ALL_BACKENDS_CHANGED();
                    }
                    ShowHelpMarker("选择游戏输入的色彩空间。\n非线性／sRGB 可能改善 FSR4 质量，也可能增加拖影。\nPQ 较少见，可能增加拖影或造成光照异常。");

                    // FSR 4 Presets
                    const char* presets[] = { "默认",  "预设 0", "预设 1", "预设 2",
                                              "预设 3", "预设 4", "预设 5" };
                    int currentPresetIdx = config->Fsr4Preset.has_value() ? config->Fsr4Preset.value() + 1 : 0;

                    if (currentPresetIdx < 0 || currentPresetIdx >= IM_ARRAYSIZE(presets))
                        currentPresetIdx = 0;

                    ImGui::SetNextItemWidth(150.0f * menuResScale);
                    if (MenuText::Combo("FSR4 预设", &currentPresetIdx, presets, IM_ARRAYSIZE(presets)))
                    {
                        if (currentPresetIdx == 0)
                            config->Fsr4Preset.reset();
                        else
                            config->Fsr4Preset = currentPresetIdx - 1;

                        state.newBackend = currentBackend;
                        MARK_ALL_BACKENDS_CHANGED();
                    }
                    ShowHelpMarker("FSR4 内部预设按不同分辨率调优，不会改变游戏里的质量档位。\n0：原生抗锯齿；1：质量／超高质量；2：均衡；3：性能；4：动态分辨率；5：极致性能。");

                    // Display the active preset right next to the combo box instead of using a table
                    ImGui::SameLine();
                    if (state.currentFsr4Preset.has_value())
                        MenuText::TextDisabled("（当前：%d）", state.currentFsr4Preset.value());
                    else if (FSR4ModelSelection::IsInt8FsrHooked())
                        MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)), "（可能回退到 FSR3）");
                    else
                        MenuText::TextDisabled("（钩子安装失败）");
                }

                if (majorFsrVersion >= 3)
                {
                    ImGui::Spacing();

                    bool debugView = config->FsrDebugView.value_or_default();
                    if (MenuText::Checkbox("超分调试视图", &debugView))
                    {
                        config->FsrDebugView = debugView;

                        // FSR 4's debug view requires backend reinit
                        if (majorFsrVersion > 3)
                        {
                            state.newBackend = currentBackend;
                            MARK_ALL_BACKENDS_CHANGED();
                        }
                    }

                    if (majorFsrVersion > 3)
                    {
                        ShowHelpMarker("左上：膨胀运动矢量\n右上：预测混合系数");
                    }
                    else
                    {
                        ShowHelpMarker("左上：膨胀运动矢量；上中：保护区域；右上：膨胀深度。\n中间：超分画面。\n左下：遮挡解除遮罩；下中：反应性；右下：细节保护抑制。");
                    }

                    if (majorFsrVersion > 3)
                    {
                        ImGui::SameLine(0.0f, 20.0f * menuResScale);
                        bool fsr4wm = config->Fsr4EnableWatermark.value_or_default();
                        if (MenuText::Checkbox("水印", &fsr4wm))
                        {
                            LOG_DEBUG("FSR4 Watermark set to {}", fsr4wm);
                            config->Fsr4EnableWatermark = fsr4wm;
                        }

                        ShowHelpMarker("更改后请保存设置，下次启动生效。");
                    }
                }

                if (currentFeature->Version() >= feature_version { 3, 1, 1 } &&
                    currentFeature->Version() < feature_version { 4, 0, 0 })
                {
                    ImGui::Spacing();

                    if (currentFeature != nullptr)
                    {
                        MenuText::Text("FSR 3.1 预设：");

                        ImGui::SameLine(0.0f, 6.0f);

                        // This will be applied by default
                        if (MenuText::Button("稳定性"))
                        {
                            auto const scaleRatioX =
                                (float) currentFeature->TargetWidth() / (float) currentFeature->RenderWidth();
                            auto const scaleRatioY =
                                (float) currentFeature->TargetHeight() / (float) currentFeature->RenderHeight();
                            auto const scaleRatio = std::max(scaleRatioX, scaleRatioY);

                            config->FsrVelocity = 0.5f;
                            config->FsrReactiveScale = 0.25f;

                            config->FsrShadingScale.reset();
                            config->FsrAccAddPerFrame.reset();
                            config->FsrMinDisOccAcc.reset();
                            config->FsrShadingScale.set_volatile_value(0.5f / scaleRatio);
                            config->FsrAccAddPerFrame.set_volatile_value(scaleRatio / 10.0f);
                            config->FsrMinDisOccAcc.set_volatile_value(scaleRatio / 20.0f);
                        }

                        ImGui::SameLine(0.0f, 6.0f);

                        if (MenuText::Button("运动"))
                        {
                            auto const scaleRatioX =
                                (float) currentFeature->TargetWidth() / (float) currentFeature->RenderWidth();
                            auto const scaleRatioY =
                                (float) currentFeature->TargetHeight() / (float) currentFeature->RenderHeight();
                            auto const scaleRatio = std::max(scaleRatioX, scaleRatioY);

                            config->FsrVelocity = 1.0f;
                            config->FsrReactiveScale = 0.5f;

                            config->FsrShadingScale.reset();
                            config->FsrAccAddPerFrame.reset();
                            config->FsrMinDisOccAcc.reset();
                            config->FsrShadingScale.set_volatile_value(1.0f / scaleRatio);
                            config->FsrAccAddPerFrame.set_volatile_value(scaleRatio / 10.0f);
                            config->FsrMinDisOccAcc.set_volatile_value(scaleRatio / 20.0f);
                        }

                        ImGui::SameLine(0.0f, 6.0f);

                        if (MenuText::Button("默认"))
                        {
                            config->FsrVelocity = 1.0f;
                            config->FsrReactiveScale = 1.0f;
                            config->FsrShadingScale = 1.0f;
                            config->FsrAccAddPerFrame = 0.333f;
                            config->FsrMinDisOccAcc = -0.333f;
                        }
                    }

                    ImGui::Spacing();

                    if (auto ch = ScopedCollapsingHeader("FSR3 超分手动调优"); ch.IsHeaderOpen())
                    {
                        ScopedIndent indent {};
                        ImGui::Spacing();
                        ImGui::Spacing();

                        ImGui::PushItemWidth(220.0f * menuResScale);

                        float velocity = config->FsrVelocity.value_or_default();
                        if (MenuText::SliderFloat("速度系数", &velocity, 0.00f, 1.0f, "%.2f"))
                            config->FsrVelocity = velocity;

                        ShowHelpMarker("0 可改善亮像素的时序稳定性。低值更稳定但拖影更多，高值可能更像素化但拖影较少。");

                        if (currentFeature->Version() >= feature_version { 3, 1, 4 })
                        {
                            // Reactive Scale
                            float reactiveScale = config->FsrReactiveScale.value_or_default();
                            if (MenuText::SliderFloat("反应性缩放", &reactiveScale, 0.0f, 1.0f, "%.3f"))
                                config->FsrReactiveScale = reactiveScale;

                            ShowHelpMarker("开发测试选项：提高反应遮罩数值，观察是否减少拖影。");

                            // Shading Scale
                            float shadingScale = config->FsrShadingScale.value_or_default();
                            if (MenuText::SliderFloat("着色变化缩放", &shadingScale, 0.0f, 1.0f, "%.3f"))
                                config->FsrShadingScale = shadingScale;

                            ShowHelpMarker("放大 FSR 3.1 计算的着色变化，使反应性更强。");

                            // Accumulation Added Per Frame
                            float accAddPerFrame = config->FsrAccAddPerFrame.value_or_default();
                            if (MenuText::SliderFloat("每帧累计增量", &accAddPerFrame, 0.0f, 1.0f, "%.3f"))
                                config->FsrAccAddPerFrame = accAddPerFrame;

                            ShowHelpMarker("控制遮挡解除或反应遮罩大于 0 时的每帧累计。降低此值并为拖影物体设置接近 1 的反应遮罩可减少拖影，但可能增加细小结构闪烁。");

                            // Min Disocclusion Accumulation
                            float minDisOccAcc = config->FsrMinDisOccAcc.value_or_default();
                            if (MenuText::SliderFloat("遮挡解除最低累计", &minDisOccAcc, -1.0f, 1.0f, "%.3f"))
                                config->FsrMinDisOccAcc = minDisOccAcc;

                            ShowHelpMarker("提高可减少相互遮挡的细物体周围白像素闪烁，过高可能增加拖影。");
                        }

                        ImGui::PopItemWidth();

                        ImGui::Spacing();
                        ImGui::Spacing();
                    }
                }
            }
        }

        // DLSS -----------------
        if ((config->DLSSEnabled.value_or_default() && currentBackend == Upscaler::DLSS &&
             currentFeature->Version().major > 2) ||
            usesDlssd)
        {

            if (usesDlssd)
                MenuText::SeparatorText("光线重建设置");
            else
                MenuText::SeparatorText("DLSS 设置");

            auto overridden =
                usesDlssd ? state.dlssdPresetsOverriddenExternally : state.dlssPresetsOverriddenExternally;

            if (overridden)
            {
                MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)), "预设正被外部工具覆盖");
                ShowHelpMarker("通常由 NVIDIA App 或 NVIDIA Inspector 等工具的覆盖设置引起。");
                // MenuText::Text("选择下方设置会关闭外部覆盖。\n"//             "but you need to Save Settings and restart the game");

                ImGui::Spacing();
            }

            if (usesDlssd)
            {
                if (bool pOverride = config->DLSSDRenderPresetOverride.value_or_default();
                    MenuText::Checkbox("渲染预设覆盖", &pOverride))
                    config->DLSSDRenderPresetOverride = pOverride;

                ShowHelpMarker("各预设各有优缺点。覆盖可能改善画质；启用或关闭后点击应用。");

                /*
                auto currentPresetIndex = GetPresetIndex(currentFeature, true);

                if (currentPresetIndex == 0)
                    MenuText::Text("当前预设：默认");
                else
                    MenuText::Text("当前预设：%c", 64 + currentPresetIndex);
                */

                ImGui::BeginDisabled(!config->DLSSDRenderPresetOverride.value_or_default() /*|| overridden*/);
                ImGui::PushItemWidth(135.0f * menuResScale);

                AddDLSSDRenderPreset("覆盖预设", &comboPreset);

                ImGui::PopItemWidth();
                ImGui::EndDisabled();
            }
            else
            {
                if (bool pOverride = config->RenderPresetOverride.value_or_default();
                    MenuText::Checkbox("渲染预设覆盖", &pOverride))
                    config->RenderPresetOverride = pOverride;

                ShowHelpMarker("各预设各有优缺点。覆盖可能改善画质；启用或关闭后点击应用。");

                /*
                auto currentPresetIndex = GetPresetIndex(currentFeature, false);

                if (currentPresetIndex == 0)
                    MenuText::Text("当前预设：默认");
                else
                    MenuText::Text("当前预设：%c", 64 + currentPresetIndex);
                */

                ImGui::BeginDisabled(!config->RenderPresetOverride.value_or_default() /*|| overridden*/);

                ImGui::PushItemWidth(135.0f * menuResScale);

                AddDLSSRenderPreset("覆盖预设", &comboPreset);

                ImGui::PopItemWidth();
                ImGui::EndDisabled();
            }

            ImGui::SameLine(0.0f, 6.0f);

            if (MenuText::Button("应用更改"))
            {
                LOG_DEBUG("Applying DLSS/DLSSD preset override changes, preset index: {}",
                          comboPreset.value_or_default());

                if (usesDlssd)
                {
                    config->DLSSDRenderPresetForAll = comboPreset.value_or_default();
                    state.newBackend = Upscaler::DLSSD;
                }
                else
                {
                    config->RenderPresetForAll = comboPreset.value_or_default();
                    state.newBackend = currentBackend;
                }

                MARK_ALL_BACKENDS_CHANGED();
            }

            ImGui::Spacing();

            if (auto ch = ScopedCollapsingHeader(usesDlssd ? "高级光线重建设置" : "高级 DLSS 设置");
                ch.IsHeaderOpen())
            {
                ScopedIndent indent {};
                ImGui::Spacing();

                bool appIdOverride = config->UseGenericAppIdWithDlss.value_or_default();
                if (MenuText::Checkbox("DLSS 使用通用应用编号", &appIdOverride))
                    config->UseGenericAppIdWithDlss = appIdOverride;

                ShowHelpMarker("NGX 使用通用应用编号，可解决部分游戏预设覆盖不生效的问题。需要重启游戏。");

                ImGui::BeginDisabled(!config->RenderPresetOverride.value_or_default() || overridden);
                ImGui::Spacing();
                ImGui::PushItemWidth(135.0f * menuResScale);

                if (usesDlssd)
                {
                    AddDLSSDRenderPreset("DLAA 预设", &config->DLSSDRenderPresetDLAA);
                    AddDLSSDRenderPreset("超高质量预设", &config->DLSSDRenderPresetUltraQuality);
                    AddDLSSDRenderPreset("质量预设", &config->DLSSDRenderPresetQuality);
                    AddDLSSDRenderPreset("均衡预设", &config->DLSSDRenderPresetBalanced);
                    AddDLSSDRenderPreset("性能预设", &config->DLSSDRenderPresetPerformance);
                    AddDLSSDRenderPreset("极致性能预设", &config->DLSSDRenderPresetUltraPerformance);
                }
                else
                {
                    AddDLSSRenderPreset("DLAA 预设", &config->RenderPresetDLAA);
                    AddDLSSRenderPreset("超高质量预设", &config->RenderPresetUltraQuality);
                    AddDLSSRenderPreset("质量预设", &config->RenderPresetQuality);
                    AddDLSSRenderPreset("均衡预设", &config->RenderPresetBalanced);
                    AddDLSSRenderPreset("性能预设", &config->RenderPresetPerformance);
                    AddDLSSRenderPreset("极致性能预设", &config->RenderPresetUltraPerformance);
                }
                ImGui::PopItemWidth();
                ImGui::EndDisabled();

                ImGui::Spacing();
                ImGui::Spacing();
            }
        }
    }
}

// Options for the built-in RTX 40 unlock, shown only while it is on and not overridden by another
// unlocker. Startup settings: they apply when DLSSG loads, so a change needs a restart, and the result of
// each is shown directly under it.
static void RenderAdaUnlockOptions(Config* config, const MfgUnlock::Status& status,
                                   void (*showHelp)(const char*))
{
    if (!MenuText::CollapsingHeader("RTX 40（Ada）多倍插帧解锁选项"))
        return;

    ImGui::Indent();

    // Frame timing fix. Auto is resolved inline, like the RTX 20/30 "Kernel Image" combo below.
    const auto resolved = MfgUnlock::ConfiguredTemporalMethod();
    const std::string autoLabel = resolved == MfgUnlock::TemporalMethod::None
                                      ? "自动（AdaBlackwellKernels=false，关闭）"
                                      : "自动（复用 Blackwell 内核）";
    const char* options[] = { autoLabel.c_str(), "复用 Blackwell 内核", "重写混合权重（PTX）" };

    const std::string chosen = config->FGDLSSGAdaTemporalFix.value_or("自动");
    int index = chosen == "重定向" ? 1 : chosen == "Ptx" ? 2 : 0;

    if (MenuText::Segmented("帧计时修正##ada", &index, options, 3))
    {
        const char* stored[] = { "自动", "重定向", "Ptx" };
        config->FGDLSSGAdaTemporalFix = std::string(stored[index]);
    }
    showHelp("超过 2X 时，生成帧可能都落在两个真实帧的中点，帧数增加却不更流畅。此功能给各帧独立时间位置。\n自动／复用 Blackwell 内核：使用模块已有内插内核，默认方案。\n重写混合权重：修改已识别 Ada 内核的 PTX，仅运动不更流畅时尝试。\n对应 [DLSSG] AdaTemporalFix，保存并重启。");

    // The result, directly under the control (which method applied, or the specific reason it did not).
    if (!status.ModuleFound)
    {
        MenuText::TextDisabled("尚未应用：DLSS-G 未加载。");
    }
    else if (status.KernelsRewritten > 0)
    {
        const bool ptx = status.TemporalAttempted == MfgUnlock::TemporalMethod::Ptx;
        MenuText::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "已应用：%s，%u %s",
                           ptx ? "重写混合权重" : "复用 Blackwell 内核", status.KernelsRewritten,
                           ptx ? "个描述符" : "个内核组");
    }
    else
    {
        MenuText::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "未应用：%s。",
                           status.TemporalDetail.empty() ? "未记录原因" : status.TemporalDetail.c_str());
    }

    if (status.ModuleFound && status.TemporalAttempted != resolved)
        MenuText::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "保存设置并重启后生效。");

    ImGui::Spacing();

    // Software frame pacing. Second because it is the rarer need: a freeze above 2X, not a setting every
    // unlock user wants.
    bool softwarePacing = config->FGDLSSGAdaFlipMeteringPatch.value_or_default();

    if (MenuText::Checkbox("软件帧调度（仅 3X 以上卡死时尝试）##ada", &softwarePacing))
        config->FGDLSSGAdaFlipMeteringPatch = softwarePacing;
    showHelp("硬件翻转计量开启时，多生成帧可能令画面停住。此功能在识别到受支持插件时，修改内存中的 Streamline DLSS-G 插件，改用软件帧调度。仅 3X 以上卡死时尝试；可先用更温和的 INI 设置 [NvApi] DisableFlipMetering=true。\n对应 [DLSSG] AdaFlipMeteringPatch，保存并重启。");

    const std::string_view pacing = status.FlipMetering;

    if (pacing.empty())
    {
        if (softwarePacing)
            MenuText::TextDisabled("尚未应用：Streamline DLSS-G 插件未加载。");
    }
    else if (pacing == "已修补")
    {
        MenuText::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "已应用到 %u 处。", status.FlipSites);
    }
    else if (pacing != "关闭")
    {
        MenuText::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "未应用：%s。", status.FlipMetering);
    }

    if (!pacing.empty() && softwarePacing != status.FlipRequested)
        MenuText::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "保存设置并重启后生效。");

    ImGui::Unindent();
}

// One line under the DLSSG override slider. The slider says what was requested; this says whether it
// happened: what the game asked for, what was sent on after any override, and what Streamline reports it
// presented at the last check. Nothing is drawn while DLSS-G is off.
static void RenderDlssgTelemetry()
{
    const auto& telemetry = MfgUnlock::GetTelemetry();

    if (!telemetry.optionsSeen.load(std::memory_order_acquire) || !telemetry.active.load(std::memory_order_relaxed))
        return;

    if (!telemetry.stateSeen.load(std::memory_order_acquire))
    {
        MenuText::TextDisabled("DLSS-G：等待 Streamline 状态…");
        return;
    }

    const unsigned int requestedX = telemetry.requested.load(std::memory_order_relaxed) + 1;
    const unsigned int sentX = telemetry.sent.load(std::memory_order_relaxed) + 1;
    const unsigned int presented = telemetry.presented.load(std::memory_order_relaxed);
    const unsigned int result = telemetry.result.load(std::memory_order_relaxed);

    const bool agrees = result == 0 && presented == sentX;
    const ImVec4 green(0.4f, 0.9f, 0.5f, 1.0f);
    const ImVec4 amber(0.95f, 0.70f, 0.20f, 1.0f);

    MenuText::TextColored(agrees ? green : amber, "游戏请求 %uX，发送 %uX，Streamline 呈现 %u 帧（最高 %u）",
                       requestedX, sentX, presented, telemetry.maxPresented.load(std::memory_order_relaxed));

    if (result != 0)
        MenuText::TextColored(amber, "此次请求的 slDLSSGSetOptions 返回值：%u。", result);

    // Only when the symptom is there: 3X or more sent, fewer presented, and nothing pacing in software.
    if (result == 0 && sentX > 2 && presented < sentX && MfgUnlock::UnlockedMax() > 0 && !MfgUnlock::SoftwarePacing())
        MenuText::TextColored(amber, "画面停住时，可尝试 RTX 40 解锁设置中的软件帧调度。");
}

void MenuCommon::RenderFrameGenerationSelection(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    DlssNr::RenderF5LowFrameGenHint(config, ctx.currentFeature == nullptr || !ctx.currentFeature->IsInited());
    bool external = config->ExternalFrameGeneration.value_or_default();
    const bool ampereActive = config->FGDLSSGAmpereMfgUnlock.value_or_default();
    if (ampereActive)
    {
        external = true;
        ImGui::BeginDisabled();
        MenuText::Checkbox("禁用内置帧生成###外部插帧／多倍插帧解锁器", &external);
        ImGui::EndDisabled();
        ShowHelpMarker("Ampere SM86 多倍插帧解锁器已启用，此选项自动锁定为开启。若需关闭，请先关闭下方 SM86 解锁。");
    }
    else
    {
        if (MenuText::Checkbox("禁用内置帧生成###外部插帧／多倍插帧解锁器", &external))
            config->ExternalFrameGeneration = external;
        ShowHelpMarker("让游戏或外部模组控制 Streamline、Reflex 和插帧。NR 和 NGX 超分仍可用。保存后重启；此开关不安装解锁器，也不为不支持的游戏添加插帧。");
    }
    if (external != state.externalFrameGeneration)
        MenuText::TextWrapped("更改插帧控制方需要保存设置并重启游戏。");

    auto& menuResScale = ctx.menuResScale;
    auto& primaryGpu = *ctx.primaryGpu;

    // A Vulkan game gets OptiFG and the outputs through the present bridge (native/VkPresentBridge.h), which is made when the
    // game starts: they are offered while it is up, and before it while Optical F5Low can make one (choose, save, restart).
    const bool vulkanNoFg = state.swapchainApi == API::Vulkan && state.swapchainInteropApi != SwapchainInteropApi::VkwDx12 &&
                            (MenuPrimaryGpu().usesDxvk || !config->DlssNrEnabled.value_or_default());

    /// FG INPUTS
    bool adaUnlock = config->FGDLSSGAdaMfgUnlock.value_or_default();
    const bool disableAda = ampereActive || state.externalFrameGeneration;

    if (disableAda)
    {
        ImGui::BeginDisabled();
        MenuText::Checkbox("40系 多倍帧生成解锁###内置 RTX 40 多倍插帧解锁（实验功能，需重启）", &adaUnlock);
        ImGui::EndDisabled();
        if (ampereActive)
        {
            ShowHelpMarker("RTX 30 SM86 解锁已启用，此功能关闭。请先关闭 AmpereMfgUnlock，保存并重启。");
        }
        else
        {
            ShowHelpMarker("外部插帧已启用，此功能关闭。请先关闭外部插帧，保存并重启。");
        }
    }
    else
    {
        if (MenuText::Checkbox("40系 多倍帧生成解锁###内置 RTX 40 多倍插帧解锁（实验功能，需重启）", &adaUnlock))
            config->FGDLSSGAdaMfgUnlock = adaUnlock;
        ShowHelpMarker("解锁 RTX 40 多倍插帧。需支持的 DLSS-G 运行库和 Streamline 2.7.1 以上，保存并重启生效。勿与其他解锁器混用；此处尚未完成 RTX 40 实测。");
    }
    if (adaUnlock && !disableAda)
    {
        const auto& status = MfgUnlock::LastStatus();
        MenuText::TextWrapped("DLSS-G %s：能力 %s，验证 %s，重定向内核组 %u，Streamline 插件上限 %s",
                           status.SnippetVersion.empty() ? "未修补" : status.SnippetVersion.c_str(),
                           status.AdvertiseMatched ? "匹配" : "不匹配",
                           status.ValidateMatched ? "匹配" : "不匹配", status.KernelsRewritten,
                           status.PluginCeiling[0] != '\0' ? status.PluginCeiling : "尚未检测到");

        RenderAdaUnlockOptions(config, status, [](const char* tip) { ShowHelpMarker(tip); });
    }

    // ── Ampere/Turing (SM86/SM75) MFG Unlock ─────────────────────────
    if (MenuText::CollapsingHeader("RTX 20／30（SM75／SM86）多倍插帧解锁"))
    {
        ImGui::Indent();

        bool ampereUnlock = config->FGDLSSGAmpereMfgUnlock.value_or_default();

        // Mutual exclusion: disable if Ada is already enabled
        const bool adaActive = config->FGDLSSGAdaMfgUnlock.value_or_default();
        if (adaActive)
        {
            ImGui::BeginDisabled();
            MenuText::Checkbox("启用 SM86／SM75 多倍插帧（实验功能，需重启）##ampere", &ampereUnlock);
            ImGui::EndDisabled();
            ShowHelpMarker("RTX 40 Ada 解锁已启用，此功能关闭。请先关闭 AdaMfgUnlock，保存并重启。");
        }
        else
        {
            if (MenuText::Checkbox("启用 SM86／SM75 多倍插帧（实验功能，需重启）##ampere", &ampereUnlock))
            {
                config->FGDLSSGAmpereMfgUnlock = ampereUnlock;
                if (ampereUnlock)
                {
                    config->ExternalFrameGeneration = true;
                    config->FGDLSSGAdaMfgUnlock = false;
                }
            }
            ShowHelpMarker("sdli1995 Ampere／Turing 解锁器，侧载 dlssg_for_sm86 代理。自动启用外部插帧，倍数由游戏菜单控制。支持 RTX 20 与 RTX 30，保存并重启生效。勿与 Ada 或其他外部解锁器混用。");
        }

        if (ampereUnlock)
        {
            // Status display
            const auto& status = AmpereMfgLoader::LastStatus();
            if (!status.ErrorMessage.empty())
                MenuText::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "错误：%s", status.ErrorMessage.c_str());
            else
            {
                std::string routerStr = AmpereMfgLoader::ResolveRouter();
                MenuText::TextWrapped("DLL：%s | 路由器：%s | INI：%s | 已加载：%s",
                                   status.DllFound ? "已找到" : "缺失",
                                   routerStr.c_str(),
                                   status.IniWritten ? "已写入" : "未写入",
                                   status.DllLoaded ? "是" : "no");
            }

            // MaxGeneratedFrames slider
            int maxFrames = config->FGDLSSGAmpereMfgMaxFrames.value_or_default();
            const char* frameLabels[] = { "默认能力（3X）", "1 (2X)", "2 (3X)", "3 (4X)" };
            const char* currentLabel = (maxFrames >= 0 && maxFrames <= 3) ? frameLabels[maxFrames] : "默认能力（3X）";
            if (MenuText::SliderInt("最大生成帧数##sm86", &maxFrames, 0, 3, currentLabel))
                config->FGDLSSGAmpereMfgMaxFrames = maxFrames;
            ShowHelpMarker("报告给游戏的最大生成帧数：1＝2X，2＝3X，3＝4X，实际数量由游戏选择。0 为默认能力上限，最多 4X。保存并重启生效。");

            // KernelImage combo
            std::string resolvedAuto = AmpereMfgLoader::ResolveAutoKernelImage();
            std::string autoLabel = (resolvedAuto != "自动") ? "自动（" + resolvedAuto + "，当前 GPU）" : "自动";
            const char* kernelOptions[] = { autoLabel.c_str(), "PTX", "Cubin" };
            std::string current = config->FGDLSSGAmpereMfgKernelImage.value_or("自动");
            int kernelIdx = (current == "PTX") ? 1 : (current == "Cubin") ? 2 : 0;
            if (MenuText::Segmented("内核映像##sm86", &kernelIdx, kernelOptions, 3))
            {
                const char* storedOptions[] = { "自动", "PTX", "Cubin" };
                config->FGDLSSGAmpereMfgKernelImage = std::string(storedOptions[kernelIdx]);
            }
            ShowHelpMarker("自动选择格式。PTX 由驱动即时编译，适合 Linux／Proton、RTX 3080 Ti 和 RTX 20。Cubin 为预编译二进制，Windows 下需精确匹配物理 SM。保存并重启。");

            // HardwareBilinear checkbox
            bool hwBilinear = config->FGDLSSGAmpereMfgHardwareBilinear.value_or_default();
            if (MenuText::Checkbox("硬件双线性（近似采样）##sm86", &hwBilinear))
                config->FGDLSSGAmpereMfgHardwareBilinear = hwBilinear;
            ShowHelpMarker("仅 SM86／RTX 30。0 为默认精确输出；1 为可选近似硬件双线性，可能额外降低约 2–4% GPU 延迟。保存并重启。");
        }

        ImGui::Unindent();
    }

    if (state.externalFrameGeneration)
    {
        MenuText::TextWrapped("外部插帧已启用，请在游戏或解锁器中设置倍数。");
        return;
    }

    static std::vector<MenuOption<FGInput>> inputOptions;
    inputOptions.clear();

    // clang-format off

    inputOptions = {
        { FGInput::NoFG, "无" },
        { FGInput::Upscaler, "OptiFG（超分输入）",
            "需要启用超分，可搭配各插帧输出，但部分组合可能不完善。避免界面异常需要 HUD 修正。" },
        { FGInput::DLSSG, "通过 Streamline 的 DLSS-G",
            "可搭配各插帧输出。需在游戏启用 DLSS 插帧，原生支持去 HUD 输入；仅限使用 Streamline 的游戏。" },
        { FGInput::NvngxFG, "通过 NGX 的 DLSS-G",
            "限 FSR 插帧替代方案。需在游戏启用 DLSS 插帧，原生支持去 HUD 输入，使用 Streamline 交换链调度。" },
        { FGInput::FSRFG, "FSR 3.1 FG",
            "可搭配各插帧输出。需在游戏启用 FSR 插帧，原生支持去 HUD 输入。" },
        { FGInput::FSRFG30, "FSR 3.0 FG",
            "可搭配各插帧输出。需在游戏启用 FSR 插帧，原生支持去 HUD 输入。" },
        { FGInput::XeFG, "XeFG" }
    };

    // clang-format on

    auto constexpr nvngxInputIndex = (uint32_t) FGInput::NvngxFG;

    // XeFG input requirements
    auto constexpr xefgInputIndex = (uint32_t) FGInput::XeFG;
    inputOptions[xefgInputIndex].set_disabled(true, "尚未实现作为输入的支持，请使用插帧输出选项。");

    // OptiFG requirements
    auto constexpr optiFgIndex = (uint32_t) FGInput::Upscaler;
    inputOptions[optiFgIndex].set_disabled(vulkanNoFg, "不支持此图形接口");

    if (!inputOptions[optiFgIndex].disabled && state.activeFgOutput == FGOutput::FSRFG && !FfxApiProxy::IsFGReady() &&
        !ffxInitTried)
    {
        ffxInitTried = true;
        FfxApiProxy::InitFfxDx12();
        inputOptions[optiFgIndex].set_disabled(!FfxApiProxy::IsFGReady(), "缺少 amd_fidelityfx_dx12.dll");
    }
    else if (!inputOptions[optiFgIndex].disabled && state.activeFgOutput == FGOutput::XeFG && !xefgInitTried &&
             XeFGProxy::Module() == nullptr)
    {
        xefgInitTried = true;
        XeFGProxy::InitXeFG();
        inputOptions[optiFgIndex].set_disabled(XeFGProxy::Module() == nullptr, "缺少 libxess_fg.dll");
    }

    // DLSSG inputs requirements
    auto constexpr dlssgInputIndex = (uint32_t) FGInput::DLSSG;
    // inputOptions[dlssgInputIndex].set_disabled(state.streamlineVersion.major == 0, "Game doesn't use streamline");
    inputOptions[dlssgInputIndex].set_disabled(state.swapchainApi == API::DX11, "不支持此图形接口");

    // FSRFG inputs requirements
    auto constexpr fsrfgInputIndex = (uint32_t) FGInput::FSRFG;
    inputOptions[fsrfgInputIndex].set_disabled(state.swapchainApi != API::DX12, "不支持此图形接口");

    // FSRFG30 inputs requirements
    auto constexpr fsrfg30InputIndex = (uint32_t) FGInput::FSRFG30;
    inputOptions[fsrfg30InputIndex].set_disabled(state.swapchainApi != API::DX12, "不支持此图形接口");

    if (!config->FGInput.has_value())
        config->FGInput = config->FGInput.value_or_default(); // need to have a value before combo

    /// FG OUTPUTS

    static std::vector<MenuOption<FGOutput>> outputOptions;
    outputOptions.clear();

    // clang-format off

    outputOptions = {
        { FGOutput::NoFG, "无" },
        { FGOutput::FSRFG, "FSR FG", "FSR3／4 插帧，RDNA4 自动升级到 FSR4 插帧。效果与 XeFG 因场景而异。" },
        { FGOutput::DLSSG, "DLSSG", "DLSS-G 输出，可搭配 Nukem 等替代模块。" },
        { FGOutput::XeFG, "XeFG", "XeFG 开销较高，通用兼容性较好。XeFG 3 通常较好处理 HUD；界面拖影时可启用界面合成。" },
    };

    // clang-format on

    // DLSSG output requirements
    auto constexpr dlssgOutputIndex = (uint32_t) FGOutput::DLSSG;
    const bool supportsDlssg = primaryGpu.nvidiaArchInfo.architecture_id >= NV_GPU_ARCHITECTURE_AD100;
    const bool hasDlssgReplacement =
        state.nukemsFgFileAvailable || state.artursFgFileAvailable || FfxApiProxy::IsFGReady(false);

    if (!supportsDlssg && hasDlssgReplacement)
    {
        outputOptions[dlssgOutputIndex].tooltip =
            "硬件不支持原生 DLSS-G，仅可使用 NGX 插帧替代模块。";
    }

    outputOptions[dlssgOutputIndex].set_disabled(vulkanNoFg, "不支持此图形接口");
    outputOptions[dlssgOutputIndex].set_disabled(!supportsDlssg && !hasDlssgReplacement,
                                                 "硬件不支持，且没有替代模块。");

    // For that one case of DX11 DLSSG
    const auto streamlineVersion = state.streamlineVersion;
    const bool nukemsUnsupportedApi =
        state.swapchainApi == API::DX11 &&
        (streamlineVersion == feature_version { 0, 0, 0 } || streamlineVersion > feature_version { 2, 0, 1 });
    inputOptions[nvngxInputIndex].set_disabled(nukemsUnsupportedApi, "不支持此图形接口");

    // FSR FG output requirements
    auto constexpr fsrfgOutputIndex = (uint32_t) FGOutput::FSRFG;
    outputOptions[fsrfgOutputIndex].set_disabled(vulkanNoFg, "不支持此图形接口");

    // XeFG output requirements
    auto constexpr xefgOutputIndex = (uint32_t) FGOutput::XeFG;
    outputOptions[xefgOutputIndex].set_disabled(vulkanNoFg, "不支持此图形接口");
    // Unsupported FG input selected
    const auto currentInputIndex = (uint32_t) state.activeFgInput;
    if (config->FGInput != FGInput::NoFG && inputOptions.size() > currentInputIndex &&
        inputOptions[currentInputIndex].disabled && state.activeFgInput == config->FGInput)
    {
        LOG_WARN("Resetting FGInput to NoFG: {}", inputOptions[currentInputIndex].label);
        config->FGInput = FGInput::NoFG;

        // Changing active can be dangerous but we are talking about an unsupported mode
        // which shouldn't even actually have taken affect
        state.activeFgInput = FGInput::NoFG;
    }

    // Unsupported FG output selected
    const auto currentOutputIndex = (uint32_t) state.activeFgOutput;
    if (config->FGOutput != FGOutput::NoFG && outputOptions.size() > currentOutputIndex &&
        outputOptions[currentOutputIndex].disabled && state.activeFgOutput == config->FGOutput)
    {
        LOG_WARN("Resetting FGOutput to NoFG: {}", outputOptions[currentOutputIndex].label);
        config->FGOutput = FGOutput::NoFG;
        state.activeFgOutput = FGOutput::NoFG;
    }

    if (!config->FGOutput.has_value())
        config->FGOutput = config->FGOutput.value_or_default(); // need to have a value before combo

    /// FG NVNGX REPLACEMENT

    static std::vector<MenuOption<FGNvngxReplacement>> nvngxOptions;
    nvngxOptions.clear();

    // clang-format off

    nvngxOptions = {
        { FGNvngxReplacement::None, "无替代（原生 DLSS-G）", "原生 DLSS-G，用于 RTX 40 以上。"},
        { FGNvngxReplacement::Nukems, "Nukem's", "FSR 3 FG" },
        { FGNvngxReplacement::Arturs, "Enabler", "FSR3 多倍插帧" },
        { FGNvngxReplacement::FFX, "FSR 3/4 FG", "通过 FFX 的 FSR3／4 插帧" },
        { FGNvngxReplacement::Combo, "FFX + Enabler", "中间生成帧用 FFX，其余用 Enabler。\n2X：FFX；3X：Enabler；4X：两者；5X：Enabler；6X：两者。" },
    };

    // clang-format on

    bool replaceFgOutputWithNvngx = false;
    bool showNvngxFgDowndown = false;

    if (config->FGInput == FGInput::NvngxFG)
    {
        config->FGOutput = FGOutput::NoFG;
        replaceFgOutputWithNvngx = true;
    }
    else if (config->FGOutput == FGOutput::DLSSG)
    {
        showNvngxFgDowndown = true;
    }

    auto constexpr fgNvngxNoneIndex = (uint32_t) FGNvngxReplacement::None;
    nvngxOptions[fgNvngxNoneIndex].set_disabled(!supportsDlssg, "不支持此硬件");

    if (replaceFgOutputWithNvngx)
    {
        nvngxOptions[fgNvngxNoneIndex].label = "无";
        nvngxOptions[fgNvngxNoneIndex].set_hidden(true);
    }

    auto constexpr fgNvngxNukemsIndex = (uint32_t) FGNvngxReplacement::Nukems;
    nvngxOptions[fgNvngxNukemsIndex].set_disabled(!state.nukemsFgFileAvailable,
                                                  "缺少 dlssg_to_fsr3_amd_is_better.dll");

    auto constexpr fgNvngxArtursIndex = (uint32_t) FGNvngxReplacement::Arturs;
    nvngxOptions[fgNvngxArtursIndex].set_disabled(!state.artursFgFileAvailable, "缺少 dlss-enabler-headless.dll");

    auto constexpr fgNvngxFfxIndex = (uint32_t) FGNvngxReplacement::FFX;
    nvngxOptions[fgNvngxFfxIndex].set_disabled(!FfxApiProxy::IsFGReady(false),
                                               "缺少 amd_fidelityfx_framegeneration_dx12.dll");

    auto constexpr fgNvngxComboIndex = (uint32_t) FGNvngxReplacement::Combo;
    nvngxOptions[fgNvngxComboIndex].set_disabled(
        !FfxApiProxy::IsFGReady(false) || !state.artursFgFileAvailable,
        "缺少 amd_fidelityfx_framegeneration_dx12.dll 或 dlss-enabler-headless.dll");

    // TODO: Automatically switch to any other option

    if (!config->FGNvngxReplacement.has_value())
        config->FGNvngxReplacement = config->FGNvngxReplacement.value_or_default(); // need to have a value before combo

    if (state.activeFgInput != FGInput::ForceXeLL)
    {
        if (MenuText::BeginTable("fgSelection", 2, ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextColumn();

            PopulateCombo("插帧输入", config->FGInput, inputOptions);
            ShowTooltip("用于插帧的数据来源，即游戏原生支持的插帧接口。");

            ImGui::TableNextColumn();

            if (replaceFgOutputWithNvngx)
            {
                // Disable None?
                if (!TryPopulateComboGroup("NGX 插帧后端", config->FGNvngxReplacement, nvngxOptions))
                    PopulateCombo("NGX 插帧后端", config->FGNvngxReplacement, nvngxOptions);
                ShowTooltip("替代原生 DLSS-G 的后端。");
            }
            else
            {
                // FGOutput always offers exactly four visible choices; present them as a single-select group
                // and keep each option's hardware/API availability as a disabled state on its button.
                const char* outputNames[4];
                bool outputEnabled[4];
                int outputChoice = 0;
                const auto storedOutput = config->FGOutput.value_or(outputOptions[0].value);
                for (int i = 0; i < 4; ++i)
                {
                    outputNames[i] = outputOptions[i].label.c_str();
                    outputEnabled[i] = !outputOptions[i].disabled;
                    if (outputOptions[i].value == storedOutput)
                        outputChoice = i;
                }

                if (MenuText::Segmented("插帧输出", &outputChoice, outputNames, 4, outputEnabled))
                    config->FGOutput = outputOptions[outputChoice].value;
                ShowTooltip("实际使用的插帧方案；不可用的后端会以自己的禁用状态显示。");
            }

            ImGui::EndTable();
        }

        // Should be on a new line
        if (showNvngxFgDowndown)
        {
            if (!TryPopulateComboGroup("NGX 插帧替代模块", config->FGNvngxReplacement, nvngxOptions))
                PopulateCombo("NGX 插帧替代模块", config->FGNvngxReplacement, nvngxOptions);
            ShowTooltip("替代原生 DLSS-G 的后端。");
        }

        // Try to avoid having None selected when the gpu doesn't support DLSSG + some fallbacks
        if (!supportsDlssg && (replaceFgOutputWithNvngx || showNvngxFgDowndown) &&
            config->FGNvngxReplacement.value_or_default() == FGNvngxReplacement::None)
        {
            if (state.nukemsFgFileAvailable)
                config->FGNvngxReplacement.set_volatile_value(FGNvngxReplacement::Nukems);

            else if (state.artursFgFileAvailable)
                config->FGNvngxReplacement.set_volatile_value(FGNvngxReplacement::Arturs);

            else if (FfxApiProxy::IsFGReady(false))
                config->FGNvngxReplacement.set_volatile_value(FGNvngxReplacement::FFX);
        }

        const bool nvngxFgChanged = (replaceFgOutputWithNvngx || showNvngxFgDowndown) &&
                                    state.activeFgNvngx != config->FGNvngxReplacement.value_or_default();
        state.fgSettingsChanged = state.activeFgOutput != config->FGOutput.value_or_default() ||
                                  state.activeFgInput != config->FGInput.value_or_default() || nvngxFgChanged;

        if (state.fgSettingsChanged)
        {
            ImGui::Spacing();
            MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.0f, 1.f)),
                               "保存设置并重启后应用更改。");
            ImGui::Spacing();
        }

        const bool dlssgInputOrOutput =
            state.activeFgOutput == FGOutput::DLSSG || state.activeFgInput == FGInput::DLSSG;

        ImGui::BeginDisabled(state.dlssgGameDMFGSupported && config->FGDLSSGOverrideForceDMFG.value_or_default());
        if (state.dlssgMfgMax.has_value() && state.dlssgMfgMax.value() >= 1 && !dlssgInputOrOutput)
        {
            auto maxInterpolationCount = state.dlssgMfgMax.value();

            if (maxInterpolationCount >= 1)
            {
                // 0 follows the game, 1 disables FG; otherwise total = generated + 1.
                int totalMultiplier = config->FGDLSSGOverrideInterpolationCount.has_value()
                    ? config->FGDLSSGOverrideInterpolationCount.value() + 1 : 0;
                const char* multiplierFormat = totalMultiplier == 0 ? "默认 (0)"
                    : totalMultiplier == 1 ? "关闭 (1X)" : "%dX";

                ImGui::PushItemWidth(95.0f * menuResScale);

                if (MenuText::SliderInt("覆盖 DLSS-G 总倍率", &totalMultiplier, 0, maxInterpolationCount + 1,
                                        multiplierFormat, ImGuiSliderFlags_AlwaysClamp))
                {
                    if (totalMultiplier == 0)
                    {
                        config->FGDLSSGOverrideInterpolationCount.reset();
                    }
                    else
                    {
                        const int framesToGenerate = totalMultiplier - 1;
                        LOG_DEBUG("DLSSG Interpolation Count set to: {}", framesToGenerate);
                        config->FGDLSSGOverrideInterpolationCount = framesToGenerate;
                    }

                    StreamlineHooks::updateDlssgOptions();
                }

                ImGui::PopItemWidth();
                ShowHelpMarker("0 为默认，1 为关闭；其余为总倍率，生成帧数＝总倍率－1。");
            }
        }

        ImGui::EndDisabled();

        if (state.dlssgMfgMax.has_value() && state.dlssgMfgMax.value() >= 1 && !dlssgInputOrOutput)
            RenderDlssgTelemetry();

        if (state.dlssgGameDMFGSupported && !dlssgInputOrOutput)
        {
            ImGui::SameLine(0.0f, 16.0f);

            if (bool dynamicMFG = config->FGDLSSGOverrideForceDMFG.value_or_default();
                MenuText::Checkbox("强制动态多倍插帧", &dynamicMFG))
            {
                config->FGDLSSGOverrideForceDMFG = dynamicMFG;
                StreamlineHooks::updateDlssgOptions();
            }

            ImGui::BeginDisabled(state.dlssgLastSetMode != sl::DLSSGMode::eDynamic);
            static float fpsTarget = config->FGDLSSGFramerateTargetDMFG.value_or_default();
            MenuText::SliderFloat("动态多倍插帧目标帧率", &fpsTarget, 0, 200, "%.0f");

            ShowHelpMarker("生效上限为 0 时自动检测显示器刷新率。");

            if (MenuText::Button("应用目标"))
            {
                config->FGDLSSGFramerateTargetDMFG = fpsTarget;
                StreamlineHooks::updateDlssgOptions();
            }

            ImGui::SameLine(0.0f, 16.0f);

            if (MenuText::Button("重置目标"))
            {
                fpsTarget = 0.0f;
                config->FGDLSSGFramerateTargetDMFG.reset();
            }

            ImGui::EndDisabled();
        }

        const bool showOutputSpecificFGSettings = state.activeFgInput == FGInput::DLSSG ||
                                                  state.activeFgInput == FGInput::FSRFG ||
                                                  state.activeFgInput == FGInput::FSRFG30;

        const bool showHudCutoff = state.activeFgInput == FGInput::NvngxFG || state.activeFgOutput == FGOutput::FSRFG;

        if (showOutputSpecificFGSettings || showHudCutoff)
        {
            ImGui::Spacing();

            if (auto ch = ScopedCollapsingHeader("高级插帧设置"); ch.IsHeaderOpen())
            {
                ScopedIndent indent {};
                ImGui::Spacing();

                if (showOutputSpecificFGSettings)
                {
                    auto fgOutput = reinterpret_cast<IFGFeature_Dx12*>(state.currentFG);
                    if (fgOutput)
                    {
                        ImGui::BeginDisabled(!fgOutput->IsActive());

                        const auto isUsingUIAny = fgOutput->IsUsingUIAny();
                        const auto isUsingHudlessAny = fgOutput->IsUsingHudlessAny();

                        bool disableUI = config->FGDisableUI.value_or_default();
                        ImGui::BeginDisabled(!isUsingUIAny && !disableUI);

                        if (MenuText::CheckboxInline("禁用界面纹理", &disableUI))
                        {
                            config->FGDisableUI = disableUI;
                            fgOutput->UpdateTarget();
                        }

                        ShowHelpMarker("忽略游戏提供的界面纹理。");

                        ImGui::EndDisabled();

                        ImGui::SameLine(0.0f, 16.0f);

                        bool disableHudless = config->FGDisableHudless.value_or_default();
                        ImGui::BeginDisabled(!isUsingHudlessAny && !disableHudless);

                        if (MenuText::Checkbox("禁用去 HUD 输入", &disableHudless))
                        {
                            config->FGDisableHudless = disableHudless;
                        }

                        ShowHelpMarker("忽略游戏提供的去 HUD 输入。");

                        ImGui::EndDisabled();

                        bool depthValidNow = config->FGDepthValidNow.value_or_default();
                        if (MenuText::CheckboxInline("深度使用 ValidNow", &depthValidNow))
                            config->FGDepthValidNow = depthValidNow;

                        ShowHelpMarker("会增加显存占用，Uniscaler 需要此设置，其他游戏也可能需要。");

                        ImGui::SameLine(0.0f, 16.0f);

                        bool velocityValidNow = config->FGVelocityValidNow.value_or_default();
                        if (MenuText::Checkbox("运动矢量使用 ValidNow", &velocityValidNow))
                            config->FGVelocityValidNow = velocityValidNow;

                        ShowHelpMarker("会增加显存占用，Uniscaler 需要此设置，其他游戏也可能需要。");

                        bool hudlessValidNow = config->FGHudlessValidNow.value_or_default();
                        if (MenuText::CheckboxInline("去 HUD 输入使用 ValidNow", &hudlessValidNow))
                            config->FGHudlessValidNow = hudlessValidNow;

                        ShowHelpMarker("会增加显存占用，部分游戏可能需要。");

                        ImGui::SameLine(0.0f, 16.0f);

                        bool firstHudless = config->FGOnlyAcceptFirstHudless.value_or_default();
                        if (MenuText::Checkbox("接受首个去 HUD 输入", &firstHudless))
                            config->FGOnlyAcceptFirstHudless = firstHudless;

                        ShowHelpMarker("源端标记多个去 HUD 资源时，仅使用第一个。");

                        if (bool skipReset = config->FGSkipReset.value_or_default();
                            MenuText::Checkbox("忽略重置信号", &skipReset))
                        {
                            config->FGSkipReset = skipReset;
                        }

                        ShowHelpMarker("忽略插帧输入的重置信号。");

                        ImGui::EndDisabled();

                        ImGui::PushItemWidth(80.0f * menuResScale);

                        auto frameAhead = config->FGAllowedFrameAhead.value_or_default();
                        if (MenuText::InputInt("允许提前帧数", &frameAhead, 1, 1) && frameAhead > 0 && frameAhead < 4)
                        {
                            config->FGAllowedFrameAhead = frameAhead;
                        }

                        ShowHelpMarker("插帧允许领先游戏的帧数。可能减少插帧反复开关，也可能产生问题。");

                        ImGui::PopItemWidth();

                        ImGui::SameLine(0.0f, 16.0f);

                        const char* ftSources[] = { "输入来源", "OptiScaler", "零值" };
                        const char* ftSourceInfos[] = { "使用 DLSS-G 或 FSR 插帧提供的帧耗时。",
                                                        "使用 OptiScaler 计算的帧耗时。",
                                                        "让 XeFG 自行处理帧耗时。" };

                        // Two sources are always offered; XeFG adds its own third. The list never
                        // exceeds three, so a single-select group fits and keeps each source's note.
                        const int currentSourceCount = state.activeFgOutput == FGOutput::XeFG ? 3 : 2;
                        int previousSet = (int) config->FTInput.value_or_default();
                        previousSet = std::clamp(previousSet, 0, IM_ARRAYSIZE(ftSources) - 1);
                        int currentSet = previousSet;

                        if (MenuText::Segmented("帧耗时输入来源", &currentSet, ftSources, currentSourceCount,
                                                nullptr, nullptr, ftSourceInfos))
                        {
                            LOG_DEBUG("FTInput has changed {} -> {}", ftSources[previousSet], ftSources[currentSet]);
                            config->FTInput = (FrameTimeSource) currentSet;
                        }

                        ShowHelpMarker("选择帧耗时来源，可能改善帧调度和卡顿。");
                    }
                }

                if (showHudCutoff)
                {
                    float fgHudCutoff = config->FGHudCutoff.value_or_default();
                    if (MenuText::SliderFloat("界面透明度截断", &fgHudCutoff, 0.00f, 1.0f, "%.2f"))
                        config->FGHudCutoff = fgHudCutoff;

                    ShowHelpMarker("截断界面透明度以辅助插值，可用界面检测视图观察差异。0 为自动。");
                }
            }
        }
    }
}

void MenuCommon::RenderFrameGenerationAdvancedSettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    if (state.externalFrameGeneration || state.activeFgInput == FGInput::ForceXeLL)
        return;
    auto fgOutput = reinterpret_cast<IFGFeature_Dx12*>(state.currentFG);
    if (((state.activeFgOutput == FGOutput::FSRFG || state.activeFgOutput == FGOutput::XeFG ||
          state.activeFgOutput == FGOutput::DLSSG) &&
         state.activeFgInput != FGInput::NoFG && state.activeFgInput != FGInput::NvngxFG) &&
        fgOutput)
    {
        MenuText::Checkbox("显示检测到的界面", &state.fgHudlessCompare);
        ShowHelpMarker("需要去除 HUD 的纹理与最终画面对比；只有界面元素应显示粉红色。");

        const auto isUsingUIAny = fgOutput->IsUsingUIAny();

        ImGui::BeginDisabled(!isUsingUIAny);

        if (bool drawUIOverFG = config->FGDrawUIOverFG.value_or_default();
            MenuText::CheckboxInline("叠加界面纹理", &drawUIOverFG))
        {
            config->FGDrawUIOverFG = drawUIOverFG;
        }
        ShowHelpMarker("将界面资源绘制到最终画面上；界面缺失时可启用。");

        ImGui::EndDisabled();

        ImGui::SameLine(0.0f, 16.0f);

        ImGui::BeginDisabled(!isUsingUIAny || !config->FGDrawUIOverFG.value_or_default());

        if (bool uiPremultipliedAlpha = config->FGUIPremultipliedAlpha.value_or_default();
            MenuText::Checkbox("界面预乘 alpha", &uiPremultipliedAlpha))
        {
            config->FGUIPremultipliedAlpha = uiPremultipliedAlpha;
        }
        ShowHelpMarker("界面过淡时可关闭。");

        ImGui::EndDisabled();
    }
}

void MenuCommon::RenderFrameGenerationRuntimeSettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& currentFeature = ctx.currentFeature;
    auto& menuResScale = ctx.menuResScale;
    auto& primaryGpu = *ctx.primaryGpu;
    auto fgOutput = state.currentFG;

    const UiTargetMode uiTargetMode = getUiTargetMode();
    const bool outputIsHdr = uiTargetMode != UiTargetMode::SDR;

    // Keep the Genshin multiplier directly below its enable request. Readiness
    // selects the original capability source; editing still changes only the
    // saved request and cannot admit FG work or claim measured output.
    static const bool genshinProcess = GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring());
    const bool genshinDlssgRequested = genshinProcess && !state.externalFrameGeneration &&
        ((state.activeFgInput == FGInput::Upscaler && state.activeFgOutput == FGOutput::DLSSG) ||
         (config->FGInput.value_or_default() == FGInput::Upscaler && config->FGOutput.value_or_default() == FGOutput::DLSSG));
    const bool dlssgRuntimeControlsReady = state.activeFgOutput == FGOutput::DLSSG &&
        state.activeFgInput != FGInput::NoFG && state.currentFGSwapchain != nullptr &&
        StreamlineProxy::LoadStreamline() && fgOutput;
    // The user's request must remain editable after disable/teardown or a
    // temporary loss of authenticated guides. This does not admit any work.
    if (genshinDlssgRequested)
    {
        bool requested = config->FGEnabled.value_or_default();
        ImGui::BeginDisabled(state.api == Vulkan);
        if (MenuText::Checkbox("启用帧生成##genshinRequest", &requested))
        {
            config->FGEnabled = requested;
            LOG_DEBUG("Genshin FG request set FGEnabled: {}", requested);
            if (requested)
            {
                state.fgChanged = true;
                if (auto* provider = dynamic_cast<DLSSG_Dx12*>(state.currentFG))
                    provider->RequestGenshinResume();
            }
        }
        ImGui::EndDisabled();
        ShowHelpMarker("请求启用帧生成；实际插帧仍需当前画面的深度、运动与呈现链路就绪。");
        const int maxInterpolationCount = dlssgRuntimeControlsReady
            ? fgOutput->GetMaxInterpolationCount()
            : (std::max)(static_cast<int>(MfgUnlock::UnlockedMax()), state.dlssgMfgMax.value_or(0));
        const int savedFrames = config->FGDLSSGInterpolationCount.value_or_default();
        const bool hasMultiplierRange = maxInterpolationCount > 1;
        const bool dynamicMFG = config->FGDLSSGForceDMFG.value_or_default();
        const bool canEditMultiplier = hasMultiplierRange && state.api != Vulkan && !dynamicMFG;
        int totalMultiplier = savedFrames + 1;
        // With unknown capability the disabled slider has no synthetic range.
        const int minimum = hasMultiplierRange ? 2 : totalMultiplier;
        const int maximum = hasMultiplierRange ? maxInterpolationCount + 1 : totalMultiplier;
        ImGui::BeginDisabled(!canEditMultiplier);
        ImGui::PushItemWidth(95.0f * menuResScale);
        if (MenuText::SliderInt(dlssgRuntimeControlsReady
                ? "帧生成倍数###总倍率##DLSSG"
                : "帧生成倍数###请求总倍率##genshinDlssg", &totalMultiplier, minimum, maximum, "%dX",
                                ImGuiSliderFlags_AlwaysClamp) && canEditMultiplier)
        {
            const int framesToGenerate = totalMultiplier - 1;
            config->FGDLSSGInterpolationCount = framesToGenerate;
            LOG_DEBUG("Genshin DLSSG requested interpolation count set to: {}", framesToGenerate);
        }
        ImGui::PopItemWidth();
        ImGui::EndDisabled();
        ShowHelpMarker("设置 DLSS-G 保存请求；不代表实际插帧。生成帧数＝总倍率－1。");
        if (state.api == Vulkan)
            MenuText::TextDisabled("Vulkan 暂不支持此帧生成路径。");
        else if (!hasMultiplierRange)
            MenuText::TextDisabled("尚未确认可调倍率范围；保留已保存请求。");
        else if (dynamicMFG)
            MenuText::TextDisabled("动态多倍插帧已启用，手动倍率不可修改。");
        else
            MenuText::TextDisabled("仅修改保存请求；实际插帧仍需输出链路就绪。");
        MenuText::TextWrapped(requested
            ? "帧生成请求：已启用。实际插帧与生成帧数尚未确认。"
            : "帧生成请求：已关闭。");
    }
    if (genshinDlssgRequested && !dlssgRuntimeControlsReady)
    {
        MenuText::SeparatorText("帧生成（DLSS-G）");
        MenuText::TextWrapped("当前原神 FG 输出尚未就绪，正在保留原生画面。");
        const auto savedFrames = config->FGDLSSGInterpolationCount.value_or_default();
        const std::string savedMultiplier = savedFrames > 0
            ? std::to_string(static_cast<int64_t>(savedFrames) + 1) + "X"
            : MenuText::Translate("默认");
        MenuText::TextWrapped("已保存请求：%s；不代表实际插帧。", savedMultiplier.c_str());
    }

    // FSR FG controls
    if (state.activeFgOutput == FGOutput::FSRFG && state.activeFgInput != FGInput::NoFG &&
        state.currentFGSwapchain != nullptr)
    {
        if (state.activeFgInput != FGInput::Upscaler ||
            (currentFeature != nullptr && !currentFeature->IsFrozen()) && FfxApiProxy::IsFGReady())
        {
            MenuText::SeparatorText("帧生成（FSR）");

            if (_ffxFGIndex < 0)
                _ffxFGIndex = config->FfxFGIndex.value_or_default();

            if (state.ffxFGVersionNames.size() > 0)
            {
                const int ffxFgCount = (int) state.ffxFGVersionIds.size();
                if (ffxFgCount > 0 && ffxFgCount < 5)
                {
                    // Short SDK list: fold into a group; the pending choice still commits via 切换插帧.
                    std::vector<std::string> labels;
                    std::vector<const char*> labelPointers;
                    labels.reserve((size_t) ffxFgCount);
                    for (int n = 0; n < ffxFgCount; ++n)
                        labels.push_back(StrFmt("FSR %s", state.ffxFGVersionNames[n]));
                    for (const auto& label : labels)
                        labelPointers.push_back(label.c_str());

                    int choice = _ffxFGIndex;
                    if (MenuText::Segmented("FFX 插帧", &choice, labelPointers.data(), ffxFgCount))
                        _ffxFGIndex = choice;
                }
                else
                {
                    ImGui::PushItemWidth(135.0f * menuResScale);

                    auto currentName = StrFmt("FSR %s", state.ffxFGVersionNames[_ffxFGIndex]);
                    if (MenuText::BeginCombo("FFX 插帧", currentName.c_str()))
                    {
                        for (int n = 0; n < state.ffxFGVersionIds.size(); n++)
                        {
                            auto name = StrFmt("FSR %s", state.ffxFGVersionNames[n]);
                            if (MenuText::Selectable(name.c_str(), config->FfxFGIndex.value_or_default() == n))
                                _ffxFGIndex = n;
                        }

                        ImGui::EndCombo();
                    }
                    ImGui::PopItemWidth();
                }

                ShowHelpMarker("FFX SDK 报告的插帧后端列表");

                ImGui::SameLine(0.0f, 6.0f);

                if (MenuText::Button("切换插帧") && _ffxFGIndex != config->FfxFGIndex.value_or_default())
                {
                    config->FfxFGIndex = _ffxFGIndex;
                    state.fgChanged = true;
                    state.scChanged = true;
                }
            }

            bool fgActive = config->FGEnabled.value_or_default();
            if (MenuText::Checkbox("启用##2", &fgActive))
            {
                config->FGEnabled = fgActive;
                LOG_DEBUG("FGEnabled set FGEnabled: {}", fgActive);

                if (config->FGEnabled.value_or_default())
                    state.fgChanged = true;
            }
            ShowHelpMarker("启用帧生成");

            bool fgAsync = config->FGAsync.value_or_default();
            if (MenuText::Checkbox("允许异步", &fgAsync))
            {
                config->FGAsync = fgAsync;

                if (config->FGEnabled.value_or_default())
                {
                    state.fgChanged = true;
                    state.scChanged = true;
                    LOG_DEBUG("Async set FGChanged");
                }
            }
            ShowHelpMarker("异步可能改善插帧性能，但可能崩溃，特别是同时启用 HUD 修正时。");

            ImGui::SameLine(0.0f, 16.0f);

            bool fgDV = config->FGDebugView.value_or_default();
            if (MenuText::Checkbox("调试视图##2", &fgDV))
            {
                config->FGDebugView = fgDV;

                if (config->FGEnabled.value_or_default())
                {
                    state.fgChanged = true;
                    LOG_DEBUG("DebugView set FGChanged");
                }
            }
            ShowHelpMarker("FSR 3.1 插帧调试视图：上方为游戏运动矢量、深度、光流运动矢量；中间为生成帧；下方为遮挡解除遮罩、无界面插值源和去 HUD 资源。");

            ImGui::SameLine(0.0f, 16.0f);

            if (state.currentFG && state.currentFG->Version().major > 3)
            {
                if (bool fgwm = config->FSRFGEnableWatermark.value_or_default();
                    MenuText::Checkbox("启用水印", &fgwm))
                {
                    LOG_DEBUG("FSRFGEnableWatermark set FGWatermark: {}", fgwm);
                    config->FSRFGEnableWatermark = fgwm;
                }

                ShowHelpMarker("更改后请保存设置，下次启动生效。");
            }

            ImGui::Spacing();

            if (auto ch = ScopedCollapsingHeader("扩展 FSR 插帧设置"); ch.IsHeaderOpen())
            {
                ScopedIndent indent {};
                ImGui::Spacing();

                MenuText::CheckboxInline("仅显示生成帧", &state.fgOnlyGenerated);
                ShowHelpMarker("仅显示 FSR 3.1 生成的帧。");

                ImGui::SameLine(0.0f, 16.0f);
                auto debugResetLines = config->FGDebugResetLines.value_or_default();
                if (MenuText::Checkbox("调试重置线", &debugResetLines))
                {
                    config->FGDebugResetLines = debugResetLines;
                    LOG_DEBUG("Enabled set FGDebugLines: {}", debugResetLines);
                }
                ShowHelpMarker("绘制插值跳过线。");

                auto debugTearLines = config->FGDebugTearLines.value_or_default();
                if (MenuText::CheckboxInline("调试撕裂线", &debugTearLines))
                {
                    config->FGDebugTearLines = debugTearLines;
                    LOG_DEBUG("Enabled set FGDebugLines: {}", debugTearLines);
                }
                ShowHelpMarker("绘制撕裂与插值跳过线。");

                ImGui::SameLine(0.0f, 16.0f);
                auto debugPacingLines = config->FGDebugPacingLines.value_or_default();
                if (MenuText::Checkbox("调试调度线", &debugPacingLines))
                {
                    config->FGDebugPacingLines = debugPacingLines;
                    LOG_DEBUG("Enabled set FGDebugLines: {}", debugPacingLines);
                }
                ShowHelpMarker("绘制帧调度线。");

                ImGui::Spacing();
                if (MenuText::TreeNode("插帧矩形设置"))
                {
                    ImGui::PushItemWidth(95.0f * menuResScale);
                    int rectLeft = config->FGRectLeft.value_or(0);
                    if (MenuText::InputInt("矩形左边", &rectLeft))
                        config->FGRectLeft = rectLeft;
                    ShowHelpMarker("调整插帧区域，适配带黑边的画面。");

                    ImGui::SameLine(0.0f, 16.0f);
                    int rectTop = config->FGRectTop.value_or(0);
                    if (MenuText::InputInt("矩形上边", &rectTop))
                        config->FGRectTop = rectTop;
                    ShowHelpMarker("调整插帧区域，适配带黑边的画面。");

                    int rectWidth = config->FGRectWidth.value_or(0);
                    if (MenuText::InputInt("矩形宽度", &rectWidth))
                        config->FGRectWidth = rectWidth;
                    ShowHelpMarker("调整插帧区域，适配带黑边的画面。");

                    ImGui::SameLine(0.0f, 16.0f);
                    int rectHeight = config->FGRectHeight.value_or(0);
                    if (MenuText::InputInt("矩形高度", &rectHeight))
                        config->FGRectHeight = rectHeight;
                    ShowHelpMarker("调整插帧区域，适配带黑边的画面。");

                    ImGui::PopItemWidth();

                    ImGui::BeginDisabled(!config->FGRectLeft.has_value() && !config->FGRectTop.has_value() &&
                                         !config->FGRectWidth.has_value() && !config->FGRectHeight.has_value());

                    if (MenuText::Button("重置插帧矩形"))
                    {
                        config->FGRectLeft.reset();
                        config->FGRectTop.reset();
                        config->FGRectWidth.reset();
                        config->FGRectHeight.reset();
                    }

                    ShowHelpMarker("恢复插帧矩形默认值。");

                    ImGui::EndDisabled();
                    ImGui::TreePop();
                }

                auto fg = state.currentFG;
                if (fg != nullptr && strcmp(fg->Name(), "FSR-FG") == 0 &&
                    FfxApiProxy::VersionDx12_FG() >= feature_version { 3, 1, 3 })
                {
                    ImGui::Spacing();

                    if (MenuText::TreeNode("帧调度调优"))
                    {
                        auto fptEnabled = config->FGFramePacingTuning.value_or_default();
                        if (MenuText::Checkbox("启用调优", &fptEnabled))
                        {
                            config->FGFramePacingTuning = fptEnabled;
                            state.fsrfgFramePaceTuningChanged = true;
                        }

                        ImGui::BeginDisabled(!config->FGFramePacingTuning.value_or_default());

                        ImGui::PushItemWidth(115.0f * menuResScale);
                        auto fptSafetyMargin = config->FGFPTSafetyMarginInMs.value_or_default();
                        if (MenuText::InputFloat("安全余量（ms）", &fptSafetyMargin, 0.01f, 0.1f, "%.2f"))
                            config->FGFPTSafetyMarginInMs = fptSafetyMargin;
                        ShowHelpMarker("安全余量，单位毫秒。FSR 默认 0.1ms，OptiScaler 默认 0.01ms。");

                        auto fptVarianceFactor = config->FGFPTVarianceFactor.value_or_default();
                        if (MenuText::SliderFloat("方差系数", &fptVarianceFactor, 0.0f, 1.0f, "%.2f"))
                            config->FGFPTVarianceFactor = fptVarianceFactor;
                        ShowHelpMarker("FSR 默认 0.1，OptiScaler 默认 0.3。");
                        ImGui::PopItemWidth();

                        auto fpHybridSpin = config->FGFPTAllowHybridSpin.value_or_default();
                        if (MenuText::Checkbox("启用混合自旋", &fpHybridSpin))
                            config->FGFPTAllowHybridSpin = fpHybridSpin;
                        ShowHelpMarker("允许帧调度自旋锁休眠以降低 CPU 占用，帧率提升可能更慢。");

                        ImGui::PushItemWidth(115.0f * menuResScale);
                        auto fptHybridSpinTime = config->FGFPTHybridSpinTime.value_or_default();
                        if (MenuText::SliderInt("混合自旋时间", &fptHybridSpinTime, 0, 100))
                            config->FGFPTHybridSpinTime = fptHybridSpinTime;
                        ShowHelpMarker("混合自旋启用时的自旋时长，以计时器分辨率单位计算。不建议低于 2，否则容易超时。");
                        ImGui::PopItemWidth();

                        auto fpWaitForSingleObjectOnFence =
                            config->FGFPTAllowWaitForSingleObjectOnFence.value_or_default();
                        if (MenuText::Checkbox("使用 fence 事件等待", &fpWaitForSingleObjectOnFence))
                        {
                            config->FGFPTAllowWaitForSingleObjectOnFence = fpWaitForSingleObjectOnFence;
                        }
                        ShowHelpMarker("等待 fence 值时使用 WaitForSingleObject，而非持续自旋。");

                        if (MenuText::Button("应用计时更改"))
                            state.fsrfgFramePaceTuningChanged = true;

                        ImGui::EndDisabled();
                        ImGui::TreePop();
                    }
                }

                ImGui::Spacing();
                ImGui::Spacing();
            }
        }
    }

    // XeFG controls
    if (state.activeFgOutput == FGOutput::XeFG && state.activeFgInput != FGInput::NoFG &&
        state.activeFgInput != FGInput::ForceXeLL && state.currentFGSwapchain != nullptr && XeFGProxy::InitXeFG() &&
        fgOutput)
    {
        MenuText::SeparatorText("帧生成（XeFG）");

        bool ignoreChecks = config->FGXeFGIgnoreInitChecks.value_or_default();

        bool nativeAA = false;
        if (state.activeFgInput == FGInput::Upscaler && currentFeature != nullptr)
            nativeAA = currentFeature->RenderWidth() == currentFeature->DisplayWidth();

        const bool correctMVs = fgOutput->IsLowResMV() || nativeAA ||
                                (State::Instance().gameQuirks & GameQuirk::ForceFGRenderSizeMVs) || ignoreChecks;

        if (!correctMVs || state.realExclusiveFullscreen)
        {
            config->FGEnabled.reset();
            config->FGXeFGDebugView.reset();
        }

        const bool restartNeeded = config->FGXeFGDepthInverted.value_or_default() != fgOutput->IsInvertedDepth() ||
                                   config->FGXeFGJitteredMV.value_or_default() != fgOutput->IsJitteredMVs() ||
                                   config->FGXeFGHighResMV.value_or_default() == fgOutput->IsLowResMV();

        bool cantActivate = false;
        if (restartNeeded)
        {
            MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)),
                               "请重启游戏以应用正确的 XeFG 设置。");
        }
        else
        {
            if (!correctMVs)
                MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)),
                                   "需要关闭膨胀运动矢量。");

            if (!ignoreChecks && state.realExclusiveFullscreen)
            {
                cantActivate = true;
                MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)), "需要无边框显示模式。");
            }

            if (!ignoreChecks && outputIsHdr)
            {
                if (state.currentSwapchainDesc.BufferDesc.Format >= DXGI_FORMAT_R32G32B32A32_TYPELESS &&
                    state.currentSwapchainDesc.BufferDesc.Format <= DXGI_FORMAT_R16G16B16A16_SINT)
                {
                    cantActivate = true;
                    MenuText::TextColored(toneMapColor(ImVec4(1.0f, 0.0f, 0.0f, 1.f)), "XeFG 仅支持 HDR10。");
                }
            }
        }

        if (!correctMVs || cantActivate || ignoreChecks)
        {
            if (MenuText::Checkbox("忽略初始化检查", &ignoreChecks))
                config->FGXeFGIgnoreInitChecks = ignoreChecks;

            ShowHelpMarker("忽略 XeFG 前置检查。不要用它跳过虚幻引擎游戏的运动矢量尺寸警告，可能崩溃或降低画质。");
        }

        ImGui::BeginDisabled(!correctMVs || cantActivate);

        bool fgActive = config->FGEnabled.value_or_default();
        if (MenuText::Checkbox("启用##3", &fgActive))
        {
            config->FGEnabled = fgActive;
            LOG_DEBUG("Enabled set FGEnabled: {}", fgActive);

            if (config->FGEnabled.value_or_default())
                state.fgChanged = true;
        }

        ShowHelpMarker("启用帧生成");

        auto maxInterpolationCount = fgOutput->GetMaxInterpolationCount();

        if (maxInterpolationCount > 1)
        {
            // Keep the saved request visible even before the provider applies it.
            int totalMultiplier = config->FGXeFGInterpolationCount.value_or_default() + 1;

            ImGui::PushItemWidth(95.0f * menuResScale);

            if (MenuText::SliderInt("总倍率##XeFG", &totalMultiplier, 2, maxInterpolationCount + 1, "%dX",
                                    ImGuiSliderFlags_AlwaysClamp))
            {
                const int framesToGenerate = totalMultiplier - 1;
                LOG_DEBUG("XeFG Interpolation Count set to: {}", framesToGenerate);
                state.fgChanged = true;
                config->FGXeFGInterpolationCount = framesToGenerate;
            }

            ImGui::PopItemWidth();

            ShowHelpMarker("设置 XeFG 保存请求；不代表实际插帧。生成帧数＝总倍率－1。");
        }

        ImGui::SameLine(0.0f, 16.0f);
        ImGui::BeginDisabled(!fgOutput->IsUsingHudlessAny() || XeFGProxy::SetUiCompositionState() == nullptr);
        bool fgCompositeUI = config->FGXeFGUIComposition.value_or_default();
        if (MenuText::Checkbox("界面合成", &fgCompositeUI))
            config->FGXeFGUIComposition = fgCompositeUI;

        ShowHelpMarker("关闭 HUD／界面插值，恢复旧 XeFG 2 行为，可修正透明界面伪影。");
        ImGui::EndDisabled();

        bool fgDV = config->FGXeFGDebugView.value_or_default();
        if (MenuText::Checkbox("调试视图##2", &fgDV))
        {
            config->FGXeFGDebugView = fgDV;

            if (config->FGXeFGDebugView.value_or_default())
            {
                state.fgChanged = true;
                LOG_DEBUG("DebugView set FGChanged");
            }
        }
        ShowHelpMarker("启用 XeFG 调试视图。");

        ImGui::EndDisabled();

        ImGui::SameLine(0.0f, 16.0f);
        bool fgBorderless = config->FGXeFGForceBorderless.value_or_default();
        if (MenuText::Checkbox("强制无边框", &fgBorderless))
            config->FGXeFGForceBorderless = fgBorderless;

        ShowHelpMarker("强制无边框显示。建议游戏全屏分辨率与显示器一致；可能影响稳定性。需要重启游戏。");

        // Disable this for now
        // ImGui::SameLine(0.0f, 16.0f);
        // MenuText::Checkbox("仅显示生成帧##2", &state.fgOnlyGenerated);
        // ShowHelpMarker("仅显示 XeFG 生成帧。");

        ImGui::Spacing();
        if (auto ch = ScopedCollapsingHeader("扩展 XeFG 设置"); ch.IsHeaderOpen())
        {
            ImGui::Spacing();
            if (MenuText::TreeNode("矩形设置"))
            {
                ImGui::PushItemWidth(95.0f * menuResScale);
                int rectLeft = config->FGRectLeft.value_or(0);
                if (MenuText::InputInt("矩形左边##2", &rectLeft))
                    config->FGRectLeft = rectLeft;
                ShowHelpMarker("调整插帧区域，适配带黑边的画面。");

                ImGui::SameLine(0.0f, 16.0f);
                int rectTop = config->FGRectTop.value_or(0);
                if (MenuText::InputInt("矩形上边##2", &rectTop))
                    config->FGRectTop = rectTop;
                ShowHelpMarker("调整插帧区域，适配带黑边的画面。");

                int rectWidth = config->FGRectWidth.value_or(0);
                if (MenuText::InputInt("矩形宽度##2", &rectWidth))
                    config->FGRectWidth = rectWidth;
                ShowHelpMarker("调整插帧区域，适配带黑边的画面。");

                ImGui::SameLine(0.0f, 16.0f);
                int rectHeight = config->FGRectHeight.value_or(0);
                if (MenuText::InputInt("矩形高度##2", &rectHeight))
                    config->FGRectHeight = rectHeight;
                ShowHelpMarker("调整插帧区域，适配带黑边的画面。");

                ImGui::PopItemWidth();

                ImGui::BeginDisabled(!config->FGRectLeft.has_value() && !config->FGRectTop.has_value() &&
                                     !config->FGRectWidth.has_value() && !config->FGRectHeight.has_value());

                if (MenuText::Button("重置插帧矩形##2"))
                {
                    config->FGRectLeft.reset();
                    config->FGRectTop.reset();
                    config->FGRectWidth.reset();
                    config->FGRectHeight.reset();
                }

                ShowHelpMarker("恢复插帧矩形默认值##2");

                ImGui::EndDisabled();
                ImGui::TreePop();
            }

            ImGui::Spacing();
            ImGui::Spacing();
        }
    }

    // DLSSG controls
    if (dlssgRuntimeControlsReady)
    {
        MenuText::SeparatorText("帧生成（DLSS-G）");

        if (state.activeFgNvngx == FGNvngxReplacement::None && (state.hdrOutputActive && outputIsHdr))
        {
            if (state.currentSwapchainDesc.BufferDesc.Format >= DXGI_FORMAT_R32G32B32A32_TYPELESS &&
                state.currentSwapchainDesc.BufferDesc.Format <= DXGI_FORMAT_R16G16B16A16_SINT)
            {
                MenuText::TextColored(toneMapColor(ImVec4(1.0f, 0.0f, 0.0f, 1.f)), "DLSS-G 仅支持 HDR10。");
            }
        }

        if (!genshinDlssgRequested)
        {
            MenuText::Text("当前 DLSS-G 状态：");
            ImGui::SameLine();
            if (auto count = DlssgInterpolationCount(state); count > 0)
            {
                MenuText::TextColored(toneMapColor(ImVec4(0.f, 1.f, 0.25f, 1.f)), "开启 %dx", count + 1);
            }
            else
            {
                MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)), "关闭");
            }

            bool fgActive = config->FGEnabled.value_or_default();
            if (MenuText::Checkbox("启用##4", &fgActive))
            {
                config->FGEnabled = fgActive;
                LOG_DEBUG("Enabled set FGEnabled: {}", fgActive);

                if (config->FGEnabled.value_or_default())
                    state.fgChanged = true;
            }

            ShowHelpMarker("启用帧生成");

        }

        auto maxInterpolationCount = fgOutput->GetMaxInterpolationCount();

        if (maxInterpolationCount > 1)
        {
            if (!genshinDlssgRequested)
            {
                ImGui::BeginDisabled(config->FGDLSSGForceDMFG.value_or_default());
                // Keep the saved request visible even before the provider applies it.
                int totalMultiplier = config->FGDLSSGInterpolationCount.value_or_default() + 1;

                ImGui::PushItemWidth(95.0f * menuResScale);

                if (MenuText::SliderInt("帧生成倍数###总倍率##DLSSG", &totalMultiplier, 2, maxInterpolationCount + 1, "%dX",
                                        ImGuiSliderFlags_AlwaysClamp))
                {
                    const int framesToGenerate = totalMultiplier - 1;
                    LOG_DEBUG("DLSSG Interpolation Count set to: {}", framesToGenerate);
                    config->FGDLSSGInterpolationCount = framesToGenerate;
                }

                ImGui::PopItemWidth();

                ShowHelpMarker("设置 DLSS-G 保存请求；不代表实际插帧。生成帧数＝总倍率－1。");

                ImGui::EndDisabled();
            }

            if (fgOutput->GetDMFGSupport())
            {
                ImGui::SameLine(0.0f, 16.0f);

                if (bool dynamicMFG = config->FGDLSSGForceDMFG.value_or_default();
                    MenuText::Checkbox("强制动态多倍插帧", &dynamicMFG))
                {
                    config->FGDLSSGForceDMFG = dynamicMFG;
                }

                ImGui::BeginDisabled(!config->FGDLSSGForceDMFG.value_or_default());
                static float fpsTarget = config->FGDLSSGFramerateTargetDMFG.value_or_default();
                MenuText::SliderFloat("动态多倍插帧目标帧率", &fpsTarget, 0, 200, "%.0f");

                ShowHelpMarker("生效上限为 0 时自动检测显示器刷新率。");

                if (MenuText::Button("应用目标"))
                {
                    config->FGDLSSGFramerateTargetDMFG = fpsTarget;
                }

                ImGui::SameLine(0.0f, 16.0f);

                if (MenuText::Button("重置目标"))
                {
                    fpsTarget = 0.0f;
                    config->FGDLSSGFramerateTargetDMFG.reset();
                }

                ImGui::EndDisabled();
            }
        }

        bool useGamesMarkers = config->FGDLSSGUseGamesReflexMarkers.value_or_default();
        ImGui::BeginDisabled(!ReflexHooks::gameIsSendingMarkers());
        if (MenuText::Checkbox("使用游戏 Reflex 标记", &useGamesMarkers))
        {
            config->FGDLSSGUseGamesReflexMarkers = useGamesMarkers;
            LOG_DEBUG("Changed set FGDLSSGUseGamesReflexMarkers: {}", useGamesMarkers);
        }
        ImGui::EndDisabled();
    }

    // OptiFG
    if (state.api != API::Vulkan && state.currentFGSwapchain != nullptr && state.activeFgInput == FGInput::Upscaler)
    {
        SeparatorWithHelpMarker("帧生成（OptiFG）", "使用超分数据进行插帧");

        if (currentFeature != nullptr && !currentFeature->IsFrozen() &&
            ((state.activeFgOutput == FGOutput::FSRFG && FfxApiProxy::IsFGReady()) ||
             (state.activeFgOutput == FGOutput::XeFG && XeFGProxy::Module() != nullptr) ||
             (state.activeFgOutput == FGOutput::DLSSG && StreamlineProxy::Module() != nullptr)))
        {
            if (!Config::Instance()->FGDisableHUDFix.value_or_default() &&
                state.swapchainInteropApi == SwapchainInteropApi::None)
            {
                bool fgHudfix = config->FGHUDFix.value_or_default();

                if (MenuText::Checkbox("HUD 修正", &fgHudfix))
                {
                    config->FGHUDFix = fgHudfix;
                    LOG_DEBUG("Enabled set FGHUDFix: {}", fgHudfix);
                    state.clearCapturedHudlesses = true;
                    state.fgChanged = true;
                }

                ShowHelpMarker("修正 HUD 稳定性，可能导致崩溃。");

                ImGui::BeginDisabled(!config->FGHUDFix.value_or_default());

                ImGui::SameLine(0.0f, 16.0f);
                ImGui::PushItemWidth(95.0f * menuResScale);
                int hudFixLimit = config->FGHUDLimit.value_or_default();
                if (MenuText::InputInt("上限", &hudFixLimit))
                {
                    if (hudFixLimit < 1)
                        hudFixLimit = 1;
                    else if (hudFixLimit > 999)
                        hudFixLimit = 999;

                    config->FGHUDLimit = hudFixLimit;
                    LOG_DEBUG("Enabled set FGHUDLimit: {}", hudFixLimit);
                }
                ShowHelpMarker("延后去 HUD 资源捕获，过高可能崩溃。");

                ImGui::SameLine(0.0f, 16.0f);
                if (MenuText::Button("分辨率##2"))
                    _showHudlessWindow = !_showHudlessWindow;

                ImGui::EndDisabled();

                auto hudExtended = config->FGHUDFixExtended.value_or_default();
                if (MenuText::CheckboxInline("扩展检查", &hudExtended))
                {
                    LOG_DEBUG("Enabled set FGHUDFixExtended: {}", hudExtended);
                    config->FGHUDFixExtended = hudExtended;
                }
                ShowHelpMarker("扩展去 HUD 资源格式检查，可能崩溃或降低性能。");
                ImGui::SameLine(0.0f, 16.0f);

                ImGui::BeginDisabled(!config->FGHUDFix.value_or_default());

                auto immediate = config->FGImmediateCapture.value_or_default();
                if (MenuText::Checkbox("立即捕获", &immediate))
                {
                    LOG_DEBUG("Enabled set FGImmediateCapture: {}", immediate);
                    config->FGImmediateCapture = immediate;
                }
                ShowHelpMarker("在着色器执行前捕获，提高找到去 HUD 资源的机会，也可能捕获无关资源。");

                ImGui::PopItemWidth();

                ImGui::EndDisabled();
            }

            bool depthScale = config->FGEnableDepthScale.value_or_default();
            if (MenuText::Checkbox("缩放深度以修正 DLSS 光线重建", &depthScale))
                config->FGEnableDepthScale = depthScale;
            ShowHelpMarker("修正 DLSS 光线重建的错误深度输入。");

            bool resourceFlip = config->FGResourceFlip.value_or_default();
            if (MenuText::CheckboxInline("翻转（Unity）", &resourceFlip))
                config->FGResourceFlip = resourceFlip;
            ShowHelpMarker("翻转 Unity 游戏的运动矢量和深度资源。");

            ImGui::SameLine(0.0f, 16.0f);

            bool resourceFlipOffset = config->FGResourceFlipOffset.value_or_default();
            if (MenuText::Checkbox("翻转时使用偏移", &resourceFlipOffset))
                config->FGResourceFlipOffset = resourceFlipOffset;
            ShowHelpMarker("用高度差作为偏移。");

            ImGui::Spacing();

            if (auto ch = ScopedCollapsingHeader("高级 OptiFG 设置"); ch.IsHeaderOpen())
            {
                ScopedIndent indent {};

                if (!Config::Instance()->FGDisableHUDFix.value_or_default() &&
                    state.swapchainInteropApi == SwapchainInteropApi::None)
                {
                    ImGui::Spacing();

                    auto rb = config->FGResourceBlocking.value_or_default();
                    if (MenuText::CheckboxInline("资源屏蔽", &rb))
                    {
                        config->FGResourceBlocking = rb;
                        LOG_DEBUG("Enabled set FGResourceBlocking: {}", rb);
                    }
                    ShowHelpMarker("屏蔽少用资源作为去 HUD 输入，减少闪烁等问题。切换 HUD 修正会重置屏蔽列表。");

                    ImGui::SameLine(0.0f, 16.0f);

                    auto rrc = config->FGRelaxedResolutionCheck.value_or_default();
                    if (MenuText::Checkbox("放宽资源检查", &rrc))
                    {
                        config->FGRelaxedResolutionCheck = rrc;
                        LOG_DEBUG("Enabled set FGRelaxedResolutionCheck: {}", rrc);
                    }
                    ShowHelpMarker("去 HUD 输入的分辨率检查放宽 32 像素，适合部分分辨率和比例下带黑边的游戏。");

                    ImGui::BeginDisabled(state.fgResetCapturedResources);
                    ImGui::PushItemWidth(95.0f * menuResScale);
                    if (MenuText::CheckboxInline("插帧创建列表", &state.fgCaptureResources))
                    {
                        if (!state.fgCaptureResources)
                            config->FGHUDLimit = 1;
                        else
                            state.fgOnlyUseCapturedResources = false;
                    }

                    ImGui::SameLine(0.0f, 16.0f);
                    if (MenuText::CheckboxInline("插帧使用列表", &state.fgOnlyUseCapturedResources))
                    {
                        if (state.fgCaptureResources)
                        {
                            state.fgCaptureResources = false;
                            config->FGHUDLimit = 1;
                        }
                    }

                    ImGui::SameLine(0.0f, 8.0f);
                    MenuText::Text("(%d)", state.fgCapturedResourceCount);

                    ImGui::PopItemWidth();

                    ImGui::SameLine(0.0f, 16.0f);

                    if (MenuText::Button("重置列表"))
                    {
                        LOG_DEBUG("Resetting captured resource list");

                        state.fgResetCapturedResources = true;
                        state.fgOnlyUseCapturedResources = false;
                    }

                    ImGui::EndDisabled();

                    ImGui::Spacing();
                    ImGui::Spacing();
                    if (MenuText::TreeNode("跟踪设置"))
                    {
                        auto ath = config->FGAlwaysTrackHeaps.value_or_default();
                        if (MenuText::Checkbox("始终跟踪描述符堆", &ath))
                        {
                            config->FGAlwaysTrackHeaps = ath;
                            LOG_DEBUG("Enabled set FGAlwaysTrackHeaps: {}", ath);
                        }
                        ShowHelpMarker("始终跟踪资源，可能降低性能，也可能修正 HUD 修正相关崩溃。");

                        auto disableRTV = config->FGHudfixDisableRTV.value_or_default();
                        if (MenuText::CheckboxInline("禁用 RTV 跟踪", &disableRTV))
                            config->FGHudfixDisableRTV = disableRTV;
                        ShowHelpMarker("不跟踪 CreateRenderTargetView，可能过滤错误去 HUD 资源。");

                        ImGui::SameLine(0.0f, 16.0f);

                        auto disableSRV = config->FGHudfixDisableSRV.value_or_default();
                        if (MenuText::Checkbox("禁用 SRV 跟踪", &disableSRV))
                            config->FGHudfixDisableSRV = disableSRV;
                        ShowHelpMarker("不跟踪 CreateShaderResourceView，可能过滤错误去 HUD 资源。");

                        auto disableUAV = config->FGHudfixDisableUAV.value_or_default();
                        if (MenuText::CheckboxInline("禁用 UAV 跟踪", &disableUAV))
                            config->FGHudfixDisableUAV = disableUAV;
                        ShowHelpMarker("不跟踪 CreateUnorderedAccessView，可能过滤错误去 HUD 资源。");

                        ImGui::SameLine(0.0f, 16.0f);

                        auto disableOM = config->FGHudfixDisableOM.value_or_default();
                        if (MenuText::Checkbox("禁用 OM 跟踪", &disableOM))
                            config->FGHudfixDisableOM = disableOM;
                        ShowHelpMarker("不跟踪 OMSetRenderTargets，可能过滤错误去 HUD 资源。");

                        auto disableSCR = config->FGHudfixDisableSCR.value_or_default();
                        if (MenuText::CheckboxInline("禁用 SCR 跟踪", &disableSCR))
                            config->FGHudfixDisableSCR = disableSCR;
                        ShowHelpMarker("不跟踪 SetComputeRootDescriptorTable，可能过滤错误去 HUD 资源。");

                        ImGui::SameLine(0.0f, 16.0f);

                        auto disableSGR = config->FGHudfixDisableSGR.value_or_default();
                        if (MenuText::Checkbox("禁用 SGR 跟踪", &disableSGR))
                            config->FGHudfixDisableSGR = disableSGR;
                        ShowHelpMarker("不跟踪 SetGraphicsRootDescriptorTable，可能过滤错误去 HUD 资源。");

                        ImGui::Spacing();

                        auto disableDI = config->FGHudfixDisableDI.value_or_default();
                        if (MenuText::Checkbox("禁用 DI 跟踪", &disableDI))
                            config->FGHudfixDisableDI = disableDI;
                        ShowHelpMarker("不跟踪 DrawInstanced，可能过滤错误去 HUD 资源。");

                        ImGui::SameLine(0.0f, 16.0f);

                        auto disableDII = config->FGHudfixDisableDII.value_or_default();
                        if (MenuText::Checkbox("禁用 DII 跟踪", &disableDII))
                            config->FGHudfixDisableDII = disableDII;
                        ShowHelpMarker("不跟踪 DrawIndexedInstanced，可能过滤错误去 HUD 资源。");

                        auto disableDispatch = config->FGHudfixDisableDispatch.value_or_default();
                        if (MenuText::Checkbox("禁用 Dispatch 跟踪", &disableDispatch))
                            config->FGHudfixDisableDispatch = disableDispatch;
                        ShowHelpMarker("不跟踪 Dispatch，可能过滤错误去 HUD 资源。");

                        ImGui::TreePop();
                    }
                }

                ImGui::Spacing();
                if (MenuText::TreeNode("资源设置"))
                {
                    bool makeMVCopies = config->FGMakeMVCopy.value_or_default();
                    if (MenuText::Checkbox("复制插帧运动矢量", &makeMVCopies))
                        config->FGMakeMVCopy = makeMVCopies;
                    ShowHelpMarker("为 OptiFG 复制运动矢量，避免可能的数据损坏。");

                    bool makeDepthCopies = config->FGMakeDepthCopy.value_or_default();
                    if (MenuText::Checkbox("复制插帧深度", &makeDepthCopies))
                        config->FGMakeDepthCopy = makeDepthCopies;
                    ShowHelpMarker("为 OptiFG 复制深度，避免可能的数据损坏。");

                    ImGui::PushItemWidth(115.0f * menuResScale);
                    float depthScaleMax = config->FGDepthScaleMax.value_or_default();
                    if (MenuText::InputFloat("插帧深度最大缩放值", &depthScaleMax, 10.0f, 100.0f, "%.1f"))
                        config->FGDepthScaleMax = depthScaleMax;
                    ShowHelpMarker("深度值除以此值。");
                    ImGui::PopItemWidth();

                    ImGui::TreePop();
                }

                ImGui::Spacing();
                if (MenuText::TreeNode("同步设置"))
                {
                    bool useMutexForPresent = config->FGUseMutexForSwapchain.value_or_default();
                    if (MenuText::Checkbox("呈现时使用互斥锁", &useMutexForPresent))
                        config->FGUseMutexForSwapchain = useMutexForPresent;
                    ShowHelpMarker("防止插帧失步与崩溃。关闭可能提高性能，但会降低稳定性。");

                    ImGui::TreePop();
                }

                ImGui::Spacing();
                ImGui::Spacing();
            }
        }
        else if (currentFeature == nullptr || currentFeature->IsFrozen())
        {
            MenuText::Text("超分未运行"); // Probably never will be visible
        }
        else if (state.activeFgOutput == FGOutput::FSRFG && !FfxApiProxy::IsFGReady())
        {
            MenuText::TextColored(toneMapColor({ 1.0f, 0.0f, 0.0f, 1.0f }),
                               "缺少 amd_fidelityfx_dx12.dll。"); // Probably never will be visible
        }
        else if (state.activeFgOutput == FGOutput::XeFG && XeFGProxy::Module() == nullptr)
        {
            MenuText::TextColored(toneMapColor({ 1.0f, 0.0f, 0.0f, 1.0f }),
                               "缺少 libxess_fg.dll。"); // Probably never will be visible
        }
    }

    const FGNvngxReplacement activeNvngxFg = state.activeFgNvngx;
    if (activeNvngxFg != FGNvngxReplacement::None)
    {
        if (activeNvngxFg == FGNvngxReplacement::Nukems)
        {
            SeparatorWithHelpMarker("帧生成（通过 Nukem DLSS-G 的 FSR3）",
                                    "需要 Nukem 的 dlssg_to_fsr3 模块。");

            if (!state.nukemsFgFileAvailable)
            {
                MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)),
                                   "请将 dlssg_to_fsr3_amd_is_better.dll 放入 OptiScaler 目录。");
            }
        }
        else if (activeNvngxFg == FGNvngxReplacement::Arturs)
        {
            SeparatorWithHelpMarker("帧生成（通过 DLSS Enabler 的 FSR3 多倍插帧）",
                                    "DLSS Enabler 模块：dlss-enabler-headless.dll");

            if (!state.artursFgFileAvailable)
            {
                MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)),
                                   "请将 dlss-enabler-headless.dll 放入 OptiScaler 目录。");
            }

            MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)),
                               "使用 DLSS Enabler 的部分功能。");
        }
        else if (activeNvngxFg == FGNvngxReplacement::FFX)
        {
            SeparatorWithHelpMarker("帧生成（通过 FFX 的 FSR）", "FFX 使用 DLSS-G 交换链。");
        }
        else if (activeNvngxFg == FGNvngxReplacement::Combo)
        {
            SeparatorWithHelpMarker("帧生成（Enabler＋FFX）",
                                    "中间生成帧用 FFX，其余用 Enabler。\n2X：FFX；3X：Enabler；4X：两者；5X：Enabler；6X：两者。");
        }

        if (state.activeFgInput == FGInput::NvngxFG)
        {

            bool dmfgActive = state.dlssgGameDMFGSupported && config->FGDLSSGOverrideForceDMFG.value_or_default();

            if (!ReflexHooks::isReflexHooked())
            {
                MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)), "Reflex 钩子未安装");
                MenuText::Text("AMD／Intel 显卡需安装 fakenvapi。");
            }
            else if (ReflexHooks::dlssgFrameCountToGenerate() == 0 && !dmfgActive)
            {
                MenuText::Text("请在游戏中选择 DLSS 帧生成，可能需要先选择 DLSS 超分。");
            }

            if (state.swapchainApi == DX12)
            {
                MenuText::Text("当前 DLSS-G 状态：");
                ImGui::SameLine();
                if (auto count = state.dlssgDetectedInterpolationCount; count > 0)
                {
                    MenuText::TextColored(toneMapColor(ImVec4(0.f, 1.f, 0.25f, 1.f)),
                                       std::format("ON {}x", count + 1).c_str());
                }
                else
                {
                    MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)), "关闭");
                }

                // Issue mostly shows up on AMD on Windows on pre-RDNA3 in some non-UE games
                // Hide to reduce confusion, config is still read
                const bool isUnrealEngine = State::Instance().NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL ||
                                            State::Instance().gameQuirks & GameQuirk::ForceUnrealEngine;
                const bool isDllProxyNvngxType =
                    activeNvngxFg == FGNvngxReplacement::Nukems || activeNvngxFg == FGNvngxReplacement::Arturs;
                if (isDllProxyNvngxType && !primaryGpu.dlssCapable && primaryGpu.fsr4Support == FSR4Support::None &&
                    !primaryGpu.usesVkd3dProton && !isUnrealEngine)
                {
                    if (bool makeDepthCopy = config->NvngxFGMakeDepthCopy.value_or_default();
                        MenuText::Checkbox("修正画面异常", &makeDepthCopy))
                    {
                        config->NvngxFGMakeDepthCopy = makeDepthCopy;
                    }
                    ShowHelpMarker("复制深度缓冲区，可修正 Windows 下部分 AMD 游戏画面异常，但可能卡顿，建议仅在需要时使用。");
                }
            }
            else if (state.swapchainApi == Vulkan)
            {
                MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)),
                                   "显示此菜单时会主动暂停 DLSS-G。");
                ImGui::Spacing();
            }
        }

        bool isLoaded = false;
        if (state.swapchainApi == Vulkan)
            isLoaded = Nvngx_FG::isVulkanAvailable();
        if (state.swapchainApi == DX12)
            isLoaded = Nvngx_FG::isDx12Available();

        if (isLoaded)
        {
            if (activeNvngxFg == FGNvngxReplacement::Arturs || activeNvngxFg == FGNvngxReplacement::Combo)
            {
                auto featureVer = Nvngx_FG::version();
                auto antighostingVer = Nvngx_FG::extraVersion();
                MenuText::Text("DE 版本：%d.%d.%d.%d   GB 版本：%d.%d", featureVer.major, featureVer.minor, featureVer.patch,
                            featureVer.reserved, antighostingVer.major, antighostingVer.minor);

                static std::vector<FlagDefinition> common_flags = {
                    { "减少拖影（GB）", 0x00100000, "启用拖影修正" },
                    { "时序 HUD 固定", 0x04000000, "固定时序 HUD，稳定呈现后备缓冲区。" }
                };

                static std::vector<FlagDefinition> uncommon_flags = {
                    //{ "Hudless UI mask", 0x02000000, "Use HUD-less as UI mask (DL2 inverted semantics)" },
                    { "HUD 插值", 0x08000000, "HUD 光流插值：0 为传统固定呈现，1 为光流变形。" },
                    { "忽略界面纹理", 0x10000000, "忽略专用 DLSSG.UI 纹理，强制传统 HUD 路径。" },
                    //{ "Dp4a active", 0x20000000, "OF pipeline using dp4a-accelerated SSD (SM 6.4+)" },
                    { "固定后备缓冲区", 0x40000000, "将 DLSSG.Backbuffer 固定为本次多倍插帧的首子帧快照。" }
                };

                static std::vector<FlagDefinition> debug_flags = {
                    { "拖影修正红色标记", 0x00200000, "调试：修正像素显示红色" },
                    { "拖影修正分屏", 0x00400000, "调试：分屏对比" },
                    { "帧索引线", 0x00010000, "" },
                    { "HUD 检测", 0x00020000, "" },
                    { "遮挡解除着色", 0x00040000, "" },
                    { "伪影检测", 0x00080000, "" },
                    { "相机运动矢量调试", 0x00800000, "调试：相机运动矢量回退区域显示蓝色" },
                    { "通用可视化", 0x01000000, "调试：梯形区域可视化" }
                };

                uint32_t temp_flags = config->NvngxFGDispatchFlags.value_or_default();
                bool changed = false;

                MenuText::Text("原始调度标志：");
                changed |= MenuText::InputScalar("##RawFlags", ImGuiDataType_U32, &temp_flags, NULL, NULL, "%08X",
                                              ImGuiInputTextFlags_CharsHexadecimal);

                ImGui::SameLine(0.0f, 20.0f * menuResScale);
                if (bool showDebug = config->NvngxFGShowDebug.value_or_default();
                    MenuText::Checkbox("显示调试", &showDebug))
                {
                    config->NvngxFGShowDebug = showDebug;
                }
                ShowHelpMarker("调试标志正常工作需要此选项。");

                ImGui::Spacing();

                if (auto ch = ScopedCollapsingHeader("当前调度标志"); ch.IsHeaderOpen())
                {
                    ScopedIndent indent {};

                    auto render_flags = [&](const std::vector<FlagDefinition>& flags)
                    {
                        for (const auto& flag : flags)
                        {
                            bool flagEnabled = (temp_flags & flag.mask) == flag.mask;
                            if (MenuText::Checkbox(flag.name.c_str(), &flagEnabled))
                            {
                                if (flagEnabled) temp_flags |= flag.mask;
                                else temp_flags &= ~flag.mask;
                                changed = true;
                            }

                            if (ImGui::IsItemHovered() && !flag.description.empty())
                            {
                                MenuText::SetTooltip("%s", flag.description.c_str());
                            }
                        }
                    };

                    MenuText::TextDisabled("常用");
                    render_flags(common_flags);

                    ImGui::Spacing();
                    MenuText::TextDisabled("较少使用");
                    render_flags(uncommon_flags);

                    if (config->NvngxFGShowDebug.value_or_default())
                    {
                        ImGui::Spacing();
                        MenuText::TextDisabled("调试");
                        render_flags(debug_flags);
                    }
                }

                if (changed)
                {
                    config->NvngxFGDispatchFlags = temp_flags;
                }
            }

            if (activeNvngxFg == FGNvngxReplacement::Nukems)
            {
                if (MenuText::Checkbox("启用调试视图", &state.dlssgDebugView))
                {
                    Nvngx_FG::setDebugView(state.dlssgDebugView);
                }
                if (MenuText::Checkbox("仅显示插值帧", &state.dlssgInterpolatedOnly))
                {
                    Nvngx_FG::setInterpolatedOnly(state.dlssgInterpolatedOnly);
                }
            }

            if (activeNvngxFg == FGNvngxReplacement::FFX || activeNvngxFg == FGNvngxReplacement::Combo)
            {
                if (_ffxFGIndex < 0)
                    _ffxFGIndex = config->FfxFGIndex.value_or_default();

                if (state.ffxFGVersionNames.size() > 0)
                {
                    const int ffxFgCount = (int) state.ffxFGVersionIds.size();
                    if (ffxFgCount > 0 && ffxFgCount < 5)
                    {
                        // Short SDK list: fold into a group; the pending choice still commits via 切换插帧.
                        std::vector<std::string> labels;
                        std::vector<const char*> labelPointers;
                        labels.reserve((size_t) ffxFgCount);
                        for (int n = 0; n < ffxFgCount; ++n)
                            labels.push_back(StrFmt("FSR %s", state.ffxFGVersionNames[n]));
                        for (const auto& label : labels)
                            labelPointers.push_back(label.c_str());

                        int choice = _ffxFGIndex;
                        if (MenuText::Segmented("FFX 插帧", &choice, labelPointers.data(), ffxFgCount))
                            _ffxFGIndex = choice;
                    }
                    else
                    {
                        ImGui::PushItemWidth(135.0f * menuResScale);

                        auto currentName = StrFmt("FSR %s", state.ffxFGVersionNames[_ffxFGIndex]);
                        if (MenuText::BeginCombo("FFX 插帧", currentName.c_str()))
                        {
                            for (int n = 0; n < state.ffxFGVersionIds.size(); n++)
                            {
                                auto name = StrFmt("FSR %s", state.ffxFGVersionNames[n]);
                                if (MenuText::Selectable(name.c_str(), config->FfxFGIndex.value_or_default() == n))
                                    _ffxFGIndex = n;
                            }

                            ImGui::EndCombo();
                        }
                        ImGui::PopItemWidth();
                    }

                    ShowHelpMarker("FFX SDK 报告的插帧后端列表");

                    ImGui::SameLine(0.0f, 6.0f);

                    if (MenuText::Button("切换插帧") && _ffxFGIndex != config->FfxFGIndex.value_or_default())
                    {
                        config->FfxFGIndex = _ffxFGIndex;
                        state.fgChanged = true;
                    }
                }

                bool fgAsync = config->FGAsync.value_or_default();
                if (MenuText::Checkbox("允许异步##2", &fgAsync))
                {
                    config->FGAsync = fgAsync;

                    if (config->FGEnabled.value_or_default())
                    {
                        state.fgChanged = true;
                        LOG_DEBUG("Async set FGChanged");
                    }
                }
                ShowHelpMarker("异步可能改善插帧性能，但可能崩溃，特别是同时启用 HUD 修正时。");

                ImGui::SameLine(0.0f, 20.0f * menuResScale);
                bool fgDV = config->FGDebugView.value_or_default();
                if (MenuText::Checkbox("调试视图##3", &fgDV))
                {
                    config->FGDebugView = fgDV;

                    if (config->FGEnabled.value_or_default())
                    {
                        state.fgChanged = true;
                        LOG_DEBUG("DebugView set FGChanged");
                    }
                }
                ShowHelpMarker("FSR 3.1 插帧调试视图：上方为游戏运动矢量、深度、光流运动矢量；中间为生成帧；下方为遮挡解除遮罩、无界面插值源和去 HUD 资源。");

                if (Nvngx_FG::version().major > 3)
                {
                    ImGui::SameLine(0.0f, 20.0f * menuResScale);
                    if (bool fgwm = config->FSRFGEnableWatermark.value_or_default();
                        MenuText::Checkbox("启用水印", &fgwm))
                    {
                        LOG_DEBUG("FSRFGEnableWatermark set FGWatermark: {}", fgwm);
                        config->FSRFGEnableWatermark = fgwm;
                    }

                    ShowHelpMarker("更改后请保存设置，下次启动生效。");
                }
            }

            if (bool disableHudless = config->NvngxFGDisableHudless.value_or_default();
                MenuText::Checkbox("禁用去 HUD 输入", &disableHudless))
            {
                config->NvngxFGDisableHudless = disableHudless;
            }
            ShowHelpMarker("部分调度标志组合可能需要此设置。");
        }
    }

    // FSR-FG Inputs
    if (state.currentFGSwapchain != nullptr &&
        (state.activeFgInput == FGInput::FSRFG || state.activeFgInput == FGInput::FSRFG30))
    {
        SeparatorWithHelpMarker("帧生成（FSR 插帧输入）", "请在游戏启用 FSR 插帧");

        auto fgOutput = reinterpret_cast<IFGFeature_Dx12*>(state.currentFG);
        if (fgOutput != nullptr)
        {
            MenuText::Text("当前 FSR 插帧状态：");
            ImGui::SameLine();
            if (state.fsrfgInputActive)
            {
                if (fgOutput->IsActive())
                    MenuText::TextColored(toneMapColor(ImVec4(0.f, 1.f, 0.25f, 1.f)), "ON");
                else
                    MenuText::TextColored(toneMapColor(ImVec4(1.0f, 0.647f, 0.0f, 1.f)), "启用插帧");
            }
            else
            {
                MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)), "关闭");
                MenuText::Text("请在游戏中选择 FSR 帧生成，可能需要先选择 FSR 超分。");
            }
        }

        bool skipConfig = config->FSRFGSkipConfigForHudless.value_or_default();
        if (MenuText::Checkbox("忽略配置中的去 HUD 输入", &skipConfig))
            config->FSRFGSkipConfigForHudless = skipConfig;

        ShowHelpMarker("不使用 ffxConfig 设置的去 HUD 输入。");

        ImGui::SameLine(0.0f, 6.0f);

        bool skipDispatch = config->FSRFGSkipDispatchForHudless.value_or_default();
        if (MenuText::Checkbox("忽略调度中的去 HUD 输入", &skipDispatch))
            config->FSRFGSkipDispatchForHudless = skipDispatch;

        ShowHelpMarker("不使用 ffxDispatch 设置的去 HUD 输入。");
    }

    // Streamline FG Inputs
    if (state.currentFGSwapchain != nullptr && state.activeFgInput == FGInput::DLSSG)
    {
        SeparatorWithHelpMarker("帧生成（Streamline 输入）", "请在游戏启用 DLSS 插帧");

        auto fgOutput = reinterpret_cast<IFGFeature_Dx12*>(state.currentFG);

        if (!ReflexHooks::isReflexHooked())
        {
            MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)), "Reflex 钩子未安装");
            MenuText::Text("AMD／Intel 显卡需安装 fakenvapi。");
        }
        else if (fgOutput != nullptr)
        {
            MenuText::Text("当前 Streamline 插帧状态：");
            ImGui::SameLine();
            if ((state.fgLastFrame - state.dlssgLastFrame) < 3)
            {
                if (fgOutput->IsActive())
                    MenuText::TextColored(toneMapColor(ImVec4(0.f, 1.f, 0.25f, 1.f)), "ON");
                else
                    MenuText::TextColored(toneMapColor(ImVec4(1.0f, 0.647f, 0.0f, 1.f)), "启用插帧");
            }
            else
            {
                MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)), "关闭");

                // A feature recreate, cache miss, or a skipped present can leave a few presents
                // without a matching DLSS-G input even though the game selected FG earlier. Keep
                // the actionable selection warning for the true never-selected state only.
                const bool dlssgWasSelected = state.dlssgLastSetMode != sl::DLSSGMode::eOff ||
                                              state.dlssgLastEvaluateFrame != UINT64_MAX;
                if (dlssgWasSelected)
                    MenuText::Text("等待游戏恢复 DLSS 插帧输入。");
                else
                    MenuText::Text("请在游戏中选择 DLSS 帧生成，可能需要先选择 DLSS 超分。");
            }
        }
    }
}

void MenuCommon::RenderFsrCommonSettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& currentFeature = ctx.currentFeature;

    if (currentFeature != nullptr && !currentFeature->IsFrozen())
    {
        // FSR Common -----------------
        if (currentFeature != nullptr && !currentFeature->IsFrozen() &&
            (state.activeFgOutput == FGOutput::FSRFG || IsFsr(currentBackend)))
        {
            SeparatorWithHelpMarker("FSR 通用设置", "同时影响 FSR 插帧和超分");

            bool useFsrVales = config->FsrUseFsrInputValues.value_or_default();
            if (MenuText::Checkbox("使用 FSR 输入数值", &useFsrVales))
                config->FsrUseFsrInputValues = useFsrVales;

            ImGui::Spacing();
            if (auto ch = ScopedCollapsingHeader("视野与相机参数"); ch.IsHeaderOpen())
            {
                ScopedIndent indent {};
                ImGui::Spacing();

                bool useVFov = config->FsrVerticalFov.has_value() || !config->FsrHorizontalFov.has_value();

                float vfov = config->FsrVerticalFov.value_or_default();
                float hfov = config->FsrHorizontalFov.value_or(90.0f);

                if (useVFov && !config->FsrVerticalFov.has_value())
                    config->FsrVerticalFov = vfov;
                else if (!useVFov && !config->FsrHorizontalFov.has_value())
                    config->FsrHorizontalFov = hfov;

                if (MenuText::RadioButton("使用垂直视野", useVFov))
                {
                    config->FsrHorizontalFov.reset();
                    config->FsrVerticalFov = vfov;
                    useVFov = true;
                }

                ImGui::SameLine(0.0f, 6.0f);

                if (MenuText::RadioButton("使用水平视野", !useVFov))
                {
                    config->FsrVerticalFov.reset();
                    config->FsrHorizontalFov = hfov;
                    useVFov = false;
                }

                if (useVFov)
                {
                    if (MenuText::SliderFloat("垂直视野", &vfov, 0.0f, 180.0f, "%.1f"))
                        config->FsrVerticalFov = vfov;

                    ShowHelpMarker("可能改善画质。");
                }
                else
                {
                    if (MenuText::SliderFloat("水平视野", &hfov, 0.0f, 180.0f, "%.1f"))
                        config->FsrHorizontalFov = hfov;

                    ShowHelpMarker("可能改善画质。");
                }

                float cameraNear;
                float cameraFar;

                cameraNear = config->FsrCameraNear.value_or_default();
                cameraFar = config->FsrCameraFar.value_or_default();

                if (MenuText::SliderFloat("相机近裁面", &cameraNear, 0.1f, 500000.0f, "%.1f"))
                    config->FsrCameraNear = cameraNear;
                ShowHelpMarker("可能改善画质并减少拖影。");

                if (MenuText::SliderFloat("相机远裁面", &cameraFar, 0.1f, 500000.0f, "%.1f"))
                    config->FsrCameraFar = cameraFar;
                ShowHelpMarker("可能改善画质并减少拖影。");

                if (MenuText::Button("重置相机参数"))
                {
                    config->FsrVerticalFov.reset();
                    config->FsrHorizontalFov.reset();
                    config->FsrCameraNear.reset();
                    config->FsrCameraFar.reset();
                }

                ImGui::SameLine(0.0f, 6.0f);
                MenuText::Text("近裁面：%.1f 远裁面：%.1f",
                            state.lastFsrCameraNear < 500000.0f ? state.lastFsrCameraNear : 500000.0f,
                            state.lastFsrCameraFar < 500000.0f ? state.lastFsrCameraFar : 500000.0f);

                ImGui::Spacing();
                ImGui::Spacing();
            }
        }
    }
}

void MenuCommon::RenderFramerateSettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& menuResScale = ctx.menuResScale;

    // Framerate ---------------------
    // While Optical F5Low's low latency runs, it or OptiScaler's limiter keeps the cap (native/NativeLowLatency.h
    // OwnsFrameCap); the section stays up either way, so it does not come and go as frame generation pauses
    const bool f5lowRunning = native::lowlatency::Running();

    if (state.reflexLimitsFps || f5lowRunning || config->OverlayMenu.value_or_default())
    {
        SeparatorWithHelpMarker(
            "帧率", "尽量使用 Reflex；AMD／Intel 显卡可通过 fakenvapi 替代。");

        static std::string currentMethod {};
        LowLatencyMode fakenvapiMode = {};
        if (state.reflexLimitsFps)
        {
            fakenvapiMode = fakenvapi::getCurrentMode();

            if (fakenvapiMode == LowLatencyMode::AntiLag2)
                currentMethod = "FSR Anti-Lag 2.0";
            else if (fakenvapiMode == LowLatencyMode::LatencyFlex)
                currentMethod = "LatencyFlex";
            else if (fakenvapiMode == LowLatencyMode::XeLL)
                currentMethod = "XeLL";
            else if (fakenvapiMode == LowLatencyMode::AntiLagVk)
                currentMethod = "Vulkan AntiLag";
            else if (fakenvapiMode == LowLatencyMode::None)
            {
                if (fakenvapi::isUsingAsMainNvapi())
                    currentMethod = "无";
                else
                    currentMethod = "Reflex";
            }

            if (state.rtssReflexInjection && fakenvapiMode == LowLatencyMode::AntiLag2 &&
                config->FGOutput.value_or_default() == FGOutput::FSRFG)
                MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.8f, 0.f, 1.f)),
                                   "RTSS Reflex 注入与 FSR Anti-Lag 2.0、FSR 插帧混用可能出现问题。");
        }
        else
        {
            // Optical F5Low's low latency, also run for XeFG alone (it feeds XeFG's XeLL when the game does not)
            const auto lowLatency = f5lowRunning ? native::lowlatency::GetStatus() : native::lowlatency::Status {};
            const char* owner = lowLatency.forXefgOnly ? "（由 OptiScaler 为 XeFG 提供）" : "（Optical F5Low）";

            if (f5lowRunning && native::lowlatency::OwnsFrameCap())
                currentMethod = std::string(lowLatency.path) + owner;
            else if (f5lowRunning)
                currentMethod = std::string("OptiScaler 限帧器") + owner;
            else if (XellHooks::canLimit())
                currentMethod = "游戏的 XeLL";
            else
                currentMethod = "OptiScaler 限帧器";
        }

        if (state.rtssReflexInjection)
            currentMethod.append(" (RTSS)");

        const bool fakenvapiInactive = (fakenvapi::isUsingAsMainNvapi() || fakenvapiMode == LowLatencyMode::XeLL) &&
                                       !fakenvapi::isLowLatencyActive() && state.reflexLimitsFps;

        if (fakenvapiInactive)
            currentMethod.append("（未运行）");

        MenuText::Text("当前方案：%s", currentMethod.c_str());

        // Run for XeFG alone, the low latency's switch is on a page an XeFG player may never open
        if (f5lowRunning && !state.reflexLimitsFps && native::lowlatency::GetStatus().forXefgOnly)
        {
            MenuText::TextDisabled("游戏没有 Reflex，OptiScaler 自行向 XeFG 的 XeLL 提供标记。");
            ShowHelpMarker("XeFG 始终使用 XeLL；需要帧休眠与标记才能降低延迟。开关位于神经渲染 > Optical F5Low > 低延迟。");
        }

        if (fakenvapiMode == LowLatencyMode::AntiLag2)
            ShowHelpMarker("FSR Anti-Lag 2.0 是 Anti-Lag 2 的新名称。");

        if (state.reflexShowWarning)
        {
            MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)),
                               "Reflex 限帧与 FSR 插帧配合会增加开销。");

            ImGui::Spacing();
        }

        // set initial value
        if (std::isinf(_limitFps))
            _limitFps = config->FramerateLimit.value_or_default();

        MenuText::SliderFloat("帧率上限", &_limitFps, 0, 200, "%.0f");

        if (MenuText::Button("应用上限"))
        {
            config->FramerateLimit = _limitFps;
        }

        ImGui::SameLine(0.0f, 16.0f);

        if (MenuText::Button("重置上限"))
        {
            _limitFps = 0.0f;
            config->FramerateLimit = _limitFps;
        }

        ImGui::Spacing();
        if (auto ch = ScopedCollapsingHeader("可变刷新率帧率上限计算器"); ch.IsHeaderOpen())
        {
            ScopedIndent indent {};
            ImGui::Spacing();

            ImGui::PushItemWidth(105.0f * menuResScale);
            MenuText::InputInt("刷新率", &refreshRate, 1, 1, ImGuiInputTextFlags_None);
            ImGui::PopItemWidth();

            float refreshRateF = static_cast<float>(refreshRate);
            // it's fine to use with real reflex, we only care about antilag
            auto fpsLimitTech = fakenvapi::getCurrentMode();
            constexpr float margin = 0.3f; // in ms
            float frameCap = std::round(10000.f / (1000.f / refreshRateF + margin)) / 10.f;

            if (fpsLimitTech == LowLatencyMode::AntiLag2 || fpsLimitTech == LowLatencyMode::AntiLagVk)
                frameCap = std::round(frameCap);

            MenuText::Text("计算上限：%.1f", frameCap);

            ImGui::SameLine(0.0f, 16.0f);

            if (MenuText::Button("设为帧率上限"))
            {
                _limitFps = frameCap;
                config->FramerateLimit = _limitFps;
            }
        }
    }
}

void MenuCommon::RenderFakenvapiSettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;

    // FAKENVAPI ---------------------------
    MenuText::SeparatorText("fakenvapi");

    // Using state.reflexLimitsFps as a detection for Reflex being used on Nvidia
    bool showLatencyFlex =
        fakenvapi::isUsingAsMainNvapi() || (state.activeFgOutput == FGOutput::XeFG && state.reflexLimitsFps);

    if (showLatencyFlex)
    {
        ImGui::BeginDisabled(state.activeFgOutput == FGOutput::XeFG || state.activeFgInput == FGInput::ForceXeLL);
        if (bool forceLFX = config->FN_ForceLatencyFlex.value_or_default();
            MenuText::Checkbox("强制 LatencyFlex", &forceLFX))
        {
            config->FN_ForceLatencyFlex = forceLFX;
        }
        ShowHelpMarker("默认优先使用可用的 FSR Anti-Lag 2.0／XeLL，此选项可强制改用 LatencyFlex。");
        ImGui::EndDisabled();

        // Keep Force XeLL on the same line if LatencyFlex is visible
        ImGui::SameLine(0.0f, 16.0f);
    }

    // Force XeLL is always visible
    bool forceXell = config->ForceXeLL.value_or_default();
    static bool activeForceXeLL = forceXell;

    if (MenuText::Checkbox("强制 XeLL", &forceXell))
    {
        config->ForceXeLL = forceXell;
    }
    ShowHelpMarker("允许非 Intel 显卡在不插帧时使用 XeLL。会禁用插帧选项，需要重启。");

    if (activeForceXeLL != forceXell)
    {
        ImGui::Spacing();
        MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.0f, 1.f)), "保存 INI 并重启后应用更改。");
        ImGui::Spacing();
    }

    if (showLatencyFlex)
    {
        bool usingLFX = fakenvapi::getCurrentMode() == LowLatencyMode::LatencyFlex;

        ImGui::BeginDisabled(!usingLFX);
        const char* lfxModeNames[] = { "保守", "积极", "Reflex 编号" };
        int lfxMode = std::clamp<int>((int) config->FN_LatencyFlexMode.value_or_default(), 0,
                                      IM_ARRAYSIZE(lfxModeNames) - 1);
        if (MenuText::Segmented("LatencyFlex 模式", &lfxMode, lfxModeNames, IM_ARRAYSIZE(lfxModeNames)))
            config->FN_LatencyFlexMode = (LFXMode) lfxMode;
        ImGui::EndDisabled();
        ShowHelpMarker("保守：最稳妥，但延迟改善可能较少。\n积极：更明显降低延迟，但部分场景可能额外降低帧率。\nReflex 编号：兼容时效果较好，不兼容的游戏会回退到积极模式。");

        const char* reflexModeNames[] = { "跟随游戏", "强制关闭", "强制开启" };
        int reflexMode = std::clamp<int>((int) config->FN_ForceReflex.value_or_default(), 0,
                                         IM_ARRAYSIZE(reflexModeNames) - 1);
        if (MenuText::Segmented("强制 Reflex", &reflexMode, reflexModeNames, IM_ARRAYSIZE(reflexModeNames)))
            config->FN_ForceReflex = (ForceReflex) reflexMode;
        // clang-format on
    }
}

template <typename T> std::string GetMenuOptionLabel(const std::vector<MenuOption<T>>& options, T targetValue)
{
    auto it = std::find_if(options.begin(), options.end(),
                           [targetValue](const MenuOption<T>& option) { return option.value == targetValue; });

    if (it != options.end())
    {
        return it->label;
    }

    return "未知";
}

void MenuCommon::RenderLowLatencySettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;

    // Low Latency ---------------------------
    MenuText::SeparatorText("低延迟");

    static std::vector<MenuOption<LowLatencyInput>> lowLatencyInput = {
        { LowLatencyInput::None, "无（关闭）" },    { LowLatencyInput::Auto, "自动" },
        { LowLatencyInput::AntiLag2, "AntiLag 2" }, { LowLatencyInput::Reflex, "Reflex" },
        { LowLatencyInput::XeLL, "XeLL" },          { LowLatencyInput::UeLowLatency, "UeLowLatency" },
    };

    static std::vector<MenuOption<LowLatencyMode>> lowLatencyOutput = {
        { LowLatencyMode::None, "无（关闭）" },
        { LowLatencyMode::Auto, "自动" },
        { LowLatencyMode::LatencyFlex, "LatencyFlex" },
        { LowLatencyMode::AntiLag2, "AntiLag 2" },
        { LowLatencyMode::XeLL, "XeLL" },
        { LowLatencyMode::AntiLagVk, "AntiLag Vk" },
        { LowLatencyMode::Reflex, "Reflex" },
    };

    LowLatencyInput activeInput {};
    LowLatencyMode activeOutput {};

    if (MenuText::BeginTable("lowLatencyActive", 2, ImGuiTableFlags_SizingStretchSame))
    {
        InputCommon::get_currently_active(activeInput, activeOutput);

        ImGui::TableNextColumn();

        MenuText::Text("当前输入：%s", GetMenuOptionLabel(lowLatencyInput, activeInput).c_str());

        ImGui::TableNextColumn();

        MenuText::Text("当前输出：%s", GetMenuOptionLabel(lowLatencyOutput, activeOutput).c_str());

        ImGui::EndTable();
    }

    if (MenuText::BeginTable("lowLatencySelection", 2, ImGuiTableFlags_SizingStretchSame))
    {
        ImGui::TableNextColumn();

        auto avalibleInputs = InputCommon::get_avaliable_inputs();

        lowLatencyInput[(uint32_t) LowLatencyInput::AntiLag2].set_disabled(!avalibleInputs[LowLatencyInput::AntiLag2]);
        lowLatencyInput[(uint32_t) LowLatencyInput::Reflex].set_disabled(!avalibleInputs[LowLatencyInput::Reflex]);
        lowLatencyInput[(uint32_t) LowLatencyInput::XeLL].set_disabled(!avalibleInputs[LowLatencyInput::XeLL]);
        lowLatencyInput[(uint32_t) LowLatencyInput::UeLowLatency].set_disabled(
            !avalibleInputs[LowLatencyInput::UeLowLatency]);

        // need to have a value before combo
        if (!config->LowLatencyInput.has_value())
            config->LowLatencyInput = config->LowLatencyInput.value_or_default();

        PopulateCombo("输入来源", config->LowLatencyInput, lowLatencyInput);

        ImGui::TableNextColumn();

        lowLatencyOutput[(uint32_t) LowLatencyMode::AntiLagVk].set_disabled(true, "不支持");
        lowLatencyOutput[(uint32_t) LowLatencyMode::Reflex].set_disabled(true, "不支持");

        // need to have a value before combo
        if (!config->LowLatencyOutput.has_value())
            config->LowLatencyOutput = config->LowLatencyOutput.value_or_default();

        PopulateCombo("输出", config->LowLatencyOutput, lowLatencyOutput);

        ImGui::EndTable();
    }

    if (activeOutput == LowLatencyMode::LatencyFlex)
    {
        const char* lfxModeNames[] = { "保守", "积极", "Reflex 编号" };
        int lfxMode = std::clamp<int>((int) config->FN_LatencyFlexMode.value_or_default(), 0,
                                      IM_ARRAYSIZE(lfxModeNames) - 1);
        if (MenuText::Segmented("LatencyFlex 模式", &lfxMode, lfxModeNames, IM_ARRAYSIZE(lfxModeNames)))
            config->FN_LatencyFlexMode = (LFXMode) lfxMode;
        ShowHelpMarker("保守：最稳妥，但延迟改善可能较少。\n积极：更明显降低延迟，但部分场景可能额外降低帧率。\nReflex 编号：兼容时效果较好，不兼容的游戏会回退到积极模式。");
    }

    const char* reflexStateNames[] = { "跟随游戏", "强制关闭", "强制开启" };
    int reflexState = std::clamp<int>((int) config->FN_ForceReflex.value_or_default(), 0,
                                      IM_ARRAYSIZE(reflexStateNames) - 1);
    if (MenuText::Segmented("强制状态", &reflexState, reflexStateNames, IM_ARRAYSIZE(reflexStateNames)))
        config->FN_ForceReflex = (ForceReflex) reflexState;
}

void MenuCommon::RenderAnisotropySettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& currentFeature = ctx.currentFeature;
    auto& menuResScale = ctx.menuResScale;
    if (state.swapchainApi != Vulkan || state.swapchainInteropApi == SwapchainInteropApi::VkwDx12)
    {
        ImGui::Spacing();
        if (auto ch = ScopedCollapsingHeader(
                "各向异性过滤",
                (currentFeature == nullptr || currentFeature->IsFrozen()) ? ImGuiTreeNodeFlags_DefaultOpen : 0);
            ch.IsHeaderOpen())
        {
            ScopedIndent indent {};
            ImGui::Spacing();
            ImGui::PushItemWidth(65.0f * menuResScale);

            auto selectedAF =
                config->AnisotropyOverride.has_value() ? std::to_string(config->AnisotropyOverride.value()) : "自动";
            if (MenuText::BeginCombo("强制各向异性过滤", selectedAF.c_str()))
            {
                if (MenuText::Selectable("自动", !config->AnisotropyOverride.has_value()))
                    config->AnisotropyOverride.reset();

                if (MenuText::Selectable("1", config->AnisotropyOverride.value_or(0) == 1))
                    config->AnisotropyOverride = 1;

                if (MenuText::Selectable("2", config->AnisotropyOverride.value_or(0) == 2))
                    config->AnisotropyOverride = 2;

                if (MenuText::Selectable("4", config->AnisotropyOverride.value_or(0) == 4))
                    config->AnisotropyOverride = 4;

                if (MenuText::Selectable("8", config->AnisotropyOverride.value_or(0) == 8))
                    config->AnisotropyOverride = 8;

                if (MenuText::Selectable("16", config->AnisotropyOverride.value_or(0) == 16))
                    config->AnisotropyOverride = 16;

                ImGui::EndCombo();
            }

            ImGui::PopItemWidth();

            bool afComp = config->AnisotropyModifyComp.value_or_default();
            if (MenuText::Checkbox("修改比较过滤", &afComp))
                config->AnisotropyModifyComp = afComp;

            ShowHelpMarker("更新比较过滤器。");

            ImGui::SameLine(0.0f, 6.0f);

            bool afMinMax = config->AnisotropyModifyMinMax.value_or_default();
            if (MenuText::Checkbox("修改最小／最大过滤", &afMinMax))
                config->AnisotropyModifyMinMax = afMinMax;

            ShowHelpMarker("更新最小／最大过滤器。");

            bool afSkipPoint = config->AnisotropySkipPointFilter.value_or_default();
            if (MenuText::Checkbox("跳过点采样过滤", &afSkipPoint))
                config->AnisotropySkipPointFilter = afSkipPoint;

            ShowHelpMarker("不更新点采样过滤器。");

            MenuText::Text("可能需要更改分辨率或预设后生效。");
        }
    }
}

void MenuCommon::RenderActiveImageSettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& currentFeature = ctx.currentFeature;
    auto& menuResScale = ctx.menuResScale;

    static const bool genshinExecutable=GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring());
    const bool genshinSrExposure=genshinExecutable && config->UseFsr2Inputs.value_or_default() &&
        (!currentFeature || currentFeature->GetUpscalerType()==Upscaler::DLSS_on12) &&
        (state.api==API::DX11 || (!currentFeature && GenshinLegacy::GetStatus().effectiveEnabled));
    // Also available when unsupported startup exposure prevented CreateFeature.
    if(genshinSrExposure && !config->AutoExposure.value_or(true)) {
        const bool liveAuto=currentFeature && currentFeature->IsInited() && currentFeature->AutoExposure();
        MenuText::TextWrapped(liveAuto ?
            "手动曝光配置不受支持：当前 SR feature 使用自动曝光。配置仍为关闭。" :
            "手动曝光配置不受支持：尚无可用的自动曝光 SR feature。配置仍为关闭。");
        if(MenuText::Button("启用自动曝光并恢复 SR##genshinExposure")) {
            config->AutoExposure=true;
            // The retained unregistered owner retries on the next token. A
            // live supported feature already has this mode, needing no reset.
            if(currentFeature && !currentFeature->AutoExposure())ReInitUpscaler();
            LOG_INFO("R048 user restored SR automatic exposure: requested_auto=true manual_input_supported=false");
        }
    }

    bool rcasEnabled = false;

    if (currentFeature != nullptr && !currentFeature->IsFrozen())
    {
        // SHARPNESS -----------------------------
        MenuText::SeparatorText("锐化强度");

        if (bool overrideSharpness = config->OverrideSharpness.value_or_default();
            MenuText::CheckboxInline("覆盖", &overrideSharpness))
        {
            config->OverrideSharpness = overrideSharpness;

            if (currentBackend == Upscaler::DLSS && currentFeature->Version().major < 3)
            {
                state.newBackend = currentBackend;
                MARK_ALL_BACKENDS_CHANGED();
            }
        }
        ShowHelpMarker("忽略游戏提供的数值，改用下方设置。");

        ImGui::SameLine(0.0f, 16.0f * menuResScale);

        float featuresCurrentSharpness = currentFeature->Sharpness();
        if (featuresCurrentSharpness > 0.0f)
            MenuText::TextDisabled("（当前锐化：%.3f）", featuresCurrentSharpness);
        else
            MenuText::TextDisabled("（当前锐化：关闭）");

        ImGui::BeginDisabled(!config->OverrideSharpness.value_or_default());

        float sharpness = config->Sharpness.value_or_default();

        if (MenuText::SliderFloat("锐化强度", &sharpness, 0.0f, 1.0f))
            config->Sharpness = sharpness;

        ImGui::EndDisabled();

        // RCAS
        // if (state.api == DX12 || state.api == DX11)
        {
            // xess or dlss version >= 2.5.1
            constexpr feature_version requiredDlssVersion = { 2, 5, 1 };
            rcasEnabled = (currentBackend == Upscaler::XeSS ||
                           (currentBackend == Upscaler::DLSS && currentFeature->Version() >= requiredDlssVersion));

            ImGui::Spacing();
            ImGui::Spacing();

            if (bool rcas = config->RcasEnabled.value_or(rcasEnabled); MenuText::Checkbox("启用 RCAS／深度感知锐化", &rcas))
                config->RcasEnabled = rcas;

            ShowHelpMarker("启用 OptiScaler 锐化滤镜，默认采用游戏强度。勾选覆盖并调整滑块可自行设置。部分超分已有锐化，不一定需要此功能。");

            ImGui::BeginDisabled(!config->RcasEnabled.value_or(rcasEnabled));

            auto sharpnessShader = (int32_t) Config::Instance()->SharpnessShader.value_or_default();

            if (MenuText::RadioButton("RCAS", &sharpnessShader, (int32_t) SharpenShader::RCAS))
            {
                Config::Instance()->SharpnessShader = SharpenShader::RCAS;
            }

            ShowHelpMarker("使用 AMD RCAS，增加对比度参数和运动自适应锐化支持。");

            ImGui::SameLine(0.0f, 6.0f);

            if (MenuText::RadioButton("深度感知（RCAS）", &sharpnessShader, (int32_t) SharpenShader::DepthAware))
            {
                Config::Instance()->SharpnessShader = SharpenShader::DepthAware;
            }

            ShowHelpMarker("更智能的深度感知 RCAS 锐化，伪影较少但开销更高。物体越远，锐化越强。");

            ImGui::SameLine(0.0f, 6.0f);

            if (MenuText::RadioButton("深度感知（DAS）", &sharpnessShader,
                                   (int32_t) SharpenShader::LocalContrastDepthAware))
            {
                Config::Instance()->SharpnessShader = SharpenShader::LocalContrastDepthAware;
            }

            ShowHelpMarker("深度感知的方向自适应亮度锐化，伪影较少但开销更高。物体越远，锐化越强。");

            ImGui::Spacing();

            if (bool overrideMotionSharpness = config->MotionSharpnessEnabled.value_or_default();
                MenuText::Checkbox("启用运动自适应锐化", &overrideMotionSharpness))
                config->MotionSharpnessEnabled = overrideMotionSharpness;
            ShowHelpMarker("根据运动调整锐化强度。");

            if (Config::Instance()->SharpnessShader.value_or_default() != SharpenShader::RCAS)
            {
                if (bool overrideMSDebug = config->MotionSharpnessDebug.value_or_default();
                    MenuText::Checkbox("深度＋运动自适应调试", &overrideMSDebug))
                    config->MotionSharpnessDebug = overrideMSDebug;

                ShowHelpMarker("蓝色标记检测到的深度边缘。红色区域增加锐化，绿色区域减少锐化。");

                if (auto ch = ScopedCollapsingHeader("高级深度感知参数"); ch.IsHeaderOpen())
                {
                    ScopedIndent indent {};
                    ImGui::Spacing();

                    if (bool clamp = config->DAClampOutput.value_or(false); MenuText::Checkbox("限制输出范围", &clamp))
                    {
                        if (clamp)
                            config->DAClampOutput = true;
                        else
                            config->DAClampOutput.reset();
                    }

                    ShowHelpMarker("将最终画面限制到 [0,1]，减少亮边和负颜色等越界伪影。适合低动态范围；HDR 是否使用取决于色调映射。未设置时跟随超分 HDR 标志。");

                    if (currentFeature->DepthLinear())
                    {
                        float depthBias = config->DADepthBias.value_or(0.0015f);
                        if (MenuText::SliderFloat("深度偏差", &depthBias, 0.005f, 0.03f, "%.4f"))
                            config->DADepthBias = depthBias;

                        ShowHelpMarker("边缘检测忽略较小深度差异。高值减少闪烁噪声，但可能柔化几何边缘；低值保留细节但可能不稳定。");

                        float depthScale = config->DADepthScale.value_or(250.0f);
                        if (MenuText::SliderFloat("深度缩放", &depthScale, 100.0f, 600.0f, "%.1f"))
                            config->DADepthScale = depthScale;

                        ShowHelpMarker("控制深度边缘两侧的锐化抑制。高值更能减少物体边缘光晕；低值更锐利但风险更高。");
                    }
                    else
                    {
                        float depthBias = config->DADepthBias.value_or(0.001f);
                        if (MenuText::SliderFloat("深度偏差", &depthBias, 0.0001f, 0.003f, "%.4f"))
                            config->DADepthBias = depthBias;

                        ShowHelpMarker("边缘检测忽略较小深度差异。高值减少闪烁噪声，但可能柔化几何边缘；低值保留细节但可能不稳定。");

                        float depthScale = config->DADepthScale.value_or(35.0f);
                        if (MenuText::SliderFloat("深度缩放", &depthScale, 25.0f, 400.0f, "%.1f"))
                            config->DADepthScale = depthScale;

                        ShowHelpMarker("控制深度边缘两侧的锐化抑制。高值更能减少物体边缘光晕；低值更锐利但风险更高。");
                    }

                    if (MenuText::Button("重置深度参数"))
                    {
                        config->DADepthBias.reset();
                        config->DADepthScale.reset();
                    }
                }
            }
            else
            {
                if (bool contrastEnabled = config->ContrastEnabled.value_or_default();
                    MenuText::Checkbox("启用对比度控制", &contrastEnabled))
                    config->ContrastEnabled = contrastEnabled;

                ShowHelpMarker("控制高对比区域的锐化。");

                ImGui::BeginDisabled(!config->ContrastEnabled.value_or_default());

                float contrast = config->Contrast.value_or_default();
                if (MenuText::SliderFloat("对比度", &contrast, -2.0f, 2.0f, "%.2f"))
                    config->Contrast = contrast;

                ShowHelpMarker("正值降低高对比区域锐化，负值提高。");

                ImGui::EndDisabled();
            }

            ImGui::Spacing();
            if (auto ch = ScopedCollapsingHeader("运动自适应锐化##2"); ch.IsHeaderOpen())
            {
                ScopedIndent indent {};
                ImGui::Spacing();

                ImGui::BeginDisabled(!config->MotionSharpnessEnabled.value_or_default());

                if (Config::Instance()->SharpnessShader.value_or_default() == SharpenShader::RCAS)
                {
                    if (bool overrideMSDebug = config->MotionSharpnessDebug.value_or_default();
                        MenuText::Checkbox("运动自适应调试", &overrideMSDebug))
                        config->MotionSharpnessDebug = overrideMSDebug;
                    ShowHelpMarker("红色区域增加锐化，绿色区域减少锐化。");
                }

                float motionSharpness = config->MotionSharpness.value_or_default();
                MenuText::SliderFloat("运动锐化增量", &motionSharpness, -1.0f, 1.0f, "%.3f");
                config->MotionSharpness = motionSharpness;

                ShowHelpMarker("运动引起的最大锐化变化。负值减少运动锐化，通常推荐；正值增加。实际变化随运动缩放并受此上限限制。");

                float motionThreshod = config->MotionThreshold.value_or_default();
                MenuText::SliderFloat("运动阈值", &motionThreshod, 0.0f, 100.0f, "%.2f");
                config->MotionThreshold = motionThreshod;

                ShowHelpMarker("开始调整锐化所需的最低运动量。高值忽略小运动，更稳定；低值更敏感。");

                float motionScale = config->MotionScaleLimit.value_or_default();
                MenuText::SliderFloat("运动范围", &motionScale, 0.01f, 100.0f, "%.2f");
                config->MotionScaleLimit = motionScale;

                ShowHelpMarker("锐化调整从零到最大强度的运动范围。大值变化更平滑，小值反应更快更强。");

                ImGui::EndDisabled();

                ImGui::Spacing();
                ImGui::Spacing();
            }

            ImGui::EndDisabled();
        }

        // UPSCALE RATIO OVERRIDE -----------------

        auto minSliderLimit = config->ExtendedLimits.value_or_default() ? 0.1f : 1.0f;
        auto maxSliderLimit = config->ExtendedLimits.value_or_default() ? 6.0f : 3.0f;

        MenuText::SeparatorText("超分比例覆盖");

        if (bool upOverride = config->UpscaleRatioOverrideEnabled.value_or_default();
            MenuText::Checkbox("统一覆盖", &upOverride))
        {
            config->UpscaleRatioOverrideEnabled = upOverride;

            if (upOverride)
                config->QualityRatioOverrideEnabled = false;
        }
        ShowHelpMarker("所有质量预设使用同一比例。例如 1080p 输出使用 1.5 倍比例时，内部为 720p。");

        if (bool qOverride = config->QualityRatioOverrideEnabled.value_or_default();
            MenuText::Checkbox("按质量档位覆盖", &qOverride))
        {
            config->QualityRatioOverrideEnabled = qOverride;

            if (qOverride)
                config->UpscaleRatioOverrideEnabled = false;
        }

        ShowHelpMarker("分别覆盖各档位比例；游戏不一定支持所有档位。1080p 输出使用 1.5 倍时，内部为 720p。");

        if (config->UpscaleRatioOverrideEnabled.value_or_default())
        {
            float urOverride = config->UpscaleRatioOverrideValue.value_or_default();
            MenuText::SliderFloat("统一比例", &urOverride, minSliderLimit, maxSliderLimit, "%.3f");
            config->UpscaleRatioOverrideValue = urOverride;
        }

        if (config->QualityRatioOverrideEnabled.value_or_default())
        {
            float qDlaa = config->QualityRatio_DLAA.value_or_default();
            if (MenuText::SliderFloat("DLAA", &qDlaa, minSliderLimit, maxSliderLimit, "%.3f"))
                config->QualityRatio_DLAA = qDlaa;

            float qUq = config->QualityRatio_UltraQuality.value_or_default();
            if (MenuText::SliderFloat("超高质量", &qUq, minSliderLimit, maxSliderLimit, "%.3f"))
                config->QualityRatio_UltraQuality = qUq;

            float qQ = config->QualityRatio_Quality.value_or_default();
            if (MenuText::SliderFloat("质量", &qQ, minSliderLimit, maxSliderLimit, "%.3f"))
                config->QualityRatio_Quality = qQ;

            float qB = config->QualityRatio_Balanced.value_or_default();
            if (MenuText::SliderFloat("均衡", &qB, minSliderLimit, maxSliderLimit, "%.3f"))
                config->QualityRatio_Balanced = qB;

            float qP = config->QualityRatio_Performance.value_or_default();
            if (MenuText::SliderFloat("性能", &qP, minSliderLimit, maxSliderLimit, "%.3f"))
                config->QualityRatio_Performance = qP;

            float qUp = config->QualityRatio_UltraPerformance.value_or_default();
            if (MenuText::SliderFloat("极致性能", &qUp, minSliderLimit, maxSliderLimit, "%.3f"))
                config->QualityRatio_UltraPerformance = qUp;
        }

        if (currentFeature != nullptr && !currentFeature->IsFrozen())
        {
            // OUTPUT SCALING -----------------------------
            // if (state.api == DX12 || state.api == DX11)
            {
                // if motion vectors are not display size
                ImGui::BeginDisabled(!currentFeature->LowResMV() &&
                                     currentFeature->RenderWidth() != currentFeature->DisplayWidth());

                MenuText::SeparatorText("输出缩放");

                float defaultRatio = 1.5f;

                if (_ssRatio == 0.0f)
                {
                    _ssRatio = config->OutputScalingMultiplier.value_or(defaultRatio);
                    _ssEnabled = config->OutputScalingEnabled.value_or_default();
                    _ssDownsampler = config->OutputScalingDownscaler.value_or_default();
                }

                ImGui::BeginDisabled((currentBackend == Upscaler::XeSS || currentBackend == Upscaler::DLSS) &&
                                     currentFeature->RenderWidth() > currentFeature->DisplayWidth());
                MenuText::CheckboxInline("启用", &_ssEnabled);
                ImGui::EndDisabled();

                ShowHelpMarker("先输出到更高内部尺寸，再缩小到显示器尺寸。小于 1 降低开销，大于 1 提高锐度并增加开销。灰色不可选时请查 Wiki 的虚幻引擎说明；下方显示目标分辨率与总比例，总比例最大 3。");

                ImGui::SameLine(0.0f, 6.0f);

                ImGui::BeginDisabled(!_ssEnabled);
                {
                    ImGui::PushItemWidth(95.0f * menuResScale);

                    // clang-format off
                    std::vector<MenuOption<Scaler>> ds_options = {
                        { Scaler::FSR1, "FSR1",
                            "默认方案，画质较好且速度很快。" },
                        { Scaler::Bicubic, "双三次",
                            "最快的传统方案，画面较柔和，适合部分缩小场景。" },
                        { Scaler::CatmullRom, "Catmull-Rom",
                            "主要用于缩小，保留对比且伪影少，但比 Lanczos 柔和。" },
                        { Scaler::Lanczos2, "Lanczos2",
                            "比 Lanczos3 更轻更快，振铃较少，但稍模糊。" },
                        { Scaler::Lanczos3, "Lanczos3",
                            "比 Lanczos2 更重更锐利，也更容易振铃；与 Kaiser3 同属高质量方案。" },
                        { Scaler::Kaiser2, "Kaiser2",
                            "类似 Lanczos2，更平滑、伪影更少，但稍模糊。" },
                        { Scaler::Kaiser3, "Kaiser3",
                            "类似 Lanczos3，伪影少得多，但 GPU 开销高得多。" },
                        { Scaler::Magic, "MAGIC",
                            "专门减少伪影，消除强烈光晕，外观自然但可能稍柔和。" }
                    };
                    // clang-format on

                    const bool isUpsampleRatio = _ssRatio < 1.0f;
                    const std::string disabledReason = "比例低于 1 时仅支持 FSR1 和双三次。";

                    for (auto& opt : ds_options)
                    {
                        if (isUpsampleRatio && opt.value > Scaler::Bicubic)
                            opt.set_disabled(true, opt.tooltip + "\n\n" + disabledReason);
                    }

                    if (isUpsampleRatio && _ssDownsampler > Scaler::Bicubic)
                        _ssDownsampler = Scaler::FSR1;

                    PopulateCombo("缩小滤镜", _ssDownsampler, ds_options);

                    ImGui::PopItemWidth();
                }
                ImGui::EndDisabled();

                bool applyEnabled = _ssEnabled != config->OutputScalingEnabled.value_or_default() ||
                                    _ssRatio != config->OutputScalingMultiplier.value_or(defaultRatio) ||
                                    _ssDownsampler != config->OutputScalingDownscaler.value_or_default();

                ImGui::BeginDisabled(!applyEnabled);
                if (MenuText::Button("应用更改"))
                {
                    config->OutputScalingEnabled = _ssEnabled;
                    config->OutputScalingMultiplier = _ssRatio;

                    if (_ssRatio < 1.0f && _ssDownsampler > Scaler::Bicubic)
                        _ssDownsampler = Scaler::FSR1;

                    config->OutputScalingDownscaler = _ssDownsampler;

                    const bool usesDlssd = currentFeature->GetUpscalerType() == Upscaler::DLSSD;
                    if (usesDlssd)
                        state.newBackend = Upscaler::DLSSD;
                    else
                        state.newBackend = currentBackend;

                    MARK_ALL_BACKENDS_CHANGED();
                }
                ImGui::EndDisabled();

                ImGui::BeginDisabled(!_ssEnabled || currentFeature->RenderWidth() > currentFeature->DisplayWidth());
                MenuText::SliderFloat("比例", &_ssRatio, 0.5f, 3.0f, "%.2f");
                ImGui::EndDisabled();

                if (currentFeature != nullptr && !currentFeature->IsFrozen())
                {
                    MenuText::Text("输出缩放：%s，目标分辨率：%dx%d（%.2f）\n抖动采样数：%d",
                                config->OutputScalingEnabled.value_or_default() ? "已启用" : "已关闭",
                                (uint32_t) (currentFeature->DisplayWidth() * _ssRatio),
                                (uint32_t) (currentFeature->DisplayHeight() * _ssRatio),
                                ((float) currentFeature->DisplayWidth() * _ssRatio) /
                                    (float) currentFeature->RenderWidth(),
                                currentFeature->JitterCount());
                }

                ImGui::EndDisabled();
            }
        }

        // INIT -----------------------------
        // Full-width rows keep localized labels, switches and their inline reset/help attachments together.
        {
            ImGui::Spacing();
            if (auto ch = ScopedCollapsingHeader("初始化标志###高级初始化标志"); ch.IsHeaderOpen())
            {
                ScopedIndent indent {};
                ImGui::Spacing();

                // AutoExposure is always enabled for XeSS with native Dx11.
                const bool autoExposureDisabled = state.api == API::DX11 && currentBackend == Upscaler::XeSS;
                ImGui::BeginDisabled(autoExposureDisabled);
                // Reject before writing Config or requesting feature retirement.
                // A false live feature may still be explicitly restored to true.
                ImGui::BeginDisabled(genshinSrExposure && currentFeature->AutoExposure());
                if (bool autoExposure = currentFeature->AutoExposure(); MenuText::Checkbox("自动曝光", &autoExposure))
                {
                    if(!genshinSrExposure || autoExposure) {
                        config->AutoExposure = autoExposure;
                        ReInitUpscaler();
                    }
                }
                ImGui::EndDisabled();
                const bool exposureHelpHovered = MenuText::ItemHelpHovered();
                // Reset remains an explicit action separate from the unavailable
                // manual toggle. The internal Genshin producer supplies auto=true.
                if(genshinSrExposure) {
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!config->AutoExposure.has_value());
                    if(MenuText::Button("R")) {
                        config->AutoExposure.reset();
                        if(!currentFeature->AutoExposure())ReInitUpscaler();
                    }
                    ImGui::EndDisabled();
                } else ShowResetButton(&config->AutoExposure, "R");
                MenuText::ItemHelp(exposureHelpHovered, genshinSrExposure ?
                    "原神 Bridge 当前仅支持自动曝光；缺少受支持的手动曝光输入，不能关闭。R 恢复游戏标志。" :
                    "部分虚幻引擎游戏需要此设置。颜色闪烁或物体拖影时可尝试。");
                ImGui::EndDisabled();

                auto accessToReactiveMask = currentFeature->AccessToReactiveMask();
                ImGui::BeginDisabled(!accessToReactiveMask);

                bool canUseReactiveMask =
                    accessToReactiveMask && currentBackend != Upscaler::DLSS &&
                    (currentBackend != Upscaler::XeSS || currentFeature->Version() >= feature_version { 2, 0, 1 });

                bool disableReactiveMask = config->DisableReactiveMask.value_or(!canUseReactiveMask);

                if (MenuText::Checkbox("禁用反应遮罩", &disableReactiveMask))
                {
                    config->DisableReactiveMask = disableReactiveMask;

                    if (currentBackend == Upscaler::XeSS)
                    {
                        state.newBackend = currentBackend;
                        MARK_ALL_BACKENDS_CHANGED();
                    }
                }

                ImGui::EndDisabled();

                if (accessToReactiveMask)
                    ShowHelpMarker("控制反应遮罩的使用。原本提供给 DLSS 的反应遮罩与 FSR／XeSS 配合时可能降低画质。");
                else
                    ShowHelpMarker("游戏未提供反应遮罩，此选项不可用。");


                if (MenuText::BeginTable("init2", 2, ImGuiTableFlags_SizingStretchProp))
                {
                    ImGui::TableNextColumn();
                    if (bool depth = currentFeature->DepthInverted(); MenuText::Checkbox("反向深度", &depth))
                    {
                        config->DepthInverted = depth;
                        ReInitUpscaler();
                    }
                    ShowResetButton(&config->DepthInverted, "R##2");
                    ShowHelpMarker("通常无需更改。");

                    ImGui::TableNextColumn();
                    if (bool hdr = currentFeature->IsHdr(); MenuText::Checkbox("HDR", &hdr))
                    {
                        config->HDR = hdr;
                        ReInitUpscaler();
                    }
                    ShowResetButton(&config->HDR, "R##1");
                    ShowHelpMarker("可能改善部分游戏的紫色偏色。");

                    ImGui::TableNextColumn();
                    if (bool mv = !currentFeature->LowResMV(); MenuText::Checkbox("显示分辨率运动矢量", &mv))
                    {
                        config->DisplayResolution = mv;

                        // Disable output scaling when
                        // Display res MV is active
                        if (mv)
                        {
                            config->OutputScalingEnabled = false;
                            _ssEnabled = false;
                        }

                        ReInitUpscaler();
                    }
                    ShowResetButton(&config->DisplayResolution, "R##4");
                    ShowHelpMarker("主要用于修正虚幻引擎游戏左上画面模糊。");

                    ImGui::TableNextColumn();

                    if (bool jitter = currentFeature->JitteredMV(); MenuText::Checkbox("消除抖动偏移", &jitter))
                    {
                        config->JitterCancellation = jitter;
                        ReInitUpscaler();
                    }
                    ShowResetButton(&config->JitterCancellation, "R##3");
                    ShowHelpMarker("修正运动数据中已包含抖动的游戏。");

                    ImGui::TableNextColumn();
                    ImGui::EndTable();
                }

                if (currentFeature->AccessToReactiveMask() && currentBackend != Upscaler::DLSS)
                {
                    ImGui::BeginDisabled(config->DisableReactiveMask.value_or(currentBackend == Upscaler::XeSS));

                    bool binaryMask = state.api == Vulkan || currentBackend == Upscaler::XeSS;
                    auto defaultBias = binaryMask ? 0.0f : 0.45f;
                    auto maskBias = config->DlssReactiveMaskBias.value_or(defaultBias);

                    if (!binaryMask)
                    {
                        if (MenuText::SliderFloat("反应遮罩偏差", &maskBias, 0.0f, 0.9f, "%.2f"))
                            config->DlssReactiveMaskBias = maskBias;

                        ShowHelpMarker("大于 0 时启用反应遮罩。");
                    }
                    else
                    {
                        bool useRM = maskBias > 0.0f;
                        if (MenuText::Checkbox("使用二值反应遮罩", &useRM))
                        {
                            if (useRM)
                                config->DlssReactiveMaskBias = 0.45f;
                            else
                                config->DlssReactiveMaskBias.reset();
                        }
                    }

                    ImGui::EndDisabled();
                }
            }
        }
    }
}

void MenuCommon::RenderMagnifierSettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;

    // Magnifier -----------------------------
    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader("放大镜"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        bool magnifierEnabled = config->MagnifierEnabled.value_or_default();
        if (MenuText::Checkbox("启用放大镜", &magnifierEnabled))
            config->MagnifierEnabled = magnifierEnabled;

        ImGui::BeginDisabled(!magnifierEnabled);

        float magnifierSize = config->MagnifierSize.value_or_default();
        if (MenuText::SliderFloat("尺寸", &magnifierSize, 5.0f, 50.0f, "画面的 %.1f%%"))
            config->MagnifierSize = magnifierSize;

        int zoomFactor = config->MagnifierZoomFactor.value_or_default();
        if (MenuText::SliderInt("放大倍数", &zoomFactor, 2, 20, "%dx"))
            config->MagnifierZoomFactor = zoomFactor;

        float borderSize = config->MagnifierBorderSize.value_or_default();
        if (MenuText::SliderFloat("边框宽度", &borderSize, 0.0f, 2.0f, "画面的 %.2f%%"))
            config->MagnifierBorderSize = borderSize;

        ImGui::Separator();
        MenuText::Text("位置");

        bool staticMode = config->MagnifierStaticPosX.has_value() && config->MagnifierStaticPosY.has_value();
        if (staticMode)
        {
            float staticX = config->MagnifierStaticPosX.value();
            if (MenuText::SliderFloat("固定位置 X", &staticX, 0.0f, 100.0f, "%.1f%%"))
                config->MagnifierStaticPosX = staticX;

            float staticY = config->MagnifierStaticPosY.value();
            if (MenuText::SliderFloat("固定位置 Y", &staticY, 0.0f, 100.0f, "%.1f%%"))
                config->MagnifierStaticPosY = staticY;

            if (MenuText::Button("重置固定位置（跟随光标）"))
            {
                config->MagnifierStaticPosX.reset();
                config->MagnifierStaticPosY.reset();
            }
        }
        else
        {
            // Button to initialize static position mode
            if (MenuText::Button("设置固定位置"))
            {
                config->MagnifierStaticPosX = 50.0f;
                config->MagnifierStaticPosY = 50.0f;
            }
            ImGui::SameLine();
            MenuText::TextDisabled("（当前跟随光标）");

            float offsetX = config->MagnifierCursorOffsetX.value_or_default();
            if (MenuText::SliderFloat("光标偏移 X", &offsetX, -300.0f, 300.0f, "%.0f px"))
                config->MagnifierCursorOffsetX = offsetX;

            float offsetY = config->MagnifierCursorOffsetY.value_or_default();
            if (MenuText::SliderFloat("光标偏移 Y", &offsetY, -300.0f, 300.0f, "%.0f px"))
                config->MagnifierCursorOffsetY = offsetY;
        }

        ImGui::EndDisabled();
        ImGui::Spacing();
    }
}
void MenuCommon::RenderQuirksSettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;

    // QUIRKS -----------------------------
    if (state.detectedQuirks.size() > 0)
    {
        ImGui::Spacing();
        if (auto ch = ScopedCollapsingHeader("已启用的兼容修正"); ch.IsHeaderOpen())
        {
            ScopedIndent indent {};
            ImGui::Spacing();

            for (const auto& quirk : state.detectedQuirks)
            {
                MenuText::TextWrapped("%s", quirk.c_str());
            }
        }
    }
}

void MenuCommon::RenderAdvancedSettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& currentFeature = ctx.currentFeature;

    // ADVANCED SETTINGS -----------------------------
    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader("高级设置"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        if (currentFeature != nullptr && !currentFeature->IsFrozen())
        {
            bool extendedLimits = config->ExtendedLimits.value_or_default();
            if (MenuText::Checkbox("启用扩展范围", &extendedLimits))
                config->ExtendedLimits = extendedLimits;

            ShowHelpMarker("扩大质量档位滑块范围，会改变分辨率检测逻辑，可能造成异常或崩溃。");
        }

        bool pcShaders = config->UsePrecompiledShaders.value_or_default();
        if (MenuText::Checkbox("使用预编译着色器", &pcShaders))
        {
            config->UsePrecompiledShaders = pcShaders;
            state.newBackend = currentBackend;
            MARK_ALL_BACKENDS_CHANGED();
        }

        // DRS
        MenuText::SeparatorText("动态分辨率缩放（DRS）");
        if (MenuText::BeginTable("drs", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableNextColumn();
            if (bool drsMin = config->DrsMinOverrideEnabled.value_or_default();
                MenuText::Checkbox("覆盖最低值", &drsMin))
                config->DrsMinOverrideEnabled = drsMin;
            ShowHelpMarker("修正忽略官方 DRS 范围的游戏。");

            ImGui::TableNextColumn();
            if (bool drsMax = config->DrsMaxOverrideEnabled.value_or_default();
                MenuText::Checkbox("覆盖最高值", &drsMax))
                config->DrsMaxOverrideEnabled = drsMax;
            ShowHelpMarker("修正忽略官方 DRS 范围的游戏。");

            ImGui::EndTable();
        }

        // Non-DLSS hotfixes -----------------------------
        if (currentFeature != nullptr && !currentFeature->IsFrozen() && currentBackend != Upscaler::DLSS)
        {
            // BARRIERS -----------------------------
            ImGui::Spacing();
            if (auto ch = ScopedCollapsingHeader("资源状态屏障"); ch.IsHeaderOpen())
            {
                ScopedIndent indent {};
                ImGui::Spacing();

                AddResourceBarrier("颜色", &config->ColorResourceBarrier);
                AddResourceBarrier("深度", &config->DepthResourceBarrier);
                AddResourceBarrier("运动", &config->MVResourceBarrier);
                AddResourceBarrier("曝光", &config->ExposureResourceBarrier);
                AddResourceBarrier("遮罩", &config->MaskResourceBarrier);
                AddResourceBarrier("输出", &config->OutputResourceBarrier);
            }

            // HOTFIXES -----------------------------
            if (state.api == DX12)
            {
                ImGui::Spacing();
                if (auto ch = ScopedCollapsingHeader("根签名"); ch.IsHeaderOpen())
                {
                    ScopedIndent indent {};
                    ImGui::Spacing();

                    if (bool crs = config->RestoreComputeSignature.value_or_default();
                        MenuText::Checkbox("恢复计算根签名", &crs))
                        config->RestoreComputeSignature = crs;

                    if (bool grs = config->RestoreGraphicSignature.value_or_default();
                        MenuText::Checkbox("恢复图形根签名", &grs))
                        config->RestoreGraphicSignature = grs;
                }
            }
        }
    }
}

void MenuCommon::RenderLoggingSettings(RenderMenuContext& ctx)
{
    auto config = ctx.config;

    // LOGGING -----------------------------
    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader("日志"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        if (config->LogToConsole.value_or_default() || config->LogToFile.value_or_default() ||
            config->LogToNGX.value_or_default())
            spdlog::default_logger()->set_level((spdlog::level::level_enum) config->LogLevel.value_or_default());
        else
            spdlog::default_logger()->set_level(spdlog::level::off);

        if (bool toFile = config->LogToFile.value_or_default(); MenuText::CheckboxInline("写入文件", &toFile))
        {
            config->LogToFile = toFile;
            PrepareLogger();
        }

        ImGui::SameLine(0.0f, 6.0f);
        if (bool toConsole = config->LogToConsole.value_or_default(); MenuText::Checkbox("输出到控制台", &toConsole))
        {
            config->LogToConsole = toConsole;
            PrepareLogger();
        }

        const char* logLevels[] = { "详细跟踪", "调试", "信息", "警告", "错误" };
        int selectedLevel=int(config->LogLevel.value_or_default());
        if(MenuText::Segmented("日志级别",&selectedLevel,logLevels,IM_ARRAYSIZE(logLevels)))
        {
            config->LogLevel=selectedLevel;
            spdlog::default_logger()->set_level((spdlog::level::level_enum)selectedLevel);
        }
    }
}

void MenuCommon::RenderThemeSettings(RenderMenuContext& ctx)
{
    auto config = ctx.config;

    // THEME -----------------------------
    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader("菜单主题与颜色"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        bool lightTheme = config->LightTheme.value_or_default();

        const ImVec4 bgDark = lightTheme ? ImVec4(0.80f, 0.82f, 0.86f, 1.00f) : ImVec4(0.09f, 0.09f, 0.10f, 1.00f);
        const ImVec4 bgMid = lightTheme ? ImVec4(0.89f, 0.91f, 0.95f, 1.00f) : ImVec4(0.11f, 0.11f, 0.12f, 1.00f);
        const ImVec4 bgLight = lightTheme ? ImVec4(0.96f, 0.97f, 0.99f, 1.00f) : ImVec4(0.14f, 0.14f, 0.15f, 1.00f);

        auto Mix = [](const ImVec4& a, const ImVec4& b, float t, float alpha = 1.0f)
        { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, alpha); };

        auto AccentSoft = [&](ImVec4 accent, float alpha = 1.0f)
        { return toneMapColor(lightTheme ? Mix(bgLight, accent, 0.24f, alpha) : Mix(bgDark, accent, 0.32f, alpha)); };

        auto AccentMed = [&](ImVec4 accent, float alpha = 1.0f)
        { return toneMapColor(lightTheme ? Mix(bgLight, accent, 0.42f, alpha) : Mix(bgDark, accent, 0.55f, alpha)); };

        auto AccentStrong = [&](ImVec4 accent, float alpha = 1.0f)
        { return toneMapColor(ImVec4(accent.x, accent.y, accent.z, alpha)); };

        if (MenuText::Checkbox("浅色主题", &lightTheme))
        {
            config->LightTheme = lightTheme;
            RebuildStyle(ctx.menuResScale);
        }

        bool modernTheme = config->ModernTheme.value_or_default();
        if (MenuText::CheckboxInline("现代样式", &modernTheme))
        {
            config->ModernTheme = modernTheme;
            RebuildStyle(ctx.menuResScale);
            ApplyThemeFont();
        }

        ImGui::SameLine(0.0f, 6.0f);
        ShowHelpMarker("圆角分区卡片与 Inter 字体；性能叠加层保留等宽字体。");

        MenuText::SeparatorText("强调色");

        MenuText::Text("预设：");
        ImGui::SameLine(0.0f, 6.0f);

        ImVec4 colorBlue = { 0.00f, 0.40f, 0.77f, 1.0f };
        ImVec4 colorTeal = { 0.00f, 1.00f, 0.91f, 1.0f };
        ImVec4 colorGray = { 0.54f, 0.54f, 0.54f, 1.0f };
        ImVec4 colorYellow = { 1.00f, 0.89f, 0.00f, 1.0f };
        ImVec4 colorGreen = { 0.25f, 1.00f, 0.00f, 1.0f };
        ImVec4 colorRed = { 1.00f, 0.00f, 0.00f, 1.0f };
        ImVec4 colorOrange = { 1.00f, 0.52f, 0.00f, 1.0f };
        ImVec4 colorPurple = { 0.576f, 0.00f, 1.00f, 1.0f };

        ImVec4 color = {};

        color = colorBlue;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (MenuText::Button("蓝色"))
        {
            ImGui::PopStyleColor(3);

            config->MenuAccentColorR = color.x;
            config->MenuAccentColorG = color.y;
            config->MenuAccentColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorTeal;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (MenuText::Button("青色"))
        {
            ImGui::PopStyleColor(3);

            config->MenuAccentColorR = color.x;
            config->MenuAccentColorG = color.y;
            config->MenuAccentColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorGray;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (MenuText::Button("灰色"))
        {
            ImGui::PopStyleColor(3);

            config->MenuAccentColorR = color.x;
            config->MenuAccentColorG = color.y;
            config->MenuAccentColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorYellow;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (MenuText::Button("黄色"))
        {
            ImGui::PopStyleColor(3);

            config->MenuAccentColorR = color.x;
            config->MenuAccentColorG = color.y;
            config->MenuAccentColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorGreen;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (MenuText::Button("绿色"))
        {
            ImGui::PopStyleColor(3);

            config->MenuAccentColorR = color.x;
            config->MenuAccentColorG = color.y;
            config->MenuAccentColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorRed;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (MenuText::Button("红色"))
        {
            ImGui::PopStyleColor(3);

            config->MenuAccentColorR = color.x;
            config->MenuAccentColorG = color.y;
            config->MenuAccentColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorOrange;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (MenuText::Button("橙色"))
        {
            ImGui::PopStyleColor(3);

            config->MenuAccentColorR = color.x;
            config->MenuAccentColorG = color.y;
            config->MenuAccentColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorPurple;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (MenuText::Button("紫色"))
        {
            ImGui::PopStyleColor(3);

            config->MenuAccentColorR = color.x;
            config->MenuAccentColorG = color.y;
            config->MenuAccentColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        float accentColor[3] = { config->MenuAccentColorR.value_or_default(),
                                 config->MenuAccentColorG.value_or_default(),
                                 config->MenuAccentColorB.value_or_default() };

        if (MenuText::ColorEdit3("自定义强调色", accentColor))
        {
            config->MenuAccentColorR = accentColor[0];
            config->MenuAccentColorG = accentColor[1];
            config->MenuAccentColorB = accentColor[2];
            RebuildStyle(ctx.menuResScale);
        }

        ImGui::Spacing();

        if (MenuText::Button("重置强调色"))
        {
            config->MenuAccentColorR.reset();
            config->MenuAccentColorG.reset();
            config->MenuAccentColorB.reset();
            RebuildStyle(ctx.menuResScale);
        }

        ImGui::Spacing();

        MenuText::SeparatorText("背景色");

        MenuText::Text("预设：");
        ImGui::SameLine(0.0f, 6.0f);

        color = colorBlue;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (MenuText::Button("蓝色##2"))
        {
            ImGui::PopStyleColor(3);

            config->MenuBGColorR = color.x;
            config->MenuBGColorG = color.y;
            config->MenuBGColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorTeal;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (MenuText::Button("青色##2"))
        {
            ImGui::PopStyleColor(3);

            config->MenuBGColorR = color.x;
            config->MenuBGColorG = color.y;
            config->MenuBGColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorGray;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (MenuText::Button("灰色##2"))
        {
            ImGui::PopStyleColor(3);

            config->MenuBGColorR = color.x;
            config->MenuBGColorG = color.y;
            config->MenuBGColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorYellow;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (MenuText::Button("黄色##2"))
        {
            ImGui::PopStyleColor(3);

            config->MenuBGColorR = color.x;
            config->MenuBGColorG = color.y;
            config->MenuBGColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorGreen;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (MenuText::Button("绿色##2"))
        {
            ImGui::PopStyleColor(3);

            config->MenuBGColorR = color.x;
            config->MenuBGColorG = color.y;
            config->MenuBGColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorRed;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (MenuText::Button("红色##2"))
        {
            ImGui::PopStyleColor(3);

            config->MenuBGColorR = color.x;
            config->MenuBGColorG = color.y;
            config->MenuBGColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorOrange;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (MenuText::Button("橙色##2"))
        {
            ImGui::PopStyleColor(3);

            config->MenuBGColorR = color.x;
            config->MenuBGColorG = color.y;
            config->MenuBGColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine(0.0f, 6.0f);

        color = colorPurple;
        ImGui::PushStyleColor(ImGuiCol_Button, AccentSoft(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentMed(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentStrong(color));

        if (MenuText::Button("紫色##2"))
        {
            ImGui::PopStyleColor(3);

            config->MenuBGColorR = color.x;
            config->MenuBGColorG = color.y;
            config->MenuBGColorB = color.z;
            RebuildStyle(ctx.menuResScale);
        }
        else
        {
            ImGui::PopStyleColor(3);
        }

        float bgColor[3] = { config->MenuBGColorR.value_or_default(), config->MenuBGColorG.value_or_default(),
                             config->MenuBGColorB.value_or_default() };

        if (MenuText::ColorEdit3("自定义背景色", bgColor))
        {
            config->MenuBGColorR = bgColor[0];
            config->MenuBGColorG = bgColor[1];
            config->MenuBGColorB = bgColor[2];
            RebuildStyle(ctx.menuResScale);
        }

        ImGui::Spacing();

        auto alpha = config->MenuBGColorA.value_or_default();
        if (MenuText::SliderFloat("背景透明度", &alpha, 0.0f, 1.0f))
        {
            config->MenuBGColorA = alpha;
            RebuildStyle(ctx.menuResScale);
        }

        ImGui::Spacing();

        if (MenuText::Button("重置背景色"))
        {
            config->MenuBGColorR.reset();
            config->MenuBGColorG.reset();
            config->MenuBGColorB.reset();
            config->MenuBGColorA.reset();
            RebuildStyle(ctx.menuResScale);
        }

        ImGui::Spacing();
    }
}

void MenuCommon::RenderFpsOverlaySettings(RenderMenuContext& ctx)
{
    auto config = ctx.config;

    // FPS OVERLAY -----------------------------
    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader("帧率叠加层"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        bool fpsEnabled = config->ShowFps.value_or_default();
        if (MenuText::Checkbox("启用帧率叠加层", &fpsEnabled))
            config->ShowFps = fpsEnabled;
        ShowHelpMarker("可独立常开，不需要启用 FG 或 NR。Reflex 计时类型显示原神 FSR 桥的真实 GPU 耗时条；FSR 从时间轴左端起画，NR 从当前呈现条右端向右画；长度为真实已完成耗时，横向锚点仅为示意，不代表同帧 GPU 时序。没有有效样本时显示暂无数据。");

        ImGui::SameLine(0.0f, 6.0f);

        bool fpsHorizontal = config->FpsOverlayHorizontal.value_or_default();
        if (MenuText::Checkbox("横向布局", &fpsHorizontal))
            config->FpsOverlayHorizontal = fpsHorizontal;

        const char* fpsPosition[] = { "左上", "右上", "左下", "右下" };
        int overlayPosChoice = std::clamp<int>((int) config->FpsOverlayPosition.value_or_default(), 0,
                                               (int) std::size(fpsPosition) - 1);

        if (MenuText::Segmented("叠加层位置", &overlayPosChoice, fpsPosition, (int) std::size(fpsPosition)))
            config->FpsOverlayPosition = (FpsOverlayPos) overlayPosChoice;

        const char* fpsType[] = { "仅帧率", "简洁",       "详细",      "详细＋图表",
                                  "完整",     "完整＋图表", "Reflex 计时" };
        const char* selectedType = fpsType[config->FpsOverlayType.value_or_default()];

        if (MenuText::BeginCombo("叠加层类型", selectedType))
        {
            for (int n = 0; n < std::size(fpsType); n++)
            {
                if (MenuText::Selectable(fpsType[n], (config->FpsOverlayType.value_or_default() == n)))
                    config->FpsOverlayType = (FpsOverlay) n;
            }

            ImGui::EndCombo();
        }

        float fpsAlpha = config->FpsOverlayAlpha.value_or_default();
        if (MenuText::SliderFloat("背景透明度", &fpsAlpha, 0.0f, 1.0f, "%.2f"))
            config->FpsOverlayAlpha = fpsAlpha;

        const char* options[] = { "跟随菜单", "0.5", "0.6", "0.7", "0.8", "0.9", "1.0", "1.1", "1.2",
                                  "1.3",          "1.4", "1.5", "1.6", "1.7", "1.8", "1.9", "2.0" };
        int currentIndex = std::max(((int) (config->FpsScale.value_or(0.0f) * 10.0f)) - 4, 0);
        float values[] = { 0.0f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.1f, 1.2f,
                           1.3f, 1.4f, 1.5f, 1.6f, 1.7f, 1.8f, 1.9f, 2.0f };

        if (MenuText::SliderInt("缩放", &currentIndex, 0, IM_ARRAYSIZE(options) - 1, options[currentIndex],
                             ImGuiSliderFlags_ClampOnInput))
        {
            if (currentIndex == 0)
                config->FpsScale.reset();
            else
                config->FpsScale = values[currentIndex];
        }

        bool useTheme = config->OverlaysUseTheme.value_or_default();
        if (MenuText::Checkbox("使用主题颜色", &useTheme))
            config->OverlaysUseTheme = useTheme;
    }
}

void MenuCommon::RenderUpscalerInputsSettings(RenderMenuContext& ctx)
{
    auto config = ctx.config;
    auto& currentFeature = ctx.currentFeature;

    // UPSCALER INPUTS -----------------------------
    ImGui::Spacing();
    auto uiStateOpen = currentFeature == nullptr || currentFeature->IsFrozen();
    if (auto ch = ScopedCollapsingHeader("超分输入", uiStateOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        if (config->EnableFsr2Inputs.value_or_default())
        {
            bool fsr2Inputs = config->UseFsr2Inputs.value_or_default();
            bool fsr2Pattern = config->Fsr2Pattern.value_or_default();

            if (MenuText::Checkbox("使用 FSR2 输入", &fsr2Inputs))
                config->SetUseFsr2Inputs(fsr2Inputs);

            if (MenuText::Checkbox("使用 FSR2 模式匹配", &fsr2Pattern))
                config->Fsr2Pattern = fsr2Pattern;
            ShowTooltip("下次启动时生效。");
        }

        if (config->EnableFsr3Inputs.value_or_default())
        {
            bool fsr3Inputs = config->UseFsr3Inputs.value_or_default();
            bool fsr3Pattern = config->Fsr3Pattern.value_or_default();

            if (MenuText::Checkbox("使用 FSR3 输入", &fsr3Inputs))
                config->UseFsr3Inputs = fsr3Inputs;

            if (MenuText::Checkbox("使用 FSR3 模式匹配", &fsr3Pattern))
                config->Fsr3Pattern = fsr3Pattern;
            ShowTooltip("下次启动时生效。");
        }

        if (config->EnableFfxInputs.value_or_default())
        {
            bool ffxInputs = config->UseFfxInputs.value_or_default();

            if (MenuText::Checkbox("使用 FFX 输入", &ffxInputs))
                config->UseFfxInputs = ffxInputs;
        }
    }
}

void MenuCommon::RenderApiAndTextureSettings(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& currentFeature = ctx.currentFeature;
    auto& menuResScale = ctx.menuResScale;

    // DX11 & DX12 -----------------------------
    if (state.swapchainApi != Vulkan || state.swapchainInteropApi == SwapchainInteropApi::VkwDx12)
    {
        // V-SYNC -----------------------------
        ImGui::Spacing();
        if (auto ch = ScopedCollapsingHeader("垂直同步设置"); ch.IsHeaderOpen())
        {
            ScopedIndent indent {};
            ImGui::Spacing();

            auto forceVsyncOn = config->ForceVsync.has_value() && config->ForceVsync.value();
            auto forceVsyncOff = config->ForceVsync.has_value() && !config->ForceVsync.value();
            bool vsyncChanged = false;

            if (MenuText::CheckboxInline("开启垂直同步", &forceVsyncOn))
            {
                if (forceVsyncOn)
                {
                    config->ForceVsync = true;
                    vsyncChanged = true;
                }
                else
                {
                    config->ForceVsync.reset();
                    vsyncChanged = true;
                }
            }
            ImGui::SameLine(0.0f, 16.0f);

            if (MenuText::Checkbox("关闭垂直同步", &forceVsyncOff))
            {
                if (forceVsyncOff)
                {
                    config->ForceVsync = false;
                    vsyncChanged = true;
                }
                else
                {
                    config->ForceVsync.reset();
                    vsyncChanged = true;
                }
            }
            ImGui::SameLine(0.0f, 16.0f);

            ImGui::BeginDisabled(!forceVsyncOn);

            const char* vsyncIntervals[] = { "0", "1", "2", "3" };
            int vsyncInterval = std::clamp<int>((int) config->VsyncInterval.value_or_default(), 0, 3);
            if (MenuText::Segmented("同步间隔", &vsyncInterval, vsyncIntervals, IM_ARRAYSIZE(vsyncIntervals)))
            {
                config->VsyncInterval = vsyncInterval;
                vsyncChanged = true;
            }

            ShowHelpMarker("控制 DXGI 呈现等待的刷新周期。0 为立即呈现，不等垂直同步；1 为每次刷新呈现；2 以上为每隔若干次刷新呈现，会降低有效帧率。高值可能减少撕裂，但增加延迟并限制帧率。通常低延迟用 0，普通垂直同步用 1。");

            ImGui::EndDisabled();
            ImGui::SameLine(0.0f, 16.0f);

            if (MenuText::Button("重置##10"))
            {
                config->ForceVsync.reset();
                vsyncChanged = true;
            }

            ShowHelpMarker("强制垂直同步开关与同步间隔");

            // FGHooks::FGPresent sends 1 instead (Intel: XeLL, which XeFG runs with, needs sync interval 1)
            if (config->ForceVsync.value_or(false) && config->VsyncInterval.value_or_default() > 1 &&
                state.activeFgOutput == FGOutput::XeFG)
            {
                MenuText::TextDisabled("XeFG 使用同步间隔 1，运行时不采用 %d。",
                                    config->VsyncInterval.value_or_default());
            }

            if (vsyncChanged && state.activeFgOutput == FGOutput::XeFG && state.currentFG != nullptr)
            {
                // To prevent XeLL issues
                LOG_DEBUG("V-Sync change detected, forcing XeFG reset");
                state.WAR_xefgRequestFGToggle = true;
            }
        }

        // MIPMAP BIAS & Anisotropy -----------------------------
        ImGui::Spacing();
        if (auto ch = ScopedCollapsingHeader("纹理 Mipmap 偏移", (currentFeature == nullptr || currentFeature->IsFrozen())
                                                                ? ImGuiTreeNodeFlags_DefaultOpen
                                                                : 0);
            ch.IsHeaderOpen())
        {
            ScopedIndent indent {};
            ImGui::Spacing();
            if (config->MipmapBiasOverride.has_value() && _mipBias == 0.0f)
                _mipBias = config->MipmapBiasOverride.value();

            MenuText::SliderFloat("纹理 Mipmap 偏移##2", &_mipBias, -15.0f, 15.0f, "%.6f");
            ShowHelpMarker("可能改善纹理模糊。负值更锐利，正值更模糊，对性能有小幅影响。");

            ImGui::BeginDisabled(!config->MipmapBiasOverride.has_value());
            {
                ImGui::BeginDisabled(config->MipmapBiasScaleOverride.has_value() &&
                                     config->MipmapBiasScaleOverride.value());
                {
                    bool mbFixed = config->MipmapBiasFixedOverride.value_or_default();
                    if (MenuText::CheckboxInline("固定偏移覆盖", &mbFixed))
                    {
                        config->MipmapBiasScaleOverride.reset();
                        config->MipmapBiasFixedOverride = mbFixed;
                    }

                    ShowHelpMarker("所有纹理使用相同覆盖值。");
                }
                ImGui::EndDisabled();

                ImGui::SameLine(0.0f, 6.0f);

                ImGui::BeginDisabled(config->MipmapBiasFixedOverride.has_value() &&
                                     config->MipmapBiasFixedOverride.value());
                {
                    bool mbScale = config->MipmapBiasScaleOverride.value_or_default();
                    if (MenuText::Checkbox("比例偏移覆盖", &mbScale))
                    {
                        config->MipmapBiasFixedOverride.reset();
                        config->MipmapBiasScaleOverride = mbScale;
                    }

                    ShowHelpMarker("覆盖值作为比例乘数；此模式用正值提高锐度。");
                }
                ImGui::EndDisabled();

                bool mbAll = config->MipmapBiasOverrideAll.value_or_default();
                if (MenuText::Checkbox("覆盖所有纹理偏移", &mbAll))
                    config->MipmapBiasOverrideAll = mbAll;

                ShowHelpMarker("覆盖所有纹理 Mipmap 值；通常仅覆盖负偏移。");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(config->MipmapBiasOverride.has_value() &&
                                 config->MipmapBiasOverride.value() == _mipBias);
            {
                if (MenuText::Button("设置"))
                {
                    config->MipmapBiasOverride = _mipBias;
                    state.lastMipBias = 100.0f;
                    state.lastMipBiasMax = -100.0f;
                }
            }
            ImGui::EndDisabled();

            ImGui::SameLine(0.0f, 6.0f);

            ImGui::BeginDisabled(!config->MipmapBiasOverride.has_value());
            {
                if (MenuText::Button("重置"))
                {
                    config->MipmapBiasOverride.reset();
                    _mipBias = 0.0f;
                    state.lastMipBias = 100.0f;
                    state.lastMipBiasMax = -100.0f;
                }
            }
            ImGui::EndDisabled();

            if (currentFeature != nullptr && !currentFeature->IsFrozen())
            {
                ImGui::SameLine(0.0f, 6.0f);

                if (MenuText::Button("计算 Mipmap 偏移"))
                    _showMipmapCalcWindow = true;
            }

            if (config->MipmapBiasOverride.has_value())
            {
                if (config->MipmapBiasFixedOverride.value_or_default())
                {
                    MenuText::Text("当前：%.3f／%.3f，目标：%.3f", state.lastMipBias, state.lastMipBiasMax,
                                config->MipmapBiasOverride.value());
                }
                else if (config->MipmapBiasScaleOverride.value_or_default())
                {
                    MenuText::Text("当前：%.3f／%.3f，目标：基础值 × %.3f", state.lastMipBias, state.lastMipBiasMax,
                                config->MipmapBiasOverride.value());
                }
                else
                {
                    MenuText::Text("当前：%.3f／%.3f，目标：基础值 + %.3f", state.lastMipBias, state.lastMipBiasMax,
                                config->MipmapBiasOverride.value());
                }
            }
            else
            {
                MenuText::Text("当前：%.3f／%.3f", state.lastMipBias, state.lastMipBiasMax);
            }

            MenuText::Text("更改分辨率或预设后生效。");
        }
    }
}

void MenuCommon::RenderKeybindSettings(RenderMenuContext& ctx)
{
    auto config = ctx.config;

    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader("快捷键"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        MenuText::Text("目前不支持组合键。");
        MenuText::Text("按 Escape 取消，Backspace 清除绑定。");
        ImGui::Spacing();

        static auto menu = Keybind("菜单", 10);
        static auto fpsOverlay = Keybind("帧率叠加层", 11);
        static auto fpsOverlayCycle = Keybind("循环切换帧率叠加层", 12);
        static auto fgEnable = Keybind("帧生成", 13);
        static auto dlssNrToggle = Keybind("神经渲染", 14);
        static auto currentOutput = Keybind("Capture current output (DX12 / NR debug)", 15);

        menu.Render(config->ShortcutKey);
        fpsOverlay.Render(config->FpsShortcutKey);
        fpsOverlayCycle.Render(config->FpsCycleShortcutKey);
        bool fgShortcutEnabled = config->FGShortcutEnabled.value_or_default();
        if (MenuText::Checkbox("启用帧生成快捷键（默认关闭）", &fgShortcutEnabled))
        {
            config->FGShortcutEnabled = fgShortcutEnabled;
            ResetFgShortcut();
            LogFgShortcutPolicy(config, "menu-opt-in");
        }
        MenuText::TextWrapped("仅明确启用后生效；输入文本、操作菜单或捕获按键时暂停。恢复后须先松键，再重新按下并松开。");
        if (!fgShortcutEnabled)
        {
            fgEnable.CancelCapture();
            MenuText::TextWrapped("帧生成快捷键已关闭；保留原键位，启用后才可修改和触发。");
        }
        ImGui::BeginDisabled(!fgShortcutEnabled);
        fgEnable.Render(config->FGShortcutKey);
        ImGui::EndDisabled();
        dlssNrToggle.Render(config->DlssNrToggleKey);
        currentOutput.Render(config->CurrentOutputCaptureKey);
        const auto capture = DlssNr::CurrentOutputCaptureStatus();
        MenuText::TextWrapped("%s", capture.message.c_str());
        if (!capture.directory.empty()) MenuText::TextWrapped("%s", capture.directory.c_str());
        MenuText::TextWrapped("Saves selected output, including debug previews, before Opt UI. Vulkan disabled.");
    }
}

// Height of the graphs and bottom bar, measured last frame so the page view can leave room for them.
static float mainMenuFooterHeight = 0.0f;

static void RenderGenshinHookCpuTiming(Config* config, void (*showHelp)(const char*))
{
    static const bool genshinProcess = GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring());
    if (!genshinProcess) return;

    MenuText::SeparatorText("Hook CPU 计时（原神）");
    bool requested = config->AcquireGenshinControls().hookCpuTiming;
    if (MenuText::Checkbox("启用 Hook CPU 实时采样", &requested))
        config->SetGenshinHookCpuTiming(requested);
    showHelp("默认关闭。在下一次 Present 生效，无需重启；点击保存设置可记住选择。"
                   "统计约每秒输出到日志，建议短时观察后关闭。累计墙钟耗时包含等待，嵌套项不可相加，不代表 CPU 独占或 GPU 耗时。");

    // Read the actual sampler bit, rather than treating the requested config as
    // proof that a Present has already published it or samples have been taken.
    const bool sampling = GenshinHookCpuTiming::Enabled();
    if (requested != sampling)
        MenuText::TextWrapped(requested ? "已请求开启，等待下一次 Present 启用采样。"
                                        : "已请求关闭，等待下一次 Present 停止采样。");
    else
        MenuText::TextWrapped(sampling ? "内存采样已启用；需短时运行后查看 Hook CPU 日志。"
                                       : "内存采样已关闭。");
}

void MenuCommon::RenderMainMenuPages(RenderMenuContext& ctx)
{
    using MenuPages::Page;

    auto config = ctx.config;

    static Page page = Page::Upscaler;
    static bool pageLoaded = false;
    static Page lastDrawnPage = Page::Upscaler;

    if (!pageLoaded)
    {
        pageLoaded = true;
        page = MenuPages::NavigationPage(MenuPages::PageFromName(config->MenuPage.value_or("")));
        lastDrawnPage = page;
    }

    const auto select = [&](Page next)
    {
        next = MenuPages::NavigationPage(next);
        if (next != page)
        {
            page = next;
            config->MenuPage = std::string(MenuPages::Name(next));
        }
    };

    // A page another part of the menu asked for (a top-nav button): take it on this draw.
    if (const auto requested = MenuPages::ConsumeRequest())
        select(*requested);

    MenuPages::MenuVisibility() = { config->MenuShowUnfinished.value_or_default(),
                                    config->MenuShowNonessential.value_or_default() };
    // A hidden diagnostics page changes navigation only, never the selected game route.
    if (!MenuPages::IsVisiblePage(page)) select(Page::Debug);
    // Keep the accepted NEXT navigation implementation as the page owner.
    NextgenUi::RenderHeader(page);

    // Clicks use the same page authority as NR cross-links and saved MenuPage names.
    if (const auto requested = MenuPages::ConsumeRequest())
        select(*requested);

    const float footerHeight = mainMenuFooterHeight > 0.0f ? mainMenuFooterHeight :
        2.0f * ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 1.0f;
    // Let the page scroll even in a short window; a minimum pane must not push the footer offscreen.
    const float bodyHeight =
        std::max(1.0f, ImGui::GetContentRegionAvail().y - footerHeight - ImGui::GetStyle().ItemSpacing.y - 2.0f);

    // The attached secondary tabs and detail sheet share the NEXT background.
    const ImGuiChildFlags paneFlags = ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding;

    // Page pane, scrolls on its own.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.f);
    if (ImGui::BeginChild("page", ImVec2(0.0f, bodyHeight), paneFlags))
    {
        if (lastDrawnPage != page)
        {
            lastDrawnPage = page;
            ImGui::SetScrollY(0.0f);
        }

        // Each section function is called whole; openFirstHeader makes the header it starts with open by default.
        const auto section = [&](void (*render)(RenderMenuContext&), bool openFirstHeader = false)
        {
            ScopedCollapsingHeader::OpenNextHeader = openFirstHeader;
            render(ctx);
            ScopedCollapsingHeader::OpenNextHeader = false;
        };

        switch (page)
        {
        case Page::Upscaler:
        {
            const MenuText::ScopedSeparatorOmission heading("超分辨率");
            section(RenderActiveUpscalerSettings);
            section(RenderUpscalerInputsSettings, true);
            break;
        }
        case Page::FrameGeneration:
            section(RenderFrameGenerationSelection);
            section(RenderFrameGenerationRuntimeSettings);
            section(RenderFsrCommonSettings);
            break;
        case Page::Image:
            section(RenderActiveImageSettings);
            section(RenderAnisotropySettings);
            break;
        case Page::FrameGenerationAdvanced:
            section(RenderFrameGenerationAdvancedSettings);
            break;
        case Page::Framerate:
            section(RenderFramerateSettings);
#ifdef LOW_LATENCY_INPUTS
            section(RenderLowLatencySettings);
#else
            section(RenderFakenvapiSettings);
#endif
            break;
        case Page::OverlayLook:
            section(RenderFpsOverlaySettings, true);
            section(RenderMagnifierSettings, true);
            section(RenderThemeSettings, true);
            break;
        case Page::Input: // Stable input INI identity; now the Settings / Keybindings page.
            section(RenderKeybindSettings, true);
            break;
        case Page::Misc:
            section(RenderQuirksSettings, true);
            section(RenderAdvancedSettings, true);
            section(RenderLoggingSettings, true);
            section(RenderApiAndTextureSettings, true);
            break;
        case Page::Developer:
            section(RenderDeveloperSettings, true);
            break;
        case Page::ProGimi:
            section(RenderGimiSettings, true);
            break;
        case Page::ProUidAuthorization:
            DrawUid::RenderPanel();
            break;
        case Page::NrF5Low:
        case Page::NrStatus:
        case Page::NrOptions:
        case Page::NrInput:
        case Page::NrOutput:
        case Page::NrPasses:
        case Page::NrDebug:
        case Page::NrLayer1:
        case Page::NrLayer2:
        case Page::NrLayer3:
        case Page::NrUnsorted:
        case Page::NrReuse:
        case Page::Hdr:
        case Page::ProFilters:
            DlssNr::RenderMenu(ctx.config, ctx.menuResScale, page);
            break;
        case Page::Debug:
            RenderGenshinHookCpuTiming(ctx.config, ShowHelpMarker);
            DlssNr::RenderMenu(ctx.config, ctx.menuResScale, page);
            break;
        case Page::Diagnostics:
            NextgenF5Panels::RenderDiagnostics(ctx.config);
            break;
        default:
            IM_ASSERT(false && "page without a case in RenderMainMenuPages");
            break;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void MenuCommon::RenderDeveloperSettings(RenderMenuContext& ctx)
{
    auto config = ctx.config;

    bool showUnfinished = config->MenuShowUnfinished.value_or_default();
    if (MenuText::Checkbox("显示未完成项目", &showUnfinished))
        config->MenuShowUnfinished = showUnfinished;

    bool showNonessential = config->MenuShowNonessential.value_or_default();
    if (MenuText::Checkbox("显示非必要项目", &showNonessential))
        config->MenuShowNonessential = showNonessential;

    MenuText::TextWrapped("未完成：原神 BeforeSR / Auto-afterSR 选项、逐层尚未实现项。显示不会开放资源门禁。");
    MenuText::TextWrapped("非必要：诊断与性能页、NR 内核计时与亮度统计、复用调试调参。核心 NR、对比及输入预览保留。");
}

void MenuCommon::RenderMainMenuGraphs(RenderMenuContext& ctx)
{
    auto config = ctx.config;
    auto& menuResScale = ctx.menuResScale;
    auto& currentFeature = ctx.currentFeature;
    auto& frameTime = ctx.frameTime;
    auto& frameRate = ctx.frameRate;

    // First footer row: live readings and scale; the second row owns language and actions.
    ImGui::Separator();
    const auto ft = StrFmt("%7.2f ms / %6.1f fps", frameTime, frameRate);
    const auto resolution = currentFeature != nullptr && !currentFeature->IsFrozen()
        ? StrFmt("%dx%d -> %dx%d", currentFeature->RenderWidth(), currentFeature->RenderHeight(),
                 currentFeature->TargetWidth(), currentFeature->TargetHeight()) : std::string();
    auto autoText = config->MenuScale.has_value() ? "自动" : StrFmt("自动（%3.1f）", menuResScale);
    // clang-format off
    const char* uiScales[] = { autoText.c_str(), "0.5", "0.6", "0.7", "0.8", "0.9", "1.0", "1.1",
                               "1.2", "1.3", "1.4", "1.5", "1.6", "1.7", "1.8", "1.9", "2.0" };
    // clang-format on

    const char* selectedScaleName = uiScales[_selectedScale];

    const auto scalePreview = MenuText::Translate(selectedScaleName);
    const auto autoPreview = MenuText::Translate(uiScales[0]);
    const float scaleWidth = std::max(ImGui::CalcTextSize(scalePreview.c_str()).x,
                                      ImGui::CalcTextSize(autoPreview.c_str()).x) +
                             ImGui::GetFrameHeight() + 2.0f * ImGui::GetStyle().FramePadding.x;

    const float available = ImGui::GetContentRegionAvail().x;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float labelWidth = MenuText::CalcTextSize("菜单缩放").x;
    float valueWidth, resolutionWidth;
    {
        ScopedMonoFont monoFont {};
        valueWidth = ImGui::CalcTextSize(ft.c_str()).x;
        resolutionWidth = ImGui::CalcTextSize(resolution.c_str()).x;
    }
    const bool scaleInline = available >= valueWidth + scaleWidth + gap * 2.f + ImGui::GetFontSize() * 2.f;
    const bool showLabel = available >= valueWidth + scaleWidth + labelWidth + gap * 3.f + ImGui::GetFontSize() * 4.f;
    const float scaleGroupWidth = scaleInline ? scaleWidth + (showLabel ? labelWidth + gap : 0.f) : 0.f;
    const bool showResolution = !resolution.empty() && available >= valueWidth + resolutionWidth + scaleGroupWidth +
        gap * 3.f + ImGui::GetFontSize() * 8.f;
    const float graphWidth = std::max(1.f, available - valueWidth - scaleGroupWidth - gap * 2.f -
        (showResolution ? resolutionWidth + gap : 0.f));
    {
        ScopedMonoFont monoFont {};
        ImGui::PlotLines(
            "##main-menu-frame-times", [](void* rb, int idx) -> float
            { return static_cast<RingBuffer<float, plotWidth>*>(rb)->At(idx); }, &gFrameTimes, plotWidth,
            0, nullptr, FLT_MAX, FLT_MAX, ImVec2(graphWidth, ImGui::GetFrameHeight()));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ft.c_str());
        if (!showResolution && !resolution.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", resolution.c_str());
        if (showResolution)
        {
            ImGui::SameLine();
            ImGui::TextUnformatted(resolution.c_str());
        }
    }
    if (scaleInline) NextgenUi::FooterSameLineIfFits(scaleGroupWidth);
    bool scaleLabelHovered = false;
    if (showLabel)
    {
        ImGui::AlignTextToFramePadding();
        MenuText::Text("菜单缩放");
        scaleLabelHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(std::min(scaleWidth, std::max(1.f, ImGui::GetContentRegionAvail().x)));
    // Keep the original setting ID while placing its label before the compact combo.
    const bool scaleOpen = ImGui::BeginCombo("###菜单缩放", scalePreview.c_str());
    if (scaleOpen)
    {
        for (int n = 0; n < std::size(uiScales); n++)
        {
            if (MenuText::Selectable(uiScales[n], (_selectedScale == n)))
            {
                _selectedScale = n;

                if (n == 0)
                    config->MenuScale.reset();
                else
                    config->MenuScale = 0.4f + (float) n / 10.0f;
            }
        }

        ImGui::EndCombo();
    }

    if (!scaleOpen && (scaleLabelHovered || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)))
        MenuText::SetTooltip("菜单缩放");
}

void MenuCommon::RenderMainMenuBottomBar(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& io = ctx.io;

    if (NextgenUi::RenderFooterLanguage())
        nextgenThemePending = true; // Apply on the next frame, outside the live style stack.
    const bool unsaved = config->HasUnsavedSettings(GetTickCount64());
    const auto saveLabel = MenuText::StableLabel("保存设置");
    const auto saveText = saveLabel.substr(0, saveLabel.find("##"));
    const ImVec2 saveSize(ImGui::CalcTextSize(saveText.c_str()).x + 2.0f * ImGui::GetStyle().FramePadding.x,
                          ImGui::GetFrameHeight());
    const float closeWidth = MenuText::CalcTextSize("关闭").x + 2.0f * ImGui::GetStyle().FramePadding.x;
    const float actionGap = ImGui::GetStyle().ItemSpacing.x;
    const float actionsWidth = saveSize.x + actionGap + closeWidth;
    const float rowRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float nextX = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + actionGap;
    if (nextX + actionsWidth <= rowRight)
        ImGui::SameLine(rowRight - actionsWidth);
    else
        NextgenUi::FooterSameLineIfFits(saveSize.x);
    if (unsaved)
    {
        // A steady selected surface marks unsaved edits without a separate status line.
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    }
    if (ImGui::Button(saveLabel.c_str(), saveSize))
        config->SaveIni(); // Only Config's successful persistence advances the saved snapshot.
    if (unsaved) ImGui::PopStyleColor();
    if (config->LastSaveFailed() && ImGui::IsItemHovered())
        MenuText::SetTooltip("保存失败；未保存的更改仍保留，请检查文件路径及写入权限。");

    NextgenUi::FooterSameLineIfFits(closeWidth, actionGap);

    if (MenuText::Button("关闭"))
    {
        _isVisible = false;
        menuOpenRequested = false;
        menuIntentEligibleForRebuild = false;
        hasGamepad = (io.BackendFlags | ImGuiBackendFlags_HasGamepad) > 0;
        io.BackendFlags &= 30;
        io.ConfigFlags = ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoKeyboard;

        _showMipmapCalcWindow = false;
        _showHudlessWindow = false;
        io.MouseDrawCursor = false;
        io.WantCaptureKeyboard = false;
        io.WantCaptureMouse = false;
    }

    auto winSize = ImGui::GetWindowSize();
    auto winPos = ImGui::GetWindowPos();

    if (state.nvngxIniDetected)
    {
        ImGui::Spacing();
        MenuText::TextColored(toneMapColor(ImVec4(1.f, 0.f, 0.f, 1.f)),
                           "检测到 nvngx.ini，请迁移到 OptiScaler.ini 后删除旧配置。");
        ImGui::Spacing();
    }

    if (lastPosition.x < -900.0f || (lastPosition.x >= winPos.x - 1.0f && lastPosition.y >= winPos.y - 1.0f &&
                                     lastPosition.x <= winPos.x + 1.0f && lastPosition.y <= winPos.y + 1.0f))
    {
        float posX;
        float posY;

        posX = ((float) io.DisplaySize.x - winSize.x) / 2.0f;
        posY = ((float) io.DisplaySize.y - winSize.y) / 2.0f;

        // don't position menu outside of screen
        if (posX < 0.0 || posY < 0.0)
        {
            posX = 50;
            posY = 50;
        }

        ImGui::SetWindowPos(ImVec2 { posX, posY });
        lastPosition.x = posX;
        lastPosition.y = posY;
    }
}

void MenuCommon::RenderMipmapBiasWindow(RenderMenuContext& ctx, ImGuiWindowFlags flags)
{
    auto config = ctx.config;
    auto& io = ctx.io;
    auto& currentFeature = ctx.currentFeature;

    // Metrics window (for debug)
    // ImGui::ShowMetricsWindow();

    // Mipmap calculation window
    if (_showMipmapCalcWindow && currentFeature != nullptr && !currentFeature->IsFrozen() && currentFeature->IsInited())
    {
        auto posX = (io.DisplaySize.x - 450.0f) / 2.0f;
        auto posY = (io.DisplaySize.y - 200.0f) / 2.0f;

        ImGui::SetNextWindowPos(ImVec2 { posX, posY }, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2 { 450.0f, 200.0f }, ImGuiCond_FirstUseEver);

        if (_displayWidth == 0)
        {
            if (config->OutputScalingEnabled.value_or_default())
            {
                _displayWidth = static_cast<uint32_t>(currentFeature->DisplayWidth() *
                                                      config->OutputScalingMultiplier.value_or_default());
            }
            else
            {
                _displayWidth = currentFeature->DisplayWidth();
            }

            _renderWidth = static_cast<uint32_t>(_displayWidth / 3.0f);
            _mipmapUpscalerQuality = 0;
            _mipmapUpscalerRatio = 3.0f;
            _mipBiasCalculated = log2((float) _renderWidth / (float) _displayWidth);
        }

        if (MenuText::Begin("纹理 Mipmap 偏移", nullptr, flags))
        {
            if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
                ImGui::SetWindowFocus();

            if (MenuText::InputScalar("显示宽度", ImGuiDataType_U32, &_displayWidth, NULL, NULL, "%u"))
            {
                if (_displayWidth <= 0)
                {
                    if (config->OutputScalingEnabled.value_or_default())
                    {
                        _displayWidth = static_cast<uint32_t>(currentFeature->DisplayWidth() *
                                                              config->OutputScalingMultiplier.value_or_default());
                    }
                    else
                    {
                        _displayWidth = currentFeature->DisplayWidth();
                    }
                }

                _renderWidth = static_cast<uint32_t>(_displayWidth / _mipmapUpscalerRatio);
                _mipBiasCalculated = log2((float) _renderWidth / (float) _displayWidth);
            }

            const char* q[] = { "极致性能", "性能", "均衡", "质量", "超高质量", "DLAA" };
            float fr[] = { 3.0f, 2.0f, 1.7f, 1.5f, 1.3f, 1.0f };
            auto configQ = _mipmapUpscalerQuality;

            const char* selectedQ = q[configQ];

            ImGui::BeginDisabled(config->UpscaleRatioOverrideEnabled.value_or_default());

            if (MenuText::BeginCombo("超分质量", selectedQ))
            {
                for (int n = 0; n < 6; n++)
                {
                    if (MenuText::Selectable(q[n], (_mipmapUpscalerQuality == n)))
                    {
                        _mipmapUpscalerQuality = n;

                        float ov = -1.0f;

                        if (config->QualityRatioOverrideEnabled.value_or_default())
                        {
                            switch (n)
                            {
                            case 0:
                                ov = config->QualityRatio_UltraPerformance.value_or(-1.0f);
                                break;

                            case 1:
                                ov = config->QualityRatio_Performance.value_or(-1.0f);
                                break;

                            case 2:
                                ov = config->QualityRatio_Balanced.value_or(-1.0f);
                                break;

                            case 3:
                                ov = config->QualityRatio_Quality.value_or(-1.0f);
                                break;

                            case 4:
                                ov = config->QualityRatio_UltraQuality.value_or(-1.0f);
                                break;
                            }
                        }

                        if (ov > 0.0f)
                            _mipmapUpscalerRatio = ov;
                        else
                            _mipmapUpscalerRatio = fr[n];

                        _renderWidth = static_cast<uint32_t>(_displayWidth / _mipmapUpscalerRatio);
                        _mipBiasCalculated = log2((float) _renderWidth / (float) _displayWidth);
                    }
                }

                ImGui::EndCombo();
            }

            ImGui::EndDisabled();

            auto minLimit = config->ExtendedLimits.value_or_default() ? 0.1f : 1.0f;
            auto maxLimit = config->ExtendedLimits.value_or_default() ? 6.0f : 3.0f;
            if (MenuText::SliderFloat("超分比例", &_mipmapUpscalerRatio, minLimit, maxLimit, "%.2f"))
            {
                _renderWidth = static_cast<uint32_t>(_displayWidth / _mipmapUpscalerRatio);
                _mipBiasCalculated = log2((float) _renderWidth / (float) _displayWidth);
            }

            if (MenuText::InputScalar("渲染宽度", ImGuiDataType_U32, &_renderWidth, NULL, NULL, "%u"))
                _mipBiasCalculated = log2((float) _renderWidth / (float) _displayWidth);

            MenuText::SliderFloat("纹理 Mipmap 偏移", &_mipBiasCalculated, -15.0f, 0.0f, "%.6f");

            // BOTTOM LINE
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::SameLine();
            ImGui::Spacing();

            constexpr float spacing = 6.0f;
            auto textSize = MenuText::CalcTextSize("使用数值");
            textSize += MenuText::CalcTextSize("关闭");
            textSize.x += ImGui::GetStyle().FramePadding.x * 5.0f + spacing; // 2 sides * 2 buttons + 1

            float avail = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - textSize.x);

            if (MenuText::Button("使用数值"))
            {
                _mipBias = _mipBiasCalculated;
                _showMipmapCalcWindow = false;
            }

            ImGui::SameLine(0.0f, spacing);

            if (MenuText::Button("关闭"))
                _showMipmapCalcWindow = false;

            ImGui::Spacing();
            ImGui::Separator();

            ImGui::End();
        }
    }
}

void MenuCommon::RenderHudlessResourcesWindow(RenderMenuContext& ctx, ImGuiWindowFlags flags)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& io = ctx.io;

    auto fg = state.currentFG;
    if (_showHudlessWindow && config->FGHUDFix.value_or_default() && fg != nullptr && fg->IsActive())
    {
        auto posX = (io.DisplaySize.x - 400.0f) / 2.0f;
        auto posY = (io.DisplaySize.y - 300.0f) / 2.0f;

        ImGui::SetNextWindowPos(ImVec2 { posX, posY }, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2 { 400.0f, 300.0f });

        if (MenuText::Begin("去 HUD 资源", nullptr, flags))
        {
            if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
                ImGui::SetWindowFocus();

            int btnCount = 100;

            if (MenuText::BeginTable("HUDlessTable", 2, ImGuiTableFlags_SizingFixedFit))
            {
                MenuText::TableSetupColumn("##1", ImGuiTableColumnFlags_WidthStretch);
                MenuText::TableSetupColumn("##2", ImGuiTableColumnFlags_WidthFixed);

                ankerl::unordered_dense::map<void*, CapturedHudlessInfo>::iterator it;

                for (it = state.capturedHudlesses.begin(); it != state.capturedHudlesses.end(); it++)
                {
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);

                    MenuText::Text("%08x，%s->%s，次数：%llu，%s", (size_t) it->first,
                                GetSourceString(it->second.captureInfo & 0xFF).c_str(),
                                GetDispatchString(it->second.captureInfo & 0xFF00).c_str(), it->second.usageCount,
                                it->second.enabled ? "活跃" : "不活跃");

                    ImGui::TableSetColumnIndex(1);

                    btnCount++;
                    std::string text;

                    if (it->second.enabled)
                        text = StrFmt("禁用##%d", btnCount);
                    else
                        text = StrFmt("启用##%d", btnCount);

                    if (MenuText::Button(text.c_str()))
                    {
                        LOG_DEBUG("HUDless {:X}: {}", (size_t) it->first,
                                  it->second.enabled ? "正在禁用" : "正在启用");
                        it->second.enabled = !it->second.enabled;
                    }
                }

                ImGui::EndTable();
            }

            if (MenuText::Button("清空##4"))
            {
                LOG_DEBUG("Clearing captured HUDless resources");
                state.clearCapturedHudlesses = true;
            }

            ImGui::SameLine(0.0f, 8.0f);

            if (MenuText::Button("关闭##4"))
                _showHudlessWindow = false;

            ImGui::End();
        }
    }
}

void MenuCommon::RenderMainMenuWindow(RenderMenuContext& ctx)
{
    auto& state = ctx.state;
    auto config = ctx.config;
    auto& frameTime = ctx.frameTime;
    auto& frameRate = ctx.frameRate;
    auto& frameTimesCalculated = ctx.frameTimesCalculated;
    auto& menuResScale = ctx.menuResScale;

    if (!_isVisible)
        return;

    // Apply the persisted theme before NEXT pushes its temporary style overrides.
    if (nextgenRebuildPending)
    {
        nextgenRebuildPending = false;
        nextgenThemePending = false;
        RebuildStyle(nextgenPendingScale);
    }
    else if (nextgenThemePending)
    {
        nextgenThemePending = false;
        RebuildStyle(menuResScale);
    }

    // Overlay font
    if (config->UseHQFont.value_or_default())
        ImGui::PushFontSize(std::round(menuResScale * fontSize));

    // If overlay is not visible frame needs to be inited
    if (!frameTimesCalculated)
    {
        float frameCnt = 0;
        frameTime = 0;
        for (size_t i = 299; i > 199; i--)
        {
            if (state.frameTimes[i] > 0.0)
            {
                frameTime += state.frameTimes[i];
                frameCnt++;
            }
        }

        frameTime /= frameCnt;
        frameRate = 1000.0 / frameTime;
    }

    // The detached windows below keep auto-resizing; the main window is resizable and scrolls inside its page pane.
    ImGuiWindowFlags flags = 0;
    flags |= ImGuiWindowFlags_NoSavedSettings;
    flags |= ImGuiWindowFlags_NoCollapse;
    flags |= ImGuiWindowFlags_AlwaysAutoResize;

    ImGuiWindowFlags mainFlags = (flags & ~ImGuiWindowFlags_AlwaysAutoResize) | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoTitleBar;

    // Last size of the main window in pixels; zero until it has been drawn once.
    static ImVec2 lastWindowSize {};

    ImVec2 wantedSize { config->MenuWidth.value_or(960.0f) * menuResScale,
                        config->MenuHeight.value_or(680.0f) * menuResScale };
    // The window can't scroll, so every size it takes (first use, rescale, drag, minimum) stays on screen.
    const ImVec2 maxSize { ctx.io.DisplaySize.x > 0.0f ? ctx.io.DisplaySize.x * 0.95f : FLT_MAX,
                           ctx.io.DisplaySize.y > 0.0f ? ctx.io.DisplaySize.y * 0.95f : FLT_MAX };
    const ImVec2 minSize { std::min(640.0f * menuResScale, maxSize.x), std::min(420.0f * menuResScale, maxSize.y) };
    wantedSize.x = std::clamp(wantedSize.x, minSize.x, maxSize.x);
    wantedSize.y = std::clamp(wantedSize.y, minSize.y, maxSize.y);
    ImGui::SetNextWindowSizeConstraints(minSize, maxSize);
    ImGui::SetNextWindowSize(wantedSize, ImGuiCond_FirstUseEver);

    if (lastWindowSize.x <= 0.0f)
        lastWindowSize = wantedSize;

    if (lastMenuScale != menuResScale)
    {
        const float oldMenuScale = lastMenuScale;
        lastMenuScale = menuResScale;

        // if UI scale is changed rescale the style
        RebuildStyle(menuResScale);

        // Keep the window the same size relative to the text.
        if (oldMenuScale > 0.0f)
        {
            lastWindowSize = { std::clamp(lastWindowSize.x * menuResScale / oldMenuScale, minSize.x, maxSize.x),
                               std::clamp(lastWindowSize.y * menuResScale / oldMenuScale, minSize.y, maxSize.y) };
            ImGui::SetNextWindowSize(lastWindowSize, ImGuiCond_Always);
        }
    }

    // NEXT presentation is a scope around the original F5 pages; feature code remains untouched.
    struct NextgenStyleActivity
    {
        NextgenStyleActivity() { nextgenStyleActive = true; }
        ~NextgenStyleActivity() { nextgenStyleActive = false; }
    } nextgenStyleActivity;
    NextgenUi::StyleScope nextgenStyle(config->LightTheme.value_or_default());

    // Main menu window (NoTitleBar): keep the original name and control ID seed.
    if (windowTitle.empty())
    {
        windowTitle = StrFmt("%s - %s %s %s %s", VER_PRODUCT_NAME, state.gameExe.c_str(),
                             state.gameName.empty() ? "" : StrFmt("- %s", state.gameName.c_str()).c_str(),
                             (state.detectedQuirks.size() > 0) ? "(Q)" : "", state.isOptiPatcherSucceed ? "(OP)" : "");
    }

    if (MenuText::Begin(windowTitle.c_str(), NULL, mainFlags))
    {
        const auto criticalPos = ImGui::GetWindowPos();
        const auto criticalSize = ImGui::GetWindowSize();
        criticalMainMenuBounds = { criticalPos.x, criticalPos.y, criticalSize.x, criticalSize.y };
        // Remember the size (relative to Menu Scale) for Save Settings.
        const ImVec2 windowSize = ImGui::GetWindowSize();
        if (std::fabs(windowSize.x - lastWindowSize.x) > 0.5f || std::fabs(windowSize.y - lastWindowSize.y) > 0.5f)
        {
            lastWindowSize = windowSize;
            config->MenuWidth = windowSize.x / menuResScale;
            config->MenuHeight = windowSize.y / menuResScale;
        }

        // Header/status messages shown above the navigation and page panes.
        RenderMainMenuHeaderMessages(ctx);

        // Navigation pane and the selected page.
        RenderMainMenuPages(ctx);

        // Diagnostics and footer actions below the page, full width.
        const float footerTop = ImGui::GetCursorPosY();
        RenderMainMenuGraphs(ctx);
        RenderMainMenuBottomBar(ctx);
        mainMenuFooterHeight = ImGui::GetCursorPosY() - footerTop;

        ImGui::End();
    }

    // Detached utility windows owned by the main menu.
    RenderMipmapBiasWindow(ctx, flags);
    RenderHudlessResourcesWindow(ctx, flags);

    if (config->UseHQFont.value_or_default())
        ImGui::PopFontSize();
}

void KeyUp(UINT vKey)
{
    inputMenu = vKey == Config::Instance()->ShortcutKey.value_or_default();
    inputFps = vKey == Config::Instance()->FpsShortcutKey.value_or_default();
    // A release-only callback cannot establish a fresh press. FG is produced solely by
    // UpdateManualInput's opted-in press/release path, including on UWP.
    // Leave FG state to the sampling/consumer path; this callback may run off the menu thread.
    inputFpsCycle = vKey == Config::Instance()->FpsCycleShortcutKey.value_or_default();
}

// The lamp, and only the lamp.
//
// Red for dark, green for full light, with its reading beside it. No status sentence: the whole
// point of a light meter is that it is read at a glance while playing, and a paragraph in the corner
// of somebody's game is not that. Everything wordy lives in the menu, which is where someone has
// already decided to stop and read.
//
// Drawn only when its own setting is on. An overlay that appears because a scan happens to be
// running is an overlay nobody asked for.
void RenderExposureScanIndicator(float alpha)
{
    using DlssNr::ExposureScan::Verdict;

    if (!Config::Instance()->DlssNrScanMeter.value_or_default())
        return;

    if (DlssNr::ExposureScan::Where() == Verdict::Off)
        return;

    int which = 0;
    float low = 0.0f, high = 0.0f;
    const float now = DlssNr::ExposureScan::BestValue(&which, &low, &high);

    // Nothing found yet, or no range to place it in: a dim lamp, which says "watching, no reading"
    // without saying it in words.
    const bool reading = now > 0.0f && high > low;

    float lit = 0.0f;

    if (reading)
    {
        // An exposure falls as the scene brightens, so the value reads backwards unless the buffer
        // holds the reciprocal -- the same question the anchor asks, answered from the same setting,
        // because a lamp contradicting the picture would be worse than no lamp.
        lit = (high - now) / (high - low);

        if (Config::Instance()->DlssNrScanInverted.value_or_default())
            lit = 1.0f - lit;

        lit = lit < 0.0f ? 0.0f : (lit > 1.0f ? 1.0f : lit);
    }

    // Red to amber to green. A straight red-to-green fade passes through a muddy brown at the
    // midpoint, and the midpoint is where most of a session is spent.
    const ImVec4 dark(0.90f, 0.22f, 0.20f, 1.0f);
    const ImVec4 mid(0.95f, 0.75f, 0.20f, 1.0f);
    const ImVec4 bright(0.35f, 0.88f, 0.38f, 1.0f);
    const ImVec4 idle(0.45f, 0.45f, 0.45f, 1.0f);

    ImVec4 lamp = idle;

    if (reading)
    {
        const float t = lit < 0.5f ? lit * 2.0f : (lit - 0.5f) * 2.0f;
        const ImVec4& a = lit < 0.5f ? dark : mid;
        const ImVec4& b = lit < 0.5f ? mid : bright;
        lamp = ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 12.0f, vp->WorkPos.y + 12.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(alpha);

    if (MenuText::Begin("DlssNrExposureScan", nullptr,
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove))
    {
        const float r = ImGui::GetFontSize() * 0.38f;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 centre(at.x + r, at.y + ImGui::GetTextLineHeight() * 0.5f);

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddCircleFilled(centre, r, ImGui::GetColorU32(lamp), 20);
        draw->AddCircle(centre, r, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.6f)), 20, 1.5f);

        ImGui::Dummy(ImVec2(r * 2.0f + 6.0f, ImGui::GetTextLineHeight()));
        ImGui::SameLine();

        ScopedMonoFont monoFont {};

        if (reading)
            MenuText::TextColored(lamp, "%3.0f%%  %.5f", lit * 100.0f, now);
        else
            MenuText::TextColored(idle, "--");
    }

    ImGui::End();
}

bool MenuCommon::RenderMenu()
{
    if (!_isInited)
        return false;

    RenderMenuContext ctx { State::Instance(), Config::Instance(), ImGui::GetIO() };
    ctx.now = Util::MillisecondsNow();
    ctx.currentFeature = ctx.state.currentFeature;

    // 1) Collect timing and input state before any ImGui drawing.
    UpdateRenderTiming(ctx);
    UpdateMenuInputMode(ctx);
    HandleMenuShortcuts(ctx);

    // 2) Prepare one-shot notifications and start a new ImGui frame only when needed.
    UpdateVersionAndStartupNotifications(ctx);
    UpdateGenshinCriticalHints(ctx.state, ctx.config);
    // This branch performs no ImGui drawing; a newly queued F5Low toast must demand this frame.
    if (!ctx.config->IsStarRailInputProfile())
        DlssNr::UpdateF5LowHint(ctx.config);
    BeginMenuFrameIfNeeded(ctx);
    OptiInput::EndFrame(_isVisible);

    // Only these windows consume GPU identity; notification-only/no-draw calls
    // do not fetch it. Any due refresh finishes before the frame borrow begins.
    // Nested calls keep their parent borrow; this scope never refreshes it.
    if (_isVisible || ctx.config->ShowFps.value_or_default())
        ctx.primaryGpu = &MenuPrimaryGpu();
    MenuGpuFrameScope gpuFrame(ctx.primaryGpu);

    // 3) Draw lightweight overlay windows first, preserving the original order.
    ctx.menuResScale = MenuResolutionScale(ctx.io);
    RenderSplashWindow(ctx);
    RenderNotifications(ctx);
    UpdateFrameTimeAverages(ctx);
    RenderPerformanceOverlay(ctx);
    RenderExposureScanIndicator(ctx.config->FpsOverlayAlpha.value_or_default());

    // 4) Draw the full settings menu last so popups and child windows keep their existing behavior.
    RenderMainMenuWindow(ctx);

    if (ctx.newFrame)
        ImGui::EndFrame();

    return ctx.newFrame;
}

void MenuCommon::Init(HWND InHwnd, bool isUWP)
{
    ++g_menuGpuLifetime;
    ResetFgShortcut();
    LogFgShortcutPolicy(Config::Instance(), "menu-init");
    // Process shutdown is terminal; rebuilding a menu must not reopen resource admission.

    HWND oldHandle = nullptr;

    if (_handle != nullptr)
    {
        oldHandle = _handle;
        LOG_DEBUG("Old Handle: {:X}, ImGui Handle: {:X}", (size_t) oldHandle,
                  (size_t) ImGui::GetMainViewport()->PlatformHandleRaw);
    }

    _handle = InHwnd;
    const bool authenticatedRebuild = IsAuthenticatedGenshinRebuildWindow(InHwnd) ||
        menuIntentEligibleForRebuild;
    _isVisible = authenticatedRebuild && menuOpenRequested;
    menuIntentEligibleForRebuild = authenticatedRebuild;
    _isUWP = isUWP;
    lastPosition = { -1000.0f, -1000.0f };

    LOG_DEBUG("Handle: {0:X}", (size_t) _handle);

    // In case d3d12 wasn't yet used up to this point, try to update GPU info late here
    IdentifyGpu::updateD3d12Capabilities();

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    (void) io;

    hasGamepad = (io.BackendFlags | ImGuiBackendFlags_HasGamepad) > 0;
    io.BackendFlags &= 30;
    io.ConfigFlags = ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoKeyboard;

    io.MouseDrawCursor = _isVisible;
    io.WantCaptureKeyboard = _isVisible;
    io.WantCaptureMouse = _isVisible;
    io.WantSetMousePos = _isVisible;

    io.IniFilename = io.LogFilename = nullptr;

    bool initResult = false;

    if (io.BackendPlatformUserData == nullptr)
    {
        if (!isUWP)
        {
            // The retained FG child is an output surface, not a second input/focus owner.
            initResult = ImGui_ImplWin32_Init(ResolveMenuInputOwner(InHwnd));
            LOG_DEBUG("ImGui_ImplWin32_Init result: {0}", initResult);
        }
        else
        {
            initResult = ImGui_ImplUwp_Init(InHwnd);
            ImGui_BindUwpKeyUp(KeyUp);
            LOG_DEBUG("ImGui_ImplUwp_Init result: {0}", initResult);
        }
    }

    if (io.Fonts->Fonts.empty())
    {
        LOG_INFO("NEXT font init: atlas empty, loading theme fonts");
        gHackFont = gInterFont = gClassicFont = nullptr;

        if (Config::Instance()->UseHQFont.value_or_default())
        {
            ImFontAtlas* atlas = io.Fonts;
            atlas->Clear();

            // This automatically becomes the next default font
            ImFontConfig fontConfig;

            if (Config::Instance()->FontSize.has_value())
                fontSize = Config::Instance()->FontSize.value();

            if (Config::Instance()->TTFFontPath.has_value())
            {
                gClassicFont =
                    atlas->AddFontFromFileTTF(wstring_to_string(Config::Instance()->TTFFontPath.value()).c_str(),
                                              fontSize, &fontConfig, io.Fonts->GetGlyphRangesDefault());
            }
            else
            {
                gHackFont = atlas->AddFontFromMemoryCompressedBase85TTF(hack_compressed_compressed_data_base85,
                                                                        fontSize, &fontConfig);
                gClassicFont = gHackFont;

                // Modern style text; Hack stays loaded for the live numbers.
                gInterFont = atlas->AddFontFromMemoryCompressedBase85TTF(inter_compressed_compressed_data_base85,
                                                                         fontSize, &fontConfig);
            }

            io.FontDefault = gClassicFont;

            // NEXT: merge the CJK glyphs the Chinese UI uses into every theme font.
            // This ImGui has no cross-font fallback (a missing glyph renders as '?'),
            // so a standalone fallback font never gets used; merging is the only route.
            // The codepoints are numbers, not literals: this source must stay ASCII-safe
            // because the execution charset on this machine is GBK.
            if (GetFileAttributesW(L"C:\\Windows\\Fonts\\msyh.ttc") != INVALID_FILE_ATTRIBUTES)
            {
                static const unsigned int kCjkCodepoints[] = {
                    0x00D7, 0x2013, 0x2014, 0x201C, 0x201D, 0x2026, 0x2192, 0x2500, 0x3001, 0x3002, 0x4E00, 0x4E09,
                    0x4E0A, 0x4E0B, 0x4E0D, 0x4E0E, 0x4E13, 0x4E14, 0x4E16, 0x4E22, 0x4E24, 0x4E2A, 0x4E2D, 0x4E3A,
                    0x4E3B, 0x4E45, 0x4E48, 0x4E49, 0x4E4B, 0x4E4E, 0x4E58, 0x4E5F, 0x4E60, 0x4E86, 0x4E8B, 0x4E8C,
                    0x4E8E, 0x4E92, 0x4E94, 0x4E9B, 0x4EA4, 0x4EA7, 0x4EAB, 0x4EAE, 0x4EC0, 0x4EC5, 0x4ECA, 0x4ECD,
                    0x4ECE, 0x4ED6, 0x4EE3, 0x4EE4, 0x4EE5, 0x4EEA, 0x4EEC, 0x4EF6, 0x4EFB, 0x4F11, 0x4F18, 0x4F1A,
                    0x4F20, 0x4F2A, 0x4F30, 0x4F3C, 0x4F3D, 0x4F46, 0x4F4D, 0x4F4E, 0x4F4F, 0x4F53, 0x4F55, 0x4F59,
                    0x4F5C, 0x4F60, 0x4F73, 0x4F7F, 0x4F8B, 0x4F9B, 0x4FA7, 0x4FBF, 0x4FDD, 0x4FE1, 0x4FEE, 0x500D,
                    0x5019, 0x503C, 0x504F, 0x505C, 0x50A8, 0x50CF, 0x5141, 0x5143, 0x5145, 0x5148, 0x5149, 0x514D,
                    0x5165, 0x5168, 0x5171, 0x5173, 0x5176, 0x5177, 0x5178, 0x517C, 0x5185, 0x518D, 0x5199, 0x51B2,
                    0x51B3, 0x51BB, 0x51C6, 0x51CF, 0x51E0, 0x51ED, 0x51FA, 0x51FB, 0x5206, 0x5207, 0x5212, 0x5217,
                    0x5219, 0x521A, 0x521B, 0x521D, 0x5220, 0x5224, 0x5229, 0x522B, 0x5230, 0x5236, 0x5237, 0x524D,
                    0x526A, 0x526F, 0x5272, 0x529B, 0x529F, 0x52A0, 0x52A8, 0x52A9, 0x52BF, 0x52FE, 0x52FF, 0x5305,
                    0x5316, 0x5339, 0x533A, 0x5347, 0x534A, 0x5355, 0x5360, 0x5361, 0x5370, 0x5373, 0x5374, 0x5386,
                    0x538B, 0x539F, 0x53BB, 0x53C2, 0x53C8, 0x53CA, 0x53CC, 0x53CD, 0x53D1, 0x53D6, 0x53D7, 0x53D8,
                    0x53E0, 0x53E3, 0x53E6, 0x53EA, 0x53EF, 0x53F0, 0x53F2, 0x53F3, 0x53F7, 0x5404, 0x5408, 0x540C,
                    0x540D, 0x540E, 0x5411, 0x5417, 0x5426, 0x5427, 0x542B, 0x542F, 0x5438, 0x5448, 0x544A, 0x5468,
                    0x547D, 0x548C, 0x54C1, 0x54CD, 0x5584, 0x5668, 0x566A, 0x56DB, 0x56DE, 0x56E0, 0x56F4, 0x56FA,
                    0x56FE, 0x5706, 0x5728, 0x573A, 0x5747, 0x574F, 0x5757, 0x5766, 0x5782, 0x578B, 0x57DF, 0x57FA,
                    0x5806, 0x586B, 0x5883, 0x5899, 0x589E, 0x58F0, 0x5904, 0x5907, 0x590D, 0x5916, 0x591A, 0x591F,
                    0x5927, 0x5929, 0x592E, 0x5931, 0x5934, 0x5947, 0x597D, 0x5982, 0x5999, 0x59A5, 0x59CB, 0x5B50,
                    0x5B57, 0x5B58, 0x5B66, 0x5B83, 0x5B88, 0x5B89, 0x5B8C, 0x5B98, 0x5B9A, 0x5B9E, 0x5BB6, 0x5BB9,
                    0x5BBD, 0x5BC6, 0x5BDF, 0x5BF8, 0x5BF9, 0x5BFB, 0x5BFC, 0x5C04, 0x5C06, 0x5C0F, 0x5C11, 0x5C1A,
                    0x5C1D, 0x5C31, 0x5C3A, 0x5C3D, 0x5C3E, 0x5C40, 0x5C42, 0x5C4F, 0x5C55, 0x5C5E, 0x5D29, 0x5DE5,
                    0x5DE6, 0x5DE7, 0x5DEE, 0x5DF1, 0x5DF2, 0x5E03, 0x5E26, 0x5E27, 0x5E38, 0x5E45, 0x5E55, 0x5E72,
                    0x5E73, 0x5E74, 0x5E76, 0x5E7B, 0x5E8F, 0x5E93, 0x5E94, 0x5EA6, 0x5ED3, 0x5EF6, 0x5EFA, 0x5F00,
                    0x5F02, 0x5F03, 0x5F0F, 0x5F15, 0x5F20, 0x5F31, 0x5F3A, 0x5F53, 0x5F55, 0x5F62, 0x5F69, 0x5F71,
                    0x5F80, 0x5F84, 0x5F85, 0x5F88, 0x5F97, 0x5FAA, 0x5FAE, 0x5FD7, 0x5FD8, 0x5FD9, 0x5FEB, 0x5FFD,
                    0x6001, 0x6027, 0x603B, 0x6062, 0x606F, 0x60C5, 0x60F3, 0x610F, 0x611F, 0x613F, 0x6162, 0x620F,
                    0x6210, 0x6211, 0x6216, 0x622A, 0x6240, 0x624B, 0x624D, 0x6253, 0x6267, 0x6269, 0x626B, 0x627E,
                    0x627F, 0x628A, 0x6291, 0x6296, 0x6297, 0x62A4, 0x62A5, 0x62D0, 0x62D2, 0x62D3, 0x62D6, 0x62DF,
                    0x62E9, 0x6301, 0x6302, 0x6307, 0x6309, 0x6321, 0x632F, 0x6355, 0x635F, 0x6362, 0x636E, 0x6377,
                    0x6389, 0x6392, 0x63A2, 0x63A5, 0x63A7, 0x63A8, 0x63CF, 0x63D0, 0x63D2, 0x641C, 0x642C, 0x642D,
                    0x6495, 0x64CE, 0x652F, 0x6536, 0x6539, 0x653E, 0x6548, 0x654F, 0x6563, 0x6570, 0x6574, 0x6587,
                    0x6591, 0x65A5, 0x65AD, 0x65B0, 0x65B9, 0x65C1, 0x65CB, 0x65E0, 0x65E5, 0x65E7, 0x65E9, 0x65F6,
                    0x660E, 0x660F, 0x6613, 0x6620, 0x662F, 0x663E, 0x6655, 0x665A, 0x666E, 0x666F, 0x6670, 0x667A,
                    0x6682, 0x6696, 0x6697, 0x66DD, 0x66F2, 0x66F4, 0x66FF, 0x6700, 0x6709, 0x671F, 0x672A, 0x672C,
                    0x673A, 0x6742, 0x6743, 0x6761, 0x6765, 0x677E, 0x6781, 0x6784, 0x679C, 0x67D0, 0x67D3, 0x67D4,
                    0x67E5, 0x6807, 0x6821, 0x6837, 0x6838, 0x6839, 0x683C, 0x6846, 0x6848, 0x6863, 0x6865, 0x68AF,
                    0x68C0, 0x69FD, 0x6A21, 0x6A2A, 0x6A59, 0x6B21, 0x6B27, 0x6B3E, 0x6B62, 0x6B63, 0x6B64, 0x6B65,
                    0x6B7B, 0x6B8B, 0x6BB5, 0x6BCF, 0x6BD4, 0x6BEB, 0x6C34, 0x6C38, 0x6C42, 0x6CA1, 0x6CBF, 0x6CD5,
                    0x6CE2, 0x6CE8, 0x6D01, 0x6D0B, 0x6D3B, 0x6D41, 0x6D45, 0x6D4B, 0x6D4F, 0x6D6E, 0x6D88, 0x6DE1,
                    0x6DF1, 0x6DF7, 0x6DFB, 0x6E05, 0x6E29, 0x6E32, 0x6E38, 0x6E83, 0x6E90, 0x6ED1, 0x6EDE, 0x6EE1,
                    0x6EE4, 0x6F0F, 0x6F14, 0x6FC0, 0x706F, 0x7070, 0x70B9, 0x70C1, 0x70C8, 0x70ED, 0x7115, 0x7136,
                    0x7167, 0x7231, 0x7247, 0x7248, 0x7269, 0x7279, 0x72B6, 0x72EC, 0x7387, 0x73A9, 0x73AF, 0x73B0,
                    0x73BB, 0x7406, 0x7483, 0x74F6, 0x751F, 0x7528, 0x7531, 0x7535, 0x753B, 0x7545, 0x754C, 0x7559,
                    0x7565, 0x767B, 0x767D, 0x767E, 0x7684, 0x76AE, 0x76D6, 0x76EE, 0x76EF, 0x76F4, 0x76F8, 0x7701,
                    0x770B, 0x771F, 0x7720, 0x773C, 0x7740, 0x775B, 0x77E2, 0x77E5, 0x77E9, 0x77ED, 0x7801, 0x7840,
                    0x786C, 0x786E, 0x793A, 0x795E, 0x7981, 0x79CD, 0x79D2, 0x79D8, 0x79EF, 0x79F0, 0x79FB, 0x7A0B,
                    0x7A0D, 0x7A33, 0x7A7A, 0x7A81, 0x7A97, 0x7ACB, 0x7AEF, 0x7B11, 0x7B26, 0x7B2C, 0x7B49, 0x7B7E,
                    0x7B80, 0x7B97, 0x7C7B, 0x7C89, 0x7C92, 0x7C97, 0x7CBE, 0x7CCA, 0x7CFB, 0x7D20, 0x7D22, 0x7D2B,
                    0x7D2F, 0x7E41, 0x7EA2, 0x7EA6, 0x7EA7, 0x7EAF, 0x7EB3, 0x7EB8, 0x7EB9, 0x7EBF, 0x7EC4, 0x7EC6,
                    0x7EC8, 0x7ECF, 0x7ED1, 0x7ED3, 0x7ED8, 0x7ED9, 0x7EDC, 0x7EDD, 0x7EDF, 0x7EE7, 0x7EEA, 0x7EED,
                    0x7EFF, 0x7F13, 0x7F16, 0x7F18, 0x7F29, 0x7F3A, 0x7F51, 0x7F69, 0x7F6E, 0x7FFB, 0x8003, 0x8005,
                    0x800C, 0x8017, 0x80A4, 0x80C0, 0x80C6, 0x80CC, 0x80FD, 0x81A8, 0x81EA, 0x81F3, 0x81F4, 0x8272,
                    0x8282, 0x82B1, 0x82E5, 0x8303, 0x8350, 0x83B7, 0x83DC, 0x843D, 0x84DD, 0x853D, 0x85CF, 0x865A,
                    0x884C, 0x8861, 0x8865, 0x8868, 0x8870, 0x88AB, 0x88C1, 0x88C2, 0x88C5, 0x892A, 0x8981, 0x8986,
                    0x89C1, 0x89C2, 0x89C4, 0x89C6, 0x89C8, 0x89C9, 0x89D2, 0x89E3, 0x89E6, 0x8B66, 0x8BA1, 0x8BA4,
                    0x8BA9, 0x8BAE, 0x8BB0, 0x8BB8, 0x8BBE, 0x8BC1, 0x8BC4, 0x8BC6, 0x8BCA, 0x8BD1, 0x8BD5, 0x8BDD,
                    0x8BE5, 0x8BE6, 0x8BEF, 0x8BF4, 0x8BF7, 0x8BFB, 0x8C01, 0x8C03, 0x8C22, 0x8D1F, 0x8D25, 0x8D28,
                    0x8D39, 0x8D44, 0x8D77, 0x8D85, 0x8D8A, 0x8DB3, 0x8DC3, 0x8DDF, 0x8DE8, 0x8DEF, 0x8DF3, 0x8E2A,
                    0x8EAB, 0x8F6C, 0x8F6E, 0x8F6F, 0x8F7B, 0x8F7D, 0x8F83, 0x8F85, 0x8F91, 0x8F93, 0x8FA8, 0x8FB9,
                    0x8FC1, 0x8FC7, 0x8FD0, 0x8FD1, 0x8FD4, 0x8FD8, 0x8FD9, 0x8FDB, 0x8FDC, 0x8FDE, 0x8FDF, 0x8FF0,
                    0x8FFD, 0x9000, 0x9001, 0x9002, 0x9006, 0x9009, 0x900F, 0x9010, 0x901A, 0x901F, 0x9020, 0x903B,
                    0x9053, 0x906E, 0x907F, 0x90A3, 0x90BB, 0x90E8, 0x90FD, 0x914D, 0x91C7, 0x91CA, 0x91CC, 0x91CD,
                    0x91CE, 0x91CF, 0x9488, 0x94A9, 0x94C3, 0x94FE, 0x9500, 0x9501, 0x9510, 0x9519, 0x951A, 0x952E,
                    0x952F, 0x955C, 0x957F, 0x95E8, 0x95EA, 0x95ED, 0x95EE, 0x95F4, 0x9608, 0x961F, 0x9632, 0x9634,
                    0x9636, 0x9644, 0x9645, 0x964D, 0x9650, 0x9664, 0x9669, 0x968F, 0x9690, 0x9694, 0x969C, 0x96C6,
                    0x96F6, 0x9700, 0x9752, 0x9759, 0x975E, 0x9760, 0x9762, 0x9875, 0x9879, 0x987A, 0x987B, 0x987F,
                    0x9884, 0x9886, 0x9888, 0x9891, 0x9897, 0x9898, 0x989C, 0x989D, 0x98CE, 0x9971, 0x9988, 0x9996,
                    0x9A6C, 0x9A71, 0x9A8C, 0x9AA4, 0x9AD8, 0x9B54, 0x9EC4, 0x9ED1, 0x9ED8, 0x9F7F, 0xFF01, 0xFF08,
                    0xFF09, 0xFF0B, 0xFF0C, 0xFF0F, 0xFF1A, 0xFF1B, 0xFF1D, 0xFF1F,
                    0,
                };
                std::string cjkText;
                for (const unsigned int* cp = kCjkCodepoints; *cp != 0; ++cp)
                {
                    const unsigned int c = *cp;
                    if (c < 0x80)
                        cjkText += static_cast<char>(c);
                    else if (c < 0x800)
                    {
                        cjkText += static_cast<char>(0xC0 | (c >> 6));
                        cjkText += static_cast<char>(0x80 | (c & 0x3F));
                    }
                    else
                    {
                        cjkText += static_cast<char>(0xE0 | (c >> 12));
                        cjkText += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                        cjkText += static_cast<char>(0x80 | (c & 0x3F));
                    }
                }

                ImFontGlyphRangesBuilder cjkBuilder;
                cjkBuilder.AddText(cjkText.c_str());
                // ImGui retains GlyphRanges by pointer; keep immutable storage alive across
                // delayed atlas builds, renderer setup, and context recreation.
                static const ImVector<ImWchar> cjkRanges = [&cjkBuilder]()
                {
                    ImVector<ImWchar> ranges;
                    cjkBuilder.BuildRanges(&ranges);
                    return ranges;
                }();

                // Same pattern as the nextgen production host: MergeMode plus an explicit
                // DstFont. Without DstFont this ImGui merges into Fonts.back() regardless.
                ImFont* cjkTargets[] = { gClassicFont, gHackFont, gInterFont };
                int cjkMerged = 0;
                for (size_t ti = 0; ti < IM_ARRAYSIZE(cjkTargets); ++ti)
                {
                    ImFont* target = cjkTargets[ti];
                    if (target == nullptr)
                        continue;
                    bool duplicate = false;
                    for (size_t tk = 0; tk < ti; ++tk)
                        duplicate |= cjkTargets[tk] == target;
                    if (duplicate)
                        continue;

                    for (const wchar_t* cjkFile : { L"msyh.ttc", L"simhei.ttf", L"simsun.ttc", L"deng.ttf" })
                    {
                        std::wstring cjkPath = std::wstring(L"C:\\Windows\\Fonts\\") + cjkFile;
                        if (GetFileAttributesW(cjkPath.c_str()) == INVALID_FILE_ATTRIBUTES)
                            continue;

                        ImFontConfig cjkFontConfig {};
                        cjkFontConfig.MergeMode = true;
                        cjkFontConfig.DstFont = target;
                        if (io.Fonts->AddFontFromFileTTF(std::string(cjkPath.begin(), cjkPath.end()).c_str(),
                                                         fontSize, &cjkFontConfig, cjkRanges.Data) != nullptr)
                        {
                            LOG_INFO("NEXT CJK font merged into theme font {}", (void*) target);
                            cjkMerged++;
                            break;
                        }
                    }
                }
                LOG_INFO("NEXT CJK merge done: {} theme font(s)", cjkMerged);
            }
        }
    }
    else
    {
        LOG_INFO("NEXT font init skipped: atlas already holds {} font(s)", io.Fonts->Fonts.Size);
    }

    if (!Config::Instance()->OverlayMenu.value_or_default())
    {
        _hdrTonemapApplied = false;
    }

    DWORD hwndPid = 0;
    DWORD hwndTid = GetWindowThreadProcessId(_handle, &hwndPid);

    LOG_DEBUG("HWND: {:X}, IsWindow: {}, HWND PID: {}, Current PID: {}, HWND TID: {}, Current TID: {}",
              (ULONG64) _handle, IsWindow(_handle), hwndPid, GetCurrentProcessId(), hwndTid, GetCurrentThreadId());

    OptiInput::Initialize(ResolveMenuInputOwner(_handle), isUWP);

    RebuildStyle();
    ApplyThemeFont();
    _isInited = true;
}

bool MenuCommon::RetargetGenshinWindow(HWND hwnd)
{
    // Called only after the overlay owner retires the outgoing renderer and
    // authenticates the incoming native-fallback or physical-child Present.
    // Preserve CPU UI state; this grants no old renderer/texture/fence reuse.
    if (!_isInited || _isUWP || State::Instance().isShuttingDown ||
        !ImGui::GetCurrentContext() || !_handle || !hwnd || hwnd == _handle ||
        !IsWindow(_handle) || !IsWindow(hwnd))
        return false;
    if (!IsAuthenticatedGenshinRebuildWindow(_handle) && !IsAuthenticatedGenshinRebuildWindow(hwnd))
        return false;
    const HWND inputOwner = ResolveMenuInputOwner(hwnd);
    if (!inputOwner || ResolveMenuInputOwner(_handle) != inputOwner ||
        ImGui::GetMainViewport()->PlatformHandleRaw != inputOwner ||
        ImGui::GetIO().BackendRendererUserData != nullptr)
        return false;
    DWORD process = 0;
    if (!GetWindowThreadProcessId(inputOwner, &process) || process != GetCurrentProcessId())
        return false;

    _handle = hwnd;
    ++g_menuGpuLifetime;
    menuIntentEligibleForRebuild = true;
    ResetFgShortcut();
    return true;
}

void MenuCommon::Shutdown()
{
    ResetFgShortcut();
    if (!MenuCommon::_isInited)
    {
        if (State::Instance().isShuttingDown)
        {
            menuOpenRequested = false;
            menuIntentEligibleForRebuild = false;
        }
        return;
    }

    // if (_oWndProc != nullptr)
    //{
    //     auto handle = (HWND) ImGui::GetMainViewport()->PlatformHandleRaw;
    //     SetLastError(0);
    //     auto restoreResult = SetWindowLongPtr(handle, GWLP_WNDPROC, (LONG_PTR) _oWndProc);
    //     auto error = GetLastError();

    //    if (restoreResult == 0 && error != 0)
    //    {
    //        LOG_ERROR("Failed to restore old WndProc. Error: {:X}", error);
    //    }

    //    _oWndProc = nullptr;
    //}

    if (!_isUWP)
        ImGui_ImplWin32_Shutdown();
    else
        ImGui_ImplUwp_Shutdown();

    ImGui::DestroyContext();

    _handle = nullptr;
    _isInited = false;
    if (State::Instance().isShuttingDown)
    {
        menuOpenRequested = false;
        menuIntentEligibleForRebuild = false;
    }
    _isVisible = false;
}

void MenuCommon::HideMenu()
{
    ResetFgShortcut();
    const bool revokeRequest = State::Instance().isShuttingDown || _handle == nullptr || !IsWindow(_handle);
    if (!_isVisible)
    {
        if (revokeRequest)
            menuOpenRequested = false;
        return;
    }

    _isVisible = false;

    // Keep a recoverable request during backend/window churn, but never carry it
    // across final teardown or an invalid native owner.
    if (revokeRequest)
        menuOpenRequested = false;

    ImGuiIO& io = ImGui::GetIO();
    (void) io;

    _showMipmapCalcWindow = false;
    _showHudlessWindow = false;

    io.MouseDrawCursor = _isVisible;
    io.WantCaptureKeyboard = _isVisible;
    io.WantCaptureMouse = _isVisible;
}
