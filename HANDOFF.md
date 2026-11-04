# F6 转交（2026-11-04，用户要求暂停）

**当前功能推进已暂停，长期 goal 为 paused。先交接，不继续编码、构建、部署或游戏测试；用户明确恢复后再继续。**

## 1. 接手位置与正式版本

- 项目根：`D:/CODE/DLSS5_NEXT`，主分支 `master`，产品 HEAD **`ed7e598`**。
- 当前正式部署与新根产物一致：**`0.1.29-final (ed7e598)` / 34,816,000 bytes / mtime UTC `2026-11-04T13:35:24.0676033Z`**。
- 该版本经 `pwsh -NoProfile -File ./build.ps1 -Jobs 12` 真实编译/链接，**exit 0，120 warnings，0 errors，70 秒**。日志 `build/msbuild.log` 是本次，后续构建会覆盖。
- 正式 DLL：`D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/OptiScaler/official/F6/OptiScaler.dll`。
- 已用 `tools/runtime/Deploy-F6.ps1` **流写原文件对象**，部署 exit 0，保四个 hardlink（正式位 + 三游戏 cache runtime）。未改 profiles。
- 本次部署前回退件：`build/rollback/OptiScaler-20261104T134036889-e6ff97d0d7494f069e7a9834d759f918.dll`（上一部署 `7727a19`）。更早正式回退件：`build/rollback/OptiScaler-20261104T132418679-759008c2bcc848f8b13e8f563f1e1e28.dll`。
- 暂停前最后实际检查：**三游戏进程数 0**，无运行中的构建、部署或游戏 CLI job。当前 DSH 已提权（实测 True）。

## 2. 本轮已集成的代码

| 主树提交 | 内容 | 验证界限 |
|---|---|---|
| `47e5a70` | NR 消费端归一化授权、相邻历史、发布者 fence COM 对象、星铁原私有 owner 在真实完成后 Reset、MAX 防护 | 构建通过；星铁本局历史融合有持续采样；最终画质与净性能 UNKNOWN |
| `2943c61` | GIMI key-cycle 有限参数域、每个真实 reader 的 immutable 参数页 | 构建通过；实际完整 MOD 像素未测 |
| `7727a19` | BeforeSR 完整选技录制见证保持到 copyback，真实收据在 selected SR Evaluate 前复核 | 构建通过；选技和完整效果最终像素未测 |
| `66e6683` + `ed7e598` | ZZZ NativeSource 与 Reflex/SDK marker clock 分离；原 source receipt→SR成功→实际 Execute/Signal→常量快照→一次 SDK Present 消费 | 构建部署通过；ZZZ 已启动本版本，但未取得实际 selected-source/NR/FG 主链验收 |

NR 的首份 DS 复核混淆了归一化前 producer `frame.NativeGuideContractVerified` 与 consumer `reuseFrame.nativeGuideContractVerified`；后者实际在 Dx12:5437 由 `nativePhase.verified` 赋值。独立 Sol 已完整核证、允许集成，不应为迎合该误报加回旧 gate。coordinateControls、单步 ordinal、完整 contract、queue/device/epoch 仍保留。

ZZZ DS 复核提出的 SetResource 时序、自锁与拒源不恢复疑点经 Lead 对当前源码核查：两 guide 的 `publish→SetResource` 成功后才 BuildGuideConstants；continuity 是 recursive_mutex；完整成功 pair 会复位 `_depthSourceRejected`。不能把未经核实的复核断言直接当运行故障。实际运行是否仍有断链，必须依本局/下一局 source 接口日志判断。

## 3. 星铁实际测试（已结束）

- 测试版本 **`7727a19`**（34,803,712 B；不是后来的 ZZZ 版本）。载入日志明确该 commit。
- `hkrpg_bilibili`，PID **29660**，creation FILETIME **134382723306110116**；真实 image `D:/APPS/Star Rail/games/Star Rail Game/StarRail.exe`。
- Session：安装根 `log/sessions/20261104-212540_StarRail_29660`。
- 8 条 NR cadence 样本均有非零最终模型 depth/MV；depth/MV **1280×800**，model work **2096×1312**，colour **2560×1600**。
- 最新 **modelCalls=3000，modelFailures=0**；第 3 条起及每600 dispatch采样持续 `feedbackHistoryUsable=true / feedbackFusionRecorded=true`。
- 最新 `sceneSource=3002 / sourceOrdinal=3001`；已发布原 fence 对象 **0x1780e20e2d0**，target/completed **2981/2981**；采样 `slotBusy=0`。
- 全日志 FG Dispatch Ok **3152**、Deactivate **1**、FG depth refusal **3**（包括启动阶段，不隐去）。这不是生成帧像素证明。
- 实际完成 GPU 记录 total/model_interval 各 **4** 组：中位 **28.63105 / 21.9612 ms**，max **173.2526 / 162.4515 ms**。各记录所属 queue/fence completed≥target。两段 inclusive，**不可相加或倒数冒整局性能**。
- **intervalUsed=1、full-model、reuseAccepted=0**：证明连续历史反馈融合有运行进展，不证明跳模型推理复用收益。
- 用精确 `stopgame --biz hkrpg_bilibili --expected-pid 29660 --expected-creation-filetime 134382723306110116` 收尾，CLI exit0，之后 NOT_RUNNING。正常关闭还是强制回退 **UNKNOWN**。
- 摘要/精确采样：`build/starrail-29660-summary.json`、`build/starrail-29660-excerpt.log`。公共正式日志已被 ZZZ 下一局覆盖。
- **本局没有截图或同条件前后对比：world、细节、拖影/曝光画质和净性能 UNKNOWN**。保持用户的 adamfg/F5-DS 独立 FG 方案，不改其调度或加复杂同步。

## 4. ZZZ 实际测试（进程已退出，原因 UNKNOWN）

- `nap_cn`，PID **61120**，creation FILETIME **134382732749316240**；image `D:/APPS/miHoYo Launcher/games/ZenlessZoneZero Game/ZenlessZoneZero.exe`。
- Session：安装根 `log/sessions/20261104-214125_ZenlessZoneZero_61120`；启动 CLI exit0。
- 正式 `OptiScaler.log` 开头明确载入 **`ed7e598`**。21:43:06 尾部仍有 Streamline Evaluate/FGPresent 返回0，之后日志中途终止。
- **未取得本次实际 NR cadence、native FG producer/source SDK Present 的有效验收样本**，不能称 ZZZ 时钟修复已跑通。
- Lead **没有向该 PID 发 stopgame**。尝试只读、精确前台客户区截图时，GetProcessById 报 NOT_RUNNING，未产生截图。最后再次检查三游戏数0。
- session 没有退出回执；检查正式 F6 下没有属于本局的新 crash dump。退出原因仍 **UNKNOWN**，不推断自然退出、崩溃或外部强杀。
- 公共日志现仍是这局，路径：安装根 `OptiScaler/official/F6/OptiScaler.log`。需要取证时先保护此局关键片段，再启动下一局避免覆盖。

## 5. 私有候选与未完成原神（不可当交件）

Worktree 根：`D:/CODE/DLSS5_NEXT-workers/`。

| 分支 / 子目录 | HEAD | 状态 |
|---|---|---|
| f6/nr-continuity / nr-continuity | `a22dc23` | 源码已按 `47e5a70` 集成；独立审阅者已结束 |
| f6/zzz-source-clock / zzz-source-clock | `d0283bb` | 与前置 `1a0a201` 已合入主树；编码者已回完整交件、工作树干净 |
| f6/mod-filter / mod-filter | `b6d9b7e` | 与前置 `bb2e0ea` 已合入；编码者结束 |
| f6/genshin-output / genshin-output | **`2384a45`（WIP）** | 继承 `0095a40`；失败子会话未完成的六文件已由 Lead 保存为 WIP，215+/31−；未复核/构建/部署/运行，主故障未验收 |

原神 WIP 六文件：`framegen/FinalOutputEndpoint.cpp/.h`、`framegen/dlssg/DLSSG_Dx12.cpp`、`inputs/FG/GenshinFgTicket.h`、`with_dx12/GenshinVisibleChild.h`、`with_dx12/dx11_with_dx12_sc.cpp`。已存 **`2384a45`**，私有工作树干净；另有 `build/genshin-paused-uncommitted.patch` 快照。**不要整文件覆盖主树；该 WIP 尚未复核/构建/部署/运行，绝不可当修复验收。**

原神核心设计进度：SDK worker 不在 PresentPermit TLS；SDK S_OK 无同步 raw 时 wrapper立即Hide，owner要求每输出同parentContent。正在拆 input receipt 与真实 output stream 身份、用 actual FinalOutput provider/physical stream generation 与 raw queue/tail 完成授权，不能借最新token/FIFO/相等数值猜生成帧源，也不能用 input fence/CPU返回/窗口ACK冒 output 完成。普通 Deactivate 会递增 inputOwnerEpoch，因此不能把它当 immutable output stream epoch。FinalOutput async 没有真实 child ReShade runtime/selection 时仍需拒绝过滤，不能冒父copy已适用异步生成帧。

**子智能体收口**：原神临时子会话 `6f6f6c81-bbd3-4085-8f60-325273bb53e4` 起初没有最终交件，停止消息及 interrupt 返回 `active teammate not found`，运行时随后报 failed。**交接提交后又收到其最终停止回执：已立即停止功能编辑，确认 HEAD=2384a45、工作树干净、无后台 jobs；没有再提交产品修改。** 最后 list_agents 中持久队员全部 inactive，job_list 全部 completed/killed，无运行中的编排、构建或CLI；三游戏进程数0。接手恢复时先核四工作区 log/status，再派唯一写者。

### 原神最终回执补充（只记录，暂停后未修）

恢复后优先复核三项：
1. `BuffersRetired` 要求 gate.stream==actual stream；若成功输入已开启 scope，但尚无任何 raw 绑定（gate.stream==0），provider 退休可能未清旧 streamOwner，重建后会拒绝新 provider。应在真实退休 seam 撤销同 outputOwner 的未绑定 scope，不能靠任意超时清空。
2. 同步 authenticated 分支仍用旧同线程 source 契约；新增 outputOwner 精确相等只在 streamAuthorized 分支，应核同步分支是否也必须比较 `sdkSource.StreamOwner().outputOwner == captured outputOwner`。
3. 编辑工具可能移除了 `FinalOutputEndpoint.h/.cpp`、`DLSSG_Dx12.cpp` 三个 UTF-8 BOM；恢复后核真实 diff，保原编码并清理无关格式差异。

编码者确认：未改 FG_Hooks/Bridge、ZZZ source clock 或 NR；验证仅静态调用链/本地 SDK headers/签名与 diff 检查，未构建或运行。`sl_dlss_g.h:170–177` 的 GetState present-thread input fence 仅管 tagged input 生命周期；`nvapi.h:18391–18423` 的异步 marker 字段本身没有 source token 到 raw bracket 的映射保证，候选没有据此猜源。

最低运行取证仍是：输入 seal 与实际 SDK S_OK；实际 creation outputOwner/chain/queue/stream/generation/sequence；raw HRESULT 与所属 physical fence target/completed；owner `association=sdk_output_stream` 的真实 tail 后 reveal；FinalOutput ON 时真实 raw chain 的物理 runtime/selection 与 EffectsExecuted。**构建、API、GPU 完成、像素和净性能仍全部 UNKNOWN。**

## 6. 任务与工具边界、恢复顺序

- 四路旧编排 `workflow-f4108f5d3405419f` 已 completed，但三项结构化结果曾为 null；真实磁盘提交与各编码者回执比编排状态权威。多次临时子会话报响应错误，别认为 failed 就绝无残留写入。
- SSE 调查 `workflow-0f9dbf5bf9344ab8` 已 killed；已只读定位 OpenAI SDK `core/streaming` JSON.parse异常，但实际坏事件根因未确证、未修运行安装包。用户要求 F6 优先，不重启此旁线。
- Runtime helper在 `tools/runtime/`，默认 launcher 指安装根 HoYoShadeHub.exe。现有 activate/click/capture CLI源码只接受原神 hk4e_cn；参数支持三个Biz不等于后三游戏自动入场已可用。
- 后续用户恢复后：先核原神私有会话静止与候选；审同文件冲突并完成真实异步输出主故障→统一 build.ps1→流写原DLL对象部署→分别精确PID/creation运行三游戏。
- 星铁后续必须分别核持续深度/反馈、world最终像素与净帧时间，不能因3152次Dispatch就当FG视觉有效。ZZZ先查本局未产生selected-source验收的实际原因，不靠去gate/扩池补症状。
- GIMI完整MOD/BeforeSR完整effects仍须真实选技/纹理/所选SR前后像素核验；没有运行证据的轴均UNKNOWN。
- 保旧根 `D:/DATA/Documents/ChatGPT/DLSS F5 NEXT`、原配置和旧回退资料；不整体清理/覆盖。普通审计DS、核心设计Sol；执行/复核/Lead集成构建分层，默认不并发构建。
