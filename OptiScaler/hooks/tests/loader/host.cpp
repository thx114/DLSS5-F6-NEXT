#include <hooks/LibraryLoad_Hooks.h>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace {
LibraryLoadHooks::LoadPlan selected;
unsigned observations=0, decisions=0, calls=0;
std::wstring observed;
std::wstring calloutPath; DWORD calloutFlags=0; unsigned nestedCalls=0;
}
LibraryLoadHooks::LoadPlan LibraryLoadHooks::Decide(std::wstring_view) { ++decisions; return selected; }
HMODULE LibraryLoadHooks::AcquireAlias(std::wstring_view, HMODULE module) {
    HMODULE result=nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(module),&result) ? result : nullptr;
}
void LibraryLoadHooks::ObserveExecutable(HMODULE module, std::wstring_view) {
    ++observations; wchar_t path[32768]{};
    assert(GetModuleFileNameW(module,path,32768)); observed=path;
    SetLastError(0x12345678);
}
extern "C" __declspec(dllexport) void OnLoaderCallout(HMODULE) {
    if (calloutPath.empty() || nestedCalls) return;
    assert(LibraryLoading::InLoaderCallout());
    auto path=calloutPath;
    UNICODE_STRING name{static_cast<USHORT>(path.size()*sizeof(wchar_t)),static_cast<USHORT>(path.size()*sizeof(wchar_t)),path.data()};
    using Native=NTSTATUS(NTAPI*)(PWSTR,PULONG,PUNICODE_STRING,PHANDLE);
    const auto original=reinterpret_cast<Native>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"LdrLoadDll"));
    ULONG flags=0;HANDLE result=nullptr;
    const auto status=LibraryLoadHooks::LoadNative(&flags,&name,&result,[&](PUNICODE_STRING replacement) {
        ++nestedCalls; return original(nullptr,&flags,replacement?replacement:&name,&result);
    });
    assert(NT_SUCCESS(status) && result && nestedCalls==1);
    FreeLibrary(static_cast<HMODULE>(result));
}
int wmain(int argc,wchar_t** argv) {
    assert(argc==3);
    const std::wstring scenario=argv[1];
    const auto root=std::filesystem::path(argv[2]);
    const auto a=(root/L"a"/L"d3d11.dll").wstring(), b=(root/L"b"/L"d3d11.dll").wstring();
    std::wstring requested=a;
    DWORD flags=LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32;
    HANDLE file=nullptr;
    if(scenario==L"altered")flags=LOAD_WITH_ALTERED_SEARCH_PATH;
    if(scenario==L"system") { requested=L"version.dll"; flags=LOAD_LIBRARY_SEARCH_SYSTEM32; }
    if(scenario==L"user") {
        assert(SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_USER_DIRS|LOAD_LIBRARY_SEARCH_SYSTEM32));
        assert(AddDllDirectory((root/L"b").c_str())); requested=L"d3d11.dll"; flags=LOAD_LIBRARY_SEARCH_USER_DIRS;
    }
    if(scenario==L"redirect"||scenario==L"fallback"||scenario==L"invalid-combination"||scenario==L"relative-load-dir") {
        selected.action=LibraryLoadHooks::LoadAction::RedirectPath;
        selected.replacement=scenario==L"fallback" ? (root/L"missing.dll").wstring() : b;
        selected.fallbackToOriginal=scenario==L"fallback";
    }
    if(scenario==L"invalid-combination")flags|=LOAD_WITH_ALTERED_SEARCH_PATH;
    if(scenario==L"relative-load-dir")requested=L"d3d11.dll";
    if(scenario==L"alias") { selected.action=LibraryLoadHooks::LoadAction::AliasModule; selected.alias=GetModuleHandleW(nullptr); }
    if(scenario==L"reject")selected.action=LibraryLoadHooks::LoadAction::Reject;
    if(scenario==L"data"||scenario==L"cached-data")flags|=LOAD_LIBRARY_AS_DATAFILE;
    if(scenario==L"image")flags|=LOAD_LIBRARY_AS_IMAGE_RESOURCE;
    if(scenario==L"unresolved")flags=DONT_RESOLVE_DLL_REFERENCES;
    if(scenario==L"missing")requested=(root/L"missing.dll").wstring();
    if(scenario==L"invalid-file")file=reinterpret_cast<HANDLE>(1);
    if(scenario==L"signature")flags|=LOAD_LIBRARY_REQUIRE_SIGNED_TARGET;
    if(scenario==L"callout-native")calloutPath=requested;
    HMODULE cached=nullptr;
    if(scenario==L"cached-data") { cached=LoadLibraryExW(a.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR); assert(cached); }
    SetEnvironmentVariableW(L"F6_LOADER_PROBE_EXECUTED",nullptr);
    if(scenario==L"native") {
        // Actual LdrLoadDll consumes a length, with no terminating zero at Length.
        std::wstring nonterminated=(root/L"a"/L"d3d11.dll").wstring();
        const auto length=nonterminated.size(); nonterminated+=L"GARBAGE";
        UNICODE_STRING name{static_cast<USHORT>(length*sizeof(wchar_t)),static_cast<USHORT>(length*sizeof(wchar_t)),nonterminated.data()};
        const auto directory=(root/L"a").wstring();
        using Native=NTSTATUS(NTAPI*)(PWSTR,PULONG,PUNICODE_STRING,PHANDLE);
        auto original=reinterpret_cast<Native>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"LdrLoadDll"));
        ULONG nativeFlags=0; HANDLE result=nullptr;
        const auto status=LibraryLoadHooks::LoadNative(&nativeFlags,&name,&result,[&](PUNICODE_STRING replacement) {
            ++calls;return original(const_cast<PWSTR>(directory.c_str()),&nativeFlags,replacement?replacement:&name,&result);
        });
        assert(NT_SUCCESS(status)&&result&&calls==1&&observations==1);
        FreeLibrary(static_cast<HMODULE>(result));
    } else {
        DWORD baselineError=0; bool failure=scenario==L"missing"||scenario==L"invalid-file"||scenario==L"signature"||
            scenario==L"invalid-combination"||scenario==L"relative-load-dir";
        if(failure) {
            SetLastError(4242); const auto baseline=LoadLibraryExW(requested.c_str(),file,flags);
            baselineError=GetLastError(); assert(!baseline);
        }
        SetLastError(4242);
        auto module=LibraryLoadHooks::LoadWin32(requested,file,flags,[&](LPCWSTR replacement) {
            ++calls; const auto* path=replacement?replacement:requested.c_str();
            if(scenario==L"nested") {
                // Simulate K32 -> KernelBase -> native with the same request.
                return LibraryLoadHooks::LoadWin32(path,file,flags,[&](LPCWSTR nested) {
                    assert(!nested); return LoadLibraryExW(path,file,flags);
                }, LibraryLoading::Layer::KernelBase);
            }
            return LoadLibraryExW(path,file,flags);
        });
        const auto error=GetLastError();
        if(failure) { assert(!module&&error==baselineError&&calls==1&&observations==0); }
        else if(scenario==L"reject") { assert(!module&&calls==0&&observations==0&&error==ERROR_MOD_NOT_FOUND); }
        else {
            assert(module&&calls==(scenario==L"alias"?0u:scenario==L"fallback"?2u:1u));
            const bool executable=LibraryLoading::Executable(flags);
            assert(observations==(executable?(scenario==L"callout-native"?2u:1u):0u));
            if(executable&&scenario!=L"system"&&scenario!=L"alias") {
                const auto identity=reinterpret_cast<int(*)()>(GetProcAddress(module,"Identity"));
                assert(identity&&identity()==((scenario==L"redirect"||scenario==L"user")?222:111));
            }
            if(!executable) {
                assert(!GetEnvironmentVariableW(L"F6_LOADER_PROBE_EXECUTED",nullptr,0));
                assert(decisions==0);
            }
            if(scenario==L"nested")assert(decisions==1);
            if(scenario==L"callout-native")assert(decisions==2 && nestedCalls==1);
            assert(error!=0x12345678); FreeLibrary(module);
        }
    }
    if(cached)FreeLibrary(cached);
    std::printf("PASS actual loader: %ls (%u calls, %u observations)\n",scenario.c_str(),calls,observations);
}
