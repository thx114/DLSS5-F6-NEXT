# F6 当前状态

更新：2026-11-12（Asia/Shanghai，UTC+08）。唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，分支 `codex/f6-rewrite`。完整架构、代码导航、问题、命令与验收顺序见[项目交接](F6-HANDOFF.md)；参考来源见[独立仓库清单](../../../DLSS5_NEXT/docs/REFERENCE-REPOSITORIES.md)。

## 当前保存与候选身份

| 对象 | 当前事实 |
|---|---|
| 累计产品保存点 | `ea8c304c3810408e4ec8c7ca1378718cc37cd02a`，保留累计源码、生产shader、未跟踪测试及工具；后续交接提交仅涉及文档与忽略说明 |
| 候选 | **DD53514B已构建、未部署**；SHA256 `DD53514B481D4E2DCAAA5F9D95ABA269E1DEB8834CFDEB6BD3E62852AD526646`，33,959,936 B |
| 构建 | Release x64 exit0，39 warning records／0 errors，14项必要NGX导出齐全；文件时间UTC `2026-11-11T16:37:07.912677Z` |
| 安装 | 仍为**81DBE9A5**，SHA256 `81DBE9A53B90990FA06BF4CB3790B240B187AAF46841906C7D665E9C0FFF8875`，33,951,744 B；部署回执2026-11-11 22:09:48（UTC+08），4路径同一硬链接对象，7份配置未改 |
| 已有新局 | PID153992／164004已有日志及用户反馈；不能继续写“尚未启动新局”，但两局不证明未部署候选已加载 |
| 实际加载／画面／性能 | loaded-memory SHA未知；DD53514B未运行游戏，Look、持续细节、卡死恢复和延迟改善未验收 |

候选是在 `db0e646` 加当时改动上构建的原字节；事后提交不会改写内嵌版本。21项关键源码哈希与原构建回执匹配，不能拿新Git HEAD冒认旧候选内嵌commit。当前身份分别见 `build/current-{build,tests,artifact,deployment}.json`；部署回执仍绑定81DB，不能用现在同名out文件替代当时字节。

## 已完成与未完成

用户确认**7C281A54解决呈现NR＋FG明显卡顿和旧帧闪回**。保留共享source queue、真实Evaluate提前封存guides、同源最终颜色汇合及实际queue/fence/Reset退休。原神默认真实源帧呈现输入→NR→持续细节保留→FG；SR前／后入口保留对照。内部FSR桥和探测禁用，外部桥2.4 ABI保留；独立NR／FG方向与用户分辨率、层数、Look、历史及倍率不变。

DD53514B修复实际Look修改／查看最终效果被`debugView=2`遮住的问题；timer复用真实recording的queue/fence/value，不另建fence或Signal，GPU完成才读、真实Reset＋完成才复用。前3次及每64次dispatch低频形成7段GPU跨度，没有新增CPU等待或像素读回；不是独占GPU活跃时间。

持续保留已与旧间隔复用解耦，有独立容差、最终历史稳定滑条和每源帧调试窗；81DB已部署这些控件和生产shader。但用户仍报告全屏拒绝、转镜头发白／噪声变化及卡死，**像素效果仍未通过**。SR J/K/L/M与FG A/B/C已接配置，实际模型加载须验；HUD-less/UI标签已准备，外部桥生产回调和alpha-only ABI未接，无输入时标UI未保护。

最新关联回归整体exit0：Look控件80、生产HLSL/WARP392、timer WARP668、18个真实历史runtime场景、FG handoff15场景／564检查、呈现输出1596检查及shared/pacing/Reflex/options/热重建等。错误颜色域负控按预期24项像素失败。离线夹具不执行NGX／游戏，不证明画质或提速。

## 性能与撤回事项

[性能审查](F6-PERFORMANCE-REVIEW.md)记录PID153992的166.103ms报告，以及PID164004的317对NR total/model中位39.630/31.759ms、逐对模型外差值中位5.288ms。已核copy reader等待83.962／98.681／22.469ms，另有hidden Present101.810／92.761／173.323ms；普通interop本局只有1个共享颜色slot，不能只轮转3个allocator或删除真实wait。包含区间不相加，稀疏样本不当整局平均，160ms唯一根因和实际改善**未闭环**。

此前误用输入描述符制造36项“旧色”失败的夹具已修正，相关根因及“省一次生产复制”声明已撤回。当前24次Look resolve均为原始模型预览是另一条明确证据，不证明全部残影根因。旧日志的其它模块也不能解释用户确认只加载OptiScaler的特定会话。

## 接续边界

本次只整理报告和Git，未重构算法、重建、部署、改profiles或操控游戏。问题检索／独立复核按用户最新要求使用当前会话新建的**gpt-6.1-sol / high**，不再用DS；核心由主会话核实。

下一步按原参数分组验SR、SR＋NR、SR＋FG、SR＋NR＋FG，每组预热5秒、采30秒，分别记源率、SDK累计区间、真实display事件及Reflex合法端点；无真实显示事件则显示率未知。持续历史关闭／开启／最终稳定分开，原神通过后星铁、ZZZ各自回归。详见[完整报告](F6-HANDOFF.md)、[诊断操作](F6-GUIDE-DIAGNOSTICS.md)与[HUD合同](FG-BRIDGE-HUD-CONTRACT.md)。

`build/`还保存候选、回退、原始日志及回执，虽被Git忽略仍须保留。旧候选详情已在保存点`ea8c304`的本页及[运行状态](F6-STATUS.md)、[历史运行研究](F6-RUNTIME-REVIEW.md)留存；旧根、其它工作树不删除，不把迁移基线或旧验收冒充当前。共享研究616项CPU测试本次通过，存于主树；本轮不以Git干净或构建成功代替游戏修复。
