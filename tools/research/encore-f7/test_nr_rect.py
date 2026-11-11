"""Independent stdlib CPU checks; no payload, evidence, decoder or GPU imports."""

from dataclasses import FrozenInstanceError, astuple, replace
from itertools import product
import unittest

from nr_rect import DescriptorRect, ResolvedRect, Resolution, resolve_rect


U32_MAX = (1 << 32) - 1


class RectSemanticsTests(unittest.TestCase):
    def test_zero_request_uses_full_texture(self):
        result = resolve_rect(DescriptorRect(0, 0, 0, 0), 1920, 1080)
        self.assertEqual(result, Resolution(
            True, ResolvedRect(0, 0, 1920, 1080, 1920, 1080), "accepted"))

    def test_explicit_request_preserves_origin_and_size(self):
        result = resolve_rect(DescriptorRect(20, 30, 400, 250), 1920, 1080)
        self.assertTrue(result.accepted)
        self.assertEqual(astuple(result.output), (20, 30, 400, 250, 1920, 1080))

    def test_zero_extent_defaults_each_axis_independently(self):
        for request, expected in (
            (DescriptorRect(0, 3, 0, 2), (0, 3, 10, 2, 10, 8)),
            (DescriptorRect(3, 0, 2, 0), (3, 0, 2, 8, 10, 8)),
        ):
            with self.subTest(request=request):
                result = resolve_rect(request, 10, 8)
                self.assertTrue(result.accepted)
                self.assertEqual(astuple(result.output), expected)

    def test_nonzero_origin_plus_zero_extent_does_not_mean_remaining(self):
        for request, expected in (
            (DescriptorRect(1, 0, 0, 1), (1, 0, 10, 1, 10, 8)),
            (DescriptorRect(0, 1, 1, 0), (0, 1, 1, 8, 10, 8)),
            (DescriptorRect(1, 1, 0, 0), (1, 1, 10, 8, 10, 8)),
        ):
            with self.subTest(request=request):
                result = resolve_rect(request, 10, 8)
                self.assertFalse(result.accepted)
                self.assertEqual(result.diagnostic, "extent-out-of-bounds")
                self.assertEqual(astuple(result.output), expected)

    def test_exact_remaining_extent_and_last_pixel_are_accepted(self):
        for request in (DescriptorRect(3, 2, 7, 6), DescriptorRect(9, 7, 1, 1)):
            with self.subTest(request=request):
                self.assertTrue(resolve_rect(request, 10, 8).accepted)

    def test_origin_equal_to_texture_fails_extent_check(self):
        for request in (DescriptorRect(10, 0, 1, 1), DescriptorRect(0, 8, 1, 1)):
            with self.subTest(request=request):
                result = resolve_rect(request, 10, 8)
                self.assertFalse(result.accepted)
                self.assertEqual(result.diagnostic, "extent-out-of-bounds")
                self.assertEqual(astuple(result.output), (*astuple(request), 10, 8))

    def test_origin_beyond_texture_fails_before_subtraction(self):
        for request in (DescriptorRect(11, 0, 1, 1), DescriptorRect(0, 9, 1, 1)):
            with self.subTest(request=request):
                result = resolve_rect(request, 10, 8)
                self.assertFalse(result.accepted)
                self.assertEqual(result.diagnostic, "origin-out-of-bounds")
                self.assertEqual(astuple(result.output), (*astuple(request), 10, 8))

    def test_oversized_request_is_preserved_not_clipped(self):
        for request in (DescriptorRect(3, 0, 8, 1), DescriptorRect(0, 2, 1, 7)):
            with self.subTest(request=request):
                result = resolve_rect(request, 10, 8)
                self.assertFalse(result.accepted)
                self.assertEqual(astuple(result.output), (*astuple(request), 10, 8))

    def test_all_uint32_texture_extents_are_not_outer_clamped(self):
        for width, height in ((1, 1), (63, 2), (16385, 20000), (U32_MAX, U32_MAX)):
            with self.subTest(width=width, height=height):
                result = resolve_rect(DescriptorRect(0, 0, 0, 0), width, height)
                self.assertTrue(result.accepted)
                self.assertEqual(astuple(result.output), (0, 0, width, height, width, height))

    def test_uint32_max_exact_boundary(self):
        result = resolve_rect(DescriptorRect(U32_MAX - 1, U32_MAX - 1, 1, 1),
                              U32_MAX, U32_MAX)
        self.assertTrue(result.accepted)
        self.assertEqual(result.output.width, 1)
        self.assertEqual(result.output.height, 1)

    def test_wrapped_addition_cannot_hide_extent_overflow(self):
        for request in (
            DescriptorRect(U32_MAX - 1, 0, 2, 1),
            DescriptorRect(0, U32_MAX - 1, 1, 2),
            DescriptorRect(1, 0, U32_MAX, 1),
            DescriptorRect(0, 1, 1, U32_MAX),
        ):
            with self.subTest(request=request):
                result = resolve_rect(request, U32_MAX, U32_MAX)
                self.assertFalse(result.accepted)
                self.assertEqual(result.diagnostic, "extent-out-of-bounds")
                self.assertEqual(astuple(result.output), (*astuple(request), U32_MAX, U32_MAX))

    def test_zero_texture_dimension_publishes_both_dimensions_only(self):
        request = DescriptorRect(7, 8, 9, 10)
        for width, height in ((0, 0), (0, 17), (17, 0), (0, U32_MAX), (U32_MAX, 0)):
            with self.subTest(width=width, height=height):
                result = resolve_rect(request, width, height)
                self.assertFalse(result.accepted)
                self.assertEqual(result.diagnostic, "texture-extent-zero")
                self.assertEqual(astuple(result.output), (0, 0, 0, 0, width, height))

    def test_absent_backend_leaves_all_output_zero(self):
        result = resolve_rect(DescriptorRect(2, 3, 4, 5), 99, 100,
                              backend_present=False)
        self.assertEqual(result, Resolution(False, ResolvedRect(0, 0, 0, 0, 0, 0),
                                            "backend-absent"))

    def test_absent_resource_leaves_all_output_zero(self):
        result = resolve_rect(DescriptorRect(2, 3, 4, 5), 99, 100,
                              resource_present=False)
        self.assertEqual(result, Resolution(False, ResolvedRect(0, 0, 0, 0, 0, 0),
                                            "resource-absent"))

    def test_every_sampled_nonzero_query_result_fails_with_zero_output(self):
        for code in (1, 2, 0x7FFFFFFF, 0x80000000, 0xBAD00005, U32_MAX):
            with self.subTest(code=code):
                result = resolve_rect(DescriptorRect(2, 3, 4, 5), 99, 100,
                                      query_result=code)
                self.assertEqual(result, Resolution(
                    False, ResolvedRect(0, 0, 0, 0, 0, 0), "query-nonzero"))

    def test_small_domain_matches_integer_containment_oracle(self):
        # Use unbounded sums in this oracle, independent of the implementation's
        # subtraction checks; explicit edge tests above cover uint32 overflow.
        for tw, th, x, y, width, height in product(range(4), repeat=6):
            expected_width = width or tw
            expected_height = height or th
            expected = (tw > 0 and th > 0 and expected_width > 0 and expected_height > 0
                        and x + expected_width <= tw and y + expected_height <= th)
            result = resolve_rect(DescriptorRect(x, y, width, height), tw, th)
            self.assertEqual(result.accepted, expected, (tw, th, x, y, width, height))
            output = ((x, y, expected_width, expected_height, tw, th)
                      if tw and th else (0, 0, 0, 0, tw, th))
            self.assertEqual(astuple(result.output), output)


class HostInputTests(unittest.TestCase):
    def test_descriptor_fields_reject_nonexact_integers_including_bool(self):
        class IntSubclass(int):
            pass

        for field in ("x", "y", "width", "height"):
            for bad in (True, False, 1.0, "1", None, IntSubclass(1)):
                with self.subTest(field=field, bad=bad), self.assertRaises(TypeError):
                    replace(DescriptorRect(0, 0, 0, 0), **{field: bad})

    def test_descriptor_fields_reject_out_of_range_without_wrapping(self):
        for field in ("x", "y", "width", "height"):
            for bad in (-1, -U32_MAX, U32_MAX + 1, 1 << 64):
                with self.subTest(field=field, bad=bad), self.assertRaises(ValueError):
                    replace(DescriptorRect(0, 0, 0, 0), **{field: bad})

    def test_scalar_arguments_reject_nonexact_integers(self):
        class IntSubclass(int):
            pass

        for field in ("texture_width", "texture_height", "query_result"):
            for bad in (True, False, 1.0, "1", None, IntSubclass(1)):
                values = dict(texture_width=10, texture_height=10, query_result=0)
                values[field] = bad
                with self.subTest(field=field, bad=bad), self.assertRaises(TypeError):
                    resolve_rect(DescriptorRect(0, 0, 0, 0), **values)

    def test_scalar_arguments_reject_out_of_range_without_wrapping(self):
        for field in ("texture_width", "texture_height", "query_result"):
            for bad in (-1, U32_MAX + 1, 1 << 64):
                values = dict(texture_width=10, texture_height=10, query_result=0)
                values[field] = bad
                with self.subTest(field=field, bad=bad), self.assertRaises(ValueError):
                    resolve_rect(DescriptorRect(0, 0, 0, 0), **values)

    def test_presence_flags_require_exact_bool(self):
        for field in ("backend_present", "resource_present"):
            for bad in (0, 1, -1, 0.0, None, "true", []):
                with self.subTest(field=field, bad=bad), self.assertRaises(TypeError):
                    resolve_rect(DescriptorRect(0, 0, 0, 0), 1, 1, **{field: bad})

    def test_invalid_host_inputs_are_not_hidden_by_native_early_returns(self):
        for flags in (dict(backend_present=False), dict(resource_present=False),
                      dict(query_result=1)):
            with self.subTest(flags=flags), self.assertRaises(ValueError):
                resolve_rect(DescriptorRect(0, 0, 0, 0), -1, 0, **flags)
        with self.assertRaises(TypeError):
            resolve_rect(DescriptorRect(0, 0, 0, 0), 1, 1,
                         backend_present=False, resource_present=1)
        with self.assertRaises(ValueError):
            resolve_rect(DescriptorRect(0, 0, 0, 0), 1, 1,
                         backend_present=False, query_result=-1)

    def test_request_must_be_a_descriptor(self):
        for bad in (None, (0, 0, 0, 0), dict(x=0, y=0, width=0, height=0)):
            with self.subTest(bad=bad), self.assertRaises(TypeError):
                resolve_rect(bad, 1, 1)

    def test_public_value_objects_are_immutable(self):
        request = DescriptorRect(0, 0, 0, 0)
        result = resolve_rect(request, 1, 1)
        for obj, field, value in ((request, "x", 1), (result.output, "width", 0),
                                  (result, "accepted", False), (result, "diagnostic", "x")):
            with self.subTest(field=field), self.assertRaises(FrozenInstanceError):
                setattr(obj, field, value)


if __name__ == "__main__":
    unittest.main()
