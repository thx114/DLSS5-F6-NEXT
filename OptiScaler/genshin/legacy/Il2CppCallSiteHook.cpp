#include "Il2CppCallSiteHook.h"
#include <SysUtils.h>

#include <Windows.h>
#include <TlHelp32.h>
#include <bcrypt.h>
#include <detours/detours.h>
#include <intrin.h>
#include <cmath>
#include <cwchar>
#pragma comment(lib,"bcrypt.lib")

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

namespace il2cpp_callsite
{
namespace
{

namespace Recipes = GenshinBridgeRecipes;
constexpr std::size_t k_patch_len = Recipes::RenderPatchLength;
constexpr std::size_t k_stub_len = 64;
// Keep the existing independent instruction boundaries and stub bodies.
constexpr std::size_t k_camera_patch_len = Recipes::CameraPatchLength;
constexpr std::size_t k_camera_stub_len = 96;
constexpr const auto& k_camera_head = Recipes::CameraHead;
constexpr std::size_t k_projection_patch_len = Recipes::ProjectionPatchLength;
constexpr std::size_t k_projection_stub_len = 96;
constexpr const auto& k_projection_head = Recipes::ProjectionHead;
constexpr const auto& k_render_head = Recipes::RenderHead;
constexpr const auto& k_ucb_head = Recipes::UcbHead;

// ---- 2026-08-26（AA 修复）：相机参数捕获全局（声明须在 shutdown() 之前）----
std::uint8_t *g_camera_render = nullptr;
std::uint8_t g_camera_saved[k_camera_patch_len] {};
std::uint8_t *g_camera_stub = nullptr;
std::atomic_bool g_camera_active { false };
float g_camera_buf[64] {};
std::size_t g_camera_count = 0;
std::atomic_uint64_t g_camera_generation { 0 };
std::mutex g_camera_mutex;

std::uint8_t *g_projection_setter = nullptr;
std::uint8_t g_projection_saved[k_projection_patch_len] {};
std::uint8_t *g_projection_stub = nullptr;
std::atomic_bool g_projection_active { false };
float g_projection_matrix[16] {};
std::uint64_t g_projection_camera = 0;
std::atomic_uint64_t g_projection_generation { 0 };
std::mutex g_projection_mutex;

std::uint8_t *g_render = nullptr;
std::uint8_t g_saved[k_patch_len] {};
std::uint8_t *g_stub = nullptr;
std::atomic_bool g_active { false };
std::atomic_uint64_t g_render_calls { 0 };
std::atomic_uint64_t g_params_generation { 0 };
CapturedParams g_params {};
// 2026-08-25：按实例存储（最多 8 个实例；主/UI 各自独立代次与参数）
struct PerInstanceParams
{
    std::uint64_t instance = 0;
    std::uint64_t generation = 0;
    CapturedParams params {};
    RenderToken pending[4] {};
    RenderToken latest_capture {};
    std::size_t next_pending = 0;
};
PerInstanceParams g_inst_params[8] {};
std::size_t g_inst_count = 0;
std::mutex g_inst_mutex;
// Capture begins outside the mutex; retirement must also fence in-flight reads.
// Saturate rather than reuse an epoch and admit an old capture after wraparound.
std::atomic_uint64_t g_render_token_epoch { 0 };
// pending 环形槽溢出计数（审核报告：4 槽满时覆盖最旧 token 是静默的）。
// 每次"该槽位已有未消费 token 却被覆盖"时 +1；桥侧可据此判断环形是否不足。
std::atomic_uint64_t g_pending_overflow { 0 };

#include "CameraProducer.inl"

// 由 stub 调用的计数 + 参数捕获入口（rcx=this, rdx=context）。
// 字段偏移（7.0，getter 字节实证）：jitter Vector2@0x24, m_FrameIndex int@0x50,
// m_RenderSize Vector2Int@0x58, m_DisplaySize Vector2Int@0x68。
__declspec(noinline) void on_render_enter(void *this_ptr, void *context_ptr)
{
    const auto capture_epoch = g_render_token_epoch.load(std::memory_order_acquire);
    g_render_calls.fetch_add(1, std::memory_order_relaxed);
    if (this_ptr != nullptr)
    {
        const auto *p = static_cast<const std::uint8_t *>(this_ptr);
        CapturedParams cp {};
        cp.instance = reinterpret_cast<std::uint64_t>(this_ptr);
        cp.context = reinterpret_cast<std::uint64_t>(context_ptr);
        std::memcpy(&cp.render_w, p + Recipes::Render.renderWidth, sizeof(cp.render_w));
        std::memcpy(&cp.render_h, p + Recipes::Render.renderHeight, sizeof(cp.render_h));
        std::memcpy(&cp.display_w, p + Recipes::Render.displayWidth, sizeof(cp.display_w));
        std::memcpy(&cp.display_h, p + Recipes::Render.displayHeight, sizeof(cp.display_h));
        std::memcpy(&cp.frame_index, p + Recipes::Render.frameIndex, sizeof(cp.frame_index));
        std::memcpy(&cp.jitter_x, p + Recipes::Render.jitterX, sizeof(cp.jitter_x));
        std::memcpy(&cp.jitter_y, p + Recipes::Render.jitterY, sizeof(cp.jitter_y));
        // Passive camera reads stay outside g_inst_mutex; epoch/params are
        // rechecked before the immutable candidate joins this Render token.
        const auto cameraObservation=camera_observe_render(cp,capture_epoch);
        // 按实例记录（保持全局兼容 + 每实例代次）
        //
        // ⚠️ 数据竞争修复（2026-09-19，审核报告）：
        // 旧实现把 `g_params = cp` 与代次自增放在**锁外**（本行之前），而读方
        // last_params() / last_params_for() 在锁外读 `g_params` → 撕裂读
        // （读到一半新一半旧的 CapturedParams，导致 render/display 尺寸与
        // jitter 来自不同帧）。现在**统一在 g_inst_mutex 内**写全局快照。
        //
        // 为什么复用 g_inst_mutex 而不是新开 g_params_mutex：本函数**先**持
        // g_inst_mutex，读方 last_params_for() 也在 g_inst_mutex 内读全局快照。
        // 若为全局快照另开一把锁，就会出现"读方持 A 求 B、写方持 B 求 A"的
        // **锁序反转**。共用一把锁可保证锁序唯一。
        {
            std::lock_guard lock(g_inst_mutex);
            const auto current_epoch = g_render_token_epoch.load(std::memory_order_relaxed);
            if (capture_epoch != current_epoch || current_epoch == UINT64_MAX ||
                g_params_generation.load(std::memory_order_relaxed) == UINT64_MAX)
                return;
            const std::uint64_t generation =
                g_params_generation.fetch_add(1, std::memory_order_release) + 1;
            g_params = cp;
            const std::uint64_t inst = cp.instance;
            std::size_t slot = g_inst_count;
            for (std::size_t i = 0; i < g_inst_count; ++i)
            {
                if (g_inst_params[i].instance == inst)
                {
                    slot = i;
                    break;
                }
            }
            if (slot >= 8)
            {
                // 超过容量：LRU 覆盖最旧的（用 generation 最小者）
                std::size_t oldest = 0;
                for (std::size_t i = 1; i < 8; ++i)
                    if (g_inst_params[i].generation < g_inst_params[oldest].generation)
                        oldest = i;
                slot = oldest;
            }
            else if (slot == g_inst_count)
            {
                ++g_inst_count;
            }
            if (g_inst_params[slot].instance != inst)
                g_inst_params[slot] = PerInstanceParams {};
            PerInstanceParams &entry = g_inst_params[slot];
            entry.instance = inst;
            entry.generation = generation;
            entry.params = cp;
            RenderToken &token = entry.pending[entry.next_pending];
            // 溢出可观测（审核报告）：该槽位若已存在未消费的 token（generation != 0
            // 且非本次），说明 4 槽环形不足，旧实现会静默覆盖 → 桥侧表现为
            // "某代次永远等不到 token"。这里计数，让情形可诊断而非静默。
            if (token.generation != 0 && token.generation != generation)
                g_pending_overflow.fetch_add(1, std::memory_order_relaxed);
            token = RenderToken {};
            token.params = cp;
            token.generation = generation;
            token.capture_tick = GetTickCount64();
            token.captureThread = GetCurrentThreadId();
            camera_freeze_render_locked(token, cameraObservation, current_epoch);
            entry.latest_capture = token;
            entry.next_pending = (entry.next_pending + 1u) % 4u;
            // Phase19: frozen publication only, never reread engine camera memory.
            if (token.generation >= 1 && token.generation <= 384)
            {
                try
                {
                    const auto& c = token.camera;
                    std::ostringstream record;
                    record << "pid=" << GetCurrentProcessId() << " generation=" << token.generation
                        << " frame=" << cp.frame_index << " instance=" << cp.instance << " context=" << cp.context
                        << " capture_epoch=" << capture_epoch << " token_tick=" << token.capture_tick
                        << " token_thread=" << token.captureThread << " camera_generation=" << c.renderGeneration
                        << " camera_frame=" << c.sourceFrame << " camera_instance=" << c.instance
                        << " camera_epoch=" << c.captureEpoch << " camera_tick=" << c.captureTick
                        << " camera_thread=" << c.renderThread << " observationSerial=" << c.observationSerial
                        << " provenance=" << unsigned(c.provenance) << " observation_context=" << c.observationContext
                        << " observation_owner=" << c.observationOwner << " primary_camera=" << c.primaryManagedCamera
                        << " selected_camera=" << c.selectedManagedCamera << " managed_camera=" << c.managedCamera
                        << " native_camera=" << c.nativeCamera << " selector=" << unsigned(c.selector)
                        << " projection_slot=" << c.observedProjectionSlot << " view_slot=" << c.observedViewSlot
                        << " slot_mode=" << unsigned(c.observedSlotMode) << " main_thread=" << c.observedMainThread
                        << " automatic=" << unsigned(c.observedProjectionAutomatic)
                        << " projection_dirty=" << unsigned(c.observedProjectionDirty) << " view_dirty=" << unsigned(c.observedViewDirty)
                        << " cpu_valid=" << c.cpuValid << " gpu_profile_valid=" << c.gpuProfileValid
                        << " graphics_device=" << c.graphicsDevice << " gpu_clip=" << unsigned(c.gpuClipControl)
                        << " gpu_reverse=" << unsigned(c.gpuReverseControl)
                        << " render=" << cp.render_w << 'x' << cp.render_h << " display=" << cp.display_w << 'x' << cp.display_h
                        << " jitter=" << cp.jitter_x << ',' << cp.jitter_y << " camera_jitter=" << c.jitterX << ',' << c.jitterY;
                    const auto bits = [&record](const char* name, const float* values, unsigned count)
                    {
                        record << ' ' << name << "=hex32:" << std::hex;
                        for (unsigned i = 0; i < count; ++i)
                        {
                            std::uint32_t value = 0;
                            std::memcpy(&value, values + i, sizeof(value));
                            if (i) record << ',';
                            record << value;
                        }
                        record << std::dec;
                    };
                    bits("P", c.cpuStoredProjection, 16);
                    bits("V", c.cpuWorldToView, 16);
                    bits("jitter_x", &cp.jitter_x, 1); bits("jitter_y", &cp.jitter_y, 1);
                    LOG_INFO("R827 phase19 render publication: {} no_authority=true", record.str());
                }
                catch (...) {} // Logging must not escape into the Render stub.
            }
        }
    }
}

void write_u64(std::uint8_t *dst, std::uint64_t value)
{
    std::memcpy(dst, &value, 8);
}

// 构建 observe/skip 两种 stub。
//
// 2026-09-19（审核报告）：旧签名 `bool build_stub(...)` **恒 return true** ——
// 唯一的调用点也忽略返回值，失败分支不可达。但"恒成功"并非事实：stub 长度是
// 按序言长度算出来的，若 k_patch_len 或指令序列变化而 k_stub_len 未同步，
// 下面的 `while (p < k_stub_len)` 填充循环**不会**报错，反而会静默写出界。
//
// 因此改为 void + **编译期断言**（比运行时 bool 更强，且零运行时成本）：
// 让"stub 放不下"在**编译期**就失败，而不是留一个永远为 true 的返回值。
void build_stub(bool skip_render, std::uint8_t *render, std::uint8_t *stub)
{
    // 逐项长度（与下方指令序列一一对应，改动时编译器会强制同步）
    constexpr std::size_t k_prologue = 4 + 5 + 5 + 10 + 2;   // sub rsp / 两次 save / mov rax+call rax
    constexpr std::size_t k_observe_extra = 5 + 5 + 4 + k_patch_len + 10 + 2;
    constexpr std::size_t k_skip_extra = 4 + 1;
    static_assert(k_prologue + k_observe_extra <= k_stub_len,
                  "observe stub 超出 k_stub_len —— 同步调整 k_stub_len");
    static_assert(k_prologue + k_skip_extra <= k_stub_len,
                  "skip stub 超出 k_stub_len —— 同步调整 k_stub_len");
    const std::uint64_t counter = reinterpret_cast<std::uint64_t>(&on_render_enter);
    std::size_t p = 0;
    // Reserve shadow space plus two private save slots.  The earlier +0x18
    // slot sat inside the callee's shadow space and could be overwritten by
    // on_render_enter once token bookkeeping added further calls.
    // sub rsp, 0x38
    stub[p++] = 0x48; stub[p++] = 0x83; stub[p++] = 0xEC; stub[p++] = 0x38;
    // mov [rsp+0x20], rcx  (private save slot after 32-byte shadow space)
    stub[p++] = 0x48; stub[p++] = 0x89; stub[p++] = 0x4C; stub[p++] = 0x24; stub[p++] = 0x20;
    // mov [rsp+0x28], rdx
    stub[p++] = 0x48; stub[p++] = 0x89; stub[p++] = 0x54; stub[p++] = 0x24; stub[p++] = 0x28;
    // mov rax, on_render_enter; call rax
    stub[p++] = 0x48; stub[p++] = 0xB8; write_u64(stub + p, counter); p += 8;
    stub[p++] = 0xFF; stub[p++] = 0xD0;
    if (!skip_render)
    {
        // mov rcx, [rsp+0x20]; mov rdx, [rsp+0x28]
        stub[p++] = 0x48; stub[p++] = 0x8B; stub[p++] = 0x4C; stub[p++] = 0x24; stub[p++] = 0x20;
        stub[p++] = 0x48; stub[p++] = 0x8B; stub[p++] = 0x54; stub[p++] = 0x24; stub[p++] = 0x28;
        // add rsp, 0x38
        stub[p++] = 0x48; stub[p++] = 0x83; stub[p++] = 0xC4; stub[p++] = 0x38;
        // 原 12 字节序言
        std::memcpy(stub + p, g_saved, k_patch_len);
        p += k_patch_len;
        // mov rax, render+12; jmp rax
        stub[p++] = 0x48; stub[p++] = 0xB8;
        write_u64(stub + p, reinterpret_cast<std::uint64_t>(render) + k_patch_len);
        p += 8;
        stub[p++] = 0xFF; stub[p++] = 0xE0;
    }
    else
    {
        // add rsp, 0x38; ret（void 方法，直接返回）
        stub[p++] = 0x48; stub[p++] = 0x83; stub[p++] = 0xC4; stub[p++] = 0x38;
        stub[p++] = 0xC3;
    }
    while (p < k_stub_len)
        stub[p++] = 0x90; // nop 填充
}

} // namespace

// RVA 自动识别：扫描 exe 代码段（BaseOfCode..SizeOfCode）找 render 序言
// （k_render_head）且在 render_rva - ucb_rva 偏移处为 ucb 序言（k_ucb_head）的候选。
// 返回候选 RVA（供配置对齐——国际服/版本更新偏移不同）。
std::vector<std::uint32_t> scan_render_candidates(std::uint64_t exe_base,
                                                  std::uint32_t render_rva, std::uint32_t ucb_rva)
{
    std::vector<std::uint32_t> candidates;
    if (exe_base == 0 || render_rva <= ucb_rva)
        return candidates;
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(exe_base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return candidates;
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(exe_base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return candidates;
    const std::size_t text_size = nt->OptionalHeader.SizeOfCode;
    if (text_size == 0 || text_size > (1u << 30))
        return candidates;
    const std::uint8_t *text = reinterpret_cast<const std::uint8_t *>(exe_base + nt->OptionalHeader.BaseOfCode);
    const std::ptrdiff_t ucb_delta = static_cast<std::ptrdiff_t>(render_rva) - static_cast<std::ptrdiff_t>(ucb_rva);
    for (std::size_t i = 0; i + sizeof(k_render_head) <= text_size; ++i)
    {
        if (std::memcmp(text + i, k_render_head, sizeof(k_render_head)) != 0)
            continue;
        // ucb 在 render 上方 ucb_delta 处（text 偏移 = i - ucb_delta）
        if (i < static_cast<std::size_t>(ucb_delta))
            continue;
        const std::size_t ucb_off = i - static_cast<std::size_t>(ucb_delta);
        if (ucb_off + sizeof(k_ucb_head) > text_size)
            continue;
        if (std::memcmp(text + ucb_off, k_ucb_head, sizeof(k_ucb_head)) != 0)
            continue;
        // 配对命中：render 在 ucb 上方 ucb_delta 处（与国服相对布局一致）
        candidates.push_back(static_cast<std::uint32_t>(nt->OptionalHeader.BaseOfCode + i));
        if (candidates.size() >= 8)
            break;
    }
    return candidates;
}

// 失败路径统一出口（2026-09-23，审核项）：
// 记录原因供调用点如实上报，并保证 `false` 的语义不变。
// 本 TU 不依赖 BridgeLogger（测试目标只编译本 cpp），故只写 out 参数、不记日志。
namespace
{
bool fail_install(const char *reason, const char **out_reason)
{
    if (out_reason != nullptr)
        *out_reason = reason;
    return false;
}

// 构造写入目标的绝对跳转：`48 B8 <stub u64> FF E0`，余下用 `0x90` 填充。
//
// ⚠️ 三处 install（render / camera / projection）与 shutdown 的校验**必须共用本函数**。
// 若两边的构造方式漂移，shutdown 的"校验后还原"会**永远判定不匹配** →
// 静默跳过还原 → 原字节留在被 patch 状态，比不校验更糟。
void build_jmp_patch(std::uint8_t *patch, std::size_t len, const std::uint8_t *stub)
{
    std::memset(patch, 0x90, len);
    patch[0] = 0x48;
    patch[1] = 0xB8;
    write_u64(patch + 2, reinterpret_cast<std::uint64_t>(stub));
    patch[10] = 0xFF;
    patch[11] = 0xE0;
}
} // namespace

bool install(std::uint64_t exe_base, const Config &cfg, const char **out_reason)
{
    if (out_reason != nullptr)
        *out_reason = nullptr;
    if (g_active.load(std::memory_order_relaxed))
        return true;
    if (exe_base == 0 || cfg.render_rva == 0)
        return fail_install("bad_rva", out_reason);

    std::uint8_t *render = reinterpret_cast<std::uint8_t *>(exe_base + cfg.render_rva);
    std::uint8_t *ucb = reinterpret_cast<std::uint8_t *>(exe_base + cfg.update_cmd_buffer_rva);

    // 序言验证（RVA 或版本不匹配时安全放弃）
    if (std::memcmp(render, k_render_head, sizeof(k_render_head)) != 0)
        return fail_install("render_prologue_mismatch", out_reason);
    if (ucb >= render || render - ucb != static_cast<std::ptrdiff_t>(cfg.render_rva - cfg.update_cmd_buffer_rva))
        return fail_install("ucb_relation_mismatch", out_reason);
    if (std::memcmp(ucb, k_ucb_head, sizeof(k_ucb_head)) != 0)
        return fail_install("ucb_prologue_mismatch", out_reason);

    // 保存原字节
    DWORD old_protect = 0;
    if (!VirtualProtect(render, k_patch_len, PAGE_EXECUTE_READWRITE, &old_protect))
        return fail_install("protect_failed", out_reason);
    std::memcpy(g_saved, render, k_patch_len);

    // 分配可执行 stub
    g_stub = static_cast<std::uint8_t *>(
        VirtualAlloc(nullptr, k_stub_len, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (g_stub == nullptr)
    {
        VirtualProtect(render, k_patch_len, old_protect, &old_protect);
        return fail_install("alloc_failed", out_reason);
    }
    build_stub(cfg.skip_render, render, g_stub);

    // 写 12 字节绝对跳转: 48 B8 <stub> FF E0
    std::uint8_t patch[k_patch_len] {};
    build_jmp_patch(patch, k_patch_len, g_stub);
    std::memcpy(render, patch, k_patch_len);
    VirtualProtect(render, k_patch_len, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), render, k_patch_len);

    g_render = render;
    g_active.store(true, std::memory_order_release);
    return true;
}

void shutdown()
{
    // Terminal before every early return, including a failed first installation.
    // No detach/wait/installation lock is taken under loader lock.
    g_cpu_camera_stopped.store(true, std::memory_order_release);
    g_cpu_camera_enabled.store(false, std::memory_order_release);
    g_render_camera_enabled.store(false, std::memory_order_release);
    const bool render_active = g_active.exchange(false, std::memory_order_acq_rel);
    const bool camera_active = g_camera_active.exchange(false, std::memory_order_acq_rel);
    const bool projection_active = g_projection_active.exchange(false, std::memory_order_acq_rel);
    if (!render_active && !camera_active && !projection_active)
        return;

    // ---------------------------------------------------------------------
    // ⚠️ 2026-09-23（审核项）：**校验后还原**，不再盲目 `memcpy(g_saved)`。
    //
    // 为什么：若期间有别的插件在我们的跳转**之上**又写了补丁（典型：OptiScaler
    // 也 hook 同一个函数），盲目还原会**把对方的补丁抹掉** —— 对方的钩子静默失效，
    // 且表现为"用了本桥之后另一个插件就不工作了"，极难归因。
    // 现在：只有当前字节**仍是我们写入的那条 jmp** 时才还原；否则跳过。
    //
    // 注：跳过还原意味着原字节留在被 patch 状态 —— 但那正是**对方的**补丁，
    // 保持现状才是正确行为（谁最后写谁负责）。
    // ---------------------------------------------------------------------
    const auto restore_if_ours = [](std::uint8_t *target, std::size_t len,
                                    const std::uint8_t *stub, const std::uint8_t *saved) -> bool
    {
        if (target == nullptr)
            return false;
        std::uint8_t expected[k_camera_patch_len] {}; // 三者中最大
        build_jmp_patch(expected, len, stub);
        if (std::memcmp(target, expected, len) != 0)
            return false; // 已被他人改写 → 不动
        DWORD old_protect = 0;
        if (!VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &old_protect))
            return false;
        std::memcpy(target, saved, len);
        VirtualProtect(target, len, old_protect, &old_protect);
        FlushInstructionCache(GetCurrentProcess(), target, len);
        return true;
    };

    if (render_active && g_render != nullptr)
    {
        restore_if_ours(g_render, k_patch_len, g_stub, g_saved);
        g_render = nullptr;
    }
    if (camera_active && g_camera_render != nullptr)
    {
        restore_if_ours(g_camera_render, k_camera_patch_len, g_camera_stub, g_camera_saved);
        g_camera_render = nullptr;
    }
    if (projection_active && g_projection_setter != nullptr)
    {
        restore_if_ours(g_projection_setter, k_projection_patch_len, g_projection_stub, g_projection_saved);
        g_projection_setter = nullptr;
    }

    // ---------------------------------------------------------------------
    // ⚠️ 2026-09-23（审核项）：**刻意不 `VirtualFree` 三个 stub**。
    //
    // 原实现无条件释放。但还原原字节只能阻止**新的**跳入，**无法排除此刻已有线程
    // 正执行在 stub 内** —— `FreeLibrary` 并不终止其他线程（只有 `TerminateProcess` 会），
    // 于是那些线程会跳进已解除映射的内存 → **崩溃**。
    //
    // stub 各仅 64/… 字节，**泄漏它们远优于崩溃**；这里把指针置空，
    // 故不存在重复释放，重新 install 也会重新分配。
    // ---------------------------------------------------------------------
    g_stub = nullptr;
    g_camera_stub = nullptr;
    g_projection_stub = nullptr;

    // 清空 per-instance 表（2026-09-19：接入测试后实测发现的缺陷）。
    //
    // 为什么必须清：`g_inst_params[i].instance` 保存的是**游戏实例的裸指针**
    // （FFX_FSR2 实例地址）。游戏重建实例后旧地址会失效；若表不清空，
    // 这些**已失效的指针会残留到下一次 install()**：
    //   - `known_instances()` 会把它们当作"仍在的实例"报出去
    //     → 上层据此查 token/代次，拿到的是别的对象的陈旧数据
    //   - `params_generation_for()` 命中陈旧槽位 → 返回过期代次
    // 而实例地址可能被**复用**（栈/堆分配器），复用后就是"把新对象误判为旧实例"。
    //
    // 实测证据：`Il2CppCallSiteHookTest` 的 `run_test()` 用**栈上**实例调用一次后，
    // 该地址即被登记；随后 `run_per_instance_test()` 期望 `known_instances` 返回 2 个，
    // 实际返回 3 个（多出前一次测试留下的失效栈地址）。此前该测试未接入 CMake，
    // 所以这个缺陷一直不可见。
    //
    // ⚠️ 2026-09-20（实机回归：游戏窗口已关但进程残留）：
    // 本函数由 `DllMain(DLL_PROCESS_DETACH)` 调用（见 Dx11FsrBridge.cpp 的 DETACH 分支，
    // 那里写着"只做 loader-lock 安全的事"）—— **在 loader lock 持有期间**。
    // 而 `g_inst_mutex` 的写方是 `on_render_enter`，那是游戏**每次**
    // `FFX_FSR2::Render` 都走的热点路径，持锁期间做参数快照。
    // 若退出瞬间渲染线程正在锁内，DETACH 就会**永久阻塞** → loader lock 永不释放
    // → 进程退不掉（窗口已关、进程残留）。
    //
    // 修法：**不阻塞**。`try_lock` 拿不到就放弃清理 —— 进程即将终止，
    // 这张表的内存随进程回收，没有清理也不会有任何后果（它只在进程内使用）。
    // 这与 `BridgeLogger::shutdown` 对 `g_file_mutex` 用 `try_lock_for` 是同一处理
    //（那里的注释已经写明："阻塞等待就等于把主线程钉死在 loader lock 里"）。
    {
        std::unique_lock<std::mutex> lock(g_inst_mutex, std::defer_lock);
        if (lock.try_lock())
        {
            for (std::size_t i = 0; i < g_inst_count; ++i)
                g_inst_params[i] = PerInstanceParams {};
            g_inst_count = 0;
        }
        // 拿不到锁：直接返回。表内容由进程终止回收，不影响正确性。
    }
}

bool active()
{
    return g_active.load(std::memory_order_relaxed);
}

std::uint64_t render_call_count()
{
    return g_render_calls.load(std::memory_order_relaxed);
}

std::uint64_t params_generation()
{
    return g_params_generation.load(std::memory_order_acquire);
}

std::uint64_t pending_overflow_count()
{
    return g_pending_overflow.load(std::memory_order_relaxed);
}

bool try_read_ui_render_observation(GenshinLegacy::Ui::RenderObservation& out) noexcept
{
    std::unique_lock lock(g_inst_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    out = {};
    out.calls = g_render_calls.load(std::memory_order_relaxed);
    out.generation = g_params_generation.load(std::memory_order_relaxed);
    out.instanceCount = static_cast<std::uint32_t>(g_inst_count);
    for (std::size_t i = 0; i < g_inst_count; ++i)
        out.captureTick = (std::max)(out.captureTick, g_inst_params[i].latest_capture.capture_tick);
    if (g_inst_count == 1)
    {
        const auto& token = g_inst_params[0].latest_capture;
        out.generation = token.generation;
        out.renderWidth = token.params.render_w; out.renderHeight = token.params.render_h;
        out.displayWidth = token.params.display_w; out.displayHeight = token.params.display_h;
    }
    return true;
}

bool last_params(CapturedParams &out)
{
    // ⚠️ 必须持锁读：写方在 g_inst_mutex 内写 g_params（见 on_render_enter）。
    // 锁外读会撕裂（读到的 render/display 尺寸与 jitter 可能来自不同帧）。
    std::lock_guard lock(g_inst_mutex);
    out = g_params;
    return out.render_w != 0 && out.render_h != 0 && out.display_w != 0 && out.display_h != 0;
}

bool last_params_for(std::uint64_t instance, CapturedParams &out, std::uint64_t &generation)
{
    std::lock_guard lock(g_inst_mutex);
    if (instance == 0)
    {
        // 兼容旧调用方：返回全局最新（已在 g_inst_mutex 内，读到的是一致快照）
        out = g_params;
        generation = g_params_generation.load(std::memory_order_relaxed);
        return out.render_w != 0 && out.render_h != 0 && out.display_w != 0 && out.display_h != 0;
    }
    for (std::size_t i = 0; i < g_inst_count; ++i)
    {
        if (g_inst_params[i].instance == instance)
        {
            out = g_inst_params[i].params;
            generation = g_inst_params[i].generation;
            return true;
        }
    }
    return false;
}

bool latest_pending_render_token_for(std::uint64_t instance,
                                     std::uint32_t render_w, std::uint32_t render_h,
                                     RenderToken &out)
{
    std::lock_guard lock(g_inst_mutex);
    if (instance == 0 || render_w == 0 || render_h == 0)
        return false;
    const std::uint64_t now = GetTickCount64();
    for (std::size_t i = 0; i < g_inst_count; ++i)
    {
        const PerInstanceParams &entry = g_inst_params[i];
        if (entry.instance != instance)
            continue;
        // Render 可能在渲染线程 draw 之前连续记录多个 token。按“最新”选择
        // 会把较早 token 一起淘汰，造成 gen/frame 跳号；时域 AA 尤其在局部
        // 动态时会短暂失效。按 generation 顺序消费最早的有效 token。
        std::uint64_t best_generation = UINT64_MAX;
        for (const RenderToken &token : entry.pending)
        {
            if (token.generation != 0 && token.generation < best_generation &&
                token.capture_tick <= now && now - token.capture_tick <= 250 &&
                token.params.render_w == render_w && token.params.render_h == render_h)
            {
                best_generation = token.generation;
                out = token;
            }
        }
        // Phase19: first selection observation per source generation, under
        // the existing lock. The oldest-token selection above is unchanged.
        if (best_generation >= 1 && best_generation <= 384)
        {
            static bool recorded[384] {};
            if (!recorded[best_generation - 1])
            {
                recorded[best_generation - 1] = true;
                try
                {
                    std::ostringstream record;
                    record << "pid=" << GetCurrentProcessId() << " generation=" << best_generation
                        << " frame=" << out.params.frame_index << " instance=" << instance
                        << " context=" << out.params.context << " tick=" << out.capture_tick
                        << " observationSerial=" << out.camera.observationSerial << " now=" << now
                        << " requested_render=" << render_w << 'x' << render_h
                        << " next_pending=" << entry.next_pending
                        << " overflow=" << g_pending_overflow.load(std::memory_order_relaxed);
                    for (unsigned slot = 0; slot < 4; ++slot)
                    {
                        const auto& t = entry.pending[slot];
                        const bool nonzero = t.generation != 0, notFuture = t.capture_tick <= now;
                        const bool ageOk = notFuture && now - t.capture_tick <= 250;
                        const bool sizeOk = t.params.render_w == render_w && t.params.render_h == render_h;
                        record << " pending" << slot << "={generation:" << t.generation << ",frame:" << t.params.frame_index
                            << ",instance:" << t.params.instance << ",context:" << t.params.context
                            << ",tick:" << t.capture_tick << ",thread:" << t.captureThread
                            << ",observationSerial:" << t.camera.observationSerial << ",epoch:" << t.camera.captureEpoch
                            << ",render:" << t.params.render_w << 'x' << t.params.render_h
                            << ",display:" << t.params.display_w << 'x' << t.params.display_h
                            << ",nonzero:" << nonzero << ",not_future:" << notFuture << ",age_ok:" << ageOk
                            << ",size_ok:" << sizeOk << ",eligible:" << (nonzero && ageOk && sizeOk)
                            << ",selected:" << (t.generation == best_generation) << '}';
                    }
                    LOG_INFO("R827 phase19 token selection: {} selection=oldest first_selection_only=true no_authority=true", record.str());
                }
                catch (...) {} // Selection and return are independent of diagnostics.
            }
        }
        return best_generation != UINT64_MAX;
    }
    return false;
}

unsigned match_pending_render_token(std::uint32_t render_w,std::uint32_t render_h,
                                    std::uint32_t display_w,std::uint32_t display_h,
                                    std::uint64_t instance,RenderToken& out)
{
    out={};
    if(!render_w || !render_h || !display_w || !display_h)return 0;
    std::lock_guard lock(g_inst_mutex);
    const auto now=GetTickCount64();unsigned matches=0;
    for(std::size_t i=0;i<g_inst_count;++i) {
        const auto& entry=g_inst_params[i];
        if(instance && entry.instance!=instance)continue;
        const RenderToken* oldest=nullptr;
        for(const auto& token:entry.pending)
            if(token.generation && token.capture_tick<=now && now-token.capture_tick<=250 &&
               token.params.render_w==render_w && token.params.render_h==render_h &&
               (!oldest || token.generation<oldest->generation))oldest=&token;
        // Preserve the FIFO: a mismatched display does not skip an older Render.
        if(!oldest || oldest->params.display_w!=display_w || oldest->params.display_h!=display_h)continue;
        if(!matches || oldest->generation>out.generation)out=*oldest;
        ++matches;
    }
    if(matches!=1)out={};
    return matches;
}

unsigned match_capture_render_token(std::uint32_t render_w,std::uint32_t render_h,
                                   std::uint32_t display_w,std::uint32_t display_h,
                                   std::uint64_t after_generation,RenderToken& out)
{
    std::lock_guard lock(g_inst_mutex);
    out={};if(!render_w || !render_h || !display_w || !display_h)return 0;
    const auto now=GetTickCount64();unsigned matches=0;
    for(std::size_t i=0;i<g_inst_count;++i){const auto& token=g_inst_params[i].latest_capture;
        if(token.generation>after_generation && token.capture_tick<=now && now-token.capture_tick<=250 &&
            token.params.render_w==render_w && token.params.render_h==render_h &&
            token.params.display_w==display_w && token.params.display_h==display_h){out=token;++matches;}}
    if(matches!=1)out={};
    return matches;
}

bool consume_render_token_for(std::uint64_t instance, std::uint64_t generation)
{
    std::lock_guard lock(g_inst_mutex);
    if (instance == 0 || generation == 0)
        return false;
    for (std::size_t i = 0; i < g_inst_count; ++i)
    {
        PerInstanceParams &entry = g_inst_params[i];
        if (entry.instance != instance)
            continue;
        bool consumed = false;
        for (RenderToken &token : entry.pending)
        {
            if (token.generation != 0 && token.generation <= generation)
            {
                token = RenderToken {};
                consumed = true;
            }
        }
        return consumed;
    }
    return false;
}

void retire_render_tokens()
{
    std::lock_guard lock(g_inst_mutex);
    const auto epoch = g_render_token_epoch.load(std::memory_order_relaxed);
    if (epoch != UINT64_MAX)
        g_render_token_epoch.store(epoch + 1u, std::memory_order_release);
    camera_revoke_locked();
    for (std::size_t i = 0; i < g_inst_count; ++i)
    {
        auto &entry = g_inst_params[i];
        for (auto &token : entry.pending)
            token = RenderToken {};
        entry.latest_capture = RenderToken {};
    }
}

void known_instances(std::uint64_t *out, std::size_t capacity, std::size_t &count)
{
    std::lock_guard lock(g_inst_mutex);
    count = 0;
    for (std::size_t i = 0; i < g_inst_count && count < capacity; ++i)
        out[count++] = g_inst_params[i].instance;
}

std::uint64_t params_generation_for(std::uint64_t instance)
{
    std::lock_guard lock(g_inst_mutex);
    if (instance == 0)
        return g_params_generation.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < g_inst_count; ++i)
    {
        if (g_inst_params[i].instance == instance)
            return g_inst_params[i].generation;
    }
    return 0;
}

// ---- 2026-08-26（AA 修复）：相机参数捕获 ----
// ConfigureJitteredProjectionMatrix(camera) @0x06B558D0 每帧被 OnPreCull 调用（7.0 实证：
// Prepare 已内联，此函数是每帧携带 camera 指针的稳定调用点）。抓取相机结构体前 64 个
// float（512B），用于确定游戏真实 near/far/FOV（全部 RE 资料未确定；桥此前硬编码占位）。
// （全局变量 g_camera_* 声明在文件顶部。）

// MSVC 不允许同一函数同时含有 SEH 和需要 C++ 展开的对象（下面的 mutex lock）。
// 将不可信指针读取隔离在无析构对象的 helper 内，随后再进入 C++ 同步区。
static bool try_copy_camera(void *camera_ptr, float out[64])
{
    __try
    {
        std::memcpy(out, camera_ptr, sizeof(float) * 64u);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool try_copy_projection(const void *matrix_ptr, float out[16])
{
    __try
    {
        std::memcpy(out, matrix_ptr, sizeof(float) * 16u);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

__declspec(noinline) void on_projection_setter_enter(void *camera_ptr, const void *matrix_ptr)
{
    if (matrix_ptr == nullptr)
        return;
    float matrix[16] {};
    if (!try_copy_projection(matrix_ptr, matrix))
        return;
    std::lock_guard lock(g_projection_mutex);
    std::memcpy(g_projection_matrix, matrix, sizeof(matrix));
    g_projection_camera = reinterpret_cast<std::uint64_t>(camera_ptr);
    g_projection_generation.fetch_add(1, std::memory_order_release);
}

// 由 stub 调用（rcx=this, rdx=camera）。SEH 保护：camera 指针可能非法/越界。
__declspec(noinline) void on_camera_enter(void *this_ptr, void *camera_ptr)
{
    if (camera_ptr == nullptr)
        return;
    float buf[64] {};
    if (!try_copy_camera(camera_ptr, buf))
        return;
    std::lock_guard lock(g_camera_mutex);
    std::memcpy(g_camera_buf, buf, sizeof(buf));
    g_camera_count = 64;
    g_camera_generation.fetch_add(1, std::memory_order_release);
}

// 构建相机捕获 stub：保存 rcx/rdx → 调 on_camera_enter → 恢复 → 透传原函数。
bool build_camera_stub(std::uint8_t *target, std::uint8_t *stub)
{
    const std::uint64_t observer = reinterpret_cast<std::uint64_t>(&on_camera_enter);
    std::size_t p = 0;
    // Shadow space plus two private save slots.  Saving at +0x18/+0x20 put
    // the old stub inside the callee's shadow space and allowed the observer
    // to overwrite rcx/rdx before the original function resumed.
    // sub rsp, 0x38
    stub[p++] = 0x48; stub[p++] = 0x83; stub[p++] = 0xEC; stub[p++] = 0x38;
    // mov [rsp+0x20], rcx（this）; mov [rsp+0x28], rdx（camera）
    stub[p++] = 0x48; stub[p++] = 0x89; stub[p++] = 0x4C; stub[p++] = 0x24; stub[p++] = 0x20;
    stub[p++] = 0x48; stub[p++] = 0x89; stub[p++] = 0x54; stub[p++] = 0x24; stub[p++] = 0x28;
    // mov rax, on_camera_enter; call rax
    stub[p++] = 0x48; stub[p++] = 0xB8; write_u64(stub + p, observer); p += 8;
    stub[p++] = 0xFF; stub[p++] = 0xD0;
    // mov rcx, [rsp+0x20]; mov rdx, [rsp+0x28]
    stub[p++] = 0x48; stub[p++] = 0x8B; stub[p++] = 0x4C; stub[p++] = 0x24; stub[p++] = 0x20;
    stub[p++] = 0x48; stub[p++] = 0x8B; stub[p++] = 0x54; stub[p++] = 0x24; stub[p++] = 0x28;
    // add rsp, 0x38
    stub[p++] = 0x48; stub[p++] = 0x83; stub[p++] = 0xC4; stub[p++] = 0x38;
    // 原完整 14 字节序言
    std::memcpy(stub + p, g_camera_saved, k_camera_patch_len);
    p += k_camera_patch_len;
    // mov rax, target+14; jmp rax
    stub[p++] = 0x48; stub[p++] = 0xB8;
    write_u64(stub + p, reinterpret_cast<std::uint64_t>(target) + k_camera_patch_len);
    p += 8;
    stub[p++] = 0xFF; stub[p++] = 0xE0;
    while (p < k_camera_stub_len)
        stub[p++] = 0x90;
    return true;
}

bool install_camera(std::uint64_t exe_base, const Config &cfg, CameraInstallResult* result)
{
    CameraInstallResult local{};
    // R729 compatibility API: authenticate passive observation, no new patch.
    return enable_render_camera_observation(exe_base,cfg,result?*result:local);
}

bool camera_ready()
{
    return g_camera_active.load(std::memory_order_relaxed) && g_camera_count > 0;
}

std::uint64_t camera_generation()
{
    return g_camera_generation.load(std::memory_order_acquire);
}

std::size_t camera_floats(float *out, std::size_t capacity)
{
    std::lock_guard lock(g_camera_mutex);
    const std::size_t n = g_camera_count < capacity ? g_camera_count : capacity;
    std::memcpy(out, g_camera_buf, n * sizeof(float));
    return n;
}

bool build_projection_stub(std::uint8_t *target, std::uint8_t *stub)
{
    const std::uint64_t observer = reinterpret_cast<std::uint64_t>(&on_projection_setter_enter);
    std::size_t p = 0;
    // Shadow space plus two private saved argument slots.
    stub[p++] = 0x48; stub[p++] = 0x83; stub[p++] = 0xEC; stub[p++] = 0x38;
    stub[p++] = 0x48; stub[p++] = 0x89; stub[p++] = 0x4C; stub[p++] = 0x24; stub[p++] = 0x20;
    stub[p++] = 0x48; stub[p++] = 0x89; stub[p++] = 0x54; stub[p++] = 0x24; stub[p++] = 0x28;
    stub[p++] = 0x48; stub[p++] = 0xB8; write_u64(stub + p, observer); p += 8;
    stub[p++] = 0xFF; stub[p++] = 0xD0;
    stub[p++] = 0x48; stub[p++] = 0x8B; stub[p++] = 0x4C; stub[p++] = 0x24; stub[p++] = 0x20;
    stub[p++] = 0x48; stub[p++] = 0x8B; stub[p++] = 0x54; stub[p++] = 0x24; stub[p++] = 0x28;
    stub[p++] = 0x48; stub[p++] = 0x83; stub[p++] = 0xC4; stub[p++] = 0x38;
    std::memcpy(stub + p, g_projection_saved, k_projection_patch_len);
    p += k_projection_patch_len;
    stub[p++] = 0x48; stub[p++] = 0xB8;
    write_u64(stub + p, reinterpret_cast<std::uint64_t>(target) + k_projection_patch_len);
    p += 8;
    stub[p++] = 0xFF; stub[p++] = 0xE0;
    while (p < k_projection_stub_len)
        stub[p++] = 0x90;
    return true;
}

bool install_projection_setter(std::uint64_t, const Config &)
{
    // R691 owns native setter exclusively through authenticated Detours.
    return false;
}

bool projection_ready()
{
    return g_projection_active.load(std::memory_order_relaxed) &&
        g_projection_generation.load(std::memory_order_acquire) != 0;
}

std::uint64_t projection_generation()
{
    return g_projection_generation.load(std::memory_order_acquire);
}

std::size_t projection_matrix(float *out, std::size_t capacity, std::uint64_t *camera_ptr)
{
    if (out == nullptr || capacity < 16)
        return 0;
    std::lock_guard lock(g_projection_mutex);
    std::memcpy(out, g_projection_matrix, sizeof(g_projection_matrix));
    if (camera_ptr != nullptr)
        *camera_ptr = g_projection_camera;
    return 16;
}

// 特征识别（优先于硬编码 RVA）：g_MethodPointers 全量指针 + FFX_FSR2 30 方法尺寸
// 序列 gap 匹配（最高分须唯一；布局不支持或同分歧义则拒绝）。内存版（游戏 exe 已加载——
// 扫描 .rdata/.data 中指向 il2cpp 段的 u64 指针）。
bool detect_ffx12_method_rvas(std::uint64_t exe_base,
                              std::uint32_t &render_rva,
                              std::uint32_t &ucb_rva,
                              std::uint32_t &camera_rva)
{
    render_rva = ucb_rva = camera_rva = 0;
    const auto& recipe = GenshinBridgeRecipes::GameMethods;
    // This detector is invoked only by the existing bounded install attempts.
    // Reuse the product logger here, never in a per-draw/frame scan.
    const auto result = [&recipe](bool matched, const char* state, int exact = -1,
                                  std::size_t bestSets = 0) {
        LOG_INFO("Genshin internal FSR method recipe: pid={} recipe={} source_commit={} state={} exact={} best_sets={}",
            GetCurrentProcessId(), recipe.recipeId, recipe.sourceCommit, state, exact, bestSets);
        return matched;
    };
    if (exe_base == 0)
        return result(false, "invalid-exe-base");
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(exe_base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return result(false, "invalid-dos-header");
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(exe_base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return result(false, "invalid-nt-header");

    const std::uint8_t *base = reinterpret_cast<const std::uint8_t *>(exe_base);
    // 1) 段表：找 il2cpp 段（方法代码）与数据段（.rdata/.data——g_MethodPointers 所在）
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    std::uint64_t il2_va = 0, il2_end = 0;
    struct DataSec { std::uint64_t va, size; };
    std::vector<DataSec> data_secs;
    for (std::uint16_t i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        const IMAGE_SECTION_HEADER &s = sec[i];
        char name[9] {};
        std::memcpy(name, s.Name, 8);
        const std::uint64_t va = s.VirtualAddress;
        if (std::strcmp(name, "il2cpp") == 0)
        {
            il2_va = va;
            il2_end = va + s.Misc.VirtualSize;
        }
        else if (s.Characteristics & IMAGE_SCN_MEM_READ &&
                 !(s.Characteristics & IMAGE_SCN_MEM_EXECUTE) &&
                 (std::strcmp(name, ".rdata") == 0 || std::strcmp(name, ".data") == 0))
        {
            data_secs.push_back({va, s.Misc.VirtualSize});
        }
    }
    if (il2_va == 0 || data_secs.empty())
        return result(false, "missing-method-sections");
    const std::uint64_t il2_lo = il2_va, il2_hi = il2_end;

    // 2) 收集数据段中指向 il2cpp 段的 u64（g_MethodPointers 候选）
    std::vector<std::uint32_t> rvas;
    for (const DataSec &ds : data_secs)
    {
        const std::size_t cnt = static_cast<std::size_t>(ds.size / 8);
        for (std::size_t i = 0; i < cnt; ++i)
        {
            const std::uint64_t v = *reinterpret_cast<const std::uint64_t *>(base + ds.va + i * 8);
            if (v >= exe_base + il2_lo && v < exe_base + il2_hi)
                rvas.push_back(static_cast<std::uint32_t>(v - exe_base));
        }
    }
    if (rvas.size() < recipe.minimumPointers)
        return result(false, "insufficient-method-pointers");
    std::sort(rvas.begin(), rvas.end());
    rvas.erase(std::unique(rvas.begin(), rvas.end()), rvas.end());

    // 3) Preserve the existing 30-method window, 29 scored gaps, tolerance,
    // minimum exact count and signed total-drift bound. Score remains ONLY
    // exact-gap count: drift is eligibility, not a new tie breaker.
    std::size_t best_idx = rvas.size(), best_sets = 0;
    int best_exact = -1;
    for (std::size_t i = 0; i + GenshinBridgeRecipes::MethodCount < rvas.size(); ++i)
    {
        int exact = 0;
        std::int64_t drift = 0;
        bool ok = true;
        for (std::size_t k = 0; k < GenshinBridgeRecipes::MethodGapCount; ++k)
        {
            const auto& expected = recipe.gaps[k];
            const std::int64_t gap = static_cast<std::int64_t>(rvas[i + k + 1]) - rvas[i + k];
            const std::int64_t dev = gap - expected.size;
            if (dev > expected.tolerance || dev < -expected.tolerance) { ok = false; break; }
            if (dev == 0) ++exact;
            drift += dev;
        }
        if (!ok || drift < -recipe.maximumDrift || drift > recipe.maximumDrift)
            continue;
        if (exact > best_exact)
        {
            best_idx = i;
            best_exact = exact;
            best_sets = 1; // A strictly better score clears any lower-score tie.
        }
        else if (exact == best_exact)
        {
            // rvas is sorted and deduplicated: different window starts are
            // different method sets, not repeated pointers to the same set.
            ++best_sets;
        }
    }
    if (best_idx == rvas.size() || best_exact < recipe.minimumExact)
        return result(false, "no-matching-method-set", best_exact, best_sets);
    if (best_sets != 1)
        return result(false, "ambiguous-best-method-sets", best_exact, best_sets);

    // No partial output on failure: TryInstallEntry retains EntryNotMatched
    // and the existing native game path, without patching a first-found tie.
    render_rva = rvas[best_idx + recipe.renderIndex];
    ucb_rva = rvas[best_idx + recipe.ucbIndex];
    camera_rva = rvas[best_idx + recipe.cameraIndex];
    return result(true, "unique-best-method-set", best_exact, best_sets);
}

} // namespace il2cpp_callsite
