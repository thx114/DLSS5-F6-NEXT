// CPU routing policy with actual WARP device/context identities. No SDK/game.
#include <d3d11.h>
#include <wrl/client.h>
#include <optional>
#include <mutex>
#include <filesystem>
#include <functional>
#include <cstdio>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
#define LOG_INFO(...) ((void)0)
enum class Upscaler {DLSS_on12,FSR22};
enum class SwapchainInteropApi {Dx11wDx12,None};
enum class FGInput {Upscaler,None};
enum class FGOutput {DLSSG,None};
struct State {SwapchainInteropApi swapchainInteropApi=SwapchainInteropApi::Dx11wDx12;
    FGInput activeFgInput=FGInput::Upscaler;FGOutput activeFgOutput=FGOutput::DLSSG;
    bool externalFrameGeneration=false,gameCalledSlInit=false;
    static State& Instance(){static State s;return s;}};
namespace Util {std::filesystem::path ExePath(){return L"StarRail.exe";}}
namespace native {bool OpticalFgNrEnabled(){return false;}}
struct NVSDK_NGX_Parameter {};
class IFeature_Dx11 {
public: Upscaler backend;bool succeeds=true,inited=false;std::function<void()> during;
    explicit IFeature_Dx11(Upscaler b):backend(b){}
    bool Init(ID3D11Device*,ID3D11DeviceContext*,NVSDK_NGX_Parameter*){if(during)during();inited=succeeds;return succeeds;}
    bool IsInited(){return inited;}Upscaler GetUpscalerType() const{return backend;}
};
class FeatureProvider_Dx11 {public:
    static std::optional<Upscaler> ActiveBackend(ID3D11Device* sourceDevice=nullptr);
    static void BindActiveFeature(IFeature_Dx11*,ID3D11DeviceContext*);
    static void ForgetFeature(const IFeature_Dx11*);
    static bool InitializeFeature(IFeature_Dx11*,ID3D11Device*,ID3D11DeviceContext*,NVSDK_NGX_Parameter*);
};
class Dx11wDx12SC {public:
    const bool _sourceFrameLatencyOwner;ID3D11Device* _dx11Device;
    Dx11wDx12SC(ID3D11Device* d,bool owns=true):_sourceFrameLatencyOwner(owns),_dx11Device(d){}
    bool StarRailF5DsTransport() const;
};
#include "Dx11BackendBindingExtracted.h"
unsigned checks=0;
void Check(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
int main(){
    ComPtr<ID3D11Device> device,other;ComPtr<ID3D11DeviceContext> context,otherContext;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"source WARP");
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&other,nullptr,&otherContext)),"other WARP");
    Dx11wDx12SC early(device.Get()),unrelated(other.Get()),notOwner(device.Get(),false);
    Check(!early.StarRailF5DsTransport(),"wrapper before any feature has no committed backend");
    IFeature_Dx11 dlss(Upscaler::DLSS_on12);
    dlss.during=[&]{Check(FeatureProvider_Dx11::ActiveBackend()==Upscaler::DLSS_on12,"Init local provider sees candidate");
        Check(!early.StarRailF5DsTransport(),"source cannot consume an uncommitted candidate");};
    Check(FeatureProvider_Dx11::InitializeFeature(&dlss,device.Get(),context.Get(),nullptr),"actual successful Init commits");
    Check(early.StarRailF5DsTransport(),"preexisting wrapper resolves later successful DLSS binding");
    Check(!unrelated.StarRailF5DsTransport()&&!notOwner.StarRailF5DsTransport(),"binding cannot authorize another device or unadmitted source");
    State::Instance().externalFrameGeneration=true;Check(!early.StarRailF5DsTransport(),"external FG retains policy exclusion");State::Instance().externalFrameGeneration=false;
    IFeature_Dx11 failed(Upscaler::FSR22);failed.succeeds=false;
    Check(!FeatureProvider_Dx11::InitializeFeature(&failed,other.Get(),otherContext.Get(),nullptr),"failed Init remains failed");
    Check(early.StarRailF5DsTransport(),"unrelated failed candidate cannot erase committed source");
    IFeature_Dx11 fsr(Upscaler::FSR22);
    Check(FeatureProvider_Dx11::InitializeFeature(&fsr,device.Get(),context.Get(),nullptr),"actual fallback commits its own backend");
    Check(!early.StarRailF5DsTransport(),"FSR does not inherit DLSS transport policy");
    FeatureProvider_Dx11::BindActiveFeature(&dlss,context.Get());
    Check(early.StarRailF5DsTransport(),"reselected live DLSS restores matching backend");
    FeatureProvider_Dx11::ForgetFeature(&fsr);Check(early.StarRailF5DsTransport(),"old instance retirement cannot erase selected owner");
    FeatureProvider_Dx11::ForgetFeature(&dlss);Check(!early.StarRailF5DsTransport(),"exact selected instance retirement revokes CPU binding");
    std::printf("PASS DX11 actual backend/source-device binding: %u checks; SDK/GPU retirement/game untested\n",checks);
}
