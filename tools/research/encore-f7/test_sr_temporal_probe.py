"""Synthetic projection/affine diagnostics; no authenticated game frames."""
import hashlib
import json
import math
from pathlib import Path
import struct
import tempfile
import unittest

from encore_math import transpose
from sr_temporal_probe import analyze_cbs, analyze_values, affine_jitter_candidates, projection_shift

ROOT = Path(__file__).parent


def projection(sign=-1.0, ox=0.0, oy=0.0):
    return [[1.5, 0.0, 0.0, 0.0], [0.0, -2.4, 0.0, 0.0],
            [ox, oy, 0.001, sign], [0.0, 0.0, 0.25, 0.0]]


def raster(point, matrix, width, height):
    # Independent point projection, rather than copying the offset formula.
    clip = [math.fsum(point[i]*matrix[i][j] for i in range(4)) for j in range(4)]
    return [(clip[0]/clip[3]+1)*width/2, (1-clip[1]/clip[3])*height/2]


def candidates(report, axis=0):
    return {r['formula']: r for r in report['axes'][axis]['candidates']}


class TemporalProbeTests(unittest.TestCase):
    def test_encore_generic_sr_keys_are_static_evidence_not_runtime_receipt(self):
        report = json.loads((ROOT/'evidence/runtime/sr-generic-parameters.json').read_text())
        keys = {row['key']: bytes.fromhex(row['bytes']) for row in report['keys']}
        expected = ('Jitter.Offset.X', 'Jitter.Offset.Y', 'Sharpness', 'Reset',
                    'MV.Scale.X', 'MV.Scale.Y', 'DLSS.Pre.Exposure', 'DLSS.Exposure.Scale')
        self.assertEqual(set(keys), set(expected))
        self.assertTrue(all(keys[name] == name.encode('ascii')+bytes(1) for name in expected))
        self.assertEqual(struct.unpack('<d', bytes.fromhex(report['literals'][0]['bytes']))[0], 1)
        self.assertFalse(report['runtime_values_authenticated'])
        self.assertFalse(report['target_executed'])

    def test_shift_matches_independent_projection_of_multiple_points(self):
        for sign in (-1, 1):
            p, c = projection(sign, 0.1, -0.05), projection(sign, 0.102, -0.046)
            result = projection_shift(p, c, (1536, 960))
            self.assertTrue(result['offset_only_candidate'])
            for xyz in ((0, 0, sign), (1, 2, sign*3), (-3, 1, sign*10)):
                a, b = raster([*xyz, 1], p, 1536, 960), raster([*xyz, 1], c, 1536, 960)
                for axis in range(2):
                    self.assertAlmostEqual(b[axis]-a[axis], result['principal_delta_raster_pixels'][axis], delta=1e-11)

    def test_d3d_y_direction_is_independent_of_projection_focal_y_sign(self):
        p, c = projection(1), projection(1, 0.002, 0.004)
        for sy in (-2.4, 2.4):
            p[1][1] = c[1][1] = sy
            self.assertEqual(projection_shift(p, c, (1000, 500))['principal_delta_raster_pixels'], [1, -1])

    def test_homogeneous_sign_and_power_of_two_scale_do_not_change_geometry(self):
        p, c = projection(), projection(ox=0.001, oy=0.002)
        expected = projection_shift(p, c, (1000, 500))
        for scale in (-4, 2, 0.5):
            scaled = [[x*scale for x in row] for row in c]
            self.assertEqual(projection_shift(p, scaled, (1000, 500)), expected)

    def test_without_raster_no_pixel_values_are_invented(self):
        report = projection_shift(projection(), projection(ox=0.01))
        self.assertIsNone(report['caller_common_raster_size'])
        self.assertIsNone(report['principal_delta_raster_pixels'])

    def test_focal_change_refuses_uniform_shift_comparison(self):
        p, c = projection(), projection(ox=0.01)
        c[0][0] += 0.1
        report = analyze_values(previous_projection=p, current_projection=c, size=(1000, 500),
                                previous_raw=[0, 0], current_raw=[0.01, 0.01])
        self.assertFalse(report['projection']['offset_only_candidate'])
        self.assertIsNone(report['raw_candidates_compared_to_D3D_raster_delta'])

    def test_depth_change_also_refuses_same_projection_family(self):
        p, c = projection(), projection()
        c[2][2] += 0.0001
        self.assertFalse(projection_shift(p, c)['offset_only_candidate'])

    def test_explicit_tolerance_does_not_erase_recorded_residual(self):
        p, c = projection(), projection()
        c[0][0] += 1e-8
        report = projection_shift(p, c, tolerance=1e-7)
        self.assertTrue(report['offset_only_candidate'])
        self.assertGreater(report['nonoffset_max_abs'], 0)
        self.assertEqual(report['offset_cause'], 'UNKNOWN')

    def test_exact_ratios_detect_subnormal_quotient_collision_review_regression(self):
        w = 2.0**1023
        p = [[3*2.0**-52, 0, 0, 0], [0, w, 0, 0], [0, 0, w, w], [0, 0, -2.0**970, 0]]
        c = [row[:] for row in p]; c[0][0] = 4*2.0**-52
        report = analyze_values(previous_projection=p, current_projection=c, size=(640, 480),
                                previous_raw=[0, 0], current_raw=[0, 0])
        self.assertFalse(report['projection']['offset_only_candidate'])
        self.assertFalse(report['projection']['nonoffset_exactly_equal'])
        self.assertTrue(report['projection']['nonoffset_max_abs_underflowed'])
        self.assertIsNone(report['raw_candidates_compared_to_D3D_raster_delta'])
        point = [2.0**1020, 0, 2.0**-52, 1]
        self.assertEqual(raster(point, c, 640, 480)[0]-raster(point, p, 640, 480)[0], 40)

    def test_exact_ratios_detect_normal_quotient_collision(self):
        epsilon = 2.0**-52
        p = [[1+epsilon, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 1], [0, 0, -0.5, 0]]
        c = [[1+2*epsilon, 0, 0, 0], [0, 1+epsilon, 0, 0],
             [0, 0, 1+epsilon, 1+epsilon], [0, 0, -0.5*(1+epsilon), 0]]
        self.assertEqual(p[0][0]/p[2][3], c[0][0]/c[2][3])
        report = projection_shift(p, c)
        self.assertFalse(report['offset_only_candidate'])
        self.assertGreater(report['nonoffset_max_abs'], 0)

    def test_exact_center_difference_is_computed_before_rounding_quotients(self):
        p = [[2, 0, 0, 0], [0, 2, 0, 0], [2.0**52+1, 0, 1, 1], [0, 0, -0.5, 0]]
        c = [[x*1.5 for x in row] for row in p]
        self.assertEqual(p[2][0]/p[2][3], c[2][0]/c[2][3])
        report = projection_shift(p, c, (640, 480))
        self.assertTrue(report['offset_only_candidate'])
        self.assertEqual(report['principal_delta_ndc'][0], 1/3)
        self.assertEqual(report['principal_delta_raster_pixels'][0], 320/3)

    def test_tiny_forbidden_terms_are_not_tolerated_as_jitter(self):
        for i, j in ((0, 1), (3, 0), (3, 1), (0, 2), (1, 3), (3, 3)):
            c = projection()
            c[i][j] = 1e-20
            with self.assertRaises(ValueError):
                projection_shift(projection(), c, tolerance=1)
        c = projection(1e308)
        c[0][0] = c[1][1] = c[3][2] = 1e308
        c[3][0] = 1e-300
        with self.assertRaises(ValueError):
            projection_shift(projection(), c)
        c[3][0], c[2][0] = 0, 1e-300
        with self.assertRaises(ValueError):
            projection_shift(projection(), c)

    def test_degenerate_nonfinite_and_overflow_projection_rejected(self):
        for index, value in (((2, 3), 0), ((0, 0), 0), ((3, 2), 0),
                             ((2, 3), 1e-320), ((2, 0), math.nan), ((1, 1), math.inf)):
            c = projection(); c[index[0]][index[1]] = value
            with self.assertRaises(ValueError):
                projection_shift(projection(), c)
        p, c = projection(1, -1e308), projection(1, 1e308)
        with self.assertRaises(ValueError):
            projection_shift(p, c)

    def test_raster_and_tolerance_are_strict(self):
        for size in ((0, 1), (1, -1), (16385, 1), (True, 1), (1.0, 1), (1,)):
            with self.assertRaises(ValueError):
                projection_shift(projection(), projection(), size)
        for tolerance in (-1, math.nan, math.inf, True):
            with self.assertRaises(ValueError):
                projection_shift(projection(), projection(), tolerance=tolerance)

    def test_axiswise_candidate_scales_include_pixel_uv_and_ndc_forms(self):
        report = affine_jitter_candidates([0, 0], [0.001, 0.002], (1000, 500), [1, -1])
        x, y = candidates(report), candidates(report, 1)
        self.assertEqual(len(x), 7)
        self.assertEqual(x['raw*extent']['predicted_delta_pixels'], 1)
        self.assertEqual(y['-raw*extent']['predicted_delta_pixels'], -1)
        self.assertEqual(x['raw*extent/2']['predicted_delta_pixels'], 0.5)
        self.assertIsNone(report['preferred_formula'])

    def test_delta_only_cannot_distinguish_negative_uv_and_half_pixel_bias(self):
        report = affine_jitter_candidates([0.1, 0.2], [0.101, 0.202], (1000, 500), [-1, -1])
        x = candidates(report)
        self.assertEqual(x['-raw*extent']['predicted_delta_pixels'], x['0.5-raw*extent']['predicted_delta_pixels'])
        self.assertNotEqual(x['-raw*extent']['current_pixels'], x['0.5-raw*extent']['current_pixels'])
        self.assertFalse(report['constant_bias_identifiable_from_deltas'])
        self.assertFalse(report['absolute_comparison_available'])

    def test_supplied_sdk_absolute_values_can_compare_translator_bias(self):
        report = affine_jitter_candidates([0, 0.002], [0.001, 0], (1000, 500),
                                          observed_previous=[0.5, -0.5], observed_current=[-0.5, 0.5])
        for axis in (0, 1):
            rows = candidates(report, axis)
            self.assertEqual(rows['0.5-raw*extent']['absolute_residuals'], [0, 0])
            self.assertEqual(rows['-raw*extent']['absolute_residuals'], [-0.5, -0.5])
            self.assertEqual(rows['-raw*extent']['signed_residual'], rows['0.5-raw*extent']['signed_residual'])
        self.assertTrue(report['absolute_comparison_available'])
        self.assertIsNone(report['preferred_formula'])

    def test_conflicting_absolute_and_delta_observations_rejected(self):
        with self.assertRaises(ValueError):
            affine_jitter_candidates([0, 0], [1, 1], (100, 100), [0, 0],
                                      observed_previous=[0, 0], observed_current=[1, 1])

    def test_constant_raw_values_do_not_prove_a_formula(self):
        report = affine_jitter_candidates([0, 0], [0, 0], (1000, 500), [0, 0])
        self.assertTrue(all(r['signed_residual'] == 0 for r in candidates(report).values()))
        self.assertIsNone(report['preferred_formula'])
        self.assertFalse(report['apply_formula'])

    def test_affine_input_and_arithmetic_overflow_rejected(self):
        for previous, current in (([True, 0], [0, 0]), ([0, math.nan], [0, 0]),
                                  ([0, 0], [1e308, 0]), ([-1e308, 0], [1e308, 0])):
            with self.assertRaises(ValueError):
                affine_jitter_candidates(previous, current, (1536, 960))

    def test_missing_pairs_and_sizes_are_not_guessed(self):
        for kwargs in ({}, {'previous_raw': [0, 0]}, {'previous_projection': projection()},
                       {'previous_raw': [0, 0], 'current_raw': [1, 1]},
                       {'previous_raw': [0, 0], 'current_raw': [1, 1], 'size': (100, 100), 'previous_sdk': [0, 0]},
                       {'previous_sdk': [0, 0], 'current_sdk': [1, 1]}):
            with self.assertRaises(ValueError):
                analyze_values(**kwargs)

    def test_sdk_absolute_values_and_delta_preserved(self):
        report = analyze_values(previous_raw=[0, 0], current_raw=[0.1, 0.2], size=(100, 50),
                                previous_sdk=[1, 2], current_sdk=[11, 12])
        self.assertEqual(report['caller_sdk_jitter_previous'], [1, 2])
        self.assertEqual(report['caller_sdk_jitter_current'], [11, 12])
        self.assertEqual(report['caller_sdk_delta_pixels'], [10, 10])
        self.assertIsNotNone(report['raw_candidates_compared_to_caller_sdk_values'])
        self.assertIsNone(report['projection'])

    def test_raw_preview_does_not_claim_sdk_observed(self):
        report = analyze_values(previous_raw=[0, 0], current_raw=[0.1, 0.2], size=(100, 50))
        self.assertIsNotNone(report['raw_candidates'])
        self.assertIsNone(report['raw_candidates_compared_to_caller_sdk_values'])
        self.assertIsNone(report['caller_sdk_delta_pixels'])

    def test_no_numerical_result_grants_camera_source_history_or_runtime(self):
        report = analyze_values(previous_projection=projection(), current_projection=projection(ox=0.001),
                                previous_raw=[0, 0], current_raw=[0.1, 0.2], size=(100, 50))
        for key in ('camera_authority', 'source_authenticated', 'frame_authenticated', 'history_authenticated',
                    'target_executed', 'gpu_tested', 'apply_formula', 'fixed_bias_identifiable_from_deltas'):
            self.assertIs(report[key], False)
        self.assertEqual(report['SDK_sign_relative_to_raster'], 'UNKNOWN')
        self.assertIsNone(report['preferred_formula'])

    def test_entire_cb_absolute_offset_and_hash(self):
        with tempfile.TemporaryDirectory(prefix='sr-probe-test-', dir=ROOT/'evidence') as directory:
            paths = [Path(directory)/name for name in ('previous.bin', 'current.bin')]
            raw = []
            for path, matrix in zip(paths, (projection(), projection(ox=0.001))):
                data = bytes(80)+struct.pack('<16f', *(x for row in matrix for x in row))+bytes(32)
                path.write_bytes(data); raw.append(data)
            report = analyze_cbs(*paths, offset=80, storage='row-major', vectors='row')
            self.assertEqual(report['resource_offset'], 80)
            self.assertEqual([i['bytes'] for i in report['input_cbs']], [176, 176])
            self.assertEqual([i['sha256'] for i in report['input_cbs']], [hashlib.sha256(x).hexdigest() for x in raw])
            self.assertTrue(report['projection']['offset_only_candidate'])
            with self.assertRaises(ValueError):
                analyze_cbs(*paths, offset=16, storage='row-major', vectors='row')

    def test_declared_layout_conventions_are_required_not_inferred(self):
        with tempfile.TemporaryDirectory(prefix='sr-probe-layout-', dir=ROOT/'evidence') as directory:
            paths = [Path(directory)/name for name in ('previous.bin', 'current.bin')]
            for path, matrix in zip(paths, (projection(), projection(ox=0.001))):
                path.write_bytes(struct.pack('<16f', *(x for row in transpose(matrix) for x in row)))
            for storage, vectors in (('column-major', 'row'), ('row-major', 'column')):
                report = analyze_cbs(*paths, offset=0, storage=storage, vectors=vectors)
                self.assertTrue(report['projection']['offset_only_candidate'])
            with self.assertRaises(ValueError):
                analyze_cbs(*paths, offset=0, storage='row-major', vectors='row')


if __name__ == '__main__':
    unittest.main(verbosity=2)
