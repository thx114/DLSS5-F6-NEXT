"""Pure reference for Encore NR work-size selection, not feature admission.

Source: fn-00156ac0.txt L328..369, instructions 0x15706d..0x157137.
Only the selection block is modeled. Work dimensions are restricted to the
already-admitted 64..16384 subset; successful selection does not establish full
admission or actual GPU-time savings. No feature creation, descriptor update,
resource lifetime, FG, or completion behavior is modeled.
"""

from dataclasses import dataclass, replace


_UINT32_MAX = (1 << 32) - 1
_UINT64_MAX = (1 << 64) - 1


def _uint(name: str, value: int, maximum: int) -> None:
    """Require a plain Python int in range; bool and coercion are rejected."""
    if type(value) is not int:
        raise TypeError(f"{name} must be a plain int (bool is not accepted)")
    if not 0 <= value <= maximum:
        raise ValueError(f"{name} must be in 0..{maximum}")


def _work_axis(name: str, value: int) -> None:
    _uint(name, value, _UINT32_MAX)
    if not 64 <= value <= 16384:
        raise ValueError(f"{name} must be in the admitted subset 64..16384")


@dataclass(frozen=True, slots=True)
class WorkSize:
    """Already-admitted work dimensions in the supported 64..16384 subset."""

    width: int
    height: int

    def __post_init__(self) -> None:
        _work_axis("width", self.width)
        _work_axis("height", self.height)


@dataclass(frozen=True, slots=True)
class OutputDesc:
    """Raw texture fields: uint64 width, uint32 height, uint32 format.

    Zero and the full unsigned maxima are representable. These fields are
    compared exactly; they are not normalized, truncated, or texture-validated.
    """

    width64: int
    height32: int
    format32: int

    def __post_init__(self) -> None:
        _uint("width64", self.width64, _UINT64_MAX)
        _uint("height32", self.height32, _UINT32_MAX)
        _uint("format32", self.format32, _UINT32_MAX)


@dataclass(frozen=True, slots=True)
class WorkPolicyState:
    """Immutable pending dimensions, uint32 count, and opaque uint64 key.

    Pending dimensions are either (0, 0), meaning empty, or a WorkSize in the
    supported subset. Count and key can hold any value of their unsigned types;
    construction imposes no additional relationship between them and pending.
    The key's producer semantics are unknown; it is not a source or frame ID.
    """

    pendingWidth: int = 0
    pendingHeight: int = 0
    pendingCount: int = 0
    lastChangeKey: int = 0

    def __post_init__(self) -> None:
        _uint("pendingWidth", self.pendingWidth, _UINT32_MAX)
        _uint("pendingHeight", self.pendingHeight, _UINT32_MAX)
        if (self.pendingWidth, self.pendingHeight) != (0, 0):
            _work_axis("pendingWidth", self.pendingWidth)
            _work_axis("pendingHeight", self.pendingHeight)
        _uint("pendingCount", self.pendingCount, _UINT32_MAX)
        _uint("lastChangeKey", self.lastChangeKey, _UINT64_MAX)


@dataclass(frozen=True, slots=True)
class WorkSelection:
    """Selection and next pending state, without committing any feature.

    heldExisting is true only when a differing request is held at feature_work
    by the below-threshold, equal-output-descriptor branch.
    """

    selected_work: WorkSize
    next_state: WorkPolicyState
    heldExisting: bool


def select_work_size(
    requested_work: WorkSize,
    feature_work: WorkSize | None,
    stored_output: OutputDesc,
    current_output: OutputDesc,
    change_key: int,
    state: WorkPolicyState = WorkPolicyState(),
) -> WorkSelection:
    """Select work dimensions and return immutable next state.

    All inputs are validated even on an early-return branch. Numeric inputs
    require plain ints, never bool, floats, or coercible values. change_key is
    opaque uint64: zero and reused/nonmonotonic keys are supported. Each change
    from lastChangeKey increments the uint32 count modulo 2**32.

    With no feature, or a request equal to feature_work, clear pending dimensions
    and count while retaining lastChangeKey. Otherwise a candidate switch resets
    count, then a changed key increments it. A resulting unsigned count >= 15
    selects the request. Below 15, hold the feature only if all three raw output
    fields match. Selection never updates feature_work or either output, and
    never clears pending state merely because a request was selected.
    """
    if not isinstance(requested_work, WorkSize):
        raise TypeError("requested_work must be a WorkSize")
    if feature_work is not None and not isinstance(feature_work, WorkSize):
        raise TypeError("feature_work must be a WorkSize or None")
    if not isinstance(stored_output, OutputDesc):
        raise TypeError("stored_output must be an OutputDesc")
    if not isinstance(current_output, OutputDesc):
        raise TypeError("current_output must be an OutputDesc")
    if not isinstance(state, WorkPolicyState):
        raise TypeError("state must be a WorkPolicyState")
    _uint("change_key", change_key, _UINT64_MAX)

    if feature_work is None or requested_work == feature_work:
        return WorkSelection(
            requested_work,
            replace(state, pendingWidth=0, pendingHeight=0, pendingCount=0),
            False,
        )

    count = state.pendingCount
    if (state.pendingWidth, state.pendingHeight) != (
        requested_work.width,
        requested_work.height,
    ):
        count = 0
    key = state.lastChangeKey
    if change_key != key:
        key = change_key
        count = (count + 1) & _UINT32_MAX

    next_state = WorkPolicyState(
        pendingWidth=requested_work.width,
        pendingHeight=requested_work.height,
        pendingCount=count,
        lastChangeKey=key,
    )
    equal_output = (
        stored_output.width64 == current_output.width64
        and stored_output.height32 == current_output.height32
        and stored_output.format32 == current_output.format32
    )
    held = count < 15 and equal_output
    return WorkSelection(feature_work if held else requested_work, next_state, held)
