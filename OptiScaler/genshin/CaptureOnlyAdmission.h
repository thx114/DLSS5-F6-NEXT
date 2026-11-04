#pragma once
#include <cstdint>

namespace NoSrCapture
{
enum class Producer : std::uint8_t { Unknown, EngineMv, OpticalFlow, CameraDepthReprojection };
enum class Confidence : std::uint8_t { Unknown, Candidate, ValidatedEstimate, AuthoritativeProfile };
enum class State : std::uint8_t { Empty, Observed, Copied, Ready, Admitted, Retired };
enum class Reason : std::uint8_t
{
    None, VersionMismatch, MissingOwner, WrongOwner, WrongDevice, WrongContext, WrongEpoch,
    MissingFrame, NonMonotonicFrame, MissingNativeSize, InvalidRect, MissingJitter,
    UnknownColour, UnknownDepth, UnknownMotion, UnsupportedProducer, UntrustedMotion,
    StaleResource, DuplicateFrame, NotReady, Retired
};
struct Extent { std::uint32_t width = 0, height = 0; };
struct Rect { std::uint32_t left = 0, top = 0, width = 0, height = 0; };
struct Signal
{
    std::uint32_t version = 1;
    Producer producer = Producer::Unknown;
    Confidence confidence = Confidence::Unknown;
    std::uint64_t producerId = 0, device = 0, context = 0, swapchain = 0, ownerThread = 0, instance = 0;
    std::uint64_t deviceEpoch = 0, resourceGeneration = 0, producerEpoch = 0;
    std::uint64_t renderedFrame = 0, sourceFrame = 0, submissionSerial = 0, presentSerial = 0;
    Extent nativeSize{}, outputSize{};
    Rect colorRect{}, depthRect{}, motionRect{};
    float jitterX = 0, jitterY = 0, previousJitterX = 0, previousJitterY = 0;
    bool jitterKnown = false, previousJitterKnown = false, colourKnown = false, depthKnown = false, motionKnown = false;
    std::uint32_t colourEncoding = 0, depthFormat = 0, motionFormat = 0;
    bool engineDirectionCurrentToPrevious = false, depthReversed = false;
};
struct Ticket
{
    Signal signal{};
    std::uint64_t captureGeneration = 0;
    State state = State::Empty;
    bool originalDrawPreserved = true;
    bool srProviderCalled = false;
};
struct Result { bool accepted = false; Reason reason = Reason::None; };
class Admission final
{
    std::uint32_t contractVersion_ = 1;
    std::uint64_t nextGeneration_ = 1, lastFrame_ = 0, lastGeneration_ = 0;
    Ticket ticket_{};
public:
    Result Observe(const Signal& signal) noexcept;
    Result CopyComplete(std::uint64_t device, std::uint64_t context, std::uint64_t resourceGeneration) noexcept;
    Result Admit(std::uint64_t device, std::uint64_t context, std::uint64_t resourceGeneration) noexcept;
    Result Retire() noexcept;
    const Ticket& ticket() const noexcept { return ticket_; }
    State state() const noexcept { return ticket_.state; }
    void Reset() noexcept;
};
const char* ReasonText(Reason reason) noexcept;
const char* ProducerText(Producer producer) noexcept;
}
