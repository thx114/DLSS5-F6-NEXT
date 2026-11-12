# 外部参考仓库与已采用实现清单

本轮桥更新：2026-11-12 已通过 GitHub API 核实 Aizawa 默认分支 `main` 为 `1865c585e02679d38881b9020611dfee8d44a9de`（2026-11-10），较下表固定参考 `26b0b7b7` 领先 16 个提交；正式 release 仍为 `2.4.0`。固定提交归档、原许可、隔离构建及部署审计在 [FSR 桥更新记录](../../DLSS5_NEXT-workers/fsr-bridge-upstream-1865c585/AUDIT.md)。新桥已原位部署到启动器实际选中的 `temp3/Dx11FsrBridge (2).dll`，SHA256 `CC301B42EC7AA31059B9FCD10B29B3915B9EB21DBE363D5F431ED9201CB904A1`；原桥完整源码身份未知，不能将“比固定参考领先 16 提交”当作安装版本差距。以下原参考清单与审阅保留历史身份。

原参考清单核验：2026-11-12（UTC+08）。本文件是 F6 及其既有研究的统一来源索引；产品交接见 [F6 完整报告](../../DLSS5_NEXT-workers/f6-rewrite/docs/F6-HANDOFF.md)。下表原核验只查本地文件、Git remote／HEAD和已有审阅证据，当时没有 fetch/pull，也未将其声明为远端最新版本；桥的后续联网核验与部署以上一段为准。其它来源的 release、驱动门槛和性能提升数字未在本轮重新认证。

“已参考”指已有源码／补丁／二进制研究记录；“已采用”须有当前产品或归属文件证据；“待参考”不是交付承诺。Git克隆、固定源码摘录、二进制逆向及仅建议的项目分别列出。F7的采用结论不自动等于F6采用，磁盘有仓库也不证明实际加载了它。

## 1. NR／FG 核心 fork：实际仓库与固定身份

下表的日期是本地提交日期，短哈希用于定位；各库原始提交保存完整对象身份。标“净”表示本次本地 status 无 tracked/untracked 改动，不包括 ignored 构建资料。

| 来源仓库 | 本地位置 | 固定身份与状态 |
|---|---|---|
| `janblade/OptiScaler-F5-DLSSNR-Multipass`（F5） | `D:/CODE/DLSS5_NEXT-workers/audit-f5-20261111`；产品近旁副本 `D:/CODE/DLSS5_NEXT-workers/f6-rewrite/build/reference/f5` | 两者均 `f10dedf93a2d5412c753c751c120d52b223800f6`，2026-11-07，净 |
| 同仓库较早克隆 | `D:/CODE/DLSS5_NEXT-workers/upstream-f5`；`D:/DATA/Documents/ChatGPT/DLSS F5 NEXT/audit/upstream-janblade/repo` | 分别 `505e302f5f38554081fc55dda3eb3f407380d3c5`（11-04）、`aeb439482c13dd7364ee2bef16fa41b902be6f98`（10-15），均净，不能混作同版 |
| `ShyVortex/OptiScaler-NR-PreSR-MFG` | `D:/CODE/DLSS5_NEXT-workers/audit-shyvortex-20261111` | `28557e64b21a3b5d5666b408ef75297093634a92`，2026-11-10，净 |
| 同仓库较早参考 | `D:/CODE/DLSS5_NEXT2_refs/shyvortex` | `0e1e7e541a2e1a1f81cb562e1075fd89bc6c3213`，2026-11-07，净；既有呈现报告引用的是此版 |
| `Skynizz/optiscaler-dlss5` | `D:/CODE/DLSS5_NEXT-workers/research-skynizz-20261111`；`D:/CODE/DLSS5_NEXT-workers/f6-rewrite/build/reference/skynizz` | 两者均 `fc702f8ca22f97b01bd6a02f3386bffb3484fc35`，2026-10-19，净 |
| `3zwr1/AMD-NR---OptiScaler` | `D:/CODE/DLSS5_NEXT-workers/research-3zwr1-20261111` | `75feb9613df246c6bd4d388ee7bdfca03b350d73`，2026-11-09，净 |
| `optiscaler/OptiScaler` 官方上游 | `D:/CODE/DLSS5_NEXT-workers/upstream-optiscaler` | `5eb4bfabf3c93cdb7e96b37065441f5c8000ccc7`，本地提交日期2026-11-03，净；旧文档的获取／记录日期不是提交日期 |
| `wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass` | `D:/DATA/Documents/ChatGPT/DLSS F5 NEXT/audit/upstream-f5/repo` | `1bd39091337cc07ba961c8e59ded57e21dc95b18`，2026-09-23，净；目录名“upstream-f5”不等于janblade |
| `Dagherbou/OptiScaler_DLSSNR` | `D:/CODE/_ref/opti-build` | `973761621353b99bee3dc7d4bb27b117fef2644f`，2026-09-03，dirty 12项；不是固定干净原文 |

| 来源 | 已参考／已采用功能及证据 | 下一步值得参考的功能与边界 |
|---|---|---|
| F5 janblade | [呈现报告的固定对照](../../DLSS5_NEXT-workers/f6-rewrite/docs/F6-PRESENTATION.md)：`dlssnr/design/pre-sr-multipass.md`的一次encode、immutable base、答案ping-pong和一次compose。当前F6重写采用相应流程；**不是整体同步F5** | DX11/DX12共享handle/fence、直接DLSSG、层链、失败传播；看实际queue和同步成本。Optical F5Low、Faces／场景分离、原生入口覆盖尚无本轮逐项移植回执 |
| ShyVortex | 较早固定版已列入只读参考；未找到11-10新HEAD的某项新功能进入当前F6的证据 | 原生NR解锁、PreSR、MFG／sm86兼容和SR选项。不能把README支持列表、驱动要求或其它游戏自带NR当原神验收 |
| Skynizz | 已研究log-edit缓存、低／高频分离、出生fingerprint；[运行核查](../../DLSS5_NEXT-workers/f6-rewrite/docs/F6-RUNTIME-REVIEW.md)记录F6原始指纹与残差共同迁移的不变量 | 区域低频照明稳定、真实内容变化放行、region保护。未整体采用其多纹理edit-cache／x8跳模型系统；不能把可参考收益当本项目性能 |
| AMDNR 3zwr1 | 旧[吸收复查](<D:/DATA/Documents/ChatGPT/DLSS F5 NEXT/audit/r1036-nr-upstream-reabsorption-20261104/RESULT.md>)记录当时F6已吸收fresh/old RGB共同分歧约束核心；**不是75feb96全部集成证明** | TemporalStability、交错模型、tonal LUT与runtime边界；进一步吸收前需逐项核当前保留范围及AMDNR原署名，不能统一标成RenoDX/MIT |
| 官方OptiScaler | F6本身延续OptiScaler；[F7上游研究](../../DLSS5_NEXT-workers/f7-clean-rewrite/docs/UPSTREAM-REFERENCE.md)另有SR/FG共享传输研究。固定版没有NR私有链 | 公共provider、FG输入／呈现、异常传播与跨API传输。F7据此重写SharedTransport属于F7，不是F6本轮新增 |
| wilsjo2 | 曝光／DeferredSr已作旧研究；当前`DlssNr_Spatial.h`明确SpatialWarp经其v0.8.9改编，见第5节 | 预曝光归一、guide缩放、残差域。旧摘录的RR专用支路不能写成普通呈现NR自动生效 |
| Dagherbou | 已定位旧参考树；未找到本轮具体功能采用证据 | 仅按待比较来源保留，不声称已审计完整实现或可直接回退 |

## 2. 固定源码摘录：OpenNR、Look 与历史算法

这些目录是先前保存的源码／API响应／许可摘录，不是当前可直接pull的独立克隆。共同根：`D:/DATA/Documents/ChatGPT/DLSS F5 NEXT/audit/r1036-nr-upstream-reabsorption-20261104/evidence`；完整分析见同级 `RESULT.md`。

| 来源 | 对应位置与固定版本 | 已参考／已采用 | 待参考与限制 |
|---|---|---|---|
| `zmodelerlover/OpenNR-ReShade`，旧名`dlss5-neural-amd` | 上述根的`opennr/`及`opennr-migration.json`；`8a8144d6bc838f2a6c50d48b4922b3af40b24c6a`，2026-10-23 | 已审阅`neural.cpp`、`history.inc`、`smooth.inc`、`compose.h`；未证整体采用 | evaluation／present运动域、源间断、异步hold。不能借裸hold、私有offset或CPU historyValid替代真实GPU完成 |
| AMDNR旧研究摘录 | `amdnr/`；当时固定`5d88cbe134563ffc994d89bbdc23502b9ea88466`，2026-11-04 | fresh/old共同分歧约束、LUT与实际guides门分析 | 不与第1节11-09克隆混版；当前保留范围须沿产品追踪 |
| Skynizz旧研究摘录 | `skynizz/`；`fc702f8ca22f97b01bd6a02f3386bffb3484fc35` | 区域low-edit、指纹陈旧度、SoftRefresh分析 | 被回退的soft reveal与现存SoftRefresh不同，不能当已验证抗闪方案 |
| wils旧研究摘录 | `wils/`；`6125a75b37f7e2a408e31934f4827e5660429203`，2026-10-25 | `DeferredSr::Before`、曝光归一和局部clip | 非第1节9月旧克隆；线性色ratio不能直接套到proxy/log域 |
| `Yukikaze20170315/RenoDX-DLSS5-Generic`，Lecram保留源 | `lecram/`；`1b7d6787817b806a61cece6b8aa38789304c4d25`，2026-09-28；另有逆向`D:/CODE/_ref/lecram-reverse-20261012` | 已读`look_common`、`compose`、`band`、`upsample`；颜色／Look lineage见现有归属 | 分离低频照明与高频细节。该保留源不是Lecram全部开发的唯一上游；逆向目录也不等于可复用源码 |
| `clshortfuse/renodx` | `renodx/`；`43c067b755a6a741f6500b2b936570f1462dcfbe`，2026-10-29 | `UpgradeToneMap`、OkLab hue及中性轴gamut已在[归属文件](../../DLSS5_NEXT-workers/f6-rewrite/Licenses/RenoDX_ATTRIBUTION.txt)明确采用 | 其颜色合成本身不是时序照明稳定器；只审过指定函数，非整库性能审计 |

用户提到的MatheusFerreiraS/OpenNR系列尚未在本次有界盘点中定位到唯一仓库和已读版本，保持“建议／待确认”。`F5-DS`也只是历史名称，不能擅自等同janblade、某条星铁路线或独立fork。未定位不等于磁盘绝不存在。

## 3. 原神桥、NGX、GIMI／ReShade 对接

| 来源 | 本地位置与身份 | 已参考／已采用 | 要参考的功能与限制 |
|---|---|---|---|
| `AizawaHikaru233/genshin_fsr_brigde` | 克隆`D:/CODE/DLSS5_NEXT2_refs/fsr_bridge`，`26b0b7b7d7a0f30f05f213237c5449cc7d6b43a6`（10-29），净；固定文本另在`D:/CODE/DLSS5_NEXT-workers/research-fsr-lead-20261108`和`research-fsr-review-20261108` | [Bridge审阅](UPSTREAM-BRIDGE-REVIEW.md)已核固定blob，研究按需FSR pass识别、多布局、间断reset；该审阅本身未复制产品实现 | schema/profile集中化、未知FSR变种发现、外桥深度／运动与HUD。**不据此重启F6内部桥或相关探测** |
| `thx114/genshin_fsr_brigde`本地魔改桥 | `D:/CODE/genshin_fsr_brigde`；origin为thx114、upstream为Aizawa；`366e778221f5b0521ba9c95838f1e731d9db26db`，dirty47项 | [历史来源](history/BRIDGE-SOURCE-REFERENCE.md)与旧技术路径记录dispatch、depth/MV和最终颜色出口研究 | 定制数据出口与HUD协商；该dirty树不代表Aizawa原文，也不证明当前F6加载了它 |
| `CXP-2024/GIMI_reshade_dlss_integration` | `D:/CODE/DLSS5_NEXT2_refs/gimi`；`cc4697187cb57bacc7c932b7f53f16be24af152a`（09-05），净；摘录`D:/CODE/DLSS5_NEXT-workers/research-gimi-lead-20261108`及`research-gimi-review-20261108` | 已审原生Device/Context IID、typed/untyped参数槽、FP16 carrier、hosted Present与绑定恢复；无整套集成证明 | 可选GIMI ABI、单Present owner、参数回收。拒绝按Evaluate次数退休；其Bridge固定来源为`620f47ca...`/1.2.3.0，不等于另写识别器 |
| `SpectrumQT/XXMI-Libs-Package` | 当前vendor：`D:/CODE/DLSS5_NEXT-workers/f6-rewrite/OptiScaler/mods/gimi/third_party`，固定`065dc746c21c71ef48bbc93cb6b90b3dcdf766e6`；旁置克隆`D:/CODE/HoyoDLSS5/_research/XXMI-Libs-v1.2.2-coexist` | [SOURCES](../../DLSS5_NEXT-workers/f6-rewrite/OptiScaler/mods/gimi/third_party/SOURCES.md)明确采用PCRE2 10.30、DXBC assembler／signature工具，禁JIT、有界操作；这不是载入整个GIMI/ReShade | shader替换与兼容需保持原签名、资源槽和GPU边界。旁置克隆`a7847ea`且有本地改动，不等于实际vendor pin |
| ReShade上游／GenericDepth | `D:/CODE/HoyoDLSS5/_research/ReShade-upstream`，本地`7bf9de8`、dirty40项；实际采用见F6`Licenses/ReShade_GenericDepth_LICENSE.txt` | GenericDepth来源用于`resource_tracking/GenericDepth_Select.h`、`native/DepthFinderCore.h`，并保留许可证 | 真实depth选择和hosted addon事件；本地ReShade克隆并非当前F6运行载荷身份 |
| HoYoShade邻接研究：`DuolaD/HoYoShade` | `D:/CODE/HoyoDLSS5/_research/HoYoShade-upstream`；另有`HoYoShade-addonpath-build`／`HoYoShade-hostguard-test`构建目录 | 已定位邻接研究／构建树，不能因此声称当前F6整体采用或本局加载 | addon、hosted Present、UI/HUD对接；启动器／邻接项目和F6产品边界分开 |
| ReShade定制树：`crosire/reshade` | `D:/CODE/HoyoDLSS5/_research/HoYoShade-single-prenr` | 实际origin是ReShade，目录名不是HoYoShade上游身份；未见F6整体采用 | 仅作launcher邻接兼容研究，与真实HoYoShade克隆分开 |

`D:/CODE/DLSS5_NEXT-workers/research-fsr-20261108`有`.git`及remote却无有效HEAD，属于未完成克隆，**不能当参考原文**。优先读上表的固定完整克隆／摘录。

外部桥HUD对接现状见[产品合同](../../DLSS5_NEXT-workers/f6-rewrite/docs/FG-BRIDGE-HUD-CONTRACT.md)：已有HUD-less/UI标签和同源双标签门，桥侧生产回调、alpha-only接入、具体新增ABI仍未完成。以真实queue/fence交接，不凭一张遮罩或SDK接受日志判定UI保护成功。

## 4. Encore、sm86 与二进制逆向

| 来源 | 对应位置与固定身份 | 已参考功能／证据 | 待参考与边界 |
|---|---|---|---|
| RTX Encore v2.0.1 | ZIP `D:/DL/RTX-Encore-v2.0.1.zip`；DLL `D:/CODE/DLSS5_NEXT-workers/f7-clean-rewrite/build/research/rtx-encore-v2.0.1/extracted/rtx-encore.dll`，42,682,368 B，SHA256 `06A7FD63EE0F0E95574F3869D70C3C9AE4AAF326BA53FF8C864064410A5F1217`；另有`rtx-encore-generic-astra`／`rtx-encore-generic-review-20261106T1306037701675Z` | 已做有界静态反汇编、DXBC、DX11→12、guide rect、NR失败不解码回写、FG输入fence和SDK标签研究；主树[研究目录](../tools/research/encore-f7/README.md)保存数学／ABI参考及固定输入。F6运行报告引用机制取舍，不等于整库移植 | 继续核精确单位、直接payload ABI、跨API同步成本；未执行目标DLL，不证明硬件支持、原神画质或6×成立 |
| Encore公开来源 | 既有研究标识`SilyNoMeta/rtx-encore@3607a171`；见[F7参考页](../../DLSS5_NEXT-workers/f7-clean-rewrite/docs/REFERENCE-RESEARCH.md) | 当时是文字／发布基准，实际机制来自上行固定二进制 | 既有“无实现源码／根LICENSE”结论仅限当时审阅，本轮不声称远端现状 |
| `sdli1995/dlssg_for_sm86`及旧SilyNoMeta分支 | `D:/DATA/Documents/ChatGPT/DLSS F5 NEXT/audit/v3-r952-dlssg-041-package-20261102/_refbin`和`_pkgtext`；未定位独立Git克隆 | [R952审包报告](<D:/DATA/Documents/ChatGPT/DLSS F5 NEXT/audit/v3-r952-dlssg-041-package-20261102/RESULT.md>)及现有`AmpereMfgLoader.cpp`／MFG许可用于加载、包结构、选项和限制研究 | backend/kernel/runtime tuple整体兼容，不能只换一个DLL。自称0.4.1的本地包被判重打包／源码归属未知，不能声称官方0.4.1已集成 |
| 历史NR参考DLL／forwarder | 入口与SHA索引在`D:/CODE/DLSS5_NEXT/tools/research/encore-f7/evidence/nr-reference-entrypoints.json`、`nr-historical-reference-exports.json`、`nr-forwarder-exports.json` | 已研究参数对象、Create/Evaluate/Shutdown、rect helper及direct/forwarder导出区分；历史参考SHA `CDFA6C1E…`与Encore DLL不同 | 不是F7选定payload，更不是当前F6模型字节证明；私有ABI须精确认证，不能仅凭导出名移植 |

共享研究已保存于主树提交`df97f06f5c2f7bdb3657633200d3c70f874c2697`。本次实际运行其 **616项CPU测试，全部通过，149.070秒**；结果在主树`build/handoff-research-tests.log`。这些测试不执行目标DLL、不认证GPU完成、也不证明游戏性能。研究内的二进制指令／反汇编／固定日志为可复核输入，不为“清目录”删除或统一改格式。

## 5. 已采用的其它来源与署名

| 来源 | 实际位置 | 已采用功能 | 待参考／注意 |
|---|---|---|---|
| PeripheralWarp（BeliyG3） | `D:/CODE/DLSS5_NEXT-workers/f6-rewrite/external/peripheral_warp`；源码明确`64902dd6` | `DlssNr_Spatial.h`、spatial HLSL沿wilsjo2 v0.8.9适配，保MIT许可；中心／边缘映射 | guide端点、量化grid和非零中心合同；压边不是语义角色mask，也不证明性能收益 |
| RenoDX / clshortfuse | 第2节摘录；`D:/CODE/HoyoDLSS5/_research/renodx`为另一邻接研究树 | 颜色合成、hue、gamut及相关MFG归属见`Licenses/RenoDX_ATTRIBUTION.txt` | 每段代码按其来源声明保留署名，不能给整个fork重标许可 |
| MFGUnlock（Dreamt / mavismmg） | `D:/CODE/HoyoDLSS5/_ref_MFGAdaUnlock`，origin `mavismmg/MFGAdaUnlock-RenoDx`，`a2ca96f`（10-02），净；产品`framegen/dlssg/MfgUnlock*.h`、`AmpereMfgLoader.cpp` | 现有MFG适配及`Licenses/MFGUnlock_LICENSE.txt` | 进程内解锁、插件加载和实际SDK倍率是不同验收项 |
| DLSS5-Feeder | `D:/CODE/HoyoDLSS5/_research/dlss5-feeder/DLSS5-Feeder-main`（源码目录，未绑定独立Git HEAD） | `OptiScaler/motion/TrustMask_Dx12.h`明确仅参考验证思想：遮挡、深度／运动／luma一致性及衰减；**本地自行实现，未复制源码** | trust mask的单位、phase、真实guides和scene cut，不能由光流假造深度；保`DLSS5Feeder_LICENSE.txt` |
| Inter字体 | `D:/CODE/DLSS5_NEXT-workers/f6-rewrite/OptiScaler/menu/font` | `Inter_Compressed.cpp`，SIL OFL许可`Licenses/Inter_OFL.txt` | 字体不是NR/FG性能算法来源 |

## 6. 官方SDK与基础依赖快照

这些是实际头文件／库／嵌入vendor快照，未必保有独立`.git`。公共根为`D:/CODE/DLSS5_NEXT-workers/f6-rewrite/external`；主树有自己的迁移基线副本，不能据同名目录认定所有文件版本相同。实际使用以[工程](../../DLSS5_NEXT-workers/f6-rewrite/OptiScaler/OptiScaler.vcxproj)、include／链接闭包和各目录许可为准。

| 来源／本地目录 | 已参考／已使用 | 需要核对的边界 |
|---|---|---|
| NVIDIA NGX／DLSS：`nvngx_dlss_sdk` | NGX SR、feature／参数和NR宿主接口的现有头文件 | 闭源模型及runtime另有字节身份，SDK头不能证明当前模型支持 |
| NVIDIA Streamline：`streamline`、`streamline1` | FG标签、options、GetState及Reflex接口；两代目录按工程使用 | F7单独审阅的SL2.14.16完整包不自动代表F6实际tuple |
| NVIDIA NVAPI：`nvapi` | 进程内驱动设置查询与FG模型覆盖 | 配置选择不证明驱动已接受，也不写驱动profile |
| AMD FidelityFX：`FidelityFX-SDK`、`FidelityFX-SDK-v2`及`OptiScaler/include`／`library` | FSR／FFX接口、版本头与实际链接库；v2部分为分发骨架 | 不能把所有目录统称已链接；内部桥禁用与保留FSR后端是不同事情 |
| AMD AntiLag2：`AntiLag2-SDK` | 现有低延迟后端接口 | 与Reflex数据口径分开，不凭目录判运行启用 |
| Intel XeSS：`xess` | SR／FG接口头和现有库 | 外部运行DLL未必齐备；本仓库DLL构建不是完整安装包 |
| Microsoft DirectX Agility：`directx_agility_sdk` | D3D12头／载荷及相关构建引用 | 运行时设备、queue与实际加载版本另核 |
| Khronos Vulkan：`vulkan` | Vulkan头／链接闭包 | DX12 WARP测试不覆盖Vulkan或全部NR历史模式 |
| LatencyFleX：`latencyflex` | 现有后端依赖／接口 | 当前游戏是否启用须另查调用链 |
| FreeType：`freetype`；Dear ImGui：`OptiScaler/`内的现有ImGui源码 | 字体和菜单UI | 非模型时间；准确源文件和构建范围见工程 |
| `magic_enum`、`nlohmann`、`simpleini` | 枚举、JSON和配置 | 按目录保原许可，不作为外部NR算法已集成证据 |
| `spdlog`、`unordered_dense` | 日志与容器 | 可测格式化／哈希开销，不能凭依赖名归因160ms |

依赖裁剪与实际保留见 [PRUNE-EXTERNAL](AUDIT/PRUNE-EXTERNAL.md)。离线shader工具、独立forwarder工程和测试输入未进入主DLL项目不等于可删。主许可和逐来源归属见产品[Licenses/README.md](../../DLSS5_NEXT-workers/f6-rewrite/Licenses/README.md)，这些来源清单不扩展闭源模型或外来包的使用／再分发许可。

## 7. 逆向工具链与其它邻接资料

| 来源 | 对应位置／类别 | 已参考与待参考边界 |
|---|---|---|
| `KhronosGroup/SPIRV-Cross`、`KhronosGroup/SPIRV-Headers`、`KhronosGroup/SPIRV-Tools` | 独立Git根分别为`D:/CODE/_ref/lecram-reverse-20261012/tools/SPIRV-Cross`、同级`SPIRV-Headers`和`SPIRV-Tools` | 逆向工具链，用于shader分析；未见作为F6产品源码采用。工具使用结果不等于游戏验证 |
| LumeniteFX | `D:/CODE/HoyoDLSS5/_research/lumenitefx/LumeniteFX-mainline`，源码快照，无独立`.git`；未绑定唯一仓库身份 | 邻接shader研究，未见F6采用；仅列待确认来源 |
| `RankFTW/rhi-repo` | `D:/CODE/HoyoDLSS5/_research/rhi-repo/rhi-repo-main`，数据库／engine-files快照；来源由launcher catalog指出 | 分发邻接资料，不能证明当前F6使用其中某个载荷 |
| Rocket 9.22 | `D:/CODE/HoyoDLSS5/_research/rocket-error-prevention`，本地二进制反汇编和分析报告 | launcher防错研究，未见F6采用；不是同名独立Git仓库 |
| XXMI局部快照 | `D:/CODE/HoyoDLSS5/_research/xxmi-coexist`，DirectX11文件与version头摘录 | 该摘录未证采用，与第3节真实XXMI克隆、产品固定vendor分开 |
| RenoDX邻接摘录 | `D:/CODE/HoyoDLSS5/_research/renodx/src`，没有独立Git根 | 该副本是否是直接来源未知；不能因此否认第2／5节已有明确颜色算法归属 |

本节这些没有独立`.git`的子目录，直接在其内运行`git rev-parse`可能返回父启动器仓库`D:/CODE/HoyoDLSS5`。必须先核对`toplevel`，不能把父仓库的HEAD当成快照版本。

## 8. 后续如何参考

优先按当前问题选读：持续历史／照明稳定看Skynizz与旧AMDNR、OpenNR、wils的表示和拒绝；Look看实际RenoDX lineage；同步与延迟看F5、官方OptiScaler及Encore的实际依赖关系；HUD/GIMI看Aizawa和CXP的源阶段与owner。每次记录**固定版本＋入口＋采用片段＋本地验证**，不要继续堆“可能支持”的名单。

同一参考库有多份旧克隆时优先使用上述已审版本，并在继续研究前读本地status。dirty参考树、摘录、包体和正式运行库不得混用；不要为了“更新参考”覆盖本地改动。远端确需跟版时另行核版本，旧报告的HEAD不冒称latest。

本清单基于有界目录检索和既有引用，覆盖当前明确使用及研究来源；未遍历所有历史worktree副本。未知仓库／功能保留未知，新增明确来源后直接补本页，不建立重复的逐轮目录。
