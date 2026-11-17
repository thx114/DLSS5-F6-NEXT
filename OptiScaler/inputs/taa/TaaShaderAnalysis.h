// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TaaProfile.h"
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <map>
#include <sstream>
#include <tuple>
#include <vector>

namespace taa::analysis {
using Microsoft::WRL::ComPtr;
struct Constant { unsigned slot=0, offset=0; bool operator==(const Constant&) const = default; };
struct Recipe {
    Profile profile;
    unsigned historySlot=0;
    std::optional<Constant> zParameters;
    // Recognizing temporal reprojection does not prove raster jitter. Some
    // engines apply it in the vertex/projection pass rather than this shader.
    bool jitterInResolve=false;
    bool validatedProjection=false;
};
struct BufferDeclaration { unsigned slot=0, bytes=0; };
// Observation roles are diagnostic labels only. They cannot construct a Profile
// or bypass any executable input/coverage/jitter contract.
struct ObservationRole { unsigned slot=0; const char* name=""; };
enum class CapturePolicy { Consecutive, SeedThenJoined };
struct ObservationContract {
    const char* name="";
    std::vector<ObservationRole> roles;
    CapturePolicy capture=CapturePolicy::Consecutive;
};
inline std::optional<ObservationContract> Observe(const void* bytes,size_t size,uint64_t hash) {
    // PID33600's complete DXBC: size + whole-byte FNV + embedded DXBC checksum.
    // SHA256 E801EE77421186D363AF5AB55E3F108E3A8247D484FA13AB61AD4CCFF280F8AC.
    // This known unresolved program needs same-draw CB/state and producer evidence.
    static constexpr unsigned char checksum[]{0x61,0xc1,0xdd,0xe0,0x88,0xe9,0x02,0xc0,0x90,0x51,0x1b,0xe4,0x7e,0xf0,0x7e,0xca};
    if(!bytes||size!=4668||hash!=0xB12FF59247A26796ull||std::memcmp(static_cast<const unsigned char*>(bytes)+4,checksum,sizeof(checksum)))return {};
    uint64_t actual=0xcbf29ce484222325ull;
    for(size_t i=0;i<size;++i){actual^=static_cast<const unsigned char*>(bytes)[i];actual*=0x100000001b3ull;}
    if(actual!=hash)return {};
    return ObservationContract{"conditional-packed-temporal-v1",{{0,"depth_hint"},{1,"motion_hint"},{2,"color_hint"},{3,"history_hint"},{4,"classification_hint"}},CapturePolicy::SeedThenJoined};
}
struct Result {
    std::vector<Recipe> recipes;
    std::vector<unsigned> textures;
    std::vector<BufferDeclaration> constantBuffers;
    std::optional<ObservationContract> observation; // independent of executable recipes
    bool vectorOutput=false; // diagnostic hint only, never input authority
    bool pixelOutputsKnown=false;
    std::array<unsigned,8> pixelOutputMasks{}; // possible declarations, not must-write/coverage proof
    unsigned threadsX=0,threadsY=0;
    std::string reason,unsupportedInstruction;
};

// A deliberately small scalar IR for *coordinates*, not a shader decompiler.
// Actual DXBC is disassembled by Microsoft's compiler. Register swizzles,
// component writes and branch joins are kept; unsupported arithmetic becomes
// opaque. Texture dimensions/names alone never supply temporal parameters.
class Coordinates final {
public:
    using Id=unsigned;
    enum class Arithmetic { AlgebraicCoordinates, PreserveOperations };
    enum class Op { Unknown, Literal, Input, Constant, Sample, Add, Mul, Div,
        Min, Max, Lt, Ge, Eq, Ne, Select, IAdd, INeg, IToF, UToF, FToI, FToU, And, Or, Xor, Not, Abs, Sqrt, Rcp, Saturate };
    struct Node { Op op=Op::Unknown; Id a=0,b=0,c=0; unsigned value=0,slot=0,component=0; };
    struct Sample { unsigned slot=0; std::array<Id,2> uv{}; };
    struct Linear { std::map<Id,double> terms; double constant=0; bool valid=true; };
    std::vector<Node> nodes{{}};
    std::vector<Sample> samples;
    std::map<unsigned,std::array<Id,4>> outputs;
    unsigned threadsX=0,threadsY=0;
    std::vector<BufferDeclaration> constantBuffers;
    std::array<unsigned,8> outputMasks{};
    std::array<unsigned,8> mustWrite{};
    unsigned exitCount=0;
    bool unsupportedControl=false;
    std::string unsupportedInstruction;
private:
    void Unsupported(std::string_view instruction) {
        unsupportedControl=true;
        if(unsupportedInstruction.empty())unsupportedInstruction=instruction;
    }
    using Key=std::tuple<Op,Id,Id,Id,unsigned,unsigned,unsigned>;
    std::map<Key,Id> interned_;
    std::vector<std::bitset<128>> dependencies_{{}};
    mutable std::map<Id,Linear> expanded_;
    std::map<std::string,std::array<Id,4>> registers_;
    struct Branch {
        decltype(registers_) before,yes;
        std::array<unsigned,8> beforeWrites{},yesWrites{};
        Id condition=0;
        bool zero=false,hasElse=false,beforeReachable=true,yesReachable=false;
    };
    struct Exit { decltype(outputs) values; std::array<unsigned,8> writes{}; };
    std::vector<Exit> exits_;
    std::array<unsigned,8> writes_{};
    bool reachable_=true;
    std::vector<Branch> branches_;
    unsigned opaque_=0;
    bool preserveOperations_=false;
    static std::string Trim(std::string_view s) {
        const auto first=s.find_first_not_of(" \t\r\n");
        if(first==s.npos)return {};
        return std::string(s.substr(first,s.find_last_not_of(" \t\r\n")-first+1));
    }
    static std::vector<std::string> Split(std::string_view s) {
        std::vector<std::string> result;size_t start=0;int depth=0;
        for(size_t i=0;i<s.size();++i) {
            if(s[i]=='('||s[i]=='[')++depth;
            if(s[i]==')'||s[i]==']')--depth;
            if(s[i]==','&&depth==0){result.push_back(Trim(s.substr(start,i-start)));start=i+1;}
        }
        result.push_back(Trim(s.substr(start)));return result;
    }
    static unsigned Index(std::string_view s,size_t from) {
        unsigned v=0;const auto r=std::from_chars(s.data()+from,s.data()+s.size(),v);
        return r.ec==std::errc{}?v:~0u;
    }
    Id Make(Node n) {
        if(nodes.size()>8192){Unsupported("coordinate-node-budget");return 0;}
        if(!preserveOperations_ && (n.op==Op::Add||n.op==Op::Mul||n.op==Op::Min||n.op==Op::Max))if(n.a>n.b)std::swap(n.a,n.b);
        const Key key{n.op,n.a,n.b,n.c,n.value,n.slot,n.component};
        if(const auto it=interned_.find(key);it!=interned_.end())return it->second;
        const auto id=unsigned(nodes.size());nodes.push_back(n);interned_.emplace(key,id);
        auto dependencies=dependencies_[n.a]|dependencies_[n.b]|dependencies_[n.c];
        if(n.op==Op::Sample)dependencies.set(n.slot);
        dependencies_.push_back(dependencies);return id;
    }
    Id Literal(float v) {return Make({Op::Literal,0,0,0,std::bit_cast<unsigned>(v)});}
    std::optional<float> Number(Id id) const {
        if(id&&nodes[id].op==Op::Literal)return std::bit_cast<float>(nodes[id].value);
        return {};
    }
    Id Operation(Op op,Id a,Id b=0,Id c=0) {
        // Position execution proofs must retain unknown/NaN dependencies and
        // original rounding points. Coordinate algebra is a separate consumer.
        if(preserveOperations_)return Make({op,a,b,c});
        if(op==Op::Select&&b==c)return b;
        if(!a || ((op==Op::Add||op==Op::Mul||op==Op::Div||op==Op::Min||op==Op::Max)&&!b))return 0;
        const auto x=Number(a),y=Number(b);
        if(op==Op::Add) {if(x&&*x==0)return b;if(y&&*y==0)return a;if(x&&y)return Literal(*x+*y);}
        if(op==Op::Mul) {if((x&&*x==0)||(y&&*y==0))return Literal(0);if(x&&*x==1)return b;if(y&&*y==1)return a;if(x&&y)return Literal(*x**y);}
        if(op==Op::Div&&x&&y&&*y!=0)return Literal(*x / *y);
        return Make({op,a,b,c});
    }
    Id Read(std::string text,unsigned lane,bool integer=false) {
        text=Trim(text);if(text.empty())return 0;
        if(text.front()=='-')return integer?Operation(Op::INeg,Read(text.substr(1),lane)):Operation(Op::Mul,Literal(-1),Read(text.substr(1),lane));
        if(text.front()=='|'&&text.back()=='|')return Operation(Op::Abs,Read(text.substr(1,text.size()-2),lane));
        if(text.starts_with("l(")) {
            const auto last=text.rfind(')');if(last==text.npos)return 0;
            const auto values=Split(std::string_view(text).substr(2,last-2));
            const auto& value=values[(std::min<size_t>)(lane,values.size()-1)];
            unsigned bits=0;
            if(value.starts_with("0x")) {const auto r=std::from_chars(value.data()+2,value.data()+value.size(),bits,16);if(r.ec!=std::errc{})return 0;}
            else if(value.find_first_of(".eE")!=value.npos) {char* end=nullptr;const float f=std::strtof(value.c_str(),&end);if(end!=value.c_str()+value.size())return 0;bits=std::bit_cast<unsigned>(f);}
            else {int literal=0;const auto r=std::from_chars(value.data(),value.data()+value.size(),literal);if(r.ec!=std::errc{})return 0;bits=unsigned(literal);}
            return Make({Op::Literal,0,0,0,bits});
        }
        const auto dot=text.find('.');
        const auto base=text.substr(0,dot);
        unsigned component=lane;
        if(dot!=text.npos) {
            const auto swizzle=text.substr(dot+1);if(swizzle.empty())return 0;
            const auto at=std::string_view("xyzw").find(swizzle[(std::min<size_t>)(lane,swizzle.size()-1)]);
            if(at==std::string_view::npos)return 0;component=unsigned(at);
        }
        if(base.starts_with("cb")) {
            const auto begin=base.find('['),end=base.find(']');if(begin==base.npos||end==base.npos)return 0;
            const auto slot=Index(base,2),reg=Index(base,begin+1);
            if(slot>13||reg>4095||base.substr(begin+1,end-begin-1).find_first_not_of("0123456789")!=std::string::npos)return 0;
            return Make({Op::Constant,0,0,0,reg*16+component*4,slot,component});
        }
        if(base.starts_with('v')) {
            auto slot=Index(base,1);
            if(slot==~0u) {
                if(base=="vThreadID")slot=256;
                else if(base=="vThreadGroupID")slot=257;
                else if(base=="vThreadIDInGroup")slot=258;
                else if(base=="vThreadIDInGroupFlattened")slot=259;
                else return 0;
            }
            return Make({Op::Input,0,0,0,0,slot,component});
        }
        const auto it=registers_.find(base);return it==registers_.end()?0:it->second[component];
    }
    void Write(const std::string& dest,const std::array<Id,4>& values) {
        const auto dot=dest.find('.');const auto base=dest.substr(0,dot);
        const auto mask=dot==dest.npos?std::string("xyzw"):dest.substr(dot+1);
        auto& result=registers_[base];
        for(unsigned i=0;i<mask.size()&&i<4;++i) {
            const auto c=std::string_view("xyzw").find(mask[i]);if(c!=std::string_view::npos)result[c]=values[c];
        }
        if(base.size()==2&&base[0]=='o'&&base[1]>='0'&&base[1]<='7') {
            const auto slot=unsigned(base[1]-'0');outputs[slot]=result;
            for(const auto c:mask){const auto lane=std::string_view("xyzw").find(c);if(lane<4)writes_[slot]|=1u<<lane;}
        }
    }
    void SyncOutputs() {
        outputs.clear();
        for(unsigned slot=0;slot<8;++slot)if(const auto it=registers_.find("o"+std::to_string(slot));it!=registers_.end())outputs[slot]=it->second;
    }
    void ExitPath() {
        if(reachable_)exits_.push_back({outputs,writes_});
        reachable_=false;
    }
    void FinalizeOutputs() {
        if(reachable_)ExitPath();
        outputs.clear();exitCount=unsigned(exits_.size());
        if(exits_.empty())return;
        mustWrite.fill(15);
        for(const auto& path:exits_)for(unsigned slot=0;slot<8;++slot)mustWrite[slot]&=path.writes[slot];
        for(const auto& path:exits_)for(const auto& [slot,values]:path.values) {
            auto& merged=outputs[slot];
            for(unsigned c=0;c<4;++c) {
                if(!merged[c])merged[c]=values[c];
                else if(values[c]&&merged[c]!=values[c])merged[c]=Make({Op::Unknown,merged[c],values[c],0,++opaque_});
            }
        }
    }
    void Join(Branch branch) {
        const bool otherReachable=branch.hasElse?branch.yesReachable:branch.beforeReachable;
        const auto& otherWrites=branch.hasElse?branch.yesWrites:branch.beforeWrites;
        const auto& other=branch.hasElse?branch.yes:branch.before;
        if(!reachable_) {
            if(otherReachable){registers_=other;writes_=otherWrites;reachable_=true;}
            SyncOutputs();return;
        }
        if(!otherReachable){SyncOutputs();return;}
        for(unsigned slot=0;slot<8;++slot)writes_[slot]&=otherWrites[slot];
        for(const auto& [key,value]:other)registers_.try_emplace(key);
        for(auto& [key,value]:registers_) {
            const auto it=other.find(key);
            for(unsigned c=0;c<4;++c) {
                const Id alternate=it==other.end()?0:it->second[c];
                // Keep the branch's data dependencies. A differing coordinate
                // stays a Select (not an invented linear expression).
                const Id yes=branch.hasElse?alternate:value[c],no=branch.hasElse?value[c]:alternate;
                value[c]=Operation(Op::Select,branch.condition,branch.zero?no:yes,branch.zero?yes:no);
            }
        }
        for(auto& [slot,value]:outputs) {
            const auto it=registers_.find("o"+std::to_string(slot));
            if(it!=registers_.end())value=it->second;else value={};
        }
    }
public:
    explicit Coordinates(std::string_view assembly,Arithmetic arithmetic=Arithmetic::AlgebraicCoordinates)
        : preserveOperations_(arithmetic==Arithmetic::PreserveOperations) {
        std::istringstream lines{std::string(assembly)};std::string line;
        while(std::getline(lines,line)) {
            line=Trim(line);if(line.empty()||line.starts_with("//"))continue;
            const auto space=line.find_first_of(" \t");auto op=line.substr(0,space);
            if(op=="dcl_constantbuffer"&&space!=line.npos) {
                const auto declaration=Trim(std::string_view(line).substr(space+1));const auto bracket=declaration.find('[');
                if(bracket!=declaration.npos) {const auto slot=Index(declaration,2),count=Index(declaration,bracket+1);
                    if(slot<14&&count&&count<=4096)constantBuffers.push_back({slot,count*16});}
                continue;
            }
            if(op=="dcl_output"&&space!=line.npos) {
                const auto declaration=Trim(std::string_view(line).substr(space+1));const auto slot=Index(declaration,1);
                const auto dot=declaration.find('.');
                if(slot<8&&dot!=declaration.npos)for(const auto c:declaration.substr(dot+1)) {
                    const auto lane=std::string_view("xyzw").find(c);if(lane<4)outputMasks[slot]|=1u<<lane;}
                continue;
            }
            if(op=="dcl_thread_group"&&space!=line.npos) {const auto group=Split(std::string_view(line).substr(space+1));if(group.size()>=2){threadsX=Index(group[0],0);threadsY=Index(group[1],0);}continue;}
            if(op.starts_with("dcl_")||op=="ps_5_0"||op=="cs_5_0"||op=="ps_4_0"||op=="cs_4_0")continue;
            if(op=="ret") {ExitPath();if(branches_.empty())break;continue;}
            if(op.starts_with("retc")||op.starts_with("discard")||op=="loop"||op=="endloop"||op.starts_with("call")||op=="switch"||op.starts_with("break")) {Unsupported(op);continue;}
            if(op=="if_nz"||op=="if_z") {
                Branch branch;branch.before=registers_;branch.beforeWrites=writes_;
                branch.condition=Read(line.substr(space+1),0);branch.zero=op=="if_z";branch.beforeReachable=reachable_;
                branches_.push_back(std::move(branch));continue;
            }
            if(op=="else") {
                if(branches_.empty()||branches_.back().hasElse){Unsupported("unmatched-else");continue;}
                auto& branch=branches_.back();branch.yes=registers_;branch.yesWrites=writes_;branch.yesReachable=reachable_;branch.hasElse=true;
                registers_=branch.before;writes_=branch.beforeWrites;reachable_=branch.beforeReachable;SyncOutputs();continue;
            }
            if(op=="endif") {
                if(branches_.empty()){Unsupported("unmatched-endif");continue;}
                auto branch=std::move(branches_.back());branches_.pop_back();Join(std::move(branch));continue;
            }
            if(!reachable_)continue;
            if(op=="loop"||op=="endloop"||op.starts_with("call")||op=="switch") {registers_.clear();continue;}
            if(space==line.npos)continue;
            const auto args=Split(std::string_view(line).substr(space+1));if(args.empty())continue;
            const auto suffix=op.find('_');const auto simple=op.substr(0,suffix);
            if(simple=="sample" && !(op=="sample"||op.starts_with("sample_indexable")||op.starts_with("sample_l")||op.starts_with("sample_b")||op.starts_with("sample_d"))) {
                Unsupported(op);continue;
            }
            if(op.starts_with("sample")||simple=="ld") {
                if(args.size()<3)continue;
                const auto slot=Index(args[2],1);if(slot>127)continue;
                if(samples.size()>=128){Unsupported("texture-sample-budget");continue;}
                samples.push_back({slot,{Read(args[1],0),Read(args[1],1)}});
                std::array<Id,4> values{};
                for(unsigned c=0;c<4;++c) {
                    unsigned component=c;const auto dot=args[2].find('.');
                    if(dot!=args[2].npos&&dot+1+c<args[2].size())component=unsigned(std::string_view("xyzw").find(args[2][dot+1+c]));
                    if(component<4)values[c]=Make({Op::Sample,0,0,0,unsigned(samples.size()),slot,component});
                }
                Write(args[0],values);continue;
            }
            if(op=="store_uav_typed") {
                const auto output=Index(args[0],1);
                if(output<8&&args.size()>=3){for(unsigned c=0;c<4;++c)outputs[output][c]=Read(args[2],c);writes_[output]=15;}
                continue;
            }
            // These instructions have two write destinations. Treat all of
            // them as unknown until both destinations have an IR definition.
            if(simple=="imul"||simple=="umul"||simple=="udiv"||simple=="sincos"||simple=="uaddc"||simple=="usubb"||simple=="swapc") {
                Unsupported(op);continue;
            }
            std::array<Id,4> values{};
            for(unsigned c=0;c<4;++c) {
                const auto read=[&](size_t i){return i<args.size()?Read(args[i],c,simple=="iadd"||simple=="ineg"||simple=="itof"):0u;};
                const auto a=read(1),b=read(2),d=read(3);
                if(simple=="mov")values[c]=a;
                else if(simple=="mad")values[c]=Operation(Op::Add,Operation(Op::Mul,a,b),d);
                else if(simple=="add")values[c]=Operation(Op::Add,a,b);
                else if(simple=="mul")values[c]=Operation(Op::Mul,a,b);
                else if(simple=="div")values[c]=Operation(Op::Div,a,b);
                else if(simple=="min")values[c]=Operation(Op::Min,a,b);
                else if(simple=="max")values[c]=Operation(Op::Max,a,b);
                else if(simple=="lt")values[c]=Operation(Op::Lt,a,b);
                else if(simple=="ge")values[c]=Operation(Op::Ge,a,b);
                else if(simple=="eq")values[c]=Operation(Op::Eq,a,b);
                else if(simple=="ne")values[c]=Operation(Op::Ne,a,b);
                else if(simple=="movc")values[c]=Operation(Op::Select,a,b,d);
                else if(simple=="iadd")values[c]=Operation(Op::IAdd,a,b);
                else if(simple=="ineg")values[c]=Operation(Op::INeg,a);
                else if(simple=="itof")values[c]=Operation(Op::IToF,a);
                else if(simple=="utof")values[c]=Operation(Op::UToF,a);
                else if(simple=="ftoi")values[c]=Operation(Op::FToI,a);
                else if(simple=="ftou")values[c]=Operation(Op::FToU,a);
                else if(simple=="and")values[c]=Operation(Op::And,a,b);
                else if(simple=="or")values[c]=Operation(Op::Or,a,b);
                else if(simple=="xor")values[c]=Operation(Op::Xor,a,b);
                else if(simple=="not")values[c]=Operation(Op::Not,a);
                else if(simple=="sqrt")values[c]=Operation(Op::Sqrt,a);
                else if(simple=="rcp")values[c]=Operation(Op::Rcp,a);
                else {
                    static constexpr std::array<std::string_view,24> singleDestination{
                        "dp2","dp3","dp4","rsq","exp","log","frc","round","deriv","ftoi","ftou","bfrev","countbits",
                        "firstbit","ubfe","ibfe","ishl","ishr","ushr","bfi","resinfo","bufinfo","lod","check"};
                    if(std::find(singleDestination.begin(),singleDestination.end(),simple)==singleDestination.end())Unsupported(op);
                    values[c]=Make({Op::Unknown,a,b,d,++opaque_});
                }
                if(op.ends_with("_sat"))values[c]=Operation(Op::Saturate,values[c]);
            }
            Write(args[0],values);
        }
        if(!branches_.empty())Unsupported("unterminated-if");
        FinalizeOutputs();
    }
    Linear Expand(Id id,double coefficient=1,unsigned depth=0) const {
        Linear out;if(!id||depth>64){out.valid=false;return out;}
        if(coefficient!=1) {
            out=Expand(id,1,depth);out.constant*=coefficient;for(auto& [term,factor]:out.terms)factor*=coefficient;return out;
        }
        if(const auto it=expanded_.find(id);it!=expanded_.end())return it->second;
        const auto& n=nodes[id];
        if(n.op==Op::Literal){out.constant=coefficient*std::bit_cast<float>(n.value);return out;}
        if(n.op==Op::Add) {
            out=Expand(n.a,coefficient,depth+1);auto rhs=Expand(n.b,coefficient,depth+1);out.valid&=rhs.valid;out.constant+=rhs.constant;
            for(const auto& [term,factor]:rhs.terms)out.terms[term]+=factor;
        } else if(n.op==Op::Mul&&(Number(n.a)||Number(n.b))) {
            const auto scalar=Number(n.a);return Expand(scalar?n.b:n.a,coefficient*double(scalar?*scalar:*Number(n.b)),depth+1);
        } else out.terms[id]=coefficient;
        expanded_[id]=out;return out;
    }
    std::bitset<128> Textures(Id id) const {return id<dependencies_.size()?dependencies_[id]:std::bitset<128>{};}
    struct AffineSample { double scale=0,offset=0;unsigned sample=0; };
    std::optional<AffineSample> Raw(Id id,unsigned slot,unsigned component) const {
        const auto expression=Expand(id);if(!expression.valid)return {};
        std::optional<AffineSample> out;
        for(const auto& [term,factor]:expression.terms) {
            if(std::abs(factor)<1e-8)continue;
            const auto& n=nodes[term];
            if(out||n.op!=Op::Sample||n.slot!=slot||n.component!=component)return {};
            out=AffineSample{factor,expression.constant,n.value};
        }
        return out;
    }
    std::optional<AffineSample> Centered(Id id,unsigned slot,unsigned component) const {
        const auto value=Raw(id,slot,component);
        if(!value||std::abs(value->offset+value->scale*127.0/255.0)>1e-6)return {};
        return value;
    }
    std::optional<AffineSample> Sign(Id id,unsigned slot,unsigned component) const {
        if(!id||nodes[id].op!=Op::IToF)return {};
        const auto& add=nodes[nodes[id].a];if(add.op!=Op::IAdd)return {};
        for(const auto& [negative,positive]:{std::pair{add.a,add.b},std::pair{add.b,add.a}}) {
            const auto& neg=nodes[negative];if(neg.op!=Op::INeg)continue;
            const auto& a=nodes[neg.a];const auto& b=nodes[positive];
            if(a.op!=Op::Lt||b.op!=Op::Lt||a.b!=b.a)continue;
            const auto zeroA=Number(a.a),zeroB=Number(b.b);
            if(!zeroA||!zeroB||*zeroA!=0||*zeroB!=0)continue;
            if(auto centered=Centered(a.b,slot,component)){centered->scale=centered->scale>0?1:-1;return centered;}
        }
        return {};
    }
    bool Factors(Id id,std::vector<Id>& factors,double& scale,unsigned depth=0) const {
        if(!id||depth>64||factors.size()>4)return false;
        if(const auto value=Number(id)){scale*=*value;return std::isfinite(scale);}
        const auto& n=nodes[id];
        if(n.op==Op::Mul)return Factors(n.a,factors,scale,depth+1)&&Factors(n.b,factors,scale,depth+1);
        factors.push_back(id);return true;
    }
    std::optional<std::pair<Encoding,float>> Motion(Id id,unsigned slot,unsigned component) const {
        if(const auto linear=Raw(id,slot,component);linear&&std::abs(linear->offset)<1e-8&&linear->scale!=0)
            return std::pair{Encoding::Linear,float(linear->scale)};
        std::vector<Id> factors;double scale=1;
        if(!Factors(id,factors,scale)||factors.size()!=3)return {};
        for(unsigned i=0;i<3;++i) {
            const auto sign=Sign(factors[i],slot,component);
            const auto a=Centered(factors[(i+1)%3],slot,component),b=Centered(factors[(i+2)%3],slot,component);
            if(sign&&a&&b&&sign->sample==a->sample&&a->sample==b->sample) {
                const double multiplier=scale*sign->scale*a->scale*b->scale/4;
                if(std::isfinite(multiplier)&&multiplier!=0)return std::pair{Encoding::SignedSquare,float(multiplier)};
            }
        }
        return {};
    }
};

inline Result Analyze(const void* bytes,size_t size,Stage stage,uint64_t hash,std::string* assembly=nullptr) {
    Result result;ComPtr<ID3DBlob> disassembly;
    if(FAILED(D3DDisassemble(bytes,size,D3D_DISASM_DISABLE_DEBUG_INFO,nullptr,&disassembly))) {result.reason="DXBC analysis is unavailable";return result;}
    const std::string_view text(static_cast<const char*>(disassembly->GetBufferPointer()),disassembly->GetBufferSize());
    if(assembly)assembly->assign(text.data(),text.size());
    const Coordinates ir(text);
    result.threadsX=ir.threadsX;result.threadsY=ir.threadsY;
    result.constantBuffers=ir.constantBuffers;
    if(stage==Stage::Pixel)result.observation=Observe(bytes,size,hash);
    result.vectorOutput=stage==Stage::Pixel&&ir.outputMasks[0]==3&&!ir.samples.empty();
    for(const auto& s:ir.samples)if(std::find(result.textures.begin(),result.textures.end(),s.slot)==result.textures.end())result.textures.push_back(s.slot);
    // Output signatures are independent of coordinate-IR support. In
    // particular, a loop must not hide an otherwise known MRT declaration.
    ComPtr<ID3D11ShaderReflection> reflection;
    if(SUCCEEDED(D3DReflect(bytes,size,IID_PPV_ARGS(&reflection)))&&stage==Stage::Pixel) {
        D3D11_SHADER_DESC signature{};
        if(SUCCEEDED(reflection->GetDesc(&signature))) {
            result.pixelOutputsKnown=true;
            for(unsigned i=0;i<signature.OutputParameters;++i) {
                D3D11_SIGNATURE_PARAMETER_DESC output{};
                if(FAILED(reflection->GetOutputParameterDesc(i,&output))) {result.pixelOutputsKnown=false;break;}
                if(output.SystemValueType==D3D_NAME_TARGET&&output.SemanticIndex<result.pixelOutputMasks.size())
                    result.pixelOutputMasks[output.SemanticIndex]|=output.Mask;
            }
        }
    }
    // A content-identified contract supplements the coordinate IR for this
    // conditional packed-motion resolve. Producer projection, alias and reset
    // checks still run against this draw's actual resources and constants.
    if(stage==Stage::Pixel && result.observation) {
        result.textures={0,1,2,3,4};
        Recipe recipe;auto& p=recipe.profile;
        p.stage=stage;p.shaderHash=hash;p.colorSlot=2;p.depthSlot=0;p.motionSlot=1;p.outputSlot=0;
        p.encoding=Encoding::SignedSquare;p.motionSampling=MotionSampling::NearestDepth5Max;
        p.motionUnits=Units::Uv;p.motionScaleX=p.motionScaleY=1;
        p.jitterSource=JitterSource::MotionProjection;p.motionJittered=false;p.hdr=true;
        p.outputRoute=OutputRoute::ObserveUpscale;
        recipe.historySlot=3;recipe.validatedProjection=true;
        result.recipes.push_back(recipe);result.reason="Waiting for validated packed-motion producer projection";
        return result;
    }
    if(ir.unsupportedControl){result.reason="Temporal shader uses unsupported control flow";result.unsupportedInstruction=ir.unsupportedInstruction;return result;}
    std::optional<Constant> zParameters;
    if(reflection) {
        UINT z=0;reflection->GetThreadGroupSize(&result.threadsX,&result.threadsY,&z);
        D3D11_SHADER_DESC desc{};reflection->GetDesc(&desc);
        for(unsigned i=0;i<desc.BoundResources;++i) {
            D3D11_SHADER_INPUT_BIND_DESC binding{};
            if(FAILED(reflection->GetResourceBindingDesc(i,&binding))||binding.Type!=D3D_SIT_CBUFFER)continue;
            auto* cb=reflection->GetConstantBufferByName(binding.Name);D3D11_SHADER_BUFFER_DESC buffer{};if(FAILED(cb->GetDesc(&buffer)))continue;
            for(unsigned j=0;j<buffer.Variables;++j){D3D11_SHADER_VARIABLE_DESC variable{};
                if(SUCCEEDED(cb->GetVariableByIndex(j)->GetDesc(&variable))&&std::string_view(variable.Name)=="_ZBufferParams"&&variable.Size>=8)
                    zParameters=Constant{binding.BindPoint,variable.StartOffset};}
        }
    }
    unsigned pairs=0;
    for(const auto& history:ir.samples)for(const auto& current:ir.samples) {
        if(++pairs>4096){result.recipes.clear();result.reason="Temporal coordinate analysis exceeded its work budget";return result;}
        if(current.slot==history.slot)continue;
        Recipe recipe;auto& p=recipe.profile;p.stage=stage;p.shaderHash=hash;p.colorSlot=current.slot;recipe.historySlot=history.slot;
        p.threadsX=result.threadsX;p.threadsY=result.threadsY;p.motionUnits=p.jitterUnits=Units::Uv;
        p.outputRoute=OutputRoute::ObserveUpscale; // Scheduling still requires an observed, proven consumer chain.
        p.motionJittered=false;recipe.zParameters=zParameters;
        bool good=true;std::array<Constant,2> jitter{};std::array<bool,2> haveJitter{};std::optional<Encoding> encoding;unsigned motionSlot=~0u;
        for(unsigned axis=0;axis<2&&good;++axis) {
            auto difference=ir.Expand(history.uv[axis]);const auto color=ir.Expand(current.uv[axis],-1);
            difference.valid&=color.valid;difference.constant+=color.constant;
            for(const auto& [term,factor]:color.terms)difference.terms[term]+=factor;
            if(!difference.valid||std::abs(difference.constant)>1e-6){good=false;break;}
            bool haveMotion=false;
            for(const auto& [term,factor]:difference.terms) {
                if(std::abs(factor)<1e-7)continue;
                const auto& node=ir.nodes[term];const auto textures=ir.Textures(term);
                if(node.op==Coordinates::Op::Constant&&!haveJitter[axis]) {
                    jitter[axis]={node.slot,node.value};(axis?p.jitterScaleY:p.jitterScaleX)=float(factor);haveJitter[axis]=true;
                } else if(textures.count()==1&&!haveMotion) {
                    unsigned slot=0;while(slot<128&&!textures.test(slot))++slot;
                    const auto decoded=ir.Motion(term,slot,axis);
                    if(!decoded||slot==current.slot||slot==history.slot||ir.Textures(current.uv[axis]).test(slot)||!ir.Textures(history.uv[axis]).test(slot)) {good=false;break;}
                    if((motionSlot!=~0u&&motionSlot!=slot)||(encoding&&*encoding!=decoded->first)){good=false;break;}
                    motionSlot=slot;encoding=decoded->first;(axis?p.motionScaleY:p.motionScaleX)=float(factor)*decoded->second;haveMotion=true;
                } else {good=false;break;}
            }
            good&=haveMotion;
        }
        if(!good||haveJitter[0]!=haveJitter[1])continue;
        recipe.jitterInResolve=haveJitter[0];
        if(recipe.jitterInResolve&&(jitter[0].slot!=jitter[1].slot||jitter[0].offset==jitter[1].offset))continue;
        bool output=false;
        for(const auto& [slot,values]:ir.outputs) {
            if(stage==Stage::Pixel&&ir.mustWrite[slot]!=15)continue;
            std::bitset<128> used;for(unsigned c=0;c<3;++c)used|=ir.Textures(values[c]);
            if(used.test(current.slot)&&used.test(history.slot)){p.outputSlot=slot;output=true;break;}
        }
        if(!output)continue;
        p.motionSlot=motionSlot;p.encoding=*encoding;p.jitterSlot=jitter[0].slot;p.jitterOffsetX=jitter[0].offset;p.jitterOffsetY=jitter[1].offset;
        const auto duplicate=std::find_if(result.recipes.begin(),result.recipes.end(),[&](const Recipe& r){return r.profile==p&&r.historySlot==recipe.historySlot;});
        if(duplicate==result.recipes.end())result.recipes.push_back(recipe);
    }
    result.reason=result.recipes.empty()?"No supported color/history/motion relationship was found":"Temporal coordinates analyzed";
    return result;
}
} // namespace taa::analysis
