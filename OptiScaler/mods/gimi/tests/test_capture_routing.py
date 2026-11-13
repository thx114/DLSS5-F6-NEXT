"""Execute production GIMI ingress/return bodies with real D3D11 WARP owners.

Only the interpreter, plan preparation and GPU-packet retirement are counters;
no game, user configuration, NVIDIA runtime or MOD package is loaded. The
content tests exercise CPU publication/writeback, not GPU completion.
"""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--negative", choices=("opaque", "present", "device", "provisional",
                                         "provisional-return", "provisional-unmap", "stale-map", "stale-return"))
args = parser.parse_args()


def extract(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    tokens = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/|[{}]'
    for token in re.finditer(tokens, source[opening:]):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[start:opening + token.end()]
    raise ValueError(f"Incomplete production body: {signature}")


source = (ROOT / "OptiScaler/mods/gimi/GimiRuntime.cpp").read_text(encoding="utf-8-sig")
header = (ROOT / "OptiScaler/mods/gimi/GimiRuntime.h").read_text(encoding="utf-8-sig")
signatures = (
    "CaptureRoute DeviceEvent(", "CaptureRoute ContextEvent(", "std::size_t Bucket(",
    "template<class T> std::uint64_t Pointer(", "struct MapKey", "bool RegisterMapResource(",
    "CaptureRoute ResourceEvent(", "struct alignas(16) ConstantBlock", "struct ConstantContent",
    "bool CopyConstantCpu(", "struct ConstantMap\n", "struct ConstantMaps\n", "ConstantMaps& ConstantMapStore(",
    "CaptureRoute RouteOf(", "std::shared_ptr<Capture> Captured(", "void RefreshCaptureFlags(",
    "void LostCoverage(", "void CreationCoverageLocked(", "void DeviceCreated(",
    "void CreationCoverage(", "CreationHookRequest ClaimCreationHooks(", "void CompleteCreationHooks(",
    "void LayoutCreated(", "void StreamOutputCoverage(", "void StreamOutputTargets(", "void StreamOutputDrawn(",
    "void StreamOutputUnknownDraw(", "void StreamOutputCleared(", "static void InvalidateReadWrite(",
    "std::uint64_t VertexMapStarting(", "void VertexMapFailed(", "void VertexUnmapped(", "void Written(",
    "static bool ConstantUnconditional(", "CpuContentSubmission CpuContentSubmission::Update(",
    "CpuContentSubmission CpuContentSubmission::Copy(", "void ConstantMapped(",
    "CpuContentSubmission CpuContentSubmission::Unmap(", "void CpuContentSubmission::Returned(",
    "void OpaqueBoundary(", "void Present(",
)
bodies = {key: extract(source, key) for key in signatures}
if args.negative:
    key, old, new = {
        "opaque": ("void OpaqueBoundary(", "if (event) captureRouting.Lose(event);", "captureRouting.Lose();"),
        "present": ("void Present(",
                    "if (Enabled() && (!s.captureConfirmed || !s.capture || device != s.capture->device.Get() || context != s.capture->context.Get())) return;", ""),
        "device": ("void DeviceCreated(", "if (s.captureConfirmed && source != CaptureSource::PresentOwner && next != s.capture) return;", ""),
        "provisional": ("void DeviceCreated(", "if (s.captureConfirmed && source != CaptureSource::PresentOwner && next != s.capture) return;",
                        "if (s.capture && source != CaptureSource::PresentOwner && next != s.capture) return;"),
        "provisional-return": ("void CpuContentSubmission::Returned(",
                               "Captured(s, RouteOf(*capture)) != capture", "s.capture != capture"),
        "provisional-unmap": ("CpuContentSubmission CpuContentSubmission::Unmap(",
                              "Captured(s, RouteOf(*capture)) != capture", "s.capture != capture"),
        "stale-map": ("void VertexMapFailed(", "if (!event || event.generation != generation) return;", "if (!event) return;"),
        "stale-return": ("void CpuContentSubmission::Returned(", "captureRouting.Lose(RouteOf(*capture))", "captureRouting.Lose()"),
    }[args.negative]
    if bodies[key].count(old) != 1:
        raise ValueError(f"Negative-control anchor changed: {args.negative}")
    bodies[key] = bodies[key].replace(old, new)

# Definitions with actual dependencies are placed between small fixture types.
fixture = (HERE / "capture_routing.cpp").read_text(encoding="utf-8")
for key, body in bodies.items():
    fixture = fixture.replace("// PRODUCTION: " + key, body + (";" if key.startswith("struct ") else ""))
for signature in ("enum class CaptureSource", "struct CreationHookRequest", "class CpuContentSubmission final"):
    fixture = fixture.replace("// HEADER: " + signature, extract(header, signature) + ";")
if "// PRODUCTION:" in fixture or "// HEADER:" in fixture:
    raise ValueError("Unresolved production insertion")
output = ROOT / "build/gimi-owner-routing"
output.mkdir(parents=True, exist_ok=True)
label = args.negative or "positive"
cpp = output / f"capture-routing-{label}.cpp"
exe = output / f"capture-routing-{label}.exe"
cpp.write_text(fixture, encoding="utf-8")
devcmd = Path("D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat")
command = (f'cl /nologo /std:c++20 /EHsc /W4 /O2 /I"{ROOT / "OptiScaler"}" '
           f'"{cpp}" /Fo"{output / (label + ".obj")}" /Fe"{exe}" '
           '/link d3d11.lib dxgi.lib d3dcompiler.lib')
batch = output / f"capture-routing-{label}.cmd"
batch.write_text(f'@echo off\ncall "{devcmd}" -arch=x64 -host_arch=x64 >nul\n'
                 f'if errorlevel 1 exit /b %errorlevel%\n{command}\nexit /b %errorlevel%\n',
                 encoding="utf-8")
subprocess.run(["cmd.exe", "/d", "/c", str(batch)], check=True)
result = subprocess.run([str(exe)], check=False)
raise SystemExit(result.returncode)
