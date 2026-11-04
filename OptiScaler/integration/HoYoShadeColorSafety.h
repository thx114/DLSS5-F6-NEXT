#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
namespace HoYoShade {
// NativeTyped is a processing contract, never an SDR/linear transfer claim.
// Copies preserve typed buffer values; selected effect sRGB states remain
// governed by the fixed API20 pair of RTVs, without adapter tone mapping.
enum class ColorMode : unsigned { TransferQualified, GenshinNativeTypedBeforeSr, GenshinNativeTypedBeforeNr };
inline const char* Text(ColorMode v) noexcept {
    switch (v) {
    case ColorMode::TransferQualified: return "transfer-qualified";
    case ColorMode::GenshinNativeTypedBeforeSr: return "genshin-native-typed-before-sr";
    case ColorMode::GenshinNativeTypedBeforeNr: return "genshin-native-typed-before-nr-finished-picture";
    default: return "invalid";
    }
}
enum class NativeColorAdmission : unsigned {
    NotRequested, ExactNativeTyped, EndpointMismatch, WrongGame, ProducerUnavailable,
    InvocationMismatch, OwnerMismatch, ReceiptMismatch, TransportMismatch, FormatMismatch, HdrUnsupported,
    ExactNativePresentPicture, PresentEndpointMismatch, PresentScopeUnavailable, PresentPictureMismatch,
    CopyProvenanceMismatch, RawColorSpaceUnsupported, SubmissionMismatch,
    // Only authenticated BeforeNR source29 -> shared28 CopyResource values;
    // neither transfer-known nor an expansion of BeforeSR format authority.
    ExactNativePresentSrgbCopy
};
inline const char* Text(NativeColorAdmission v) noexcept {
    switch (v) {
    case NativeColorAdmission::NotRequested: return "not-requested";
    case NativeColorAdmission::ExactNativeTyped: return "exact-native-typed-transfer-unknown";
    case NativeColorAdmission::EndpointMismatch: return "before-sr-endpoint-mismatch";
    case NativeColorAdmission::WrongGame: return "exact-genshin-executable-required";
    case NativeColorAdmission::ProducerUnavailable: return "current-producer-unavailable";
    case NativeColorAdmission::InvocationMismatch: return "current-invocation-mismatch";
    case NativeColorAdmission::OwnerMismatch: return "exact-owner-or-generation-mismatch";
    case NativeColorAdmission::ReceiptMismatch: return "current-resource-receipt-mismatch";
    case NativeColorAdmission::TransportMismatch: return "exact-input-transport-mismatch";
    case NativeColorAdmission::FormatMismatch: return "native-rgba8-typed-copy-format-required";
    case NativeColorAdmission::HdrUnsupported: return "known-hdr-not-supported";
    case NativeColorAdmission::ExactNativePresentPicture: return "exact-present-picture-native-typed-transfer-unknown";
    case NativeColorAdmission::ExactNativePresentSrgbCopy: return "exact-present-picture-rgba8-srgb-to-unorm-copy-transfer-unknown";
    case NativeColorAdmission::PresentEndpointMismatch: return "before-nr-finished-picture-endpoint-mismatch";
    case NativeColorAdmission::PresentScopeUnavailable: return "current-authenticated-present-color-scope-required";
    case NativeColorAdmission::PresentPictureMismatch: return "current-final-picture-or-owner-mismatch";
    case NativeColorAdmission::CopyProvenanceMismatch: return "exact-final-picture-shared-copy-required";
    case NativeColorAdmission::RawColorSpaceUnsupported: return "native-raw-colorspace-outside-initial-contract";
    case NativeColorAdmission::SubmissionMismatch: return "current-native-submission-receipt-required";
    default: return "invalid";
    }
}
enum class ConsumerAccess : unsigned { Safe, Quarantined, Unavailable };
// Physical identities, not a CPU frame counter. The owning implementation pins
// these COM objects. A CPU generation change never revokes an old denial.
struct ConsumerEndpoint {
    uint64_t chain = 0, device = 0, queue = 0, color = 0, generation = 0;
};
class ConsumerQuarantine {
public:
    static constexpr size_t Capacity = 32;
    size_t Add(const ConsumerEndpoint& key) noexcept {
        for (size_t i=0; i<count; ++i)
            if (keys[i].chain == key.chain && keys[i].device == key.device &&
                keys[i].queue == key.queue && keys[i].color == key.color && keys[i].generation == key.generation) return i;
        if (count == Capacity || !key.device || !key.color || !key.generation) {
            overflow = true; return Capacity;
        }
        keys[count] = key; return count++;
    }
    ConsumerAccess Check(const ConsumerEndpoint& key) const noexcept {
        if (overflow) return ConsumerAccess::Unavailable;
        for (size_t i=0; i<count; ++i)
            if ((key.chain && keys[i].chain == key.chain) ||
                (key.color && keys[i].color == key.color && keys[i].device == key.device))
                return ConsumerAccess::Quarantined;
        return ConsumerAccess::Safe;
    }
private:
    std::array<ConsumerEndpoint, Capacity> keys{};
    size_t count = 0;
    bool overflow = false;
    // Deliberately no CPU Reset/Resize/Present-complete mutator. Unknown state
    // AND retirement require a restoration proof this adapter does not have.
};
// Requested=false must still run Enabled(): it owns the legacy on->off release.
template<class Enabled> bool AdmitNativeFrame(bool requested, Enabled&& enabled) {
    const bool active = enabled();
    return active && requested;
}
}
