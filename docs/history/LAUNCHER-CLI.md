# 启动器 CLI 与启动加载核对（V3-R042）

历史接口核对（2026-10-29），不是当前运行授权。R1077当前F6配置/CLI/退出边界见[HANDOFF](../HANDOFF.md)：正式profiles权威、runtime投影/退出合并；本文旧F5/cache身份、hash/整套备份流程不恢复为当前门禁或恢复授权。当前不运行游戏，后续仅Root沿已知CLI。

本文只记录启动器 CLI 的可复核接口和操作边界。读取源码不等于已安装版本的行为证明；没有同 PID 证据时，加载与功能状态均为 **UNKNOWN**。

## 1. 源码版本与范围

- 仓库：`D:\CODE\HoyoDLSS5`
- 分支 / HEAD：`main` / `66345e2a9c5c63a807926993e0dfc61683f7a408`
- 工作树当时为 dirty；`Program.cs` 有未提交修改。以下按实际读取文件计算，不能只报 HEAD。
- 读取日期：2026-10-29。

| 文件 | 行数 | SHA256 |
| --- | ---: | --- |
| `src/HoYoShadeHub/Program.cs` | 599 | `023CAD4CF648AE86630516FEDEA09F3EA01661EFB7CEBF0A39CE2ADC8A67415D` |
| `src/HoYoShadeHub/Features/Plugins/LauncherActionRunner.cs` | 1777 | `30532B8F2AB6AC7B05C39D47C026BACA49DF7C952ED6F95FD5F205FAC56C45B1` |
| `src/HoYoShadeHub/AppConfig.cs` | 2780 | `9F944A2C2DFCA6CEB8E106DD8F869F172E3CFC1201F0F7B123DD2DC92E347570` |
| `src/HoYoShadeHub/Features/GameLauncher/GameLaunchPipeline.cs` | 1099 | `9D19D193659A19622E3CA13ADC62E39E99C410A003DAF860022EF80CDC4AC0ED` |

补充只读核对了互锁实现：`src/HoYoShadeHub.Extensions/Games/OptiSmoothMotionPolicy.cs:9-37`、`src/HoYoShadeHub/Features/Plugins/OptiSmoothMotionInterlock.cs:28-45`。

## 2. `run --biz --file`

```powershell
$hub = 'D:\APPS\test\TEST\HoYoShadeHub_Portable_1.4.5.0_x64_Full\app-1.4.6.12\HoYoShadeHub.exe'
& $hub run --biz hk4e_cn --file 'D:\absolute\steps.json' 2>&1 |
    Tee-Object -FilePath 'D:\absolute\cli-output.log'
$cliExitCode = $LASTEXITCODE
```

- `run`、`auto`、`action` 进入同一个 CLI 分派；`--file` 也可用 `--json` 内联 JSON 代替：`Program.cs:152-155`、`Program.cs:334-355`。
- 必须提供可解析的 `--biz`；动作文件可写成 `{"steps":[...]}` 或 `{"actions":[...]}`：`Program.cs:371-383`。
- `launch_game`、`stop_game` 等进程动作从非管理员控制台调用时会请求 UAC，并用接力文件把子进程输出回传到当前控制台：`Program.cs:388-394`、`Program.cs:464-515`。
- 退出码：所有步骤摘要都不含 `✗` 时为 `0`，任一含 `✗` 时为 `1`：`Program.cs:440-456`。退出码只代表 CLI 步骤报告，不等于游戏功能验收。
- Windows GUI apphost 直接用 `&` 调用时可能提前返回，得到空输出和 `$LASTEXITCODE=$null`。上例用输出管线等待；仍须核实返回值与实际 relay/PID。空退出码必须记为未捕获，不能当作 `0`，也不能据此立即重复启动。

最小示例：

```json
{
  "steps": [
    { "action": "set_launch_option", "key": "useOptiScaler", "enabled": true },
    { "action": "launch_game" }
  ]
}
```

`run` 创建无 UI 上下文；依赖页面或弹窗的步骤可能跳过或失败：`Program.cs:426-438`。

## 3. `set_launch_option`：实际只有 6 个 key

`key` 必须为字符串，`enabled` 必须为布尔值，当前游戏由 `--biz` 决定：`LauncherActionRunner.cs:919-926`。

| CLI key（camelCase） | 实际实现 |
| --- | --- |
| `useXxmiInject` | `AppConfig.SetUseXxmiInjectLaunchOption` |
| `useHoYoShade` | `AppConfig.SetUseHoYoShadeLaunchOption` |
| `useOpenHoYoShade` | `AppConfig.SetUseOpenHoYoShadeLaunchOption` |
| `useFpsUnlock` | `AppConfig.SetUseFpsUnlockLaunchOption` |
| `useModules` | `AppConfig.SetUseModulesLaunchOption` |
| `useOptiScaler` | `AppConfig.SetUseOptiScalerLaunchOption` |

上表六个 `case` 位于 `LauncherActionRunner.cs:929-934`。注释还提到 `useStarward`、`genshinBlender`、`zzzBlender`，但 `switch` 没有这些 `case`，传给 CLI 会返回“未知启动选项”：`LauncherActionRunner.cs:915-940`。不要把注释中的名称写成已支持。

逐游戏开关的持久化 key 是 `launch_option_<option>_<gameBiz>_<gameId>`：`AppConfig.cs:2195-2198`。OptiScaler 读取默认 `false`，写入走 `SetValue`：`AppConfig.cs:2241-2252`；持久化为 `Setting` 表的 `INSERT OR REPLACE`：`AppConfig.cs:2706-2724`。请用 CLI，不要直接用 SQL 拼写内部 key。

`launch_game` 本身不会自动打开 OptiScaler；启动管线只在 `GetUseOptiScalerLaunchOption(gameId) == true` 时准备和注入：`GameLaunchPipeline.cs:86,96`、`LauncherActionRunner.cs:163,186-210`。

## 4. OptiScaler 与驱动 AI 插帧互锁

- 互锁只适用于 `hk4e`、`hkrpg`、`nap` 游戏族，并且必须 `optEnabled=true`：`OptiSmoothMotionPolicy.cs:9-13`。
- 开启 `useOptiScaler` 时，`AppConfig` 调用 `OptiSmoothMotionInterlock.OnOptiEnabled`：`AppConfig.cs:2246-2252`。CLI 的“设置成功”不等于驱动状态已确认关闭。
- 若目标 EXE 已有设置，读取为开时会写该 EXE 的 Smooth Motion 为关并回读；若 per-EXE 状态缺失但全局为开，会创建设置并关闭目标 EXE：`OptiSmoothMotionPolicy.cs:21-37`。策略意图是不改全局值，也不改 OptiScaler 自己的帧生成；实际驱动结果属于独立状态。
- 启动管线再次等待 `EnsureAsync`，驱动写入或回读失败会阻断启动：`GameLaunchPipeline.cs:96-98`、`OptiSmoothMotionPolicy.cs:31-36`。互锁串行门防止驱动操作并发，但 setter 的异步通知本身不是完成回执；启动前检查与实际回读结果仍须单独确认。
- **备份数据库不会备份 NVIDIA 驱动 per-EXE/全局状态。** 执行 owner 应在开启 Opti 前保留只读驱动查询结果，并把 before/after 写入轮次记录。本轮不新增驱动查询 CLI；执行 owner 已使用已证实的只读查询完成 per-EXE OFF 预检，具体预检入口不在本文重复定义。

## 5. 原神实际 NR 配置路径与保护范围

Root/执行 owner 已给出安装现场事实：原神独立 NR 实际选择

`...\cache\games\hk4e_cn\OptiRuntime\CCE7798EBDFD717A`

其中 `OptiScaler.dll` 同 Build04，`OptiScaler.ini` 与 `...\OptiScaler\official\DLSS F5` 下的 INI **不同**。这是已安装现场事实，不是从上述源码推出来的。

配置备份和恢复必须同时覆盖：

1. cache 的 `OptiScaler.ini`、`profiles\hk4e_cn.ini` 及现有 `.hub-runtime-owner` / `.hub-runtime-baseline` 等启动配置记账。
2. official 的对应 root/profile 与配置记账，保留两处原有差异。
3. Hub 数据库的一致性备份及逐游戏开关原值；SQLite WAL 活跃时不能仅复制主 DB 后宣称备份完整。
4. 两处 DLL 的身份与 hash。本轮不替换 DLL，复用已核验的 R041 rollback，避免重复复制同一产物。

不要把 official 目录当成原神当前唯一配置。应关联同一 PID 的 Hub 注入目标、profile 激活/缓存准备记录、运行日志与 session 配置快照。`LogFileName` 只决定日志位置；cache INI 可以把日志写到 official，不能据此反推读取了 official INI。`session.txt` 的 `mod` 行也可能来自磁盘候选收集，不能单独证明该模块已映射到游戏进程。

## 6. 启动加载证据

`launch_game` 在 CLI（无 `LauncherPage` / `XamlRoot`）中要求等到注入流程结束再返回，并把 PID 打到控制台：`LauncherActionRunner.cs:207-210`；等待语义见 `GameLaunchPipeline.cs:51-65`。注入稳定后启动会话日志采集：`GameLaunchPipeline.cs:952-983`。

应保存并按同一 PID 关联：

- CLI 输出的 PID 与本地启动时间；
- `log\sessions\<timestamp>_<exe>_<pid>\session.txt` 的 PID、配置与日志关联；出现 `mod OptiScaler` 后仍核查候选收集来源；
- 匹配 PID/时间的 `OptiScaler.log`，核对 `[Log] LogFileName` 指向 cache 还是 official；
- 实际 `OptiScaler.dll` 与 `OptiScaler.ini` 的 SHA256；
- 若模块枚举被拒绝（例如 `ERROR_ACCESS_DENIED (5)`），模块枚举和内存镜像 hash 保持 **UNKNOWN**。可用同次 Hub 注入成功和新 Opt 日志执行证据单独说明运行活动；禁止把磁盘候选表升级为已枚举的加载模块。

证据等级必须分开：源码修改、真实 TU 编译、完整构建、磁盘部署、同 PID 加载、同 PID 功能/视觉验收。本文只做 source/文档核对；未启动 Hub、未运行游戏，因此当前 installed/runtime 行为仍以既有现场记录为准。

## 7. `stopgame` 强停 fallback

- 无精确条件参数的 `stopgame` / `killgame` 入口执行 `stop_game`；带条件时有独立 `ExactStopCondition` / `ExactProcessStop` 分支：`Program.cs:173-195`。本轮未审查该分支为无强停保证，不能据名称声称安全正常退出。
- `stop_game` 先调用 `CloseMainWindow()`，最多轮询 **20 次 × 250 ms（约 5 秒）**；仍未退出就调用无附加条件的 `process.Kill()`，并返回“已强杀游戏”：`LauncherActionRunner.cs:411-447`。
- `close_game`、`kill_game`、`stopgame` 是同一分派别名：`LauncherActionRunner.cs:163-164`。
- 因此 `stopgame` 不能作为“正常关闭”、存档完成或游戏自行退出的证明。强杀后的日志截断、dump 和配置回写状态都要单独判定。本任务不执行任何停止或进程控制。

## 8. 配置备份 / 恢复边界

本次核对的四个 CLI 文件没有通用“数据库备份 / 恢复”动作；不要把 `stop_game` 或改动 Setting 当成回滚。手动保护至少包括：

- `HoYoShadeHubDatabase.db`；
- 第 5 节列出的 cache 与 official 两套配置及 profile/记账；DLL 未替换时引用现有有效 rollback；
- 涉及驱动互锁时的 NVIDIA per-EXE/全局状态快照。

备份到带时间戳的新目录，保持角色分目录，避免两份 `OptiScaler.ini` 同名覆盖；复制后核对 SHA256。SQLite 在线备份可改变页头字节，须区分逻辑一致性/完整性校验与原始字节 hash。覆盖恢复前必须由执行 owner 确认没有 Hub/游戏占用，并保留恢复前副本和原备份。数据库由进程缓存，恢复后应由新 Hub 进程读取。`GameLaunchPipeline.cs:952-983` 的退出回调由 Hub 把 root INI Store 回 profile，并非证明 Opt 只在退出时写 INI。活动游戏期间的配置变化应记录，不能覆盖潜在用户修改。

`GameLaunchPipeline.cs:451-485` 另有游戏目录 `dlssg` 替换的 `.hysx\game-dll-backup`；它只保护该 DLL 替换，**不保护** Hub 数据库、两套 OptiScaler 配置或驱动状态。

驱动状态如何回滚：**UNKNOWN**。本轮源码和已核对 CLI 没有给出通用驱动配置恢复命令；如现场已变，应由执行 owner 按已记录的原值单独审查、恢复并复核，不得声称恢复数据库即可恢复驱动。
