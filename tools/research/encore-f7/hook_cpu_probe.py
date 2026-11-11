"""Offline F7 hook CPU snapshots, never a continuous trace or GPU measurement.

CLI: python -B hook_cpu_probe.py --input F7.log --expected-sha256 HEX --output report.json
Eight legacy rows cannot exclude a truncated eleven-row writer; sampled needs eleven.
Publication follows dlss_trace_probe.write_report's reserved-tree/atomic-replace
strategy, implemented locally to avoid importing unrelated research dependencies.
"""
from __future__ import annotations

import argparse
from decimal import Decimal, InvalidOperation
from fractions import Fraction
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys
import tempfile


SOURCE_ROOT = Path('D:/CODE/DLSS5_NEXT-workers/f7-clean-rewrite/src/platform')
# Parser-reviewed reference bytes, never the compiled-source identity of a case.
# The main product worktree may change independently during or after analysis.
SOURCE_SHA256 = {
    'D3D11Surface.cpp': '54f6469e5517b897af2347960d5966140617121b073d9555714580c2eb82effb',
    'HookCpuTiming.h': 'b0da3d9fcd671ae81aa6c563aaa9aba993a9f1a14d01cc41b52820484d1f5c45',
    'HookCpuScopes.h': 'b960a2c9d2978edd0be362cb85273c30b786ce5e7e3165f24adbd14ad741c9c5',
}
SAMPLED_SOURCE_SHA256 = {
    'D3D11Surface.cpp': 'a72ea81021554618aafd7ca23f0a8080bb1ec94a7ef704797f65f4e4d1f50136',
    'HookCpuTiming.h': '9beca35fb2ce78a3bd7f430723d06e8b629cdd45d623f913fe15a1916c4133ab',
    'HookCpuScopes.h': '431140c5bae31cfcde7e758f15bb2475e2be2278c0e9f1c2b30455348f91fb25',
}
PROFILE_SOURCE_SHA256 = {'legacy': SOURCE_SHA256, 'sampled': SAMPLED_SOURCE_SHA256}
STAGES = ('TokenWork', 'NativeRender', 'DrawSelect', 'DrawBindings',
          'CompositionPrepare', 'NativeDraw', 'CompositionCommit', 'SrDispatch',
          'SurfaceLockWait', 'CompositionLockWait', 'ShaderGetter')
COUNTERS = ('ShaderHit', 'ShaderMiss', 'TargetDraw', 'BindingCapture', 'NestedSuppressed')
SAMPLING_STRIDES = {
    'TokenWork': 1, 'NativeRender': 1, 'DrawSelect': 128, 'DrawBindings': 1,
    'CompositionPrepare': 128, 'NativeDraw': 128, 'CompositionCommit': 128, 'SrDispatch': 1,
    'SurfaceLockWait': 128, 'CompositionLockWait': 128, 'ShaderGetter': 128,
}
HEADER_MARKER, STAGE_MARKER = '[hook-cpu]', '[hook-cpu-stage]'
PREAMBLE = 'cumulative accepted CPU wall scopes;'
HEADER_TYPES = {
    'inclusive_categories_do_not_sum': 'literal:true', 'diagnostic_load': 'literal:true',
    'GPU': 'literal:unknown', 'simulation': 'literal:unknown',
    'binding_cache': 'literal:not-implemented', 'draw_select_coverage': 'literal:DrawIndexed-only',
    'native_draw_coverage': 'literal:observed-graphics', 'pid': 'u32',
    'process_creation_filetime': 'u64', 'creation_valid': 'bool', 'qpc_hz': 'i64',
    'report_qpc': 'i64', 'present_index': 'u64', 'dropped_samples': 'u64',
    'dropped_counters': 'u64', 'invalid_stage': 'u64', 'invalid_counter': 'u64', 'overflow': 'u32',
    **{key: 'u64' for key in COUNTERS},
}
SAMPLED_MARKER_TYPES = {
    'sampled_stages_do_not_extrapolate': 'literal:true',
    'observed_max_only': 'literal:true', 'periodic_aliasing': 'literal:unknown',
}
SAMPLED_HEADER_TYPES = {
    **HEADER_TYPES, **SAMPLED_MARKER_TYPES, 'binding_cache': 'literal:separately-reported',
}
PROFILE_HEADER_TYPES = {'legacy': HEADER_TYPES, 'sampled': SAMPLED_HEADER_TYPES}
STAGE_TYPES = {
    'stage': 'stage', 'samples': 'u64', 'invalid_clock': 'u64',
    'sum_ms': 'ms', 'mean_ms': 'ms',
    **{f'p{p}_{key}': kind for p in (95, 99) for key, kind in (
        ('valid', 'bool'), ('lower_inclusive', 'bool'), ('lower_ms', 'ms'), ('upper_ms', 'ms'))},
    'max_ms': 'ms', 'max_begin_qpc': 'i64', 'max_end_qpc': 'i64', 'max_thread': 'u32',
    'over33_ms': 'u64', 'over50_ms': 'u64', 'over100_ms': 'u64', 'overflow': 'u32',
}
SAMPLED_STAGE_TYPES = {**STAGE_TYPES, 'sampling_stride': 'u32'}
PROFILE_STAGE_TYPES = {'legacy': STAGE_TYPES, 'sampled': SAMPLED_STAGE_TYPES}
INTEGER = re.compile(r'-?(?:0|[1-9][0-9]*)\Z')
NUMBER = re.compile(r'-?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?\Z')
# Logger::TimeStamp and LevelName: only the header receives this prefix.
LOGGER_PREFIX = re.compile(r'(?:\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z )?\[INFO\s*\] ')
BOUNDS = {'u32': (0, 2**32-1), 'u64': (0, 2**64-1), 'i64': (-2**63, 2**63-1)}
LOSS_HEADER = ('dropped_samples', 'dropped_counters', 'invalid_stage', 'invalid_counter', 'overflow')


def _raw_fields(text: str) -> dict[str, str]:
    """Tokenize once, retaining unknown values but rejecting duplicates everywhere."""
    fields = {}
    for token in text.split():
        if token.count('=') != 1:
            raise ValueError('malformed field: '+token)
        key, value = token.split('=')
        if not re.fullmatch('[A-Za-z][A-Za-z0-9_]*', key) or not value:
            raise ValueError('malformed field: '+token)
        if key in fields:
            raise ValueError('duplicate field: '+key)
        fields[key] = value
    return fields


def _fields(text: str, schema: dict[str, str]) -> dict:
    fields, unknown = {}, {}
    for key, value in _raw_fields(text).items():
        if key not in schema:
            unknown[key] = {'raw': value, 'status': 'unknown', 'type_and_semantics_certified': False}
            continue
        kind = schema[key]
        if kind.startswith('literal:'):
            if value != kind.partition(':')[2]:
                raise ValueError('changed contract field: '+key)
            fields[key] = value
        elif kind == 'stage':
            if value not in STAGES:
                raise ValueError('unknown stage: '+value)
            fields[key] = value
        elif kind == 'ms':
            if not NUMBER.fullmatch(value):
                raise ValueError('invalid milliseconds: '+key)
            try:
                number = Decimal(value)
                if not math.isfinite(float(number)):
                    raise ValueError('nonfinite milliseconds: '+key)
            except (InvalidOperation, OverflowError) as error:
                raise ValueError('invalid milliseconds: '+key) from error
            # Keep the printed spelling; writer default precision is six significant digits.
            fields[key] = value
        else:
            integer_kind = 'u32' if kind == 'bool' else kind
            if not INTEGER.fullmatch(value):
                raise ValueError('invalid integer: '+key)
            number = int(value)
            low, high = BOUNDS[integer_kind]
            if not low <= number <= high:
                raise ValueError('integer out of range: '+key)
            if kind == 'bool' and number not in (0, 1):
                raise ValueError('boolean must be 0 or 1: '+key)
            fields[key] = bool(number) if kind == 'bool' else number
    missing = schema.keys() - fields.keys()
    if missing:
        raise ValueError('missing fields: '+', '.join(sorted(missing)))
    fields['_unknown_fields'] = unknown
    return fields


def _print_bounds(text: str) -> tuple[Fraction, Fraction]:
    """Conservative envelope for default ostream precision=6, not exact values."""
    number = Decimal(text)
    center = Fraction(number)
    if not center:
        return center, center
    # Protect conversion from hostile exponents that underflow a Python float.
    if not -330 <= number.adjusted() <= 309:
        raise ValueError('milliseconds outside writer double range')
    quantum = Fraction(10) ** (number.adjusted()-5)
    radius = quantum/2 + abs(center)/2**50
    return center-radius, center+radius


def _matches_print(text: str, exact: Fraction) -> bool:
    lower, upper = _print_bounds(text)
    return lower <= exact <= upper


def _quantile_candidates(fields: dict, percentile: int, hz: int,
                         maximum: int, rank: int) -> list[tuple[int, int, int]]:
    """Intersect possible writer bins with observable order-statistic constraints.

    Each candidate is (bin index, first allowed tick, last allowed tick).
    These are feasibility bounds, not recovered samples or a histogram.
    Called only for valid quantiles, so count/bin overflow has already been excluded.
    """
    prefix = f'p{percentile}_'
    lower, upper = fields[prefix+'lower_ms'], fields[prefix+'upper_ms']
    first, last = (maximum, maximum) if rank == fields['samples'] else (0, maximum)
    if not fields['overflow'] & 8:  # Threshold overflow makes the rank cut counts unknown.
        for threshold in (33, 50, 100):
            cut = fields['samples']-fields[f'over{threshold}_ms']
            edge = hz*threshold//1000
            if rank <= cut:
                last = min(last, edge)
            else:
                first = max(first, edge+1)
    if fields[prefix+'lower_inclusive']:
        return [(0, 0, 0)] if first == 0 and last >= 0 else []
    candidates = []
    for bin_index in range(1, 258):
        low = hz*(bin_index-1)//2000
        high = maximum if bin_index == 257 else hz*bin_index//2000
        allowed_low, allowed_high = max(low+1, first), min(high, last)
        if allowed_low <= allowed_high and _matches_print(lower, Fraction(low*1000, hz)) and \
                _matches_print(upper, Fraction(high*1000, hz)):
            candidates.append((bin_index, allowed_low, allowed_high))
    return candidates


def _json_fields(fields: dict, schema: dict[str, str]) -> dict:
    # All 64-bit integers and process identity components survive JSON consumers.
    return {key: str(value) if schema[key] in ('u64', 'i64') or key == 'pid' else value
            for key, value in fields.items() if key in schema}


def _header(text: str) -> dict:
    if not text.startswith(PREAMBLE+' '):
        raise ValueError('missing cumulative CPU wall preamble')
    tokens = text[len(PREAMBLE):]
    raw = _raw_fields(tokens)
    if 'sampling_stride' in raw:
        raise ValueError('stage sampling_stride cannot appear in a header')
    sampled = bool(SAMPLED_MARKER_TYPES.keys() & raw.keys()) or raw.get('binding_cache') == 'separately-reported'
    profile = 'sampled' if sampled else 'legacy'
    # A partial/contradictory sampled marker set must fail this schema, never
    # fall back to legacy or become unknown extension fields.
    fields = _fields(tokens, PROFILE_HEADER_TYPES[profile])
    fields['_profile'] = profile
    if fields['qpc_hz'] <= 0 or fields['pid'] == 0:
        raise ValueError('QPC frequency and PID must be positive')
    if fields['creation_valid'] != (fields['process_creation_filetime'] != 0):
        raise ValueError('creation_valid disagrees with FILETIME')
    # PresentClockNow failure may be -1; preserve it without invalidating earlier samples.
    if fields['report_qpc'] < -1:
        raise ValueError('invalid report QPC')
    if fields['overflow'] & ~255:
        raise ValueError('unknown overflow bits')
    return fields


def _stage(text: str, hz: int, profile: str = 'legacy') -> dict:
    schema = PROFILE_STAGE_TYPES[profile]
    fields = _fields(text, schema)
    if SAMPLED_MARKER_TYPES.keys() & fields['_unknown_fields'].keys() or 'binding_cache' in fields['_unknown_fields']:
        raise ValueError('header profile declarations cannot appear in a stage')
    if profile == 'legacy' and 'sampling_stride' in fields['_unknown_fields']:
        raise ValueError('legacy stage cannot declare sampling_stride; mixed stage formats')
    stride = fields.get('sampling_stride')
    if profile == 'sampled' and stride != SAMPLING_STRIDES[fields['stage']]:
        raise ValueError('sampling_stride disagrees with sampled stage policy: '+fields['stage'])
    count, overflow = fields['samples'], fields['overflow']
    if overflow & ~255:
        raise ValueError('unknown overflow bits')
    if not (0 <= fields['over100_ms'] <= fields['over50_ms'] <= fields['over33_ms'] <= count):
        raise ValueError('threshold counts must be ordered and <= samples')
    for key in ('sum_ms', 'max_ms'):
        if Decimal(fields[key]) < 0:
            raise ValueError('negative '+key)
    begin, end = fields['max_begin_qpc'], fields['max_end_qpc']
    if begin < 0 or end < begin:
        raise ValueError('invalid maximum QPC endpoints')
    ticks = end-begin  # Subtract integers before any float conversion.
    exact_ms = Fraction(ticks*1000, hz)
    if not _matches_print(fields['max_ms'], exact_ms):
        raise ValueError('max_ms disagrees with exact QPC duration')
    if not count:
        if any(fields[key] for key in ('max_begin_qpc', 'max_end_qpc', 'max_thread',
                                       'over33_ms', 'over50_ms', 'over100_ms')) or any(
                Decimal(fields[key]) != 0 for key in ('sum_ms', 'max_ms')):
            raise ValueError('zero samples cannot carry a maximum call or totals')
    mean_invalid = not count or bool(overflow & 3)
    if mean_invalid:
        if Decimal(fields['mean_ms']) != -1:
            raise ValueError('invalid average must be -1')
    else:
        if Decimal(fields['mean_ms']) < 0:
            raise ValueError('negative mean_ms')
        sum_lower, sum_upper = _print_bounds(fields['sum_ms'])
        mean_lower, mean_upper = _print_bounds(fields['mean_ms'])
        if max(sum_lower, mean_lower*count) > min(sum_upper, mean_upper*count):
            raise ValueError('mean_ms disagrees with printed sum/samples')
    if count and not (overflow & 2):
        lo, hi = _print_bounds(fields['sum_ms'])
        if hi < exact_ms or (not overflow & 1 and lo > exact_ms*count):
            raise ValueError('sum_ms inconsistent with maximum and sample count')
    intervals, candidates, ranks = {}, {}, {}
    for p in (95, 99):
        prefix = f'p{p}_'
        valid = fields[prefix+'valid']
        if valid != (count > 0 and not overflow & 5):
            raise ValueError('quantile validity inconsistent with samples/overflow')
        lower, upper = (Decimal(fields[prefix+name+'_ms']) for name in ('lower', 'upper'))
        inclusive = fields[prefix+'lower_inclusive']
        if lower < 0 or upper < lower:
            raise ValueError('invalid printed quantile interval')
        if not valid:
            if lower != 0 or upper != 0 or inclusive:
                raise ValueError('invalid quantile must carry empty bounds')
        elif inclusive:
            if lower != 0 or upper != 0:
                raise ValueError('only the zero bin is lower-inclusive')
        rank = (count*p+99)//100 if valid else None
        if valid:
            candidates[p] = _quantile_candidates(fields, p, hz, ticks, rank)
            ranks[p] = rank
            if not candidates[p]:
                raise ValueError(f'p{p} interval has no writer bin satisfying nearest-rank/max/threshold constraints')
        intervals[f'p{p}'] = {
            'valid': valid, 'lower_inclusive': inclusive, 'upper_inclusive': True,
            'nearest_rank': str(rank) if valid else None,
            'lower_ms_printed': fields[prefix+'lower_ms'],
            'upper_ms_printed': fields[prefix+'upper_ms'],
            'exact_percentile': False, 'histogram_reconstructed': False,
        }
    if intervals['p95']['valid']:
        # Compare exact candidate supports, not rounded decimal edge centers.
        if ranks[95] == ranks[99]:
            compatible = any(i == j and max(lo, other_lo) <= min(hi, other_hi)
                             for i, lo, hi in candidates[95] for j, other_lo, other_hi in candidates[99])
        else:
            compatible = any(i <= j and lo <= other_hi
                             for i, lo, _ in candidates[95] for j, _, other_hi in candidates[99])
        if not compatible:
            raise ValueError('p95/p99 candidate bins contradict nearest-rank ordering')
    # Thresholds are > floor(hz * milliseconds / 1000), not >= printed milliseconds.
    if count:
        for threshold in (33, 50, 100):
            above = fields[f'over{threshold}_ms']
            if (ticks > hz*threshold//1000) != (above > 0):
                raise ValueError('threshold count disagrees with maximum')
    loss = [key for key in ('invalid_clock', 'overflow') if fields[key]]
    return {
        'fields': _json_fields(fields, schema),
        'sampling': {
            'declared_stride': stride,
            'reference_stride': stride if profile == 'sampled' else 1,
            'periodically_sampled': profile == 'sampled' and stride > 1,
            'selection': ('initial TLS phase zero selects enabled/requested outermost ordinals 1,129,257,... '
                          'per thread/stage; ongoing phase is not reconstructed'
                          if stride == 128 else 'each enabled/requested outermost scope'),
            'tls_phase_scope': 'thread-and-stage' if profile == 'sampled' else None,
            'collector_reset_resets_other_thread_tls_phase': False if profile == 'sampled' else None,
            'periodic_aliasing': 'unknown' if profile == 'sampled' else 'not-declared',
        },
        'statistics_scope': 'accepted selected scopes only' if profile == 'sampled' else 'accepted recorded scopes only',
        'observed_max_only': True, 'population_statistics_estimated': False,
        'unknown_fields': fields['_unknown_fields'],
        'known_field_contract_only': not bool(fields['_unknown_fields']),
        'percentile_intervals': intervals,
        'max_call': None if not count else {
            'begin_qpc': str(begin), 'end_qpc': str(end), 'thread': fields['max_thread'],
            'duration_ticks': str(ticks), 'duration_ms': float(exact_ms),
            'duration_ms_exact': {'numerator': str(exact_ms.numerator),
                                  'denominator': str(exact_ms.denominator)},
            'qpc_hz': str(hz), 'meaning': 'one observed maximum accepted inclusive CPU wall scope',
        },
        'coverage': 'unknown', 'loss_reasons': loss,
        'accepted_max_call_usable': bool(count),
    }


def analyze_text(text: str) -> dict:
    """Parse strict known fields; source attestation is added by analyze_file."""
    snapshots, active = [], None

    def finish(reason: str) -> None:
        nonlocal active
        if active is None:
            return
        count = len(active['stages'])
        profile = active['profile']
        legacy_ambiguous = profile == 'legacy' and count == 8
        complete = count == 11 or legacy_ambiguous
        active.update({
            'stage_set_complete': complete, 'stage_count': count, 'terminated_by': reason,
            'format': profile+'-11' if count == 11 else 'legacy-8-compatible' if legacy_ambiguous else 'incomplete-prefix',
            'legacy_or_truncated_current_ambiguous': legacy_ambiguous,
            'missing_stages_for_current_writer': list(STAGES[count:]),
            'coverage': 'unknown',
            'coverage_reasons': ['cumulative accepted samples only; outstanding/unobserved scopes unknown',
                                 'no continuous trace or reset serial in this format'],
        })
        active['contains_unknown_fields'] = bool(active['unknown_fields']) or any(
            stage['unknown_fields'] for stage in active['stages'])
        active['field_contract_certified'] = not active['contains_unknown_fields']
        if not complete:
            active['coverage_reasons'].append('incomplete snapshot prefix')
        if legacy_ambiguous:
            active['coverage_reasons'].append('eight rows also fit a truncated eleven-stage writer')
        if profile == 'sampled':
            active['coverage_reasons'].append('accepted selected scopes only; no stride extrapolation or full-tail coverage')
        if active['contains_unknown_fields']:
            active['coverage_reasons'].append('unknown extension fields; types/semantics not certified; no extrapolation')
        active['coverage_reasons'].extend(active['loss_reasons'])
        for stage in active['stages']:
            active['coverage_reasons'].extend(stage['fields']['stage']+': '+key for key in stage['loss_reasons'])
        snapshots.append(active)
        active = None

    for line_number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        try:
            if HEADER_MARKER in line:
                prefix, marker, body = line.partition(HEADER_MARKER)
                if (prefix and not LOGGER_PREFIX.fullmatch(prefix)) or HEADER_MARKER in body:
                    raise ValueError('malformed header marker or Logger prefix')
                if STAGE_MARKER in line:
                    raise ValueError('multiple markers on one line')
                finish('next-header')
                header = _header(body.strip())
                profile = header['_profile']
                active = {
                    'header_line': line_number, 'last_line': line_number,
                    'profile': profile,
                    'runtime_compiled_source_identity': 'unknown',
                    'source_contract_certified': False,
                    'header': _json_fields(header, PROFILE_HEADER_TYPES[profile]), 'stages': [],
                    'unknown_fields': header['_unknown_fields'],
                    'identity': {key: str(header[key]) for key in ('pid', 'process_creation_filetime', 'qpc_hz')},
                    'identity_complete': header['creation_valid'],
                    'loss_reasons': [key for key in LOSS_HEADER if header[key]],
                    'counter_semantics': {
                        'collection_rate': 'full-rate', 'accepted_records_only': True,
                        'can_drop_or_overflow': True, 'total_call_denominator': False,
                        'ShaderHit_is_binding_cache_hit': False,
                    },
                    'binding_cache_reporting': {
                        'header_declaration': header['binding_cache'],
                        'separate_marker': '[fsr-binding-cache]' if profile == 'sampled' else None,
                        'separate_metrics_parsed': False, 'real_binding_cache_hit_rate': None,
                        'ShaderHit_is_binding_cache_hit': False,
                    },
                }
                if header['report_qpc'] < 0:
                    active['loss_reasons'].append('invalid report clock')
                continue
            if STAGE_MARKER in line or re.search(r'\[hook-cpu-stage[^\]]*\]', line):
                if not line.startswith(STAGE_MARKER+' '):
                    raise ValueError('stage marker must be exactly [hook-cpu-stage] at line start')
                if active is None:
                    raise ValueError('stage without header')
                stage = _stage(line[len(STAGE_MARKER):], int(active['header']['qpc_hz']), active['profile'])
                names = [entry['fields']['stage'] for entry in active['stages']]
                name = stage['fields']['stage']
                if name in names:
                    raise ValueError('duplicate stage: '+name)
                if len(names) >= len(STAGES) or name != STAGES[len(names)]:
                    raise ValueError('stage order is not a writer prefix')
                stage['line'] = line_number
                active['stages'].append(stage)
                active['last_line'] = line_number
                continue
            if '[hook-cpu' in line:
                raise ValueError('unknown hook CPU marker')
            if active is not None and line:
                # Logger writes an entire multiline snapshot in one WriteFile call.
                # Unrelated text cannot bridge two parts of an otherwise complete group.
                finish('unrelated-log-line')
        except ValueError as error:
            raise ValueError(f'line {line_number}: {error}') from error
    finish('EOF')
    if not snapshots:
        raise ValueError('no hook CPU snapshot headers found')
    seen_reports, previous, previous_profile = set(), None, None
    latest_by_identity = {}
    last_complete = None
    for index, snapshot in enumerate(snapshots):
        identity = tuple(snapshot['identity'].values())
        profile = snapshot['profile']
        report_identity = (profile, *identity, snapshot['header']['report_qpc'], snapshot['header']['present_index'])
        snapshot['repeated_report_identity'] = report_identity in seen_reports
        seen_reports.add(report_identity)
        snapshot['previous_identity_relation'] = ('first' if previous is None else
            'same-process-and-hz' if identity == previous else 'different-process-or-hz')
        previous = identity
        snapshot['previous_profile_relation'] = ('first' if previous_profile is None else
                                                 'same-profile' if profile == previous_profile else 'different-profile')
        previous_profile = profile
        if snapshot['stage_set_complete']:
            last_complete = index
            # Missing FILETIME cannot identify a process lifetime, even with the same PID.
            key = (profile, *identity) if snapshot['identity_complete'] else (profile, *identity, index)
            latest_by_identity[key] = index
    return {
        'schema': 'f7-hook-cpu-probe-v1', 'snapshots': snapshots,
        'last_complete_snapshot_index': last_complete,
        'last_complete_by_identity': [
            {'identity': snapshots[index]['identity'], 'profile': snapshots[index]['profile'], 'snapshot_index': index}
            for index in latest_by_identity.values()],
        'profiles_present': list(dict.fromkeys(snapshot['profile'] for snapshot in snapshots)),
        'semantics': {
            'last_means': 'file order, not a timeline sorted by report_qpc',
            'timing': 'accepted outermost per-stage inclusive CPU wall scopes, including waits/preemption',
            'quantiles': 'printed histogram bounds only; six significant digits; no exact percentile',
            'eight_stage_ambiguity': 'no version/count/tail marker distinguishes legacy 8 from a truncated 11',
            'observer_cost_included': True, 'continuous_trace_available': False,
            'sampling_extrapolation_performed': False,
            'stride_expansion_performed': False, 'counters_are_total_call_denominator': False,
            'population_call_count_estimated': False, 'population_cpu_cost_estimated': False,
            'population_percentiles_estimated': False,
            'binding_cache_hit_rate_included': False, 'ShaderHit_is_binding_cache_hit': False,
            'coverage': 'unknown', 'complete_distribution_proven': False,
            'stage_totals_additive': False, 'stage_subtraction_valid': False,
            'snapshot_deltas_computed': False, 'present_is_render_denominator': False,
            'max_call_scope': 'one stage snapshot only; never min(begin)/max(end) across snapshots',
            'identities_or_frequencies_merged': False, 'reset_boundaries_known': False,
            'gpu_time_measured': False, 'thread_busy_measured': False,
            'causality_proven': False, 'performance_baseline': False,
        },
        'source_contract_certified': False,
        'runtime_compiled_source_identity': 'unknown',
    }


def _stat_id(stat) -> dict:
    return {key: str(getattr(stat, key)) for key in
            ('st_dev', 'st_ino', 'st_size', 'st_mtime_ns', 'st_ctime_ns')}


def _read_bound(path: Path, expected_sha256: str | None = None) -> tuple[bytes, dict]:
    if expected_sha256 is not None and not re.fullmatch('[0-9a-fA-F]{64}', expected_sha256):
        raise ValueError('expected_sha256 must be 64 hexadecimal characters')
    before = _stat_id(path.stat())
    raw = path.read_bytes()
    after = _stat_id(path.stat())
    if before != after or len(raw) != int(before['st_size']):
        raise ValueError('input changed while being read: '+str(path))
    digest = hashlib.sha256(raw).hexdigest()
    if expected_sha256 is not None and digest != expected_sha256.lower():
        raise ValueError('input SHA256 does not match expected_sha256')
    return raw, {'path': str(path.resolve(strict=True)), 'sha256': digest, 'bytes': len(raw),
                 'stat_before': before, 'stat_after': after,
                 'expected_sha256_verified': expected_sha256 is not None}


def analyze_file(path: Path, expected_sha256: str | None = None) -> dict:
    raw, binding = _read_bound(Path(path), expected_sha256)
    try:
        text = raw.decode('utf-8-sig', errors='strict')
    except UnicodeDecodeError as error:
        raise ValueError('input is not valid UTF-8 text') from error
    report = analyze_text(text)
    report['input'] = binding
    sources = []
    profiles = report['profiles_present']
    for name in SOURCE_SHA256:
        _, source = _read_bound(SOURCE_ROOT/name)
        expected = {profile: PROFILE_SOURCE_SHA256[profile][name] for profile in profiles}
        matches = {profile: source['sha256'] == digest for profile, digest in expected.items()}
        source['parser_reference_sha256_by_profile'] = expected
        source['matches_parser_reference_by_profile'] = matches
        # Keep the singular binding for a single-profile input. Mixed input has
        # no single reference SHA and must not inherit the currently matching one.
        source['parser_reference_sha256'] = expected[profiles[0]] if len(profiles) == 1 else None
        source['matches_parser_reference'] = all(matches.values())
        sources.append(source)
    report['source_bindings'] = sources
    contracts = {}
    for profile in profiles:
        reference_matches = all(source['matches_parser_reference_by_profile'][profile] for source in sources)
        contracts[profile] = {
            'parser_reference_sha256': dict(PROFILE_SOURCE_SHA256[profile]),
            'read_source_matches_reference': reference_matches,
            'source_contract_certified': reference_matches and not any(
                snapshot['contains_unknown_fields'] for snapshot in report['snapshots'] if snapshot['profile'] == profile),
            'certification_scope': 'parser-read-reference-only',
            'runtime_compiled_source_identity': 'unknown',
        }
    report['profile_source_contracts'] = contracts
    report['source_reference_matches'] = all(contract['read_source_matches_reference'] for contract in contracts.values())
    report['source_contract_certified'] = all(contract['source_contract_certified'] for contract in contracts.values())
    for snapshot in report['snapshots']:
        contract = contracts[snapshot['profile']]
        snapshot['source_reference_matches'] = contract['read_source_matches_reference']
        snapshot['source_contract_certified'] = contract['read_source_matches_reference'] and not snapshot['contains_unknown_fields']
    report['source_binding_scope'] = (
        'parser-read local reference source bytes only; does not authenticate historical case compiled source, '
        'the log producer or loaded binary')
    if not report['source_contract_certified']:
        report['source_contract_unknown'] = 'source drift or unknown extension fields; no contract certification'
    return report


def _protected_paths(source: Path) -> list[Path]:
    return [source, Path(__file__).resolve(), *(SOURCE_ROOT/name for name in SOURCE_SHA256)]


def _protect_output(protected: list[Path], output: Path) -> None:
    """Reserve the entire output tree, including hardlink/symlink/junction aliases.

    Same policy as verify.protect_inputs, without importing its unrelated tools.
    Atomic replacement additionally prevents truncation through a late file alias.
    """
    root = output.parent.resolve()
    sources = [path.resolve(strict=True) for path in protected]
    for path in sources:
        if not path.is_file():
            raise ValueError('input must be a regular file: '+str(path))
        if path.is_relative_to(root):
            raise ValueError('refusing input inside generated output tree: '+str(path))
    if output.exists() and output.is_dir():
        raise ValueError('output path is a directory')
    pending = [output.parent] if output.parent.exists() else []
    visited = set()
    while pending:
        directory = pending.pop()
        resolved = directory.resolve()
        if resolved in visited:
            continue
        visited.add(resolved)
        for candidate in directory.iterdir():
            if candidate.is_dir():
                pending.append(candidate)
            elif candidate.is_file() and any(candidate.samefile(source) for source in sources):
                raise ValueError('refusing output alias of input: '+str(candidate))


def _revalidate_bindings(report: dict) -> None:
    for binding in [report['input'], *report['source_bindings']]:
        _, current = _read_bound(Path(binding['path']), binding['sha256'])
        if current['stat_after'] != binding['stat_after']:
            raise ValueError('input identity/stat changed before publication: '+binding['path'])


def write_report(source: Path, output: Path, report: dict) -> None:
    """Atomic publication, with input/contract identity rechecks before replacement."""
    source, output = Path(source), Path(output)
    bound_input = Path(report['input']['path'])
    if not source.samefile(bound_input):
        raise ValueError('source argument does not match report input')
    protected = [*_protected_paths(source), bound_input,
                 *(Path(binding['path']) for binding in report['source_bindings'])]
    serialized = json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False)+'\n'
    temporary = None
    try:
        _protect_output(protected, output)
        _revalidate_bindings(report)
        output.parent.mkdir(parents=True, exist_ok=True)
        _protect_output(protected, output)
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', newline='\n',
                dir=output.parent, prefix='.'+output.name+'-', suffix='.tmp', delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(serialized)
        _protect_output(protected, output)
        _revalidate_bindings(report)
        os.replace(temporary, output)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--expected-sha256')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args(argv)
    source, output = args.input.expanduser(), args.output.expanduser()
    try:
        _protect_output(_protected_paths(source), output)
        report = analyze_file(source, args.expected_sha256)
        write_report(source, output, report)
    except (OSError, RuntimeError, ValueError) as error:
        print('error: '+str(error), file=sys.stderr)
        return 2
    print('PASS: wrote '+str(output))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
