"""FG preview admission wiring; stdlib only, no compiler/SDK/GPU execution."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "DLSSG_Dx12.cpp").read_text(encoding="utf-8-sig")


def body(signature):
    start = SOURCE.index("{", SOURCE.index(signature))
    depth = 1
    for end in range(start + 1, len(SOURCE)):
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        if not depth:
            return SOURCE[start:end + 1]
    raise AssertionError(signature)


CAPTURE = body("void DLSSG_Dx12::CaptureAcceptedGuides(")
OPEN = body("bool DLSSG_Dx12::TryOpenGuidePreviewRecording(")
SUBMIT = body("void DLSSG_Dx12::ObserveGuideSubmission(")


class PreviewAdmissionTests(unittest.TestCase):
    def test_input_hdr_is_not_final_colour_transfer(self):
        self.assertNotIn("_constants.flags[FG_Flags::Hdr]", CAPTURE)
        self.assertIn("if (colourOnly) inputs.colourHdr=", CAPTURE)
        self.assertIn("DlssNr::NativeInputColourSpace(_swapChain,final->description.Format)", CAPTURE)
        self.assertIn("DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709", CAPTURE)

    def test_colour_does_not_require_or_read_borrowed_guides(self):
        self.assertIn("const bool colourOnly=preview && demand.mode==GuideCapture::PreviewMode::FgColour;", CAPTURE)
        block = CAPTURE[CAPTURE.index("if (!colourOnly || (offline && ownedGuides))"):
                        CAPTURE.index("RequestGuideHooks();")]
        self.assertIn("if (!ownedGuides)", block)
        for assignment in ("inputs.depth=", "inputs.motion=", "inputs.motionToUvX=", "inputs.motionToUvY="):
            self.assertIn(assignment, block)
            self.assertEqual(CAPTURE.count(assignment), 1)
        self.assertIn("if (offline && ownedGuides) GuideCapture::RecordOffline", CAPTURE)

    def test_real_current_tag_identity_is_required(self):
        for check in ("!depth.accepted", "!motion.accepted", "depth.frame!=_frameCount",
                      "motion.frame!=_frameCount", "depth.generation!=_inputContinuity.SourceGeneration()",
                      "motion.generation!=depth.generation", "!(depth.owner==owner)",
                      "!(motion.owner==owner)", "snapshot.epoch!=_inputContinuity.SourceGeneration()",
                      "depth.nativeSerial!=_nativeGuideUses[slot].source.serial",
                      "motion.nativeSerial!=_nativeGuideUses[slot].source.serial"):
            self.assertIn(check, CAPTURE)

    def test_current_final_colour_and_ready_sdk_tag_are_required(self):
        for check in ("final->source==_upscalerColourSources[slot]", "UpscalerColourCurrent(final->source)",
                      "ready->second", "actual.validity==FG_ResourceValidity::UntilPresent",
                      "UpscalerColourIdentity(colour)==final->endpoint.colour",
                      "actual.width==final->description.Width", "actual.height==final->description.Height"):
            self.assertIn(check, CAPTURE)
        self.assertIn("inputs.colour=colour;inputs.colourState=actual.state", CAPTURE)

    def test_optional_open_uses_actual_allocator_fence_without_wait(self):
        self.assertIn("PollUIAllocator(slot)!=UIAllocatorStatus::Ready", OPEN)
        self.assertIn("_uiCloseFailed[slot]", OPEN)
        self.assertIn("_gameCommandQueue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT", OPEN)
        self.assertIn("OnDevice(_device,_gameCommandQueue)", OPEN)
        for forbidden in ("GetUICommandList(", "WaitFor", "->Wait(", "->Signal(",
                          "ExecuteCommandLists(", "CreateCommand"):
            self.assertNotIn(forbidden, OPEN)
        self.assertLess(OPEN.index("PollUIAllocator("), OPEN.index("_uiCommandAllocator[slot]->Reset()"))
        self.assertLess(OPEN.index("_uiCommandList[slot]->Reset("), OPEN.index("_uiCommandListResetted[slot]=true"))

    def test_hooks_precede_optional_reset_and_no_nr_enable_gate(self):
        self.assertLess(CAPTURE.index("RequestGuideHooks();"), CAPTURE.index("TryOpenGuidePreviewRecording(slot)"))
        self.assertLess(CAPTURE.index("TryOpenGuidePreviewRecording(slot)"), CAPTURE.index("std::shared_lock resources("))
        for forbidden in ("DlssNrEnabled", "DlssNrFinishedPicture", "GuideCapture::Submitted(", "GuideCapture::Reset("):
            self.assertNotIn(forbidden, CAPTURE + OPEN)

    def test_colour_execute_cannot_replace_native_producer_fence(self):
        self.assertIn("if (fence==_uiFence && (depth.input.cmdList!=_uiCommandList[slot] ||", SUBMIT)
        self.assertIn("motion.input.cmdList!=_uiCommandList[slot])) return;", SUBMIT)
        self.assertLess(SUBMIT.index("if (fence==_uiFence"), SUBMIT.index("observation.fence=fence"))

    def test_present_orders_native_source_before_optional_recording(self):
        present = body("bool DLSSG_Dx12::Present(")
        submit = body("bool DLSSG_Dx12::SubmitUICommandList(")
        self.assertLess(present.index("OrderNativeGuides("), present.index("CaptureAcceptedGuides("))
        self.assertLess(present.index("CaptureAcceptedGuides("), present.index("SubmitUICommandList("))
        self.assertLess(submit.index("->Close()"), submit.index("DlssNr::FinishedPictureSubmitting("))
        self.assertLess(submit.index("DlssNr::FinishedPictureSubmitting("), submit.index("->ExecuteCommandLists("))
        self.assertLess(submit.index("->ExecuteCommandLists("), submit.index("->Signal(_uiFence,"))
        self.assertIn("DlssNr::FinishedPictureSubmitted(_gameCommandQueue,1,", submit)


if __name__ == "__main__":
    unittest.main()
