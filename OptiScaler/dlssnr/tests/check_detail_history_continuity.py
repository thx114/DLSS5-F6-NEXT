"""No-build static probes of the production predicate and its early-return wiring.

C++ lifecycle/Host tests beside this file are for Lead to compile separately.
This does not execute D3D12, shader bytecode, the model or the game.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
POLICY = (ROOT / "dlssnr/DlssNrDetailReuse.h").read_text(encoding="utf-8")
HOST = (ROOT / "dlssnr/DlssNrDetailReuseHost.h").read_text(encoding="utf-8")
BACKEND = (ROOT / "shaders/dlssnr/DlssNr_DetailReuse.inl").read_text(encoding="utf-8")
DX12 = (ROOT / "shaders/dlssnr/DlssNr_Dx12.cpp").read_text(encoding="utf-8")
MENU = (ROOT / "dlssnr/DlssNr_Menu.cpp").read_text(encoding="utf-8")


def expression(source):
    translated = source.replace("&&", " and ").replace("||", " or ").replace("!", " not ")
    return " ".join(translated.split())


class HistoryContinuity(unittest.TestCase):
    def test_actual_predicate_endpoints(self):
        predicate = re.search(r"constexpr bool Matches\([^{}]+\) const\s*\{\s*return (.*?);", POLICY, re.S)[1]
        # Evaluate the exact production scalar predicate, without maintaining a
        # second policy implementation. The separate C++ tests exercise Observe.
        for published, current, ordinal, forward, sourceClock, expected in [
            (10, 11, 10, True, True, True),
            (1798, 1800, 1799, True, False, False),  # PID144892 late history
            (10, 12, 11, True, False, False),        # GPU-busy intervening input
            (10, 12, 10, True, True, False),         # actual missing source
            (10, 16, 10, True, False, True),         # physical FG clock
            (10, 16, 10, True, True, False),
            (10, 10, 10, False, True, False),        # duplicate
            (10, 9, 10, False, False, False),        # rollback
            (10, 11, 10, False, True, False),        # unobserved first input
            (2**64 - 1, 0, 2**64 - 1, False, True, False),
        ]:
            with self.subTest(published=published, current=current, sourceClock=sourceClock):
                values = dict(published=published, current=current, ordinal=ordinal,
                              forward=forward, sourceClock=sourceClock)
                self.assertEqual(eval(expression(predicate), {"__builtins__": {}}, values), expected)

    def test_observation_precedes_every_early_return(self):
        before = BACKEND.split("Plan BeforeModel(const Frame& f)", 1)[1].split("void AfterModelHandoff", 1)[0]
        observation = before.index("observedInputs.Observe(f.frameNumber)")
        self.assertLess(observation, before.index("CollectHistory();"))
        self.assertLess(observation, before.index("return plan;"))
        self.assertIn("previousInput.Matches(publishedOrdinal, f.frameNumber, singleSourceStep)", before)
        self.assertIn("historyPublished && matchingOrdinal && matchingContract && !f.modelReset", before)
        self.assertIn("observedInputs.Reset();", BACKEND.split("void Release()", 1)[1])

    def test_native_source_clock_is_never_fg_step_learning(self):
        self.assertIn("const bool sourceClock = f.nativeTemporalRouteRequested || f.pictureFlowReuseVerified;", HOST)
        self.assertIn("if (!sourceClock) _steps.Add(f.frameNumber);", HOST)
        self.assertIn("facts.maxStep = sourceClock ? 1ull : MaxStepFor(w.withFg, _steps.Typical());", HOST)

    def test_existing_gpu_and_native_motion_boundaries_preserved(self):
        before = BACKEND.split("Plan BeforeModel(const Frame& f)", 1)[1].split("void AfterModelHandoff", 1)[0]
        self.assertIn("if (!recordingAvailable && PendingRecordings() != 0)", before)
        self.assertIn("params.TemporalMode == 0u && !plan.reused", before)
        self.assertIn("host.Finish(facts, plan.active, plan.reused, composedReadable)", before)
        self.assertNotIn("pictureFlow->", before)
        self.assertNotIn("OpticalFlow", before)

    def test_presented_feedback_is_independent_of_legacy_reuse_toggle(self):
        self.assertIn("RequestsPresentedFeedback(\n        cfg.DlssNrPresentedTemporalDetail.value_or_default(), vulkanGame)", DX12)
        self.assertNotIn("DlssNrDetailReuse.value_or_default() &&\n        cfg.DlssNrPresentedTemporalDetail", DX12)
        self.assertIn("RequiresFullModelEvaluation(", DX12)

    def test_genshin_presentation_does_not_silence_legacy_reuse(self):
        before = BACKEND.split("Plan BeforeModel(const Frame& f)", 1)[1].split("bool orderedAdvance", 1)[0]
        self.assertIn("if (f.independentFeedbackRequested)", before)
        self.assertNotIn("f.info->GenshinPresentPipeline", before)

    def test_feedback_status_and_legacy_controls_are_separate(self):
        self.assertIn("if (temporal)\n    {\n        const auto status = DlssNr::DetailReuseStatus();", MENU)
        self.assertEqual(MENU.count("if (legacyDetailReuseActive)"), 2)
        self.assertIn("间隔复用：持续反馈期间未运行", MENU)

    def test_old_generic_rule_accepts_the_logged_wrong_endpoint(self):
        # Frozen former matchingOrdinal expression, verified against pre-fix
        # source. A negative fixture must not silently change when HEAD advances.
        predicate = "published < current && (!singleSourceStep || current - published == 1)"
        values = dict(published=1798, current=1800, singleSourceStep=False)
        self.assertTrue(eval(expression(predicate), {"__builtins__": {}}, values),
                        "negative control must reproduce old non-adjacent acceptance")


if __name__ == "__main__":
    unittest.main(verbosity=2)
