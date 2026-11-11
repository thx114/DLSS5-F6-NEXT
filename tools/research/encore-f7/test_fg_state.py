"""Synthetic layout and evidence-boundary tests, not SDK/hardware tests."""
import struct
import unittest

from fg_state import STATE_GUID, assess, decode_state


def sample(count=6, maximum=5, status=0):
    raw = bytearray(88)
    raw[8:24] = STATE_GUID
    struct.pack_into('<Q', raw, 24, 4)
    struct.pack_into('<Q', raw, 0x20, 123456)
    struct.pack_into('<IIII', raw, 0x28, status, 128, count, maximum)
    struct.pack_into('<QQ', raw, 0x40, 0x12345678, 99)
    raw[0x39], raw[0x50] = 1, 2
    return bytes(raw)


class FgStateTests(unittest.TestCase):
    def test_fixed_layout_does_not_read_neighboring_fields_as_counts(self):
        result = decode_state(sample())
        self.assertEqual(result['estimated_vram_bytes'], 123456)
        self.assertEqual(result['sdk_presented_since_previous_GetState'], 6)
        self.assertEqual(result['max_generated'], 5)
        self.assertEqual(result['inputs_fence_pointer'], '0x12345678')
        self.assertEqual(result['inputs_fence_value'], 99)
        self.assertTrue(result['vsync_supported'])
        self.assertIsNone(result['dynamic_mfg_supported'])
        self.assertFalse(result['inputs_retirement_proven'])

    def test_wrong_guid_version_and_truncation_are_rejected(self):
        raw = sample()
        for wrong in (raw[:-1], raw+bytes(8), bytes(88), raw[:24]+bytes(8)+raw[32:]):
            with self.assertRaises(ValueError):
                decode_state(wrong)

    def test_six_request_with_x2_capability_and_accepted_fallback(self):
        result = assess(sample(2, 1), enabled=True, requested_total=6,
                        get_state_result=0, configured_generated=1, set_options_result=0)
        self.assertFalse(result['requested_within_reported_capability'])
        self.assertFalse(result['six_x_capability_reported'])
        self.assertEqual(result['accepted_options_generated'], 1)
        self.assertFalse(result['six_x_options_accepted'])

    def test_six_counter_and_accepted_options_still_not_scanout_proof(self):
        result = assess(sample(), enabled=True, requested_total=6, get_state_result=0,
                        configured_generated=5, set_options_result=0, base_presents=1,
                        exclusive_query_window=True)
        self.assertTrue(result['six_x_capability_reported'])
        self.assertTrue(result['six_x_options_accepted'])
        self.assertEqual(result['sdk_interval_ratio'], 6)
        self.assertFalse(result['sustained_six_x_proven'])
        self.assertEqual(result['per_source_generation_count'], 'UNKNOWN')

    def test_interval_counter_is_not_per_frame_or_cumulative(self):
        result = assess(sample(12, 5), enabled=True, requested_total=6, get_state_result=0,
                        base_presents=2, exclusive_query_window=True)
        self.assertEqual(result['sdk_interval_ratio'], 6)
        other_reader = assess(sample(0, 5), enabled=True, requested_total=6, get_state_result=0,
                              base_presents=2, exclusive_query_window=False)
        self.assertIsNone(other_reader['sdk_interval_ratio'])
        self.assertTrue(other_reader['warnings'])

    def test_zero_counter_does_not_retain_last_high_count(self):
        high = assess(sample(6), enabled=True, requested_total=6, get_state_result=0,
                      base_presents=1, exclusive_query_window=True)
        low = assess(sample(0), enabled=True, requested_total=6, get_state_result=0,
                     base_presents=1, exclusive_query_window=True)
        self.assertEqual(high['sdk_interval_ratio'], 6)
        self.assertEqual(low['sdk_interval_ratio'], 0)

    def test_failed_calls_and_bad_status_do_not_authenticate_fields(self):
        failed = assess(sample(), enabled=True, requested_total=6, get_state_result=3,
                        configured_generated=5, set_options_result=7, base_presents=1,
                        exclusive_query_window=True)
        self.assertIsNone(failed['six_x_capability_reported'])
        self.assertIsNone(failed['accepted_options_generated'])
        self.assertIsNone(failed['sdk_interval_ratio'])
        bad = assess(sample(status=8 | 32), enabled=True, requested_total=6, get_state_result=0,
                     base_presents=1, exclusive_query_window=True)
        self.assertEqual(bad['state']['unknown_or_reserved_status_bits'], 32)
        self.assertIn('common-constants-invalid', bad['state']['status_flags'])
        self.assertIsNone(bad['sdk_interval_ratio'])

    def test_off_and_no_query_window(self):
        result = assess(sample(6), enabled=False, requested_total=6, get_state_result=0,
                        configured_generated=5, set_options_result=0, base_presents=0)
        self.assertEqual(result['request']['generated'], 0)
        self.assertIsNone(result['sdk_interval_ratio'])
        self.assertFalse(result['six_x_options_accepted'])
        self.assertTrue(result['warnings'])

    def test_invalid_observation_values(self):
        common = dict(enabled=True, requested_total=6, get_state_result=0)
        for key, value in (('requested_total', 7), ('requested_total', True),
                           ('get_state_result', -1), ('configured_generated', -1),
                           ('base_presents', -1), ('enabled', 1)):
            with self.assertRaises(ValueError):
                assess(sample(), **(common | {key: value}))


if __name__ == '__main__':
    unittest.main(verbosity=2)
