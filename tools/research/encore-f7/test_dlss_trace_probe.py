from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import dlss_trace_probe as probe


COLUMNS = (
    'actual_call,begin_qpc,end_qpc,completed,sdk_result,slot,source,instance,context,'
    'session_serial,feature_serial,history_binding_serial,'
    'previous_generation,previous_frame,previous_flags,effective_generation,effective_frame,effective_flags,'
    'current_generation,current_frame,reset_reasons,sdk_reset,lifecycle_flags,'
    'jitter_x,jitter_y,motion_scale_x,motion_scale_y,token_jitter_encoding,render_w,render_h,display_w,display_h'
)


def event(call, begin, end, **changes):
    values = [call, begin, end, 1, 1, 0, 77, 77, 88, 1, 1, 1,
              call-1, call-1, 3, call-1, call-1, 3, call, call, 0, 0, 0,
              '0.125', '-0.25', '1536', '960', 1, 1536, 960, 2560, 1600]
    fields = dict(zip(COLUMNS.split(','), values))
    fields.update(changes)
    return [str(fields[name]) for name in COLUMNS.split(',')]


def capture(rows=None, *, configuration=1, anchor=None, frequency=1000,
            mask=0, incomplete=0, tail=True, **end_changes):
    rows = rows if rows is not None else [event(1, 110, 120), event(2, 140, 170), event(3, 190, 210)]
    if anchor is None:
        anchor = int(rows[0][1]) if rows else 0
    sealed = max([anchor+frequency]+[int(row[2]) for row in rows if int(row[3]) == 1])
    flags = dict(mask=mask, incomplete=incomplete, invalid_clock=int(bool(mask & 4)),
                 capacity=int(bool(mask & 1)), truncated=int(bool(mask & 1)),
                 early_stop=int(bool(mask & 2)), stop_fallback=int(bool(mask & 512)),
                 not_started=int(bool(mask & 64)), invalid_ticket=int(bool(mask & 8)),
                 call_overflow=int(bool(mask & 16)))
    metadata = (
        f'F7_DLSS_RESET_TRACE_BEGIN version=1 configuration={configuration} '
        f'delay_seconds=0 window_seconds=1 capacity=8192 qpc_frequency={frequency} '
        f'started=1 anchor_qpc={anchor} delay_ticks=0 end_offset_ticks={frequency} '
        f'sealed_qpc={sealed} flush_requested_qpc={sealed} '
        'performance_baseline=0 disk_write_success=unknown logger_write_cost=caller-measured'
    )
    text = metadata+'\nflags '+' '.join(f'{k}={v}' for k, v in flags.items())+'\n'
    text += 'columns='+COLUMNS+'\n'
    text += ('history_flags=occupied:1|recorded:2|needsReset:4|needsRecreate:8 '
             'lifecycle_flags=created:1|recreated:2|rebind:4|migrate:8 '
             'reset_reasons=requested:1|needsReset:2|notRecorded:4|generationNotIncreasing:8|frameIndexDiscontinuity:16 '
             'history_binding_serial_semantics=successful-bind-or-migrate-counter-not-sdk-history-length '
             'lifecycle_serials=caller-saturating\n')
    text += ''.join('E,'+','.join(row)+'\n' for row in rows)
    if tail:
        completed = sum(int(row[3]) for row in rows)
        footer = dict(tail_marker=1, configuration=configuration,
                      actual_calls=int(rows[-1][0]) if rows else 0,
                      first_captured_call=int(rows[0][0]) if rows else 0,
                      last_captured_call=int(rows[-1][0]) if rows else 0,
                      events=len(rows), completed=completed, unfinished=len(rows)-completed,
                      sdk_resets=sum(int(row[21]) != 0 for row in rows),
                      incomplete=incomplete, serialization_us=17,
                      missing_tail_means_incomplete=1, gpu_completion='unproven',
                      performance_baseline=0, disk_write_success='unknown')
        footer.update(end_changes)
        text += 'F7_DLSS_RESET_TRACE_END '+' '.join(f'{k}={v}' for k, v in footer.items())+'\n'
    return text


def block(text):
    result = probe.analyze_text(text)
    assert result['schema'] == 1
    return result['blocks'][0]


class TraceTimingTests(unittest.TestCase):
    def test_cpu_durations_and_between_call_gaps_are_separate(self):
        report = probe.analyze_text(capture())
        value = report['blocks'][0]
        self.assertTrue(value['capture_complete'])
        self.assertTrue(value['timing_usable'])
        self.assertEqual(value['events'], 3)
        self.assertEqual(value['completed'], 3)
        metrics = value['evaluate_cpu']
        self.assertEqual(metrics['sum_ticks'], '60')
        self.assertEqual(metrics['max_ticks'], '30')
        self.assertEqual(metrics['p50_ticks'], '20')
        self.assertEqual(metrics['p95_ticks'], '30')
        self.assertEqual(metrics['p99_ticks'], '30')
        self.assertEqual(metrics['sum_ms'], 60.0)
        self.assertEqual(value['between_evaluates_cpu']['sum_ticks'], '40')
        self.assertEqual(value['between_evaluates_cpu']['count'], 2)
        for name in ('gpu_completion_proven', 'causality_proven', 'performance_baseline'):
            self.assertIs(report[name], False)

    def test_high_qpc_ticks_are_subtracted_before_any_float_conversion(self):
        anchor = 9007199254740993
        value = block(capture([event(1, anchor, anchor+1)], anchor=anchor))
        self.assertEqual(value['evaluate_cpu']['sum_ticks'], '1')
        self.assertEqual(value['evaluate_cpu']['sum_ms'], 1.0)
        self.assertEqual(value['configuration'], '1')

    def test_one_long_sdk_call_is_not_clipped_at_window_end(self):
        value = block(capture([event(1, 110, 120), event(2, 1090, 1300)], anchor=110))
        self.assertEqual(value['evaluate_cpu']['sum_ticks'], '220')
        self.assertTrue(value['timing_usable'])

    def test_missing_tail_preserves_prefix_and_marks_incomplete(self):
        value = block(capture(tail=False))
        self.assertIs(value['tail_present'], False)
        self.assertIs(value['capture_complete'], False)
        self.assertEqual(value['events'], 3)
        self.assertEqual(value['evaluate_cpu']['count'], 3)

    def test_unfinished_sdk_call_has_no_duration_even_with_zero_result(self):
        rows = [event(1, 110, 0, completed=0, sdk_result=0)]
        value = block(capture(rows, incomplete=1))
        self.assertEqual(value['unfinished'], 1)
        self.assertFalse(value['capture_complete'])
        self.assertEqual(value['evaluate_cpu']['count'], 0)
        self.assertIsNone(value['evaluate_cpu']['sum_ticks'])

    def test_actual_call_gap_is_not_interpreted_as_sdk_free_time(self):
        value = block(capture([event(1, 110, 120), event(3, 200, 210)]))
        self.assertEqual(value['noncontiguous_pairs'], 1)
        self.assertEqual(value['between_evaluates_cpu']['count'], 0)

    def test_overlap_is_flagged_and_not_converted_to_negative_gap(self):
        value = block(capture([event(1, 110, 150), event(2, 140, 170)]))
        self.assertEqual(value['overlapping_pairs'], 1)
        metrics = value['between_evaluates_cpu']
        self.assertTrue(metrics is None or metrics['count'] == 0)

    def test_invalid_clock_disables_all_cpu_timing(self):
        value = block(capture([event(1, 110, 109)], mask=4, incomplete=1))
        self.assertFalse(value['timing_usable'])
        self.assertIsNone(value['evaluate_cpu'])
        self.assertIsNone(value['between_evaluates_cpu'])
        self.assertEqual(value['completed'], 1)

    def test_invalid_ticket_disables_cpu_time_association(self):
        value = block(capture(mask=8, incomplete=1))
        self.assertFalse(value['timing_usable'])
        self.assertIsNone(value['evaluate_cpu'])

    def test_capacity_prefix_can_be_usable_but_is_not_complete(self):
        rows = [event(i+1, 110+i, 111+i) for i in range(8192)]
        text = capture(rows, frequency=10000, mask=1, incomplete=1)
        # Capacity seals in Begin; the final End may follow that sealed QPC.
        text = text.replace('sealed_qpc=10110', 'sealed_qpc=8301')
        value = block(text)
        self.assertFalse(value['capture_complete'])
        self.assertTrue(value['timing_usable'])
        self.assertEqual(value['evaluate_cpu']['count'], 8192)

    def test_stop_fallback_is_not_a_complete_capture(self):
        value = block(capture(mask=512, incomplete=1))
        self.assertFalse(value['capture_complete'])
        self.assertEqual(value['evaluate_cpu']['count'], 3)

    def test_nonfinite_observed_input_does_not_invent_or_erase_qpc(self):
        value = block(capture([event(1, 110, 120, jitter_x='nan', motion_scale_y='inf')]))
        self.assertEqual(value['evaluate_cpu']['sum_ticks'], '10')
        self.assertTrue(value['warnings'])

    def test_failed_sdk_return_still_has_a_measured_cpu_call(self):
        value = block(capture([event(1, 110, 130, sdk_result=0xbad00005)]))
        self.assertEqual(value['evaluate_cpu']['count'], 1)
        self.assertEqual(value['evaluate_cpu']['sum_ticks'], '20')

    def test_saturated_serial_is_reported_without_joining_its_identity(self):
        maximum = 2**64-1
        value = block(capture([event(1, 110, 120, feature_serial=maximum)]))
        self.assertTrue(value['warnings'])
        self.assertEqual(value['evaluate_cpu']['count'], 1)

    def test_multiple_blocks_are_never_joined_into_one_interval(self):
        report = probe.analyze_text(capture(configuration=1)+'ordinary logger line\n'+capture(configuration=2))
        self.assertEqual([v['configuration'] for v in report['blocks']], ['1', '2'])
        self.assertEqual([v['between_evaluates_cpu']['count'] for v in report['blocks']], [2, 2])

    def test_new_begin_seals_previous_missing_tail_without_merging(self):
        report = probe.analyze_text(capture(configuration=1, tail=False)+capture(configuration=2))
        self.assertEqual(len(report['blocks']), 2)
        self.assertFalse(report['blocks'][0]['capture_complete'])
        self.assertTrue(report['blocks'][1]['capture_complete'])


class TraceFormatTests(unittest.TestCase):
    def test_no_begin_is_rejected(self):
        with self.assertRaises(ValueError):
            probe.analyze_text('ordinary log only\n')

    def test_footer_count_mismatch_is_rejected(self):
        with self.assertRaises(ValueError):
            probe.analyze_text(capture(events=9))

    def test_footer_configuration_mismatch_is_rejected(self):
        text = capture().replace('TRACE_END tail_marker=1 configuration=1',
                                 'TRACE_END tail_marker=1 configuration=2')
        with self.assertRaises(ValueError):
            probe.analyze_text(text)

    def test_unknown_version_is_rejected(self):
        with self.assertRaises(ValueError):
            probe.analyze_text(capture().replace('version=1', 'version=2', 1))

    def test_duplicate_header_key_is_rejected(self):
        with self.assertRaises(ValueError):
            probe.analyze_text(capture().replace('version=1', 'version=1 version=1', 1))

    def test_duplicate_columns_is_rejected(self):
        with self.assertRaises(ValueError):
            probe.analyze_text(capture().replace('columns='+COLUMNS, 'columns='+COLUMNS+'\ncolumns='+COLUMNS))

    def test_changed_columns_is_rejected(self):
        with self.assertRaises(ValueError):
            probe.analyze_text(capture().replace('jitter_x,jitter_y', 'jitter_y,jitter_x'))

    def test_corrupt_event_is_not_silently_skipped(self):
        with self.assertRaises(ValueError):
            probe.analyze_text(capture().replace('E,1,110', 'E,not-an-int,110', 1))

    def test_duplicate_call_is_rejected(self):
        with self.assertRaises(ValueError):
            probe.analyze_text(capture([event(1, 110, 120), event(1, 140, 150)]))

    def test_boolean_domain_is_exact(self):
        with self.assertRaises(ValueError):
            probe.analyze_text(capture([event(1, 110, 120, completed=2)], completed=1, unfinished=0))

    def test_qpc_and_identity_integer_overflows_are_rejected(self):
        for changes in (dict(begin_qpc=2**63), dict(source=2**64), dict(current_frame=2**32)):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                probe.analyze_text(capture([event(1, 110, 120, **changes)]))

    def test_unexpected_interior_log_line_is_rejected(self):
        with self.assertRaises(ValueError):
            probe.analyze_text(capture().replace('E,2,140', '[unexpected logger data]\nE,2,140', 1))

    def test_actual_calls_may_include_uncaptured_boundary_call(self):
        value = block(capture(actual_calls=4))
        self.assertEqual(value['events'], 3)
        self.assertTrue(value['capture_complete'])


class TraceConsistencyTests(unittest.TestCase):
    def test_zero_delay_prefix_truncated_after_descriptor_keeps_missing_tail(self):
        prefix = chr(10).join(capture().splitlines()[:4])+chr(10)
        value = block(prefix)
        self.assertFalse(value['tail_present'])
        self.assertFalse(value['capture_complete'])
        self.assertEqual(value['events'], 0)
        self.assertEqual(value['evaluate_cpu']['count'], 0)

    def test_started_zero_delay_empty_capture_is_not_complete(self):
        with self.assertRaisesRegex(ValueError, 'anchoring event'):
            probe.analyze_text(capture([]))

    def test_complete_seal_must_include_end_that_crosses_window(self):
        text = capture([event(1, 110, 1300)])
        text = text.replace('sealed_qpc=1300', 'sealed_qpc=1200')
        with self.assertRaisesRegex(ValueError, 'precedes a recorded End'):
            probe.analyze_text(text)

    def test_incomplete_seal_cannot_precede_an_observed_begin(self):
        text = capture(mask=512, incomplete=1)
        text = text.replace('sealed_qpc=1110', 'sealed_qpc=100')
        with self.assertRaisesRegex(ValueError, 'sealed QPC precedes'):
            probe.analyze_text(text)

    def test_complete_tail_sealed_before_window_end_is_rejected(self):
        text = capture().replace('sealed_qpc=1110 flush_requested_qpc=1110',
                                 'sealed_qpc=210 flush_requested_qpc=210')
        with self.assertRaisesRegex(ValueError, 'sealed before'):
            probe.analyze_text(text)

    def test_zero_delay_missing_first_call_cannot_claim_complete(self):
        value = block(capture([event(2, 140, 150), event(3, 170, 180)]))
        self.assertFalse(value['capture_complete'])
        self.assertTrue(value['warnings'])

    def test_zero_delay_first_begin_must_match_anchor_for_complete_capture(self):
        value = block(capture(anchor=100))
        self.assertFalse(value['capture_complete'])
        self.assertTrue(value['warnings'])

    def test_arbitrary_uncaptured_tail_call_count_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'one uncaptured'):
            probe.analyze_text(capture(actual_calls=99999))

    def test_cross_call_qpc_rollback_disables_timing_even_without_flag(self):
        value = block(capture([event(1, 100, 110), event(2, 200, 210), event(3, 140, 150)]))
        self.assertFalse(value['timing_usable'])
        self.assertIsNone(value['evaluate_cpu'])
        self.assertIsNone(value['between_evaluates_cpu'])
        self.assertFalse(value['capture_complete'])

    def test_missing_interior_call_cannot_claim_complete(self):
        value = block(capture([event(1, 110, 120), event(3, 200, 210)]))
        self.assertFalse(value['capture_complete'])
        self.assertEqual(value['noncontiguous_pairs'], 1)

    def test_flush_cannot_precede_sealing(self):
        text = capture().replace('flush_requested_qpc=1110', 'flush_requested_qpc=1109')
        with self.assertRaisesRegex(ValueError, 'flush QPC precedes'):
            probe.analyze_text(text)

    def test_flush_cannot_precede_completed_call(self):
        text = capture([event(1, 110, 1300)], mask=512, incomplete=1)
        text = text.replace('sealed_qpc=1300', 'sealed_qpc=1100')
        text = text.replace('flush_requested_qpc=1300', 'flush_requested_qpc=1200')
        with self.assertRaisesRegex(ValueError, 'flush QPC precedes'):
            probe.analyze_text(text)


class TraceFileTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='dlss-trace-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root/'F7.log'
        self.source.write_bytes(capture().encode('utf-8'))
        self.output = self.root/'reports/trace.json'

    def test_input_identity_is_bound_to_exact_bytes(self):
        expected = hashlib.sha256(self.source.read_bytes()).hexdigest()
        report = probe.analyze_file(self.source, expected_sha256=expected)
        self.assertEqual(report['input']['sha256'], expected)
        self.assertEqual(report['input']['bytes'], self.source.stat().st_size)
        self.assertIs(report['input']['expected_sha256_verified'], True)

    def test_wrong_expected_hash_does_not_write_output(self):
        with redirect_stderr(io.StringIO()), redirect_stdout(io.StringIO()):
            try:
                result = probe.main(['--input', str(self.source), '--output', str(self.output),
                                     '--expected-sha256', '0'*64])
            except SystemExit as error:
                result = error.code
        self.assertNotEqual(result, 0)
        self.assertFalse(self.output.exists())

    def test_output_hardlink_to_input_is_rejected_without_byte_changes(self):
        self.output.parent.mkdir()
        os.link(self.source, self.output)
        before = self.source.read_bytes()
        with redirect_stderr(io.StringIO()), redirect_stdout(io.StringIO()):
            try:
                result = probe.main(['--input', str(self.source), '--output', str(self.output)])
            except SystemExit as error:
                result = error.code
        self.assertNotEqual(result, 0)
        self.assertEqual(self.source.read_bytes(), before)

    def test_nonfinite_observation_can_be_written_as_strict_json(self):
        self.source.write_bytes(capture([event(1, 110, 120, jitter_x='nan')]).encode('utf-8'))
        with redirect_stdout(io.StringIO()):
            result = probe.main(['--input', str(self.source), '--output', str(self.output)])
        self.assertEqual(result, 0)
        value = json.loads(self.output.read_text(encoding='utf-8'),
                           parse_constant=lambda text: self.fail('non-JSON numeric constant: '+text))
        self.assertEqual(value['blocks'][0]['evaluate_cpu']['sum_ticks'], '10')

    def test_input_inside_reserved_output_tree_is_preserved(self):
        self.output.parent.mkdir()
        source = self.output.parent/'source.log'
        source.write_bytes(self.source.read_bytes())
        before = source.read_bytes()
        with redirect_stderr(io.StringIO()), redirect_stdout(io.StringIO()):
            try:
                result = probe.main(['--input', str(source), '--output', str(self.output)])
            except SystemExit as error:
                result = error.code
        self.assertNotEqual(result, 0)
        self.assertEqual(source.read_bytes(), before)
        self.assertFalse(self.output.exists())

    def test_file_changed_while_reading_is_rejected(self):
        original = Path.read_bytes
        def changed(path):
            value = original(path)
            if path == self.source:
                path.write_bytes(value+b'changed')
            return value
        with patch.object(Path, 'read_bytes', changed), self.assertRaises(ValueError):
            probe.analyze_file(self.source)

    def test_output_hardlink_created_during_read_is_rejected_without_overwriting_input(self):
        self.output.parent.mkdir()
        before = self.source.read_bytes()
        original = Path.read_bytes
        def interleaved(path):
            value = original(path)
            if path == self.source and not self.output.exists():
                os.link(self.source, self.output)
            return value
        with patch.object(Path, 'read_bytes', interleaved), redirect_stderr(io.StringIO()):
            result = probe.main(['--input', str(self.source), '--output', str(self.output),
                                 '--expected-sha256', hashlib.sha256(before).hexdigest()])
        self.assertNotEqual(result, 0)
        self.assertEqual(self.source.read_bytes(), before)

    def test_atomic_replace_does_not_follow_late_output_hardlink(self):
        self.output.parent.mkdir()
        before = self.source.read_bytes()
        replace = probe.os.replace
        def interleaved(source, destination):
            os.link(self.source, destination)
            return replace(source, destination)
        with (patch.object(probe.os, 'replace', interleaved),
              redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO())):
            result = probe.main(['--input', str(self.source), '--output', str(self.output)])
        self.assertIn(result, (0, 2))
        self.assertEqual(self.source.read_bytes(), before)
        if result == 0:
            self.assertFalse(self.output.samefile(self.source))
            self.assertTrue(json.loads(self.output.read_text())['blocks'][0]['capture_complete'])
        else:
            self.assertEqual(self.output.read_bytes(), before)
        self.assertEqual(list(self.output.parent.glob('*.tmp')), [])


if __name__ == '__main__':
    unittest.main()
