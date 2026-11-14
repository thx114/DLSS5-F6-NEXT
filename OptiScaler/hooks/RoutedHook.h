#pragma once
#include "MethodHooks.h"
#include <mutex>
#include <tuple>

namespace HookOwnership {
template<class Domain> inline thread_local unsigned RoutedDepth = 0;

// One trampoline per actual implementation, including proxy implementations.
// An outer proxy owns F6's policy; nested native calls keep their exact original
// and do not repeat wrapping or feature creation. No proxy object is unwrapped.
template<auto Callback, size_t MethodIndex, class Domain, class Function = decltype(Callback)>
class RoutedHook;

template<auto Callback, size_t MethodIndex, class Domain, class Result, class... Args>
class RoutedHook<Callback, MethodIndex, Domain, Result(*)(Args...)> final {
    using Function = Result(*)(Args...);
    inline static MethodHooks<Function> hooks;
    struct Frame {
        Frame* previous;
        Function original;
        const void* object;
    };
    inline static thread_local Frame* active = nullptr;
    static const void* Object([[maybe_unused]] Args... args) noexcept {
        if constexpr (MethodIndex == SIZE_MAX) return nullptr;
        else return std::get<0>(std::tuple<Args...>(args...));
    }
    template<size_t Index> static Result Thunk(Args... args) {
        auto call = hooks.Acquire(Index);
        const auto original = call.Get();
        if (!original) return static_cast<Result>(E_UNEXPECTED);
        Frame frame{active, original, Object(args...)};
        active = &frame;
        struct Exit {
            Frame* previous;
            ~Exit() { active = previous; --RoutedDepth<Domain>; }
        } exit{frame.previous};
        if (RoutedDepth<Domain>++ != 0) {
            const auto result=original(args...);
            if constexpr (requires { Domain::Returned(result,args...); }) Domain::Returned(result,args...);
            return result;
        }
        return Callback(args...);
    }
public:
    LONG Add(Own::Batch& batch, PVOID entry) const noexcept {
        const auto thunks = []<size_t... I>(std::index_sequence<I...>) {
            return std::array<Function, sizeof...(I)>{&Thunk<I>...};
        }(std::make_index_sequence<decltype(hooks)::capacity>{});
        return hooks.Add(batch, entry, thunks);
    }
    void AppendSlots(std::vector<Own::Slot*>& slots) const {
        hooks.AppendSlots(slots);
    }
    Result operator()(Args... args) const {
        struct Suppress {
            Suppress() { ++RoutedDepth<Domain>; }
            ~Suppress() { --RoutedDepth<Domain>; }
        } suppress;
        const auto object = Object(args...);
        for (auto* frame = active; frame; frame = frame->previous)
            if (frame->object == object) return frame->original(args...);
        if constexpr (MethodIndex != SIZE_MAX) {
            // Direct internal calls may use a different factory. Resolve that
            // object's actual method; never reuse the first factory's original.
            if (!object) return static_cast<Result>(E_INVALIDARG);
            const auto entry = (*reinterpret_cast<PVOID* const*>(object))[MethodIndex];
            std::unique_lock lock(Own::PublicationMutex);
            const auto index = hooks.Find(entry);
            if (index == decltype(hooks)::capacity) {
                lock.unlock();
                return reinterpret_cast<Function>(entry)(args...);
            }
            auto call = hooks.Acquire(index);
            lock.unlock();
            const auto original = call.Get();
            return original ? original(args...) : static_cast<Result>(E_UNEXPECTED);
        } else return static_cast<Result>(E_UNEXPECTED);
    }
};
}
