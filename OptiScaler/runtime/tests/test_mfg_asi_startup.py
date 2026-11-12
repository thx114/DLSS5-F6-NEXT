"""Compile the production ASI startup against real Windows loader/Toolhelp and a harmless fixture.

Never loads the user's ASI and does not start or inspect games. SDK output is untested.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
main = (ROOT / 'OptiScaler/dllmain.cpp').read_text(encoding='utf-8-sig')
streamline = (ROOT / 'OptiScaler/proxies/Streamline_Proxy.h').read_text(encoding='utf-8-sig')
ngx = (ROOT / 'OptiScaler/proxies/NVNGX_Proxy.h').read_text(encoding='utf-8-sig')
assert 'MfgUnlock::SkipExternalPlugin(fileName,' in main
scan = main[main.index('if (MfgAsiStartup::OwnsName(fileName))'):main.index('HMODULE hMod = nullptr;')]
assert 'MfgAsiStartup::Select(entry.path());' in scan and 'continue;' in scan
worker = main[main.index('DWORD WINAPI getGpuInfo'):main.index('BOOL APIENTRY DllMain')]
assert worker.index('MfgAsiStartup::Initialize();') < worker.index('HoYoShade::Initialize')
attach = main[main.index('BOOL APIENTRY DllMain'):]
assert 'MfgAsiStartup::WorkerUnavailable();' in attach
assert 'MfgAsiStartup::Initialize();' not in attach
assert 'if (!MfgAsiStartup::AwaitGraphicsStartup()) return false;' in streamline
sl_hook = (ROOT / 'OptiScaler/hooks/Streamline_Hooks.cpp').read_text(encoding='utf-8-sig')
game_init = sl_hook[sl_hook.index('sl::Result StreamlineHooks::hkslInit('):sl_hook.index('sl::Result StreamlineHooks::hkslIsFeatureSupported(')]
assert game_init.index('MfgAsiStartup::AwaitGraphicsStartup()') > game_init.index('std::erase(localFeaturesToLoad, sl::kFeatureDLSS_G)')
assert game_init.index('MfgAsiStartup::AwaitGraphicsStartup()') > game_init.index('return o_slInit(localPref, sdkVersion);')
assert 'std::find(localPref.featuresToLoad' in game_init
assert 'MfgAsiStartup::AwaitGraphicsStartup' not in ngx, 'SR/NR initialization stays independent of ASI failure'

# Execute the actual effective-feature tail, rather than reproducing its routing
# predicate in a test. Only the wait dependency is replaced by a counted seam.
game_tail = game_init[game_init.index('    if (State::Instance().activeFgInput == FGInput::DLSSG'):]
routing = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>
enum class FGInput { NoFG, DLSSG };
enum class FGOutput { NoFG, DLSSG };
struct State {
    FGInput activeFgInput = FGInput::NoFG;
    FGOutput activeFgOutput = FGOutput::NoFG;
    static State& Instance() { static State state; return state; }
};
namespace sl {
using Feature = unsigned;
constexpr Feature kFeatureDLSS_G = 1;
enum class Result { eOk, eErrorNotInitialized };
struct Preferences { const Feature* featuresToLoad; size_t numFeaturesToLoad; };
}
namespace ConsumerFixture {
bool ready = true; unsigned waits = 0;
bool AwaitGraphicsStartup() { ++waits; return ready; }
}
unsigned calls = 0;
std::vector<sl::Feature> consumed;
sl::Result o_slInit(const sl::Preferences& pref, uint64_t) {
    ++calls; consumed.assign(pref.featuresToLoad, pref.featuresToLoad + pref.numFeaturesToLoad);
    return sl::Result::eOk;
}
sl::Result ProductionTail(const sl::Preferences& pref) {
    auto localPref = pref;
    constexpr uint64_t sdkVersion = 0;
'''
routing += game_tail.replace('MfgAsiStartup::', 'ConsumerFixture::')
routing += r'''
int main() {
    unsigned checks = 0;
    for (bool inputOwner : { false, true }) for (bool outputOwner : { false, true })
    for (bool gameFg : { false, true }) for (bool ready : { false, true }) {
        const sl::Feature features[] = { 2, gameFg ? 1u : 3u };
        const sl::Preferences pref { features, 2 };
        State::Instance().activeFgInput = inputOwner ? FGInput::DLSSG : FGInput::NoFG;
        State::Instance().activeFgOutput = outputOwner ? FGOutput::DLSSG : FGOutput::NoFG;
        ConsumerFixture::ready = ready; ConsumerFixture::waits = calls = 0; consumed.clear();
        const bool waits = gameFg && !inputOwner && !outputOwner;
        const bool accepted = !waits || ready;
        const auto result = ProductionTail(pref);
        if ((result == sl::Result::eOk) != accepted || ConsumerFixture::waits != unsigned(waits) ||
            calls != unsigned(accepted) || (accepted && consumed.front() != 2) ||
            (accepted && (inputOwner || outputOwner) &&
             std::find(consumed.begin(), consumed.end(), sl::kFeatureDLSS_G) != consumed.end()) ||
            features[1] != (gameFg ? 1u : 3u)) throw std::runtime_error("effective-feature routing");
        ++checks;
    }
    std::printf("production effective-feature routing PASS: %u cases\n", checks);
}
'''

fixture = r'''
#include <Windows.h>
static unsigned count = 0;
extern "C" __declspec(dllexport) bool MfgUnlockCoreLoaded() { return true; }
#ifndef OMIT_ENTRY
extern "C" __declspec(dllexport) void InitializeASI() { ++count; }
#endif
extern "C" __declspec(dllexport) unsigned TestInitCount() { return count; }
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
'''
host = r'''
#include <Windows.h>
#include <runtime/MfgAsiStartup.h>
#include <runtime/ModuleLifetime.h>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
void Check(bool result, const char* message) { if(!result) throw std::runtime_error(message); }
int wmain(int argc, wchar_t** argv) {
try {
    Check(argc == 3, "arguments");
    const std::wstring scenario = argv[1];
    const std::filesystem::path path = argv[2];
    ModuleLifetime::InitializeContextProbe();
    Check(MfgAsiStartup::OwnsName(L"mfgunlocker.asi") &&
          MfgAsiStartup::OwnsName(L"mfgunlocker-loadlate.asi") &&
          !MfgAsiStartup::OwnsName(L"other.asi"), "plugin aliases routed consistently");
    if(scenario == L"disabled") {
        MfgAsiStartup::Initialize(); MfgAsiStartup::WorkerUnavailable();
        Check(MfgAsiStartup::AwaitGraphicsStartup(0), "disabled path remains ready");
        Check(!GetModuleHandleW(path.c_str()), "disabled path did not load plugin");
    } else {
        if(scenario == L"duplicate") Check(LoadLibraryW(path.c_str()) != nullptr, "preload fixture");
        MfgAsiStartup::Select(path);
        Check(!MfgAsiStartup::AwaitGraphicsStartup(1), "pending startup is not ready");
        if(scenario == L"worker") MfgAsiStartup::WorkerUnavailable();
        MfgAsiStartup::Initialize();
        if(scenario == L"success") {
            Check(MfgAsiStartup::AwaitGraphicsStartup(), "successful entry permits consumer");
            MfgAsiStartup::Initialize();
            auto module = GetModuleHandleW(path.c_str());
            auto count = reinterpret_cast<unsigned(*)()>(GetProcAddress(module, "TestInitCount"));
            Check(count && count() == 1, "entry called once");
        } else {
            Check(!MfgAsiStartup::AwaitGraphicsStartup(), "failed startup refuses consumers");
            if(scenario == L"duplicate") {
                auto count = reinterpret_cast<unsigned(*)()>(GetProcAddress(GetModuleHandleW(path.c_str()), "TestInitCount"));
                Check(count && count() == 0, "existing owner was not initialized again");
            }
        }
    }
    std::puts("production ASI startup PASS");return 0;
} catch(const std::exception& error) { std::fprintf(stderr,"FAIL: %s\n",error.what());return 1; }
}
'''
with tempfile.TemporaryDirectory(prefix='.mfg-startup-', dir=HERE) as temp:
    directory = Path(temp)
    (directory / 'proxies').mkdir()
    (directory / 'pch.h').write_text('#pragma once\n#include <Windows.h>\n#include <filesystem>\n#include <cwchar>\n#define LOG_INFO(...) ((void)0)\n')
    (directory / 'Logger.h').write_text('#pragma once\n#define LOG_INFO(...) ((void)0)\n')
    (directory / 'proxies/Ntdll_Proxy.h').write_text(
        '#pragma once\n#include <Windows.h>\nstruct NtdllProxy { static HMODULE LoadLibraryExW_Ldr(LPCWSTR p,HANDLE h,DWORD f) { return LoadLibraryExW(p,h,f); } };\n')
    (directory / 'proxies/KernelBase_Proxy.h').write_text(
        '#pragma once\n#include <Windows.h>\nstruct KernelBaseProxy { static auto GetProcAddress_() { return &::GetProcAddress; } };\n')
    (directory / 'fixture.cpp').write_text(fixture)
    (directory / 'host.cpp').write_text(host)
    (directory / 'routing.cpp').write_text(routing)
    batch = directory / 'compile.cmd'
    batch.write_text(
        '@echo off\ncall "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64\n'
        'if errorlevel 1 exit /b %errorlevel%\n'
        'cl /nologo /std:c++20 /EHsc /W4 /WX /LD fixture.cpp /link /OUT:MFGUnlocker.asi\n'
        'if errorlevel 1 exit /b %errorlevel%\n'
        'cl /nologo /std:c++20 /EHsc /W4 /WX /LD /DOMIT_ENTRY fixture.cpp /link /OUT:NoEntry.asi\n'
        'if errorlevel 1 exit /b %errorlevel%\n'
        'cl /nologo /std:c++20 /EHsc /W4 /WX routing.cpp /Fe:routing.exe\n'
        'if errorlevel 1 exit /b %errorlevel%\n'
        f'cl /nologo /std:c++20 /EHsc /W4 /WX /I"{directory}" /I"{ROOT / "OptiScaler"}" '
        f'host.cpp "{ROOT / "OptiScaler/runtime/MfgAsiStartup.cpp"}" /Fe:host.exe\n'
        'exit /b %errorlevel%\n', encoding='utf-8', newline='\r\n')
    subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=directory, check=True)
    subprocess.run([str(directory / 'routing.exe')], cwd=directory, check=True)
    (directory / 'Invalid.asi').write_bytes(b'not a PE module')
    for scenario, filename in [('disabled','MFGUnlocker.asi'),('success','MFGUnlocker.asi'),
                               ('duplicate','MFGUnlocker.asi'),('missing','Missing.asi'),
                               ('worker','MFGUnlocker.asi'),('entry','NoEntry.asi'),('invalid','Invalid.asi')]:
        subprocess.run([str(directory / 'host.exe'), scenario, str(directory / filename)],
                       cwd=directory, check=True, timeout=20)
print('PASS: production startup wiring and seven real Windows loader scenarios (harmless fixture only)')
