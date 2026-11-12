# F6 项目交接入口

更新：2026-11-12。当前修复与验收边界见[产品交接第0节](../../DLSS5_NEXT-workers/f6-rewrite/docs/F6-HANDOFF.md)及[产品当前状态](../../DLSS5_NEXT-workers/f6-rewrite/docs/CURRENT-STATE.md)。

产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，`codex/f6-rewrite`；源码提交b1ee21020a58，ADFF3486已按用户要求部署并完成磁盘核验。FG排队、退出入口和英文已补，HDR暂缓；HUDless/UI缺桥输入。本根仅为`codex/navigation-20261112`导航和迁移基线。

正式及三个游戏缓存均为ADFF3486，原文件身份／硬链接不变；桥保持CC301B42，2,535项受保护文件及profiles不变。旧DLL已备份，旧.next已归档移除。用户已确认ZZZ关闭FG后卡顿消失、仍有GPU等待；新候选尚无游戏验收。构建／测试回执在产品树`build/repair-20261112/repair-current-*`，当前部署回执已指ADFF3486，核验见产品树`build/f6-deploy-ADFF3486-verified.json`。
