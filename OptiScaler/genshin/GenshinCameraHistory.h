// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "legacy/Il2CppCallSiteHook.h"
#include "GenshinOutputContract.h"
#include "GenshinSrOutputGrid.h"
#include <DirectXMath.h>
#include <sl_consts.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <optional>
#include <utility>

// Pure CPU adaptation of the pinned, actual camera observation. This file
// never issues DeviceZ Facts, obtains another token, or dispatches GPU work.
namespace GenshinCameraHistory {
using namespace DirectX;
using Matrix=std::array<float,16>;
// Bit checks remain meaningful under the product /fp:fast compiler mode.
inline bool Finite(float v) noexcept {return (std::bit_cast<uint32_t>(v)&0x7f800000u)!=0x7f800000u;}
inline bool Finite(const Matrix& a) noexcept {
    float max=0;for(float v:a){if(!Finite(v))return false;max=(std::max)(max,std::fabs(v));}
    return max>0;
}
inline XMMATRIX Load(const Matrix& a) noexcept {
    XMFLOAT4X4 f;std::memcpy(&f,a.data(),64);return XMLoadFloat4x4(&f);
}
inline Matrix Store(FXMMATRIX a) noexcept {
    XMFLOAT4X4 f;XMStoreFloat4x4(&f,a);Matrix out;std::memcpy(out.data(),&f,64);return out;
}
inline Matrix Read(const float (&a)[16]) noexcept {Matrix out;std::memcpy(out.data(),a,64);return out;}
inline bool Inverse(const Matrix& a,Matrix& out) noexcept {
    if(!Finite(a))return false;XMVECTOR determinant;out=Store(XMMatrixInverse(&determinant,Load(a)));
    const float d=XMVectorGetX(determinant);
    return Finite(d) && std::fabs(d)>1.e-12f && Finite(out);
}
inline Matrix Multiply(const Matrix& a,const Matrix& b) noexcept {return Store(XMMatrixMultiply(Load(a),Load(b)));}
inline bool Near(float a,float b,float epsilon=2.e-4f) noexcept {
    return Finite(a) && Finite(b) && std::fabs(a-b)<=epsilon*((std::max)(1.0f,(std::max)(std::fabs(a),std::fabs(b))));
}
inline void ToSdk(const Matrix& a,sl::float4x4& out) noexcept {static_assert(sizeof(out)==64);std::memcpy(&out,a.data(),64);}
struct Sample {
    GenshinGuides::Owner owner{};
    il2cpp_callsite::CameraSnapshot actual{};
    Matrix projection{},projectionJittered{},worldToClip{},viewToWorld{};
    // Effective perspective parameters derived from the observed projection for
    // RenderObservation; engine getter values only for ConfigureSetter.
    float nearPlane=0,farPlane=0,fovRadians=0,aspect=0;
    bool reversed=false,valid=false;
};
// Versioned GPU adapter/caller dependencies are authenticated by the producer.
// October: 0x14FFA50/0x1856B36; November: 0x1510C90/0x186AB86, vtable+0x178.
// Inputs are the actual config E0/E1 bytes captured with the producer. Array
// layout remains Unity column-major; interpreted as DirectX rows this is the
// exact transpose required for row-vector multiplication in sl::Constants.
inline Matrix GpuProjection(Matrix p,unsigned char clip,unsigned char reverse) noexcept {
    if(clip){if(reverse)for(unsigned c=0;c<4;++c)p[c*4+2]=-p[c*4+2];}
    else for(unsigned c=0;c<4;++c){p[c*4+1]=-p[c*4+1];p[c*4+2]=.5f*(p[c*4+3]+(reverse?-p[c*4+2]:p[c*4+2]));}
    return p;
}
inline std::optional<Sample> Adapt(const il2cpp_callsite::RenderToken& token,
    const GenshinGuides::Owner& owner) noexcept {
    const auto& a=token.camera;const auto& p=token.params;
    if(!a.gpuProfileValid || !a.graphicsDevice || !a.cpuValid || !a.nativeCamera ||
       !a.managedCamera || a.captureEpoch==UINT64_MAX || a.instance!=p.instance || a.renderGeneration!=token.generation ||
       a.sourceFrame!=p.frame_index || a.captureTick>token.capture_tick || token.capture_tick-a.captureTick>250 ||
       owner.instance!=p.instance || owner.generation!=token.generation || owner.sourceFrame!=p.frame_index ||
       !owner.device || !owner.context || !owner.deviceEpoch || !owner.producerEpoch || !owner.configEpoch ||
       !owner.historyEpoch || !owner.thread || !a.renderThread)return {};
    if(a.provenance==il2cpp_callsite::CameraProvenance::RenderObservation) {
        // Passive slot observations are candidates only. Resolve must still
        // match the unchanged P/V to every actual returned GPU depth writer.
        // Render captures on its own engine thread; the exact frozen token is
        // consumed later by the separately authenticated D3D owner thread.
        // Bind the observed slot/thread to that same callback, never to an
        // unrelated latest sample or the consumer thread by coincidence.
        if(!token.captureThread || a.renderThread!=token.captureThread ||
           !a.observationSerial || a.observationContext!=p.context || !a.observationContext ||
           !a.observationOwner || !a.primaryManagedCamera || a.selectedManagedCamera!=a.managedCamera ||
           !a.observedMainThread || a.cpuNoTaaValid)return {};
        const auto* profile=GenshinBridgeRecipes::CameraLayout(a.observedProfile);
        if(!profile)return {};
        const bool main=a.renderThread==a.observedMainThread;
        if(a.observedSlotMode!=(main?il2cpp_callsite::CameraSlotMode::MainCache:il2cpp_callsite::CameraSlotMode::WorkerMirror) ||
           a.observedProjectionSlot!=a.nativeCamera+(main?profile->mainProjection:profile->workerProjection) ||
           a.observedViewSlot!=a.nativeCamera+(main?profile->mainView:profile->workerView))return {};
        Sample s;s.owner=owner;s.actual=a;
        s.projectionJittered=GpuProjection(Read(a.cpuStoredProjection),a.gpuClipControl,a.gpuReverseControl);
        const auto view=Read(a.cpuWorldToView);
        if(!Inverse(view,s.viewToWorld) || !Finite(s.projectionJittered))return {};
        const auto& q=s.projectionJittered;
        // Canonical finite perspective only; reject oblique Z, orthographic,
        // infinite far and unexpected XY/W terms instead of inventing scalars.
        for(unsigned i:{1u,2u,3u,4u,6u,7u,12u,13u,15u})if(q[i]!=0)return {};
        if(q[11]!=-1 || q[0]==0 || q[5]==0 || q[10]==0 || q[10]==-1)return {};
        const float atZero=q[14]/q[10],atOne=q[14]/(1+q[10]);
        if(!Finite(atZero) || !Finite(atOne) || atZero<=0 || atOne<=0 || atZero==atOne)return {};
        s.nearPlane=(std::min)(atZero,atOne);s.farPlane=(std::max)(atZero,atOne);
        s.reversed=atOne<atZero;
        s.fovRadians=2*std::atan(1/std::fabs(q[5]));s.aspect=std::fabs(q[5]/q[0]);
        if(!Finite(s.fovRadians) || s.fovRadians<=0 || s.fovRadians>=3.14159265358979323846f ||
           !Finite(s.aspect) || s.aspect<=0)return {};
        // No noTAA matrix is asserted here. ForSrOutput derives the effective
        // output reference solely from the actual accepted DLSS jitter grid.
        s.projection=s.projectionJittered;s.worldToClip=Multiply(view,s.projection);
        s.valid=Finite(s.worldToClip);return s.valid?std::optional<Sample>(s):std::nullopt;
    }
    if(a.provenance!=il2cpp_callsite::CameraProvenance::ConfigureSetter || a.renderThread!=owner.thread || !a.captureEpoch || !a.cpuNoTaaValid ||
       !a.configureSerial || !a.configureThread || !Finite(a.nearPlane) || !Finite(a.farPlane) ||
       a.nearPlane<=0 || a.farPlane<=a.nearPlane || !Finite(a.fovDegrees) || a.fovDegrees<=0 ||
       a.fovDegrees>=180 || !Finite(a.aspect) || a.aspect<=0)return {};
    auto pj=Read(a.cpuProjectionJittered),stored=Read(a.cpuStoredProjection),pn=Read(a.cpuProjectionNoTaa);
    Matrix inversePj;if(!Inverse(pj,inversePj) || !Finite(stored) || !Finite(pn))return {};
    // Native setter stores C*Pj. Recover the OBSERVED correction; do not assume
    // C=identity or remove the current jitter from an unrelated getter slot.
    const auto correction=Multiply(inversePj,stored);
    // 0x19B3060 changes X/Y only, preserving clip Z/W. Reject unexpected C.
    for(unsigned c=0;c<4;++c)if(!Near(correction[c*4+2],c==2?1.0f:0.0f) ||
        !Near(correction[c*4+3],c==3?1.0f:0.0f))return {};
    Sample s;s.owner=owner;s.actual=a;s.nearPlane=a.nearPlane;s.farPlane=a.farPlane;
    s.fovRadians=a.fovDegrees*(3.14159265358979323846f/180.0f);s.aspect=a.aspect;
    const auto noTaaStored=Multiply(pn,correction);
    const auto witnessedNoTaa=Read(a.cpuStoredProjectionNoTaa);
    for(unsigned i=0;i<16;++i)if(!Near(noTaaStored[i],witnessedNoTaa[i]))return {};
    // Preserve observed clip Z/W bits after validating the reconstructed correction.
    s.projection=GpuProjection(witnessedNoTaa,a.gpuClipControl,a.gpuReverseControl);
    s.projectionJittered=GpuProjection(stored,a.gpuClipControl,a.gpuReverseControl);
    const auto view=Read(a.cpuWorldToView);s.viewToWorld=Read(a.cpuViewToWorld);
    if(!Finite(view) || !Finite(s.viewToWorld) || !Finite(s.projection) || !Finite(s.projectionJittered))return {};
    const auto inverseCheck=Multiply(view,s.viewToWorld);
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)if(!Near(inverseCheck[r*4+c],r==c?1.0f:0.0f))return {};
    // Authenticate the actual perspective endpoints. This validates CAMERA
    // projection only; no depth texture or writer semantics follow from it.
    auto depth=[&](float distance){const float z=-distance;const auto& q=s.projection;
        const float w=z*q[11]+q[15];return (z*q[10]+q[14])/w;};
    const float nearZ=depth(a.nearPlane),farZ=depth(a.farPlane);
    const bool forward=Near(nearZ,0) && Near(farZ,1),reversed=Near(nearZ,1) && Near(farZ,0);
    if((!forward && !reversed) || s.projection[11]!=-1.0f || s.projection[15]!=0.0f)return {};
    s.reversed=reversed;s.worldToClip=Multiply(view,s.projection);s.valid=Finite(s.worldToClip);
    return s.valid?std::optional<Sample>(s):std::nullopt;
}
// DLSS jitter has the same signed screen-pixel displacement as the actual
// raster projection (NVIDIA sample PlanarView + SL->NGX callsite receipts).
// Remove the ACCEPTED offset from Pj to obtain the SR output reference. Raw
// noTAA and the bridge's mode-3 .5 are never assumed to be that reference.
inline std::optional<Sample> ForSrOutput(std::optional<Sample> source,
    const std::optional<GenshinGuides::SrOutputGrid>& grid) noexcept {
    if(!source || !source->valid || !grid || !grid->Matches(source->owner))return {};
    auto out=*source;out.projection=out.projectionJittered;
    const float dx=2*grid->jitterPixelsX/grid->renderWidth,dy=-2*grid->jitterPixelsY/grid->renderHeight;
    if(!Finite(dx) || !Finite(dy))return {};
    for(unsigned r=0;r<4;++r){out.projection[r*4]-=dx*out.projection[r*4+3];
        out.projection[r*4+1]-=dy*out.projection[r*4+3];}
    Matrix view;if(!Inverse(out.viewToWorld,view) || !Finite(out.projection))return {};
    out.worldToClip=Multiply(view,out.projection);out.valid=Finite(out.worldToClip);
    return out.valid?std::optional<Sample>(out):std::nullopt;
}
inline bool SameLineage(const Sample& a,const Sample& b) noexcept {
    const auto& x=a.owner;const auto& y=b.owner;
    return a.valid && b.valid && x.device==y.device && x.context==y.context && x.deviceEpoch==y.deviceEpoch &&
        x.producerEpoch==y.producerEpoch && x.configEpoch==y.configEpoch && x.instance==y.instance &&
        x.historyEpoch==y.historyEpoch && x.thread==y.thread && a.reversed==b.reversed &&
        a.actual.nativeCamera==b.actual.nativeCamera && a.actual.managedCamera==b.actual.managedCamera &&
        a.actual.captureEpoch==b.actual.captureEpoch && a.actual.graphicsDevice==b.actual.graphicsDevice &&
        a.actual.gpuClipControl==b.actual.gpuClipControl && a.actual.gpuReverseControl==b.actual.gpuReverseControl &&
        a.actual.provenance==b.actual.provenance &&
        (a.actual.provenance!=il2cpp_callsite::CameraProvenance::RenderObservation ||
         (a.actual.observationContext==b.actual.observationContext && a.actual.observationOwner==b.actual.observationOwner &&
          a.actual.primaryManagedCamera==b.actual.primaryManagedCamera && a.actual.selectedManagedCamera==b.actual.selectedManagedCamera &&
          a.actual.selector==b.actual.selector && a.actual.observedProjectionSlot==b.actual.observedProjectionSlot &&
          a.actual.observedViewSlot==b.actual.observedViewSlot &&
          a.actual.observedMainThread==b.actual.observedMainThread && a.actual.observedSlotMode==b.actual.observedSlotMode));
}
class History final {
    std::optional<Sample> previous_,current_;
    std::optional<sl::Constants> constants_;
    bool currentCertified_=false,previousCertified_=false;
    void RebuildConstants() noexcept {
        constants_.reset();
        if(!current_ || !previous_ || !currentCertified_ || !previousCertified_)return;
        const auto& a=*current_;const auto& b=*previous_;
        if(!SameLineage(b,a))return;
        const bool render=a.actual.provenance==il2cpp_callsite::CameraProvenance::RenderObservation;
        const auto serialA=render?a.actual.observationSerial:a.actual.configureSerial;
        const auto serialB=render?b.actual.observationSerial:b.actual.configureSerial;
        // These are adjacent successful transactions from the original SR
        // producer. Global game-frame numbers can jump at stage boundaries;
        // wall-clock stalls do not change two immutable camera matrices.
        if(a.owner.sourceFrame<=b.owner.sourceFrame || b.owner.generation==UINT64_MAX ||
           a.owner.generation!=b.owner.generation+1 || !serialA || serialA<=serialB ||
           a.actual.captureTick<b.actual.captureTick)return;
        Matrix invCurrent,invP,reverseTransform;
        if(!Inverse(a.worldToClip,invCurrent) || !Inverse(a.projection,invP))return;
        const auto toPrevious=Multiply(invCurrent,b.worldToClip);
        if(!Inverse(toPrevious,reverseTransform))return;
        sl::Constants c{};ToSdk(a.projection,c.cameraViewToClip);ToSdk(invP,c.clipToCameraView);
        ToSdk(toPrevious,c.clipToPrevClip);ToSdk(reverseTransform,c.prevClipToClip);
        const auto& v=a.viewToWorld;
        c.cameraPos={v[12],v[13],v[14]};c.cameraRight={v[0],v[1],v[2]};
        c.cameraUp={v[4],v[5],v[6]};c.cameraFwd={-v[8],-v[9],-v[10]};
        c.cameraNear=a.nearPlane;c.cameraFar=a.farPlane;
        c.cameraFOV=a.fovRadians;c.cameraAspectRatio=a.aspect;
        c.depthInverted=a.reversed?sl::Boolean::eTrue:sl::Boolean::eFalse;
        c.orthographicProjection=sl::Boolean::eFalse;c.reset=sl::Boolean::eFalse;
        // Final-picture jitter and motion fields are assigned by the actual
        // final guide producer. No raw UV jitter becomes SDK pixel jitter here.
        constants_=c;
    }
public:
    void Clear() noexcept {previous_.reset();current_.reset();constants_.reset();
        currentCertified_=previousCertified_=false;}
    // Successful SR/copyback alone never certifies camera-to-depth semantics.
    void Commit(std::optional<Sample> sample,bool reset) noexcept {
        constants_.reset();previous_=current_;previousCertified_=currentCertified_;
        current_=std::move(sample);currentCertified_=false;
        if(!current_ || reset){previous_.reset();previousCertified_=false;}
    }
    // The sole product callsite follows PreparedDepth and exact DepthMatches
    // on the same successful canonical History. Both retained samples need
    // their own returned-writer/mapped-depth certification before FG use.
    bool Certify(const GenshinGuides::Owner& owner) noexcept {
        if(!current_ || !GenshinGuides::SameOwner(current_->owner,owner))return false;
        currentCertified_=true;RebuildConstants();return true;
    }
    const Sample* Current()const noexcept {return current_?&*current_:nullptr;}
    const GenshinGuides::Owner* PredecessorFor(const GenshinGuides::Owner& owner)const noexcept {
        return For(owner) && previous_?&previous_->owner:nullptr;
    }
    const sl::Constants* For(const GenshinGuides::Owner& owner)const noexcept {
        return current_ && constants_ && GenshinGuides::SameOwner(current_->owner,owner)?&*constants_:nullptr;
    }
};
}
