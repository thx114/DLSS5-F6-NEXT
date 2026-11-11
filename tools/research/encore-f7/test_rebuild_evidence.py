import unittest

from rebuild_evidence import (public_vtable_slots, fg_resource_tag_contract,
                              nr_device_holder_contract, nr_shutdown_scope_contract,
                              ngx_shutdown_interposition_contract, public_loader_flags,
                              ngx_hook_transaction_contract)
from types import SimpleNamespace
from unittest.mock import patch

import rebuild_evidence


class HeaderSlotTests(unittest.TestCase):
    HEADER = '''typedef struct IFenceVtbl {
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( IFence * This );
        ULONG ( STDMETHODCALLTYPE *AddRef )( IFence * This );
        ULONG ( STDMETHODCALLTYPE *Release )( IFence * This );
        UINT64 ( STDMETHODCALLTYPE *Completed )( IFence * This );
    } IFenceVtbl;'''

    def test_observed_c_header_order_maps_to_win64_offsets(self):
        self.assertEqual(public_vtable_slots(self.HEADER, 'IFence', {'Completed': 24}),
                         [dict(method='Completed', slot=3, byte_offset='0x18')])

    def test_wrong_slot_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'slot changed'):
            public_vtable_slots(self.HEADER, 'IFence', {'Completed': 16})

    def test_duplicate_method_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'slot changed'):
            public_vtable_slots(self.HEADER.replace('*Release', '*Completed'), 'IFence', {'Completed': 24})

    def test_missing_interface_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'missing C vtable'):
            public_vtable_slots(self.HEADER, 'INotPresent', {'Completed': 24})

    def test_missing_closing_declaration_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'unterminated C vtable'):
            public_vtable_slots(self.HEADER.replace('} IFenceVtbl;', ''), 'IFence', {'Completed': 24})


class NrDeviceHolderContractTests(unittest.TestCase):
    def setUp(self):
        self.guid = bytes.fromhex('f1199818b61d574bbe541821339b85f7')
        self.records = [(0x92d0, 0x92f8, 0xd03a14), (0x151020, 0x15255e, 0xd19dc4),
                        (0x156ac0, 0x159a3f, 0xd197c4), (0x15dad0, 0x15db04, 0xd1a014),
                        (0x143430, 0x14369e, 0xd19570)]
        methods = ['QueryInterface', 'AddRef', 'Release', 'GetPrivateData',
                   'SetPrivateData', 'SetPrivateDataInterface', 'SetName', 'GetDevice']
        self.header = 'typedef struct ID3D12DeviceChildVtbl {'+''.join(
            'HRESULT ( STDMETHODCALLTYPE *'+name+' )( ID3D12DeviceChild * This );'
            for name in methods)+'} ID3D12DeviceChildVtbl;'

    def pe(self):
        return SimpleNamespace(get_data=lambda rva, size: self.guid[:size],
            parse_data_directories=lambda directories: None,
            DIRECTORY_ENTRY_EXCEPTION=[SimpleNamespace(struct=SimpleNamespace(
                BeginAddress=lo, EndAddress=hi, UnwindData=unwind)) for lo, hi, unwind in self.records])

    def test_cpu_holds_do_not_certify_retirement_or_shutdown(self):
        report = nr_device_holder_contract(self.pe(), self.header)
        self.assertEqual(len(report['function_records']), 5)
        self.assertEqual(report['feature_device_hold']['release_device_rva'], '0x15dafa')
        for name in ('target_executed', 'payload_executed', 'gpu_tested',
                     'resource_retirement_proven', 'module_lifetime_proven',
                     'shutdown_contract_proven', 'complete_session_lifetime_proven'):
            self.assertIs(report[name], False)

    def test_every_device_guid_byte_is_bound(self):
        original = self.guid
        for index in range(len(original)):
            self.guid = original[:index]+bytes([original[index] ^ 1])+original[index+1:]
            with self.subTest(index=index), self.assertRaisesRegex(ValueError, 'device GUID'):
                nr_device_holder_contract(self.pe(), self.header)

    def test_device_child_slot_drift_is_rejected(self):
        for name in ('AddRef', 'Release', 'GetDevice'):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'vtable slot'):
                nr_device_holder_contract(self.pe(), self.header.replace('*'+name, '*Changed'))

    def test_each_function_boundary_and_unwind_is_bound(self):
        original = self.records[:]
        for index, record in enumerate(original):
            for field in range(3):
                changed = list(record)
                changed[field] += 1
                self.records = original[:index]+[tuple(changed)]+original[index+1:]
                with self.subTest(index=index, field=field), self.assertRaisesRegex(ValueError, 'function boundary'):
                    nr_device_holder_contract(self.pe(), self.header)

    def test_duplicate_function_record_is_rejected(self):
        self.records.append(self.records[-1])
        with self.assertRaisesRegex(ValueError, 'function boundary'):
            nr_device_holder_contract(self.pe(), self.header)


class NrShutdownScopeContractTests(unittest.TestCase):
    def setUp(self):
        self.records = [(0x356f60, 0x35829a, 0xd3c130), (0x35a450, 0x35a4cc, 0xd3c84c),
                        (0x539bc0, 0x539bea, 0xd03020), (0x539bea, 0x539c0a, 0xd3c870),
                        (0x539c0a, 0x539c43, 0xd3c884), (0x153260, 0x155b5e, 0xd19ac4),
                        (0x1458f0, 0x145d10, 0xd19c90)]
        self.import_name = b'FreeLibrary'
        self.import_address = 0x18053b3f0
        self.cleanup_imports = [(0x53b668, b'HeapFree', b'KERNEL32.dll'),
                                (0x53b348, b'GetLastError', b'KERNEL32.dll')]
        self.records += [(0xc6c0, 0xc725, 0xd03004), (0xc7b0, 0xc818, 0xd03004),
                         (0x18200, 0x1826f, 0xd03004), (0x35ae70, 0x35ae89, 0xd03480),
                         (0x35ae89, 0x35aee8, 0xd3c7e4), (0x35aee8, 0x35af26, 0xd3c7f8),
                         (0x50b6c0, 0x50b6fc, 0xd59d34)]
        # Independent raw windows from the SHA-fixed DLL, never loaded/executed.
        self.data = {
            0xc6c0: bytes.fromhex(
                '40534883ec20488bd9488b49184883f90f7639488b03488d51014881fa00100000721e4c8b40f849'
                '2bc04883e8084883f81f7706488d5128eb0ab905000000cd294c8bc0498bc8e8e4af4d0048c74310'
                '0000000048c743180f000000c603004883c4205bc3'),
            0xc7b0: bytes.fromhex(
                '40534883ec20488b11488bd94885d27451488b4110482bc248c1f803488d04c500000000483d0010'
                '0000721e488b4af8482bd14883ea084883fa1f77064883c027eb0ab905000000cd29488bca488bd0'
                'e8ebae4d0033c048890348894308488943104883c4205bc3'),
            0x18200: bytes.fromhex(
                '40534883ec20488bd9488b49184883f9077641488b03488d144d020000004881fa0010000072224c'
                '8b40f8492bc04883e8084883f81f770a488d144d29000000eb0ab905000000cd294c8bc0498bc8e8'
                '9cf44c0033c048c743100000000066890348c74318070000004883c4205bc3'),
            0x35ae70: bytes.fromhex(
                '48895c2410564883ec20488b19488bf14885db0f849200000048897c2430488b7908483bdf741248'
                '8d4b18e82018cbff4883c348483bdf75ee4c8b0648b8398ee3388ee3380e488b4e10488b7c243049'
                '2bc848f7e948c1fa02488bc248c1e83f4803d0488d04d2488d14c5000000004881fa00100000721e'
                '498b48f84c2bc14983e8084983f81f77064883c227eb0ab905000000cd29498bc8e8e2c7180033c0'
                '4889064889460848894610488b5c24384883c4205ec3'),
            0x4e76f0: bytes.fromhex(
                'e9ab090000'),
            0x4e80a0: bytes.fromhex(
                'e93bfd0000'),
            0x4f7de0: bytes.fromhex(
                'e9db380100'),
            0x50b6c0: bytes.fromhex(
                '4885c97436534883ec204c8bc133d2488b0d5ab71501ff158cff020085c07516ff1562fc02008bc8'
                'e88fb5feff8bd8e8c8b6feff89184883c4205bc3'),
            0x539bc0: bytes.fromhex(
                '4883ec28488d0d95a41201e8e02badff488b0d79a41201488b410848c70000000000488b094885c9'
                '742048895c242090488b19ba20000000e8f3dafaff488bcb4885db75eb488b5c2420488b0d3fa412'
                '01ba20000000e8d5dafaff488d0d3ea31201e8d9e5adff488d0d1aa31201e83d12e2ff488d0d26a2'
                '12014883c428e9bde5adff'),
        }

    def get_data(self, rva, size):
        for lo, raw in self.data.items():
            if lo <= rva < lo+len(raw):
                return raw[rva-lo:rva-lo+size]
        return b''

    def assert_byte_drift_rejected(self, sites, message):
        original = self.data.copy()
        for rva, size in sites:
            lo = next(lo for lo, raw in original.items() if lo <= rva < lo+len(raw))
            raw = original[lo]
            for offset in range(size):
                self.data = original.copy()
                index = rva-lo+offset
                self.data[lo] = raw[:index]+bytes([raw[index] ^ 1])+raw[index+1:]
                with self.subTest(rva=hex(rva), byte=offset), self.assertRaisesRegex(ValueError, message):
                    nr_shutdown_scope_contract(self.pe(), 0x180000000)
        self.data = original

    def pe(self):
        return SimpleNamespace(get_data=self.get_data, parse_data_directories=lambda directories: None,
            DIRECTORY_ENTRY_IMPORT=[SimpleNamespace(dll=b'KERNEL32.dll', imports=[
                SimpleNamespace(address=self.import_address, name=self.import_name)])]+[
                SimpleNamespace(dll=dll, imports=[SimpleNamespace(address=0x180000000+rva, name=name)])
                for rva, name, dll in self.cleanup_imports],
            DIRECTORY_ENTRY_EXCEPTION=[SimpleNamespace(struct=SimpleNamespace(
                BeginAddress=lo, EndAddress=hi, UnwindData=unwind)) for lo, hi, unwind in self.records])

    def test_bounded_observations_do_not_claim_absent_dynamic_callers(self):
        report = nr_shutdown_scope_contract(self.pe(), 0x180000000)
        self.assertEqual(report['context_evaluate']['context_field_offset'], '0x20')
        self.assertEqual(report['context_evaluate']['api_slot_offset'], '0x10')
        for name in ('shutdown_call_identified', 'complete_dynamic_call_coverage_proven',
                     'transitive_cleanup_authenticated', 'complete_session_lifetime_proven',
                     'successful_session_unload_proven', 'resource_retirement_proven',
                     'target_executed', 'payload_executed', 'gpu_tested'):
            self.assertIs(report[name], False)

    def test_wrong_import_name_or_slot_is_rejected(self):
        self.import_name = b'LoadLibraryW'
        with self.assertRaisesRegex(ValueError, 'FreeLibrary import'):
            nr_shutdown_scope_contract(self.pe(), 0x180000000)
        self.import_name = b'FreeLibrary'
        self.import_address += 8
        with self.assertRaisesRegex(ValueError, 'FreeLibrary import'):
            nr_shutdown_scope_contract(self.pe(), 0x180000000)

    def test_each_cleanup_segment_is_required(self):
        original = self.records[:]
        for index in (2, 3, 4):
            self.records = original[:index]+original[index+1:]
            with self.subTest(index=index), self.assertRaisesRegex(ValueError, 'function boundary'):
                nr_shutdown_scope_contract(self.pe(), 0x180000000)

    def test_boundary_or_unwind_drift_is_rejected(self):
        original = self.records[:]
        for index, record in enumerate(original):
            for field in range(3):
                changed = list(record)
                changed[field] += 1
                self.records = original[:index]+[tuple(changed)]+original[index+1:]
                with self.subTest(index=index, field=field), self.assertRaisesRegex(ValueError, 'function boundary'):
                    nr_shutdown_scope_contract(self.pe(), 0x180000000)

    def test_cleanup_structure_keeps_normal_heap_and_unknown_failure_paths_separate(self):
        report = nr_shutdown_scope_contract(self.pe(), 0x180000000)
        cleanup = report['bounded_cleanup']
        self.assertEqual(len(report['function_records']), 14)
        self.assertEqual(len(report['imports']), 3)
        self.assertEqual(len(cleanup['windows']), 8)
        self.assertEqual(len(cleanup['direct_edges']), 13)
        self.assertEqual(len(cleanup['leaf_thunks']), 3)
        self.assertEqual(len(cleanup['import_calls']), 2)
        config = next(row for row in cleanup['windows'] if row['name'] == 'nr-cleanup-config-records')
        self.assertEqual([(row['begin'], row['end'], row['unwind']) for row in config['function_records']],
                         [('0x35ae70', '0x35ae89', '0xd03480'),
                          ('0x35ae89', '0x35aee8', '0xd3c7e4'),
                          ('0x35aee8', '0x35af26', '0xd3c7f8')])
        layouts = cleanup['container_layouts']
        self.assertEqual(layouts['config_records']['record_stride'], '0x48')
        self.assertEqual(layouts['config_records']['inner_narrow_offset'], '0x18')
        self.assertEqual(layouts['pointer_buffer']['element_stride'], '0x8')
        self.assertEqual(layouts['narrow_buffer']['inline_capacity'], 15)
        self.assertEqual(layouts['wide_buffer']['inline_capacity'], 7)
        self.assertEqual(layouts['wide_buffer']['unit_bytes'], 2)
        self.assertEqual(cleanup['normal_heap_path']['nonzero_target_rva'], '0x50b6f6')
        self.assertFalse(cleanup['normal_heap_path']['runtime_success_proven'])
        self.assertFalse(cleanup['failure_path']['helpers_fully_recovered'])
        self.assertEqual([row['target_rva'] for row in cleanup['unknown_failure_helpers']],
                         ['0x4f6c7c', '0x4f6dbc'])
        for row in cleanup['unknown_failure_helpers']:
            self.assertEqual(row['status'], 'UNKNOWN')
            self.assertFalse(row['body_decoded'])
            self.assertFalse(any(lo <= int(row['target_rva'], 16) < hi
                                 for lo, hi in rebuild_evidence.RUNTIME_RANGES.values()))

    def test_all_direct_edge_bytes_including_failure_helper_calls_are_bound(self):
        self.assert_byte_drift_rejected([(rva, 5) for rva in (
            0x539bcb, 0x539bf8, 0x539c16, 0x539c22, 0x539c2e, 0x539c3e,
            0x35ae9b, 0x35af09, 0xc707, 0xc800, 0x1824f, 0x50b6e8, 0x50b6ef)], 'direct edge bytes')

    def test_all_five_leaf_thunk_bytes_are_bound(self):
        self.assert_byte_drift_rejected([(rva, 5) for rva in (0x4e76f0, 0x4e80a0, 0x4f7de0)],
                                        'leaf thunk bytes')

    def test_leaf_thunks_must_have_no_overlapping_pdata(self):
        original = self.records[:]
        for rva in (0x4e76f0, 0x4e80a0, 0x4f7de0):
            for lo, hi in ((rva, rva+5), (rva-1, rva+1), (rva+4, rva+6)):
                self.records = original+[(lo, hi, 0xd03004)]
                with self.subTest(rva=hex(rva), lo=lo), self.assertRaisesRegex(ValueError, 'covered by .pdata'):
                    nr_shutdown_scope_contract(self.pe(), 0x180000000)

    def test_import_instruction_bytes_are_bound_to_the_iat_slots(self):
        self.assert_byte_drift_rejected([(0x50b6d6, 6), (0x50b6e0, 6)], 'import call bytes')

    def test_cleanup_import_names_slots_modules_and_presence_are_bound(self):
        original = self.cleanup_imports[:]
        for index, (rva, name, dll) in enumerate(original):
            for changed in ((rva+8, name, dll), (rva, b'OtherApi', dll), (rva, None, dll),
                            (rva, name, b'OTHER.dll'), None):
                self.cleanup_imports = original.copy()
                if changed is None:
                    del self.cleanup_imports[index]
                else:
                    self.cleanup_imports[index] = changed
                with self.subTest(index=index, changed=changed), self.assertRaisesRegex(ValueError, 'cleanup import changed'):
                    nr_shutdown_scope_contract(self.pe(), 0x180000000)

    def test_each_new_helper_pdata_segment_is_required_once(self):
        original = self.records[:]
        for index in range(7, len(original)):
            for records in (original[:index]+original[index+1:], original+[original[index]]):
                self.records = records
                with self.subTest(index=index, length=len(records)), self.assertRaisesRegex(ValueError, 'function boundary'):
                    nr_shutdown_scope_contract(self.pe(), 0x180000000)

    def test_declared_complete_cleanup_window_boundaries_are_bound(self):
        report = nr_shutdown_scope_contract(self.pe(), 0x180000000)
        for window in report['bounded_cleanup']['windows']:
            name = window['name']
            lo, hi = rebuild_evidence.RUNTIME_RANGES[name]
            for changed in ((lo+1, hi), (lo, hi-1), (lo, hi+1), None):
                with patch.dict(rebuild_evidence.RUNTIME_RANGES):
                    if changed is None:
                        del rebuild_evidence.RUNTIME_RANGES[name]
                    else:
                        rebuild_evidence.RUNTIME_RANGES[name] = changed
                    with self.subTest(name=name, changed=changed), self.assertRaisesRegex(ValueError, 'window boundary'):
                        nr_shutdown_scope_contract(self.pe(), 0x180000000)

    def test_every_reported_field_stride_and_heap_control_instruction_byte_is_bound(self):
        report = nr_shutdown_scope_contract(self.pe(), 0x180000000)
        sites = [(int(row['rva'], 16), len(bytes.fromhex(row['bytes'])))
                 for row in report['bounded_cleanup']['instruction_bindings']]
        self.assert_byte_drift_rejected(sites, 'instruction bytes')

    def test_short_instruction_reads_are_rejected(self):
        self.data[0x50b6c0] = self.data[0x50b6c0][:-1]
        with self.assertRaisesRegex(ValueError, 'instruction bytes'):
            nr_shutdown_scope_contract(self.pe(), 0x180000000)

    def test_verify_counts_use_actual_report_arrays(self):
        from verify import nr_shutdown_cleanup_counts
        report = nr_shutdown_scope_contract(self.pe(), 0x180000000)
        counts = nr_shutdown_cleanup_counts(report)
        for field, array in rebuild_evidence.NR_SHUTDOWN_CLEANUP_ARRAYS.items():
            self.assertEqual(counts[field], len(report['bounded_cleanup'][array]))
            report['bounded_cleanup'][array].append({})
            self.assertEqual(nr_shutdown_cleanup_counts(report)[field], counts[field]+1)

    def test_verify_rejects_missing_or_non_array_cleanup_counts(self):
        from verify import nr_shutdown_cleanup_counts
        with self.assertRaisesRegex(ValueError, 'missing NR shutdown'):
            nr_shutdown_cleanup_counts({})
        for array in rebuild_evidence.NR_SHUTDOWN_CLEANUP_ARRAYS.values():
            for invalid in (None, {}, 'rows', 1):
                report = nr_shutdown_scope_contract(self.pe(), 0x180000000)
                report['bounded_cleanup'][array] = invalid
                with self.subTest(array=array, invalid=invalid), self.assertRaisesRegex(ValueError, 'invalid NR shutdown'):
                    nr_shutdown_cleanup_counts(report)


class NgxShutdownInterpositionTests(unittest.TestCase):
    def setUp(self):
        self.data = {0x55b208: b'NVSDK_NGX_D3D12_Shutdown1\0',
                     0x559c10: ('shutdown api=D3D12 device=%p result=0x%08X registryInvalidated=%u\0').encode('utf-16-le'),
                     0x55b560: ('[NR NGX target] index=%zu export=%S entry=%p core-rva=0x%llX\0').encode('utf-16-le')}
        self.imports = [(0x53b3a8, b'GetProcAddress'), (0x53b560, b'AcquireSRWLockExclusive'),
                        (0x53b550, b'ReleaseSRWLockExclusive')]
        self.records = [(0x932f0, 0x939d7, 0xd0c564), (0xffa00, 0xffa4f, 0xd0300c),
                        (0xff8f0, 0xff9bd, 0xd0af84), (0x109dd0, 0x10b465, 0xd0c814)]
        self.section = SimpleNamespace(Name=b'.data\0\0\0', VirtualAddress=0xd66000,
            Misc_VirtualSize=0x900e74, SizeOfRawData=0x22200, Characteristics=0xc0000040)
        self.sections = [self.section]

    def pe(self):
        return SimpleNamespace(get_data=lambda rva, size: self.data[rva][:size],
            parse_data_directories=lambda directories: None, sections=self.sections,
            DIRECTORY_ENTRY_IMPORT=[SimpleNamespace(dll=b'KERNEL32.dll', imports=[
                SimpleNamespace(address=0x180000000+rva, name=name) for rva, name in self.imports])],
            DIRECTORY_ENTRY_EXCEPTION=[SimpleNamespace(struct=SimpleNamespace(
                BeginAddress=lo, EndAddress=hi, UnwindData=unwind)) for lo, hi, unwind in self.records])

    def test_forwarding_slot_is_not_an_nr_shutdown_or_gpu_certificate(self):
        report = ngx_shutdown_interposition_contract(self.pe(), 0x180000000)
        self.assertNotEqual(report['forwarding_slot']['rva'], report['direct_nr_shutdown_slot_rva'])
        self.assertIsNone(report['forwarding_slot']['raw_pointer_bytes'])
        self.assertEqual(report['forwarding_slot']['runtime_value'], 'unknown')
        self.assertFalse(report['target_diagnostic']['is_installation_proof'])
        self.assertFalse(report['transaction_caller']['helper_abi_authenticated'])
        self.assertFalse(report['clear_writer']['proves_runtime_zero_value'])
        self.assertFalse(report['clear_writer']['is_gpu_retirement'])
        for name in ('target_executed', 'payload_executed', 'gpu_tested',
                     'original_pointer_runtime_target_authenticated', 'installed_trampoline_authenticated',
                     'nr_payload_shutdown_caller_proven', 'shutdown_abi_authenticated',
                     'transitive_postprocess_effects_authenticated', 'resource_retirement_proven',
                     'registry_invalidation_is_gpu_retirement'):
            self.assertIs(report[name], False)

    def test_ascii_and_wide_literals_include_every_byte_and_terminator(self):
        original = self.data.copy()
        for rva, value in original.items():
            for index in range(len(value)):
                self.data = original.copy()
                self.data[rva] = value[:index]+bytes([value[index] ^ 1])+value[index+1:]
                with self.subTest(rva=hex(rva), index=index), self.assertRaisesRegex(ValueError, 'literal changed'):
                    ngx_shutdown_interposition_contract(self.pe(), 0x180000000)

    def test_each_import_name_and_address_is_bound(self):
        original = self.imports[:]
        for index, (rva, name) in enumerate(original):
            for changed in ((rva+8, name), (rva, b'OtherApi')):
                self.imports = original[:index]+[changed]+original[index+1:]
                with self.subTest(index=index, changed=changed), self.assertRaisesRegex(ValueError, 'import changed'):
                    ngx_shutdown_interposition_contract(self.pe(), 0x180000000)

    def test_all_function_records_are_exact(self):
        original = self.records[:]
        for index, record in enumerate(original):
            for field in range(3):
                changed = list(record)
                changed[field] += 1
                self.records = original[:index]+[tuple(changed)]+original[index+1:]
                with self.subTest(index=index, field=field), self.assertRaisesRegex(ValueError, 'function boundary'):
                    ngx_shutdown_interposition_contract(self.pe(), 0x180000000)

    def test_virtual_tail_is_not_fabricated_as_raw_pointer_bytes(self):
        self.sections = []
        with self.assertRaisesRegex(ValueError, 'pointer section changed'):
            ngx_shutdown_interposition_contract(self.pe(), 0x180000000)
        self.sections = [self.section, self.section]
        with self.assertRaisesRegex(ValueError, 'pointer section changed'):
            ngx_shutdown_interposition_contract(self.pe(), 0x180000000)
        self.sections = [self.section]
        self.section.SizeOfRawData = self.section.Misc_VirtualSize
        with self.assertRaisesRegex(ValueError, 'section layout changed'):
            ngx_shutdown_interposition_contract(self.pe(), 0x180000000)


class HookTransactionContractTests(unittest.TestCase):
    HEADER = '''#define GET_MODULE_HANDLE_EX_FLAG_PIN (0x00000001)
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT (0x00000002)
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS (0x00000004)
'''

    def setUp(self):
        self.data = {rva: (name+'\0').encode('ascii') for rva, name in (
            (0x540488, 'refused-context'), (0x540508, 'begin'), (0x540518, 'duplicate-target'),
            (0x540510, 'target'), (0x540530, 'replacement-owner'), (0x540548, 'precondition'),
            (0x540558, 'attach'), (0x540560, 'commit'), (0x540498, 'snapshot'),
            (0x5404c8, 'enumerate-thread'), (0x5404b8, 'open-thread'),
            (0x5404e0, 'update-thread'), (0x5404f0, 'update-exited-thread'))}
        for rva, iat in ((0x4de981, 0x53b3d0), (0x4de9a5, 0x53b498), (0x4de96f, 0x53b348),
                         (0x4de98d, 0x53b3f8), (0x4de993, 0x53b400), (0x4de999, 0x53b408)):
            self.data[rva] = b'\xff\x25'+(iat-rva-6).to_bytes(4, 'little', signed=True)
        self.imports = [(0x53b3a0, b'GetModuleHandleExW'), (0x53b340, b'CloseHandle'),
            (0x53b348, b'GetLastError'), (0x53b3d0, b'GetCurrentThreadId'), (0x53b498, b'VirtualProtect'),
            (0x53b3f8, b'CreateToolhelp32Snapshot'), (0x53b400, b'Thread32First'),
            (0x53b408, b'Thread32Next'), (0x53b3d8, b'OpenThread'),
            (0x53b3c0, b'GetCurrentProcessId'), (0x53b3c8, b'GetCurrentThread'),
            (0x53b360, b'WaitForSingleObject')]
        self.records = [(0x196a0, 0x19ac6, 0xd04ea0), (0x425320, 0x425390, 0xd030c8),
            (0x424870, 0x4248c6, 0xd030c8), (0x4251d0, 0x425315, 0xd07d38),
            (0x425390, 0x425827, 0xd4a0f4), (0x424f50, 0x424f72, 0xd4a124),
            (0x4248d0, 0x424901, 0xd4a10c), (0x1b700, 0x1b95d, 0xd04e54)]

    def pe(self):
        return SimpleNamespace(get_data=lambda rva, size: self.data[rva][:size],
            parse_data_directories=lambda directories: None,
            DIRECTORY_ENTRY_IMPORT=[SimpleNamespace(dll=b'KERNEL32.dll', imports=[
                SimpleNamespace(address=0x180000000+rva, name=name) for rva, name in self.imports])],
            DIRECTORY_ENTRY_EXCEPTION=[SimpleNamespace(struct=SimpleNamespace(
                BeginAddress=lo, EndAddress=hi, UnwindData=unwind)) for lo, hi, unwind in self.records])

    def test_conditional_owner_mutation_and_cpu_wait_are_not_clean_retry_or_gpu_proof(self):
        report = ngx_hook_transaction_contract(self.pe(), 0x180000000, self.HEADER)
        self.assertTrue(report['begin_failure']['may_follow_cpu_owner_acquisition'])
        self.assertFalse(report['begin_failure']['no_state_mutation_or_clean_retry_guaranteed'])
        self.assertFalse(report['begin_failure']['runtime_failure_reproduced'])
        self.assertFalse(report['begin_failure']['outer_nonzero_path_calls_abort'])
        self.assertFalse(report['cpu_owner']['is_gpu_fence'])
        pin = report['wrapper_module_pin_attempt']
        self.assertEqual(pin['flags'], 1 | 4)
        self.assertNotIn('GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT', pin['flag_names'])
        self.assertFalse(pin['actual_module_pin_proven'])
        self.assertFalse(pin['nr_payload_module_identity_proven'])
        self.assertFalse(report['thread_admission']['wait0_clears_update_error'])
        self.assertFalse(report['thread_admission']['exit_flag_is_gpu_completion'])
        for name in ('target_executed', 'payload_executed', 'gpu_tested', 'private_helper_abi_authenticated',
                     'backend_helpers_fully_recovered', 'complete_thread_coverage_proven',
                     'exception_cleanup_proven', 'runtime_installation_proven', 'complete_rollback_proven',
                     'resource_retirement_proven', 'safe_unload_proven'):
            self.assertIs(report[name], False)

    def test_flag_drift_missing_or_duplicate_definition_is_rejected(self):
        self.assertEqual(public_loader_flags(self.HEADER)['GET_MODULE_HANDLE_EX_FLAG_PIN'], 1)
        for line in self.HEADER.splitlines():
            if not line:
                continue
            for source in (self.HEADER.replace(line, ''), self.HEADER+'\n'+line,
                           self.HEADER.replace(line, line.replace(')', ' | 0x08)'))):
                with self.subTest(line=line, source=source), self.assertRaisesRegex(ValueError, 'flag changed'):
                    public_loader_flags(source)

    def test_all_stage_and_thunk_bytes_are_bound(self):
        original = self.data.copy()
        for rva, value in original.items():
            for index in range(len(value)):
                self.data = original.copy()
                self.data[rva] = value[:index]+bytes([value[index] ^ 1])+value[index+1:]
                with self.subTest(rva=hex(rva), index=index), self.assertRaisesRegex(ValueError, 'stage changed|thunk changed'):
                    ngx_hook_transaction_contract(self.pe(), 0x180000000, self.HEADER)

    def test_all_import_names_and_slots_are_bound(self):
        original = self.imports[:]
        for index, (rva, name) in enumerate(original):
            for changed in ((rva+8, name), (rva, b'OtherApi')):
                self.imports = original[:index]+[changed]+original[index+1:]
                with self.subTest(index=index), self.assertRaisesRegex(ValueError, 'import changed'):
                    ngx_hook_transaction_contract(self.pe(), 0x180000000, self.HEADER)

    def test_all_function_boundaries_and_unwind_are_bound(self):
        original = self.records[:]
        for index, record in enumerate(original):
            for field in range(3):
                changed = list(record)
                changed[field] += 1
                self.records = original[:index]+[tuple(changed)]+original[index+1:]
                with self.subTest(index=index, field=field), self.assertRaisesRegex(ValueError, 'function boundary'):
                    ngx_hook_transaction_contract(self.pe(), 0x180000000, self.HEADER)


class FgResourceContractTests(unittest.TestCase):
    def setUp(self):
        self.data = {
            0x53c870: bytes.fromhex('cf709d3a1824724b839113f8721c7261'),
            0x53c880: bytes.fromhex('ad5a6a4c45b46c4987ff1af3845be653'),
            0x53ce20: bytes.fromhex('f1199818b61d574bbe541821339b85f7'),
            0x53ce30: bytes.fromhex('0f0d165b1bac85418ba8b3ae42a5a455'),
            0x53d0f8: b'slSetTagForFrame\0',
        }
        methods = ['QueryInterface', 'AddRef', 'Release', 'GetPrivateData', 'SetPrivateData',
                   'SetPrivateDataInterface', 'SetName', 'GetDevice', 'Map', 'Unmap', 'GetDesc']
        self.headers = {
            'd3d12.h': 'typedef struct ID3D12ResourceVtbl {'+''.join(
                'HRESULT ( STDMETHODCALLTYPE *'+name+' )( ID3D12Resource * This );' for name in methods
            )+'} ID3D12ResourceVtbl;',
            'sl_core_types.h': '''
                constexpr BufferType kBufferTypeDepth = 0;
                constexpr BufferType kBufferTypeMotionVectors = 1;
                constexpr BufferType kBufferTypeHUDLessColor = 2;
                enum ResourceLifecycle { eOnlyValidNow, eValidUntilPresent, eValidUntilEvaluate };
            ''',
            'ffx_framegeneration.h': '''
                #define FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION 0x00020002u
                #define FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE 0x00020004u
            ''',
        }
        class Pe:
            def get_data(_, rva, size):
                return self.data[rva][:size]
        self.pe = Pe()

    def test_static_identity_is_separate_from_runtime_proof(self):
        report = fg_resource_tag_contract(self.pe, 0x180000000, self.headers)
        self.assertEqual(report['lifecycle']['eOnlyValidNow'], 0)
        self.assertEqual(report['tags']['kBufferTypeMotionVectors'], 1)
        self.assertEqual(report['exact_key_slot']['count'], 8)
        for key in ('target_executed', 'gpu_tested', 'source_consumption_proven',
                    'resource_retirement_proven', 'public_abi_compiled'):
            self.assertIs(report[key], False)

    def test_each_guid_single_byte_change_is_rejected(self):
        for rva in (0x53c870, 0x53c880, 0x53ce20, 0x53ce30):
            original = self.data[rva]
            self.data[rva] = bytes([original[0] ^ 1])+original[1:]
            with self.subTest(rva=hex(rva)), self.assertRaisesRegex(ValueError, 'GUID mismatch'):
                fg_resource_tag_contract(self.pe, 0x180000000, self.headers)
            self.data[rva] = original

    def test_tag_export_terminator_is_required(self):
        self.data[0x53d0f8] = b'slSetTagForFrame!'
        with self.assertRaisesRegex(ValueError, 'export mismatch'):
            fg_resource_tag_contract(self.pe, 0x180000000, self.headers)

    def test_lifecycle_reordering_and_explicit_values_are_rejected(self):
        original = self.headers['sl_core_types.h']
        for replacement in ('eValidUntilPresent, eOnlyValidNow', 'eOnlyValidNow = 1, eValidUntilPresent'):
            self.headers['sl_core_types.h'] = original.replace('eOnlyValidNow, eValidUntilPresent', replacement)
            with self.subTest(replacement=replacement), self.assertRaisesRegex(ValueError, 'lifecycle order'):
                fg_resource_tag_contract(self.pe, 0x180000000, self.headers)

    def test_descriptor_and_tag_numeric_drift_are_rejected(self):
        for file, old, new, error in (
                ('ffx_framegeneration.h', '0x00020004u', '0x00020005u', 'descriptor type'),
                ('sl_core_types.h', 'kBufferTypeDepth = 0', 'kBufferTypeDepth = 9', 'tag value')):
            original = self.headers[file]
            self.headers[file] = original.replace(old, new)
            with self.subTest(file=file), self.assertRaisesRegex(ValueError, error):
                fg_resource_tag_contract(self.pe, 0x180000000, self.headers)
            self.headers[file] = original

    def test_public_resource_slot_drift_is_rejected(self):
        self.headers['d3d12.h'] = self.headers['d3d12.h'].replace('*Map', '*GetDesc')
        with self.assertRaisesRegex(ValueError, 'slot changed'):
            fg_resource_tag_contract(self.pe, 0x180000000, self.headers)


class FgInputFenceContractTests(unittest.TestCase):
    def setUp(self):
        # Raw fixture extracted from the SHA-fixed original PE, statically read only.
        # This synthetic PE tests drift rejection; it does not certify the original.
        self.data = {
            0x141ae: bytes.fromhex('4533ff4c897db0c745b8e1c88accc745bc79a1f544c745c097fae741c745c412f9bc6148c745c8040000004c897dd04c897dd84c897de06644897de84c897df04c897df844887d004c897d904c897d884c897d984c897da0488d4d90ff15407152004533c0488d55b0488d0db21ed500ff1584ced800448be0488d4d88ff151f715200418bdf44387e180f85b900000041389c36860500000f84ab0000004585e40f85a2000000488b7df04885ff0f84950000004c8b7df84d85ff0f8485000000488b07488bcfff5040493bc773774533c94533c033d233c9ff15db705200488bd84885c0742b4c8b0f4c8bc0498bd7488bcf41ff514885c07817ba88130000488bcbff15a970520085c0750540b701eb084032ff4885db7409488bcbff156f705200bb010000004084ff7519488d15e69c5200488bcee8b6abffffb806007a88e9ed0700004533ff'),
            0x14d1d: bytes.fromhex('488d0d7c7b5200ffd748897528488bf84885c00f84110200004c8d4528488bd0b906000000ff155866520085c00f84f701000048395d280f85ed010000488d0df778520048893d40c3d800'),
            0x1667c6c: bytes.fromhex('d0400100044b01000042d000'),
            0x1667c84: bytes.fromhex('204c0100804f0100cc3cd000'),
            0xd04200: bytes.fromhex('19330a0022016b060df00be009d007c00570046003300250f07b4e00'),
            0xd03ccc: bytes.fromhex('010f06000f340a000f32087007600650'),
            0x53c8a0: bytes.fromhex('736c444c535347476574537461746500'),
        }
        self.records = [(0x140d0, 0x14b04, 0xd04200), (0x14c20, 0x14f80, 0xd03ccc)]
        self.imports = [(0x53b368, b'CreateEventW'), (0x53b360, b'WaitForSingleObject'),
                        (0x53b340, b'CloseHandle'), (0x53b3a0, b'GetModuleHandleExW')]
        methods = ['QueryInterface', 'AddRef', 'Release', 'GetPrivateData', 'SetPrivateData',
                   'SetPrivateDataInterface', 'SetName', 'GetDevice', 'GetCompletedValue',
                   'SetEventOnCompletion', 'Signal']
        self.header = 'typedef struct ID3D12FenceVtbl {'+''.join(
            'HRESULT ( STDMETHODCALLTYPE *'+name+' )( ID3D12Fence * This );' for name in methods
        )+'} ID3D12FenceVtbl;'

    def get_data(self, rva, size):
        for lo, raw in self.data.items():
            if lo <= rva < lo+len(raw):
                return raw[rva-lo:rva-lo+size]
        return b''

    def pe(self):
        return SimpleNamespace(get_data=self.get_data, parse_data_directories=lambda directories: None,
            DIRECTORY_ENTRY_IMPORT=[SimpleNamespace(dll=b'KERNEL32.dll', imports=[
                SimpleNamespace(address=0x180000000+rva, name=name) for rva, name in self.imports])],
            DIRECTORY_ENTRY_EXCEPTION=[SimpleNamespace(struct=SimpleNamespace(
                BeginAddress=lo, EndAddress=hi, UnwindData=unwind)) for lo, hi, unwind in self.records])

    def contract(self):
        return rebuild_evidence.fg_input_fence_contract(self.pe(), 0x180000000, self.header)

    def test_static_paths_preserve_sentinel_and_no_requery_limits(self):
        report = self.contract()
        paths = {row['name']: row for row in report['paths']}
        self.assertTrue(paths['completed_value']['uint64_max_passes_compare'])
        self.assertFalse(paths['completed_value']['sentinel_guard_in_query_window'])
        self.assertFalse(paths['wait_success_tail']['fence_requery_present'])
        self.assertEqual(paths['wait']['accepted_result'], 0)
        self.assertEqual(paths['wait']['timeout_ms'], 5000)
        self.assertIn('not classified', paths['wait']['other_result'])
        self.assertFalse(paths['close_nonnull_event']['result_checked'])
        self.assertEqual(report['state_resolver']['observed_slot_rva'], '0xda10a8')
        self.assertEqual(report['state_resolver']['bounded_end'], '0x14d68')
        self.assertEqual(report['state_layout']['pointer_offset'], '0x40')
        self.assertEqual(report['state_layout']['target_offset'], '0x48')
        for key in ('target_executed', 'gpu_tested', 'cpu_stall_measured', 'f7_causality_proven',
                    'resource_retirement_proven', 'safe_unload_proven'):
            self.assertIs(report[key], False)
        self.assertFalse(report['state_resolver']['dynamic_callee_safety_proven'])
        self.assertFalse(report['state_resolver']['module_pinned'])

    def test_every_instruction_byte_and_exact_tail_is_bound(self):
        original = self.data.copy()
        for lo in (0x141ae, 0x14d1d):
            raw = original[lo]
            for index in range(len(raw)):
                self.data = original.copy()
                self.data[lo] = raw[:index]+bytes([raw[index] ^ 1])+raw[index+1:]
                with self.subTest(rva=hex(lo+index)), self.assertRaisesRegex(ValueError, 'instruction bytes'):
                    self.contract()
        self.data = original

    def test_no_query_guard_or_postwait_query_can_be_inserted(self):
        original = self.data[0x141ae]
        for rva, replacement in ((0x14278, b'\x48\x83\xf8\xff'),
                                  (0x142c8, b'\xff\x50\x40')):
            index = rva-0x141ae
            self.data[0x141ae] = original[:index]+replacement+original[index+len(replacement):]
            with self.subTest(rva=hex(rva)), self.assertRaisesRegex(ValueError, 'instruction bytes'):
                self.contract()

    def test_raw_function_records_unwind_and_name_cannot_drift(self):
        original = self.data.copy()
        for rva in (0x1667c6c, 0x1667c84, 0xd04200, 0xd03ccc, 0x53c8a0):
            raw = original[rva]
            for index in range(len(raw)):
                self.data = original.copy()
                self.data[rva] = raw[:index]+bytes([raw[index] ^ 1])+raw[index+1:]
                with self.subTest(rva=hex(rva), index=index), self.assertRaisesRegex(ValueError, 'bytes changed'):
                    self.contract()

    def test_function_boundary_unwind_missing_and_duplicates_rejected(self):
        original = self.records[:]
        for index, record in enumerate(original):
            for field in range(3):
                changed = list(record); changed[field] += 1
                self.records = original[:index]+[tuple(changed)]+original[index+1:]
                with self.subTest(index=index, field=field), self.assertRaisesRegex(ValueError, 'function boundary'):
                    self.contract()
        for changed in (original[:1], original+[original[0]]):
            self.records = changed
            with self.assertRaisesRegex(ValueError, 'function boundary'):
                self.contract()

    def test_each_import_name_address_and_dll_is_bound(self):
        original = self.imports[:]
        for index, (rva, name) in enumerate(original):
            for changed in ((rva+8, name), (rva, b'CreateEventA')):
                self.imports = original[:index]+[changed]+original[index+1:]
                with self.subTest(index=index, changed=changed), self.assertRaisesRegex(ValueError, 'import changed'):
                    self.contract()
        self.imports = original
        pe = self.pe(); pe.DIRECTORY_ENTRY_IMPORT[0].dll = b'Other.dll'
        with self.assertRaisesRegex(ValueError, 'import changed'):
            rebuild_evidence.fg_input_fence_contract(pe, 0x180000000, self.header)

    def test_fence_slots_are_read_from_supplied_sdk_header(self):
        original = self.header
        for name in ('GetCompletedValue', 'SetEventOnCompletion'):
            self.header = original.replace('*'+name, '*Other')
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'slot changed'):
                self.contract()
        self.header = original.replace('*GetDevice', '*SetEventOnCompletion')
        with self.assertRaisesRegex(ValueError, 'slot changed'):
            self.contract()

    def test_no_duplicate_window_or_legacy_anchors_added(self):
        self.assertEqual(rebuild_evidence.RUNTIME_RANGES['fg-post-present'], (0x140d0, 0x14b04))
        self.assertEqual(len(rebuild_evidence.RUNTIME_ANCHORS), 573)
        self.assertNotIn('fg-input-fence', rebuild_evidence.RUNTIME_RANGES)
        self.assertEqual(len(self.contract()['function_records']), 2)


if __name__ == '__main__':
    unittest.main()
