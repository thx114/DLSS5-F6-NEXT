"""Extract real FG device/queue adapters; only the Lead runs CPU compilation.

python -B OptiScaler/hooks/tests/test_fg_device_identity.py
python -B OptiScaler/hooks/tests/test_fg_device_identity.py --cpu
python -B OptiScaler/hooks/tests/test_fg_device_identity.py --cpu --negative-strict-device-control

Positive CPU execution must pass; the strict-device control must exit nonzero
at a known-wrapper scenario. Static-only success is not CPU execution evidence.
Real Windows COM/WRL headers are required, but no FG SDK, D3D runtime or GPU.
Generated compilation inputs and executables live in an OS temporary directory.
"""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
checks = 0


def check(condition, message):
    global checks
    checks += 1
    if not condition:
        raise SystemExit("FAIL: " + message)


def block(text, start):
    """Extract a definition, ignoring comment/string braces without rewriting it."""
    check(text.count(start) == 1, "one definition: " + start)
    begin = text.index(start)
    masked = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda match: " " * len(match.group()), text)
    opened = masked.index("{", begin)
    depth = 1
    end = opened + 1
    while depth and end < len(masked):
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    check(depth == 0, "balanced definition: " + start)
    return text[begin:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpu", action="store_true")
    parser.add_argument("--negative-strict-device-control", action="store_true")
    parser.add_argument("--negative-nr-identity-control", action="store_true")
    parser.add_argument("--devcmd", type=Path,
                        default=Path("D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat"))
    args = parser.parse_args()
    check(not args.negative_strict_device_control or args.cpu,
          "executable negative control requires --cpu")

    paths = [ROOT / "OptiScaler/hooks/FG_Hooks.cpp",
             ROOT / "OptiScaler/runtime/D3D12DeviceIdentity.h",
             ROOT / "OptiScaler/hooks/FGQueueIdleGate.h",
             ROOT / "OptiScaler/framegen/FGHookRetirement.h",
             HERE / "fg_device_identity.cpp", HERE / "device-identity-stubs/d3d12.h",
             ROOT / "OptiScaler/shaders/dlssnr/DlssNr_DetailReuse.inl",
             ROOT / "OptiScaler/shaders/dlssnr/DlssNr_RecordingLifetime.h"]
    snapshot = {path: path.read_bytes() for path in paths}
    source = snapshot[paths[0]].decode("utf-8-sig")
    adapters = {
        "CALLBACK": block(source, "struct ScopedFGCallback") + ";",
        "OWNER": block(source, "struct FGSwapchainOwner") + ";",
        "STRICT": block(source, "bool SameComObject(IUnknown* a, IUnknown* b)"),
        "IO": block(source, "struct FGQueueIo") + ";",
        "COMPLETE": block(source, "FGHookRetirementResult CompleteFGQueue("),
        "NR_RECORDING_IDENTITY": "namespace DlssNrGpuLifetime {\n" + block(snapshot[paths[7]].decode("utf-8-sig"),
            "inline bool SameDevice(ID3D12DeviceChild* a, ID3D12DeviceChild* b)") + "\n}",
        "NR_IDENTITY": "namespace DetailReuse {\n" + block(snapshot[paths[6]].decode("utf-8-sig"),
            "bool SameDevice(ID3D12DeviceChild* a, ID3D12DeviceChild* b)") + "\n}",
    }
    if args.negative_nr_identity_control:
        check(args.cpu, "NR identity negative requires CPU execution")
        original = adapters["NR_RECORDING_IDENTITY"]
        adapters["NR_RECORDING_IDENTITY"] = original.replace("D3D12DeviceIdentity::Same(", "SameComObject(")
        check(adapters["NR_RECORDING_IDENTITY"] != original, "NR identity negative changes production resolver")
    device_status = block(adapters["IO"], "HRESULT DeviceStatus() const")
    check(device_status.count("D3D12DeviceIdentity::Same(") == 2,
          "production validates queue and fence against the bound device")
    check("#include <runtime/D3D12DeviceIdentity.h>" in source,
          "production includes the real identity header")
    if args.negative_strict_device_control:
        legacy = device_status.replace("D3D12DeviceIdentity::Same(", "SameComObject(")
        check(legacy != device_status, "negative mutation changes DeviceStatus")
        adapters["IO"] = adapters["IO"].replace(device_status, legacy, 1)
        print("CONTROL strict DeviceStatus mutation enabled; CPU assertion must fail", flush=True)

    fixture = snapshot[paths[4]].decode("utf-8-sig")
    for key, adapter in adapters.items():
        marker = "// INSERT_PRODUCT_" + key + "\n"
        check(fixture.count(marker) == 1, "single insertion point: " + key)
        fixture = fixture.replace(marker, adapter + "\n")
    check("// INSERT_PRODUCT_" not in fixture, "all product adapters inserted")
    for include in ("runtime/D3D12DeviceIdentity.h", "hooks/FGQueueIdleGate.h",
                    "framegen/FGHookRetirement.h", "wrl/client.h"):
        check("#include <" + include + ">" in fixture, "real include: " + include)
    check("namespace Microsoft" not in fixture, "fixture does not replace WRL")
    check("#undef IID_PPV_ARGS" not in fixture, "fixture preserves Windows IID_PPV_ARGS")
    check(snapshot[paths[4]].isascii() and snapshot[paths[5]].isascii(), "C++ inputs are ACP-safe ASCII")

    def unchanged():
        for path, original in snapshot.items():
            check(path.read_bytes() == original, "source snapshot unchanged: " + str(path))

    unchanged()
    for path in paths[:4]:
        print("INPUT", path.relative_to(ROOT).as_posix(),
              hashlib.sha256(snapshot[path]).hexdigest(), flush=True)
    print(f"PASS static extraction/wiring: {checks} checks; CPU scenarios NOT RUN", flush=True)
    if not args.cpu:
        return
    with tempfile.TemporaryDirectory(prefix="fg-device-identity-") as temporary:
        directory = Path(temporary)
        cpp, exe = directory / "fixture.cpp", directory / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        batch = directory / "compile.cmd"
        batch.write_text(
            f'@echo off\ncall "{args.devcmd}" -no_logo -arch=x64 -host_arch=x64\n'
            'if errorlevel 1 exit /b %errorlevel%\n'
            f'cl /nologo /std:c++20 /EHsc /W4 /WX /Od '
            f'/I"{HERE / "device-identity-stubs"}" /I"{ROOT / "OptiScaler"}" '
            f'/Fe:"{exe}" /Fo:"{directory}/" "{cpp}"\n'
            'exit /b %errorlevel%\n', encoding="utf-8", newline="\r\n")
        unchanged()
        subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=directory, check=True)
        unchanged()
        result = subprocess.run([str(exe)], cwd=directory, check=False)
        unchanged()
        if result.returncode:
            raise SystemExit(result.returncode)
        # Preserve the fixture's real exit code. A surviving negative control
        # must exit zero so a caller expecting failure can detect that survival.
    print(f"PASS source snapshot and static checks: {checks}", flush=True)


if __name__ == "__main__":
    main()
