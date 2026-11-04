# F6 开发、部署与测试状态（已暂停）

**用户要求暂停当前功能并结束子智能体，完整转交见根 [HANDOFF.md](../HANDOFF.md)。长期 goal 已 paused；不自动继续编码、构建、部署或游戏。** 当前正式版本为 `ed7e598` / 34,816,000 B / mtime UTC `2026-11-04T13:35:24.0676033Z`，构建 exit0、120 warnings、0 errors，已流写原对象部署。ZZZ PID61120已退出、原因UNKNOWN，未取得本局NR/FG主链验收；原神遗留六文件已保存私有WIP `2384a45`（未复核/未构建/未运行）。子会话失败终止通知已收到，持久队员全 inactive、后台 jobs 全 completed/killed、三游戏进程数0。

以下是本轮运行中的阶段记录，若与最终转交冲突，以 [HANDOFF.md](../HANDOFF.md) 的保存点和测试界限为准。

当前优先级：**F6 开发 → 部署 → 测试**。用户已恢复管理员 DSH，Lead 实测权限 True。SSE 解析错误调查按用户指示暂停；诊断副本在 `build/dsh-diagnostics/`，没有修改运行中的 DSH 安装包。

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
