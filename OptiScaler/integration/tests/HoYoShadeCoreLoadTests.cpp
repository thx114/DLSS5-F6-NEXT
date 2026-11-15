#include <Windows.h>
#include "../HoYoShadeCoreLoad.h"
#include <cstdio>
#include <thread>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 2;
    unsigned checks = 0;
    const auto check = [&](bool value, const char* message) {
        ++checks;
        if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ExitProcess(1); }
    };
    // The caller's last no-core snapshot is deliberately stale: the external
    // loader wins before our LoadLibrary. Module equality used to claim F6.
    HMODULE external = nullptr;
    std::thread first([&] { external = LoadLibraryW(argv[1]); });
    first.join();
    check(external != nullptr, "external first load");
    {
        HoYoShade::CoreLoad::Witness witness;
        const auto returned = witness.Load(argv[1]);
        check(returned == external, "LoadLibrary returns the externally loaded module");
        check(!witness.FirstLoad(returned), "external winner never acquires managed ownership");
        FreeLibrary(returned);
    }
    FreeLibrary(external);
    check(GetModuleHandleW(L"ReShade64.dll") == nullptr, "fixture unloaded");
    {
        HoYoShade::CoreLoad::Witness witness;
        const auto loaded = witness.Load(argv[1]);
        check(loaded != nullptr, "managed first load");
        check(witness.FirstLoad(loaded), "actual first-load notification certifies ownership");
        HMODULE otherThread = nullptr;
        std::thread later([&] { otherThread = LoadLibraryW(argv[1]); });
        later.join();
        check(otherThread == loaded, "later external caller shares the same image");
        FreeLibrary(otherThread);
        FreeLibrary(loaded);
    }
    std::printf("PASS real Windows core load ownership: %u checks\n", checks);
}
