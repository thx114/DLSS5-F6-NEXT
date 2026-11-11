#include "MaintenanceGate.h"
#include <winhttp.h>
#include <cstring>
#include <cwchar>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "user32.lib")

namespace MaintenanceGate
{
namespace
{
std::uint64_t FileTicks(FILETIME time) noexcept
{
    ULARGE_INTEGER value {};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}
bool RememberedExpiry() noexcept
{
    wchar_t path[1024] {};
    if (!GetLatchPath(path, 1024))
        return false;
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    char value[sizeof(LatchMarker)] {};
    DWORD bytes = 0;
    const bool valid = ReadFile(file, value, sizeof(value), &bytes, nullptr) &&
        bytes == sizeof(LatchMarker) - 1 && memcmp(value, LatchMarker, sizeof(LatchMarker) - 1) == 0;
    CloseHandle(file);
    return valid;
}
void RememberExpiry() noexcept
{
    wchar_t path[1024] {};
    if (!GetLatchPath(path, 1024))
    {
        OutputDebugStringW(L"F5 maintenance: HTTPS expiry found; no fixed local cache path, so proof was NOT persisted.\n");
        return;
    }
    // The directory is created only by this post-DllMain worker.
    wchar_t directory[1024] {};
    wcscpy_s(directory, path);
    if (auto* separator = wcsrchr(directory, L'\\')) *separator = L'\0';
    if (!CreateDirectoryW(directory, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        OutputDebugStringW(L"F5 maintenance: HTTPS expiry found; cache directory failed, so proof was NOT persisted.\n");
        return;
    }
    // Exact fixed marker; readers reject partial/trailing/cross-version data.
    // No settings files, registry keys or game files are changed.
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    bool saved = false;
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD bytes = 0;
        saved = WriteFile(file, LatchMarker, sizeof(LatchMarker) - 1, &bytes, nullptr) &&
                bytes == sizeof(LatchMarker) - 1 && FlushFileBuffers(file);
        CloseHandle(file);
    }
    OutputDebugStringW(saved
        ? L"F5 maintenance: HTTPS expiry persisted; injection will be refused on the next load.\n"
        : L"F5 maintenance: HTTPS expiry found; cache write failed, so proof was NOT persisted.\n");
}
DWORD WINAPI Worker(LPVOID parameter)
{
    const bool rejected = parameter != nullptr;
    if (rejected)
    {
        // Only rejected loads may claim injection is disabled. No ImGui, no
        // render-thread callback and no message box while holding loader lock.
        MessageBoxW(nullptr, Notice, L"OptiScaler", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return 0;
    }
    std::uint64_t ticks = 0;
    if (QueryHttpsDate(L"www.microsoft.com", ticks) || QueryHttpsDate(L"www.cloudflare.com", ticks))
    {
        if (IsExpiredAt(ticks))
            RememberExpiry();
        else
            OutputDebugStringW(L"F5 maintenance: HTTPS Date is before 2026-10-31T16:00:00Z.\n");
    }
    else
        OutputDebugStringW(L"F5 maintenance: HTTPS Date unavailable; local load cutoff remains in force.\n");
    return 0;
}
}

bool GetLatchPath(wchar_t* path, DWORD capacity) noexcept
{
    if (!path || capacity < 4)
        return false;
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", path, capacity);
    constexpr DWORD suffixLength = static_cast<DWORD>(sizeof(LatchSuffix) / sizeof(wchar_t) - 1);
    if (!length || length >= capacity || length + suffixLength >= capacity ||
        !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) ||
        path[1] != L':' || path[2] != L'\\')
        return false; // Never open a UNC, relative, device or unknown cache path.
    wchar_t drive[] { path[0], L':', L'\\', L'\0' };
    if (GetDriveTypeW(drive) != DRIVE_FIXED)
        return false;
    wcscpy_s(path + length, capacity - length, LatchSuffix);
    return true;
}

bool RejectThisLoad() noexcept
{
    FILETIME now {};
    GetSystemTimeAsFileTime(&now);
    // An expired local clock cannot be overridden by a network failure, old
    // Date response, deleted preference or a pre-cutoff trusted observation.
    return IsExpiredAt(FileTicks(now)) || RememberedExpiry();
}

void QueueWorker(HMODULE module, bool rejected) noexcept
{
    // A worker must never execute unmapped code. Retain this build until process
    // exit, including the minimal-forwarding branch. Do not join in DllMain.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(module), &pinned))
        return;
    const HANDLE worker = CreateThread(nullptr, 0, Worker, rejected ? reinterpret_cast<void*>(1) : nullptr,
                                       0, nullptr);
    if (worker)
        CloseHandle(worker);
}

bool QueryHttpsDate(const wchar_t* host, std::uint64_t& ticks) noexcept
{
    // Full TLS verification remains enabled. No redirects or automatic logon;
    // this is a Date-only HEAD request, with no game/user/config data attached.
    HINTERNET session = WinHttpOpen(L"OptiScaler-F5-Maintenance/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return false;
    HINTERNET connection = nullptr, request = nullptr;
    bool valid = false;
    do
    {
        if (!WinHttpSetTimeouts(session, 1500, 1500, 1500, 1500))
            break;
        connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connection)
            break;
        request = WinHttpOpenRequest(connection, L"HEAD", L"/", nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!request)
            break;
        DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        DWORD disabled = WINHTTP_DISABLE_COOKIES;
        if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)) ||
            !WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon)) ||
            !WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)))
            break;
        if (!WinHttpSendRequest(request, L"Cache-Control: no-cache\r\n", static_cast<DWORD>(-1),
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr))
            break;
        DWORD status = 0, bytes = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX) ||
            status < 200 || status >= 400)
            break;
        SYSTEMTIME date {};
        bytes = sizeof(date);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_DATE | WINHTTP_QUERY_FLAG_SYSTEMTIME,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &date, &bytes, WINHTTP_NO_HEADER_INDEX))
            break;
        FILETIME converted {};
        if (!SystemTimeToFileTime(&date, &converted))
            break;
        ticks = FileTicks(converted);
        valid = true;
    } while (false);
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return valid;
}
}
