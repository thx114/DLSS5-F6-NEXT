#include "GimiShaderTools.h"
#include "GimiDxbcBridge.h"
#include "GimiOutlineContract.h"
#include "GimiDiffuseContract.h"
#include "GimiVertexContract.h"
#define PCRE2_STATIC
#define PCRE2_CODE_UNIT_WIDTH 8
#include "third_party/pcre2/include/pcre2.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <set>
#include <stdexcept>

namespace gimi
{
namespace
{
constexpr std::size_t TextLimit = 4u * 1024u * 1024u, RegexCodeLimit = 8u * 1024u * 1024u;
struct PcreCodeFree { void operator()(pcre2_code* p) const noexcept { pcre2_code_free(p); } };
struct MatchFree { void operator()(pcre2_match_data* p) const noexcept { pcre2_match_data_free(p); } };
struct ContextFree { void operator()(pcre2_match_context* p) const noexcept { pcre2_match_context_free(p); } };
using Code = std::unique_ptr<pcre2_code, PcreCodeFree>;
using Match = std::unique_ptr<pcre2_match_data, MatchFree>;
using Context = std::unique_ptr<pcre2_match_context, ContextFree>;
std::string PcreError(int code)
{
    PCRE2_UCHAR text[160]{};
    const auto size = pcre2_get_error_message(code, text, sizeof(text));
    return size > 0 ? std::string(reinterpret_cast<char*>(text), static_cast<std::size_t>(size)) : "PCRE2 failure";
}
bool Number(std::string_view text, std::uint32_t& value)
{
    int base = 10;
    if (text.size() > 2 && text.substr(0, 2) == "0x") { base = 16; text.remove_prefix(2); }
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size();
}
std::string Compact(std::string_view text)
{
    std::string result;
    for (char ch : text) if (ch != ' ' && ch != '\t' && ch != '\r') result += ch;
    return result;
}
std::vector<std::string> Lines(const std::string& text)
{
    std::vector<std::string> lines;
    for (std::size_t begin = 0; begin < text.size();)
    {
        const auto end = text.find('\n', begin);
        auto line = Trim(std::string_view(text).substr(begin, end == text.npos ? text.size() - begin : end - begin));
        const auto comment = line.find("//");
        if (comment != line.npos) line = Trim(std::string_view(line).substr(0, comment));
        if (!line.empty()) lines.push_back(std::move(line));
        begin = end == text.npos ? text.size() : end + 1;
    }
    return lines;
}
bool Resource120(std::string_view line)
{
    for (std::size_t at = line.find("t120"); at != line.npos; at = line.find("t120", at + 4))
    {
        const auto name = [](char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'; };
        if ((!at || !name(line[at - 1])) && (at + 4 == line.size() || !name(line[at + 4]))) return true;
    }
    return false;
}
constexpr const char* ParameterDeclaration = "dcl_resource_texture1d(float,float,float,float)t120";
struct CodeContract
{
    std::vector<std::string> declarations;
    std::array<IniParameterRead, 256> reads{};
    std::uint32_t readCount = 0, temps = 0, parameterDeclarations = 0;
    std::array<unsigned, 3> outlineDeclarations{};
    std::array<unsigned, 3> diffuseDeclarations{};
    std::array<UINT, 14> constantVectors{}, addedConstantBytes{}, rewriteConstantBytes{};
};
bool Load120(const std::string& compact, CodeContract& contract)
{
    // Inspect actual instructions disassembled from the FINAL assembled DXBC,
    // not regex text or a declaration. Only direct integer texel/mip0 loads.
    constexpr std::string_view opcode = "ld_indexable(texture1d)(float,float,float,float)";
    if (compact.compare(0, opcode.size(), opcode)) return false;
    const auto operands = std::string_view(compact).substr(opcode.size());
    std::array<std::string_view, 3> args{};
    unsigned count = 0, depth = 0; std::size_t begin = 0;
    for (std::size_t i = 0; i <= operands.size(); ++i)
    {
        if (i != operands.size() && operands[i] == '(') ++depth;
        else if (i != operands.size() && operands[i] == ')') { if (!depth) return false; --depth; }
        if (i == operands.size() || (operands[i] == ',' && !depth))
        { if (count == args.size()) return false; args[count++] = operands.substr(begin, i - begin); begin = i + 1; }
    }
    if (depth || count != args.size()) return false;
    const auto dot = args[0].find('.'); std::uint32_t reg = 0;
    if (dot == args[0].npos || args[0].empty() || args[0].front() != 'r' || !Number(args[0].substr(1, dot - 1), reg) || reg >= contract.temps) return false;
    const auto mask = args[0].substr(dot + 1);
    if (mask.empty() || mask.size() > 4 || args[2].size() != 9 || args[2].substr(0, 5) != "t120.") return false;
    auto address = args[1];
    if (address.size() < 4 || address.substr(0, 2) != "l(" || address.back() != ')') return false;
    address = address.substr(2, address.size() - 3);
    std::array<std::uint32_t, 4> coordinates{}; count = 0; begin = 0;
    for (std::size_t i = 0; i <= address.size(); ++i)
        if (i == address.size() || address[i] == ',')
        {
            if (count == coordinates.size() || !Number(address.substr(begin, i - begin), coordinates[count++])) return false;
            begin = i + 1;
        }
    if ((count != 2 && count != 4) || coordinates[0] >= IniParameterVectors || coordinates[1]) return false;
    if (count == 4 && (coordinates[2] || coordinates[3])) return false;
    std::uint8_t written = 0, components = 0;
    for (char ch : mask)
    {
        const auto lane = std::string_view("xyzw").find(ch);
        if (lane == std::string_view::npos || (written & (1u << lane))) return false;
        written |= static_cast<std::uint8_t>(1u << lane);
        const auto source = std::string_view("xyzw").find(args[2][5 + lane]);
        if (source == std::string_view::npos) return false;
        components |= static_cast<std::uint8_t>(1u << source);
    }
    for (std::uint32_t i = 0; i < contract.readCount; ++i)
        if (contract.reads[i].vector == coordinates[0]) { contract.reads[i].components |= components; return true; }
    if (contract.readCount == contract.reads.size()) return false;
    contract.reads[contract.readCount++] = {coordinates[0], components};
    return true;
}
bool DescribeCode(const std::string& text, bool patched, CodeContract& contract, std::string& error,
                  bool exactOutline = false, bool exactDiffuse = false,
                  bool vertexStage = false, const CodeContract* vertexOriginal = nullptr)
{
    const auto lines = Lines(text);
    if (lines.empty() || lines.front() != (vertexStage ? "vs_5_0" : "ps_5_0"))
    { error = "Authentic stage-specific shader 5.0 code required"; return false; }
    unsigned tempDeclarations = 0;
    bool body = false;
    // Obtain the temp count before checking any instruction destination.
    for (const auto& line : lines)
        if (line.compare(0, 10, "dcl_temps ") == 0)
        {
            if (++tempDeclarations != 1 || !Number(Trim(std::string_view(line).substr(10)), contract.temps) || contract.temps > 4096)
            { error = "Invalid/bounded shader temporary count"; return false; }
        }
    for (std::size_t i = 1; i < lines.size(); ++i)
    {
        const auto& line = lines[i];
        auto compact = Compact(line);
        if (line.compare(0, 4, "dcl_") == 0)
        {
            if (body) { error = "Shader declarations occur after executable code"; return false; }
            if (line.compare(0, 10, "dcl_temps ") == 0) continue;
            // Immediate-constant declarations can span lines. Preserve their
            // complete content, not merely their opening declaration spelling.
            int depth = 0;
            for (char ch : line) { if (ch == '{') ++depth; else if (ch == '}') --depth; }
            while (depth > 0 && i + 1 < lines.size())
            {
                const auto& continuation = lines[++i]; compact += Compact(continuation);
                for (char ch : continuation) { if (ch == '{') ++depth; else if (ch == '}') --depth; }
            }
            if (depth) { error = "Unclosed shader declaration"; return false; }
            if (vertexStage && compact.compare(0, 18, "dcl_constantbuffer") == 0)
            {
                vertex::ConstantDeclaration constant;
                if (!vertex::Constant(compact, constant) || contract.constantVectors[constant.slot])
                { error = "Invalid/duplicate VS constant-buffer declaration"; return false; }
                contract.constantVectors[constant.slot] = constant.vectors;
                // Preserve original declaration bytes/order (including dynamic
                // indexing). Only absent, exact new slots may be added.
                if (patched && vertexOriginal && !vertexOriginal->constantVectors[constant.slot])
                {
                    if (!vertex::AddedConstant(constant))
                    { error = "New VS CB requires exact CB8[7]/CB12[10]/CB13[10] immediate range"; return false; }
                    contract.addedConstantBytes[constant.slot] = constant.vectors * 16u;
                    continue;
                }
            }
            if (patched && compact == ParameterDeclaration)
            { if (++contract.parameterDeclarations != 1) { error = "Duplicate t120 declaration"; return false; } continue; }
            bool added = false;
            if (patched && exactOutline)
                for (unsigned n = 0; n < contract.outlineDeclarations.size(); ++n)
                    if (compact == outline::Declarations[n + 1])
                    {
                        if (++contract.outlineDeclarations[n] != 1)
                        { error = "Duplicate exact Outline resource/sampler declaration"; return false; }
                        added = true; break;
                    }
            if (patched && exactDiffuse)
                for (unsigned n = 0; n < contract.diffuseDeclarations.size(); ++n)
                    if (compact == diffuse::Declarations[n + 1])
                    {
                        if (++contract.diffuseDeclarations[n] != 1)
                        { error = "Duplicate exact Diffuse declaration"; return false; }
                        added = true; break;
                    }
            if (added) continue;
            if (Resource120(compact)) { error = "Original/reserved t120 declaration conflicts with this consumer"; return false; }
            contract.declarations.push_back(std::move(compact));
        }
        else
        {
            body = true;
            // No relative resource index can hide a read of the reserved slot.
            // PS5.0 direct operands only; do not infer a resource-array ABI.
            for (std::size_t at = 0; at < compact.size(); ++at) if (compact[at] == 't')
            {
                auto index = at + 1;
                while (index < compact.size() && compact[index] >= '0' && compact[index] <= '9') ++index;
                if (index < compact.size() && compact[index] == '[')
                { error = "Relative resource operands are unsupported by the exact t120 consumer"; return false; }
            }
            if (Resource120(compact) && (!patched || !Load120(compact, contract)))
            { error = "Unsupported t120 instruction: direct bounded integer Texture1D float4 load required"; return false; }
        }
    }
    if (patched && (!contract.parameterDeclarations || !contract.readCount))
    { error = "No authentic t120 declaration AND actual load in assembled code"; return false; }
    if (patched && exactOutline)
    {
        for (auto count : contract.outlineDeclarations) if (count != 1)
        { error = "Missing exact Outline t69/t70/s15 declaration"; return false; }
        if (contract.readCount != 2)
        { error = "Exact Outline requires actual bounded t120 reads of both 69 and 70"; return false; }
        for (std::uint32_t i = 0; i < contract.readCount; ++i)
            if ((contract.reads[i].vector != 69 && contract.reads[i].vector != 70) || contract.reads[i].components != 15)
            { error = "Unexpected exact Outline parameter index/component contract"; return false; }
    }
    if (patched && exactDiffuse)
    {
        for (auto count : contract.diffuseDeclarations) if (count != 1)
        { error = "Missing exact Diffuse t69/t72/o6 declaration"; return false; }
        if (contract.readCount != 2) { error = "Diffuse requires real IniParams 69/70 loads"; return false; }
        for (unsigned i = 0; i < contract.readCount; ++i)
            if ((contract.reads[i].vector != 69 && contract.reads[i].vector != 70) || contract.reads[i].components != 15)
            { error = "Unexpected Diffuse parameter operand"; return false; }
    }
    return true;
}
std::vector<std::string> Instructions(const std::string& text)
{
    std::vector<std::string> result;
    int braces = 0;
    for (const auto& line : Lines(text))
    {
        if (braces || line.compare(0, 4, "dcl_") == 0)
        {
            for (char ch : line) { if (ch == '{') ++braces; else if (ch == '}') --braces; }
            continue;
        }
        if (line != "ps_5_0" && line != "vs_5_0") result.push_back(Compact(line));
    }
    return result;
}
bool DirectRegister(std::string_view text, char file, std::uint32_t& index)
{
    constexpr std::string_view mask = ".xyzw";
    if (text.size() <= 1 + mask.size() || text.front() != file || text.substr(text.size() - mask.size()) != mask) return false;
    return Number(text.substr(1, text.size() - 1 - mask.size()), index);
}
bool ExactOutputConsumer(const std::string& original, const std::string& candidate,
    const ShaderSource& source, const CodeContract& baseline, const CodeContract& actual, std::string& error)
{
    // First executable contract is deliberately narrow: every original code
    // instruction stays exact, except a full float4 output MOV may become
    // [direct t120 load -> mul original source by loaded float4 -> same MOV].
    // A new scratch >= the original temp count cannot clobber game computation.
    // This proves a real output data path, not a dead parameter load, while
    // retaining original branch/discard structure and all other instructions.
    const auto before = Instructions(original), after = Instructions(candidate);
    std::size_t i = 0, j = 0;
    unsigned chains = 0;
    while (i < before.size() && j < after.size())
    {
        if (before[i] == after[j]) { ++i; ++j; continue; }
        const auto& old = before[i];
        if (old.compare(0, 3, "mov") || j + 2 >= after.size()) break;
        const auto comma = old.find(',', 3);
        if (comma == old.npos || old.find(',', comma + 1) != old.npos) break;
        const auto destination = std::string_view(old).substr(3, comma - 3);
        const auto input = std::string_view(old).substr(comma + 1);
        std::uint32_t output = 0, inputRegister = 0;
        if (!DirectRegister(destination, 'o', output) || output >= 6 ||
            !DirectRegister(input, 'r', inputRegister) || inputRegister >= baseline.temps) break;
        bool floatTarget = false;
        for (UINT n = 0; n < source.Signature().outputCount; ++n)
        {
            const auto& parameter = source.Signature().outputs[n];
            if (parameter.registerIndex == output && parameter.systemValue == D3D_NAME_TARGET &&
                parameter.component == D3D_REGISTER_COMPONENT_FLOAT32 && parameter.mask == 15) floatTarget = true;
        }
        if (!floatTarget) break;
        constexpr std::string_view opcode = "ld_indexable(texture1d)(float,float,float,float)";
        if (after[j].compare(0, opcode.size(), opcode)) break;
        const auto loadComma = after[j].find(',', opcode.size());
        if (loadComma == after[j].npos) break;
        const auto scratch = std::string_view(after[j]).substr(opcode.size(), loadComma - opcode.size());
        std::uint32_t scratchRegister = 0;
        if (!DirectRegister(scratch, 'r', scratchRegister) || scratchRegister < baseline.temps || scratchRegister >= actual.temps ||
            after[j].size() < 9 || after[j].substr(after[j].size() - 9) != "t120.xyzw") break;
        CodeContract load; load.temps = actual.temps;
        if (!Load120(after[j], load) || load.readCount != 1 || load.reads[0].components != 15) break;
        const auto multiply = "mul" + std::string(scratch) + "," + std::string(input) + "," + std::string(scratch);
        const auto write = "mov" + std::string(destination) + "," + std::string(scratch);
        if (after[j + 1] != multiply || after[j + 2] != write) break;
        ++chains; ++i; j += 3;
    }
    if (chains && i == before.size() && j == after.size()) return true;
    error = "Unsupported replacement instruction contract: preserve original code and replace a float4 output MOV only with direct t120 load, mul and same output MOV";
    return false;
}
bool CanonicalLiterals(std::string& instruction)
{
    for (std::size_t at = instruction.find("l("); at != instruction.npos; at = instruction.find("l(", at))
    {
        const auto end = instruction.find(')', at + 2);
        if (end == instruction.npos) return false;
        const auto literal = instruction.substr(at + 2, end - at - 2);
        std::string canonical;
        unsigned count = 0;
        for (std::size_t begin = 0; begin < literal.size();)
        {
            const auto comma = literal.find(',', begin);
            const auto value = std::string_view(literal).substr(begin, comma == literal.npos ? literal.size() - begin : comma - begin);
            std::uint32_t bits = 0;
            if (value.size() > 2 && value.substr(0, 2) == "0x")
            { if (!Number(value, bits)) return false; }
            else if (value.find_first_of(".eE") != value.npos)
            {
                float number = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
                if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size()) return false;
                static_assert(sizeof(number) == sizeof(bits));
                std::memcpy(&bits, &number, sizeof(bits));
            }
            else
            {
                std::int32_t number = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
                if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size()) return false;
                std::memcpy(&bits, &number, sizeof(bits));
            }
            if (++count > 4) return false;
            if (!canonical.empty()) canonical += ',';
            canonical += std::to_string(bits);
            begin = comma == literal.npos ? literal.size() : comma + 1;
        }
        if (count != 1 && count != 2 && count != 4) return false;
        if (count == 2)
        {
            // The fixed assemble_literal_operand writes four DWORDs for a
            // comma-bearing literal. The exact 69/70/169 mip0 address
            // spellings encode (index,0,0,0); compare that exact encoding
            // without rewriting the supplied template or accepting general
            // two-component literals. D3D disassembly emits all four lanes.
            constexpr std::string_view load = "ld_indexable(texture1d)(float,float,float,float)";
            if ((literal != "69,0" && literal != "70,0" && literal != "169,0") || instruction.compare(0, load.size(), load) ||
                instruction.substr(end + 1) != ",t120.xyzw") return false;
            canonical += ",0,0";
        }
        instruction.replace(at + 2, end - at - 2, canonical);
        at += canonical.size() + 3;
    }
    return true;
}

// Typed inserted operands: original instructions remain opaque and exact in
// order. The only new executable vocabulary is the Offset arithmetic/guard
// and literal t120 load; no output, relative index, sampler or hidden reader.
struct VertexOperand
{
    bool constant = false, negative = false;
    unsigned reg = 0, element = 0;
    std::string lanes;
    std::uint8_t mask = 0;
};
bool VertexOperandOf(std::string_view text, unsigned temps, const CodeContract& code,
                     bool destination, VertexOperand& out)
{
    if (!text.empty() && text.front() == '-') { out.negative = true; text.remove_prefix(1); }
    if (text.empty() || (destination && out.negative)) return false;
    const auto dot = text.find('.');
    if (dot == text.npos) return false;
    const auto name = text.substr(0, dot), lanes = text.substr(dot + 1);
    if (lanes.empty() || lanes.size() > 4) return false;
    for (char lane : lanes)
    {
        const auto index = std::string_view("xyzw").find(lane);
        if (index == std::string_view::npos) return false;
        out.mask |= static_cast<std::uint8_t>(1u << index);
    }
    out.lanes = lanes;
    if (destination)
    {
        std::string ordered;
        for (unsigned i = 0; i != 4; ++i) if (out.mask & (1u << i)) ordered += "xyzw"[i];
        if (lanes != ordered) return false;
    }
    else if (lanes.size() == 2 || lanes.size() == 3)
    {
        // Fixed local assembler handleSwizzle uses operand MASK mode for
        // 2/3-character source spellings. Preserve that encoding, never turn
        // xw into an invented xyzw swizzle. The real Offset source has xw and
        // yyy; yyy encodes the one consumed y mask. Raw text goes to assembler.
        if (lanes != "xw" && lanes != "yyy") return false;
        if (lanes == "yyy") out.lanes = "y";
    }
    if (name.empty()) return false;
    if (name.front() == 'r')
        return Number(name.substr(1), out.reg) && out.reg < temps;
    if (destination || name.substr(0, 2) != "cb") return false;
    const auto open = name.find('['), close = name.find(']');
    if (open == name.npos || close != name.size() - 1 || close <= open ||
        !Number(name.substr(2, open - 2), out.reg) || out.reg >= code.constantVectors.size() ||
        !Number(name.substr(open + 1, close - open - 1), out.element) ||
        out.element >= code.constantVectors[out.reg]) return false;
    out.constant = true;
    return true;
}
bool TypedVertexRewrite(const std::string& original, const std::string& candidate,
    const CodeContract& baseline, CodeContract& actual, std::string& error)
{
    const auto before = Instructions(original), after = Instructions(candidate);
    actual.rewriteConstantBytes = {};
    std::array<std::uint8_t, 4096> defined{}, parameter{};
    std::size_t old = 0;
    unsigned guardDepth = 0, loads = 0, consumers = 0;
    const auto reject = [&](const std::string& instruction) {
        error = "Typed VS rewrite rejected instruction: " + instruction.substr(0, 180); return false;
    };
    for (const auto& line : after)
    {
        if (old < before.size() && line == before[old])
        {
            // New guards cannot wrap or remove game instructions/branches.
            if (guardDepth) return reject(line);
            // Do not carry a newly initialized scratch through an original
            // control-flow join/call/loop. Real Offset spans contain no such
            // crossing; an unrelated regex must not infer definite assignment.
            for (const auto boundary : {"if_", "else", "endif", "loop", "endloop", "switch", "case",
                "default", "endswitch", "break", "continue", "call", "ret", "label", "interface_call"})
                if (line.compare(0, std::strlen(boundary), boundary) == 0)
                { defined = {}; parameter = {}; break; }
            ++old; continue;
        }
        constexpr std::string_view load = "ld_indexable(texture1d)(float,float,float,float)";
        if (line.compare(0, load.size(), load) == 0)
        {
            CodeContract read; read.temps = actual.temps;
            const auto comma = line.find(',', load.size());
            VertexOperand destination;
            if (guardDepth || comma == line.npos || !Load120(line, read) || read.readCount != 1 ||
                read.reads[0].vector != 169 || read.reads[0].components != 15 ||
                !VertexOperandOf(std::string_view(line).substr(load.size(), comma - load.size()),
                    actual.temps, actual, true, destination) || destination.reg < baseline.temps || destination.mask != 15)
                return reject(line);
            defined[destination.reg] = parameter[destination.reg] = 15;
            ++loads; continue;
        }
        if (line == "endif")
        { if (!guardDepth) return reject(line); --guardDepth; continue; }
        if (line.compare(0, 5, "if_nz") == 0)
        {
            VertexOperand condition;
            if (guardDepth || !VertexOperandOf(std::string_view(line).substr(5), actual.temps, actual, false, condition) ||
                !condition.constant || condition.negative || (condition.reg != 12 && condition.reg != 13) ||
                condition.element != 3 || condition.lanes != "y") return reject(line);
            actual.rewriteConstantBytes[condition.reg] = (std::max)(actual.rewriteConstantBytes[condition.reg],
                (condition.element + 1) * 16u);
            ++guardDepth; continue;
        }
        if (line.compare(0, 3, "add") && line.compare(0, 3, "mul")) return reject(line);
        const auto a = line.find(',', 3), b = a == line.npos ? a : line.find(',', a + 1);
        if (a == line.npos || b == line.npos || line.find(',', b + 1) != line.npos) return reject(line);
        VertexOperand destination, left, right;
        if (!VertexOperandOf(std::string_view(line).substr(3, a - 3), actual.temps, actual, true, destination) ||
            !VertexOperandOf(std::string_view(line).substr(a + 1, b - a - 1), actual.temps, actual, false, left) ||
            !VertexOperandOf(std::string_view(line).substr(b + 1), actual.temps, actual, false, right)) return reject(line);
        bool usesParameter = false;
        for (const auto& input : {left, right})
        {
            if (input.constant)
                actual.rewriteConstantBytes[input.reg] = (std::max)(actual.rewriteConstantBytes[input.reg],
                    (input.element + 1) * 16u);
            else
            {
                if (input.reg >= baseline.temps && (defined[input.reg] & input.mask) != input.mask) return reject(line);
                usesParameter |= (parameter[input.reg] & input.mask) != 0;
            }
        }
        if (destination.reg >= baseline.temps)
        {
            // Conditional writes alone cannot prove a scratch initialized on
            // the unexecuted branch. Conservative refusal, no default value.
            if (guardDepth) return reject(line);
            defined[destination.reg] |= destination.mask;
        }
        if (usesParameter) { parameter[destination.reg] |= destination.mask; ++consumers; }
        else parameter[destination.reg] &= static_cast<std::uint8_t>(~destination.mask);
    }
    if (old != before.size() || guardDepth || !loads || !consumers || actual.readCount != 1 ||
        actual.reads[0].vector != 169 || actual.reads[0].components != 15)
    { error = "Typed VS must retain every original instruction in order, close guards and consume real t120[169]"; return false; }
    return true;
}
bool ExactVertexAssembly(const std::string& proposed, const std::string& actual, std::string& error)
{
    auto expected = Instructions(proposed), result = Instructions(actual);
    // Model ONLY the exact source MASK spelling used by the supplied local
    // assembler, not a new swizzle/coordinate interpretation. No text mutation
    // is applied to input INIs or the raw code sent to the assembler.
    for (auto& line : expected)
        if (!line.compare(0, 3, "mul"))
        {
            const auto at = line.rfind(".yyy");
            if (at != line.npos && at + 4 == line.size()) line.erase(at + 2);
        }
    if (expected.size() == result.size())
    {
        bool same = true;
        for (std::size_t i = 0; i < expected.size(); ++i)
            if (!CanonicalLiterals(expected[i]) || !CanonicalLiterals(result[i]) || expected[i] != result[i])
            { same = false; break; }
        if (same) return true;
    }
    error = "Final VS executable differs from typed source opcode/register/mask/literal contract";
    return false;
}

bool ExactOutlineAssembly(const std::string& proposed, const std::string& actual,
                          std::uint32_t originalTemps, std::string& error)
{
    // The proposed text came ONLY from the complete content-pinned local
    // template and its real match on original bytes. Check the FINAL executable
    // instruction stream too; do not accept a new declaration as a consumer.
    auto expected = Instructions(proposed), result = Instructions(actual);
    // Fixed XXMI Assembler.cpp assembleOp (654-750): only leading |...|
    // sets the ABS operand extension. Its atoi numeric-prefix parse encodes
    // rN_abs.x as plain rN.x. Do NOT rewrite the shader/template to abs or
    // strip the supplied spelling before assembly. Compare that one exact
    // pinned-template operand against the ACTUAL disassembled encoding here.
    // General suffix aliases and other instructions remain unsupported.
    const auto msg = "r" + std::to_string(originalTemps + 3);
    const auto ini69 = "r" + std::to_string(originalTemps + 1);
    const auto supplied = "ftou" + msg + ".x," + ini69 + "_abs.x";
    const auto encoded = "ftou" + msg + ".x," + ini69 + ".x";
    unsigned prefixOperands = 0;
    for (auto& instruction : expected) if (instruction.find("_abs") != instruction.npos)
    {
        if (instruction != supplied || ++prefixOperands != 1)
        { error = "Outline suffix is outside the fixed source numeric-prefix operand contract"; return false; }
        instruction = encoded; // comparison model only; raw source went to assembler
    }
    if (prefixOperands != 1)
    { error = "Exact Outline source lacks its fixed numeric-prefix operand"; return false; }
    if (expected.size() == result.size())
    {
        bool same = true;
        for (std::size_t n = 0; n < expected.size(); ++n)
            if (!CanonicalLiterals(expected[n]) || !CanonicalLiterals(result[n]) || expected[n] != result[n])
            { same = false; break; }
        if (same) return true;
    }
    error = "Final executable differs from the exact Outline instruction/operand contract; no inferred modifier/swizzle/opcode is accepted";
    return false;
}
bool ExactDiffuseAssembly(const std::string& proposed, const std::string& actual, std::string& error)
{
    auto expected = Instructions(proposed), result = Instructions(actual);
    if (expected.size() == result.size())
    {
        bool same = true;
        for (std::size_t n = 0; n < expected.size(); ++n)
            if (!CanonicalLiterals(expected[n]) || !CanonicalLiterals(result[n]) || expected[n] != result[n])
            { same = false; break; }
        if (same) return true;
    }
    error = "Final executable differs from exact Diffuse opcode/register/swizzle/literal contract";
    return false;
}
bool DiffuseOriginalSlots(const CodeContract& baseline)
{
    for (const auto& declaration : baseline.declarations)
        for (const auto reserved : {std::string_view("t69"), std::string_view("t72"), std::string_view("o6")})
            for (auto at = declaration.find(reserved); at != declaration.npos; at = declaration.find(reserved, at + 1))
            {
                const auto end = at + reserved.size();
                if (end == declaration.size() || declaration[end] < '0' || declaration[end] > '9') return false;
            }
    return true;
}
bool OutlineOriginalSlots(const CodeContract& baseline)
{
    for (const auto& declaration : baseline.declarations)
    {
        // Resource declarations end in tN; sampler declarations contain sN,mode.
        // These slots must be absent in the actual ORIGINAL code, not just in
        // stale RDEF metadata. No replacement of an existing game binding ABI.
        const auto t = declaration.rfind('t');
        std::uint32_t index = 0;
        if (t != declaration.npos)
        {
            auto end = t + 1;
            while (end < declaration.size() && declaration[end] >= '0' && declaration[end] <= '9') ++end;
            if (Number(std::string_view(declaration).substr(t + 1, end - t - 1), index) &&
                (index == 69 || index == 70)) return false;
        }
        if (declaration.compare(0, 11, "dcl_sampler") == 0)
        {
            const auto comma = declaration.find(',', 12);
            if (comma == declaration.npos || !Number(std::string_view(declaration).substr(12, comma - 12), index) || index == 15)
                return false;
        }
    }
    return true;
}
void TempReplace(std::string& text, const std::string& name, const std::string& replacement)
{
    for (const auto& search : {"${" + name + "}", "$" + name})
        for (auto pos = text.find(search); pos != text.npos; pos = text.find(search, pos + replacement.size()))
        {
            if (pos && (text[pos - 1] == '$' || text[pos - 1] == '\\')) continue;
            // Unbraced names must be a whole name (temp r0 must not eat r01).
            if (search[1] != '{' && pos + search.size() < text.size())
            {
                const char ch = text[pos + search.size()];
                if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_') continue;
            }
            text.replace(pos, search.size(), replacement);
        }
}
std::size_t DeclarationBoundary(const std::string& text)
{
    std::size_t boundary = std::string::npos;
    int braces = 0;
    for (std::size_t begin = 0; begin < text.size();)
    {
        const auto end = text.find('\n', begin);
        if (end == text.npos) break;
        auto line = Trim(std::string_view(text).substr(begin, end - begin));
        const auto comment = line.find("//");
        if (comment != line.npos) line = Trim(std::string_view(line).substr(0, comment));
        if (!line.empty())
        {
            if (!braces && line != "ps_5_0" && line != "vs_5_0" && line.compare(0, 4, "dcl_") != 0) break;
            for (char ch : line) { if (ch == '{') ++braces; else if (ch == '}') --braces; }
            if (braces < 0) return text.npos;
            if (!braces) boundary = end + 1;
        }
        begin = end + 1;
    }
    return braces ? text.npos : boundary;
}
bool Declarations(std::string& text, const ShaderRegexGroup& group, unsigned temps, std::string& error)
{
    if (temps > 4096 || group.temps.size() > 4096 - temps) { error = "Shader temporary register limit exceeded"; return false; }
    if (!group.temps.empty())
    {
        const auto at = text.find("\ndcl_temps ");
        if (at != text.npos)
        {
            const auto end = text.find('\n', at + 1);
            if (end == text.npos) { error = "Missing shader temporary declaration boundary"; return false; }
            text.replace(at + 1, end - at - 1, "dcl_temps " + std::to_string(temps + group.temps.size()));
        }
        else
        {
            const auto boundary = DeclarationBoundary(text);
            if (boundary == text.npos) { error = "Missing shader declaration boundary"; return false; }
            text.insert(boundary, "dcl_temps " + std::to_string(temps + group.temps.size()) + "\n");
        }
    }
    for (const auto& declaration : group.declarations)
    {
        bool exists = false;
        for (const auto& line : Lines(text)) exists |= Compact(line) == Compact(declaration);
        if (exists) continue;
        // Compiler 46/47 may spell the SAME original slot CB/cb. Do not
        // insert a duplicate or silently resize/change indexing on that slot.
        vertex::ConstantDeclaration incoming;
        if (vertex::Constant(Compact(declaration), incoming))
            for (const auto& line : Lines(text))
            {
                vertex::ConstantDeclaration current;
                if (!vertex::Constant(Compact(line), current) || current.slot != incoming.slot) continue;
                if (current.vectors != incoming.vectors || current.dynamic != incoming.dynamic)
                { error = "Inserted VS constant declaration conflicts with the original slot/range/indexing"; return false; }
                exists = true; break;
            }
        if (exists) continue;
        const auto boundary = DeclarationBoundary(text);
        if (boundary == text.npos) { error = "Missing declaration insertion boundary"; return false; }
        text.insert(boundary, declaration + "\n");
    }
    return text.size() <= TextLimit;
}
} // namespace
struct ShaderRegexLibrary::Impl
{
    struct Rule { Id program = InvalidId; const ShaderRegexPattern* source = nullptr; Code code; };
    std::shared_ptr<const Plan> plan;
    std::vector<Rule> rules;
};
std::shared_ptr<const ShaderRegexLibrary> ShaderRegexLibrary::Compile(std::shared_ptr<const Plan> plan, std::vector<Diagnostic>& diagnostics)
{
    try
    {
        auto impl = std::make_shared<Impl>(); impl->plan = std::move(plan);
        if (!impl->plan) return {};
        std::size_t bytes = 0;
        for (const auto& group : impl->plan->ShaderRegexGroups())
            for (const auto& pattern : group.patterns)
            {
                if (impl->rules.size() == GimiCachedMatches) throw std::runtime_error("Bounded compiled pattern capacity exceeded");
                int error = 0; PCRE2_SIZE offset = 0;
                Code code(pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.pattern.data()), pattern.pattern.size(),
                    PCRE2_CASELESS | PCRE2_MULTILINE, &error, &offset, nullptr));
                if (!code)
                { diagnostics.push_back({DiagnosticCode::InvalidSyntax, pattern.source, "PCRE2 offset " + std::to_string(offset) + ": " + PcreError(error)}); return {}; }
                PCRE2_SIZE size = 0;
                if (pcre2_pattern_info(code.get(), PCRE2_INFO_SIZE, &size) || size > RegexCodeLimit - bytes)
                { diagnostics.push_back({DiagnosticCode::Limit, pattern.source, "Compiled PCRE2 code budget exceeded"}); return {}; }
                bytes += size;
                std::uint32_t count = 0, stride = 0; PCRE2_SPTR names = nullptr;
                if (pcre2_pattern_info(code.get(), PCRE2_INFO_NAMECOUNT, &count) ||
                    pcre2_pattern_info(code.get(), PCRE2_INFO_NAMEENTRYSIZE, &stride) ||
                    pcre2_pattern_info(code.get(), PCRE2_INFO_NAMETABLE, &names))
                    throw std::runtime_error("PCRE2 named capture query failed");
                for (std::uint32_t i = 0; i < count; ++i)
                    if (std::find(group.temps.begin(), group.temps.end(), reinterpret_cast<const char*>(names + i * stride + 2)) != group.temps.end())
                    { diagnostics.push_back({DiagnosticCode::InvalidSyntax, pattern.source, "Named capture overlaps a shader temporary"}); return {}; }
                impl->rules.push_back({group.program, &pattern, std::move(code)});
            }
        return std::shared_ptr<const ShaderRegexLibrary>(new ShaderRegexLibrary(std::move(impl)));
    }
    catch (const std::exception& exception) { diagnostics.push_back({DiagnosticCode::Unsupported, {}, exception.what()}); return {}; }
}
bool Dx11PixelReplacement::ValidFor(ID3D11Device* device, ID3D11PixelShader* original, ShaderIdentity identity,
                                    std::uint64_t plan, std::uint64_t deviceGeneration) const noexcept
{
    if (!source_ || !device || device_.Get() != device || !shader_ || !original || original_.Get() != original ||
        !plan || plan == UINT64_MAX || plan_ != plan || !lease_ || lease_ == UINT64_MAX || !readCount_ || readCount_ > reads_.size() ||
        source_->Model() != ShaderModel::PS50 || identity.knowledge != Knowledge::Known || identity.generation != deviceGeneration) return false;
    const auto& captured = source_->Original();
    return captured.knowledge == Knowledge::Known && captured.object == identity.object && captured.hash == identity.hash &&
        captured.generation == identity.generation && identity.object && identity.object != UINT64_MAX && identity.generation && identity.generation != UINT64_MAX;
}
struct ShaderRegexLibrary::Prepared
{
    ShaderEntryLink link;
    std::shared_ptr<const Dx11PixelReplacement> pixel;
    std::shared_ptr<const Dx11VertexReplacement> vertex;
    std::vector<Diagnostic> diagnostics;
};
PreparedPixelShader ShaderRegexLibrary::Prepare(std::shared_ptr<const ShaderSource> source, ShaderIdentity original,
    ShaderModel model, ID3D11Device* device, ID3D11PixelShader* originalShader, std::uint64_t plan,
    std::uint64_t lease) const
{
    auto prepared = PrepareStage(std::move(source), original, model, device, originalShader, plan, lease, false);
    return {std::move(prepared.link), std::move(prepared.pixel), std::move(prepared.diagnostics)};
}
PreparedVertexShader ShaderRegexLibrary::PrepareVertex(std::shared_ptr<const ShaderSource> source, ShaderIdentity original,
    ShaderModel model, ID3D11Device* device, ID3D11VertexShader* originalShader, std::uint64_t plan,
    std::uint64_t lease) const
{
    auto prepared = PrepareStage(std::move(source), original, model, device, originalShader, plan, lease, true);
    return {std::move(prepared.link), std::move(prepared.vertex), std::move(prepared.diagnostics)};
}
bool Dx11VertexReplacement::ValidFor(ID3D11Device* device, ID3D11VertexShader* original, ShaderIdentity identity,
    std::uint64_t plan, std::uint64_t deviceGeneration) const noexcept
{
    if (!source_ || !device || device_.Get() != device || !shader_ || !original || original_.Get() != original ||
        !plan || plan == UINT64_MAX || plan_ != plan || !lease_ || lease_ == UINT64_MAX || readCount_ != 1 ||
        reads_[0].vector != 169 || reads_[0].components != 15 || source_->Model() != ShaderModel::VS50 ||
        identity.knowledge != Knowledge::Known || identity.generation != deviceGeneration) return false;
    const auto& captured = source_->Original();
    return captured.knowledge == Knowledge::Known && captured.object == identity.object && captured.hash == identity.hash &&
        captured.generation == identity.generation && identity.object && identity.object != UINT64_MAX &&
        identity.generation && identity.generation != UINT64_MAX;
}
ShaderRegexLibrary::Prepared ShaderRegexLibrary::PrepareStage(std::shared_ptr<const ShaderSource> source, ShaderIdentity original,
    ShaderModel model, ID3D11Device* device, ID3D11DeviceChild* originalShader, std::uint64_t planGeneration,
    std::uint64_t replacementLease, bool vertexStage) const
{
    Prepared result;
    const Target stage{vertexStage ? TargetKind::VertexShader : TargetKind::PixelShader, 0};
    std::array<std::string, 14> constantLabels, readLabels;
    std::array<UINT, 14> requiredRanges{};
    const auto label = [](const SourceLocation& source) {
        const auto path = source.file.u8string();
        return std::string(reinterpret_cast<const char*>(path.data()), path.size()) + ":" + std::to_string(source.line);
    };
    const auto& plan = *impl_->plan;
    SourceLocation at;
    const auto fail = [&](std::string error) { result.diagnostics.push_back({DiagnosticCode::Unsupported, at, std::move(error)}); };
    try
    {
        std::array<Id, GimiCachedMatches> matched{}; std::uint32_t count = 0;
        bool needsText = false;
        for (const auto& group : plan.ShaderRegexGroups())
            if (group.RequiresBytecode() && std::find(group.models.begin(), group.models.end(), model) != group.models.end())
            { needsText = true; at = group.source; break; }
        if (!needsText)
        {
            auto entry = LinkShaderEntry(plan, original, model, stage);
            result.link = std::move(entry.link); result.diagnostics = std::move(entry.diagnostics); return result;
        }
        if (!source || model != (vertexStage ? ShaderModel::VS50 : ShaderModel::PS50) || source->Model() != model || !originalShader || !device ||
            source->Original().object != original.object || source->Original().hash != original.hash ||
            source->Original().generation != original.generation || !planGeneration || planGeneration == UINT64_MAX ||
            !replacementLease || replacementLease == UINT64_MAX)
        { fail("Required authentic stage-specific 5.0 source/lease/generation unavailable; no Pattern inferred"); return result; }
        Microsoft::WRL::ComPtr<ID3D11Device> actualDevice; originalShader->GetDevice(&actualDevice);
        if (actualDevice.Get() != device) { fail("Original shader does not belong to the preparing device"); return result; }
        std::string text, error;
        if (!dxbc::Disassemble(source->Bytes(), text, error)) { fail(error); return result; }
        const auto originalText = text;
        CodeContract baseline;
        if (!DescribeCode(text, false, baseline, error, false, false, vertexStage)) { fail(error); return result; }
        Context context(pcre2_match_context_create(nullptr));
        if (!context) { fail("PCRE2 match context allocation failed"); return result; }
        pcre2_set_match_limit(context.get(), 500000);
        pcre2_set_depth_limit(context.get(), 256);
        pcre2_set_heap_limit(context.get(), 16384); // KiB, one preparation operation
        bool changed = false, exactOutline = false, exactDiffuse = false;
        SourceLocation changedAt;
        for (const auto& group : plan.ShaderRegexGroups())
        {
            if (!group.RequiresBytecode() || std::find(group.models.begin(), group.models.end(), model) == group.models.end()) continue;
            at = group.source;
            auto candidate = text; bool groupMatched = true, groupPatched = false, groupReplacementMatched = false;
            // Temp allocation must use the current text, including earlier
            // successful groups. A failed group discards its COMPLETE candidate.
            unsigned temps = 0;
            for (const auto& line : Lines(candidate)) if (line.compare(0, 10, "dcl_temps ") == 0)
                if (!Number(Trim(std::string_view(line).substr(10)), temps) || temps > 4096)
                { fail("Invalid current shader temporary count"); return result; }
            for (const auto& rule : impl_->rules) if (rule.program == group.program)
            {
                at = rule.source->source;
                Match match(pcre2_match_data_create_from_pattern(rule.code.get(), nullptr));
                if (!match) { fail("PCRE2 match data allocation failed"); return result; }
                const auto rc = pcre2_match(rule.code.get(), reinterpret_cast<PCRE2_SPTR>(candidate.data()), candidate.size(), 0, 0, match.get(), context.get());
                if (rc == PCRE2_ERROR_NOMATCH) { groupMatched = false; break; }
                if (rc < 0) { fail("PCRE2 match: " + PcreError(rc)); return result; }
                if (!rule.source->replace) continue;
                groupReplacementMatched = true;
                at = rule.source->replacementSource;
                auto replacement = rule.source->replacement;
                for (std::size_t i = 0; i < group.temps.size(); ++i) TempReplace(replacement, group.temps[i], "r" + std::to_string(temps + i));
                std::vector<PCRE2_UCHAR> output((std::min)(TextLimit + 1, candidate.size() + replacement.size() + 1024));
                PCRE2_SIZE length = output.size();
                auto replaced = pcre2_substitute(rule.code.get(), reinterpret_cast<PCRE2_SPTR>(candidate.data()), candidate.size(), 0,
                    PCRE2_SUBSTITUTE_EXTENDED | PCRE2_SUBSTITUTE_OVERFLOW_LENGTH, match.get(), context.get(),
                    reinterpret_cast<PCRE2_SPTR>(replacement.data()), replacement.size(), output.data(), &length);
                if (replaced == PCRE2_ERROR_NOMEMORY && length <= TextLimit)
                {
                    output.resize(length + 1); length = output.size();
                    replaced = pcre2_substitute(rule.code.get(), reinterpret_cast<PCRE2_SPTR>(candidate.data()), candidate.size(), 0,
                        PCRE2_SUBSTITUTE_EXTENDED, match.get(), context.get(), reinterpret_cast<PCRE2_SPTR>(replacement.data()), replacement.size(), output.data(), &length);
                }
                if (replaced != 1 || length > TextLimit)
                { fail("PCRE2 bounded single substitution failed: " + (replaced < 0 ? PcreError(replaced) : "expected one replacement")); return result; }
                candidate.assign(reinterpret_cast<char*>(output.data()), length);
                groupPatched = true;
            }
            if (!groupMatched) continue;
            if (groupPatched || !group.declarations.empty())
            {
                if (!Declarations(candidate, group, temps, error)) { fail(error.empty() ? "Shader assembly text bound exceeded" : error); return result; }
                if (groupReplacementMatched && candidate == text)
                { fail("Pattern.Replace matched but produced no changed shader code; no consumer is published"); return result; }
                if (candidate != text)
                {
                    if (vertexStage)
                    {
                        CodeContract typed;
                        if (!DescribeCode(candidate, true, typed, error, false, false, true, &baseline) ||
                            typed.declarations != baseline.declarations || typed.temps < baseline.temps ||
                            !TypedVertexRewrite(originalText, candidate, baseline, typed, error))
                        { fail(error.empty() ? "VS rewrite changes original declarations/signature" : error); return result; }
                        for (unsigned slot = 0; slot < typed.rewriteConstantBytes.size(); ++slot)
                            if (typed.rewriteConstantBytes[slot] > requiredRanges[slot])
                            {
                                requiredRanges[slot] = typed.rewriteConstantBytes[slot];
                                readLabels[slot] = label(at);
                            }
                        for (const auto& member : group.sections)
                            if (LowerAscii(member.name).ends_with(".insertdeclarations"))
                                for (const auto& declaration : member.lines)
                                {
                                    vertex::ConstantDeclaration constant;
                                    if (vertex::Constant(Compact(declaration.raw), constant) &&
                                        typed.addedConstantBytes[constant.slot] && constantLabels[constant.slot].empty())
                                        constantLabels[constant.slot] = label(declaration.source);
                                }
                    }
                    const bool outlineGroup = !vertexStage && outline::ExactTemplate(group);
                    const bool diffuseGroup = !vertexStage && diffuse::ExactTemplate(group);
                    if (((outlineGroup || diffuseGroup) && changed) || ((exactOutline || exactDiffuse) && !outlineGroup && !diffuseGroup))
                    { fail("Exact Outline/Diffuse cannot be composed with another bytecode transformation"); return result; }
                    if (outlineGroup)
                    {
                        if (!OutlineOriginalSlots(baseline))
                        { fail("Original shader already occupies an Outline t69/t70/s15 slot; no conflict is overridden"); return result; }
                        // Preserve the exact supplied operand spelling through
                        // assembly. ExactOutlineAssembly verifies the pinned
                        // source's real no-ABS encoding, not presumed intent.
                    }
                    if (diffuseGroup && !DiffuseOriginalSlots(baseline))
                    { fail("Original shader occupies reserved Diffuse t69/t72/o6 interface"); return result; }
                    changed = true; exactOutline = outlineGroup; exactDiffuse = diffuseGroup; changedAt = at;
                }
            }
            if (count == matched.size()) { fail("Pattern match-link capacity exceeded"); return result; }
            matched[count++] = group.program; text = std::move(candidate);
        }
        const ShaderGroupMatches prepared{matched.data(), count};
        auto entry = LinkShaderEntry(plan, original, model, stage, &prepared);
        if (!entry) { result.diagnostics = std::move(entry.diagnostics); return result; }
        if (changed && text == originalText)
        { fail("Matched transformations cancel to the original shader; no replacement consumer is published"); return result; }
        if (changed)
        {
            at = changedAt;
            CodeContract proposed;
            if (!DescribeCode(text, true, proposed, error, exactOutline, exactDiffuse, vertexStage, vertexStage ? &baseline : nullptr) || proposed.declarations != baseline.declarations ||
                proposed.temps < baseline.temps || ((exactOutline || exactDiffuse) && proposed.temps != baseline.temps + (exactOutline ? outline::Temps.size() : diffuse::Temps.size())))
            { fail(error.empty() ? "Patch changes original input/output/resource declarations; MRT/new resource ABI is unsupported" : error); return result; }
            if (vertexStage ? !TypedVertexRewrite(originalText, text, baseline, proposed, error) :
                (!exactOutline && !exactDiffuse && !ExactOutputConsumer(originalText, text, *source, baseline, proposed, error)))
            { fail(error); return result; }
            std::vector<std::uint8_t> bytes;
            PixelTarget6Signature target6;
            if (exactDiffuse && !PreparePixelTarget6Signature(*source, target6))
            { fail("Original signature cannot accept the exact added float4 SV_Target6"); return result; }
            if (!(exactDiffuse ? dxbc::AssemblePixelTarget6(text, source->Bytes(), target6, bytes, error) :
                dxbc::AssemblePreservingSignatures(text, source->Bytes(), bytes, error))) { fail(error); return result; }
            std::string actualText;
            CodeContract actual;
            if (!dxbc::Disassemble(bytes, actualText, error) || !DescribeCode(actualText, true, actual, error, exactOutline, exactDiffuse, vertexStage, vertexStage ? &baseline : nullptr) ||
                actual.declarations != baseline.declarations || actual.temps != proposed.temps ||
                (vertexStage && actual.addedConstantBytes != proposed.addedConstantBytes))
            { fail(error.empty() ? "Assembled code changed the original interface" : error); return result; }
            if (vertexStage ? (!ExactVertexAssembly(text, actualText, error) ||
                !TypedVertexRewrite(originalText, actualText, baseline, actual, error)) :
                exactOutline ? !ExactOutlineAssembly(text, actualText, baseline.temps, error) : exactDiffuse ?
                !ExactDiffuseAssembly(text, actualText, error) : !ExactOutputConsumer(originalText, actualText, *source, baseline, actual, error))
            { fail(error); return result; }
            Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
            if (FAILED(D3DReflect(bytes.data(), bytes.size(), IID_PPV_ARGS(&reflection))) || !(exactDiffuse ? MatchesPixelTarget6Signature(target6, *reflection.Get()) : MatchesOriginalShaderSignature(*source, *reflection.Get())))
            { fail("Final DXBC real signature differs from the exact original or requested Target6 contract"); return result; }
            if (vertexStage)
            {
                auto replacement = std::make_shared<Dx11VertexReplacement>();
                if (FAILED(originalShader->QueryInterface(IID_PPV_ARGS(&replacement->original_))))
                { fail("Authentic original VS interface unavailable"); return result; }
                const auto hr = device->CreateVertexShader(bytes.data(), bytes.size(), nullptr, &replacement->shader_);
                if (FAILED(hr) || !replacement->shader_)
                { fail("Authentic device CreateVertexShader failed HRESULT=" + std::to_string(static_cast<std::uint32_t>(hr))); return result; }
                replacement->source_ = std::move(source); replacement->device_ = device;
                replacement->plan_ = planGeneration; replacement->lease_ = replacementLease;
                replacement->reads_ = actual.reads; replacement->readCount_ = actual.readCount;
                replacement->addedConstantBytes_ = actual.addedConstantBytes;
                replacement->requiredConstantBytes_ = actual.rewriteConstantBytes;
                for (unsigned slot = 0; slot < actual.addedConstantBytes.size(); ++slot)
                    replacement->requiredConstantBytes_[slot] = (std::max)(replacement->requiredConstantBytes_[slot],
                        actual.addedConstantBytes[slot]);
                replacement->sourceLabel_ = label(changedAt);
                for (unsigned slot = 0; slot < constantLabels.size(); ++slot)
                    if (constantLabels[slot].empty()) constantLabels[slot] = std::move(readLabels[slot]);
                replacement->constantLabels_ = std::move(constantLabels);
                result.vertex = std::move(replacement);
            }
            else
            {
                auto replacement = std::make_shared<Dx11PixelReplacement>();
                const auto hr = device->CreatePixelShader(bytes.data(), bytes.size(), nullptr, &replacement->shader_);
                if (FAILED(hr) || !replacement->shader_)
                { fail("Authentic device CreatePixelShader failed HRESULT=" + std::to_string(static_cast<std::uint32_t>(hr))); return result; }
                replacement->source_ = std::move(source); replacement->device_ = device;
                if (FAILED(originalShader->QueryInterface(IID_PPV_ARGS(&replacement->original_))))
                { fail("Authentic original PS interface unavailable"); return result; }
                replacement->plan_ = planGeneration; replacement->lease_ = replacementLease;
                replacement->reads_ = actual.reads; replacement->readCount_ = actual.readCount;
                replacement->outlineResources_ = exactOutline; replacement->diffuseResources_ = exactDiffuse;
                result.pixel = std::move(replacement);
            }
        }
        result.link = std::move(entry.link);
        return result;
    }
    catch (const std::exception& exception) { fail(exception.what()); return result; }
    catch (...) { fail("Shader consumer preparation failed"); return result; }
}
} // namespace gimi
