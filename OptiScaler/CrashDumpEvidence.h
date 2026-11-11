// Diagnostic evidence only: no handler installation or exception-policy changes.
#pragma once
#include <Windows.h>
#include <DbgHelp.h>
#include <cstdio>
namespace crashdump {
struct Result { bool written=false; DWORD error=0, sizeError=0; unsigned long long bytes=0; };
inline bool FormatName(wchar_t (&name)[160], bool creationTimeAvailable, const FILETIME& created,
    DWORD processId, DWORD threadId, ULONGLONG tick, LONGLONG sequence) noexcept {
    if (creationTimeAvailable)
        return swprintf_s(name, L"OptiScaler_crash_%lu_%lu_%08lX%08lX_%llu_%lld.dmp", processId,
            threadId, created.dwHighDateTime, created.dwLowDateTime, tick, sequence) > 0;
    return swprintf_s(name, L"OptiScaler_crash_%lu_%lu_creation-unavailable_%llu_%lld.dmp", processId,
        threadId, tick, sequence) > 0;
}
inline bool Name(wchar_t (&name)[160]) noexcept {
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool hasCreationTime = GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE;
    static volatile LONG64 sequence = 0;
    const auto identity = InterlockedIncrement64(&sequence);
    return FormatName(name, hasCreationTime, created, GetCurrentProcessId(), GetCurrentThreadId(), GetTickCount64(), identity);
}
inline Result Write(const wchar_t* path, EXCEPTION_POINTERS* exceptionInfo) noexcept {
    Result r;
    const auto file=CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(file==INVALID_HANDLE_VALUE){r.error=GetLastError();return r;}
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId=GetCurrentThreadId();info.ExceptionPointers=exceptionInfo;info.ClientPointers=FALSE;
    r.written=MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
        static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs|MiniDumpWithUnloadedModules|
            MiniDumpWithThreadInfo|MiniDumpWithIndirectlyReferencedMemory), &info, nullptr, nullptr)!=FALSE;
    if(!r.written)r.error=GetLastError();
    LARGE_INTEGER size{};
    if(GetFileSizeEx(file,&size))r.bytes=static_cast<unsigned long long>(size.QuadPart);
    else r.sizeError=GetLastError();
    CloseHandle(file);return r;
}
}
