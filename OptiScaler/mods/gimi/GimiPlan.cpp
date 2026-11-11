#include "GimiPlan.h"
#include "GimiExpression.h"
#include "GimiOutlineContract.h"
#include "GimiDiffuseContract.h"
#include "GimiVertexContract.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace gimi
{
namespace detail
{
struct CompileFailure { Diagnostic diagnostic; };
[[noreturn]] static void Fail(const SourceLocation& source, std::string message,
                             DiagnosticCode code = DiagnosticCode::Unsupported)
{
    throw CompileFailure{{code, source, std::move(message)}};
}

static bool Starts(std::string_view s, std::string_view prefix) { return s.compare(0, prefix.size(), prefix) == 0; }
static bool Integer(std::string_view s, std::uint32_t& value, int base = 10)
{
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), value, base);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}
static bool Parameter(std::string_view s, Id& id)
{
    if (s.empty()) return false;
    const auto component = std::string_view("xyzw").find(s[0]);
    std::uint32_t index = 0;
    if (component == std::string_view::npos || (s.size() > 1 && !Integer(s.substr(1), index)) || index >= IniParameterVectors)
        return false;
    id = index * 4 + static_cast<Id>(component);
    return true;
}
static bool Variable(std::string_view v)
{
    if (v.size() < 2 || v[0] != '$') return false;
    if (v.find('\\') == v.npos && v.find('-') != v.npos) return false;
    for (const auto c : v.substr(1))
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '\\' || c == '.' || c == '-'))
            return false;
    return true;
}

using Scope = std::vector<std::map<std::string, Id>>;
struct Operator { std::string_view token; int precedence; ExprOp op; bool right = false; };
// Versioned language table, deliberately independent of ORFix's character data.
static constexpr Operator Operators[] = {
    {"||", 1, ExprOp::Or}, {"&&", 2, ExprOp::And}, {"==", 3, ExprOp::Equal}, {"!=", 3, ExprOp::NotEqual},
    {"<=", 4, ExprOp::LessEqual}, {">=", 4, ExprOp::GreaterEqual}, {"<", 4, ExprOp::Less}, {">", 4, ExprOp::Greater},
    {"+", 5, ExprOp::Add}, {"-", 5, ExprOp::Subtract}, {"**", 7, ExprOp::Power, true},
    {"//", 6, ExprOp::FloorDivide}, {"*", 6, ExprOp::Multiply}, {"/", 6, ExprOp::Divide}, {"%", 6, ExprOp::Modulo}};

class ExpressionParser
{
    std::string text_;
    std::size_t pos_ = 0;
    SourceLocation source_;
    std::function<ExprToken(std::string_view)> operand_;
    std::vector<ExprToken> code_;
    std::uint32_t depth_ = 0, maxDepth_;

    void Space() { while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t')) ++pos_; }
    void Atom()
    {
        Space();
        if (pos_ == text_.size()) Fail(source_, "Missing expression operand", DiagnosticCode::InvalidSyntax);
        const char c = text_[pos_];
        if (c == '+' || c == '-' || c == '!')
        {
            ++pos_;
            Parse(8); // upstream unary binds more tightly than exponent
            code_.push_back({c == '+' ? ExprOp::Positive : c == '-' ? ExprOp::Negate : ExprOp::Not});
            return;
        }
        if (c == '(')
        {
            ++pos_; Parse(1); Space();
            if (pos_ == text_.size() || text_[pos_++] != ')') Fail(source_, "Unclosed expression parenthesis", DiagnosticCode::InvalidSyntax);
            return;
        }
        if ((c >= '0' && c <= '9') || c == '.')
        {
            // Float parsing intentionally treats leading zeros as decimal (ORFix
            // uses 037731.17). Locale-independent decimal/scientific literals.
            float value = 0;
            const auto parsed = std::from_chars(text_.data() + pos_, text_.data() + text_.size(), value, std::chars_format::general);
            if (parsed.ec != std::errc() || parsed.ptr == text_.data() + pos_ || !std::isfinite(value))
                Fail(source_, "Invalid/nonfinite numeric literal");
            pos_ = static_cast<std::size_t>(parsed.ptr - text_.data());
            code_.push_back({ExprOp::Literal, value});
            return;
        }
        const auto start = pos_;
        while (pos_ < text_.size())
        {
            const auto x = text_[pos_];
            if ((x >= 'a' && x <= 'z') || (x >= '0' && x <= '9') || x == '_' || x == '$' || x == '\\' || x == '.') ++pos_;
            // Stage SRV targets use ps-tN/vs-tN, not subtraction.
            else if (x == '-' && pos_ == start + 2 && (text_.compare(start, 2, "ps") == 0 || text_.compare(start, 2, "vs") == 0) &&
                     pos_ + 1 < text_.size() && text_[pos_ + 1] == 't') ++pos_;
            // A package-relative namespace may contain a hyphen in a filename;
            // only consume it inside the namespace portion, before a '\\'.
            else if (x == '-' && text_.find('\\', start) < pos_ &&
                     text_.find('\\', pos_ + 1) < text_.find_first_of(" \t()!<>=+*/%&|,$", pos_ + 1)) ++pos_;
            else break;
        }
        if (start == pos_) Fail(source_, "Unsupported expression token at column " + std::to_string(pos_ + 1));
        code_.push_back(operand_(std::string_view(text_).substr(start, pos_ - start)));
    }
    void Parse(int minimum)
    {
        if (++depth_ > maxDepth_) Fail(source_, "Expression nesting limit exceeded", DiagnosticCode::Limit);
        Atom();
        for (;;)
        {
            Space();
            const Operator* found = nullptr;
            // Longest operator wins, so unsupported === never becomes == plus =.
            for (const auto& op : Operators)
                if (text_.compare(pos_, op.token.size(), op.token) == 0 &&
                    (!found || op.token.size() > found->token.size())) found = &op;
            if (!found || found->precedence < minimum) break;
            pos_ += found->token.size();
            Parse(found->precedence + (found->right ? 0 : 1));
            code_.push_back({found->op});
        }
        --depth_;
    }
public:
    ExpressionParser(std::string_view text, SourceLocation source, std::uint32_t depth,
                     std::function<ExprToken(std::string_view)> operand)
        : text_(LowerAscii(text)), source_(std::move(source)), operand_(std::move(operand)), maxDepth_(depth) {}
    Expression Compile()
    {
        Parse(1); Space();
        if (pos_ != text_.size()) Fail(source_, "Unsupported expression syntax at column " + std::to_string(pos_ + 1));
        std::uint32_t current = 0, maximum = 0;
        for (const auto& token : code_)
        {
            if (ExpressionOperand(token.op)) ++current;
            else if (token.op <= ExprOp::Not) { if (!current) Fail(source_, "Invalid unary expression"); }
            else { if (current < 2) Fail(source_, "Invalid binary expression"); --current; }
            maximum = (std::max)(maximum, current);
        }
        if (current != 1 || maximum > maxDepth_) Fail(source_, "Expression stack limit exceeded", DiagnosticCode::Limit);
        return {source_, std::move(code_), maximum};
    }
};

class Compiler
{
    const CompileRequest& request_;
    std::shared_ptr<Plan> plan_{new Plan};
    std::map<std::string, const ConfigSection*> sections_;
    std::map<std::string, std::vector<const ConfigSection*>> regexSections_;
    std::map<std::string, ConfigEntry> declarations_;
    std::vector<const ConfigEntry*> constantOrder_;
    bool initializingConstants_ = false;
    std::map<std::string, Id> globals_, resources_, programs_;
    std::set<std::string> compilingGlobals_, compilingPrograms_;
    std::set<std::string> triggerFiles_, fixedKeySections_;
    std::uint32_t commandCount_ = 0, tokenCount_ = 0;
    std::uint32_t initializationCommands_ = 0;
    std::map<Id, Id> initialResourceOrigins_;
    std::set<std::string> initializingResourceLists_;

    const ConfigSection& Section(std::string name, const SourceLocation& at)
    {
        const auto found = sections_.find(LowerAscii(name));
        if (found == sections_.end()) Fail(at, "Missing section dependency: " + name, DiagnosticCode::MissingDependency);
        return *found->second;
    }
    std::string ResolveSection(std::string_view name, std::string_view ns, const SourceLocation& at)
    {
        const auto qualified = SectionName(name, ns);
        if (sections_.count(qualified)) return qualified;
        const auto plain = LowerAscii(name);
        if (sections_.count(plain)) return plain;
        Fail(at, "Missing section dependency: " + plain, DiagnosticCode::MissingDependency);
    }
    Id Global(std::string name, const SourceLocation& at)
    {
        if (const auto existing = globals_.find(name); existing != globals_.end()) return existing->second;
        const auto declaration = declarations_.find(name);
        if (declaration == declarations_.end()) Fail(at, "Unknown variable dependency: " + name, DiagnosticCode::MissingDependency);
        if (!compilingGlobals_.insert(name).second) Fail(at, "Global initializer cycle: " + name, DiagnosticCode::Cycle);
        if (compilingGlobals_.size() > request_.limits.callDepth) Fail(at, "Global initializer depth exceeded", DiagnosticCode::Limit);
        const auto& e = declaration->second;
        Scope empty(1);
        const Id expression = Expr(e.value.empty() ? "0" : e.value, e, empty, true);
        const auto initial = Constant(plan_->expressions_[expression], at);
        if (plan_->variables_.size() >= request_.maxVariables) Fail(at, "Variable limit exceeded", DiagnosticCode::Limit);
        const Id id = static_cast<Id>(plan_->variables_.size());
        plan_->variables_.push_back({e.source, name, initial, Starts(e.key, "global persist "), false});
        globals_.emplace(name, id);
        compilingGlobals_.erase(name);
        return id;
    }
    Id Var(std::string_view name, const ConfigEntry& at, const Scope& scope)
    {
        for (auto i = scope.rbegin(); i != scope.rend(); ++i)
            if (const auto found = i->find(std::string(name)); found != i->end()) return found->second;
        const auto qualified = VariableName(name, at.nameSpace);
        const auto resolved = declarations_.count(qualified) ? qualified : LowerAscii(name);
        if ((initializingConstants_ || !compilingGlobals_.empty()) && !globals_.count(resolved))
            Fail(at.source, "Global static initializer references a not-yet-initialized variable: " + resolved);
        return Global(resolved, at.source);
    }
    void Touch(Target target)
    {
        auto& touch = plan_->touches_;
        if (target.kind == TargetKind::VertexBuffer) touch.vertexBuffers |= std::uint32_t{1} << target.index;
        else if (target.kind == TargetKind::IndexBuffer) touch.indexBuffer = true;
        else if (target.kind == TargetKind::PixelTexture) touch.pixelTextures[target.index / 64] |= std::uint64_t{1} << (target.index % 64);
        else if (target.kind == TargetKind::VertexTexture) touch.vertexTextures[target.index / 64] |= std::uint64_t{1} << (target.index % 64);
        else if (target.kind == TargetKind::This) touch.dynamicThis = true;
    }
    Target TargetOf(std::string_view text, const ConfigEntry& at, bool shader = false)
    {
        const auto name = LowerAscii(Trim(text));
        std::uint32_t index = 0;
        if (name == "null") return {};
        if (name == "this") return {TargetKind::This, 0};
        if (name == "ib") return {TargetKind::IndexBuffer, 0};
        if (name == "od") { plan_->touches_.outputMetadata = true; return {TargetKind::DepthTarget, 0}; }
        if (Starts(name, "o") && Integer(std::string_view(name).substr(1), index) && index < 8)
        { plan_->touches_.outputMetadata = true; return {TargetKind::RenderTarget, index}; }
        if (shader && name == "vs") return {TargetKind::VertexShader, 0};
        if (shader && name == "ps") return {TargetKind::PixelShader, 0};
        if (Starts(name, "vb") && Integer(std::string_view(name).substr(2), index) && index < 32)
            return {TargetKind::VertexBuffer, index};
        if (Starts(name, "ps-t") && Integer(std::string_view(name).substr(4), index) && index < 128)
            return {TargetKind::PixelTexture, index};
        if (Starts(name, "vs-t") && Integer(std::string_view(name).substr(4), index) && index < 128)
            return {TargetKind::VertexTexture, index};
        if (Starts(name, "resource")) return {TargetKind::Resource, Resource(ResolveSection(name, at.nameSpace, at.source), at.source)};
        Fail(at.source, "Unsupported resource target: " + name);
    }
    Id Expr(std::string_view value, const ConfigEntry& at, const Scope& scope, bool constant = false)
    {
        auto expression = ExpressionParser(value, at.source, request_.limits.expressionStack, [&](std::string_view token) -> ExprToken {
            if (token == "true") return {ExprOp::Literal, 1};
            if (token == "false") return {ExprOp::Literal, 0};
            if (!token.empty() && token[0] == '$') return {ExprOp::Variable, 0, Var(token, at, scope)};
            if (constant) Fail(at.source, "Global initializers may only use constants and declared global variables");
            Id parameter;
            if (Parameter(token, parameter))
            {
                plan_->touches_.iniParameters = true;
                return {ExprOp::Parameter, 0, parameter};
            }
            constexpr std::pair<std::string_view, ExprOp> builtins[] = {{"first_index", ExprOp::FirstIndex},
                {"index_count", ExprOp::IndexCount}, {"vertex_count", ExprOp::VertexCount}, {"instance_count", ExprOp::InstanceCount},
                {"first_vertex", ExprOp::FirstVertex}, {"first_instance", ExprOp::FirstInstance}, {"draw_type", ExprOp::DrawType}};
            for (const auto& builtin : builtins) if (token == builtin.first) return {builtin.second};
            constexpr std::pair<std::string_view, NativeValue> native[] = {
                {"time", NativeValue::Time}, {"window_width", NativeValue::WindowWidth},
                {"window_height", NativeValue::WindowHeight}, {"effective_dpi", NativeValue::EffectiveDpi},
                {"rt_width", NativeValue::RenderTargetWidth}, {"rt_height", NativeValue::RenderTargetHeight},
                {"res_width", NativeValue::ResolutionWidth}, {"res_height", NativeValue::ResolutionHeight},
                {"cursor_window_x", NativeValue::CursorWindowX}, {"cursor_window_y", NativeValue::CursorWindowY},
                {"cursor_hotspot_x", NativeValue::CursorHotspotX}, {"cursor_hotspot_y", NativeValue::CursorHotspotY},
                {"cursor_showing", NativeValue::CursorShowing}};
            for (const auto& operand : native)
                if (token == operand.first)
                {
                    if (operand.second == NativeValue::RenderTargetWidth || operand.second == NativeValue::RenderTargetHeight)
                        plan_->touches_.outputMetadata = true;
                    return {ExprOp::NativeValue, 0, static_cast<Id>(operand.second)};
                }
            return {ExprOp::Filter, 0, 0, TargetOf(token, at, true)};
        }).Compile();
        if (expression.code.size() > request_.maxExpressionTokens - tokenCount_)
            Fail(at.source, "Expression token budget exceeded", DiagnosticCode::Limit);
        tokenCount_ += static_cast<std::uint32_t>(expression.code.size());
        const Id id = static_cast<Id>(plan_->expressions_.size());
        plan_->expressions_.push_back(std::move(expression));
        return id;
    }
    // Global initialization has no runtime callbacks and evaluates in float32.
    float Constant(const Expression& expression, const SourceLocation& at)
    {
        std::vector<float> stack;
        stack.reserve(expression.stack);
        for (const auto& t : expression.code)
        {
            if (t.op == ExprOp::Literal) stack.push_back(t.literal);
            else if (t.op == ExprOp::Variable) stack.push_back(plan_->variables_[t.index].initial);
            // Appended/live operands have no value in startup constants.
            // Reject before any unary/binary stack access, including draw_type.
            else if (ExpressionOperand(t.op)) Fail(at, "Nonconstant global initializer");
            else if (t.op <= ExprOp::Not)
            {
                if (!Unary(t.op, stack.back())) Fail(at, "Nonconstant global initializer");
            }
            else
            {
                const float rhs = stack.back(); stack.pop_back();
                float& lhs = stack.back();
                if (!Binary(t.op, lhs, rhs)) Fail(at, "Nonconstant global initializer");
            }
        }
        if (stack.size() != 1 || !std::isfinite(stack.back())) Fail(at, "Invalid/nonfinite global initializer");
        return stack.back();
    }
    Id Resource(const std::string& name, const SourceLocation& at)
    {
        if (const auto found = resources_.find(name); found != resources_.end()) return found->second;
        const auto& section = Section(name, at);
        triggerFiles_.insert(LowerAscii(PathUtf8(section.source.file)));
        ResourceSpec spec{section.source, name};
        std::set<std::string> keys;
        std::string type;
        for (const auto& e : section.entries)
        {
            if (!e.hasEquals || !keys.insert(e.key).second) Fail(e.source, "Malformed/duplicate Resource property: " + e.key);
            if (e.key == "filename")
            {
                auto value = e.value;
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
                std::replace(value.begin(), value.end(), '\\', '/');
                spec.filename = std::filesystem::weakly_canonical(e.source.file.parent_path() / std::filesystem::u8path(value));
                const auto* root = request_.config->RootFor(e.source.file);
                if (!root || root->empty()) Fail(e.source, "Resource source has no admitted package root");
                // Windows root components are case insensitive. Canonical path
                // spelling alone need not match the INI's filename spelling.
                auto a = root->begin(), b = spec.filename.begin();
                for (; a != root->end(); ++a, ++b)
                    if (b == spec.filename.end() || LowerAscii(PathUtf8(*a)) != LowerAscii(PathUtf8(*b)))
                        Fail(e.source, "Resource escapes its declaring package root");
                if (b == spec.filename.end()) Fail(e.source, "Resource filename names its package root");
                if (!std::filesystem::is_regular_file(spec.filename)) Fail(e.source, "Missing resource file", DiagnosticCode::MissingDependency);
            }
            else if (e.key == "type") type = LowerAscii(e.value);
            else if (e.key == "format") spec.format = LowerAscii(e.value);
            else if (e.key == "data") spec.data = e.value;
            else if (e.key == "array")
            { if (!Integer(e.value, spec.array) || !spec.array || spec.array > 4096) Fail(e.source, "Resource array exceeds native slice"); }
            else if (e.key == "stride")
            {
                if (!Integer(e.value, spec.stride) || !spec.stride) Fail(e.source, "Invalid Resource stride");
            }
            else Fail(e.source, "Unsupported Resource property: " + e.key);
        }
        if (type == "structuredbuffer")
        {
            if (!spec.array || !spec.format.empty()) Fail(section.source, "StructuredBuffer requires exact array and no typed view format");
            if (spec.filename.empty() && spec.data.empty())
            {
                if (spec.stride != 16 || spec.array != 4096)
                    Fail(section.source, "Only the declared stride=16,array=4096 formatter output is writable");
                spec.kind = ResourceKind::PositionsBuffer;
            }
            else
            {
                if (!spec.filename.empty() && !spec.data.empty()) Fail(section.source, "Resource filename and data cannot coexist");
                spec.kind = ResourceKind::StructuredBuffer;
            }
        }
        else if (!spec.data.empty())
        {
            if (!spec.filename.empty() || type != "buffer" || spec.array || spec.stride)
                Fail(section.source, "Unsupported inline buffer declaration");
            spec.kind = ResourceKind::InlineBuffer;
        }
        else if (spec.filename.empty())
        {
            if (!keys.empty()) Fail(section.source, "Only empty aliases or explicit supported resource declarations are allowed");
        }
        else
        {
            const auto extension = LowerAscii(PathUtf8(spec.filename.extension()));
            if (type.empty()) type = extension == ".buf" || extension == ".ib" || extension == ".vb" ? "buffer" : "texture2d";
            if (type == "buffer") spec.kind = ResourceKind::FileBuffer;
            else if (type == "texture2d") spec.kind = ResourceKind::FileTexture;
            else Fail(section.source, "Unsupported Resource type: " + type);
            // Resource decoder/format validator is the next preload stage; this
            // stage neither guesses formats nor calls DDS/GPU creation routines.
        }
        if (plan_->resources_.size() >= request_.maxResources) Fail(at, "Resource limit exceeded", DiagnosticCode::Limit);
        const Id id = static_cast<Id>(plan_->resources_.size());
        plan_->resources_.push_back(std::move(spec)); resources_.emplace(name, id);
        return id;
    }

    // A startup-only interpreter over declarations, not a GPU command tape.
    // Restrict the entire called closure to unqualified run/ref/null, so PRE
    // and POST scheduling cannot hide a dynamic or unsupported command.
    void InitializeResources(const ConfigEntry& entry)
    {
        if (++commandCount_ > request_.maxCommands || ++initializationCommands_ > request_.limits.instructions)
            Fail(entry.source, "Static resource initialization command budget exceeded", DiagnosticCode::Limit);
        if (!entry.hasEquals) Fail(entry.source, "Static resource initialization requires '='", DiagnosticCode::InvalidSyntax);
        auto value = LowerAscii(Trim(entry.value));
        if (entry.key == "run")
        {
            if (!Starts(value, "commandlist")) Fail(entry.source, "Static initialization run requires a CommandList");
            const auto name = ResolveSection(value, entry.nameSpace, entry.source);
            const auto& section = Section(name, entry.source);
            if (!Starts(LowerAscii(section.name), "commandlist")) Fail(entry.source, "Static initialization requires a CommandList section");
            if (!initializingResourceLists_.insert(name).second)
                Fail(entry.source, "Static initialization run cycle: " + name, DiagnosticCode::Cycle);
            if (initializingResourceLists_.size() > request_.limits.callDepth)
                Fail(entry.source, "Static initialization run depth exceeded", DiagnosticCode::Limit);
            triggerFiles_.insert(LowerAscii(PathUtf8(section.source.file)));
            for (const auto& declaration : section.declarations)
                triggerFiles_.insert(LowerAscii(PathUtf8(declaration.file)));
            for (const auto& command : section.entries)
            {
                triggerFiles_.insert(LowerAscii(PathUtf8(command.source.file)));
                InitializeResources(command);
            }
            initializingResourceLists_.erase(name);
            return;
        }
        // No branches, variables, phase prefixes, parameter writes, pipeline
        // operands or GPU copy/create operations are admitted by this subset.
        if (!Starts(entry.key, "resource"))
            Fail(entry.source, "Static initialization supports only resource alias references and nested run");
        const auto destination = TargetOf(entry.key, entry);
        if (destination.kind != TargetKind::Resource || plan_->resources_[destination.index].kind != ResourceKind::Alias)
            Fail(entry.source, "Static initialization destination must be an empty resource alias");
        constexpr std::string_view suffix = " unless_null";
        bool unlessNull = false;
        if (value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            unlessNull = true; value = Trim(std::string_view(value).substr(0, value.size() - suffix.size()));
        }
        const bool reference = Starts(value, "ref ");
        if (reference) value = Trim(std::string_view(value).substr(4));
        if (value != "null" && (!reference || !Starts(value, "resource")))
            Fail(entry.source, "Static initialization requires explicit ref Resource or null");
        const auto source = TargetOf(value, entry);
        Id origin = InvalidId;
        if (source.kind == TargetKind::Resource)
        {
            const auto& resource = plan_->resources_[source.index];
            if (resource.kind == ResourceKind::Alias)
            {
                const auto previous = initialResourceOrigins_.find(source.index);
                if (previous != initialResourceOrigins_.end()) origin = previous->second;
            }
            else
            {
                if (resource.kind == ResourceKind::PositionsBuffer)
                    Fail(entry.source, "Static initialization cannot reference a writable GPU output");
                origin = source.index;
            }
        }
        else if (source.kind != TargetKind::Null) Fail(entry.source, "Invalid static resource initialization source");
        if (unlessNull && origin == InvalidId) return;
        // Snapshot now: A=ref B followed by B=ref C must not retarget A.
        initialResourceOrigins_[destination.index] = origin;
        plan_->initialResources_.push_back({entry.source, destination.index, origin});
    }

    static bool IsMetadata(std::string_view key, ProgramKind kind)
    {
        if (kind == ProgramKind::ShaderRegex) return key == "shader_model" || key == "temps" || key == "filter_index";
        if (kind != ProgramKind::TextureOverride && kind != ProgramKind::ShaderOverride) return false;
        constexpr std::string_view keys[] = {"hash", "match_bind_flags", "match_usage", "match_type", "match_format", "match_first_index", "match_index_count", "match_priority", "filter_index", "allow_duplicate_hash", "override_vertex_count", "override_byte_stride", "uav_byte_stride"};
        return std::find(std::begin(keys), std::end(keys), key) != std::end(keys);
    }
    void Override(const ConfigSection& section, Id program, ProgramKind kind)
    {
        OverrideSpec spec;
        spec.source = section.source; spec.program = program; spec.shader = kind == ProgramKind::ShaderOverride;
        bool texture2D = false;
        std::set<std::string> keys;
        for (const auto& e : section.entries)
        {
            if (!IsMetadata(e.key, kind)) continue;
            if (!e.hasEquals || !keys.insert(e.key).second) Fail(e.source, "Malformed/duplicate identity metadata: " + e.key);
            const auto value = LowerAscii(Trim(e.value));
            if (e.key == "hash")
            {
                const auto expected = spec.shader ? 16u : 8u;
                const auto r = std::from_chars(value.data(), value.data() + value.size(), spec.hash, 16);
                if (value.size() != expected || r.ec != std::errc() || r.ptr != value.data() + value.size())
                    Fail(e.source, "Expected exact upstream " + std::to_string(expected) + "-digit hash");
            }
            else if (e.key == "override_vertex_count" || e.key == "override_byte_stride" || e.key == "uav_byte_stride")
            {
                std::uint32_t n = 0;
                if (spec.shader || !Integer(value, n) || !n) Fail(e.source, "Expected positive TextureOverride buffer creation metadata");
                if (e.key == "override_vertex_count") spec.resize.vertexCount = n;
                else if (e.key == "override_byte_stride") spec.resize.byteStride = n;
                else spec.resize.uavStride = n;
            }
            else if (e.key == "match_first_index" || e.key == "match_index_count")
            {
                if (spec.shader) Fail(e.source, "Shader draw-range metadata is outside this slice");
                auto number = value;
                if (e.key == "match_index_count")
                {
                    // Parse once; draw comparisons remain integer/unsigned even
                    // above float's exact range. Field arithmetic is unsupported.
                    const std::pair<std::string_view, UnsignedComparison> operators[] = {
                        {"<=", UnsignedComparison::LessEqual}, {">=", UnsignedComparison::GreaterEqual},
                        {"=", UnsignedComparison::Equal}, {"!", UnsignedComparison::NotEqual},
                        {"<", UnsignedComparison::Less}, {">", UnsignedComparison::Greater}};
                    for (const auto& [text, op] : operators) if (number.starts_with(text))
                    { spec.indexCountComparison = op; number = Trim(std::string_view(number).substr(text.size())); break; }
                }
                std::uint32_t n;
                if (!Integer(number, n)) Fail(e.source, "Expected unsigned literal draw-range predicate");
                if (e.key == "match_first_index") { spec.hasFirstIndex = true; spec.firstIndex = n; }
                else { spec.hasIndexCount = true; spec.indexCount = n; }
            }
            else if (e.key == "match_bind_flags")
            {
                // Deliberately exact bare flags only. +/- and numeric masks
                // must not silently acquire the semantics of a bare flag.
                if (spec.shader) Fail(e.source, "Shader descriptor metadata unsupported");
                const std::pair<std::string_view, UINT> flags[] = {
                    {"index_buffer", D3D11_BIND_INDEX_BUFFER}, {"shader_resource", D3D11_BIND_SHADER_RESOURCE},
                    {"render_target", D3D11_BIND_RENDER_TARGET}};
                std::string_view remaining(value);
                while (!remaining.empty())
                {
                    const auto begin = remaining.find_first_not_of(" \t");
                    if (begin == remaining.npos) break;
                    remaining.remove_prefix(begin);
                    const auto end = remaining.find_first_of(" \t");
                    const auto token = remaining.substr(0, end);
                    UINT bit = 0;
                    for (const auto& flag : flags) if (token == flag.first) bit = flag.second;
                    if (!bit || (spec.bindFlags & bit)) Fail(e.source, "Unsupported/duplicate bare match_bind_flags token");
                    spec.bindFlags |= bit;
                    if (end == remaining.npos) break;
                    remaining.remove_prefix(end);
                }
            }
            else if (e.key == "match_usage")
            {
                if (spec.shader) Fail(e.source, "Shader descriptor metadata unsupported");
                if (value == "default") spec.usage = D3D11_USAGE_DEFAULT;
                else if (value == "immutable") spec.usage = D3D11_USAGE_IMMUTABLE;
                else if (value == "dynamic") spec.usage = D3D11_USAGE_DYNAMIC;
                else if (value == "staging") spec.usage = D3D11_USAGE_STAGING;
                else Fail(e.source, "Unsupported exact resource match_usage");
            }
            else if (e.key == "match_type")
            {
                if (spec.shader || (value != "buffer" && value != "texture2d"))
                    Fail(e.source, "Only Buffer/Texture2D descriptor rules are supported");
                texture2D = value == "texture2d";
            }
            else if (e.key == "match_format")
            {
                if (spec.shader || value != "r8_typeless") Fail(e.source, "Unsupported exact resource match_format");
                spec.resourceFormat = DXGI_FORMAT_R8_TYPELESS;
            }
            else if (e.key == "match_priority")
            {
                const auto r = std::from_chars(value.data(), value.data() + value.size(), spec.priority);
                if (r.ec != std::errc() || r.ptr != value.data() + value.size()) Fail(e.source, "Invalid match_priority");
            }
            else if (e.key == "allow_duplicate_hash")
            {
                if (!spec.shader || value != "overrule") Fail(e.source, "Only shader allow_duplicate_hash=overrule is supported");
                spec.overrule = true;
            }
            else
            {
                Scope scope(1);
                const Id expression = Expr(value, e, scope, true);
                spec.filterIndex = Constant(plan_->expressions_[expression], e.source);
                spec.hasFilterIndex = true;
            }
        }
        const bool descriptor = keys.count("match_bind_flags") || keys.count("match_usage") || keys.count("match_type") || keys.count("match_format");
        if (keys.count("hash") && descriptor)
            Fail(section.source, "Mixed hash/descriptor rules unsupported; do not silently discard predicates");
        if (!keys.count("hash"))
        {
            if (spec.shader || !keys.count("match_bind_flags"))
                Fail(section.source, "Override requires an upstream hash or supported exact resource descriptor", DiagnosticCode::MissingDependency);
            if (texture2D)
            {
                if (!keys.count("match_format") || spec.bindFlags != (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET))
                    Fail(section.source, "Texture2D descriptor requires implemented exact format and SRV/RTV flags");
                spec.matchKind = OverrideMatchKind::Texture2DDescriptor;
            }
            else
            {
                if (keys.count("match_format") || spec.bindFlags != D3D11_BIND_INDEX_BUFFER)
                    Fail(section.source, "Buffer descriptor requires exact INDEX_BUFFER flags without texture format");
                spec.matchKind = OverrideMatchKind::BufferDescriptor;
            }
        }
        if (spec.resize.vertexCount || spec.resize.byteStride || spec.resize.uavStride)
        {
            const auto bytes = std::uint64_t(spec.resize.vertexCount) * spec.resize.byteStride;
            if (spec.matchKind != OverrideMatchKind::Hash || !spec.resize.vertexCount || !spec.resize.byteStride || bytes > UINT32_MAX || (bytes % 4) ||
                spec.hasFirstIndex || spec.hasIndexCount || (spec.resize.uavStride && bytes % spec.resize.uavStride))
                Fail(section.source, "Unsupported buffer creation size/stride or draw-dependent resize");
            spec.resize.hash = static_cast<std::uint32_t>(spec.hash); spec.resize.byteWidth = static_cast<std::uint32_t>(bytes);
        }
        plan_->overrides_.push_back(spec);
    }

    static std::string RegexName(const ConfigSection& section)
    {
        // Split the local spelling, not canonical namespace/path dots.
        return SectionName(std::string_view(section.name).substr(0, section.name.find('.')), section.nameSpace);
    }
    void ShaderRegex(const ConfigSection& section, Id program)
    {
        ShaderRegexGroup group;
        group.source = section.source; group.name = section.canonicalName; group.program = program;
        const auto found = regexSections_.find(group.name);
        if (found == regexSections_.end()) Fail(section.source, "Missing coherent ShaderRegex group", DiagnosticCode::MissingDependency);
        const ConfigSection* pattern = nullptr;
        const ConfigSection* replacement = nullptr;
        for (const auto* member : found->second)
        {
            group.sections.push_back({member->source, member->canonicalName, member->entries});
            if (member == &section) continue;
            const auto local = LowerAscii(member->name);
            const auto suffix = local.substr(local.find('.') + 1);
            if (suffix == "pattern") pattern = member;
            else if (suffix == "pattern.replace") replacement = member;
            else if (suffix == "insertdeclarations")
                for (const auto& entry : member->entries)
                {
                    // Validate the COMPLETE group below. A declaration alone
                    // cannot opt into Outline resources or a new output ABI.
                    group.declarations.push_back(entry.raw);
                }
            else Fail(member->source, "Unsupported ShaderRegex member: " + member->name);
        }
        if (replacement && !pattern) Fail(replacement->source, "ShaderRegex replacement requires its real Pattern", DiagnosticCode::MissingDependency);
        if (pattern)
        {
            ShaderRegexPattern rule; rule.source = pattern->source;
            for (const auto& entry : pattern->entries) rule.pattern += entry.raw;
            if (rule.pattern.empty() || rule.pattern.size() > 262144)
                Fail(pattern->source, "Empty or oversized ShaderRegex Pattern", DiagnosticCode::Limit);
            if (replacement)
            {
                rule.replace = true; rule.replacementSource = replacement->source;
                for (const auto& entry : replacement->entries) rule.replacement += entry.raw;
                if (rule.replacement.size() > 262144) Fail(replacement->source, "ShaderRegex replacement exceeds text bound", DiagnosticCode::Limit);
            }
            group.patterns.push_back(std::move(rule));
        }
        std::set<std::string> metadata;
        for (const auto& entry : section.entries)
            if (entry.key == "temps" || entry.key == "filter_index")
            {
                if (!entry.hasEquals || !metadata.insert(entry.key).second)
                    Fail(entry.source, "Malformed/duplicate ShaderRegex metadata", DiagnosticCode::InvalidSyntax);
                if (entry.key == "filter_index")
                {
                    const auto text = Trim(entry.value);
                    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), group.filterIndex);
                    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || !std::isfinite(group.filterIndex))
                        Fail(entry.source, "ShaderRegex filter_index must be a finite float literal", DiagnosticCode::InvalidSyntax);
                    group.hasFilterIndex = true;
                }
                else
                {
                    std::set<std::string> names; std::string token;
                    const auto take = [&] {
                        if (token.empty()) return;
                        if (token.size() > 32 || !((token[0] >= 'A' && token[0] <= 'Z') || (token[0] >= 'a' && token[0] <= 'z') || token[0] == '_') ||
                            token.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != token.npos)
                            Fail(entry.source, "Invalid named shader temporary", DiagnosticCode::InvalidSyntax);
                        names.insert(std::exchange(token, std::string{}));
                    };
                    for (char ch : entry.value) { if (ch == ' ' || ch == '\t') take(); else token += ch; }
                    take();
                    if (names.size() > 64) Fail(entry.source, "Named shader temporary bound exceeded", DiagnosticCode::Limit);
                    group.temps.assign(names.begin(), names.end());
                }
            }
        bool declared = false;
        for (const auto& entry : section.entries) if (entry.key == "shader_model")
        {
            if (declared || !entry.hasEquals)
                Fail(entry.source, "Duplicate/malformed ShaderRegex shader_model", DiagnosticCode::InvalidSyntax);
            declared = true;
            const auto text = LowerAscii(Trim(entry.value));
            constexpr std::pair<std::string_view, ShaderModel> models[] = {
                {"vs_4_0", ShaderModel::VS40}, {"vs_4_1", ShaderModel::VS41},
                {"vs_5_0", ShaderModel::VS50}, {"vs_5_1", ShaderModel::VS51},
                {"ps_4_0", ShaderModel::PS40}, {"ps_4_1", ShaderModel::PS41},
                {"ps_5_0", ShaderModel::PS50}, {"ps_5_1", ShaderModel::PS51}};
            std::size_t at = 0;
            while (at < text.size())
            {
                const auto end = text.find_first_of(" \t", at);
                const auto token = std::string_view(text).substr(at, end == text.npos ? text.size() - at : end - at);
                const auto model = std::find_if(std::begin(models), std::end(models), [&](const auto& value) { return value.first == token; });
                if (model == std::end(models))
                    Fail(entry.source, "Unsupported exact ShaderRegex shader_model: " + std::string(token) +
                         "; only reflected VS/PS 4_0, 4_1, 5_0 and 5_1 are implemented");
                if (std::find(group.models.begin(), group.models.end(), model->second) == group.models.end())
                    group.models.push_back(model->second);
                at = end == text.npos ? text.size() : text.find_first_not_of(" \t", end);
                if (at == text.npos) break;
            }
            if (group.models.empty()) Fail(entry.source, "ShaderRegex requires a nonempty exact shader_model set");
        }
        if (!declared) Fail(section.source, "ShaderRegex is missing shader_model", DiagnosticCode::MissingDependency);
        if (group.RequiresBytecode() && (group.models.size() != 1 ||
            (group.models.front() != ShaderModel::PS50 && group.models.front() != ShaderModel::VS50)))
            Fail(section.source, "Bytecode consumers require exact ps_5_0 or typed vs_5_0; other models retain model-only support");
        const bool exactOutline = outline::ExactTemplate(group);
        const bool vertexStage = group.models.size() == 1 && group.models.front() == ShaderModel::VS50;
        for (const auto& member : group.sections)
            if (LowerAscii(member.name).ends_with(".insertdeclarations"))
                for (const auto& declaration : member.lines)
                {
                    const auto compact = outline::CompactDeclaration(declaration.raw);
                    if (vertexStage ? !vertex::Declaration(compact) :
                        (!exactOutline && !diffuse::ExactTemplate(group) && compact !=
                            "dcl_resource_texture1d(float,float,float,float)t120"))
                        Fail(declaration.source, vertexStage ?
                            "Typed VS insertion supports only Texture1D<float4> t120 and exact immediate CB8[7]/CB12[10]/CB13[10]" :
                            "New shader resources/outputs require the complete exact local Outline or Diffuse template");
                }
        if ((!group.temps.empty() || !group.declarations.empty()) && group.patterns.empty())
            Fail(section.source, "Temporary/declaration insertion requires an actual Pattern in this bounded consumer slice");
        if (!group.declarations.empty() || std::any_of(group.patterns.begin(), group.patterns.end(), [](const auto& p) { return p.replace; }))
            plan_->touches_.iniParameters = true;
        plan_->shaderRegex_.push_back(std::move(group));
    }

    struct Flow
    {
        std::array<Id, 2> branch{InvalidId, InvalidId};
        std::array<std::vector<Id>, 2> ends;
        bool hasElse = false;
    };
    void Emit(std::array<std::vector<Command>, 2>& lists, Command command, int phase)
    {
        for (int p = 0; p != 2; ++p)
            if (phase == -1 || phase == p)
            {
                if (++commandCount_ > request_.maxCommands) Fail(command.source, "Command budget exceeded", DiagnosticCode::Limit);
                lists[p].push_back(command);
            }
    }
    void Commands(const ConfigSection& section, ProgramKind kind, std::array<std::vector<Command>, 2>& lists)
    {
        Scope scope(1);
        std::vector<Flow> flow;
        for (const auto& e : section.entries)
        {
            if (IsMetadata(e.key, kind) || (kind == ProgramKind::RasterCustom && (e.key == "cull" || e.key == "front"))) continue;
            auto key = e.key;
            const auto value = LowerAscii(Trim(e.value));
            const auto raw = LowerAscii(e.raw);
            int phase = 0;
            bool explicitPhase = false;
            if (Starts(key, "post ")) { key.erase(0, 5); phase = 1; explicitPhase = true; }
            else if (Starts(key, "pre ")) { key.erase(0, 4); explicitPhase = true; }
            const bool isIf = Starts(raw, "if ");
            const bool isElif = Starts(raw, "elif ") || Starts(raw, "else if ");
            const bool isElse = raw == "else", isEnd = raw == "endif";
            // Raw flow syntax contains equality operators; the INI delimiter
            // splitter must not turn `if $vs == 12345564` into an assignment.
            if (isIf || isElif || isElse || isEnd)
            {
                if (isIf)
                {
                    const Id expression = Expr(Trim(std::string_view(raw).substr(3)), e, scope);
                    Flow f;
                    for (int p = 0; p != 2; ++p)
                    {
                        f.branch[p] = static_cast<Id>(lists[p].size());
                        Command branch; branch.source = e.source; branch.op = CommandOp::BranchFalse; branch.expressions[0] = expression;
                        Emit(lists, branch, p);
                    }
                    flow.push_back(std::move(f)); scope.emplace_back();
                    if (flow.size() > request_.limits.callDepth) Fail(e.source, "Flow nesting limit exceeded", DiagnosticCode::Limit);
                }
                else
                {
                    if (flow.empty()) Fail(e.source, "Unpaired flow-control directive", DiagnosticCode::InvalidSyntax);
                    auto& f = flow.back();
                    if ((isElif || isElse) && f.hasElse) Fail(e.source, "Flow branch after else", DiagnosticCode::InvalidSyntax);
                    scope.pop_back();
                    if (isEnd)
                    {
                        for (int p = 0; p != 2; ++p)
                        {
                            const Id end = static_cast<Id>(lists[p].size());
                            if (f.branch[p] != InvalidId) lists[p][f.branch[p]].index = end;
                            for (const auto jump : f.ends[p]) lists[p][jump].index = end;
                        }
                        flow.pop_back();
                    }
                    else
                    {
                        Id expression = InvalidId;
                        if (isElif) expression = Expr(Trim(std::string_view(raw).substr(Starts(raw, "elif ") ? 5 : 8)), e, scope);
                        for (int p = 0; p != 2; ++p)
                        {
                            f.ends[p].push_back(static_cast<Id>(lists[p].size()));
                            Command jump; jump.source = e.source; jump.op = CommandOp::Jump; Emit(lists, jump, p);
                            lists[p][f.branch[p]].index = static_cast<Id>(lists[p].size());
                            if (isElif)
                            {
                                f.branch[p] = static_cast<Id>(lists[p].size());
                                Command branch; branch.source = e.source; branch.op = CommandOp::BranchFalse;
                                branch.expressions[0] = expression; Emit(lists, branch, p);
                            }
                            else f.branch[p] = InvalidId;
                        }
                        f.hasElse = isElse; scope.emplace_back();
                    }
                }
                continue;
            }
            if ((kind == ProgramKind::TextureOverride || kind == ProgramKind::ShaderOverride) && Starts(key, "match_"))
                Fail(e.source, "Unsupported override predicate: " + key);
            Command command; command.source = e.source;
            if (Starts(key, "local "))
            {
                const auto name = Trim(std::string_view(key).substr(6));
                if (!Variable(name) || name.find('\\') != name.npos) Fail(e.source, "Invalid lexical local name");
                if (name.find_first_of(".-") != name.npos) Fail(e.source, "Local names must be plain identifiers");
                for (const auto& level : scope) if (level.count(name)) Fail(e.source, "Local redeclares a visible local: " + name);
                if (plan_->variables_.size() >= request_.maxVariables) Fail(e.source, "Variable limit exceeded", DiagnosticCode::Limit);
                const Id id = static_cast<Id>(plan_->variables_.size());
                plan_->variables_.push_back({e.source, section.canonicalName + ":" + name, 0, false, true});
                scope.back().emplace(name, id);
                if (!e.hasEquals) continue;
                command.op = CommandOp::Assign; command.index = id; command.expressions[0] = Expr(e.value, e, scope);
            }
            else if (!key.empty() && key.front() == '$')
            {
                if (!Variable(key)) Fail(e.source, "Unsupported variable assignment target: " + key);
                command.op = CommandOp::Assign; command.index = Var(key, e, scope); command.expressions[0] = Expr(e.value, e, scope);
            }
            else if (Parameter(key, command.index))
            {
                command.op = CommandOp::SetParameter; command.expressions[0] = Expr(e.value, e, scope);
                plan_->touches_.iniParameters = true;
            }
            else if (key == "store")
            {
                // Narrow native scalar store. All required branches compile;
                // this is neither an alias nor a general GPU readback command.
                const auto first = value.find(','), second = first == value.npos ? value.npos : value.find(',', first + 1);
                if (first == value.npos || second == value.npos || value.find(',', second + 1) != value.npos)
                    Fail(e.source, "store requires exactly $variable, ps-cbN, float-index");
                const auto variable = Trim(std::string_view(value).substr(0, first));
                const auto target = Trim(std::string_view(value).substr(first + 1, second - first - 1));
                const auto scalar = Trim(std::string_view(value).substr(second + 1));
                if (!Variable(variable) || !Starts(target, "ps-cb") ||
                    !Integer(std::string_view(target).substr(5), command.constantSlot) ||
                    command.constantSlot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT ||
                    !Integer(scalar, command.constantFloat) || command.constantFloat >= 16384)
                    Fail(e.source, "store requires a declared variable, PS CB slot 0..13 and float index 0..16383");
                command.op = CommandOp::StorePixelConstant; command.index = Var(variable, e, scope);
            }
            else if (key == "run")
            {
                const bool custom = Starts(value, "customshader");
                const bool unbind = value == "builtincommandlistunbindallrendertargets";
                if (!custom && !unbind && !Starts(value, "commandlist")) Fail(e.source, "Unsupported explicit run target: " + value);
                command.op = CommandOp::Run; command.together = explicitPhase;
                command.index = ProgramOf(unbind ? value : ResolveSection(value, e.nameSpace, e.source), e.source);
                // Fixed ParseRunShader defaults to PRE; unlike CommandList it
                // executes its own PRE+POST once, not again at native POST.
                if (!explicitPhase && !custom) phase = -1;
            }
            else if (key == "checktextureoverride")
            {
                command.op = CommandOp::Check; command.together = explicitPhase;
                command.sourceTarget = TargetOf(value, e);
                if (command.sourceTarget.kind == TargetKind::Null) Fail(e.source, "Cannot check null target");
                if (!explicitPhase) phase = -1;
            }
            else if (Starts(key, "vs-cb"))
            {
                // Real Offset cleanup is a null setter, not a resource filter,
                // alias, CPU store or authorization to bind arbitrary buffers.
                if (!Integer(std::string_view(key).substr(5), command.index) ||
                    command.index >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT || value != "null")
                    Fail(e.source, "VS constant-buffer cleanup requires an exact slot and bare null");
                command.op = CommandOp::UnbindVertexConstant;
                plan_->touches_.vertexConstantNulls |= std::uint32_t{1} << command.index;
            }
            else if (key == "mouse_draw" && (kind == ProgramKind::SoftwareMouse || kind == ProgramKind::SoftwareMouseBW))
            {
                command.op = CommandOp::MouseDraw;
                command.index = kind == ProgramKind::SoftwareMouse ? 1u : 2u;
            }
            else if (key == "handling")
            {
                if (value != "skip" || phase == 1) Fail(e.source, "Only PRE handling=skip is supported");
                command.op = CommandOp::Skip;
            }
            else if (key == "clear")
            {
                command.op = CommandOp::ClearTarget;
                command.sourceTarget = TargetOf(value, e);
                if (command.sourceTarget.kind != TargetKind::Resource ||
                    plan_->resources_[command.sourceTarget.index].kind != ResourceKind::Alias)
                    Fail(e.source, "Bare clear requires a private copy_desc alias; explicit values/UAV/game targets are unsupported");
                plan_->touches_.clearTargets = true;
            }
            else if (key == "draw" && value == "from_caller")
            {
                command.op = CommandOp::DrawFromCaller;
            }
            else if ((key == "drawindexed" || key == "drawindexedinstanced") && value == "auto")
            {
                command.op = key == "drawindexed" ? CommandOp::DrawIndexedAuto : CommandOp::DrawIndexedInstancedAuto;
            }
            else if (key == "drawindexed" || key == "draw" || key == "drawindexedinstanced")
            {
                const unsigned arguments = key == "draw" ? 2u : key == "drawindexedinstanced" ? 5u : 3u;
                command.op = key == "draw" ? CommandOp::Draw : key == "drawindexedinstanced" ? CommandOp::DrawIndexedInstanced : CommandOp::DrawIndexed;
                std::size_t begin = 0;
                for (unsigned i = 0; i < arguments; ++i)
                {
                    const auto comma = value.find(',', begin);
                    if ((i + 1 != arguments && comma == value.npos) || (i + 1 == arguments && comma != value.npos))
                        Fail(e.source, arguments == 2 ? "draw requires exactly vertexCount, firstVertex" :
                            "indexed draw requires exactly count, [instances,] firstIndex, baseVertex [,firstInstance]");
                    command.expressions[i] = Expr(Trim(std::string_view(value).substr(begin, comma == value.npos ? value.size() - begin : comma - begin)), e, scope);
                    begin = comma == value.npos ? value.size() : comma + 1;
                }
            }
            else if (key == "o0" && value == "set_viewport bb")
            {
                // A real native swapchain operand, not a fabricated resource ID.
                // The dedicated Present packet proves the owning RTV/viewport.
                command.op = CommandOp::BackbufferViewport;
                command.destination = {TargetKind::RenderTarget, 0};
                plan_->touches_.backbufferViewport = plan_->touches_.outputSetters = true;
            }
            else
            {
                command.op = CommandOp::Reference;
                command.destination = TargetOf(key, e);
                if (command.destination.kind == TargetKind::Null) Fail(e.source, "Cannot assign to null");
                auto source = value;
                bool reference = false, copy = false, description = false;
                if (Starts(source, "ref ")) { reference = true; source = Trim(std::string_view(source).substr(4)); }
                else if (Starts(source, "reference ")) { reference = true; source = Trim(std::string_view(source).substr(10)); }
                else if (Starts(source, "copy ")) { copy = true; source = Trim(std::string_view(source).substr(5)); }
                else if (Starts(source, "copy_desc ")) { description = true; source = Trim(std::string_view(source).substr(10)); }
                constexpr std::string_view suffix = " unless_null";
                if (source.size() >= suffix.size() && source.compare(source.size() - suffix.size(), suffix.size(), suffix) == 0)
                {
                    command.unlessNull = true; source = Trim(std::string_view(source).substr(0, source.size() - suffix.size()));
                }
                command.sourceTarget = TargetOf(source, e);
                // Explicit content copy remains depth-only. copy_desc creates
                // private storage without authorizing its uninitialized contents.
                if (copy)
                {
                    if (command.sourceTarget.kind != TargetKind::DepthTarget ||
                        command.destination.kind != TargetKind::Resource ||
                        plan_->resources_[command.destination.index].kind != ResourceKind::Alias)
                        Fail(e.source, "Only explicit copy oD to an alias is supported");
                    command.op = CommandOp::CopyDepth;
                    plan_->touches_.depthCopy = true;
                }
                if (description)
                {
                    if ((command.sourceTarget.kind != TargetKind::RenderTarget && command.sourceTarget.kind != TargetKind::DepthTarget) ||
                        command.destination.kind != TargetKind::Resource ||
                        plan_->resources_[command.destination.index].kind != ResourceKind::Alias)
                        Fail(e.source, "copy_desc requires an actual OM output and an empty declared alias");
                    auto& resource = plan_->resources_[command.destination.index];
                    if (resource.outputSlot == InvalidId)
                    {
                        if (plan_->outputAliasCount_ == OutputAliasCapacity)
                            Fail(e.source, "Private output alias capacity exceeded", DiagnosticCode::Limit);
                        resource.outputSlot = plan_->outputAliasCount_++;
                    }
                    command.op = CommandOp::CopyDescription; plan_->touches_.outputDescription = true;
                }
                if (command.sourceTarget.kind != TargetKind::Null && !reference && !copy && !description &&
                    (command.destination.kind == TargetKind::Resource ||
                     (command.sourceTarget.kind != TargetKind::Resource && command.sourceTarget.kind != command.destination.kind)))
                    Fail(e.source, "Implicit/full GPU copy is unsupported; explicit ref is required for alias destinations");
                const bool outputSource = command.sourceTarget.kind == TargetKind::RenderTarget || command.sourceTarget.kind == TargetKind::DepthTarget;
                const bool outputDestination = command.destination.kind == TargetKind::RenderTarget || command.destination.kind == TargetKind::DepthTarget;
                if (outputDestination)
                {
                    if ((command.sourceTarget.kind != TargetKind::Null &&
                         (!reference || command.sourceTarget.kind != TargetKind::Resource ||
                          plan_->resources_[command.sourceTarget.index].kind != ResourceKind::Alias)) || copy || description)
                        Fail(e.source, "OM setter requires null or explicit ref from an owning alias");
                    plan_->touches_.outputSetters = true;
                }
                if (outputSource && !copy && !description)
                {
                    if (!reference || command.destination.kind != TargetKind::Resource ||
                        plan_->resources_[command.destination.index].kind != ResourceKind::Alias)
                        Fail(e.source, "OM reference requires an explicit owning alias destination");
                    plan_->touches_.outputReferences = true;
                }
                Touch(command.destination);
            }
            if (!e.hasEquals) Fail(e.source, "Command requires '='", DiagnosticCode::InvalidSyntax);
            Emit(lists, std::move(command), phase);
        }
        if (!flow.empty()) Fail(section.source, "Missing endif", DiagnosticCode::InvalidSyntax);
    }

    void FormatText(const ConfigSection& section, std::array<std::vector<Command>, 2>& lists)
    {
        // Full sequence from the provided main.ini, independent of section and
        // resource names. No extra property/branch/dispatch can be discarded.
        constexpr std::string_view keys[] = {"cs", "vs", "ps", "hs", "ds", "gs", "cs-t100", "",
            "cs-t113", "cs-t114", "clear", "cs-u0", "dispatch", "", "cs-u0", "cs-t100", "cs-t113", "cs-t114"};
        if (section.entries.size() != std::size(keys)) Fail(section.source, "Unsupported CustomShader command closure; only exact FormatText compute sequence is implemented");
        for (unsigned i = 0; i < std::size(keys); ++i)
            if (!keys[i].empty() && (!section.entries[i].hasEquals || section.entries[i].key != keys[i]))
                Fail(section.entries[i].source, "Unsupported CustomShader order/property; no partial execution");
        for (const unsigned i : {1u, 2u, 3u, 4u, 5u, 14u, 15u, 16u, 17u})
            if (LowerAscii(Trim(section.entries[i].value)) != "null") Fail(section.entries[i].source, "CustomShader stage/unbind must be explicit null");
        const auto& cs = section.entries[0];
        std::string path = Trim(cs.value);
        if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);
        std::replace(path.begin(), path.end(), '\\', '/');
        FormatTextSpec spec; spec.source = cs.source;
        spec.shader = std::filesystem::weakly_canonical(cs.source.file.parent_path() / std::filesystem::u8path(path));
        const auto* root = request_.config->RootFor(cs.source.file);
        if (!root || root->empty()) Fail(cs.source, "Compute source has no admitted package root");
        auto a = root->begin(), b = spec.shader.begin();
        for (; a != root->end(); ++a, ++b)
            if (b == spec.shader.end() || LowerAscii(PathUtf8(*a)) != LowerAscii(PathUtf8(*b)))
                Fail(cs.source, "Compute source escapes its admitted package root");
        if (b == spec.shader.end() || !std::filesystem::is_regular_file(spec.shader)) Fail(cs.source, "Missing explicit compute HLSL", DiagnosticCode::MissingDependency);
        auto resource = [&](unsigned i) {
            const auto& e = section.entries[i]; const auto value = LowerAscii(Trim(e.value));
            if (!Starts(value, "resource")) Fail(e.source, "FormatText slot requires an explicit Resource");
            return Resource(ResolveSection(value, e.nameSpace, e.source), e.source);
        };
        spec.font = resource(6); spec.text = resource(8); spec.parameters = resource(9); spec.positions = resource(11);
        if (resource(10) != spec.positions) Fail(section.entries[10].source, "FormatText clear and u0 must name the same resource");
        const auto guard = LowerAscii(Trim(section.entries[7].raw));
        constexpr std::string_view suffix = " !== null";
        if (!Starts(guard, "if ") || guard.size() <= 3 + suffix.size() ||
            guard.compare(guard.size() - suffix.size(), suffix.size(), suffix) != 0)
            Fail(section.entries[7].source, "FormatText requires the real resource !== null guard");
        const auto guarded = Trim(std::string_view(guard).substr(3, guard.size() - 3 - suffix.size()));
        if (Resource(ResolveSection(guarded, section.entries[7].nameSpace, section.entries[7].source), section.entries[7].source) != spec.text ||
            LowerAscii(Trim(section.entries[13].raw)) != "endif") Fail(section.entries[7].source, "FormatText guard does not pair its actual text resource");
        auto dispatch = LowerAscii(Trim(section.entries[12].value));
        dispatch.erase(std::remove_if(dispatch.begin(), dispatch.end(), [](char c) { return c == ' ' || c == '\t'; }), dispatch.end());
        if (dispatch != "1,1,1") Fail(section.entries[12].source, "Only the real one-group FormatText dispatch is supported");
        if (plan_->resources_[spec.positions].kind != ResourceKind::PositionsBuffer)
            Fail(section.entries[11].source, "FormatText u0 requires declared structured output, not an alias or fake material");
        Command command; command.source = section.source; command.op = CommandOp::FormatText;
        command.index = static_cast<Id>(plan_->formatText_.size());
        plan_->formatText_.push_back(std::move(spec));
        plan_->touches_.iniParameters = true;
        Emit(lists, std::move(command), 0);
    }

    void RenderText(const ConfigSection& section, std::array<std::vector<Command>, 2>& lists)
    {
        // The installed main.ini's complete ordered VS/GS/PS declaration.
        // BuiltIn unbind, bb, fixed draw and post unbinds become one native
        // draw action later. Arbitrary CustomShader syntax remains refused.
        constexpr std::string_view keys[] = {
            "vs", "gs", "ps", "hs", "ds", "cs", "depth_clip_enable", "cull", "blend", "topology",
            "run", "o0", "gs-t100", "ps-t100", "post gs-t100", "post ps-t100",
            "gs-t112", "gs-t113", "gs-t114", "ps-t114", "draw", "gs-t112", "gs-t113",
            "gs-t114", "ps-t114", "resourcetext", "resourcetextparams"};
        if (section.entries.size() != std::size(keys))
            Fail(section.source, "Unsupported RenderText declaration/command count");
        for (unsigned i = 0; i < std::size(keys); ++i)
            if (!section.entries[i].hasEquals || section.entries[i].key != keys[i])
                Fail(section.entries[i].source, "Unsupported RenderText property or order");
        auto value = [&](unsigned i) { return LowerAscii(Trim(section.entries[i].value)); };
        for (const unsigned i : {3u, 4u, 5u, 14u, 15u, 21u, 22u, 23u, 24u, 25u, 26u})
            if (value(i) != "null") Fail(section.entries[i].source, "RenderText requires an original explicit null stage/unbind");
        if (value(6) != "false" || value(7) != "none" ||
            value(8) != "add src_alpha inv_src_alpha" || value(9) != "point_list" ||
            value(10) != "builtincommandlistunbindallrendertargets" ||
            value(11) != "set_viewport no_view_cache bb")
            Fail(section.source, "Unsupported RenderText raster/OM/backbuffer state");
        auto draw = value(20);
        draw.erase(std::remove_if(draw.begin(), draw.end(), [](char c) { return c == ' ' || c == 9; }), draw.end());
        if (draw != "4096,0") Fail(section.entries[20].source, "RenderText requires the original point draw");
        auto shaderPath = [&](unsigned i) {
            const auto& e = section.entries[i];
            std::string path = Trim(e.value);
            if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);
            std::replace(path.begin(), path.end(), static_cast<char>(92), '/');
            const auto resolved = std::filesystem::weakly_canonical(e.source.file.parent_path() / std::filesystem::u8path(path));
            const auto* root = request_.config->RootFor(e.source.file);
            if (!root || root->empty()) Fail(e.source, "RenderText source has no admitted package root");
            auto a = root->begin(), b = resolved.begin();
            for (; a != root->end(); ++a, ++b)
                if (b == resolved.end() || LowerAscii(PathUtf8(*a)) != LowerAscii(PathUtf8(*b)))
                    Fail(e.source, "RenderText source escapes its package root");
            if (b == resolved.end() || !std::filesystem::is_regular_file(resolved))
                Fail(e.source, "Missing explicit RenderText HLSL", DiagnosticCode::MissingDependency);
            return resolved;
        };
        RenderTextSpec spec; spec.source = section.source; spec.shader = shaderPath(0);
        if (shaderPath(1) != spec.shader || shaderPath(2) != spec.shader)
            Fail(section.source, "RenderText VS/GS/PS must use one declared source");
        auto resource = [&](unsigned i) {
            const auto& e = section.entries[i]; const auto name = value(i);
            if (!Starts(name, "resource")) Fail(e.source, "RenderText binding requires explicit Resource");
            return Resource(ResolveSection(name, e.nameSpace, e.source), e.source);
        };
        spec.font = resource(12); spec.positions = resource(16);
        spec.text = resource(17); spec.parameters = resource(18);
        if (resource(13) != spec.font || resource(19) != spec.parameters ||
            plan_->resources_[spec.positions].kind != ResourceKind::PositionsBuffer ||
            plan_->resources_[spec.text].kind != ResourceKind::Alias ||
            plan_->resources_[spec.parameters].kind != ResourceKind::Alias ||
            plan_->resources_[spec.font].kind != ResourceKind::FileTexture)
            Fail(section.source, "RenderText resources differ from fixed Font/Text/Params/Position contract");
        for (const unsigned i : {25u, 26u})
        {
            const auto& e = section.entries[i];
            const auto id = Resource(ResolveSection(e.key, e.nameSpace, e.source), e.source);
            if (id != (i == 25 ? spec.text : spec.parameters))
                Fail(e.source, "RenderText final alias clear targets wrong Resource");
        }
        Command render; render.source = section.source; render.op = CommandOp::RenderText;
        render.index = static_cast<Id>(plan_->renderText_.size());
        plan_->renderText_.push_back(std::move(spec));
        plan_->touches_.iniParameters = true;
        Emit(lists, std::move(render), 0);
        for (const unsigned i : {25u, 26u})
        {
            Command clear; clear.source = section.entries[i].source; clear.op = CommandOp::Reference;
            clear.destination = {TargetKind::Resource, i == 25 ? plan_->renderText_.back().text : plan_->renderText_.back().parameters};
            Emit(lists, std::move(clear), 0);
        }
    }

    void DebugOverlay(const ConfigSection& section, std::array<std::vector<Command>, 2>& lists)
    {
        // This exact original CustomShader is a separate fullscreen pipeline.
        // No arbitrary shader/state property, sampler preset or implicit draw ABI.
        constexpr std::string_view keys[] = {
            "vs", "hs", "ds", "gs", "ps", "cs", "alpha_to_coverage", "sample_mask",
            "blend_factor[0]", "blend_factor[1]", "blend_factor[2]", "blend_factor[3]",
            "blend", "alpha", "cull", "topology", "draw"};
        constexpr std::string_view values[] = {
            "", "null", "null", "null", "", "null", "0", "ffffffff",
            "0", "0", "0", "0", "add src_alpha inv_src_alpha", "add one zero", "back", "triangle_list", "3,0"};
        if (section.entries.size() != std::size(keys))
            Fail(section.source, "DebugShaderOutput requires its complete original CustomShader declaration");
        for (unsigned i = 0; i < std::size(keys); ++i)
        {
            const auto& e = section.entries[i];
            if (!e.hasEquals || e.key != keys[i]) Fail(e.source, "Unsupported DebugShaderOutput property/order");
            if (i == 0 || i == 4) continue;
            auto value = LowerAscii(Trim(e.value));
            if (i == 16) value.erase(std::remove_if(value.begin(), value.end(),
                [](char c) { return c == ' ' || c == 9; }), value.end());
            if (value != values[i]) Fail(e.source, "Unsupported DebugShaderOutput state or draw");
        }
        auto shaderPath = [&](unsigned i) {
            const auto& e = section.entries[i];
            auto path = Trim(e.value);
            if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);
            std::replace(path.begin(), path.end(), static_cast<char>(92), '/');
            const auto resolved = std::filesystem::weakly_canonical(e.source.file.parent_path() / std::filesystem::u8path(path));
            const auto* root = request_.config->RootFor(e.source.file);
            if (!root || root->empty()) Fail(e.source, "DebugShaderOutput source has no admitted package root");
            auto a = root->begin(), b = resolved.begin();
            for (; a != root->end(); ++a, ++b)
                if (b == resolved.end() || LowerAscii(PathUtf8(*a)) != LowerAscii(PathUtf8(*b)))
                    Fail(e.source, "DebugShaderOutput source escapes its package root");
            if (b == resolved.end() || !std::filesystem::is_regular_file(resolved))
                Fail(e.source, "Missing explicit DebugShaderOutput HLSL", DiagnosticCode::MissingDependency);
            return resolved;
        };
        DebugOverlaySpec spec; spec.source = section.source; spec.shader = shaderPath(0);
        if (shaderPath(4) != spec.shader) Fail(section.source, "DebugShaderOutput VS/PS require the same original source");
        Command command; command.source = section.source; command.op = CommandOp::DebugOverlay;
        command.index = static_cast<Id>(plan_->debugOverlays_.size());
        plan_->debugOverlays_.push_back(std::move(spec));
        plan_->touches_.debugOverlay = plan_->touches_.iniParameters = true;
        Emit(lists, std::move(command), 0);
    }

    void Mouse(const ConfigSection& section, ProgramKind kind, Id program,
               std::array<std::vector<Command>, 2>& lists)
    {
        constexpr std::pair<std::string_view, std::string_view> outer[] = {
            {"vs", "shaders\\mouse.hlsl"},
            {"ps", "shaders\\mouse.hlsl"},
            {"hs", "null"},
            {"ds", "null"},
            {"gs", "null"},
            {"cull", "none"},
            {"topology", "triangle_strip"},
            {"run", "builtincommandlistunbindallrendertargets"},
            {"o0", "set_viewport bb"},
            {"local $bak_x0", "x0"},
            {"local $bak_y0", "y0"},
            {"local $bak_z0", "z0"},
            {"local $bak_w0", "w0"},
            {"local $bak_x1", "x1"},
            {"local $bak_y1", "y1"},
            {"local $bak_z1", "z1"},
            {"local $bak_w1", "w1"},
            {"run", "commandlist\\gimiv8\\passwindowinfo"},
            {"x1", "x0"},
            {"y1", "y0"},
            {"x0", "cursor_window_x"},
            {"y0", "cursor_window_y"},
            {"z0", "cursor_hotspot_x"},
            {"w0", "cursor_hotspot_y"},
            {"z1", "cursor_showing"},
            {"resourcesoftwaremousebackuptexture1", "reference vs-t100"},
            {"resourcesoftwaremousebackuptexture2", "reference vs-t101"},
            {"resourcesoftwaremousebackuptexture3", "reference ps-t100"},
            {"resourcesoftwaremousebackuptexture4", "reference ps-t101"},
            {"vs-t100", "resourcemask"},
            {"vs-t101", "resourcecolor"},
            {"ps-t100", "resourcemask"},
            {"ps-t101", "resourcecolor"},
            {"blend", "add src_alpha inv_src_alpha"},
            {"w1", "1"},
            {"draw", "4, 0"},
            {"run", "customshadersoftwaremousebw"},
            {"post vs-t100", "reference resourcesoftwaremousebackuptexture1"},
            {"post vs-t101", "reference resourcesoftwaremousebackuptexture2"},
            {"post ps-t100", "reference resourcesoftwaremousebackuptexture3"},
            {"post ps-t101", "reference resourcesoftwaremousebackuptexture4"},
            {"x0", "$bak_x0"},
            {"y0", "$bak_y0"},
            {"z0", "$bak_z0"},
            {"w0", "$bak_w0"},
            {"x1", "$bak_x1"},
            {"y1", "$bak_y1"},
            {"z1", "$bak_z1"},
            {"w1", "$bak_w1"}};
        constexpr std::pair<std::string_view, std::string_view> bw[] = {
            {"blend", "add inv_dest_color src_alpha"},
            {"w1", "2"},
            {"draw", "4, 0"}};
        const bool nested = kind == ProgramKind::SoftwareMouseBW;
        const auto* expected = nested ? bw : outer;
        const auto count = nested ? std::size(bw) : std::size(outer);
        if (section.entries.size() != count) Fail(section.source, "SoftwareMouse requires its complete original declaration");
        for (std::size_t n = 0; n < count; ++n)
        {
            const auto& entry = section.entries[n];
            auto value = LowerAscii(Trim(entry.value));
            if (!entry.hasEquals || entry.key != expected[n].first || value != expected[n].second)
                Fail(entry.source, "SoftwareMouse property/order/command differs from the fixed original");
        }
        if (!nested)
        {
            const auto& entry = section.entries[0]; auto path = Trim(entry.value);
            std::replace(path.begin(), path.end(), static_cast<char>(92), '/');
            const auto resolved = std::filesystem::weakly_canonical(entry.source.file.parent_path() / std::filesystem::u8path(path));
            const auto* root = request_.config->RootFor(entry.source.file);
            if (!root || root->empty()) Fail(entry.source, "Mouse source has no admitted package root");
            auto a = root->begin(), b = resolved.begin();
            for (; a != root->end(); ++a, ++b)
                if (b == resolved.end() || LowerAscii(PathUtf8(*a)) != LowerAscii(PathUtf8(*b)))
                    Fail(entry.source, "Mouse source escapes its admitted package root");
            if (b == resolved.end() || !std::filesystem::is_regular_file(resolved))
                Fail(entry.source, "Missing original Mouse VS/PS source", DiagnosticCode::MissingDependency);
            plan_->programs_[program].mouse = static_cast<Id>(plan_->softwareMouse_.size());
            plan_->softwareMouse_.push_back({entry.source, resolved});
        }
        ConfigSection commands = section; commands.entries.clear();
        for (auto entry : section.entries)
        {
            if (entry.key == "vs" || entry.key == "ps" || entry.key == "hs" || entry.key == "ds" || entry.key == "gs" ||
                entry.key == "cull" || entry.key == "topology" || entry.key == "blend") continue;
            if (entry.key == "draw") { entry.key = "mouse_draw"; entry.raw = "mouse_draw = 4, 0"; }
            commands.entries.push_back(std::move(entry));
        }
        Commands(commands, kind, lists);
        plan_->touches_.iniParameters = plan_->touches_.backbufferViewport = true;
    }

    Id ProgramOf(const std::string& name, const SourceLocation& at)
    {
        if (compilingPrograms_.count(name)) Fail(at, "CommandList run cycle: " + name, DiagnosticCode::Cycle);
        if (const auto found = programs_.find(name); found != programs_.end()) return found->second;
        if (name == "builtincommandlistunbindallrendertargets")
        {
            // XXMI IniHandler.cpp's exact built-in: nine null setters, PRE only.
            // Ordinary run phase pairing is preserved by a real CommandList IR.
            if (plan_->programs_.size() >= request_.maxPrograms) Fail(at, "Program limit exceeded", DiagnosticCode::Limit);
            const Id id = static_cast<Id>(plan_->programs_.size());
            Program builtin{at, name, ProgramKind::CommandList, {}};
            for (unsigned slot = 0; slot < 9; ++slot)
            {
                if (++commandCount_ > request_.maxCommands) Fail(at, "Command budget exceeded", DiagnosticCode::Limit);
                Command command; command.source = at; command.op = CommandOp::Reference;
                command.destination = {slot == 8 ? TargetKind::DepthTarget : TargetKind::RenderTarget, slot == 8 ? 0u : slot};
                builtin.commands[0].push_back(command);
            }
            plan_->programs_.push_back(std::move(builtin)); programs_.emplace(name, id);
            plan_->touches_.outputSetters = true;
            return id;
        }
        const auto& section = Section(name, at);
        triggerFiles_.insert(LowerAscii(PathUtf8(section.source.file)));
        // Global merged Present executes as one ordered program. Retain every
        // physical contributor, including empty headers, in its dependency set.
        for (const auto& declaration : section.declarations)
            triggerFiles_.insert(LowerAscii(PathUtf8(declaration.file)));
        for (const auto& entry : section.entries)
            triggerFiles_.insert(LowerAscii(PathUtf8(entry.source.file)));
        const auto lower = LowerAscii(section.name);
        ProgramKind kind;
        if (Starts(lower, "commandlist")) kind = ProgramKind::CommandList;
        else if (Starts(lower, "textureoverride")) kind = ProgramKind::TextureOverride;
        else if (Starts(lower, "shaderoverride")) kind = ProgramKind::ShaderOverride;
        else if (Starts(lower, "shaderregex") && lower.find('.') == lower.npos) kind = ProgramKind::ShaderRegex;
        else if (lower == "present") kind = ProgramKind::Present;
        // XXMI's default CustomShader has no stage/state overrides. With no
        // entries its PRE/POST are empty, including the legacy Help dummy.
        // Keep the declaration, dependencies and program budget. One nonempty
        // entry must still traverse the strict real shader contracts below.
        else if (Starts(lower, "customshader") && section.entries.empty()) kind = ProgramKind::EmptyCustomShader;
        else if (lower == "customshadersoftwaremouse") kind = ProgramKind::SoftwareMouse;
        else if (lower == "customshadersoftwaremousebw") kind = ProgramKind::SoftwareMouseBW;
        else if (lower == "customshaderrendertext") kind = ProgramKind::RenderText;
        else if (lower == "customshadershowshaderoutput") kind = ProgramKind::DebugOverlay;
        else if (Starts(lower, "customshader") && section.entries.size() == 4 &&
            section.entries[0].key == "cull" && section.entries[1].key == "front" &&
            section.entries[2].key == "run" && section.entries[3].key == "run") kind = ProgramKind::RasterCustom;
        else if (Starts(lower, "customshader")) kind = ProgramKind::FormatText;
        else Fail(at, "Unsupported executable section: " + name);
        if (plan_->programs_.size() >= request_.maxPrograms) Fail(at, "Program limit exceeded", DiagnosticCode::Limit);
        if (compilingPrograms_.size() >= request_.limits.callDepth) Fail(at, "Run dependency depth exceeded", DiagnosticCode::Limit);
        const Id id = static_cast<Id>(plan_->programs_.size());
        plan_->programs_.push_back({section.source, name, kind, {}});
        programs_.emplace(name, id); compilingPrograms_.insert(name);
        // Recursive compilation may reallocate programs_; keep the lists local.
        std::array<std::vector<Command>, 2> lists;
        if (kind == ProgramKind::RasterCustom)
        {
            const auto cull = LowerAscii(Trim(section.entries[0].value));
            const auto front = LowerAscii(Trim(section.entries[1].value));
            const unsigned raster = cull == "none" && front == "counterclockwise" ? 1u :
                cull == "back" && front == "clockwise" ? 2u : 0u;
            if (!raster || std::any_of(section.entries.begin(), section.entries.end(), [](const auto& e) { return !e.hasEquals; }))
                Fail(section.source, "Unsupported scoped CustomShader raster contract");
            plan_->programs_[id].raster = raster;
            plan_->touches_.rasterCustom = plan_->touches_.outputMetadata = true;
        }
        if (kind == ProgramKind::ShaderRegex) ShaderRegex(section, id);
        if (kind == ProgramKind::FormatText) FormatText(section, lists);
        else if (kind == ProgramKind::RenderText) RenderText(section, lists);
        else if (kind == ProgramKind::DebugOverlay) DebugOverlay(section, lists);
        else if (kind == ProgramKind::SoftwareMouse || kind == ProgramKind::SoftwareMouseBW) Mouse(section, kind, id, lists);
        else Commands(section, kind, lists);
        plan_->programs_[id].commands = std::move(lists);
        if (kind == ProgramKind::TextureOverride || kind == ProgramKind::ShaderOverride) Override(section, id, kind);
        compilingPrograms_.erase(name);
        return id;
    }

    void FixedKeyCycle(const ConfigSection& section)
    {
        // This is the four-section original GIMIv8 KeyBindings.ini grammar, not
        // a generic hotkey parser. An immutable declaration does not install a
        // keyboard hook or make a persisted value safe to load/save.
        struct Expected
        {
            const char* section;
            const char* chord;
            const char* variable;
            const char* condition;
            const char* declaringFile;
            float initial;
            bool persistent;
            bool rootGlobal;
        };
        static constexpr Expected known[] = {
            {"keytogglemods", "no_modifiers f6", "$costume_mods", nullptr, "d3dx.ini", 1, false, true},
            {"keytoggleuserguide", "no_modifiers f12", "$show_user_guide", nullptr, "main.ini", 0, false, false},
            {"keytogglehuntingmodeguide", "ctrl f12", "$show_hunting_mode_guide", nullptr, "main.ini", 0, false, false},
            {"keyhidefirstrunnotification", "no_modifiers f10", "$first_run", "$first_run == 1", "main.ini", 1, true, false},
        };
        const auto name = LowerAscii(section.name);
        const auto expected = std::find_if(std::begin(known), std::end(known), [&](const Expected& item) {
            return name == item.section;
        });
        if (expected == std::end(known) || LowerAscii(section.nameSpace) != "gimiv8" ||
            LowerAscii(PathUtf8(section.source.file.filename())) != "keybindings.ini")
            Fail(section.source, "Unsupported required Key section outside the fixed original GIMIv8 KeyBindings.ini graph");
        if (!request_.config->host.rootIni.empty())
        {
            const auto* authority = request_.config->RootFor(section.source.file);
            if (!authority || *authority == request_.config->root)
                Fail(section.source, "MOD-local Key grammar is not a native core cycle; implement its real input/scope owner before importing it");
        }
        // The closure scan may revisit this same immutable section.
        if (!fixedKeySections_.insert(name).second) return;
        std::map<std::string, const ConfigEntry*> fields;
        for (const auto& entry : section.entries)
        {
            if (!entry.hasEquals || entry.source.file != section.source.file ||
                !fields.emplace(entry.key, &entry).second)
                Fail(entry.source, "Fixed GIMI Key entry is missing '=', duplicated, or from another file");
        }
        if (fields.size() != (expected->condition ? 4u : 3u))
            Fail(section.source, "Fixed GIMI Key cycle has missing or unsupported entries");
        const auto required = [&](std::string_view key) -> const ConfigEntry& {
            const auto found = fields.find(std::string(key));
            if (found == fields.end()) Fail(section.source, "Fixed GIMI Key cycle is missing " + std::string(key));
            return *found->second;
        };
        const auto& key = required("key");
        const auto& type = required("type");
        const auto& assignment = required(expected->variable);
        if (LowerAscii(Trim(key.value)) != expected->chord ||
            LowerAscii(Trim(type.value)) != "cycle" || Trim(assignment.value) != "0,1")
            Fail(section.source, "Fixed GIMI key chord, cycle type, or value order differs from upstream");
        Scope empty(1);
        const Id variable = Var(expected->variable, assignment, empty);
        const auto& declared = plan_->variables_[variable];
        const auto resolved = expected->rootGlobal ? LowerAscii(expected->variable) :
            VariableName(expected->variable, section.nameSpace);
        const auto& host = request_.config->host;
        const bool authenticRoot = expected->rootGlobal && !host.rootIni.empty() && declared.source.file == host.rootIni;
        if (declared.name != resolved ||
            (authenticRoot ? (declared.initial != 0 && declared.initial != 1) : declared.initial != expected->initial) ||
            declared.persistent != expected->persistent ||
            (expected->rootGlobal && !host.rootIni.empty() ? !authenticRoot :
                LowerAscii(PathUtf8(declared.source.file.filename())) != expected->declaringFile))
            Fail(assignment.source, "Fixed GIMI Key target is not its original declared global/default/persist contract");
        KeyCycleSpec cycle;
        cycle.source = section.source;
        cycle.name = section.canonicalName;
        cycle.chord = LowerAscii(Trim(key.value));
        cycle.variable = variable;
        if (expected->condition)
        {
            const auto& condition = required("condition");
            if (LowerAscii(Trim(condition.value)) != expected->condition)
                Fail(condition.source, "Fixed first-run Key condition differs from upstream");
            cycle.condition = Expr(condition.value, condition, empty);
        }
        plan_->keyCycles_.push_back(std::move(cycle));
        triggerFiles_.insert(LowerAscii(PathUtf8(section.source.file)));
    }

public:
    explicit Compiler(const CompileRequest& request) : request_(request) {}
    std::shared_ptr<const Plan> Compile()
    {
        if (!request_.config || request_.entries.empty()) Fail({}, "Select an explicit configuration and component entry", DiagnosticCode::InvalidSyntax);
        if (!request_.limits.actions || !request_.limits.instructions || !request_.limits.callDepth ||
            !request_.limits.matchesPerCheck || !request_.limits.expressionStack || request_.limits.callDepth > 64 || request_.limits.expressionStack > 512)
            Fail({}, "Invalid execution limits", DiagnosticCode::Limit);
        plan_->config_ = request_.config; plan_->limits_ = request_.limits;
        for (const auto& section : request_.config->sections)
        {
            sections_.emplace(section.canonicalName, &section);
            if (Starts(LowerAscii(section.name), "shaderregex"))
                regexSections_[RegexName(section)].push_back(&section);
        }
        // Every explicitly loaded file/include is required. An imported main
        // model entry or TexFx group cannot disappear merely because no literal
        // run points into its file. This collects declarations, not executions.
        for (const auto& file : request_.config->files)
            triggerFiles_.insert(LowerAscii(PathUtf8(file.path)));
        for (const auto& section : request_.config->sections)
            if (section.canonicalName == "constants")
                for (const auto& e : section.entries)
                {
                    auto key = e.key;
                    if (Starts(key, "global "))
                    {
                        key = Trim(std::string_view(key).substr(7));
                        if (Starts(key, "persist ")) key = Trim(std::string_view(key).substr(8));
                        if (!Variable(key)) Fail(e.source, "Invalid global declaration: " + key);
                        const auto name = VariableName(key, e.nameSpace);
                        if (!declarations_.emplace(name, e).second) Fail(e.source, "Duplicate global declaration: " + name);
                    }
                    else
                    {
                        Id scalar;
                        if (!e.hasEquals || (key != "run" && !Parameter(key, scalar)))
                            Fail(e.source, "Unsupported Constants command; only globals, static IniParams and static resource initialization run are implemented");
                    }
                    constantOrder_.push_back(&e);
                }
        // Globals, static parameters and resource initialization calls keep
        // their original merged order. Alias snapshots do not execute a GPU
        // tape or admit dynamic Constants commands.
        initializingConstants_ = true;
        for (const auto* entry : constantOrder_)
        {
            auto key = entry->key;
            if (Starts(key, "global "))
            {
                key = Trim(std::string_view(key).substr(7));
                if (Starts(key, "persist ")) key = Trim(std::string_view(key).substr(8));
                Global(VariableName(key, entry->nameSpace), entry->source);
            }
            else if (key == "run") InitializeResources(*entry);
            else
            {
                Id scalar;
                if (!Parameter(key, scalar)) Fail(entry->source, "Invalid initial IniParams scalar", DiagnosticCode::InvalidSyntax);
                Scope empty(1);
                const auto expression = Expr(entry->value, *entry, empty, true);
                for (const auto& token : plan_->expressions_[expression].code)
                    if (token.op == ExprOp::Variable && plan_->variables_[token.index].persistent)
                        Fail(entry->source, "Static IniParams initialisation from a persisted variable is unsupported");
                const auto value = Constant(plan_->expressions_[expression], entry->source);
                plan_->initialParameters_.push_back({entry->source, scalar, value});
                plan_->touches_.iniParameters = true;
            }
        }
        initializingConstants_ = false;
        // Static parameter/resource initialization consumes merged Constants.
        // Keep all of its real contributor files in the automatic-trigger
        // closure; accepting initial values must not drop companion captures.
        if (!plan_->initialParameters_.empty() || !plan_->initialResources_.empty())
            for (const auto* entry : constantOrder_)
                triggerFiles_.insert(LowerAscii(PathUtf8(entry->source.file)));
        for (const auto& file : request_.triggerFiles)
        {
            const auto absolute = std::filesystem::weakly_canonical(file.is_absolute() ? file : request_.config->root / file);
            triggerFiles_.insert(LowerAscii(PathUtf8(absolute)));
        }
        for (const auto& entry : request_.entries)
        {
            const auto& section = Section(entry, {});
            triggerFiles_.insert(LowerAscii(PathUtf8(section.source.file)));
            plan_->entries_.push_back(ProgramOf(section.canonicalName, section.source));
        }
        std::size_t previousFiles;
        do
        {
            previousFiles = triggerFiles_.size();
            for (const auto& section : request_.config->sections)
                if (triggerFiles_.count(LowerAscii(PathUtf8(section.source.file))) ||
                    std::any_of(section.declarations.begin(), section.declarations.end(), [&](const auto& at) {
                        return triggerFiles_.count(LowerAscii(PathUtf8(at.file))) != 0; }) ||
                    std::any_of(section.entries.begin(), section.entries.end(), [&](const auto& entry) {
                        return triggerFiles_.count(LowerAscii(PathUtf8(entry.source.file))) != 0; }))
                {
                    const auto name = LowerAscii(section.name);
                    if (Starts(name, "textureoverride") || Starts(name, "shaderoverride") || name == "present")
                        ProgramOf(section.canonicalName, section.source);
                    else if (Starts(name, "shaderregex"))
                    {
                        const auto main = sections_.find(RegexName(section));
                        if (main == sections_.end())
                            Fail(section.source, "ShaderRegex member has no main group: " + section.name, DiagnosticCode::MissingDependency);
                        ProgramOf(main->first, section.source);
                    }
                    else if (Starts(name, "customshader")) ProgramOf(section.canonicalName, section.source);
                    else if (Starts(name, "key")) FixedKeyCycle(section);
                    else if (Starts(name, "resource") || Starts(name, "commandlist") || Starts(name, "include") || name == "constants") {}
                    else if (section.adaptedHost && !request_.config->host.rootIni.empty() &&
                        section.source.file == request_.config->host.rootIni) {} // every field already has an explicit native/source-tool disposition
                    else Fail(section.source, "Unsupported required dependency section: " + section.name);
                }
        } while (previousFiles != triggerFiles_.size());
        if (!fixedKeySections_.empty() && fixedKeySections_.size() != 4)
            Fail({}, "Incomplete fixed original GIMI KeyBindings cycle set");
        // A misspelled trigger file must not silently drop all of its dependencies.
        for (const auto& file : triggerFiles_)
            if (std::none_of(request_.config->files.begin(), request_.config->files.end(), [&](const auto& f) {
                return LowerAscii(PathUtf8(f.path)) == file; }))
                Fail({std::filesystem::u8path(file), 1}, "Required trigger file was not loaded", DiagnosticCode::MissingDependency);
        std::stable_sort(plan_->overrides_.begin(), plan_->overrides_.end(), [&](const auto& a, const auto& b) {
            return std::tie(a.shader, a.matchKind, a.hash, a.priority, plan_->programs_[a.program].name) <
                   std::tie(b.shader, b.matchKind, b.hash, b.priority, plan_->programs_[b.program].name);
        });
        std::sort(plan_->shaderRegex_.begin(), plan_->shaderRegex_.end(), [](const auto& a, const auto& b) {
            return a.name < b.name;
        });
        plan_->overrideIndices_.assign(plan_->programs_.size(), InvalidId);
        for (Id i = 0; i < plan_->overrides_.size(); ++i) plan_->overrideIndices_[plan_->overrides_[i].program] = i;
        // Accumulate potential binding requirements through aliases, once at Load.
        // All referenced preloaded resources receive the plan's conservative mask;
        // no recursive graph/flags propagation occurs in a draw.
        for (auto& resource : plan_->resources_) resource.bindings = plan_->touches_;
        std::vector<std::uint8_t> reach(plan_->programs_.size());
        std::function<void(Id, std::uint8_t)> mark = [&](Id id, std::uint8_t bit) {
            if (id >= reach.size() || (reach[id] & bit)) return;
            reach[id] |= bit;
            for (const auto& phase : plan_->programs_[id].commands)
                for (const auto& command : phase) if (command.op == CommandOp::Run) mark(command.index, bit);
        };
        for (Id id = 0; id < plan_->programs_.size(); ++id)
        {
            const auto kind = plan_->programs_[id].kind;
            if (kind == ProgramKind::Present || kind == ProgramKind::SoftwareMouse || kind == ProgramKind::SoftwareMouseBW)
                mark(id, 1); // declaration capability only; never an automatic entry
            if (kind == ProgramKind::ShaderOverride || kind == ProgramKind::ShaderRegex || kind == ProgramKind::TextureOverride) mark(id, 2);
        }
        for (const auto& section : request_.config->sections) if (LowerAscii(section.name) == "device")
            for (const auto& entry : section.entries)
            {
                const auto value = LowerAscii(Trim(entry.value));
                if (entry.key == "hide_cursor")
                {
                    if (value != "0" && value != "1" && value != "false" && value != "true")
                        Fail(entry.source, "hide_cursor requires an actual boolean");
                    plan_->hideCursor_ = value == "1" || value == "true";
                }
                // F6 owns actual client coordinates and has no XXMI upscaling
                // hook. Refuse explicit virtual-coordinate policy, never guess
                // GAME_INTERNAL dimensions from an SR texture.
                if (entry.key == "upscaling" && value != "0" && value != "false")
                    Fail(entry.source, "F6 mouse owns actual client pixels; Device.upscaling (SCREEN_UPSCALING) is unsupported");
                if (entry.key == "cursor_upscaling_bypass" && value != "0" && value != "1" && value != "false" && value != "true")
                    Fail(entry.source, "cursor_upscaling_bypass requires an actual boolean");
            }
        plan_->nativeOnly_.resize(reach.size());
        for (Id id = 0; id < reach.size(); ++id) plan_->nativeOnly_[id] = reach[id] == 1;
        // The GS must read exactly the Position object written by the CS, not
        // another equally shaped buffer or a separately inferred alias.
        for (const auto& render : plan_->renderText_)
            if (std::none_of(plan_->formatText_.begin(), plan_->formatText_.end(), [&](const auto& format) {
                return format.shader == render.shader && format.font == render.font &&
                    format.text == render.text && format.parameters == render.parameters &&
                    format.positions == render.positions; }))
                Fail(render.source, "RenderText has no paired fixed FormatText output/resource contract");
        return std::move(plan_);
    }
};
} // namespace detail

BufferResizeResult CompileBufferResizeRules(const Config& config)
{
    BufferResizeResult result;
    const auto fail = [&](const SourceLocation& at, const char* text) {
        result.diagnostics.push_back({DiagnosticCode::Unsupported, at, text});
    };
    for (const auto& section : config.sections)
    {
        const bool requested = std::any_of(section.entries.begin(), section.entries.end(), [](const auto& e) {
            return e.key == "override_vertex_count" || e.key == "override_byte_stride" || e.key == "uav_byte_stride";
        });
        if (!requested)
        {
            if (section.name.find("VertexLimitRaise") != std::string::npos)
                fail(section.source, "Unsupported legacy implicit VertexLimitRaise size; explicit count and stride required");
            continue;
        }
        BufferResizeRule rule; bool hash = false, count = false, stride = false, uav = false;
        bool valid = LowerAscii(section.name).starts_with("textureoverride");
        for (const auto& e : section.entries)
        {
            const auto value = LowerAscii(Trim(e.value));
            auto number = [&](std::uint32_t& out, bool& seen, int base) {
                if (seen || !e.hasEquals || value.empty()) { valid = false; return; }
                seen = true; const auto r = std::from_chars(value.data(), value.data() + value.size(), out, base);
                valid &= r.ec == std::errc() && r.ptr == value.data() + value.size();
            };
            if (e.key == "hash") { number(rule.hash, hash, 16); valid &= value.size() == 8; }
            else if (e.key == "override_vertex_count") number(rule.vertexCount, count, 10);
            else if (e.key == "override_byte_stride") number(rule.byteStride, stride, 10);
            else if (e.key == "uav_byte_stride") number(rule.uavStride, uav, 10);
            else if (e.key.starts_with("match_")) valid = false;
        }
        const auto bytes = std::uint64_t(rule.vertexCount) * rule.byteStride;
        if (!valid || !hash || !count || !stride || !rule.vertexCount || !rule.byteStride ||
            !bytes || bytes > 128u * 1024u * 1024u || bytes % 4 || (uav && (!rule.uavStride || bytes % rule.uavStride)))
        { fail(section.source, "Unsupported explicit buffer resize: exact hash, positive integral size/stride, no draw-dependent matching required"); continue; }
        rule.byteWidth = static_cast<std::uint32_t>(bytes);
        auto same = std::find_if(result.rules.begin(), result.rules.end(), [&](const auto& r) { return r.hash == rule.hash; });
        if (same != result.rules.end())
        {
            if (same->byteStride != rule.byteStride || same->uavStride != rule.uavStride)
            { fail(section.source, "Unsupported conflicting per-hash UAV/vertex stride"); continue; }
            if (rule.byteWidth > same->byteWidth) *same = rule;
        }
        else if (result.rules.size() < 128) result.rules.push_back(rule);
        else fail(section.source, "Buffer resize rule capacity exceeded");
    }
    if (!result) result.rules.clear();
    return result;
}

CompileResult CompilePlan(const CompileRequest& request)
{
    try { return {detail::Compiler(request).Compile(), {}}; }
    catch (const detail::CompileFailure& e) { return {nullptr, {e.diagnostic}}; }
    catch (const std::exception& e) { return {nullptr, {{DiagnosticCode::Io, {}, e.what()}}}; }
}

std::pair<const OverrideSpec*, std::size_t> Plan::Candidates(std::uint64_t hash, bool shader) const noexcept
{
    const auto key = std::make_tuple(shader, OverrideMatchKind::Hash, hash);
    const auto first = std::lower_bound(overrides_.begin(), overrides_.end(), key,
        [](const auto& entry, const auto& k) { return std::make_tuple(entry.shader, entry.matchKind, entry.hash) < k; });
    const auto last = std::upper_bound(first, overrides_.end(), key,
        [](const auto& k, const auto& entry) { return k < std::make_tuple(entry.shader, entry.matchKind, entry.hash); });
    return {first == last ? nullptr : &*first, static_cast<std::size_t>(last - first)};
}
std::pair<const OverrideSpec*, std::size_t> Plan::BufferDescriptorCandidates() const noexcept
{
    const auto key = std::make_pair(false, OverrideMatchKind::BufferDescriptor);
    const auto first = std::lower_bound(overrides_.begin(), overrides_.end(), key,
        [](const auto& entry, const auto& k) { return std::make_pair(entry.shader, entry.matchKind) < k; });
    const auto last = std::upper_bound(first, overrides_.end(), key,
        [](const auto& k, const auto& entry) { return k < std::make_pair(entry.shader, entry.matchKind); });
    return {first == last ? nullptr : &*first, static_cast<std::size_t>(last - first)};
}
std::pair<const OverrideSpec*, std::size_t> Plan::TextureDescriptorCandidates() const noexcept
{
    const auto key = std::make_pair(false, OverrideMatchKind::Texture2DDescriptor);
    const auto first = std::lower_bound(overrides_.begin(), overrides_.end(), key,
        [](const auto& entry, const auto& k) { return std::make_pair(entry.shader, entry.matchKind) < k; });
    const auto last = std::upper_bound(first, overrides_.end(), key,
        [](const auto& k, const auto& entry) { return k < std::make_pair(entry.shader, entry.matchKind); });
    return {first == last ? nullptr : &*first, static_cast<std::size_t>(last - first)};
}
} // namespace gimi
