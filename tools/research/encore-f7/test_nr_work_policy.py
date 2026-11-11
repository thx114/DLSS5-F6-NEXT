"""CPU-only tests of the bounded NR selection reference; no GPU claims."""

from dataclasses import FrozenInstanceError, replace
import unittest

from nr_work_policy import (
    OutputDesc,
    WorkPolicyState,
    WorkSelection,
    WorkSize,
    select_work_size,
)


UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1


class WorkPolicyTests(unittest.TestCase):
    def setUp(self):
        self.feature = WorkSize(1920, 1080)
        self.request = WorkSize(1280, 720)
        self.output = OutputDesc(3840, 2160, 28)

    def choose(self, **overrides):
        inputs = dict(
            requested_work=self.request,
            feature_work=self.feature,
            stored_output=self.output,
            current_output=self.output,
            change_key=1,
            state=WorkPolicyState(),
        )
        inputs.update(overrides)
        return select_work_size(**inputs)

    def pending(self, count=0, key=1):
        return WorkPolicyState(1280, 720, count, key)

    def test_initial_state_is_empty_with_zero_key(self):
        self.assertEqual(WorkPolicyState(), WorkPolicyState(0, 0, 0, 0))

    def test_default_state_is_supported(self):
        result = select_work_size(
            self.request, self.feature, self.output, self.output, 1
        )
        self.assertEqual(result.next_state, self.pending(1, 1))
        self.assertTrue(result.heldExisting)

    def test_no_feature_selects_request_and_preserves_key(self):
        for key in (0, 7, UINT64_MAX):
            with self.subTest(key=key):
                result = self.choose(
                    feature_work=None,
                    state=self.pending(UINT32_MAX, key),
                    change_key=12,
                )
                self.assertEqual(result.selected_work, self.request)
                self.assertEqual(result.next_state, WorkPolicyState(0, 0, 0, key))
                self.assertFalse(result.heldExisting)

    def test_initial_no_feature_and_initial_same_work(self):
        for feature in (None, self.request):
            with self.subTest(feature=feature):
                result = self.choose(feature_work=feature)
                self.assertEqual(result.selected_work, self.request)
                self.assertEqual(result.next_state, WorkPolicyState())
                self.assertFalse(result.heldExisting)

    def test_same_work_clears_candidate_and_count_only(self):
        result = self.choose(
            requested_work=WorkSize(1920, 1080),
            state=self.pending(27, 99),
            current_output=OutputDesc(1, 2, 3),
            change_key=100,
        )
        self.assertEqual(result.selected_work, self.feature)
        self.assertEqual(result.next_state, WorkPolicyState(0, 0, 0, 99))
        self.assertFalse(result.heldExisting)

    def test_first_different_candidate_changed_key_counts_once(self):
        result = self.choose()
        self.assertEqual(result.selected_work, self.feature)
        self.assertEqual(result.next_state, self.pending(1, 1))
        self.assertTrue(result.heldExisting)

    def test_first_candidate_same_key_can_stay_zero(self):
        result = self.choose(change_key=0)
        self.assertEqual(result.next_state, self.pending(0, 0))
        self.assertTrue(result.heldExisting)

    def test_count_fourteen_holds_with_unchanged_key(self):
        result = self.choose(state=self.pending(14))
        self.assertEqual(result.next_state, self.pending(14))
        self.assertEqual(result.selected_work, self.feature)
        self.assertTrue(result.heldExisting)

    def test_fourteen_to_fifteen_selects_request(self):
        result = self.choose(state=self.pending(14), change_key=2)
        self.assertEqual(result.next_state, self.pending(15, 2))
        self.assertEqual(result.selected_work, self.request)
        self.assertFalse(result.heldExisting)

    def test_full_sequence_holds_for_fourteen_changes(self):
        state = WorkPolicyState()
        for key in range(1, 17):
            with self.subTest(key=key):
                result = self.choose(state=state, change_key=key)
                self.assertEqual(result.next_state, self.pending(key, key))
                self.assertEqual(result.heldExisting, key < 15)
                self.assertEqual(
                    result.selected_work, self.feature if key < 15 else self.request
                )
                state = result.next_state

    def test_same_key_never_increments_count(self):
        for count in (0, 1, 14, 15, 16, UINT32_MAX):
            with self.subTest(count=count):
                state = self.pending(count)
                result = self.choose(state=state)
                self.assertEqual(result.next_state, state)
                self.assertEqual(result.heldExisting, count < 15)

    def test_count_comparison_is_unsigned(self):
        for count in (15, 16, 1 << 31, UINT32_MAX):
            with self.subTest(count=count):
                result = self.choose(state=self.pending(count))
                self.assertEqual(result.selected_work, self.request)
                self.assertFalse(result.heldExisting)

    def test_zero_key_can_increment_after_nonzero(self):
        result = self.choose(state=self.pending(4, 9), change_key=0)
        self.assertEqual(result.next_state, self.pending(5, 0))

    def test_keys_a_b_a_count_each_transition(self):
        state = self.pending(0, 0)
        for count, key in enumerate((8, 3, 8), start=1):
            result = self.choose(state=state, change_key=key)
            self.assertEqual(result.next_state, self.pending(count, key))
            state = result.next_state

    def test_uint64_key_maximum_is_preserved(self):
        result = self.choose(change_key=UINT64_MAX)
        self.assertEqual(result.next_state, self.pending(1, UINT64_MAX))
        repeated = self.choose(state=result.next_state, change_key=UINT64_MAX)
        self.assertEqual(repeated.next_state, result.next_state)

    def test_candidate_switch_same_key_resets_count_to_zero(self):
        result = self.choose(state=WorkPolicyState(960, 540, 25, 1))
        self.assertEqual(result.next_state, self.pending(0, 1))
        self.assertTrue(result.heldExisting)

    def test_candidate_switch_changed_key_resets_then_increments(self):
        result = self.choose(state=WorkPolicyState(960, 540, UINT32_MAX, 9))
        self.assertEqual(result.next_state, self.pending(1, 1))
        self.assertTrue(result.heldExisting)

    def test_each_candidate_axis_change_resets_count(self):
        for width, height in ((1281, 720), (1280, 721)):
            with self.subTest(width=width, height=height):
                result = self.choose(state=WorkPolicyState(width, height, 14, 1))
                self.assertEqual(result.next_state, self.pending(0, 1))

    def test_return_to_feature_preserves_key_for_next_candidate(self):
        returned = self.choose(
            requested_work=self.feature,
            state=self.pending(14, 9),
            change_key=123,
        )
        self.assertEqual(returned.next_state, WorkPolicyState(0, 0, 0, 9))
        resumed = self.choose(state=returned.next_state, change_key=9)
        self.assertEqual(resumed.next_state, self.pending(0, 9))
        self.assertTrue(resumed.heldExisting)

    def test_each_output_field_change_immediately_selects_request(self):
        changes = dict(width64=3841, height32=2161, format32=29)
        for field, value in changes.items():
            for count in (0, 14):
                with self.subTest(field=field, count=count):
                    state = self.pending(count)
                    result = self.choose(
                        state=state, current_output=replace(self.output, **{field: value})
                    )
                    self.assertEqual(result.selected_work, self.request)
                    self.assertEqual(result.next_state, state)
                    self.assertFalse(result.heldExisting)

    def test_output_mismatch_still_updates_candidate_and_key(self):
        result = self.choose(current_output=OutputDesc(1, 2, 3))
        self.assertEqual(result.selected_work, self.request)
        self.assertEqual(result.next_state, self.pending(1, 1))

    def test_output_width_high_bits_are_compared(self):
        result = self.choose(
            state=self.pending(),
            current_output=replace(self.output, width64=self.output.width64 + (1 << 32)),
        )
        self.assertEqual(result.selected_work, self.request)
        self.assertFalse(result.heldExisting)

    def test_output_full_unsigned_domain_compares_exactly(self):
        for desc in (OutputDesc(0, 0, 0), OutputDesc(UINT64_MAX, UINT32_MAX, UINT32_MAX)):
            with self.subTest(desc=desc):
                result = self.choose(stored_output=desc, current_output=desc)
                self.assertTrue(result.heldExisting)

    def test_count_wraps_to_zero_before_threshold_check(self):
        result = self.choose(state=self.pending(UINT32_MAX, 1), change_key=2)
        self.assertEqual(result.next_state, self.pending(0, 2))
        self.assertEqual(result.selected_work, self.feature)
        self.assertTrue(result.heldExisting)

    def test_wrapped_count_with_output_change_selects_request(self):
        result = self.choose(
            state=self.pending(UINT32_MAX, 1),
            change_key=2,
            current_output=replace(self.output, format32=0),
        )
        self.assertEqual(result.next_state, self.pending(0, 2))
        self.assertEqual(result.selected_work, self.request)

    def test_count_max_minus_one_increments_without_signed_conversion(self):
        result = self.choose(state=self.pending(UINT32_MAX - 1, 1), change_key=2)
        self.assertEqual(result.next_state, self.pending(UINT32_MAX, 2))
        self.assertFalse(result.heldExisting)

    def test_selection_does_not_commit_feature_or_clear_pending(self):
        selected = self.choose(state=self.pending(14, 1), change_key=2)
        self.assertEqual(selected.next_state, self.pending(15, 2))
        repeated = self.choose(state=selected.next_state, change_key=2)
        self.assertEqual(repeated.next_state, selected.next_state)
        self.assertEqual(repeated.selected_work, self.request)
        # Only an explicit caller-supplied feature change takes the clear branch.
        committed = self.choose(
            feature_work=self.request, state=repeated.next_state, change_key=3
        )
        self.assertEqual(committed.next_state, WorkPolicyState(0, 0, 0, 2))

    def test_output_selection_does_not_commit_output_description(self):
        selected = self.choose(current_output=replace(self.output, format32=0))
        held_again = self.choose(state=selected.next_state)
        self.assertEqual(held_again.selected_work, self.feature)
        self.assertTrue(held_again.heldExisting)

    def test_empty_pending_does_not_require_zero_count(self):
        state = WorkPolicyState(0, 0, UINT32_MAX, 1)
        self.assertEqual(self.choose(state=state).next_state, self.pending(0, 1))

    def test_inputs_are_unchanged_and_calls_are_repeatable(self):
        state = self.pending(14)
        before = (state, self.feature, self.request, self.output)
        first = self.choose(state=state, change_key=2)
        second = self.choose(state=state, change_key=2)
        self.assertEqual(first, second)
        self.assertEqual(before, (self.pending(14), WorkSize(1920, 1080),
                                  WorkSize(1280, 720), OutputDesc(3840, 2160, 28)))
        self.assertIsNot(first.next_state, state)

    def test_all_value_objects_and_results_are_frozen(self):
        result = self.choose()
        objects = (
            (self.feature, "width", 64),
            (self.output, "width64", 0),
            (result.next_state, "pendingCount", 0),
            (result, "heldExisting", False),
        )
        for obj, field, value in objects:
            with self.subTest(type=type(obj).__name__):
                with self.assertRaises(FrozenInstanceError):
                    setattr(obj, field, value)
        self.assertIsInstance(result, WorkSelection)
        self.assertIs(type(result.heldExisting), bool)

    def test_work_size_supports_inclusive_subset_boundaries(self):
        for width in (64, 16384):
            for height in (64, 16384):
                with self.subTest(width=width, height=height):
                    work = WorkSize(width, height)
                    result = self.choose(requested_work=work, feature_work=None)
                    self.assertEqual(result.selected_work, work)

    def test_work_size_rejects_values_outside_supported_subset(self):
        for field in ("width", "height"):
            for value in (-1, 0, 63, 16385, UINT32_MAX, UINT32_MAX + 1):
                with self.subTest(field=field, value=value):
                    args = dict(width=64, height=64)
                    args[field] = value
                    with self.assertRaises(ValueError):
                        WorkSize(**args)

    def test_pending_dimensions_reject_half_empty_or_outside_subset(self):
        for width, height in ((0, 64), (64, 0), (63, 64), (64, 16385),
                              (-1, 64), (UINT32_MAX + 1, 64)):
            with self.subTest(width=width, height=height):
                with self.assertRaises(ValueError):
                    WorkPolicyState(width, height)

    def test_unsigned_fields_reject_negative_and_overflow(self):
        fields = (
            (OutputDesc, dict(width64=0, height32=0, format32=0), "width64", UINT64_MAX),
            (OutputDesc, dict(width64=0, height32=0, format32=0), "height32", UINT32_MAX),
            (OutputDesc, dict(width64=0, height32=0, format32=0), "format32", UINT32_MAX),
            (WorkPolicyState, {}, "pendingCount", UINT32_MAX),
            (WorkPolicyState, {}, "lastChangeKey", UINT64_MAX),
        )
        for factory, defaults, field, maximum in fields:
            for value in (-1, maximum + 1):
                with self.subTest(factory=factory.__name__, field=field, value=value):
                    with self.assertRaises(ValueError):
                        factory(**(defaults | {field: value}))
        for key in (-1, UINT64_MAX + 1):
            with self.subTest(change_key=key):
                with self.assertRaises(ValueError):
                    self.choose(change_key=key)

    def test_every_numeric_input_rejects_bool_and_non_plain_int(self):
        class IntSubclass(int):
            pass

        fields = (
            (WorkSize, dict(width=64, height=64), "width"),
            (WorkSize, dict(width=64, height=64), "height"),
            (OutputDesc, dict(width64=0, height32=0, format32=0), "width64"),
            (OutputDesc, dict(width64=0, height32=0, format32=0), "height32"),
            (OutputDesc, dict(width64=0, height32=0, format32=0), "format32"),
            (WorkPolicyState, {}, "pendingWidth"),
            (WorkPolicyState, {}, "pendingHeight"),
            (WorkPolicyState, {}, "pendingCount"),
            (WorkPolicyState, {}, "lastChangeKey"),
        )
        for value in (True, False, 64.0, "64", None, IntSubclass(64)):
            for factory, defaults, field in fields:
                with self.subTest(factory=factory.__name__, field=field, value=value):
                    with self.assertRaises(TypeError):
                        factory(**(defaults | {field: value}))
            with self.subTest(change_key=value):
                with self.assertRaises(TypeError):
                    self.choose(change_key=value)

    def test_function_rejects_wrong_structured_argument_types(self):
        invalid = dict(
            requested_work=(1280, 720),
            feature_work=(1920, 1080),
            stored_output=(3840, 2160, 28),
            current_output=(3840, 2160, 28),
            state={},
        )
        for field, value in invalid.items():
            for bad in (value, True, False, None):
                if field == "feature_work" and bad is None:
                    continue
                with self.subTest(field=field, bad=bad):
                    with self.assertRaises(TypeError):
                        self.choose(**{field: bad})

    def test_early_branches_still_reject_invalid_arguments(self):
        for feature in (None, self.request):
            for invalid in (dict(change_key=True), dict(change_key=-1),
                            dict(current_output=None), dict(stored_output=False),
                            dict(state=None)):
                with self.subTest(feature=feature, invalid=invalid):
                    with self.assertRaises((TypeError, ValueError)):
                        self.choose(feature_work=feature, **invalid)
