"""Run production BeforeRaster/capture against real D3D11 WARP bindings.

Owner/demand and numerical capture are fixtures. No Draw, GPU submission, SDK,
game, performance claim, or production DLL build is involved.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
source = (ROOT / 'OptiScaler/genshin/GenshinReturnedRaster.h').read_text(encoding='utf-8-sig')


def definition(signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]


types = source[source.index('enum class PixelShaderPresence'):source.index('inline const char* RefusalText')]
raster = source[source.index('struct Binding {'):source.index('inline GenshinGuides::DepthSource Describe(')]
raster = raster.replace('struct Raster {', 'struct Raster {\n    ConstructionProbe constructionProbe;')
depth = source[source.index('struct RasterDepthBinding {'):source.index('// r is freshly emplaced')]
helpers = '\n'.join(definition(signature) for signature in (
    'inline void DiscardPending(', 'inline bool ClearView(', 'inline int Find(',
    'inline UINT TrackedRasterOutputBindings(', 'inline bool SnapshotCandidate(',
    'inline bool Snapshot(', 'inline bool SnapshotTicket('))
before = definition('inline void BeforeRaster(')
assert 'OMGetRenderTargets' not in definition('inline bool SnapshotCandidate(')
assert 'OMGetDepthStencilState' not in definition('inline bool SnapshotCandidate(')
assert 'ClearView(' not in definition('inline bool SnapshotCandidate(')

fixture = r'''
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include "genshin/GenshinDepthDraw.h"
#include "genshin/GenshinDepthRetention.h"
#include "genshin/GenshinInputObservation.h"
#include "runtime/Dx11RasterBindings.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <variant>
using Microsoft::WRL::ComPtr;
unsigned checks=0,constructed=0,moved=0,copied=0,captures=0,retired=0;
bool requested=true,numerics=true;
void Check(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAILED: %s\n",why);std::abort();}}
struct ConstructionProbe {
    ConstructionProbe(){++constructed;}
    ConstructionProbe(const ConstructionProbe&){++copied;}
    ConstructionProbe(ConstructionProbe&&)noexcept{++moved;}
};
namespace GenshinLegacy {
FramePipeline::InputObservationDemand ReadNewInputObservationDemand()noexcept{return {requested};}
}
namespace GenshinDepthSemantic {
struct Draw {};
struct CaptureFailureObservation {unsigned reason=0;};
struct WriterGroups {};
struct Binding {ID3D11Buffer* buffer;UINT first,count;};
std::shared_ptr<const Draw> Capture(ID3D11DeviceContext*,ID3D11VertexShader*,ID3D11HullShader*,
    ID3D11DomainShader*,ID3D11PixelShader*,D3D11_PRIMITIVE_TOPOLOGY,const std::array<Binding,42>&,
    const D3D11_VIEWPORT&,D3D11_COMPARISON_FUNC,uint64_t,uint64_t,CaptureFailureObservation*)noexcept {
    ++captures;return numerics?std::make_shared<Draw>():nullptr;
}
}
namespace GenshinQueryScope {bool KnownQuiescent(ID3D11DeviceContext*)noexcept{return true;}}
// PRODUCTION_TYPES
enum class ClaimFailure:unsigned {None};
// PRODUCTION_RASTER
// PRODUCTION_UNPROVED
namespace detail {
struct Track {Interval interval;};
struct State {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    Epochs epochs;DWORD thread=0;uint64_t serial=10;
    std::array<Track,4> tracks;std::optional<PendingRaster> pending;
    int pendingSlot=-1;bool indirectReturned=false;
    struct {unsigned serial=0;} clear;
};
State state;
struct Access {State& s=state;explicit operator bool()const noexcept{return true;}};
bool Owner(State& s,ID3D11DeviceContext* c,Epochs e)noexcept {
    return s.context.Get()==c && s.epochs==e && s.thread==GetCurrentThreadId();
}
uint64_t Next(State& s)noexcept{return ++s.serial;}
void NoteLifecycle(State&,ID3D11Resource*,const char*,unsigned=0)noexcept {}
void NoteRetirement(State&,const Interval&,const char*,Refusal=Refusal::None)noexcept {++retired;}
// PRODUCTION_DEPTH
// PRODUCTION_HELPERS
}
// PRODUCTION_BEFORE
void Prepare(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Resource* resource,
             const D3D11_TEXTURE2D_DESC& texture,ID3D11DepthStencilView* view,bool complete=true) {
    detail::state={};auto& s=detail::state;s.device=device;s.context=context;s.epochs={1,1};s.thread=GetCurrentThreadId();
    auto& i=s.tracks[0].interval;i.resource=resource;i.texture=texture;view->GetDesc(&i.clearDesc);
    i.semanticComplete=complete;i.clearReturned=true;i.returnedSerial=7;i.version=5;
    requested=numerics=true;captures=retired=0;
    GenshinDepthMapping::ObserveDrawArguments(context,2,3,9,-2,1,0);
}
Raster& Full(){return std::get<Raster>(*detail::state.pending);}
int main(){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
        D3D11_SDK_VERSION,&device,nullptr,&context)),"create WARP for binding-only tests");
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=32;td.MipLevels=td.ArraySize=1;
    td.Format=DXGI_FORMAT_R32_TYPELESS;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11DepthStencilView> view,readOnly;
    Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&texture)),"create retained depth allocation");
    D3D11_DEPTH_STENCIL_VIEW_DESC vd{};vd.Format=DXGI_FORMAT_D32_FLOAT;vd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    Check(SUCCEEDED(device->CreateDepthStencilView(texture.Get(),&vd,&view)),"create depth view");
    vd.Flags=D3D11_DSV_READ_ONLY_DEPTH;
    Check(SUCCEEDED(device->CreateDepthStencilView(texture.Get(),&vd,&readOnly)),"create read-only alias");
    context->OMSetRenderTargets(0,nullptr,view.Get());
    ComPtr<ID3DBlob> code;
    const char* shader="float4 main(uint id:SV_VertexID):SV_POSITION{return float4(id,0,0,1);}";
    Check(SUCCEEDED(D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&code,nullptr)),"compile binding-only VS");
    ComPtr<ID3D11VertexShader> vs;
    Check(SUCCEEDED(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs)),"create VS");
    context->VSSetShader(vs.Get(),nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT viewport{0,0,32,32,0,1};context->RSSetViewports(1,&viewport);

    Prepare(device.Get(),context.Get(),texture.Get(),td,view.Get());
    auto beforeConstructed=constructed,beforeMoved=moved,beforeCopied=copied;
    BeforeRaster(context.Get(),{1,1});
    Check(constructed==beforeConstructed+1 && moved==beforeMoved && copied==beforeCopied && captures==1,
          "successful BeforeRaster constructs exactly one Raster with no moves or copies");
    Check(Full().view.Get()==view.Get() && Full().source.Get()==texture.Get() && Full().depthDesc.DepthEnable &&
          Full().depthDesc.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ALL && Full().depthDesc.DepthFunc==D3D11_COMPARISON_LESS &&
          !Full().depthDesc.StencilEnable && Full().depthDesc.StencilReadMask==D3D11_DEFAULT_STENCIL_READ_MASK &&
          Full().draw.start==9 && Full().draw.base==-2 && Full().epochs==Epochs{1,1},
          "same-call admitted DSV/default depth state and exact draw arguments reach the snapshot");
    Check(FramePipeline::MatchesDx11RasterBindings(context.Get(),Full(),Full().constantContext.Get()),
          "captured same-context constant interface preserves the full public comparison");
    Prepare(device.Get(),context.Get(),texture.Get(),td,view.Get());requested=false;
    auto& i=detail::state.tracks[0].interval;
    beforeConstructed=constructed;beforeMoved=moved;beforeCopied=copied;
    BeforeRaster(context.Get(),{1,1});
    Check(constructed==beforeConstructed && moved==beforeMoved && copied==beforeCopied && !captures &&
          !detail::state.pending && !i.returnedSerial && i.version>5 && !i.semanticComplete &&
          i.firstUnprovedReason==unsigned(Refusal::WriteBoundary),
          "no consumer constructs no Raster while the real writer still invalidates provenance");
    requested=true;BeforeRaster(context.Get(),{1,1});
    Check(std::holds_alternative<DrawTicket>(*detail::state.pending) && !captures && !i.semanticComplete,
          "restarting a consumer cannot revive an interval missed while stopped");
    beforeConstructed=constructed;beforeMoved=moved;beforeCopied=copied;
    for(unsigned n=0;n<10000;++n)BeforeRaster(context.Get(),{1,1});
    Check(constructed==beforeConstructed && moved==beforeMoved && copied==beforeCopied && !captures,
          "10000 production ticket admissions construct move and copy no Raster");

    context->OMSetRenderTargets(0,nullptr,readOnly.Get());
    Prepare(device.Get(),context.Get(),texture.Get(),td,view.Get());BeforeRaster(context.Get(),{1,1});
    Check(!detail::state.pending && i.returnedSerial==7 && i.version==5 && i.semanticComplete,
          "read-only DSV alias preserves the previous completed writer");
    context->OMSetRenderTargets(0,nullptr,view.Get());
    D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;dd.DepthFunc=D3D11_COMPARISON_LESS;
    ComPtr<ID3D11DepthStencilState> noWrite,write;
    Check(SUCCEEDED(device->CreateDepthStencilState(&dd,&noWrite)),"create nonwriting depth state");
    context->OMSetDepthStencilState(noWrite.Get(),3);
    Prepare(device.Get(),context.Get(),texture.Get(),td,view.Get());BeforeRaster(context.Get(),{1,1});
    Check(!detail::state.pending && i.returnedSerial==7 && i.version==5,"disabled depth writes retain the completed writer");
    dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D11_COMPARISON_GREATER;
    Check(SUCCEEDED(device->CreateDepthStencilState(&dd,&write)),"create explicit depth state");
    context->OMSetDepthStencilState(write.Get(),7);
    Prepare(device.Get(),context.Get(),texture.Get(),td,view.Get());BeforeRaster(context.Get(),{1,1});
    Check(Full().depth.Get()==write.Get() && Full().reference==7 && Full().depthDesc.DepthFunc==D3D11_COMPARISON_GREATER,
          "explicit depth identity descriptor and stencil reference are transferred unchanged");
    context->OMSetDepthStencilState(nullptr,0);

    D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS);
    bd.MiscFlags=D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;ComPtr<ID3D11Buffer> indirect;
    Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&indirect)),"create actual indirect argument allocation");
    Prepare(device.Get(),context.Get(),texture.Get(),td,view.Get());
    GenshinDepthMapping::ObserveDrawArguments(context.Get(),6,0,0,0,0,0,indirect.Get());
    BeforeRaster(context.Get(),{1,1});
    Check(Full().draw.indirectBuffer.Get()==indirect.Get() && Full().draw.indirectIdentity && Full().retention.count==1,
          "indirect capture keeps its actual COM identity and original budget admission");
    detail::state.pending.reset();GenshinDepthMapping::CurrentDraw()={};
    Check(GenshinDepthRetention::ReadTelemetry().bytes==0,"releasing the bracket releases its own buffer charge");

    Prepare(device.Get(),context.Get(),texture.Get(),td,view.Get());context->VSSetShader(nullptr,nullptr,0);
    BeforeRaster(context.Get(),{1,1});
    Check(Full().refusal==Refusal::MissingVertex && !Full().view && !Full().source && !Full().depth &&
          !Full().constantContext && !Full().retention.count && !i.semanticComplete &&
          i.firstUnprovedReason==unsigned(Refusal::MissingVertex),
          "rejected capture preserves the compact failure and releases every partial owner");
    context->VSSetShader(vs.Get(),nullptr,0);

    D3D11_TEXTURE2D_DESC colorDesc=td;colorDesc.Format=DXGI_FORMAT_R32_FLOAT;
    colorDesc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> color;ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11UnorderedAccessView> uav;
    Check(SUCCEEDED(device->CreateTexture2D(&colorDesc,nullptr,&color)),"create tracked nondepth output");
    Check(SUCCEEDED(device->CreateRenderTargetView(color.Get(),nullptr,&rtv)),"create RTV output");
    Check(SUCCEEDED(device->CreateUnorderedAccessView(color.Get(),nullptr,&uav)),"create UAV output");
    for(unsigned mode=1;mode<=3;++mode){
        context->OMSetRenderTargets(0,nullptr,view.Get());
        Prepare(device.Get(),context.Get(),texture.Get(),td,view.Get());
        auto& output=detail::state.tracks[1].interval;output.resource=color;output.texture=colorDesc;
        output.texture.BindFlags=(mode&1?D3D11_BIND_RENDER_TARGET:0)|(mode&2?D3D11_BIND_UNORDERED_ACCESS:0);
        if(mode==1){auto* raw=rtv.Get();context->OMSetRenderTargets(1,&raw,view.Get());}
        else{auto* raw=uav.Get();context->OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,view.Get(),0,1,&raw,nullptr);}
        BeforeRaster(context.Get(),{1,1});
        Check(detail::state.pending && Full().view.Get()==view.Get() && retired==1 && !output.resource,
              "combined RTV/UAV getter retains the same DSV and retires only the actual nondepth writer");
    }
    context->ClearState();detail::state={};GenshinDepthMapping::CurrentDraw()={};
    std::printf("Returned raster capture: %u WARP/CPU checks passed; binding and construction proofs only\n",checks);
}
'''.replace('// PRODUCTION_TYPES', types).replace('// PRODUCTION_RASTER', raster)
fixture = fixture.replace('// PRODUCTION_UNPROVED', definition('inline void Unproved('))
fixture = fixture.replace('// PRODUCTION_DEPTH', depth).replace('// PRODUCTION_HELPERS', helpers)
fixture = fixture.replace('// PRODUCTION_BEFORE', before)

with tempfile.TemporaryDirectory(prefix='returned-capture-', dir=ROOT / 'build', ignore_cleanup_errors=True) as directory:
    out = Path(directory)
    (out / 'test.cpp').write_text(fixture, encoding='utf-8')
    command = (f'cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /I"{ROOT / "OptiScaler"}" '
               f'/Fe:"{out / "test.exe"}" /Fo:"{out}/" "{out / "test.cpp"}" /link d3d11.lib d3dcompiler.lib')
    batch = out / 'compile.cmd'
    batch.write_text('@echo off\ncall "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64\n'
                     'if errorlevel 1 exit /b %errorlevel%\n' + command + '\nexit /b %errorlevel%\n',
                     encoding='utf-8', newline='\r\n')
    subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=out, check=True)
    subprocess.run([str(out / 'test.exe')], cwd=out, check=True)
