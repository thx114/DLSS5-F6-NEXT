// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <nvsdk_ngx_params.h>
#include <array>
#include <cstring>

namespace OptiBridge
{
// A native D3D11 NGX table may ignore D3D12 typed setters. Keep translated
// resources in this synchronous view; scalar/provider metadata still uses the
// caller's table, including keys unknown to OptiScaler. No COM ownership or GPU
// completion is implied by a parameter mapping.
class NgxDx12ResourceView final : public NVSDK_NGX_Parameter
{
    enum class Domain { Unbound, Generic, Dx11, Dx12 };
    struct Slot { void* resource = nullptr; Domain domain = Domain::Unbound; };
    inline static constexpr std::array<const char*, 6> names_ {
        NVSDK_NGX_Parameter_Color, NVSDK_NGX_Parameter_Depth, NVSDK_NGX_Parameter_MotionVectors,
        NVSDK_NGX_Parameter_Output, NVSDK_NGX_Parameter_ExposureTexture,
        NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask
    };
    NVSDK_NGX_Parameter& source_;
    std::array<Slot, names_.size()> slots_ {};

    static std::size_t Index(const char* key) noexcept
    {
        if (key)
            for (std::size_t i = 0; i < names_.size(); ++i)
                if (std::strcmp(key, names_[i]) == 0) return i;
        return names_.size();
    }
    template<class Resource> void SetResource(const char* key, Resource* resource, Domain domain)
    {
        const auto i = Index(key);
        if (i == names_.size()) source_.Set(key, resource);
        else slots_[i] = {resource, domain};
    }
    template<class Resource> NVSDK_NGX_Result GetResource(const char* key, Resource** resource, Domain domain) const
    {
        if (!resource) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        const auto i = Index(key);
        if (i == names_.size() || slots_[i].domain == Domain::Unbound)
            return source_.Get(key, resource);
        *resource = nullptr;
        const auto& slot = slots_[i];
        if (domain != Domain::Generic && slot.domain != Domain::Generic && slot.domain != domain)
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        *resource = static_cast<Resource*>(slot.resource);
        return NVSDK_NGX_Result_Success; // Explicit null must shadow the caller's old resource.
    }
public:
    explicit NgxDx12ResourceView(NVSDK_NGX_Parameter& source) noexcept : source_(source) {}
    NgxDx12ResourceView(const NgxDx12ResourceView&) = delete;
    NgxDx12ResourceView& operator=(const NgxDx12ResourceView&) = delete;

#define OPTI_NGX_FORWARD_VALUE(Type) \
    void Set(const char* key, Type value) override { source_.Set(key, value); } \
    NVSDK_NGX_Result Get(const char* key, Type* value) const override { return source_.Get(key, value); }
    OPTI_NGX_FORWARD_VALUE(unsigned long long)
    OPTI_NGX_FORWARD_VALUE(float)
    OPTI_NGX_FORWARD_VALUE(double)
    OPTI_NGX_FORWARD_VALUE(unsigned int)
    OPTI_NGX_FORWARD_VALUE(int)
#undef OPTI_NGX_FORWARD_VALUE

    void Set(const char* key, void* value) override { SetResource(key, value, Domain::Generic); }
    void Set(const char* key, ID3D11Resource* value) override { SetResource(key, value, Domain::Dx11); }
    void Set(const char* key, ID3D12Resource* value) override { SetResource(key, value, Domain::Dx12); }
    NVSDK_NGX_Result Get(const char* key, void** value) const override { return GetResource(key, value, Domain::Generic); }
    NVSDK_NGX_Result Get(const char* key, ID3D11Resource** value) const override { return GetResource(key, value, Domain::Dx11); }
    NVSDK_NGX_Result Get(const char* key, ID3D12Resource** value) const override { return GetResource(key, value, Domain::Dx12); }
    void Reset() override { source_.Reset(); slots_ = {}; }
};
}
