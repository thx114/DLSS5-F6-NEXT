"""Extract real recording owners/notifications for the CPU-only fixture.

The normal runner retains its missing-filter control. Additional mutations can
be compiled by the lead with --mutation; every mutant must return fixture code
42 (compilation failure is not evidence that a regression was detected).
--check-only validates extraction without generating or compiling anything.
"""
from pathlib import Path
import argparse
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path, nargs="?")
parser.add_argument("--negative-control", action="store_true")
parser.add_argument("--mutation", choices=("missing-reset", "wrong-slot", "missing-execution", "old-observation", "missing-lease"))
parser.add_argument("--check-only", action="store_true")
args = parser.parse_args()
if args.negative_control and args.mutation:
    parser.error("choose one negative control at a time")
if not args.check_only and args.output is None:
    parser.error("output is required unless --check-only is used")
root = Path(__file__).resolve().parents[5]
source = (root / "OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp").read_text(encoding="utf-8-sig")
header = (root / "OptiScaler/framegen/dlssg/DLSSG_Dx12.h").read_text(encoding="utf-8-sig")


def extract(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


signatures = (
    "bool DLSSG_Dx12::NativeGuideRetired(",
    "std::shared_ptr<DLSSG_Dx12::NativeGuideUse> DLSSG_Dx12::AcquireNativeGuideRecording(",
    "void DLSSG_Dx12::ClearNativeGuideFrame(",
    "dlssg::NativeSource DLSSG_Dx12::NativeGuideFrameSource(",
    "bool DLSSG_Dx12::NativeGuidesRetired(",
    "UINT64 DLSSG_Dx12::NativeGuidesSubmitting(",
    "void DLSSG_Dx12::NativeGuidesSubmitted(",
    "void DLSSG_Dx12::NativeGuidesReset(",
    "std::shared_ptr<DLSSG_Dx12::NativeGuideUse> DLSSG_Dx12::FindNativeGuideSource(",
    "void DLSSG_Dx12::CompleteNativeGuideSource(",
)
methods = "\n\n".join(extract(source, signature) for signature in signatures)
owner = extract(header, "struct NativeGuideUse") + ";"
pair = extract(source, "bool DLSSG_Dx12::SetUpscalerGuidePair(")
assert pair.index("AcquireNativeGuideRecording()") < pair.index("use.list=list;") < pair.index("_nativeGuideFrames[slot]=&use;") < pair.index("_nativeGuideListInterest.Publish(") < pair.index("_nativeGuideTracking.store(true")
assert pair.index("_nativeGuideTracking.store(true") < pair.index("_managedRetained=true;_continuityOwner=true;_teardownBorrowedInputs=true;_guideContractOwned=true;")
assert source.count("_nativeGuideListInterest.Publish(") == 1
assert source.count("_nativeGuideListInterest.Clear()") == 1
drain = extract(source, "bool DLSSG_Dx12::CompletePendingTeardown(")
assert drain.index("NativeGuidesRetired()") < drain.index("_nativeGuideRecordings.clear()") < drain.index("_nativeGuideListInterest.Clear()") < drain.index("_nativeGuideTracking.store(false")
for signature in signatures[5:8]:
    notification = extract(source, signature)
    assert notification.index("_nativeGuideListInterest.MayContain(") < notification.index("std::lock_guard continuityLock")


def replace_once(old, new):
    global methods
    if methods.count(old) != 1:
        raise RuntimeError(f"Mutation anchor must occur once: {old!r}")
    methods = methods.replace(old, new, 1)


if args.negative_control:
    methods, removed = re.subn(
        r"    if \(!_nativeGuideListInterest\.MayContain\([^\n]+\)\) return(?: 0)?;\n", "", methods)
    if removed != 3:
        raise RuntimeError(f"Expected three production filters, found {removed}")
elif args.mutation == "missing-reset":
    replace_once("_nativeGuideFailed || !use.closed", "_nativeGuideFailed")
elif args.mutation == "wrong-slot":
    replace_once("if (_nativeGuideFrames[slot]!=&use) continue;",
                 "if (slot!=GetIndex()) continue;")
elif args.mutation == "missing-execution":
    replace_once(" || use.execution!=execution", "")
elif args.mutation == "missing-lease":
    replace_once("it->use_count()!=1 || ", "")
elif args.mutation == "old-observation":
    replace_once("if (_nativeGuideFrames[slot]==&use && _guideObservations[slot].frame==use.source.frame &&\n"
                 "            _guideObservations[slot].snapshot.epoch==use.source.generation)", "if (true)")

if not args.check_only:
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("#ifdef NATIVE_GUIDE_TYPES_ONLY\n" + owner + "\n#else\n" + methods + "\n#endif\n", encoding="utf-8")
print(f"Validated real NativeGuideUse and {len(signatures)} production methods; "
      f"control={'missing-filter' if args.negative_control else args.mutation or 'production'}; "
      f"{'no files generated' if args.check_only else 'include generated'}; C++ not compiled")
