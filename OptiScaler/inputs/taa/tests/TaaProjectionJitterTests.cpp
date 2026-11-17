#include <inputs/taa/TaaAutoDx11.h>
#include <inputs/taa/TaaPassDx11.h>
#include "TaaProjectionFixture.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <thread>

using namespace taa;
namespace {
unsigned checks=0;
void Check(bool ok,const char* text) {++checks;if(!ok)throw std::runtime_error(text);}
void Hr(HRESULT hr) {Check(SUCCEEDED(hr),"WARP API call");}
std::vector<unsigned char> Bytes(const std::filesystem::path& path) {
    std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};
}
void Captured(const std::filesystem::path& root) {
    const auto ps=Bytes(root/"PS-137B32A04AB724F0.dxbc"),vs=Bytes(root/"VS-F9C1F953EE80980A.dxbc");
    Check(jitter::Identify(ps.data(),ps.size())==jitter::Program::MotionPixel,"content identity of actual captured motion PS");
    Check(jitter::Identify(vs.data(),vs.size())==jitter::Program::MotionVertex,"content identity of actual captured ray VS");
    auto corrupt=ps;corrupt.back()^=1;
    Check(jitter::Identify(corrupt.data(),corrupt.size())==jitter::Program::None,"one changed DXBC byte cannot reuse a verified layout");
    automatic::Program p(ps.data(),ps.size());
    p.Analyze(Stage::Pixel,0x137B32A04AB724F0ull,[](auto&&,auto&&,const auto&){return true;});
    Check(p.projection==jitter::Program::MotionPixel,"layout survives release of diagnostic bytecode");
    unsigned valid=0,unknown=0;
    for(const auto& entry:std::filesystem::directory_iterator(root/"temporal")) {
        const auto name=entry.path().filename().string();if(!name.starts_with("PS-137B32A04AB724F0-")||!name.ends_with(".constants.bin"))continue;
        const auto bytes=Bytes(entry.path());Check(bytes.size()>=1184,"real PS buffer has bytes and known mask");
        if(std::find(bytes.begin()+592,bytes.begin()+1184,0)!=bytes.begin()+1184){++unknown;continue;}
        std::array<float,148> cb{};std::memcpy(cb.data(),bytes.data(),sizeof(cb));
        const auto result=jitter::Projection(cb,2560,1600);Check(bool(result),"captured same-camera projection contract");
        Check(std::abs(result->x+cb[52]/cb[40]*1280)<1e-6&&std::abs(result->y-cb[53]/cb[45]*800)<1e-6,"exact render-pixel sign of real projection offsets");++valid;
    }
    Check(valid==30&&unknown==1,"all fixed-run known samples accepted and the unknown sample excluded");
    std::printf("PASS captured matrices: evidence=%s known=%u unknown=%u\n",root.string().c_str(),valid,unknown);
}
}
int main(int argc,char** argv) {
    try {
        // Exact float matrices from PID46068 owner3551 (AE camera motion).
        std::array<float,552> packed{};
        const float inverse[]{0.6470582485f,0.000000000f,0.000000000f,0.000000000f,0.000000000f,-0.4044114053f,0.000000000f,0.000000000f,301.0989685f,18.64962769f,155.6968842f,9.999799728f,0.006157339085f,0.0001749410731f,1.003118157f,0.0002002675901f},current[]{1.545455933f,0.000000000f,0.000000000f,0.000000000f,0.000000000f,-2.472729445f,0.000000000f,0.000000000f,0.000000000f,0.000000000f,-0.00002002716064f,1.000000000f,-46.53445053f,4.611640453f,0.1003138274f,-15.56999969f};
        std::copy(std::begin(inverse),std::end(inverse),packed.begin()+134*4);
        std::copy(std::begin(current),std::end(current),packed.begin()+118*4);
        auto contract=jitter::PackedProjection(packed,2544,1584);
        Check(contract&&contract->depthInverted&&std::abs(contract->offset.x+.25f)<.001f&&std::abs(contract->offset.y-.3888889f)<.001f,"real packed camera inverse proves reverse depth and NGX jitter");
        auto badPacked=packed;badPacked[118*4]+=.1f;Check(!jitter::PackedProjection(badPacked,2544,1584),"different camera projection cannot borrow inverse");
        badPacked=packed;badPacked[137*4]+=.01f;Check(!jitter::PackedProjection(badPacked,2544,1584),"arbitrary asymmetric projection exceeds supported jitter");
        badPacked=packed;badPacked[120*4+2]=std::numeric_limits<float>::quiet_NaN();Check(!jitter::PackedProjection(badPacked,2544,1584),"nonfinite packed matrix rejected");
        jitter::PackedProjectionDiagnostics diagnosis;
        Check(bool(jitter::PackedProjection(packed,2544,1584,&diagnosis))&&diagnosis.examined&&
              std::string(diagnosis.reason)=="accepted"&&diagnosis.depthA<0&&diagnosis.depthB>0,
              "accepted packed projection reports the evaluated depth contract");
        const auto rejectedAs=[&](const auto& values,unsigned width,unsigned height,const char* reason) {
            Check(!jitter::PackedProjection(values,width,height,&diagnosis)&&diagnosis.examined&&
                  std::string(diagnosis.reason)==reason,"packed failure reports its exact evaluated predicate");
        };
        rejectedAs(packed,0,1584,"empty extent");
        badPacked=packed;badPacked[120*4+2]=std::numeric_limits<float>::quiet_NaN();
        rejectedAs(badPacked,2544,1584,"nonfinite matrix");
        badPacked=packed;badPacked[119*4]=.01f;
        rejectedAs(badPacked,2544,1584,"nonorthogonal spatial columns");
        badPacked=packed;badPacked[118*4]=0;
        rejectedAs(badPacked,2544,1584,"invalid spatial column norm");
        badPacked=packed;badPacked[118*4+2]=.01f;
        rejectedAs(badPacked,2544,1584,"depth column not parallel to W");
        badPacked=packed;badPacked[121*4+2]=-.1f;
        rejectedAs(badPacked,2544,1584,"unsupported depth convention");
        badPacked=packed;badPacked[134*4]+=.1f;
        rejectedAs(badPacked,2544,1584,"inverse product mismatch");
        badPacked=packed;badPacked[137*4]+=.01f;
        rejectedAs(badPacked,2544,1584,"jitter outside supported range");
        auto cb=test::ProjectionConstants(16,16);auto offset=jitter::Projection(cb,16,16);
        Check(offset&&offset->x==.25f&&offset->y==-.125f,"projection shift converts to NGX render-pixel convention");
        auto invalid=cb;invalid[72]+=.1f;Check(!jitter::Projection(invalid,16,16),"different projection scale rejected");
        invalid=cb;invalid[74]=.1f;Check(!jitter::Projection(invalid,16,16),"nonrigid view rejected");
        invalid=cb;invalid[20]=.1f;Check(!jitter::Projection(invalid,16,16),"different camera eye rejected");
        invalid=cb;invalid[41]=.1f;Check(!jitter::Projection(invalid,16,16),"unverified inverse layout rejected");
        invalid=cb;invalid[72+2*4]=.01f;Check(!jitter::Projection(invalid,16,16),"nonzero motion projection center rejected");
        Check(!jitter::Projection(cb,32,16),"current buffer extent must match the actual motion target");
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        const auto texture=[&](DXGI_FORMAT format,UINT bind){D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=16;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=format;d.BindFlags=bind;ComPtr<ID3D11Texture2D> t;Hr(device->CreateTexture2D(&d,nullptr,&t));return t;};
        auto motion=texture(DXGI_FORMAT_R16G16_FLOAT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE);
        auto depth=texture(DXGI_FORMAT_R32_TYPELESS,D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE);
        auto color=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_SHADER_RESOURCE);
        auto history=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_SHADER_RESOURCE);
        auto target=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_RENDER_TARGET);
        ComPtr<ID3D11RenderTargetView> motionRt,targetRt;ComPtr<ID3D11DepthStencilView> depthView;
        Hr(device->CreateRenderTargetView(motion.Get(),nullptr,&motionRt));Hr(device->CreateRenderTargetView(target.Get(),nullptr,&targetRt));
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};dd.Format=DXGI_FORMAT_D32_FLOAT;dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        Hr(device->CreateDepthStencilView(depth.Get(),&dd,&depthView));
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32_FLOAT;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
        std::array<ComPtr<ID3D11ShaderResourceView>,4> views;
        Hr(device->CreateShaderResourceView(depth.Get(),&sd,&views[0]));Hr(device->CreateShaderResourceView(motion.Get(),nullptr,&views[1]));
        Hr(device->CreateShaderResourceView(color.Get(),nullptr,&views[2]));Hr(device->CreateShaderResourceView(history.Get(),nullptr,&views[3]));
        ComPtr<ID3D11Buffer> buffer;D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(cb);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.Usage=D3D11_USAGE_DYNAMIC;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        Hr(device->CreateBuffer(&bd,nullptr,&buffer));constants::Observe(buffer.Get());
        const auto write=[&]{D3D11_MAPPED_SUBRESOURCE mapped{};const auto hr=context->Map(buffer.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped);Hr(hr);constants::Mapped(context.Get(),buffer.Get(),0,D3D11_MAP_WRITE_DISCARD,&mapped,hr);std::memcpy(mapped.pData,cb.data(),sizeof(cb));constants::BeforeUnmap(context.Get(),buffer.Get(),0);context->Unmap(buffer.Get(),0);constants::AfterUnmap(context.Get(),buffer.Get(),0);};
        const auto bindMotion=[&]{context->ClearState();ID3D11RenderTargetView* rt=motionRt.Get();context->OMSetRenderTargets(1,&rt,depthView.Get());ID3D11Buffer* b=buffer.Get();context->PSSetConstantBuffers(0,1,&b);D3D11_VIEWPORT v{0,0,16,16,0,1};context->RSSetViewports(1,&v);};
        jitter::Tracker tracker;const DrawObservationDx11::Consumer consumer{};DrawObservationDx11::Subscribe(DrawObservationDx11::Slot::TaaJitter,&consumer,true);
        const auto next=[&]{DrawObservationDx11::Present(context.Get());bindMotion();};
        const DrawObservationDx11::Draw draw{DrawObservationDx11::Kind::Indexed,3};
        const auto before=[&]{return tracker.Before(context.Get(),draw,*DrawObservationDx11::owner,7,jitter::Program::MotionPixel,jitter::Program::MotionVertex);};
        next();Check(!before()&&!tracker.Current(),"unobserved first-frame constants remain unavailable");
        next();write();auto cookie=before();Check(cookie&&!tracker.Current(),"sample is unavailable until original command returns");
        tracker.After(cookie,DrawObservationDx11::Completion::Consumed,7);Check(!tracker.Current(),"GIMI-consumed producer cannot publish jitter");
        next();cookie=before();tracker.After(cookie,DrawObservationDx11::Completion::OriginalReturned,8);Check(!tracker.Current(),"request revision change drops pending source");
        next();cookie=before();tracker.After(cookie,DrawObservationDx11::Completion::OriginalReturned,7);
        Check(tracker.Current()&&tracker.Current()->Matches(context.Get(),7,motion.Get(),depth.Get(),16,16,DXGI_FORMAT_R16G16_FLOAT),"current complete same-owner projection sample");
        Check(!tracker.Current()->Matches(context.Get(),8,motion.Get(),depth.Get(),16,16,DXGI_FORMAT_R16G16_FLOAT),"wrong request revision rejected");
        Check(!tracker.Current()->Matches(context.Get(),7,target.Get(),depth.Get(),16,16,DXGI_FORMAT_R16G16_FLOAT),"same-sized different motion texture rejected");
        Check(!tracker.Current()->Matches(context.Get(),7,motion.Get(),target.Get(),16,16,DXGI_FORMAT_R16G16_FLOAT),"different native depth camera rejected");
        Check(!tracker.Current()->Matches(context.Get(),7,motion.Get(),depth.Get(),16,16,DXGI_FORMAT_R16G16_UNORM),"different typed motion view rejected");
        std::thread other([&]{Check(!tracker.Current()->Matches(context.Get(),7,motion.Get(),depth.Get(),16,16,DXGI_FORMAT_R16G16_FLOAT),"other thread cannot borrow a sample");});other.join();
        // Resolve and the real Pass both consume the same typed sample, while
        // a normal CB profile keeps its own independent path.
        context->ClearState();ID3D11RenderTargetView* rt=targetRt.Get();context->OMSetRenderTargets(1,&rt,nullptr);
        std::array<ID3D11ShaderResourceView*,4> raw{};for(unsigned i=0;i<4;++i)raw[i]=views[i].Get();context->PSSetShaderResources(0,4,raw.data());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);D3D11_VIEWPORT viewport{0,0,16,16,0,1};context->RSSetViewports(1,&viewport);
        ComPtr<ID3D11DepthStencilState> noDepth;D3D11_DEPTH_STENCIL_DESC nd{};Hr(device->CreateDepthStencilState(&nd,&noDepth));context->OMSetDepthStencilState(noDepth.Get(),0);
        automatic::DepthCleared(context.Get(),depthView.Get(),D3D11_CLEAR_DEPTH,1);
        analysis::Result shader;analysis::Recipe recipe;recipe.profile.colorSlot=2;recipe.profile.motionSlot=1;recipe.historySlot=3;shader.recipes.push_back(recipe);shader.textures={0,1,2,3};
        const jitter::Input input{tracker.Current(),7};auto resolved=automatic::Resolve(context.Get(),Stage::Pixel,shader,input);
        Check(resolved.profile&&resolved.profile->jitterSource==JitterSource::MotionProjection,"automatic temporal roles accept the proven live projection source");
        PassDx11 pass(device.Get(),context.Get());const Invocation call{Stage::Pixel,3};
        Check(pass.Probe(*resolved.profile,call,input),pass.Error().c_str());
        Check(!pass.Probe(*resolved.profile,call),"projection profile cannot fall back to missing CB jitter");
        Check(pass.Begin(*resolved.profile,call,true,input),pass.Error().c_str());
        Check(!pass.Finish([&](const Frame& frame){Check(frame.jitterX==.25f&&frame.jitterY==-.125f,"Begin freezes measured jitter through the transaction");return false;}),"no fabricated provider output authorizes copyback");
        context->Flush();for(unsigned i=0;!pass.Idle()&&i<10000;++i)Sleep(1);Check(pass.Idle(),"slot retires through its actual GPU event");
        Check(!before()&&!tracker.Current(),"multiple same-frame cameras invalidate the first source");
        next();cookie=before();tracker.After(cookie,DrawObservationDx11::Completion::OriginalReturned,7);Check(bool(tracker.Current()),"next frame recovers without cached floats");
        constants::OpaqueWrite();Check(!tracker.Current()->Matches(context.Get(),7,motion.Get(),depth.Get(),16,16,DXGI_FORMAT_R16G16_FLOAT),"opaque writes cannot authorize a recorded sample");
        next();write();cookie=before();tracker.After(cookie,DrawObservationDx11::Completion::OriginalReturned,7);next();
        Check(!tracker.Current()->Matches(context.Get(),7,motion.Get(),depth.Get(),16,16,DXGI_FORMAT_R16G16_FLOAT),"previous-frame projection cannot be replayed");
        if(argc>1)Captured(argv[1]);
        std::printf("PASS TAA projection math/current-frame/source/transaction: %u checks\n",checks);
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
