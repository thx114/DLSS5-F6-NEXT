# Encore NR：接口、失败路径与成本位置

2026-11-07，北京时间。固定DLL身份见README；SHA256
06a7fd63ee0f0e95574f3869d70c3c9ae4aaf326ba53ff8c864064410a5f1217。
ImageBase 0x180000000，所有地址均为RVA。限定静态证据，不是完整NR实现或GPU结果。

## 输入与执行顺序

直接payload路由已补查：Encore五项NR函数来自已加载nvngx_dlssnr.dll，
不是SR driver-core同名函数的推定别名。确切表流、参数对象及F7探针边界见
NR-PAYLOAD-ABI.md；原core feature18拒绝不能推导直接payload不支持。

前轮证据的generic bridge在0x16c085调用SR、0x16c55c调用NR；SR的Output槽+0x60
经0x16c170的NR frame与0x151fc9的Output setter传递，NR在0x156b61/0x156b6b读取。
这些较早的调用链见原只读研究 rtx-encore-generic-astra 的 report.md/
guides-sync.md与evidence/fn-00156ac0.txt；本轮新增解码窗口从创建位置开始。

|位置|本轮直接解码的行为|
|---|---|
|0x157ef8..0x157f13|feature handle输出位于runtime+0x158，参数位于+0x1f8；feature编号0x12，经函数表+8创建|
|0x1582e2..0x15839b|原Output经CopyTextureRegion复制到runtime+0x1a0；原Output尚未由此覆盖|
|0x1584c0/0x158512/0x158563|+0x1a8作为UAV槽4、+0x1b0作为SRV槽7、原Output作为UAV槽9绑定|
|0x1587e8..0x158803|编码Dispatch，groups=(ceil(workW/8),ceil(workH/8),1)|
|0x15887c..0x1588a3|DLSSNR.Color=runtime+0x1a8，DLSSNR.Output=runtime+0x1b0|
|0x158c27..0x158c3c|将feature handle与参数交函数表+0x10 Evaluate|
|0x158dc7..0x158e29|检查结果后才进入解码Dispatch，groups=(ceil(effectiveOutputRectW/8),ceil(effectiveOutputRectH/8),1)|

解码使用的[rbp-0x58]/[rbp-0x54]由上游0x156c53..0x156c9f计算。每一维为
min(textureExtent-min(textureExtent,subrectBase), requestedExtent或textureExtent)，
其中requestedExtent为0时改用textureExtent。它是裁剪后的有效输出rect，可能小于
整张Output纹理；计算原件见上述fn-00156ac0.txt，不在本轮限定新解码窗口内。

本篇先定位调用与资源链；续查已将四个实际PSO与字节表闭合，并恢复颜色/残差公式，
见NR-CODEC.md及新增数值/HLSL参考；续查NR-WARP.md另恢复每轴warp与Guides端点变换。
CPU参数生成、完整调度/异常路径仍未闭环；不能据资源名断言与F7输入域等价或直接删转换。

## Depth、MV与subrect

0x1588b9和0x1588e9分别设置DLSSNR.Depth/MVec，可选working guides覆盖默认输入。
0x158903..0x158a3c分别填写Color/Output、Depth、MV的subrect：

- Color/Output使用base0与work尺寸；默认Depth使用独立查询的base及depth尺寸，
  MV使用自己的base及尺寸，不能因为Color尺寸已知就套给所有原生guides。
- 可选working guides分支改用base0/work尺寸。它与默认原生guide分支是不同合同。
- 0x158a47..0x158ae7在默认分支按条件计算incomingScaleX×MVwidth/renderWidth
  与incomingScaleY×MVheight/renderHeight；相应render维度为0或相等时保留incomingScale。
  working guides分支使用1。不能概括为所有NR输入均要求scale1或统一归一化。
- 0x158aec..0x158b0a将输入flags的bit3用于DLSSNR.DepthInverted。
  0x158b26..0x158b49将runtime状态字节与incoming reset作OR，再转为Boolean写Reset；
  reset 字节在 Evaluate 前后的写入和工作尺寸稳定块现已定向补查，见
  NR-WORK-POLICY.md；它仍不是 GPU 完成标志，完整异常生命周期不由此获证。

设置的调参名包括DLSSNR.Enabled、Intensity、LocalToneStrength、
LocalStructureStrength、SkinStructureStrength、Style、UseAutoMask、UICorrection，
见0x158b17..0x158bfd。参数名字能证明API词汇，不能认证游戏输入、UI剥离或资源完成。

另核历史CDF载荷中的rect helper：它以零size代表完整texture尺寸，严格拒绝越界，
不执行上述Encore外层的min裁剪；false时输出可能已部分写入。两份DLL及两层
语义不能混用。完整字节合同和独立`nr_rect.py`数值参考见NR-PAYLOAD-ABI.md末节。

## 失败后的颜色与GPU所有权

0x158dc9将Evaluate结果与0xfff00000相与，0x158dcf比较0xBAD00000，
0x158dd5匹配时跳到0x158e6d，绕过解码和原Output写入。非BAD分支才执行0x158e29。
0x158e84指向的失败日志也说明保留DLSS输出。**在这一已识别失败分支中保留原SR颜色**，
不等于所有异常都安全回退，更不等于NGX返回即可释放已提交的资源。

F7可借鉴事务式颜色提交：NR未确认可用时不覆盖成功SR结果；但这不应通过丢失实际
queue/fence owner、吞掉提交后Signal失败或借前一帧来实现。失败时GPU可能已有工作，
完成、隔离和资源退休仍由所属queue/fence证明；本包没有恢复Encore全部异常清理CFG。

## 与F7的交集和性能方向

只读对照 F7 src/neural/NrDx12.cpp 的SetTuning/SetFrame（约86..130）：多数同名
feature18参数已经存在；其注释明确要求guides已映射到finished raster，再统一
base0与Color尺寸。**本轮未回查全部caller/guide mapper，不将其统一subrect判为bug**，
也不建议把Encore默认原生guide缩放重复施加到已映射数据。

1. 对齐同ticket颜色、depth、MV的原始尺寸/有效rect、映射后尺寸、尺度与DepthInverted，
   保留source/SDK/display身份分离。把本报告当参数核对表，不新增逐帧扫描或CPU读回。
2. 在同一实际queue/ticket上分开测CopyTextureRegion、编码、NGX Evaluate、解码及跨API
   等待。静态可见一次颜色复制及两个Dispatch位置，**没有GPU毫秒或带宽实测**，不能宣称
   它们是瓶颈或删掉后必然加速；还未覆盖全部可选guide转换与上游准备成本。
3. 优先复用已有同ticket原生guides，避免默认引入Encore的稠密光流/屏幕拟合全栈。
   是否能省复制/codec须先证明输入输出格式、数值域、别名与SDK状态合同。
4. 颜色提交与完成通知分开：只在成功SR/NR真实消费链上提交流水资格，保持失败分支
   原颜色与实际GPU资源持有；不以新增池容量、旧帧或CPU成功代替完成证明。

证据在evidence/runtime/nr-*.json、runtime-anchors.json及manifest.json；
旧bridge到NR调用链保留原只读研究来源。解码没有运行Encore或调用NGX，不证明F7的NR
画面、6×、游戏兼容、GPU性能或安全卸载。warp/Guides局部数学见NR-WARP.md，
完整CPU参数生成/调度与所有异常分支仍未闭合。
