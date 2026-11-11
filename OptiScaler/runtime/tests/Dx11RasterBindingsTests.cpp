// SPDX-License-Identifier: GPL-3.0-or-later
// Real WARP public-state validation; no game, NGX, NR or FG execution.
#include "../Dx11RasterBindings.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>

using Microsoft::WRL::ComPtr;
namespace {
unsigned checks = 0;
void Check(bool good, const char* label) {
    ++checks;
    if (!good) { std::fprintf(stderr, "FAILED: %s\n", label); std::abort(); }
}
// A fixture for the production comparator's binding-only interface. The real
// application supplies its already-retained Raster, including separate proof
// and budget owners. No capture implementation is duplicated in this suite.
struct Snapshot {
    ComPtr<ID3D11DepthStencilView> view;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;
    ComPtr<ID3D11InputLayout> layout;
    std::array<ComPtr<ID3D11Buffer>,32> vertices{};
    std::array<UINT,32> strides{}, offsets{};
    ComPtr<ID3D11Buffer> indices;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
    UINT indexOffset = 0, reference = 0;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    struct Binding { ComPtr<ID3D11Buffer> buffer; UINT first=0, count=4096; };
    std::array<Binding,56> constants{};
    std::array<D3D11_VIEWPORT,16> viewports{};
    std::array<D3D11_RECT,16> scissors{};
    UINT viewportCount=0, scissorCount=0;
};
ComPtr<ID3DBlob> Compile(const char* code, const char* profile) {
    ComPtr<ID3DBlob> blob, error;
    const HRESULT hr=D3DCompile(code,std::strlen(code),nullptr,nullptr,nullptr,"main",profile,0,0,&blob,&error);
    if(FAILED(hr) && error)std::fprintf(stderr,"%s\n",static_cast<const char*>(error->GetBufferPointer()));
    Check(SUCCEEDED(hr),"compile real test shader"); return blob;
}
}
int main() {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
        &device,nullptr,&context)),"create WARP");
    ComPtr<ID3D11DeviceContext1> context1;
    Check(SUCCEEDED(context.As(&context1)),"WARP supports real CB range getters");
    Snapshot expected;
    const auto matches=[&]{
        const bool queried=FramePipeline::MatchesDx11RasterBindings(context.Get(),expected);
        const bool retained=FramePipeline::MatchesDx11RasterBindings(context.Get(),expected,context1.Get());
        Check(queried==retained,"retained same-context interface preserves public binding validation");
        return retained;
    };
    Check(!FramePipeline::MatchesDx11RasterBindings(nullptr,expected),"null context refuses");
    Check(matches(),"unchanged default public bindings match");
    expected.viewportCount=17; Check(!matches(),"oversized expected viewport refuses"); expected.viewportCount=0;
    expected.scissorCount=17; Check(!matches(),"oversized expected scissor refuses"); expected.scissorCount=0;

    D3D11_BUFFER_DESC bd{};bd.ByteWidth=512;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    ComPtr<ID3D11Buffer> vertex,otherVertex,index,otherIndex,cb,otherCb;
    Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&vertex)),"create VB");
    Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&otherVertex)),"create alternate VB");
    bd.BindFlags=D3D11_BIND_INDEX_BUFFER;
    Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&index)),"create IB");
    Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&otherIndex)),"create alternate IB");
    bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&cb)),"create CB");
    Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&otherCb)),"create alternate CB");
    auto* vb=vertex.Get();UINT stride=12,offset=0;
    context->IASetVertexBuffers(31,1,&vb,&stride,&offset);
    Check(!matches(),"last VB slot mutation refuses");
    expected.vertices[31]=vertex;expected.strides[31]=stride;
    Check(matches(),"retained VB identity matches");
    offset=4;context->IASetVertexBuffers(31,1,&vb,&stride,&offset);Check(!matches(),"VB offset mutation refuses");
    offset=0;stride=16;context->IASetVertexBuffers(31,1,&vb,&stride,&offset);Check(!matches(),"VB stride mutation refuses");
    stride=12;vb=otherVertex.Get();context->IASetVertexBuffers(31,1,&vb,&stride,&offset);Check(!matches(),"same-layout other VB refuses");
    vb=vertex.Get();context->IASetVertexBuffers(31,1,&vb,&stride,&offset);
    expected.indices=index;expected.indexFormat=DXGI_FORMAT_R16_UINT;
    context->IASetIndexBuffer(index.Get(),DXGI_FORMAT_R16_UINT,0);Check(matches(),"retained IB matches");
    context->IASetIndexBuffer(otherIndex.Get(),DXGI_FORMAT_R16_UINT,0);Check(!matches(),"other IB refuses");
    context->IASetIndexBuffer(index.Get(),DXGI_FORMAT_R32_UINT,0);Check(!matches(),"IB format mutation refuses");
    context->IASetIndexBuffer(index.Get(),DXGI_FORMAT_R16_UINT,4);Check(!matches(),"IB offset mutation refuses");
    context->IASetIndexBuffer(index.Get(),DXGI_FORMAT_R16_UINT,0);

    expected.topology=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    context->IASetPrimitiveTopology(expected.topology);Check(matches(),"topology matches");
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);Check(!matches(),"topology mutation refuses");
    context->IASetPrimitiveTopology(expected.topology);
    expected.viewportCount=1;expected.viewports[0]={0,0,64,64,0,1};
    context->RSSetViewports(1,expected.viewports.data());Check(matches(),"viewport matches");
    auto viewport=expected.viewports[0];viewport.MinDepth=0.1f;
    context->RSSetViewports(1,&viewport);Check(!matches(),"viewport depth mutation refuses");
    context->RSSetViewports(0,nullptr);Check(!matches(),"viewport count mutation refuses");
    context->RSSetViewports(1,expected.viewports.data());
    expected.scissorCount=1;expected.scissors[0]={0,0,64,64};
    context->RSSetScissorRects(1,expected.scissors.data());Check(matches(),"scissors match");
    auto rect=expected.scissors[0];rect.right=63;context->RSSetScissorRects(1,&rect);Check(!matches(),"scissor mutation refuses");
    context->RSSetScissorRects(0,nullptr);Check(!matches(),"scissor count mutation refuses");
    context->RSSetScissorRects(1,expected.scissors.data());

    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=64;td.MipLevels=td.ArraySize=1;
    td.Format=DXGI_FORMAT_D32_FLOAT;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11DepthStencilView> otherView;
    Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&texture)),"create depth texture");
    Check(SUCCEEDED(device->CreateDepthStencilView(texture.Get(),nullptr,&expected.view)),"create retained DSV");
    Check(SUCCEEDED(device->CreateDepthStencilView(texture.Get(),nullptr,&otherView)),"create different DSV of same plane");
    context->OMSetRenderTargets(0,nullptr,expected.view.Get());Check(matches(),"DSV matches");
    context->OMSetRenderTargets(0,nullptr,otherView.Get());Check(!matches(),"different DSV of same plane refuses");
    context->OMSetRenderTargets(0,nullptr,nullptr);Check(!matches(),"removed DSV refuses");
    context->OMSetRenderTargets(0,nullptr,expected.view.Get());
    D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D11_COMPARISON_LESS;
    Check(SUCCEEDED(device->CreateDepthStencilState(&dd,&expected.depth)),"create depth state");
    context->OMSetDepthStencilState(expected.depth.Get(),0);Check(matches(),"depth state matches");
    context->OMSetDepthStencilState(expected.depth.Get(),1);Check(!matches(),"stencil reference mutation refuses");
    context->OMSetDepthStencilState(nullptr,0);Check(!matches(),"depth state mutation refuses");
    context->OMSetDepthStencilState(expected.depth.Get(),0);
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    Check(SUCCEEDED(device->CreateRasterizerState(&rd,&expected.raster)),"create raster state");
    context->RSSetState(expected.raster.Get());Check(matches(),"raster state matches");
    context->RSSetState(nullptr);Check(!matches(),"raster state mutation refuses");context->RSSetState(expected.raster.Get());

    const auto vertexBlob=Compile("float4 main(float3 p:POSITION):SV_POSITION{return float4(p,1);}","vs_5_0");
    Check(SUCCEEDED(device->CreateVertexShader(vertexBlob->GetBufferPointer(),vertexBlob->GetBufferSize(),nullptr,&expected.vs)),"create VS");
    context->VSSetShader(expected.vs.Get(),nullptr,0);Check(matches(),"VS matches");
    context->VSSetShader(nullptr,nullptr,0);Check(!matches(),"VS mutation refuses");context->VSSetShader(expected.vs.Get(),nullptr,0);
    D3D11_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,31,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    Check(SUCCEEDED(device->CreateInputLayout(&element,1,vertexBlob->GetBufferPointer(),vertexBlob->GetBufferSize(),&expected.layout)),"create input layout");
    context->IASetInputLayout(expected.layout.Get());Check(matches(),"layout matches");
    context->IASetInputLayout(nullptr);Check(!matches(),"layout mutation refuses");context->IASetInputLayout(expected.layout.Get());
    const auto pixelBlob=Compile("float4 main():SV_TARGET{return 1;}","ps_5_0");
    Check(SUCCEEDED(device->CreatePixelShader(pixelBlob->GetBufferPointer(),pixelBlob->GetBufferSize(),nullptr,&expected.ps)),"create PS");
    context->PSSetShader(expected.ps.Get(),nullptr,0);Check(matches(),"PS matches");
    context->PSSetShader(nullptr,nullptr,0);Check(!matches(),"PS mutation refuses");context->PSSetShader(expected.ps.Get(),nullptr,0);
    const auto geometryBlob=Compile("struct V{float4 p:SV_POSITION;};[maxvertexcount(3)] void main(triangle V v[3],inout TriangleStream<V> s){s.Append(v[0]);s.Append(v[1]);s.Append(v[2]);}","gs_5_0");
    ComPtr<ID3D11GeometryShader> geometry;
    Check(SUCCEEDED(device->CreateGeometryShader(geometryBlob->GetBufferPointer(),geometryBlob->GetBufferSize(),nullptr,&geometry)),"create GS");
    context->GSSetShader(geometry.Get(),nullptr,0);Check(!matches(),"unsupported GS refuses");context->GSSetShader(nullptr,nullptr,0);
    ComPtr<ID3D11Predicate> predicate;D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION_PREDICATE,0};
    Check(SUCCEEDED(device->CreatePredicate(&qd,&predicate)),"create predicate");
    context->SetPredication(predicate.Get(),FALSE);Check(!matches(),"predicated state refuses");context->SetPredication(nullptr,FALSE);

    auto* cbRaw=cb.Get();UINT first=0,count=16;
    context1->VSSetConstantBuffers1(13,1,&cbRaw,&first,&count);
    Check(!matches(),"last VS CB slot mutation refuses");expected.constants[13]={cb,0,16};Check(matches(),"VS CB range matches");
    first=16;context1->VSSetConstantBuffers1(13,1,&cbRaw,&first,&count);Check(!matches(),"CB first constant mutation refuses");
    first=0;count=32;context1->VSSetConstantBuffers1(13,1,&cbRaw,&first,&count);Check(!matches(),"CB range size mutation refuses");
    count=16;context1->VSSetConstantBuffers1(13,1,&cbRaw,&first,&count);
    cbRaw=otherCb.Get();context1->VSSetConstantBuffers1(13,1,&cbRaw,&first,&count);Check(!matches(),"same-size other CB refuses");
    cbRaw=cb.Get();context1->VSSetConstantBuffers1(13,1,&cbRaw,&first,&count);
    context1->PSSetConstantBuffers1(13,1,&cbRaw,&first,&count);Check(!matches(),"last PS CB slot mutation refuses");
    expected.constants[27]={cb,0,16};Check(matches(),"PS CB range matches");

    const auto hullBlob=Compile("struct V{float4 p:SV_POSITION;};struct F{float e[3]:SV_TessFactor;float i:SV_InsideTessFactor;};F patch(InputPatch<V,3> p,uint id:SV_PrimitiveID){F f;f.e[0]=f.e[1]=f.e[2]=f.i=1;return f;}[domain(\"tri\")][partitioning(\"integer\")][outputtopology(\"triangle_cw\")][outputcontrolpoints(3)][patchconstantfunc(\"patch\")]V main(InputPatch<V,3> p,uint i:SV_OutputControlPointID){return p[i];}","hs_5_0");
    const auto domainBlob=Compile("struct V{float4 p:SV_POSITION;};struct F{float e[3]:SV_TessFactor;float i:SV_InsideTessFactor;};[domain(\"tri\")]float4 main(F f,float3 uv:SV_DomainLocation,const OutputPatch<V,3> p):SV_POSITION{return uv.x*p[0].p+uv.y*p[1].p+uv.z*p[2].p;}","ds_5_0");
    Check(SUCCEEDED(device->CreateHullShader(hullBlob->GetBufferPointer(),hullBlob->GetBufferSize(),nullptr,&expected.hs)),"create HS");
    Check(SUCCEEDED(device->CreateDomainShader(domainBlob->GetBufferPointer(),domainBlob->GetBufferSize(),nullptr,&expected.ds)),"create DS");
    context->HSSetShader(expected.hs.Get(),nullptr,0);context->DSSetShader(expected.ds.Get(),nullptr,0);
    expected.topology=D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;context->IASetPrimitiveTopology(expected.topology);
    Check(matches(),"paired tessellation bindings match");
    context->HSSetShader(nullptr,nullptr,0);Check(!matches(),"HS mutation refuses");context->HSSetShader(expected.hs.Get(),nullptr,0);
    context->DSSetShader(nullptr,nullptr,0);Check(!matches(),"DS mutation refuses");context->DSSetShader(expected.ds.Get(),nullptr,0);
    context1->HSSetConstantBuffers1(13,1,&cbRaw,&first,&count);Check(!matches(),"last HS CB slot mutation refuses");
    expected.constants[41]={cb,0,16};Check(matches(),"HS CB range matches");
    context1->DSSetConstantBuffers1(13,1,&cbRaw,&first,&count);Check(!matches(),"last DS CB slot mutation refuses");
    expected.constants[55]={cb,0,16};Check(matches(),"DS CB range matches");
    first=16;context1->DSSetConstantBuffers1(13,1,&cbRaw,&first,&count);Check(!matches(),"DS range mutation refuses");
    first=0;context1->DSSetConstantBuffers1(13,1,&cbRaw,&first,&count);
    Check(matches(),"all original bindings restored");
    context->ClearState();Check(!matches(),"opaque ClearState refuses");
    struct Reference {unsigned releases=0;void Release(){++releases;}} firstReference,lastReference;
    const auto mismatch=[&]{
        FramePipeline::detail::Dx11GetterReferences<Reference,4> references;
        references.data()[0]=&firstReference;references.data()[1]=&firstReference;
        references.data()[3]=&lastReference;
        return references[0]!=&firstReference;
    };
    Check(!mismatch() && firstReference.releases==2 && lastReference.releases==1,
          "early mismatch releases every returned slot exactly once, including repeated interfaces and the last slot");
    std::printf("Dx11RasterBindings: %u real WARP checks passed; bindings only, no content/GPU/NR/FG proof\n",checks);
}
