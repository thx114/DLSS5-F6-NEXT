#pragma once
#include "GimiPlan.h"
#include <charconv>

namespace gimi::vertex
{
// Declaration parsing is shared by plan preparation and FINAL executable
// inspection. No declaration or filename by itself grants a shader capability.
struct ConstantDeclaration
{
    unsigned slot = 0, vectors = 0;
    bool dynamic = false;
};
inline bool Unsigned(std::string_view text, unsigned& value) noexcept
{
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc() && result.ptr == text.data() + text.size();
}
inline bool Constant(std::string_view text, ConstantDeclaration& out) noexcept
{
    constexpr std::string_view prefix = "dcl_constantbuffer";
    if (text.substr(0, prefix.size()) != prefix) return false;
    text.remove_prefix(prefix.size());
    if (text.substr(0, 2) != "CB" && text.substr(0, 2) != "cb") return false;
    text.remove_prefix(2);
    const auto open = text.find('['), close = text.find(']');
    if (open == text.npos || close == text.npos || close <= open ||
        !Unsigned(text.substr(0, open), out.slot) || out.slot >= 14 ||
        !Unsigned(text.substr(open + 1, close - open - 1), out.vectors) ||
        !out.vectors || out.vectors > 4096) return false;
    const auto suffix = text.substr(close + 1);
    if (suffix != ",immediateIndexed" && suffix != ",dynamicIndexed") return false;
    out.dynamic = suffix == ",dynamicIndexed";
    return true;
}
inline bool AddedConstant(const ConstantDeclaration& declaration) noexcept
{
    return !declaration.dynamic && ((declaration.slot == 8 && declaration.vectors == 7) ||
        ((declaration.slot == 12 || declaration.slot == 13) && declaration.vectors == 10));
}
inline bool Declaration(std::string_view compact) noexcept
{
    if (compact == "dcl_resource_texture1d(float,float,float,float)t120") return true;
    ConstantDeclaration declaration;
    return Constant(compact, declaration) && AddedConstant(declaration);
}
} // namespace gimi::vertex
