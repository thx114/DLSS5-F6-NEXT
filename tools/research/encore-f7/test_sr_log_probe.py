"""Synthetic tests for offline sparse F7 log parsing/print-model comparisons."""
from fractions import Fraction
import math
import json
from pathlib import Path
import random
import struct
import unittest
from unittest.mock import Mock

from sr_log_probe import (analyze, attach_session_receipt, compare_jitter, decimal_value,
                          parse_line, printed_interval)


def line(**changes):
    fields = dict(evaluate='1', result='1', history_slot='0', instance='100', context='200',
                  generation='300', token_frame='4', sdk='300', frame='5', owner='600',
                  queue='700', epoch='1', fence_target='1', render='1536x960', display='2560x1600',
                  token_jitter='0.000325521,0.000347222', token_jitter_encoding='0',
                  sdk_pixel_jitter='0.5,0.333333', sdk_mv_scale='1,1', requested_reset='0', sdk_reset='1',
                  creation_flags='3', depth_inverted='1', color_format='28', depth_format='41',
                  motion_format='16', frame_time_ms='16.67',
                  semantics='sdk-call-inputs; completion-and-quality-unproven')
    fields.update(changes)
    return '2026-11-06T22:59:00.000Z [INFO ] [dlss-input] '+' '.join(k+'='+v for k, v in fields.items())


def models(raw, sdk, extent=1536, **kwargs):
    return {r['formula']: r for r in compare_jitter(raw, sdk, extent, **kwargs)}


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


class SrLogProbeTests(unittest.TestCase):
    def test_schema_fields_and_original_line_preserved(self):
        row = parse_line(line(), 7)
        self.assertEqual(row['line'], 7)
        self.assertEqual(row['raw_line'], line())
        self.assertEqual(row['fields']['token_jitter'], ['0.000325521', '0.000347222'])
        self.assertEqual(row['fields']['render'], [1536, 960])
        self.assertEqual(row['fields']['sdk_reset'], 1)

    def test_unrelated_and_old_f6_logs_not_parsed(self):
        self.assertIsNone(parse_line('Genshin SR32 native provider: jitter=(0,0)', 1))
        self.assertIsNone(parse_line('2026-11-06T22:59:00.000Z [INFO ] [transport] complete', 1))
        with self.assertRaises(ValueError):
            analyze('old provider log only')

    def test_known_earlier_f7_formatter_lacks_encoding_without_guessing(self):
        text = line().replace(' token_jitter_encoding=0', '')
        row = parse_line(text, 1)
        self.assertFalse(row['token_jitter_encoding_recorded'])
        self.assertNotIn('token_jitter_encoding', row['fields'])
        self.assertEqual(analyze(text)['records'], 1)

    def test_duplicate_unknown_and_missing_fields_fail(self):
        for text in (line()+' evaluate=2', line(future='1'), line().replace(' sdk_reset=1', '')):
            with self.assertRaises(ValueError):
                analyze(text)

    def test_truncated_prefix_and_invalid_timestamp_fail(self):
        for text in (line()[10:], line().replace('2026-11-06', '2026-13-06'),
                     line().replace('[INFO ]', '[DEBUG]'), line()[:-1]):
            with self.assertRaises(ValueError):
                analyze(text)

    def test_nonfinite_and_pathological_numeric_tokens_fail(self):
        for value in ('nan', 'inf', '-inf', '1e999999', '0e-999999', '1e41', '1,2', '1.0evil'):
            with self.assertRaises(ValueError):
                decimal_value(value)
        with self.assertRaises(ValueError):
            printed_interval('3.5e38')

    def test_uint_range_and_bool_fields_are_strict(self):
        for changes in ({'frame': '-1'}, {'frame': str(2**64)}, {'result': str(2**32)},
                        {'sdk_reset': '2'}, {'requested_reset': '-1'}, {'depth_inverted': 'yes'}):
            with self.assertRaises(ValueError):
                analyze(line(**changes))

    def test_extent_pair_and_time_validation(self):
        for changes in ({'render': '0x960'}, {'render': '1536x16385'}, {'display': '2560'},
                        {'token_jitter': '0.1'}, {'sdk_pixel_jitter': 'nan,0'},
                        {'sdk_mv_scale': '1,2,3'}, {'frame_time_ms': '-1'}):
            with self.assertRaises(ValueError):
                analyze(line(**changes))

    def test_model_name_contract_and_failure_result_separated(self):
        report = analyze(line(result='3134193665'))
        self.assertEqual(len(report['observations'][0]['axes'][0]['models']), 7)
        self.assertFalse(report['observations'][0]['evaluate_result_is_success_code_1'])
        self.assertFalse(report['gpu_completion_proven'])

    def test_printf_zero_has_explicit_exact_zero_assumption(self):
        self.assertEqual(printed_interval('0'), (0, 0))
        self.assertEqual(printed_interval('-0'), (0, 0))
        self.assertTrue(analyze(line())['declared_print_model']['zero_means_exact_zero'])

    def test_full_quantum_overapproximation(self):
        self.assertEqual(printed_interval('0.123457'), (Fraction(123456, 10**6), Fraction(123458, 10**6)))
        self.assertEqual(printed_interval('1'), (Fraction(99999, 100000), Fraction(100001, 100000)))
        self.assertEqual(printed_interval('1e-5'), (Fraction(99999, 10**10), Fraction(100001, 10**10)))

    def test_negative_and_exponent_intervals(self):
        lo, hi = printed_interval('-1.23457e-3')
        self.assertLess(lo, Fraction(-123457, 10**8))
        self.assertGreater(hi, Fraction(-123457, 10**8))

    def test_raw_times_extent_printing_noise_does_not_false_reject(self):
        row = models('0.000325521', '0.5')['raw*extent']
        self.assertNotEqual(row['printed_center_residual_pixels'], 0)
        self.assertTrue(row['intervals_overlap_under_declared_print_model'])
        self.assertEqual(row['minimum_interval_gap_pixels'], 0)

    def test_distinguishes_same_row_half_pixel_bias(self):
        row = models('0.000325521', '0')
        self.assertTrue(row['0.5-raw*extent']['intervals_overlap_under_declared_print_model'])
        self.assertFalse(row['raw*extent']['intervals_overlap_under_declared_print_model'])
        self.assertFalse(row['-raw*extent']['intervals_overlap_under_declared_print_model'])

    def test_positive_only_sample_is_not_physical_contract(self):
        report = analyze(line())
        self.assertEqual(report['physical_jitter_semantics'], 'UNKNOWN')
        self.assertIsNone(report['preferred_formula'])
        self.assertFalse(report['apply_formula'])

    def test_small_subnormal_printed_values_are_accepted_by_intervals(self):
        raw = f32(2.0**-149)
        sdk = f32(raw*1536)
        row = models(format(raw, '.6g'), format(sdk, '.6g'))['raw*extent']
        self.assertTrue(row['intervals_overlap_under_declared_print_model'])

    def test_synthetic_float32_printed_transform_samples_remain_possible(self):
        rng = random.Random(20261107)
        for _ in range(300):
            raw, extent = f32(rng.uniform(-0.001, 0.001)), rng.randint(1, 16384)
            for formula, value in (('raw*extent', f32(raw*extent)),
                                   ('0.5-raw*extent', f32(0.5-raw*extent)),
                                   ('-raw*extent/2', f32(-raw*extent/2))):
                row = models(format(raw, '.6g'), format(value, '.6g'), extent)[formula]
                self.assertTrue(row['intervals_overlap_under_declared_print_model'], (raw, value, extent, row))

    def test_near_decimal_power_boundaries_not_false_rejected(self):
        for raw in (f32(0.9999995), f32(0.09999995), f32(-0.9999995), f32(0.00009999995)):
            for extent in (1, 1536, 16384):
                row = models(format(raw, '.6g'), format(f32(raw*extent), '.6g'), extent)['raw*extent']
                self.assertTrue(row['intervals_overlap_under_declared_print_model'])

    def test_precision_model_requires_explicit_valid_digits(self):
        for digits in (0, 10, True, 6.0):
            with self.assertRaises(ValueError):
                analyze(line(), significant_digits=digits)
        self.assertEqual(analyze(line(), significant_digits=4)['declared_print_model']['significant_digits'], 4)

    def test_same_model_can_have_multiple_possible_candidates(self):
        rows = models('0', '0', 1)
        self.assertGreater(sum(x['intervals_overlap_under_declared_print_model'] for x in rows.values()), 1)

    def test_reset_samples_are_descriptive_not_history_proof(self):
        report = analyze(line(sdk_reset='1')+chr(10)+line(evaluate='600', sdk_reset='0'))
        self.assertEqual(report['descriptive_scopes'][0]['sdk_reset_values'], [0, 1])
        self.assertFalse(report['quality_accepted'])
        self.assertFalse(report['gpu_completion_proven'])

    def test_each_context_epoch_or_extent_is_a_separate_descriptive_scope(self):
        report = analyze(chr(10).join((line(), line(epoch='2'), line(render='1920x1080'), line(context='201'))))
        self.assertEqual(len(report['descriptive_scopes']), 4)
        self.assertFalse(report['adjacent_frames_assumed'])

    def test_log_open_boundary_separates_reused_identity_and_reset_samples(self):
        opened = '2026-11-06T22:58:00.000Z [INFO ] [log] F7 rewrite log opened: synthetic.log'
        report = analyze(chr(10).join((opened, line(sdk_reset='1'), opened, line(sdk_reset='0'))))
        self.assertEqual([r['log_segment'] for r in report['observations']], [1, 2])
        self.assertEqual(len(report['descriptive_scopes']), 2)
        self.assertEqual([r['sdk_reset_values'] for r in report['descriptive_scopes']], [[1], [0]])
        self.assertEqual(len(report['log_open_boundaries']), 2)

    def test_missing_log_open_header_does_not_invent_observed_session(self):
        row = analyze(line())['observations'][0]
        self.assertEqual(row['log_segment'], 0)
        self.assertFalse(row['log_open_boundary_observed'])

    def test_malformed_log_open_header_cannot_hide_segment_boundary(self):
        bad = 'invalid [log] F7 rewrite log opened: synthetic.log'
        with self.assertRaises(ValueError):
            analyze(line()+chr(10)+bad+chr(10)+line())

    def test_printed_float32_max_rounding_above_binary_limit_is_supported(self):
        largest = struct.unpack('<f', bytes.fromhex('ffff7f7f'))[0]
        for precision in range(1, 10):
            for value in (largest, -largest):
                token = format(value, '.'+str(precision)+'g')
                lo, hi = printed_interval(token, precision)
                self.assertLessEqual(lo, Fraction(value))
                self.assertGreaterEqual(hi, Fraction(value))
                self.assertTrue(models(token, token, 1, significant_digits=precision)['raw']['intervals_overlap_under_declared_print_model'])

    def test_far_outside_finite_float32_interval_is_refused(self):
        for token in ('4e38', '-4e38'):
            for precision in (4, 6, 9):
                with self.assertRaises(ValueError):
                    printed_interval(token, precision)

    def test_motion_scale_both_axes_reject_disjoint_float32_print_intervals(self):
        for token in ('4e38', '-4e38'):
            for pair in (token+',1', '1,'+token):
                for precision in range(2, 10):
                    with self.subTest(pair=pair, precision=precision):
                        text = line(sdk_mv_scale=pair)
                        with self.assertRaisesRegex(ValueError, 'print interval does not intersect'):
                            parse_line(text, 1, significant_digits=precision)
                        with self.assertRaisesRegex(ValueError, 'print interval does not intersect'):
                            analyze(text, significant_digits=precision)

    def test_frame_time_rejects_disjoint_intervals_and_negative_centers(self):
        for precision in range(1, 10):
            with self.subTest(precision=precision):
                if precision >= 2:
                    with self.assertRaisesRegex(ValueError, 'print interval does not intersect'):
                        parse_line(line(frame_time_ms='4e38'), 1, significant_digits=precision)
                    with self.assertRaisesRegex(ValueError, 'print interval does not intersect'):
                        analyze(line(frame_time_ms='4e38'), significant_digits=precision)
                for token in ('-1', '-1e-45', '-3e38', '-4e38'):
                    with self.subTest(token=token):
                        with self.assertRaisesRegex(ValueError, 'negative frame time'):
                            parse_line(line(frame_time_ms=token), 1, significant_digits=precision)
                        with self.assertRaisesRegex(ValueError, 'negative frame time'):
                            analyze(line(frame_time_ms=token), significant_digits=precision)

    def test_non_jitter_float32_max_prints_preserve_tokens_at_all_precisions(self):
        largest = struct.unpack('<f', bytes.fromhex('ffff7f7f'))[0]
        for precision in range(1, 10):
            with self.subTest(precision=precision):
                positive = format(largest, '.'+str(precision)+'g')
                negative = format(-largest, '.'+str(precision)+'g')
                text = line(sdk_mv_scale=positive+','+negative, frame_time_ms=positive)
                row = parse_line(text, 1, significant_digits=precision)
                observed = analyze(text, significant_digits=precision)['observations'][0]
                for parsed in (row, observed):
                    self.assertEqual(parsed['fields']['sdk_mv_scale'], [positive, negative])
                    self.assertEqual(parsed['fields']['frame_time_ms'], positive)
                    self.assertEqual(parsed['raw_line'], text)

    def test_non_jitter_zero_and_subnormal_prints_remain_supported(self):
        smallest = f32(2.0**-149)
        for precision in range(1, 10):
            tiny = format(smallest, '.'+str(precision)+'g')
            for pair, time in (('0,-0', '0'), ('-0,+0', '-0'), (tiny+',-'+tiny, tiny)):
                with self.subTest(precision=precision, pair=pair, time=time):
                    text = line(sdk_mv_scale=pair, frame_time_ms=time)
                    row = parse_line(text, 1, significant_digits=precision)
                    observed = analyze(text, significant_digits=precision)['observations'][0]
                    for parsed in (row, observed):
                        self.assertEqual(parsed['fields']['sdk_mv_scale'], pair.split(','))
                        self.assertEqual(parsed['fields']['frame_time_ms'], time)

    def test_non_jitter_precision_reaches_parser_and_default_remains_six(self):
        # Each token is possible only under a sufficiently loose print model;
        # neither rejecting its center nor silently using p=6 is equivalent.
        for field, template in (('sdk_mv_scale', '{},1'), ('sdk_mv_scale', '1,{}'),
                                ('frame_time_ms', '{}')):
            with self.subTest(field=field, template=template):
                coarse = line(**{field: template.format('3.4029e38')})
                for operation in (lambda text, **kw: parse_line(text, 1, **kw), analyze):
                    self.assertIsNotNone(operation(coarse, significant_digits=4))
                    with self.assertRaisesRegex(ValueError, 'print interval does not intersect'):
                        operation(coarse)
                    default = line(**{field: template.format('3.402824e38')})
                    self.assertIsNotNone(operation(default))
                    with self.assertRaisesRegex(ValueError, 'print interval does not intersect'):
                        operation(default, significant_digits=9)
                    # Full-quantum p=1 deliberately overlaps even for 4e38.
                    self.assertIsNotNone(operation(line(**{field: template.format('4e38')}),
                                                   significant_digits=1))

    def test_sidecar_claims_are_reported_with_hash_not_authenticated(self):
        receipt = dict(schema='f7.genshin-candidate-session.v2', pid=123, creationFileTime=456,
                       actualModule=[dict(path='D:/synthetic/OptiScaler.dll', diskSha256='a'*64,
                                          mappedFullShaKnown=False)], runtimeInputsStable=True,
                       runtimeInputsAfter={'D:/synthetic/OptiScaler.ini': 'b'*64}, cameraAuthority=True)
        path = Mock(); path.read_bytes.return_value = json.dumps(receipt).encode()
        path.resolve.return_value = Path('D:/synthetic/result.json')
        report = analyze(line())
        attach_session_receipt(report, path)
        sidecar = report['caller_supplied_session_receipt']
        self.assertEqual(sidecar['reported_pid'], 123)
        self.assertEqual(sidecar['reported_creation_filetime'], 456)
        self.assertEqual(len(sidecar['sha256']), 64)
        self.assertFalse(sidecar['process_or_loaded_memory_identity_authenticated'])
        self.assertFalse(report['provenance_authenticated'])
        self.assertFalse(report['gpu_completion_proven'])

    def test_bad_sidecars_and_duplicate_keys_are_rejected(self):
        base = dict(schema='f7.genshin-candidate-session.v2', pid=123, creationFileTime=456,
                    actualModule=[], runtimeInputsAfter={})
        cases = [json.dumps(base | change).encode() for change in (
            {'schema': 'old-F6'}, {'pid': True}, {'creationFileTime': 0}, {'actualModule': None},
            {'runtimeInputsAfter': []}, {'runtimeInputsAfter': {'OptiScaler.ini': 'invalid'}})]
        cases.append(b'{"schema":1,"schema":2}')
        for raw in cases:
            path = Mock(); path.read_bytes.return_value = raw
            path.resolve.return_value = Path('D:/synthetic/result.json')
            with self.assertRaises(ValueError):
                attach_session_receipt(analyze(line()), path)

    def test_sparse_or_reordered_rows_are_not_reinterpreted_as_adjacency(self):
        report = analyze(chr(10).join((line(evaluate='600', token_frame='2000'),
                                       line(evaluate='2', token_frame='3'))))
        self.assertEqual([r['fields']['evaluate'] for r in report['observations']], [600, 2])
        self.assertFalse(report['adjacent_frames_assumed'])

    def test_model_count_is_per_axis_per_row_not_per_frame(self):
        report = analyze(line()+chr(10)+line(evaluate='600'))
        count = report['model_counts']['x']['raw*extent']
        self.assertEqual(count['overlap']+count['disjoint'], 2)
        self.assertEqual(report['records'], 2)

    def test_invalid_row_cannot_be_hidden_behind_valid_rows(self):
        with self.assertRaises(ValueError):
            analyze(line()+chr(10)+line(sdk_pixel_jitter='not-a-number,0'))

    def test_semantics_exactness_not_unknown_upgrade(self):
        with self.assertRaises(ValueError):
            analyze(line(semantics='sdk-call-inputs; GPU-complete'))

    def test_large_finite_decimal_intermediate_avoids_float_overflow(self):
        rows = compare_jitter('3.40282e38', '3.40282e38', 16384)
        self.assertTrue(all(math.isfinite(x['printed_center_residual_pixels']) for x in rows))
        self.assertFalse(models('3.40282e38', '3.40282e38', 16384)['raw*extent']['intervals_overlap_under_declared_print_model'])

    def test_pair_spaces_and_scientific_notation_preserved(self):
        report = analyze(line(token_jitter='3.25521e-4, 3.47222e-4', sdk_pixel_jitter='5e-1, 3.33333e-1'))
        self.assertEqual(report['observations'][0]['fields']['token_jitter'][0], '3.25521e-4')


if __name__ == '__main__':
    unittest.main(verbosity=2)
