// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "DlssNr_Common.h"
#include <cmath>
#include <cstring>

namespace DlssNrHistoryExposure {
// Only the ordinary source-output passthrough pixel chain is independent of
// WhitePoint/exposure. Other domains keep their exact scalar/GPU contract.
struct Input {
    bool feedback=false,sourceOutput=false,beforeUpscale=false,nativePreSr=false;
    bool finishedPicture=false,linearHdr=true,spatial=false;
    unsigned compare=0,debug=0,inputEncoding=0,colourEncoding=0;
    bool encodingKnown=true,encodingForced=false;
    float preExposure=1;
};
inline bool Independent(const Input& f,const DlssNrConstants& encode) {
    const bool domain=((f.colourEncoding==0||f.colourEncoding==2)&&f.inputEncoding==0) ||
        (f.colourEncoding==3&&f.inputEncoding==3);
    return f.feedback&&f.sourceOutput&&!f.beforeUpscale&&!f.nativePreSr&&!f.finishedPicture&&
        !f.linearHdr&&!f.spatial&&!f.compare&&!f.debug&&domain&&
        encode.Mode==DlssNrMode_Encode&&encode.Passthrough==1&&encode.InputEncoding==f.inputEncoding&&
        std::isfinite(f.preExposure)&&f.preExposure>0;
}
inline DlssNrConstants CanonicalEncoding(const DlssNrConstants& source) {
    auto c=source;
    c.WhitePoint=1;c.UseGameExposure=c.UseExposureWhitePoint=0;
    c.ExposurePreMul=0;c.ExposureBaseScale=c.ExposureTrim=1;
    c.ExposureSourceWidth=c.ExposureSourceHeight=c.MeterCopiesExposure=0;
    c.ExposureTrimAnchorCount=c.ExposureTrimPreview=0;
    // The ABI declares eight consecutive exposure/trim pairs.
    static_assert(offsetof(DlssNrConstants,AutoExposureShadowProtection)==
        offsetof(DlssNrConstants,ExposureTrimAnchorExposure0)+16*sizeof(float));
    std::memset(reinterpret_cast<unsigned char*>(&c)+offsetof(DlssNrConstants,ExposureTrimAnchorExposure0),0,16*sizeof(float));
    c.AutoExposureShadowProtection=0;
    return c;
}
inline uint64_t Domain(const Input& f) {
    // Keep unspecified versus declared sRGB, forced choice and producer facts
    // separate even when their edge shader both uses InputEncoding=0.
    return (1ull<<63)|(uint64_t(f.colourEncoding)<<32)|
        (uint64_t(f.encodingKnown)<<40)|(uint64_t(f.encodingForced)<<41);
}
} // namespace DlssNrHistoryExposure
