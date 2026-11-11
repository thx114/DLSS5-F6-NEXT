#include "GimiConfig.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>

namespace gimi
{
std::string LowerAscii(std::string_view v)
{
    std::string result(v);
    for (char& c : result)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return result;
}

std::string Trim(std::string_view v)
{
    const auto a = v.find_first_not_of(" \t\r\n");
    if (a == v.npos) return {};
    return std::string(v.substr(a, v.find_last_not_of(" \t\r\n") - a + 1));
}

std::string PathUtf8(const std::filesystem::path& path)
{
    // Works with both C++17 std::string and C++20 std::u8string paths.
    const auto utf8 = path.generic_u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::string SectionName(std::string_view name, std::string_view ns)
{
    const auto n = LowerAscii(name);
    // These prefixes are language categories, not heuristics for MOD identity.
    constexpr std::string_view prefixes[] = {"textureoverride", "shaderoverride", "shaderregex",
        "commandlist", "customshader", "resource", "include", "key", "preset"};
    if (!ns.empty())
        for (auto prefix : prefixes)
            if (n.compare(0, prefix.size(), prefix) == 0)
            {
                // Explicit category\namespace\name is already canonical.
                // Never prepend the caller namespace to an absolute reference.
                if (n.size() > prefix.size() && n[prefix.size()] == '\\') return n;
                return std::string(prefix) + "\\" + LowerAscii(ns) + "\\" + n.substr(prefix.size());
            }
    return n;
}

std::string VariableName(std::string_view name, std::string_view ns)
{
    if (ns.empty() || name.size() < 2 || name[1] == '\\') return LowerAscii(name);
    return "$\\" + LowerAscii(ns) + "\\" + LowerAscii(name.substr(1));
}

const std::filesystem::path* Config::RootFor(const std::filesystem::path& file) const
{
    const auto key = LowerAscii(PathUtf8(file));
    for (const auto& known : files)
        if (LowerAscii(PathUtf8(known.path)) == key) return &known.packageRoot;
    return nullptr;
}

namespace
{
struct LoadFailure { Diagnostic diagnostic; };
[[noreturn]] void Fail(DiagnosticCode code, const SourceLocation& at, std::string message)
{
    throw LoadFailure{{code, at, std::move(message)}};
}

std::string PathKey(const std::filesystem::path& p) { return LowerAscii(PathUtf8(p)); }

// Deliberately limited to the upstream '*'/'?' filename glob subset.
// Unsupported bracket/escape syntax is rejected instead of changing meaning.
bool Glob(std::string_view pattern, std::string_view name)
{
    const auto p = LowerAscii(pattern), n = LowerAscii(name);
    std::size_t i = 0, j = 0, star = std::string::npos, retry = 0;
    while (j < n.size())
    {
        if (i < p.size() && (p[i] == '?' || p[i] == n[j])) { ++i; ++j; }
        else if (i < p.size() && p[i] == '*') { star = i++; retry = j; }
        else if (star != std::string::npos) { i = star + 1; j = ++retry; }
        else return false;
    }
    while (i < p.size() && p[i] == '*') ++i;
    return i == p.size();
}

std::filesystem::path IniPath(std::string value)
{
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
    std::replace(value.begin(), value.end(), '\\', '/');
    return std::filesystem::u8path(value);
}

// Reject malformed UTF-8 rather than interpreting an ANSI filename differently.
bool Utf8(std::string_view s)
{
    for (std::size_t i = 0; i < s.size();)
    {
        const auto a = static_cast<unsigned char>(s[i++]);
        if (a < 0x80) { if (!a) return false; continue; }
        unsigned count = 0, cp = 0, minimum = 0;
        if (a >= 0xc2 && a <= 0xdf) { count = 1; cp = a & 31; minimum = 0x80; }
        else if (a >= 0xe0 && a <= 0xef) { count = 2; cp = a & 15; minimum = 0x800; }
        else if (a >= 0xf0 && a <= 0xf4) { count = 3; cp = a & 7; minimum = 0x10000; }
        else return false;
        if (count > s.size() - i) return false;
        while (count--)
        {
            const auto b = static_cast<unsigned char>(s[i++]);
            if ((b & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (b & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}

bool Contains(const std::filesystem::path& root, const std::filesystem::path& file)
{
    auto a = root.begin(), b = file.begin();
    for (; a != root.end(); ++a, ++b)
        if (b == file.end() || PathKey(*a) != PathKey(*b)) return false;
    return true;
}

class Loader
{
    const LoadRequest& request_;
    std::shared_ptr<Config> config_ = std::make_shared<Config>();
    std::set<std::string> seen_;
    std::map<std::string, std::size_t> sections_;
    struct Exclusion { std::string packageRoot, pattern; };
    std::vector<Exclusion> excludes_;
    std::filesystem::path activeRoot_, namespaceRoot_;
    bool selectedByHost_ = false;
    std::vector<std::string> hostExcludes_;
    std::uint64_t bytes_ = 0;
    std::uint32_t entries_ = 0, directoryEntries_ = 0;
    struct Include { ConfigEntry entry; std::filesystem::path directory, packageRoot; };
    std::map<std::string, std::vector<Include>> pendingIncludes_;
    std::map<std::string, std::vector<std::pair<std::string, SourceLocation>>> edges_;

    std::filesystem::path Resolve(const std::filesystem::path& p, const SourceLocation& at)
    {
        const auto resolved = std::filesystem::weakly_canonical(p.is_absolute() ? p : activeRoot_ / p);
        auto r = activeRoot_.begin(), t = resolved.begin();
        for (; r != activeRoot_.end(); ++r, ++t)
            if (t == resolved.end() || PathKey(*r) != PathKey(*t))
                Fail(DiagnosticCode::Unsupported, at, "Dependency escapes the selected package root");
        return resolved;
    }

    void Recursive(const std::filesystem::path& path, const SourceLocation& at, unsigned depth)
    {
        if (depth > request_.limits.includeDepth)
            Fail(DiagnosticCode::Limit, at, "Recursive include depth exceeded");
        const auto dir = Resolve(path, at);
        if (!std::filesystem::is_directory(dir))
            Fail(DiagnosticCode::MissingDependency, at, "Recursive include directory not found: " + PathUtf8(dir));
        std::vector<std::filesystem::path> files, dirs;
        for (const auto& e : std::filesystem::directory_iterator(dir))
        {
            if (++directoryEntries_ > request_.limits.directoryEntries)
                Fail(DiagnosticCode::Limit, at, "Recursive directory entry limit exceeded");
            const auto filename = PathUtf8(e.path().filename());
            if (std::any_of(excludes_.begin(), excludes_.end(), [&](const auto& x) { return x.packageRoot == PathKey(activeRoot_) && Glob(x.pattern, filename); }))
                continue;
            // Do not follow directory aliases; selected files still pass Resolve.
            if (e.is_symlink()) Fail(DiagnosticCode::Unsupported, at, "Recursive include contains a symbolic link");
            if (e.is_directory()) dirs.push_back(e.path());
            else if (LowerAscii(PathUtf8(e.path().extension())) == ".ini") files.push_back(e.path());
        }
        const auto less = [](const auto& a, const auto& b) { return PathKey(a) < PathKey(b); };
        std::sort(files.begin(), files.end(), less);
        std::sort(dirs.begin(), dirs.end(), less);
        for (const auto& f : files) Read(f, false, at, depth + 1);
        for (const auto& d : dirs) Recursive(d, at, depth + 1);
    }

    void Read(const std::filesystem::path& path, bool main, const SourceLocation& from, unsigned depth, bool dependency = true)
    {
        if (depth > request_.limits.includeDepth) Fail(DiagnosticCode::Limit, from, "Include depth exceeded");
        const auto file = Resolve(path, from);
        const auto key = PathKey(file);
        if (dependency) edges_[PathKey(from.file)].push_back({key, from});
        if (seen_.count(key))
        {
            // Same file under different admission roots would give its includes
            // and resources ambiguous authority. Do not let load order choose.
            const auto* originalRoot = config_->RootFor(file);
            if (!originalRoot || PathKey(*originalRoot) != PathKey(activeRoot_))
                Fail(DiagnosticCode::Unsupported, from, "Library file has conflicting package roots: " + PathUtf8(file));
            return; // upstream includes a physical path once
        }
        if (config_->files.size() >= request_.limits.files) Fail(DiagnosticCode::Limit, from, "INI file limit exceeded");
        if (!std::filesystem::is_regular_file(file))
            Fail(DiagnosticCode::MissingDependency, from, "INI not found: " + PathUtf8(file));
        const auto size = std::filesystem::file_size(file);
        if (size > request_.limits.bytes - bytes_) Fail(DiagnosticCode::Limit, from, "INI byte budget exceeded");
        bytes_ += size;
        std::ifstream stream(file, std::ios::binary);
        if (!stream) Fail(DiagnosticCode::Io, from, "Cannot open INI: " + PathUtf8(file));
        std::string text(static_cast<std::size_t>(size), '\0');
        if ((size && !stream.read(text.data(), static_cast<std::streamsize>(size))) ||
            stream.peek() != std::char_traits<char>::eof())
            Fail(DiagnosticCode::Io, from, "INI changed or incomplete read: " + PathUtf8(file));
        if (text.compare(0, 3, "\xef\xbb\xbf") == 0) text.erase(0, 3);
        if (!Utf8(text)) Fail(DiagnosticCode::Unsupported, {file, 1}, "INI encoding must be valid UTF-8 (optional BOM); UTF-16/ANSI unsupported");
        seen_.insert(key);
        // Namespace identity and filesystem authority are distinct. An explicit
        // original XXMI base preserves real cross-file canonical names without
        // permitting this MOD to read outside its selected package sandbox.
        const auto& namespaceBase = namespaceRoot_.empty() ? activeRoot_ : namespaceRoot_;
        auto a = namespaceBase.begin(), b = file.begin();
        for (; a != namespaceBase.end(); ++a, ++b)
            if (b == file.end() || PathKey(*a) != PathKey(*b))
                Fail(DiagnosticCode::Unsupported, from, "INI lies outside the explicit namespace root: " + PathUtf8(file));
        auto relative = PathUtf8(std::filesystem::relative(file, namespaceBase));
        std::replace(relative.begin(), relative.end(), '/', '\\');
        const bool hostFile = !config_->host.rootIni.empty() && PathKey(file) == PathKey(config_->host.rootIni);
        std::string ns = main && (namespaceRoot_.empty() || hostFile) ? "" : relative;
        std::size_t current = static_cast<std::size_t>(-1);
        bool preamble = true;
        std::uint32_t number = 0;
        for (std::size_t start = 0; start < text.size();)
        {
            const auto end = text.find('\n', start);
            const auto length = (end == text.npos ? text.size() : end) - start;
            SourceLocation at{file, ++number};
            if (length > request_.limits.lineBytes) Fail(DiagnosticCode::Limit, at, "INI line byte limit exceeded");
            auto line = Trim(std::string_view(text).substr(start, length));
            start = end == text.npos ? text.size() : end + 1;
            if (line.empty() || line.front() == ';') continue;
            if (line.front() == '[')
            {
                const auto close = line.find(']');
                if (close == line.npos || !Trim(std::string_view(line).substr(close + 1)).empty())
                    Fail(DiagnosticCode::InvalidSyntax, at, "Malformed section header (inline comments unsupported)");
                const auto name = Trim(std::string_view(line).substr(1, close - 1));
                if (name.empty()) Fail(DiagnosticCode::InvalidSyntax, at, "Empty section name");
                const auto canonical = SectionName(name, ns);
                auto existing = sections_.find(canonical);
                // Global Constants/Present sections append, with entry-specific namespace.
                const bool global = canonical == LowerAscii(name);
                if (existing != sections_.end() && !(global && !ns.empty() && (canonical == "constants" || canonical == "present")))
                    Fail(DiagnosticCode::Unsupported, at, "Duplicate section: " + canonical);
                if (existing != sections_.end()) current = existing->second;
                else
                {
                    if (config_->sections.size() >= request_.limits.sections)
                        Fail(DiagnosticCode::Limit, at, "Section limit exceeded");
                    current = config_->sections.size();
                    sections_.emplace(canonical, current);
                    config_->sections.push_back({at, name, ns, canonical, {}});
                }
                config_->sections[current].declarations.push_back(at);
                preamble = false;
                continue;
            }
            const auto equal = line.find('=');
            ConfigEntry entry{at, ns, line, LowerAscii(Trim(std::string_view(line).substr(0, equal))),
                equal == line.npos ? "" : Trim(std::string_view(line).substr(equal + 1)), equal != line.npos};
            if (preamble)
            {
                if (entry.key != "namespace" || !entry.hasEquals)
                    Fail(DiagnosticCode::Unsupported, at, "Unsupported INI preamble (only namespace is implemented)");
                // The real d3dx_patch.ini explicitly clears its inherited file
                // namespace. Empty is a meaningful root namespace, not missing
                // syntax. Duplicate-section rules still apply unchanged.
                if (hostFile && !entry.value.empty())
                    Fail(DiagnosticCode::Unsupported, at, "Original host root must have the global namespace");
                ns = entry.value;
                continue;
            }
            if (++entries_ > request_.limits.entries) Fail(DiagnosticCode::Limit, at, "Entry limit exceeded");
            config_->sections[current].entries.push_back(entry);
            if (LowerAscii(config_->sections[current].name).compare(0, 7, "include") == 0)
            {
                if (!entry.hasEquals || entry.value.empty()) Fail(DiagnosticCode::InvalidSyntax, at, "Empty include directive");
                if (entry.key == "exclude_recursive")
                {
                    // Upstream reads the main [Include] excludes before recursion.
                    if (!main || LowerAscii(config_->sections[current].name) != "include" ||
                        entry.value.find_first_of("[]\\/") != std::string::npos)
                        Fail(DiagnosticCode::Unsupported, at, "Only root [Include] filename '*'/'?' exclusions are supported");
                    excludes_.push_back({PathKey(activeRoot_), entry.value});
                    if (hostFile) hostExcludes_.push_back(entry.value);
                }
                else if (entry.key == "user_config" && hostFile && LowerAscii(config_->sections[current].name) == "include")
                {
                    if (!config_->host.userConfig.empty())
                        Fail(DiagnosticCode::InvalidSyntax, at, "Duplicate host user_config authority");
                    config_->host.userConfig = Resolve(file.parent_path() / IniPath(entry.value), at);
                }
                else if (entry.key == "include" || entry.key == "include_recursive")
                    pendingIncludes_[config_->sections[current].canonicalName].push_back({entry, file.parent_path(), activeRoot_});
                else Fail(DiagnosticCode::Unsupported, at, "Unsupported include directive: " + entry.key);
            }
        }
        config_->files.push_back({file, ns, activeRoot_});
    }

    void AdaptHost()
    {
        // This whitelist describes F6's import boundary, not permissive INI
        // emulation. Unknown fields (even in a known section) fail at source.
        // SourceTool settings are retained with an explicit non-execution
        // disposition, never registered as game keys, launchers or DLL hooks.
        auto& host = config_->host;
        const auto boolean = [&](const ConfigEntry& e) {
            const auto v = LowerAscii(Trim(e.value));
            if (v != "0" && v != "1" && v != "true" && v != "false")
                Fail(DiagnosticCode::InvalidSyntax, e.source, "Host boolean requires 0/1/false/true: " + e.key);
            return v == "1" || v == "true";
        };
        const auto integer = [&](const ConfigEntry& e, int minimum, int maximum) {
            int value = 0;
            const auto text = Trim(e.value);
            const auto r = std::from_chars(text.data(), text.data() + text.size(), value);
            if (r.ec != std::errc{} || r.ptr != text.data() + text.size() || value < minimum || value > maximum)
                Fail(DiagnosticCode::InvalidSyntax, e.source, "Host integer outside supported range: " + e.key);
            return value;
        };
        for (auto& section : config_->sections)
        {
            if (PathKey(section.source.file) != PathKey(host.rootIni)) continue;
            const auto name = LowerAscii(section.name);
            const bool hostSection = name == "loader" || name == "hunting" || name == "system" ||
                name == "device" || name == "stereo" || name == "rendering" ||
                name == "profile" || name == "logging" || name == "input";
            if (!hostSection) continue; // Constants/Present/commands stay fully required
            std::set<std::string> seen;
            for (const auto& e : section.entries)
            {
                if (!e.hasEquals || !seen.insert(e.key).second)
                    Fail(DiagnosticCode::InvalidSyntax, e.source, "Ambiguous host setting: " + e.key);
                const auto v = LowerAscii(Trim(e.value));
                std::string mapping;
                HostSettingKind kind = HostSettingKind::Native;
                const auto fixed = [&](std::string_view expected, const char* meaning) {
                    if (v != expected)
                        Fail(DiagnosticCode::Unsupported, e.source, "F6 host requires " + name + "." + e.key +
                            "=" + std::string(expected) + "; implement the corresponding native owner before importing this value");
                    mapping = meaning;
                };
                if (name == "loader")
                {
                    if (e.key == "target")
                    {
                        if (v != "yuanshen.exe" && v != "genshinimpact.exe")
                            Fail(DiagnosticCode::Unsupported, e.source, "Host target is not a supported Genshin executable");
                        mapping = "Genshin-only runtime admission; no external process is launched";
                    }
                    else if (e.key == "module") fixed("d3d11.dll", "source DX11 backend; existing Opt device/capture only");
                    else if (e.key == "require_admin")
                    { boolean(e); kind = HostSettingKind::SourceTool; mapping = "original launcher privilege request; F6 never elevates"; }
                    else if (e.key == "loader" || e.key == "launch")
                    { kind = HostSettingKind::SourceTool; mapping = "original launcher metadata; never executed by native import"; }
                }
                else if (name == "hunting")
                {
                    if (e.key == "hunting") fixed("0", "hunting disabled; native import installs no hunting owner");
                    else
                    {
                        static constexpr std::string_view controls[] = {
                            "marking_mode", "next_marking_mode", "marking_actions", "previous_pixelshader", "next_pixelshader", "mark_pixelshader",
                            "previous_vertexshader", "next_vertexshader", "mark_vertexshader", "previous_indexbuffer", "next_indexbuffer", "mark_indexbuffer",
                            "previous_vertexbuffer", "next_vertexbuffer", "mark_vertexbuffer", "previous_computeshader", "next_computeshader", "mark_computeshader",
                            "previous_geometryshader", "next_geometryshader", "mark_geometryshader", "previous_domainshader", "next_domainshader", "mark_domainshader",
                            "previous_hullshader", "next_hullshader", "mark_hullshader", "done_hunting", "toggle_hunting", "analyse_frame", "analyse_options",
                            "repeat_rate", "verbose_overlay", "take_screenshot", "reload_fixes", "reload_config", "wipe_user_config", "show_original",
                            "monitor_performance", "freeze_performance_monitor", "monitor_performance_interval"};
                        if (std::find(std::begin(controls), std::end(controls), e.key) != std::end(controls))
                        { kind = HostSettingKind::SourceTool; mapping = "original debug-host control; F6 menu owns reload/disable/diagnostics, original key is not installed"; }
                    }
                }
                else if (name == "system")
                {
                    if (e.key == "check_foreground_window") fixed("1", "existing HWND-bound KeyboardFrame focus admission");
                    else if (e.key == "skip_early_includes_load") fixed("1", "immutable import at existing Present preparation");
                    else if (e.key == "dll_initialization_delay" || e.key == "config_initialization_delay") fixed("0", "no additional initialization delay");
                    else if (e.key == "settings_auto_save_interval")
                    { host.saveIntervalSeconds = integer(e, -1, 86400); mapping = "native per-session persistent-state save interval; -1 disables timed saves"; }
                    else if (e.key == "load_library_redirect")
                    { fixed("2", "original loader policy; no library redirection or secondary core in F6"); kind = HostSettingKind::SourceTool; }
                    else if (e.key == "allow_check_interface" || e.key == "allow_create_device" || e.key == "allow_platform_update")
                    { fixed("1", "original proxy creation policy; F6 uses its existing game device owner"); kind = HostSettingKind::SourceTool; }
                    else if (e.key == "screen_width" || e.key == "screen_height")
                    { integer(e, e.key == "screen_width" ? 640 : 480, e.key == "screen_width" ? 15360 : 8640);
                      kind = HostSettingKind::SourceTool; mapping = "original host fallback dimensions retained; native HWND/Mouse values remain real and never substituted"; }
                    else if (e.key == "clear_unknown_settings")
                    { boolean(e); kind = HostSettingKind::SourceTool; mapping = "original XXMI sidecar maintenance; F6 retains unknown values in its independent append-only state"; }
                }
                else if (name == "device")
                {
                    if (e.key == "hide_cursor" || e.key == "cursor_upscaling_bypass")
                    { boolean(e); mapping = "R1045 native input/cursor owner on actual client pixels"; }
                    else if (e.key == "upscaling") fixed("0", "unvirtualized real client coordinates; no SCREEN_UPSCALING hook");
                    else if (e.key == "force_stereo") fixed("0", "mono native device; no stereo owner");
                    else if (e.key == "full_screen")
                    { fixed("0", "original window-creation preference; F6 retains the actual game-owned swapchain without changing its mode"); kind = HostSettingKind::SourceTool; }
                    else if (e.key == "get_resolution_from") fixed("swap_chain", "real swapchain resolution used by capture and texture identity");
                }
                else if (name == "stereo")
                {
                    if (e.key == "automatic_mode" || e.key == "unlock_separation" || e.key == "unlock_convergence" ||
                        e.key == "create_profile" || e.key == "force_no_nvapi")
                        fixed("0", "stereo behavior disabled; no native stereo/driver profile modifications");
                }
                else if (name == "rendering")
                {
                    if (e.key == "shader_hash") fixed("3dmigoto", "existing authentic unseeded FNV-1 shader identity");
                    else if (e.key == "ini_params") fixed("120", "existing complete immutable IniParams t120 pages");
                    else if (e.key == "texture_hash" || e.key == "track_texture_updates" || e.key == "track_region_hashes")
                        fixed("0", "existing capture creation identity policy; no per-draw hash/readback");
                    else if (e.key == "allow_buffer_resize")
                    { host.allowBufferResize = boolean(e); mapping = "existing creation-time CompileBufferResizeRules admission"; }
                    else if (e.key == "rasterizer_disable_scissor" || e.key == "fix_sv_position") fixed("0", "no global raster/shader transformation");
                    else if (e.key == "stereo_params") fixed("-1", "no stereo parameter SRV");
                    else if (e.key == "recursive_include") fixed("1", "bounded source include resolver inside each original library authority");
                    else if (e.key == "assemble_signature_comments" || e.key == "disassemble_undecipherable_custom_data" || e.key == "patch_assembly_cb_offsets")
                        fixed("1", "existing exact supported shader transformations preserve real original DXBC signatures; unsupported shader grammar still rejects");
                    else if (e.key == "cache_shaders" || e.key == "export_fixed" || e.key == "export_shaders" ||
                             e.key == "export_hlsl" || e.key == "export_binary" || e.key == "dump_usage")
                    { fixed("0", "original disk cache/export disabled; native immutable preload remains bounded"); kind = HostSettingKind::Inactive; }
                    else if (e.key == "override_directory" || e.key == "cache_directory" || e.key == "storage_directory")
                    { kind = HostSettingKind::SourceTool; mapping = "original debug/cache path; never discovery, write authority, or automatic ShaderFixes import"; }
                }
                else if (name == "logging")
                {
                    if (e.key == "log_level") fixed("disabled", "original host logging disabled; F6 keeps its existing diagnostics");
                    else if (e.key == "calls" || e.key == "input" || e.key == "debug" || e.key == "unbuffered" ||
                        e.key == "force_cpu_affinity" || e.key == "convergence" || e.key == "separation" ||
                        e.key == "debug_locks" || e.key == "crash" || e.key == "show_warnings")
                        fixed("0", "original API logging/OS debugging side effects disabled");
                }
                else if (name == "input")
                {
                    if (e.key == "input") { host.inputEnabled = boolean(e); mapping = "native initial key-disable state; does not enable MOD selection"; }
                    else if (e.key == "toggle_input")
                    { fixed("ctrl alt shift vk_end", "same-owner KeyboardFrame rising edge, no second input hook"); host.toggleInput = true; }
                    else if (e.key == "input_disable_mode")
                    {
                        if (v != "mods" && v != "all") Fail(DiagnosticCode::Unsupported, e.source, "Native input_disable_mode supports mods/all only");
                        host.disableAllInput = v == "all"; mapping = "original recursive-MOD versus core-library key scope; core cycles remain active in mods scope";
                    }
                }
                if (mapping.empty())
                    Fail(DiagnosticCode::Unsupported, e.source, "Unsupported native host setting " + name + "." + e.key +
                        "; add a real native mapping before importing it (no field is discarded)");
                host.settings.push_back({e.source, name, e.key, e.value, std::move(mapping), kind});
            }
            section.adaptedHost = true;
        }
    }

    void CheckCycles(const std::string& file, std::map<std::string, unsigned>& colors, unsigned depth)
    {
        colors[file] = 1;
        for (const auto& edge : edges_[file])
        {
            if (colors[edge.first] == 1) Fail(DiagnosticCode::Cycle, edge.second, "Include cycle: " + edge.first);
            if (depth >= request_.limits.includeDepth) Fail(DiagnosticCode::Limit, edge.second, "Include dependency depth exceeded");
            if (!colors[edge.first]) CheckCycles(edge.first, colors, depth + 1);
        }
        colors[file] = 2;
    }

public:
    explicit Loader(const LoadRequest& request) : request_(request) {}
    std::shared_ptr<const Config> Load()
    {
        if (request_.root.empty() || (request_.recursiveRoot ? !request_.files.empty() : request_.files.empty()))
            Fail(DiagnosticCode::InvalidSyntax, {request_.root, 1}, "Select either root INI files or one explicit recursive MOD directory");
        config_->root = std::filesystem::weakly_canonical(std::filesystem::absolute(request_.root));
        if (!std::filesystem::is_directory(config_->root))
            Fail(DiagnosticCode::MissingDependency, {request_.root, 1}, "Selected package directory not found");
        if (!request_.namespaceRoot.empty())
        {
            if (!request_.namespaceRoot.is_absolute())
                Fail(DiagnosticCode::InvalidSyntax, {request_.namespaceRoot, 1}, "Namespace root must be an explicit absolute directory");
            namespaceRoot_ = std::filesystem::weakly_canonical(request_.namespaceRoot);
            if (!std::filesystem::is_directory(namespaceRoot_))
                Fail(DiagnosticCode::MissingDependency, {request_.namespaceRoot, 1}, "Namespace directory not found");
        }
        if (!request_.hostRoot.empty())
        {
            if (!request_.hostRoot.is_absolute())
                Fail(DiagnosticCode::InvalidSyntax, {request_.hostRoot, 1}, "HostRoot must be an explicit absolute original root INI");
            config_->host.rootIni = std::filesystem::weakly_canonical(request_.hostRoot);
            const auto hostDirectory = config_->host.rootIni.parent_path();
            if (!namespaceRoot_.empty() && PathKey(namespaceRoot_) != PathKey(hostDirectory))
                Fail(DiagnosticCode::InvalidSyntax, {request_.namespaceRoot, 1}, "NamespaceRoot must equal HostRoot's parent for authentic original names");
            namespaceRoot_ = hostDirectory;
            if (!Contains(hostDirectory, config_->root) || PathKey(hostDirectory) == PathKey(config_->root))
                Fail(DiagnosticCode::Unsupported, {request_.root, 1}, "Select one MOD below the original host directory, never the entire host root");
            activeRoot_ = hostDirectory;
            Read(config_->host.rootIni, true, {config_->host.rootIni, 1}, 0, false);
            AdaptHost(); // all source fields admitted before following any host include
            if (config_->host.userConfig.empty()) config_->host.userConfig = hostDirectory / "d3dx_user.ini";
            for (const auto& pattern : hostExcludes_) excludes_.push_back({PathKey(config_->root), pattern});
            // Selection is explicit but still obeys original recursive excludes,
            // including every ancestor below the host recursive ingress.
            // Actual selected files are read when that original Include executes.
        }
        activeRoot_ = config_->root;
        if (request_.hostRoot.empty())
        {
            if (request_.recursiveRoot)
            {
                Recursive(config_->root, {config_->root, 1}, 0);
                if (config_->files.empty())
                    Fail(DiagnosticCode::MissingDependency, {config_->root, 1}, "Selected MOD directory contains no INI files");
            }
            else for (std::size_t i = 0; i < request_.files.size(); ++i)
                Read(request_.files[i], i == 0, {request_.files[i], 1}, 0, false);
        }
        if (request_.libraries.size() > 16)
            Fail(DiagnosticCode::Limit, {request_.root, 1}, "At most 16 explicit library INIs are supported");
        for (const auto& library : request_.libraries)
        {
            if (!library.is_absolute())
                Fail(DiagnosticCode::InvalidSyntax, {library, 1}, "Library INI must be an explicit absolute path");
            const auto file = std::filesystem::weakly_canonical(library);
            activeRoot_ = file.parent_path();
            // Imported roots have their real declared namespace, or the same
            // relative-path default as an ordinary included file. They do not
            // become a second unqualified root INI.
            Read(file, false, {file, 1}, 0, false);
        }
        // Upstream consumes Include sections in case-insensitive section order,
        // preserving duplicate directive order inside each section, in waves.
        while (!pendingIncludes_.empty())
        {
            auto wave = std::move(pendingIncludes_);
            pendingIncludes_.clear();
            for (const auto& section : wave)
                for (const auto& include : section.second)
                {
                    activeRoot_ = include.packageRoot;
                    const auto path = Resolve(include.directory / IniPath(include.entry.value), include.entry.source);
                    const bool hostDirective = !config_->host.rootIni.empty() &&
                        PathKey(include.entry.source.file) == PathKey(config_->host.rootIni);
                    if (hostDirective && include.entry.key == "include_recursive")
                    {
                        // A host scan is a container declaration, NOT authority
                        // to enumerate other MODs. Require exactly one original
                        // ingress to contain the explicit selected package.
                        if (!Contains(path, config_->root))
                        {
                            config_->host.settings.push_back({include.entry.source, "include", include.entry.key, include.entry.value,
                                "unselected recursive container; not enumerated or imported", HostSettingKind::Inactive});
                            continue;
                        }
                        config_->host.settings.push_back({include.entry.source, "include", include.entry.key, include.entry.value,
                            "original container narrowed to the explicit selected MOD and original exclusions", HostSettingKind::Native});
                        if (selectedByHost_)
                            Fail(DiagnosticCode::InvalidSyntax, include.entry.source, "Selected MOD belongs to multiple host recursive Includes");
                        if (request_.recursiveRoot && PathKey(path) == PathKey(config_->root))
                            Fail(DiagnosticCode::Unsupported, include.entry.source, "Select one MOD directory below the host recursive container, not the whole Mods container");
                        selectedByHost_ = true;
                        const auto relative = config_->root.lexically_relative(path);
                        for (const auto& part : relative)
                            for (const auto& pattern : hostExcludes_)
                                if (Glob(pattern, PathUtf8(part)))
                                    Fail(DiagnosticCode::Unsupported, include.entry.source, "Selected MOD is excluded by original host pattern: " + pattern);
                        activeRoot_ = config_->root;
                        if (request_.recursiveRoot) Recursive(config_->root, include.entry.source, 0);
                        else for (const auto& selected : request_.files)
                        {
                            const auto file = Resolve(selected, {selected, 1});
                            for (const auto& pattern : hostExcludes_)
                                if (Glob(pattern, PathUtf8(file.filename())))
                                    Fail(DiagnosticCode::Unsupported, include.entry.source, "Selected INI is excluded by original host pattern: " + pattern);
                            Read(file, false, include.entry.source, 0);
                        }
                    }
                    else if (hostDirective && include.entry.key == "include")
                    {
                        // Core Include chooses an explicit library file. Its
                        // parent is the authority for its own relative graph;
                        // the host parent never becomes a MOD resource sandbox.
                        if (Contains(config_->root, path) || Contains(path.parent_path(), config_->root))
                            Fail(DiagnosticCode::Unsupported, include.entry.source, "Host explicit Include overlaps selected MOD authority; use its recursive ingress only");
                        config_->host.settings.push_back({include.entry.source, "include", include.entry.key, include.entry.value,
                            "explicit original library; separate parent sandbox and unchanged required graph", HostSettingKind::Native});
                        activeRoot_ = path.parent_path();
                        Read(path, false, include.entry.source, 0);
                    }
                    else if (include.entry.key == "include") Read(path, false, include.entry.source, 0);
                    else Recursive(path, include.entry.source, 0);
                }
        }
        if (!config_->host.rootIni.empty() && !selectedByHost_)
            Fail(DiagnosticCode::MissingDependency, {config_->host.rootIni, 1}, "No original include_recursive container admits the explicit selected MOD");
        if (config_->files.empty() || (!config_->host.rootIni.empty() &&
            std::none_of(config_->files.begin(), config_->files.end(), [&](const auto& f) {
                return PathKey(f.packageRoot) == PathKey(config_->root); })))
            Fail(DiagnosticCode::MissingDependency, {config_->root, 1}, "Selected MOD has no admitted INI after original exclusions");
        std::map<std::string, unsigned> colors;
        for (const auto& file : config_->files)
            if (!colors[PathKey(file.path)]) CheckCycles(PathKey(file.path), colors, 0);
        return std::move(config_);
    }
};
} // namespace

LoadResult LoadConfig(const LoadRequest& request)
{
    try { return {Loader(request).Load(), {}}; }
    catch (const LoadFailure& e) { return {nullptr, {e.diagnostic}}; }
    catch (const std::exception& e) { return {nullptr, {{DiagnosticCode::Io, {request.root, 1}, e.what()}}}; }
}
} // namespace gimi
