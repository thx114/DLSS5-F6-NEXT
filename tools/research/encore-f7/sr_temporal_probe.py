"""Offline projection/raster and raw-to-pixel jitter consistency diagnostics.

Independent math, not recovered Encore source or an SDK integration. No
calibration, preferred formula, camera/source/history authority or GPU calls.
"""
from __future__ import annotations

import argparse
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path

from cb_probe import at
from encore_math import float4_buffer, require_finite_matrix


def vector2(value, name: str) -> list[float]:
    if not isinstance(value, (list, tuple)) or len(value) != 2:
        raise ValueError(name+' needs two finite numbers')
    if any(type(x) not in (int, float) for x in value):
        raise ValueError(name+' needs two finite numbers')
    try:
        result = [float(x) for x in value]
    except OverflowError as error:
        raise ValueError(name+' overflow') from error
    if not all(math.isfinite(x) for x in result):
        raise ValueError(name+' needs two finite numbers')
    return result


def raster_size(value) -> tuple[int, int]:
    if (not isinstance(value, (tuple, list)) or len(value) != 2 or
            any(type(x) is not int or not 1 <= x <= 16384 for x in value)):
        raise ValueError('explicit common raster size must be two integers in 1..16384')
    return tuple(value)


def _tolerance(value) -> float:
    if type(value) not in (int, float):
        raise ValueError('finite nonnegative projection tolerance required')
    try:
        value = float(value)
    except OverflowError as error:
        raise ValueError('projection tolerance overflow') from error
    if not math.isfinite(value) or value < 0:
        raise ValueError('finite nonnegative projection tolerance required')
    return value


def _projection(value) -> tuple[list[list[float]], list[list[Fraction]]]:
    try:
        require_finite_matrix(value)
        rows = [[float(x) for x in row] for row in value]
    except (TypeError, ValueError, OverflowError) as error:
        raise ValueError('finite 4x4 row-vector matrix required') from error
    wz = rows[2][3]
    if wz == 0:
        raise ValueError('perspective w coefficient is zero')
    allowed = {(0, 0), (1, 1), (2, 0), (2, 1), (2, 2), (2, 3), (3, 2)}
    if any(rows[i][j] != 0 for i in range(4) for j in range(4) if (i, j) not in allowed):
        raise ValueError('requires exact sparse perspective shape; no oblique/skew/affine approximation')
    exact = [[Fraction(x)/Fraction(wz) for x in row] for row in rows]
    try:
        normalized = [[float(x) for x in row] for row in exact]
    except OverflowError as error:
        raise ValueError('projection normalization overflow') from error
    if not all(math.isfinite(x) for row in normalized for x in row):
        raise ValueError('projection normalization overflow')
    if any(rows[i][j] != 0 and normalized[i][j] == 0 for i in range(4) for j in range(4)):
        raise ValueError('projection normalization underflow')
    rows = normalized
    if rows[0][0] == 0 or rows[1][1] == 0 or rows[3][2] == 0:
        raise ValueError('degenerate perspective matrix')
    return rows, exact


def projection_shift(previous, current, size=None, *, tolerance: float = 0.0) -> dict:
    """D3D top-left raster: dx=W/2*dNDCx, dy=-H/2*dNDCy.

    Caller declares common view basis, lens, viewport orientation and layout.
    Offset-only changes may be jitter, lens shift, crop or another cause.
    tolerance compares normalized NON-offset terms; default requires equality.
    """
    tolerance = _tolerance(tolerance)
    (p, pe), (c, ce) = _projection(previous), _projection(current)
    nonoffset = [abs(ce[i][j]-pe[i][j]) for i in range(4) for j in range(4)
                 if (i, j) not in ((2, 0), (2, 1))]
    maximum = max(nonoffset)
    exact_delta = [ce[2][i]-pe[2][i] for i in range(2)]
    try:
        maximum_float = float(maximum)
        delta = vector2([float(x) for x in exact_delta], 'principal point delta')
    except OverflowError as error:
        raise ValueError('projection difference overflow') from error
    if any(value != 0 and rounded == 0 for value, rounded in zip(exact_delta, delta)):
        raise ValueError('principal point delta underflow')
    raster = raster_size(size) if size is not None else None
    pixels = None
    if raster:
        exact_pixels = [exact_delta[0]*Fraction(raster[0], 2), -exact_delta[1]*Fraction(raster[1], 2)]
        try:
            pixels = vector2([float(x) for x in exact_pixels], 'raster delta')
        except OverflowError as error:
            raise ValueError('raster delta overflow') from error
        if any(value != 0 and rounded == 0 for value, rounded in zip(exact_pixels, pixels)):
            raise ValueError('raster delta underflow')
    return dict(previous_ndc_center=p[2][:2], current_ndc_center=c[2][:2],
                principal_delta_ndc=delta, nonoffset_max_abs=maximum_float,
                nonoffset_max_abs_exact_ratio=[maximum.numerator, maximum.denominator],
                nonoffset_exactly_equal=maximum == 0,
                nonoffset_max_abs_underflowed=maximum != 0 and maximum_float == 0,
                coefficient_comparison='exact ratios of supplied binary64 inputs',
                nonoffset_tolerance=tolerance, offset_only_candidate=maximum <= Fraction(tolerance),
                caller_common_raster_size=raster, principal_delta_raster_pixels=pixels,
                raster_convention='D3D top-left, x right, y down',
                offset_cause='UNKNOWN', common_view_basis_authenticated=False,
                camera_authority=False, jitter_semantics_authenticated=False)


def affine_jitter_candidates(previous_raw, current_raw, size, observed_delta=None, *,
                             observed_previous=None, observed_current=None) -> dict:
    """Axiswise candidates; observed_delta's meaning/sign is CALLER supplied.

    Never ranks, picks, applies, or certifies a transform. In particular -raw*W
    and .5-raw*W have identical deltas: the constant bias is unidentifiable.
    The same ambiguity exists for any fixed bias, not only .5.
    """
    previous = vector2(previous_raw, 'previous raw jitter')
    current = vector2(current_raw, 'current raw jitter')
    dims = raster_size(size)
    observed = vector2(observed_delta, 'caller observed delta') if observed_delta is not None else None
    if (observed_previous is None) != (observed_current is None):
        raise ValueError('both absolute observations required')
    obs_prev = vector2(observed_previous, 'caller previous value') if observed_previous is not None else None
    obs_cur = vector2(observed_current, 'caller current value') if observed_current is not None else None
    if obs_prev is not None:
        absolute_delta = vector2([obs_cur[i]-obs_prev[i] for i in range(2)], 'absolute observation delta')
        if observed is not None and observed != absolute_delta:
            raise ValueError('absolute observations and supplied delta disagree')
        observed = absolute_delta
    axes = []
    for i, extent in enumerate(dims):
        candidates = []
        for formula, scale, bias in (('raw', 1.0, 0.0), ('-raw', -1.0, 0.0),
                ('raw*extent', float(extent), 0.0), ('-raw*extent', -float(extent), 0.0),
                ('0.5-raw*extent', -float(extent), 0.5),
                ('raw*extent/2', extent*0.5, 0.0), ('-raw*extent/2', -extent*0.5, 0.0)):
            prev_value, value = vector2([previous[i]*scale+bias, current[i]*scale+bias], 'affine values')
            delta = (current[i]-previous[i])*scale
            residual = delta-observed[i] if observed is not None else None
            vector2([delta, residual if residual is not None else 0.0], 'affine delta/residual')
            absolute_residuals = (vector2([prev_value-obs_prev[i], value-obs_cur[i]], 'absolute residuals')
                                  if obs_prev is not None else None)
            candidates.append(dict(formula=formula, scale=scale, bias_pixels=bias,
                                   previous_pixels=prev_value, current_pixels=value,
                                   predicted_delta_pixels=delta, signed_residual=residual,
                                   absolute_residuals=absolute_residuals))
        axes.append(dict(axis='xy'[i], candidates=candidates))
    return dict(previous_raw=previous, current_raw=current, caller_common_raster_size=dims,
                caller_observed_delta=observed, axes=axes,
                caller_observed_previous=obs_prev, caller_observed_current=obs_cur,
                absolute_comparison_available=obs_prev is not None,
                constant_bias_identifiable_from_deltas=False, preferred_formula=None,
                apply_formula=False, jitter_semantics_authenticated=False)


def analyze_values(*, previous_projection=None, current_projection=None, size=None,
                   previous_raw=None, current_raw=None, previous_sdk=None, current_sdk=None,
                   tolerance: float = 0.0) -> dict:
    """Combine separately labelled geometry, token values and caller SDK values."""
    pairs = ((previous_projection, current_projection), (previous_raw, current_raw),
             (previous_sdk, current_sdk))
    if any((a is None) != (b is None) for a, b in pairs):
        raise ValueError('each previous/current input pair must be complete')
    if previous_projection is None and previous_raw is None:
        raise ValueError('projection pair or raw jitter pair required')
    tolerance = _tolerance(tolerance)
    dims = raster_size(size) if size is not None else None
    if previous_raw is not None and dims is None:
        raise ValueError('raw jitter candidates need an explicit common raster size')
    projection = (projection_shift(previous_projection, current_projection, dims, tolerance=tolerance)
                  if previous_projection is not None else None)
    sdk_delta, prev, cur = None, None, None
    if previous_sdk is not None:
        prev, cur = vector2(previous_sdk, 'previous caller SDK jitter'), vector2(current_sdk, 'current caller SDK jitter')
        sdk_delta = vector2([cur[i]-prev[i] for i in range(2)], 'caller SDK jitter delta')
    raw_models = (affine_jitter_candidates(previous_raw, current_raw, dims)
                  if previous_raw is not None else None)
    vs_sdk = (affine_jitter_candidates(previous_raw, current_raw, dims, sdk_delta,
                                       observed_previous=prev, observed_current=cur)
              if previous_raw is not None and sdk_delta is not None else None)
    vs_raster = None
    if (previous_raw is not None and projection and projection['offset_only_candidate'] and
            projection['principal_delta_raster_pixels'] is not None):
        vs_raster = affine_jitter_candidates(previous_raw, current_raw, dims,
                                             projection['principal_delta_raster_pixels'])
    return dict(schema=1, method='offline caller-supplied numerical hypotheses',
                projection=projection, caller_sdk_jitter_previous=prev, caller_sdk_jitter_current=cur,
                caller_sdk_delta_pixels=sdk_delta, raw_candidates=raw_models,
                raw_candidates_compared_to_caller_sdk_values=vs_sdk,
                raw_candidates_compared_to_D3D_raster_delta=vs_raster,
                SDK_sign_relative_to_raster='UNKNOWN', fixed_bias_identifiable_from_deltas=False,
                preferred_formula=None, apply_formula=False, camera_authority=False,
                source_authenticated=False, frame_authenticated=False, history_authenticated=False,
                target_executed=False, gpu_tested=False)


def analyze_cbs(previous: Path, current: Path, *, offset: int, storage: str, vectors: str,
                size=None, previous_raw=None, current_raw=None, previous_sdk=None, current_sdk=None,
                tolerance: float = 0.0) -> dict:
    inputs, matrices = [], []
    for path in (previous, current):
        raw = path.read_bytes()
        matrices.append(at(float4_buffer(raw), offset, storage, vectors))
        inputs.append(dict(path=str(path.resolve()), bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest()))
    report = analyze_values(previous_projection=matrices[0], current_projection=matrices[1],
                            size=size, previous_raw=previous_raw, current_raw=current_raw,
                            previous_sdk=previous_sdk, current_sdk=current_sdk, tolerance=tolerance)
    report.update(input_cbs=inputs, resource_offset=offset, assumed_storage=storage, assumed_vectors=vectors,
                  offset_domain='absolute bytes in entire resource; Context1 range not reinterpreted')
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--previous-cb', type=Path)
    parser.add_argument('--current-cb', type=Path)
    parser.add_argument('--projection-offset', type=lambda x: int(x, 0))
    parser.add_argument('--storage', choices=('row-major', 'column-major'), default='row-major')
    parser.add_argument('--vectors', choices=('row', 'column'), default='row')
    parser.add_argument('--raster-size', type=int, nargs=2)
    parser.add_argument('--raw-previous', type=float, nargs=2)
    parser.add_argument('--raw-current', type=float, nargs=2)
    parser.add_argument('--sdk-previous', type=float, nargs=2)
    parser.add_argument('--sdk-current', type=float, nargs=2)
    parser.add_argument('--nonoffset-tolerance', type=float, default=0.0)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        if bool(args.previous_cb) != bool(args.current_cb):
            raise ValueError('both complete-resource CB files are required')
        kwargs = dict(size=args.raster_size, previous_raw=args.raw_previous, current_raw=args.raw_current,
                      previous_sdk=args.sdk_previous, current_sdk=args.sdk_current, tolerance=args.nonoffset_tolerance)
        if args.previous_cb:
            if args.projection_offset is None:
                raise ValueError('--projection-offset is required for CB input')
            if args.output and any(args.output.resolve() == p.resolve() or
                    (args.output.exists() and args.output.samefile(p)) for p in (args.previous_cb, args.current_cb)):
                raise ValueError('refusing to overwrite input CB evidence')
            report = analyze_cbs(args.previous_cb, args.current_cb, offset=args.projection_offset,
                                 storage=args.storage, vectors=args.vectors, **kwargs)
        else:
            if args.projection_offset is not None:
                raise ValueError('--projection-offset without CB files')
            report = analyze_values(**kwargs)
        text = json.dumps(report, indent=2, allow_nan=False)+chr(10)
        if args.output:
            args.output.write_text(text, encoding='utf-8')
        else:
            print(text, end='')
    except (ValueError, OSError, OverflowError) as error:
        parser.exit(2, str(error)+chr(10))


if __name__ == '__main__':
    main()
