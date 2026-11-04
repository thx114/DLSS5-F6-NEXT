# F6 当前状态

更新：2026-11-04。用户已恢复持续开发、统一构建部署与三游戏验证；长期 goal 为 active。本次恢复仍为开发轮次 1，等待、服务错误和回归修订不单独算完成轮次。每完成 10 轮重新评估实际实现与运行效果。

## 工作区与职责

F6 集成根是 `D:/CODE/DLSS5_NEXT-workers/f6-integration`，分支 `codex/f6-integration`。原根 `D:/CODE/DLSS5_NEXT` 已由并发 F7 任务使用；不得 checkout、reset、clean 或覆盖其在途文件。旧根保留作历史与回退来源。

Sol 是核心源码唯一写者，另一名 Sol 独立复核；DS 处理只读搜索和日志。Lead 统一集成、构建、部署和操作游戏，不并发构建。星铁特意保留 adamfg/F5-DS 独立 FG 调度。保留 profiles，不强退游戏，不拿输入 fence、CPU 返回或窗口 ACK 证明物理输出完成。

## 当前部署

正式位：`D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/OptiScaler/official/F6/OptiScaler.dll`。

- 当前部署 `0.1.29-final (0613134)`，34,817,024 B，mtime UTC `2026-11-04T14:56:07.4441618Z`。
- SHA256 `EF7BE632CB3D05E37AC598293CB01B3767048F19851AE08CD6459D7BE8E793A4`；本次接续已重新核对，正式位和三游戏 runtime 四个 hardlink 完整。
- Lead 在独立 `genshin-compile` 对 `0613134` 真实 Release x64 构建：exit 0，131 warnings、0 errors，87 秒。经本树 `Deploy-F6.ps1` 流写原对象，profiles 未变。
- 回退件：`build/rollback/OptiScaler-20261104T145758333-364e7bec85d4497e922f620a32323959.dll`。
- 此部署包含 NR 完整模型链历史修复，尚不包含下述 ZZZ 接入和原神输出候选。

## 正在集成的候选

| 项目 | 代码与验证 | 未完成 |
|---|---|---|
| NR 完整链历史 | 私有 `1247a68`，已集成 `0613134`，独立审阅通过；CPU 74 项通过，已构建部署 | 三游戏动态细节、跳模型复用和净性能 |
| 原神 SDK 输入与真实输出 | `genshin-output` 固定 `6f806fe`，已无冲突合入 Lead 待审区；保存 SDK 调用结果独立于 Reflex marker，精确 provider/physical stream 绑定，真实最后 raw tail 与有界 reveal admission；Lead MSVC C++20/O2 独立 CPU 85 项通过 | 独立 Sol 对固定提交最终静态审阅 PASS；产品构建部署、真实 owner reveal 与画面仍待完成 |
| ZZZ F6 NGX 输入接入 | `zzz-input-route` 唯一 Sol CLI `74291` 继续；预载和未来加载的原生入口统一 ledger，原生/selected 调用分流与生命周期 | ptr+recorded ID 冲突拒绝、并发 Init 快速拒绝等最终回归与固定提交；未集成、未部署 |

原神实际 FinalOutput child runtime/selection 未成立时继续拒绝过滤；不能用父窗口已过滤作为异步 child 的证明。CPU 测试仅证明标量契约，不证明 GPU 或显示。

## 实际游戏证据

### ZZZ：仍未修复验收

用户明确反馈：**无 F6 DLSS 输入，无 FG/NR**。失败复测 PID33548，日志载入 `ed7e598`，有 1616 条通用 Streamline Evaluate，但本项目 NGX DX12 Create/Evaluate、NR cadence、FG 深度与 source/constants 收据均为 0。启动日志确认 `sl.dlss.dll`、`nvngx.dll` 已预载，旧预载 hook 只接 GetFeatureRequirements，未接 Create/Evaluate；具体缓存函数指针的查找接口未确证。

全日志：`build/zzz-33548-OptiScaler.log`，SHA256 `BD8E7629F62D7C6005FD5E6CE61792388F8756F1F5D200651207DD339C4E8DCB`。证据提取：`build/zzz-33548-evidence.json`。PID 已退出，Lead 未 stop，原因 UNKNOWN。不能以游戏自身 Streamline 调用或独立 NR 签名告警替代 F6 主链定位。

### 星铁：输入链有进展，动态画质待验

最新复测 PID27252，creation FILETIME `134382779130093750`，日志确认 `0613134`。Computer Use 观察到世界角色画面；仅静态观察，未改配置或操作登录。

- NR 8 组 cadence，最新 modelCalls=3000、failures=0，depth/MV 非零且 rect=1280×800，work=2096×1312；模型链 completed/planned=1/1，historyAnswerComplete=true。后 6 组反馈历史可用且融合已录制。
- 最新发布者 fence `0x1bea62344e0` target/completed=2999/2997，queue-ordered-gpu-pending，不能称该历史已完成或退休。
- FG depth/MV 1280×800、tags accepted；Dispatch Ok=3026，启动拒绝=3。保持 F5-DS。intervalUsed=1、reuseAccepted=0，跳模型收益未验证。
- completed GPU total/model_interval 各 3 组，各自原 fence 到达；中位 25.3727/19.6106 ms，max 29.2567/21.1251 ms。inclusive 不可相加，也不是整局净性能。

全日志 `build/starrail-27252-OptiScaler.log`，SHA256 `231CE581E9F84796E55B2EB81D8161691DB993BB68EF0929313CC234A64D1A19`；采样 `build/starrail-27252-summary.json`。进程已退出，Lead 未 stop；尾部有实际 FG owned queue fence teardown，退出原因 UNKNOWN。动态拖影、曝光、遮挡细节和净性能 UNKNOWN。

### 原神

本次固定候选尚未实机运行。必须分别核实际 SDK 输入 seal、creation owner/stream、raw HRESULT、所属 queue/fence 最后 tail、owner reveal，以及启用 FinalOutput 时真实 child 的 runtime/selection/EffectsExecuted。不能由构建或窗口可见推导生成帧像素正确。

## 次优先材料与后续

DS 限定检查了游戏根、hk4e_cn cache 和安装根 HoYoShade，未找到完整 GIMI Mods/d3dx.ini；此结论仅限检查范围。当前 ReShade preset 是 Mod OFF.ini，启用 MartysMods Launchpad/MXAO，实际 shaders 存在；BeforeSR 有材料可做单独验证，但尚无生效或像素证明。未扫描私密缓存。

下一步：收原神固定提交最终复核，统一构建；收 ZZZ 固定候选和独立复核，合入统一产品后再部署。部署前确认三游戏均未运行，保留日志和 profiles、hardlink；分别启动一次绑定新 PID/creation/version/session，先核 selected SR，再核 NR depth/MV、FG source/constants/Present 与物理输出，最后验证动态细节和同条件净性能。

`tools/runtime/Get-F6RunEvidence.ps1` 对保存/关闭日志用同一排写句柄解析和 hash，拒绝 PID/commit 不符，保留原行号。已实际提取上述两局；计数只是日志行，RecordedFenceTargetReached 只是记录值，不独立证明对象归属、selected SR、像素或性能。

`HANDOFF.md` 是暂停时历史快照；迁移说明见 `CURRENT-STATE.md`，旧逐轮记录见 `history/`。启动器精确 activate/click/capture helper 仅 hk4e_cn 语义已知，参数接受三 Biz 不代表后三游戏操作已实现。
