#include <mods/gimi/GimiPresentSource.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>

using Microsoft::WRL::ComPtr;
ID3D11Device* nativeDevice=nullptr;
unsigned remembered=0, captured=0;
namespace DrawObservationDx11 {
uint64_t ShaderByteHash(const void*,SIZE_T) { return 1; }
void RememberShader(ID3D11DeviceChild*,uint64_t,SIZE_T) { ++remembered; }
}
namespace gimi::runtime {
struct SourceFixtureState { std::mutex ownerMutex; PresentSources presentSources; };
SourceFixtureState& Store() { static SourceFixtureState state; return state; }
#include "source_registry.inc"
void StopAccepting() noexcept { assert(false); }
void ShaderCreated(ID3D11Device* device,ID3D11DeviceChild*,const void*,SIZE_T,bool,bool) {
    assert(device==nativeDevice); ++captured;
}
}
HWND observedMainWindow=nullptr;
namespace Util {
HWND GetProcessWindow() { return observedMainWindow; }
bool CheckForRealObject(const char*,IUnknown*,IUnknown**) { return false; }
}
namespace GenericDepthDx11 {
#include "source_register.inc"
}
bool ModCreationEnabled(ID3D11Device* device) { return device==nativeDevice; }
template<class F> struct TestHooks {
    std::array<F,2> originals;
    struct Call { F original; F Get() const { return original; } };
    Call Acquire(size_t index) { return {originals.at(index)}; }
};
using Pixel=HRESULT(*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11PixelShader**);
using Vertex=HRESULT(*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11VertexShader**);
TestHooks<Pixel> pixelHooks;
TestHooks<Vertex> vertexHooks;
static thread_local unsigned pixelCreationDepth=0,vertexCreationDepth=0;
#include "shader_ingress.inc"
HRESULT NativePixel(ID3D11Device* d,const void* b,SIZE_T n,ID3D11ClassLinkage* c,ID3D11PixelShader** s) { return d->CreatePixelShader(b,n,c,s); }
HRESULT NativeVertex(ID3D11Device* d,const void* b,SIZE_T n,ID3D11ClassLinkage* c,ID3D11VertexShader** s) { return d->CreateVertexShader(b,n,c,s); }
HRESULT ProxyPixel(ID3D11Device*,const void* b,SIZE_T n,ID3D11ClassLinkage* c,ID3D11PixelShader** s) { return hkCreatePixelShader<1>(nativeDevice,b,n,c,s); }
HRESULT ProxyVertex(ID3D11Device*,const void* b,SIZE_T n,ID3D11ClassLinkage* c,ID3D11VertexShader** s) { return hkVisibleCreateVertexShader<1>(nativeDevice,b,n,c,s); }
int main() {
    ComPtr<ID3D11Device> a,b; ComPtr<ID3D11DeviceContext> ac,bc;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&a,nullptr,&ac)));
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&b,nullptr,&bc)));
    ComPtr<IDXGIFactory2> factory; assert(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    std::array<HWND,3> windows{}; std::array<ComPtr<IDXGISwapChain1>,3> chains;
    for (unsigned i=0;i<3;++i) {
        windows[i]=CreateWindowExW(0,L"STATIC",L"F6 owner fixture",WS_OVERLAPPED,0,0,192,128,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        assert(windows[i]);
        DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=192; desc.Height=128; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=2; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        assert(SUCCEEDED(factory->CreateSwapChainForHwnd(i==0?a.Get():b.Get(),windows[i],&desc,nullptr,nullptr,&chains[i])));
    }
    using namespace gimi::runtime;
    assert(!GenericDepthDx11::RegisterModPresentSource(chains[1].Get(),windows[1]));
    observedMainWindow=windows[1]; // game HWND becomes eligible after construction.
    assert(GenericDepthDx11::RegisterModPresentSource(chains[1].Get(),windows[1]));
    SelectPresentSource(chains[1].Get(),b.Get(),bc.Get()); // B is explicitly selected despite A creating first.
    assert(!AcquirePresentSource(chains[0].Get())); // auxiliary A cannot confirm its provisional capture.
    assert(!AcquirePresentSource(chains[2].Get())); // another chain sharing B's context is still auxiliary.
    const auto selected=AcquirePresentSource(chains[1].Get()); assert(selected && selected->live.load());
    SelectPresentSource(chains[1].Get(),b.Get(),bc.Get());
    assert(AcquirePresentSource(chains[1].Get())->generation==selected->generation);
    SelectPresentSource(chains[0].Get(),a.Get(),ac.Get());
    assert(!selected->live.load() && !AcquirePresentSource(chains[1].Get()));
    const auto replacement=AcquirePresentSource(chains[0].Get()); assert(replacement->generation>selected->generation);
    ForgetPresentSource(chains[1].Get()); assert(replacement->live.load());
    ForgetPresentSource(chains[0].Get()); assert(!replacement->live.load() && !AcquirePresentSource(chains[0].Get()));
    SelectPresentSource(chains[0].Get(),a.Get(),ac.Get());
    const auto serialOwner=AcquirePresentSource(chains[0].Get());
    HANDLE entered=CreateEventW(nullptr,TRUE,FALSE,nullptr), switched=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    assert(entered && switched);
    std::unique_lock ownerLock(Store().ownerMutex);
    std::thread selector([&] { SetEvent(entered);SelectPresentSource(chains[1].Get(),b.Get(),bc.Get());SetEvent(switched); });
    assert(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0);
    assert(WaitForSingleObject(switched,25)==WAIT_TIMEOUT && serialOwner->live.load());
    ownerLock.unlock(); selector.join();
    assert(!serialOwner->live.load() && AcquirePresentSource(chains[1].Get()));
    CloseHandle(entered);CloseHandle(switched);
    ForgetPresentSource(chains[1].Get());
    nativeDevice=b.Get(); pixelHooks.originals={ProxyPixel,NativePixel}; vertexHooks.originals={ProxyVertex,NativeVertex};
    for (bool vertex : {false,true}) {
        ComPtr<ID3DBlob> code,errors;
        const char* text=vertex?"float4 main(uint id:SV_VertexID):SV_Position{return float4(0,0,0,1);}":"float4 main():SV_Target{return float4(1,0,0,1);}";
        assert(SUCCEEDED(D3DCompile(text,strlen(text),nullptr,nullptr,nullptr,"main",vertex?"vs_5_0":"ps_5_0",0,0,&code,&errors)));
        if (vertex) { ComPtr<ID3D11VertexShader> shader; assert(SUCCEEDED(hkVisibleCreateVertexShader<0>(a.Get(),code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader))); }
        else { ComPtr<ID3D11PixelShader> shader; assert(SUCCEEDED(hkCreatePixelShader<0>(a.Get(),code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader))); }
    }
    assert(captured==2 && remembered==2); // real nested proxy -> native callbacks, one registration each.
    for (unsigned i=0;i<3;++i) { chains[i].Reset(); DestroyWindow(windows[i]); }
    std::puts("PASS GIMI production source selection and nested shader ingress with real WARP owners");
}
