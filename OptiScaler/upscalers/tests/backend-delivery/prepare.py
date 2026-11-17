"""Extract production delivery tails and the complete DX11 rebuild adapter."""
from pathlib import Path
import hashlib
import json
import sys

root = Path(__file__).resolve().parents[4]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
inputs = {}


def read(name):
    path = root / name
    inputs[name] = hashlib.sha256(path.read_bytes()).hexdigest().upper()
    return path.read_text(encoding="utf-8-sig")


dx11 = read("OptiScaler/upscalers/IFeature_Dx11.cpp")
dx12 = read("OptiScaler/upscalers/IFeature_Dx12.cpp")
provider = read("OptiScaler/upscalers/FeatureProvider_Dx11.cpp")
header = read("OptiScaler/upscalers/FeatureProvider_Dx11.h")
menu = read("OptiScaler/menu/menu_common.cpp")
ngx = read("OptiScaler/inputs/NVNGX_DLSS_Dx11.cpp")
assert menu.count("BackendSelection::Commit(state.api, state.newBackend, *config);") == 1
apply = menu.index('if (MenuText::Button("切换超分##2")')
commit = menu.index("BackendSelection::Commit(state.api, state.newBackend, *config);")
assert apply < commit < menu.index("MARK_ALL_BACKENDS_CHANGED();", apply)
assert "cfg.Dx11Upscaler =" not in provider


def function(text, marker):
    start = text.index(marker)
    opening = text.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


macro_start = menu.index('#define MARK_ALL_BACKENDS_CHANGED()')
macro_end = menu.index('static float fontSize', macro_start)
(out / "Dx11MenuApplyProduction.inl").write_text(
    menu[macro_start:macro_end] + function(menu, 'if (MenuText::Button("切换超分##2")') +
    '\n#undef MARK_ALL_BACKENDS_CHANGED\n', encoding="utf-8")
registration = ngx.index('    // Register every successfully created handle immediately.')
(out / "Dx11CreateRegistrationProduction.inl").write_text(
    ngx[registration:ngx.index('    auto deviceContext =', registration)], encoding="utf-8")


tails = [
    ("Dx11DeliveryProduction.inl", dx11, "    const auto evaluation = EvaluateInternal(InDeviceContext"),
    ("Dx12DeliveryProduction.inl", dx12, "    // Upscaler will write to the first active shader"),
]
for name, source, marker in tails:
    begin = source.index(marker)
    end = source.index("\n}\n", begin)
    (out / name).write_text(source[begin:end], encoding="utf-8")
(out / "Dx11RebuildProduction.inl").write_text(
    function(provider, "bool FeatureProvider_Dx11::ChangeFeature("), encoding="utf-8")
(out / "Dx11FallbackProduction.inl").write_text(
    function(header, "    static constexpr Upscaler FallbackUpscaler("), encoding="utf-8")

backends = [
    "dlss/DLSSFeature_Dx11", "dlss/DLSSFeature_Dx12",
    "dlssd/DLSSDFeature_Dx11", "dlssd/DLSSDFeature_Dx12",
    "fsr2/FSR2Feature_Dx11", "fsr2/FSR2Feature_Dx12",
    "fsr2_212/FSR2Feature_Dx12_212", "fsr31/FSR31Feature_Dx11",
    "xess/XeSSFeature_Dx11", "xess/XeSSFeature_Dx12", "ffx/FFXFeature_Dx12",
]
for backend in backends:
    source = read(f"OptiScaler/upscalers/{backend}.cpp")
    signature = source.index("::EvaluateInternal(")
    body = function(source, source[source.rfind("\n", 0, signature) + 1:signature + 19])
    executable = "\n".join(line for line in body.splitlines() if not line.lstrip().startswith("//"))
    assert "return true;" not in executable and "return false;" not in executable, backend
    assert executable.count("return FeatureEvaluation::OutputRecorded;") == 1, backend
    assert executable.rstrip().endswith("return FeatureEvaluation::OutputRecorded;\n}"), backend
    assert "Result" in executable or "result" in executable, backend
    # All deferred rebuild returns are explicit and cannot be mistaken for output.
    for branch in executable.split("return FeatureEvaluation::NoOutput;")[:-1]:
        assert "changeBackend[" in branch[-220:], backend
(out / "extraction.json").write_text(json.dumps(inputs, indent=2) + "\n", encoding="utf-8")
print("PASS extracted production DX11/DX12 delivery, full DX11 rebuild and 11 provider result contracts")
