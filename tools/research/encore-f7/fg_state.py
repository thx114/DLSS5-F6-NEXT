"""Offline DLSSGState v4 / Windows x64 ABI diagnostic. Never calls the SDK.

Only this declared layout is supported. A successful decode is not evidence
that an API succeeded or the provider generated/displayed any particular frame.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct

STATE_GUID = bytes.fromhex('e1c88acc79a1f54497fae74112f9bc61')
LAYOUT = 'sl-DLSSGState-v4-win64-pack8'
SIZE = 88


def uint(value: int, name: str, bits: int = 32) -> int:
    if type(value) is not int or not 0 <= value < 2**bits:
        raise ValueError(name+' must be an unsigned '+str(bits)+'-bit integer')
    return value


def decode_state(raw: bytes) -> dict:
    if len(raw) != SIZE:
        raise ValueError('expected exactly 88 bytes of declared DLSSGState v4 x64 layout')
    if raw[8:24] != STATE_GUID or struct.unpack_from('<Q', raw, 24)[0] != 4:
        raise ValueError('wrong GUID or unsupported structure version; no layout guessing')
    def u32(offset: int) -> int:
        return struct.unpack_from('<I', raw, offset)[0]
    def u64(offset: int) -> int:
        return struct.unpack_from('<Q', raw, offset)[0]
    def boolean(offset: int) -> bool | None:
        return bool(raw[offset]) if raw[offset] in (0, 1) else None
    status = u32(0x28)
    names = ((1, 'resolution-too-low'), (2, 'reflex-not-detected-at-runtime'),
             (4, 'HDR-format-not-supported'), (8, 'common-constants-invalid'),
             (16, 'GetCurrentBackBufferIndex-not-called'))
    return dict(layout=LAYOUT, byte_count=len(raw), sha256=hashlib.sha256(raw).hexdigest(),
                struct_version=4, next_pointer=hex(u64(0)), estimated_vram_bytes=u64(0x20),
                status=status, status_flags=[name for bit, name in names if status & bit],
                unknown_or_reserved_status_bits=status & ~31, min_dimension=u32(0x2c),
                sdk_presented_since_previous_GetState=u32(0x30), max_generated=u32(0x34),
                vsync_supported=boolean(0x39), dynamic_mfg_supported=boolean(0x50),
                inputs_fence_pointer=hex(u64(0x40)), inputs_fence_value=u64(0x48),
                next_pointer_dereferenced=False, inputs_retirement_proven=False,
                api_success_proven=False, display_multiplier_proven=False)


def assess(raw: bytes, *, enabled: bool, requested_total: int, get_state_result: int,
           configured_generated: int | None = None, set_options_result: int | None = None,
           base_presents: int | None = None, exclusive_query_window: bool = False) -> dict:
    """Separate user intent, SetOptions acceptance, capability and interval count.

    base_presents is only useful if the caller independently counted successful
    real-frame Presents in precisely the interval between these two GetState
    observations. Even then it supplies an interval ratio, not scanout proof.
    Result codes and exclusivity are caller-supplied observations, not verified
    by this offline process.
    """
    if type(enabled) is not bool or type(exclusive_query_window) is not bool:
        raise ValueError('enabled and exclusive_query_window must be bool')
    uint(requested_total, 'requested_total')
    if not 1 <= requested_total <= 6:
        raise ValueError('requested_total must be 1..6')
    uint(get_state_result, 'get_state_result')
    if configured_generated is not None:
        uint(configured_generated, 'configured_generated')
    if set_options_result is not None:
        uint(set_options_result, 'set_options_result')
    if base_presents is not None:
        uint(base_presents, 'base_presents', 64)
    state = decode_state(raw)
    state_ok = get_state_result == 0 and state['status'] == 0
    cap = state['max_generated'] if get_state_result == 0 else None
    requested_generated = requested_total-1 if enabled else 0
    accepted = (configured_generated if set_options_result == 0 else None)
    warnings = []
    if get_state_result != 0:
        warnings.append('GetState failed: returned fields are not valid telemetry')
    elif state['status']:
        warnings.append('SDK status is nonzero: presentation count is not a successful-FG receipt')
    if configured_generated is not None and set_options_result is None:
        warnings.append('configured count has no SetOptions result')
    if not enabled and configured_generated not in (None, 0):
        warnings.append('disabled request and nonzero configured count require separate mode evidence')
    if accepted is not None and cap is not None and accepted > cap:
        warnings.append('SetOptions accepted count exceeds returned capability: inspect tuple/mode/query epoch')
    if base_presents is not None and not exclusive_query_window:
        warnings.append('query window not exclusive: another GetState may reset the SDK interval counter')
    if base_presents == 0 and state['sdk_presented_since_previous_GetState']:
        warnings.append('SDK count with no counted base Present: delayed or differently scoped work')
    ratio = None
    if state_ok and exclusive_query_window and base_presents:
        ratio = state['sdk_presented_since_previous_GetState']/base_presents
    return dict(schema=1, evidence_domain='offline caller-supplied SDK observations only',
                request=dict(enabled=enabled, total=requested_total, generated=requested_generated),
                configured_generated=configured_generated, set_options_result=set_options_result,
                accepted_options_generated=accepted, get_state_result=get_state_result, state=state,
                requested_within_reported_capability=(requested_generated <= cap if cap is not None else None),
                six_x_capability_reported=(cap >= 5 if cap is not None and state_ok else None),
                six_x_options_accepted=bool(enabled and accepted == 5),
                sdk_interval_ratio=ratio, base_presents_in_same_query_window=base_presents,
                exclusive_query_window_reported=exclusive_query_window, warnings=warnings,
                per_source_generation_count='UNKNOWN', external_display_multiplier='UNKNOWN',
                sustained_six_x_proven=False, inputs_retirement_proven=False)


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--state', type=Path, required=True, help='exact 88-byte v4 struct snapshot')
    p.add_argument('--enabled', choices=('true', 'false'), required=True)
    p.add_argument('--requested-total', type=int, required=True)
    p.add_argument('--get-state-result', type=lambda s: int(s, 0), required=True)
    p.add_argument('--configured-generated', type=int)
    p.add_argument('--set-options-result', type=lambda s: int(s, 0))
    p.add_argument('--base-presents', type=int)
    p.add_argument('--exclusive-query-window', action='store_true')
    p.add_argument('--output', type=Path)
    a = p.parse_args()
    try:
        result = assess(a.state.read_bytes(), enabled=a.enabled == 'true',
                        requested_total=a.requested_total, get_state_result=a.get_state_result,
                        configured_generated=a.configured_generated, set_options_result=a.set_options_result,
                        base_presents=a.base_presents, exclusive_query_window=a.exclusive_query_window)
        result['state_file'] = str(a.state.resolve())
        output = json.dumps(result, indent=2, allow_nan=False)
        if a.output:
            a.output.write_text(output+chr(10), encoding='utf-8')
        else:
            print(output)
    except (ValueError, OSError) as error:
        p.exit(2, str(error)+chr(10))


if __name__ == '__main__':
    main()
