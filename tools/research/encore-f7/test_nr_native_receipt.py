"""Bounded checks for archived native receipt verification."""

import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from nr_native_receipt import NATIVE_INPUTS, NGX_INPUTS, verify_receipt


class NativeReceiptTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="nr-native-receipt-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.native = self.root / "relocated" / "native"
        self.ngx = self.root / "relocated" / "sdk-headers"
        self.native.mkdir(parents=True)
        self.ngx.mkdir(parents=True)
        self.receipt_path = self.root / "archive" / "result.json"
        self.log_path = self.root / "archive" / "ctest.log"
        self.receipt_path.parent.mkdir()
        self.files = {}
        for name in NATIVE_INPUTS:
            self.files[name] = self.native / name
        for name in NGX_INPUTS:
            self.files[name] = self.ngx / name
        for name, path in self.files.items():
            path.write_bytes(("fixture input: " + name).encode("ascii"))
        self.payload = {
            "checks": 341,
            "failures": 0,
            "inputs": {name: self.digest(path) for name, path in self.files.items()},
            "compiler": {"id": "MSVC", "version": "19.44.35229.0"},
            "pointerBits": 64,
            "payload_called": False,
            "sdk_initialized": False,
            "gpu_tested": False,
        }
        self.receipt = dict(self.payload, schema="f7.nr-parameter-store-cpu.v1",
                            utc="2026-11-07T05:33:05.701869+00:00",
                            buildDirectory=r"Z:\archived\build\native")
        self.write_receipt()
        self.write_log()

    @staticmethod
    def digest(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def write_receipt(self, text=None):
        self.receipt_path.write_text(text if text is not None else
                                     json.dumps(self.receipt), encoding="utf-8")

    def write_log(self, payload=None, *, summary="100% tests passed, 0 tests failed out of 1",
                   passed="1/1 Test #1: NrParameterStoreTests ............   Passed    0.03 sec"):
        record = self.payload if payload is None else payload
        text = ("test 1\n1: " + json.dumps(record, separators=(",", ":")) + "\n" +
                passed + "\n\n" + summary + "\n")
        self.log_path.write_text(text, encoding="utf-8")

    def test_accepts_relocated_matching_inputs_and_reports_scope(self):
        result = verify_receipt(self.receipt_path, self.log_path, self.native, self.ngx)
        self.assertEqual(result["checks"], 341)
        self.assertTrue(result["archivedCtestPassed"])
        self.assertTrue(result["currentHashesMatch"])
        self.assertIsNone(result["compilerExitCode"])
        self.assertFalse(result["recompiled"])
        self.assertFalse(result["reexecuted"])
        self.assertEqual(result["buildDirectory"], self.receipt["buildDirectory"])
        self.assertEqual(result["archivedUtc"], self.receipt["utc"])
        self.assertEqual(result["nativeDirectory"], str(self.native.resolve()))
        self.assertEqual(result["ngxInclude"], str(self.ngx.resolve()))
        self.assertEqual(result["receipt"]["path"], str(self.receipt_path.resolve()))
        self.assertEqual(result["ctestLog"]["path"], str(self.log_path.resolve()))
        self.assertEqual(result["receipt"]["sha256"], self.digest(self.receipt_path))
        self.assertEqual(result["ctestLog"]["sha256"], self.digest(self.log_path))
        self.assertEqual(len(result["inputs"]), 8)
        self.assertNotIn("341 Python", " ".join(result["scope"]))

    def test_rejects_stdout_payload_mismatch(self):
        changed = dict(self.payload, checks=342)
        self.write_log(changed)
        with self.assertRaisesRegex(ValueError, "does not exactly match"):
            verify_receipt(self.receipt_path, self.log_path, self.native, self.ngx)

    def test_archive_digests_hash_parsed_bytes_when_paths_change_after_read(self):
        archive = {path: path.read_bytes()
                   for path in (self.receipt_path, self.log_path)}
        reads = {path: 0 for path in archive}
        read_bytes = Path.read_bytes

        def replace_after_read(path):
            data = read_bytes(path)
            if path in archive:
                reads[path] += 1
                path.write_bytes(b"replaced after the parsed snapshot was read\n")
            return data

        with patch.object(Path, "read_bytes", replace_after_read):
            result = verify_receipt(self.receipt_path, self.log_path, self.native, self.ngx)
        for field, path in (("receipt", self.receipt_path), ("ctestLog", self.log_path)):
            with self.subTest(field=field):
                self.assertEqual(reads[path], 1)
                self.assertEqual(result[field]["sha256"], hashlib.sha256(archive[path]).hexdigest())
                self.assertNotEqual(result[field]["sha256"], self.digest(path))

    def test_stdout_payload_requires_exact_json_types(self):
        mutations = (
            ('"checks":341', '"checks":341.0'),
            ('"failures":0', '"failures":false'),
            ('"pointerBits":64', '"pointerBits":64.0'),
            ('"payload_called":false', '"payload_called":0'),
        )
        for old, new in mutations:
            with self.subTest(mutation=new):
                self.write_log()
                text = self.log_path.read_text(encoding="utf-8")
                self.assertIn(old, text)
                self.log_path.write_text(text.replace(old, new, 1), encoding="utf-8")
                with self.assertRaises(ValueError):
                    verify_receipt(self.receipt_path, self.log_path, self.native, self.ngx)

    def test_rejects_bool_as_integer(self):
        self.receipt["checks"] = True
        self.write_receipt()
        with self.assertRaisesRegex(ValueError, "positive integer"):
            verify_receipt(self.receipt_path, self.log_path, self.native, self.ngx)

    def test_rejects_nonzero_failures_and_false_execution_claims(self):
        for field, value in (("failures", 1), ("gpu_tested", True),
                             ("payload_called", True), ("sdk_initialized", True)):
            with self.subTest(field=field):
                self.setUp_receipt()
                self.receipt[field] = value
                self.write_receipt()
                with self.assertRaises(ValueError):
                    verify_receipt(self.receipt_path, self.log_path, self.native, self.ngx)

    def setUp_receipt(self):
        self.receipt = dict(self.payload, schema="f7.nr-parameter-store-cpu.v1",
                            utc="2026-11-07T05:33:05.701869+00:00",
                            buildDirectory=r"Z:\archived\build\native")

    def test_rejects_drifted_or_missing_ngx_header(self):
        header = self.files["nvsdk_ngx_defs.h"]
        header.write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "hash differs"):
            verify_receipt(self.receipt_path, self.log_path, self.native, self.ngx)
        header.unlink()
        with self.assertRaisesRegex(ValueError, "cannot hash input"):
            verify_receipt(self.receipt_path, self.log_path, self.native, self.ngx)

    def test_rejects_duplicate_json_keys_in_receipt_and_stdout(self):
        self.write_receipt('{"checks":1,"checks":2}')
        with self.assertRaisesRegex(ValueError, "duplicate JSON key"):
            verify_receipt(self.receipt_path, self.log_path, self.native, self.ngx)
        self.write_receipt()
        self.write_log()
        text = self.log_path.read_text(encoding="utf-8")
        self.log_path.write_text(text.replace('"checks":341', '"checks":341,"checks":341', 1),
                                 encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate JSON key"):
            verify_receipt(self.receipt_path, self.log_path, self.native, self.ngx)

    def test_rejects_duplicate_stdout_records_and_nonpassing_ctest(self):
        self.write_log()
        with self.log_path.open("a", encoding="utf-8") as stream:
            stream.write("1: " + json.dumps(self.payload) + "\n")
        with self.assertRaisesRegex(ValueError, "exactly one JSON"):
            verify_receipt(self.receipt_path, self.log_path, self.native, self.ngx)
        self.write_log(summary="0% tests passed, 1 tests failed")
        with self.assertRaisesRegex(ValueError, "1/1 tests passed"):
            verify_receipt(self.receipt_path, self.log_path, self.native, self.ngx)

    def test_rejects_unknown_or_conflicting_numbered_result_rows(self):
        row = "1/1 Test #1: NrParameterStoreTests ............   Passed    0.03 sec"
        for extra in (
            "2/2 Test #2: UnexpectedTest ............   Passed    0.01 sec",
            "2/2 Test #2: Unexpected Test ............   Passed    0.01 sec",
            "2/2 Test #2: Unexpected Test ............ ***Exception 0.01 sec",
            "1/1 Test #1: NrParameterStoreTests ............   Failed    0.03 sec",
        ):
            with self.subTest(extra=extra):
                self.write_log()
                with self.log_path.open("a", encoding="utf-8") as stream:
                    stream.write(extra + "\n")
                with self.assertRaisesRegex(ValueError, "only the expected passing test"):
                    verify_receipt(self.receipt_path, self.log_path, self.native, self.ngx)

    def test_rejects_contradictory_or_duplicate_summary(self):
        for extra in ("0% tests passed, 1 tests failed out of 1",
                      "100% tests passed, 0 tests failed out of 1"):
            with self.subTest(extra=extra):
                self.write_log()
                with self.log_path.open("a", encoding="utf-8") as stream:
                    stream.write(extra + "\n")
                with self.assertRaisesRegex(ValueError, "1/1 tests passed"):
                    verify_receipt(self.receipt_path, self.log_path, self.native, self.ngx)


if __name__ == "__main__":
    unittest.main()
