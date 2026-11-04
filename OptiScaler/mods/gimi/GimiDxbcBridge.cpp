#include "GimiDxbcBridge.h"
#include "GimiShaderSource.h"
// Preserve the legacy Windows byte ABI in this modern C++ bridge.
namespace gimi_dxbc_vendor { using ::byte; }
#include "third_party/dxbc/stdafx.h"
#include <mutex>
namespace gimi { namespace dxbc {
namespace {
std::mutex toolMutex; // vendor opcode maps are mutable; never call on a draw
bool Container(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.size() < 36 || bytes.size() > 1024u * 1024u || std::memcmp(bytes.data(), "DXBC", 4)) return false;
    const auto word = [&](std::size_t n) { std::uint32_t value; std::memcpy(&value, bytes.data() + n, 4); return value; };
    if (word(20) != 1 || word(24) != bytes.size()) return false;
    const auto count = word(28);
    if (!count || count > 128 || count > (bytes.size() - 32) / 4) return false;
    unsigned code = 0;
    std::size_t precedingEnd = 32 + count * 4;
    for (unsigned i = 0; i < count; ++i)
    {
        const auto offset = word(32 + 4 * i);
        if ((offset & 3) || offset < 32 + count * 4 || offset > bytes.size() - 8) return false;
        const auto length = word(offset + 4);
        if (length > bytes.size() - offset - 8 || offset < precedingEnd) return false;
        // The fixed assembler adjusts later table entries by table index.
        // Unordered/overlapping chunks are therefore outside its contract.
        precedingEnd = offset + 8 + length;
        if (!std::memcmp(bytes.data() + offset, "SHDR", 4) || !std::memcmp(bytes.data() + offset, "SHEX", 4))
        {
            ++code;
            if (length < 8 || (length & 3) || word(offset + 12) != length / 4) return false;
            // Exact pixel/vertex 5.0 only. The assembler is shared, but the
            // callers must prove their separate typed reader/signature ABI.
            if (word(offset + 8) != 0x50 && word(offset + 8) != 0x10050) return false;
            const auto end = offset + 8 + length;
            for (std::size_t at = offset + 16; at < end;)
            {
                const auto token = word(at);
                auto words = (token >> 24) & 0x7f;
                if ((token & 0x7ff) == 0x35) // custom data has a second length token
                { if (end - at < 8) return false; words = word(at + 4); if (words < 2) return false; }
                if (!words || words > (end - at) / 4) return false;
                at += words * 4;
            }
        }
    }
    return code == 1;
}
bool AddTarget6(const std::vector<std::uint8_t>& original, const PixelTarget6Signature& contract,
                std::vector<std::uint8_t>& seeded)
{
    if (!contract.valid || !contract.expected.outputCount) return false;
    auto word = [&](std::size_t at) { std::uint32_t v; std::memcpy(&v, original.data() + at, 4); return v; };
    const auto chunks = word(28);
    unsigned output = chunks, stride = 0, prefix = 0;
    for (unsigned i = 0; i < chunks; ++i)
    {
        const auto at = word(32 + i * 4);
        unsigned s = 0, p = 0;
        if (!std::memcmp(original.data() + at, "OSGN", 4)) s = 24;
        else if (!std::memcmp(original.data() + at, "OSG1", 4)) { s = 32; p = 4; }
        else if (!std::memcmp(original.data() + at, "OSG5", 4)) return false; // PS5.0 never needs geometry-stream ABI
        if (s) { if (output != chunks) return false; output = i; stride = s; prefix = p; }
    }
    if (output == chunks) return false;
    const auto at = word(32 + output * 4), length = word(at + 4);
    if (length < 8) return false;
    const auto count = word(at + 8);
    if (word(at + 12) != 8 || count != contract.expected.outputCount - 1 || count > 64 || count > (length - 8) / stride) return false;
    const auto entriesEnd = 8 + count * stride;
    std::vector<std::uint8_t> payload(length + stride, 0);
    std::memcpy(payload.data(), original.data() + at + 8, entriesEnd);
    std::memcpy(payload.data() + entriesEnd + stride, original.data() + at + 8 + entriesEnd, length - entriesEnd);
    auto put = [&](std::size_t offset, std::uint32_t value) { std::memcpy(payload.data() + offset, &value, 4); };
    put(0, count + 1);
    for (unsigned i = 0; i < count; ++i)
    {
        const auto field = 8 + i * stride + prefix;
        const auto name = word(at + 8 + field);
        if (name < entriesEnd || name >= length ||
            !std::memchr(original.data() + at + 8 + name, 0, length - name)) return false;
        put(field, name + stride);
    }
    const auto& added = contract.expected.outputs[count];
    if (added.registerIndex != 6 || added.semanticIndex != 6 || added.systemValue != D3D_NAME_TARGET ||
        added.component != D3D_REGISTER_COMPONENT_FLOAT32 || added.mask != 15 || added.readWriteMask || added.stream ||
        added.precision != D3D_MIN_PRECISION_DEFAULT || std::strcmp(added.semantic.data(), "SV_Target")) return false;
    const auto entry = entriesEnd + prefix;
    put(entry, static_cast<std::uint32_t>(payload.size()));
    // Raw OSGN/OSG1 encodes SV_Target as system value 0 (fixed
    // SignatureParser.cpp system_value_abbreviations); D3DReflect reports TARGET.
    put(entry + 4, 6); put(entry + 8, 0); put(entry + 12, D3D_REGISTER_COMPONENT_FLOAT32); put(entry + 16, 6);
    payload[entry + 20] = 15;
    constexpr char semantic[] = "SV_Target";
    payload.insert(payload.end(), semantic, semantic + sizeof(semantic));
    while (payload.size() & 3) payload.push_back(0);
    const auto delta = payload.size() - length;
    seeded = original; seeded.erase(seeded.begin() + at + 8, seeded.begin() + at + 8 + length);
    seeded.insert(seeded.begin() + at + 8, payload.begin(), payload.end());
    auto set = [&](std::size_t offset, std::uint32_t value) { std::memcpy(seeded.data() + offset, &value, 4); };
    set(at + 4, static_cast<std::uint32_t>(payload.size())); set(24, static_cast<std::uint32_t>(seeded.size()));
    for (unsigned i = output + 1; i < chunks; ++i) set(32 + i * 4, static_cast<std::uint32_t>(word(32 + i * 4) + delta));
    // The existing assembler recomputes its mandatory container checksum.
    return Container(seeded);
}
struct CacheScope
{
    CacheScope() { gimi_dxbc_vendor::ResetInstructionCache(); }
    ~CacheScope() { gimi_dxbc_vendor::ResetInstructionCache(); }
};
}
bool Disassemble(const std::vector<std::uint8_t>& bytes, std::string& text, std::string& error)
{
    text.clear();
    if (!Container(bytes)) { error = "Unsupported/malformed PS5.0/VS5.0 DXBC container"; return false; }
    std::lock_guard<std::mutex> lock(toolMutex);
    try
    {
        CacheScope cache;
        auto copy = bytes;
        std::vector<std::uint8_t> result;
        const auto hr = gimi_dxbc_vendor::disassembler(&copy, &result, nullptr, 0, true, true, false);
        if (hr != S_OK || result.empty() || result.size() > 4u * 1024u * 1024u)
        { error = "DXBC disassembly failed or exceeded text bound"; return false; }
        if (result.back() == 0) result.pop_back();
        text.assign(result.begin(), result.end());
        return true;
    }
    catch (const std::exception& exception) { error = exception.what(); return false; }
    catch (...) { error = "DXBC disassembly exception"; return false; }
}
static bool Assemble(const std::string& text, const std::vector<std::uint8_t>& original,
                     const PixelTarget6Signature* target6, std::vector<std::uint8_t>& bytes, std::string& error)
{
    bytes.clear();
    if (!Container(original) || text.empty() || text.size() > 4u * 1024u * 1024u || text.find('\0') != std::string::npos)
    { error = "Unsupported original DXBC or assembly text bound"; return false; }
    std::lock_guard<std::mutex> lock(toolMutex);
    try
    {
        std::vector<char> assembly(text.begin(), text.end());
        assembly.push_back(0); // vendor splitter accepts a terminated assembly
        CacheScope cache;
        // Re-prime exact opaque/precision-corrected original instruction maps
        // inside THIS operation's lock; another preparation cannot replace them.
        auto copy = original;
        std::vector<std::uint8_t> originalText;
        if (gimi_dxbc_vendor::disassembler(&copy, &originalText, nullptr, 0, true, true, false) != S_OK ||
            originalText.empty() || originalText.size() > 4u * 1024u * 1024u)
        { error = "Original instruction-map preparation failed"; return false; }
        // Null error collector is intentional: never continue after a parse
        // error and mutate a container with a partial/empty instruction vector.
        std::vector<std::uint8_t> seeded;
        if (target6 && !AddTarget6(original, *target6, seeded))
        { error = "Unsupported real PS output-signature container for Target6 extension"; return false; }
        bytes = gimi_dxbc_vendor::assembler(&assembly, target6 ? seeded : original, nullptr);
        if (!Container(bytes)) { error = "Assembler did not return a bounded PS5.0/VS5.0 DXBC"; bytes.clear(); return false; }
        return true;
    }
    catch (const std::exception& exception) { error = exception.what(); bytes.clear(); return false; }
    catch (...) { error = "DXBC assembly exception"; bytes.clear(); return false; }
}
bool AssemblePreservingSignatures(const std::string& text, const std::vector<std::uint8_t>& original,
                                 std::vector<std::uint8_t>& bytes, std::string& error)
{ return Assemble(text, original, nullptr, bytes, error); }
bool AssemblePixelTarget6(const std::string& text, const std::vector<std::uint8_t>& original,
                         const PixelTarget6Signature& target6, std::vector<std::uint8_t>& bytes, std::string& error)
{ return Assemble(text, original, &target6, bytes, error); }
} }
