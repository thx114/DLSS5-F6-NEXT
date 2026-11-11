"""Check FG Present observation wiring and optionally run the actual CPU adapter.

python -B OptiScaler/hooks/tests/test_fg_sdk_present_timing.py --cpu

The CPU fixture uses the exact adapter extracted from FG_Hooks.cpp and the real
GenshinHookCpuTiming implementation. Only temporary test files are generated;
no project, common runner, DLL, graphics API, configuration, or game is used.
"""
import argparse
from collections import Counter
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
PRODUCT = "OptiScaler/hooks/FG_Hooks.cpp"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cpu", action="store_true")
parser.add_argument("--revision", help="negative control: inspect the selected Git version")
parser.add_argument("--devcmd", type=Path, default=Path("D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat"))
args = parser.parse_args()
checks = 0


def check(ok, message):
    global checks
    checks += 1
    if not ok:
        raise SystemExit(f"FAIL: {message}")


def git_source(revision):
    return subprocess.check_output(["git", "show", f"{revision}:{PRODUCT}"], cwd=ROOT).decode("utf-8-sig")


def section(source, start, end):
    check(start in source and end in source, f"production boundary exists: {start}")
    return source[source.index(start):source.index(end, source.index(start))]


source = git_source(args.revision) if args.revision else (ROOT / PRODUCT).read_text(encoding="utf-8-sig")
baseline = git_source("HEAD")
adapter = section(source, "// FG SDK timing adapter begin", "// FG SDK timing adapter end")
check(adapter.count("std::forward<Present>(present)()") == 1, "observation invokes exactly one original callable")
check("collect && !fgSdkPresentTimingActive" in adapter and "thread_local bool fgSdkPresentTimingActive=false" in adapter,
      "same-thread synchronous reentry is suppressed independently of SDK workers")
check(adapter.count("Stage::FgSdkPresent") == 1, "one sampler owns the native interval")
check("timing.Returned(static_cast<uint32_t>(result),GenshinHookCpuTiming::ResultKind::Hresult)" in adapter,
      "original HRESULT is observed without changing its bits")
check(adapter.index("timing.Finish()") < adapter.index("if(observed)"), "published endpoints stop before caller postwork")
check("~Restore(){if(owns)fgSdkPresentTimingActive=false;}" in adapter, "unwind restores thread-local observation ownership")
check("return result;" in adapter, "original status passes through")
check(adapter.index("timing.Finish()") < adapter.index("GuideDiagnostics::PresentReturned("),
      "passive output event is outside the timed SDK interval")
check("const auto apiLastError=GetLastError()" in adapter and "SetLastError(apiLastError)" in adapter,
      "passive output event preserves the original API last-error state")
for forbidden in ("GetState(", "LOG_", "Sleep(", "Wait(", "new ", "mutex", "PresentPermit", "BeforePresent", "AfterPresent", "FrameCount("):
    check(forbidden not in adapter, f"native timing adapter has no functional/metadata side effect: {forbidden}")

route = section(source, "bool GenshinFgTimingRequested(", "// Only these hook lifecycle")
check(route.index("DXGI_PRESENT_TEST") < route.index("Util::ExePath()"), "TEST and disabled sampling short-circuit before executable lookup")
check("!GenshinHookCpuTiming::Enabled()" in route, "existing default-off control remains required")
check("static const bool genshin=" in route and "GetLastError()" in route and "SetLastError(error)" in route,
      "executable detection is cached and preserves LastError")
for forbidden in ("PresentScope::Current", "currentFG", "IsActive(", "IsOwnedChild(", "IsPhysicalOutput(", "GetDesc(", "GetDevice("):
    check(forbidden not in route, f"worker observation requires no mutable source/provider/resource metadata: {forbidden}")

present = section(source, "HRESULT FGHooks::hkFGPresent(", "HRESULT FGHooks::hkFGPresent1(")
present1 = section(source, "HRESULT FGHooks::hkFGPresent1(", "HRESULT FGHooks::FGPresent(")
for name, code in (("Present", present), ("Present1", present1)):
    check(code.count("ObserveFgSdkPresent(GenshinFgTimingRequested(Flags)") == 2,
          f"both existing {name} bypass forwards use the same native timing adapter")
    check("pPresentParameters" not in code if name == "Present" else "Flags, pPresentParameters)" in code,
          f"{name} preserves its existing parameter shape")

main = source[source.index("HRESULT FGHooks::FGPresent("):]
shutdown = section(main, "if (state.isShuttingDown)", "auto willPresent")
check(shutdown.count("ObserveFgSdkPresent(GenshinFgTimingRequested(Flags)") == 2,
      "existing shutdown forwards retain exact once-only native observation")
check("collectSdkPresent=collectStarRailPacing || GenshinFgTimingRequested(Flags)" in main,
      "Genshin native observation is independent of StarRail pacing and source TLS")
sdk = section(main, "GenshinHookCpuTiming::WallSample starRailSdkWall;", "const auto starRailSdkResult=")
check(sdk.index("if (starRailSourceFailed)") < sdk.index("dlssg::zzz::RunPresent(") < sdk.index("ObserveFgSdkPresent(collectSdkPresent"),
      "SDK scope exists only inside the real admitted RunPresent callback")
check(sdk.count("o_FGSCPresent(") == 1 and sdk.count("o_FGSCPresent1(") == 1,
      "Present/Present1 remain alternative original calls inside a single observation")
check("effectivePresentParameters" in sdk and "},&starRailSdkWall)" in sdk,
      "existing effective parameters and exact StarRail attribution sample are preserved")
check("sdkPresentTiming" not in source, "the old enclosing scope cannot double-count native calls or include result logging")
check(main.index("PresentPermit::BeforePresent(") < main.index("GenshinHookCpuTiming::WallSample starRailSdkWall;") <
      main.index("PresentPermit::AfterPresent("), "input authorization/SDK return brackets stay in their original order")

# Verify that wrapping did not add calls to the vendor, pacing or authority APIs.
patterns = (
    r"\b(?:o_FGSCPresent1?|sc->Present1?)\s*\([^;\n]*?\)",
    r"\bGenshinVisibleChild::PresentPermit::[A-Za-z_][A-Za-z_0-9]*\(",
    r"\bGenshinBridgeFg::[A-Za-z_:]+\(",
)
for pattern in patterns:
    check(Counter(re.findall(pattern, source)) == Counter(re.findall(pattern, baseline)),
          f"original API/authority/pacing call inventory unchanged: {pattern}")
# Source pacing intentionally moved to the retained-token decision. Its
# concurrency suite verifies ownership and unlock order; here retain the
# invariant that the timing observer adds no limiter or SDK invocation.
check(source.count("FrameLimit::sleep(") == baseline.count("FrameLimit::sleep("),
      "moving immutable limiter arguments does not add another limiter")
check(source.count("StreamlineProxy::ReflexSleep()(*localToken)") == 1 and
      source.count("pacingLease->Sleep()") == 1,
      "legacy and retained direct-source waits are distinct once-only branches")
check(source.count("Stage::FgSdkPresent") == 1, "native observation is sampled in exactly one shared adapter")
print(f"PASS {checks} FG SDK source-wiring checks", flush=True)

if args.cpu:
    check(not args.revision, "CPU fixture runs the working product adapter")
    fixture = (HERE / "fg_sdk_present_timing.cpp").read_text(encoding="utf-8-sig")
    check(fixture.count("// INSERT_PRODUCT_FG_SDK_TIMING") == 1, "CPU fixture has one exact product insertion point")
    with tempfile.TemporaryDirectory(prefix=".fg-sdk-cpu-", dir=HERE) as temporary:
        output = Path(temporary)
        cpp = output / "fixture.cpp"
        cpp.write_text(fixture.replace("// INSERT_PRODUCT_FG_SDK_TIMING", adapter), encoding="utf-8")
        exe = output / "fixture.exe"
        # Fixed local paths, all quoted; no interpolation of task bodies, tokens,
        # environment dumps, global compiler settings, or project mutations.
        command = (
            f'cl /nologo /std:c++20 /EHsc /W4 /WX /Od '
            f'/I"{ROOT / "OptiScaler"}" '
            f'/Fe:"{exe}" /Fo:"{output}/" "{cpp}" '
            f'"{ROOT / "OptiScaler/genshin/GenshinHookCpuTiming.cpp"}"'
        )
        batch = output / "compile.cmd"
        batch.write_text(
            f'@echo off\ncall "{args.devcmd}" -no_logo -arch=x64 -host_arch=x64\n'
            f'if errorlevel 1 exit /b %errorlevel%\n{command}\nexit /b %errorlevel%\n',
            encoding="utf-8", newline="\r\n",
        )
        subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=output, check=True)
        subprocess.run([str(exe)], cwd=output, check=True)
