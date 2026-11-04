# 文档索引

## 当前入口

- [CURRENT-STATE.md](CURRENT-STATE.md)：实际整理结果、构建验证、旧根位置与后续问题。
- [README](../README.md)：项目目录与构建命令。
- [AGENTS](../AGENTS.md)：当前协作规则，DS/sol 分工及产品边界。
- [构建入口](../build.ps1)：Release x64；输出和日志收在 `build/`。
- [Look shader 工具说明](../tools/shaders/README.md)：从既有 recipe 迁入后的调用方法和验证范围。
- [许可与署名](../Licenses/README.md)、[主许可证](../LICENSE)。
- [CLI 导航](../CLI.md)：启动器历史接口说明，不代表本轮已运行游戏。

## 本次整理记录

初始审计是删除前快照，执行记录说明最终取舍；不能用初始“候选”清单代替实际删除结果。

| 记录 | 用途 |
|---|---|
| [源码初始清单](AUDIT/SOURCE-INVENTORY.md) | 工程引用、顶层规模与候选冗余 |
| [依赖初始审计](AUDIT/EXTERNAL.md) | 依赖引用与体积分布 |
| [构建配置审计](AUDIT/BUILD-CONFIG.md) | 工程条目、旧 PostBuild 和构建入口问题 |
| [源码精简执行](AUDIT/PRUNE-SOURCE.md) | 实际删除、保留测试输入的理由及未确定项 |
| [依赖精简执行](AUDIT/PRUNE-EXTERNAL.md) | 实际裁剪、许可保留、受保护依赖核对 |

## 旧根历史文档

以下十一份从旧根保留原文。旧根是 `D:/DATA/Documents/ChatGPT/DLSS F5 NEXT`，历史运行证据仍在那里；其中相对路径需按旧布局解读。技术事实仍可参考，但旧状态、旧批准和旧部署流程不自动代表当前状态。

| 文档 | 内容 |
|---|---|
| [HANDOFF](history/HANDOFF.md) | R1077 交接、正式版、三游戏未修问题与私有候选 |
| [旧当前状态](history/CURRENT-STATE-GOALS-AGENTS.md) | 旧根在 2026-11-04 收口时的状态 |
| [旧代理规则](history/AGENTS.md) | 旧协调工作区职责和资源边界；新根规则见根 AGENTS |
| [F6 技术路径](history/F6-TECHNICAL-PATHS.md) | 源码链路、consumer、坐标、fence、历史消费导航 |
| [旧构建环境](history/BUILD-ENVIRONMENT.md) | 旧 junction、增量缓存和构建部署入口 |
| [回归历史](history/REGRESSION-HISTORY.md) | 失败签名与旧轮次证据导航 |
| [Bridge 源码参考](history/BRIDGE-SOURCE-REFERENCE.md) | 原神桥的上游与源码关系 |
| [启动器 CLI 历史手册](history/LAUNCHER-CLI.md) | 旧接口核对；不是本轮运行回执 |
| [Bridge 跟版](history/GENSHIN-FSR-BRIDGE-UPSTREAM-UPDATE.md) | 旧维护流程，关联脚本留在旧根 |
| [OPTDRN1 v1](history/OPTDRN1-v1.md) | 日志格式与旧解码工具说明 |
| [低工作区准备](history/LOW-WORKSPACE-PREPARATION.md) | 旧工作区准备脚本说明 |
