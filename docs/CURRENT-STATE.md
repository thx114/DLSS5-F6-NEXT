# DLSS5_NEXT 当前状态

更新：2026-11-12。**产品修复候选ADFF3486已构建、未部署；HDR暂缓。** 当前工作包含FG排队锁环修复、退出入口保护和完整英文，详见[产品当前状态](../../DLSS5_NEXT-workers/f6-rewrite/docs/CURRENT-STATE.md)与[交接第0节](../../DLSS5_NEXT-workers/f6-rewrite/docs/F6-HANDOFF.md)。

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，`codex/f6-rewrite`，源码提交 `b1ee21020a58`。本根 `D:/CODE/DLSS5_NEXT` 为 `codex/navigation-20261112` 导航／迁移基线，不在本根构建最新产品。GitHub目标 `thx114/DLSS5-F6-NEXT`，main未改。

ZZZ旧局186740已出现selected DLSS接入；用户实测关闭FG后周期卡顿消失，仍有GPU等待。新修复、退出和英文布局尚待游戏验收。16组定向验证及Release x64通过；2425项输入构建前后相同，53必要NGX导出及全部948导出核验一致。新候选SHA256 `ADFF3486F21C52FCD0FE72D73A14BB3AFA93E599A2A73AD72DCC7EAD1F790372`。

正式F6保持5265915D，桥保持CC301B42。本轮不写正式部署或profiles，不查询、启动或操控游戏。HUDless/UI缺真实桥输入，不以SR颜色或reactive/transparency mask伪造。

回执在产品树 `build/repair-20261112/repair-current-*`，构建指针与安装指针分开。HDR已归档撤出候选。保留真实queue/fence/Reset、独立SR/NR/FG语义及用户参数；不扩大池子或借旧帧。旧根、其它工作树、build候选／回退／证据和许可证全部保留。来源见[仓库清单](REFERENCE-REPOSITORIES.md)。
