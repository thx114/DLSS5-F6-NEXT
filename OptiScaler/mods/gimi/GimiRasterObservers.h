#pragma once

#include "GimiCommands.h"
#include <algorithm>

namespace gimi
{
// A separate immutable subscription, compiled only at Load. This never edits
// a source program or turns a TextureOverride into an automatic callback.
// Bit 0 permits a complete original program in an observation packet; bit 1
// additionally proves a resource program has only CPU variable effects.
inline constexpr std::uint8_t ObservePacketProgram = 1, ObserveCpuResource = 2, ObserveVariableWrite = 4;
class RasterObservers final
{
public:
    static constexpr unsigned TargetCapacity = 32;
    struct Root
    {
        Id program = InvalidId;
        bool packet = false;
        std::array<Target, TargetCapacity> targets{};
        unsigned targetCount = 0;
    };
    struct Exact { std::uint64_t hash = 0; Root root; };
    struct Routes
    {
        const Root* exact = nullptr;
        const std::vector<Root>* model = nullptr;
    };
private:
    struct Summary
    {
        bool safe = true, packet = false, writes = false, queries = false;
        std::vector<Target> targets;
    };
    std::vector<std::uint8_t> programs_;
    std::vector<Exact> exact_;
    std::array<std::vector<Root>, 9> models_;
    std::vector<std::uint64_t> resources_;
    bool active_ = false;
public:
    bool Compile(const Plan& plan, Diagnostic& failure)
    {
        // Visiting both original phases also rejects recursion. Check is a
        // dynamic edge: its complete real match set is guarded by CommandState,
        // rather than assuming all possible resources are CPU-only at Load.
        std::vector<Summary> summaries(plan.Programs().size());
        std::vector<std::uint8_t> state(summaries.size());
        const auto mergeTarget = [](Summary& into, Target target) {
            if (std::find(into.targets.begin(), into.targets.end(), target) == into.targets.end())
                into.targets.push_back(target);
            if (into.targets.size() > TargetCapacity) into.safe = false;
        };
        const auto visit = [&](auto&& self, Id id, unsigned depth) -> bool {
            if (id >= summaries.size() || depth >= plan.Limits().callDepth) return false;
            if (state[id] == 2) return summaries[id].safe;
            if (state[id] == 1) return false;
            state[id] = 1;
            auto& value = summaries[id];
            const auto& program = plan.Programs()[id];
            if (program.kind != ProgramKind::CommandList && program.kind != ProgramKind::TextureOverride &&
                program.kind != ProgramKind::ShaderOverride && program.kind != ProgramKind::ShaderRegex &&
                program.kind != ProgramKind::EmptyCustomShader) value.safe = false;
            for (unsigned phase = 0; phase != 2; ++phase)
                for (const auto& command : program.commands[phase])
                {
                    switch (command.op)
                    {
                    case CommandOp::Assign: value.writes = true; break;
                    case CommandOp::StorePixelConstant:
                        // No speculative POST CB read before the real return.
                        value.writes = value.queries = true; if (phase) value.safe = false; break;
                    case CommandOp::BranchFalse: case CommandOp::Jump: break;
                    case CommandOp::SetParameter: case CommandOp::UnbindVertexConstant:
                        value.packet = true; break;
                    case CommandOp::Check:
                    {
                        value.queries = true;
                        const auto target = command.sourceTarget;
                        if (phase || target.kind == TargetKind::This ||
                            (target.kind != TargetKind::PixelTexture && target.kind != TargetKind::VertexTexture &&
                             target.kind != TargetKind::VertexBuffer && target.kind != TargetKind::IndexBuffer))
                            value.safe = false;
                        else mergeTarget(value, target);
                        break;
                    }
                    case CommandOp::Run:
                        if (!self(self, command.index, depth + 1)) value.safe = false;
                        else
                        {
                            const auto& callee = summaries[command.index];
                            value.packet |= callee.packet; value.writes |= callee.writes;
                            // POST must depend only on variables, TIME and the
                            // authentic outer draw arguments. Conservatively
                            // reject any callee with PRE queries from POST.
                            if (phase && callee.queries) value.safe = false;
                            value.queries |= callee.queries;
                            for (const auto target : callee.targets) mergeTarget(value, target);
                        }
                        break;
                    default: value.safe = false; break;
                    }
                    for (const auto expression : command.expressions)
                    {
                        if (expression == InvalidId) continue;
                        if (expression >= plan.Expressions().size()) { value.safe = false; continue; }
                        for (const auto& token : plan.Expressions()[expression].code)
                            if (token.op == ExprOp::Parameter || token.op == ExprOp::Filter ||
                                (token.op == ExprOp::NativeValue && token.index != static_cast<Id>(NativeValue::Time)))
                                value.safe = false;
                    }
                }
            state[id] = 2;
            return value.safe;
        };
        for (Id id = 0; id < summaries.size(); ++id) visit(visit, id, 0);
        programs_.resize(summaries.size());
        for (Id id = 0; id < summaries.size(); ++id)
        {
            const auto& value = summaries[id];
            if (!value.safe) continue;
            programs_[id] = ObservePacketProgram;
            if (!value.packet && value.targets.empty()) programs_[id] |= ObserveCpuResource;
            if (value.writes) programs_[id] |= ObserveVariableWrite;
        }
        for (const auto& spec : plan.Overrides())
            if (!spec.shader && spec.matchKind == OverrideMatchKind::Hash &&
                (programs_[spec.program] & ObserveCpuResource) && summaries[spec.program].writes)
                resources_.push_back(spec.hash);
        std::sort(resources_.begin(), resources_.end());
        resources_.erase(std::unique(resources_.begin(), resources_.end()), resources_.end());
        const auto makeRoot = [&](Id id) {
            Root root; root.program = id; root.packet = summaries[id].packet;
            root.targetCount = static_cast<unsigned>(summaries[id].targets.size());
            std::copy(summaries[id].targets.begin(), summaries[id].targets.end(), root.targets.begin());
            return root;
        };
        for (const auto& spec : plan.Overrides())
            if (spec.shader && (programs_[spec.program] & ObservePacketProgram) &&
                (summaries[spec.program].writes || !summaries[spec.program].targets.empty()))
                exact_.push_back({spec.hash, makeRoot(spec.program)});
        // Exact shader merging is already unsupported by the original linker.
        // Do not select a sibling merely because it is CPU-only.
        for (std::size_t i = 1; i < plan.Overrides().size(); ++i)
            if (plan.Overrides()[i].shader && plan.Overrides()[i - 1].shader &&
                plan.Overrides()[i].hash == plan.Overrides()[i - 1].hash)
            {
                failure = {DiagnosticCode::Unsupported, plan.Overrides()[i].source,
                    "Independent raster observation requires unambiguous original ShaderOverride identity"};
                return false;
            }
        for (const auto& group : plan.ShaderRegexGroups())
            if (!group.RequiresBytecode() && (programs_[group.program] & ObservePacketProgram) &&
                !summaries[group.program].targets.empty() && !resources_.empty())
                for (const auto model : group.models)
                    if (static_cast<unsigned>(model) < models_.size())
                        models_[static_cast<unsigned>(model)].push_back(makeRoot(group.program));
        std::sort(exact_.begin(), exact_.end(), [](const auto& a, const auto& b) { return a.hash < b.hash; });
        for (const auto& model : models_)
            if (model.size() + 1 > plan.Limits().matchesPerCheck || model.size() + 1 > 128)
            {
                failure = {DiagnosticCode::Unsupported, {}, "Independent raster observation exceeds original root capacity"};
                return false;
            }
        active_ = !exact_.empty();
        for (const auto& model : models_) active_ |= !model.empty();
        return true;
    }
    Routes For(std::uint64_t hash, ShaderModel model) const noexcept
    {
        Routes out;
        const auto found = std::lower_bound(exact_.begin(), exact_.end(), hash,
            [](const Exact& value, std::uint64_t key) { return value.hash < key; });
        if (found != exact_.end() && found->hash == hash) out.exact = &found->root;
        if (static_cast<unsigned>(model) < models_.size()) out.model = &models_[static_cast<unsigned>(model)];
        return out;
    }
    bool Resource(std::uint64_t hash) const noexcept
    { return std::binary_search(resources_.begin(), resources_.end(), hash); }
    const std::uint8_t* Programs() const noexcept { return programs_.data(); }
    bool Active() const noexcept { return active_; }
};
} // namespace gimi
