#pragma once
// Included only by dllmain.cpp after its export forwarding objects are defined.
// Deliberately separate from CheckWorkingMode: no Config, State, plugin search,
// original-DLL search, provider initialization, spoofing, graphics hooks or ASI.
static bool InitializeExpiredSystemForwarding(HMODULE self)
{
    wchar_t path[32768] {};
    const DWORD length = GetModuleFileNameW(self, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path))
        return false;
    const wchar_t* name = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') name = p + 1;
    // Normal optiscaler.dll/.asi has no system original; remain inert.
    if (_wcsicmp(name, L"optiscaler.dll") == 0 || _wcsicmp(name, L"optiscaler.asi") == 0)
        return true;
    const bool supported = _wcsicmp(name, L"version.dll") == 0 || _wcsicmp(name, L"winmm.dll") == 0 ||
        _wcsicmp(name, L"wininet.dll") == 0 || _wcsicmp(name, L"winhttp.dll") == 0 ||
        _wcsicmp(name, L"dbghelp.dll") == 0 || _wcsicmp(name, L"dxgi.dll") == 0 ||
        _wcsicmp(name, L"d3d12.dll") == 0;
    if (!supported)
        return true; // Unknown/ASI name: inert, never search plugin directories.
    wchar_t systemPath[MAX_PATH] {};
    const UINT systemLength = GetSystemDirectoryW(systemPath, MAX_PATH);
    if (!systemLength || systemLength >= MAX_PATH ||
        wcslen(name) + systemLength + 2 > MAX_PATH)
        return false;
    systemPath[systemLength] = L'\\';
    wcscpy_s(systemPath + systemLength + 1, MAX_PATH - systemLength - 1, name);
    // The absolute System32 path prevents recursion into this renamed proxy.
    const HMODULE system = LoadLibraryExW(systemPath, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!system || system == self)
        return false;
    KernelBaseProxy::Init(); // Resolves exports only; does not install hooks.
    if (_wcsicmp(name, L"version.dll") == 0) version.LoadOriginalLibrary(system);
    else if (_wcsicmp(name, L"winmm.dll") == 0) winmm.LoadOriginalLibrary(system);
    else if (_wcsicmp(name, L"wininet.dll") == 0) wininet.LoadOriginalLibrary(system);
    else if (_wcsicmp(name, L"winhttp.dll") == 0) winhttp.LoadOriginalLibrary(system);
    else if (_wcsicmp(name, L"dbghelp.dll") == 0) dbghelp.LoadOriginalLibrary(system);
    else if (_wcsicmp(name, L"dxgi.dll") == 0)
    {
        DxgiProxy::InitSystemForwarding(system);
        dxgi.LoadOriginalLibrary(system);
    }
    else if (_wcsicmp(name, L"d3d12.dll") == 0)
    {
        D3d12Proxy::Init(system);
        d3d12.LoadOriginalLibrary(system);
    }
    return true;
}
