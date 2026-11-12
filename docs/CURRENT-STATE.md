# DLSS5_NEXT 当前状态

更新：2026-11-12。**原神、星铁、绝区零均已选择 `local/F6-ASI`，三游戏隔离 runtime 已部署菜单输入候选 `6D377DD4`／源码 `3665de5`。ASI 仅负责 MFG 解锁，F6 保留 SR／NR／FG、`External=false` 和各游戏 NR／6×请求。冷开菜单点击与周期卡顿仍待实机验收，HDR 暂缓。**

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，`codex/f6-rewrite`；本根仅导航／迁移基线。新 DLL、三 runtime 硬链接和 1589 项升级前保护文件已核验，旧共享正式部署、旧绝专属包／runtimes、全局 state、原配置及回退保留。菜单输入隔离、单次平台采样和可见性 policy 通过真实 ImGui 受控回归，不能替代游戏画面或性能验收。须从托盘完整退出启动器后重启，CLI 写数据库不刷新现有 GUI 缓存。

权威明细见[产品当前状态](../../DLSS5_NEXT-workers/f6-rewrite/docs/CURRENT-STATE.md)与[产品交接第0节](../../DLSS5_NEXT-workers/f6-rewrite/docs/F6-HANDOFF.md)。本根历史材料不能覆盖产品当前状态。
