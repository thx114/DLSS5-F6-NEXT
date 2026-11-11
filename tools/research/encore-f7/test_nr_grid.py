"""Bounded synthetic integer/grid tests, not provider/GPU performance tests."""
import math
import random
import struct
import unittest

from nr_grid import (base_work_extent, grid_cost, native_round_int32, optimize_grid,
                     warped_work_extent)
from nr_warp import Axis, native_axis_from_quantized


class NrGridTests(unittest.TestCase):
    def test_halfway_rounds_away_from_zero_not_python_even(self):
        for x, expected in ((0.5, 1), (1.5, 2), (2.5, 3), (-0.5, -1), (-2.5, -3),
                            (0.0, 0), (-0.0, 0), (2147483647.0, 2147483647),
                            (-2147483648.0, -2147483648)):
            self.assertEqual(native_round_int32(x), expected)

    def test_neighbors_of_half_do_not_use_add_half_double_rounding(self):
        self.assertEqual(native_round_int32(math.nextafter(0.5, 0.0)), 0)
        self.assertEqual(native_round_int32(math.nextafter(-0.5, 0.0)), 0)
        self.assertEqual(native_round_int32(math.nextafter(0.5, 1.0)), 1)
        self.assertEqual(native_round_int32(math.nextafter(-0.5, -1.0)), -1)

    def test_subnormal_rounds_to_zero(self):
        self.assertEqual(native_round_int32(math.ulp(0.0)), 0)
        self.assertEqual(native_round_int32(-math.ulp(0.0)), 0)

    def test_round_matches_native_double_bit_sequence(self):
        # Directly translates 504060's shift/ADC/mask sequence, independently
        # of nr_grid's modf implementation. Only finite int32-result inputs.
        def bit_round(x):
            bits = struct.unpack('<Q', struct.pack('<d', x))[0]
            exponent = (bits >> 52) & 0x7ff
            if exponent < 1023:
                bits = (bits & (1 << 63)) | ((1023 << 52) if exponent == 1022 else 0)
            elif exponent < 1075:
                shift = 1075-exponent
                bits = (((bits >> shift)+((bits >> (shift-1)) & 1)) << shift) & (2**64-1)
            return int(struct.unpack('<d', struct.pack('<Q', bits))[0])
        randomizer = random.Random(504060)
        values = [randomizer.uniform(-2147483647, 2147483646) for _ in range(2000)]
        for half in (-1000.5, -2.5, -0.5, 0.5, 2.5, 1000.5):
            values += [math.nextafter(half, -math.inf), half, math.nextafter(half, math.inf)]
        for x in values:
            self.assertEqual(native_round_int32(x), bit_round(x))

    def test_round_rejects_exceptional_native_domain(self):
        for x in (float('nan'), float('inf'), -float('inf'), 2**31-0.5,
                  -(2**31)-0.5, 2**10000, True, '1'):
            with self.subTest(x=str(x)[:20]), self.assertRaises(ValueError):
                native_round_int32(x)

    def test_grid_score_literal_cases(self):
        for wh, levels, padded, extra, score in (
                ((64, 64), (4, 4), (320, 320), 16, 107520),
                ((65, 65), (6, 6), (320, 320), 0, 102400),
                ((640, 360), (6, 7), (640, 384), 0, 245760),
                ((1280, 720), (6, 6), (1280, 768), 64, 1032192),
                ((1920, 1080), (6, 7), (1920, 1152), 0, 2211840),
                ((3840, 2160), (6, 6), (3840, 2176), 0, 8355840),
                ((16384, 16384), (6, 6), (16384, 16384), 64, 269484032)):
            result = grid_cost(*wh)
            self.assertEqual((result.levels, result.padded_extent, result.extra_width, result.score),
                             (levels, padded, extra, score))

    def test_score_is_not_monotonic_pixel_area_or_symmetric_padding(self):
        self.assertLess(grid_cost(65, 65).score, grid_cost(64, 64).score)
        small = grid_cost(64, 64)
        self.assertEqual(small.score, (320+16)*320)
        self.assertNotEqual(small.score, (320+16)*(320+16))

    def test_repeated_floor_halves_are_not_repeated_ceil_halves(self):
        self.assertEqual(grid_cost(65, 64).levels, (6, 4))
        self.assertEqual(grid_cost(1920, 1081).levels, (6, 6))

    def test_alignment_and_extra_column_conditions(self):
        for width in range(64, 300):
            r = grid_cost(width, 257)
            for p, step in zip(r.padded_extent, r.steps):
                self.assertGreaterEqual(p, 320)
                if p > 320:
                    self.assertEqual(p % step, 0)
            expected = r.steps[0] if all(p % (4*s) == 0 for p, s in zip(r.padded_extent, r.steps)) else 0
            self.assertEqual(r.extra_width, expected)

    def test_known_search_outputs(self):
        for wh, expected in (((64, 64), (64, 64)), ((640, 360), (640, 360)),
                             ((1280, 720), (1272, 716)), ((1600, 900), (1594, 896)),
                             ((1920, 1080), (1907, 1072)), ((2560, 1440), (2560, 1440)),
                             ((3840, 2160), (3776, 2124)), ((16384, 16384), (16064, 16064))):
            self.assertEqual(optimize_grid(*wh), expected)

    def test_search_preserves_native_initial_candidate_on_equal_score(self):
        self.assertEqual(optimize_grid(2560, 1440), (2560, 1440))

    def test_search_stays_in_two_percent_rectangle_and_never_worsens_score(self):
        randomizer = random.Random(20261107)
        for _ in range(120):
            width, height = randomizer.randint(64, 4096), randomizer.randint(64, 4096)
            w, h = optimize_grid(width, height)
            self.assertTrue(max(64, (98*width+99)//100) <= w <= width)
            self.assertTrue(max(64, (98*height+99)//100) <= h <= height)
            self.assertLessEqual(grid_cost(w, h).score, grid_cost(width, height).score)

    def test_search_matches_exhaustive_rectangle_selection(self):
        # Independent enumeration of the small search rectangle tests search
        # ordering/ties and floor/ceil eligibility; score itself is shared.
        for width, height, rw, rh in ((320, 181, 1920, 1080), (777, 439, 1920, 1080),
                                      (129, 131, 1, 1), (1920, 1080, 1920, 1080)):
            best = width, height
            best_key = grid_cost(*best).score, -width*height
            for cw in range(max(64, (98*width+99)//100), width+1):
                for ch in range(max(64, (98*height+99)//100), height+1):
                    if not abs(cw*rh-ch*rw) < rw:
                        continue
                    if 400*abs(cw*rh-ch*rw) > rw*ch:
                        continue
                    key = grid_cost(cw, ch).score, -cw*ch
                    if key < best_key:
                        best, best_key = (cw, ch), key
            self.assertEqual(optimize_grid(width, height, rw, rh), best)

    def test_one_zero_reference_replaces_both_reference_dimensions(self):
        plain = optimize_grid(1920, 1080)
        self.assertEqual(optimize_grid(1920, 1080, 0, 99), plain)
        self.assertEqual(optimize_grid(1920, 1080, 99, 0), plain)

    def test_dissimilar_reference_can_leave_original_fallback(self):
        self.assertEqual(optimize_grid(1920, 1080, 1, 1), (1920, 1080))

    def test_search_invalid_grid_uint32_returns_unchanged(self):
        for wh in ((0, 100), (63, 100), (100, 16385), (0xffffffff, 100)):
            self.assertEqual(optimize_grid(*wh), wh)

    def test_bounded_and_strict_grid_input(self):
        for args in ((True, 100), (-1, 100), (100.0, 100), (2**32, 100)):
            with self.assertRaises(ValueError):
                optimize_grid(*args)
        with self.assertRaises(ValueError):
            optimize_grid(100, 100, 20000, 20000)
        for wh in ((0, 100), (63, 100), (16385, 100)):
            with self.assertRaises(ValueError):
                grid_cost(*wh)

    def test_positive_scale_clamps_float32_and_ignores_external_limits(self):
        self.assertEqual(base_work_extent(1920, 1080, 0.1, external_width=50, external_height=50), (634, 356))
        self.assertEqual(base_work_extent(1920, 1080, 1.1), (1920, 1080))
        self.assertEqual(base_work_extent(65, 67, 0.5), (33, 34))

    def test_nonpositive_scale_needs_both_limits(self):
        self.assertEqual(base_work_extent(1920, 1080, 0, external_width=1280, external_height=800), (1280, 800))
        self.assertEqual(base_work_extent(1920, 1080, -1, external_width=1280), (1920, 1080))
        self.assertEqual(base_work_extent(1920, 1080, 0, external_height=800), (1920, 1080))

    def test_base_optimizer_uses_original_aspect_reference(self):
        work = base_work_extent(1920, 1080, 0, external_width=1280, external_height=800)
        self.assertEqual(base_work_extent(1920, 1080, 0, external_width=1280, external_height=800, optimize=True),
                         optimize_grid(*work, 1920, 1080))
        self.assertEqual(work, (1280, 800))

    def test_disabled_axes_do_not_imply_16_pixel_minimum(self):
        self.assertEqual(warped_work_extent((3, 7), (Axis(), Axis())), (3, 7))

    def test_enabled_axes_use_float32_gmax_then_round_and_min16(self):
        axis = Axis(0xA0401000, 0.5, 0.5)
        self.assertEqual(warped_work_extent((65, 7), (axis, axis)), (33, 16))
        self.assertEqual(warped_work_extent((65, 7), (axis, Axis())), (33, 7))

    def test_second_optimizer_is_after_warp_with_current_aspect(self):
        axis = native_axis_from_quantized(0xA0401000, 0.5)
        base = base_work_extent(1920, 1080, 0.9, optimize=True)
        warped = warped_work_extent(base, (axis, Axis()))
        self.assertEqual(warped_work_extent(base, (axis, Axis()), optimize=True), optimize_grid(*warped))

    def test_reference_extent_does_not_silently_pass_nan_or_overflow(self):
        for scale in (float('nan'), float('inf'), -float('inf'), 2**10000, True):
            with self.assertRaises(ValueError):
                base_work_extent(1920, 1080, scale)
        with self.assertRaises(ValueError):
            warped_work_extent((16384, 1080), (Axis(0xA0401000, 2), Axis()))
        with self.assertRaises(ValueError):
            base_work_extent(100, 100, 1, optimize=1)
        with self.assertRaises(ValueError):
            warped_work_extent((100, 100), (Axis(1, 0.5), Axis()))


if __name__ == '__main__':
    unittest.main(verbosity=2)
