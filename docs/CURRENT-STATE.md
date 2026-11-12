# DLSS5_NEXT 当前状态

更新：2026-11-12，**普通内置 FG 修复版 5265915D 与 FSR 桥 CC301B42 已按用户要求部署，待新局实测**。本根只做导航，最新事实见[完整交接第 0 节](../../DLSS5_NEXT-workers/f6-rewrite/docs/F6-HANDOFF.md)和[产品当前状态](../../DLSS5_NEXT-workers/f6-rewrite/docs/CURRENT-STATE.md)。

| 对象 | 当前事实 |
|---|---|
| 产品树 | D:/CODE/DLSS5_NEXT-workers/f6-rewrite，唯一 F6 写树，codex/f6-rewrite，HEAD 9ab9686；大量既有未提交内容，禁止 reset/clean |
| 本根 | D:/CODE/DLSS5_NEXT，master 迁移基线、导航与共享研究，未合并最新 F6，本根构建不能当最新产品 |
| 最新反馈 | 崩铁 OK，ZZZ 仍未检测 DLSS；FG 用内置，不用 ASI。ZZZ 旧局确有 MFGUnlocker.asi 调用；崩铁反馈不代表新候选内置 FG、精确显示率或排队测量已通过 |
| 源码状态 | 设备身份路由、NGX 生命周期和星铁锁外 pacing 已集成，最终定向回归及独立静态复核完成 |
| 当前版本 | 普通内置 FG 5265915D 已构建并部署；current-artifact/build/tests/deployment 对应新版本，旧候选及安装回执保留 |
| F6 安装身份 | 正式及三个游戏缓存 DLL 的磁盘 SHA 均为 5265915D，四路径原文件身份／硬链接不变；旧安装 8FCB8632 已备份，2,535 项保护文件不变，进程内字节未知 |
| 未决 | ZZZ 旧 PID180352 有 DLSS Create／Evaluate 但均为 backend；5265915D 已部署，ZZZ 接入、内置 FG 与性能待新局实测，旧局具体来源拒绝未知 |
| FSR 桥 | 用户授权更新，上游 1865c585 已获取／构建并部署实际选中桥 temp3/Dx11FsrBridge (2).dll，SHA CC301B42；旧桥／INI 已备份，配置和 F6 DLL 不变，游戏效果待验；[审计](../../DLSS5_NEXT-workers/fsr-bridge-upstream-1865c585/AUDIT.md) |

最终证据见产品树 `build/repair-20261112/verification-next/`：1,245 项声明输入指纹、17 组定向检查及独立静态复核，构建后核验 53 必要 NGX 导出及全部 948 个导出名称／序号。清单遗漏 FgDepthContract.h，不能单凭清单认证其构建时字节；测试不证明 SDK、游戏或性能通过。

磁盘部署核验已完成，游戏由用户手动启动，各项绑定新 PID／加载身份／当局日志。后续源码改动需重新冻结输入并重跑受影响测试，不覆盖旧证据；实际部署回执与构建回执分别维护。

保留真实 queue/fence/Reset、独立 SR／NR／FG、用户参数和 profiles；不扩大池子、借旧帧或删拒绝条件。历史 7C281A54 主链改善有用户反馈，其它逐项验。构建、部署、实际加载、画面和性能分别验收。

旧根 D:/DATA/Documents/ChatGPT/DLSS F5 NEXT、其它工作树、build/ 候选／回退／证据和许可证保留，build/ 不能整目录当临时缓存删除。来源见[仓库清单](REFERENCE-REPOSITORIES.md)，共享研究见[研究入口](../tools/research/encore-f7/README.md)，历史迁移审计见 docs/AUDIT/，不能冒充当前验收。
