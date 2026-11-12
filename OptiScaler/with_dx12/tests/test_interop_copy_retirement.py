"""Check common interop retirement; --cpu compiles extracted production blocks.

The fixture executes the real admission, wait, Reset and Execute/Signal blocks
with observable CPU stand-ins. It does not run D3D, the SDK, or a game.
Default mode never invokes a compiler. Lead owns all --cpu/build execution.
"""
import argparse
import re
from pathlib import Path
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cpu", action="store_true")
parser.add_argument("--devcmd", type=Path,
                    default=Path("D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat"))
args = parser.parse_args()
source = (HERE.parent / "dx11_with_dx12_sc.cpp").read_text(encoding="utf-8-sig")
header = (HERE.parent / "dx11_with_dx12_sc.h").read_text(encoding="utf-8-sig")
checks = 0


def check(ok, message):
    global checks
    checks += 1
    if not ok:
        raise AssertionError(message)


def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def block(text, first, last):
    start = text.index(first)
    return text[start:text.index(last, start)]


writer = function(source, "bool Dx11wDx12SC::_CopyDx11BackBufferToShared(")
copy = function(source, "bool Dx11wDx12SC::_CopyDx11SharedToDx12FGBackBuffer(")
wait_signature = "bool Dx11wDx12SC::_WaitForCopyAllocator(UINT slot, std::vector<UINT64>& fenceValues)"
consume_signature = wait_signature[:-1] + ", CopySlotAdmission& admission)"
wait = function(source, wait_signature)
consume = function(source, consume_signature)
queue_drain = function(source, "bool WaitForQueueIdle(")
copy_drain = function(source, "bool Dx11wDx12SC::_WaitForCopyQueueIdle()")
recover = function(source, "bool Dx11wDx12SC::_TryRecoverNativeChild()")
native_drain = function(source, "bool Dx11wDx12SC::_TryDrainNativeChild()")
revoke = function(source, "void Dx11wDx12SC::_RevokeNativeChild(")
check(recover.index("_childRecoveryEntered = true") < recover.index("_TryDrainNativeChild()") <
      recover.index("_childRevoked = false"), "only the revoked child polls before re-opening admission")
check(native_drain.index("PrepareForSwapchainChange()") < native_drain.index("_WaitForCopyQueueIdle()") <
      native_drain.index("_ReleaseInteropBackBuffers()"), "provider and both real queues gate release")
check("_recoveryCopyDrain = {};" in revoke and "_recoveryPresentDrain = {};" in revoke,
      "a new revocation invalidates the entire drain")
admission = block(header, "    class CopySlotAdmission final", "    // Interop slot admission end")
writer_admission = block(writer, "    // Interop shared writer admission begin",
                         "    // Interop shared writer admission end")
reset = block(copy, "    if (!_WaitForCopyAllocator(copySlot, _copyAllocatorFenceValues, admission))",
              "    CallerCpuTrace::Scope listResetCpu")
submission = block(copy, "    colorQueue->ExecuteCommandLists(1, lists);",
                   "    if (auto* scope = GenshinBridgeFg::PresentScope::Current(); scope && copySlot < _genshinFgImports.size())")
uncertain = block(submission, "        // Interop uncertain submission begin",
                  "        // Interop uncertain submission end")

check(writer.index("admission.Clear();") < writer.index(writer_admission),
      "a new writer cannot borrow a previous transaction's proof")
check(writer.index(writer_admission) < writer.index("_RequestSharedBackBuffer(index, false)") <
      writer.index("_msaaResolve.ResolveIn(") < writer.index("_dx11Context->CopyResource("),
      "exact reader retirement precedes replacement, Resolve and CopyResource")
check("_WaitForCopyAllocator(copySlot, _copyAllocatorFenceValues)" in writer_admission and
      "admission.Record(this, &_copyAllocatorFenceValues, _copyFence, _copyAllocators[copySlot]," in writer_admission,
      "only the actual slot reader wait can publish admission")
check("_sourceFrameLatencyOwner" not in writer_admission and "_nativeParentRoute" not in writer_admission,
      "common lifetime admission is independent of special source ownership")
check(copy.index(reset) < copy.index("_copyCommandLists[copySlot]->Reset(allocator, nullptr)"),
      "allocator Reset consumes the same call-local pre-write admission")
check("WaitForSingleObject" not in consume and "GetCompletedValue" not in consume and
      "admission.Consume(this, &fenceValues, _copyFence, _copyAllocators[slot], slot, fenceValues[slot])" in consume,
      "changed admission fails without polling or a second/late completion wait")
check(wait.count("WaitForSingleObject(") == 1 and "WaitForSingleObject(_copyFenceEvent, 5000)" in wait,
      "the existing bounded wait is moved, not multiplied or lengthened")
check(wait.index("_nativeParentRoute && GenshinBridgeFg::PresentScope::Current()") <
      wait.index("WaitForSingleObject("), "native optional Busy remains nonblocking")
check('_visibleReceipt.reason = "CopySlotBusy"' in wait,
      "early native Busy retains the caller's nonterminal classification")
check("if (completedValue == UINT64_MAX)" in wait and "actual != UINT64_MAX && actual >= fenceValue" in wait,
      "device removal or a spurious event cannot admit common resource reuse")
check("CopySlotAdmission(const CopySlotAdmission&) = delete" in admission and
      "Clear(); // A mismatch" in admission,
      "the proof is noncopyable and consumed on success or mismatch")
for entry in (
    "HRESULT STDMETHODCALLTYPE Dx11wDx12SC::Present(UINT",
    "HRESULT Dx11wDx12SC::_PresentNativeVisible(",
):
    present = function(source, entry)
    check("CopySlotAdmission copyAdmission;" in present and
          "_CopyDx11BackBufferToShared(" in present and
          "_CopyDx11SharedToDx12FGBackBuffer(" in present and
          len(re.findall(r"_CopyDx11(?:BackBufferToShared|SharedToDx12FGBackBuffer)\(\s*\w+\s*,\s*copyAdmission(?:\s*,\s*copiedColour\.GetAddressOf\(\))?\s*\)", present)) == 2,
          f"both copies share a fresh local proof in {entry}")
check(submission.index("ExecuteCommandLists(") < submission.index("colorQueue->Signal(") <
      submission.index("if (FAILED(result))") < submission.index(uncertain) <
      submission.index("_lastInteropCopyFenceValue = signalValue;"),
      "uncertainty applies only after actual Execute and failed Signal")
check("_sourceFrameLatencyOwner" not in uncertain and "_nativeParentRoute" not in uncertain,
      "failed submissions quarantine all routes without granting special rights")
for witness in (
    "_sourceCopyUncertain.store(true, std::memory_order_release)",
    "_copyAllocatorFenceValues[copySlot] = UINT64_MAX", "_lastInteropCopyFenceValue = 0",
    "AddRef();", "colorQueue->AddRef();", "_dx12Device->AddRef();",
    "fgBackBuffer.Detach()", "HoYoShade::QuarantineColor(_fgSwapChain, colorQueue, _dx12Device,",
):
    check(witness in uncertain, f"uncertain work retains/denies the real operand: {witness}")
for entry in (
    "ULONG STDMETHODCALLTYPE Dx11wDx12SC::Release()",
    "HRESULT STDMETHODCALLTYPE Dx11wDx12SC::HookResizeBuffers(",
    "HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeBuffers1(",
    "bool Dx11wDx12SC::_WaitForCopyQueueIdle()",
    "void Dx11wDx12SC::_ReleaseInteropBackBuffers()",
    "bool Dx11wDx12SC::_ReleaseInteropObjects()",
):
    check("_sourceCopyUncertain.load(std::memory_order_acquire)" in function(source, entry),
          f"uncertainty cannot be cleared by normal retirement: {entry}")
check("_copyAllocators.assign(copyAllocatorCount, nullptr)" in source and
      "std::max<UINT>(_bufferCount != 0 ? _bufferCount : 3, 3)" in source,
      "existing ring capacity is unchanged")
print(f"PASS {checks} common interop retirement wiring checks; CPU/D3D/SDK/game not executed", flush=True)

if args.cpu:
    with tempfile.TemporaryDirectory(prefix="f6-interop-retirement-") as temporary:
        output = Path(temporary)
        for name, content in {
            "InteropSlotAdmissionProduction.h": admission,
            "InteropCopyWaitProduction.inl": wait + "\n" + consume,
            "InteropQueueDrainProduction.inl": queue_drain + "\n" + copy_drain,
            "InteropWriterAdmissionProduction.inl": writer_admission,
            "InteropCopyResetProduction.inl": reset,
            "InteropCopySubmissionProduction.inl": submission,
        }.items():
            (output / name).write_text(content, encoding="utf-8")
        fixture = HERE / "InteropCopyRetirementTests.cpp"
        exe = output / "InteropCopyRetirementTests.exe"
        batch = output / "compile.cmd"
        batch.write_text(
            f'@echo off\ncall "{args.devcmd}" -no_logo -arch=x64 -host_arch=x64\n'
            'if errorlevel 1 exit /b %errorlevel%\n'
            f'cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /Od /I"{output}" '
            f'/Fe:"{exe}" /Fo:"{output}/" "{fixture}"\nexit /b %errorlevel%\n',
            encoding="utf-8", newline="\r\n",
        )
        subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=output, check=True)
        subprocess.run([str(exe)], cwd=output, check=True)
