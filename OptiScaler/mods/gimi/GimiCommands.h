#pragma once

#include "GimiPlan.h"

namespace gimi
{
// Opaque PRELOADED object/view lease IDs, never a hash, COM pointer, or address
// identity. Zero object means known null. The external owner retains every live
// lease across Resource assignments and all GPU submissions/retirement.
struct Binding
{
    std::uint64_t object = 0;
    std::uint64_t view = 0;
    std::uint64_t generation = 0;
    std::uint32_t stride = 0, offset = 0, format = 0, byteSize = 0;
};

// Upstream DrawCallInfo.h exposes these stable numbers through draw_type.
// DrawCommandType is a DIFFERENT enum and must never supply these values.
// Only authentic entry points currently owned by this runtime are represented.
enum class PhysicalDrawCall : std::uint8_t { Unknown = 0, Draw = 1, DrawIndexed = 2, DrawIndexedInstanced = 4 };

struct DrawContext
{
    std::uint32_t firstIndex = 0, indexCount = 0;
    std::uint32_t firstVertex = 0, vertexCount = 0;
    std::uint32_t firstInstance = 0, instanceCount = 1;
    Target thisTarget;
    std::int32_t baseVertex = 0; // exact original DrawIndexed argument, no vertexCount inference
    bool instanced = false; // authentic physical boundary, not inferred from count
    bool drawIndexed = true; // exact physical boundary kind, never inferred from an IB binding
    // Appended: old aggregate callers retain their layout of explicit fields.
    // Unknown must not acquire a physical type from default flags/counts.
    PhysicalDrawCall physicalCall = PhysicalDrawCall::Unknown;
};
inline bool PhysicalDrawType(const DrawContext& draw, float& value) noexcept
{
    switch (draw.physicalCall)
    {
    case PhysicalDrawCall::Draw:
        if (draw.drawIndexed || draw.instanced) return false;
        break;
    case PhysicalDrawCall::DrawIndexed:
        if (!draw.drawIndexed || draw.instanced) return false;
        break;
    case PhysicalDrawCall::DrawIndexedInstanced:
        if (!draw.drawIndexed || !draw.instanced) return false;
        break;
    default: return false; // no fake Present/indirect/dispatch/default context
    }
    value = static_cast<float>(draw.physicalCall);
    return true;
}

// A manually selected resource override must be reached through an actual
// PRE Check of this exact original binding, not an unrelated Run/POST or a
// same-hash object. The caller retains these numeric leases for PrepareDraw.
inline constexpr std::uint32_t MaxOverrideRequirements = 32;
struct OverrideRequirement
{
    Id program = InvalidId;
    Target target;
    Binding binding;
};

struct MatchQuery
{
    Target target;
    Binding binding;
    const DrawContext* draw = nullptr;
};

// These callbacks are queries during PREPARE, never GPU side effects. A false
// result means unknown/unsupported, not an empty/null success. They must be
// bounded, noexcept, allocation-free, IO-free and operate on cached/preloaded
// data. Returned match IDs MUST come from this Plan's compiled Overrides and be
// in hash-then-descriptor order, each priority/name ordered; overflow must
// return false, not truncate.
struct CommandQueries
{
    void* context = nullptr;
    bool (*readBinding)(void*, Target, Binding&) noexcept = nullptr;
    bool (*filterIndex)(void*, const Plan&, const MatchQuery&, float&) noexcept = nullptr;
    bool (*readParameter)(void*, Id, float&) noexcept = nullptr;
    bool (*textureMatches)(void*, const Plan&, const MatchQuery&, Id*, std::uint32_t capacity,
                           std::uint32_t& count) noexcept = nullptr;
    // PREPARE only: acquire an already-created private copy destination. The
    // executor retains both owners before any GPU effect; never copies here.
    bool (*depthCopy)(void*, Binding source, Binding& destination) noexcept = nullptr;
    bool (*copyDescription)(void*, Target source, Binding, Id alias, Binding& destination) noexcept = nullptr;
    bool (*nativeValue)(void*, NativeValue, Binding, float&) noexcept = nullptr;
    bool (*outputReference)(void*, Binding, bool depth, Binding&) noexcept = nullptr;
    // Reached store only: real PS getter/range and current CPU-content authority.
    // No GPU operation, allocation, synthetic null value or immutable-page cache.
    bool (*readPixelConstant)(void*, Id slot, Id floatIndex, float&) noexcept = nullptr;
};

enum class ActionOp : std::uint8_t { Bind, CopyDepth, SetResource, SetParameter, DrawIndexed, Draw, OriginalDrawBoundary, FormatText, RenderText, CopyDescription, ClearTarget, BindOutput, BeginRasterCustom, EndRasterCustom, DrawIndexedInstanced, DebugOverlay, BackbufferViewport, BeginMouse, EndMouse, MouseDraw, UnbindVertexConstant };
struct Action
{
    ActionOp op = ActionOp::Bind;
    // Numeric source indices avoid copying filesystem paths/strings per draw.
    Id program = InvalidId;
    std::uint32_t command = 0;
    Phase phase = Phase::Pre;
    Target destination;
    Binding binding;
    Binding previous; // SetResource: allows owner retention/retirement without scans
    Id parameter = 0;
    float value = 0;
    std::uint32_t count = 0, first = 0, instances = 0, firstInstance = 0;
    unsigned raster = 0;
    bool autoIndices = false; // resolve against tape-position retained IB
    std::int32_t baseVertex = 0;
    bool skipOriginal = false; // OriginalDrawBoundary only
    Id debugOverlay = InvalidId;
    std::array<Binding, 4> overlayBindings{}; // exact tape-position t20/t21/t22/t23
    Id formatText = InvalidId, renderText = InvalidId;
    std::array<Binding, 4> formatBindings{}; // exact font/text/params/output leases at this command
    std::array<Binding, 4> renderBindings{}; // exact font/text/params/same CS Position leases
    Id constantSlot = InvalidId; // UnbindVertexConstant only; not an IniParams scalar
};

struct BatchView
{
    const Plan* plan = nullptr;
    const Action* actions = nullptr;
    std::uint32_t count = 0;
    bool skipOriginal = false;
    Phase phase = Phase::Pre;
    const TouchMask* touches = nullptr;
    bool includesDrawBoundary = false;
};

struct ProgramInvocation
{
    Id program = InvalidId;
    Target thisTarget;
};

enum class ExecutionError : std::uint8_t
{
    None, InvalidProgram, PendingBatch, InvalidPhase, QueryUnavailable, UnknownBinding,
    UnknownFilter, UnknownParameter, MatchFailure, InvalidMatch, RecursionLimit,
    InstructionLimit, ActionLimit, InvalidExpression, InvalidDrawArguments, PostSkip, RequiredProgramMissing,
    UnsupportedPresent, UnknownConstant, ObservationEffect, UnselectedOverride
};
struct PrepareResult
{
    ExecutionError error = ExecutionError::None;
    Id program = InvalidId;
    std::uint32_t command = 0;
    Phase phase = Phase::Pre;
    BatchView batch;
    explicit operator bool() const noexcept { return error == ExecutionError::None; }
};

// Resolve a failed numeric execution location outside the hook; the pointed-to
// location belongs to the immutable plan. No path/string construction here.
const SourceLocation* ExecutionSource(const Plan& plan, const PrepareResult& result) noexcept;

// Missing bits are unknown, including a zero-initialized snapshot. No success
// defaults. A snapshot belongs only to its real native phase and token.
struct NativePresentValues
{
    std::array<float, static_cast<unsigned>(NativeValue::Count)> values{};
    std::uint16_t known = 0;
    bool Has(std::uint16_t required) const noexcept { return (known & required) == required; }
};
// Potential reads per physical phase, used only to select metadata sampling.
// Unchosen BranchFalse/Jump arms need not be known. Evaluate checks every
// operand actually reached; failure still rolls back the entire prepared phase.
using PresentValueRequirements = std::array<std::uint16_t, 2>;

// Load-time native Present closure: CPU variables/real native facts, existing
// TextPrinter, finite immutable parameters, private POST clears, or DebugOverlay PRE.
// Every branch/phase is checked. Executors still prove the full reached tape,
// immutable parameter pages, actual resource/state owners and GPU retirement.
// Mixed TextPrinter/overlay PRE uses one ordered preflight/reservation/final reader.
// Draw-derived facts never acquire made-up Present values.
bool ValidatePresentVariables(const Plan&, Id program, Diagnostic& failure, PresentValueRequirements&, bool* compute = nullptr);

// Load-owned callback admission, separate from declaration/required closure.
// Full lookup results remain intact; an unselected effect rejects the tape.
inline constexpr std::uint8_t SelectDependency = 1, SelectPassive = 2,
    SelectExplicit = 4, SelectSkipCompanion = 8;

// One serial owner/context per state; create at Load/device preparation, never
// in a draw. The plan is immutable and can be shared; mutable variables/aliases
// cannot be shared concurrently. No locks/IO/GPU calls/logging here.
//
// Prepare is transactional: it journals ONLY modified semantic slots, builds a
// bounded action tape, and simulates bindings for later expressions/checks.
// Unknown dependency, cycle/limit/argument failure -> rollback, no batch.
// Validate the ENTIRE tape (leases, capabilities, restoration, shader/layout,
// resource hazards, draw bounds, parameter visibility) before its first GPU
// effect. Accept only after successful submission; otherwise Discard. A partial
// GPU failure requires executor restoration/quarantine and must NOT replay the
// original draw as if no replacement was submitted.
class CommandState final
{
public:
    explicit CommandState(std::shared_ptr<const Plan> plan);
    ~CommandState();
    CommandState(const CommandState&) = delete;
    CommandState& operator=(const CommandState&) = delete;
    const std::shared_ptr<const Plan>& PlanOwner() const noexcept;
    bool CanInitialize() const noexcept;

    // Preload stage only: initialize alias/file leases and persisted variables.
    bool InitializeResource(Id resource, Binding binding) noexcept;
    bool InitializeVariable(Id variable, float value) noexcept;
    bool ReadVariable(Id variable, float& value) const noexcept;
    bool ReadResource(Id resource, Binding& value) const noexcept;
    // Fixed complete-chord rising edges, smart 0/1 cycle and condition. Atomic
    // across this snapshot: failure rolls back values, edge latches and cursors.
    // Caller owns serialization and must be outside all draw/native pairs.
    bool ApplyKeyCycles(const std::array<bool, 4>& down, std::array<bool, 4>& previous,
                        std::array<int, 4>& cursors) noexcept;

    PrepareResult Prepare(Id program, Phase phase, const DrawContext& draw,
                          const CommandQueries& queries) noexcept;
    // Shader overrides and the selected component can share one physical draw
    // boundary. Supply their exact ordered roots here; PRE and POST must have
    // the same roots/context. No external reentrant command callback is needed.
    PrepareResult PrepareSequence(const ProgramInvocation* programs, std::uint32_t count,
                                  Phase phase, const DrawContext& draw,
                                  const CommandQueries& queries) noexcept;
    // Preferred Draw/DrawIndexed replacement path: resolves PRE and POST into one
    // transaction BEFORE any GPU effect. The tape includes the exact original
    // draw boundary, so POST cannot discover an unknown dependency after skip.
    // Only eligible when the identity layer proves queried resources stay stable
    // across this draw (GPU-written/unknown identities are unsupported).
    // If requiredProgram is set, the complete tape must actually reach that
    // entry; otherwise it rolls back before any GPU effect. The per-program
    // epoch stamps are preallocated, not a draw-time dependency scan.
    // Every requiredOverrides entry must be reached by a real PRE Check of
    // its exact target/binding. One missing entry rolls back the whole tape.
    PrepareResult PrepareDraw(const ProgramInvocation* programs, std::uint32_t count,
                              const DrawContext& draw, const CommandQueries& queries,
                              Id requiredProgram = InvalidId,
                              const OverrideRequirement* requiredOverrides = nullptr,
                              std::uint32_t requiredOverrideCount = 0,
                              const std::uint8_t* observationPrograms = nullptr,
                              const std::uint8_t* selectionPrograms = nullptr) noexcept;
    // The load-time checker covers every branch of both pure-CPU phases. PRE
    // is transactional. Invariant POST (only TIME/variables/literals) can also
    // be preflighted before PRE commits. Window/DPI-dependent POST is evaluated
    // transactionally only after the actual return supplies its real snapshot.
    // Complete(false) retains PRE without executing or publishing any POST.
    PrepareResult BeginVariablePresent(const ProgramInvocation*, std::uint32_t count,
                                      const NativePresentValues&, bool preflightPost) noexcept;
    bool CompleteVariablePresent(bool actualReturn, const NativePresentValues& = {}) noexcept;
    // Native GPU phases remain pending until complete preflight/reservation/
    // submission. PRE opens the real-return token; POST admits private clears
    // and finite parameter-page writes only. Accept closes POST atomically.
    // Discard rolls back semantic slots, never releases submitted GPU owners.
    PrepareResult PrepareNativePost(const NativePresentValues&, const CommandQueries&) noexcept;
    PrepareResult PrepareNativePresent(const ProgramInvocation*, std::uint32_t,
                                      const NativePresentValues&, const CommandQueries&) noexcept;
    bool AcceptNativePresent() noexcept;
    void DiscardNativePresent() noexcept;
    void Accept() noexcept;
    void Discard() noexcept;
    // Call once after the physical original/replacement draw boundary and POST
    // processing. Prevents a POST belonging to another entry/invocation.
    bool FinishInvocation() noexcept;

private:
    struct Storage;
    std::unique_ptr<Storage> state_;
};
} // namespace gimi
