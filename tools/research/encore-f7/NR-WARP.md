# Encore NR warp：颜色、深度与运动矢量的共用坐标

2026-11-07。基于本包固定 SHA256 的 Encore v2.0.1，地址均为 RVA。
本报告及代码是独立离线参考；没有修改 F7 产品或运行 GPU。

## 能直接帮助 F7 的结论

- 开启非线性压缩时，MV 不能只乘 WorkSize/SourceSize。应将位移终点映射到
  work 坐标，再减当前 work 像素中心。深度、MV、颜色必须共享实际映射参数。
- Guides 从深度、MV 的独立尺寸和 base 做最近点读取。该 shader 不线性化深度，
  不生成遮挡感知最邻近深度，不认证相机或补 source/frame 身份。
- 颜色 shader 某轴 OutSize==WorkSize 时跳过该轴 warp；Guides 则按 packed enable
  决定。Host 不能给匹配尺寸的颜色轴保留一个实际改变坐标的 guide warp。
- 固定四次 Newton 反求不是精确逆。参考实现保留这个误差，不悄悄改成精确求根，
  也不将合成极端参数的误差称为 Encore 正常运行缺陷。

## 字节码、PSO 和常量闭环

Guides blob 位于 0x594390，20,296 bytes，SHA256
e35a2f7096a7d29887404d932d28da514eac48e91fd7ad18bafca6345a5aae7e。
本次新解码 694 个 token 行，含声明；FXC 注释约 684 个 instruction slots，
不将其当动态执行次数或 GPU 时间。

0x14f897 直接 LEA 指向该 blob；0x14f8a2 给 ShaderBytecodeLength=0x4f48；
0x14f8d7 调用创建 compute PSO。这里没有颜色四 shader 那样的外置 pointer/size 表。
0x153119 设置 20 个 32-bit root 常量，即 80B；0x15319a/0x1531a3 按
ceil(WorkSize/8) 调度。NR 主调用 0x158796，受 0x158789..0x1587a5 的分支限定；
另一路 0x15cf40 不属于本报告已恢复的 Guides 调用。

| byte offset | 类型 | 字段 |
|---:|---|---|
| 0 | uint2 | WorkSize |
| 8 | uint2 | MotionSize |
| 16 | uint2 | MotionBase |
| 24 | uint2 | DepthSize |
| 32 | uint2 | DepthBase |
| 40 | float2 | UvScale |
| 48 | uint2 | Axes |
| 56 | float2 | Gmax |
| 64 | float2 | Center |
| 72 | float2 | Reserved，未使用 |

t0=Depth(float)，t1=Motion(float2)，u0=DepthOut(float)，u1=MotionOut(float2)。
0x158609..0x158647 将同一组 Axes/Gmax/Center 写入颜色常量；
0x15874d..0x158771 再写入 Guides。UvScale 在 0x158705..0x158745 为
输入对象字段 +0x1e4/+0x1e8 分别除以 +0x84/+0x88 后写入。未由名称猜测这些
字段就是任意纹理大小，也未据此认证 F7 当前原生 MV 的符号或单位。

## 每轴曲线

packed bit31 为 enable，A=(bits0..11)*float32(0x39800801)，B=(bits12..23)/2048，
S=(bits24..30)/128，K=1-S。对非负半径 d：

    P(d) = d                                         d <= A
         = A+B*(t-K*(t^3-.5*t^4)), t=(d-A)/B         A < d < A+B
         = A+B*(1-K/2)+S*(d-A-B)                     d >= A+B

令 c=Center，g=Gmax。原图 normalized u 到 work normalized v：

    c==.5: F(u) = .5 + .5*sign(2u-1)*P(abs(2u-1))/g
    其它:  F(u) = (P(2c)+sign(u-c)*P(2*abs(u-c)))/(2g)

反求：

    c==.5: I4(v) = .5 + .5*sign(2v-1)*P_inverse4(abs(2v-1)*g)
    其它:  d=2vg-P(2c); I4(v)=c+.5*sign(d)*P_inverse4(abs(d))

P_inverse4 在内段直接返回 y，外段用线性逆。在中间段令
q=(y-A)/B，t=q，并**恰好四次**：

    t = clamp(t-(t-K*(t^3-.5*t^4)-q)/(1-K*(3*t^2-2*t^3)),0,1)
    最后返回 A+B*t

Guides x-centered 的四次 add_sat token offset 为
0x70c/0x878/0x9e4/0xb50；颜色 encode 同支为
0x820/0x98c/0xaf8/0xc64。参考使用 float64，不复现 DXBC float32/FMA 舍入。

0x39800801 是 float32(1/4095)，约 0.00024420025874860585；FXC 文本只显示
0.000244，不能据显示小数改成 1/4096。独立复核发现的这项 P2 已修正代码和 golden，
并增加 Guides/encode/decode 三个原始 token 的立即数字节断言。

常见端点归一化可代数推导为 g=(P(2c)+P(2(1-c)))/2。实际 CPU writer 使用
double 的 qw/4095、float32 的 Center/1-Center，以及 float32 的 Gmax 存储；
它不是把 shader 的 float32 reciprocal 乘法原封不动放到 CPU。生产接入须保持实际
Gmax，不能为了端点严格为 0/1 擅自重新归一化。详见下一节。

本代码支持enabled时B>=0、S>0；零B的分段数学结果由内/外段覆盖。
这不保证GPU一定跳过未选中的Newton算术，不以分段形式推断实际指令成本。
这不宣称所有数学输入都由CPU writer发射，或所有原生异常路径都已复现。
测试的低 slopeBits=1 是分析者合成输入，低于本次确认的 CPU 成功打包下限 32。
t=.99 严格处在过渡段时，四步反求可产生
约 -0.02753 的原始 radius 误差；该例不靠把精确端点误送 Newton 分支制造误差。
该例不能作为 Encore 正常配置的数值缺陷证据。

## CPU 参数与工作尺寸的已核实部分

0x14efc0 读取 enabled、preset、Width/Height/Falloff/Edge 与 CenterX/Y。
preset==3使用显式值；已核0..2从0x55a020的24B表读取，值见下节。更高编号不猜表。
0x14f010..0x14f091 把三个 uint32 百分数上限限到 100，Edge 限到 [25,100]。
每轴调用 0x14e6d0，先把 Center 限到 [0,1]。调用使用 10000 为基准尺寸，
宽度与 falloff 百分数先乘 100。设 R 是 0x4ffe60 的返回值：

    qw = clamp(R(4095*widthPercent/100),0,4095)
    qf = clamp(R(4096*falloffPercent/100),0,4095)
    qe = clamp(R(128*float32(edgePercent/100)),32,128)

widthPercent==100 在量化前返回 disabled；qe==128 在 0x14e7e1..0x14e7e5
返回disabled，因此已识别成功轴的qe为32..127。falloff为0确实能生成B=0，现有参考已支持。
新增定向恢复了 R 的有限、int32结果范围：0x4ffe60先经0x50d390分类，再调
0x504060按double位域舍入，最后检查[-2147483648,2147483647]并截断转换。
半整数远离零（2.5到3，-2.5到-3），不依赖Python round的ties-to-even。
nr_grid.native_round_int32保留该有限子集，NaN/Inf、异常处理/errno与越界返回0不模仿，
明确拒绝；也不用x+.5避免临近.5的double二次舍入。

CPU 曲线 0x14beb0..0x14bf57 与上面的分段多项式同形，但 A=qw/4095（double）、
B=qf/2048、S=qe/128。0x14e990..0x14ea0f 用这条 CPU 曲线求 Gmax：
center==.5 时 Pcpu(1)，否则 (Pcpu(2c)+Pcpu(2*float32(1-c)))/2，最后转 float32。
native_axis_from_quantized 仅实现这段已量化参数到 Gmax/Center 的过程，不生成配置或尺寸。

0x14f153..0x14f186 的配对门槛：两轴都 disabled 时保持双 disabled；
否则若 double(GmaxX)*double(GmaxY)>double(0.95)，也清两轴并把 Gmax 设为 1。
比较的不是 Center 乘积；单轴 disabled 不会仅因这一事实直接清另一轴。
这对应后续 less than five percent pixel saving 的拒绝日志，是工作网格节省策略，
不证明实际总 GPU 时间至少节省 5%。

0x15d660 的基础 work 生成：正比例值夹到 float32 [0.33,1]，再 R(request*ratio)
并至少为 1；非正比例且两个外部限制尺寸都非零时，逐轴 min(request,external)。
无此有效限制则比例取 1。可选0x14f320的整数优化目标与候选选择已恢复，见下节。
0x156f12..0x156f72 对 enabled 轴将基础 work 再乘 Gmax，经 R 后至少 16；
可选调整后任一 work 维度小于 64 时拒绝（0x156f9c..0x156fb4）。
ctx+0x10c/+0x12c的完整配置来源与所有异常边界仍unknown，不自动移植为产品策略。

## 完整有限每轴writer与已核preset

nr_axis_config.py提供axis_from_native_inputs、pair_from_percentages、pair_from_preset，
使用已核R舍入、float32中心/Gmax存储和pair门槛。输入仅支持有限float32参数、
uint32计数及1..16384的基准extent；不复现NaN/Inf选择、errno/异常或MXCSR。

0x14e6d0返回每轴24B（little-endian IffIIf），两轴连续48B：

|偏移|类型|离线字段及含义|
|---|---|---|
|+0|uint32|packed；disabled时0|
|+4|float32|Gmax|
|+8|float32|Center；保留原生clamp内的-0|
|+c|uint32|inner_pixels=min(inputInner,N)，不等于量化qw|
|+10|uint32|transition_pixels，见下面公式|
|+14|float32|boundary_derivative，原生边界分支参数|

令A=qw/4095（CPU double），B=qf/2048，S=qe/128，K=1-S，
rFar=2*max(float32(1-Center),Center)。尾字段为：

    transition_pixels=R((N/2)*clamp(rFar-A,0,B))
    boundary_derivative=S                         B<=0或rFar>=A+B
                       =1-K*(3t^2-2t^3)           其它，t=(rFar-A)/B

B=0、rFar=A时两侧导数可不同；这里保留原生返回S的分支，不将字段当唯一解析导数。
不足16、inner覆盖N或edge量化128时单轴disabled，仍保留clamped Center/inner_pixels。
整个config关闭时则返回默认轴（Center=.5/inner=0）。pair节省门槛只清packed和Gmax，
不擦掉其余已生成字段。48B native pair不能直接当80B Guides CB；颜色/Guides仅取
packed/Gmax/Center并按各自ABI安排，to_bytes只用于检查CPU结构，不是SDK提交权限。

0x55a020、0x55a038、0x55a050的表项（名称指针+四个uint32）：

|索引/名称|Width %|Height %|Falloff %|Edge %|
|---|---:|---:|---:|---:|
|0 light|65|65|10|65|
|1 balanced|55|55|8|50|
|2 strong|40|40|8|35|

这些是Encore warp preset，不是F7 SR质量/模型预设。pair_from_preset仅接收0..2；
自定义走pair_from_percentages（preset3）。native选择器未见更高编号边界检查，
参考工具拒绝猜测。evidence/nr-warp/native-warp-presets.json保存原始字节、名称RVA和值，
提取时直接断言三项表与NUL结尾名称，统一verify绑定文件哈希。

    & D:/APPS/conda/python.exe -B tools/research/encore-f7/nr_axis_config.py --preset 1
    & D:/APPS/conda/python.exe -B tools/research/encore-f7/nr_axis_config.py --width-percent 50 --height-percent 50 --falloff-percent 0 --edge-percent 50

CLI的preset与显式四百分数互斥，只输出离线参数，不读取或修改F7配置。
配置身份仍有缺口：0x156d9e调用后复制48B，0x157432..0x15744e写到r14+0x118起，
因此该对象r14+0x12c来自轴+0x14字段。它不等于0x156f75所读r12+0x12c的boolean开关；
两者对象同一性未证。0x15d660输入ctx的+0x10c比例及+0x12c优化开关上游仍未知，
不因偏移相同就串错producer/consumer，也不将研究参数当产品默认值或运行配置。

## 可选网格成本与两阶段尺寸搜索

nr_grid.py独立复现0x14b4b0的整数score和0x14f320的搜索；名字是分析标签。
支持score输入64..16384；搜索的uint32输入超出此范围按原生early-return原样返回，
显式reference的非零维度限定1..16384。其它超大reference可能涉及32位商截断，
不在参考实现范围。score不是GPU毫秒、分配字节、实际dispatch数或已恢复的模型布局。

对每轴n，先h0=align4(ceil(n/2))，其后五次hi=align4(floor(h(i-1)/2))。
k=[h0<n]+[(h0&4)!=0]+sum[hi<h(i-1)]，step=2^k。
p=max(320,ceil(n/step)*step)；注意先取整再max320，320不必整除step。
若px%(4*stepX)==0且py%(4*stepY)==0，score=(px+stepX)*py；否则score=px*py。
这是额外加X一列step，不是两轴都扩大。64x64得107520，65x65反而得102400，
说明本函数score不随输入像素面积单调增加。

0x14f320以原尺寸为初始best；任一reference为零则两轴一起回退为原尺寸。
候选宽遍历max(64,ceil(.98*W))..W，候选高只试floor/ceil(cw*referenceH/referenceW)，
且须在max(64,ceil(.98*H))..H。再要求
400*abs(cw*referenceH-referenceW*ch)<=referenceW*ch，即该交叉积定义的相对误差<=.25%。
选score更小者，相同score时保留面积更大者，再完全相同则保留先遇到的候选；
没有更优合格候选就保留初始尺寸，不强迫初始尺寸满足新的reference aspect。

主链有两次可选调用，不能合并：
1. 0x15d76e：base ratio/external limits之后，用原请求宽高作为reference。
2. 0x156f8a：各enabled轴再乘float32 Gmax，R后至少16，再以reference=0优化。
最终仍受64最小维度拒绝；参考函数只返回尺寸，不伪造成功NR准入。
work参考仅接收CPU writer使用的packed=0 disabled表示；shader数学Axis允许的其它
bit31未置位但packed非零表示在此拒绝，因为原生work分支实际比较整个packed是否为0。

独立CLI只算候选，不改配置：

    & D:/APPS/conda/python.exe -B tools/research/encore-f7/nr_grid.py 1920 1080

此输入本地参考结果1907x1072，score从2211840到2088960；3840x2160选3776x2124，
score从8355840到8216576。不是推荐分辨率或性能收益，是否真实触发取决于配置/调用链。
F7若采用此方向，应只在实际尺寸/配置变化时计算并缓存纯参数，在变更时重新处理guide/
颜色映射与NR历史；不能把候选缓存当source/frame/资源完成证明。先以同帧GPU时间
比较候选Evaluate及额外encode/guides/decode/复制等待的总成本，再决定启用。

## Guides 的完整局部公式

逐轴计算，v 为当前 work 像素中心，m 为实际采样到的 MV：

    v = (id+.5)/WorkSize
    p = I4(v)
    depthPixel = DepthBase + min(floor(saturate(p)*DepthSize),DepthSize-1)
    motionPixel = MotionBase + min(floor(saturate(p)*MotionSize),MotionSize-1)
    DepthOut[id] = Depth[depthPixel]
    e = p + Motion[motionPixel]*UvScale
    MotionOut[id] = (F(e)-v)*WorkSize

采样仅夹紧 p 的副本；终点 e 使用未夹紧 p。离屏终点仍走同一曲线，不先 saturate。
token 0x2f30..0x30e0 对应深度取样/写入，0x30fc..0x313c 对应 MV 取样与终点，
0x4a40/0x4a60/0x4a7c 对应减 v、乘 work、写 MV。

无 warp 时退化为 m*UvScale*WorkSize。开启 warp 后不是全局比例；即使 m=0，
浮点/四步求逆残差也可能保留在输出中。严格复现时不擅自把零运动特判为零；
F7 若选择不同数值方法，要另设等价容差和 GPU 画面实验。

## 代码和性能取舍

nr_warp.py 提供 Axis、encode_box_axis、decode_axis、guide_position、
nearest_address、guide_motion、native_axis_from_quantized；可与 nr_codec.py 的颜色残差组合做离线检查。
nr_warp_reference.hlsli 是可复用映射函数；nr_guides_reference.hlsl 是独立
80B Guides ABI 的参考入口。既有矩形 codec 的 32B CB 没有被改成原版 64B。

Host 仍须检查 rect+base 不越 view、非零尺寸、目标容量、enable/extent 一致性、
有限参数及所有中间乘积、颜色域、source/frame、真实资源状态和所属 queue/fence。
参考 shader 不做资源分配、barrier、退休，也不为深度或 MV 授予真实游戏身份。

只读复查 F7 src/neural/NrDx12.h/.cpp：现有 NrGuideMapping 记录同画面映射身份与
原始/finished rect，NrDx12 要求颜色、guide、输出同 raster；SetFrame 使用已经准备好的
guide 与显式 motionScale，并不实现 Encore 的可变 work warp。现有 no-warp 合同不因
缺少这些参数而构成缺陷。若另开 warp 实验，应将实际 Axes/Gmax/Center 与颜色域
纳入不可变映射记录，并在映射变化时处理 NR history reset；不能仅改 Width/Height，
或拿原始 MV 搭配压缩颜色绕过现有身份/尺寸检查。

F7 应先保留可证明的同尺寸 SDR/no-warp 路径，核对能否沿用现有 guide；
不要仅为了模仿 Encore 新加完整 warp/color/guide pass。需要降低 NR 工作像素时，
再以同帧 GPU 时间分开测 encode、guides、Evaluate、decode、复制和等待，比较
整体收益与画面/运动误差。该 Guides 分支每输出像素一深度、一 MV 读取、两输出，
并含坐标反求/正映射；颜色 encode 的 footprint 成本还随压缩率变化，不能只按
work 像素减少比例预测整栈加速。本次没有性能实测。

## 验证

统一 verify 验证 CPU 测试和三个 cs_5_1 入口的实际 FXC 编译
（/Ges /WX /O3）；新增 22 项测试覆盖 bit 字段、原始缩放立即数、CPU Gmax、曲线连接、偏心、footprint、
extent bypass、独立子矩形、端点 MV 与四步残差，并核固定 shader token。
nr-warp profile 新解码 guide 调用、CPU packing/Gmax、配对门槛与 work 尺寸窗口，
计数见evidence/validation.json；当前含1038条native、694个token、42个指令断言、13个常量断言与3项preset表断言
和一组直接 PSO bytecode 绑定。统一验证同时保存 include 哈希，改变 hlsli 后必须重新编译。
未运行生成的 shader，未验收 GPU 等价、真实配置范围、游戏画面、性能或 6× provider。

Sol 独立定向复核已关闭 A 缩放常量 P2，无新增确定 P1/P2。复核实跑 64 项测试，
逐字节核对 614 条 native、17 个指令与 7 个常量断言；另在内存中解释原始 Guides
指令跑 300 组输入（0 mismatch，最大 float64 差异 8.53e-14），解释 CPU 曲线核
1,000 组量化输入的 Center/Gmax 存储值一致。两组独立解释器属于复核记录，未纳入
verify 的 64 项测试或作为本包文件交付；复核没有编译、运行 GPU 或部署。

新增grid/round独立24项合成回归，覆盖临近半整数、subnormal、整数上下界、原生score
golden、非单调score、搜索矩形/宽高比/平局、缺省reference、基础比例/双限制和两阶段
调用。旧614条native复核记录不自动覆盖新增加的357条native，独立审查另行记录。
一项测试以原生double位移/ADC的另一种逐步翻译核2018个有限舍入输入，计为一个测试。

grid冻结版另经Sol独立原始字节复核，无确定P1/P2；亲跑24项测试。内存oracle以
474条目标指令解释建立，核118079组唯一score输入、4314组optimizer、145338个有限
double（116027返回一致、29311越界异常按参考合同拒绝）、2140组base work、1248组
warp work（1233组域内一致、15组bounded拒绝），另核两组非canonical disabled拒绝。
这些解释器未交付为文件、未计入统一118项；没有执行原生DLL或穷尽二维输入。
Sol另直接核971条静态指令/33锚点/9常量全部匹配，不能把971条都算作语义解释覆盖。
grid单独交付时仅重解码nr-warp并核旧FXC产物；本次axis补查后的完整verify已重解码
四profile，并实际重编三个cs_5_1入口（/Ges /WX /O3），全部exit0。
写者CLI实测常见候选、原生early-return与参数拒绝，无产品或GPU操作。

完整axis/pair新增部分经Sol独立终审，无确定P1/P2；亲跑86项NR测试通过。
原始指令内存解释核53组24B轴、31组48B显式pair逐字节一致，另核三个preset wrapper；
原token解释12组B=0映射与16组SDR/HDR常量纹理encode/decode输出均有限，
与float64参考最大差分别约8.24e-7、3.73e-9，不作GPU逐位声明。
Sol另核1038条native/42锚点/13常量/3表项和3448条相关shader token；
解释器不作为本包文件交付，不计统一144项。终审指出的文档哈希滞后在最终verify刷新，
写者另实测三preset CLI、零falloff、禁用默认值及五种非法CLI拒绝，不写产品或配置文件。
