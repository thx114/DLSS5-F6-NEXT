#include "CaptureOnlyConsumer.h"

namespace NoSrConsumer
{
Receipt Consumer::BeforeNativeDraw(const il2cpp_callsite::RenderToken& token,
                                   const NoSrCapture::Signal& signal, CopyOps ops) noexcept
{
    receipt_ = {};
    receipt_.hookSeen = true;
    receipt_.originalDrawPreserved = true;
    receipt_.instance = token.params.instance;
    receipt_.renderGeneration = token.generation;
    if (!token.generation || !token.params.instance ||
        signal.instance != token.params.instance ||
        signal.renderedFrame != token.params.frame_index ||
        signal.sourceFrame != token.params.frame_index ||
        signal.nativeSize.width != token.params.render_w ||
        signal.nativeSize.height != token.params.render_h ||
        signal.outputSize.width != token.params.display_w ||
        signal.outputSize.height != token.params.display_h)
    {
        receipt_.reason = Reason::TokenMismatch;
        return receipt_;
    }
    receipt_.captureReached = true;
    const auto observed = admission_.Observe(signal);
    receipt_.admissionReason=observed.reason;
    if (!observed.accepted)
    {
        receipt_.reason = Reason::AdmissionRejected;
        receipt_.state = admission_.state();
        return receipt_;
    }
    receipt_.copyAttempted = true;
    if (!ops.copy || !ops.copy(ops.opaque, signal, token))
    {
        receipt_.reason = Reason::CopyFailed;
        receipt_.state = admission_.state();
        return receipt_;
    }
    receipt_.presentSerial=signal.presentSerial;
    if(ops.asynchronous)
    {
        receipt_.state=admission_.state();
        receipt_.reason=Reason::CopyPending;
        return receipt_;
    }
    return CompleteCopy(signal.device,signal.context,signal.resourceGeneration);
}

Receipt Consumer::CompleteCopy(std::uint64_t device,std::uint64_t context,
                              std::uint64_t resourceGeneration) noexcept
{
    const auto copied = admission_.CopyComplete(device,context,resourceGeneration);
    if (!copied.accepted)
    {
        receipt_.reason = Reason::CopyFailed;
        receipt_.state = admission_.state();
        return receipt_;
    }
    receipt_.copyCompleted=true;
    const auto admitted = admission_.Admit(device,context,resourceGeneration);
    receipt_.admitted = admitted.accepted;
    receipt_.state = admission_.state();
    receipt_.reason = admitted.accepted ? Reason::None : Reason::AdmissionRejected;
    return receipt_;
}

Receipt Consumer::OnPresent(std::uint64_t instance, std::uint64_t renderGeneration,
                            std::uint64_t presentSerial) noexcept
{
    if (!receipt_.admitted || receipt_.instance != instance ||
        receipt_.renderGeneration != renderGeneration || receipt_.presentSerial != presentSerial)
    {
        receipt_.reason = Reason::PresentMismatch;
        return receipt_;
    }
    const auto retired = admission_.Retire();
    receipt_.state = admission_.state();
    receipt_.admitted = receipt_.state == NoSrCapture::State::Admitted;
    receipt_.reason = retired.accepted ? Reason::Retired : Reason::PresentMismatch;
    return receipt_;
}

Receipt Consumer::Reset() noexcept
{
    admission_.Reset();
    receipt_ = {};
    return receipt_;
}
Receipt Consumer::OnCompletedPresent(std::uint64_t instance,std::uint64_t generation,std::uint64_t serial) noexcept
{
    if(!receipt_.admitted || receipt_.instance!=instance || receipt_.renderGeneration!=generation || !serial ||
        receipt_.state==NoSrCapture::State::Retired){receipt_.reason=Reason::PresentMismatch;return receipt_;}
    const auto retired=admission_.Retire();receipt_.state=admission_.state();
    receipt_.admitted = receipt_.state == NoSrCapture::State::Admitted;
    if(retired.accepted)receipt_.presentSerial=serial;
    receipt_.reason=retired.accepted?Reason::Retired:Reason::PresentMismatch;return receipt_;
}
}
