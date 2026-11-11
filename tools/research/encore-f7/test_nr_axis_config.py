"""Finite native-axis/pair references, using synthetic inputs only."""
from fractions import Fraction
import json
import math
from pathlib import Path
import random
import struct
import unittest

from nr_axis_config import (PRESETS, NativeAxis, axis_from_native_inputs,
                            pair_from_percentages, pair_from_preset)
from nr_warp import native_axis_from_quantized


def f32(value):
    return struct.unpack('<f', struct.pack('<f', float(value)))[0]


def pair(**changes):
    return pair_from_percentages(**(dict(enabled=True, width_percent=50, height_percent=50,
                                        falloff_percent=25, edge_percent=50) | changes))


class NativeAxisConfigTests(unittest.TestCase):
    def test_complete_24_byte_layout_and_zero_falloff_golden(self):
        a = axis_from_native_inputs(10000, 5000, 0, 0.5, 0.5)
        self.assertEqual(a.to_bytes().hex(), '000800c00004403f0000003f88130000000000000000003f')
        self.assertEqual(len(a.to_bytes()), 24)
        self.assertEqual(a.packed, 0xc0000800)
        self.assertEqual(a.transition_pixels, 0)
        self.assertEqual(a.boundary_derivative, 0.5)

    def test_zero_falloff_factory_feeds_existing_shared_warp_reference(self):
        p = pair(falloff_percent=0)
        for native in (p.x, p.y):
            axis = native.shader_axis()
            other = native_axis_from_quantized(native.packed, native.center)
            self.assertEqual(axis, other)
            self.assertEqual(axis.b, 0)

    def test_quantization_and_full_transition_tail(self):
        a = axis_from_native_inputs(10000, 0, 5000, 0.25, 0.5)
        self.assertEqual(a.packed, 0xa0800000)
        self.assertEqual(a.gmax, 0.625)
        self.assertEqual(a.transition_pixels, 5000)
        self.assertEqual(a.boundary_derivative, 0.25)

    def test_saturated_falloff_and_partial_transition_derivative(self):
        a = axis_from_native_inputs(10000, 0, 10000, 0.25, 0.5)
        self.assertEqual((a.packed >> 12) & 4095, 4095)
        b, t, k = Fraction(4095, 2048), Fraction(2048, 4095), Fraction(3, 4)
        self.assertEqual(a.gmax, f32(b*(t-k*(t**3-t**4/2))))
        self.assertEqual(a.boundary_derivative, f32(1-k*(3*t*t-2*t*t*t)))
        self.assertEqual(a.transition_pixels, 5000)
        self.assertGreater(a.boundary_derivative, 0.25)

    def test_center_clamp_and_negative_zero_storage(self):
        low = axis_from_native_inputs(10000, 5000, 2500, 0.5, -1)
        high = axis_from_native_inputs(10000, 5000, 2500, 0.5, 2)
        negzero = axis_from_native_inputs(10000, 5000, 2500, 0.5, -0.0)
        self.assertEqual((low.center, high.center), (0, 1))
        self.assertEqual(negzero.to_bytes()[8:12], bytes.fromhex('00000080'))
        self.assertEqual(math.copysign(1, negzero.center), -1)

    def test_disabled_when_extent_too_small_retains_input_aux(self):
        a = axis_from_native_inputs(15, 7, 10, 0.5, 0.25)
        self.assertEqual(a, NativeAxis(center=0.25, inner_pixels=7))

    def test_disabled_when_inner_covers_extent(self):
        a = axis_from_native_inputs(10000, 20000, 100, 0.5, 0.25)
        self.assertEqual(a, NativeAxis(center=0.25, inner_pixels=10000))

    def test_disabled_when_edge_rounds_to_128(self):
        for edge in (1, 2, 0.999):
            self.assertEqual(axis_from_native_inputs(10000, 5000, 2500, edge, 0.25),
                             NativeAxis(center=0.25, inner_pixels=5000))

    def test_edge_clamp_and_successful_code_limits(self):
        low = axis_from_native_inputs(10000, 5000, 2500, -1, 0.5)
        high = axis_from_native_inputs(10000, 5000, 2500, 0.99, 0.5)
        self.assertEqual((low.packed >> 24) & 127, 32)
        self.assertEqual((high.packed >> 24) & 127, 127)

    def test_falloff_is_clamped_before_quantization(self):
        self.assertEqual(axis_from_native_inputs(10000, 5000, 10000, 0.5, 0.5),
                         axis_from_native_inputs(10000, 5000, 0xffffffff, 0.5, 0.5))

    def test_existing_quantized_gmax_matches_nonzero_falloff_producer(self):
        randomizer = random.Random(20261107)
        for _ in range(200):
            a = axis_from_native_inputs(10000, randomizer.randrange(10000),
                                        randomizer.randrange(1, 10001),
                                        randomizer.uniform(0.25, 0.99), randomizer.random())
            axis = native_axis_from_quantized(a.packed, a.center)
            self.assertEqual((a.gmax, a.center), (axis.gmax, axis.center))

    def test_config_disabled_returns_defaults_not_user_centers(self):
        p = pair(enabled=False, center_x=0.1, center_y=0.9)
        self.assertEqual((p.x, p.y), (NativeAxis(), NativeAxis()))
        self.assertEqual(p.disabled_reason, 'configuration-disabled')

    def test_pair_serializes_two_axis_structs_not_guide_cb(self):
        p = pair()
        self.assertEqual(len(p.to_bytes()), 48)
        self.assertEqual(p.to_bytes(), p.x.to_bytes()+p.y.to_bytes())

    def test_percent_clamps_match_explicit_clamped_values(self):
        self.assertEqual(pair(width_percent=101, height_percent=200, falloff_percent=999, edge_percent=0),
                         pair(width_percent=100, height_percent=100, falloff_percent=100, edge_percent=25))
        self.assertEqual(pair(edge_percent=999), pair(edge_percent=100))

    def test_product_gate_clears_only_packed_and_gmax(self):
        p = pair(width_percent=95, height_percent=95, falloff_percent=0, center_x=0.484375, center_y=0.515625)
        self.assertEqual(p.disabled_reason, 'less-than-five-percent-grid-saving')
        for a, center in ((p.x, 0.484375), (p.y, 0.515625)):
            self.assertEqual((a.packed, a.gmax, a.center, a.inner_pixels), (0, 1, center, 9500))
            self.assertEqual(a.boundary_derivative, 0.5)

    def test_single_disabled_axis_does_not_automatically_disable_other(self):
        p = pair(width_percent=89, height_percent=100, falloff_percent=0)
        self.assertIsNone(p.disabled_reason)
        self.assertNotEqual(p.x.packed, 0)
        self.assertEqual(p.y.packed, 0)

    def test_single_active_axis_can_fail_product_gate(self):
        p = pair(width_percent=95, height_percent=100, falloff_percent=0)
        self.assertEqual(p.disabled_reason, 'less-than-five-percent-grid-saving')
        self.assertEqual((p.x.packed, p.y.packed), (0, 0))

    def test_both_disabled_axes_have_distinct_reason(self):
        p = pair(width_percent=100, height_percent=100)
        self.assertEqual(p.disabled_reason, 'both-axes-disabled')

    def test_explicit_edge_is_float32_division(self):
        p = pair(edge_percent=33)
        a = axis_from_native_inputs(10000, 5000, 2500, f32(33/100), 0.5)
        self.assertEqual(p.x, a)

    def test_three_presets_match_fixed_table_bytes_and_values(self):
        path = Path(__file__).parent/'evidence/nr-warp/native-warp-presets.json'
        rows = json.loads(path.read_text())
        self.assertEqual(len(rows), 3)
        for row, preset in zip(rows, PRESETS):
            pointer, *values = struct.unpack('<Q4I', bytes.fromhex(row['bytes']))
            self.assertEqual(pointer, 0x180000000+int(row['name_rva'], 16))
            self.assertEqual((row['name'], *values), preset)

    def test_presets_match_explicit_path_and_keep_caller_centers(self):
        for index, (_, width, height, falloff, edge) in enumerate(PRESETS):
            expected = pair(width_percent=width, height_percent=height, falloff_percent=falloff,
                            edge_percent=edge, center_x=0.25, center_y=0.75)
            self.assertEqual(pair_from_preset(index, enabled=True, center_x=0.25, center_y=0.75), expected)

    def test_preset_disabled_uses_default_axis_structs(self):
        for index in range(3):
            p = pair_from_preset(index, enabled=False, center_x=0.1, center_y=0.9)
            self.assertEqual((p.x, p.y), (NativeAxis(), NativeAxis()))

    def test_unknown_preset_indices_rejected_instead_of_table_guess(self):
        for value in (-1, 3, 4, 255, True, 1.0):
            with self.assertRaises(ValueError):
                pair_from_preset(value, enabled=True)

    def test_bad_dimensions_percentages_and_nonfinite_fail_closed(self):
        for args in ((0, 1, 1, 0.5, 0.5), (16385, 1, 1, 0.5, 0.5),
                     (100, -1, 1, 0.5, 0.5), (100, 1, -1, 0.5, 0.5),
                     (True, 1, 1, 0.5, 0.5), (100, 1, 1, math.nan, 0.5),
                     (100, 1, 1, 0.5, math.inf)):
            with self.assertRaises(ValueError):
                axis_from_native_inputs(*args)
        for changes in ({'enabled': 1}, {'width_percent': -1}, {'edge_percent': True},
                        {'height_percent': 2**32}, {'center_y': math.nan}):
            with self.assertRaises(ValueError):
                pair(**changes)


if __name__ == '__main__':
    unittest.main(verbosity=2)
