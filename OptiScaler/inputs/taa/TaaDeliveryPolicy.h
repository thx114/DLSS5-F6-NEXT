// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TaaProfile.h"

namespace taa::consumer {
// A proven extent is persistent feature configuration, never authority to bind
// a later frame. Every delivery still proves that frame's complete draw chain.
// Global screen dimensions also describe auxiliary/interop swapchains and do
// not supersede the actual linked consumer's authenticated output extent.
class DeliveryPolicy final {
    unsigned width_=0,height_=0,outputWidth_=0,outputHeight_=0;
public:
    bool Defer(OutputRoute route,unsigned width,unsigned height) const {
        return route==OutputRoute::ObserveUpscale && width==width_ && height==height_ &&
            outputWidth_>width && outputHeight_>height;
    }
    unsigned OutputWidth() const {return outputWidth_;}
    unsigned OutputHeight() const {return outputHeight_;}
    bool Observe(unsigned width,unsigned height,unsigned outputWidth,unsigned outputHeight) {
        if(!width || !height || outputWidth<=width || outputHeight<=height)return false;
        width_=width;height_=height;outputWidth_=outputWidth;outputHeight_=outputHeight;return true;
    }
    // Only configuration/owner changes forget a proven extent. A failed current
    // consumer cancels its own transaction without changing the NGX layout.
    void Revoke(){width_=height_=outputWidth_=outputHeight_=0;}
};
} // namespace taa::consumer
