# F6 当前状态

更新：2026-11-12。**新局确认 native guide 四呈现槽与真实 producer recording 生命周期耦合，可导致当前源帧拒绝与 FG 反复启停。源码 8df406c4715d 已拆分所有权；候选 0C7D6709 构建、导出、定向回归及独立复核通过，尚未部署，卡顿改善待实机验收。HDR 暂缓。**

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，`codex/f6-rewrite`；导航树 `D:/CODE/DLSS5_NEXT` 保留迁移基线。GitHub目标 `thx114/DLSS5-F6-NEXT`。按本轮根AGENTS.md“不改正式部署或游戏profiles”约束，正式及缓存DLL、配置和profiles未改。

| 项目 | 当前事实 |
|---|---|
| 固定新局 | PID185668，18:08–18:33，日志版本51fdb7b；`build/repair-20261112/zzz-185668.log`，108,242,264 B／1,267,211行，SHA256 `65A474D3DDC491D805DE1329B0604ECE4BB629C30ECE362D2B2B19BAD7830632`；loaded-memory SHA未知 |
| 拒绝原因 | 594条详细busy，已输出窗口累计2328次采样全为Reset pending；详细行均closed=false、submitted=true、signalled=true，after_refusal采样fence均达到target。不能倒推拒绝瞬间GPU状态或仅凭日志排除Reset通知缺失 |
| 源与时序 | selected Evaluate 41904次，Handle1000000／1000001分别11097／30807次，共6条list。epoch8主要阶段同list复用帧距以6最多（22567对），其次4（5501）／5（1652），不等于实际Reset间隔。FG启／停、最大呈现延迟1→6／6→1各1510次；401条busy可在同源帧窗口关联eOff及6→1 |
| 所有权修复 | 四槽只借用精确当前source身份；独立recording持有DS／RF、CB／descriptor、copies及真实producer收据。成功Reset＋自身queue/fence完成且CPU不再持有才可回收；按真实录制按需获取、复用已退休owner并回收多余退休对象，BUFFER_COUNT仍4 |
| 通知与保留 | Execute前后携带同次execution凭据；旧通知不能更新新帧observation。短锁interest覆盖全部活跃recording及wrapper/native别名；shared CPU lease和提交快照防重入释放／迭代失效。重放、Signal失败、未知完成继续保留；终止quarantine有意保留完整owner |
| 候选 | `build/out/OptiScaler.dll`，SHA256 `0C7D6709A0364761BA05C9AD03E3B7EA3F2D13ABCF8BF3FD1A9DDBF712425A6E`，33,990,144 B，Release x64，内置FG；源码 `8df406c4715db5e7ebc03c04764f47f67efe12cd`。BUILT_NOT_DEPLOYED |
| 验证 | 2428项输入构建前后不变；53必要NGX及全部948导出名称／序号一致。真实抽取10个生产方法／owner的106项CPU检查及六种运行负控、16套ZZZ／128接线、97项options接线、guide接线、7套核心含实际WARP回归通过；gpt-6.1-sol/high独立复核无剩余确定blocker。101条构建警告留在日志 |
| 正式部署 | 仍为1ABD9AFD／源码51fdb7b；正式与三缓存、原File ID／硬链接未改；2588项配置／profiles保护文件哈希一致，FSR桥仍CC301B42。未查询、启动、终止或操控游戏。current-deployment仍指旧正式字节 |
| 功能边界 | SR／NR／FG独立、用户6×参数、监视最新有效报告策略保留；缺实例凭据的enrichment仍unknown。native borrowed guide preview拒绝门未开放；HUDless/UI仍缺真实桥输入。退出、画面和性能分别验收，构建不证明卡顿消失 |

证据前缀 `build/repair-20261112/zzz-recordings-{source,tests,review,artifact}.json`；固定日志可复现分析 `analyze-zzz-recordings.py`，汇总 `evidence.json`，完整 `zzz-185668-analysis.json`。旧SelectedListCounts只匹配单handle的错误已由全handle解析取代，不再引用。1ABD9AFD候选及旧指针／构建日志已备份为同前缀previous文件与 `build/rollback/zzz-recordings-pre-1ABD9AFD.dll`；旧zzz-followup／repair-current证据保留。正式未改核验 `zzz-recordings-official-unchanged.json`，current-deployment原回执未改。

下一步由用户安排部署后，再按真实DLL占用原位写入或记.next待替换，保持硬链接与配置；随后用户手动复现。新局日志启动版本须匹配8df406c，关联 `F6 native guide recording lifecycle:` 的owner回收、当前源连续性、FG启停／最大延迟及实际显示节奏。不重复关闭FG对照。尚未宣称全部可见卡顿已解释或已修好。
