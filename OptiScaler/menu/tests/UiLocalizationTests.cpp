// Display-only tests of the production dictionary, formatter and stable labels.
#include <menu/UiLocalization.h>
#include <menu/CriticalErrorWindow.h>
#include <imgui/ImGuiNotify.hpp>
#include <cstdlib>
#include <iostream>

namespace { bool english = false; unsigned checks = 0; }
namespace NextgenUi { bool English() { return english; } }

static void Check(bool value, const char* message)
{
    ++checks;
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

static std::string Identity(const std::string& label)
{
    const auto reset = label.rfind("###");
    return reset == std::string::npos ? label : label.substr(reset + 3);
}

int main()
{
    const auto translator = +[](const char* text) { return MenuText::Translate(text); };
    english = false;
    ImGuiToast toast {ImGuiToastType::Info, 15000};
    toast.setTitle("DLSS 神经渲染");
    toast.setContent("%s", "GPU完成");
    const auto id = toast.getId();
    Check(toast.resolveContent(translator) == "GPU完成", "Toast lost its Chinese source");
    english = true;
    Check(toast.resolveContent(translator) == MenuText::Translate("GPU完成"), "Visible toast did not switch to English");
    toast.setContent("%s", MenuText::Translate("GPU完成").c_str());
    english = false;
    Check(toast.resolveContent(translator) == "GPU完成", "Queued English toast did not switch back to Chinese");
    toast.setContentProvider([] { return MenuText::Format("源：%.1f Hz", 60.0); });
    Check(toast.resolveContent(translator) == "源：60.0 Hz", "Deferred toast did not format Chinese");
    english = true;
    Check(toast.resolveContent(translator) == "Source: 60.0 Hz", "Deferred toast did not reformat English");
    Check(toast.getId() == id, "Language change reinserted the toast");
    toast.setContent("%s", "replacement");
    Check(toast.resolveContent(translator) == "replacement", "Literal setter did not replace deferred content");
    for (const auto& [cn, en] : MenuText::Dictionary)
    {
        english = false;
        Check(MenuText::Translate(cn) == cn, "Chinese source changed in Chinese mode");
        const auto cnLabel = MenuText::StableLabel(cn);
        english = true;
        Check(MenuText::Translate(cn) == en, "English dictionary lookup failed");
        Check(Identity(cnLabel) == Identity(MenuText::StableLabel(cn)), "Language changed the ImGui identity");
    }

    english = true;
    Check(MenuText::StableLabel("NR 每帧细节保留调试###NrHistoryPerFrame") ==
          "NR per-frame detail retention debug###NrHistoryPerFrame", "History window identity changed");
    Check(MenuText::StableLabel("Draw UID（按需、仅内存）##draw-uid") ==
          "Draw UID (on demand, memory only)###Draw UID（按需、仅内存）##draw-uid", "Legacy double-hash identity changed");
    Check(MenuText::StableLabel("###菜单缩放") == "###菜单缩放", "Hidden identity was translated");
    Check(MenuText::Format("源：%.1f Hz", 60.0) == "Source: 60.0 Hz", "Source-rate format failed");
    Check(MenuText::Format("GPU 开始前排队：%.1fms（同份报告）", 5.5) ==
          "Queue before GPU start: 5.5ms (same report)", "Queue monitor format failed");
    Check(MenuText::Format("%s / %s", "是", "未证明") == "yes / Unproven", "Embedded state translation failed");
    Check(MenuText::Format("历史贡献：%.1f%% 采样点 | 源帧 %llu | %llu ms 前", 12.5, 42ull, 7ull) ==
          "History contribution: 12.5% of sample points | source 42 | 7 ms ago", "History percentage format failed");
    Check(MenuText::Format("深度 %s %u×%u／运动 %s %u×%u／工作 %u×%u／相位 %s",
          "收到", 10u, 20u, "缺失", 30u, 40u, 50u, 60u, "未验证") ==
          "Depth Received 10x20 / motion missing 30x40 / working 50x60 / phase Unverified", "Guide field arguments shifted");
    Check(MenuText::Format("已完成贡献采样：%.2f%% 采样点／源 %llu／%llu ms 前（独立异步采样，非每帧读回）",
          2.25, 99ull, 3ull) ==
          "Completed contribution sample: 2.25% of sample points / source 99 / 3 ms ago (independent asynchronous sample, not per-frame readback)", "Contribution sample format failed");
    Check(MenuText::Format("%s", "feedback-recorded-awaiting-publication") ==
          "feedback-recorded-awaiting-publication", "Machine status token changed");
    Check(MenuText::Format("%s", "D:\\配置\\100%\\原神.ini") == "D:\\配置\\100%\\原神.ini", "User path changed");
    Check(MenuText::Format("%s", "100% 源帧") == "100% Source frame", "Literal percent treated as a format");

    english = false;
    Check(MenuText::Format("源：%.1f Hz", 60.0) == "源：60.0 Hz", "Chinese source-rate format changed");
    Check(MenuText::Format("%s / %s", "是", "未证明") == "是 / 未证明", "Chinese embedded states changed");
    Check(MenuText::Format("历史贡献：%.1f%% 采样点 | 源帧 %llu | %llu ms 前", 12.5, 42ull, 7ull) ==
          "历史贡献：12.5% 采样点 | 源帧 42 | 7 ms 前", "Chinese percentage format changed");
    std::cout << checks << " localization checks passed\n";
}
