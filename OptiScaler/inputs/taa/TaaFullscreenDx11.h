// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TaaGeometryReadbackDx11.h"
#include <d3d11.h>
#include <array>
#include <vector>
#include <cmath>
#include <climits>
#include <limits>
#include <string_view>
#include <limits>
namespace taa::fullscreen {
inline constexpr GUID LayoutKey={0xf6aa0004,0x928b,0x4cb7,{0xa5,0x91,0xda,0x11,0x71,0x21,0x83,0x05}};
struct Position {unsigned slot=0,offset=0;bool valid=false;};
inline unsigned Bytes(DXGI_FORMAT f){switch(f){case DXGI_FORMAT_R32G32_FLOAT:return 8;case DXGI_FORMAT_R32G32B32_FLOAT:return 12;case DXGI_FORMAT_R32G32B32A32_FLOAT:return 16;case DXGI_FORMAT_R32_FLOAT:case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UINT:return 4;default:return 0;}}
inline void Remember(ID3D11InputLayout* layout,const D3D11_INPUT_ELEMENT_DESC* elements,unsigned count) {
    if(!layout||!elements||count>32)return;
    Position p;std::array<unsigned,32> ends{};bool known=true;
    for(unsigned i=0;i<count;++i){const auto& e=elements[i];if(e.InputSlot>=32){known=false;break;}const auto size=Bytes(e.Format);if(!size){known=false;break;}
        const auto offset=e.AlignedByteOffset==D3D11_APPEND_ALIGNED_ELEMENT?ends[e.InputSlot]:e.AlignedByteOffset;ends[e.InputSlot]=offset+size;
        if(e.SemanticName&&std::string_view(e.SemanticName)=="POSITION"&&e.SemanticIndex==0){if(p.valid){known=false;break;}
            p={e.InputSlot,offset,e.InputSlotClass==D3D11_INPUT_PER_VERTEX_DATA&&!e.InstanceDataStepRate&&size>=8};}}
    if(!known)p.valid=false;layout->SetPrivateData(LayoutKey,sizeof(p),&p);
}
struct Point {double x=0,y=0;};
// An ordered position program, never an algebraically folded transform.
// Only exact normal float32 arithmetic is admitted for the actual VB values.
#if defined(_MSC_VER)
#pragma float_control(precise,on,push)
#endif
struct PositionTransform {
    enum class Source { VertexBuffer, VertexIdTriangle };
    Source source=Source::VertexBuffer;
    enum class Op { Literal, Input, Constant, Add, Mul };
    struct Step {Op op=Op::Literal;unsigned a=0,b=0,lane=0,slot=0,offset=0;float literal=0;bool varying=false;};
    std::vector<Step> steps;
    std::array<unsigned,4> outputs{};
    bool BindConstants(ID3D11DeviceContext* context) {
        Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1;
        if(context)context->QueryInterface(IID_PPV_ARGS(&context1));
        std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>,14> buffers;
        std::array<UINT,14> first{},count{};std::array<bool,14> read{};
        for(auto& step:steps)if(step.op==Op::Constant) {
            if(!context1||step.slot>=14)return false;
            if(!read[step.slot]) {context1->VSGetConstantBuffers1(step.slot,1,&buffers[step.slot],&first[step.slot],&count[step.slot]);read[step.slot]=true;}
            const uint64_t at=uint64_t(first[step.slot])*16+step.offset;
            if(!buffers[step.slot]||uint64_t(step.offset)+4>uint64_t(count[step.slot])*16||at>UINT_MAX)return false;
            std::vector<unsigned char> data,known;
            if(!constants::Snapshot(buffers[step.slot].Get(),unsigned(at),4,data,known)||data.size()!=4||known.size()!=4||
                std::find(known.begin(),known.end(),0)!=known.end())return false;
            std::memcpy(&step.literal,data.data(),4);
            if(!std::isfinite(step.literal)||(step.literal!=0&&std::abs(step.literal)<(std::numeric_limits<float>::min)()))return false;
            step.op=Op::Literal;
        }
        return true;
    }
    bool Apply(const float* xy,Point& point) const {
        if(steps.empty()||steps.size()>64)return false;
        std::array<float,64> values{};
        for(unsigned i=0;i<steps.size();++i) {
            const auto& s=steps[i];double value=0;
            if(s.op==Op::Literal)value=s.literal;
            else if(s.op==Op::Input) {if(s.lane>=2)return false;value=xy[s.lane];}
            else if(s.op==Op::Constant)return false;
            else {
                if(s.a>=i||s.b>=i)return false;
                const double a=values[s.a],b=values[s.b];
                value=s.op==Op::Add?a+b:a*b;
                // A large exponent gap must not hide a lost addend even in
                // double. No CPU/GPU rounding or fused-MAD ambiguity is used
                // as authority to replace a physical consumer.
                if(s.op==Op::Add && ((value-a)!=b || (value-b)!=a))return false;
            }
            const float rounded=static_cast<float>(value);
            if(!std::isfinite(rounded)||double(rounded)!=value ||
               (rounded!=0&&std::abs(rounded)<(std::numeric_limits<float>::min)()))return false;
            values[i]=rounded;
        }
        for(const auto output:outputs)if(output>=steps.size())return false;
        if(values[outputs[2]]<0||values[outputs[2]]>1||values[outputs[3]]!=1)return false;
        point={values[outputs[0]],values[outputs[1]]};return true;
    }
};
#if defined(_MSC_VER)
#pragma float_control(pop)
#endif
inline double Cross(Point a,Point b,Point c){return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);}
inline double Area(const std::vector<Point>& p){double area=0;for(size_t i=0;i<p.size();++i){auto a=p[i],b=p[(i+1)%p.size()];area+=a.x*b.y-a.y*b.x;}return std::abs(area)*.5;}
inline std::vector<Point> Clip(std::vector<Point> source,const std::vector<Point>& bounds){
    for(size_t i=0;i<bounds.size()&&!source.empty();++i){std::vector<Point> next;const auto a=bounds[i],b=bounds[(i+1)%bounds.size()];
        auto previous=source.back();double pd=Cross(a,b,previous);
        for(const auto current:source){const double cd=Cross(a,b,current);if((pd>=0)!=(cd>=0)){const double t=pd/(pd-cd);next.push_back({previous.x+t*(current.x-previous.x),previous.y+t*(current.y-previous.y)});}if(cd>=0)next.push_back(current);previous=current;pd=cd;}source=std::move(next);
    }return source;
}
inline bool Geometry(ID3D11DeviceContext* context,bool indexed,unsigned count,unsigned first,int base,Evidence* evidence=nullptr,const PositionTransform* transform=nullptr) {
    if(!context)return false;
    if(count!=3&&count!=4&&count!=6)return false;
    std::array<Point,6> vertices{};
    if(transform&&transform->source==PositionTransform::Source::VertexIdTriangle) {
        // This program reads only SV_VertexID. No bound layout/VB can prove
        // or invalidate it; the actual IDs and common raster proof do.
        // Indexed procedural semantics are not part of this contract.
        if(indexed||base)return false;
        for(unsigned i=0;i<count;++i) {
            const uint64_t id=uint64_t(first)+i;if(id>UINT_MAX)return false;
            vertices[i]={double((id&1)*4)-1,double((id&2)*2)-1};
        }
    } else {
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;context->IAGetInputLayout(&layout);Position position;UINT size=sizeof(position);
    if(!layout||FAILED(layout->GetPrivateData(LayoutKey,&size,&position))||size!=sizeof(position)||!position.valid)return false;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;UINT stride=0,offset=0;context->IAGetVertexBuffers(position.slot,1,&buffer,&stride,&offset);
    if(!buffer||!stride||position.offset+8>stride)return false;
    Microsoft::WRL::ComPtr<ID3D11Buffer> indices;DXGI_FORMAT indexFormat{};UINT indexOffset=0;
    if(indexed){context->IAGetIndexBuffer(&indices,&indexFormat,&indexOffset);if(!indices||(indexFormat!=DXGI_FORMAT_R16_UINT&&indexFormat!=DXGI_FORMAT_R32_UINT))return false;}
    const auto read=[&](ID3D11Buffer* source,unsigned at,unsigned bytes,void* value,bool index=false){
        std::vector<unsigned char> data,known;
        const auto available=[&]{return constants::Snapshot(source,at,bytes,data,known)&&std::find(known.begin(),known.end(),0)==known.end();};
        if(!available()&&(!evidence||!evidence->Recover(source,index)||!available()))return false;
        std::memcpy(value,data.data(),bytes);return true;
    };
    for(unsigned i=0;i<count;++i){int64_t index=int64_t(first)+i;
        if(indexed){unsigned raw=0;const unsigned length=indexFormat==DXGI_FORMAT_R16_UINT?2:4;const uint64_t at=indexOffset+uint64_t(index)*length;if(at>UINT_MAX||!read(indices.Get(),unsigned(at),length,&raw,true))return false;index=int64_t(raw)+base;}
        const uint64_t at=offset+uint64_t(index)*stride+position.offset;float xy[2]{};
        if(index<0||at>UINT_MAX||!read(buffer.Get(),unsigned(at),8,xy)||!std::isfinite(xy[0])||!std::isfinite(xy[1])||std::abs(xy[0])>16||std::abs(xy[1])>16)return false;
        if(transform) {if(!transform->Apply(xy,vertices[i]))return false;}
        else vertices[i]={xy[0],xy[1]};
        if(!std::isfinite(vertices[i].x)||!std::isfinite(vertices[i].y))return false;
    }
    }
    D3D11_PRIMITIVE_TOPOLOGY topology{};context->IAGetPrimitiveTopology(&topology);
    if((topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST&&count!=3&&count!=6)||(topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP&&count>4))return false;
    if(topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST&&topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)return false;
    D3D11_RASTERIZER_DESC raster{};raster.CullMode=D3D11_CULL_BACK;Microsoft::WRL::ComPtr<ID3D11RasterizerState> state;context->RSGetState(&state);if(state)state->GetDesc(&raster);
    std::vector<std::vector<Point>> polygons;const std::vector<Point> square{{-1,-1},{1,-1},{1,1},{-1,1}};
    for(unsigned triangle=0;triangle<(count==3?1u:2u);++triangle){unsigned a=triangle*3,b=a+1,c=a+2;if(topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP){a=triangle;b=triangle+1;c=triangle+2;if(triangle&1)std::swap(a,b);}
        auto va=vertices[a],vb=vertices[b],vc=vertices[c];const auto cross=Cross(va,vb,vc);if(std::abs(cross)<1e-8)continue;
        const bool front=(cross>0)==bool(raster.FrontCounterClockwise);if((raster.CullMode==D3D11_CULL_BACK&&!front)||(raster.CullMode==D3D11_CULL_FRONT&&front))continue;
        if(cross<0)std::swap(vb,vc);polygons.push_back(Clip({va,vb,vc},square));}
    double covered=0;for(const auto& polygon:polygons)covered+=Area(polygon);
    if(polygons.size()==2&&!polygons[0].empty()&&!polygons[1].empty())covered-=Area(Clip(polygons[0],polygons[1]));
    return covered>=4-1e-6;
}
} // namespace taa::fullscreen
