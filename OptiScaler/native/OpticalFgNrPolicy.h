#pragma once
#include <Config.h>
#include <windows.h>
#include <cwchar>
#include <inputs/taa/TaaInputDx11.h>
namespace native {
inline bool OpticalFgNrEnabled()
{
    if (taa::OwnsInput()) return false;
    // Experimental ownership is selected only at process startup, after INI loading.
    // A StarRail profile/root value must never change another game's input authority.
    static const bool enabled = [] {
        if (!Config::Instance()->DlssNrOpticalFgNr.value_or_default()) return false;
        wchar_t path[32768] {};
        const auto size = GetModuleFileNameW(nullptr, path, (DWORD) std::size(path));
        if (!size || size >= std::size(path)) return false;
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        return _wcsicmp(name, L"StarRail.exe") == 0;
    }();
    return enabled;
}
}
