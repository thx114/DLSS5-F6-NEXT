#include "GimiCommands.h"
#include "GimiRasterObservers.h"
#include "GimiExpression.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace gimi
{
const SourceLocation* ExecutionSource(const Plan& plan, const PrepareResult& result) noexcept
{
    if (result.program >= plan.Programs().size()) return nullptr;
    const auto& program = plan.Programs()[result.program];
    const auto phase = static_cast<unsigned>(result.phase);
    if (phase > 1 || result.command >= program.commands[phase].size()) return &program.source;
    return &program.commands[phase][result.command].source;
}

namespace
{
constexpr std::size_t VertexTextureBase = 32 + 1 + 128 + 2 + 8 + 1;
constexpr std::size_t PipelineSlots = VertexTextureBase + 128;
constexpr std::size_t ParameterSlots = IniParameterScalars;
std::size_t PipelineSlot(Target target) noexcept
{
    switch (target.kind)
    {
    case TargetKind::VertexBuffer: return target.index < 32 ? target.index : PipelineSlots;
    case TargetKind::IndexBuffer: return 32;
    case TargetKind::PixelTexture: return target.index < 128 ? 33 + target.index : PipelineSlots;
    case TargetKind::VertexShader: return 161;
    case TargetKind::PixelShader: return 162;
    case TargetKind::RenderTarget: return target.index < 8 ? 163 + target.index : PipelineSlots;
    case TargetKind::DepthTarget: return !target.index ? 171 : PipelineSlots;
    case TargetKind::VertexTexture: return target.index < 128 ? VertexTextureBase + target.index : PipelineSlots;
    default: return PipelineSlots;
    }
}
bool SameDraw(const DrawContext& a, const DrawContext& b) noexcept
{
    return a.firstIndex == b.firstIndex && a.indexCount == b.indexCount && a.firstVertex == b.firstVertex &&
           a.vertexCount == b.vertexCount && a.firstInstance == b.firstInstance && a.instanceCount == b.instanceCount &&
           a.baseVertex == b.baseVertex && a.drawIndexed == b.drawIndexed && a.instanced == b.instanced &&
           a.physicalCall == b.physicalCall;
}
bool UnsignedDraw(float v, std::uint32_t& out) noexcept
{
    if (!std::isfinite(v) || std::trunc(v) != v || v < 0 || static_cast<double>(v) > 4294967295.0) return false;
    out = static_cast<std::uint32_t>(v); return true;
}
bool SignedDraw(float v, std::int32_t& out) noexcept
{
    if (!std::isfinite(v) || std::trunc(v) != v || static_cast<double>(v) < -2147483648.0 ||
        static_cast<double>(v) > 2147483647.0) return false;
    out = static_cast<std::int32_t>(v); return true;
}
} // namespace

bool ValidatePresentVariables(const Plan& plan, Id program, Diagnostic& failure,
                              PresentValueRequirements& required, bool* compute)
{
    struct Check
    {
        const Plan& plan;
        Diagnostic& failure;
        std::vector<std::array<std::uint8_t, 4>> visited;
        std::vector<std::array<std::uint16_t, 4>> uses;
        bool native = false;
        bool Reject(const SourceLocation& at, const char* reason)
        { failure = {DiagnosticCode::Unsupported, at, reason}; return false; }
        bool ExpressionOf(Id id, const SourceLocation& at, std::uint16_t& mask, bool parameter)
        {
            if (id >= plan.Expressions().size()) return Reject(at, "Unsupported Present expression");
            for (const auto& token : plan.Expressions()[id].code)
            {
                if (DrawExpressionOperand(token.op) &&
                    !(parameter && (token.op == ExprOp::Parameter ||
                        (token.op == ExprOp::Filter && token.target.kind == TargetKind::Resource))))
                    return Reject(plan.Expressions()[id].source, "Unsupported native draw/filter/parameter value");
                if (parameter && (token.op == ExprOp::Parameter ||
                    (token.op == ExprOp::Filter && token.target.kind == TargetKind::Resource)))
                    native = true; // needs the authentic parameter/alias query adapter, even without a GPU action
                if (token.op == ExprOp::NativeValue)
                {
                    if (token.index >= static_cast<Id>(NativeValue::Count)) return Reject(at, "Invalid native value");
                    mask |= NativeValueBit(static_cast<NativeValue>(token.index));
                }
            }
            return true;
        }
        bool Visit(Id id, Phase phase, Phase physical, unsigned depth)
        {
            if (id >= plan.Programs().size()) return Reject({}, "Invalid native run target");
            const auto& p = plan.Programs()[id];
            if (depth >= plan.Limits().callDepth) return Reject(p.source, "Native run depth exceeded");
            const auto slot = static_cast<unsigned>(phase) + 2 * static_cast<unsigned>(physical);
            auto& state = visited[id][slot];
            if (state == 2) return true;
            if (state == 1) return Reject(p.source, "Native run cycle");
            if (p.kind != ProgramKind::Present && p.kind != ProgramKind::CommandList &&
                p.kind != ProgramKind::EmptyCustomShader && p.kind != ProgramKind::FormatText && p.kind != ProgramKind::RenderText && p.kind != ProgramKind::DebugOverlay &&
                p.kind != ProgramKind::SoftwareMouse && p.kind != ProgramKind::SoftwareMouseBW)
                return Reject(p.source, "Unsupported native run kind");
            const bool overlayPre = physical == Phase::Pre && plan.Touches().backbufferViewport && !plan.DebugOverlays().empty();
            const bool gpuPre = physical == Phase::Pre && !plan.FormatTexts().empty() && plan.NativeOnly(id);
            const bool mousePre = physical == Phase::Pre && !plan.SoftwareMouse().empty();
            const bool parameterPre = gpuPre || overlayPre || mousePre;
            const bool targetPost = physical == Phase::Post && plan.Touches().clearTargets;
            // Non-window scalars use the same finite immutable pages as raster.
            // PreloadResources proves every value; this admits no live draw fact.
            const bool finiteParameters = plan.Touches().iniParameters;
            if ((p.kind == ProgramKind::FormatText || p.kind == ProgramKind::RenderText) && !gpuPre)
                return Reject(p.source, "Text shaders require exclusive native PRE; GPU POST/raster is not implemented");
            state = 1; auto& mask = uses[id][slot];
            for (const auto& c : p.commands[static_cast<unsigned>(phase)])
            {
                switch (c.op)
                {
                case CommandOp::Assign: case CommandOp::BranchFalse:
                    if (!ExpressionOf(c.expressions[0], c.source, mask, parameterPre || targetPost || finiteParameters)) return false;
                    break;
                case CommandOp::Jump: break;
                case CommandOp::Run:
                    if (c.index >= plan.Programs().size()) return Reject(c.source, "Invalid Mouse run target");
                    if (plan.Programs()[c.index].kind == ProgramKind::SoftwareMouse ||
                        plan.Programs()[c.index].kind == ProgramKind::SoftwareMouseBW)
                    {
                        if (physical != Phase::Pre) return Reject(c.source, "Mouse CustomShader cannot run in native POST");
                        if (!Visit(c.index, Phase::Pre, physical, depth + 1) || !Visit(c.index, Phase::Post, physical, depth + 1)) return false;
                        mask |= uses[c.index][0] | uses[c.index][1];
                    }
                    else if (c.together)
                    {
                        if (!Visit(c.index, Phase::Pre, physical, depth + 1) || !Visit(c.index, Phase::Post, physical, depth + 1)) return false;
                        mask |= uses[c.index][2 * static_cast<unsigned>(physical)] | uses[c.index][1 + 2 * static_cast<unsigned>(physical)];
                    }
                    else
                    {
                        if (!Visit(c.index, phase, physical, depth + 1)) return false;
                        mask |= uses[c.index][slot];
                    }
                    break;
                case CommandOp::SetParameter:
                    if (!(finiteParameters && c.index >= 8 && c.index < IniParameterScalars) &&
                        !(mousePre && c.index < 8) && !targetPost && !(overlayPre && (c.index == 2 || c.index == 3 || c.index == 70 * 4)) &&
                        (!gpuPre || (c.index != 0 && c.index != 1 && c.index != 3)))
                        return Reject(c.source, "Native parameters require finite immutable scalars or the original restored text/mouse/overlay scope");
                    if (!ExpressionOf(c.expressions[0], c.source, mask, true)) return false;
                    native = true; break;
                case CommandOp::ClearTarget:
                    if (!targetPost || c.sourceTarget.kind != TargetKind::Resource)
                        return Reject(c.source, "Native clear requires the actual POST and a private target alias");
                    native = true; break;
                case CommandOp::Reference:
                {
                    const auto mouseSlot = [](Target t) { return (t.kind == TargetKind::VertexTexture || t.kind == TargetKind::PixelTexture) &&
                        (t.index == 100 || t.index == 101); };
                    if (!((mousePre && ((c.destination.kind == TargetKind::Resource && mouseSlot(c.sourceTarget)) ||
                          (mouseSlot(c.destination) && (c.sourceTarget.kind == TargetKind::Resource || c.sourceTarget.kind == TargetKind::Null)) ||
                          ((c.destination.kind == TargetKind::RenderTarget || c.destination.kind == TargetKind::DepthTarget) && c.sourceTarget.kind == TargetKind::Null))) ||
                          (parameterPre && c.destination.kind == TargetKind::Resource &&
                           (c.sourceTarget.kind == TargetKind::Resource || c.sourceTarget.kind == TargetKind::Null)) ||
                          (overlayPre && c.destination.kind == TargetKind::PixelTexture && c.destination.index >= 20 &&
                           c.destination.index <= 23 && (c.sourceTarget.kind == TargetKind::Resource || c.sourceTarget.kind == TargetKind::Null)) ||
                          (overlayPre && (c.destination.kind == TargetKind::RenderTarget || c.destination.kind == TargetKind::DepthTarget) &&
                           c.sourceTarget.kind == TargetKind::Null)))
                        return Reject(c.source, "Native PRE requires custom aliases, overlay t20..23, or explicit null outputs");
                    native = true; break;
                }
                case CommandOp::BackbufferViewport:
                    if (!(overlayPre || mousePre) || !(c.destination == Target{TargetKind::RenderTarget, 0}))
                        return Reject(c.source, "Backbuffer viewport requires native overlay PRE");
                    native = true; break;
                case CommandOp::MouseDraw:
                    if (!mousePre || (p.kind != ProgramKind::SoftwareMouse && p.kind != ProgramKind::SoftwareMouseBW))
                        return Reject(c.source, "Mouse draw requires its original native PRE scope");
                    native = true; break;
                case CommandOp::DebugOverlay:
                    if (!overlayPre || c.index >= plan.DebugOverlays().size())
                        return Reject(c.source, "Debug overlay requires real backbuffer native PRE");
                    native = true; break;
                case CommandOp::FormatText:
                    if (!gpuPre || c.index >= plan.FormatTexts().size()) return Reject(c.source, "Unsupported native compute stage");
                    native = true; break;
                case CommandOp::RenderText:
                    if (!gpuPre || c.index >= plan.RenderTexts().size()) return Reject(c.source, "Unsupported native text draw stage");
                    native = true; break;
                default:
                    return Reject(c.source, "Unsupported required native command or GPU stage");
                }
            }
            state = 2; return true;
        }
    } check{plan, failure, std::vector<std::array<std::uint8_t, 4>>(plan.Programs().size()),
             std::vector<std::array<std::uint16_t, 4>>(plan.Programs().size())};
    required = {}; if (compute) *compute = false;
    if (!check.Visit(program, Phase::Pre, Phase::Pre, 0) || !check.Visit(program, Phase::Post, Phase::Post, 0)) return false;
    required[0] = check.uses[program][0]; required[1] = check.uses[program][3];
    if (compute) *compute = check.native;
    return true;
}

struct CommandState::Storage
{
    struct CachedBinding { Binding value; std::uint64_t epoch = 0; };
    struct CachedParameter { float value = 0; std::uint64_t epoch = 0; };
    struct Journal
    {
        Id index = 0;
        bool resource = false, known = false;
        float value = 0;
        Binding binding;
    };
    std::shared_ptr<const Plan> plan;
    std::vector<float> variables;
    std::vector<float> presentPre; // allocated with the immutable plan
    const NativePresentValues* nativeValues = nullptr;
    std::uint32_t presentPreInstructions = 0;
    std::vector<Binding> resources;
    std::vector<std::uint8_t> resourceKnown;
    std::vector<std::uint64_t> variableEpoch, resourceEpoch, programEpoch;
    std::array<CachedBinding, PipelineSlots> pipeline{};
    std::vector<CachedParameter> parameters;
    std::vector<Journal> journal;
    std::vector<Action> actions;
    std::vector<float> expressionStack;
    std::vector<Id> matchStack;
    std::vector<ProgramInvocation> invocation;
    std::uint32_t journalCount = 0, actionCount = 0, instructions = 0, invocationCount = 0;
    std::uint64_t epoch = 0;
    bool pending = false, skip = false, open = false, postAccepted = false, combined = false;
    bool purePresent = false, presentOpen = false, nativeCompute = false, nativePending = false;
    Phase physicalPhase = Phase::Pre;
    bool nativeBackbuffer = false;
    DrawContext draw{}, invocationDraw{};
    const CommandQueries* queries = nullptr;
    const OverrideRequirement* requiredOverrides = nullptr; // synchronous PrepareDraw only
    std::uint32_t requiredOverrideCount = 0;
    const std::uint8_t* observationPrograms = nullptr; // synchronous original-tape guard
    const std::uint8_t* selectionPrograms = nullptr; // load-owned callback authority
    Id selectedShader = InvalidId;
    const MatchQuery* callbackQuery = nullptr; // original binding of this Check, before sibling edits
    std::array<bool, MaxOverrideRequirements> overrideReached{};
    PrepareResult result;

    explicit Storage(std::shared_ptr<const Plan> p) : plan(std::move(p))
    {
        if (!plan) throw std::invalid_argument("CommandState requires a compiled Plan");
        const auto& limits = plan->Limits();
        variables.reserve(plan->Variables().size());
        for (const auto& v : plan->Variables()) variables.push_back(v.initial);
        presentPre.resize(variables.size());
        resources.resize(plan->Resources().size()); resourceKnown.resize(resources.size());
        for (std::size_t i = 0; i < resources.size(); ++i)
            resourceKnown[i] = plan->Resources()[i].kind == ResourceKind::Alias;
        variableEpoch.resize(variables.size()); resourceEpoch.resize(resources.size());
        parameters.resize(ParameterSlots);
        journal.resize(variables.size() + resources.size());
        actions.resize(limits.actions); expressionStack.resize(limits.expressionStack);
        matchStack.resize(static_cast<std::size_t>(limits.callDepth) * limits.matchesPerCheck);
        invocation.resize(limits.matchesPerCheck);
        programEpoch.resize(plan->Programs().size());
    }
    bool Error(ExecutionError error) noexcept { result.error = error; return false; }
    void ReachedOverride(Id program, Target target, Binding binding, Phase phase) noexcept
    {
        if (physicalPhase != Phase::Pre || phase != Phase::Pre) return;
        for (std::uint32_t i = 0; i < requiredOverrideCount; ++i)
        {
            const auto& r = requiredOverrides[i]; const auto& b = r.binding;
            if (r.program == program && r.target == target && b.object == binding.object &&
                b.view == binding.view && b.generation == binding.generation &&
                b.stride == binding.stride && b.offset == binding.offset &&
                b.format == binding.format && b.byteSize == binding.byteSize)
                overrideReached[i] = true;
        }
    }
    bool Tick() noexcept
    {
        if (instructions >= plan->Limits().instructions) return Error(ExecutionError::InstructionLimit);
        ++instructions; return true;
    }
    Target ResolveThis(Target target) const noexcept
    {
        return target.kind == TargetKind::This ? draw.thisTarget : target;
    }
    bool BindingOf(Target target, Binding& binding) noexcept
    {
        target = ResolveThis(target);
        if (target.kind == TargetKind::Null) { binding = {}; return true; }
        if (target.kind == TargetKind::Resource)
        {
            if (target.index >= resources.size() || !resourceKnown[target.index]) return Error(ExecutionError::UnknownBinding);
            binding = resources[target.index]; return true;
        }
        const auto index = PipelineSlot(target);
        if (index == PipelineSlots) return Error(ExecutionError::UnknownBinding);
        auto& cached = pipeline[index];
        if (cached.epoch != epoch)
        {
            if (!queries->readBinding) return Error(ExecutionError::QueryUnavailable);
            if (!queries->readBinding(queries->context, target, cached.value)) return Error(ExecutionError::UnknownBinding);
            cached.epoch = epoch;
        }
        binding = cached.value; return true;
    }
    void SaveVariable(Id index) noexcept
    {
        if (variableEpoch[index] == epoch) return;
        variableEpoch[index] = epoch;
        auto& saved = journal[journalCount++];
        saved.index = index; saved.resource = false; saved.value = variables[index];
    }
    void SaveResource(Id index) noexcept
    {
        if (resourceEpoch[index] == epoch) return;
        resourceEpoch[index] = epoch;
        auto& saved = journal[journalCount++];
        saved.index = index; saved.resource = true; saved.binding = resources[index]; saved.known = resourceKnown[index] != 0;
    }
    void Rollback() noexcept
    {
        while (journalCount)
        {
            const auto& saved = journal[--journalCount];
            if (saved.resource) { resources[saved.index] = saved.binding; resourceKnown[saved.index] = saved.known; }
            else variables[saved.index] = saved.value;
        }
    }
    bool ActionOf(Action action) noexcept
    {
        if (actionCount == actions.size()) return Error(ExecutionError::ActionLimit);
        action.program = result.program; action.command = result.command; action.phase = result.phase;
        actions[actionCount++] = action; return true;
    }
    bool SetBinding(Target target, Binding binding) noexcept
    {
        target = ResolveThis(target);
        if (target.kind == TargetKind::Resource)
        {
            if (target.index >= resources.size()) return Error(ExecutionError::UnknownBinding);
            Action action; action.op = ActionOp::SetResource; action.destination = target;
            action.binding = binding; action.previous = resources[target.index];
            if (!ActionOf(action)) return false;
            SaveResource(target.index); resources[target.index] = binding; resourceKnown[target.index] = true;
            return true;
        }
        const auto index = PipelineSlot(target);
        if (index == PipelineSlots || target.kind == TargetKind::VertexShader || target.kind == TargetKind::PixelShader)
            return Error(ExecutionError::UnknownBinding);
        const bool output = target.kind == TargetKind::RenderTarget || target.kind == TargetKind::DepthTarget;
        if (output && binding.object)
        {
            Binding typed;
            if (!queries->outputReference || !queries->outputReference(queries->context, binding,
                target.kind == TargetKind::DepthTarget, typed)) return Error(ExecutionError::UnknownBinding);
            binding = typed;
        }
        Action action;
        action.op = output ? ActionOp::BindOutput : ActionOp::Bind;
        action.destination = target; action.binding = binding;
        if (!ActionOf(action)) return false;
        pipeline[index] = {binding, epoch};
        if (output && target.kind == TargetKind::RenderTarget && !target.index) nativeBackbuffer = false;
        return true;
    }
    bool ParameterOf(Id index, float& value) noexcept
    {
        if (index >= parameters.size()) return Error(ExecutionError::UnknownParameter);
        auto& cached = parameters[index];
        if (cached.epoch != epoch)
        {
            if (!queries->readParameter) return Error(ExecutionError::QueryUnavailable);
            if (!queries->readParameter(queries->context, index, cached.value)) return Error(ExecutionError::UnknownParameter);
            cached.epoch = epoch;
        }
        value = cached.value; return true;
    }
    bool Evaluate(Id id, float& value) noexcept
    {
        if (id >= plan->Expressions().size()) return Error(ExecutionError::InvalidExpression);
        const auto& expression = plan->Expressions()[id];
        std::size_t size = 0;
        for (const auto& token : expression.code)
        {
            if (purePresent && DrawExpressionOperand(token.op) &&
                !(nativeCompute && (token.op == ExprOp::Parameter ||
                    (token.op == ExprOp::Filter && token.target.kind == TargetKind::Resource))))
                return Error(ExecutionError::UnsupportedPresent);
            if (!Tick()) return false;
            if (ExpressionOperand(token.op))
            {
                if (size == expressionStack.size()) return Error(ExecutionError::InvalidExpression);
                float v = 0;
                switch (token.op)
                {
                case ExprOp::Literal: v = token.literal; break;
                case ExprOp::Variable:
                    if (token.index >= variables.size()) return Error(ExecutionError::InvalidExpression);
                    v = variables[token.index]; break;
                case ExprOp::Parameter: if (!ParameterOf(token.index, v)) return false; break;
                case ExprOp::Filter:
                {
                    Binding binding;
                    const auto target = ResolveThis(token.target);
                    if (!BindingOf(target, binding)) return false;
                    if (!binding.object) v = -0.0f; // upstream's known-null marker
                    else
                    {
                        if (!queries->filterIndex) return Error(ExecutionError::QueryUnavailable);
                        const MatchQuery query{target, binding, &draw};
                        if (!queries->filterIndex(queries->context, *plan, query, v)) return Error(ExecutionError::UnknownFilter);
                    }
                    break;
                }
                case ExprOp::FirstIndex: v = static_cast<float>(draw.firstIndex); break;
                case ExprOp::IndexCount: v = static_cast<float>(draw.indexCount); break;
                case ExprOp::VertexCount: v = static_cast<float>(draw.vertexCount); break;
                case ExprOp::InstanceCount: v = draw.instanced ? static_cast<float>(draw.instanceCount) : 0.0f; break;
                // Fixed XXMI DrawCallInfo stores BaseVertexLocation in unsigned
                // FirstVertex; expression conversion preserves that behavior.
                // from_caller below separately retains the exact signed INT.
                case ExprOp::FirstVertex: v = static_cast<float>(draw.drawIndexed ? static_cast<std::uint32_t>(draw.baseVertex) : draw.firstVertex); break;
                case ExprOp::FirstInstance: v = static_cast<float>(draw.firstInstance); break;
                case ExprOp::DrawType:
                    if (!PhysicalDrawType(draw, v)) return Error(ExecutionError::QueryUnavailable);
                    break;
                case ExprOp::NativeValue:
                    if (token.index >= static_cast<Id>(NativeValue::Count)) return Error(ExecutionError::QueryUnavailable);
                    if (purePresent)
                    {
                        if (!nativeValues) return Error(ExecutionError::QueryUnavailable);
                        const auto operand = static_cast<NativeValue>(token.index);
                        const bool outputOperand = operand == NativeValue::RenderTargetWidth || operand == NativeValue::RenderTargetHeight;
                        const auto& outputSlot = pipeline[PipelineSlot({TargetKind::RenderTarget, 0})];
                        if (!(outputOperand && (nativeBackbuffer || (outputSlot.epoch == epoch && !outputSlot.value.object))) &&
                            !nativeValues->Has(NativeValueBit(operand))) return Error(ExecutionError::QueryUnavailable);
                        if (operand == NativeValue::RenderTargetWidth || operand == NativeValue::RenderTargetHeight)
                        {
                            if (nativeBackbuffer)
                            {
                                const auto resolution = operand == NativeValue::RenderTargetWidth ? NativeValue::ResolutionWidth : NativeValue::ResolutionHeight;
                                if (!nativeValues->Has(NativeValueBit(resolution))) return Error(ExecutionError::QueryUnavailable);
                                v = nativeValues->values[static_cast<Id>(resolution)];
                            }
                            else if (pipeline[PipelineSlot({TargetKind::RenderTarget, 0})].epoch == epoch &&
                                !pipeline[PipelineSlot({TargetKind::RenderTarget, 0})].value.object) v = -1.0f;
                            else v = nativeValues->values[token.index];
                        }
                        else v = nativeValues->values[token.index];
                    }
                    else
                    {
                        Binding output;
                        const auto operand = static_cast<NativeValue>(token.index);
                        if ((operand == NativeValue::RenderTargetWidth || operand == NativeValue::RenderTargetHeight) &&
                            !BindingOf({TargetKind::RenderTarget, 0}, output)) return false;
                        if (!queries->nativeValue || !queries->nativeValue(queries->context, operand, output, v))
                            return Error(ExecutionError::QueryUnavailable);
                    }
                    if (!std::isfinite(v)) return Error(ExecutionError::InvalidExpression);
                    break;
                default: return Error(ExecutionError::InvalidExpression);
                }
                expressionStack[size++] = v;
            }
            else if (token.op <= ExprOp::Not)
            {
                if (!size || !detail::Unary(token.op, expressionStack[size - 1])) return Error(ExecutionError::InvalidExpression);
            }
            else
            {
                if (size < 2) return Error(ExecutionError::InvalidExpression);
                const float rhs = expressionStack[--size];
                if (!detail::Binary(token.op, expressionStack[size - 1], rhs)) return Error(ExecutionError::InvalidExpression);
            }
        }
        if (size != 1) return Error(ExecutionError::InvalidExpression);
        value = expressionStack[0]; return true;
    }
    bool SelectedCallback(Id id, Phase phase) noexcept
    {
        if (!selectionPrograms || (selectionPrograms[id] & (SelectDependency | SelectPassive))) return true;
        const auto flags = selectionPrograms[id];
        const auto* spec = plan->OverrideForProgram(id);
        if (!spec) return false; // e.g. an unselected package ShaderRegex
        if (spec->shader) return (flags & SelectExplicit) && id == selectedShader;
        if (!(flags & (SelectExplicit | SelectSkipCompanion))) return false;
        for (std::uint32_t i = 0; i < requiredOverrideCount; ++i)
        {
            const auto& required = requiredOverrides[i];
            const auto* selected = plan->OverrideForProgram(required.program);
            if (!selected || !(required.target == draw.thisTarget) ||
                selected->hash != spec->hash ||
                (!(flags & SelectSkipCompanion) && required.program != id)) continue;
            // POST cleanup may see the replacement binding. Its authority is
            // the exact PRE Check already reached in this same transaction.
            if (physicalPhase == Phase::Post || phase == Phase::Post) return overrideReached[i];
            Binding binding;
            // Earlier ordered siblings may already have replaced the slot.
            // The Check's original exact query authorizes the whole sibling
            // set; never reclassify the new custom binding as the game input.
            if (callbackQuery && callbackQuery->target == required.target) binding = callbackQuery->binding;
            else if (!BindingOf(required.target, binding)) return false;
            const auto& original = required.binding;
            if (binding.object == original.object && binding.view == original.view &&
                binding.generation == original.generation && binding.stride == original.stride &&
                binding.offset == original.offset && binding.format == original.format &&
                binding.byteSize == original.byteSize) return true;
        }
        return false;
    }
    bool Execute(Id id, Phase phase, unsigned depth) noexcept
    {
        if (depth >= plan->Limits().callDepth) return Error(ExecutionError::RecursionLimit);
        if (id >= plan->Programs().size()) return Error(ExecutionError::InvalidProgram);
        if (!SelectedCallback(id, phase))
        {
            result.program = id; result.command = 0; result.phase = phase;
            return Error(ExecutionError::UnselectedOverride);
        }
        if (observationPrograms && (!(observationPrograms[id] & ObservePacketProgram) ||
            (plan->Programs()[id].kind == ProgramKind::TextureOverride &&
             !(observationPrograms[id] & ObserveCpuResource))))
        {
            result.program = id; result.command = 0; result.phase = phase;
            return Error(ExecutionError::ObservationEffect);
        }
        programEpoch[id] = epoch; // actual reached entry, including a real Check
        if (purePresent && plan->Programs()[id].kind != ProgramKind::Present &&
            plan->Programs()[id].kind != ProgramKind::CommandList &&
            plan->Programs()[id].kind != ProgramKind::EmptyCustomShader &&
            !(nativeCompute && (plan->Programs()[id].kind == ProgramKind::FormatText ||
                plan->Programs()[id].kind == ProgramKind::RenderText || plan->Programs()[id].kind == ProgramKind::DebugOverlay ||
                plan->Programs()[id].kind == ProgramKind::SoftwareMouse || plan->Programs()[id].kind == ProgramKind::SoftwareMouseBW))) return Error(ExecutionError::UnsupportedPresent);
        const auto& commands = plan->Programs()[id].commands[static_cast<unsigned>(phase)];
        for (std::uint32_t pc = 0; pc < commands.size();)
        {
            result.program = id; result.command = pc; result.phase = phase;
            if (!Tick()) return false;
            const auto& command = commands[pc++];
            if (purePresent && command.op != CommandOp::Assign && command.op != CommandOp::BranchFalse &&
                command.op != CommandOp::Jump && command.op != CommandOp::Run &&
                !(nativeCompute && ((physicalPhase == Phase::Pre && (command.op == CommandOp::SetParameter ||
                    command.op == CommandOp::Reference || command.op == CommandOp::FormatText || command.op == CommandOp::RenderText ||
                    command.op == CommandOp::DebugOverlay || command.op == CommandOp::BackbufferViewport || command.op == CommandOp::MouseDraw)) ||
                    (physicalPhase == Phase::Post && (command.op == CommandOp::ClearTarget || command.op == CommandOp::SetParameter)))))
                return Error(ExecutionError::UnsupportedPresent);
            float a = 0, b = 0, c = 0;
            switch (command.op)
            {
            case CommandOp::Assign:
                if (command.index >= variables.size()) return Error(ExecutionError::InvalidProgram);
                if (!Evaluate(command.expressions[0], a)) return false;
                SaveVariable(command.index); variables[command.index] = a;
                break;
            case CommandOp::StorePixelConstant:
                if (purePresent) return Error(ExecutionError::UnsupportedPresent);
                if (command.index >= variables.size()) return Error(ExecutionError::InvalidProgram);
                if (!queries->readPixelConstant) return Error(ExecutionError::QueryUnavailable);
                if (!queries->readPixelConstant(queries->context, command.constantSlot, command.constantFloat, a) ||
                    !std::isfinite(a)) return Error(ExecutionError::UnknownConstant);
                SaveVariable(command.index); variables[command.index] = a;
                break;
            case CommandOp::FormatText:
            {
                if (!purePresent || !nativeCompute || physicalPhase != Phase::Pre || command.index >= plan->FormatTexts().size())
                    return Error(ExecutionError::UnsupportedPresent);
                const auto& format = plan->FormatTexts()[command.index];
                Action action; action.op = ActionOp::FormatText; action.formatText = command.index;
                if (!BindingOf({TargetKind::Resource, format.text}, action.formatBindings[1])) return false;
                // Known-null is the source's legal no-dispatch branch, not a
                // fabricated resource or a claim of actual material/CS work.
                if (!action.formatBindings[1].object) break;
                if (!BindingOf({TargetKind::Resource, format.font}, action.formatBindings[0]) ||
                    !BindingOf({TargetKind::Resource, format.parameters}, action.formatBindings[2]) ||
                    !BindingOf({TargetKind::Resource, format.positions}, action.formatBindings[3]) || !ActionOf(action)) return false;
                break;
            }
            case CommandOp::BackbufferViewport:
            {
                if (!purePresent || !nativeCompute || physicalPhase != Phase::Pre || !plan->Touches().backbufferViewport)
                    return Error(ExecutionError::UnsupportedPresent);
                nativeBackbuffer = true;
                Action action; action.op = ActionOp::BackbufferViewport;
                action.destination = {TargetKind::RenderTarget, 0};
                if (!ActionOf(action)) return false;
                break;
            }
            case CommandOp::MouseDraw:
            {
                if (!purePresent || !nativeCompute || physicalPhase != Phase::Pre) return Error(ExecutionError::UnsupportedPresent);
                Action action; action.op = ActionOp::MouseDraw; action.raster = command.index;
                for (unsigned n = 0; n < 4; ++n)
                    if (!BindingOf({n < 2 ? TargetKind::VertexTexture : TargetKind::PixelTexture, 100 + n % 2}, action.overlayBindings[n])) return false;
                if (!ActionOf(action)) return false;
                break;
            }
            case CommandOp::DebugOverlay:
            {
                if ((purePresent && (!nativeCompute || physicalPhase != Phase::Pre || !plan->Touches().backbufferViewport)) ||
                    command.index >= plan->DebugOverlays().size())
                    return Error(ExecutionError::InvalidProgram);
                Action action; action.op = ActionOp::DebugOverlay; action.debugOverlay = command.index;
                for (unsigned i = 0; i < action.overlayBindings.size(); ++i)
                    if (!BindingOf({TargetKind::PixelTexture, 20 + i}, action.overlayBindings[i])) return false;
                if (!ActionOf(action)) return false;
                break;
            }
            case CommandOp::RenderText:
            {
                if (!purePresent || !nativeCompute || physicalPhase != Phase::Pre || command.index >= plan->RenderTexts().size())
                    return Error(ExecutionError::UnsupportedPresent);
                const auto& render = plan->RenderTexts()[command.index];
                Action action; action.op = ActionOp::RenderText; action.renderText = command.index;
                if (!BindingOf({TargetKind::Resource, render.font}, action.renderBindings[0]) ||
                    !BindingOf({TargetKind::Resource, render.text}, action.renderBindings[1]) ||
                    !BindingOf({TargetKind::Resource, render.parameters}, action.renderBindings[2]) ||
                    !BindingOf({TargetKind::Resource, render.positions}, action.renderBindings[3])) return false;
                for (const auto& binding : action.renderBindings) if (!binding.object) return Error(ExecutionError::UnknownBinding);
                if (!ActionOf(action)) return false;
                break;
            }
            case CommandOp::UnbindVertexConstant:
            {
                // Actual Offset post-run reaches both logical callee phases.
                // Qualify the PHYSICAL boundary; never turn Present/PRE into a
                // draw cleanup or silently ignore the setter.
                if (purePresent || physicalPhase != Phase::Post) return Error(ExecutionError::InvalidPhase);
                if (command.index >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT ||
                    !(plan->Touches().vertexConstantNulls & (std::uint32_t{1} << command.index)))
                    return Error(ExecutionError::InvalidProgram);
                Action action; action.op = ActionOp::UnbindVertexConstant; action.constantSlot = command.index;
                if (!ActionOf(action)) return false;
                break;
            }
            case CommandOp::SetParameter:
            {
                if (command.index >= parameters.size()) return Error(ExecutionError::InvalidProgram);
                if (!Evaluate(command.expressions[0], a)) return false;
                Action action; action.op = ActionOp::SetParameter; action.parameter = command.index; action.value = a;
                if (!ActionOf(action)) return false;
                parameters[command.index] = {a, epoch};
                break;
            }
            case CommandOp::ClearTarget:
            {
                Binding binding;
                if (!BindingOf(command.sourceTarget, binding)) return false;
                if (!binding.object) break; // actual null is the upstream no-op
                Action action; action.op = ActionOp::ClearTarget; action.binding = binding;
                if (!ActionOf(action)) return false;
                break;
            }
            case CommandOp::CopyDescription:
            {
                const auto sourceTarget = ResolveThis(command.sourceTarget);
                const auto alias = ResolveThis(command.destination);
                if (purePresent || alias.kind != TargetKind::Resource ||
                    (sourceTarget.kind != TargetKind::RenderTarget && sourceTarget.kind != TargetKind::DepthTarget))
                    return Error(ExecutionError::UnknownBinding);
                Binding source, destination;
                if (!BindingOf(sourceTarget, source)) return false;
                if (!source.object)
                {
                    if (!command.unlessNull && !SetBinding(alias, {})) return false;
                    break;
                }
                if (!queries->copyDescription || !queries->copyDescription(queries->context, sourceTarget, source, alias.index, destination))
                    return Error(ExecutionError::UnknownBinding);
                Action action; action.op = ActionOp::CopyDescription;
                action.destination = alias; action.binding = destination; action.previous = source;
                if (!ActionOf(action) || !SetBinding(alias, destination)) return false;
                break;
            }
            case CommandOp::CopyDepth:
            {
                if (purePresent || ResolveThis(command.sourceTarget).kind != TargetKind::DepthTarget ||
                    ResolveThis(command.destination).kind != TargetKind::Resource)
                    return Error(ExecutionError::UnknownBinding);
                Binding source, destination;
                if (!BindingOf(command.sourceTarget, source)) return false;
                if (!source.object)
                {
                    if (!command.unlessNull && !SetBinding(command.destination, {})) return false;
                    break; // real known-null unless_null leaves the old alias exactly intact
                }
                if (!queries->depthCopy || !queries->depthCopy(queries->context, source, destination))
                    return Error(ExecutionError::UnknownBinding);
                Action action; action.op = ActionOp::CopyDepth;
                action.destination = command.destination; action.binding = destination; action.previous = source;
                if (!ActionOf(action) || !SetBinding(command.destination, destination)) return false;
                break;
            }
            case CommandOp::Reference:
            {
                const auto source = ResolveThis(command.sourceTarget);
                const auto destination = ResolveThis(command.destination);
                if (purePresent && (source.kind == TargetKind::RenderTarget || source.kind == TargetKind::DepthTarget ||
                    ((destination.kind == TargetKind::RenderTarget || destination.kind == TargetKind::DepthTarget) &&
                     !(nativeCompute && physicalPhase == Phase::Pre && plan->Touches().backbufferViewport && source.kind == TargetKind::Null))))
                    return Error(ExecutionError::UnsupportedPresent);
                Binding binding;
                if (!BindingOf(command.sourceTarget, binding)) return false;
                if (command.sourceTarget.kind == TargetKind::Null || !command.unlessNull || binding.object)
                    if (!SetBinding(command.destination, binding)) return false;
                break;
            }
            case CommandOp::Run:
            {
                if (command.index >= plan->Programs().size()) return Error(ExecutionError::InvalidProgram);
                const auto& callee = plan->Programs()[command.index];
                if (callee.kind == ProgramKind::SoftwareMouse || callee.kind == ProgramKind::SoftwareMouseBW)
                {
                    if (!purePresent || !nativeCompute || physicalPhase != Phase::Pre) return Error(ExecutionError::UnsupportedPresent);
                    // CustomShader run is a complete nested scope at this tape
                    // position. POST resource setters are inside that scope.
                    // Save the interpreter's pipeline cursor as well as the GPU
                    // state; global aliases/locals retain their actual effects.
                    const auto savedPipeline = pipeline; const bool savedBackbuffer = nativeBackbuffer;
                    Action scope; scope.op = ActionOp::BeginMouse; scope.debugOverlay = callee.mouse;
                    scope.raster = callee.kind == ProgramKind::SoftwareMouse ? 1u : 2u;
                    if (!ActionOf(scope) || !Execute(command.index, Phase::Pre, depth + 1) ||
                        !Execute(command.index, Phase::Post, depth + 1)) return false;
                    scope.op = ActionOp::EndMouse;
                    if (!ActionOf(scope)) return false;
                    pipeline = savedPipeline; nativeBackbuffer = savedBackbuffer;
                    break;
                }
                if (callee.kind == ProgramKind::RasterCustom)
                {
                    if (purePresent) return Error(ExecutionError::UnsupportedPresent);
                    // XXMI CustomShader saves its OM state at CALL entry and
                    // restores it on return, independently of native phase exit.
                    std::array<Binding, 9> outputs{};
                    for (unsigned i = 0; i < outputs.size(); ++i)
                        if (!BindingOf({i == 8 ? TargetKind::DepthTarget : TargetKind::RenderTarget, i == 8 ? 0u : i}, outputs[i])) return false;
                    Action scope; scope.op = ActionOp::BeginRasterCustom; scope.raster = callee.raster;
                    if (!ActionOf(scope) || !Execute(command.index, Phase::Pre, depth + 1) ||
                        !Execute(command.index, Phase::Post, depth + 1)) return false;
                    scope.op = ActionOp::EndRasterCustom;
                    if (!ActionOf(scope)) return false;
                    for (unsigned i = 0; i < outputs.size(); ++i)
                    {
                        const Target target{i == 8 ? TargetKind::DepthTarget : TargetKind::RenderTarget, i == 8 ? 0u : i};
                        pipeline[PipelineSlot(target)] = {outputs[i], epoch};
                    }
                    break;
                }
                if (command.together)
                {
                    if (!Execute(command.index, Phase::Pre, depth + 1) || !Execute(command.index, Phase::Post, depth + 1)) return false;
                }
                else if (!Execute(command.index, phase, depth + 1)) return false;
                break;
            }
            case CommandOp::Check:
            {
                const auto target = ResolveThis(command.sourceTarget);
                Binding binding;
                if (!BindingOf(target, binding)) return false;
                if (!binding.object) break;
                if (!queries->textureMatches) return Error(ExecutionError::QueryUnavailable);
                auto* matches = matchStack.data() + static_cast<std::size_t>(depth) * plan->Limits().matchesPerCheck;
                std::uint32_t count = 0;
                const MatchQuery query{target, binding, &draw};
                if (!queries->textureMatches(queries->context, *plan, query, matches, plan->Limits().matchesPerCheck, count) ||
                    count > plan->Limits().matchesPerCheck) return Error(ExecutionError::MatchFailure);
                const OverrideSpec* previous = nullptr;
                // Validate the entire match set before executing any of it.
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    const auto* spec = plan->OverrideForProgram(matches[i]);
                    // Plan storage orders hash groups first, descriptor group
                    // last; each group retains priority/name order. A Check's
                    // hash group must have exactly one hash, then descriptors.
                    if (!spec || spec->shader || !spec->MatchesDraw(draw.drawIndexed, draw.firstIndex, draw.indexCount) ||
                        (previous && (spec <= previous ||
                         (previous->matchKind != OverrideMatchKind::Hash && spec->matchKind != previous->matchKind) ||
                         (previous->matchKind == OverrideMatchKind::Hash && spec->matchKind == OverrideMatchKind::Hash &&
                          previous->hash != spec->hash))))
                        return Error(ExecutionError::InvalidMatch);
                    previous = spec;
                }
                const Target saved = draw.thisTarget;
                struct CallbackScope
                {
                    Storage& state; const MatchQuery* previous;
                    ~CallbackScope() { state.callbackQuery = previous; }
                } callbackScope{*this, callbackQuery};
                callbackQuery = &query;
                draw.thisTarget = target;
                if (command.together)
                {
                    // All PRE matches, then all POST matches (not interleaved).
                    for (const auto p : {Phase::Pre, Phase::Post})
                        for (std::uint32_t i = 0; i < count; ++i)
                        {
                            ReachedOverride(matches[i], target, binding, p);
                            if (!Execute(matches[i], p, depth + 1)) { draw.thisTarget = saved; return false; }
                        }
                }
                else
                    for (std::uint32_t i = 0; i < count; ++i)
                    {
                        ReachedOverride(matches[i], target, binding, phase);
                        if (!Execute(matches[i], phase, depth + 1)) { draw.thisTarget = saved; return false; }
                    }
                draw.thisTarget = saved;
                break;
            }
            case CommandOp::Skip:
                if (physicalPhase == Phase::Post) return Error(ExecutionError::PostSkip);
                skip = true; break;
            case CommandOp::Draw:
            {
                if (!Evaluate(command.expressions[0], a) || !Evaluate(command.expressions[1], b)) return false;
                Action action; action.op = ActionOp::Draw;
                if (!UnsignedDraw(a, action.count) || !UnsignedDraw(b, action.first))
                    return Error(ExecutionError::InvalidDrawArguments);
                if (!ActionOf(action)) return false;
                break;
            }
            case CommandOp::DrawIndexed:
            {
                if (!Evaluate(command.expressions[0], a) || !Evaluate(command.expressions[1], b) || !Evaluate(command.expressions[2], c)) return false;
                Action action; action.op = ActionOp::DrawIndexed;
                if (!UnsignedDraw(a, action.count) || !UnsignedDraw(b, action.first) || !SignedDraw(c, action.baseVertex))
                    return Error(ExecutionError::InvalidDrawArguments);
                if (!ActionOf(action)) return false;
                break;
            }
            case CommandOp::DrawIndexedInstanced:
            {
                float d = 0, e = 0;
                Action action; action.op = ActionOp::DrawIndexedInstanced;
                if (!Evaluate(command.expressions[0], a) || !Evaluate(command.expressions[1], b) ||
                    !Evaluate(command.expressions[2], c) || !Evaluate(command.expressions[3], d) ||
                    !Evaluate(command.expressions[4], e)) return false;
                if (!UnsignedDraw(a, action.count) || !UnsignedDraw(b, action.instances) ||
                    !UnsignedDraw(c, action.first) || !SignedDraw(d, action.baseVertex) || !UnsignedDraw(e, action.firstInstance))
                    return Error(ExecutionError::InvalidDrawArguments);
                if (!ActionOf(action)) return false;
                break;
            }
            case CommandOp::DrawIndexedAuto:
            case CommandOp::DrawIndexedInstancedAuto:
            {
                Action action; action.autoIndices = true;
                action.op = command.op == CommandOp::DrawIndexedAuto ? ActionOp::DrawIndexed : ActionOp::DrawIndexedInstanced;
                // No float conversion of the exact physical arguments.
                action.instances = draw.instanced ? draw.instanceCount : 0;
                action.firstInstance = draw.instanced ? draw.firstInstance : 0;
                if (!ActionOf(action)) return false;
                break;
            }
            case CommandOp::DrawFromCaller:
            {
                // No indirect buffer/count inferred from an indexed binding.
                if (draw.instanced && !draw.drawIndexed) return Error(ExecutionError::InvalidDrawArguments);
                Action action;
                action.op = draw.instanced ? ActionOp::DrawIndexedInstanced : draw.drawIndexed ? ActionOp::DrawIndexed : ActionOp::Draw;
                action.count = draw.drawIndexed ? draw.indexCount : draw.vertexCount;
                action.first = draw.drawIndexed ? draw.firstIndex : draw.firstVertex;
                action.baseVertex = draw.baseVertex; action.instances = draw.instanceCount; action.firstInstance = draw.firstInstance;
                if (!ActionOf(action)) return false;
                break;
            }
            case CommandOp::BranchFalse:
                if (!Evaluate(command.expressions[0], a)) return false;
                if (a) break;
                [[fallthrough]];
            case CommandOp::Jump:
                if (command.index < pc || command.index > commands.size()) return Error(ExecutionError::InvalidProgram);
                pc = command.index; break;
            }
        }
        return true;
    }
};

CommandState::CommandState(std::shared_ptr<const Plan> plan) : state_(std::make_unique<Storage>(std::move(plan))) {}
CommandState::~CommandState() = default;
const std::shared_ptr<const Plan>& CommandState::PlanOwner() const noexcept { return state_->plan; }
bool CommandState::CanInitialize() const noexcept { return !state_->pending && !state_->open && !state_->presentOpen; }

bool CommandState::InitializeResource(Id id, Binding binding) noexcept
{
    auto& s = *state_;
    if (s.pending || s.open || s.presentOpen || id >= s.resources.size() ||
        (!binding.object && s.plan->Resources()[id].kind != ResourceKind::Alias)) return false;
    s.resources[id] = binding; s.resourceKnown[id] = true; return true;
}
bool CommandState::InitializeVariable(Id id, float value) noexcept
{
    auto& s = *state_;
    if (s.pending || s.open || s.presentOpen || id >= s.variables.size() || !s.plan->Variables()[id].persistent || !std::isfinite(value)) return false;
    s.variables[id] = value; return true;
}
bool CommandState::ReadVariable(Id id, float& value) const noexcept
{
    const auto& s = *state_;
    if (s.pending || id >= s.variables.size()) return false;
    value = s.variables[id]; return true;
}
bool CommandState::ApplyKeyCycles(const std::array<bool, 4>& down, std::array<bool, 4>& previous,
                                  std::array<int, 4>& cursors) noexcept
{
    auto& s = *state_;
    const auto& cycles = s.plan->KeyCycles();
    if (!CanInitialize() || cycles.size() != 4 || s.epoch == UINT64_MAX) return false;
    // No draw/parameter/native facts can be guessed at an input boundary.
    for (const auto& cycle : cycles)
    {
        if (cycle.variable >= s.variables.size()) return false;
        if (cycle.condition == InvalidId) continue;
        if (cycle.condition >= s.plan->Expressions().size()) return false;
        for (const auto& token : s.plan->Expressions()[cycle.condition].code)
            if (DrawExpressionOperand(token.op) || token.op == ExprOp::NativeValue) return false;
    }
    for (const int cursor : cursors) if (cursor < -1 || cursor > 1) return false;
    auto nextCursors = cursors;
    ++s.epoch; s.result = {}; s.journalCount = s.actionCount = s.instructions = 0;
    for (unsigned i = 0; i < cycles.size(); ++i)
    {
        if (!down[i] || previous[i]) continue;
        const auto& cycle = cycles[i];
        int current = nextCursors[i];
        // XXMI smart=true: synchronize to the current variable, not a cursor
        // invented from zero. Unknown values use the previous cycle position.
        for (int j = 0; j < 2; ++j)
            if (s.variables[cycle.variable] == cycle.values[j]) { current = j; break; }
        const int next = (current + 1) % 2;
        nextCursors[i] = next;
        float condition = 1;
        if (cycle.condition != InvalidId && !s.Evaluate(cycle.condition, condition))
        { s.Rollback(); return false; }
        if (!condition) continue;
        s.SaveVariable(cycle.variable); s.variables[cycle.variable] = cycle.values[next];
    }
    s.journalCount = 0;
    cursors = nextCursors; previous = down;
    return true;
}

bool CommandState::ReadResource(Id id, Binding& value) const noexcept
{
    const auto& s = *state_;
    if (s.pending || id >= s.resources.size() || !s.resourceKnown[id]) return false;
    value = s.resources[id]; return true;
}

PrepareResult CommandState::Prepare(Id program, Phase phase, const DrawContext& draw, const CommandQueries& queries) noexcept
{
    const ProgramInvocation entry{program, draw.thisTarget};
    return PrepareSequence(&entry, 1, phase, draw, queries);
}

PrepareResult CommandState::PrepareSequence(const ProgramInvocation* programs, std::uint32_t count, Phase phase,
                                           const DrawContext& draw, const CommandQueries& queries) noexcept
{
    auto& s = *state_;
    PrepareResult failure;
    if (s.pending || s.presentOpen) { failure.error = ExecutionError::PendingBatch; return failure; }
    if (!programs || !count || count > s.invocation.size() || (phase != Phase::Pre && phase != Phase::Post))
    { failure.error = ExecutionError::InvalidProgram; return failure; }
    if ((phase == Phase::Pre && s.open) || (phase == Phase::Post && (!s.open || s.postAccepted)))
    { failure.error = ExecutionError::InvalidPhase; return failure; }
    for (std::uint32_t i = 0; i < count; ++i)
        if (programs[i].program >= s.plan->Programs().size() || programs[i].thisTarget.kind == TargetKind::This)
        { failure.error = ExecutionError::InvalidProgram; return failure; }
    if (phase == Phase::Post)
    {
        if (count != s.invocationCount || !SameDraw(draw, s.invocationDraw))
        { failure.error = ExecutionError::InvalidPhase; return failure; }
        for (std::uint32_t i = 0; i < count; ++i)
            if (programs[i].program != s.invocation[i].program || !(programs[i].thisTarget == s.invocation[i].thisTarget))
            { failure.error = ExecutionError::InvalidPhase; return failure; }
    }
    // Avoid a once-per-wrap whole-state scan in a hook; reload after uint64 wrap.
    if (s.epoch == (std::numeric_limits<std::uint64_t>::max)())
    { failure.error = ExecutionError::InstructionLimit; return failure; }
    ++s.epoch;
    s.result = {}; s.result.phase = phase;
    s.combined = false;
    s.physicalPhase = phase; s.skip = false;
    s.journalCount = s.actionCount = s.instructions = 0;
    s.draw = draw; s.queries = &queries;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        s.draw.thisTarget = programs[i].thisTarget;
        if (!s.Execute(programs[i].program, phase, 0))
        {
            s.Rollback(); s.actionCount = 0; s.queries = nullptr;
            return s.result;
        }
    }
    s.queries = nullptr;
    if (phase == Phase::Pre)
    {
        for (std::uint32_t i = 0; i < count; ++i) s.invocation[i] = programs[i];
        s.invocationCount = count; s.invocationDraw = draw;
    }
    s.pending = true;
    s.result.batch = {s.plan.get(), s.actions.data(), s.actionCount, s.skip, phase, &s.plan->Touches()};
    return s.result;
}

PrepareResult CommandState::PrepareDraw(const ProgramInvocation* programs, std::uint32_t count,
                                       const DrawContext& draw, const CommandQueries& queries, Id requiredProgram,
                                       const OverrideRequirement* requiredOverrides, std::uint32_t requiredOverrideCount,
                                       const std::uint8_t* observationPrograms,
                                       const std::uint8_t* selectionPrograms) noexcept
{
    auto& s = *state_;
    if ((requiredProgram != InvalidId && requiredProgram >= s.plan->Programs().size()) ||
        requiredOverrideCount > MaxOverrideRequirements || (requiredOverrideCount && !requiredOverrides))
    { PrepareResult failure; failure.error = ExecutionError::InvalidProgram; return failure; }
    const OverrideSpec* previous = nullptr;
    for (std::uint32_t i = 0; i < requiredOverrideCount; ++i)
    {
        const auto& r = requiredOverrides[i]; const auto* spec = s.plan->OverrideForProgram(r.program);
        if (!spec || spec->shader || spec->matchKind != OverrideMatchKind::Hash ||
            !spec->MatchesDraw(draw.drawIndexed, draw.firstIndex, draw.indexCount) ||
            (previous && spec <= previous) || !r.binding.object || !r.binding.generation ||
            (r.target.kind != TargetKind::IndexBuffer && r.target.kind != TargetKind::VertexBuffer &&
             r.target.kind != TargetKind::PixelTexture && r.target.kind != TargetKind::VertexTexture))
        { PrepareResult failure; failure.error = ExecutionError::InvalidMatch; return failure; }
        previous = spec; // exact plan order also rejects duplicate requirements
    }
    s.requiredOverrides = requiredOverrides; s.requiredOverrideCount = requiredOverrideCount;
    s.observationPrograms = observationPrograms;
    s.selectionPrograms = selectionPrograms; s.selectedShader = requiredProgram;
    std::fill_n(s.overrideReached.begin(), requiredOverrideCount, false);
    struct ClearRequirements
    {
        Storage& s;
        ~ClearRequirements()
        {
            s.requiredOverrides = nullptr; s.requiredOverrideCount = 0; s.observationPrograms = nullptr;
            s.selectionPrograms = nullptr; s.selectedShader = InvalidId; s.callbackQuery = nullptr;
        }
    } clear{s};
    auto result = PrepareSequence(programs, count, Phase::Pre, draw, queries);
    if (!result) return result;
    s.pending = false;
    s.result.program = InvalidId; s.result.command = 0; s.result.phase = Phase::Pre;
    Action boundary; boundary.op = ActionOp::OriginalDrawBoundary; boundary.skipOriginal = s.skip;
    if (!s.ActionOf(boundary))
    {
        s.Rollback(); s.actionCount = 0; s.result.batch = {}; return s.result;
    }
    s.physicalPhase = Phase::Post;
    s.queries = &queries;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        s.draw.thisTarget = programs[i].thisTarget;
        if (!s.Execute(programs[i].program, Phase::Post, 0))
        {
            s.Rollback(); s.actionCount = 0; s.queries = nullptr;
            // Invalidate the earlier PRE view: no partially prepared tape escapes.
            s.result.batch = {}; return s.result;
        }
    }
    s.queries = nullptr;
    if (requiredProgram != InvalidId && s.programEpoch[requiredProgram] != s.epoch)
    {
        s.Rollback(); s.actionCount = 0; s.result.batch = {};
        s.result.program = requiredProgram; s.result.command = 0; s.result.phase = Phase::Pre;
        s.result.error = ExecutionError::RequiredProgramMissing;
        return s.result; // no GPU effect, alias/variable journal rolled back
    }
    for (std::uint32_t i = 0; i < requiredOverrideCount; ++i)
        if (!s.overrideReached[i])
        {
            s.Rollback(); s.actionCount = 0; s.result.batch = {};
            s.result.program = requiredOverrides[i].program; s.result.command = 0; s.result.phase = Phase::Pre;
            s.result.error = ExecutionError::RequiredProgramMissing;
            return s.result; // a matching identity is not proof of real Check consumption
        }
    s.combined = true; s.pending = true;
    s.result.batch = {s.plan.get(), s.actions.data(), s.actionCount, s.skip, Phase::Pre, &s.plan->Touches(), true};
    return s.result;
}

PrepareResult CommandState::BeginVariablePresent(const ProgramInvocation* programs, std::uint32_t count,
                                                const NativePresentValues& values, bool preflightPost) noexcept
{
    auto& s = *state_;
    s.purePresent = true; s.nativeValues = &values;
    const CommandQueries queries{};
    const DrawContext noDraw{}; // purePresent explicitly rejects every draw-derived operand
    auto result = PrepareSequence(programs, count, Phase::Pre, noDraw, queries);
    if (!result) { s.nativeValues = nullptr; s.purePresent = false; return result; }
    std::copy(s.variables.begin(), s.variables.end(), s.presentPre.begin());
    s.presentPreInstructions = s.instructions;
    // TIME is frozen for this token. A POST depending only on TIME/variables can
    // be preflighted now, preserving the invariant M6 whole-pair early refusal.
    // Future window/DPI/OM/swap-chain values cannot be speculated into POST.
    // The caller only enables this for a phase whose potential reads are invariant.
    s.pending = false; s.physicalPhase = Phase::Post; s.queries = &queries;
    bool ready = s.actionCount == 0;
    if (!ready) s.Error(ExecutionError::UnsupportedPresent);
    if (preflightPost)
        for (std::uint32_t i = 0; ready && i < count; ++i)
        {
            s.draw.thisTarget = programs[i].thisTarget;
            ready = s.Execute(programs[i].program, Phase::Post, 0);
        }
    result = s.result;
    s.Rollback(); s.queries = nullptr; s.nativeValues = nullptr; s.pending = false; s.purePresent = false;
    s.actionCount = 0; s.result.batch = {}; result.batch = {};
    if (!ready) { s.invocationCount = 0; return result; }
    std::copy(s.presentPre.begin(), s.presentPre.end(), s.variables.begin());
    s.presentOpen = true; return result;
}
bool CommandState::CompleteVariablePresent(bool actualReturn, const NativePresentValues& values) noexcept
{
    auto& s = *state_;
    if (!s.presentOpen) return false;
    s.presentOpen = false;
    if (!actualReturn) { s.invocationCount = 0; return true; }
    // A fresh journal epoch is essential: POST may write a variable already
    // written by PRE. Failure must restore the committed PRE, not the old frame.
    if (s.epoch == (std::numeric_limits<std::uint64_t>::max)())
    { s.invocationCount = 0; return false; }
    ++s.epoch;
    s.result = {}; s.result.phase = Phase::Post; s.physicalPhase = Phase::Post;
    s.instructions = s.presentPreInstructions; // one combined bounded native-pair budget
    s.journalCount = s.actionCount = 0; s.purePresent = true; s.nativeValues = &values;
    const CommandQueries queries{}; s.queries = &queries;
    bool ready = true;
    for (std::uint32_t i = 0; ready && i < s.invocationCount; ++i)
    {
        s.draw.thisTarget = s.invocation[i].thisTarget;
        ready = s.Execute(s.invocation[i].program, Phase::Post, 0);
    }
    ready = ready && s.actionCount == 0;
    if (!ready) s.Rollback(); // atomic CPU phase; never a partially published POST
    else s.journalCount = 0;
    s.invocationCount = s.actionCount = 0; s.queries = nullptr; s.nativeValues = nullptr; s.purePresent = false;
    return ready;
}

PrepareResult CommandState::PrepareNativePresent(const ProgramInvocation* programs, std::uint32_t count,
                                               const NativePresentValues& values, const CommandQueries& queries) noexcept
{
    auto& s = *state_;
    if (s.pending || s.presentOpen || s.open) { PrepareResult error; error.error = ExecutionError::PendingBatch; return error; }
    s.purePresent = s.nativeCompute = true; s.nativeBackbuffer = false; s.nativeValues = &values;
    auto result = PrepareSequence(programs, count, Phase::Pre, {}, queries);
    s.nativeValues = nullptr; s.purePresent = s.nativeCompute = false;
    if (result) { s.nativePending = true; s.presentPreInstructions = s.instructions; }
    return result;
}
PrepareResult CommandState::PrepareNativePost(const NativePresentValues& values, const CommandQueries& queries) noexcept
{
    auto& s = *state_;
    if (!s.presentOpen || s.pending || s.nativePending || s.epoch == UINT64_MAX)
    { PrepareResult error; error.error = ExecutionError::PendingBatch; return error; }
    ++s.epoch; s.presentOpen = false;
    s.result = {}; s.result.phase = s.physicalPhase = Phase::Post;
    s.instructions = s.presentPreInstructions; s.journalCount = s.actionCount = 0;
    s.purePresent = s.nativeCompute = true; s.nativeBackbuffer = false; s.nativeValues = &values; s.queries = &queries;
    bool ready = true;
    for (std::uint32_t i = 0; ready && i < s.invocationCount; ++i)
    {
        s.draw.thisTarget = s.invocation[i].thisTarget;
        ready = s.Execute(s.invocation[i].program, Phase::Post, 0);
    }
    s.queries = nullptr; s.nativeValues = nullptr; s.purePresent = s.nativeCompute = false;
    if (!ready) { s.Rollback(); s.actionCount = s.invocationCount = 0; return s.result; }
    s.pending = s.nativePending = true;
    s.result.batch = {s.plan.get(), s.actions.data(), s.actionCount, s.skip, Phase::Post, &s.plan->Touches()};
    return s.result;
}
bool CommandState::AcceptNativePresent() noexcept
{
    auto& s = *state_;
    if (!s.nativePending || !s.pending) return false;
    s.journalCount = 0; s.pending = s.nativePending = false;
    s.presentOpen = s.physicalPhase == Phase::Pre;
    if (!s.presentOpen) s.invocationCount = 0;
    s.result.batch = {}; return true;
}
void CommandState::DiscardNativePresent() noexcept
{
    auto& s = *state_;
    if (!s.nativePending) return;
    Discard(); s.nativePending = false; s.invocationCount = 0;
}

void CommandState::Accept() noexcept
{
    auto& s = *state_;
    if (!s.pending) return;
    s.journalCount = 0; s.pending = false;
    if (s.combined) { s.invocationCount = 0; return; }
    if (s.physicalPhase == Phase::Pre) { s.open = true; s.postAccepted = false; }
    else s.postAccepted = true;
}
void CommandState::Discard() noexcept
{
    auto& s = *state_;
    if (!s.pending) return;
    s.Rollback(); s.actionCount = 0; s.pending = false;
}
bool CommandState::FinishInvocation() noexcept
{
    auto& s = *state_;
    if (s.pending || !s.open || !s.postAccepted) return false;
    s.open = false; s.postAccepted = false; s.invocationCount = 0;
    return true;
}
} // namespace gimi
