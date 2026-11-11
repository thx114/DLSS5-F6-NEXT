"""Offline reference for bounded Encore NR grid arithmetic, not a GPU cost model.

Integer score/selection: RVA 14b4b0 / 14f320. Finite int32 rounding:
4ffe60 -> 504060. Work-size stages: 15d660 / 156f12..156f99.
No provider calls, allocation prediction, runtime config decoding or timing.
"""
from __future__ import annotations

from dataclasses import asdict, dataclass
import argparse
import json
import math
import struct

from nr_warp import Axis


def _uint(value: int, name: str) -> int:
    if type(value) is not int or not 0 <= value <= 0xffffffff:
        raise ValueError(name+' must be uint32')
    return value


def _extent(value: int) -> int:
    if not 1 <= _uint(value, 'extent') <= 16384:
        raise ValueError('reference extent must be 1..16384')
    return value


def _f32(value: float) -> float:
    if type(value) not in (int, float):
        raise ValueError('numeric value required')
    try:
        result = struct.unpack('<f', struct.pack('<f', value))[0]
    except (OverflowError, struct.error) as error:
        raise ValueError('value outside finite float32') from error
    if not math.isfinite(result):
        raise ValueError('value outside finite float32')
    return result


def native_round_int32(value: float) -> int:
    """Finite, in-range subset: nearest with halfway cases away from zero.

    Rejects exceptional/out-of-range values instead of imitating native errno,
    floating exception or fallback-zero behavior. Does not use Python round.
    """
    if type(value) not in (int, float):
        raise ValueError('numeric value required')
    if value <= -(2**31)-0.5 or value >= 2**31-0.5:
        raise ValueError('rounded result outside signed32')
    if not math.isfinite(value):
        raise ValueError('finite numeric value required')
    fraction, whole = math.modf(abs(value))
    magnitude = int(whole)+(fraction >= 0.5)
    result = -magnitude if value < 0 else magnitude
    if not -(2**31) <= result < 2**31:
        raise ValueError('rounded result outside signed32')
    return result


def _level_count(extent: int) -> int:
    # First reduction is ceil(n/2), followed by five floor(n/2) reductions.
    first = (((extent+1)//2)+3) & ~3
    count = int(first < extent)+int(bool(first & 4))
    current = first
    for _ in range(5):
        following = ((current//2)+3) & ~3
        count += following < current
        current = following
    return count


@dataclass(frozen=True)
class GridCost:
    levels: tuple[int, int]
    steps: tuple[int, int]
    padded_extent: tuple[int, int]
    extra_width: int
    score: int


def grid_cost(width: int, height: int) -> GridCost:
    """Exact integer-score formula on the optimizer's 64..16384 domain.

    Names describe arithmetic only, not recovered provider allocations.
    This score is neither milliseconds nor bytes nor shader invocations.
    """
    if not 64 <= _extent(width) <= 16384 or not 64 <= _extent(height) <= 16384:
        raise ValueError('grid score domain is 64..16384 per dimension')
    kx, ky = _level_count(width), _level_count(height)
    sx, sy = 1 << kx, 1 << ky
    px = max(320, ((width+sx-1)//sx)*sx)
    py = max(320, ((height+sy-1)//sy)*sy)
    extra = sx if px % (4*sx) == 0 and py % (4*sy) == 0 else 0
    return GridCost((kx, ky), (sx, sy), (px, py), extra, (px+extra)*py)


def optimize_grid(width: int, height: int, reference_width: int = 0,
                  reference_height: int = 0) -> tuple[int, int]:
    """14f320 bounded candidate search, preserving native ties and fallback.

    Invalid-sized uint32 input returns unchanged, as the observed early exit.
    If either reference dimension is zero, both default to input dimensions.
    """
    for name, value in (('width', width), ('height', height),
                        ('reference_width', reference_width), ('reference_height', reference_height)):
        _uint(value, name)
    if not 64 <= width <= 16384 or not 64 <= height <= 16384:
        return width, height
    if not reference_width or not reference_height:
        reference_width, reference_height = width, height
    elif reference_width > 16384 or reference_height > 16384:
        raise ValueError('explicit reference dimensions outside bounded 1..16384 domain')
    best, best_score, best_area = (width, height), grid_cost(width, height).score, width*height
    min_width, min_height = max(64, (98*width+99)//100), max(64, (98*height+99)//100)
    for candidate_width in range(min_width, width+1):
        quotient, remainder = divmod(candidate_width*reference_height, reference_width)
        for candidate_height in (quotient, quotient+bool(remainder)):
            if not min_height <= candidate_height <= height:
                continue
            target = reference_width*candidate_height
            error = abs(candidate_width*reference_height-target)
            if 400*error > target:
                continue
            score = grid_cost(candidate_width, candidate_height).score
            area = candidate_width*candidate_height
            if score < best_score or (score == best_score and area > best_area):
                best, best_score, best_area = (candidate_width, candidate_height), score, area
    return best


def base_work_extent(width: int, height: int, scale: float, *,
                     external_width: int = 0, external_height: int = 0,
                     optimize: bool = False) -> tuple[int, int]:
    """15d660 given a finite scale, external limits and explicit optimizer flag."""
    _extent(width)
    _extent(height)
    _uint(external_width, 'external_width')
    _uint(external_height, 'external_height')
    if type(optimize) is not bool:
        raise ValueError('optimize must be bool')
    scale = _f32(scale)
    if scale <= 0 and external_width and external_height:
        work = min(width, external_width), min(height, external_height)
    else:
        ratio = min(1.0, max(_f32(0.33), scale)) if scale > 0 else 1.0
        work = max(1, native_round_int32(width*ratio)), max(1, native_round_int32(height*ratio))
    return optimize_grid(*work, width, height) if optimize else work


def warped_work_extent(base: tuple[int, int], axes: tuple[Axis, Axis], *,
                       optimize: bool = False) -> tuple[int, int]:
    """156f12..156f99 on caller-supplied axes, AFTER base_work_extent.

    This is a second optimizer call with no reference dimensions. It can change
    each dimension again; combining both searches into one changes behavior.
    Does not implement pair eligibility or produce packed config parameters.
    """
    if len(base) != 2 or len(axes) != 2 or type(optimize) is not bool:
        raise ValueError('two base dimensions, two axes and bool optimize required')
    result = []
    for extent, axis in zip(base, axes):
        _extent(extent)
        if not isinstance(axis, Axis):
            raise ValueError('Axis required')
        if axis.packed and not axis.enabled:
            raise ValueError('native work reference requires canonical zero disabled axis')
        result.append(max(16, native_round_int32(extent*_f32(axis.gmax))) if axis.enabled else extent)
    if any(x > 16384 for x in result):
        raise ValueError('warped extent outside bounded reference domain')
    return optimize_grid(*result) if optimize else tuple(result)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('width', type=int)
    parser.add_argument('height', type=int)
    parser.add_argument('--reference-width', type=int, default=0)
    parser.add_argument('--reference-height', type=int, default=0)
    args = parser.parse_args()
    try:
        selected = optimize_grid(args.width, args.height, args.reference_width, args.reference_height)
        score = lambda extent: asdict(grid_cost(*extent)) if all(64 <= x <= 16384 for x in extent) else None
        print(json.dumps(dict(input_extent=[args.width, args.height], selected_extent=selected,
                              input_cost=score((args.width, args.height)), selected_cost=score(selected),
                              score_is_gpu_time=False, target_executed=False, gpu_tested=False), indent=2))
    except ValueError as error:
        parser.exit(2, str(error)+chr(10))


if __name__ == '__main__':
    main()
