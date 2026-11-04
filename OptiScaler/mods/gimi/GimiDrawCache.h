#pragma once

#include "GimiCommands.h"
#include "GimiIdentity.h"
#include <atomic>

namespace gimi
{
inline constexpr unsigned GimiVertexSlots = 8;
inline constexpr unsigned GimiCachedMatches = 128;

struct ShaderIdentity
{
    Knowledge knowledge = Knowledge::Unknown;
    std::uint64_t hash = 0, object = 0, generation = 0;
    // Must be the upstream shader_hash=3dmigoto identity, not an FSR classifier
    // hash. Capture/bytecode hashing integration is owned by the next adapter.
};
// Prepared once for this original shader object/generation and immutable Plan.
// The explicit ShaderOverride stays first; model groups append in canonical
// order. Draw never scans groups, matches regex, or rewrites these lists.
struct ShaderEntryLink
{
    ShaderIdentity original;
    ShaderModel model = ShaderModel::Unknown;
    Target stage;
    std::array<ProgramInvocation, GimiCachedMatches> programs{};
    std::uint32_t count = 0;
    float filterIndex = 0;
    bool valid = false;
};
struct ShaderEntryResult
{
    ShaderEntryLink link;
    std::vector<Diagnostic> diagnostics;
    explicit operator bool() const noexcept { return link.valid; }
};
// Present/load preparation only. Unknown non-null models, stage mismatch,
// duplicate overrides and capacity overflow reject the complete link.
struct ShaderGroupMatches
{
    const Id* programs = nullptr;
    std::uint32_t count = 0;
};
ShaderEntryResult LinkShaderEntry(const Plan&, ShaderIdentity, ShaderModel, Target stage,
                                 const ShaderGroupMatches* preparedPatterns = nullptr);

struct QualifiedLayout
{
    Knowledge knowledge = Knowledge::Unknown;
    std::uint64_t object = 0, contract = 0, deviceGeneration = 0;
};
struct LayoutElement
{
    std::string semantic;
    std::uint32_t semanticIndex = 0, slot = 0, offset = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
struct LayoutRequest
{
    SourceLocation source;
    std::uint64_t contract = 0; // immutable, globally unique across rebuilds
    std::uint64_t vertexShaderHash = 0;
    std::vector<LayoutElement> elements; // explicit byte offsets, no APPEND here
    std::array<std::uint32_t, GimiVertexSlots> strides{};
};
class LayoutContract final
{
    friend struct LayoutCompiler;
    LayoutRequest request_;
    std::array<std::uint32_t, GimiVertexSlots> ends_{};
    std::uint32_t slots_ = 0;
public:
    // Creation-time exact descriptor comparison. This slice requires explicit
    // offsets; APPEND layouts stay unsupported. No descriptor/string draw scans.
    QualifiedLayout Qualify(std::uint64_t layoutLease, std::uint64_t deviceGeneration,
                            const D3D11_INPUT_ELEMENT_DESC*, std::uint32_t count,
                            ShaderIdentity vertexShader) const noexcept;
    const auto& Strides() const noexcept { return request_.strides; }
    const auto& Ends() const noexcept { return ends_; }
    auto Slots() const noexcept { return slots_; }
    auto IdValue() const noexcept { return request_.contract; }
    auto VertexShaderHash() const noexcept { return request_.vertexShaderHash; }
};
struct LayoutResult
{
    std::shared_ptr<const LayoutContract> layout;
    std::vector<Diagnostic> diagnostics;
    explicit operator bool() const noexcept { return layout != nullptr; }
};
LayoutResult CompileLayout(LayoutRequest request);

struct IndexWindow
{
    bool known = false;
    std::uint64_t object = 0, contentGeneration = 0;
    std::uint32_t offset = 0, first = 0, count = 0, minimum = 0, maximum = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
// Off-draw only, scans ONLY an explicitly requested range in a full CPU IB
// image. Caller retains the resulting value with the resource/content revision;
// unknown GPU-only indices never get a guessed [0,IndexCount) vertex bound.
bool CompileIndexWindow(const ResourceIdentity& index, CpuImage fullBuffer, DXGI_FORMAT format,
                        std::uint32_t offset, std::uint32_t first, std::uint32_t count,
                        IndexWindow& out) noexcept;

struct DrawEpochs
{
    std::uint64_t plan = 0, consumer = 0, device = 0, context = 0, bindings = 0;
};
struct IndexedDrawSnapshot
{
    bool immediate = false, drawIndexed = false, internal = false;
    DrawEpochs epochs;
    DrawContext draw;
    QualifiedLayout layout;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ShaderIdentity vertexShader, pixelShader;
    Binding indexBinding;
    ResourceIdentity indexIdentity;
    IndexWindow indices;
    std::array<Binding, GimiVertexSlots> vertices{};
    std::array<ResourceIdentity, GimiVertexSlots> vertexIdentities{};
};
// Created at Load/Present, never by a draw. IDs belong to the exact immutable
// plan and retained resource descriptor, independent of content revisions.
struct OutputViewMetadata
{
    std::uint64_t view = 0, generation = 0;
    std::uint32_t dimension = 0, mip = 0, firstSlice = 0, sliceCount = 0, firstSubresource = 0, flags = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // view format, never resource match_format
    bool depth = false;
};
inline bool operator==(const OutputViewMetadata& a, const OutputViewMetadata& b) noexcept
{
    return a.view == b.view && a.generation == b.generation && a.dimension == b.dimension && a.mip == b.mip &&
        a.firstSlice == b.firstSlice && a.sliceCount == b.sliceCount && a.firstSubresource == b.firstSubresource &&
        a.flags == b.flags && a.format == b.format && a.depth == b.depth;
}
struct DescriptorMatches
{
    std::uint64_t planGeneration = 0, object = 0, deviceGeneration = 0, descriptorGeneration = 0;
    OutputViewMetadata output;
    std::array<Id, GimiCachedMatches> programs{};
    std::uint32_t count = 0;
    bool ready = false, overflow = false;
};
DescriptorMatches PrepareBufferDescriptorMatches(const Plan&, std::uint64_t planGeneration,
                                                  const ResourceIdentity&) noexcept;
DescriptorMatches PrepareTextureDescriptorMatches(const Plan&, std::uint64_t planGeneration,
                                                    const ResourceIdentity&, OutputViewMetadata) noexcept;
struct MatchSubject
{
    Target target;
    Binding binding;
    ResourceIdentity resource;
    ShaderIdentity shader;
    const DescriptorMatches* descriptors = nullptr; // retained serial owner, never a resource-read lease
};
enum class MatchStatus : std::uint8_t { Disabled, NoMatch, Matched, Unknown, Unsupported, Limit };
struct DrawMatches
{
    MatchStatus status = MatchStatus::Unknown;
    std::array<Id, GimiCachedMatches> programs{};
    std::uint32_t count = 0;
    float filterIndex = 0;
    bool filterKnown = false;
};

// One serial context owner, fixed direct-mapped cache with positive AND negative
// results. It caches only eligibility/program IDs/filter metadata; aliases,
// variables, expression branches and action batches remain live in CommandState.
// No lock is acquired here. Get enabled FIRST before entering an owner's lock.
class DrawCache final
{
public:
    DrawCache(std::shared_ptr<const Plan>, std::shared_ptr<const LayoutContract>,
              D3D11_PRIMITIVE_TOPOLOGY topology, std::uint64_t planGeneration,
              std::uint32_t slots = 256);
    ~DrawCache();
    DrawCache(const DrawCache&) = delete;
    DrawCache& operator=(const DrawCache&) = delete;
    // The outer runtime shares this gate and checks it before its lock. False
    // makes Lookup return without touching keys, resources or the interpreter.
    static bool Enabled(const std::atomic_bool& gate) noexcept { return gate.load(std::memory_order_acquire); }
    const DrawMatches& Lookup(bool enabled, const IndexedDrawSnapshot&, const MatchSubject&) noexcept;
    bool CopyTextureMatches(bool enabled, const IndexedDrawSnapshot&, const MatchSubject&,
                            Id* output, std::uint32_t capacity, std::uint32_t& count) noexcept;
    bool Filter(bool enabled, const IndexedDrawSnapshot&, const MatchSubject&, float&) noexcept;
private:
    struct Storage;
    std::unique_ptr<Storage> state_;
    const DrawMatches& LookupQuery(bool enabled, const IndexedDrawSnapshot&, const MatchSubject&, bool metadataQuery) noexcept;
};
struct DrawCacheResult
{
    std::unique_ptr<DrawCache> cache;
    std::vector<Diagnostic> diagnostics;
    explicit operator bool() const noexcept { return cache != nullptr; }
};
// Preferred load entry: rejects ambiguous ShaderOverride duplicate/overrule
// semantics, unreasonable limits and topology with actual plan source locations.
DrawCacheResult CreateDrawCache(std::shared_ptr<const Plan>, std::shared_ptr<const LayoutContract>,
                               D3D11_PRIMITIVE_TOPOLOGY, std::uint64_t planGeneration,
                               std::uint32_t slots = 256);

// Implemented glue for R478's callbacks, with the existing owner retaining game
// and preloaded leases. Providers resolve IDs, never cast them to COM pointers.
struct CachedIdentityQueries
{
    void* owner = nullptr;
    bool (*readBinding)(void*, Target, Binding&) noexcept = nullptr;
    bool (*resource)(void*, Binding, ResourceIdentity&) noexcept = nullptr;
    bool (*shader)(void*, Target, Binding, ShaderIdentity&) noexcept = nullptr;
    // Optional prepared link classification. Preserves explicit original
    // filter_index; model-only linked shaders use upstream default 1.
    bool (*shaderFilter)(void*, Target, Binding, float&) noexcept = nullptr;
    const DescriptorMatches* (*descriptorMatches)(void*, Binding) noexcept = nullptr;
    bool (*readPixelConstant)(void*, Id, Id, float&) noexcept = nullptr;
    bool (*nativeValue)(void*, NativeValue, Binding, float&) noexcept = nullptr;
};
class DrawQueryAdapter final
{
    DrawCache* cache_;
    const Plan* plan_;
    const IndexedDrawSnapshot* draw_;
    CachedIdentityQueries queries_;
    bool enabled_;
    bool Subject(const Plan&, const MatchQuery&, MatchSubject&) const noexcept;
    static bool Read(void*, Target, Binding&) noexcept;
    static bool Constant(void*, Id, Id, float&) noexcept;
    static bool Native(void*, NativeValue, Binding, float&) noexcept;
    static bool FilterQuery(void*, const Plan&, const MatchQuery&, float&) noexcept;
    static bool TextureQuery(void*, const Plan&, const MatchQuery&, Id*, std::uint32_t, std::uint32_t&) noexcept;
public:
    // Stack-safe, allocation-free; lifetime must cover PrepareDraw. Providers
    // are cached O(1) lookups. The original draw snapshot is fixed for this tape.
    DrawQueryAdapter(DrawCache& cache, const Plan& plan, const IndexedDrawSnapshot& draw,
                     CachedIdentityQueries queries, bool enabled) noexcept
        : cache_(&cache), plan_(&plan), draw_(&draw), queries_(queries), enabled_(enabled) {}
    CommandQueries Commands() noexcept;
};
// Outside the hook, for Unknown/Unsupported/Limit after rejecting the ENTIRE
// selected component. It supplies the selected INI file/line, never a hot log.
Diagnostic DescribeMatchFailure(const Plan&, Id selectedProgram, MatchStatus);
} // namespace gimi
