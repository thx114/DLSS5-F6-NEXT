# Encore NR codec：已恢复的颜色与残差重建

2026-11-07，北京时间。固定DLL SHA256及路径见README；本轮仅静态逆向与离线参考代码，
没有加载目标或调用SDK/GPU。shader标签由分析者命名，不是PDB符号。

## 字节到实际PSO的链路

|shader RVA / bytes|pointer+size表RVA|runtime PSO槽|用途|
|---|---|---|---|
|0x57f800 / 27520|0x586380|+0xa0|encode，无exposure纹理|
|0x586390 / 12680|0x589518|+0xa8|decode，无exposure纹理|
|0x589530 / 27952|0x590260|+0xb0|encode，exposure纹理变体|
|0x590270 / 13108|0x5935a8|+0xb8|decode，exposure纹理变体|

0x149c93..0x149cf9装入这四组{bytecode VA,size,PSO输出槽}；0x149d10..0x149d87
逐项创建compute pipeline。新提取器直接核对四个表的指针/长度，不依赖旧shader标签猜测。
0x1587b9..0x1587d1依据[r15+0x98]选择+a0/+b0；decode在0x158ddb..0x158dea
选+a8或已保存的+b8地址。后者存储见新增窗口0x157a2e..0x157a4b。

0x144fd0..0x145113设置PSO及root；0x14504b给出16个32-bit常量，即64B。
四个shader的CB均为OutSize(uint2)、OutBase(uint2)、WorkSize(uint2)、Hdr(uint)、
Exposure(float)，以及后32B的Foveation/Gmax/Center/Reserved。

resource合同为Original(t0)、encode Target(u0)；decode另有Proxy(t1)、Neural(t2)；
exposure变体有ExposureImage(t3)。结合原runtime窗口：Original来自保存的SR颜色副本，
Proxy是+0x1a8的encoded工作图，Neural是+0x1b0的NGX输出，decode写回原Output。
原图alpha与NR工作图alpha不能混为一谈。

## 数学：encode先过滤，decode重建残差

以下只展开矩形、关闭foveation的分支。warp映射及Guides的独立参考见NR-WARP.md，
没有将本文件的矩形参考入口静默扩成另一种常量ABI。

1. Encode将一个work像素对应的输出rect footprint作面积加权平均。每个RGB样本先将
   NaN/Inf置0、负数截0，再累积重叠面积并除总面积；不能先对每个样本非线性编码再平均。
2. SDR只是clamp到[0,1]，不再做线性转sRGB。HDR先乘有效exposure，令p=max(R,G,B)。
   p大于0.75时，peak改为0.75+0.25×(1-exp2((p-0.75)×K))，K的float32位型
   为0xc1b8aa3b（约-23.0831203），按新旧peak之比缩放RGB。随后clamp并作sRGB-like
   分段编码。encode写alpha=1。关键token offset为0x63ec..0x66f4。
3. Decode先用原全尺寸像素重新计算encoded center，映射到work坐标后取四邻域。每个
   tap权重为bilinearWeight×(exp2(-72.1347503662×||Proxy-center||²)+0.0010000000475)。
   对Neural-Proxy作加权平均得到delta；它不是对Neural颜色直接做普通bilinear。
   普通decode token offset为0x20f8..0x28dc。
4. SDR返回clamp(center+delta)。HDR令a=DecodeGamma(center)、
   b=DecodeGamma(clamp(center+delta))，逐通道gain=clamp(1+(b-a)/max(a,epsilon),0.25,4)，
   最终为sanitized原HDR×gain，alpha保持原图。epsilon与权重下限同为0x3a83126f。
   不能改写成b/max(a,epsilon)：a很暗时两者不同，零残差应保留原图。

Gamma常量按DXBC立即数的float32位型保存在nr_codec.py/HLSL中；CPU使用float64，
不冒充GPU float32/FMA逐位结果。Hdr普通shader以非零为HDR，exposure变体以bit0识别HDR、
bit1决定读exposure图；本轮CPU已识别选择值0/1/3，参考实现只支持这个子集。

exposure变体读t3(0,0).x；非有限或非正样本改用1，再乘基础Exposure，结果至少1e-4。
基础Exposure及纹理身份/有效性仍由调用方证明，不能因为存在指针就认证正确的曝光来源。

## 实际代码和接入边界

- nr_codec.py：独立颜色转换、四tap残差、HDR重建及一维面积footprint数值参考。
- test_nr_codec.py：14项CPU回归，包含HDR高光/零残差、gain上下限、SDR同raster简化、
  edge affinity、曝光异常、面积覆盖、输入拒绝，以及固定DXBC指令立即数核对。
- nr_codec_rect_reference.hlsl：独立EncodeReference/DecodeReference入口，无foveation。
  CB为32B前缀，t0/t1/t2/t3/u0与上述资源角色对应；它不是原64B root ABI的drop-in PSO。

Host须先确认有效OutBase/OutSize、WorkSize非零、所有view范围与格式、关闭warp、
HdrFlags为0/1/3、有限RGB/exposure及乘积；Proxy/Neural也须处在有限数值域。原始RGB的
NaN/Inf处理已恢复，不代表所有极端浮点中间结果都已实现。
两入口不管理分配、barrier、descriptor更新、queue、fence或原图保留；不得以编译成功
或CPU函数返回授予真实ticket/GPU完成/显示资格。Original副本、工作图和目标须有独立且
合法的view/state/生命周期，不能为省copy引入未证明的SRV/UAV别名读写。

## 对F7最有价值的取舍

F7 NrDx12::SetFrame目前将传入finished color直接交feature18；NrFrame声明guide映射与
身份，但当前该结构未携带显式HDR/曝光/颜色编码域。格式RGBA16_FLOAT本身不能回答这些
问题；本轮未审完实际producer，不据此判定现有输入错误，也不把Encore包装宣布为SDK强制。

- **SDR、同尺寸、同rect、无warp时可有简化。** 一个输出像素只对应一个work像素，
  delta=Neural-Proxy且center=Proxy，重建RGB退化为clamp(Neural)。已有正确display-referred
  SDR输入不应默认新增完整面积encode和四tap decode。alpha仍要按原图合同保留；
  CPU恒等式不证明SDK实际数据域、画面或GPU舍入完全一致。
- **HDR应单列合同。** 不能把原始线性HDR直接当作已编码代理，也不能把Neural工作图
  直接写回原HDR替代上述相对gain。先绑定实际颜色域/曝光/同ticket源，再做固定输入的
  GPU数值与画面对照，不能仅按游戏名或纹理格式猜测。
- Encode成本包含覆盖footprint的动态循环，decode包含每像素四组Proxy+Neural读取及
  权重计算；shader静态指令数不是GPU毫秒。先测实际分辨率、work比例、pass数与既有
  color/guide复制。跨NR pass合并encode/decode会改变非线性与历史语义，不直接当等价优化。

## 可复现验证

统一verify.py新增nr-codec profile：300条限定本机指令、四个shader的2754个token，
直接校验四个bytecode表。带--fxc时还用/Ges /WX /O3编译两个参考入口为cs_5_1，
reference-compile.json记录源码/编译器/产物哈希和真实退出码；没有执行生成的shader。

warp/foveation数学和Guides局部公式已另见NR-WARP.md；完整CPU参数生成/调度/异常路径、
实际NGX数据域与GPU等价/性能仍未验收。
独立Sol已核新代码、提取/验证路径及本报告，在所述finite/no-warp范围内无确定P1/P2。
另直接对固定DLL核对300条本机指令、2754条shader token与四个字节表，实跑14项新增
CPU测试通过；in-memory反例确认缺失/重复入口、失败编译、错误target和越目录产物被拒绝。
该复核没有编译、运行GPU或验收游戏，完整42项及两个入口编译由统一verify实际执行。
