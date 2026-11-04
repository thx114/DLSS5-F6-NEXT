# tools/shaders — Look 着色器再生成 recipe（已改路径守卫，未生成 shader）

`regenerate-look-shaders.py` 从旧根原样复制后，只改了**路径解析与守卫**，编译/反射/ABI/生成逻辑一字未动。

- 来源：`D:/DATA/Documents/ChatGPT/DLSS F5 NEXT/worktrees/r5-shader-tool-path/audit/r5-shader-tool-path-20261018/regenerate-look-shaders.py`（原 27,884 B / 522 行）
- 本次改动仅 4 处：`PROJECT` 推导、三个目标根常量、`--source/--evidence/--cleanup` 的帮助文字与守卫、新增 `require_output_dir()`。

## 它做什么

用 `dxc.exe` 把 `OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl` 编成 DXIL 与 SPIR-V，校验指令与 `Params` 偏移（`PARAMS_SIZE=312`、`MASK_OFFSET=308`、`gLook*` 240–308），再重写 `DlssNr_Shader.h` / `DlssNr_Shader_Vk.h` 两个内嵌头，前像备份到 `--cleanup`，证据写 `--evidence`。

## 现在的路径规则

| 项 | 规则 |
|---|---|
| `PROJECT` | `Path(__file__).resolve().parents[2]` —— 由脚本自身位置推导，本根下即 `D:\CODE\DLSS5_NEXT`，不再硬编码旧根 |
| `--source` | 必须**等于** `PROJECT/OptiScaler`（解析后），传旧根或任何其它树都被拒 |
| `--evidence` | 必须位于 `PROJECT/build/shaders/evidence` 之下，且是**新目录**；拒绝根本身、已存在目录 |
| `--cleanup` | 必须位于 `PROJECT/build/shaders/backups` 之下，且是**新目录**；拒绝根本身、已存在目录 |
| 其它 | 三个路径都要求绝对路径；evidence/cleanup 互不包含；其余校验（输入 hlsl/hlsli、dxc 及伴生 DLL、并发字节一致性）原样保留 |

## 当前示例命令（绝对路径）

```powershell
python "D:\CODE\DLSS5_NEXT\tools\shaders\regenerate-look-shaders.py" `
  --source   "D:\CODE\DLSS5_NEXT\OptiScaler" `
  --evidence "D:\CODE\DLSS5_NEXT\build\shaders\evidence\look-20261104-01" `
  --cleanup  "D:\CODE\DLSS5_NEXT\build\shaders\backups\look-20261104-01"
```

`--dxc` 可省略，默认用 `D:\CODE\DLSS5_NEXT\OptiScaler\shaders\shader_tools\dxc.exe`（同目录 `dxcompiler.dll`、`dxil.dll` 已就位）。输出目录落在 `build/` 下，已被 `.gitignore` 覆盖。

## 本次验证到哪一步（重要）

**只做了参数与语法验证，没有生成任何 shader**：

- `py_compile` 语法通过；`--help` 正常输出。
- 五条错误路径均按要求拒绝且**零写入**（`build/shaders` 目录根本没有被创建）：旧根 `--source`、`--evidence` 指向根本身、`--evidence` 越界、`--cleanup` 越界、相对路径。
- 未运行成功路径：它会**原地改写产品源码的两个内嵌头**并写备份；实际再生成后需要重新构建和验证。

脚本自带的 `status` 语义不变：成功也只到 `SHADERS_GENERATED_ONLY`（`targetedCompile/fullBuild/dllBuild = NOT_RUN`、`deployed=false`），不等于构建、部署或画面验收。

## 迁移取舍

只复制脚本，未复制同目录 `REPORT.md`、`__pycache__/`（证据/缓存）；未整合 `OptiScaler/shaders/shader_tools/*.bat`，本工具不依赖它们。
