"""CPU/wiring checks for the Legacy owner callback boundary; no DLL/GPU execution.

Run: python -B OptiScaler/genshin/tests/test_legacy_owner_boundary.py
The transition tests evaluate the production notification predicate. The other
checks bind the refactor to its actual call sites and retained authority gates.
"""

from pathlib import Path
import re
import unittest


SOURCE = (Path(__file__).resolve().parents[1] / "GenshinLegacyCapture.cpp").read_text(
    encoding="utf-8"
)


def block(signature: str, source: str = SOURCE) -> str:
    """Read a C++ block, ignoring braces inside comments and string literals."""
    start = source.index(signature)
    opening = source.index("{", start)
    tokens = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\{|\}', re.S)
    depth = 0
    for token in tokens.finditer(source, opening):
        value = token.group()
        if value == "{":
            depth += 1
        elif value == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : token.start()]
    raise AssertionError(f"Unclosed C++ block: {signature}")


BOUNDARY = block("class OwnerCallbackBoundary final")
NOTIFY = block("static void ObserveCaptureControls(")


def notification_epochs(epochs: list[int]) -> list[int]:
    # Execute the actual production equality predicate, rather than maintaining
    # another monotonic/version policy in the regression fixture.
    condition = re.search(r"if\((writerNotifiedEpoch[^)]*)\)", NOTIFY).group(1)
    expression = condition.replace("controls.epoch", "epoch")
    allowed = re.fullmatch(r"writerNotifiedEpoch\s*!=\s*epoch", expression)
    if not allowed:
        raise AssertionError(f"Unsupported production predicate: {condition}")
    notified = 0
    emitted = []
    for epoch in epochs:
        if eval(expression, {"__builtins__": {}}, {"writerNotifiedEpoch": notified, "epoch": epoch}):
            emitted.append(epoch)
            notified = epoch
    return emitted


class ControlsTransitions(unittest.TestCase):
    def test_steady_owner_avoids_per_callback_writer_exchange(self):
        self.assertEqual(notification_epochs([7] * 100_000), [7])

    def test_each_changed_epoch_still_notifies(self):
        self.assertEqual(notification_epochs([1, 1, 2, 2, 3]), [1, 2, 3])

    def test_stale_callback_does_not_hide_aba_revocation(self):
        self.assertEqual(notification_epochs([8, 9, 8, 9]), [8, 9, 8, 9])

    def test_unknown_epoch_is_not_a_monotonic_admission_floor(self):
        self.assertEqual(notification_epochs([1, 0, 1]), [1, 0, 1])

    def test_ps_domain_and_live_session_are_not_epoch_cached(self):
        guard = NOTIFY.index("if(writerNotifiedEpoch")
        self.assertLess(NOTIFY.index("PsState::ObserveConfig("), guard)
        self.assertGreater(NOTIFY.index("ObserveCaptureSessionControls(controls);"), guard)
        conditional = block("if(writerNotifiedEpoch", NOTIFY)
        self.assertNotIn("PsState::", conditional)
        self.assertNotIn("RuntimeCapture::", conditional)

    def test_session_stop_keeps_live_state_and_actual_epoch(self):
        stop = block("static void ObserveCaptureSessionControls(")
        for required in (
            "capture.active.load(std::memory_order_acquire)",
            "!controls.settings.enabled",
            "!controls.settings.captureOnlyObservation",
            "controls.epoch!=capture.configEpoch",
            "RuntimeCapture::Stop()",
        ):
            self.assertIn(required, stop)


class BoundaryWiring(unittest.TestCase):
    def test_config_snapshot_and_timing_have_the_required_lock_order(self):
        declarations = ["controls_;", "wait_{", "lock_{nativeMutex}", "held_;"]
        positions = [BOUNDARY.index(part) for part in declarations]
        self.assertEqual(positions, sorted(positions))
        constructor = block("explicit OwnerCallbackBoundary(std::optional", BOUNDARY)
        self.assertLess(constructor.index("wait_.Finish()"), constructor.index("held_.emplace("))
        self.assertNotIn("AcquireGenshinControls", constructor)
        self.assertIn("if(controls_)ObserveCaptureControls(*controls_);", constructor)
        self.assertNotIn("~OwnerCallbackBoundary", BOUNDARY)

    def test_inactive_capture_callback_does_not_acquire_config(self):
        acquire = block("static std::optional<ControlsSnapshot> CaptureControls()", BOUNDARY)
        self.assertLess(acquire.index("return std::nullopt"), acquire.index("AcquireGenshinControls()"))
        self.assertIn("RuntimeCapture::Get().active.load(std::memory_order_acquire)", acquire)

    def test_controls_are_published_only_by_boundary_entry(self):
        calls = re.findall(r"ObserveCaptureControls\(", SOURCE)
        self.assertEqual(len(calls), 2)  # definition and the boundary constructor
        for removed in ("CaptureControlsAtBoundary", "NoteControls(", "captureNotifiedEpoch", "captureNotifiedActive", "ReturnedRasterCurrent("):
            self.assertNotIn(removed, SOURCE)

    def test_status_publication_does_not_renotify_old_controls(self):
        status = block("void UpdateStatus() const", BOUNDARY)
        self.assertIn("UpdateControlsStatus(controls_?*controls_:nativeControls)", status)
        self.assertNotIn("ObserveCaptureControls", status)
        present = block("void OnPresent(ID3D11Device* device)")
        self.assertEqual(present.count("OwnerCallbackBoundary boundary(controls);"), 1)
        self.assertEqual(present.count("UpdateControlsStatus(controls);"), 1)

    def test_retained_identity_precedes_fallback_com_queries(self):
        owner = block("bool ImmediateDeviceMatches(", BOUNDARY)
        retained = owner.index("if(nativeDevice && context==nativeContext.Get())return true;")
        self.assertLess(retained, owner.index("context->GetType()"))
        self.assertLess(retained, owner.index("context->GetDevice("))
        self.assertEqual(owner.count("context->GetType()"), 1)
        self.assertEqual(owner.count("context->GetDevice("), 1)
        self.assertNotIn("GetDeviceRemovedReason", owner)
        self.assertNotIn("static ", owner)

    def test_raster_keeps_live_controls_owner_thread_and_drain_gates(self):
        raster = block("bool RasterCurrent(", BOUNDARY)
        for required in (
            "if(!controls_)return false;",
            "!RouteActive(controls)",
            "!shaderHookInstalled",
            "!Config::Instance()->GenshinControlsCurrent(controls.epoch)",
            "context==nativeContext.Get()",
            "GetCurrentThreadId()==nativeThread",
            "nativeDeviceEpoch",
            "nativeControls.epoch==controls.epoch",
            "nativeControls.settings==controls.settings",
            "!nativePipeline->NeedsDrain()",
            "ImmediateDeviceMatches(context)",
        ):
            self.assertIn(required, raster)
        self.assertNotIn("->GetDevice(", raster)
        self.assertNotIn("->GetType()", raster)

    def test_shared_observer_keeps_skip_qualification_and_invalidation(self):
        observer = block("bool ObserveRaster(", BOUNDARY)
        self.assertIn("draw?ObservationOnlyCurrent(context,epoch):OwnerObservationCurrent(context,epoch)", observer)
        self.assertIn("if(!RasterCurrent(context))", observer)
        self.assertIn("GenshinReturnedRaster::Invalidate(context,nullptr,GenshinReturnedRaster::Boundary::Owner)", observer)
        self.assertIn("ownerObservation.srOnly=false;", observer)
        self.assertIn("if(draw && nativePipeline)nativePipeline->SubscribeDrawObservation(true);", observer)

    def test_all_four_raster_callbacks_use_the_shared_contract(self):
        for signature, mode in (
            ("void BeforeNativeDepthRaster(", "true"),
            ("static bool BeforeModRasterKind(", "true"),
            ("void BeforeNativeDepthClear(", "false"),
            ("void AfterNativeDepthClear(", "false"),
        ):
            with self.subTest(callback=signature):
                callback = block(signature)
                self.assertEqual(callback.count("OwnerCallbackBoundary boundary(controls);"), 1)
                self.assertIn(f"boundary.ObserveRaster(context,{mode})", callback)
                self.assertNotIn("std::lock_guard lock(nativeMutex)", callback)

    def test_draw_compute_and_post_reuse_only_immutable_owner_relation(self):
        before = block("Action BeforeNativeDraw(")
        after = block("void AfterNativeDraw(")
        compute = block("void ObserveNativeDepthCompute(")
        self.assertIn("!sameOwner && !boundary.ImmediateDeviceMatches(context)", before)
        self.assertIn("if(boundary.RasterCurrent(context))", after)
        self.assertIn("if(!boundary.ImmediateDeviceMatches(context))return;", compute)
        self.assertIn("GenshinDepthEvidence::LocationCurrent(context,GetCurrentThreadId(),nativeDeviceEpoch,controls.epoch)", compute)
        self.assertIn("context!=nativeContext.Get() || GetCurrentThreadId()!=nativeThread", compute)
        for callback in (before, after, compute):
            self.assertNotIn("context->GetDevice(", callback)
            self.assertNotIn("context->GetType()", callback)

    def test_draw_return_controls_are_revalidated_and_epoch_change_revokes(self):
        acquire = block("static ControlsSnapshot AcquireDrawControls(")
        for required in ("drawControls->context==context", "drawControls->thread==GetCurrentThreadId()", "controls.version==1", "controls.bytes==sizeof(ControlsSnapshot)", "GenshinControlsCurrent(drawControls->controls.epoch)", "return config.AcquireGenshinControls();"):
            self.assertIn(required, acquire)
        after = block("void AfterNativeDraw(")
        self.assertIn("controls.epoch!=nativeControls.epoch || controls.settings!=nativeControls.settings", after)
        self.assertIn("nativePipeline->RevokeTransport();DrainNativePipeline();UpdateControlsStatus(controls);return;", after)

    def test_new_session_is_checked_after_manual_start(self):
        before = block("Action BeforeNativeDraw(")
        self.assertLess(before.index("ObserveManualDraw("), before.index("ObserveCaptureSessionControls(controls);"))

    def test_refactor_does_not_create_completion_or_callable_authority(self):
        for forbidden in ("StableLocked", "ExistingOriginal", "SeedCreatedContext", "GetData(", "WaitForSingleObject", "Flush(", "producer", "static bool current"):
            self.assertNotIn(forbidden, BOUNDARY)
        before = block("Action BeforeNativeDraw(")
        self.assertIn("ResourceProof::Configure(", before)
        self.assertIn("DrainNativePipeline()", before)
        self.assertIn("TryInstallEntry();if(!nativeStatus.entryInstalled)", before)
        self.assertIn("if(!shaderHookInstalled)", before)


if __name__ == "__main__":
    unittest.main(verbosity=2)
