# Execute 与录制代次的提交边界

2026-11-15 代码审计。本文记录下一项实现闭包；**尚未修改公共提交链，也没有证明它是旧局高延迟或残影的根因**。用户已选择稍后手测，代码工作继续。

## 已确认的缺口

`ResTrack_dx12.cpp` 已有 pre → 实际 Execute → post，但 `FinishedPictureSubmitting` 的凭据目前只服务 `GuideCapture`。NR 的 `Registry::Submitted` 在 post 按当前 `open_[list]` 查找代次。

实际 Execute 已发生、另一个线程成功 Reset、随后 post 才到达，是合法的 CPU 时序。此时旧 token 可被标记为 discarded；如果同一 list 已开始新一代录制，旧 post 还可能命中新 token。只在 profiler 中增加 `Complete()` 检查，不能修复公共凭据本身的错配。

当前 `DlssNrKernelProfile.h` 的额外缺陷是等 90 次 evaluation 就 Map 并清 pending。帧数不能证明 GPU 完成，旧可执行 list 也可能重放。它只在 KernelProfile 开启时使用，不能用来解释未确认该选项状态的性能问题。

## 实现约束

1. pre 必须固定实际 queue、真实 list、具体 `Use`、generation 和执行 serial，并在真实 Execute 前登记承诺执行。post 只能完善这一捕获，不再按当前 list 找代次。
2. token 在 pending execution 非零时不能 `GpuComplete` 或 `Complete`。成功 Reset 只关闭旧录制，不能把已承诺执行标为 discarded；Reset 失败不关闭代次。
3. 多层包装入口和真实 hook 已会嵌套 pre。外层凭据与 hook 内凭据需要同一次调用的明确归属，不能误报 replay，不能依赖唯一裸指针或把所有本地 submitted 提前设为 true。
4. post 在真实 Execute 返回后 Signal 实际 queue/fence；缺少 post、Signal 失败、重复执行和 device removed 保留隔离，不能把一次 fence 完成当成所有未来重放的完成。
5. captured post 的收尾独立于当前 watcher。Reset 删除旧 watcher、创建新代次、逆序 post，都不能修改新代次状态。
6. 不跨实际 Execute 持有全局 NR 锁，不新增渲染线程等待、flush 或等空槽的轮询。沿用 `g_nrMutex → Native mutex` 锁顺序，NVAPI launch 回调不反向取 NR 锁。

## 必须同步的消费者

| 消费者 | 当前缺口 | 必须闭合的责任 |
|---|---|---|
| `NrGpuExecutionLifetime` / `DlssNr_RecordingLifetime.h` | post 查当前 open list；Reset 可先 discarded | 捕获实际 Use、执行 serial、pending 与实际完成 |
| `DlssNr_OrderedLifetime.h` | 本地 submitted=false 就清 pending/commands | 保留已承诺记录，按捕获代次安装回执；DetailReuse 忙碌判断覆盖未完成 Use |
| `DlssNr_DetailHistory.inl` | 同上，历史发布可能丢失 | 保留已有 OwnersBusy 和真实退休条件，不把逻辑发布当可释放 |
| `DlssNr_GpuTime.h` | resetNotified 拒绝迟到 post，新 sample 可被旧 list 命中 | 按 Use/generation 接真实公共 fence；不另发 Signal |
| `DlssNr_DetailReuse.inl` Coverage | Reset 提前清 pending，Signal 失败也清槽 | 绑定录制与执行代次；失败保留未知完成；不要再只靠独立 fence 的首次完成复用 |
| `DlssNr_SceneCut.inl` | readback Reset 清槽、post 裸 list 匹配 | 同代次关闭及所有实际执行完成；保留隔离 |
| `DlssNrSceneCut.h` model reset receipts | 旧 post 可确认新 attempt | 捕获具体 attempt/pass 与录制、执行代次 |
| `DlssNr_Late.inl` capture slots | Reset 可回退 done；旧 post 可 Signal 新 ready | 固定 slot/serial/ready/fence；不得撤销已承诺执行 |
| `DlssNrCurrentOutputCapture.h` | 后台可在本地未提交分支释放 source/list/allocator | capture Job 自有 pre/post 与锁内状态；后台仍不无锁读 NR token |
| `DlssNrKernelProfile.h` | evaluation 数冒充完成；临时 queue 取频率 | 若保留 GPU 采样，query/readback 由同一 Use 持有，真实 queue 取频率，完成并成功 Reset 后复用 |

这不是给各消费者各补一个布尔值。实现时应收敛公共录制/提交身份，再让独立 GPU owner 复用它；必须保留不同消费者的实际 fence、发布、读回和后台锁责任。

## 针对性验证

- `pre → Execute → Reset → post`，以及 Reset 后立即为同一 list 创建新代次。
- 两次执行的 post 逆序、重复 post、外层 fallback 与 hook 嵌套、超过 16 个 list 的批次。
- 实际 GPU 未完成、Signal 失败、Reset 失败、取消未执行录制、可执行 list 重放、device removed。
- 对每个消费者分别断言 owner 不早释放、槽不早复用、旧 post 不修改新代次。现有 GPU-time/DetailHistory 生命周期夹具不覆盖完整的跨消费者窗口，不能代替这些检查。
- Vulkan 无 DX12 cmd/Use 的调用不能读 DX12 token；DX11、DX12、DX11-w-DX12、Vulkan 和 Vulkan-w-DX12 编译闭合。

先完成这一公共边界，再将内核 GPU 采样接入。有限 SR 生产队列拆分仍须单独闭合输入资源、BeforeSR filter、guide 发布、实际 source queue/fence 与 SDK buffer 写入，不能从本文直接推导已经允许拆分。
