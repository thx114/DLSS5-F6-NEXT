#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <type_traits>
#include <utility>

#include "NrParameterStore.h"
#include "NrParameterStoreConsumer.h"
#include "NativeBuildInputs.h"

namespace
{
using Store = encore_research::NrParameterStore<>;
using U64 = unsigned long long;
constexpr auto Success = NVSDK_NGX_Result_Success;
constexpr auto Missing = NVSDK_NGX_Result_FAIL_MissingInput;
constexpr auto Invalid = NVSDK_NGX_Result_FAIL_InvalidParameter;
constexpr auto Fail = NVSDK_NGX_Result_Fail;
constexpr U64 U64Max = 18446744073709551615ULL;
constexpr U64 BeyondDoubleInteger = 9007199254740993ULL;
constexpr unsigned int U32Max = 4294967295U;
constexpr int I32Max = 2147483647;
constexpr int I32Min = -2147483647 - 1;

static_assert(sizeof(void*) * CHAR_BIT == 64, "CPU fixture requires 64-bit pointers");
static_assert(std::numeric_limits<float>::is_iec559 && std::numeric_limits<float>::digits == 24);
static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits == 53);
static_assert(std::is_final_v<Store> && std::is_base_of_v<NVSDK_NGX_Parameter, Store>);
static_assert(!std::is_abstract_v<Store>);
static_assert(!std::has_virtual_destructor_v<NVSDK_NGX_Parameter>);
static_assert(!std::is_copy_constructible_v<Store> && !std::is_copy_assignable_v<Store>);
static_assert(!std::is_move_constructible_v<Store> && !std::is_move_assignable_v<Store>);
static_assert(std::is_nothrow_default_constructible_v<Store> && std::is_nothrow_destructible_v<Store>);

template <typename... Values>
constexpr bool NoThrowOverloads() noexcept
{
    return ((noexcept(std::declval<Store&>().Set("key", std::declval<Values>())) &&
             noexcept(std::declval<const Store&>().Get("key", std::declval<Values*>()))) && ...);
}
static_assert(NoThrowOverloads<U64, float, double, unsigned int, int,
                              ID3D11Resource*, ID3D12Resource*, void*>());
static_assert(noexcept(std::declval<Store&>().Reset()));
static_assert(noexcept(std::declval<const Store&>().Healthy()));
static_assert(noexcept(std::declval<const Store&>().FirstError()));
static_assert(noexcept(std::declval<const Store&>().Size()));

template <std::size_t N>
constexpr bool IsSha256(const char (&value)[N]) noexcept
{
    if constexpr (N != 65)
        return false;
    else
    {
        for (std::size_t i = 0; i < 64; ++i)
            if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f')))
                return false;
        return value[64] == '\0';
    }
}
namespace inputs = encore_research::build_inputs;
static_assert(IsSha256(inputs::storeSha256) && IsSha256(inputs::consumerCppSha256) &&
              IsSha256(inputs::consumerHeaderSha256) && IsSha256(inputs::testsSha256) &&
              IsSha256(inputs::cmakeSha256) && IsSha256(inputs::templateSha256) &&
              IsSha256(inputs::ngxParamsSha256) && IsSha256(inputs::ngxDefsSha256));
static_assert(sizeof(inputs::compilerId) > 1 && sizeof(inputs::compilerVersion) > 1);

struct Checks
{
    unsigned int checks = 0;
    unsigned int failures = 0;

    void Expect(bool passed, const char* scenario, const char* detail = "") noexcept
    {
        ++checks;
        if (!passed)
        {
            ++failures;
            std::fprintf(stderr, "FAIL: %s %s\n", scenario, detail);
        }
    }
};

template <typename T>
bool SameValue(T left, T right) noexcept
{
    if constexpr (std::is_floating_point_v<T>)
        return left == right && (left != T{0} || std::signbit(left) == std::signbit(right));
    else
        return left == right;
}

template <typename T>
void Read(Checks& checks, const NVSDK_NGX_Parameter& parameters, const char* name,
          T expected, const char* scenario)
{
    T value{};
    if constexpr (std::is_arithmetic_v<T>)
        value = expected == static_cast<T>(73) ? static_cast<T>(74) : static_cast<T>(73);
    const auto result = parameters.Get(name, &value);
    checks.Expect(result == Success && SameValue(value, expected), scenario);
}

template <typename T>
void Unchanged(Checks& checks, const NVSDK_NGX_Parameter& parameters, const char* name,
               NVSDK_NGX_Result expected, T sentinel, const char* scenario)
{
    T value = sentinel;
    const auto result = parameters.Get(name, &value);
    checks.Expect(result == expected && SameValue(value, sentinel), scenario);
}

// Addresses are opaque CPU tokens only, never COM objects or dereference targets.
struct Tokens
{
    alignas(std::max_align_t) unsigned char first[64]{};
    alignas(std::max_align_t) unsigned char second[64]{};
    alignas(std::max_align_t) unsigned char third[64]{};
};

void CheckConsumer(Checks& checks, const encore_research::ConsumerResults& results,
                   const encore_research::ConsumerValues& actual,
                   const encore_research::ConsumerValues& expected,
                   NVSDK_NGX_Result expectedResult, const char* phase)
{
    checks.Expect(results.ull == expectedResult && actual.ull == expected.ull, phase, "ULL");
    checks.Expect(results.single == expectedResult && SameValue(actual.single, expected.single), phase, "float");
    checks.Expect(results.real == expectedResult && SameValue(actual.real, expected.real), phase, "double");
    checks.Expect(results.uint == expectedResult && actual.uint == expected.uint, phase, "uint");
    checks.Expect(results.integer == expectedResult && actual.integer == expected.integer, phase, "int");
    checks.Expect(results.d3d11 == expectedResult && actual.d3d11 == expected.d3d11, phase, "D3D11 pointer");
    checks.Expect(results.d3d12 == expectedResult && actual.d3d12 == expected.d3d12, phase, "D3D12 pointer");
    checks.Expect(results.opaque == expectedResult && actual.opaque == expected.opaque, phase, "void pointer");
}

void CrossTranslationUnit(Checks& checks)
{
    Tokens tokens;
    Store store;
    NVSDK_NGX_Parameter& publicParameters = store;
    const encore_research::ConsumerValues input{
        U64Max, -13.5F, 1.23456789012345, U32Max, I32Min,
        static_cast<ID3D11Resource*>(static_cast<void*>(tokens.first)),
        static_cast<ID3D12Resource*>(static_cast<void*>(tokens.second)), tokens.third};
    const encore_research::ConsumerValues sentinel{
        77ULL, 91.25F, -92.5, 93U, -94,
        static_cast<ID3D11Resource*>(static_cast<void*>(tokens.third)),
        static_cast<ID3D12Resource*>(static_cast<void*>(tokens.first)), tokens.second};

    auto output = sentinel;
    auto results = encore_research::ConsumerGetAll(publicParameters, output);
    CheckConsumer(checks, results, output, sentinel, Missing, "cross-TU empty");
    encore_research::ConsumerSetAll(publicParameters, input);
    results = encore_research::ConsumerGetAll(publicParameters, output);
    CheckConsumer(checks, results, output, input, Success, "cross-TU round trip");
    checks.Expect(store.Healthy() && store.Size() == 8, "eight public setter overloads inserted distinct keys");

    store.Set(nullptr, 1);
    output = sentinel;
    results = encore_research::ConsumerGetAll(publicParameters, output);
    CheckConsumer(checks, results, output, sentinel, Fail, "cross-TU sticky blocks all getters");
    encore_research::ConsumerReset(publicParameters);
    checks.Expect(store.Healthy() && store.Size() == 0 && store.FirstError() == Store::Error::None,
                  "cross-TU Reset restores empty health");
    results = encore_research::ConsumerGetAll(publicParameters, output);
    CheckConsumer(checks, results, output, sentinel, Missing, "cross-TU Reset cleared all overloads");
    encore_research::ConsumerSetAll(publicParameters, input);
    results = encore_research::ConsumerGetAll(publicParameters, output);
    CheckConsumer(checks, results, output, input, Success, "cross-TU recovery round trip");
}

void IntegerRanges(Checks& checks)
{
    Store store;
    store.Set("value", U64Max);
    Read(checks, store, "value", U64Max, "ULLMAX remains exact");
    Unchanged(checks, store, "value", Missing, 71U, "ULLMAX cannot narrow to U32");
    Unchanged(checks, store, "value", Missing, -71, "ULLMAX cannot narrow to I32");
    Read(checks, store, "value", 0x1p64, "ULLMAX may round to finite double");
    Read(checks, store, "value", 0x1p64F, "ULLMAX may round to finite float");
    Read(checks, store, "value", U64Max, "floating reads never rewrite ULL storage");
    store.Set("value", BeyondDoubleInteger);
    Read(checks, store, "value", BeyondDoubleInteger, "2^53+1 remains exact");
    Read(checks, store, "value", 0x1p53, "2^53+1 permits rounded double output");
    Read(checks, store, "value", BeyondDoubleInteger, "rounded double read preserves original integer");

    store.Set("value", U32Max);
    Read(checks, store, "value", U32Max, "unsigned setter retains U32MAX");
    Read(checks, store, "value", 4294967295ULL, "U32MAX widens exactly");
    Unchanged(checks, store, "value", Missing, -71, "U32MAX exceeds signed maximum");
    store.Set("value", 2147483647U);
    Read(checks, store, "value", I32Max, "U32 at I32MAX narrows exactly");
    store.Set("value", 2147483648ULL);
    Read(checks, store, "value", 2147483648U, "I32 upper bound remains valid U32");
    Unchanged(checks, store, "value", Missing, -71, "I32MAX+1 rejected as integer");
    store.Set("value", 4294967296ULL);
    Unchanged(checks, store, "value", Missing, 71U, "U32MAX+1 rejected as integer");

    store.Set("value", I32Min);
    Read(checks, store, "value", I32Min, "I32MIN remains exact");
    Unchanged(checks, store, "value", Missing, 71U, "I32MIN cannot become unsigned");
    Unchanged(checks, store, "value", Missing, 71ULL, "I32MIN cannot become ULL");
    store.Set("value", I32Max);
    Read(checks, store, "value", I32Max, "I32MAX remains exact");
    Read(checks, store, "value", 2147483647U, "positive I32 widens to U32");
    Read(checks, store, "value", 2147483647ULL, "positive I32 widens to ULL");
    store.Set("value", -1);
    Unchanged(checks, store, "value", Missing, 71U, "negative integer cannot become U32");
    Unchanged(checks, store, "value", Missing, 71ULL, "negative integer cannot become ULL");
    store.Set("value", 0);
    Read(checks, store, "value", 0U, "integer zero converts to U32");
    Read(checks, store, "value", 0ULL, "integer zero converts to ULL");
    store.Set("value", -42);
    Read(checks, store, "value", -42.0F, "signed integer converts to float");
    Read(checks, store, "value", -42.0, "signed integer converts to double");
    checks.Expect(store.Healthy() && store.Size() == 1, "integer range failures do not poison or insert");
}

template <typename Source, typename Target>
struct ConversionCase
{
    Source input;
    NVSDK_NGX_Result result;
    Target expected;
    const char* scenario;
};

template <typename Source, typename Target, std::size_t N>
void ConversionCases(Checks& checks, const std::array<ConversionCase<Source, Target>, N>& cases,
                     const char* group)
{
    Store store;
    for (const auto& test : cases)
    {
        store.Set("value", test.input);
        Target output = static_cast<Target>(73);
        const auto result = store.Get("value", &output);
        const Target expected = test.result == Success ? test.expected : static_cast<Target>(73);
        checks.Expect(result == test.result && SameValue(output, expected) && store.Healthy(),
                      group, test.scenario);
    }
}

template <typename Source>
void FloatingIntegerBounds(Checks& checks, const char* group)
{
    const Source infinity = std::numeric_limits<Source>::infinity();
    const Source upper32 = static_cast<Source>(0x1p32);
    const Source upper64 = static_cast<Source>(0x1p64);
    const Source upperSigned = static_cast<Source>(0x1p31);
    const Source lowerSigned = -upperSigned;
    // IEEE binary32/64 predecessors, independently stated as integer constants.
    constexpr unsigned int below32 = std::is_same_v<Source, float> ? 4294967040U : 4294967295U;
    constexpr U64 below64 = std::is_same_v<Source, float> ? 18446742974197923840ULL : 18446744073709549568ULL;
    constexpr int belowSigned = std::is_same_v<Source, float> ? 2147483520 : I32Max;
    constexpr int aboveLower = std::is_same_v<Source, float> ? -2147483520 : -2147483647;

    ConversionCases(checks, std::array<ConversionCase<Source, unsigned int>, 7>{{
        {static_cast<Source>(-1), Missing, 0U, "negative integral to U32"},
        {static_cast<Source>(-0.75), Success, 0U, "negative fraction truncates to unsigned zero"},
        {static_cast<Source>(0), Success, 0U, "U32 lower inclusive"},
        {std::nextafter(upper32, static_cast<Source>(0)), Success, below32, "U32 upper predecessor"},
        {upper32, Missing, 0U, "2^32 exclusive"},
        {std::nextafter(upper32, infinity), Missing, 0U, "above 2^32"},
        {(std::numeric_limits<Source>::max)(), Missing, 0U, "large finite to U32"}
    }}, group);
    ConversionCases(checks, std::array<ConversionCase<Source, U64>, 7>{{
        {static_cast<Source>(-1), Missing, 0ULL, "negative integral to U64"},
        {static_cast<Source>(-0.75), Success, 0ULL, "negative fraction truncates to U64 zero"},
        {static_cast<Source>(0), Success, 0ULL, "U64 lower inclusive"},
        {std::nextafter(upper64, static_cast<Source>(0)), Success, below64, "U64 upper predecessor"},
        {upper64, Missing, 0ULL, "2^64 exclusive; never cast rounded ULLMAX"},
        {std::nextafter(upper64, infinity), Missing, 0ULL, "above 2^64"},
        {(std::numeric_limits<Source>::max)(), Missing, 0ULL, "large finite to U64"}
    }}, group);
    ConversionCases(checks, std::array<ConversionCase<Source, int>, 8>{{
        {std::nextafter(lowerSigned, -infinity), std::is_same_v<Source, float> ? Missing : Success,
         I32Min, "below -2^31: float is out of range, double fraction truncates to INT_MIN"},
        {lowerSigned, Success, I32Min, "-2^31 inclusive"},
        {std::nextafter(lowerSigned, static_cast<Source>(0)), Success, aboveLower, "above -2^31 truncates toward zero"},
        {std::nextafter(upperSigned, static_cast<Source>(0)), Success, belowSigned, "I32 upper predecessor"},
        {upperSigned, Missing, 0, "2^31 exclusive"},
        {std::nextafter(upperSigned, infinity), Missing, 0, "above 2^31"},
        {(std::numeric_limits<Source>::max)(), Missing, 0, "large positive to I32"},
        {-(std::numeric_limits<Source>::max)(), Missing, 0, "large negative to I32"}
    }}, group);
}

void FractionalEdges(Checks& checks)
{
    ConversionCases(checks, std::array<ConversionCase<double, int>, 4>{{
        {-2147483648.75, Success, I32Min, "truncate before lower-bound comparison"},
        {-2147483649.0, Missing, 0, "whole value below lower bound"},
        {2147483647.75, Success, I32Max, "truncate before upper-bound comparison"},
        {2147483648.0, Missing, 0, "exact exclusive signed upper bound"}
    }}, "fractional I32 edges");
    ConversionCases(checks, std::array<ConversionCase<double, unsigned int>, 4>{{
        {4294967295.75, Success, U32Max, "fraction above U32MAX truncates into range"},
        {4294967296.0, Missing, 0U, "exact exclusive unsigned upper bound"},
        {-0.9999999999999999, Success, 0U, "negative near-minus-one truncates to zero"},
        {-1.0001, Missing, 0U, "negative value truncates below zero"}
    }}, "fractional U32 edges");
}

void FloatingOutputsAndZero(Checks& checks)
{
    Store store;
    const float floatMax = (std::numeric_limits<float>::max)();
    const double doubleMax = (std::numeric_limits<double>::max)();
    const double wideFloatMax = static_cast<double>(floatMax);
    const double infinity = std::numeric_limits<double>::infinity();

    store.Set("value", doubleMax);
    Read(checks, store, "value", doubleMax, "finite double maximum retained");
    Unchanged(checks, store, "value", Missing, -19.25F, "double maximum cannot narrow to float");
    store.Set("value", -doubleMax);
    Unchanged(checks, store, "value", Missing, -19.25F, "negative double maximum cannot narrow to float");
    store.Set("value", wideFloatMax);
    Read(checks, store, "value", floatMax, "exact float maximum may narrow");
    store.Set("value", std::nextafter(wideFloatMax, infinity));
    Unchanged(checks, store, "value", Missing, -19.25F, "just above finite float range rejected before rounding");
    store.Set("value", std::nextafter(-wideFloatMax, -infinity));
    Unchanged(checks, store, "value", Missing, -19.25F, "just below finite float range rejected");
    store.Set("value", floatMax);
    Read(checks, store, "value", wideFloatMax, "float maximum widens to double");
    store.Set("value", std::numeric_limits<float>::denorm_min());
    Read(checks, store, "value", std::numeric_limits<float>::denorm_min(), "float subnormal retained");
    Read(checks, store, "value", static_cast<double>(std::numeric_limits<float>::denorm_min()),
         "float subnormal widens without loss");
    store.Set("value", std::numeric_limits<double>::denorm_min());
    Read(checks, store, "value", 0.0F, "positive double underflow may round to positive float zero");
    store.Set("value", -std::numeric_limits<double>::denorm_min());
    Read(checks, store, "value", -0.0F, "negative double underflow preserves zero sign");

    store.Set("value", -0.0F);
    Read(checks, store, "value", -0.0F, "float negative zero retained");
    Read(checks, store, "value", -0.0, "float negative zero widens with sign");
    Read(checks, store, "value", 0ULL, "negative zero truncates to unsigned zero");
    Read(checks, store, "value", 0, "negative zero truncates to signed zero");
    store.Set("value", -0.0);
    Read(checks, store, "value", -0.0, "double negative zero retained");
    Read(checks, store, "value", -0.0F, "double negative zero narrows with sign");
    store.Set("value", 0.0);
    Read(checks, store, "value", 0.0, "positive zero replaces negative numeric zero");
    Read(checks, store, "value", 0.0F, "positive double zero narrows with sign");
    checks.Expect(store.Healthy(), "floating range failures never poison the store");
}

void ReadSharedPointer(Checks& checks, const NVSDK_NGX_Parameter& parameters,
                       const char* name, void* expected, const char* phase)
{
    Tokens sentinel;
    auto* d3d11 = static_cast<ID3D11Resource*>(static_cast<void*>(sentinel.first));
    auto* d3d12 = static_cast<ID3D12Resource*>(static_cast<void*>(sentinel.second));
    void* opaque = sentinel.third;
    const auto result11 = parameters.Get(name, &d3d11);
    const auto result12 = parameters.Get(name, &d3d12);
    const auto resultVoid = parameters.Get(name, &opaque);
    checks.Expect(result11 == Success && static_cast<void*>(d3d11) == expected, phase, "D3D11 view");
    checks.Expect(result12 == Success && static_cast<void*>(d3d12) == expected, phase, "D3D12 view");
    checks.Expect(resultVoid == Success && opaque == expected, phase, "void view");
}

void IndependentSlots(Checks& checks)
{
    Tokens tokens;
    Store store;
    auto* const pointer11 = static_cast<ID3D11Resource*>(static_cast<void*>(tokens.first));
    auto* const pointer12 = static_cast<ID3D12Resource*>(static_cast<void*>(tokens.second));
    void* const third = tokens.third;

    store.Set("numeric-first", BeyondDoubleInteger);
    Unchanged(checks, store, "numeric-first", Missing, pointer11, "numeric-only has no D3D11 pointer");
    Unchanged(checks, store, "numeric-first", Missing, pointer12, "numeric-only has no D3D12 pointer");
    Unchanged(checks, store, "numeric-first", Missing, third, "numeric-only has no void pointer");
    store.Set("numeric-first", pointer11);
    Read(checks, store, "numeric-first", BeyondDoubleInteger, "numeric -> pointer preserves exact ULL");
    ReadSharedPointer(checks, store, "numeric-first", tokens.first, "resource setter shares pointer slot");
    store.Set("numeric-first", -12.75);
    Read(checks, store, "numeric-first", -12.75, "numeric -> pointer -> numeric replaces numeric kind");
    Read(checks, store, "numeric-first", -12, "updated numeric kind uses truncation");
    ReadSharedPointer(checks, store, "numeric-first", tokens.first, "numeric update retains all pointer views");

    store.Set("pointer-first", pointer12);
    Unchanged(checks, store, "pointer-first", Missing, 71ULL, "pointer-only has no ULL");
    Unchanged(checks, store, "pointer-first", Missing, -71.25F, "pointer-only has no float");
    Unchanged(checks, store, "pointer-first", Missing, -71.25, "pointer-only has no double");
    Unchanged(checks, store, "pointer-first", Missing, 71U, "pointer-only has no U32");
    Unchanged(checks, store, "pointer-first", Missing, -71, "pointer-only has no I32");
    store.Set("pointer-first", -7);
    ReadSharedPointer(checks, store, "pointer-first", tokens.second, "pointer -> numeric retains pointer");
    store.Set("pointer-first", third);
    Read(checks, store, "pointer-first", -7, "pointer -> numeric -> pointer retains numeric");
    ReadSharedPointer(checks, store, "pointer-first", third, "void setter replaces shared pointer slot");

    const auto checkNull = [&](auto nullValue, const char* phase)
    {
        store.Set("pointer-first", third);
        store.Set("pointer-first", nullValue);
        ReadSharedPointer(checks, store, "pointer-first", nullptr, phase);
        Read(checks, store, "pointer-first", -7, "nullptr pointer update does not erase numeric");
    };
    checkNull(static_cast<ID3D11Resource*>(nullptr), "D3D11 nullptr is present");
    checkNull(static_cast<ID3D12Resource*>(nullptr), "D3D12 nullptr is present");
    checkNull(static_cast<void*>(nullptr), "void nullptr is present");
    checks.Expect(store.Healthy() && store.Size() == 2, "two-slot writes consume one entry per key");

    store.Reset();
    checks.Expect(store.Healthy() && store.Size() == 0, "Reset clears dual-slot entries");
    Unchanged(checks, store, "numeric-first", Missing, -71, "Reset removes numeric slot");
    Unchanged(checks, store, "numeric-first", Missing, third, "Reset removes pointer slot");
    store.Set("numeric-first", 3);
    Unchanged(checks, store, "numeric-first", Missing, third, "reused entry does not inherit old pointer");
    store.Reset();
    store.Set("numeric-first", static_cast<void*>(nullptr));
    ReadSharedPointer(checks, store, "numeric-first", nullptr, "null-only entry remains present");
    Unchanged(checks, store, "numeric-first", Missing, -71, "reused pointer entry does not inherit numeric");
}

template <typename T>
void InvalidGetterOverload(Checks& checks, Store& store, const char* tooLong,
                           T sentinel, const char* type)
{
    const NVSDK_NGX_Parameter& parameters = store;
    Unchanged(checks, parameters, nullptr, Invalid, sentinel, type);
    Unchanged(checks, parameters, "", Invalid, sentinel, type);
    Unchanged(checks, parameters, tooLong, Invalid, sentinel, type);
    Unchanged(checks, parameters, "missing", Missing, sentinel, type);
    checks.Expect(parameters.Get("both", static_cast<T*>(nullptr)) == Invalid,
                  "null output rejected", type);
    checks.Expect(parameters.Get(nullptr, static_cast<T*>(nullptr)) == Invalid,
                  "null name and output rejected", type);
    checks.Expect(store.Healthy() && store.FirstError() == Store::Error::None && store.Size() == 1,
                  "invalid getter never poisons or inserts", type);
}

template <typename... Values>
void UnhealthyArgumentPriority(Checks& checks, const Store& store)
{
    // The poisoned-store result takes precedence even over invalid arguments.
    (checks.Expect(store.Get(nullptr, static_cast<Values*>(nullptr)) == Fail,
                   "sticky failure takes priority over null getter arguments"), ...);
}

void InvalidGetters(Checks& checks)
{
    Tokens tokens;
    Store store;
    std::array<char, 129> tooLong{};
    tooLong.fill('x');
    tooLong.back() = '\0';
    store.Set("both", 42);
    store.Set("both", static_cast<void*>(tokens.first));
    InvalidGetterOverload(checks, store, tooLong.data(), 71ULL, "invalid ULL getter");
    InvalidGetterOverload(checks, store, tooLong.data(), -71.25F, "invalid float getter");
    InvalidGetterOverload(checks, store, tooLong.data(), -71.25, "invalid double getter");
    InvalidGetterOverload(checks, store, tooLong.data(), 71U, "invalid U32 getter");
    InvalidGetterOverload(checks, store, tooLong.data(), -71, "invalid I32 getter");
    InvalidGetterOverload(checks, store, tooLong.data(),
                          static_cast<ID3D11Resource*>(static_cast<void*>(tokens.second)), "invalid D3D11 getter");
    InvalidGetterOverload(checks, store, tooLong.data(),
                          static_cast<ID3D12Resource*>(static_cast<void*>(tokens.second)), "invalid D3D12 getter");
    InvalidGetterOverload(checks, store, tooLong.data(), static_cast<void*>(tokens.second), "invalid void getter");
    Read(checks, store, "both", 42, "invalid getter sequence preserves numeric value");
    ReadSharedPointer(checks, store, "both", tokens.first, "invalid getter sequence preserves pointer value");
    store.Set(nullptr, 0);
    UnhealthyArgumentPriority<U64, float, double, unsigned int, int,
                              ID3D11Resource*, ID3D12Resource*, void*>(checks, store);
}

void NamesAndCapacity(Checks& checks)
{
    using SmallStore = encore_research::NrParameterStore<4, 4>;
    SmallStore small;
    Tokens tokens;
    char borrowedName[] = "Ab";
    small.Set(borrowedName, 11);
    borrowedName[0] = 'X';
    small.Set("ab", 12);
    small.Set("Abc", 13);
    small.Set("abcd", 14);
    Read(checks, small, "Ab", 11, "key is copied instead of borrowing caller memory");
    Read(checks, small, "ab", 12, "case-sensitive key remains distinct");
    Read(checks, small, "Abc", 13, "shared-prefix key remains distinct");
    Read(checks, small, "abcd", 14, "exact MaxName length accepted");
    Unchanged(checks, small, "AB", Missing, -71, "case mismatch never aliases an entry");
    small.Set("Abc", BeyondDoubleInteger);
    small.Set("abcd", static_cast<void*>(tokens.first));
    small.Set("abcd", 14.5F);
    Read(checks, small, "Abc", BeyondDoubleInteger, "full capacity allows existing numeric update");
    Read(checks, small, "abcd", 14.5F, "full capacity allows numeric kind change");
    ReadSharedPointer(checks, small, "abcd", tokens.first, "full capacity allows adding second slot");
    Read(checks, small, "Ab", 11, "other key survives full-capacity updates");
    Read(checks, small, "ab", 12, "different case survives full-capacity updates");
    checks.Expect(small.Healthy() && small.Size() == 4, "full updates do not require another entry");
    small.Set("zz", 99);
    checks.Expect(!small.Healthy() && small.FirstError() == SmallStore::Error::CapacityExceeded && small.Size() == 4,
                  "new key at capacity latches failure without insertion");
    Unchanged(checks, small, "Ab", Fail, -71, "overflow blocks stale numeric reads");
    Unchanged(checks, small, "abcd", Fail, static_cast<void*>(tokens.second), "overflow blocks stale pointer reads");
    small.Set(nullptr, 9);
    small.Set("Ab", std::numeric_limits<double>::infinity());
    checks.Expect(small.FirstError() == SmallStore::Error::CapacityExceeded, "first sticky error is retained");
    small.Reset();
    small.Set("zz", 99);
    Read(checks, small, "zz", 99, "Reset recovers from capacity error");
    Unchanged(checks, small, "abcd", Missing, static_cast<void*>(tokens.first), "Reset discards old full-store keys");

    Store names;
    std::array<char, 128> maximumName{};
    maximumName.fill('k');
    maximumName.back() = '\0';
    std::array<char, 129> tooLong{};
    tooLong.fill('k');
    tooLong.back() = '\0';
    names.Set(maximumName.data(), U64Max);
    Read(checks, names, maximumName.data(), U64Max, "default 127-byte key accepted");
    Unchanged(checks, names, tooLong.data(), Invalid, -71, "128-byte getter name rejected without output write");
    checks.Expect(names.Healthy() && names.Size() == 1, "overlong getter does not latch or insert");
    names.Set(tooLong.data(), 1);
    checks.Expect(names.FirstError() == Store::Error::NameTooLong && names.Size() == 1,
                  "overlong setter latches error without aliasing maximum name");
    Unchanged(checks, names, maximumName.data(), Fail, 71ULL, "overlong setter prevents stale reads");
    names.Reset();
    names.Set(maximumName.data(), 17);
    Read(checks, names, maximumName.data(), 17, "maximum name reusable after Reset");

    encore_research::NrParameterStore<0, 4> zeroCapacity;
    zeroCapacity.Set("key", 1);
    checks.Expect(zeroCapacity.FirstError() == decltype(zeroCapacity)::Error::CapacityExceeded,
                  "zero capacity rejects insertion without array access");
    zeroCapacity.Reset();
    Unchanged(checks, zeroCapacity, "key", Missing, -71, "zero-capacity Reset remains usable");
}

void DefaultCapacity(Checks& checks)
{
    Store store;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned int i = 0; i < 64; ++i)
    {
        const char key[]{'k', hex[i / 16], hex[i % 16], '\0'};
        store.Set(key, i);
    }
    bool distinct = true;
    for (unsigned int i = 0; i < 64; ++i)
    {
        const char key[]{'k', hex[i / 16], hex[i % 16], '\0'};
        unsigned int output = 999;
        const auto result = store.Get(key, &output);
        if (result != Success || output != i)
            distinct = false;
    }
    checks.Expect(distinct && store.Healthy() && store.Size() == 64,
                  "default 64-entry store preserves all distinct keys");
    store.Set("k3f", U64Max);
    Read(checks, store, "k3f", U64Max, "default full store permits last-entry update");
    store.Set("extra", 64U);
    checks.Expect(store.FirstError() == Store::Error::CapacityExceeded && store.Size() == 64,
                  "default store rejects entry 65");
    Unchanged(checks, store, "k00", Fail, 71U, "default overflow blocks existing first entry");
}

void InvalidSetterNames(Checks& checks)
{
    Tokens tokens;
    std::array<char, 129> tooLong{};
    tooLong.fill('x');
    tooLong.back() = '\0';
    const std::array<const char*, 3> names{nullptr, "", tooLong.data()};
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        // Exercise both setter routes without repeating every numeric overload.
        for (unsigned int pointerRoute = 0; pointerRoute < 2; ++pointerRoute)
        {
            Store store;
            store.Set("old", 41);
            store.Set("old", static_cast<void*>(tokens.first));
            if (pointerRoute == 0)
                store.Set(names[i], 19);
            else
                store.Set(names[i], static_cast<void*>(nullptr));
            const auto expected = i == 2 ? Store::Error::NameTooLong : Store::Error::InvalidName;
            int numeric = 71;
            void* pointer = tokens.second;
            const auto numericResult = store.Get("old", &numeric);
            const auto pointerResult = store.Get("old", &pointer);
            checks.Expect(!store.Healthy() && store.FirstError() == expected && store.Size() == 1 &&
                          numericResult == Fail && numeric == 71 &&
                          pointerResult == Fail && pointer == tokens.second,
                          "invalid setter name latches and blocks both slots");
            store.Set("new", std::numeric_limits<double>::infinity());
            checks.Expect(store.FirstError() == expected && store.Size() == 1,
                          "later setter failure cannot replace first error");
            store.Reset();
            store.Set("old", 8);
            Read(checks, store, "old", 8, "Reset recovers from invalid setter name");
            checks.Expect(store.Healthy() && store.FirstError() == Store::Error::None,
                          "Reset clears sticky name error");
        }
    }
}

template <typename Source>
void NonFiniteSetters(Checks& checks, const char* group)
{
    Tokens tokens;
    const std::array<Source, 3> values{
        std::numeric_limits<Source>::infinity(),
        -std::numeric_limits<Source>::infinity(),
        std::numeric_limits<Source>::quiet_NaN()};
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        Store store;
        store.Set("old", 41);
        store.Set("old", static_cast<void*>(tokens.first));
        store.Set(i == 1 ? "new" : "old", values[i]);
        int numeric = 71;
        void* pointer = tokens.second;
        const auto numericResult = store.Get("old", &numeric);
        const auto pointerResult = store.Get("old", &pointer);
        checks.Expect(store.FirstError() == Store::Error::NonFiniteValue && store.Size() == 1 &&
                      numericResult == Fail && numeric == 71 &&
                      pointerResult == Fail && pointer == tokens.second,
                      group, "infinity/NaN setter poisons all reads without writing outputs");
        store.Set(nullptr, 9);
        checks.Expect(store.FirstError() == Store::Error::NonFiniteValue,
                      group, "nonfinite error remains first");
        store.Reset();
        store.Set("old", static_cast<Source>(3.5));
        Read(checks, store, "old", static_cast<Source>(3.5), "finite numeric setter works after Reset");
        Unchanged(checks, store, "old", Missing, static_cast<void*>(tokens.second),
                  "Reset after nonfinite failure clears old pointer slot");
        checks.Expect(store.Healthy(), group, "getter failure after recovery remains healthy");
    }
}

void PrintReceipt(const Checks& checks)
{
    // Exactly one stdout JSON record. These hashes bind configured inputs to
    // this EXE; passing same-compiler virtual calls is not payload ABI evidence.
    std::printf(
        "{\"checks\":%u,\"failures\":%u,\"inputs\":{"
        "\"NrParameterStore.h\":\"%s\","
        "\"NrParameterStoreConsumer.cpp\":\"%s\","
        "\"NrParameterStoreConsumer.h\":\"%s\","
        "\"NrParameterStoreTests.cpp\":\"%s\","
        "\"CMakeLists.txt\":\"%s\","
        "\"NativeBuildInputs.h.in\":\"%s\","
        "\"nvsdk_ngx_params.h\":\"%s\","
        "\"nvsdk_ngx_defs.h\":\"%s\"},"
        "\"compiler\":{\"id\":\"%s\",\"version\":\"%s\"},"
        "\"pointerBits\":%u,\"payload_called\":false,\"sdk_initialized\":false,\"gpu_tested\":false}\n",
        checks.checks, checks.failures,
        inputs::storeSha256, inputs::consumerCppSha256, inputs::consumerHeaderSha256,
        inputs::testsSha256, inputs::cmakeSha256, inputs::templateSha256,
        inputs::ngxParamsSha256, inputs::ngxDefsSha256, inputs::compilerId, inputs::compilerVersion,
        static_cast<unsigned int>(sizeof(void*) * CHAR_BIT));
}

} // namespace

int main()
{
    Checks checks;
    CrossTranslationUnit(checks);
    IntegerRanges(checks);
    FloatingIntegerBounds<float>(checks, "float -> integer bounds");
    FloatingIntegerBounds<double>(checks, "double -> integer bounds");
    FractionalEdges(checks);
    FloatingOutputsAndZero(checks);
    IndependentSlots(checks);
    InvalidGetters(checks);
    NamesAndCapacity(checks);
    DefaultCapacity(checks);
    InvalidSetterNames(checks);
    NonFiniteSetters<float>(checks, "nonfinite float setter");
    NonFiniteSetters<double>(checks, "nonfinite double setter");
    PrintReceipt(checks);
    return checks.failures == 0 ? 0 : 1;
}
