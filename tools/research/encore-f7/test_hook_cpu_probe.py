"""Dedicated CPU-only regressions. All filesystem mutations are simulated in memory.

Run python -B test_hook_cpu_probe.py. No temp files, bytecode, native code, DLL
loading, GPU/game/deployment activity or network calls are needed by these tests.
"""
from __future__ import annotations

from contextlib import ExitStack, redirect_stderr, redirect_stdout
import hashlib
import io
import json
import os
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import hook_cpu_probe as probe


def header(**changes) -> str:
    fields = {key: kind.partition(':')[2] for key, kind in probe.HEADER_TYPES.items()
              if kind.startswith('literal:')}
    fields.update(pid=123, process_creation_filetime=134116000000000001, creation_valid=1,
                  qpc_hz=10000000, report_qpc=9007199254741999, present_index=7,
                  dropped_samples=0, dropped_counters=0, invalid_stage=0, invalid_counter=0,
                  overflow=0, **{key: 0 for key in probe.COUNTERS})
    fields.update(changes)
    return '[hook-cpu] '+probe.PREAMBLE+' '+' '.join(f'{key}={fields[key]}' for key in probe.HEADER_TYPES)


def stage(name='TokenWork', **changes) -> str:
    fields = dict(stage=name, samples=2, invalid_clock=0, sum_ms='2', mean_ms='1',
                  p95_valid=1, p95_lower_inclusive=0, p95_lower_ms='0.5', p95_upper_ms='1',
                  p99_valid=1, p99_lower_inclusive=0, p99_lower_ms='0.5', p99_upper_ms='1',
                  max_ms='1', max_begin_qpc=9007199254740993, max_end_qpc=9007199254750993,
                  max_thread=42, over33_ms=0, over50_ms=0, over100_ms=0, overflow=0)
    fields.update(changes)
    return '[hook-cpu-stage] '+' '.join(f'{key}={fields[key]}' for key in probe.STAGE_TYPES)


def zero_stage(name='TokenWork', **changes) -> str:
    fields = dict(samples=0, sum_ms=0, mean_ms=-1, max_ms=0, max_begin_qpc=0,
                  max_end_qpc=0, max_thread=0,
                  p95_valid=0, p95_lower_inclusive=0, p95_lower_ms=0, p95_upper_ms=0,
                  p99_valid=0, p99_lower_inclusive=0, p99_lower_ms=0, p99_upper_ms=0)
    fields.update(changes)
    return stage(name, **fields)


def capture(count=11, **changes) -> str:
    return '\n'.join([header(**changes), *(stage(name) for name in probe.STAGES[:count])])


def bin_edges(ticks, hz=10000000):
    index = next((i for i in range(1, 257) if ticks <= hz*i//2000), 257)
    low = hz*(index-1)//2000
    high = ticks if index == 257 else hz*index//2000
    return f'{low*1000/hz:.6g}', f'{high*1000/hz:.6g}'


def single_stage(text: str, **changes) -> dict:
    return probe.analyze_text(header(**changes)+'\n'+text)['snapshots'][0]['stages'][0]


# Protocol fixtures deliberately spell the new writer literals and policy,
# independently of the production schema/stride maps. These are synthetic logs.
SAMPLED_MARKERS = ('sampled_stages_do_not_extrapolate=true',
                   'observed_max_only=true', 'periodic_aliasing=unknown')
EXPECTED_STRIDES = {
    'TokenWork': 1, 'NativeRender': 1, 'DrawSelect': 128, 'DrawBindings': 1,
    'CompositionPrepare': 128, 'NativeDraw': 128, 'CompositionCommit': 128, 'SrDispatch': 1,
    'SurfaceLockWait': 128, 'CompositionLockWait': 128, 'ShaderGetter': 128,
}


def sampled_header(**changes) -> str:
    return header(**changes).replace('binding_cache=not-implemented', 'binding_cache=separately-reported') \
        .replace(probe.PREAMBLE, probe.PREAMBLE+' '+' '.join(SAMPLED_MARKERS), 1)


def sampled_stage(name='TokenWork', **changes) -> str:
    stride = changes.pop('sampling_stride', EXPECTED_STRIDES[name])
    return stage(name, **changes).replace('stage='+name, f'stage={name} sampling_stride={stride}', 1)


def sampled_capture(count=11, **changes) -> str:
    return '\n'.join([sampled_header(**changes), *(sampled_stage(name) for name in probe.STAGES[:count])])


class ParseTests(unittest.TestCase):
    def test_both_complete_groups_and_last_complete_selection(self):
        report = probe.analyze_text(capture(8)+'\n'+capture(11, present_index=8)+'\n'+capture(3, present_index=9))
        self.assertEqual([s['stage_set_complete'] for s in report['snapshots']], [True, True, False])
        self.assertEqual(report['last_complete_snapshot_index'], 1)
        self.assertTrue(report['snapshots'][0]['legacy_or_truncated_current_ambiguous'])
        self.assertFalse(report['semantics']['complete_distribution_proven'])

    def test_every_prefix_at_eof_and_next_header(self):
        for count in range(12):
            for ending in ('EOF', 'next-header'):
                with self.subTest(count=count, ending=ending):
                    text = capture(count)
                    if ending == 'next-header':
                        text += '\n'+capture(1, present_index=8)
                    snapshot = probe.analyze_text(text)['snapshots'][0]
                    self.assertEqual(snapshot['stage_count'], count)
                    self.assertEqual(snapshot['stage_set_complete'], count in (8, 11))
                    self.assertEqual(snapshot['terminated_by'], ending)

    def test_eight_plus_one_is_incomplete_not_legacy_complete(self):
        report = probe.analyze_text(capture(9))
        self.assertIsNone(report['last_complete_snapshot_index'])
        self.assertEqual(report['snapshots'][0]['format'], 'incomplete-prefix')

    def test_real_logger_prefix_only_on_header(self):
        value = probe.analyze_text('2026-11-07T01:02:03.004Z [INFO ] '+capture())
        self.assertEqual(value['last_complete_snapshot_index'], 0)
        for prefix in ('[INFO ] ', '2026-11-07T01:02:03.004Z [INFO] '):
            self.assertEqual(probe.analyze_text(prefix+capture())['last_complete_snapshot_index'], 0)

    def test_exact_markers_and_orphans(self):
        cases = [stage(), header()+'\n[INFO ] '+stage(), header()+'\nX'+stage(),
                 header()+'\n'+stage().replace('[hook-cpu-stage]', '[hook-cpu-stage-extra]'),
                 capture().replace('[hook-cpu]', 'forged [hook-cpu]', 1),
                 header()+' [hook-cpu] '+header(), header()+'\n[hook-cpu-extra] x=1']
        for text in cases:
            with self.subTest(text=text[:90]), self.assertRaises(ValueError):
                probe.analyze_text(text)

    def test_unknown_missing_duplicate_fields_and_stages(self):
        cases = [header()+' pid=123', header().replace(' pid=123', ''),
                 header()+'\n'+stage()+' samples=2',
                 header()+'\n'+stage().replace(' samples=2', ''),
                 header()+'\n'+stage('FutureStage'), header()+'\n'+stage()+'\n'+stage(),
                 header()+'\n'+stage('NativeRender'), header()+' future=1 future=2']
        for text in cases:
            with self.subTest(text=text[:100]), self.assertRaises(ValueError):
                probe.analyze_text(text)

    def test_extension_fields_remain_raw_unknown_without_certification(self):
        text = header()+' future_policy=unknown future_max_note=observed'
        text += '\n'+stage()+' future_stride=64'
        snapshot = probe.analyze_text(text)['snapshots'][0]
        self.assertTrue(snapshot['contains_unknown_fields'])
        self.assertFalse(snapshot['field_contract_certified'])
        self.assertEqual(snapshot['stages'][0]['unknown_fields']['future_stride'],
                         {'raw': '64', 'status': 'unknown', 'type_and_semantics_certified': False})
        self.assertTrue(snapshot['stages'][0]['accepted_max_call_usable'])

    def test_strict_types_and_ranges(self):
        for fields in ({'pid': '1.0'}, {'pid': -1}, {'pid': 2**32}, {'qpc_hz': 0},
                       {'qpc_hz': 2**63}, {'process_creation_filetime': 2**64},
                       {'creation_valid': 2}, {'creation_valid': 0}, {'report_qpc': -2}):
            with self.subTest(fields=fields), self.assertRaises(ValueError):
                probe.analyze_text(header(**fields))
        for fields in ({'samples': '2.0'}, {'samples': 2**64}, {'invalid_clock': -1},
                       {'max_end_qpc': 2**63}, {'p95_valid': 'true'}, {'max_thread': 2**32},
                       {'sum_ms': 'NaN'}, {'mean_ms': 'inf'}, {'max_ms': '1e999999'},
                       {'overflow': 256}, {'sum_ms': -1}):
            with self.subTest(fields=fields), self.assertRaises(ValueError):
                single_stage(stage(**fields))

    def test_repeated_identities_are_preserved_and_hz_changes_never_join(self):
        text = '\n'.join([capture(), capture(), capture(process_creation_filetime=134116000000000002),
                          capture(0, qpc_hz=3), capture(pid=456)])
        report = probe.analyze_text(text)
        self.assertEqual(len(report['snapshots']), 5)
        self.assertTrue(report['snapshots'][1]['repeated_report_identity'])
        self.assertEqual(report['snapshots'][2]['previous_identity_relation'], 'different-process-or-hz')
        self.assertEqual(report['snapshots'][3]['identity']['qpc_hz'], '3')
        self.assertEqual(len(report['last_complete_by_identity']), 3)
        self.assertFalse(report['semantics']['identities_or_frequencies_merged'])

    def test_unavailable_process_creation_is_not_lifetime_identity(self):
        text = capture(process_creation_filetime=0, creation_valid=0)
        report = probe.analyze_text(text+'\n'+text)
        self.assertFalse(report['snapshots'][0]['identity_complete'])
        self.assertEqual(len(report['last_complete_by_identity']), 2)

    def test_exact_int64_endpoints_and_rational_ms_survive_json(self):
        begin = 2**63-100
        value = single_stage(stage(samples=1, sum_ms='0.0003', mean_ms='0.0003', max_ms='0.0003',
                                   max_begin_qpc=begin, max_end_qpc=begin+3,
                                   p95_lower_ms=0, p95_upper_ms='0.5',
                                   p99_lower_ms=0, p99_upper_ms='0.5'))
        decoded = json.loads(json.dumps(value, allow_nan=False))
        self.assertEqual(decoded['max_call']['duration_ticks'], '3')
        self.assertEqual(decoded['max_call']['begin_qpc'], str(begin))
        self.assertEqual(decoded['max_call']['duration_ms_exact'], {'numerator': '3', 'denominator': '10000'})
        high = probe.analyze_text(capture(process_creation_filetime=2**64-1, present_index=2**64-1))
        self.assertEqual(high['snapshots'][0]['header']['present_index'], str(2**64-1))
        self.assertEqual(high['snapshots'][0]['header']['pid'], '123')

    def test_non_decimal_qpc_frequency_and_writer_rounding(self):
        value = single_stage(stage(samples=1, sum_ms='333.333', mean_ms='333.333', max_ms='333.333',
                                   max_begin_qpc=0, max_end_qpc=1,
                                   p95_lower_ms=0, p95_upper_ms='333.333',
                                   p99_lower_ms=0, p99_upper_ms='333.333',
                                   over33_ms=1, over50_ms=1, over100_ms=1), qpc_hz=3)
        self.assertEqual(value['max_call']['duration_ms_exact'], {'numerator': '1000', 'denominator': '3'})
        with self.assertRaisesRegex(ValueError, 'max_ms'):
            single_stage(stage(max_ms='1.00001'))

    def test_zero_samples_do_not_invent_max_call(self):
        value = single_stage(zero_stage(invalid_clock=4))
        self.assertIsNone(value['max_call'])
        self.assertEqual(value['loss_reasons'], ['invalid_clock'])
        for change in ({'max_begin_qpc': 1, 'max_end_qpc': 1}, {'sum_ms': 1},
                       {'mean_ms': 0}, {'p99_valid': 1}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                single_stage(zero_stage(**change))

    def test_zero_duration_accepted_call_is_distinct_from_zero_samples(self):
        text = zero_stage(samples=1, mean_ms=0, max_thread=42, p95_valid=1, p99_valid=1,
                          p95_lower_inclusive=1, p99_lower_inclusive=1)
        self.assertEqual(single_stage(text)['max_call']['duration_ticks'], '0')

    def test_threshold_order_samples_and_strict_edges(self):
        begin = 9007199254740993
        for ticks, counts in ((330000, (0, 0, 0)), (330001, (1, 0, 0)),
                              (500000, (1, 0, 0)), (500001, (1, 1, 0)),
                              (1000000, (1, 1, 0)), (1000001, (1, 1, 1))):
            ms = f'{ticks/10000:.6g}'
            lower, upper = bin_edges(ticks)
            text = stage(samples=1, sum_ms=ms, mean_ms=ms, max_ms=ms,
                         max_end_qpc=begin+ticks, p95_lower_ms=lower, p95_upper_ms=upper,
                         p99_lower_ms=lower, p99_upper_ms=upper,
                         over33_ms=counts[0], over50_ms=counts[1], over100_ms=counts[2])
            with self.subTest(ticks=ticks):
                self.assertIsNotNone(single_stage(text)['max_call'])
        for change in ({'over33_ms': 3}, {'over50_ms': 1}, {'over100_ms': 1}, {'over33_ms': 1}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                single_stage(stage(**change))

    def test_loss_preserves_legal_max_and_coverage_unknown(self):
        for field in probe.LOSS_HEADER:
            report = probe.analyze_text(header(**{field: 1})+'\n'+stage(invalid_clock=3))
            snapshot = report['snapshots'][0]
            self.assertEqual(snapshot['coverage'], 'unknown')
            self.assertIn(field, snapshot['coverage_reasons'])
            self.assertTrue(snapshot['stages'][0]['accepted_max_call_usable'])
        value = single_stage(stage(overflow=2, mean_ms=-1))
        self.assertTrue(value['percentile_intervals']['p95']['valid'])
        self.assertEqual(value['max_call']['duration_ticks'], '10000')
        # Count/bin overflow invalidates quantiles, but does not erase recorded maximum endpoints.
        for flag in (1, 4):
            value = single_stage(stage(overflow=flag, mean_ms=-1 if flag == 1 else 1,
                p95_valid=0, p95_lower_ms=0, p95_upper_ms=0,
                p99_valid=0, p99_lower_ms=0, p99_upper_ms=0))
            self.assertIsNotNone(value['max_call'])

    def test_quantiles_are_intervals_and_invalid_bounds_rejected(self):
        value = single_stage(stage())['percentile_intervals']['p95']
        self.assertFalse(value['exact_percentile'])
        self.assertFalse(value['lower_inclusive'])
        self.assertTrue(value['upper_inclusive'])
        for change in ({'p95_lower_ms': -1}, {'p95_upper_ms': '0.4'},
                       {'p95_lower_inclusive': 1}, {'p95_valid': 0},
                       {'p95_lower_ms': 1, 'p95_upper_ms': '1.5'},
                       {'p95_upper_ms': '1e300', 'p99_upper_ms': '1e300'},
                       {'p99_lower_ms': '0', 'p99_upper_ms': '0.5'}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                single_stage(stage(**change))

    def test_unrelated_line_cannot_bridge_incomplete_group(self):
        text = header()+'\n'+stage()+'\nunrelated message\n'+stage('NativeRender')
        with self.assertRaisesRegex(ValueError, 'without header'):
            probe.analyze_text(text)
        report = probe.analyze_text(capture()+'\nunrelated message\n')
        self.assertEqual(report['snapshots'][0]['terminated_by'], 'unrelated-log-line')

    def test_empty_malformed_and_mid_field_truncation(self):
        for text in ('', 'normal log line', capture()[:-3]):
            with self.subTest(text=text[-80:]), self.assertRaises(ValueError):
                probe.analyze_text(text)


class QuantileRankTests(unittest.TestCase):
    @staticmethod
    def record(count, *, maximum=10000, p95_ticks=10000, p99_ticks=10000,
               hz=10000000, sum_ticks=None, **changes):
        """Fixture edges from explicit chosen values; no histogram or population expansion."""
        total = count*maximum if sum_ticks is None else sum_ticks
        printed = lambda ticks: f'{ticks*1000/hz:.6g}'
        fields = dict(samples=count, sum_ms=printed(total), mean_ms=f'{total*1000/hz/count:.6g}',
                      max_ms=printed(maximum), max_end_qpc=9007199254740993+maximum)
        for p, value in ((95, p95_ticks), (99, p99_ticks)):
            low, high = ('0', '0') if value == 0 else bin_edges(value, hz)
            fields.update({f'p{p}_lower_ms': low, f'p{p}_upper_ms': high,
                           f'p{p}_lower_inclusive': int(value == 0)})
        fields.update(changes)
        return stage(**fields)

    def test_reported_single_sample_counterexample_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'nearest-rank/max/threshold'):
            single_stage(stage(samples=1, sum_ms='1', p95_lower_ms='0', p95_upper_ms='0.5',
                               p99_lower_ms='0', p99_upper_ms='0.5'))

    def test_rank_at_count_requires_max_for_both_percentiles(self):
        for p, counts in ((95, (1, 2, 18, 19)), (99, (1, 2, 98, 99))):
            for count in counts:
                with self.subTest(percentile=p, count=count):
                    value = single_stage(self.record(count))
                    self.assertEqual(value['percentile_intervals'][f'p{p}']['nearest_rank'], str(count))
                    changes = {f'p{p}_lower_ms': '0', f'p{p}_upper_ms': '0.5'}
                    with self.assertRaisesRegex(ValueError, f'p{p} interval'):
                        single_stage(self.record(count, **changes))
                    # The zero bin must also include the max when the rank is the count.
                    changes[f'p{p}_lower_inclusive'] = 1
                    changes[f'p{p}_upper_ms'] = '0'
                    with self.assertRaisesRegex(ValueError, f'p{p} interval'):
                        single_stage(self.record(count, **changes))

    def test_first_rank_below_count_can_use_an_earlier_bin(self):
        for count, p99_ticks in ((20, 10000), (100, 2500)):
            with self.subTest(count=count):
                value = single_stage(self.record(count, p95_ticks=2500, p99_ticks=p99_ticks,
                                                sum_ticks=(count-1)*2500+10000))
                self.assertEqual(value['percentile_intervals']['p95']['nearest_rank'], str(count*95//100))
                self.assertEqual(value['percentile_intervals']['p99']['nearest_rank'], str(count-1 if count == 100 else count))

    def test_uint64_count_rank_and_overflow_gates(self):
        count = 2**64-1
        value = single_stage(self.record(count, p95_ticks=2500, p99_ticks=7500,
                                        overflow=2, sum_ms='1.84467e15', mean_ms=-1))
        self.assertEqual(value['percentile_intervals']['p95']['nearest_rank'], '17524406870024074035')
        self.assertEqual(value['percentile_intervals']['p99']['nearest_rank'], '18262276632972456099')
        # Sum overflow does not remove the max constraint at rank n.
        with self.assertRaisesRegex(ValueError, 'p95 interval'):
            single_stage(self.record(19, p95_ticks=2500, overflow=2, mean_ms=-1))
        for flag in (1, 4, 5):
            with self.subTest(overflow=flag):
                value = single_stage(zero_stage(samples=1, sum_ms=1,
                    mean_ms=-1 if flag & 1 else 1, max_ms=1,
                    max_begin_qpc=9007199254740993, max_end_qpc=9007199254750993,
                    max_thread=42, overflow=flag))
                self.assertIsNone(value['percentile_intervals']['p95']['nearest_rank'])
                self.assertIsNotNone(value['max_call'])

    def test_integer_max_bin_and_print_rounding_at_high_hz(self):
        hz = 2**63-1
        edge = hz*256//2000  # Exactly the last finite bin's inclusive upper tick.
        for maximum in (edge, edge+1):
            with self.subTest(maximum=maximum):
                record = self.record(1, maximum=maximum, p95_ticks=maximum, p99_ticks=maximum,
                                     hz=hz, over33_ms=1, over50_ms=1, over100_ms=1)
                value = single_stage(record, qpc_hz=hz)
                self.assertEqual(value['max_call']['duration_ticks'], str(maximum))
                self.assertEqual(value['fields']['max_ms'], '128')
        # Printed max is still 128, but one tick above the edge cannot use bin 256.
        with self.assertRaisesRegex(ValueError, 'p95 interval'):
            single_stage(self.record(1, maximum=edge+1, p95_ticks=edge, p99_ticks=edge+1,
                                    hz=hz, over33_ms=1, over50_ms=1, over100_ms=1), qpc_hz=hz)

    def test_known_threshold_rank_cuts_reject_both_sides(self):
        for threshold in (33, 50, 100):
            for p, above in ((95, 5), (99, 1)):
                edge = threshold*10000
                maximum = 1010000
                counts = {f'over{t}_ms': 100 if t < threshold else above for t in (33, 50, 100)}
                total = (100-above)*edge+above*maximum
                # rank == count-above: the order statistic can be exactly the threshold tick.
                q95 = edge
                q99 = edge if p == 99 else maximum
                with self.subTest(threshold=threshold, percentile=p):
                    single_stage(self.record(100, maximum=maximum, p95_ticks=q95, p99_ticks=q99,
                                             sum_ticks=total, **counts))
                    with self.assertRaisesRegex(ValueError, f'p{p} interval'):
                        single_stage(self.record(100, maximum=maximum, p95_ticks=q95, p99_ticks=q99,
                            sum_ticks=total, **counts, **{f'p{p}_lower_ms': str(threshold),
                                                        f'p{p}_upper_ms': str(threshold+0.5)}))
                    # One more value above the threshold puts that same rank strictly above it.
                    counts = {f'over{t}_ms': 100 if t < threshold else above+1 for t in (33, 50, 100)}
                    total = (99-above)*edge+(above+1)*maximum
                    q95 = maximum if p == 95 else edge
                    single_stage(self.record(100, maximum=maximum, p95_ticks=q95, p99_ticks=maximum,
                                             sum_ticks=total, **counts))
                    with self.assertRaisesRegex(ValueError, f'p{p} interval'):
                        single_stage(self.record(100, maximum=maximum, p95_ticks=q95, p99_ticks=maximum,
                            sum_ticks=total, **counts, **{f'p{p}_lower_ms': str(threshold-0.5),
                                                        f'p{p}_upper_ms': str(threshold)}))

    def test_threshold_counterexample_and_threshold_overflow(self):
        text = self.record(100, maximum=1010000, p95_ticks=2500, p99_ticks=2500,
                           sum_ticks=99990000, over33_ms=99, over50_ms=99, over100_ms=99)
        with self.assertRaisesRegex(ValueError, 'p95 interval'):
            single_stage(text)
        # Threshold overflow removes only those counter-derived cuts, not max/rank constraints.
        value = single_stage(text.replace('overflow=0', 'overflow=8'))
        self.assertTrue(value['percentile_intervals']['p95']['valid'])
        self.assertIn('overflow', value['loss_reasons'])
        with self.assertRaisesRegex(ValueError, 'p95 interval'):
            single_stage(self.record(1, p95_ticks=2500, overflow=8))

    def test_low_hz_threshold_ticks_and_zero_bin(self):
        for hz, edge, maximum in ((3, 0, 1), (1001, 33, 34)):
            counts = {f'over{t}_ms': (5 if edge <= hz*t//1000 else 100)
                      if maximum > hz*t//1000 else 0 for t in (33, 50, 100)}
            with self.subTest(hz=hz):
                value = single_stage(self.record(100, maximum=maximum, p95_ticks=edge, p99_ticks=maximum,
                                                hz=hz, sum_ticks=95*edge+5*maximum, **counts), qpc_hz=hz)
                self.assertEqual(value['percentile_intervals']['p95']['lower_inclusive'], edge == 0)
                with self.assertRaisesRegex(ValueError, 'p95 interval'):
                    single_stage(self.record(100, maximum=maximum, p95_ticks=maximum, p99_ticks=maximum,
                                             hz=hz, sum_ticks=95*edge+5*maximum, **counts), qpc_hz=hz)

    def test_p95_p99_order_uses_candidate_ticks(self):
        with self.assertRaisesRegex(ValueError, 'p95/p99.*ordering'):
            single_stage(self.record(1000, p95_ticks=7500, p99_ticks=2500,
                                     sum_ticks=5000000))
        value = single_stage(self.record(1000, p95_ticks=7500, p99_ticks=7500, sum_ticks=7502500))
        self.assertEqual(value['percentile_intervals']['p95']['lower_ms_printed'], '0.5')


class SampledProtocolTests(unittest.TestCase):
    def test_full_sampled_writer_known_profile_and_declared_stride(self):
        report = probe.analyze_text('2026-11-07T05:06:07.008Z [INFO ] '+sampled_capture())
        snapshot = report['snapshots'][0]
        self.assertEqual(snapshot['profile'], 'sampled')
        self.assertEqual(snapshot['format'], 'sampled-11')
        self.assertEqual(report['runtime_compiled_source_identity'], 'unknown')
        self.assertEqual(snapshot['runtime_compiled_source_identity'], 'unknown')
        self.assertFalse(snapshot['source_contract_certified'])
        self.assertEqual(snapshot['stage_count'], 11)
        self.assertTrue(snapshot['stage_set_complete'])
        self.assertFalse(snapshot['contains_unknown_fields'])
        self.assertEqual(snapshot['header']['binding_cache'], 'separately-reported')
        for entry in snapshot['stages']:
            name = entry['fields']['stage']
            stride = EXPECTED_STRIDES[name]
            with self.subTest(stage=name):
                self.assertEqual(entry['fields']['sampling_stride'], stride)
                self.assertEqual(entry['sampling']['declared_stride'], stride)
                self.assertEqual(entry['sampling']['periodically_sampled'], stride == 128)
                self.assertEqual(entry['statistics_scope'], 'accepted selected scopes only')
                self.assertTrue(entry['observed_max_only'])

    def test_sampled_every_prefix_at_eof_or_next_header_requires_eleven(self):
        for count in range(12):
            for ending in ('EOF', 'next-header'):
                with self.subTest(count=count, ending=ending):
                    text = sampled_capture(count)
                    if ending == 'next-header':
                        text += '\n'+sampled_capture(0, present_index=8)
                    report = probe.analyze_text(text)
                    snapshot = report['snapshots'][0]
                    self.assertEqual(snapshot['stage_count'], count)
                    self.assertEqual(snapshot['stage_set_complete'], count == 11)
                    self.assertEqual(snapshot['terminated_by'], ending)
                    self.assertFalse(snapshot['legacy_or_truncated_current_ambiguous'])
                    self.assertEqual(report['last_complete_snapshot_index'], 0 if count == 11 else None)
        report = probe.analyze_text(capture(8)+'\n'+sampled_capture(8))
        self.assertTrue(report['snapshots'][0]['legacy_or_truncated_current_ambiguous'])
        self.assertFalse(report['snapshots'][1]['stage_set_complete'])
        self.assertEqual(report['last_complete_snapshot_index'], 0)

    def test_all_stage_stride_policies_and_invalid_values(self):
        for name, expected in EXPECTED_STRIDES.items():
            for invalid in (0, -1, 64, 128 if expected == 1 else 1, '1.0', 'true', '01', 2**32):
                with self.subTest(stage=name, invalid=invalid), self.assertRaises(ValueError):
                    probe._stage(sampled_stage(name, sampling_stride=invalid)[len(probe.STAGE_MARKER):],
                                 10000000, 'sampled')

    def test_sampled_header_literals_are_required_and_cannot_hybridize(self):
        for marker in SAMPLED_MARKERS:
            for replacement in ('', marker.split('=')[0]+'=false'):
                text = sampled_header().replace(' '+marker, ' '+replacement)
                with self.subTest(marker=marker, replacement=replacement), self.assertRaises(ValueError):
                    probe.analyze_text(text)
        cases = [
            sampled_header().replace('binding_cache=separately-reported', 'binding_cache=not-implemented'),
            sampled_header().replace('binding_cache=separately-reported', 'binding_cache=unknown'),
            header().replace('binding_cache=not-implemented', 'binding_cache=separately-reported'),
            sampled_header()+' observed_max_only=true', sampled_header()+' sampling_stride=1',
        ]
        for text in cases:
            with self.subTest(text=text[:150]), self.assertRaises(ValueError):
                probe.analyze_text(text)

    def test_stage_stride_missing_duplicate_or_mixed_format_is_rejected(self):
        cases = [sampled_header()+'\n'+stage(), header()+'\n'+sampled_stage(),
                 sampled_header()+'\n'+sampled_stage()+' sampling_stride=1',
                 sampled_header()+'\n'+sampled_stage()+' observed_max_only=true',
                 sampled_header()+'\n'+sampled_stage()+'\n'+stage('NativeRender'),
                 header()+'\n'+stage()+'\n'+sampled_stage('NativeRender')]
        for text in cases:
            with self.subTest(text=text[-100:]), self.assertRaises(ValueError):
                probe.analyze_text(text)

    def test_unknown_extensions_remain_unknown_under_sampled_profile(self):
        text = sampled_header()+' future_policy=unreviewed'
        text += '\n'+sampled_stage()+' future_count=9007199254740995'
        snapshot = probe.analyze_text(text)['snapshots'][0]
        self.assertEqual(snapshot['profile'], 'sampled')
        self.assertTrue(snapshot['contains_unknown_fields'])
        self.assertFalse(snapshot['field_contract_certified'])
        self.assertEqual(snapshot['unknown_fields']['future_policy']['status'], 'unknown')
        self.assertEqual(snapshot['stages'][0]['unknown_fields']['future_count']['raw'], '9007199254740995')
        self.assertEqual(snapshot['stages'][0]['sampling']['declared_stride'], 1)
        for extra in (' future_policy=a future_policy=b', ' future_policy='):
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                probe.analyze_text(sampled_header()+extra)

    def test_mixed_profiles_never_collapse_same_process_or_frequency(self):
        report = probe.analyze_text('\n'.join((capture(), sampled_capture(), capture(), sampled_capture(qpc_hz=10000000))))
        self.assertEqual(report['profiles_present'], ['legacy', 'sampled'])
        self.assertEqual(len(report['snapshots']), 4)
        self.assertFalse(report['snapshots'][1]['repeated_report_identity'])
        self.assertTrue(report['snapshots'][2]['repeated_report_identity'])
        self.assertEqual(report['snapshots'][1]['previous_identity_relation'], 'same-process-and-hz')
        self.assertEqual(report['snapshots'][1]['previous_profile_relation'], 'different-profile')
        self.assertEqual([(entry['profile'], entry['snapshot_index']) for entry in report['last_complete_by_identity']],
                         [('legacy', 2), ('sampled', 3)])

    def test_accepted_samples_counters_and_binding_cache_are_not_extrapolated(self):
        text = sampled_capture(ShaderHit=100000, ShaderMiss=7, TargetDraw=99,
                               BindingCapture=11, NestedSuppressed=222, dropped_samples=3, dropped_counters=2)
        # Separate cache records are deliberately outside this report's parser.
        text += '\n[fsr-binding-cache] binding-hit=900000 binding-miss=1 overflow=0'
        report = probe.analyze_text(text)
        snapshot = report['snapshots'][0]
        self.assertEqual(snapshot['header']['ShaderHit'], '100000')
        self.assertFalse(snapshot['counter_semantics']['total_call_denominator'])
        self.assertTrue(snapshot['counter_semantics']['can_drop_or_overflow'])
        self.assertEqual(snapshot['coverage'], 'unknown')
        self.assertIn('dropped_samples', snapshot['loss_reasons'])
        self.assertIsNone(snapshot['binding_cache_reporting']['real_binding_cache_hit_rate'])
        self.assertFalse(snapshot['binding_cache_reporting']['separate_metrics_parsed'])
        self.assertFalse(snapshot['binding_cache_reporting']['ShaderHit_is_binding_cache_hit'])
        for entry in snapshot['stages']:
            self.assertEqual(entry['fields']['samples'], '2')
            self.assertEqual(entry['fields']['sum_ms'], '2')
            self.assertEqual(entry['max_call']['duration_ticks'], '10000')
            self.assertEqual(entry['percentile_intervals']['p99']['nearest_rank'], '2')
            self.assertFalse(entry['population_statistics_estimated'])
            self.assertEqual(entry['sampling']['tls_phase_scope'], 'thread-and-stage')
            self.assertFalse(entry['sampling']['collector_reset_resets_other_thread_tls_phase'])
        for key in ('sampling_extrapolation_performed', 'stride_expansion_performed',
                    'population_call_count_estimated', 'population_cpu_cost_estimated',
                    'population_percentiles_estimated', 'present_is_render_denominator',
                    'binding_cache_hit_rate_included', 'ShaderHit_is_binding_cache_hit'):
            self.assertIs(report['semantics'][key], False)

    def test_sampled_high_qpc_precision_and_quantile_constraints_are_preserved(self):
        begin = 2**63-100
        text = sampled_stage(samples=1, sum_ms='0.0003', mean_ms='0.0003', max_ms='0.0003',
                             max_begin_qpc=begin, max_end_qpc=begin+3,
                             p95_lower_ms=0, p95_upper_ms='0.5', p99_lower_ms=0, p99_upper_ms='0.5')
        report = probe.analyze_text(sampled_header(process_creation_filetime=2**64-1, present_index=2**64-1)+'\n'+text)
        snapshot = json.loads(json.dumps(report, allow_nan=False))['snapshots'][0]
        self.assertEqual(snapshot['header']['process_creation_filetime'], str(2**64-1))
        self.assertEqual(snapshot['stages'][0]['max_call']['duration_ticks'], '3')
        self.assertEqual(snapshot['stages'][0]['max_call']['begin_qpc'], str(begin))
        for record in (
            QuantileRankTests.record(1, p95_ticks=2500, p99_ticks=2500),
            QuantileRankTests.record(100, maximum=1010000, p95_ticks=2500, p99_ticks=2500,
                                    sum_ticks=99990000, over33_ms=99, over50_ms=99, over100_ms=99),
        ):
            with self.subTest(record=record[-120:]), self.assertRaisesRegex(ValueError, 'p95 interval'):
                probe.analyze_text(sampled_header()+'\n'+record.replace('stage=TokenWork', 'stage=TokenWork sampling_stride=1'))


class MemoryFS:
    """Small inode/link-aware filesystem model; never writes to the host disk."""
    root = Path('D:/__hook_cpu_probe_memory_only__')

    def __init__(self):
        self.files, self.dirs, self.links = {}, set(), {}
        self.serial, self.replacements, self.temps = 100, [], []
        self.source, self.output = self.root/'input.log', self.root/'reports/report.json'
        self.put(self.source, capture().encode())
        for name in probe.SOURCE_SHA256:
            self.put(probe.SOURCE_ROOT/name, (probe.SOURCE_ROOT/name).read_bytes())
        self.put(Path(probe.__file__).resolve(), Path(probe.__file__).read_bytes())

    def norm(self, path):
        return Path(os.path.abspath(path))

    def resolve(self, path, strict=False):
        path = self.norm(path)
        for _ in range(20):
            for alias, target in self.links.items():
                if path.is_relative_to(alias):
                    path = target/path.relative_to(alias)
                    break
            else:
                if strict and path not in self.files and path not in self.dirs:
                    raise FileNotFoundError(str(path))
                return path
        raise RuntimeError('link cycle')

    def put(self, path, data):
        path = self.norm(path)
        self.serial += 1
        self.files[path] = SimpleNamespace(data=data, inode=self.serial, mtime=self.serial)
        self.dirs.update(path.parents)

    def stat(self, path):
        path = self.resolve(path, strict=True)
        if path in self.dirs:
            return SimpleNamespace(st_dev=1, st_ino=1, st_size=0, st_mtime_ns=1, st_ctime_ns=1)
        node = self.files[path]
        return SimpleNamespace(st_dev=1, st_ino=node.inode, st_size=len(node.data),
                               st_mtime_ns=node.mtime, st_ctime_ns=node.inode)

    def read(self, path):
        return self.files[self.resolve(path, strict=True)].data

    def mutate(self, path, data):
        node = self.files[self.resolve(path, strict=True)]
        node.data = data
        node.mtime += 1

    def iterdir(self, path):
        resolved = self.resolve(path, strict=True)
        return iter(sorted({candidate for candidate in self.files.keys() | self.dirs | self.links.keys()
                            if candidate.parent == resolved}, key=str))

    def samefile(self, left, right):
        return self.stat(left).st_ino == self.stat(right).st_ino

    def hardlink(self, source, destination):
        destination = self.norm(destination)
        self.files[destination] = self.files[self.resolve(source, strict=True)]
        self.dirs.update(destination.parents)

    def symlink(self, source, destination):
        destination = self.norm(destination)
        self.links[destination] = self.norm(source)
        self.dirs.update(destination.parents)

    def mkdir(self, path, **kwargs):
        self.dirs.add(self.resolve(path))
        self.dirs.update(self.resolve(path).parents)

    def unlink(self, path, **kwargs):
        self.files.pop(self.norm(path), None)

    def temporary(self, *, dir, prefix, suffix, **kwargs):
        name = Path(dir)/(prefix+'virtual'+suffix)
        self.put(name, b'')
        self.temps.append(name)
        fs = self
        class Stream:
            def __init__(self):
                self.name = str(name)
            def __enter__(self):
                return self
            def write(self, text):
                fs.mutate(name, text.encode('utf-8'))
            def __exit__(self, *args):
                return False
        return Stream()

    def replace(self, source, destination):
        source, destination = self.norm(source), self.norm(destination)
        # A file link at destination is replaced as an entry, never followed.
        self.links.pop(destination, None)
        self.files[destination] = self.files.pop(source)
        self.replacements.append((source, destination))

    def install(self, stack):
        for name, function in {
            'resolve': lambda p, strict=False: self.resolve(p, strict),
            'stat': lambda p, **k: self.stat(p), 'read_bytes': lambda p: self.read(p),
            'is_file': lambda p: self.resolve(p) in self.files,
            'is_dir': lambda p: self.resolve(p) in self.dirs,
            'exists': lambda p: self.resolve(p) in self.files or self.resolve(p) in self.dirs,
            'iterdir': lambda p: self.iterdir(p), 'samefile': lambda p, q: self.samefile(p, q),
            'mkdir': lambda p, **k: self.mkdir(p, **k), 'unlink': lambda p, **k: self.unlink(p, **k),
        }.items():
            stack.enter_context(patch.object(Path, name, function))
        stack.enter_context(patch.object(probe.tempfile, 'NamedTemporaryFile', self.temporary))
        stack.enter_context(patch.object(probe.os, 'replace', self.replace))


class FileTests(unittest.TestCase):
    def setUp(self):
        self.fs = MemoryFS()
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.fs.install(self.stack)

    def run_cli(self, *extra, output=None):
        with redirect_stderr(io.StringIO()) as stderr, redirect_stdout(io.StringIO()) as stdout:
            result = probe.main(['--input', str(self.fs.source), '--output', str(output or self.fs.output), *extra])
        return result, stdout.getvalue(), stderr.getvalue()

    def test_full_cli_strict_json_source_sha_and_stat_binding(self):
        digest = hashlib.sha256(self.fs.read(self.fs.source)).hexdigest()
        code, stdout, stderr = self.run_cli('--expected-sha256', digest.upper())
        self.assertEqual((code, stderr), (0, ''))
        self.assertIn('PASS', stdout)
        report = json.loads(self.fs.read(self.fs.output))
        self.assertEqual(report['input']['sha256'], digest)
        self.assertEqual(report['input']['stat_before'], report['input']['stat_after'])
        self.assertTrue(report['input']['expected_sha256_verified'])
        expected = all(binding['sha256'] == binding['parser_reference_sha256']
                       for binding in report['source_bindings'])
        self.assertEqual(report['source_contract_certified'], expected)
        self.assertEqual(len(report['source_bindings']), 3)
        self.assertEqual(len(self.fs.replacements), 1)
        self.assertTrue(all(path not in self.fs.files for path in self.fs.temps))

    def test_matching_reference_certifies_only_known_fields(self):
        references = {name: hashlib.sha256(self.fs.read(probe.SOURCE_ROOT/name)).hexdigest()
                      for name in probe.SOURCE_SHA256}
        with patch.dict(probe.SOURCE_SHA256, references):
            self.assertTrue(probe.analyze_file(self.fs.source)['source_contract_certified'])
            self.fs.mutate(self.fs.source, (capture()+' future=opaque').encode())
            report = probe.analyze_file(self.fs.source)
            self.assertTrue(report['source_reference_matches'])
            self.assertFalse(report['source_contract_certified'])

    def test_wrong_hash_bad_hash_missing_input_and_bad_utf8_never_publish(self):
        for digest in ('0'*64, 'xyz'):
            with self.subTest(digest=digest):
                self.assertEqual(self.run_cli('--expected-sha256', digest)[0], 2)
        self.fs.mutate(self.fs.source, b'\xff')
        self.assertEqual(self.run_cli()[0], 2)
        del self.fs.files[self.fs.norm(self.fs.source)]
        self.assertEqual(self.run_cli()[0], 2)
        self.assertFalse(self.fs.replacements)
        self.assertFalse(self.fs.temps)

    def test_direct_hardlink_symlink_and_directory_junction_aliases(self):
        before = self.fs.read(self.fs.source)
        self.assertEqual(self.run_cli(output=self.fs.source)[0], 2)
        self.fs.hardlink(self.fs.source, self.fs.output)
        self.assertEqual(self.run_cli()[0], 2)
        del self.fs.files[self.fs.norm(self.fs.output)]
        self.fs.symlink(self.fs.source, self.fs.output)
        self.assertEqual(self.run_cli()[0], 2)
        del self.fs.links[self.fs.norm(self.fs.output)]
        alias = self.fs.root/'junction'
        self.fs.symlink(self.fs.source.parent, alias)
        self.assertEqual(self.run_cli(output=alias/'input.log')[0], 2)
        self.assertEqual(self.fs.read(self.fs.source), before)
        self.assertFalse(self.fs.temps)

    def test_source_inside_reserved_tree_and_contract_source_alias(self):
        self.assertEqual(self.run_cli(output=self.fs.root/'report.json')[0], 2)
        self.fs.hardlink(probe.SOURCE_ROOT/'HookCpuTiming.h', self.fs.output)
        self.assertEqual(self.run_cli()[0], 2)

    def test_mutation_while_reading_is_rejected(self):
        original = self.fs.read
        def changed(path):
            raw = original(path)
            if self.fs.norm(path) == self.fs.norm(self.fs.source):
                self.fs.mutate(path, raw+b'changed')
            return raw
        with patch.object(Path, 'read_bytes', lambda p: changed(p)):
            self.assertEqual(self.run_cli()[0], 2)
        self.assertFalse(self.fs.temps)

    def test_same_bytes_identity_change_is_rejected_before_publish(self):
        report = probe.analyze_file(self.fs.source)
        self.fs.put(self.fs.source, self.fs.read(self.fs.source))
        with self.assertRaisesRegex(ValueError, 'identity/stat'):
            probe.write_report(self.fs.source, self.fs.output, report)
        self.assertFalse(self.fs.temps)

    def test_writer_cannot_protect_a_different_input_argument(self):
        report = probe.analyze_file(self.fs.source)
        other = self.fs.root/'other.log'
        self.fs.put(other, b'not the analyzed input')
        before = self.fs.read(self.fs.source)
        with self.assertRaisesRegex(ValueError, 'does not match report input'):
            probe.write_report(other, self.fs.source, report)
        self.assertEqual(self.fs.read(self.fs.source), before)

    def test_source_contract_drift_is_bound_but_not_certified(self):
        path = probe.SOURCE_ROOT/'HookCpuScopes.h'
        self.fs.mutate(path, self.fs.read(path)+b'\n// changed\n')
        report = probe.analyze_file(self.fs.source)
        self.assertFalse(report['source_contract_certified'])
        self.assertIn('source_contract_unknown', report)
        self.assertNotEqual(report['source_bindings'][2]['sha256'], probe.SOURCE_SHA256[path.name])

    def test_alias_created_while_reading_is_rejected(self):
        original, before = self.fs.read, self.fs.read(self.fs.source)
        def linked(path):
            raw = original(path)
            if self.fs.norm(path) == self.fs.norm(self.fs.source):
                self.fs.hardlink(self.fs.source, self.fs.output)
            return raw
        with patch.object(Path, 'read_bytes', lambda p: linked(p)):
            self.assertEqual(self.run_cli()[0], 2)
        self.assertEqual(original(self.fs.source), before)
        self.assertFalse(self.fs.temps)

    def test_atomic_replace_never_follows_a_late_hardlink(self):
        before = self.fs.read(self.fs.source)
        def replace(source, destination):
            self.fs.hardlink(self.fs.source, destination)
            self.fs.replace(source, destination)
        with patch.object(probe.os, 'replace', replace):
            self.assertEqual(self.run_cli()[0], 0)
        self.assertEqual(self.fs.read(self.fs.source), before)
        self.assertFalse(self.fs.samefile(self.fs.source, self.fs.output))

    def test_input_changes_after_serialization_rejected_and_temp_cleaned(self):
        report = probe.analyze_file(self.fs.source)
        temporary = self.fs.temporary
        def changed(**kwargs):
            stream = temporary(**kwargs)
            self.fs.mutate(self.fs.source, b'changed')
            return stream
        with patch.object(probe.tempfile, 'NamedTemporaryFile', changed), self.assertRaises(ValueError):
            probe.write_report(self.fs.source, self.fs.output, report)
        self.assertFalse(self.fs.replacements)
        self.assertTrue(all(path not in self.fs.files for path in self.fs.temps))

    def test_replace_error_preserves_previous_report_and_cleans_temp(self):
        self.fs.put(self.fs.output, b'previous report')
        with patch.object(probe.os, 'replace', side_effect=OSError('simulated replace failure')):
            self.assertEqual(self.run_cli()[0], 2)
        self.assertEqual(self.fs.read(self.fs.output), b'previous report')
        self.assertTrue(all(path not in self.fs.files for path in self.fs.temps))

    def test_missing_required_cli_option_and_help(self):
        for argv, code in (([], 2), (['--input', str(self.fs.source)], 2), (['--help'], 0)):
            with self.subTest(argv=argv), redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as caught:
                    probe.main(argv)
                self.assertEqual(caught.exception.code, code)


class SampledFileTests(unittest.TestCase):
    # Reuse only the memory filesystem setup/CLI helpers, not inherited test methods.
    setUp = FileTests.setUp
    run_cli = FileTests.run_cli

    def matching_sampled_reference(self):
        references = {name: hashlib.sha256(self.fs.read(probe.SOURCE_ROOT/name)).hexdigest()
                      for name in probe.SAMPLED_SOURCE_SHA256}
        return patch.dict(probe.SAMPLED_SOURCE_SHA256, references)

    def test_sampled_cli_binds_its_source_profile_and_publishes_atomically(self):
        self.fs.mutate(self.fs.source, sampled_capture().encode())
        digest = hashlib.sha256(self.fs.read(self.fs.source)).hexdigest()
        with self.matching_sampled_reference():
            code, _, stderr = self.run_cli('--expected-sha256', digest)
        self.assertEqual((code, stderr), (0, ''))
        report = json.loads(self.fs.read(self.fs.output))
        self.assertEqual(report['input']['sha256'], digest)
        self.assertTrue(report['source_reference_matches'])
        self.assertTrue(report['source_contract_certified'])
        self.assertEqual(report['runtime_compiled_source_identity'], 'unknown')
        self.assertEqual(report['snapshots'][0]['runtime_compiled_source_identity'], 'unknown')
        self.assertEqual(report['profiles_present'], ['sampled'])
        self.assertEqual(len(self.fs.replacements), 1)
        for binding in report['source_bindings']:
            self.assertEqual(set(binding['parser_reference_sha256_by_profile']), {'sampled'})
            self.assertTrue(binding['matches_parser_reference_by_profile']['sampled'])
        self.assertTrue(all(path not in self.fs.files for path in self.fs.temps))

    def test_current_source_never_certifies_mixed_legacy_snapshots(self):
        # These legacy references are frozen independently of the sampled references.
        self.assertEqual(probe.SOURCE_SHA256, {
            'D3D11Surface.cpp': '54f6469e5517b897af2347960d5966140617121b073d9555714580c2eb82effb',
            'HookCpuTiming.h': 'b0da3d9fcd671ae81aa6c563aaa9aba993a9f1a14d01cc41b52820484d1f5c45',
            'HookCpuScopes.h': 'b960a2c9d2978edd0be362cb85273c30b786ce5e7e3165f24adbd14ad741c9c5',
        })
        self.assertEqual(probe.SAMPLED_SOURCE_SHA256, {
            'D3D11Surface.cpp': 'a72ea81021554618aafd7ca23f0a8080bb1ec94a7ef704797f65f4e4d1f50136',
            'HookCpuTiming.h': '9beca35fb2ce78a3bd7f430723d06e8b629cdd45d623f913fe15a1916c4133ab',
            'HookCpuScopes.h': '431140c5bae31cfcde7e758f15bb2475e2be2278c0e9f1c2b30455348f91fb25',
        })
        self.fs.mutate(self.fs.source, (capture()+'\n'+sampled_capture()).encode())
        with self.matching_sampled_reference():
            report = probe.analyze_file(self.fs.source)
        self.assertFalse(report['source_reference_matches'])
        self.assertFalse(report['source_contract_certified'])
        self.assertFalse(report['profile_source_contracts']['legacy']['read_source_matches_reference'])
        self.assertTrue(report['profile_source_contracts']['sampled']['source_contract_certified'])
        self.assertFalse(report['snapshots'][0]['source_contract_certified'])
        self.assertTrue(report['snapshots'][1]['source_contract_certified'])
        for binding in report['source_bindings']:
            self.assertIsNone(binding['parser_reference_sha256'])
            self.assertEqual(set(binding['parser_reference_sha256_by_profile']), {'legacy', 'sampled'})
        self.assertEqual(report['profile_source_contracts']['sampled']['runtime_compiled_source_identity'], 'unknown')

    def test_each_sampled_reference_source_drift_blocks_certification(self):
        self.fs.mutate(self.fs.source, sampled_capture().encode())
        with self.matching_sampled_reference():
            self.assertTrue(probe.analyze_file(self.fs.source)['source_contract_certified'])
            for name in probe.SAMPLED_SOURCE_SHA256:
                path = probe.SOURCE_ROOT/name
                original = self.fs.read(path)
                self.fs.mutate(path, original+b'\n// simulated source drift\n')
                with self.subTest(source=name):
                    report = probe.analyze_file(self.fs.source)
                    self.assertFalse(report['source_contract_certified'])
                    self.assertFalse(report['profile_source_contracts']['sampled']['read_source_matches_reference'])
                    self.assertEqual(report['runtime_compiled_source_identity'], 'unknown')
                self.fs.mutate(path, original)

    def test_unknown_sampled_extensions_block_certification_without_losing_samples(self):
        self.fs.mutate(self.fs.source, (sampled_header()+' future_policy=unknown\n'+
                                       sampled_stage()+' future_stat=9007199254740995').encode())
        with self.matching_sampled_reference():
            report = probe.analyze_file(self.fs.source)
        self.assertTrue(report['source_reference_matches'])
        self.assertFalse(report['source_contract_certified'])
        self.assertFalse(report['profile_source_contracts']['sampled']['source_contract_certified'])
        self.assertTrue(report['snapshots'][0]['stages'][0]['accepted_max_call_usable'])

    def test_sampled_cli_still_rejects_input_and_reference_output_aliases(self):
        self.fs.mutate(self.fs.source, sampled_capture().encode())
        self.fs.hardlink(self.fs.source, self.fs.output)
        original = self.fs.read(self.fs.source)
        self.assertEqual(self.run_cli()[0], 2)
        self.assertEqual(self.fs.read(self.fs.source), original)
        del self.fs.files[self.fs.norm(self.fs.output)]
        path = probe.SOURCE_ROOT/'HookCpuScopes.h'
        original_reference = self.fs.read(path)
        self.fs.hardlink(path, self.fs.output)
        self.assertEqual(self.run_cli()[0], 2)
        self.assertEqual(self.fs.read(path), original_reference)
        self.assertFalse(self.fs.temps)

    def test_sampled_source_mutation_before_publication_is_rejected(self):
        self.fs.mutate(self.fs.source, sampled_capture().encode())
        report = probe.analyze_file(self.fs.source)
        path = probe.SOURCE_ROOT/'HookCpuTiming.h'
        self.fs.mutate(path, self.fs.read(path)+b'\n// changed before publication\n')
        with self.assertRaisesRegex(ValueError, 'SHA256'):
            probe.write_report(self.fs.source, self.fs.output, report)
        self.assertFalse(self.fs.replacements)
        self.assertFalse(self.fs.temps)


if __name__ == '__main__':
    unittest.main(verbosity=2)
