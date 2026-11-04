# DLSS5_NEXT

OptiScaler 分支的产品源码（F6 线），构建产物为 `OptiScaler.dll`（Windows x64，C++/MSVC）。

本根只保留**当前源码、外部依赖、构建入口和文档**；旧根的 audit / worktrees / work / _evidence 等历史目录不迁移。

## 目录

| 路径 | 内容 |
|---|---|
| `OptiScaler/` | 产品源码，工程文件 `OptiScaler/OptiScaler.vcxproj` |
| `external/` | 第三方依赖（含预编译 `.lib` / `.dll`，随库保留，不要删除） |
| `build.ps1` | 唯一构建入口 |
| `build/` | 生成物：`obj/`、`out/`、`temp/`、`msbuild.log`（可整目录删除重建） |
| `docs/` | 文档；入口见 `docs/INDEX.md` |
| `CLI.md` | 启动器 CLI 要点 |

## 构建

```powershell
pwsh -NoProfile -File .\build.ps1            # 增量构建
pwsh -NoProfile -File .\build.ps1 -Clean     # 先删 obj/out/temp 再构建
pwsh -NoProfile -File .\build.ps1 -Jobs 8    # 并行编译单元
```

前置条件：

- PowerShell 7+
- MSBuild：`D:\VS2022BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe`
- MSVC x64 工具链（由 MSBuild 所在安装提供）

脚本内部把 `SolutionDir` 指向本根，因此 `OptiScaler/` 与 `external/` 必须保持同级并列；工程按 `$(SolutionDir)external\...` 引用依赖。构建失败返回非 0 退出码，完整输出在 `build\msbuild.log`。

## 产物

`build\out\OptiScaler.dll`（Release x64）。脚本在 MSBuild 成功后校验该文件存在，并打印大小与 UTC 时间戳。

## 文档

`docs/INDEX.md` 是文档索引。除该索引外，`docs/` 下的内容均为**历史交接文档**：文中路径指向旧根 `D:/DATA/Documents/ChatGPT/DLSS F5 NEXT`，仅作参考，不构成当前构建/部署说明；当前构建入口以本根 `build.ps1` 为准。
