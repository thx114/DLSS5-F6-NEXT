#include <hooks/MethodHooks.h>
#include <hooks/RoutedHook.h>
#include <cassert>
#include <cstdio>
#include <utility>

using Function = int(*)(int);
HookOwnership::MethodHooks<Function> hooks;
thread_local unsigned depth = 0;
unsigned observations = 0;
__declspec(noinline) int First(int value) { volatile int input = value; return input * 3 + 7; }
__declspec(noinline) int Proxy(int value) { volatile int input = value; return First(input) + 13; }
template<size_t Index> __declspec(noinline) int Thunk(int value) {
    const bool outer = depth++ == 0;
    auto call = hooks.Acquire(Index);
    const auto result = call.Get()(value);
    --depth;
    if (outer) ++observations;
    return result;
}
namespace Routed {
struct Object;
using Call = HRESULT(*)(Object*, int);
struct Object { Call* table; int kind; Object* inner = nullptr; };
unsigned nestedReturns=0;
struct Domain { static void Returned(HRESULT,Object*,int) { ++nestedReturns; } };
unsigned policies = 0;
HRESULT Policy(Object*, int);
HookOwnership::RoutedHook<&Policy, 0, Domain> route;
__declspec(noinline) HRESULT Native(Object* self, int value) {
    assert(self->kind == 1); volatile int input = value; return input * 5 + 11;
}
__declspec(noinline) HRESULT Proxy(Object* self, int value) {
    assert(self->kind == 2 && self->inner); volatile int input = value;
    return self->inner->table[0](self->inner, input) + 100;
}
__declspec(noinline) HRESULT Other(Object* self, int value) {
    assert(self->kind == 3); volatile int input = value; return input * 7 + 23;
}
HRESULT Policy(Object* self, int value) { ++policies; return route(self, value); }
void Test() {
    Call nativeTable[]{Native}, proxyTable[]{Proxy}, otherTable[]{Other};
    Object native{nativeTable,1}, proxy{proxyTable,2,&native}, other{otherTable,3};
    for (unsigned round = 0; round < 2; ++round) {
        { HookOwnership::Own::Batch batch;
          assert(route.Add(batch, reinterpret_cast<PVOID>(Native)) == NO_ERROR);
          assert(route.Add(batch, reinterpret_cast<PVOID>(Proxy)) == NO_ERROR);
          assert(batch.Commit() == NO_ERROR); }
        policies = nestedReturns = 0;
        assert(proxy.table[0](&proxy, 4) == 131 && policies == 1 && nestedReturns == 1);
        assert(native.table[0](&native, 4) == 31 && policies == 2);
        // Public helper chooses this object's original, even without a thunk
        // frame. An unrelated factory must not use the first target's code.
        assert(route(&other, 4) == 51 && policies == 2);
        assert(route(&native, 4) == 31 && policies == 2);
        assert(route(&proxy, 4) == 131 && policies == 2);
        std::vector<HookOwnership::Own::Slot*> slots; route.AppendSlots(slots);
        assert(slots.size() == 2);
        assert(HookOwnership::Own::DetachAll(std::span<HookOwnership::Own::Slot* const>(slots)) == NO_ERROR);
        assert(proxy.table[0](&proxy,4) == 131 && policies == 2);
    }
}
}
int main() {
    const auto thunks = []<size_t... I>(std::index_sequence<I...>) {
        return std::array<Function, sizeof...(I)>{&Thunk<I>...};
    }(std::make_index_sequence<decltype(hooks)::capacity>{});
    auto first = reinterpret_cast<PVOID>(&First), proxy = reinterpret_cast<PVOID>(&Proxy);
    for (unsigned iteration = 0; iteration < 2; ++iteration) {
        { HookOwnership::Own::Batch batch;
          assert(hooks.Add(batch, first, thunks) == NO_ERROR);
          assert(hooks.Add(batch, proxy, thunks) == NO_ERROR);
          assert(batch.Commit() == NO_ERROR); }
        assert(hooks.Covers(first) && hooks.Covers(proxy));
        observations = 0;
        assert(First(4) == 19 && observations == 1);
        assert(Proxy(4) == 32 && observations == 2);
        { HookOwnership::Own::Batch batch;
          assert(hooks.Add(batch, first, thunks) == NO_ERROR);
          assert(hooks.Add(batch, proxy, thunks) == NO_ERROR);
          assert(batch.Commit() == NO_ERROR); }
        assert(Proxy(4) == 32 && observations == 3);
        std::vector<HookOwnership::Own::Slot*> slots;
        hooks.AppendSlots(slots);
        assert(slots.size() == 2);
        {
            auto activeCall=hooks.Acquire(0);
            assert(activeCall.Get()(4)==19);
            assert(HookOwnership::Own::DetachAll(std::span<HookOwnership::Own::Slot* const>(slots)) == ERROR_BUSY);
            assert(hooks.Covers(first) && hooks.Covers(proxy));
        }
        assert(HookOwnership::Own::DetachAll(std::span<HookOwnership::Own::Slot* const>(slots)) == NO_ERROR);
        assert(!hooks.Covers(first) && !hooks.Covers(proxy));
        observations = 0;
        assert(Proxy(4) == 32 && observations == 0);
    }
    // The first staged method must roll back when the second cannot be installed.
    { HookOwnership::Own::Batch batch;
      assert(hooks.Add(batch, first, thunks) == NO_ERROR);
      assert(hooks.Add(batch, nullptr, thunks) == ERROR_INVALID_PARAMETER);
      assert(batch.Commit() == ERROR_INVALID_PARAMETER); }
    assert(!hooks.Covers(first) && First(4) == 19);
    Routed::Test();
    std::puts("Method Hooks: distinct native/proxy targets, exact originals, nested observation, idempotence, rollback and reinstall passed");
}
