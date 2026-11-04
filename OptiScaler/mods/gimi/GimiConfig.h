#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gimi
{
using Id = std::uint32_t;
inline constexpr Id InvalidId = ~Id{0};

struct SourceLocation
{
    std::filesystem::path file;
    std::uint32_t line = 1;
};

enum class DiagnosticCode { Unsupported, InvalidSyntax, MissingDependency, Cycle, Limit, Io };
struct Diagnostic
{
    DiagnosticCode code = DiagnosticCode::Unsupported;
    SourceLocation source;
    std::string message;
};

// The raw spelling is retained for flow control and precise diagnostics.
// Entries are a sequence, never a key/value map: duplicate commands matter.
struct ConfigEntry
{
    SourceLocation source;
    std::string nameSpace;
    std::string raw;
    std::string key;
    std::string value;
    bool hasEquals = false;
};

struct ConfigSection
{
    SourceLocation source;
    std::string name;
    std::string nameSpace;
    std::string canonicalName;
    std::vector<ConfigEntry> entries;
    // Every physical declaration, including empty merged Present/Constants.
    // Entry locations remain the actual per-command source.
    std::vector<SourceLocation> declarations;
    // Set ONLY for an exact original-root host section whose EVERY field was
    // admitted by the native adapter. Required program sections never use it.
    bool adaptedHost = false;
};

struct ConfigFile
{
    std::filesystem::path path;
    std::string nameSpace;
    // Exact sandbox which admitted this physical file. Shared global sections
    // retain per-entry source paths; imports never grant the MOD access to the
    // union of all library roots. Used only while loading/compiling.
    std::filesystem::path packageRoot;
};

// An explicit F6 host import is not a second 3DMigoto process. Keep every
// original setting and its disposition; only the named native mappings execute.
enum class HostSettingKind { Native, SourceTool, Inactive };
struct HostSetting
{
    SourceLocation source;
    std::string section, key, value, mapping;
    HostSettingKind kind = HostSettingKind::Native;
};
struct HostImport
{
    std::filesystem::path rootIni, userConfig;
    std::vector<HostSetting> settings;
    bool inputEnabled = true, disableAllInput = false, toggleInput = false;
    bool allowBufferResize = true;
    std::int32_t saveIntervalSeconds = 60;
};

struct Config
{
    std::filesystem::path root;
    std::vector<ConfigFile> files;
    std::vector<ConfigSection> sections;
    HostImport host;
    const std::filesystem::path* RootFor(const std::filesystem::path& file) const;
};

struct LoadLimits
{
    std::uint32_t files = 1024;
    std::uint32_t sections = 32768;
    std::uint32_t entries = 262144;
    std::uint32_t includeDepth = 32;
    std::uint32_t directoryEntries = 65536;
    std::uint64_t bytes = 32ull * 1024 * 1024;
    std::uint32_t lineBytes = 16384;
};

struct LoadRequest
{
    std::filesystem::path root;
    // First file is the root INI (empty default namespace, unless namespaceRoot
    // is supplied). Additional files have their relative-path namespace, unless
    // their preamble overrides it.
    // Explicit includes can load DISABLED* files, as in the upstream package.
    std::vector<std::filesystem::path> files;
    LoadLimits limits;
    // Explicit absolute library INIs, not discovery or a namespace-to-filename
    // guess. Each parent is an independent sandbox. Includes keep that origin;
    // the full declarations/triggers remain required by CompilePlan.
    std::vector<std::filesystem::path> libraries;
    // Explicit directory ingress: recursively load EVERY .ini under root,
    // including DISABLED* (no host exclusion policy is inferred). Mutually
    // exclusive with files. Every file gets its relative-path namespace and
    // remains required; selecting a directory never selects its overrides.
    bool recursiveRoot = false;
    // Optional original XXMI directory used ONLY to derive default namespaces,
    // e.g. Mods\Pack\Part.ini. Must contain every admitted INI. Explicit
    // preambles, including an empty namespace, still override the default.
    // It grants no include/resource authority: each packageRoot stays separate.
    // With this set, even files[0] is an included MOD, not a second root INI.
    std::filesystem::path namespaceRoot;
    // Explicit absolute ORIGINAL d3dx.ini. Enables the native-host contract:
    // root Constants + core includes + only this selected MOD, root exclusions,
    // authentic original namespaces, and last-stage read-only user-state import.
    // Not a library and never a recursive authorization for its whole Mods tree.
    std::filesystem::path hostRoot;
};

struct LoadResult
{
    std::shared_ptr<const Config> config;
    std::vector<Diagnostic> diagnostics;
    explicit operator bool() const noexcept { return config != nullptr; }
};

// Load-time only. No provider, game, device, or proxy dependency.
LoadResult LoadConfig(const LoadRequest& request);
std::string LowerAscii(std::string_view value);
std::string Trim(std::string_view value);
std::string PathUtf8(const std::filesystem::path& path);
std::string SectionName(std::string_view name, std::string_view nameSpace);
std::string VariableName(std::string_view name, std::string_view nameSpace);
// Section/variable names are UTF-8 with ASCII case folding, like GIMI names.
// Paths remain native filesystem::path and retain their original case.
} // namespace gimi
