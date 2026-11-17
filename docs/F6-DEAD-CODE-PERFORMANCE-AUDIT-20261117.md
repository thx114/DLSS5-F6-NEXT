# F6 精简后的死代码与性能审计（2026-11-17）

本审计基于第三批精简后的活动代码。静态可确定的是额外操作及责任边界；没有新游戏 CPU/GPU 采样，不能把以下项目认定为卡顿、排队或高延迟的实测根因。

## 本轮已处理

DeferredSR 的独立 timestamp/resolve 退休机制已接入公共 recording token；关闭实验链产生的状态恢复包络、FG `_resourceFrame` 写入、重复时钟/哈希/消费者查询、无消费者 DrawUiMask FG 捕获已去除。保留 SR/NR/FG 真实资源身份、所属 queue/fence 和 recording 关闭条件。详见[第三批报告](F6-DEAD-CODE-CLEANUP-3-20261117.md)。

## 活动路径的后续优化

| 位置 | 静态确认的额外工作 | 建议与必须保留的语义 |
|---|---|---|
| `framegen/IFGFeature.cpp`、`IFGFeature_Dx12.cpp` | 固定资源种类仍使用 hash 容器；每帧 clear 后重新插入节点 | 改为按 ResourceTypeCOUNT 索引的固定槽与独立 presence/ready 位，保留锁和当前帧缺失状态，不能把空槽当有效输入 |
| `shaders/dlssnr/DlssNr_RecordingLifetime.h` | 每个新提交 token 建 fence；每个 shader pass 的 Recording 先扫描 Collect，再查已有 token | 在真正 queue timeline 上复用 fence；dispatch 边界取得 token 后向下传递。保留 post-Execute Signal、Reset/discard、重放隔离、跨队列与设备身份 |
| `shaders/dlssnr/DlssNr_GpuTime.h` | TakeCompleted 把 fresh 与空 vector 交换，消费后销毁，丢失容量 | 有序 callback 消费后 clear 保留容量，或在既有上限内使用固定缓冲。不能丢样本或省略 fence/recording 退休 |
| `upscalers/IFeature_Dx11.cpp` | 每次 Evaluate 动态构造最多三个固定后处理阶段，使用 vector 与 std::function | 使用栈上固定阶段描述，保留反向布置输入、正向执行、当前 resource ownership 和失败传播；全部关闭时本来没有该 vector 分配 |

以上四项尚未实施，收益需要在相同配置、当前加载身份和当前帧时间线上分别采样。不能通过扩大池、去掉等待或复用旧帧代替优化。

## 可维护性与并行开发边界

plain/wrapped DXGI 两条入口都有实际消费者，不能作为死代码删除。它们在 size-to-window、overlay 判定、descriptor 准备、adapter 枚举和 recursion 状态方面存在重复及行为差异。后续应统一描述符策略、路由判定、创建结果发布和 adapter 选择；入口保留各自的原始 trampoline/COM 调用及 identity。需要两路完整创建路径的对照测试，现有 shared suites 尚不足以证明等价。

GIMI 的旧 split Prepare/FinishInvocation 协议与 AcquireShaderSource 闭包仍需在 MI 的最终事务实现中核对并收敛。MI 正在并行修改该模块，本轮只保存已部署基线，没有把未完成 WIP 或未经共同集成的删除放进提交。

ResTrack 的活动 HUDless tracking 不是死代码：draw 从当前 binding 取候选，`Hudfix_Dx12::CheckForHudless` 把实际 HudlessColor 交给 FG。persistent binding、descriptor lifetime、NR Reset/Execute hooks 均保留。移除 DrawUiMask 未接通 FG 支线不等于修好了用户报告的 HUDless 问题。

产品已知问题继续保留：FG/NR 残影、HUDless、GPU 排队、跨深度细节残留、SR/NR 耗时与延迟、计时缺陷，以及 GIMI/ReShade/XeFG/跨厂商 NR/UI 计划；不能以本轮构建和离线测试代替逐游戏、设备、功能验收。
