#pragma once
#include <cstdint>
#include "CaptureOnlyAdmission.h"
#include "legacy/Il2CppCallSiteHook.h"

namespace NoSrConsumer
{
enum class Reason : std::uint8_t { None, TokenMismatch, CopyFailed, PresentMismatch, AdmissionRejected, Retired,
    CopyPending, ProducerUnavailable, ProducerMismatch };
struct CopyOps
{
    bool (*copy)(void*, const NoSrCapture::Signal&, const il2cpp_callsite::RenderToken&) noexcept = nullptr;
    void* opaque = nullptr;
    bool asynchronous = false;
};
struct Receipt
{
    bool hookSeen = false, captureReached = false, copyAttempted = false, copyCompleted = false;
    bool admitted = false, originalDrawPreserved = true, srProviderCalled = false;
    std::uint64_t instance = 0, renderGeneration = 0, presentSerial = 0;
    NoSrCapture::State state = NoSrCapture::State::Empty;
    Reason reason = Reason::None;
    NoSrCapture::Reason admissionReason=NoSrCapture::Reason::None;
};
class Consumer final
{
    NoSrCapture::Admission admission_;
    Receipt receipt_{};
public:
    Receipt BeforeNativeDraw(const il2cpp_callsite::RenderToken& token,
                             const NoSrCapture::Signal& signal, CopyOps ops) noexcept;
    Receipt OnPresent(std::uint64_t instance, std::uint64_t renderGeneration,
                      std::uint64_t presentSerial) noexcept;
    Receipt CompleteCopy(std::uint64_t device,std::uint64_t context,
                         std::uint64_t resourceGeneration) noexcept;
    Receipt OnCompletedPresent(std::uint64_t instance,std::uint64_t renderGeneration,
                               std::uint64_t actualSerial) noexcept;
    Receipt Reset() noexcept;
    const Receipt& receipt() const noexcept { return receipt_; }
};
}
