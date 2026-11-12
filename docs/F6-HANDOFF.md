# F6 项目交接入口

更新：2026-11-12。**ZZZ新局确认native四呈现槽与producer recording生命周期耦合；源码8df406c4715d已修所有权，候选0C7D6709构建／测试／独立复核通过，尚未部署。卡顿改善待实机验收，HDR暂缓。** 详见[产品当前状态](../../DLSS5_NEXT-workers/f6-rewrite/docs/CURRENT-STATE.md)和[交接第0节](../../DLSS5_NEXT-workers/f6-rewrite/docs/F6-HANDOFF.md)。

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，`codex/f6-rewrite`；本根是 `codex/navigation-20261112` 导航／迁移基线。GitHub目标 `thx114/DLSS5-F6-NEXT`。正式四路径仍1ABD9AFD／51fdb7b，桥CC301B42，硬链接、配置和profiles未改；按本轮AGENTS不改正式部署。

PID185668固定日志108,242,264 B／1,267,211行，版本51fdb7b：2328次已输出busy采样全为Reset pending，594条详细行after_refusal fence采样均达target；FG启／停及最大呈现延迟1→6／6→1各1510次。源list复用帧距不等于Reset间隔，完整卡顿根因仍有验收边界。新实现分开真实录制GPU owner与四槽当前身份，保留Reset／自身queue/fence／source、execution与失败保留门。

候选SHA256 `0C7D6709A0364761BA05C9AD03E3B7EA3F2D13ABCF8BF3FD1A9DDBF712425A6E`，Release x64；2428项输入／948导出和定向回归通过。证据在产品 `build/repair-20261112/zzz-recordings-*`，日志分析 `evidence.json`；candidate指针不代表正式已部署。未查询或操控游戏；用户安排部署后手动验收，不重复关闭FG对照。旧候选、回退、其它工作树及历史证据保留。
