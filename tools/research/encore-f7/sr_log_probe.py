"""Offline parser for F7's [dlss-input] log, never the old F6 SR32 format.

Per-row raw->SDK jitter hypotheses only. No temporal pairing, formula choice,
physical jitter claim, GPU completion, crash diagnosis or performance timing.
Decimal print uncertainty is an explicitly declared model, not authenticated.
"""
from __future__ import annotations

import argparse
from datetime import datetime
from decimal import Decimal, InvalidOperation
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import re

FIELDS = frozenset('''evaluate result history_slot instance context generation token_frame sdk frame owner
queue epoch fence_target render display token_jitter token_jitter_encoding sdk_pixel_jitter
sdk_mv_scale requested_reset sdk_reset creation_flags depth_inverted color_format depth_format
motion_format frame_time_ms semantics'''.split())
FLOAT_PAIRS = ('token_jitter', 'sdk_pixel_jitter', 'sdk_mv_scale')
NONINTEGER = frozenset((*FLOAT_PAIRS, 'render', 'display', 'frame_time_ms', 'semantics'))
NUMBER = re.compile(r'[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?\Z')
PREFIX = re.compile(r'^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z) \[INFO \] \[dlss-input\] (.+)$')
ENTRY = re.compile(r'(?:^| )([a-z_]+)=')
SEMANTICS = 'sdk-call-inputs; completion-and-quality-unproven'
FLOAT32_MAX = Fraction(2**128-2**104)
OPEN_PREFIX = re.compile(r'^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z) \[INFO \] \[log\] F7 rewrite log opened: (.+)$')


def _uint(text: str, bits: int = 64) -> int:
    if not re.fullmatch(r'[0-9]{1,20}', text):
        raise ValueError('unsigned decimal integer required')
    result = int(text)
    if result >= 2**bits:
        raise ValueError('unsigned integer overflow')
    return result


def decimal_value(text: str) -> Fraction:
    if len(text) > 64 or not NUMBER.fullmatch(text):
        raise ValueError('bounded finite decimal number required')
    try:
        value = Decimal(text)
    except InvalidOperation as error:
        raise ValueError('invalid decimal') from error
    if not value.is_finite() or abs(value.as_tuple().exponent) > 64 or value.adjusted() > 40:
        raise ValueError('decimal outside bounded float32 log domain')
    result = Fraction(value)
    return result


def printed_interval(text: str, significant_digits: int = 6) -> tuple[Fraction, Fraction]:
    """Conservative full decimal quantum, rather than an optimistic half bin.

    Assumes finite float32 printed by defaultfloat/round-to-nearest with p
    significant digits, scientific notation for small nonzero values. Hence
    printed zero denotes zero in this model. Changed precision/locale/format
    needs another contract. Returned intervals deliberately over-approximate.
    """
    if type(significant_digits) is not int or not 1 <= significant_digits <= 9:
        raise ValueError('significant_digits must be 1..9')
    value = decimal_value(text)
    if not value:
        return value, value
    power = Decimal(text).adjusted()-significant_digits+1
    quantum = Fraction(10)**power
    lo, hi = max(-FLOAT32_MAX, value-quantum), min(FLOAT32_MAX, value+quantum)
    if lo > hi:
        raise ValueError('print interval does not intersect finite float32 range')
    return lo, hi


def compare_jitter(raw: str, sdk: str, extent: int, *, significant_digits: int = 6) -> list[dict]:
    if type(extent) is not int or not 1 <= extent <= 16384:
        raise ValueError('render extent must be 1..16384')
    lo, hi = printed_interval(raw, significant_digits)
    out_lo, out_hi = printed_interval(sdk, significant_digits)
    results = []
    for formula, scale, bias in (
            ('raw', Fraction(1), Fraction(0)), ('-raw', Fraction(-1), Fraction(0)),
            ('raw*extent', Fraction(extent), Fraction(0)), ('-raw*extent', Fraction(-extent), Fraction(0)),
            ('0.5-raw*extent', Fraction(-extent), Fraction(1, 2)),
            ('raw*extent/2', Fraction(extent, 2), Fraction(0)),
            ('-raw*extent/2', Fraction(-extent, 2), Fraction(0))):
        predicted = sorted((lo*scale+bias, hi*scale+bias))
        # A deliberately loose final float32 + intermediate-double margin.
        # It includes subnormals; this is not an exact IEEE rounding oracle.
        margin = max(abs(x) for x in predicted)*Fraction(1, 2**22)+Fraction(1, 2**148)
        predicted[0] -= margin
        predicted[1] += margin
        possible = predicted[0] <= out_hi and out_lo <= predicted[1]
        gap = max(Fraction(0), out_lo-predicted[1], predicted[0]-out_hi)
        results.append(dict(formula=formula, intervals_overlap_under_declared_print_model=possible,
                            minimum_interval_gap_pixels=float(gap),
                            printed_center_residual_pixels=float(decimal_value(raw)*scale+bias-decimal_value(sdk))))
    return results


def parse_line(line: str, line_number: int, *, significant_digits: int = 6) -> dict | None:
    if '[dlss-input]' not in line:
        return None
    match = PREFIX.fullmatch(line)
    if not match:
        raise ValueError('line '+str(line_number)+': unsupported/truncated dlss-input prefix')
    utc, body = match.groups()
    datetime.strptime(utc, '%Y-%m-%dT%H:%M:%S.%fZ')
    markers = list(ENTRY.finditer(body))
    if not markers or markers[0].start() != 0:
        raise ValueError('invalid dlss-input field sequence')
    fields = {}
    for index, marker in enumerate(markers):
        key = marker.group(1)
        if key in fields:
            raise ValueError('duplicate dlss-input field: '+key)
        fields[key] = body[marker.end():markers[index+1].start() if index+1 < len(markers) else len(body)].strip()
    if (fields.keys() not in (FIELDS, FIELDS-{'token_jitter_encoding'}) or
            fields.get('semantics') != SEMANTICS):
        raise ValueError('unsupported dlss-input field contract')
    values = {}
    for name in fields.keys()-NONINTEGER:
        values[name] = _uint(fields[name], 32 if name in (
            'result', 'history_slot', 'token_jitter_encoding', 'creation_flags',
            'color_format', 'depth_format', 'motion_format', 'requested_reset', 'sdk_reset', 'depth_inverted') else 64)
    for name in ('requested_reset', 'sdk_reset', 'depth_inverted'):
        if values[name] not in (0, 1):
            raise ValueError(name+' must be 0 or 1')
    for name in ('render', 'display'):
        parts = fields[name].split('x')
        if len(parts) != 2:
            raise ValueError('invalid '+name)
        values[name] = [_uint(part, 32) for part in parts]
        if any(not 1 <= part <= 16384 for part in values[name]):
            raise ValueError('unsupported '+name+' extent')
    for name in FLOAT_PAIRS:
        parts = fields[name].split(',')
        if len(parts) != 2:
            raise ValueError('invalid '+name)
        parts = [part.strip() for part in parts]
        for part in parts:
            if name == 'sdk_mv_scale':
                printed_interval(part, significant_digits)
            else:
                decimal_value(part)  # Jitter intervals are checked by compare_jitter.
        values[name] = parts
    if decimal_value(fields['frame_time_ms']) < 0:
        raise ValueError('negative frame time')
    printed_interval(fields['frame_time_ms'], significant_digits)
    values['frame_time_ms'] = fields['frame_time_ms']
    return dict(line=line_number, utc=utc, fields=values, raw_line=line,
                token_jitter_encoding_recorded='token_jitter_encoding' in fields)


def analyze(text: str, *, significant_digits: int = 6) -> dict:
    printed_interval('1', significant_digits)  # Validate even if no rows.
    rows, openings, segment = [], [], 0
    for number, line in enumerate(text.splitlines(), 1):
        if '[log] F7 rewrite log opened:' in line:
            boundary = OPEN_PREFIX.fullmatch(line)
            if not boundary:
                raise ValueError('line '+str(number)+': malformed log-open boundary')
            datetime.strptime(boundary[1], '%Y-%m-%dT%H:%M:%S.%fZ')
            segment += 1
            openings.append(dict(line=number, utc=boundary[1], log_segment=segment))
            continue
        row = parse_line(line, number, significant_digits=significant_digits)
        if row is None:
            continue
        fields = row['fields']
        row['log_segment'] = segment
        row['log_open_boundary_observed'] = segment != 0
        row['axes'] = [dict(axis='xy'[i], models=compare_jitter(fields['token_jitter'][i],
                         fields['sdk_pixel_jitter'][i], fields['render'][i],
                         significant_digits=significant_digits)) for i in range(2)]
        row['evaluate_result_is_success_code_1'] = fields['result'] == 1
        rows.append(row)
    if not rows:
        raise ValueError('no supported F7 dlss-input rows')
    groups = {}
    for row in rows:
        fields = row['fields']
        # No cross-row deltas or adjacency conclusions. This key only groups
        # descriptive samples; it is not an authenticated temporal session.
        key = (row['log_segment'],)+tuple(fields[name] for name in ('instance', 'context', 'generation', 'sdk', 'owner',
                                             'queue', 'epoch', 'history_slot'))+tuple(fields['render'])+tuple(fields['display'])
        group = groups.setdefault(key, dict(scope={name: fields[name] for name in (
            'instance', 'context', 'generation', 'sdk', 'owner', 'queue', 'epoch', 'history_slot', 'render', 'display')},
            log_segment=row['log_segment'], lines=[], requested_reset_values=set(), sdk_reset_values=set()))
        group['lines'].append(row['line'])
        group['requested_reset_values'].add(fields['requested_reset'])
        group['sdk_reset_values'].add(fields['sdk_reset'])
    summaries = []
    for group in groups.values():
        group['requested_reset_values'] = sorted(group['requested_reset_values'])
        group['sdk_reset_values'] = sorted(group['sdk_reset_values'])
        summaries.append(group)
    model_counts = {axis: {} for axis in 'xy'}
    for row in rows:
        for axis in row['axes']:
            for model in axis['models']:
                counts = model_counts[axis['axis']].setdefault(model['formula'], dict(overlap=0, disjoint=0))
                counts['overlap' if model['intervals_overlap_under_declared_print_model'] else 'disjoint'] += 1
    return dict(schema=1, format='F7-dlss-input-with-or-without-encoding', records=len(rows),
                declared_print_model=dict(significant_digits=significant_digits, uncertainty='full decimal quantum plus loose float32 rounding margin',
                                          zero_means_exact_zero=True, format_authenticated=False),
                observations=rows, log_open_boundaries=openings, descriptive_scopes=summaries, model_counts=model_counts,
                interval_model_is_overapproximation=True, float32_execution_witness_proven=False,
                adjacent_frames_assumed=False, preferred_formula=None, apply_formula=False,
                physical_jitter_semantics='UNKNOWN', provenance_authenticated=False,
                gpu_completion_proven=False, quality_accepted=False, device_fault_cause='UNKNOWN',
                target_executed=False, gpu_tested=False)


def attach_session_receipt(report: dict, path: Path) -> None:
    """Bind caller-selected sidecar bytes; do not authenticate their claims."""
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('duplicate receipt key: '+key)
            result[key] = value
        return result
    raw = path.read_bytes()
    receipt = json.loads(raw.decode('utf-8-sig'), object_pairs_hook=unique_object)
    if type(receipt) is not dict or receipt.get('schema') != 'f7.genshin-candidate-session.v2':
        raise ValueError('unsupported session receipt schema')
    for name, bits in (('pid', 32), ('creationFileTime', 64)):
        value = receipt.get(name)
        if type(value) is not int or not 1 <= value < 2**bits:
            raise ValueError('missing or invalid receipt '+name)
    modules = receipt.get('actualModule')
    if type(modules) is not list or any(type(item) is not dict for item in modules):
        raise ValueError('receipt actualModule must be a list of observations')
    observed_modules = [{name: item.get(name) for name in
                        ('path', 'bytes', 'diskSha256', 'mappedFullShaKnown', 'base')} for item in modules]
    configs = {}
    observed_inputs = receipt.get('runtimeInputsAfter', {})
    if type(observed_inputs) is not dict:
        raise ValueError('receipt runtimeInputsAfter must be a hash mapping')
    for name, digest in observed_inputs.items():
        if str(name).replace(chr(92), '/').split('/')[-1].lower() == 'optiscaler.ini':
            if type(digest) is not str or not re.fullmatch(r'[0-9a-fA-F]{64}', digest):
                raise ValueError('invalid reported configuration SHA256')
            configs[name] = digest.lower()
    report['caller_supplied_session_receipt'] = dict(
        file=str(path.resolve()), sha256=hashlib.sha256(raw).hexdigest(),
        reported_pid=receipt['pid'], reported_creation_filetime=receipt['creationFileTime'],
        reported_started_utc=receipt.get('startedUtc'), reported_modules=observed_modules,
        reported_config_sha256=configs, reported_runtime_inputs_stable=receipt.get('runtimeInputsStable'),
        reported_final_process_state=receipt.get('finalProcessState'),
        association='caller-selected sidecar, not proven from dlss-input line contents',
        process_or_loaded_memory_identity_authenticated=False)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--significant-digits', type=int, default=6)
    parser.add_argument('--session-receipt', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        inputs = [args.input]+([args.session_receipt] if args.session_receipt else [])
        if args.output and any(path.resolve() == args.output.resolve() or
                (args.output.exists() and path.samefile(args.output)) for path in inputs):
            raise ValueError('refusing to overwrite input log')
        raw = args.input.read_bytes()
        report = analyze(raw.decode('utf-8-sig'), significant_digits=args.significant_digits)
        report.update(input_file=str(args.input.resolve()), input_bytes=len(raw),
                      input_sha256=hashlib.sha256(raw).hexdigest())
        if args.session_receipt:
            attach_session_receipt(report, args.session_receipt)
        output = json.dumps(report, indent=2, allow_nan=False)+chr(10)
        if args.output:
            args.output.write_text(output, encoding='utf-8')
        else:
            print(output, end='')
    except (ValueError, OSError, OverflowError) as error:
        parser.exit(2, str(error)+chr(10))


if __name__ == '__main__':
    main()
