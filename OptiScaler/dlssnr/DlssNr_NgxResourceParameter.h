#pragma once
#include <nvsdk_ngx_defs.h>

namespace DlssNr::NgxParameter
{
// Public NGX typed and void* setters may use separate slots. Return only a
// successful non-null result; the caller still proves resource/device/frame identity.
// Borrowed pointer: this helper does not retain, mutate, or invent a resource.
template <class Resource, class Parameters>
Resource* ReadResource(Parameters* parameters, const char* key)
{
    if (!parameters || !key) return nullptr;
    Resource* typed = nullptr;
    if (parameters->Get(key, &typed) == NVSDK_NGX_Result_Success && typed)
        return typed;
    void* untyped = nullptr;
    if (parameters->Get(key, &untyped) == NVSDK_NGX_Result_Success && untyped)
        return static_cast<Resource*>(untyped);
    return nullptr;
}
}
