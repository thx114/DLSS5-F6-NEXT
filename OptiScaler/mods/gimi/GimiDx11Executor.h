#pragma once

#include "GimiDx11Resources.h"

namespace gimi
{
class Dx11ExecutionLease;

enum class Dx11ExecutionOutcome : std::uint8_t { Cancelled, Submitted, Quarantine };

// Implemented by the EXISTING hook/runtime owner, not a second renderer. Its
// reserved fixed-capacity slot owns the packet independently of the executor.
// CPU return is not GPU completion: Submitted/Quarantine retain it through the
// caller's real existing submission fence/retire path. UINT64_MAX is failure,
// not completion. Shutdown/failed Signal retains/quarantines it as usual.
class Dx11DrawOwner
{
public:
    virtual ~Dx11DrawOwner() = default;
    virtual bool Current(ID3D11Device*, ID3D11DeviceContext*, const DrawEpochs&) const noexcept = 0;
    // Atomically reserve GPU retention AND the ordered alias journal before any
    // setter/draw. Validate previous/new alias leases and pre-reserve persistent
    // alias ownership; Submitted cannot subsequently fail to publish ownership.
    // On false, retain nothing. On true, own packet until Cancelled (no GPU),
    // real completion (Submitted), or the existing quarantine teardown policy.
    // These callbacks must not allocate, issue GPU work or mutate GPU bindings.
    virtual bool Reserve(std::shared_ptr<const Dx11ExecutionLease> packet,
                         std::uint64_t& reservation) noexcept = 0;
    virtual void Finish(std::uint64_t reservation, Dx11ExecutionOutcome) noexcept = 0;

    // Enter/leave the ORIGINAL internal suppression used by GenericDepth
    // (saved t_genshinCapture/t_copying or its existing owner equivalent).
    // No executor-owned substitute TLS. A successful enter must be observable
    // by InternalActive; leave restores the saved nesting state.
    virtual bool EnterInternal(ID3D11DeviceContext*, std::uint64_t& nesting) noexcept = 0;
    virtual bool InternalActive(ID3D11DeviceContext*) const noexcept = 0;
    virtual void LeaveInternal(ID3D11DeviceContext*, std::uint64_t nesting) noexcept = 0;

    // Real MOD consumer observation for EVERY physical original/replacement
    // raster; integrate the existing Before/After depth/FG/NR observation here
    // even though the actual internal Draw uses the original trampoline.
    // They must not issue commands/change bindings/reenter/mutate the plan,
    // epochs, suppression or input identities. False before raster aborts with
    // NO open observation; true always pairs with a real draw and AfterRaster.
    // AfterRaster must finish observation even when returning false, which
    // quarantines the packet. These are tracking callbacks, not GPU handlers.
    virtual bool BeforeRaster(ID3D11DeviceContext*, UINT count, UINT first,
                              INT baseVertex, bool originalBoundary) noexcept = 0;
    virtual bool AfterRaster(ID3D11DeviceContext*, UINT count, UINT first,
                             INT baseVertex, bool originalBoundary) noexcept = 0;
    // Preflight capability only: no observation opens here. Nonindexed draws
    // must not be translated into an indexed event with a synthetic base.
    virtual bool SupportsIndexedInstanced() const noexcept { return false; }
    virtual bool BeforeIndexedInstanced(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT, bool) noexcept { return false; }
    virtual bool AfterIndexedInstanced(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT, bool) noexcept { return false; }
    virtual bool SupportsRaster(bool indexed) const noexcept { return indexed; }
    virtual bool BeforeDraw(ID3D11DeviceContext*, UINT, UINT, bool) noexcept { return false; }
    virtual bool AfterDraw(ID3D11DeviceContext*, UINT, UINT, bool) noexcept { return false; }
};

using Dx11OriginalDraw = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using Dx11OriginalDrawIndexed = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using Dx11OriginalDrawIndexedInstanced = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);

enum class Dx11ExecutionError : std::uint8_t
{
    None, Disabled, InvalidRequest, Busy, WrongOwner, WrongContext, Unsupported,
    UnsupportedParameter, Prepare, Tape, Capacity, Binding, Generation, Layout,
    Hazard, IndexBounds, VertexBounds, Retention, Suppression, Raster, OwnerChanged,
    OutlineMaterial, OutlineSampler, PixelMaterial, PixelSampler, VertexConstant, VertexShader
};
struct Dx11ExecutionResult
{
    Dx11ExecutionError error = Dx11ExecutionError::None;
    PrepareResult prepared;
    std::uint32_t action = 0, rasterCalls = 0;
    bool originalCalled = false, originalSkipped = false;
    // True only after consuming a real boundary, OR once any GPU operation may
    // have been issued. Caller must NEVER draw fallback original in that case,
    // including Quarantine/partial failure. Preflight failure leaves false.
    bool consumed = false, quarantined = false;
    Id vertexConstantSlot = InvalidId;
    const char* vertexConstantReason = nullptr; // static text, no draw allocation
    explicit operator bool() const noexcept { return error == Dx11ExecutionError::None; }
};

struct Dx11ExecutionRequest
{
    // The outer runtime MUST test the shared enable gate before owner locking,
    // resource lookup, interpreter work or allocation. This is a second early
    // check; a disabled call touches no context/state/provider/owner.
    bool enabled = false;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    CommandState* commands = nullptr;
    const ProgramInvocation* programs = nullptr;
    std::uint32_t programCount = 0;
    // The manually selected component must actually be reached by the shader
    // entry/check tape. A matching resource alone cannot authorize execution.
    Id requiredProgram = InvalidId;
    const OverrideRequirement* requiredOverrides = nullptr; // synchronous, at most 32
    std::uint32_t requiredOverrideCount = 0;
    DrawContext draw;
    CommandQueries queries;
    std::shared_ptr<const Dx11StaticResources> statics;
    std::uint32_t parameterState = 0; // exact committed immutable-page state
    Dx11PipelineBinding pipeline;
    Dx11ResourceProvider* resources = nullptr;
    Dx11DrawOwner* owner = nullptr;
    Dx11OriginalDrawIndexed original = nullptr;
    Dx11OriginalDraw originalDraw = nullptr;
    Dx11OriginalDrawIndexedInstanced originalInstanced = nullptr;
    // Optional immutable Plan-sized guard. An independent observation can
    // never execute an unselected resource replacement through Check or Run.
    const std::uint8_t* observationPrograms = nullptr;
    const std::uint8_t* selectionPrograms = nullptr; // Plan-sized, selected tape only
};

// Read-only submitted packet; buffers/arrays and their COM references were
// allocated at executor construction. Runtime Reserve receives the shared owner
// BEFORE the first GPU effect. It must retain it even if the executor dies.
// Alias data is available during Reserve for its persistent ownership journal.
class Dx11ExecutionLease final
{
    friend class Dx11Executor;
    struct Storage;
    std::unique_ptr<Storage> state_;
    explicit Dx11ExecutionLease(std::uint32_t maxActions);
public:
    ~Dx11ExecutionLease();
    Dx11ExecutionLease(const Dx11ExecutionLease&) = delete;
    Dx11ExecutionLease& operator=(const Dx11ExecutionLease&) = delete;
    const Dx11AliasChange* Aliases() const noexcept;
    std::uint32_t AliasCount() const noexcept;
    const Dx11RetainedBinding* Resources() const noexcept;
    std::uint32_t ResourceCount() const noexcept;
    const Dx11PipelineBinding& Pipeline() const noexcept;
    const std::shared_ptr<const Plan>& PlanOwner() const noexcept;
    const std::shared_ptr<const Dx11StaticResources>& ParameterOwner() const noexcept;
    std::uint32_t InitialParameters() const noexcept;
    std::uint32_t FinalParameters() const noexcept;
    const Dx11StreamOutputBinding& StreamOutput() const noexcept;
    const std::array<std::uint64_t, D3D11_SO_BUFFER_SLOT_COUNT>& StreamBytesIssued() const noexcept;
};

// One serial authenticated immediate-context owner. Construct at load/device
// preparation; no locks, IO, hash, readback, upload, fence, Flush, CPU wait or
// growing allocations in Execute. Exact immediate Draw/DrawIndexed only.
// Bounded original GS/SO only; no instanced/deferred/indirect/HS/DS/ORFix claim. Geometry is issued with
// the game's qualified shaders/layout; no dummy GPU handler or external loader.
class Dx11Executor final
{
    struct Storage;
    std::unique_ptr<Storage> state_;
public:
    // Failure to initialize throws only at this off-draw load boundary. Tape
    // capacity must cover Plan::Limits().actions; bounded in [1,4096], flights
    // in [1,16]. Exhaustion bypasses before touching the GPU, without a wait.
    explicit Dx11Executor(std::uint32_t maxActions = 1024, std::uint32_t flights = 4);
    ~Dx11Executor();
    Dx11Executor(const Dx11Executor&) = delete;
    Dx11Executor& operator=(const Dx11Executor&) = delete;
    Dx11ExecutionResult Execute(const Dx11ExecutionRequest&) noexcept;
};
} // namespace gimi
