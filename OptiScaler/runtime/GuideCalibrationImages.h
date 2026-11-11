// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GuideCalibration.h"

namespace GuideCalibration {
struct Image {
    uint32_t width = 0, height = 0;
    std::vector<Pixel> pixels;
    // Invalid/unobserved pixels are masked out; black is never claimed evidence.
    std::vector<uint8_t> mask;
};
struct Images {
    Image warped, residual, depthContribution;
    Rejection rejection = Rejection::BadMetadata;
    CandidateFlags flags;
};
// At most three 160x100 RGB images plus masks. Diagnostic depth contribution is
// score decomposition (R=temporal depth discrepancy, G=edge mismatch, B=occlusion),
// NEVER measured NR history contribution or a feedback-acceptance visualization.
// Observe uses the same explicit colour/raster phase and finite bilinear guide
// support as scoring; rejected or missing support stays masked in all views.
inline Images MakeImages(const Frame& previous, const Frame& current, CandidateFlags flags,
                         uint64_t nowMs, const Options& options = {}) {
    Images images; images.flags = flags;
    const auto pair = ScorePair(previous,current,nowMs,options);
    images.rejection = pair.rejection;
    if (!pair.valid) return images;
    images.rejection = pair.candidates[flags.Index()].rejection;
    for (auto* image : {&images.warped,&images.residual,&images.depthContribution}) {
        image->width = current.width; image->height = current.height;
        image->pixels.resize(size_t(current.width)*current.height);
        image->mask.resize(image->pixels.size());
    }
    for (unsigned y = 0; y < current.height; ++y) for (unsigned x = 0; x < current.width; ++x) {
        const auto v = Detail::Observe(previous,current,x,y,flags,options);
        if (!v.compared) continue;
        const auto i = Detail::Index(current,x,y);
        images.warped.pixels[i] = v.warped;
        const float error = static_cast<float>(v.residual);
        images.residual.pixels[i] = {error,error,error};
        images.depthContribution.pixels[i] = {static_cast<float>(v.depthError),static_cast<float>(v.edgeError),v.occluded ? 1.f : 0.f};
        images.warped.mask[i] = images.residual.mask[i] = images.depthContribution.mask[i] = 1;
    }
    return images;
}
} // namespace GuideCalibration
