#pragma once

#include "GimiDx11Executor.h"
#include "GimiShaderSource.h"
#include <string>
#include <vector>

namespace OptiInput { struct KeyboardFrame; }

namespace gimi::runtime
{
// Explicit ingress, not a discovered character/LOD or shader ABI.
struct ComponentSelection
{
    std::string component, trigger;
    bool operator==(const ComponentSelection&) const = default;
};
// Selection capacity is independent of the existing 32 requirements per tape:
// one package can have more parts than one physical draw can consume. Vodyanitsa
// plus Mermaid has 33 actionable resource entries. No implicit part discovery.
inline constexpr std::uint32_t MaxComponentSelections = 64;
// Legacy component accepts 1..64 exact names separated by '|' at one trigger,
// within the existing 255-byte UI field. Qualified tokens instead declare
// vb0@canonicalName|vb1@canonicalName|ib@canonicalName (at most 8192 bytes).
// Tokens must be all qualified or all unqualified. API callers can instead use
// entries below with component empty. All parsing happens outside the hook.
// Each live matched resource entry needs a real PRE Check of its exact original
// binding in ONE complete transactional tape. No discovery or automatic callback
// is granted by selection. Real shader entries come from explicitly loaded INIs.
// One ShaderOverride retains vs/ps; mixed/multiple shader roots reject.
struct Selection
{
    bool enabled = false;
    // Explicit INI file, or explicit MOD directory (all descendant .ini files
    // required, no inferred DISABLED exclusions). Empty never means cwd.
    // Directory defaults are relative-path namespaces, including the filename;
    // entries must use those exact canonical names. No override auto-selection.
    std::string ini, component, trigger = "ib";
    // Optional explicit absolute dependency INIs separated by '|'. Empty adds
    // no libraries. Parsed once at Present preparation.
    std::string libraries;
    // Optional explicit ORIGINAL XXMI root INI, for first-time read-only import.
    // Empty means declaration defaults. Never infer authority from a component.
    // F6 always owns an independent LocalAppData sidecar, never XXMI output.
    std::string importRoot;
    // Optional structured equivalent of the qualified list. 1..64 distinct
    // canonical names, each at most 255 bytes, at explicit ib/vb0..7/ps-t0..127.
    // Appended to preserve existing aggregate callers and default-off behavior.
    std::vector<ComponentSelection> entries;
    // Optional explicit original XXMI directory for upstream default namespace
    // identity. Applies to the selected MOD and explicit libraries/includes;
    // all must be inside it, but their IO sandboxes do not expand. This only
    // derives names. Host import below is the separate explicit authorization
    // for original root declarations, core and the selected MOD only.
    // Appended for source-compatible aggregate callers. Empty keeps old names.
    std::string namespaceRoot;
    // Original root INI; imports its real declarations through F6's explicit
    // host adapter. Optional namespaceRoot must then equal its parent directory.
    // Empty preserves the existing standalone/library contract.
    std::string hostRoot;
};
void Configure(Selection selection, bool reload = false);
// Existing menu input owner only, after BeginFrame and before EndFrame. No IO.
void InputFrame(const OptiInput::KeyboardFrame&, bool accepting) noexcept;
Selection Selected();
bool Enabled() noexcept; // FIRST operation at every optional draw/event entry
bool NeedsPresent() noexcept; // includes disabled/replaced submitted owners
void StopAccepting() noexcept; // hook unload: preserve pending Present owners

// Supplied by GenericDepth: uses its existing internal suppression and real
// tracking-only MOD before/after callbacks. Never a second hook/renderer.
struct HookServices
{
    bool (*enter)(ID3D11DeviceContext*, std::uint64_t&) noexcept = nullptr;
    bool (*internal)(ID3D11DeviceContext*) noexcept = nullptr;
    void (*leave)(ID3D11DeviceContext*, std::uint64_t) noexcept = nullptr;
    bool (*before)(ID3D11DeviceContext*, UINT, UINT, INT, bool) noexcept = nullptr;
    bool (*after)(ID3D11DeviceContext*, UINT, UINT, INT, bool) noexcept = nullptr;
    bool (*beforeDraw)(ID3D11DeviceContext*, UINT vertexCount, UINT firstVertex,
                       bool originalBoundary) noexcept = nullptr;
    bool (*afterDraw)(ID3D11DeviceContext*, UINT vertexCount, UINT firstVertex,
                      bool originalBoundary) noexcept = nullptr;
    // Both authentic trampolines are required for a command tape that mixes
    // Draw and DrawIndexed; never route back through the public hook.
    Dx11OriginalDraw draw = nullptr;
    Dx11OriginalDrawIndexed drawIndexed = nullptr;
    // Optional authentic bridge, appended to preserve existing aggregate setup.
    // Root must wire real per-instance observer and trampoline; no vtable guess.
    Dx11OriginalDrawIndexedInstanced drawIndexedInstanced = nullptr;
    bool (*beforeIndexedInstanced)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT, bool) noexcept = nullptr;
    bool (*afterIndexedInstanced)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT, bool) noexcept = nullptr;
};
// Coverage is the existing committed common/write/context-boundary hook set;
// creation coverage is independently reported by D3D11_Hooks after commit.
void DeviceCreated(ID3D11Device*, ID3D11DeviceContext*, HookServices, bool coverage) noexcept;
void CreationCoverage(ID3D11Device*, bool complete) noexcept;
// Initializer/Present only, one attempt per capture generation and Configure revision.
// Claim/complete release owner/request locks across COM and the owned transaction.
struct CreationHookRequest
{
    std::uint64_t revision = 0, generation = 0;
};
CreationHookRequest ClaimCreationHooks(ID3D11Device*, ID3D11DeviceContext*) noexcept;
void CompleteCreationHooks(ID3D11Device*, ID3D11DeviceContext*, CreationHookRequest, LONG result) noexcept;
// Creation hook calls Prepare BEFORE the authentic CreateBuffer, keeping token
// alive through BufferCreated. No descriptor retrofit on already-created buffers.
void PrepareBufferCreation(ID3D11Device*, const D3D11_BUFFER_DESC*,
                           const D3D11_SUBRESOURCE_DATA*, BufferCreationEdit&) noexcept;
void BufferCreated(ID3D11Device*, ID3D11Buffer*, const D3D11_BUFFER_DESC*,
                   const D3D11_SUBRESOURCE_DATA*, const BufferCreationEdit* = nullptr) noexcept;
// False closes MOD for an unsupported resized view; caller preserves the
// original real API call/range. Never label that fallback as compatible resize.
bool PrepareUnorderedView(ID3D11Device*, ID3D11Resource*, const D3D11_UNORDERED_ACCESS_VIEW_DESC*,
                          D3D11_UNORDERED_ACCESS_VIEW_DESC&, bool& changed) noexcept;
void UnorderedViewCreated(ID3D11Device*, ID3D11Resource*, ID3D11UnorderedAccessView*) noexcept;
void GeometryShaderCreated(ID3D11Device*, ID3D11GeometryShader*, const void*, SIZE_T,
                           ID3D11ClassLinkage*, const D3D11_SO_DECLARATION_ENTRY*, UINT,
                           const UINT*, UINT, UINT rasterizedStream, bool withStreamOutput) noexcept;
// Public context hook owner must publish complete actual SO setter, every Draw
// kind, ClearState and opaque/deferred coverage before SO execution is admitted.
void StreamOutputCoverage(ID3D11Device*, ID3D11DeviceContext*, bool) noexcept;
void StreamOutputTargets(ID3D11DeviceContext*, UINT, ID3D11Buffer* const*, const UINT*) noexcept;
void StreamOutputDrawn(ID3D11DeviceContext*, UINT count, UINT instances = 1) noexcept;
void StreamOutputUnknownDraw(ID3D11DeviceContext*) noexcept;
void StreamOutputCleared(ID3D11DeviceContext*) noexcept;
void TextureCreated(ID3D11Device*, ID3D11Texture2D*, const D3D11_TEXTURE2D_DESC*,
                    const D3D11_SUBRESOURCE_DATA*) noexcept;
void ViewCreated(ID3D11Device*, ID3D11Resource*, ID3D11ShaderResourceView*) noexcept;
void ShaderCreated(ID3D11Device*, ID3D11DeviceChild*, const void*, SIZE_T,
                   bool vertex, bool hasLinkage) noexcept;
// Present/toolchain preparation ONLY (takes the owner lock). Exact original
// shader + lease/generation/hash, never pointer-only lookup or a draw query.
// Ready means retained original bytes/signature, NOT patch/consumer support.
// Missing/late capture, linkage, size and budget failure never authorize patching.
ShaderSourceResult AcquireShaderSource(ID3D11Device*, ID3D11DeviceChild*, ShaderIdentity original) noexcept;
void LayoutCreated(ID3D11Device*, ID3D11InputLayout*, const D3D11_INPUT_ELEMENT_DESC*, UINT) noexcept;
// Constant-time metadata invalidation; independently revokes current CB bytes.
// Optional content transactions use only legal CPU input or an owned shadow.
class CpuContentSubmission final
{
    std::shared_ptr<void> capture_, resource_, source_;
    std::uint64_t revision_ = 0, contract_ = 0, sourceRevision_ = 0;
    UINT begin_ = 0, end_ = 0;
    DWORD thread_ = 0;
public:
    CpuContentSubmission() noexcept = default;
    static bool HasPendingMaps() noexcept; // one atomic; no Store/COM/allocation
    explicit operator bool() const noexcept { return static_cast<bool>(resource_); }
    CpuContentSubmission(const CpuContentSubmission&) = delete;
    CpuContentSubmission& operator=(const CpuContentSubmission&) = delete;
    CpuContentSubmission(CpuContentSubmission&&) noexcept = default;
    CpuContentSubmission& operator=(CpuContentSubmission&&) noexcept = default;
    static CpuContentSubmission Update(ID3D11DeviceContext*, ID3D11Resource*, UINT sub,
                                      const D3D11_BOX*, const void*, UINT flags = 0) noexcept;
    static CpuContentSubmission Copy(ID3D11DeviceContext*, ID3D11Resource* destination, ID3D11Resource* source,
                                    UINT sub = 0, UINT x = 0, UINT y = 0, UINT z = 0, UINT sourceSub = 0,
                                    const D3D11_BOX* = nullptr, UINT flags = 0, bool whole = true) noexcept;
    // Mandatory shadow writeback survives config off/device-owner replacement.
    // Take BEFORE all original Unmap owners; Returned only after real Unmap.
    static CpuContentSubmission Unmap(ID3D11DeviceContext*, ID3D11Resource*, UINT sub, bool observe = true) noexcept;
    void Returned() noexcept;
};
// Called LAST after existing successful Map owners. knownCpuShadow describes
// only the legal memory source, never Genshin numerical/payload authority.
void ConstantMapped(ID3D11DeviceContext*, ID3D11Resource*, UINT sub, D3D11_MAP, UINT flags,
                    D3D11_MAPPED_SUBRESOURCE*, HRESULT, std::uint64_t mapGeneration, bool knownCpuShadow) noexcept;
void Written(ID3D11DeviceContext*, ID3D11Resource*) noexcept;
void ViewWritten(ID3D11DeviceContext*, ID3D11View*) noexcept;
// Exact existing Map/Unmap boundaries. Begin is before the actual API; failed
// Map clears the pending latch, successful Map keeps it until actual Unmap.
std::uint64_t VertexMapStarting(ID3D11DeviceContext*, ID3D11Resource*, UINT subresource, D3D11_MAP) noexcept;
void VertexMapFailed(ID3D11DeviceContext*, ID3D11Resource*, UINT subresource, std::uint64_t generation) noexcept;
void VertexUnmapped(ID3D11DeviceContext*, ID3D11Resource*, UINT subresource) noexcept;
void OpaqueBoundary(ID3D11DeviceContext*) noexcept;
// consumed=true is authoritative even after partial submission failure.
// Only the existing outer original hook owns this return receipt. A nested
// or consumed replacement cannot close it. No Store/lock when unarmed.
class RasterReturnScope final
{
    ID3D11DeviceContext* context_ = nullptr;
    std::uint64_t ticket_ = 0;
public:
    explicit RasterReturnScope(ID3D11DeviceContext* c) noexcept : context_(c) {}
    ~RasterReturnScope();
    RasterReturnScope(const RasterReturnScope&) = delete;
    RasterReturnScope& operator=(const RasterReturnScope&) = delete;
    bool Arm(ID3D11DeviceContext*, std::uint64_t) noexcept;
    void Returned() noexcept;
private:
    void Complete(bool) noexcept;
};
bool TryDrawIndexed(ID3D11DeviceContext*, UINT, UINT, INT, Dx11OriginalDrawIndexed,
                    RasterReturnScope* = nullptr) noexcept;
bool TryDrawIndexedInstanced(ID3D11DeviceContext*, UINT count, UINT instances, UINT first, INT base,
                            UINT firstInstance, Dx11OriginalDrawIndexedInstanced, RasterReturnScope* = nullptr) noexcept;
bool TryDraw(ID3D11DeviceContext*, UINT vertexCount, UINT firstVertex, Dx11OriginalDraw original, RasterReturnScope* = nullptr) noexcept;
// Call on the authentic existing Present lifecycle even after config off.
// At most one Signal per owner with actual unsealed MOD packets; never waits.
void Present(ID3D11Device*, ID3D11DeviceContext*, UINT width, UINT height) noexcept;

// Real native D3D11 Present/Present1 invocation only. The existing outer
// owner calls Begin before its overlay, then Returned ONLY after its original
// call has actually returned. No original callback is owned or called here.
// TEST is ignored before COM/preparation. No guessed provider/generated route.
// Destruction cancels an outstanding PRE; it never dispatches POST or marks GPU
// completion. The token retains exact COM and session generations across calls.
class NativePresentScope final
{
public:
    NativePresentScope() noexcept = default;
    ~NativePresentScope() noexcept;
    NativePresentScope(const NativePresentScope&) = delete;
    NativePresentScope& operator=(const NativePresentScope&) = delete;
    void Begin(IDXGISwapChain*, UINT sync, UINT flags, bool present1) noexcept;
    void Returned(IDXGISwapChain*, UINT sync, UINT flags, bool present1, HRESULT) noexcept;
private:
    void Finish(bool returned, IDXGISwapChain*, UINT, UINT, bool, HRESULT) noexcept;
    std::shared_ptr<void> session_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapchain_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    std::uint64_t ticket_ = 0, deviceGeneration_ = 0, planGeneration_ = 0, revision_ = 0;
    DWORD thread_ = 0, windowThread_ = 0;
    HWND window_ = nullptr; // observed identity, never a retained HWND lifetime lease
    NativePresentValues preValues_;
    PresentValueRequirements valueRequirements_{};
    UINT sync_ = 0, flags_ = 0;
    bool present1_ = false;
};

struct NativePresentObservation
{
    std::uint64_t pre = 0, post = 0, cancelled = 0, rejected = 0;
    std::uint64_t invocation = 0, deviceGeneration = 0, planGeneration = 0;
    std::uintptr_t swapchain = 0, device = 0, context = 0; // memory-only observations, not leases
    DWORD thread = 0;
    UINT sync = 0, flags = 0;
    bool present1 = false, returned = false;
    HRESULT result = S_OK; // meaningful only when returned is true; not GPU completion
};
struct Observation
{
    char message[768]{};
    std::uint64_t matched = 0, executed = 0, unknown = 0, busy = 0;
    std::uint32_t pending = 0, quarantined = 0;
    NativePresentObservation nativePresent;
    char stateFile[2048]{}; // F6-owned output path, not XXMI user_config
    bool stateDirty = false, stateBlocked = false, stateImported = false;
    std::uint64_t stateLoadedUtc = 0, stateSavedUtc = 0; // UTC FILETIME units
    // Published only after real Load reaches its normal resource/state success
    // boundary; not game/pixel qualification. Source-tool fields are explicit.
    char hostRoot[2048]{};
    std::uint32_t hostNativeSettings = 0, hostSourceSettings = 0, hostInactiveSettings = 0;
    std::uint64_t inputSnapshots = 0, inputConsumed = 0, inputRejected = 0;
};
Observation Observe() noexcept;
} // namespace gimi::runtime
