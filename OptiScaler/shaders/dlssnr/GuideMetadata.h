// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>

namespace DlssNr::GuideMetadata {
enum class Field : unsigned { CreateFlags, RenderWidth, RenderHeight, DepthX, DepthY,
    MotionX, MotionY, ScaleX, ScaleY, JitterX, JitterY, DepthBounds, MotionBounds, Count };
enum class Origin : unsigned { Unknown, Evaluate, RetainedFeature, ApiDefault, ActualCall };
struct Value { double value=0; Origin origin=Origin::Unknown; bool valid=false; };
struct Provenance {
    std::array<Value,static_cast<unsigned>(Field::Count)> fields{};
    void Set(Field f,double value,bool valid,Origin origin) {
        fields[static_cast<unsigned>(f)]={value,valid?origin:Origin::Unknown,valid};
    }
};
// nvsdk_ngx_helpers_vk.h's common DLSS evaluation contract marks subrect
// bases OPTIONAL (leave zero if unused), and forwards them under the same
// API-independent NGX parameter keys. This applies only to DLSS, not DLSSD.
// A supplied value always wins; never turn a malformed supplied value into zero.
inline bool OptionalBase(unsigned& value,bool supplied,bool rayReconstruction) {
    if (supplied) return true;
    if (rayReconstruction) return false;
    value=0; return true;
}
// A live selected provider resolves the actual render extent using its own
// GetRenderResolution policy. It is not inferred from a resource allocation.
inline bool RenderExtent(unsigned& width,unsigned& height,bool widthRead,bool heightRead,
                         unsigned selectedWidth,unsigned selectedHeight,bool selectedKnown) {
    if ((widthRead && !width) || (heightRead && !height)) return false;
    if (widthRead && heightRead) return true;
    if (!selectedKnown || !selectedWidth || !selectedHeight) return false;
    width=selectedWidth; height=selectedHeight; return true;
}
} // namespace DlssNr::GuideMetadata
