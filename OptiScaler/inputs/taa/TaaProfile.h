// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <SimpleIni.h>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace taa {
enum class Mode { Off, Discover, Replace };
enum class Stage { Pixel, Compute };
enum class Units { Pixels, Uv, Ndc };
enum class Encoding { Linear, SignedSquare };
enum class MotionSampling { Center, NearestDepth5Max };
enum class JitterSource { ResolveConstants, MotionProjection };
enum class OutputRoute { TaaTarget, ObserveUpscale };
inline std::string Lower(std::string_view value) {
    std::string result(value);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
    return result;
}
inline std::optional<Mode> ParseMode(std::string_view text) {
    const auto value = Lower(text);
    if (value.empty() || value == "auto" || value == "off") return Mode::Off;
    if (value == "discover") return Mode::Discover;
    if (value == "replace") return Mode::Replace;
    return std::nullopt;
}
inline const char* ModeName(Mode mode) {
    switch (mode) { case Mode::Discover: return "Discover"; case Mode::Replace: return "Replace";
    default: return "Off"; }
}
struct Profile {
    JitterSource jitterSource = JitterSource::ResolveConstants;
    OutputRoute outputRoute = OutputRoute::TaaTarget;
    Stage stage = Stage::Pixel;
    uint64_t shaderHash = 0; // FNV-1a of the complete DXBC, NOT a 3DMigoto hash
    unsigned colorSlot = 0, depthSlot = 0, motionSlot = 0, outputSlot = 0;
    unsigned jitterSlot = 0, jitterOffsetX = 0, jitterOffsetY = 0;
    unsigned threadsX = 0, threadsY = 0;
    Units motionUnits = Units::Pixels, jitterUnits = Units::Pixels;
    Encoding encoding = Encoding::Linear;
    MotionSampling motionSampling = MotionSampling::Center;
    float motionScaleX = 0, motionScaleY = 0, jitterScaleX = 0, jitterScaleY = 0;
    bool motionJittered = false, depthInverted = false, hdr = false;
    // HDR is an SR create flag, not a declaration of the input transfer.
    // Automatic recipes retain their existing fallback without claiming a source declaration.
    bool hdrDeclared = false;
    bool operator==(const Profile&) const = default;
};
struct ProfileResult {
    std::optional<Profile> profile;
    std::string error;
};
// One strict schema for the runtime, UI reload, and fixtures. No engine-name or
// texture-format heuristic can authorize a temporal input contract.
inline ProfileResult ReadProfile(const CSimpleIniA& ini) {
    Profile p;
    ProfileResult result;
    const auto fail = [&](const char* key) { if (result.error.empty()) result.error = std::string("Invalid/missing TAA profile field: ") + key; };
    if(const auto* route=ini.GetValue("TAA.Profile","OutputRoute",nullptr)) {
        const auto value=Lower(route);
        if(value=="observe-upscale")p.outputRoute=OutputRoute::ObserveUpscale;
        else if(value!="taa-target")fail("OutputRoute");
    }
    const auto get = [&](const char* key) -> std::string_view {
        const auto* text = ini.GetValue("TAA.Profile", key, nullptr);
        if (!text || !*text) { fail(key); return {}; }
        return text;
    };
    const auto number = [&](const char* key, unsigned& out, unsigned max) {
        auto text = get(key); unsigned value = 0;
        const auto r = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || r.ec != std::errc{} || r.ptr != text.data() + text.size() || value > max) fail(key);
        else out = value;
    };
    const auto scalar = [&](const char* key, float& out) {
        const auto text = std::string(get(key)); char* end = nullptr;
        const auto value = std::strtof(text.c_str(), &end);
        if (text.empty() || end != text.c_str() + text.size() || !std::isfinite(value) || value == 0) fail(key);
        else out = value;
    };
    const auto boolean = [&](const char* key, bool& out) {
        const auto text = Lower(get(key));
        if (text == "true") out = true; else if (text == "false") out = false; else fail(key);
    };
    const auto units = [&](const char* key, Units& out) {
        const auto text = Lower(get(key));
        if (text == "pixels") out = Units::Pixels; else if (text == "uv") out = Units::Uv;
        else if (text == "ndc") out = Units::Ndc; else fail(key);
    };
    if (get("Schema") != "1") fail("Schema");
    const auto stage = Lower(get("Stage"));
    if (stage == "pixel") p.stage = Stage::Pixel; else if (stage == "compute") p.stage = Stage::Compute; else fail("Stage");
    const auto hash = get("ShaderHash");
    const auto r = std::from_chars(hash.data(), hash.data() + hash.size(), p.shaderHash, 16);
    if (hash.size() != 16 || r.ec != std::errc{} || r.ptr != hash.data() + hash.size() || !p.shaderHash) fail("ShaderHash");
    number("ColorSlot", p.colorSlot, 127); number("DepthSlot", p.depthSlot, 127); number("MotionSlot", p.motionSlot, 127);
    if (p.colorSlot == p.depthSlot || p.colorSlot == p.motionSlot || p.depthSlot == p.motionSlot) fail("distinct SRV slots");
    number("OutputSlot", p.outputSlot, 7);
    number("JitterSlot", p.jitterSlot, 13); number("JitterOffsetX", p.jitterOffsetX, 65532); number("JitterOffsetY", p.jitterOffsetY, 65532);
    if (p.jitterOffsetX % 4 || p.jitterOffsetY % 4 || p.jitterOffsetX == p.jitterOffsetY) fail("JitterOffsetX/Y");
    const auto encoding = Lower(get("MotionEncoding"));
    if (encoding == "linear") p.encoding = Encoding::Linear;
    else if (encoding == "signed-square") p.encoding = Encoding::SignedSquare; else fail("MotionEncoding");
    units("MotionUnits", p.motionUnits); units("JitterUnits", p.jitterUnits);
    scalar("MotionScaleX", p.motionScaleX); scalar("MotionScaleY", p.motionScaleY);
    scalar("JitterScaleX", p.jitterScaleX); scalar("JitterScaleY", p.jitterScaleY);
    boolean("MotionJittered", p.motionJittered); boolean("DepthInverted", p.depthInverted); boolean("HDR", p.hdr);
    if (result.error.empty()) p.hdrDeclared = true;
    if (Lower(get("History")) != "preserve-original") fail("History");
    if (p.stage == Stage::Compute) {
        number("ThreadsX", p.threadsX, 1024); number("ThreadsY", p.threadsY, 1024);
        if (!p.threadsX || !p.threadsY || p.threadsX * p.threadsY > 1024) fail("ThreadsX/Y");
    }
    if (result.error.empty()) result.profile = p;
    return result;
}
inline float PixelScale(Units units, unsigned extent, float scale) {
    return scale * (units == Units::Uv ? float(extent) : units == Units::Ndc ? float(extent) * 0.5f : 1.0f);
}
} // namespace taa
