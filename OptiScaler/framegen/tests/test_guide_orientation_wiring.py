"""Integration guards for the tested FG policy; not SDK/game validation."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[3]


def source(path):
    return (ROOT / 'OptiScaler' / path).read_text(encoding='utf-8-sig')


class OrientationWiring(unittest.TestCase):
    def test_configuration_keeps_explicit_false(self):
        header, config = source('Config.h'), source('Config.cpp')
        for key in ('DepthTextureFlipY', 'MotionTextureFlipY', 'MotionYInverted'):
            field = 'FG' + key
            self.assertRegex(header, rf'CustomOptional<bool, NoDefault>\s+{field};')
            self.assertIn(f'{field}.set_from_config(readBool("OptiFG", "{key}"))', config)
            self.assertIn(f'"OptiFG", "{key}", GetBoolValue(this->{field}.value_for_config())', config)

    def test_all_fg_consumers_use_independent_policy(self):
        for path in ('framegen/dlssg/DLSSG_Dx12.cpp', 'framegen/ffx/FSRFG_Dx12.cpp',
                     'framegen/xefg/XeFG_Dx12.cpp'):
            cpp = source(path)
            self.assertIn('framegen::ResolveGuideOrientation', cpp)
            self.assertIn('orientation.MotionCopy()', cpp)
            self.assertIn('orientation.DepthCopy()', cpp)
            self.assertNotIn('FGResourceFlip.value_or_default()', cpp)

    def test_owner_and_per_guide_conversion(self):
        cpp = source('framegen/dlssg/DLSSG_Dx12.cpp')
        owner = cpp.split('DLSSG_Dx12::CurrentInputOwner() const', 1)[1].split('\n}', 1)[0]
        self.assertIn('orientation.Key()', owner)
        self.assertIn('orientation.UsesOffset()', owner)
        owned = cpp.split('DLSSG_Dx12::FlipOwnedUpscalerGuide', 1)[1].split('\n}', 1)[0]
        self.assertIn('depth ? orientation.depthFlipY : orientation.motionFlipY', owned)
        self.assertIn('orientation.motionYInverted', owned)
        self.assertIn('!depth,', owned)
        pair = cpp.split('DLSSG_Dx12::SetUpscalerGuidePair', 1)[1].split('\n}', 1)[0]
        self.assertIn('!depthFlip : !motionFlip', pair)
        self.assertIn('guide-copy-generation-needs-drain', pair)
        # The source scales are normalized once. The sign is applied in the
        # copied texture, never a second time through the SDK scale.
        constants = cpp.split('DLSSG_Dx12::BuildGuideConstants', 1)[1].split('\n}', 1)[0]
        self.assertNotIn('motionYInverted', constants)
        self.assertIn('_mvScaleY[slot] / (float) mv->height', constants)

    def test_common_conversion_does_not_treat_depth_as_velocity(self):
        cpp = source('framegen/IFGFeature_Dx12.cpp')
        flip = cpp.split('IFGFeature_Dx12::FlipResource', 1)[1].split('\n}', 1)[0]
        self.assertIn('const bool velocity = type == FG_ResourceType::Velocity;', flip)
        self.assertIn('resource->height, velocity,', flip)
        self.assertIn('velocity ? orientation.motionFlipY : orientation.depthFlipY', flip)

    def test_menu_changes_rebuild_fg_and_do_not_change_nr(self):
        menu = source('menu/menu_common.cpp')
        block = menu.split('const auto orientationOverride =', 1)[1].split('ImGui::Spacing();', 1)[0]
        self.assertIn('state.fgChanged = true', block)
        self.assertIn('setting = std::nullopt', block)
        for field in ('FGDepthTextureFlipY', 'FGMotionTextureFlipY', 'FGMotionYInverted'):
            self.assertIn(field, block)
        self.assertNotRegex(block, r'config->DlssNr\w*\s*=')
        policy = source('framegen/GuideOrientation.h')
        self.assertNotIn('DlssNr', policy)


if __name__ == '__main__':
    unittest.main(verbosity=2)
