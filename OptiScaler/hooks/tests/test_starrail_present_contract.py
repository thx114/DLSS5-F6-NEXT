"""Run the real StarRail final-admission bodies with fake SDK calls.

Uses the production InputContinuity and suppression transaction. No game or
graphics SDK is loaded; --legacy-gap-grace must fail the first-gap check.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--legacy-gap-grace", action="store_true")
args = parser.parse_args()
provider = (ROOT / "OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp").read_text(encoding="utf-8-sig")
provider += "\n" + "\n".join(p.read_text(encoding="utf-8-sig") for p in sorted(
    next(a / "OptiScaler/framegen/dlssg" for a in Path(__file__).resolve().parents
         if (a / "OptiScaler/framegen/dlssg").is_dir()).glob("DLSSG_Dx12_*.cpp")))
hooks = (ROOT / "OptiScaler/hooks/FG_Hooks.cpp").read_text(encoding="utf-8-sig")


def function(signature):
    start = provider.index(signature)
    brace = provider.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (provider[end] == "{") - (provider[end] == "}")
        end += 1
    return provider[start:end]


gap = function("bool DLSSG_Dx12::PresentStarRailInputGap(").replace("DLSSG_Dx12::", "")
prepare = function("DLSSG_Dx12::StarRailPresent DLSSG_Dx12::PrepareStarRailPresent(")
prepare = prepare.replace("DLSSG_Dx12::", "")
if args.legacy_gap_grace:
    gap = gap.replace("return _inputContinuity.Held() ?", "if (++legacyGapCount < 4) return true;\n    return _inputContinuity.Held() ?")

# The CPU fixture below executes final admission. These wiring checks ensure
# the real hook obtains its pacing lease only after that admission and Dispatch.
early_begin = hooks.index("if (willPresent && fgFeatureActive && state.activeFgOutput")
early_end = hooks.index("const bool fgPresentAdmitted", early_begin)
assert "!starRailF5DsScheduling" in hooks[early_begin:early_end]
dispatch_at = hooks.index("const bool dispatched=fg->Present();")
admission_at = hooks.index("PrepareStarRailPresent(true, &starRailDispatchedFrame)")
start_at = hooks.index("beginSourcePresentPacing(starRailDispatchedFrame)")
sdk_at = hooks.index("? o_FGSCPresent(This, SyncInterval, Flags)")
assert dispatch_at < admission_at < start_at < sdk_at
assert "fgPacingActive() && starRailDispatchedFrame" in hooks[admission_at:start_at]
helper_start = hooks.index("const auto beginSourcePresentPacing =")
helper_end = hooks.index("\n    };", helper_start)
helper = hooks[helper_start:helper_end]
assert "fg->FrameCount()" not in helper
assert "static_cast<uint32_t>(sourceFrame)" in helper and "sourceFrame > UINT32_MAX" in helper
assert "BeginDlssgPacing(" in helper and "sourcePacingTokenFrame=markerTokenFrame" in helper
assert "static_cast<uint64_t>(*localToken)==sourcePacingTokenFrame" in hooks
assert hooks.index("presentMutexRelease.Release();", sdk_at) < hooks.index("pacingLease->Sleep();", sdk_at)
assert hooks.index("if (directDlssSource) presentExecution.Finish();", sdk_at) < hooks.index("pacingLease->Sleep();", sdk_at)
start_assignment_at = helper.index("sourcePacingStartCalled=")
start_assignment = helper[start_assignment_at:helper.index(";", start_assignment_at) + 1]
lease_at = hooks.index("if (genshinPacing && fgPacingAfterPresent && sourcePacingStartCalled && sourcePacingEndCalled &&")
condition_at = hooks.index("(", lease_at)
condition_end, depth = condition_at + 1, 1
while depth:
    depth += (hooks[condition_end] == "(") - (hooks[condition_end] == ")")
    condition_end += 1
lease_condition = hooks[condition_at + 1:condition_end - 1]

fixture = r'''
#include <framegen/dlssg/DlssgInputContinuity.h>
#include <runtime/StarRailResidencyPolicy.h>
#include <cstdio>
#include <mutex>
#include <stdexcept>
using UINT64 = unsigned long long;
constexpr unsigned BUFFER_COUNT = 4;
unsigned checks = 0;
void Check(bool ok, const char* message) {
    ++checks; if (!ok) throw std::runtime_error(message);
}
// Execute the actual Start-result and post-SDK lease gates. The SDK token and
// marker results are fake; real token-domain lifetime is tested separately.
namespace sl { enum class Result { eOk, eErrorInvalidState }; }
struct TokenView {
    uint64_t value=41;
    operator uint64_t() const { return value; }
};
bool MarkerAllowsLease(bool markerOk, bool mutateInStart, bool endCalled, bool mutateInSdk) {
    TokenView token;
    auto* localToken=&token;
    const uint64_t markerTokenFrame=41, sourcePacingTokenFrame=41;
    const auto markerResult=markerOk ? sl::Result::eOk : sl::Result::eErrorInvalidState;
    bool sourcePacingStartCalled=false;
    if(mutateInStart) ++token.value;
    // INSERT_START_ASSIGNMENT
    if(mutateInSdk) ++token.value;
    const bool sourcePacingEndCalled=endCalled;
    const bool genshinPacing=true, fgPacingAfterPresent=true;
    const auto tokenResult=sl::Result::eOk;
    return // INSERT_LEASE_CONDITION
        ;
}
struct Provider {
    enum class StarRailPresent { Proceed, NativeFallback, Failed };
    std::recursive_mutex _inputContinuityMutex;
    dlssg::InputContinuity<BUFFER_COUNT> _inputContinuity;
    dlssg::InputOwner inputOwner{}, _runningInputOwner{}, _starRailDispatchOwner{};
    UINT64 _frameCount=41, _starRailDispatchFrame=41, _starRailDispatchGeneration=1, _lastDispatchedFrame=41;
    bool _starRailUiFailed=false, _starRailRuntimeOwner=true, _starRailDispatchPending=true;
    bool _starRailUiBusy=false, _starRailUiGap=false, _haveRunningOptions=true;
    bool exact=true, fresh=true, offOk=true, tagsOk=true, changeInOff=false, offAccepted=false;
    unsigned offCalls=0, tagCalls=0, deactivations=0;
    [[maybe_unused]] unsigned legacyGapCount=0;
    Provider() {
        inputOwner.device=0x10; inputOwner.queue=0x20; inputOwner.epoch=1;
        inputOwner.width=1920; inputOwner.height=1080;
        _runningInputOwner=_starRailDispatchOwner=inputOwner;
        Observe();
    }
    unsigned GetIndex() const { return unsigned(_frameCount % BUFFER_COUNT); }
    dlssg::InputOwner CurrentInputOwner() const { return inputOwner; }
    bool IsStarRailF5DsOwner() const { return exact; }
    bool IsStarRailContinuousOwner() const { return exact; }
    bool IsStarRailUiOwner() const { return exact; }
    bool OwnsUpscalerDepthContract() const { return false; }
    bool GuideTagsCurrent(UINT64,UINT64) const { return true; }
    void RefuseDepthContract(const char*) { _starRailUiFailed=true; }
    void Deactivate() { ++deactivations; }
    void FailStarRailUiFrame() { _starRailUiFailed=true; }
    int SelectStarRailF5DsFrame(UINT64& selected) {
        selected=_frameCount; return fresh ? int(GetIndex()) : -1;
    }
    void Observe() {
        _inputContinuity.Observe(GetIndex(),_frameCount,inputOwner,true,_inputContinuity.SourceGeneration());
        _inputContinuity.Observe(GetIndex(),_frameCount,inputOwner,false,_inputContinuity.SourceGeneration());
    }
    bool SuppressStarRailGapForPresent() {
        const auto frame=_frameCount;
        const auto snapshot=inputOwner;
        const bool ok=StarRailResidency::SuppressFgGap(offAccepted,
            [&] { return frame==_frameCount && snapshot==inputOwner; },
            [&] { ++offCalls; if(changeInOff) ++_frameCount; return offOk; },
            [&] { ++tagCalls; return tagsOk; });
        if(ok) offAccepted=true;
        return ok;
    }
    bool SuspendForInputGap() {
        _inputContinuity.Hold(_frameCount,inputOwner);
        _starRailDispatchPending=false;
        return SuppressStarRailGapForPresent();
    }
    // INSERT_GAP
    // INSERT_PREPARE
};
int main() try {
    for(unsigned faults=0;faults<16;++faults) {
        const bool markerOk=!(faults&1), mutateStart=(faults&2)!=0;
        const bool endCalled=!(faults&4), mutateSdk=(faults&8)!=0;
        Check(MarkerAllowsLease(markerOk,mutateStart,endCalled,mutateSdk)==
              (markerOk && !mutateStart && endCalled && !mutateSdk),
              "failed Start/End or reentrant token mutation cannot authorize pacing Sleep");
    }
    using P=Provider::StarRailPresent;
    Provider p;
    UINT64 frame=999;
    Check(p.PrepareStarRailPresent(false,&frame)==P::Proceed && frame==0,
          "early NR admission cannot mint a dispatch pacing receipt");
    Check(p.PrepareStarRailPresent(true,&frame)==P::Proceed && frame==41,
          "final admission returns the exact successful Dispatch source");
    Check(!p._starRailDispatchPending, "one successful Dispatch is consumed once");
    p.fresh=false;
    for(unsigned i=0;i<5;++i) {
        frame=999;
        Check(p.PrepareStarRailPresent(true,&frame)==P::NativeFallback && frame==0,
              "missing source cannot inherit an old pacing frame");
        Check(p.offCalls==1 && p.tagCalls==i+1,
              "first missing source must send Off; every original Present must clear tags");
    }
    Check(p._inputContinuity.Held() && p._haveRunningOptions && !p.deactivations,
          "input gap retains provider allocations without authorizing generation");
    p._frameCount=42; p.Observe();
    p._starRailDispatchFrame=42; p._lastDispatchedFrame=42;
    p._starRailDispatchPending=true; p._inputContinuity.Resume(); // successful fresh Dispatch
    p.offAccepted=false;
    Check(p.PrepareStarRailPresent(true,&frame)==P::Proceed && frame==42,
          "fresh successful Dispatch restores pacing on its own source");
    p._starRailDispatchPending=true;
    p._starRailDispatchGeneration=0;
    Check(p.PrepareStarRailPresent(true,&frame)==P::NativeFallback && frame==0,
          "a stale source generation cannot mint a pacing receipt");
    for(unsigned fault=0;fault<5;++fault) {
        Provider q; q.fresh=false; q._starRailDispatchPending=false;
        if(fault==0) q.offOk=false;
        if(fault==1) q.tagsOk=false;
        if(fault==2) ++q.inputOwner.epoch;
        if(fault==3) q.changeInOff=true;
        if(fault==4) q.exact=false;
        frame=999;
        Check(q.PrepareStarRailPresent(true,&frame)==P::Failed && frame==0,
              "Off/tag failure or changed owner/source must veto SDK Present");
        Check(!q.offAccepted, "failed suppression cannot publish an Off acknowledgement");
    }
    Provider busy; busy._starRailUiBusy=true;
    Check(busy.PrepareStarRailPresent(true,&frame)==P::NativeFallback && frame==0 && busy.offCalls==1,
          "busy source also suppresses its first unpaired SDK Present");
    std::printf("PASS %u StarRail real-admission CPU checks; marker/SDK/unlock wiring passed\n",checks);
    return 0;
} catch(const std::exception& e) {
    std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1;
}
'''.replace("// INSERT_GAP", gap).replace("// INSERT_PREPARE", prepare).replace(
    "// INSERT_START_ASSIGNMENT", start_assignment).replace("// INSERT_LEASE_CONDITION", lease_condition)
with tempfile.TemporaryDirectory(prefix=".starrail-present-", dir=HERE, ignore_cleanup_errors=True) as temporary:
    directory=Path(temporary)
    cpp=directory/"fixture.cpp"
    cpp.write_text(fixture,encoding="utf-8")
    exe=directory/"fixture.exe"
    batch=directory/"compile.cmd"
    batch.write_text(
        '@echo off\ncall "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64\n'
        'if errorlevel 1 exit /b %errorlevel%\n'
        f'cl /nologo /std:c++20 /EHsc /W4 /WX /Od /I"{ROOT / "OptiScaler"}" /Fe:"{exe}" /Fo:"{directory}/" "{cpp}"\n'
        'if errorlevel 1 exit /b %errorlevel%\n'
        f'"{exe}"\nexit /b %errorlevel%\n',encoding="utf-8",newline="\r\n")
    completed=subprocess.run(["cmd.exe","/d","/c",str(batch)],cwd=directory)
raise SystemExit(completed.returncode)
