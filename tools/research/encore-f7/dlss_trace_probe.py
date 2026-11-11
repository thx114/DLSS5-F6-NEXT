'''Strict offline parser for F7 DlssResetTrace v1 CPU timing records.'''
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
BEGIN_MARKER = 'F7_DLSS_RESET_TRACE_BEGIN'
END_MARKER = 'F7_DLSS_RESET_TRACE_END'
COLUMNS = (
    'actual_call,begin_qpc,end_qpc,completed,sdk_result,slot,source,instance,context,'
    'session_serial,feature_serial,history_binding_serial,'
    'previous_generation,previous_frame,previous_flags,effective_generation,effective_frame,effective_flags,'
    'current_generation,current_frame,reset_reasons,sdk_reset,lifecycle_flags,'
    'jitter_x,jitter_y,motion_scale_x,motion_scale_y,token_jitter_encoding,render_w,render_h,display_w,display_h'
)
ROW_NAMES = tuple(COLUMNS.split(','))
DESCRIPTOR = (
    'history_flags=occupied:1|recorded:2|needsReset:4|needsRecreate:8 '
    'lifecycle_flags=created:1|recreated:2|rebind:4|migrate:8 '
    'reset_reasons=requested:1|needsReset:2|notRecorded:4|generationNotIncreasing:8|frameIndexDiscontinuity:16 '
    'history_binding_serial_semantics=successful-bind-or-migrate-counter-not-sdk-history-length '
    'lifecycle_serials=caller-saturating'
)
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
INT32_MIN, INT32_MAX = -(1 << 31), (1 << 31) - 1
INT64_MIN, INT64_MAX = -(1 << 63), (1 << 63) - 1
FLAG_BITS = {'capacity': 1, 'early_stop': 2, 'invalid_clock': 4, 'invalid_ticket': 8,
             'call_overflow': 16, 'not_started': 64, 'stop_fallback': 512}
BAD_TIMING_FLAGS = 4 | 8 | 16 | 32 | 128 | 256
SERIAL_FIELDS = ('session_serial', 'feature_serial', 'history_binding_serial')
IDENTITY_FIELDS = ('slot', 'source', 'instance', 'context', *SERIAL_FIELDS)
INTEGER_RE = re.compile('-?(?:0|[1-9][0-9]*)')
FLOAT_RE = re.compile('[+-]?(?:(?:[0-9]+(?:[.][0-9]*)?|[.][0-9]+)(?:[eE][+-]?[0-9]+)?|nan|inf)', re.IGNORECASE)
KEY_RE = re.compile('[a-z][a-z0-9_]*')
HEADER_TYPES = {
    'version': 'literal:1', 'configuration': 'u64', 'delay_seconds': 'u32',
    'window_seconds': 'u32', 'capacity': 'u32', 'qpc_frequency': 'i64',
    'started': 'bool', 'anchor_qpc': 'i64', 'delay_ticks': 'i64',
    'end_offset_ticks': 'i64', 'sealed_qpc': 'i64', 'flush_requested_qpc': 'i64',
    'performance_baseline': 'literal:0', 'disk_write_success': 'literal:unknown',
    'logger_write_cost': 'literal:caller-measured',
}
FLAG_TYPES = {'mask': 'u32', 'incomplete': 'bool', 'invalid_clock': 'bool',
              'capacity': 'bool', 'truncated': 'bool', 'early_stop': 'bool',
              'stop_fallback': 'bool', 'not_started': 'bool', 'invalid_ticket': 'bool',
              'call_overflow': 'bool'}
FOOTER_TYPES = {
    'tail_marker': 'literal:1', 'configuration': 'u64', 'actual_calls': 'u64',
    'first_captured_call': 'u64', 'last_captured_call': 'u64', 'events': 'u32',
    'completed': 'u32', 'unfinished': 'u32', 'sdk_resets': 'u32',
    'incomplete': 'bool', 'serialization_us': 'i64',
    'missing_tail_means_incomplete': 'literal:1', 'gpu_completion': 'literal:unproven',
    'performance_baseline': 'literal:0', 'disk_write_success': 'literal:unknown',
}
ROW_TYPES = {
    'actual_call': 'u64', 'begin_qpc': 'i64', 'end_qpc': 'i64', 'completed': 'bool',
    'sdk_result': 'u32', 'slot': 'u32', 'source': 'u64', 'instance': 'u64',
    'context': 'u64', 'session_serial': 'u64', 'feature_serial': 'u64',
    'history_binding_serial': 'u64', 'previous_generation': 'u64',
    'previous_frame': 'u32', 'previous_flags': 'u32', 'effective_generation': 'u64',
    'effective_frame': 'u32', 'effective_flags': 'u32', 'current_generation': 'u64',
    'current_frame': 'u32', 'reset_reasons': 'u32', 'sdk_reset': 'i32',
    'lifecycle_flags': 'u32', 'jitter_x': 'float', 'jitter_y': 'float',
    'motion_scale_x': 'float', 'motion_scale_y': 'float',
    'token_jitter_encoding': 'u32', 'render_w': 'u32', 'render_h': 'u32',
    'display_w': 'u32', 'display_h': 'u32',
}

def _integer(value: str, kind: str) -> int:
    if not INTEGER_RE.fullmatch(value):
        raise ValueError('invalid {} integer: {!r}'.format(kind, value))
    number = int(value, 10)
    bounds = {'u32': (0, UINT32_MAX), 'u64': (0, UINT64_MAX),
              'i32': (INT32_MIN, INT32_MAX), 'i64': (INT64_MIN, INT64_MAX)}
    low, high = bounds[kind]
    if not low <= number <= high:
        raise ValueError('{} integer out of range: {!r}'.format(kind, value))
    return number

def _typed_fields(tokens: str, schema: dict[str, str], label: str) -> dict:
    values = {}
    for token in tokens.split():
        if token.count('=') != 1:
            raise ValueError('malformed {} field: {!r}'.format(label, token))
        key, value = token.split('=', 1)
        if not KEY_RE.fullmatch(key) or not value:
            raise ValueError('malformed {} field: {!r}'.format(label, token))
        if key in values:
            raise ValueError('duplicate {} key: {}'.format(label, key))
        values[key] = value
    if set(values) != set(schema):
        missing, extra = sorted(set(schema)-set(values)), sorted(set(values)-set(schema))
        raise ValueError('{} keys changed (missing={}, extra={})'.format(label, missing, extra))
    parsed = {}
    for key, kind in schema.items():
        value = values[key]
        if kind.startswith('literal:'):
            expected = kind.partition(':')[2]
            if value != expected:
                raise ValueError('{} {} changed: {!r}'.format(label, key, value))
            parsed[key] = value
        elif kind == 'bool':
            number = _integer(value, 'u32')
            if number not in (0, 1):
                raise ValueError('{} {} must be 0 or 1'.format(label, key))
            parsed[key] = bool(number)
        else:
            parsed[key] = _integer(value, kind)
    return parsed

def _header(tokens: str) -> dict:
    value = _typed_fields(tokens, HEADER_TYPES, 'BEGIN')
    if value['configuration'] == 0 or value['capacity'] != 8192:
        raise ValueError('invalid version 1 configuration or capacity')
    if value['delay_seconds'] > 600 or not 1 <= value['window_seconds'] <= 30:
        raise ValueError('invalid version 1 capture window')
    frequency = value['qpc_frequency']
    total = value['delay_seconds'] + value['window_seconds']
    if frequency <= 0 or frequency > INT64_MAX // total:
        raise ValueError('invalid version 1 QPC frequency')
    if value['delay_ticks'] != frequency * value['delay_seconds']:
        raise ValueError('delay_ticks does not match QPC metadata')
    if value['end_offset_ticks'] != frequency * total:
        raise ValueError('end_offset_ticks does not match QPC metadata')
    if value['anchor_qpc'] < 0 or value['sealed_qpc'] < 0:
        raise ValueError('negative anchor or sealed QPC')
    if not value['started'] and value['anchor_qpc'] != 0:
        raise ValueError('unstarted capture has a nonzero anchor')
    return value

def _flags(tokens: str) -> dict:
    value = _typed_fields(tokens, FLAG_TYPES, 'flags')
    mask = value['mask']
    if mask & ~0x3ff:
        raise ValueError('unknown version 1 flag bits')
    for name, bit in FLAG_BITS.items():
        if value[name] != bool(mask & bit):
            raise ValueError('flags {} does not match mask'.format(name))
    if value['truncated'] != value['capacity']:
        raise ValueError('flags truncated does not match capacity')
    return value

def _event(line: str, index: int, warnings: list[str]) -> dict:
    if not line.startswith('E,'):
        raise ValueError('malformed event row')
    cells = line[2:].split(',')
    if len(cells) != len(ROW_NAMES):
        raise ValueError('event {} has {} columns, expected {}'.format(index, len(cells), len(ROW_NAMES)))
    row = {}
    for name, text in zip(ROW_NAMES, cells):
        kind = ROW_TYPES[name]
        if kind == 'float':
            if not FLOAT_RE.fullmatch(text):
                raise ValueError('invalid float in event {} {}: {!r}'.format(index, name, text))
            number = float(text)
            if not math.isfinite(number):
                warnings.append('event {} {} is non-finite; QPC timing remains unchanged'.format(index, name))
            row[name] = number
        elif kind == 'bool':
            number = _integer(text, 'u32')
            if number not in (0, 1):
                raise ValueError('event {} {} must be 0 or 1'.format(index, name))
            row[name] = bool(number)
        else:
            row[name] = _integer(text, kind)
    return row

def _marker(line: str, marker: str) -> str | None:
    position = 0
    found = None
    while True:
        position = line.find(marker, position)
        if position < 0:
            return found
        end = position + len(marker)
        before_ok = position == 0 or line[position-1].isspace()
        after_ok = end == len(line) or line[end].isspace()
        if before_ok and after_ok:
            if found is not None:
                raise ValueError('duplicate {} marker on one line'.format(marker))
            found = line[end:].strip()
        position = end

def _new_block(header_text: str) -> dict:
    return {'header': _header(header_text), 'flags': None, 'columns_seen': False,
            'descriptor_seen': False, 'rows': [], 'seen_calls': set(), 'warnings': []}

def _validate_block(state: dict, tail: bool, footer: dict | None) -> tuple[int, int]:
    header, flags, rows = state['header'], state['flags'], state['rows']
    if flags is None or not state['columns_seen'] or not state['descriptor_seen']:
        raise ValueError('trace block is missing required v1 preamble lines')
    if len(rows) > header['capacity']:
        raise ValueError('event count exceeds serialized capacity')
    if not header['started'] and rows:
        raise ValueError('unstarted capture contains events')
    if tail and header['started'] and header['delay_ticks'] == 0 and not rows:
        raise ValueError('started zero-delay capture is missing its anchoring event')
    if len(rows) == header['capacity'] and not flags['capacity']:
        raise ValueError('full event ring is missing capacity flag')
    if tail and flags['capacity'] and len(rows) != header['capacity']:
        raise ValueError('capacity footer does not contain the full event ring')
    if flags['not_started'] and header['started']:
        raise ValueError('not_started flag disagrees with BEGIN metadata')
    if not header['started'] and not (flags['not_started'] or flags['invalid_clock']):
        raise ValueError('unstarted capture has neither not_started nor invalid_clock flag')
    completed = resets = previous_call = 0
    for index, row in enumerate(rows, 1):
        call = row['actual_call']
        if call == 0 or call <= previous_call:
            raise ValueError('duplicate or unordered actual_call at event {}'.format(index))
        previous_call = call
        completed += int(row['completed'])
        resets += int(row['sdk_reset'] != 0)
        begin = row['begin_qpc']
        if row['completed'] and (begin < 0 or row['end_qpc'] < 0 or row['end_qpc'] < begin):
            state['warnings'].append('event {} has negative or reversed QPC; CPU timing disabled'.format(index))
        elif not row['completed'] and (row['end_qpc'] != 0 or row['sdk_result'] != 0):
            raise ValueError('unfinished event {} has non-default EndEvaluate fields'.format(index))
        if begin >= 0 and not flags['invalid_clock']:
            offset = begin - header['anchor_qpc']
            if offset < header['delay_ticks'] or offset >= header['end_offset_ticks']:
                raise ValueError('event {} Begin QPC is outside configured capture window'.format(index))
        if any(row[name] == UINT64_MAX for name in SERIAL_FIELDS):
            message = 'UINT64_MAX diagnostic serial is saturated; stream identity equality is not proven'
            if message not in state['warnings']:
                state['warnings'].append(message)
    unfinished = len(rows) - completed
    if (flags['mask'] or unfinished) and not flags['incomplete']:
        raise ValueError('flags incomplete disagrees with mask and unfinished count')
    if not tail:
        # The flags summarize the entire serialized capture. A missing suffix
        # may contain unfinished rows that are absent from this legal prefix.
        return completed, resets
    if flags['incomplete'] != bool(flags['mask'] or unfinished):
        raise ValueError('flags incomplete disagrees with mask and unfinished count')
    if footer is None:
        raise ValueError('tail marker has no parsed footer')
    if footer['configuration'] != header['configuration']:
        raise ValueError('BEGIN and END configurations differ')
    if footer['events'] != len(rows) or footer['completed'] != completed:
        raise ValueError('END event counts disagree with captured rows')
    if footer['unfinished'] != unfinished or footer['sdk_resets'] != resets:
        raise ValueError('END unfinished/reset counts disagree with captured rows')
    if footer['incomplete'] != flags['incomplete']:
        raise ValueError('flags and END incomplete values differ')
    first = rows[0]['actual_call'] if rows else 0
    last = rows[-1]['actual_call'] if rows else 0
    if footer['first_captured_call'] != first or footer['last_captured_call'] != last:
        raise ValueError('END captured-call range disagrees with event rows')
    if footer['actual_calls'] < last or footer['serialization_us'] < 0:
        raise ValueError('invalid END actual_calls or serialization duration')
    if header['started'] and footer['actual_calls'] == 0:
        raise ValueError('started capture has no actual calls')
    if rows and footer['actual_calls'] > last+1:
        raise ValueError('END actual_calls exceeds the one uncaptured sealing Begin permitted by v1')
    if not flags['invalid_clock']:
        if header['started'] and header['sealed_qpc'] < header['anchor_qpc']:
            raise ValueError('sealed QPC precedes the anchoring call')
        if any(row['begin_qpc'] > header['sealed_qpc'] for row in rows):
            raise ValueError('sealed QPC precedes a captured Begin')
        if header['flush_requested_qpc'] < header['sealed_qpc']:
            raise ValueError('flush QPC precedes the sealed capture')
        if any(row['completed'] and row['end_qpc'] > header['flush_requested_qpc'] for row in rows):
            raise ValueError('flush QPC precedes a completed recorded call')
        if (not flags['incomplete'] and header['started'] and
                header['sealed_qpc']-header['anchor_qpc'] < header['end_offset_ticks']):
            raise ValueError('complete capture was sealed before its configured window expired')
        if not flags['incomplete'] and any(row['completed'] and row['end_qpc'] > header['sealed_qpc']
                                           for row in rows):
            raise ValueError('complete capture sealed QPC precedes a recorded End')
    return completed, resets

def _metrics(values: list[int], frequency: int) -> dict:
    names = ('sum', 'max', 'p50', 'p95', 'p99')
    if not values:
        return {'count': 0, **{name+'_ticks': None for name in names},
                **{name+'_ms': None for name in names}}
    ordered = sorted(values)
    tick_values = {
        'sum': sum(values), 'max': ordered[-1],
        'p50': ordered[(len(ordered)*50+99)//100-1],
        'p95': ordered[(len(ordered)*95+99)//100-1],
        'p99': ordered[(len(ordered)*99+99)//100-1],
    }
    result = {'count': len(values)}
    for name, value in tick_values.items():
        result[name+'_ticks'] = str(value)
        result[name+'_ms'] = value * 1000.0 / frequency
    return result

def _identity_relation(left: dict, right: dict) -> str:
    if any(left[name] != right[name] for name in IDENTITY_FIELDS):
        return 'different'
    if any(left[name] == UINT64_MAX or right[name] == UINT64_MAX for name in SERIAL_FIELDS):
        return 'unknown_saturated_serial'
    return 'same'

def _report_block(state: dict, tail: bool, footer: dict | None) -> dict:
    completed, resets = _validate_block(state, tail, footer)
    header, flags, rows = state['header'], state['flags'], state['rows']
    unfinished = len(rows) - completed
    timing_usable = header['qpc_frequency'] > 0 and not (flags['mask'] & BAD_TIMING_FLAGS)
    for row in rows:
        if row['begin_qpc'] < 0 or (row['completed'] and
                (row['end_qpc'] < 0 or row['end_qpc'] < row['begin_qpc'])):
            timing_usable = False
            break
    evaluate_ticks = ([row['end_qpc']-row['begin_qpc'] for row in rows if row['completed']]
                      if timing_usable else [])
    warnings = list(state['warnings'])
    sequence_complete = True
    if rows and header['delay_ticks'] == 0 and (
            rows[0]['actual_call'] != 1 or rows[0]['begin_qpc'] != header['anchor_qpc']):
        sequence_complete = False
        warnings.append('zero-delay capture omits or contradicts the first anchoring actual call')
    previous_begin = None
    previous_end = None
    for row in rows:
        if ((previous_begin is not None and row['begin_qpc'] < previous_begin) or
                (previous_end is not None and row['begin_qpc'] < previous_end)):
            timing_usable = False
            sequence_complete = False
        previous_begin = row['begin_qpc']
        previous_end = row['end_qpc'] if row['completed'] else None
    if not timing_usable:
        evaluate_ticks = []
    noncontiguous = overlapping = unknown = 0
    gap_ticks, gap_pairs = [], []
    for left, right in zip(rows, rows[1:]):
        relation = _identity_relation(left, right)
        pair = {'from_actual_call': str(left['actual_call']),
                'to_actual_call': str(right['actual_call']),
                'identity_relation': relation, 'status': None, 'gap_ticks': None}
        if relation == 'different':
            warnings.append('global call interval {}->{} crosses stream identity; not attributed to one stream'.format(
                left['actual_call'], right['actual_call']))
        elif relation == 'unknown_saturated_serial':
            warnings.append('global call interval {}->{} has saturated serial identity; sameness is unknown'.format(
                left['actual_call'], right['actual_call']))
        if right['actual_call'] != left['actual_call'] + 1:
            sequence_complete = False
            noncontiguous += 1
            unknown += 1
            pair['status'] = 'noncontiguous_actual_call'
            warnings.append('captured calls {}->{} are noncontiguous; intervening calls are unknown'.format(
                left['actual_call'], right['actual_call']))
        elif not left['completed']:
            unknown += 1
            pair['status'] = 'previous_call_unfinished'
            warnings.append('call {} has no EndEvaluate; following gap is unknown'.format(left['actual_call']))
        elif left['end_qpc'] < 0 or right['begin_qpc'] < 0 or left['end_qpc'] < left['begin_qpc']:
            unknown += 1
            pair['status'] = 'invalid_qpc'
        elif left['end_qpc'] > right['begin_qpc']:
            overlapping += 1
            pair['status'] = 'overlap'
            warnings.append('call brackets {}->{} overlap in recorded QPC values'.format(
                left['actual_call'], right['actual_call']))
        elif not timing_usable:
            unknown += 1
            pair['status'] = 'timing_unusable'
        else:
            gap = right['begin_qpc'] - left['end_qpc']
            gap_ticks.append(gap)
            pair['status'] = 'measured_global_interval'
            pair['gap_ticks'] = str(gap)
        gap_pairs.append(pair)
    if not tail:
        warnings.append('END tail marker is missing; captured prefix is incomplete')
    for name, bit in FLAG_BITS.items():
        if flags['mask'] & bit:
            warnings.append('capture flag set: '+name)
    if unfinished:
        warnings.append('{} captured SDK call(s) lack EndEvaluate and have no CPU duration'.format(unfinished))
    if not timing_usable:
        warnings.append('QPC timing association is unusable; CPU metrics are omitted')
    block = {
        'configuration': str(header['configuration']), 'tail_present': tail,
        'capture_complete': bool(tail and not flags['incomplete'] and flags['mask'] == 0 and
                                 unfinished == 0 and sequence_complete and timing_usable),
        'events': len(rows), 'completed': completed, 'unfinished': unfinished,
        'reset_count': resets, 'flags': flags['mask'], 'timing_usable': timing_usable,
        'evaluate_cpu': _metrics(evaluate_ticks, header['qpc_frequency']) if timing_usable else None,
        'between_evaluates_cpu': _metrics(gap_ticks, header['qpc_frequency']) if timing_usable else None,
        'noncontiguous_pairs': noncontiguous, 'overlapping_pairs': overlapping,
        'unknown_gap_pairs': unknown, 'warnings': warnings, 'observer_overhead_included': True,
        'window': {
            'started': header['started'], 'qpc_frequency': str(header['qpc_frequency']),
            'anchor_qpc': str(header['anchor_qpc']), 'delay_seconds': header['delay_seconds'],
            'window_seconds': header['window_seconds'], 'delay_ticks': str(header['delay_ticks']),
            'end_offset_ticks': str(header['end_offset_ticks']),
            'sealed_qpc': str(header['sealed_qpc']),
            'flush_requested_qpc': str(header['flush_requested_qpc']),
            'capacity': header['capacity'],
        },
        'gap_pairs': gap_pairs,
    }
    if footer is not None:
        block['actual_calls'] = str(footer['actual_calls'])
        block['first_captured_call'] = str(footer['first_captured_call'])
        block['last_captured_call'] = str(footer['last_captured_call'])
    return block

def analyze_text(text: str) -> dict:
    if not isinstance(text, str):
        raise TypeError('text must be str')
    blocks, active, stage = [], None, None

    def close_missing_tail():
        nonlocal active, stage
        if active is not None:
            blocks.append(_report_block(active, False, None))
            active = stage = None

    for line_number, line in enumerate(text.splitlines(), 1):
        begin_text, end_text = _marker(line, BEGIN_MARKER), _marker(line, END_MARKER)
        if begin_text is not None and end_text is not None:
            raise ValueError('line {} contains both trace markers'.format(line_number))
        if begin_text is not None:
            if active is not None:
                close_missing_tail()
            try:
                active = _new_block(begin_text)
            except ValueError as error:
                raise ValueError('line {}: {}'.format(line_number, error)) from error
            stage = 'flags'
            continue
        if end_text is not None:
            if active is None:
                raise ValueError('orphan END marker on line {}'.format(line_number))
            if stage != 'events':
                raise ValueError('END marker before complete preamble on line {}'.format(line_number))
            try:
                footer = _typed_fields(end_text, FOOTER_TYPES, 'END')
                blocks.append(_report_block(active, True, footer))
            except ValueError as error:
                raise ValueError('line {}: {}'.format(line_number, error)) from error
            active = stage = None
            continue
        if active is None:
            continue
        try:
            if stage == 'flags':
                if not line.startswith('flags '):
                    raise ValueError('expected flags line')
                active['flags'] = _flags(line[len('flags '):])
                stage = 'columns'
            elif stage == 'columns':
                if not line.startswith('columns='):
                    raise ValueError('expected columns line')
                if line[len('columns='):] != COLUMNS:
                    raise ValueError('version 1 column order changed or duplicated')
                active['columns_seen'] = True
                stage = 'descriptor'
            elif stage == 'descriptor':
                if line != DESCRIPTOR:
                    raise ValueError('expected exact v1 descriptor line')
                active['descriptor_seen'] = True
                stage = 'events'
            elif stage == 'events':
                if not line.startswith('E,'):
                    raise ValueError('unexpected line inside trace block')
                row = _event(line, len(active['rows'])+1, active['warnings'])
                call = row['actual_call']
                if call in active['seen_calls']:
                    raise ValueError('duplicate actual_call row: {}'.format(call))
                active['seen_calls'].add(call)
                active['rows'].append(row)
        except ValueError as error:
            raise ValueError('line {}: {}'.format(line_number, error)) from error
    if active is not None:
        close_missing_tail()
    if not blocks:
        raise ValueError('no F7_DLSS_RESET_TRACE_BEGIN version=1 block found')
    return {
        'schema': 1,
        'method': 'QPC wall-clock interval summary of serialized DlssResetTrace v1 calls',
        'scope': {
            'evaluate_cpu': ('end_qpc - begin_qpc is a CPU wall interval: begin QPC is sampled before '
                             'BeginEvaluate records the event, and end QPC after SDK Evaluate returns but '
                             'before EndEvaluate bookkeeping. Includes BeginEvaluate recorder overhead and '
                             'conditional branches; excludes EndEvaluate bookkeeping. Does not measure '
                             'SDK-internal CPU cycles, GPU execution time, or GPU completion'),
            'between_evaluates_cpu': ('next begin_qpc minus prior end_qpc for consecutive actual calls with '
                                      'a completed prior call and no overlap; global CPU wall interval between '
                                      'recorded brackets, not attributable to a lock, component, GPU wait, '
                                      'or game pass'),
            'observer_overhead_included': True, 'cross_configuration_join': False,
            'gpu_execution_measured': False,
        },
        'blocks': blocks, 'gpu_completion_proven': False,
        'causality_proven': False, 'performance_baseline': False,
    }

def analyze_file(path: Path, expected_sha256: str | None = None) -> dict:
    source = Path(path)
    if expected_sha256 is not None and not re.fullmatch('[0-9a-fA-F]{64}', expected_sha256):
        raise ValueError('expected_sha256 must be 64 hexadecimal characters')
    before = source.stat()
    raw = source.read_bytes()
    after = source.stat()
    before_id = (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
    after_id = (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)
    if before_id != after_id or len(raw) != before.st_size or len(raw) != after.st_size:
        raise ValueError('input changed while being read')
    digest = hashlib.sha256(raw).hexdigest()
    verified = expected_sha256 is not None
    if verified and digest.lower() != expected_sha256.lower():
        raise ValueError('input SHA256 does not match expected_sha256')
    try:
        text = raw.decode('utf-8-sig', errors='strict')
    except UnicodeDecodeError as error:
        raise ValueError('input is not valid UTF-8 text') from error
    report = analyze_text(text)
    report['input'] = {'path': str(source.resolve()), 'sha256': digest,
                       'bytes': len(raw), 'expected_sha256_verified': verified}
    return report

def write_report(source: Path, output: Path, report: dict) -> None:
    """Publish via a separate file so replacing a late alias cannot truncate input."""
    from verify import protect_inputs
    protected = [source, Path(__file__).resolve(), Path(__file__).with_name('verify.py').resolve()]
    serialized = json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + chr(10)
    temporary = None
    try:
        protect_inputs(protected, output.parent)
        if output.exists() and output.is_dir():
            raise ValueError('output path is a directory')
        output.parent.mkdir(parents=True, exist_ok=True)
        protect_inputs(protected, output.parent)
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', newline=chr(10),
                dir=output.parent, prefix='.'+output.name+'-', suffix='.tmp', delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(serialized)
        protect_inputs(protected, output.parent)
        os.replace(temporary, output)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--expected-sha256')
    args = parser.parse_args(argv)
    source, output = args.input.expanduser(), args.output.expanduser()
    try:
        from verify import protect_inputs
        protected = [source, Path(__file__).resolve(), Path(__file__).with_name('verify.py').resolve()]
        protect_inputs(protected, output.parent)
        report = analyze_file(source, args.expected_sha256)
        write_report(source, output, report)
    except (OSError, RuntimeError, ValueError) as error:
        print('error: {}'.format(error), file=sys.stderr)
        return 2
    print('PASS: wrote {}'.format(output))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
