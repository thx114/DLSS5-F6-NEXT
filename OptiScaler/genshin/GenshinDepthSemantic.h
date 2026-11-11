// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GenshinCameraHistory.h"
#include "GenshinNativeMrtContract.h"
#include "GenshinDepthEvidence.h"
#include "GenshinDepthPortableResources.h"
#include "GenshinDepthRetention.h"
#include "GenshinInputObservation.h"
#include "../runtime/CpuSnapshotBytes.h"
#include "../runtime/MappedResourceJournal.h"
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <sstream>
#include <regex>
#include <unordered_map>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <malloc.h>
#pragma comment(lib,"d3dcompiler.lib")

// R696 fixed contract. No TypedGuides include and no caller-supplied matrix
// qualification flag. Only actual shader bytes and same-draw CB payloads can
// connect a returned writer to CameraHistory's observed camera matrices.
namespace GenshinDepthSemantic {
using Microsoft::WRL::ComPtr;
using Matrix=GenshinCameraHistory::Matrix;
struct Draw;
class WriterGroups;
struct WriterGroup;
enum class Reason : unsigned char { NoWriters, WrongOwner, ClearMismatch, Busy, WrongWriter,
    PayloadPending, PayloadFailed, ShaderOrMatrix, CompareMismatch, ViewportMismatch,
    NonUniformProjectionPhase, BridgeControls, BridgeSubmissionMismatch, OutputKernelPhaseUnproved, Qualified };
struct Result {
    bool deviceZ=false,phase=false,reversed=false;
    float clear=0,depthToOutputUvX=0,depthToOutputUvY=0;
    uint64_t writers=0,groups=0;
    Reason reason=Reason::NoWriters;
    // Measurements, never an output-phase permission. Zero initialization is
    // not a phase fact: only phase=true could authorize depthToOutputUv*.
    float rasterPixelsX=0,rasterPixelsY=0,submittedPixelsX=0,submittedPixelsY=0;
    bool submissionClamped=false;
    // Actual source-plane proof, never a replacement engine camera or a
    // previous-frame association. Published only after every writer agrees.
    Matrix rasterWorldToClip{},rasterProjection{};
    bool rasterBasis=false;
};
inline const char* ReasonText(Reason r)noexcept {
    constexpr const char* names[]={"NO_RETURNED_WRITERS","OWNER_OR_CAMERA_MISMATCH","FAR_CLEAR_MISMATCH",
        "SEMANTIC_STATE_BUSY","WRITER_OWNER_MISMATCH","SAME_DRAW_CB_PENDING","SAME_DRAW_CB_FAILED",
        "SHADER_OR_ACTUAL_GPU_MATRIX_UNPROVED","DEPTH_COMPARE_MISMATCH","FULL_VIEWPORT_MISMATCH",
        "NON_UNIFORM_PROJECTION_PHASE","BRIDGE_PHASE_CONTROLS_UNSUPPORTED","ACTUAL_BRIDGE_JITTER_MISMATCH",
        "SELECTED_SR_OUTPUT_KERNEL_PHASE_UNPROVED","ACTUAL_DLSS12_OUTPUT_GRID_MATCHED"};
    const auto n=static_cast<unsigned>(r);return n<std::size(names)?names[n]:"INVALID_SEMANTIC_REASON";
}
struct Binding {ID3D11Buffer* buffer=nullptr;UINT first=0,count=0;};
// Scalar provenance of this exact failed Capture, copied with its real raster
// interval. Never a shader/CB certificate, and never consulted by admission.
enum class CaptureFailure:unsigned {NotAttempted,None,Vertex,Pixel,Owner,Arguments,BufferState,
    IndirectPending,Allocation,PayloadFailed,Retention,Loss,Exception,Hull,Domain,StageLinkage,TerminalDataflow};
struct CaptureFailureObservation {
    CaptureFailure reason=CaptureFailure::NotAttempted;
    uint64_t tick=0,call=0,revision=0,loss=0;uintptr_t buffer=0;DWORD thread=0;
    UINT kind=0,count=0,instances=0,instanceStart=0,slot=UINT_MAX,first=0,constants=0,state=0;
    UINT stage=0,terminalInstruction=0,dependency=0,programFailure=0,resourceSlot=UINT_MAX,resourceType=UINT_MAX;
    uintptr_t shader=0;
    std::array<char,40> terminalOpcode{};
    bool indirect=false;
};
inline const char* CaptureFailureText(CaptureFailure reason)noexcept {
    constexpr const char* names[]{"NOT_ATTEMPTED","CAPTURE_PASSED","VERTEX_PROGRAM_UNAVAILABLE_OR_UNQUALIFIED",
        "PIXEL_PROGRAM_UNAVAILABLE_OR_UNQUALIFIED","SEMANTIC_OWNER_OR_ACCESS","ACTUAL_DRAW_ARGUMENTS_UNSUPPORTED",
        "SAME_DRAW_BUFFER_STATE_OR_RANGE","INDIRECT_SAME_DRAW_PAYLOAD_NOT_READY","PAYLOAD_ALLOCATION_OR_BUDGET",
        "SAME_DRAW_PAYLOAD_FAILED","DRAW_BUFFER_RETENTION","SEMANTIC_LOSS_CHANGED","CAPTURE_EXCEPTION","HULL_PROGRAM_UNAVAILABLE_OR_UNQUALIFIED",
        "DOMAIN_PROGRAM_UNAVAILABLE_OR_UNQUALIFIED","TESSELLATION_STAGE_LINKAGE","TERMINAL_POSITION_DATAFLOW_UNSUPPORTED"};
    const auto i=static_cast<unsigned>(reason);return i<std::size(names)?names[i]:"INVALID_CAPTURE_FAILURE";
}
// Called only inside authenticated Genshin ReturnedRaster callbacks. These
// proofs are immutable same-draw handles, not a camera/DeviceZ override API.
std::shared_ptr<const Draw> Capture(ID3D11DeviceContext*,ID3D11VertexShader*,ID3D11HullShader*,ID3D11DomainShader*,ID3D11PixelShader*,D3D11_PRIMITIVE_TOPOLOGY,
    const std::array<Binding,42>&,const D3D11_VIEWPORT&,D3D11_COMPARISON_FUNC,
    uint64_t deviceEpoch,uint64_t configEpoch,CaptureFailureObservation* observation=nullptr)noexcept;
bool Returned(ID3D11DeviceContext*,const std::shared_ptr<const Draw>&)noexcept;
// Poll uses DONOTFLUSH / DO_NOT_WAIT; neither it nor capture waits for GPU work.
void Poll(ID3D11DeviceContext*)noexcept;
// Numeric shadow mutation calls bracket the actual unchanged API invocation.
class Submission;
void Invalidate(ID3D11DeviceContext*,ID3D11Resource*,bool mapped=false)noexcept;
void Unmapped(ID3D11DeviceContext*,ID3D11Resource*)noexcept;
Result Resolve(const WriterGroups&,float actualClear,
    const GenshinLegacy::NativeMrt::Frame&,const GenshinFsr::Parameters&,
    const GenshinCameraHistory::Sample&,const GenshinGuides::SrOutputGrid* outputGrid=nullptr)noexcept;
// Same captured writers/clear, qualified once by the immutable native source.
// Unlike Resolve's exact observed-camera contract, this proves the actual
// raster lens and rigid view; Render's newer pose/XY jitter is not source Z.
Result ResolvePrepared(const WriterGroups&,float actualClear,
    const GenshinLegacy::NativeMrt::Frame&,const GenshinFsr::Parameters&,
    const GenshinCameraHistory::Sample&,const GenshinGuides::SrOutputGrid*)noexcept;
}

namespace GenshinDepthSemantic {
namespace detail {
inline bool Finite(float f)noexcept {uint32_t b;std::memcpy(&b,&f,4);return (b&0x7f800000u)!=0x7f800000u;}
inline bool Copy(void* d,const void* s,size_t n)noexcept {
    SIZE_T got=0;return d && s && n && ReadProcessMemory(GetCurrentProcess(),s,d,n,&got) && got==n;
}
enum class Op:unsigned char {Unknown,Constant,Input,Buffer,Add,Mul,Opaque,Saturate,IndexedBuffer,IntegerAdd,IntegerMul,Select,
    Absolute,IntegerNegate,IntegerAnd,IntegerOr,IntegerShiftLeft};
inline bool Unary(Op op)noexcept {
    return op==Op::Saturate || op==Op::IndexedBuffer || op==Op::Absolute || op==Op::IntegerNegate;
}
inline bool BinaryNode(Op op)noexcept {
    return op==Op::Add || op==Op::Mul || op==Op::IntegerAdd || op==Op::IntegerMul ||
        op==Op::IntegerAnd || op==Op::IntegerOr || op==Op::IntegerShiftLeft;
}
struct Node {Op op=Op::Unknown;int a=0,b=0,c=0;float value=0;UINT slot=0,row=0,lane=0;
    // Structural dependency, retained even through opaque operations and zero
    // coefficients. It is not a guessed numeric SV_InstanceID value.
    bool instanceDependent=false;
};
enum class Stage:unsigned char {Vertex,Pixel,Hull,Domain,ControlVertex};
enum class ProgramFailure:unsigned {None,CreateBytes,Reflection,ShaderType,ResourceBinding,NonCbResource,
    PositionSignature,InputSignature,PatchSignature,Disassembly,TerminalDataflow,CacheUnavailable};
inline const char* ProgramFailureText(unsigned value)noexcept {
    constexpr const char* names[]{"NONE","CREATE_BYTES_UNAVAILABLE","REFLECTION_UNAVAILABLE","SHADER_STAGE_MISMATCH",
        "RESOURCE_BINDING_LAYOUT","PRE_RASTER_NON_CB_RESOURCE","TERMINAL_POSITION_SIGNATURE","INPUT_SIGNATURE",
        "PATCH_SIGNATURE","DISASSEMBLY_UNAVAILABLE","TERMINAL_DATAFLOW","PROGRAM_CACHE_UNAVAILABLE"};
    return value<std::size(names)?names[value]:"UNKNOWN_PROGRAM_FAILURE";
}
struct Program {
    Stage stage=Stage::Vertex;bool valid=false,interfaceValid=false;
    ComPtr<ID3D11ShaderReflection> reflection;
    ProgramFailure failure=ProgramFailure::CreateBytes;UINT resourceSlot=UINT_MAX,resourceType=UINT_MAX;
    UINT failedInstruction=0,failedDependency=0;std::array<char,40> failedOpcode{};std::vector<Node> nodes{{}};
    std::array<int,4> world{};UINT matrixSlot=0,matrixRow=0;bool transpose=false;
    // Every reachable acyclic return/position alternative, all sharing the
    // same exact CB matrix location. MatrixFor proves W for every alternative.
    std::vector<std::array<int,4>> worlds;
    std::array<bool,14> used{};bool needsPositionW=false;
    // Phase19 observation only, retained from the already-read shader bytes.
    uint64_t diagnosticByteHash=0;UINT diagnosticByteSize=0,diagnosticTerminal=UINT_MAX;
};
inline std::string Strip(std::string s) {
    const auto comment=s.find("//");if(comment!=s.npos)s.resize(comment);
    const auto begin=s.find_first_not_of(" \t\r\n\0",0,5);if(begin==s.npos)return {};
    s.erase(0,begin);const auto end=s.find_last_not_of(" \t\r\n\0",s.npos,5);s.resize(end+1);return s;
}
inline std::vector<std::string> Operands(const std::string& s) {
    std::vector<std::string> out;unsigned depth=0;size_t start=0;
    for(size_t i=0;i<s.size();++i){if(s[i]=='(')++depth;else if(s[i]==')'){if(!depth)return {};--depth;}
        else if(s[i]==',' && !depth){out.push_back(Strip(s.substr(start,i-start)));start=i+1;}}
    if(depth)return {};out.push_back(Strip(s.substr(start)));return out;
}
inline int NodeId(Program& p,Node n,std::unordered_multimap<uint64_t,int>& intern) {
    if(p.nodes.size()>=8192)return 0;
    const auto dependent=[&](int id){return id>0 && static_cast<size_t>(id)<p.nodes.size() && p.nodes[id].instanceDependent;};
    if(n.op==Op::Input && n.slot==2)n.instanceDependent=true;
    else if(BinaryNode(n.op))n.instanceDependent=dependent(n.a) || dependent(n.b);
    else if(Unary(n.op))n.instanceDependent=dependent(n.a);
    else if(n.op==Op::Select)n.instanceDependent=dependent(n.a) || dependent(n.b) || dependent(n.c);
    // Opaque.a is an instruction ordinal, not an operand edge. Parser supplies
    // its dependency bit from every actual source before discarding the edges.
    // Intern scalar expressions: terminal matrix terms need exact same-world
    // expressions independent of instruction spelling/register reuse.
    // New world shaders previously scanned the entire DAG for every operand
    // lane. Index construction candidates; exact equality still interns nodes.
    uint32_t bits=0;std::memcpy(&bits,&n.value,sizeof(bits));
    uint64_t key=static_cast<unsigned>(n.op);
    const std::array<uint32_t,8> parts{static_cast<uint32_t>(n.a),static_cast<uint32_t>(n.b),static_cast<uint32_t>(n.c),
        n.slot,n.row,n.lane,bits,static_cast<uint32_t>(n.instanceDependent)};
    for(auto part:parts)key=(key^part)*1099511628211ull;
    const auto range=intern.equal_range(key);
    for(auto it=range.first;it!=range.second;++it){const auto& a=p.nodes[it->second];
        if(a.op==n.op && a.a==n.a && a.b==n.b && a.c==n.c && a.slot==n.slot && a.row==n.row && a.lane==n.lane &&
           a.instanceDependent==n.instanceDependent && !std::memcmp(&a.value,&n.value,4))return it->second;}
    p.nodes.push_back(n);const auto id=static_cast<int>(p.nodes.size()-1);
    intern.emplace(key,id);return id;
}
struct Parser {
    Program& p;UINT input=UINT_MAX,output=UINT_MAX,instance=UINT_MAX;
    std::unordered_multimap<uint64_t,int> intern; // compilation only, released with Parser
    std::array<std::array<int,4>,256> r{};std::array<std::array<int,4>,32> o{};
    std::array<UINT,32> inputMasks{},patchMasks{};UINT controlPoints=0;
    std::array<bool,16> samplerDeclarations{};
    std::array<bool,128> resourceDeclarations{};
    std::array<bool,128> floatTexture2d{};
    // Declarations describe register layout. Stripped DXBC can retain SHEX
    // access instructions without RDEF bindings, so reflection is not an
    // instruction dependency oracle. Preserve its non-CB refusal independently;
    // validate each actual supported read against declarations below.
    bool ResourceDeclaration(const std::string& op,const std::string& text) {
        std::smatch m;
        if(op=="dcl_sampler"){
            static const std::regex sampler(R"(s([0-9]+), (mode_default|mode_comparison|mode_mono))");
            if(!std::regex_match(text,m,sampler))return false;
            const auto slot=std::stoul(m[1]);if(slot>=samplerDeclarations.size() || samplerDeclarations[slot])return false;
            samplerDeclarations[slot]=true;return true;
        }
        static const std::regex kind(R"(dcl_resource_(buffer|texture1d|texture1darray|texture2d|texture2darray|texture3d|texturecube|texturecubearray))");
        static const std::regex resource(R"(\((float|uint|sint|unorm|snorm|mixed|unused), *(float|uint|sint|unorm|snorm|mixed|unused), *(float|uint|sint|unorm|snorm|mixed|unused), *(float|uint|sint|unorm|snorm|mixed|unused)\) t([0-9]+))");
        if(!std::regex_match(op,kind) || !std::regex_match(text,m,resource))return false;
        const auto slot=std::stoul(m[5]);if(slot>=resourceDeclarations.size() || resourceDeclarations[slot])return false;
        resourceDeclarations[slot]=true;
        floatTexture2d[slot]=op=="dcl_resource_texture2d" && m[1]=="float" && m[2]=="float" && m[3]=="float" && m[4]=="float";
        return true;
    }
    bool NumericDependencies() {
        // Frozen numeric CBs serve the camera VP and homogeneous W only.
        // World XYZ (including skinning/material CBs) is universally symbolic;
        // reading it cannot require an unrelated same-draw numerical payload.
        // This plan runs only after EVERY return factored to the same matrix.
        p.used={};p.needsPositionW=false;if(p.matrixSlot>=p.used.size() || p.worlds.empty())return false;
        p.used[p.matrixSlot]=true;
        std::vector<int> pending;std::vector<bool> visited(p.nodes.size());
        for(const auto& world:p.worlds)pending.push_back(world[3]);
        while(!pending.empty()){
            const int id=pending.back();pending.pop_back();
            if(id<=0 || static_cast<size_t>(id)>=p.nodes.size())return false;
            if(visited[id])continue;visited[id]=true;const auto& n=p.nodes[id];
            if(n.op==Op::Buffer || n.op==Op::IndexedBuffer){
                if(n.slot>=p.used.size())return false;p.used[n.slot]=true;
            }
            if(n.op==Op::Input && n.slot==0 && n.lane==3)p.needsPositionW=true;
            if(BinaryNode(n.op)){pending.push_back(n.a);pending.push_back(n.b);}
            else if(Unary(n.op))pending.push_back(n.a);
            else if(n.op==Op::Select){pending.push_back(n.a);pending.push_back(n.b);pending.push_back(n.c);}
            // Opaque.a is an ordinal, never an operand edge. Evaluate still
            // refuses it as W or as a matrix address; no numeric permission.
        }
        return true;
    }
    using Vector=std::array<int,4>;
    // Analysis-time SSA snapshots, never game-state replay or a runtime loop.
    // Vector storage keeps the bounded 16-frame control stack off the hook stack.
    struct FlowFrame {
        decltype(r) entryR{},thenR{};decltype(o) entryO{},thenO{};
        int condition=0;bool zero=false,hasElse=false,thenLive=false;
    };
    std::vector<FlowFrame> flow;unsigned terminalVisits=0;
    int Select(int condition,int yes,int no) {
        if(!condition || !yes || !no)return 0;
        // Unknown-instance control stays structural even with equal branches.
        if(yes==no && !p.nodes[condition].instanceDependent)return yes;
        Node n;n.op=Op::Select;n.a=condition;n.b=yes;n.c=no;return NodeId(p,n,intern);
    }
    bool Merge(const FlowFrame& frame) {
        for(size_t i=0;i<r.size();++i)for(unsigned c=0;c<4;++c)
            r[i][c]=frame.zero?Select(frame.condition,r[i][c],frame.thenR[i][c]):
                Select(frame.condition,frame.thenR[i][c],r[i][c]);
        for(size_t i=0;i<o.size();++i)for(unsigned c=0;c<4;++c)
            o[i][c]=frame.zero?Select(frame.condition,o[i][c],frame.thenO[i][c]):
                Select(frame.condition,frame.thenO[i][c],o[i][c]);
        return p.nodes.size()<8192;
    }
    Vector Read(const std::string& text,bool integer=false) {
        if(text.size()>1 && text.front()=='-' && text[1]!='-'){
            auto value=Read(text.substr(1),integer);
            if(integer){for(auto& lane:value)if(lane){Node n;n.op=Op::IntegerNegate;n.a=lane;lane=NodeId(p,n,intern);}}
            else {Node minus;minus.op=Op::Constant;minus.value=-1;
                const int id=NodeId(p,minus,intern);for(auto& lane:value)lane=Binary(Op::Mul,id,lane);}
            return value;
        }
        if(text.size()>2 && text.front()=='|' && text.back()=='|'){
            if(integer)return {};auto value=Read(text.substr(1,text.size()-2));
            for(auto& lane:value)if(lane){Node n;n.op=Op::Absolute;n.a=lane;lane=NodeId(p,n,intern);}return value;
        }
        Vector result{};std::smatch m;
        if(p.stage==Stage::Domain){
            static const std::regex cp(R"((?:vcp|vicp)\[([0-9]+)\]\[([0-9]+)\](?:\.([xyzw]{1,4}))?)");
            static const std::regex domain(R"(vDomain(?:\.([xyzw]{1,4}))?)");
            static const std::regex patch(R"(vpc\[([0-9]+)\](?:\.([xyzw]{1,4}))?)");
            UINT kind=0,row=0,point=0;std::string swizzle;
            if(std::regex_match(text,m,cp)){
                point=UINT(std::stoul(m[1]));row=UINT(std::stoul(m[2]));swizzle=m[3];kind=3;
                if(point>=controlPoints || row>=32)return {};
            }else if(std::regex_match(text,m,domain)){swizzle=m[1];kind=4;}
            else if(std::regex_match(text,m,patch)){row=UINT(std::stoul(m[1]));swizzle=m[2];kind=5;if(row>=32)return {};}
            if(kind){
                if(swizzle.empty())swizzle="xyzw";if(swizzle.size()!=1 && swizzle.size()!=4)return {};
                for(UINT c=0;c<4;++c){const UINT lane=UINT(std::string("xyzw").find(swizzle[swizzle.size()==1?0:c]));
                    if((kind==3 && !(inputMasks[row]&(1u<<lane))) || (kind==5 && !(patchMasks[row]&(1u<<lane))))return {};
                    Node n;n.op=Op::Input;n.slot=kind;n.row=kind==3?point*32+row:row;n.lane=lane;
                    result[c]=NodeId(p,n,intern);
                }
                return result;
            }
        }
        static const std::regex reg(R"(([rv])([0-9]+)(?:\.([xyzw]{1,4}))?)");
        static const std::regex cb(R"(cb([0-9]+)\[([0-9]+)\](?:\.([xyzw]{1,4}))?)");
        static const std::regex indexed(R"(cb([0-9]+)\[(r[0-9]+\.[xyzw]) \+ ([0-9]+)\](?:\.([xyzw]{1,4}))?)");
        if(std::regex_match(text,m,indexed)){
            const UINT slot=static_cast<UINT>(std::stoul(m[1])),offset=static_cast<UINT>(std::stoul(m[3]));
            if(slot>=14 || offset>=4096)return {};const auto index=Read(m[2]);if(!index[0])return {};
            auto swizzle=m[4].str();if(swizzle.empty())swizzle="xyzw";if(swizzle.size()!=1&&swizzle.size()!=4)return {};
            p.used[slot]=true;for(unsigned c=0;c<4;++c){Node n;n.op=Op::IndexedBuffer;n.a=index[0];n.slot=slot;n.row=offset;
                n.lane=static_cast<UINT>(std::string("xyzw").find(swizzle[swizzle.size()==1?0:c]));result[c]=NodeId(p,n,intern);}return result;
        }
        if(text.size()>3 && text.substr(0,2)=="l(" && text.back()==')'){
            const auto parts=Operands(text.substr(2,text.size()-3));if(parts.size()!=1 && parts.size()!=4)return result;
            for(unsigned c=0;c<4;++c){char* end=nullptr;const auto& s=parts[parts.size()==1?0:c];
                Node n;n.op=Op::Constant;
                // Disassembly integer/hex literals denote raw 32-bit lanes.
                // Never round an address via float, or turn 0x3f800000 into
                // the numerical float 1065353216. Floating spellings stay f32.
                const bool hex=s.size()>2 && s[0]=='0' && (s[1]=='x' || s[1]=='X');
                if(hex || s.find_first_of(".eE")==s.npos){
                    const auto bits=std::strtoll(s.c_str(),&end,hex?16:10);
                    if(end!=s.c_str()+s.size() || bits<INT32_MIN || bits>UINT32_MAX)return {};
                    const auto raw=static_cast<uint32_t>(bits);std::memcpy(&n.value,&raw,4);
                }else {n.value=std::strtof(s.c_str(),&end);if(end!=s.c_str()+s.size() || !Finite(n.value))return {};}
                result[c]=NodeId(p,n,intern);}return result;
        }
        bool buffer=std::regex_match(text,m,cb);UINT slot=0,row=0;std::string swizzle;
        if(buffer){slot=static_cast<UINT>(std::stoul(m[1]));row=static_cast<UINT>(std::stoul(m[2]));swizzle=m[3];
            if(slot>=14 || row>=4096)return {};p.used[slot]=true;
        }else if(std::regex_match(text,m,reg)){
            row=static_cast<UINT>(std::stoul(m[2]));swizzle=m[3];if(row>=256)return {};
        }else return {};
        if(swizzle.empty())swizzle="xyzw";if(swizzle.size()!=1 && swizzle.size()!=4)return {};
        for(unsigned c=0;c<4;++c){const auto lane=std::string("xyzw").find(swizzle[swizzle.size()==1?0:c]);
            if(buffer){Node n;n.op=Op::Buffer;n.slot=slot;n.row=row;n.lane=static_cast<UINT>(lane);result[c]=NodeId(p,n,intern);}
            else if(m[1]=="r")result[c]=r[row][lane];
            else if(row<32 && (inputMasks[row]&(1u<<lane))){Node n;n.op=Op::Input;n.row=row;
                n.slot=row==input?0u:row==instance?2u:1u;n.lane=static_cast<UINT>(lane);result[c]=NodeId(p,n,intern);}
        }return result;
    }
    bool Write(const std::string& text,const Vector& v) {
        static const std::regex dest(R"(([ro])([0-9]+)(?:\.([xyzw]{1,4}))?)");std::smatch m;
        if(!std::regex_match(text,m,dest))return false;const auto index=std::stoul(m[2]);
        const bool isOutput=m[1]=="o";if(index>=(isOutput?32:256))return false;
        const auto mask=m[3].str().empty()?std::string("xyzw"):m[3].str();unsigned prior=0;
        for(unsigned j=0;j<mask.size();++j){const auto c=std::string("xyzw").find(mask[j]);
            if(j && c<=prior)return false;prior=static_cast<unsigned>(c);
            // DXBC lanes are positional, even for a destination component mask.
            (isOutput?o[index]:r[index])[c]=v[c];}
        return true;
    }
    int Binary(Op op,int a,int b){if(!a || !b)return 0;Node n;n.op=op;n.a=a;n.b=b;return NodeId(p,n,intern);}
    Vector Opaque(unsigned instruction,const std::vector<Vector>& inputs,unsigned dot=0,unsigned result=0) {
        Vector value{};
        for(unsigned c=0;c<4;++c){bool valid=true,dependent=false;
            for(const auto& input:inputs)for(unsigned j=0;j<(dot?dot:1);++j){const auto id=input[dot?j:c];
                if(!id){valid=false;continue;}dependent=dependent || p.nodes[id].instanceDependent;}
            if(valid){Node n;n.op=Op::Opaque;n.a=static_cast<int>(instruction);n.row=result;n.lane=dot?0:c;
                n.instanceDependent=dependent;value[c]=NodeId(p,n,intern);}
        }
        return value;
    }
    bool SampleL(const std::vector<std::string>& a,unsigned instruction) {
        // Only the actual read-only float Texture2D + explicit scalar LOD
        // encoding seen in the captured world shaders is decoded here. The
        // sampled value is universal XYZ, never a numeric W/matrix/address.
        if(a.size()!=5)return false;std::smatch t,s;
        static const std::regex texture(R"(t([0-9]+)\.([xyzw]{4}))"),sampler(R"(s([0-9]+))");
        if(!std::regex_match(a[2],t,texture) || !std::regex_match(a[3],s,sampler))return false;
        const auto ti=std::stoul(t[1]),si=std::stoul(s[1]);
        if(ti>=floatTexture2d.size() || !floatTexture2d[ti] || si>=samplerDeclarations.size() || !samplerDeclarations[si])return false;
        const auto uv=Read(a[1]),lod=Read(a[4]);if(!uv[0] || !uv[1] || !lod[0])return false;
        const auto values=Opaque(instruction,{{uv[0],uv[0],uv[0],uv[0]},
            {uv[1],uv[1],uv[1],uv[1]},{lod[0],lod[0],lod[0],lod[0]}});
        Vector selected{};const auto swizzle=t[2].str();
        for(unsigned c=0;c<4;++c)selected[c]=values[std::string("xyzw").find(swizzle[c])];
        return Write(a[0],selected);
    }
    bool Run(const std::string& assembly) {
        std::istringstream inputStream(assembly);std::string line;unsigned instructions=0;bool version=false,ended=false,live=true;
        while(std::getline(inputStream,line)){
            line=Strip(line);if(line.empty())continue;const auto space=line.find_first_of(" \t");
            auto op=line.substr(0,space);const auto rest=space==line.npos?std::string{}:Strip(line.substr(space+1));
            p.failedInstruction=instructions+1;
            p.failedOpcode={};op.copy(p.failedOpcode.data(),p.failedOpcode.size()-1);
            if(!version){
                if(p.stage==Stage::Domain?line!="ds_5_0":(line!="vs_5_0" && line!="vs_4_0" && line!="vs_4_1"))return false;
                version=true;continue;
            }
            if(op.rfind("dcl_",0)==0){
                if(ended || instructions)return false;
                const bool common=op=="dcl_globalFlags" || op=="dcl_constantbuffer" || op=="dcl_input" ||
                    op=="dcl_input_siv" || op=="dcl_input_sgv" || op=="dcl_output" || op=="dcl_output_siv" || op=="dcl_temps";
                const bool tess=p.stage==Stage::Domain && (op=="dcl_input_control_point_count" || op=="dcl_tessellator_domain");
                if(!common && !tess){
                    // Declarations do not authorize accesses. The unchanged
                    // reflection non-CB gate and the explicit read decoder
                    // below are independent checks, including stripped DXBC.
                    if(ResourceDeclaration(op,rest))continue;
                    p.failedDependency=1;return false;
                }
                if(op=="dcl_constantbuffer" && rest.find("immediateIndexed")==rest.npos && rest.find("dynamicIndexed")==rest.npos)return false;continue;
            }
            if(++instructions>2048)return false;
            // The compiler may retain one final unreachable ret after both
            // arms returned. No new effect or unproved position is admitted.
            if(ended){if(op=="ret" && rest.empty() && flow.empty())continue;return false;}
            p.failedDependency=4; // actual structured control, distinct from factorization
            if(op=="if_z" || op=="if_nz"){
                if(!live || flow.size()>=16)return false;const auto condition=Read(rest);
                if(!condition[0])return false;
                flow.emplace_back();auto& frame=flow.back();
                frame.entryR=r;frame.entryO=o;frame.condition=condition[0];frame.zero=op=="if_z";continue;
            }
            if(op=="else"){
                if(!rest.empty() || flow.empty() || flow.back().hasElse)return false;
                auto& frame=flow.back();frame.thenR=r;frame.thenO=o;frame.thenLive=live;frame.hasElse=true;
                r=frame.entryR;o=frame.entryO;live=true;continue;
            }
            if(op=="endif"){
                if(!rest.empty() || flow.empty())return false;auto& frame=flow.back();
                if(!frame.hasElse){frame.thenR=r;frame.thenO=o;frame.thenLive=live;r=frame.entryR;o=frame.entryO;live=true;}
                if(frame.thenLive && live){if(!Merge(frame))return false;}
                else if(frame.thenLive){r=frame.thenR;o=frame.thenO;live=true;}
                // A returning arm has already recorded its exact position;
                // only the still-live arm participates in following instructions.
                flow.pop_back();if(!live && flow.empty())ended=true;continue;
            }
            if(!live)return false;
            if(op=="ret" || op=="retc_z" || op=="retc_nz"){
                if(op=="ret"?!rest.empty():!Read(rest)[0])return false;
                p.failedDependency=2;
                if(output>=32 || !Terminal(o[output]))return false;
                if(op=="ret"){live=false;ended=flow.empty();}continue;
            }
            const bool saturate=op.size()>4 && op.compare(op.size()-4,4,"_sat")==0;
            if(saturate)op.resize(op.size()-4);
            // Dependency 3 identifies a decoded instruction/operand domain;
            // declaration (1) and final SV_Position factorization (2) are separate.
            p.failedDependency=3;
            const auto a=Operands(rest);
            if(op=="sample_l_indexable(texture2d)(float,float,float,float)"){
                if(saturate || !SampleL(a,instructions))return false;continue;
            }
            if(op=="udiv"){
                if(saturate || a.size()!=4 || (a[0]=="null" && a[1]=="null"))return false;
                const auto x=Read(a[2],true),y=Read(a[3],true);
                // Quotient and remainder have distinct SSA values. Both read
                // the pre-instruction operands even when destinations alias.
                const auto quotient=Opaque(instructions,{x,y},0,0),remainder=Opaque(instructions,{x,y},0,1);
                if((a[0]!="null" && !Write(a[0],quotient)) || (a[1]!="null" && !Write(a[1],remainder)))return false;
                continue;
            }
            if(op=="iadd" || op=="imul" || op=="and" || op=="or" || op=="ishl"){
                if(saturate || (op=="imul"?a.size()!=4:a.size()!=3) || (op=="imul" && a[0]!="null"))return false;
                const auto x=Read(a[op=="imul"?2:1],true),y=Read(a[op=="imul"?3:2],true);Vector result{};
                const auto operation=op=="iadd"?Op::IntegerAdd:op=="imul"?Op::IntegerMul:
                    op=="and"?Op::IntegerAnd:op=="or"?Op::IntegerOr:Op::IntegerShiftLeft;
                for(unsigned c=0;c<4;++c)result[c]=Binary(operation,x[c],y[c]);
                if(!Write(a[op=="imul"?1:0],result))return false;continue;
            }
            // These operations are bounded straight-line, single-destination
            // writes. Their numeric value may stay opaque upstream of world
            // XYZ; they cannot certify W, matrix coefficients or addresses.
            // Structured selection retains BOTH alternatives and every return.
            unsigned opaqueArity=0;
            if(op=="rsq" || op=="rcp" || op=="sqrt" || op=="exp" || op=="log" ||
               op=="frc" || op=="round_ne" || op=="round_ni" || op=="round_pi" || op=="round_z" ||
               op=="itof" || op=="utof" || op=="ftoi" || op=="ftou")opaqueArity=2;
            if(op=="dp3" || op=="dp2" || op=="min" || op=="max" || op=="lt" || op=="ge" || op=="eq" || op=="ne" ||
               op=="div" || op=="ilt" || op=="ige" || op=="ieq" || op=="ine" || op=="imax" || op=="imin")opaqueArity=3;
            if(op=="movc"){
                if(a.size()!=4)return false;
                const auto condition=Read(a[1]),yes=Read(a[2]),no=Read(a[3]);Vector value{};
                for(unsigned c=0;c<4;++c){value[c]=Select(condition[c],yes[c],no[c]);
                    if(saturate && value[c]){Node n;n.op=Op::Saturate;n.a=value[c];value[c]=NodeId(p,n,intern);}}
                if(!Write(a[0],value))return false;continue;
            }
            if(opaqueArity){
                if(a.size()!=opaqueArity)return false;
                const bool integer=op=="itof" || op=="utof" || op=="ilt" || op=="ige" || op=="ieq" || op=="ine" || op=="imax" || op=="imin";
                if(saturate && (integer || op=="ftoi" || op=="ftou"))return false;
                std::vector<Vector> operands;for(unsigned j=1;j<opaqueArity;++j)operands.push_back(Read(a[j],integer));
                auto value=Opaque(instructions,operands,op=="dp2"?2:op=="dp3"?3:0);
                if(saturate)for(auto& id:value)if(id){Node n;n.op=Op::Saturate;n.a=id;id=NodeId(p,n,intern);}
                if(!Write(a[0],value))return false;continue;
            }
            const unsigned arity=op=="mov"?2:op=="mad"?4:3;
            if((op!="mov" && op!="add" && op!="mul" && op!="mad" && op!="dp4") || a.size()!=arity)return false;
            const auto x=Read(a[1]);Vector y{},z{},v{};if(arity>2)y=Read(a[2]);if(arity>3)z=Read(a[3]);
            for(unsigned c=0;c<4;++c){
                if(op=="mov")v[c]=x[c];
                else if(op=="mul")v[c]=Binary(Op::Mul,x[c],y[c]);
                else if(op=="add")v[c]=Binary(Op::Add,x[c],y[c]);
                else if(op=="mad")v[c]=Binary(Op::Add,Binary(Op::Mul,x[c],y[c]),z[c]);
            }
            if(op=="dp4"){int sum=Binary(Op::Mul,x[0],y[0]);for(unsigned c=1;c<4;++c)sum=Binary(Op::Add,sum,Binary(Op::Mul,x[c],y[c]));v.fill(sum);}
            if(saturate)for(auto& id:v){if(id){Node n;n.op=Op::Saturate;n.a=id;id=NodeId(p,n,intern);}}
            if(!Write(a[0],v))return false;
        }const bool valid=version && ended && !live && flow.empty() && !p.worlds.empty();
        if(valid){p.failedInstruction=0;p.failedOpcode={};p.failedDependency=0;}return valid;
    }
    bool Terms(int id,std::vector<std::pair<int,int>>& terms,unsigned depth=0) {
        if(!id || depth>64 || terms.size()>4)return false;const auto n=p.nodes[id];
        if(n.op==Op::Add)return Terms(n.a,terms,depth+1) && Terms(n.b,terms,depth+1);
        // DXBC folds homogeneous world.w=1 into a bare fourth CB row.
        // Retain exactly the same algebraic term; MatrixFor must still prove
        // the common world vector's W is 1 before certifying a camera VP.
        if(n.op==Op::Buffer){Node one;one.op=Op::Constant;one.value=1;
            const int unit=NodeId(p,one,intern);if(!unit)return false;
            terms.push_back({id,unit});return terms.size()<=4;}
        if(n.op!=Op::Mul)return false;
        if(p.nodes[n.a].op==Op::Buffer)terms.push_back({n.a,n.b});
        else if(p.nodes[n.b].op==Op::Buffer)terms.push_back({n.b,n.a});else return false;
        return terms.size()<=4;
    }
    bool RecordWorld(const Vector& world,UINT slot,UINT row,bool transpose) {
        if(p.worlds.empty()){p.matrixSlot=slot;p.matrixRow=row;p.transpose=transpose;p.world=world;}
        else if(p.matrixSlot!=slot || p.matrixRow!=row || p.transpose!=transpose)return false;
        if(std::find(p.worlds.begin(),p.worlds.end(),world)!=p.worlds.end())return true;
        if(p.worlds.size()>=64)return false;p.worlds.push_back(world);return true;
    }
    bool Terminal(const Vector& position,unsigned depth=0) {
        // Count visits, including repeated identical leaves, so acyclic
        // selection cannot turn deduplication into exponential analysis work.
        if(depth>16 || ++terminalVisits>128)return false;
        // A branch/movc may select whole clip positions. Split only actual
        // SSA Select edges, carrying unchanged lanes into both alternatives.
        // Every possible leaf must factor to the same real CB matrix.
        int condition=0;
        for(int id:position){if(id<=0 || size_t(id)>=p.nodes.size())return false;
            if(p.nodes[id].op==Op::Select){condition=p.nodes[id].a;break;}}
        if(condition){Vector yes=position,no=position;
            for(unsigned c=0;c<4;++c){const auto& n=p.nodes[position[c]];
                if(n.op==Op::Select && n.a==condition){yes[c]=n.b;no[c]=n.c;}}
            return Terminal(yes,depth+1) && Terminal(no,depth+1);
        }
        std::array<std::vector<std::pair<int,int>>,4> terms;
        for(unsigned c=0;c<4;++c)if(!Terms(position[c],terms[c]) || terms[c].size()!=4)return false;
        for(bool transpose:{false,true})for(const auto& seed:terms[0]){
            const auto& n=p.nodes[seed.first];if(!transpose && n.lane!=0)continue;
            // Four consecutive rows, either row-vector mul/mad or dp4 columns.
            for(unsigned base= n.row>=3?n.row-3:0;base<=n.row;++base){
                std::array<int,4> world{};bool okay=true;
                for(unsigned c=0;c<4 && okay;++c){std::array<bool,4> seen{};
                    for(const auto& term:terms[c]){const auto& b=p.nodes[term.first];
                        if(b.slot!=n.slot || b.row<base || b.row>=base+4){okay=false;break;}
                        const unsigned r=transpose?b.lane:b.row-base;
                        if(r>=4 || seen[r] || (transpose?b.row!=base+c:b.lane!=c) ||
                           (world[r] && world[r]!=term.second)){okay=false;break;}
                        seen[r]=true;world[r]=term.second;}
                }
                if(okay && RecordWorld(world,n.slot,base,transpose))return true;
            }
        }return false;
    }
};
inline Program Analyze(ID3D11DeviceChild* shader,Stage stage) {
    Program p;p.stage=stage;const bool vertex=stage==Stage::Vertex,domain=stage==Stage::Domain;
    const bool terminal=vertex || domain,pixel=stage==Stage::Pixel;
    std::vector<unsigned char> bytes;if(!GenshinDepthEvidence::ReadShaderBytes(shader,bytes))return p;
    p.diagnosticByteHash=GenshinDepthEvidence::ByteHash(bytes.data(),bytes.size());p.diagnosticByteSize=UINT(bytes.size());
    p.failure=ProgramFailure::Reflection;
    if(FAILED(D3DReflect(bytes.data(),bytes.size(),IID_PPV_ARGS(&p.reflection))))return p;
    auto* reflection=p.reflection.Get();D3D11_SHADER_DESC desc{};
    const UINT type=pixel?D3D11_SHVER_PIXEL_SHADER:domain?D3D11_SHVER_DOMAIN_SHADER:
        stage==Stage::Hull?D3D11_SHVER_HULL_SHADER:D3D11_SHVER_VERTEX_SHADER;
    p.failure=ProgramFailure::ShaderType;
    if(FAILED(reflection->GetDesc(&desc)) || D3D11_SHVER_GET_TYPE(desc.Version)!=type)return p;
    p.failure=ProgramFailure::ResourceBinding;
    for(UINT i=0;i<desc.BoundResources;++i){D3D11_SHADER_INPUT_BIND_DESC b{};
        if(FAILED(reflection->GetResourceBindingDesc(i,&b)))return p;
        p.resourceSlot=b.BindPoint;p.resourceType=UINT(b.Type);
        if(b.Type==D3D_SIT_CBUFFER){if(b.BindCount!=1 || b.BindPoint>=14)return p;
            if(!terminal)p.used[b.BindPoint]=true;}
        else if(!pixel || (b.Type!=D3D_SIT_TEXTURE && b.Type!=D3D_SIT_SAMPLER && b.Type!=D3D_SIT_TBUFFER &&
                          b.Type!=D3D_SIT_STRUCTURED && b.Type!=D3D_SIT_BYTEADDRESS)){p.failure=ProgramFailure::NonCbResource;return p;}
    }
    p.resourceSlot=p.resourceType=UINT_MAX;p.failure=ProgramFailure::PositionSignature;
    UINT position=UINT_MAX;
    for(UINT i=0;i<desc.OutputParameters;++i){D3D11_SIGNATURE_PARAMETER_DESC a{};
        if(FAILED(reflection->GetOutputParameterDesc(i,&a)))return p;
        if(terminal && a.SystemValueType==D3D_NAME_POSITION){
            if(position!=UINT_MAX || a.Mask!=15 || a.ComponentType!=D3D_REGISTER_COMPONENT_FLOAT32)return p;position=a.Register;
        }
        if(pixel && a.SystemValueType!=D3D_NAME_TARGET && a.SystemValueType!=D3D_NAME_UNDEFINED && a.SystemValueType!=D3D_NAME_COVERAGE)return p;
    }
    p.interfaceValid=true;
    if(!terminal){p.valid=true;p.failure=ProgramFailure::None;return p;}
    p.diagnosticTerminal=position;Parser parser{p};parser.output=position;parser.controlPoints=desc.cControlPoints;
    p.failure=ProgramFailure::InputSignature;
    for(UINT i=0;i<desc.InputParameters;++i){D3D11_SIGNATURE_PARAMETER_DESC a{};
        if(FAILED(reflection->GetInputParameterDesc(i,&a)) || a.Register>=32)return p;
        parser.inputMasks[a.Register]|=a.Mask;
        if(vertex && a.SystemValueType==D3D_NAME_INSTANCE_ID){
            if(parser.instance!=UINT_MAX || a.ComponentType!=D3D_REGISTER_COMPONENT_UINT32 || a.Mask!=1)return p;parser.instance=a.Register;
        }
        if(vertex && a.SemanticName && !_stricmp(a.SemanticName,"POSITION") && !a.SemanticIndex &&
           a.SystemValueType==D3D_NAME_UNDEFINED && a.ComponentType==D3D_REGISTER_COMPONENT_FLOAT32 && (a.Mask&7)==7){
            if(parser.input!=UINT_MAX)return p;parser.input=a.Register;
        }
    }
    if(domain)for(UINT i=0;i<desc.PatchConstantParameters;++i){p.failure=ProgramFailure::PatchSignature;D3D11_SIGNATURE_PARAMETER_DESC a{};
        if(FAILED(reflection->GetPatchConstantParameterDesc(i,&a)) || a.Register>=32)return p;parser.patchMasks[a.Register]|=a.Mask;
    }
    if(vertex && parser.input==UINT_MAX){p.failure=ProgramFailure::InputSignature;return p;}
    if(position==UINT_MAX){p.failure=ProgramFailure::PositionSignature;return p;}
    p.failure=ProgramFailure::Disassembly;
    ComPtr<ID3DBlob> assembly;
    if(FAILED(D3DDisassemble(bytes.data(),bytes.size(),0,nullptr,&assembly)) || !assembly || assembly->GetBufferSize()>512*1024)return p;
    p.failure=ProgramFailure::TerminalDataflow;
    p.valid=parser.Run(std::string(static_cast<const char*>(assembly->GetBufferPointer()),assembly->GetBufferSize()));
    if(p.valid)p.valid=parser.NumericDependencies();
    if(p.valid)p.failure=ProgramFailure::None;return p;
}
struct CachedShader {ComPtr<ID3D11DeviceChild> shader;std::shared_ptr<const Program> program;Stage stage=Stage::Vertex;};
inline std::mutex shaderMutex;inline std::array<CachedShader,256> shaderCache;
inline std::shared_ptr<const Program> Shader(ID3D11DeviceChild* shader,Stage stage) {
    if(!shader)return {};std::unique_lock lock(shaderMutex,std::try_to_lock);if(!lock.owns_lock())return {};
    CachedShader* slot=nullptr;
    for(auto& e:shaderCache){
        if(e.shader.Get()==shader && e.stage==stage)return e.program;
        if(!slot && !e.shader)slot=&e;
    }
    if(!slot){
        // Misses alone advance this cursor under shaderMutex. Bound reclamation
        // to eight slots, even when all Programs are held by writer evidence.
        static size_t recycleNext=0;
        constexpr size_t recycleBudget=8;
        for(size_t i=0;i<recycleBudget;++i){
            auto& e=shaderCache[recycleNext];recycleNext=(recycleNext+1)%shaderCache.size();
            // Draw/evidence (including Before/After pairing) must keep exactly
            // the same Program. Only the cache's sole root may be replaced;
            // invalid Programs are eligible under the same ownership rule.
            if(e.program.use_count()==1){slot=&e;break;}
        }
    }
    if(!slot)return {};
    // Analyze the actual shader under the original lock. Leave the old entry
    // intact if analysis/allocation throws; Capture retains its fail-closed catch.
    auto p=std::make_shared<Program>(Analyze(shader,stage));
    if(!p->valid)GenshinDepthEvidence::ObserveShader(shader,UINT(stage),UINT(p->failure),p->failedInstruction,
        p->failedDependency,p->failedOpcode,p->resourceSlot,p->resourceType);
    *slot={shader,p,stage};return p;
}
} // namespace detail
} // namespace GenshinDepthSemantic

namespace GenshinDepthSemantic {
namespace detail {
// CPU timestamps/source labels are diagnostic observations, never command keys
// or GPU-completion facts. Unknown covers a placeholder not yet queued/copied.
enum class PayloadOrigin : unsigned char { Unknown, CpuUpdate, DynamicShadow, DeferredCopy, CpuPortable };
struct Payload {
    GenshinDepthRetention::Token retention; // released after bytes
    Payload()=default;Payload(const Payload&)=delete;Payload& operator=(const Payload&)=delete;
    FramePipeline::CpuSnapshotBytes bytes;bool ready=false,failed=false;
    PayloadOrigin diagnosticOrigin=PayloadOrigin::Unknown;
    uint64_t diagnosticCreatedTick=GetTickCount64(),diagnosticSourceTick=0;
    // Phase20: original API interception return address only; never a command key.
    uintptr_t diagnosticUploadCaller=0;
};
struct Buffer {
    // Tokens precede owners; replacement assignment preserves release order.
    GenshinDepthRetention::Buffers<1> retention;
    Buffer()=default;Buffer(const Buffer&)=default;Buffer(Buffer&&)noexcept=default;
    Buffer& operator=(Buffer other)noexcept {this->~Buffer();new(this) Buffer(std::move(other));return *this;}
    ComPtr<ID3D11Buffer> object;D3D11_BUFFER_DESC desc{};uint64_t revision=0;
    bool mapped=false,mappingKnown=true;std::shared_ptr<Payload> payload;
    // A returned draw authorizes only this exact still-current CB revision for
    // deferred readback. Any write/loss detaches payload and this eligibility.
    std::weak_ptr<Payload> returnedPayload;uint64_t returnedRevision=0,returnedLoss=0;
};
struct Flight {
    // Tokens precede owners; replacement assignment preserves release order.
    GenshinDepthRetention::Buffers<28> retention;
    Flight()=default;Flight(const Flight&)=default;Flight(Flight&&)noexcept=default;
    Flight& operator=(Flight other)noexcept {this->~Flight();new(this) Flight(std::move(other));return *this;}
    ComPtr<ID3D11DeviceContext> context;ComPtr<ID3D11Query> event;
    std::array<ComPtr<ID3D11Buffer>,14> source{},staging{};
    std::array<std::shared_ptr<Payload>,14> destination{};
    bool queued=false,failed=false;
};
struct State {
    std::mutex mutex;ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    FramePipeline::InputObservationSession observation;
    DWORD thread=0;uint64_t deviceEpoch=0,configEpoch=0,serial=0,lossEpoch=0;
    static constexpr size_t BufferCapacity=128;
    std::array<Buffer,BufferCapacity> buffers{};std::array<Flight,3> flights{};
    // Non-owning index of the same retained CB slots, sorted by exact resource
    // interface address. Access holds mutex; admission is append-only until
    // BindOwner clears both tables. No payload/revision/eligibility is cached.
    std::array<Buffer*,BufferCapacity> bufferIndex{};size_t bufferCount=0;
    void ClearBuffers()noexcept {bufferCount=0;bufferIndex={};buffers={};}
};
inline std::shared_ptr<Payload> NewPayload(size_t size) {
    auto p=std::make_shared<Payload>();
    p->retention=GenshinDepthRetention::Payload(p.get(),size);
    if(!p->retention)return {};
    p->bytes=FramePipeline::CpuSnapshotBytes(size);
    return p;
}
inline State& StateStore(){static State s;return s;}
inline std::atomic<uint64_t> loss{1};
inline void Lost()noexcept {auto n=loss.load();while(n!=UINT64_MAX && !loss.compare_exchange_weak(n,n+1)){};}
// Called only under State::mutex, before any live numerical cache access.
// Detach live references; do NOT change frozen Draw payloads or Flight owners.
inline bool SyncLoss(State& s)noexcept {
    const auto observed=loss.load();if(observed==UINT64_MAX)return false;
    if(s.lossEpoch!=observed){
        for(auto& b:s.buffers)if(b.object){
            b.payload.reset();
            if(s.serial==UINT64_MAX){loss=UINT64_MAX;return false;}
            b.revision=++s.serial;b.mapped=false;
            // A missed Map may still be open on a dynamic buffer. Only an
            // observed Unmap can restore this certainty. Immutable/default
            // CPU-inaccessible CBs cannot legally have an open CPU mapping.
            b.mappingKnown=b.desc.Usage==D3D11_USAGE_IMMUTABLE ||
                (b.desc.Usage==D3D11_USAGE_DEFAULT && !b.desc.CPUAccessFlags);
        }
        s.lossEpoch=observed;
    }
    return s.lossEpoch==loss.load();
}
struct Access {
    State& s=StateStore();std::unique_lock<std::mutex> lock{s.mutex,std::try_to_lock};
    bool synchronized=false;
    Access(){if(!lock.owns_lock())Lost();else {
        // Stop new admission independently of old Draw/Flight/map custody.
        // Both stop and restart revoke live numeric versions; frozen readers
        // and mandatory application writeback keep their independent owners.
        const auto demand=GenshinLegacy::ReadNewInputObservationDemand();
        if(s.observation.Observe(demand)){
            if(!demand.Requested())for(auto& buffer:s.buffers){
                const auto& payload=buffer.payload;
                if(!payload || payload->ready || payload->failed)continue;
                bool queued=false;
                for(const auto& flight:s.flights)if(flight.queued)
                    for(const auto& destination:flight.destination)queued=queued || destination==payload;
                // No GPU work exists for this placeholder. Its frozen reader
                // must fail explicitly rather than await a copy we stopped.
                // Already queued payloads still resolve through their real EVENT.
                if(!queued)payload->failed=true;
            }
            Lost();
            static unsigned reports=0;
            if(reports++<32)try {
                LOG_INFO("F6 numeric input observation: pid={} thread={} new_inputs={} nr={} fg={} effects={} preview={} capture={} unknown={} loss={} old_flights_and_writeback_preserved=true",
                    GetCurrentProcessId(),GetCurrentThreadId(),s.observation.Requested(),demand.nr,demand.fg,
                    demand.effects,demand.preview,demand.capture,demand.unknown,loss.load());
            }catch(...){}
        }
        synchronized=SyncLoss(s);
    }}
    explicit operator bool()const noexcept{return lock.owns_lock() && synchronized &&
        s.lossEpoch==loss.load() && s.lossEpoch!=UINT64_MAX;}
};
inline thread_local unsigned internal=0;
struct Internal {Internal(){++internal;}~Internal(){--internal;}};
inline bool Supported(const D3D11_BUFFER_DESC& d)noexcept {
    return d.ByteWidth && d.ByteWidth<=65536 && !(d.ByteWidth%16) && d.BindFlags==D3D11_BIND_CONSTANT_BUFFER &&
        !d.MiscFlags && !d.StructureByteStride && (d.Usage==D3D11_USAGE_DEFAULT ||
        d.Usage==D3D11_USAGE_DYNAMIC || d.Usage==D3D11_USAGE_IMMUTABLE);
}
inline bool Context(const State& s,ID3D11DeviceContext* c)noexcept {
    return c && c==s.context.Get() && s.device && s.deviceEpoch && s.configEpoch &&
        // BindOwner retained and checked this exact immediate context.
        s.thread==GetCurrentThreadId() && SUCCEEDED(s.device->GetDeviceRemovedReason());
}
// At most eight address comparisons for the existing 128-slot capacity. An
// unknown resource is still canonicalized by QueryInterface in Find below.
inline size_t BufferPosition(const State& s,ID3D11Resource* resource)noexcept {
    const auto key=reinterpret_cast<uintptr_t>(resource);
    size_t first=0,last=s.bufferCount;
    while(first<last){
        const auto middle=first+(last-first)/2;
        const auto candidate=reinterpret_cast<uintptr_t>(static_cast<ID3D11Resource*>(s.bufferIndex[middle]->object.Get()));
        if(candidate<key)first=middle+1;else last=middle;
    }return first;
}
inline Buffer* Find(State& s,ID3D11Resource* resource,bool create=false) {
    if(!resource)return nullptr;
    // Exact retained interface hits need no repeated buffer QueryInterface.
    // Unknown/wrapped resource interfaces keep the original canonical lookup.
    auto position=BufferPosition(s,resource);
    if(position<s.bufferCount && s.bufferIndex[position]->object.Get()==resource)return s.bufferIndex[position];
    ComPtr<ID3D11Buffer> buffer;
    if(FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer))))return nullptr;
    position=BufferPosition(s,buffer.Get());
    if(position<s.bufferCount && s.bufferIndex[position]->object.Get()==buffer.Get())return s.bufferIndex[position];
    if(!create)return nullptr;
    D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);ComPtr<ID3D11Device> device;buffer->GetDevice(&device);
    if(device.Get()!=s.device.Get() || !Supported(desc))return nullptr;
    if(s.bufferCount==s.buffers.size())return nullptr;
    auto* empty=&s.buffers[s.bufferCount];
    if(!empty->retention.Add(buffer.Get()))return nullptr;
    empty->object=buffer;empty->desc=desc;empty->revision=++s.serial;
    // A returned public creation/upload/copy may already prove a whole CPU version.
    // Adopt it only while the exact resource still has that immutable image;
    // an intervening Map/Update/GPU/opaque write has already detached it.
    if(GenshinDepthPortable::enabled.load())if(auto image=GenshinDepthPortable::Read(buffer.Get())){
        if(image->Covers(0,desc.ByteWidth)){auto payload=NewPayload(desc.ByteWidth);
            if(payload&&image->CopyTo(payload->bytes.data(),0,desc.ByteWidth)){
                payload->ready=true;payload->diagnosticOrigin=PayloadOrigin::CpuPortable;
                payload->diagnosticSourceTick=GetTickCount64();empty->payload=std::move(payload);}}
    }
    // Publish only after the unchanged device/descriptor/retention admission.
    // Existing Buffer addresses and every frozen Draw/Flight owner stay stable.
    for(auto i=s.bufferCount;i>position;--i)s.bufferIndex[i]=s.bufferIndex[i-1];
    s.bufferIndex[position]=empty;++s.bufferCount;return empty;
}
inline bool BindOwner(State& s,ID3D11DeviceContext* c,uint64_t deviceEpoch,uint64_t configEpoch) {
    if(!c || !deviceEpoch || !configEpoch)return false;
    // Access already holds the semantic owner mutex. A retained D3D11 context
    // cannot change device/type between its Before/After/CB callbacks.
    ComPtr<ID3D11Device> queried;auto* device=s.device.Get();
    if(s.context.Get()!=c || !device){
        if(c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
        c->GetDevice(&queried);device=queried.Get();
    }
    if(!device || FAILED(device->GetDeviceRemovedReason()))return false;
    if(s.context.Get()!=c || s.deviceEpoch!=deviceEpoch || s.configEpoch!=configEpoch || s.thread!=GetCurrentThreadId()){
        // Do not release pending GPU resources on an ownership change. They
        // remain in their three physical slots until completion can be polled.
        Lost();s.ClearBuffers();s.context=c;s.device=device;s.deviceEpoch=deviceEpoch;s.configEpoch=configEpoch;s.thread=GetCurrentThreadId();
    }return SyncLoss(s);
}
inline bool DeferredEligible(const State& s,const Buffer& b)noexcept {
    return b.object && b.payload && !b.payload->ready && !b.payload->failed &&
        b.mappingKnown && !b.mapped && b.returnedRevision==b.revision &&
        b.returnedLoss==s.lossEpoch && s.lossEpoch==loss.load() && b.returnedPayload.lock()==b.payload;
}
inline bool Queued(const State& s,const std::shared_ptr<Payload>& payload)noexcept {
    for(const auto& flight:s.flights)if(flight.queued)for(const auto& p:flight.destination)if(p==payload)return true;
    return false;
}
inline bool QueueDeferredLocked(State& s,ID3D11DeviceContext* c,const Buffer* only=nullptr) {
    if(!Context(s,c))return false;
    // A pre-write freeze of an already-returned reader is old-work custody.
    // Present may not start a broad new collection while consumers are off.
    if(!only && !s.observation.Requested())return true;
    bool pending=false;
    if(only)pending=DeferredEligible(s,*only) && !Queued(s,only->payload);
    else for(const auto& b:s.buffers)pending=pending || (DeferredEligible(s,b) && !Queued(s,b.payload));
    if(!pending)return true;
    // No allocation/admission attempt when all physical flights are retained.
    // Pending owners remain untouched; a later Poll may retire their events.
    bool available=false;for(const auto& flight:s.flights)available=available || !flight.queued;
    if(!available)return false;
    // Present, a returned query End or a pre-write boundary only offers an opportunity.
    // Full query/native admission is mandatory; never close game queries or wait.
    GenshinQueryScope::PrivateWork permitted(c);if(!permitted)return false;
    for(auto& flight:s.flights){
        if(flight.queued)continue;
        Flight next;next.context=c;unsigned count=0;
        for(auto& b:s.buffers){
            if(count==14)break;
            if((only && &b!=only) || !DeferredEligible(s,b) || Queued(s,b.payload))continue;
            auto desc=b.desc;desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=desc.StructureByteStride=0;
            desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            if(FAILED(s.device->CreateBuffer(&desc,nullptr,&next.staging[count])))return false;
            if(!next.retention.Add(next.staging[count].Get()) || !next.retention.Add(b.object.Get()))return false;
            next.source[count]=b.object;next.destination[count]=b.payload;++count;
        }
        if(!count)break;
        D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};if(FAILED(s.device->CreateQuery(&q,&next.event)))return false;
        // Publish owners before any GPU submission; old revisions can then
        // change without altering their already ordered historical snapshot.
        flight=std::move(next);flight.queued=true;
        {Internal internalScope;for(unsigned i=0;i<count;++i){
            flight.destination[i]->diagnosticOrigin=PayloadOrigin::DeferredCopy;
            flight.destination[i]->diagnosticSourceTick=GetTickCount64();
            c->CopyResource(flight.staging[i].Get(),flight.source[i].Get());}
            c->End(flight.event.Get());}
        if(only)break; // One threatened revision, not a new broad CB scan.
    }
    return true;
}
inline void PollLocked(State& s,ID3D11DeviceContext* c) {
    Internal internalScope;
    for(auto& f:s.flights){if(!f.queued || f.failed || f.context.Get()!=c)continue;
        BOOL complete=FALSE;const auto hr=c->GetData(f.event.Get(),&complete,sizeof(complete),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if(hr==S_FALSE || (hr==S_OK && !complete))continue;
        if(FAILED(hr)){f.failed=true;for(auto& p:f.destination)if(p)p->failed=true;continue;}
        bool all=true;
        for(unsigned i=0;i<14;++i){auto& p=f.destination[i];if(!p || p->ready || p->failed)continue;
            D3D11_MAPPED_SUBRESOURCE mapped{};const auto map=c->Map(f.staging[i].Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
            if(map==DXGI_ERROR_WAS_STILL_DRAWING){all=false;continue;}
            if(FAILED(map) || !mapped.pData){p->failed=true;continue;}
            const bool copied=Copy(p->bytes.data(),mapped.pData,p->bytes.size());c->Unmap(f.staging[i].Get(),0);
            p->ready=copied;p->failed=!copied;
        }
        if(all)f={}; // EVENT completed: safe to recycle this physical slot.
    }
}
} // namespace detail
inline bool Internal()noexcept{return detail::internal!=0;}
// Frozen numerical inputs only. A returned writer never re-reads a game CB
// through Binding.buffer: that address is a diagnostic/equivalence label.
// Ready bytes live in Payload; pending GPU copies retain their actual source,
// staging and event in Flight until completion. Capture's resource/argument
// roots remain in Draw until the original same-call return is authenticated.
struct WriterProof {
    std::shared_ptr<const detail::Program> program;
    // Terminal first, then VS/HS upstream for DS; all used frozen payloads
    // participate in readiness/equivalence, although numeric slots are 0..13.
    std::array<Binding,42> binding{};std::array<uint64_t,42> revision{};
    std::array<std::shared_ptr<detail::Payload>,42> payload{};
    std::array<bool,42> used{};
    ComPtr<ID3D11InputLayout> positionLayout;UINT positionSlot=UINT_MAX;bool positionDefaultW=false;
    ComPtr<ID3D11DeviceContext> context;ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;ComPtr<ID3D11PixelShader> ps;
    D3D11_PRIMITIVE_TOPOLOGY topology=D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    D3D11_VIEWPORT viewport{};uint64_t deviceEpoch=0,configEpoch=0,loss=0,call=0;DWORD thread=0;
    D3D11_COMPARISON_FUNC compare=D3D11_COMPARISON_NEVER;
    UINT instances=1,instanceStart=0,argumentKind=0;
    bool indirectArgumentsUnknown=false;
    mutable bool returned=false;
};
struct Draw : WriterProof {
    // These roots belong to Capture/Returned, not numerical coverage storage.
    // Both tokens and actual owners precede base destruction on teardown.
    GenshinDepthRetention::Buffers<43> retention;
    std::array<ComPtr<ID3D11Buffer>,42> held{};
    GenshinDepthMapping::DrawArguments arguments{};
    Draw()=default;Draw(const Draw&)=default;Draw(Draw&&)noexcept=default;
    Draw& operator=(Draw other)noexcept {this->~Draw();new(this) Draw(std::move(other));return *this;}
};
struct WriterGroup {
    WriterProof proof;uint64_t multiplicity=0;
};
enum class CoverageStatus {Stored,Budget,Overflow,InvalidProof,Allocation};
// Equivalence is precisely the immutable input to MatrixFor/Resolve, not the
// invocation ID used for Before/After. Geometry XYZ is universally symbolic;
// every actual instance range and every read CB revision/range stays in the key.
inline bool Equivalent(const WriterProof& a,const WriterProof& b)noexcept {
    if(!a.returned || !b.returned || a.program!=b.program || a.context.Get()!=b.context.Get() ||
       a.vs.Get()!=b.vs.Get() || a.hs.Get()!=b.hs.Get() || a.ds.Get()!=b.ds.Get() || a.ps.Get()!=b.ps.Get() ||
       a.topology!=b.topology || a.deviceEpoch!=b.deviceEpoch || a.configEpoch!=b.configEpoch || a.loss!=b.loss ||
       a.thread!=b.thread || a.instances!=b.instances || a.instanceStart!=b.instanceStart ||
       a.indirectArgumentsUnknown!=b.indirectArgumentsUnknown || a.argumentKind!=b.argumentKind || a.compare!=b.compare ||
       std::memcmp(&a.viewport,&b.viewport,sizeof(a.viewport)) || a.revision!=b.revision || a.payload!=b.payload ||
       a.positionLayout.Get()!=b.positionLayout.Get() || a.positionSlot!=b.positionSlot ||
       a.positionDefaultW!=b.positionDefaultW)return false;
    if(a.used!=b.used)return false;
    for(UINT i=0;i<a.binding.size();++i)if(a.used[i] && (a.binding[i].buffer!=b.binding[i].buffer ||
       a.binding[i].first!=b.binding[i].first || a.binding[i].count!=b.binding[i].count))return false;
    return true;
}
// Copy/claim snapshots share immutable group storage. Appending detaches only
// when a reader holds the old version. Actual array capacity includes each
// compact proof and is charged once against the existing payload byte limit.
// No per-writer retention-registry entry, extra allocation or writer-count cap.
// Only adjacent exact equivalents merge; every actual returned draw is counted.
class WriterGroups {
    struct Storage {
        GenshinDepthRetention::Token charge;
        std::unique_ptr<WriterGroup[]> groups;
        size_t count=0,capacity=0;
    };
    std::shared_ptr<Storage> storage_;
    bool Writable(size_t required) {
        if(storage_ && storage_.use_count()==1 && required<=storage_->capacity)return true;
        const size_t old=storage_?storage_->count:0;
        size_t capacity=storage_?storage_->capacity:4;
        if(required>capacity){if(capacity>SIZE_MAX/2)return false;capacity*=2;}
        if(capacity<required || capacity>(GenshinDepthRetention::PayloadBudget-sizeof(Storage))/sizeof(WriterGroup))return false;
        auto next=std::make_shared<Storage>();
        next->charge=GenshinDepthRetention::Payload(next.get(),sizeof(Storage)+capacity*sizeof(WriterGroup));
        if(!next->charge)return false;
        next->groups=std::make_unique<WriterGroup[]>(capacity);next->capacity=capacity;next->count=old;
        for(size_t i=0;i<old;++i)next->groups[i]=storage_->groups[i];
        storage_=std::move(next);return true;
    }
public:
    bool empty()const noexcept {return !storage_ || !storage_->count;}
    size_t size()const noexcept {return storage_?storage_->count:0;}
    const WriterGroup* begin()const noexcept {return storage_?storage_->groups.get():nullptr;}
    const WriterGroup* end()const noexcept {return storage_?storage_->groups.get()+storage_->count:nullptr;}
    CoverageStatus Append(const std::shared_ptr<const Draw>& draw)noexcept {
        if(!draw || !draw->returned || !draw->program || !draw->program->valid)return CoverageStatus::InvalidProof;
        const bool merge=!empty() && Equivalent(storage_->groups[storage_->count-1].proof,*draw);
        if(merge && storage_->groups[storage_->count-1].multiplicity==UINT64_MAX)return CoverageStatus::Overflow;
        try{
            if(!Writable(size()+(merge?0:1)))return CoverageStatus::Budget;
            if(merge)++storage_->groups[storage_->count-1].multiplicity;
            else storage_->groups[storage_->count++]={static_cast<const WriterProof&>(*draw),1};
            return CoverageStatus::Stored;
        }catch(...){return CoverageStatus::Allocation;}
    }
};
namespace detail {
inline bool SameArguments(const GenshinDepthMapping::DrawArguments& a,
    const GenshinDepthMapping::DrawArguments& b)noexcept {
    return a.context==b.context && a.thread==b.thread && a.kind==b.kind && a.count==b.count &&
        a.start==b.start && a.base==b.base && a.instances==b.instances && a.instanceStart==b.instanceStart &&
        a.observationCall==b.observationCall && a.indirectArgumentsUnknown==b.indirectArgumentsUnknown &&
        a.indirectBuffer.Get()==b.indirectBuffer.Get() && a.indirectIdentity.Get()==b.indirectIdentity.Get() &&
        a.indirectOffset==b.indirectOffset;
}
inline bool UnknownInstances(const WriterProof& draw)noexcept {return draw.indirectArgumentsUnknown;}
inline bool CaptureArguments(Draw& draw,const GenshinDepthMapping::DrawArguments& a,ID3D11DeviceContext* c)noexcept {
    if(a.context!=c || a.thread!=GetCurrentThreadId() || !a.observationCall)return false;
    if(a.kind==6){
        if(!a.indirectArgumentsUnknown || !a.indirectBuffer || !a.indirectIdentity || (a.indirectOffset&3u) ||
           a.count || a.start || a.base || a.instances || a.instanceStart)return false;
        draw.instances=draw.instanceStart=0; // explicitly unknown, never instance zero
    }else{
        if(a.indirectArgumentsUnknown || a.kind<1 || a.kind>4 || !a.count)return false;
        if(a.kind==3 || a.kind==4){
            if(!a.instances || a.instances>256 || uint64_t(a.instanceStart)+a.instances>UINT_MAX)return false;
            draw.instances=a.instances;draw.instanceStart=a.instanceStart;
        }else if(a.instances!=1)return false;
    }
    if(!draw.retention.Add(a.indirectBuffer.Get()))return false;
    draw.arguments=a;draw.argumentKind=a.kind;draw.indirectArgumentsUnknown=a.indirectArgumentsUnknown;
    draw.call=a.observationCall;return true;
}
}
// Ownership comes only from the existing authenticated raster callbacks.
// Establishing a numerical collector owner grants no semantic fact.
inline void ObserveOwner(ID3D11DeviceContext* c,uint64_t deviceEpoch,uint64_t configEpoch)noexcept {
    if(Internal())return;try{detail::Access a;if(a)detail::BindOwner(a.s,c,deviceEpoch,configEpoch);}
    catch(...){detail::Lost();}
}
inline void Poll(ID3D11DeviceContext* c)noexcept {
    if(Internal())return;try{detail::Access a;if(a && detail::Context(a.s,c)){
        detail::PollLocked(a.s,c);detail::QueueDeferredLocked(a.s,c);
    }}catch(...){detail::Lost();}
}
inline void Invalidate(ID3D11DeviceContext* c,ID3D11Resource* r,bool mapped)noexcept {
    if(Internal())return;try{detail::Access a;if(!a)return;
        if(!r){detail::Lost();detail::SyncLoss(a.s);return;}
        auto* b=detail::Find(a.s,r);
        // An observed write names its allocation. Foreign-context texture/IA
        // work cannot modify these retained, exclusively CONSTANT_BUFFER
        // allocations. Do not turn that unrelated operation into a missed Map
        // of every dynamic CB; unknown whole-context writes still lose above.
        if(!b)return;
        {
            // Invalidate is BEFORE the original write/Map. A returned writer
            // can still own this exact Pending revision; Present is too late
            // after this reset detaches it. Freeze only that live revision in
            // the existing ordered flight, never read the replacement bytes.
            // Ready CPU snapshots and already queued snapshots need no work.
            if(b->payload && b->payload.use_count()>1 && detail::DeferredEligible(a.s,*b) &&
               !detail::Queued(a.s,b->payload))detail::QueueDeferredLocked(a.s,c,b);
            // Admission/full-flight failure never delays or suppresses the
            // game's write. Old payloads stay unproved; no success is invented.
            b->payload.reset();b->revision=++a.s.serial;
            if(mapped){b->mapped=true;b->mappingKnown=true;}}
        // A real foreign write to this exact CB revokes only its live version.
        // Its CPU mapping state is unknown until an owning, returned Unmap.
        // Frozen older payloads and unrelated CB revisions remain independent.
        if(c && a.s.context && c!=a.s.context.Get())b->mappingKnown=false;
    }catch(...){detail::Lost();}
}
inline void Unmapped(ID3D11DeviceContext* c,ID3D11Resource* r)noexcept {
    if(Internal())return;try{detail::Access a;if(!a)return;
        if(!r){detail::Lost();detail::SyncLoss(a.s);return;}
        if(auto* b=detail::Find(a.s,r)){b->payload.reset();b->revision=++a.s.serial;
            b->mapped=false;b->mappingKnown=c && c==a.s.context.Get();}
        // The exact resource is known even for a non-owning context. Never
        // manufacture quiescence/payloads there or revoke unrelated CBs.
    }catch(...){detail::Lost();}
}
namespace detail {
// Redirect only successful whole dynamic-CB WRITE_DISCARD maps. The application
// writes ordinary aligned CPU memory; Unmap copies it to the driver's write-only
// pointer. We NEVER read driver write-combined memory (D3D11 Map contract).
// Lifetime is independent of semantic owner/loss/reset: once a pointer has been
// returned to the application its writeback must survive every numerical loss.
struct CpuMap {
    // Tokens precede owners; replacement assignment preserves release order.
    GenshinDepthRetention::Buffers<1> retention;
    CpuMap()=default;CpuMap(const CpuMap&)=default;CpuMap(CpuMap&&)noexcept=default;
    CpuMap& operator=(CpuMap other)noexcept {this->~CpuMap();new(this) CpuMap(std::move(other));return *this;}
    ComPtr<ID3D11DeviceContext> context;ComPtr<ID3D11Buffer> resource;
    // This payload remains unpublished and not Ready until the exact Unmap.
    // The same aligned storage becomes the returned immutable CPU snapshot.
    std::shared_ptr<Payload> shadow;void* driver=nullptr;UINT bytes=0,sub=0;
    DWORD thread=0;uint64_t revision=0,loss=0;
};
using CpuMaps=FramePipeline::MappedResourceJournal<CpuMap,32>;
inline CpuMaps& MapStore(){static CpuMaps maps;return maps;}
inline CpuMap TakeMap(ID3D11DeviceContext* c,ID3D11Resource* r,UINT sub) {
    auto& store=MapStore();
    if(!r || !store.HasEntries())return {};
    auto out=store.Take({reinterpret_cast<uintptr_t>(c),reinterpret_cast<uintptr_t>(r),sub});
    if(out.shadow || !store.HasEntries())return out;
    // Unknown/wrapped interfaces retain canonical lookup. Exact retained
    // interface hits and an empty journal need no COM call.
    ComPtr<ID3D11Buffer> buffer;
    if(FAILED(r->QueryInterface(IID_PPV_ARGS(&buffer))) || buffer.Get()==r)return {};
    return store.Take({reinterpret_cast<uintptr_t>(c),reinterpret_cast<uintptr_t>(buffer.Get()),sub});
}
}
inline bool Mapped(ID3D11DeviceContext* c,ID3D11Resource* r,UINT sub,D3D11_MAP type,
    UINT flags,D3D11_MAPPED_SUBRESOURCE* mapped,HRESULT hr)noexcept {
    if(Internal() || FAILED(hr) || !mapped || !mapped->pData)return false;
    try{detail::Access a;if(!a || !a.s.observation.Requested() || !detail::Context(a.s,c))return false;auto* b=detail::Find(a.s,r,true);if(!b)return false;
        b->payload.reset();b->revision=++a.s.serial;b->mapped=true;b->mappingKnown=true;
        // Partial/NO_OVERWRITE updates cannot supply a full-buffer CPU contract.
        // Refuse numerical capture while forwarding their original mapped data.
        if(sub || flags || type!=D3D11_MAP_WRITE_DISCARD || b->desc.Usage!=D3D11_USAGE_DYNAMIC ||
           b->desc.CPUAccessFlags!=D3D11_CPU_ACCESS_WRITE)return false;
        detail::CpuMap next;if(!next.retention.Add(b->object.Get()))return false;
        next.context=c;next.resource=b->object;next.driver=mapped->pData;
        next.bytes=b->desc.ByteWidth;next.sub=sub;next.thread=GetCurrentThreadId();
        next.revision=b->revision;next.loss=a.s.lossEpoch;
        next.shadow=detail::NewPayload(next.bytes);if(!next.shadow)return false;
        auto* data=next.shadow->bytes.data();
        // This proves only pure-CB isolation. The journal must retain writeback
        // through owner/loss changes; mapped CBs still cannot feed Capture.
        if(detail::MapStore().TryInsert({reinterpret_cast<uintptr_t>(c),
            reinterpret_cast<uintptr_t>(b->object.Get()),sub},std::move(next))){
            mapped->pData=data;return true;}
        // Capacity/allocation/contention leaves the original Map untouched.
    }catch(...){detail::Lost();}
    return false;
}
class Submission final {
    GenshinDepthRetention::Buffers<1> retention_;
    ComPtr<ID3D11DeviceContext> context_;ComPtr<ID3D11Buffer> target_;
    std::shared_ptr<detail::Payload> payload_;uint64_t revision_=0,loss_=0;DWORD thread_=0;bool unmap_=false;
    detail::CpuMap mapped_; // held through the actual original Unmap
public:
    Submission()=default;Submission(const Submission&)=default;Submission(Submission&&)noexcept=default;
    Submission& operator=(Submission other)noexcept {this->~Submission();new(this) Submission(std::move(other));return *this;}
    static Submission Update(ID3D11DeviceContext* c,ID3D11Resource* r,UINT sub,const D3D11_BOX* box,
        const void* data,UINT flags=0,uintptr_t diagnosticUploadCaller=0)noexcept {
        Submission out;if(Internal())return out;
        try{detail::Access a;if(!a || !a.s.observation.Requested() || !detail::Context(a.s,c))return out;
            // Invalidate already revoked any actually tracked destination.
            // A foreign upload cannot admit another context's CB into this
            // fixed owner table or publish numerical bytes on its behalf.
            auto* b=detail::Find(a.s,r,true);if(!b)return out;
            b->payload.reset();b->revision=++a.s.serial;
            if(!b->mappingKnown || b->mapped || sub || box || flags || b->desc.Usage!=D3D11_USAGE_DEFAULT)return out;
            auto payload=detail::NewPayload(b->desc.ByteWidth);if(!payload)return out;
            if(!detail::Copy(payload->bytes.data(),data,payload->bytes.size()))return out;
            payload->diagnosticOrigin=detail::PayloadOrigin::CpuUpdate;payload->diagnosticSourceTick=GetTickCount64();
            payload->diagnosticUploadCaller=diagnosticUploadCaller;
            if(!out.retention_.Add(b->object.Get()))return out;
            payload->ready=true;out.context_=c;out.target_=b->object;out.payload_=std::move(payload);
            out.revision_=b->revision;out.loss_=a.s.lossEpoch;out.thread_=GetCurrentThreadId();
        }catch(...){detail::Lost();}return out;
    }
    // Bracket the unchanged original Unmap. Driver writeback is mandatory even
    // after a semantic reset, loss, wrong thread, or owner/config transition.
    static Submission Unmap(ID3D11DeviceContext* c,ID3D11Resource* r,UINT sub,
        uintptr_t diagnosticUploadCaller=0)noexcept {
        Submission out;if(Internal())return out;
        try{
            out.mapped_=detail::TakeMap(c,r,sub);
            if(out.mapped_.shadow)std::memcpy(out.mapped_.driver,out.mapped_.shadow->bytes.data(),out.mapped_.bytes);
            detail::Access a;if(!a || sub)return out;auto* b=detail::Find(a.s,r);if(!b)return out;
            const bool owner=detail::Context(a.s,c);
            const bool exact=owner && a.s.observation.Requested() && b->mappingKnown && b->mapped && out.mapped_.shadow &&
                out.mapped_.thread==GetCurrentThreadId() && out.mapped_.revision==b->revision &&
                out.mapped_.loss==a.s.lossEpoch && a.s.lossEpoch==detail::loss.load();
            b->payload.reset();b->revision=++a.s.serial;
            if(!owner){b->mappingKnown=false;return out;}
            if(!out.retention_.Add(b->object.Get()))return out;
            out.context_=c;out.target_=b->object;out.revision_=b->revision;out.loss_=a.s.lossEpoch;
            out.thread_=GetCurrentThreadId();out.unmap_=true;
            if(exact){auto payload=out.mapped_.shadow;
                payload->diagnosticOrigin=detail::PayloadOrigin::DynamicShadow;payload->diagnosticSourceTick=GetTickCount64();
                payload->diagnosticUploadCaller=diagnosticUploadCaller;
                payload->ready=true;out.payload_=std::move(payload);}
        }catch(...){detail::Lost();}return out;
    }
    // A successful receipt belongs only to this original Update/Unmap and its
    // exact pure-CB revision. Frozen older Raster payloads/private CBs do not
    // read the rewritten application buffer. This never makes an older Pending
    // payload Ready, nor grants an exemption to arbitrary buffers/GPU copies.
    bool Returned()noexcept {
        if(!target_)return false;
        bool frozen=false;
        try{detail::Access a;if(!a){target_.Reset();return false;}auto* b=detail::Find(a.s,target_.Get());
            if(b && detail::Context(a.s,context_.Get()) && thread_==GetCurrentThreadId() &&
               loss_==a.s.lossEpoch && loss_==detail::loss.load() && revision_==b->revision &&
               (unmap_ || (b->mappingKnown && !b->mapped))){
                // Only the returned subresource-zero Unmap restores certainty
                // after SyncLoss. Owner/thread/loss/revision above still match
                // this exact original call; no Map state is inferred at Capture.
                // A lost/non-exact shadow stays absent and needs the unchanged
                // same-draw deferred-copy/query/GPU-completion proof for bytes.
                if(unmap_){b->mapped=false;b->mappingKnown=true;}
                if(a.s.observation.Requested())b->payload=payload_;else b->payload.reset();
                // Both factories populate payload_ only from the complete
                // original CPU upload/shadow; a non-exact Unmap has no payload.
                frozen=a.s.observation.Requested() && payload_ && payload_->ready && !payload_->failed &&
                    payload_->bytes.size()==b->desc.ByteWidth;
            }
        }catch(...){detail::Lost();}target_.Reset();return frozen;
    }
};
inline std::shared_ptr<const Draw> Capture(ID3D11DeviceContext* c,ID3D11VertexShader* vs,ID3D11HullShader* hs,ID3D11DomainShader* ds,ID3D11PixelShader* ps,D3D11_PRIMITIVE_TOPOLOGY topology,
    const std::array<Binding,42>& bindings,const D3D11_VIEWPORT& viewport,D3D11_COMPARISON_FUNC compare,
    uint64_t deviceEpoch,uint64_t configEpoch,CaptureFailureObservation* observation)noexcept {
    if(observation)*observation={};
    if(Internal())return {};
    const auto refuse=[&](CaptureFailure reason,UINT slot=UINT_MAX,const Binding* binding=nullptr,
                         uint64_t revision=0,UINT state=0)->std::shared_ptr<const Draw>{
        if(observation){
            const auto& draw=GenshinDepthMapping::CurrentDraw();
            observation->reason=reason;observation->tick=GetTickCount64();observation->call=draw.observationCall;
            observation->thread=GetCurrentThreadId();observation->kind=draw.kind;observation->count=draw.count;
            observation->instances=draw.instances;observation->instanceStart=draw.instanceStart;
            observation->indirect=draw.indirectArgumentsUnknown;observation->slot=slot;
            observation->revision=revision;observation->loss=detail::loss.load();observation->state=state;
            if(binding){observation->buffer=reinterpret_cast<uintptr_t>(binding->buffer);
                observation->first=binding->first;observation->constants=binding->count;}
        }return {};
    };
    try{
        using Stage=detail::Stage;
        if(bool(hs)!=bool(ds))return refuse(CaptureFailure::StageLinkage);
        const auto programRefusal=[&](CaptureFailure reason,const std::shared_ptr<const detail::Program>& program,
            ID3D11DeviceChild* shader,Stage stage)->std::shared_ptr<const Draw>{
            auto result=refuse(reason);
            if(observation){observation->shader=reinterpret_cast<uintptr_t>(shader);observation->stage=UINT(stage);
                observation->programFailure=UINT(program?program->failure:detail::ProgramFailure::CacheUnavailable);
                if(program){observation->terminalInstruction=program->failedInstruction;observation->terminalOpcode=program->failedOpcode;
                    observation->dependency=program->failedDependency;observation->resourceSlot=program->resourceSlot;observation->resourceType=program->resourceType;}}
            return result;
        };
        const auto vertex=detail::Shader(vs,ds?Stage::ControlVertex:Stage::Vertex);
        const auto hull=hs?detail::Shader(hs,Stage::Hull):nullptr;
        const auto program=ds?detail::Shader(ds,Stage::Domain):vertex;
        if(!vertex || !vertex->interfaceValid)return programRefusal(CaptureFailure::Vertex,vertex,vs,ds?Stage::ControlVertex:Stage::Vertex);
        if(hs && (!hull || !hull->interfaceValid))return programRefusal(CaptureFailure::Hull,hull,hs,Stage::Hull);
        auto* terminal=ds?static_cast<ID3D11DeviceChild*>(ds):static_cast<ID3D11DeviceChild*>(vs);
        if(!program || !program->interfaceValid)return programRefusal(ds?CaptureFailure::Domain:CaptureFailure::Vertex,program,terminal,ds?Stage::Domain:Stage::Vertex);
        if(!program->valid)return programRefusal(CaptureFailure::TerminalDataflow,program,terminal,ds?Stage::Domain:Stage::Vertex);
        if(ds && !GenshinDepthEvidence::TessellationSignature(vertex->reflection.Get(),hull->reflection.Get(),program->reflection.Get(),topology))
            return refuse(CaptureFailure::StageLinkage);
        const auto pixel=ps?detail::Shader(ps,Stage::Pixel):nullptr;
        if(ps && (!pixel || !pixel->valid))return programRefusal(CaptureFailure::Pixel,pixel,ps,Stage::Pixel);
        detail::Access a;if(!a || !a.s.observation.Requested() || !detail::BindOwner(a.s,c,deviceEpoch,configEpoch))return refuse(CaptureFailure::Owner);
        auto out=std::make_shared<Draw>();
        if(!detail::CaptureArguments(*out,GenshinDepthMapping::CurrentDraw(),c))return refuse(CaptureFailure::Arguments);
        if(!detail::UnknownInstances(*out))detail::PollLocked(a.s,c);
        out->program=program;
        if(program->needsPositionW && !ds){
            // D3D11.3 8.19.2/8.20/19.1.3.3: a missing float-format A
            // defaults to 1 even for OOB fetches, but an unbound VB returns 0.
            // The float4 VS signature alone supplies neither fact.
            // Existing create-time layout metadata is immutable; no VB scan,
            // new capture/readback or per-draw allocation is needed here.
            c->IAGetInputLayout(&out->positionLayout);
            const auto layout=GenshinDepthPortable::ReadLayout(out->positionLayout.Get());
            if(layout && GenshinDepthPortable::SameDevice(c,out->positionLayout.Get())){
                const D3D11_INPUT_ELEMENT_DESC* position=nullptr;bool duplicate=false;
                for(UINT i=0;i<layout->count;++i){const auto& e=layout->elements[i];
                    if(e.SemanticName && !_stricmp(e.SemanticName,"POSITION") && !e.SemanticIndex){
                        if(position)duplicate=true;position=&e;}}
                if(position && !duplicate && position->InputSlot<32 &&
                   position->InputSlotClass==D3D11_INPUT_PER_VERTEX_DATA && !position->InstanceDataStepRate){
                    switch(position->Format){
                    case DXGI_FORMAT_R32G32B32_FLOAT:case DXGI_FORMAT_R32G32_FLOAT:case DXGI_FORMAT_R32_FLOAT:
                    case DXGI_FORMAT_R16G16_FLOAT:case DXGI_FORMAT_R16_FLOAT:{
                        ComPtr<ID3D11Buffer> bound;UINT stride=0,offset=0;
                        c->IAGetVertexBuffers(position->InputSlot,1,&bound,&stride,&offset);
                        if(bound && GenshinDepthPortable::SameDevice(c,bound.Get())){
                            out->positionSlot=position->InputSlot;out->positionDefaultW=true;
                        }
                        break;
                    }
                    default:break; // Explicit A, integer/reinterpreted/unknown formats never default float W.
                    }
                }
            }
        }
        for(UINT i=0;i<14;++i){
            out->binding[i]=bindings[(ds?28:0)+i];out->used[i]=program->used[i];
            if(ds){out->binding[14+i]=bindings[i];out->used[14+i]=vertex->used[i];
                out->binding[28+i]=bindings[14+i];out->used[28+i]=hull->used[i];}
        }
        out->context=c;out->vs=vs;out->hs=hs;out->ds=ds;out->ps=ps;out->topology=topology;out->viewport=viewport;out->compare=compare;out->deviceEpoch=deviceEpoch;
        out->configEpoch=configEpoch;out->loss=a.s.lossEpoch;out->thread=GetCurrentThreadId();
        for(unsigned i=0;i<out->used.size();++i)if(out->used[i]){
            const auto& bind=out->binding[i];
            if(observation)observation->stage=i<14?UINT(program->stage):i<28?UINT(Stage::ControlVertex):UINT(Stage::Hull);
            auto* b=detail::Find(a.s,bind.buffer,true);
            const auto bufferRefusal=[&](CaptureFailure reason)->std::shared_ptr<const Draw>{
                const UINT observed=(b?1u:0u)|(b && b->mappingKnown?2u:0u)|(b && b->mapped?4u:0u)|
                    (b && b->payload?8u:0u)|(b && b->payload && b->payload->ready?16u:0u)|
                    (b && b->payload && b->payload->failed?32u:0u);
                auto result=refuse(reason,i,&bind,b?b->revision:0,observed);
                GenshinDepthEvidence::ObserveShader(terminal,UINT(program->stage),0x10000u+UINT(reason),
                    program->failedInstruction,program->failedDependency,program->failedOpcode,i,UINT_MAX,
                    b?b->revision:0,a.s.lossEpoch,observed,bind.buffer,bind.first,bind.count,UINT(a.s.bufferCount));
                return result;
            };
            if(!b || !b->mappingKnown || b->mapped || !bind.count || uint64_t(bind.first)*16>=b->desc.ByteWidth)
                return bufferRefusal(CaptureFailure::BufferState);
            // Kind6 never initiates a deferred CB readback. Only a ready,
            // immutable snapshot of this exact same-call revision is admissible.
            if(detail::UnknownInstances(*out) && (!b->payload || !b->payload->ready))
                return bufferRefusal(CaptureFailure::IndirectPending);
            if(!b->payload)b->payload=detail::NewPayload(b->desc.ByteWidth);
            if(!b->payload)return bufferRefusal(CaptureFailure::Allocation);
            if(b->payload->failed)return bufferRefusal(CaptureFailure::PayloadFailed);
            if(!out->retention.Add(b->object.Get()))return bufferRefusal(CaptureFailure::Retention);
            out->held[i]=b->object;out->revision[i]=b->revision;out->payload[i]=b->payload;
        }if(out->loss!=detail::loss.load())return refuse(CaptureFailure::Loss);
        if(observation)observation->reason=CaptureFailure::None;return out;
    }catch(...){detail::Lost();return refuse(CaptureFailure::Exception);}
}
inline bool Returned(ID3D11DeviceContext* c,const std::shared_ptr<const Draw>& draw)noexcept {
    if(!draw || Internal())return false;try{
        detail::Access a;if(!a || draw->returned || !detail::Context(a.s,c) || draw->context.Get()!=c || draw->thread!=GetCurrentThreadId() ||
            draw->loss!=a.s.lossEpoch || draw->loss!=detail::loss.load() ||
            draw->call!=GenshinDepthMapping::CurrentDraw().observationCall ||
            !detail::SameArguments(draw->arguments,GenshinDepthMapping::CurrentDraw()))return false;
        if(draw->positionLayout){ComPtr<ID3D11InputLayout> actual;c->IAGetInputLayout(&actual);
            if(actual.Get()!=draw->positionLayout.Get())return false;}
        if(draw->positionDefaultW){
            if(draw->positionSlot>=32)return false;
            ComPtr<ID3D11Buffer> bound;UINT stride=0,offset=0;
            c->IAGetVertexBuffers(draw->positionSlot,1,&bound,&stride,&offset);
            if(!bound || !GenshinDepthPortable::SameDevice(c,bound.Get()))return false;
            // No vertex bytes or buffer identity are used numerically: the
            // default holds for every bound buffer/address, including OOB.
            // Do not retain a large VB in immutable numerical writer coverage.
        }
        if(!detail::UnknownInstances(*draw))detail::PollLocked(a.s,c);
        for(unsigned i=0;i<draw->payload.size();++i)if(draw->payload[i]){
            auto* b=detail::Find(a.s,draw->held[i].Get());
            if(!b || !b->mappingKnown || b->mapped || b->revision!=draw->revision[i] || b->payload!=draw->payload[i] ||
                draw->payload[i]->failed || (detail::UnknownInstances(*draw) && !draw->payload[i]->ready))return false;
        }
        if(draw->loss!=detail::loss.load())return false;
        for(unsigned i=0;i<draw->payload.size();++i)if(draw->payload[i] && !detail::UnknownInstances(*draw)){
            auto* b=detail::Find(a.s,draw->held[i].Get());
            b->returnedPayload=draw->payload[i];b->returnedRevision=draw->revision[i];b->returnedLoss=draw->loss;
        }
        // API return and readiness are separate facts. Pending payloads remain
        // pending; Resolve cannot certify DeviceZ until every byte is ready.
        draw->returned=true;return true;
    }catch(...){detail::Lost();return false;}
}
namespace detail {
struct Affine {std::array<double,4> v{};bool valid=false,unresolvedInstance=false;}; // input XYZ plus constant
inline bool Scalar(const WriterProof& draw,UINT slot,UINT row,UINT lane,float& out)noexcept {
    if(slot>=14 || lane>=4 || !draw.payload[slot] || !draw.payload[slot]->ready || draw.payload[slot]->failed)return false;
    const auto& b=draw.binding[slot];const uint64_t offset=(uint64_t(b.first)+row)*16+lane*4;
    const auto& data=draw.payload[slot]->bytes;
    if(row>=b.count || offset>data.size() || 4>data.size()-offset)return false;
    std::memcpy(&out,data.data()+static_cast<size_t>(offset),4);return Finite(out);
}
inline bool Integer(const WriterProof& draw,int id,UINT instance,uint32_t& out,unsigned depth=0)noexcept {
    if(!draw.program || id<=0 || static_cast<size_t>(id)>=draw.program->nodes.size() || depth>64)return false;
    const auto n=draw.program->nodes[id];
    if(UnknownInstances(draw) && n.instanceDependent)return false;
    if(n.op==Op::Input && n.slot==2 && n.lane==0){if(UnknownInstances(draw))return false;out=instance;return true;}
    if(n.op==Op::Constant){std::memcpy(&out,&n.value,4);return true;}
    if(n.op==Op::IntegerNegate){uint32_t value=0;if(!Integer(draw,n.a,instance,value,depth+1))return false;
        out=0u-value;return true;}
    if(n.op==Op::Buffer){float value=0;
        // CB integer lanes are raw uint32 bits, never converted from float.
        if(n.slot>=14 || n.lane>=4 || !draw.payload[n.slot] || !draw.payload[n.slot]->ready || draw.payload[n.slot]->failed)return false;
        const auto& b=draw.binding[n.slot];const auto& bytes=draw.payload[n.slot]->bytes;
        const uint64_t offset=(uint64_t(b.first)+n.row)*16+n.lane*4;
        if(n.row>=b.count || offset>bytes.size() || 4>bytes.size()-offset)return false;
        std::memcpy(&out,bytes.data()+static_cast<size_t>(offset),4);return true;
    }
    if(n.op==Op::Select){uint32_t yes=0,no=0;
        if(!Integer(draw,n.b,instance,yes,depth+1) || !Integer(draw,n.c,instance,no,depth+1) || yes!=no)return false;
        out=yes;return true;
    }
    if(n.op!=Op::IntegerAdd && n.op!=Op::IntegerMul && n.op!=Op::IntegerAnd &&
       n.op!=Op::IntegerOr && n.op!=Op::IntegerShiftLeft)return false;uint32_t a=0,b=0;
    if(!Integer(draw,n.a,instance,a,depth+1) || !Integer(draw,n.b,instance,b,depth+1))return false;
    // Only the low 32-bit result is accepted; an out-of-binding final row
    // refuses. imul high-result consumers and unknown integer ops are absent.
    switch(n.op){
    case Op::IntegerAdd:out=a+b;break;case Op::IntegerMul:out=a*b;break;
    case Op::IntegerAnd:out=a&b;break;case Op::IntegerOr:out=a|b;break;
    case Op::IntegerShiftLeft:out=a<<(b&31u);break;default:return false;
    }return true;
}
inline Affine Evaluate(const WriterProof& draw,int id,std::vector<Affine>& cache,std::vector<bool>& visited,unsigned depth=0,UINT instance=0) {
    if(!draw.program || id<=0 || depth>128 || static_cast<size_t>(id)>=cache.size() ||
       static_cast<size_t>(id)>=visited.size() || static_cast<size_t>(id)>=draw.program->nodes.size())
        return Affine{{},false,UnknownInstances(draw)};
    if(visited[id])return cache[id];visited[id]=true;
    cache[id].unresolvedInstance=UnknownInstances(draw); // fail closed even for a malformed cycle
    const auto& n=draw.program->nodes[id];Affine out;
    // Dependency is structural: zero*unknown, cancellation, saturate and
    // opaque operations cannot manufacture instance-independent W.
    if(UnknownInstances(draw) && n.instanceDependent){out.unresolvedInstance=true;cache[id]=out;return out;}
    if(n.op==Op::Constant){out.v[3]=n.value;out.valid=true;}
    else if(n.op==Op::Input && n.slot==0){
        if(n.lane<3){out.v[n.lane]=1;out.valid=true;}
        else if(n.lane==3 && draw.positionDefaultW && draw.positionLayout){out.v[3]=1;out.valid=true;}
    }
    else if(n.op==Op::Buffer){float value=0;if(Scalar(draw,n.slot,n.row,n.lane,value)){out.v[3]=value;out.valid=true;}}
    else if(n.op==Op::IndexedBuffer){uint32_t row=0;float value=0;
        if(Integer(draw,n.a,instance,row) && row<4096 && n.row<4096-row && Scalar(draw,n.slot,row+n.row,n.lane,value)){
            out.v[3]=value;out.valid=true;}
        else if(UnknownInstances(draw))out.unresolvedInstance=true;
    }
    else if(n.op==Op::Select){
        const auto yes=Evaluate(draw,n.b,cache,visited,depth+1,instance),no=Evaluate(draw,n.c,cache,visited,depth+1,instance);
        out.unresolvedInstance=yes.unresolvedInstance || no.unresolvedInstance;
        if(!out.unresolvedInstance && yes.valid && no.valid && yes.v==no.v)out=yes;
    }
    else if(n.op==Op::Saturate || n.op==Op::Absolute){
        const auto a=Evaluate(draw,n.a,cache,visited,depth+1,instance);
        out.unresolvedInstance=a.unresolvedInstance;
        if(!out.unresolvedInstance && a.valid && a.v[0]==0 && a.v[1]==0 && a.v[2]==0){
            out.v[3]=n.op==Op::Absolute?std::abs(a.v[3]):std::clamp(a.v[3],0.0,1.0);out.valid=true;}
    }
    else if(n.op==Op::Add || n.op==Op::Mul){
        const auto a=Evaluate(draw,n.a,cache,visited,depth+1,instance),b=Evaluate(draw,n.b,cache,visited,depth+1,instance);
        const auto zero=[](const Affine& x)noexcept {return x.valid && x.v[0]==0 && x.v[1]==0 && x.v[2]==0 && x.v[3]==0;};
        // A numerically proved zero transform coefficient removes an upstream
        // opaque XYZ term from the homogeneous component. Unknown/nonzero
        // coefficients never do. This is the same finite affine algebra used
        // for raw vertex inputs; it grants no unknown matrix or XYZ value.
        out.unresolvedInstance=a.unresolvedInstance || b.unresolvedInstance;
        if(out.unresolvedInstance){out.valid=false;}
        else if(n.op==Op::Mul && (zero(a) || zero(b))){out.valid=true;}
        else if(a.valid && b.valid){
            if(n.op==Op::Add){for(unsigned i=0;i<4;++i)out.v[i]=a.v[i]+b.v[i];out.valid=true;}
            else {const bool ac=a.v[0]==0 && a.v[1]==0 && a.v[2]==0,bc=b.v[0]==0 && b.v[1]==0 && b.v[2]==0;
                if(ac || bc){for(unsigned i=0;i<4;++i)out.v[i]=ac?a.v[3]*b.v[i]:b.v[3]*a.v[i];out.valid=true;}}
        }
    }
    for(double x:out.v){uint64_t bits=0;std::memcpy(&bits,&x,8);if((bits&0x7ff0000000000000ull)==0x7ff0000000000000ull)out.valid=false;}
    // Universal unknown-instance W proof: every reachable unresolved value
    // (including Opaque with no visible instance edge) is sticky. Known-instance
    // evaluation retains its existing zero-coefficient reduction semantics.
    if(UnknownInstances(draw) && !out.valid)out.unresolvedInstance=true;
    cache[id]=out;return out;
}
// R821 diagnostic output only. No caller-supplied qualification and no W gate
// relaxation. All coefficients/operands below describe the CPU affine attempt
// over the frozen same-draw payload, never an observed per-vertex GPU W.
enum class WFailureReason : unsigned char { None, AffineUnproved, NotExactOne };
enum class WOperandState : unsigned char {
    Finite, NonFinite, MissingPayload, PendingPayload, FailedPayload, BindingRange, InvalidIndex
};
struct WOperand {
    int node=0;UINT slot=UINT_MAX,row=UINT_MAX,lane=UINT_MAX,first=0,count=0;
    uint32_t bits=0;WOperandState state=WOperandState::BindingRange;
    int indexNode=0;UINT rowBase=0,indexValue=0;bool indexResolved=false,bindingRangeOk=false;
    uint64_t byteOffset=0,payloadBytes=0;
};
struct WFailure {
    WFailureReason reason=WFailureReason::None;
    UINT instance=0;int worldNode=0;Affine affine{};
    std::array<WOperand,8> operands{};unsigned operandCount=0,nodesVisited=0;
    bool graphTruncated=false,operandsTruncated=false,opaqueInputsUnavailable=false;
};
inline const char* WFailureText(WFailureReason r)noexcept {
    return r==WFailureReason::AffineUnproved?"AFFINE_W_UNPROVED":
        r==WFailureReason::NotExactOne?"AFFINE_W_NOT_EXACT_ONE":"NO_W_FAILURE";
}
inline WOperand ObserveWOperand(const WriterProof& draw,int id,UINT instance)noexcept {
    WOperand out;out.node=id;const auto& n=draw.program->nodes[id];
    out.slot=n.slot;out.row=n.row;out.rowBase=n.row;out.lane=n.lane;
    // Frozen payload and binding size only; never query live buffer/context.
    if(out.slot<14){const auto& b=draw.binding[out.slot];out.first=b.first;out.count=b.count;
        if(draw.payload[out.slot])out.payloadBytes=draw.payload[out.slot]->bytes.size();}
    if(n.op==Op::IndexedBuffer){uint32_t row=0;out.indexNode=n.a;
        if(!Integer(draw,n.a,instance,row)){
            out.row=UINT_MAX;out.state=WOperandState::InvalidIndex;return out;}
        out.indexResolved=true;out.indexValue=row;
        if(row>=4096 || n.row>=4096-row){out.row=UINT_MAX;out.state=WOperandState::InvalidIndex;return out;}
        out.row=row+n.row;
    }
    if(out.slot>=14 || out.lane>=4)return out;
    const auto& b=draw.binding[out.slot];out.bindingRangeOk=out.row<b.count;
    out.byteOffset=(uint64_t(b.first)+out.row)*16+out.lane*4;
    const auto& payload=draw.payload[out.slot];
    if(!payload){out.state=WOperandState::MissingPayload;return out;}
    if(payload->failed){out.state=WOperandState::FailedPayload;return out;}
    if(!payload->ready){out.state=WOperandState::PendingPayload;return out;}
    const uint64_t offset=(uint64_t(b.first)+out.row)*16+out.lane*4;
    if(out.row>=b.count || offset>payload->bytes.size() || 4>payload->bytes.size()-offset)return out;
    float value=0;std::memcpy(&value,payload->bytes.data()+static_cast<size_t>(offset),4);
    std::memcpy(&out.bits,&value,4);out.state=Finite(value)?WOperandState::Finite:WOperandState::NonFinite;
    return out;
}
inline void ObserveWFailure(const WriterProof& draw,int root,UINT instance,const Affine& affine,WFailure& out)noexcept {
    out={};out.reason=affine.valid?WFailureReason::NotExactOne:WFailureReason::AffineUnproved;
    out.instance=instance;out.worldNode=root;out.affine=affine;
    // Existing Opaque nodes store an instruction ordinal in a, NOT operand
    // edges. Never follow it as a node ID or claim its inputs were observed.
    std::array<int,64> pending{},visited{};unsigned count=1,seen=0;pending[0]=root;
    while(count && seen<visited.size()){
        const int id=pending[--count];
        if(id<=0 || static_cast<size_t>(id)>=draw.program->nodes.size()){out.graphTruncated=true;continue;}
        bool duplicate=false;for(unsigned i=0;i<seen;++i)if(visited[i]==id){duplicate=true;break;}
        if(duplicate)continue;visited[seen++]=id;
        const auto& n=draw.program->nodes[id];
        if(n.op==Op::Buffer || n.op==Op::IndexedBuffer){
            if(out.operandCount<out.operands.size())out.operands[out.operandCount++]=ObserveWOperand(draw,id,instance);
            else out.operandsTruncated=true;
        }
        if(n.op==Op::Opaque){out.opaqueInputsUnavailable=true;continue;}
        auto push=[&](int child)noexcept {if(!child)return;
            if(count<pending.size())pending[count++]=child;else out.graphTruncated=true;};
        if(n.op==Op::Select){push(n.c);push(n.b);push(n.a);}
        else if(BinaryNode(n.op)){push(n.b);push(n.a);}
        else if(Unary(n.op))push(n.a);
    }
    out.nodesVisited=seen;if(count)out.graphTruncated=true;
}
inline bool MatrixFor(const WriterProof& draw,Matrix& matrix,WFailure* failure=nullptr) {
    if(failure)*failure={};
    // Returned checked coverage/revision/loss BEFORE issuing the ordered copy.
    // A later state swap/write may revoke current content, but cannot alter
    // these already copied historical bytes. Owner/retirement is rechecked by
    // Resolve/Receipt; do not compare this frozen draw to a later loss epoch.
    if(!draw.returned || !draw.program || !draw.program->valid)return false;
    const auto& p=*draw.program;
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c){float v=0;
        if(!Scalar(draw,p.matrixSlot,p.matrixRow+(p.transpose?c:r),p.transpose?r:c,v))return false;matrix[r*4+c]=v;}
    // The terminal, shared world vector may contain opaque XYZ skinning or
    // other arithmetic. The proved actual camera VP is sufficient for Z/W;
    // numeric XYZ reconstruction is unnecessary. W still MUST be proved 1.
    std::vector<Affine> cache(p.nodes.size());std::vector<bool> visited(p.nodes.size());
    if(p.worlds.empty())return false;
    for(const auto& world:p.worlds)for(int id:world)if(!id)return false;
    const bool unknown=UnknownInstances(draw);
    if(unknown){
        if(draw.argumentKind!=6 || draw.instances || draw.instanceStart)return false;
    }else if(draw.argumentKind==6 || !draw.instances || draw.instances>256 ||
             uint64_t(draw.instanceStart)+draw.instances>UINT_MAX)return false;
    // One symbolic proof for kind6, not an enumeration or an instance0 sample.
    // Integer/Evaluate refuse every unresolved instance dependency in this mode.
    for(UINT ordinal=0;ordinal<(unknown?1u:draw.instances);++ordinal){
        const UINT instance=unknown?UINT_MAX:ordinal;
        std::fill(cache.begin(),cache.end(),Affine{});std::fill(visited.begin(),visited.end(),false);
        // D3D11 SV_InstanceID is the draw-local ordinal [0, InstanceCount).
        // StartInstanceLocation offsets IA instance-data fetching, not this
        // system value (D3D11.3 spec 8.4.2 / 8.6.2). Any shader iadd of a CB
        // base is already retained in Integer's DAG and must execute once.
        // Preserve raw instanceStart and all original identity/range gates.
        for(const auto& world:p.worlds){
            const auto w=Evaluate(draw,world[3],cache,visited,0,instance);
            if(!w.valid || w.v[0]!=0 || w.v[1]!=0 || w.v[2]!=0 || w.v[3]!=1){
                if(failure)ObserveWFailure(draw,world[3],instance,w,*failure);
                return false;
            }
        }
    }
    return true;
}
inline bool SameMatrix(const Matrix& a,const Matrix& b)noexcept {
    for(unsigned i=0;i<16;++i)if(!Finite(a[i]) || !Finite(b[i]) || !GenshinCameraHistory::Near(a[i],b[i],2.e-5f))return false;return true;
}
// Prove M = V * P from actual GPU terminal-matrix bytes. P is a finite
// canonical perspective with observed lens/endpoints, V a rigid affine view.
// M's W column determines Vz; orthogonal X/Y residuals determine the actual
// off-axis/jitter terms. No engine pose, guessed projection, previous token,
// texture dimensions or depth comparison can manufacture this factorization.
inline bool RasterProjection(const Matrix& m,const GenshinCameraHistory::Sample& camera,Matrix& projection)noexcept {
    const auto& p=camera.projectionJittered;
    if(camera.actual.provenance!=il2cpp_callsite::CameraProvenance::RenderObservation ||
       !GenshinCameraHistory::Finite(m) || !GenshinCameraHistory::Finite(p))return false;
    for(unsigned i:{1u,2u,3u,4u,6u,7u,12u,13u,15u})if(p[i]!=0)return false;
    if(p[11]!=-1 || p[0]==0 || p[5]==0 || p[10]==0 || p[10]==-1)return false;
    const auto nearRaster=[](double a,double b)noexcept {
        return GenshinCameraHistory::Near(static_cast<float>(a),static_cast<float>(b),2.e-5f);
    };
    std::array<double,3> z{},x{},y{};double zz=0,zw=0,xz=0,yz=0;
    for(unsigned r=0;r<3;++r){z[r]=-double(m[r*4+3]);zz+=z[r]*z[r];
        zw+=z[r]*double(m[r*4+2]);xz+=z[r]*double(m[r*4]);yz+=z[r]*double(m[r*4+1]);}
    if(!nearRaster(zz,1))return false;
    const double actualA=zw/zz,actualB=double(m[14])+double(m[15])*actualA;
    if(!nearRaster(actualA,p[10]) || !nearRaster(actualB,p[14]))return false;
    // Validate the observed Z coefficients directly in every actual GPU row.
    // Re-inverting a fitted float VP for its far endpoint is ill-conditioned
    // (A nearRaster zero/-1, cancellation in translated B). Keep the authenticated
    // lens coefficients only after that actual-row/reconstruction proof.
    const double a=p[10],b=p[14],jx=xz/zz,jy=yz/zz;
    const double atZero=b/a,atOne=b/(1+a);
    if(!Finite(static_cast<float>(atZero)) || !Finite(static_cast<float>(atOne)) ||
       atZero<=0 || atOne<=0 || atZero==atOne ||
       !nearRaster((std::min)(atZero,atOne),camera.nearPlane) ||
       !nearRaster((std::max)(atZero,atOne),camera.farPlane))return false;
    Matrix view{};view[15]=1;double xx=0,yy=0,xy=0,zx=0,zy=0;
    for(unsigned r=0;r<4;++r){
        const double vz=-double(m[r*4+3]);
        view[r*4]=static_cast<float>((double(m[r*4])-jx*vz)/p[0]);
        view[r*4+1]=static_cast<float>((double(m[r*4+1])-jy*vz)/p[5]);
        view[r*4+2]=static_cast<float>(vz);
        if(r<3){x[r]=view[r*4];y[r]=view[r*4+1];xx+=x[r]*x[r];yy+=y[r]*y[r];
            xy+=x[r]*y[r];zx+=z[r]*x[r];zy+=z[r]*y[r];}
        // A canonical perspective cannot contain oblique Z or an affine
        // scale hidden in W. Include the translated fourth row explicitly.
        if(!nearRaster(m[r*4+2],a*vz+(r==3?b:0)))return false;
    }
    if(!nearRaster(xx,1) || !nearRaster(yy,1) || !nearRaster(xy,0) || !nearRaster(zx,0) || !nearRaster(zy,0))return false;
    const auto determinant=[](const Matrix& v)noexcept {
        return double(v[0])*(double(v[5])*v[10]-double(v[6])*v[9])-
            double(v[1])*(double(v[4])*v[10]-double(v[6])*v[8])+
            double(v[2])*(double(v[4])*v[9]-double(v[5])*v[8]);
    };
    const double orientation=determinant(camera.viewToWorld);
    if(!nearRaster(std::fabs(orientation),1) || !nearRaster(determinant(view),orientation))return false;
    projection={};projection[0]=p[0];projection[5]=p[5];projection[11]=-1;
    projection[8]=static_cast<float>(jx);projection[9]=static_cast<float>(jy);
    projection[10]=static_cast<float>(a);projection[14]=static_cast<float>(b);
    return SameMatrix(m,GenshinCameraHistory::Multiply(view,projection));
}
// Called only while Resolve holds the existing semantic mutex. Never reset:
// two category bits per numeric generation bound the entire process to 768
// receipts, including repeated Resolve and owner/config/generation restarts.
inline void ObserveSemanticJoin(const WriterProof& draw,const GenshinLegacy::NativeMrt::Frame& frame,
    const GenshinCameraHistory::Sample& camera,const Matrix& matrix,const Matrix& expected,
    bool evaluated,bool extracted,bool matched,Reason reason)noexcept {
    try{
        if(!frame.generation || frame.generation>384)return;
        static std::array<unsigned char,384> reported{};
        auto& seen=reported[static_cast<size_t>(frame.generation-1)];
        const bool firstExtracted=extracted && !(seen&1),firstRefusal=!matched && !(seen&2);
        if(!firstExtracted && !firstRefusal)return;
        // Reserve before formatting/logging: even a failed diagnostic cannot
        // retry indefinitely or escape into Resolve's semantic return path.
        seen|=static_cast<unsigned char>((firstExtracted?1:0)|(firstRefusal?2:0));
        std::ostringstream cb,actualBits,expectedBits,actualValues,expectedValues;
        actualBits<<std::hex;expectedBits<<std::hex;
        actualValues.precision(std::numeric_limits<float>::max_digits10);
        expectedValues.precision(std::numeric_limits<float>::max_digits10);
        for(unsigned i=0;i<16;++i){uint32_t a=0,e=0;
            std::memcpy(&a,&matrix[i],4);std::memcpy(&e,&expected[i],4);
            if(i){actualBits<<',';expectedBits<<',';actualValues<<',';expectedValues<<',';}
            actualBits<<a;expectedBits<<e;actualValues<<matrix[i];expectedValues<<expected[i];
        }
        for(unsigned i=0;i<14;++i)if(draw.program && draw.program->used[i]){
            const auto& b=draw.binding[i];const auto& payload=draw.payload[i];
            const auto origin=payload?payload->diagnosticOrigin:PayloadOrigin::Unknown;
            const char* source=origin==PayloadOrigin::CpuPortable?"CPU_PORTABLE_WHOLE":origin==PayloadOrigin::CpuUpdate?"CPU_UPDATE_WHOLE":
                origin==PayloadOrigin::DynamicShadow?"DYNAMIC_WRITE_DISCARD_SHADOW":
                origin==PayloadOrigin::DeferredCopy?"DEFERRED_COPY_QUEUE":"UNKNOWN";
            // Resolve image metadata only after the existing bounded category reservation.
            // Resolve-time unpinned image only: unload/address reuse may differ from upload time.
            // No upload-time module lifetime or engine-command authority is established.
            const auto caller=payload?payload->diagnosticUploadCaller:0;
            std::array<char,MAX_PATH> callerPath{};const char* callerModule="UNKNOWN";uintptr_t callerBase=0;
            MEMORY_BASIC_INFORMATION callerInfo{};
            if(caller && VirtualQuery(reinterpret_cast<const void*>(caller),&callerInfo,sizeof(callerInfo))==sizeof(callerInfo) &&
               callerInfo.Type==MEM_IMAGE && callerInfo.AllocationBase){
                const auto length=GetModuleFileNameA(static_cast<HMODULE>(callerInfo.AllocationBase),
                    callerPath.data(),static_cast<DWORD>(callerPath.size()));
                if(length && length<callerPath.size()){
                    callerBase=reinterpret_cast<uintptr_t>(callerInfo.AllocationBase);callerModule=callerPath.data();
                    for(const char* p=callerPath.data();*p;++p)if(*p=='\\' || *p=='/')callerModule=p+1;
                }
            }
            cb<<"[slot="<<i<<" object="<<reinterpret_cast<uintptr_t>(draw.binding[i].buffer)
                <<" binding_object="<<reinterpret_cast<uintptr_t>(b.buffer)<<" first="<<b.first<<" count="<<b.count
                <<" revision="<<draw.revision[i]<<" payload="<<bool(payload)
                <<" ready="<<bool(payload && payload->ready)<<" failed="<<bool(payload && payload->failed)
                <<" bytes="<<(payload?payload->bytes.size():0)<<" source="<<source
                <<" created_tick="<<(payload?payload->diagnosticCreatedTick:0)
                <<" source_tick="<<(payload?payload->diagnosticSourceTick:0)
                <<" caller="<<caller<<" callerSource="<<(caller?"ORIGINAL_API_INTERCEPTION_RETURN_ADDRESS":"UNKNOWN")
                <<" callerModuleAtResolve="<<callerModule<<" callerRvaAtResolve=";
            if(callerBase)cb<<"0x"<<std::hex<<(caller-callerBase)<<std::dec;else cb<<"UNKNOWN";
            cb<<" callerImageObservation=RESOLVE_TIME_UNPINNED]";
        }
        const auto& owner=camera.owner;const auto* program=draw.program.get();
        LOG_INFO("Phase19 semantic join: pid={} generation={} source_frame={} device={} context={} device_epoch={} owner_epoch={} config_epoch={} instance={} thread={} render_context={} token_tick={} native_draw_serial={} camera_source_frame={} camera_generation={} camera_history_epoch={} call={} vs={} draw_context={} draw_thread={} draw_device_epoch={} draw_config_epoch={} draw_loss={} returned={} vs_hash_fnv1a64={:016X} vs_bytes={} terminal_position_register={} matrix_slot={} matrix_row={} transpose={} instances={} instance_start={} first_extracted={} first_refusal={} matrix_evaluated={} extracted={} match_evaluated={} match={} stage_reason={} actual={} expected={} actual_f32_bits={} expected_f32_bits={} cb={} missing_tick=UNKNOWN actual_when_unextracted=PARTIAL_OR_UNAVAILABLE cpu_timestamp_observation_only=true command_identity=UNKNOWN no_authority=true",
            GetCurrentProcessId(),frame.generation,frame.frame,frame.device,frame.context,frame.deviceEpoch,frame.ownerEpoch,
            frame.configEpoch,frame.instance,frame.thread,frame.renderContext,frame.tokenTick,frame.drawSerial,
            owner.sourceFrame,owner.generation,owner.historyEpoch,draw.call,reinterpret_cast<uintptr_t>(draw.vs.Get()),
            reinterpret_cast<uintptr_t>(draw.context.Get()),draw.thread,draw.deviceEpoch,draw.configEpoch,draw.loss,draw.returned,
            program?program->diagnosticByteHash:0,program?program->diagnosticByteSize:0,
            program?program->diagnosticTerminal:UINT_MAX,program?program->matrixSlot:UINT_MAX,
            program?program->matrixRow:UINT_MAX,program && program->transpose,draw.instances,draw.instanceStart,
            firstExtracted,firstRefusal,evaluated,extracted,extracted,matched,ReasonText(reason),
            actualValues.str(),expectedValues.str(),actualBits.str(),expectedBits.str(),cb.str());
    }catch(...){/* Diagnostics must never change the original semantic outcome. */}
}
} // namespace detail
inline Result ResolveBasis(const WriterGroups& writers,float actualClear,
    const GenshinLegacy::NativeMrt::Frame& frame,const GenshinFsr::Parameters& p,
    const GenshinCameraHistory::Sample& camera,const GenshinGuides::SrOutputGrid* outputGrid,bool preparedRaster)noexcept {
    Result out;try{
        if(writers.empty())return out;
        out.reason=Reason::WrongOwner;
        const auto& owner=camera.owner;
        if(!camera.valid || writers.empty() || !detail::Finite(actualClear) || owner.context!=frame.context ||
            owner.device!=frame.device || owner.deviceEpoch!=frame.deviceEpoch || owner.producerEpoch!=frame.ownerEpoch ||
            owner.configEpoch!=frame.configEpoch || owner.instance!=frame.instance || owner.generation!=frame.generation ||
            owner.sourceFrame!=frame.frame || owner.thread!=frame.thread || owner.thread!=GetCurrentThreadId() ||
            camera.actual.instance!=frame.instance || camera.actual.sourceFrame!=frame.frame ||
            camera.actual.renderGeneration!=frame.generation || camera.actual.jitterX!=frame.rawJitterX ||
            camera.actual.jitterY!=frame.rawJitterY || p.instance!=frame.instance || p.generation!=frame.generation ||
            p.sourceFrame!=frame.frame || p.renderWidth!=frame.renderWidth || p.renderHeight!=frame.renderHeight ||
            p.outputWidth!=frame.outputWidth || p.outputHeight!=frame.outputHeight)return out;
        out.reason=Reason::ClearMismatch;
        if(!preparedRaster && actualClear!=(camera.reversed?0.0f:1.0f))return out;
        // Event data belongs to these frozen draws, not the current bindings.
        // Hold the same lock through payload reads: Poll publishes bytes only
        // here/under this mutex, never via racy flags or a caller ready bool.
        auto* context=reinterpret_cast<ID3D11DeviceContext*>(frame.context);
        out.reason=Reason::Busy;detail::Access access;if(!access)return out;
        out.reason=Reason::WrongOwner;
        if(!detail::Context(access.s,context) || access.s.deviceEpoch!=frame.deviceEpoch ||
           access.s.configEpoch!=frame.configEpoch)return out;
        detail::PollLocked(access.s,context);
        Matrix view;if(!GenshinCameraHistory::Inverse(camera.viewToWorld,view))return out;
        const auto gpuJittered=GenshinCameraHistory::Multiply(view,camera.projectionJittered);
        uint64_t covered=0;Matrix sourceMatrix{},sourceProjection{};bool sourceKnown=false;
        const bool raster=preparedRaster && camera.actual.provenance==il2cpp_callsite::CameraProvenance::RenderObservation;
        for(const auto& group:writers){const auto* draw=&group.proof;Matrix matrix{};out.reason=Reason::WrongWriter;
            if(!group.multiplicity || covered>UINT64_MAX-group.multiplicity)return out;covered+=group.multiplicity;
            if(!draw || draw->context.Get()!=context || draw->thread!=owner.thread || draw->deviceEpoch!=frame.deviceEpoch ||
               draw->configEpoch!=frame.configEpoch)return out;
            for(const auto& payload:draw->payload)if(payload){
                if(payload->failed){out.reason=Reason::PayloadFailed;
                    detail::ObserveSemanticJoin(*draw,frame,camera,matrix,gpuJittered,false,false,false,out.reason);return out;}
                if(!payload->ready){out.reason=Reason::PayloadPending;
                    detail::ObserveSemanticJoin(*draw,frame,camera,matrix,gpuJittered,false,false,false,out.reason);return out;}
            }
            out.reason=Reason::ShaderOrMatrix;
            static unsigned wReports=0;detail::WFailure wFailure;
            const bool extracted=detail::MatrixFor(*draw,matrix,wReports<8?&wFailure:nullptr);
            bool matched=extracted && detail::SameMatrix(matrix,gpuJittered);
            if(raster){
                // One actual source VP for the complete selected interval,
                // across all returns/instances/writer multiplicities. The
                // existing exact observed-camera Resolve below is unchanged.
                matched=extracted && (sourceKnown?detail::SameMatrix(matrix,sourceMatrix):
                    detail::RasterProjection(matrix,camera,sourceProjection));
                if(matched && !sourceKnown){sourceMatrix=matrix;sourceKnown=true;}
            }
            detail::ObserveSemanticJoin(*draw,frame,camera,matrix,raster&&sourceKnown?sourceMatrix:gpuJittered,true,extracted,matched,out.reason);
            if(!matched){
                // Access holds the existing semantic mutex. Separate cap from
                // R811 so earlier matrix mismatches cannot spend the W budget.
                if(wFailure.reason!=detail::WFailureReason::None && wReports<8){++wReports;
                    LOG_INFO("R821 W refusal: pid={} frame={} generation={} context={} thread={} device_epoch={} config_epoch={} call={} vs={} ps={} matrix_slot={} matrix_row={} instances={} instance_start={} failed_instance={} reason={} world_node={} affine_valid={} affine_xyz_const={},{},{},{} nodes={} graph_truncated={} operands_truncated={} opaque_inputs_unavailable={} actual_vertex_W=UNKNOWN ia_default_w={} ia_layout={} no_authority=true",
                        GetCurrentProcessId(),frame.frame,frame.generation,reinterpret_cast<uintptr_t>(draw->context.Get()),draw->thread,
                        draw->deviceEpoch,draw->configEpoch,draw->call,reinterpret_cast<uintptr_t>(draw->vs.Get()),reinterpret_cast<uintptr_t>(draw->ps.Get()),
                        draw->program->matrixSlot,draw->program->matrixRow,draw->instances,draw->instanceStart,wFailure.instance,
                        detail::WFailureText(wFailure.reason),wFailure.worldNode,wFailure.affine.valid,
                        wFailure.affine.v[0],wFailure.affine.v[1],wFailure.affine.v[2],wFailure.affine.v[3],wFailure.nodesVisited,
                        wFailure.graphTruncated,wFailure.operandsTruncated,wFailure.opaqueInputsUnavailable,
                        draw->positionDefaultW,reinterpret_cast<uintptr_t>(draw->positionLayout.Get()));
                    for(unsigned i=0;i<wFailure.operandCount;++i){const auto& operand=wFailure.operands[i];
                        LOG_INFO("R822 W CB address: pid={} frame={} generation={} call={} sv_instance_id={} node={} slot={} row={} lane={} first={} count={} state={} raw_bits={:08x} index_node={} index_resolved={} index_value={} row_base={} binding_range_ok={} byte_offset={} payload_bytes={} buffer={} revision={} source=frozen_same_draw_CB actual_vertex_W=UNKNOWN no_authority=true",
                            GetCurrentProcessId(),frame.frame,frame.generation,draw->call,wFailure.instance,operand.node,
                            operand.slot,operand.row,operand.lane,operand.first,operand.count,static_cast<unsigned>(operand.state),operand.bits,
                            operand.indexNode,operand.indexResolved,operand.indexValue,operand.rowBase,operand.bindingRangeOk,
                            operand.byteOffset,operand.payloadBytes,operand.slot<14?reinterpret_cast<uintptr_t>(draw->binding[operand.slot].buffer):0,
                            operand.slot<14?draw->revision[operand.slot]:0);
                    }
                }
                static unsigned reports=0;if(reports<8){++reports;
                    LOG_INFO("R811 matrix refusal: pid={} generation={} writer={} extracted={} matrix_slot={} matrix_row={} actual={},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{} expected={},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{} no_authority=true",
                        GetCurrentProcessId(),frame.generation,reinterpret_cast<uintptr_t>(draw->vs.Get()),extracted,draw->program?draw->program->matrixSlot:UINT_MAX,draw->program?draw->program->matrixRow:UINT_MAX,
                        matrix[0],matrix[1],matrix[2],matrix[3],matrix[4],matrix[5],matrix[6],matrix[7],matrix[8],matrix[9],matrix[10],matrix[11],matrix[12],matrix[13],matrix[14],matrix[15],
                        gpuJittered[0],gpuJittered[1],gpuJittered[2],gpuJittered[3],gpuJittered[4],gpuJittered[5],gpuJittered[6],gpuJittered[7],gpuJittered[8],gpuJittered[9],gpuJittered[10],gpuJittered[11],gpuJittered[12],gpuJittered[13],gpuJittered[14],gpuJittered[15]);
                }return out;
            }
            // Compare corroborates the proved camera convention; it never
            // supplies that convention when projection/writer proof is absent.
            out.reason=Reason::CompareMismatch;
            const bool writerReversed=raster?sourceProjection[14]/(1+sourceProjection[10])<sourceProjection[14]/sourceProjection[10]:camera.reversed;
            if(writerReversed?(draw->compare!=D3D11_COMPARISON_GREATER && draw->compare!=D3D11_COMPARISON_GREATER_EQUAL):
                (draw->compare!=D3D11_COMPARISON_LESS && draw->compare!=D3D11_COMPARISON_LESS_EQUAL))return out;
            const auto& v=draw->viewport;
            out.reason=Reason::ViewportMismatch;
            if(v.TopLeftX!=0 || v.TopLeftY!=0 || v.Width!=frame.renderWidth || v.Height!=frame.renderHeight ||
               v.MinDepth!=0 || v.MaxDepth!=1)return out;
        }
        // For a native source derive the texture's direction from its actual
        // proved projection endpoints, never by relabelling camera.reversed.
        const bool reversed=raster?sourceProjection[14]/(1+sourceProjection[10])<sourceProjection[14]/sourceProjection[10]:camera.reversed;
        if(actualClear!=(reversed?0.0f:1.0f)){out.reason=Reason::ClearMismatch;return out;}
        out.deviceZ=true;out.reversed=reversed;out.clear=actualClear;out.writers=covered;out.groups=writers.size();
        if(raster){out.rasterBasis=true;out.rasterWorldToClip=sourceMatrix;out.rasterProjection=sourceProjection;}
        // ConfigureSetter has an independently observed noTAA reference.
        // RenderObservation has only actual Pj: the writer proof above plus
        // the accepted output grid below define its phase without asserting
        // a raw/noTAA jitter value. NaN marks that unavailable measurement.
        float pixelX=std::numeric_limits<float>::quiet_NaN(),pixelY=pixelX;
        if(camera.actual.provenance==il2cpp_callsite::CameraProvenance::ConfigureSetter) {
            const auto& j=camera.projectionJittered;const auto& n=camera.projection;
            out.reason=Reason::NonUniformProjectionPhase;
            unsigned pivot=0;for(unsigned r=1;r<4;++r)if(std::fabs(j[r*4+3])>std::fabs(j[pivot*4+3]))pivot=r;
            if(!detail::Finite(j[pivot*4+3]) || j[pivot*4+3]==0)return out;
            const float dx=(j[pivot*4]-n[pivot*4])/j[pivot*4+3],dy=(j[pivot*4+1]-n[pivot*4+1])/j[pivot*4+3];
            for(unsigned r=0;r<4;++r)if(!GenshinCameraHistory::Near(j[r*4]-n[r*4],dx*j[r*4+3],1.e-5f) ||
                !GenshinCameraHistory::Near(j[r*4+1]-n[r*4+1],dy*j[r*4+3],1.e-5f) ||
                !GenshinCameraHistory::Near(j[r*4+2],n[r*4+2],1.e-5f) || !GenshinCameraHistory::Near(j[r*4+3],n[r*4+3],1.e-5f))return out;
            pixelX=.5f*dx*frame.renderWidth;pixelY=-.5f*dy*frame.renderHeight;
        } else if(camera.actual.provenance!=il2cpp_callsite::CameraProvenance::RenderObservation)return out;
        const auto& controls=frame.controls;
        out.reason=Reason::BridgeControls;
        if(controls.jitterDelay || controls.jitterMode!=3 || !controls.clampJitterHalf || controls.depthFlipY ||
           p.originX || p.originY || !detail::Finite(p.jitterX) || !detail::Finite(p.jitterY))return out;
        const float preX=.5f-frame.rawJitterX*frame.renderWidth,preY=.5f-frame.rawJitterY*frame.renderHeight;
        const float sdkX=std::clamp(preX,-.5f,.5f),sdkY=std::clamp(preY,-.5f,.5f);
        out.rasterPixelsX=pixelX;out.rasterPixelsY=pixelY;
        out.submittedPixelsX=p.jitterX;out.submittedPixelsY=p.jitterY;
        out.submissionClamped=preX!=sdkX || preY!=sdkY;
        out.reason=Reason::BridgeSubmissionMismatch;
        if(!GenshinCameraHistory::Near(p.jitterX,sdkX,1.e-6f) || !GenshinCameraHistory::Near(p.jitterY,sdkY,1.e-6f))return out;
        // The actual DLSS12 call and coordinate-preserving postpipeline
        // define the output reference: Pout = Praster - Jaccepted. NVIDIA's
        // sample uses +2*Jx/W,-2*Jy/H in its raster projection and forwards
        // the SAME signed values to NGX. Thus output UV -> raster depth UV
        // adds +Jaccepted/renderExtent. There is no invented .5 correction.
        out.reason=Reason::OutputKernelPhaseUnproved;
        if(!outputGrid || !outputGrid->Matches(owner) || outputGrid->renderWidth!=frame.renderWidth ||
           outputGrid->renderHeight!=frame.renderHeight || outputGrid->outputWidth!=frame.outputWidth ||
           outputGrid->outputHeight!=frame.outputHeight)return out;
        out.depthToOutputUvX=outputGrid->jitterPixelsX/frame.renderWidth;
        out.depthToOutputUvY=outputGrid->jitterPixelsY/frame.renderHeight;
        if(!detail::Finite(out.depthToOutputUvX) || !detail::Finite(out.depthToOutputUvY))return out;
        out.phase=true;out.reason=Reason::Qualified;
        return out;
    }catch(...){return {};}
}
inline Result Resolve(const WriterGroups& writers,float actualClear,
    const GenshinLegacy::NativeMrt::Frame& frame,const GenshinFsr::Parameters& p,
    const GenshinCameraHistory::Sample& camera,const GenshinGuides::SrOutputGrid* grid)noexcept {
    return ResolveBasis(writers,actualClear,frame,p,camera,grid,false);
}
inline Result ResolvePrepared(const WriterGroups& writers,float actualClear,
    const GenshinLegacy::NativeMrt::Frame& frame,const GenshinFsr::Parameters& p,
    const GenshinCameraHistory::Sample& camera,const GenshinGuides::SrOutputGrid* grid)noexcept {
    return ResolveBasis(writers,actualClear,frame,p,camera,grid,true);
}
} // namespace GenshinDepthSemantic
