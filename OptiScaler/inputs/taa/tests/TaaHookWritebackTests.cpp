// Executes the production Map/Unmap hook bodies with a real WARP mapping and
// the GIMI staging/writeback boundary. The interposer is a fixture; the captured
// bytes, production hooks, resource mirror and GPU buffer are real.
#include <inputs/taa/TaaConstantBuffer.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>

using Microsoft::WRL::ComPtr;
bool TrackingInternal(){return false;}
namespace taa {bool NeedsHooks(){return true;} void ConsumerWrite(ID3D11DeviceContext*,ID3D11Resource*){}}
namespace DrawObservationDx11 {
bool NeedsDraw(){return false;}
void NonDrawWrite(ID3D11DeviceContext*){}
}
namespace gimi::runtime {
inline bool enabled=true,active=false,returned=false;
inline void* driver=nullptr;
inline std::array<float,4> staging{};
bool Enabled(){return enabled;}
uint64_t VertexMapStarting(ID3D11DeviceContext*,ID3D11Resource*,UINT,D3D11_MAP){return 1;}
void VertexMapFailed(ID3D11DeviceContext*,ID3D11Resource*,UINT,uint64_t){assert(false);}
void ConstantMapped(ID3D11DeviceContext*,ID3D11Resource*,UINT,D3D11_MAP,UINT,
    D3D11_MAPPED_SUBRESOURCE* mapped,HRESULT,uint64_t,bool){
    driver=mapped->pData;mapped->pData=staging.data();active=true;
}
struct CpuContentSubmission {
    bool valid=false;
    static bool HasPendingMaps(){return active;}
    static CpuContentSubmission Unmap(ID3D11DeviceContext*,ID3D11Resource*,UINT,bool){
        assert(active&&driver);std::memcpy(driver,staging.data(),sizeof(staging));active=false;return {true};
    }
    explicit operator bool() const{return valid;}
    void Returned(){returned=true;}
};
void VertexUnmapped(ID3D11DeviceContext*,ID3D11Resource*,UINT){}
}
HRESULT o_DepthMap(ID3D11DeviceContext* c,ID3D11Resource* r,UINT sub,D3D11_MAP type,UINT flags,D3D11_MAPPED_SUBRESOURCE* mapped){
    const auto result=c->Map(r,sub,type,flags,mapped);
    if(SUCCEEDED(result))std::memset(mapped->pData,0,16); // distinct from both submitted jitter values
    return result;
}
void o_DepthUnmap(ID3D11DeviceContext* c,ID3D11Resource* r,UINT sub){c->Unmap(r,sub);}
#include "taa_map_hooks.inc"
int main(){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)));
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=16;desc.Usage=D3D11_USAGE_DYNAMIC;
    desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> buffer;assert(SUCCEEDED(device->CreateBuffer(&desc,nullptr,&buffer)));
    float value=0;assert(!taa::constants::Read(buffer.Get(),0,value));
    for(const bool enabled:{true,false}){
        gimi::runtime::enabled=enabled;gimi::runtime::returned=false;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        assert(SUCCEEDED(hkDepthMap(context.Get(),buffer.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped)));
        const std::array<float,4> current{enabled?.375f:-.625f,.25f,0,0};
        std::memcpy(mapped.pData,current.data(),sizeof(current));
        // Pending staging writebacks remain mandatory after MODs turn off.
        gimi::runtime::enabled=false;
        hkDepthUnmap(context.Get(),buffer.Get(),0);
        if(!taa::constants::Read(buffer.Get(),0,value)||value!=current[0]){
            std::fputs("FAIL jitter mirror must observe the completed staging writeback\n",stderr);std::exit(19);
        }
        assert(!enabled||gimi::runtime::returned);
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> readback;assert(SUCCEEDED(device->CreateBuffer(&desc,nullptr,&readback)));
        context->CopyResource(readback.Get(),buffer.Get());
        assert(SUCCEEDED(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped)));
        float gpuValue=0;std::memcpy(&gpuValue,mapped.pData,sizeof(gpuValue));context->Unmap(readback.Get(),0);
        assert(gpuValue==value);
    }
    std::puts("PASS actual Map/Unmap hooks: staging writeback, MOD-off pending writeback and GPU/mirror identity");
}
