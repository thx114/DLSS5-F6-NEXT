# 项目协作规则

本仓库当前工作根为 `D:/CODE/DLSS5_NEXT`。先读 [README](README.md) 和 [当前状态](docs/CURRENT-STATE.md)；`docs/history/` 是旧根交接资料，其中的路径和旧构建部署流程不能直接套用。

## 目录与构建

- `OptiScaler/` 是产品源码；`external/` 是真实依赖目录，两者保持同级。
- 构建入口：`pwsh -NoProfile -File ./build.ps1`。默认 Release x64，自动使用最多 8 个编译工作进程；可传 `-Jobs N` 或 `-Clean`。
- 产物和 UTF-8 日志在 `build/out/`、`build/msbuild.log`；生成版本头由工程 PreBuildEvent 写入并被 Git 忽略。
- 主许可证在 `LICENSE`，第三方署名在 `Licenses/` 和各依赖目录中，整理时一并保留。
- Look shader 的既有生成 recipe 在 `tools/shaders/`；普通 DLL 构建直接使用现有嵌入头，不需要重新生成 shader。

## 分工

- 普通审计、搜索、文档和机械整理使用 DS；核心代码设计可调用 `gpt-6.1-sol`，遵循用户当次指定。
- 先给子智能体划定互不冲突的文件范围和依赖关系。执行者改动，另一智能体复核，Lead 审最终 diff 并统一构建。
- 不让多个任务同时编译或修改同一文件；交件说明实际改动、验证结果和未确定项。
- 以实际引用和使用链路判断冗余；没有列入主工程的头文件、测试输入、离线 shader、独立子工程不自动等于死代码。
- 只维护简短的当前说明和必要变更记录，避免恢复旧项目逐轮堆叠证据目录的做法。

## 产品边界

- 本轮完成的是迁移、整理和构建，三游戏 NR/FG 的运行问题仍未验收通过。
- 资源退休仍须由实际所属 queue/fence 完成证明；不以 CPU 返回、Ready、窗口 ACK 或其它对象的 fence 替代。
- 原神、星铁、ZZZ 的 source/SDK/frame 身份和显示 owner 分开；不能靠删拒绝条件、借用旧帧或扩大池子冒修复。
- 保留用户配置和独立 SR/NR/FG 语义。部署、游戏运行与画面验证按当前任务另行安排；构建成功本身不证明功能正确。
- 旧根 `D:/DATA/Documents/ChatGPT/DLSS F5 NEXT` 作为历史和回退来源保留；本轮不删除旧根、不改正式部署或游戏 profiles。
