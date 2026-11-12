# F6 项目交接入口

更新：2026-11-12。**已按用户要求继续，最新交接见[产品报告第 0 节](../../DLSS5_NEXT-workers/f6-rewrite/docs/F6-HANDOFF.md)**，包含当前源码修复、测试证据、候选身份、未确定项和后续验收。

唯一写树 D:/CODE/DLSS5_NEXT-workers/f6-rewrite，codex/f6-rewrite，HEAD 9ab9686，大量既有未提交改动，禁止 reset/clean。本根 master 仅保留迁移基线、导航和共享研究。

**FG 用内置、不用 ASI。** F6 5265915D 与 FSR 桥 CC301B42 已按用户要求部署；正式及三游戏缓存 F6 DLL 磁盘 SHA 均为 5265915D，原文件身份／硬链接不变，旧安装 8FCB8632 已备份，2,535 项受保护文件未变。部署前用户反馈崩铁 OK、ZZZ 未检测 DLSS；旧 PID180352 被归 backend，不能替代新版本实测。新版本冻结清单遗漏 FgDepthContract.h，验证边界保留。

构建测试证据在产品树 build/repair-20261112/verification-next/；本次部署核验见 build/f6-deploy-5265915D-verified.json 和 current-deployment.json，旧候选及回执保留。实际加载、画面和性能待分别验收，游戏由用户手动启动。参考[仓库清单](REFERENCE-REPOSITORIES.md)与[当前状态](CURRENT-STATE.md)。
