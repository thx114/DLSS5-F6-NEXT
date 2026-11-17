"""Execute the production DX11 Release body with controlled owner/vendor seams.

The real bridge fence/recording drain is covered by BridgeRetirementWarp. This
fixture checks that the API keeps its owner on refusal and has no implicit delay.
"""
import argparse
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--negative-skip-retirement", action="store_true")
args = parser.parse_args()
source = (ROOT / "OptiScaler/inputs/NVNGX_DLSS_Dx11.cpp").read_text(encoding="utf-8-sig")
begin = source.index("NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_ReleaseFeature(")
opening = source.index("{", begin)
end, depth = opening + 1, 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
body = source[begin:end]
if args.negative_skip_retirement:
    body = body.replace("!deviceContext->PrepareForShutdown()", "false")

fixture = r'''
#include <map>
#include <memory>
#include <chrono>
#include <cstdio>
#include <cstdlib>
using NVSDK_NGX_Result = int;
constexpr int NVSDK_NGX_Result_Success=1, NVSDK_NGX_Result_Fail=2;
constexpr int NVSDK_NGX_Result_FAIL_FeatureNotFound=3;
constexpr unsigned DLSS_MOD_ID_OFFSET=1000000;
struct NVSDK_NGX_Handle { unsigned Id; };
int prepared=0, destroyed=0, unretired=0, nativeCalls=0, delayCalls=0;
long long delayMs=0;
int nativeResult=NVSDK_NGX_Result_Success;
bool cleanup=true, enabled=true, haveNative=true, delayed=false, shutdown=false;
NVSDK_NGX_Handle* nativeHandle=nullptr;
namespace std::this_thread {
void sleep_for(std::chrono::milliseconds value) { ++delayCalls; delayMs+=value.count(); }
}
struct Feature {
    bool canRetire=true, retired=false;
    bool PrepareForShutdown() { ++prepared; return retired=canRetire; }
    ~Feature() { ++destroyed; if(!retired) ++unretired; }
};
struct Context { std::unique_ptr<Feature> feature; };
std::map<unsigned,Context> Dx11Contexts;
struct State {
    Feature* currentFeature=nullptr;
    std::map<unsigned,bool> changeBackend;
    static State& Instance() { static State s; return s; }
};
struct Config {
    struct Flag { bool* value; bool value_or_default() const { return *value; } };
    Flag DLSSEnabled{&enabled}, Dx11DelayedInit{&delayed};
    static Config* Instance() { static Config c; return &c; }
};
struct ModuleLifetime { static bool CleanupAllowed() { return cleanup; } };
struct FeatureLifecycleTrace { FeatureLifecycleTrace(const char*, unsigned) {} };
int NativeRelease(NVSDK_NGX_Handle* handle) { ++nativeCalls; nativeHandle=handle; return nativeResult; }
struct NVNGXProxy {
    static auto D3D11_ReleaseFeature() -> int(*)(NVSDK_NGX_Handle*) { return haveNative?NativeRelease:nullptr; }
};
#define NVSDK_NGX_API
#define LOG_ERROR(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_TRACE(...) ((void)0)
BODY
unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)) { std::printf("FAIL %u: %s\n",checks,#x); return 1; } } while(0)
void Fresh() {
    for(auto& [id,c]:Dx11Contexts) if(c.feature)c.feature->retired=true;
    Dx11Contexts.clear(); State::Instance()={};
    prepared=destroyed=unretired=nativeCalls=delayCalls=0; delayMs=0;
    cleanup=enabled=haveNative=true; delayed=shutdown=false;
    nativeResult=NVSDK_NGX_Result_Success; nativeHandle=nullptr;
}
Feature* Add(unsigned id, bool canRetire=true) {
    auto p=std::make_unique<Feature>(); p->canRetire=canRetire;
    auto raw=p.get(); Dx11Contexts[id].feature=std::move(p);
    State::Instance().changeBackend[id]=true; return raw;
}
int main() {
    NVSDK_NGX_Handle handle{DLSS_MOD_ID_OFFSET};
    Fresh(); CHECK(NVSDK_NGX_D3D11_ReleaseFeature(nullptr)==NVSDK_NGX_Result_Success);
    CHECK(!prepared && !destroyed && !nativeCalls && !delayCalls);
    cleanup=false; State::Instance().currentFeature=Add(handle.Id);
    CHECK(NVSDK_NGX_D3D11_ReleaseFeature(&handle)==NVSDK_NGX_Result_Fail);
    CHECK(!prepared && !destroyed && Dx11Contexts.count(handle.Id)==1);

    Fresh(); auto held=Add(handle.Id,false); State::Instance().currentFeature=held;
    CHECK(NVSDK_NGX_D3D11_ReleaseFeature(&handle)==NVSDK_NGX_Result_Fail);
    CHECK(prepared==1 && !destroyed && !unretired && !delayCalls);
    CHECK(Dx11Contexts.at(handle.Id).feature.get()==held && State::Instance().currentFeature==held);
    CHECK(State::Instance().changeBackend.count(handle.Id)==1);
    held->canRetire=true;
    CHECK(NVSDK_NGX_D3D11_ReleaseFeature(&handle)==NVSDK_NGX_Result_Success);
    CHECK(prepared==2 && destroyed==1 && !unretired && !delayCalls && Dx11Contexts.empty());
    CHECK(!State::Instance().currentFeature && State::Instance().changeBackend.empty());
    CHECK(NVSDK_NGX_D3D11_ReleaseFeature(&handle)==NVSDK_NGX_Result_Success && destroyed==1);

    Fresh(); auto other=Add(handle.Id+1); Add(handle.Id); State::Instance().currentFeature=other;
    CHECK(NVSDK_NGX_D3D11_ReleaseFeature(&handle)==NVSDK_NGX_Result_Success);
    CHECK(State::Instance().currentFeature==other && Dx11Contexts.size()==1 && !unretired);
    Fresh(); Dx11Contexts[handle.Id]={}; State::Instance().changeBackend[handle.Id]=true;
    CHECK(NVSDK_NGX_D3D11_ReleaseFeature(&handle)==NVSDK_NGX_Result_Success);
    CHECK(Dx11Contexts.empty() && State::Instance().changeBackend.empty() && !prepared);

    Fresh(); delayed=true; Add(handle.Id);
    CHECK(NVSDK_NGX_D3D11_ReleaseFeature(&handle)==NVSDK_NGX_Result_Success);
    CHECK(delayCalls==1 && delayMs==500 && destroyed==1 && !unretired);
    Fresh(); delayed=true; shutdown=true; Add(handle.Id);
    CHECK(NVSDK_NGX_D3D11_ReleaseFeature(&handle)==NVSDK_NGX_Result_Success && !delayCalls && !unretired);

    Fresh(); NVSDK_NGX_Handle vendor{17}; nativeResult=7;
    CHECK(NVSDK_NGX_D3D11_ReleaseFeature(&vendor)==7 && nativeCalls==1 && nativeHandle==&vendor);
    CHECK(!prepared && !destroyed && !delayCalls);
    haveNative=false;
    CHECK(NVSDK_NGX_D3D11_ReleaseFeature(&vendor)==NVSDK_NGX_Result_FAIL_FeatureNotFound && nativeCalls==1);
    haveNative=true; enabled=false;
    CHECK(NVSDK_NGX_D3D11_ReleaseFeature(&vendor)==NVSDK_NGX_Result_FAIL_FeatureNotFound && nativeCalls==1);

    Fresh();
    for(unsigned i=0;i<45;++i) {
        handle.Id=DLSS_MOD_ID_OFFSET+i; State::Instance().currentFeature=Add(handle.Id);
        CHECK(NVSDK_NGX_D3D11_ReleaseFeature(&handle)==NVSDK_NGX_Result_Success);
    }
    CHECK(prepared==45 && destroyed==45 && !unretired && !delayCalls && !delayMs);
    CHECK(Dx11Contexts.empty() && State::Instance().changeBackend.empty() && !State::Instance().currentFeature);
    std::printf("PASS %u production DX11 release checks; 45 retirements, zero implicit delays; no GPU\n",checks);
}
'''.replace("BODY", body)
out = ROOT / "build/common-core-tests/Dx11ReleaseRetirement"
if args.negative_skip_retirement:
    out /= "negative"
out.mkdir(parents=True, exist_ok=True)
cpp, exe = out / "fixture.cpp", out / "fixture.exe"
cpp.write_text(fixture, encoding="utf-8")
devcmd = "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat"
batch = out / "build.cmd"
batch.write_text(f'@echo off\ncall "{devcmd}" -arch=x64 -host_arch=x64 >nul\n'
                 'if errorlevel 1 exit /b %errorlevel%\n'
                 f'cl.exe /nologo /std:c++20 /EHsc /W4 /Fe:"{exe}" "{cpp}"\n'
                 'exit /b %errorlevel%\n', encoding="utf-8")
subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=out, check=True)
raise SystemExit(subprocess.run([str(exe)], cwd=out).returncode)
