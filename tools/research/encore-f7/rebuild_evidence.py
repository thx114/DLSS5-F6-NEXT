"""Fresh bounded static decode. Never loads Encore or modifies product files."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
SHA = '06a7fd63ee0f0e95574f3869d70c3c9ae4aaf326ba53ff8c864064410a5f1217'
RANGES = {'camera-constants': (0xe010, 0xe807),
          'synthetic-projection': (0x13050, 0x13447),
          'submit-camera': (0x137ec, 0x138a3)}
RUNTIME_RANGES = {'fg-options': (0x14f80, 0x15299),
                  'fg-post-present': (0x140d0, 0x14b04),
                  'fg-request-callback': (0xdc130, 0xdc2b5),
                  'fg-state-telemetry': (0xf5cc0, 0xf5ea9),
                  'fg-state-callback': (0xf5eb0, 0xf6028),
                  'fg-callback-registration': (0xe97b7, 0xe97f1),
                  'fg-capacity-feedback-window': (0xf7dc0, 0xf7e20),
                  'nr-create': (0x157ef8, 0x157f33),
                  'nr-copy-encode': (0x1582e2, 0x158807),
                  'nr-parameters': (0x15887c, 0x158c0c),
                  'nr-evaluate': (0x158c0c, 0x158c5b),
                  'nr-result-decode': (0x158dc7, 0x158e90),
                  'nr-feature-reset': (0x1569b0, 0x156a32),
                  'nr-work-hysteresis': (0x15706d, 0x157137),
                  'nr-created-identity': (0x157f86, 0x157fcd),
                  'nr-reset-outcome': (0x158e90, 0x158e9e),
                  'nr-warp-failure-fallback': (0x158f27, 0x158fe8),
                  'nr-warp-fallback-call': (0x1593b3, 0x15941b),
                  'nr-payload-select': (0x357315, 0x357373),
                  'nr-payload-load-wrapper': (0x35a650, 0x35a677),
                  'nr-payload-load-call': (0x357494, 0x3574c0),
                  'nr-payload-loaded-identity': (0x357517, 0x35757f),
                  'nr-payload-export-resolve': (0x35757f, 0x3575e8),
                  'nr-payload-api-commit': (0x357f2f, 0x357f87),
                  'nr-payload-api-accessor': (0x3553a0, 0x3553b2),
                  'nr-payload-init-call': (0x151ec2, 0x151f09),
                  'nr-payload-evaluate-table-caller': (0x15225a, 0x1522ca),
                  'nr-parameter-constructor': (0x142604, 0x142649),
                  'nr-parameter-setters': (0x15a650, 0x15a6cd),
                  'nr-parameter-readers': (0x14c920, 0x14ce3c),
                  'nr-parameter-clear': (0x1566c0, 0x1566ff),
                  'sr-generic-prepare': (0x35d9fd, 0x35e1e9),
                  'sr-generic-api-wrappers': (0x35e340, 0x35e39d),
                  'numeric-parameter-store': (0x14f1d0, 0x14f2ac),
                  'pointer-parameter-store': (0x14f550, 0x14f62a),
                  'nr-parameter-find-or-insert': (0x1419a0, 0x141b4a),
                  'nr-feature-release-owner': (0x157c77, 0x157d0d),
                  'nr-feature-handle-commit': (0x157f33, 0x157f86),
                  'nr-feature-release-callback': (0x15dad0, 0x15db04),
                  'nr-feature-held-interface-copy': (0xcb70, 0xcbb4)}
RUNTIME_RANGES.update({
    'nr-device-get-from-child': (0x15140f, 0x151453),
    'nr-device-admission-reject': (0x152458, 0x15247b),
    'nr-device-output-address': (0x92d0, 0x92f8),
    'nr-device-runtime-hold': (0x1518f0, 0x15192b),
    'nr-runtime-destructor': (0x143430, 0x14369e),
    'nr-shutdown-export-required': (0x357640, 0x35764b),
    'nr-payload-state-cleanup-registration': (0x35a479, 0x35a4b6),
    'nr-payload-state-exit-cleanup': (0x539bc0, 0x539c43),
    'nr-cleanup-narrow-buffer': (0xc6c0, 0xc725),
    'nr-cleanup-pointer-buffer': (0xc7b0, 0xc818),
    'nr-cleanup-wide-buffer': (0x18200, 0x1826f),
    'nr-cleanup-config-records': (0x35ae70, 0x35af26),
    'nr-cleanup-free-thunk-1': (0x4e76f0, 0x4e76f5),
    'nr-cleanup-free-thunk-2': (0x4e80a0, 0x4e80a5),
    'nr-cleanup-free-thunk-3': (0x4f7de0, 0x4f7de5),
    'nr-cleanup-heap-free': (0x50b6c0, 0x50b6fc),
    'nr-payload-table-context-capture': (0x155019, 0x15503d),
    'nr-payload-context-evaluate': (0x145c8b, 0x145cae),
    'nr-payload-missing-export-unload': (0x358122, 0x35812b),
    'ngx-shutdown-descriptor': (0x93356, 0x9337a),
    'ngx-shutdown-resolve-slots': (0x93421, 0x9344e),
    'ngx-shutdown-wrapper': (0xffa00, 0xffa4f),
    'ngx-shutdown-postprocess': (0xff8f0, 0xff9bd),
    'ngx-shutdown-pair-registration': (0x936d2, 0x93753),
    'ngx-shutdown-target-diagnostic': (0x10af75, 0x10b028),
    'ngx-shutdown-transaction-result': (0x10b044, 0x10b08d),
    'ngx-shutdown-clear-slots': (0x10b0c0, 0x10b0ed),
    'ngx-hook-transaction': (0x196a0, 0x19ac6),
    'ngx-hook-begin-owner': (0x425320, 0x425390),
    'ngx-hook-protection-prepare': (0x424870, 0x4248c6),
    'ngx-hook-abort-owner-gate': (0x4251d0, 0x4251eb),
    'ngx-hook-commit-owner-gate': (0x425390, 0x4253fb),
    'ngx-hook-target-normalizer-wrapper': (0x424f50, 0x424f72),
    'ngx-hook-attach-wrapper': (0x4248d0, 0x424901),
    'ngx-hook-thread-id-thunk': (0x4de981, 0x4de987),
    'ngx-hook-protect-thunk': (0x4de9a5, 0x4de9ab),
    'ngx-hook-last-error-thunk': (0x4de96f, 0x4de975),
    'ngx-hook-thread-admission': (0x1b700, 0x1b95d),
    'ngx-hook-snapshot-thunk': (0x4de98d, 0x4de993),
    'ngx-hook-thread-first-thunk': (0x4de993, 0x4de999),
    'ngx-hook-thread-next-thunk': (0x4de999, 0x4de99f),
})
CODEC_RANGES = {'nr-pso-initializer': (0x149af0, 0x149e29),
                'nr-output-extent': (0x156c53, 0x156ca3),
                'nr-root-bind': (0x144fd0, 0x145113),
                'nr-exposure-decode-slot': (0x157a2e, 0x157a4b)}
# Analyst names, not original symbols. Each table contains {bytecode VA, size}.
CODEC_SHADERS = {
    'nr-encode': dict(rva='0x57f800', size=27520, table_rva='0x586380'),
    'nr-decode': dict(rva='0x586390', size=12680, table_rva='0x589518'),
    'nr-encode-exposure': dict(rva='0x589530', size=27952, table_rva='0x590260'),
    'nr-decode-exposure': dict(rva='0x590270', size=13108, table_rva='0x5935a8'),
}
WARP_RANGES = {'nr-guide-pso': (0x14f87f, 0x14f8da),
               'nr-guide-root-dispatch': (0x1530b0, 0x1531b2),
               'nr-guide-constants': (0x158609, 0x1587a7),
               'nr-native-curve': (0x14beb0, 0x14bf57),
               'nr-axis-packing-gmax': (0x14e6d0, 0x14eb0e),
               'nr-pair-eligibility': (0x14efc0, 0x14f198),
               'nr-pair-call-copy': (0x156d97, 0x156dba),
               'nr-pair-store': (0x157432, 0x157456),
               'nr-work-extent': (0x15d660, 0x15d78f),
               'nr-warp-work-extent': (0x156f0d, 0x156fba),
               'nr-grid-cost': (0x14b4b0, 0x14b641),
               'nr-grid-search': (0x14f320, 0x14f4ee),
               'nr-native-round': (0x4ffe60, 0x4ffec6),
               'nr-round-bits': (0x504060, 0x5040c4),
               'nr-round-classify': (0x50d390, 0x50d3ec)}
WARP_SHADERS = {
    'nr-guides': dict(rva='0x594390', size=20296, code_ref='0x14f897', size_ref='0x14f8a2'),
}
WARP_ANCHORS = {
    0x14f897: ('lea', 'rax, [rip + 0x444af2]'),
    0x14f8a2: ('mov', 'qword ptr [rbp - 0x38], 0x4f48'),
    0x14f8d7: ('call', 'qword ptr [rbx + 0x58]'),
    0x153119: ('mov', 'r8d, 0x14'),
    0x153122: ('call', 'qword ptr [rax + 0x118]'),
    0x1531af: ('call', 'qword ptr [rax + 0x70]'),
    0x15871d: ('divss', 'xmm1, xmm0'),
    0x158741: ('divss', 'xmm1, xmm0'),
    0x158796: ('call', '0x1801530b0'),
    0x14e94b: ('divsd', 'xmm12, xmm7'),
    0x14e966: ('mulsd', 'xmm14, qword ptr [rip + 0x44dab9]'),
    0x14f16c: ('mulsd', 'xmm1, xmm0'),
    0x14f178: ('jbe', '0x18014f18d'),
    0x14e7e1: ('cmp', 'r14d, 0x7f'),
    0x14e7e5: ('ja', '0x18014eaf0'),
    0x156f9c: ('cmp', 'eax, 0x40'),
    0x156fa1: ('cmp', 'ecx, 0x40'),
    0x14b4bc: ('shr', 'eax, 1'),
    0x14b4cf: ('and', 'r8d, 0xfffffffc'),
    0x14b547: ('shl', 'r14d, cl'),
    0x14b5c7: ('mov', 'edi, 0x140'),
    0x14b60c: ('jne', '0x18014b631'),
    0x14b622: ('cmovne', 'ecx, r9d'),
    0x14b626: ('imul', 'rax, rcx'),
    0x14f393: ('imul', 'ecx, esi, 0x62'),
    0x14f450: ('imul', 'rax, rax, 0x190'),
    0x14f475: ('jbe', '0x18014f485'),
    0x15d76e: ('call', '0x18014f320'),
    0x156f8a: ('call', '0x18014f320'),
    0x4ffe85: ('call', '0x180504060'),
    0x504092: ('adc', 'rax, 0'),
    0x5040ac: ('cmp', 'rdx, 0x3fe'),
    0x50d3c6: ('inc', 'ax'),
    0x14e92b: ('mov', 'dword ptr [rdi], r14d'),
    0x14ea3e: ('mulsd', 'xmm11, qword ptr [rax]'),
    0x14ea63: ('mov', 'dword ptr [rdi + 0x10], eax'),
    0x14ea6e: ('jae', '0x18014eab4'),
    0x14eab9: ('movss', 'dword ptr [rdi + 0x14], xmm0'),
    0x14efdc: ('cmp', 'cl, 3'),
    0x14eff5: ('lea', 'r9, [rip + 0x40b024]'),
    0x156d9e: ('call', '0x18014efc0'),
    0x15744e: ('movups', 'xmmword ptr [r14 + 0x138], xmm0'),
}
WARP_LITERALS = {0x59c420: ('falloff-scale', '<d', 1/2048),
                 0x59c428: ('edge-scale', '<d', 1/128),
                 0x59c430: ('max-area-ratio', '<d', 0.95),
                 0x59c440: ('edge-quantization', '<d', 128.0),
                 0x59c448: ('falloff-quantization', '<d', 2048.0),
                 0x59c450: ('inner-quantization', '<d', 4095.0),
                 0x59c41c: ('min-work-scale', '<f', struct.unpack('<f', struct.pack('<I', 0x3ea8f5c3))[0]),
                 0xce9468: ('round-max-signed32', '<d', 2147483647.0),
                 0xce9470: ('round-min-signed32', '<d', -2147483648.0),
                 0x59c438: ('derivative-three', '<d', 3.0),
                 0x59c418: ('min-edge-scale', '<f', 0.25),
                 0x57f56c: ('clamp-min-edge-scale', '<f', 0.25),
                 0x57f570: ('clamp-max-edge-scale', '<f', 1.0)}
# Fixed-target instruction assertions, separate from interpretation in the report.
# No assertion below authenticates a runtime call, source identity or scanout.
RUNTIME_ANCHORS = {
    0x15140f: ('mov', 'r15, qword ptr [r14 + 0x20]'),
    0x151424: ('call', '0x1800092d0'),
    0x15142c: ('lea', 'rdx, [rip + 0x3eb9ed]'),
    0x151436: ('call', 'qword ptr [rbx + 0x38]'),
    0x15144a: ('cmp', 'rcx, rdx'),
    0x15144d: ('jne', '0x18015245d'),
    0x15245d: ('mov', 'byte ptr [rdi], 1'),
    0x15246a: ('mov', 'qword ptr [rsp + 0x70], r13'),
    0x152472: ('call', 'qword ptr [rax + 0x10]'),
    0x152476: ('jmp', '0x1801512c0'),
    0x92ef: ('mov', 'rax, rbx'),
    0x1518f0: ('mov', 'r15, qword ptr [rsi + 0x50]'),
    0x1518f4: ('mov', 'rbx, qword ptr [rsp + 0x70]'),
    0x15190d: ('call', 'qword ptr [rax + 8]'),
    0x151918: ('mov', 'qword ptr [r15 + 0x90], rbx'),
    0x151927: ('call', 'qword ptr [rax + 0x10]'),
    0x1434c8: ('mov', 'rbx, qword ptr [rdi + 0x168]'),
    0x1434db: ('lock xadd', 'dword ptr [rbx + 8], eax'),
    0x1434eb: ('call', 'qword ptr [rax]'),
    0x14362f: ('mov', 'rcx, qword ptr [rdi + 0x90]'),
    0x143645: ('call', 'qword ptr [rax + 0x10]'),
    0x357640: ('cmp', 'qword ptr [rbp - 0x50], 0'),
    0x357645: ('je', '0x180358122'),
    0x35a479: ('lea', 'rax, [rip + 0x13099e0]'),
    0x35a4aa: ('lea', 'rcx, [rip + 0x1df70f]'),
    0x35a4b1: ('call', '0x1804e7a50'),
    0x539bc4: ('lea', 'rcx, [rip + 0x112a495]'),
    0x539bcb: ('call', '0x18000c7b0'),
    0x539bf8: ('call', '0x1804e76f0'),
    0x539c22: ('call', '0x180018200'),
    0x539c2e: ('call', '0x18035ae70'),
    0x539c3e: ('jmp', '0x180018200'),
    0x15502b: ('mov', 'qword ptr [rbp + 0x120], r14'),
    0x145c8b: ('mov', 'r10, qword ptr [rbx + 0x20]'),
    0x145caa: ('call', 'qword ptr [r10 + 0x10]'),
    0x358122: ('mov', 'rcx, r15'),
    0x358125: ('call', 'qword ptr [rip + 0x1e32c5]'),
    0x93356: ('lea', 'rax, [rip + 0x4c7eab]'),
    0x9335d: ('mov', 'qword ptr [rsp + 0x60], rax'),
    0x93362: ('lea', 'rax, [rip + 0x13e623f]'),
    0x93369: ('mov', 'qword ptr [rsp + 0x68], rax'),
    0x9336e: ('lea', 'rax, [rip + 0x6c68b]'),
    0x93375: ('mov', 'qword ptr [rsp + 0x70], rax'),
    0x93421: ('cmp', 'byte ptr [rbx + 0x18], 0'),
    0x9342d: ('call', 'qword ptr [rip + 0x4a7f75]'),
    0x9343a: ('mov', 'rax, qword ptr [rbx + 8]'),
    0x93442: ('mov', 'qword ptr [rax], rcx'),
    0xffa0a: ('mov', 'rdi, rcx'),
    0xffa25: ('call', '0x180056590'),
    0xffa2a: ('mov', 'rax, qword ptr [rip + 0x1379b77]'),
    0xffa31: ('mov', 'rcx, rdi'),
    0xffa34: ('call', 'rax'),
    0xffa36: ('mov', 'rdx, rdi'),
    0xffa39: ('mov', 'ecx, eax'),
    0xffa3b: ('mov', 'ebx, eax'),
    0xffa3d: ('call', '0x1800ff8f0'),
    0xffa42: ('mov', 'eax, ebx'),
    0xff8fd: ('movzx', 'eax, byte ptr [rip + 0x1378f34]'),
    0xff90b: ('je', '0x1800ff9ac'),
    0xff911: ('cmp', 'ecx, 1'),
    0xff91d: ('call', 'qword ptr [rip + 0x43bc3d]'),
    0xff92a: ('mov', 'r8d, 0xc00'),
    0xff947: ('call', '0x18051f420'),
    0xff95b: ('call', 'qword ptr [rip + 0x43bbef]'),
    0xff96f: ('cmp', 'rax, 0x400'),
    0xff99d: ('mov', 'r8d, ebx'),
    0xff9a3: ('sete', 'r9b'),
    0xff9a7: ('call', '0x1800d0200'),
    0x936d2: ('mov', 'r13, qword ptr [rsp + r14 + 0x48]'),
    0x936d7: ('cmp', 'qword ptr [r13], 0'),
    0x936e9: ('mov', 'qword ptr [r13], 0'),
    0x9373d: ('mov', 'rax, qword ptr [rsp + r14 + 0x50]'),
    0x93742: ('mov', 'qword ptr [rdi + 8], rax'),
    0x93746: ('mov', 'qword ptr [rdi], r13'),
    0x10af75: ('xor', 'r15d, r15d'),
    0x10afa8: ('call', '0x1800932f0'),
    0x10afe6: ('mov', 'rax, qword ptr [rcx + rax*8]'),
    0x10afea: ('mov', 'rcx, qword ptr [rax]'),
    0x10aff8: ('mov', 'qword ptr [rsp + 0x20], rcx'),
    0x10b002: ('mov', 'r9, qword ptr [r9 + rbx*8]'),
    0x10b017: ('call', '0x180074a90'),
    0x10b023: ('call', '0x180155db0'),
    0x10b04c: ('je', '0x18010b0c0'),
    0x10b072: ('call', '0x1800196a0'),
    0x10b077: ('mov', 'r9d, dword ptr [rax + 8]'),
    0x10b07e: ('je', '0x18010b1d7'),
    0x10b087: ('je', '0x18010b0c0'),
    0x10b0e6: ('mov', 'qword ptr [rip + 0x136e4bb], r15'),
    0x19749: ('call', '0x180425320'),
    0x19753: ('jne', '0x18001998a'),
    0x19798: ('call', '0x180424f50'),
    0x197cb: ('je', '0x18001995f'),
    0x1981a: ('mov', 'rdx, qword ptr [rcx + rdi*8 + 8]'),
    0x1982c: ('mov', 'ecx, 5'),
    0x19831: ('call', 'qword ptr [rip + 0x521b69]'),
    0x19839: ('je', '0x1800199a8'),
    0x1986c: ('call', '0x18001b700'),
    0x19875: ('jne', '0x1800198a3'),
    0x19883: ('call', 'r13'),
    0x198a3: ('call', '0x1804251d0'),
    0x198b8: ('call', 'qword ptr [rip + 0x521a82]'),
    0x19966: ('mov', 'dword ptr [rbp - 0x29], 0xd'),
    0x19976: ('mov', 'dword ptr [rbp - 0x29], 0x57'),
    0x19985: ('call', '0x1804251d0'),
    0x199c1: ('call', 'qword ptr [rip + 0x521981]'),
    0x199cc: ('mov', 'dword ptr [rbp - 0x29], 0x57'),
    0x19a27: ('mov', 'rcx, qword ptr [r8 + rcx*8]'),
    0x19a2b: ('call', '0x1804248d0'),
    0x19a35: ('jne', '0x1800198a3'),
    0x19a6c: ('call', '0x180425390'),
    0x19a71: ('mov', 'dword ptr [rbp - 0x29], eax'),
    0x19aa6: ('cmp', 'qword ptr [r8 + rax*8], r9'),
    0x425324: ('cmp', 'dword ptr [rip + 0x123fe25], 0'),
    0x425334: ('call', '0x1804de981'),
    0x42534a: ('lock cmpxchg', 'dword ptr [rcx], edx'),
    0x425350: ('je', '0x180425359'),
    0x42537a: ('call', '0x180424870'),
    0x42537f: ('mov', 'dword ptr [rip + 0x123fdcf], eax'),
    0x425385: ('mov', 'eax, dword ptr [rip + 0x123fdc9]'),
    0x42489d: ('mov', 'r8d, 0x40'),
    0x4248a3: ('mov', 'edx, 0x10000'),
    0x4248ad: ('call', '0x1804de9a5'),
    0x4248b4: ('jne', '0x1804248bd'),
    0x4248b6: ('call', '0x1804de96f'),
    0x4251d4: ('call', '0x1804de981'),
    0x4251d9: ('cmp', 'dword ptr [rip + 0x123ff71], eax'),
    0x4251e1: ('mov', 'eax, 0x10dd'),
    0x4253cb: ('call', '0x1804de981'),
    0x4253d0: ('cmp', 'dword ptr [rip + 0x123fd7a], eax'),
    0x4253d8: ('mov', 'eax, 0x10dd'),
    0x4253eb: ('call', '0x1804251d0'),
    0x424f68: ('call', '0x1804246a0'),
    0x4248f7: ('call', '0x180424910'),
    0x1b74e: ('mov', 'ecx, 4'),
    0x1b753: ('call', '0x1804de98d'),
    0x1b774: ('mov', 'dword ptr [rsp + 0x50], 0x1c'),
    0x1b7a8: ('call', 'qword ptr [rip + 0x51fc12]'),
    0x1b7b4: ('call', 'qword ptr [rip + 0x51fc16]'),
    0x1b7c6: ('mov', 'ecx, 0x10001a'),
    0x1b7cb: ('call', 'qword ptr [rip + 0x51fc07]'),
    0x1b7e9: ('mov', 'qword ptr [rdx], rax'),
    0x1b7f0: ('mov', 'dword ptr [rdx + 8], eax'),
    0x1b850: ('call', 'qword ptr [rip + 0x51faea]'),
    0x1b889: ('call', '0x180425830'),
    0x1b88e: ('mov', 'dword ptr [r15 + 8], eax'),
    0x1b8c1: ('cmp', 'eax, 0x57'),
    0x1b8f0: ('call', 'qword ptr [rip + 0x51fa6a]'),
    0x1b8fb: ('mov', 'byte ptr [rsi + 0x1c], cl'),
    0x1b927: ('call', 'qword ptr [rip + 0x51fa9b]'),
    0x1b930: ('call', '0x180425830'),
    0x1504a: ('xchg', 'dword ptr [rip + 0xd51004], eax'),
    0x15077: ('cmove', 'edi, ebx'),
    0x150a3: ('lea', 'eax, [rdi - 1]'),
    0x15182: ('mov', 'dword ptr [rip + 0xd8bfd0], edi'),
    0x15200: ('mov', 'dword ptr [rbp - 0x7c], 1'),
    0x15238: ('xchg', 'dword ptr [rip + 0xd50e12], ebx'),
    0xdc281: ('shl', 'rax, 0x20'),
    0xdc287: ('or', 'rax, rcx'),
    0xf5ce8: ('mov', 'edi, dword ptr [rbx + 0x30]'),
    0xf5dcd: ('cmp', 'esi, 2'),
    0xf5dd6: ('mov', 'edi, dword ptr [rip + 0x12def8c]'),
    0xf5de9: ('xchg', 'dword ptr [rip + 0x12def79], edi'),
    0xf5e1f: ('mov', 'eax, dword ptr [rbx + 0x34]'),
    0xf5e2f: ('cmp', 'byte ptr [rbx + 0x50], 1'),
    0x157f06: ('mov', 'edx, 0x12'),
    0x157f13: ('call', 'qword ptr [rax + 8]'),
    0x15839b: ('call', 'qword ptr [rax + 0x80]'),
    0x1584c0: ('mov', 'rdx, qword ptr [r14 + 0x1a8]'),
    0x158512: ('mov', 'rdx, qword ptr [r14 + 0x1b0]'),
    0x1587fd: ('mov', 'r9d, 1'),
    0x158803: ('call', 'qword ptr [r10 + 0x70]'),
    0x15887c: ('mov', 'r8, qword ptr [r14 + 0x1a8]'),
    0x158892: ('mov', 'r8, qword ptr [r14 + 0x1b0]'),
    0x158af2: ('shr', 'eax, 3'),
    0x158b2a: ('or', 'eax, dword ptr [rbp + 0x5c]'),
    0x158c3c: ('call', 'qword ptr [rax + 0x10]'),
    0x158dc9: ('and', 'esi, 0xfff00000'),
    0x158dcf: ('cmp', 'esi, 0xbad00000'),
    0x158dd5: ('je', '0x180158e6d'),
    0x158e29: ('call', 'qword ptr [r10 + 0x70]'),
    0x1569c8: ('mov', 'qword ptr [rcx + 0x160], rbp'),
    0x156a19: ('mov', 'qword ptr [rdi + 0x158], rbp'),
    0x156a20: ('mov', 'qword ptr [rdi + 0x170], rbp'),
    0x15706d: ('cmp', 'qword ptr [r14 + 0x158], 0'),
    0x15709b: ('jne', '0x1801570af'),
    0x1570bd: ('mov', 'dword ptr [r14 + 0x188], ebx'),
    0x1570ca: ('cmp', 'qword ptr [r14 + 0x190], rcx'),
    0x1570da: ('inc', 'eax'),
    0x1570e3: ('cmp', 'eax, 0xf'),
    0x1570e6: ('jae', '0x180157137'),
    0x1570ef: ('cmp', 'qword ptr [r14 + 0x1c8], rax'),
    0x1570fe: ('cmp', 'dword ptr [r14 + 0x1d0], eax'),
    0x15710d: ('cmp', 'dword ptr [r14 + 0x1d8], eax'),
    0x157119: ('mov', 'dword ptr [rbp - 0x80], edx'),
    0x157129: ('mov', 'qword ptr [r14 + 0x180], rbx'),
    0x157130: ('mov', 'dword ptr [r14 + 0x188], ebx'),
    0x157f89: ('mov', 'dword ptr [r14 + 0x170], edx'),
    0x157f9e: ('mov', 'dword ptr [r14 + 0x178], eax'),
    0x157fbe: ('mov', 'byte ptr [r14], 1'),
    0x157fc2: ('mov', 'dword ptr [r14 + 0x188], 0'),
    0x158b30: ('mov', 'byte ptr [r14], 1'),
    0x158e90: ('cmp', 'esi, 0xbad00000'),
    0x158e96: ('sete', 'al'),
    0x158e99: ('mov', 'byte ptr [rbx], al'),
    0x158e9b: ('mov', 'byte ptr [r14], al'),
    0x158f5e: ('mov', 'byte ptr [r14 + 0x148], 1'),
    0x158f66: ('mov', 'byte ptr [rbx], 0'),
    0x158f7f: ('call', '0x1801569b0'),
    0x158fdd: ('mov', 'byte ptr [r14], 1'),
    0x1593fc: ('mov', 'byte ptr [rsp + 0x20], 1'),
    0x159413: ('call', '0x180156ac0'),
    0x35731d: ('lea', 'r14, [rip + 0x3c447c]'),
    0x35736a: ('add', 'r14, 0x68'),
    0x3574ae: ('call', '0x18035a650'),
    0x35755b: ('call', '0x18035a6b0'),
    0x357584: ('lea', 'rdx, [rip + 0x3e1485]'),
    0x35758b: ('call', 'qword ptr [rip + 0x1e3e17]'),
    0x35759f: ('call', 'qword ptr [rip + 0x1e3e03]'),
    0x3575b6: ('call', 'qword ptr [rip + 0x1e3dec]'),
    0x3575ca: ('call', 'qword ptr [rip + 0x1e3dd8]'),
    0x3575de: ('call', 'qword ptr [rip + 0x1e3dc4]'),
    0x357f5a: ('mov', 'qword ptr [rsi + 0x70], rax'),
    0x357f62: ('mov', 'qword ptr [rsi + 0x78], rax'),
    0x357f6a: ('mov', 'qword ptr [rsi + 0x80], rax'),
    0x357f75: ('mov', 'qword ptr [rsi + 0x88], rax'),
    0x357f80: ('mov', 'qword ptr [rsi + 0x90], rax'),
    0x3553a9: ('add', 'rax, 0x70'),
    0x151ed4: ('call', '0x1803553a0'),
    0x151ede: ('mov', 'r9d, 0x15'),
    0x151ef7: ('call', 'qword ptr [rax]'),
    0x15225a: ('call', '0x1803553a0'),
    0x15226a: ('mov', 'qword ptr [rsp + 0x60], rax'),
    0x1522c5: ('call', '0x180156ac0'),
    0x14260b: ('lea', 'rcx, [rip + 0x45713e]'),
    0x142612: ('mov', 'qword ptr [rdi + 0x1f8], rcx'),
    0x15a659: ('jmp', '0x18014f1d0'),
    0x15a66b: ('jmp', '0x18014f1d0'),
    0x15a674: ('jmp', '0x18014f1d0'),
    0x15a680: ('jmp', '0x18014f1d0'),
    0x15a690: ('jmp', '0x18014f550'),
    0x35da01: ('movsd', 'xmm6, qword ptr [rip + 0x1e2197]'),
    0x35dc87: ('call', '0x18035e340'),
    0x35e068: ('xor', 'edi, edi'),
    0x35e06a: ('mov', 'qword ptr [rbx + 0x40], rdi'),
    0x35e06e: ('mov', 'byte ptr [rbx + 0x50], 1'),
    0x35e072: ('xorps', 'xmm2, xmm2'),
    0x35e075: ('lea', 'rdx, [rip + 0x23f544]'),
    0x35e081: ('call', '0x18014f1d0'),
    0x35e086: ('xorps', 'xmm2, xmm2'),
    0x35e095: ('call', '0x18014f1d0'),
    0x35e09c: ('cmp', 'byte ptr [r14 + 0x3a], dil'),
    0x35e0a0: ('setne', 'al'),
    0x35e0a7: ('cvtdq2pd', 'xmm2, xmm2'),
    0x35e0cb: ('call', '0x18014f1d0'),
    0x35e0df: ('call', '0x18014f1d0'),
    0x35e12d: ('call', '0x18014f1d0'),
    0x35e141: ('call', '0x18014f1d0'),
    0x35e1cd: ('lea', 'r9, [rsp + 0x20]'),
    0x35e1e4: ('call', '0x18035e370'),
    0x35e34a: ('mov', 'edx, 1'),
    0x35e352: ('call', 'r10'),
    0x35e382: ('xor', 'r9d, r9d'),
    0x35e38e: ('call', 'rbx'),
    0x14f1f7: ('movaps', 'xmm6, xmm2'),
    0x14f27c: ('movsd', 'qword ptr [rbx + 0x40], xmm6'),
    0x14f281: ('mov', 'byte ptr [rbx + 0x50], 1'),
    0x14f5fc: ('mov', 'qword ptr [rbx + 0x48], rsi'),
    0x14f600: ('mov', 'byte ptr [rbx + 0x51], 1'),
    0x14ccfc: ('cmp', 'byte ptr [rbx + 0x51], 0'),
    0x14cd02: ('mov', 'rax, qword ptr [rbx + 0x48]'),
    0x1419d1: ('xor', 'r13d, r13d'),
    0x141a5d: ('call', '0x180140430'),
    0x141a62: ('test', 'al, al'),
    0x141a64: ('jne', '0x180141a81'),
    0x141a66: ('mov', 'qword ptr [r12], r14'),
    0x141a6a: ('mov', 'byte ptr [r12 + 8], al'),
    0x141a6f: ('jmp', '0x180141b29'),
    0x141ae2: ('mov', 'qword ptr [rax + 0x40], r13'),
    0x141ae6: ('mov', 'qword ptr [rax + 0x48], r13'),
    0x141aea: ('mov', 'word ptr [rax + 0x50], 0'),
    0x141b1f: ('mov', 'qword ptr [r12], rax'),
    0x141b23: ('mov', 'byte ptr [r12 + 8], 1'),
    0x141b29: ('mov', 'rax, r12'),
    0x141b43: ('ret', ''),
    0x141b44: ('call', '0x180118a10'),
    0x141b49: ('int3', ''),
    0x157c77: ('mov', 'ecx, 0x28'),
    0x157c7c: ('call', '0x1804e76a0'),
    0x157ca1: ('lea', 'rax, [rip + 0x444650]'),
    0x157ca8: ('mov', 'qword ptr [rdi], rax'),
    0x157ccf: ('lea', 'rdx, [r14 + 0x90]'),
    0x157cd9: ('call', '0x18000cb70'),
    0x157cde: ('mov', 'rcx, qword ptr [rbp + 0x68]'),
    0x157ce2: ('mov', 'rax, qword ptr [rcx + 0x18]'),
    0x157ce6: ('mov', 'qword ptr [rdi + 0x20], rax'),
    0x157f3f: ('mov', 'qword ptr [rdi + 0x18], rax'),
    0x157f4f: ('mov', 'qword ptr [rbp - 0x40], rax'),
    0x157f53: ('mov', 'qword ptr [rbp - 0x38], rdi'),
    0x157f62: ('call', '0x18007b120'),
    0x15dad9: ('mov', 'rcx, qword ptr [rcx + 0x18]'),
    0x15dadd: ('test', 'rcx, rcx'),
    0x15dae0: ('je', '0x18015dae6'),
    0x15dae2: ('call', 'qword ptr [rbx + 0x20]'),
    0x15dae6: ('mov', 'rcx, qword ptr [rbx + 0x10]'),
    0x15daf7: ('mov', 'rax, qword ptr [rcx]'),
    0x15dafa: ('call', 'qword ptr [rax + 0x10]'),
    0xcb90: ('call', 'qword ptr [rax + 8]'),
    0xcb97: ('mov', 'qword ptr [rbx], rdi'),
    0xcba2: ('call', 'qword ptr [rax + 0x10]'),
}
SR_PARAMETER_KEYS = {0x59d5b0: 'Jitter.Offset.X', 0x59d5c0: 'Jitter.Offset.Y',
                     0x5a6718: 'Sharpness', 0x599640: 'Reset',
                     0x59a260: 'MV.Scale.X', 0x59a270: 'MV.Scale.Y',
                     0x59d5e8: 'DLSS.Pre.Exposure', 0x59d5d0: 'DLSS.Exposure.Scale'}
NR_PAYLOAD_EXPORT_NAMES = {
    0x738a10: 'NVSDK_NGX_D3D12_Init_Ext',
    0x55a6a0: 'NVSDK_NGX_D3D12_CreateFeature',
    0x55a760: 'NVSDK_NGX_D3D12_EvaluateFeature',
    0x55a940: 'NVSDK_NGX_D3D12_ReleaseFeature',
    0x55b208: 'NVSDK_NGX_D3D12_Shutdown1',
}
NR_PAYLOAD_IDENTITY_ROWS = (
    (0x71b7a0, 0x71b990, 'e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e',
     0x55bca8, '310.8.0'),
    (0x71b808, 0x55bcc0, '2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f',
     0x55bcb0, '310.9.16'),
)
# Observed slots in this fixed MSVC binary, never inferred from declaration order.
NR_PARAMETER_VTABLE = (0x15a690, 0x15a690, 0x15a690, 0x15a650, 0x15a660,
                       0x15a680, 0x15a670, 0x15a6a0, 0x14cc70, 0x14cc70,
                       0x14cc70, 0x14c920, 0x14c9f0, 0x14cba0, 0x14cac0,
                       0x14cd40, 0x1566c0)
NR_FEATURE_OWNER_VTABLE_PREFIX = (0x15dad0, 0x115df0)
RUNTIME_RANGES.update({
    'dx11-completion-create': (0xec820, 0xecb25),
    'dx11-completion-destroy': (0x851d0, 0x85248),
    'dx11-completion-poll': (0xdd5b0, 0xdd61f),
    'dx11-completion-end': (0xa9a00, 0xa9a24),
    'dx11-completion-cached': (0x9c1e0, 0x9c1e5),
    'nr-payload-import-slot-lookup': (0x355750, 0x3557f1),
    'nr-payload-import-resolve': (0x3575e8, 0x35765d),
    'nr-payload-module-name-replacement': (0x356c30, 0x356d03),
    'nr-payload-module-name-install': (0x357c95, 0x357cd1),
    'nr-payload-after-name-install': (0x357ed0, 0x357f2f),
    'nr-payload-protected-write': (0x35ab30, 0x35abcf),
    'nr-payload-own-module-reference': (0x35a680, 0x35a6ae),
    'nr-payload-loader-state': (0x356fac, 0x356fb4),
    'nr-feature-format-reset-gate': (0x15703b, 0x15706d),
    'nr-feature-reuse-gate': (0x157bdf, 0x157c31),
    'nr-feature-creation-prepare': (0x157c31, 0x157c77),
    'nr-feature-common-parameters': (0x15bdf0, 0x15bf09),
    'nr-feature-local-scaling-callback': (0x15a240, 0x15a272),
    'fg-ffx-configure-hudless': (0x827a, 0x840c),
    'fg-ffx-prepare-admission': (0x84b1, 0x86dd),
    'fg-ffx-prepare-guide-tags': (0x87d4, 0x88ac),
    'fg-frame-slot-exact-key': (0x1000c, 0x1005a),
    'fg-frame-slot-exact-return': (0x1035a, 0x1036d),
    'fg-sl-tag-build-and-submit': (0x15b0d, 0x15c96),
    'fg-sl-resource-admission': (0x16050, 0x16143),
    'fg-sl-tag-export-resolve': (0x10bbe, 0x10bd8),
    'fg-sl-export-module-check': (0x12d60, 0x12dbd),
})
RUNTIME_ANCHORS.update({
    0x827a: ('test', 'rdx, rdx'),
    0x8296: ('mov', 'r9d, 0x90'),
    0x82a4: ('call', 'qword ptr [rip + 0x5330de]'),
    0x82b2: ('cmp', 'qword ptr [rsp + 0x60], 0x90'),
    0x82c1: ('cmp', 'qword ptr [rsp + 0x70], 0x20002'),
    0x82d0: ('cmp', 'byte ptr [rsp + 0xa8], 0'),
    0x8366: ('call', '0x18000ffa0'),
    0x8386: ('mov', 'byte ptr [rbx + 0x2c0], 1'),
    0x83da: ('mov', 'dword ptr [rsp + 0x20], 2'),
    0x83e2: ('xor', 'r8d, r8d'),
    0x83eb: ('call', '0x180015ad0'),
    0x84da: ('call', 'qword ptr [rip + 0x532ea8]'),
    0x84e8: ('cmp', 'qword ptr [rsp + 0x60], 0xb8'),
    0x84f7: ('cmp', 'qword ptr [rbp - 0x40], 0x20004'),
    0x85b7: ('call', '0x18000ffa0'),
    0x85c8: ('cmp', 'byte ptr [rax + 0x240], 0'),
    0x85ea: ('call', 'r8'),
    0x8627: ('call', 'rax'),
    0x86d8: ('mov', 'byte ptr [r14 + 0xc], 1'),
    0x881e: ('mov', 'dword ptr [rsp + 0x20], r15d'),
    0x8823: ('mov', 'r9, qword ptr [rbp + 0x18]'),
    0x8831: ('call', '0x180015ad0'),
    0x885b: ('mov', 'dword ptr [rsp + 0x20], 1'),
    0x8863: ('mov', 'r9, qword ptr [rbp + 0x48]'),
    0x8871: ('call', '0x180015ad0'),
    0x10015: ('and', 'eax, 7'),
    0x10018: ('imul', 'rdi, rax, 0x2b0'),
    0x1002d: ('cmp', 'qword ptr [rsi + 0x348], rdx'),
    0x10034: ('je', '0x180010364'),
    0x10042: ('jne', '0x18001005a'),
    0x1004c: ('je', '0x18001005a'),
    0x10364: ('lea', 'rax, [rsi + 0x348]'),
    0x15b33: ('call', '0x180016050'),
    0x15b4f: ('je', '0x180015b77'),
    0x15b6b: ('call', 'qword ptr [rax]'),
    0x15b8e: ('mov', 'byte ptr [rbp], 0'),
    0x15bf7: ('mov', 'dword ptr [rbp - 0x38], esi'),
    0x15bfa: ('xor', 'eax, 1'),
    0x15bfd: ('mov', 'dword ptr [rbp - 0x34], eax'),
    0x15c23: ('mov', 'r9d, 1'),
    0x15c34: ('mov', 'rcx, qword ptr [rdi + 0x20]'),
    0x15c38: ('call', 'qword ptr [rip + 0xd8b44a]'),
    0x15c3e: ('test', 'eax, eax'),
    0x15c62: ('xor', 'bl, 1'),
    0x160a6: ('call', 'qword ptr [rax + 0x50]'),
    0x160a9: ('cmp', 'dword ptr [rsp + 0x28], 3'),
    0x160b0: ('cmp', 'dword ptr [rsp + 0x4c], 1'),
    0x160ee: ('call', 'qword ptr [rbx + 0x38]'),
    0x160fa: ('cmp', 'rcx, qword ptr [rip + 0xd722cf]'),
    0x10bbe: ('lea', 'rdx, [rip + 0x52c533]'),
    0x10bd1: ('mov', 'qword ptr [rip + 0xd904b0], rax'),
    0x12d6d: ('call', 'qword ptr [rip + 0x528635]'),
    0x12d91: ('call', 'qword ptr [rip + 0x528609]'),
    0x12d9b: ('cmp', 'qword ptr [rsp + 0x40], rdi'),
    0x15703b: ('cmp', 'byte ptr [r15 + 0x80], 0'),
    0x157045: ('cmp', 'dword ptr [r14 + 0x118], 0'),
    0x15704f: ('cmp', 'dword ptr [r14 + 0x130], 0'),
    0x15705c: ('call', '0x1801569b0'),
    0x157061: ('mov', 'byte ptr [r14], 1'),
    0x157be6: ('cmp', 'qword ptr [r14 + 0x158], 0'),
    0x157bf3: ('cmp', 'dword ptr [r14 + 0x170], eax'),
    0x157bff: ('cmp', 'dword ptr [r14 + 0x174], eax'),
    0x157c0b: ('cmp', 'dword ptr [r14 + 0x178], eax'),
    0x157c12: ('je', '0x180157c1c'),
    0x157c17: ('call', '0x1801569b0'),
    0x157c23: ('cmp', 'qword ptr [r14 + 0x158], 0'),
    0x157c2b: ('jne', '0x18015806a'),
    0x157c34: ('call', '0x1801566c0'),
    0x157c4b: ('call', '0x18015bdf0'),
    0x157c56: ('and', 'eax, 0xffffffbe'),
    0x157c5d: ('cvtdq2pd', 'xmm6, xmm6'),
    0x157c72: ('call', '0x18014f1d0'),
    0x15be09: ('lea', 'rdx, [rip + 0x416508]'),
    0x15be26: ('call', '0x18014f1d0'),
    0x15be40: ('call', '0x18014f1d0'),
    0x15be52: ('call', '0x18014f1d0'),
    0x15be64: ('call', '0x18014f1d0'),
    0x15be76: ('call', '0x18014f1d0'),
    0x15be88: ('call', '0x18014f1d0'),
    0x15be8d: ('movsd', 'xmm2, qword ptr [rip + 0x3e3d0b]'),
    0x15bea4: ('lea', 'r8, [rip - 0x1c6b]'),
    0x15beae: ('lea', 'rdx, [rip + 0x43dca3]'),
    0x15beb5: ('call', '0x18014f550'),
    0x15bec0: ('call', '0x18035a390'),
    0x15bec7: ('lea', 'rdx, [rip + 0x43dcb2]'),
    0x15bee1: ('lea', 'rdx, [rip + 0x416500]'),
    0x15bede: ('xorps', 'xmm2, xmm2'),
    0x15bf04: ('jmp', '0x18014f1d0'),
    0x15a244: ('test', 'rcx, rcx'),
    0x15a249: ('mov', 'eax, 0xbad00005'),
    0x15a256: ('lea', 'rdx, [rip + 0x43f8c3]'),
    0x15a25d: ('movss', 'xmm2, dword ptr [rip + 0x3e592f]'),
    0x15a265: ('call', 'qword ptr [rax + 0x30]'),
    0x15a268: ('mov', 'eax, 1'),
    0x35757f: ('mov', 'qword ptr [rsp + 0x68], r15'),
    0x356fac: ('call', '0x18035a450'),
    0x356fb1: ('mov', 'rsi, rax'),
    0x3575e8: ('lea', 'rdx, [rip + 0x3e1441]'),
    0x3575ef: ('mov', 'rdi, qword ptr [rsp + 0x68]'),
    0x3575f7: ('call', '0x180355750'),
    0x3575ff: ('mov', 'qword ptr [rbp - 0x20], rax'),
    0x357603: ('lea', 'rdx, [rip + 0x213f2e]'),
    0x35760d: ('call', '0x180355750'),
    0x355765: ('mov', 'ecx, dword ptr [rax + rcx + 0x90]'),
    0x3557a0: ('mov', 'rax, qword ptr [rbx]'),
    0x3557af: ('add', 'rcx, rax'),
    0x3557b2: ('call', '0x18051ff10'),
    0x3557b9: ('je', '0x1803557e3'),
    0x3557e3: ('mov', 'rax, rdi'),
    0x357c9f: ('mov', 'rcx, qword ptr [rbp - 0x20]'),
    0x357ca3: ('mov', 'rdx, qword ptr [rcx]'),
    0x357ca6: ('mov', 'qword ptr [rsi + 0x148], rdx'),
    0x357cad: ('lea', 'rax, [rip - 0x1084]'),
    0x357cb4: ('mov', 'qword ptr [rsp + 0x50], rax'),
    0x357cb9: ('mov', 'r8d, 8'),
    0x357cc4: ('call', '0x18035ab30'),
    0x357ccb: ('jne', '0x180357ed0'),
    0x357ed0: ('cmp', 'byte ptr [rsp + 0x40], 0'),
    0x357ed5: ('je', '0x180357f2f'),
    0x357ee5: ('lea', 'rax, [rip - 0x165c]'),
    0x357efc: ('call', '0x18035ab30'),
    0x357f03: ('jne', '0x180357f2f'),
    0x356c5f: ('lea', 'rdx, [rip + 0x3a1a]'),
    0x356c66: ('mov', 'ecx, 6'),
    0x356c6b: ('call', 'qword ptr [rip + 0x1e472f]'),
    0x356c71: ('cmp', 'rsi, qword ptr [rsp + 0x30]'),
    0x356c76: ('jne', '0x180356cdf'),
    0x356c81: ('cmp', 'edi, 0xa'),
    0x356c84: ('jae', '0x180356ca8'),
    0x356ca8: ('mov', 'dword ptr [rbx], 0x76006e'),
    0x356cae: ('mov', 'eax, 9'),
    0x356cb3: ('mov', 'dword ptr [rbx + 4], 0x67006e'),
    0x356cba: ('mov', 'dword ptr [rbx + 8], 0x2e0078'),
    0x356cc1: ('mov', 'dword ptr [rbx + 0xc], 0x6c0064'),
    0x356cc8: ('mov', 'dword ptr [rbx + 0x10], 0x6c'),
    0x356cdf: ('call', '0x18035a450'),
    0x356cfc: ('jmp', 'qword ptr [rax + 0x148]'),
    0x35a692: ('lea', 'rdx, [rip - 0x19]'),
    0x35a69e: ('call', 'qword ptr [rip + 0x1e0cfc]'),
    0x35ab55: ('mov', 'r8d, 0x40'),
    0x35ab5e: ('call', 'qword ptr [rip + 0x1e0934]'),
    0x35ab71: ('call', '0x18051f420'),
    0x35ab86: ('call', 'qword ptr [rip + 0x1e090c]'),
    0x35ab9d: ('call', 'qword ptr [rip + 0x1e09fd]'),
    0xec89e: ('call', 'qword ptr [rax + 0x18]'),
    0xec8f9: ('lea', 'rax, [rip + 0x46d008]'),
    0xec910: ('call', 'qword ptr [rip + 0x44eaba]'),
    0xec916: ('mov', 'dword ptr [rbx + 0x30], eax'),
    0xec919: ('mov', 'word ptr [rbx + 0x34], 0'),
    0xec96c: ('call', 'qword ptr [r13 + 0xc0]'),
    0xec99a: ('mov', 'qword ptr [rbx + 0x18], rsi'),
    0xec9d3: ('call', 'qword ptr [r15 + 0x140]'),
    0xeca5a: ('mov', 'qword ptr [rdi], r12'),
    0xdd5c0: ('call', 'qword ptr [rip + 0x45de0a]'),
    0xdd5c6: ('cmp', 'eax, ebx'),
    0xdd5ca: ('movzx', 'eax, byte ptr [rdi + 0x24]'),
    0xdd5d2: ('movzx', 'eax, byte ptr [rdi + 0x25]'),
    0xdd5da: ('mov', 'dword ptr [rsp + 0x40], 0'),
    0xdd5e2: ('mov', 'rcx, qword ptr [rdi + 0x10]'),
    0xdd5e9: ('mov', 'dword ptr [rsp + 0x20], 1'),
    0xdd5f1: ('mov', 'r9d, 4'),
    0xdd5fc: ('mov', 'rdx, qword ptr [rdi + 0x18]'),
    0xdd600: ('call', 'qword ptr [rax + 0xe8]'),
    0xdd606: ('test', 'eax, eax'),
    0xdd60a: ('cmp', 'dword ptr [rsp + 0x40], eax'),
    0xdd610: ('mov', 'byte ptr [rdi + 0x25], 1'),
    0xa9a09: ('mov', 'rcx, qword ptr [rcx + 8]'),
    0xa9a10: ('mov', 'rdx, qword ptr [rbx + 0x18]'),
    0xa9a14: ('call', 'qword ptr [rax + 0xe0]'),
    0xa9a1a: ('mov', 'byte ptr [rbx + 0x24], 1'),
    0x9c1e0: ('movzx', 'eax, byte ptr [rcx + 0x25]'),
    0x851f3: ('call', 'qword ptr [rax + 0x10]'),
    0x8520b: ('call', 'qword ptr [rax + 0x10]'),
    0x85223: ('call', 'qword ptr [rax + 0x10]'),
    0x14275: ('call', 'qword ptr [rax + 0x40]'),
    0x14278: ('cmp', 'rax, r15'),
    0x14287: ('call', 'qword ptr [rip + 0x5270db]'),
    0x142a1: ('call', 'qword ptr [r9 + 0x48]'),
    0x142a9: ('mov', 'edx, 0x1388'),
    0x142b1: ('call', 'qword ptr [rip + 0x5270a9]'),
    0x142b7: ('test', 'eax, eax'),
    0x142cb: ('call', 'qword ptr [rip + 0x52706f]'),
    0x142e5: ('call', '0x18000eea0'),
    0x142ea: ('mov', 'eax, 0x887a0006'),
})
COMPLETION_API_SLOTS = {
    'd3d11.h': {
        'ID3D11Device': {'CreateQuery': 0xc0, 'GetImmediateContext': 0x140},
        'ID3D11DeviceContext': {'GetDevice': 0x18, 'End': 0xe0, 'GetData': 0xe8},
    },
    'd3d12.h': {'ID3D12Fence': {'GetCompletedValue': 0x40, 'SetEventOnCompletion': 0x48}},
}
SHADERS = ('feed-preprocess', 'feed-guides', 'encore-pso-38',
           'encore-pso-40', 'encore-pso-48')

def sha(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def public_vtable_slots(source: str, interface: str, expected: dict[str, int]) -> list[dict]:
    """Read the C-interface order from an explicit SDK header, for Win64 only."""
    try:
        tail = source.split('typedef struct '+interface+'Vtbl', 1)[1]
    except IndexError as error:
        raise ValueError('missing C vtable: '+interface) from error
    body, closing, _ = tail.partition('} '+interface+'Vtbl')
    if not closing:
        raise ValueError('unterminated C vtable: '+interface)
    methods = re.findall(r'[*]([A-Za-z_][A-Za-z_0-9]*)[ ]*[)][(]', body)
    result = []
    for name, offset in expected.items():
        if methods.count(name) != 1 or methods.index(name)*8 != offset:
            raise ValueError('public Win64 vtable slot changed: '+interface+'::'+name)
        result.append(dict(method=name, slot=methods.index(name), byte_offset=hex(offset)))
    return result


def completion_contract(pe, base: int, header_root: Path) -> tuple[dict, dict[str, str]]:
    slots = []
    for index, expected in enumerate((0x851d0, 0x9c1e0, 0xdd5b0, 0xa9a00)):
        address = 0x559908+index*8
        raw = pe.get_data(address, 8)
        if struct.unpack('<Q', raw)[0] != base+expected:
            raise ValueError('D11Completion vtable prefix changed')
        slots.append(dict(slot=hex(index*8), table_rva=hex(address), bytes=raw.hex(),
                          function_rva=hex(expected)))
    col = pe.get_data(0xcf74f8, 24)
    pointer = pe.get_data(0x559900, 8)
    name = '.?AUD11Completion@ngx_live_preset@@'
    name_raw = pe.get_data(0xd80478, len(name)+1)
    if (struct.unpack('<6I', col) != (1, 0, 0, 0xd80468, 0xcf7520, 0xcf74f8) or
            struct.unpack('<Q', pointer)[0] != base+0xcf74f8 or
            name_raw != name.encode('ascii')+bytes(1)):
        raise ValueError('D11Completion RTTI changed')
    pe.parse_data_directories(directories=[1])
    imports = {entry.address-base: (module.dll.decode('ascii'), entry.name.decode('ascii'))
               for module in pe.DIRECTORY_ENTRY_IMPORT for entry in module.imports if entry.name}
    expected_imports = {0x53b3d0: ('KERNEL32.dll', 'GetCurrentThreadId'),
                        0x53b360: ('KERNEL32.dll', 'WaitForSingleObject'),
                        0x53b368: ('KERNEL32.dll', 'CreateEventW'),
                        0x53b340: ('KERNEL32.dll', 'CloseHandle')}
    if any(imports.get(rva) != expected for rva, expected in expected_imports.items()):
        raise ValueError('completion Win32 import changed')
    headers, hashes = {}, {}
    for file, interfaces in COMPLETION_API_SLOTS.items():
        path = (header_root/file).resolve()
        header_bytes = path.read_bytes()
        source = header_bytes.decode('utf-8-sig')
        header_hash = hashlib.sha256(header_bytes).hexdigest()
        headers[file] = dict(path=str(path), sha256=header_hash,
                             interfaces={name: public_vtable_slots(source, name, methods)
                                         for name, methods in interfaces.items()})
        hashes[str(path)] = header_hash
        if file == 'd3d11.h':
            compact = ''.join(source.split())
            flag = re.search(r'D3D11_ASYNC_GETDATA_DONOTFLUSH=(0x[0-9a-fA-F]+|[0-9]+)[,}]', compact)
            if flag is None or int(flag[1], 0) != 1:
                raise ValueError('D3D11 DONOTFLUSH flag changed')
    report = dict(vtable_prefix=slots,
        rtti=dict(locator_rva='0xcf74f8', locator_bytes=col.hex(), locator_pointer_rva='0x559900',
                  locator_pointer_bytes=pointer.hex(), type_descriptor_rva='0xd80468',
                  name_rva='0xd80478', name_bytes=name_raw.hex(), name=name),
        imports=[dict(iat_rva=hex(rva), dll=dll, name=name)
                 for rva, (dll, name) in expected_imports.items()],
        public_api_headers=headers, query_flag=dict(name='D3D11_ASYNC_GETDATA_DONOTFLUSH', value=1),
        target_executed=False, gpu_tested=False, performance_measured=False,
        caller_schedule_authenticated=False, f7_causality_proven=False)
    return report, hashes


def nr_import_contract(pe, base: int) -> dict:
    names = []
    for rva, expected in ((0x738a30, 'GetModuleFileNameW'), (0x56b538, 'GetProcAddress')):
        raw = pe.get_data(rva, len(expected)+1)
        if raw != expected.encode('ascii')+bytes(1):
            raise ValueError('NR loader import name changed')
        names.append(dict(rva=hex(rva), bytes=raw.hex(), name=expected))
    pe.parse_data_directories(directories=[1])
    imports = {entry.address-base: (module.dll.decode('ascii'), entry.name.decode('ascii'))
               for module in pe.DIRECTORY_ENTRY_IMPORT for entry in module.imports if entry.name}
    expected_imports = {0x53b3a0: ('KERNEL32.dll', 'GetModuleHandleExW'),
                        0x53b448: ('KERNEL32.dll', 'SetLastError'),
                        0x53b498: ('KERNEL32.dll', 'VirtualProtect'),
                        0x53b370: ('KERNEL32.dll', 'GetCurrentProcess'),
                        0x53b5a0: ('KERNEL32.dll', 'FlushInstructionCache')}
    if any(imports.get(rva) != expected for rva, expected in expected_imports.items()):
        raise ValueError('NR loader import-write API changed')
    return dict(import_names=names,
        imports=[dict(iat_rva=hex(rva), dll=dll, name=name)
                 for rva, (dll, name) in expected_imports.items()],
        replacement_rva='0x356c30', install_rva='0x357cc4',
        original_function_state_offset='0x148', installed_pointer_bytes=8,
        target_executed=False, payload_executed=False, gpu_tested=False,
        cross_payload_abi_authenticated=False, f7_integration_ready=False,
        scope='Fixed Encore import lookup, installation and module-name replacement only; not an implementation to transplant')


def _function_records(pe, expected: dict[str, tuple[int, int, int]], label: str) -> list[dict]:
    pe.parse_data_directories(directories=[3])
    functions = [(entry.struct.BeginAddress, entry.struct.EndAddress, entry.struct.UnwindData)
                 for entry in pe.DIRECTORY_ENTRY_EXCEPTION]
    records = []
    for name, record in expected.items():
        if functions.count(record) != 1:
            raise ValueError(label+' function boundary mismatch: '+name)
        records.append(dict(name=name, begin=hex(record[0]), end=hex(record[1]), unwind=hex(record[2])))
    return records


FG_INPUT_FENCE_INSTRUCTIONS = (
    (0x141ae, '4533ff', 'xor', 'r15d, r15d'),
    (0x141b1, '4c897db0', 'mov', 'qword ptr [rbp - 0x50], r15'),
    (0x141b5, 'c745b8e1c88acc', 'mov', 'dword ptr [rbp - 0x48], 0xcc8ac8e1'),
    (0x141bc, 'c745bc79a1f544', 'mov', 'dword ptr [rbp - 0x44], 0x44f5a179'),
    (0x141c3, 'c745c097fae741', 'mov', 'dword ptr [rbp - 0x40], 0x41e7fa97'),
    (0x141ca, 'c745c412f9bc61', 'mov', 'dword ptr [rbp - 0x3c], 0x61bcf912'),
    (0x141d1, '48c745c804000000', 'mov', 'qword ptr [rbp - 0x38], 4'),
    (0x141d9, '4c897dd0', 'mov', 'qword ptr [rbp - 0x30], r15'),
    (0x141dd, '4c897dd8', 'mov', 'qword ptr [rbp - 0x28], r15'),
    (0x141e1, '4c897de0', 'mov', 'qword ptr [rbp - 0x20], r15'),
    (0x141e5, '6644897de8', 'mov', 'word ptr [rbp - 0x18], r15w'),
    (0x141ea, '4c897df0', 'mov', 'qword ptr [rbp - 0x10], r15'),
    (0x141ee, '4c897df8', 'mov', 'qword ptr [rbp - 8], r15'),
    (0x141f2, '44887d00', 'mov', 'byte ptr [rbp], r15b'),
    (0x141f6, '4c897d90', 'mov', 'qword ptr [rbp - 0x70], r15'),
    (0x141fa, '4c897d88', 'mov', 'qword ptr [rbp - 0x78], r15'),
    (0x141fe, '4c897d98', 'mov', 'qword ptr [rbp - 0x68], r15'),
    (0x14202, '4c897da0', 'mov', 'qword ptr [rbp - 0x60], r15'),
    (0x14206, '488d4d90', 'lea', 'rcx, [rbp - 0x70]'),
    (0x1420a, 'ff1540715200', 'call', 'qword ptr [rip + 0x527140]'),
    (0x14210, '4533c0', 'xor', 'r8d, r8d'),
    (0x14213, '488d55b0', 'lea', 'rdx, [rbp - 0x50]'),
    (0x14217, '488d0db21ed500', 'lea', 'rcx, [rip + 0xd51eb2]'),
    (0x1421e, 'ff1584ced800', 'call', 'qword ptr [rip + 0xd8ce84]'),
    (0x14224, '448be0', 'mov', 'r12d, eax'),
    (0x14227, '488d4d88', 'lea', 'rcx, [rbp - 0x78]'),
    (0x1422b, 'ff151f715200', 'call', 'qword ptr [rip + 0x52711f]'),
    (0x14231, '418bdf', 'mov', 'ebx, r15d'),
    (0x14234, '44387e18', 'cmp', 'byte ptr [rsi + 0x18], r15b'),
    (0x14238, '0f85b9000000', 'jne', '0x1800142f7'),
    (0x1423e, '41389c3686050000', 'cmp', 'byte ptr [r14 + rsi + 0x586], bl'),
    (0x14246, '0f84ab000000', 'je', '0x1800142f7'),
    (0x1424c, '4585e4', 'test', 'r12d, r12d'),
    (0x1424f, '0f85a2000000', 'jne', '0x1800142f7'),
    (0x14255, '488b7df0', 'mov', 'rdi, qword ptr [rbp - 0x10]'),
    (0x14259, '4885ff', 'test', 'rdi, rdi'),
    (0x1425c, '0f8495000000', 'je', '0x1800142f7'),
    (0x14262, '4c8b7df8', 'mov', 'r15, qword ptr [rbp - 8]'),
    (0x14266, '4d85ff', 'test', 'r15, r15'),
    (0x14269, '0f8485000000', 'je', '0x1800142f4'),
    (0x1426f, '488b07', 'mov', 'rax, qword ptr [rdi]'),
    (0x14272, '488bcf', 'mov', 'rcx, rdi'),
    (0x14275, 'ff5040', 'call', 'qword ptr [rax + 0x40]'),
    (0x14278, '493bc7', 'cmp', 'rax, r15'),
    (0x1427b, '7377', 'jae', '0x1800142f4'),
    (0x1427d, '4533c9', 'xor', 'r9d, r9d'),
    (0x14280, '4533c0', 'xor', 'r8d, r8d'),
    (0x14283, '33d2', 'xor', 'edx, edx'),
    (0x14285, '33c9', 'xor', 'ecx, ecx'),
    (0x14287, 'ff15db705200', 'call', 'qword ptr [rip + 0x5270db]'),
    (0x1428d, '488bd8', 'mov', 'rbx, rax'),
    (0x14290, '4885c0', 'test', 'rax, rax'),
    (0x14293, '742b', 'je', '0x1800142c0'),
    (0x14295, '4c8b0f', 'mov', 'r9, qword ptr [rdi]'),
    (0x14298, '4c8bc0', 'mov', 'r8, rax'),
    (0x1429b, '498bd7', 'mov', 'rdx, r15'),
    (0x1429e, '488bcf', 'mov', 'rcx, rdi'),
    (0x142a1, '41ff5148', 'call', 'qword ptr [r9 + 0x48]'),
    (0x142a5, '85c0', 'test', 'eax, eax'),
    (0x142a7, '7817', 'js', '0x1800142c0'),
    (0x142a9, 'ba88130000', 'mov', 'edx, 0x1388'),
    (0x142ae, '488bcb', 'mov', 'rcx, rbx'),
    (0x142b1, 'ff15a9705200', 'call', 'qword ptr [rip + 0x5270a9]'),
    (0x142b7, '85c0', 'test', 'eax, eax'),
    (0x142b9, '7505', 'jne', '0x1800142c0'),
    (0x142bb, '40b701', 'mov', 'dil, 1'),
    (0x142be, 'eb08', 'jmp', '0x1800142c8'),
    (0x142c0, '4032ff', 'xor', 'dil, dil'),
    (0x142c3, '4885db', 'test', 'rbx, rbx'),
    (0x142c6, '7409', 'je', '0x1800142d1'),
    (0x142c8, '488bcb', 'mov', 'rcx, rbx'),
    (0x142cb, 'ff156f705200', 'call', 'qword ptr [rip + 0x52706f]'),
    (0x142d1, 'bb01000000', 'mov', 'ebx, 1'),
    (0x142d6, '4084ff', 'test', 'dil, dil'),
    (0x142d9, '7519', 'jne', '0x1800142f4'),
    (0x142db, '488d15e69c5200', 'lea', 'rdx, [rip + 0x529ce6]'),
    (0x142e2, '488bce', 'mov', 'rcx, rsi'),
    (0x142e5, 'e8b6abffff', 'call', '0x18000eea0'),
    (0x142ea, 'b806007a88', 'mov', 'eax, 0x887a0006'),
    (0x142ef, 'e9ed070000', 'jmp', '0x180014ae1'),
    (0x142f4, '4533ff', 'xor', 'r15d, r15d'),
    (0x14d1d, '488d0d7c7b5200', 'lea', 'rcx, [rip + 0x527b7c]'),
    (0x14d24, 'ffd7', 'call', 'rdi'),
    (0x14d26, '48897528', 'mov', 'qword ptr [rbp + 0x28], rsi'),
    (0x14d2a, '488bf8', 'mov', 'rdi, rax'),
    (0x14d2d, '4885c0', 'test', 'rax, rax'),
    (0x14d30, '0f8411020000', 'je', '0x180014f47'),
    (0x14d36, '4c8d4528', 'lea', 'r8, [rbp + 0x28]'),
    (0x14d3a, '488bd0', 'mov', 'rdx, rax'),
    (0x14d3d, 'b906000000', 'mov', 'ecx, 6'),
    (0x14d42, 'ff1558665200', 'call', 'qword ptr [rip + 0x526658]'),
    (0x14d48, '85c0', 'test', 'eax, eax'),
    (0x14d4a, '0f84f7010000', 'je', '0x180014f47'),
    (0x14d50, '48395d28', 'cmp', 'qword ptr [rbp + 0x28], rbx'),
    (0x14d54, '0f85ed010000', 'jne', '0x180014f47'),
    (0x14d5a, '488d0df7785200', 'lea', 'rcx, [rip + 0x5278f7]'),
    (0x14d61, '48893d40c3d800', 'mov', 'qword ptr [rip + 0xd8c340], rdi'),
)


def fg_input_fence_contract(pe, base: int, d3d12_header: str) -> dict:
    """Bind fixed-target CPU wait branches, never a runtime retirement certificate."""
    def bind(rva: int, expected: bytes, label: str) -> dict:
        raw = pe.get_data(rva, len(expected))
        if raw != expected:
            raise ValueError('FG input-fence '+label+' bytes changed: '+hex(rva))
        return dict(rva=hex(rva), bytes=raw.hex())

    instructions = [dict(bind(rva, bytes.fromhex(raw), 'instruction'),
                         mnemonic=mnemonic, operands=operands)
                    for rva, raw, mnemonic, operands in FG_INPUT_FENCE_INSTRUCTIONS]
    slots = public_vtable_slots(d3d12_header, 'ID3D12Fence',
                               {'GetCompletedValue': 0x40, 'SetEventOnCompletion': 0x48})
    pe.parse_data_directories(directories=[1])
    imports = {entry.address-base: (module.dll.decode('ascii'), entry.name.decode('ascii'))
               for module in pe.DIRECTORY_ENTRY_IMPORT for entry in module.imports if entry.name}
    expected_imports = {0x53b368: ('KERNEL32.dll', 'CreateEventW'),
                        0x53b360: ('KERNEL32.dll', 'WaitForSingleObject'),
                        0x53b340: ('KERNEL32.dll', 'CloseHandle'),
                        0x53b3a0: ('KERNEL32.dll', 'GetModuleHandleExW')}
    if any(imports.get(rva) != expected for rva, expected in expected_imports.items()):
        raise ValueError('FG input-fence import changed')
    records = _function_records(pe, {
        'post_present': (0x140d0, 0x14b04, 0xd04200),
        'state_resolver_enclosing_function': (0x14c20, 0x14f80, 0xd03ccc),
    }, 'FG input-fence')
    for record, pdata, unwind in zip(records, (0x1667c6c, 0x1667c84), (
            '19330a0022016b060df00be009d007c00570046003300250f07b4e00',
            '010f06000f340a000f32087007600650')):
        expected = struct.pack('<3I', *(int(record[key], 16) for key in ('begin', 'end', 'unwind')))
        record['pdata'] = bind(pdata, expected, 'pdata')
        record['unwind_record'] = bind(int(record['unwind'], 16), bytes.fromhex(unwind), 'unwind')
    name = bind(0x53c8a0, b'slDLSSGGetState\0', 'resolver name')
    name['name'] = 'slDLSSGGetState'
    paths = [
        dict(name='owner_gate', compare_rva='0x14234', branch_rva='0x14238',
             required='owner byte +0x18 equals zero', bypass_rva='0x142f7'),
        dict(name='current_flags_gate', compare_rva='0x1423e', branch_rva='0x14246',
             required='byte at r14+rsi+0x586 is nonzero', bypass_rva='0x142f7'),
        dict(name='get_state_result_gate', compare_rva='0x1424c', branch_rva='0x1424f',
             required='saved EAX in r12d equals zero', bypass_rva='0x142f7'),
        dict(name='null_fence', compare_rva='0x14259', branch_rva='0x1425c', bypass_rva='0x142f7'),
        dict(name='zero_target', compare_rva='0x14266', branch_rva='0x14269', bypass_rva='0x142f4'),
        dict(name='completed_value', call_rva='0x14275', compare_rva='0x14278', branch_rva='0x1427b',
             predicate='uint64 completed >= uint64 target', bypass_rva='0x142f4',
             uint64_max_passes_compare=True, sentinel_guard_in_query_window=False,
             exact_query_window_begin='0x1426f', exact_query_window_end='0x1427d'),
        dict(name='create_event', call_rva='0x14287', compare_rva='0x14290', branch_rva='0x14293',
             null_failure_rva='0x142c0', arguments=[0, 0, 0, 0]),
        dict(name='set_event', call_rva='0x142a1', compare_rva='0x142a5', branch_rva='0x142a7',
             predicate='signed HRESULT < 0 fails', failure_rva='0x142c0'),
        dict(name='wait', call_rva='0x142b1', timeout_ms=5000, timeout_instruction_rva='0x142a9',
             compare_rva='0x142b7', branch_rva='0x142b9', accepted_result=0,
             accepted_result_name='WAIT_OBJECT_0', other_result='failure; exact reason not classified',
             failure_rva='0x142c0', success_marker_rva='0x142bb'),
        dict(name='close_nonnull_event', null_check_rva='0x142c3', branch_rva='0x142c6',
             call_rva='0x142cb', result_checked=False,
             scope='normal paths with a nonnull created event; dynamic callee behavior unknown'),
        dict(name='wait_preparation_or_wait_failure', compare_rva='0x142d6', branch_rva='0x142d9',
             helper_call_rva='0x142e5', helper_rva='0xeea0', return_instruction_rva='0x142ea',
             return_value='0x887a0006', exit_branch_rva='0x142ef', exit_rva='0x14ae1'),
        dict(name='wait_success_tail', begin='0x142b7', end='0x142f4',
             fence_requery_present=False, scope='exact normal instruction path only'),
    ]
    return dict(instructions=instructions, imports=[dict(iat_rva=hex(rva), dll=dll, name=name)
                for rva, (dll, name) in expected_imports.items()], function_records=records,
        public_fence_slots=slots, paths=paths,
        state_layout=dict(base='rbp-0x50', guid='e1c88acc79a1f54497fae74112f9bc61',
                          version=4, version_instruction_rva='0x141d1', pointer_offset='0x40',
                          pointer_local='rbp-0x10', target_offset='0x48', target_local='rbp-0x8',
                          get_state_call_rva='0x1421e', saved_result_rva='0x14224'),
        state_resolver=dict(name=name, observed_slot_rva='0xda10a8', call_rva='0x1421e',
            bounded_begin='0x14d1d', bounded_end='0x14d68', indirect_resolver_call_rva='0x14d24',
            flags=6, module_identity_call_rva='0x14d42', module_compare_rva='0x14d50',
            null_branch_rva='0x14d30', module_query_failure_branch_rva='0x14d4a',
            module_mismatch_branch_rva='0x14d54', rejection_rva='0x14f47', commit_rva='0x14d61',
            dynamic_callee_safety_proven=False, module_pinned=False),
        target_executed=False, gpu_tested=False, cpu_stall_measured=False,
        f7_causality_proven=False, resource_retirement_proven=False, safe_unload_proven=False,
        scope='Fixed Encore normal CPU branches and bounded resolver only; no dynamic callee, GPU completion or runtime safety proof')


def nr_device_holder_contract(pe, d3d12_header: str) -> dict:
    """Bind the observed CPU device/feature holds; never certify GPU retirement."""
    guid = pe.get_data(0x53ce20, 16)
    if guid.hex() != 'f1199818b61d574bbe541821339b85f7':
        raise ValueError('NR device GUID mismatch')
    slots = public_vtable_slots(d3d12_header, 'ID3D12DeviceChild',
                               {'AddRef': 8, 'Release': 16, 'GetDevice': 0x38})
    expected = {
        'output_address_helper': (0x92d0, 0x92f8, 0xd03a14),
        'device_admission_caller': (0x151020, 0x15255e, 0xd19dc4),
        'feature_create_evaluate_caller': (0x156ac0, 0x159a3f, 0xd197c4),
        'feature_release_callback': (0x15dad0, 0x15db04, 0xd1a014),
        'runtime_destructor': (0x143430, 0x14369e, 0xd19570),
    }
    records = _function_records(pe, expected, 'NR device-holder')
    return dict(device_guid=dict(rva='0x53ce20', bytes=guid.hex(), name='ID3D12Device'),
        public_device_child_slots=slots, function_records=records,
        device_acquisition=dict(call_rva='0x151436', output_slot='rsp+0x70',
                                expected_device_compare_rva='0x15144a'),
        runtime_device_hold=dict(field='runtime+0x90', store_rva='0x151918',
                                 addref_rva='0x15190d', old_release_rva='0x151927'),
        feature_device_hold=dict(field='holder+0x10', copy_call_rva='0x157cd9',
                                 release_feature_rva='0x15dae2', release_device_rva='0x15dafa'),
        runtime_drop_order=dict(feature_holder_field='runtime+0x168',
                                conditional_feature_callback_rva='0x1434eb',
                                device_field='runtime+0x90', device_release_rva='0x143645'),
        target_executed=False, payload_executed=False, gpu_tested=False,
        resource_retirement_proven=False, module_lifetime_proven=False,
        shutdown_contract_proven=False, complete_session_lifetime_proven=False,
        scope='Fixed-target CPU device references and conditional holder destruction only; no GPU or session retirement certificate')


# Analyst labels for these fixed direct paths only. The failure helper bodies
# deliberately remain outside RUNTIME_RANGES; these calls do not recover CRT.
NR_CLEANUP_DIRECT_EDGES = (
    (0x539bcb, 0xc7b0, 'call', 'pointer-buffer'),
    (0x539bf8, 0x4e76f0, 'call', 'linked-node buffer'),
    (0x539c16, 0x4e76f0, 'call', 'linked-node root buffer'),
    (0x539c22, 0x18200, 'call', 'first wide-buffer'),
    (0x539c2e, 0x35ae70, 'call', 'config-record vector'),
    (0x539c3e, 0x18200, 'jmp', 'tail wide-buffer'),
    (0x35ae9b, 0xc6c0, 'call', 'record inner narrow-buffer'),
    (0x35af09, 0x4e76f0, 'call', 'config backing buffer'),
    (0xc707, 0x4e76f0, 'call', 'narrow backing buffer'),
    (0xc800, 0x4e76f0, 'call', 'pointer backing buffer'),
    (0x1824f, 0x4e76f0, 'call', 'wide backing buffer'),
    (0x50b6e8, 0x4f6c7c, 'call', 'UNKNOWN failure helper'),
    (0x50b6ef, 0x4f6dbc, 'call', 'UNKNOWN failure helper'),
)
NR_CLEANUP_THUNKS = ((0x4e76f0, 0x4e80a0), (0x4e80a0, 0x4f7de0), (0x4f7de0, 0x50b6c0))
NR_CLEANUP_IMPORT_CALLS = ((0x50b6d6, 0x53b668, 'HeapFree'),
                           (0x50b6e0, 0x53b348, 'GetLastError'))
# Raw instruction bytes bind the offsets, stride, size and branch observations
# independently of the mnemonic assertions used by the runtime profile decode.
NR_CLEANUP_BINDINGS = {
    0x539bc4: ('488d0d95a41201', 'lea', 'rcx, [rip + 0x112a495]', 'pointer-buffer at 0x1664060'),
    0x539bd0: ('488b0d79a41201', 'mov', 'rcx, qword ptr [rip + 0x112a479]', 'node root at 0x1664050'),
    0x539bd7: ('488b4108', 'mov', 'rax, qword ptr [rcx + 8]', 'node root link +0x8'),
    0x539bdb: ('48c70000000000', 'mov', 'qword ptr [rax], 0', 'clear pointed-to link'),
    0x539be2: ('488b09', 'mov', 'rcx, qword ptr [rcx]', 'node root +0x0'),
    0x539bf0: ('488b19', 'mov', 'rbx, qword ptr [rcx]', 'next node +0x0 before free'),
    0x539bf3: ('ba20000000', 'mov', 'edx, 0x20', 'node allocation size argument'),
    0x539c03: ('75eb', 'jne', '0x180539bf0', 'continue non-null node traversal'),
    0x539c0a: ('488b0d3fa41201', 'mov', 'rcx, qword ptr [rip + 0x112a43f]', 'node root backing pointer'),
    0x539c11: ('ba20000000', 'mov', 'edx, 0x20', 'root allocation size argument'),
    0x539c1b: ('488d0d3ea31201', 'lea', 'rcx, [rip + 0x112a33e]', 'wide-buffer at 0x1663f60'),
    0x539c27: ('488d0d1aa31201', 'lea', 'rcx, [rip + 0x112a31a]', 'config vector at 0x1663f48'),
    0x539c33: ('488d0d26a21201', 'lea', 'rcx, [rip + 0x112a226]', 'wide-buffer at 0x1663e60'),
    0xc6c9: ('488b4918', 'mov', 'rcx, qword ptr [rcx + 0x18]', 'narrow capacity +0x18'),
    0xc6cd: ('4883f90f', 'cmp', 'rcx, 0xf', 'narrow inline capacity 15'),
    0xc6d1: ('7639', 'jbe', '0x18000c70c', 'narrow inline skips free'),
    0xc6d3: ('488b03', 'mov', 'rax, qword ptr [rbx]', 'narrow backing pointer +0x0'),
    0xc6d6: ('488d5101', 'lea', 'rdx, [rcx + 1]', 'narrow capacity plus terminator'),
    0xc70c: ('48c7431000000000', 'mov', 'qword ptr [rbx + 0x10], 0', 'narrow length +0x10 reset'),
    0xc714: ('48c743180f000000', 'mov', 'qword ptr [rbx + 0x18], 0xf', 'narrow capacity reset'),
    0xc71c: ('c60300', 'mov', 'byte ptr [rbx], 0', 'narrow first byte reset'),
    0xc7b6: ('488b11', 'mov', 'rdx, qword ptr [rcx]', 'pointer-buffer begin +0x0'),
    0xc7bf: ('7451', 'je', '0x18000c812', 'null pointer-buffer skips free/reset'),
    0xc7c1: ('488b4110', 'mov', 'rax, qword ptr [rcx + 0x10]', 'pointer-buffer allocation end +0x10'),
    0xc7c5: ('482bc2', 'sub', 'rax, rdx', 'pointer-buffer allocated byte span'),
    0xc7c8: ('48c1f803', 'sar', 'rax, 3', 'pointer element count divides by 8'),
    0xc7cc: ('488d04c500000000', 'lea', 'rax, [rax*8]', 'pointer element stride 8'),
    0xc805: ('33c0', 'xor', 'eax, eax', 'pointer reset value zero'),
    0xc807: ('488903', 'mov', 'qword ptr [rbx], rax', 'pointer begin +0x0 reset'),
    0xc80a: ('48894308', 'mov', 'qword ptr [rbx + 8], rax', 'pointer end +0x8 reset'),
    0xc80e: ('48894310', 'mov', 'qword ptr [rbx + 0x10], rax', 'pointer allocation end +0x10 reset'),
    0x18209: ('488b4918', 'mov', 'rcx, qword ptr [rcx + 0x18]', 'wide capacity +0x18'),
    0x1820d: ('4883f907', 'cmp', 'rcx, 7', 'wide inline capacity 7'),
    0x18211: ('7641', 'jbe', '0x180018254', 'wide inline skips free'),
    0x18213: ('488b03', 'mov', 'rax, qword ptr [rbx]', 'wide backing pointer +0x0'),
    0x18216: ('488d144d02000000', 'lea', 'rdx, [rcx*2 + 2]', 'wide 2-byte units plus terminator'),
    0x18254: ('33c0', 'xor', 'eax, eax', 'wide reset value zero'),
    0x18256: ('48c7431000000000', 'mov', 'qword ptr [rbx + 0x10], 0', 'wide length +0x10 reset'),
    0x1825e: ('668903', 'mov', 'word ptr [rbx], ax', 'wide first 2-byte unit reset'),
    0x18261: ('48c7431807000000', 'mov', 'qword ptr [rbx + 0x18], 7', 'wide capacity reset'),
    0x35ae7a: ('488b19', 'mov', 'rbx, qword ptr [rcx]', 'config begin +0x0'),
    0x35ae83: ('0f8492000000', 'je', '0x18035af1b', 'null config begin skips cleanup'),
    0x35ae8e: ('488b7908', 'mov', 'rdi, qword ptr [rcx + 8]', 'config end +0x8'),
    0x35ae95: ('7412', 'je', '0x18035aea9', 'empty config vector skips inner cleanup'),
    0x35ae97: ('488d4b18', 'lea', 'rcx, [rbx + 0x18]', 'config inner narrow-buffer +0x18'),
    0x35aea0: ('4883c348', 'add', 'rbx, 0x48', 'config record stride 0x48'),
    0x35aea7: ('75ee', 'jne', '0x18035ae97', 'repeat inner cleanup up to end'),
    0x35aea9: ('4c8b06', 'mov', 'r8, qword ptr [rsi]', 'config backing pointer +0x0'),
    0x35aeb6: ('488b4e10', 'mov', 'rcx, qword ptr [rsi + 0x10]', 'config allocation end +0x10'),
    0x35af0e: ('33c0', 'xor', 'eax, eax', 'config reset value zero'),
    0x35af10: ('488906', 'mov', 'qword ptr [rsi], rax', 'config begin +0x0 reset'),
    0x35af13: ('48894608', 'mov', 'qword ptr [rsi + 8], rax', 'config end +0x8 reset'),
    0x35af17: ('48894610', 'mov', 'qword ptr [rsi + 0x10], rax', 'config allocation end +0x10 reset'),
    0xc6ff: ('cd29', 'int', '0x29', 'narrow invalid-allocation fail-fast'),
    0xc7f8: ('cd29', 'int', '0x29', 'pointer invalid-allocation fail-fast'),
    0x18247: ('cd29', 'int', '0x29', 'wide invalid-allocation fail-fast'),
    0x35af04: ('cd29', 'int', '0x29', 'config invalid-allocation fail-fast'),
    0x50b6c0: ('4885c9', 'test', 'rcx, rcx', 'null allocation test'),
    0x50b6c3: ('7436', 'je', '0x18050b6fb', 'null allocation returns without HeapFree'),
    0x50b6ca: ('4c8bc1', 'mov', 'r8, rcx', 'HeapFree allocation argument'),
    0x50b6cd: ('33d2', 'xor', 'edx, edx', 'HeapFree flags zero'),
    0x50b6cf: ('488b0d5ab71501', 'mov', 'rcx, qword ptr [rip + 0x115b75a]', 'heap handle from 0x1666e30'),
    0x50b6dc: ('85c0', 'test', 'eax, eax', 'HeapFree BOOL result test'),
    0x50b6de: ('7516', 'jne', '0x18050b6f6', 'nonzero HeapFree result skips failure helpers'),
    0x50b6e6: ('8bc8', 'mov', 'ecx, eax', 'GetLastError result passed to UNKNOWN helper'),
    0x50b6ed: ('8bd8', 'mov', 'ebx, eax', 'save first UNKNOWN helper result'),
    0x50b6f4: ('8918', 'mov', 'dword ptr [rax], ebx', 'write result through second UNKNOWN helper pointer'),
    0x50b6fb: ('c3', 'ret', '', 'allocation-free block return'),
}
RUNTIME_ANCHORS.update({rva: (mnemonic, hex(0x180000000+target))
                        for rva, target, mnemonic, _ in NR_CLEANUP_DIRECT_EDGES})
RUNTIME_ANCHORS.update({rva: ('jmp', hex(0x180000000+target)) for rva, target in NR_CLEANUP_THUNKS})
RUNTIME_ANCHORS.update({rva: ('call', f'qword ptr [rip + {iat-rva-6:#x}]')
                        for rva, iat, _ in NR_CLEANUP_IMPORT_CALLS})
RUNTIME_ANCHORS.update({rva: (mnemonic, operands)
                        for rva, (_, mnemonic, operands, _) in NR_CLEANUP_BINDINGS.items()})
NR_SHUTDOWN_CLEANUP_ARRAYS = {
    'nr_shutdown_cleanup_window_assertions': 'windows',
    'nr_shutdown_cleanup_edge_assertions': 'direct_edges',
    'nr_shutdown_cleanup_thunk_assertions': 'leaf_thunks',
    'nr_shutdown_cleanup_import_call_assertions': 'import_calls',
    'nr_shutdown_cleanup_instruction_assertions': 'instruction_bindings',
    'nr_shutdown_cleanup_unknown_helpers': 'unknown_failure_helpers',
}


def nr_shutdown_scope_contract(pe, base: int) -> dict:
    """Bind exact lookup/cleanup windows without asserting an absent dynamic call."""
    pe.parse_data_directories(directories=[1])
    imports = {entry.address-base: (module.dll.decode('ascii'), entry.name.decode('ascii'))
               for module in pe.DIRECTORY_ENTRY_IMPORT for entry in module.imports if entry.name}
    if imports.get(0x53b3f0) != ('KERNEL32.dll', 'FreeLibrary'):
        raise ValueError('NR shutdown-scope FreeLibrary import changed')
    for _, iat, name in NR_CLEANUP_IMPORT_CALLS:
        if imports.get(iat) != ('KERNEL32.dll', name):
            raise ValueError('NR cleanup import changed: '+name)
    records = _function_records(pe, {
        'payload_loader': (0x356f60, 0x35829a, 0xd3c130),
        'state_accessor': (0x35a450, 0x35a4cc, 0xd3c84c),
        'state_exit_cleanup_head': (0x539bc0, 0x539bea, 0xd03020),
        'state_exit_cleanup_middle': (0x539bea, 0x539c0a, 0xd3c870),
        'state_exit_cleanup_tail': (0x539c0a, 0x539c43, 0xd3c884),
        'context_capture_caller': (0x153260, 0x155b5e, 0xd19ac4),
        'context_evaluate_consumer': (0x1458f0, 0x145d10, 0xd19c90),
        'cleanup_narrow_buffer': (0xc6c0, 0xc725, 0xd03004),
        'cleanup_pointer_buffer': (0xc7b0, 0xc818, 0xd03004),
        'cleanup_wide_buffer': (0x18200, 0x1826f, 0xd03004),
        'cleanup_config_head': (0x35ae70, 0x35ae89, 0xd03480),
        'cleanup_config_middle': (0x35ae89, 0x35aee8, 0xd3c7e4),
        'cleanup_config_tail': (0x35aee8, 0x35af26, 0xd3c7f8),
        'cleanup_heap_free': (0x50b6c0, 0x50b6fc, 0xd59d34),
    }, 'NR shutdown-scope')
    def bind(rva: int, expected: bytes, label: str) -> dict:
        raw = pe.get_data(rva, len(expected))
        if raw != expected:
            raise ValueError('NR cleanup '+label+' bytes changed: '+hex(rva))
        return dict(rva=hex(rva), bytes=raw.hex())

    edges = []
    for rva, target, mnemonic, purpose in NR_CLEANUP_DIRECT_EDGES:
        raw = (b'\xe8' if mnemonic == 'call' else b'\xe9')+struct.pack('<i', target-rva-5)
        edges.append(dict(bind(rva, raw, 'direct edge'), target_rva=hex(target),
                          mnemonic=mnemonic, purpose=purpose))
    thunks = []
    for rva, target in NR_CLEANUP_THUNKS:
        row = bind(rva, b'\xe9'+struct.pack('<i', target-rva-5), 'leaf thunk')
        if any(entry.struct.BeginAddress < rva+5 and entry.struct.EndAddress > rva
               for entry in pe.DIRECTORY_ENTRY_EXCEPTION):
            raise ValueError('NR cleanup leaf thunk unexpectedly covered by .pdata: '+hex(rva))
        thunks.append(dict(row, end_rva=hex(rva+5), target_rva=hex(target),
                           mnemonic='jmp', pdata_coverage=False))
    calls = [dict(bind(rva, b'\xff\x15'+struct.pack('<i', iat-rva-6), 'import call'),
                  iat_rva=hex(iat), dll='KERNEL32.dll', name=name)
             for rva, iat, name in NR_CLEANUP_IMPORT_CALLS]
    bindings = [dict(bind(rva, bytes.fromhex(raw), 'instruction'), mnemonic=mnemonic,
                     operands=operands, observation=observation)
                for rva, (raw, mnemonic, operands, observation) in NR_CLEANUP_BINDINGS.items()]
    windows = []
    expected_windows = (
        ('nr-cleanup-narrow-buffer', 0xc6c0, 0xc725),
        ('nr-cleanup-pointer-buffer', 0xc7b0, 0xc818),
        ('nr-cleanup-wide-buffer', 0x18200, 0x1826f),
        ('nr-cleanup-config-records', 0x35ae70, 0x35af26),
        ('nr-cleanup-free-thunk-1', 0x4e76f0, 0x4e76f5),
        ('nr-cleanup-free-thunk-2', 0x4e80a0, 0x4e80a5),
        ('nr-cleanup-free-thunk-3', 0x4f7de0, 0x4f7de5),
        ('nr-cleanup-heap-free', 0x50b6c0, 0x50b6fc),
    )
    for name, lo, hi in expected_windows:
        if RUNTIME_RANGES.get(name) != (lo, hi):
            raise ValueError('NR cleanup decode window boundary changed: '+name)
        windows.append(dict(name=name, begin=hex(lo), end=hex(hi),
                            function_records=[row for row in records
                                              if lo <= int(row['begin'], 16) < hi]))
    cleanup = dict(windows=windows, direct_edges=edges, leaf_thunks=thunks,
        import_calls=calls, instruction_bindings=bindings,
        container_layouts=dict(
            narrow_buffer=dict(pointer_offset='0x0', length_offset='0x10', capacity_offset='0x18',
                               inline_capacity=15, unit_bytes=1),
            pointer_buffer=dict(begin_offset='0x0', end_offset='0x8', allocation_end_offset='0x10',
                                element_stride='0x8'),
            wide_buffer=dict(pointer_offset='0x0', length_offset='0x10', capacity_offset='0x18',
                             inline_capacity=7, unit_bytes=2),
            config_records=dict(begin_offset='0x0', end_offset='0x8', allocation_end_offset='0x10',
                                record_stride='0x48', inner_narrow_offset='0x18',
                                inner_argument_rva='0x35ae97', advance_rva='0x35aea0'),
            linked_nodes=dict(next_offset='0x0', allocation_size_argument='0x20')),
        normal_heap_path=dict(null_return_branch_rva='0x50b6c3', allocation_to_r8_rva='0x50b6ca',
            flags_zero_rva='0x50b6cd', heap_handle_slot_rva='0x1666e30', heap_handle_load_rva='0x50b6cf',
            heap_free_call_rva='0x50b6d6', result_test_rva='0x50b6dc', nonzero_branch_rva='0x50b6de',
            nonzero_target_rva='0x50b6f6', return_rva='0x50b6fb', runtime_success_proven=False),
        failure_path=dict(condition='HeapFree returned zero', get_last_error_call_rva='0x50b6e0',
                          result_store_rva='0x50b6f4', helpers_fully_recovered=False),
        unknown_failure_helpers=[dict(call_rva=hex(rva), target_rva=hex(target),
                                      status='UNKNOWN', body_decoded=False)
                                 for rva, target in ((0x50b6e8, 0x4f6c7c), (0x50b6ef, 0x4f6dbc))],
        invalid_allocation_fail_fast_rvas=['0xc6ff', '0xc7f8', '0x18247', '0x35af04'],
        scope='Only the decoded direct paths show CPU container memory cleanup; failure helper bodies remain UNKNOWN; no payload Shutdown, GPU retirement or successful-session unload authentication')
    return dict(function_records=records,
        imports=[dict(iat_rva=hex(iat), dll='KERNEL32.dll', name=name)
                 for iat, name in ((0x53b3f0, 'FreeLibrary'), (0x53b668, 'HeapFree'), (0x53b348, 'GetLastError'))],
        bounded_cleanup=cleanup,
        payload_state_rva='0x1663e60', api_table_rva='0x1663ed0', shutdown_slot_rva='0x1663ef0',
        shutdown_export_required=dict(check_rva='0x357640', missing_branch_rva='0x357645',
                                       missing_target_rva='0x358122', unload_call_rva='0x358125'),
        exit_cleanup=dict(registration_call_rva='0x35a4b1', entry_rva='0x539bc0', end_rva='0x539c43'),
        context_evaluate=dict(capture_rva='0x15502b', captured_table_read_rva='0x145c8b',
                              context_field_offset='0x20', api_call_rva='0x145caa', api_slot_offset='0x10'),
        target_executed=False, payload_executed=False, gpu_tested=False,
        shutdown_call_identified=False, complete_dynamic_call_coverage_proven=False,
        transitive_cleanup_authenticated=False, complete_session_lifetime_proven=False,
        successful_session_unload_proven=False, resource_retirement_proven=False,
        scope='Bounded required-export failure, exit-cleanup direct container paths and captured Evaluate call only; not proof that Shutdown has no dynamic callers')


def ngx_shutdown_interposition_contract(pe, base: int) -> dict:
    """Separate the captured NGX forwarding slot from the direct NR API table."""
    literals = []
    for rva, text, encoding in (
            (0x55b208, 'NVSDK_NGX_D3D12_Shutdown1', 'ascii'),
            (0x559c10, 'shutdown api=D3D12 device=%p result=0x%08X registryInvalidated=%u', 'utf-16-le'),
            (0x55b560, '[NR NGX target] index=%zu export=%S entry=%p core-rva=0x%llX', 'utf-16-le')):
        expected = (text+'\0').encode(encoding)
        raw = pe.get_data(rva, len(expected))
        if raw != expected:
            raise ValueError('NGX shutdown interposition literal changed: '+hex(rva))
        literals.append(dict(rva=hex(rva), text=text, encoding=encoding, bytes=raw.hex()))
    pe.parse_data_directories(directories=[1])
    imports = {entry.address-base: (module.dll.decode('ascii'), entry.name.decode('ascii'))
               for module in pe.DIRECTORY_ENTRY_IMPORT for entry in module.imports if entry.name}
    expected_imports = {0x53b3a8: ('KERNEL32.dll', 'GetProcAddress'),
                        0x53b560: ('KERNEL32.dll', 'AcquireSRWLockExclusive'),
                        0x53b550: ('KERNEL32.dll', 'ReleaseSRWLockExclusive')}
    if any(imports.get(rva) != expected for rva, expected in expected_imports.items()):
        raise ValueError('NGX shutdown interposition import changed')
    records = _function_records(pe, {
        'descriptor_and_resolver': (0x932f0, 0x939d7, 0xd0c564),
        'forwarding_wrapper': (0xffa00, 0xffa4f, 0xd0300c),
        'postprocess': (0xff8f0, 0xff9bd, 0xd0af84),
        'registration_consumer': (0x109dd0, 0x10b465, 0xd0c814),
    }, 'NGX shutdown interposition')
    # This pointer lies beyond .data's raw bytes. Do not turn get_data()==b''
    # into eight observed zero bytes or assume its runtime function identity.
    slot = 0x14795a8
    sections = [section for section in pe.sections if section.VirtualAddress <= slot <
                section.VirtualAddress+section.Misc_VirtualSize]
    if len(sections) != 1:
        raise ValueError('NGX shutdown pointer section changed')
    section = sections[0]
    layout = (section.Name, section.VirtualAddress, section.Misc_VirtualSize,
              section.SizeOfRawData, section.Characteristics)
    if layout != (b'.data\0\0\0', 0xd66000, 0x900e74, 0x22200, 0xc0000040):
        raise ValueError('NGX shutdown pointer section layout changed')
    if slot < section.VirtualAddress+section.SizeOfRawData:
        raise ValueError('NGX shutdown pointer unexpectedly file-backed')
    return dict(literals=literals, function_records=records,
        imports=[dict(iat_rva=hex(rva), dll=dll, name=name)
                 for rva, (dll, name) in expected_imports.items()],
        descriptor=dict(name_store_rva='0x9335d', pointer_slot_store_rva='0x93369',
                        wrapper_store_rva='0x93375', wrapper_rva='0xffa00'),
        registration_pair=dict(slot_pointer_load_rva='0x936d2', null_check_rva='0x936d7',
                               conditional_rejection_clear_rva='0x936e9',
                               wrapper_store_rva='0x93742', slot_pointer_store_rva='0x93746',
                               slot_pointer_member_offset='0x0', wrapper_member_offset='0x8', bytes=16),
        target_diagnostic=dict(resolver_call_rva='0x10afa8', pair_slot_pointer_load_rva='0x10afe6',
                               entry_value_dereference_rva='0x10afea',
                               entry_value_stack_store_rva='0x10aff8',
                               export_name_load_rva='0x10b002', format_call_rva='0x10b017',
                               following_callback_rva='0x10b023', is_installation_proof=False),
        transaction_caller=dict(empty_pair_list_branch_rva='0x10b04c',
                                helper_call_rva='0x10b072', helper_rva='0x196a0',
                                returned_field_read_rva='0x10b077', returned_field_offset='0x8',
                                zero_branch_rva='0x10b07e', zero_target_rva='0x10b1d7',
                                clear_target_rva='0x10b0c0', helper_abi_authenticated=False),
        clear_writer=dict(zero_register_setup_rva='0x10af75', value_register='R15',
                          slot_write_rva='0x10b0e6', proves_runtime_zero_value=False,
                          is_gpu_retirement=False),
        forwarding_slot=dict(rva=hex(slot), section='.data', section_rva=hex(section.VirtualAddress),
                             section_virtual_size=hex(section.Misc_VirtualSize),
                             section_raw_size=hex(section.SizeOfRawData),
                             file_backed=False, raw_pointer_bytes=None, runtime_value='unknown',
                             resolver_write_rva='0x93442', wrapper_read_rva='0xffa2a'),
        wrapper_flow=dict(saved_input_register='RCX', forwarded_call_rva='0xffa34',
                          postprocess_call_rva='0xffa3d', saved_result_register='EBX',
                          result_restore_rva='0xffa42'),
        postprocess=dict(enabled_flag_rva='0x1478838', result_compare_rva='0xff911',
                         result_literal=1, conditional_block_bytes='0xc00',
                         diagnostic_limit='0x400', diagnostic_call_rva='0xff9a7'),
        direct_nr_shutdown_slot_rva='0x1663ef0',
        target_executed=False, payload_executed=False, gpu_tested=False,
        original_pointer_runtime_target_authenticated=False, installed_trampoline_authenticated=False,
        nr_payload_shutdown_caller_proven=False, shutdown_abi_authenticated=False,
        transitive_postprocess_effects_authenticated=False, resource_retirement_proven=False,
        registry_invalidation_is_gpu_retirement=False,
        scope='Fixed NGX descriptor, slot-address pair, diagnostic dereference, caller branches and wrapper/postprocess; no installation, direct NR shutdown or safe-unload certificate')


def public_loader_flags(source: str) -> dict[str, int]:
    result = {}
    for name, expected in (('GET_MODULE_HANDLE_EX_FLAG_PIN', 1),
                           ('GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT', 2),
                           ('GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS', 4)):
        values = re.findall(r'^\s*#define\s+'+name+r'\s+\(?\s*(0x[0-9a-fA-F]+|[0-9]+)\s*\)?\s*$',
                            source, re.M)
        if len(values) != 1 or int(values[0], 0) != expected:
            raise ValueError('public module-handle flag changed: '+name)
        result[name] = expected
    return result


def ngx_hook_transaction_contract(pe, base: int, loader_header: str) -> dict:
    """Recover bounded CPU transaction control, not an ABI or live-patching recipe."""
    flags = public_loader_flags(loader_header)
    stages = []
    for rva, name in ((0x540488, 'refused-context'), (0x540508, 'begin'),
                       (0x540518, 'duplicate-target'), (0x540510, 'target'),
                       (0x540530, 'replacement-owner'), (0x540548, 'precondition'),
                       (0x540558, 'attach'), (0x540560, 'commit'),
                       (0x540498, 'snapshot'), (0x5404c8, 'enumerate-thread'),
                       (0x5404b8, 'open-thread'), (0x5404e0, 'update-thread'),
                       (0x5404f0, 'update-exited-thread')):
        raw = pe.get_data(rva, len(name)+1)
        if raw != (name+'\0').encode('ascii'):
            raise ValueError('NGX hook transaction stage changed: '+name)
        stages.append(dict(rva=hex(rva), name=name, bytes=raw.hex()))
    pe.parse_data_directories(directories=[1])
    imports = {entry.address-base: (module.dll.decode('ascii'), entry.name.decode('ascii'))
               for module in pe.DIRECTORY_ENTRY_IMPORT for entry in module.imports if entry.name}
    expected_imports = {0x53b3a0: ('KERNEL32.dll', 'GetModuleHandleExW'),
                        0x53b340: ('KERNEL32.dll', 'CloseHandle'),
                        0x53b348: ('KERNEL32.dll', 'GetLastError'),
                        0x53b3d0: ('KERNEL32.dll', 'GetCurrentThreadId'),
                        0x53b498: ('KERNEL32.dll', 'VirtualProtect'),
                        0x53b3f8: ('KERNEL32.dll', 'CreateToolhelp32Snapshot'),
                        0x53b400: ('KERNEL32.dll', 'Thread32First'),
                        0x53b408: ('KERNEL32.dll', 'Thread32Next'),
                        0x53b3d8: ('KERNEL32.dll', 'OpenThread'),
                        0x53b3c0: ('KERNEL32.dll', 'GetCurrentProcessId'),
                        0x53b3c8: ('KERNEL32.dll', 'GetCurrentThread'),
                        0x53b360: ('KERNEL32.dll', 'WaitForSingleObject')}
    if any(imports.get(rva) != expected for rva, expected in expected_imports.items()):
        raise ValueError('NGX hook transaction import changed')
    thunks = []
    for rva, iat in ((0x4de981, 0x53b3d0), (0x4de9a5, 0x53b498), (0x4de96f, 0x53b348),
                     (0x4de98d, 0x53b3f8), (0x4de993, 0x53b400), (0x4de999, 0x53b408)):
        raw = pe.get_data(rva, 6)
        if raw != b'\xff\x25'+struct.pack('<i', iat-rva-6):
            raise ValueError('NGX hook transaction thunk changed: '+hex(rva))
        thunks.append(dict(rva=hex(rva), iat_rva=hex(iat), bytes=raw.hex()))
    records = _function_records(pe, {
        'transaction_caller': (0x196a0, 0x19ac6, 0xd04ea0),
        'begin_helper': (0x425320, 0x425390, 0xd030c8),
        'protection_prepare': (0x424870, 0x4248c6, 0xd030c8),
        'abort_helper_owner_prefix': (0x4251d0, 0x425315, 0xd07d38),
        'commit_helper_owner_prefix': (0x425390, 0x425827, 0xd4a0f4),
        'target_identity_wrapper': (0x424f50, 0x424f72, 0xd4a124),
        'attach_wrapper': (0x4248d0, 0x424901, 0xd4a10c),
        'thread_admission': (0x1b700, 0x1b95d, 0xd04e54),
    }, 'NGX hook transaction')
    return dict(stages=stages, public_loader_flags=flags, function_records=records, thunks=thunks,
        imports=[dict(iat_rva=hex(rva), dll=dll, name=name)
                 for rva, (dll, name) in expected_imports.items()],
        result_fields_observed={'0x0': 'stage-name pointer', '0x8': 'raw32 error/result',
                                '0x10': 'item index', '0x18': 'thread id',
                                '0x1c': 'thread-exit observation byte', '0x20': 'failed pointer-slot address'},
        cpu_owner=dict(thread_id_slot_rva='0x1665150', pending_error_slot_rva='0x1665154',
                       acquire_cas_rva='0x42534a', abort_owner_compare_rva='0x4251d9',
                       commit_owner_compare_rva='0x4253d0', is_gpu_fence=False),
        begin_failure=dict(caller_call_rva='0x19749', nonzero_branch_rva='0x19753',
                           return_copy_target_rva='0x1998a', protection_call_rva='0x42537a',
                           protection_error_read_rva='0x4248b6', error_store_rva='0x42537f',
                           error_return_read_rva='0x425385',
                           may_follow_cpu_owner_acquisition=True,
                           direct_owner_clear_seen_after_acquisition_in_begin=False,
                           outer_nonzero_path_calls_abort=False,
                           no_state_mutation_or_clean_retry_guaranteed=False,
                           runtime_failure_reproduced=False),
        target_validation=dict(identity_helper_rva='0x424f50', duplicate_branch_rva='0x197cb',
                               duplicate_error='0xd', invalid_target_error='0x57',
                               identity_is_com_or_resource_authority=False),
        wrapper_module_pin_attempt=dict(call_rva='0x19831', flags=5,
            flag_names=['GET_MODULE_HANDLE_EX_FLAG_PIN', 'GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS'],
            target_member_offset='0x8', target='pair wrapper address, not original entry or selected NR payload',
            failure_target_rva='0x199a8', actual_module_pin_proven=False, nr_payload_module_identity_proven=False),
        later_steps=dict(thread_helper_call_rva='0x1986c', thread_helper_rva='0x1b700',
                         precondition_call_rva='0x19883', abort_call_rvas=['0x198a3', '0x19985'],
                         attach_call_rva='0x19a2b', commit_call_rva='0x19a6c',
                         failed_slot_compare_rva='0x19aa6', close_thread_handle_rva='0x198b8'),
        thread_admission=dict(snapshot_flags=4, entry_bytes='0x1c', remote_access_mask='0x10001a',
            same_process_filter_rva='0x1b7ae', skip_current_thread_compare_rva='0x1b7bf',
            open_call_rva='0x1b7cb', open_error_skip_value='0x57', open_error_compare_rva='0x1b8c1',
            record_bytes=16, record_handle_offset='0x0', record_thread_id_offset='0x8',
            snapshot_close_rva='0x1b850', update_helper_rva='0x425830',
            remote_update_call_rva='0x1b889', remote_error_store_rva='0x1b88e',
            failed_update_wait_rva='0x1b8f0', wait_timeout=0, exit_flag_store_rva='0x1b8fb',
            wait0_clears_update_error=False, exit_flag_is_gpu_completion=False,
            current_pseudo_handle_rva='0x1b927', current_update_call_rva='0x1b930',
            current_pseudo_handle_stored_in_vector=False, update_helper_fully_recovered=False),
        target_executed=False, payload_executed=False, gpu_tested=False,
        private_helper_abi_authenticated=False, backend_helpers_fully_recovered=False,
        complete_thread_coverage_proven=False, exception_cleanup_proven=False,
        runtime_installation_proven=False, complete_rollback_proven=False,
        resource_retirement_proven=False, safe_unload_proven=False,
        scope='Fixed-target normal CPU control flow, public Win32 flag values and partial backend owner gates; no runtime reproduction, universal Detours contract or GPU certificate')


def nr_creation_contract(pe) -> dict:
    keys = []
    for rva, expected in ((0x572318, 'Width'), (0x572320, 'Height'), (0x572328, 'OutWidth'),
            (0x572338, 'OutHeight'), (0x599b38, 'DLSSNR.Width'), (0x599b48, 'DLSSNR.Height'),
            (0x599b20, 'DLSSNR.ScalingRatio'), (0x599b58, 'DLSSNRComputeScalingRatioCallback'),
            (0x599b80, 'DLSSNR.Hint.Render.Preset'), (0x5723e8, 'PerfQualityValue'),
            (0x55abe0, 'DLSS.Feature.Create.Flags')):
        raw = pe.get_data(rva, len(expected)+1)
        if raw != expected.encode('ascii')+bytes(1):
            raise ValueError('Encore NR creation key changed')
        keys.append(dict(rva=hex(rva), bytes=raw.hex(), key=expected))
    literals = []
    for rva, fmt, expected in ((0x53fba0, '<d', 1.0), (0x53fb94, '<f', 1.0)):
        raw = pe.get_data(rva, struct.calcsize(fmt))
        if struct.unpack(fmt, raw)[0] != expected:
            raise ValueError('Encore NR creation ratio constant changed')
        literals.append(dict(rva=hex(rva), bytes=raw.hex(), format=fmt, value=expected))
    return dict(keys=keys, literals=literals, local_callback_rva='0x15a240',
        common_parameter_initializer_rva='0x15bdf0', create_flag_mask='0xffffffbe',
        feature_identity_fields=['runtime+0x170', 'runtime+0x174', 'runtime+0x178'],
        target_executed=False, callback_executed=False, gpu_tested=False,
        complete_reuse_admission_proven=False, cross_payload_abi_authenticated=False)

def fg_resource_tag_contract(pe, base: int, headers: dict[str, str]) -> dict:
    """Check the bounded FFX-to-SL tag bridge against explicit public headers."""
    guids = {
        0x53c870: ('sl::Resource', 'cf709d3a1824724b839113f8721c7261'),
        0x53c880: ('sl::ResourceTag', 'ad5a6a4c45b46c4987ff1af3845be653'),
        0x53ce20: ('ID3D12Device', 'f1199818b61d574bbe541821339b85f7'),
        0x53ce30: ('ID3D12GraphicsCommandList', '0f0d165b1bac85418ba8b3ae42a5a455'),
    }
    literals = []
    for rva, (name, expected) in guids.items():
        raw = pe.get_data(rva, 16)
        if raw.hex() != expected:
            raise ValueError('FG resource GUID mismatch: '+name)
        literals.append(dict(rva=hex(rva), name=name, bytes=raw.hex()))
    export = 'slSetTagForFrame'
    raw = pe.get_data(0x53d0f8, len(export)+1)
    if raw != export.encode('ascii')+bytes(1):
        raise ValueError('FG tag export mismatch')
    slots = public_vtable_slots(headers['d3d12.h'], 'ID3D12Resource',
                                {'GetDevice': 0x38, 'GetDesc': 0x50})
    types = headers['sl_core_types.h']
    tags = {'kBufferTypeDepth': 0, 'kBufferTypeMotionVectors': 1, 'kBufferTypeHUDLessColor': 2}
    for name, value in tags.items():
        if not re.search(r'constexpr\s+BufferType\s+'+name+r'\s*=\s*'+str(value)+r'\s*;', types):
            raise ValueError('SL tag value mismatch: '+name)
    match = re.search(r'enum\s+ResourceLifecycle\s*\{(.*?)\}', types, re.S)
    body = re.sub(r'//[^\n]*', '', match[1]) if match else ''
    lifecycle = [word.strip() for word in body.split(',') if word.strip()]
    if lifecycle != ['eOnlyValidNow', 'eValidUntilPresent', 'eValidUntilEvaluate']:
        raise ValueError('SL resource lifecycle order changed')
    descriptors = {'FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION': 0x20002,
                   'FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE': 0x20004}
    for name, value in descriptors.items():
        if not re.search(r'#define\s+'+name+r'\s+0x0*'+format(value, 'x')+r'u\b',
                         headers['ffx_framegeneration.h'], re.I):
            raise ValueError('FFX descriptor type mismatch: '+name)
    return dict(
        guids=literals, export=dict(rva='0x53d0f8', name=export, bytes=raw.hex()),
        tag_pointer_rva='0xda1088', resource_slots=slots, tags=tags,
        lifecycle={name: index for index, name in enumerate(lifecycle)},
        descriptors={name: hex(value) for name, value in descriptors.items()},
        observed_descriptor_sizes={'configure': '0x90', 'prepare': '0xb8'},
        exact_key_slot=dict(count=8, stride='0x2b0', helper_rva='0xffa0',
                            compared_key_offset='0x348', matching_return_rva='0x10364'),
        module_base=hex(base), target_executed=False, gpu_tested=False,
        source_consumption_proven=False, resource_retirement_proven=False,
        complete_frame_slot_lifecycle_proven=False, public_abi_compiled=False)


def shader_tokens(blob: bytes, dump: str, base: int) -> list[dict]:
    if blob[:4] != b'DXBC' or struct.unpack_from('<I', blob, 24)[0] != len(blob):
        raise ValueError('invalid DXBC container')
    count = struct.unpack_from('<I', blob, 28)[0]
    chunks = [struct.unpack_from('<I', blob, 32+4*i)[0] for i in range(count)]
    code = [c for c in chunks if blob[c:c+4] in (b'SHDR', b'SHEX')]
    if len(code) != 1:
        raise ValueError('expected one code chunk')
    c = code[0]
    length = struct.unpack_from('<I', blob, c+4)[0]
    rows, expected = [], 0
    for m in re.finditer(r'^ *(?:[0-9]+ +)?0x([0-9a-fA-F]+): *(.+)$', dump, re.M):
        offset = int(m[1], 16)
        token = struct.unpack_from('<I', blob, c+8+offset)[0]
        words = 2 if offset == 0 else (token >> 24) & 127
        if offset and token & 0x7ff == 53:
            words = struct.unpack_from('<I', blob, c+12+offset)[0]
        end = offset+4*words
        if offset != expected or not words or end > length:
            raise ValueError(f'invalid token at {offset:#x}')
        rows.append(dict(rva=hex(base+c+8+offset), offset=hex(offset), asm=m[2].strip(),
                         bytes=blob[c+8+offset:c+8+end].hex()))
        expected = end
    if expected != length:
        raise ValueError(f'incomplete token decode {expected}/{length}')
    return rows

def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--reference-root', type=Path, required=True, help='F7 build/research')
    p.add_argument('--fxc', type=Path)
    p.add_argument('--windows-sdk-include', type=Path, help='Explicit Windows SDK um header directory for runtime profile')
    p.add_argument('--profile', choices=('camera', 'runtime', 'nr-codec', 'nr-warp'), default='camera')
    p.add_argument('--output', type=Path, default=Path(__file__).parent/'evidence')
    args = p.parse_args()
    if args.profile != 'runtime' and args.fxc is None:
        p.error('--fxc is required for shader profiles')
    if args.profile == 'runtime' and args.windows_sdk_include is None:
        p.error('--windows-sdk-include is required for runtime completion API evidence')
    ranges = {'camera': RANGES, 'runtime': RUNTIME_RANGES, 'nr-codec': CODEC_RANGES,
              'nr-warp': WARP_RANGES}[args.profile]
    shaders = {'camera': SHADERS, 'runtime': (), 'nr-codec': tuple(CODEC_SHADERS),
               'nr-warp': tuple(WARP_SHADERS)}[args.profile]
    first = args.reference_root/'rtx-encore-v2.0.1'
    second = args.reference_root/'rtx-encore-generic-astra'
    target = first/'extracted/rtx-encore.dll'
    actual = sha(target)
    if actual != SHA:
        raise ValueError(f'target identity mismatch: {actual}')
    sys.path.insert(0, str(first/'python-deps'))
    import pefile
    import capstone
    pe = pefile.PE(str(target), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    dis = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    dis.detail = True
    out = args.output.resolve()
    if args.profile != 'camera':
        out = out/args.profile
    sdk = args.reference_root.parent.parent/'external/streamline'
    sdk_paths = [sdk/name for name in ('sl_struct.h', 'sl_dlss_g.h', 'sl_consts.h')]
    completion, completion_header_hashes = None, {}
    hook_transaction = None
    if args.profile == 'runtime':
        from verify import protect_inputs
        sdk_paths.append(args.reference_root.parent.parent/'external/nvngx_dlss_sdk/nvsdk_ngx_params.h')
        sdk_paths.append(args.windows_sdk_include/'libloaderapi.h')
        fg_header_paths = {
            'sl_core_types.h': sdk/'sl_core_types.h',
            'sl_core_api.h': sdk/'sl_core_api.h',
            'ffx_framegeneration.h': args.reference_root.parent.parent/'external/FidelityFX-SDK/ffx-api/include/ffx_api/ffx_framegeneration.h',
            'ffx_api_types.h': args.reference_root.parent.parent/'external/FidelityFX-SDK/ffx-api/include/ffx_api/ffx_api_types.h',
            'd3d12.h': args.windows_sdk_include/'d3d12.h',
        }
        sdk_paths.extend(fg_header_paths.values())
        sdk_paths.extend(args.windows_sdk_include/file for file in COMPLETION_API_SLOTS)
        protect_inputs([target, Path(__file__), Path(__file__).with_name('verify.py'),
                        Path(pefile.__file__), Path(capstone.__file__), Path(capstone._cs._name),
                        *sdk_paths], out)
        completion, completion_header_hashes = completion_contract(pe, base, args.windows_sdk_include)
        hook_transaction = ngx_hook_transaction_contract(pe, base,
            (args.windows_sdk_include/'libloaderapi.h').read_text(encoding='utf-8-sig'))
    out.mkdir(parents=True, exist_ok=True)
    manifest = dict(schema=1, profile=args.profile, target=str(target.resolve()), sha256=actual, image_base=hex(base),
                    method='bounded fresh static decode', target_executed=False,
                    camera_authority=False, code=[], shaders=[],
                    tools=dict(python=sys.version.split()[0], pefile=pefile.__version__,
                               capstone=capstone.__version__, fxc=str(args.fxc.resolve()) if args.fxc else None))
    all_rows = {}
    for name, (lo, hi) in ranges.items():
        raw = pe.get_data(lo, hi-lo)
        insns = list(dis.disasm(raw, base+lo))
        if b''.join(bytes(i.bytes) for i in insns) != raw:
            raise ValueError(f'incomplete native decode: {name}')
        rows = []
        for i in insns:
            refs = [hex(i.address-base+i.size+o.mem.disp) for o in i.operands
                    if o.type == capstone.x86.X86_OP_MEM and o.mem.base == capstone.x86.X86_REG_RIP]
            rows.append(dict(rva=hex(i.address-base), bytes=bytes(i.bytes).hex(),
                             mnemonic=i.mnemonic, operands=i.op_str, data_refs=refs))
            all_rows[i.address-base] = rows[-1]
        (out/(name+'.json')).write_text(json.dumps(rows, indent=2), encoding='utf-8')
        manifest['code'].append(dict(name=name, lo=hex(lo), hi=hex(hi), instructions=len(rows),
                                     sha256=hashlib.sha256(raw).hexdigest()))
    assertions = {'runtime': RUNTIME_ANCHORS, 'nr-warp': WARP_ANCHORS}.get(args.profile)
    anchors_name = 'runtime-anchors.json' if args.profile == 'runtime' else 'warp-anchors.json'
    if assertions:
        anchors = []
        for rva, expected in assertions.items():
            row = all_rows.get(rva)
            if row is None or (row['mnemonic'], row['operands']) != expected:
                raise ValueError(f'instruction anchor mismatch at {rva:#x}: {row}')
            anchors.append(row)
        (out/anchors_name).write_text(json.dumps(anchors, indent=2), encoding='utf-8')
        manifest['instruction_assertions'] = len(anchors)
    constants = []
    for rva in range(0x53fb70, 0x53fbc0, 4):
        raw = pe.get_data(rva, 4)
        constants.append(dict(rva=hex(rva), bytes=raw.hex(), value=struct.unpack('<f', raw)[0]))
    (out/'camera-literals.json').write_text(json.dumps(constants, indent=2), encoding='utf-8')
    old = []
    if args.profile == 'camera':
        old = json.loads((second/'shader-manifest.json').read_text())
        old += json.loads((second/'encore-shader-manifest.json').read_text())
    for name in shaders:
        records = {'nr-codec': CODEC_SHADERS, 'nr-warp': WARP_SHADERS}.get(args.profile)
        record = records[name] if records else next(m for m in old if m['name'] == name)
        rva = int(record['rva'], 16)
        raw = pe.get_data(rva, record['size'])
        if args.profile == 'nr-codec':
            pointer, size = struct.unpack('<QQ', pe.get_data(int(record['table_rva'], 16), 16))
            if (pointer, size) != (base+rva, record['size']):
                raise ValueError('codec bytecode table mismatch: '+name)
        elif args.profile == 'nr-warp':
            if hex(rva) not in all_rows[int(record['code_ref'], 16)]['data_refs']:
                raise ValueError('guide bytecode native reference mismatch')
            if record['size'] != 0x4f48:
                raise ValueError('guide bytecode immediate size mismatch')
        elif hashlib.sha256(raw).hexdigest() != record['sha256']:
            raise ValueError(f'prior shader mismatch: {name}')
        with tempfile.TemporaryDirectory(prefix='encore-static-', dir=out) as tmp:
            sample = Path(tmp)/'sample.dxbc'
            sample.write_bytes(raw)
            r = subprocess.run([str(args.fxc), '/nologo', '/dumpbin', '/Ni', '/No', str(sample)],
                               capture_output=True, check=True)
            dump = r.stdout.decode('utf-8', errors='strict').replace(chr(13)+chr(10), chr(10))
        tokens = shader_tokens(raw, dump, rva)
        (out/(name+'.asm.txt')).write_text(dump, encoding='utf-8')
        (out/(name+'.tokens.json')).write_text(json.dumps(tokens, indent=2), encoding='utf-8')
        manifest['shaders'].append(dict(name=name, rva=hex(rva), size=len(raw),
                                        table_rva=record.get('table_rva'), code_ref=record.get('code_ref'),
                                        size_ref=record.get('size_ref'),
                                        sha256=hashlib.sha256(raw).hexdigest(), tokens=len(tokens)))
    names = [name+'.json' for name in ranges] + ['camera-literals.json']
    if args.profile == 'runtime':
        keys = []
        for rva, expected in SR_PARAMETER_KEYS.items():
            raw = pe.get_data(rva, len(expected)+1)
            if raw != expected.encode('ascii')+bytes(1):
                raise ValueError('SR parameter key mismatch: '+expected)
            keys.append(dict(rva=hex(rva), bytes=raw.hex(), key=expected))
        raw = pe.get_data(0x53fba0, 8)
        if struct.unpack('<d', raw)[0] != 1.0:
            raise ValueError('SR unity double mismatch')
        data = dict(keys=keys, literals=[dict(rva='0x53fba0', bytes=raw.hex(), format='double', value=1.0)],
                    target_executed=False, runtime_values_authenticated=False)
        (out/'sr-generic-parameters.json').write_text(json.dumps(data, indent=2), encoding='utf-8')
        names.append('sr-generic-parameters.json')
        manifest['parameter_key_assertions'] = len(keys)
        manifest['literal_assertions'] = 1
        exports, rows, slots = [], [], []
        for rva, expected in NR_PAYLOAD_EXPORT_NAMES.items():
            raw = pe.get_data(rva, len(expected)+1)
            if raw != expected.encode('ascii')+bytes(1):
                raise ValueError('NR direct export name mismatch: '+expected)
            exports.append(dict(rva=hex(rva), bytes=raw.hex(), name=expected))
        for table, digest_rva, digest, version_rva, version in NR_PAYLOAD_IDENTITY_ROWS:
            prefix = pe.get_data(table, 16)
            if struct.unpack('<QQ', prefix) != (base+digest_rva, base+version_rva):
                raise ValueError('NR identity table pointer mismatch')
            for rva, expected in ((digest_rva, digest), (version_rva, version)):
                if pe.get_data(rva, len(expected)+1) != expected.encode('ascii')+bytes(1):
                    raise ValueError('NR identity table string mismatch')
            rows.append(dict(table_rva=hex(table), prefix_bytes=prefix.hex(), sha256_text_rva=hex(digest_rva),
                             sha256_text=digest, version_text_rva=hex(version_rva), version_text=version))
        for index, expected in enumerate(NR_PARAMETER_VTABLE):
            address = 0x599750+index*8
            raw = pe.get_data(address, 8)
            if struct.unpack('<Q', raw)[0] != base+expected:
                raise ValueError('NR parameter vtable mismatch: '+str(index))
            slots.append(dict(slot=hex(index*8), table_rva=hex(address), bytes=raw.hex(), function_rva=hex(expected)))
        col = pe.get_data(0xcf8248, 24)
        if struct.unpack('<6I', col) != (1, 0, 0, 0xd81188, 0xcf8270, 0xcf8248):
            raise ValueError('NR parameter RTTI locator mismatch')
        locator_pointer = pe.get_data(0x599748, 8)
        if struct.unpack('<Q', locator_pointer)[0] != base+0xcf8248:
            raise ValueError('NR parameter vtable locator pointer mismatch')
        rtti_name = '.?AVParameters@nr_runtime@@'
        name_bytes = pe.get_data(0xd81198, len(rtti_name)+1)
        if name_bytes != rtti_name.encode('ascii')+bytes(1):
            raise ValueError('NR parameter RTTI name mismatch')
        owner_slots = []
        for index, expected in enumerate(NR_FEATURE_OWNER_VTABLE_PREFIX):
            address = 0x59c2f8+index*8
            raw = pe.get_data(address, 8)
            if struct.unpack('<Q', raw)[0] != base+expected:
                raise ValueError('NR feature owner vtable mismatch: '+str(index))
            owner_slots.append(dict(slot=hex(index*8), table_rva=hex(address),
                                    bytes=raw.hex(), function_rva=hex(expected)))
        data = dict(payload_export_names=exports, payload_identity_rows=rows, parameter_vtable=slots,
                    feature_owner_vtable_prefix=owner_slots,
                    parameter_rtti=dict(locator_rva='0xcf8248', locator_bytes=col.hex(),
                        locator_pointer_rva='0x599748', locator_pointer_bytes=locator_pointer.hex(),
                        type_descriptor_rva='0xd81188', name_rva='0xd81198',
                        name_bytes=name_bytes.hex(), name=rtti_name),
                    target_executed=False, payload_executed=False, payload_abi_authenticated=False,
                    identity_rows_are_historical_labels=True)
        (out/'nr-payload-api-tables.json').write_text(json.dumps(data, indent=2), encoding='utf-8')
        names.append('nr-payload-api-tables.json')
        input_fence = fg_input_fence_contract(pe, base,
            fg_header_paths['d3d12.h'].read_text(encoding='utf-8-sig'))
        (out/'fg-input-fence-contract.json').write_text(json.dumps(input_fence, indent=2), encoding='utf-8')
        names.append('fg-input-fence-contract.json')
        for field, array in (('instruction', 'instructions'), ('import', 'imports'),
                             ('function', 'function_records'), ('api_slot', 'public_fence_slots'),
                             ('path', 'paths')):
            manifest['fg_input_fence_'+field+'_assertions'] = len(input_fence[array])
        device_holder = nr_device_holder_contract(pe, fg_header_paths['d3d12.h'].read_text(encoding='utf-8-sig'))
        (out/'nr-device-holder-contract.json').write_text(json.dumps(device_holder, indent=2), encoding='utf-8')
        names.append('nr-device-holder-contract.json')
        manifest['nr_device_holder_guid_assertions'] = 1
        manifest['nr_device_holder_api_slot_assertions'] = len(device_holder['public_device_child_slots'])
        manifest['nr_device_holder_function_assertions'] = len(device_holder['function_records'])
        shutdown_scope = nr_shutdown_scope_contract(pe, base)
        (out/'nr-shutdown-scope-contract.json').write_text(json.dumps(shutdown_scope, indent=2), encoding='utf-8')
        names.append('nr-shutdown-scope-contract.json')
        manifest['nr_shutdown_scope_import_assertions'] = len(shutdown_scope['imports'])
        manifest['nr_shutdown_scope_function_assertions'] = len(shutdown_scope['function_records'])
        for field, array in NR_SHUTDOWN_CLEANUP_ARRAYS.items():
            manifest[field] = len(shutdown_scope['bounded_cleanup'][array])
        interposition = ngx_shutdown_interposition_contract(pe, base)
        (out/'ngx-shutdown-interposition-contract.json').write_text(json.dumps(interposition, indent=2), encoding='utf-8')
        names.append('ngx-shutdown-interposition-contract.json')
        manifest['ngx_shutdown_interposition_literal_assertions'] = len(interposition['literals'])
        manifest['ngx_shutdown_interposition_import_assertions'] = len(interposition['imports'])
        manifest['ngx_shutdown_interposition_function_assertions'] = len(interposition['function_records'])
        manifest['ngx_shutdown_interposition_section_assertions'] = 1
        (out/'ngx-hook-transaction-contract.json').write_text(json.dumps(hook_transaction, indent=2), encoding='utf-8')
        names.append('ngx-hook-transaction-contract.json')
        manifest['ngx_hook_transaction_stage_assertions'] = len(hook_transaction['stages'])
        manifest['ngx_hook_transaction_import_assertions'] = len(hook_transaction['imports'])
        manifest['ngx_hook_transaction_thunk_assertions'] = len(hook_transaction['thunks'])
        manifest['ngx_hook_transaction_function_assertions'] = len(hook_transaction['function_records'])
        manifest['ngx_hook_transaction_flag_assertions'] = len(hook_transaction['public_loader_flags'])
        manifest['payload_export_name_assertions'] = len(exports)
        manifest['payload_identity_row_assertions'] = len(rows)
        manifest['parameter_vtable_assertions'] = len(slots)
        manifest['parameter_rtti_assertions'] = 3
        manifest['feature_owner_vtable_assertions'] = len(owner_slots)
        (out/'dx11-completion-contract.json').write_text(json.dumps(completion, indent=2), encoding='utf-8')
        names.append('dx11-completion-contract.json')
        manifest['completion_vtable_assertions'] = len(completion['vtable_prefix'])
        manifest['completion_rtti_assertions'] = 3
        manifest['completion_import_assertions'] = len(completion['imports'])
        manifest['completion_api_slot_assertions'] = sum(len(methods) for interfaces in
            COMPLETION_API_SLOTS.values() for methods in interfaces.values())
        manifest['completion_query_flag_assertions'] = 1
        loader = nr_import_contract(pe, base)
        (out/'nr-payload-import-contract.json').write_text(json.dumps(loader, indent=2), encoding='utf-8')
        names.append('nr-payload-import-contract.json')
        manifest['payload_import_name_assertions'] = len(loader['import_names'])
        manifest['payload_import_write_api_assertions'] = len(loader['imports'])
        creation = nr_creation_contract(pe)
        (out/'nr-creation-contract.json').write_text(json.dumps(creation, indent=2), encoding='utf-8')
        names.append('nr-creation-contract.json')
        manifest['nr_creation_key_assertions'] = len(creation['keys'])
        manifest['nr_creation_literal_assertions'] = len(creation['literals'])
        fg_tags = fg_resource_tag_contract(pe, base,
            {name: path.read_text(encoding='utf-8-sig') for name, path in fg_header_paths.items()})
        (out/'fg-resource-tag-contract.json').write_text(json.dumps(fg_tags, indent=2), encoding='utf-8')
        names.append('fg-resource-tag-contract.json')
        manifest['fg_resource_guid_assertions'] = len(fg_tags['guids'])
        manifest['fg_resource_export_assertions'] = 1
        manifest['fg_resource_api_slot_assertions'] = len(fg_tags['resource_slots'])
        manifest['fg_resource_enum_assertions'] = len(fg_tags['tags'])+len(fg_tags['lifecycle'])+len(fg_tags['descriptors'])
    if assertions:
        names.append(anchors_name)
    names += [name+suffix for name in shaders for suffix in ('.asm.txt', '.tokens.json')]
    if args.profile == 'nr-codec':
        manifest['bytecode_table_assertions'] = len(CODEC_SHADERS)
    if args.profile == 'nr-warp':
        manifest['bytecode_native_binding_assertions'] = len(WARP_SHADERS)
        literals = []
        for rva, (name, fmt, expected) in WARP_LITERALS.items():
            raw = pe.get_data(rva, struct.calcsize(fmt))
            value = struct.unpack(fmt, raw)[0]
            if value != expected:
                raise ValueError('warp CPU literal mismatch: '+name)
            literals.append(dict(name=name, rva=hex(rva), bytes=raw.hex(), value=value, format=fmt))
        (out/'native-warp-literals.json').write_text(json.dumps(literals, indent=2), encoding='utf-8')
        names.append('native-warp-literals.json')
        manifest['literal_assertions'] = len(literals)
        presets = []
        expected_presets = (('light', (65, 65, 10, 65)),
                            ('balanced', (55, 55, 8, 50)),
                            ('strong', (40, 40, 8, 35)))
        for index, (name, expected) in enumerate(expected_presets):
            rva = 0x55a020+index*24
            raw = pe.get_data(rva, 24)
            pointer, *values = struct.unpack('<Q4I', raw)
            observed_name = pe.get_data(pointer-base, len(name)+1)
            if tuple(values) != expected or observed_name != name.encode('ascii')+bytes(1):
                raise ValueError('native warp preset mismatch: '+name)
            presets.append(dict(index=index, rva=hex(rva), bytes=raw.hex(), name=name,
                                name_rva=hex(pointer-base), values=values))
        (out/'native-warp-presets.json').write_text(json.dumps(presets, indent=2), encoding='utf-8')
        names.append('native-warp-presets.json')
        manifest['preset_table_assertions'] = len(presets)
    manifest['sdk_reference_hashes'] = {str(path.resolve()): sha(path) for path in sdk_paths}
    if args.profile == 'runtime':
        manifest['sdk_reference_hashes'].update(completion_header_hashes)
    manifest['files'] = {name: sha(out/name) for name in sorted(names)}
    (out/'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(json.dumps(dict(status='PASS', native_instructions=sum(x['instructions'] for x in manifest['code']),
                          shader_tokens=sum(x['tokens'] for x in manifest['shaders']), target_sha256=actual)))

if __name__ == '__main__':
    main()
