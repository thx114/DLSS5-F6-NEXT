"""Execute production semantic Access/SyncLoss with CPU owner fixtures."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
semantic = (ROOT / 'OptiScaler/genshin/GenshinDepthSemantic.h').read_text(encoding='utf-8-sig')
owner = (ROOT / 'OptiScaler/genshin/GenshinLegacyCapture.cpp').read_text(encoding='utf-8-sig')
raster = (ROOT / 'OptiScaler/genshin/GenshinReturnedRaster.h').read_text(encoding='utf-8-sig')
portable = (ROOT / 'OptiScaler/genshin/GenshinDepthPortableResources.h').read_text(encoding='utf-8-sig')

def region(start, end):
    return semantic.split(start, 1)[1].split(end, 1)[0]

mapped = region('inline bool Mapped(', 'class Submission final')
update = region('static Submission Update(', 'static Submission Unmap(')
unmap = region('static Submission Unmap(', '// A successful receipt')
returned = region('bool Returned()noexcept {', 'inline std::shared_ptr<const Draw> Capture(')
capture = semantic[semantic.rindex('inline std::shared_ptr<const Draw> Capture('):]
assert mapped.index('observation.Requested()') < mapped.index('detail::Find(a.s,r,true)')
assert update.index('observation.Requested()') < update.index('detail::Find(a.s,r,true)')
assert unmap.index('std::memcpy(out.mapped_.driver') < unmap.index('detail::Access a;')
assert 'owner && a.s.observation.Requested()' in unmap
assert 'if(a.s.observation.Requested())b->payload=payload_;else b->payload.reset();' in returned
assert 'observation.Requested() || !detail::BindOwner' in capture
poll = region('inline void Poll(ID3D11DeviceContext* c)noexcept {', 'inline void Invalidate(')
assert 'detail::PollLocked(a.s,c);detail::QueueDeferredLocked(a.s,c);' in poll
queue = region('inline bool QueueDeferredLocked(', 'inline void PollLocked(')
assert 'if(!only && !s.observation.Requested())return true;' in queue
demand = owner.split('FramePipeline::InputObservationDemand ReadNewInputObservationDemand()', 1)[1].split('bool UpdateNativeObservationDemand', 1)[0]
for forbidden in ('nativeMutex', 'gpuPending', 'gpuQuarantined', 'workerFinished', 'AcquireGenshinControls'):
    assert forbidden not in demand, forbidden
assert 'cfg.GenshinControlsCurrent(captureEpoch)' in demand
before_raster = raster.split('inline void BeforeRaster(',1)[1].split('inline void IndirectReturned(',1)[0]
assert before_raster.index('t.interval.returnedSerial=0') < before_raster.index('ReadNewInputObservationDemand()') < before_raster.index('s.pending.emplace(')
assert 'Unproved(t.interval,Refusal::WriteBoundary);return;' in before_raster
assert before_raster.index('if(t.interval.semanticComplete)') < before_raster.index('s.pending.emplace(std::in_place_type<Raster>)') < before_raster.index('detail::Snapshot(c,raster,e,std::move(binding))')
assert 's.pending.emplace(std::in_place_type<DrawTicket>)' in before_raster
assert 'detail::SnapshotTicket(c,ticket,e)' in before_raster
portable_mapped=portable.split('inline void Mapped(',1)[1].split('// Take BEFORE original Unmap',1)[0]
assert portable_mapped.index('auto r=Written(resource);') < portable_mapped.index('ReadNewInputObservationDemand()') < portable_mapped.index('next.shadow=Allocate')
assert 'ReadNewInputObservationDemand' not in portable.split('inline void Created(',1)[1].split('inline void LayoutCreated(',1)[0]
assert 'ReadNewInputObservationDemand' not in portable.split('inline Unmap BeforeUnmap(',1)[1]
for signature in ('inline Upload Update(', 'inline Upload Copy(', 'inline std::shared_ptr<const Image> Before('):
    assert 'ReadNewInputObservationDemand().Requested()' in portable.split(signature,1)[1].split('try{',1)[0].split('\n}',1)[0]
print('Input observation wiring checks passed')

# Use the actual production loss synchronization and try-lock Access, rather
# than implementing a parallel transition algorithm in the test.
production = 'inline bool SyncLoss(State& s)noexcept {' + region('inline bool SyncLoss(State& s)noexcept {', 'inline thread_local unsigned internal=0;')
fixture = r'''
#include "runtime/InputObservation.h"
#include "runtime/CpuSnapshotBytes.h"
#include "runtime/MappedResourceJournal.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <cstdint>
#define LOG_INFO(...) ((void)0)
enum {D3D11_USAGE_DEFAULT,D3D11_USAGE_IMMUTABLE,D3D11_USAGE_DYNAMIC};
FramePipeline::InputObservationDemand requestedInputs{true};
namespace GenshinLegacy {
FramePipeline::InputObservationDemand ReadNewInputObservationDemand()noexcept {return requestedInputs;}
}
struct Desc {int Usage=D3D11_USAGE_DYNAMIC;unsigned CPUAccessFlags=1;};
struct Payload {int value=7;bool ready=false,failed=false;};
struct Buffer {bool object=true;Desc desc;std::shared_ptr<Payload> payload;
    uint64_t revision=0;bool mapped=false,mappingKnown=true;};
struct Flight {bool queued=false;std::array<std::shared_ptr<Payload>,3> destination;};
struct State {std::mutex mutex;FramePipeline::InputObservationSession observation;
    uint64_t lossEpoch=0,serial=0;std::array<Buffer,3> buffers;
    std::array<Flight,3> flights;};
State& StateStore(){static State s;return s;}
std::atomic<uint64_t> loss{1};
void Lost()noexcept {auto n=loss.load();while(n!=UINT64_MAX && !loss.compare_exchange_weak(n,n+1)){};}
// PRODUCTION
unsigned checks=0;
void Check(bool ok,const char* message){++checks;if(!ok){std::fprintf(stderr,"FAILED: %s\n",message);std::abort();}}
int main(){
    for(unsigned mask=0;mask<64;++mask){
        FramePipeline::InputObservationDemand d{bool(mask&1),bool(mask&2),bool(mask&4),bool(mask&8),bool(mask&16),bool(mask&32)};
        Check(d.Requested()==(mask!=0),"every independent new-input consumer retains demand");
    }
    auto& s=StateStore();
    {Access a;Check(bool(a) && s.observation.Requested(),"initial NR demand");}
    auto frozen=std::make_shared<Payload>();frozen->ready=true;s.buffers[0].payload=frozen;
    auto pending=std::make_shared<Payload>();s.buffers[1].payload=pending;
    auto queued=std::make_shared<Payload>();s.buffers[2].payload=queued;
    s.buffers[1].desc={D3D11_USAGE_IMMUTABLE,0};s.buffers[2].desc={D3D11_USAGE_DEFAULT,0};
    s.flights[0].queued=true;s.flights[0].destination[0]=queued;
    std::weak_ptr<Payload> gpu=s.flights[0].destination[0];
    using Bytes=FramePipeline::CpuSnapshotBytes;
    FramePipeline::MappedResourceJournal<std::shared_ptr<Bytes>,2> maps;
    auto shadow=std::make_shared<Bytes>(64);auto* application=shadow->data();
    Check(maps.TryInsert({11,21,0},std::move(shadow)),"application shadow registered");
    std::memset(application,0x37,64);auto before=loss.load();requestedInputs={};
    {Access a;Check(bool(a) && !s.observation.Requested(),"NR off stops new inputs");}
    Check(loss.load()==before+1 && !s.buffers[0].payload,"stop revokes live numeric payload");
    Check(frozen->value==7 && frozen->ready && !frozen->failed && !gpu.expired(),"stop leaves frozen readers and GPU flight custody");
    Check(pending->failed && !queued->failed && !queued->ready,"stop cancels only unqueued pending input, not real GPU flight");
    Check(!s.buffers[0].mappingKnown && s.buffers[1].mappingKnown && s.buffers[2].mappingKnown,"stop does not invent dynamic mapping certainty");
    auto revision=s.buffers[0].revision;
    for(unsigned i=0;i<1000;++i){Access a;Check(bool(a),"stable stopped access remains synchronized");}
    Check(s.buffers[0].revision==revision && loss.load()==before+1,"stopped calls do not churn generations");
    Check(maps.HasEntries(),"stop never cancels outstanding application writeback");
    auto completed=maps.Take({11,21,0});std::array<unsigned char,64> driver{};
    std::memcpy(driver.data(),completed->data(),64);
    Check(driver[0]==0x37 && driver[63]==0x37 && !maps.HasEntries(),"exact pending shadow survives stop and writes back");
    requestedInputs.nr=true;{Access a;Check(bool(a) && s.observation.Requested(),"NR restart restores observation");}
    Check(loss.load()==before+2 && s.buffers[0].revision>revision && !s.buffers[0].payload,"restart cannot revive old live payload");
    requestedInputs={};{Access a;Check(bool(a) && !s.observation.Requested(),"second stop");}
    requestedInputs.unknown=true;{Access a;Check(bool(a) && s.observation.Requested(),"unknown demand retains conservative observation");}
    auto savedLoss=loss.load();
    std::unique_lock held(s.mutex);bool refused=false;
    std::thread contender([&]{Access a;refused=!bool(a);});contender.join();held.unlock();
    Check(refused && loss.load()==savedLoss+1,"contention preserves original loss behavior");
    {Access a;Check(bool(a) && s.lossEpoch==loss.load(),"owner resynchronizes after contention");}
    loss=UINT64_MAX;{Access a;Check(!bool(a),"saturated loss fails closed");}
    std::printf("Input observation CPU: %u checks passed; no graphics API or game\n",checks);
}
'''.replace('// PRODUCTION', production)
# Windows may briefly retain a compiler/debug-service directory handle after
# the checked child processes exit. Cleanup is best effort; test failures are not.
with tempfile.TemporaryDirectory(prefix='input-observation-', dir=ROOT / 'build', ignore_cleanup_errors=True) as directory:
    out=Path(directory)
    (out / 'test.cpp').write_text(fixture, encoding='utf-8')
    command=(f'cl /nologo /std:c++20 /EHsc /W4 /WX /Od /I"{ROOT / "OptiScaler"}" '
             f'/Fe:"{out / "test.exe"}" /Fo:"{out}/" "{out / "test.cpp"}"')
    batch=out / 'compile.cmd'
    batch.write_text('@echo off\ncall "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64\n'
                     'if errorlevel 1 exit /b %errorlevel%\n'+command+'\nexit /b %errorlevel%\n',encoding='utf-8',newline='\r\n')
    subprocess.run(['cmd.exe','/d','/c',str(batch)],cwd=out,check=True)
    subprocess.run([str(out / 'test.exe')],cwd=out,check=True)
