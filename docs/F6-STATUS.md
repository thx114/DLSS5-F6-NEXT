# F6 当前状态

更新：2026-11-05。用户已恢复持续开发、统一构建部署与三游戏验证；长期 goal 为 active。本次恢复仍为开发轮次 1，等待、服务错误和回归修订不单独算完成轮次。每完成 10 轮重新评估实际实现与运行效果。

用户最新指出先前原神、星铁、ZZZ 的 SR/NR/FG 均可用，仅缺 NR 深度。以该功能基线为恢复目标：最近 ZZZ 无 F6 输入、原神显示不连续是回归，不能用深度/所有权的局部进展宣称整体进步。先追最后可用 DLL、源码与配置并恢复三游戏既有功能，再验证新增 NR 深度；不预先断言回归由 NR 深度代码引起。

## 工作区与职责

F6 集成根是 `D:/CODE/DLSS5_NEXT-workers/f6-integration`，分支 `codex/f6-integration`。原根 `D:/CODE/DLSS5_NEXT` 已由并发 F7 任务使用；不得 checkout、reset、clean 或覆盖其在途文件。旧根保留作历史与回退来源。

Sol 是核心源码唯一写者，另一名 Sol 独立复核；DS 处理只读搜索和日志。Lead 统一集成、构建、部署和操作游戏，不并发构建。星铁特意保留 adamfg/F5-DS 独立 FG 调度。保留 profiles，不强退游戏，不拿输入 fence、CPU 返回或窗口 ACK 证明物理输出完成。

## 当前部署

正式位：`D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/OptiScaler/official/F6/OptiScaler.dll`。

- 当前部署 `0.1.29-final (bac4b64)`，34,827,264 B，mtime UTC `2026-11-04T15:37:31.9427821Z`。
- SHA256 `6A207553506314DA0A0778DC7D45E87E2D8AF46E94228CFB73C82571F432FB64`；本次接续已重新核对，正式位和三游戏 runtime 四个 hardlink 完整。
- Lead 在独立 `genshin-compile` 对 `bac4b64` 真实 Release x64 构建：exit 0，123 warnings、0 errors，84 秒。经本树 `Deploy-F6.ps1` 流写原对象，profiles 未变。
- 回退件：`build/rollback/OptiScaler-20261104T153748776-d803b08d96b649f28aa7a9c289bd308c.dll`。
- 此部署包含 NR 完整模型链历史修复及原神 `6f806fe` 输出候选，尚不包含 ZZZ `199597d` 输入接入。

## 正在集成的候选

| 项目 | 代码与验证 | 未完成 |
|---|---|---|
| NR 完整链历史 | 私有 `1247a68`，已集成 `0613134`，独立审阅通过；CPU 74 项通过，已构建部署 | 三游戏动态细节、跳模型复用和净性能 |
| 原神 SDK 输入与真实输出 | `genshin-output` 固定 `6f806fe`，已集成 `bac4b64`；保存 SDK 调用结果独立于 Reflex marker，精确 provider/physical stream 绑定，真实最后 raw tail 与有界 reveal admission；Lead MSVC C++20/O2 独立 CPU 85 项通过 | 独立 Sol 最终静态审阅 PASS，已构建部署 `bac4b64`；实机 PID6748 有真实 owner tail 后 reveal；输入/显示周期失效、动态画面与净性能待验 |
| ZZZ F6 NGX 输入接入 | `zzz-input-route` 基础提交 `199597d`；预载和未来加载原生入口统一 ledger，ptr+recorded ID 归属与生命周期；Lead 独立运行 9 组生产 Router CPU 回归通过，尚未集成 | Confucius 审出真实 backend Release 失败被析构吞掉的 P1；原 Sol 会话 CLI65676 正补失败传播、完整 context 保留和内部重建预检。旧待审 merge 已撤销，待最终提交复核后整体合入构建部署 |
| 原神 FinalOutput child runtime | `finaloutput-runtime` 从 `bac4b64` 分离，仅 HoYoShade 集成文件；已要求 Rawls 停止新增编码、保留现状，优先基线恢复 | child 创建/配置/selection/合法 raw 前更新和 GPU 退休尚未闭合，ON 实机待验；不得把扩展此链当作恢复原有 SR/NR/FG 的替代 |

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

### 原神：物理输出有进展，连续性仍未验收

最初 launch-only CLI 真正 exit 1：配置投影被拒。已只读核 SQL 的原神 per-game 选择及全局 state.json 都是 official/F7；F6 主 INI 与 profiles/hk4e_cn.ini 完整。新 runtime 5712D6B04FE83A06 的 177,152 B DLL 属另一构建，不能误判 F6 缺配置。

经启动器正式步骤临时选择 official/F6 后，真实 CLI PID25808 exit 0，原神 PID6748，creation FILETIME `134382808496529911`，session `20261104-234739_YuanShen_6748`，日志确认 `bac4b64`。启动后已正式恢复 per-game official/F7（restore CLI exit 0），use_optiscaler=true，三个 F6 profiles 哈希均未变，未改全局选择。

- 日志有项目 DX11 Create/Evaluate 4/446 行，3 组 NR cadence；最后采样 modelCalls=3/failures=0、depth/MV 2560×1600、model 2096×1312；历史 source fence `0x28461b34cb0` target/completed=2/2，feedbackHistoryUsable/FusionRecorded=true。intervalUsed=1、reuseAccepted=0。
- 独立 Sol 按固定源码与本局日志确认：游戏 1536×960 depth 经 PreparedDepth/ResolveIsolated 映射为 2560×1600 picture-depth，再进入实际模型；MV 是 finished-picture optical flow，不是原生 render MV。nativePhaseVerified=false 等字段不否定 pictureTemporalQualified 授权，但不证明全屏 depth 有效。日志50658一次 ISOLATED_PICTURE_MAPPING_SUBMISSION_FAILED 导致明确无 depth 推理并 reset，下一源恢复；具体提交失败阶段待查，不借旧 depth。
- FG tagged depth/MV 2560×1600、tags accepted；7 条 bounded Dispatch Ok 日志，不等于全局实际调用数。
- 18 条保存 SDK closure、16 条 stream-output、17 条 owner-output，其中 16 条 event=shown-after-physical-tail。例如 stream2/generation1/sequence119，raw queue `285869E5030`、fence `284606604A0`、candidate/lastRaw target/completed均119/119，inFlight/uncertain=false，association=sdk_output_stream；输入收据不负责输出完成。
- 显示仍反复出现 SdkStreamPostvalidationRefused / NoAuthenticatedCameraOrGuides，epoch 周期递增，原写者正在修 drain 后下一输入的连续性。FinalOutput startup latchedEnabled=false/stage1，本局未观察 ON 链。独立复核确认已有 child runtime/selection 消费代码，但实际 child 创建、配置、raw 前回调更新及退休尚无显式闭环；缺少回调不能推断 runtime 不存在，不能冒用父 selection。

完整日志 `build/genshin-6748-OptiScaler.log`，11,672,989 B，SHA256 `73038E08D0941DF68F628A328F38AADFC923DD19B0786C90CC31FE00FF9A33E6`；字段 `build/genshin-6748-evidence.json`。游戏已退出，Lead 未 stop，原因 UNKNOWN。窗口截图请求未取得可用的动态画面验收；source、GPU、owner 日志不证明生成帧像素、细节或性能正确。

## 次优先材料与后续

DS 限定检查了游戏根、hk4e_cn cache 和安装根 HoYoShade，未找到完整 GIMI Mods/d3dx.ini；此结论仅限检查范围。当前 ReShade preset 是 Mod OFF.ini，启用 MartysMods Launchpad/MXAO，实际 shaders 存在；BeforeSR 有材料可做单独验证，但尚无生效或像素证明。未扫描私密缓存。

下一步：DS 追最后三游戏 SR/NR/FG 可用基线的 DLL、源码与配置身份，Lead 对照回归边界；原有 ZZZ Release 安全和原神连续性修复继续收口，暂不扩大新增 runtime。仅在明确基线关系且最终独立复核通过后统一构建部署。部署前确认三游戏均未运行，保留日志和 profiles、hardlink；分别绑定新 PID/creation/version/session，先核原有 SR/NR/FG 可用性，再核新增 NR depth/MV、历史、动态细节和同条件净性能。

`tools/runtime/Get-F6RunEvidence.ps1` 对保存/关闭日志用同一排写句柄解析和 hash，拒绝 PID/commit 不符，保留原行号。已实际提取上述两局；计数只是日志行，RecordedFenceTargetReached 只是记录值，不独立证明对象归属、selected SR、像素或性能。

`HANDOFF.md` 是暂停时历史快照；迁移说明见 `CURRENT-STATE.md`，旧逐轮记录见 `history/`。启动器精确 activate/click/capture helper 仅 hk4e_cn 语义已知，参数接受三 Biz 不代表后三游戏操作已实现。
