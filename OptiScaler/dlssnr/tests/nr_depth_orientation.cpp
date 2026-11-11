// CPU contract checks only. These do not identify native texture orientation or validate GPU pixels.
#include "../../shaders/dlssnr/DlssNr_GuideCoordinates.h"
#include "../../shaders/dlssnr/DlssNr_Spatial.h"

#include <cstdio>
#include <cstdlib>

namespace
{
unsigned checks = 0;
void Require(bool condition, const char* message)
{
    ++checks;
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
}

int main()
{
    using DlssNr::GuideCoordinates::Resolve;
    // PID31592 declared settings: depth/motion/sign false, picture flow true,
    // scene depth not picture aligned. Reproduces the logged key, not its pixel correctness.
    const auto observed = Resolve(false, false, false, true, true, false);
    Require(observed.key == 8 && !observed.depthRowsFlip && !observed.motionRowsFlip &&
                !observed.vectorYNegated, "recorded ZZZ NR contract remains unflipped");

    for (unsigned settings = 0; settings < 8; ++settings)
    {
        const bool depth = (settings & 1) != 0, motion = (settings & 2) != 0, sign = (settings & 4) != 0;
        const auto native = Resolve(depth, motion, sign, true, false, false);
        Require(native.key == settings, "native rows and vector sign have independent bits");
        const auto picture = Resolve(depth, motion, sign, true, true, false);
        Require(!picture.motionRowsFlip && !picture.vectorYNegated,
                "native overrides cannot flip or negate finished-picture motion");
        Require(picture.depthRowsFlip == depth && picture.key == (8u | (settings & 1u)),
                "scene-depth correction remains independent from finished-picture motion");
        const auto missing = Resolve(depth, motion, sign, false, true, false);
        Require(!missing.depthRowsFlip && missing.key == picture.key,
                "missing depth cannot change the history coordinate contract");
        const auto aligned = Resolve(depth, motion, sign, true, true, true);
        Require(!aligned.depthRowsFlip && !aligned.motionRowsFlip && !aligned.vectorYNegated && aligned.key == 24,
                "already aligned depth and picture motion cannot be corrected twice");
    }

    // Distinct allocation sizes and nonzero offsets catch allocation-wide flips,
    // a depth rect accidentally applied to picture motion, and double MV scaling.
    const auto regions = DlssNr::ResolveGuideRegions({1300, 830}, {2600, 1640}, {1280, 800}, {2560, 1600},
                                                  false, 7, 11, 13, 17);
    Require(regions.depth.x == 7 && regions.depth.y == 11 && regions.depth.width == 1280 &&
                regions.depth.height == 800, "capture depth retains its render subrect");
    Require(regions.motion.x == 13 && regions.motion.y == 17 && regions.motion.width == 2560 &&
                regions.motion.height == 1600, "picture motion retains its independent output subrect");
    for (unsigned flips = 0; flips < 4; ++flips)
    {
        const auto c = DlssNr::Spatial::NormalizeGuides(regions, (flips & 1) != 0, (flips & 2) != 0);
        Require(c.mode == 104 && c.width == 2560 && c.height == 1600, "normalization covers both effective guides");
        Require(c.depthRect[0] == 7 && c.depthRect[1] == 11 && c.depthRect[2] == 1280 && c.depthRect[3] == 800,
                "depth flip uses the active rect rather than the allocation");
        Require(c.motionRect[0] == 13 && c.motionRect[1] == 17 && c.motionRect[2] == 2560 && c.motionRect[3] == 1600,
                "motion rect is not inferred from depth");
        Require(c.padding[0] == ((flips & 1) ? 1.0f : 0.0f) && c.padding[1] == ((flips & 2) ? 1.0f : 0.0f),
                "depth and motion row flags remain independent");
        Require(c.motionScale[0] == 0 && c.motionScale[1] == 0, "row normalization does not introduce MV scaling");
    }
    std::printf("NR depth orientation CPU PASS checks=%u; native rows/GPU/pixels remain UNKNOWN\n", checks);
}
