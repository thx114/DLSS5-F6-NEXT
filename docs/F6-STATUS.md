# F6 开发、部署与测试状态（已恢复）

**2026-11-04 用户已恢复持续开发、分派编码、统一构建部署与三游戏验证；长期 goal 为 active。** 暂停时的完整快照见根 [HANDOFF.md](../HANDOFF.md)。本次恢复轮次 1，每完成 10 轮依据实际提交与运行效果重新评估实现方案；服务错误和重复等待不计作已完成开发轮次。

接手实测：主树保存点 `726430b`、产品及正式 DLL `ed7e598` / 34,816,000 B，二者 SHA256 相同，正式位与三游戏 cache 的四个硬链接完整，接手时三游戏与编译进程数均为 0。当前 Lead 执行会话实测**未提权**，交接时 DSH 已提权的结论不适用于当前会话。恢复后已独立编译原神保存点并运行一次现有正式 ZZZ；尚未部署新候选。

**工作区隔离**：原根已由并发任务切换到 `f7/clean-rewrite`，保留其全部在途文件。F6 的集成、构建部署和当前说明统一位于 `D:/CODE/DLSS5_NEXT-workers/f6-integration`（`codex/f6-integration`）；不得对原根 checkout、reset 或清理。

当前三个互不冲突任务：Sol 在 `D:/CODE/DLSS5_NEXT-workers/genshin-output` 完成原神实际 SDK output stream/queue-tail/owner reveal 生命周期；Sol 在 `D:/CODE/DLSS5_NEXT-workers/nr-detail` 核查并修复三游戏 NR 深度、相邻历史与细节保留的确证缺陷；DeepSeek 只读定位 ZZZ PID61120 日志断点。星铁现有 adamfg/F5-DS 式 FG 保持独立，GIMI/BeforeSR 次优先。Lead 是唯一集成、构建与游戏操作 owner。

两名 Sol 首次因 `429 Too Many Requests` 终止；Lead 已核工作树并向原会话发送继续指令，未增加重复写者。原神已有合并提交 `b904cae`（`2384a45` + 主树 `726430b`），NR 独立树位于 `726430b`；均未有本次功能交件。按 `efficient-waiting` 技能接收完成通知，观察超时不授权重启。

Lead 在独立 detached 工作树 `D:/CODE/DLSS5_NEXT-workers/genshin-compile` 对保存点 `b904cae` 完成真实全量编译/链接：`build.ps1 -Jobs 8`，exit 0，335 warnings、0 errors，194 秒；产物 `0.1.29-final (b904cae)` / 34,836,992 B / mtime UTC `2026-11-04T14:21:02.5380537Z`。该结果仅证明原 WIP 编译闭合；原神写者还在修生命周期缺口，未集成或部署此产物。

ZZZ PID61120 日志已保护到 `build/zzz-61120-OptiScaler.log`。已核本项目 selected-source/constants/producer 收据为零；游戏 Streamline 初始化有 NR 签名加载告警，但本项目实际模型经 `shaders/dlssnr/DlssNr_Dx12.cpp` 的独立 forwarder 到达，不能由该告警直接推断项目 NR 加载失败。正在区分游戏 Streamline 与 selected SR/NGX 接入链路。

ZZZ 复测 PID **33548**：launch-only CLI exit 0，经启动器自身 UAC 中继注入，日志确认载入 **`ed7e598`**。用户实测明确反馈“无 F6 的 DLSS 输入，无 FG/NR”；本局也没有项目 NGX Create/Evaluate、NR cadence 或 native FG producer 收据，不能以游戏 Streamline Evaluate/Present 冒验收。启动早期日志确认 `sl.dlss.dll`、`nvngx.dll` 已在内存；现有预载 NGX 接入仅 detour GetFeatureRequirements，未接管 Create/Evaluate。Sol 正在独立 `zzz-input-route` 工作树修复输入接入及原生调用/句柄归属。PID33548 已退出，Lead 未发送 stop，退出原因 UNKNOWN；全日志已保护到本工作树 `build/zzz-33548-OptiScaler.log`。

独立审阅确认原神游戏 Reflex marker 分支会把缺失的本地 marker 编码成 0，导致成功 Dispatch 的 SDK 输入收据被拒；已扩原写者范围到 `FG_Hooks.cpp` / `GenshinBridge_Present_Dx12.h` 修复，未集成。NR 写者已给出完整 pass 链历史候选，Lead 用 MSVC 14.44 / C++20 / O2 独立运行 `dlssnr/tests/nr_model_chain_history.cpp`，**exit 0 / 74 checks PASS**；该结果只覆盖 CPU cadence 与完整链判定，不证明 GPU、画面或性能。NR 候选仍在独立审阅，等待最终提交。

**NR 本次交件及部署完成**：原提交 `1247a68`，独立 Sol 审阅通过，集成为 `0613134`。唯一 Lead 在 `genshin-compile` detached `0613134` 真实编译/链接，**exit 0 / 131 warnings / 0 errors / 87 秒**。DLL `0.1.29-final (0613134)` / **34,817,024 B** / mtime UTC `2026-11-04T14:56:07.4441618Z` / SHA256 `EF7BE632CB3D05E37AC598293CB01B3767048F19851AE08CD6459D7BE8E793A4`。经本树 `Deploy-F6.ps1` 流写原对象，exit 0，四个 hardlink 与 hash 复核，profiles 未变。回退件 `build/rollback/OptiScaler-20261104T145758333-364e7bec85d4497e922f620a32323959.dll`。此版本不含尚在编码的 ZZZ/原神接入修复。

**星铁复测 PID27252**：launch-only CLI exit 0；CIM creation FILETIME `134382779130093750`，日志确认 `0613134`。Computer Use 已观察世界角色画面，未操作登录或改配置。8 组 NR cadence，最新 `modelCalls=3000 / modelFailures=0`，实际模型 depth/MV 非零、rect 1280×800、work 2096×1312，`modelPassesCompleted=1 / planned=1 / historyAnswerComplete=true`；后 6 组 `feedbackHistoryUsable=true / feedbackFusionRecorded=true`。最新同发布者 fence `0x1bea62344e0` target/completed **2999/2997**，`historyReadiness=queue-ordered-gpu-pending`，不能冒称该历史已完成/退休。FG 实际 tagged depth/MV 1280×800、tags accepted，Dispatch Ok **3026**，启动拒绝 **3**；保持 F5-DS scheduling。`intervalUsed=1 / reuseAccepted=0`，跳模型复用未验。

本局 completed GPU total/model_interval 各 3 组，各自原 fence completed≥target；中位 **25.3727 / 19.6106 ms**，max **29.2567 / 21.1251 ms**，inclusive 不可相加，也不是整局净性能。进程已退出，Lead 未 stop；日志有 `F5 FG teardown complete: owned queue fence 1, slFreeResources=eOk`，退出原因仍 UNKNOWN。只观察了静态世界，动态拖影/曝光/遮挡细节与净性能仍 UNKNOWN。完整日志 `build/starrail-27252-OptiScaler.log`（SHA256 `231CE581E9F84796E55B2EB81D8161691DB993BB68EF0929313CC234A64D1A19`），字段采样和 GPU 汇总 `build/starrail-27252-summary.json`。

以下保留暂停前的阶段记录。当前执行状态以上文为准；历史提交与测试的最终边界以 [HANDOFF.md](../HANDOFF.md) 为准。

当前优先级：**F6 开发 → 部署 → 测试**。SSE 解析错误调查按用户指示暂停；诊断副本在 `build/dsh-diagnostics/`，没有修改运行中的 DSH 安装包。

## 当前轮次与目标

长期目标轮次 1/256，每第 10 轮重新评估实际编码产出与运行效果。主优先级为原神、星铁、ZZZ 的真实 NR/FG 深度、相邻历史复用、细节与连续性。星铁保留 adamfg/F5-DS 独立 FG，不为统一框架增加同步或池重建。GIMI/BeforeSR 次优先。

## 已集成与正在完成

| 功能 | 主树提交 / 私有保存点 | 实际状态 |
|---|---|---|
| NR 相邻历史与实际 fence 退休 | `47e5a70`（私有原件 `a22dc23`） | 已集成、构建、部署并运行星铁；DS 的 producer/consumer 字段混淆经独立 Sol 核证为假阳性。真实 consumer phase 授权、完整 coordinate/contract、单步 ordinal 不放宽 |
| GIMI immutable 参数页 | `2943c61` | 已集成、构建、部署；实际 MOD 像素和性能 UNKNOWN |
| BeforeSR 完整录制/copyback→selected SR 收据 | `7727a19` | 已集成、构建、部署；真实选技/像素尚未运行核验。DS 提出的中途 frame 改变应拒绝属于身份守卫，Lead 已核同次函数内 endpoint→参数回读，而非允许变化后借旧输出 |
| ZZZ source 与 SDK 时钟分离 | `66e6683` + `ed7e598`（私有 `1a0a201` + `d0283bb`） | 已读累计完整候选并集成，正在统一构建，DS 独立复核中；尚未部署/运行该版本 |
| 原神异步 physical stream/visible owner | 私有 `f6/genshin-output` 当前 `0095a40` 后继续编码 | 未合入本轮部署。正闭合独立 stream scope、真实 raw queue/tail 和异步 pending reveal，不能用最新 source/FIFO 猜生成帧归属 |

私有 worktree 在 `D:/CODE/DLSS5_NEXT-workers/{nr-continuity,zzz-source-clock,genshin-output,mod-filter}`；共享 DLSSG/FG_Hooks 的游戏专属函数由 Lead 逐段集成，不整文件覆盖。

## 本轮构建与正式部署（已完成部分）

- 构建版本：`7727a19`，Release x64，经 `build.ps1 -Jobs 12` 实际编译/链接，**exit 0 / 173 warnings / 0 errors / 77 秒**。
- DLL：**34,803,712 bytes**，mtime UTC **`2026-11-04T13:23:20.5557451Z`**，版本 **`0.1.29-final (7727a19)`**。
- 用 [Deploy-F6.ps1](../tools/runtime/Deploy-F6.ps1) 将上述产物流写正式原对象，**DEPLOYED_IN_PLACE**，exit 0。部署前后 fsutil 均核四个 hardlink：正式 F6 与三个游戏 runtime，未 rename/替换。
- 原正式回退副本：`build/rollback/OptiScaler-20261104T132418679-759008c2bcc848f8b13e8f563f1e1e28.dll`。未写 profiles。
- 部署入口先在本地硬链接夹具实际验证了同一文件对象、alias 更新和原字节回退副本，未拿该夹具当游戏运行验收。

## 星铁实际运行：PID 29660

- Biz `hkrpg_bilibili`；creation FILETIME **134382723306110116**；真实启动回执 PID 与进程 image/creation 已绑定。
- Session：`D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/log/sessions/20261104-212540_StarRail_29660`。
- 日志开头确认 **`OptiScaler v0.1.29-final (7727a19) loaded`**。精确 stop CLI exit 0，之后 PID29660=NOT_RUNNING。现回执不足以区分正常退出与强制回退，写 **UNKNOWN**。
- 8 条实际 cadence 样本均有非零模型 depth/MV，depth/MV rect **1280×800**；model work **2096×1312**、colour **2560×1600**。最新 dispatch/modelCalls **3000**，modelFailures **0**。
- 从第 3 条样本开始、其后每 600 dispatch 的 6 条样本 `feedbackHistoryUsable=true`、`feedbackFusionRecorded=true`；最新 `sceneSource=3002/sourceOrdinal=3001`，history **completed**，same published fence object **0x1780e20e2d0**，target/completed **2981/2981**，slotBusy **0**。
- 本局日志累计 FG Dispatch Ok **3152**；Deactivate **1**；FG depth refusal **3**（包含启动阶段，不能隐去）。这些计数不证明生成帧最终像素。
- GPU completed 记录各 4 组：total 中位 **28.63105 ms**、max **173.2526 ms**；model_interval 中位 **21.9612 ms**、max **162.4515 ms**。各自真实 queue/fence 的 completed ≥ target。两者 inclusive **不可相加**，也不可用倒数冒整局性能。
- **intervalUsed=1 / full-model / reuseAccepted=0**：这是连续历史反馈融合生效，不能据此称跳过模型推理已验证。原有反馈恢复会增加以前没执行的 GPU 工作。
- **最终像素、world/动态遮挡/曝光细节、净性能收益均 UNKNOWN**。本局没有截图或同条件前后对比。
- ASCII 摘要与精确采样保留在 `build/starrail-29660-summary.json`、`build/starrail-29660-excerpt.log`，避免下一局覆盖公共 runtime log 后失去来源。

## 后续执行

1. 完成 ZZZ 当前构建与 DS 复核，成功后就地部署，沿 launch-only+精确 PID/creation stop 运行，核新日志的 source serial/list/gen/epoch/sdkFrame 与实际 SDK Present 一次消费。
2. 继续原神物理输出主故障，交件后合共享函数冲突、构建、部署；同局核 input source 与真实 physical stream/output fence/owner。SDK input fence、CPU Present 返回和窗口 ACK 都不授 output 完成。
3. 运行 GIMI 完整 MOD、BeforeSR 完整 effects 到 selected Evaluate 的真实像素对比，不把本次 default-off 下普通运行当两功能验收。
4. 稳定后再测可证跳模型 detail reuse 与同条件净性能；保用户 profiles，不套 PureSR 或为过测改画质。

现有 launcher 的 activate_game/click_game/capture_game CLI 仅原神 hk4e_cn 单步骤可用。[精确 helper](../tools/runtime/README.md)虽接受三个 Biz，不能据此冒星铁/ZZZ已具自动入场能力。三游戏旧 USER_FAIL 仍有效，直至各轴有本轮证据。
