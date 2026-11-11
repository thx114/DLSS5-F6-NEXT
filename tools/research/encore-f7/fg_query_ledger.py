"""Offline caller-recorded GetState boundaries. Never calls SDK or authenticates provenance.

One ordinal covers ALL query attempts across sites/viewports in one SDK session.
Records are ordered as called, never sorted/grouped to hide intervening readers.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Iterable

from fg_state import uint

FIELDS = frozenset('''schema session_id pid process_creation_filetime viewport_id
owner_id display_epoch runtime_tuple_sha256 query_ordinal query_site query_qpc
base_present_total dropped_total all_queries_recorded query_boundary_serialized
options_epoch options_mode options_generated set_options_result requested_total
get_state_result status sdk_presented max_generated'''.split())
U64 = ('process_creation_filetime', 'display_epoch', 'query_ordinal', 'query_qpc',
       'base_present_total', 'dropped_total', 'options_epoch')
U32 = ('pid', 'viewport_id', 'requested_total', 'get_state_result', 'status',
       'sdk_presented', 'max_generated')


def validate(row: dict) -> dict:
    if type(row) is not dict or row.keys() != FIELDS:
        raise ValueError('each query requires exactly the schema-1 fields')
    if type(row['schema']) is not int or row['schema'] != 1:
        raise ValueError('unsupported query ledger schema')
    for name in U64:
        uint(row[name], name, 64)
    for name in U32:
        uint(row[name], name)
    if not row['pid'] or not row['process_creation_filetime'] or not row['query_ordinal']:
        raise ValueError('PID, process creation FILETIME and query ordinal must be nonzero')
    if not 1 <= row['requested_total'] <= 6:
        raise ValueError('requested_total must be 1..6')
    for name in ('session_id', 'owner_id', 'query_site'):
        if type(row[name]) is not str or not row[name].strip() or len(row[name]) > 256:
            raise ValueError(name+' must be a nonempty identifier of at most 256 characters')
    digest = row['runtime_tuple_sha256']
    if (type(digest) is not str or len(digest) != 64 or
            any(c not in '0123456789abcdef' for c in digest)):
        raise ValueError('runtime_tuple_sha256 must be a lowercase SHA256 digest')
    for name in ('all_queries_recorded', 'query_boundary_serialized'):
        if type(row[name]) is not bool:
            raise ValueError(name+' must be bool')
    if row['options_mode'] not in ('unknown', 'off', 'on'):
        raise ValueError('options_mode must be unknown, off or on')
    for name in ('options_generated', 'set_options_result'):
        if row[name] is not None:
            uint(row[name], name)
    if row['set_options_result'] == 0:
        count, mode = row['options_generated'], row['options_mode']
        if count is None or mode == 'unknown' or (mode == 'off' and count != 0) or (mode == 'on' and count == 0):
            raise ValueError('accepted options require off/zero or on/positive generated count')
    return dict(row)


def _endpoint_rejections(row: dict) -> list[str]:
    reasons = []
    for field, reason in (('all_queries_recorded', 'query-coverage-unproven'),
                          ('query_boundary_serialized', 'query-boundary-not-serialized')):
        if not row[field]:
            reasons.append(reason)
    if row['get_state_result'] != 0:
        reasons.append('GetState-failed')
    elif row['status'] != 0:
        reasons.append('runtime-status-nonzero')
    if row['set_options_result'] != 0:
        reasons.append('accepted-options-unknown')
    elif row['get_state_result'] == 0 and row['options_generated'] > row['max_generated']:
        reasons.append('accepted-generated-exceeds-reported-max')
    return reasons


def analyze(records: Iterable[dict]) -> dict:
    """Reject malformed/order-regressed input; resync after two clean endpoints.

    A failed call may or may not reset the SDK counter: both adjacent windows
    are refused. A valid ratio remains only an SDK aggregate, never scanout,
    per-source generation, sustained 6x or resource retirement proof.
    """
    previous, base_highwater, observations = {}, {}, []
    for record in records:
        row = validate(record)
        session = row['session_id']
        prev = previous.get(session)
        if prev:
            if (row['pid'], row['process_creation_filetime']) != (prev['pid'], prev['process_creation_filetime']):
                raise ValueError('session reused across process identities')
            if row['query_ordinal'] <= prev['query_ordinal']:
                raise ValueError('query ordinals must strictly increase across every site and viewport')
            for name in ('query_qpc', 'dropped_total', 'options_epoch', 'display_epoch'):
                if row[name] < prev[name]:
                    raise ValueError(name+' regressed within a session')
        scope = (session, row['viewport_id'], row['owner_id'], row['display_epoch'])
        if row['base_present_total'] < base_highwater.get(scope, 0):
            raise ValueError('successful base-Present counter regressed within its display scope')
        base_highwater[scope] = row['base_present_total']
        reasons = _endpoint_rejections(row)
        delta = None
        if prev is None:
            reasons.append('first-query-has-no-baseline')
        else:
            reasons.extend('previous-'+reason for reason in _endpoint_rejections(prev))
            if row['query_ordinal'] != prev['query_ordinal']+1:
                reasons.append('query-ordinal-gap')
            for field, reason in (
                    ('viewport_id', 'viewport-changed'), ('owner_id', 'display-owner-changed'),
                    ('display_epoch', 'display-epoch-changed'),
                    ('runtime_tuple_sha256', 'runtime-tuple-changed'),
                    ('dropped_total', 'record-loss'), ('options_epoch', 'options-epoch-changed'),
                    ('options_mode', 'options-mode-changed'),
                    ('options_generated', 'options-generated-changed'),
                    ('max_generated', 'reported-max-changed')):
                if row[field] != prev[field]:
                    reasons.append(reason)
            if all(row[name] == prev[name] for name in ('viewport_id', 'owner_id', 'display_epoch')):
                delta = row['base_present_total']-prev['base_present_total']
                if not delta:
                    reasons.append('no-successful-base-Presents-in-window')
        valid = not reasons
        state_valid = row['get_state_result'] == 0
        state_ok = state_valid and row['status'] == 0
        accepted = row['options_generated'] if row['set_options_result'] == 0 else None
        observations.append(dict(
            raw=row, previous_query_ordinal=prev['query_ordinal'] if prev else None,
            state_fields_valid=state_valid, accepted_options_generated=accepted,
            six_x_capability_reported=(row['max_generated'] >= 5 if state_ok else None),
            six_x_options_accepted=bool(accepted == 5 and row['options_mode'] == 'on'),
            base_presents_in_query_window=delta,
            sdk_interval_ratio=row['sdk_presented']/delta if valid else None,
            sdk_interval_fraction=([row['sdk_presented'], delta] if valid else None),
            candidate_window_consistent=valid, window_rejections=reasons))
        previous[session] = row
    if not observations:
        raise ValueError('empty query ledger')
    return dict(schema=1, evidence_domain='offline caller-supplied query ledger only',
                record_count=len(observations), session_count=len(previous),
                consistent_candidate_windows=sum(x['candidate_window_consistent'] for x in observations),
                observations=observations, provenance_authenticated=False,
                per_source_generation_count='UNKNOWN', external_display_multiplier='UNKNOWN',
                sustained_six_x_proven=False, inputs_retirement_proven=False,
                target_executed=False, gpu_tested=False)


def _unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate JSON key: '+key)
        result[key] = value
    return result


def read_jsonl(text: str) -> list[dict]:
    rows = []
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        try:
            rows.append(json.loads(line, object_pairs_hook=_unique_object))
        except ValueError as error:
            raise ValueError('line '+str(number)+': '+str(error)) from error
    return rows


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True, help='schema-1 JSONL, one row per actual GetState attempt')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        if args.output and (args.input.resolve() == args.output.resolve() or
                            (args.output.exists() and args.input.samefile(args.output))):
            raise ValueError('refusing to overwrite input evidence')
        raw = args.input.read_bytes()
        report = analyze(read_jsonl(raw.decode('utf-8-sig')))
        report.update(input_file=str(args.input.resolve()), input_sha256=hashlib.sha256(raw).hexdigest())
        result = json.dumps(report, indent=2, allow_nan=False)+chr(10)
        if args.output:
            args.output.write_text(result, encoding='utf-8')
        else:
            print(result, end='')
    except (ValueError, OSError) as error:
        parser.exit(2, str(error)+chr(10))


if __name__ == '__main__':
    main()
