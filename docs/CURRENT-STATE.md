# F6 当前状态

更新：2026-11-12。**按用户“绝改为 ASI，其他游戏不变”的最新指令，绝区零已单独选择 `local/F6-ZZZ-ASI`；原神、星铁仍为 `official/F6` 内置方案。绝专属 DLL `283B6B07`／源码 `97a2cad` 已构建、定向验证、独立复核并部署到独立文件对象；尚未实机验证 ASI 加载或卡顿改善。HDR 暂缓。**

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，`codex/f6-rewrite`。本次只新增绝区零包／runtime和修改绝区零一个构建选择键；共享正式 F6、原神／星铁文件、全局选择及原有游戏 profiles 均保留。

| 项目 | 当前事实 |
|---|---|
| 绝区零构建 | `local/F6-ZZZ-ASI`，独立包 `D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/OptiScaler/local/F6-ZZZ-ASI`，独立 runtime `cache/games/nap_cn/OptiRuntime/A5009284E9948E8D` |
| DLL 身份 | SHA256 `283B6B07A7CF9926437F2BEF188961064EE5BBB7ABEE9DEC72DE91783BE80BD6`，33,994,240 B，版本 `0.1.29-final (97a2cad)`；源码 `97a2cadfba894e20dbbd6c95efb476ec354903c3` |
| ASI | 复用已有 `MFGUnlocker.asi`，SHA256 `171AC18D4D632D12DEE22D5CAF2085F301BACFDB9A462561D93DBCBB61D6A4F8`；绝专属插件路径，未把插件嵌入 DLL，未运行用户 ASI 进行离线测试 |
| 配置语义 | 绝专属 `AdaMfgUnlock=false`、`AmpereMfgUnlock=false`、`LoadAsiPlugins=true`；`External=false` 保留 OptiScaler FG 输入／呈现。MFG 解锁由 ASI 负责；SR、NR、FG 请求与原 5 张生成帧／6×设置不变 |
| 启动实现 | 通用 ASI 枚举只登记规范 MFG 插件，既有 post-attach worker 单次初始化；重复 owner、失败、异常、worker 创建失败均终态保留。FG 消费前有界等待，loader／TLS／自身线程重入不等；未选 ASI 为 no-op，SR／NR 不依赖其成功 |
| 游戏 SL | SL2 有效请求仍含 DLSSG 才等待；Opti-owned 分支先移除游戏 FG 再直接初始化游戏 SR／Reflex。未声称 SL1 全路径已覆盖 |
| 定向验证 | 生产 Gate 28项、真实 loader context 5项、options1132项、builtin谓词41项、Reflex19项；实际生产 startup 在无害模块下跑7种 Windows loader 场景，抽取实际 SL 有效请求尾部16种组合通过；gpt-6.1-sol/high 最终静态复核无确定 blocker |
| 构建 | Release x64 exit0，2431项冻结输入构建前后不变，全部948导出名称／序号不变；既有警告保留于 `zzz-asi-build.log` |
| 其他游戏 | 原神、星铁仍选择 `official/F6`，共享及旧四路径 DLL 仍 `1ABD9AFD`／源码 `51fdb7b`，原 File ID／硬链接不变；233项本次保护文件逐字节及身份不变，全局 `OptiScaler/state.json` 未改 |
| NR／桥 | 绝 NR 仍为310.9.16-SF／`CDFA6C1EC784867182164C7836F324F087B5C9F374D0C9FE46F54999FEABF3ED`；共享 FSR 桥仍 CC301B42，未调整 HDR、HUDless、游戏文件或图形配置 |
| 验收边界 | 本次未查询、启动、终止或操控游戏。磁盘及单游戏选择已核验，实际加载、生成帧显示和周期卡顿改善未知，用户下次重启启动器及绝区零后验收；不重复关闭 FG 对照 |

当前候选与测试指针为 `build/current-{artifact,build,tests}.json`。共享正式部署回执 `build/current-deployment.json` 保持原字节；绝单游戏部署回执单列 `build/current-game-deployment-nap_cn.json`，完整证据为 `build/repair-20261112/zzz-asi-{source,tests,review,artifact,deployment}.json`，部署可复核脚本 `deploy-zzz-asi.py`。启动器CLI只执行 `set_opt_build`，数据库实际只改变 `launch_option_optiscaler_build_nap_cn_x6znKlJ0xK`。回到内置时只需将绝区零构建选择切回 `official/F6`，不覆盖共享 DLL。

上一轮 recording 所有权修复源码 `8df406c` 和未部署候选 `0C7D6709` 保留；本次源代码继续包含该修复，但本次用户“仍然有”不能单独证明未部署候选已受测。PID185668／日志51fdb7b的2328次 Reset-pending采样、1510次FG启停与四槽耦合证据仍在 `zzz-recordings-*`／`evidence.json`；不能将它们当本次 ASI 运行证据。旧候选完整备份 `build/rollback/zzz-asi-pre-0C7D6709.dll`，旧当前指针在 `zzz-asi-previous/`；旧日志和回退保留。
