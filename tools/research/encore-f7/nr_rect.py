"""Pure CPU reference of fixed CDF NR helper RVA 0x17a20..0x17b12.

Source pin: SHA256
cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed.
This identifies the studied source; this module does not verify source bytes.

The input descriptor is 24 bytes: resource pointer at +0, uint32 x/y at
+8/+12, requested width/height at +16/+20. Output is six uint32 values:
x, y, width, height, texture_width, texture_height. The native helper clears
all output first, then queries the backend vtable +0x190 through CFG. That
method's identity is unknown; only query_result == 0 continues.

Dimensions supplied here model raw query output, not authenticated resources.
Failures preserve writes already performed. Zero requested extents mean full
texture extents, even at nonzero origins; there is no clipping or outer Encore
clamp. Diagnostics are synthetic labels, not raw SDK return codes.

This is not a drop-in F7 implementation or proof of texture ownership, format
validation, hardware support, or GPU completion. It performs no target calls.
"""

from dataclasses import dataclass


_UINT32_MAX = (1 << 32) - 1


def _uint32(name: str, value: int) -> None:
    if type(value) is not int:
        raise TypeError(f"{name} must be an exact int, not bool or another type")
    if not 0 <= value <= _UINT32_MAX:
        raise ValueError(f"{name} must be in uint32 range")


@dataclass(frozen=True, slots=True)
class DescriptorRect:
    """Four uint32 fields at descriptor +8..+23, excluding the resource pointer."""

    x: int
    y: int
    width: int
    height: int

    def __post_init__(self) -> None:
        for name in ("x", "y", "width", "height"):
            _uint32(name, getattr(self, name))


@dataclass(frozen=True, slots=True)
class ResolvedRect:
    """Output in native field order, including writes retained on false returns."""

    x: int
    y: int
    width: int
    height: int
    texture_width: int
    texture_height: int

    def __post_init__(self) -> None:
        for name in ("x", "y", "width", "height", "texture_width", "texture_height"):
            _uint32(name, getattr(self, name))


@dataclass(frozen=True, slots=True)
class Resolution:
    """Boolean helper outcome, observable output, and a synthetic diagnostic."""

    accepted: bool
    output: ResolvedRect
    diagnostic: str


def resolve_rect(
    request: DescriptorRect,
    texture_width: int,
    texture_height: int,
    *,
    backend_present: bool = True,
    resource_present: bool = True,
    query_result: int = 0,
) -> Resolution:
    """Model normal returns for valid, nonaliasing input/output storage.

    All host inputs are validated before evaluating native branches. Bad host
    types raise TypeError; integers outside uint32 raise ValueError, even when
    a native early return would ignore that argument. Presence flags must be
    exact bools. Pointer faults, aliasing, query exceptions and indirect method
    internals are outside this reference.
    """
    if not isinstance(request, DescriptorRect):
        raise TypeError("request must be a DescriptorRect")
    _uint32("texture_width", texture_width)
    _uint32("texture_height", texture_height)
    _uint32("query_result", query_result)
    for name, flag in (("backend_present", backend_present),
                       ("resource_present", resource_present)):
        if type(flag) is not bool:
            raise TypeError(f"{name} must be an exact bool")

    output = ResolvedRect(0, 0, 0, 0, 0, 0)
    if not backend_present:
        return Resolution(False, output, "backend-absent")
    if not resource_present:
        return Resolution(False, output, "resource-absent")
    if query_result != 0:
        return Resolution(False, output, "query-nonzero")

    # Both queried dimensions are published before either zero check.
    output = ResolvedRect(0, 0, 0, 0, texture_width, texture_height)
    if texture_width == 0 or texture_height == 0:
        return Resolution(False, output, "texture-extent-zero")

    width = request.width if request.width != 0 else texture_width
    height = request.height if request.height != 0 else texture_height
    output = ResolvedRect(request.x, request.y, width, height,
                          texture_width, texture_height)
    # Explicit in the native helper, though unreachable with the validated
    # uint32 inputs and nonzero texture dimensions above.
    if width == 0 or height == 0:
        return Resolution(False, output, "extent-zero")
    if request.x > texture_width or request.y > texture_height:
        return Resolution(False, output, "origin-out-of-bounds")
    # Subtract only after origin bounds: never emulate wrapped x + width.
    if width > texture_width - request.x or height > texture_height - request.y:
        return Resolution(False, output, "extent-out-of-bounds")
    return Resolution(True, output, "accepted")
