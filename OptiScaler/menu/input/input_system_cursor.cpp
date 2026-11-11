#include "pch.h"
#include "input_system_internal.h"

#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#endif

#if defined(_MSC_VER)
#define OPTI_INPUT_RETURN_ADDRESS() _ReturnAddress()
#elif defined(__GNUC__) || defined(__clang__)
#define OPTI_INPUT_RETURN_ADDRESS() __builtin_return_address(0)
#else
#define OPTI_INPUT_RETURN_ADDRESS() nullptr
#endif

namespace OptiInput
{
RECT GetVirtualScreenRect()
{
    RECT rect {};

    rect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    rect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    rect.right = rect.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    rect.bottom = rect.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    return rect;
}

void BeginCursorClipBlockLocked()
{
    if (_state.CursorClipReleasedForMenu)
        return;

    LOG_INFO("begin cursor clip block input:{} target:{}", static_cast<void*>(_state.InputHwnd),
             static_cast<void*>(_state.TargetHwnd));

    RECT currentClip {};

    _state.HasSavedClipRect = false;
    _state.SavedClipWasActive = false;
    _state.SavedClipRect = {};

    if (o_GetClipCursor != nullptr && o_GetClipCursor(&currentClip))
    {
        _state.SavedClipRect = currentClip;
        _state.HasSavedClipRect = true;
        _state.SavedClipWasActive = !IsVirtualScreenRect(currentClip);
    }

    _state.DeferredClipRect = {};
    _state.HasDeferredClipRect = false;
    _state.DeferredClipIsNull = false;

    if (o_ClipCursor != nullptr)
        o_ClipCursor(nullptr);

    _state.CursorClipReleasedForMenu = true;
}

void EndCursorClipBlockLocked()
{
    if (!_state.CursorClipReleasedForMenu)
        return;

    LOG_INFO("end cursor clip block hasDeferred:{} deferredNull:{} hasSaved:{} savedActive:{}",
             _state.HasDeferredClipRect ? 1 : 0, _state.DeferredClipIsNull ? 1 : 0, _state.HasSavedClipRect ? 1 : 0,
             _state.SavedClipWasActive ? 1 : 0);

    if (_state.HasDeferredClipRect)
    {
        if (_state.DeferredClipIsNull)
        {
            if (o_ClipCursor != nullptr)
                o_ClipCursor(nullptr);
        }
        else
        {
            if (o_ClipCursor != nullptr)
                o_ClipCursor(&_state.DeferredClipRect);
        }
    }
    else if (_state.HasSavedClipRect && _state.SavedClipWasActive)
    {
        if (o_ClipCursor != nullptr)
            o_ClipCursor(&_state.SavedClipRect);
    }
    else
    {
        if (o_ClipCursor != nullptr)
            o_ClipCursor(nullptr);
    }

    _state.SavedClipRect = {};
    _state.HasSavedClipRect = false;
    _state.SavedClipWasActive = false;

    _state.DeferredClipRect = {};
    _state.HasDeferredClipRect = false;
    _state.DeferredClipIsNull = false;

    _state.CursorClipReleasedForMenu = false;
}

void StoreDeferredClipCursorLocked(const RECT* rect)
{
    _state.HasDeferredClipRect = true;

    if (rect == nullptr)
    {
        _state.DeferredClipRect = {};
        _state.DeferredClipIsNull = true;
        return;
    }

    _state.DeferredClipRect = *rect;
    _state.DeferredClipIsNull = false;
}

bool RectEquals(const RECT& a, const RECT& b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool IsVirtualScreenRect(const RECT& rect) { return RectEquals(rect, GetVirtualScreenRect()); }

namespace
{

bool TryGetModuleFromAddress(const void* address, HMODULE& module)
{
    module = nullptr;

    if (address == nullptr)
        return false;

    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(address), &module) != FALSE &&
           module != nullptr;
}

bool IsInternalCursorReadCaller(const void* callerAddress)
{
    HMODULE selfModule = nullptr;
    HMODULE callerModule = nullptr;

    if (!TryGetModuleFromAddress(reinterpret_cast<const void*>(&hkGetCursorPos), selfModule))
        return false;

    if (!TryGetModuleFromAddress(callerAddress, callerModule))
        return false;

    return callerModule == selfModule;
}

bool GetBlockedCursorScreenPosLocked(POINT& point)
{
    if (_state.ExternalVirtualMouseActive && _state.ExternalVirtualMouseInitialized && _state.ExternalTargetProcess &&
        _state.InputHwnd == nullptr && _state.TargetHwnd != nullptr && IsWindow(_state.TargetHwnd))
    {
        POINT screenPos = _state.ExternalVirtualMouseClient;
        if (ClientToScreen(_state.TargetHwnd, &screenPos))
        {
            _state.MouseScreenPos = screenPos;
            _state.ExternalGetCursorPosVirtualizedThisFrame = true;
            _state.ExternalGetCursorPosVirtualizedCount++;
            point = screenPos;
            return true;
        }
    }

    _state.ExternalGetCursorPosVirtualizedThisFrame = true;
    _state.ExternalGetCursorPosVirtualizedCount++;

    if (_state.HasBlockedCursorScreenPos)
        point = _state.BlockedCursorScreenPos;
    else
        point = _state.MouseScreenPos;

    return true;
}

DWORD PackCursorMessagePos(const POINT& point)
{
    return static_cast<DWORD>(MAKELONG(static_cast<SHORT>(point.x), static_cast<SHORT>(point.y)));
}
} // namespace

BOOL WINAPI hkGetCursorPos(LPPOINT point)
{
    if (point == nullptr)
        return FALSE;

    {
        std::unique_lock lock(_state.Mutex);

        if (ShouldBlockCursorInputLocked() && !IsInternalCursorReadCaller(OPTI_INPUT_RETURN_ADDRESS()))
        {
            _state.GetCursorPosBlockedCount++;
            GetBlockedCursorScreenPosLocked(*point);
            return TRUE;
        }
    }

    return o_GetCursorPos(point);
}

BOOL WINAPI hkSetCursorPos(int x, int y)
{
    {
        std::unique_lock lock(_state.Mutex);

        if (ShouldBlockCursorInputLocked())
        {
            _state.SetCursorPosBlockedCount++;
            return TRUE;
        }
    }

    return o_SetCursorPos(x, y);
}

BOOL WINAPI hkGetPhysicalCursorPos(LPPOINT point)
{
    if (point == nullptr)
        return FALSE;

    {
        std::unique_lock lock(_state.Mutex);

        if (ShouldBlockCursorInputLocked() && !IsInternalCursorReadCaller(OPTI_INPUT_RETURN_ADDRESS()))
        {
            _state.GetPhysicalCursorPosBlockedCount++;
            GetBlockedCursorScreenPosLocked(*point);
            return TRUE;
        }
    }

    return o_GetPhysicalCursorPos != nullptr ? o_GetPhysicalCursorPos(point) : o_GetCursorPos(point);
}

BOOL WINAPI hkSetPhysicalCursorPos(int x, int y)
{
    {
        std::unique_lock lock(_state.Mutex);

        if (ShouldBlockCursorInputLocked())
        {
            _state.SetPhysicalCursorPosBlockedCount++;
            return TRUE;
        }
    }

    return o_SetPhysicalCursorPos != nullptr ? o_SetPhysicalCursorPos(x, y) : o_SetCursorPos(x, y);
}

DWORD WINAPI hkGetMessagePos()
{
    {
        std::unique_lock lock(_state.Mutex);

        if (ShouldBlockCursorInputLocked() && !IsInternalCursorReadCaller(OPTI_INPUT_RETURN_ADDRESS()))
        {
            POINT point {};
            _state.GetMessagePosBlockedCount++;
            GetBlockedCursorScreenPosLocked(point);
            return PackCursorMessagePos(point);
        }
    }

    return o_GetMessagePos();
}

int WINAPI hkGetMouseMovePointsEx(UINT pointSize, LPMOUSEMOVEPOINT point, LPMOUSEMOVEPOINT buffer, int bufferPoints,
                                  DWORD resolution)
{
    {
        std::unique_lock lock(_state.Mutex);

        if ((ShouldBlockMouseInputLocked() || ShouldBlockCursorInputLocked()) &&
            !IsInternalCursorReadCaller(OPTI_INPUT_RETURN_ADDRESS()))
        {
            _state.MouseMovePointsBlockedCount++;
            return 0;
        }
    }

    return o_GetMouseMovePointsEx(pointSize, point, buffer, bufferPoints, resolution);
}

BOOL WINAPI hkClipCursor(const RECT* rect)
{
    {
        std::unique_lock lock(_state.Mutex);

        if (ShouldBlockCursorInputLocked())
        {
            _state.ClipCursorBlockedCount++;
            OPTIINPUT_LOG_VERBOSE("deferring ClipCursor rect:{}", rect != nullptr ? 1 : 0);
            StoreDeferredClipCursorLocked(rect);
            return TRUE;
        }
    }

    return o_ClipCursor(rect);
}

BOOL WINAPI hkGetClipCursor(LPRECT rect)
{
    if (rect == nullptr)
        return FALSE;

    {
        std::unique_lock lock(_state.Mutex);

        if (ShouldBlockCursorInputLocked())
        {
            _state.GetClipCursorVirtualizedCount++;
            OPTIINPUT_LOG_VERBOSE("virtualizing GetClipCursor");
            *rect = GetVirtualScreenRect();
            return TRUE;
        }
    }

    return o_GetClipCursor(rect);
}

GameCursorShape::~GameCursorShape() { if (cursor) DestroyCursor(cursor); }

namespace
{
bool SaveGameCursorLocked(HCURSOR requested) noexcept
{
    if (_state.GameCursorInvisible && requested == _state.GameCursorInvisible) return false;
    if (_state.GameCursorKnown && requested == _state.GameCursorRequested && (!requested || _state.GameCursorSaved)) return true;
    _state.GameCursorRequested = requested; _state.GameCursorKnown = true;
    _state.GameCursorSaved.reset(); ++_state.GameCursorRevision;
    if (!requested) return true; // genuine game null, not a synthetic cursor
    if (!_state.GameCursorActive) return true; // inactive input tracks only the actual request, no icon work
    try
    {
        auto shape = std::make_shared<GameCursorShape>();
        shape->cursor = static_cast<HCURSOR>(CopyIcon(requested));
        ICONINFO icon{};
        if (!shape->cursor || !GetIconInfo(shape->cursor, &icon)) return false;
        shape->hotspot = {static_cast<LONG>(icon.xHotspot), static_cast<LONG>(icon.yHotspot)};
        if (icon.hbmColor) DeleteObject(icon.hbmColor);
        if (icon.hbmMask) DeleteObject(icon.hbmMask);
        if (icon.fIcon) return false;
        _state.GameCursorSaved = std::move(shape); return true;
    }
    catch (...) { return false; }
}
bool CursorWindowLocked(HWND window, DWORD& thread) noexcept
{
    DWORD process = 0; thread = window ? GetWindowThreadProcessId(window, &process) : 0;
    return _state.Initialized && _state.GameCursorHooks && window == _state.TargetHwnd &&
        !_state.ExternalTargetProcess && thread && thread == _state.TargetThreadId &&
        process == GetCurrentProcessId() && IsWindow(window) &&
        GetPropW(window, GameCursorEpochProperty) ==
            reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(_state.GameCursorWindowEpoch));
}
}

void ReleaseGameCursorLocked() noexcept
{
    if (!_state.GameCursorActive && !_state.GameCursorInvisible) return;
    if (_state.GameCursorActive && _state.GameCursorHide)
    {
        CURSORINFO info{sizeof(info)};
        if (o_GetCursorInfo(&info) && info.hCursor == _state.GameCursorInvisible)
            o_SetCursor(_state.GameCursorRequested);
    }
    _state.GameCursorActive = _state.GameCursorHide = false;
    _state.GameCursorWindow = nullptr; ++_state.GameCursorRevision;
    // Preserve the last GAME request across menu ownership and MOD disable.
    // Internal ImGui SetCursor must never become a new game cursor on reentry.
    // Invisible cursor is input-owner lifetime, not a GPU object. Destroy only
    // after restoring it; a failed restore keeps the OS-owned handle alive.
    CURSORINFO info{sizeof(info)};
    if (_state.GameCursorInvisible && o_GetCursorInfo(&info) && info.hCursor != _state.GameCursorInvisible)
    { DestroyCursor(_state.GameCursorInvisible); _state.GameCursorInvisible = nullptr; }
}
void InvalidateGameCursorLocked() noexcept
{
    ReleaseGameCursorLocked();
    if (_state.TargetHwnd && GetPropW(_state.TargetHwnd, GameCursorEpochProperty) ==
        reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(_state.GameCursorWindowEpoch)))
        RemovePropW(_state.TargetHwnd, GameCursorEpochProperty);
    _state.GameCursorKnown = false;
    _state.GameCursorSaved.reset(); _state.GameCursorRequested = nullptr;
}
void ReleaseGameCursor() noexcept
{ std::unique_lock lock(_state.Mutex); ReleaseGameCursorLocked(); }

HCURSOR WINAPI hkGameSetCursor(HCURSOR requested)
{
    if (IsInternalCursorReadCaller(OPTI_INPUT_RETURN_ADDRESS())) return o_SetCursor(requested);
    std::unique_lock lock(_state.Mutex);
    DWORD thread = 0;
    if (!CursorWindowLocked(_state.TargetHwnd, thread) || GetCurrentThreadId() != thread) return o_SetCursor(requested);
    const auto previous = _state.GameCursorRequested;
    SaveGameCursorLocked(requested);
    const auto actualPrevious = o_SetCursor(_state.GameCursorHide ? _state.GameCursorInvisible : requested);
    return _state.GameCursorActive && _state.GameCursorHide ? previous : actualPrevious;
}
HCURSOR WINAPI hkGameGetCursor()
{
    if (IsInternalCursorReadCaller(OPTI_INPUT_RETURN_ADDRESS())) return o_GetCursor();
    std::unique_lock lock(_state.Mutex);
    if (_state.GameCursorActive && _state.GameCursorHide && GetCurrentThreadId() == _state.TargetThreadId)
        return _state.GameCursorRequested;
    return o_GetCursor();
}
BOOL WINAPI hkGameGetCursorInfo(PCURSORINFO info)
{
    const auto result = o_GetCursorInfo(info);
    if (!result || !info || IsInternalCursorReadCaller(OPTI_INPUT_RETURN_ADDRESS())) return result;
    std::unique_lock lock(_state.Mutex);
    if (_state.GameCursorActive && (info->flags & CURSOR_SHOWING))
        info->hCursor = _state.GameCursorRequested;
    return result;
}
namespace
{
LRESULT CursorDefWindowProc(HWND w, UINT m, WPARAM p, LPARAM l,
                           LRESULT (WINAPI *original)(HWND, UINT, WPARAM, LPARAM))
{
    if (m != WM_SETCURSOR) return original(w, m, p, l);
    bool ownsWindow = false, active = false;
    {
        std::unique_lock lock(_state.Mutex); DWORD thread = 0;
        ownsWindow = CursorWindowLocked(w, thread) && GetCurrentThreadId() == thread;
        active = _state.GameCursorActive;
    }
    if (!ownsWindow) return original(w, m, p, l);
    if (!active)
    {
        // Default-off input follows the real Windows procedure unchanged.
        // Remember only its actual client-area result, with no icon allocation.
        const auto result = original(w, m, p, l);
        if (LOWORD(l) == HTCLIENT)
        {
            std::unique_lock lock(_state.Mutex); DWORD thread = 0;
            if (CursorWindowLocked(w, thread) && GetCurrentThreadId() == thread)
            {
                const auto actual = o_GetCursor();
                if (!_state.GameCursorInvisible || actual != _state.GameCursorInvisible) SaveGameCursorLocked(actual);
                if (_state.GameCursorHide) o_SetCursor(_state.GameCursorInvisible);
            }
        }
        return result;
    }
    // Primary cursor.cpp's parent-first WM_SETCURSOR contract. Never hold the
    // input mutex across a parent callback: it may reenter these same hooks.
    if (const auto parent = GetAncestor(w, GA_PARENT))
        if (const auto handled = SendMessageW(parent, m, p, l)) return handled;
    // Non-client/error handling belongs to the original Windows procedure;
    // it may show a real resize/arrow cursor without replacing the GAME shape.
    if (LOWORD(l) != HTCLIENT) return original(w, m, p, l);
    const auto requested = reinterpret_cast<HCURSOR>(GetClassLongPtrW(w, GCLP_HCURSOR));
    if (requested)
    {
        std::unique_lock lock(_state.Mutex); DWORD thread = 0;
        if (CursorWindowLocked(w, thread) && GetCurrentThreadId() == thread)
        {
            SaveGameCursorLocked(requested);
            o_SetCursor(_state.GameCursorHide ? _state.GameCursorInvisible : requested);
        }
    }
    return FALSE; // allow children to choose their own class cursor, as primary
}
}
LRESULT WINAPI hkCursorDefWindowProcA(HWND w, UINT m, WPARAM p, LPARAM l)
{
    if (m != WM_SETCURSOR || IsInternalCursorReadCaller(OPTI_INPUT_RETURN_ADDRESS())) return o_CursorDefWindowProcA(w, m, p, l);
    return CursorDefWindowProc(w, m, p, l, o_CursorDefWindowProcA);
}
LRESULT WINAPI hkCursorDefWindowProcW(HWND w, UINT m, WPARAM p, LPARAM l)
{
    if (m != WM_SETCURSOR || IsInternalCursorReadCaller(OPTI_INPUT_RETURN_ADDRESS())) return o_CursorDefWindowProcW(w, m, p, l);
    return CursorDefWindowProc(w, m, p, l, o_CursorDefWindowProcW);
}

bool CaptureGameCursor(HWND window, std::uint64_t nativeEpoch, bool hideHardware, GameCursorSnapshot& out) noexcept
{
    out = {}; std::unique_lock lock(_state.Mutex); DWORD thread = 0;
    struct FailedCapture
    {
        bool complete = false;
        ~FailedCapture() { if (!complete) ReleaseGameCursorLocked(); }
    } capture;
    if (!nativeEpoch || nativeEpoch == UINT64_MAX || !CursorWindowLocked(window, thread) ||
        _state.MenuVisible || !_state.Focused)
    { ReleaseGameCursorLocked(); return false; }
    CURSORINFO info{sizeof(info)};
    if (!o_GetCursorInfo(&info)) return false;
    const bool entering = !_state.GameCursorActive || _state.GameCursorWindow != window;
    if (entering)
    {
        ReleaseGameCursorLocked();
        _state.GameCursorActive = true; _state.GameCursorWindow = window;
        if (!SaveGameCursorLocked(_state.GameCursorKnown ? _state.GameCursorRequested : info.hCursor))
        { ReleaseGameCursorLocked(); return false; }
    }
    // Subsequent requests arrive only from the game SetCursor/WM_SETCURSOR
    // owner. Never overwrite them using a menu-modified hardware handle.
    if (hideHardware && !_state.GameCursorInvisible)
    {
        const int width = GetSystemMetrics(SM_CXCURSOR), height = GetSystemMetrics(SM_CYCURSOR);
        if (width <= 0 || height <= 0 || width > 256 || height > 256 || width % 8) return false;
        std::array<BYTE, 8192> andBits{}, xorBits{}; andBits.fill(0xff);
        _state.GameCursorInvisible = CreateCursor(nullptr, 0, 0, width, height, andBits.data(), xorBits.data());
        if (!_state.GameCursorInvisible) return false;
    }
    // Hide by transparent handle, preserving Windows' actual ShowCursor count.
    // Like XXMI hide_cursor, even a null request uses the transparent handle. No ShowCursor balancing loops.
    const auto desired = hideHardware ? _state.GameCursorInvisible : _state.GameCursorRequested;
    const bool policyChanged = entering || hideHardware != _state.GameCursorHide;
    if (policyChanged) o_SetCursor(desired);
    _state.GameCursorHide = hideHardware;
    CURSORINFO after{sizeof(after)};
    if (!o_GetCursorInfo(&after) || (after.flags & ~DWORD(CURSOR_SHOWING | CURSOR_SUPPRESSED))) return false;
    // Like the primary non-client path, do not repeatedly force the invisible
    // cursor over Windows' resize/error cursor. Retain actual hardware identity
    // separately from the saved game shape, and prove it again before submit.
    if (policyChanged && (after.flags & CURSOR_SHOWING) && after.hCursor != desired) return false;
    out.hardware = after.hCursor;
    out.screen = after.ptScreenPos; out.client = out.screen;
    // Same API thread/DPI context for both coordinates and dimensions. F6 has
    // no XXMI SCREEN_UPSCALING virtual dimensions. Never use ImGui coordinates.
    if (!ScreenToClient(window, &out.client) || !GetClientRect(window, &out.clientRect) ||
        out.clientRect.left || out.clientRect.top || out.clientRect.right <= 0 || out.clientRect.bottom <= 0) return false;
    out.dpi = GetDpiForWindow(window); out.coordinateContext = GetThreadDpiAwarenessContext();
    if (!out.dpi || !out.coordinateContext) return false;
    out.showing = (after.flags & CURSOR_SHOWING) != 0;
    if (!_state.GameCursorSaved) return false; // unknown hotspot is never zero-filled
    out.shape = _state.GameCursorSaved; out.window = window; out.windowThread = thread;
    out.nativeEpoch = nativeEpoch; out.windowEpoch = _state.GameCursorWindowEpoch;
    out.cursorRevision = _state.GameCursorRevision;
    capture.complete = true; return true;
}
bool ValidateGameCursor(const GameCursorSnapshot& snapshot) noexcept
{
    std::unique_lock lock(_state.Mutex); DWORD thread = 0;
    RECT rect{}; POINT client = snapshot.screen; CURSORINFO actual{sizeof(actual)};
    if (!o_GetCursorInfo(&actual) || snapshot.showing != ((actual.flags & CURSOR_SHOWING) != 0)) return false;
    if (snapshot.showing && actual.hCursor != snapshot.hardware) return false;
    return snapshot.shape && snapshot.nativeEpoch && CursorWindowLocked(snapshot.window, thread) &&
        thread == snapshot.windowThread && snapshot.windowEpoch == _state.GameCursorWindowEpoch &&
        _state.GameCursorActive && _state.GameCursorWindow == snapshot.window &&
        snapshot.cursorRevision == _state.GameCursorRevision && !_state.MenuVisible && _state.Focused &&
        AreDpiAwarenessContextsEqual(snapshot.coordinateContext, GetThreadDpiAwarenessContext()) &&
        GetDpiForWindow(snapshot.window) == snapshot.dpi && GetClientRect(snapshot.window, &rect) &&
        RectEquals(rect, snapshot.clientRect) && ScreenToClient(snapshot.window, &client) &&
        client.x == snapshot.client.x && client.y == snapshot.client.y;
}

} // namespace OptiInput
