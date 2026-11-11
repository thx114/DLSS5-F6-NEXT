#include "GimiDrawCache.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace gimi
{
namespace
{
bool SameSemantic(std::string_view expected, const char* actual) noexcept
{
    if (!actual) return false;
    for (const auto c : expected)
    {
        auto a = *actual++;
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
        if (a != c) return false;
    }
    return !*actual;
}
bool KnownBuffer(const ResourceIdentity& r, Binding b, std::uint64_t device) noexcept
{
    constexpr UINT writes = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL;
    constexpr UINT shared = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    return r.knowledge == Knowledge::Known && r.kind == IdentityKind::Buffer && (r.contentKnown || r.gpuOrdered) && r.hashKnown &&
        r.object && r.object == b.object && r.deviceGeneration == device && b.generation == device &&
        r.buffer.ByteWidth == b.byteSize && !(r.buffer.BindFlags & (r.gpuOrdered ? (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL) : writes)) && !(r.buffer.MiscFlags & shared);
}
struct CacheKey
{
    // Only numeric lease IDs/generations/arguments, never content hashing.
    std::array<std::uint64_t, 176> words{};
    std::uint32_t count = 0;
    void Add(std::uint64_t v) noexcept { words[count++] = v; }
    bool operator==(const CacheKey& b) const noexcept
    {
        return count == b.count && std::equal(words.begin(), words.begin() + count, b.words.begin());
    }
};
std::uint64_t Pair(std::uint32_t a, std::uint32_t b) noexcept { return a | (std::uint64_t(b) << 32); }
void AddBinding(CacheKey& k, Binding b) noexcept
{
    k.Add(b.object); k.Add(b.view); k.Add(b.generation);
    k.Add(Pair(b.stride, b.offset)); k.Add(Pair(b.format, b.byteSize));
}
void AddIdentity(CacheKey& k, const ResourceIdentity& r) noexcept
{
    k.Add(r.object); k.Add(r.deviceGeneration); k.Add(r.contentGeneration); k.Add(r.descriptorGeneration);
    k.Add(std::uint64_t(r.upstreamHash) | (std::uint64_t(r.knowledge) << 32) | (std::uint64_t(r.kind) << 40) |
        (std::uint64_t(r.hashKnown) << 48) | (std::uint64_t(r.contentKnown) << 49) | (std::uint64_t(r.origin) << 50) | (std::uint64_t(r.gpuOrdered) << 53));
    k.Add(r.buffer.ByteWidth);
}
void AddShader(CacheKey& k, ShaderIdentity s) noexcept
{
    k.Add(std::uint64_t(s.knowledge)); k.Add(s.hash); k.Add(s.object); k.Add(s.generation);
}
void KeyOf(CacheKey& k, const IndexedDrawSnapshot& d, const MatchSubject& s, std::uint32_t vertexSlots) noexcept
{
    k.count = 0; // reuse allocated scratch; no full fixed-array clear per draw
    k.Add(d.epochs.plan); k.Add(d.epochs.consumer); k.Add(d.epochs.device); k.Add(d.epochs.context); k.Add(d.epochs.bindings);
    k.Add(d.immediate | (std::uint64_t(d.drawIndexed) << 1) | (std::uint64_t(d.internal) << 2) |
        (std::uint64_t(d.draw.drawIndexed) << 3) | (std::uint64_t(d.draw.instanced) << 4) |
        (std::uint64_t(d.draw.physicalCall) << 5));
    k.Add(Pair(d.draw.firstIndex, d.draw.indexCount)); k.Add(Pair(static_cast<std::uint32_t>(d.draw.baseVertex), d.draw.instanceCount));
    k.Add(Pair(d.draw.firstInstance, d.draw.vertexCount)); k.Add(d.draw.firstVertex);
    k.Add(d.layout.object); k.Add(d.layout.contract); k.Add(d.layout.deviceGeneration); k.Add(std::uint64_t(d.layout.knowledge));
    k.Add(d.topology); AddShader(k, d.vertexShader); AddShader(k, d.pixelShader);
    AddBinding(k, d.indexBinding); AddIdentity(k, d.indexIdentity);
    const auto& w = d.indices;
    k.Add(w.known); k.Add(w.object); k.Add(w.contentGeneration); k.Add(Pair(w.offset, w.first));
    k.Add(Pair(w.count, w.minimum)); k.Add(Pair(w.maximum, w.format));
    for (unsigned slot = 0; slot < GimiVertexSlots; ++slot)
        if (vertexSlots & (1u << slot)) { AddBinding(k, d.vertices[slot]); AddIdentity(k, d.vertexIdentities[slot]); }
    k.Add(Pair(static_cast<std::uint32_t>(s.target.kind), s.target.index));
    AddBinding(k, s.binding); AddIdentity(k, s.resource); AddShader(k, s.shader);
    // Candidate readiness can change at Present without a content write.
    const auto* prepared = s.descriptors;
    k.Add(prepared ? prepared->planGeneration : 0);
    k.Add(prepared ? prepared->object : 0); k.Add(prepared ? prepared->deviceGeneration : 0);
    k.Add(prepared ? prepared->descriptorGeneration : 0);
    k.Add(prepared ? (std::uint64_t(prepared->count) | (std::uint64_t(prepared->ready) << 32) |
        (std::uint64_t(prepared->overflow) << 33)) : 0);
    if (prepared)
    {
        const auto& o = prepared->output;
        k.Add(o.view); k.Add(o.generation); k.Add(Pair(o.dimension, o.mip)); k.Add(Pair(o.firstSlice, o.sliceCount));
        k.Add(Pair(o.firstSubresource, o.flags)); k.Add(Pair(o.format, o.depth));
    }
    // Maximum 163 words including query mode with all eight slots, under capacity.
}
constexpr DrawMatches Disabled{MatchStatus::Disabled};
} // namespace
ShaderEntryResult LinkShaderEntry(const Plan& plan, ShaderIdentity original, ShaderModel model, Target stage,
                                 const ShaderGroupMatches* preparedPatterns)
{
    ShaderEntryResult result;
    auto& link = result.link;
    link.original = original; link.model = model; link.stage = stage;
    const auto source = plan.Entries().empty() ? SourceLocation{} : plan.Programs()[plan.Entries().front()].source;
    const auto fail = [&](const SourceLocation& at, const char* message) {
        result.diagnostics.push_back({DiagnosticCode::Unsupported, at, message});
    };
    const bool vertex = stage.kind == TargetKind::VertexShader;
    if ((!vertex && stage.kind != TargetKind::PixelShader) || stage.index)
    { fail(source, "Automatic shader entry supports only exact VS/PS stages"); return result; }
    if (original.knowledge == Knowledge::Null && !original.hash && !original.object && !original.generation && !vertex)
    { link.filterIndex = -0.0f; link.valid = true; return result; }
    const bool vertexModel = model >= ShaderModel::VS40 && model <= ShaderModel::VS51;
    const bool pixelModel = model >= ShaderModel::PS40 && model <= ShaderModel::PS51;
    if (original.knowledge != Knowledge::Known || !original.object || !original.generation ||
        (vertex ? !vertexModel : !pixelModel))
    { fail(source, "Unknown original shader identity/reflected model; no automatic entry inferred"); return result; }
    const auto append = [&](Id program, const SourceLocation& at) {
        if (program >= plan.Programs().size() || link.count >= link.programs.size() ||
            link.count >= plan.Limits().matchesPerCheck)
        { fail(at, "Automatic shader entry exceeds bounded program capacity"); return false; }
        // No resource is being checked at a shader root; this is established
        // only by the interpreter's actual Check and restored on its return.
        link.programs[link.count++] = {program, {}};
        return true;
    };
    const auto overrides = plan.Candidates(original.hash, true);
    if (overrides.second > 1)
    { fail(overrides.first[1].source, "ShaderOverride equal-hash merging remains unsupported"); return result; }
    if (overrides.second)
    {
        const auto& entry = *overrides.first;
        if (!append(entry.program, entry.source)) return result;
        link.filterIndex = entry.hasFilterIndex ? entry.filterIndex : 1.0f;
    }
    bool explicitFilter = overrides.second && overrides.first->hasFilterIndex;
    for (const auto& group : plan.ShaderRegexGroups())
        if (std::find(group.models.begin(), group.models.end(), model) != group.models.end())
        {
            if (group.RequiresBytecode())
            {
                if (!preparedPatterns || preparedPatterns->count > GimiCachedMatches ||
                    (preparedPatterns->count && !preparedPatterns->programs))
                { fail(group.source, "Pattern groups require authentic prepared bytecode matches"); return result; }
                bool matched = false;
                for (std::uint32_t i = 0; i < preparedPatterns->count; ++i) matched |= preparedPatterns->programs[i] == group.program;
                if (!matched) continue;
            }
            const auto& program = plan.Programs()[group.program];
            if (program.commands[0].empty() && program.commands[1].empty() && !group.hasFilterIndex) continue;
            if (!append(group.program, group.source)) return result;
            if (!explicitFilter && group.hasFilterIndex) { link.filterIndex = group.filterIndex; explicitFilter = true; }
            else if (!explicitFilter) link.filterIndex = 1.0f;
        }
    link.valid = true;
    return result;
}

struct LayoutCompiler
{
    static LayoutResult Compile(LayoutRequest request)
    {
        const auto bad = [&](const char* message) -> LayoutResult {
            return {nullptr, {{DiagnosticCode::Unsupported, request.source, message}}};
        };
        if (!request.contract || request.elements.empty() || request.elements.size() > D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT)
            return bad("Exact nonempty bounded layout and unique contract ID required");
        auto contract = std::make_shared<LayoutContract>();
        for (auto& element : request.elements)
        {
            if (element.semantic.empty() || element.semantic.size() > 64 || element.semantic.find('\0') != std::string::npos ||
                element.slot >= GimiVertexSlots || element.offset == D3D11_APPEND_ALIGNED_ELEMENT)
                return bad("Unsupported layout semantic/slot/APPEND offset");
            element.semantic = LowerAscii(element.semantic);
            const auto bytes = FormatElementBytes(element.format);
            if (!bytes || std::uint64_t(element.offset) + bytes > request.strides[element.slot])
                return bad("Unsupported layout format or element exceeds exact vertex stride");
            contract->ends_[element.slot] = (std::max)(contract->ends_[element.slot], element.offset + bytes);
            contract->slots_ |= 1u << element.slot;
        }
        contract->request_ = std::move(request);
        return {std::move(contract), {}};
    }
};
LayoutResult CompileLayout(LayoutRequest request)
{
    try { return LayoutCompiler::Compile(std::move(request)); }
    catch (const std::exception& e) { return {nullptr, {{DiagnosticCode::Io, {}, e.what()}}}; }
}
QualifiedLayout LayoutContract::Qualify(std::uint64_t lease, std::uint64_t device,
    const D3D11_INPUT_ELEMENT_DESC* elements, std::uint32_t count, ShaderIdentity shader) const noexcept
{
    if (!lease || !device || !elements || count != request_.elements.size() ||
        shader.knowledge != Knowledge::Known || !shader.object || !shader.generation || shader.hash != request_.vertexShaderHash) return {};
    for (unsigned i = 0; i < count; ++i)
    {
        const auto& a = elements[i]; const auto& b = request_.elements[i];
        if (!SameSemantic(b.semantic, a.SemanticName) || a.SemanticIndex != b.semanticIndex || a.InputSlot != b.slot ||
            a.AlignedByteOffset != b.offset || a.Format != b.format ||
            a.InputSlotClass != D3D11_INPUT_PER_VERTEX_DATA || a.InstanceDataStepRate) return {};
    }
    return {Knowledge::Known, lease, request_.contract, device};
}
bool CompileIndexWindow(const ResourceIdentity& index, CpuImage cpu, DXGI_FORMAT format,
    std::uint32_t offset, std::uint32_t first, std::uint32_t count, IndexWindow& out) noexcept
{
    out = {};
    if (index.knowledge != Knowledge::Known || index.kind != IdentityKind::Buffer || !index.contentKnown ||
        (!index.hashKnown && index.origin != ResourceOrigin::Custom) || !index.object || !cpu.data || cpu.bytes < index.buffer.ByteWidth || !count ||
        (format != DXGI_FORMAT_R16_UINT && format != DXGI_FORMAT_R32_UINT)) return false;
    const auto element = FormatElementBytes(format);
    const auto begin = std::uint64_t(offset) + std::uint64_t(first) * element;
    const auto end = begin + std::uint64_t(count) * element;
    if (offset % element || end > index.buffer.ByteWidth || ResourceCrc32c(0, cpu.data, index.buffer.ByteWidth) != index.dataHash) return false;
    auto minimum = UINT32_MAX, maximum = 0u;
    const auto* bytes = static_cast<const std::uint8_t*>(cpu.data) + static_cast<std::size_t>(begin);
    for (std::uint32_t n = 0; n < count; ++n)
    {
        std::uint32_t value;
        if (element == 2) { std::uint16_t v; std::memcpy(&v, bytes, 2); value = v; }
        else std::memcpy(&value, bytes, 4);
        minimum = (std::min)(minimum, value); maximum = (std::max)(maximum, value);
        bytes += element;
    }
    out = {true, index.object, index.contentGeneration, offset, first, count, minimum, maximum, format};
    return true;
}
DescriptorMatches PrepareBufferDescriptorMatches(const Plan& plan, std::uint64_t generation,
                                                   const ResourceIdentity& resource) noexcept
{
    DescriptorMatches out;
    if (!generation || resource.knowledge != Knowledge::Known || resource.kind != IdentityKind::Buffer ||
        resource.origin != ResourceOrigin::Game || !resource.object || !resource.deviceGeneration ||
        !resource.descriptorGeneration) return out;
    out.planGeneration = generation; out.object = resource.object;
    out.deviceGeneration = resource.deviceGeneration; out.descriptorGeneration = resource.descriptorGeneration;
    const auto candidates = plan.BufferDescriptorCandidates();
    for (std::size_t i = 0; i < candidates.second; ++i)
    {
        const auto& rule = candidates.first[i];
        // Bare upstream index_buffer has an all-bits mask, not a contains test.
        if (resource.buffer.Usage != rule.usage || resource.buffer.BindFlags != rule.bindFlags) continue;
        if (out.count >= out.programs.size() || out.count >= plan.Limits().matchesPerCheck)
        { out.count = 0; out.overflow = true; out.ready = true; return out; }
        out.programs[out.count++] = rule.program;
    }
    out.ready = true;
    return out;
}
DescriptorMatches PrepareTextureDescriptorMatches(const Plan& plan, std::uint64_t generation,
    const ResourceIdentity& resource, OutputViewMetadata output) noexcept
{
    DescriptorMatches out;
    if (!generation || resource.knowledge != Knowledge::Known || resource.kind != IdentityKind::Texture2D ||
        resource.origin != ResourceOrigin::Game || !resource.object || !resource.deviceGeneration ||
        !resource.descriptorGeneration || !output.view || !output.generation || !output.sliceCount) return out;
    out.planGeneration = generation; out.object = resource.object;
    out.deviceGeneration = resource.deviceGeneration; out.descriptorGeneration = resource.descriptorGeneration;
    out.output = output;
    const auto candidates = plan.TextureDescriptorCandidates();
    for (std::size_t i = 0; i < candidates.second; ++i)
    {
        const auto& rule = candidates.first[i];
        // Whole resource descriptor, including ALL bind bits; NOT RTV/DSV Format.
        if (resource.texture.Usage != rule.usage || resource.texture.BindFlags != rule.bindFlags ||
            resource.texture.Format != rule.resourceFormat) continue;
        if (out.count >= out.programs.size() || out.count >= plan.Limits().matchesPerCheck)
        { out.count = 0; out.overflow = true; out.ready = true; return out; }
        out.programs[out.count++] = rule.program;
    }
    out.ready = true; return out;
}
struct DrawCache::Storage
{
    struct Entry { bool valid = false; CacheKey key; DrawMatches result; };
    std::shared_ptr<const Plan> plan;
    std::shared_ptr<const LayoutContract> layout;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    std::uint64_t generation;
    bool descriptorRules = false, textureDescriptorRules = false;
    std::vector<Entry> entries;
    CacheKey scratch;

    MatchStatus Qualify(const IndexedDrawSnapshot& d) const noexcept
    {
        if (!d.immediate || d.drawIndexed != d.draw.drawIndexed || d.internal || (!d.draw.instanced && (d.draw.instanceCount != 1 || d.draw.firstInstance)) ||
            (d.draw.instanced && !d.drawIndexed) ||
            d.topology != topology) return MatchStatus::Unsupported;
        if (d.epochs.plan != generation || !d.epochs.consumer || !d.epochs.device || !d.epochs.context ||
            d.layout.knowledge != Knowledge::Known || !d.layout.object || d.layout.contract != layout->IdValue() ||
            d.layout.deviceGeneration != d.epochs.device || d.vertexShader.knowledge != Knowledge::Known ||
            !d.vertexShader.object || !d.vertexShader.generation || d.vertexShader.hash != layout->VertexShaderHash()) return MatchStatus::Unknown;
        std::uint64_t last = 0;
        if (d.drawIndexed)
        {
            if (!KnownBuffer(d.indexIdentity, d.indexBinding, d.epochs.device)) return MatchStatus::Unknown;
            const auto& w = d.indices;
            if (!w.known || w.object != d.indexBinding.object || w.contentGeneration != d.indexIdentity.contentGeneration ||
                w.first != d.draw.firstIndex || w.count != d.draw.indexCount || w.offset != d.indexBinding.offset ||
                w.format != d.indexBinding.format) return MatchStatus::Unknown;
            const auto element = FormatElementBytes(w.format);
            if (!d.draw.indexCount || (w.format != DXGI_FORMAT_R16_UINT && w.format != DXGI_FORMAT_R32_UINT) ||
                d.indexBinding.offset % element || std::uint64_t(d.indexBinding.offset) +
                (std::uint64_t(d.draw.firstIndex) + d.draw.indexCount) * element > d.indexBinding.byteSize)
                return MatchStatus::Unsupported;
            const auto first = std::int64_t(w.minimum) + d.draw.baseVertex;
            const auto maximum = std::int64_t(w.maximum) + d.draw.baseVertex;
            if (first < 0 || maximum < first || maximum > UINT32_MAX) return MatchStatus::Unsupported;
            last = static_cast<std::uint64_t>(maximum);
        }
        else
        {
            if (!d.draw.vertexCount) return MatchStatus::Unsupported;
            last = std::uint64_t(d.draw.firstVertex) + d.draw.vertexCount - 1;
            if (last > UINT32_MAX) return MatchStatus::Unsupported;
        }
        for (unsigned slot = 0; slot < GimiVertexSlots; ++slot)
            if (layout->Slots() & (1u << slot))
            {
                const auto& b = d.vertices[slot];
                if (!KnownBuffer(d.vertexIdentities[slot], b, d.epochs.device)) return MatchStatus::Unknown;
                if (b.stride != layout->Strides()[slot] || std::uint64_t(b.offset) + std::uint64_t(last) * b.stride + layout->Ends()[slot] > b.byteSize)
                    return MatchStatus::Unsupported;
            }
        return MatchStatus::Matched;
    }
    DrawMatches Match(const IndexedDrawSnapshot& draw, const MatchSubject& subject, bool metadataQuery) const noexcept
    {
        DrawMatches result;
        // Metadata queries are not raster eligibility. The executor still
        // authenticates the current pipeline/getter and preflights every draw.
        // The public Lookup used by root selection retains full qualification.
        if (!metadataQuery)
        {
            result.status = Qualify(draw);
            if (result.status != MatchStatus::Matched) return result;
        }
        else if (!draw.immediate || draw.internal || draw.drawIndexed != draw.draw.drawIndexed ||
            draw.epochs.plan != generation || !draw.epochs.consumer || !draw.epochs.device || !draw.epochs.context)
            return result;
        const bool shader = subject.target.kind == TargetKind::VertexShader || subject.target.kind == TargetKind::PixelShader;
        std::uint64_t hash = 0;
        bool queryHash = true;
        const DescriptorMatches* descriptors = nullptr;
        if (shader)
        {
            if (subject.shader.knowledge == Knowledge::Null && !subject.binding.object && !subject.binding.view)
            {
                result.status = MatchStatus::NoMatch; result.filterKnown = true; result.filterIndex = -0.0f; return result;
            }
            if (subject.shader.knowledge != Knowledge::Known || !subject.shader.object ||
                subject.shader.object != subject.binding.object || subject.shader.generation != subject.binding.generation)
            { result.status = MatchStatus::Unknown; return result; }
            hash = subject.shader.hash;
        }
        else
        {
            if (subject.resource.knowledge == Knowledge::Null && !subject.binding.object && !subject.binding.view)
            {
                result.status = MatchStatus::NoMatch; result.filterKnown = true; result.filterIndex = -0.0f; return result;
            }
            const auto& r = subject.resource;
            if (r.knowledge == Knowledge::Known && r.origin == ResourceOrigin::Custom && r.object == subject.binding.object && r.object &&
                r.deviceGeneration == draw.epochs.device && subject.binding.generation == draw.epochs.device)
            {
                // Known bound custom MOD object, matching real XXMI's empty
                // candidates, not Unknown and not a fake hash=0 candidate.
                result.status = MatchStatus::NoMatch; result.filterKnown = true; result.filterIndex = 0.0f; return result;
            }
            if (r.knowledge != Knowledge::Known || r.origin != ResourceOrigin::Game || r.kind == IdentityKind::Unknown ||
                r.object != subject.binding.object || !r.object || !r.descriptorGeneration ||
                r.deviceGeneration != draw.epochs.device || subject.binding.generation != draw.epochs.device)
            { result.status = MatchStatus::Unknown; return result; }
            queryHash = r.hashKnown && (r.contentKnown || r.gpuOrdered);
            if (!queryHash && !metadataQuery) { result.status = MatchStatus::Unknown; return result; }
            const bool output = subject.target.kind == TargetKind::RenderTarget || subject.target.kind == TargetKind::DepthTarget;
            if ((r.kind == IdentityKind::Buffer && descriptorRules) || (output && textureDescriptorRules))
            {
                descriptors = subject.descriptors;
                if (!descriptors || !descriptors->ready || descriptors->planGeneration != generation ||
                    descriptors->object != r.object || descriptors->deviceGeneration != r.deviceGeneration ||
                    descriptors->descriptorGeneration != r.descriptorGeneration)
                { result.status = MatchStatus::Unknown; return result; }
                if (output && (r.kind != IdentityKind::Texture2D || !subject.binding.view ||
                    descriptors->output.view != subject.binding.view || !descriptors->output.generation ||
                    descriptors->output.format != static_cast<DXGI_FORMAT>(subject.binding.format) ||
                    descriptors->output.depth != (subject.target.kind == TargetKind::DepthTarget)))
                { result.status = MatchStatus::Unknown; return result; }
                if (descriptors->overflow || descriptors->count > GimiCachedMatches ||
                    descriptors->count > plan->Limits().matchesPerCheck)
                { result.status = MatchStatus::Limit; return result; }
            }
            constexpr UINT writes = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL;
            constexpr UINT shared = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            const auto binds = r.kind == IdentityKind::Buffer ? r.buffer.BindFlags : r.texture.BindFlags;
            const auto misc = r.kind == IdentityKind::Buffer ? r.buffer.MiscFlags : r.texture.MiscFlags;
            if (queryHash && ((binds & (r.gpuOrdered ? (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL) : writes)) || (misc & shared)))
            { result.status = MatchStatus::Unsupported; return result; }
            if (subject.target.kind == TargetKind::VertexBuffer || subject.target.kind == TargetKind::IndexBuffer)
            {
                if (r.kind != IdentityKind::Buffer || (subject.target.kind == TargetKind::VertexBuffer && subject.target.index >= 32) ||
                    (subject.target.kind == TargetKind::IndexBuffer && subject.target.index != 0))
                { result.status = MatchStatus::Unsupported; return result; }
            }
            else if (output)
            {
                if (r.kind != IdentityKind::Texture2D || !subject.binding.view ||
                    (subject.target.kind == TargetKind::RenderTarget ? subject.target.index >= 8 : subject.target.index != 0))
                { result.status = MatchStatus::Unsupported; return result; }
            }
            else if (((subject.target.kind == TargetKind::PixelTexture || subject.target.kind == TargetKind::VertexTexture) &&
                       (!subject.binding.view || subject.target.index >= 128)) ||
                (subject.target.kind != TargetKind::PixelTexture && subject.target.kind != TargetKind::VertexTexture &&
                 subject.target.kind != TargetKind::Resource))
            { result.status = MatchStatus::Unsupported; return result; }
            hash = r.upstreamHash;
        }
        if (queryHash && metadataQuery)
        {
            result.status = Qualify(draw);
            if (result.status != MatchStatus::Matched) return result;
        }
        const auto candidates = queryHash ? plan->Candidates(hash, shader) : std::pair<const OverrideSpec*, std::size_t>{nullptr, 0};
        const auto total = candidates.second + (descriptors ? descriptors->count : 0);
        if (total > plan->Limits().matchesPerCheck || total > GimiCachedMatches)
        { result.status = MatchStatus::Limit; return result; }
        result.filterKnown = true; result.filterIndex = 0.0f;
        bool hasFilter = false;
        for (std::size_t i = 0; i < candidates.second; ++i)
        {
            const auto& match = candidates.first[i];
            // An index-constrained override has no proven nonindexed query
            // contract in this slice. Never silently execute it as index zero.
            if (!draw.drawIndexed && (match.hasFirstIndex || match.hasIndexCount))
            { result.status = MatchStatus::Unsupported; result.count = 0; result.filterKnown = false; return result; }
            if (!match.MatchesDraw(draw.drawIndexed, draw.draw.firstIndex, draw.draw.indexCount)) continue;
            result.programs[result.count++] = match.program;
            if (match.hasFilterIndex) { result.filterIndex = match.filterIndex; hasFilter = true; }
        }
        // Do not globally sort the concatenation: hash matches precede the
        // descriptor subset even when a descriptor has priority -9999.
        for (std::uint32_t i = 0; descriptors && i < descriptors->count; ++i)
        {
            const auto* match = plan->OverrideForProgram(descriptors->programs[i]);
            if (!match || match->shader || match->matchKind != (subject.resource.kind == IdentityKind::Buffer ?
                    OverrideMatchKind::BufferDescriptor : OverrideMatchKind::Texture2DDescriptor) ||
                (i && match <= plan->OverrideForProgram(descriptors->programs[i - 1])))
            { result = {}; return result; }
            if (!match->MatchesDraw(draw.drawIndexed, draw.draw.firstIndex, draw.draw.indexCount)) continue;
            result.programs[result.count++] = match->program;
            if (match->hasFilterIndex) { result.filterIndex = match->filterIndex; hasFilter = true; }
        }
        result.status = result.count ? MatchStatus::Matched : MatchStatus::NoMatch;
        if (result.count && !hasFilter) result.filterIndex = 1.0f; // real process_texture_filter fallback
        return result;
    }
};
DrawCache::DrawCache(std::shared_ptr<const Plan> plan, std::shared_ptr<const LayoutContract> layout,
    D3D11_PRIMITIVE_TOPOLOGY topology, std::uint64_t generation, std::uint32_t slots)
    : state_(std::make_unique<Storage>())
{
    if (!plan || !layout || !generation || !slots || slots > 4096 || (slots & (slots - 1)) ||
        (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && topology != D3D11_PRIMITIVE_TOPOLOGY_POINTLIST) ||
        plan->Limits().matchesPerCheck > GimiCachedMatches)
        throw std::invalid_argument("Invalid exact Draw/DrawIndexed cache request");
    state_->plan = std::move(plan); state_->layout = std::move(layout);
    state_->descriptorRules = state_->plan->BufferDescriptorCandidates().second != 0;
    state_->textureDescriptorRules = state_->plan->TextureDescriptorCandidates().second != 0;
    for (std::size_t i = 0; i < state_->plan->Overrides().size(); ++i)
    {
        const auto& o = state_->plan->Overrides()[i];
        if (o.shader && i && state_->plan->Overrides()[i - 1].shader && state_->plan->Overrides()[i - 1].hash == o.hash)
            throw std::invalid_argument("ShaderOverride equal-hash merging unsupported; use CreateDrawCache for source diagnostic");
    }
    state_->topology = topology; state_->generation = generation; state_->entries.resize(slots);
}
DrawCache::~DrawCache() = default;
const DrawMatches& DrawCache::Lookup(bool enabled, const IndexedDrawSnapshot& draw, const MatchSubject& subject) noexcept
{
    return LookupQuery(enabled, draw, subject, false);
}
const DrawMatches& DrawCache::LookupQuery(bool enabled, const IndexedDrawSnapshot& draw,
                                        const MatchSubject& subject, bool metadataQuery) noexcept
{
    if (!enabled) return Disabled;
    auto& s = *state_;
    KeyOf(s.scratch, draw, subject, s.layout->Slots());
    s.scratch.Add(metadataQuery);
    const auto& key = s.scratch;
    // Numeric bucket selection only. Collision always compares the entire key.
    const auto first = draw.drawIndexed ? draw.draw.firstIndex : draw.draw.firstVertex;
    const auto count = draw.drawIndexed ? draw.draw.indexCount : draw.draw.vertexCount;
    auto bucket = subject.binding.object ^ subject.shader.hash ^ draw.indexBinding.object ^
        (std::uint64_t(first) << 11) ^ count ^ draw.layout.object ^ (std::uint64_t(draw.drawIndexed) << 63);
    bucket ^= bucket >> 33; bucket *= 0xff51afd7ed558ccdULL; bucket ^= bucket >> 33;
    auto& entry = s.entries[static_cast<std::size_t>(bucket) & (s.entries.size() - 1)];
    if (entry.valid && entry.key == key) return entry.result;
    entry.key.count = key.count;
    std::copy_n(key.words.data(), key.count, entry.key.words.data());
    entry.result = s.Match(draw, subject, metadataQuery); entry.valid = true;
    return entry.result;
}
bool DrawCache::CopyTextureMatches(bool enabled, const IndexedDrawSnapshot& draw, const MatchSubject& subject,
    Id* output, std::uint32_t capacity, std::uint32_t& count) noexcept
{
    count = 0;
    if (!enabled) return false;
    if (subject.target.kind == TargetKind::VertexShader || subject.target.kind == TargetKind::PixelShader) return false;
    const auto& result = LookupQuery(enabled, draw, subject, true);
    if ((result.status != MatchStatus::Matched && result.status != MatchStatus::NoMatch) ||
        result.count > capacity || (result.count && !output)) return false;
    if (result.count) std::copy_n(result.programs.data(), result.count, output);
    count = result.count; return true;
}
bool DrawCache::Filter(bool enabled, const IndexedDrawSnapshot& draw, const MatchSubject& subject, float& out) noexcept
{
    const auto& result = LookupQuery(enabled, draw, subject, true);
    if (!result.filterKnown) return false;
    out = result.filterIndex; return true;
}
DrawCacheResult CreateDrawCache(std::shared_ptr<const Plan> plan, std::shared_ptr<const LayoutContract> layout,
    D3D11_PRIMITIVE_TOPOLOGY topology, std::uint64_t generation, std::uint32_t slots)
{
    if (!plan) return {nullptr, {{DiagnosticCode::InvalidSyntax, {}, "Plan required"}}};
    for (std::size_t i = 0; i < plan->Overrides().size(); ++i)
    {
        const auto& o = plan->Overrides()[i];
        // overrule is upstream duplicate-warning policy; with a unique hash it
        // changes no execution/filter semantics. Actual duplicates still need
        // upstream ordered metadata/command-list merging and stay Unsupported.
        if (o.shader && i && plan->Overrides()[i - 1].shader && plan->Overrides()[i - 1].hash == o.hash)
            return {nullptr, {{DiagnosticCode::Unsupported, o.source, "ShaderOverride equal-hash merging unsupported in the exact-resource slice"}}};
    }
    try { return {std::make_unique<DrawCache>(plan, std::move(layout), topology, generation, slots), {}}; }
    catch (const std::exception& e)
    {
        const auto source = plan->Entries().empty() ? SourceLocation{} : plan->Programs()[plan->Entries().front()].source;
        return {nullptr, {{DiagnosticCode::Unsupported, source, e.what()}}};
    }
}
bool DrawQueryAdapter::Subject(const Plan& plan, const MatchQuery& query, MatchSubject& out) const noexcept
{
    if (!enabled_ || &plan != plan_ || !query.draw || query.draw->firstIndex != draw_->draw.firstIndex ||
        query.draw->indexCount != draw_->draw.indexCount || query.draw->baseVertex != draw_->draw.baseVertex ||
        query.draw->drawIndexed != draw_->draw.drawIndexed || query.draw->instanced != draw_->draw.instanced || query.draw->firstVertex != draw_->draw.firstVertex ||
        query.draw->vertexCount != draw_->draw.vertexCount ||
        query.draw->firstInstance != draw_->draw.firstInstance || query.draw->instanceCount != draw_->draw.instanceCount)
        return false;
    out = {}; out.target = query.target; out.binding = query.binding;
    const bool shader = query.target.kind == TargetKind::VertexShader || query.target.kind == TargetKind::PixelShader;
    if (!query.binding.object && !query.binding.view)
    {
        out.resource = ResourceIdentity::KnownNull(); out.shader.knowledge = Knowledge::Null; return true;
    }
    if (shader) return queries_.shader && queries_.shader(queries_.owner, query.target, query.binding, out.shader);
    if (!queries_.resource || !queries_.resource(queries_.owner, query.binding, out.resource)) return false;
    if (queries_.descriptorMatches) out.descriptors = queries_.descriptorMatches(queries_.owner, query.binding);
    return true;
}
bool DrawQueryAdapter::Read(void* context, Target target, Binding& binding) noexcept
{
    const auto& self = *static_cast<DrawQueryAdapter*>(context);
    return self.enabled_ && self.queries_.readBinding && self.queries_.readBinding(self.queries_.owner, target, binding);
}
bool DrawQueryAdapter::FilterQuery(void* context, const Plan& plan, const MatchQuery& query, float& value) noexcept
{
    auto& self = *static_cast<DrawQueryAdapter*>(context);
    MatchSubject subject;
    if (!self.Subject(plan, query, subject)) return false;
    if ((query.target.kind == TargetKind::VertexShader || query.target.kind == TargetKind::PixelShader) &&
        self.queries_.shaderFilter)
        return self.queries_.shaderFilter(self.queries_.owner, query.target, query.binding, value);
    return self.cache_->Filter(self.enabled_, *self.draw_, subject, value);
}
bool DrawQueryAdapter::TextureQuery(void* context, const Plan& plan, const MatchQuery& query, Id* output,
    std::uint32_t capacity, std::uint32_t& count) noexcept
{
    auto& self = *static_cast<DrawQueryAdapter*>(context);
    count = 0;
    MatchSubject subject;
    return self.Subject(plan, query, subject) &&
        self.cache_->CopyTextureMatches(self.enabled_, *self.draw_, subject, output, capacity, count);
}
bool DrawQueryAdapter::Constant(void* raw, Id slot, Id scalar, float& value) noexcept
{
    const auto& q = *static_cast<DrawQueryAdapter*>(raw);
    return q.enabled_ && q.queries_.readPixelConstant &&
        q.queries_.readPixelConstant(q.queries_.owner, slot, scalar, value);
}
bool DrawQueryAdapter::Native(void* raw, NativeValue operand, Binding binding, float& value) noexcept
{
    const auto& q = *static_cast<DrawQueryAdapter*>(raw);
    return q.enabled_ && q.queries_.nativeValue &&
        q.queries_.nativeValue(q.queries_.owner, operand, binding, value);
}
CommandQueries DrawQueryAdapter::Commands() noexcept
{
    CommandQueries out{this, &Read, &FilterQuery, nullptr, &TextureQuery};
    out.readPixelConstant = &Constant; out.nativeValue = &Native; return out;
}
Diagnostic DescribeMatchFailure(const Plan& plan, Id selected, MatchStatus status)
{
    const auto source = selected < plan.Programs().size() ? plan.Programs()[selected].source : SourceLocation{};
    switch (status)
    {
    case MatchStatus::Unknown: return {DiagnosticCode::Unsupported, source, "Selected component requires known CPU-content identity, exact qualified layout and precompiled index range; current dependency is Unknown"};
    case MatchStatus::Unsupported: return {DiagnosticCode::Unsupported, source, "Selected component is outside the supported immediate, stable, bounded triangle/point-list Draw/DrawIndexed slice"};
    case MatchStatus::Limit: return {DiagnosticCode::Unsupported, source, "Selected component exceeds the bounded match range; no truncated/partial execution is available"};
    default: return {DiagnosticCode::InvalidSyntax, source, "DescribeMatchFailure requires an actual Unknown/Unsupported/Limit result"};
    }
}
} // namespace gimi
