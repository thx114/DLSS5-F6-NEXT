#include "GimiResources.h"
#include "GimiDds.h"
#include "GimiExpression.h"
#include <fstream>
#include <limits>
#include <stdexcept>
#include <algorithm>
#include <sstream>
#include <locale>
#include <cstring>

namespace gimi
{
namespace
{
struct ResourceFailure { Diagnostic diagnostic; HRESULT hr = S_OK; };
[[noreturn]] void Fail(const SourceLocation& source, std::string message,
                       DiagnosticCode code = DiagnosticCode::Unsupported, HRESULT hr = S_OK)
{
    throw ResourceFailure{{code, source, std::move(message)}, hr};
}
SourceLocation FieldSource(const Plan& plan, const ResourceSpec& resource, std::string_view field)
{
    for (const auto& section : plan.Configuration()->sections)
        if (section.canonicalName == resource.name)
            for (const auto& entry : section.entries) if (entry.key == field) return entry.source;
    return resource.source;
}
// Only typed formats whose actual sampled return belongs to the float
// family. Integer, depth, typeless, planar and unknown formats stay refused.
// The actual device must additionally advertise SHADER_SAMPLE below.
bool FloatTextureSampleFormat(DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
    case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
        return true;
    default: return false;
    }
}
// Output aliases need an owning RTV/DSV at execution, not an immutable SRV.
// Keep this use separate and propagate it through every alias predecessor; a
// file/inline resource must never acquire OM permission via an alias chain.
struct Usage { bool vertex = false, index = false, texture = false, dynamic = false, output = false; };
bool Merge(Usage& a, Usage b) noexcept
{
    const auto old = a;
    a.vertex |= b.vertex; a.index |= b.index; a.texture |= b.texture; a.dynamic |= b.dynamic; a.output |= b.output;
    return old.vertex != a.vertex || old.index != a.index || old.texture != a.texture ||
        old.dynamic != a.dynamic || old.output != a.output;
}
std::vector<Usage> ResourceUsage(const Plan& plan)
{
    std::vector<Usage> usage(plan.Resources().size());
    std::vector<std::pair<Id, Id>> edges;
    // Only the final startup binding feeds runtime uses. Earlier snapshots
    // already carry their canonical source, including alias-to-alias copies.
    std::vector<Id> initialized(plan.Resources().size(), InvalidId);
    for (const auto& initial : plan.InitialResources())
    {
        if (initial.destination >= initialized.size() ||
            (initial.resource != InvalidId && initial.resource >= initialized.size()))
            Fail(initial.source, "Invalid static resource initializer", DiagnosticCode::InvalidSyntax);
        initialized[initial.destination] = initial.resource;
    }
    for (Id id = 0; id < initialized.size(); ++id)
        if (initialized[id] != InvalidId) edges.emplace_back(initialized[id], id);
    for (const auto& format : plan.FormatTexts())
        for (const auto id : {format.font, format.text, format.parameters, format.positions})
        {
            if (id >= usage.size()) Fail(format.source, "Invalid FormatText resource");
            usage[id].texture = true;
        }
    for (const auto& program : plan.Programs())
        for (const auto& list : program.commands)
            for (const auto& command : list)
            {
                if (command.op != CommandOp::Reference || command.sourceTarget.kind != TargetKind::Resource) continue;
                const Id id = command.sourceTarget.index;
                if (id >= usage.size()) Fail(command.source, "Invalid compiled resource reference", DiagnosticCode::InvalidSyntax);
                switch (command.destination.kind)
                {
                case TargetKind::Resource: edges.emplace_back(id, command.destination.index); break;
                case TargetKind::VertexBuffer: usage[id].vertex = true; break;
                case TargetKind::IndexBuffer: usage[id].index = true; break;
                case TargetKind::PixelTexture: case TargetKind::VertexTexture: usage[id].texture = true; break;
                case TargetKind::This: usage[id].dynamic = true; break;
                case TargetKind::RenderTarget: case TargetKind::DepthTarget:
                    usage[id].output = true; break;
                default: Fail(command.source, "Unsupported static resource destination");
                }
            }
    // Fixed-point propagation is Load-only and has at most five bits per node.
    bool changed;
    do
    {
        changed = false;
        for (const auto edge : edges)
        {
            if (edge.second >= usage.size()) Fail({}, "Invalid compiled alias destination", DiagnosticCode::InvalidSyntax);
            changed |= Merge(usage[edge.first], usage[edge.second]);
        }
    } while (changed);
    return usage;
}
// A conservative finite-set analysis of ONLY the assignments feeding GPU
// parameters. Branches/calls are overapproximated, never used to drop a write.
// Runtime still evaluates the original ordered IR; these sets authorise pages,
// not cached branches, aliases or action tapes.
struct ParameterDomain
{
    std::vector<std::uint32_t> bits;
    bool unknown = false;
};
constexpr std::size_t ParameterStateLimit = 512;
constexpr std::size_t ParameterProofBudget = 4u * 1024u * 1024u;
struct ParameterFilters
{
    ParameterDomain shader, resource, any;
    const ParameterDomain& For(Target target) const noexcept
    {
        if (target.kind == TargetKind::VertexShader || target.kind == TargetKind::PixelShader) return shader;
        // "this" inherits the caller's binding kind; do not guess its stage.
        return target.kind == TargetKind::This ? any : resource;
    }
};
bool AddValue(ParameterDomain& domain, std::uint32_t bits, const SourceLocation& at)
{
    if (std::find(domain.bits.begin(), domain.bits.end(), bits) != domain.bits.end()) return false;
    if (domain.bits.size() == ParameterStateLimit)
        Fail(at, "IniParams finite value set exceeds 512; no draw-time upload fallback", DiagnosticCode::Limit);
    domain.bits.push_back(bits); return true;
}
bool MergeDomain(ParameterDomain& to, const ParameterDomain& from, const SourceLocation& at)
{
    // Unknown is the top of the value lattice. Do not keep enumerating an
    // unbounded recurrence, or retain creation/default bits as its authority.
    if (to.unknown) return false;
    if (from.unknown) { to.unknown = true; to.bits.clear(); return true; }
    bool changed = false;
    for (const auto bits : from.bits) changed |= AddValue(to, bits, at);
    return changed;
}
ParameterDomain ParameterExpression(const Expression& expression, const std::vector<ParameterDomain>& variables,
                                    const std::vector<ParameterDomain>& parameters, const IniParameterSpec& spec,
                                    const ParameterFilters& filters, std::size_t& budget)
{
    std::vector<ParameterDomain> stack;
    stack.reserve(expression.stack);
    const auto tick = [&] {
        if (!budget) Fail(expression.source, "IniParams finite proof budget exhausted", DiagnosticCode::Limit);
        --budget;
    };
    for (const auto& token : expression.code)
    {
        tick();
        ParameterDomain value;
        switch (token.op)
        {
        case ExprOp::Literal: value.bits.push_back(IniParameterBits(token.literal)); break;
        case ExprOp::Variable:
            if (token.index >= variables.size()) Fail(expression.source, "Invalid IniParams variable", DiagnosticCode::InvalidSyntax);
            value = variables[token.index]; break;
        case ExprOp::Parameter:
            if (token.index >= parameters.size()) Fail(expression.source, "Invalid IniParams scalar", DiagnosticCode::InvalidSyntax);
            value = parameters[token.index];
            if (value.bits.empty() && !value.unknown) value.bits.push_back(spec.defaults[token.index]);
            break;
        case ExprOp::Filter: value = filters.For(token.target); break;
        // Authentic indexed-instanced callers carry arbitrary instance data.
        // Keep it unknown here; comparisons may still prove a finite result,
        // but a guessed noninstanced 1/0 must never authorize a reader page.
        case ExprOp::InstanceCount: case ExprOp::FirstInstance:
            value.unknown = true; break;
        case ExprOp::DrawType:
            // Exact finite union of the supported authentic physical callers.
            // The runtime still refuses Unknown/inconsistent/unsupported types.
            for (const auto call : {PhysicalDrawCall::Draw, PhysicalDrawCall::DrawIndexed, PhysicalDrawCall::DrawIndexedInstanced})
                value.bits.push_back(IniParameterBits(static_cast<float>(call)));
            break;
        case ExprOp::NativeValue: // unbounded live values cannot authorize immutable parameter pages
        case ExprOp::FirstIndex: case ExprOp::IndexCount: case ExprOp::VertexCount: case ExprOp::FirstVertex:
            value.unknown = true; break;
        default:
            if (stack.empty()) Fail(expression.source, "Invalid IniParams expression", DiagnosticCode::InvalidSyntax);
            if (token.op == ExprOp::Negate || token.op == ExprOp::Positive || token.op == ExprOp::Not)
            {
                value.unknown = stack.back().unknown;
                if (value.unknown && token.op == ExprOp::Not)
                { value.unknown = false; value.bits = {0, IniParameterBits(1.0f)}; }
                for (const auto bits : stack.back().bits)
                {
                    tick(); auto v = IniParameterFloat(bits);
                    if (!detail::Unary(token.op, v)) Fail(expression.source, "Invalid IniParams unary operator");
                    AddValue(value, IniParameterBits(v), expression.source);
                }
                stack.pop_back();
            }
            else
            {
                auto rhs = std::move(stack.back()); stack.pop_back();
                if (stack.empty()) Fail(expression.source, "Invalid IniParams binary expression", DiagnosticCode::InvalidSyntax);
                value.unknown = stack.back().unknown || rhs.unknown;
                // Comparison/logical results have an exact finite superset
                // even when their real CPU input is continuously varying.
                if (value.unknown && token.op >= ExprOp::Equal && token.op <= ExprOp::Or)
                { value.unknown = false; value.bits = {0, IniParameterBits(1.0f)}; }
                for (const auto left : stack.back().bits) for (const auto right : rhs.bits)
                {
                    tick(); auto v = IniParameterFloat(left);
                    if (!detail::Binary(token.op, v, IniParameterFloat(right))) Fail(expression.source, "Invalid IniParams binary operator");
                    AddValue(value, IniParameterBits(v), expression.source);
                }
                stack.pop_back();
            }
            break;
        }
        stack.push_back(std::move(value));
    }
    if (stack.size() != 1) Fail(expression.source, "Invalid IniParams expression result", DiagnosticCode::InvalidSyntax);
    return std::move(stack.back());
}
IniParameterSpec ParameterUsage(const Plan& plan)
{
    IniParameterSpec spec;
    if (!plan.Touches().iniParameters) return spec;
    spec.required = true;
    for (const auto& initial : plan.InitialParameters())
    {
        if (initial.scalar >= IniParameterScalars) Fail(initial.source, "Invalid initial IniParams scalar", DiagnosticCode::InvalidSyntax);
        spec.defaults[initial.scalar] = IniParameterBits(initial.value);
        if (spec.source.file.empty()) spec.source = initial.source;
    }
    std::vector<bool> needed(plan.Variables().size()), written(IniParameterScalars), rasterWritten(IniParameterScalars);
    std::vector<const Command*> assignments, stores;
    std::vector<const Command*> nativeParameterReaders;
    for (Id programId = 0; programId < plan.Programs().size(); ++programId)
    for (const auto& phase : plan.Programs()[programId].commands) for (const auto& command : phase)
    {
        const bool native = plan.NativeOnly(programId);
        if (command.op == CommandOp::StorePixelConstant) stores.push_back(&command);
        // A real Present packet supplies a bounded dynamic snapshot and requires
        // restoration. These writes never authorize an immutable DRAW page.
        const bool privatePost = plan.Touches().clearTargets && &phase == &plan.Programs()[programId].commands[1];
        if (native && command.op == CommandOp::SetParameter && !privatePost && command.index < 8)
        {
            // Native text/Mouse coordinates stay scoped packet snapshots.
            // Overlay z=0/w=1 also needs immutable reader images; non-window
            // finite scalars are handled below for both native and raster.
            // No guessed default or dynamic native local restore enters here.
            const auto* expression = command.expressions[0] < plan.Expressions().size() ?
                &plan.Expressions()[command.expressions[0]] : nullptr;
            const auto* literal = expression && expression->code.size() == 1 && expression->code[0].op == ExprOp::Literal ?
                &expression->code[0] : nullptr;
            const bool overlayLiteral = plan.Touches().backbufferViewport && literal &&
                ((command.index == 2 && literal->literal == 0) || (command.index == 3 && literal->literal == 1) ||
                 (command.index == 70 * 4 && (literal->literal == 1 || literal->literal == 2)));
            if (!overlayLiteral) continue;
        }
        if (native && command.op == CommandOp::Assign && command.expressions[0] < plan.Expressions().size())
            for (const auto& token : plan.Expressions()[command.expressions[0]].code)
                if (token.op == ExprOp::Parameter && token.index < 8)
                { nativeParameterReaders.push_back(&command); break; }
        if (command.op == CommandOp::SetParameter)
        {
            if (command.index >= written.size()) Fail(command.source, "Invalid IniParams write", DiagnosticCode::InvalidSyntax);
            if (spec.source.file.empty()) spec.source = command.source;
            written[command.index] = true;
            rasterWritten[command.index] = rasterWritten[command.index] || !native || privatePost || command.index >= 8;
        }
        if (command.op == CommandOp::SetParameter || command.op == CommandOp::Assign) assignments.push_back(&command);
        // t120 belongs to the private parameter resource for the entire tape.
        // Resource aliases of that slot would require another ownership model.
        for (const auto target : {command.destination, command.sourceTarget})
            if ((target.kind == TargetKind::PixelTexture || target.kind == TargetKind::VertexTexture) && target.index == IniParameterRegister)
                Fail(command.source, "IniParams t120 conflicts with an explicit resource command");
    }
    for (const auto& expression : plan.Expressions()) for (const auto& token : expression.code)
    {
        if (token.op == ExprOp::Parameter && spec.source.file.empty()) spec.source = expression.source;
        if (token.op == ExprOp::Filter && (token.target.kind == TargetKind::PixelTexture ||
            token.target.kind == TargetKind::VertexTexture) && token.target.index == IniParameterRegister)
            Fail(expression.source, "IniParams t120 cannot also be a game-resource filter");
    }
    std::size_t budget = ParameterProofBudget;
    bool changed;
    do
    {
        changed = false;
        for (const auto* command : assignments)
        {
            if (!budget) Fail(command->source, "IniParams dependency proof budget exhausted", DiagnosticCode::Limit);
            --budget;
            if (command->op == CommandOp::Assign && (command->index >= needed.size() || !needed[command->index])) continue;
            if (command->expressions[0] >= plan.Expressions().size()) Fail(command->source, "Invalid IniParams assignment", DiagnosticCode::InvalidSyntax);
            for (const auto& token : plan.Expressions()[command->expressions[0]].code) if (token.op == ExprOp::Variable)
            {
                if (token.index >= needed.size()) Fail(command->source, "Invalid IniParams dependency", DiagnosticCode::InvalidSyntax);
                if (!needed[token.index]) { needed[token.index] = true; changed = true; }
            }
        }
    } while (changed);
    assignments.erase(std::remove_if(assignments.begin(), assignments.end(), [&](const Command* command) {
        return command->op == CommandOp::Assign && (command->index >= needed.size() || !needed[command->index]);
    }), assignments.end());
    std::vector<ParameterDomain> variables(needed.size()), parameters(IniParameterScalars);
    for (Id id = 0; id < variables.size(); ++id) if (needed[id])
    {
        // A persisted float has an open input domain, like a CPU store. It may
        // still feed a finite comparison; it cannot supply a guessed page value.
        if (plan.Variables()[id].persistent) variables[id].unknown = true;
        else variables[id].bits.push_back(IniParameterBits(plan.Variables()[id].initial));
    }
    // Key cycles are semantic writers outside the command IR. Include every
    // declared value before the assignment fixed point, so a later F6/F12 edge
    // cannot select a value absent from the immutable pages. Conditions never
    // remove values from the proof. Persistent input remains an open domain.
    for (const auto& cycle : plan.KeyCycles())
    {
        if (cycle.variable >= variables.size())
            Fail(cycle.source, "Invalid IniParams key-cycle variable", DiagnosticCode::InvalidSyntax);
        auto& domain = variables[cycle.variable];
        if (needed[cycle.variable] && !domain.unknown)
            for (const auto value : cycle.values) AddValue(domain, IniParameterBits(value), cycle.source);
    }
    for (const auto* store : stores)
    {
        if (store->index >= variables.size()) Fail(store->source, "Invalid store variable", DiagnosticCode::InvalidSyntax);
        if (needed[store->index]) { variables[store->index].unknown = true; variables[store->index].bits.clear(); }
    }
    for (Id scalar = 0; scalar < written.size(); ++scalar) if (written[scalar])
        parameters[scalar].bits.push_back(spec.defaults[scalar]);
    ParameterFilters filters;
    filters.shader.bits = filters.resource.bits = filters.any.bits = {0, 0x80000000u, IniParameterBits(1.0f)};
    for (const auto& match : plan.Overrides()) if (match.hasFilterIndex)
    {
        AddValue(match.shader ? filters.shader : filters.resource, IniParameterBits(match.filterIndex), match.source);
        AddValue(filters.any, IniParameterBits(match.filterIndex), match.source);
    }
    // A resource classification cannot be returned by a real ps/vs query.
    // Hash-only shader overrides remain eligible for BOTH stages: no stage is
    // inferred from a section name. Prepared regex classifications are shaders.
    for (const auto& group : plan.ShaderRegexGroups()) if (group.hasFilterIndex)
    {
        AddValue(filters.shader, IniParameterBits(group.filterIndex), group.source);
        AddValue(filters.any, IniParameterBits(group.filterIndex), group.source);
    }
    do
    {
        changed = false;
        for (const auto* command : assignments)
        {
            if (command->op == CommandOp::Assign && (command->index >= needed.size() || !needed[command->index])) continue;
            const auto& expression = plan.Expressions()[command->expressions[0]];
            auto domain = ParameterExpression(expression, variables, parameters, spec, filters, budget);
            // Only x/y/z/w and the Mouse vector can carry a dynamic native
            // snapshot. A saved x70 is an exact finite page value, not a live
            // window coordinate; preserve its proved domain across restore.
            if (std::find(nativeParameterReaders.begin(), nativeParameterReaders.end(), command) != nativeParameterReaders.end())
                domain.unknown = true;
            // Allow unbounded intermediate variables; only actual page writes
            // must converge to a finite set. Branch-only health comparisons
            // choose among the original writes and never upload a dynamic page.
            auto& to = command->op == CommandOp::SetParameter ? parameters[command->index] : variables[command->index];
            changed |= MergeDomain(to, domain, expression.source);
        }
    } while (changed);
    for (const auto* command : assignments) if (command->op == CommandOp::SetParameter && parameters[command->index].unknown)
        Fail(command->source, "IniParams depends on an unbounded CPU store/live value; immutable pages require a finite result (for example a comparison), no draw-time upload fallback");
    // Keep the existing scalar ABI and all written IDs, including singleton
    // domains. A page is now an exact complete-image key, not a radix product
    // that unnecessarily crosses native overlay w=1 with every raster x70.
    // The raster cube still includes EVERY intermediate write and restore.
    using Image = std::vector<std::uint32_t>;
    // Reader values come from the proved original writes/defaults, never from
    // a synthetic pass. In particular both signs of z=0 stay distinct keys.
    const auto readerBits = [&](Id scalar) {
        auto bits = parameters[scalar].bits;
        if (bits.empty()) bits.push_back(spec.defaults[scalar]);
        return bits;
    };
    const auto overlayZ = readerBits(2), overlayW = readerBits(3), overlayPass = readerBits(70 * 4);
    std::vector<ParameterDomain> raster;
    Image initial;
    spec.slots.reserve(static_cast<std::size_t>(std::count(written.begin(), written.end(), true)));
    for (Id scalar = 0; scalar < written.size(); ++scalar) if (written[scalar])
    {
        spec.slots.push_back({scalar, 0, {}}); // stride zero: exact-key state column
        raster.push_back(rasterWritten[scalar] ? std::move(parameters[scalar]) : ParameterDomain{{spec.defaults[scalar]}, false});
        initial.push_back(spec.defaults[scalar]);
    }
    const auto tick = [&](std::size_t count = 1) {
        if (count > budget) Fail(spec.source, "IniParams finite page proof budget exhausted", DiagnosticCode::Limit);
        budget -= count;
    };
    const auto less = [&](const Image& a, const Image& b) {
        for (std::size_t n = 0; n < a.size(); ++n)
        {
            tick();
            if (a[n] != b[n]) return a[n] < b[n];
        }
        return false;
    };
    std::vector<Image> images;
    const auto add = [&](const Image& image) {
        const auto found = std::lower_bound(images.begin(), images.end(), image, less);
        tick(image.size());
        if (found != images.end() && *found == image) return;
        if (images.size() == IniParameterPageLimit)
            Fail(spec.source, "IniParams complete immutable image union exceeds its bounded page capacity", DiagnosticCode::Limit);
        images.insert(found, image);
    };
    const auto enumerate = [&](bool overlay, float pass, std::uint32_t zero) {
        std::vector<std::size_t> positions(raster.size());
        Image image(raster.size());
        // Fixed original native reader contract: z=0, w=1, x70=1/2.
        // Native text x/y/w remain a bounded packet snapshot and must be
        // restored before this reader; the runtime checks the ENTIRE image.
        const auto fixed = [&](Id scalar) {
            return overlay && (scalar == 2 || scalar == 3 || scalar == 70 * 4);
        };
        // An unwritten scalar is exactly its ordered static default. Do not
        // invent a write/slot for a pass that cannot match that full image.
        if (overlay) for (const Id scalar : {Id(2), Id(3), Id(70 * 4)})
            if (!written[scalar] && spec.defaults[scalar] != (scalar == 2 ? zero : IniParameterBits(scalar == 3 ? 1.0f : pass))) return;
        std::size_t product = 1;
        for (std::size_t n = 0; n < raster.size(); ++n)
        {
            tick();
            const auto count = fixed(spec.slots[n].scalar) ? 1 : raster[n].bits.size();
            if (!count || count > IniParameterPageLimit / product)
                Fail(spec.source, "IniParams complete immutable reader product exceeds its bounded page capacity at scalar " +
                    std::to_string(spec.slots[n].scalar) + " (prefix=" + std::to_string(product) +
                    ", domain=" + std::to_string(count) + ")", DiagnosticCode::Limit);
            product *= count;
        }
        for (std::size_t state = 0; state < product; ++state)
        {
            for (std::size_t n = 0; n < raster.size(); ++n)
            {
                tick();
                const auto scalar = spec.slots[n].scalar;
                image[n] = fixed(scalar) ? (scalar == 2 ? zero : IniParameterBits(scalar == 3 ? 1.0f : pass)) :
                    raster[n].bits[positions[n]];
            }
            add(image);
            for (std::size_t n = positions.size(); n-- > 0;)
            {
                tick();
                if (fixed(spec.slots[n].scalar)) continue;
                if (++positions[n] < raster[n].bits.size()) break;
                positions[n] = 0;
            }
        }
    };
    enumerate(false, 0, 0);
    if (plan.Touches().backbufferViewport && !plan.DebugOverlays().empty())
    {
        // These are the only immutable native readers admitted by the real
        // overlay. Their projection varies ALL remaining raster scalars; no
        // branch, action, default or unrelated parameter is dropped/cached.
        // Intersect with the actual proved values and the real reader
        // contract, rather than authorizing a literal absent from the graph.
        if (std::find(overlayW.begin(), overlayW.end(), IniParameterBits(1.0f)) != overlayW.end())
            for (const auto zero : overlayZ) if (IniParameterFloat(zero) == 0.0f)
                for (const float pass : {1.0f, 2.0f})
                {
                    tick();
                    if (std::find(overlayPass.begin(), overlayPass.end(), IniParameterBits(pass)) != overlayPass.end())
                        enumerate(true, pass, zero);
                }
    }
    const auto found = std::lower_bound(images.begin(), images.end(), initial, less);
    tick(initial.size());
    if (found == images.end() || *found != initial)
        Fail(spec.source, "IniParams initial image is absent from the complete closure");
    spec.initial = static_cast<std::uint32_t>(found - images.begin());
    spec.states = static_cast<std::uint32_t>(images.size());
    for (std::size_t n = 0; n < spec.slots.size(); ++n)
    {
        auto& values = spec.slots[n].values; values.reserve(images.size());
        for (const auto& image : images) { tick(); values.push_back(image[n]); }
    }
    return spec;
}
std::vector<std::uint8_t> ReadFile(const ResourceSpec& spec, const SourceLocation& source,
                                    PreloadLimits limits, std::uint64_t& total)
{
    std::ifstream input(spec.filename, std::ios::binary | std::ios::ate);
    if (!input) Fail(source, "Cannot open resource: " + PathUtf8(spec.filename), DiagnosticCode::Io);
    const auto end = input.tellg();
    if (end <= 0) Fail(source, "Empty or unreadable resource", DiagnosticCode::Io);
    const auto size = static_cast<std::uint64_t>(end);
    if (size > limits.perFileBytes || total > limits.totalBytes || size > limits.totalBytes - total ||
        size > static_cast<std::uint64_t>((std::numeric_limits<std::streamsize>::max)()) || size > SIZE_MAX)
        Fail(source, "Resource preload byte budget exceeded", DiagnosticCode::Limit);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)) || input.peek() != std::char_traits<char>::eof())
        Fail(source, "Resource changed or incomplete read during preload", DiagnosticCode::Io);
    total += size;
    return bytes;
}
} // namespace
struct ResourceFileLoader
{
    static std::shared_ptr<const ResourceFiles> Load(std::shared_ptr<const Plan> plan, PreloadLimits limits)
    {
        if (!plan || !limits.perFileBytes || !limits.totalBytes) Fail({}, "Plan and nonzero resource budgets required", DiagnosticCode::InvalidSyntax);
        const auto usage = ResourceUsage(*plan);
        auto files = std::make_shared<ResourceFiles>();
        files->plan_ = plan;
        files->parameters_ = ParameterUsage(*plan);
        files->resources_.resize(plan->Resources().size());
        const auto& parameters = files->parameters_;
        std::uint64_t total = std::uint64_t(parameters.states) * IniParameterScalars * sizeof(float);
        total += std::uint64_t(plan->InitialResources().size()) * sizeof(ResourceInitializer);
        // Charge the exact-key table as well as all immutable GPU pages to
        // the existing preload budget. More singleton writes add metadata,
        // never extra pages or an unbudgeted scalar-count escape hatch.
        if (parameters.required)
        {
            total += sizeof(IniParameterSpec) + parameters.slots.size() * sizeof(IniParameterSlot);
            for (const auto& slot : parameters.slots) total += slot.values.size() * sizeof(std::uint32_t);
        }
        const auto& budgetSource = parameters.source.file.empty() && !plan->InitialResources().empty() ?
            plan->InitialResources().front().source : parameters.source;
        if (total > limits.totalBytes) Fail(budgetSource, "Static initialization/IniParams pages/key table exceed resource preload budget", DiagnosticCode::Limit);
        for (Id id = 0; id < plan->Resources().size(); ++id)
        {
            const auto& spec = plan->Resources()[id];
            auto& item = files->resources_[id]; item.kind = spec.kind;
            if (spec.kind == ResourceKind::Alias) continue; // known null at initialization
            if (usage[id].output)
                Fail(spec.source, "Immutable static resource cannot supply an owning RTV/DSV, including through aliases");
            const auto source = FieldSource(*plan, spec, "filename");
            if (!spec.filename.empty()) item.bytes = ReadFile(spec, source, limits, total);
            else if (spec.kind == ResourceKind::PositionsBuffer) item.bytes.resize(16u * 4096u);
            else if (!spec.data.empty())
            {
                const auto data = Trim(spec.data);
                if (data.size() >= 2 && data.front() == '"' && data.back() == '"')
                {
                    if (spec.kind != ResourceKind::InlineBuffer) Fail(spec.source, "String data requires a typed byte buffer");
                    // Fixed ConstructInitialDataString copies bytes between the
                    // quotes verbatim, WITHOUT a synthetic NUL or escape decode.
                    item.bytes.assign(data.begin() + 1, data.end() - 1);
                }
                else
                {
                    std::istringstream values(data); values.imbue(std::locale::classic());
                    std::string type; values >> type;
                    if (spec.kind != ResourceKind::StructuredBuffer || LowerAscii(type) != "r32_float")
                        Fail(spec.source, "Only explicit R32_FLOAT structured initial data is supported");
                    float value;
                    while (values >> value)
                    {
                        if (!std::isfinite(value) || item.bytes.size() >= 4096u * 2048u) Fail(spec.source, "Invalid/oversize structured initial data");
                        const auto offset = item.bytes.size(); item.bytes.resize(offset + sizeof(value));
                        std::memcpy(item.bytes.data() + offset, &value, sizeof(value));
                    }
                    if (!values.eof()) Fail(spec.source, "Malformed structured initial data");
                }
            }
            if (spec.filename.empty())
            {
                if (item.bytes.empty() || item.bytes.size() > limits.perFileBytes || total > limits.totalBytes || item.bytes.size() > limits.totalBytes - total)
                    Fail(spec.source, "Inline/structured resource budget exceeded", DiagnosticCode::Limit);
                total += item.bytes.size();
            }
            auto format = ParseResourceFormat(spec.format);
            if (spec.kind == ResourceKind::InlineBuffer && spec.format.empty()) format = DXGI_FORMAT_R8_UINT;
            if (!spec.format.empty() && format == DXGI_FORMAT_UNKNOWN)
                Fail(FieldSource(*plan, spec, "format"), "Unsupported resource format: " + spec.format);
            const auto use = usage[id];
            if (spec.kind == ResourceKind::FileTexture)
            {
                if (use.vertex || use.index) Fail(spec.source, "Texture2D cannot supply vertex/index buffer bindings");
                if (LowerAscii(PathUtf8(spec.filename.extension())) != ".dds") Fail(source, "Unsupported texture file format; DDS Texture2D required (PNG/WIC not linked)");
                detail::DdsImage image;
                std::string error;
                if (!detail::DecodeDds(item.bytes, image, error)) Fail(source, std::move(error));
                if (format != DXGI_FORMAT_UNKNOWN && format != image.descriptor.Format)
                    Fail(FieldSource(*plan, spec, "format"), "Unsupported DDS format reinterpretation/conversion");
                if (spec.stride) Fail(FieldSource(*plan, spec, "stride"), "Texture2D stride override unsupported");
                item.texture = image.descriptor; item.viewFormat = image.descriptor.Format;
                for (const auto mip : image.mips) item.mips.push_back({mip.offset, mip.rowPitch, mip.slicePitch});
                continue;
            }
            if (spec.kind == ResourceKind::StructuredBuffer || spec.kind == ResourceKind::PositionsBuffer)
            {
                if (use.vertex || use.index || !spec.array || item.bytes.empty() || item.bytes.size() % spec.array)
                    Fail(spec.source, "Structured resource has incompatible bindings/array/data");
                const auto stride = item.bytes.size() / spec.array;
                if (!stride || stride > 2048 || stride % 4 || (spec.stride && spec.stride != stride))
                    Fail(spec.source, "StructuredBuffer requires exact data-derived stride");
                item.stride = static_cast<UINT>(stride);
                item.buffer = {static_cast<UINT>(item.bytes.size()),
                    spec.kind == ResourceKind::PositionsBuffer ? D3D11_USAGE_DEFAULT : D3D11_USAGE_IMMUTABLE,
                    UINT(D3D11_BIND_SHADER_RESOURCE | (spec.kind == ResourceKind::PositionsBuffer ? D3D11_BIND_UNORDERED_ACCESS : 0)),
                    0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, item.stride};
                item.viewFormat = DXGI_FORMAT_UNKNOWN;
                continue;
            }
            if (spec.kind != ResourceKind::FileBuffer && spec.kind != ResourceKind::InlineBuffer) Fail(spec.source, "Unsupported resource kind");
            if (item.bytes.size() > UINT32_MAX) Fail(source, "DX11 Buffer exceeds UINT ByteWidth", DiagnosticCode::Limit);
            const auto element = FormatElementBytes(format);
            if (format != DXGI_FORMAT_UNKNOWN && !element)
                Fail(FieldSource(*plan, spec, "format"), "Unsupported block-compressed/typeless buffer format");
            item.stride = spec.stride ? spec.stride : element;
            if (!item.stride || item.bytes.size() % item.stride)
                Fail(FieldSource(*plan, spec, "stride"), "Buffer requires exact nonzero stride/format and integral element count");
            if (use.index && format != DXGI_FORMAT_R16_UINT && format != DXGI_FORMAT_R32_UINT)
                Fail(FieldSource(*plan, spec, "format"), "Index buffer requires explicit R16_UINT or R32_UINT");
            if (use.index && item.stride != element) Fail(spec.source, "Index-buffer stride must equal the declared index format size");
            const bool srv = use.texture || (use.dynamic && element);
            if (srv && (!element || item.bytes.size() % element))
                Fail(FieldSource(*plan, spec, "format"), "Typed buffer SRV requires a supported format and exact elements");
            // Vertex and index creation flags are inexpensive, immutable and
            // legal together. Actual bind destination still needs tape preflight.
            item.buffer = {static_cast<UINT>(item.bytes.size()), D3D11_USAGE_IMMUTABLE,
                UINT(D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER | (srv ? D3D11_BIND_SHADER_RESOURCE : 0)), 0, 0, 0};
            item.viewFormat = format;
        }
        files->totalBytes_ = total; files->byteLimit_ = limits.totalBytes;
        return files;
    }
};
FilePreloadResult PreloadResources(std::shared_ptr<const Plan> plan, PreloadLimits limits)
{
    try { return {ResourceFileLoader::Load(std::move(plan), limits), {}}; }
    catch (const ResourceFailure& e) { return {nullptr, {e.diagnostic}}; }
    catch (const std::exception& e) { return {nullptr, {{DiagnosticCode::Io, {}, e.what()}}}; }
}
struct Dx11ResourceLoader
{
    static std::shared_ptr<const Dx11StaticResources> Create(std::shared_ptr<const ResourceFiles> files,
        ID3D11Device* device, std::uint64_t generation, std::uint64_t first, HashPolicy policy)
    {
        if (!files || !device || !generation || !first || files->Resources().size() > UINT64_MAX - first || policy.textureVersion > 1)
            Fail({}, "Invalid device/publication lease range/hash policy", DiagnosticCode::InvalidSyntax);
        if (policy.trackRegionHashes)
        {
            const auto& plan = files->Commands();
            const auto source = plan.Entries().empty() ? SourceLocation{} : plan.Programs()[plan.Entries().front()].source;
            Fail(source, "Unsupported XXMI track_region_hashes mode; this slice uses exact full-resource identity and separately compiled draw/index ranges");
        }
        auto set = std::make_shared<Dx11StaticResources>();
        if (files->Commands().Touches().debugOverlay || !files->Commands().SoftwareMouse().empty())
        {
            std::vector<Diagnostic> diagnostics;
            set->debugOverlay_ = CreateDebugOverlayLibrary(files->PlanOwner(), device, diagnostics);
            if (!set->debugOverlay_)
            {
                if (!diagnostics.empty()) throw ResourceFailure{std::move(diagnostics.front()), E_FAIL};
                Fail({}, "DebugShaderOutput resource preparation failed", DiagnosticCode::Unsupported);
            }
        }
        if (files->Commands().Touches().rasterCustom)
        {
            // ParseRSState defaults, NOT a merge with the game's state.
            D3D11_RASTERIZER_DESC desc{}; desc.FillMode = D3D11_FILL_SOLID; desc.DepthClipEnable = TRUE;
            for (unsigned i = 0; i < set->raster_.size(); ++i)
            {
                desc.CullMode = i ? D3D11_CULL_BACK : D3D11_CULL_NONE; desc.FrontCounterClockwise = !i;
                const auto hr = device->CreateRasterizerState(&desc, set->raster_[i].GetAddressOf());
                if (FAILED(hr) || !set->raster_[i]) Fail({}, "Exact TexFx raster state creation failed", DiagnosticCode::Unsupported, hr);
            }
        }
        set->files_ = files; set->firstLease_ = first; set->deviceGeneration_ = generation;
        set->leases_.resize(files->Resources().size());
        const auto& parameters = files->Parameters();
        if (parameters.required)
        {
            // Every full-size page and its exact-key columns were charged to
            // the existing preload budget before any device object is created.
            // Publication is all-or-nothing; a failed page never reaches draw.
            set->parameters_.resize(parameters.states);
            D3D11_TEXTURE1D_DESC desc{};
            desc.Width = IniParameterVectors; desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            std::vector<float> values(IniParameterScalars);
            for (Id scalar = 0; scalar < IniParameterScalars; ++scalar)
                values[scalar] = IniParameterFloat(parameters.defaults[scalar]);
            for (std::uint32_t state = 0; state < parameters.states; ++state)
            {
                for (const auto& slot : parameters.slots)
                    values[slot.scalar] = IniParameterFloat(slot.values[state]);
                auto& page = set->parameters_[state];
                D3D11_SUBRESOURCE_DATA initial{values.data(), 0, 0};
                auto hr = device->CreateTexture1D(&desc, &initial, page.texture.GetAddressOf());
                if (FAILED(hr)) Fail(parameters.source, "DX11 immutable IniParams Texture1D creation failed", DiagnosticCode::Unsupported, hr);
                D3D11_SHADER_RESOURCE_VIEW_DESC view{};
                view.Format = desc.Format; view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D; view.Texture1D.MipLevels = 1;
                hr = device->CreateShaderResourceView(page.texture.Get(), &view, page.srv.GetAddressOf());
                if (FAILED(hr)) Fail(parameters.source, "DX11 immutable IniParams SRV creation failed", DiagnosticCode::Unsupported, hr);
            }
        }
        for (Id id = 0; id < files->Resources().size(); ++id)
        {
            const auto& input = files->Resources()[id];
            const auto& spec = files->Commands().Resources()[id];
            auto& lease = set->leases_[id];
            lease.identity = ResourceIdentity::KnownNull();
            if (input.kind == ResourceKind::Alias) continue;
            const auto source = FieldSource(files->Commands(), spec, "filename");
            const auto object = first + id;
            HRESULT hr;
            IdentityRecord identity;
            if (input.kind != ResourceKind::FileTexture)
            {
                D3D11_SUBRESOURCE_DATA initial{input.bytes.data(), 0, 0};
                hr = device->CreateBuffer(&input.buffer, &initial, lease.buffer.GetAddressOf());
                if (FAILED(hr)) Fail(source, "DX11 immutable CreateBuffer failed", DiagnosticCode::Unsupported, hr);
                hr = lease.buffer.As(&lease.resource);
                if (FAILED(hr)) Fail(source, "DX11 buffer resource interface unavailable", DiagnosticCode::Unsupported, hr);
                if (input.buffer.BindFlags & D3D11_BIND_SHADER_RESOURCE)
                {
                    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
                    view.Format = input.viewFormat; view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                    const UINT element = (input.buffer.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) ?
                        input.buffer.StructureByteStride : FormatElementBytes(input.viewFormat);
                    if (!element || input.buffer.ByteWidth % element) Fail(source, "Invalid structured/typed view element size");
                    view.Buffer.NumElements = input.buffer.ByteWidth / element;
                    hr = device->CreateShaderResourceView(lease.resource.Get(), &view, lease.srv.GetAddressOf());
                    if (FAILED(hr)) Fail(source, "DX11 typed buffer SRV unsupported by device/format", DiagnosticCode::Unsupported, hr);
                }
                if (input.buffer.BindFlags & D3D11_BIND_UNORDERED_ACCESS)
                {
                    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
                    view.Format = DXGI_FORMAT_UNKNOWN; view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                    view.Buffer.NumElements = input.buffer.ByteWidth / input.buffer.StructureByteStride;
                    hr = device->CreateUnorderedAccessView(lease.resource.Get(), &view, lease.uav.GetAddressOf());
                    if (FAILED(hr)) Fail(source, "DX11 declared structured output UAV creation failed", DiagnosticCode::Unsupported, hr);
                }
                if (!identity.CreateBuffer(object, generation, input.buffer, {input.bytes.data(), input.bytes.size(), 0}))
                    Fail(source, "Unsupported buffer identity descriptor/CPU image");
                lease.binding = {object, lease.srv.Get() ? object : 0, generation, input.stride, 0,
                    static_cast<std::uint32_t>(input.viewFormat), input.buffer.ByteWidth};
            }
            else
            {
                std::vector<D3D11_SUBRESOURCE_DATA> initial;
                initial.reserve(input.mips.size());
                for (const auto mip : input.mips)
                    initial.push_back({input.bytes.data() + mip.offset, mip.rowPitch, mip.slicePitch});
                Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
                hr = device->CreateTexture2D(&input.texture, initial.data(), texture.GetAddressOf());
                if (FAILED(hr)) Fail(source, "DX11 immutable CreateTexture2D failed", DiagnosticCode::Unsupported, hr);
                hr = texture.As(&lease.resource);
                if (FAILED(hr)) Fail(source, "DX11 texture resource interface unavailable", DiagnosticCode::Unsupported, hr);
                D3D11_SHADER_RESOURCE_VIEW_DESC view{};
                view.Format = input.viewFormat; view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                view.Texture2D.MipLevels = input.texture.MipLevels;
                hr = device->CreateShaderResourceView(lease.resource.Get(), &view, lease.srv.GetAddressOf());
                if (FAILED(hr)) Fail(source, "DX11 texture SRV unsupported by device/format", DiagnosticCode::Unsupported, hr);
                UINT support = 0;
                lease.sampleableFloat2D = FloatTextureSampleFormat(view.Format) &&
                    SUCCEEDED(device->CheckFormatSupport(view.Format, &support)) &&
                    (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) && (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE);
                // A non-sampleable resource can still serve an existing exact
                // integer/load consumer. Only the new Outline path requires
                // this proof; no global resource/default semantics change.
                const auto mip = input.mips.front();
                if (!identity.CreateTexture2D(object, generation, input.texture,
                    {input.bytes.data() + mip.offset, mip.slicePitch, mip.rowPitch}, policy))
                    Fail(source, "Unsupported texture hash policy/descriptor/CPU layout");
                lease.binding = {object, object, generation, 0, 0, static_cast<std::uint32_t>(input.viewFormat), 0};
            }
            lease.identity = identity.Snapshot();
            // XXMI custom resources bypass HackerDevice creation registration.
            // Keep the computed CPU/descriptor creation fingerprint for bounds
            // and offline index windows, but NEVER turn it into game triggers.
            lease.identity.origin = ResourceOrigin::Custom;
            lease.identity.upstreamHash = 0;
            lease.identity.hashKnown = false;
        }
        // All immutable objects exist before startup aliases are materialized.
        // Copy the real lease/COM references and canonical source identity;
        // never allocate another texture or mint an alias-specific object ID.
        // Get(), Initialize() and Session's owning alias journal then start
        // from the same binding. Nothing is published on any earlier failure.
        for (const auto& initial : files->Commands().InitialResources())
        {
            if (initial.destination >= set->leases_.size() ||
                files->Resources()[initial.destination].kind != ResourceKind::Alias ||
                (initial.resource != InvalidId && (initial.resource >= set->leases_.size() ||
                 files->Resources()[initial.resource].kind == ResourceKind::Alias ||
                 files->Resources()[initial.resource].kind == ResourceKind::PositionsBuffer)))
                Fail(initial.source, "Invalid immutable startup resource binding", DiagnosticCode::InvalidSyntax);
            auto& destination = set->leases_[initial.destination];
            if (initial.resource != InvalidId) destination = set->leases_[initial.resource];
            else { destination = {}; destination.identity = ResourceIdentity::KnownNull(); }
        }
        return set;
    }
};
Dx11PreloadResult CreateDx11Resources(std::shared_ptr<const ResourceFiles> files, ID3D11Device* device,
    std::uint64_t generation, std::uint64_t firstLease, HashPolicy policy)
{
    try { return {Dx11ResourceLoader::Create(std::move(files), device, generation, firstLease, policy), {}, S_OK}; }
    catch (const ResourceFailure& e) { return {nullptr, {e.diagnostic}, e.hr}; }
    catch (const std::exception& e) { return {nullptr, {{DiagnosticCode::Io, {}, e.what()}}, E_FAIL}; }
}
bool Dx11StaticResources::Get(Id id, Dx11LeaseView& out) const noexcept
{
    out = {};
    if (id >= leases_.size()) return false;
    const auto& l = leases_[id];
    out = {l.resource.Get(), l.buffer.Get(), l.srv.Get(), l.binding, l.identity, l.sampleableFloat2D, l.uav.Get()};
    return true;
}
bool Dx11StaticResources::Resolve(Binding binding, Dx11LeaseView& out) const noexcept
{
    out = {};
    if (!binding.object) { out.identity = ResourceIdentity::KnownNull(); return !binding.view; }
    if (binding.generation != deviceGeneration_ || binding.object < firstLease_ || binding.object - firstLease_ >= leases_.size()) return false;
    const auto& lease = leases_[static_cast<std::size_t>(binding.object - firstLease_)];
    if (binding.object != lease.binding.object || binding.view != lease.binding.view ||
        binding.byteSize != lease.binding.byteSize || binding.format != lease.binding.format || binding.stride != lease.binding.stride ||
        (lease.buffer.Get() ? binding.offset > binding.byteSize : binding.offset != 0)) return false;
    out = {lease.resource.Get(), lease.buffer.Get(), lease.srv.Get(), binding, lease.identity, lease.sampleableFloat2D, lease.uav.Get()};
    return true;
}
bool Dx11StaticResources::Initialize(CommandState& state) const noexcept
{
    // State must be freshly created for this exact plan and have no pending
    // batch. The explicit plan identity check prevents partial initialization.
    if (state.PlanOwner().get() != files_->PlanOwner().get() || !state.CanInitialize()) return false;
    for (Id id = 0; id < leases_.size(); ++id)
        if (!state.InitializeResource(id, leases_[id].binding)) return false;
    return true;
}
} // namespace gimi
