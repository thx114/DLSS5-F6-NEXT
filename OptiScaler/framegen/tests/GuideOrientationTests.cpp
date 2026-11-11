#include "../GuideOrientation.h"
#include "../dlssg/DlssgInputContinuity.h"
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <concepts>
#include <string>
// Extracted from the current Config.h by the common runner, not a mock.
#include "GuideOrientationConfigBase.h"

struct ConfigFixture
{
    CustomOptional<bool> FGResourceFlip{false};
    CustomOptional<bool> FGResourceFlipOffset{false};
    CustomOptional<bool, NoDefault> FGDepthTextureFlipY, FGMotionTextureFlipY, FGMotionYInverted;
};

unsigned checks = 0;
void Check(bool condition, int line)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "GuideOrientation failed at %d\n", line); std::abort(); }
}
#define CHECK(x) Check(bool(x), __LINE__)

int main()
{
    using framegen::ResolveGuideOrientation;
    const std::optional<bool> values[] = {std::nullopt, false, true};
    for (bool legacy : {false, true})
    for (auto depth : values)
    for (auto motion : values)
    for (auto sign : values)
    for (bool offset : {false, true})
    {
        const auto actual = ResolveGuideOrientation(legacy, depth, motion, sign, offset);
        const bool expectedDepth = depth.has_value() ? *depth : legacy;
        const bool expectedMotion = motion.has_value() ? *motion : legacy;
        const bool expectedSign = sign.has_value() ? *sign : legacy;
        CHECK(actual.depthFlipY == expectedDepth);
        CHECK(actual.motionFlipY == expectedMotion);
        CHECK(actual.motionYInverted == expectedSign);
        CHECK(actual.DepthCopy() == expectedDepth);
        CHECK(actual.MotionCopy() == (expectedMotion || expectedSign));
        CHECK(actual.UsesOffset() == (offset && (expectedDepth || expectedMotion)));
        CHECK(actual.Key() == (unsigned(expectedDepth) | (unsigned(expectedMotion) << 1) |
                               (unsigned(expectedSign) << 2)));
    }
    // All coordinate changes invalidate same-frame guide ownership, including
    // sign-only changes. No old pair can survive changing only an override.
    for (unsigned mask = 0; mask != 8; ++mask)
    {
        dlssg::InputOwner owner{};
        owner.flip = ResolveGuideOrientation(false, bool(mask & 1), bool(mask & 2), bool(mask & 4), false).Key();
        dlssg::InputContinuity<2> continuity;
        continuity.Observe(0, 17, owner, true, continuity.SourceGeneration());
        continuity.Observe(0, 17, owner, false, continuity.SourceGeneration());
        CHECK(continuity.Current(0, 17, owner));
        for (unsigned changed = 0; changed != 8; ++changed)
        {
            auto next = owner;
            next.flip = changed;
            CHECK(continuity.Current(0, 17, next) == (mask == changed));
        }
    }
    ConfigFixture config;
    config.FGResourceFlip=true;
    CHECK(ResolveGuideOrientation(config).Key() == 7);
    config.FGMotionYInverted.set_from_config(false);
    CHECK(ResolveGuideOrientation(config).Key() == 3);
    CHECK(config.FGMotionYInverted.value_for_config().has_value());
    CHECK(config.FGMotionYInverted.value_for_config() == false);
    config.FGDepthTextureFlipY=false;
    config.FGMotionTextureFlipY=false;
    CHECK(ResolveGuideOrientation(config).Key() == 0);
    CHECK(config.FGDepthTextureFlipY.value_for_config() == false);
    config.FGMotionYInverted=std::nullopt;
    CHECK(ResolveGuideOrientation(config).Key() == 4);
    CHECK(!config.FGMotionYInverted.value_for_config());
    ConfigFixture reloaded;
    reloaded.FGResourceFlip.set_from_config(config.FGResourceFlip.value_for_config());
    reloaded.FGDepthTextureFlipY.set_from_config(config.FGDepthTextureFlipY.value_for_config());
    reloaded.FGMotionTextureFlipY.set_from_config(config.FGMotionTextureFlipY.value_for_config());
    reloaded.FGMotionYInverted.set_from_config(config.FGMotionYInverted.value_for_config());
    CHECK(ResolveGuideOrientation(reloaded).Key() == 4);
    std::printf("GuideOrientation: %u checks passed\n", checks);
}
