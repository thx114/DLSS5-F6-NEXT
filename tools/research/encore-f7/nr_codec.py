'''Offline float64 reference for recovered NR color and residual math.

No SDK, GPU, camera authority, foveation or resource-lifetime implementation.
The reference uses shader float32 literals but does not reproduce rounding/FMA.
Proxy/neural inputs and intermediate math must be finite; float32 overflow is
outside this numeric reference, even when Python float64 could represent it.
'''
from __future__ import annotations

import math
import struct
from collections.abc import Sequence

RGB = tuple[float, float, float]


def literal(bits: int) -> float:
    return struct.unpack('<f', struct.pack('<I', bits))[0]


KNEE_EXPONENT = literal(0xc1b8aa3b)
EDGE_EXPONENT = literal(0xc29044fe)
EPSILON = literal(0x3a83126f)
EXPOSURE_FLOOR = literal(0x38d1b717)
LINEAR_SWITCH = literal(0x3b4d2e1c)
LINEAR_SCALE = literal(0x414eb852)
INVERSE_GAMMA = literal(0x3ed55555)
GAMMA_SCALE = literal(0x3f870a3d)
GAMMA_BIAS = literal(0x3d6147ae)
GAMMA_SWITCH = literal(0x3d25aee6)
INVERSE_LINEAR_SCALE = literal(0x3d9e8391)
INVERSE_GAMMA_SCALE = literal(0x3f72a76f)
GAMMA = literal(0x4019999a)
FLOAT32_MAX = literal(0x7f7fffff)


def finite(value: float) -> float:
    value = float(value)
    if not math.isfinite(value) or abs(value) > FLOAT32_MAX:
        raise ValueError('outside finite float32 reference domain')
    return value


def rgb(values: Sequence[float]) -> RGB:
    if len(values) != 3:
        raise ValueError('expected three color components')
    return tuple(finite(v) for v in values)


def clean_rgb(values: Sequence[float]) -> RGB:
    if len(values) != 3:
        raise ValueError('expected three color components')
    # The shader masks exponent-all-ones values to zero before using RGB.
    return tuple(0.0 if not math.isfinite(float(v)) else max(0.0, finite(v)) for v in values)


def saturate(value: float) -> float:
    return min(1.0, max(0.0, finite(value)))


def effective_exposure(base: float, sample: float | None = None) -> float:
    base = finite(base)
    if base <= 0:
        raise ValueError('base exposure must be positive')
    if sample is None or not math.isfinite(float(sample)) or sample <= 0:
        sample = 1.0
    return max(EXPOSURE_FLOOR, finite(base*finite(sample)))


def linear_to_encoded(value: float) -> float:
    value = saturate(value)
    return (value*LINEAR_SCALE if value <= LINEAR_SWITCH else
            GAMMA_SCALE*value**INVERSE_GAMMA-GAMMA_BIAS)


def encoded_to_linear(value: float) -> float:
    value = saturate(value)
    return (value*INVERSE_LINEAR_SCALE if value <= GAMMA_SWITCH else
            ((value+GAMMA_BIAS)*INVERSE_GAMMA_SCALE)**GAMMA)


def encode_color(values: Sequence[float], *, hdr: bool, exposure: float = 1.0) -> RGB:
    if type(hdr) is not bool:
        raise ValueError('hdr must be bool')
    color = clean_rgb(values)
    if not hdr:
        return tuple(saturate(c) for c in color)
    # The normal CPU path already clamps exposure; reject a different contract.
    exposure = finite(exposure)
    if exposure < EXPOSURE_FLOOR:
        raise ValueError('supply resolved exposure at least EXPOSURE_FLOOR')
    scaled = rgb([c*exposure for c in color])
    peak = max(scaled)
    if peak > 0.75:
        knee = 0.75+0.25*(1.0-math.exp2((peak-0.75)*KNEE_EXPONENT))
        scaled = tuple(c*knee/peak for c in scaled)
    return tuple(linear_to_encoded(c) for c in scaled)


def weighted_residual(center: Sequence[float], proxies: Sequence[Sequence[float]],
                      neural: Sequence[Sequence[float]], fraction: Sequence[float]) -> RGB:
    '''Four taps ordered (0,0), (1,0), (0,1), (1,1); no texture sampling.'''
    center = rgb(center)
    if len(proxies) != 4 or len(neural) != 4 or len(fraction) != 2:
        raise ValueError('expected four proxy/neural taps and two fractions')
    x, y = (finite(v) for v in fraction)
    if not 0 <= x <= 1 or not 0 <= y <= 1:
        raise ValueError('bilinear fractions outside [0,1]')
    base = ((1-x)*(1-y), x*(1-y), (1-x)*y, x*y)
    total = 0.0
    result = [0.0]*3
    for proxy, output, bilinear in zip(proxies, neural, base, strict=True):
        proxy, output = rgb(proxy), rgb(output)
        distance = finite(sum((p-c)**2 for p, c in zip(proxy, center, strict=True)))
        weight = bilinear*(math.exp2(distance*EDGE_EXPONENT)+EPSILON)
        total += weight
        for channel in range(3):
            result[channel] += finite(output[channel]-proxy[channel])*weight
    return rgb([v/total for v in result]) if total > 0 else (0.0, 0.0, 0.0)


def reconstruct_color(original: Sequence[float], residual: Sequence[float], *,
                      hdr: bool, exposure: float = 1.0) -> RGB:
    '''RGB only. A full shader keeps the original alpha, not neural alpha.'''
    original = clean_rgb(original)
    delta = rgb(residual)
    center = encode_color(original, hdr=hdr, exposure=exposure)
    enhanced = tuple(saturate(c+d) for c, d in zip(center, delta, strict=True))
    if not hdr:
        return enhanced
    before = tuple(encoded_to_linear(c) for c in center)
    after = tuple(encoded_to_linear(c) for c in enhanced)
    gains = [min(4.0, max(0.25, 1.0+(b-a)/max(a, EPSILON)))
             for a, b in zip(before, after, strict=True)]
    return rgb([c*g for c, g in zip(original, gains, strict=True)])


def box_axis(out_extent: int, work_extent: int, index: int) -> tuple[tuple[int, float], ...]:
    '''Non-foveated area-filter footprint on one relative output-rect axis.'''
    if any(type(v) is not int for v in (out_extent, work_extent, index)):
        raise ValueError('integer dimensions/index required')
    if not 1 <= out_extent <= 16384 or not 1 <= work_extent <= 16384 or not 0 <= index < work_extent:
        raise ValueError('invalid bounded reference raster')
    left, right = index*out_extent/work_extent, (index+1)*out_extent/work_extent
    return tuple((i, max(0.0, min(right, i+1)-max(left, i)))
                 for i in range(math.floor(left), min(out_extent, math.ceil(right))))
