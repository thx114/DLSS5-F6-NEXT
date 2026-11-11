import json
import math
import struct
from pathlib import Path
import unittest

import nr_codec
import nr_warp as n


def synthetic_axis(slope_bits=64, center=0.5):
    # Explicit analyst-selected parameters; not a recovered native CPU writer.
    a = n.Axis(0x80000000 | (slope_bits << 24) | (1024 << 12) | 1024, 1, center)
    gmax = (a.curve(2*center)+a.curve(2*(1-center)))/2
    return n.Axis(a.packed, gmax, center)


class WarpTests(unittest.TestCase):
    def test_packed_bit_fields(self):
        a = synthetic_axis()
        self.assertEqual(a.packed, 0xc0400400)
        self.assertTrue(a.enabled)
        self.assertEqual((a.a, a.b, a.slope), (0.2500610649585724, 0.5, 0.5))
        self.assertEqual(a.gmax, 0.7500305324792862)

    def test_piecewise_polynomial_and_join_values(self):
        a = synthetic_axis()
        for d, expected in [(0, 0), (a.a, a.a), (a.a+0.25, a.a+0.2265625),
                            (a.a+0.5, a.a+0.375), (1, 0.625+a.a/2), (1.5, 0.875+a.a/2)]:
            self.assertEqual(a.curve(d), expected)

    def test_piecewise_join_derivatives(self):
        a = synthetic_axis()
        h = 1e-6
        for d, slope in [(a.a, 1), (a.a+a.b, a.slope)]:
            for delta in (-h, h):
                self.assertAlmostEqual((a.curve(d+delta)-a.curve(d))/delta, slope, delta=1e-7)

    def test_centered_forward_golden_and_symmetry(self):
        a = synthetic_axis()
        self.assertAlmostEqual(n.Axis(a.packed, 0.75, 0.5).forward(0.25), 0.18228149103803587)
        for i in range(101):
            u = i/100
            self.assertAlmostEqual(a.forward(u)+a.forward(1-u), 1)

    def test_offcenter_endpoint_normalization_is_explicit(self):
        a = synthetic_axis(center=0.25)
        self.assertEqual(a.gmax, 0.7383041479611162)
        self.assertEqual(a.forward(0), 0)
        self.assertEqual(a.forward(1), 1)
        self.assertAlmostEqual(a.inverse_shader(0), 0, delta=1e-14)
        self.assertAlmostEqual(a.inverse_shader(1), 1, delta=1e-14)
        # Center need not map to work UV .5 in an off-center warp.
        self.assertNotEqual(a.forward(a.center), 0.5)

    def test_forward_monotonic_and_inverse_with_ordinary_synthetic_slope(self):
        for center in (0, 0.25, 0.5, 0.8, 1):
            a = synthetic_axis(center=center)
            values = [a.forward(i/1000) for i in range(1001)]
            self.assertTrue(all(x < y for x, y in zip(values, values[1:])))
            for i in range(101):
                self.assertAlmostEqual(a.inverse_shader(a.forward(i/100)), i/100, delta=2e-13)

    def test_inverse_four_steps_is_not_an_exact_inverse(self):
        a = synthetic_axis(slope_bits=1)
        original = a.a+a.b*0.99  # Strictly inside transition, not endpoint branch.
        mapped = a.curve(original)
        self.assertLess(mapped, a.curve(a.a+a.b))
        self.assertAlmostEqual(a.inverse_curve_shader(mapped)-original, -0.02752672255631161, delta=1e-12)
        self.assertGreater(abs(a.inverse_curve_shader(mapped)-original), 0.02)

    def test_inverse_outer_branch_is_linear_including_exact_join(self):
        a = synthetic_axis(slope_bits=1)
        for d in (a.a+a.b, 1, 1.5):
            self.assertAlmostEqual(a.inverse_curve_shader(a.curve(d)), d, delta=1e-14)

    def test_disabled_axis_is_identity_even_with_nondefault_auxiliary_fields(self):
        a = n.Axis(0x00400400, 0.25, 0.7)
        for u in (-0.5, 0, 0.25, 1, 1.5):
            self.assertEqual(a.forward(u), u)
            self.assertEqual(a.inverse_shader(u), u)

    def test_color_matching_extent_bypass_is_per_axis(self):
        a = synthetic_axis()
        self.assertEqual(n.codec_coordinate(0.25, 8, 8, a), 0.25)
        self.assertEqual(n.codec_coordinate(0.25, 8, 8, a, inverse=True), 0.25)
        self.assertNotEqual(n.codec_coordinate(0.25, 8, 4, a), 0.25)
        self.assertEqual(n.encode_box_axis(8, 8, 3, a), ((3, 1.0),))

    def test_footprint_without_warp_matches_existing_rectangle_reference(self):
        for out in range(1, 10):
            for work in range(1, 10):
                for i in range(work):
                    actual = dict(n.encode_box_axis(out, work, i))
                    expected = dict(nr_codec.box_axis(out, work, i))
                    for pixel in actual.keys() | expected.keys():
                        self.assertAlmostEqual(actual.get(pixel, 0), expected.get(pixel, 0), delta=1e-13)

    def test_warp_encode_footprints_cover_source_axis(self):
        for center in (0.25, 0.5, 0.8):
            a = synthetic_axis(center=center)
            coverage = [0.0]*64
            for i in range(32):
                for pixel, weight in n.encode_box_axis(64, 32, i, a):
                    self.assertGreater(weight, 0)
                    coverage[pixel] += weight
            for total in coverage:
                self.assertAlmostEqual(total, 1, places=12)

    def test_decode_clamps_indices_but_keeps_fraction(self):
        self.assertEqual(n.decode_axis(4, 2, 0), (0, 0, 0.75))
        self.assertEqual(n.decode_axis(4, 2, 3), (1, 1, 0.25))
        self.assertEqual(n.decode_axis(8, 8, 3, synthetic_axis()), (3, 4, 0.0))

    def test_guides_have_independent_depth_and_motion_rectangles(self):
        p = n.guide_position((1, 2), (4, 4), (n.Axis(), n.Axis()))
        self.assertEqual(p, (0.375, 0.625))
        self.assertEqual(n.nearest_address(p, (8, 16), (3, 5)), (6, 15))
        self.assertEqual(n.nearest_address(p, (4, 4), (20, 30)), (21, 32))
        self.assertEqual(n.nearest_address((-0.01, 1.01), (8, 8), (3, 5)), (3, 12))

    def test_guides_do_not_apply_color_extent_bypass(self):
        a = synthetic_axis()
        p = n.guide_position((0, 0), (8, 8), (a, n.Axis()))
        self.assertNotEqual(p[0], (0.5/8))
        self.assertEqual(p[1], (0.5/8))

    def test_unwarped_motion_reduces_to_uv_displacement_times_work_raster(self):
        self.assertEqual(n.guide_motion((3, 4), (16, 8), (n.Axis(), n.Axis()),
                                        (2, -3), (1/32, 1/16)), (1, -1.5))

    def test_warp_motion_uses_endpoint_difference_including_offscreen_endpoint(self):
        a = synthetic_axis(center=0.25)
        pixel, size = (4, 6), (16, 8)
        p = n.guide_position(pixel, size, (a, a))
        motion, scale = (16, -4), (1/16, 1/8)
        actual = n.guide_motion(pixel, size, (a, a), motion, scale)
        self.assertGreater(p[0]+motion[0]*scale[0], 1)
        for j in range(2):
            expected = (a.forward(p[j]+motion[j]*scale[j])-(pixel[j]+0.5)/size[j])*size[j]
            self.assertAlmostEqual(actual[j], expected)
        self.assertGreater(abs(actual[0]-motion[0]), 1)

    def test_four_step_inverse_residual_is_retained_for_zero_motion(self):
        a = synthetic_axis(slope_bits=1)
        actual = n.guide_motion((255, 0), (256, 1), (a, n.Axis()), (0, 0), (1, 1))
        self.assertAlmostEqual(actual[0], -0.09027797031595242, delta=2e-11)
        self.assertEqual(actual[1], 0)

    def test_invalid_inputs_fail_closed(self):
        calls = [lambda: n.Axis(True), lambda: n.Axis(0x100000000),
                 lambda: n.Axis(0x80000000),
                 lambda: n.Axis(0x80400000), lambda: n.Axis(gmax=0),
                 lambda: n.Axis(center=math.nan), lambda: n.Axis(center=-0.1),
                 lambda: n.decode_axis(0, 1, 0), lambda: n.decode_axis(1, 1, True),
                 lambda: n.nearest_address((0, 0), (0, 8)),
                 lambda: n.nearest_address((0, 0), (8, 8), (16380, 0)),
                 lambda: n.guide_motion((0, 0), (1, 1), (n.Axis(),), (0, 0), (1, 1)),
                 lambda: synthetic_axis().forward(math.inf)]
        for call in calls:
            with self.assertRaises(ValueError):
                call()

    def test_guide_bytecode_contains_fixed_newton_and_sampling_anchors(self):
        path = Path(__file__).parent/'evidence/nr-warp/nr-guides.tokens.json'
        rows = {int(r['offset'], 16): r for r in json.loads(path.read_text())}
        for offset in (0x70c, 0x878, 0x9e4, 0xb50):
            self.assertTrue(rows[offset]['asm'].startswith('add_sat '))
        self.assertEqual(rows[0x313c]['asm'], 'mad r1.zw, r1.zzzw, CB0[0][2].zzzw, r3.xxxy')
        self.assertEqual(rows[0x4a40]['asm'], 'add r0.xyzw, -r0.xyzw, r3.xyxx')
        self.assertEqual(rows[0x4a60]['asm'], 'mul r0.xyzw, r1.xyxx, r0.xyzw')

    def test_axis_a_scale_is_original_float32_immediate_not_pretty_printed_decimal(self):
        root = Path(__file__).parent/'evidence'
        for profile, name, offset in [('nr-warp', 'nr-guides', 0x25c),
                                      ('nr-codec', 'nr-encode', 0x370),
                                      ('nr-codec', 'nr-decode', 0x7d0)]:
            rows = json.loads((root/profile/(name+'.tokens.json')).read_text())
            raw = bytes.fromhex(next(r['bytes'] for r in rows if int(r['offset'], 16) == offset))
            self.assertEqual(struct.unpack('<I', raw[-4:])[0], 0x39800801)
        self.assertEqual(n.AXIS_A_SCALE, struct.unpack('<f', struct.pack('<I', 0x39800801))[0])
        self.assertNotEqual(n.AXIS_A_SCALE, 1/4096)

    def test_native_gmax_uses_cpu_division_and_float32_storage(self):
        packed = 0xc0400400
        expected = struct.unpack('<f', struct.pack('<f', 0.625+0.5*(1024/4095)))[0]
        a = n.native_axis_from_quantized(packed, 0.5)
        self.assertEqual(a.gmax, expected)
        self.assertNotEqual(a.gmax, a.curve(1))
        self.assertEqual(n.native_axis_from_quantized(packed, -1).center, 0)
        self.assertEqual(n.native_axis_from_quantized(packed, 2).center, 1)
        self.assertEqual(n.native_axis_from_quantized(0, 0.25).gmax, 1)
        with self.assertRaises(ValueError):
            n.native_axis_from_quantized(synthetic_axis(slope_bits=1).packed, 0.5)

    def test_zero_transition_uses_inner_outer_branches_without_newton(self):
        for center in (0, 0.25, 0.5, 1):
            axis = n.native_axis_from_quantized(0xc0000800, center)
            self.assertEqual(axis.b, 0)
            self.assertEqual(axis.curve(axis.a), axis.a)
            self.assertEqual(axis.curve(axis.a+1), axis.a+0.5)
            self.assertEqual(axis.inverse_curve_shader(axis.a+0.25), axis.a+0.5)
            for u in (-0.25, 0, 0.1, 0.25, 0.5, 0.75, 1, 1.25):
                self.assertAlmostEqual(axis.inverse_shader(axis.forward(u)), u, delta=2e-14)

    def test_native_quantized_center_preserves_negative_zero(self):
        a = n.native_axis_from_quantized(0xc0400400, -0.0)
        self.assertEqual(struct.pack('<f', a.center).hex(), '00000080')


if __name__ == '__main__':
    unittest.main()
