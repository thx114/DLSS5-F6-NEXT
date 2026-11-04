// Standalone D3D11 fixture: real HLSL execution, never a product DLL build.
#include <windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>
#include "../../GenshinFgGameMotion_Hlsl.h"
#define GenshinFgGameMotionHlsl R852GameMotionHlsl
#include "R852_GenshinFgGameMotion_Hlsl.h"
#undef GenshinFgGameMotionHlsl
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0,failures=0;
void check(bool pass,const char* name){++checks;if(!pass){++failures;if(failures<20)printf("FAIL %s\n",name);}}
void hr(HRESULT result,const char* name){if(FAILED(result)){printf("SETUP_FAIL %s %08lx\n",name,(unsigned long)result);ExitProcess(2);}}
struct V2{float x=0,y=0;};
struct V4{float x=0,y=0,z=0,w=1;};
struct H2{uint16_t x,y;};
struct H4{uint16_t x,y,z,w;};
float half(float x){return DirectX::PackedVector::XMConvertHalfToFloat(DirectX::PackedVector::XMConvertFloatToHalf(x));}
struct Matrix{float v[16]{};Matrix(){for(unsigned i=0;i<4;++i)v[i*5]=1;}};
struct Control{
    unsigned w=128,h=64,rw=64,rh=32;V2 current{},previous{};float clear=1;unsigned reversed=0,pad0=0,pad1=0;
    V2 inverse{1.f/128,1.f/64},reserved{};Matrix clipToWorld,previousClip;
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
bool sentinel(V4 p){return p.x==65504&&p.y==65504;}
bool valid(float z,const Control& c){return std::isfinite(z)&&z>=0&&z<=1&&z!=c.clear;}
// R16 UAV conversion can choose either adjacent half at a rounding boundary.
// Accept only the two representable values bracketing the exact float result;
// sentinel, wrong sign/scale or any value outside that interval still fails.
bool halfConversion(float actual,float expected){
    auto bits=DirectX::PackedVector::XMConvertFloatToHalf(expected);
    const float rounded=DirectX::PackedVector::XMConvertHalfToFloat(bits);
    if(actual==rounded)return true;if(rounded==expected)return false;
    if(rounded>expected)bits+=bits&0x8000?1:-1;else bits+=bits&0x8000?-1:1;
    return actual==DirectX::PackedVector::XMConvertHalfToFloat(bits);
}
V4 transform(V4 p,const Matrix& m){return {
    p.x*m.v[0]+p.y*m.v[4]+p.z*m.v[8]+p.w*m.v[12],
    p.x*m.v[1]+p.y*m.v[5]+p.z*m.v[9]+p.w*m.v[13],
    p.x*m.v[2]+p.y*m.v[6]+p.z*m.v[10]+p.w*m.v[14],
    p.x*m.v[3]+p.y*m.v[7]+p.z*m.v[11]+p.w*m.v[15]};}
V4 divided(V4 p){return {p.x/p.w,p.y/p.w,p.z/p.w,1};}
Matrix store(DirectX::FXMMATRIX m){DirectX::XMFLOAT4X4 v;DirectX::XMStoreFloat4x4(&v,m);Matrix out;memcpy(out.v,&v,64);return out;}
struct Part{float left,right,bottom,top,z;V4 displacement{0,0,0,0};float previousScale=1;};
struct Hit{V4 point;int part=-1;};
// Ray/rectangle intersections, not the shader's depth-consistency predicate.
Hit intersect(float u,float v,const Matrix& inverse,const std::vector<Part>& parts,bool previous){
    const auto a=divided(transform({2*u-1,1-2*v,0,1},inverse));
    const auto b=divided(transform({2*u-1,1-2*v,1,1},inverse));
    float nearest=std::numeric_limits<float>::infinity();Hit hit;
    for(size_t i=0;i<parts.size();++i){const auto& p=parts[i];const float s=previous?p.previousScale:1;
        const auto d=previous?p.displacement:V4{0,0,0,0};
        const float z=p.z*s+d.z,t=(z-a.z)/(b.z-a.z);
        const V4 world{a.x+t*(b.x-a.x),a.y+t*(b.y-a.y),z,1};
        // These scene planes all have negative world Z and both cameras look
        // toward them. Their ray intersections are ordered by -world Z, for
        // either depth convention (near/far interpolation t reverses in Z).
        const float distance=-z;
        if(world.x<p.left*s+d.x||world.x>p.right*s+d.x||world.y<p.bottom*s+d.y||world.y>p.top*s+d.y)continue;
        if(distance<nearest){nearest=distance;hit={world,int(i)};}
    }
    return hit;
}
struct Counts{unsigned staticValid=0,animated=0,recovered=0,radial=0;};
Counts scenes(ID3D11ComputeShader* source,ID3D11ComputeShader* baseline){
    using namespace DirectX;Counts totals;
    const char* names[]{"static","lateral","toward-camera","away-from-camera","independent-bones","same-ray-animation"};
    for(unsigned rz=0;rz<2;++rz)for(unsigned frame=0;frame<4;++frame)for(unsigned kind=0;kind<6;++kind){
        Control c;c.clear=rz?0.f:1.f;c.reversed=rz;c.current={.5f/c.rw,-.25f/c.rh};c.previous={-.25f/c.rw,.5f/c.rh};
        const auto projection=XMMatrixPerspectiveFovRH(1.2f,2.f,rz?20.f:1.f,rz?1.f:20.f);
        const auto camera=XMMatrixRotationX(frame&2?-.12f:.12f)*XMMatrixRotationY(frame&1?-.22f:.22f)*XMMatrixTranslation(.04f,-.03f,.02f);
        const auto clip=store(XMMatrixInverse(nullptr,camera)*projection);c.clipToWorld=store(XMMatrixInverse(nullptr,XMMatrixInverse(nullptr,camera)*projection));
        c.previousClip=store(projection);const auto previousInverse=store(XMMatrixInverse(nullptr,projection));
        std::vector<Part> parts{{-100,100,-100,100,-8},{-.35f,.35f,-.75f,.8f,-3.2f},{-.9f,-.4f,-.15f,.35f,-3.1f},{.4f,.9f,-.15f,.35f,-3.1f}};
        for(size_t k=1;k<parts.size();++k){
            if(kind==1)parts[k].displacement={.15f,.08f,0,0};
            if(kind==2)parts[k].displacement={.08f,-.05f,-.6f,0};
            if(kind==3)parts[k].displacement={-.08f,.05f,.5f,0};
            if(kind==4)parts[k].displacement=k==1?V4{.02f,.03f,-.4f,0}:k==2?V4{-.12f,.15f,-.8f,0}:V4{.12f,-.15f,.3f,0};
            if(kind==5)parts[k].previousScale=1.2f;
        }
        const unsigned n=c.rw*c.rh;std::vector<float> depth(n,c.clear),previous(n,c.clear);std::vector<V2> motion(n);
        std::vector<int> labels(n,-1),previousLabels(n,-1);std::vector<V4> world(n);
        for(unsigned y=0;y<c.rh;++y)for(unsigned x=0;x<c.rw;++x){const unsigned i=y*c.rw+x;
            const float nu=(x+.5f)/c.rw,nv=(y+.5f)/c.rh;
            const auto hit=intersect(nu-c.current.x,nv-c.current.y,c.clipToWorld,parts,false);labels[i]=hit.part;world[i]=hit.point;
            check(hit.part>=0,"scene current ray hit");
            const auto currentClip=divided(transform(hit.point,clip));depth[i]=currentClip.z;
            const auto& p=parts[hit.part];const auto d=p.displacement;
            const V4 oldWorld{hit.point.x*p.previousScale+d.x,hit.point.y*p.previousScale+d.y,hit.point.z*p.previousScale+d.z,1};
            const auto previousClip=divided(transform(oldWorld,c.previousClip));
            const float pu=(previousClip.x+1)*.5f+c.previous.x,pv=(1-previousClip.y)*.5f+c.previous.y;
            motion[i]={half(pu-nu),half(pv-nv)};
            const auto old=intersect(nu-c.previous.x,nv-c.previous.y,previousInverse,parts,true);previousLabels[i]=old.part;
            check(old.part>=0,"scene previous ray hit");previous[i]=divided(transform(old.point,c.previousClip)).z;
        }
        const auto old=run(c,depth,motion,previous,baseline),now=run(c,depth,motion,previous,source);
        unsigned interior=0,falseRejects=0,recovered=0,statics=0,radial=0;
        for(unsigned y=0;y<c.h;++y)for(unsigned x=0;x<c.w;++x){const unsigned i=y*c.w+x;
            check(now.depth[i]==old.depth[i],"R852 device-Z rows and donor bit-exact");
            const float u=(x+.5f)/c.w,v=1-(y+.5f)/c.h;
            const int bx=int(floorf((u+c.current.x)*c.rw)),by=int(floorf((v+c.current.y)*c.rh));
            const std::array<std::array<int,2>,5> taps{{{bx,by},{bx-1,by},{bx+1,by},{bx,by-1},{bx,by+1}}};
            int donor=-1,label=-1;bool homogeneous=true;
            for(auto at:taps){if(at[0]<0||at[1]<0||at[0]>=int(c.rw)||at[1]>=int(c.rh)){homogeneous=false;continue;}
                const int j=at[1]*int(c.rw)+at[0];if(label<0)label=labels[j];else if(label!=labels[j])homogeneous=false;
                if(donor<0||(rz?depth[j]>depth[donor]:depth[j]<depth[donor]))donor=j;
            }
            if(donor<0)continue;
            check(now.depth[i]==depth[donor],"five-cross nearest depth for both Z conventions");
            const float nu=(donor%c.rw+.5f)/c.rw,nv=(donor/c.rw+.5f)/c.rh;
            const V2 mv=motion[donor],vs{mv.x+c.current.x-c.previous.x,mv.y+c.current.y-c.previous.y};
            const float pu=u+vs.x,pv=v+vs.y,du=nu+mv.x,dv=nv+mv.y;
            if(pu<0||pu>=1||pv<0||pv>=1||du<0||du>=1||dv<0||dv>=1){check(sentinel(now.motion[i]),"history bounds sentinel");continue;}
            const int previousAt=int(floorf(dv*c.rh))*int(c.rw)+int(floorf(du*c.rw));
            if(!valid(previous[previousAt],c)){check(sentinel(now.motion[i]),"invalid historical depth sentinel");continue;}
            const bool vectorMatches=halfConversion(now.motion[i].x,vs.x*c.w)&&halfConversion(now.motion[i].y,-vs.y*c.h);
            check(vectorMatches,"actual game motion and DLSSG Y sign retained");
            if(!homogeneous||previousLabels[previousAt]!=labels[donor])continue;
            if(labels[donor]==0||kind==0){++statics;if(!sentinel(old.motion[i]))check(now.motion[i].x==old.motion[i].x&&now.motion[i].y==old.motion[i].y,"R852 static accepted vectors unchanged");continue;}
            ++interior;if(sentinel(old.motion[i])){++falseRejects;if(!sentinel(now.motion[i]))++recovered;}
            if(kind==5){const auto cameraOnly=divided(transform(world[donor],c.previousClip));
                const auto& part=parts[labels[donor]];const auto animated=divided(transform({world[donor].x*part.previousScale,world[donor].y*part.previousScale,world[donor].z*part.previousScale,1},c.previousClip));
                check(fabsf(cameraOnly.x-animated.x)<1.e-6f&&fabsf(cameraOnly.y-animated.y)<1.e-6f,"same-ray animation has zero camera XY residual");
                check(fabsf(cameraOnly.z-animated.z)>.01f,"same-ray animation changes previous device-Z");++radial;
            }
        }
        if(kind>0)check(interior>100,"substantial true animated predecessor coverage");
        if(kind>=2)check(falseRejects>100,"R852 rejects legitimate animated motion");
        totals.staticValid+=statics;totals.animated+=interior;totals.recovered+=recovered;totals.radial+=radial;
        printf("SCENE name=%s reversed=%u camera=%u static=%u animated=%u baseline_false_rejects=%u recovered=%u radial=%u\n",names[kind],rz,frame,statics,interior,falseRejects,recovered,radial);
    }
    return totals;
}
void safetyAndAmbiguity(ID3D11ComputeShader* source,ID3D11ComputeShader* baseline){
    for(unsigned rz=0;rz<2;++rz){Control c;c.reversed=rz;c.clear=rz?0.f:1.f;
        const unsigned n=c.rw*c.rh;std::vector<float> depth(n,.4f),previous(n,.4f);std::vector<V2> motion(n,{.03125f,-.03125f});
        const auto unchanged=run(c,depth,motion,previous,source);check(std::any_of(unchanged.motion.begin(),unchanged.motion.end(),sentinel),"edge leaves image");
        const float nan=std::numeric_limits<float>::quiet_NaN(),inf=std::numeric_limits<float>::infinity();
        for(float bad:{c.clear,-.1f,1.1f,nan,inf}){
            const auto r=run(c,depth,motion,std::vector<float>(n,bad),source);
            check(std::all_of(r.motion.begin(),r.motion.end(),sentinel),"clear/nonfinite/out-of-range previous depth rejected");
            check(r.depth==unchanged.depth,"missing history never fabricates current device-Z");
            const auto d=run(c,std::vector<float>(n,bad),motion,previous,source);
            check(std::all_of(d.motion.begin(),d.motion.end(),sentinel),"invalid current depth rejected");
            check(std::all_of(d.depth.begin(),d.depth.end(),[&](float z){return z==c.clear;}),"invalid current depth writes clear");
        }
        for(V2 bad:{V2{nan,0},V2{0,inf},V2{65504,65504},V2{32,0},V2{0,-32},V2{-2,0},V2{0,2}}){
            const auto r=run(c,depth,std::vector<V2>(n,bad),previous,source);
            check(std::all_of(r.motion.begin(),r.motion.end(),sentinel),"invalid/oversized/out-of-bounds motion stays sentinel");
        }
        {
            const float nearZ=rz?.8f:.2f,farZ=rz?.3f:.7f;
            auto d=std::vector<float>(n,farZ);auto m=std::vector<V2>(n);
            // At output (64,32), the cross is centered at native (32,15).
            d[15*c.rw+32]=nearZ;m[15*c.rw+32]={nan,0};
            const auto fallback=run(c,d,m,previous,source);
            check(fallback.depth[32*c.w+64]==farZ&&!sentinel(fallback.motion[32*c.w+64]),"invalid closest donor cannot poison valid cross fallback");
            m[15*c.rw+32]={};const auto closest=run(c,d,m,previous,source);
            check(closest.depth[32*c.w+64]==nearZ&&!sentinel(closest.motion[32*c.w+64]),"valid closest donor restores nearest foreground");
            d.assign(n,farZ);m.assign(n,{-.015625f,0});d[15*c.rw]=nearZ;
            const auto donorOutside=run(c,d,m,previous,source);
            check(sentinel(donorOutside.motion[32*c.w+2])&&donorOutside.depth[32*c.w+2]==nearZ,"donor history outside rejects even when output history is inside");
            d.assign(n,farZ);d[15*c.rw+1]=nearZ;
            const auto outputOutside=run(c,d,m,previous,source);
            check(sentinel(outputOutside.motion[32*c.w])&&outputOutside.depth[32*c.w]==nearZ,"output history outside rejects even when donor history is inside");
            d.assign(n,farZ);m.assign(n,{1.f/256,0});const auto exactEdge=run(c,d,m,previous,source);
            check(sentinel(exactEdge.motion[32*c.w+127]),"exact previous UV equals one rejects before lookup");
        }
        // Paired semantic counterexample: an animated surface moving in Z and
        // a newly visible static surface over prior background have identical
        // available depth/MV inputs. This adapter cannot classify either one.
        std::fill(motion.begin(),motion.end(),V2{});std::fill(previous.begin(),previous.end(),.6f);
        const auto animated=run(c,depth,motion,previous,source),staticDisocclusion=run(c,depth,motion,previous,source);
        const auto old=run(c,depth,motion,previous,baseline);
        check(std::all_of(old.motion.begin(),old.motion.end(),sentinel),"R852 Z gate rejects both indistinguishable semantics");
        for(unsigned i=0;i<c.w*c.h;++i){check(!sentinel(animated.motion[i]),"valid same-ray dynamic motion retained");
            check(animated.depth[i]==.4f&&staticDisocclusion.depth[i]==.4f,"true device-Z retained for DLSSG disocclusion");
            check(animated.motion[i].x==staticDisocclusion.motion[i].x&&animated.motion[i].y==staticDisocclusion.motion[i].y,"no invented static-vs-animated classification");}
        printf("AMBIGUITY reversed=%u animated_and_static_disocclusion=%u identical_inputs=1 delegated_to_dlssg=1\n",rz,c.w*c.h);
    }
}
void save(const std::string& path,const void* data,size_t n){std::ofstream f(path,std::ios::binary);f.write((const char*)data,n);if(!f)ExitProcess(3);}
ComPtr<ID3DBlob> compile(const char* source,const std::string& dir,const char* tag){
    ComPtr<ID3DBlob> blob,messages;const auto result=D3DCompile(source,strlen(source),tag,nullptr,nullptr,"NormalizeNative","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&messages);
    if(messages)printf("COMPILER %s %s\n",tag,(const char*)messages->GetBufferPointer());hr(result,"compile actual HLSL");
    save(dir+"/"+tag+".dxbc",blob->GetBufferPointer(),blob->GetBufferSize());
    ComPtr<ID3DBlob> disasm;hr(D3DDisassemble(blob->GetBufferPointer(),blob->GetBufferSize(),0,nullptr,&disasm),"disassemble");save(dir+"/"+tag+".asm",disasm->GetBufferPointer(),disasm->GetBufferSize());
    ComPtr<ID3D11ShaderReflection> reflection;hr(D3DReflect(blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&reflection)),"reflect");
    D3D11_SHADER_BUFFER_DESC cb{};hr(reflection->GetConstantBufferByName("Control")->GetDesc(&cb),"Control reflection");check(cb.Size==192,"unchanged Control192 ABI");
    const char* names[]{"pictureSize","producerSize","currentCorrection","previousCorrection","clearValue","reversedZ","inversePictureSize","currentClipToWorld","previousWorldToClip"};
    const unsigned offsets[]{0,8,16,24,32,36,48,64,128};
    for(unsigned i=0;i<9;++i){D3D11_SHADER_VARIABLE_DESC v{};hr(reflection->GetConstantBufferByName("Control")->GetVariableByName(names[i])->GetDesc(&v),"variable reflection");check(v.StartOffset==offsets[i],"Control variable offset preserved");
        printf("REFLECTION %s %s offset=%u size=%u used=%u\n",tag,names[i],v.StartOffset,v.Size,unsigned((v.uFlags&D3D_SVF_USED)!=0));}
    return blob;
}
}
int main(int argc,char** argv){
    if(argc!=3&&argc!=4)return 4;const std::string dir=argv[1],mode=argv[2];if(mode!="hardware"&&mode!="warp")return 4;
    const bool negativeControl=argc==4&&std::string(argv[3])=="--baseline-control";
    if(argc==4&&!negativeControl)return 4;
    printf("MODE baseline_negative_control=%u\n",unsigned(negativeControl));
    const auto compiled=compile(negativeControl?R852GameMotionHlsl::Source:GenshinFgGameMotionHlsl::Source,dir,"candidate"),old=compile(R852GameMotionHlsl::Source,dir,"r852");
    hr(D3D11CreateDevice(nullptr,mode=="warp"?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"debug device");
    ComPtr<IDXGIDevice> dxgi;hr(device.As(&dxgi),"dxgi");ComPtr<IDXGIAdapter> adapter;hr(dxgi->GetAdapter(&adapter),"adapter");DXGI_ADAPTER_DESC desc{};hr(adapter->GetDesc(&desc),"adapter desc");
    char name[256]{};WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,name,sizeof(name),nullptr,nullptr);printf("ADAPTER %s mode=%s vendor=%04x device=%04x\n",name,mode.c_str(),desc.VendorId,desc.DeviceId);
    ComPtr<ID3D11ComputeShader> source,baseline;hr(device->CreateComputeShader(compiled->GetBufferPointer(),compiled->GetBufferSize(),nullptr,&source),"candidate shader");hr(device->CreateComputeShader(old->GetBufferPointer(),old->GetBufferSize(),nullptr,&baseline),"baseline shader");
    const auto totals=scenes(source.Get(),baseline.Get());safetyAndAmbiguity(source.Get(),baseline.Get());
    check(totals.recovered>2000,"substantial dynamic false rejections reproduced and recovered");check(totals.radial>1000,"same-ray animation disproves camera-XY classifier");
    context->ClearState();context->Flush();ComPtr<ID3D11InfoQueue> info;hr(device.As(&info),"debug queue");unsigned errors=0,warnings=0;
    for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T n=0;info->GetMessage(i,nullptr,&n);std::vector<char> bytes(n);auto* m=(D3D11_MESSAGE*)bytes.data();info->GetMessage(i,m,&n);
        if(m->Severity<=D3D11_MESSAGE_SEVERITY_ERROR){++errors;printf("D3D_ERROR %s\n",m->pDescription);}if(m->Severity==D3D11_MESSAGE_SEVERITY_WARNING)++warnings;}
    check(errors==0,"zero D3D debug errors");check(warnings==0,"zero D3D debug warnings");
    printf("RESULT checks=%u failures=%u debug_errors=%u debug_warnings=%u static=%u animated=%u recovered=%u same_ray=%u product_build=NOT_RUN deployment=NOT_RUN\n",checks,failures,errors,warnings,totals.staticValid,totals.animated,totals.recovered,totals.radial);return failures?1:0;
}
