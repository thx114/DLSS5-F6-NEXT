#pragma once

#include "GimiCommands.h"
#include <map>

namespace gimi
{
// F6 independent, per-game user state. Never writes any package/XXMI INI.
// Owner calls Load last, then PollDirty/Save only outside draw/native pairs.
// The sidecar is an append-only, bounded transaction document; no compaction
// silently discards unknown content. A torn transaction is a hard refusal.
class PersistentState final
{
public:
    bool Load(CommandState&, std::string_view originalRoot, std::string& error, const Config* importedConfig = nullptr);
    bool PollDirty(const CommandState&) noexcept;
    bool Save(CommandState&, std::string& error);
    bool Dirty() const noexcept { return dirty_; }
    bool InitialSave() const noexcept { return seed_; }
    const std::filesystem::path& Path() const noexcept { return path_; }
    std::uint64_t LastLoadedUtc() const noexcept { return loadedUtc_; }
    std::uint64_t LastSavedUtc() const noexcept { return savedUtc_; }
    bool Imported() const noexcept { return imported_; }
private:
    std::filesystem::path path_;
    std::vector<Id> ids_;
    std::vector<float> saved_;
    std::map<std::string, std::string> baseline_;
    bool existed_ = false, seed_ = false, dirty_ = false, imported_ = false;
    std::uint64_t loadedUtc_ = 0, savedUtc_ = 0;
};
} // namespace gimi
