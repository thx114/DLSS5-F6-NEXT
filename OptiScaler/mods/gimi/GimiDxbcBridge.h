#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace gimi { struct PixelTarget6Signature; namespace dxbc {
// Serialized internal tool only, called at Present preparation. No IO/logger.
bool Disassemble(const std::vector<std::uint8_t>&, std::string&, std::string& error);
// Preserve all original signature/resource metadata chunks. Only executable
// SHDR/SHEX is replaced: this entry cannot add outputs or claim RDEF lists
// new t120/VS CB inputs. Typed callers inspect the FINAL declarations/loads
// and retain their actual readers independently of the original RDEF.
bool AssemblePreservingSignatures(const std::string&, const std::vector<std::uint8_t>& original,
                                 std::vector<std::uint8_t>&, std::string& error);
// Narrow authentic signature extension; all original descriptors/chunks survive.
bool AssemblePixelTarget6(const std::string&, const std::vector<std::uint8_t>& original,
                         const PixelTarget6Signature&, std::vector<std::uint8_t>&, std::string& error);
} }
