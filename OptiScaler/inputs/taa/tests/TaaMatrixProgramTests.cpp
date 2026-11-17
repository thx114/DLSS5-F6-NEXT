#include <inputs/taa/TaaConsumerDx11.h>
#include <fstream>
#include <cstdio>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
namespace {unsigned checks=0;void Check(bool ok,const char* label){++checks;if(!ok)throw std::runtime_error(label);}void Hr(HRESULT hr){Check(SUCCEEDED(hr),"D3D11 API");}}
int main(int argc,char**argv){try{
    Check(argc==2,"captured shader directory required");std::ifstream in(std::string(argv[1])+"/VS-F1CE86BC0BE8E994.dxbc",std::ios::binary);std::vector<char> bytes((std::istreambuf_iterator<char>(in)),{});
    auto p=taa::consumer::Describe(bytes.data(),bytes.size(),taa::consumer::ProgramStage::Vertex);Check(p.hash==0xF1CE86BC0BE8E994ull&&p.supported,"actual captured position program");
    Check(p.constants==std::vector<unsigned>({0})&&p.textures.empty(),"captured VS preserves CB0 without RDEF");
    for(unsigned i=0;i<128;++i)Check(taa::consumer::Describe(bytes.data(),bytes.size(),taa::consumer::ProgramStage::Vertex).supported,"stripped SM4 static linkage is deterministic");
    std::ifstream pin(std::string(argv[1])+"/PS-6CA04A04AFEE2570.dxbc",std::ios::binary);std::vector<char> psbytes((std::istreambuf_iterator<char>(pin)),{});
    const auto post=taa::consumer::Describe(psbytes.data(),psbytes.size(),taa::consumer::ProgramStage::Pixel);
    Check(post.supported&&post.hash==0x6CA04A04AFEE2570ull&&post.constants==std::vector<unsigned>({0})&&post.textures==std::vector<unsigned>({0,1,2,3,4}),"captured PS preserves CB0, bloom, noise, LUT and max texture without RDEF");
    for(const auto* name:{"PS-88CBD14623C28B8E.dxbc","PS-F63F3FF630FEBDB4.dxbc"}) {
        std::ifstream input(std::string(argv[1])+"/"+name,std::ios::binary);std::vector<char> shader((std::istreambuf_iterator<char>(input)),{});
        const auto program=taa::consumer::Describe(shader.data(),shader.size(),taa::consumer::ProgramStage::Pixel);
        Check(program.supported&&program.constants.empty()&&program.textures==std::vector<unsigned>({0}),"other exact consumer programs retain executable texture declarations");
    }
    ComPtr<ID3D11Device>d;ComPtr<ID3D11DeviceContext>c;Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c));ComPtr<ID3D11DeviceContext1>c1;Hr(c.As(&c1));
    const D3D11_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};ComPtr<ID3D11InputLayout>layout;Hr(d->CreateInputLayout(&element,1,bytes.data(),bytes.size(),&layout));taa::fullscreen::Remember(layout.Get(),&element,1);c->IASetInputLayout(layout.Get());
    float points[]{-1,-1,3,-1,-1,3};D3D11_BUFFER_DESC desc{};desc.ByteWidth=sizeof(points);desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA init{points,0,0};ComPtr<ID3D11Buffer>vb;Hr(d->CreateBuffer(&desc,&init,&vb));taa::constants::Initialized(vb.Get(),&init);ID3D11Buffer*raw=vb.Get();UINT stride=8,offset=0;c->IASetVertexBuffers(0,1,&raw,&stride,&offset);
    const unsigned indices[]{0,1,2};desc.ByteWidth=sizeof(indices);desc.BindFlags=D3D11_BIND_INDEX_BUFFER;init.pSysMem=indices;ComPtr<ID3D11Buffer>ib;Hr(d->CreateBuffer(&desc,&init,&ib));taa::constants::Initialized(ib.Get(),&init);c->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    float matrix[128]{};const auto identity=[&]{std::fill(std::begin(matrix),std::end(matrix),0.0f);matrix[48]=1;matrix[53]=-1;matrix[62]=.5f;matrix[63]=1;};identity();desc.ByteWidth=sizeof(matrix);desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;init.pSysMem=matrix;ComPtr<ID3D11Buffer>cb;Hr(d->CreateBuffer(&desc,&init,&cb));taa::constants::Initialized(cb.Get(),&init);raw=cb.Get();c->VSSetConstantBuffers(0,1,&raw);
    const auto update=[&]{c->UpdateSubresource(cb.Get(),0,nullptr,matrix,0,0);taa::constants::Updated(c.Get(),cb.Get(),0,nullptr,matrix);};
    const auto coverage=[&]{auto current=p.position;return current.BindConstants(c.Get())&&taa::fullscreen::Geometry(c.Get(),true,3,0,0,nullptr,&current);};
    Check(coverage(),"actual indexed F1CE reflection covers full target");
    for(auto [lane,value]:std::array<std::pair<unsigned,float>,8>{{{48,.5f},{60,.5f},{62,-.1f},{62,1.1f},{63,.5f},{48,std::bit_cast<float>(0x7fc00000u)},{48,std::bit_cast<float>(0x7f800000u)},{48,1e-40f}}}){identity();matrix[lane]=value;update();Check(!coverage(),"partial, translated, clipped, non-unit, NaN, Inf, subnormal matrices are refused");}
    identity();update();auto shadow=taa::constants::Find(cb.Get());{std::lock_guard lock(shadow->mutex);shadow->known[0]=0;}Check(coverage(),"irrelevant unknown CB byte does not erase current proof");{std::lock_guard lock(shadow->mutex);shadow->known[48*4]=0;}Check(!coverage(),"one relevant unknown CB byte revokes proof");
    identity();std::copy_n(matrix,64,matrix+64);std::fill_n(matrix,64,0.0f);update();UINT first=16,count=16;c1->VSSetConstantBuffers1(0,1,&raw,&first,&count);Check(coverage(),"actual nonzero CB1 range is bound before position execution");count=0;c1->VSSetConstantBuffers1(0,1,&raw,&first,&count);Check(!coverage(),"empty bound range refuses matrix authority");
    raw=nullptr;c->VSSetConstantBuffers(0,1,&raw);Check(!coverage(),"unbound CB cannot reuse a prior proof");
    std::printf("PASS captured matrix/range/known-byte/indexed/float rejection: %u checks\n",checks);return 0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
