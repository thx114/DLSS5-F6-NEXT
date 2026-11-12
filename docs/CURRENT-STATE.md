# F6 当前状态

更新：2026-11-12。**原神、星铁、绝区零均已在磁盘选择 `local/F6-ASI`，隔离包及三游戏 runtime 已部署菜单输入候选 `6D377DD4`／源码 `3665de5`。ASI 只负责 MFG 解锁，F6 继续负责 SR／NR／FG，`External=false`。冷开菜单点击和周期卡顿仍待实机验收；HDR 暂缓。**

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，`codex/f6-rewrite`。本次三游戏 ASI 授权取代此前“仅绝区零、其他游戏不变”；旧 `official/F6`、`local/F6-ZZZ-ASI`、旧 runtimes、共享正式 DLL 四硬链接与共享 `current-deployment.json` 保留。

| 项目 | 当前事实 |
|---|---|
| 统一包 | `D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/OptiScaler/local/F6-ASI` |
| 游戏 runtime | `cache/games/<biz>/OptiRuntime/784CEF0274B47D8E`；biz 分别为 `hk4e_cn`、`hkrpg_bilibili`、`nap_cn`，各有自己的 profile、NR 和 authority/baseline |
| DLL | SHA256 `6D377DD43A2355EB434D10FCD4FC657219FE9F6BD78AA8D912B245E17B7CC17F`，33,998,336 B，`0.1.29-final (3665de5)`；构建源码 `3665de5174106ce2f491b910e0278f24f44ce04a` |
| 部署 | `DEPLOYED_IN_PLACE`；新包与三 runtime 的 4 个 DLL alias 均为新字节，原 FileId／硬链接保留。旧候选 283B6B07 和实际部署回退均有独立备份 |
| ASI | 既有 `MFGUnlocker.asi`，SHA256 `171AC18D4D632D12DEE22D5CAF2085F301BACFDB9A462561D93DBCBB61D6A4F8`；不嵌入 DLL，插件路径指向新包 |
| 配置 | 三游戏均 `AdaMfgUnlock=false`、`AmpereMfgUnlock=false`、`LoadAsiPlugins=true`、`External=false`。原 5 张生成帧／6×请求、SR／NR 参数及各自 NR 选择保留 |
| NR | 原神 `3E83DFE5910FE9609A51197531FF446B336762C7EF273F1092BBCBD0D6DFE9B9`；星铁和绝区零 `CDFA6C1EC784867182164C7836F324F087B5C9F374D0C9FE46F54999FEABF3ED`（310.9.16-SF）。新核验 plan 固定这三份 NR 身份 |
| 菜单改动 | Win32 后端通过线程内 bypass 读取真实状态；移除 DX11 overlay 冗余平台轮询；快捷键先于可见性 policy；不再伪造 HasGamepad 或写 WantSetMousePos；保留键盘／手柄导航，去除 Tab 重复导航框 |
| 已确认缺陷 | 原代码能让自身 Shift 补偿读到假松键、手柄读到 neutral；重复平台轮询还会缩短 DeltaTime。它们均有可执行旧行为负控，但尚未证明是用户“冷开即点不动”的全部根因 |
| 输入验证 | 仓库真实 ImGui 与抽取的生产 backend／input／DX11 overlay：51,959 项检查通过，8 个可执行负控和 1 个调用点清单负控检出。OS／D3D 采集使用受控 seam，未访问游戏或加载用户 ASI |
| 其他验证 | 隔离部署夹具 42 项；本地化静态清单 2013 项词条、2328 处中文、6 个负控通过。Release x64 exit0，2435 项声明输入构建前后一致，948 个导出名称／序号不变；独立 gpt-6.1-sol/high 复核无剩余确定 blocker |
| 诊断 | 每次打开菜单最多 48 条 `F6 menu input`，记录采集／消费坐标、按钮、Shift、导航、队列及输入 flags；不记录输入文本。新包日志为 `logs/<biz>/OptiScaler.log` |
| 旧运行证据 | 固定会话 `20261112-214427_ZenlessZoneZero_193648` 已证明旧绝专属 97a2cad 加载、ASI `status=10 InitReturned`；不是新统一包／新候选的加载、解锁倍率、显示或性能证明 |
| 保护边界 | 菜单 DLL 升级前后 1589 项旧文件字节与 FileId 核验通过。初次三游戏准备后曾观察旧绝配置／metadata 的 4 处外部重写及 LastCatalogFetchUtc 变化，另留记录；SR／NR／FG 有效参数未变，没有恢复旧快照 |
| 未改动 | 未查询、启动、终止或操控游戏；未改游戏文件、HDR、FSR 桥、正式共享部署或全局 state；不重复要求关闭 FG 对照 |

**生效步骤：从托盘“退出”完整结束启动器，再无参数重启启动器，然后由用户启动游戏。** CLI 的 `set_opt_build` 只改数据库，不刷新已打开 GUI 的 AppConfig 缓存；关窗口留在托盘不算完整重启。没有受支持的外部 reload／exit CLI，不改启动器源码。

当前构建指针为 `build/current-{artifact,build,tests}.json`；隔离部署为 `build/current-asi-deployment.json` 和 `build/current-game-deployment-<biz>.json`，不能把它们当共享正式部署回执。当前核验 plan 为 `build/repair-20261112/menu-input-asi-plan.json`；部署前基线 `menu-input-pre.json`、实际 DLL 写入 `menu-input-dll-deployment.json`、升级复核 `menu-input-deployment-verification.json`。菜单分析／测试／构建证据为同目录 `f6-menu-*.json/log`、`menu-input-{source,artifact,tests}.json` 与构建日志。初次三游戏 ASI 的 `three-game-asi-*` 和旧绝区零 `zzz-asi-*` 均保留，不重复 prepare 已存在目录。

回退可在启动器切回各游戏原构建选择（原神／星铁 `official/F6`，绝区零 `local/F6-ZZZ-ASI`）；若只回退新统一包 DLL，沿已核回退字节用 `Deploy-F6.ps1 -BuildId local/F6-ASI` 原位写入并复核全部 alias。占用只暂存 `.next`，不查询或终止游戏，不把暂存当成功部署。
