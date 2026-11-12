# F6 当前状态

更新：2026-11-12。**用户确认 ADFF3486 的 ZZZ 间歇性卡顿仍在；1ABD9AFD 诊断版已原位部署并完成磁盘核验。新版本修正 Reflex 旧报告选择并增加原生 guide 阻塞原因遥测，卡顿根因及改善尚未证实。HDR 暂缓。**

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，分支 `codex/f6-rewrite`；本次源码提交 `51fdb7b19f09`。导航树 `D:/CODE/DLSS5_NEXT` 只维护导航与迁移基线。GitHub 目标 `thx114/DLSS5-F6-NEXT`。

| 项目 | 当前事实 |
|---|---|
| 最新用户实测 | ADFF3486 下 ZZZ 仍周期卡顿，监视读数约70降至接近0时卡住，随后回升；此前关闭FG后不卡。旧锁环修复未解决本次症状 |
| 固定会话 | `build/repair-20261112/zzz-190880.log`，PID190880，日志版本b1ee210；10,761,256 B／123,177行，SHA256 C93B42515ADD8A047B64A1C1ECCCDB2B4A6DDD166B0FE41744B101511D3191DE。loaded-memory SHA未知 |
| 已观察链路 | selected Evaluate 2756、FG Dispatch成功2573；160次SDK插帧状态切换、159次最大呈现延迟变化，其中1→6及6→1各79次。例源frame2392槽忙后eOff／6→1，frame2393成功后eOn／1→6；时序相关，不等于根因证明 |
| 尚缺证据 | 原拒绝日志各20条且节流；不能区分未Reset与真实producer fence未完成。6条源list及4槽不足以证明池容量缺陷；没有扩大池子或借旧帧 |
| 新增诊断 | `F6 native guide reuse blocked:`，首次三条、之后按需至多每秒一条。记录slot/list/queue/fence/target及reset_pending等逐窗原因；计数跨slot且重叠，身份属于最后尝试，fence采样在拒绝之后，只观察，不参与退休 |
| 监视修正 | D3D／Vulkan固定选最新有效完成报告；过期清空，缺失／非嵌套保持unknown。Bridge／NR与Reflex缺报告实例关联凭据，enrichment保持unknown；独立NR计时链未改。渲染队列跨度和GPU开始前等待分开，读数不代表物理队列占用 |
| 保留功能 | 内置FG、独立SR／NR／FG、既有退出入口保护和完整英文保留；退出正常性及英文布局仍待各自实测。HUDless/UI仍缺真实桥输入，见[输入契约](FG-BRIDGE-HUD-CONTRACT.md) |
| 当前诊断DLL | `build/out/OptiScaler.dll`；SHA256 `1ABD9AFDDD605B3D55EF3466EDBDD7946DCA6FD8B362A4C2030A4FCC61DE9ABC`；33,989,632 B，Release x64，内置FG，无新增ASI |
| 本次验证 | 2428项输入构建前后不变；53必要NGX导出及全部948导出名称／序号不变。原生通知29项及旧锁环负控、ZZZ16套／107接线、options95项、Reflex策略58项／真实方法抽取31项／queue12项、5套核心定向回归通过；gpt-6.1-sol/high独立复核无确定blocker。构建警告保留于日志 |
| 正式部署 | 正式与三个游戏缓存均为1ABD9AFD，原File ID／四个硬链接不变，2,535项受保护文件与profiles不变；桥保持CC301B42。ADFF3486已备份；未查询、启动、终止或操控游戏 |

本轮证据集中在 `build/repair-20261112/zzz-followup-{source,tests,review,artifact}.json`，部署核验 `zzz-followup-deploy-verified.json`；`build/current-{artifact,build,tests}.json` 指新诊断候选，`build/current-deployment.json` 指已核验的1ABD9AFD。旧 `repair-current-*`／ADFF3486回执保留原身份；ADFF3486回退在 `build/rollback/zzz-followup-pre-ADFF3486.dll`，部署时也另存原字节。

下一步由用户手动启动ZZZ，保留现有FG参数，在同场景运行约30秒并复现卡顿；读取日志版本51fdb7b的新局固定日志，关联新增busy原因、源帧／SDK帧及启停时序。不重复要求已完成的关闭FG对照。尚无新版本游戏验收，不能把监视修正或离线构建当作卡顿已修复。保留全部真实queue/fence/Reset门和用户配置。
