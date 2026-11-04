# F6 功能推进状态与重启交接

**已按用户请求暂停，等待以管理员权限重启 DSH。** 当前长期目标处于 paused；收到用户“继续”后再恢复编码、构建和运行。不要因打开本页自动启动智能体或游戏。

## 当前目标与轮次

- 目标轮次：**1 / 256**；每 10 轮检查实际代码交付和运行效果，下次整体重评在第 **10** 轮。
- 主优先级：原神、星铁、ZZZ 的 NR/FG 真实深度、相邻历史复用、细节与连续性。
- 星铁保留用户要求的 adamfg/F5-DS 独立 FG 路线，避免额外同步、池重建和复杂跨域认证拖垮帧时间。
- 次优先：GIMI 完整 MOD 消费与 BeforeSR 效果进入实际 selected SR。
- 普通搜查/文档/独立复核使用 DS；核心设计与实现使用 `gpt-6.1-sol`。

## 已停止的任务与保留代码

编排任务 `workflow-5ce62c01deb54c24` 已请求取消，回执为 **killed**；原编排不能当作已完成交件，独立 DS 复核也未收齐。四个私有 worktree 都从主树 `7572f42` 建立，保留在 `D:/CODE/DLSS5_NEXT-workers/`。没有合并回主树。

| 任务 | 分支 / 子目录 | 暂停时保存点 | 准确状态 |
|---|---|---|---|
| task-10 | f6/nr-continuity / nr-continuity | 7572f42 | 尚无源码改动；R1075 两文件候选仍在旧根，尚未集成 |
| task-11 | f6/zzz-source-clock / zzz-source-clock | **1a0a201** | 8 文件、+186/-52 行；Lead 将未完成修改保存为 WIP，未完成复核/构建/运行 |
| task-12 | f6/genshin-output / genshin-output | **b77a9c4** | 6 文件、+117/-11 行；Lead 将未完成修改保存为 WIP，未完成复核/构建/运行 |
| task-13 | f6/mod-filter / mod-filter | **bb2e0ea** | 编码者已提交 GIMI 两文件 +30/-5 行；尚未集成/复核/构建/运行；BeforeSR 未取得新交件 |

ZZZ 与原神候选都涉及 `DLSSG_Dx12.cpp/.h` 和 `FG_Hooks.cpp`。恢复后要逐段审功能边界并解决冲突，不能整文件覆盖或把 WIP 当已修版本。模型会话已中止，恢复时应以这些磁盘分支重新派工，不假设旧子会话还在运行。

## 本轮已完成的准备

- task-9（DS）只读核对现有 launcher、launch-only、精确 stop、session 与 hardlink 部署接口。
- task-14（DS）只读核对星铁 FG 热路径与 C4744 布局警告。Present 记账路径未发现固定每帧额外 GPU Wait/Flush/全量重建；UI allocator 忙时的背压等待与间隙 Off/On 频率仍需实际测量。C4744 根因未确认，未据猜测改同步代码。
- task-15（DS）迁入 [精确入场 helper](../tools/runtime/README.md)：Biz 参数化、当前安装根 launcher、60 秒有界等待与超时 UNKNOWN。保留 PID/creation/image/HWND 和单次输入语义。
- helper 的 PowerShell Parser/参数核对通过；Lead 用 Add-Type 实际编译其 C# 依赖成功，只加载类型，没有调用窗口/输入方法。
- 当前会话管理员身份实测 **False**。用户因此要求暂停并重启 DSH；本轮未尝试绕过权限或触发 UAC。

## 构建与运行基线

- 最近验证的主树产品代码来自 `8a34d8c`，DLL 为 **34,795,008 bytes**，版本 `0.1.29-final (8a34d8c)`；清缓存构建 exit 0，之后增量构建也 exit 0。见 [迁移基线](CURRENT-STATE.md)。
- 本波没有集成新功能、没有构建上述候选、没有部署、没有启动/激活/停止游戏、没有修改 profiles。
- 官方 DLL 仍是旧正式件 **34,796,544 bytes**，原有四个 hardlink（正式位与三个 runtime）保持。以后部署只就地写原对象，不 rename/Copy 替换；旧根和回退来源继续保留。
- 旧 USER_FAIL/未验收继续有效；新代码提交、API dispatch、模型参数、GPU 完成、最终像素和净性能分别核证。

## 管理员重启后的接续顺序

1. 打开同一会话，工作目录保持 `D:/CODE/DLSS5_NEXT`；收到用户继续指令后恢复 goal，并确认新 DSH 确实以管理员运行。
2. 读取四分支实际 log/status/diff，从保存点继续 Sol 核心任务，DS 独立复核；保留本页记载的三游戏隔离和星铁 FG 边界。优先闭合 NR/R1075 与 ZZZ/原神候选，不重新做迁移或清空工作区。
3. Lead 集成可证的完整交件、统一构建。不要把仅完成 GIMI 的私有提交冒充 BeforeSR 或三游戏验收。
4. 运行采用现有 launch-only `run --biz <biz> --file <json>`，步骤 `{"steps":[{"action":"launch_game"}]}`。从真实 stdout 取得 PID，再从该精确进程取创建 FILETIME；不要按名字盲绑。
5. 收尾用 `stopgame --biz <biz> --expected-pid <pid> --expected-creation-filetime <filetime>`；不使用 testgame 的 legacy stop 冒精确证据。精确 helper 与 launcher 既有权限通道复用，不新造输入路径或改配置预设。

现有 capture_game CLI 仅支持 hk4e_cn 单步骤调用；缺少 session 退出记录只说明收尾未知，不能推断自然退出/强杀或功能成功。只维护本页当前事实与必要交件，不恢复逐轮堆积大型证据树。
