#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace PersistentSr
{
// Exact bytes, never a hash. Presence and type are part of the recipe.
struct Atom
{
    std::string name;
    std::uint32_t type = 0;
    std::vector<std::uint8_t> value;
    bool operator==(const Atom&) const = default;
};
template<class T> Atom Scalar(std::string name, std::uint32_t type, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    Atom a { std::move(name), type, std::vector<std::uint8_t>(sizeof(T)) };
    std::memcpy(a.value.data(), &value, sizeof(T));
    return a;
}
struct Recipe
{
    std::vector<Atom> parameters; // Sorted complete typed create map, before provider mutation.
    std::vector<Atom> configuration;
    bool operator==(const Recipe&) const = default;
};
inline bool ConfigurationMatches(const std::vector<Atom>& requested, const std::vector<Atom>& effective,
                                 const std::vector<Atom>& current)
{
    return current == requested || current == effective;
}
inline bool MissingExposureTransition(bool wasAuto, bool wasChanged, bool isAuto, bool isChanged,
                                      bool featureAuto, bool sameBackendRequest, bool sameConfiguration)
{
    return !wasAuto && !wasChanged && isAuto && isChanged && !featureAuto &&
           sameBackendRequest && sameConfiguration;
}
struct Binding
{
    std::uintptr_t device11 = 0, context11 = 0, device12 = 0, queue12 = 0, module = 0;
    std::uint64_t providerGeneration = 0;
    bool operator==(const Binding&) const = default;
};
struct Key
{
    Recipe recipe;
    Binding binding;
    std::optional<bool> autoExposure;
    bool operator==(const Key&) const = default;
};
enum class Status { Reused, Created, Reserved, Unknown, Stale, Mismatch, Full, HandlesFull, Closed };

// All operations, including backend calls, must hold Guard(). A process-pinned
// Pool has no timer/eviction/destruction path. Capacity includes retired owners.
template<class Owner, class Handle, std::size_t Capacity = 2, std::size_t MaxHandles = 4096> class Pool
{
  public:
    struct Slot
    {
        std::unique_ptr<Owner> owner;
        Key key;
        std::uint32_t backendId = 0;
        bool active = false, reusable = false, reserved = false;
    };
    struct Lease
    {
        Handle handle {};
        std::uint32_t id = 0;
        std::size_t slot = 0;
        bool live = false;
    };
    struct Result
    {
        Status status = Status::Unknown;
        Handle* handle = nullptr;
        Slot* slot = nullptr;
        Lease* lease = nullptr;
    };
    // Serialize full exported operations and reject recursive provider callbacks.
    // A recursive mutex alone would allow Release/Create inside an Evaluate.
    class Entry
    {
      public:
        explicit Entry(Pool& p) : pool(p), guard(p.mutex), entered(!p.busy)
        { if (entered) pool.busy = true; }
        ~Entry() { if (entered) pool.busy = false; }
        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;
        explicit operator bool() const { return entered; }
      private:
        Pool& pool;
        std::unique_lock<std::recursive_mutex> guard;
        bool entered;
    };
    bool Closed() const { return closed; }
    std::size_t OwnerCount() const { return ownerCount; }
    std::size_t HandleCount() const { return handleCount; }
    Slot& OwnerAt(std::size_t index) { return slots.at(index); }
    bool HasSpareHandle() const { return !closed && handleCount < MaxHandles; }
    bool CanCreate() const { return !closed && ownerCount < Capacity && handleCount < MaxHandles; }
    bool HasSpareOwner() const { return !closed && ownerCount < Capacity; }
    bool OwnsId(std::uint32_t id) const { return IdUsed(id); }

    Result Lookup(const Handle* pointer)
    {
        // Issued leases occupy a fixed array and keep their addresses forever.
        // Index that storage directly: SR Create/Release churn must not make
        // every Evaluate scan all prior tombstones. Do not dereference an
        // unrecognized caller pointer or accept an ID-only/copy alias.
        if (!pointer || handleCount == 0) return {};
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        const auto first = reinterpret_cast<std::uintptr_t>(&leases[0].handle);
        if (address < first) return {};
        const auto offset = address - first;
        if (offset % sizeof(Lease) != 0) return {};
        const auto index = offset / sizeof(Lease);
        if (index >= handleCount) return {};
        auto& lease = leases[index];
        if (pointer != &lease.handle) return {};
        if (closed || !lease.live || lease.handle.Id != lease.id)
            return { Status::Stale, &lease.handle, nullptr, &lease };
        auto& slot = slots[lease.slot];
        if (!slot.owner || !slot.active || !slot.reusable)
            return { Status::Stale, &lease.handle, nullptr, &lease };
        return { Status::Reused, &lease.handle, &slot, &lease };
    }
    Result Acquire(const Key& key, std::uint32_t logicalId)
    {
        if (closed) return { Status::Closed };
        if (handleCount == MaxHandles) return { Status::HandlesFull };
        for (std::size_t i = 0; i < ownerCount; ++i)
        {
            auto& slot = slots[i];
            if (slot.owner && slot.reusable && !slot.active && slot.key == key)
                return Issue(i, logicalId, Status::Reused);
        }
        return { ownerCount == Capacity ? Status::Full : Status::Mismatch };
    }
    // Own the empty Owner and capacity BEFORE construction/Init can submit GPU
    // work. Failed initialization remains pinned in this nonreusable slot.
    Result Reserve(const Key& key, std::uint32_t backendId, std::unique_ptr<Owner>& owner)
    {
        if (closed) return { Status::Closed };
        if (ownerCount == Capacity) return { Status::Full };
        if (!owner || IdUsed(backendId)) return { Status::Mismatch };
        const auto index = ownerCount++;
        auto& slot = slots[index];
        slot.owner = std::move(owner); slot.key = key; slot.backendId = backendId; slot.reserved = true;
        return { Status::Reserved, nullptr, &slot };
    }
    Result Publish(Slot* slot, std::uint32_t logicalId)
    {
        if (closed) return { Status::Closed };
        if (handleCount == MaxHandles) return { Status::HandlesFull };
        if (IdUsed(logicalId)) return { Status::Mismatch };
        for (std::size_t i = 0; i < ownerCount; ++i)
            if (&slots[i] == slot && slot->owner && slot->reserved)
            {
                slot->reusable = true; slot->reserved = false;
                return Issue(i, logicalId, Status::Created);
            }
        return { Status::Mismatch };
    }
    Status Release(const Handle* handle)
    {
        auto result = Lookup(handle);
        if (result.status != Status::Reused) return result.status;
        result.lease->live = false;
        result.slot->active = false;
        return Status::Reused;
    }
    void FailReservation(Slot* slot)
    {
        for (std::size_t i = 0; i < ownerCount; ++i)
            if (&slots[i] == slot && slot->reserved) slot->reserved = false;
    }
    void Retire(const Handle* handle)
    {
        auto result = Lookup(handle);
        if (result.status != Status::Reused) return;
        result.slot->reusable = false; result.slot->active = false; result.lease->live = false;
    }
    // Used once for manual -> learned auto exposure. Old owner/resources stay
    // pinned, replacement has a fresh internal ID, external handle stays stable.
    Result PublishReplacement(const Handle* handle, Slot* fresh)
    {
        auto old = Lookup(handle);
        if (old.status != Status::Reused) return old;
        for (std::size_t i = 0; i < ownerCount; ++i)
            if (&slots[i] == fresh && fresh->owner && fresh->reserved)
            {
                old.slot->reusable = false; old.slot->active = false;
                fresh->active = fresh->reusable = true; fresh->reserved = false;
                old.lease->slot = i;
                return { Status::Created, &old.lease->handle, fresh, old.lease };
            }
        return { Status::Mismatch };
    }
    void Shutdown()
    {
        if (closed) return;
        closed = true;
        for (std::size_t i = 0; i < handleCount; ++i) leases[i].live = false;
        for (std::size_t i = 0; i < ownerCount; ++i)
            slots[i].active = slots[i].reusable = slots[i].reserved = false;
    }

  private:
    bool IdUsed(std::uint32_t id) const
    {
        if (!id) return true;
        for (std::size_t i = 0; i < handleCount; ++i) if (leases[i].id == id) return true;
        for (std::size_t i = 0; i < ownerCount; ++i) if (slots[i].backendId == id) return true;
        return false;
    }
    Result Issue(std::size_t index, std::uint32_t id, Status status)
    {
        if (IdUsed(id)) return { Status::Mismatch };
        auto& lease = leases[handleCount++];
        lease.handle.Id = id; lease.id = id; lease.slot = index; lease.live = true;
        auto& slot = slots[index]; slot.active = true;
        return { status, &lease.handle, &slot, &lease };
    }
    std::recursive_mutex mutex;
    std::array<Slot, Capacity> slots {};
    std::array<Lease, MaxHandles> leases {};
    std::size_t ownerCount = 0, handleCount = 0;
    bool closed = false, busy = false;
};
}
