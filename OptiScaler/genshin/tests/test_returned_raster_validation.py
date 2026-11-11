"""Execute the actual returned-raster transaction with CPU owner/proof fixtures.

Public D3D11 getter behavior is tested separately by Dx11RasterBindingsTests.
Here the comparator and numerical terminal are test doubles, so this suite
checks orchestration and rejection, not the numerical proof implementation.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
source = (ROOT / 'OptiScaler/genshin/GenshinReturnedRaster.h').read_text(encoding='utf-8-sig')
signature = 'inline void RasterReturned(ID3D11DeviceContext* c,Epochs e)noexcept {'
transaction = signature + source.split(signature, 1)[1].split('// Read-only inspection', 1)[0]
def definition(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 2]

ticket_types = source[source.index('struct DrawTicket {'):source.index('struct Interval {')]
raster_types = source[source.index('struct Binding {'):source.index('struct DrawTicket {')]
raster_types = raster_types.replace('struct Raster {', 'struct Raster {\n    ConstructionProbe constructionProbe;')
helpers = (definition(source, 'inline const GenshinDepthMapping::DrawArguments& PendingDraw(') + '\n' +
           definition(source, 'inline Epochs PendingEpochs(') + '\n' +
           definition(source, 'inline bool SnapshotTicket('))
semantic = (ROOT / 'OptiScaler/genshin/GenshinDepthSemantic.h').read_text(encoding='utf-8-sig')
same_arguments = definition(semantic, 'inline bool SameArguments(')
assert 'detail::Snapshot(' not in transaction
assert 'MatchesDx11RasterBindings(c,*raster,raster->constantContext.Get())' in transaction
assert 'GenshinDepthSemantic::Returned(c,out.last.semantic)' in transaction
assert 'GenshinQueryScope::KnownQuiescent(c)' in transaction

fixture = r'''
#include <d3d11_1.h>
#include "genshin/GenshinDepthDraw.h"
#include "genshin/GenshinDepthRetention.h"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <cstdio>
#include <cstdlib>
#include <utility>
unsigned checks=0,bindings=0,queries=0,terminal=0,constructed=0;
unsigned moved=0,copied=0;
struct ConstructionProbe {
    ConstructionProbe(){++constructed;}
    ConstructionProbe(const ConstructionProbe&){++copied;}
    ConstructionProbe(ConstructionProbe&&)noexcept{++moved;}
};
using Microsoft::WRL::ComPtr;
enum class PixelShaderPresence {Null,PresentUnqualified};
bool bindingMatches=true,queryQuiescent=true,terminalAccepts=true,ownerCurrent=true;
void Check(bool value,const char* why){++checks;if(!value){std::fprintf(stderr,"FAILED: %s\n",why);std::abort();}}
struct Epochs{uint64_t device=1,config=1;bool operator==(const Epochs&)const=default;};
enum class Refusal {None,SemanticCapture,RasterDepthBias,MissingReturn,ReturnedState,CoverageOverflow,SemanticReturn,CoverageAllocation,CoverageBudget,WriteBoundary,DrawAuthority};
struct Proof{mutable bool returned=false;};
struct Identity final : IUnknown {
    ULONG refs=1;static inline unsigned alive=0;
    Identity(){++alive;}
    ~Identity(){--alive;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{
        if(!out)return E_POINTER;*out=nullptr;
        if(id!=__uuidof(IUnknown))return E_NOINTERFACE;
        *out=this;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override{const auto remaining=--refs;if(!remaining)delete this;return remaining;}
};
namespace GenshinDepthSemantic {
using Draw=Proof;
struct CaptureFailureObservation {unsigned reason=0;};
namespace detail {
// PRODUCTION_ARGUMENT_COMPARISON
}
enum class CoverageStatus{Stored,Overflow,Allocation,InvalidProof,Budget};
bool Returned(ID3D11DeviceContext*,const std::shared_ptr<const Proof>& proof)noexcept{
    ++terminal;if(!proof || proof->returned || !terminalAccepts)return false;proof->returned=true;return true;
}
}
// PRODUCTION_RASTER_TYPES
// PRODUCTION_TICKET_TYPES
namespace FramePipeline {bool MatchesDx11RasterBindings(ID3D11DeviceContext*,const Raster&,ID3D11DeviceContext1*)noexcept{++bindings;return bindingMatches;}}
namespace GenshinQueryScope{bool KnownQuiescent(ID3D11DeviceContext*)noexcept{++queries;return queryQuiescent;}}
struct Writers {
    unsigned appended=0;GenshinDepthSemantic::CoverageStatus result=GenshinDepthSemantic::CoverageStatus::Stored;
    auto Append(const std::shared_ptr<const Proof>&){++appended;return result;}
};
struct Interval {
    Raster last;bool lastIsTicket=true,clearReturned=true,semanticComplete=true;
    uint64_t returnedSerial=19,returnedDraws=0,unresolvedWriters=0,coveredWriters=0;
    Refusal first=Refusal::None;Writers semanticWriters;
};
struct Track{Interval interval;};
struct State{std::array<Track,1> tracks;int pendingSlot=0;std::optional<PendingRaster> pending;bool indirectReturned=false;};
State state;
void Unproved(Interval& i,Refusal r,const Raster* =nullptr)noexcept{i.semanticComplete=false;if(i.first==Refusal::None)i.first=r;}
namespace detail {
struct Access {State& s=state;explicit operator bool()const noexcept{return true;}};
bool Owner(State&,ID3D11DeviceContext*,Epochs)noexcept{return ownerCurrent;}
uint64_t Next(State&)noexcept{return 23;}
// PRODUCTION_HELPERS
}
// PRODUCTION_TRANSACTION
Raster& Full(){return std::get<Raster>(*state.pending);}
DrawTicket& Ticket(){
    state.pendingSlot=0;
    auto& value=std::get<DrawTicket>(state.pending.emplace(std::in_place_type<DrawTicket>));
    detail::SnapshotTicket(GenshinDepthMapping::CurrentDraw().context,value,{});
    return value;
}
void Reset(){
    state=State{};state.pending.emplace(std::in_place_type<Raster>);
    bindingMatches=queryQuiescent=terminalAccepts=ownerCurrent=true;
    bindings=queries=terminal=0;
    GenshinDepthMapping::ObserveDrawArguments(reinterpret_cast<ID3D11DeviceContext*>(uintptr_t(1)),2,3,0,0,1,0);
    Full().draw=GenshinDepthMapping::CurrentDraw();
    Full().semantic=std::make_shared<Proof>();Full().queryQuiescent=true;
}
void SetIndirect(){
    auto& draw=GenshinDepthMapping::CurrentDraw();
    draw.indirectArgumentsUnknown=true;draw.kind=6;draw.count=draw.instances=0;
    Full().draw=draw;
}
int main(){
    auto* c=reinterpret_cast<ID3D11DeviceContext*>(uintptr_t(1));
    Reset();const auto before=constructed;RasterReturned(c,{});
    auto& i=state.tracks[0].interval;
    Check(!state.pending && i.returnedDraws==1 && i.coveredWriters==1 && i.returnedSerial==23,"one exact returned writer appended");
    Check(constructed==before && bindings==1 && queries==1 && terminal==1,"return validates original proof without constructing another raster");
    Check(!i.lastIsTicket,"a retained full snapshot records its non-ticket payload");
    Reset();bindingMatches=false;RasterReturned(c,{});
    Check(!i.semanticComplete && i.first==Refusal::ReturnedState && !terminal && !queries,"binding mismatch stays sticky and never enters terminal");
    Reset();Full().epochs.config=2;RasterReturned(c,{});
    Check(!i.semanticComplete && !bindings && !terminal,"changed config refuses before public binding comparison");
    Reset();++Full().draw.observationCall;RasterReturned(c,{});
    Check(!i.semanticComplete && !bindings && !terminal,"different original call refuses");
    Reset();SetIndirect();RasterReturned(c,{});
    Check(!i.semanticComplete && !bindings && !terminal,"missing indirect original return refuses");
    Reset();SetIndirect();state.indirectReturned=true;RasterReturned(c,{});
    Check(i.semanticComplete && i.coveredWriters==1 && terminal==1,"authenticated indirect original return retains terminal check");
    Reset();terminalAccepts=false;RasterReturned(c,{});
    Check(!i.semanticComplete && i.first==Refusal::SemanticReturn && i.unresolvedWriters==1 && !i.coveredWriters,"terminal revision/payload refusal cannot be repaired by fresh capture");
    Reset();Full().semantic->returned=true;RasterReturned(c,{});
    Check(i.first==Refusal::SemanticReturn && !i.coveredWriters,"one proof cannot certify two original returns");
    Reset();queryQuiescent=false;RasterReturned(c,{});
    Check(i.semanticComplete && !i.last.queryQuiescent && queries==1,"query state is re-observed after actual return");
    Reset();Full().queryQuiescent=false;RasterReturned(c,{});
    Check(!i.last.queryQuiescent && queries==1,"a later quiet query cannot repair before-query coverage");
    Reset();i.semanticComplete=false;Full().semantic.reset();RasterReturned(c,{});
    Check(!i.semanticComplete && i.unresolvedWriters==1 && !terminal,"previous unproved writer stays unresolved");
    Reset();i.semanticComplete=false;Ticket();RasterReturned(c,{});
    Check(!i.semanticComplete && i.unresolvedWriters==1 && !bindings && !queries && !terminal,"ticket-only writer skips public state and terminal checks");
    Reset();RasterReturned(c,{});i.semanticComplete=false;
    const auto initialConstructed=constructed,initialMoved=moved,initialCopied=copied;
    bindings=queries=0;
    std::weak_ptr<const Proof> releasedProof=i.last.semantic;
    i.last.strides[31]=79;i.last.constants[55].first=16;i.last.reference=7;
    Ticket();RasterReturned(c,{});
    Check(releasedProof.expired() && !i.last.semantic && !i.last.queryQuiescent && i.lastIsTicket,
          "first ticket releases the previous proof without constructing another Raster");
    Check(i.last.strides[31]==0 && i.last.constants[55].first==0 && i.last.constants[55].count==4096 &&
          !i.last.reference && i.last.viewportCount==16 && i.last.scissorCount==16,
          "ticket transition preserves the old empty binding record");
    for(unsigned n=0;n<100000;++n){state.pendingSlot=0;Ticket();RasterReturned(c,{});}
    Check(constructed==initialConstructed && moved==initialMoved && copied==initialCopied &&
          i.returnedDraws==100002 && i.unresolvedWriters==100001 && !bindings && !queries && terminal==1,
          "100001 actual ticket returns neither construct nor move nor copy a Raster");
    Reset();i.semanticComplete=false;++Ticket().draw.observationCall;RasterReturned(c,{});
    Check(i.first==Refusal::ReturnedState && i.unresolvedWriters==1 && !bindings && !terminal,"ticket-only call mismatch remains unresolved");
    Reset();i.semanticComplete=false;Ticket().epochs.config=2;RasterReturned(c,{});
    Check(i.first==Refusal::ReturnedState && i.unresolvedWriters==1 && !bindings && !queries && !terminal,"ticket-only epoch mismatch refuses without getters");
    Reset();i.semanticComplete=false;++Ticket().draw.start;RasterReturned(c,{});
    Check(i.first==Refusal::ReturnedState && !bindings && !queries && !terminal,"ticket-only full draw arguments must match");
    Reset();i.semanticComplete=false;Ticket().draw.context=nullptr;GenshinDepthMapping::CurrentDraw().context=nullptr;RasterReturned(c,{});
    Check(i.first==Refusal::ReturnedState && !bindings && !queries && !terminal,"ticket-only matching foreign arguments cannot claim this context return");
    Reset();i.semanticComplete=false;GenshinDepthMapping::CurrentDraw().indirectArgumentsUnknown=true;Ticket();RasterReturned(c,{});
    Check(i.first==Refusal::ReturnedState && !bindings && !terminal,"ticket-only indirect writer requires the original return");
    Reset();i.semanticComplete=false;GenshinDepthMapping::CurrentDraw().indirectArgumentsUnknown=true;Ticket();state.indirectReturned=true;RasterReturned(c,{});
    Check(i.returnedDraws==1 && i.unresolvedWriters==1 && !i.coveredWriters && !bindings && !queries && !terminal,"ticket-only returned indirect writer never gains semantic authority");
    Reset();i.semanticComplete=false;
    GenshinDepthMapping::CurrentDraw().indirectIdentity.Attach(new Identity);
    Ticket();
    Check(Identity::alive==1,"ticket retains the actual call identity across the original draw");
    RasterReturned(c,{});
    Check(!i.last.draw.indirectIdentity && !i.last.draw.indirectBuffer,"returned ticket stores no uncharged indirect COM roots");
    GenshinDepthMapping::CurrentDraw().indirectIdentity.Reset();
    Check(!Identity::alive,"finishing current observation releases ticket identity without another clear");
    Reset();Ticket();RasterReturned(c,{});
    Check(!i.semanticComplete && i.first==Refusal::WriteBoundary && !i.coveredWriters && !terminal,"ticket cannot certify even an accidentally complete interval");
    Reset();i.semanticComplete=false;Ticket();i.returnedDraws=UINT64_MAX;RasterReturned(c,{});
    Check(!i.returnedSerial && !state.pending && !bindings && !terminal,"ticket counter overflow clears the bracket without wrapping");
    Reset();i.semanticComplete=false;Ticket();ownerCurrent=false;RasterReturned(c,{});
    Check(state.pending && !bindings && !queries && !terminal,"foreign owner cannot consume a ticket");
    Reset();i.semanticComplete=false;Ticket();i.first=Refusal::SemanticCapture;RasterReturned(c,{});
    Check(i.first==Refusal::SemanticCapture && !i.semanticComplete && i.unresolvedWriters==1,"ticket preserves the original sticky refusal");
    Reset();Full().semantic.reset();RasterReturned(c,{});
    Check(i.first==Refusal::SemanticCapture && !i.coveredWriters,"missing numerical capture refuses");
    Reset();state.pending.reset();RasterReturned(c,{});
    Check(i.first==Refusal::MissingReturn && !i.returnedSerial && !terminal,"missing before snapshot invalidates serial");
    Reset();i.clearReturned=false;RasterReturned(c,{});
    Check(!i.semanticComplete && !bindings && !terminal,"missing clear return refuses");
    Reset();i.returnedDraws=UINT64_MAX;RasterReturned(c,{});
    Check(!i.returnedSerial && !state.pending && !terminal,"coverage overflow refuses without wrapping");
    Reset();i.semanticWriters.result=GenshinDepthSemantic::CoverageStatus::Budget;RasterReturned(c,{});
    Check(i.first==Refusal::CoverageBudget && !i.coveredWriters,"metadata budget refusal preserved");
    Reset();ownerCurrent=false;RasterReturned(c,{});
    Check(state.pending && !bindings && !terminal,"foreign owner cannot consume pending pair");
    Reset();state.pending.reset();const auto constructions=constructed;
    bool captured=true;
    for(unsigned n=0;n<100000;++n){
        PendingRaster pending(std::in_place_type<DrawTicket>);
        auto& ticket=std::get<DrawTicket>(pending);
        captured=detail::SnapshotTicket(c,ticket,{}) && captured;
    }
    Check(captured && constructed==constructions,"100000 real ticket captures construct no Raster and call no D3D getters");
    using Pending=std::optional<PendingRaster>;
    alignas(Pending) std::array<unsigned char,sizeof(Pending)> storage;
    storage.fill(0xa5);
    auto* pending=new(storage.data()) Pending;
    pending->emplace(std::in_place_type<DrawTicket>);
    bool untouched=true;
    for(size_t n=storage.size()/2;n<storage.size()-64;++n)untouched=untouched && storage[n]==0xa5;
    pending->~Pending();
    Check(untouched,"ticket emplacement leaves the inactive Raster payload bytes untouched");
    DrawTicket bad;
    Check(!detail::SnapshotTicket(nullptr,bad,{}) && bad.refusal==Refusal::DrawAuthority,"ticket capture rejects foreign context");
    Check(detail::SnapshotTicket(c,bad,{}) && bad.refusal==Refusal::WriteBoundary,"a fresh valid observation cannot inherit a previous ticket failure");
    GenshinDepthMapping::CurrentDraw().context=nullptr;
    Check(!detail::SnapshotTicket(nullptr,bad,{}),"ticket capture rejects absent context even when both pointers are null");
    GenshinDepthMapping::CurrentDraw().context=c;
    GenshinDepthMapping::CurrentDraw().thread=0;
    Check(!detail::SnapshotTicket(c,bad,{}) && bad.refusal==Refusal::DrawAuthority,"ticket capture rejects foreign thread");
    GenshinDepthMapping::CurrentDraw().thread=GetCurrentThreadId();GenshinDepthMapping::CurrentDraw().observationCall=0;
    Check(!detail::SnapshotTicket(c,bad,{}) && bad.refusal==Refusal::DrawAuthority,"ticket capture rejects absent call");
    std::printf("Returned raster transaction: %u CPU checks passed; production orchestration, proof/comparator fixtures\n",checks);
}
'''.replace('// PRODUCTION_TRANSACTION', transaction).replace('// PRODUCTION_RASTER_TYPES', raster_types).replace('// PRODUCTION_TICKET_TYPES', ticket_types).replace('// PRODUCTION_HELPERS', helpers).replace('// PRODUCTION_ARGUMENT_COMPARISON', same_arguments)

with tempfile.TemporaryDirectory(prefix='returned-validation-', dir=ROOT / 'build', ignore_cleanup_errors=True) as directory:
    out = Path(directory)
    (out / 'test.cpp').write_text(fixture, encoding='utf-8')
    command = (f'cl /nologo /std:c++20 /EHsc /W4 /WX /Od /I"{ROOT / "OptiScaler"}" '
               f'/Fe:"{out / "test.exe"}" /Fo:"{out}/" "{out / "test.cpp"}"')
    batch = out / 'compile.cmd'
    batch.write_text('@echo off\ncall "%VSDEVCMD%" -no_logo -arch=x64 -host_arch=x64\n'
                     'if errorlevel 1 exit /b %errorlevel%\n' + command + '\nexit /b %errorlevel%\n',
                     encoding='utf-8', newline='\r\n')
    subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=out, check=True)
    subprocess.run([str(out / 'test.exe')], cwd=out, check=True)
