"""Verify production resize wiring and optionally compile the CPU policy tests.

No graphics device, SDK or game is executed; passing is not display acceptance.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cpu", action="store_true")
parser.add_argument("--devcmd", type=Path, default=Path(os.environ.get("VSDEVCMD", "VsDevCmd.bat")))
args = parser.parse_args()
directory = Path(__file__).resolve().parent
source = (directory.parent / "dx11_with_dx12_sc.cpp").read_text(encoding="utf-8-sig")
checks = 0


def check(condition, message):
    global checks
    if not condition:
        raise AssertionError(message)
    checks += 1


def body(signature, text=source):
    start = text.index("{", text.index(signature))
    depth, end = 1, start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


adapter = body("Dx11InteropSwapchain::ResizeArgs ResizeArgsForFg(")
check(adapter.index("PresenterResizeArgs(") < adapter.index("GameQuirk::XeFGBridgeResizeAsCreated"),
      "format/count normalization applies before any XeFG-only game quirk")
check("args.flags = desc.Flags" in adapter and "activeFgOutput == FGOutput::XeFG" in adapter,
      "XeFG flag preservation remains limited to its existing quirk")

present = body("HRESULT STDMETHODCALLTYPE Dx11wDx12SC::Present(")
failure = present.index("_fgResizeStatus.Failure()")
check(failure < present.index("GenshinBridgeFg::PresentScope") < present.index("_InitInteropObjects()"),
      "failed resize blocks producer scope and all interop allocations/copies")
check("FAILED(failure))\n        return failure;" in present,
      "Present preserves the actual provider failure rather than returning hidden-chain success")
check(present.index("DXGI_PRESENT_TEST") < failure,
      "native TEST handling remains ahead of the presenter failure gate")

for method, native_call in (
    ("ResizeBuffers", "_real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags)"),
    ("ResizeBuffers1", "_real3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask, ppPresentQueue)"),
):
    implementation = "HookResizeBuffers" if method == "ResizeBuffers" else method
    resize = body(f"HRESULT STDMETHODCALLTYPE Dx11wDx12SC::{implementation}(")
    check(native_call in resize, f"{method}: native arguments retain original sRGB/count/queues")
    check("const bool skipFgResize = _fgResizeStatus.CanSkipResize() &&" in resize,
          f"{method}: equivalent GetDesc cannot suppress actual recovery resize")
    check(resize.index("_fgResizeStatus.RecoveryFailure(Config::Instance()->FGSkipResizeBuffers.value_or_default())") <
          resize.index("PrepareForSwapchainChange()"),
          f"{method}: failed presenter never enters lower-hook optional skip/index-reset calls")
    check("_fgSwapChain->ResizeBuffers(fgArgs.bufferCount, childWidth, childHeight, fgArgs.format, fgArgs.flags)" in resize,
          f"{method}: presenter uses the normalized arguments")
    check(resize.index("PrepareForSwapchainChange()") < resize.index("_WaitForCopyQueueIdle()") <
          resize.index("_ReleaseInteropBackBuffers()") < resize.index(native_call),
          f"{method}: real provider and interop drains still precede resource release/native resize")
    check("_fgResizeStatus.Complete(realResult, fgResult, !skipFgResize && _fgSwapChain != nullptr)" in resize,
          f"{method}: only the actual resize result changes presenter validity")
    check(resize.index("if (SUCCEEDED(realResult))\n") < resize.index("_RefreshCachedSwapchainDesc();", resize.index("LOG_DEBUG(\"Dx11wDx12SC")),
          f"{method}: successful native resize updates its descriptor even after presenter failure")

# Factory is read-only in this task. Check the live creation policy so a later
# change cannot quietly make our resize mapping disagree with creation again.
factory = (directory.parents[1] / "hooks" / "DxgiFactory_Hooks.cpp").read_text(encoding="utf-8-sig")
create_format = body("static bool PrepareDx12FlipFormat(", factory)
for srgb, unorm in (("R8G8B8A8_UNORM_SRGB", "R8G8B8A8_UNORM"), ("B8G8R8A8_UNORM_SRGB", "B8G8R8A8_UNORM")):
    case = create_format.split(f"case DXGI_FORMAT_{srgb}:", 1)[1].split("return true;", 1)[0]
    check(f"format = DXGI_FORMAT_{unorm};" in case, "factory and resize use the same sRGB storage mapping")
for name in ("PrepareDx12InteropDesc", "PrepareDx12InteropDesc1"):
    check("PrepareDx12FlipFormat(" in body(f"static bool {name}(", factory),
          "both creation entry points share the checked format mapping")

print(f"PASS {checks} interop swapchain wiring checks; graphics/SDK/display untested", flush=True)
if args.cpu:
    with tempfile.TemporaryDirectory(prefix="f6-interop-swapchain-") as temporary:
        output = Path(temporary)
        exe = output / "InteropSwapchainPolicyTests.exe"
        obj = output / "InteropSwapchainPolicyTests.obj"
        fixture = directory / "InteropSwapchainPolicyTests.cpp"
        command = (f'cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /Od '
                   f'/Fe:"{exe}" /Fo:"{obj}" "{fixture}"')
        batch = output / "compile.cmd"
        batch.write_text(
            f'@echo off\ncall "{args.devcmd}" -no_logo -arch=x64 -host_arch=x64\n'
            f'if errorlevel 1 exit /b %errorlevel%\n{command}\nexit /b %errorlevel%\n',
            encoding="utf-8", newline="\r\n",
        )
        # DevCmd can start helpers which retain their cwd after cl exits. Keep
        # that cwd outside the temporary output so cleanup can remove it.
        subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=directory, check=True)
        subprocess.run([str(exe)], cwd=directory, check=True)
