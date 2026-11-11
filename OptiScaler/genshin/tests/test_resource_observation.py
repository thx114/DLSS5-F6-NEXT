"""Resource observation lifecycle/wiring; optional native CPU execution, no GPU."""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cpu', action='store_true')
args = parser.parse_args()
raster = (ROOT / 'OptiScaler/genshin/GenshinReturnedRaster.h').read_text(encoding='utf-8-sig')
timing = (ROOT / 'OptiScaler/genshin/GenshinHookCpuTiming.h').read_text(encoding='utf-8-sig')
hooks = (ROOT / 'OptiScaler/resource_tracking/GenericDepth_Dx11.cpp').read_text(encoding='utf-8-sig')
owner = (ROOT / 'OptiScaler/genshin/GenshinLegacyCapture.cpp').read_text(encoding='utf-8-sig')
checks = 0

def check(condition, message):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(message)

signature = 'inline void DiscardPending(State& s)noexcept {'
start = raster.index(signature)
end = raster.index('\n}', start) + 2
discard = raster[start:end]
check('std::optional<PendingRaster> pending' in raster, 'absence is represented without constructing a Raster')
check('std::variant<DrawTicket,Raster>' in raster and 'SnapshotTicket' in raster, 'invalidated intervals retain only a draw ticket')
check('s.pending.reset()' in discard and 's.pending={}' not in discard, 'empty invalidation never rebuilds a snapshot')
check('Unproved(i,Refusal::MissingReturn' in discard and 'i.returnedSerial=0' in discard, 'unpaired real draws still revoke return provenance')
check(raster.count('s.pending.emplace(') == 2 and 's.pending.emplace()' not in raster,
      'BeforeRaster constructs only the selected alternative without value-initializing inactive storage')
check('if(!s.pending)return false;' in raster and 'if(!s.pending){Unproved(out,Refusal::MissingReturn)' in raster,
      'indirect and returned paths refuse an absent snapshot')
check('raster.pending || raster.clear.resource' in owner, 'idle consumer still rejects retained pending work')
check('raster.pending.source' not in owner, 'all external consumers migrated')
for name in ['MapPreimage', 'MapInvalidate', 'MapSemantic', 'MapPortable', 'MapProbe',
             'UnmapInvalidate', 'UnmapProbe', 'UnmapPortable', 'UnmapSemantic']:
    check(f'Stage::{name}' in hooks and f'"{name}"' in timing, f'{name} measures the existing consumer call')
check('return std::forward<Work>(work)();' in timing, 'observer preserves result type and invocation')
print(f'Resource observation wiring: {checks} checks passed', flush=True)

if args.cpu:
    fixture = r'''
#include <genshin/GenshinHookCpuTiming.h>
#include <array>
#include <optional>
#include <variant>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <cstdio>
#include <cstdlib>
unsigned checks=0,constructed=0,destroyed=0,refused=0;
void Check(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAILED: %s\n",why);std::abort();}}
enum class Refusal {MissingReturn};
struct Raster {std::shared_ptr<int> lease;Raster(){++constructed;}~Raster(){++destroyed;}};
struct DrawTicket {};
using PendingRaster=std::variant<DrawTicket,Raster>;
struct Interval {uint64_t returnedSerial=13;};
struct Track {Interval interval;};
struct State {std::array<Track,4> tracks;std::optional<PendingRaster> pending;int pendingSlot=-1;bool indirectReturned=false;};
void Unproved(Interval&,Refusal,const Raster*)noexcept {++refused;}
// INSERT_PRODUCTION_DISCARD
int main(){
    State s;
    for(unsigned i=0;i<100000;++i)DiscardPending(s);
    Check(!constructed && !destroyed && !refused,"100000 empty invalidations construct no snapshots");
    auto lease=std::make_shared<int>(9);std::weak_ptr<int> weak=lease;
    std::get<Raster>(s.pending.emplace(std::in_place_type<Raster>)).lease=std::move(lease);s.pendingSlot=2;s.indirectReturned=true;
    DiscardPending(s);
    Check(constructed==1 && destroyed==1 && weak.expired(),"real pending snapshot releases exactly once");
    Check(refused==1 && s.tracks[2].interval.returnedSerial==0 && s.pendingSlot==-1 && !s.indirectReturned,"missing real return revokes its exact interval");
    DiscardPending(s);Check(refused==1 && destroyed==1,"repeated invalidation cannot retire twice");
    s.pendingSlot=1;DiscardPending(s);
    Check(refused==2 && s.tracks[1].interval.returnedSerial==0,"missing snapshot with a named bracket fails closed");
    s.pending.emplace(std::in_place_type<Raster>);DiscardPending(s);
    Check(destroyed==2 && refused==2,"unindexed partial snapshot releases without inventing an interval");
    for(unsigned i=0;i<100000;++i){s.pending.emplace();DiscardPending(s);}
    Check(constructed==2 && destroyed==2 && refused==2,"100000 ticket invalidations construct no full snapshots");
    s.pending.emplace();s.pendingSlot=3;DiscardPending(s);
    Check(refused==3 && s.tracks[3].interval.returnedSerial==0 && constructed==2,"unpaired ticket still revokes the exact return serial");
    using namespace GenshinHookCpuTiming;
    control=0;unsigned calls=0;
    auto value=ObserveWork(Stage::MapSemantic,[&]{++calls;return std::make_unique<int>(17);});
    Check(calls==1 && value && *value==17,"disabled observer forwards a move-only result exactly once");
    int number=5;int& reference=ObserveWork(Stage::MapProbe,[&]() -> int& {return number;});reference=8;
    Check(number==8,"observer preserves reference return type");
    ObserveWork(Stage::UnmapProbe,[&]{++calls;});Check(calls==2,"void observer calls once");
    bool threw=false;try{ObserveWork(Stage::UnmapSemantic,[]{throw std::runtime_error("expected");});}catch(const std::runtime_error&){threw=true;}
    Check(threw,"observer preserves exceptions");
    LARGE_INTEGER hz{};Check(QueryPerformanceFrequency(&hz)!=FALSE,"native CPU clock available");
    frequency=hz.QuadPart;control=1;
    for(unsigned i=0;i<65;++i)ObserveWork(Stage::MapPortable,[]{});
    const auto bucket=HookCount*PathCount+static_cast<unsigned>(Stage::MapPortable)*2;
    Check(local.values[bucket][Calls]==65 && local.values[bucket][Samples]==2,"new stages retain the existing stride64 sampler");
    for(unsigned i=0;i<StageCount;++i)Check(StageName(i) && StageName(i)[0],"every stage has a valid publication name");
    control=0;
    std::printf("Resource observation CPU: %u checks passed; no graphics API or game\n",checks);
}
'''.replace('// INSERT_PRODUCTION_DISCARD', discard)
    with tempfile.TemporaryDirectory(prefix='resource-observe-', dir=ROOT / 'build') as temporary:
        out = Path(temporary)
        cpp = out / 'test.cpp'
        cpp.write_text(fixture, encoding='utf-8')
        command = (f'cl /nologo /std:c++20 /EHsc /W4 /WX /Od /I"{ROOT / "OptiScaler"}" '
                   f'/Fe:"{out / "test.exe"}" /Fo:"{out}/" "{cpp}" '
                   f'"{ROOT / "OptiScaler/genshin/GenshinHookCpuTiming.cpp"}"')
        batch = out / 'compile.cmd'
        batch.write_text('@echo off\ncall "%VSDEVCMD%" -no_logo -arch=x64 -host_arch=x64\n'
                         'if errorlevel 1 exit /b %errorlevel%\n' + command + '\nexit /b %errorlevel%\n',
                         encoding='utf-8', newline='\r\n')
        subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=out, check=True)
        subprocess.run([str(out / 'test.exe')], cwd=out, check=True)
