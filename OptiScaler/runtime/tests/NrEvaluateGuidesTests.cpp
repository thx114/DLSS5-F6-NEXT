// SPDX-License-Identifier: GPL-3.0-or-later
// Real D3D12 resource bounds and production Evaluate reader. No NGX SDK calls.
#include <shaders/dlssnr/DlssNr_EvaluateGuides.h>
#include <inputs/NgxFeatureRegistry.h>
#include <wrl/client.h>
#include <dxgi1_4.h>
#include <string>
#include <unordered_map>
#include <variant>
#include <type_traits>
#include <stdexcept>
#include <cstdio>
using Microsoft::WRL::ComPtr;
namespace {
struct Parameters final : NVSDK_NGX_Parameter
{
    using Value = std::variant<unsigned long long, float, double, unsigned int, int,
                               ID3D11Resource*, ID3D12Resource*, void*>;
    std::unordered_map<std::string, Value> values;
    mutable std::unordered_map<std::string, unsigned> reads;
    mutable unsigned reads11 = 0, reads12 = 0, readsVoid = 0;
    bool writeOnFailure = false;

    template <class T> NVSDK_NGX_Result Read(const char* key, T* value) const
    {
        ++reads[key];
        if constexpr (std::is_same_v<T, ID3D11Resource*>) ++reads11;
        if constexpr (std::is_same_v<T, ID3D12Resource*>) ++reads12;
        if constexpr (std::is_same_v<T, void*>) ++readsVoid;
        const auto it = values.find(key);
        if (it != values.end())
        {
            if (const auto* stored = std::get_if<T>(&it->second))
            {
                *value = *stored;
                return NVSDK_NGX_Result_Success;
            }
        }
        if (writeOnFailure)
        {
            if constexpr (std::is_pointer_v<T>)
                *value = reinterpret_cast<T>(uintptr_t { 0xbad });
            else
                *value = static_cast<T>(123);
        }
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }

#define TEST_PARAMETER_OVERLOAD(T) \
    void Set(const char* key, T value) override { values.insert_or_assign(key, value); } \
    NVSDK_NGX_Result Get(const char* key, T* value) const override { return Read(key, value); }
    TEST_PARAMETER_OVERLOAD(unsigned long long)
    TEST_PARAMETER_OVERLOAD(float)
    TEST_PARAMETER_OVERLOAD(double)
    TEST_PARAMETER_OVERLOAD(unsigned int)
    TEST_PARAMETER_OVERLOAD(int)
    TEST_PARAMETER_OVERLOAD(ID3D11Resource*)
    TEST_PARAMETER_OVERLOAD(ID3D12Resource*)
    TEST_PARAMETER_OVERLOAD(void*)
#undef TEST_PARAMETER_OVERLOAD

    void Reset() override { values.clear(); }
    unsigned ReadCount(const char* key) const
    {
        const auto it = reads.find(key);
        return it == reads.end() ? 0 : it->second;
    }
};

struct Source {
 unsigned ngxHandle=7; const void* virtualContext=this; uint64_t virtualGeneration=12;
 unsigned featureFlags=NVSDK_NGX_DLSS_Feature_Flags_MVLowRes|NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
 bool featureFlagsKnown=true; unsigned renderWidth=8,renderHeight=4; bool renderExtentKnown=true;
 GuideSourceMetadata::Ptr rawMetadata;
};
}
int main(){try{
 unsigned checks=0;const auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("Evaluate input contract");};
 NgxFeatureRegistry registry; registry.Record(7,NVSDK_NGX_Feature_SuperSampling,17);
 const auto first=registry.Read(7);registry.Forget(7);registry.Record(7,NVSDK_NGX_Feature_SuperSampling,17);
 check(first.generation!=0&&registry.Read(7).generation>first.generation);
 check(registry.Read(99).generation==0);
 ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;ComPtr<ID3D12Device> device;
 check(SUCCEEDED(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory))));
 check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
 check(SUCCEEDED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))));
 D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
 D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=16;rd.Height=8;
 rd.DepthOrArraySize=1;rd.MipLevels=1;rd.Format=DXGI_FORMAT_R32_FLOAT;rd.SampleDesc.Count=1;
 ComPtr<ID3D12Resource> depth,motion;
 check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&depth))));
 rd.Format=DXGI_FORMAT_R32G32_FLOAT;
 check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&motion))));
 Parameters p;Source source;
 p.Set(NVSDK_NGX_Parameter_MV_Scale_X,8.0f);p.Set(NVSDK_NGX_Parameter_MV_Scale_Y,-4.0f);
 p.Set(NVSDK_NGX_Parameter_Jitter_Offset_X,0.25f);p.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,-0.25f);
 const auto read=[&]{DlssNrFrameInfo f;f.OutputWidth=16;f.OutputHeight=8;
  DlssNr::EvaluateGuides::Read(f,p,source,depth.Get(),motion.Get());return f;};
 auto f=read();check(f.NativeGuideMetadataKnown&&f.NativeCurrentJitterKnown&&f.DepthInverted&&f.MotionVectorsLowResolution);
 check(f.RenderSubrectWidth==8&&f.RenderSubrectHeight==4&&f.MvScaleY==-4);
 check(f.DepthSubrectBaseX==0&&f.MotionSubrectBaseY==0);
 check(!f.NativeBaseIdentityVerified&&!f.NativeSceneRoute&&!f.NativePreSrRoute); // facts never grant authority
 p.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,9u);check(!read().NativeGuideMetadataKnown);
 p.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,8u);check(read().NativeGuideMetadataKnown);
 p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,0u);check(!read().NativeGuideMetadataKnown);
 p.values.erase(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width);
 p.values.erase(NVSDK_NGX_Parameter_MV_Scale_Y);check(!read().NativeGuideMetadataKnown);
 p.writeOnFailure=true;f=read();check(!f.NativeGuideMetadataKnown&&f.MvScaleY==1);p.writeOnFailure=false;
 p.Set(NVSDK_NGX_Parameter_MV_Scale_Y,-4.0f);
 p.values.erase(NVSDK_NGX_Parameter_Jitter_Offset_Y);check(!read().NativeCurrentJitterKnown);
 p.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,-0.25f);
 source.featureFlagsKnown=false;check(!read().NativeGuideMetadataKnown);
 source.featureFlagsKnown=true;f=read();check(f.NativeGuideMetadataKnown);
 check(!DlssNr::EvaluateGuides::Fits(depth.Get(),~0u,0,8,4));
 check(!DlssNr::EvaluateGuides::Fits(depth.Get(),0,0,0,4));
 check(!DlssNr::EvaluateGuides::Fits(nullptr,0,0,8,4));
 std::printf("NrEvaluateGuides: %u checks passed\n",checks);return 0;
}catch(const std::exception&e){std::puts(e.what());return 1;}}
