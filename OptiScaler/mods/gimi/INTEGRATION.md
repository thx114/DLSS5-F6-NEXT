# R478 接入文档：FSR 可复用项、额外依赖与 draw 缓存

本轮只写语言/Plan/Commands，不改现有 hook。产品接点以 R477 读取的 dirty 源 `work/r294-r266-build/OptiScaler` 为定位依据；并行稳定性修复后要重新按具体 hunk 接入。源码存在不代表本轮用户 PID 已装该 hook。

## 从现有 FSR/深度路径能拿到的

| 能力 | 现有位置 | GIMI 可复用范围 |
| --- | --- | --- |
| DrawIndexed trampoline 与参数 | `resource_tracking/GenericDepth_Dx11.cpp` 的 `hkDrawIndexed` / `o_DrawIndexed` | 保留 IndexCount/StartIndex/BaseVertex 和原调用，首片限定已认证 immediate context/精确单部件 |
| Before/原调用/After 配对 | 同文件 Draw 包装、`genshin/GenshinLegacyCapture.cpp` | 复用 owner/配对，接入真实 MOD consumer；MOD 开启下一次 eligible draw 生效，不能被 SR count/PS 早筛抹掉 |
| shader 分类缓存思路 | R459 已有 CPU 分类缓存 | 借用代次与事件失效设计，不把 FSR hash/地址直接当 GIMI identity |
| internal draw 防重入与游戏/device/context 资格 | 既有 Bridge/ReturnedRaster 调用边界 | 真正 MOD draw 走现有 internal 抑制；不另建 proxy/loader 或复制一套 Bridge |
| IA/VB/IB 状态读取 | `genshin/GenshinReturnedRaster.h` | 已有读取接点；包括格式/stride/offset，不能只存指针 |
| PS 资源读取与特定恢复示例 | `genshin/GenshinDepthMapping.h` | 复用窄 getter/owner 思路，特定深度 replay 不是通用模型执行器 |
| Map/Unmap/Update/Copy/clear 等观察 | GenericDepth 的 writer/失效接点 | 接 identity 更新/失效事件；现有观察不是已实现的 GIMI hash 生命周期 |
| 既有 CPU hook 低开销统计 | R448 现有开关/聚合路径 | 如后续需要成本统计复用这里，不做每 draw 日志或新诊断框架 |

因此首片可仅匹配部分真实 DrawIndexed，不要求拦截所有 draw。但资源生命周期事件和必要 shader 身份仍必须准确。其他 draw、instanced/indirect/deferred、未知设备/游戏原样 forward；不能因此称全角色/LOD/阴影完整兼容。

## 下一阶段实际要补的

| 模块/接点 | 需要完成的产品行为 | 消费 R478 的接口 |
| --- | --- | --- |
| Resources/Identity | Load 时文件解码、格式/布局校验、immutable GPU 创建；精确上游 resource/shader hash、descriptor/内容代次、复制传播、未知写失效与 lease owner | `Resources`, `Overrides`, `Candidates(hash, shader)`, `OverrideForProgram`, `InitializeResource` |
| DX11 binding cache | 认证 context 当前已知绑定/代次；按精确槽位返回稳定 lease，未知不能当 null | `CommandQueries::readBinding`、`Binding` |
| Identity/filter matcher | 资源/首索引/范围/优先级/重复 shader hash 规则，ORFix shader capture 触发；不得只按 count 匹配 | `filterIndex`, `textureMatches`, `ProgramInvocation` |
| DX11 executor | 整 tape preflight、状态/hazard 保存恢复、按序 VB/IB/PS 绑定与明确 DrawIndexed、internal 防重入、真实 skip/原 draw 边界 | `PrepareDraw`, `BatchView`, `Action` |
| 参数资源 | `x70` 等 CPU 值和真正 shader 可见性；无兼容 GPU 参数机制的路线明确 Unsupported | `SetParameter` action；不能忽略或每 draw upload |
| Runtime | 默认关、手动选择、帧外发布 immutable Plan、真实 consumer 代次、关闭/设备重建后的 owner 保留和 GPU 退休 | Plan/state generation；原 controls/owner 接点 |
| 工程接入 | 以后由明确 owner 将本目录 cpp/h 静态编入原 Opt DLL | 本轮没有改 vcxproj/Config/菜单 |

Map/Copy 的完整内容未知、晚挂接旧资源无 CPU 初值、GPU-only 写入或未知 descriptor 时必须标 Unknown，不从地址/相同 draw count/旧 hash 猜匹配，不同步 readback。R478 提供 hash 元数据，不实现其算法；资源阶段须读取真实 `ResourceHash.cpp/.h` 及创建/更新/复制语义。

## draw 缓存契约（接入时必须实现）

禁止把 parser、依赖解析、身份重算、文件读取或全库扫描放进 draw。静态资格使用有界直接映射/固定容量缓存，碰撞仅淘汰，不能误命中。记录失配亦可缓存，但 Unknown 与确定无匹配分开。

建议缓存键：Plan/publication 代次、MOD consumer/启用代次、game/device/context 代次、hook/draw 类型、当前相关 VS/PS identity 及 binding 代次、IB/VB/被查贴图的 lease/content/descriptor 代次、IndexCount/FirstIndex/BaseVertex、首片 layout/topology/骨骼兼容资格。只缓存匹配到的短 program IDs 和静态资格。不要缓存裸 COM 地址当永恒身份，不能沿用另一 PID/device/context 的结果。

失效由已有创建/Map/Unmap/Update/Copy/clear/销毁事件、shader/IA/相关资源重绑、设备重建、上下文不透明边界、Plan/配置/consumer 切换推进代次；未知或部分写使旧内容 identity 立即失效。禁用路径在解释器/资源锁前早退。consumer 即时切换不能等旧负缓存过期。deferred/opaque 边界继续 fail closed。

缓存不冻结 `$` 变量、局部作用域、资源 alias、参数值、分支、真实当前绑定或 post 行为；它们由已编译 IR 在本次 invocation 计算。`CommandState` 内已有每批 lazy binding/parameter cache：同一 binding 未被 tape 改动时只 query 一次；写入后更新模拟值，后续表达式/Check 看新绑定。Check 的 provider 可复用精确身份的外层资格缓存，返回编译好的有界 ID 序列。无每 draw hash/IO/readback/upload/日志/增长分配。

`PrepareDraw` 只对原 draw 不改变所查询 identity/绑定资格的稳定源可用；shader 会写入的 GPU-only identity 不符合此首片。它在 GPU 前将所有 PRE/POST 解析为 action tape，但不是 GPU 执行证明。整 tape 失败不 skip；发生部分提交后不能补跑原 draw 伪装恢复，交真实 owner 恢复/隔离/退休。

## 不能沿用的推论

FSR 的“PS 已分类”“缓存 ready”“count=3”不代表角色 Resource hash 已知、骨骼兼容或 MOD 可画；DrawIndexed 数量不证明骨骼/LOD/透明/阴影。现有 CS 为主的 `Dx11BridgeStateGuard` 不覆盖 GIMI 所需全部 IA/PS/hazard。MOD 必须在真实替换绘制后给现有深度/FG/NR观察记真实几何；保原 SR 后 game RTV copy 与 GPU fence/retire/quarantine，不能破坏用户已有60帧基线换功能标签。

有界参数路径现由 `GimiIniParams.h`、`GimiResources`、`GimiDx11Executor`、`GimiRuntime` 接续：有序静态初始化与有限域闭包 → immutable Texture1D/SRV 预创建 → 整批参数 action/page 预检 → PRE/POST 顺序绑定及真实原绑定恢复 → 成功提交才提交 CPU 页面状态 → 既有真实 fence/quarantine 保留 owner。draw 不上传/Map/readback，不缓存动态参数或 tape。不再以人为 8 槽限制写入标量；所有声明写入及单值域保留，raster 全有限积与真实 native overlay reader 的 z=0/w=1/x70=1或2 投影合并为最多 512 个完整状态键。shader/resource filter 按真实查询类型分域，this 保守取并集；native 一次匹配整个 image 并核全部未写默认值，不为逐标量拼页制造不可达中间页。原 proof 与预加载字节预算覆盖闭包、键表及 GPU 页，4096-float4/t120 ABI、signed-zero、完整图拒绝和参数 touch mask 保留。

R1006 保原始 VS/PS 未使用 t120 的真实反射事实，另由最终字节码和真实 CreatePixelShader 发布窄 replacement capability；不按 Texture1D 类型或名称猜测外来 shader ABI。R990 已把无 Pattern、仅 shader_model/命令的完整 ShaderRegex group 接入 Plan，并在既有 ShaderCreated 反射模型、Present/PreparePipeline 处按原身份/代次准备不可变链接。原 ShaderOverride 捕获先于按 canonical group 顺序追加的命令；同 draw 仅提交 VS PRE → PS PRE → 原 draw/skip → VS POST → PS POST 的有界列表。TextureOverride 只经真实 check 执行，选中组件必须确实被 tape 到达；不能由创建/绑定或名称自动执行。Skin 的检查顺序来自源命令，不硬编码部件。原 shader filter 保留，模型链接默认分类为1，已绑定无匹配为+0，空 PS 为-0。

全部显式加载文件/include 进入 required 声明闭包；Pattern/Replace/InsertDeclarations 仅允许 R1006 的精确 PS5.0 输出调制契约，其他声明/patch、未知键/模型、真实全局 Present、CustomShader 等仍按源行拒绝，完整 main/TexFx 不会被截成假兼容。HS/DS 及未知 GS 仍拒绝；既有已认证 GS/SO passthrough 还要求无 GS override 入口，t120 继续保原 no-GS 限制。实际 TexFx 整包 consumer 和重复 shader hash 合并未实现；窄 ShaderRegex consumer 见下文。唯一 hash 的 `allow_duplicate_hash=overrule` 仅保上游重复告警策略。

本片无 tests/build/deploy/hash/manifest/fixture/游戏/profile 操作，缓存命中率、执行成本、ORFix 像素结果和全栈性能均 UNKNOWN。

## R1006 F6 source-preparation boundary

R990 automatic entry changes are present only in this isolated candidate.
The original Shader owner now retains optional immutable source bytes and
owned real input/output signatures. Preparation acquisition is exact-device,
lease and generation bound; per-Capture retained payload is capped separately
from game resource bytes. No new draw-time reflection/allocation is added.
The PS5.0 target6 signature helper remains a request-only API and is not used
by the supported no-new-output consumer. `GimiShaderTools` compiles bounded
PCRE2 at Load and performs actual transformation/CreatePixelShader inside
Session::PreparePipeline at existing Present preparation under real internal
suppression. Proposed and final code must preserve every original instruction
except exact float4 output MOV -> t120 load/fresh-temp multiply/same output MOV.
Outputs 0..5 and the original authentic signatures are retained. Other resource,
sampler and output declarations are rejected; installed TexFx is not complete.

Dx11PipelineBinding carries the owning replacement capability. The existing
executor validates it before Reserve, binds it immediately before physical
raster/Before, and restores the actual original PS alongside touched SRV/IA
bindings. The packet's copied pipeline retains the replacement/source/original
and device; its immutable parameter owner retains pages. Existing real fence
retirement/quarantine is unchanged. No alternate GPU owner, completion signal,
hook, core, loader, UI or profile path is added. The existing callbacks see the
actual replacement shader and keep their own identity/reader refusal rules;
no original shader classification is spoofed for the new object.

Root integrates only the cumulative product hunks and performs any formal
build/deploy. This worker has run no product compilation, tests, validators,
fixtures, hashing, deployment or game operation. Source changes do not establish
successful CreatePixelShader, real-game rendering or performance.


## M5a exact Outline material continuation (source only)

This candidate contains a separate exact-content Outline template contract and actual t69/t70/s15 material preflight/bind/restore/packet-retention path. It does not authorize arbitrary shader resources or change the original output signature. Only actual immutable user DDS Texture2D leases with a typed float sample view and device SHADER_SAMPLE support qualify; each real raster sees the command tape state in source order. The actual current s15 object (including observed null) is retained and used unchanged, never synthesized. Every required lease is reserved through the existing real GPU retirement/quarantine path before setters; no new queue, fence, Present hook, core or loader is introduced.

M5b supersedes the earlier source refusal: fixed XXMI ShaderRegex preserves `${ini69}_abs.x`, and its actual assembler uses the numeric register prefix without setting ABS. Preparation sends the original spelling unchanged to that assembler, then compares the final executable against this exact no-ABS source contract for this one pinned template operand; no suffix stripping or inferred absolute-value correction is performed. Author confirmation is not a gate. The known Vodyanitsa INIs provide no t69/t70 bindings, but those materials are optional in this template: unbound resinfo dimensions are zero, sample branches are skipped, and actual t120-driven threshold/discard/fallback instructions remain. All-null resources are legal without being reported as sampled material. Nonnull resources still require exact view/device/owner qualification; sampler filter checks follow their possible sample path. No runtime or pixel result has been observed. See third_party/texfx-outline/SOURCE.md and audit m5b/RESULT.md; the older m5 report remains a frozen historical delivery. Full Diffuse/MRT/Present transparency work is separate.

M5b also qualifies the existing ordinary command-bound DDS consumer against the actual original PS creation reflection. Complete tape simulation checks each real Draw/DrawIndexed, including explicit VS PRE rasters; immutable typed float Texture2D SRVs must match a declared original PS Texture2D float slot. No Diffuse-to-slot mapping is inferred: the real SetTextures/ORFix command tape supplies it. The packet retains original getter-derived samplers, the static DDS resource/view and alias owners through existing GPU retirement; only touched SRVs are set/restored. Unchanged game resources remain under the existing serial owner contract. The first ordinary color-DDS slice conservatively refuses any original comparison sampler or reduction filter because texture-to-sampler pairing is not reflected; it does not synthesize a sampler. An undeclared bound slot is transport only and provides no consumer claim. No replacement shader, new output, core, Present hook or user-config edit is needed for this ordinary path. HeadA/whole-package/runtime success remains unknown.
