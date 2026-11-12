# F6 当前状态

更新：2026-11-12。**原神、星铁、绝区零的 `local/F6-ASI` 包及三个隔离 runtime 已原位部署 `A6D12031`，源码基于 `75d7a27` 的未提交工作树。F6 继续负责 SR／NR／FG，`External=false`。此前菜单点击／焦点已获用户初步通过反馈，新版游戏加载、画面与周期卡顿尚待观察；HDR 暂缓。**

本轮源码已移除旧功能入口和实现，保留通用 ASI 插件加载；新建包工具只引用用户自备插件目录，不复制插件。本次仅更新既有包 DLL，没有复制或运行用户 ASI。Release x64 构建及离线验证通过，2421 项冻结构建输入前后一致、948 个导出不变；冻结身份与验证见 `build/repair-20261112/asi-only-{source,artifact,tests}.json`。

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，`codex/f6-rewrite`。本次三游戏 ASI 授权取代此前“仅绝区零、其他游戏不变”；旧 `official/F6`、`local/F6-ZZZ-ASI`、旧 runtimes、共享正式 DLL 四硬链接与共享 `current-deployment.json` 保留。

| 项目 | 当前事实 |
|---|---|
| 统一包 | `D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/OptiScaler/local/F6-ASI` |
| 游戏 runtime | `cache/games/<biz>/OptiRuntime/784CEF0274B47D8E`；biz 分别为 `hk4e_cn`、`hkrpg_bilibili`、`nap_cn`，各有自己的 profile、NR 和 authority/baseline |
| DLL | SHA256 `A6D12031D8B33AF050A81DF75029EEC7096D0AC4668D47308CC2FB6D545727D6`，33,951,744 B，`0.1.29-final (75d7a27)`；源码为 `75d7a27e9639b7f050eba3a83dea37456c2e9eed` 基础上的未提交工作树，由冻结清单绑定 |
| 部署 | `DEPLOYED_IN_PLACE`；包与三 runtime 的 4 个 DLL alias 均已独立回读为新字节，原 FileId／完整硬链接集合保留；回退 DLL 独立核验为实际部署前的 `6D377DD4` |
| 配置 | 三游戏均 `LoadAsiPlugins=true`、`External=false`。原 5 张生成帧／6×请求、SR／NR 参数及各自 NR 选择保留 |
| NR | 原神 `3E83DFE5910FE9609A51197531FF446B336762C7EF273F1092BBCBD0D6DFE9B9`；星铁和绝区零 `CDFA6C1EC784867182164C7836F324F087B5C9F374D0C9FE46F54999FEABF3ED`（310.9.16-SF）。新核验 plan 固定这三份 NR 身份 |
| 菜单改动 | Win32 后端通过线程内 bypass 读取真实状态；移除 DX11 overlay 冗余平台轮询；快捷键先于可见性 policy；不再伪造 HasGamepad 或写 WantSetMousePos；保留键盘／手柄导航，去除 Tab 重复导航框 |
| 已确认缺陷 | 原代码能让自身 Shift 补偿读到假松键、手柄读到 neutral；重复平台轮询还会缩短 DeltaTime。它们均有可执行旧行为负控，但尚未证明是用户“冷开即点不动”的全部根因 |
| 输入验证 | 仓库真实 ImGui 与抽取的生产 backend／input／DX11 overlay：51,959 项检查通过，8 个可执行负控和 1 个调用点清单负控检出。OS／D3D 采集使用受控 seam，未访问游戏或加载用户 ASI |
| 当前离线验证 | 通用 ASI 224 项检查／22 个无害夹具场景、部署工具11项测试；Telemetry 53、OptionsPolicy 1132、PresetMenu 18、静态接线96；最终本地化静态1999词条、2313处中文、6个负控通过。Runtime 6034项执行后仅删5条无引用词条，最终字典重跑静态；Release x64 exit0，保留既有warning |
| 诊断 | 每次打开菜单最多 48 条 `F6 menu input`，记录采集／消费坐标、按钮、Shift、导航、队列及输入 flags；不记录输入文本。新包日志为 `logs/<biz>/OptiScaler.log` |
| 旧运行证据 | 固定会话 `20261112-214427_ZenlessZoneZero_193648` 已证明旧绝专属 97a2cad 加载、ASI `status=10 InitReturned`；不是新统一包／新候选的加载、显示倍率或性能证明 |
| 保护边界 | 本次新基线1857项文件部署前后字节、FileId及大小均一致，覆盖当前配置／插件／NR、旧包／runtime、全局state和共享部署回执；三游戏构建选择仍为 `local/F6-ASI`。历史外部配置重写另留记录，没有恢复旧快照 |
| 未改动 | 未查询、启动、终止或操控游戏；未改游戏文件、HDR、FSR 桥、正式共享部署或全局 state；不重复要求关闭 FG 对照 |

**生效步骤：由用户手动重开游戏加载新 DLL。** 本次构建选择没有变化。此前首次切换构建时要求完整重启启动器，是为刷新 GUI 配置缓存，不能据旧步骤推断本次游戏已经加载新版。

当前构建指针为 `build/current-{artifact,build,tests}.json`；隔离部署回执为 `build/current-asi-deployment.json` 和 `build/current-game-deployment-<biz>.json`，共享 `build/current-deployment.json` 未改。本次新基线、DLL写入及独立复核分别是 `build/repair-20261112/asi-only-deployment-pre.json`、`asi-only-dll-deployment.json`、`asi-only-deployment-verification.json`。回退路径见回执，旧指针备份为同目录 `asi-only-before-deploy-*`。历史 `menu-input-*`、`three-game-asi-*`、`zzz-asi-*` 保留；旧plan不满足当前 `UserPluginsPath` 规则，不用新工具重新prepare或verify现有旧包。

回退可在启动器切回各游戏原构建选择（原神／星铁 `official/F6`，绝区零 `local/F6-ZZZ-ASI`）；若只回退新统一包 DLL，沿已核回退字节用 `Deploy-F6.ps1 -BuildId local/F6-ASI` 原位写入并复核全部 alias。占用只暂存 `.next`，不查询或终止游戏，不把暂存当成功部署。
