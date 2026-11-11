"""Offline float64 reference for Encore's separable NR warp and guide geometry.

Follows four shader Newton steps, not an exact analytic inverse. No float32/FMA
parity, camera authority, SDK, resources, fences, or runtime parameter generator.
Accepted packed parameters are a safe mathematical subset, NOT a claim about
which configurations the native CPU writer actually emits.
"""
from __future__ import annotations

from dataclasses import dataclass
import math
import struct
from collections.abc import Sequence

from nr_codec import finite, literal

AXIS_A_SCALE = literal(0x39800801)


def sign(value: float) -> int:
    return (value > 0) - (value < 0)


def raster(extent: int, index: int | None = None) -> None:
    if type(extent) is not int or not 1 <= extent <= 16384:
        raise ValueError('expected nonzero bounded reference extent')
    if index is not None and (type(index) is not int or not 0 <= index < extent):
        raise ValueError('index outside reference raster')


@dataclass(frozen=True)
class Axis:
    packed: int = 0
    gmax: float = 1.0
    center: float = 0.5

    def __post_init__(self) -> None:
        if type(self.packed) is not int or not 0 <= self.packed <= 0xffffffff:
            raise ValueError('packed axis must be uint32')
        if finite(self.gmax) <= 0 or not 0 <= finite(self.center) <= 1:
            raise ValueError('positive finite Gmax and normalized center required')
        if self.enabled and self.slope == 0:
            raise ValueError('zero slope outside supported reference domain')

    @property
    def enabled(self) -> bool:
        return bool(self.packed & 0x80000000)

    @property
    def a(self) -> float:
        return (self.packed & 0xfff) * AXIS_A_SCALE

    @property
    def b(self) -> float:
        return ((self.packed >> 12) & 0xfff) / 2048

    @property
    def slope(self) -> float:
        return ((self.packed >> 24) & 0x7f) / 128

    def curve(self, radius: float) -> float:
        radius = finite(radius)
        if radius < 0:
            raise ValueError('radius must be nonnegative')
        if not self.enabled or radius <= self.a:
            return radius
        k = 1 - self.slope
        if radius >= self.a + self.b:
            return finite(self.a + self.b*(1-k/2) + self.slope*(radius-self.a-self.b))
        t = (radius-self.a)/self.b
        return finite(self.a + self.b*(t-k*(t*t*t-0.5*t*t*t*t)))

    def inverse_curve_shader(self, radius: float) -> float:
        radius = finite(radius)
        if radius < 0:
            raise ValueError('radius must be nonnegative')
        if not self.enabled or radius <= self.a:
            return radius
        k = 1 - self.slope
        edge = self.a + self.b*(1-k/2)
        if radius >= edge:
            return finite(self.a + self.b + (radius-edge)/self.slope)
        q = (radius-self.a)/self.b
        t = q
        for _ in range(4):
            f = t-k*(t*t*t-0.5*t*t*t*t)-q
            derivative = 1-k*(3*t*t-2*t*t*t)
            t = min(1.0, max(0.0, t-f/derivative))
        return finite(self.a + self.b*t)

    def forward(self, u: float) -> float:
        """Original normalized coordinate -> work; no clamp at image edges."""
        u = finite(u)
        if not self.enabled:
            return u
        if self.center == 0.5:
            d = finite(2*u-1)
            return finite(0.5 + 0.5*sign(d)*self.curve(abs(d))/self.gmax)
        d = finite(u-self.center)
        return finite((self.curve(2*self.center) + sign(d)*self.curve(abs(2*d)))/(2*self.gmax))

    def inverse_shader(self, v: float) -> float:
        """Work -> original using shader's finite four-step inverse."""
        v = finite(v)
        if not self.enabled:
            return v
        if self.center == 0.5:
            d = finite(2*v-1)
            return finite(0.5 + 0.5*sign(d)*self.inverse_curve_shader(finite(abs(d)*self.gmax)))
        d = finite(2*v*self.gmax - self.curve(2*self.center))
        return finite(self.center + 0.5*sign(d)*self.inverse_curve_shader(abs(d)))


def native_axis_from_quantized(packed: int, center: float) -> Axis:
    """CPU Gmax for supplied quantized fields, NOT the full config writer.

    Native scalar CPU code divides qw by double 4095. Shader uses the rounded
    float32 reciprocal AXIS_A_SCALE. Preserve the CPU float32 Center, 1-Center
    subtraction and final Gmax storage. Quantization/extent optimization omitted.
    """
    value = finite(center)
    value = 0.0 if value < 0.0 else 1.0 if value > 1.0 else value
    c = struct.unpack('<f', struct.pack('<f', value))[0]
    axis = Axis(packed, 1.0, c)
    if not axis.enabled:
        return axis
    if ((packed >> 24) & 0x7f) < 32:
        raise ValueError('outside observed native successful edge-code range')
    a, b, slope = (packed & 0xfff)/4095, axis.b, axis.slope
    k = 1-slope

    def cpu_curve(d: float) -> float:
        if d <= a:
            return d
        if d >= a+b:
            return a+b*(1-k/2)+slope*(d-a-b)
        t = (d-a)/b
        return a+b*(t-k*(t*t*t-0.5*t*t*t*t))

    other = struct.unpack('<f', struct.pack('<f', 1.0-c))[0]
    gmax = cpu_curve(1.0) if c == 0.5 else (cpu_curve(2*c)+cpu_curve(2*other))/2
    gmax = struct.unpack('<f', struct.pack('<f', gmax))[0]
    return Axis(packed, gmax, c)


def codec_coordinate(value: float, out_extent: int, work_extent: int,
                     axis: Axis, *, inverse: bool = False) -> float:
    """Color shaders bypass the axis if its two extents match."""
    raster(out_extent)
    raster(work_extent)
    value = finite(value)
    if out_extent == work_extent:
        return value
    return axis.inverse_shader(value) if inverse else axis.forward(value)


def encode_box_axis(out_extent: int, work_extent: int, index: int,
                    axis: Axis = Axis()) -> tuple[tuple[int, float], ...]:
    """Relative source texels and overlap lengths for one encode footprint."""
    raster(work_extent, index)
    low = max(0.0, codec_coordinate(index/work_extent, out_extent, work_extent, axis, inverse=True)*out_extent)
    high = min(float(out_extent), codec_coordinate((index+1)/work_extent, out_extent, work_extent, axis, inverse=True)*out_extent)
    if high <= low:
        return ()
    return tuple((i, max(0.0, min(high, i+1)-max(low, i)))
                 for i in range(math.floor(low), min(out_extent, math.ceil(high))))


def decode_axis(out_extent: int, work_extent: int, index: int,
                axis: Axis = Axis()) -> tuple[int, int, float]:
    """Two clamped work indices and interpolation fraction for color decode."""
    raster(out_extent, index)
    position = codec_coordinate((index+0.5)/out_extent, out_extent, work_extent, axis)*work_extent-0.5
    base = math.floor(position)
    return max(0, min(work_extent-1, base)), max(0, min(work_extent-1, base+1)), position-base


def pair(values: Sequence[float]) -> tuple[float, float]:
    if len(values) != 2:
        raise ValueError('expected two components')
    return finite(values[0]), finite(values[1])


def guide_position(pixel: Sequence[int], work_size: Sequence[int],
                   axes: Sequence[Axis]) -> tuple[float, float]:
    if len(pixel) != 2 or len(work_size) != 2 or len(axes) != 2:
        raise ValueError('expected two axes')
    for extent, index in zip(work_size, pixel, strict=True):
        raster(extent, index)
    # Guides obey packed enable bits even if a color extent happens to match.
    return tuple(a.inverse_shader((i+0.5)/w) for a, i, w in zip(axes, pixel, work_size, strict=True))


def nearest_address(position: Sequence[float], size: Sequence[int],
                    base: Sequence[int] = (0, 0)) -> tuple[int, int]:
    position = pair(position)
    if len(size) != 2 or len(base) != 2:
        raise ValueError('expected two raster axes')
    result = []
    for u, extent, offset in zip(position, size, base, strict=True):
        raster(extent)
        if type(offset) is not int or not 0 <= offset <= 16383 or offset+extent > 16384:
            raise ValueError('invalid bounded source rectangle')
        result.append(offset + min(extent-1, math.floor(min(1.0, max(0.0, u))*extent)))
    return tuple(result)


def guide_motion(pixel: Sequence[int], work_size: Sequence[int], axes: Sequence[Axis],
                 motion: Sequence[float], uv_scale: Sequence[float]) -> tuple[float, float]:
    """Raw sampled MV -> work-pixel displacement; retain inverse residual."""
    position = guide_position(pixel, work_size, axes)
    motion, uv_scale = pair(motion), pair(uv_scale)
    return tuple(finite((a.forward(finite(p+m*s))-(i+0.5)/w)*w)
                 for a, p, m, s, i, w in zip(axes, position, motion, uv_scale, pixel, work_size, strict=True))
