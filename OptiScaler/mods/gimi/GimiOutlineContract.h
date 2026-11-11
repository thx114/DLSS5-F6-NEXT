#pragma once
#include "GimiPlan.h"
#include <algorithm>
#include <array>

namespace gimi { namespace outline {
// Exact local TexFx 1.0661 Outline template. Content, NOT section/file names,
// grants this bounded resource contract. See third_party/texfx-outline/SOURCE.md
// and its original CC BY-SA 4.0 license. No draw-time string inspection.
inline constexpr std::string_view Pattern = R"gimiOutline((?(DEFINE)(?# This is where the Contract Demons are stored.)(?'fl'-?\d+\.?\d{0,6})(?'cmp'\.[xyzw]{1,4})(?'l1'l\((?&fl)\))(?'l4'l\((?&fl),\h(?&fl),\h(?&fl),\h(?&fl)\))(?'r'-?r\d\d?\.[xyzw]{1,4})(?'v'-?v\d\d?\.[xyzw]{1,4})(?'rv'-?[rv]\d\d?\.[xyzw]{1,4})(?'cb'-?cb\d\[\d+\]\.\w{1,4})(?'icb'icb\[-?r\d\d?\.[xyzw]{1,4}\h\+\h-?\d+\]\.[xyzw]{1,4})(?'dcl2d'dcl_resource_texture[12dcube]{2,4}\h\(float,float,float,float\)\ht\d\d?\n)(?'dclin'dcl_input_ps[\w\h]+(?&v)\n)(?'s2d'sample_[b_]{0,2}(aoffimmi_)?indexable(\(\d,\d,\d\))?\(texture2d\)\(float,float,float,float\)))(?s)(?'MatchOutline'dcl_output\ho0\.xyzw\ndcl_output\ho1\.xyzw\ndcl_output\ho2\.xyzw\ndcl_output\ho3\.x\ndcl_output\ho4\.x\ndcl_output\ho5\.x\n.*)(?'ModestyA'\h*lt\h(?&r),\h((?&cb)|(?&r)),\h)l\(0\.950000\)\n(?'ModestyB'^\h*if_nz\h(?&r)\n^\h+div\h(?&r),\h(?&v),\h(?&v)\n^\h+mul\h(?&r),\h(?&r),\h(?&cb)\n^\h+mul\h(?&r),\h(?&r),\h(?&l4)\n^\h+ge\h(?&r),\h(?&r),\h(?&r)\n^\h+frc\h(?&r),\h\|(?&r)\|\n^\h+movc\h(?&r),\h(?&r),\h(?&r),\h(?&r)\n^\h+mul\h(?&r),\h(?&r),\h(?&l4)\n^\h+ftou\h(?&r),\h(?&r)\n^\h+dp\d\h(?&r),\h(?&cb),\h(?&icb)\n^\h+dp\d\h(?&r),\h(?&cb),\h(?&icb)\n^\h+dp\d\h(?&r),\h(?&cb),\h(?&icb)\n^\h+dp\d\h(?&r),\h(?&cb),\h(?&icb)\n^\h+dp\d\h(?&r),\h(?&r),\h(?&icb)\n^\h+mad\h(?&r),\h((?&cb)|(?&r)),\h(?&l1),\h(?&r)\n^\h+add\h(?&r),\h(?&r),\h(?&l1)\n^\h+lt\h(?&r),\h(?&r),\h(?&l1)\n)(?'Modesty'\h*discard_n?z\h(?&r)\n)(?'TillTexcord'.*)(?'sample'(?&s2d)\h(?&r),\h(?'TexcoordX'[r]\d\d?\.[xyz][yzw][xyz][xyz]),\ht[01].[xyzw]{4},\hs\d(,\h\w\d+(\.[xyzw]{1,4})?)?$\n)(?'TillSetColor'.*else\s*mov\h(?'color'(?&r)),\hv\d\.(?'cswizzle'[xyzw]{4}).*endif)\n(?'TillRet'.*)ret)gimiOutline";
inline constexpr std::string_view Replacement = R"gimiOutline(${MatchOutline}ld_indexable(texture1d)(float,float,float,float) ${ini69}.xyzw, l(69,0), t120.xyzw\nftou ${msg}.x, ${ini69}_abs.x\nand ${msg}.y, ${msg}.x, l(0x1)\nif_z ${msg}.y\nld_indexable(texture1d)(float,float,float,float) ${ini70}.xyzw, l(70,0), t120.xyzw\neq ${msg}.y, ${ini70}.x, l(-10.000000)\neq ${msg}.z, ${ini70}.x, l(-20.000000)\nor ${msg}.y, ${msg}.y, ${msg}.z\nmovc ${ini70}.x, ${msg}.y, ${ini70}.x, l(0.0)\ndiscard_nz ${ini70}.x\nendif\nand ${msg}.y, ${msg}.x, l(0x20)\nmovc ${ini69}.x, ${msg}.y, l(0.10000), l(0.95)\n${ModestyA}${ini69}.x\n${ModestyB}and ${msg}.z, ${msg}.x, l(0x10)\nor ${msg}.z, ${msg}.z, ${msg}.y\nif_nz ${msg}.z\n${Modesty}endif\n${TillTexcord}\nmov ${texcoord}.xyzw, ${TexcoordX}\n${sample}${TillSetColor}\n${TexcoordX:+and ${msg}.y, ${msg}.x, l(0x1)\nif_z ${msg}.y\nresinfo_indexable(texture2d)(float,float,float,float) ${dim}.xyyy, l(0), t70.xyyy\nif_nz ${dim}.xy\nsample_aoffimmi_indexable(1,1,1)(texture2d)(float,float,float,float) ${tex70}.xyzw, ${texcoord}.xyzw, t70.xyzw, s15\nmov ${color}, ${tex70}.${cswizzle}\nendif\nendif\n}${TillRet}and ${msg}.y, ${msg}.x, l(0x1)\nif_z ${msg}.y\nmov ${tex69}.xyzw, l(0.000000, 0.000000, 0.000000, 0.000000)\nresinfo_indexable(texture2d)(float,float,float,float) ${dim}.xyzw, l(0), t69.xyzw\nif_nz ${dim}.xyyy\nsample_indexable(texture2d)(float,float,float,float) ${tex69}.xyzw, ${texcoord}.xyzw, t69.xyzw, s15\nendif\nmovc ${tex69}.x, ${tex69}.x, ${tex69}.x, ${ini69}.w\ndp3 ${r0}.x, ${tex69}.xyzx, ${tex69}.xyzx\nne ${r0}.x, ${r0}.x, l(0.000000)\nand ${r0}.x, ${r0}.x, l(0x3f800000)\nif_nz ${r0}.x\nand ${msg}.w, ${msg}.x, l(0x8)\nif_nz ${msg}.w\neq ${tex69}.x, ${tex69}.x, l(1.0)\ndiscard_nz ${tex69}.x\nelse\ndiscard_nz ${tex69}.x\nendif\nendif\nendif\nret)gimiOutline";
inline constexpr std::array<std::string_view, 8> Temps = {
    "dim", "ini69", "ini70", "msg", "r0", "tex69", "tex70", "texcoord"};
inline constexpr std::array<std::string_view, 4> Declarations = {
    "dcl_resource_texture1d(float,float,float,float)t120",
    "dcl_resource_texture2d(float,float,float,float)t69",
    "dcl_resource_texture2d(float,float,float,float)t70",
    "dcl_samplers15,mode_default"};
inline std::string CompactDeclaration(std::string_view line)
{
    std::string result;
    for (char ch : line) if (ch != ' ' && ch != '\t' && ch != '\r') result += ch;
    return result;
}
inline bool ExactTemplate(const ShaderRegexGroup& group)
{
    if (group.models.size() != 1 || group.models.front() != ShaderModel::PS50 ||
        group.patterns.size() != 1 || !group.patterns.front().replace ||
        group.patterns.front().pattern != Pattern || group.patterns.front().replacement != Replacement ||
        group.temps.size() != Temps.size() || group.declarations.size() != Declarations.size()) return false;
    for (std::size_t i = 0; i < Temps.size(); ++i) if (group.temps[i] != Temps[i]) return false;
    for (std::size_t i = 0; i < Declarations.size(); ++i)
        if (CompactDeclaration(group.declarations[i]) != Declarations[i]) return false;
    return true;
}
} } // namespace gimi::outline
