# tools/runtime

`deploy_f6_asi.py` 根据固定 JSON plan 新建独立的 Hub 包与各游戏 runtime，并通过 `set_opt_build` 选择。依次执行 `prepare --plan <文件>`、`select --plan <文件>`；已有目录会拒绝再次 prepare。`verify --plan <文件>` 只核对磁盘选择、原文件保护清单、各游戏配置和 NR 绑定，并更新独立回执。plan 可固定各游戏的 `NrSHA256`；最近部署的 plan 记录在 `build/current-asi-deployment.json` 的 `Plan` 字段，旧记录仍需满足下述兼容要求。三游戏分别使用自己的 profile 与 NR，不把共享包默认配置当成游戏有效配置。CLI 写数据库后，已打开的 Hub 需要从托盘完整退出并重启，才能刷新构建选择缓存。

通用 ASI 插件由用户自行安装。本工具不下载、打包或复制 ASI，也不复制载荷根下的 `plugins/` 目录。plan 必须通过顶层 `UserPluginsPath` 或 `Games[]` 内同名字段（游戏字段优先）明确引用现存的绝对目录；目录必须与待生成的包及 runtimes 分离。工具将其原样投影到该游戏的 `[Plugins] Path`，保持 `LoadAsiPlugins=true`、`External=false` 和各游戏其余 SR／NR／FG 参数，不更改、加载或验证用户插件内容。示例 plan 字段：

```json
{
  "UserPluginsPath": "D:/UserPlugins/F6",
  "Games": [
    {"Biz": "hk4e_cn", "UserPluginsPath": "D:/UserPlugins/Genshin"}
  ]
}
```

示例只展示新增路径字段，其余 artifact、源码／测试身份、profile、NR 与保护清单仍须完整提供。`prepare` 会跳过载荷中任何 `.asi`；`verify` 遇到包或 runtime 内残留的 `.asi` 会拒绝认证，不会删除或迁移它们。没有 `UserPluginsPath` 的旧计划会在任何写入或 CLI 调用前明确报错，需要另建计划及 BuildId；历史计划与回执保留。原有 BuildId、EvidencePrefix、NR 固定身份和独立回执文件名继续支持，旧的插件哈希字段不再用于认证，新回执改记每游戏 `UserPluginsPath`、`UserPluginsCopied=false`、`UserPluginsExecutionVerified=false` 及顶层 `UserPluginsDistributed=false`。磁盘核验不代表插件执行、游戏加载、画面或性能验收。

部署脚本离线回归：`python -B tools/tests/test_f6_user_plugins_deployment.py`。仅在临时目录构造载荷、数据库和各游戏配置，实际执行 `prepare`／`verify`，不调用真实启动器或游戏，也不写安装目录。

已有隔离包更新 DLL 时使用 `Deploy-F6.ps1 -BuildId local/F6-ASI -Artifact <已核验DLL>`。默认 BuildId 仍是 `official/F6`；只允许该正式身份或 `local/单目录名`。该操作沿用原位写入、备份和占用时 `.next` 暂存，不改变 profile 或选择。新候选、隔离部署与共享正式部署分别记回执，不因生成 `.next` 而声称已部署。

菜单输入回归：`python OptiScaler/menu/tests/test_menu_input.py`。它运行仓库中的 ImGui 及抽取的生产后端／输入函数，验证鼠标、键盘、手柄、开关菜单与输入屏蔽；不访问游戏。新 DLL 每次打开菜单最多记录 48 条 `F6 menu input`，包括采集／消费坐标、按钮、导航与事件队列，不记录输入文本。该证据用于排查菜单冷开点击失效，不能凭离线测试宣称游戏内已修复。

`Deploy-F6.ps1` 将已构建 DLL 流写到正式文件原对象，保留 runtime hardlink，并备份原字节；构建、部署和实际游戏验收分别记录。

部署不查询游戏进程，只尝试独占打开目标DLL。目标被实际占用时，将候选写至正式DLL旁的 `OptiScaler.dll.next`，校验完整字节后发布，返回 `STAGED_NEXT_FILE_IN_USE`、`PendingArtifact`、SHA256及 `InstalledChanged=false`。这是待替换，不更新正式部署回执；权限、磁盘等其它错误仍失败。占用解除后，用候选或 `.next` 作为 `-Artifact` 再执行同一脚本原位部署，保持正式文件硬链接；不自动重命名替换。部署回归：`pwsh -NoProfile -File ./tools/tests/Test-F6Deployment.ps1`，仅操作独立夹具，不修改正式安装。

`Get-F6RunEvidence.ps1` 从已关闭或保存的日志提取版本、PID、DX11/DX12 输入日志、NR cadence、FG 深度标签、ZZZ 入口归属/SDK source、原神 SDK 输入/物理输出/owner 显示及 GPU 完成字段。它拒绝版本/PID 不符及读取期间变化的文件，输出保留原行号和最后若干样本。计数均为可能限频的日志行数，不是总调用数或成功数；owner shown 也不证明生成帧像素。Streamline Evaluate 不证明 F6 输入，queue-ordered history 不等于 GPU 完成，interval 不可相加。画面与性能验收另行进行。

`-Latency` 将每条有效 Reflex 报告拆为报告开始至队列入口 `PreQueueReportMs`、GPU 前队列、GPU 区间、GPU 结束至报告末尾四段；四段重建同一报告总长。按 PID、frame 和六个端点去重，`DuplicateReflexRows` 保留重复计数。启停边界与稳态应分开看，报告总长不能直接称作 GPU 排队。解析回归：`pwsh -NoProfile -File ./tools/tests/Test-F6LatencyEvidence.ps1`。

```powershell
pwsh -NoProfile -File ./tools/runtime/Get-F6RunEvidence.ps1 `
  -Log ./build/starrail-27252-OptiScaler.log -ExpectedPid 27252 -ExpectedCommit 0613134 `
  -Output ./build/starrail-27252-evidence.json
```

## 精确入场 helper 的迁移记录与限制

从启动器仓库机械迁移的两个文件，**只做参数与语法验证，未执行**：

| 文件 | 来源 | 大小 |
|---|---|---|
| `Invoke-ExactGameEntry.ps1` | `D:/CODE/HoyoDLSS5/scripts/Invoke-ExactGameEntry.ps1` | 11,118 → 12,156 B |
| `ExactGameWindowInspection.cs` | `D:/CODE/HoyoDLSS5/scripts/ExactGameWindowInspection.cs` | 5,637 B（逐字节未改） |

启动器本体、`F5GameTest` 旧大模块、配置与 profile 均未改动或搬运。

**当前已知能力限制**：launcher 源码的 `ExactGameActivationRequest.ValidateCliBatch` 和 `ExactGameClickRequest.ValidateCliBatch` 均只接受 `hk4e_cn`。脚本接受三个 Biz 只是参数化转发，不证明启动器已支持星铁/ZZZ入场；后两者当前会在 CLI preflight 被拒。未部署修改过的 launcher，不把这个 helper 称作三游戏已可用入口。

## 相对原件的改动（仅 4 处）

1. `-Biz` 改为**必填** `ValidateSet('hk4e_cn','hkrpg_bilibili','nap_cn')`，替换 CLI 调用里硬编码的 `--biz hk4e_cn`。
2. `-LauncherPath` 缺省改为现存安装根 `D:\APPS\test\TEST\HoYoShadeHub_Portable_1.4.5.0_x64_Full\HoYoShadeHub.exe`（原缺省指向已不存在的 `app-1.4.6.12\`）；显式传参仍可覆盖。
3. `child.WaitForExit()` 无界等待 → `WaitForExit($CliTimeoutSeconds * 1000)`，新增 `-CliTimeoutSeconds`（`ValidateRange(1,120)`，缺省 60）。
4. 超时分支：写 `result.json`（`waitOutcome = UNKNOWN_TIMEOUT`、`exitCode = null`）后直接抛错 —— **不读 `ExitCode`、不重试、不 kill 子 CLI、不终止游戏**；只有确认已退出才读取 `ExitCode`。

精准绑定语义原样保留：`ExpectedPid` + `ExpectedCreationFileTime`（FILETIME）+ `MainModule.FileName` 镜像比对 + `ExpectedWindow`/`ResolveWindow` 唯一候选 + 前台/可见/最小化检查 + 单次 `activate_game` / 单次 `click_game`，失败即停不重试。

## 用法

```powershell
# 已在提升权限的 PowerShell 会话中运行
pwsh -NoProfile -File .\tools\runtime\Invoke-ExactGameEntry.ps1 `
  -ExpectedPid <pid> -ExpectedCreationFileTime <filetime> -Biz hk4e_cn [-ResolveWindow]
```

## 验证边界（本次实际做了什么）

- PowerShell `Parser::ParseFile`：**无语法错误**（1504 tokens）。
- 参数元数据核对：12 个参数，`Biz` 为 `Mandatory` + `ValidateSet`，`CliTimeoutSeconds` 带 `ValidateRange(1,120)` 缺省 60，`LauncherPath` 缺省为现存 exe，`ExpectedPid`/`ExpectedCreationFileTime` 仍必填。
- C# 成员核对通过；Lead 另用 `Add-Type -LiteralPath ./tools/runtime/ExactGameWindowInspection.cs` 实际编译成功，两个公开类型均可加载，没有调用窗口查询或输入方法。
- **未运行**：未执行 helper、未启动启动器 CLI、未启动/关闭游戏、未部署、未写 profile。因此点击/入场结果仍为 UNKNOWN。

## 注意

- 脚本首行要求**已提升权限**的会话，且 `LauncherPath` 必须是存在的绝对 exe 路径，否则在动作前就 throw。
- 记录目录固定写到 `%TEMP%\hys-exact-entry-<guid>\`（原逻辑，未改）。
