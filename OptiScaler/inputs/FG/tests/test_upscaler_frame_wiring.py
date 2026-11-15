"""Static adapter checks; these do not execute C++/Windows/GPU code.

The C++ suite tests scalar contracts. This suite separately checks that both
real callers use that code without bypassing their existing source boundaries.
"""
from pathlib import Path
import re
import unittest


FG_INPUTS = Path(__file__).resolve().parents[1]
PRODUCT = FG_INPUTS.parents[1]


def read(path):
    return path.read_text(encoding="utf-8-sig")


def function_body(text, signature):
    start = text.index("{", text.index(signature))
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


class FrameInputWiringTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.native = function_body(read(FG_INPUTS / "Upscaler_Inputs_Dx12.cpp"),
                                   "dlssg::NativeSource UpscalerInputsDx12::UpscaleStart(")
        cls.interop = function_body(read(FG_INPUTS / "Upscaler_Inputs_Dx11wDx12.cpp"),
                                    "void UpscalerInputsDx11wDx12::UpscaleStart(")
        cls.common = read(FG_INPUTS / "UpscalerFrameParameters.h")

    def test_both_real_callers_use_one_scalar_pipeline(self):
        for caller in (self.native, self.interop):
            with self.subTest(caller="native" if caller is self.native else "interop"):
                for operation in ("ReadFrameParameters", "MakeFgConstants", "ApplyFrameParameters"):
                    self.assertEqual(caller.count("fg_inputs::" + operation), 1)
                self.assertLess(caller.index("StartNewFrame()"), caller.index("fg_inputs::ApplyFrameParameters"))
                for setter in ("SetCameraValues", "SetFrameTimeDelta", "SetMVScale", "SetJitter", "SetReset",
                               "SetInterpolationRect"):
                    self.assertNotIn("fg->" + setter, caller)
                self.assertIn("*InParameters, *feature, cfg,", caller)
                self.assertIn("MakeFgConstants<FG_Constants, FG_Flags>(frame)", caller)
                self.assertRegex(caller, r"OptiKeys::FSR_NearPlane,\s*OptiKeys::FSR_FarPlane,\s*"
                                        r"OptiKeys::FSR_CameraFovVertical,\s*OptiKeys::FSR_ViewSpaceToMetersFactor")

    def test_public_input_policies_remain_enabled(self):
        policy = read(PRODUCT / "runtime/F6InputPolicy.h")
        for flag in ("DirectDlssInputs", "PublicFsrInputs"):
            self.assertRegex(policy, r"constexpr\s+bool\s+" + flag + r"\s*=\s*true")
        for caller in (self.native, self.interop):
            self.assertNotIn("GenshinSrReceipt", caller)
            self.assertNotIn("GenshinTransport", caller)

    def test_native_source_join_and_guard_still_precede_publication(self):
        caller = self.native
        self.assertIn("NativeObject(InCmdList)", caller)
        self.assertIn("selectedList.As(&selectedIdentity)", caller)
        self.assertIn("SelectedFeatureIdentity<IFeature>(feature)", caller)
        scope = caller.index("Selection selectedEntry")
        reject = caller.index('RejectUpscalerGuideSource("native-frame-scope-missing-or-mismatched")')
        guard = caller.index("own->BeginNativeUpscalerInputs")
        advance = caller.index("const auto startedFrame=fg->StartNewFrame()")
        apply = caller.index("fg_inputs::ApplyFrameParameters")
        publish = caller.index("own->SetUpscalerGuidePair(static_cast<IFeature*>(feature),commandList")
        self.assertLess(scope, reject)
        self.assertLess(reject, guard)
        self.assertLess(guard, advance)
        self.assertLess(advance, apply)
        self.assertLess(apply, publish)
        self.assertIn("startedFrame==previousFrame", caller)
        self.assertIn("checkedSource && (!fg->CanSubmitInputs()", caller)
        self.assertIn("checkedSource ? fg->GuardExecution(FGExecutionScope::Inputs)", caller)
        self.assertIn("own->NativeGuideSource(InCmdList)", caller)

    def test_interop_cache_and_live_source_identity_are_not_scalar_proof(self):
        caller = self.interop
        self.assertLess(caller.index("if (!prepareInputs())"), caller.index("fg->EvaluateState"))
        self.assertLess(caller.index("fg->GuardExecution(FGExecutionScope::Inputs)"),
                        caller.index("fg->StartNewFrame()"))
        self.assertLess(caller.index("if (!fg->CanSubmitInputs()) return"), caller.index("fg->StartNewFrame()"))
        self.assertIn("if (!fg->CanSubmitInputs() || startedFrame == previousFrame) return", caller)
        self.assertIn("if (!cmdList) return", caller)
        for check in ("cache.frameId!=prepared", "cache.Depth.LastPreparedFrame!=prepared",
                      "cache.Mv.LastPreparedFrame!=prepared", "sourceDepthTexture.Get()!=cache.Depth.SourceTexture",
                      "sourceMotionTexture.Get()!=cache.Mv.SourceTexture"):
            self.assertIn(check, caller)
        reject = caller.index('RejectUpscalerGuideSource("selected-sr-cache-source-mismatch")')
        self.assertLess(reject, caller.index("own->SetUpscalerGuidePair(static_cast<IFeature*>(feature),cmdList"))
        self.assertIn("ReadResource<ID3D11Resource>(*InParameters, NVSDK_NGX_Parameter_Depth)", caller)
        self.assertIn("ReadResource<ID3D12Resource>(*InParameters, NVSDK_NGX_Parameter_Depth)", self.native)

    def test_public_fsr_discovery_remains_available(self):
        fsr = read(FG_INPUTS / "FSR3_Dx12_FG.cpp")
        for entry in ("HookFSR3FGExeInputs", "HookFSR3FGInputs"):
            implementation = function_body(fsr, "void FSR3FG::" + entry + "()")
            self.assertNotIn("FsrBridgeEnabled", implementation)
            self.assertIn("Detour", implementation)
        output = function_body(fsr, "void FSR3FG::SetUpscalerInputs(")
        for key in ("NVSDK_NGX_Parameter_Depth", "NVSDK_NGX_Parameter_MotionVectors"):
            self.assertIn(key, output)
        loader = read(PRODUCT / "dllmain.cpp")
        for entry in ("HookFSR2VkExeInputs()", "HookFSR2Dx11ExeInputs()", "HookFSR2ExeInputs()"):
            self.assertIn(entry, loader)
        proxy = read(PRODUCT / "proxies/FfxApi_Proxy.h")
        self.assertIn("F6InputPolicy::PublicFsrInputs", proxy)
        self.assertIn("main_vk.CreateContext != nullptr", proxy)
        self.assertIn("VULKAN_Dispatch()", read(PRODUCT / "inputs/FfxApi_Vk.cpp"))
        self.assertIn("void HookFSR2VkExeInputs()", read(PRODUCT / "inputs/FSR2_Vk.cpp"))

    def test_failed_selected_dx11_sr_cannot_publish_fg_guides(self):
        entry = function_body(read(PRODUCT / "inputs/NVNGX_DLSS_Dx11.cpp"),
                              "static NVSDK_NGX_Result EvaluateDx11(")
        gate = entry.index("if (upscaleResult")
        self.assertLess(gate, entry.index("UpscalerInputsDx11wDx12::UpscaleStart"))
        self.assertLess(entry.index("deviceContext->Evaluate(InDevCtx, InParameters)"), gate)

    def test_all_upscaler_guide_routes_defer_before_publication_during_warmup(self):
        provider = read(PRODUCT / "framegen/dlssg/DLSSG_Dx12.cpp")
        provider += "\n" + "\n".join(p.read_text(encoding="utf-8-sig") for p in sorted(
            next(a / "OptiScaler/framegen/dlssg" for a in Path(__file__).resolve().parents
                 if (a / "OptiScaler/framegen/dlssg").is_dir()).glob("DLSSG_Dx12_*.cpp")))
        pair = function_body(provider, "bool DLSSG_Dx12::SetUpscalerGuidePair(")
        # PID79484 repeatedly activated at N, then tore down at N+1 while
        # target=N+10: SetResource's scheduled pause is not a failed SDK tag.
        gate = pair.index("if (!IsActive() || IsPaused()) return false;")
        for first_side_effect in ("OnDevice(_device,list)", "++use.value",
                                  "_managedRetained=true", "shader->Dispatch(",
                                  "return SetResource(&tag)"):
            self.assertLess(gate, pair.index(first_side_effect))
        self.assertNotIn("nativeZzzSource && (!IsActive() || IsPaused())", pair)
        # The caller advances before publication, so deferring can reach target.
        for caller in (self.native, self.interop):
            self.assertLess(caller.index("StartNewFrame()"), caller.index("SetUpscalerGuidePair("))

    def test_actual_partial_guide_publication_keeps_failure_retirement(self):
        provider = read(PRODUCT / "framegen/dlssg/DLSSG_Dx12.cpp")
        provider += "\n" + "\n".join(p.read_text(encoding="utf-8-sig") for p in sorted(
            next(a / "OptiScaler/framegen/dlssg" for a in Path(__file__).resolve().parents
                 if (a / "OptiScaler/framegen/dlssg").is_dir()).glob("DLSSG_Dx12_*.cpp")))
        pair = function_body(provider, "bool DLSSG_Dx12::SetUpscalerGuidePair(")
        refusal = pair.index('RefuseDepthContract("guide-pair-partial-publication")')
        self.assertLess(pair.index("_managedRetained=true"), refusal)
        self.assertLess(pair.index("!publish(FG_ResourceType::Depth"), refusal)
        reject = function_body(provider, "bool DLSSG_Dx12::RefuseDepthContract(")
        self.assertIn("_managedRetained=true;\n    Deactivate();", reject)
        present = function_body(provider, "bool DLSSG_Dx12::Present()")
        submit = function_body(provider, "bool DLSSG_Dx12::SubmitUICommandList(")
        self.assertIn("if (!SubmitUICommandList(static_cast<UINT>(fIndex))) return failSubmission();", present)
        self.assertIn("_gameCommandQueue->ExecuteCommandLists", submit)
        self.assertIn("_gameCommandQueue->Signal(_uiFence,_uiAllocatorFenceValues[index])", submit)
        self.assertLess(submit.index("->ExecuteCommandLists"), submit.index("_uiCommandListResetted[index]=false"))
        self.assertLess(submit.index("_uiCommandListResetted[index]=false"), submit.index("->Signal(_uiFence,"))

    def test_native_dlss_provider_calls_do_not_enter_fsr_observation(self):
        for api in ("Dx11", "Dx12"):
            source = read(PRODUCT / ("upscalers/dlss/DLSSFeature_" + api + ".cpp"))
            evaluate = function_body(source, "::EvaluateInternal(")
            self.assertIn("ProcessEvaluateParams(InParameters)", evaluate)
            self.assertIn("_EvaluateFeature()(In", evaluate)
            self.assertNotIn("Genshin", source)
        interop = read(PRODUCT / "upscalers/IFeature_Dx11wDx12.cpp")
        for retired in ("GenshinSrReceipt", "GenshinTransport"):
            self.assertNotIn(retired, interop)
        self.assertIn("DlssNr::EvaluateBeforeUpscale", interop)
        self.assertIn("DlssNr::EvaluateAfterUpscale", interop)
        self.assertIn("Dx12CommandQueue->Signal(Dx12Fence, fenceValue)", interop)
        self.assertIn("FramePipeline::ObserveFence(completedAfter, allocatorFenceValue)", interop)

    def test_ffx_api_fg_inputs_use_public_policy(self):
        ffx = read(FG_INPUTS / "FfxApi_Dx12_FG.cpp")
        for operation in ("CreateContext", "DestroyContext", "Configure", "Query", "Dispatch"):
            implementation = function_body(ffx, "ffxReturnCode_t ffx" + operation + "_Dx12FG(")
            self.assertTrue(implementation.lstrip("{\n \t").startswith(
                "if constexpr (!F6InputPolicy::PublicFsrInputs) return PASSTHRU_RETURN_CODE;"))
        self.assertNotIn("FsrBridgeEnabled", ffx)

    def test_current_menu_retains_nr_and_guide_controls(self):
        menu = read(PRODUCT / "dlssnr/DlssNr_Menu.cpp")
        for retired in ("RenderGenshinManualCapture", "RenderGenshinFgGuidePreview", "GenshinDepthProvider"):
            self.assertNotIn(retired, menu)
        for control in ("config->DlssNrRunBeforeSr = beforeSr", "config->DlssNrFinishedPicture = false", "config->DlssNrDebugView ="):
            self.assertIn(control, menu)
        self.assertIn("GuideDiagnosticsMenu::RenderPreviewStrip(config)", menu)
        unified = read(PRODUCT / "menu/GuideDiagnosticsMenu.cpp")
        self.assertIn("config->DlssNrGuidePreview=", unified)
        self.assertIn("GuideCapture::SetPreviewDemand", unified)

    def test_common_request_code_has_no_source_or_gpu_authority(self):
        for forbidden in ("GameRouting::", "dlssg::", "State::Instance", "Config::Instance", "DLSSG_Dx12.h"):
            self.assertNotIn(forbidden, self.common)
        self.assertNotRegex(self.common, r"\b(StartNewFrame|EvaluateState|SetResource|SetCameraData|Signal|"
                                         r"WaitForSingleObject|GetCompletedValue|SetOptions)\s*\(")

    def test_synthetic_camera_keys_match_product_definitions(self):
        # The portable C++ test avoids importing OptiTypes and its logging
        # dependencies. Check its four synthetic key spellings against source.
        definitions = read(PRODUCT / "OptiTypes.h")
        tests = read(FG_INPUTS / "tests/UpscalerFrameParametersTests.cpp")
        for key in ("FSR_NearPlane", "FSR_FarPlane", "FSR_CameraFovVertical", "FSR_ViewSpaceToMetersFactor"):
            match = re.search(r'\b' + key + r'\s*=\s*"([^"]+)"', definitions)
            self.assertIsNotNone(match, key)
            self.assertIn('"' + match.group(1) + '"', tests)


if __name__ == "__main__":
    unittest.main()
