"""Offline PE export observations, never a payload execution or support probe.

Only pefile reads PE structures. No DLL loading, ABI inference, or forwarded
export resolution is performed. Importing this module does not import pefile.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from functools import lru_cache
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import sys


REQUIRED_EXPORTS = tuple("NVSDK_NGX_D3D12_" + suffix for suffix in (
    "Init_Ext", "CreateFeature", "EvaluateFeature", "ReleaseFeature", "Shutdown1",
))
OPTIONAL_EXPORTS = tuple("NVSDK_NGX_D3D12_" + suffix for suffix in (
    "AllocateParameters", "GetCapabilityParameters", "DestroyParameters",
    "GetScratchBufferSize",
))
HISTORICAL_TABLE_LABELS = {
    "e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e": "310.8.0",
    "2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f": "310.9.16",
}


class InspectionError(ValueError):
    """Input or dependency cannot produce a complete static observation."""


def _reference_pefile_path(reference_root: Path) -> Path:
    return (Path(reference_root).resolve() / "rtx-encore-v2.0.1" /
            "python-deps" / "pefile.py")


def load_pefile(reference_root: Path):
    """Load the explicit reference dependency, without a global pefile fallback."""
    source = _reference_pefile_path(reference_root)
    if not source.is_file():
        raise InspectionError(f"reference pefile.py is missing: {source}")
    return load_pefile_file(source)


@lru_cache(maxsize=4)
def load_pefile_file(pefile_file: Path):
    """Import an explicit Python source snapshot, without reading/writing pyc."""
    source = Path(pefile_file).resolve()
    if not source.is_file():
        raise InspectionError(f"pefile source is missing: {source}")
    source_bytes = source.read_bytes()
    spec = importlib.util.spec_from_file_location("_nr_payload_pefile", source)
    if spec is None or spec.loader is None:
        raise InspectionError(f"cannot load reference dependency: {source}")
    module = importlib.util.module_from_spec(spec)
    old_path = sys.path[:]
    old_bytecode = sys.dont_write_bytecode
    try:
        sys.path.insert(0, str(source.parent))
        sys.dont_write_bytecode = True
        # Execute the same source bytes whose hash is reported, rather than
        # allowing an existing bytecode cache to substitute another snapshot.
        exec(compile(source_bytes, str(source), "exec"), module.__dict__)
    except Exception as exc:
        raise InspectionError(f"cannot import pefile source {source}: {exc}") from exc
    finally:
        sys.path[:] = old_path
        sys.dont_write_bytecode = old_bytecode
    module._nr_payload_identity = {
        "pefile_path": str(source),
        "pefile_sha256": hashlib.sha256(source_bytes).hexdigest(),
        "pefile_version": getattr(module, "__version__", None),
    }
    return module


@dataclass(frozen=True)
class ExportObservation:
    name: bytes
    ordinal: int
    rva: int
    forwarder: bytes | None = None
    invalid_reason: str | None = None
    file_offset: int | None = None
    in_executable_section: bool = False


def _ascii(value: bytes) -> str | None:
    try:
        return value.decode("ascii")
    except UnicodeDecodeError:
        return None


def classify_exports(exports: list[ExportObservation]) -> dict:
    """Pure classification; presence is an export-table observation only."""
    by_name = {}
    rows = []
    for export in exports:
        if export.name in by_name:
            raise InspectionError(f"duplicate named export: {export.name.hex()}")
        status = ("forwarder" if export.forwarder is not None else
                  "invalid" if export.invalid_reason or export.rva == 0 or
                  export.file_offset is None else "direct-present")
        row = {
            "name": _ascii(export.name), "name_hex": export.name.hex(),
            "ordinal": export.ordinal, "rva": export.rva,
            "forwarder": _ascii(export.forwarder) if export.forwarder is not None else None,
            "forwarder_hex": export.forwarder.hex() if export.forwarder is not None else None,
            "status": status, "invalid_reason": export.invalid_reason,
            "file_offset": export.file_offset,
            "in_executable_section": export.in_executable_section,
        }
        rows.append(row)
        by_name[export.name] = row

    def select(names):
        return {name: by_name.get(name.encode("ascii"), {
            "name": name, "ordinal": None, "rva": None, "forwarder": None,
            "status": "missing",
        }) for name in names}

    return {"named_exports": rows, "required_exports": select(REQUIRED_EXPORTS),
            "optional_exports": select(OPTIONAL_EXPORTS)}


def _file_span(pe, raw: bytes, rva: int, size: int = 1):
    """Validate pefile's mapping against actual headers/section raw bytes.

    pefile may map an RVA to overlay bytes or a section's virtual-only tail.
    Neither is a file-backed image location for this inspector.
    """
    if rva <= 0 or size <= 0 or rva + size > pe.OPTIONAL_HEADER.SizeOfImage:
        raise InspectionError("RVA is zero or outside SizeOfImage")
    sections = [s for s in pe.sections if s.VirtualAddress <= rva <
                s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData)]
    if len(sections) > 1:
        raise InspectionError("RVA has overlapping section mappings")
    if sections:
        section = sections[0]
        delta = rva - section.VirtualAddress
        if delta + size > section.SizeOfRawData:
            raise InspectionError("RVA is not backed by section file bytes")
        offset = section.PointerToRawData + delta
        remaining = min(section.SizeOfRawData - delta,
                        pe.OPTIONAL_HEADER.SizeOfImage - rva)
    else:
        section = None
        if rva + size > pe.OPTIONAL_HEADER.SizeOfHeaders:
            raise InspectionError("RVA is not in headers or a file-backed section")
        offset = rva
        remaining = min(pe.OPTIONAL_HEADER.SizeOfHeaders - rva,
                        pe.OPTIONAL_HEADER.SizeOfImage - rva)
    if offset < 0 or offset + size > len(raw):
        raise InspectionError("RVA extends past input file bytes")
    if pe.get_offset_from_rva(rva) != offset:
        raise InspectionError("ambiguous pefile/raw section mapping")
    return offset, min(remaining, len(raw) - offset), section


def _string(pe, raw: bytes, rva: int, end_rva: int | None = None) -> bytes:
    offset, remaining, _ = _file_span(pe, raw, rva)
    if end_rva is not None:
        remaining = min(remaining, end_rva - rva)
    end = raw.find(b"\0", offset, offset + remaining)
    if end < 0:
        raise InspectionError("unterminated export name or forwarder")
    value = pe.get_string_at_rva(rva, max_length=end - offset + 1)
    if value != raw[offset:end]:
        raise InspectionError("incomplete pefile export string")
    return value


def read_pe(raw: bytes, pefile) -> dict:
    """Use pefile to read headers and declared export tables from original bytes."""
    pe = None
    try:
        pe = pefile.PE(data=raw, fast_load=True)
        fh, oh = pe.FILE_HEADER, pe.OPTIONAL_HEADER
        if fh.Machine != 0x8664 or oh.Magic != 0x20B or not fh.Characteristics & 0x2000:
            raise InspectionError("input must be AMD64 / PE32+ with IMAGE_FILE_DLL")
        section_end = fh.get_file_offset() + fh.sizeof() + fh.SizeOfOptionalHeader + 40 * fh.NumberOfSections
        if (fh.SizeOfOptionalHeader < oh.sizeof() or
                not 0 < oh.SizeOfHeaders <= len(raw) or
                not section_end <= oh.SizeOfHeaders <= oh.SizeOfImage or
                len(pe.sections) != fh.NumberOfSections):
            raise InspectionError("truncated or inconsistent PE headers")
        for section in pe.sections:
            if (section.SizeOfRawData and
                    section.PointerToRawData + section.SizeOfRawData > len(raw)):
                raise InspectionError("truncated section raw bytes")

        exports = []
        table = {"present": False, "rva": 0, "size": 0,
                 "declared_functions": 0, "declared_names": 0}
        directory = oh.DATA_DIRECTORY[0] if oh.DATA_DIRECTORY else None
        if directory and (directory.VirtualAddress or directory.Size):
            start, size = directory.VirtualAddress, directory.Size
            if not start or size < 40:
                raise InspectionError("invalid export directory range")
            _file_span(pe, raw, start, size)
            pe.parse_data_directories(directories=[0])
            parsed = getattr(pe, "DIRECTORY_ENTRY_EXPORT", None)
            if parsed is None:
                raise InspectionError("pefile could not parse export directory")
            header = parsed.struct
            for address, count, width in (
                (header.AddressOfFunctions, header.NumberOfFunctions, 4),
                (header.AddressOfNames, header.NumberOfNames, 4),
                (header.AddressOfNameOrdinals, header.NumberOfNames, 2),
            ):
                if count:
                    _file_span(pe, raw, address, count * width)
            table = {"present": True, "rva": start, "size": size,
                     "declared_functions": header.NumberOfFunctions,
                     "declared_names": header.NumberOfNames}
            # pefile's symbols list omits named entries with RVA 0 and may stop
            # early on malformed names. Read every declared name using pefile's
            # typed accessors, so omitted entries and duplicate names cannot hide.
            for index in range(header.NumberOfNames):
                ordinal_index = pe.get_word_at_rva(header.AddressOfNameOrdinals + 2 * index)
                if ordinal_index is None or ordinal_index >= header.NumberOfFunctions:
                    raise InspectionError("export name ordinal is outside function table")
                name_rva = pe.get_dword_at_rva(header.AddressOfNames + 4 * index)
                name = _string(pe, raw, name_rva)
                if not name:
                    raise InspectionError("empty export name")
                rva = pe.get_dword_at_rva(header.AddressOfFunctions + 4 * ordinal_index)
                if rva is None:
                    raise InspectionError("unreadable export RVA")
                forwarder = None
                invalid = None
                offset = None
                executable = False
                if start <= rva < start + size:
                    forwarder = _string(pe, raw, rva, start + size)
                else:
                    try:
                        offset, _, section = _file_span(pe, raw, rva)
                        executable = bool(section and section.Characteristics & 0x20000000)
                    except InspectionError as exc:
                        invalid = str(exc)
                exports.append(ExportObservation(name, header.Base + ordinal_index,
                    rva, forwarder, invalid, offset, executable))
        return {
            "architecture": {"machine": fh.Machine, "machine_name": "AMD64",
                             "optional_header_magic": oh.Magic, "format": "PE32+",
                             "is_dll": True, "size_of_image": oh.SizeOfImage},
            "export_table": table, **classify_exports(exports),
            "pefile_warnings": pe.get_warnings(),
        }
    except InspectionError:
        raise
    except Exception as exc:
        raise InspectionError(f"PE parse failed: {exc}") from exc
    finally:
        if pe is not None:
            pe.close()


def _check_output(input_path: Path, output_path: Path | None,
                  *dependency_sources: Path) -> None:
    if output_path is None:
        return
    protected = [("input", input_path),
                 *(("pefile source", source) for source in dependency_sources)]
    for label, source in protected:
        if (source.resolve() == output_path.resolve() or
                (source.exists() and output_path.exists() and
                 os.path.samefile(source, output_path))):
            raise InspectionError(f"output must not alias {label} (including hardlinks)")


def inspect_file(input_path: Path, reference_root: Path | None = None,
                 expected_sha256: str | None = None, *,
                 pefile_file: Path | None = None) -> dict:
    """Inspect bytes with one explicit dependency; hash refusal precedes loading."""
    if (reference_root is None) == (pefile_file is None):
        raise InspectionError("supply exactly one of reference_root or pefile_file")
    raw = input_path.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if expected_sha256 is not None:
        if not re.fullmatch(r"[0-9a-fA-F]{64}", expected_sha256):
            raise InspectionError("expected SHA256 must contain exactly 64 hex digits")
        if digest != expected_sha256.lower():
            raise InspectionError(f"expected SHA256 mismatch; input SHA256 is {digest}")
    pefile = (load_pefile(reference_root) if reference_root is not None else
              load_pefile_file(pefile_file))
    observations = read_pe(raw, pefile)
    return {
        "schema_version": 1,
        "input": {"path": str(input_path.resolve()), "sha256": digest, "bytes": len(raw)},
        "analyzer": {**pefile._nr_payload_identity,
                     "dependency_mode": "reference-root" if reference_root is not None else "pefile-file"},
        "historical_table_label": HISTORICAL_TABLE_LABELS.get(digest),
        **observations,
        "target_executed": False, "gpu_tested": False,
        "abi_authenticated": False, "support_proven": False, "support": "UNKNOWN",
        "limitations": [
            "Export presence and executable-section membership do not authenticate functions or ABI.",
            "Forwarders are recorded without resolving or executing their targets.",
            "Historical table labels are hash row matches, not authenticity or version proof.",
            "Missing optional exports and unknown hashes do not establish lack of support.",
        ],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="DLL to read as bytes")
    dependency = parser.add_mutually_exclusive_group(required=True)
    dependency.add_argument("--reference-root", type=Path, help="F7 build/research (strict; no fallback)")
    dependency.add_argument("--pefile-file", type=Path, help="explicit existing pefile Python source")
    parser.add_argument("--expected-sha256")
    parser.add_argument("--output", type=Path, help="JSON destination; default stdout")
    args = parser.parse_args(argv)
    try:
        source = (_reference_pefile_path(args.reference_root)
                  if args.reference_root is not None else args.pefile_file)
        _check_output(args.input, args.output, source)
        report = inspect_file(args.input, args.reference_root, args.expected_sha256,
                              pefile_file=args.pefile_file)
        rendered = json.dumps(report, ensure_ascii=True, indent=2) + "\n"
        if args.output is None:
            sys.stdout.write(rendered)
        else:
            _check_output(args.input, args.output, source,
                          Path(report["analyzer"]["pefile_path"]))
            args.output.write_text(rendered, encoding="utf-8")
    except (InspectionError, OSError, ValueError) as exc:
        print(f"nr_payload_inspect: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
