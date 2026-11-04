#pragma once

#include <memory>
#include <utility>

template <typename FeatureType> struct ContextData
{
    using ParameterOwner = std::unique_ptr<NVSDK_NGX_Parameter, void (*)(NVSDK_NGX_Parameter*)>;
    // Only a table allocated for this transition is owned here. A caller's custom
    // OptiScaler table is still borrowed, regardless of its allocation tag.
    ParameterOwner createParamsOwner { nullptr, nullptr };
    std::unique_ptr<FeatureType> feature;
    NVSDK_NGX_Parameter* createParams = nullptr;
    int changeBackendCounter = 0;

    ContextData() = default;
    ContextData(const ContextData&) = delete;
    ContextData& operator=(const ContextData&) = delete;
    ContextData(ContextData&& other) noexcept { *this = std::move(other); }
    ContextData& operator=(ContextData&& other) noexcept
    {
        if (this != &other)
        {
            // Dense-map erase/assignment can replace a live entry. Its feature
            // must be destroyed before the parameters used during initialization.
            feature.reset();
            ClearCreateParams();
            createParamsOwner = std::move(other.createParamsOwner);
            feature = std::move(other.feature);
            createParams = std::exchange(other.createParams, nullptr);
            changeBackendCounter = std::exchange(other.changeBackendCounter, 0);
        }
        return *this;
    }

    void ClearCreateParams()
    {
        createParams = nullptr;
        createParamsOwner.reset();
    }

    void BorrowCreateParams(NVSDK_NGX_Parameter* parameters)
    {
        ClearCreateParams();
        createParams = parameters;
    }

    void OwnCreateParams(ParameterOwner parameters)
    {
        ClearCreateParams();
        createParamsOwner = std::move(parameters);
        createParams = createParamsOwner.get();
    }
};
