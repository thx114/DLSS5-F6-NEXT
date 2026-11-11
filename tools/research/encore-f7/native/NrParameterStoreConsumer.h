#pragma once

#include <cstddef>
#include <nvsdk_ngx_params.h>

namespace encore_research
{

struct ConsumerValues
{
    unsigned long long ull = 0;
    float single = 0.0F;
    double real = 0.0;
    unsigned int uint = 0;
    int integer = 0;
    ID3D11Resource* d3d11 = nullptr;
    ID3D12Resource* d3d12 = nullptr;
    void* opaque = nullptr;
};

struct ConsumerResults
{
    NVSDK_NGX_Result ull = NVSDK_NGX_Result_Fail;
    NVSDK_NGX_Result single = NVSDK_NGX_Result_Fail;
    NVSDK_NGX_Result real = NVSDK_NGX_Result_Fail;
    NVSDK_NGX_Result uint = NVSDK_NGX_Result_Fail;
    NVSDK_NGX_Result integer = NVSDK_NGX_Result_Fail;
    NVSDK_NGX_Result d3d11 = NVSDK_NGX_Result_Fail;
    NVSDK_NGX_Result d3d12 = NVSDK_NGX_Result_Fail;
    NVSDK_NGX_Result opaque = NVSDK_NGX_Result_Fail;
};

// Ordinary same-compiler virtual calls in a separate translation unit. The
// consumer sees only the public interface, not the producer implementation.
// These calls do not establish compatibility with any external payload ABI.
void ConsumerSetAll(NVSDK_NGX_Parameter& parameters, const ConsumerValues& values);
ConsumerResults ConsumerGetAll(const NVSDK_NGX_Parameter& parameters, ConsumerValues& outputs);
void ConsumerReset(NVSDK_NGX_Parameter& parameters);

} // namespace encore_research
