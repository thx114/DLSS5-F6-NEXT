// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace FramePipeline {
// Keep the existing setting/INI API, but publish completed scalar mutations
// for observation threads. This does not make legacy reference reads or
// concurrent writers to Base safe; observers use only the atomic publication.
template<class Base,class T> class ObservedSetting final : public Base {
    static_assert(std::is_same_v<T,bool> || (std::is_enum_v<T> && sizeof(T)<=4));
    T default_;
    std::atomic<uint64_t> published_{0};
    static uint64_t Pack(std::optional<T> value,T fallback)noexcept {
        return (uint64_t(static_cast<uint32_t>(value.value_or(fallback)))<<1)|(value?1u:0u);
    }
    void Publish()noexcept {
        published_.store(Pack(Base::has_value()?std::optional<T>(Base::value()):std::nullopt,default_),std::memory_order_release);
    }
public:
    explicit ObservedSetting(T value):Base(value),default_(value),published_(Pack(std::nullopt,value)){}
    ObservedSetting(const ObservedSetting& other):Base(static_cast<const Base&>(other)),
        default_(other.default_),published_(other.published_.load(std::memory_order_acquire)){}
    ObservedSetting(ObservedSetting&& other):Base(std::move(static_cast<Base&>(other))),
        default_(other.default_),published_(other.published_.load(std::memory_order_acquire)){}
    ObservedSetting& operator=(const ObservedSetting& other){
        Base::operator=(static_cast<const Base&>(other));default_=other.default_;Publish();return *this;
    }
    ObservedSetting& operator=(ObservedSetting&& other){
        Base::operator=(std::move(static_cast<Base&>(other)));default_=other.default_;Publish();return *this;
    }
    ObservedSetting& operator=(T value){Base::operator=(value);Publish();return *this;}
    ObservedSetting& operator=(const std::optional<T>& value){Base::operator=(value);Publish();return *this;}
    void set_from_config(const std::optional<T>& value){Base::set_from_config(value);Publish();}
    void set_volatile_value(const T& value){Base::set_volatile_value(value);Publish();}
    void reset()noexcept {Base::reset();Publish();}
    std::optional<T> ReadObserved()const noexcept {
        const auto bits=published_.load(std::memory_order_acquire);
        return bits&1u?std::optional<T>(static_cast<T>(uint32_t(bits>>1))):std::nullopt;
    }
    T ObservedValue()const noexcept {return static_cast<T>(uint32_t(published_.load(std::memory_order_acquire)>>1));}
};
}
