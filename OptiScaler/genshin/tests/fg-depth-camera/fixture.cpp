// Audit-only fixture. No product entry point, injection, menu or configuration changes.
#include <windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "../../GenshinFgGameMotion_Hlsl.h"
#include "../../../native/F5LowShaderBytecode.h"
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0,failures=0;
void check(bool pass,const char* what){++checks;if(!pass){++failures;if(failures<20)printf("FAIL %s\n",what);}}
void hr(HRESULT result,const char* what){if(FAILED(result)){printf("SETUP_FAIL %s %08lx\n",what,(unsigned long)result);ExitProcess(2);}}
void bytes(const std::string& path,const void* p,size_t n){std::ofstream f(path,std::ios::binary);f.write((const char*)p,n);if(!f)ExitProcess(3);}
struct V2{float x=0,y=0;};
struct V4{float x=0,y=0,z=0,w=0;};
struct H2{uint16_t x,y;};
struct H4{uint16_t x,y,z,w;};
float halfRound(float x){return DirectX::PackedVector::XMConvertHalfToFloat(DirectX::PackedVector::XMConvertFloatToHalf(x));}
struct Matrix{float v[16]{};Matrix(){for(unsigned i=0;i<4;++i)v[i*5]=1;}};
struct Control{
    unsigned w,h,rw,rh;V2 current,previous;float clear;unsigned reversed,pad0=0,pad1=0;
    V2 inverse,reserved;Matrix clipToWorld,previousClip;
};
static_assert(sizeof(Control)==192);
struct Texture{ComPtr<ID3D11Texture2D> resource;ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11UnorderedAccessView> uav;};
ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
Texture texture(unsigned w,unsigned h,DXGI_FORMAT format,const void* data,unsigned stride,bool output){
    Texture t;D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=format;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|(output?D3D11_BIND_UNORDERED_ACCESS:0);
    D3D11_SUBRESOURCE_DATA initial{data,w*stride,0};hr(device->CreateTexture2D(&d,data?&initial:nullptr,&t.resource),"texture");
    hr(device->CreateShaderResourceView(t.resource.Get(),nullptr,&t.srv),"srv");
    if(output)hr(device->CreateUnorderedAccessView(t.resource.Get(),nullptr,&t.uav),"uav");return t;
}
template<class T>std::vector<T> read(const Texture& t){
    D3D11_TEXTURE2D_DESC d{};t.resource->GetDesc(&d);d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;hr(device->CreateTexture2D(&d,nullptr,&staging),"staging");context->CopyResource(staging.Get(),t.resource.Get());
    D3D11_MAPPED_SUBRESOURCE m{};hr(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&m),"map");std::vector<T> result(d.Width*d.Height);
    for(unsigned y=0;y<d.Height;++y)memcpy(result.data()+y*d.Width,(char*)m.pData+y*m.RowPitch,d.Width*sizeof(T));
    context->Unmap(staging.Get(),0);return result;
}
template<class T>std::vector<T> flip(const std::vector<T>& src,unsigned w,unsigned h){
    auto dst=src;for(unsigned y=0;y<h;++y)std::copy_n(src.data()+y*w,w,dst.data()+(h-1-y)*w);return dst;
}
bool valid(float z,const Control& c){return std::isfinite(z)&&z>=0&&z<=1&&z!=c.clear;}
struct Result{std::vector<float> depth;std::vector<V4> motion;};
Result run(const Control& c,const std::vector<float>& depth,const std::vector<V2>& motion,const std::vector<float>& previous,ID3D11ComputeShader* shader){
    auto d=texture(c.rw,c.rh,DXGI_FORMAT_R32_FLOAT,depth.data(),4,false);
    std::vector<H2> packed;for(auto v:motion)packed.push_back({DirectX::PackedVector::XMConvertFloatToHalf(v.x),DirectX::PackedVector::XMConvertFloatToHalf(v.y)});
    auto m=texture(c.rw,c.rh,DXGI_FORMAT_R16G16_FLOAT,packed.data(),4,false);
    auto p=texture(c.rw,c.rh,DXGI_FORMAT_R32_FLOAT,previous.data(),4,false);
    auto od=texture(c.w,c.h,DXGI_FORMAT_R32_FLOAT,nullptr,4,true);
    auto om=texture(c.w,c.h,DXGI_FORMAT_R16G16B16A16_FLOAT,nullptr,8,true);
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(c);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA data{&c,0,0};ComPtr<ID3D11Buffer> cb;hr(device->CreateBuffer(&bd,&data,&cb),"cb");
    ID3D11ShaderResourceView* views[]{m.srv.Get(),d.srv.Get(),p.srv.Get()};
    ID3D11UnorderedAccessView* outputs[]{om.uav.Get(),od.uav.Get()};auto* raw=cb.Get();
    context->CSSetShader(shader,nullptr,0);context->CSSetShaderResources(0,3,views);context->CSSetUnorderedAccessViews(0,2,outputs,nullptr);
    context->CSSetConstantBuffers(0,1,&raw);context->Dispatch((c.w+7)/8,(c.h+7)/8,1);
    ID3D11ShaderResourceView* emptyV[3]{};ID3D11UnorderedAccessView* emptyU[2]{};
    context->CSSetShaderResources(0,3,emptyV);context->CSSetUnorderedAccessViews(0,2,emptyU,nullptr);
    std::vector<V4> unpacked;for(auto packedOutput:read<H4>(om))unpacked.push_back({DirectX::PackedVector::XMConvertHalfToFloat(packedOutput.x),DirectX::PackedVector::XMConvertHalfToFloat(packedOutput.y),DirectX::PackedVector::XMConvertHalfToFloat(packedOutput.z),DirectX::PackedVector::XMConvertHalfToFloat(packedOutput.w)});
    return {read<float>(od),unpacked};
}
// Analytic moving sloped plane. The native texture coordinate and raster
// coordinate are deliberately distinct: currentCorrection=CB28+Jaccepted.
// Here Jaccepted=0 and previousCorrection=0, so the R850-preimage ray is exact
// and the withdrawn R850 compensation is exact-only when CB28 is also zero.
V4 transform(V4 p,const Matrix& m){
    return {p.x*m.v[0]+p.y*m.v[4]+p.z*m.v[8]+p.w*m.v[12],
            p.x*m.v[1]+p.y*m.v[5]+p.z*m.v[9]+p.w*m.v[13],
            p.x*m.v[2]+p.y*m.v[6]+p.z*m.v[10]+p.w*m.v[14],
            p.x*m.v[3]+p.y*m.v[7]+p.z*m.v[11]+p.w*m.v[15]};
}
bool sentinel(V4 p){return p.x==65504&&p.y==65504;}
bool motionEqual(V4 a,V4 b){
    const auto close=[](float x,float y){const float m=(std::max)(fabsf(x),fabsf(y));if(m<1.e-6f)return fabsf(x-y)<.001f;
        int e=0;frexpf(m,&e);return fabsf(x-y)<=ldexpf(1.f,e-11)+.001f;}; // one FP16 ULP at the consumer magnitude
    return close(a.x,b.x)&&close(a.y,b.y);
}
Matrix store(DirectX::FXMMATRIX m){DirectX::XMFLOAT4X4 v;DirectX::XMStoreFloat4x4(&v,m);Matrix out;memcpy(out.v,&v,64);return out;}
V4 divided(V4 p){return {p.x/p.w,p.y/p.w,p.z/p.w,1};}
Matrix r850AdaptForAbOnly(const Matrix& raster,float cx,float cy){
    DirectX::XMFLOAT4X4 input;memcpy(&input,raster.v,64);
    return store(DirectX::XMMatrixMultiply(DirectX::XMMatrixTranslation(2*cx,-2*cy,0),DirectX::XMLoadFloat4x4(&input)));
}
void perspectiveCases(ID3D11ComputeShader* source,ID3D11ComputeShader* oldEmbedded){
    using namespace DirectX;
    unsigned r850Rejected=0,validInterior=0,rayMismatch=0,signedVectors=0,baselineDepthRejects=0;
    for(unsigned reversed=0;reversed<2;++reversed)for(unsigned frame=0;frame<4;++frame){
        Control c{};c.w=128;c.h=64;c.rw=64;c.rh=32;c.inverse={1.f/c.w,1.f/c.h};
        c.clear=reversed?0.f:1.f;c.reversed=reversed;
        c.current={.5f/c.rw,-.25f/c.rh};c.previous={0,0};
        const auto projection=XMMatrixPerspectiveFovRH(1.2f,2.f,reversed?20.f:1.f,reversed?1.f:20.f);
        const float yaw=(frame&1?-.45f:.45f),pitch=(frame&2?-.25f:.25f);
        const auto camera=XMMatrixRotationX(pitch)*XMMatrixRotationY(yaw)*XMMatrixTranslation(.04f,-.03f,.02f);
        const auto inverse=store(XMMatrixInverse(nullptr,XMMatrixInverse(nullptr,camera)*projection));
        c.clipToWorld=inverse;c.previousClip=store(projection);
        const auto planeClip=transform({0,0,-2,1},c.previousClip);
        const float oldZ=planeClip.z/planeClip.w;
        std::vector<float> depth(c.rw*c.rh),previous(depth.size(),oldZ);std::vector<V2> motion(depth.size());
        for(unsigned y=0;y<c.rh;++y)for(unsigned x=0;x<c.rw;++x){
            // nativeUv is the prepared-texture/history coordinate. The raw
            // depth texel's camera ray is nativeUv-CB28, not nativeUv.
            const float nu=(x+.5f)/c.rw,nv=(y+.5f)/c.rh;
            const float u=nu-c.current.x,v=nv-c.current.y;
            const auto a=divided(transform({2*u-1,1-2*v,0,1},inverse));
            const auto b=divided(transform({2*u-1,1-2*v,1,1},inverse));
            const float time=(-2-a.z)/(b.z-a.z);
            const V4 world{a.x+time*(b.x-a.x),a.y+time*(b.y-a.y),-2,1};
            const auto currentClip=transform(world,store(XMMatrixInverse(nullptr,camera)*projection));
            depth[y*c.rw+x]=currentClip.z/currentClip.w;
            const auto prev=divided(transform(world,c.previousClip));
            const float previousU=(prev.x+1)*.5f,previousV=(1-prev.y)*.5f;
            motion[y*c.rw+x]={halfRound(previousU-nu),halfRound(previousV-nv)};
        }
        c.clipToWorld=inverse;
        const auto baseline=run(c,depth,motion,previous,oldEmbedded);
        const auto fixed=run(c,depth,motion,previous,source);
        c.clipToWorld=r850AdaptForAbOnly(inverse,c.current.x,c.current.y);
        const auto r850Old=run(c,depth,motion,previous,oldEmbedded);
        const auto r850New=run(c,depth,motion,previous,source);
        unsigned count=0,rejected=0;
        for(unsigned y=0;y<c.h;++y)for(unsigned x=0;x<c.w;++x){
            const unsigned at=y*c.w+x;
            check(fixed.depth[at]==baseline.depth[at]&&fixed.depth[at]==r850New.depth[at],"R847 depth rows and donor unchanged");
            check(sentinel(r850Old.motion[at])==sentinel(r850New.motion[at]),"Y-only source change does not alter R850 A/B admission");
            const float u=(x+.5f)/c.w,v=1-(y+.5f)/c.h;
            const int bx=int(floorf((u+c.current.x)*c.rw)),by=int(floorf((v+c.current.y)*c.rh));
            constexpr int ox[]{0,-1,1,0,0},oy[]{0,0,0,-1,1};
            int donor=-1;float z=reversed?0.f:1.f;
            for(unsigned k=0;k<5;++k){int sx=bx+ox[k],sy=by+oy[k];if(sx<0||sy<0||sx>=int(c.rw)||sy>=int(c.rh))continue;
                const int i=sy*int(c.rw)+sx;if(!valid(depth[i],c))continue;
                if(donor<0||(reversed?depth[i]>z:depth[i]<z)){donor=i;z=depth[i];}}
            if(donor<0)continue;
            const auto mv=motion[donor];
            const V2 vs{mv.x+c.current.x-c.previous.x,mv.y+c.current.y-c.previous.y};
            const float pu=u+vs.x,pv=v+vs.y;
            const float nu=(donor%c.rw+.5f)/c.rw,nv=(donor/c.rw+.5f)/c.rh;
            const float du=nu+mv.x,dv=nv+mv.y;
            if(pu<0||pu>=1||pv<0||pv>=1||du<0||du>=1||dv<0||dv>=1)continue;
            if(sentinel(baseline.motion[at])){check(sentinel(fixed.motion[at]),"Y-only source change preserves baseline depth rejection");++baselineDepthRejects;continue;}
            ++count;
            const V4 expected{halfRound(vs.x*c.w),halfRound(-vs.y*c.h),0,0};
            const V4 oldExpected{halfRound(vs.x*c.w),halfRound(vs.y*c.h),0,0};
            check(motionEqual(fixed.motion[at],expected),"DLSSG VelocityFlip equivalent negates only final vector Y");
            check(motionEqual(baseline.motion[at],oldExpected),"R849 embedded baseline retained for A/B");
            if(fabsf(expected.y)>0.01f)++signedVectors;
            if(sentinel(r850New.motion[at]))++rejected;
            else check(motionEqual(r850New.motion[at],expected),"R850 A/B retains consumer sign when admission survives");
            const float rasterU=nu-c.current.x,rasterV=nv-c.current.y;
            const auto physical=divided(transform({2*rasterU-1,1-2*rasterV,z,1},inverse));
            const auto preimage=divided(transform({2*rasterU-1,1-2*rasterV,z,1},inverse));
            const auto adapted=divided(transform({2*rasterU-1,1-2*rasterV,z,1},r850AdaptForAbOnly(inverse,c.current.x,c.current.y)));
            check(fabsf(physical.x-preimage.x)<1.e-6f&&fabsf(physical.y-preimage.y)<1.e-6f&&fabsf(physical.z-preimage.z)<1.e-6f,"R850 preimage uses nativeUv-CB28 raster ray");
            const bool different=fabsf(physical.x-adapted.x)>1.e-5f||fabsf(physical.y-adapted.y)>1.e-5f||fabsf(physical.z-adapted.z)>1.e-5f;
            check(different,"R850 compensation incorrectly uses nativeUv as rasterUv");if(different)++rayMismatch;
        }
        check(count>1500,"substantial perspective-camera interior coverage");
        c.clipToWorld=inverse;
        auto missing=previous;for(auto& z:missing)z=c.clear;
        const auto noHistory=run(c,depth,motion,missing,source);
        check(std::all_of(noHistory.motion.begin(),noHistory.motion.end(),sentinel),"missing history stays rejected");
        auto occluded=previous;for(auto& z:occluded)z+=reversed?-.1f:.1f;
        const auto wrongSurface=run(c,depth,motion,occluded,source);
        check(std::all_of(wrongSurface.motion.begin(),wrongSurface.motion.end(),sentinel),"wrong previous surface stays rejected");
        r850Rejected+=rejected;validInterior+=count;
        printf("PERSPECTIVE reversed=%u frame=%u interior=%u r850_false_rejections=%u\n",reversed,frame,count,rejected);
    }
    check(signedVectors>12000,"substantial signed nonzero-Y consumer witnesses");
    check(r850Rejected>1000,"R850 camera compensation reproduced false motion rejection");
    check(rayMismatch>12000,"R850 camera compensation reproduced physical ray mismatch");
    printf("WITNESSES valid=%u signed_y=%u r850_false_rejections=%u ray_mismatch=%u baseline_depth_rejects=%u\n",validInterior,signedVectors,r850Rejected,rayMismatch,baselineDepthRejects);
}
}
int main(int argc,char** argv){
    if(argc!=3)return 4;std::string dir=argv[1];bool warp=std::string(argv[2])=="warp";
    ComPtr<ID3DBlob> compiled,messages;
    hr(D3DCompile(GenshinFgGameMotionHlsl::Source,strlen(GenshinFgGameMotionHlsl::Source),"GenshinGameMotion",nullptr,nullptr,"NormalizeNative","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&compiled,&messages),"compile actual recipe");
    auto* embedded=F5LowShaderBytecode::Find("GenshinGameMotion","NormalizeNative");check(embedded!=nullptr,"embedded entry");if(!embedded)return 5;
    check(embedded->size==compiled->GetBufferSize()&&!memcmp(embedded->code,compiled->GetBufferPointer(),embedded->size),"actual embedded matches formal source");
    uint64_t hash=14695981039346656037ull;for(const char* p=GenshinFgGameMotionHlsl::Source;*p;++p){hash^=(unsigned char)*p;hash*=1099511628211ull;}
    check(F5LowShaderBytecode::GameMotionSourceMatches(hash),"actual source freshness guard");
    bytes(dir+"/source.dxbc",compiled->GetBufferPointer(),compiled->GetBufferSize());bytes(dir+"/actual-embedded.dxbc",embedded->code,embedded->size);
    ComPtr<ID3DBlob> disasm;hr(D3DDisassemble(compiled->GetBufferPointer(),compiled->GetBufferSize(),0,nullptr,&disasm),"disassemble");bytes(dir+"/reflection.asm",disasm->GetBufferPointer(),disasm->GetBufferSize());
    ComPtr<ID3D11ShaderReflection> reflect;hr(D3DReflect(compiled->GetBufferPointer(),compiled->GetBufferSize(),IID_PPV_ARGS(&reflect)),"reflect");
    D3D11_SHADER_BUFFER_DESC cbd{};hr(reflect->GetConstantBufferByName("Control")->GetDesc(&cbd),"control reflection");check(cbd.Size==sizeof(Control),"Control192");
    const char* names[]{"pictureSize","producerSize","currentCorrection","previousCorrection","clearValue","reversedZ","inversePictureSize","currentClipToWorld","previousWorldToClip"};
    const unsigned offsets[]{0,8,16,24,32,36,48,64,128};
    for(unsigned i=0;i<9;++i){D3D11_SHADER_VARIABLE_DESC v{};hr(reflect->GetConstantBufferByName("Control")->GetVariableByName(names[i])->GetDesc(&v),"variable reflection");check(v.StartOffset==offsets[i],"CB variable offset");printf("REFLECTION %s offset=%u size=%u\n",names[i],v.StartOffset,v.Size);}
    D3D_FEATURE_LEVEL level{};hr(D3D11CreateDevice(nullptr,warp?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context),"debug device");
    ComPtr<IDXGIDevice> dxgi;device.As(&dxgi);ComPtr<IDXGIAdapter> adapter;dxgi->GetAdapter(&adapter);DXGI_ADAPTER_DESC desc{};adapter->GetDesc(&desc);
    char adapterName[256]{};WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,adapterName,sizeof(adapterName),nullptr,nullptr);printf("ADAPTER %s mode=%s\n",adapterName,warp?"warp":"hardware");
    std::ifstream oldFile(dir+"/r849-baseline.dxbc",std::ios::binary|std::ios::ate);check(oldFile.good(),"pinned R849 baseline readable");
    const auto oldSize=oldFile.tellg();oldFile.seekg(0);std::vector<char> oldBytes(static_cast<size_t>(oldSize));oldFile.read(oldBytes.data(),oldSize);
    ComPtr<ID3D11ComputeShader> a,b;hr(device->CreateComputeShader(compiled->GetBufferPointer(),compiled->GetBufferSize(),nullptr,&a),"source shader");hr(device->CreateComputeShader(oldBytes.data(),oldBytes.size(),nullptr,&b),"pinned R849 baseline shader");
    perspectiveCases(a.Get(),b.Get());
    context->ClearState();context->Flush();ComPtr<ID3D11InfoQueue> info;hr(device.As(&info),"debug queue");unsigned errors=0;
    for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T n=0;info->GetMessage(i,nullptr,&n);std::vector<char> buf(n);auto* message=(D3D11_MESSAGE*)buf.data();info->GetMessage(i,message,&n);
        if(message->Severity<=D3D11_MESSAGE_SEVERITY_ERROR){++errors;printf("D3D_ERROR %s\n",message->pDescription);}}
    check(errors==0,"debug layer zero errors");printf("RESULT checks=%u failures=%u debug_errors=%u source_fnv=%llu embedded=old-R849-AB-only\n",checks,failures,errors,(unsigned long long)hash);
    return failures?1:0;
}
