# DLSS5_NEXT

OptiScaler F6 分支，Windows x64 / C++ / MSVC。当前源码、构建和依赖已迁入本仓库；旧工作区和逐轮运行证据留在原位置。

## 目录

| 路径 | 内容 |
|---|---|
| [OptiScaler/](OptiScaler/) | 当前产品源码与 MSBuild 工程 |
| [external/](external/) | 精简后的第三方头文件、库和许可说明，与产品源码保持同级 |
| [build.ps1](build.ps1) | Release x64 构建入口 |
| `build/` | 生成的 obj/out/temp、UTF-8 日志；Git 忽略 |
| [tools/shaders/](tools/shaders/) | Look shader 的既有再生成 recipe；普通 DLL 构建无需运行 |
| [docs/](docs/) | 当前说明、整理记录和旧文档存档 |
| [LICENSE](LICENSE)、[Licenses/](Licenses/) | 主许可证与第三方署名 |
| [AGENTS.md](AGENTS.md) | 协作与模型分工；[CLI.md](CLI.md)为启动器历史接口导航 |

## 构建

需要 PowerShell 7 和 MSVC v143；脚本使用本机 MSBuild：`D:/VS2022BuildTools/MSBuild/Current/Bin/amd64/MSBuild.exe`。

```powershell
pwsh -NoProfile -File ./build.ps1                   # 增量构建，默认最多8个编译工作进程
pwsh -NoProfile -File ./build.ps1 -Clean -Jobs 12   # 清缓存后全量构建
```

产物：[build/out/OptiScaler.dll](build/out/OptiScaler.dll)。完整 UTF-8 日志：[build/msbuild.log](build/msbuild.log)。失败返回 MSBuild 的真实退出码。

精简后已完成一次清缓存的真实编译和链接，`exit 0`。这验证源码与构建依赖闭合；游戏运行、NR/FG 画面和性能仍需分别验证。此入口只构建 DLL，未组装完整安装包或自动部署。

## 文档

先读 [当前状态](docs/CURRENT-STATE.md)；完整索引见 [docs/INDEX.md](docs/INDEX.md)。[docs/history/](docs/history/)保存十一份旧根交接文档，文中旧路径和部署结论按历史上下文理解。后续维护以简短的当前说明和必要变更记录为主。
