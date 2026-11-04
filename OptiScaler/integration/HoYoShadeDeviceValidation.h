#pragma once
#include <array>
#include <cstdint>

namespace HoYoShade::DeviceValidation
{
// Scalar evidence only. Never dereference these addresses after the call's
// COM owners leave scope. Sampled identity is never execution authority.
enum class Role : unsigned { Color, ConsumerQueue, ProducerQueue, Fence, Chain, Count };
enum class Site : unsigned { Stage, PresentOriginal, Record, Binding, Count };
enum class Predicate : unsigned
{
    None, ColorPresent, ConsumerQueuePresent, ChainPresent,
    ColorGetDevice, ConsumerGetDevice, ProducerGetDevice, FenceGetDevice, ChainGetDevice,
    ConsumerDeviceEqual, ProducerDeviceEqual, FenceDeviceEqual, RecordDeviceEqual,
    BindingDevicePresent, BindingQueuePresent, BindingFactoryPresent, BindingConfigPresent,
    BindingDeviceEqual, BindingQueueEqual,
    DeviceHealthy, FenceCompletedValid, PresentIndexed, PresentBuffer, PresentCreateFence,
    PresentFenceValue, PresentSignal, ProducerWait, SubmittedFenceDomain
};
constexpr const char* Text(Role role) noexcept
{
    constexpr const char* names[]{"color", "consumer_queue", "producer_queue", "fence", "chain"};
    return static_cast<unsigned>(role) < static_cast<unsigned>(Role::Count) ? names[static_cast<unsigned>(role)] : "unknown";
}
constexpr const char* Text(Site site) noexcept
{
    constexpr const char* names[]{"RenderStage", "RenderPresentOriginal", "RecordStage", "EnsureNativeBinding"};
    return static_cast<unsigned>(site) < static_cast<unsigned>(Site::Count) ? names[static_cast<unsigned>(site)] : "unknown";
}
constexpr const char* Text(Predicate predicate) noexcept
{
    switch (predicate) {
#define HYS_PREDICATE(value, text) case Predicate::value: return text
    HYS_PREDICATE(None, "none");
    HYS_PREDICATE(ColorPresent, "color.nonnull");
    HYS_PREDICATE(ConsumerQueuePresent, "consumer_queue.nonnull");
    HYS_PREDICATE(ChainPresent, "chain.nonnull");
    HYS_PREDICATE(ColorGetDevice, "color.GetDevice");
    HYS_PREDICATE(ConsumerGetDevice, "consumer_queue.GetDevice");
    HYS_PREDICATE(ProducerGetDevice, "producer_queue.GetDevice");
    HYS_PREDICATE(FenceGetDevice, "fence.GetDevice");
    HYS_PREDICATE(ChainGetDevice, "chain.GetDevice");
    HYS_PREDICATE(ConsumerDeviceEqual, "color_device==consumer_device");
    HYS_PREDICATE(ProducerDeviceEqual, "color_device==producer_device");
    HYS_PREDICATE(FenceDeviceEqual, "color_device==fence_device");
    HYS_PREDICATE(SubmittedFenceDomain, "exact-native-submitted-fence-domain");
    HYS_PREDICATE(RecordDeviceEqual, "color_device==request_device");
    HYS_PREDICATE(BindingDevicePresent, "binding_device.nonnull");
    HYS_PREDICATE(BindingQueuePresent, "binding_queue.nonnull");
    HYS_PREDICATE(BindingFactoryPresent, "binding_factory.nonnull");
    HYS_PREDICATE(BindingConfigPresent, "binding_config.nonnull");
    HYS_PREDICATE(BindingDeviceEqual, "binding_device==color_device");
    HYS_PREDICATE(BindingQueueEqual, "binding_queue==request_queue");
    HYS_PREDICATE(DeviceHealthy, "device.GetDeviceRemovedReason");
    HYS_PREDICATE(FenceCompletedValid, "fence.GetCompletedValue!=UINT64_MAX");
    HYS_PREDICATE(PresentIndexed, "chain.QueryInterface.IDXGISwapChain3");
    HYS_PREDICATE(PresentBuffer, "chain.GetBuffer");
    HYS_PREDICATE(PresentCreateFence, "device.CreateFence");
    HYS_PREDICATE(PresentFenceValue, "producer.value<UINT64_MAX-1");
    HYS_PREDICATE(PresentSignal, "consumer_queue.Signal");
    HYS_PREDICATE(ProducerWait, "consumer_queue.Wait");
#undef HYS_PREDICATE
    }
    return "unknown";
}
struct Snapshot
{
    Predicate first = Predicate::None;
    int32_t failureHr = 0;
    bool hrKnown = false, nullSuccess = false;
    uint32_t queried = 0;
    std::array<int32_t, static_cast<unsigned>(Role::Count)> hrs{};
    std::array<uint64_t, static_cast<unsigned>(Role::Count)> devices{};
    bool removedKnown = false, completedKnown = false;
    int32_t removedHr = 0;
    uint64_t completed = 0;
    uint64_t relatedDevice = 0, relatedQueue = 0;
    bool operator==(const Snapshot&) const = default;

    bool Fail(Predicate predicate, int32_t hr = 0, bool known = false, bool null = false) noexcept
    {
        if (first == Predicate::None) { first = predicate; failureHr = hr; hrKnown = known; nullSuccess = null; }
        return false;
    }
    bool Check(bool condition, Predicate predicate) noexcept { return condition || Fail(predicate); }
    bool Api(Predicate predicate, int32_t hr, bool pointerPresent = true) noexcept
    {
        return (hr >= 0 && pointerPresent) || Fail(predicate, hr, true, hr >= 0 && !pointerPresent);
    }
    bool Device(Role role, int32_t hr, uint64_t pointer) noexcept
    {
        const auto index = static_cast<unsigned>(role);
        queried |= 1u << index; hrs[index] = hr; devices[index] = pointer;
        constexpr Predicate predicates[]{Predicate::ColorGetDevice, Predicate::ConsumerGetDevice,
            Predicate::ProducerGetDevice, Predicate::FenceGetDevice, Predicate::ChainGetDevice};
        return Api(predicates[index], hr, pointer != 0);
    }
};
struct DeviceQuery { int32_t hr; uint64_t pointer; };
// Access owns returned COM interfaces. Preserve the original short-circuit
// call order: diagnostics add no GetDevice, health or fence polling.
template<class Access> bool ValidateStage(Access& access, Snapshot& evidence)
{
    for (unsigned index = 0; index < 4; ++index) {
        const auto role = static_cast<Role>(index);
        const auto query = access.GetDevice(role);
        if (!evidence.Device(role, query.hr, query.pointer)) return false;
    }
    const auto& d = evidence.devices;
    if (!evidence.Check(d[0] == d[1], Predicate::ConsumerDeviceEqual) ||
        !evidence.Check(d[0] == d[2], Predicate::ProducerDeviceEqual)) return false;
    // Only the production Access with a private native submission capability
    // can validate a forwarded fence. Scalar address pairs grant no authority.
    bool submittedDomain = false;
    if constexpr (requires { access.SubmittedFenceDomain(); }) {
        if (access.HasSubmission()) {
            submittedDomain = access.SubmittedFenceDomain();
            if (!evidence.Check(submittedDomain, Predicate::SubmittedFenceDomain)) return false;
        }
    }
    if (!submittedDomain && !evidence.Check(d[0] == d[3], Predicate::FenceDeviceEqual)) return false;
    evidence.removedKnown = true; evidence.removedHr = access.RemovedReason();
    if (!evidence.Api(Predicate::DeviceHealthy, evidence.removedHr)) return false;
    evidence.completedKnown = true; evidence.completed = access.CompletedValue();
    return evidence.Check(evidence.completed != UINT64_MAX, Predicate::FenceCompletedValid);
}
}
