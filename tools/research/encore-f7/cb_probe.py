"""Read entire captured CBs; report numerical hypotheses without camera authority."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
from encore_math import (clip_to_previous, float4_buffer, inverse_pair, matrix,
                         multiply, projection_candidates, residual, rigid_diagnostics)


def integer(value: str) -> int:
    return int(value, 0)


def at(values: tuple[float, ...], offset: int, storage: str, vectors: str) -> list:
    if offset < 0 or offset % 16 or offset+64 > len(values)*4:
        raise ValueError(f'matrix resource offset {offset} is unaligned or outside complete CB')
    return matrix(values[offset//4:offset//4+16], storage, vectors)


def analyze(path: Path, metadata: Path | None, offsets: list[int], storage: str,
            vectors: str, pair: list[int] | None, product: list[int] | None,
            previous: Path | None, clip_offset: int | None) -> dict:
    raw = path.read_bytes()
    values = float4_buffer(raw)
    report = dict(schema=1, cb=str(path.resolve()), cb_bytes=len(raw),
                  cb_sha256=hashlib.sha256(raw).hexdigest(), byte_order='little-endian',
                  offset_domain='absolute bytes in entire resource; no range slicing',
                  assumed_storage=storage, assumed_vectors=vectors,
                  camera_authority=False, source_authenticated=False, frame_authenticated=False,
                  matrices=[])
    if metadata:
        source = metadata.read_bytes()
        m = json.loads(source)
        if m.get('cbByteWidth') != len(raw) or m.get('cbFile') != path.name:
            raise ValueError('metadata CB filename/width does not match complete input')
        first, count = m.get('firstConstant16ByteUnits'), m.get('numConstants16ByteUnits')
        if type(first) is not int or type(count) is not int or first < 0 or count < 0:
            raise ValueError('missing/invalid Context1 range; do not guess')
        report['metadata'] = dict(path=str(metadata.resolve()), sha256=hashlib.sha256(source).hexdigest(),
                                  observation={k: m[k] for k in ('pid', 'captureOrdinal', 'qpc',
                                               'vertexShaderHashFnv1a64', 'pixelShaderHashFnv1a64',
                                               'cbSlot', 'cbDumpRange') if k in m},
                                  firstConstant16ByteUnits=first, numConstants16ByteUnits=count,
                                  requested_bytes=[first*16, (first+count)*16],
                                  requested_range_extends_past_resource=(first+count)*16 > len(raw),
                                  range_interpretation='recorded only; no crop, clamp or correction')
    for offset in offsets:
        numeric = dict(resource_offset=offset, resource_float4=offset//16,
                       raw_rows=at(values, offset, 'row-major', 'row'), interpretations=[])
        for layout in ('row-major', 'column-major'):
            for convention in ('row', 'column'):
                a = at(values, offset, layout, convention)
                numeric['interpretations'].append(dict(storage=layout, vectors=convention,
                                      projection=projection_candidates(a), rigid=rigid_diagnostics(a)))
        report['matrices'].append(numeric)
    get = lambda offset: at(values, offset, storage, vectors)
    if pair:
        report['inverse_pair'] = dict(offsets=pair, residual=inverse_pair(*(get(o) for o in pair)))
    if product:
        a, b, c = (get(o) for o in product)
        report['product'] = dict(offsets=product, order='first * second compared with third',
                                residual=residual(multiply(a, b), c))
    if previous:
        if clip_offset is None:
            raise ValueError('--previous-cb needs explicit --clip-offset in both entire resources')
        previous_raw = previous.read_bytes()
        previous_values = float4_buffer(previous_raw)
        report['clip_relation'] = dict(previous_cb=str(previous.resolve()),
                                      previous_sha256=hashlib.sha256(previous_raw).hexdigest(),
                                      resource_offset=clip_offset,
                                      assumptions='common world; same declared layout; jitter retained; order supplied by caller',
                                      result=clip_to_previous(get(clip_offset), at(previous_values, clip_offset, storage, vectors)))
    return report


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cb', type=Path, required=True)
    p.add_argument('--metadata', type=Path)
    p.add_argument('--offset', type=integer, action='append', required=True,
                   help='absolute byte offset in entire resource, repeatable')
    p.add_argument('--storage', choices=('row-major', 'column-major'), default='row-major')
    p.add_argument('--vectors', choices=('row', 'column'), default='row')
    p.add_argument('--inverse-pair', type=integer, nargs=2)
    p.add_argument('--product', type=integer, nargs=3)
    p.add_argument('--previous-cb', type=Path)
    p.add_argument('--clip-offset', type=integer)
    p.add_argument('--output', type=Path)
    a = p.parse_args()
    try:
        result = analyze(a.cb, a.metadata, a.offset, a.storage, a.vectors,
                         a.inverse_pair, a.product, a.previous_cb, a.clip_offset)
        text = json.dumps(result, indent=2, allow_nan=False)
        if a.output:
            a.output.write_text(text+chr(10), encoding='utf-8')
        else:
            print(text)
    except (ValueError, OSError) as error:
        p.exit(2, str(error)+chr(10))


if __name__ == '__main__':
    main()
