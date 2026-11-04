# F6 构建入口与保留增量

Root为唯一产品集成、根构建部署和游戏CLI owner；当前产物/失败边界见[HANDOFF](HANDOFF.md)，规则见[AGENTS](AGENTS.md)。R1077只整理路径与文档，修改后的入口尚未运行，不为整理新增构建或验证。

```powershell
& .\BUILD-DEPLOY.ps1 -Variant F6
```

省略Variant也默认F6；显式Main在扫描/编译/部署前拒绝并exit1，不恢复冻结F5。

|用途|实际位置|
|---|---|
|唯一物理产品源码|根 `OptiScaler/`|
|活依赖|根 `external` junction→`work/full-source-f5-v11-candidate-20261018/external`，真实目标原位保护|
|兼容MSBuild入口|`work/f6-source/OptiScaler` junction→根产品；`SolutionDir`/工作目录仍为字面 `work/f6-source`；其.git/注册不动|
|mtime扫描|仅根OptiScaler与external解析出的活依赖；不递归audit/worktrees/整个协调根|
|有效增量/固定产物|`work/f6-build/obj-main`、`compiler-temp`、`out/OptiScaler.dll`；PCH/PDB/tlog原位保留|
|唯一正式新部署|`D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/OptiScaler/official/F6/OptiScaler.dll`|
|冻结输入|`work/r294-r266-build`与正式同级 `DLSS F5` 覆盖包|
|调用shell|`C:/Program Files/PowerShell/7/pwsh.exe`，login=false|

沿现有Release x64 Build、MSBuild `D:/VS2022BuildTools/MSBuild/Current/Bin/amd64/MSBuild.exe`；`IntDir/OutDir`、串行选项、`PostBuildEventUseInBuild=false`保持。只用输入/artifact/正式DLL的LastWriteTimeUtc决策，报告actual exit与mtime；不清obj/out/PCH/PDB，不touch源码制造重编，不增加TU/tests/hash/manifest/fixture/SDK检查或部署后检。兼容字面路径旨在保增量，不保证下次完全零重编。

正式流写原对象以保runtime硬链接；不以Copy/rename替换正式DLL、不改profiles。实际编译失败按exit停；只有编译成功且明确正式共享占用，才沿既有 `-StageOnly` 旁置同目录 `.pending`，报告入口/旁置实际exit与mtime及STAGED-ONLY。其他I/O失败不冒旁置成功；不扫描/强关未知进程。

## Look shader既有recipe

只在实际修改相关shader时由明确owner使用：`worktrees/r5-shader-tool-path/audit/r5-shader-tool-path-20261018/regenerate-look-shaders.py`，`--source`传根OptiScaler绝对路径；`--evidence`、`--cleanup`沿现有recipe。历史调用见 `audit/r484-layer-shader-generation-20261020/LUNA-GENERATE.ps1`。工具所在注册worktree、生成输出/reflection/offset/receipt都保留，不另造dxc或验证层，不为整理重新生成。

R1073交件记录现mask版本global ABI20261018、layer ABI20261020、DXIL mask ABI20261104；LookMode240、Halo300、HaloBrightSide304、mask308、Params312bytes、consumer CB512。历史Params308是mask扩展前记录，不据旧说明回滚现声明；本轮未执行recipe或复核嵌入二进制，不冒新的shader验收。

旧工具定位仍保：`recovered-temp/temp-project-retention-20261018/F5-COMBINED-20261017/source/OptiScaler/shaders/shader_tools` 的便携DXC1.9.2602.17及同套dxc/dxcompiler/dxil；SDK10.0.26100.0的DXC1.8.2502.11历史实际SPIR-V失败，不凭help宣称双格式生成。历史MSBuild17.14.60.43110、MSVC14.44.35207与Python `D:/APPS/conda/python.exe`仅工具导航，不作为每轮检查前置。
