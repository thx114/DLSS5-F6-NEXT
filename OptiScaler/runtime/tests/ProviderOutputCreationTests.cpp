// SPDX-License-Identifier: GPL-3.0-or-later
#include <hooks/ProviderOutputCreation.h>
#include <hooks/RoutedHook.h>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>

// CPU factory fixture: the real scope flag and routed hooks are exercised;
// provider/device authentication is supplied, not claimed as SDK/GPU proof.
namespace framegen::final_output
{
CreationScope::CreationScope(const std::shared_ptr<Provider>&) noexcept { ++depth; }
CreationScope::~CreationScope() { --depth; }
}
namespace
{
unsigned checks = 0, wrappers = 0, policies = 0, alive = 0;
bool authenticate = true, scoped = true, negative = false;
void Check(bool ok, const char* why)
{
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
struct __declspec(uuid("2B83C8A0-7953-4DE7-B0C2-4DCA1F519BEF")) Object : IUnknown
{
    ULONG refs = 1;
    Object() { ++alive; }
    virtual ~Object() { --alive; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
    {
        *output = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(Object)) return E_NOINTERFACE;
        *output = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const auto left = --refs; if (!left) delete this; return left; }
};
struct __declspec(uuid("B0AC077C-BA6A-45F2-AC68-D997A8B601E7")) Wrapper : Object
{
    Object* raw;
    UINT flags;
    Wrapper(Object* source, IUnknown*, HWND, UINT requested, bool, bool, std::optional<UINT>)
        : raw(source), flags(requested) { ++wrappers; }
    ~Wrapper() override { raw->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
    {
        if (iid == __uuidof(Wrapper)) { *output = this; AddRef(); return S_OK; }
        return Object::QueryInterface(iid, output);
    }
};
struct Proxy : Object
{
    Object* inner;
    GUID contract;
    Proxy(Object* source, GUID id) : inner(source), contract(id) {}
    ~Proxy() override { inner->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
    {
        if (iid == contract) { *output = inner; inner->AddRef(); return S_OK; }
        return Object::QueryInterface(iid, output);
    }
};
void Observe(HRESULT result, Object** output)
{
    if (negative) return;
    ProviderOutputCreation::Observe<Wrapper>(result, output, nullptr, nullptr, 0x802u, false, false,
        [](Object*, IUnknown*) { return authenticate; });
}
struct Domain {
    static void Returned(HRESULT result, unsigned, Object** output) { Observe(result, output); }
    static void ReturnedDirect(HRESULT result, unsigned mode, Object** output) { Returned(result, mode, output); }
};
HRESULT Policy(unsigned, Object**);
HookOwnership::RoutedHook<&Policy, SIZE_MAX, Domain> route;
__declspec(noinline) HRESULT Native(unsigned mode, Object** output)
{
    *output = mode == 2 ? nullptr : new Object;
    volatile HRESULT result = mode == 1 ? S_FALSE : mode == 3 ? E_FAIL : S_OK;
    return result;
}
__declspec(noinline) HRESULT SdkProxy(unsigned mode, Object** output)
{
    const auto result = Native(mode, output);
    if (SUCCEEDED(result) && *output) *output = new Proxy(*output, NativeObjectContracts::Streamline);
    return result;
}
__declspec(noinline) HRESULT ReShadeProxy(unsigned mode, Object** output)
{
    const auto result = SdkProxy(mode, output);
    if (SUCCEEDED(result) && *output) *output = new Proxy(*output, NativeObjectContracts::ReShade);
    return result;
}
HRESULT Policy(unsigned mode, Object** output)
{
    ++policies;
    std::optional<framegen::final_output::CreationScope> creation;
    if (scoped) creation.emplace(nullptr);
    return route(mode, output);
}
namespace DirectFactory
{
struct Factory;
using Method = HRESULT(*)(Factory*, Object**);
struct Factory { Method* table; };
struct Domain {
    static void ReturnedDirect(HRESULT result, Factory*, Object** output) { Observe(result, output); }
};
HRESULT Policy(Factory*, Object**);
HookOwnership::RoutedHook<&Policy, 0, Domain> route;
__declspec(noinline) HRESULT Native(Factory*, Object** output) { *output = new Object; return S_FALSE; }
__declspec(noinline) HRESULT Other(Factory*, Object** output) { *output = new Object; return S_OK; }
HRESULT Policy(Factory* factory, Object** output) { return route(factory, output); }
void Test()
{
    HookOwnership::Own::Batch batch;
    Check(route.Add(batch, reinterpret_cast<PVOID>(&Native)) == NO_ERROR, "register direct factory slot");
    Check(batch.Commit() == NO_ERROR, "commit direct factory hook");
    Method nativeTable[] { Native }, otherTable[] { Other };
    Factory native { nativeTable }, other { otherTable };
    authenticate = true;
    framegen::final_output::CreationScope creation(nullptr);
    for (Factory* factory : { &native, &other })
    {
        Object* output = nullptr;
        const auto count = wrappers;
        const auto hr = route(factory, &output);
        Check(hr == (factory == &native ? S_FALSE : S_OK), "direct original preserves exact factory result");
        Check(wrappers == count + 1 && dynamic_cast<Wrapper*>(output), "capture a direct factory with no hooked lower layer");
        output->Release();
        Check(alive == 0, "direct returned interface has one transferred owner");
    }
    std::vector<HookOwnership::Own::Slot*> slots;
    route.AppendSlots(slots);
    Check(HookOwnership::Own::DetachAll(std::span<HookOwnership::Own::Slot* const>(slots)) == NO_ERROR, "detach direct factory slot");
}
}
}
int main(int argc, char** argv)
{
    negative = argc == 2 && std::string_view(argv[1]) == "--negative";
    HookOwnership::Own::Batch batch;
    for (const auto entry : { &Native, &SdkProxy, &ReShadeProxy })
        Check(route.Add(batch, reinterpret_cast<PVOID>(entry)) == NO_ERROR, "register actual nested factory implementations");
    Check(batch.Commit() == NO_ERROR, "commit factory hooks");
    for (const bool allow : { false, true }) for (const bool scope : { false, true })
    {
        authenticate = allow; scoped = scope;
        for (const unsigned mode : { 0u, 1u, 2u, 3u })
        {
            const auto before = wrappers, outer = policies;
            Object* output = nullptr;
            const auto hr = ReShadeProxy(mode, &output);
            Check(hr == (mode == 1 ? S_FALSE : mode == 3 ? E_FAIL : S_OK), "preserve exact creation result");
            Check(policies == outer + 1, "nested factories cannot repeat source policy");
            const bool captured = allow && scope && mode < 2;
            Check(wrappers == before + static_cast<unsigned>(captured), "nested SDK raw output must be captured exactly once");
            if (output)
            {
                if (SUCCEEDED(hr))
                {
                    auto* shade = dynamic_cast<Proxy*>(output);
                    auto* sdk = shade ? dynamic_cast<Proxy*>(shade->inner) : nullptr;
                    Check(shade && sdk, "preserve both outer proxy objects");
                    auto* physical = dynamic_cast<Wrapper*>(sdk->inner);
                    Check(bool(physical) == captured, "SDK retains physical wrapper while its input remains a proxy");
                    if (physical)
                    {
                        Check(physical->flags == 0x802u && physical->raw->refs == 1, "transfer returned reference and creation flags");
                        framegen::final_output::CreationScope again(nullptr);
                        const auto count = wrappers;
                        Observe(S_OK, &sdk->inner);
                        Observe(S_OK, &output);
                        Check(wrappers == count && physical->refs == 1, "repeat observation never wraps an owner or a proxy twice");
                    }
                }
                output->Release();
            }
            Check(alive == 0, "all returned and queried COM references balance");
            Check(!framegen::final_output::CreationScope::Active(), "restore source role after factory returns");
        }
    }
    std::vector<HookOwnership::Own::Slot*> slots;
    route.AppendSlots(slots);
    Check(HookOwnership::Own::DetachAll(std::span<HookOwnership::Own::Slot* const>(slots)) == NO_ERROR, "detach test hooks");
    DirectFactory::Test();
    std::printf("PASS %u nested provider output capture, proxy preservation, statuses and owned-reference checks\n", checks);
}
