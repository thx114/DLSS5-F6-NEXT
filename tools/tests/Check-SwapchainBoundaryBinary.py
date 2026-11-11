"""Check the actual PE vtables, without loading the DLL or third-party packages."""
import json
from pathlib import Path
import struct
import sys


def verify(path):
    raw = Path(path).read_bytes()
    pe = struct.unpack_from('<I', raw, 0x3c)[0]
    assert raw[pe:pe+4] == b'PE\0\0', 'Not a PE image'
    count = struct.unpack_from('<H', raw, pe+6)[0]
    optional_size = struct.unpack_from('<H', raw, pe+20)[0]
    optional = pe+24
    assert struct.unpack_from('<H', raw, optional)[0] == 0x20b, 'Expected PE32+'
    base = struct.unpack_from('<Q', raw, optional+24)[0]
    size = struct.unpack_from('<I', raw, optional+56)[0]
    image = bytearray(size)
    for i in range(count):
        section = optional+optional_size+i*40
        va, length, offset = struct.unpack_from('<III', raw, section+12)
        image[va:va+length] = raw[offset:offset+length]
    rows = {}
    for name in ('Dx11wDx12SC', 'WrappedIDXGISwapChain4'):
        pos = image.find(('.?AV'+name+'@@').encode())
        assert pos >= 16, f'Missing RTTI for {name}'
        descriptor = pos-16
        for col in range(0, len(image)-24, 4):
            if struct.unpack_from('<I', image, col)[0] != 1:
                continue
            if struct.unpack_from('<I', image, col+12)[0] != descriptor:
                continue
            if struct.unpack_from('<I', image, col+20)[0] != col:
                continue
            reference = image.find(struct.pack('<Q', base+col))
            while reference >= 0:
                if reference % 8 == 0:
                    table = reference+8
                    slots = {str(i): struct.unpack_from('<Q', image, table+8*i)[0]-base
                             for i in (10, 11, 13, 14, 37, 38, 40)}
                    if all(0 < value < size for value in slots.values()):
                        rows[name] = {'vtableRva': table, 'slots': slots}
                        break
                reference = image.find(struct.pack('<Q', base+col), reference+1)
            if name in rows:
                break
        assert name in rows, f'Missing vtable for {name}'
    print(json.dumps(rows, indent=2))
    assert rows['Dx11wDx12SC']['slots'] == rows['WrappedIDXGISwapChain4']['slots'], \
        'Different original method entry points: cross-wrapper dispatch is unsafe'
    print('PASS: all seven production vtable entries are identical')


if __name__ == '__main__':
    verify(sys.argv[1])
