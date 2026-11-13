"""Check common interop retirement; --cpu compiles extracted production blocks.

The fixture executes the real admission, wait, Reset, Execute/Signal and present
queue ownership code with observable CPU stand-ins and real WRL ComPtr ownership.
It does not run D3D, the SDK, or a game. Default mode never invokes a compiler.
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
present_wait = function(source, "bool Dx11wDx12SC::_WaitForInteropCopyOnPresentQueue()")
admission = block(header, "    class CopySlotAdmission final", "    // Interop slot admission end")
writer_admission = block(writer, "    // Interop shared writer admission begin",
                         "    // Interop shared writer admission end")
reset = block(copy, "    if (!_WaitForCopyAllocator(copySlot, _copyAllocatorFenceValues, admission))",
              "    CallerCpuTrace::Scope listResetCpu")
submission = block(copy, "    colorQueue->ExecuteCommandLists(1, lists);",
                   "    if (Config::Instance()->DlssNrEnabled.value_or_default()")
uncertain = block(submission, "        // Interop uncertain submission begin",
                  "        // Interop uncertain submission end")

check(writer.index("admission.Clear();") < writer.index(writer_admission),
      "a new writer cannot borrow a previous transaction's proof")
check(writer.index(writer_admission) < writer.index("_real->GetBuffer(index,") <
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
check("if (completedValue == UINT64_MAX)" in wait and "actual != UINT64_MAX && actual >= fenceValue" in wait,
      "device removal or a spurious event cannot admit common resource reuse")
check("CopySlotAdmission(const CopySlotAdmission&) = delete" in admission and
      "Clear(); // A mismatch" in admission,
      "the proof is noncopyable and consumed on success or mismatch")
for entry in (
    "HRESULT STDMETHODCALLTYPE Dx11wDx12SC::Present(UINT",
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
producer = function(source, "bool Dx11wDx12SC::_WaitDx11ThenDx12()")
check(producer.index("_dx11Context4->Signal(") < producer.index("_dx11Context4->Flush()") <
      producer.index("_dx12CommandQueue->Wait("), "actual writer Signal/Flush precedes reader queue Wait")
print(f"PASS {checks} common interop retirement wiring checks; CPU/D3D/SDK/game not executed", flush=True)

if args.cpu:
    # Reintroduce the original bug only in a generated negative control. Both
    # variants execute the full extracted function and actual production drain.
    assignment = "_interopPresentQueue = queue; // arm ownership BEFORE"
    check(present_wait.count(assignment) == 1, "negative control targets one owner adoption")
    legacy_present_wait = present_wait.replace(
        "Dx11wDx12SC::_WaitForInteropCopyOnPresentQueue()",
        "Dx11wDx12SC::_WaitForInteropCopyOnPresentQueueLegacy()", 1,
    ).replace(assignment, "if (queue) _interopPresentQueue = queue; // arm ownership BEFORE", 1)
    producer = producer.replace("Dx11wDx12SC::_WaitDx11ThenDx12()", "ProducerFixture::Handoff()")
    producer_legacy = producer.replace("ProducerFixture::Handoff()", "ProducerFixture::HandoffLegacy()")
    check(producer_legacy.count("result = _dx12CommandQueue->Wait(_dx12SharedFence, waitValue);") == 1,
          "writer-wait negative targets one actual queue wait")
    producer_legacy = producer_legacy.replace("result = _dx12CommandQueue->Wait(_dx12SharedFence, waitValue);", "result = S_OK;")
    check(writer_admission.count("!_WaitForCopyAllocator(copySlot, _copyAllocatorFenceValues)") == 1,
          "reader-retirement negative targets the actual pre-write wait")
    writer_legacy = writer_admission.replace("!_WaitForCopyAllocator(copySlot, _copyAllocatorFenceValues)", "false")
    writer_admission = "if (negativeReaderRetirement) {\n" + writer_legacy + "\n} else {\n" + writer_admission + "\n}"
    with tempfile.TemporaryDirectory(prefix="f6-interop-retirement-") as temporary:
        output = Path(temporary)
        for name, content in {
            "InteropSlotAdmissionProduction.h": admission,
            "InteropProducerHandoffProduction.inl": producer + "\n" + producer_legacy,
            "InteropCopyWaitProduction.inl": wait + "\n" + consume,
            "InteropQueueDrainProduction.inl": queue_drain + "\n" + copy_drain,
            "InteropPresentQueueProduction.inl": present_wait + "\n" + legacy_present_wait,
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
        subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=HERE, check=True)
        subprocess.run([str(exe)], cwd=HERE, check=True)
        negative = subprocess.run([str(exe), "--negative-null-owner"], cwd=HERE,
                                  capture_output=True, text=True)
        check(negative.returncode == 1 and
              "FAIL: successful retirement adopts the exact new owner, including null" in negative.stderr,
              f"legacy conditional adoption must fail the owner transition regression: {negative}")
        print("PASS legacy queue-to-null negative control rejected by executed ownership regression", flush=True)

        for mode, diagnostic in (("--negative-writer-wait", "writer Signal and Flush precede the exact reader queue Wait"),
                                 ("--negative-reader-retirement", "one existing wait admits one write")):
            result = subprocess.run([str(exe), mode], cwd=HERE, capture_output=True, text=True)
            check(result.returncode == 1 and "FAIL: " + diagnostic in result.stderr,
                  f"specific missing synchronization must be detected: {mode}: {result}")
        print("PASS missing writer Wait and missing reader retirement controls rejected by production CPU checks")
