#include <runtime/LibraryObservation.h>
#include <runtime/LibraryLoadRequest.h>
#include <cassert>
#include <cstdio>
#include <filesystem>

LibraryLoading::Observations observations;
unsigned applied = 0, callouts = 0, scans = 0;
void Scan() {
    ++scans;
    LibraryLoading::VisitInitialized([](HMODULE module, std::wstring_view name, void*) {
        if (name == L"d3d11.dll" || name == L"loaderdep.dll") observations.Enqueue(module);
    }, nullptr);
}
void Apply(HMODULE module, bool) {
    assert(!LibraryLoading::InLoaderCallout());
    assert(LibraryLoading::Initialized(module));
    ++applied;
}
extern "C" __declspec(dllexport) void OnLoaderCallout(HMODULE module) {
    assert(LibraryLoading::InLoaderCallout()); ++callouts;
    observations.Enqueue(module); observations.RequestScan();
    observations.Process(!LibraryLoading::InLoaderCallout(), Scan, Apply);
    assert(applied == 0); // actual DllMain delivery cannot initialize hooks
}
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    assert(argc == 3);
    const std::filesystem::path root = argv[2];
    const auto path = (root / L"a" / L"d3d11.dll").wstring();
    const bool unresolved = std::wstring_view(argv[1]) == L"unresolved";
    auto module = LoadLibraryExW(path.c_str(), nullptr, unresolved ? DONT_RESOLVE_DLL_REFERENCES : (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32));
    if (!module) { std::fprintf(stderr,"Load probe failed: %lu path=%ls\n", GetLastError(), path.c_str()); return 2; }
    if (unresolved) {
        assert(!LibraryLoading::Initialized(module));
        // A later executable request returning this cached image also cannot
        // grant it initialization or let dependency reconciliation hook it.
        auto cached = LoadLibraryExW(path.c_str(), nullptr, 0); assert(cached == module);
        observations.Enqueue(cached); observations.RequestScan();
        observations.Process(true, Scan, Apply);
        assert(applied == 0 && scans == 1 && callouts == 0);
        FreeLibrary(cached); FreeLibrary(module);
        std::puts("PASS production observer: unresolved and cached unresolved images rejected");
        return 0;
    }
    assert(callouts == 1 && applied == 0 && LibraryLoading::Initialized(module));
    observations.Process(true, Scan, Apply);
    assert(applied == 2 && scans == 1); // direct DLL + its static dependency
    observations.RequestScan(); observations.Process(true, Scan, Apply);
    assert(applied == 2 && scans == 2); // no repeated Streamline-style init
    const auto base = module; FreeLibrary(module);
    assert(GetModuleHandleW(path.c_str()) == base); // installed hook image pinned
    module = LoadLibraryExW(path.c_str(), nullptr, 0); assert(module == base);
    observations.Enqueue(module); observations.Process(true, Scan, Apply);
    assert(applied == 2 && callouts == 1); FreeLibrary(module);
    // Empty pending queue still reconciles an independently requested scan.
    LibraryLoading::Observations empty;
    empty.RequestScan(); unsigned reconciled = 0;
    empty.Process(true, [&] { ++reconciled; empty.Enqueue(base); }, [&](HMODULE, bool) { ++reconciled; });
    assert(reconciled == 2);
    LibraryLoading::Observations retry;
    retry.Enqueue(base); unsigned attempts=0;
    retry.Process(true, [] {}, [&](HMODULE,bool) { ++attempts; return false; });
    assert(attempts==1);
    retry.Process(true, [] {}, [&](HMODULE,bool) { ++attempts; return true; });
    assert(attempts==2);
    retry.RequestScan(); retry.Process(true, [] {}, [&](HMODULE,bool) { ++attempts; return true; });
    assert(attempts==2);
    LibraryLoading::Observations reentrant;
    reentrant.Enqueue(base); unsigned nestedScans=0;
    reentrant.Process(true, [&] { ++nestedScans; }, [&](HMODULE,bool) { reentrant.RequestScan(); return true; });
    assert(nestedScans==1 && !reentrant.HasWork());
    std::puts("PASS production observer: real DllMain deferral, static imports, idempotence, pin/reload and empty-queue scan");
}
