#include "pch.h"
#include "input_system_internal.h"

#include <detours/detours.h>
#include <genshin/GenshinInputHookTransaction.h>

#include <tlhelp32.h>
#include <vector>
#include <string_view>

// #define USE_HID_HOOKS

static bool messageHooks = false;
static bool keyStateHooks = false;
static bool getPosHooks = false;
static bool clipCursorHooks = false;
static bool message2Hooks = false;
static bool hidHooks = false;
static bool rawHooks = false;
static bool windowsHooks = false;
static bool positionHooks = false;
static bool getPhysicalCursorAlias = false;
static bool setPhysicalCursorAlias = false;
static bool positionIATHooks = false;

namespace OptiInput
{
SHORT RealGetAsyncKeyStateSafe(int vk)
{
    ScopedHookBypass bypass;

    if (o_GetAsyncKeyState != nullptr)
        return o_GetAsyncKeyState(vk);

    return ::GetAsyncKeyState(vk);
}

SHORT RealGetKeyStateSafe(int vk)
{
    ScopedHookBypass bypass;

    if (o_GetKeyState != nullptr)
        return o_GetKeyState(vk);

    return ::GetKeyState(vk);
}

BOOL RealGetCursorPosSafe(LPPOINT point)
{
    ScopedHookBypass bypass;

    if (o_GetCursorPos != nullptr)
        return o_GetCursorPos(point);

    return ::GetCursorPos(point);
}

namespace
{
struct IatPatch
{
    void** Entry = nullptr;
    void* Original = nullptr;
    void* Detour = nullptr;
    std::string Name;
};

std::vector<IatPatch> gCursorIatPatches;

bool IsSelfModule(HMODULE module)
{
    HMODULE selfModule = nullptr;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&InstallHooks), &selfModule))
    {
        return false;
    }

    return module == selfModule;
}

bool PatchIatEntry(HMODULE module, const char* importName, void* original, void* detour)
{
    if (module == nullptr || importName == nullptr || original == nullptr || detour == nullptr)
        return false;

    if (IsSelfModule(module))
        return false;

    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);

    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);

    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    const auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    if (importDir.VirtualAddress == 0 || importDir.Size == 0)
        return false;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
    bool patched = false;

    for (; desc->Name != 0; ++desc)
    {
        auto* thunkOrig = desc->OriginalFirstThunk != 0
                              ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk)
                              : nullptr;

        auto* thunkIat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);

        if (thunkIat == nullptr)
            continue;

        for (size_t index = 0; thunkIat[index].u1.Function != 0; ++index)
        {
            const char* name = nullptr;

            if (thunkOrig != nullptr && !(thunkOrig[index].u1.Ordinal & IMAGE_ORDINAL_FLAG))
            {
                auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + thunkOrig[index].u1.AddressOfData);
                name = reinterpret_cast<const char*>(byName->Name);
            }

            if (name == nullptr || strcmp(name, importName) != 0)
                continue;

            auto** entry = reinterpret_cast<void**>(&thunkIat[index].u1.Function);

            // Only patch entries that still point to the real API.
            // This avoids overwriting another hook chain.
            if (*entry != original)
            {
                LOG_DEBUG("OptiInput IAT skip {} module:{} entry:{} current:{} expected:{}", importName,
                          static_cast<void*>(module), static_cast<void*>(entry), *entry, original);
                continue;
            }

            DWORD oldProtect = 0;
            if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &oldProtect))
            {
                LOG_WARN("OptiInput IAT protect failed {} module:{} entry:{} gle:{}", importName,
                         static_cast<void*>(module), static_cast<void*>(entry), GetLastError());
                continue;
            }

            *entry = detour;

            DWORD ignored = 0;
            VirtualProtect(entry, sizeof(void*), oldProtect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), entry, sizeof(void*));

            gCursorIatPatches.push_back({ entry, original, detour, importName });

            LOG_INFO("OptiInput IAT patched {} module:{} entry:{} original:{} detour:{}", importName,
                     static_cast<void*>(module), static_cast<void*>(entry), original, detour);

            patched = true;
        }
    }

    return patched;
}

bool PatchCursorIatForModule(HMODULE module)
{
    bool patched = false;

    patched |= PatchIatEntry(module, "GetCursorPos", reinterpret_cast<void*>(o_GetCursorPos),
                             reinterpret_cast<void*>(hkGetCursorPos));

    patched |= PatchIatEntry(module, "SetCursorPos", reinterpret_cast<void*>(o_SetCursorPos),
                             reinterpret_cast<void*>(hkSetCursorPos));

    return patched;
}

bool InstallCursorIatHooks()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        LOG_WARN("OptiInput cursor IAT snapshot failed gle:{}", GetLastError());
        return false;
    }

    MODULEENTRY32W entry {};
    entry.dwSize = sizeof(entry);

    bool patchedAny = false;

    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            patchedAny |= PatchCursorIatForModule(entry.hModule);
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);

    LOG_INFO("OptiInput cursor IAT hooks installed patched:{} entries:{}", patchedAny ? 1 : 0,
             gCursorIatPatches.size());

    return patchedAny;
}

void RemoveCursorIatHooks()
{
    for (auto it = gCursorIatPatches.rbegin(); it != gCursorIatPatches.rend(); ++it)
    {
        IatPatch& patch = *it;

        if (patch.Entry == nullptr || patch.Original == nullptr)
            continue;

        DWORD oldProtect = 0;
        if (!VirtualProtect(patch.Entry, sizeof(void*), PAGE_READWRITE, &oldProtect))
            continue;

        if (*patch.Entry == patch.Detour)
            *patch.Entry = patch.Original;

        DWORD ignored = 0;
        VirtualProtect(patch.Entry, sizeof(void*), oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), patch.Entry, sizeof(void*));
    }

    LOG_INFO("OptiInput cursor IAT hooks removed entries:{}", gCursorIatPatches.size());

    gCursorIatPatches.clear();
}

void ResolveOptionalUser32Exports()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");

    if (user32 == nullptr)
        user32 = LoadLibraryW(L"user32.dll");

    if (user32 == nullptr)
        return;

    if (o_GetPhysicalCursorPos == nullptr)
    {
        o_GetPhysicalCursorPos =
            reinterpret_cast<GetPhysicalCursorPos_t>(GetProcAddress(user32, "GetPhysicalCursorPos"));
    }

    if (o_SetPhysicalCursorPos == nullptr)
    {
        o_SetPhysicalCursorPos =
            reinterpret_cast<SetPhysicalCursorPos_t>(GetProcAddress(user32, "SetPhysicalCursorPos"));
    }
}
} // namespace

bool InstallHooks()
{
    GenshinNativeQuery::StartupInstallScope inputInstallScope;
    if (_state.HooksInstalled)
        return true;

    LOG_INFO("installing Win32 input hooks currentPid:{} targetPid:{} inputPid:{}", _state.CurrentProcessId,
             _state.TargetProcessId, _state.InputProcessId);

    ResolveOptionalUser32Exports();

    if (!messageHooks)
    {
        GenshinInputHooks::Transaction inputTransaction;

        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_PeekMessageA), hkPeekMessageA);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_PeekMessageW), hkPeekMessageW);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_GetMessageA), hkGetMessageA);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_GetMessageW), hkGetMessageW);

        const LONG result = inputTransaction.Commit();

        messageHooks = result == NO_ERROR;
        if (messageHooks)
            LOG_INFO("Win32 message hooks installed");
        else
            LOG_ERROR("Win32 message hook installation failed result:{}", result);
    }

    if (!keyStateHooks)
    {
        GenshinInputHooks::Transaction inputTransaction;

        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_GetAsyncKeyState), hkGetAsyncKeyState);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_GetKeyState), hkGetKeyState);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_GetKeyboardState), hkGetKeyboardState);

        const LONG result = inputTransaction.Commit();

        keyStateHooks = result == NO_ERROR;
        if (keyStateHooks)
            LOG_INFO("Win32 key state hooks installed");
        else
            LOG_ERROR("Win32 key state hook installation failed result:{}", result);
    }

    if (!getPosHooks)
    {
        GenshinInputHooks::Transaction inputTransaction;

        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_GetMessagePos), hkGetMessagePos);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_GetMouseMovePointsEx), hkGetMouseMovePointsEx);

        const LONG result = inputTransaction.Commit();

        getPosHooks = result == NO_ERROR;
        if (getPosHooks)
            LOG_INFO("Win32 GetMessagePos hooks installed");
        else
            LOG_ERROR("Win32 GetMessagePos hook installation failed result:{}", result);
    }

    if (!clipCursorHooks)
    {
        GenshinInputHooks::Transaction inputTransaction;

        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_ClipCursor), hkClipCursor);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_GetClipCursor), hkGetClipCursor);

        const LONG result = inputTransaction.Commit();

        clipCursorHooks = result == NO_ERROR;
        if (clipCursorHooks)
            LOG_INFO("Win32 clip cursor hooks installed");
        else
            LOG_ERROR("Win32 clip cursor hook installation failed result:{}", result);
    }

    if (!message2Hooks)
    {
        GenshinInputHooks::Transaction inputTransaction;

        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_SendInput), hkSendInput);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_mouse_event), hkmouse_event);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_PostMessageA), hkPostMessageA);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_PostMessageW), hkPostMessageW);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_SendMessageA), hkSendMessageA);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_SendMessageW), hkSendMessageW);

        const LONG result = inputTransaction.Commit();

        message2Hooks = result == NO_ERROR;
        if (message2Hooks)
            LOG_INFO("Win32 message hooks installed");
        else
            LOG_ERROR("Win32 message hook installation failed result:{}", result);
    }

#ifdef USE_HID_HOOKS
    if (!State::Instance().isRunningOnLinux && !hidHooks)
    {
        GenshinInputHooks::Transaction inputTransaction;

        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_CreateFileA), hkCreateFileA);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_CreateFileW), hkCreateFileW);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_ReadFile), hkReadFile);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_DeviceIoControl), hkDeviceIoControl);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_CloseHandle), hkCloseHandle);

        const LONG result = inputTransaction.Commit();

        hidHooks = result == NO_ERROR;
        if (hidHooks)
            LOG_INFO("Win32 HID hooks installed");
        else
            LOG_ERROR("Win32 HID hook installation failed result:{}", result);
    }
#endif //  USE_HID_HOOKS

    if (!rawHooks)
    {
        GenshinInputHooks::Transaction inputTransaction;

        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_GetRawInputData), hkGetRawInputData);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_GetRawInputBuffer), hkGetRawInputBuffer);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_RegisterRawInputDevices), hkRegisterRawInputDevices);

        const LONG result = inputTransaction.Commit();

        rawHooks = result == NO_ERROR;
        if (rawHooks)
            LOG_INFO("Win32 raw input hooks installed");
        else
            LOG_ERROR("Win32 raw input installation failed result:{}", result);
    }

    if (!windowsHooks)
    {
        GenshinInputHooks::Transaction inputTransaction;

        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_SetWindowsHookExA), hkSetWindowsHookExA);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_SetWindowsHookExW), hkSetWindowsHookExW);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_UnhookWindowsHookEx), hkUnhookWindowsHookEx);

        const LONG result = inputTransaction.Commit();

        windowsHooks = result == NO_ERROR;
        if (windowsHooks)
            LOG_INFO("Win32 windows input hooks installed");
        else
            LOG_ERROR("Win32 windows input installation failed result:{}", result);
    }

    if (!positionHooks)
    {
        // Some Windows builds export both API names at the exact same entry.
        // Decide before Detours replaces the original slots with trampolines.
        const bool getAlias = o_GetPhysicalCursorPos != nullptr && o_GetPhysicalCursorPos == o_GetCursorPos;
        const bool setAlias = o_SetPhysicalCursorPos != nullptr && o_SetPhysicalCursorPos == o_SetCursorPos;
        GenshinInputHooks::Transaction inputTransaction;

        // Extend the EXISTING input transaction/teardown owner, no second hook.
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_SetCursor), hkGameSetCursor);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_GetCursor), hkGameGetCursor);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_GetCursorInfo), hkGameGetCursorInfo);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_CursorDefWindowProcA), hkCursorDefWindowProcA);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_CursorDefWindowProcW), hkCursorDefWindowProcW);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_GetCursorPos), hkGetCursorPos,
            getAlias ? reinterpret_cast<PVOID*>(&o_GetPhysicalCursorPos) : nullptr);
        inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_SetCursorPos), hkSetCursorPos,
            setAlias ? reinterpret_cast<PVOID*>(&o_SetPhysicalCursorPos) : nullptr);

        if (o_GetPhysicalCursorPos != nullptr && !getAlias)
            inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_GetPhysicalCursorPos), hkGetPhysicalCursorPos);

        if (o_SetPhysicalCursorPos != nullptr && !setAlias)
            inputTransaction.Attach(reinterpret_cast<PVOID*>(&o_SetPhysicalCursorPos), hkSetPhysicalCursorPos);

        const LONG result = inputTransaction.Commit();

        positionHooks = result == NO_ERROR;
        _state.GameCursorHooks = positionHooks;
        if (positionHooks) {
            getPhysicalCursorAlias = getAlias;
            setPhysicalCursorAlias = setAlias;
            LOG_INFO("Cursor export ownership getAlias:{} setAlias:{} singleEntryPerAlias:true",getAlias,setAlias);
        }
        if (positionHooks)
            LOG_INFO("Win32 position input hooks installed");
        else
            LOG_ERROR("Win32 position input installation failed result:{}", result);
    }

    // If CursorPos hooks are not installed, try to install
    // IAT (Import Address Table) hooks for CursorPos
    if (!positionHooks && !positionIATHooks)
        positionIATHooks = InstallCursorIatHooks();

#ifdef USE_HID_HOOKS
    const bool hidReady = State::Instance().isRunningOnLinux || hidHooks;
    _state.HooksInstalled = messageHooks && keyStateHooks && getPosHooks && clipCursorHooks && message2Hooks &&
                            hidReady && rawHooks && windowsHooks && (positionHooks || positionIATHooks);
#else
    _state.HooksInstalled = messageHooks && keyStateHooks && getPosHooks && clipCursorHooks && message2Hooks &&
                            rawHooks && windowsHooks && (positionHooks || positionIATHooks);
#endif // USE_HID_HOOKS

    return _state.HooksInstalled;
}

bool RemoveHooks()
{
    const bool hasDetourHooks = messageHooks || keyStateHooks || getPosHooks || clipCursorHooks || message2Hooks ||
                                hidHooks || rawHooks || windowsHooks || positionHooks;

    if (!hasDetourHooks && !positionIATHooks)
    {
        _state.HooksInstalled = false;
        return true;
    }

    LOG_INFO("removing Win32 input hooks");

    LONG result = NO_ERROR;

    if (hasDetourHooks)
    {
        GenshinInputHooks::Transaction inputTransaction;

        if (messageHooks)
        {
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_PeekMessageA), hkPeekMessageA);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_PeekMessageW), hkPeekMessageW);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_GetMessageA), hkGetMessageA);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_GetMessageW), hkGetMessageW);
        }

        if (keyStateHooks)
        {
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_GetAsyncKeyState), hkGetAsyncKeyState);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_GetKeyState), hkGetKeyState);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_GetKeyboardState), hkGetKeyboardState);
        }

        if (getPosHooks)
        {
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_GetMessagePos), hkGetMessagePos);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_GetMouseMovePointsEx), hkGetMouseMovePointsEx);
        }

        if (clipCursorHooks)
        {
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_ClipCursor), hkClipCursor);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_GetClipCursor), hkGetClipCursor);
        }

        if (message2Hooks)
        {
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_SendInput), hkSendInput);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_mouse_event), hkmouse_event);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_PostMessageA), hkPostMessageA);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_PostMessageW), hkPostMessageW);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_SendMessageA), hkSendMessageA);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_SendMessageW), hkSendMessageW);
        }

        if (hidHooks)
        {
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_CreateFileA), hkCreateFileA);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_CreateFileW), hkCreateFileW);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_ReadFile), hkReadFile);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_DeviceIoControl), hkDeviceIoControl);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_CloseHandle), hkCloseHandle);
        }

        if (rawHooks)
        {
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_GetRawInputData), hkGetRawInputData);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_GetRawInputBuffer), hkGetRawInputBuffer);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_RegisterRawInputDevices), hkRegisterRawInputDevices);
        }

        if (windowsHooks)
        {
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_SetWindowsHookExA), hkSetWindowsHookExA);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_SetWindowsHookExW), hkSetWindowsHookExW);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_UnhookWindowsHookEx), hkUnhookWindowsHookEx);
        }

        if (positionHooks)
        {
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_SetCursor), hkGameSetCursor);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_GetCursor), hkGameGetCursor);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_GetCursorInfo), hkGameGetCursorInfo);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_CursorDefWindowProcA), hkCursorDefWindowProcA);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_CursorDefWindowProcW), hkCursorDefWindowProcW);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_GetCursorPos), hkGetCursorPos,
                getPhysicalCursorAlias ? reinterpret_cast<PVOID*>(&o_GetPhysicalCursorPos) : nullptr);
            inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_SetCursorPos), hkSetCursorPos,
                setPhysicalCursorAlias ? reinterpret_cast<PVOID*>(&o_SetPhysicalCursorPos) : nullptr);

            if (o_GetPhysicalCursorPos != nullptr && !getPhysicalCursorAlias)
                inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_GetPhysicalCursorPos), hkGetPhysicalCursorPos);

            if (o_SetPhysicalCursorPos != nullptr && !setPhysicalCursorAlias)
                inputTransaction.Detach(reinterpret_cast<PVOID*>(&o_SetPhysicalCursorPos), hkSetPhysicalCursorPos);
        }

        result = inputTransaction.Commit();

        if (result == NO_ERROR)
        {
            messageHooks = false;
            keyStateHooks = false;
            getPosHooks = false;
            clipCursorHooks = false;
            message2Hooks = false;
            hidHooks = false;
            rawHooks = false;
            windowsHooks = false;
            positionHooks = false;
            _state.GameCursorHooks = false;
            getPhysicalCursorAlias = false;
            setPhysicalCursorAlias = false;
        }
        else
        {
            LOG_WARN("Win32 input hook removal failed result:{}; retaining hook state for a safe retry", result);
        }
    }

    if (positionIATHooks)
    {
        RemoveCursorIatHooks();
        positionIATHooks = false;
    }

    const bool hidReady = State::Instance().isRunningOnLinux || hidHooks;
    _state.HooksInstalled = messageHooks && keyStateHooks && getPosHooks && clipCursorHooks && message2Hooks &&
                            hidReady && rawHooks && windowsHooks && (positionHooks || positionIATHooks);

    return result == NO_ERROR;
}

} // namespace OptiInput
