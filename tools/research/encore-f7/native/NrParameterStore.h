#pragma once

#include <cstddef>
#include <nvsdk_ngx_params.h>

#include <array>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace encore_research
{

/**
 * Original, bounded CPU parameter storage using the public NGX class interface.
 * Each case-sensitive key has independent numeric and opaque-pointer slots.
 * Numeric writes replace only the numeric slot; all three pointer overloads
 * share one pointer slot, and a null pointer is a present value.
 *
 * Names must point to readable NUL-terminated storage. At most MaxName + 1
 * bytes are examined; accepted names are copied, never borrowed. Non-null Get
 * outputs must point to writable objects of the requested type. This class
 * cannot validate arbitrary host addresses and requires external serialization.
 *
 * Set failures latch the first Error and freeze subsequent writes. While
 * unhealthy, every Get returns Fail (even for invalid arguments), without
 * touching its output. Getter failures never latch an Error. Reset clears both
 * slots, all keys and the error; it is CPU bookkeeping, never GPU retirement.
 *
 * Pointers are never dereferenced, AddRef'd, released or granted ownership.
 * The caller retains actual resources and their same-owner queue/fence lifetime
 * obligations. Object addresses must remain stable: copying/moving is disabled.
 * NVSDK_NGX_Parameter has no virtual destructor; never delete via a base pointer.
 * This implementation neither calls NGX nor claims an external payload ABI.
 */
template <std::size_t Capacity = 64, std::size_t MaxName = 127>
class NrParameterStore final : public NVSDK_NGX_Parameter
{
    static_assert(MaxName < (std::numeric_limits<std::size_t>::max)());
    static_assert(std::numeric_limits<int>::digits == 31);
    static_assert(std::numeric_limits<unsigned int>::digits == 32);
    static_assert(std::numeric_limits<unsigned long long>::digits == 64);
    static_assert(std::numeric_limits<float>::radix == 2);
    static_assert(std::numeric_limits<double>::radix == 2);

public:
    enum class Error
    {
        None,
        InvalidName,
        NameTooLong,
        NonFiniteValue,
        CapacityExceeded
    };

    NrParameterStore() noexcept = default;
    ~NrParameterStore() noexcept = default;
    NrParameterStore(const NrParameterStore&) = delete;
    NrParameterStore& operator=(const NrParameterStore&) = delete;
    NrParameterStore(NrParameterStore&&) = delete;
    NrParameterStore& operator=(NrParameterStore&&) = delete;

    [[nodiscard]] bool Healthy() const noexcept { return error_ == Error::None; }
    [[nodiscard]] Error FirstError() const noexcept { return error_; }
    [[nodiscard]] std::size_t Size() const noexcept { return size_; }

    void Set(const char* name, unsigned long long value) noexcept override
    {
        SetNumeric(name, value);
    }
    void Set(const char* name, float value) noexcept override { SetNumeric(name, value); }
    void Set(const char* name, double value) noexcept override { SetNumeric(name, value); }
    void Set(const char* name, unsigned int value) noexcept override
    {
        SetNumeric(name, static_cast<unsigned long long>(value));
    }
    void Set(const char* name, int value) noexcept override { SetNumeric(name, value); }
    void Set(const char* name, ID3D11Resource* value) noexcept override
    {
        SetPointer(name, static_cast<void*>(value));
    }
    void Set(const char* name, ID3D12Resource* value) noexcept override
    {
        SetPointer(name, static_cast<void*>(value));
    }
    void Set(const char* name, void* value) noexcept override { SetPointer(name, value); }

    NVSDK_NGX_Result Get(const char* name, unsigned long long* out) const noexcept override
    {
        return GetNumeric(name, out);
    }
    NVSDK_NGX_Result Get(const char* name, float* out) const noexcept override
    {
        return GetNumeric(name, out);
    }
    NVSDK_NGX_Result Get(const char* name, double* out) const noexcept override
    {
        return GetNumeric(name, out);
    }
    NVSDK_NGX_Result Get(const char* name, unsigned int* out) const noexcept override
    {
        return GetNumeric(name, out);
    }
    NVSDK_NGX_Result Get(const char* name, int* out) const noexcept override
    {
        return GetNumeric(name, out);
    }
    NVSDK_NGX_Result Get(const char* name, ID3D11Resource** out) const noexcept override
    {
        return GetPointer(name, out);
    }
    NVSDK_NGX_Result Get(const char* name, ID3D12Resource** out) const noexcept override
    {
        return GetPointer(name, out);
    }
    NVSDK_NGX_Result Get(const char* name, void** out) const noexcept override
    {
        return GetPointer(name, out);
    }

    void Reset() noexcept override
    {
        // Clear inactive entries too; no opaque pointer has ownership semantics.
        for (auto& entry : entries_)
            entry = Entry{};
        size_ = 0;
        error_ = Error::None;
    }

private:
    enum class NumericKind { Unset, Int, Ull, Float, Double };

    union NumericValue
    {
        int integer;
        unsigned long long ull = 0;
        float single;
        double real;
    };

    struct Entry
    {
        std::array<char, MaxName + 1> name{};
        std::size_t nameLength = 0;
        NumericValue numeric{};
        NumericKind kind = NumericKind::Unset;
        void* pointer = nullptr;
        bool hasPointer = false;
    };

    [[nodiscard]] static Error ReadName(const char* name, std::size_t& length) noexcept
    {
        if (name == nullptr)
            return Error::InvalidName;
        for (std::size_t i = 0; i <= MaxName; ++i)
        {
            if (name[i] == '\0')
            {
                if (i == 0)
                    return Error::InvalidName;
                length = i;
                return Error::None;
            }
        }
        return Error::NameTooLong;
    }

    [[nodiscard]] const Entry* Find(const char* name, std::size_t length) const noexcept
    {
        for (std::size_t i = 0; i < size_; ++i)
        {
            const auto& entry = entries_[i];
            if (entry.nameLength != length)
                continue;
            std::size_t j = 0;
            while (j < length && entry.name[j] == name[j])
                ++j;
            if (j == length)
                return &entry;
        }
        return nullptr;
    }

    void Latch(Error error) noexcept
    {
        if (Healthy())
            error_ = error;
    }

    [[nodiscard]] Entry* FindOrInsert(const char* name, std::size_t length) noexcept
    {
        // Search before the capacity check: full stores still allow key updates
        // and filling the other slot of an existing key.
        if (const Entry* entry = Find(name, length))
            return &entries_[static_cast<std::size_t>(entry - entries_.data())];
        if (size_ == Capacity)
        {
            Latch(Error::CapacityExceeded);
            return nullptr;
        }
        auto& entry = entries_[size_];
        for (std::size_t i = 0; i < length; ++i)
            entry.name[i] = name[i];
        entry.name[length] = '\0';
        entry.nameLength = length;
        ++size_;
        return &entry;
    }

    template <typename T>
    void SetNumeric(const char* name, T value) noexcept
    {
        if (!Healthy())
            return;
        std::size_t length = 0;
        const Error nameError = ReadName(name, length);
        if (nameError != Error::None)
        {
            Latch(nameError);
            return;
        }
        if constexpr (std::is_floating_point_v<T>)
        {
            if (!std::isfinite(value))
            {
                Latch(Error::NonFiniteValue);
                return;
            }
        }
        Entry* entry = FindOrInsert(name, length);
        if (entry == nullptr)
            return;
        if constexpr (std::is_same_v<T, int>)
        {
            entry->numeric.integer = value;
            entry->kind = NumericKind::Int;
        }
        else if constexpr (std::is_same_v<T, unsigned long long>)
        {
            entry->numeric.ull = value;
            entry->kind = NumericKind::Ull;
        }
        else if constexpr (std::is_same_v<T, float>)
        {
            entry->numeric.single = value;
            entry->kind = NumericKind::Float;
        }
        else
        {
            static_assert(std::is_same_v<T, double>);
            entry->numeric.real = value;
            entry->kind = NumericKind::Double;
        }
    }

    void SetPointer(const char* name, void* value) noexcept
    {
        if (!Healthy())
            return;
        std::size_t length = 0;
        const Error nameError = ReadName(name, length);
        if (nameError != Error::None)
        {
            Latch(nameError);
            return;
        }
        if (Entry* entry = FindOrInsert(name, length))
        {
            entry->pointer = value;
            entry->hasPointer = true;
        }
    }

    /**
     * Integer-to-integer checks use exact integer ranges, never a floating
     * intermediate. Floating-to-integer first truncates toward zero, then
     * checks [0, 2^digits) or [-2^digits, 2^digits) before casting. These bounds
     * are exact even when long double has only double precision (MSVC).
     * Thus -0.75 -> unsigned zero succeeds, while integer -1 is rejected.
     * Floating outputs permit ordinary rounding and underflow to signed zero,
     * but reject values outside the target's finite range before conversion.
     */
    template <typename To, typename From>
    [[nodiscard]] static bool Convert(From value, To& out) noexcept
    {
        if constexpr (std::is_integral_v<To> && std::is_integral_v<From>)
        {
            if (!std::in_range<To>(value))
                return false;
            out = static_cast<To>(value);
            return true;
        }
        else if constexpr (std::is_integral_v<To>)
        {
            if (!std::isfinite(value))
                return false;
            const long double truncated = std::trunc(static_cast<long double>(value));
            const long double upper = std::ldexp(1.0L, std::numeric_limits<To>::digits);
            const long double lower = std::is_signed_v<To> ? -upper : 0.0L;
            if (!(truncated >= lower && truncated < upper))
                return false;
            out = static_cast<To>(truncated);
            return true;
        }
        else
        {
            const long double wide = static_cast<long double>(value);
            const long double maximum = static_cast<long double>((std::numeric_limits<To>::max)());
            if (!std::isfinite(wide) || wide < -maximum || wide > maximum)
                return false;
            const To converted = static_cast<To>(value);
            if (!std::isfinite(converted))
                return false;
            out = converted;
            return true;
        }
    }

    [[nodiscard]] NVSDK_NGX_Result Lookup(
        const char* name, const void* out, const Entry*& entry) const noexcept
    {
        if (!Healthy())
            return NVSDK_NGX_Result_Fail;
        if (out == nullptr)
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        std::size_t length = 0;
        if (ReadName(name, length) != Error::None)
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        entry = Find(name, length);
        return entry == nullptr ? NVSDK_NGX_Result_FAIL_MissingInput : NVSDK_NGX_Result_Success;
    }

    template <typename T>
    [[nodiscard]] NVSDK_NGX_Result GetNumeric(const char* name, T* out) const noexcept
    {
        const Entry* entry = nullptr;
        const auto result = Lookup(name, out, entry);
        if (result != NVSDK_NGX_Result_Success)
            return result;
        T converted{};
        bool success = false;
        switch (entry->kind)
        {
        case NumericKind::Int: success = Convert(entry->numeric.integer, converted); break;
        case NumericKind::Ull: success = Convert(entry->numeric.ull, converted); break;
        case NumericKind::Float: success = Convert(entry->numeric.single, converted); break;
        case NumericKind::Double: success = Convert(entry->numeric.real, converted); break;
        case NumericKind::Unset: break;
        }
        if (!success)
            return NVSDK_NGX_Result_FAIL_MissingInput;
        *out = converted;
        return NVSDK_NGX_Result_Success;
    }

    template <typename T>
    [[nodiscard]] NVSDK_NGX_Result GetPointer(const char* name, T** out) const noexcept
    {
        const Entry* entry = nullptr;
        const auto result = Lookup(name, out, entry);
        if (result != NVSDK_NGX_Result_Success)
            return result;
        if (!entry->hasPointer)
            return NVSDK_NGX_Result_FAIL_MissingInput;
        *out = static_cast<T*>(entry->pointer);
        return NVSDK_NGX_Result_Success;
    }

    std::array<Entry, Capacity> entries_{};
    std::size_t size_ = 0;
    Error error_ = Error::None;
};

} // namespace encore_research
