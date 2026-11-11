"""Standard-library CPU regressions. No DLL, SDK, GPU or camera acceptance."""
import json
import math
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
from cb_probe import analyze
from encore_math import (clip_to_previous, evaluate_motion, float4_buffer, guide_depth,
                         identity, inverse, inverse_pair, matrix, motion_basis, multiply,
                         pack_flow, projection_candidates, residual, robust_weight,
                         transpose, unpack_flow)


def perspective(near=0.1, far=1000.0, sign=1, reverse=False, low=0):
    zn, zf = (1, low) if reverse else (low, 1)
    b = (zn-zf)*near if far is None else (zn-zf)/(1/near-1/far)
    a = zf if far is None else zf-b/far
    return [[1.0, 0, 0, 0], [0, 2.0, 0, 0], [0, 0, sign*a, sign], [0, 0, b, 0]]


class ProjectionTests(unittest.TestCase):
    def check_planes(self, p, low, near, far):
        result = projection_candidates(p)
        self.assertTrue(result['accepted'])
        h = next(x for x in result['hypotheses'] if x['clip_range'] == low)
        self.assertAlmostEqual(h['near'], near)
        if far is None:
            self.assertTrue(h['infinite_far'])
            self.assertIsNone(h['far'])
        else:
            self.assertAlmostEqual(h['far'], far, places=6)
        self.assertFalse(result['camera_authority'])
        return result

    def test_finite_forward_lh_and_rh(self):
        for sign in (-1, 1):
            self.check_planes(perspective(sign=sign), 'zero-to-one', 0.1, 1000)

    def test_reverse_and_infinite(self):
        for sign in (-1, 1):
            for reverse in (False, True):
                for far in (None, 700.0):
                    self.check_planes(perspective(0.25, far, sign, reverse), 'zero-to-one', 0.25, far)

    def test_opengl_range_cannot_be_assumed(self):
        r = self.check_planes(perspective(low=-1), 'minus-one-to-one', 0.1, 1000)
        self.assertEqual(len(r['hypotheses']), 2)

    def test_storage_and_vector_convention_ambiguity(self):
        p = perspective()
        flat = [v for r in p for v in r]
        self.assertEqual(matrix(flat), p)
        self.assertEqual(matrix(flat, 'column-major', 'column'), p)
        self.assertEqual(matrix(flat, 'column-major', 'row'), transpose(p))
        self.assertEqual(matrix(flat, 'row-major', 'column'), transpose(p))
        with self.assertRaises(ValueError):
            matrix(flat, 'UNKNOWN', 'row')

    def test_scaled_mirrored_offaxis_matrix(self):
        p = perspective()
        p[1][1] *= -1
        p[2][0], p[2][1] = 0.02, -0.03
        scaled = [[v*7 for v in r] for r in p]
        r = self.check_planes(scaled, 'zero-to-one', 0.1, 1000)
        self.assertEqual(r['offset_semantics'], 'UNKNOWN')
        self.assertEqual(r['ndc_center'], [0.02, -0.03])
        self.assertAlmostEqual(r['aspect'], 2)

    def test_not_projection_and_nonfinite(self):
        self.assertFalse(projection_candidates(identity())['accepted'])
        p = perspective()
        p[0][1] = 0.5
        self.assertFalse(projection_candidates(p)['accepted'])
        p[0][0] = math.nan
        self.assertFalse(projection_candidates(p)['accepted'])
        with self.assertRaises(ValueError):
            matrix([math.inf]*16)

    def test_small_near_cannot_hide_homogeneous_or_oblique_terms(self):
        for row, col in ((0, 2), (1, 2), (0, 3), (1, 3), (3, 3)):
            p = perspective(near=1e-6, far=1000, reverse=True)
            p[row][col] = 1e-5
            quantized = matrix(struct.unpack('<16f', struct.pack('<16f', *(v for r in p for v in r))))
            self.assertFalse(projection_candidates(quantized)['accepted'])

    def test_small_far_gap_not_rounded_to_infinity(self):
        p = perspective(0.1, 1e12, reverse=True)
        r = projection_candidates(p)
        h = r['hypotheses'][0]
        self.assertAlmostEqual(h['near'], 0.1)
        self.assertAlmostEqual(h['far']/1e12, 1, places=12)
        self.assertFalse(h['infinite_far'])

    def test_captured_numeric_projection_regression(self):
        # Selected numbers only from the captured CB; no semantic attestation.
        values = [1.5088834762573242, 0, 0, 0, 0, -2.4142134189605713, 0, 0,
                  0.0010579226072877645, -0.0014660363085567951,
                  0.00041681528091430664, -1, 0, 0, 0.25010421872138977, 0]
        r = projection_candidates(matrix(values))
        self.assertEqual(len(r['hypotheses']), 1)
        self.assertTrue(r['hypotheses'][0]['reverse_z'])
        self.assertAlmostEqual(r['fov_y_radians'], math.pi/4, places=6)
        self.assertAlmostEqual(r['hypotheses'][0]['far'], 600.0361075361076)


class MatrixTests(unittest.TestCase):
    def test_nonfinite_or_overflow_cannot_report_zero_residual(self):
        for value in (math.nan, math.inf, -math.inf):
            a = identity()
            a[3][3] = value
            for call in (lambda: residual(a, identity()), lambda: inverse_pair(a, identity()),
                         lambda: inverse_pair(identity(), a), lambda: inverse(a)):
                with self.assertRaises(ValueError):
                    call()
        a, b = identity(), identity()
        a[3][3], b[3][3] = 1e308, -1e308
        with self.assertRaises(ValueError):
            residual(a, b)
        with self.assertRaises(ValueError):
            multiply(a, a)

    def test_nonidentity_clip_history(self):
        v = identity()
        v[3][:3] = [3, -2, 10]
        p = perspective()
        previous = multiply(v, p)
        current = perspective(0.2, 700, sign=-1, reverse=True)
        result = clip_to_previous(current, previous)
        self.assertLess(result['reconstruction']['max_abs'], 1e-12)
        self.assertGreater(residual(result['matrix'], identity())['max_abs'], 1)
        self.assertFalse(result['history_authenticated'])

    def test_inverse_pair_both_orders(self):
        a = identity()
        a[0][0], a[3][0] = -1, 42
        pair = inverse_pair(a, inverse(a))
        self.assertEqual(pair['a_times_b']['max_abs'], 0)
        self.assertEqual(pair['b_times_a']['max_abs'], 0)
        with self.assertRaises(ValueError):
            inverse([[0.0]*4 for _ in range(4)])


class GuideTests(unittest.TestCase):
    def test_depth_mapping_finite_distance_identity(self):
        for z in (0.0, 0.1, 0.9, 0.999, 1.0):
            distance = 1000/(1000-999*z)
            self.assertAlmostEqual(guide_depth(z), (distance-1)/999)
            self.assertAlmostEqual(guide_depth(1-z, True), guide_depth(z))
        self.assertEqual(guide_depth(math.nan, has_depth=False), 1)
        with self.assertRaises(ValueError):
            guide_depth(-0.1)

    def test_width_normalizes_both_screen_axes(self):
        b = motion_basis(0, 0, 200, 100, 2, 0.5, True)
        self.assertAlmostEqual(b[1], -0.99)
        self.assertAlmostEqual(b[2], -0.49)
        self.assertAlmostEqual(b[6], 0.02)
        self.assertEqual(motion_basis(0, 0, 200, 100, 2, 0, False)[6:], (0, 0, 0))
        self.assertEqual(motion_basis(0, 0, 200, 100, 2, 0, True)[6], 1)
        with self.assertRaises(ValueError):
            motion_basis(0, 0, 0, 100, 1, 0, False)

    def test_model_layout_and_robust_weight(self):
        b = motion_basis(15, 20, 200, 100, 2, 0.5, True)
        c = [0.0]*18
        c[0], c[3], c[9], c[17] = 2, 3, -5, 7
        x, y = evaluate_motion(c, b)
        self.assertAlmostEqual(x, 2+3*b[3])
        self.assertAlmostEqual(y, -5+7*b[8])
        self.assertEqual(robust_weight(0.5, 2, 2), 0.125)
        with self.assertRaises(ValueError):
            robust_weight(1, 1, 0)

    def test_signed_flow_and_nearest_even(self):
        self.assertEqual(unpack_flow(0x0020ffe0), (-1, 1))
        self.assertEqual(unpack_flow(pack_flow(1/64, 3/64)), (0, 2/32))
        self.assertEqual(unpack_flow(pack_flow(-1024, 1023.96875)), (-1024, 1023.96875))
        self.assertEqual(unpack_flow(pack_flow(-1500, 1500)), (-1024, 1023.96875))
        for x in (-1010.25, -1.234, 0, 17.532, 1000):
            decoded = unpack_flow(pack_flow(x, -x))
            self.assertLessEqual(abs(decoded[0]-x), 1/64)
        for x in (math.nan, math.inf, 2**31):
            with self.assertRaises(ValueError):
                pack_flow(x, 0)

    def test_flow_signed32_range_after_nearest_even(self):
        with self.assertRaises(ValueError):
            pack_flow((2**31-0.5)/32, 0)
        self.assertEqual(unpack_flow(pack_flow((2**31-1)/32, 0))[0], 1023.96875)
        self.assertEqual(unpack_flow(pack_flow(-(2**31)/32, 0))[0], -1024)


class CaptureTests(unittest.TestCase):
    def test_full_resource_preserved_and_range_not_sliced(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cb, meta = root/'capture.vs-cb2.bin', root/'capture.json'
            values = [0.0]*140
            values[20:36] = [v for r in perspective() for v in r]
            cb.write_bytes(struct.pack('<140f', *values))
            meta.write_text(json.dumps(dict(cbFile=cb.name, cbByteWidth=560,
                    firstConstant16ByteUnits=5, numConstants16ByteUnits=4)))
            result = analyze(cb, meta, [80], 'row-major', 'row', None, None, None, None)
            self.assertEqual(result['cb_bytes'], 560)
            self.assertEqual(result['metadata']['requested_bytes'], [80, 144])
            self.assertEqual(result['matrices'][0]['resource_offset'], 80)
            self.assertTrue(result['matrices'][0]['interpretations'][0]['projection']['accepted'])
            self.assertFalse(result['camera_authority'])
            with self.assertRaises(ValueError):
                analyze(cb, meta, [512], 'row-major', 'row', None, None, None, None)
            with self.assertRaises(ValueError):
                analyze(cb, meta, [81], 'row-major', 'row', None, None, None, None)
            with self.assertRaises(ValueError):
                analyze(cb, meta, [80], 'row-major', 'row', None, None, cb, None)
            meta.write_text(json.dumps(dict(cbFile=cb.name, cbByteWidth=64,
                    firstConstant16ByteUnits=0, numConstants16ByteUnits=4)))
            with self.assertRaises(ValueError):
                analyze(cb, meta, [0], 'row-major', 'row', None, None, None, None)

    def test_truncated_cb_rejected(self):
        for data in (b'', bytes(15), bytes(63), bytes(559)):
            with self.assertRaises(ValueError):
                float4_buffer(data)


if __name__ == '__main__':
    unittest.main(verbosity=2)
