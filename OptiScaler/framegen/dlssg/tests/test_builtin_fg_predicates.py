"""Execute extracted built-in FG eligibility/wait predicates; no provider patching or SDK execution."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[4]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--negative', action='store_true')
args = parser.parse_args()
source = (ROOT / 'OptiScaler/framegen/dlssg/MfgUnlock.cpp').read_text(encoding='utf-8-sig')

def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

pending = function('bool MfgUnlock::Pending()')
if args.negative:
    needle = ' && !LastStatus().ModuleFound'
    assert pending.count(needle) == 1
    pending = pending.replace(needle, '')
loader = (ROOT / 'OptiScaler/hooks/LibraryLoad_Hooks.cpp').read_text(encoding='utf-8-sig')
assert 'IsProviderPath(normalizedPath) && MfgUnlock::ShouldInterceptProviderLoad()' in loader
assert 'IsProviderPath(normalizedPath) && MfgUnlock::Pending()' not in loader
menu = (ROOT / 'OptiScaler/menu/menu_common.cpp').read_text(encoding='utf-8-sig')
start = menu.index('if (config->FGDLSSGAmpereMfgUnlock.value_or_default())')
stop = menu.index('bool builtinMfg', start)
legacy = menu[start:stop]
assert 'MenuText::Button(' in legacy and 'config->FGDLSSGAmpereMfgUnlock = false;' in legacy
assert legacy.index('MenuText::Button(') < legacy.index('config->FGDLSSGAmpereMfgUnlock = false;')
fixture = r'''
#include <cstdio>
#include <stdexcept>
struct Setting { bool value=false; bool value_or_default() const { return value; } };
struct Config {
    Setting FGDLSSGAdaMfgUnlock, FGDLSSGAmpereMfgUnlock;
    static Config* Instance() { static Config config; return &config; }
};
struct State { bool externalFrameGeneration=false; static State& Instance() { static State state; return state; } };
enum class VendorId { Nvidia, Other };
constexpr unsigned NV_GPU_ARCHITECTURE_AD100=0x190;
struct Gpu { VendorId vendorId=VendorId::Nvidia; struct { unsigned architecture_id=NV_GPU_ARCHITECTURE_AD100; } nvidiaArchInfo; };
namespace IdentifyGpu { Gpu gpu; const Gpu& getPrimaryGpu() { return gpu; } }
namespace MfgUnlock {
struct Status { bool ModuleFound=false, AdvertiseMatched=false, ValidateMatched=false; unsigned KernelsRewritten=0; };
Status fixtureStatus;
Status LastStatus() { return fixtureStatus; }
bool Pending(); bool ShouldInterceptProviderLoad(); unsigned UnlockedMax();
}
constexpr unsigned kMaxGeneratedFrames=5;
'''
fixture += function('bool AdaUnlockWanted()') + '\n'
fixture += function('bool MfgUnlock::ShouldInterceptProviderLoad()') + '\n' + pending + '\n'
fixture += function('unsigned int MfgUnlock::UnlockedMax()') + '\n'
fixture += r'''
unsigned checks=0;
void Check(bool ok,const char* message) { ++checks; if(!ok) throw std::runtime_error(message); }
int main() {
try {
    auto& cfg=*Config::Instance();
    cfg.FGDLSSGAdaMfgUnlock.value=true;
    Check(MfgUnlock::Pending(),"provider discovery remains pending before first attempt");
    Check(MfgUnlock::ShouldInterceptProviderLoad(),"eligible provider loads intercepted");
    MfgUnlock::fixtureStatus.ModuleFound=true;
    Check(!MfgUnlock::Pending(),"completed provider attempt ends capability wait");
    Check(MfgUnlock::ShouldInterceptProviderLoad(),"later provider loads remain intercepted");
    Check(MfgUnlock::UnlockedMax()==0,"unknown signatures cannot claim unlocked cap");
    for(unsigned mask=0;mask<8;++mask) {
        MfgUnlock::fixtureStatus.AdvertiseMatched=(mask&1)!=0;
        MfgUnlock::fixtureStatus.ValidateMatched=(mask&2)!=0;
        MfgUnlock::fixtureStatus.KernelsRewritten=(mask&4)!=0;
        Check(!MfgUnlock::Pending(),"attempt outcome cannot reintroduce endless capability wait");
        Check(MfgUnlock::UnlockedMax()==(mask==7 ? 5u : 0u),"cap requires both gates and compatible kernels");
    }
    for(bool found:{false,true}) {
        MfgUnlock::fixtureStatus.ModuleFound=found;
        for(unsigned guard=0;guard<5;++guard) {
            cfg.FGDLSSGAdaMfgUnlock.value=guard!=0;
            cfg.FGDLSSGAmpereMfgUnlock.value=guard==1;
            State::Instance().externalFrameGeneration=guard==2;
            IdentifyGpu::gpu.vendorId=guard==3 ? VendorId::Other : VendorId::Nvidia;
            IdentifyGpu::gpu.nvidiaArchInfo.architecture_id=guard==4 ? 0x1b0 : NV_GPU_ARCHITECTURE_AD100;
            Check(!MfgUnlock::Pending(),"ineligible config or architecture must not wait");
            Check(!MfgUnlock::ShouldInterceptProviderLoad(),"ineligible config or architecture must not intercept");
        }
    }
    std::printf("PASS %u extracted builtin FG predicate checks; menu/loader wiring checked; no provider/SDK/game execution\n",checks);
    return 0;
} catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL builtin FG predicate: %s (check %u)\n",error.what(),checks);
    return 1;
}
}
'''
out = ROOT / 'build' / ('builtin-fg-predicates-negative' if args.negative else 'builtin-fg-predicates')
out.mkdir(parents=True, exist_ok=True)
cpp = out / 'fixture.cpp'
cpp.write_text(fixture, encoding='utf-8')
exe = out / 'fixture.exe'
batch = out / 'compile.cmd'
batch.write_text('@echo off\ncall "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64\nif errorlevel 1 exit /b %errorlevel%\n' + f'cl /nologo /std:c++20 /EHsc /W4 /WX /Od /Fe:"{exe}" /Fo:"{out}/" "{cpp}"\nexit /b %errorlevel%\n', encoding='ascii', newline='\r\n')
result = subprocess.run(['cmd.exe','/d','/c',str(batch)],cwd=ROOT)
if result.returncode:
    raise SystemExit(result.returncode)
raise SystemExit(subprocess.run([str(exe)],cwd=ROOT,timeout=30).returncode)
