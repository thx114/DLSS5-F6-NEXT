"""Decode one SHA-pinned historical NR reference; never load the payload."""
from __future__ import annotations

import argparse
import hashlib
import importlib
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
from nr_payload_inspect import inspect_file, load_pefile_file, read_pe

REFERENCE_SHA = 'cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed'
SHUTDOWN_FUNCTIONS = {
    'object-token': (0xe8b0, 0xe93c, 0xfe1b4,
                     '0106020006520230'),
    'shutdown-cleanup-entry': (0x23dc0, 0x23ddb, 0xfdf38,
                               '010a04000a3407000a320670'),
    'shutdown-cleanup-body': (0x23ddb, 0x23e12, 0x10057c,
                              '2105020005640600c03d0200db3d020038df0f00'),
    'shutdown-cleanup-tail': (0x23e12, 0x23e57, 0x100590,
                              '21000000c03d0200db3d020038df0f00'),
    'shutdown-helper-entry': (0x1b090, 0x1b098, 0xff75c,
                              '01080300084204e002c00000'),
    'shutdown-helper-setup': (0x1b098, 0x1b0c3, 0xff768,
                              '2117040017f404000534080090b0010098b001005cf70f00'),
    'shutdown-helper-body': (0x1b0c3, 0x1b1b4, 0xff780,
                             '210f06000f740b000a640a000554090098b00100c3b0010068f70f00'),
    'shutdown-helper-tail': (0x1b1b4, 0x1b1d3, 0xff79c,
                             '2100000098b00100c3b0010068f70f00'),
    'shutdown-helper-return': (0x1b1d3, 0x1b20b, 0xff7ac,
                               '2100000090b0010098b001005cf70f00'),
}
SHUTDOWN_IDENTITY_ARRAYS = {
    'shutdown_identity_guid_assertions': 'guids',
    'shutdown_identity_instruction_assertions': 'anchors',
    'shutdown_identity_function_assertions': 'functions',
    'shutdown_identity_path_assertions': 'paths',
}
STATS_ARRAYS = {
    'stats_key_assertions': 'keys',
    'stats_instruction_assertions': 'anchors',
    'stats_function_assertions': 'functions',
    'stats_path_assertions': 'paths',
}
CREATE_ARRAYS = {
    'create_key_assertions': 'keys',
    'create_instruction_assertions': 'anchors',
    'create_function_assertions': 'functions',
    'create_table_assertions': 'tables',
    'create_read_assertions': 'reads',
    'create_path_assertions': 'paths',
}
RECT_ARRAYS = {
    'rect_instruction_assertions': 'anchors',
    'rect_function_assertions': 'functions',
    'rect_path_assertions': 'paths',
}
# Only this parent includes trailing switch DATA. Other windows still require
# their own exact BeginAddress/EndAddress pair, never mere containment.
PDATA_RANGES = {'create-common-body': (0x17b20, 0x181d1)}
RANGES = {
    'd3d12-init-ext': (0x156b0, 0x157e0),
    'd3d12-create': (0x15010, 0x150e1),
    'd3d12-create-body': (0x150e1, 0x1525a),
    'd3d12-create-epilogue': (0x1525a, 0x15275),
    'd3d12-create-lock-failure': (0x15275, 0x1527d),
    'd3d12-evaluate': (0x15280, 0x1548b),
    'd3d12-release': (0x15900, 0x159e4),
    'd3d12-shutdown1': (0x15b00, 0x15be6),
    'init-common': (0x13d40, 0x14015),
    'shutdown-common': (0x14210, 0x143fc),
    'wide-substring-search': (0xa8010, 0xa8222),
    'd3d12-populate-parameters': (0x157e0, 0x15900),
    'published-scaling-ratio-callback': (0x12740, 0x12841),
    'published-stats-callback': (0x14920, 0x14a72),
    'create-common-body': (0x17b20, 0x18140),
    'descriptor-rect-helper': (0x17a20, 0x17b12),
}
RANGES.update((name, (lo, hi)) for name, (lo, hi, _, _) in SHUTDOWN_FUNCTIONS.items())
EXPORT_RVAS = dict(zip((
    'NVSDK_NGX_D3D12_Init_Ext', 'NVSDK_NGX_D3D12_CreateFeature',
    'NVSDK_NGX_D3D12_EvaluateFeature', 'NVSDK_NGX_D3D12_ReleaseFeature',
    'NVSDK_NGX_D3D12_Shutdown1'), (0x156b0, 0x15010, 0x15280, 0x15900, 0x15b00)))
ANCHORS = {
    0x157fb: ('mov', 'rdx, qword ptr [rsp + 0x268]'),
    0x15819: ('call', 'qword ptr [rip + 0xc18f9]'),
    0x1585c: ('call', 'qword ptr [rip + 0xc181e]'),
    0x1586e: ('call', '0x1800a8010'),
    0x15876: ('jne', '0x1800158a8'),
    0x158a1: ('mov', 'eax, 0xbad00002'),
    0x158ab: ('lea', 'r8, [rip - 0xf92]'),
    0x158b2: ('lea', 'rdx, [rip + 0xc3c1f]'),
    0x158bc: ('mov', 'rax, qword ptr [rax]'),
    0x158c8: ('lea', 'r8, [rip - 0x318f]'),
    0x158cf: ('lea', 'rdx, [rip + 0xc3c1a]'),
    0x158d9: ('mov', 'rax, qword ptr [rax]'),
    0x158e2: ('mov', 'eax, 1'),
    0x12749: ('test', 'rcx, rcx'),
    0x1276d: ('mov', 'eax, 0xbad00005'),
    0x12780: ('lea', 'rdx, [rip + 0xc6ff1]'),
    0x12787: ('mov', 'dword ptr [rsp + 0x40], 0'),
    0x1278f: ('mov', 'rax, qword ptr [rax + 0x60]'),
    0x12799: ('and', 'eax, 0xfff00000'),
    0x1279e: ('cmp', 'eax, 0xbad00000'),
    0x127b9: ('movss', 'xmm0, dword ptr [rip + 0xc5fbb]'),
    0x127c1: ('sub', 'ecx, 2'),
    0x127c4: ('je', '0x180012808'),
    0x127c6: ('sub', 'ecx, 2'),
    0x127c9: ('je', '0x1800127fe'),
    0x127cb: ('cmp', 'ecx, 1'),
    0x127ce: ('je', '0x180012808'),
    0x127f3: ('mov', 'eax, 0xbad00010'),
    0x127fe: ('movss', 'xmm1, dword ptr [rip + 0xc7042]'),
    0x12808: ('movaps', 'xmm1, xmm0'),
    0x1280b: ('movss', 'xmm2, dword ptr [rip + 0xc7031]'),
    0x12813: ('comiss', 'xmm1, xmm2'),
    0x1281b: ('minss', 'xmm0, xmm1'),
    0x12822: ('mov', 'rax, qword ptr [rax + 0x30]'),
    0x12826: ('lea', 'rdx, [rip + 0xc7003]'),
    0x12836: ('mov', 'eax, 1'),
    0x1502d: ('mov', 'rdx, qword ptr [rsp + 0x298]'),
    0x15049: ('mov', 'ecx, 6'),
    0x15051: ('call', 'qword ptr [rip + 0xc20c1]'),
    0x15078: ('mov', 'eax, 0xbad00002'),
    0x15092: ('call', 'qword ptr [rip + 0xc1fe8]'),
    0x15098: ('lea', 'rdx, [rip + 0xc42e1]'),
    0x150a4: ('call', '0x1800a8010'),
    0x150a9: ('test', 'rax, rax'),
    0x150ac: ('jne', '0x1800150e1'),
    0x150d7: ('mov', 'eax, 0xbad00002'),
    0x1513e: ('mov', 'qword ptr [rsi], 0'),
    0x15209: ('mov', 'r9, rsi'),
    0x1520c: ('mov', 'r8, rbp'),
    0x1520f: ('mov', 'rdx, rbx'),
    0x15212: ('call', '0x180017b20'),
    0x1522b: ('mov', 'ecx, dword ptr [rax]'),
    0x15240: ('mov', 'qword ptr [rcx + 0x18], rdi'),
    0x15265: ('call', '0x1800a59a0'),
    0x152ad: ('mov', 'rdx, qword ptr [rsp + 0x288]'),
    0x152c0: ('lea', 'ecx, [rbx + 6]'),
    0x152c3: ('call', 'qword ptr [rip + 0xc1e4f]'),
    0x152ea: ('mov', 'eax, 0xbad00002'),
    0x15304: ('call', 'qword ptr [rip + 0xc1d76]'),
    0x1530a: ('lea', 'rdx, [rip + 0xc406f]'),
    0x15316: ('call', '0x1800a8010'),
    0x1531b: ('test', 'rax, rax'),
    0x1531e: ('jne', '0x180015353'),
    0x15349: ('mov', 'eax, 0xbad00002'),
    0x15450: ('call', '0x180018250'),
    0x156da: ('mov', 'rbp, r8'),
    0x156e5: ('mov', 'rdx, qword ptr [rsp + 0x298]'),
    0x156f5: ('mov', 'ebx, r9d'),
    0x156f8: ('lea', 'ecx, [r14 + 6]'),
    0x156fc: ('call', 'qword ptr [rip + 0xc1a16]'),
    0x15723: ('mov', 'eax, 0xbad00002'),
    0x1573d: ('call', 'qword ptr [rip + 0xc193d]'),
    0x15743: ('lea', 'rdx, [rip + 0xc3c36]'),
    0x1574f: ('call', '0x1800a8010'),
    0x15754: ('test', 'rax, rax'),
    0x15757: ('jne', '0x180015789'),
    0x15782: ('mov', 'eax, 0xbad00002'),
    0x15789: ('mov', 'r9, qword ptr [rsp + 0x2c0]'),
    0x15791: ('mov', 'r8d, ebx'),
    0x157ae: ('mov', 'qword ptr [rsp + 0x20], rbp'),
    0x157b3: ('call', '0x180013d40'),
    0x1591b: ('mov', 'rdx, qword ptr [rsp + 0x268]'),
    0x15934: ('mov', 'ecx, 6'),
    0x15939: ('call', 'qword ptr [rip + 0xc17d9]'),
    0x15960: ('mov', 'eax, 0xbad00002'),
    0x15977: ('call', 'qword ptr [rip + 0xc1703]'),
    0x1597d: ('lea', 'rdx, [rip + 0xc39fc]'),
    0x15989: ('call', '0x1800a8010'),
    0x1598e: ('test', 'rax, rax'),
    0x15991: ('jne', '0x1800159c3'),
    0x159bc: ('mov', 'eax, 0xbad00002'),
    0x159c6: ('call', '0x180014020'),
    0x15b1b: ('mov', 'rdx, qword ptr [rsp + 0x268]'),
    0x15b28: ('mov', 'rbx, rcx'),
    0x15b34: ('mov', 'ecx, 6'),
    0x15b39: ('call', 'qword ptr [rip + 0xc15d9]'),
    0x15b60: ('mov', 'eax, 0xbad00002'),
    0x15b77: ('call', 'qword ptr [rip + 0xc1503]'),
    0x15b7d: ('lea', 'rdx, [rip + 0xc37fc]'),
    0x15b89: ('call', '0x1800a8010'),
    0x15b91: ('jne', '0x180015bc3'),
    0x15bbc: ('mov', 'eax, 0xbad00002'),
    0x15bc3: ('mov', 'dl, 1'),
    0x15bc5: ('mov', 'rcx, rbx'),
    0x15bc8: ('call', '0x180014210'),
    0x13d87: ('mov', 'rbp, qword ptr [rsp + 0xa0]'),
    0x13dbf: ('call', '0x18000e8b0'),
    0x1422b: ('mov', 'r12, rcx'),
    0x14250: ('mov', 'rcx, r12'),
    0x14253: ('call', '0x18000e8b0'),
}


# Independently checked original bytes as well as decoder text. The two indirect
# token calls dispatch the previously loaded RAX target through a CFG slot;
# these RIP slots are not method-name imports or SDK ABI authentication.
SHUTDOWN_ANCHORS = {
    0xe8b0: ("4053", "push", "rbx"),
    0xe8b2: ("4883ec30", "sub", "rsp, 0x30"),
    0xe8b6: ("48c744244800000000", "mov", "qword ptr [rsp + 0x48], 0"),
    0xe8bf: ("488bd9", "mov", "rbx, rcx"),
    0xe8c2: ("4885c9", "test", "rcx, rcx"),
    0xe8c5: ("7508", "jne", "0x18000e8cf"),
    0xe8c7: ("33c0", "xor", "eax, eax"),
    0xe8c9: ("4883c430", "add", "rsp, 0x30"),
    0xe8cd: ("5b", "pop", "rbx"),
    0xe8ce: ("c3", "ret", ""),
    0xe8cf: ("488b01", "mov", "rax, qword ptr [rcx]"),
    0xe8d2: ("4c8d4c2448", "lea", "r9, [rsp + 0x48]"),
    0xe8d7: ("4c8d442440", "lea", "r8, [rsp + 0x40]"),
    0xe8dc: ("c744244008000000", "mov", "dword ptr [rsp + 0x40], 8"),
    0xe8e4: ("488d1575a40c00", "lea", "rdx, [rip + 0xca475]"),
    0xe8eb: ("488b4018", "mov", "rax, qword ptr [rax + 0x18]"),
    0xe8ef: ("ff15338b0c00", "call", "qword ptr [rip + 0xc8b33]"),
    0xe8f5: ("85c0", "test", "eax, eax"),
    0xe8f7: ("7938", "jns", "0x18000e931"),
    0xe8f9: ("b801000000", "mov", "eax, 1"),
    0xe8fe: ("f0480fc10511171000", "lock xadd", "qword ptr [rip + 0x101711], rax"),
    0xe907: ("48ffc0", "inc", "rax"),
    0xe90a: ("4c8d4c2448", "lea", "r9, [rsp + 0x48]"),
    0xe90f: ("4889442448", "mov", "qword ptr [rsp + 0x48], rax"),
    0xe914: ("488d1545a40c00", "lea", "rdx, [rip + 0xca445]"),
    0xe91b: ("488b03", "mov", "rax, qword ptr [rbx]"),
    0xe91e: ("41b808000000", "mov", "r8d, 8"),
    0xe924: ("488bcb", "mov", "rcx, rbx"),
    0xe927: ("488b4020", "mov", "rax, qword ptr [rax + 0x20]"),
    0xe92b: ("ff15f78a0c00", "call", "qword ptr [rip + 0xc8af7]"),
    0xe931: ("488b442448", "mov", "rax, qword ptr [rsp + 0x48]"),
    0xe936: ("4883c430", "add", "rsp, 0x30"),
    0xe93a: ("5b", "pop", "rbx"),
    0xe93b: ("c3", "ret", ""),
    0x13dbc: ("488bcd", "mov", "rcx, rbp"),
    0x13dbf: ("e8ecaaffff", "call", "0x18000e8b0"),
    0x13dc4: ("4889442430", "mov", "qword ptr [rsp + 0x30], rax"),
    0x13dc9: ("4c8d442430", "lea", "r8, [rsp + 0x30]"),
    0x13dce: ("488d542448", "lea", "rdx, [rsp + 0x48]"),
    0x13dd3: ("488d0db6cf4800", "lea", "rcx, [rip + 0x48cfb6]"),
    0x13dda: ("e821060000", "call", "0x180014400"),
    0x1422b: ("4c8be1", "mov", "r12, rcx"),
    0x14242: ("85c0", "test", "eax, eax"),
    0x14244: ("0f85aa010000", "jne", "0x1800143f4"),
    0x1424a: ("8d6801", "lea", "ebp, [rax + 1]"),
    0x1424d: ("4533ff", "xor", "r15d, r15d"),
    0x14250: ("498bcc", "mov", "rcx, r12"),
    0x14253: ("e858a6ffff", "call", "0x18000e8b0"),
    0x14258: ("4c8be8", "mov", "r13, rax"),
    0x14265: ("483bd9", "cmp", "rbx, rcx"),
    0x14268: ("0f844e010000", "je", "0x1800143bc"),
    0x14280: ("4c8d7310", "lea", "r14, [rbx + 0x10]"),
    0x14284: ("4d85e4", "test", "r12, r12"),
    0x14287: ("7410", "je", "0x180014299"),
    0x14289: ("4d392e", "cmp", "qword ptr [r14], r13"),
    0x1428c: ("740b", "je", "0x180014299"),
    0x1428e: ("41ffc7", "inc", "r15d"),
    0x14291: ("488b1b", "mov", "rbx, qword ptr [rbx]"),
    0x14294: ("e90e010000", "jmp", "0x1800143a7"),
    0x14299: ("488d4b18", "lea", "rcx, [rbx + 0x18]"),
    0x1429d: ("e81efb0000", "call", "0x180023dc0"),
    0x142a2: ("83f801", "cmp", "eax, 1"),
    0x142a5: ("7406", "je", "0x1800142ad"),
    0x142a7: ("4084ff", "test", "dil, dil"),
    0x142aa: ("0f45e8", "cmovne", "ebp, eax"),
    0x14357: ("488b1b", "mov", "rbx, qword ptr [rbx]"),
    0x1437b: ("498d4e08", "lea", "rcx, [r14 + 8]"),
    0x1437f: ("e83cfa0000", "call", "0x180023dc0"),
    0x143a7: ("483bd9", "cmp", "rbx, rcx"),
    0x143aa: ("0f85d0feffff", "jne", "0x180014280"),
    0x143b0: ("4585ff", "test", "r15d, r15d"),
    0x143ba: ("7519", "jne", "0x1800143d5"),
    0x143bc: ("488d0dcdc94800", "lea", "rcx, [rip + 0x48c9cd]"),
    0x143c3: ("e8b80b0000", "call", "0x180014f80"),
    0x143c8: ("488d0da1bc0f00", "lea", "rcx, [rip + 0xfbca1]"),
    0x143cf: ("e8dcecffff", "call", "0x1800130b0"),
    0x143d5: ("488bce", "mov", "rcx, rsi"),
    0x143d8: ("e85bf90800", "call", "0x1800a3d38"),
    0x143dd: ("8bc5", "mov", "eax, ebp"),
    0x143f3: ("c3", "ret", ""),
    0x143f4: ("8bc8", "mov", "ecx, eax"),
    0x143f6: ("e849fa0800", "call", "0x1800a3e44"),
    0x143fb: ("cc", "int3", ""),
    0x1b0bd: ("0f84f1000000", "je", "0x18001b1b4"),
    0x1b100: ("ff1522c30b00", "call", "qword ptr [rip + 0xbc322]"),
    0x1b162: ("ff15c0c20b00", "call", "qword ptr [rip + 0xbc2c0]"),
    0x1b181: ("ff15a1c20b00", "call", "qword ptr [rip + 0xbc2a1]"),
    0x1b1b8: ("e8f32a0000", "call", "0x18001dcb0"),
    0x1b1d1: ("741d", "je", "0x18001b1f0"),
    0x1b1da: ("7414", "je", "0x18001b1f0"),
    0x1b1e2: ("488b4020", "mov", "rax, qword ptr [rax + 0x20]"),
    0x1b1e6: ("ff153cc20b00", "call", "qword ptr [rip + 0xbc23c]"),
    0x1b1ec: ("4d896610", "mov", "qword ptr [r14 + 0x10], r12"),
    0x1b1f0: ("498d4e70", "lea", "rcx, [r14 + 0x70]"),
    0x1b1f4: ("e807040000", "call", "0x18001b600"),
    0x1b1f9: ("b801000000", "mov", "eax, 1"),
    0x1b1fe: ("45886618", "mov", "byte ptr [r14 + 0x18], r12b"),
    0x1b202: ("4883c428", "add", "rsp, 0x28"),
    0x1b206: ("415e", "pop", "r14"),
    0x1b208: ("415c", "pop", "r12"),
    0x1b20a: ("c3", "ret", ""),
    0x23dca: ("488bd9", "mov", "rbx, rcx"),
    0x23dcd: ("bf01000000", "mov", "edi, 1"),
    0x23dd2: ("488b4940", "mov", "rcx, qword ptr [rcx + 0x40]"),
    0x23dd6: ("4885c9", "test", "rcx, rcx"),
    0x23dd9: ("7437", "je", "0x180023e12"),
    0x23de0: ("e8ab72ffff", "call", "0x18001b090"),
    0x23de5: ("488b7340", "mov", "rsi, qword ptr [rbx + 0x40]"),
    0x23de9: ("8bf8", "mov", "edi, eax"),
    0x23deb: ("4885f6", "test", "rsi, rsi"),
    0x23dee: ("7415", "je", "0x180023e05"),
    0x23df0: ("488bce", "mov", "rcx, rsi"),
    0x23df3: ("e8782fffff", "call", "0x180016d70"),
    0x23df8: ("baa8000000", "mov", "edx, 0xa8"),
    0x23dfd: ("488bce", "mov", "rcx, rsi"),
    0x23e00: ("e85f140800", "call", "0x1800a5264"),
    0x23e0a: ("48c7434000000000", "mov", "qword ptr [rbx + 0x40], 0"),
    0x23e12: ("488b4b70", "mov", "rcx, qword ptr [rbx + 0x70]"),
    0x23e19: ("742f", "je", "0x180023e4a"),
    0x23e1e: ("488b4010", "mov", "rax, qword ptr [rax + 0x10]"),
    0x23e22: ("ff1500360b00", "call", "qword ptr [rip + 0xb3600]"),
    0x23e34: ("ba01000000", "mov", "edx, 1"),
    0x23e39: ("488b00", "mov", "rax, qword ptr [rax]"),
    0x23e3c: ("ff15e6350b00", "call", "qword ptr [rip + 0xb35e6]"),
    0x23e42: ("48c7437000000000", "mov", "qword ptr [rbx + 0x70], 0"),
    0x23e4f: ("8bc7", "mov", "eax, edi"),
    0x23e56: ("c3", "ret", ""),
}


# Complete bounded callback bytes, independently decoded from the fixed-SHA PE.
# Keeping the intervening instructions binds negative observations (no result
# check or second-output read) without inventing a general dataflow parser.
STATS_ANCHORS = {
    0x14920: ('48895c2420', 'mov', 'qword ptr [rsp + 0x20], rbx'),
    0x14925: ('57', 'push', 'rdi'),
    0x14926: ('4883ec20', 'sub', 'rsp, 0x20'),
    0x1492a: ('488bd9', 'mov', 'rbx, rcx'),
    0x1492d: ('4885c9', 'test', 'rcx, rcx'),
    0x14930: ('752f', 'jne', '0x180014961'),
    0x14932: ('4c8d0da74d0c00', 'lea', 'r9, [rip + 0xc4da7]'),
    0x14939: ('4c8d05c04d0c00', 'lea', 'r8, [rip + 0xc4dc0]'),
    0x14940: ('baf8000000', 'mov', 'edx, 0xf8'),
    0x14945: ('488d0d444c0c00', 'lea', 'rcx, [rip + 0xc4c44]'),
    0x1494c: ('e8df95ffff', 'call', '0x18000df30'),
    0x14951: ('b80500d0ba', 'mov', 'eax, 0xbad00005'),
    0x14956: ('488b5c2448', 'mov', 'rbx, qword ptr [rsp + 0x48]'),
    0x1495b: ('4883c420', 'add', 'rsp, 0x20'),
    0x1495f: ('5f', 'pop', 'rdi'),
    0x14960: ('c3', 'ret', ''),
    0x14961: ('33c0', 'xor', 'eax, eax'),
    0x14963: ('4889442430', 'mov', 'qword ptr [rsp + 0x30], rax'),
    0x14968: ('4889442438', 'mov', 'qword ptr [rsp + 0x38], rax'),
    0x1496d: ('488d3dccc34800', 'lea', 'rdi, [rip + 0x48c3cc]'),
    0x14974: ('48897c2440', 'mov', 'qword ptr [rsp + 0x40], rdi'),
    0x14979: ('488bcf', 'mov', 'rcx, rdi'),
    0x1497c: ('e8aff30800', 'call', '0x1800a3d30'),
    0x14981: ('85c0', 'test', 'eax, eax'),
    0x14983: ('0f85e1000000', 'jne', '0x180014a6a'),
    0x14989: ('488b0508c44800', 'mov', 'rax, qword ptr [rip + 0x48c408]'),
    0x14990: ('488b10', 'mov', 'rdx, qword ptr [rax]'),
    0x14993: ('483bd0', 'cmp', 'rdx, rax'),
    0x14996: ('0f8496000000', 'je', '0x180014a32'),
    0x1499c: ('488b8a88000000', 'mov', 'rcx, qword ptr [rdx + 0x88]'),
    0x149a3: ('4885c9', 'test', 'rcx, rcx'),
    0x149a6: ('0f8486000000', 'je', '0x180014a32'),
    0x149ac: ('488b01', 'mov', 'rax, qword ptr [rcx]'),
    0x149af: ('4c8d442438', 'lea', 'r8, [rsp + 0x38]'),
    0x149b4: ('488d542430', 'lea', 'rdx, [rsp + 0x30]'),
    0x149b9: ('488b80f8010000', 'mov', 'rax, qword ptr [rax + 0x1f8]'),
    0x149c0: ('ff15622a0c00', 'call', 'qword ptr [rip + 0xc2a62]'),
    0x149c6: ('90', 'nop', ''),
    0x149c7: ('488bcf', 'mov', 'rcx, rdi'),
    0x149ca: ('e869f30800', 'call', '0x1800a3d38'),
    0x149cf: ('488b03', 'mov', 'rax, qword ptr [rbx]'),
    0x149d2: ('4c8b442430', 'mov', 'r8, qword ptr [rsp + 0x30]'),
    0x149d7: ('488d15624d0c00', 'lea', 'rdx, [rip + 0xc4d62]'),
    0x149de: ('488bcb', 'mov', 'rcx, rbx'),
    0x149e1: ('488b4038', 'mov', 'rax, qword ptr [rax + 0x38]'),
    0x149e5: ('ff153d2a0c00', 'call', 'qword ptr [rip + 0xc2a3d]'),
    0x149eb: ('488b03', 'mov', 'rax, qword ptr [rbx]'),
    0x149ee: ('41b828000000', 'mov', 'r8d, 0x28'),
    0x149f4: ('488d15514d0c00', 'lea', 'rdx, [rip + 0xc4d51]'),
    0x149fb: ('488bcb', 'mov', 'rcx, rbx'),
    0x149fe: ('488b4018', 'mov', 'rax, qword ptr [rax + 0x18]'),
    0x14a02: ('ff15202a0c00', 'call', 'qword ptr [rip + 0xc2a20]'),
    0x14a08: ('488b03', 'mov', 'rax, qword ptr [rbx]'),
    0x14a0b: ('4533c0', 'xor', 'r8d, r8d'),
    0x14a0e: ('488d153b4d0c00', 'lea', 'rdx, [rip + 0xc4d3b]'),
    0x14a15: ('488bcb', 'mov', 'rcx, rbx'),
    0x14a18: ('488b4018', 'mov', 'rax, qword ptr [rax + 0x18]'),
    0x14a1c: ('ff15062a0c00', 'call', 'qword ptr [rip + 0xc2a06]'),
    0x14a22: ('b801000000', 'mov', 'eax, 1'),
    0x14a27: ('488b5c2448', 'mov', 'rbx, qword ptr [rsp + 0x48]'),
    0x14a2c: ('4883c420', 'add', 'rsp, 0x20'),
    0x14a30: ('5f', 'pop', 'rdi'),
    0x14a31: ('c3', 'ret', ''),
    0x14a32: ('4c8d0dd74c0c00', 'lea', 'r9, [rip + 0xc4cd7]'),
    0x14a39: ('4c8d05c04c0c00', 'lea', 'r8, [rip + 0xc4cc0]'),
    0x14a40: ('ba04010000', 'mov', 'edx, 0x104'),
    0x14a45: ('488d0d444b0c00', 'lea', 'rcx, [rip + 0xc4b44]'),
    0x14a4c: ('e8df94ffff', 'call', '0x18000df30'),
    0x14a51: ('90', 'nop', ''),
    0x14a52: ('488bcf', 'mov', 'rcx, rdi'),
    0x14a55: ('e8def20800', 'call', '0x1800a3d38'),
    0x14a5a: ('b80700d0ba', 'mov', 'eax, 0xbad00007'),
    0x14a5f: ('488b5c2448', 'mov', 'rbx, qword ptr [rsp + 0x48]'),
    0x14a64: ('4883c420', 'add', 'rsp, 0x20'),
    0x14a68: ('5f', 'pop', 'rdi'),
    0x14a69: ('c3', 'ret', ''),
    0x14a6a: ('8bc8', 'mov', 'ecx, eax'),
    0x14a6c: ('e8d3f30800', 'call', '0x1800a3e44'),
    0x14a71: ('cc', 'int3', ''),
}


CREATE_ANCHORS = {
    0x17b4d: ('4c894c2470', 'mov', 'qword ptr [rsp + 0x70], r9'),
    0x17b52: ('498bd8', 'mov', 'rbx, r8'),
    0x17b5a: ('4c8bf1', 'mov', 'r14, rcx'),
    0x17b5d: ('33ff', 'xor', 'edi, edi'),
    0x17b5f: ('40387918', 'cmp', 'byte ptr [rcx + 0x18], dil'),
    0x17b63: ('750a', 'jne', '0x180017b6f'),
    0x17b65: ('b80400d0ba', 'mov', 'eax, 0xbad00004'),
    0x17b6a: ('e9ab050000', 'jmp', '0x18001811a'),
    0x17b6f: ('488b09', 'mov', 'rcx, qword ptr [rcx]'),
    0x17b72: ('488b01', 'mov', 'rax, qword ptr [rcx]'),
    0x17b75: ('488bd3', 'mov', 'rdx, rbx'),
    0x17b78: ('488b80c8010000', 'mov', 'rax, qword ptr [rax + 0x1c8]'),
    0x17b7f: ('ff15a3f80b00', 'call', 'qword ptr [rip + 0xbf8a3]'),
    0x17b85: ('498b0e', 'mov', 'rcx, qword ptr [r14]'),
    0x17b88: ('4885c9', 'test', 'rcx, rcx'),
    0x17b8b: ('7469', 'je', '0x180017bf6'),
    0x17b8d: ('488b01', 'mov', 'rax, qword ptr [rcx]'),
    0x17b90: ('488b4038', 'mov', 'rax, qword ptr [rax + 0x38]'),
    0x17b94: ('ff158ef80b00', 'call', 'qword ptr [rip + 0xbf88e]'),
    0x17b9a: ('8d88c0feffff', 'lea', 'ecx, [rax - 0x140]'),
    0x17ba0: ('83f970', 'cmp', 'ecx, 0x70'),
    0x17ba3: ('774a', 'ja', '0x180017bef'),
    0x17ba5: ('4863c9', 'movsxd', 'rcx, ecx'),
    0x17ba8: ('4c8d055184feff', 'lea', 'r8, [rip - 0x17baf]'),
    0x17baf: ('410fb68c0860810100', 'movzx', 'ecx, byte ptr [r8 + rcx + 0x18160]'),
    0x17bb8: ('418b948840810100', 'mov', 'edx, dword ptr [r8 + rcx*4 + 0x18140]'),
    0x17bc0: ('4903d0', 'add', 'rdx, r8'),
    0x17bc3: ('ffe2', 'jmp', 'rdx'),
    0x17bc5: ('bf40010000', 'mov', 'edi, 0x140'),
    0x17bca: ('eb2a', 'jmp', '0x180017bf6'),
    0x17be8: ('bfa0010000', 'mov', 'edi, 0x1a0'),
    0x17bed: ('eb07', 'jmp', '0x180017bf6'),
    0x17bef: ('3d40010000', 'cmp', 'eax, 0x140'),
    0x17bf4: ('7d35', 'jge', '0x180017c2b'),
    0x17bf6: ('c7442428b0010000', 'mov', 'dword ptr [rsp + 0x28], 0x1b0'),
    0x17bfe: ('897c2420', 'mov', 'dword ptr [rsp + 0x20], edi'),
    0x17c1c: ('e80f63ffff', 'call', '0x18000df30'),
    0x17c21: ('b80100d0ba', 'mov', 'eax, 0xbad00001'),
    0x17c26: ('e9ef040000', 'jmp', '0x18001811a'),
    0x17c2b: ('488d4db0', 'lea', 'rcx, [rbp - 0x50]'),
    0x17c2f: ('e8dce4ffff', 'call', '0x180016110'),
    0x17c35: ('488b03', 'mov', 'rax, qword ptr [rbx]'),
    0x17c38: ('4c8d45d8', 'lea', 'r8, [rbp - 0x28]'),
    0x17c3c: ('488d15ed280c00', 'lea', 'rdx, [rip + 0xc28ed]'),
    0x17c43: ('488bcb', 'mov', 'rcx, rbx'),
    0x17c46: ('488b4060', 'mov', 'rax, qword ptr [rax + 0x60]'),
    0x17c4a: ('ff15d8f70b00', 'call', 'qword ptr [rip + 0xbf7d8]'),
    0x17c50: ('8b4dd8', 'mov', 'ecx, dword ptr [rbp - 0x28]'),
    0x17c53: ('41bf01000000', 'mov', 'r15d, 1'),
    0x17c59: ('413bc7', 'cmp', 'eax, r15d'),
    0x17c5c: ('410f45cf', 'cmovne', 'ecx, r15d'),
    0x17c60: ('894dd8', 'mov', 'dword ptr [rbp - 0x28], ecx'),
    0x17c63: ('488b03', 'mov', 'rax, qword ptr [rbx]'),
    0x17c66: ('4c8d45dc', 'lea', 'r8, [rbp - 0x24]'),
    0x17c6a: ('488d15d7280c00', 'lea', 'rdx, [rip + 0xc28d7]'),
    0x17c71: ('488bcb', 'mov', 'rcx, rbx'),
    0x17c74: ('488b4060', 'mov', 'rax, qword ptr [rax + 0x60]'),
    0x17c78: ('ff15aaf70b00', 'call', 'qword ptr [rip + 0xbf7aa]'),
    0x17c7e: ('8b4ddc', 'mov', 'ecx, dword ptr [rbp - 0x24]'),
    0x17c81: ('413bc7', 'cmp', 'eax, r15d'),
    0x17c84: ('410f45cf', 'cmovne', 'ecx, r15d'),
    0x17c88: ('894ddc', 'mov', 'dword ptr [rbp - 0x24], ecx'),
    0x17c8b: ('488b03', 'mov', 'rax, qword ptr [rbx]'),
    0x17c8e: ('4c8d45b0', 'lea', 'r8, [rbp - 0x50]'),
    0x17c92: ('488d15c7280c00', 'lea', 'rdx, [rip + 0xc28c7]'),
    0x17c99: ('488bcb', 'mov', 'rcx, rbx'),
    0x17c9c: ('488b4060', 'mov', 'rax, qword ptr [rax + 0x60]'),
    0x17ca0: ('ff1582f70b00', 'call', 'qword ptr [rip + 0xbf782]'),
    0x17ca6: ('8b4db0', 'mov', 'ecx, dword ptr [rbp - 0x50]'),
    0x17ca9: ('413bc7', 'cmp', 'eax, r15d'),
    0x17cac: ('0f45cf', 'cmovne', 'ecx, edi'),
    0x17caf: ('894db0', 'mov', 'dword ptr [rbp - 0x50], ecx'),
    0x17cb2: ('488b03', 'mov', 'rax, qword ptr [rbx]'),
    0x17cb5: ('4c8d45b4', 'lea', 'r8, [rbp - 0x4c]'),
    0x17cb9: ('488d15b0280c00', 'lea', 'rdx, [rip + 0xc28b0]'),
    0x17cc0: ('488bcb', 'mov', 'rcx, rbx'),
    0x17cc3: ('488b4060', 'mov', 'rax, qword ptr [rax + 0x60]'),
    0x17cc7: ('ff155bf70b00', 'call', 'qword ptr [rip + 0xbf75b]'),
    0x17ccd: ('8b55b4', 'mov', 'edx, dword ptr [rbp - 0x4c]'),
    0x17cd0: ('413bc7', 'cmp', 'eax, r15d'),
    0x17cd3: ('0f45d7', 'cmovne', 'edx, edi'),
    0x17cd6: ('8955b4', 'mov', 'dword ptr [rbp - 0x4c], edx'),
    0x17cf6: ('488b03', 'mov', 'rax, qword ptr [rbx]'),
    0x17cf9: ('4c8d45e0', 'lea', 'r8, [rbp - 0x20]'),
    0x17cfd: ('488d157c280c00', 'lea', 'rdx, [rip + 0xc287c]'),
    0x17d04: ('488bcb', 'mov', 'rcx, rbx'),
    0x17d07: ('488b4058', 'mov', 'rax, qword ptr [rax + 0x58]'),
    0x17d0b: ('ff1517f70b00', 'call', 'qword ptr [rip + 0xbf717]'),
    0x17d11: ('8b4de0', 'mov', 'ecx, dword ptr [rbp - 0x20]'),
    0x17d14: ('413bc7', 'cmp', 'eax, r15d'),
    0x17d17: ('0f45cf', 'cmovne', 'ecx, edi'),
    0x17d1a: ('0fbaf11f', 'btr', 'ecx, 0x1f'),
    0x17d1e: ('894de0', 'mov', 'dword ptr [rbp - 0x20], ecx'),
    0x17d5c: ('49837e1000', 'cmp', 'qword ptr [r14 + 0x10], 0'),
    0x17d61: ('7545', 'jne', '0x180017da8'),
    0x17d63: ('498b0e', 'mov', 'rcx, qword ptr [r14]'),
    0x17d66: ('488b01', 'mov', 'rax, qword ptr [rcx]'),
    0x17d69: ('498d5610', 'lea', 'rdx, [r14 + 0x10]'),
    0x17d6d: ('488b4018', 'mov', 'rax, qword ptr [rax + 0x18]'),
    0x17d71: ('ff15b1f60b00', 'call', 'qword ptr [rip + 0xbf6b1]'),
    0x17d77: ('85c0', 'test', 'eax, eax'),
    0x17d79: ('742d', 'je', '0x180017da8'),
    0x17d9e: ('bb0200d0ba', 'mov', 'ebx, 0xbad00002'),
    0x17da3: ('e967030000', 'jmp', '0x18001810f'),
    0x17f29: ('e8d2900000', 'call', '0x180021000'),
    0x17f2e: ('8bd8', 'mov', 'ebx, eax'),
    0x17f30: ('250000f0ff', 'and', 'eax, 0xfff00000'),
    0x17f35: ('3d0000d0ba', 'cmp', 'eax, 0xbad00000'),
    0x17f3a: ('7532', 'jne', '0x180017f6e'),
    0x17f69: ('e963010000', 'jmp', '0x1800180d1'),
    0x17f6e: ('41807e1900', 'cmp', 'byte ptr [r14 + 0x19], 0'),
    0x17f73: ('7553', 'jne', '0x180017fc8'),
    0x17f7b: ('e8205c0000', 'call', '0x18001dba0'),
    0x17f80: ('8bd8', 'mov', 'ebx, eax'),
    0x17f82: ('250000f0ff', 'and', 'eax, 0xfff00000'),
    0x17f87: ('3d0000d0ba', 'cmp', 'eax, 0xbad00000'),
    0x17f8c: ('7535', 'jne', '0x180017fc3'),
    0x17fbe: ('e90e010000', 'jmp', '0x1800180d1'),
    0x17fc3: ('41c6461901', 'mov', 'byte ptr [r14 + 0x19], 1'),
    0x1800e: ('e89df3ffff', 'call', '0x1800173b0'),
    0x18013: ('8bd8', 'mov', 'ebx, eax'),
    0x18015: ('250000f0ff', 'and', 'eax, 0xfff00000'),
    0x1801a: ('3d0000d0ba', 'cmp', 'eax, 0xbad00000'),
    0x1801f: ('0f84ac000000', 'je', '0x1800180d1'),
    0x18066: ('488b442468', 'mov', 'rax, qword ptr [rsp + 0x68]'),
    0x1806b: ('488b4c2470', 'mov', 'rcx, qword ptr [rsp + 0x70]'),
    0x18070: ('488901', 'mov', 'qword ptr [rcx], rax'),
    0x180cc: ('bb01000000', 'mov', 'ebx, 1'),
    0x18118: ('8bc3', 'mov', 'eax, ebx'),
    0x18124: ('e877d80800', 'call', '0x1800a59a0'),
    0x1813c: ('c3', 'ret', ''),
    0x1813d: ('0f1f00', 'nop', 'dword ptr [rax]'),
}


RECT_ANCHORS = {
    0x17a39: ('33c0', 'xor', 'eax, eax'),
    0x17a3b: ('488bfa', 'mov', 'rdi, rdx'),
    0x17a3e: ('498900', 'mov', 'qword ptr [r8], rax'),
    0x17a41: ('498bd8', 'mov', 'rbx, r8'),
    0x17a44: ('49894008', 'mov', 'qword ptr [r8 + 8], rax'),
    0x17a48: ('49894010', 'mov', 'qword ptr [r8 + 0x10], rax'),
    0x17a4c: ('488b12', 'mov', 'rdx, qword ptr [rdx]'),
    0x17a4f: ('4885c9', 'test', 'rcx, rcx'),
    0x17a52: ('0f849d000000', 'je', '0x180017af5'),
    0x17a58: ('4885d2', 'test', 'rdx, rdx'),
    0x17a5b: ('0f8494000000', 'je', '0x180017af5'),
    0x17a61: ('4889442448', 'mov', 'qword ptr [rsp + 0x48], rax'),
    0x17a66: ('4c8d442420', 'lea', 'r8, [rsp + 0x20]'),
    0x17a6b: ('89442450', 'mov', 'dword ptr [rsp + 0x50], eax'),
    0x17a6f: ('488b01', 'mov', 'rax, qword ptr [rcx]'),
    0x17a72: ('c644245401', 'mov', 'byte ptr [rsp + 0x54], 1'),
    0x17a77: ('488b8090010000', 'mov', 'rax, qword ptr [rax + 0x190]'),
    0x17a7e: ('ff15a4f90b00', 'call', 'qword ptr [rip + 0xbf9a4]'),
    0x17a84: ('85c0', 'test', 'eax, eax'),
    0x17a86: ('756d', 'jne', '0x180017af5'),
    0x17a88: ('8b542420', 'mov', 'edx, dword ptr [rsp + 0x20]'),
    0x17a8c: ('8b4c2424', 'mov', 'ecx, dword ptr [rsp + 0x24]'),
    0x17a90: ('895310', 'mov', 'dword ptr [rbx + 0x10], edx'),
    0x17a93: ('894b14', 'mov', 'dword ptr [rbx + 0x14], ecx'),
    0x17a96: ('85d2', 'test', 'edx, edx'),
    0x17a98: ('745b', 'je', '0x180017af5'),
    0x17a9a: ('85c9', 'test', 'ecx, ecx'),
    0x17a9c: ('7457', 'je', '0x180017af5'),
    0x17a9e: ('448b5708', 'mov', 'r10d, dword ptr [rdi + 8]'),
    0x17aa2: ('448bda', 'mov', 'r11d, edx'),
    0x17aa5: ('448913', 'mov', 'dword ptr [rbx], r10d'),
    0x17aa8: ('448bc1', 'mov', 'r8d, ecx'),
    0x17aab: ('448b4f0c', 'mov', 'r9d, dword ptr [rdi + 0xc]'),
    0x17aaf: ('44894b04', 'mov', 'dword ptr [rbx + 4], r9d'),
    0x17ab3: ('8b4710', 'mov', 'eax, dword ptr [rdi + 0x10]'),
    0x17ab6: ('85c0', 'test', 'eax, eax'),
    0x17ab8: ('440f45d8', 'cmovne', 'r11d, eax'),
    0x17abc: ('44895b08', 'mov', 'dword ptr [rbx + 8], r11d'),
    0x17ac0: ('8b4714', 'mov', 'eax, dword ptr [rdi + 0x14]'),
    0x17ac3: ('85c0', 'test', 'eax, eax'),
    0x17ac5: ('440f45c0', 'cmovne', 'r8d, eax'),
    0x17ac9: ('4489430c', 'mov', 'dword ptr [rbx + 0xc], r8d'),
    0x17acd: ('4585db', 'test', 'r11d, r11d'),
    0x17ad0: ('7423', 'je', '0x180017af5'),
    0x17ad2: ('4585c0', 'test', 'r8d, r8d'),
    0x17ad5: ('741e', 'je', '0x180017af5'),
    0x17ad7: ('443bd2', 'cmp', 'r10d, edx'),
    0x17ada: ('7719', 'ja', '0x180017af5'),
    0x17adc: ('443bc9', 'cmp', 'r9d, ecx'),
    0x17adf: ('7714', 'ja', '0x180017af5'),
    0x17ae1: ('412bd2', 'sub', 'edx, r10d'),
    0x17ae4: ('443bda', 'cmp', 'r11d, edx'),
    0x17ae7: ('770c', 'ja', '0x180017af5'),
    0x17ae9: ('412bc9', 'sub', 'ecx, r9d'),
    0x17aec: ('443bc1', 'cmp', 'r8d, ecx'),
    0x17aef: ('7704', 'ja', '0x180017af5'),
    0x17af1: ('b001', 'mov', 'al, 1'),
    0x17af3: ('eb02', 'jmp', '0x180017af7'),
    0x17af5: ('32c0', 'xor', 'al, al'),
}

def digest(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def reference_bytes(source: Path) -> bytes:
    """Read the explicit historical source and verify its fixed content identity."""
    raw = source.read_bytes()
    if hashlib.sha256(raw).hexdigest() != REFERENCE_SHA:
        raise ValueError('historical NR reference identity mismatch')
    return raw


def shutdown_identity_contract(pe, code: dict) -> dict:
    """Bind bounded token/selection observations to bytes and fresh decode only.

    No target calls are made. Vtable offsets and argument registers are observed;
    no method names, hardware eligibility or complete session-map ABI are inferred.
    """
    guid = pe.get_data(0xd8d60, 16)
    if guid.hex() != '11d7e043ea7b684ea077e9d11a7625f3':
        raise ValueError('historical NR shutdown identity GUID bytes changed')
    functions = []
    for name, (lo, hi, unwind, unwind_bytes) in SHUTDOWN_FUNCTIONS.items():
        span = code.get(name, {})
        if (span.get('lo'), span.get('hi')) != (hex(lo), hex(hi)):
            raise ValueError('historical NR shutdown identity decoded boundary changed: '+name)
        entries = [entry.struct for entry in pe.DIRECTORY_ENTRY_EXCEPTION
                   if entry.struct.BeginAddress == lo]
        if (len(entries) != 1 or entries[0].EndAddress != hi or
                entries[0].UnwindData != unwind):
            raise ValueError('historical NR shutdown identity pdata/unwind changed: '+name)
        pdata_rva = pe.get_rva_from_offset(entries[0].get_file_offset())
        pdata_bytes = pe.get_data(pdata_rva, 12)
        if pdata_bytes != struct.pack('<III', lo, hi, unwind):
            raise ValueError('historical NR shutdown identity pdata bytes changed: '+name)
        actual_unwind = pe.get_data(unwind, len(bytes.fromhex(unwind_bytes)))
        if actual_unwind.hex() != unwind_bytes:
            raise ValueError('historical NR shutdown identity unwind bytes changed: '+name)
        functions.append(dict(name=name, lo=hex(lo), hi=hex(hi), pdata_rva=hex(pdata_rva),
                              pdata_bytes=pdata_bytes.hex(), unwind_rva=hex(unwind),
                              unwind_bytes=actual_unwind.hex()))
    for name in ('init-common', 'shutdown-common'):
        lo, hi = RANGES[name]
        if (code.get(name, {}).get('lo'), code.get(name, {}).get('hi')) != (hex(lo), hex(hi)):
            raise ValueError('historical NR shutdown identity common boundary changed: '+name)
    rows = {int(row['rva'], 16): row for span in code.values() for row in span['rows']}
    anchors = []
    for rva, (raw, mnemonic, operands) in SHUTDOWN_ANCHORS.items():
        expected = dict(rva=hex(rva), bytes=raw, mnemonic=mnemonic, operands=operands)
        if rows.get(rva) != expected or pe.get_data(rva, len(bytes.fromhex(raw))).hex() != raw:
            raise ValueError('historical NR shutdown identity instruction changed at '+hex(rva))
        anchors.append(dict(expected))
    normal_returns = [row['rva'] for name in SHUTDOWN_FUNCTIONS
                      if name.startswith('shutdown-helper-')
                      for row in code[name]['rows'] if row['mnemonic'] == 'ret']
    if normal_returns != ['0x1b20a']:
        raise ValueError('historical NR shutdown identity helper normal return changed')

    def path(name: str, rvas: tuple[int, ...], **observations) -> dict:
        if any(rva not in SHUTDOWN_ANCHORS for rva in rvas):
            raise ValueError('unbound shutdown identity path anchor: '+name)
        return dict(name=name, anchor_rvas=[hex(rva) for rva in rvas], **observations)

    paths = [
        path('null-object-token', (0xe8c2, 0xe8c5, 0xe8c7, 0xe8ce),
             result=0, target_vtable_call=False),
        path('read-vtable-arguments', (0xe8b6, 0xe8cf, 0xe8d2, 0xe8d7, 0xe8dc,
                                      0xe8e4, 0xe8eb, 0xe8ef),
             vtable_offset='0x18', size_storage_bits=32, requested_size=8,
             size_slot='rsp+0x40', output_slot='rsp+0x48', output_bytes=8,
             initial_output=0, guid_rva='0xd8d60', dispatch='CFG indirect RAX target',
             method_name_authenticated=False),
        path('read-nonnegative-result', (0xe8ef, 0xe8f5, 0xe8f7, 0xe931, 0xe93b),
             condition='signed EAX >= 0', result='8-byte stack output value',
             returned_size_rechecked=False, value_nonzero_required=False),
        path('read-negative-generates-token', (0xe8f5, 0xe8f7, 0xe8f9, 0xe8fe,
                                               0xe907, 0xe90f),
             condition='signed EAX < 0', global_counter_rva='0x110018',
             atomic_global_increment=1, local_rax_old_value_increment=1,
             result='old global value + 1; local INC does not increment global again'),
        path('write-vtable-result-unchecked', (0xe90a, 0xe90f, 0xe914, 0xe91b, 0xe91e,
                                               0xe924, 0xe927, 0xe92b, 0xe931, 0xe93b),
             vtable_offset='0x20', size=8, value_address='rsp+0x48', guid_rva='0xd8d60',
             dispatch='CFG indirect RAX target', write_result_checked=False,
             method_name_authenticated=False, result='generated stack token regardless of write EAX'),
        path('init-token-address-handoff', (0x13dbc, 0x13dbf, 0x13dc4, 0x13dc9,
                                            0x13dce, 0x13dd3, 0x13dda),
             token_slot='rsp+0x30', callee_rva='0x14400', argument='R8 = token address',
             callee_body_decoded=False, map_insertion_abi_authenticated=False,
             full_session_map_chain_proven=False),
        path('shutdown-original-object-token', (0x1422b, 0x14250, 0x14253, 0x14258),
             input='original RCX saved in R12', token_register='R13'),
        path('null-selects-shown-container', (0x14280, 0x14284, 0x14287, 0x14299),
             condition='original object is null', token_filter=False,
             scope='entries in the shown container only; not all process objects'),
        path('nonnull-token-match', (0x14280, 0x14284, 0x14287, 0x14289, 0x1428c, 0x14299),
             condition='original object nonnull and qword [node+0x10] == R13 token',
             compared_bytes=8, raw_pointer_comparison=False),
        path('nonnull-token-mismatch-skips', (0x14289, 0x1428c, 0x1428e, 0x14291,
                                              0x14294, 0x143a7, 0x143aa),
             condition='qword [node+0x10] != R13 token', skipped_counter='R15D increment',
             next_node='qword [node]', selected_cleanup_called=False),
        path('cleanup-preserves-helper-result', (0x23dcd, 0x23dd2, 0x23dd6, 0x23dd9,
                                                 0x23de0, 0x23de5, 0x23de9, 0x23deb,
                                                 0x23dee, 0x23df3, 0x23e00, 0x23e0a,
                                                 0x23e12, 0x23e22, 0x23e3c, 0x23e42,
                                                 0x23e4f, 0x23e56),
             initial_edi=1, helper_rva='0x1b090', helper_result_saved='EDI = EAX',
             cleanup_continues_after_helper=True, cleared_object_offsets=['0x40', '0x70'],
             return_value='saved EDI, not later free/call EAX',
             deep_calls_and_exceptions='UNKNOWN'),
        path('helper-unique-normal-return-literal-one', (0x1b0bd, 0x1b1d1, 0x1b1da,
                                                        0x1b1f4, 0x1b1f9, 0x1b1fe,
                                                        0x1b202, 0x1b206, 0x1b208, 0x1b20a),
             normal_return_sites=normal_returns, normal_eax=1,
             deep_calls_and_exceptions='UNKNOWN', runtime_failure_reproduced=False),
        path('common-non-one-conditional-only', (0x14299, 0x1429d, 0x142a2, 0x142a5,
                                                 0x142a7, 0x142aa),
             condition='cleanup EAX != 1; if DIL != 0 then EBP = EAX',
             runtime_failure_reachability_proven=False, runtime_failure_reproduced=False,
             shown_helper_normal_eax=1),
        path('common-loop-skips-and-return', (0x14265, 0x14268, 0x1424a, 0x1424d,
                                               0x14357, 0x1437b, 0x1437f, 0x143a7,
                                               0x143aa, 0x143b0, 0x143ba, 0x143bc,
                                               0x143c3, 0x143c8, 0x143cf, 0x143d5,
                                               0x143d8, 0x143dd, 0x143f3),
             normal_result='saved EBP, initialized to 1 after zero lock result',
             skipped_nonzero='branch to unlock at 0x143d5, omitting shown final cleanup calls',
             selected_entry_removed_in_shown_path=True),
        path('common-lock-nonzero', (0x14242, 0x14244, 0x143f4, 0x143f6, 0x143fb),
             condition='lock helper EAX != 0', shown_path='call 0xa3e44 then int3',
             runtime_reachability_proven=False),
    ]
    return dict(schema=1, scope='bounded historical static shutdown object token and selection',
                guids=[dict(rva='0xd8d60', bytes=guid.hex(), size=16)],
                anchors=anchors, functions=functions, paths=paths,
                target_executed=False, payload_loaded=False, gpu_tested=False,
                f7_selected_payload=False, abi_authenticated=False, hardware_support_proven=False,
                gpu_fence_identity=False, raw_pointer_identity=False,
                stable_guid_authentication=False, same_object_future_key_stability_proven=False,
                token_instability_condition='write has no effect and a later read fails again',
                token_instability_reproduced=False, actual_failure_reproduced=False,
                full_session_map_chain_proven=False, map_insertion_abi_authenticated=False,
                caller_guard_bypassed=False, deep_dynamic_calls='UNKNOWN',
                exceptions='UNKNOWN', support='UNKNOWN')


def stats_contract(pe, code: dict) -> dict:
    """Bind this callback body only; registry and dynamic callee semantics stay unknown."""
    name = 'published-stats-callback'
    lo, hi, unwind = 0x14920, 0x14a72, 0xff2a4
    unwind_bytes = '110a04000a3409000a320670cc740a00b8f20f00'
    span = code.get(name, {})
    if (span.get('lo'), span.get('hi')) != (hex(lo), hex(hi)):
        raise ValueError('historical NR stats decoded boundary changed')
    entries = [entry.struct for entry in pe.DIRECTORY_ENTRY_EXCEPTION
               if entry.struct.BeginAddress == lo]
    if (len(entries) != 1 or entries[0].EndAddress != hi or entries[0].UnwindData != unwind):
        raise ValueError('historical NR stats pdata/unwind changed')
    pdata_rva = pe.get_rva_from_offset(entries[0].get_file_offset())
    pdata_bytes = pe.get_data(pdata_rva, 12)
    if pdata_bytes != struct.pack('<III', lo, hi, unwind):
        raise ValueError('historical NR stats pdata bytes changed')
    actual_unwind = pe.get_data(unwind, len(bytes.fromhex(unwind_bytes)))
    if actual_unwind.hex() != unwind_bytes:
        raise ValueError('historical NR stats unwind bytes changed')
    functions = [dict(name=name, lo=hex(lo), hi=hex(hi), pdata_rva=hex(pdata_rva),
                      pdata_bytes=pdata_bytes.hex(), unwind_rva=hex(unwind),
                      unwind_bytes=actual_unwind.hex())]
    keys = []
    for rva, key in ((0xd9740, 'SizeInBytes'), (0xd974c, '#D'), (0xd9750, '#E')):
        raw = pe.get_data(rva, len(key)+1)
        if raw != key.encode('ascii')+b'\0':
            raise ValueError('historical NR stats key bytes changed at '+hex(rva))
        keys.append(dict(rva=hex(rva), bytes=raw.hex(), name=key))
    anchors = [dict(rva=hex(rva), bytes=raw, mnemonic=mnemonic, operands=operands)
               for rva, (raw, mnemonic, operands) in STATS_ANCHORS.items()]
    # Check the complete sequence, including NOPs, epilogues and abnormal tail.
    # A dictionary of selected rows alone could hide inserted result checks.
    if span.get('rows') != anchors or any(
            pe.get_data(rva, len(bytes.fromhex(raw))).hex() != raw
            for rva, (raw, _, _) in STATS_ANCHORS.items()):
        raise ValueError('historical NR stats instruction changed')

    def path(name: str, rvas: tuple[int, ...], **observations) -> dict:
        if any(rva not in STATS_ANCHORS for rva in rvas):
            raise ValueError('unbound stats path anchor: '+name)
        return dict(name=name, anchor_rvas=[hex(rva) for rva in rvas], **observations)

    paths = [
        path('null-parameter', (0x1492a, 0x1492d, 0x14930, 0x1494c, 0x14951, 0x14960),
             condition='RCX parameter object is null', normal_eax='0xbad00005',
             normal_return_requires='shown logging call returns'),
        path('zero-outputs-and-lock', (0x14961, 0x14963, 0x14968, 0x1496d,
                                       0x14974, 0x14979, 0x1497c, 0x14981, 0x14983),
             slots=['rsp+0x30', 'rsp+0x38'], output_bits=64, initial_value=0,
             lock_address_rva='0x4a0d40', lock_helper_rva='0xa3d30',
             lock_role='observational; in-module direct call, not an OS mutex import',
             helper_full_semantics='UNKNOWN'),
        path('lock-nonzero-abnormal-tail', (0x14981, 0x14983, 0x14a6a, 0x14a6c, 0x14a71),
             condition='lock helper EAX != 0', forwarded_argument='ECX = EAX',
             helper_rva='0xa3e44', helper_semantics='UNKNOWN; may throw',
             shown_tail='direct call then int3; no ordinary return shown',
             runtime_reachability_proven=False),
        path('first-record-object-selection', (0x14989, 0x14990, 0x14993, 0x14996,
                                               0x1499c, 0x149a3, 0x149a6),
             global_head_pointer_rva='0x4a0d98', first_node='qword [head]',
             empty_condition='first node == head', object='qword [first node+0x88]',
             missing_destination_rva='0x14a32', iteration_in_shown_body=False,
             device_or_feature_key_selection_in_shown_body=False,
             registry_insertion_and_aggregate_semantics='UNKNOWN'),
        path('object-vtable-call', (0x149ac, 0x149af, 0x149b4, 0x149b9, 0x149c0),
             object_argument='RCX = selected object', vtable_offset='0x1f8',
             first_output_address='RDX = &rsp+0x30', second_output_address='R8 = &rsp+0x38',
             dispatch='indirect RAX target via slot 0xd7428',
             object_interface='UNKNOWN', method_name='UNKNOWN'),
        path('unchecked-result-then-unlock', (0x149c0, 0x149c6, 0x149c7, 0x149ca, 0x149cf),
             returned_value_checked=False, unlock_helper_rva='0xa3d38',
             unlock_argument='RCX = lock address', helper_full_semantics='UNKNOWN',
             normal_continuation_requires='dynamic call and unlock helper return'),
        path('write-first-output', (0x149cf, 0x149d2, 0x149d7, 0x149de, 0x149e1, 0x149e5),
             key='SizeInBytes', key_rva='0xd9740', vtable_offset='0x38',
             value='qword [rsp+0x30]', value_bits=64, receiver='original parameter object',
             second_output_used_in_shown_success_path=False),
        path('write-raw-D', (0x149eb, 0x149ee, 0x149f4, 0x149fb, 0x149fe, 0x14a02),
             key='#D', key_rva='0xd974c', vtable_offset='0x18', value=40,
             value_register='R8D', key_semantics='UNKNOWN'),
        path('write-raw-E', (0x14a08, 0x14a0b, 0x14a0e, 0x14a15, 0x14a18, 0x14a1c),
             key='#E', key_rva='0xd9750', vtable_offset='0x18', value=0,
             value_register='R8D', key_semantics='UNKNOWN'),
        path('literal-success', (0x149e5, 0x149eb, 0x14a02, 0x14a08,
                                 0x14a1c, 0x14a22, 0x14a31),
             normal_eax=1, setter_results_checked=False,
             normal_return_requires='all shown calls return', gpu_completion_proven=False),
        path('missing-record-or-object', (0x14996, 0x149a6, 0x14a32, 0x14a4c,
                                          0x14a51, 0x14a52, 0x14a55, 0x14a5a, 0x14a69),
             condition='first node == head or selected object is null',
             unlock_helper_rva='0xa3d38', normal_eax='0xbad00007',
             normal_return_requires='shown logging and unlock calls return'),
    ]
    return dict(schema=1, scope='bounded historical static GetStats callback body only',
                callback_rva=hex(lo), keys=keys, anchors=anchors, functions=functions, paths=paths,
                observed_input='RCX parameter object; no additional input argument used in shown body',
                registry_insertion_and_aggregate_semantics='UNKNOWN', object_interface='UNKNOWN',
                slot_1f8_method_name='UNKNOWN', deep_dynamic_calls='UNKNOWN', exceptions='UNKNOWN',
                observed_role='stats output retrieval and parameter writeback',
                gpu_completion_proven=False, gpu_fence_identity=False,
                target_executed=False, callback_executed=False, payload_loaded=False, gpu_tested=False,
                f7_selected_payload=False, abi_authenticated=False, hardware_support_proven=False,
                support='UNKNOWN')


def create_contract(pe, code: dict) -> dict:
    """Observe direct reads/selector in one historical body, not a nested Create ABI."""
    name, lo, code_hi, parent_hi, unwind = 'create-common-body', 0x17b20, 0x18140, 0x181d1, 0xff7bc
    span = code.get(name, {})
    if (span.get('lo'), span.get('hi')) != (hex(lo), hex(code_hi)):
        raise ValueError('historical NR create decoded boundary changed')
    entries = [entry.struct for entry in pe.DIRECTORY_ENTRY_EXCEPTION if entry.struct.BeginAddress == lo]
    if (len(entries) != 1 or entries[0].EndAddress != parent_hi or entries[0].UnwindData != unwind):
        raise ValueError('historical NR create parent pdata/unwind changed')
    pdata_rva = pe.get_rva_from_offset(entries[0].get_file_offset())
    pdata_bytes = pe.get_data(pdata_rva, 12)
    if pdata_bytes != struct.pack('<III', lo, parent_hi, unwind):
        raise ValueError('historical NR create pdata bytes changed')
    # Header + ten unwind-code slots + handler RVA. Language-specific data and
    # exception-handler semantics are not decoded or certified by this record.
    unwind_bytes = '192d0a001c0169000df00be009d007c0057004600330025004590a00'
    if pe.get_data(unwind, len(bytes.fromhex(unwind_bytes))).hex() != unwind_bytes:
        raise ValueError('historical NR create unwind bytes changed')
    functions = [dict(name=name, lo=hex(lo), hi=hex(parent_hi), code_hi=hex(code_hi),
                      pdata_rva=hex(pdata_rva), pdata_bytes=pdata_bytes.hex(),
                      unwind_rva=hex(unwind), unwind_bytes=unwind_bytes,
                      unwind_scope='header, unwind codes and handler RVA only',
                      language_specific_handler_data_decoded=False)]
    raw_code = pe.get_data(lo, code_hi-lo)
    code_sha = hashlib.sha256(raw_code).hexdigest()
    if code_sha != 'a9eb49104e3630d71e02554eed5fd14c8b6589b1f64aa23d9fe4526ccbf973fd':
        raise ValueError('historical NR create code bytes changed')
    rows = span.get('rows', [])
    if (span.get('instructions') != len(rows) or len(rows) != 379 or
            span.get('sha256') != code_sha or
            ''.join(row['bytes'] for row in rows) != raw_code.hex()):
        raise ValueError('historical NR create decoded code changed')
    by_rva = {int(row['rva'], 16): row for row in rows}
    anchors = []
    for rva, (raw, mnemonic, operands) in CREATE_ANCHORS.items():
        expected = dict(rva=hex(rva), bytes=raw, mnemonic=mnemonic, operands=operands)
        if by_rva.get(rva) != expected or pe.get_data(rva, len(bytes.fromhex(raw))).hex() != raw:
            raise ValueError('historical NR create instruction changed at '+hex(rva))
        anchors.append(expected)
    keys = []
    for rva, key in ((0xda530, 'CreationNodeMask'), (0xda548, 'VisibilityNodeMask'),
                     (0xda560, 'DLSSNR.Width'), (0xda570, 'DLSSNR.Height'),
                     (0xda580, 'DLSSNR.Hint.Render.Preset')):
        raw = pe.get_data(rva, len(key)+1)
        if raw != key.encode('ascii')+b'\0':
            raise ValueError('historical NR create key bytes changed at '+hex(rva))
        keys.append(dict(rva=hex(rva), name=key, bytes=raw.hex()))
    tables = []
    table_specs = (
        ('selector-targets', 0x18140, 4,
         'c57b01002b7c01002b7c01002b7c01002b7c0100e87b01002b7c0100ef7b0100'),
        ('selector-indices', 0x18160, 1,
         '00070707070707070707070707070707'
         '00070707070707070707070707070707'
         '01070707070707070707070707070707'
         '02070707070707070707070707070707'
         '03070707070707070707070707070707'
         '04070707070707070707070707070707'
         '05070707070707070707070707070707' '06'),
    )
    for table_name, rva, entry_bytes, expected in table_specs:
        raw = pe.get_data(rva, len(bytes.fromhex(expected)))
        if raw.hex() != expected:
            raise ValueError('historical NR create table bytes changed: '+table_name)
        values = list(struct.unpack('<8I', raw)) if entry_bytes == 4 else list(raw)
        tables.append(dict(name=table_name, lo=hex(rva), hi=hex(rva+len(raw)), bytes=raw.hex(),
                           byte_count=len(raw), entry_bytes=entry_bytes, entries=values, decoded_as_code=False))
    targets, indices = (table['entries'] for table in tables)
    selector = dict(input='low 32 bits of EAX from object slot +0x38',
                    table_condition='unsigned ((EAX - 0x140) modulo 2^32) <= 0x70',
                    index_table_rva='0x18160', target_table_rva='0x18140',
                    indexed_cases=[dict(value=hex(0x140+offset), index=index, target_rva=hex(targets[index]))
                                   for offset, index in enumerate(indices) if index != 7],
                    default_index=7, default_target_rva=hex(targets[7]),
                    fallback_condition='signed EAX >= 0x140 reaches 0x17c2b; otherwise 0x17bf6',
                    direct_read_entry_rva='0x17c2b', rejection_entry_rva='0x17bf6',
                    selector_identity='UNKNOWN', hardware_support_proven=False)

    def path(name: str, rvas: tuple[int, ...], **observations) -> dict:
        if any(rva not in CREATE_ANCHORS for rva in rvas):
            raise ValueError('unbound create path anchor: '+name)
        return dict(name=name, anchor_rvas=[hex(rva) for rva in rvas], **observations)

    reads = []
    for key, slot, offset, call, rvas, fallback, value_type in (
        ('CreationNodeMask', 'rbp-0x28', '0x60', 0x17c4a,
         (0x17c35, 0x17c38, 0x17c3c, 0x17c43, 0x17c46, 0x17c4a, 0x17c50, 0x17c53, 0x17c59, 0x17c5c, 0x17c60), 1, 'uint32'),
        ('VisibilityNodeMask', 'rbp-0x24', '0x60', 0x17c78,
         (0x17c63, 0x17c66, 0x17c6a, 0x17c71, 0x17c74, 0x17c78, 0x17c7e, 0x17c81, 0x17c84, 0x17c88), 1, 'uint32'),
        ('DLSSNR.Width', 'rbp-0x50', '0x60', 0x17ca0,
         (0x17c8b, 0x17c8e, 0x17c92, 0x17c99, 0x17c9c, 0x17ca0, 0x17ca6, 0x17ca9, 0x17cac, 0x17caf), 0, 'uint32'),
        ('DLSSNR.Height', 'rbp-0x4c', '0x60', 0x17cc7,
         (0x17cb2, 0x17cb5, 0x17cb9, 0x17cc0, 0x17cc3, 0x17cc7, 0x17ccd, 0x17cd0, 0x17cd3, 0x17cd6), 0, 'uint32'),
        ('DLSSNR.Hint.Render.Preset', 'rbp-0x20', '0x58', 0x17d0b,
         (0x17cf6, 0x17cf9, 0x17cfd, 0x17d04, 0x17d07, 0x17d0b, 0x17d11, 0x17d14, 0x17d17, 0x17d1a, 0x17d1e), 0, 'int32'),
    ):
        reads.append(path(key, rvas, key_rva=next(row['rva'] for row in keys if row['name'] == key),
                          parameter_vtable_offset=offset, call_rva=hex(call), output_slot=slot,
                          value_type=value_type, exact_success_eax=1, otherwise_value=fallback,
                          default_register='R15D = 1' if fallback else 'EDI = 0 at these reads only',
                          postprocess='clear bit 31 via BTR ECX,31' if offset == '0x58' else 'none shown'))
    paths = [
        path('wrapper-ready-byte', (0x17b5a, 0x17b5d, 0x17b5f, 0x17b63, 0x17b65, 0x17b6a),
             condition='byte [original RCX+0x18] == 0', normal_eax='0xbad00004'),
        path('parameter-pass-before-direct-reads', (0x17b52, 0x17b6f, 0x17b72, 0x17b75, 0x17b78, 0x17b7f),
             vtable_offset='0x1c8', argument='RDX = original R8 parameter object',
             dispatch='indirect RAX target via slot 0xd7428', method_name='UNKNOWN',
             nested_parameter_reads='UNKNOWN; parameter object passed before five direct reads'),
        path('selector-query-and-tables', (0x17b85, 0x17b88, 0x17b8b, 0x17b8d, 0x17b90, 0x17b94,
                                          0x17b9a, 0x17ba0, 0x17ba3, 0x17ba5, 0x17ba8, 0x17baf, 0x17bb8, 0x17bc0, 0x17bc3),
             vtable_offset='0x38', method_name='UNKNOWN', null_object_destination='0x17bf6'),
        path('selector-rejection', (0x17bc5, 0x17bca, 0x17be8, 0x17bed, 0x17bef, 0x17bf4,
                                   0x17bf6, 0x17bfe, 0x17c1c, 0x17c21, 0x17c26),
             normal_eax='0xbad00001', normal_return_requires='shown calls return'),
        path('direct-read-entry-default-register', (0x17b5d, 0x17bf4, 0x17c2b, 0x17c2f, 0x17c53),
             edi_at_direct_reads=0, r15d_success_and_mask_default=1, scope='these five direct reads only'),
        path('additional-indirect-zero-gate', (0x17d5c, 0x17d61, 0x17d63, 0x17d66, 0x17d69,
                                              0x17d6d, 0x17d71, 0x17d77, 0x17d79, 0x17d9e, 0x17da3),
             skipped_if='qword [wrapper+0x10] != 0', method_name='UNKNOWN',
             condition='indirect call EAX != 0', saved_result='EBX = 0xbad00002',
             comparison='zero/nonzero, not BAD-family mask'),
        path('resolution-call-result-mask', (0x17f29, 0x17f2e, 0x17f30, 0x17f35, 0x17f3a, 0x17f69),
             callee_rva='0x21000', callee_semantics='UNKNOWN', result_saved_in='EBX',
             rejected_condition='(EAX & 0xfff00000) == 0xbad00000', rejection_cleanup_rva='0x180d1'),
        path('kernel-init-call-result-mask', (0x17f6e, 0x17f73, 0x17f7b, 0x17f80, 0x17f82, 0x17f87,
                                            0x17f8c, 0x17fbe, 0x17fc3),
             callee_rva='0x1dba0', callee_semantics='UNKNOWN', skipped_if='byte [wrapper+0x19] != 0',
             rejected_condition='(EAX & 0xfff00000) == 0xbad00000', rejection_cleanup_rva='0x180d1'),
        path('later-call-result-mask', (0x1800e, 0x18013, 0x18015, 0x1801a, 0x1801f),
             callee_rva='0x173b0', callee_semantics='UNKNOWN',
             rejected_condition='(EAX & 0xfff00000) == 0xbad00000', rejection_cleanup_rva='0x180d1'),
        path('handle-publication-and-success-tail', (0x17b4d, 0x18066, 0x1806b, 0x18070, 0x180cc, 0x18118, 0x18124, 0x1813c),
             publication='qword [original R9] = qword [rsp+0x68]', saved_normal_result=1,
             normal_return_requires='all intervening nested, cleanup and guard calls return',
             support_proven=False, gpu_completion_proven=False),
    ]
    return dict(schema=1, scope='bounded historical internal Create direct reads and selector only',
                code_span=dict(lo=hex(lo), hi=hex(code_hi), byte_count=len(raw_code),
                               instructions=len(rows), sha256=code_sha),
                keys=keys, anchors=anchors, functions=functions, tables=tables, reads=reads, paths=paths,
                selector=selector, nested_calls_and_parameter_reads='UNKNOWN', object_interfaces='UNKNOWN',
                full_create_abi_authenticated=False, selector_acceptance_proves_support=False,
                target_executed=False, payload_loaded=False, gpu_tested=False, f7_selected_payload=False,
                abi_authenticated=False, hardware_support_proven=False, gpu_completion_proven=False,
                runtime_failure_reachability_proven=False, caller_gates_bypassed=False, support='UNKNOWN')


def rect_contract(pe, code: dict) -> dict:
    """Recover one strict rectangle helper, without naming its backend interface."""
    name, lo, hi, unwind = 'descriptor-rect-helper', 0x17a20, 0x17b12, 0xff9f8
    span = code.get(name, {})
    if (span.get('lo'), span.get('hi')) != (hex(lo), hex(hi)):
        raise ValueError('historical NR rect decoded boundary changed')
    records = [entry.struct for entry in pe.DIRECTORY_ENTRY_EXCEPTION
               if entry.struct.BeginAddress == lo]
    if len(records) != 1 or records[0].EndAddress != hi or records[0].UnwindData != unwind:
        raise ValueError('historical NR rect pdata/unwind changed')
    pdata_rva = pe.get_rva_from_offset(records[0].get_file_offset())
    pdata_bytes = pe.get_data(pdata_rva, 12)
    if pdata_bytes != struct.pack('<III', lo, hi, unwind):
        raise ValueError('historical NR rect pdata bytes changed')
    unwind_bytes = '191904000a3411000ab2067088580a00'
    if pe.get_data(unwind, 16).hex() != unwind_bytes:
        raise ValueError('historical NR rect unwind bytes changed')
    raw = pe.get_data(lo, hi-lo)
    expected_sha = 'e0361004c0ba100abba548530322d223502513293c322e07355fe869e880cc70'
    if len(raw) != hi-lo or hashlib.sha256(raw).hexdigest() != expected_sha:
        raise ValueError('historical NR rect code bytes changed')
    rows = span.get('rows', [])
    if (len(rows) != 72 or span.get('instructions') != 72 or span.get('sha256') != expected_sha or
            ''.join(row['bytes'] for row in rows) != raw.hex()):
        raise ValueError('historical NR rect decoded code changed')
    by_rva = {int(row['rva'], 16): row for row in rows}
    anchors = []
    for rva, (raw_hex, mnemonic, operands) in RECT_ANCHORS.items():
        expected = dict(rva=hex(rva), bytes=raw_hex, mnemonic=mnemonic, operands=operands)
        if by_rva.get(rva) != expected:
            raise ValueError('historical NR rect instruction changed at '+hex(rva))
        anchors.append(expected)

    def path(name, rvas, **observations):
        if any(rva not in RECT_ANCHORS for rva in rvas):
            raise ValueError('unbound rect path: '+name)
        return dict(name=name, anchor_rvas=[hex(rva) for rva in rvas], **observations)

    paths = [
        path('zero-output-before-query', (0x17a39, 0x17a3e, 0x17a44, 0x17a48, 0x17a4c),
             initial_output_bytes=24, initial_value=0,
             precondition='input descriptor and output storage are valid nonoverlapping memory',
             descriptor_pointer_null_checked=False, output_pointer_null_checked=False),
        path('backend-and-resource-presence', (0x17a4f, 0x17a52, 0x17a58, 0x17a5b),
             backend_argument='RCX', resource_argument='qword [original RDX]',
             missing_result=False, output_on_missing=[0, 0, 0, 0, 0, 0]),
        path('query-result-zero-required', (0x17a66, 0x17a6f, 0x17a77, 0x17a7e, 0x17a84, 0x17a86),
             vtable_offset='0x190', method_name='UNKNOWN', exact_accepted_eax=0,
             dispatch_slot_rva='0xd7428', nonzero_result=False,
             query_descriptor_full_layout='UNKNOWN'),
        path('texture-dimensions-before-validation', (0x17a88, 0x17a8c, 0x17a90, 0x17a93,
                                                       0x17a96, 0x17a98, 0x17a9a, 0x17a9c),
             query_output_offsets=[0, 4], texture_dimensions_bits=32,
             copied_output_offsets=[16, 20], zero_dimension_result=False,
             failure_output_is_always_all_zero=False),
        path('requested-rect-and-full-extent-default', (0x17a9e, 0x17aa5, 0x17aab, 0x17aaf,
                 0x17ab3, 0x17ab6, 0x17ab8, 0x17abc, 0x17ac0, 0x17ac3, 0x17ac5, 0x17ac9),
             descriptor_offsets=[8, 12, 16, 20], field_bits=32,
             zero_requested_extent='use full texture extent, not remaining extent after origin',
             origin_clamped=False, extent_clamped=False),
        path('unsigned-subtraction-bounds', (0x17acd, 0x17ad0, 0x17ad2, 0x17ad5, 0x17ad7,
                 0x17ada, 0x17adc, 0x17adf, 0x17ae1, 0x17ae4, 0x17ae7, 0x17ae9, 0x17aec, 0x17aef),
             condition='extent nonzero; origin <= texture extent; size <= texture extent - origin',
             comparison='unsigned uint32', addition_wrap_used=False,
             failure_preserves_written_fields=True),
        path('boolean-normal-result', (0x17af1, 0x17af3, 0x17af5),
             accepted_al=1, rejected_al=0, normal_return_requires='callee and stack guard return',
             gpu_completion_proven=False),
    ]
    return dict(schema=1, scope='bounded historical descriptor rectangle helper only',
        code_span=dict(lo=hex(lo), hi=hex(hi), byte_count=hi-lo, instructions=72, sha256=expected_sha),
        functions=[dict(lo=hex(lo), hi=hex(hi), pdata_rva=hex(pdata_rva),
            pdata_bytes=pdata_bytes.hex(), unwind_rva=hex(unwind), unwind_bytes=unwind_bytes,
            language_specific_handler_data_decoded=False)], anchors=anchors, paths=paths,
        input_layout=dict(resource_offset=0, resource_bits=64, origin_x_offset=8, origin_y_offset=12,
                          requested_width_offset=16, requested_height_offset=20),
        output_layout=dict(origin_x_offset=0, origin_y_offset=4, width_offset=8, height_offset=12,
                           texture_width_offset=16, texture_height_offset=20, bytes=24),
        backend_interface='UNKNOWN', backend_method='UNKNOWN', deep_dynamic_calls='UNKNOWN',
        exceptions='UNKNOWN', format_or_resource_type_validated_in_shown_body=False,
        complete_evaluate_contract_proven=False, target_executed=False, payload_loaded=False,
        gpu_tested=False, f7_selected_payload=False, abi_authenticated=False,
        hardware_support_proven=False, gpu_completion_proven=False, support='UNKNOWN')


def scaling_contract(pe, inspection: dict) -> dict:
    export = next((entry for entry in inspection['named_exports'] if
                   entry['name'] == 'NVSDK_NGX_D3D12_PopulateParameters_Impl'), None)
    if export is None or export['status'] != 'direct-present' or export['rva'] != 0x157e0:
        raise ValueError('historical NR Populate export changed')
    keys = []
    for rva, expected in ((0xd94d8, 'DLSSNRGetStatsCallback'),
                           (0xd94f0, 'DLSSNRComputeScalingRatioCallback'),
                           (0xd9778, 'PerfQualityValue'), (0xd9830, 'DLSSNR.ScalingRatio')):
        raw = pe.get_data(rva, len(expected)+1)
        if raw != expected.encode('ascii')+bytes(1):
            raise ValueError('historical NR scaling key changed')
        keys.append(dict(rva=hex(rva), bytes=raw.hex(), name=expected))
    literals = []
    for rva, expected in ((0xd877c, '0000803f'), (0xd9848, '9a99593f'), (0xd9844, '0000003f')):
        raw = pe.get_data(rva, 4)
        if raw.hex() != expected:
            raise ValueError('historical NR scaling constant changed')
        literals.append(dict(rva=hex(rva), bytes=raw.hex(), value=struct.unpack('<f', raw)[0]))
    return dict(populate_export=export, keys=keys, literals=literals,
                ratio_callback_rva='0x12740', stats_callback_rva='0x14920',
                stats_callback_body_decoded=True, target_executed=False,
                callback_executed=False, hardware_support_proven=False)


def decode_window(pe, decoder, base: int, lo: int, hi: int) -> dict:
    raw = pe.get_data(lo, hi-lo)
    if len(raw) != hi-lo:
        raise ValueError('incomplete file span')
    instructions = list(decoder.disasm(raw, base+lo))
    if b''.join(bytes(i.bytes) for i in instructions) != raw:
        raise ValueError('incomplete instruction decode')
    rows = []
    address = base+lo
    for instruction in instructions:
        if instruction.address != address or not instruction.bytes:
            raise ValueError('non-contiguous instruction decode')
        rows.append(dict(rva=hex(address-base), bytes=bytes(instruction.bytes).hex(),
                         mnemonic=instruction.mnemonic, operands=instruction.op_str))
        address += len(instruction.bytes)
    return dict(lo=hex(lo), hi=hex(hi), sha256=hashlib.sha256(raw).hexdigest(),
                instructions=len(rows), rows=rows)


def check_anchors(rows: dict[int, dict]) -> list[dict]:
    result = []
    for rva, expected in ANCHORS.items():
        row = rows.get(rva)
        if row is None or (row['mnemonic'], row['operands']) != expected:
            raise ValueError('reference instruction mismatch at '+hex(rva))
        result.append(row)
    return result


def load_capstone(capstone_root: Path):
    """Bind the explicit decoder and every file in its observed toolchain."""
    expected_module = (capstone_root/'capstone/__init__.py').resolve()
    old_path = sys.path[:]
    try:
        sys.path.insert(0, str(capstone_root.resolve()))
        capstone = importlib.import_module('capstone')
    finally:
        sys.path[:] = old_path
    if Path(capstone.__file__).resolve() != expected_module:
        raise ValueError('Capstone did not come from the explicit directory')
    native = Path(capstone._cs._name)
    if not native.is_absolute() or not native.is_file():
        raise ValueError('cannot bind the loaded Capstone library')
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    package = expected_module.parent
    tool_files = sorted(path.resolve() for path in package.rglob('*') if path.is_file())
    if native.resolve() not in tool_files:
        tool_files.append(native.resolve())
    return decoder, dict(capstone=capstone.__version__, capstone_root=str(capstone_root.resolve()),
                         capstone_native=str(native.resolve()),
                         files={str(path): digest(path) for path in tool_files})


def build_report(input_path: Path, pefile_source: Path, capstone_root: Path) -> dict:
    inspection = inspect_file(input_path, expected_sha256=REFERENCE_SHA, pefile_file=pefile_source)
    for name, expected in EXPORT_RVAS.items():
        row = inspection['required_exports'][name]
        if row['status'] != 'direct-present' or row['rva'] != expected:
            raise ValueError('reference export mismatch: '+name)
    raw = reference_bytes(input_path)
    pefile = load_pefile_file(pefile_source)
    pe = pefile.PE(data=raw, fast_load=True)
    pe.parse_data_directories(directories=[1, 3])
    base = pe.OPTIONAL_HEADER.ImageBase
    if base != 0x180000000:
        raise ValueError('reference image base changed')
    decoder, toolchain = load_capstone(capstone_root)
    code, rows = {}, {}
    for name, (lo, hi) in RANGES.items():
        pdata_lo, pdata_hi = PDATA_RANGES.get(name, (lo, hi))
        if not any(e.struct.BeginAddress == pdata_lo and e.struct.EndAddress == pdata_hi
                   for e in pe.DIRECTORY_ENTRY_EXCEPTION):
            raise ValueError('reference pdata boundary mismatch: '+name)
        code[name] = decode_window(pe, decoder, base, lo, hi)
        rows.update((int(row['rva'], 16), row) for row in code[name]['rows'])
    imports = {entry.address-base: (module.dll.decode('ascii'), entry.name.decode('ascii'))
               for module in pe.DIRECTORY_ENTRY_IMPORT for entry in module.imports if entry.name}
    expected_imports = {0xd7118: ('KERNEL32.dll', 'GetModuleHandleExA'),
                        0xd7080: ('KERNEL32.dll', 'GetModuleFileNameW')}
    if any(imports.get(rva) != value for rva, value in expected_imports.items()):
        raise ValueError('reference caller-check import mismatch')
    literal = pe.get_data(0xd9380, 20)
    if literal != 'nvngx.dll'.encode('utf-16-le')+bytes(2):
        raise ValueError('reference caller-name literal mismatch')
    return dict(schema=1, profile='historical-nr-entrypoints', input=inspection['input'],
                export_inspection=inspection, image_base=hex(base), code=code,
                anchors=check_anchors(rows),
                imports=[dict(iat_rva=hex(rva), dll=dll, name=name)
                         for rva, (dll, name) in expected_imports.items()],
                caller_name=dict(rva='0xd9380', bytes=literal.hex(), text='nvngx.dll'),
                published_scaling=scaling_contract(pe, inspection),
                published_stats=stats_contract(pe, code),
                internal_create=create_contract(pe, code),
                descriptor_rect=rect_contract(pe, code),
                shutdown_identity=shutdown_identity_contract(pe, code),
                tools=toolchain,
                target_executed=False, payload_loaded=False, gpu_tested=False,
                f7_selected_payload=False, abi_authenticated=False, support='UNKNOWN')


def reference_sources(document: dict) -> list[Path]:
    try:
        if (type(document['schema']) is not int or document['schema'] != 1 or
                document['profile'] != 'historical-nr-entrypoints' or
                not isinstance(document['tools']['files'], dict)):
            raise ValueError('unexpected historical NR report schema/profile')
        return [Path(document['input']['path']),
                Path(document['export_inspection']['analyzer']['pefile_path']),
                *(Path(path) for path in document['tools']['files'])]
    except (KeyError, TypeError) as error:
        raise ValueError('incomplete historical NR report') from error


def verify_saved_report(path: Path) -> dict:
    document = json.loads(path.read_text(encoding='utf-8'))
    reference_sources(document)
    flags = ('target_executed', 'payload_loaded', 'gpu_tested', 'f7_selected_payload', 'abi_authenticated')
    if any(document.get(key) is not False for key in flags) or document.get('support') != 'UNKNOWN':
        raise ValueError('historical NR report exceeds its static scope')
    source = Path(document['input']['path'])
    raw = reference_bytes(source)
    if (document['input']['sha256'] != REFERENCE_SHA or document['input']['bytes'] != len(raw)):
        raise ValueError('historical NR reference identity mismatch')
    inspection = document['export_inspection']
    if inspection['input'] != document['input']:
        raise ValueError('historical NR export identity mismatch')
    if (any(inspection.get(key) is not False for key in
            ('target_executed', 'gpu_tested', 'abi_authenticated', 'support_proven')) or
            inspection.get('support') != 'UNKNOWN'):
        raise ValueError('historical NR export report exceeds its static scope')
    pefile_source = Path(inspection['analyzer']['pefile_path'])
    if digest(pefile_source) != inspection['analyzer']['pefile_sha256']:
        raise ValueError('historical NR pefile source changed')
    pefile = load_pefile_file(pefile_source)
    observed = read_pe(raw, pefile)
    if any(inspection.get(key) != value for key, value in observed.items()):
        raise ValueError('historical NR export observation changed')
    pe = pefile.PE(data=raw, fast_load=True)
    pe.parse_data_directories(directories=[1, 3])
    base = pe.OPTIONAL_HEADER.ImageBase
    if document['image_base'] != hex(base) or set(document['code']) != set(RANGES):
        raise ValueError('historical NR code inventory changed')
    if (json.dumps(document['published_scaling'], sort_keys=True, allow_nan=False) !=
            json.dumps(scaling_contract(pe, inspection), sort_keys=True, allow_nan=False)):
        raise ValueError('historical NR published scaling contract changed')
    decoder, toolchain = load_capstone(Path(document['tools']['capstone_root']))
    if toolchain != document['tools']:
        raise ValueError('historical NR decoder toolchain changed')
    code, rows = {}, {}
    for name, (lo, hi) in RANGES.items():
        pdata_lo, pdata_hi = PDATA_RANGES.get(name, (lo, hi))
        if not any(e.struct.BeginAddress == pdata_lo and e.struct.EndAddress == pdata_hi
                   for e in pe.DIRECTORY_ENTRY_EXCEPTION):
            raise ValueError('historical NR pdata boundary changed')
        code[name] = decode_window(pe, decoder, base, lo, hi)
        if document['code'][name] != code[name]:
            raise ValueError('historical NR decoded instructions changed: '+name)
        rows.update((int(row['rva'], 16), row) for row in code[name]['rows'])
    if document['anchors'] != check_anchors(rows):
        raise ValueError('historical NR anchors changed')
    published_stats = stats_contract(pe, code)
    if (json.dumps(document.get('published_stats'), sort_keys=True, allow_nan=False) !=
            json.dumps(published_stats, sort_keys=True, allow_nan=False)):
        raise ValueError('historical NR published stats contract changed or exceeds static scope')
    internal_create = create_contract(pe, code)
    if (json.dumps(document.get('internal_create'), sort_keys=True, allow_nan=False) !=
            json.dumps(internal_create, sort_keys=True, allow_nan=False)):
        raise ValueError('historical NR internal create contract changed or exceeds static scope')
    descriptor_rect = rect_contract(pe, code)
    if (json.dumps(document.get('descriptor_rect'), sort_keys=True, allow_nan=False) !=
            json.dumps(descriptor_rect, sort_keys=True, allow_nan=False)):
        raise ValueError('historical NR descriptor rect contract changed or exceeds static scope')
    shutdown_identity = shutdown_identity_contract(pe, code)
    # JSON type-sensitive equality also refuses True for literal integer 1 and
    # numeric zero for scope False. Unknown/additional claims fail closed.
    if (json.dumps(document.get('shutdown_identity'), sort_keys=True, allow_nan=False) !=
            json.dumps(shutdown_identity, sort_keys=True, allow_nan=False)):
        raise ValueError('historical NR shutdown identity contract changed or exceeds static scope')
    imports = {entry.address-base: (module.dll.decode('ascii'), entry.name.decode('ascii'))
               for module in pe.DIRECTORY_ENTRY_IMPORT for entry in module.imports if entry.name}
    expected_imports = {0xd7118: ('KERNEL32.dll', 'GetModuleHandleExA'),
                        0xd7080: ('KERNEL32.dll', 'GetModuleFileNameW')}
    expected_rows = [dict(iat_rva=hex(rva), dll=dll, name=name)
                     for rva, (dll, name) in expected_imports.items()]
    if (document['imports'] != expected_rows or
            any(imports.get(rva) != value for rva, value in expected_imports.items())):
        raise ValueError('historical NR caller-check import changed')
    literal = document['caller_name']
    if (literal != dict(rva='0xd9380', bytes=pe.get_data(0xd9380, 20).hex(), text='nvngx.dll') or
            pe.get_data(0xd9380, 20) != 'nvngx.dll'.encode('utf-16-le')+bytes(2)):
        raise ValueError('historical NR literal changed')
    return dict(file=path.name, sha256=digest(path), input_sha256=REFERENCE_SHA,
                windows=len(RANGES), instructions=sum(x['instructions'] for x in code.values()),
                instruction_assertions=len(document['anchors']),
                scaling_key_assertions=len(document['published_scaling']['keys']),
                scaling_literal_assertions=len(document['published_scaling']['literals']),
                **{field: len(published_stats[array]) for field, array in STATS_ARRAYS.items()},
                **{field: len(internal_create[array]) for field, array in CREATE_ARRAYS.items()},
                create_code_bytes=internal_create['code_span']['byte_count'],
                create_code_instructions=internal_create['code_span']['instructions'],
                create_data_bytes=sum(table['byte_count'] for table in internal_create['tables']),
                **{field: len(descriptor_rect[array]) for field, array in RECT_ARRAYS.items()},
                **{field: len(shutdown_identity[array])
                   for field, array in SHUTDOWN_IDENTITY_ARRAYS.items()},
                static_decode_refreshed=True, decoder_identity_verified=True,
                target_executed=False, gpu_tested=False, f7_selected_payload=False,
                abi_authenticated=False, support='UNKNOWN')


def main(argv: list[str] | None = None) -> int:
    from verify import protect_inputs
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--pefile-file', type=Path, required=True)
    parser.add_argument('--capstone-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        package = args.capstone_root/'capstone'
        if not (package/'__init__.py').is_file():
            raise ValueError('explicit Capstone directory is missing')
        inputs = [args.input, args.pefile_file, Path(__file__),
                  Path(__file__).with_name('nr_payload_inspect.py'),
                  Path(__file__).with_name('verify.py')]
        inputs.extend(path for path in package.rglob('*') if path.is_file())
        protect_inputs(inputs, args.output.parent)
        report = build_report(args.input, args.pefile_file, args.capstone_root)
        protect_inputs(inputs+[Path(path) for path in report['tools']['files']], args.output.parent)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2, allow_nan=False)+chr(10), encoding='utf-8')
        print(json.dumps(dict(status='PASS', sha256=REFERENCE_SHA,
              instructions=sum(row['instructions'] for row in report['code'].values()),
              anchors=len(report['anchors']), payload_loaded=False, gpu_tested=False)))
        return 0
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    raise SystemExit(main())
