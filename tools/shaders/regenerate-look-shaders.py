"""Existing Look recipe, F6 source-selected revision (R1046).

Generate DXIL/SPIR-V, reflect the actual Params layout and publish matching
headers using the existing instruction/offset checks and receipt outputs.
Vulkan keeps its legacy global Look; UI-mask capability is DXIL-only.
"""
from pathlib import Path
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import tempfile


# Project root is derived from this file's location: <root>/tools/shaders/<this file>.
PROJECT = Path(__file__).resolve().parents[2]
# Generation targets are pinned to this root; the recipe never writes the old tree.
SOURCE_ROOT = (PROJECT / "OptiScaler").resolve()
EVIDENCE_ROOT = (PROJECT / "build/shaders/evidence").resolve()
BACKUP_ROOT = (PROJECT / "build/shaders/backups").resolve()
PARAMS_SIZE = 312
MASK_OFFSET = 308
INPUT_NAMES = ("dlssnr.hlsl", "dlssnr_pq.hlsli", "dlssnr_replace_curve.hlsli")
LOOK_OFFSETS = dict(zip((
    "gLookMode", "gLookStrength", "gLookBrighten", "gLookDarken", "gLookColour",
    "gLookHue", "gLookShadows", "gLookMidtones", "gLookHighlights", "gLookMaxBrighten",
    "gLookMaxDarken", "gLookMaxColour", "gLookTone", "gLookDetail", "gLookDetailRadius",
    "gLookHalo", "gLookHaloBrightSide",
), range(240, 308, 4)))
PARAM_OFFSETS = {**LOOK_OFFSETS, "gLayerLookUiMask": MASK_OFFSET}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def reachable(start, edges):
    seen, pending = set(), [start]
    while pending:
        block = pending.pop()
        if block not in seen:
            seen.add(block)
            pending.extend(edges.get(block, ()))
    return seen


def ancestors(roots, dependencies):
    return reachable("root", {**dependencies, "root": set(roots)}) - {"root"}


def dominators(entry, edges):
    live = reachable(entry, edges)
    predecessors = {b: set() for b in live}
    for p in live:
        for b in edges.get(p, ()):
            if b in live: predecessors[b].add(p)
    dom = {b: ({entry} if b == entry else set(live)) for b in live}
    changed = True
    while changed:
        changed = False
        for b in live - {entry}:
            incoming = [dom[p] for p in predecessors[b]]
            value = {b} | set.intersection(*incoming)
            if value != dom[b]:
                dom[b], changed = value, True
    return dom


def parse_spirv(path):
    data = path.read_bytes()
    require(len(data) >= 20 and len(data) % 4 == 0, "Truncated SPIR-V")
    words = struct.unpack("<%dI" % (len(data) // 4), data)
    require(words[0] == 0x07230203, "Invalid SPIR-V magic")
    index, instructions = 5, []
    while index < len(words):
        count, opcode = words[index] >> 16, words[index] & 0xFFFF
        require(count > 0 and index + count <= len(words), "Invalid SPIR-V instruction bounds")
        instructions.append((opcode, words[index + 1:index + count]))
        index += count
    return instructions


def sp_string(words):
    return struct.pack("<%dI" % len(words), *words).split(b"\0")[0].decode("utf-8")


def verify_spirv(path):
    """Decode real SPIR-V, follow uniform gMode, not a debug-name/14 search."""
    inst = parse_spirv(path)
    constants, pointers, variables, members, offsets, names, decorations = {}, {}, {}, {}, {}, {}, {}
    structs, scalar_sizes, entry = {}, {}, None
    for op, a in inst:
        if op == 5: names[a[0]] = sp_string(a[1:])
        elif op == 6: members[(a[0], a[1])] = sp_string(a[2:])
        elif op in (21, 22): scalar_sizes[a[0]] = a[1] // 8
        elif op == 30: structs[a[0]] = a[1:]
        elif op == 32: pointers[a[0]] = (a[1], a[2])
        elif op == 43 and len(a) == 3: constants[a[1]] = a[2]
        elif op == 59: variables[a[1]] = (a[0], a[2])
        elif op == 71: decorations[(a[0], a[1])] = a[2:]
        elif op == 72 and len(a) == 4 and a[2] == 35: offsets[(a[0], a[1])] = a[3]
        elif op == 15 and a[0] == 5 and sp_string(a[2:]) == "CSMain": entry = a[1]
    require(entry is not None, "Missing GLCompute CSMain")
    uniform = [(v, pointers[t][1]) for v, (t, storage) in variables.items()
               if storage == 2 and t in pointers and members.get((pointers[t][1], 0)) == "gMode"]
    require(len(uniform) == 1, "Cannot uniquely identify Params uniform")
    cb, owner = uniform[0]
    layout = {members.get((owner, i), str(i)): offsets.get((owner, i)) for i in range(len(structs[owner]))}
    require(layout.get("gMode") == 0, "SPIR-V gMode offset changed")
    for name, offset in PARAM_OFFSETS.items():
        require(layout.get(name) == offset, f"SPIR-V offset changed: {name}")
    require(all(t in scalar_sizes and scalar_sizes[t] == 4 for t in structs[owner]), "Unexpected Params member type")
    size = max(offsets[(owner, i)] + 4 for i in range(len(structs[owner])))
    require(size == PARAMS_SIZE, f"SPIR-V Params size {size}, expected {PARAMS_SIZE}")
    resources = {}
    for name, binding in (("gSource", 1), ("gModel", 2), ("gTarget", 5)):
        found = [v for v, (_, storage) in variables.items() if storage == 0 and names.get(v) == name
                 and decorations.get((v, 33)) == (binding,) and decorations.get((v, 34)) == (0,)]
        require(len(found) == 1, f"Unexpected SPIR-V resource binding: {name}")
        resources[name] = found[0]
    blocks, edges, defs, locations, deps = {}, {}, {}, {}, {}
    function, block, first = None, None, None
    # Result-producing instructions used by the reviewed DXC 1.9 compute recipe.
    typed = {12, 45, 55, 57, 59, 61, 65, 66, 67, 68, 70, 77, 78, 79, 80, 81, 82, 83, 84, 86,
             *range(87, 99), *range(100, 108), *range(109, 216), 245}
    for op, a in inst:
        if op == 54: function = a[1]
        if function == entry:
            if op == 248:
                block = a[0]; blocks[block] = []; edges[block] = set()
                if first is None: first = block
            if block is not None: blocks[block].append((op, a))
            if op in typed:
                result = a[1]; defs[result] = (op, a); locations[result] = block
                refs = a[2:]
                if op == 12: refs = (a[2], *a[4:])  # ExtInst number is a literal.
                elif op == 81: refs = a[2:3]  # CompositeExtract indices are literals.
                elif op == 79: refs = a[2:4]  # VectorShuffle indices are literals.
                elif op == 245: refs = a[2::2]  # Phi values, not predecessor labels.
                deps[result] = set(refs)
            if op == 249: edges[block].add(a[0])
            elif op == 250: edges[block].update(a[1:3])
            elif op == 251: edges[block].update((a[1], *a[3::2]))
        if op == 56: function, block = None, None
    require(first is not None, "Missing SPIR-V entry body")
    loads = {}
    for result, (op, a) in defs.items():
        if op != 61: continue
        access = defs.get(a[2])
        if access and access[0] == 65:
            aa = access[1]
            if aa[2] == cb and len(aa) == 4 and aa[3] in constants:
                member = constants[aa[3]]
                loads[result] = offsets.get((owner, member))
    mode = {v for v, off in loads.items() if off == 0}
    gates = []
    for b, body in blocks.items():
        for op, a in body:
            if op == 250 and a[0] in defs:
                cop, ca = defs[a[0]]
                if cop in (170, 171) and ((ca[2] in mode and constants.get(ca[3]) == 14)
                                                        or (ca[3] in mode and constants.get(ca[2]) == 14)):
                    gates.append((b, a[1] if cop == 170 else a[2], a[2] if cop == 170 else a[1]))
            elif op == 251 and a[0] in mode:
                for i in range(2, len(a), 2):
                    if a[i] == 14:
                        # The false side of a switch includes every other case.
                        other = {a[1], *(a[j + 1] for j in range(2, len(a), 2) if j != i)}
                        gates.append((b, a[i + 1], other))
    require(len(gates) == 1, "SPIR-V lacks unique actual gMode==14 control flow")
    gate, yes, no = gates[0]
    require(gate in reachable(first, edges), "Unreachable SPIR-V Mode14 gate")
    others = no if isinstance(no, set) else {no}
    region = reachable(yes, edges) - set().union(*(reachable(n, edges) for n in others))
    require(yes in region, "SPIR-V Mode14 has no distinct body")
    dom = dominators(first, edges)
    allowed = reachable(yes, edges) | dom[gate]
    roots, image_reads, writes = set(), [], 0
    yes_blocks = reachable(yes, edges)
    for b in allowed:
        for op, a in blocks[b]:
            if op == 99 and b in yes_blocks:  # OpImageWrite, verify the actual image lineage.
                lineage = ancestors(a[:3], deps)
                if resources["gTarget"] in lineage:
                    writes += 1; roots.update(a[:3])
            elif op in (87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98):
                lineage = ancestors(a[2:3], deps)
                image_reads.append((a[1], {name for name in ("gSource", "gModel") if resources[name] in lineage}))
            elif op == 250 and b in region: roots.add(a[0])
    used = ancestors(roots, deps)
    reads = set().union(*(names for result, names in image_reads if result in used))
    consumed = sorted({off for result, off in loads.items() if result in used and locations[result] in allowed
                       and off in LOOK_OFFSETS.values()})
    require(writes > 0 and reads == {"gSource", "gModel"}, "Mode14 SPIR-V missing real source/model reads or target write")
    require(consumed == list(LOOK_OFFSETS.values()), f"Mode14 SPIR-V parameter consumption incomplete: {consumed}")
    return {"status": "PASS", "method": "binary instructions, uniform access, CFG and data/control use",
            "modeGateBlock": gate, "mode14EntryBlock": yes, "mode14ExclusiveBlocks": len(region),
            "lookConsumedOffsets": consumed, "resources": resources, "targetWrites": writes,
            "offsets": layout, "paramsSize": size, "vulkanLayerRuntime": "DISABLED_BY_HOST_CONTRACT"}


def verify_dxil(text):
    """Analyze dumpbin of the exact stripped production DXIL container."""
    main = re.search(r"define void @CSMain\([^\n]*\)\s*\{(.*?)^\}", text, re.S | re.M)
    require(main is not None, "Missing DXIL CSMain")
    require(re.search(r"\[312 x i8\]", text), "Actual DXIL Params size is not 312")
    blocks, edges, defs, locations, deps = {"entry": []}, {"entry": set()}, {}, {}, {}
    block = "entry"
    for line in main[1].splitlines():
        label = re.match(r"\s*(?:;\s*<label>:(\w+)|([-\w.]+):)", line)
        if label:
            block = label[1] or label[2]; blocks[block] = []; edges[block] = set()
        code = line.split(";", 1)[0].strip()
        if not code: continue
        blocks[block].append(code)
        assignment = re.match(r"(%[-\w.]+) = (.*)", code)
        if assignment:
            value, expr = assignment.groups(); defs[value] = expr; locations[value] = block
            deps[value] = set(re.findall(r"%[-\w.]+", expr))
        if code.startswith("br "): edges[block].update(re.findall(r"label %([-\w.]+)", code))
        elif code.startswith("switch "):
            raise RuntimeError("DXIL switch lowering needs explicit reviewed CFG parser support; headers unchanged")
    handles, cbregs, loads = {}, {}, {}
    for value, expr in defs.items():
        m = re.search(r"@dx.op.createHandle\(i32 57, i8 (\d+), i32 (\d+), i32 (\d+),", expr)
        if m: handles[value] = tuple(map(int, m.groups()))
    for value, expr in defs.items():
        m = re.search(r"@dx.op.cbufferLoadLegacy\.(?:i32|f32)\(i32 59, %dx.types.Handle (%[-\w.]+), i32 (\d+)\)", expr)
        if m and handles.get(m[1]) == (2, 0, 0): cbregs[value] = int(m[2])
    for value, expr in defs.items():
        m = re.match(r"extractvalue %dx.types.CBufRet\.(?:i32|f32) (%[-\w.]+), (\d+)$", expr)
        if m and m[1] in cbregs: loads[value] = cbregs[m[1]] * 16 + int(m[2]) * 4
    mode = {v for v, off in loads.items() if off == 0}
    gates = []
    for b, body in blocks.items():
        for code in body:
            m = re.match(r"br i1 (%[-\w.]+), label %([-\w.]+), label %([-\w.]+)", code)
            if not m: continue
            eq = re.fullmatch(r"icmp (eq|ne) i32 (%[-\w.]+), 14", defs.get(m[1], ""))
            if eq and eq[2] in mode:
                gates.append((b, m[2] if eq[1] == "eq" else m[3], m[3] if eq[1] == "eq" else m[2]))
    require(len(gates) == 1, "DXIL lacks unique actual gMode==14 control flow")
    gate, yes, no = gates[0]
    require(gate in reachable("entry", edges), "Unreachable DXIL Mode14 gate")
    region = reachable(yes, edges) - reachable(no, edges)
    require(yes in region, "DXIL Mode14 has no distinct body")
    dom = dominators("entry", edges)
    allowed = reachable(yes, edges) | dom[gate]
    roots, image_reads, writes = set(), [], 0
    yes_blocks = reachable(yes, edges)
    for b in allowed:
        for code in blocks[b]:
            m = re.search(r"@dx.op.textureStore\.f32\(i32 67, %dx.types.Handle (%[-\w.]+),", code)
            if m and b in yes_blocks and handles.get(m[1]) == (1, 0, 0):
                writes += 1; roots.update(re.findall(r"%[-\w.]+", code))
            m = re.search(r"@dx.op.(?:textureLoad|sampleLevel)\.f32\(i32 \d+, %dx.types.Handle (%[-\w.]+),", code)
            if m and handles.get(m[1]) in ((0, 0, 0), (0, 1, 1), (0, 4, 4)):
                result = re.match(r"(%[-\w.]+) =", code)
                if result: image_reads.append((result[1], handles[m[1]][1]))
            m = re.match(r"br i1 (%[-\w.]+),", code)
            if m and b in region: roots.add(m[1])
    used = ancestors(roots, deps)
    reads = {resource for result, resource in image_reads if result in used}
    consumed = sorted({off for result, off in loads.items() if result in used and locations[result] in allowed
                       and off in LOOK_OFFSETS.values()})
    require(writes > 0 and reads == {0, 1, 4}, "Mode14 DXIL missing actual t0/t1/t4 reads or u0 write")
    require(any(off == MASK_OFFSET and result in used and locations[result] in allowed
                for result, off in loads.items()), "Mode14 DXIL does not consume mask policy at offset 308")
    require(consumed == list(LOOK_OFFSETS.values()), f"Mode14 DXIL parameter consumption incomplete: {consumed}")
    return {"status": "PASS", "method": "actual dumpbin SSA, CB0.gMode, CFG and data/control use",
            "modeGateBlock": gate, "mode14EntryBlock": yes, "mode14ExclusiveBlocks": len(region),
            "lookConsumedOffsets": consumed, "srvReads": sorted(reads), "uav0Writes": writes, "paramsSize": PARAMS_SIZE,
            "layerUiMaskOffset": MASK_OFFSET, "layerUiMaskSrv": 4}


def spirv_offsets(path):
    names, decorations = {}, []
    for opcode, operands in parse_spirv(path):
        if opcode == 6:
            names[operands[:2]] = sp_string(operands[2:])
        if opcode == 72 and len(operands) >= 4 and operands[2] == 35:
            decorations.append((operands[0], operands[1], operands[3]))
    return {names.get((owner, member), f"{owner}:{member}"): offset for owner, member, offset in decorations}


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True,
                        help="absolute path to this project's OptiScaler source root (the only accepted target)")
    parser.add_argument("--evidence", type=Path, required=True,
                        help="new directory inside build/shaders/evidence")
    parser.add_argument("--cleanup", type=Path, required=True,
                        help="new directory inside build/shaders/backups")
    parser.add_argument(
        "--dxc",
        type=Path,
        metavar="ABSOLUTE_PATH",
        help="absolute path to dxc.exe; defaults to the source-local compiler",
    )
    return parser.parse_args()


def resolve_dxc(requested, opti):
    if requested is None:
        dxc = opti / "shaders/shader_tools/dxc.exe"
    else:
        if not requested.is_absolute():
            raise RuntimeError("--dxc must be an absolute path to dxc.exe")
        dxc = requested
    dxc = dxc.resolve()
    require(dxc.name.lower() == "dxc.exe", "Compiler must be the verified dxc.exe")
    if not dxc.is_file():
        raise RuntimeError(f"Missing compiler: {dxc}")
    for companion in ("dxcompiler.dll", "dxil.dll"):
        path = dxc.parent / companion
        if not path.is_file():
            raise RuntimeError(f"Missing DXC companion DLL: {path}")
    return dxc


def extract_header(path, symbol):
    text = path.read_text(encoding="utf-8-sig")
    body = re.search(r"\b" + re.escape(symbol) + r"\s*\[\s*\]\s*=\s*\{([^}]+)\}", text, re.S)
    require(body is not None, f"Missing byte array: {path}:{symbol}")
    require(not re.sub(r"0x[0-9a-fA-F]{2}|[\s,]", "", body[1]), f"Unexpected byte array syntax: {path}")
    return bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})", body[1]))


def check_dxil_container(path):
    data = path.read_bytes()
    require(len(data) >= 32 and data[:4] == b"DXBC", "Missing actual DXIL container")
    size, count = struct.unpack_from("<II", data, 24)
    require(size == len(data) and count <= (len(data) - 32) // 4, "Invalid DXIL container directory")
    parts = []
    for offset in struct.unpack_from("<%dI" % count, data, 32):
        require(offset >= 32 + count * 4 and offset + 8 <= len(data), "Invalid DXIL part offset")
        tag, length = struct.unpack_from("<4sI", data, offset)
        require(offset + 8 + length <= len(data), "Truncated DXIL part")
        parts.append(tag)
    require(parts.count(b"DXIL") == 1, "Container lacks unique DXIL part")


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def verify_inputs(shader_dir):
    # Keep the existing local-include contract; current bytes are the inputs,
    # not the historical frozen-F5 content identities.
    for name in INPUT_NAMES:
        source = shader_dir / name
        require(source.is_file(), f"Missing recipe input: {source}")
        includes = re.findall(r'^\s*#\s*include\s*[<"]([^">]+)[">]',
                              source.read_text(encoding="utf-8-sig"), re.M)
        require(all(i in INPUT_NAMES for i in includes), f"Unsupported recipe include: {name}")


def emit_header(binary, generated, target, symbol):
    # Preserve the old header's BOM and newline policy without copying old bytes.
    old = target.read_bytes()
    newline = "\r\n" if b"\r\n" in old else "\n"
    require(not (b"\r\n" in old and old.count(b"\n") != old.count(b"\r\n")), "Mixed header newline policy")
    lines = ["#pragma once", "#define DLSSNR_LOOK_SHADER_ABI 20261018",
             "#define DLSSNR_LAYER_LOOK_SHADER_ABI 20261020"]
    if symbol == "DlssNr_cso":
        lines.append("#define DLSSNR_LAYER_UI_MASK_SHADER_ABI 20261104")
    lines += ["", "// Generated by the existing Look recipe from the selected F6 source.",
              "// Params: 312 bytes; original Look offsets unchanged; UI-mask scalar at 308.",
              "// DXIL mode14: real t0/t1/t4 reader. Vulkan layer runtime remains disabled.",
              f"inline static const unsigned char {symbol}[] = {{"]
    data = binary.read_bytes()
    for offset in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{value:02x}" for value in data[offset:offset + 16]) + ",")
    lines.append("};")
    encoded = (newline.join(lines) + newline).encode("utf-8")
    generated.write_bytes((b"\xef\xbb\xbf" if old.startswith(b"\xef\xbb\xbf") else b"") + encoded)
    require(extract_header(generated, symbol) == data, f"Generated header payload round trip failed: {generated}")


def replace_headers(headers, cleanup):
    backed_up, replacements, temporary = [], [], []
    try:
        for generated, target in headers:
            backup = cleanup / target.name if target.exists() else None
            if backup is not None and backup.exists():
                raise RuntimeError(f"Existing holding file: {backup}")
            with tempfile.NamedTemporaryFile(
                prefix=f".{target.name}.", suffix=".tmp", dir=target.parent, delete=False
            ) as handle:
                temp = Path(handle.name)
            temporary.append(temp)
            shutil.copy2(generated, temp)
            if generated.read_bytes() != temp.read_bytes():
                raise RuntimeError(f"Temporary header copy mismatch: {target}")
            if backup is not None:
                old_bytes = target.read_bytes()
                target.rename(backup)
                backed_up.append((backup, target))
                require(backup.read_bytes() == old_bytes, f"Backup copy mismatch: {backup}")
            os.replace(temp, target)
            temporary.remove(temp)
            replacements.append({
                "source": str(generated),
                "destination": str(target),
                "backup": str(backup) if backup is not None else None,
                "sourceBytes": generated.stat().st_size,
                "destinationBytes": target.stat().st_size,
            })
    except Exception:
        for temp in temporary:
            if temp.exists():
                temp.unlink()
        for generated, target in reversed(headers):
            if target.exists() and any(item[1] == target for item in backed_up):
                partial = cleanup / (target.name + ".partial")
                if partial.exists():
                    raise RuntimeError(f"Existing partial holding file: {partial}")
                target.rename(partial)
        for backup, target in reversed(backed_up):
            backup.rename(target)
        raise
    return replacements


def require_output_dir(path, root, label):
    """Output must be a fresh directory strictly inside root; never root itself."""
    require(path != root, f"{label} cannot be the output root itself: {root}")
    require(path.is_relative_to(root), f"{label} must be inside {root}")
    require(not path.exists(), f"Use a new {label.lower()} directory; no overwrite: {path}")


def main():
    args = parse_args()
    require(all(p.is_absolute() for p in (args.source, args.evidence, args.cleanup)), "All paths must be absolute")
    opti = args.source.resolve()
    require(opti.is_dir(), f"Missing selected source root: {opti}")
    require(opti == SOURCE_ROOT, f"--source must be the project source root: {SOURCE_ROOT}")
    evidence = args.evidence.resolve()
    cleanup = args.cleanup.resolve()
    shader = opti / "shaders/dlssnr/precompile/dlssnr.hlsl"
    require_output_dir(evidence, EVIDENCE_ROOT, "--evidence")
    require_output_dir(cleanup, BACKUP_ROOT, "--cleanup")
    require(not evidence.is_relative_to(cleanup) and not cleanup.is_relative_to(evidence),
            "Evidence and cleanup directories must not overlap")
    require(not evidence.exists() and not cleanup.exists(), "Use new evidence/cleanup directories; no overwrite")
    verify_inputs(shader.parent)
    source_inputs = {name: (shader.parent / name).read_bytes() for name in INPUT_NAMES}
    dxc = resolve_dxc(args.dxc, opti)
    original_headers = {shader.parent / name: (shader.parent / name).read_bytes()
                        for name in ("DlssNr_Shader.h", "DlssNr_Shader_Vk.h")}
    script_bytes = Path(__file__).read_bytes()
    evidence.mkdir(parents=True)
    cleanup.mkdir(parents=True)
    inputs = evidence / "inputs"
    inputs.mkdir()
    for name in INPUT_NAMES:
        shutil.copy2(shader.parent / name, inputs / name)
    verify_inputs(inputs)
    compiled_shader = inputs / shader.name
    products, commands = [], []
    def run(command, log_name):
        with (evidence / log_name).open("wb") as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, cwd=inputs)
        commands.append({"argv": command, "exitCode": result.returncode, "log": str(evidence / log_name),
                         "logBytes": (evidence / log_name).stat().st_size})
        require(result.returncode == 0, f"{log_name}: exit {result.returncode}; headers not published")
    for backend, flags, filename, symbol in [
        ("dxil", ["-Qstrip_reflect"], "DlssNr_Shader.h", "DlssNr_cso"),
        ("spirv", ["-spirv", "-D", "VK_MODE", "-Cc", "-Vi"], "DlssNr_Shader_Vk.h", "dlssnr_spv"),
    ]:
        binary = evidence / ("dlssnr." + ("cso" if backend == "dxil" else "spv"))
        command = [str(dxc), "-T", "cs_6_0", "-E", "CSMain", "-O3", "-Qstrip_debug",
                   "-I", str(inputs), *flags, str(compiled_shader), "-Fo", str(binary)]
        run(command, backend + ".log")
        products.append((backend, binary, evidence / filename, shader.parent / filename, symbol))
    check_dxil_container(evidence / "dlssnr.cso")
    run([str(dxc), "-dumpbin", str(evidence / "dlssnr.cso")], "dxil-dumpbin.txt")
    dxil_proof = verify_dxil((evidence / "dxil-dumpbin.txt").read_text(encoding="utf-8-sig"))
    spirv_proof = verify_spirv(evidence / "dlssnr.spv")
    write_json(evidence / "spirv-offsets.json", spirv_proof["offsets"])
    reflection_command = [str(dxc), "-T", "cs_6_0", "-E", "CSMain", "-O3", "-I", str(shader.parent),
                          str(compiled_shader), "-Qstrip_debug", "-Fo", str(evidence / "reflection.cso"),
                          "-Fc", str(evidence / "reflection.txt")]
    reflection_command[reflection_command.index("-I") + 1] = str(inputs)
    run(reflection_command, "reflection.log")
    reflection = (evidence / "reflection.txt").read_text(encoding="utf-8-sig")
    for name, expected in PARAM_OFFSETS.items():
        if not re.search(r"\b" + name + r"\s*;\s*;\s*Offset:\s*" + str(expected) + r"\b", reflection):
            raise RuntimeError(f"DXIL reflection missing expected offset {name}={expected}")
    require(re.search(r"\}\s*Params;\s*;\s*Offset:\s*0\s+Size:\s*312\b", reflection),
            "DXIL reflection Params size is not 312")
    verify_inputs(inputs)
    verify_inputs(shader.parent)
    for name, expected in source_inputs.items():
        require((inputs / name).read_bytes() == expected and (shader.parent / name).read_bytes() == expected,
                f"Selected recipe input changed during compilation: {name}")
    require(Path(__file__).read_bytes() == script_bytes, "Generator changed during compilation")
    for target, expected in original_headers.items():
        require(target.read_bytes() == expected, f"Active header changed concurrently: {target}")
    proof = {"status": "PASS", "dxil": dxil_proof, "spirv": spirv_proof,
             "reflectionParamsSize": PARAMS_SIZE, "globalLookAbi": 20261018, "layerLookAbi": 20261020,
             "layerUiMaskDxilAbi": 20261104, "vulkanLayerRuntime": "DISABLED_BY_HOST_CONTRACT",
             "scope": "compiled instruction capability; no GPU execution or visual acceptance"}
    write_json(evidence / "mode14-proof.json", proof)
    # The layer marker is first written here, after BOTH actual bytecode proofs.
    payloads, headers = {}, []
    for backend, binary, generated, target, symbol in products:
        emit_header(binary, generated, target, symbol)
        payloads[backend] = {"binary": str(binary),
                             "header": str(generated),
                             "embeddedLength": binary.stat().st_size, "headerRoundTrip": "PASS"}
        headers.append((generated, target))
    receipt = {"status": "VALIDATED_NOT_INSTALLED", "sourceRoot": str(opti),
               "shader": str(shader), "inputs": list(INPUT_NAMES),
               "generator": str(Path(__file__).resolve()),
               "compiler": str(dxc), "commands": commands,
               "preimageHeaderBytes": {str(p): len(b) for p, b in original_headers.items()},
               "proof": str(evidence / "mode14-proof.json"),
               "payloads": payloads, "globalLookAbi": 20261018, "layerLookAbi": 20261020,
               "layerUiMaskDxilAbi": 20261104, "vulkanLayerRuntime": "DISABLED_BY_HOST_CONTRACT",
               "paramsSize": PARAMS_SIZE, "consumerCbAllocation": 512, "lookOffsets": PARAM_OFFSETS,
               "targetedCompile": "NOT_RUN", "fullBuild": "NOT_RUN",
               "dllBuild": "NOT_RUN", "deployed": False, "runtimeVerified": False}
    write_json(evidence / "shader-receipt.json", receipt)
    for target, expected in original_headers.items():
        require(target.read_bytes() == expected, f"Active header changed before replacement: {target}")
    receipt["replacements"] = replace_headers(headers, cleanup)
    receipt["status"] = "SHADERS_GENERATED_ONLY"
    write_json(evidence / "shader-receipt.json", receipt)
    print("Fresh DXIL/SPIR-V Mode14 verified and embedded. TU/full build/deployment/runtime remain pending.")


if __name__ == "__main__":
    main()
