"""Offline numerical diagnostics. No camera, writer, frame or GPU authority.

Matrices below use row vectors; input layout conversion is always explicit.
Only the guide/model arithmetic is reconstructed from Encore. Matrix probing
is independent diagnostic code, not recovered source or an SDK adapter.
"""
from __future__ import annotations

import math
import struct
from typing import Sequence

Matrix = list[list[float]]


def require_finite_matrix(a: Matrix) -> None:
    if len(a) != 4 or any(len(row) != 4 for row in a) or not all(math.isfinite(v) for row in a for v in row):
        raise ValueError('expected finite 4x4 matrix')


def matrix(values: Sequence[float], storage: str = 'row-major',
           vectors: str = 'row') -> Matrix:
    if len(values) != 16 or not all(math.isfinite(v) for v in values):
        raise ValueError('matrix needs 16 finite values')
    if storage not in ('row-major', 'column-major') or vectors not in ('row', 'column'):
        raise ValueError('declare storage and vector convention')
    a = [list(values[4*i:4*i+4]) for i in range(4)]
    if (storage == 'column-major') != (vectors == 'column'):
        a = transpose(a)
    return a


def transpose(a: Matrix) -> Matrix:
    require_finite_matrix(a)
    return [list(r) for r in zip(*a)]


def identity() -> Matrix:
    return [[float(i == j) for j in range(4)] for i in range(4)]


def multiply(a: Matrix, b: Matrix) -> Matrix:
    require_finite_matrix(a)
    require_finite_matrix(b)
    try:
        result = [[math.fsum(a[i][k]*b[k][j] for k in range(4))
                   for j in range(4)] for i in range(4)]
    except OverflowError as error:
        raise ValueError('matrix product overflow') from error
    require_finite_matrix(result)
    return result


def residual(a: Matrix, b: Matrix) -> dict:
    require_finite_matrix(a)
    require_finite_matrix(b)
    errors = [abs(a[i][j]-b[i][j]) for i in range(4) for j in range(4)]
    if not all(math.isfinite(v) for v in errors):
        raise ValueError('matrix residual overflow')
    absolute = max(errors)
    return dict(max_abs=absolute, relative_max=absolute/max(1.0, *(abs(v) for r in b for v in r)))


def inverse(a: Matrix) -> Matrix:
    require_finite_matrix(a)
    work = [list(a[i])+identity()[i] for i in range(4)]
    for col in range(4):
        pivot = max(range(col, 4), key=lambda r: abs(work[r][col]))
        if work[pivot][col] == 0:
            raise ValueError('singular matrix')
        work[col], work[pivot] = work[pivot], work[col]
        divisor = work[col][col]
        work[col] = [v/divisor for v in work[col]]
        for row in range(4):
            if row != col:
                factor = work[row][col]
                work[row] = [v-factor*w for v, w in zip(work[row], work[col])]
    result = [r[4:] for r in work]
    if not all(math.isfinite(v) for r in result for v in r):
        raise ValueError('inverse overflow')
    return result


def inverse_pair(a: Matrix, b: Matrix) -> dict:
    return dict(a_times_b=residual(multiply(a, b), identity()),
                b_times_a=residual(multiply(b, a), identity()))


def rigid_diagnostics(a: Matrix) -> dict:
    require_finite_matrix(a)
    gram = [[math.fsum(a[i][k]*a[j][k] for k in range(3))
             for j in range(3)] for i in range(3)]
    determinant = (a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1])
                   - a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0])
                   + a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0]))
    return dict(orthogonality_max_abs=max(abs(gram[i][j]-float(i == j))
                                         for i in range(3) for j in range(3)),
                determinant3=determinant,
                affine_column_max_abs=max(abs(a[i][3]-float(i == 3)) for i in range(4)),
                translation_row=a[3][:3], camera_authority=False)


def projection_candidates(a: Matrix, tolerance: float = 1e-5) -> dict:
    """Test a sparse perspective form; return BOTH compatible clip ranges.

    [sx,0,0,0; 0,sy,0,0; ox,oy,A,s; 0,0,B,0], s=+1 or -1.
    For positive view distance d, NDC z=(s*A)+B/d. Neither depth
    convention nor an offset's jitter/lens-shift meaning is authenticated.
    """
    if not math.isfinite(tolerance) or tolerance < 0:
        raise ValueError('invalid tolerance')
    try:
        require_finite_matrix(a)
    except ValueError:
        return dict(accepted=False, reason='invalid-matrix')
    if a[2][3] == 0:
        return dict(accepted=False, reason='nonfinite-or-no-perspective-w')
    scale = abs(a[2][3])
    p = [[v/scale for v in r] for r in a]
    if not all(math.isfinite(v) for r in p for v in r):
        return dict(accepted=False, reason='normalization-overflow')
    # Small forbidden terms in z/w are not small relative to an arbitrary
    # near distance. Do not silently approximate an oblique/rational frustum.
    if any(p[i][j] != 0 for i, j in ((0, 2), (1, 2), (0, 3), (1, 3), (3, 3))):
        return dict(accepted=False, reason='noncanonical-depth-or-homogeneous-w')
    allowed = {(0, 0), (1, 1), (2, 0), (2, 1), (2, 2), (2, 3), (3, 2)}
    error = max(abs(p[i][j]) for i in range(4) for j in range(4) if (i, j) not in allowed)
    reference = max(1.0, abs(p[0][0]), abs(p[1][1]), abs(p[2][2]))
    shape = error/reference
    if shape > tolerance or p[0][0] == 0 or p[1][1] == 0 or p[3][2] == 0:
        return dict(accepted=False, reason='not-sparse-perspective', shape_residual=shape)
    s, b = p[2][3], p[3][2]
    az = s*p[2][2]
    reverse = b > 0
    hypotheses = []
    for label, low in (('zero-to-one', 0.0), ('minus-one-to-one', -1.0)):
        zn, zf = (1.0, low) if reverse else (low, 1.0)
        dn, df = zn-az, zf-az
        if dn == 0:
            continue
        near = b/dn
        far = b/df if df else None
        if not math.isfinite(near) or near <= 0:
            continue
        if far is not None and (not math.isfinite(far) or far <= near):
            continue
        def full_ndc(distance: float | None) -> float:
            if distance is None:
                return s*p[2][2]/(s*p[2][3])
            z = s*distance
            return (z*p[2][2]+p[3][2])/(z*p[2][3]+p[3][3])
        hypotheses.append(dict(clip_range=label, reverse_z=reverse, near=near, far=far,
                               infinite_far=far is None, far_endpoint_gap=abs(df),
                               endpoint_residual_domain='full normalized matrix on view axis',
                               near_ndc_residual=abs(full_ndc(near)-zn),
                               far_ndc_residual=abs(full_ndc(far)-zf)))
    return dict(accepted=bool(hypotheses), reason='numerical-candidate-only',
                shape_residual=shape, homogeneous_scale=scale, w_z_sign=s,
                focal_scale=[p[0][0], p[1][1]],
                fov_y_radians=2*math.atan(1/abs(p[1][1])),
                aspect=abs(p[1][1]/p[0][0]),
                ndc_center=[s*p[2][0], s*p[2][1]], offset_semantics='UNKNOWN',
                depth_a=az, depth_b=b, hypotheses=hypotheses, camera_authority=False)


def clip_to_previous(current_world_to_clip: Matrix, previous_world_to_clip: Matrix) -> dict:
    """Algebraic relation under declared common world and row-vector convention.

    Does not remove jitter, authenticate history, or prove either frame's owner.
    """
    result = multiply(inverse(current_world_to_clip), previous_world_to_clip)
    back = inverse(result)
    return dict(matrix=result, inverse=back, inverse_residual=inverse_pair(result, back),
                reconstruction=residual(multiply(current_world_to_clip, result), previous_world_to_clip),
                camera_authority=False, history_authenticated=False)


def guide_depth(z: float, reversed_z: bool = False, has_depth: bool = True) -> float:
    """Encore feed-preprocess DXBC; input domain [0,1], absent depth -> 1."""
    if not has_depth:
        return 1.0
    if not math.isfinite(z) or not 0 <= z <= 1:
        raise ValueError('depth must be finite in [0,1]')
    t = 1-z if reversed_z else z
    return t/(1000-999*t)


def motion_basis(pixel_x: float, pixel_y: float, full_width: float, full_height: float,
                 scale: float, depth: float, has_depth: bool) -> tuple[float, ...]:
    """pso-38/48 nine-term screen-flow basis, NOT a view/projection camera."""
    values = (pixel_x, pixel_y, full_width, full_height, scale, depth)
    if not all(math.isfinite(v) for v in values) or min(full_width, full_height, scale) <= 0:
        raise ValueError('invalid basis input')
    x = ((pixel_x+0.5)*scale-full_width/2)/(full_width/2)
    y = ((pixel_y+0.5)*scale-full_height/2)/(full_width/2)
    q = min(0.01/max(depth, 0.0001), 1.0) if has_depth else 0.0
    return 1.0, x, y, x*x, x*y, y*y, q, q*x, q*y


def evaluate_motion(coefficients: Sequence[float], basis: Sequence[float]) -> tuple[float, float]:
    if len(coefficients) != 18 or len(basis) != 9:
        raise ValueError('need 18 coefficients and nine basis terms')
    if not all(math.isfinite(v) for v in (*coefficients, *basis)):
        raise ValueError('nonfinite motion model')
    return tuple(math.fsum(c*v for c, v in zip(coefficients[start:start+9], basis))
                 for start in (0, 9))


def robust_weight(confidence: float, residual_pixels: float, sigma_pixels: float) -> float:
    if not all(math.isfinite(v) for v in (confidence, residual_pixels, sigma_pixels)) or sigma_pixels <= 0:
        raise ValueError('invalid robust weight')
    return confidence*confidence/(1+(residual_pixels/sigma_pixels)**2)


def unpack_flow(word: int) -> tuple[float, float]:
    if not isinstance(word, int) or not 0 <= word <= 0xffffffff:
        raise ValueError('need one unsigned 32-bit packed word')
    def signed16(v: int) -> int:
        return v-65536 if v >= 32768 else v
    return signed16(word & 65535)/32.0, signed16(word >> 16)/32.0


def pack_flow(x: float, y: float) -> int:
    """Nearest-even S10.5 for finite in-range integer-conversion inputs.

    Reject outside signed32 conversion range: DXBC ftoi overflow/NaN is not
    redefined here as an invented saturation policy.
    """
    def part(v: float) -> int:
        if not math.isfinite(v) or not math.isfinite(v*32):
            raise ValueError('flow outside finite signed32 conversion range')
        rounded = round(v*32)
        if not -(2**31) <= rounded < 2**31:
            raise ValueError('rounded flow outside signed32 conversion range')
        return max(-32768, min(32767, rounded)) & 65535
    return part(x) | (part(y) << 16)


def float4_buffer(raw: bytes) -> tuple[float, ...]:
    if not raw or len(raw) % 16:
        raise ValueError('expected complete little-endian float4 CB resource')
    return struct.unpack('<'+str(len(raw)//4)+'f', raw)
