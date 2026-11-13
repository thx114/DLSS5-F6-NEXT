# F6 当前状态

会话切换交接（2026-11-13）：用户要求编写交接后更换会话与模型，性能开发停在下述已部署、待手测状态。本次只更新文档；新会话从[交接第0节](F6-HANDOFF.md)的停止点和恢复顺序继续，先读当前 `AGENTS.md`。第0.05节及后续旧自主操作游戏授权已撤销，测试完全由用户手动完成；不因续接重复构建／部署。

更新：2026-11-13。主线是**关闭输入预览时仍存在的崩铁 FG 卡顿，以及 GPU 排队／等待**。原神用户反馈基本流畅但有排队／延迟。绝区零 FG 深度／运动预览空白已定位源码限制，用户要求先集中处理正常渲染卡顿，该预览改造已保存并撤出候选。

唯一产品树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，分支 `codex/f6-rewrite`，HEAD `6cf94c0979a2697775992f4ffa8005f57c8817eb` + 累计未提交改动。不得 reset/clean；本轮未提交、未推送。游戏由用户手动测试，AI 不控制桌面或通过 CLI 启停游戏；启动器选择、INI、profiles、ASI 不变，HDR 暂缓。

## 本轮修复与候选

- 正常 StarRail 路径在 `SetUpscalerGuidePair` 成功后立即提交原 guide recording 并归还共享输入，避免其归还 fence 排在 Present 的颜色复制／finished-picture NR 之后。沿用真实 Execute／Signal → DX11 GPU Wait，不增加 CPU 完成等待或资源槽位；原神已有早归还。正常Present的预览mode路由改为原子读取，避免新增长阻塞依赖。
- FG 深度／运动和离线采样仍保留原 recording；FG 颜色预览先确认本源最终颜色已 join，再预约采样，避免提前消耗节流机会。无离线请求时需求查询直接返回，不读诊断时钟。
- Release x64 最终构建 exit 0，90 秒，DLL `AB0B37E017C59F54608A67A8C5AA6BB2E360FAF55088A7727FFDF20A31457F2F`，33,899,520 B，mtime UTC `2026-11-13T11:26:47.5877949Z`。已有编译／链接警告仍在。
- 独立复核发现的demand mutex依赖已修复为原子GetPreviewMode，持锁跨线程用例通过；复核未发现剩余阻断。已原位覆盖 `local/F6-StarRail-GapPacing` 与 `local/F6-ASI`，三个游戏既有runtime别名随原文件对象同步；原FileId与2／6个硬链接均保留。中间候选B84未部署。回执 `early-guides-deployment.json`、`early-guides-installed-after.json`，汇总 `early-guides-result.json`。待用户重开游戏、关闭预览手动复测，尚无本候选实机验收。
- WARP handoff 30 场景／1186 检查及旧队列、延迟 guide 读取、缺 DX11 writer Wait 负控通过；GuideCapture／GuidePreview WARP、producer replay 检查通过。43 项 StarRail 准入、10 项预览采样门及GuideFgWiring连接检查通过。未重跑无关全量回归，无本候选实机验收。

## 手动测试与未解决项

证据目录 `build/fg-runtime-followup-20261113/`。崩铁 PID188816（26B）用户反馈“有所改善，但仍会卡顿”；持续前台约98秒段的 SR 输入间隔中位44.730 ms、P95 57.869 ms，174份有效唯一Reflex报告的GPU启动前等待中位48.460 ms、P95 56.179 ms。`copyAllocatorCpuWait` 前台样本最高40.555 ms，保护真实仍在读取共享纹理的GPU工作，不能直接删除。

原神 PID209436 使用A6构建，用户反馈基本流畅但有排队／延迟；周期前台观测内873份有效唯一Reflex报告的GPU启动前等待中位17.663 ms、P95 51.019 ms；复制槽CPU等待样本最高50.029 ms。首帧Exposure缺失和两次Off+retain `eErrorInvalidState` 均保留，不宣称零错误。两游戏构建和场景不同，不作横向性能结论。

崩铁部分约半秒新Evaluate空档与用户ASI日志的feature_parked/reused区间对应，Present仍继续；这只能确认逻辑feature生命周期空档，不能归因为ASI吞调用。Reflex GPU区间减活跃时间也不能证明CPU导致GPU空闲，尚无同帧跨时钟关联。新候选无实际显示间隔采样或加载内存SHA。

绝区零 PID201984实际使用A6，日志已有真实producer提交、SR成功及depth/MV tags accepted，尚不等于最终有效插帧。原生SR游戏列表与预览要求的独立UI列表不同，存在确定源码拒绝；本局日志未直接记录该拒绝命中。未完成的copy-only预览方案在 `native-preview-deferred.patch`，不在当前DLL中。

## 下一步与保留边界

最终DLL已原位覆盖，下一步请用户手动重开游戏、关闭预览复测战斗卡顿和GPU排队。继续区分SR新输入空档、复制槽退休等待与SDK Present／Reflex等待；早归还只收窄已确认依赖，不宣称消除全部卡顿。

SR对象缓存继续由用户自备 `NgxFeatureCache.asi` 负责，不恢复内置PersistentSR。无depth/motion的菜单FG尚未实现；不借旧帧、不造假guides、不删除真实同步、不扩大池。9项已知问题与7项TODO继续保留。源码保存点、日志及回执见[交接第0节](F6-HANDOFF.md)。
