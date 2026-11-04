# F6 当前状态

2026-11-04最终收口：用户明确只要交接，长期F6目标已实际paused。误新派的R1079/Ptolemy、R1080/Meitner、R1081/Beauvoir均已停止、关闭并实际归档，当前无活跃子智能体。没有新集成/构建/部署/游戏测试；最新正式仍为08:30:49联合版。

2026-11-04，R1077整理交接。根 `OptiScaler/` 为唯一物理当前源码；旧 `work/f6-source/OptiScaler` 仅兼容junction。Root接回唯一集成/构建/部署/CLI所有权；本轮已停止继续修复，不启动新构建/游戏。

## 正式版与路径

- 最后正式为R1072+R1073联合版：根40289/cell719 compile/link/Deployed in place actualexit0；artifactUTC2026-11-04T08:30:49.1715003Z/34796544bytes，日志 `audit/r1073-before-sr-filter-consumption-20261104/F6-BUILD-DEPLOY-joint.log`。包含R1067 continuity/R1069 sustained等此前正式片；不含R1075候选，R1076/R1078零hunk。
- 根 `BUILD-DEPLOY.ps1` 默认F6；显式Main报冻结exit1。仅扫描根OptiScaler/活external，MSBuild字面入口仍work/f6-source，原work/f6-build/obj-main/out/PCH/PDB/tlog保留。整理后的入口NOT-RUN，未新构建/部署/验证。
- 新部署只official/F6，原位流写保runtime硬链接；Main/official DLSS F5覆盖包冻结。正式profiles权威，保当前画质/倍率/SL/PTX、MOD/UID开关和真实GPU完成。

## 未修结果

- 原神51780/session20261104-161924_YuanShen_51780，t0北京16:19:14.423，creationUNKNOWN：180s CLI0，后精确只读notrunning，退出原因未知。NR只有3个低频model_depthtrue样本，不证明world持续深度或像素；用户FG无效果、Win菜单才见监控USER_FAIL。R1076确证异步SDK61148 raw与render52060 TLS显示许可断链，同步失焦短暂shown，0hunk未修。
- ZZZ48964/creation134382543438743436：120s CLI0且明确forced kill；selectedSR1591、NR cadence/modelDepth非空，FG0/Deactivate113。source23成功提交后Reflex改4882，current-guide-pair拒绝；4893→4896重复。R1078源码调查0hunk，未修；不以缺某模型日志字符串判NR0。
- 星铁34648/creation134382512438861274：90s CLI0/forced fallback，FGOk2058/Deactivate0；NR真实GPU135组，total中位56.5535ms包含model53.8952ms，不能相加/倒数当world帧率。scene1801/history1799、反馈请求却未融合；动态曝光/残影/细节及净性能未通过。保用户adamfg式独立FG。
- 这些CLI都早于最新08:30:49联合版；R1072完整MOD Load/像素和R1073 BeforeSR真实效果/净成本尚无新运行验收。旧设备hung首因UNKNOWN，3个深度样本不覆盖旧持续失败。

## 候选、owner和下一步

- R1075两文件私有候选已交，PRIVATE-SOURCE-ONLY/未集成/未构建/未运行，before/candidate/patch/RESULT原位保护；作者已close/archive。后续Root有限审真实相邻源发布/消费/退休，保单步ordinal、contract、真实fence/owner及原神/ZZZ隔离。
- R1076/R1078均0-byte patch/0hunk，已交调查RESULT、全部句柄空且close/archive，不能冒正式修复。R1072/R1073已正式、作者关闭；R1074 DESIGN已交关闭。后续编码6.1Sol/high/fork=false；DeepSeek仅日志/行搜查。
- R1077真实源码上移、2个兼容/依赖junction、15旧根文件与3空脚手架归cleanup、2活参考移docs；共21次move、11份文档/入口字节前像。保Git/dirty/有效增量/依赖/冻结包/配置/最近rollback/唯一证据，未知旧树保留，同盘归档不称释放空间。
- 整理worker全部同步操作结束后停止写入、全写权归Root；无活终端/构建/部署/CLI/game/后台操作。Root有限读入口/链接/交接后关闭归档，不加测试。
- 完整三游戏source/consumer/坐标/owner/fence/history/滤镜/GIMI/UID/UI/配置/CLI及保留清单见 [HANDOFF](HANDOFF.md)，技术主体见 [F6技术路径](F6-TECHNICAL-PATHS.md)，事实归 [RECORD](audit/v3-r044-nr-off-fg-performance-20261030/RECORD.md)，逐轮归 [INDEX](PERFORMANCE-ROUND-INDEX.md)。
- Root已完成有限读取；R1077 Hooke全部句柄归还后已close/archive，当前无活跃子智能体，本轮整理交接收口。
