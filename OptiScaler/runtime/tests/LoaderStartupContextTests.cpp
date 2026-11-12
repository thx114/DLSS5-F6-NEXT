#include <runtime/LoaderStartupContext.h>
#include <genshin/GenshinNativeStartup.h>
#include <atomic>
#include <cstdio>
#include <thread>

namespace
{
constinit std::atomic<int> tlsContext { -1 };
constinit std::atomic<ULONG> tlsProcessFlags { 0xffffffff };

LoaderStartupContext::Status CurrentContext()
{
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto probe = module ? reinterpret_cast<LoaderStartupContext::ProbeFunction>(
        GetProcAddress(module, "RtlIsThreadWithinLoaderCallout")) : nullptr;
    return LoaderStartupContext::Probe(module, probe);
}
}

extern "C" void NTAPI ObserveStartupTls(void*, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        ULONG flags = 0xffffffff;
        SIZE_T read = 0;
        const auto peb = static_cast<uintptr_t>(__readgsqword(0x60));
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(peb + 0x50), &flags, sizeof(flags), &read);
        tlsProcessFlags.store(flags, std::memory_order_relaxed);
        tlsContext.store(static_cast<int>(CurrentContext()), std::memory_order_relaxed);
    }
}

#pragma section(".CRT$XLB", long, read)
extern "C" __declspec(allocate(".CRT$XLB")) PIMAGE_TLS_CALLBACK loaderContextTls = ObserveStartupTls;
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:loaderContextTls")

int main()
{
    using Status = LoaderStartupContext::Status;
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* message) {
        ++checks;
        if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
    };
    const auto tls = tlsContext.load();
    check(tls == static_cast<int>(Status::LoaderCallout) || tls == static_cast<int>(Status::ProbeUnavailable),
          "actual executable TLS callback must refuse startup waiting");
    check(CurrentContext() == Status::Ready, "ordinary main thread may wait after loader startup");
    check(GenshinNativeStartup::ProbeLoaderCallout() == GenshinNativeStartup::Status::Ready,
          "native startup wrapper preserves the shared context result");
    Status worker = Status::ProbeUnavailable;
    std::thread([&] { worker = CurrentContext(); }).join();
    check(worker == Status::Ready, "ordinary worker may wait outside its thread attach callback");
    check(LoaderStartupContext::Probe(nullptr, nullptr) == Status::ProbeUnavailable,
          "missing runtime context cannot authorize waiting");
    std::printf("LoaderStartupContext: %u checks, %u failures; TLS context=%d (1=callout, 2=unavailable); "
                "process flags=0x%lx; real Windows TLS/main/worker, no external module execution\n",
                checks, failures, tls, tlsProcessFlags.load());
    return failures ? 1 : 0;
}
