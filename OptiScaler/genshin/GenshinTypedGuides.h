// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <inputs/GenshinFsrContract.h>
#include "GenshinOutputContract.h"
#include "GenshinNativeMrtContract.h"
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include "GenshinSrOutputGrid.h"
#include "GenshinCameraHistory.h"
namespace GenshinSrReceipt {class Scope;}
namespace GenshinGameMotion {class Reference;struct NativeGuideBundle;}

namespace GenshinReturnedRaster {class Evidence;}
namespace GenshinCameraHistory {struct Sample;}
namespace GenshinGuides {
// Receipt validation is unconditional. Only the optional next-writer collector
// is subscribed here; omitted/unknown subscriptions retain the original path.
enum class ObservationSubscription : unsigned char { Full, SrReceiptOnly };
enum class Source : unsigned char { Unknown, TextureDescriptor, ActualRenderToken, PreparationShader, RequestedControl, PinnedBridgeProfile, ActualRasterDepth, ActualDepthMapping, ActualNativeAccumulate };
enum class ColourDomain : unsigned char { Unknown, SceneLinear, DisplayLinear, DisplayEncoded, NativeBounded };
enum class DepthConvention : unsigned char { Unknown, DeviceZForward, DeviceZReversed, LinearView, NumericalMaximumWins };
enum class MotionDirection : unsigned char { Unknown, CurrentToPrevious, PreviousToCurrent };
enum class MotionEncoding : unsigned char { Unknown, PreparedSignedSquareHalf2 };
enum class JitterUnits : unsigned char { Unknown, RenderPixels, NormalizedUv };
enum class Reason : unsigned char { None, MissingReceipt, WrongThread, WrongOwner, StaleFrame, ConfigChanged,
    Retired, ResourceMismatch, WrongDimensions, UnsupportedFormat, NonFinite, ProducerBuildUnknown,
    ColourDomainUnknown, DepthConventionUnknown, MotionDirectionUnknown, JitterUnitsUnknown,
    SubrectUnknown, OutputConsumerUnknown, EvaluateUnconfirmed, MissingTemporalParameters,
    TemporalParametersChanged };
template<class T> struct Fact { T value{};Source source=Source::Unknown; };
struct TextureFact {
    uint64_t identity=0,device=0;
    Fact<uint32_t> width,height;
    Fact<DXGI_FORMAT> format;
};
// Source facts are observations of the original selected content. These fields
// survive a semantic refusal; none can authorize a picture or GPU reader.
struct DepthSource {
    uintptr_t selected=0,original=0,prepared=0,copiedFrom=0;
    uint64_t lifetime=0,originalLifetime=0,loss=0,version=0,clear=0,returned=0,total=0,covered=0,unresolved=0,firstCall=0;
    unsigned mip=0,copyHops=0,refusal=0,stageMask=0,boundary=UINT_MAX,claim=0;
    unsigned captureReason=0,captureStage=0,terminalInstruction=0,dependency=0,cbSlot=UINT_MAX;
    uint64_t cbRevision=0,cbLoss=0;unsigned cbState=0;
    uintptr_t shader=0;unsigned programFailure=0,resourceSlot=UINT_MAX,resourceType=UINT_MAX;
    std::array<char,40> terminalOpcode{};
    const char* operation="NONE";
    bool clearReturned=false,coverageComplete=false;
};
class PreparedDepthSource;
struct Facts {
    Owner owner;
    std::shared_ptr<const PreparedDepthSource> preparedSource;
    DepthSource depthSource{};
    // Local consumer-time result. These measurements do not authorize phase.
    const char* depthSemanticFailure="NO_SAME_FRAME_SEMANTIC_EVIDENCE";
    float depthRasterPixelsX=0,depthRasterPixelsY=0,depthSubmittedPixelsX=0,depthSubmittedPixelsY=0;
    bool depthSubmissionClamped=false;
    std::array<TextureFact,4> textures; // colour, prepared depth, prepared motion, private SR output
    Fact<float> jitterX,jitterY,motionScale,motionScaleX,motionScaleY;
    Fact<MotionEncoding> motionEncoding;
    Fact<ColourDomain> colourDomain;
    Fact<DepthConvention> depthConvention;
    // Only actual clear/projection/phase observations may write these.
    // Requested INI/NGX values and NumericalMaximumWins grant no evidence.
    Fact<float> depthClear,depthToOutputJitterX,depthToOutputJitterY;
    Fact<MotionDirection> motionDirection;
    Fact<JitterUnits> jitterUnits;
    // Real private surfaces are full origin-zero; engine source subrect/producer ABI remain unknown.
    Fact<uint32_t> renderWidth,renderHeight,outputWidth,outputHeight;
    bool engineSubrectKnown=false,producerBuildKnown=false;
    std::shared_ptr<const GenshinNr::ShaderProfile> nrProfile;
    // Optional original-input clear/returned API provenance, never semantic
    // authority. Current-content revocation is separate from immutable history.
    std::shared_ptr<const GenshinReturnedRaster::Evidence> returnedRaster;
    // The actual Bridge observer attaches a same-call downstream correspondence.
    std::array<OutputCorrespondence,2> outputConsumers{};
};
// Sealed SOURCE contract, independent of FG enablement and of final-picture
// mapping. The actual transport packet separately retains the immutable GPU
// copy and submission. No public ready/coverage or projection override API.
class PreparedDepthSource final {
    friend class Receipt;
    Owner owner_{},canonicalOwner_{};
    std::array<TextureFact,4> textures_{};
    DepthSource provenance_{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> prepared_;
    std::shared_ptr<const GenshinLegacy::NativeMrt::Frame> frame_;
    std::shared_ptr<const GenshinCameraHistory::Sample> camera_;
    SrOutputGrid grid_{};
    float outputToRasterX_=0,outputToRasterY_=0;
    std::shared_ptr<const GenshinReturnedRaster::Evidence> returnedRaster_;
    GenshinFsr::Parameters parameters_{};
    // Memoization over immutable same-generation evidence, not mutable source
    // semantics. Only pending CB publication/busy ownership may be retried.
    struct Numerics {
        DepthConvention convention=DepthConvention::Unknown;
        float clear=0,jx=0,jy=0,rasterX=0,rasterY=0,submittedX=0,submittedY=0;
        bool deviceZ=false,phase=false,clamped=false,rasterBasis=false;
        GenshinCameraHistory::Matrix rasterWorldToClip{},rasterProjection{};
        const char* failure="NO_SELECTED_RETURNED_RASTER";
    };
    mutable std::shared_ptr<const Numerics> numerics_;
    void ApplyNumerics(Facts&)const noexcept;
    PreparedDepthSource()=default;
public:
    const Owner& OwnerInterfaces()const noexcept{return owner_;}
    const Owner& OwnerCanonical()const noexcept{return canonicalOwner_;}
    const GenshinLegacy::NativeMrt::Frame& Frame()const noexcept{return *frame_;}
    const GenshinCameraHistory::Sample& Camera()const noexcept{return *camera_;}
    const SrOutputGrid& Grid()const noexcept{return grid_;}
    const auto& Textures()const noexcept{return textures_;}
    uint64_t Identity()const noexcept{return textures_[1].identity;}
    uint32_t Width()const noexcept{return grid_.renderWidth;}
    uint32_t Height()const noexcept{return grid_.renderHeight;}
    // Native prepared raster rows: preparation did not reflect depth. The
    // separate DLSSG row reflection/donor selection is not this source basis.
    bool YFlip()const noexcept{return false;}
    // Camera endpoints alone do NOT describe the texture. DeviceZ and clear
    // are published only by actual clear + complete returned-writer proof.
    DepthConvention Convention()const noexcept{return numerics_?numerics_->convention:DepthConvention::Unknown;}
    float ClearValue()const noexcept{return numerics_?numerics_->clear:0.f;}
    bool NumericsKnown()const noexcept{return numerics_ && numerics_->deviceZ && numerics_->phase;}
    bool MatchesNumerics(const Facts&)const noexcept;
    bool RasterBasisKnown()const noexcept{return NumericsKnown() && numerics_->rasterBasis;}
    const GenshinCameraHistory::Matrix* RasterProjection()const noexcept{return RasterBasisKnown()?&numerics_->rasterProjection:nullptr;}
    float OutputToRasterX()const noexcept{return outputToRasterX_;}
    float OutputToRasterY()const noexcept{return outputToRasterY_;}
    bool Matches(const Facts&)const noexcept;
};
struct Call {
    Owner owner;
    std::array<ID3D11Resource*,4> resources{};
    uint32_t renderWidth=0,renderHeight=0;
    float jitterX=0,jitterY=0,motionScaleX=0,motionScaleY=0;
    bool temporalParametersRead=false;
    OutputRequest outputConsumer{};
};
class Receipt final {
    Facts facts_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>,4> lease_;
    mutable std::atomic<bool> retired_{false};
    std::shared_ptr<const Receipt> producer_; // preserves revocation of a derived consumer receipt
    std::shared_ptr<const GenshinCameraHistory::Sample> depthCamera_;
    std::shared_ptr<const GenshinLegacy::NativeMrt::Frame> depthFrame_;
    GenshinFsr::Parameters depthParameters_{};
    mutable std::optional<SrOutputGrid> outputGrid_;
    mutable std::shared_ptr<const PreparedDepthSource> preparedSource_;
    mutable std::shared_ptr<const GenshinGameMotion::NativeGuideBundle> nativeGuide_;
    friend class GenshinSrReceipt::Scope;
    void RecordOutputGrid(const SrOutputGrid& grid)const noexcept {
        if(GetCurrentThreadId()!=facts_.owner.thread || retired_.load(std::memory_order_acquire) ||
           !grid.Matches(facts_.owner) || outputGrid_)return;
        outputGrid_=grid;
    }
    Receipt()=default;
public:
    // Only a real producer observation constructs the immutable record. No ready/semantic override argument.
    static std::shared_ptr<const Receipt> Observe(ID3D11DeviceContext*,const GenshinFsr::Parameters&,
        const std::array<ID3D11Texture2D*,4>&,uint64_t deviceEpoch,uint64_t producerEpoch,uint64_t configEpoch,
        const GenshinLegacy::NativeMrt::Transaction* native=nullptr,
        std::shared_ptr<const GenshinReturnedRaster::Evidence> returnedRaster={},
        const GenshinCameraHistory::Sample* camera=nullptr,
        ObservationSubscription subscription=ObservationSubscription::Full);
    // Attach only an observed same-owner/same-output consumer correspondence.
    // This does not grant any missing guide semantics or provider success.
    static std::shared_ptr<const Receipt> WithOutputConsumer(std::shared_ptr<const Receipt>,const OutputCorrespondence&);
    const Facts& Get()const noexcept{return facts_;}
    // Consumer-time local enrichment of frozen SAME-FRAME evidence. No ready
    // override, no mutation of original Facts, no borrowing later-frame CBs.
    Facts DepthFactsForPresent()const noexcept;
    std::shared_ptr<const PreparedDepthSource> PreparedSourceForPresent()const noexcept;
    std::optional<SrOutputGrid> OutputGrid()const noexcept;
    GenshinGameMotion::Reference GameMotionReferenceForPresent()const noexcept;
    // Exact native-accumulate facts for FG; does not require a world raster mapping.
    std::shared_ptr<const GenshinGameMotion::NativeGuideBundle> NativeGuideForPresent()const noexcept;
    // Same-call content identity/liveness only. This does not assert temporal,
    // colour transfer, depth, motion or downstream NR/FG semantics.
    bool CurrentResources(const Owner& owner, const std::array<ID3D11Resource*,4>& resources) const noexcept {
        if (retired_.load(std::memory_order_acquire) || GetCurrentThreadId()!=facts_.owner.thread ||
            !SameOwner(owner,facts_.owner) || (producer_ && !producer_->CurrentResources(owner,resources)))return false;
        for (size_t i=0;i<resources.size();++i)
            if (!resources[i] || resources[i]!=static_cast<ID3D11Resource*>(lease_[i].Get()))return false;
        return true;
    }
    Reason Match(const Call&)const noexcept;
    // A preparation ticket validates every guide fact before SR; it grants no completed output.
    Reason Prepare(const Call&,Consumer)const noexcept;
    Reason Admit(const Call&,Consumer,bool actualEvaluateSucceeded)const noexcept;
    // Retirement revokes admission while leases remain held by the GPU owner until completion.
    void Retire()const noexcept {retired_.store(true,std::memory_order_release);}
};
const char* ReasonText(Reason)noexcept;
void PublishRejected(const Owner&,Consumer,Reason)noexcept;
void PublishRejected(const Receipt*,Consumer,Reason)noexcept;
}
