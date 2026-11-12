"""Check loader/exit veto placement on real FG cleanup entrypoints.

Windows context and complete-owner behavior are executed by the adjacent
module-lifetime fixture and the FG hook/Release adapter fixtures.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]
checks = 0


def check(condition, message):
    global checks
    if not condition:
        raise AssertionError(message)
    checks += 1


def body(source, signature):
    start = source.index("{", source.index(signature))
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start + 1:end - 1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


def before_work(source, signature, refusal):
    entry = body(source, signature)
    check(refusal in entry, signature + " has an explicit non-success veto")
    prefix = entry[:entry.index(refusal)]
    prefix = re.sub(r"//[^\n]*|/\*[\s\S]*?\*/", "", prefix).strip()
    prefix = re.sub(r"using \w+ = [^;]+;", "", prefix).strip()
    check(not prefix, signature + " veto precedes locks/logs/COM/SDK and state changes")


dlssg = read("OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp")
native = read("OptiScaler/hooks/ngx/NativeDx12Hooks.cpp")
for method in ("Stop", "StopDevice", "Release", "BStop", "BStopDevice", "BRelease", "Destroy"):
    before_work(native, "NVSDK_NGX_Result " + method + "(",
                "if (!ModuleLifetime::CleanupAllowed()) return NVSDK_NGX_Result_Fail;")
for api in ("D3D11", "D3D12"):
    source = read(f"OptiScaler/inputs/NVNGX_DLSS_{'Dx11' if api == 'D3D11' else 'Dx12'}.cpp")
    for method in ("ReleaseFeature", "DestroyParameters") + (("Shutdown", "Shutdown1") if api == "D3D11" else ()):
        before_work(source, f"NVSDK_NGX_Result NVSDK_NGX_{api}_{method}(",
                    "if (!ModuleLifetime::CleanupAllowed()) return NVSDK_NGX_Result_Fail;")
    if api == "D3D12":
        before_work(source, "static NVSDK_NGX_Result ShutdownDx12(",
                    "if (!ModuleLifetime::CleanupAllowed()) return NVSDK_NGX_Result_Fail;")
        for method in ("Shutdown", "Shutdown1"):
            entry = body(source, f"NVSDK_NGX_Result NVSDK_NGX_D3D12_{method}(")
            check(re.fullmatch(r"\s*return ShutdownDx12\([^;]+;\s*", entry),
                  method + " does no work before the common DX12 shutdown veto")
for method in ("Deactivate", "DestroyFGContext"):
    before_work(dlssg, "void DLSSG_Dx12::" + method + "(",
                "if (!ModuleLifetime::CleanupAllowed()) return;")
for method in ("CompletePendingTeardown", "CompletePendingTeardownForTermination",
               "PrepareForSwapchainChange", "Shutdown", "ReleaseObjectsChecked", "ReleaseSwapchain"):
    before_work(dlssg, "bool DLSSG_Dx12::" + method + "(",
                "if (!ModuleLifetime::CleanupAllowed()) return false;")
before_work(dlssg, "FGHookRetirementResult DLSSG_Dx12::CompletePendingTeardownForHook(",
            "if (!ModuleLifetime::CleanupAllowed()) return { Result::Refused, DXGI_ERROR_WAS_STILL_DRAWING };")
before_work(dlssg, "FGHookRetirementReceipt DLSSG_Dx12::RetireSwapchainForHook(",
            "if (!ModuleLifetime::CleanupAllowed()) return FGHookRetirementReceipt::Refused(DXGI_ERROR_WAS_STILL_DRAWING);")
before_work(dlssg, "dlssg::options::Submission DLSSG_Dx12::SetReleaseOptions(",
            "if (!ModuleLifetime::CleanupAllowed()) return {};")

hooks = read("OptiScaler/hooks/FG_Hooks.cpp")
for path, owner in (("OptiScaler/with_dx12/dx11_with_dx12_sc.cpp", "Dx11wDx12SC"),
                    ("OptiScaler/wrapped/wrapped_swapchain.cpp", "WrappedIDXGISwapChain4")):
    source = read(path)
    for method in ("HookSetFullscreenState", "HookResizeBuffers", "HookResizeTarget", "ResizeBuffers1"):
        before_work(source, f"HRESULT STDMETHODCALLTYPE {owner}::{method}(",
                    "if (!ModuleLifetime::CleanupAllowed()) return DXGI_ERROR_WAS_STILL_DRAWING;")
    if owner == "Dx11wDx12SC":
        for method in ("SetRotation", "SetSourceSize", "SetMatrixTransform"):
            before_work(source, f"HRESULT STDMETHODCALLTYPE {owner}::{method}(",
                        "if (!ModuleLifetime::CleanupAllowed()) return DXGI_ERROR_WAS_STILL_DRAWING;")
        for method in ("_TryDrainNativeChild", "_TryRecoverNativeChild"):
            before_work(source, f"bool {owner}::{method}(",
                        "if (!ModuleLifetime::CleanupAllowed()) return false;")
        for method in ("_TryRetireNativeChildOnOwner", "_RevokeNativeChild"):
            before_work(source, f"void {owner}::{method}(",
                        "if (!ModuleLifetime::CleanupAllowed()) return;")
for method in ("hkSetFullscreenState", "hkResizeTarget"):
    before_work(hooks, "HRESULT FGHooks::" + method + "(",
                "if (!ModuleLifetime::CleanupAllowed()) return DXGI_ERROR_WAS_STILL_DRAWING;")
for method in ("RetireFGResources", "CompleteExistingFGTeardown"):
    before_work(hooks, "FGHookRetirementResult " + method + "(",
                "if (!ModuleLifetime::CleanupAllowed()) return { Result::Refused, DXGI_ERROR_WAS_STILL_DRAWING };")
for method in ("hkResizeBuffers", "hkResizeBuffers1"):
    entry = body(hooks, "HRESULT FGHooks::" + method + "(")
    veto = entry.index("if (!ModuleLifetime::CleanupAllowed()) return DXGI_ERROR_WAS_STILL_DRAWING;")
    for token in ("HoYoShade::ColorConsumers", "fgResizeForward", "std::unique_lock lifetime", "GuardExecution("):
        check(veto < entry.index(token), method + " veto precedes " + token)
release = body(hooks, "ULONG FGHooks::hkFGRelease(")
check(release.index("ModuleLifetime::CleanupAllowed()") < release.index("std::unique_lock lifetime"),
      "Release veto precedes the owner lock while consuming only the caller reference")

lifetime = read("OptiScaler/runtime/ModuleLifetime.h")
context = body(lifetime, "CurrentContext()")
check(all(token not in context for token in ("GetModuleHandle", "GetProcAddress", "InitializeContextProbe", "LoadLibrary")),
      "runtime context check never resolves loader APIs")
check("contextProbe.load(std::memory_order_acquire)" in context and "LoaderStartupContext::Probe(" in context,
      "runtime context reads the published probe and retains the real loader check")
pin = body(lifetime, "PinCallbacks(")
check("InitializeContextProbe();" in pin and "GET_MODULE_HANDLE_EX_FLAG_PIN" in pin,
      "callback registration fixes module lifetime and publishes the probe")
main = read("OptiScaler/dllmain.cpp")
attach = main[main.index("case DLL_PROCESS_ATTACH:"):main.index("case DLL_PROCESS_DETACH:")]
check(attach.index("ModuleLifetime::PinCallbacks(hModule)") < attach.index("DisableThreadLibraryCalls("),
      "module lifetime is fixed before initialization publishes callbacks")
detach = main[main.index("case DLL_PROCESS_DETACH:"):main.index("case DLL_THREAD_ATTACH:")]
check(all(token not in detach for token in ("Shutdown(", "CloseLogger(", "FreeLibrary", "LOG_", "spdlog::", "WaitFor")),
      "detach performs no SDK/logger/module cleanup or waits")
kernel = body(read("OptiScaler/hooks/Kernel_Hooks.cpp"), "VOID WINAPI KernelHooks::hk_K32_ExitProcess(")
check(kernel.index("RequestProcessExit();") < kernel.index("DrainLogForExit();"),
      "ExitProcess revokes new cleanup before the existing log drain")
print(f"PASS {checks} cleanup entrypoint/cache/attach/exit wiring checks; not a game or SDK test")
