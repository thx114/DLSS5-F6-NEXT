// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TaaConstantBuffer.h"
#include <resource_tracking/DrawObservation_Dx11.h>
#include <array>
#include <cmath>
#include <optional>

namespace taa::jitter {
using Microsoft::WRL::ComPtr;
enum class Program { None, MotionPixel, MotionVertex, PackedCameraPixel, PackedCameraVertex };
// A versioned, content-identified layout, verified against PID54276's actual
// DXBC and same-frame scene projections. This is not a generic inference from
// unused constants, an engine name, a changing float or a sampling sequence.
// Source and numerical evidence: docs/F6-STAR-RAIL-TAA-20261116.md.
inline Program Identify(const void* data,size_t size) {
    if(!data||(size!=1060&&size!=580&&size!=1456&&size!=496))return Program::None;
    const auto hash=DrawObservationDx11::ShaderByteHash(data,size);
    if(size==1060&&hash==0x137B32A04AB724F0ull)return Program::MotionPixel;
    if(size==580&&hash==0xF9C1F953EE80980Aull)return Program::MotionVertex;
    if(size==1456&&hash==0xAE2AA729C623837Aull)return Program::PackedCameraPixel;
    if(size==496&&hash==0x98109168E808BC85ull)return Program::PackedCameraVertex;
    return Program::None;
}
struct Offset { float x=0,y=0; };
using Matrix=std::array<std::array<double,4>,4>;
// Row-vector convention. The inverse's XY offsets are checked against a
// centered camera projection and a rigid view with the actual camera origin.
// Both camera handednesses are supported; no current/previous equality gate.
inline std::optional<Offset> Projection(const std::array<float,148>& cb,unsigned width,unsigned height) {
    if(!width||!height||cb[16]!=float(width)||cb[17]!=float(height))return {};
    Matrix inverse{},projection{};
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j) {
        inverse[i][j]=cb[(10+i)*4+j];projection[i][j]=cb[(18+i)*4+j];
        if(!std::isfinite(inverse[i][j])||!std::isfinite(projection[i][j]))return {};
    }
    for(unsigned i=0;i<3;++i)if(!std::isfinite(cb[20+i]))return {};
    const auto a=inverse[0][0],b=inverse[1][1],c=inverse[2][3],f=inverse[3][2];
    if(std::abs(a)<1e-6||std::abs(b)<1e-6||std::abs(c)<1e-6||std::abs(std::abs(f)-1)>1e-6)return {};
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j) {
        const bool used=(i==0&&j==0)||(i==1&&j==1)||(i==2&&j==3)||i==3;
        if(!used&&std::abs(inverse[i][j])>1e-6)return {};
    }
    const double dx=-inverse[3][0]/a,dy=-inverse[3][1]/b;
    const double x=dx*width*.5,y=-dy*height*.5;
    if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>2||std::abs(y)>2)return {};
    double ww=0;for(unsigned i=0;i<3;++i)ww+=projection[i][3]*projection[i][3];
    if(std::abs(ww-1)>2e-4)return {};
    for(unsigned j=0;j<2;++j) {
        double dot=0;for(unsigned i=0;i<3;++i)dot+=projection[i][j]*projection[i][3];
        if(std::abs(dot/ww)>2e-6)return {}; // the baseline is actually centered
    }
    inverse[3][0]=inverse[3][1]=0; // inverse of the verified centered projection
    Matrix view{};
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k)
        view[i][j]+=projection[i][k]*inverse[k][j];
    if(std::abs(view[3][3]-1)>2e-4)return {};
    for(unsigned i=0;i<3;++i) {
        if(std::abs(view[i][3])>2e-4)return {};
        for(unsigned j=0;j<3;++j) {
            double dot=0;for(unsigned k=0;k<3;++k)dot+=view[i][k]*view[j][k];
            if(std::abs(dot-(i==j?1:0))>2e-4)return {};
        }
        double origin=view[3][i];for(unsigned k=0;k<3;++k)origin+=cb[20+k]*view[k][i];
        if(std::abs(origin)>.002)return {};
    }
    // FSR2's documented (+2*x/W,-2*y/H) projection translation is forwarded
    // unchanged to NGX by inputs/FSR2_Dx11.cpp; offsets use render pixels.
    return Offset{float(x),float(y)};
}
struct ProjectionContract { Offset offset; bool depthInverted=false; };
struct PackedProjectionDiagnostics {
    bool examined=false;
    const char* reason="projection not examined";
    unsigned width=0,height=0;
    uint64_t writeSerial=0,constantEpoch=0;
    double offAxisDot=0,normX=0,normY=0,normW=0;
    double depthA=0,depthB=0,nearPlane=0,farPlane=0;
    double productError=0,rasterError=0,offsetX=0,offsetY=0;
};
// The identified camera-motion shader reads device Z through an inverse
// jittered view-projection. Its centered unjittered projection has a rigid view:
// orthogonal spatial XY/W columns and deviceZ=a+b/clipW. I*U must be an XY
// homogeneous translation only. This rejects stale cameras, shear and arbitrary
// asymmetric matrices rather than inferring jitter from a changing float.
inline std::optional<ProjectionContract> PackedProjection(const std::array<float,552>& cb,unsigned width,unsigned height,
                                                           PackedProjectionDiagnostics* diagnostics=nullptr) {
    PackedProjectionDiagnostics local;
    auto& d=diagnostics?*diagnostics:local;
    d={};d.examined=true;d.width=width;d.height=height;
    const auto reject=[&](const char* reason)->std::optional<ProjectionContract>{d.reason=reason;return {};};
    if(!width||!height)return reject("empty extent");
    Matrix inverse{},current{},joined{};
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j) {
        inverse[i][j]=cb[(134+i)*4+j];current[i][j]=cb[(118+i)*4+j];
        if(!std::isfinite(inverse[i][j])||!std::isfinite(current[i][j]))return reject("nonfinite matrix");
    }
    double norms[3]{};constexpr unsigned columns[]{0,1,3};
    for(unsigned a=0;a<3;++a)for(unsigned b=0;b<3;++b) {
        double dot=0;for(unsigned i=0;i<3;++i)dot+=current[i][columns[a]]*current[i][columns[b]];
        if(a==b)norms[a]=dot;else if(std::abs(dot)>2e-4){d.offAxisDot=dot;return reject("nonorthogonal spatial columns");}
    }
    d.normX=norms[0];d.normY=norms[1];d.normW=norms[2];
    if(norms[0]<1e-6||norms[1]<1e-6||std::abs(norms[2]-1)>2e-4)return reject("invalid spatial column norm");
    double a=0;for(unsigned i=0;i<3;++i)a+=current[i][2]*current[i][3];a/=norms[2];
    d.depthA=a;
    for(unsigned i=0;i<3;++i)if(std::abs(current[i][2]-a*current[i][3])>2e-5)return reject("depth column not parallel to W");
    const double b=current[3][2]-a*current[3][3];
    const double nearPlane=b/(1-a),farPlane=-b/a;
    d.depthB=b;d.nearPlane=nearPlane;d.farPlane=farPlane;
    if(!(a<0&&b>0&&nearPlane>0&&farPlane>nearPlane&&std::isfinite(farPlane)))return reject("unsupported depth convention");
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k)joined[i][j]+=inverse[i][k]*current[k][j];
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j) {
        if(i==3&&j<2)continue;
        const double error=std::abs(joined[i][j]-(i==j?1:0));
        if(error>d.productError)d.productError=error;
        if(error>1e-4)return reject("inverse product mismatch");
    }
    const double dx=joined[3][0],dy=joined[3][1];
    const double x=-dx*width*.5,y=dy*height*.5;
    d.offsetX=x;d.offsetY=y;
    if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>2||std::abs(y)>2)return reject("jitter outside supported range");
    Matrix raster=current;
    for(unsigned i=0;i<4;++i){raster[i][0]-=dx*current[i][3];raster[i][1]-=dy*current[i][3];}
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j) {
        double left=0,right=0;for(unsigned k=0;k<4;++k){left+=raster[i][k]*inverse[k][j];right+=inverse[i][k]*raster[k][j];}
        const double error=(std::max)(std::abs(left-(i==j?1:0)),std::abs(right-(i==j?1:0)));
        if(error>d.rasterError)d.rasterError=error;
        if(error>1e-4)return reject("raster inverse mismatch");
    }
    d.reason="accepted";
    return ProjectionContract{{float(x),float(y)},true};
}
struct Sample {
    ComPtr<ID3D11Texture2D> motion,depth;
    ID3D11DeviceContext* context=nullptr;
    uint64_t frame=0,epoch=0,revision=0,constantEpoch=0,writeSerial=0;
    DWORD thread=0;
    unsigned width=0,height=0;
    DXGI_FORMAT motionFormat=DXGI_FORMAT_UNKNOWN;
    Offset offset;
    std::optional<bool> validatedDepthConvention;
    bool Matches(ID3D11DeviceContext* c,uint64_t r,ID3D11Texture2D* m,ID3D11Texture2D* d,
        unsigned w,unsigned h,DXGI_FORMAT format) const {
        const auto& owner=DrawObservationDx11::owner;
        return r&&revision==r&&context==c&&thread==GetCurrentThreadId()&&owner&&owner->Matches(c)&&
            owner->frame==frame&&owner->epoch==epoch&&constantEpoch==constants::ConstantEpoch(c)&&
            motion.Get()==m&&depth.Get()==d&&width==w&&height==h&&motionFormat==format;
    }
};
struct Input { const Sample* sample=nullptr;uint64_t revision=0; };
// One live sample on the immediate owner thread. No diagnostic time window,
// registry lock, cross-frame float cache, GPU wait or completion authority.
class Tracker final {
    std::optional<Sample> pending_,ready_;
    uint64_t serial_=0,frame_=0,epoch_=0,revision_=0;
    unsigned calls_=0;
    const char* error_="producer not observed";
    PackedProjectionDiagnostics packedDiagnostics_;
    uint64_t Reject(const char* reason){error_=reason;return 0;}
    uint64_t BeforePacked(ID3D11DeviceContext* context,const DrawObservationDx11::Draw& draw,
        const DrawObservationDx11::Owner& owner,uint64_t revision) {
        pending_.reset();ready_.reset();packedDiagnostics_={};
        if(++calls_!=1)return Reject("multiple camera producers in owner frame");
        if(!revision||!owner.Matches(context)||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE||
            draw.kind!=DrawObservationDx11::Kind::Draw||!draw.countKnown||draw.count!=3||draw.instances!=1||draw.start)return Reject("producer owner or draw contract");
        ComPtr<ID3D11DeviceContext1> c;if(FAILED(context->QueryInterface(IID_PPV_ARGS(&c))))return Reject("D3D11.1 context unavailable");
        std::array<ID3D11RenderTargetView*,2> raw{};context->OMGetRenderTargets(2,raw.data(),nullptr);
        std::array<ComPtr<ID3D11RenderTargetView>,2> outputs;for(unsigned i=0;i<2;++i)outputs[i].Attach(raw[i]);
        if(!outputs[1])return Reject("packed motion MRT1 missing");
        D3D11_RENDER_TARGET_VIEW_DESC rt{};outputs[1]->GetDesc(&rt);
        ComPtr<ID3D11ShaderResourceView> input;context->PSGetShaderResources(0,1,&input);if(!input)return Reject("native depth t0 missing");
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};input->GetDesc(&srv);
        if(rt.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D||rt.Texture2D.MipSlice||rt.Format!=DXGI_FORMAT_R10G10B10A2_UNORM||
            srv.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D||srv.Texture2D.MostDetailedMip||srv.Format!=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS)return Reject("packed producer typed view contract");
        Sample sample;ComPtr<ID3D11Resource> resource;outputs[1]->GetResource(&resource);if(FAILED(resource.As(&sample.motion)))return Reject("motion Texture2D missing");
        resource.Reset();input->GetResource(&resource);if(FAILED(resource.As(&sample.depth)))return Reject("depth Texture2D missing");
        D3D11_TEXTURE2D_DESC md{},dd{};sample.motion->GetDesc(&md);sample.depth->GetDesc(&dd);
        packedDiagnostics_.width=md.Width;packedDiagnostics_.height=md.Height;
        if(md.Width!=dd.Width||md.Height!=dd.Height||md.MipLevels!=1||dd.MipLevels!=1||md.ArraySize!=1||dd.ArraySize!=1||md.SampleDesc.Count!=1||dd.SampleDesc.Count!=1)return Reject("producer resource layout mismatch");
        std::array<D3D11_VIEWPORT,16> views{};UINT count=UINT(views.size());context->RSGetViewports(&count,views.data());
        if(count!=1||views[0].TopLeftX||views[0].TopLeftY||views[0].Width!=float(md.Width)||views[0].Height!=float(md.Height))return Reject("partial producer viewport");
        ComPtr<ID3D11Buffer> buffer;UINT first=0,length=0;c->PSGetConstantBuffers1(0,1,&buffer,&first,&length);
        if(!buffer||length*16<2208)return Reject("producer CB0 range missing");
        std::vector<unsigned char> bytes,known;
        if(!constants::Snapshot(buffer.Get(),first*16,2208,bytes,known,&sample.writeSerial,&sample.constantEpoch)||!sample.writeSerial||std::find(known.begin(),known.end(),0)!=known.end())return Reject("producer CB0 contents unknown");
        std::array<float,552> values{};std::memcpy(values.data(),bytes.data(),sizeof(values));
        const auto contract=PackedProjection(values,md.Width,md.Height,&packedDiagnostics_);
        packedDiagnostics_.writeSerial=sample.writeSerial;packedDiagnostics_.constantEpoch=sample.constantEpoch;
        if(!contract)return Reject(packedDiagnostics_.reason);
        sample.context=context;sample.frame=owner.frame;sample.epoch=owner.epoch;sample.thread=owner.thread;sample.revision=revision;
        sample.width=md.Width;sample.height=md.Height;sample.motionFormat=rt.Format;sample.offset=contract->offset;
        sample.validatedDepthConvention=contract->depthInverted;pending_=std::move(sample);error_="ready after physical producer returns";return ++serial_;
    }
public:
    void Reset() {pending_.reset();ready_.reset();frame_=epoch_=revision_=0;calls_=0;packedDiagnostics_={};}
    unsigned Calls()const{return calls_;}
    const char* Error()const{return error_;}
    const PackedProjectionDiagnostics& PackedDiagnostics()const{return packedDiagnostics_;}
    const Sample* Current() const {return ready_?&*ready_:nullptr;}
    uint64_t Before(ID3D11DeviceContext* context,const DrawObservationDx11::Draw& draw,
        const DrawObservationDx11::Owner& owner,uint64_t revision,Program pixel,Program vertex) {
        if(frame_!=owner.frame||epoch_!=owner.epoch||revision_!=revision) {
            Reset();frame_=owner.frame;epoch_=owner.epoch;revision_=revision;
        }
        if(pixel==Program::PackedCameraPixel&&vertex==Program::PackedCameraVertex)return BeforePacked(context,draw,owner,revision);
        if(pixel!=Program::MotionPixel||vertex!=Program::MotionVertex)return 0;
        pending_.reset();ready_.reset();
        if(++calls_!=1||!revision||!owner.Matches(context)||context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE||
            (draw.kind!=DrawObservationDx11::Kind::Draw&&draw.kind!=DrawObservationDx11::Kind::Indexed)||
            !draw.countKnown||draw.count!=3||draw.start||draw.base||draw.instances!=1)return 0;
        ComPtr<ID3D11DeviceContext1> c;if(FAILED(context->QueryInterface(IID_PPV_ARGS(&c))))return 0;
        ComPtr<ID3D11RenderTargetView> output;ComPtr<ID3D11DepthStencilView> depth;
        context->OMGetRenderTargets(1,&output,&depth);if(!output||!depth)return 0;
        D3D11_RENDER_TARGET_VIEW_DESC rt{};output->GetDesc(&rt);
        D3D11_DEPTH_STENCIL_VIEW_DESC ds{};depth->GetDesc(&ds);
        if(rt.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D||rt.Texture2D.MipSlice||
            ds.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D||ds.Texture2D.MipSlice)return 0;
        Sample sample;ComPtr<ID3D11Resource> resource;output->GetResource(&resource);
        if(FAILED(resource.As(&sample.motion)))return 0;
        resource.Reset();depth->GetResource(&resource);if(FAILED(resource.As(&sample.depth)))return 0;
        D3D11_TEXTURE2D_DESC md{},dd{};sample.motion->GetDesc(&md);sample.depth->GetDesc(&dd);
        if(md.SampleDesc.Count!=1||md.MipLevels!=1||md.ArraySize!=1||dd.SampleDesc.Count!=1||dd.MipLevels!=1||dd.ArraySize!=1||
            md.Width!=dd.Width||md.Height!=dd.Height||rt.Format!=DXGI_FORMAT_R16G16_FLOAT)return 0;
        UINT count=16;std::array<D3D11_VIEWPORT,16> views{};context->RSGetViewports(&count,views.data());
        if(count!=1||views[0].TopLeftX||views[0].TopLeftY||views[0].Width!=float(md.Width)||views[0].Height!=float(md.Height))return 0;
        ComPtr<ID3D11GeometryShader> gs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> domain;
        context->GSGetShader(&gs,nullptr,nullptr);context->HSGetShader(&hs,nullptr,nullptr);context->DSGetShader(&domain,nullptr,nullptr);
        if(gs||hs||domain)return 0;
        ComPtr<ID3D11Buffer> buffer;UINT first=0,length=0;c->PSGetConstantBuffers1(0,1,&buffer,&first,&length);
        if(!buffer||length*16<592)return 0;
        std::vector<unsigned char> bytes,known;
        if(!constants::Snapshot(buffer.Get(),first*16,592,bytes,known,&sample.writeSerial,&sample.constantEpoch)||
            !sample.writeSerial||std::find(known.begin(),known.end(),0)!=known.end())return 0;
        std::array<float,148> values{};std::memcpy(values.data(),bytes.data(),sizeof(values));
        const auto offset=Projection(values,md.Width,md.Height);if(!offset)return 0;
        sample.context=context;sample.frame=owner.frame;sample.epoch=owner.epoch;sample.thread=owner.thread;
        sample.revision=revision;sample.width=md.Width;sample.height=md.Height;sample.motionFormat=rt.Format;sample.offset=*offset;
        pending_=std::move(sample);return ++serial_;
    }
    void After(uint64_t cookie,DrawObservationDx11::Completion completion,uint64_t revision) {
        if(!cookie||cookie!=serial_||!pending_)return;
        auto next=std::move(*pending_);pending_.reset();
        if(completion==DrawObservationDx11::Completion::OriginalReturned&&next.revision==revision&&
            next.Matches(next.context,revision,next.motion.Get(),next.depth.Get(),next.width,next.height,next.motionFormat))ready_=std::move(next);
    }
};
} // namespace taa::jitter
