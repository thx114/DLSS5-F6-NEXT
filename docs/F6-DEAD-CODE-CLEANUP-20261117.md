# F6 第一批死代码精简

日期：2026-11-17。范围为已确认无调用／无写入的旧接口，以及 scanner 的重复实现。

## 协作与范围

已通知 F6 主会话，并只读核对 MI-6.2 Astra 当前的 S0／S1 语言层拆分及生产 compiler／VM 回归任务。此批只修改以下五个生产文件；主会话的监控、FG 状态、Reflex、英文，以及 TAA／HDR、MI／GIMI 均由各自会话继续。

| 文件 | 修改 |
|---|---|
| `OptiScaler/proxies/Streamline_Proxy.h` | 移除三个旧手动插件 loader、无人调用的 DX11 初始化／getter／标志、专用函数指针及 typedef。 |
| `OptiScaler/resource_tracking/ResTrack_dx12.cpp` | 移除无调用的 CPU／GPU handle 转换、CBV heap 查询及专属 TLS 缓存；删除恒不可达的 `_presentDone == false` 分支。 |
| `OptiScaler/resource_tracking/ResTrack_dx12.h` | 移除上述声明、无人调用的 GPU handle 写入／清除 API、未使用字段和 HUDless 私有状态。 |
| `OptiScaler/scanner/scanner.cpp` | 名称重载解析模块后转调原有 HMODULE 重载，消除重复扫描；移除无调用的相对指令地址 helper。 |
| `OptiScaler/scanner/scanner.h` | 移除上述 helper 声明。 |

五文件增加 1 行、删除 346 行，净减少 345 行。修改前五文件均无已有 Git diff，且与 EE231043 保存点逐文件一致。

## 保留的活动责任

- Streamline 的 `LoadStreamline`、`HookStreamline`、`SetD3DDeviceAndBind`、`InitWithD3D12` 函数体保持原样；DLSSG／Reflex／PCL 继续通过当前 interposer 绑定。
- 描述符 CPU 写入、按 index 批量复制、GPU handle 读取、heap 查找／失效及锁保持。未删除有效的 HUDless 准入检查、资源销毁跟踪或实际 queue／fence 退休。
- scanner 的 HMODULE 扫描实现保持原样；名称入口原样转交 pattern、offset 和 startAddress。
- DX11、DX12、DX11-w-DX12、Vulkan 原生及 Vulkan-w-DX12 的编译项保持。

## 独立构建与验证

以已交付 EE231043 的 `build/taa-fg-hdr-20261117/source-final/manifest.json` 为基线，复制其 1912 项源文件及 37 项静态库，再仅覆盖上述五文件。没有取共享树的其他并行 WIP，也没有更新共享 `build/out`。

- 隔离构建：`build/dead-code-cleanup-20261117/isolated/build.ps1 -Jobs 8`，Release x64，MSBuild exit 0，用时 2 分 11 秒。编译器／链接器警告仍存在，不报告零警告。
- DLL：`build/dead-code-cleanup-20261117/isolated/build/out/OptiScaler.dll`，33,230,848 B，SHA256 `5EBDD2CC49FD685702C0D435615E039567F90A939897230C4EB34A765CE4ACE5`。
- 四套公共定向回归通过：`DlssgOptionsPolicy`（9204 检查）、`DlssgPacingLifetime`（21 场景／323 检查）、`FgColourJoin`、`MethodHooks`。完整 shared regression 显式跳过。
- `framegen/dlssg/tests/GuideFgWiringTests.py` 通过；它是静态接线检查，不执行 SDK 或游戏。
- 构建后核对全部 1949 项输入及共享树的五个负责文件，零漂移；五文件 `git diff --check` 通过。

证据目录为 `build/dead-code-cleanup-20261117/`：`before/` 与 `before-manifest.json` 保存修改前五文件；`cleanup.patch` 保存精确源码差异；`source-manifest.json` 固定隔离输入；`verification.json` 汇总构建身份、输入核对及测试范围。构建／公共回归／接线日志分别为 `build-console.log`、`common-core-console.log`、`guide-fg-wiring.log`。

## 交付边界

本批仅完成源码精简与独立编译／定向验证，未部署、未操作游戏或用户配置。DLL 大小与 EE231043 相同；死代码可能已被链接优化移除，不能把源码行数减少换算为运行性能收益。没有新增游戏加载、画面、延迟或性能验收。

此 DLL 只用于本批验证；后续产品合版由主会话把这五文件改动纳入其最新源码及测试，不以它覆盖后续 TAA／HDR／MI 或监控成果。
