// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TaaPassDx11.h"
#include "TaaFullscreenDx11.h"
#include "TaaShaderAnalysis.h"
#include "TaaShaderBindings.h"
#include <resource_tracking/DrawObservation_Dx11.h>
#include <d3d11shader.h>
#include <vector>

namespace taa::consumer {
// Versioned byte identities describe the actual postprocess operations; resource
// identity and coverage are proved again for every physical command. No engine
// name, equal texture size or shader identity alone authorizes a replacement.
struct Program {
    bool supported=false;
    std::vector<unsigned> constants,textures;
    bool positionFromVertex=false;
    uint64_t hash=0;
    fullscreen::PositionTransform position;
    std::string refusal;
};
enum class ProgramStage { Pixel, Vertex };
struct PixelContract {
    uint64_t hash;size_t bytes;const char* name;
    unsigned constantBytes,textures,samplers; // CB0 and contiguous t0/s0 declarations
};
inline constexpr PixelContract PixelPrograms[]{
    {0x6CA04A04AFEE2570ull,2972,"scene-tonemap-v1",6960,5,2},
    {0x88CBD14623C28B8Eull,580,"screen-copy-v1",0,1,1},
    {0xF63F3FF630FEBDB4ull,588,"screen-copy-v2",0,1,1},
    {0xF3EF7F661CCF4AF8ull,292,"screen-copy-v3",0,1,1},
    {0x12F210414D64E9FAull,3092,"alpha-scene-tonemap-v1",6960,6,2},
    {0xC6C24FFD1D81EA68ull,4256,"lens-scene-tonemap-v1",7424,6,3},
    {0xF53695D73045BEB1ull,3552,"chromatic-scene-tonemap-v1",6960,5,2},
    {0x46D956B3CEB5978Cull,3548,"radial-scene-tonemap-v1",6960,6,2},
};
inline const PixelContract* FindPixelProgram(uint64_t hash,size_t bytes) {
    for(const auto& program:PixelPrograms)if(program.hash==hash&&program.bytes==bytes)return &program;
    return nullptr;
}
// Replay keeps the physical shader and UVs. Position proof retains each float
// operation and checks it against the actual command's vertex values.
inline std::optional<fullscreen::PositionTransform> PositionProgram(ID3D11ShaderReflection* reflection,
    const D3D11_SHADER_DESC& desc,std::string_view assembly,std::string* refusal=nullptr) {
    const auto fail=[&](const char* reason)->std::optional<fullscreen::PositionTransform>{if(refusal)*refusal=reason;return {};};
    if(D3D11_SHVER_GET_TYPE(desc.Version)!=D3D11_SHVER_VERTEX_SHADER) {
        if(refusal)*refusal="vertex stage mismatch version="+std::to_string(desc.Version);return {};
    }
    unsigned input=~0u,output=~0u;
    for(unsigned i=0;i<desc.InputParameters;++i) {
        D3D11_SIGNATURE_PARAMETER_DESC p{};if(FAILED(reflection->GetInputParameterDesc(i,&p)))return {};
        if(p.SemanticName && !_stricmp(p.SemanticName,"POSITION") && !p.SemanticIndex &&
            p.SystemValueType==D3D_NAME_UNDEFINED && p.ComponentType==D3D_REGISTER_COMPONENT_FLOAT32 && (p.Mask&3)==3) {
            if(input!=~0u)return {};input=p.Register;
        }
    }
    for(unsigned i=0;i<desc.OutputParameters;++i) {
        D3D11_SIGNATURE_PARAMETER_DESC p{};if(FAILED(reflection->GetOutputParameterDesc(i,&p)))return {};
        if(p.SystemValueType==D3D_NAME_POSITION) {if(output!=~0u || p.Mask!=15)return {};output=p.Register;}
        else if(p.SystemValueType!=D3D_NAME_UNDEFINED)return {};
    }
    if(input==~0u || output>=8)return fail("position signature unavailable");
    using IR=analysis::Coordinates;using Op=IR::Op;
    IR ir(assembly,IR::Arithmetic::PreserveOperations);
    const auto found=ir.outputs.find(output);
    if(ir.unsupportedControl || found==ir.outputs.end() || ir.mustWrite[output]!=15 || !ir.exitCount)return fail("position control flow or writes unproven");
    const auto literal=[&](unsigned id)->std::optional<float> {
        if(!id||id>=ir.nodes.size()||ir.nodes[id].op!=Op::Literal)return {};
        const float v=std::bit_cast<float>(ir.nodes[id].value);
        return std::isfinite(v)?std::optional<float>(v):std::nullopt;
    };
    const auto z=literal(found->second[2]),w=literal(found->second[3]);
    if((z&&(*z<0||*z>1))||(w&&*w!=1))return fail("literal clip depth or homogeneous w invalid");
    fullscreen::PositionTransform program;
    std::map<unsigned,unsigned> emitted;
    const auto emit=[&](auto&& self,unsigned id,unsigned depth)->std::optional<unsigned> {
        if(!id||id>=ir.nodes.size()||depth>32||program.steps.size()>=64)return {};
        if(const auto it=emitted.find(id);it!=emitted.end())return it->second;
        const auto& n=ir.nodes[id];fullscreen::PositionTransform::Step step;
        if(n.op==Op::Literal) {const auto value=literal(id);if(!value)return {};step.literal=*value;}
        else if(n.op==Op::Input) {
            if(n.slot!=input||n.component>=2)return {};
            step.op=fullscreen::PositionTransform::Op::Input;step.lane=n.component;step.varying=true;
        } else if(n.op==Op::Constant) {
            if(n.slot>=14||n.value>65532||n.value%4)return {};
            step.op=fullscreen::PositionTransform::Op::Constant;step.slot=n.slot;step.offset=n.value;
        } else if(n.op==Op::Add||n.op==Op::Mul) {
            // A matrix remains an ordered affine program. Resolve each current
            // CB lane before actual geometry proof; never algebraically erase it.
            const auto a=self(self,n.a,depth+1),b=self(self,n.b,depth+1);
            if(!a||!b)return {};step.a=*a;step.b=*b;
            if(n.op==Op::Mul&&program.steps[*a].varying&&program.steps[*b].varying)return {};
            step.varying=program.steps[*a].varying||program.steps[*b].varying;
            step.op=n.op==Op::Add?fullscreen::PositionTransform::Op::Add:fullscreen::PositionTransform::Op::Mul;
        } else return {};
        const auto index=unsigned(program.steps.size());program.steps.push_back(step);emitted.emplace(id,index);return index;
    };
    for(unsigned c=0;c<4;++c) {const auto id=emit(emit,found->second[c],0);if(!id)return fail("position uses unsupported operands or arithmetic");program.outputs[c]=*id;}
    return program;
}

inline Program Describe(const void* bytes,size_t size,ProgramStage stage) {
    Program result;if(!bytes||size<32)return result;
    const auto hash=DrawObservationDx11::ShaderByteHash(bytes,size);result.hash=hash;
    const bool pixel=stage==ProgramStage::Pixel;
    const auto* contract=pixel?FindPixelProgram(hash,size):nullptr;
    if(pixel && !contract){result.refusal="unregistered postprocess byte identity";return result;}
    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
    if(FAILED(D3DReflect(bytes,size,IID_PPV_ARGS(&reflection))))return result;
    D3D11_SHADER_DESC desc{};if(FAILED(reflection->GetDesc(&desc)))return result;
    const unsigned expected=pixel?D3D11_SHVER_PIXEL_SHADER:D3D11_SHVER_VERTEX_SHADER;
    if(D3D11_SHVER_GET_TYPE(desc.Version)!=expected){result.refusal="shader stage mismatch";return result;}
    ComPtr<ID3DBlob> disassembly;
    if(FAILED(D3DDisassemble(bytes,size,D3D_DISASM_PRINT_HEX_LITERALS,nullptr,&disassembly)))return result;
    const std::string_view assembly(static_cast<const char*>(disassembly->GetBufferPointer()),disassembly->GetBufferSize());
    const auto declared=bindings::Read(assembly);
    if(!declared.valid){result.refusal=declared.refusal;return result;}
    if(contract) {
        const auto consecutive=[](const auto& slots,unsigned count) {
            if(slots.size()!=count)return false;
            for(unsigned i=0;i<count;++i)if(slots[i]!=i)return false;
            return true;
        };
        if(!consecutive(declared.constants,contract->constantBytes?1u:0u)||
            declared.constantBytes[0]!=contract->constantBytes||
            !consecutive(declared.textures,contract->textures)||!consecutive(declared.samplers,contract->samplers)) {
            result.refusal="postprocess executable declarations disagree with registered contract";return result;
        }
    }
    result.constants=declared.constants;result.textures=declared.textures;
    const auto contains=[](const auto& slots,unsigned slot){return std::find(slots.begin(),slots.end(),slot)!=slots.end();};
    // RDEF is optional. When present it must agree with executable declarations;
    // its absence never turns a shader into a program with no resources.
    if(desc.BoundResources&&desc.BoundResources!=declared.constants.size()+declared.textures.size()+declared.samplers.size())return result;
    for(unsigned i=0;i<desc.BoundResources;++i) {
        D3D11_SHADER_INPUT_BIND_DESC binding{};if(FAILED(reflection->GetResourceBindingDesc(i,&binding)))return result;
        if(binding.BindCount!=1)return result;
        if(binding.Type==D3D_SIT_CBUFFER) {if(!contains(declared.constants,binding.BindPoint))return result;}
        else if(binding.Type==D3D_SIT_TEXTURE&&binding.Dimension==D3D_SRV_DIMENSION_TEXTURE2D) {if(!contains(declared.textures,binding.BindPoint))return result;}
        else if(binding.Type==D3D_SIT_SAMPLER) {if(!contains(declared.samplers,binding.BindPoint))return result;}
        else return result;
    }
    if(pixel)for(unsigned i=0;i<desc.OutputParameters;++i) {
        D3D11_SIGNATURE_PARAMETER_DESC output{};
        if(FAILED(reflection->GetOutputParameterDesc(i,&output))||output.SystemValueType!=D3D_NAME_TARGET||output.SemanticIndex!=0)return result;
    }
    if(!pixel) {
        if(!result.textures.empty())return result;
        // Captured procedural fullscreen v1: bfi/and on SV_VertexID, then
        // clip=(-1,-1),(3,-1),(-1,3), z=w=1 for IDs 0..2. The physical
        // shader (including its UVs) is replayed; this contract supplies only
        // positions for the shared command/culling/coverage proof.
        if(hash==0x98109168E808BC85ull&&size==496) {
            if(!declared.constants.empty()||!declared.samplers.empty())return result;
            result.position.source=fullscreen::PositionTransform::Source::VertexIdTriangle;
        } else {
            const auto position=PositionProgram(reflection.Get(),desc,assembly,&result.refusal);
            if(!position)return result;
            for(const auto& step:position->steps)if(step.op==fullscreen::PositionTransform::Op::Constant&&
                (step.slot>=declared.constantBytes.size()||step.offset+sizeof(float)>declared.constantBytes[step.slot]))return result;
            result.position=*position;
        }
    }
    result.supported=true;result.positionFromVertex=!pixel;return result;
}
struct Command {
    bool indexed=false;
    unsigned count=0,first=0,instances=1,firstInstance=0;
    int base=0;
};
struct Surface {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
};
inline DXGI_FORMAT Storage(DXGI_FORMAT format) {
    if(format==DXGI_FORMAT_R8G8B8A8_UNORM||format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    if(format==DXGI_FORMAT_B8G8R8A8_UNORM||format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    return format;
}
inline bool MakeSurface(ID3D11Device* device,unsigned width,unsigned height,DXGI_FORMAT format,Surface& out) {
    D3D11_TEXTURE2D_DESC old{};if(out.texture)out.texture->GetDesc(&old);
    D3D11_RENDER_TARGET_VIEW_DESC existing{};if(out.rtv)out.rtv->GetDesc(&existing);
    if(old.Width==width&&old.Height==height&&old.Format==Storage(format)&&existing.Format==format&&out.rtv&&out.srv)return true;
    out={};D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=Storage(format);d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    D3D11_RENDER_TARGET_VIEW_DESC r{};r.Format=format;r.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC s{};s.Format=format;s.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;s.Texture2D.MipLevels=1;
    return SUCCEEDED(device->CreateTexture2D(&d,nullptr,&out.texture))&&
        SUCCEEDED(device->CreateRenderTargetView(out.texture.Get(),&r,&out.rtv))&&SUCCEEDED(device->CreateShaderResourceView(out.texture.Get(),&s,&out.srv));
}
class Chain final {
public:
    struct NodeIdentity {unsigned ordinal=0,width=0,height=0;uint64_t pixel=0,vertex=0;uintptr_t input=0,output=0;int parent=-2;};
    struct Diagnostics {unsigned draws=0,linked=0,disconnected=0,invalidated=0;uint64_t pixel=0,vertex=0;uintptr_t input=0;NodeIdentity current,refused;std::string reason;};
private:
    Diagnostics diagnostics_;
    bool observing_=false;
    struct Edge {ComPtr<ID3D11Texture2D> resource;int node=-1;bool valid=true;};
    struct Constants {std::array<ComPtr<ID3D11Buffer>,14> buffers;std::array<UINT,14> first{},count{};};
    struct Node {
        int parent=-1;
        Command command;
        ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11InputLayout> layout;
        D3D11_PRIMITIVE_TOPOLOGY topology{};
        std::array<ComPtr<ID3D11Buffer>,32> vertices;
        std::array<UINT,32> strides{},offsets{};
        ComPtr<ID3D11Buffer> indices;DXGI_FORMAT indexFormat{};UINT indexOffset=0;
        std::array<Constants,2> constants;
        std::array<ComPtr<ID3D11Texture2D>,128> textures;
        std::array<ComPtr<ID3D11ShaderResourceView>,128> views;
        std::array<ComPtr<ID3D11SamplerState>,16> samplers;
        DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
        D3D11_SHADER_RESOURCE_VIEW_DESC inputView{};
        ComPtr<ID3D11RasterizerState> raster;
        Surface output;
    };
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11DeviceContext1> context1_;
    ComPtr<ID3DDeviceContextState> work_;
    std::vector<Edge> edges_;
    std::vector<std::unique_ptr<Node>> nodes_;
    unsigned used_=0;
    ComPtr<ID3D11Texture2D> candidateOutput_;
    ComPtr<ID3D11ShaderResourceView> original_,replacement_;
    ComPtr<ID3D11Texture2D> inputStorage_;
    int candidateParent_=-1,candidateNode_=-1;
    unsigned width_=0,height_=0;
    DXGI_FORMAT format_=DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11DepthStencilState> noDepth_;
    bool bound_=false;
    fullscreen::Evidence geometry_;
    std::string error_;
    bool Fail(std::string_view reason){error_=reason;diagnostics_.reason=error_;diagnostics_.refused=diagnostics_.current;return false;}
    bool SnapshotBuffer(ID3D11Buffer* source,ComPtr<ID3D11Buffer>& destination,UINT bind) {
        if(!source){destination.Reset();return true;}
        D3D11_BUFFER_DESC d{},old{};source->GetDesc(&d);if(destination)destination->GetDesc(&old);
        if(!destination||old.ByteWidth!=d.ByteWidth||old.BindFlags!=bind) {
            destination.Reset();d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=d.MiscFlags=d.StructureByteStride=0;d.BindFlags=bind;
            if(FAILED(device_->CreateBuffer(&d,nullptr,&destination)))return false;
        }
        context_->CopyResource(destination.Get(),source);return true;
    }
    bool SnapshotConstants(Node& node,const Program& program,bool pixel) {
        auto& out=node.constants[pixel?1:0];
        for(unsigned slot=0;slot<14;++slot) {
            if(std::find(program.constants.begin(),program.constants.end(),slot)==program.constants.end()){out.buffers[slot].Reset();continue;}
            ComPtr<ID3D11Buffer> source;
            if(pixel)context1_->PSGetConstantBuffers1(slot,1,&source,&out.first[slot],&out.count[slot]);
            else context1_->VSGetConstantBuffers1(slot,1,&source,&out.first[slot],&out.count[slot]);
            if(!source||!SnapshotBuffer(source.Get(),out.buffers[slot],D3D11_BIND_CONSTANT_BUFFER))return false;
        }
        return true;
    }
    bool SnapshotNode(Node& node,const Command& command,const Program& pixel,const Program& vertex) {
        node.command=command;node.parent=candidateParent_;node.format=format_;
        original_->GetDesc(&node.inputView);
        ComPtr<ID3D11RasterizerState> originalRaster;context_->RSGetState(&originalRaster);
        D3D11_RASTERIZER_DESC raster{};
        if(originalRaster)originalRaster->GetDesc(&raster);
        else {raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_BACK;raster.DepthClipEnable=TRUE;}
        raster.ScissorEnable=FALSE;node.raster.Reset();if(FAILED(device_->CreateRasterizerState(&raster,&node.raster)))return false;
        UINT classes=0;context_->VSGetShader(&node.vs,nullptr,&classes);if(classes)return false;
        context_->PSGetShader(&node.ps,nullptr,&classes);if(classes)return false;
        context_->IAGetInputLayout(&node.layout);context_->IAGetPrimitiveTopology(&node.topology);
        std::array<ID3D11Buffer*,32> buffers{};context_->IAGetVertexBuffers(0,32,buffers.data(),node.strides.data(),node.offsets.data());
        for(unsigned i=0;i<32;++i){ComPtr<ID3D11Buffer> held;held.Attach(buffers[i]);if(!SnapshotBuffer(held.Get(),node.vertices[i],D3D11_BIND_VERTEX_BUFFER))return false;}
        ComPtr<ID3D11Buffer> index;context_->IAGetIndexBuffer(&index,&node.indexFormat,&node.indexOffset);
        if(!SnapshotBuffer(index.Get(),node.indices,D3D11_BIND_INDEX_BUFFER)||!SnapshotConstants(node,pixel,true)||!SnapshotConstants(node,vertex,false))return false;
        std::array<ID3D11SamplerState*,16> samplers{};context_->PSGetSamplers(0,16,samplers.data());
        for(unsigned i=0;i<16;++i)node.samplers[i].Attach(samplers[i]);
        for(unsigned slot=0;slot<128;++slot) {
            if(slot==0||std::find(pixel.textures.begin(),pixel.textures.end(),slot)==pixel.textures.end()){node.views[slot].Reset();continue;}
            ComPtr<ID3D11ShaderResourceView> view;context_->PSGetShaderResources(slot,1,&view);if(!view){node.views[slot].Reset();continue;}
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};view->GetDesc(&v);ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
            ComPtr<ID3D11Texture2D> source;if(FAILED(resource.As(&source)))return false;
            D3D11_TEXTURE2D_DESC d{},old{};source->GetDesc(&d);if(node.textures[slot])node.textures[slot]->GetDesc(&old);
            d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=d.MiscFlags=0;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            if(!node.textures[slot]||std::memcmp(&old,&d,sizeof(d))) {
                node.views[slot].Reset();node.textures[slot].Reset();
                if(FAILED(device_->CreateTexture2D(&d,nullptr,&node.textures[slot])))return false;
            }
            context_->CopyResource(node.textures[slot].Get(),source.Get());node.views[slot].Reset();
            if(FAILED(device_->CreateShaderResourceView(node.textures[slot].Get(),&v,&node.views[slot])))return false;
        }
        return true;
    }
    bool Coverage(const Command& command,ID3D11RenderTargetView* output,ID3D11DepthStencilView* depth) {
        if(command.instances!=1||command.firstInstance||(command.count!=3&&command.count!=4&&command.count!=6))return Fail("unsupported compositor command");
        D3D11_RENDER_TARGET_VIEW_DESC rt{};output->GetDesc(&rt);
        if(rt.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D||rt.Texture2D.MipSlice)return Fail("compositor output subresource");
        ComPtr<ID3D11Resource> resource;output->GetResource(&resource);if(FAILED(resource.As(&candidateOutput_)))return Fail("compositor output is not Texture2D");
        D3D11_TEXTURE2D_DESC d{};candidateOutput_->GetDesc(&d);width_=d.Width;height_=d.Height;format_=rt.Format;
        if(d.SampleDesc.Count!=1||d.ArraySize!=1||d.MipLevels!=1)return Fail("compositor output layout");
        ComPtr<ID3D11Predicate> predicate;BOOL pred=FALSE;context_->GetPredication(&predicate,&pred);if(predicate)return Fail("predicated compositor");
        std::array<D3D11_VIEWPORT,16> vp{};UINT count=16;context_->RSGetViewports(&count,vp.data());
        if(count!=1||vp[0].TopLeftX||vp[0].TopLeftY||vp[0].Width!=float(width_)||vp[0].Height!=float(height_))return Fail("partial compositor viewport");
        ComPtr<ID3D11RasterizerState> raster;context_->RSGetState(&raster);if(raster){D3D11_RASTERIZER_DESC r{};raster->GetDesc(&r);
            if(r.FillMode!=D3D11_FILL_SOLID)return Fail("wireframe compositor");
            if(r.ScissorEnable){std::array<D3D11_RECT,16> rectangles{};count=16;context_->RSGetScissorRects(&count,rectangles.data());
                if(count!=1||rectangles[0].left>0||rectangles[0].top>0||rectangles[0].right<LONG(width_)||rectangles[0].bottom<LONG(height_))return Fail("partial compositor scissor");}}
        if(const auto* reason=DepthStencilDx11::Capture(context_.Get(),depth).Reason())return Fail(reason);
        ComPtr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT mask=0;context_->OMGetBlendState(&blend,factors,&mask);if(!(mask&1))return Fail("masked compositor samples");
        if(blend){D3D11_BLEND_DESC b{};blend->GetDesc(&b);if(b.RenderTarget[0].BlendEnable||b.RenderTarget[0].RenderTargetWriteMask!=15)return Fail("blended or partial compositor output");}
        ComPtr<ID3D11VertexShader> currentVertex;ComPtr<ID3D11PixelShader> currentPixel;
        UINT vertexClasses=0,pixelClasses=0;
        context_->VSGetShader(&currentVertex,nullptr,&vertexClasses);context_->PSGetShader(&currentPixel,nullptr,&pixelClasses);
        if(!currentVertex||!currentPixel||vertexClasses||pixelClasses)return Fail("compositor class linkage or missing shader");
        ComPtr<ID3D11GeometryShader> gs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;
        context_->GSGetShader(&gs,nullptr,nullptr);context_->HSGetShader(&hs,nullptr,nullptr);context_->DSGetShader(&ds,nullptr,nullptr);if(gs||hs||ds)return Fail("compositor geometry stages");
        std::array<ID3D11Buffer*,4> so{};context_->SOGetTargets(4,so.data());bool stream=false;for(auto* buffer:so)if(buffer){stream=true;buffer->Release();}if(stream)return Fail("compositor stream output");
        std::array<ID3D11UnorderedAccessView*,8> uavs{};context_->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uavs.data());
        bool writable=false;for(auto* view:uavs)if(view){writable=true;view->Release();}if(writable)return Fail("compositor UAV side effects");
        D3D11_PRIMITIVE_TOPOLOGY topology{};context_->IAGetPrimitiveTopology(&topology);
        return (topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST||topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)||Fail("compositor topology");
    }
    void Publish(ID3D11Texture2D* resource,int node){for(auto& edge:edges_)if(edge.resource.Get()==resource)edge.valid=false;edges_.push_back({resource,node,true});}
public:
    explicit Chain(ID3D11DeviceContext* context):context_(context),geometry_(context){context->GetDevice(&device_);context_.As(&context1_);}
    bool Idle()const{return geometry_.Idle();}
    const std::string& Error()const{return error_;}
    const Diagnostics& Activity()const{return diagnostics_;}
    unsigned Width()const{return width_;}unsigned Height()const{return height_;}
    bool LinkedThisDraw()const{return candidateParent_!=-2;}
    unsigned ReplayCount()const{unsigned count=0;for(int n=candidateParent_;n>=0;n=nodes_[n]->parent)++count;return count;}
    void Reset(ID3D11Texture2D* source,bool observing=false){edges_.clear();used_=0;diagnostics_={};observing_=observing;candidateOutput_.Reset();original_.Reset();replacement_.Reset();bound_=false;Publish(source,-1);}
    void Invalidate(ID3D11Resource* resource){for(auto& edge:edges_)if(edge.valid&&edge.resource.Get()==resource){edge.valid=false;++diagnostics_.invalidated;}}
    void Copy(ID3D11Resource* destination,ID3D11Resource* source,bool full) {
        int parent=-2;for(auto& edge:edges_)if(edge.valid&&edge.resource.Get()==source)parent=edge.node;
        Invalidate(destination);if(!full||parent==-2)return;
        ComPtr<ID3D11Texture2D> texture;D3D11_TEXTURE2D_DESC a{},b{};if(FAILED(destination->QueryInterface(IID_PPV_ARGS(&texture))))return;
        texture->GetDesc(&a);ComPtr<ID3D11Texture2D> from;if(FAILED(source->QueryInterface(IID_PPV_ARGS(&from))))return;from->GetDesc(&b);
        if(a.Width==b.Width&&a.Height==b.Height&&a.Format==b.Format&&a.MipLevels==1&&a.ArraySize==1)Publish(texture.Get(),parent);
    }
    // Always observe output writes, even for an unsupported command. An unknown
    // write revokes its edge. It never turns a same-size texture into provenance.
    bool Before(const Command& command,const Program& pixel,const Program& vertex,unsigned renderWidth,unsigned renderHeight) {
        error_.clear();candidateOutput_.Reset();candidateParent_=-2;candidateNode_=-1;original_.Reset();replacement_.Reset();bound_=false;
        ++diagnostics_.draws;diagnostics_.pixel=pixel.hash;diagnostics_.vertex=vertex.hash;
        diagnostics_.current={};diagnostics_.current.ordinal=diagnostics_.draws;
        diagnostics_.current.pixel=pixel.hash;diagnostics_.current.vertex=vertex.hash;
        std::array<ID3D11RenderTargetView*,8> raw{};ComPtr<ID3D11DepthStencilView> depth;context_->OMGetRenderTargets(8,raw.data(),&depth);
        std::array<ComPtr<ID3D11RenderTargetView>,8> targets;for(unsigned i=0;i<8;++i)targets[i].Attach(raw[i]);
        ComPtr<ID3D11ShaderResourceView> input;context_->PSGetShaderResources(0,1,&input);
        ComPtr<ID3D11Resource> source;if(input)input->GetResource(&source);diagnostics_.input=reinterpret_cast<uintptr_t>(source.Get());
        for(const auto& edge:edges_)if(edge.valid&&edge.resource.Get()==source.Get())candidateParent_=edge.node;
        diagnostics_.current.input=diagnostics_.input;diagnostics_.current.parent=candidateParent_;
        if(targets[0]) {ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> texture;targets[0]->GetResource(&resource);
            diagnostics_.current.output=reinterpret_cast<uintptr_t>(resource.Get());
            if(SUCCEEDED(resource.As(&texture))) {D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
                diagnostics_.current.width=desc.Width;diagnostics_.current.height=desc.Height;}}
        for(const auto& target:targets)if(target){ComPtr<ID3D11Resource> r;target->GetResource(&r);Invalidate(r.Get());}
        std::array<ID3D11UnorderedAccessView*,8> unordered{};context_->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,unordered.data());
        for(auto* view:unordered)if(view){ComPtr<ID3D11Resource> r;view->GetResource(&r);Invalidate(r.Get());view->Release();}
        if(candidateParent_==-2){if(pixel.supported||vertex.supported)++diagnostics_.disconnected;return false;}
        ++diagnostics_.linked;
        if(!pixel.supported)return Fail("linked compositor pixel program is unsupported");
        if(!vertex.supported)return Fail(vertex.refusal.empty()?"linked compositor vertex program is unsupported":
            std::string("linked compositor vertex program is unsupported: ")+vertex.refusal);
        // Replay replaces t0 only. A second reference to any live chain edge
        // would otherwise mix the new full-size result with the old picture.
        for(const auto slot:pixel.textures)if(slot!=0) {
            ComPtr<ID3D11ShaderResourceView> view;context_->PSGetShaderResources(slot,1,&view);
            ComPtr<ID3D11Resource> resource;if(view)view->GetResource(&resource);
            for(const auto& edge:edges_)if(edge.valid&&edge.resource.Get()==resource.Get()&&resource)
                return Fail("compositor secondary input aliases the TAA chain");
        }
        if(!vertex.textures.empty())return Fail("linked compositor vertex program reads textures");
        if(!targets[0])return Fail("linked compositor has no target zero");
        for(unsigned i=1;i<8;++i)if(targets[i])return Fail("MRT compositor is observation-only");
        if(!Coverage(command,targets[0].Get(),depth.Get()))return false;
        auto position=vertex.position;
        if(vertex.positionFromVertex) {
            if(!position.BindConstants(context_.Get()))return Fail("waiting for proven current fullscreen position constants");
            if(!fullscreen::Geometry(context_.Get(),command.indexed,command.count,command.first,command.base,&geometry_,&position))
                return Fail("waiting for proven fullscreen position/vertex/index content");
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};input->GetDesc(&view);
        if(view.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D||view.Texture2D.MostDetailedMip||source.Get()==candidateOutput_.Get())return Fail("compositor source subresource or alias");
        ComPtr<ID3D11Texture2D> sourceTexture;if(FAILED(source.As(&sourceTexture)))return false;D3D11_TEXTURE2D_DESC sd{};sourceTexture->GetDesc(&sd);
        if(sd.MipLevels!=1||sd.ArraySize!=1||sd.SampleDesc.Count!=1)return Fail("compositor source layout");
        original_=input;
        if(width_==renderWidth&&height_==renderHeight) {
            if(used_==8)return Fail("compositor chain exceeds eight same-frame nodes");
            // Observation proves command topology without replay snapshots or GPU
            // copies. Geometry readback retains its own EVENT retirement.
            if(observing_){++used_;candidateNode_=-3;return true;}
            if(nodes_.size()<=used_)nodes_.push_back(std::make_unique<Node>());
            if(!SnapshotNode(*nodes_[used_],command,pixel,vertex))return Fail("compositor input snapshot failed");
            candidateNode_=int(used_++);return true;
        }
        if(width_<=renderWidth||height_<=renderHeight||std::abs(double(width_)/height_-double(renderWidth)/renderHeight)>.005)return Fail("compositor is not a matching upscale edge");
        return true;
    }
    bool NeedsResolve()const{return candidateOutput_&&original_&&candidateNode_==-1&&candidateParent_!=-2;}
    bool Bind(ID3D11Texture2D* currentOutput) {
        if(!NeedsResolve()||!currentOutput||!context1_)return false;
        if(!work_){ComPtr<ID3D11Device1> d;D3D_FEATURE_LEVEL chosen{};const auto level=device_->GetFeatureLevel();
            if(FAILED(device_.As(&d))||FAILED(d->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&chosen,&work_)))return Fail("compositor state isolation failed");}
        std::vector<int> path;for(int n=candidateParent_;n>=0;n=nodes_[n]->parent)path.push_back(n);std::reverse(path.begin(),path.end());
        ComPtr<ID3D11ShaderResourceView> color;D3D11_SHADER_RESOURCE_VIEW_DESC sourceDesc{};original_->GetDesc(&sourceDesc);
        const DXGI_FORMAT firstFormat=path.empty()?sourceDesc.Format:nodes_[path.front()]->inputView.Format;
        D3D11_SHADER_RESOURCE_VIEW_DESC first{};first.Format=firstFormat;first.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;first.Texture2D.MipLevels=1;
        D3D11_TEXTURE2D_DESC inputDesc{};currentOutput->GetDesc(&inputDesc);
        ID3D11Texture2D* viewSource=currentOutput;
        if(inputDesc.Format!=firstFormat&&inputDesc.Format!=Storage(firstFormat)) {
            if(Storage(inputDesc.Format)!=Storage(firstFormat))return Fail("consumer SR view format family mismatch");
            // NGX keeps its typed UAV output. Only consumers needing another
            // typed view use a format-compatible typeless copy, on this queue.
            auto copyDesc=inputDesc;copyDesc.Format=Storage(firstFormat);copyDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            copyDesc.Usage=D3D11_USAGE_DEFAULT;copyDesc.CPUAccessFlags=copyDesc.MiscFlags=0;
            D3D11_TEXTURE2D_DESC old{};if(inputStorage_)inputStorage_->GetDesc(&old);
            if(!inputStorage_||std::memcmp(&old,&copyDesc,sizeof(old))) {inputStorage_.Reset();if(FAILED(device_->CreateTexture2D(&copyDesc,nullptr,&inputStorage_)))return Fail("consumer typed input storage allocation failed");}
            context_->CopyResource(inputStorage_.Get(),currentOutput);viewSource=inputStorage_.Get();
        }
        if(FAILED(device_->CreateShaderResourceView(viewSource,&first,&color)))return Fail("consumer SR view format mismatch");
        {
            ContextStateGuard state(context1_.Get(),work_.Get());
            for(const int index:path) {
                auto& node=*nodes_[index];
                ComPtr<ID3D11Resource> inputResource;color->GetResource(&inputResource);color.Reset();
                if(FAILED(device_->CreateShaderResourceView(inputResource.Get(),&node.inputView,&color)))return Fail("compositor node input view format mismatch");
                if(!MakeSurface(device_.Get(),width_,height_,node.format,node.output))return Fail("compositor sidecar allocation failed");
                context_->ClearState();context_->VSSetShader(node.vs.Get(),nullptr,0);context_->PSSetShader(node.ps.Get(),nullptr,0);
                context_->IASetInputLayout(node.layout.Get());context_->IASetPrimitiveTopology(node.topology);
                std::array<ID3D11Buffer*,32> vertices{};for(unsigned i=0;i<32;++i)vertices[i]=node.vertices[i].Get();
                context_->IASetVertexBuffers(0,32,vertices.data(),node.strides.data(),node.offsets.data());context_->IASetIndexBuffer(node.indices.Get(),node.indexFormat,node.indexOffset);
                for(unsigned stage=0;stage<2;++stage){auto& cb=node.constants[stage];std::array<ID3D11Buffer*,14> buffers{};for(unsigned i=0;i<14;++i)buffers[i]=cb.buffers[i].Get();
                    if(stage)context1_->PSSetConstantBuffers1(0,14,buffers.data(),cb.first.data(),cb.count.data());else context1_->VSSetConstantBuffers1(0,14,buffers.data(),cb.first.data(),cb.count.data());}
                std::array<ID3D11ShaderResourceView*,128> views{};for(unsigned i=0;i<128;++i)views[i]=node.views[i].Get();views[0]=color.Get();context_->PSSetShaderResources(0,128,views.data());
                std::array<ID3D11SamplerState*,16> samplers{};for(unsigned i=0;i<16;++i)samplers[i]=node.samplers[i].Get();context_->PSSetSamplers(0,16,samplers.data());
                ID3D11RenderTargetView* target=node.output.rtv.Get();context_->OMSetRenderTargets(1,&target,nullptr);
                D3D11_VIEWPORT viewport{0,0,float(width_),float(height_),0,1};context_->RSSetViewports(1,&viewport);context_->RSSetState(node.raster.Get());
                // The captured contract has no DS attachment or side effects.
                if(!noDepth_){D3D11_DEPTH_STENCIL_DESC depth{};if(FAILED(device_->CreateDepthStencilState(&depth,&noDepth_)))return Fail("compositor depth state creation failed");}context_->OMSetDepthStencilState(noDepth_.Get(),0);
                const auto& cmd=node.command;if(cmd.indexed)context_->DrawIndexed(cmd.count,cmd.first,cmd.base);else context_->Draw(cmd.count,cmd.first);
                color=node.output.srv;
            }
        }
        if(!path.empty()) {
            D3D11_SHADER_RESOURCE_VIEW_DESC last{};color->GetDesc(&last);
            if(last.Format!=sourceDesc.Format){ComPtr<ID3D11Resource> resource;color->GetResource(&resource);color.Reset();if(FAILED(device_->CreateShaderResourceView(resource.Get(),&sourceDesc,&color)))return Fail("compositor typed consumer view mismatch");}
        }
        replacement_=color;ID3D11ShaderResourceView* view=replacement_.Get();context_->PSSetShaderResources(0,1,&view);bound_=true;return true;
    }
    bool Returned() {
        const bool delivered=bound_;
        if(bound_){ID3D11ShaderResourceView* original=original_.Get();context_->PSSetShaderResources(0,1,&original);bound_=false;}
        else if(candidateOutput_&&(candidateNode_>=0||observing_))Publish(candidateOutput_.Get(),observing_?-1:candidateNode_);
        return delivered;
    }
    void AbandonBinding(){if(bound_){ID3D11ShaderResourceView* original=original_.Get();context_->PSSetShaderResources(0,1,&original);bound_=false;}}
};
} // namespace taa::consumer
