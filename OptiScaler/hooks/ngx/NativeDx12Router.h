#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

// Both the Windows adapter and the standalone tests execute this state machine.
// Numeric handle ids are receipt data only; routing uses creation pointers.
namespace NgxInput
{
enum class Owner { Native, Selected, Backend, Unknown };

template <class Api> class Dx12Router
{
  public:
    using Handle = typename Api::Handle;
    using Feature = typename Api::Feature;
    using Result = typename Api::Result;

  private:
    struct Entry
    {
        Feature feature;
        unsigned id;
        Owner owner;
        bool evaluated = false;
        bool released = false;
        bool busy = false;
        std::atomic_bool conflicted { false };
        std::recursive_mutex mutex;
        Entry(Feature f, unsigned i, Owner o) : feature(f), id(i), owner(o) {}
    };
    Api& api;
    // Never hold the pointer-map lock across vendor calls: native worker
    // callbacks may run while the selected input thread waits for those workers.
    std::mutex mapMutex;
    std::unordered_map<const Handle*, std::shared_ptr<Entry>> handles;
    std::recursive_mutex inputMutex;
    std::mutex lifecycleMutex;
    unsigned activeCalls = 0;
    bool stopping = false;
    std::atomic<unsigned> selectedCount { 0 };
    std::atomic<unsigned> selectedCalls { 0 };
    std::atomic_bool quarantined { false };
    bool inputReady = false;

    // Admission is nonblocking across vendor calls. A worker callback must
    // never wait for a caller that is itself waiting for that worker.
    struct Operation
    {
        Dx12Router& router;
        bool admitted = false;
        explicit Operation(Dx12Router& r, bool nativeCleanup = false) : router(r)
        {
            std::lock_guard lock(router.lifecycleMutex);
            if (!router.stopping || nativeCleanup)
            {
                ++router.activeCalls;
                admitted = true;
            }
        }
        ~Operation()
        {
            if (admitted)
            {
                std::lock_guard lock(router.lifecycleMutex);
                --router.activeCalls;
            }
        }
        explicit operator bool() const { return admitted; }
    };
    struct StopOperation
    {
        Dx12Router& router;
        bool admitted = false;
        explicit StopOperation(Dx12Router& r) : router(r)
        {
            std::lock_guard lock(router.lifecycleMutex);
            if (!router.stopping && router.activeCalls == 0)
            {
                router.stopping = true;
                admitted = true;
            }
        }
        ~StopOperation()
        {
            if (admitted)
            {
                std::lock_guard lock(router.lifecycleMutex);
                router.stopping = false;
            }
        }
        explicit operator bool() const { return admitted; }
    };

    struct InputCall
    {
        std::atomic<unsigned>& count;
        explicit InputCall(std::atomic<unsigned>& c) : count(c) { ++count; }
        ~InputCall() { --count; }
    };
    struct EntryCall
    {
        Entry& entry;
        explicit EntryCall(Entry& e) : entry(e) { entry.busy = true; }
        ~EntryCall() { entry.busy = false; }
    };
    auto Find(const Handle* handle)
    {
        std::lock_guard lock(mapMutex);
        const auto it = handles.find(handle);
        return it == handles.end() ? std::shared_ptr<Entry>() : it->second;
    }
    bool Record(Handle* handle, Feature feature, Owner owner)
    {
        const auto entry = std::make_shared<Entry>(feature, api.Id(handle), owner);
        {
            std::lock_guard lock(mapMutex);
            const auto it = handles.find(handle);
            if (it != handles.end())
            {
                auto& old = *it->second;
                // Repeated identical Create is idempotent. Conflicting live
                // ownership is not evidence that its old context was released.
                if (!old.conflicted && old.id == entry->id && old.feature == feature && old.owner == owner) return true;
                old.conflicted = true;
                quarantined = true; // includes native-record -> new selected collision
                api.Receipt("OwnershipConflict/restart-required", Owner::Unknown, handle, std::nullopt, entry->id);
                return false;
            }
            handles.insert_or_assign(handle, entry);
            if (owner == Owner::Selected) ++selectedCount;
        }
        api.Receipt("Create", owner, handle, feature, entry->id);
        return true;
    }
    bool Valid(const std::shared_ptr<Entry>& entry, const Handle* handle)
    {
        if (entry->released || entry->conflicted) return false;
        if (api.Id(handle) == entry->id) return true;
        entry->conflicted = true;
        quarantined = true;
        api.Receipt("IdentityMismatch/restart-required", Owner::Unknown, handle, std::nullopt, api.Id(handle));
        return false;
    }

    template <class Admission, class NativeStop, class SelectedStop>
    Result ShutdownIf(Admission admission, NativeStop nativeStop, SelectedStop selectedStop)
    {
        // A native worker must not wait for its selected caller.
        if (HasSelected()) return api.Fail();
        StopOperation stop(*this);
        if (!stop) return api.Fail();
        std::unique_lock lock(inputMutex, std::try_to_lock);
        if (!lock.owns_lock()) return api.Fail();
        if (HasSelected() || !admission()) return api.Fail();
        const auto result = nativeStop();
        if (!api.Success(result)) return result;
        if (inputReady) selectedStop(result);
        inputReady = false;
        { std::lock_guard map(mapMutex); handles.clear(); }
        api.ProviderReady(false);
        api.ForgetInitDevice();
        return result;
    }

  public:
    explicit Dx12Router(Api& functions) : api(functions) {}
    bool HasSelected() const { return selectedCount.load() != 0 || selectedCalls.load() != 0 || quarantined.load(); }

    template <class NativeCall>
    Result InvokeNative(NativeCall call, bool nativeCleanup = false)
    {
        Operation operation(*this, nativeCleanup);
        return operation ? call() : api.Fail();
    }

    template <class Device, class NativeInit, class SelectedInit>
    Result Init(bool external, Device device, NativeInit nativeInit, SelectedInit selectedInit)
    {
        Operation operation(*this);
        if (!operation) return api.Fail();
        // A native worker may have clean TLS while the selected caller waits
        // for it. Refuse atomically before waiting for that caller's input lock.
        if (HasSelected()) return api.Fail();
        std::unique_lock lock(inputMutex, std::try_to_lock);
        if (!lock.owns_lock()) return api.Fail();
        if (HasSelected() || !api.AcceptInitDevice(device)) return api.Fail();
        InputCall busy(selectedCalls);
        const auto result = nativeInit();
        if (!api.Success(result)) return result;
        api.ObserveInitDevice(device);
        api.ProviderReady(true);
        if (external) inputReady = api.Success(selectedInit(result));
        return result; // optional input attachment cannot rewrite native success
    }

    template <class Command, class Parameters>
    Result Create(bool external, Command command, Feature feature, Parameters parameters, Handle** out,
                  bool nativeCleanup = false)
    {
        Operation operation(*this, nativeCleanup);
        if (!operation) return api.Fail();
        if (nativeCleanup && external) return api.Fail();
        if (!external || !out || !api.IsSelectedFeature(feature))
        {
            const auto result = api.NativeCreate(command, feature, parameters, out);
            if (api.Success(result) && out && *out &&
                !Record(*out, feature, external ? Owner::Native : Owner::Backend)) return api.Fail();
            return result;
        }
        if (quarantined.load()) return api.Fail();
        std::unique_lock lock(inputMutex, std::try_to_lock);
        if (!lock.owns_lock()) return api.Fail();
        if (quarantined.load()) return api.Fail();
        InputCall busy(selectedCalls);
        const auto device = api.AcquireCommandDevice(command);
        if (!device) return api.Fail();
        if (!inputReady) inputReady = api.AttachInput(device);
        if (!inputReady)
        {
            const auto result = api.NativeCreate(command, feature, parameters, out);
            if (api.Success(result) && *out && !Record(*out, feature, Owner::Native)) return api.Fail();
            return result;
        }
        // Project Create writes (*out)->Id if nonnull. Always use a local,
        // empty output, even if the caller left an old native handle in *out.
        Handle* created = nullptr;
        const auto result = api.SelectedCreate(command, feature, parameters, &created);
        if (created)
        {
            // A failing partial Create may have recorded GPU work. Retain its
            // owner and return its real result; do not immediately Release it.
            *out = created;
            if (!Record(created, feature, Owner::Selected)) return api.Fail();
        }
        return api.Success(result) && !created ? api.Fail() : result;
    }

    template <class Command, class Parameters, class Callback>
    Result Evaluate(bool external, Command command, const Handle* handle, Parameters parameters, Callback callback,
                    bool nativeCleanup = false)
    {
        Operation operation(*this, nativeCleanup);
        if (!operation) return api.Fail();
        const auto entry = Find(handle); // pointer ownership precedes caller eligibility
        if (!entry)
        {
            api.Receipt("Evaluate", external ? Owner::Unknown : Owner::Backend, handle, std::nullopt, std::nullopt);
            return api.NativeEvaluate(command, handle, parameters, callback);
        }
        if (nativeCleanup && entry->owner == Owner::Selected) return api.Fail();
        std::unique_lock input(inputMutex, std::defer_lock);
        if (entry->owner == Owner::Selected && !input.try_lock()) return api.Fail();
        std::unique_lock lock(entry->mutex, std::try_to_lock);
        if (!lock.owns_lock() || entry->busy) return api.Fail();
        if (!Valid(entry, handle)) return api.Fail();
        EntryCall busy(*entry);
        // A known handle fixes the route, not the command list's device. Reject
        // foreign-device input before any selected NR/SR/FG work or receipt.
        if (entry->owner == Owner::Selected && !api.AcceptCommandDevice(command)) return api.Fail();
        if (!entry->evaluated)
        {
            entry->evaluated = true;
            api.Receipt("Evaluate", entry->owner, handle, entry->feature, entry->id);
        }
        // No fallback Evaluate, even on selected failure.
        return entry->owner == Owner::Selected
            ? api.SelectedEvaluate(command, handle, parameters, callback)
            : api.NativeEvaluate(command, handle, parameters, callback);
    }

    Result Release(bool external, Handle* handle, bool nativeCleanup = false)
    {
        Operation operation(*this, nativeCleanup);
        if (!operation) return api.Fail();
        const auto entry = Find(handle);
        if (!entry)
        {
            api.Receipt("Release", external ? Owner::Unknown : Owner::Backend, handle, std::nullopt, std::nullopt);
            return api.NativeRelease(handle);
        }
        if (nativeCleanup && entry->owner == Owner::Selected) return api.Fail();
        std::unique_lock input(inputMutex, std::defer_lock);
        if (entry->owner == Owner::Selected && !input.try_lock()) return api.Fail();
        std::unique_lock lock(entry->mutex, std::try_to_lock);
        if (!lock.owns_lock() || entry->busy) return api.Fail();
        if (!Valid(entry, handle)) return api.Fail();
        EntryCall busy(*entry);
        const auto result = entry->owner == Owner::Selected ? api.SelectedRelease(handle) : api.NativeRelease(handle);
        if (api.Success(result))
        {
            entry->released = true;
            // Existing input Release destroys its context, but not its heap
            // handle. Delete only our proven selected allocation.
            if (entry->owner == Owner::Selected)
            {
                api.DeleteSelectedHandle(handle);
                --selectedCount;
            }
            std::lock_guard map(mapMutex);
            if (const auto it = handles.find(handle); it != handles.end() && it->second == entry) handles.erase(it);
        }
        api.Receipt("Release", entry->owner, handle, entry->feature, entry->id);
        return result;
    }

    template <class NativeStop, class SelectedStop>
    Result Shutdown(bool /*external*/, NativeStop nativeStop, SelectedStop selectedStop)
    {
        return ShutdownIf([] { return true; }, nativeStop, selectedStop);
    }

    template <class Device, class NativeStop, class SelectedStop>
    Result ShutdownDevice(bool /*external*/, Device device, NativeStop nativeStop, SelectedStop selectedStop)
    {
        return ShutdownIf([&] { return api.AcceptShutdownDevice(device); }, nativeStop, selectedStop);
    }
};
}
