"""Resident full-model wiring and the real Host::Gate/Decide/Cadence CPU test.

--cpu compiles this isolated test only. Environment stubs replace Config/State/
status, never the production Gate, cadence, guide policies or producer helper.
--negative-control additionally proves removing the production Gate rejection
fails the same warm-history test. No DLL, SDK execution or game is involved.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cpu", action="store_true")
parser.add_argument("--negative-control", action="store_true")
parser.add_argument("--devcmd", type=Path, default=Path("D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat"))
args = parser.parse_args()
checks = 0


def check(ok, message):
    global checks
    checks += 1
    if not ok:
        raise SystemExit(f"FAIL: {message}")


def source(path):
    return (ROOT / "OptiScaler" / path).read_text(encoding="utf-8-sig")


frame = source("shaders/dlssnr/DlssNr_FrameInfo.h")
resident = source("runtime/NrResident.h")
dx12 = source("shaders/dlssnr/DlssNr_Dx12.cpp")
backend = source("shaders/dlssnr/DlssNr_DetailReuse.inl")
host = source("dlssnr/DlssNrDetailReuseHost.h")
check("bool FullModelRequired = false;" in frame, "all existing producers default to their previous semantics")
helper = resident[resident.index("template<class Frame> void UseMeasuredFlow"):resident.index("// Recording/Evaluate")]
check("frame.FullModelRequired = true;" in helper, "actual resident measured-flow producer requires full model")
check("frame.FreshColorResetFallback = false;" in helper and "frame.MotionIsPictureFlow = frame.MotionMatchesPicture = true;" in helper,
      "measured motion remains valid, independently of the zero-motion reset fallback")
mapping = re.search(r"reuseFrame\.fullModelRequired\s*=\s*frame\.FullModelRequired\s*;", dx12)
check(mapping is not None and mapping.start() < dx12.index("reusePlan = DetailReuse::BeforeModel(reuseFrame)"),
      "actual DX12 consumer forwards full-model requirement before reuse planning")
check("DlssNrDetailReuse::HostFrame facts = f;" in backend and "host.Gate(" in backend and "host.Decide(facts, plan.active, want)" in backend,
      "real reuse planner carries inherited full-model fact to Gate and cadence")
gate = host[host.index("template <class ShaderReady> Wanted Gate"):host.index("void AllocationFailed")]
check(gate.index("f.fullModelRequired") < gate.index("UnavailableOnRoute(") < gate.index("shaderReady()"),
      "full-model rejection precedes legacy motion-only route and shader allocation")
check("plan.active = Prepare(f, want.wanted, want.keep" in backend,
      "production allocation and cadence activity consume the real Gate result")
vit = re.search(r"const bool vitEveryFrame\s*=\s*([^;]+);", dx12)
check(vit is not None and "frame.FullModelRequired" in vit[1], "actual model loop disables bottleneck reuse too")
check("vitEveryFrame ? 1u : std::clamp(cfg.DlssNrVitEvery.value_or_default()" in dx12 and
      "vitEveryFrame ? 1u : std::clamp(cfg.DlssNrVitEveryPlain.value_or_default()" in dx12,
      "both actual bottleneck cadence arguments evaluate every required frame")
print(f"NrResidentConsumer wiring: {checks} checks passed", flush=True)

if args.cpu or args.negative_control:
    output = ROOT / "build" / "nr-resident-consumer"
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cpu-", dir=output) as temporary:
        scratch = Path(temporary)

        def run(label, override=None):
            exe = scratch / f"{label}.exe"
            includes = [HERE / "nr-resident" / "stubs"]
            if override:
                includes.append(override)
            includes.append(ROOT / "OptiScaler")
            include_args = " ".join(f'/I"{path}"' for path in includes)
            command = (f'call "{args.devcmd}" -no_logo -arch=x64 -host_arch=x64 && '
                       f'cl /nologo /std:c++20 /EHsc /W4 /WX /Od {include_args} '
                       f'/Fe:"{exe}" /Fo:"{scratch / (label + ".obj")}" "{HERE / "NrResidentConsumerTests.cpp"}"')
            subprocess.run(["cmd.exe", "/d", "/s", "/c", command], cwd=scratch, check=True)
            return subprocess.run([str(exe)], cwd=scratch, capture_output=True, text=True)

        result = run("positive")
        print(result.stdout, end="")
        check(result.returncode == 0, f"real Gate/cadence CPU test: {result.stderr}")
        if args.negative_control:
            rejection = ('f.fullModelRequired\n'
                         '                                 ? "current picture requires full model evaluation"\n'
                         '                                 : f.edgeCompression')
            check(rejection in host, "negative control can remove only the full-model Gate rejection")
            override = scratch / "negative"
            (override / "dlssnr").mkdir(parents=True)
            (override / "dlssnr" / "DlssNrDetailReuseHost.h").write_text(
                host.replace(rejection, "f.edgeCompression", 1), encoding="utf-8")
            result = run("negative", override)
            check(result.returncode != 0 and "resident full-model Gate rejects reuse even with warm history" in result.stderr,
                  "warm-history regression must fail without the real consumer rejection")
            print("NrResidentConsumer negative control: missing Gate rejection detected")
