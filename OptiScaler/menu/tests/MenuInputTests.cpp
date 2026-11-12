// Real ImGui core + extracted production input/backend. OS seams below never
// create/query windows, load plugins, or read the user's physical input.
#include <Windows.h>
#include <Xinput.h>
#include <d3d11.h>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <menu/MenuInputPolicy.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace Fixture
{
unsigned checks = 0;
std::string mode;
std::array<bool, 256> physicalKeys {};
bool connected = false;
WORD padButtons = 0;
unsigned keyReads = 0, padReads = 0, cursorWrites = 0;
LONGLONG clockNow = 0;
unsigned pollsInFrame = 0, platformSamples = 0, rendererFrames = 0;
bool insideOverlayFrame = false;
bool throwOnClock = false;
void AdvanceFrameClock() { clockNow += 1000; pollsInFrame = 0; }
void Check(bool value, const char* tag, const char* message)
{
    ++checks;
    if (!value)
    {
        std::fprintf(stderr, "FAIL [%s] check %u: %s\n", tag, checks, message);
        std::exit(1);
    }
}
}

namespace OptiInput
{
struct Button { bool Down = false, Pressed = false; };
struct TestInputState
{
    std::mutex Mutex;
    bool Initialized = true, Focused = true, MenuVisible = true;
    bool BlockKeyboard = true, BlockMouse = true, BlockGamepad = true;
    std::array<Button, 256> Keys {};
    std::array<Button, 5> MouseButtons {};
    POINT MouseClientPos { 0, 0 };
    float MouseWheel = 0;
    std::wstring TextInput;
    unsigned GetKeyStateBlockedCount = 0;
    unsigned XInputGetStateCallCount = 0, XInputGetStatePassedCount = 0, XInputGetStateBlockedCount = 0;
} _state;
thread_local int bypassHookDepth = 0;
SHORT WINAPI hkGetKeyState(int vk);
DWORD WINAPI hkXInputGetState(DWORD, XINPUT_STATE*);
SHORT RealGetAsyncKeyStateSafe(int vk)
{
    return Fixture::physicalKeys.at(vk) ? static_cast<SHORT>(0x8000) : 0;
}
SHORT (WINAPI* o_GetKeyState)(int) = [](int vk) -> SHORT {
    ++Fixture::keyReads;
    return RealGetAsyncKeyStateSafe(vk);
};
DWORD WINAPI PhysicalPad(DWORD, XINPUT_STATE* out)
{
    ++Fixture::padReads;
    if (!out) return ERROR_BAD_ARGUMENTS;
    *out = {};
    if (!Fixture::connected) return ERROR_DEVICE_NOT_CONNECTED;
    out->dwPacketNumber = 42;
    out->Gamepad.wButtons = Fixture::padButtons;
    return ERROR_SUCCESS;
}
DWORD (WINAPI* o_XInputGetState)(DWORD, XINPUT_STATE*) = PhysicalPad;
void UpdateImGuiMouseDrawCursorLocked(ImGuiIO&) {}
void RefreshInputAcquisitionModeLocked() {}
}

// The actual backend NewFrame, key workaround, mouse-data code and XInput
// mapping are extracted unchanged. Only external Windows services are seams.
using PFN_XInputGetCapabilities = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
using PFN_XInputGetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
SHORT WINAPI TestGetKeyState(int vk) { return OptiInput::hkGetKeyState(vk); }
BOOL WINAPI TestGetClientRect(HWND, LPRECT out) { *out = { 0, 0, 800, 600 }; return TRUE; }
BOOL WINAPI TestQueryPerformanceCounter(LARGE_INTEGER* out)
{
    if (Fixture::throwOnClock) throw std::runtime_error("controlled backend failure");
    // A present arrives every 1000 ticks, while a duplicate sample in the
    // same present is only one tick later. Counting calls alone cannot test
    // the bad DeltaTime caused by two adjacent backend NewFrame calls.
    out->QuadPart = Fixture::clockNow + Fixture::pollsInFrame++;
    ++Fixture::platformSamples;
    return TRUE;
}
BOOL WINAPI TestGetCursorPos(LPPOINT) { return FALSE; } // Input owner supplies position in FeedImGui.
HWND WINAPI TestGetForegroundWindow() { return reinterpret_cast<HWND>(1); }
BOOL WINAPI TestIsChild(HWND, HWND) { return FALSE; }
HWND WINAPI TestWindowFromPoint(POINT) { return nullptr; }
BOOL WINAPI TestClientToScreen(HWND, LPPOINT) { return TRUE; }
BOOL WINAPI TestScreenToClient(HWND, LPPOINT) { return TRUE; }
BOOL WINAPI TestSetCursorPos(int, int) { ++Fixture::cursorWrites; return TRUE; }
static void ImGui_ImplWin32_UpdateMonitors() {}
static bool ImGui_ImplWin32_UpdateMouseCursor(ImGuiIO&, ImGuiMouseCursor) { return true; }
#define GetKeyState TestGetKeyState
#define GetClientRect TestGetClientRect
#define QueryPerformanceCounter TestQueryPerformanceCounter
#define GetCursorPos TestGetCursorPos
#define GetForegroundWindow TestGetForegroundWindow
#define IsChild TestIsChild
#define WindowFromPoint TestWindowFromPoint
#define ClientToScreen TestClientToScreen
#define ScreenToClient TestScreenToClient
#define SetCursorPos TestSetCursorPos
#include "production_backend.inc"
#undef GetKeyState
#undef GetClientRect
#undef QueryPerformanceCounter
#undef GetCursorPos
#undef GetForegroundWindow
#undef IsChild
#undef WindowFromPoint
#undef ClientToScreen
#undef ScreenToClient
#undef SetCursorPos

#define OPTIINPUT_LOG_VERBOSE(...) ((void)0)
namespace OptiInput
{
#include "production_input.inc"
#include "lost_fast_click.inc"
}

// Inert renderer/device services let us execute the actual DX11 overlay
// function. DrawUi returns false after running the input frame, so no COM/GPU
// method executes. Device pointers are sentinels, never real game objects.
namespace MenuHotpaths { enum class Backend { Dx11 }; }
enum class MenuCpuStage { UiBuild, ImGuiRender, Record };
struct MenuCpuFrame
{
    MenuCpuFrame(MenuHotpaths::Backend, ID3D11Device*, unsigned, bool) {}
    bool DrawReceipt = false, NoUi = false;
};
struct MenuCpuSpan { MenuCpuSpan(MenuCpuStage) {} void Finish() {} };
namespace NativeMotionDx11 { void OnPresent(IDXGISwapChain*, ID3D11Device*) {} }
namespace F6InputPolicy { constexpr bool FsrBridgeEnabled = false; }
namespace GenshinDepthProvider
{
void BeginPreviewDraw(ID3D11DeviceContext*) {}
void EndPreviewDraw(ID3D11DeviceContext*, bool) {}
}
namespace MenuOverlayBase
{
std::function<bool()> render;
bool IsInited() { return true; }
void HideMenu() {}
bool RenderMenu() { return render(); }
}
static bool _dx11Device = true, _isInited = true;
static ID3D11Device* g_pd3dDevice = reinterpret_cast<ID3D11Device*>(1);
static ID3D11DeviceContext* g_pd3dDeviceContext = reinterpret_cast<ID3D11DeviceContext*>(1);
static ID3D11RenderTargetView* g_pd3dRenderTarget = reinterpret_cast<ID3D11RenderTargetView*>(1);
static unsigned g_outputMenuGeneration = 0;
struct PresentResult { const char* Decision; };
static PresentResult* g_presentResult = nullptr;
static void CreateRenderTargetDx11(IDXGISwapChain*) {}
static bool ImGui_ImplDX11_Init(ID3D11Device*, ID3D11DeviceContext*) { return true; }
static void ImGui_ImplDX11_NewFrame() { ++Fixture::rendererFrames; }
static void ImGui_ImplDX11_RenderDrawData(ImDrawData*) {}
template<typename Draw> static bool RenderWithBackbufferReceipt(ImDrawData*, Draw) { return false; }
static void NoteOverlayWrite(unsigned) {}
#define LOG_FUNC() ((void)0)
#include "production_dx11_overlay.inc"

namespace Fixture
{
struct Menu
{
    ImGuiContext* context = ImGui::CreateContext();
    ImGui_ImplWin32_Data backend;
    bool visible = true;
    std::array<ImGuiID, 3> ids {};
    std::array<ImVec2, 3> centers {};
    std::array<unsigned, 3> activations {};

    Menu()
    {
        physicalKeys.fill(false);
        connected = false; padButtons = 0;
        auto& state = OptiInput::_state;
        state.Keys = {}; state.MouseButtons = {}; state.TextInput.clear();
        state.MouseWheel = 0; state.MouseClientPos = { 0, 0 };
        state.Focused = state.Initialized = state.MenuVisible = true;
        state.BlockKeyboard = state.BlockMouse = state.BlockGamepad = true;
        auto& io = ImGui::GetIO();
        io.IniFilename = io.LogFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.BackendPlatformUserData = &backend;
        backend.hWnd = reinterpret_cast<HWND>(1);
        backend.TicksPerSecond = 60000;
        backend.XInputGetState = OptiInput::hkXInputGetState;
        backend.XInputGetCapabilities = [](DWORD, DWORD, XINPUT_CAPABILITIES*) -> DWORD {
            return connected ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
        };
        backend.WantUpdateHasGamepad = true;
        for (int i = 0; i < 4; ++i) Frame();
    }
    ~Menu()
    {
        ImGui::GetIO().BackendPlatformUserData = nullptr;
        ImGui::DestroyContext(context);
    }
    void Frame(bool focusEN = false)
    {
        if (!insideOverlayFrame) AdvanceFrameClock();
        auto& io = ImGui::GetIO();
        OptiInput::_state.MenuVisible = visible;
        if (mode == "old-visibility")
            io.ConfigFlags = visible ? ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad
                : ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoKeyboard;
        else
            MenuInputPolicy::SetVisible(io, visible);
        if (mode == "unbypassed-shift" || mode == "unbypassed-gamepad")
            ImGui_ImplWin32_NewFrame();
        else
            OptiInput::NewImGuiWin32Frame();
        if (mode == "lost-fast-click") OptiInput::LostFastClickFeed(visible);
        else OptiInput::FeedImGui(visible);
        ImGui::NewFrame();
        if (visible)
        {
            ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(480, 300), ImGuiCond_Always);
            ImGui::Begin("F6 input fixture", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
            const char* labels[] = { "EN", "Other action", "Third action" };
            for (int i = 0; i < 3; ++i)
            {
                if (focusEN && i == 0) ImGui::SetKeyboardFocusHere();
                ids[i] = ImGui::GetID(labels[i]);
                if (ImGui::Button(labels[i], ImVec2(180, 40))) ++activations[i];
                const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                centers[i] = ImVec2((a.x + b.x) * .5f, (a.y + b.y) * .5f);
            }
            ImGui::End();
        }
        ImGui::Render();
        for (auto& button : OptiInput::_state.MouseButtons) button.Pressed = false;
    }
    void Key(int vk, bool down)
    {
        physicalKeys[vk] = down;
        OptiInput::_state.Keys[vk].Down = down;
    }
    void Move(int index)
    {
        OptiInput::_state.MouseClientPos = { LONG(centers[index].x), LONG(centers[index].y) };
        Frame(); Frame();
    }
    void Click(int index, bool fast = false)
    {
        Move(index);
        auto& button = OptiInput::_state.MouseButtons[0];
        button.Pressed = true;
        button.Down = !fast;
        Frame();
        button.Down = false;
        for (int i = 0; i < 4; ++i) Frame();
    }
    void FocusEN()
    {
        Frame(true); Frame();
        Check(context->NavId == ids[0], "NAV_FOCUS", "EN received navigation focus");
        Key(VK_DOWN, true); Frame();
        Key(VK_DOWN, false); Frame(); Frame();
        Check(context->NavId == ids[1] && context->NavCursorVisible, "KEYBOARD_NAV", "Down moves navigation to another button");
        Key(VK_UP, true); Frame();
        Key(VK_UP, false); Frame(); Frame();
        Check(context->NavId == ids[0] && context->NavCursorVisible, "KEYBOARD_NAV", "Up returns visible navigation cursor to EN");
    }
    void Pad(WORD buttons)
    {
        connected = true; padButtons = buttons;
        backend.WantUpdateHasGamepad = true;
        Frame();
    }
};

void OverlayFrame(Menu& menu)
{
    AdvanceFrameClock();
    insideOverlayFrame = true;
    auto& io = ImGui::GetIO();
    io.BackendRendererUserData = reinterpret_cast<void*>(1);
    MenuOverlayBase::render = [&] { menu.Frame(); return false; };
    if (mode.starts_with("dx11-")) OldRenderImGui_DX11(nullptr);
    else RenderImGui_DX11(nullptr);
    MenuOverlayBase::render = {};
    io.BackendRendererUserData = nullptr;
    insideOverlayFrame = false;
}

void Dx11OverlayTests()
{
    Menu menu;
    menu.Key(VK_LSHIFT, true);
    connected = true;
    padButtons = XINPUT_GAMEPAD_A;
    menu.backend.WantUpdateHasGamepad = true;
    for (int i = 0; i < 3; ++i) menu.Frame();
    for (int i = 0; i < 8; ++i)
    {
        const unsigned beforeSamples = platformSamples, beforeRenderer = rendererFrames;
        OverlayFrame(menu);
        const bool shiftStable = ImGui::IsKeyDown(ImGuiKey_LeftShift) && !ImGui::IsKeyReleased(ImGuiKey_LeftShift);
        const bool padStable = ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown) && !ImGui::IsKeyReleased(ImGuiKey_GamepadFaceDown);
        // Test each failure independently: old pre-poll must be caught for
        // both input streams as well as sampling ownership and timing.
        if (mode == "dx11-unbypassed-shift")
            Check(shiftStable, "DX11_SHIFT_STABLE", "bare DX11 pre-poll fabricates Shift up before bypassed common frame");
        if (mode == "dx11-unbypassed-gamepad")
            Check(padStable, "DX11_PAD_STABLE", "bare DX11 pre-poll alternates neutral and real gamepad state");
        if (mode == "dx11-short-delta")
            Check(ImAbs(ImGui::GetIO().DeltaTime - 1.0f / 60.0f) < 0.000001f,
                  "FRAME_DELTA", "DeltaTime must measure one present interval, not adjacent platform polls");
        if (mode == "dx11-unbypassed-shift" || mode == "dx11-unbypassed-gamepad") continue;
        Check(platformSamples - beforeSamples == 1 && pollsInFrame == 1,
              "ONE_PLATFORM_SAMPLE", "actual DX11 overlay/common frame samples platform exactly once");
        Check(rendererFrames - beforeRenderer == 1, "DX11_RENDERER_FRAME", "DX11 renderer NewFrame is retained exactly once");
        Check(ImAbs(ImGui::GetIO().DeltaTime - 1.0f / 60.0f) < 0.000001f,
              "FRAME_DELTA", "DeltaTime remains the full 1000-tick present interval");
        Check(shiftStable, "DX11_SHIFT_STABLE", "actual DX11 overlay preserves held Shift without false release");
        Check(padStable, "DX11_PAD_STABLE", "actual DX11 overlay preserves held gamepad A without false release");
    }
}

void PolicyTests()
{
    Menu menu;
    auto& io = ImGui::GetIO();
    constexpr int managed = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad |
        ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoKeyboard;
    // Exercise every managed-bit starting state and every individual unrelated
    // bit (including future/reserved bits), both capability states and toggles.
    for (int bit = 0; bit < 30; ++bit)
    {
        const int unrelated = (1 << bit) & ~managed;
        for (int initial = 0; initial < 32; ++initial)
        for (int caps : { 0, 1 << bit, int(ImGuiBackendFlags_HasGamepad | ImGuiBackendFlags_HasMouseCursors |
                               ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_HasSetMousePos) })
        {
            const int flags[] = { ImGuiConfigFlags_NavEnableKeyboard, ImGuiConfigFlags_NavEnableGamepad,
                ImGuiConfigFlags_NoMouse, ImGuiConfigFlags_NoMouseCursorChange, ImGuiConfigFlags_NoKeyboard };
            io.ConfigFlags = unrelated;
            for (int i = 0; i < 5; ++i) if (initial & (1 << i)) io.ConfigFlags |= flags[i];
            io.BackendFlags = caps;
            for (bool visible : { false, true, true, false, false, true })
            {
                io.WantSetMousePos = !visible;
                MenuInputPolicy::SetVisible(io, visible);
                const int expected = visible ? ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad
                    : ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoKeyboard;
                Check(io.ConfigFlags == (unrelated | expected), "POLICY_BITS", "only five owned config bits change");
                Check(io.BackendFlags == caps, "BACKEND_PRESERVED", "policy preserves all backend capabilities");
                Check(io.WantSetMousePos == !visible, "CORE_CURSOR_OWNERSHIP", "visibility leaves core cursor request untouched");
            }
        }
    }
    io.ConfigFlags = ImGuiConfigFlags_IsSRGB;
    io.BackendFlags = ImGuiBackendFlags_RendererHasTextures;
    menu.Frame();
    Check((io.ConfigFlags & ImGuiConfigFlags_IsSRGB) != 0, "CONFIG_PRESERVED", "frame must preserve unrelated flags");
}

void ShiftAndMouseTests()
{
    Menu menu;
    menu.Key(VK_LSHIFT, true);
    menu.Frame(); menu.Frame();
    for (int i = 0; i < 12; ++i)
    {
        menu.Frame();
        Check(ImGui::IsKeyDown(ImGuiKey_LeftShift), "SHIFT_STABLE", "held Shift must not acquire a synthetic key-up");
        Check(!ImGui::IsKeyReleased(ImGuiKey_LeftShift), "SHIFT_STABLE", "backend workaround must not fabricate Shift release");
        Check(ImGui::GetIO().KeyShift && menu.context->InputEventsQueue.empty(), "SHIFT_STABLE", "held Shift leaves no modifier conflict or queued false transitions");
    }
    Check(OptiInput::hkGetKeyState(VK_LSHIFT) == 0, "GAME_BLOCKING", "game still sees blocked Shift outside backend scope");
    menu.Click(1);
    Check(menu.activations[1] == 1 && menu.activations[0] == 0, "SHIFT_CLICK", "held Shift permits exactly one target click");
    Check(ImGui::IsKeyDown(ImGuiKey_LeftShift), "SHIFT_CLICK", "Shift survives click without false transitions");
    menu.Key(VK_LSHIFT, false); menu.Frame(); menu.Frame();
    Check(!ImGui::IsKeyDown(ImGuiKey_LeftShift), "SHIFT_RELEASE", "real Shift release remains supported");
    menu.Key(VK_LSHIFT, true); menu.Key(VK_RSHIFT, true); menu.Frame(); menu.Frame();
    // Release one of two physically held Shift keys through the full feed.
    physicalKeys[VK_LSHIFT] = false; OptiInput::_state.Keys[VK_LSHIFT].Down = false;
    menu.Frame(); menu.Frame();
    Check(!ImGui::IsKeyDown(ImGuiKey_LeftShift) && ImGui::IsKeyDown(ImGuiKey_RightShift),
          "SHIFT_RELEASE", "left release cannot release still-held right Shift");
    menu.Key(VK_RSHIFT, false); menu.Frame(); menu.Frame();
    menu.FocusEN();
    menu.Click(1);
    Check(menu.activations[1] == 2 && menu.activations[0] == 0, "MOUSE_TAKEOVER", "mouse moves from EN focus and activates another button");
    Check(!menu.context->NavCursorVisible, "NAV_CURSOR_AUTO_HIDE", "standard mouse takeover hides keyboard navigation cursor");
    Check(menu.context->NavId == menu.ids[1], "MOUSE_TAKEOVER", "clicked button owns focus");
    Check(cursorWrites == 0, "NO_CURSOR_WARP", "normal navigation and clicks never request cursor warps");
    for (int i = 0; i < 6; ++i) menu.Click(2, true);
    Check(menu.activations[2] == 6, "FAST_CLICK", "batched press/release produces exactly one activation per click");
    menu.FocusEN();
    menu.Key(VK_RETURN, true); menu.Frame();
    menu.Key(VK_RETURN, false); menu.Frame(); menu.Frame();
    Check(menu.activations[0] == 1, "KEYBOARD_ACTIVATE", "Enter still activates navigation-focused EN");
}

void BackendReleaseRepairTests()
{
    Menu menu;
    menu.Key(VK_LSHIFT, true); menu.Key(VK_RSHIFT, true);
    menu.Frame(); menu.Frame();
    // Do not update the input snapshot: test only the real backend's useful
    // workaround before FeedImGui has any chance to supply a release event.
    physicalKeys[VK_LSHIFT] = false;
    ImGui::GetIO().ClearEventsQueue();
    OptiInput::NewImGuiWin32Frame();
    bool releasedLeft = false, releasedRight = false;
    for (const auto& event : menu.context->InputEventsQueue)
    {
        if (event.Type != ImGuiInputEventType_Key || event.Key.Down) continue;
        releasedLeft |= event.Key.Key == ImGuiKey_LeftShift;
        releasedRight |= event.Key.Key == ImGuiKey_RightShift;
    }
    Check(releasedLeft && !releasedRight, "BACKEND_RELEASE_REPAIR", "backend still repairs missing Shift key-up using actual physical state");
}

void GamepadTests()
{
    Menu menu;
    menu.Pad(XINPUT_GAMEPAD_A); menu.Frame();
    Check(ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown), "PAD_STATE", "backend receives real A instead of game-blocked zero");
    XINPUT_STATE game {};
    Check(OptiInput::hkXInputGetState(0, &game) == ERROR_SUCCESS && game.Gamepad.wButtons == 0,
          "GAME_BLOCKING", "game sees neutral connected pad outside backend scope");
    menu.Pad(0); menu.Frame(); menu.FocusEN();
    menu.Pad(XINPUT_GAMEPAD_DPAD_DOWN); menu.Pad(0); menu.Frame(); menu.Frame();
    Check(menu.context->NavId == menu.ids[1] && menu.context->NavCursorVisible, "PAD_NAV", "gamepad navigates to another button");
    const auto before = menu.activations[1];
    menu.Pad(XINPUT_GAMEPAD_A); menu.Pad(0); menu.Frame(); menu.Frame();
    Check(menu.activations[1] == before + 1, "PAD_ACTIVATE", "gamepad A activates navigation target once");
    menu.visible = false; menu.Pad(XINPUT_GAMEPAD_A); menu.Frame();
    Check(!ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown) && menu.context->InputEventsQueue.empty(),
          "CLOSED_CLEAR", "closed overlay clears backend gamepad events too");
    menu.Pad(0); menu.visible = true; menu.Frame(); menu.Frame();
    Check(menu.activations[1] == before + 1, "REOPEN_NO_REPLAY", "hidden gamepad press never replays on reopen");
    connected = false; menu.backend.WantUpdateHasGamepad = true;
    menu.Frame();
    Check(!(ImGui::GetIO().BackendFlags & ImGuiBackendFlags_HasGamepad), "NO_PHANTOM_PAD", "backend reports physical disconnect");
    for (bool visible : { false, true, false, true })
    {
        menu.visible = visible; menu.Frame();
        Check(!(ImGui::GetIO().BackendFlags & ImGuiBackendFlags_HasGamepad), "NO_PHANTOM_PAD", "visibility toggles cannot invent HasGamepad");
    }
}

void VisibilityAndFocusTests()
{
    Menu menu;
    menu.Key('A', true); menu.Key(VK_LSHIFT, true);
    OptiInput::_state.MouseButtons[0].Down = true;
    menu.Frame(); menu.Frame();
    menu.visible = false;
    OptiInput::_state.TextInput = L"hidden";
    menu.Frame();
    auto& io = ImGui::GetIO();
    Check(!ImGui::IsKeyDown(ImGuiKey_A) && !ImGui::IsKeyDown(ImGuiKey_LeftShift) && !io.MouseDown[0],
          "CLOSED_CLEAR", "closing clears held keyboard, modifiers and mouse");
    Check(menu.context->InputEventsQueue.empty() && io.InputQueueCharacters.empty(), "CLOSED_CLEAR", "closed frame retains no queued input/text");
    menu.Key('A', false); menu.Key(VK_LSHIFT, false);
    OptiInput::_state.MouseButtons[0].Down = false;
    OptiInput::_state.TextInput.clear();
    menu.visible = true; menu.Frame(); menu.Frame();
    Check(!ImGui::IsKeyPressed(ImGuiKey_A) && !io.KeyShift && !io.MouseClicked[0], "REOPEN_NO_REPLAY", "reopen has no released hidden input");
    menu.Click(1);
    Check(menu.activations[1] == 1, "REOPEN_CLICK", "first click after reopen works");
    menu.Key('A', true); menu.Frame();
    OptiInput::_state.Focused = false; menu.Frame(); menu.Frame();
    Check(!ImGui::IsKeyDown(ImGuiKey_A) && !io.KeyShift, "FOCUS_CLEAR", "focus loss clears held inputs through ImGui focus event");
    menu.Key('A', false); OptiInput::_state.Focused = true;
    menu.Frame(); menu.Frame();
    Check(!ImGui::IsKeyPressed(ImGuiKey_A), "FOCUS_CLEAR", "focus regain cannot replay old key");
}

void BypassLifetimeTests()
{
    Menu menu;
    Check(OptiInput::bypassHookDepth == 0, "BYPASS_SCOPE", "initial bypass depth is zero");
    {
        OptiInput::ScopedHookBypass outer;
        OptiInput::NewImGuiWin32Frame();
        Check(OptiInput::bypassHookDepth == 1, "BYPASS_SCOPE", "nested backend call preserves outer bypass");
        int otherDepth = -1;
        std::thread thread([&] { otherDepth = OptiInput::bypassHookDepth; }); thread.join();
        Check(otherDepth == 0, "BYPASS_THREAD", "bypass does not leak to game input thread");
    }
    Fixture::throwOnClock = true;
    bool caught = false;
    try { OptiInput::NewImGuiWin32Frame(); } catch (const std::runtime_error&) { caught = true; }
    Fixture::throwOnClock = false;
    Check(caught && OptiInput::bypassHookDepth == 0, "BYPASS_SCOPE", "exception unwinds backend bypass without leaving game unblocked");
}
}

int main(int argc, char** argv)
{
    using namespace Fixture;
    if (argc > 1) mode = argv[1];
    if (mode.starts_with("dx11-")) Dx11OverlayTests();
    else if (mode == "unbypassed-gamepad") GamepadTests();
    else if (mode == "unbypassed-shift" || mode == "lost-fast-click") ShiftAndMouseTests();
    else if (mode == "old-visibility") PolicyTests();
    else
    {
        PolicyTests(); ShiftAndMouseTests(); BackendReleaseRepairTests(); GamepadTests(); VisibilityAndFocusTests(); BypassLifetimeTests(); Dx11OverlayTests();
        std::printf("PASS: %u checks; real ImGui/policy/backend/FeedImGui; key reads=%u pad reads=%u\n", checks, keyReads, padReads);
    }
}
