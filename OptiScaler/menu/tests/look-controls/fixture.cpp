// SPDX-License-Identifier: GPL-3.0-or-later
#include <dlssnr/LayerLookProfile.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace UiTest {
inline std::string action, scope;
inline float edit = 1.5f;
inline bool english = true;
inline unsigned notices = 0;
inline std::vector<std::string> scopes;
inline bool Click(const char* label) {
    if (action.empty() || action != label || (!scope.empty() &&
        std::find(scopes.begin(), scopes.end(), scope) == scopes.end())) return false;
    action.clear(); return true;
}
}
using ImGuiTreeNodeFlags = int;
constexpr int ImGuiTreeNodeFlags_DefaultOpen = 1;
constexpr int ImGuiSliderFlags_AlwaysClamp = 1;
namespace ImGui {
inline bool CollapsingHeader(const char*, int) { return true; }
inline void PushID(const char* id) { UiTest::scopes.emplace_back(id); }
inline void PushID(int id) { UiTest::scopes.push_back(std::to_string(id)); }
inline void PopID() { UiTest::scopes.pop_back(); }
inline void SameLine() {}
inline void BeginDisabled(bool) {}
inline void EndDisabled() {}
inline bool SmallButton(const char* label) { return UiTest::Click(label); }
inline bool Button(const char* label) { return UiTest::Click(label); }
inline bool RadioButton(const char* label, bool) { return UiTest::Click(label); }
inline bool Combo(const char* label, int* value, const char*) {
    if (!UiTest::Click(label)) return false;
    *value = 1; return true;
}
}
namespace MenuText {
inline void SeparatorText(const char*) {}
inline void TextWrapped(const char*, const char*) { ++UiTest::notices; }
inline bool SmallButton(const char* label, bool visible = true) {
    return visible && UiTest::Click(label);
}
inline bool Checkbox(const char* label, bool* value) {
    if (!UiTest::Click(label)) return false;
    *value = !*value; return true;
}
inline bool SliderFloat(const char* label, float* value, float, float, const char*, int, const char* = nullptr) {
    if (!UiTest::Click(label)) return false;
    *value = UiTest::edit; return true;
}
}
namespace NextgenUi { inline bool English() { return UiTest::english; } }
namespace NextgenSliderStyle { inline bool IsDefault(float a, float b) { return a == b; } }

#include <LookControlsProduction.h>

template<class T> struct Option {
    T value{};
    T value_or_default() const { return value; }
    Option& operator=(T v) { value = v; return *this; }
};
#define LOOK_FLOATS(X) X(Strength) X(Brighten) X(Darken) X(Colour) X(Hue) X(Shadows) \
    X(Midtones) X(Highlights) X(MaxBrighten) X(MaxDarken) X(MaxColour) X(Tone) \
    X(Detail) X(DetailRadius) X(Halo) X(HaloBrightSide)
struct Look {
    Option<bool> DlssNrLookMode;
    Option<unsigned> inherit, overrides, uiMask;
#define DECLARE_FLOAT(name) Option<float> DlssNrLook##name;
    LOOK_FLOATS(DECLARE_FLOAT)
#undef DECLARE_FLOAT
};
struct Configuration : Look {
    Option<unsigned> DlssNrDebugView;
    Option<bool> DlssNrLayerLookMode;
    std::array<Look, DlssNr::MaxPassCount> DlssNrLayerLooks;
};

int main() {
    Configuration c;
    unsigned checks = 0;
    const auto check = [&](bool ok) { ++checks; assert(ok); };
    const auto arm = [&](const char* action, const char* scope = "") {
        UiTest::action = action; UiTest::scope = scope; c.DlssNrDebugView = 2u;
    };
    // Passive visits never modify the diagnostic selection, including fallback.
    for (unsigned view = 0; view < 4; ++view) {
        c.DlssNrDebugView = view;
        NextgenLookControls::Render(&c);
        NextgenLookControls::RenderGlobal(&c, 2, true);
        NextgenLookControls::RenderGlobal(&c, 2, false);
        NextgenLookControls::RenderLayer(&c, 0, 2);
        c.DlssNrLayerLookMode = true;
        NextgenLookControls::RenderLayer(&c, 1, 2);
        check(c.DlssNrDebugView.value == view);
        c.DlssNrLayerLookMode = false;
    }
    for (unsigned i = 0; i < DlssNr::LayerLook::Parameters.size(); ++i) {
        const auto& p = DlssNr::LayerLook::Parameters[i];
        arm((std::string(p.en) + "###value").c_str(), (std::string("Look") + p.key).c_str());
        NextgenLookControls::Render(&c);
        check(UiTest::action.empty() && c.DlssNrDebugView.value == 0);
        DlssNr::LayerLook::Visit(c, [&](unsigned j, auto& v) { if (j == i) check(v.value == std::clamp(UiTest::edit, p.low, p.high)); });
        arm("Reset###reset", (std::string("Look") + p.key).c_str());
        NextgenLookControls::Render(&c);
        check(UiTest::action.empty() && c.DlssNrDebugView.value == 0);
        DlssNr::LayerLook::Visit(c, [&](unsigned j, auto& v) { if (j == i) check(v.value == p.neutral); });
    }
    arm("Show final result###lookFinalView");
    const float previous = c.DlssNrLookMidtones.value;
    NextgenLookControls::RenderGlobal(&c, 2, true);
    check(c.DlssNrDebugView.value == 0 && c.DlssNrLookMidtones.value == previous);
    arm("Independent per-layer Look###globalLayerMode");
    NextgenLookControls::RenderGlobal(&c, 2, true);
    check(c.DlssNrLayerLookMode.value && c.DlssNrDebugView.value == 0);
    auto& layer = c.DlssNrLayerLooks[1];
    arm("Enable Look for this layer###f5LookEnabled");
    NextgenLookControls::RenderLayer(&c, 1, 2);
    check(layer.DlssNrLookMode.value && c.DlssNrDebugView.value == 0);
    arm("Parameter source"); NextgenLookControls::RenderLayer(&c, 1, 2);
    check(layer.inherit.value == 1 && c.DlssNrDebugView.value == 0);
    arm("Override###override", "6"); NextgenLookControls::RenderLayer(&c, 1, 2);
    check(layer.overrides.value == (1u << 6) && c.DlssNrDebugView.value == 0);
    arm("Midtones"); NextgenLookControls::RenderLayer(&c, 1, 2);
    check(layer.DlssNrLookMidtones.value == 1.5f && c.DlssNrDebugView.value == 0);
    arm("Reset", "6"); NextgenLookControls::RenderLayer(&c, 1, 2);
    check(layer.overrides.value == 0 && layer.DlssNrLookMidtones.value == 1.5f && c.DlssNrDebugView.value == 0);
    arm("Protect real UI"); NextgenLookControls::RenderLayer(&c, 1, 2);
    check(layer.uiMask.value == 1 && c.DlssNrDebugView.value == 0);
    arm("Reset layer parameters"); NextgenLookControls::RenderLayer(&c, 1, 2);
    check(c.DlssNrDebugView.value == 0);
    UiTest::edit = std::numeric_limits<float>::quiet_NaN();
    arm("Midtones###value", "LookMidtones"); NextgenLookControls::Render(&c);
    check(c.DlssNrDebugView.value == 2 && c.DlssNrLookMidtones.value == previous);
    UiTest::english = false;
    arm("查看最终效果###lookFinalView"); NextgenLookControls::RenderGlobal(&c, 2, true);
    check(c.DlssNrDebugView.value == 0 && UiTest::action.empty());
    check(UiTest::scopes.empty());
    std::printf("Production Look controls: %u checks passed; browsing, sliders, reset, inheritance, final preview\n", checks);
}

