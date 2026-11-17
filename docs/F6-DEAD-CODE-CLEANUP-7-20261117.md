# 第七批死代码清理

2026-11-17；父提交 `8e5d37c2358cebb5ad38ca00ebf3fac6157f166c`。本批只删除经调用端核实的无消费状态／函数，与公共 SR 输出修复在同一冻结候选中验证。

| 删除闭包 | 保留的实际行为 |
|---|---|
| LowLatencyTech 的 effective_fg_state 字段／虚函数、四个 override，以及 fakenvapi forced_fg／fg helper 和调用 | 实际 set_fg_type、低延迟输入转发和真实 SDK 调用 |
| 仅用于上述状态的 12 帧重复 ID 历史、reportFGPresent 的纯赋值及相连旧注释 | AntiLag2 reportFGPresent 中 context／PrivateData 的设置与清除 |
| XeLL 对象内从未调用的 o_xellGetVersion、o_xellSetLoggingCallback 成员及两种宏下的赋值 | XeLLProxy、实际 hook 转发，以及仍被真实调用的 forced_mode |
| NativeProducerCpuTiming::Frame 未使用的 Id()／Sampled() getter | CPU timing 字段、采样及公开计时语义 |

没有按同名符号全局删除：`hooks/Xell_Hooks.*` 中同名函数指针仍有真实转发调用，继续保留。没有移除任何后端、资源／queue／fence 退休或活动 hook gate。

最终候选 Release x64、10 套定向公共回归、6 个 adapter 的有效结果和 24 项实际 SR SDK 宿主通过。生产 delivery／选择／重建回归现为 77 项；shutdown fixture 先前的缺 stub 编译失败及对应修复、预期失败负变异单独保留证据；不将失败静默改计通过。完整 shared 回归显式跳过，未部署、未游戏验收。

精确输入、diff、构建、回归与总回执在 `build/backend-output-20261117/`，SR 修复及验收边界见 [输出与选择报告](F6-SR-BACKEND-DELIVERY-20261117.md)。本批属于源代码条件分支／状态闭包清理。已检查本地 Git 分支及 worktree 归属；活动工作树分支、尚有独有提交的分支和代码回退入口不据此删除。
