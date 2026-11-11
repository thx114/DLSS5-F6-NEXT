#pragma once
#include <d3d11.h>
#include <cstdint>
#include <memory>
namespace GenshinLegacy::NativeMrt {struct Frame;}
namespace GenshinGuides {class Receipt;}
namespace GenshinGameMotion {
// Live native-consumer ABI observation, independent of numerical NR admission.
// No public booleans or requested motion flags can construct its evidence.
class NativeReference final {
    friend class GenshinGuides::Receipt;
    friend class Reference;
    uintptr_t context_=0,buffer_=0;
    uint64_t revision_=0,loss_=0,deviceEpoch_=0,configEpoch_=0,shader_=0,instance_=0,generation_=0;
    uint32_t sourceFrame_=0,renderWidth_=0,renderHeight_=0;
    uint32_t first_=0,count_=0;
    float renderInvX_=0,renderInvY_=0,outputInvX_=0,outputInvY_=0;
    float cb28x_=0,cb28y_=0;
    float rawJitterX_=0,rawJitterY_=0;
    NativeReference()=default;
public:
    static std::shared_ptr<const NativeReference> Observe(ID3D11DeviceContext*,const GenshinLegacy::NativeMrt::Frame&) noexcept;
    // The original Bridge prefers the current Render token for accepted SR.
    // CB28 may instead repeat the preceding token after a stage transition.
    // PairCompatible certifies either exact relation; neither predicate alone
    // authorizes a pair or supplies a predecessor.
    bool SameFramePhase()const noexcept {return cb28x_==rawJitterX_ && cb28y_==rawJitterY_;}
    bool PreviousFramePhase(const NativeReference& previous)const noexcept {
        return cb28x_==previous.rawJitterX_ && cb28y_==previous.rawJitterY_;
    }
    bool Same(const NativeReference& b)const noexcept {
        return context_==b.context_ && buffer_==b.buffer_ && revision_==b.revision_ && loss_==b.loss_ &&
            deviceEpoch_==b.deviceEpoch_ && configEpoch_==b.configEpoch_ && shader_==b.shader_ &&
            instance_==b.instance_ && generation_==b.generation_ && sourceFrame_==b.sourceFrame_ &&
            renderWidth_==b.renderWidth_ && renderHeight_==b.renderHeight_ && first_==b.first_ && count_==b.count_ &&
            renderInvX_==b.renderInvX_ && renderInvY_==b.renderInvY_ &&
            outputInvX_==b.outputInvX_ && outputInvY_==b.outputInvY_ && cb28x_==b.cb28x_ && cb28y_==b.cb28y_ &&
            rawJitterX_==b.rawJitterX_ && rawJitterY_==b.rawJitterY_;
    }
};
// Sky/clear motion is deliberately absent: NormalizeNative emits the invalid
// sentinel for clear depth, so those MVs are not consumed and cannot certify
// or reject camera correspondence. Depth clear, parallax and direction remain
// mandatory. This pure contract is shared by product and actual-header tests.
inline bool QualifySemanticSample(int eligible,float parallax,int clearSamples,
    int negativeScore,int positiveScore)noexcept {
    const int best=positiveScore>=negativeScore?positiveScore:negativeScore;
    const int other=positiveScore>=negativeScore?negativeScore:positiveScore;
    return eligible>=32 && parallax>=2.f && clearSamples>0 && best>=24 &&
        best*5>=eligible*3 && best>=other+8;
}
}
