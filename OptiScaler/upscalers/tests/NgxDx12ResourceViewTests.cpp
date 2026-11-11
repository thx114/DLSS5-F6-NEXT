// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../NgxDx12ResourceView.h"
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>

namespace
{
// Models a native DX11 table that silently ignores typed DX12 resource writes.
// Pointer values are inert identities; this fixture invokes no COM or GPU API.
struct Dx11Parameters final : NVSDK_NGX_Parameter
{
    using Value = std::variant<unsigned long long, float, double, unsigned int, int, void*>;
    std::unordered_map<std::string, Value> values;
    unsigned pointerWrites = 0;
    template<class T> NVSDK_NGX_Result Read(const char* key, T* value) const
    {
        if (!value) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        const auto it = values.find(key);
        if (it != values.end())
            if (const auto* stored = std::get_if<T>(&it->second))
            {
                *value = *stored;
                return NVSDK_NGX_Result_Success;
            }
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
#define PARAM_VALUE(Type) \
    void Set(const char* key, Type value) override { values.insert_or_assign(key, value); } \
    NVSDK_NGX_Result Get(const char* key, Type* value) const override { return Read(key, value); }
    PARAM_VALUE(unsigned long long)
    PARAM_VALUE(float)
    PARAM_VALUE(double)
    PARAM_VALUE(unsigned int)
    PARAM_VALUE(int)
#undef PARAM_VALUE
    void Set(const char* key, void* value) override { ++pointerWrites; values.insert_or_assign(key, value); }
    void Set(const char* key, ID3D11Resource* value) override { Set(key, static_cast<void*>(value)); }
    void Set(const char*, ID3D12Resource*) override {} // Native DX11 table's incompatible setter.
    NVSDK_NGX_Result Get(const char* key, void** value) const override { return Read(key, value); }
    NVSDK_NGX_Result Get(const char* key, ID3D11Resource** value) const override
    {
        void* p = nullptr;
        const auto result = Read(key, &p);
        if (result == NVSDK_NGX_Result_Success) *value = static_cast<ID3D11Resource*>(p);
        return result;
    }
    NVSDK_NGX_Result Get(const char*, ID3D12Resource**) const override { return NVSDK_NGX_Result_FAIL_InvalidParameter; }
    void Reset() override { values.clear(); }
};
constexpr std::array<const char*, 6> keys {
    NVSDK_NGX_Parameter_Color, NVSDK_NGX_Parameter_Depth, NVSDK_NGX_Parameter_MotionVectors,
    NVSDK_NGX_Parameter_Output, NVSDK_NGX_Parameter_ExposureTexture,
    NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask
};
}

int main()
{
    Dx11Parameters source;
    auto* input11 = reinterpret_cast<ID3D11Resource*>(uintptr_t {0x1100});
    auto* input12 = reinterpret_cast<ID3D12Resource*>(uintptr_t {0x1200});
    for (auto* key : keys) source.Set(key, input11);
    const auto originalWrites = source.pointerWrites;
    source.Set(keys[0], input12);
    void* p = nullptr;
    assert(source.Get(keys[0], &p) == NVSDK_NGX_Result_Success && p == input11);

    {
        OptiBridge::NgxDx12ResourceView view(source);
        for (auto* key : keys)
        {
            ID3D11Resource* before = nullptr;
            assert(view.Get(key, &before) == NVSDK_NGX_Result_Success && before == input11);
            view.Set(key, input12);
            ID3D12Resource* typed = nullptr;
            assert(view.Get(key, &typed) == NVSDK_NGX_Result_Success && typed == input12);
            assert(view.Get(key, &p) == NVSDK_NGX_Result_Success && p == input12);
            assert(view.Get(key, &before) == NVSDK_NGX_Result_FAIL_InvalidParameter && !before);
            assert(source.Get(key, &p) == NVSDK_NGX_Result_Success && p == input11);
        }
        // Provider scratch-output rebinding and optional-null masking remain local.
        view.Set(keys[3], static_cast<void*>(nullptr));
        ID3D12Resource* output = input12;
        assert(view.Get(keys[3], &output) == NVSDK_NGX_Result_Success && !output);
        view.Set(keys[4], static_cast<void*>(nullptr));
        assert(view.Get(keys[4], &p) == NVSDK_NGX_Result_Success && !p);
        view.Set(keys[3], input12);
        assert(view.Get(keys[3], &output) == NVSDK_NGX_Result_Success && output == input12);

        // Unknown numeric metadata and callbacks must retain caller semantics.
#define VALUE_CASE(Type, Key, Value) \
        view.Set(Key, static_cast<Type>(Value)); \
        { Type actual{}; assert(view.Get(Key, &actual) == NVSDK_NGX_Result_Success && actual == static_cast<Type>(Value)); \
          assert(source.Get(Key, &actual) == NVSDK_NGX_Result_Success && actual == static_cast<Type>(Value)); }
        VALUE_CASE(unsigned long long, "unknown.ull", 123456789);
        VALUE_CASE(float, "unknown.float", 0.25);
        VALUE_CASE(double, "unknown.double", 0.5);
        VALUE_CASE(unsigned int, "unknown.uint", 123);
        VALUE_CASE(int, "unknown.int", -7);
#undef VALUE_CASE
        auto* callback = reinterpret_cast<void*>(uintptr_t {0xca11});
        view.Set("unknown.callback", callback);
        assert(view.Get("unknown.callback", &p) == NVSDK_NGX_Result_Success && p == callback);
        int missing = 73;
        assert(view.Get("unknown.missing", &missing) == NVSDK_NGX_Result_FAIL_InvalidParameter && missing == 73);
        assert(source.pointerWrites == originalWrites + 1); // Only the unknown callback forwarded.
    }
    for (auto* key : keys)
        assert(source.Get(key, &p) == NVSDK_NGX_Result_Success && p == input11);

    // Failure/unwinding cannot leave translated pointers in the caller's table.
    try
    {
        OptiBridge::NgxDx12ResourceView failed(source);
        for (auto* key : keys) failed.Set(key, input12);
        throw 1;
    }
    catch (int) {}
    for (auto* key : keys)
        assert(source.Get(key, &p) == NVSDK_NGX_Result_Success && p == input11);
    OptiBridge::NgxDx12ResourceView next(source);
    ID3D11Resource* next11 = nullptr;
    assert(next.Get(keys[0], &next11) == NVSDK_NGX_Result_Success && next11 == input11);
    next.Set(keys[0], input12);
    next.Reset();
    assert(next.Get(keys[0], &p) == NVSDK_NGX_Result_FAIL_InvalidParameter);
    std::puts("PASS NGX DX12 resource view: six translated resources, typed/void reads, null masking, caller preservation, metadata, unwind and reset; CPU only");
}
