"""Verify an archived NR parameter-store CPU receipt without running native code."""

from datetime import datetime
import hashlib
import json
from pathlib import Path
import re


NATIVE_INPUTS = (
    "NrParameterStore.h",
    "NrParameterStoreConsumer.cpp",
    "NrParameterStoreConsumer.h",
    "NrParameterStoreTests.cpp",
    "CMakeLists.txt",
    "NativeBuildInputs.h.in",
)
NGX_INPUTS = (
    "nvsdk_ngx_params.h",
    "nvsdk_ngx_defs.h",
)
_INPUTS = NATIVE_INPUTS + NGX_INPUTS
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _read_json(path: Path):
    try:
        data = path.read_bytes()
        parsed = json.loads(data.decode("utf-8"), object_pairs_hook=_unique_object)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read valid JSON from {path}: {error}") from error
    return parsed, hashlib.sha256(data).hexdigest()


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise ValueError(f"cannot hash input {path}: {error}") from error
    return digest.hexdigest()


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def _read_ctest(path: Path):
    try:
        data = path.read_bytes()
        text = data.decode("utf-8")
    except (OSError, UnicodeError) as error:
        raise ValueError(f"cannot read CTest log {path}: {error}") from error

    records = []
    for line in text.splitlines():
        match = re.match(r"\s*(\d+): (\{.*\})\s*\Z", line)
        if match:
            try:
                records.append((int(match.group(1)),
                                json.loads(match.group(2), object_pairs_hook=_unique_object)))
            except (json.JSONDecodeError, ValueError) as error:
                raise ValueError(f"invalid CTest JSON record: {error}") from error
    _require(len(records) == 1, "CTest log must contain exactly one JSON stdout record")
    _require(records[0][0] == 1, "CTest JSON stdout must belong to test #1")

    result_rows = [line for line in text.splitlines()
                   if re.match(r"[ \t]*\d+/\d+ Test #\d+:", line)]
    _require(len(result_rows) == 1 and re.fullmatch(
        r"[ \t]*1/1 Test #1:[ \t]*NrParameterStoreTests[ \t]+\.{3,}[ \t]+"
        r"Passed[ \t]+\d+(?:\.\d+)?[ \t]+sec[ \t\r]*", result_rows[0]) is not None,
             "CTest log must contain only the expected passing test result")
    summaries = [line for line in text.splitlines()
                 if re.match(r"[ \t]*\d+% tests passed,", line)]
    _require(len(summaries) == 1 and summaries[0].strip() ==
             "100% tests passed, 0 tests failed out of 1",
             "CTest log must show exactly 1/1 tests passed and 0 failed")
    return records[0][1], hashlib.sha256(data).hexdigest()


def _validate_payload(payload):
    _require(isinstance(payload, dict), "native stdout payload must be a JSON object")
    _require(type(payload.get("checks")) is int and payload["checks"] > 0,
             "stdout checks must be a positive integer")
    _require(type(payload.get("failures")) is int and payload["failures"] == 0,
             "stdout failures must be integer zero")
    _require(type(payload.get("pointerBits")) is int and payload["pointerBits"] == 64,
             "stdout pointerBits must be integer 64")
    for flag in ("payload_called", "sdk_initialized", "gpu_tested"):
        _require(payload.get(flag) is False, f"stdout {flag} must be exactly false")
    compiler = payload.get("compiler")
    _require(isinstance(compiler, dict) and set(compiler) == {"id", "version"} and
             all(isinstance(compiler[key], str) and compiler[key].strip()
                 for key in ("id", "version")), "stdout compiler id and version must be nonempty strings")
    inputs = payload.get("inputs")
    _require(isinstance(inputs, dict) and set(inputs) == set(_INPUTS),
             "stdout must bind exactly the six native files and two NGX headers")
    for name, digest in inputs.items():
        _require(isinstance(digest, str) and _SHA256.fullmatch(digest) is not None,
                 f"stdout {name} must have a lowercase SHA-256 digest")
    _require(set(payload) == {"checks", "failures", "inputs", "compiler", "pointerBits",
                              "payload_called", "sdk_initialized", "gpu_tested"},
             "stdout payload fields do not match the native fixture schema")


def verify_receipt(receipt_path: Path, ctest_log: Path,
                   native_directory: Path, ngx_include: Path) -> dict:
    """Verify archived fixture evidence and rehash its eight current inputs."""
    receipt_path = Path(receipt_path)
    ctest_log = Path(ctest_log)
    native_directory = Path(native_directory)
    ngx_include = Path(ngx_include)
    receipt, receipt_digest = _read_json(receipt_path)
    _require(isinstance(receipt, dict), "receipt must be a JSON object")

    expected_fields = {
        "checks", "failures", "inputs", "compiler", "pointerBits",
        "payload_called", "sdk_initialized", "gpu_tested", "schema", "utc",
        "buildDirectory",
    }
    _require(set(receipt) == expected_fields, "receipt fields do not match the native fixture schema")
    _require(type(receipt["checks"]) is int and receipt["checks"] > 0,
             "checks must be a positive integer")
    _require(type(receipt["failures"]) is int and receipt["failures"] == 0,
             "failures must be integer zero")
    _require(type(receipt["pointerBits"]) is int and receipt["pointerBits"] == 64,
             "pointerBits must be integer 64")
    _require(receipt["schema"] == "f7.nr-parameter-store-cpu.v1", "unexpected receipt schema")
    for flag in ("payload_called", "sdk_initialized", "gpu_tested"):
        _require(receipt[flag] is False, f"{flag} must be exactly false")
    compiler = receipt["compiler"]
    _require(isinstance(compiler, dict) and set(compiler) == {"id", "version"} and
             all(isinstance(compiler[key], str) and compiler[key].strip()
                 for key in ("id", "version")), "compiler id and version must be nonempty strings")
    _require(isinstance(receipt["buildDirectory"], str) and receipt["buildDirectory"].strip(),
             "buildDirectory must be a nonempty recorded string")
    _require(isinstance(receipt["utc"], str), "utc must be a timestamp string")
    try:
        timestamp = datetime.fromisoformat(receipt["utc"].replace("Z", "+00:00"))
    except ValueError as error:
        raise ValueError("utc must be a valid timezone-aware timestamp") from error
    _require(timestamp.tzinfo is not None and timestamp.utcoffset() is not None,
             "utc must be a valid timezone-aware timestamp")

    payload = {key: receipt[key] for key in (
        "checks", "failures", "inputs", "compiler", "pointerBits",
        "payload_called", "sdk_initialized", "gpu_tested")}
    stdout_payload, ctest_digest = _read_ctest(ctest_log)
    _validate_payload(stdout_payload)
    _require(stdout_payload == payload,
             "CTest stdout JSON does not exactly match the receipt payload")

    recorded = receipt["inputs"]
    _require(isinstance(recorded, dict) and set(recorded) == set(_INPUTS),
             "receipt must bind exactly the six native files and two NGX headers")
    current = {}
    for name in _INPUTS:
        digest = recorded[name]
        _require(isinstance(digest, str) and _SHA256.fullmatch(digest) is not None,
                 f"{name} must have a lowercase SHA-256 digest")
        root = native_directory if name in NATIVE_INPUTS else ngx_include
        actual = _sha256(root / name)
        current[name] = actual
        _require(actual == digest, f"current input hash differs from receipt: {name}")

    return {
        "schema": "f7.nr-parameter-store-verification.v1",
        "receipt": {"path": str(receipt_path.resolve()), "sha256": receipt_digest},
        "ctestLog": {"path": str(ctest_log.resolve()), "sha256": ctest_digest},
        "nativeDirectory": str(native_directory.resolve()),
        "ngxInclude": str(ngx_include.resolve()),
        "archivedUtc": receipt["utc"],
        "buildDirectory": receipt["buildDirectory"],
        "checks": receipt["checks"],
        "failures": receipt["failures"],
        "compiler": compiler,
        "compilerExitCode": None,
        "pointerBits": receipt["pointerBits"],
        "payloadCalled": receipt["payload_called"],
        "sdkInitialized": receipt["sdk_initialized"],
        "gpuTested": receipt["gpu_tested"],
        "inputs": {name: {"receiptSha256": recorded[name], "currentSha256": current[name],
                           "matches": True} for name in _INPUTS},
        "currentHashesMatch": True,
        "archivedCtestPassed": True,
        "recompiled": False,
        "reexecuted": False,
        "scope": [
            "Archived native checks are reported separately from Python test counts.",
            "The verifier rehashed current inputs; it did not compile or execute the recorded binary.",
            "No actual NGX payload ABI, SDK initialization, GPU behavior, or game behavior is established.",
            "Compiler process exit status is unknown; CTest pass is the archived test result.",
        ],
    }
