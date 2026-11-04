// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "CaptureOnlyBridge.h"

namespace GenshinLegacy::Engine
{
enum class Reason : std::uint8_t { Unobserved, CandidateSemanticsIncomplete, WrongOwner, UnknownPass,
    UnsupportedViews, MissingFrame, DuplicateOrOutOfOrder, InstanceCapacity, Reset };
enum class MotionEncoding : std::uint8_t { Unknown, LegacySignedSquare127Over255 };
// Facts observed by the actual FSR draw caller are distinct from semantic proof.
// No device-Z direction, colour transfer, jitter units or camera defaults become known here.
struct Receipt
{
    Reason reason=Reason::Unobserved;
    NoSrCapture::Signal candidate{};
    std::array<std::uint64_t,3> resources{};
    std::array<std::uint32_t,3> srvSlots{};
    std::uint64_t ownerEpoch=0,shaderHash=0,shaderIdentity=0,sourceGeneration=0;
    MotionEncoding motionEncoding=MotionEncoding::Unknown;
    std::array<ResourceProof::Witness,3> sourceWrites{};
    ResourceProof::Witness outputIdentity{},presentedBackbuffer{};
    std::uint64_t observedSwapchain=0,observedPresentSerial=0;
    HRESULT observedPresentResult=E_PENDING;
    bool originalDrawReturned=false,directOutputMatchesBackbuffer=false,sourceToTargetProven=false;
    bool actualRenderWitness=false,knownLegacyPass=false,rawJitterFinite=false,previousRawJitterContiguous=false;
    bool colourSemanticsKnown=false,depthSemanticsKnown=false,motionSemanticsKnown=false,jitterUnitsKnown=false;
    bool producerBuildVerified=false,resourceWriteGenerationKnown=false,nrReady=false,fgReady=false;
};
class Producer final
{
    struct History {std::uint64_t instance=0,generation=0,frame=0;float jitterX=0,jitterY=0;};
    std::array<History,8> histories_{};
    Receipt receipt_{};
    std::uint64_t device_=0,context_=0,deviceEpoch_=0,ownerEpoch_=0,thread_=0;
public:
    void Bind(ID3D11Device*,ID3D11DeviceContext*,std::uint64_t deviceEpoch,std::uint64_t ownerEpoch,std::uint64_t thread) noexcept;
    bool Observe(const il2cpp_callsite::RenderToken&,const CaptureSource&,NoSrCapture::Signal&) noexcept;
    static bool Publish(const il2cpp_callsite::RenderToken&,const CaptureSource&,NoSrCapture::Signal&,void*) noexcept;
    Receipt GetReceipt()const noexcept{return receipt_;}
    void Reset() noexcept;
    void AfterOriginalDraw(std::uint64_t renderGeneration) noexcept;
    void NotePresent(const Presentation::Receipt&,const ResourceProof::Witness&,bool sameRender) noexcept;
};
const char* ReasonText(Reason) noexcept;
}
