// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "NrGpuExecutionLifetime.h"
#include "DlssNr_OrderedLifetime.h"

void NrGpuExecutionLifetime::ParkFeature(void*& feature, ReleaseFeature overrideRelease,
                                       NVSDK_NGX_Parameter** parameters)
{
    if (!feature && (!parameters || !*parameters)) return;
    Retired entry;
    entry.uses=recordings_.Snapshot();
    entry.feature=feature;
    entry.release=overrideRelease;
    feature=nullptr;
    if (parameters) { entry.parameters=*parameters; *parameters=nullptr; }
    retired_.push_back(std::move(entry));
}

void NrGpuExecutionLifetime::ParkResource(ID3D12Resource*& resource)
{
    if (!resource) return;
    Retired entry;
    entry.uses=recordings_.Snapshot();
    entry.resource=resource;
    resource=nullptr;
    retired_.push_back(std::move(entry));
}

void NrGpuExecutionLifetime::CollectCompleted(ReleaseFeature currentDefault, DestroyParameters destroyParameters)
{
    DlssNrGpuLifetime::CollectRetired(retired_,[&](const Retired& entry) {
        const auto release=entry.release ? entry.release : currentDefault;
        if (entry.feature && release) release(entry.feature);
        // A parameter block remains owned until its feature's actual uses end,
        // and is destroyed after the feature, preserving the NGX API order.
        if (entry.parameters && destroyParameters) destroyParameters(entry.parameters);
        if (entry.resource) entry.resource->Release();
    });
}

bool NrGpuExecutionLifetime::ContainsRetiredResource(ID3D12Resource* resource) const noexcept
{
    return std::any_of(retired_.begin(),retired_.end(),[&](const Retired& entry) { return entry.resource==resource; });
}
