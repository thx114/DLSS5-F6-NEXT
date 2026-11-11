# DLSS5_NEXT 当前状态

更新：2026-11-12（UTC+08）。本页是导航摘要，完整事实见[产品交接](../../DLSS5_NEXT-workers/f6-rewrite/docs/F6-HANDOFF.md)及[产品当前状态](../../DLSS5_NEXT-workers/f6-rewrite/docs/CURRENT-STATE.md)。

## 当前产品与保存点

| 位置 | 当前角色 |
|---|---|
| `D:/CODE/DLSS5_NEXT-workers/f6-rewrite` | 唯一F6产品写树，`codex/f6-rewrite`；累计产品保存点`ea8c304c3810408e4ec8c7ca1378718cc37cd02a` |
| `D:/CODE/DLSS5_NEXT` | `master`；迁移基线、导航和共享研究，未合并F6最新产品源码 |
| `tools/research/encore-f7` | 主树共享研究；保存点`df97f06f5c2f7bdb3657633200d3c70f874c2697`，本次616项CPU测试通过 |
| `D:/DATA/Documents/ChatGPT/DLSS F5 NEXT` | 旧根、回退与历史证据，原位保留 |

最新候选 **DD53514B**：Release x64 exit0，33,959,936 B，SHA256 `DD53514B481D4E2DCAAA5F9D95ABA269E1DEB8834CFDEB6BD3E62852AD526646`，**未部署**。正式安装仍为 **81DBE9A5**，完整SHA `81DBE9A53B90990FA06BF4CB3790B240B187AAF46841906C7D665E9C0FFF8875`。本次再次核对两文件，未修改候选、安装DLL或游戏配置。

DD53514B包含Look诊断预览冲突修复、复用真实recording fence的GPU计时与低频分段；之前夹具误读描述符的“旧色”归因已撤回。原构建基于`db0e646`及当时改动，事后Git保存点不会改写DLL内嵌版本；21项关键源码哈希仍与构建回执一致。

## 已确认与待验收

用户确认7C281A54解决呈现NR＋FG明显卡顿／旧帧闪回。原神默认真实源帧呈现输入→NR→细节保留→FG；保留SR前后对照、用户参数和独立NR/FG设置。内部FSR桥禁用，外部桥2.4 ABI保留兼容。

持续细节保留、旋转发白／噪声稳定、Look实机、卡死恢复、高延迟、轻微FG残影和HUD保护仍未验收。PID153992／164004已有新局，不能再写“尚未启动新局”；160ms峰值唯一根因未知。星铁、ZZZ分别回归，构建和CPU/WARP通过不等于运行通过。

本次仅整理文档与Git：保留未跟踪源码、生产shader、研究固定输入、许可证和回退；累计产品及共享研究分别提交，其后保存本报告／导航。其它工作树不修改、不合并、不删除；不推送、不部署或操控游戏。

## 接手入口

- [完整交接](../../DLSS5_NEXT-workers/f6-rewrite/docs/F6-HANDOFF.md)：关键接口、准确候选／部署身份、命令、原始证据和后续验收。
- [外部参考仓库](REFERENCE-REPOSITORIES.md)：对应位置、版本、已参考／待参考功能，区分源码、摘录与二进制。
- [共享研究说明](../tools/research/encore-f7/README.md)：逆向与离线测试的范围，未执行外来目标DLL。
- [Bridge／GIMI评估](UPSTREAM-BRIDGE-REVIEW.md)：固定版本源码比较；不是本次运行回执。
- [性能审查](../../DLSS5_NEXT-workers/f6-rewrite/docs/F6-PERFORMANCE-REVIEW.md)：copy reader、hidden Present、模型外成本及已撤回归因。

## 迁移基线与资料保留

2026-11-04完成主根迁移及清缓存Release构建，移除约110.94 MiB可证冗余；保留真实依赖、许可、离线shader和被引用测试输入。具体历史事实见[源码执行](AUDIT/PRUNE-SOURCE.md)、[依赖执行](AUDIT/PRUNE-EXTERNAL.md)、[构建审计](AUDIT/BUILD-CONFIG.md)，不代表当前功能验收。

`build/`是ignored本地资料目录，除可重建obj/out还保存运行日志、DLL回退和身份回执；不要为干净Git删除它。主树和F6树保存点及最终文档提交以各自`git log -3 --oneline`为准；本次没有把累计F6代码合并到master。
