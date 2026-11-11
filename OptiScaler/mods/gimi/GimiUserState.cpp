#include "GimiUserState.h"
#include <Windows.h>
#include <ShlObj.h>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <system_error>
#include <stdexcept>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Uuid.lib")

namespace gimi
{
namespace
{
constexpr std::size_t MaxBytes = 4u * 1024u * 1024u;
constexpr std::string_view Magic = "; F6 GIMI independent persistent state v1";
constexpr std::string_view Begin = "f6.gimi.persistent.v1", Commit = "f6.gimi.commit.v1";
using Values = std::map<std::string, std::string>;
struct File
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    File() = default;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};
bool Fail(std::string& error, const char* text)
{ error = text; return false; }
std::uint64_t Utc() noexcept
{
    FILETIME ft{}; GetSystemTimeAsFileTime(&ft);
    return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}
bool Open(File& file, const std::filesystem::path& path, bool write, bool create, bool& missing, std::string& error)
{
    missing = false;
    // Deny concurrent writers AND delete/rename of this exact file object.
    // This is a Windows file share contract, not a cross-app input claim.
    file.handle = CreateFileW(path.c_str(), GENERIC_READ | (write ? GENERIC_WRITE : 0),
        FILE_SHARE_READ, nullptr, create ? CREATE_NEW : OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file.handle == INVALID_HANDLE_VALUE)
    {
        const auto code = GetLastError();
        if (!write && (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)) { missing = true; return true; }
        if (create && (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS))
            return Fail(error, "F6 sidecar appeared after initial load; conflict, no overwrite permitted");
        return Fail(error, "F6 state file unavailable, removed or held by another writer; state retained");
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(file.handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(file.handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || (write && info.nNumberOfLinks != 1))
        return Fail(error, "F6 state refuses non-regular/reparse/hard-linked files");
    return true;
}
bool Read(File& file, std::string& text, std::string& error)
{
    LARGE_INTEGER size{}, start{};
    if (!GetFileSizeEx(file.handle, &size) || size.QuadPart < 0 || size.QuadPart > MaxBytes ||
        !SetFilePointerEx(file.handle, start, nullptr, FILE_BEGIN))
        return Fail(error, "F6 state exceeds 4 MiB or cannot be read");
    text.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    if (!text.empty() && (!ReadFile(file.handle, text.data(), static_cast<DWORD>(text.size()), &read, nullptr) || read != text.size()))
        return Fail(error, "F6 state incomplete read");
    if (text.find('\0') != text.npos || (!text.empty() &&
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0)))
        return Fail(error, "F6 state requires valid UTF-8 without NUL");
    return true;
}
template<class F> bool Lines(std::string_view text, F consume, std::string& error)
{
    for (std::size_t start = 0; start < text.size();)
    {
        const auto end = text.find('\n', start);
        const auto count = (end == text.npos ? text.size() : end) - start;
        if (count > 16384) return Fail(error, "F6 state line exceeds 16 KiB");
        auto line = Trim(text.substr(start, count));
        start = end == text.npos ? text.size() : end + 1;
        if (!consume(line)) return false;
    }
    return true;
}
bool Document(const std::string& bytes, Values& values, std::string& error)
{
    if (bytes.empty()) return true;
    if (bytes.compare(0, Magic.size(), Magic) != 0)
        return Fail(error, "Not an F6 state document; no overwrite permitted");
    bool pending = false;
    Values staged;
    return Lines(bytes, [&](const std::string& line) {
        if (line.empty() || line[0] == ';' || line[0] == '#') return true;
        if (line[0] == '[')
        {
            if (line.back() != ']') return Fail(error, "Malformed F6 state section");
            const auto section = LowerAscii(Trim(std::string_view(line).substr(1, line.size() - 2)));
            if (section == Begin)
            {
                if (pending) return Fail(error, "Unfinished F6 state transaction");
                pending = true; staged.clear();
            }
            else if (section == Commit)
            {
                if (!pending) return Fail(error, "Unpaired F6 state commit");
                for (const auto& item : staged) values[item.first] = item.second;
                staged.clear(); pending = false;
            }
            else if (pending) return Fail(error, "Unknown section inside F6 transaction");
            return true;
        }
        if (!pending) return true; // preserve other namespaces/unknown verbatim
        const auto equal = line.find('=');
        if (equal == line.npos) return Fail(error, "Malformed F6 state assignment");
        const auto key = LowerAscii(Trim(std::string_view(line).substr(0, equal)));
        if (key.empty() || !staged.emplace(key, Trim(std::string_view(line).substr(equal + 1))).second)
            return Fail(error, "Duplicate/empty F6 state key in one transaction");
        return true;
    }, error) && (!pending || Fail(error, "Uncommitted F6 state tail; repair required, no overwrite"));
}
bool Number(std::string_view text, float& value)
{
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(value);
}
std::string Number(float value)
{
    char text[64]{};
    const auto result = std::to_chars(text, text + sizeof(text), value, std::chars_format::general,
        std::numeric_limits<float>::max_digits10);
    if (result.ec != std::errc{}) throw std::runtime_error("Cannot encode persistent value");
    return std::string(text, result.ptr);
}
std::optional<std::string> Find(const Values& values, const std::string& key)
{
    const auto found = values.find(key);
    if (found == values.end()) return {};
    return found->second;
}
bool ProductPath(std::filesystem::path& path, std::string& error)
{
    PWSTR known = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &known)) || !known)
        return Fail(error, "F6 LocalAppData state root unavailable");
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owner(known, &CoTaskMemFree);
    path = std::filesystem::path(owner.get());
    if (!path.is_absolute()) return Fail(error, "F6 state root must be absolute");
    path /= L"HoYoShadeHub\\F6\\GIMI\\Genshin\\persist-v1.ini";
    return true;
}
bool Import(std::string_view originalRoot, Values& imported, std::string& error, const Config* importedConfig)
{
    const auto root = std::filesystem::u8path(originalRoot);
    if (!root.is_absolute()) return Fail(error, "Initial import needs an explicit absolute ORIGINAL root INI");
    File file; bool missing = false; std::string bytes;
    const bool nativeHost = importedConfig && !importedConfig->host.rootIni.empty();
    if (nativeHost && LowerAscii(PathUtf8(std::filesystem::weakly_canonical(root))) !=
        LowerAscii(PathUtf8(importedConfig->host.rootIni)))
        return Fail(error, "State import root differs from compiled HostRoot authority");
    if (!nativeHost && (!Open(file, root, false, false, missing, error) || missing || !Read(file, bytes, error)))
        return Fail(error, "Cannot read explicit original root INI for initial import");
    if (bytes.compare(0, 3, "\xef\xbb\xbf") == 0) bytes.erase(0, 3);
    bool include = false, foundInclude = false, userSeen = false;
    std::filesystem::path user = root.parent_path() / L"d3dx_user.ini";
    if (!nativeHost && !Lines(bytes, [&](const std::string& line) {
        if (line.empty() || line[0] == ';' || line[0] == '#') return true;
        if (line[0] == '[')
        {
            if (line.back() != ']') return Fail(error, "Invalid original root section");
            include = LowerAscii(Trim(std::string_view(line).substr(1, line.size() - 2))) == "include";
            if (include && foundInclude) return Fail(error, "Ambiguous original root [Include]");
            foundInclude |= include; return true;
        }
        if (!include) return true;
        const auto equal = line.find('=');
        if (equal == line.npos || LowerAscii(Trim(std::string_view(line).substr(0, equal))) != "user_config") return true;
        if (userSeen) return Fail(error, "Duplicate original root user_config authority");
        userSeen = true;
        auto value = Trim(std::string_view(line).substr(equal + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        if (value.empty()) return Fail(error, "Empty original user_config");
        user = std::filesystem::u8path(value);
        if (!user.is_absolute()) user = root.parent_path() / user;
        return true;
    }, error)) return false;
    if (nativeHost) user = importedConfig->host.userConfig;
    else if (!foundInclude) return Fail(error, "Initial import requires unambiguous original root [Include]");
    File config;
    if (!Open(config, user, false, false, missing, error)) return false;
    if (missing) return true; // authentic XXMI default when user file is absent
    if (!Read(config, bytes, error)) return false;
    if (bytes.compare(0, 3, "\xef\xbb\xbf") == 0) bytes.erase(0, 3);
    bool constants = false;
    return Lines(bytes, [&](const std::string& line) {
        if (line.empty() || line[0] == ';' || line[0] == '#') return true;
        if (line[0] == '[')
        {
            if (line.back() != ']') return Fail(error, "Invalid imported user_config section");
            constants = LowerAscii(Trim(std::string_view(line).substr(1, line.size() - 2))) == "constants";
            return true;
        }
        if (!constants) return true;
        const auto equal = line.find('=');
        if (equal == line.npos) return Fail(error, "Unsupported imported user_config command");
        const auto key = LowerAscii(Trim(std::string_view(line).substr(0, equal)));
        if (key.empty() || key[0] != '$' || !imported.emplace(key, Trim(std::string_view(line).substr(equal + 1))).second)
            return Fail(error, "Ambiguous imported variable assignment");
        return true;
    }, error);
}
} // namespace

bool PersistentState::Load(CommandState& commands, std::string_view originalRoot, std::string& error, const Config* importedConfig)
{
    if (!commands.CanInitialize()) return Fail(error, "State load cannot cross a pending interpreter transaction");
    const auto& variables = commands.PlanOwner()->Variables();
    std::size_t serializedBudget = 256; // transaction/header plus finite scalar spelling
    for (Id i = 0; i < variables.size(); ++i)
        if (variables[i].persistent)
        {
            const auto& name = variables[i].name;
            if (variables[i].local || name.empty() || name[0] != '$' ||
                name.find_first_of("=\r\n[];#") != name.npos)
                return Fail(error, "Persistent key is not a supported canonical global namespace scalar");
            if (name.size() > 16320 || name.size() + 64 > MaxBytes - serializedBudget)
                return Fail(error, "Persistent namespace keys exceed the bounded sidecar line/document budget");
            serializedBudget += name.size() + 64;
            ids_.push_back(i); saved_.push_back(variables[i].initial);
        }
    if (ids_.empty()) return true;
    if (!ProductPath(path_, error)) return false;
    File file; bool missing = false; std::string bytes;
    if (!Open(file, path_, false, false, missing, error)) return false;
    existed_ = !missing;
    if (!missing)
    {
        if (!Read(file, bytes, error)) return false;
        if (bytes.empty()) return Fail(error, "Existing F6 sidecar is empty/incomplete; explicit repair required");
        if (!Document(bytes, baseline_, error)) return false;
    }
    // An existing sidecar is authoritative even for keys it does not contain:
    // newly declared variables start from declarations, never re-import XXMI.
    Values initial = baseline_;
    if (missing && !originalRoot.empty())
    {
        if (!Import(originalRoot, initial, error, importedConfig)) return false;
        imported_ = true;
    }
    std::vector<float> values = saved_;
    for (std::size_t i = 0; i < ids_.size(); ++i)
    {
        const auto found = initial.find(variables[ids_[i]].name);
        if (found != initial.end() && !Number(found->second, values[i]))
            return Fail(error, "Known persistent value is not a finite scalar; import/load refused");
    }
    // Complete validation precedes every mutation; InitializeVariable's full
    // preconditions (id/persist/finite/no pending) are already proven here.
    for (std::size_t i = 0; i < ids_.size(); ++i)
        if (!commands.InitializeVariable(ids_[i], values[i])) return Fail(error, "Persistent initialization refused");
    saved_ = std::move(values); seed_ = missing; dirty_ = seed_; loadedUtc_ = Utc();
    return true;
}

bool PersistentState::PollDirty(const CommandState& commands) noexcept
{
    dirty_ = seed_;
    for (std::size_t i = 0; i < ids_.size(); ++i)
    {
        float value = 0;
        if (!commands.ReadVariable(ids_[i], value)) { dirty_ = true; continue; }
        dirty_ |= value != saved_[i];
    }
    return dirty_;
}

bool PersistentState::Save(CommandState& commands, std::string& error)
{
    if (!commands.CanInitialize()) return Fail(error, "F6 state save cannot cross a pending interpreter transaction");
    if (!PollDirty(commands)) return true;
    // Allocation, directory access and IO occur ONLY in explicit Present save.
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    if (ec) return Fail(error, "Cannot create F6 independent state directory");
    File file; bool missing = false; std::string bytes;
    if (!Open(file, path_, true, !existed_, missing, error) || !Read(file, bytes, error)) return false;
    Values current;
    if (existed_ && bytes.empty()) return Fail(error, "F6 sidecar cleared externally; conflict, no overwrite permitted");
    if (!Document(bytes, current, error)) return false;
    const auto& variables = commands.PlanOwner()->Variables();
    std::vector<float> values = saved_;
    std::vector<std::size_t> changed;
    std::string transaction = "\n[F6.GIMI.Persistent.v1]\n";
    for (std::size_t i = 0; i < ids_.size(); ++i)
    {
        if (!commands.ReadVariable(ids_[i], values[i]) || !std::isfinite(values[i]))
            return Fail(error, "F6 state has no committed finite variable value");
        if (!seed_ && values[i] == saved_[i]) continue;
        const auto& key = variables[ids_[i]].name;
        if (Find(current, key) != Find(baseline_, key))
            return Fail(error, "F6 state same-key external edit conflict; unsaved memory retained, reload refused");
        changed.push_back(i);
        transaction += key + " = " + Number(values[i]) + "\n";
    }
    transaction += "[F6.GIMI.Commit.v1]\n";
    if (bytes.empty()) transaction.insert(0, std::string(Magic) + "\n");
    if (transaction.size() > MaxBytes - bytes.size())
        return Fail(error, "F6 state 4 MiB append budget exhausted; no automatic compaction/overwrite");
    // Do not rewrite any previous byte, namespace or unknown field. The share
    // mode excludes writers/renames during read-conflict-check-append-flush.
    // A power-loss/partial-write tail has no valid commit and refuses reload;
    // this is not a claim of crash-atomic filesystem durability.
    LARGE_INTEGER end{}; DWORD written = 0;
    if (!SetFilePointerEx(file.handle, end, nullptr, FILE_END) ||
        !WriteFile(file.handle, transaction.data(), static_cast<DWORD>(transaction.size()), &written, nullptr) ||
        written != transaction.size() || !FlushFileBuffers(file.handle))
        return Fail(error, "F6 state append/flush failed; memory retained; incomplete tail needs repair");
    for (const auto i : changed)
    {
        saved_[i] = values[i]; baseline_[variables[ids_[i]].name] = Number(values[i]);
    }
    existed_ = true; seed_ = dirty_ = false; savedUtc_ = Utc();
    return true;
}
} // namespace gimi
