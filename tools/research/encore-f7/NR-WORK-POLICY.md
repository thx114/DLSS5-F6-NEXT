# Encore NR 工作尺寸稳定与 reset 标志

2026-11-07，北京时间。固定目标 SHA256 及路径见 README；地址均为 RVA。
本篇只恢复所列选择块与状态写入，不运行 Encore、SDK 或 GPU。

## 工作尺寸选择不是纹理或历史复用证明

`0x15706d..0x157137` 位于候选 work 尺寸计算之后、后续资源与 feature 准备之前。
runtime 字段为：当前 feature `+0x158`、其 work 宽高 `+0x170/+0x174`、
候选宽高 `+0x180/+0x184`、uint32 计数 `+0x188`、上次 opaque key `+0x190`。
当前调用的 key 从输入对象 `+0x60` 读取；其 producer 语义没有闭合，不称真实帧号。

选择逻辑如下：

1. 没有 feature，或请求 work 等于已有 feature work：选请求，清候选和计数。
   此路径不更新上次 key；不能把清零扩展为清整个状态。
2. 两者不同时，候选变更先清计数；本次 key 与上次不同才更新 key 并递增计数。
   同 key 重复调用不计，`A→B→A` 每次变化都计；key 不要求单调或非零。
3. 递增为原生 uint32 运算。`0xffffffff+1` 回到 0，随后用无符号比较判断是否达到 15。
4. 计数达到 15，选新请求。未达到时，还须比较 runtime `+0x1c8/+0x1d0/+0x1d8`
   与当次 Output 的 GetDesc 宽度、高度、格式；分别为 64、32、32 位比较。
   三者全相同才暂选已有 feature 的 work，任一不同立即选新请求。

这不是固定等待 15 次调用或 15 个真实帧；候选变化可重新计数，重复 key 可持续保持。
选择为新请求后，这个块不自动提交 feature、不清候选；后续实际创建成功等路径另行更新。
`0x157f86..0x157fc2` 的成功创建路径记录 work 宽高、preset，设置 reset 标志，
并清候选计数；其它 preset、warp、设备或配置条件仍可能使 feature 重建。
当前 feature 的存在、输入已准入以及缓存 tuple 的有效性，均由调用方提供。

`nr_work_policy.py` 提供不可变 `WorkPolicyState`、`WorkSize`、`OutputDesc` 与
`select_work_size`。支持上游已经准入的 work 各轴 64..16384；OutputDesc 保留完整
uint64 宽、uint32 高和格式，只比较字段，不认证真实纹理。函数只返回 selected work、
下一候选状态和 heldExisting，不持有资源、不更新实际 feature 或输出描述。

```python
from nr_work_policy import OutputDesc, WorkPolicyState, WorkSize, select_work_size

output = OutputDesc(2560, 1600, 28)
state = WorkPolicyState()
decision = select_work_size(WorkSize(1536, 960), WorkSize(1920, 1200),
                            output, output, change_key=1, state=state)
assert decision.heldExisting
assert decision.selected_work == WorkSize(1920, 1200)
assert decision.next_state.pendingCount == 1
```

示例是合成值；没有把选择结果提交为 feature，也没有读写任何纹理。

## 补查：创建准备与局部复用门禁

0x157bdf..0x157c31检查已有feature；work宽高（runtime+0x170/+0x174）或
preset（+0x178）不同就调用0x1569b0清理feature，再进入创建。三字段均相同的
分支可跳过这次Create，但它不是完整的设备、session、guide或生命周期准入。
更早的0x15703b..0x15706d还检查option byte+0x80及两轴packed字段+0x118/+0x130，
命中时提前清理feature并请求reset。因此不能把“同尺寸/preset”当作全局复用许可。
option的最终配置含义仍未闭合，不给它猜一个用户设置名称。

创建时先清参数容器，调用0x15bdf0..0x15bf09准备如下字段：

| 字段 | 固定调用链中的准备值 |
|---|---|
| Width/Height、OutWidth/OutHeight、DLSSNR.Width/Height | 同一组已选NR work宽高；不是由Out前缀推定最终显示尺寸 |
| DLSSNR.ScalingRatio | double 1 |
| DLSSNRComputeScalingRatioCallback | 指向本Encore模块的0x15a240，作为pointer槽写入 |
| DLSSNR.Hint.Render.Preset | 从配置字段+0x108经0x35a390映射；本批不声称映射已完整恢复 |
| PerfQualityValue | numeric 0 |

随后0x157c50..0x157c72取本次frame flags，与0xffffffbe相与，再经signed32到
double转换写DLSS.Feature.Create.Flags。只记录原始掩码与转换，不把未知bit含义
或这个固定调用点的值推广为所有NR版本的要求。

本地回调0x15a240..0x15a272在参数指针为空时返回0xbad00005；非空则通过
参数vtable+0x30写float DLSSNR.ScalingRatio=1，再返回1。它不读取
PerfQualityValue，也没有向模型查询默认缩放比。该callback指针与初始ratio都是
Encore主动提供的策略，不是载荷已发布能力或实测输入尺寸的证据。

独立历史CDF参考的PopulateParameters_Impl发布了另一实现：它按PerfQualityValue
区分ratio，详NR-PAYLOAD-ABI.md。两个实现不能混称为“模型默认都返回1”。
F7当前NrDx12的core路由只准备另一套参数集合；这个静态差异不能单独解释其
scratch查询拒绝或证明缺少这些字段就是bug。接入前仍需核选定payload的实际
读取、callback ABI、颜色/guide域及同DeviceOwner的GPU完成。

历史CDF内部Create的限定正文已另核五个直接Get：NR专用宽高、preset和两个
node mask；F7当前已设置这五项。该正文还在直接Get之前将参数对象交给另一
未知方法，不能据此断言其它参数不被间接读取。选择表数据不计作机器指令，
精确返回码、fallback和代码/数据边界见NR-PAYLOAD-ABI.md的内部Create节。

新原始证据为runtime/nr-feature-{format-reset-gate,reuse-gate,creation-prepare,
common-parameters,local-scaling-callback}.json和nr-creation-contract.json，
共五个新增窗口119条指令/40锚点、11个ASCII key及float/double两个常量断言。
本篇没有提供私有callback移植实现或执行任何创建/重置/释放。

## reset 的可见状态写入

`0x158b26` 读取 runtime byte0，与 incoming reset 做 OR，作为 DLSSNR.Reset。
`0x158b30` 在 Evaluate 前把 byte0 置 1，因此它不是 GPU 完成标志。
`0x158dc9` 将返回码与 `0xfff00000` 相与，BAD 族绕过解码写回；
`0x158e90..0x158e9b` 将 BAD 族比较结果同时写 byte1 和 byte0。
这条正常返回路径中，非 BAD 结果清两字节，BAD 结果保留 reset 请求。
这些是原生实现的分类规则，不认证每种非 BAD 数值为受支持 SDK 成功码。

失败且有 warp 时还存在另一条路径：`0x158f5e` 置 `+0x148`，清 byte1，
调用 `0x1569b0`，把两个 packed axis 恢复为禁用值并再次置 byte0。
helper 清 feature 指针、宽高以及共享 holder，并可能调用 holder 析构虚函数；
`0x159413` 随后自调用 NR 入口，传入强制标志 1。
这里没有恢复 holder 析构内的完整 GPU 退休证明，不能把这条重试照搬为 F7
提交失败、设备故障或 Signal 失败后可以销毁、重试或借旧帧的许可。

## F7 可采用的方向与边界

工作尺寸候选变化时，可在控制路径先比较整数状态，避免把短暂候选抖动直接变成
每次都重建 feature 的要求。采用旧 work 尺寸仍须对**当前 ticket 的颜色、depth、MV**
重新映射，使用当前输入；它不代表旧帧、旧 guides 或已退休资源可再次使用。
真实资源重建与释放仍须遵循 F7 同 DeviceOwner/session 及所属 queue/fence 证明。

F7 当前 `NrDx12` 已单列 needsReset、记录、执行提交和实际 fence 退休；本篇不把这些
状态合并成 Encore 的 byte 标志，也不把其实现判为缺陷。NR 生产接入仍未完成。
只有实际接入存在 work 尺寸频繁变化时，这个策略才有可能减少创建抖动；固定尺寸
场景没有据此推导出的收益。需分别测创建次数、停顿及同条件整帧 GPU 时间。
本批不增加产品每帧扫描、日志、SDK 调用、等待或池容量。

静态原件在 `evidence/runtime/nr-work-hysteresis.json`、`nr-created-identity.json`、
`nr-feature-reset.json`、`nr-reset-outcome.json` 和两个 `nr-warp-*.json`；生成入口是
`rebuild_evidence.py --profile runtime`。manifest 绑定目标哈希，锚点记录实际指令字节。
38 项独立 CPU 回归覆盖上述整数决策、阈值、重复 key、字段变化及支持域拒绝；
完整 CFG、producer key 语义、SDK 内部历史、GPU 安全/性能与画质仍未认证。
