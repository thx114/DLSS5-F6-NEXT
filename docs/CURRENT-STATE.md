# F6 当前状态

更新：2026-11-12，**普通内置 FG 修复版本 5265915D 与 FSR 桥 CC301B42 均已部署，待用户实机验证。** 最新修复、测试和边界见[完整报告第 0 节](F6-HANDOFF.md)。唯一产品写树 D:/CODE/DLSS5_NEXT-workers/f6-rewrite，codex/f6-rewrite，HEAD 9ab9686；大量既有未提交改动保留，不 reset/clean。

| 项目 | 当前事实 |
|---|---|
| 用户反馈 | 崩铁 OK、ZZZ 仍未检测 DLSS；优先验证 ZZZ 接入，FG 用内置，不用 ASI。旧 F6 在 ZZZ 本局实际调用 MFGUnlocker.asi；崩铁反馈不认证新候选的内置 FG 或性能 |
| 普通候选 | build/out/OptiScaler.dll；5265915DF9DC34951F10E3D60CB11819EADB5FAAABD427F7F4DDA846B1360238；33,949,696 B，Release x64 exit 0 |
| 链接／导出 | MfgUnlock.obj 已链接，无私有模块／旧 ASI 对象，53 必要 NGX 导出齐全；全部导出名称／序号不变 |
| 源码身份 | build/repair-20261112/verification-next/source.json；1,245 项声明输入指纹不变。清单遗漏 FgDepthContract.h，亦不含生成版本头和全部外部传递依赖，不能单凭清单认证被遗漏文件的构建时字节 |
| 机器指针 | current-artifact.json／current-build.json／current-tests.json 对应 5265915D；current-deployment.json 记录本次实际部署，旧回执已另存 |
| F6 安装复核 | 正式 DLL 和三个游戏缓存的磁盘 SHA256 均为 5265915D，四路径原文件身份／硬链接不变；原安装 8FCB8632 已备份，2,535 项保护文件不变，loaded-memory SHA 未知 |
| ZZZ 最新运行 | PID180352 固定日志有两组 DLSS Create／Evaluate，均 owner=backend，selected 为 0；缺 Create origin/caller，不能定位具体来源拒绝，也不能作为 5265915D 的失败证据 |
| 未决 | 5265915D 已部署，ZZZ 接入与内置 FG 待新局实测；崩铁记部署前用户反馈通过，精确 FG 显示率、排队改善及旧 hook 错误 8 根因仍未测定 |
| FSR 桥 | 按用户要求已将上游 1865c585 的 Release x64 桥原位部署到启动器实际选中的 temp3/Dx11FsrBridge (2).dll，SHA CC301B42；旧桥已备份，配置及 F6 DLL 未改，游戏效果待验；[审计与回退](../../fsr-bridge-upstream-1865c585/AUDIT.md) |

7D 阶段补菜单 FG_Hooks 声明依赖；分离能力 Pending 与后续 provider 加载处理；保留旧配置并提供明确提示／手动关闭按钮。独立复核发现工厂公共 Begin 在取得 Detours 所有权后失败会遗留事务，已统一在原始 Begin 包装器中清理，绝不 Abort 已有事务；不改变真实 queue/fence/Reset 退休门或 FG quarantine。

最终定向测试回执为 `build/repair-20261112/verification-next/tests/tests.json`：17 组通过，设备 66 项、Router 15 套、Release 7 套、生命周期 40 项、caller 49 项及 11 套核心 CPU／WARP 等通过，负控按预期检出。独立静态复核未留确定阻断，记录在同目录上层 `review.json`；实际 SDK、游戏和显示未验证。旧 `build/repair-20261112/tests.json` 保持原身份。下文为 7D 阶段记录，当前以第 0 节为准。

最终通过 FG hook 54 场景／1867 检查，caller 42、admission 38、内置谓词 41、Begin 清理 26 检查；五种独立负控检出预期错误。ZZZ scope 16 套／wiring 107、NGX Router 9／Release 7、latency adapter 1380／调度 1065 及十一套 CPU／WARP 核心定向回归通过。shared 段在最后工厂修复前执行成功，修复后不冒称全量公共套件重跑。gpt-6.1-sol / high 独立复核及主会话核实详见 build/input-integration-review.json。

候选、源码、测试与日志见 build/input-integration-{standard-artifact,source,tests,review}.json，旧普通 8C9418B6／私有 92D1BACE 在 build/rollback/queue-latency-* 原位保留。离线测试不证明 SDK 推理、游戏画面、真实显示率或延迟。

## 2026-11-12 10:18–10:30 实测补充

三局已独立锁定：[崩铁大世界 PID129300](../build/starrail-129300-20261112/analysis.md)、[绝区零 DX12 PID197852](../build/zzz-197852-20261112/analysis.md)、[崩铁副本 PID181400](../build/starrail-181400-20261112/analysis.md)。副本完整日志 21,726,659 B 已保住，非仅启动器 16 MiB 尾段；完整索引见[固定测试记录](../build/runtime-tests-20261112.md)。

大世界用户反馈插帧卡顿／延迟高，性能未通过。稳定分析窗口源入口约 24.67 Hz，中位间隔 40.492 ms，最高 81.398 ms；Reflex 报告中位／P95 为 46.978／72.421 ms，最高 106.432 ms 中 GPU 前排队 69.117 ms。副本报告中位／P95 为 45.597／85.916 ms，也有排队尖峰；3.287 秒独立异常主要发生在 queue 开始前，不是持续 GPU 耗时。两局均有 FG 成功提交，但真实显示率未知；场景／焦点不同，不能当修复对照。

绝区零确有成功的 DX12 DLSS Create／Evaluate，两个句柄均被记为 backend，未接入 selected 超分输入，不是游戏没开 DLSS。三局都有私有 FG 模块初始化记录，**不是未部署的普通内置 7D042E99 验收**；精确 loaded-memory SHA 未知。本轮只锁日志、分析和更新记录，不改产品、用户参数或部署。

后续部署只按目标 DLL 实际占用处理：可写原位保留硬链接，被占用写同目录 OptiScaler.dll.next，仅记待替换；不查询或终止游戏。用户手动启动／进入场景，分别绑定新 PID／加载身份／当局日志。保留独立 SR／NR／FG、用户参数、内部 FSR 禁用和外部桥 2.4 ABI。[参考来源](../../../DLSS5_NEXT/docs/REFERENCE-REPOSITORIES.md)。
