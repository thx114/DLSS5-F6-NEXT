"""Reproduce CPU tests, static-artifact hashes, and optional captured CB reports."""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest

sys.dont_write_bytecode = True
from cb_probe import analyze
from rebuild_evidence import SHA, NR_SHUTDOWN_CLEANUP_ARRAYS
from sr_temporal_probe import analyze_cbs
from sr_log_probe import analyze as analyze_sr_log, attach_session_receipt

ROOT = Path(__file__).resolve().parent
STATIC_MANIFESTS = ('manifest.json', 'runtime/manifest.json',
                    'nr-codec/manifest.json', 'nr-warp/manifest.json')


def sha(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def nr_shutdown_cleanup_counts(report: dict) -> dict[str, int]:
    """Count actual saved contract arrays, not a second hand-maintained total."""
    cleanup = report.get('bounded_cleanup')
    if not isinstance(cleanup, dict):
        raise ValueError('missing NR shutdown bounded cleanup report')
    counts = {}
    for field, array in NR_SHUTDOWN_CLEANUP_ARRAYS.items():
        rows = cleanup.get(array)
        if not isinstance(rows, list):
            raise ValueError('invalid NR shutdown cleanup array: '+array)
        counts[field] = len(rows)
    return counts


def historical_shutdown_identity_counts(report: dict) -> dict[str, int]:
    """Count the historical contract arrays, independently of Encore profiles."""
    from nr_reference_evidence import SHUTDOWN_IDENTITY_ARRAYS
    identity = report.get('shutdown_identity')
    if not isinstance(identity, dict):
        raise ValueError('missing historical NR shutdown identity report')
    counts = {}
    for field, array in SHUTDOWN_IDENTITY_ARRAYS.items():
        rows = identity.get(array)
        if not isinstance(rows, list):
            raise ValueError('invalid historical NR shutdown identity array: '+array)
        counts[field] = len(rows)
    return counts


def recorded_static_inputs(out: Path) -> tuple[list[Path], list[Path]]:
    """Protect saved static reports and their external sources before any writes."""
    def confined(directory: Path, name: str) -> Path:
        artifact = directory/name
        if artifact.resolve().parent != directory.resolve():
            raise ValueError('saved static artifact outside profile: '+str(artifact))
        return artifact

    external, retained = [], []
    for name in STATIC_MANIFESTS:
        path = out/name
        if not path.is_file():
            continue
        document = json.loads(path.read_text(encoding='utf-8'))
        references, files = document.get('sdk_reference_hashes', {}), document.get('files', {})
        if not isinstance(references, dict) or not isinstance(files, dict):
            raise ValueError('invalid saved static input inventory: '+str(path))
        retained.append(path)
        if document.get('target'):
            external.append(Path(document['target']))
        external.extend(Path(source) for source in references)
        for file in files:
            retained.append(confined(path.parent, file))
    for name in ('nr-codec/reference-compile.json', 'nr-warp/reference-compile.json'):
        path = out/name
        if path.is_file():
            retained.append(path)
            document = json.loads(path.read_text(encoding='utf-8'))
            if document.get('compiler'):
                external.append(Path(document['compiler']))
            if document.get('source'):
                external.append(confined(ROOT, document['source']))
            includes, entries = document.get('includes', {}), document.get('entrypoints', [])
            if not isinstance(includes, dict) or not isinstance(entries, list):
                raise ValueError('invalid reference compiler input inventory: '+str(path))
            external.extend(confined(ROOT, file) for file in includes)
            for entry in entries:
                if not isinstance(entry, dict) or not isinstance(entry.get('file'), str):
                    raise ValueError('invalid reference artifact inventory: '+str(path))
                retained.append(confined(path.parent, entry['file']))
    return external, retained


def explicit_inputs(args: argparse.Namespace) -> list[Path]:
    """Data and tools supplied by this invocation, before any output is written."""
    inputs = [path for path in (args.sr_log, args.sr_session_receipt,
                               getattr(args, 'dlss_trace', None)) if path is not None]
    if getattr(args, 'nr_native_receipt', None) is not None:
        from nr_native_receipt import NATIVE_INPUTS, NGX_INPUTS
        inputs.extend((args.nr_native_receipt, args.nr_native_ctest_log))
        inputs.extend(ROOT/'native'/name for name in NATIVE_INPUTS)
        inputs.extend(args.nr_native_ngx_include/name for name in NGX_INPUTS)
    if args.capture_directory:
        inputs.extend(args.capture_directory/name for index in (1, 2)
                      for name in (f'capture-{index}.vs-cb2.bin', f'capture-{index}.json'))
    if args.reference_root:
        inputs.extend((args.fxc,
                       args.reference_root/'rtx-encore-v2.0.1/extracted/rtx-encore.dll',
                       args.reference_root/'rtx-encore-generic-astra/shader-manifest.json',
                       args.reference_root/'rtx-encore-generic-astra/encore-shader-manifest.json'))
        sdk = args.reference_root.parent.parent/'external/streamline'
        inputs.extend(sdk/name for name in ('sl_struct.h', 'sl_dlss_g.h', 'sl_consts.h',
                                           'sl_core_types.h', 'sl_core_api.h'))
        inputs.append(args.reference_root.parent.parent/'external/nvngx_dlss_sdk/nvsdk_ngx_params.h')
        ffx = args.reference_root.parent.parent/'external/FidelityFX-SDK/ffx-api/include/ffx_api'
        inputs.extend(ffx/name for name in ('ffx_framegeneration.h', 'ffx_api_types.h'))
        sdk_headers = getattr(args, 'windows_sdk_include', None)
        if sdk_headers:
            inputs.extend(sdk_headers/file for file in ('d3d11.h', 'd3d12.h', 'libloaderapi.h'))
    historical = ROOT/'evidence/nr-reference-entrypoints.json'
    if historical.is_file():
        from nr_reference_evidence import reference_sources
        inputs.extend(reference_sources(json.loads(historical.read_text(encoding='utf-8'))))
    recorded, _ = recorded_static_inputs(ROOT/'evidence')
    inputs.extend(recorded)
    return inputs


def protect_inputs(inputs: list[Path], out: Path, *, retained_reports: tuple[Path, ...] = ()) -> None:
    """Reserve the output tree and reject aliases before mkdir, tests or FXC.

    Checking the entire generated tree also protects inputs from optional static
    rebuilds, without maintaining a second list of every producer's filenames.
    Follow directory links once so aliases in profile subdirectories are covered.
    Retained reports may occupy their own exact path in the output tree; every
    other path aliasing one is rejected, including symlinks and directory links.
    """
    output_root = out.resolve()
    sources = []
    for path, retained in ([(path, False) for path in inputs]+
                           [(path, True) for path in retained_reports]):
        source = path.resolve(strict=True)
        if not source.is_file():
            raise ValueError('input must be a regular file: '+str(path))
        if source.is_relative_to(output_root) and not retained:
            raise ValueError('refusing input inside generated output tree: '+str(path))
        allowed_path = os.path.normcase(os.path.abspath(path)) if retained else None
        sources.append((source, allowed_path))
    if not sources:
        return
    pending = [out] if out.exists() else []
    visited = set()
    while pending:
        directory = pending.pop()
        resolved = directory.resolve()
        if resolved in visited:
            continue
        visited.add(resolved)
        for candidate in directory.iterdir():
            if candidate.is_dir():
                pending.append(candidate)
            elif candidate.is_file():
                for source, allowed_path in sources:
                    if candidate.samefile(source):
                        if allowed_path == os.path.normcase(os.path.abspath(candidate)):
                            continue
                        raise ValueError('refusing output alias of input: '+str(source))


def compile_reference(fxc: Path, out: Path, source_name: str, entries: tuple[str, ...],
                      includes: tuple[str, ...] = ()) -> None:
    source = ROOT/source_name
    report = dict(source=source.name, source_sha256=sha(source),
                  includes={name: sha(ROOT/name) for name in includes},
                  compiler=str(fxc.resolve()), compiler_sha256=sha(fxc),
                  target='cs_5_1', flags=['/Ges', '/WX', '/O3'],
                  gpu_tested=False, entrypoints=[])
    for entry in entries:
        output = out/('reference-'+entry+'.dxbc')
        result = subprocess.run([str(fxc), '/nologo', '/Ges', '/WX', '/O3',
                                 '/T', 'cs_5_1', '/E', entry, '/Fo', str(output), str(source)],
                                capture_output=True)
        if result.returncode:
            raise RuntimeError((result.stdout+result.stderr).decode('utf-8', errors='replace'))
        report['entrypoints'].append(dict(entry=entry, file=output.name,
                                         sha256=sha(output), compiler_exit=0))
    (out/'reference-compile.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


def verify_reference(report_path: Path, source_name: str, entries: tuple[str, ...],
                     includes: tuple[str, ...] = ()) -> dict:
    reference = json.loads(report_path.read_text(encoding='utf-8'))
    if reference['source'] != source_name or reference['source_sha256'] != sha(ROOT/source_name):
        raise ValueError('reference shader source changed; recompile with --reference-root/--fxc')
    expected_includes = {name: sha(ROOT/name) for name in includes}
    if reference.get('includes', {}) != expected_includes:
        raise ValueError('reference shader include changed or missing')
    if reference['target'] != 'cs_5_1' or sorted(x['entry'] for x in reference['entrypoints']) != sorted(entries):
        raise ValueError('expected reference entrypoints must be present exactly once')
    if reference.get('gpu_tested') is not False:
        raise ValueError('reference compiler report must not claim GPU validation')
    if reference['flags'] != ['/Ges', '/WX', '/O3'] or sha(Path(reference['compiler'])) != reference['compiler_sha256']:
        raise ValueError('reference compiler/flags changed')
    for entry in reference['entrypoints']:
        artifact = (report_path.parent/entry['file']).resolve()
        if (artifact.parent != report_path.parent.resolve() or entry['compiler_exit'] != 0 or
                sha(artifact) != entry['sha256'] or artifact.read_bytes()[:4] != b'DXBC'):
            raise ValueError('reference compiled shader changed')
    return reference


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture-directory', type=Path)
    parser.add_argument('--reference-root', type=Path)
    parser.add_argument('--fxc', type=Path)
    parser.add_argument('--windows-sdk-include', type=Path)
    parser.add_argument('--sr-log', type=Path)
    parser.add_argument('--sr-session-receipt', type=Path)
    parser.add_argument('--dlss-trace', type=Path)
    parser.add_argument('--dlss-trace-sha256')
    parser.add_argument('--nr-native-receipt', type=Path,
                        help='verify an existing owner CPU receipt without running native code')
    parser.add_argument('--nr-native-ctest-log', type=Path)
    parser.add_argument('--nr-native-ngx-include', type=Path)
    args = parser.parse_args(argv)
    native_arguments = (args.nr_native_receipt, args.nr_native_ctest_log, args.nr_native_ngx_include)
    if any(path is not None for path in native_arguments) and not all(
            path is not None for path in native_arguments):
        parser.error('--nr-native-receipt, --nr-native-ctest-log and --nr-native-ngx-include '
                     'must be supplied together')
    if args.sr_session_receipt and not args.sr_log:
        parser.error('--sr-session-receipt requires --sr-log')
    if args.dlss_trace_sha256 and not args.dlss_trace:
        parser.error('--dlss-trace-sha256 requires --dlss-trace')
    if bool(args.reference_root) != bool(args.fxc):
        parser.error('--reference-root and --fxc must be supplied together')
    if args.reference_root and args.windows_sdk_include is None:
        parser.error('--windows-sdk-include is required with --reference-root for runtime API evidence')
    out = ROOT/'evidence'
    try:
        historical = out/'nr-reference-entrypoints.json'
        retained = tuple(path for path in (historical, out/'nr-historical-reference-exports.json')
                         if path.is_file()) if historical.is_file() else ()
        _, static_reports = recorded_static_inputs(out)
        retained += tuple(static_reports)
        protect_inputs(explicit_inputs(args), out, retained_reports=retained)
        native_parameter_store = None
        if args.nr_native_receipt is not None:
            from nr_native_receipt import verify_receipt
            native_parameter_store = verify_receipt(
                args.nr_native_receipt, args.nr_native_ctest_log, ROOT/'native',
                args.nr_native_ngx_include)
    except (ValueError, OSError, RuntimeError) as error:
        parser.error(str(error))
    out.mkdir(exist_ok=True)
    if args.reference_root:
        subprocess.run([sys.executable, '-B', str(ROOT/'rebuild_evidence.py'),
                        '--reference-root', str(args.reference_root), '--fxc', str(args.fxc),
                        '--output', str(out)], check=True)
        subprocess.run([sys.executable, '-B', str(ROOT/'rebuild_evidence.py'),
                        '--reference-root', str(args.reference_root), '--profile', 'runtime',
                        '--windows-sdk-include', str(args.windows_sdk_include),
                        '--output', str(out)], check=True)
        subprocess.run([sys.executable, '-B', str(ROOT/'rebuild_evidence.py'),
                        '--reference-root', str(args.reference_root), '--profile', 'nr-codec',
                        '--fxc', str(args.fxc), '--output', str(out)], check=True)
        subprocess.run([sys.executable, '-B', str(ROOT/'rebuild_evidence.py'),
                        '--reference-root', str(args.reference_root), '--profile', 'nr-warp',
                        '--fxc', str(args.fxc), '--output', str(out)], check=True)
        compile_reference(args.fxc, out/'nr-codec', 'nr_codec_rect_reference.hlsl',
                          ('EncodeReference', 'DecodeReference'))
        compile_reference(args.fxc, out/'nr-warp', 'nr_guides_reference.hlsl',
                          ('GuidesReference',), ('nr_warp_reference.hlsli',))
    manifest_path = out/'manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    runtime_manifest = out/'runtime/manifest.json'
    profiles = {}
    codec_manifest = out/'nr-codec/manifest.json'
    warp_manifest = out/'nr-warp/manifest.json'
    for expected_profile, current in (('camera', manifest_path), ('runtime', runtime_manifest),
                                      ('nr-codec', codec_manifest), ('nr-warp', warp_manifest)):
        document = json.loads(current.read_text(encoding='utf-8'))
        if document['sha256'] != SHA or document['profile'] != expected_profile:
            raise ValueError('static target/profile identity mismatch: '+str(current))
        for name, expected in document['files'].items():
            path = (current.parent/name).resolve()
            if path.parent != current.parent.resolve() or sha(path) != expected:
                raise ValueError('static artifact changed: '+name)
        for name, expected in document.get('sdk_reference_hashes', {}).items():
            if sha(Path(name)) != expected:
                raise ValueError('SDK reference changed: '+name)
        profiles[expected_profile] = dict(
            native_instructions=sum(x['instructions'] for x in document['code']),
            shader_tokens=sum(x['tokens'] for x in document['shaders']),
            instruction_assertions=document.get('instruction_assertions', 0),
            literal_assertions=document.get('literal_assertions', 0),
            preset_table_assertions=document.get('preset_table_assertions', 0),
            parameter_key_assertions=document.get('parameter_key_assertions', 0),
            bytecode_table_assertions=document.get('bytecode_table_assertions', 0),
            bytecode_native_binding_assertions=document.get('bytecode_native_binding_assertions', 0))
        profiles[expected_profile].update(
            payload_export_name_assertions=document.get('payload_export_name_assertions', 0),
            payload_identity_row_assertions=document.get('payload_identity_row_assertions', 0),
            parameter_vtable_assertions=document.get('parameter_vtable_assertions', 0),
            feature_owner_vtable_assertions=document.get('feature_owner_vtable_assertions', 0))
        profiles[expected_profile]['parameter_rtti_assertions'] = document.get('parameter_rtti_assertions', 0)
        for field in ('completion_vtable_assertions', 'completion_rtti_assertions',
                      'completion_import_assertions', 'completion_api_slot_assertions',
                      'completion_query_flag_assertions', 'payload_import_name_assertions',
                      'payload_import_write_api_assertions', 'nr_creation_key_assertions',
                      'nr_creation_literal_assertions', 'fg_resource_guid_assertions',
                      'fg_resource_export_assertions', 'fg_resource_api_slot_assertions',
                      'fg_resource_enum_assertions', 'nr_device_holder_guid_assertions',
                      'nr_device_holder_api_slot_assertions', 'nr_device_holder_function_assertions',
                      'nr_shutdown_scope_import_assertions', 'nr_shutdown_scope_function_assertions',
                      'ngx_shutdown_interposition_literal_assertions', 'ngx_shutdown_interposition_import_assertions',
                      'ngx_shutdown_interposition_function_assertions', 'ngx_shutdown_interposition_section_assertions',
                      'ngx_hook_transaction_stage_assertions', 'ngx_hook_transaction_import_assertions',
                      'ngx_hook_transaction_thunk_assertions', 'ngx_hook_transaction_function_assertions',
                      'ngx_hook_transaction_flag_assertions',
                      'fg_input_fence_instruction_assertions', 'fg_input_fence_import_assertions',
                      'fg_input_fence_function_assertions', 'fg_input_fence_api_slot_assertions',
                      'fg_input_fence_path_assertions'):
            profiles[expected_profile][field] = document.get(field, 0)
        if expected_profile == 'runtime':
            contract_name = 'nr-shutdown-scope-contract.json'
            if contract_name not in document['files']:
                raise ValueError('NR shutdown contract missing from static artifact inventory')
            counts = nr_shutdown_cleanup_counts(json.loads(
                (current.parent/contract_name).read_text(encoding='utf-8')))
            if any(type(document.get(field)) is not int or document[field] != count
                   for field, count in counts.items()):
                raise ValueError('NR shutdown cleanup manifest count mismatch')
            profiles[expected_profile].update(counts)
    target = Path(manifest['target'])
    if not target.is_file() or sha(target) != manifest['sha256']:
        raise ValueError('original target missing or changed')
    reference_path = out/'nr-codec/reference-compile.json'
    reference = verify_reference(reference_path, 'nr_codec_rect_reference.hlsl',
                                 ('EncodeReference', 'DecodeReference'))
    guide_reference_path = out/'nr-warp/reference-compile.json'
    guide_reference = verify_reference(guide_reference_path, 'nr_guides_reference.hlsl',
                                       ('GuidesReference',), ('nr_warp_reference.hlsli',))
    compiled_entries = len(reference['entrypoints'])+len(guide_reference['entrypoints'])
    log = io.StringIO()
    suite = unittest.defaultTestLoader.discover(str(ROOT), pattern='test_*.py')
    result = unittest.TextTestRunner(stream=log, verbosity=2).run(suite)
    (out/'cpu-tests.txt').write_text(log.getvalue(), encoding='utf-8')
    if not result.wasSuccessful():
        print(log.getvalue())
        raise SystemExit(1)
    captures, temporal_reports = [], []
    if args.capture_directory:
        directory = args.capture_directory.resolve()
        paths = [directory/f'capture-{n}.vs-cb2.bin' for n in (1, 2)]
        for index, cb in enumerate(paths, 1):
            output = out/f'f7-title-capture-{index}.json'
            report = analyze(cb, directory/f'capture-{index}.json', [80, 144, 208, 272],
                             'row-major', 'row', [144, 208], [144, 80, 272],
                             paths[0] if index == 2 else None, 272 if index == 2 else None)
            output.write_text(json.dumps(report, indent=2, allow_nan=False)+chr(10), encoding='utf-8')
            captures.append(dict(file=output.name, sha256=sha(output),
                                 cb_sha256=report['cb_sha256'], camera_authority=False))
        output = out/'f7-title-projection-shift.json'
        report = analyze_cbs(paths[0], paths[1], offset=80, storage='row-major', vectors='row')
        output.write_text(json.dumps(report, indent=2, allow_nan=False)+chr(10), encoding='utf-8')
        temporal_reports.append(dict(file=output.name, sha256=sha(output),
                                     cb_sha256=[x['sha256'] for x in report['input_cbs']],
                                     raster_size_supplied=False, raw_or_sdk_jitter_supplied=False,
                                     camera_authority=False, source_authenticated=False))
    sr_log_reports = []
    if args.sr_log:
        raw = args.sr_log.read_bytes()
        report = analyze_sr_log(raw.decode('utf-8-sig'))
        report.update(input_file=str(args.sr_log.resolve()), input_bytes=len(raw),
                      input_sha256=hashlib.sha256(raw).hexdigest())
        if args.sr_session_receipt:
            attach_session_receipt(report, args.sr_session_receipt)
        output = out/'f7-sdk-input-log.json'
        output.write_text(json.dumps(report, indent=2, allow_nan=False)+chr(10), encoding='utf-8')
        sr_log_reports.append(dict(file=output.name, sha256=sha(output), input_log_sha256=report['input_sha256'],
                                   records=report['records'], provenance_authenticated=False,
                                   process_or_loaded_memory_identity_authenticated=False,
                                   formula_selected=False, gpu_completion_proven=False))
    dlss_trace_reports = []
    if args.dlss_trace:
        from dlss_trace_probe import analyze_file, write_report
        trace = analyze_file(args.dlss_trace, expected_sha256=args.dlss_trace_sha256)
        output = out/'f7-dlss-trace.json'
        write_report(args.dlss_trace, output, trace)
        dlss_trace_reports.append(dict(file=output.name, sha256=sha(output),
            input_sha256=trace['input']['sha256'], blocks=len(trace['blocks']),
            gpu_completion_proven=False, causality_proven=False, performance_baseline=False))
    historical_nr_reference = None
    historical = out/'nr-reference-entrypoints.json'
    if historical.is_file():
        from nr_reference_evidence import verify_saved_report
        historical_nr_reference = verify_saved_report(historical)
        historical_document = json.loads(historical.read_text(encoding='utf-8'))
        counts = historical_shutdown_identity_counts(historical_document)
        if any(historical_nr_reference.get(key) != value for key, value in counts.items()):
            raise ValueError('historical NR shutdown identity counts disagree with fresh decode')
        historical_nr_reference.update(counts)
        exports = out/'nr-historical-reference-exports.json'
        if exports.is_file():
            stored = historical_document['export_inspection']
            if json.loads(exports.read_text(encoding='utf-8')) != stored:
                raise ValueError('historical NR export reports disagree')
            historical_nr_reference['export_report_sha256'] = sha(exports)
    validation = dict(status='PASS', method='CPU tests and file/hash/numeric reproducibility only',
                      historical_nr_reference=historical_nr_reference,
                      native_parameter_store=native_parameter_store,
                      python=sys.version.split()[0], cpu_tests=result.testsRun,
                      failures=len(result.failures), errors=len(result.errors),
                      skipped=len(result.skipped),
                      static_manifest_sha256=sha(manifest_path),
                      runtime_manifest_sha256=sha(runtime_manifest), static_profiles=profiles,
                      codec_manifest_sha256=sha(codec_manifest),
                      warp_manifest_sha256=sha(warp_manifest),
                      reference_compile_sha256=sha(reference_path),
                      guide_reference_compile_sha256=sha(guide_reference_path),
                      reference_compiled_entrypoints=compiled_entries,
                      static_decode_refreshed=bool(args.reference_root),
                      target_sha256=manifest['sha256'], target_executed=False,
                      gpu_tested=False, product_integrated=False, camera_authority=False,
                      captures=captures, temporal_reports=temporal_reports, sr_log_reports=sr_log_reports,
                      dlss_trace_reports=dlss_trace_reports,
                      cpu_log_sha256=sha(out/'cpu-tests.txt'),
                      source_files={p.relative_to(ROOT).as_posix(): sha(p) for p in sorted(
                          list(ROOT.iterdir())+list((ROOT/'native').glob('*')))
                          if p.is_file() and (p.suffix in ('.py', '.md', '.hlsl', '.hlsli', '.h', '.cpp', '.in')
                                              or p.name == 'CMakeLists.txt')})
    (out/'validation.json').write_text(json.dumps(validation, indent=2), encoding='utf-8')
    print(json.dumps(dict(status='PASS', cpu_tests=result.testsRun, captured_reports=len(captures),
                          native_parameter_store=native_parameter_store,
                          temporal_reports=len(temporal_reports),
                          sr_log_reports=len(sr_log_reports),
                          dlss_trace_reports=len(dlss_trace_reports),
                          static_profiles=profiles, reference_compiled_entrypoints=compiled_entries,
                          target_executed=False, gpu_tested=False)))


if __name__ == '__main__':
    main()
