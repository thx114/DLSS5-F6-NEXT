# F6 开发转交

最新候选与实现见 [当前状态](CURRENT-STATE.md)：**7C281A54** 于 **2026-11-11 13:27:06（UTC+08）**构建、离线回归并原位部署。四条运行库硬链接路径／File ID 不变，七份配置未改；回退件为此前 **0AD66318**，见 `build/current-deployment.json`。本轮隔离 SDK 输出队列并在长呈现 NR 前提交真实 FG guides 读取、沿准确 queue／共享 fence 交回 DX11 写权限；WARP 15 场景／564 检查及最终共享回归通过。**实机画面、帧节奏和延迟仍待用户新局验证**，不以构建或部署当作画面通过。下一次优先按原参数测试呈现 NR＋FG、细节历史关闭。

首先读 [当前状态](CURRENT-STATE.md)、[预览操作](F6-GUIDE-DIAGNOSTICS.md)与当前 build 回执。保留用户配置、独立 SR／NR／FG、真实 source／SDK／display 身份及所属 queue/fence／Reset 退休门；内部 FSR 桥继续禁用。下方为历史记录。

更新：2026-11-08。接手先读本文件、[协作规则](../AGENTS.md)和[当前状态](CURRENT-STATE.md)。**当前候选 10503898 实机失败：NR 先约 7 FPS，用户“切了下画面”后约 27 FPS；FG×6 仍慢放，关闭 FG 后画面卡死不动。尚未修复，也未达到大世界纯 NR 至少 35 FPS 的目标。**

## 1. 工作位置与用户要求

- 唯一产品写树：`D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，分支 `codex/f6-rewrite`，HEAD 基线 `db0e6469aea26af0358512c66b6f1e605ad37e9b`。大量改写和新增头文件、测试尚未提交；交接或提交必须包含未跟踪源码，不能只取 HEAD。禁止 reset、checkout、清理或覆盖现有并行改动。
- 主根 `D:/CODE/DLSS5_NEXT` 保留；`D:/DATA/Documents/ChatGPT/DLSS F5 NEXT` 是历史与回退来源。主根的迁移记录、旧根构建流程均不能直接当作本工作树的当前状态。
- 用户授权大改：结合 Encore／sm86／F5 和逆向结果重写、精简 F6，形成公共抽象接口，减少原神／星铁／ZZZ 整条项目线路的复制隔离；星铁可按实际需要部分隔离。当前重构尚未完成验收。
- 保留用户配置和 SR／NR／FG 独立开关、效果语义，不降低 NR 层数、工作分辨率或强度来冒充优化。
- 不点击提权游戏，不强退。启动、进入世界、切换场景、视觉验收和正常退出由用户手动完成。此前启动入口曾误选 F8；不能拿 F8 会话验收 F6，也不要擅改用户的 F6／F8 选择。
- 子智能体仅在当前主会话新建和管理；不得到旧会话调用或恢复，创建超过 30 分钟禁止复用，默认每次新建。规则已写入工作树和主根 `AGENTS.md`。此前实例已关闭，不要恢复它们。
- 本次用户确认只加载 OptiScaler。过去把 Bridge 等其它模块归入本局性能原因使用了旧日志，已经撤回；`session.txt` 的 `mod Bridge` 是启动器配置记录，不能证明模块加载。

## 2. 最新候选、部署与实测身份

| 项目 | 当前记录 |
|---|---|
| 候选 | `10503898`，完整 SHA256 `10503898606F5CBD6C0951A9BE4AEEE014815633715348139187EF0EF4538D5B` |
| 构建产物 | `build/out/OptiScaler.dll`，35,017,216 B；mtime `2026-11-08T05:35:44.5034057Z` |
| 构建 | UTC 05:34:01—05:35:45，Release x64，MSBuild exit 0，144 warnings／0 errors |
| 正式 DLL | `D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/OptiScaler/official/F6/OptiScaler.dll` |
| 部署 | UTC 05:36:54 原位部署；回执记录四条硬链接、原文件对象和七份配置保留，未改 F6／F8 选择。本次交接再次核对产物与正式 DLL 哈希相同 |
| 最新会话 | `20261108-134312_YuanShen_107608`，PID `107608`；日志载入版本 `v0.1.29-final (db0e646)` |
| 身份边界 | 会话 PID、日志版本与已部署磁盘候选关联；精确 loaded-memory SHA 未核。多个含未提交改动的候选都显示 `db0e646`，commit 相同不代表 DLL 相同 |
| 用户实测 | 原神大世界，两层、50% NR：先约 7 FPS，用户“切了下画面”后约 27 FPS；FG×6 慢放，关闭 FG 后画面冻结。不能擅自把“切画面”解释成传送、切窗口或改分辨率 |
| 会话收尾 | 启动器记录 `13:46:23` 进程退出，退出码 `-805306369`；**不是已确认正常退出**。退出触发方及方式未确认 |

上表会话及下文游戏日志时间为 **2026-11-08 UTC+08:00**；构建、部署的 `Z` 时间为 UTC。不要按当前终端时区直接对齐。

本次只保存日志和更新交接，未继续改产品代码、重新构建、部署或回退。读取时 CIM 仅见历史 PID51144／60652，均 0 线程／0 句柄；这既不表示有活跃新游戏，也不能证明 107608 正常退出。

回退文件：`build/rollback/OptiScaler-20261108T053654413-fc3599deff9040709cdf7489449189e9.dll`，SHA256 `8240EE70B2504844FBAA4FED4DEB37D0D681127DBD57DC36A00CE0446FC8A35D`。**这是同样性能失败的上一候选，不是已知良好版本。**

## 3. 当前问题及证据边界

| 优先级 | 问题 | 已知与未知 |
|---|---|---|
| P0 | 关闭 FG 后画面冻结 | 最新用户明确复现。日志记录 FG 关闭、`Off+retain` 和一次回到 native fallback，随后截断；不能据此认定已找到死锁位置或某条 Wait 必然有错 |
| P1 | 纯 NR 不达标且启动表现不稳定 | 10503898 为 7→27 FPS，目标至少 35；上一候选 8240EE70 大世界约 25。操作变化的精确时间未记录，不能从日志猜出 7→27 的原因 |
| P1 | NR＋FG×6 慢放、卡顿 | SDK 接受 5 个生成帧不证明真正显示六倍或流畅。最新局仍有 SDK 在 `eOn` 请求下反复停用／恢复和 `flushAll`，触发原因未知 |
| P1 | NR 可选深度映射不完整 | 最新世界输入出现 `producer-depth-without-picture-uv-mapping`：motion 存在，NR model depth 缺失。源有 depth 不等于可直接用于 picture；不能删除映射约束或借旧深度 |
| P2 | 进入世界／切场景曾掉 DLSS | 已实现新旧 FSR 变种认证和 RGB 合成；最新局世界 shader 持续生产到 FG 关闭前，但不能外推所有场景已修复 |
| P2 | 登录界面性能与旧帧闪回历史回归 | 较早候选有严重低源帧率、闪回，后有用户反馈无顿卡／暂未见闪回。10503898 未建立完整登录性能与闪回通过结论 |
| P2 | 星铁 SR＋NR＋FG 联合闪回、卡顿 | 用户怀疑反复释放资源；历史 PID48136 仅有尾段日志，不足以确证。当前候选尚未重新验收星铁 |
| P2 | ZZZ 与强制常驻 | ZZZ 只有旧版本恢复反馈，不代表当前通过。独立常驻逻辑已有代码，但 FG 缺合法 depth／motion 仍 Off＋retain／直通，未实现无深度持续插帧；不得靠旧帧完成该需求 |

NR 常驻的当前颜色 fallback 与场景 NR 是不同语义；有效光流时仍须执行实际模型，缺光流使用零运动并 reset。不能把 fallback 可运行写成场景 guides 已正确。星铁／ZZZ 历史范围见 [F6 状态](F6-STATUS.md)。

## 4. 最新日志怎么读

### 找对会话和原文

安装根虽然目录名带 `1.4.5.0`，本局启动器元数据显示 `hub=1.4.8.2`；不要靠目录名推断程序版本。

```powershell
Set-Location D:\CODE\DLSS5_NEXT-workers\f6-rewrite
$install = 'D:\APPS\test\TEST\HoYoShadeHub_Portable_1.4.5.0_x64_Full'
Get-ChildItem -LiteralPath "$install\log\sessions" -Directory |
    Sort-Object LastWriteTimeUtc -Descending |
    Select-Object -First 5 Name, LastWriteTimeUtc
$sessionDir = Join-Path $install 'log\sessions\20261108-134312_YuanShen_107608'
Get-Content -LiteralPath "$sessionDir\session.txt"
Get-ChildItem -LiteralPath $sessionDir | Select-Object Name, Length, LastWriteTimeUtc
```

`session.txt` 给出 PID、启动时间、快照路径、活动日志别名和退出状态。本局 `[Log] LogFileName` 指向正式 F6 目录中的 `OptiScaler.log`；该文件会被后续会话覆盖，优先读 session 固定快照。`digest.txt` 仅用于定位，E／W 是摘要规则分类，包含正常证据字段，不能把数量直接当运行错误或根因。ReShade 等旧配置快照也不是当前加载证据。

本局原始快照：`opti-configured-2E415BFEBE7E11B2-OptiScaler.log`。已按原始 bytes 保存为 [build/genshin-107608-OptiScaler.log](../build/genshin-107608-OptiScaler.log)：**6,749,117 B／42,099 行**，SHA256 `0A9AD2E956CB1FD06DEC8A2C663164917544E174EA9D16A69E7C6BC51FFEA242`。首行 `13:43:11.184001`，末行 `13:45:34.693762`，末行不完整；保留原貌，不补写。

配置快照：`cfg-2E415BFEBE7E11B2-OptiScaler.ini`。注意 `HookCpuTiming=true` 是配置文本，不能据此推断本次会话运行态采样已开启；本局重型 `R448` 记录为零，F6 有每会话默认关闭逻辑。轻量 FSR HUD 与重型 Debug 采样分开。

### 运行现成解析器

以下命令已对本局执行成功，结果在 [build/genshin-107608-evidence.json](../build/genshin-107608-evidence.json)：

```powershell
$gameLog = '.\build\genshin-107608-OptiScaler.log'
Get-FileHash -LiteralPath $gameLog -Algorithm SHA256
pwsh -NoProfile -File ./tools/runtime/Get-F6RunEvidence.ps1 `
    -Log $gameLog -ExpectedPid 107608 -ExpectedCommit db0e646 `
    -Output build/genshin-107608-evidence.json

rg -n 'FGEnabled:|teardown|Deactivate|FrameGenerationDisabled|sdk-reflex' $gameLog
rg -n 'R082 Present CPU window|Genshin producer progress|R846 FG visibility-boundary|F6 owner-output|F6 physical-output refusal' $gameLog
rg -n 'R849 NR actual input|DLSS-NR GPU evidence completed|Native motion \(D3D11\) prep GPU completed|DLSS-NR cadence recorded|flushAll|DLSS-G interpolation state changed' $gameLog
```

新局必须重新核对 PID、日志载入版本、session 启动时间和部署候选，再替换参数；不要沿用 107608。解析器只接受已关闭／复制完成的日志，通过读共享锁排除活动 writer；若文件仍被写入，应待会话结束后固定副本，不绕过锁。哈希针对完整原始 bytes，解码 replacement 不得影响哈希。

`-SampleLimit` 默认 64，只保留每类末尾 64 条详细样本；Counters 是全文件匹配行数，也不是实际调用总数／成功数。查早期 7 FPS 阶段须回原文，不能只看末尾样本。解析器的 `IndependentlyVerified` 保持 false 是证据边界，不是解析失败。

### 关键字段及误读风险

| 日志入口 | 用途与边界 |
|---|---|
| `Genshin producer progress`、shader hash、generation／source_frame／picture | 看真实 FSR 生产与帧身份是否前进；token-matched 不证明 GPU 完成或显示 |
| `R082 Present CPU window` | 区分相邻源 Present 入口间隔、本体 wall、外部等待；不是显示 FPS，也不是 GPU 排他耗时 |
| `DLSS-NR cadence recorded`、`R849 NR actual input` | 核对实际两层、workSize、modelCalls、motion／depth、映射拒绝；modelFailures=0 不证明视觉正确 |
| `Native motion (D3D11) prep GPU completed` | 按同一输入的 source_epoch／generation／picture 关联前处理；flow／guides 是包含区间，不直接加到另一帧模型时间上 |
| `DLSS-NR GPU evidence completed` | 对齐 sampleDispatch／sequence 和实际 queue／fence／target／completed；observe 时刻通常晚于被测输入，model_interval 是 total 子集 |
| `F6 FG capability`、`F6 FG configuration` | 看能力、配置请求、缓存命中和接受结果；`raw_max_generated=5`／`fixed_total=6` 不证明生成像素或扫描输出 |
| `R827 FG constants`、`R827 FG retirement`、`R1069 sdk-reflex` | 对齐输入、reset、SDK 标记、sleep 和资源退休；Ready、CPU 返回、窗口 ACK 不能替代所属队列完成证明 |
| `R846 FG visibility-boundary`、`F6 owner-output`、`R564 visible` | 区分 SDK 接受、物理输出、窗口 reveal、native fallback；child_call 包含整个 hook 和 ReflexSleep，不等于原始 SDK 独占成本 |
| `flushAll`、`DLSS-G interpolation state changed` | SDK 内部启停不等于 F6 反复调用 SetOptions Off；先对齐请求、源资格、常量和生命周期 |
| `R448` 与 FSR HUD | 前者是重型 Hook CPU 采样；HUD 的 `~ ms/s` 是已核验根 Hook 附加 CPU 耗时的采样估算，排除原游戏调用，不是单帧延迟 |

### 本局已经确认的时间线

- `13:43:10.919+08:00`：launcher 会话启动；`13:43:11.184001`：载入 db0e646。
- 实际 NR 记录为输出 `2560x1600`、模型 `1280x800`、两层；不是拿降低设置解释性能变化。
- `13:44:14.465396` 起已有世界 FSR `FEAC669E48FD931E` 的 producer progress，持续到 `13:45:34.111481`；本局不能继续按旧版“进入世界后完全无 SR 输入”解释全部症状。
- `13:45:13.432155`：UI 请求 FG=true；`.627303`：唯一一条 FG configuration，SDK 接受 `5 generated / fixed total 6`。
- FG 开启段有 17 次插帧状态变化：9 次 enabled、8 次在 `eOn/5 generated` 请求下 disabled，另有 8 条 `flushAll`。原因未知；不能据此盲改 Reflex 或源资格门。
- `13:45:34.638013`：UI 请求 FG=false；`.674164`：`DLSSG_Dx12::Deactivate` 记录 `Off+retain`；`.693762`：末行 `FrameGenerationDisabled`，`nativeCalled=true`、`childCalled=false`、fallback=true，行尾截断。只证明记录到了回退尝试，不能证明画面恢复或 drain 完成。
- `13:46:23`：launcher 收尾记录进程退出，退出码 `-805306369`。未确认退出方式，也没有正常退出证明。

用户“切了下画面”的操作时间和 7→27 FPS 分界未记录；上面的事件不能擅自拿来替代该分界。

最新低频 GPU 样本例：sequence1536 的 NR total `16.8970 ms`、model_interval `15.3139 ms`；另一个输入的 flow 约 `4.16 ms`。它们不能相加得出整帧耗时或根因。解析器有 1,560 条 DX11 Evaluate、56 条 GPU 完成证据，都是匹配日志行数，不是全会话显示帧数。

## 5. 上一局基准及已撤回的归因

8240EE70／PID116868，session `20261108-130053_YuanShen_116868`：`13:00:50—13:03:37`，有正常退出记录。用户大世界两层 50% NR 约 25 FPS，目标 35；NR＋FG×6 慢且卡。原文 `build/genshin-116868-OptiScaler.log`，9,382,056 B／51,832 行，SHA256 `07B4B80CC99627EA6D6007D3DB6208740F009743D31AF883BA00340A2702A8ED`，解析 `build/genshin-116868-evidence.json`。

- 仅 NR 末段源 Present 间隔约 39～42 ms，本体约 0.1～0.24 ms；Native Run CPU 通常 1.5～1.9 ms，allocator wait 很小。仍缺同一输入的完整排他成本链。
- 54 条 child 调用多数整数 0 ms，后段 15～32 ms 及少量长尖峰；包含完整 hook 和 ReflexSleep。**此前“SDK 持续约 31 ms”说法已经撤回。**
- 47 次 SDK 插帧状态变化中，22 次在 eOn 请求下停用且邻近 flushAll，原因未知。F6 configuration 只有 3 条，后两条 submitted=false，配置缓存确有命中；不能说 F6 每帧重设 Off 导致 SDK 启停。
- 常量 52 条中 7 条 reset=1，其余为 0，均 result=0、gap_resume=false；未证实持续 reset 是根因。
- 更早用户确认 NR、FG 关闭后可恢复帧率，仅适用于当时版本。最新关闭 FG 冻结已经覆盖该验收结论，不能继续写“开关均正常”。

## 6. 源码接手入口与最近改动

| 位置 | 负责内容／下一步关注 |
|---|---|
| `OptiScaler/genshin/GenshinLegacyCapture.cpp`、`GenshinReturnedRaster.h` | 实际 Draw 观测、ticket／原调用返回配对和 Raster；同调用 DSV／深度状态验证、thread／epoch／数值证明 |
| `OptiScaler/runtime/Dx11RasterBindings.h` | context 绑定快照、保留 Context1、返回后的公共绑定比较；不能拿只读缓存命中代替重新验证 |
| `OptiScaler/genshin/GenshinFsrProfiles.h`、`GenshinFsrComposite.h`、`OptiScaler/inputs/GenshinFsrDx11.cpp` | 新旧 FSR 创建字节认证、真实 native RTV0／RTV1 alpha 与 RGB 合成；不可只放开哈希 |
| `OptiScaler/native/Dx11FrameSource.cpp`、`NativeDriver.cpp`、`NativeProducer.cpp` | source 获取、前处理、flow／guides、真实完成记录与 NR 消费 |
| `OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp`、`DlssNr_Late.inl` | 两层实际模型调用、历史／cadence、GPU query、模型输入映射及完成证据 |
| `OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp`、`DlssgOptionsPolicy.h`、`OptiScaler/hooks/FG_Hooks.cpp` | FG Dispatch／Deactivate／teardown、SDK 配置缓存、常量与 Reflex；最新冻结重点入口 |
| `OptiScaler/with_dx12/dx11_with_dx12_sc.cpp`、`GenshinVisibleChild.h` | `_PresentNativeVisible`、私有 copy、queue Wait、父／子窗口 owner、hide／reveal／drain／fallback |
| `OptiScaler/genshin/GenshinHookCpuTiming.h`、`OptiScaler/menu/menu_common.cpp` | 轻量 FSR HUD、重型实时采样开关、缓存窗口统计 |

10503898 本批主要变更：

1. 返回 ticket 仅更新轻量字段，前一完整快照绑定只清理一次；有效捕获直接填入唯一 Raster，复用同次已验证 DSV／深度状态和保留的 Context1。
2. `PrivateCopyOrdering::SubmittedWait`／`OrderPrivateCopyWait` 合并同私有事务、同 owner、同 queue／fence／value 的重复成功 Wait；其它身份仍实际提交。复核发现的异对象重入引用缺口已修正：通过重入门后才取得调用局部 COM pins，覆盖 Wait，再移交 scope。
3. `BeginDrawObservation` 清理未匹配 engine-return ticket，`AfterOriginalDraw` 仅对精确捕获 Draw 查询 engine token 并核对 serial／generation／instance／frame，普通场景 Draw 不再复制 receipt／查询。
4. NR 模型调用差值跨 owner／config 重置时记 unknown，避免无符号下溢；GPU 完成证据每 64 个 sequence 补低频详细样本，没有新增等待。

**关闭 FG 的冻结发生在上述改动后的实测；优先复核本批私有事务与既有 teardown／hide／drain 的交互，但尚无证据将根因归给 Wait 去重。** 源帧、SDK 和显示 owner 必须分开；资源只按实际所属 queue／query／fence 完成后退休，禁止删拒绝条件、扩大池子或借旧帧掩盖问题。

FSR HUD 已有单独一行：`FSR - HOOK 延迟: xxx ms/s  shader 缓存命中：xx%  绑定命中：xx%`，无查询显示 `--`，取最近完整窗口。历史 PID106132 末个窗口 metadata 97.2%、Current binding 100%，仅证明当时窗口有真实命中，不能当作最新局的缓存性能结论。

## 7. 构建、测试和部署入口

```powershell
Set-Location D:\CODE\DLSS5_NEXT-workers\f6-rewrite
pwsh -NoProfile -File ./build.ps1
pwsh -NoProfile -File ./tools/tests/Run-CommonCoreTests.ps1
```

默认 Release x64，最多 8 个编译工作进程，可用 `-Jobs N`／`-Clean`；MSBuild 在 `D:/VS2022BuildTools/MSBuild/Current/Bin/amd64/MSBuild.exe`。普通 DLL 构建用现有嵌入 shader 头，不需重生成 Look shader。产物、构建日志、控制台、测试日志分别为 `build/out/OptiScaler.dll`、`build/msbuild.log`、`build/current-build-console.log`、`build/common-core-tests.log`。

上一完整公共 runner UTC 05:30:10—05:33:36 exit0；返回事务43、捕获27、绑定133、输入观察1081、资源观察73、私有Wait46及既有回归通过。100,000 次 ticket 捕获／100,001 次返回无 Raster 构造／移动／复制；有效捕获一次构造、零移动／复制。静态独立复核最终未见具体新增语义缺陷；CPU／WARP 不执行真实游戏或 SDK 画面，均不能覆盖最新实机失败。本次仅文档交接，不重复跑产品测试。

现有回执 `build/current-{build,tests,artifact,deployment,performance-review}.json` 区分构建／离线验证／部署／用户验收；`build/active-genshin-session.json` 更新为最新已结束的 PID107608，上一局116868仍可从固定日志读取。`build/` 被 Git 忽略，换机器／清构建前须另行保留日志、JSON和回退 DLL，不能指望源码提交包含它们。

部署使用 `tools/runtime/Deploy-F6.ps1`，**本次未执行**。未来部署须先确认无活跃使用 DLL 的游戏，再用该脚本原位写入已有目标文件对象，保留四条硬链接；不要 rename／replace／删除后复制。链接为正式 F6 与 `cache/games/{hk4e_cn,hkrpg_bilibili,nap_cn}/OptiRuntime/2E415BFEBE7E11B2/OptiScaler.dll`。保留用户 profiles，仅一个 owner 构建／部署，不让多个任务同时编译。

## 8. 接手后的顺序

1. **先解决关闭 FG 冻结。** 以13:45:34的最后一次源输入、SDK提交、FG=false、Off＋retain、hide／drain和native fallback为一条事务核对，确认当前源画面是否持续交付、哪些锁／队列仍待完成。现有日志无后续，不可仅凭末行定位阻塞栈；如确需新证据，再与用户安排最小复现和采集，不先大范围加高频日志。
2. **再解释纯 NR 7→27及距离35的差距。** 保持两层50%等设置，对同一输入关联 producer、CPU hook、跨 API 传输、flow／guides、model和输出节拍，区分启动／场景迁移与稳态。缺用户操作时间时保留 unknown，不把“切画面”当作特定动作。
3. **单独追踪 FG 慢放。** 对齐8次 flushAll、17次 SDK状态变化、源输入资格、常量、Reflex和显示 owner，确认生成输出是否真正到达显示；不以 SDK ordinal、presented batch 或六倍请求当显示帧率。
4. **完成公共接口与跨场景回归。** 保留真实创建字节认证、RTV合成与映射约束，覆盖登录→世界→切场景；再测星铁联合功能与资源退休、ZZZ。没有实测通过前，不写“三游戏已修复”。

用户指定参考优先级：`AizawaHikaru233/genshin_fsr_brigde` 首选，其次 `CXP-2024/GIMI_reshade_dlss_integration`；仓库地址在 [AGENTS.md](../AGENTS.md)。Encore／sm86／F5／F5-DS 可对照，但路径和调查结论要按当前磁盘／源码重新核对；参考实现支持何种 FG 不能凭名称推断。不要在旧会话恢复子智能体来继续调查。
