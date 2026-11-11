"""Synthetic PE32+ integration tests; no payload DLL is read or executed.

Discover respects NR_PAYLOAD_REFERENCE_ROOT when set. Otherwise only the tests
explicitly select the current interpreter's installed pefile source using
find_spec; production entry points never select an implicit dependency.
"""

from contextlib import redirect_stderr, redirect_stdout
from functools import lru_cache
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import nr_payload_inspect as inspector


SCRIPT = Path(inspector.__file__).resolve()
REQUIRED = inspector.REQUIRED_EXPORTS
OPTIONAL = inspector.OPTIONAL_EXPORTS


@lru_cache(maxsize=1)
def dependency_options():
    """Bind the test dependency lazily, keeping module import pefile-free."""
    if "NR_PAYLOAD_REFERENCE_ROOT" in os.environ:
        return {"reference_root": Path(os.environ["NR_PAYLOAD_REFERENCE_ROOT"])}
    spec = importlib.util.find_spec("pefile")
    if spec is None or spec.origin is None or not Path(spec.origin).is_file():
        raise RuntimeError("tests need NR_PAYLOAD_REFERENCE_ROOT or an installed pefile source")
    return {"pefile_file": Path(spec.origin)}


def dependency_cli_args(options=None):
    options = dependency_options() if options is None else options
    return [argument for name, path in options.items()
            for argument in ("--" + name.replace("_", "-"), str(path))]


def selected_pefile():
    options = dependency_options()
    if "reference_root" in options:
        return inspector.load_pefile(options["reference_root"])
    return inspector.load_pefile_file(options["pefile_file"])


def directory_alias(alias: Path, target: Path):
    """Create only a temporary directory alias; TemporaryDirectory cleans it up."""
    if os.name == "nt":
        result = subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J",
                                 str(alias), str(target)],
                                capture_output=True, text=True, check=False)
        if result.returncode != 0:
            raise RuntimeError(f"mklink /J failed: {result.stdout} {result.stderr}")
    else:
        alias.symlink_to(target, target_is_directory=True)


def offset(rva):
    return 0x200 + rva - 0x1000


def change(raw, position, fmt, *values):
    result = bytearray(raw)
    struct.pack_into(fmt, result, position, *values)
    return bytes(result)


def fixture(names=REQUIRED, targets=None, executable=True, export_table=True):
    """One section, real DOS/COFF/PE32+ headers, and a complete export table.

    Strings occupy the export-directory range. Direct targets are backed by
    bytes beyond that range; a virtual-only tail also exists for negative cases.
    Test code constructs bytes; production parsing remains entirely in pefile.
    """
    raw = bytearray(0x1200)
    raw[:2] = b"MZ"
    struct.pack_into("<I", raw, 0x3C, 0x80)
    raw[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", raw, 0x84, 0x8664, 1, 0, 0, 0, 0xF0, 0x2022)
    opt = 0x98
    struct.pack_into("<H", raw, opt, 0x20B)
    struct.pack_into("<II", raw, opt + 16, 0x1900, 0x1000)
    struct.pack_into("<Q", raw, opt + 24, 0x180000000)
    struct.pack_into("<II", raw, opt + 32, 0x1000, 0x200)
    struct.pack_into("<HH", raw, opt + 40, 6, 0)
    struct.pack_into("<HH", raw, opt + 48, 6, 0)
    struct.pack_into("<II", raw, opt + 56, 0x3000, 0x200)
    struct.pack_into("<HH", raw, opt + 68, 3, 0x8160)
    struct.pack_into("<QQQQ", raw, opt + 72, 0x100000, 0x1000, 0x100000, 0x1000)
    struct.pack_into("<I", raw, opt + 108, 16)
    if export_table:
        struct.pack_into("<II", raw, opt + 112, 0x1000, 0x800)
    struct.pack_into("<8sIIIIIIHHI", raw, opt + 0xF0, b".text\0\0\0",
                     0x1200, 0x1000, 0x1000, 0x200, 0, 0, 0, 0,
                     0x60000020 if executable else 0x40000040)
    encoded = [name.encode("ascii") if isinstance(name, str) else name for name in names]
    if len(encoded) > 16:
        raise ValueError("fixture table capacity exceeded")
    struct.pack_into("<IIHHIIIIIII", raw, offset(0x1000), 0, 0, 0, 0,
                     0x10E0, 11, len(encoded), len(encoded), 0x1040, 0x1080, 0x10C0)
    raw[offset(0x10E0):offset(0x10E0) + 12] = b"fixture.dll\0"
    cursor = 0x1100
    for index, name in enumerate(encoded):
        target = targets[index] if targets is not None else 0x1900 + index
        if isinstance(target, bytes):
            rva = 0x1700 + index * 24
            raw[offset(rva):offset(rva) + len(target) + 1] = target + b"\0"
        else:
            rva = target
        struct.pack_into("<I", raw, offset(0x1040) + 4 * index, rva)
        struct.pack_into("<I", raw, offset(0x1080) + 4 * index, cursor)
        struct.pack_into("<H", raw, offset(0x10C0) + 2 * index, index)
        raw[offset(cursor):offset(cursor) + len(name) + 1] = name + b"\0"
        cursor += len(name) + 1
    if cursor > 0x1700:
        raise ValueError("fixture strings overlap forwarder slots")
    return bytes(raw)


class PureClassificationTests(unittest.TestCase):
    def test_exact_five_required_names(self):
        self.assertEqual(REQUIRED, tuple("NVSDK_NGX_D3D12_" + name for name in (
            "Init_Ext", "CreateFeature", "EvaluateFeature", "ReleaseFeature", "Shutdown1")))

    def test_missing_is_distinct_from_direct_and_forwarder(self):
        rows = [inspector.ExportObservation(REQUIRED[0].encode(), 11, 0x1900, file_offset=0xB00),
                inspector.ExportObservation(REQUIRED[1].encode(), 12, 0x1700, b"other.Func")]
        report = inspector.classify_exports(rows)["required_exports"]
        self.assertEqual([report[name]["status"] for name in REQUIRED[:3]],
                         ["direct-present", "forwarder", "missing"])

    def test_duplicate_raw_name_rejected(self):
        item = inspector.ExportObservation(b"unrelated", 1, 0x100)
        with self.assertRaisesRegex(inspector.InspectionError, "duplicate"):
            inspector.classify_exports([item, item])

    def test_name_matching_is_exact_ascii(self):
        for name in (REQUIRED[0].lower().encode(), b"_" + REQUIRED[0].encode(),
                     REQUIRED[0].encode() + b"@8", REQUIRED[0].encode() + b"\xff"):
            with self.subTest(name=name):
                report = inspector.classify_exports([inspector.ExportObservation(name, 1, 0x1900,
                                                                                  file_offset=0xB00)])
                self.assertEqual(report["required_exports"][REQUIRED[0]]["status"], "missing")

    def test_zero_or_unmapped_is_never_direct(self):
        for rva, file_offset, reason in ((0, 0, None), (0x1900, None, None),
                                         (0x3000, 0x100, "outside image")):
            with self.subTest(rva=rva):
                report = inspector.classify_exports([inspector.ExportObservation(
                    b"name", 1, rva, invalid_reason=reason, file_offset=file_offset)])
                self.assertEqual(report["named_exports"][0]["status"], "invalid")

    def test_historical_labels_are_only_two_fixed_rows(self):
        self.assertEqual(inspector.HISTORICAL_TABLE_LABELS, {
            "e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e": "310.8.0",
            "2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f": "310.9.16",
        })

    def test_import_needs_no_pefile_or_bytecode(self):
        code = ("import sys; sys.path.insert(0, sys.argv[1]); "
                "import nr_payload_inspect, test_nr_payload_inspect; "
                "assert not any('pefile' in name for name in sys.modules)")
        result = subprocess.run([sys.executable, "-I", "-S", "-B", "-c", code, str(SCRIPT.parent)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)


class ParsedPETests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pefile = selected_pefile()

    def read(self, raw):
        return inspector.read_pe(raw, self.pefile)

    def test_valid_minimal_pe_and_ordinals(self):
        report = self.read(fixture())
        self.assertEqual(report["architecture"]["format"], "PE32+")
        self.assertEqual(report["architecture"]["machine_name"], "AMD64")
        self.assertEqual(report["export_table"]["declared_names"], 5)
        for index, name in enumerate(REQUIRED):
            row = report["required_exports"][name]
            self.assertEqual((row["status"], row["ordinal"], row["rva"], row["file_offset"]),
                             ("direct-present", 11 + index, 0x1900 + index, 0xB00 + index))
            self.assertTrue(row["in_executable_section"])

    def test_nonexecutable_file_backed_export_is_observation(self):
        row = self.read(fixture(executable=False))["required_exports"][REQUIRED[0]]
        self.assertEqual(row["status"], "direct-present")
        self.assertFalse(row["in_executable_section"])

    def test_missing_optional_exports_are_reported(self):
        report = self.read(fixture())
        self.assertEqual({row["status"] for row in report["optional_exports"].values()}, {"missing"})

    def test_optional_exports_present(self):
        report = self.read(fixture(REQUIRED + OPTIONAL))
        self.assertEqual({row["status"] for row in report["optional_exports"].values()},
                         {"direct-present"})

    def test_absent_export_table_reports_missing(self):
        report = self.read(fixture(export_table=False))
        self.assertFalse(report["export_table"]["present"])
        self.assertEqual(report["named_exports"], [])
        self.assertEqual({row["status"] for row in report["required_exports"].values()}, {"missing"})

    def test_empty_export_table(self):
        self.assertEqual(self.read(fixture(names=()))["named_exports"], [])

    def test_forwarder_is_recorded_without_following(self):
        for forwarder in (b"other.Func", b"other.#123", b"unavailable.Func"):
            with self.subTest(forwarder=forwarder):
                row = self.read(fixture(targets=[forwarder] + [0x1900] * 4))["required_exports"][REQUIRED[0]]
                self.assertEqual(row["status"], "forwarder")
                self.assertEqual(row["forwarder"], forwarder.decode())
                self.assertIsNone(row["file_offset"])
                self.assertFalse(row["in_executable_section"])

    def test_named_zero_rva_stays_visible_and_invalid(self):
        row = self.read(fixture(targets=[0] + [0x1900] * 4))["required_exports"][REQUIRED[0]]
        self.assertEqual(row["rva"], 0)
        self.assertEqual(row["status"], "invalid")

    def test_out_of_image_is_invalid(self):
        for rva in (0x3000, 0xFFFFFFFF):
            with self.subTest(rva=rva):
                row = self.read(fixture(targets=[rva] + [0x1900] * 4))["required_exports"][REQUIRED[0]]
                self.assertEqual(row["status"], "invalid")
                self.assertIn("SizeOfImage", row["invalid_reason"])

    def test_virtual_only_tail_is_invalid(self):
        row = self.read(fixture(targets=[0x2100] + [0x1900] * 4))["required_exports"][REQUIRED[0]]
        self.assertEqual(row["status"], "invalid")
        self.assertIn("file bytes", row["invalid_reason"])

    def test_gap_and_overlay_do_not_count_as_image_bytes(self):
        for raw in (fixture(targets=[0x2500] + [0x1900] * 4),
                    fixture(targets=[0x2500] + [0x1900] * 4) + b"\0" * 0x2000):
            with self.subTest(length=len(raw)):
                row = self.read(raw)["required_exports"][REQUIRED[0]]
                self.assertEqual(row["status"], "invalid")

    def test_last_raw_byte_is_locatable(self):
        row = self.read(fixture(targets=[0x1FFF] + [0x1900] * 4))["required_exports"][REQUIRED[0]]
        self.assertEqual((row["status"], row["file_offset"]), ("direct-present", 0x11FF))

    def test_duplicate_name_different_ordinals_rejected(self):
        for names, targets in ((REQUIRED + (REQUIRED[0],), None),
                               (("unrelated", "unrelated"), [0x1900, 0x1901]),
                               ((REQUIRED[0], REQUIRED[0]), [0, 0x1900])):
            with self.subTest(names=names):
                with self.assertRaisesRegex(inspector.InspectionError, "duplicate"):
                    self.read(fixture(names, targets=targets))

    def test_duplicate_name_same_ordinal_rejected(self):
        raw = change(fixture((REQUIRED[0], REQUIRED[0])), offset(0x10C0) + 2, "<H", 0)
        with self.assertRaisesRegex(inspector.InspectionError, "duplicate"):
            self.read(raw)

    def test_nonascii_names_are_not_required_matches(self):
        name = REQUIRED[0].encode() + b"\xff"
        report = self.read(fixture((name,)))
        self.assertEqual(report["required_exports"][REQUIRED[0]]["status"], "missing")
        self.assertIsNone(report["named_exports"][0]["name"])
        self.assertEqual(report["named_exports"][0]["name_hex"], name.hex())

    def test_invalid_architectures_rejected(self):
        for position, fmt, value in ((0x84, "<H", 0x14C), (0x84, "<H", 0xAA64),
                                      (0x98, "<H", 0x10B), (0x96, "<H", 0x22)):
            with self.subTest(value=value):
                with self.assertRaises(inspector.InspectionError):
                    self.read(change(fixture(), position, fmt, value))

    def test_truncated_headers_and_sections_rejected(self):
        for length in (0, 2, 0x80, 0x100, 0x1A0, 0x200, 0x1100, 0x11FF):
            with self.subTest(length=length):
                with self.assertRaises(inspector.InspectionError):
                    self.read(fixture()[:length])

    def test_inconsistent_headers_rejected(self):
        for position, fmt, value in ((0x86, "<H", 2), (0x94, "<H", 8),
                                      (0x98 + 60, "<I", 0x100), (0x98 + 56, "<I", 0x100)):
            with self.subTest(position=position):
                with self.assertRaises(inspector.InspectionError):
                    self.read(change(fixture(), position, fmt, value))

    def test_broken_export_directory_rejected(self):
        for rva, size in ((0, 0x800), (0x1000, 0), (0x1000, 39), (0x2800, 40),
                          (0x1000, 0x3000)):
            with self.subTest(rva=rva, size=size):
                with self.assertRaises(inspector.InspectionError):
                    self.read(change(fixture(), 0x98 + 112, "<II", rva, size))

    def test_truncated_export_arrays_rejected(self):
        for relative, value in ((20, 0xFFFFFFFF), (24, 0xFFFFFFFF), (28, 0x1FFF),
                                 (32, 0x1FFF), (36, 0x1FFF)):
            with self.subTest(relative=relative):
                with self.assertRaises(inspector.InspectionError):
                    self.read(change(fixture(), offset(0x1000) + relative, "<I", value))

    def test_bad_name_ordinal_rejected(self):
        with self.assertRaises(inspector.InspectionError):
            self.read(change(fixture(), offset(0x10C0), "<H", 5))

    def test_bad_name_pointer_rejected(self):
        for rva in (0, 0x2100, 0x3000):
            with self.subTest(rva=rva):
                with self.assertRaises(inspector.InspectionError):
                    self.read(change(fixture(), offset(0x1080), "<I", rva))

    def test_unterminated_name_rejected(self):
        raw = bytearray(change(fixture(), offset(0x1080), "<I", 0x1FFC))
        raw[-4:] = b"name"
        with self.assertRaisesRegex(inspector.InspectionError, "unterminated"):
            self.read(bytes(raw))

    def test_unterminated_forwarder_rejected_at_directory_end(self):
        raw = bytearray(fixture(targets=[0x17FC] + [0x1900] * 4))
        raw[offset(0x17FC):offset(0x1800)] = b"abcd"
        with self.assertRaisesRegex(inspector.InspectionError, "unterminated"):
            self.read(bytes(raw))

    def test_forwarder_range_end_is_exclusive(self):
        row = self.read(fixture(targets=[0x1800] + [0x1900] * 4))["required_exports"][REQUIRED[0]]
        self.assertEqual(row["status"], "direct-present")
        self.assertIsNone(row["forwarder"])

    def test_header_export_bytes_do_not_certify_executable_code(self):
        row = self.read(fixture(targets=[0x100] + [0x1900] * 4))["required_exports"][REQUIRED[0]]
        self.assertEqual((row["status"], row["file_offset"]), ("direct-present", 0x100))
        self.assertFalse(row["in_executable_section"])


class DependencyLoaderTests(unittest.TestCase):
    def test_explicit_source_identity_and_import_state(self):
        with tempfile.TemporaryDirectory(prefix="nr-payload-loader-") as temporary:
            source = Path(temporary) / "source_fixture.py"
            raw = (b"import sys\n__version__ = 'fixture-version'\n"
                   b"observed_path = list(sys.path)\n"
                   b"observed_bytecode = sys.dont_write_bytecode\n")
            source.write_bytes(raw)
            old_path, old_bytecode = sys.path[:], sys.dont_write_bytecode
            module = inspector.load_pefile_file(source)
            self.assertEqual(module.observed_path, [str(source.parent.resolve()), *old_path])
            self.assertTrue(module.observed_bytecode)
            self.assertEqual(sys.path, old_path)
            self.assertIs(sys.dont_write_bytecode, old_bytecode)
            self.assertEqual(module._nr_payload_identity, {
                "pefile_path": str(source.resolve()),
                "pefile_sha256": hashlib.sha256(raw).hexdigest(),
                "pefile_version": "fixture-version",
            })
            self.assertIs(inspector.load_pefile_file(source), module)
            self.assertFalse((source.parent / "__pycache__").exists())

    def test_failed_import_restores_state_without_bytecode(self):
        with tempfile.TemporaryDirectory(prefix="nr-payload-loader-") as temporary:
            source = Path(temporary) / "broken.py"
            source.write_text("raise RuntimeError('intentional fixture failure')\n", encoding="utf-8")
            old_path, old_bytecode = sys.path[:], sys.dont_write_bytecode
            with self.assertRaisesRegex(inspector.InspectionError, "intentional fixture failure"):
                inspector.load_pefile_file(source)
            self.assertEqual(sys.path, old_path)
            self.assertIs(sys.dont_write_bytecode, old_bytecode)
            self.assertFalse((source.parent / "__pycache__").exists())

    def test_missing_reference_stays_strict_after_explicit_source_loaded(self):
        selected_pefile()
        with tempfile.TemporaryDirectory(prefix="nr-payload-loader-") as temporary:
            with self.assertRaisesRegex(inspector.InspectionError, "reference pefile.py is missing"):
                inspector.load_pefile(Path(temporary))


class CLITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        selected_pefile()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="nr-payload-test-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.input = self.directory / "synthetic.dll"
        self.output = self.directory / "report.json"
        self.raw = fixture()
        self.input.write_bytes(self.raw)
        self.digest = hashlib.sha256(self.raw).hexdigest()

    def invoke(self, *extra, input_path=None, dependency=None):
        args = ["--input", str(input_path or self.input),
                *dependency_cli_args(dependency), *extra]
        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            result = inspector.main(args)
        self.assertEqual(self.input.read_bytes(), self.raw)
        return result, stdout.getvalue(), stderr.getvalue()

    def test_stdout_sha_bytes_unknown_support(self):
        result, stdout, stderr = self.invoke()
        self.assertEqual((result, stderr), (0, ""))
        report = json.loads(stdout)
        self.assertEqual(report["input"], {"path": str(self.input.resolve()),
                         "sha256": self.digest, "bytes": len(self.raw)})
        self.assertIsNone(report["historical_table_label"])
        self.assertEqual(report["support"], "UNKNOWN")
        for name in ("target_executed", "gpu_tested", "abi_authenticated", "support_proven"):
            self.assertIs(report[name], False)
        self.assertFalse(self.output.exists())

    def assert_analyzer_identity(self, report, dependency_mode):
        source = Path(selected_pefile().__file__).resolve()
        self.assertEqual(report["analyzer"], {
            "pefile_path": str(source),
            "pefile_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "pefile_version": selected_pefile().__version__,
            "dependency_mode": dependency_mode,
        })

    def test_report_binds_selected_analyzer_identity(self):
        result, stdout, stderr = self.invoke()
        self.assertEqual((result, stderr), (0, ""))
        self.assert_analyzer_identity(json.loads(stdout), next(iter(dependency_options())).replace("_", "-"))

    def test_explicit_pefile_file_cli_report(self):
        dependency = {"pefile_file": Path(selected_pefile().__file__)}
        result, stdout, stderr = self.invoke(dependency=dependency)
        self.assertEqual((result, stderr), (0, ""))
        self.assert_analyzer_identity(json.loads(stdout), "pefile-file")

    def test_inspect_file_keyword_dependency(self):
        report = inspector.inspect_file(self.input, expected_sha256=self.digest,
                                         pefile_file=Path(selected_pefile().__file__))
        self.assertEqual(report["input"]["sha256"], self.digest)
        self.assert_analyzer_identity(report, "pefile-file")

    def test_reference_root_cli_and_positional_api(self):
        reference = self.directory / "reference"
        parent = reference / "rtx-encore-v2.0.1"
        parent.mkdir(parents=True)
        directory_alias(parent / "python-deps", Path(selected_pefile().__file__).resolve().parent)
        report = inspector.inspect_file(self.input, reference, self.digest)
        self.assert_analyzer_identity(report, "reference-root")
        result, stdout, stderr = self.invoke(dependency={"reference_root": reference})
        self.assertEqual((result, stderr), (0, ""))
        self.assert_analyzer_identity(json.loads(stdout), "reference-root")

    def test_inspect_file_rejects_missing_or_conflicting_dependencies(self):
        for options in ({}, {"reference_root": self.directory,
                             "pefile_file": Path(selected_pefile().__file__)}):
            with self.subTest(options=options):
                with self.assertRaisesRegex(inspector.InspectionError, "exactly one"):
                    inspector.inspect_file(self.input, **options)

    def test_inspect_file_mismatch_precedes_dependency_loading(self):
        for options in ({"reference_root": self.directory},
                        {"pefile_file": self.directory / "absent.py"}):
            with self.subTest(options=options):
                with self.assertRaisesRegex(inspector.InspectionError, "mismatch"):
                    inspector.inspect_file(self.input, expected_sha256="0" * 64, **options)

    def test_missing_explicit_source_has_no_report(self):
        result, stdout, stderr = self.invoke("--output", str(self.output),
            dependency={"pefile_file": self.directory / "absent.py"})
        self.assertEqual((result, stdout), (1, ""))
        self.assertIn("pefile source is missing", stderr)
        self.assertFalse(self.output.exists())

    def test_failed_explicit_import_preserves_existing_report(self):
        source = self.directory / "broken.py"
        source.write_text("raise RuntimeError('fixture import failure')\n", encoding="utf-8")
        self.output.write_bytes(b"unchanged")
        result, stdout, stderr = self.invoke("--output", str(self.output),
                                            dependency={"pefile_file": source})
        self.assertEqual((result, stdout, self.output.read_bytes()), (1, "", b"unchanged"))
        self.assertIn("fixture import failure", stderr)

    def test_cli_rejects_conflicting_dependencies_without_output(self):
        result = subprocess.run([sys.executable, "-B", str(SCRIPT), "--input", str(self.input),
                                 "--reference-root", str(self.directory),
                                 "--pefile-file", str(selected_pefile().__file__),
                                 "--output", str(self.output)],
                                capture_output=True, text=True, check=False)
        self.assertEqual((result.returncode, result.stdout), (2, ""))
        self.assertFalse(self.output.exists())
        self.assertEqual(self.input.read_bytes(), self.raw)

    def test_output_file_and_uppercase_expected_hash(self):
        result, stdout, stderr = self.invoke("--expected-sha256", self.digest.upper(),
                                              "--output", str(self.output))
        self.assertEqual((result, stdout, stderr), (0, "", ""))
        self.assertEqual(json.loads(self.output.read_text(encoding="utf-8"))["input"]["sha256"], self.digest)

    def test_mismatch_never_creates_output(self):
        result, stdout, stderr = self.invoke("--expected-sha256", "0" * 64, "--output", str(self.output))
        self.assertEqual((result, stdout), (1, ""))
        self.assertIn("mismatch", stderr)
        self.assertFalse(self.output.exists())

    def test_mismatch_preserves_existing_output(self):
        self.output.write_bytes(b"unchanged")
        result, stdout, _ = self.invoke("--expected-sha256", "f" * 64, "--output", str(self.output))
        self.assertEqual((result, stdout, self.output.read_bytes()), (1, "", b"unchanged"))

    def test_bad_expected_hash_rejected(self):
        for value in ("", "abc", "g" * 64, "0" * 65):
            with self.subTest(value=value):
                result, stdout, _ = self.invoke("--expected-sha256", value, "--output", str(self.output))
                self.assertEqual((result, stdout), (1, ""))
                self.assertFalse(self.output.exists())

    def test_parse_failure_writes_no_stdout_or_report(self):
        for raw in (b"not PE", fixture()[:-1], fixture(REQUIRED + (REQUIRED[0],)),
                    change(fixture(), 0x84, "<H", 0xAA64)):
            with self.subTest(length=len(raw)):
                self.raw = raw
                self.input.write_bytes(raw)
                result, stdout, _ = self.invoke("--output", str(self.output))
                self.assertEqual((result, stdout), (1, ""))
                self.assertFalse(self.output.exists())

    def test_parse_failure_preserves_existing_report(self):
        self.raw = b"not PE"
        self.input.write_bytes(self.raw)
        self.output.write_bytes(b"previous report")
        result, stdout, _ = self.invoke("--output", str(self.output))
        self.assertEqual((result, stdout, self.output.read_bytes()), (1, "", b"previous report"))

    def test_input_output_same_path_rejected(self):
        result, stdout, stderr = self.invoke("--output", str(self.input))
        self.assertEqual((result, stdout), (1, ""))
        self.assertIn("alias", stderr)

    def test_resolved_path_alias_rejected(self):
        alias = self.directory / "unused" / ".." / self.input.name
        result, stdout, stderr = self.invoke("--output", str(alias))
        self.assertEqual((result, stdout), (1, ""))
        self.assertIn("alias", stderr)

    def test_hardlink_alias_rejected(self):
        os.link(self.input, self.output)
        result, stdout, stderr = self.invoke("--output", str(self.output))
        self.assertEqual((result, stdout), (1, ""))
        self.assertIn("alias", stderr)
        self.assertEqual(self.output.read_bytes(), self.raw)

    def test_input_alias_of_output_rejected(self):
        os.link(self.input, self.output)
        result, stdout, stderr = self.invoke("--output", str(self.input), input_path=self.output)
        self.assertEqual((result, stdout), (1, ""))
        self.assertIn("alias", stderr)

    def test_missing_reference_does_not_fallback_to_installed_library(self):
        result, stdout, stderr = self.invoke("--output", str(self.output),
                                            dependency={"reference_root": self.directory})
        self.assertEqual((result, stdout), (1, ""))
        self.assertIn("reference pefile.py is missing", stderr)
        self.assertFalse(self.output.exists())

    def test_mismatch_is_checked_before_missing_dependency(self):
        for dependency in ({"reference_root": self.directory},
                           {"pefile_file": self.directory / "absent.py"}):
            with self.subTest(dependency=dependency):
                result, stdout, stderr = self.invoke("--expected-sha256", "0" * 64,
                                                    dependency=dependency)
                self.assertEqual((result, stdout), (1, ""))
                self.assertIn("mismatch", stderr)

    def test_missing_input_has_no_output(self):
        result, stdout, _ = self.invoke("--output", str(self.output),
                                       input_path=self.directory / "absent.dll")
        self.assertEqual((result, stdout), (1, ""))
        self.assertFalse(self.output.exists())

    def test_actual_cli_entrypoint(self):
        result = subprocess.run([sys.executable, "-B", str(SCRIPT), "--input", str(self.input),
                                 *dependency_cli_args(),
                                 "--expected-sha256", self.digest],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["input"]["sha256"], self.digest)
        self.assertEqual(self.input.read_bytes(), self.raw)

    def test_cli_requires_explicit_dependency(self):
        result = subprocess.run([sys.executable, "-B", str(SCRIPT), "--input", str(self.input)],
                                capture_output=True, text=True, check=False)
        self.assertEqual((result.returncode, result.stdout), (2, ""))
        self.assertIn("--reference-root", result.stderr)
        self.assertIn("--pefile-file", result.stderr)

    def test_actual_cli_mismatch_has_no_report_or_mutation(self):
        result = subprocess.run([sys.executable, "-B", str(SCRIPT), "--input", str(self.input),
                                 *dependency_cli_args(),
                                 "--expected-sha256", "0" * 64, "--output", str(self.output)],
                                capture_output=True, text=True, check=False)
        self.assertEqual((result.returncode, result.stdout), (1, ""))
        self.assertFalse(self.output.exists())
        self.assertEqual(self.input.read_bytes(), self.raw)

    def test_directory_alias_rejected(self):
        target = self.directory / "target"
        alias = self.directory / "alias"
        target.mkdir()
        self.input = target / self.input.name
        self.input.write_bytes(self.raw)
        directory_alias(alias, target)
        result, stdout, stderr = self.invoke("--output", str(alias / self.input.name))
        self.assertEqual((result, stdout), (1, ""))
        self.assertIn("alias", stderr)

    def copied_dependency(self, mode):
        """All destructive-regression destinations refer only to independent copies."""
        installed = Path(selected_pefile().__file__).resolve()
        reference = self.directory / "copied-reference"
        dependencies = reference / "rtx-encore-v2.0.1" / "python-deps"
        dependencies.mkdir(parents=True)
        source = dependencies / "pefile.py"
        shutil.copyfile(installed, source)
        shutil.copytree(installed.parent / "ordlookup", dependencies / "ordlookup",
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        self.assertFalse(os.path.samefile(installed, source))
        self.assertTrue(source.resolve().is_relative_to(self.directory.resolve()))
        tracked = [installed, self.input, *dependencies.rglob("*.py")]
        hashes = {path: hashlib.sha256(path.read_bytes()).hexdigest() for path in tracked}
        options = ({"reference_root": reference} if mode == "reference-root" else
                   {"pefile_file": source})
        return options, source, hashes

    def dependency_output_alias(self, source, kind):
        if kind == "same-path":
            return source
        if kind == "hardlink":
            os.link(source, self.output)
            return self.output
        alias = self.directory / "dependency-alias"
        directory_alias(alias, source.parent)
        return alias / source.name

    def assert_dependency_hashes(self, hashes):
        for path, before in hashes.items():
            with self.subTest(protected_path=path):
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), before)

    def assert_dependency_alias_rejected_before_inspection(self, mode, kind):
        options, source, hashes = self.copied_dependency(mode)
        output = self.dependency_output_alias(source, kind)
        with patch.object(inspector, "inspect_file", wraps=inspector.inspect_file) as inspect:
            result, stdout, stderr = self.invoke("--output", str(output), dependency=options)
            inspect.assert_not_called()
        self.assertEqual((result, stdout), (1, ""))
        self.assertIn("alias pefile source", stderr)
        self.assert_dependency_hashes(hashes)

    def test_pefile_file_output_same_path_rejected_before_inspection(self):
        self.assert_dependency_alias_rejected_before_inspection("pefile-file", "same-path")

    def test_pefile_file_output_hardlink_rejected_before_inspection(self):
        self.assert_dependency_alias_rejected_before_inspection("pefile-file", "hardlink")

    def test_pefile_file_output_junction_rejected_before_inspection(self):
        self.assert_dependency_alias_rejected_before_inspection("pefile-file", "directory-alias")

    def test_reference_root_output_same_path_rejected_before_inspection(self):
        self.assert_dependency_alias_rejected_before_inspection("reference-root", "same-path")

    def test_reference_root_output_hardlink_rejected_before_inspection(self):
        self.assert_dependency_alias_rejected_before_inspection("reference-root", "hardlink")

    def test_reference_root_output_junction_rejected_before_inspection(self):
        self.assert_dependency_alias_rejected_before_inspection("reference-root", "directory-alias")

    def assert_dependency_alias_rechecked_before_write(self, mode):
        options, source, hashes = self.copied_dependency(mode)
        actual_inspect = inspector.inspect_file

        def inspect_then_create_alias(*args, **kwargs):
            report = actual_inspect(*args, **kwargs)
            os.link(source, self.output)
            return report

        with patch.object(inspector, "inspect_file", side_effect=inspect_then_create_alias) as inspect:
            result, stdout, stderr = self.invoke("--output", str(self.output), dependency=options)
            inspect.assert_called_once()
        self.assertEqual((result, stdout), (1, ""))
        self.assertIn("alias pefile source", stderr)
        self.assert_dependency_hashes(hashes)

    def test_pefile_file_output_rechecked_after_successful_inspection(self):
        self.assert_dependency_alias_rechecked_before_write("pefile-file")

    def test_reference_root_output_rechecked_after_successful_inspection(self):
        self.assert_dependency_alias_rechecked_before_write("reference-root")


if __name__ == "__main__":
    unittest.main()
