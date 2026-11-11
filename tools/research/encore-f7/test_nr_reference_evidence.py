from contextlib import redirect_stderr
import copy
import io
import json
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import nr_reference_evidence as evidence


class WindowTests(unittest.TestCase):
    def decode(self, raw, instructions, size=2):
        pe = SimpleNamespace(get_data=lambda lo, length: raw)
        decoder = SimpleNamespace(disasm=lambda data, address: instructions)
        return evidence.decode_window(pe, decoder, 0x180000000, 0x20, 0x20+size)

    def instruction(self, address, raw):
        return SimpleNamespace(address=address, bytes=raw, mnemonic='nop', op_str='')

    def test_complete_contiguous_decode(self):
        raw = bytes((0x90, 0x90))
        rows = [self.instruction(0x180000020, raw[:1]), self.instruction(0x180000021, raw[1:])]
        result = self.decode(raw, rows)
        self.assertEqual(result['instructions'], 2)
        self.assertEqual(result['rows'][1]['rva'], '0x21')

    def test_short_file_span_is_refused(self):
        with self.assertRaisesRegex(ValueError, 'file span'):
            self.decode(bytes((0x90,)), [])

    def test_incomplete_decoder_output_is_refused(self):
        with self.assertRaisesRegex(ValueError, 'instruction decode'):
            self.decode(bytes((0x90, 0x90)), [self.instruction(0x180000020, bytes((0x90,)))])

    def test_address_gap_is_refused_even_when_bytes_match(self):
        raw = bytes((0x90, 0x90))
        with self.assertRaisesRegex(ValueError, 'non-contiguous'):
            self.decode(raw, [self.instruction(0x180000021, raw)])

    def test_empty_instruction_is_refused(self):
        raw = bytes((0x90, 0x90))
        with self.assertRaisesRegex(ValueError, 'non-contiguous'):
            self.decode(raw, [self.instruction(0x180000020, b''), self.instruction(0x180000020, raw)])

    def test_matching_anchor_is_preserved(self):
        row = dict(rva='0x20', bytes='90', mnemonic='nop', operands='')
        with patch.object(evidence, 'ANCHORS', {0x20: ('nop', '')}):
            self.assertEqual(evidence.check_anchors({0x20: row}), [row])

    def test_missing_anchor_is_refused(self):
        with patch.object(evidence, 'ANCHORS', {0x20: ('nop', '')}), self.assertRaises(ValueError):
            evidence.check_anchors({})

    def test_changed_anchor_is_refused(self):
        row = dict(rva='0x20', bytes='90', mnemonic='ret', operands='')
        with patch.object(evidence, 'ANCHORS', {0x20: ('nop', '')}), self.assertRaises(ValueError):
            evidence.check_anchors({0x20: row})


class InputProtectionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='nr-reference-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.payload = self.root/'payload.dll'
        self.payload.write_bytes(b'not a payload')
        self.pefile = self.root/'pefile.py'
        self.pefile.write_bytes(b'# unused dependency')
        self.capstone = self.root/'deps/capstone'
        self.capstone.mkdir(parents=True)
        (self.capstone/'__init__.py').write_bytes(b'# unused Capstone')
        self.library = self.capstone/'capstone.dll'
        self.library.write_bytes(b'not a library')
        self.output = self.root/'output/report.json'

    def arguments(self):
        return ['--input', str(self.payload), '--pefile-file', str(self.pefile),
                '--capstone-root', str(self.capstone.parent), '--output', str(self.output)]

    def rejected_before_build(self):
        before = {str(p): p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        with (patch.object(evidence, 'build_report') as build,
              redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error):
            evidence.main(self.arguments())
        self.assertEqual(error.exception.code, 2)
        build.assert_not_called()
        self.assertEqual(before, {str(p): p.read_bytes() for p in self.root.rglob('*') if p.is_file()})

    def test_payload_hardlink_output_is_preserved(self):
        self.output.parent.mkdir()
        os.link(self.payload, self.output)
        self.rejected_before_build()

    def test_pefile_hardlink_output_is_preserved(self):
        self.output.parent.mkdir()
        os.link(self.pefile, self.output)
        self.rejected_before_build()

    def test_capstone_library_hardlink_output_is_preserved(self):
        self.output.parent.mkdir()
        os.link(self.library, self.output)
        self.rejected_before_build()

    def test_payload_inside_output_tree_is_preserved(self):
        self.output.parent.mkdir()
        self.payload = self.output.parent/'payload.dll'
        self.payload.write_bytes(b'original bytes')
        self.rejected_before_build()

    def test_wrong_hash_does_not_import_dependencies_or_write(self):
        with (patch.object(evidence, 'load_pefile_file') as load,
              patch.object(evidence.importlib, 'import_module') as import_module,
              redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error):
            evidence.main(self.arguments())
        self.assertEqual(error.exception.code, 2)
        load.assert_not_called()
        import_module.assert_not_called()
        self.assertFalse(self.output.parent.exists())
        self.assertEqual(self.payload.read_bytes(), b'not a payload')


class SavedReportTests(unittest.TestCase):
    """Exercise report verification with a deterministic two-instruction decoder."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='nr-saved-report-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.payload = self.root/'fixture.bin'
        self.raw = bytes.fromhex('48895c241090')
        self.payload.write_bytes(self.raw)
        self.pefile = self.root/'pefile.py'
        self.pefile.write_bytes(b'# deterministic test fixture')
        self.report = self.root/'report.json'
        self.base = 0x180000000
        self.literal = 'nvngx.dll'.encode('utf-16-le')+bytes(2)
        self.pe = SimpleNamespace(
            OPTIONAL_HEADER=SimpleNamespace(ImageBase=self.base),
            DIRECTORY_ENTRY_EXCEPTION=[SimpleNamespace(
                struct=SimpleNamespace(BeginAddress=0x20, EndAddress=0x26))],
            DIRECTORY_ENTRY_IMPORT=[SimpleNamespace(dll=b'KERNEL32.dll', imports=[
                SimpleNamespace(address=self.base+0xd7118, name=b'GetModuleHandleExA'),
                SimpleNamespace(address=self.base+0xd7080, name=b'GetModuleFileNameW')])],
            parse_data_directories=lambda **kwargs: None,
            get_data=lambda lo, length: self.literal if lo == 0xd9380 else self.raw)
        instructions = [SimpleNamespace(address=self.base+0x20, bytes=self.raw[:5],
                                        mnemonic='mov', op_str='qword ptr [rsp + 0x10], rbx'),
                        SimpleNamespace(address=self.base+0x25, bytes=self.raw[5:],
                                        mnemonic='nop', op_str='')]
        self.decoder = SimpleNamespace(disasm=lambda data, address: instructions)
        self.tools = dict(capstone_root=str(self.root), capstone_native=str(self.pefile),
                          files={str(self.pefile): evidence.digest(self.pefile)})
        self.identity = dict(path=str(self.payload), bytes=len(self.raw), sha256=evidence.digest(self.payload))
        scope = dict(target_executed=False, gpu_tested=False, abi_authenticated=False, support='UNKNOWN')
        inspection = dict(input=self.identity, analyzer=dict(pefile_path=str(self.pefile),
                          pefile_sha256=evidence.digest(self.pefile)), support_proven=False, **scope)
        code = evidence.decode_window(self.pe, self.decoder, self.base, 0x20, 0x26)
        self.document = dict(schema=1, profile='historical-nr-entrypoints', input=self.identity,
            export_inspection=inspection, image_base=hex(self.base), code=dict(fixture=code),
            anchors=[code['rows'][1]], imports=[
                dict(iat_rva='0xd7118', dll='KERNEL32.dll', name='GetModuleHandleExA'),
                dict(iat_rva='0xd7080', dll='KERNEL32.dll', name='GetModuleFileNameW')],
            caller_name=dict(rva='0xd9380', bytes=self.literal.hex(), text='nvngx.dll'),
            tools=self.tools, published_scaling=dict(keys=[], literals=[]),
            published_stats=dict(keys=[], anchors=[], functions=[], paths=[]),
            internal_create=dict(keys=[], anchors=[], functions=[], tables=[], reads=[], paths=[],
                                 code_span=dict(byte_count=0, instructions=0)),
            descriptor_rect=dict(anchors=[], functions=[], paths=[]),
            shutdown_identity=dict(guids=[], anchors=[], functions=[], paths=[]),
            payload_loaded=False, f7_selected_payload=False, **scope)

    def verify(self):
        self.report.write_text(json.dumps(self.document), encoding='utf-8')
        module = SimpleNamespace(PE=lambda **kwargs: self.pe)
        with (patch.object(evidence, 'REFERENCE_SHA', self.identity['sha256']),
              patch.object(evidence, 'RANGES', dict(fixture=(0x20, 0x26))),
              patch.object(evidence, 'ANCHORS', {0x25: ('nop', '')}),
              patch.object(evidence, 'load_pefile_file', return_value=module),
              patch.object(evidence, 'read_pe', return_value={}),
              patch.object(evidence, 'scaling_contract', return_value=dict(keys=[], literals=[])),
              patch.object(evidence, 'stats_contract',
                           return_value=dict(keys=[], anchors=[], functions=[], paths=[])),
              patch.object(evidence, 'create_contract', return_value=self.document['internal_create']),
              patch.object(evidence, 'rect_contract', return_value=self.document['descriptor_rect']),
              patch.object(evidence, 'shutdown_identity_contract',
                           return_value=dict(guids=[], anchors=[], functions=[], paths=[])),
              patch.object(evidence, 'load_capstone', return_value=(self.decoder, self.tools))):
            return evidence.verify_saved_report(self.report)

    def test_saved_report_is_redecoded_and_counts_come_from_decoder(self):
        result = self.verify()
        self.assertEqual(result['instructions'], 2)
        self.assertIs(result['static_decode_refreshed'], True)
        self.assertIs(result['decoder_identity_verified'], True)

    def test_non_anchor_mnemonic_tampering_is_rejected(self):
        self.document['code']['fixture']['rows'][0]['mnemonic'] = 'ret'
        with self.assertRaisesRegex(ValueError, 'decoded instructions'):
            self.verify()

    def test_fabricated_instruction_boundaries_and_count_are_rejected(self):
        entry = self.document['code']['fixture']
        entry['rows'][:1] = [dict(rva=hex(0x20+i), bytes=bytes((byte,)).hex(),
                                  mnemonic='nop', operands='') for i, byte in enumerate(self.raw[:5])]
        entry['instructions'] = len(entry['rows'])
        with self.assertRaisesRegex(ValueError, 'decoded instructions'):
            self.verify()

    def test_changed_decoder_identity_is_rejected(self):
        self.document['tools'] = dict(self.tools, capstone_root=str(self.root/'another-root'))
        with self.assertRaisesRegex(ValueError, 'decoder toolchain'):
            self.verify()

    def test_ordinary_window_still_requires_exact_pdata_not_containment(self):
        self.pe.DIRECTORY_ENTRY_EXCEPTION[0].struct.EndAddress = 0x27
        with self.assertRaisesRegex(ValueError, 'pdata boundary'):
            self.verify()


class ShutdownIdentityTests(unittest.TestCase):
    """Read/decode the SHA-pinned historical source; all mutations stay in memory.

    The old saved report supplies paths and decoder provenance, not the new
    expected contract. No historical report is generated or updated on disk.
    """

    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parent
        saved = json.loads((root/'evidence/nr-reference-entrypoints.json').read_text(encoding='utf-8'))
        cls.source = Path(saved['input']['path'])
        raw = evidence.reference_bytes(cls.source)
        if evidence.digest(Path(saved['export_inspection']['analyzer']['pefile_path'])) != \
                saved['export_inspection']['analyzer']['pefile_sha256']:
            raise ValueError('historical test pefile identity changed')
        module = evidence.load_pefile_file(Path(saved['export_inspection']['analyzer']['pefile_path']))
        cls.pe = module.PE(data=raw, fast_load=True)
        cls.pe.parse_data_directories(directories=[3])
        cls.decoder, observed = evidence.load_capstone(Path(saved['tools']['capstone_root']))
        if observed != saved['tools']:
            raise ValueError('historical test decoder identity changed')
        cls.decoded = cls.decode(cls.pe)
        cls.contract = evidence.shutdown_identity_contract(cls.pe, cls.decoded)
        cls.fixture = copy.deepcopy(saved)
        cls.fixture['code'] = cls.decoded
        cls.fixture['shutdown_identity'] = cls.contract
        # The archived report intentionally remains legacy until Lead regenerates
        # it. Refresh new contracts from the real source entirely in memory.
        cls.stats = evidence.stats_contract(cls.pe, cls.decoded)
        cls.fixture['published_stats'] = cls.stats
        cls.create = evidence.create_contract(cls.pe, cls.decoded)
        cls.fixture['internal_create'] = cls.create
        cls.rect = evidence.rect_contract(cls.pe, cls.decoded)
        cls.fixture['descriptor_rect'] = cls.rect
        cls.fixture['published_scaling'] = evidence.scaling_contract(cls.pe, saved['export_inspection'])
        rows = {int(row['rva'], 16): row for span in cls.decoded.values() for row in span['rows']}
        cls.fixture['anchors'] = evidence.check_anchors(rows)

    def setUp(self):
        self.document = copy.deepcopy(self.fixture)

    @classmethod
    def decode(cls, pe):
        return {name: evidence.decode_window(pe, cls.decoder, cls.pe.OPTIONAL_HEADER.ImageBase, lo, hi)
                for name, (lo, hi) in evidence.RANGES.items()}

    def changed_pe(self, rva=None, replacement=b'', entries=None):
        def get_data(lo, size):
            raw = self.pe.get_data(lo, size)
            if rva is None:
                return raw
            begin, end = max(lo, rva), min(lo+len(raw), rva+len(replacement))
            if begin >= end:
                return raw
            return raw[:begin-lo]+replacement[begin-rva:end-rva]+raw[end-lo:]
        return SimpleNamespace(get_data=get_data, get_rva_from_offset=self.pe.get_rva_from_offset,
                               DIRECTORY_ENTRY_EXCEPTION=(self.pe.DIRECTORY_ENTRY_EXCEPTION
                                                          if entries is None else entries))

    def verify(self):
        # Exercise the real saved-report verifier without writing a real report.
        virtual = Path(__file__).with_name('_in_memory_shutdown_fixture.json')
        serialized = json.dumps(self.document, allow_nan=False)
        original_read, original_digest = Path.read_text, evidence.digest

        def read_text(path, *args, **kwargs):
            return serialized if path == virtual else original_read(path, *args, **kwargs)

        def digest(path):
            return (evidence.hashlib.sha256(serialized.encode()).hexdigest()
                    if path == virtual else original_digest(path))

        with patch.object(Path, 'read_text', new=read_text), patch.object(evidence, 'digest', side_effect=digest):
            return evidence.verify_saved_report(virtual)

    def contract_path(self, name):
        return next(row for row in self.document['shutdown_identity']['paths'] if row['name'] == name)

    def test_original_bytes_and_complete_pdata_inventory(self):
        self.assertEqual(len(self.decoded), 25)
        self.assertEqual(sum(span['instructions'] for span in self.decoded.values()), 1705)
        self.assertEqual(sum(self.decoded[name]['instructions'] for name in evidence.SHUTDOWN_FUNCTIONS), 172)
        self.assertEqual(len(self.contract['functions']), 9)
        self.assertEqual([self.decoded[name]['instructions'] for name in evidence.SHUTDOWN_FUNCTIONS],
                         [34, 8, 13, 19, 3, 9, 62, 8, 16])
        self.assertEqual(len(self.contract['anchors']), 127)
        self.assertEqual(len(self.contract['paths']), 15)
        self.assertEqual(self.contract['guids'],
                         [dict(rva='0xd8d60', bytes='11d7e043ea7b684ea077e9d11a7625f3', size=16)])

    def test_fresh_saved_verification_and_array_counts(self):
        from verify import historical_shutdown_identity_counts
        result = self.verify()
        counts = historical_shutdown_identity_counts(self.document)
        self.assertEqual(counts, dict(shutdown_identity_guid_assertions=1,
                                     shutdown_identity_instruction_assertions=127,
                                     shutdown_identity_function_assertions=9,
                                     shutdown_identity_path_assertions=15))
        self.assertEqual({key: result[key] for key in counts}, counts)
        self.assertEqual(result['windows'], 25)
        self.assertEqual(result['instructions'], 1705)
        self.assertEqual(result['instruction_assertions'], 110)
        self.assertEqual({field: result[field] for field in evidence.STATS_ARRAYS},
                         dict(stats_key_assertions=3, stats_instruction_assertions=79,
                              stats_function_assertions=1, stats_path_assertions=11))
        self.assertIs(result['static_decode_refreshed'], True)
        self.assertIs(result['abi_authenticated'], False)

    def test_semantic_bounds_are_explicit(self):
        read = self.contract_path('read-vtable-arguments')
        self.assertEqual((read['size_storage_bits'], read['requested_size'], read['output_bytes'],
                          read['initial_output']), (32, 8, 8, 0))
        self.assertFalse(self.contract_path('read-nonnegative-result')['returned_size_rechecked'])
        generated = self.contract_path('read-negative-generates-token')
        self.assertEqual(generated['atomic_global_increment'], 1)
        self.assertEqual(generated['local_rax_old_value_increment'], 1)
        self.assertFalse(self.contract_path('write-vtable-result-unchecked')['write_result_checked'])
        helper = self.contract_path('helper-unique-normal-return-literal-one')
        self.assertEqual(helper['normal_return_sites'], ['0x1b20a'])
        self.assertEqual(helper['normal_eax'], 1)
        self.assertFalse(self.contract_path('common-non-one-conditional-only')['runtime_failure_reachability_proven'])
        self.assertFalse(self.contract_path('init-token-address-handoff')['full_session_map_chain_proven'])

    def test_real_guid_byte_drift_is_rejected(self):
        raw = self.pe.get_data(0xd8d60, 16)
        pe = self.changed_pe(0xd8d60, bytes((raw[0] ^ 1,))+raw[1:])
        with self.assertRaisesRegex(ValueError, 'GUID bytes'):
            evidence.shutdown_identity_contract(pe, self.decoded)

    def test_real_instruction_byte_drift_after_fresh_decode_is_rejected(self):
        mutations = {
            0xe8b6: '48c744244801000000',  # preinitialized output is no longer zero
            0xe8dc: 'c744244004000000',    # DWORD size is no longer eight
            0xe8eb: '488b4010',          # read vtable offset drift
            0xe8f7: '7838',              # nonnegative-result branch changed
            0xe8fe: 'f0480fc10512171000', # atomic global address drift
            0xe907: '48ffc8',            # local old value is decremented
            0xe91e: '41b804000000',      # write size drift
            0xe927: '488b4028',          # write vtable offset drift
            0xe931: '488b442440',        # returned slot drift
            0x13dc9: '4c8d442438',       # Init no longer passes the token slot
            0x14287: '7411',            # null selector destination drift
            0x14289: '4d3926',          # token comparison register drift
            0x1428e: '41ffcf',          # skipped counter drift
            0x14294: 'e90f010000',      # skip continuation drift
            0x23de9: '8bc0',            # helper result no longer saved in EDI
            0x23e4f: '8bc0',            # cleanup no longer returns saved EDI
            0x1b1f9: 'b802000000',      # helper normal return literal drift
            0x1b20a: 'cc',              # unique normal ret removed
        }
        for rva, raw in mutations.items():
            with self.subTest(rva=hex(rva)):
                pe = self.changed_pe(rva, bytes.fromhex(raw))
                decoded = self.decode(pe)
                with self.assertRaisesRegex(ValueError, 'instruction changed'):
                    evidence.shutdown_identity_contract(pe, decoded)

    def test_extra_helper_normal_return_is_rejected(self):
        pe = self.changed_pe(0x1b0c3, bytes.fromhex('c390909090'))
        with self.assertRaisesRegex(ValueError, 'helper normal return'):
            evidence.shutdown_identity_contract(pe, self.decode(pe))

    def test_each_pdata_end_and_unwind_field_drift_is_rejected(self):
        for name, (lo, hi, unwind, _) in evidence.SHUTDOWN_FUNCTIONS.items():
            for field, replacement in (('BeginAddress', lo+1), ('EndAddress', hi-1),
                                       ('UnwindData', unwind+4)):
                with self.subTest(name=name, field=field):
                    entries = []
                    for entry in self.pe.DIRECTORY_ENTRY_EXCEPTION:
                        original = entry.struct
                        if original.BeginAddress == lo:
                            values = dict(BeginAddress=lo, EndAddress=hi, UnwindData=unwind,
                                          get_file_offset=original.get_file_offset)
                            values[field] = replacement
                            entry = SimpleNamespace(struct=SimpleNamespace(**values))
                        entries.append(entry)
                    with self.assertRaisesRegex(ValueError, 'pdata/unwind'):
                        evidence.shutdown_identity_contract(self.changed_pe(entries=entries), self.decoded)

    def test_missing_or_duplicate_pdata_entries_are_rejected(self):
        for name, (lo, _, _, _) in evidence.SHUTDOWN_FUNCTIONS.items():
            entry = next(row for row in self.pe.DIRECTORY_ENTRY_EXCEPTION if row.struct.BeginAddress == lo)
            for entries in ([row for row in self.pe.DIRECTORY_ENTRY_EXCEPTION if row is not entry],
                            [*self.pe.DIRECTORY_ENTRY_EXCEPTION, entry]):
                with self.subTest(name=name, entries=len(entries)), self.assertRaisesRegex(ValueError, 'pdata/unwind'):
                    evidence.shutdown_identity_contract(self.changed_pe(entries=entries), self.decoded)

    def test_real_pdata_record_bytes_are_rechecked(self):
        for function in self.contract['functions']:
            with self.subTest(name=function['name']):
                raw = bytes.fromhex(function['pdata_bytes'])
                pe = self.changed_pe(int(function['pdata_rva'], 16), bytes((raw[0] ^ 1,))+raw[1:])
                with self.assertRaisesRegex(ValueError, 'pdata bytes'):
                    evidence.shutdown_identity_contract(pe, self.decoded)

    def test_each_real_unwind_byte_span_is_rechecked(self):
        for function in self.contract['functions']:
            with self.subTest(name=function['name']):
                raw = bytes.fromhex(function['unwind_bytes'])
                pe = self.changed_pe(int(function['unwind_rva'], 16), raw[:-1]+bytes((raw[-1] ^ 1,)))
                with self.assertRaisesRegex(ValueError, 'unwind bytes'):
                    evidence.shutdown_identity_contract(pe, self.decoded)

    def test_each_new_decoded_boundary_drift_is_rejected(self):
        for name in evidence.SHUTDOWN_FUNCTIONS:
            for field in ('lo', 'hi'):
                with self.subTest(name=name, field=field):
                    code = copy.deepcopy(self.decoded)
                    code[name][field] = hex(int(code[name][field], 16)+1)
                    with self.assertRaisesRegex(ValueError, 'decoded boundary'):
                        evidence.shutdown_identity_contract(self.pe, code)

    def test_cleanup_and_helper_cannot_be_collapsed_into_one_window(self):
        for names in (tuple(name for name in evidence.SHUTDOWN_FUNCTIONS if 'cleanup' in name),
                      tuple(name for name in evidence.SHUTDOWN_FUNCTIONS if 'helper' in name)):
            code = copy.deepcopy(self.decoded)
            first, last = code[names[0]], code[names[-1]]
            first['hi'] = last['hi']
            for name in names[1:]:
                del code[name]
            with self.assertRaisesRegex(ValueError, 'decoded boundary'):
                evidence.shutdown_identity_contract(self.pe, code)

    def test_saved_contract_guid_and_unwind_tampering_are_rejected(self):
        for array, field in (('guids', 'bytes'), ('functions', 'pdata_bytes'),
                             ('functions', 'unwind_rva'), ('functions', 'unwind_bytes')):
            with self.subTest(array=array, field=field):
                self.document = copy.deepcopy(self.fixture)
                self.document['shutdown_identity'][array][0][field] = '00'
                with self.assertRaisesRegex(ValueError, 'shutdown identity contract'):
                    self.verify()

    def test_saved_nonlegacy_anchor_bytes_cannot_be_hidden_by_window_hash(self):
        span = self.document['code']['object-token']
        row = next(row for row in span['rows'] if row['rva'] == '0xe8eb')
        row['bytes'] = '488b4010'
        span['sha256'] = evidence.hashlib.sha256(bytes.fromhex(''.join(row['bytes'] for row in span['rows']))).hexdigest()
        with self.assertRaisesRegex(ValueError, 'decoded instructions'):
            self.verify()

    def test_saved_contract_anchor_and_path_deletion_are_rejected(self):
        for array in ('anchors', 'paths', 'functions', 'guids'):
            with self.subTest(array=array):
                self.document = copy.deepcopy(self.fixture)
                self.document['shutdown_identity'][array].pop()
                with self.assertRaisesRegex(ValueError, 'shutdown identity contract'):
                    self.verify()

    def test_saved_semantic_field_drift_is_rejected(self):
        changes = (
            ('read-vtable-arguments', 'size_storage_bits', 64),
            ('read-vtable-arguments', 'initial_output', 1),
            ('read-nonnegative-result', 'returned_size_rechecked', True),
            ('read-negative-generates-token', 'atomic_global_increment', 2),
            ('write-vtable-result-unchecked', 'write_result_checked', True),
            ('write-vtable-result-unchecked', 'dispatch', 'SetPrivateData IAT'),
            ('null-selects-shown-container', 'scope', 'all process objects'),
            ('nonnull-token-match', 'raw_pointer_comparison', True),
            ('helper-unique-normal-return-literal-one', 'normal_eax', 0),
            ('helper-unique-normal-return-literal-one', 'normal_eax', True),
            ('cleanup-preserves-helper-result', 'return_value', 'last free EAX'),
            ('common-non-one-conditional-only', 'runtime_failure_reachability_proven', True),
            ('init-token-address-handoff', 'full_session_map_chain_proven', True),
        )
        for name, field, replacement in changes:
            with self.subTest(path=name, field=field):
                self.document = copy.deepcopy(self.fixture)
                self.contract_path(name)[field] = replacement
                with self.assertRaisesRegex(ValueError, 'shutdown identity contract'):
                    self.verify()

    def test_saved_overclaims_and_extra_scope_fields_are_rejected(self):
        fields = ('target_executed', 'payload_loaded', 'gpu_tested', 'f7_selected_payload',
                  'abi_authenticated', 'hardware_support_proven', 'gpu_fence_identity',
                  'raw_pointer_identity', 'stable_guid_authentication',
                  'same_object_future_key_stability_proven', 'token_instability_reproduced',
                  'actual_failure_reproduced', 'full_session_map_chain_proven',
                  'map_insertion_abi_authenticated', 'caller_guard_bypassed', 'unsupported_new_claim')
        for field, replacement in [*((field, True) for field in fields), ('abi_authenticated', 0)]:
            with self.subTest(field=field, replacement=replacement):
                self.document = copy.deepcopy(self.fixture)
                self.document['shutdown_identity'][field] = replacement
                with self.assertRaisesRegex(ValueError, 'shutdown identity contract'):
                    self.verify()

    def test_saved_unknown_dynamic_boundaries_cannot_be_promoted(self):
        for field in ('support', 'deep_dynamic_calls', 'exceptions'):
            with self.subTest(field=field):
                self.document = copy.deepcopy(self.fixture)
                self.document['shutdown_identity'][field] = 'PROVEN'
                with self.assertRaisesRegex(ValueError, 'shutdown identity contract'):
                    self.verify()

    def test_saved_missing_contract_is_rejected(self):
        del self.document['shutdown_identity']
        with self.assertRaisesRegex(ValueError, 'shutdown identity contract'):
            self.verify()

    def test_saved_same_sha_alternate_source_is_accepted_and_fully_redecoded(self):
        alternate = self.source.with_name('same-content-reference-copy.dll')
        raw = self.source.read_bytes()
        self.document['input']['path'] = str(alternate)
        self.document['export_inspection']['input']['path'] = str(alternate)
        original_read = Path.read_bytes

        def read_bytes(path):
            return raw if path == alternate else original_read(path)

        with (patch.object(Path, 'read_bytes', new=read_bytes),
              patch.object(evidence, 'decode_window', wraps=evidence.decode_window) as decode,
              patch.object(evidence, 'shutdown_identity_contract',
                           wraps=evidence.shutdown_identity_contract) as contract):
            result = self.verify()
        self.assertEqual(decode.call_count, len(evidence.RANGES))
        contract.assert_called_once()
        self.assertEqual(result['input_sha256'], evidence.REFERENCE_SHA)
        self.assertEqual((result['windows'], result['instructions']), (25, 1705))
        self.assertIs(result['static_decode_refreshed'], True)
        self.assertIs(result['abi_authenticated'], False)
        self.assertIs(result['f7_selected_payload'], False)
        self.assertIs(self.document['shutdown_identity']['hardware_support_proven'], False)

    def test_saved_changed_content_alternate_source_is_refused_before_import_or_output(self):
        alternate = self.source.with_name('different-content-reference-copy.dll')
        self.document['input']['path'] = str(alternate)
        self.document['export_inspection']['input']['path'] = str(alternate)
        original_read = Path.read_bytes

        def read_bytes(path):
            return b'different historical reference bytes' if path == alternate else original_read(path)

        with (patch.object(Path, 'read_bytes', new=read_bytes),
              patch.object(evidence, 'load_pefile_file') as loader,
              patch.object(evidence, 'load_capstone') as decoder,
              patch.object(evidence, 'decode_window') as decode,
              patch.object(Path, 'write_text') as write_text,
              patch.object(Path, 'write_bytes') as write_bytes,
              self.assertRaisesRegex(ValueError, 'reference identity')):
            self.verify()
        for operation in (loader, decoder, decode, write_text, write_bytes):
            operation.assert_not_called()

    def test_changed_original_bytes_are_refused_before_dependency_import(self):
        original_read = Path.read_bytes
        offset = self.pe.get_offset_from_rva(0xe8eb)

        def read_bytes(path):
            raw = original_read(path)
            if path == self.source:
                raw = raw[:offset]+bytes((raw[offset] ^ 1,))+raw[offset+1:]
            return raw

        with (patch.object(Path, 'read_bytes', new=read_bytes),
              patch.object(evidence, 'load_pefile_file') as loader,
              self.assertRaisesRegex(ValueError, 'reference identity')):
            self.verify()
        loader.assert_not_called()

    def test_historical_counts_use_arrays_and_refuse_malformed_arrays(self):
        from verify import historical_shutdown_identity_counts
        for field, array in evidence.SHUTDOWN_IDENTITY_ARRAYS.items():
            record = copy.deepcopy(self.fixture)
            record[field] = 99999
            record['shutdown_identity'][array].pop()
            counts = historical_shutdown_identity_counts(record)
            self.assertEqual(counts[field], len(self.contract[array])-1)
            for malformed in (None, {}, 9, ()):
                with self.subTest(array=array, malformed=malformed):
                    record['shutdown_identity'][array] = malformed
                    with self.assertRaisesRegex(ValueError, 'invalid historical NR shutdown identity array'):
                        historical_shutdown_identity_counts(record)
        with self.assertRaisesRegex(ValueError, 'missing historical NR shutdown identity'):
            historical_shutdown_identity_counts({})

    def stats_path(self, name):
        return next(row for row in self.document['published_stats']['paths'] if row['name'] == name)

    def test_stats_real_bytes_and_single_function_inventory(self):
        span = self.decoded['published-stats-callback']
        self.assertEqual((span['lo'], span['hi'], span['instructions']), ('0x14920', '0x14a72', 79))
        self.assertEqual(self.stats['anchors'], span['rows'])
        self.assertEqual(self.stats['keys'], [
            dict(rva='0xd9740', name='SizeInBytes', bytes='53697a65496e427974657300'),
            dict(rva='0xd974c', name='#D', bytes='234400'),
            dict(rva='0xd9750', name='#E', bytes='234500')])
        self.assertEqual(self.stats['functions'], [dict(name='published-stats-callback',
            lo='0x14920', hi='0x14a72', pdata_rva='0x4aa9bc',
            pdata_bytes='20490100724a0100a4f20f00', unwind_rva='0xff2a4',
            unwind_bytes='110a04000a3409000a320670cc740a00b8f20f00')])
        self.assertEqual(len(self.stats['paths']), 11)
        self.assertIs(self.fixture['published_scaling']['stats_callback_body_decoded'], True)

    def test_stats_semantics_remain_bounded_to_shown_body(self):
        self.assertEqual(self.stats_path('null-parameter')['normal_eax'], '0xbad00005')
        outputs = self.stats_path('zero-outputs-and-lock')
        self.assertEqual((outputs['slots'], outputs['output_bits'], outputs['initial_value']),
                         (['rsp+0x30', 'rsp+0x38'], 64, 0))
        self.assertEqual(outputs['lock_address_rva'], '0x4a0d40')
        failure = self.stats_path('lock-nonzero-abnormal-tail')
        self.assertEqual(failure['helper_rva'], '0xa3e44')
        self.assertEqual(failure['helper_semantics'], 'UNKNOWN; may throw')
        self.assertNotIn('normal_eax', failure)
        selection = self.stats_path('first-record-object-selection')
        self.assertEqual((selection['global_head_pointer_rva'], selection['first_node'], selection['object']),
                         ('0x4a0d98', 'qword [head]', 'qword [first node+0x88]'))
        self.assertIs(selection['iteration_in_shown_body'], False)
        self.assertIs(selection['device_or_feature_key_selection_in_shown_body'], False)
        call = self.stats_path('object-vtable-call')
        self.assertEqual((call['vtable_offset'], call['object_interface'], call['method_name']),
                         ('0x1f8', 'UNKNOWN', 'UNKNOWN'))
        self.assertEqual(call['dispatch'], 'indirect RAX target via slot 0xd7428')
        self.assertEqual((call['first_output_address'], call['second_output_address']),
                         ('RDX = &rsp+0x30', 'R8 = &rsp+0x38'))
        self.assertIs(self.stats_path('unchecked-result-then-unlock')['returned_value_checked'], False)
        output = self.stats_path('write-first-output')
        self.assertEqual((output['value'], output['value_bits'], output['vtable_offset']),
                         ('qword [rsp+0x30]', 64, '0x38'))
        self.assertIs(output['second_output_used_in_shown_success_path'], False)
        self.assertEqual((self.stats_path('write-raw-D')['value'], self.stats_path('write-raw-E')['value']),
                         (40, 0))
        self.assertEqual(self.stats_path('literal-success')['normal_eax'], 1)
        self.assertIs(self.stats_path('literal-success')['setter_results_checked'], False)
        self.assertEqual(self.stats_path('missing-record-or-object')['normal_eax'], '0xbad00007')
        for field in ('target_executed', 'callback_executed', 'payload_loaded', 'gpu_tested',
                      'abi_authenticated', 'f7_selected_payload', 'hardware_support_proven',
                      'gpu_completion_proven', 'gpu_fence_identity'):
            self.assertIs(self.stats[field], False)
        for field in ('registry_insertion_and_aggregate_semantics', 'object_interface',
                      'slot_1f8_method_name', 'deep_dynamic_calls', 'exceptions', 'support'):
            self.assertEqual(self.stats[field], 'UNKNOWN')
        self.assertEqual(self.stats['observed_role'], 'stats output retrieval and parameter writeback')
        self.assertNotIn('telemetry_only', self.stats)

    def test_stats_each_key_and_terminator_byte_drift_is_rejected(self):
        for key in self.stats['keys']:
            raw = bytes.fromhex(key['bytes'])
            for offset in (0, len(raw)-1):
                with self.subTest(key=key['name'], offset=offset):
                    pe = self.changed_pe(int(key['rva'], 16)+offset, bytes((raw[offset] ^ 1,)))
                    with self.assertRaisesRegex(ValueError, 'stats key bytes'):
                        evidence.stats_contract(pe, self.decoded)

    def test_stats_real_instruction_drift_including_selection_and_unchecked_adjacency(self):
        mutations = {
            0x1492d: '4885d2',            # input guard changed from RCX
            0x14951: 'b80600d0ba',        # null-parameter result
            0x14963: '4889442438',        # first output zeroing slot
            0x14968: '4889442430',        # second output zeroing slot
            0x1496d: '488d3dcdc34800',    # global lock address
            0x1497c: 'e8b0f30800',        # in-module lock helper target
            0x14983: '0f84e1000000',      # lock failure branch condition
            0x14989: '488b0509c44800',    # global head pointer source
            0x14990: '488b5008',          # no longer loads first node [head]
            0x14993: '483bd1',            # head comparison register
            0x14996: '0f8497000000',      # empty-container destination
            0x1499c: '488b8a80000000',    # selected object offset
            0x149a6: '0f8586000000',      # null-object branch condition
            0x149af: '4c8d442430',        # second output address
            0x149b4: '488d542438',        # first output address
            0x149b9: '488b8000020000',    # dynamic vtable offset
            0x149c0: 'ff15632a0c00',      # dispatch slot
            0x149c6: '85c09090',          # inserts test EAX instead of NOP + mov RCX
            0x149c7: '488bd0',            # unlock no longer receives lock address
            0x149ca: 'e86af30800',        # unlock direct target
            0x149d2: '4c8b442438',        # uses second output instead of first
            0x149d7: '488d15634d0c00',    # SizeInBytes key reference
            0x149e1: '488b4030',          # first setter vtable offset
            0x149ee: '41b829000000',      # #D value
            0x149fe: '488b4020',          # #D setter vtable offset
            0x14a0b: '4533c9',            # #E value no longer zeroed
            0x14a22: 'b802000000',        # literal success
            0x14a5a: 'b80600d0ba',        # missing-record result
            0x14a6c: 'e8d4f30800',        # abnormal helper target
            0x14a71: 'c3',                # ordinary return replaces int3
        }
        for rva, raw in mutations.items():
            with self.subTest(rva=hex(rva)):
                pe = self.changed_pe(rva, bytes.fromhex(raw))
                lo, hi = evidence.RANGES['published-stats-callback']
                code = dict(self.decoded)
                code['published-stats-callback'] = evidence.decode_window(
                    pe, self.decoder, self.pe.OPTIONAL_HEADER.ImageBase, lo, hi)
                with self.assertRaisesRegex(ValueError, 'stats instruction'):
                    evidence.stats_contract(pe, code)

    def test_stats_original_bytes_rechecked_even_with_stale_decode(self):
        pe = self.changed_pe(0x149c6, b'\xcc')
        with self.assertRaisesRegex(ValueError, 'stats instruction'):
            evidence.stats_contract(pe, self.decoded)

    def test_stats_pdata_fields_missing_and_duplicate_records_are_rejected(self):
        selected = next(row for row in self.pe.DIRECTORY_ENTRY_EXCEPTION
                        if row.struct.BeginAddress == 0x14920)
        other = [row for row in self.pe.DIRECTORY_ENTRY_EXCEPTION if row is not selected]
        entry_sets = [other, [*other, selected, selected]]
        for field, replacement in (('BeginAddress', 0x14921), ('EndAddress', 0x14a71),
                                   ('UnwindData', 0xff2a8)):
            values = dict(BeginAddress=0x14920, EndAddress=0x14a72, UnwindData=0xff2a4,
                          get_file_offset=selected.struct.get_file_offset)
            values[field] = replacement
            entry_sets.append([*other, SimpleNamespace(struct=SimpleNamespace(**values))])
        for index, entries in enumerate(entry_sets):
            with self.subTest(case=index), self.assertRaisesRegex(ValueError, 'stats pdata/unwind'):
                evidence.stats_contract(self.changed_pe(entries=entries), self.decoded)

    def test_stats_raw_pdata_and_unwind_bytes_are_rechecked(self):
        function = self.stats['functions'][0]
        for field, byte_field, message in (('pdata_rva', 'pdata_bytes', 'stats pdata bytes'),
                                          ('unwind_rva', 'unwind_bytes', 'stats unwind bytes')):
            raw = bytes.fromhex(function[byte_field])
            # Include both unwind header and trailing handler data, not just prologue codes.
            for offset in (0, len(raw)-1):
                pe = self.changed_pe(int(function[field], 16)+offset, bytes((raw[offset] ^ 1,)))
                with self.subTest(field=field, offset=offset), self.assertRaisesRegex(ValueError, message):
                    evidence.stats_contract(pe, self.decoded)

    def test_stats_decoded_boundary_and_complete_adjacency_are_required(self):
        for field in ('lo', 'hi'):
            code = copy.deepcopy(self.decoded)
            code['published-stats-callback'][field] = hex(int(code['published-stats-callback'][field], 16)+1)
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'stats decoded boundary'):
                evidence.stats_contract(self.pe, code)
        with self.assertRaisesRegex(ValueError, 'stats decoded boundary'):
            evidence.stats_contract(self.pe, {})
        for modification in ('delete', 'duplicate', 'reorder'):
            code = copy.deepcopy(self.decoded)
            rows = code['published-stats-callback']['rows']
            index = next(index for index, row in enumerate(rows) if row['rva'] == '0x149c6')
            if modification == 'delete':
                rows.pop(index)
            elif modification == 'duplicate':
                rows.insert(index, dict(rows[index]))
            else:
                rows[index:index+2] = rows[index:index+2][::-1]
            with self.subTest(modification=modification), self.assertRaisesRegex(ValueError, 'stats instruction'):
                evidence.stats_contract(self.pe, code)

    def test_stats_saved_contract_missing_or_incomplete_is_rejected(self):
        del self.document['published_stats']
        with self.assertRaisesRegex(ValueError, 'published stats contract'):
            self.verify()
        for array in evidence.STATS_ARRAYS.values():
            for mode in ('delete', 'remove-one', 'wrong-type'):
                self.document = copy.deepcopy(self.fixture)
                contract = self.document['published_stats']
                if mode == 'delete':
                    del contract[array]
                elif mode == 'remove-one':
                    contract[array].pop()
                else:
                    contract[array] = {}
                with self.subTest(array=array, mode=mode), self.assertRaisesRegex(ValueError, 'published stats contract'):
                    self.verify()

    def test_stats_saved_bytes_path_and_function_tampering_are_rejected(self):
        for array, field in (('keys', 'bytes'), ('keys', 'name'), ('anchors', 'bytes'),
                             ('functions', 'unwind_rva'), ('functions', 'unwind_bytes'),
                             ('functions', 'pdata_bytes'), ('paths', 'anchor_rvas')):
            self.document = copy.deepcopy(self.fixture)
            self.document['published_stats'][array][0][field] = '00'
            with self.subTest(array=array, field=field), self.assertRaisesRegex(ValueError, 'published stats contract'):
                self.verify()

    def test_stats_saved_semantic_and_json_type_tampering_are_rejected(self):
        changes = (
            ('zero-outputs-and-lock', 'initial_value', False),
            ('zero-outputs-and-lock', 'output_bits', 32),
            ('zero-outputs-and-lock', 'lock_role', 'OS mutex import'),
            ('lock-nonzero-abnormal-tail', 'helper_semantics', 'known normal error return'),
            ('lock-nonzero-abnormal-tail', 'normal_eax', '0xbad00007'),
            ('first-record-object-selection', 'iteration_in_shown_body', True),
            ('first-record-object-selection', 'device_or_feature_key_selection_in_shown_body', True),
            ('object-vtable-call', 'method_name', 'GetStats'),
            ('object-vtable-call', 'dispatch', 'authenticated CFG import'),
            ('unchecked-result-then-unlock', 'returned_value_checked', True),
            ('write-first-output', 'second_output_used_in_shown_success_path', True),
            ('write-raw-D', 'value', 40.0),
            ('write-raw-E', 'value', False),
            ('literal-success', 'normal_eax', True),
            ('literal-success', 'setter_results_checked', 0),
            ('literal-success', 'gpu_completion_proven', True),
            ('missing-record-or-object', 'normal_eax', '0xbad00005'),
        )
        for name, field, replacement in changes:
            self.document = copy.deepcopy(self.fixture)
            self.stats_path(name)[field] = replacement
            with self.subTest(path=name, field=field), self.assertRaisesRegex(ValueError, 'published stats contract'):
                self.verify()

    def test_stats_saved_scope_unknowns_and_extra_claims_fail_closed(self):
        changes = [(field, True) for field in (
            'target_executed', 'callback_executed', 'payload_loaded', 'gpu_tested',
            'abi_authenticated', 'f7_selected_payload', 'hardware_support_proven',
            'gpu_completion_proven', 'gpu_fence_identity', 'unsupported_new_claim')]
        changes += [(field, 'PROVEN') for field in (
            'object_interface', 'slot_1f8_method_name', 'deep_dynamic_calls', 'exceptions',
            'support', 'registry_insertion_and_aggregate_semantics')]
        changes += [('scope', 'full registry and aggregate behavior'), ('schema', True),
                    ('schema', 1.0), ('telemetry_only', True), ('telemetry_only', False),
                    ('observed_role', 'side-effect-free telemetry only'), ('abi_authenticated', 0)]
        for field, replacement in changes:
            self.document = copy.deepcopy(self.fixture)
            self.document['published_stats'][field] = replacement
            with self.subTest(field=field, replacement=replacement), self.assertRaisesRegex(ValueError, 'published stats contract'):
                self.verify()

    def test_stats_saved_decode_cannot_be_disabled_or_retyped(self):
        for replacement in (False, 1):
            self.document = copy.deepcopy(self.fixture)
            self.document['published_scaling']['stats_callback_body_decoded'] = replacement
            with self.subTest(value=replacement), self.assertRaisesRegex(ValueError, 'published scaling contract'):
                self.verify()

    def test_stats_saved_counts_use_verified_arrays_not_external_totals(self):
        for field in evidence.STATS_ARRAYS:
            self.document[field] = 99999
        result = self.verify()
        for field, array in evidence.STATS_ARRAYS.items():
            self.assertEqual(result[field], len(self.stats[array]))
        self.assertEqual(result['instruction_assertions'], 110)

    def test_stats_build_report_includes_fresh_contract_without_writing_archive(self):
        report = evidence.build_report(self.source,
            Path(self.fixture['export_inspection']['analyzer']['pefile_path']),
            Path(self.fixture['tools']['capstone_root']))
        self.assertEqual(report['published_stats'], self.stats)
        self.assertIs(report['published_scaling']['stats_callback_body_decoded'], True)
        self.assertEqual(len(report['code']), 25)
        self.assertEqual(sum(span['instructions'] for span in report['code'].values()), 1705)
        self.assertEqual(len(report['anchors']), 110)
        self.assertEqual(report['internal_create'], self.create)

    def create_path(self, name):
        return next(row for row in self.document['internal_create']['paths'] if row['name'] == name)

    def test_create_code_data_partition_parent_and_real_counts(self):
        contract = self.create
        self.assertEqual(contract['code_span'], dict(lo='0x17b20', hi='0x18140', byte_count=1568,
            instructions=379, sha256='a9eb49104e3630d71e02554eed5fd14c8b6589b1f64aa23d9fe4526ccbf973fd'))
        self.assertEqual(len(contract['functions']), 1)
        function = contract['functions'][0]
        self.assertEqual((function['lo'], function['hi'], function['code_hi'], function['pdata_rva'],
                          function['unwind_rva'], function['pdata_bytes']),
                         ('0x17b20', '0x181d1', '0x18140', '0x4aad40', '0xff7bc',
                          '207b0100d1810100bcf70f00'))
        self.assertEqual([(table['lo'], table['hi'], table['byte_count']) for table in contract['tables']],
                         [('0x18140', '0x18160', 32), ('0x18160', '0x181d1', 113)])
        self.assertEqual(sum(table['byte_count'] for table in contract['tables']), 145)
        self.assertEqual(1568+145, 0x181d1-0x17b20)
        rows = self.decoded['create-common-body']['rows']
        self.assertEqual(rows[-2]['rva'], '0x1813c')
        self.assertEqual(rows[-1], dict(rva='0x1813d', bytes='0f1f00', mnemonic='nop',
                                       operands='dword ptr [rax]'))
        self.assertTrue(all(int(row['rva'], 16) < 0x18140 for row in rows))
        self.assertTrue(all(table['decoded_as_code'] is False for table in contract['tables']))

    def test_create_raw_selector_targets_and_low32_mapping(self):
        targets, indices = (table['entries'] for table in self.create['tables'])
        self.assertEqual(targets, [0x17bc5, 0x17c2b, 0x17c2b, 0x17c2b,
                                  0x17c2b, 0x17be8, 0x17c2b, 0x17bef])
        self.assertEqual(len(indices), 113)
        cases = [(0x140, 0, 0x17bc5), (0x150, 0, 0x17bc5), (0x160, 1, 0x17c2b),
                 (0x170, 2, 0x17c2b), (0x180, 3, 0x17c2b), (0x190, 4, 0x17c2b),
                 (0x1a0, 5, 0x17be8), (0x1b0, 6, 0x17c2b)]
        self.assertEqual(self.create['selector']['indexed_cases'],
                         [dict(value=hex(value), index=index, target_rva=hex(target))
                          for value, index, target in cases])
        explicit_offsets = {value-0x140 for value, _, _ in cases}
        self.assertTrue(all(index == 7 for offset, index in enumerate(indices) if offset not in explicit_offsets))
        self.assertEqual(self.create['selector']['input'], 'low 32 bits of EAX from object slot +0x38')
        self.assertIn('unsigned', self.create['selector']['table_condition'])
        self.assertIn('signed EAX >= 0x140', self.create['selector']['fallback_condition'])
        self.assertIs(self.create['selector']['hardware_support_proven'], False)

    def test_create_five_direct_defaults_and_unknown_nested_reads(self):
        expected = [('CreationNodeMask', '0x60', 'uint32', 1),
                    ('VisibilityNodeMask', '0x60', 'uint32', 1),
                    ('DLSSNR.Width', '0x60', 'uint32', 0),
                    ('DLSSNR.Height', '0x60', 'uint32', 0),
                    ('DLSSNR.Hint.Render.Preset', '0x58', 'int32', 0)]
        self.assertEqual([(row['name'], row['parameter_vtable_offset'], row['value_type'],
                           row['otherwise_value']) for row in self.create['reads']], expected)
        self.assertTrue(all(row['exact_success_eax'] == 1 for row in self.create['reads']))
        self.assertEqual(self.create['reads'][-1]['postprocess'], 'clear bit 31 via BTR ECX,31')
        passed = self.create_path('parameter-pass-before-direct-reads')
        self.assertEqual((passed['vtable_offset'], passed['method_name']), ('0x1c8', 'UNKNOWN'))
        self.assertEqual(passed['nested_parameter_reads'],
                         'UNKNOWN; parameter object passed before five direct reads')
        self.assertEqual(self.create['nested_calls_and_parameter_reads'], 'UNKNOWN')
        self.assertNotIn('no_nested_reads', self.create)
        self.assertEqual(self.create_path('direct-read-entry-default-register')['scope'],
                         'these five direct reads only')
        self.assertEqual(self.create_path('additional-indirect-zero-gate')['comparison'],
                         'zero/nonzero, not BAD-family mask')
        for name in ('resolution-call-result-mask', 'kernel-init-call-result-mask', 'later-call-result-mask'):
            self.assertEqual(self.create_path(name)['rejected_condition'],
                             '(EAX & 0xfff00000) == 0xbad00000')

    def test_create_each_real_key_and_terminator_drift_is_rejected(self):
        for key in self.create['keys']:
            raw = bytes.fromhex(key['bytes'])
            for offset in (0, len(raw)-1):
                pe = self.changed_pe(int(key['rva'], 16)+offset, bytes((raw[offset] ^ 1,)))
                with self.subTest(key=key['name'], offset=offset), self.assertRaisesRegex(ValueError, 'create key bytes'):
                    evidence.create_contract(pe, self.decoded)

    def test_create_every_table_byte_drift_is_rejected_without_decoding_data(self):
        for table in self.create['tables']:
            raw = bytes.fromhex(table['bytes'])
            for offset, value in enumerate(raw):
                pe = self.changed_pe(int(table['lo'], 16)+offset, bytes((value ^ 1,)))
                with self.subTest(table=table['name'], offset=offset), self.assertRaisesRegex(ValueError, 'create table bytes'):
                    evidence.create_contract(pe, self.decoded)

    def test_create_real_code_drift_defaults_selector_and_failure_gates(self):
        mutations = {
            0x17b5d: '33f6', 0x17b5f: '40387919', 0x17b78: '488b80c0010000',
            0x17b90: '488b4030', 0x17ba3: '764a', 0x17baf: '410fb68c0861810100',
            0x17bb8: '418b948844810100', 0x17bf4: '7335',
            0x17c46: '488b4058', 0x17c53: '41bf02000000', 0x17c5c: '410f44cf',
            0x17c84: '410f44cf', 0x17cac: '0f44cf', 0x17cd3: '0f44d7',
            0x17d07: '488b4060', 0x17d17: '0f44cf', 0x17d1a: '0fbaf11e',
            0x17d79: '752d', 0x17f29: 'e8d3900000', 0x17f30: '250000e0ff',
            0x17f7b: 'e8215c0000', 0x17f8c: '7435', 0x1801f: '0f85ac000000',
            0x18070: '488911', 0x180cc: 'bb02000000', 0x1813c: 'cc',
        }
        for rva, raw in mutations.items():
            pe = self.changed_pe(rva, bytes.fromhex(raw))
            code = dict(self.decoded)
            code['create-common-body'] = evidence.decode_window(
                pe, self.decoder, self.pe.OPTIONAL_HEADER.ImageBase, 0x17b20, 0x18140)
            with self.subTest(rva=hex(rva)), self.assertRaisesRegex(ValueError, 'create code bytes'):
                evidence.create_contract(pe, code)

    def test_create_selected_decode_text_and_code_boundary_drift(self):
        code = copy.deepcopy(self.decoded)
        row = next(row for row in code['create-common-body']['rows'] if row['rva'] == '0x17cac')
        row['mnemonic'] = 'cmove'
        with self.assertRaisesRegex(ValueError, 'create instruction'):
            evidence.create_contract(self.pe, code)
        for hi in (0x1813d, 0x18160, 0x181d1):
            code = copy.deepcopy(self.decoded)
            code['create-common-body']['hi'] = hex(hi)
            with self.subTest(hi=hex(hi)), self.assertRaisesRegex(ValueError, 'create decoded boundary'):
                evidence.create_contract(self.pe, code)

    def test_create_parent_pdata_is_unique_and_cannot_be_shortened_to_code(self):
        selected = next(row for row in self.pe.DIRECTORY_ENTRY_EXCEPTION if row.struct.BeginAddress == 0x17b20)
        others = [row for row in self.pe.DIRECTORY_ENTRY_EXCEPTION if row is not selected]
        entry_sets = [others, [*others, selected, selected]]
        for field, replacement in (('BeginAddress', 0x17b21), ('EndAddress', 0x18140),
                                   ('EndAddress', 0x181d2), ('UnwindData', 0xff7c0)):
            values = dict(BeginAddress=0x17b20, EndAddress=0x181d1, UnwindData=0xff7bc,
                          get_file_offset=selected.struct.get_file_offset)
            values[field] = replacement
            entry_sets.append([*others, SimpleNamespace(struct=SimpleNamespace(**values))])
        for case, entries in enumerate(entry_sets):
            with self.subTest(case=case), self.assertRaisesRegex(ValueError, 'create parent pdata/unwind'):
                evidence.create_contract(self.changed_pe(entries=entries), self.decoded)

    def test_create_raw_parent_and_unwind_byte_drift_are_rejected(self):
        function = self.create['functions'][0]
        for field, raw_field, message in (('pdata_rva', 'pdata_bytes', 'create pdata bytes'),
                                          ('unwind_rva', 'unwind_bytes', 'create unwind bytes')):
            raw = bytes.fromhex(function[raw_field])
            for offset, value in enumerate(raw):
                pe = self.changed_pe(int(function[field], 16)+offset, bytes((value ^ 1,)))
                with self.subTest(field=field, offset=offset), self.assertRaisesRegex(ValueError, message):
                    evidence.create_contract(pe, self.decoded)

    def test_create_saved_missing_or_deleted_contract_arrays_fail_closed(self):
        del self.document['internal_create']
        with self.assertRaisesRegex(ValueError, 'internal create contract'):
            self.verify()
        for array in evidence.CREATE_ARRAYS.values():
            self.document = copy.deepcopy(self.fixture)
            self.document['internal_create'][array].pop()
            with self.subTest(array=array), self.assertRaisesRegex(ValueError, 'internal create contract'):
                self.verify()

    def test_create_saved_defaults_tables_and_json_types_are_bound(self):
        changes = [('reads', 0, 'exact_success_eax', True), ('reads', 0, 'otherwise_value', True),
                   ('reads', 2, 'otherwise_value', False), ('reads', 4, 'postprocess', 'none'),
                   ('reads', 4, 'value_type', 'uint32'), ('tables', 0, 'byte_count', 32.0),
                   ('tables', 1, 'decoded_as_code', True), ('functions', 0, 'hi', '0x18140'),
                   ('functions', 0, 'unwind_bytes', '00'), ('keys', 0, 'bytes', '00'),
                   ('anchors', 0, 'bytes', '90'), ('paths', 0, 'normal_eax', '0xbad00001')]
        for array, index, field, value in changes:
            self.document = copy.deepcopy(self.fixture)
            self.document['internal_create'][array][index][field] = value
            with self.subTest(array=array, field=field), self.assertRaisesRegex(ValueError, 'internal create contract'):
                self.verify()
        self.document = copy.deepcopy(self.fixture)
        self.document['internal_create']['selector']['indexed_cases'][0]['index'] = False
        with self.assertRaisesRegex(ValueError, 'internal create contract'):
            self.verify()

    def test_create_saved_scope_and_extra_support_claims_are_rejected(self):
        changes = [('schema', True), ('abi_authenticated', 0), ('support', 'SUPPORTED'),
                   ('nested_calls_and_parameter_reads', 'no nested reads'),
                   ('object_interfaces', 'known architecture interface'),
                   ('scope', 'complete nested Create ABI')]
        changes += [(field, True) for field in (
            'target_executed', 'payload_loaded', 'gpu_tested', 'f7_selected_payload',
            'full_create_abi_authenticated', 'selector_acceptance_proves_support',
            'hardware_support_proven', 'gpu_completion_proven', 'runtime_failure_reachability_proven',
            'caller_gates_bypassed', 'no_nested_reads', 'patch_recommended')]
        for field, value in changes:
            self.document = copy.deepcopy(self.fixture)
            self.document['internal_create'][field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'internal create contract'):
                self.verify()

    def test_create_counts_use_real_arrays_and_preserve_legacy_stats_counts(self):
        for field in evidence.CREATE_ARRAYS:
            self.document[field] = 99999
        result = self.verify()
        for field, array in evidence.CREATE_ARRAYS.items():
            self.assertEqual(result[field], len(self.create[array]))
        self.assertEqual((result['create_code_bytes'], result['create_code_instructions'], result['create_data_bytes']),
                         (1568, 379, 145))
        self.assertEqual((result['instruction_assertions'], result['stats_instruction_assertions']), (110, 79))


    def test_rect_original_function_layout_and_partial_failure_semantics(self):
        rect = self.rect
        self.assertEqual(rect['code_span']['instructions'], 72)
        self.assertEqual(rect['code_span']['byte_count'], 242)
        self.assertEqual([len(rect[name]) for name in ('anchors', 'functions', 'paths')], [59, 1, 7])
        self.assertEqual(rect['input_layout']['requested_width_offset'], 16)
        self.assertEqual(rect['output_layout']['texture_width_offset'], 16)
        paths = {path['name']: path for path in rect['paths']}
        self.assertEqual(paths['query-result-zero-required']['exact_accepted_eax'], 0)
        self.assertIs(paths['texture-dimensions-before-validation']['failure_output_is_always_all_zero'], False)
        self.assertIs(paths['requested-rect-and-full-extent-default']['origin_clamped'], False)
        self.assertIs(paths['requested-rect-and-full-extent-default']['extent_clamped'], False)
        self.assertIs(paths['unsigned-subtraction-bounds']['failure_preserves_written_fields'], True)
        result = self.verify()
        self.assertEqual([result[name] for name in evidence.RECT_ARRAYS], [59, 1, 7])

    def test_rect_code_and_decode_semantic_drift_are_rejected(self):
        for rva, raw in ((0x17a86, '746d'), (0x17ab8, '440f44d8'), (0x17ada, '7319'),
                         (0x17ae1, '4103d2'), (0x17af1, 'b000')):
            with self.subTest(rva=hex(rva)):
                pe = self.changed_pe(rva, bytes.fromhex(raw))
                with self.assertRaisesRegex(ValueError, 'rect code bytes'):
                    evidence.rect_contract(pe, self.decoded)
        code = copy.deepcopy(self.decoded)
        row = next(row for row in code['descriptor-rect-helper']['rows'] if row['rva'] == '0x17ab8')
        row['mnemonic'] = 'cmove'
        with self.assertRaisesRegex(ValueError, 'rect instruction'):
            evidence.rect_contract(self.pe, code)

    def test_rect_parent_unwind_and_boundary_are_bound(self):
        original = next(e for e in self.pe.DIRECTORY_ENTRY_EXCEPTION if e.struct.BeginAddress == 0x17a20)
        others = [e for e in self.pe.DIRECTORY_ENTRY_EXCEPTION if e is not original]
        for entries in (others, [*others, original, original]):
            with self.assertRaisesRegex(ValueError, 'rect pdata/unwind'):
                evidence.rect_contract(self.changed_pe(entries=entries), self.decoded)
        function = self.rect['functions'][0]
        for field, raw_field, message in (('pdata_rva', 'pdata_bytes', 'rect pdata bytes'),
                                          ('unwind_rva', 'unwind_bytes', 'rect unwind bytes')):
            raw = bytes.fromhex(function[raw_field])
            for offset, byte in enumerate(raw):
                with self.subTest(field=field, offset=offset), self.assertRaisesRegex(ValueError, message):
                    evidence.rect_contract(self.changed_pe(int(function[field], 16)+offset,
                                                         bytes((byte ^ 1,))), self.decoded)
        code = copy.deepcopy(self.decoded)
        code['descriptor-rect-helper']['hi'] = '0x17b11'
        with self.assertRaisesRegex(ValueError, 'rect decoded boundary'):
            evidence.rect_contract(self.pe, code)

    def test_rect_saved_semantic_type_and_scope_tampering_fail_closed(self):
        for mutation in ('missing', 'clamped', 'zeroed', 'query-result', 'false-as-zero',
                         'unknown-interface', 'gpu-proof', 'extra-claim'):
            self.document = copy.deepcopy(self.fixture)
            rect = self.document['descriptor_rect']
            paths = {path['name']: path for path in rect['paths']}
            if mutation == 'missing':
                del self.document['descriptor_rect']
            elif mutation == 'clamped':
                paths['requested-rect-and-full-extent-default']['extent_clamped'] = True
            elif mutation == 'zeroed':
                paths['texture-dimensions-before-validation']['failure_output_is_always_all_zero'] = True
            elif mutation == 'query-result':
                paths['query-result-zero-required']['exact_accepted_eax'] = False
            elif mutation == 'false-as-zero':
                rect['gpu_tested'] = 0
            elif mutation == 'unknown-interface':
                rect['backend_interface'] = 'ID3D12Resource'
            elif mutation == 'gpu-proof':
                rect['gpu_completion_proven'] = True
            else:
                rect['complete_evaluate_contract_proven'] = True
            with self.subTest(mutation=mutation), self.assertRaisesRegex(ValueError, 'descriptor rect contract'):
                self.verify()


if __name__ == '__main__':
    unittest.main()
