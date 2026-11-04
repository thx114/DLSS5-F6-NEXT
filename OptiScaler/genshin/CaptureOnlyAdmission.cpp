#include "CaptureOnlyAdmission.h"
namespace NoSrCapture
{
namespace
{
bool FullRect(const Rect& r, const Extent& e) noexcept
{ return e.width && e.height && r.left == 0 && r.top == 0 && r.width == e.width && r.height == e.height; }
bool OwnerFields(const Signal& s) noexcept
{ return s.producerId && s.device && s.context && s.swapchain && s.ownerThread && s.instance &&
         s.deviceEpoch && s.resourceGeneration && s.producerEpoch; }
}
Result Admission::Observe(const Signal& s) noexcept
{
    ticket_ = {};
    if (s.version != contractVersion_) return {false, Reason::VersionMismatch};
    if (!OwnerFields(s)) return {false, Reason::MissingOwner};
    // Present completion is supplied later by the actual native caller. Do not
    // require or infer an upcoming successful Present during source observation.
    if (!s.renderedFrame || !s.sourceFrame || !s.submissionSerial || s.sourceFrame != s.renderedFrame)
        return {false, Reason::MissingFrame};
    if (s.renderedFrame <= lastFrame_) return {false, s.renderedFrame == lastFrame_ ? Reason::DuplicateFrame : Reason::NonMonotonicFrame};
    if (!s.nativeSize.width || !s.nativeSize.height || !s.outputSize.width || !s.outputSize.height) return {false, Reason::MissingNativeSize};
    if (!FullRect(s.colorRect, s.nativeSize) || !FullRect(s.depthRect, s.nativeSize) || !FullRect(s.motionRect, s.nativeSize))
        return {false, Reason::InvalidRect};
    if (!s.jitterKnown || !s.previousJitterKnown) return {false, Reason::MissingJitter};
    if (!s.colourKnown || !s.colourEncoding) return {false, Reason::UnknownColour};
    if (!s.depthKnown || !s.depthFormat) return {false, Reason::UnknownDepth};
    if (!s.motionKnown || !s.motionFormat) return {false, Reason::UnknownMotion};
    if (s.producer == Producer::Unknown) return {false, Reason::UnsupportedProducer};
    if (s.producer == Producer::EngineMv &&
        (s.confidence != Confidence::AuthoritativeProfile || !s.engineDirectionCurrentToPrevious))
        return {false, Reason::UntrustedMotion};
    if (s.producer == Producer::OpticalFlow && s.confidence < Confidence::ValidatedEstimate)
        return {false, Reason::UntrustedMotion};
    ticket_.signal = s;
    ticket_.captureGeneration = nextGeneration_++;
    ticket_.state = State::Observed;
    return {true, Reason::None};
}
Result Admission::CopyComplete(std::uint64_t device, std::uint64_t context, std::uint64_t resourceGeneration) noexcept
{
    if (ticket_.state != State::Observed) return {false, Reason::NotReady};
    if (device != ticket_.signal.device) return {false, Reason::WrongDevice};
    if (context != ticket_.signal.context) return {false, Reason::WrongContext};
    if (resourceGeneration != ticket_.signal.resourceGeneration) return {false, Reason::StaleResource};
    ticket_.state = State::Copied;
    ticket_.state = State::Ready;
    return {true, Reason::None};
}
Result Admission::Admit(std::uint64_t device, std::uint64_t context, std::uint64_t resourceGeneration) noexcept
{
    if (ticket_.state != State::Ready) return {false, Reason::NotReady};
    if (device != ticket_.signal.device) return {false, Reason::WrongDevice};
    if (context != ticket_.signal.context) return {false, Reason::WrongContext};
    if (resourceGeneration != ticket_.signal.resourceGeneration) return {false, Reason::StaleResource};
    ticket_.state = State::Admitted;
    lastFrame_ = ticket_.signal.renderedFrame;
    lastGeneration_ = ticket_.captureGeneration;
    return {true, Reason::None};
}
Result Admission::Retire() noexcept
{
    if (ticket_.state == State::Empty || ticket_.state == State::Retired) return {false, Reason::Retired};
    ticket_.state = State::Retired;
    return {true, Reason::None};
}
void Admission::Reset() noexcept { ticket_ = {}; lastFrame_ = 0; lastGeneration_ = 0; }
const char* ReasonText(Reason r) noexcept
{
    static constexpr const char* names[] = {"none","version mismatch","missing owner","wrong owner","wrong device","wrong context",
        "wrong epoch","missing frame","non-monotonic frame","missing native size","invalid rect","missing jitter",
        "unknown colour","unknown depth","unknown motion","unsupported producer","untrusted motion","stale resource",
        "duplicate frame","not ready","retired"};
    return names[static_cast<unsigned>(r)];
}
const char* ProducerText(Producer p) noexcept
{
    switch (p) { case Producer::EngineMv: return "engineMV"; case Producer::OpticalFlow: return "opticalFlow";
    case Producer::CameraDepthReprojection: return "cameraDepthReprojection"; default: return "unknown"; }
}
}
