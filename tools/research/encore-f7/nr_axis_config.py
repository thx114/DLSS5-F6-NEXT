"""Finite offline reference for Encore's native 24-byte NR axis writer.

RVA 14e6d0 plus percent/pair gates in 14efc0 and three verified presets.
No runtime config parsing, SDK, history permission, resources or GPU operations.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass, replace
import json
import struct

from nr_grid import _f32, native_round_int32
from nr_warp import Axis

PRESETS = (('light', 65, 65, 10, 65),
           ('balanced', 55, 55, 8, 50),
           ('strong', 40, 40, 8, 35))


def _uint(value: int, name: str) -> int:
    if type(value) is not int or not 0 <= value <= 0xffffffff:
        raise ValueError(name+' must be uint32')
    return value


@dataclass(frozen=True)
class NativeAxis:
    packed: int = 0
    gmax: float = 1.0
    center: float = 0.5
    inner_pixels: int = 0
    transition_pixels: int = 0
    boundary_derivative: float = 1.0

    def to_bytes(self) -> bytes:
        return struct.pack('<IffIIf', self.packed, self.gmax, self.center,
                           self.inner_pixels, self.transition_pixels, self.boundary_derivative)

    def shader_axis(self) -> Axis:
        # Guides/color use only these three fields, not all native 24 bytes.
        return Axis(self.packed, self.gmax, self.center)


@dataclass(frozen=True)
class NativePair:
    x: NativeAxis
    y: NativeAxis
    disabled_reason: str | None

    def to_bytes(self) -> bytes:
        return self.x.to_bytes()+self.y.to_bytes()


def axis_from_native_inputs(extent: int, inner_pixels: int, falloff_pixels: int,
                            edge_scale: float, center: float) -> NativeAxis:
    """14e6d0, bounded extent 1..16384 and finite float32 inputs.

    Preserves operation order and float32 Center/1-Center/Gmax/derivative
    storage. Does not emulate NaN selection, MXCSR or native errno behavior.
    """
    if not 1 <= _uint(extent, 'extent') <= 16384:
        raise ValueError('reference extent must be 1..16384')
    inner_pixels = min(extent, _uint(inner_pixels, 'inner_pixels'))
    falloff_pixels = min(extent, _uint(falloff_pixels, 'falloff_pixels'))
    raw_center = _f32(center)
    c = 0.0 if raw_center < 0.0 else 1.0 if raw_center > 1.0 else raw_center
    edge = _f32(edge_scale)
    result = NativeAxis(center=c, inner_pixels=inner_pixels)
    if extent < 16 or inner_pixels >= extent:
        return result
    qe = min(128, max(32, native_round_int32(min(1.0, max(0.25, edge))*128.0)))
    if qe > 127:
        return result
    half = extent*0.5
    qw = min(4095, max(0, native_round_int32((inner_pixels*0.5)/half*4095.0)))
    qf = min(4095, max(0, native_round_int32(falloff_pixels/half*2048.0)))
    packed = 0x80000000 | (qe << 24) | (qf << 12) | qw
    a, b, slope = qw/4095.0, qf/2048.0, qe/128.0
    k = 1.0-slope

    def curve(d: float) -> float:
        if d <= a:
            return d
        if d >= a+b:
            return a+b*(1.0-k/2.0)+slope*(d-a-b)
        t = (d-a)/b
        return a+b*(t-k*(t*t*t-0.5*t*t*t*t))

    other = _f32(1.0-c)
    gmax = curve(1.0) if c == 0.5 else (curve(2.0*c)+curve(2.0*other))*0.5
    far_radius = 2.0*max(float(other), float(c))
    remainder = far_radius-a
    transition = native_round_int32(half*min(b, max(0.0, remainder)))
    if b <= 0.0 or far_radius >= a+b:
        derivative = slope
    else:
        t = remainder/b
        derivative = 1.0-(3.0-2.0*t)*(t*k*t)
    return NativeAxis(packed, _f32(gmax), c, inner_pixels, transition, _f32(derivative))


def pair_from_percentages(*, enabled: bool, width_percent: int, height_percent: int,
                          falloff_percent: int, edge_percent: int,
                          center_x: float = 0.5, center_y: float = 0.5) -> NativePair:
    """Explicit preset==3 path ONLY, followed by native Gmax product gate.

    Disabled config returns defaults; product-gate disable changes only packed
    and Gmax, retaining the other axis fields just as the native stores do.
    """
    if type(enabled) is not bool:
        raise ValueError('enabled must be bool')
    width = min(100, _uint(width_percent, 'width_percent'))
    height = min(100, _uint(height_percent, 'height_percent'))
    falloff = min(100, _uint(falloff_percent, 'falloff_percent'))
    edge = min(100, max(25, _uint(edge_percent, 'edge_percent')))
    cx, cy = _f32(center_x), _f32(center_y)
    if not enabled:
        return NativePair(NativeAxis(), NativeAxis(), 'configuration-disabled')
    edge_scale = _f32(float(edge)/100.0)
    x = axis_from_native_inputs(10000, width*100, falloff*100, edge_scale, cx)
    y = axis_from_native_inputs(10000, height*100, falloff*100, edge_scale, cy)
    if not x.packed and not y.packed:
        return NativePair(x, y, 'both-axes-disabled')
    if float(x.gmax)*float(y.gmax) > 0.95:
        return NativePair(replace(x, packed=0, gmax=1.0), replace(y, packed=0, gmax=1.0),
                          'less-than-five-percent-grid-saving')
    return NativePair(x, y, None)


def pair_from_preset(preset: int, *, enabled: bool, center_x: float = 0.5,
                     center_y: float = 0.5) -> NativePair:
    """Known 0..2 entries only. Native selector does not prove other indices safe."""
    if type(preset) is not int or not 0 <= preset < len(PRESETS):
        raise ValueError('known table preset must be 0, 1 or 2; use explicit percentages for custom')
    _, width, height, falloff, edge = PRESETS[preset]
    return pair_from_percentages(enabled=enabled, width_percent=width, height_percent=height,
                                 falloff_percent=falloff, edge_percent=edge, center_x=center_x, center_y=center_y)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--preset', type=int, choices=(0, 1, 2))
    parser.add_argument('--width-percent', type=int)
    parser.add_argument('--height-percent', type=int)
    parser.add_argument('--falloff-percent', type=int)
    parser.add_argument('--edge-percent', type=int)
    parser.add_argument('--center-x', type=float, default=0.5)
    parser.add_argument('--center-y', type=float, default=0.5)
    parser.add_argument('--disabled', action='store_true')
    args = parser.parse_args()
    try:
        fields = (args.width_percent, args.height_percent, args.falloff_percent, args.edge_percent)
        if args.preset is not None:
            if any(value is not None for value in fields):
                raise ValueError('choose a preset OR four explicit percentage fields')
            pair = pair_from_preset(args.preset, enabled=not args.disabled,
                                    center_x=args.center_x, center_y=args.center_y)
        else:
            if any(value is None for value in fields):
                raise ValueError('four explicit percentage fields are required without --preset')
            pair = pair_from_percentages(enabled=not args.disabled, width_percent=args.width_percent,
                                         height_percent=args.height_percent, falloff_percent=args.falloff_percent,
                                         edge_percent=args.edge_percent, center_x=args.center_x, center_y=args.center_y)
        print(json.dumps(dict(axis_reference=asdict(pair), native_pair_hex=pair.to_bytes().hex(),
                              native_pair_bytes=48, preset_table_used=args.preset is not None,
                              target_executed=False, gpu_tested=False, runtime_config_authenticated=False),
                         indent=2, allow_nan=False))
    except (ValueError, OverflowError, struct.error) as error:
        parser.exit(2, str(error)+chr(10))


if __name__ == '__main__':
    main()
