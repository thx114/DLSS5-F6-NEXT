#!/usr/bin/env python3
"""Stdlib-only offline tests. Optional F6_GUIDE_PAIR_FIXTURE=<C++ --write-fixture output>."""
import contextlib
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[1] / "runtime" / "Analyze-F6GuidePair.py"
SPEC = importlib.util.spec_from_file_location("f6_guide_pair", MODULE)
ANALYZER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ANALYZER)


def candidates(valid=False):
    result = []
    for index in range(8):
        metrics = dict.fromkeys(ANALYZER.METRICS, 0.0)
        metrics.update(dict.fromkeys(ANALYZER.COUNTS, 0))
        if valid:
            metrics.update(score=.05+.05*index, reprojection=.01, coverage=.8,
                           eligible=64, depthSamples=64, motionSamples=64,
                           compared=52, occluded=0, outOfBounds=0)
        empty = {"width": 0, "height": 0, "pixels": [], "mask": []}
        result.append({"index": index, "depthRows": bool(index & 1),
                       "motionRows": bool(index & 2), "vectorY": bool(index & 4),
                       "valid": valid, "reason": "synthetic numeric fixture" if valid else "nonconsecutive source epochs",
                       "rejectionCode": 0 if valid else 9, "metrics": metrics,
                       "images": {name: copy.deepcopy(empty) for name in ANALYZER.VIEWS}})
    return result


def document():
    # Synthetic observations with intentionally insufficient phase/epoch evidence.
    def frame(source, tick):
        return {"width": 8, "height": 8, "stream": 0, "valid": True,
                "completedTick": tick+1, "colourPresent": True, "depthPresent": True,
                "motionPresent": True, "contributionPresent": False,
                "colour": [[.1, .2, .3, 1] for _ in range(64)],
                "depth": [.5]*64, "motion": [[1, 1] for _ in range(64)], "contribution": [],
                "motionToUvX": .125, "motionToUvY": .125,
                "stamp": {"source": source, "epoch": 7, "feature": 3, "configuration": 4,
                          "owner": 5, "tick": tick, "stage": 5,
                          "depthFlip": False, "motionFlip": False, "yNegated": False,
                          "phaseVerified": False, "addressPhaseKnown": False,
                          "motionJitterKnown": False, "fields": [{} for _ in range(14)]}}
    return {"schema": ANALYZER.SCHEMA,
            "identity": {"pid": 42, "diskDllSHA": "a"*64, "loadedMemoryKnown": False,
                         "loadedMemorySHA": None, "configuration": "synthetic", "capturedUtc": ""},
            "frames": [frame(20, 1000), frame(21, 1016)],
            "comparison": {"algorithm": ANALYZER.ALGORITHM, "valid": False,
                           "mode": "rejected", "numericAvailable": False, "phaseKnown": False, "assumptions": "",
                           "reason": "nonconsecutive source epochs", "autoApply": False,
                           "candidates": candidates()}}


class GuidePairTests(unittest.TestCase):
    def test_refusal_is_reported_without_invented_zero_error(self):
        report = ANALYZER.analyze(document())
        self.assertFalse(report["scoreValid"])
        self.assertFalse(report["phaseKnown"])
        self.assertIsNone(report["recommendation"])
        self.assertIsNone(report["observationalBest"])
        self.assertTrue(all(row["metrics"] is None for row in report["candidates"]))
        self.assertFalse(report["identity"]["loadedMemoryKnown"])

    def test_structural_negative_controls(self):
        edits = [
            lambda d: d.update(schema="guessed-schema"),
            lambda d: d["frames"].pop(),
            lambda d: d["frames"][1].update(stream=2),
            lambda d: d["frames"][1].update(width=9),
            lambda d: d["frames"][1]["stamp"].update(owner=6),
            lambda d: d["frames"][1]["stamp"].update(epoch=8),
            lambda d: d["frames"][1]["stamp"].update(source=22),
            lambda d: d["frames"][1]["stamp"].update(tick=1000),
            lambda d: d["frames"][1].update(motionToUvY=0),
            lambda d: d["frames"][1].update(completedTick=1),
            lambda d: d["frames"][1].update(depth=[]),
            lambda d: d["identity"].update(loadedMemoryKnown=True),
            lambda d: d["identity"].update(loadedMemorySHA="a"*64),
            lambda d: d["comparison"].update(autoApply=True),
            lambda d: d["comparison"]["candidates"][3].update(vectorY=True),
            lambda d: d["comparison"]["candidates"].pop(),
            lambda d: d["comparison"]["candidates"][0]["metrics"].update(score=float("nan")),
        ]
        for edit in edits:
            with self.subTest(edit=edit):
                data = document()
                edit(data)
                with self.assertRaises((ValueError, TypeError, KeyError)):
                    ANALYZER.analyze(data)

    def test_nonfinite_raw_tokens_preserved(self):
        data = document()
        data["frames"][0]["colour"][0][0] = "NaN"
        data["frames"][1]["depth"][0] = "+Infinity"
        data["frames"][1]["motion"][0][1] = "-Infinity"
        ANALYZER.analyze(data)
        self.assertEqual(data["frames"][0]["colour"][0][0], "NaN")
        data["frames"][0]["depth"][0] = "nan"
        with self.assertRaises(ValueError):
            ANALYZER.analyze(data)

    def test_ranking_is_observational_and_ties_abstain(self):
        comparison = document()["comparison"]
        comparison.update(valid=True, candidates=candidates(True), mode="phase-verified observation", phaseKnown=True)
        rows, best, reason = ANALYZER.summarize_candidates(comparison)
        self.assertEqual(len(rows), 8)
        self.assertEqual(best["index"], 0)
        self.assertIn("no direction recommendation", reason)
        comparison["candidates"][1]["metrics"]["score"] = .05
        self.assertIsNone(ANALYZER.summarize_candidates(comparison)[1])
        comparison["candidates"][1]["metrics"]["score"] = .05000001
        self.assertIsNone(ANALYZER.summarize_candidates(comparison)[1])

    def test_numeric_claim_without_phase_rejected(self):
        data = document()
        data["comparison"].update(valid=True, candidates=candidates(True))
        with self.assertRaises(ValueError):
            ANALYZER.analyze(data)

    def test_portable_error_images_and_masks(self):
        data = document()
        view = {"width": 8, "height": 8, "pixels": [[.25, .25, .25] for _ in range(64)],
                "mask": [0]+[1]*63}
        ANALYZER.check_view(view, 8, 8)
        data["comparison"]["candidates"][0]["images"]["residual"] = view
        with tempfile.TemporaryDirectory() as temp:
            files = ANALYZER.write_images(data, Path(temp))
            self.assertEqual(len(files), 2)
            payload = Path(files[0]).read_bytes()
            header = b"P6\n8 8\n255\n"
            self.assertTrue(payload.startswith(header))
            self.assertEqual(payload[len(header):len(header)+3], bytes((255, 0, 255)))
            self.assertEqual(len(payload)-len(header), 8*8*3)
            self.assertTrue(Path(files[1]).read_bytes().startswith(b"P5\n8 8\n255\n"))
        invalid = copy.deepcopy(view)
        invalid["mask"][0] = 2
        with self.assertRaises(ValueError):
            ANALYZER.check_view(invalid, 8, 8)

    def test_cli_json_report_and_input_preserved(self):
        with tempfile.TemporaryDirectory() as temp:
            input_path = Path(temp)/"pair.json"
            output_path = Path(temp)/"report.json"
            original = json.dumps(document()).encode("utf-8")
            input_path.write_bytes(original)
            self.assertEqual(ANALYZER.main([str(input_path), "--output", str(output_path)]), 0)
            self.assertEqual(input_path.read_bytes(), original)
            self.assertEqual(json.loads(output_path.read_text("utf-8"))["schema"], "f6.guide-pair-report.v1")
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(ANALYZER.main([str(input_path), "--output", str(input_path)]), 2)
            input_path.write_text('{"schema":"x","schema":"y"}', encoding="utf-8")
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(ANALYZER.main([str(input_path)]), 2)

    @unittest.skipUnless(os.environ.get("F6_GUIDE_PAIR_FIXTURE"), "optional C++ fixture not provided")
    def test_cpp_serialized_observation(self):
        data = json.loads(Path(os.environ["F6_GUIDE_PAIR_FIXTURE"]).read_text("utf-8"))
        report = ANALYZER.analyze(data)
        self.assertFalse(report["scoreValid"])
        self.assertIsNone(report["recommendation"])
        self.assertEqual(len(report["candidates"]), 8)


if __name__ == "__main__":
    unittest.main()
