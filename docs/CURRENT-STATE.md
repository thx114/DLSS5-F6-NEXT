# DLSS5_NEXT 当前状态

更新：2026-11-12。**原神、星铁、绝区零的 `local/F6-ASI` 包及三个隔离 runtime 已原位部署 `A6D12031`，源码基于 `75d7a27` 的未提交工作树。F6 保留 SR／NR／FG、`External=false` 和各游戏 NR／6×请求。此前菜单点击／焦点已获用户初步通过反馈，新版加载、画面和周期卡顿尚待观察，HDR 暂缓。**

本轮源码已移除旧功能入口和实现，保留通用 ASI 插件加载；新建包工具只引用用户自备插件目录，不复制插件。本次仅更新既有包 DLL，没有复制或运行用户 ASI；2421项冻结构建输入、948个导出核验通过，四个DLL alias的新字节及原FileId／硬链接完整，1857项保护文件未变。源码、离线验证和部署证据见产品树 `build/repair-20261112/asi-only-*`。

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，`codex/f6-rewrite`；本根仅导航／迁移基线。旧共享正式部署、旧绝专属包／runtimes、全局 state、用户配置／插件及回退保留。本次构建选择未变，由用户手动重开游戏加载新版；离线回归和磁盘部署不能替代游戏画面或性能验收。

权威明细见[产品当前状态](../../DLSS5_NEXT-workers/f6-rewrite/docs/CURRENT-STATE.md)与[产品交接第0节](../../DLSS5_NEXT-workers/f6-rewrite/docs/F6-HANDOFF.md)。本根历史材料不能覆盖产品当前状态。
