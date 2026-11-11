"""Real filesystem regressions for preserving verification source evidence."""
from contextlib import redirect_stderr
import argparse
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import verify


class VerifyInputProtectionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='encore-verify-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.out = self.root/'evidence'
        self.input = self.root/'source.log'
        self.input.write_bytes(b'original source bytes\n')

    def assert_rejected_before_work(self, arguments, *, expected_error=None, root=None):
        root = self.root if root is None else root
        before = {str(path.relative_to(root)): path.read_bytes()
                  for path in root.rglob('*') if path.is_file()}
        stderr = io.StringIO()
        with patch.object(verify, 'ROOT', root), \
                patch.object(verify.subprocess, 'run') as subprocess_run, \
                patch.object(verify.unittest.defaultTestLoader, 'discover') as discover, \
                redirect_stderr(stderr), self.assertRaises(SystemExit) as error:
            verify.main(arguments)
        self.assertEqual(error.exception.code, 2)
        if expected_error is not None:
            self.assertIn(expected_error, stderr.getvalue())
        subprocess_run.assert_not_called()
        discover.assert_not_called()
        after = {str(path.relative_to(root)): path.read_bytes()
                 for path in root.rglob('*') if path.is_file()}
        self.assertEqual(after, before)

    def test_log_in_output_tree_is_rejected_before_cpu_log_write(self):
        self.out.mkdir()
        source = self.out/'cpu-tests.txt'
        source.write_bytes(b'original log\n')
        self.assert_rejected_before_work(['--sr-log', str(source)])

    def test_receipt_in_output_tree_is_rejected_before_any_report_write(self):
        self.out.mkdir()
        receipt = self.out/'validation.json'
        receipt.write_bytes(b'original receipt\n')
        self.assert_rejected_before_work(['--sr-log', str(self.input),
                                          '--sr-session-receipt', str(receipt)])

    def test_log_hardlink_alias_of_generated_report_is_rejected(self):
        self.out.mkdir()
        os.link(self.input, self.out/'f7-sdk-input-log.json')
        self.assert_rejected_before_work(['--sr-log', str(self.input)])

    def test_receipt_hardlink_alias_of_cpu_log_is_rejected(self):
        self.out.mkdir()
        receipt = self.root/'result.json'
        receipt.write_bytes(b'original receipt\n')
        os.link(receipt, self.out/'cpu-tests.txt')
        self.assert_rejected_before_work(['--sr-log', str(self.input),
                                          '--sr-session-receipt', str(receipt)])

    def test_nested_static_output_alias_is_rejected(self):
        directory = self.out/'runtime'
        directory.mkdir(parents=True)
        os.link(self.input, directory/'manifest.json')
        self.assert_rejected_before_work(['--sr-log', str(self.input)])

    def test_missing_input_does_not_create_output_directory(self):
        self.assert_rejected_before_work(['--sr-log', str(self.root/'missing.log')])
        self.assertFalse(self.out.exists())

    def test_input_directory_is_rejected_before_work(self):
        self.assert_rejected_before_work(['--sr-log', str(self.root)])
        self.assertFalse(self.out.exists())

    def test_unrelated_outputs_and_external_inputs_are_allowed(self):
        self.out.mkdir()
        (self.out/'validation.json').write_bytes(b'old generated report\n')
        verify.protect_inputs([self.input], self.out)
        self.assertEqual(self.input.read_bytes(), b'original source bytes\n')

    def test_absent_output_tree_and_external_input_are_allowed_without_writes(self):
        verify.protect_inputs([self.input], self.out)
        self.assertFalse(self.out.exists())

    def test_capture_binary_and_metadata_inputs_are_all_preflighted(self):
        capture = self.root/'capture'
        capture.mkdir()
        for index in (1, 2):
            for suffix in ('.vs-cb2.bin', '.json'):
                (capture/(f'capture-{index}'+suffix)).write_bytes(b'capture input\n')
        self.out.mkdir()
        os.link(capture/'capture-2.json', self.out/'f7-title-capture-1.json')
        self.assert_rejected_before_work(['--capture-directory', str(capture)])

    def test_explicit_rebuild_inputs_include_target_tools_manifests_and_headers(self):
        reference = self.root/'build/research'
        compiler = self.root/'fxc.exe'
        args = argparse.Namespace(sr_log=self.input, sr_session_receipt=None,
                                  capture_directory=None, reference_root=reference, fxc=compiler,
                                  windows_sdk_include=self.root/'windows-sdk/um')
        with patch.object(verify, 'ROOT', self.root):
            paths = verify.explicit_inputs(args)
        self.assertEqual(len(paths), 16)
        self.assertIn(compiler, paths)
        self.assertIn(reference/'rtx-encore-v2.0.1/extracted/rtx-encore.dll', paths)
        self.assertIn(reference/'rtx-encore-generic-astra/shader-manifest.json', paths)
        self.assertIn(reference/'rtx-encore-generic-astra/encore-shader-manifest.json', paths)
        for name in ('sl_struct.h', 'sl_dlss_g.h', 'sl_consts.h',
                     'sl_core_types.h', 'sl_core_api.h'):
            self.assertIn(self.root/'external/streamline'/name, paths)
        self.assertIn(self.root/'external/nvngx_dlss_sdk/nvsdk_ngx_params.h', paths)
        ffx = self.root/'external/FidelityFX-SDK/ffx-api/include/ffx_api'
        for name in ('ffx_framegeneration.h', 'ffx_api_types.h'):
            self.assertIn(ffx/name, paths)
        for name in ('d3d11.h', 'd3d12.h', 'libloaderapi.h'):
            self.assertIn(self.root/'windows-sdk/um'/name, paths)

    def assert_new_sdk_header_aliases_rejected(self, *, old_manifest):
        header_names = ('windows-sdk/um/libloaderapi.h',
                        'external/streamline/sl_core_types.h',
                        'external/streamline/sl_core_api.h',
                        'external/FidelityFX-SDK/ffx-api/include/ffx_api/ffx_framegeneration.h',
                        'external/FidelityFX-SDK/ffx-api/include/ffx_api/ffx_api_types.h')
        for index, name in enumerate(header_names):
            with self.subTest(header=name):
                root = self.root/f'case-{index}'
                out = root/'evidence'
                reference = root/'build/research'
                compiler = root/'fxc.exe'
                sdk = root/'windows-sdk/um'
                streamline = root/'external/streamline'
                old_headers = [streamline/file for file in ('sl_struct.h', 'sl_dlss_g.h', 'sl_consts.h')]
                old_headers += [root/'external/nvngx_dlss_sdk/nvsdk_ngx_params.h',
                                sdk/'d3d11.h', sdk/'d3d12.h']
                inputs = [compiler, reference/'rtx-encore-v2.0.1/extracted/rtx-encore.dll',
                          reference/'rtx-encore-generic-astra/shader-manifest.json',
                          reference/'rtx-encore-generic-astra/encore-shader-manifest.json',
                          *old_headers, *(root/file for file in header_names)]
                for path in inputs:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(('original input: '+str(path.relative_to(root))).encode('utf-8'))
                out.mkdir()
                if old_manifest:
                    runtime = out/'runtime'
                    runtime.mkdir()
                    (runtime/'manifest.json').write_text(json.dumps(dict(
                        sdk_reference_hashes={str(path): verify.sha(path) for path in old_headers},
                        files={})), encoding='utf-8')
                else:
                    self.assertFalse((out/'runtime/manifest.json').exists())
                arguments = ['--reference-root', str(reference), '--fxc', str(compiler),
                             '--windows-sdk-include', str(sdk)]
                header = root/name
                alias = out/'camera-literals.json'
                original = header.read_bytes()
                os.link(header, alias)
                self.assertTrue(alias.samefile(header))
                with (patch.object(Path, 'mkdir') as mkdir,
                      patch.object(Path, 'write_text') as write_text,
                      patch.object(Path, 'write_bytes') as write_bytes):
                    self.assert_rejected_before_work(
                        arguments, root=root,
                        expected_error='refusing output alias of input: '+str(header.resolve()))
                mkdir.assert_not_called()
                write_text.assert_not_called()
                write_bytes.assert_not_called()
                self.assertEqual(header.read_bytes(), original)
                self.assertEqual(alias.read_bytes(), original)

    def test_new_sdk_header_camera_aliases_are_rejected_without_manifest(self):
        self.assert_new_sdk_header_aliases_rejected(old_manifest=False)

    def test_new_sdk_header_camera_aliases_are_rejected_with_old_manifest(self):
        self.assert_new_sdk_header_aliases_rejected(old_manifest=True)

    def test_directory_link_to_external_input_is_rejected(self):
        source_directory = self.root/'raw-input'
        source_directory.mkdir()
        source = source_directory/'source.log'
        source.write_bytes(b'directory-linked source\n')
        self.out.mkdir()
        link = self.out/'runtime'
        if os.name == 'nt':
            result = subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J',
                                     str(link), str(source_directory)], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            link.symlink_to(source_directory, target_is_directory=True)
        with self.assertRaises(ValueError):
            verify.protect_inputs([source], self.out)
        self.assertEqual(source.read_bytes(), b'directory-linked source\n')


    def historical_record(self):
        self.out.mkdir(exist_ok=True)
        payload = self.root/'payload.dll'
        payload.write_bytes(b'payload bytes')
        pefile = self.root/'pefile.py'
        pefile.write_bytes(b'pefile bytes')
        return dict(schema=1, profile='historical-nr-entrypoints', input=dict(path=str(payload)),
                    export_inspection=dict(analyzer=dict(pefile_path=str(pefile))),
                    tools=dict(files={str(self.input): '0'*64}))

    def test_recorded_nr_payload_alias_is_rejected_before_work(self):
        record = self.historical_record()
        os.link(Path(record['input']['path']), self.out/'cpu-tests.txt')
        (self.out/'nr-reference-entrypoints.json').write_text(json.dumps(record), encoding='utf-8')
        self.assert_rejected_before_work([])

    def test_recorded_nr_tool_alias_is_rejected_before_work(self):
        record = self.historical_record()
        os.link(self.input, self.out/'cpu-tests.txt')
        (self.out/'nr-reference-entrypoints.json').write_text(json.dumps(record), encoding='utf-8')
        self.assert_rejected_before_work([])

    def test_boolean_nr_schema_is_rejected_before_work(self):
        record = self.historical_record()
        record['schema'] = True
        (self.out/'nr-reference-entrypoints.json').write_text(json.dumps(record), encoding='utf-8')
        self.assert_rejected_before_work([])

    def test_recorded_nr_report_cpu_log_hardlink_is_rejected_before_work(self):
        record = self.historical_record()
        report = self.out/'nr-reference-entrypoints.json'
        report.write_text(json.dumps(record), encoding='utf-8')
        os.link(report, self.out/'cpu-tests.txt')
        self.assert_rejected_before_work([])

    def test_retained_report_nested_output_alias_is_rejected(self):
        self.out.mkdir()
        report = self.out/'nr-reference-entrypoints.json'
        report.write_bytes(b'original report')
        nested = self.out/'runtime'
        nested.mkdir()
        os.link(report, nested/'manifest.json')
        with self.assertRaisesRegex(ValueError, 'output alias'):
            verify.protect_inputs([], self.out, retained_reports=(report,))
        self.assertEqual(report.read_bytes(), b'original report')

    def test_recorded_nr_exports_cpu_log_hardlink_is_rejected_before_work(self):
        record = self.historical_record()
        (self.out/'nr-reference-entrypoints.json').write_text(json.dumps(record), encoding='utf-8')
        exports = self.out/'nr-historical-reference-exports.json'
        exports.write_text(json.dumps(record['export_inspection']), encoding='utf-8')
        os.link(exports, self.out/'cpu-tests.txt')
        self.assert_rejected_before_work([])

    def test_retained_report_at_its_own_path_is_allowed_without_writes(self):
        self.out.mkdir()
        report = self.out/'nr-reference-entrypoints.json'
        report.write_bytes(b'original report')
        verify.protect_inputs([], self.out, retained_reports=(report,))
        self.assertEqual(report.read_bytes(), b'original report')

    def test_saved_sdk_header_alias_is_rejected_without_fresh_decode_arguments(self):
        runtime = self.out/'runtime'
        runtime.mkdir(parents=True)
        header = self.root/'d3d11.h'
        header.write_bytes(b'original public SDK header')
        (runtime/'manifest.json').write_text(json.dumps(
            dict(sdk_reference_hashes={str(header): '0'*64}, files={})), encoding='utf-8')
        os.link(header, self.out/'cpu-tests.txt')
        self.assert_rejected_before_work([])

    def test_saved_static_contract_alias_is_rejected_before_cpu_log_write(self):
        runtime = self.out/'runtime'
        runtime.mkdir(parents=True)
        contract = runtime/'dx11-completion-contract.json'
        contract.write_bytes(b'original static contract')
        (runtime/'manifest.json').write_text(json.dumps(
            dict(sdk_reference_hashes={}, files={contract.name: '0'*64})), encoding='utf-8')
        os.link(contract, self.out/'cpu-tests.txt')
        self.assert_rejected_before_work([])

    def reference_input_alias(self, kind):
        directory = self.out/'nr-codec'
        directory.mkdir(parents=True)
        files = dict(compiler=self.root/'fxc.exe', source=self.root/'reference.hlsl',
                     include=self.root/'reference.hlsli', artifact=directory/'reference-entry.dxbc')
        for name, file in files.items():
            file.write_bytes(('original '+name+' bytes').encode('ascii'))
        record = dict(compiler=str(files['compiler']), source=files['source'].name,
                      includes={files['include'].name: '0'*64},
                      entrypoints=[dict(file=files['artifact'].name)])
        (directory/'reference-compile.json').write_text(json.dumps(record), encoding='utf-8')
        os.link(files[kind], self.out/'cpu-tests.txt')
        self.assert_rejected_before_work([])

    def test_saved_reference_compiler_alias_is_rejected_before_work(self):
        self.reference_input_alias('compiler')

    def test_saved_reference_shader_source_alias_is_rejected_before_work(self):
        self.reference_input_alias('source')

    def test_saved_reference_shader_include_alias_is_rejected_before_work(self):
        self.reference_input_alias('include')

    def test_saved_reference_dxbc_alias_is_rejected_before_work(self):
        self.reference_input_alias('artifact')

    def test_dlss_trace_in_generated_tree_is_rejected_before_work(self):
        self.out.mkdir()
        source = self.out/'cpu-tests.txt'
        source.write_bytes(b'original trace input')
        self.assert_rejected_before_work(['--dlss-trace', str(source)])

    def test_dlss_trace_hardlink_alias_is_rejected_before_work(self):
        self.out.mkdir()
        os.link(self.input, self.out/'f7-dlss-trace.json')
        self.assert_rejected_before_work(['--dlss-trace', str(self.input)])

    def test_dlss_trace_expected_hash_requires_explicit_input(self):
        self.assert_rejected_before_work(['--dlss-trace-sha256', '0'*64])

    def native_inputs(self):
        from nr_native_receipt import NATIVE_INPUTS, NGX_INPUTS
        receipt, log = self.root/'native-result.json', self.root/'ctest.log'
        sdk = self.root/'sdk'
        paths = [receipt, log, *(self.root/'native'/name for name in NATIVE_INPUTS),
                 *(sdk/name for name in NGX_INPUTS)]
        for path in paths:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(('original input: '+path.name).encode('utf-8'))
        arguments = ['--nr-native-receipt', str(receipt), '--nr-native-ctest-log', str(log),
                     '--nr-native-ngx-include', str(sdk)]
        return paths, arguments

    def test_native_options_require_complete_group_before_any_work(self):
        for option in ('--nr-native-receipt', '--nr-native-ctest-log', '--nr-native-ngx-include'):
            with self.subTest(option=option):
                self.assert_rejected_before_work([option, str(self.input)],
                                                expected_error='must be supplied together')

    def test_all_native_inputs_are_protected_before_any_work(self):
        paths, arguments = self.native_inputs()
        self.out.mkdir()
        alias = self.out/'cpu-tests.txt'
        for path in paths:
            with self.subTest(input=path.name):
                os.link(path, alias)
                try:
                    self.assert_rejected_before_work(arguments,
                                                     expected_error='output alias of input')
                finally:
                    alias.unlink()

    def test_invalid_native_receipt_is_rejected_before_tests_or_output_creation(self):
        _, arguments = self.native_inputs()
        self.assert_rejected_before_work(arguments)
        self.assertFalse(self.out.exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
