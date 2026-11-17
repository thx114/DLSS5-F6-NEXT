// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <charconv>
#include <string>
#include <string_view>
#include <vector>

namespace taa::bindings {
// Executable DXBC declarations remain present when RDEF has been stripped.
// They define the complete replay binding contract; reflection only cross-checks it.
struct Declarations {
    bool valid=false;
    std::vector<unsigned> constants,textures,samplers;
    std::array<unsigned,14> constantBytes{};
    std::string refusal;
};
inline std::string_view Trim(std::string_view s) {
    const auto a=s.find_first_not_of(" \t\r\n");
    if(a==s.npos)return {};
    return s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);
}
inline bool Number(std::string_view s,unsigned& value) {
    if(s.empty())return false;
    const auto result=std::from_chars(s.data(),s.data()+s.size(),value);
    return result.ec==std::errc{}&&result.ptr==s.data()+s.size();
}
inline bool Register(std::string_view s,std::string_view prefix,unsigned limit,unsigned& slot) {
    return s.starts_with(prefix)&&Number(s.substr(prefix.size()),slot)&&slot<limit;
}
inline Declarations Read(std::string_view assembly) {
    Declarations out;bool model=false,instructions=false;
    const auto fail=[&](std::string_view reason) {out.refusal=reason;return out;};
    const auto add=[](std::vector<unsigned>& slots,unsigned slot) {
        for(const auto old:slots)if(old==slot)return false;
        slots.push_back(slot);return true;
    };
    while(!assembly.empty()) {
        const auto end=assembly.find('\n');
        auto line=assembly.substr(0,end);
        assembly=end==assembly.npos?std::string_view{}:assembly.substr(end+1);
        if(const auto zero=line.find('\0');zero!=line.npos)line=line.substr(0,zero);
        if(const auto comment=line.find("//");comment!=line.npos)line=line.substr(0,comment);
        line=Trim(line);if(line.empty())continue;
        const auto space=line.find_first_of(" \t");
        const auto op=line.substr(0,space);
        const auto args=space==line.npos?std::string_view{}:Trim(line.substr(space+1));
        if(op=="vs_4_0"||op=="vs_4_1"||op=="vs_5_0"||op=="ps_4_0"||op=="ps_4_1"||op=="ps_5_0") {
            if(model||instructions||!args.empty())return fail("duplicate or malformed shader model");
            model=true;continue;
        }
        if(!model)return fail("unsupported shader model");
        // Class linkage may have no usable RDEF. Inspect executable declarations
        // and calls, including SM5's interface_call, before position IR drops dcl_*.
        if(op.find("interface")!=op.npos||op.find("function")!=op.npos||op.starts_with("call")||
            op.starts_with("fcall")||op=="label")return fail("shader class linkage or calls");
        if(!op.starts_with("dcl_")){instructions=true;continue;}
        if(instructions)return fail("resource declaration after instructions");
        if(op=="dcl_constantbuffer") {
            const auto open=args.find('['),close=args.find(']');
            if(open==args.npos||close==args.npos||close<=open)return fail("malformed constant buffer declaration");
            unsigned slot=0,count=0;
            if(!Register(args.substr(0,open),"CB",14,slot)||!Number(args.substr(open+1,close-open-1),count)||!count||count>4096)
                return fail("constant buffer slot or extent");
            const auto indexing=Trim(args.substr(close+1));
            if(indexing!=", immediateIndexed"&&indexing!=", dynamicIndexed")return fail("unsupported constant buffer indexing");
            if(!add(out.constants,slot))return fail("duplicate constant buffer declaration");
            out.constantBytes[slot]=count*16;continue;
        }
        if(op=="dcl_resource_texture2d") {
            if(args.empty()||args.front()!='(')return fail("malformed Texture2D declaration");
            const auto close=args.find(')');if(close==args.npos)return fail("malformed Texture2D component types");
            auto types=Trim(args.substr(1,close-1));unsigned count=0;
            if(types.empty()||types.back()==',')return fail("malformed Texture2D component list");
            while(!types.empty()) {
                const auto comma=types.find(',');const auto type=Trim(types.substr(0,comma));
                if(type!="float"&&type!="sint"&&type!="uint")return fail("unsupported Texture2D component type");
                ++count;types=comma==types.npos?std::string_view{}:types.substr(comma+1);
            }
            unsigned slot=0;if(count!=4||!Register(Trim(args.substr(close+1)),"t",128,slot))return fail("Texture2D slot or component count");
            if(!add(out.textures,slot))return fail("duplicate texture declaration");continue;
        }
        if(op=="dcl_sampler") {
            const auto comma=args.find(',');unsigned slot=0;
            if(comma==args.npos||!Register(Trim(args.substr(0,comma)),"s",16,slot)||Trim(args.substr(comma+1))!="mode_default")
                return fail("unsupported sampler declaration");
            if(!add(out.samplers,slot))return fail("duplicate sampler declaration");continue;
        }
        if(op=="dcl_input"||op=="dcl_input_sgv"||op=="dcl_input_siv"||op=="dcl_input_ps"||
            op=="dcl_input_ps_sgv"||op=="dcl_input_ps_siv"||op=="dcl_output"||op=="dcl_output_sgv"||
            op=="dcl_output_siv"||op=="dcl_temps"||op=="dcl_indexableTemp"||op=="dcl_globalFlags")continue;
        return fail("unsupported shader declaration");
    }
    if(!model||!instructions)return fail("missing shader program");
    out.valid=true;return out;
}
}
