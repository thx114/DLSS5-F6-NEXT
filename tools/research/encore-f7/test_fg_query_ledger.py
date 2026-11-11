"""Synthetic query-ledger consistency tests, with no SDK/GPU execution."""
import json
import unittest

from fg_query_ledger import analyze, read_jsonl


def sample(ordinal=1, base=0, **changes):
    return dict(schema=1, session_id='synthetic-session', pid=123,
                process_creation_filetime=123456, viewport_id=0, owner_id='synthetic-display',
                display_epoch=0, runtime_tuple_sha256='a'*64, query_ordinal=ordinal,
                query_site='after-present', query_qpc=ordinal*100, base_present_total=base,
                dropped_total=0, all_queries_recorded=True, query_boundary_serialized=True,
                options_epoch=1, options_mode='on', options_generated=5, set_options_result=0,
                requested_total=6, get_state_result=0, status=0, sdk_presented=6, max_generated=5) | changes


def observations(*rows):
    return analyze(rows)['observations']


class FgQueryLedgerTests(unittest.TestCase):
    def test_first_query_is_only_baseline(self):
        first, = observations(sample(base=999))
        self.assertIsNone(first['sdk_interval_ratio'])
        self.assertIn('first-query-has-no-baseline', first['window_rejections'])

    def test_counter_is_current_interval_not_difference_of_raw_counts(self):
        _, row = observations(sample(sdk_presented=20), sample(2, 2, sdk_presented=12))
        self.assertEqual(row['sdk_interval_ratio'], 6)
        self.assertEqual(row['sdk_interval_fraction'], [12, 2])
        self.assertTrue(row['six_x_capability_reported'])
        self.assertTrue(row['six_x_options_accepted'])

    def test_zero_is_preserved_not_smoothed_to_old_six(self):
        row = observations(sample(), sample(2, 1), sample(3, 2, sdk_presented=0))[-1]
        self.assertEqual(row['sdk_interval_ratio'], 0)
        self.assertEqual(row['raw']['sdk_presented'], 0)

    def test_capability_refresh_is_a_recorded_boundary(self):
        rows = observations(sample(), sample(2, 1, query_site='capability-refresh', sdk_presented=2),
                            sample(3, 2, sdk_presented=4))
        self.assertEqual([r['sdk_interval_ratio'] for r in rows], [None, 2, 4])

    def test_skipped_query_resynchronizes_at_next_adjacent_clean_pair(self):
        rows = observations(sample(), sample(3, 2), sample(4, 3))
        self.assertIn('query-ordinal-gap', rows[1]['window_rejections'])
        self.assertIsNone(rows[1]['sdk_interval_ratio'])
        self.assertEqual(rows[2]['sdk_interval_ratio'], 6)

    def test_failure_invalidates_both_adjacent_windows(self):
        rows = observations(sample(), sample(2, 1, get_state_result=7), sample(3, 2), sample(4, 3))
        self.assertFalse(rows[1]['state_fields_valid'])
        self.assertIsNone(rows[1]['six_x_capability_reported'])
        self.assertIsNone(rows[1]['sdk_interval_ratio'])
        self.assertIn('previous-GetState-failed', rows[2]['window_rejections'])
        self.assertEqual(rows[3]['sdk_interval_ratio'], 6)

    def test_bad_status_does_not_certify_runtime_or_ratio(self):
        rows = observations(sample(), sample(2, 1, status=32), sample(3, 2))
        self.assertTrue(rows[1]['state_fields_valid'])
        self.assertIsNone(rows[1]['six_x_capability_reported'])
        self.assertIsNone(rows[1]['sdk_interval_ratio'])
        self.assertIn('previous-runtime-status-nonzero', rows[2]['window_rejections'])

    def test_options_change_away_and_back_is_caught_by_epoch(self):
        row = observations(sample(), sample(2, 1, options_epoch=3))[-1]
        self.assertIn('options-epoch-changed', row['window_rejections'])
        self.assertIsNone(row['sdk_interval_ratio'])

    def test_identical_successful_options_can_keep_epoch(self):
        row = observations(sample(), sample(2, 1))[-1]
        self.assertTrue(row['candidate_window_consistent'])

    def test_failed_options_not_accepted_even_if_requested_and_cap_six(self):
        row = observations(sample(), sample(2, 1, set_options_result=9))[-1]
        self.assertIsNone(row['accepted_options_generated'])
        self.assertFalse(row['six_x_options_accepted'])
        self.assertTrue(row['six_x_capability_reported'])
        self.assertIsNone(row['sdk_interval_ratio'])

    def test_fallback_request_and_cap_are_independent(self):
        row = observations(sample(options_generated=1, max_generated=1),
                           sample(2, 1, options_generated=1, max_generated=1, sdk_presented=2))[-1]
        self.assertEqual(row['raw']['requested_total'], 6)
        self.assertEqual(row['accepted_options_generated'], 1)
        self.assertFalse(row['six_x_capability_reported'])
        self.assertFalse(row['six_x_options_accepted'])
        self.assertEqual(row['sdk_interval_ratio'], 2)

    def test_off_mode_is_zero_generation_not_request_six(self):
        row = observations(sample(options_mode='off', options_generated=0),
                           sample(2, 1, options_mode='off', options_generated=0, sdk_presented=1))[-1]
        self.assertEqual(row['sdk_interval_ratio'], 1)
        self.assertFalse(row['six_x_options_accepted'])

    def test_counter_without_base_presents_is_not_multiplier(self):
        row = observations(sample(), sample(2, 0))[-1]
        self.assertIsNone(row['sdk_interval_ratio'])
        self.assertIn('no-successful-base-Presents-in-window', row['window_rejections'])

    def test_loss_invalidates_crossing_window_then_resync(self):
        rows = observations(sample(), sample(2, 1, dropped_total=1), sample(3, 2, dropped_total=1))
        self.assertIn('record-loss', rows[1]['window_rejections'])
        self.assertEqual(rows[2]['sdk_interval_ratio'], 6)

    def test_missing_coverage_or_serialization_refuses_adjacent_windows(self):
        for flag in ('all_queries_recorded', 'query_boundary_serialized'):
            rows = observations(sample(), sample(2, 1, **{flag: False}), sample(3, 2), sample(4, 3))
            self.assertIsNone(rows[1]['sdk_interval_ratio'])
            self.assertIsNone(rows[2]['sdk_interval_ratio'])
            self.assertEqual(rows[3]['sdk_interval_ratio'], 6)

    def test_identity_changes_require_fresh_baseline(self):
        for changes, reason in (({'owner_id': 'other'}, 'display-owner-changed'),
                                ({'display_epoch': 1}, 'display-epoch-changed'),
                                ({'runtime_tuple_sha256': 'b'*64}, 'runtime-tuple-changed'),
                                ({'viewport_id': 1}, 'viewport-changed'),
                                ({'options_generated': 1}, 'options-generated-changed'),
                                ({'max_generated': 6}, 'reported-max-changed')):
            rows = observations(sample(), sample(2, 1, **changes), sample(3, 2, **changes))
            self.assertIn(reason, rows[1]['window_rejections'])
            self.assertEqual(rows[2]['sdk_interval_ratio'], 6)

    def test_other_viewport_query_cannot_be_hidden_by_grouping(self):
        rows = observations(sample(), sample(2, 0, viewport_id=1), sample(3, 1), sample(4, 2))
        self.assertIn('viewport-changed', rows[2]['window_rejections'])
        self.assertIsNone(rows[2]['sdk_interval_ratio'])
        self.assertEqual(rows[3]['sdk_interval_ratio'], 6)

    def test_process_restart_requires_new_session(self):
        with self.assertRaises(ValueError):
            observations(sample(), sample(2, 1, process_creation_filetime=123457))
        rows = observations(sample(), sample(session_id='new-session', process_creation_filetime=123457))
        self.assertIsNone(rows[-1]['sdk_interval_ratio'])

    def test_interleaved_sessions_have_independent_baselines(self):
        rows = observations(sample(), sample(session_id='second'), sample(2, 1),
                            sample(2, 1, session_id='second'))
        self.assertEqual([r['sdk_interval_ratio'] for r in rows], [None, None, 6, 6])

    def test_duplicate_or_reordered_queries_fail_entire_input(self):
        for ordinal in (1, 2):
            with self.assertRaises(ValueError):
                observations(sample(2), sample(ordinal))

    def test_regressed_counters_fail_instead_of_rebaselining(self):
        for field in ('query_qpc', 'dropped_total', 'options_epoch', 'display_epoch', 'base_present_total'):
            with self.assertRaises(ValueError):
                observations(sample(**{field: 2}), sample(2, **{field: 1}))

    def test_base_regression_cannot_hide_behind_viewport_switch(self):
        with self.assertRaises(ValueError):
            observations(sample(1, 9), sample(2, 0, viewport_id=1), sample(3, 8))

    def test_accepted_above_cap_refuses_window(self):
        row = observations(sample(), sample(2, 1, max_generated=1))[-1]
        self.assertIn('accepted-generated-exceeds-reported-max', row['window_rejections'])
        self.assertIsNone(row['sdk_interval_ratio'])

    def test_large_present_counter_uses_exact_delta(self):
        n = 2**63
        row = observations(sample(base=n), sample(2, n+3, sdk_presented=18))[-1]
        self.assertEqual(row['base_presents_in_query_window'], 3)
        self.assertEqual(row['sdk_interval_fraction'], [18, 3])

    def test_unknown_options_allow_raw_reporting_only(self):
        row = observations(sample(), sample(2, 1, options_mode='unknown',
                                            options_generated=None, set_options_result=None))[-1]
        self.assertIsNone(row['accepted_options_generated'])
        self.assertIsNone(row['sdk_interval_ratio'])
        self.assertEqual(row['raw']['sdk_presented'], 6)

    def test_sdk_count_never_upgrades_display_or_retirement(self):
        report = analyze([sample(), sample(2, 1)])
        for name in ('provenance_authenticated', 'sustained_six_x_proven', 'inputs_retirement_proven',
                     'target_executed', 'gpu_tested'):
            self.assertIs(report[name], False)
        self.assertEqual(report['external_display_multiplier'], 'UNKNOWN')
        self.assertEqual(report['per_source_generation_count'], 'UNKNOWN')

    def test_empty_and_missing_extra_schema_fields_rejected(self):
        with self.assertRaises(ValueError):
            analyze([])
        for row in ([], {'schema': 1}, sample() | {'future': 1}):
            with self.assertRaises(ValueError):
                analyze([row])

    def test_strict_types_ranges_and_identifiers(self):
        for changes in ({'schema': True}, {'sdk_presented': True}, {'query_ordinal': 0},
                        {'get_state_result': -1}, {'query_qpc': 2**64}, {'max_generated': 1.0},
                        {'requested_total': 7}, {'all_queries_recorded': 1}, {'owner_id': ' '},
                        {'pid': 0}, {'runtime_tuple_sha256': 'z'*64}, {'session_id': 'x'*257},
                        {'options_generated': -1}, {'set_options_result': True}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                analyze([sample(**changes)])

    def test_accepted_configuration_must_be_coherent(self):
        for changes in ({'options_mode': 'unknown'}, {'options_mode': 'off'},
                        {'options_generated': None}, {'options_generated': 0}, {'options_mode': 'auto'}):
            with self.assertRaises(ValueError):
                analyze([sample(**changes)])

    def test_jsonl_preserves_order_and_rejects_duplicate_keys(self):
        raw = chr(10).join(json.dumps(row) for row in (sample(), sample(2, 1)))
        self.assertEqual(analyze(read_jsonl(raw))['consistent_candidate_windows'], 1)
        for raw in ('{"schema":1,"schema":1}', '{broken'):
            with self.assertRaises(ValueError):
                read_jsonl(raw)


if __name__ == '__main__':
    unittest.main(verbosity=2)
