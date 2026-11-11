#pragma once
// Il2CppCallSiteHook.h — 游戏 il2cpp 驱动方法调用点钩子（Phase 1.5 落地）。
// 背景：7.0 的 FFX_FSR2::Render 位于 exe_base + 0x06B59670（经 g_MethodPointers
// 全量匹配 + 字段偏移字节验证，见 D:\Dump\work\fsr2-callsite-70.md）。
// 本模块对 Render 做 12 字节绝对跳转 patch（mov rax,stub; jmp rax）：
//   observe 模式：stub 计数后透传原函数（零行为改变，验证调用点 + 调用频率）；
//   skip 模式：stub 直接返回（实验性：会切断桥 Mode 2 的 jitter/触发链，勿与
//   Fsr2TranslationMode=2 同开；仅用于隔离测试）。
// 安装前验证 Render/UpdateCommandBuffer 序言字节，不匹配则放弃（回退 draw 层家族跳过）。

#include <cstddef>
#include <cstdint>
#include <vector>
#include "../GenshinFsrUiObservation.h"
#include "../GenshinBridgeRecipes.h"

namespace il2cpp_callsite
{
struct Config
{
    bool skip_render = false;
    std::uint32_t render_rva = GenshinBridgeRecipes::Legacy70.render;            // FFX_FSR2::Render (7.0)
    std::uint32_t update_cmd_buffer_rva = GenshinBridgeRecipes::Legacy70.ucb; // UpdateCommandBuffer (布局校验用)
    bool capture_camera = false;                      // 2026-08-26：抓取游戏真实相机参数
    std::uint32_t camera_rva = GenshinBridgeRecipes::Legacy70.camera;            // ConfigureJitteredProjectionMatrix (7.0，每帧被 OnPreCull 调用)
    bool capture_projection = false;                  // 只读抓取已构造的 jittered projection matrix
    std::uint32_t projection_setter_rva = GenshinBridgeRecipes::Legacy70.projectionSetter; // Camera setter: rcx=Camera*, rdx=Matrix4x4*
};

// stub 从 FFX_FSR2 实例捕获的每帧参数（字段偏移来自 6.6/6.7 dump，getter 字节已实证）
struct CapturedParams
{
    std::uint64_t instance = 0; // this 指针（区分多实例/多相机渲染）
    std::uint64_t context = 0;  // Render 第二参数（渲染上下文，可能含相机参数）
    std::uint32_t render_w = 0, render_h = 0;
    std::uint32_t display_w = 0, display_h = 0;
    std::uint32_t frame_index = 0;
    float jitter_x = 0.0f, jitter_y = 0.0f;
};

// R691 actual CPU camera values. Every array is Unity column-major,
// column-vector convention. No member is an sl::Constants matrix.
enum class CameraProvenance : std::uint8_t { None, ConfigureSetter, RenderObservation };
enum class CameraSlotMode : std::uint8_t { None, MainCache, WorkerMirror };
struct CameraSnapshot
{
    CameraProvenance provenance=CameraProvenance::None;
    // R729 passive candidate only: no getter ran and no writer is certified here.
    std::uint64_t observationSerial=0, observationContext=0, observationOwner=0;
    std::uint64_t primaryManagedCamera=0, selectedManagedCamera=0;
    std::uint64_t observedProjectionSlot=0, observedViewSlot=0;
    std::uint32_t observedMainThread=0;
    std::uint8_t selector=0, observedProjectionAutomatic=0, observedProjectionDirty=0, observedViewDirty=0;
    GenshinBridgeRecipes::PassiveCameraVersion observedProfile=GenshinBridgeRecipes::PassiveCameraVersion::None;
    CameraSlotMode observedSlotMode=CameraSlotMode::None;
    std::uint64_t instance=0, managedCamera=0, nativeCamera=0;
    std::uint64_t captureEpoch=0, captureTick=0, configureSerial=0, renderGeneration=0;
    std::uint32_t sourceFrame=0, configureThread=0, renderThread=0;
    float cpuProjectionJittered[16]{}; // exact Configure setter argument
    float cpuProjectionNoTaa[16]{}; // only this Configure's +2*j removed
    float cpuStoredProjection[16]{}; // Configure: C * setter input; Render: unchanged passive P slot bytes
    float cpuWorldToView[16]{}, cpuViewToWorld[16]{};
    float nearPlane=0, farPlane=0, fovDegrees=0, aspect=0, jitterX=0, jitterY=0;
    bool cpuValid=false, cpuNoTaaValid=false;
    bool gpuProjectionValid=false, historyValid=false;
    float cpuStoredProjectionNoTaa[16]{}; // C * P without this Configure TAA, CPU clip.
    std::uintptr_t graphicsDevice=0; // Engine gfx object, NOT ID3D12Device.
    std::uint8_t gpuClipControl=0, gpuReverseControl=0; // actual config +E0/+E1
    bool gpuProfileValid=false; // adapter provenance only, never depth/FG admission
};

// One concrete FFX_FSR2::Render invocation.  The draw hook must consume a
// token, not guess from a mutable "latest parameters" snapshot.
struct RenderToken
{
    CapturedParams params {};
    CameraSnapshot camera {}; // Frozen by g_inst_mutex with this exact token.
    std::uint64_t generation = 0;
    std::uint64_t capture_tick = 0;
    std::uint32_t captureThread = 0; // Exact Render callback thread, distinct from D3D consumer owner.
};

// 安装钩子。失败时原代码未改动（调用方应回退到 draw 层家族跳过）。
//
// ⚠️ 2026-09-23（审核项）：**失败原因通过 `out_reason` 如实报出**。
// 此前 6 条失败路径一律静默 `return false`，调用点只知道"失败了"，
// **分不清是 RVA 为 0、序言不匹配（游戏更新）还是 VirtualAlloc 失败** ——
// 而这三者的处置完全不同（前者是配置问题、中者是版本问题、后者是资源问题）。
//
// 为什么不用日志：本 TU **刻意不依赖 BridgeLogger**（测试目标只编译本 cpp，
// 引入日志会破坏其链接）。记录由调用点负责，本模块只负责**如实报告原因**。
// 取值：`bad_rva` / `render_prologue_mismatch` / `ucb_relation_mismatch` /
//       `ucb_prologue_mismatch` / `protect_failed` / `alloc_failed`
bool install(std::uint64_t exe_base, const Config &cfg, const char **out_reason = nullptr);
// 还原原字节（DLL_PROCESS_DETACH 调用）。
//
// ⚠️ 2026-09-23：**不再释放 stub**（详见实现处注释：还原只能阻止"新的"跳入，
// 无法排除已有线程正执行在 stub 内；stub 仅 64 字节，泄漏远优于崩溃）。
void shutdown();
bool active();
// observe/skip 模式下 stub 累计的 Render 调用次数。
std::uint64_t render_call_count();
// 参数捕获：每次 Render 调用（stub 透传时）更新；返回捕获代数（桥据此判断新鲜度）。
std::uint64_t params_generation();
bool last_params(CapturedParams &out);
// UI-only bounded read: never waits for Render, consumes tokens or changes capture.
bool try_read_ui_render_observation(GenshinLegacy::Ui::RenderObservation& out) noexcept;
// 2026-08-25（多实例修复）：按实例存储参数——主/UI 实例的 Render 各自更新自己的
// 参数与代次，避免 UI 实例污染主实例的 dispatch（参数错用 + 代次门控阻塞 = 交替输出）。
// instance=0 时返回全局最新（兼容旧调用方）。
bool last_params_for(std::uint64_t instance, CapturedParams &out, std::uint64_t &generation);
// Return the newest unconsumed Render token for this exact instance and render
// size.  A successful SDK dispatch must subsequently retire that generation
// and every older pending token for the same instance.
bool latest_pending_render_token_for(std::uint64_t instance,
                                     std::uint32_t render_w, std::uint32_t render_h,
                                     RenderToken &out);
// CaptureOnly reads the actual latest Render witness under one lock, without
// consuming or changing the SR FIFO. Multiple matching instances are ambiguous.
unsigned match_capture_render_token(std::uint32_t render_w,std::uint32_t render_h,
                                   std::uint32_t display_w,std::uint32_t display_h,
                                   std::uint64_t after_generation,RenderToken& out);
// Select the oldest eligible token of each matching instance under ONE lock.
// A nonzero instance narrows selection to a previously associated game output.
unsigned match_pending_render_token(std::uint32_t render_w, std::uint32_t render_h,
                                    std::uint32_t display_w, std::uint32_t display_h,
                                    std::uint64_t instance, RenderToken& out);
bool consume_render_token_for(std::uint64_t instance, std::uint64_t generation);
// Native context/drain boundaries revoke both SR FIFO and CaptureOnly witnesses.
// Normal successful SR consumption keeps its independent Present observation.
void retire_render_tokens();
// 2026-09-19（审核报告）：pending 环形槽每实例只有 4 个，写满时**覆盖最旧** token。
// 旧实现是静默的——桥侧只会看到"某个 Render 代次永远等不到 token"，
// 却无从判断是识别失败还是被环形覆盖。此计数让该情形**可观测**：
// 非零即说明 4 槽不足（同一实例在 4 次 Render 内未被消费），应扩大环形或查消费路径。
std::uint64_t pending_overflow_count();
// 已记录的实例列表（用于 draw→实例匹配）。
void known_instances(std::uint64_t *out, std::size_t capacity, std::size_t &count);
// 上一帧 jitter 需按实例隔离：桥用 instance 区分 jdelay 状态，故提供按实例的帧代次辅助。
std::uint64_t params_generation_for(std::uint64_t instance);

// 2026-08-26（AA 修复）：相机参数捕获——ConfigureJitteredProjectionMatrix(camera)
// R729 install_camera authenticates/enables passive existing-Render observation.
// No camera/scalar/setter hooks, thread enlistment, code patch or install retry.
// Values are consumed only through RenderToken.camera; legacy raw accessor stays empty.
// R708: error domain is explicit; predicate refusal has status=0, never a stale GetLastError.
// Caller owns retry scheduling. Only retryable=true permits a bounded new attempt.
// R715: peer access/lifecycle failures refuse the ENTIRE attempt. Retry means
// fresh authentication/enumeration, after any owned transaction has aborted.
// Persistent refusal exhausts the caller budget; a failed peer is never skipped.
struct CameraInstallResult
{
    const char* phase="not-attempted";
    const char* domain="predicate";
    std::uint32_t status=0, abortStatus=0, rva=0, threadId=0;
    bool retryable=false;
};
bool install_camera(std::uint64_t exe_base, const Config &cfg, CameraInstallResult* result=nullptr);
bool camera_ready();
std::uint64_t camera_generation();
std::size_t camera_floats(float *out, std::size_t capacity); // 拷贝捕获到的相机前段浮点

// 在 ConfigureJitteredProjectionMatrix 的已确认 setter 入口观察最终 4x4 矩阵。
// 只复制 setter 的 rdx 入参，绝不修改 Camera 或矩阵内容。
bool install_projection_setter(std::uint64_t exe_base, const Config &cfg);
bool projection_ready();
std::uint64_t projection_generation();
std::size_t projection_matrix(float *out, std::size_t capacity, std::uint64_t *camera_ptr);

// RVA 自动识别：扫描 exe 代码段找 render 序言 + ucb 序言（固定偏移配对）的候选。
// 国际服/版本更新时 RVA 偏移不同——用序言特征对齐（输出候选 RVA 供配置/验证）。
std::vector<std::uint32_t> scan_render_candidates(std::uint64_t exe_base,
                                                  std::uint32_t render_rva, std::uint32_t ucb_rva);

// 特征识别（优先于硬编码 RVA）：g_MethodPointers 全量指针 + FFX_FSR2 30 方法
// 尺寸序列 gap 匹配。当前已知布局见 GenshinBridgeRecipes；仅接受唯一最高分方法集。
// 较低分近似候选不算歧义，同分最高分不同方法集拒绝；不保证未来布局/ABI。
// 成功输出 render/ucb/camera RVA（VA-exe_base）；失败返回 false 且三个输出均为0。
// 仅初始化/既有有界重试调用；TryInstallEntry 失败保原生路径，不回落旧默认RVA。
bool detect_ffx12_method_rvas(std::uint64_t exe_base,
                              std::uint32_t &render_rva,
                              std::uint32_t &ucb_rva,
                              std::uint32_t &camera_rva);
} // namespace il2cpp_callsite
