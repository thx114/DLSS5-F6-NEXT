# tools/runtime — 精确游戏入场 helper（已迁移，未运行）

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
