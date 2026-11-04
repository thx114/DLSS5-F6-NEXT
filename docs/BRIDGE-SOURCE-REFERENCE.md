# 原神 Bridge 上游跟版与源码参考

R1077整理：本参考由项目根移至docs，正文保留历史行号/证据；当前路径与状态见[HANDOFF](../HANDOFF.md)。历史裸路径按项目根解释。

## R1033/R1034 新版原神当前状态（2026-11-04）

最新用户实测R1034后FG深度存在、NR仍缺；PID39008模型真正depth=null，原始源已供但returned writer coverage0/253→0/279、picture mapping0（R849 L36847/50730）。R1039独立续修source/content/coverage到实际model闭包；不能沿用旧PID5284 coverage169/169，不能拿FG深度代NR映射/改AfterSR或撤门。

PID5284动态方法检测适配新Render0x6F22900/UCB0x6F22890/camera0x6F1EC20，SR继续；F6 passive相机profile仍锁旧Render0x6DF8860及旧camera/PE/code范围，首拒observer-render-identity，连带FG guides与NR picture-depth。维护不是自动解释，完整证据见audit/r1033-genshin-login-bridge-20261104/RESULT.md。

本轮只读官方HEAD/main仍26b0b7b7d7a0f30f05f213237c5449cc7d6b43a6，固定原文保该audit/incoming/upstream；无F6 passive profile直接更新。R1034在物理F6候选静态还原完整新版相机依赖并有据实现，不能只换RVA/统一平移/改pin放行。下方R936保维护流程与历史接点，旧Main路径/owner不恢复权限；只向F6集成构建部署。

## R936 当前权威维护入口（2026-11-02，Asia/Shanghai）

本节替代下方历史快照的当前维护流程与分工。已读当前 [AGENTS](../AGENTS.md)、[回归索引](REGRESSION-HISTORY.md) 和 [R869/R881/R894 基线比较](../audit/v3-r869-bridge-baseline-comparison-20261102/RESULT.md)。旧行号、来源摘要值与当时 owner 均只保历史，不重新授予部署权限或验证门禁。当前产物/运行验收由 Root 的 CURRENT/INDEX/RECORD 维护，本 worker 不改这三件。

### 来源与固定提交

- **U（官方）**：[AizawaHikaru233/genshin_fsr_brigde](https://github.com/AizawaHikaru233/genshin_fsr_brigde)。本轮实际执行 `git ls-remote --symref https://github.com/AizawaHikaru233/genshin_fsr_brigde.git HEAD`，返回 `refs/heads/main`，HEAD **`26b0b7b7d7a0f30f05f213237c5449cc7d6b43a6`**。这是本轮观察时刻的远端 HEAD，与 R869 基线相同；以后更新须重新查，不能永久称 latest，也不能据此宣称支持下一游戏版本。
- 已实际读取该提交的 `Dx11FsrBridge/{Il2CppCallSiteHook.cpp,Dx11FsrBridge.cpp,Ffx12Backend.cpp,Fsr2FamilyTakeover.h,Fsr2FamilyTakeover.cpp}` 原文。GitHub 未认证 API 返回 rate limit，故改读固定提交 raw 原文；没有使用可变 main 代替固定版本，没有拿到本次提交日期/标题，不猜元数据。
- **L（本地定制参考）**：`D:/CODE/genshin_fsr_brigde`，实际 `origin=thx114/genshin_fsr_brigde`，`upstream=AizawaHikaru233/genshin_fsr_brigde`；本地 HEAD `366e778221f5b0521ba9c95838f1e731d9db26db`。Dx11FsrBridge.cpp、CMakeLists、package INI、Fsr2TranslationLayer.cpp/.h 均 dirty，另有未跟踪定制文件。不得 pull/reset/checkout 覆盖，也不能把 L 或下方历史 B 当 U 当前字节。
- **O（F5 逻辑源码根）**：`D:/DATA/Documents/ChatGPT/DLSS F5 NEXT/work/r294-r266-build/OptiScaler`。下表 O 路径从此展开，U 路径从上述固定提交的 Dx11FsrBridge/ 展开；行号仅是本轮导航提示，后续以实际符号定位。
- O 的 `genshin/legacy`、`inputs` 已确认是 junction，物理目标在 `D:/DATA/Documents/ChatGPT/DLSS F5 NEXT/work/full-source-f5-v11-candidate-20261018/OptiScaler/` 对应目录。建立候选须复制当前实际字节到真实物理 worktree，不能复制 junction 充当隔离；其他写入目录同样先核真实目标。

下表与本节中未带目录的 `Genshin*.h/.cpp` 均指 `O/genshin/` 内文件，`legacy/...` 指 `O/genshin/legacy/...`；`inputs/...`、`native/...`、`upscalers/...`、`resource_tracking/...` 直接相对 O。例如 `GenshinLegacyCapture.cpp` 的完整逻辑路径是 `D:/DATA/Documents/ChatGPT/DLSS F5 NEXT/work/r294-r266-build/OptiScaler/genshin/GenshinLegacyCapture.cpp`。

### 版本敏感点：官方 → F5

| 版本敏感项 | U 固定提交中的符号与事实 | O 准确接点与同步边界 |
| --- | --- | --- |
| 方法表结构 pattern，而非固定 RVA | `Il2CppCallSiteHook.cpp:879–918`，`detect_ffx12_method_rvas` 的 k_seq/tol_for/评分/索引。30 方法窗口评分 29 gap，最少 64 pointers、20 exact、总 drift ±0x1000，camera/UCB/Render 索引 19/24/25。 | `genshin/GenshinBridgeRecipes.h:16–43` 的 `GameMethods` 已集中来源提交、gaps/tolerance/阈值/索引；`genshin/legacy/Il2CppCallSiteHook.cpp:941–1050` 消费。保本地最高分唯一性拒绝 `ambiguous-best-method-sets`，不照抄上游先取一组，也不放宽 tolerance/exact。 |
| 序言与完整指令边界 | `Il2CppCallSiteHook.cpp:18–40` 的 k_patch_len/k_render_head/k_ucb_head；Render 12 字节。camera/projection 独立 14/16 字节也见此提交原文。 | `genshin/GenshinBridgeRecipes.h:52–70` 的 RenderHead/UcbHead/RenderPatchLength、CameraHead/ProjectionHead；`legacy/Il2CppCallSiteHook.cpp:26–37,358–377` 消费并核序言。本地 camera/projection 来源与指令边界仍独立，同值不重新授 ABI；序言失配拒绝，不盲套旧 RVA。 |
| Render 对象字段 layout | `Il2CppCallSiteHook.cpp:87–106` 的 on_render_enter：jitter X/Y `0x24/0x28`，frame `0x50`，render W/H `0x58/0x5C`，display W/H `0x68/0x6C`。 | `genshin/GenshinBridgeRecipes.h:46–51` 的 `Render`；`legacy/Il2CppCallSiteHook.cpp:90–106` 已使用它。`legacy/CameraProducer.inl:66–73` 在 shared 前像仍重复同七偏移，R936 私有候选改为同一 Render，见下；未集成前须同时核这两处。 |
| accumulate/PRE/SMAA shader | `Fsr2FamilyTakeover.h:18–23` 的六个 k_*_hash，cpp 的 is_pre_pass/is_accumulate/is_smaa；`Dx11FsrBridge.cpp:197–199` target 默认值。accumulate `0x78057A29AF6C2D99`。 | `genshin/legacy/Fsr2FamilyTakeover.h:17–22`；`GenshinNativeMrtContract.h:18,116,144` 的 AccumulateShader/Validate/BridgeProfile；`GenshinNrProfile.h:20–35` 的 ShaderProfile。须联动核所有角色；NR 精确字节数/内容身份与数值语义不能随一个 FNV 常量盲换。`GenshinFamilySkip.cpp:266–293,548–594` 的真实对象/device/epoch/metadata 证明保留。 |
| scene/final-picture shader | `Dx11FsrBridge.cpp:5898` 的 k_final_scene_ps 有同值 `0x773B7BD7A2C6971F`。已读 U 主文件/Family 文件未找到本地 `0xB58064BC5041A048` 同值来源，其对应为 UNKNOWN，不把有限搜索称全 repo 无实现。 | `GenshinLegacyCapture.cpp:89,614–616` 的 SceneShader/PictureShader 及实际分支。这些最终画面消费者须独立核真实 draw/输出角色，不能用 accumulate 更新替代，也不能从 hash 命中直接授 picture 资格。 |
| game SRV/RTV/CB 槽与格式 | `Dx11FsrBridge.cpp:7972–8078` 的 identify_target_upscaler_resources：CB0≥464，以尺寸/格式/weight/history 识别；t0/t3 只作 tie-breaker。try_fsr2_translation_draw `:11857–11888,12092–12096` 按所选槽取真实资源。 | `GenshinLegacyCapture.cpp:129–149,197–220,268–274` 的 Bindings/ReadBindings，固定 color t0/depth t2/motion t3、RTV1、companion RTV0、CB0≥464；`GenshinNativeMrtContract.h:52–66,110–160` 再核 schema/format/subresource/owner。**F5 比 U 动态布局识别更窄**。变槽时联动 getter、NativeFrame、Validate/BridgeProfile、prepare 绑定与实际 RTV copyback，不能只改默认 slot 或猜测放宽。 |
| depth 标量与 motion 解码 | `Ffx12Backend.cpp:458–470` 的 g_depth_extract_hlsl；`:478–499` 的 g_motion_decode_d11_hlsl，decoded 为中心 0.498039 的负 signed-square，RAW/flip/deadzone 另行控制。 | `GenshinLegacyCapture.cpp:440–453` 的 PrepareShader：depth 标量、中心 `127.0/255.0`、`-sign(d)*(abs(d)*2)^2`，depth/motion Y flip 分开；`:1622–1660` 真 selected resource/prepare 交接。只记录约定相近，不称十进制近似与分数逐位相同，不盲同步 RAW/deadzone。`GenshinGameMotionReference.h`、`GenshinFgGameMotion*.h` 的 history 解释随真实编码变化复核。 |
| jitter / motion scale / phase | `Dx11FsrBridge.cpp:12141–12158` 优先 paired Render jitter；`:12284–12310` mode/delay，`:12356–12364` signed render-size motion scale；`Ffx12Backend.cpp:2217–2227` 交真实 dispatch。CB28 fallback 不是当前 Render token。 | `GenshinNativeMrtContract.h:276–299` 的 ApplyProfile，`GenshinLegacyCapture.cpp:1566` 消费；`inputs/GenshinFsrDx11.cpp` 向所选 SR 交 motionScale/尺寸。`GenshinGameMotionReference.h:24–31` 与 `GenshinFgGameMotion.h` 管 SameFrame/PreviousFramePhase。保设置、finite、同 instance/generation/前帧连续性；不以零 jitter 或改 mode 冒兼容。 |
| 相机/clip/矩阵私有 ABI | U camera hook/缓冲观察可参考，但不证明 F5 的 context→owner→camera、graphics device、实际 clip-depth layout。Render pattern 成功不等于这些 ABI 成功。 | `legacy/CameraProducer.inl:108–120,150–185,244–269` 的 GPU profile globals/method、owner/context offsets、matrix slots、jitter 去除；`GenshinTypedGuides.cpp`、`GenshinDepthMapping.h` 消费。独立核真实对应，未知仍缺 optional guide；SR 不因 optional camera 失证被阻断。 |
| 失败与旧 RVA | 上游动态 detector 返回值为主；历史 Config RVA 不能证明下一版本入口。 | `GenshinLegacyCapture.cpp:2853–2892` 的 TryInstallEntry：既有最多3次/5秒间隔，失败 EntryNotMatched；成功才填实际 render/UCB/camera 并 install。`legacy/Il2CppCallSiteHook.h:22–27` 的 Legacy70 只是历史默认，失败后不得启用它、恢复旧 fallback 或增加逐帧扫描。 |

这些是源码维护接点，**不是未知新游戏版本的自动兼容承诺**。方法自动定位成功后，字段、shader、槽位、motion 编码、相机 ABI 仍可独立变化。PS 缓存命中不证明上述游戏语义成立。

### 本地独有层：保接口，不全量同步

- `GenshinLegacyCapture.cpp::CapturePipeline::{BeforeDraw,AfterOriginalDraw,CompleteNativeReplacement}` 保实际路线和 companion 输出，不以 U 整段 Draw wrapper 覆盖，不一概断言全部路线先跑原生 draw。
- `inputs/GenshinFsrDx11.cpp::Dx11Owner::Submit` → 现有 selected feature Evaluate / `upscalers/IFeature_Dx11wDx12.cpp` → `genshin/GenshinDx12Transport.cpp::Lease::{Execute,Restored,Copyback}`。U g_runtime.dispatch 是对应阶段，不是同一 API/生命周期。保 owner/queue/fence/pending/reader/retire/quarantine 与部分提交失败后的真实资源持有。
- `GenshinLegacyCapture.cpp::CompleteNativeReplacement` 的实际 CopySubresourceRegion（本轮 :1766）立即写 game RTV/subresource，后续 HUD 有 reader。U `Ffx12Backend.cpp:2261–2278` 可延至 Present 的 async pending 不直接移植；只可沿已有真实提交合批显式 Flush，保 Wait/copy 顺序。
- `GenshinLegacyCapture.cpp::TakePresentNr`、`native/NativeDriverDx11.cpp::OnPresent`、`native/NativeDriver.cpp::RunFrame`、`GenshinProducerTransport.h::PreparedDepth`、`GenshinPresentNr.h::Ticket::DepthMatches/SelectFgPicture`、`inputs/FG/GenshinBridge_Present_Dx12.h` 保 FinishedPicture/Present、!finished、same-picture/owner/history，不能把 U 场景 SR guides 直接授最终画面资格。
- `GenshinFamilySkip.cpp`、`GenshinPsStateCache.h`、`GenshinPublicPsBinding.h`、`resource_tracking/GenericDepth_Dx11.cpp` 的 CPU PS cache 与 Present 安装 owner 是 F5 自有层。保五角色 receipt/retained alias、setter 后真 getter、版本化 refresh、Stop/owner/device 失效；不随上游替换，不授 GPU 完成资格。
- `genshin/GenshinNativeQueryProvenance.h` 等私有 D3D11 recipe 与游戏方法 recipe 分域；上游游戏入口变更不证明未知 driver/module ABI，也不为 CPU cache 重新要求 GPU 私有 recipe 成功。

### 每次游戏更新的跟版步骤

1. 读 CURRENT 的实际 shared/owner/产物、AGENTS 与 REGRESSION，保 dirty/正式 profiles；记录游戏版本/区域/exe、上游固定 commit、当前第一拒绝边界。未知写 UNKNOWN，recipe 名中 7-layout 不表示游戏版本已验收。
2. 对明确官方 URL 执行 `git ls-remote --symref … HEAD`，再读**该 commit**上述五个文件，与本节上次固定 commit 比较。只人工读源码，不新增下载器、运行时更新/加载器、网络依赖游戏路径、INI/API/线程。官方未改也不能推游戏未变。
3. 按表分类：pattern/indices/prologue → GameMethods/Heads；字段 → Render 与相机 reread；shader → family/MRT/精确 NR profile；槽格式 → getter/schema/prepare/copyback；motion/jitter → decode/scale/phase；私有 camera/driver → 单列证据。仅 RVA 位移且实际 layout 全仍成立时才可沿现有动态定位；不改历史 RVA 当修复。
4. 需要产品变更时建真物理 worktree，用写集的 **shared 当前 dirty 实际字节**作前像，局部实现；不以 HEAD 或 junction 冒隔离。固定提交与接点继续维护在本参考，不另建重复维护报告，不因拉取/编译扩大游戏支持范围。
5. 原神管理有限审新增 hunk，Root 串行安排集成；唯一 Sol low 沿根 BUILD-DEPLOY.ps1，按 actual exit/mtime 交付。Root协调记录，Sol high产品/复杂脚本；不恢复旧 Root编码/部署分工，不新增 tests/hash/manifest/验证器/独立审批门禁。
6. 用户继续手动测；只读对应 session.txt/Opt 日志，分开核 entry、真实 target draw/bindings/token、selected SR Evaluate、真实 RTV copyback、PS hit、FinishedPicture NR/FG 和用户视觉。成本复用 HookCpuTiming，命中率/Present间隔/部署不代净 CPU/Reflex/大世界验收。歧义/序言/schema/phase 失败保持原拒绝或 fallback，不能删 gate 强称兼容。

### R936 已落地与交接

- **文档 IMPLEMENTED**：本节补权威固定提交、六类敏感点及本地独有层映射、更新流程；下方历史摘录与摘要值保留，旧 rehash 门禁撤回，不重算历史值或做部署后检。
- **源码私有 IMPLEMENTED / NOT-INTEGRATED / NOT-BUILT / NOT-DEPLOYED**：真 worktree `worktrees/r936-genshin-upstream-maintenance`，分支 `codex/r936-genshin-upstream-maintenance`；写集仅 `work/full-source-f5-v11-candidate-20261018/OptiScaler/genshin/legacy/CameraProducer.inl::camera_params`。前像为该 shared 物理文件当前 dirty 字节，保 UTF-8 无 BOM/CRLF；隔离目录/文件均无 junction。
- 七处字段偏移改引用既有 `GenshinBridgeRecipes::Render`；仍依次读 render W/H、display W/H、frame、jitter X/Y，各4字节，后续非零尺寸/finite 检查不变。`Il2CppCallSiteHook.h` 已含该 recipe，cpp 内含 CameraProducer.inl；不新增版本头、不改私有 camera ABI、扫描/默认值/准入或 GPU 资源规则。
- 候选 [candidate.patch](../audit/v3-r936-genshin-upstream-maintenance-20261102/candidate.patch)：**1文件 / 1hunk / +10/-4**，是对 shared 实际字节的 delta。worktree 仅放写集与相关当前源码/头上下文副本，不是完整可构建 checkout；额外 header 副本只供审读，不是新产品写集。
- 本轮只回读该 hunk、核七偏移同值与 include 消费关系；无 tests/hash/manifest/fixture/验证器、build/deploy/config/game。待 Root 交原神管理有限审后局部承接；无 shared 产品 writer 或执行句柄。CURRENT/INDEX/RECORD 仍由 Root 记录。

---

# 历史快照：2026-10-24 / 2026-10-25 Bridge 对照

Historical inspection: 2026-10-24 (Asia/Shanghai). These sections retain then-current source mappings, ownership statements and line numbers as OBSERVATION-ONLY history. Current work follows the R936 maintenance entry above and current AGENTS.md; implementation is Sol high, fixed build/deployment is unique Sol low, and Root coordinates. Historical mappings are not a verified build, loaded DLL, functional repair or visual acceptance.

Original source directory, preserving its actual spelling: `D:\CODE\genshin_fsr_brigde\Dx11FsrBridge` (**B** below). Live Opt source directory: `D:\DATA\Documents\ChatGPT\DLSS F5 NEXT\work\r294-r266-build\OptiScaler` (**O** below). All B/O paths expand against these absolute roots. Line ranges are one-based, checked against actual files on the inspection date. SHA256 pins below identify bytes rather than coordination HEAD or deployment identity.

Read [AGENTS.md](../AGENTS.md) and [REGRESSION-HISTORY.md](REGRESSION-HISTORY.md). Keep the Genshin internal FSR/selected SR/FinishedPicture-Present NR route distinct from Star Rail and ZZZ routes. Runtime findings, unresolved questions and next-step conclusions remain in the canonical [phase42 original Bridge depth review](../audit/r827-depth-pool-accounting-20261023/phase42-original-bridge-depth-review.md) and its linked round record; this reference does not duplicate their runtime conclusions.

## Original function -> live Opt callsite

| Stage | Actual original Bridge | Live Opt correspondence and limit |
|---|---|---|
| Hook / Render | B/Il2CppCallSiteHook.cpp:86-101, `on_render_enter`: instance/context, frame, render/display size and raw jitter. B/Dx11FsrBridge.cpp:12531-12582, `hooked_draw_indexed` -> `inspect_target_upscaler_draw` -> `try_fsr2_translation_draw` (definition :10514). Per-instance generation/duplicate handling :10780-10814. | O/genshin/legacy/Il2CppCallSiteHook.cpp:101-120, `on_render_enter`, same fields plus camera observation. O/genshin/GenshinLegacyCapture.cpp:1069-1088 matches the Render token; :1150-1156 arms the native transaction and lets the real native draw run first; `CapturePipeline::AfterOriginalDraw`, :1514-1544, checks its return and calls `CompleteNativeReplacement` (:1158). This preserves companion MRT work. |
| Selected inputs | B/Dx11FsrBridge.cpp:8167-8215, `identify_target_upscaler_resources`, identifies output/history/LUT/depth-like layout; :10833-10903 queries actual accumulate SRVs/RTVs and uses draw-selected color/depth/motion/output slots. :10864-10884 publishes selected depth through DepthProvider. | O/genshin/GenshinLegacyCapture.cpp:137-190, `ReadBindings`, checks selected views, subresources, formats and dimensions. O/genshin/GenshinNativeMrtContract.h:141-151, `BridgeProfile`, pins the exact accumulate input slots {0,2,3} and output RTV1. O/genshin/GenshinLegacyCapture.cpp:1164-1170 captures that claimed depth SRV and separately associates ReturnedRaster evidence. This is not arbitrary world-DSV selection; provider publication is not camera proof. |
| Depth preparation | B/Ffx12Backend.cpp:379-395, `g_depth_extract_hlsl`: scalar .x extraction, optional Y flip, depth scale (initial value 1 at :282). `dispatch_gpu_shared` (:1780), :1801-1863, creates the readable format-specific source SRV, binds and dispatches extraction. | O/genshin/GenshinLegacyCapture.cpp:302-316, `PrepareShader`, reads scalar depth and decodes motion. `CompleteNativeReplacement`, :1253-1264, copies selected color, binds actual selected depth/motion SRVs, dispatches preparation, then supplies private depth/motion/output resources. Scalar copying does not linearize depth or recover a camera. |
| Motion + jitter | B/Ffx12Backend.cpp:403-428, signed-square motion decoder, with separate raw variant. B/Dx11FsrBridge.cpp:11148-11149 takes token jitter; :11296-11320 applies configured modes; :11360-11377 applies motion sign/render scale and requested near/far/FOV. Convention settings wired at :4054-4057. | O/genshin/GenshinLegacyCapture.cpp:313-315 decodes motion; :1191-1194 calls `NativeMrt::ApplyProfile`. O/genshin/GenshinNativeMrtContract.h:252-271 applies token jitter/delay/mode/clamp, requested camera settings, motion/depth controls and validation. Requested settings are not authenticated game-camera facts. |
| Submit / Evaluate | B/Dx11FsrBridge.cpp:11099-11104 fills selected resources; :11687 calls `ffx12::dispatch`. B/Ffx12Backend.cpp:2703-2728 validates inputs; :2819 calls `dispatch_gpu_shared`; :1989-2021 builds FFX parameters and calls **g_runtime.dispatch**. :2026-2041 restores COMMON, executes and signals. | O/genshin/GenshinLegacyCapture.cpp:870-882, storage `Submit`, -> `Dx11Owner::Submit`; :1269-1282 attempts optional guide receipt and submits SR. O/inputs/GenshinFsrDx11.cpp:258-274 -> `DispatchGenshinFsr2Dx11`; O/inputs/FSR2_Dx11.cpp:674-688 -> `NVSDK_NGX_D3D11_EvaluateFeature`; O/inputs/NVNGX_DLSS_Dx11.cpp:904-917 authenticates DX12 transport and calls selected feature Evaluate. O/upscalers/IFeature_Dx11wDx12.cpp:730-733,798-802,885-889 binds imports, calls real DX12 feature Evaluate, executes and records submission. Original FFX dispatch and Opt Evaluate are corresponding stages, not identical APIs or proof that a particular backend executed in a game. |
| Game RTV copyback | B/Ffx12Backend.cpp:2047-2063 waits/copies to `input.output_target` synchronously or retains a pending target; `finish_gpu_shared` (:2079), :2102-2105 completes pending copy. `finish_pending`, :2889-2892, calls it; B/Dx11FsrBridge.cpp:7349 calls `ffx12::finish_pending` in `hooked_present` (:7248). | O/genshin/GenshinDx12Transport.cpp:208-224, `Lease::Copyback`, waits and copies imported output to private D3D11 output; O/upscalers/IFeature_Dx11wDx12.cpp:932-939 invokes it. O/genshin/GenshinLegacyCapture.cpp:1298-1300 claims the output; :1329-1358 copies private SR output to the selected actual game RTV subresource and records canonical identity. CPU-ordered copyback is not final visible acceptance. |
| Present NR | Original Present completion above finishes SR output handoff; it is not Opt's FinishedPicture NR contract. | O/native/NativeDriverDx11.cpp:109-141, `NativeMotionDx11::OnPresent`, gets `GenshinLegacy::TakePresentNr`, opens `GenshinPresentNr::Scope`, and runs the native producer. O/native/NativeDriver.cpp:248-271, `NativeDriver::RunFrame`, authenticates consumer and calls `DlssNr::ApplyNativeInput`. O/genshin/GenshinLegacyCapture.cpp:1657-1686, `CapturePipeline::TakePresentNr` (definition :1568), gets a fresh picture and optional `PreparedDepth`; O/genshin/GenshinProducerTransport.h:327-395, `ProducerFrame::PreparedDepth`, checks owner/resource/fences, maps scene depth into the picture and retains a lease. Ordinary NR and optional depth/FG authority are distinct. |
| FG | Original SR extraction/dispatch/copyback is not a frame-generation admission transaction. | O/genshin/GenshinPresentNr.h:270-281, `Ticket::DepthMatches`, requires exact owner/frame/picture/target mapping. O/inputs/FG/GenshinBridge_Present_Dx12.h:184 and :443-463 requires matched depth, camera, producer, motion and consumer; :908-933, `PresentScope::TryPresent`/`Dispatch`, sets constants, tags depth/velocity/present color and calls provider Present. O/hooks/FG_Hooks.cpp:1425-1450 calls `ApplyToFinishedPicture` then `PresentScope::Dispatch`. Actual provider invocation, generated frames and visual/performance acceptance remain separate evidence levels. |

## Key original recipe snippets

Excerpt boundaries below are explicit; omitted lines are not implied to be adjacent. Comments or historical claims in the original source are not new runtime evidence.

B/Il2CppCallSiteHook.cpp:93-101, inside `on_render_enter`:

~~~cpp
cp.instance = reinterpret_cast<std::uint64_t>(this_ptr);
cp.context = reinterpret_cast<std::uint64_t>(context_ptr);
std::memcpy(&cp.render_w, p + 0x58, 4);
std::memcpy(&cp.render_h, p + 0x5C, 4);
std::memcpy(&cp.display_w, p + 0x68, 4);
std::memcpy(&cp.display_h, p + 0x6C, 4);
std::memcpy(&cp.frame_index, p + 0x50, 4);
std::memcpy(&cp.jitter_x, p + 0x24, 4);
std::memcpy(&cp.jitter_y, p + 0x28, 4);
~~~

B/Dx11FsrBridge.cpp:11099-11104, selected input/output handoff in `try_fsr2_translation_draw`:

~~~cpp
sdk_in.color = color_tex;
sdk_in.depth = depth_tex;
sdk_in.motion = motion_tex;
sdk_in.transparency = transparency_tex;
sdk_in.output_target = output_tex;
sdk_in.reset = g_config.ffx12_force_reset || st->reset_next;
~~~

B/Ffx12Backend.cpp:391-394, depth extraction shader:

~~~hlsl
uint width, height;
in_depth.GetDimensions(width, height);
uint sy = (g_flip_y > 0.5) ? (height - 1 - id.y) : id.y;
out_depth[id.xy] = in_depth.Load(int3(id.x, sy, 0)).x * g_depth_scale;
~~~

B/Ffx12Backend.cpp:427-428, decoded branch of motion shader (raw branch and optional deadzone are separate):

~~~hlsl
float2 d = c.rg - 0.498039;
float2 mv = -sign(d) * (4.0 * d * d) * g_flip;
~~~

B/Ffx12Backend.cpp:2008-2021, original FFX dispatch parameters/call:

~~~cpp
dispatch.jitterOffset = {input.jitter_x, input.jitter_y};
dispatch.motionVectorScale = {input.motion_scale_x, input.motion_scale_y};
dispatch.renderSize = {input.render_w, input.render_h};
dispatch.enableSharpening = input.enable_sharpening;
dispatch.sharpness = input.sharpness;
dispatch.frameTimeDelta = input.frame_time_delta_ms;
dispatch.preExposure = input.pre_exposure;
dispatch.reset = reset;
dispatch.cameraNear = input.camera_near;
dispatch.cameraFar = input.camera_far;
dispatch.cameraFovAngleVertical = input.camera_fov_vertical;
dispatch.viewSpaceToMetersFactor = 1.0f;
const ffxReturnCode_t rc = g_runtime.dispatch(
    &sc.ctx, reinterpret_cast<const ffxDispatchDescHeader *>(&dispatch));
~~~

B/Ffx12Backend.cpp:2047-2054, synchronous original game-target copyback:

~~~cpp
// ---- ③ 输出交接：同步模式立即等待+拷贝；异步模式记 pending，Present 前 finish ----
if (!g_async_upscale)
{
    if (FAILED(g_game_ctx4->Wait(g_shared_fence11.Get(), v2)))
        return false;
    if (input.output_target)
        game_context->CopyResource(input.output_target, g_tex_output.d11.Get());
    game_context->Flush();
~~~

## Original, cross-API and additional sufficient-evidence gates

| Class | Concrete evidence and interpretation |
|---|---|
| Original recipe | Render instance/generation, selected accumulate resources, dimensions/formats, temporal inputs and prepared depth/motion. These are visible in B's selection, shader and dispatch paths above. B's selection is layout-based; O's exact accumulate schema additionally pins t0/t2/t3 and RTV1 and validates native return. Neither scalar extraction nor a requested depth-inverted flag proves the actual camera. |
| Cross-API requirements in kind | Shared resource ownership, state transitions, queue ordering, completion, restoration and copyback. B/Ffx12Backend.cpp:1984-1988,2026-2053 already uses barriers, execution, shared fences and waits. O/genshin/GenshinDx12Transport.cpp:186-224,284-306 checks actual submission/completion/retained objects. This establishes the need for safe handoff, not necessity of every exact Opt predicate. |
| Additional Present consumer needs | Current final-picture correspondence, scene/output mapping and validity coverage, same owner/history and completed resource lease: O/genshin/GenshinProducerTransport.h:327-395 and GenshinPresentNr.h:270-281. Equal-sized textures alone do not prove scene depth describes postprocessing/HUD pixels. |
| Additional Opt sufficient-evidence policy; necessity UNKNOWN | O/genshin/GenshinTypedGuides.cpp:200-233, `Receipt::DepthFactsForPresent`, currently requires selected ReturnedRaster, matched camera/frame, returned clear and exhaustive semantic writers before publishing ActualRasterDepth/ActualDepthMapping. O/genshin/GenshinDepthSemantic.h:936-1056, `Resolve`, iterates every writer and checks matrix/clear/phase/submission. O/genshin/GenshinDepthMapping.h:471-506 admits those fact sources. These are not gates found in B's scalar extraction/FFX dispatch recipe. No bypass follows from that distinction. |
| Additional conservative tracking policy | At this inspection O/resource_tracking/GenericDepth_Dx11.cpp:1018-1042 first tries `PendingIndirect` and falls back to `BeforeUntrackedRaster`; O/genshin/GenshinReturnedRaster.h:523-547 revokes affected RTV/UAV/writable-DSV tracks. These files have changed concurrently since phase42; its former :499-522 range no longer identifies the same function. This reference reports source state only, not approval or verification of the ongoing root patch. |
| Additional FG needs | Same-picture depth, actual camera/current-predecessor and motion correspondence, final-color/outcome join, provider resource tagging and Present. O/genshin/GenshinPresentNr.h:331-335, `SelectFgPicture`, rejects before/deferred routes and keeps `!finished` fail-closed when NR is enabled. NR-disabled original-picture FG is a distinct branch, not after-SR NR acceptance. |

SR independence is explicit in O/genshin/GenshinLegacyCapture.cpp:870-882 (`semanticsProven=false`, requested-control provenance with SR allowed), :1269-1282 (optional typed guides, SR still submitted), and :1361-1365 (optional camera failure cannot prevent successful SR/native NR). Successful SR does not upgrade unknown Present depth, camera or FG semantics. Preserve existing resource and consumer gates while root assesses evidence; this document authorizes no gate removal.

## 历史 R829 lifetime simplification (当时 candidate)

The original Bridge scalar extraction/selection recipe above is unchanged. Current Opt differences are bounded retained-buffer/payload accounting, exact method6 callback-end TLS release with executable gating, refusal to seed unqualified optional motion history, and release of completed sole-owned mapping-node raster roots. Deferred replay payloads/private constant buffers and mapping-held buffer roots now carry shared budget tokens through final ownership. Scratch ClearState is deliberately deferred because its shared hook can revoke writer observations. The original selected-depth preparation and SR submission remain independent of optional mapped-depth/camera qualification; this does not grant unknown semantics to Present NR/FG. No new raw-depth consumer or alternate depth mode is claimed.

Current source/build/deployment/runtime and acceptance are recorded only in [R829](../audit/r829-retention-budget-20261024/REPORT.md). The function map remains a recipe reference, not a running-game result; untouched line anchors retain the original inspection context. Before edits, resolve actual functions and compare current bytes. The new GenshinDepthRetention.h and all eight candidate changes are pinned by that round's source-manifest.json.

## R830 actual VS dependencies

Opt deferred raster now ignores VS SRVs that the retained actual VS bytecode does not read. Reflection still refuses all non-CB dependencies. This removes an Opt-only extra restriction and leaves original Bridge scalar selected-depth/SR source excerpts unchanged. UAV IA, GS/HS/DS and all numerical/query/native/queue gates remain. This is not world depth or FG acceptance. See [R830](../audit/r830-live-shader-dependencies-20261024/REPORT.md).

## R837 stage-transition CB28 / Render-token phase (2026-10-25)

User directed rechecking stage-dependent reads against the actual original Bridge. The original dirty source tree was read only; its three pinned hashes remain unchanged.

| Concern | Actual original Bridge | Current Opt candidate and evidence boundary |
|---|---|---|
| Stage/instance selection | B/Dx11FsrBridge.cpp:10562-10566 explicitly isolates parameters and generation per instance so UI/secondary-instance Render cannot overwrite the main accumulate state. :10780-10814 retains per-instance consumption and duplicate handling. | Existing Opt owner/instance/generation/epoch gates remain unchanged. This candidate does not add a global jitter cache or share a phase across instances. |
| Current draw jitter authority | B/Dx11FsrBridge.cpp:11148-11149 assigns `sdk_in.jitter_x/y = call_params.jitter_x/y`. Comments at :11150-11157 say the current draw CB0 can lag one token and prioritize the paired current Render token. CB0 is read only for the invalid-token fallback at :11158-11168 and later fallback code. | O/genshin/GenshinTypedGuides.cpp:223-226 now computes `nativeToSrX/Y` from the current authenticated Render token, not a lagging CB28. Same-phase numerical results are unchanged; the previous-token-only R835 attempt remains rejected and is not restored. |
| Accepted phase relation | The original does not require CB28 to equal the current Render token before using that token for SR. Its source comments explicitly allow CB28 to repeat the preceding token. | O/genshin/GenshinGameMotionReference.h adds an exact `PreviousFramePhase` predicate. O/genshin/GenshinFgGameMotion.h:94 accepts same-frame OR previous-token phase only with the existing adjacent source frame, increasing generation, same owner/instance/context/buffer/device/config/shader and finite correction gates. First, duplicate, gap, wrong-buffer and arbitrary mismatch remain rejected. |

R836 PID8236/session20261025-113358_YuanShen_8236 is the negative runtime anchor: user reported severe stutter/rollback and FG not working after entering the world; late native-visible receipts repeat `NoAuthenticatedCameraOrGuides`, `childCalled=false`. `DlssNr.FinishedPicture=true` is preserved and the final Present-NR sample reached its consumer. These observations do not prove visual NR success or establish phase as the sole stutter cause.

The R837 candidate also makes refusal logging report reason/mask changes and bounded periodic samples on already-refused paths; success gates are unchanged. Actual-header phase checks pass, but complete build, deployment, loaded-PID world behavior and user visual acceptance remain separate. See [R837](../audit/r837-genshin-pause-continuity-20261025/RECORD.md). No original Bridge file was modified and no after-SR NR substitution is allowed.

## 历史 SHA256 of inspected files（仅保旧快照）

Paths use the exact historical B/O roots stated above. Values below are retained source snapshots, not current source/build/deployment/runtime receipts. Later changes resolve actual symbols and bytes; no rehash, manifest, verifier or build/deployment gate is requested by this historical table.

| File | SHA256 |
|---|---|
| B/Il2CppCallSiteHook.cpp | 3A3560C6341CA704F0470D60A50B4A4FB82FD5812D2E2F2611CC4A8F38187C9E |
| B/Dx11FsrBridge.cpp | 0716B1B7AE9850B212B811B50B269C16E54F58819548A2FA631B614F50D43AE2 |
| B/Ffx12Backend.cpp | 39EA73385D670D59237446DA119E10885048E21A0C39BB27098434E36A131018 |
| O/genshin/legacy/Il2CppCallSiteHook.cpp | 7DDC8534005D86904A0C5C791C97C0FD5C925D3EB49465D54366276D7AEC8634 |
| O/genshin/GenshinLegacyCapture.cpp | E354C852ACAFA69B55469EA5A1D17D6C151479DC5199607AAD0472D53845EFD8 |
| O/genshin/GenshinNativeMrtContract.h | 0C0CD71CD0CFE4D708F25E38C3D6E9FB0EAA5DD4679C267F2FB6DF7CB2D8950D |
| O/genshin/GenshinProducerTransport.h | 7F03E3DC878A6F524AF17BC85EAA9133F3103FFCE9A2783A385AE419F7E40A92 |
| O/genshin/GenshinTypedGuides.cpp | B58845B4D0A78047D23F669822FB73250FCAE906DF02F7FB25929B6EB480ADBF |
| O/genshin/GenshinDepthMapping.h | 57E2E3D3A4DFAFEE1679293AD842A8B0D1E53E7A81898BED34067D96EBDC2D39 |
| O/genshin/GenshinPresentNr.h | E5F13DFD61E686535183D32730A2D21877CE13CCDB853DCAF19AEE81BF50CBF1 |
| O/inputs/FG/GenshinBridge_Present_Dx12.h | 25811B10D43DEE3C86FB9687C93EB755137B662AD766FD79E74C400CB9D0BA0A |
| O/genshin/GenshinDx12Transport.cpp | 15DF98BDDC283AEF0FE8DD5ABB0F15152542FE99F2BB665E6207FAACC7D67A18 |
| O/upscalers/IFeature_Dx11wDx12.cpp | AF5586BC6B23A773321015F6FB78EACA982A7FF70E3AE921A3134F60E2E92143 |
| O/hooks/FG_Hooks.cpp | 0F68F5DD734D537E22BDD11CD92790AE3AE6645881FC525B705951401DD34B97 |
| O/native/NativeDriverDx11.cpp | 31470993A89A922E612878ACFC1CBB5335D88E4F6D0478CD7E046A4B0FF951E7 |
| O/resource_tracking/GenericDepth_Dx11.cpp | 18731A453C50415C13628A4C6CFA876122740F898910BDD3F5188B61EB246235 |
| O/genshin/GenshinReturnedRaster.h | 64B34945013ECA5760389998437196A032439A84EF962422564D566BDEB51B68 |

Only this reference is written by the sidecar. No agent spawned; no build, deployment, configuration or game operation performed.
