// Admission and command-order test using the actual production copy body.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
using UINT = unsigned;
using UINT64 = uint64_t;
struct ID3D11Resource {};
struct ID3D11Texture2D : ID3D11Resource {};
struct ID3D12Resource {};
struct ID3D12CommandQueue {};
std::vector<unsigned> commands;
struct ID3D11DeviceContext4
{
    ID3D11Resource* target = nullptr;
    ID3D11Resource* source = nullptr;
    void CopyResource(ID3D11Resource* to, ID3D11Resource* from) { target=to; source=from; commands.push_back(2); }
    void Flush() { commands.push_back(3); }
};
#define DX11_WITH_DX12_CACHED_FRAMES 2
#define LOG_ERROR(...) ((void)0)
class Dx11WithDx12
{
public:
    enum class ResourceMask { None, Output };
    struct Output
    {
        UINT64 LastPreparedFrame = 0;
        ID3D12Resource* Dx12Resource = nullptr;
        ID3D11Texture2D* SharedTexture = nullptr;
    };
    struct Cache
    {
        UINT64 frameId = 0;
        Output Output[2] {};
        ID3D11Resource* ParamOutput[2] {};
    };
#include "Dx11OutputCopyBinding.inl"
    inline static std::recursive_mutex InputReadMutex;
    inline static ID3D11DeviceContext4* Dx11DeviceContext = nullptr;
    inline static ID3D12CommandQueue* Dx12CommandQueue = nullptr;
    inline static Cache resourceCache;
    inline static UINT64 LastPreparedUpscalerFrameId = 0;
    inline static UINT UpscalerFrameIndex = 0;
    inline static ResourceMask LastPreparedUpscalerMask = ResourceMask::None;
    inline static bool syncOk = true;
    inline static ID3D12CommandQueue* signalled = nullptr;
    static Cache& GetUpscalerResourceCache() { return resourceCache; }
    static bool CheckMask(ResourceMask mask, ResourceMask required) { return mask == required; }
    static bool SyncDx12ToDx11(ID3D12CommandQueue* queue) { commands.push_back(1); signalled=queue; return syncOk; }
    static bool CopyUpscalerOutputToDx11(const OutputCopy& expected, bool writeTarget);
};
#include "Dx11OutputCopyProduction.inl"
namespace
{
unsigned checks = 0;
void Check(bool ok, const char* label)
{
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
ID3D11DeviceContext4 context, otherContext;
ID3D12CommandQueue queue, otherQueue;
ID3D11Texture2D target, shared, otherTexture;
ID3D12Resource source, otherSource;
Dx11WithDx12::OutputCopy Reset()
{
    using D = Dx11WithDx12;
    context = {}; commands.clear(); D::syncOk=true; D::signalled=nullptr;
    D::Dx11DeviceContext=&context; D::Dx12CommandQueue=&queue;
    D::resourceCache={}; D::resourceCache.frameId=D::LastPreparedUpscalerFrameId=42;
    D::UpscalerFrameIndex=1; D::LastPreparedUpscalerMask=D::ResourceMask::Output;
    D::resourceCache.Output[1]={42,&source,&shared}; D::resourceCache.ParamOutput[1]=&target;
    return {1,42,&context,&queue,&target,&shared,&source};
}
template<class F> void Reject(const char* label, F mutate)
{
    auto expected=Reset(); mutate(expected);
    Check(!Dx11WithDx12::CopyUpscalerOutputToDx11(expected,true),label);
    Check(commands.empty() && !context.target,"reject before any wait, copy or flush");
}
}
int main()
{
    using D = Dx11WithDx12;
    auto expected=Reset();
    Check(D::CopyUpscalerOutputToDx11(expected,true),"matching binding admitted");
    Check(commands==std::vector<unsigned>{1,2,3} && D::signalled==&queue &&
          context.target==&target && context.source==&shared,"actual queue handoff precedes exact target copy and flush");
    expected=Reset();
    Check(D::CopyUpscalerOutputToDx11(expected,false),"unproven output still returns submitted reads");
    Check(commands==std::vector<unsigned>{1,3} && !context.target,"unproven output waits and flushes without stale copy");
    expected=Reset(); D::syncOk=false;
    Check(!D::CopyUpscalerOutputToDx11(expected,true),"failed return synchronization is not delivery");
    Check(commands==std::vector<unsigned>{1} && !context.target,"failed wait never copies target");
    Reject("missing frame",[](auto& e){e.cacheFrame=0;});
    Reject("invalid frame sentinel",[](auto& e){e.cacheFrame=UINT64_MAX;});
    Reject("invalid slot",[](auto& e){e.slot=2;});
    Reject("wrong caller context",[](auto& e){e.context=&otherContext;});
    Reject("wrong provider queue",[](auto& e){e.queue=&otherQueue;});
    Reject("missing caller output",[](auto& e){e.target=nullptr;});
    Reject("missing shared output",[](auto& e){e.shared=nullptr;});
    Reject("missing provider output",[](auto& e){e.source=nullptr;});
    Reject("cache rebound to another frame",[](auto&){D::resourceCache.frameId=43;});
    Reject("last prepared frame changed",[](auto&){D::LastPreparedUpscalerFrameId=43;});
    Reject("slot reused",[](auto&){D::UpscalerFrameIndex=0;});
    Reject("output not prepared",[](auto&){D::LastPreparedUpscalerMask=D::ResourceMask::None;});
    Reject("old output generation",[](auto&){D::resourceCache.Output[1].LastPreparedFrame=41;});
    Reject("provider resource rebound",[](auto&){D::resourceCache.Output[1].Dx12Resource=&otherSource;});
    Reject("shared resource rebound",[](auto&){D::resourceCache.Output[1].SharedTexture=&otherTexture;});
    Reject("target resource rebound",[](auto&){D::resourceCache.ParamOutput[1]=&otherTexture;});
    std::printf("PASS actual output-copy admission and command ordering: %u checks\n",checks);
}
