"""Compile the actual source/physical-output cap blocks with a simulated SDK worker.

--legacy-worker substitutes the pre-fix HEAD worker block as a negative control.
This proves CPU lock ownership only, not SDK GPU completion or game rendering.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--legacy-worker", action="store_true")
args = parser.parse_args()
wrapper = (ROOT / "OptiScaler/wrapped/wrapped_swapchain.cpp").read_text(encoding="utf-8-sig")
source = (ROOT / "OptiScaler/hooks/FG_Hooks.cpp").read_text(encoding="utf-8-sig")


def block(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]


output = block(wrapper, "// FG output Reflex ownership begin", "// FG output Reflex ownership end")
cap = block(source, "// FG source Reflex ownership begin", "// FG source Reflex ownership end")
assert "if (!providerOutput)" in output
assert "StarRail" not in output and "State::Instance()" not in output
assert "_providerOutput = framegen::final_output::CaptureProviderOutputBinding(_real, _device);" in wrapper
assert "_starRailProviderOutput = IsStarRailProcess() && _providerOutput;" in wrapper
assert wrapper.count("_finalOutput.get(), _providerOutput, _starRailProviderOutput,") == 2
assert "const bool sourcePresent = willPresent && !providerOutput;" in wrapper
assert "const bool deferNativeEpoch = providerOutput ||" in wrapper
assert "if (!providerOutput && State::Instance().currentCommandQueue == nullptr" in wrapper
assert "if (!providerOutput && State::Instance().swapchainInteropApi != SwapchainInteropApi::VkwDx12)" in wrapper
assert "SourcePresent::Scope sourceOwner(sourcePresent);" in wrapper
assert "SourcePresent::TickFeature();" in wrapper
assert "if (sourcePresent && (fg == nullptr || !fg->IsActive() || fg->IsPaused()))" in wrapper
assert "if (sourcePresent)\n        SourcePresent::ObserveState(" in wrapper
assert "SourcePresent::Scope proxySourceOwner(willPresent && !state.externalFrameGeneration" in source
assert "if (proxySourceOwner.Owns()) SourcePresent::ObserveProxySource(This, SyncInterval, Flags);" in source
assert source.index("if (proxySourceOwner.Owns())") < source.index("const auto pacingPresentFrame =")
assert "if (!providerOutput) XellHooks::update();" in wrapper
assert wrapper.count("if (!_providerOutput && !State::Instance().reflexLimitsFps") == 2
assert source.index("auto presentExecution =") < source.index("const bool dispatched=fg->Present();") < source.index(cap)
assert source.index(cap) < source.index("GenshinHookCpuTiming::WallSample starRailSdkWall;")
assert "auto pacingExecution = fg->GuardExecution(FGExecutionScope::Lifecycle);" in (
    ROOT / "OptiScaler/hooks/Reflex_Hooks.cpp").read_text(encoding="utf-8-sig")
print("PASS FG Reflex creation binding and source/SDK ordering checks", flush=True)
if args.legacy_worker:
    old = subprocess.check_output(["git", "show", "HEAD:OptiScaler/wrapped/wrapped_swapchain.cpp"], cwd=ROOT).decode("utf-8-sig")
    output = block(old, "static const bool starRailExecutable =", "XellHooks::update();")
fixture = (HERE / "fg_reflex_ownership.cpp").read_text(encoding="utf-8-sig")
with tempfile.TemporaryDirectory(prefix=".fg-reflex-cpu-", dir=HERE) as temporary:
    directory = Path(temporary)
    cpp = directory / "fixture.cpp"
    cpp.write_text(fixture.replace("// INSERT_OUTPUT_REFLEX", output).replace("// INSERT_SOURCE_REFLEX", cap), encoding="utf-8")
    exe = directory / "fixture.exe"
    batch = directory / "compile.cmd"
    batch.write_text(
        '@echo off\ncall "%VSDEVCMD%" -no_logo -arch=x64 -host_arch=x64\n'
        'if errorlevel 1 exit /b %errorlevel%\n'
        f'cl /nologo /std:c++20 /EHsc /W4 /WX /Od /Fe:"{exe}" /Fo:"{directory}/" "{cpp}"\n'
        'exit /b %errorlevel%\n', encoding="utf-8", newline="\r\n")
    subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=directory, check=True)
    subprocess.run([str(exe)], cwd=directory, check=True)
