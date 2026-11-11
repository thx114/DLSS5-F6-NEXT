#include <wrapped/SwapchainHookBoundary.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

#pragma warning(push)
#pragma warning(disable: 4100) // Deliberately unused COM stub parameters.
struct Stub : SwapchainHookBoundary {
    template<class T> explicit Stub(T*) : SwapchainHookBoundary(OperationsFor<T>()) {}
HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override { return {}; }
ULONG STDMETHODCALLTYPE AddRef() override { return {}; }
ULONG STDMETHODCALLTYPE Release() override { return {}; }
HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override { return {}; }
HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override { return {}; }
HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override { return {}; }
HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override { return {}; }
HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice) override { return {}; }
HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override { return {}; }
HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) override { return {}; }
HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) override { return {}; }
HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput) override { return {}; }
HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override { return {}; }
HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount) override { return {}; }
HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) override { return {}; }
HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) override { return {}; }
HRESULT STDMETHODCALLTYPE GetHwnd(HWND* pHwnd) override { return {}; }
HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void** ppUnk) override { return {}; }
HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags,
                                       const DXGI_PRESENT_PARAMETERS* pPresentParameters) override { return {}; }
BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported(void) override { return {}; }
HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) override { return {}; }
HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* pColor) override { return {}; }
HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* pColor) override { return {}; }
HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override { return {}; }
HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* pRotation) override { return {}; }
HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override { return {}; }
HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* pWidth, UINT* pHeight) override { return {}; }
HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override { return {}; }
HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override { return {}; }
HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject(void) override { return {}; }
HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) override { return {}; }
HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) override { return {}; }
UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex(void) override { return {}; }
HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                             UINT SwapChainFlags, const UINT* pCreationNodeMask,
                                             IUnknown* const* ppPresentQueue) override { return {}; }
};
#pragma warning(pop)
// Deliberately different concrete layouts, as in physical output vs interop.
template<int Padding, int Identity> struct Chain final : Stub {
    std::array<unsigned char, Padding> unrelated{};
    unsigned calls = 0;
    explicit Chain() : Stub(this) {}
    HRESULT hit() { ++calls; return Identity; }
    HRESULT HookSetFullscreenState(BOOL a, IDXGIOutput* b) { return a==TRUE && !b ? hit() : E_INVALIDARG; }
    HRESULT HookGetFullscreenState(BOOL* a, IDXGIOutput** b) { if(!a||!b)return E_POINTER;*a=TRUE;*b=nullptr;return hit(); }
    HRESULT HookResizeBuffers(UINT a,UINT b,UINT c,DXGI_FORMAT d,UINT e) { return a==3&&b==1920&&c==1080&&d==DXGI_FORMAT_R8G8B8A8_UNORM&&e==2 ? hit():E_INVALIDARG; }
    HRESULT HookResizeTarget(const DXGI_MODE_DESC* a) { return a&&a->Width==1920 ? hit():E_INVALIDARG; }
    HRESULT HookCheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE a,UINT* b) { if(!b)return E_POINTER;*b=Identity;return a==DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ? hit():E_INVALIDARG; }
    HRESULT HookSetColorSpace1(DXGI_COLOR_SPACE_TYPE a) { return a==DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ? hit():E_INVALIDARG; }
    HRESULT HookSetHDRMetaData(DXGI_HDR_METADATA_TYPE a,UINT b,void* c) { return a==DXGI_HDR_METADATA_TYPE_NONE&&!b&&!c ? hit():E_INVALIDARG; }
};
static unsigned checks=0;
static void check(bool b) { ++checks;if(!b){std::fprintf(stderr,"FAILED check %u\n",checks);std::exit(1);} }
template<class Fn, class... Args> __declspec(noinline) static HRESULT call(void* saved,IDXGISwapChain4* chain,Args... args) {
    return reinterpret_cast<Fn>(saved)(chain,args...);
}
int main() {
    using A=Chain<16,101>;using B=Chain<1024,202>;
    static_assert(!std::is_abstract_v<A> && !std::is_abstract_v<B>);
    A a;B b;
    auto table=[](IDXGISwapChain4* p){return *reinterpret_cast<void***>(p);};
    auto* av=table(&a);auto* bv=table(&b);
    constexpr unsigned slots[]={10,11,13,14,37,38,40};
    for(auto i:slots)check(av[i]==bv[i]);
    // Model the external bridge's 41-slot per-instance cloned vtable, retaining
    // one original per method from either wrapper and calling it with both.
    std::array<void*,41> clone{};
    for(unsigned i=0;i<41;++i)clone[i]=bv[i];
    *reinterpret_cast<void***>(static_cast<IDXGISwapChain4*>(&b))=clone.data();
    for(auto* saved:{av,bv})for(IDXGISwapChain4* p:{static_cast<IDXGISwapChain4*>(&a),static_cast<IDXGISwapChain4*>(&b)}) {
        const HRESULT id=p==static_cast<IDXGISwapChain4*>(&a)?101:202;
        using SF=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain4*,BOOL,IDXGIOutput*);
        check(call<SF>(saved[10],p,TRUE,static_cast<IDXGIOutput*>(nullptr))==id);
        using GF=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain4*,BOOL*,IDXGIOutput**);
        BOOL full=FALSE;IDXGIOutput* output=reinterpret_cast<IDXGIOutput*>(1);
        check(call<GF>(saved[11],p,&full,&output)==id&&full&&output==nullptr);
        using RB=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain4*,UINT,UINT,UINT,DXGI_FORMAT,UINT);
        check(call<RB>(saved[13],p,3u,1920u,1080u,DXGI_FORMAT_R8G8B8A8_UNORM,2u)==id);
        using RT=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain4*,const DXGI_MODE_DESC*);
        DXGI_MODE_DESC desc{};desc.Width=1920;
        check(call<RT>(saved[14],p,static_cast<const DXGI_MODE_DESC*>(&desc))==id);
        check(call<RT>(saved[14],p,static_cast<const DXGI_MODE_DESC*>(nullptr))==E_INVALIDARG);
        using CC=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain4*,DXGI_COLOR_SPACE_TYPE,UINT*);
        UINT support=0;
        check(call<CC>(saved[37],p,DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,&support)==id&&support==static_cast<UINT>(id));
        using SC=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain4*,DXGI_COLOR_SPACE_TYPE);
        check(call<SC>(saved[38],p,DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)==id);
        using HDR=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain4*,DXGI_HDR_METADATA_TYPE,UINT,void*);
        check(call<HDR>(saved[40],p,DXGI_HDR_METADATA_TYPE_NONE,0u,static_cast<void*>(nullptr))==id);
    }
    *reinterpret_cast<void***>(static_cast<IDXGISwapChain4*>(&b))=bv;
    check(a.calls==14&&b.calls==14);
    std::printf("SwapchainHookBoundary: %u checks passed (both original owners, different layouts, cloned 41-slot vtable)\n",checks);
}
