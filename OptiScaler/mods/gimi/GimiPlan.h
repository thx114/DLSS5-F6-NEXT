#pragma once

#include "GimiConfig.h"
#include "GimiSkinningContract.h"
#include <array>
#include <utility>

namespace gimi
{
inline constexpr Id IniParameterVectors = 4096;
inline constexpr Id IniParameterScalars = IniParameterVectors * 4;
inline constexpr unsigned IniParameterRegister = 120;
struct IniParameterInitializer
{
    SourceLocation source;
    Id scalar = 0;
    float value = 0;
};
// Ordered startup alias snapshots. InvalidId is known null; every nonnull
// source is a canonical immutable resource, never another mutable alias.
struct ResourceInitializer
{
    SourceLocation source;
    Id destination = InvalidId, resource = InvalidId;
};
enum class Phase : std::uint8_t { Pre, Post };
enum class TargetKind : std::uint8_t { Null, Resource, VertexBuffer, IndexBuffer, PixelTexture, This, VertexShader, PixelShader, RenderTarget, DepthTarget, VertexTexture };
struct Target
{
    TargetKind kind = TargetKind::Null;
    Id index = 0;
};
inline bool operator==(Target a, Target b) noexcept { return a.kind == b.kind && a.index == b.index; }

// Fixed private-target contract: both TexFx faces (7 color + depth each),
// one replacement generation per alias, and the existing eight depth-copy seats.
inline constexpr unsigned OutputAliasCapacity = 16;
inline constexpr unsigned OutputAliasGenerations = 2;
inline constexpr unsigned DepthCopyCapacity = 8;
inline constexpr unsigned OutputTextureCapacity = OutputAliasCapacity * OutputAliasGenerations;
inline constexpr unsigned PrivateTextureCapacity = OutputTextureCapacity + DepthCopyCapacity;
inline constexpr std::uint64_t PrivateTextureByteBudget = 256ull * 1024 * 1024;
struct TouchMask
{
    std::uint32_t vertexBuffers = 0;
    std::array<std::uint64_t, 2> pixelTextures{};
    std::array<std::uint64_t, 2> vertexTextures{}; // distinct VS SRVs, never IA vertex buffers
    bool indexBuffer = false;
    bool dynamicThis = false;
    bool iniParameters = false;
    bool outputMetadata = false; // query only: never an OM setter permission
    bool depthCopy = false; // narrow oD -> custom alias GPU copy, no OM setter
    bool outputDescription = false, clearTargets = false;
    bool backbufferViewport = false; // explicit native o0=set_viewport bb, never an alias
    bool debugOverlay = false; // content-authenticated TexFx fullscreen reader
    bool rasterCustom = false; // exact scoped cull/front CustomShader
    bool outputReferences = false, outputSetters = false; // owning RTV/DSV only, never SRV permission
    std::uint32_t vertexConstantNulls = 0; // explicit null-only setters, no CB alias/read permission
};

enum class ResourceKind : std::uint8_t { Alias, FileBuffer, FileTexture, InlineBuffer, StructuredBuffer, PositionsBuffer };
struct ResourceSpec
{
    SourceLocation source;
    std::string name;
    ResourceKind kind = ResourceKind::Alias;
    std::filesystem::path filename;
    // Exact declarative metadata for the next preload stage. No guessed DXGI ABI.
    std::string format;
    std::uint32_t stride = 0;
    TouchMask bindings;
    std::string data; // exact inline payload, decoded once at preload
    std::uint32_t array = 0;
    Id outputSlot = InvalidId; // immutable copy_desc destination seat, assigned at compilation
};

struct VariableSpec
{
    SourceLocation source;
    std::string name;
    float initial = 0;
    bool persistent = false;
    bool local = false; // static lexical slot shared by the program's pre/post
};
// Parsed intent only. No keyboard event or persistent file lifecycle is
// authorized by the presence of a Key section in an immutable Plan.
struct KeyCycleSpec
{
    SourceLocation source;
    std::string name, chord;
    Id variable = InvalidId, condition = InvalidId;
    std::array<float, 2> values{0, 1};
};

enum class ExprOp : std::uint8_t
{
    Literal, Variable, Parameter, Filter, FirstIndex, IndexCount, VertexCount,
    InstanceCount, FirstVertex, FirstInstance,
    Negate, Positive, Not, Add, Subtract, Multiply, Divide, FloorDivide, Modulo, Power,
    Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual, And, Or,
    NativeValue, // appended: existing operand/operator IDs stay stable
    DrawType // original physical DrawCallInfo type, not a replacement action
};
constexpr bool ExpressionOperand(ExprOp op) noexcept
{ return op <= ExprOp::FirstInstance || op == ExprOp::NativeValue || op == ExprOp::DrawType; }
constexpr bool DrawExpressionOperand(ExprOp op) noexcept
{ return (op >= ExprOp::Parameter && op <= ExprOp::FirstInstance) || op == ExprOp::DrawType; }
// Authentic native Present operands; not draw defaults or immutable IniParams.
enum class NativeValue : std::uint8_t
{
    Time, WindowWidth, WindowHeight, EffectiveDpi,
    RenderTargetWidth, RenderTargetHeight, ResolutionWidth, ResolutionHeight, CursorWindowX, CursorWindowY, CursorHotspotX, CursorHotspotY, CursorShowing, Count
};
static_assert(static_cast<unsigned>(NativeValue::Count) <= 16, "Native Present mask capacity");
constexpr std::uint16_t NativeValueBit(NativeValue value) noexcept
{ return static_cast<std::uint16_t>(1u << static_cast<unsigned>(value)); }

struct ExprToken
{
    ExprOp op = ExprOp::Literal;
    float literal = 0;
    Id index = 0;
    Target target;
};
struct Expression
{
    SourceLocation source;
    std::vector<ExprToken> code;
    std::uint32_t stack = 0;
};

enum class CommandOp : std::uint8_t { Assign, SetParameter, CopyDepth, Reference, Run, Check, Skip, DrawIndexed, Draw, BranchFalse, Jump, FormatText, RenderText, CopyDescription, ClearTarget, DrawIndexedInstanced, DrawIndexedAuto, DrawIndexedInstancedAuto, DrawFromCaller, DebugOverlay, BackbufferViewport, MouseDraw, UnbindVertexConstant, StorePixelConstant };
struct Command
{
    SourceLocation source;
    CommandOp op = CommandOp::Skip;
    Id index = InvalidId; // variable/parameter/program or jump destination
    std::array<Id, 5> expressions{InvalidId, InvalidId, InvalidId, InvalidId, InvalidId};
    Target destination, sourceTarget;
    Id constantSlot = InvalidId, constantFloat = InvalidId; // store: scalar index, NOT float4 index
    bool unlessNull = false;
    bool together = false; // explicit pre/post run/check executes callee PRE then POST
};

// An entirely empty CustomShader is a retained declaration, not a compute
// contract. It has no state overrides, commands, GPU actions or resource uses.
enum class ProgramKind : std::uint8_t { CommandList, TextureOverride, ShaderOverride, Present, ShaderRegex, FormatText, RenderText, RasterCustom, DebugOverlay, EmptyCustomShader, SoftwareMouse, SoftwareMouseBW };
// Exact reflected models, never guessed from a shader name/hash. Unknown models
// cannot link a model-only group. Pattern/bytecode transformation is separate.
enum class ShaderModel : std::uint8_t { Unknown, VS40, VS41, VS50, VS51, PS40, PS41, PS50, PS51 };
struct ShaderRegexSection
{
    SourceLocation source;
    std::string name;
    std::vector<ConfigEntry> lines; // original raw lines/source, not regex-as-commands
};
struct ShaderRegexPattern
{
    SourceLocation source;
    std::string pattern, replacement; // raw lines concatenated, NOT newline-joined
    bool replace = false;
    SourceLocation replacementSource; // authentic Pattern.Replace section
};
struct ShaderRegexGroup
{
    SourceLocation source;
    std::string name;
    Id program = InvalidId;
    std::vector<ShaderModel> models;
    std::vector<ShaderRegexSection> sections; // coherent main + dotted members
    std::vector<ShaderRegexPattern> patterns; // fixed upstream supports one Pattern
    std::vector<std::string> temps, declarations;
    bool hasFilterIndex = false;
    float filterIndex = 0;
    bool RequiresBytecode() const noexcept { return !patterns.empty() || !declarations.empty(); }
};
// One qualified compute contract, not a name whitelist. The compiler accepts
// the complete real CS declaration/bind/guard/clear/dispatch/unbind sequence.
struct FormatTextSpec
{
    SourceLocation source;
    std::filesystem::path shader;
    Id font = InvalidId, text = InvalidId, parameters = InvalidId, positions = InvalidId;
};
// Declarative shape of the fixed original GUI draw. Native admission still
// requires the real paired CS output, backbuffer, state and fence path.
struct RenderTextSpec
{
    SourceLocation source;
    std::filesystem::path shader;
    Id font = InvalidId, text = InvalidId, parameters = InvalidId, positions = InvalidId;
};
struct DebugOverlaySpec
{
    SourceLocation source;
    std::filesystem::path shader; // both stages, authenticated before device publication
};
struct SoftwareMouseSpec
{
    SourceLocation source;
    std::filesystem::path shader;
};
struct Program
{
    SourceLocation source;
    std::string name;
    ProgramKind kind = ProgramKind::CommandList;
    std::array<std::vector<Command>, 2> commands;
    Id mouse = InvalidId; // authenticated VS/PS index; BW inherits parent
    unsigned raster = 0; // 1: none/CCW, 2: back/CW, exact XXMI defaults otherwise
};

enum class OverrideMatchKind : std::uint8_t { Hash, BufferDescriptor, Texture2DDescriptor };
enum class UnsignedComparison : std::uint8_t { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };
inline bool CompareUnsigned(std::uint32_t actual, UnsignedComparison op, std::uint32_t value) noexcept
{
    switch (op)
    {
    case UnsignedComparison::Equal: return actual == value;
    case UnsignedComparison::NotEqual: return actual != value;
    case UnsignedComparison::Less: return actual < value;
    case UnsignedComparison::LessEqual: return actual <= value;
    case UnsignedComparison::Greater: return actual > value;
    case UnsignedComparison::GreaterEqual: return actual >= value;
    }
    return false;
}
struct OverrideSpec
{
    SourceLocation source;
    Id program = InvalidId;
    std::uint64_t hash = 0; // upstream uint32 resource / uint64 shader identity
    bool shader = false;
    OverrideMatchKind matchKind = OverrideMatchKind::Hash;
    // Exact resource Usage/BindFlags/Format, independent of view format/content.
    D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
    std::uint32_t bindFlags = 0;
    DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
    bool hasFirstIndex = false;
    std::uint32_t firstIndex = 0;
    bool hasIndexCount = false;
    std::uint32_t indexCount = 0;
    UnsignedComparison indexCountComparison = UnsignedComparison::Equal;
    bool MatchesDraw(bool indexed, std::uint32_t first, std::uint32_t count) const noexcept
    {
        return (indexed || (!hasFirstIndex && !hasIndexCount)) &&
            (!hasFirstIndex || firstIndex == first) &&
            (!hasIndexCount || CompareUnsigned(count, indexCountComparison, indexCount));
    }
    bool hasFilterIndex = false;
    float filterIndex = 0;
    std::int32_t priority = 0;
    bool overrule = false;
    BufferResizeRule resize; // creation-time only, never a draw action
};

struct ExecutionLimits
{
    std::uint32_t instructions = 32768;
    std::uint32_t actions = 1024;
    std::uint32_t callDepth = 32;
    std::uint32_t matchesPerCheck = 128;
    std::uint32_t expressionStack = 128;
};

struct CompileRequest
{
    std::shared_ptr<const Config> config;
    // Exact canonical names, available in Config::sections. No keyword matching.
    // Entries select the compilation closure. Runtime executes shader roots;
    // a selected TextureOverride must be reached through an actual Check.
    std::vector<std::string> entries;
    // All loaded files/includes already contribute their required declarations,
    // filters and automatic shader groups, beyond literal run references.
    // Additional explicit mandatory files must also exist in the loaded Config.
    std::vector<std::filesystem::path> triggerFiles;
    ExecutionLimits limits;
    std::uint32_t maxPrograms = 8192;
    std::uint32_t maxVariables = 16384;
    std::uint32_t maxResources = 8192;
    std::uint32_t maxCommands = 131072;
    std::uint32_t maxExpressionTokens = 1048576;
};

namespace detail { class Compiler; }
class CommandState;
class Plan final
{
    friend class detail::Compiler;
    friend class CommandState;
    Plan() = default;
    std::shared_ptr<const Config> config_;
    ExecutionLimits limits_;
    std::vector<Program> programs_;
    std::vector<Expression> expressions_;
    std::vector<VariableSpec> variables_;
    std::vector<KeyCycleSpec> keyCycles_;
    std::vector<IniParameterInitializer> initialParameters_; // ordered Constants writes
    std::vector<ResourceInitializer> initialResources_; // ordered static Constants alias snapshots
    std::vector<ResourceSpec> resources_;
    unsigned outputAliasCount_ = 0;
    std::vector<OverrideSpec> overrides_; // shader/match-kind/hash/priority/name ordered at Load
    std::vector<ShaderRegexGroup> shaderRegex_; // canonical group-name order
    std::vector<Id> overrideIndices_;
    std::vector<Id> entries_;
    std::vector<FormatTextSpec> formatText_;
    std::vector<RenderTextSpec> renderText_;
    std::vector<DebugOverlaySpec> debugOverlays_;
    std::vector<SoftwareMouseSpec> softwareMouse_;
    bool hideCursor_ = false;
    std::vector<std::uint8_t> nativeOnly_; // never reachable from any raster root
    TouchMask touches_;
public:
    Plan(const Plan&) = delete;
    Plan& operator=(const Plan&) = delete;
    const auto& Programs() const noexcept { return programs_; }
    const auto& Expressions() const noexcept { return expressions_; }
    const auto& Variables() const noexcept { return variables_; }
    const auto& KeyCycles() const noexcept { return keyCycles_; }
    const auto& InitialParameters() const noexcept { return initialParameters_; }
    const auto& InitialResources() const noexcept { return initialResources_; }
    const auto& Resources() const noexcept { return resources_; }
    const auto& Overrides() const noexcept { return overrides_; }
    const auto& ShaderRegexGroups() const noexcept { return shaderRegex_; }
    const auto& Entries() const noexcept { return entries_; }
    const auto& FormatTexts() const noexcept { return formatText_; }
    const auto& RenderTexts() const noexcept { return renderText_; }
    const auto& DebugOverlays() const noexcept { return debugOverlays_; }
    const auto& SoftwareMouse() const noexcept { return softwareMouse_; }
    bool HideCursor() const noexcept { return hideCursor_; }
    bool NativeOnly(Id program) const noexcept { return program < nativeOnly_.size() && nativeOnly_[program]; }
    const auto& Limits() const noexcept { return limits_; }
    const auto& Touches() const noexcept { return touches_; }
    const auto& Configuration() const noexcept { return config_; }
    const OverrideSpec* OverrideForProgram(Id program) const noexcept
    {
        if (program >= overrideIndices_.size() || overrideIndices_[program] == InvalidId) return nullptr;
        return &overrides_[overrideIndices_[program]];
    }
    // Binary search only: provider filters the returned equal-hash range using
    // known identity/descriptor/index eligibility. Unknown identity NEVER matches.
    std::pair<const OverrideSpec*, std::size_t> Candidates(std::uint64_t hash, bool shader) const noexcept;
    // Preparation only. Resource owners cache the matching subset off-draw.
    std::pair<const OverrideSpec*, std::size_t> BufferDescriptorCandidates() const noexcept;
    std::pair<const OverrideSpec*, std::size_t> TextureDescriptorCandidates() const noexcept;
};

struct CompileResult
{
    std::shared_ptr<const Plan> plan;
    std::vector<Diagnostic> diagnostics;
    explicit operator bool() const noexcept { return plan != nullptr; }
};
CompileResult CompilePlan(const CompileRequest& request);
struct BufferResizeResult
{
    std::vector<BufferResizeRule> rules;
    std::vector<Diagnostic> diagnostics;
    explicit operator bool() const noexcept { return diagnostics.empty(); }
};
// Present/load only: arming prerequisites does not execute a partial character.
BufferResizeResult CompileBufferResizeRules(const Config&);
} // namespace gimi
