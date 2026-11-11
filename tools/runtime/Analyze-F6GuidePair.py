#!/usr/bin/env python3
"""Inspect f6.guide-pair.v1; stdlib JSON report and optional PPM error maps.

Scores/maps reuse the production GuideCalibration pixel sampler and score formula,
with same-generation offline admission and explicit exploratory phase hypotheses.
This reader never substitutes a second optical-flow/scoring implementation.
No wizard, automatic config, GPU ownership or retirement authority.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import sys

SCHEMA = "f6.guide-pair.v1"
METRICS = ("score", "reprojection", "depthConsistency", "edgeAlignment",
           "coverage", "occlusionFraction", "outOfBoundsFraction",
           "motionX", "motionY", "depthEdgeEvidence")
COUNTS = ("eligible", "depthSamples", "motionSamples", "compared", "occluded", "outOfBounds")
VIEWS = ("warped", "residual", "depthContribution")
NONFINITE = ("NaN", "+Infinity", "-Infinity")
ALGORITHM = "GuidePairExport::Compare/GuideCalibration::Detail::ScoreCandidate/Observe"


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def integer(value):
    return type(value) is int and value >= 0


def finite(value):
    return type(value) in (int, float) and math.isfinite(value)


def raw_number(value):
    return finite(value) or (type(value) is str and value in NONFINITE)


def sha(value):
    return (isinstance(value, str) and len(value) == 64
            and all(c in "0123456789abcdefABCDEF" for c in value))


def vectors(array, n, channels, name, raw=False):
    require(isinstance(array, list) and len(array) == n, name + " array length")
    check = raw_number if raw else finite
    for pixel in array:
        require(isinstance(pixel, list) and len(pixel) == channels
                and all(check(v) for v in pixel), name + " pixel channels/numbers")


def check_frame(frame):
    require(isinstance(frame, dict), "frame must be an object")
    w, h = frame["width"], frame["height"]
    require(integer(w) and integer(h) and 3 <= w <= 160 and 3 <= h <= 100, "reduced grid bounds")
    n = w * h
    for key in ("valid", "colourPresent", "depthPresent", "motionPresent"):
        require(frame[key] is True, key + " must be observed")
    require(type(frame["contributionPresent"]) is bool, "contribution presence")
    require(integer(frame["stream"]) and frame["stream"] < 4, "stream")
    vectors(frame["colour"], n, 4, "raw colour", raw=True)
    vectors(frame["motion"], n, 2, "raw motion", raw=True)
    require(len(frame["depth"]) == n and all(raw_number(v) for v in frame["depth"]), "raw depth")
    contribution = frame["contribution"]
    require(isinstance(contribution, list) and len(contribution) in (0, n)
            and all(raw_number(v) for v in contribution), "contribution pixels")
    require(not frame["contributionPresent"] or len(contribution) == n, "contribution missing")
    for key in ("motionToUvX", "motionToUvY"):
        require(finite(frame[key]) and frame[key] != 0, "finite nonzero " + key)
    stamp = frame["stamp"]
    for key in ("source", "epoch", "feature", "configuration", "owner", "tick"):
        require(integer(stamp[key]) and stamp[key] > 0, "missing stamp " + key)
    require(stamp["stage"] == 5 and integer(frame["completedTick"])
            and frame["completedTick"] >= stamp["tick"], "completion stamp")
    for key in ("depthFlip", "motionFlip", "yNegated", "phaseVerified",
                "addressPhaseKnown", "motionJitterKnown"):
        require(type(stamp[key]) is bool, "missing direction/phase " + key)
    require(isinstance(stamp["fields"], list) and len(stamp["fields"]) == 14, "stamp provenance fields")
    return stamp


def check_view(view, w, h):
    require(isinstance(view, dict), "image object")
    require((view["width"], view["height"]) in ((0, 0), (w, h)), "image dimensions")
    n = view["width"] * view["height"]
    vectors(view["pixels"], n, 3, "diagnostic image")
    require(len(view["mask"]) == n and all(type(v) is int and v in (0, 1) for v in view["mask"]), "image mask")
    require(all(0 <= c <= 1 for p, valid in zip(view["pixels"], view["mask"]) if valid for c in p), "image normalized range")


def summarize_candidates(comparison):
    require(comparison["algorithm"] == ALGORITHM, "unknown scoring algorithm")
    require(comparison["mode"] in ("rejected", "exploratory", "phase-verified observation"), "comparison mode")
    require(type(comparison["numericAvailable"]) is bool and type(comparison["phaseKnown"]) is bool, "evidence status")
    require(isinstance(comparison["assumptions"], str), "explicit coordinate assumptions required")
    require(type(comparison["valid"]) is bool and comparison["autoApply"] is False, "comparison authority")
    candidates = comparison["candidates"]
    require(isinstance(candidates, list) and len(candidates) == 8, "exactly eight candidates required")
    rows = []
    for index, candidate in enumerate(candidates):
        require(candidate["index"] == index, "candidate order/index")
        for name, bit in (("depthRows", 1), ("motionRows", 2), ("vectorY", 4)):
            require(type(candidate[name]) is bool and candidate[name] == bool(index & bit), "candidate direction flags")
        require(type(candidate["valid"]) is bool and integer(candidate["rejectionCode"]), "candidate status")
        require(isinstance(candidate["reason"], str), "candidate rejection reason")
        metrics = candidate["metrics"]
        require(all(integer(metrics[k]) for k in COUNTS), "metric counters")
        require(all(finite(metrics[k]) and metrics[k] >= 0 for k in METRICS), "nonfinite or negative metrics")
        require(all(metrics[k] <= 1 for k in ("score", "reprojection", "depthConsistency",
                "edgeAlignment", "coverage", "occlusionFraction", "outOfBoundsFraction", "depthEdgeEvidence")), "metric range")
        require(metrics["compared"] <= metrics["eligible"] and metrics["occluded"] <= metrics["compared"], "metric count consistency")
        if candidate["valid"]:
            require(comparison["valid"] and comparison["phaseKnown"] and candidate["rejectionCode"] == 0 and metrics["compared"] >= 32, "invalid claimed numeric evidence")
        rows.append({k: candidate[k] for k in ("index", "depthRows", "motionRows", "vectorY", "valid", "reason")})
        # Zero-filled metrics on metadata refusal are unavailable, not zero error.
        rows[-1]["metrics"] = metrics if metrics["eligible"] else None
    ranked = sorted((r for r in rows if r["valid"]), key=lambda r: r["metrics"]["score"])
    best = None
    ambiguity = "no valid candidate evidence"
    if len(ranked) >= 2:
        a, b = ranked[0]["metrics"]["score"], ranked[1]["metrics"]["score"]
        gap = (b-a)/max(b, 1e-9) if b-a > 1e-6 else 0
        if gap >= .15:
            best = {"index": ranked[0]["index"], "relativeGap": gap}
            ambiguity = "single-pair numeric ranking only; no direction recommendation"
        else:
            ambiguity = "tie or relative score gap below 15 percent"
    return rows, best, ambiguity


def analyze(document):
    require(document["schema"] == SCHEMA, "unsupported schema")
    identity = document["identity"]
    require(integer(identity["pid"]) and identity["pid"] > 0, "explicit PID required")
    require(identity["diskDllSHA"] == "" or sha(identity["diskDllSHA"]), "disk DLL SHA256")
    require(type(identity["loadedMemoryKnown"]) is bool, "loaded-memory identity status")
    require((identity["loadedMemoryKnown"] and sha(identity["loadedMemorySHA"]))
            or (not identity["loadedMemoryKnown"] and identity["loadedMemorySHA"] is None), "disk hash cannot establish loaded memory")
    frames = document["frames"]
    require(isinstance(frames, list) and len(frames) == 2, "exactly two frames")
    a, b = (check_frame(frame) for frame in frames)
    for key in ("feature", "epoch", "configuration", "owner"):
        require(a[key] == b[key], key + " identity changed")
    require(b["source"] - a["source"] == 1 and b["tick"] > a["tick"], "nonconsecutive sources/timestamps")
    require(all(frames[0][k] == frames[1][k] for k in ("stream", "width", "height")), "stream/grid changed")
    comparison = document["comparison"]
    rows, best, ambiguity = summarize_candidates(comparison)
    # Report the production scorer's refusal as written. Do not repair timestamps,
    # epochs or metadata to obtain numeric evidence during offline analysis.
    if comparison["valid"]:
        require(all(s["phaseVerified"] and s["addressPhaseKnown"] for s in (a, b)),
                "numeric score requires observed colour/guide phase")
        require(comparison["mode"] == "phase-verified observation", "exploratory scores cannot authorize ranking")
    for candidate in comparison["candidates"]:
        for name in VIEWS:
            check_view(candidate["images"][name], frames[1]["width"], frames[1]["height"])
    phase_known = all(s["phaseVerified"] and s["addressPhaseKnown"] for s in (a, b))
    return {"schema": "f6.guide-pair-report.v1", "identity": identity,
            "sources": [a["source"], b["source"]], "epochs": [a["epoch"], b["epoch"]],
            "phaseKnown": phase_known, "scoreValid": comparison["valid"],
            "mode": comparison["mode"], "numericAvailable": comparison["numericAvailable"],
            "assumptions": comparison["assumptions"],
            "scoreReason": comparison["reason"], "candidates": rows,
            "observationalBest": best, "ambiguity": ambiguity,
            "configurationApplied": False, "recommendation": None,
            "scoreProvenance": "serialized production CPU score; reader does not independently recompute ScorePair",
            "authority": "observations only; no GPU retirement or configuration authority"}


def write_images(document, directory):
    """Portable PPM plus PGM mask; no numpy/PIL dependency. Masked RGB is magenta."""
    directory.mkdir(parents=True, exist_ok=True)
    written = []
    for candidate in document["comparison"]["candidates"]:
        for name in VIEWS:
            view = candidate["images"][name]
            if not view["width"] or not any(view["mask"]):
                continue
            stem = directory / ("candidate-%d-%s" % (candidate["index"], name))
            rgb = bytearray()
            for pixel, valid in zip(view["pixels"], view["mask"]):
                rgb.extend(round(max(0, min(1, v))*255) for v in pixel) if valid else rgb.extend((255, 0, 255))
            dimensions = ("%d %d\n255\n" % (view["width"], view["height"])).encode("ascii")
            ppm, pgm = stem.with_suffix(".ppm"), stem.with_suffix(".pgm")
            ppm.write_bytes(b"P6\n" + dimensions + rgb)
            pgm.write_bytes(b"P5\n" + dimensions + bytes(v*255 for v in view["mask"]))
            written.extend((str(ppm), str(pgm)))
    return written


def no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON field: " + key)
        result[key] = value
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--output", type=Path, help="default: stdout")
    parser.add_argument("--images-dir", type=Path, help="optional PPM+PGM error maps")
    args = parser.parse_args(argv)
    try:
        require(not args.output or args.output.resolve() != args.input.resolve(), "cannot overwrite input export")
        raw = args.input.read_bytes()
        require(len(raw) <= 64*1024*1024, "export exceeds 64 MiB bound")
        document = json.loads(raw.decode("utf-8-sig"), object_pairs_hook=no_duplicates,
                              parse_constant=lambda value: (_ for _ in ()).throw(ValueError("non-JSON number: " + value)))
        report = analyze(document)
        report["inputSHA256"] = hashlib.sha256(raw).hexdigest()
        report["images"] = write_images(document, args.images_dir) if args.images_dir else []
        text = json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(text, encoding="utf-8")
        else:
            sys.stdout.write(text)
        return 0
    except (ValueError, KeyError, TypeError, OSError, OverflowError) as error:
        sys.stderr.write("guide pair rejected: %s\n" % error)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
