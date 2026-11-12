"""Extract real FG Hook/Set/Create/Retire adapters; CPU compilation is Lead-only.

python -B OptiScaler/hooks/tests/test_fg_hook_install.py
python -B OptiScaler/hooks/tests/test_fg_hook_install.py --cpu
python -B OptiScaler/hooks/tests/test_fg_hook_install.py --cpu --negative-ignore-install

The negative control must fail. No product build, SDK, GPU or game is executed.
All generated files stay in a temporary fg_hook_install directory under tests.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cpu", action="store_true")
parser.add_argument("--negative-ignore-install", action="store_true")
parser.add_argument("--negative-ignore-pin-refusal", action="store_true")
parser.add_argument("--negative-keep-retirement-lease", action="store_true")
parser.add_argument("--devcmd", type=Path,
                    default=Path("D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat"))
args = parser.parse_args()
source = (ROOT / "OptiScaler/hooks/FG_Hooks.cpp").read_text(encoding="utf-8-sig")
header = (ROOT / "OptiScaler/hooks/FG_Hooks.h").read_text(encoding="utf-8-sig")


def check(condition, message):
    if not condition:
        raise SystemExit("FAIL: " + message)


def block(start):
    """These adapter bodies have balanced braces (including their comments)."""
    begin = source.index(start)
    opened = source.index("{", begin)
    depth = 1
    end = opened + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end]


adapters = {
    "OWNER": block("struct FGSwapchainOwner") + ";",
    "CALLBACK": block("struct ScopedFGCallback") + ";",
    "QUERY": block("HRESULT FGHooks::SwapchainHookStatus()"),
    "HOOK": block("HRESULT FGHooks::HookFGSwapchain("),
    "SET": block("HRESULT FGHooks::SetFGSwapchain("),
    "CREATE": block("HRESULT FGHooks::CreateSwapChain("),
    "CREATE_HWND": block("HRESULT FGHooks::CreateSwapChainForHwnd("),
    "RETIRE": block("template<class Release> HRESULT RetireFGForCreate("),
    "PIN_RESULT": block("struct FGPinReleaseResult") + ";",
    "RELEASE": block("template<class Release> FGPinReleaseResult ReleaseFGPin("),
    "CAPTURE": block("bool CaptureStarRailSourceFrameLatencyOwner("),
}
globals_begin = source.index("FGSwapchainOwner* fgOwner = nullptr;")
globals_end = source.index("\n", source.index("std::atomic<HRESULT> fgHookStatus", globals_begin))
adapters["GLOBALS"] = source[globals_begin:globals_end]
slots_begin = header.index("    inline static PFN_ResizeBuffers o_FGSCResizeBuffers")
slots_end = header.index("    inline static HWND _hwnd", slots_begin)
adapters["SLOTS"] = header[slots_begin:slots_end]

check("static HRESULT SetFGSwapchain(" in header, "Set exposes HRESULT")
check("static HRESULT HookFGSwapchain(" in header, "Hook exposes HRESULT")
check("static HRESULT SwapchainHookStatus() noexcept;" in header, "readonly readiness query declared")
check("memory_order_acquire" in adapters["QUERY"], "query observes release publication")
check(adapters["SET"].index("HookFGSwapchain(") < adapters["SET"].index("currentFGSwapchain ="),
      "hook transaction precedes state publication")
check(adapters["SET"].index("currentFGSwapchain =") < adapters["SET"].index("fgHookStatus.store(S_OK"),
      "Ready follows source publication")
for key in ("CREATE", "CREATE_HWND"):
    body = adapters[key]
    check(body.index("hookResult != S_OK") < body.rindex("*callerDesc = *pDesc;"),
          key + " leaves descriptor unchanged on hook failure")
    check("unpublishedCreateReference = std::exchange(*ppSwapChain, nullptr)" in body,
          key + " retains unreturned SDK reference")
    check("fgOwner->quarantined || !_fgHooksInstalled || !o_FGRelease" in body,
          key + " rejects missing release before preserve/retire")
    failure = body[body.index("if (hookResult != S_OK)"):body.rindex("*callerDesc = *pDesc;")]
    check("Release(" not in failure and "Deactivate(" not in failure,
          key + " failure neither releases nor deactivates")
check(adapters["RETIRE"].index("owner->quarantined || !release") <
      adapters["RETIRE"].index("CompleteExistingFGTeardown("), "Retire refuses before touching provider")
check("DetourTransactionAbort()" in adapters["HOOK"] and "abortResult != NO_ERROR" in adapters["HOOK"],
      "rollback result checked")
check("_fgHookInstallFailure != S_OK" in adapters["HOOK"], "install error is terminal, no retry")
if args.negative_ignore_install:
    check(args.cpu, "negative control requires --cpu")
    for key in ("CREATE", "CREATE_HWND"):
        adapters[key] = adapters[key].replace(
            "if (hookResult != S_OK)", "if (SUCCEEDED(hookResult) && hookResult != S_OK)")
if args.negative_ignore_pin_refusal:
    check(args.cpu, "negative control requires --cpu")
    adapters["RETIRE"] = adapters["RETIRE"].replace(
        "return ReleaseFGPin(owner, release).released ? S_OK : DXGI_ERROR_WAS_STILL_DRAWING;",
        "ReleaseFGPin(owner, release); return S_OK;")
if args.negative_keep_retirement_lease:
    check(args.cpu, "negative control requires --cpu")
    adapters["RELEASE"] = adapters["RELEASE"].replace("owner->retirement = {};", "")

fixture = (HERE / "fg_hook_install.cpp").read_text(encoding="utf-8-sig")
for key, adapter in adapters.items():
    marker = "// INSERT_PRODUCT_" + key + "\n"
    check(fixture.count(marker) == 1, "single insertion point: " + key)
    fixture = fixture.replace(marker, adapter + "\n")
check("// INSERT_PRODUCT_" not in fixture, "all production insertion points resolved")
check((HERE / "fg_hook_install.cpp").read_bytes().isascii(), "fixture is ACP-safe ASCII")
print("PASS real FG hook installation adapter extraction, fixture assembly and wiring checks", flush=True)
if not args.cpu:
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix=".fg_hook_install-", dir=HERE) as temporary:
    directory = Path(temporary)
    cpp = directory / "fg_hook_install.cpp"
    cpp.write_text(fixture, encoding="utf-8")
    exe = directory / "fg_hook_install.exe"
    batch = directory / "fg_hook_install_compile.cmd"
    batch.write_text(
        f'@echo off\ncall "{args.devcmd}" -no_logo -arch=x64 -host_arch=x64\n'
        'if errorlevel 1 exit /b %errorlevel%\n'
        f'cl /nologo /std:c++20 /EHsc /W4 /WX /Od /Fe:"{exe}" /Fo:"{directory}/" "{cpp}"\n'
        'exit /b %errorlevel%\n', encoding="ascii", newline="\r\n")
    subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=directory, check=True)
    subprocess.run([str(exe)], cwd=directory, check=True)
