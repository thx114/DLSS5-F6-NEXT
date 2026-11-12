"""Exercise the production generic loader with disposable Windows DLL fixtures.

Only generated fixtures in a TemporaryDirectory are loaded. No installed ASI,
game, SDK, deployment, full build or shared test runner is accessed.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
SOURCE = ROOT / "OptiScaler/runtime/AsiPluginStartup.cpp"
main = (ROOT / "OptiScaler/dllmain.cpp").read_text(encoding="utf-8-sig")
attach = main[main.index("BOOL APIENTRY DllMain"):]
worker = main[main.index("DWORD WINAPI getGpuInfo"):main.index("BOOL APIENTRY DllMain")]
selection = main[main.index("void LoadAsiPlugins()"):main.index("// nvapi64.dll")]
assert "AsiPluginStartup::Configure(" in selection
assert "PluginPath.value_or" in selection and "LateAsiPluginsDelay.value_or_default" in selection
assert "AsiPluginStartup::Initialize();" not in attach
assert "AsiPluginStartup::WorkerUnavailable();" in attach
assert "LoadAsiPlugins.value_or_default()" in attach
assert worker.index("AsiPluginStartup::Initialize();") < worker.index("HoYoShade::Initialize")
assert "InitializeASI" not in main and "PatchResult" not in main
assert "isOptiPatcherSucceed = true" in selection and "set_volatile_value(false)" in selection
for forbidden in ("Mfg", "mfg", "Unlock", "OwnsName"):
    assert forbidden not in SOURCE.read_text() + selection, forbidden

# Keep the real consumer routing under test too: only an effective FG feature
# request waits; NGX SR/NR remain independent of the optional plugin queue.
streamline = (ROOT / "OptiScaler/proxies/Streamline_Proxy.h").read_text(encoding="utf-8-sig")
ngx = (ROOT / "OptiScaler/proxies/NVNGX_Proxy.h").read_text(encoding="utf-8-sig")
sl_hook = (ROOT / "OptiScaler/hooks/Streamline_Hooks.cpp").read_text(encoding="utf-8-sig")
game_init = sl_hook[sl_hook.index("sl::Result StreamlineHooks::hkslInit("):
                    sl_hook.index("sl::Result StreamlineHooks::hkslIsFeatureSupported(")]
assert "if (!AsiPluginStartup::AwaitGraphicsStartup()) return false;" in streamline
assert "AsiPluginStartup::AwaitGraphicsStartup" not in ngx
assert game_init.index("AsiPluginStartup::AwaitGraphicsStartup()") > game_init.index(
    "std::erase(localFeaturesToLoad, sl::kFeatureDLSS_G)")
assert game_init.index("AsiPluginStartup::AwaitGraphicsStartup()") > game_init.index(
    "return o_slInit(localPref, sdkVersion);")
game_tail = game_init[game_init.index("    if (State::Instance().activeFgInput == FGInput::DLSSG"):]
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
    ++calls;
    if (pref.featuresToLoad)
        consumed.assign(pref.featuresToLoad, pref.featuresToLoad + pref.numFeaturesToLoad);
    return sl::Result::eOk;
}
sl::Result ProductionTail(const sl::Preferences& pref) {
    auto localPref = pref;
    constexpr uint64_t sdkVersion = 0;
'''
routing += game_tail.replace("AsiPluginStartup::", "ConsumerFixture::")
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
    State::Instance().activeFgInput = FGInput::NoFG;
    State::Instance().activeFgOutput = FGOutput::NoFG;
    ConsumerFixture::ready = false; ConsumerFixture::waits = calls = 0;
    if (ProductionTail({nullptr, 0}) != sl::Result::eOk || ConsumerFixture::waits || calls != 1)
        throw std::runtime_error("empty effective features must not wait");
    ++checks;
    std::printf("production effective-feature routing PASS: %u checks\n", checks);
}
'''

fixture = r'''
#include <Windows.h>
#include <stdexcept>
using Event = void (*)(unsigned, unsigned);
void Notify(unsigned kind) {
    auto event = reinterpret_cast<Event>(GetProcAddress(GetModuleHandleW(nullptr), "FixtureEvent"));
    if (event) event(PLUGIN_ID, kind);
}
#ifndef OMIT_ENTRY
extern "C" __declspec(dllexport) void InitializeASI() {
    Notify(0);
#ifdef THROW_CPP
    throw std::runtime_error("fixture entry exception");
#endif
#ifdef THROW_SEH
    RaiseException(0xe1234567, 0, 0, nullptr);
#endif
}
#endif
extern "C" __declspec(dllexport) bool PatchResult() {
    Notify(1);
#ifdef THROW_PATCH
    throw std::runtime_error("fixture patch exception");
#else
    return (PLUGIN_ID % 2) != 0;
#endif
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
'''

probe = r'''
#include <Windows.h>
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        auto probe = reinterpret_cast<void (*)()>(GetProcAddress(GetModuleHandleW(nullptr), "FixtureLoaderProbe"));
        if (probe) probe();
    }
    return TRUE;
}
'''

host = r'''
#include <Windows.h>
#include <TlHelp32.h>
#include <runtime/AsiPluginStartup.h>
#include <runtime/ModuleLifetime.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

std::array<std::atomic<unsigned>, 16> entries {}, patches {};
std::atomic<unsigned> checks { 0 }, unsafe { 0 }, patched { 0 }, lateThreads { 0 }, probes { 0 };
std::atomic<bool> block { false }, entered { false }, releaseEntry { false };
std::atomic<bool> failThread { false }, failInventory { false };
std::atomic<bool> badReentry { false }, throwCallback { false };
DWORD mainThread = 0;
ULONGLONG started = 0;
std::atomic<ULONGLONG> firstLate { 0 };
std::filesystem::path removeOnEntry;

void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

HANDLE FixtureCreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE start,
                           LPVOID arg, DWORD flags, LPDWORD id) {
    ++lateThreads;
    if (failThread) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    return ::CreateThread(sa, stack, start, arg, flags, id);
}
HANDLE FixtureSnapshot(DWORD flags, DWORD id) {
    if (failInventory) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
    return ::CreateToolhelp32Snapshot(flags, id);
}

extern "C" __declspec(dllexport) void FixtureEvent(unsigned id, unsigned kind) {
    if (ModuleLifetime::CurrentContext() != LoaderStartupContext::Status::Ready ||
        mainThread == GetCurrentThreadId()) ++unsafe;
    if (!kind) ++entries[id]; else ++patches[id];
    // Real plugin -> host -> loader reentry, on both ordinary and late paths.
    AsiPluginStartup::Initialize();
    if (!lateThreads && AsiPluginStartup::AwaitGraphicsStartup(0)) badReentry = true;
    if (id == 6 || id == 7) {
        ULONGLONG zero = 0;
        firstLate.compare_exchange_strong(zero, GetTickCount64());
    }
    if (id == 1 && !kind) {
        if (!removeOnEntry.empty()) std::filesystem::remove(removeOnEntry);
        if (block) {
            entered = true;
            while (!releaseEntry) std::this_thread::yield();
        }
    }
}
extern "C" __declspec(dllexport) void FixtureLoaderProbe() {
    ++probes;
    Check(ModuleLifetime::CurrentContext() != LoaderStartupContext::Status::Ready, "real loader callout");
    AsiPluginStartup::Initialize();
    Check(!AsiPluginStartup::AwaitGraphicsStartup(0), "loader callout cannot wait or consume queue");
}
void Patched() {
    ++patched;
    if (throwCallback) throw std::runtime_error("fixture patch callback");
}
void RunWorker() { std::thread([] { AsiPluginStartup::Initialize(); }).join(); }
bool WaitFor(const std::atomic<unsigned>& value, unsigned expected) {
    const auto deadline = GetTickCount64() + 4000;
    while (value.load() < expected && GetTickCount64() < deadline) Sleep(2);
    return value.load() == expected;
}

int wmain(int argc, wchar_t** argv) {
try {
    Check(argc == 4, "arguments");
    const std::wstring scenario = argv[1];
    const std::filesystem::path directory = argv[2], assets = argv[3];
    mainThread = GetCurrentThreadId();
    ModuleLifetime::InitializeContextProbe();
    Check(ModuleLifetime::CurrentContext() == LoaderStartupContext::Status::Ready, "host context ready");
    if (scenario == L"disabled") {
        RunWorker(); AsiPluginStartup::WorkerUnavailable();
        Check(AsiPluginStartup::AwaitGraphicsStartup(0), "disabled queue is terminal");
        Check(entries[1] == 0 && lateThreads == 0, "disabled never scans or starts threads");
    } else {
        if (scenario == L"preloaded" || scenario == L"preloaded-hardlink" || scenario == L"preloaded-other")
            Check(LoadLibraryW((directory.parent_path() / L"preloaded.dll").c_str()) != nullptr, "preload fixture");
        AsiPluginStartup::Configure(directory, scenario == L"late-zero" ? -1 : 1, Patched);
        Check(!AsiPluginStartup::AwaitGraphicsStartup(1), "configured queue initially pending");
        if (scenario == L"worker-failure") {
            AsiPluginStartup::WorkerUnavailable(); RunWorker();
            Check(entries[1] == 0 && AsiPluginStartup::AwaitGraphicsStartup(0), "failed worker is terminal, no load");
        } else {
            if (scenario == L"loader") {
                Check(LoadLibraryW((assets / L"probe.dll").c_str()) != nullptr, "load callout probe");
                Check(probes == 1 && entries[1] == 0, "callout did not load plugin");
                Check(!AsiPluginStartup::AwaitGraphicsStartup(0), "callout preserves pending work");
            }
            if (scenario == L"unknown-context") {
                const auto probe = ModuleLifetime::contextProbe.exchange(nullptr);
                RunWorker();
                Check(entries[1] == 0 && !AsiPluginStartup::AwaitGraphicsStartup(0), "unknown context defers startup");
                ModuleLifetime::contextProbe.store(probe);
            }
            if (scenario == L"late-thread-failure") failThread = true;
            if (scenario == L"inventory-failure") failInventory = true;
            if (scenario == L"patch-callback") throwCallback = true;
            if (scenario == L"removed") removeOnEntry = directory / L"B.asi";
            started = GetTickCount64();
            if (scenario == L"timeout") {
                block = true;
                std::jthread worker([] { AsiPluginStartup::Initialize(); });
                const auto deadline = GetTickCount64() + 3000;
                while (!entered && GetTickCount64() < deadline) Sleep(1);
                const bool wasEntered = entered;
                const bool timedOut = !AsiPluginStartup::AwaitGraphicsStartup(5);
                AsiPluginStartup::WorkerUnavailable(); // Cannot overwrite active work.
                releaseEntry = true;
                worker.join();
                Check(wasEntered && timedOut, "real entry blocks barrier until it returns");
            } else if (scenario == L"concurrent") {
                std::vector<std::jthread> workers;
                for (unsigned i = 0; i != 12; ++i) workers.emplace_back([] { AsiPluginStartup::Initialize(); });
            } else RunWorker();
            Check(AsiPluginStartup::AwaitGraphicsStartup(20), "terminal queue permits SDK's own support check");
            RunWorker(); // Queue and late batch must remain once-only.
            AsiPluginStartup::Configure(assets, 0, Patched); // Selection is immutable after attach.
            if (scenario == L"normal") {
                Check(entries[1] == 1 && entries[2] == 1 && entries[3] == 0, "multiple arbitrary plugins and optional entry");
                Check(patches[1] == 1 && patches[2] == 1 && patches[3] == 1 && patched == 2,
                      "PatchResult true/false and patch-only plugin preserved");
            } else if (scenario == L"failures") {
                Check(entries[4] == 1 && entries[5] == 1 && entries[10] == 1 && entries[2] == 1,
                      "C++ and hardware entry faults do not prevent later plugins");
                Check(patches[4] == 0 && patches[5] == 0 && patches[10] == 1 && patches[2] == 1,
                      "failed entry skips PatchResult; patch fault is contained");
            } else if (scenario == L"missing" || scenario == L"empty" || scenario == L"invalid" ||
                       scenario == L"inventory-failure") {
                Check(entries[1] == 0 && lateThreads == 0, "optional failures do not load or block");
            } else if (scenario == L"preloaded" || scenario == L"preloaded-hardlink") {
                Check(entries[1] == 0 && patches[1] == 0, "already loaded identity never initialized again");
                Check(entries[2] == 1, "unrelated existing modules cannot suppress other plugins");
            } else if (scenario == L"preloaded-other") {
                Check(entries[1] == 1, "different already loaded content does not suppress plugin");
            } else if (scenario == L"late" || scenario == L"late-zero" || scenario == L"late-exception") {
                if (scenario == L"late") {
                    Check(entries[1] == 1 && entries[6] == 0 && entries[7] == 0,
                          "ordinary barrier excludes explicit delayed plugins");
                }
                Check(WaitFor(entries[6], 1) && WaitFor(patches[7], 1), "all delayed plugins initialize");
                // Wait past the original duplicate-thread window, then observe counters.
                Sleep(50);
                Check(entries[6] == 1 && entries[7] == 1 && lateThreads == 1, "one late thread and one call per plugin");
                if (scenario != L"late-zero") Check(firstLate >= started + 900, "late delay is preserved");
                if (scenario == L"late") Check(entries[1] == 1, "late aliases do not reinitialize ordinary plugin");
                if (scenario == L"late-exception") Check(entries[4] == 1 && patches[4] == 0, "late fault does not stop batch");
            } else if (scenario == L"late-thread-failure") {
                Check(entries[1] == 1 && entries[6] == 0 && lateThreads == 1, "failed late worker cannot block ordinary barrier");
            } else if (scenario == L"removed") {
                Check(entries[1] == 1 && entries[2] == 0 && patches[3] == 1, "missing queued file does not stop later plugins");
            } else if (scenario == L"patch-callback") {
                Check(entries[1] == 1 && entries[2] == 1 && patched == 1, "callback exception does not stop queue");
            } else {
                Check(entries[1] == 1 && patches[1] == 1, "arbitrary plugin initializes exactly once");
            }
        }
    }
    Check(unsafe == 0, "all plugin exports run outside loader callouts and attach thread");
    Check(!badReentry, "recursive plugin cannot await its own queue");
    std::printf("ASI fixture PASS: %ls (%u checks)\n", scenario.c_str(), checks.load());
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
}
}
'''

with tempfile.TemporaryDirectory(prefix="asi-plugin-startup-") as temp:
    directory = Path(temp)
    (directory / "proxies").mkdir()
    (directory / "pch.h").write_text(
        '#pragma once\n#include <Windows.h>\n#include <TlHelp32.h>\n'
        'HANDLE FixtureCreateThread(LPSECURITY_ATTRIBUTES,SIZE_T,LPTHREAD_START_ROUTINE,LPVOID,DWORD,LPDWORD);\n'
        'HANDLE FixtureSnapshot(DWORD,DWORD);\n'
        '#define CreateThread FixtureCreateThread\n#define CreateToolhelp32Snapshot FixtureSnapshot\n')
    (directory / "Logger.h").write_text(
        '#pragma once\ntemplate<class... Args> void FixtureLog(Args&&...) {}\n'
        '#define LOG_INFO(...) FixtureLog(__VA_ARGS__)\n#define LOG_ERROR(...) FixtureLog(__VA_ARGS__)\n')
    (directory / "proxies/Ntdll_Proxy.h").write_text(
        '#pragma once\n#include <Windows.h>\nstruct NtdllProxy { static HMODULE LoadLibraryExW_Ldr'
        '(LPCWSTR p,HANDLE h,DWORD f) { return LoadLibraryExW(p,h,f); } };\n')
    (directory / "proxies/KernelBase_Proxy.h").write_text(
        '#pragma once\n#include <Windows.h>\nstruct KernelBaseProxy {'
        ' static auto GetProcAddress_() { return &::GetProcAddress; } };\n')
    for name, content in (("fixture.cpp", fixture), ("probe.cpp", probe), ("host.cpp", host),
                          ("routing.cpp", routing)):
        (directory / name).write_text(content)
    variants = {1: "", 2: "", 3: "/DOMIT_ENTRY", 4: "/DTHROW_CPP", 5: "/DTHROW_SEH",
                6: "", 7: "", 10: "/DTHROW_PATCH"}
    commands = [
        '@echo off',
        'call "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64',
        'if errorlevel 1 exit /b %errorlevel%',
    ]
    for plugin_id, flags in variants.items():
        commands += [f'cl /nologo /std:c++20 /EHs /MD /W4 /WX /LD /DPLUGIN_ID={plugin_id} '
                     f'{flags} fixture.cpp /link /OUT:fixture{plugin_id}.dll',
                     'if errorlevel 1 exit /b %errorlevel%']
    commands += [
        'cl /nologo /std:c++20 /EHsc /MD /W4 /WX /LD probe.cpp /link /OUT:probe.dll',
        'if errorlevel 1 exit /b %errorlevel%',
        'cl /nologo /std:c++20 /EHsc /MD /W4 /WX routing.cpp /Fe:routing.exe',
        'if errorlevel 1 exit /b %errorlevel%',
        f'cl /nologo /std:c++20 /EHsc /MD /W4 /WX /I"{directory}" /I"{ROOT / "OptiScaler"}" '
        f'host.cpp "{SOURCE}" /Fe:host.exe',
        'if errorlevel 1 exit /b %errorlevel%',
        f'cl /nologo /std:c++20 /EHsc /MD /W4 /WX /I"{ROOT / "OptiScaler"}" '
        f'"{HERE / "PluginStartupGateTests.cpp"}" /Fe:gate.exe',
        'exit /b %errorlevel%',
    ]
    batch = directory / "compile.cmd"
    batch.write_text("\n".join(commands) + "\n", encoding="utf-8", newline="\r\n")
    subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=directory, check=True, timeout=90)
    subprocess.run([str(directory / "gate.exe")], check=True, timeout=20)
    subprocess.run([str(directory / "routing.exe")], check=True, timeout=20)

    scenarios = {
        "disabled": {"A.asi": 1},
        "normal": {"A.asi": 1, "B.ASI": 2, "optional.asi": 3, "ignored.dll": 6},
        "failures": {"A.asi": 4, "B.asi": 5, "C.asi": 10, "D.asi": None, "E.asi": 2},
        "missing": {}, "empty": {}, "invalid": {"bad.asi": None},
        "worker-failure": {"A.asi": 1}, "timeout": {"A.asi": 1},
        "loader": {"A.asi": 1}, "unknown-context": {"A.asi": 1},
        "concurrent": {"A.asi": 1}, "inventory-failure": {"A.asi": 1},
        "duplicates": {"A.asi": 1, "renamed.asi": 1},
        "preloaded": {"candidate.asi": 1, "other.asi": 2},
        "preloaded-hardlink": {"candidate.asi": 1, "other.asi": 2},
        "preloaded-other": {"candidate.asi": 1},
        "late": {"ordinary.asi": 1, "first-loadlate.asi": 6,
                 "second-LOADLATE.ASI": 7, "alias-loadlate.asi": 1},
        "late-zero": {"first-loadlate.asi": 6, "second-loadlate.asi": 7},
        "late-exception": {"A-loadlate.asi": 4, "B-loadlate.asi": 6, "C-loadlate.asi": 7},
        "late-thread-failure": {"ordinary.asi": 1, "first-loadlate.asi": 6, "second-loadlate.asi": 7},
        "removed": {"A.asi": 1, "B.asi": 2, "C.asi": 3},
        "patch-callback": {"A.asi": 1, "B.asi": 2},
    }
    for scenario, plugins in scenarios.items():
        case = directory / scenario
        target = case / "plugins"
        target.mkdir(parents=True)
        for filename, plugin_id in plugins.items():
            if plugin_id is None:
                (target / filename).write_bytes(b"not a PE image")
            else:
                shutil.copyfile(directory / f"fixture{plugin_id}.dll", target / filename)
        (target / "directory.asi").mkdir()
        if scenario == "duplicates":
            os.link(target / "A.asi", target / "hardlink.asi")
        if scenario.startswith("preloaded"):
            if scenario == "preloaded-hardlink":
                os.link(target / "candidate.asi", case / "preloaded.dll")
            else:
                shutil.copyfile(directory / f"fixture{2 if scenario == 'preloaded-other' else 1}.dll",
                                case / "preloaded.dll")
        if scenario == "missing":
            shutil.rmtree(target)
        subprocess.run([str(directory / "host.exe"), scenario, str(target), str(directory)],
                       cwd=directory, check=True, timeout=15)
    print(f"PASS: production generic startup, queue gate and {len(scenarios)} Windows fixture scenarios")
