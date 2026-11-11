"""Check private-copy wait wiring; --cpu compiles the exact production receipt.

The CPU fixture uses observable stand-in queue/fence owners. It does not run a
graphics queue, execute the vendor SDK, prove GPU retirement, or measure FPS.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cpu", action="store_true")
parser.add_argument("--devcmd", type=Path, default=Path("D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat"))
args = parser.parse_args()
directory = Path(__file__).resolve().parent
source = (directory.parent / "dx11_with_dx12_sc.cpp").read_text(encoding="utf-8-sig")
checks = 0


def check(condition, message):
    global checks
    if not condition:
        raise AssertionError(message)
    checks += 1


def body(signature):
    start = source.index("{", source.index(signature))
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


sync = body("bool Dx11wDx12SC::_WaitDx11ThenDx12()")
copy = body("bool Dx11wDx12SC::_CopyDx11SharedToDx12FGBackBuffer(")
idle = body("bool Dx11wDx12SC::_WaitForCopyQueueIdle()")
adapter = body("HRESULT OrderPrivateCopyWait(")
visible = body("HRESULT Dx11wDx12SC::_PresentNativeVisible(")
check("OrderPrivateCopyWait(this, _dx12CommandQueue, _dx12SharedFence, waitValue)" in sync,
      "producer handoff uses the shared submission path")
check("OrderPrivateCopyWait(this, colorQueue, _dx12SharedFence, _sharedFenceValue - 1)" in copy,
      "actual color writer uses the exact same submission path")
check("->Wait(_dx12SharedFence" not in sync + copy,
      "the private copy cannot bypass the common Wait submission path")
check(sync.index("_dx11Context4->Signal(_dx11Fence, waitValue)") <
      sync.index("_dx11Context4->Flush()") < sync.index("OrderPrivateCopyWait("),
      "actual DX11 Signal and Flush still precede the queue handoff")
check(copy.index("PrepareGenshinColorWrite(") < copy.index("OrderPrivateCopyWait(") <
      copy.index("colorQueue->ExecuteCommandLists(") < copy.index("colorQueue->Signal(_copyFence, signalValue)"),
      "actual present-queue authentication and copy retirement are preserved")
check(copy.index("_WaitForCopyAllocator(") < copy.index("allocator->Reset()"),
      "submission reuse does not replace allocator completion")
check("scope->owner != owner" in adapter and "return submit()" in adapter and
      "scope->wait.Order(queue, fence, value," in adapter,
      "other owners and unscoped interop retain real submissions")
check("submittedQueue = queue" in adapter and "submittedFence = fence" in adapter and
      adapter.index("scope->wait.Order(") < adapter.index("submittedQueue = queue") <
      adapter.index("submittedQueue->Wait(") < adapter.index("scope->waitQueue = std::move(submittedQueue)"),
      "accepted calls pin owners locally before Wait and publish owners only after it returns")
check(visible.index("ScopedPrivateCopySlot privateSlot(") < visible.index("copied=_WaitDx11ThenDx12()") <
      visible.index("copied=_CopyDx11SharedToDx12FGBackBuffer("),
      "both native handoffs share one private-copy transaction")
check("_WaitDx11ThenDx12()" in idle and "_dx12CommandQueue->Wait(_copyFence, _lastInteropCopyFenceValue)" in idle and
      "_dx12CommandQueue->Signal(_copyFence,waitValue)" in idle and "WaitForQueueIdle(_interopPresentQueue.Get()" in idle,
      "lifecycle still drains both actual queue owners before retiring allocations")
check("SubmittedWait" not in visible and visible.count("_fgSwapChain->Present(0, 0)") == 1,
      "Wait receipt cannot authorize SDK Present or multiply source submissions")

# Compile the actual production policy, scope and adapter. No shadow copy of
# their behavior is maintained in the test fixture.
production = source[source.index("// Private copy wait receipt begin"):
                    source.index("// Armed only around this authenticated Genshin private-copy transaction.")]
check("class SubmittedWait final" in production and "HRESULT OrderPrivateCopyWait(" in production,
      "fixture extracts the production policy and its owner-scoped adapter")
print(f"PASS {checks} private-copy source wiring checks; D3D/SDK/GPU/FPS untested", flush=True)
if args.cpu:
    with tempfile.TemporaryDirectory(prefix="f6-private-copy-wait-") as temporary:
        output = Path(temporary)
        (output / "PrivateCopyWaitProduction.h").write_text(production, encoding="utf-8")
        fixture = directory / "PrivateCopyWaitTests.cpp"
        exe = output / "PrivateCopyWaitTests.exe"
        obj = output / "PrivateCopyWaitTests.obj"
        command = (f'cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /Od /I"{output}" '
                   f'/Fe:"{exe}" /Fo:"{obj}" "{fixture}"')
        batch = output / "compile.cmd"
        batch.write_text(
            f'@echo off\ncall "{args.devcmd}" -no_logo -arch=x64 -host_arch=x64\n'
            f'if errorlevel 1 exit /b %errorlevel%\n{command}\nexit /b %errorlevel%\n',
            encoding="utf-8", newline="\r\n",
        )
        subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=output, check=True)
        subprocess.run([str(exe)], cwd=output, check=True)
