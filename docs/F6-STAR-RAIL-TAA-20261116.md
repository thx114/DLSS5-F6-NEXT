# 崩铁 TAA / TAAU 输入研究

2026-11-16。用户要求新增只读 `gpt-6.1-sol high` 子任务；已完成并关闭。研究实例 `01a1e0df-8ef2-7921-b2b3-b76dd9754dbb`。本项不修改游戏、配置、注入器或 F6 捕获路由，不把其他游戏的绑定当作崩铁事实。

## 当前判断

值得继续研究 TAA pass 作为公共输入来源；**尚不能确认崩铁的 TAA 是 TAAU，也没有证明目前可直接支持 FG / SR / NR 历史**。当前没有崩铁专属 shader dump、常量布局、同帧有效输入/输出区域或实际 depth / motion 绑定证据。暂不实现按猜测识别 pass 的产品 Hook。

“作为放大器”须区分两件事：如果游戏 temporal resolve 本身从较小有效采样网格重建到较大输出，才可研究原生 TAAU 输出后端；普通同分辨率 TAA 可以提供抗锯齿后的颜色或原生 guides，但不会因外部捕获自动获得放大能力。先 TAA、后空间缩放，以及较大分配中的较小有效 rect，都不能独自证明 TAAU。

## 已找到的逆向材料

- 本工作区 `build/taa-hook-research-20261114/REPORT.zh-CN.md`，SHA256 `0113867CBD36A8BEABD67C92DBE18F4CBCDFFD0B5A2DA049AB684C0BD36D2EDE`；本轮重新核过哈希。样本 Lecram SHA256 为 `F0A2610C5A1A10D0AB15BABE8C91274A7E600611CDC279A41E26E355ED609B7A`，样本身份来自该报告，本轮不重新执行样本。
- 报告的 DX11 通用逆向证明存在颜色、深度、运动、jitter、DLSS 执行及条件写回链。`pseudocode/003-dx11-capture.c:1518` 先重放原 pass，`:1608` 才运行 DLSS，`:1822` 决定回写。因此不能据此承诺省掉游戏 TAA 成本。
- 报告约第 128 行起是**绝区零历史版本**的具体槽位、非线性运动编码及两个输出；不移植这些槽位、hash 或编码到崩铁。
- 报告固定的 Luma 参考提交 `80936e7822f70625d86dcb762be537b2420ad209` 可借鉴识别、绑定验证及替换方法；它不是当前崩铁的运行证据。原有参考的来源及字节身份保留在该报告的 `references/sources.json`。

## 需要取得的崩铁证据

| 问题 | 必须验证的内容 | 目前状态 |
|---|---|---|
| 是 TAA 还是 TAAU | 同一 resolve 的当前输入、history、输出 view、viewport、有效 rect；重建算法或多帧采样关系 | 未知 |
| depth | 原始资源与 typed view、尺寸/rect、正反 Z、投影参数、天空/无效值 | 未知 |
| motion | 编码解码、单位、正负方向、Y 方向、jitter 是否已计入；静止相机、相机移动、角色独立运动分别核对 | 未知 |
| 帧与历史 | 当前色、上一色、jitter、reset、切镜头、菜单、分辨率变化；所有 history 输出与消费者 | 未知 |
| FG 输入 | 实际基帧颜色与 guides 同代，render/display 区域及 HUD/后处理关系；保留至真实消费完成 | 未知 |
| GPU 所有权 | 实际 device / context / command list / queue，读写顺序及所属 fence；成功 Reset 与资源退休 | 未知 |

仅深度加相机矩阵可重建相机运动，不能代替角色、衣物、形变物体的原生运动。TAA 前没有 HUD 的颜色，也不自动符合最终基帧的 HUDless 契约。

NR 模型单帧颜色处理与 NR 细节历史分别验收；前者能运行，不证明后者有可用的时序 guides。SR 成功也不能替代 FG 在稍后消费时对同帧资源内容和寿命的要求。

## 建议实现顺序

1. 获取用户手动游戏测试产生的当前版本 shader / pass / 绑定材料。先只读确认候选和上述契约，不全局拦截所有 Draw，不用轮转/指针等式认定内容同帧。
2. 将游戏语义放入小型来源适配器，统一发布颜色、guides、有效区域、帧身份与 GPU 寿命。接口接现有 `OptiScaler/inputs/FG/UpscalerFrameParameters.h`、`Upscaler_Inputs_Dx11wDx12.cpp`、`framegen/dlssg/DLSSG_Dx12_Inputs.cpp` 及 `shaders/dlssnr/SceneGuideContract.h`，来源与放大算法分开。
3. 先保留原 pass，只验证采集是否正确、是否重复复制，以及各消费者是否实际收到同帧输入。报告这时仍有原 TAA 成本。
4. 如证实 TAAU，可研究其原生重建结果作为一个输出来源；如仅普通 TAA，研究把其输入交给既有 DLSS / FSR / XeSS 等后端。替换原 pass 之前须闭合全部 history 写入、下游后处理与 reset。
5. 最后才按同场景、同分辨率、同功能状态及实际 GPU 队列时间判断性能价值。无收益或需要堆叠重复处理时停止该路线。

当前交付是研究结论和证据清单，不是崩铁新后端、TAAU 支持或实机可用声明。原神继续沿外部 FSR 桥进入 NGX，不恢复已移除的内部原神 FSR 捕获。
