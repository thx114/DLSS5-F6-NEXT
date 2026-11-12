"""Check and execute the two production Release prefixes without COM/SDK cleanup."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--devcmd", type=Path, default=Path("D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat"))
args = parser.parse_args()
directory = Path(__file__).resolve().parent
root = directory.parents[2]


def body(text, signature):
    start = text.index("{", text.index(signature))
    depth, end = 1, start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


prefixes = []
for path, owner in (
    ("OptiScaler/with_dx12/dx11_with_dx12_sc.cpp", "Dx11wDx12SC"),
    ("OptiScaler/wrapped/wrapped_swapchain.cpp", "WrappedIDXGISwapChain4"),
):
    source = (root / path).read_text(encoding="utf-8-sig")
    baseline = subprocess.check_output(["git", "show", f"1c4e103:{path}"], cwd=root).decode("utf-8-sig").replace("\r\n", "\n")
    release = body(source, f"ULONG STDMETHODCALLTYPE {owner}::Release()")
    old_release = body(baseline, f"ULONG STDMETHODCALLTYPE {owner}::Release()")
    prefix = release[1:release.index("LOG_TRACE(")]
    assert prefix.index("InterlockedDecrement") < prefix.index("ModuleLifetime::CleanupAllowed()")
    assert release[release.index("LOG_TRACE("):] == old_release[old_release.index("LOG_TRACE("):]
    assert "#include <runtime/ModuleLifetime.h>" in source
    assert all(token not in prefix for token in (
        "State::", "_real->", "delete ", "_nativeParentRoute", "GuardExecution", "LOG_", "_finalReleaseInProgress"))
    prefixes.append(prefix)
assert prefixes[0] == prefixes[1]
print("PASS both guards precede logs/owners; normal Release tails byte-for-text match baseline", flush=True)

# Execute the actual extracted entry in a CPU fixture. The helper is the real
# ModuleLifetime header; Windows/loader probes alone are controlled substitutes.
header = (root / "OptiScaler/runtime/ModuleLifetime.h").as_posix()
context_header = (root / "OptiScaler/runtime/LoaderStartupContext.h").as_posix()
fixture = r'''
#include <Windows.h>
#include <iostream>
#include "CONTEXT_HEADER_PATH"
static bool moduleKnown = true, probeKnown = true, inLoader = false;
static int loaderProbes = 0;
static int moduleResolutions = 0, procedureResolutions = 0;
static BOOLEAN NTAPI FakeProbe() { ++loaderProbes; return inLoader ? TRUE : FALSE; }
static HMODULE WINAPI FakeModule(LPCWSTR) {
    ++moduleResolutions;
    return moduleKnown ? reinterpret_cast<HMODULE>(1) : nullptr;
}
static FARPROC WINAPI FakeProc(HMODULE, LPCSTR) {
    ++procedureResolutions;
    return probeKnown ? reinterpret_cast<FARPROC>(FakeProbe) : nullptr;
}
namespace LoaderStartupContext {
    inline Status ControlledProbe(HMODULE module, ProbeFunction probe) {
        if (!module || !probe) return Status::ProbeUnavailable;
        return probe() ? Status::LoaderCallout : Status::Ready;
    }
}
#define GetModuleHandleW FakeModule
#define GetProcAddress FakeProc
#define Probe ControlledProbe
#include "HEADER_PATH"
#undef Probe
#undef GetModuleHandleW
#undef GetProcAddress
struct Owner {
    LONG _refcount;
    unsigned logs = 0, sdk = 0, destructors = 0;
    ULONG Release() {
RELEASE_PREFIX
        ++logs;
        if (ret == 0) { ++sdk; ++destructors; }
        return ret;
    }
};
int main() {
    for (int context = 0; context < 5; ++context) {
        ModuleLifetime::processExitRequested.store(context == 0);
        ModuleLifetime::detaching.store(context == 1);
        inLoader = context == 2;
        moduleKnown = context != 3;
        probeKnown = context != 4;
        ModuleLifetime::InitializeContextProbe();
        const auto resolutions = moduleResolutions + procedureResolutions;
        for (LONG refs = 1; refs < 6; ++refs) {
            Owner owner {refs};
            const auto result = owner.Release();
            if (result != static_cast<ULONG>(refs == 1 ? 1 : refs-1) ||
                owner._refcount != static_cast<LONG>(result) ||
                owner.logs || owner.sdk || owner.destructors ||
                resolutions != moduleResolutions + procedureResolutions) return 1;
        }
    }
    ModuleLifetime::processExitRequested.store(false);
    ModuleLifetime::detaching.store(false);
    inLoader = false; moduleKnown = true; probeKnown = true;
    ModuleLifetime::InitializeContextProbe();
    Owner finalOwner {1}, liveOwner {3};
    if (finalOwner.Release() != 0 || finalOwner.logs != 1 || finalOwner.sdk != 1 || finalOwner.destructors != 1) return 2;
    if (liveOwner.Release() != 2 || liveOwner.logs != 1 || liveOwner.sdk || liveOwner.destructors) return 3;
    const auto beforeExit = loaderProbes;
    ModuleLifetime::RequestProcessExit();
    Owner exitingOwner {1};
    if (exitingOwner.Release() != 1 || loaderProbes != beforeExit || exitingOwner.logs) return 4;
    std::cout << "PASS 28 dynamic Release guard cases with production CleanupAllowed; no SDK/destructor executed\n";
}
'''.replace("CONTEXT_HEADER_PATH", context_header).replace("HEADER_PATH", header).replace("RELEASE_PREFIX", prefixes[0])

with tempfile.TemporaryDirectory(prefix="f6-release-guard-") as temporary:
    output = Path(temporary)
    cpp, exe, obj = output / "ReleaseTests.cpp", output / "ReleaseTests.exe", output / "ReleaseTests.obj"
    cpp.write_text(fixture, encoding="utf-8")
    batch = output / "compile.cmd"
    batch.write_text(f'@echo off\ncall "{args.devcmd}" -no_logo -arch=x64 -host_arch=x64\n'
                     'if errorlevel 1 exit /b %errorlevel%\n'
                     f'cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /Od '
                     f'/Fe:"{exe}" /Fo:"{obj}" "{cpp}"\nexit /b %errorlevel%\n',
                     encoding="utf-8", newline="\r\n")
    subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=directory, check=True)
    subprocess.run([str(exe)], cwd=directory, check=True)
