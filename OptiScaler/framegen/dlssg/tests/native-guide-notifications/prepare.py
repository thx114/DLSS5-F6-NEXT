"""Compile the actual producer notifications with CPU queue/fence test doubles."""
from pathlib import Path
import argparse
import re

parser = argparse.ArgumentParser()
parser.add_argument("output", type=Path)
parser.add_argument("--negative-control", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parents[5]
source = (root / "OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp").read_text(encoding="utf-8-sig")

def extract(signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

methods = "\n\n".join(extract(signature) for signature in (
    "bool DLSSG_Dx12::NativeGuideRetired(",
    "void DLSSG_Dx12::NativeGuidesSubmitting(",
    "void DLSSG_Dx12::NativeGuidesSubmitted(",
    "void DLSSG_Dx12::NativeGuidesReset(",
))
pair = extract("bool DLSSG_Dx12::SetUpscalerGuidePair(")
assert pair.index("NativeGuideRetired(use)") < pair.index("use.list=list;") < pair.index("_nativeGuideListInterest.Publish(") < pair.index("_nativeGuideTracking.store(true") < pair.index("_managedRetained=true;_continuityOwner=true;_teardownBorrowedInputs=true;_guideContractOwned=true;")
assert source.count("_nativeGuideListInterest.Publish(") == 1
assert source.count("_nativeGuideListInterest.Clear()") == 1
drain = extract("bool DLSSG_Dx12::CompletePendingTeardown(")
assert drain.index("NativeGuidesRetired()") < drain.index("for (auto& use:_nativeGuideUses) use={};") < drain.index("_nativeGuideListInterest.Clear()") < drain.index("_nativeGuideTracking.store(false")
for name in ("NativeGuidesSubmitting", "NativeGuidesSubmitted", "NativeGuidesReset"):
    notification = extract(f"void DLSSG_Dx12::{name}(")
    assert notification.index("_nativeGuideListInterest.MayContain(") < notification.index("std::lock_guard continuityLock")
if args.negative_control:
    methods, removed = re.subn(r"    if \(!_nativeGuideListInterest\.MayContain\([^\n]+\)\) return;\n", "", methods)
    if removed != 3:
        raise RuntimeError(f"Expected three production filters, found {removed}")
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(methods + "\n", encoding="utf-8")
print(f"Extracted four real production methods; negative_control={args.negative_control}")
