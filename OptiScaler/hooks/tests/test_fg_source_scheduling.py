"""Static wiring plus optional CPU/concurrency tests. Only Lead should run it.

python -B OptiScaler/hooks/tests/test_fg_source_scheduling.py --cpu
python -B OptiScaler/hooks/tests/test_fg_source_scheduling.py --cpu --negative-locked-wait
python -B OptiScaler/hooks/tests/test_fg_source_scheduling.py --revision HEAD

The negative locked-wait executable must fail: the actual old FinishWork
contract leaves continuity held. --revision HEAD checks old production wiring
and must fail, before any compiler invocation, on the FSR-dependent source gate
and consumptive/fictional state reporting. No product build or deployment.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cpu",action="store_true")
parser.add_argument("--negative-locked-wait",action="store_true")
parser.add_argument("--revision")
parser.add_argument("--devcmd",type=Path,default=Path("D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat"))
args=parser.parse_args()


def read(path):
    if args.revision:
        return subprocess.check_output(["git","show",f"{args.revision}:{path}"],cwd=ROOT).decode("utf-8-sig")
    return (ROOT/path).read_text(encoding="utf-8-sig")


def check(ok,message):
    if not ok:
        raise SystemExit(f"FAIL: {message}")


def section(source,start,end):
    check(start in source and end in source,f"production boundaries exist: {start}")
    begin=source.index(start)
    return source[begin:source.index(end,begin)]


fg=read("OptiScaler/hooks/FG_Hooks.cpp")
hooks=read("OptiScaler/hooks/Streamline_Hooks.cpp")
interface=read("OptiScaler/framegen/IFGFeature_Dx12.h")
check("const bool genshinPacing = sourcePacingScope.Owns();" in fg,
      "real direct-DLSS source owns pacing independently of disabled FSR bridge")
check("F6InputPolicy::DirectDlssInputs && genshinDirectExecutable" in fg,
      "direct source route requires product input policy and exact executable")
decision=section(fg,"// FG source pacing decision begin","// FG source pacing decision end")
check("pacingLease=std::move(sourcePacingLease)" in decision,
      "guarded decision retains the original reservation rather than reacquiring after Present")
begin=section(fg,"    sl::FrameToken* localToken = nullptr;","    const bool fgPresentAdmitted")
check("StreamlineHooks::BeginDlssgPacing(" in begin and "executionOwner->PacingLifetime()" in begin and
      "StreamlineProxy::RawGetNewFrameToken()" in begin,
      "concrete source lifecycle reserves BEFORE requesting the original SDK token")
check("AcquirePacingLease" not in fg,"no empty base lease silently disables Reflex pacing")
provider=read("OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp")
provider_header=read("OptiScaler/framegen/dlssg/DLSSG_Dx12.h")
check("PacingLifetime() override final" in provider_header and
      "std::shared_ptr<framegen::DlssgPacingLifetime> DLSSG_Dx12::PacingLifetime()" in provider and
      "_pacingLifetime->OpenAdmission()" in provider,
      "real DLSSG provider implements admitted token lifecycle rather than empty default")
for start,end in (
    ("void DLSSG_Dx12::Deactivate()","void DLSSG_Dx12::RetryUnretainedProviderOff()"),
    ("bool DLSSG_Dx12::Shutdown()","bool DLSSG_Dx12::OwnsDepthContract()"),
    ("bool DLSSG_Dx12::ReleaseObjectsChecked()","void DLSSG_Dx12::CreateObjects(")):
    body=section(provider,start,end)
    check("_pacingLifetime->CloseAdmission()" in body,f"SDK lifecycle closes pacing admission: {start}")
check("_pacingLifetime->Idle()" in section(provider,"bool DLSSG_Dx12::CompletePendingTeardown()",
      "bool DLSSG_Dx12::CompletePendingTeardownForTermination()"),
      "actual teardown defers SDK free while an original token lease is live")
check(decision.index("const auto pacingDecision=")<decision.index("presentMutexRelease.Release();")<
      decision.index("presentExecution.Finish();"),"immutable pacing decision precedes FG mutex/execution unlock")
check(fg.index("physicalInput.reset();")<fg.index("presentExecution.FinishWork();"),
      "actual SDK input/output transaction closes before teardown/unlock")
after=section(fg,"    if (genshinPacing)\n    {\n        // No provider, raw SDK token","    if(genshinSdkTransaction){\n        // No new state query")
for forbidden in ("fg->","executionOwner->","*localToken","ReflexHooks::","Config::","state."):
    check(forbidden not in after,f"post-unlock pacing has no mutable/raw provider access: {forbidden}")
check("pacingLease->Sleep()" in after and "pacingDecision.frameLimit" in after,
      "post-unlock pacing executes only its retained lease and immutable policy")
state_hook=section(hooks,"sl::Result StreamlineHooks::hkslDLSSGGetState(",
                   "bool StreamlineHooks::hkreflex_slOnPluginLoad(")
check("state.numFramesActuallyPresented =" not in state_hook,
      "game state preserves raw SDK query-interval presented counts")
check("o_slDLSSGGetState(viewport" not in hooks,"all hook SDK reads go through the central serial gate")
options=section(hooks,"sl::Result StreamlineHooks::hkslDLSSGSetOptions(","sl::Result StreamlineHooks::hkslDLSSGSetOptionsSerial(")
check("ReadDlssgStateSample" in options and "QueryDlssgState(" not in options,
      "cold capability lookup inside SetOptions never consumes another counter interval")
check("SubmitDlssgOptions(o_slDLSSGSetOptions" in options,"game SetOptions shares SDK query serialization")
for path,owner in (("OptiScaler/wrapped/wrapped_swapchain.cpp","WrappedIDXGISwapChain4"),
                   ("OptiScaler/with_dx12/dx11_with_dx12_sc.cpp","Dx11wDx12SC")):
    source=read(path)
    getter=section(source,f"HANDLE STDMETHODCALLTYPE {owner}::GetFrameLatencyWaitableObject(void)",
                   f"HRESULT STDMETHODCALLTYPE {owner}::SetMatrixTransform")
    check("FrameLatencyCaller(_ReturnAddress())" in getter and "DlssgPluginLoaded()" in getter,
          f"{owner} observes actual caller role and SDK residency independently of FG mode")
    check("framegen::latency::Select" in getter and "Target::None" in getter,
          f"{owner} refuses only proven app-owned unrequested SDK wait objects")
    check("applicationFlags.has_value()" in source,"factory original flags remain distinct from effective flags")
print("PASS FG source pacing/query/waitable wiring (CPU/SDK/game not executed)",flush=True)
if not args.cpu:
    raise SystemExit(0)
check(not args.revision,"CPU fixture is bound to current real headers; use revision only for static negative control")
guard=section(interface,"class FGExecutionGuard\n{","struct Dx12Resource")
release=section(fg,"    struct PresentMutexRelease {","    } presentMutexRelease")+"    };\n"
fixture=(HERE/"fg_source_scheduling.cpp").read_text(encoding="utf-8-sig")
with tempfile.TemporaryDirectory(prefix=".fg-source-cpu-",dir=HERE) as temporary:
    directory=Path(temporary)
    cpp=directory/"fixture.cpp"
    cpp.write_text(fixture.replace("// INSERT_EXECUTION_GUARD",guard).replace("// INSERT_MUTEX_RELEASE",release),encoding="utf-8")
    exe=directory/"fixture.exe"
    negative=" /DF6_NEGATIVE_LOCKED_PACING" if args.negative_locked_wait else ""
    batch=directory/"compile.cmd"
    batch.write_text(
        f'@echo off\ncall "{args.devcmd}" -no_logo -arch=x64 -host_arch=x64\n'
        'if errorlevel 1 exit /b %errorlevel%\n'
        f'cl /nologo /std:c++20 /EHsc /W4 /WX /Od{negative} /I"{ROOT / "OptiScaler"}" '
        f'/I"{ROOT / "external/streamline"}" /Fe:"{exe}" /Fo:"{directory}/" "{cpp}"\n'
        'exit /b %errorlevel%\n',encoding="utf-8",newline="\r\n")
    subprocess.run(["cmd.exe","/d","/c",str(batch)],cwd=directory,check=True)
    subprocess.run([str(exe)],cwd=directory,check=True,timeout=30)
