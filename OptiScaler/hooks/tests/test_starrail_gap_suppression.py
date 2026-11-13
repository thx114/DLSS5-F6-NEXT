"""Execute the actual provider suppression/ACK invalidation bodies with fake SDK calls.

No game, graphics device or NVIDIA SDK library is loaded. The negative control
removes exception cleanup from the extracted body and must fail the same checks.
"""
import argparse
import importlib.util
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--negative-no-cleanup", action="store_true")
args = parser.parse_args()
spec = importlib.util.spec_from_file_location(
    "production_extract", ROOT / "OptiScaler/dlssnr/tests/nr-reconfiguration/prepare.py")
extractor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extractor)
source = (ROOT / "OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp").read_text(encoding="utf-8-sig")
suppression = extractor.extract(source, "bool DLSSG_Dx12::SuppressStarRailGapForPresent(")
if args.negative_no_cleanup:
    suppression = extractor.replace_once(suppression,
        "if (!completed) provider.InvalidateDispatchOptions();",
        "/* negative control: exception/failure leaves acknowledgements intact */")
bodies = suppression + "\n" + "\n".join(extractor.extract(source, signature) for signature in (
    "void DLSSG_Dx12::InvalidateDispatchOptions(",
    "void DLSSG_Dx12::InvalidateDlssgOptions("))

fixture = r'''
#include <framegen/dlssg/DlssgOptionsPolicy.h>
#include <runtime/StarRailResidencyPolicy.h>
#include <atomic>
#include <cstdio>
#include <functional>
#include <stdexcept>
using UINT64 = unsigned long long;
unsigned checks=0, scenarios=0;
void Check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
#define LOG_INFO(...) ((void)0)
std::atomic<bool> g_dlssgTeardownQuarantined=false;
namespace CallerCpuTrace {
enum class Op { FgDlssgOptions };
struct Scope {
    template<class... T> Scope(T&&...){}
    void Finish(uint32_t,bool){}
};
}
struct State {
    unsigned dlssgDetectedInterpolationCount=5;
    static State& Instance(){static State state;return state;}
};
namespace ReflexHooks {
unsigned frameCount=5;
void setDlssgFrameCount(unsigned count){frameCount=count;}
}
struct Token : sl::FrameToken {
    uint32_t value=41;
    operator uint32_t() const override{return value;}
};
namespace StreamlineProxy {
using OffFn=sl::Result(*)(const sl::ViewportHandle&,const sl::DLSSGOptions&);
using TokenFn=sl::Result(*)(sl::FrameToken*&,uint32_t*);
using TagFn=sl::Result(*)(const sl::FrameToken&,const sl::ViewportHandle&,
    const sl::ResourceTag*,unsigned,void*);
OffFn off=nullptr;TokenFn token=nullptr;TagFn tags=nullptr;
OffFn DLSSGSetOptions(){return off;}
TokenFn GetNewFrameToken(){return token;}
TagFn SetTagForFrame(){return tags;}
}
namespace StreamlineHooks {
sl::Result SubmitDlssgOptions(StreamlineProxy::OffFn call,const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& options){return call(viewport,options);}
}
struct DLSSG_Dx12 {
    dlssg::InputContinuity<4> _inputContinuity;
    dlssg::InputOwner inputOwner{},_runningInputOwner{};
    dlssg::options::Scope scope{};
    dlssg::options::ConfigurationState _dispatchOptions;
    dlssg::options::Acknowledgement<sl::DLSSGOptions> _dlssgOptionsAck;
    dlssg::options::Acknowledgement<sl::ReflexOptions> _reflexOptionsAck;
    dlssg::options::GenerationState _generationState;
    sl::DLSSGOptions _lastRunningOptions{};
    sl::ReflexOptions reflex{};
    sl::ViewportHandle viewport{0};
    UINT64 _frameCount=41,_providerOptionsLifecycle=1;
    unsigned _framesToInterpolate=5;
    bool _starRailGapSuppressing=false,_haveRunningOptions=true,_starRailGapOffAccepted=false;
    bool _starRailDispatchPending=true,_starRailUiGap=false,_starRailUiFailed=false;
    bool _hookRetiring=false,_ownerChanging=false,_teardownPending=false;
    bool _teardownCompleting=false,_teardownQuarantined=false,continuous=true;
    bool IsStarRailContinuousOwner() const{return continuous;}
    dlssg::InputOwner CurrentInputOwner() const{return inputOwner;}
    bool ForceStarRailFgResident() const{return true;}
    void InvalidateDispatchOptions();
    void InvalidateDlssgOptions();
    bool SuppressStarRailGapForPresent();
    DLSSG_Dx12(){
        inputOwner.device=0x10;inputOwner.queue=0x20;inputOwner.epoch=1;
        inputOwner.width=1920;inputOwner.height=1080;_runningInputOwner=inputOwner;
        _inputContinuity.Hold(_frameCount,inputOwner);
        scope.provider={1,1,0x10,0x20,0x30,0x40,0x50,0x60,0x70,0,1920,1080};
        scope.input=inputOwner;scope.sourceGeneration=1;
        scope.reflexPlugin=0x80;scope.reflexSetOptions=0x90;scope.enabled=true;
        _lastRunningOptions.mode=sl::DLSSGMode::eOn;
        _lastRunningOptions.numFramesToGenerate=5;
        reflex.mode=sl::ReflexMode::eLowLatency;
        Check(Apply(_dlssgOptionsAck,_lastRunningOptions).confirmed,"seed real FG ACK");
        Check(Apply(_reflexOptionsAck,reflex).confirmed,"seed real Reflex ACK");
        _generationState.acceptedKnown=true;
        _generationState.acceptedGenerated=5;
    }
    template<class T> dlssg::options::Submission Apply(dlssg::options::Acknowledgement<T>& ack,const T& options){
        return _dispatchOptions.Apply(ack,scope,options,_frameCount,true,false,
            []{return true;},[]{return sl::Result::eOk;});
    }
};
// INSERT_PRODUCTION
enum class Fault {
    None,OffFalse,OffThrow,TokenFalse,TokenThrow,TokenNull,TokenWrong,TagsFalse,TagsThrow,
    OffMissing,TokenMissing,TagsMissing,OwnerInOff,FrameInToken,LifecycleInTags,Reentrant
};
struct Calls {
    DLSSG_Dx12& provider;Fault fault;
    unsigned off=0,tokens=0,tags=0;
    Token token;
    static Calls* active;
    explicit Calls(DLSSG_Dx12& p,Fault f=Fault::None):provider(p),fault(f){
        active=this;StreamlineProxy::off=f==Fault::OffMissing?nullptr:Off;
        StreamlineProxy::token=f==Fault::TokenMissing?nullptr:GetToken;
        StreamlineProxy::tags=f==Fault::TagsMissing?nullptr:Tags;
    }
    ~Calls(){active=nullptr;StreamlineProxy::off=nullptr;StreamlineProxy::token=nullptr;StreamlineProxy::tags=nullptr;}
    static sl::Result Off(const sl::ViewportHandle&,const sl::DLSSGOptions& options){
        auto& c=*active;++c.off;
        Check(options.mode==sl::DLSSGMode::eOff,"gap sends actual Off payload");
        Check((uint32_t(options.flags)&uint32_t(sl::DLSSGFlags::eRetainResourcesWhenOff))!=0,"gap retains allocations");
        if(c.fault==Fault::OffThrow)throw std::runtime_error("sdk-off");
        if(c.fault==Fault::OwnerInOff)++c.provider.inputOwner.epoch;
        if(c.fault==Fault::Reentrant)
            Check(!c.provider.SuppressStarRailGapForPresent(),"reentrant suppression cannot issue nested SDK calls");
        return c.fault==Fault::OffFalse?sl::Result::eErrorInvalidState:sl::Result::eOk;
    }
    static sl::Result GetToken(sl::FrameToken*& token,uint32_t* frame){
        auto& c=*active;++c.tokens;
        if(c.fault==Fault::TokenThrow)throw std::runtime_error("sdk-token");
        c.token.value=*frame+(c.fault==Fault::TokenWrong?1u:0u);
        token=c.fault==Fault::TokenNull?nullptr:&c.token;
        if(c.fault==Fault::FrameInToken)++c.provider._frameCount;
        return c.fault==Fault::TokenFalse?sl::Result::eErrorInvalidState:sl::Result::eOk;
    }
    static sl::Result Tags(const sl::FrameToken& token,const sl::ViewportHandle&,
        const sl::ResourceTag* tags,unsigned count,void* list){
        auto& c=*active;++c.tags;
        Check(count==4&&!list&&uint32_t(token)==c.provider._frameCount,"null tags belong to observed source");
        const sl::BufferType types[]{sl::kBufferTypeDepth,sl::kBufferTypeMotionVectors,
            sl::kBufferTypeHUDLessColor,sl::kBufferTypeUIColorAndAlpha};
        for(unsigned i=0;i<count;++i)Check(!tags[i].resource&&tags[i].type==types[i],"every guide tag cleared");
        if(c.fault==Fault::TagsThrow)throw std::runtime_error("sdk-tags");
        if(c.fault==Fault::LifecycleInTags)++c.provider._providerOptionsLifecycle;
        return c.fault==Fault::TagsFalse?sl::Result::eErrorInvalidState:sl::Result::eOk;
    }
};
Calls* Calls::active=nullptr;
void Success(){
    ++scenarios;DLSSG_Dx12 p;Calls calls(p);
    Check(p.SuppressStarRailGapForPresent(),"first retained gap succeeds");
    Check(!p._starRailGapSuppressing&&p._starRailGapOffAccepted,"successful suppression publishes Off and clears guard");
    Check(!p._dlssgOptionsAck.valid&&p._reflexOptionsAck.valid,"successful gap only invalidates FG ACK");
    Check(!p._generationState.acceptedKnown&&!p._framesToInterpolate,"gap never authorizes generated frames");
    Check(!p._starRailDispatchPending&&ReflexHooks::frameCount==0,"gap clears pending dispatch and reported generation");
    for(unsigned i=0;i<3;++i)Check(p.SuppressStarRailGapForPresent(),"cached Off still clears this Present's tags");
    Check(calls.off==1&&calls.tokens==4&&calls.tags==4,"one Off but per-Present token and null tags");
    Check(!p.Apply(p._reflexOptionsAck,p.reflex).submitted,"successful gap preserves real Reflex ACK");
    Check(p.Apply(p._dlssgOptionsAck,p._lastRunningOptions).submitted,"fresh On must really submit after gap");
}
void Failure(Fault fault,bool cached){
    ++scenarios;DLSSG_Dx12 p;Calls calls(p,fault);p._starRailGapOffAccepted=cached;
    bool result=false,threw=false;
    try{result=p.SuppressStarRailGapForPresent();}catch(const std::runtime_error&){threw=true;}
    const bool throwing=fault==Fault::OffThrow||fault==Fault::TokenThrow||fault==Fault::TagsThrow;
    Check(!result&&threw==throwing,"SDK rejection/exception cannot complete suppression");
    Check(!p._starRailGapSuppressing,"failure/exception releases reentrancy guard");
    Check(!p._dlssgOptionsAck.valid&&!p._reflexOptionsAck.valid,"failure/exception revokes both configuration ACKs");
    Check(!p._generationState.acceptedKnown&&!p._framesToInterpolate,"failed suppression revokes applied generation");
    if(!cached)Check(!p._starRailGapOffAccepted,"partial Off does not publish cached success");
    else Check(calls.off==0,"cached-Off failure must not invent another Off");
    Check(p.Apply(p._reflexOptionsAck,p.reflex).submitted,"uncertain Reflex state requires actual resubmission");
    Check(p.Apply(p._dlssgOptionsAck,p._lastRunningOptions).submitted,"uncertain FG state requires actual resubmission");
}
int main() try{
    Success();
    for(auto fault:{Fault::OffFalse,Fault::OffThrow,Fault::TokenFalse,Fault::TokenThrow,Fault::TokenNull,
        Fault::TokenWrong,Fault::TagsFalse,Fault::TagsThrow,Fault::OffMissing,Fault::TokenMissing,
        Fault::TagsMissing,Fault::OwnerInOff,Fault::FrameInToken,Fault::LifecycleInTags})Failure(fault,false);
    for(auto fault:{Fault::TokenFalse,Fault::TokenThrow,Fault::TokenNull,Fault::TokenWrong,Fault::TagsFalse,
        Fault::TagsThrow,Fault::TokenMissing,Fault::TagsMissing,Fault::FrameInToken,Fault::LifecycleInTags})Failure(fault,true);
    {++scenarios;DLSSG_Dx12 p;Calls calls(p,Fault::Reentrant);
        Check(p.SuppressStarRailGapForPresent()&&calls.off==1,"outer transaction survives rejected reentrant call");}
    for(UINT64 frame:{UINT64(0),UINT64(UINT32_MAX)+1}){++scenarios;DLSSG_Dx12 p;Calls calls(p);
        p._frameCount=frame;p._inputContinuity.Hold(frame,p.inputOwner);
        Check(!p.SuppressStarRailGapForPresent()&&!calls.tokens,"invalid SDK source never truncates into a token");
        Check(!p._reflexOptionsAck.valid&&!p._starRailGapSuppressing,"invalid source clears ACK and guard");}
    std::printf("PASS %u actual suppression scenarios, %u CPU checks; fake SDK boundaries; no game/GPU claims\n",scenarios,checks);
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 17;}
'''.replace("// INSERT_PRODUCTION", bodies)
directory = ROOT / "build/common-core-tests/StarRailGapSuppression"
directory.mkdir(parents=True, exist_ok=True)
variant = "negative-no-cleanup" if args.negative_no_cleanup else "production"
cpp, exe = directory / (variant + ".cpp"), directory / (variant + ".exe")
cpp.write_text(fixture, encoding="utf-8")
batch = directory / (variant + ".cmd")
batch.write_text(
    '@echo off\ncall "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64\n'
    'if errorlevel 1 exit /b %errorlevel%\n'
    f'cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /Od /I"{ROOT / "OptiScaler"}" '
    f'/I"{ROOT / "external/streamline"}" /Fe:"{exe}" /Fo:"{directory / variant}.obj" "{cpp}"\n'
    'if errorlevel 1 exit /b %errorlevel%\n'
    f'"{exe}"\nexit /b %errorlevel%\n', encoding="utf-8", newline="\r\n")
raise SystemExit(subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=directory).returncode)
