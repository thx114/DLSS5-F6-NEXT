// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <utility>
#include <nvsdk_ngx_defs.h>

// Shared by the DX11, DX12 and Vulkan adapters. These are CPU ownership and
// rebuild phases; none of them certifies GPU completion. The adapter retains
// its existing feature-release and queue/fence contract.
namespace FeatureTransition
{
enum class Phase { Invalid, Retire, Construct, Initialize };

// The counter remains visible to the public NGX adapters, which distinguish an
// incomplete successful phase from an actual Evaluate. Never wrap a bad counter
// into another retirement or accidentally report an unexecuted initialization.
inline Phase Advance(int& counter) noexcept
{
    switch (counter)
    {
    case 0: counter = 1; return Phase::Retire;
    case 1: counter = 2; return Phase::Construct;
    case 2: counter = 3; return Phase::Initialize;
    default: return Phase::Invalid;
    }
}

// Allocate the next table and capture the recipe before detaching the old
// feature. The retiring context owns its own initialization table until AFTER
// its feature destructor/release; a custom table supplied by the caller remains
// borrowed even when it carries an OptiScaler allocation tag.
template<class Context, class Parameters, class Allocate, class Populate>
std::unique_ptr<Context> Prepare(Context& current, Parameters* supplied,
                                 bool borrow, Allocate&& allocate, Populate&& populate)
{
    if (!current.feature || (borrow && (!supplied || supplied == current.createParamsOwner.get()))) return {};

    auto retired = std::make_unique<Context>();
    typename Context::ParameterOwner next { nullptr, nullptr };
    if (!borrow)
    {
        next = std::forward<Allocate>(allocate)();
        if (!next) return {};
    }
    auto* target = borrow ? supplied : next.get();
    std::forward<Populate>(populate)(*target, *current.feature);

    retired->feature = std::move(current.feature);
    retired->createParamsOwner = std::move(current.createParamsOwner);
    retired->createParams = std::exchange(current.createParams, nullptr);
    if (borrow) current.BorrowCreateParams(supplied);
    else current.OwnCreateParams(std::move(next));
    return retired;
}

// One recipe, irrespective of graphics API or executable. Passthrough tables
// keep all caller-specific keys; synthesized tables receive these common keys.
template<class Parameters, class Feature>
void CopyRecipe(Parameters& parameters, Feature& feature)
{
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, feature.GetFeatureFlags());
    parameters.Set(NVSDK_NGX_Parameter_Width, feature.RenderWidth());
    parameters.Set(NVSDK_NGX_Parameter_Height, feature.RenderHeight());
    parameters.Set(NVSDK_NGX_Parameter_OutWidth, feature.DisplayWidth());
    parameters.Set(NVSDK_NGX_Parameter_OutHeight, feature.DisplayHeight());
    parameters.Set(NVSDK_NGX_Parameter_PerfQualityValue, feature.PerfQualityValue());
}
}
