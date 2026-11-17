"""Execute the real DX11 Shutdown body with controlled feature/vendor seams.

No game, ASI, D3D runtime or GPU is loaded. The interop override is also checked
to keep the real queue/fence drain; the mock cannot prove GPU completion.
"""
import argparse
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--negative-skip-retirement", action="store_true")
args = parser.parse_args()
source = (ROOT / "OptiScaler/inputs/NVNGX_DLSS_Dx11.cpp").read_text(encoding="utf-8-sig")
begin = source.index("static NVSDK_NGX_Result ShutdownDx11Body(")
opening = source.index("{", begin)
end, depth = opening + 1, 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
body = source[begin:end]
assert "NVSDK_NGX_D3D11_ReleaseFeature(" not in body, "Shutdown must bypass external Release detours"
assert "Dx11Contexts.clear()" not in body, "Shutdown must keep unretired owners"
bridge = (ROOT / "OptiScaler/upscalers/IFeature_Dx11wDx12.h").read_text(encoding="utf-8-sig")
assert "bool PrepareForShutdown() override { return !initRetention && ReleaseSharedResources(true); }" in bridge
assert body.index("PrepareForShutdown()") < body.index("Dx11Contexts.erase") < body.index("NVNGXProxy::D3D11_Shutdown()")
for name, call in [("Shutdown", "ShutdownDx11Body(shutdownDx11Request.device, shutdownDx11Request.deviceSpecific)"),
                   ("Shutdown1", "return shutdownEntry()")]:
    entry = source[source.index(f"NVSDK_NGX_Result NVSDK_NGX_D3D11_{name}("):]
    entry = entry[:entry.index("\n}")]
    assert entry.index("ModuleLifetime::CleanupAllowed()") < entry.index(call)
assert "auto* volatile shutdownEntry = &NVSDK_NGX_D3D11_Shutdown;" in source
assert "std::exchange(shutdownDx11Request, ShutdownDx11Request { InDevice, true })" in source
assert "~RestoreRequest() { shutdownDx11Request = previous; }" in source
if args.negative_skip_retirement:
    body = body.replace("feature && !feature->PrepareForShutdown()", "false")

fixture = r'''
#include <map>
#include <memory>
#include <cstdio>
#include <cstdlib>
using NVSDK_NGX_Result = int;
constexpr int NVSDK_NGX_Result_Success=1, NVSDK_NGX_Result_Fail=2;
struct ID3D11Device {} device;
int prepared=0, destroyed=0, unretiredDestruction=0, stops=0, stops1=0, resets=0;
int stopResult=NVSDK_NGX_Result_Success;
bool initialized=true, enabled=true, haveStop=true, haveStop1=true;
ID3D11Device* stoppedDevice=nullptr;
struct Feature {
    bool canRetire=true, retired=false;
    bool PrepareForShutdown() { ++prepared; return retired=canRetire; }
    ~Feature() { ++destroyed; if (!retired) ++unretiredDestruction; }
};
struct Context { std::unique_ptr<Feature> feature; };
std::map<unsigned,Context> Dx11Contexts;
ID3D11Device* D3D11Device=&device;
ID3D11Device* taaAttachedDevice=nullptr;
bool shutdown=false;
struct State {
    Feature* currentFeature=nullptr;
    std::map<unsigned,bool> changeBackend;
    bool nvngxDx11Inited=true;
    static State& Instance() { static State s; return s; }
};
struct Config {
    struct Flag { bool value_or_default() { return enabled; } } DLSSEnabled;
    static Config* Instance() { static Config c; return &c; }
};
int Stop() { ++stops; return stopResult; }
int Stop1(ID3D11Device* d) { ++stops1; stoppedDevice=d; return stopResult; }
struct NVNGXProxy {
    static bool IsDx11Inited() { return initialized; }
    static void SetDx11Inited(bool b) { initialized=b; }
    static auto D3D11_Shutdown() -> int(*)() { return haveStop?Stop:nullptr; }
    static auto D3D11_Shutdown1() -> int(*)(ID3D11Device*) { return haveStop1?Stop1:nullptr; }
};
struct Dx11WithDx12 {
    static void ResetUpscalerResourceCache(bool sync) { if (!sync) std::abort(); ++resets; }
};
#define LOG_ERROR(...) ((void)0)
BODY
unsigned checks=0;
#define CHECK(x) do { ++checks; if (!(x)) { std::printf("FAIL check %u: %s\n", checks, #x); return 1; } } while(0)
void Fresh() {
    // Only the fixture releases its retained mock owner between scenarios.
    for (auto& [id,c]:Dx11Contexts) if(c.feature)c.feature->retired=true;
    Dx11Contexts.clear(); State::Instance()={};
    prepared=destroyed=unretiredDestruction=stops=stops1=resets=0;
    initialized=enabled=haveStop=haveStop1=true;
    stopResult=NVSDK_NGX_Result_Success; stoppedDevice=nullptr;
    D3D11Device=&device; shutdown=false;
}
Feature* Add(unsigned id, bool canRetire=true) {
    auto p=std::make_unique<Feature>(); p->canRetire=canRetire;
    auto raw=p.get(); Dx11Contexts[id].feature=std::move(p);
    State::Instance().changeBackend[id]=true; return raw;
}
int main() {
    Fresh(); State::Instance().currentFeature=Add(11); Add(22);
    CHECK(ShutdownDx11Body()==NVSDK_NGX_Result_Success);
    CHECK(Dx11Contexts.empty() && prepared==2 && destroyed==2 && !unretiredDestruction);
    CHECK(stops==1 && !stops1 && resets==1 && !initialized && !D3D11Device);
    CHECK(!State::Instance().currentFeature && State::Instance().changeBackend.empty());
    CHECK(!State::Instance().nvngxDx11Inited && !shutdown);
    CHECK(ShutdownDx11Body()==NVSDK_NGX_Result_Success && stops==1 && destroyed==2);

    Fresh(); auto held=Add(10,false); State::Instance().currentFeature=held;
    CHECK(ShutdownDx11Body()==NVSDK_NGX_Result_Fail);
    CHECK(Dx11Contexts.size()==1 && State::Instance().currentFeature==held);
    CHECK(!destroyed && prepared==1 && !stops && !resets && initialized && D3D11Device==&device);
    CHECK(State::Instance().changeBackend.count(10)==1 && State::Instance().nvngxDx11Inited);
    held->canRetire=true;
    CHECK(ShutdownDx11Body()==NVSDK_NGX_Result_Success);
    CHECK(Dx11Contexts.empty() && destroyed==1 && !unretiredDestruction && stops==1);

    Fresh(); Add(1); held=Add(2,false); State::Instance().currentFeature=held;
    CHECK(ShutdownDx11Body()==NVSDK_NGX_Result_Fail);
    CHECK(Dx11Contexts.size()==1 && Dx11Contexts.count(2) && destroyed==1 && !unretiredDestruction);
    CHECK(State::Instance().currentFeature==held && !stops && !resets);

    Fresh(); State::Instance().currentFeature=Add(3);
    CHECK(ShutdownDx11Body(&device,true)==NVSDK_NGX_Result_Success);
    CHECK(!stops && stops1==1 && stoppedDevice==&device && destroyed==1 && !unretiredDestruction);
    Fresh(); Add(4); haveStop1=false;
    CHECK(ShutdownDx11Body(&device,true)==NVSDK_NGX_Result_Success && stops==1 && !stops1);
    Fresh(); Add(5); stopResult=7;
    CHECK(ShutdownDx11Body()==7 && initialized && !resets && !shutdown);
    CHECK(Dx11Contexts.empty() && destroyed==1 && !unretiredDestruction);
    stopResult=NVSDK_NGX_Result_Success;
    CHECK(ShutdownDx11Body()==NVSDK_NGX_Result_Success && !initialized && resets==1);
    for(int i=0;i<8;++i) {
        Fresh(); Add(6); enabled=(i&1)==0; initialized=(i&2)==0; haveStop=(i&4)==0;
        const bool shouldStop=enabled && initialized && haveStop;
        CHECK(ShutdownDx11Body()==NVSDK_NGX_Result_Success);
        CHECK(destroyed==1 && !unretiredDestruction && stops==int(shouldStop) && resets==1);
    }
    Fresh(); Dx11Contexts[9]={}; State::Instance().changeBackend[9]=true;
    CHECK(ShutdownDx11Body()==NVSDK_NGX_Result_Success && Dx11Contexts.empty());
    CHECK(!prepared && !destroyed && State::Instance().changeBackend.empty());
    std::printf("PASS %u actual DX11 shutdown body checks; controlled owners/vendors, no GPU\n",checks);
}
'''.replace("BODY", body)
out = ROOT / "build/starrail-asi-20261113/shutdown-test"
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
