# DLSS5_NEXT 当前状态

更新：2026-11-12。**ADFF3486 经用户实测仍有ZZZ间歇性卡顿；1ABD9AFD诊断版已原位部署并核验。监视器旧报告选择已修正，新增native guide阻塞原因记录；真实卡顿根因及改善未证实。HDR暂缓。** 详见[产品当前状态](../../DLSS5_NEXT-workers/f6-rewrite/docs/CURRENT-STATE.md)和[交接第0节](../../DLSS5_NEXT-workers/f6-rewrite/docs/F6-HANDOFF.md)。

唯一产品写树 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite`，分支 `codex/f6-rewrite`，本次源码提交 `51fdb7b`；本根 `D:/CODE/DLSS5_NEXT` 为 `codex/navigation-20261112` 导航／迁移基线。GitHub目标 `thx114/DLSS5-F6-NEXT`。

新固定日志PID190880（版本b1ee210）记录160次SDK插帧状态切换、159次最大呈现延迟变化，包含缺源后eOff／6→1及下一帧恢复eOn／1→6。原节流日志不能区分Reset未完成与真实producer fence未完成。新日志 `F6 native guide reuse blocked:` 只观察，保留原拒绝／退休门；不扩大池子、借旧帧或修改6×参数。Reflex选最新有效完成报告，缺实例凭据的Bridge／NR enrichment为unknown，独立NR计时链未改。

Release x64通过，2428项输入构建前后相同，53必要NGX及全部948导出一致，定向回归和独立复核通过。诊断SHA256 `1ABD9AFDDD605B3D55EF3466EDBDD7946DCA6FD8B362A4C2030A4FCC61DE9ABC`。正式及三个游戏缓存一致，原File ID／硬链接、2,535项受保护文件及profiles不变；桥保持CC301B42，旧ADFF3486已备份。退出保护及完整英文保留，HDR暂缓，HUDless/UI仍缺真实输入。

当前证据在产品树 `build/repair-20261112/zzz-followup-*`，部署核验 `zzz-followup-deploy-verified.json`；旧 `repair-current-*`／ADFF3486证据保留。用户手动启动ZZZ并复现后，再读版本51fdb7b的新局日志；不重复要求关闭FG对照。未查询或操控游戏；新版本性能、画面、退出与英文布局分别验收。旧根、其它工作树及build候选／回退／证据保留。
