# F6 当前状态

更新：2026-11-05。长期 goal 为 active，用户要求直接使用旧根已有源码恢复原有功能，不把寻找旧DLL作为前提。用户已确认 5a61e97 下星铁 SR/NR/FG 基本正常，ZZZ 插帧恢复；当前优先原神回归。原神仍失败，核心写者对照 F5 查 SR/FSR 重建和显示生命周期；ZZZ NR 方向与卡顿继续在隔离树诊断。历史轮次数不代表功能进度；不把局部契约、构建或日志成功算作一轮功能完成。

用户最新指出先前原神、星铁、ZZZ 的 SR/NR/FG 均可用，仅缺 NR 深度。以该功能基线为恢复目标：最近 ZZZ 无 F6 输入、原神显示不连续是回归，不能用深度/所有权的局部进展宣称整体进步。以旧根 `work/r294-r266-build/OptiScaler` 的F5 Main源码作行为参考；旧根顶层OptiScaler是后期F6，不能混淆。保留旧树原样，在F6隔离树修复并构建，先恢复三游戏既有功能，再验证新增NR深度；不预先断言回归由NR深度代码引起。

## 工作区与职责

F6 集成根是 `D:/CODE/DLSS5_NEXT-workers/f6-integration`，分支 `codex/f6-integration`。原根 `D:/CODE/DLSS5_NEXT` 已由并发 F7 任务使用；不得 checkout、reset、clean 或覆盖其在途文件。旧根保留作历史与回退来源。

Sol 是核心源码唯一写者，另一名 Sol 独立复核；DS 处理只读搜索和日志。Lead 统一集成、构建、部署和操作游戏，不并发构建。星铁特意保留 adamfg/F5-DS 独立 FG 调度。保留 profiles，不强退游戏，不拿输入 fence、CPU 返回或窗口 ACK 证明物理输出完成。

## 当前部署

正式位：`D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/OptiScaler/official/F6/OptiScaler.dll`。

- 当前部署 `0.1.29-final (5a61e97)`，34,894,336 B，mtime UTC `2026-11-04T18:39:07.0789682Z`。
- SHA256 `5078A81B185D1532DA1AF509D317D63FC2D21D5B97E7B97664D90110ACA4C419`；流写前后文件 ID 和四个 hardlink 一致，部署时三个 profiles 哈希未变。
- 独立 `genshin-compile` 真实 Release x64 构建：exit0，118 warnings、0 errors，84秒；部署前游戏数0。回执 `build/5a61e97-product.json`。独立源码复核PASS，三个TU语法exit0、集成CPU15组/static100项exit0。
- ZZZ 插帧恢复点另存 `build/rollback/OptiScaler-5a61e97-fg-restored.dll`，SHA同部署。仅用户确认ZZZ插帧恢复，不是三游戏全部可用基线。本次部署回退件 `build/rollback/OptiScaler-20261104T183930959-66dc649a02564857bf6959ca442c8034.dll` 是失败版 accf7e8。
- 已集成并经独立 Sol 复核：ZZZ `199597d+ca05507` 输入接入/后端释放，原神 `808dc81` drain 连续性。Lead CPU：Router9组、Release7组、原神109项及wiring39项通过；均不证明游戏功能恢复。
- 已收尾 ZZZ PID31592，creation FILETIME `134382911960461160`，CLI73828 exit0，日志确认5a61e97。用户确认插帧恢复，但间歇卡顿、较高延迟和小卡顿仍在；NR深度上下颠倒（实际模型方向未定），运动仍光流。进程已退出，Lead未stop；退出方式UNKNOWN。
- `a100461+accf7e8` 修正同步 SDK 帧与安全基础呈现，`5a61e97` 统一虚继承的 IFeature 基类身份并显式传同次 selected feature，修复入口成功却发布全拒的问题。真实 source/tags/constants/SDK Present 采样4398/4399/4400及5054一致；SDK另有presented6/status0采样，用户实测才是插帧恢复依据。
- 原神 PID64796 实测失败：用户反馈无深度、无FG/DLSS，有NR，很卡，FSR疑似反复重启；原神仍失败，Halley 候选待交件。Sol隔离树 `genshin-baseline-recovery` 继续查完整回归根因，先不扩展FinalOutput。星铁 5a61e97 已由用户确认 SR/NR/FG 基本正常，保持adamfg/F5-DS。
- 现场 ZZZ PID50760 正在运行；正式位和 profiles 未动，不自动操作 UI、强退或部署。

## 正在集成的候选

| 项目 | 代码与验证 | 未完成 |
|---|---|---|
| NR 完整链历史 | 私有 `1247a68`，已集成 `0613134`，独立审阅通过；CPU 74 项通过，已构建部署 | 三游戏动态细节、跳模型复用和净性能 |
| NR 方向诊断候选 | `ee6f8c8` 已独立 Confucius PASS，Lead 集成为 `9546c2e`；Lead CPU 63 项 exit0 | 仅诊断，不改翻转/算法/GPU 行为；未产品构建、未部署 |
| 原神 SDK 输入与真实输出 | `6f806fe` 输出生命周期、`808dc81` drain前先生产原生guides并精确撤销未交SDK收据；独立复核PASS，已集成部署74f5ef8 | 原神仍失败；Halley 候选待交件；既有PID6748仅bac4b64，不能作为新修复验收 |
| ZZZ F6 NGX 输入接入 | `199597d+ca05507`，独立复核PASS，已集成部署74f5ef8；Release失败保完整context并阻native Shutdown，内部替换先预检 | 未知/注入前已创建handle仍native-forward；必须实测owner=selected的Create/Evaluate及SR/NR/FG，hook安装不等于恢复 |
| 原神 FinalOutput child runtime | `finaloutput-runtime` 从 `bac4b64` 分离，仅 HoYoShade 集成文件；已要求 Rawls 停止新增编码、保留现状，优先基线恢复 | child 创建/配置/selection/合法 raw 前更新和 GPU 退休尚未闭合，ON 实机待验；不得把扩展此链当作恢复原有 SR/NR/FG 的替代 |

原神实际 FinalOutput child runtime/selection 未成立时继续拒绝过滤；不能用父窗口已过滤作为异步 child 的证明。CPU 测试仅证明标量契约，不证明 GPU 或显示。

## 实际游戏证据

### ZZZ：插帧已恢复，深度方向与性能未通过

用户明确确认5a61e97插帧恢复。最终日志 `build/zzz-31592-OptiScaler-final.log`，11,989,796 B，SHA256 `89CD0FDC15CC8B37F070D53D86AD0C08FBA87AA048ED59CA39C95537F0C6B581`；提取 `build/zzz-31592-final-evidence.json`。日志匹配：selected Create/Evaluate各2、NR cadence8、FG Dispatch Ok3014、source SDK Present8、depth refusal51。限频source/tags/constants/Present帧一致，SDK有presented6；NR仍motionIsPictureFlow=true、reuseAccepted=0。以上计数不是成功帧数或动态画质证明。用户要求先测另两游戏，方向修复与卡顿调查不得覆盖此恢复点。

DS对运行中固定快照独立核对了4组完整source/tag/constants/producer/Present同帧同list样本（4398、4399、4400、5054），不扩为全局逐帧证明。卡顿证据：5处waitCPUFence 500ms超时（行35348/35703/36049/37641/44119）、4处Pacer flush超时，后段65904/65906还有worker超时；NR自身GPU区间不能解释这些CPU等待。一次SR/FG feature切换后仍有间歇高延迟，不能只归启动。6X硬件flip metering警告与每帧SetOptions/GetState是待测风险，不是已证因果；不因此盲改配置、删fence或扩大池。原神优先，ZZZ性能后续沿实际SDK调用/队列等待边界定位。

最新 PID56784 的日志保存在 `build/zzz-56784-OptiScaler.log`（6,790,706 B、85,311行，SHA256 `0B81B6CAF86B1474161272EC8EB83E367385D318699A3639538EEDDE7135625C`），提取 `build/zzz-56784-evidence.json`。8条限频 SL scope 样本明确同线程真实 frame4554–4561、viewport2、规范化 list；NGX selected Create/Evaluate各2条，但没有 selected frame source/tag/SDK Present，FG Dispatch Ok=0，64条限频depth refusal。入口选择未报缺scope，发布却拒绝：优先检查同次 selected feature 在不同基类指针表示及 provider viewport 生命周期中的接线；不能删除身份条件来掩盖。NR仍有深度和历史融合、motionIsPictureFlow=true、reuseAccepted=0。本局不证明任何 FG 恢复或动态画质收益。

前局 PID46372 用户明确反馈：**FG没了，深度有，运动还是光流**。日志确认 `74f5ef8`，selected NGX Create/Evaluate各2条、项目DX12 Evaluate6256条、NR cadence8条、FG Dispatch Ok2811条；这些是日志匹配数，不能替代显示验收。7条限频SDK Present返回0，但限频MFG状态始终 `presented=1`；生成帧未得到证明，不能据此推翻用户反馈。

NR末次采样 modelCalls=2226、modelFailures=0，depth rect2080×1296、MV rect2560×1600；`motionIsPictureFlow=true`、`producerNativeGuideContract=false`、`sceneModelGuides=true`、`sceneModelMotion=false`。独立源码复核确认该光流选择在迁移基点已存在，Late.inl到74f5ef8未改；原生场景depth经规范化供模型使用，原生MV未成为模型最终MV。恢复FG不以前置改造NR运动为条件。FG有1280×800 depth/MV tags，64条限频拒绝主要是slot-in-flight/no-fresh-source。intervalUsed=1、reuseAccepted=0；历史发布者fence target/completed=2998/2997，仍pending。深度存在不等于原生运动已接入，也不证明全屏动态细节正确。

全日志 `build/zzz-46372-OptiScaler.log`，9,157,217 B、112,699行，SHA256 `517DC3B09C74089DECEE4082570373A27D142776290A0320EFA54490EFCD97A6`；提取 `build/zzz-46372-evidence.json`。末尾记录 `Unhandled exception C0000005 at 0`，成功写出 `OptiScaler_crash_46372_55540_01DD6C7A19435B6D_62980968_1.dmp`，260,939,502 B。Lead未停止游戏；有崩溃记录，具体故障模块与因果尚未确定。

只读定位：日志35448发布SDK/token帧1，35451 SDK却请求frame4646并退用frame1，确证一次constants身份断链；不据此独断整局FG或崩溃原因。66e6683/ed7e598独立SDK计数与原样转发的Reflex ID是优先比较范围，仅授权收口这一错误，不扩大NR/显示架构。presented=1是距上次GetState的呈现数，非累计生成数。转储展开的故障栈为0→AkSoundEngine.dll+0xD47E0等；匹配映像+0xD47DE是call rax、RAX=0，不能将音频模块空调用直接归因FG。失败DLL另存 `build/rollback/OptiScaler-74f5ef8.dll`，SHA与本局一致，仅作失败参照。

前一失败局 PID33548/`ed7e598` 是“无F6输入、无NR/FG”：1616条通用Streamline Evaluate，而项目NGX、NR cadence、FG depth及source收据为0。其日志 `build/zzz-33548-OptiScaler.log`，SHA256 `BD8E7629F62D7C6005FD5E6CE61792388F8756F1F5D200651207DD339C4E8DCB`。输入接入已有变化，但最新局仍未恢复FG可用性。

### 星铁：本次 SR/NR/FG 用户确认基本正常

5a61e97 本局 PID30980，creation FILETIME `134382917978895818`，CLI61816真实exit0。**用户确认 SR、NR、插帧基本正常**；这是本轮基础功能验收，不扩大为动态细节、深度像素或跳模型复用/净性能验收。原生运动仍在，保持adamfg/F5-DS调度。进程已退出，Lead未stop；退出方式UNKNOWN。

运行中采样 `build/starrail-30980-OptiScaler.log`（SHA256 `D6284100E41F8F3A91021759DC6BD9E471449E40BC1CBA2D6C8186FB5EC47AAC`），模型 depth/MV均1280×800、work2096×1312，motionIsPictureFlow=false、nativePhaseVerified=true，modelCalls1200/failures0、historyUsable/FusionRecorded=true。fence target/completed1199/1198仍pending；intervalUsed1/reuseAccepted0。FG日志明确F5-DS，后段SDK有presented6/status0。此前日志匹配1790条Dispatch Ok、3条启动拒绝；这些计数不是实际插帧数。尝试退出后取完整日志时，公共日志已属于另一PID68120；提取器实际拒绝PID不匹配，未产生30980的final evidence。当前只有上述已绑定的运行中采样，不能把后局日志归入本局。

此前复测 PID27252，creation FILETIME `134382779130093750`，日志确认 `0613134`。Computer Use 观察到世界角色画面；仅静态观察，未改配置或操作登录。

- NR 8 组 cadence，最新 modelCalls=3000、failures=0，depth/MV 非零且 rect=1280×800，work=2096×1312；模型链 completed/planned=1/1，historyAnswerComplete=true。后 6 组反馈历史可用且融合已录制。
- 最新发布者 fence `0x1bea62344e0` target/completed=2999/2997，queue-ordered-gpu-pending，不能称该历史已完成或退休。
- FG depth/MV 1280×800、tags accepted；Dispatch Ok=3026，启动拒绝=3。保持 F5-DS。intervalUsed=1、reuseAccepted=0，跳模型收益未验证。
- completed GPU total/model_interval 各 3 组，各自原 fence 到达；中位 25.3727/19.6106 ms，max 29.2567/21.1251 ms。inclusive 不可相加，也不是整局净性能。

全日志 `build/starrail-27252-OptiScaler.log`，SHA256 `231CE581E9F84796E55B2EB81D8161691DB993BB68EF0929313CC234A64D1A19`；采样 `build/starrail-27252-summary.json`。进程已退出，Lead 未 stop；尾部有实际 FG owned queue fence teardown，退出原因 UNKNOWN。动态拖影、曝光、遮挡细节和净性能 UNKNOWN。

### 原神：最新实测失败，不能以物理输出日志宣称恢复

最新 PID64796，creation FILETIME `134382915999556944`，日志载入5a61e97；启动时逐游戏Opt开关true、选择official/F6，未改回旧快照的F7。用户实测：**没有深度、没有FG和DLSS，有NR，很卡，FSR好像一直重启**。保存 `build/genshin-64796-OptiScaler.log`，33,974,207 B，SHA256 `25A991216035C0178367F4626F830C4BF1CF100C9CC9547603C5A149290E971A`；提取 `build/genshin-64796-evidence.json`。DX11 Create/Evaluate日志8/797、NR cadence4、SDK closure92、owner shown82，同时物理拒绝43及epoch反复变化。此局否定功能恢复；不能以 shown-after-physical-tail 或SDK返回0推翻用户反馈。进程已退出，Lead未stop，退出方式UNKNOWN。Sol先查selected SR重建/FSR fallback与显示拒绝的实际生命周期因果。

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

可用基线搜寻：DS已限定检查旧项目根、workers、安装根（含OptiScaler、cache/games及具名备份）及已知recovered-temp/cleanup/_evidence/work/artifacts/references，未找到 F5_三游戏覆盖包_Final_20261103.zip 或33,359,360 B的DLL。声明SHA256 `5986BC7377A34A5451ED14DC0CE481A20F30F8808B966ADC47583CF845986BB3`只有清单证据。r301输出有33,361,408 B、SHA `8AD975292E08EE5B735B035C0B8F05AABF05F8E1EBA10697806AF29F2B6DC8CE`、UTC2026-11-03T14:31:31Z的实际候选，版本同为a4d7a345，但与F5 Final不同，尚未绑定用户可用反馈；预迁移34,796,544 B的a4d7a345也不是已知可用版本。版本号和目录标签不能充当基线，不盲目部署。

下一步优先原神重建/显示回归；星铁SR/NR/FG与ZZZ插帧均有本轮用户确认，保留5a61e97恢复点。ZZZ NR方向和间歇卡顿仅在隔离树调查，暂停新增NR原生MV/FinalOutput扩展。星铁不改adamfg/F5-DS调度；复用收益与三游戏动态细节尚未验收。部署前确认游戏均未运行，保留现有profiles及hardlinks。

`tools/runtime/Get-F6RunEvidence.ps1` 对保存/关闭日志用同一排写句柄解析和 hash，拒绝 PID/commit 不符，保留原行号。已实际提取上述两局；计数只是日志行，RecordedFenceTargetReached 只是记录值，不独立证明对象归属、selected SR、像素或性能。

`HANDOFF.md` 是暂停时历史快照；迁移说明见 `CURRENT-STATE.md`，旧逐轮记录见 `history/`。启动器精确 activate/click/capture helper 仅 hk4e_cn 语义已知，参数接受三 Biz 不代表后三游戏操作已实现。
