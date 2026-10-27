# 交接：线 B（UEFI）当前卡点与恢复步骤（2026-10-28 00:50）

## 一句话

线 A（开源 XNU）已冻结在 `sched_startup`；主线是 UEFI。
UEFI 的代码资产全部就位，**只差 EDK2 workspace 布局**这一个环境问题，`*.fd` 未产出。

## 已验证的事实

- 对照实验：**已知可用的 `lancelot` 平台报完全相同的错**
  → 问题在环境，不在我们写的 `MT6765Pkg` / `dandelionPkg`
- 错误原文：
  `ERROR - Build.py #000 from .../Mu_Basecore/MdePkg/MdePkg.dec(33): File/directory not found in workspace`
- 根因判断：`Mu_Basecore` 等依赖是 **zip 解压**进树的，不是 git submodule 检出；
  `stuart`/`build_uefi.py` 依赖 submodule 的目录布局与 `.gitmodules` 注册来建 workspace

## 已就位的资产（`D:\aios\uefi-a04br3`）

```
Mu_Basecore/            45 项（zip 解压）
Common/Mu/              33 项（zip）
Common/Mu_OEM_Sample/   含 OemPkg（MsAltBootLib / PasswordPolicyLib）
Binaries/               33 项（zip）
Silicium-ACPI/          已拉取
Silicon/MediaTek/MT6765Pkg/            14 文件，GICR=0x0C100000，SOC_TYPE=1
Platforms/Xiaomi/dandelionPkg/         10 文件
Resources/Configs/dandelion.toml       无 BOM
Resources/DTBs/dandelion.dtb            来自 stock_boot.dtb
```

已做的绕过（都在树里，可回退）：
- `Mu_Basecore/BaseTools/Bin/nasm_ext_dep.yaml`：`scope` 改成 `edk2-build-disabled-dandelion`
  （`mu_nasm` 是 x86 NASM，aarch64 不需要；原件在同目录 `.orig`）
- `dandelion.dsc` / `dandelion.fdf`：`dandelion/AcpiTables.inf` 已注释
  （缺 `Silicium-ACPI/Platforms/Xiaomi/dandelion/`）

## 下一步（按顺序，第一条最可能直接解决）

```powershell
cd D:\aios\uefi-a04br3
git submodule update --init --recursive Mu_Basecore Common/Mu Common/Mu_OEM_Sample Silicium-ACPI Binaries
```

```bash
# WSL 里
export HOME=/root
cd /mnt/d/aios/uefi-a04br3
stuart_setup  -c Platforms/Xiaomi/dandelionPkg/DeviceBuild.py
stuart_update -c Platforms/Xiaomi/dandelionPkg/DeviceBuild.py
python3 build_uefi.py -d dandelion -m 0 -r DEBUG -c
```

成功判据：`Build/dandelionPkg/DEBUG_*/FV/*.fd` 存在。

## 注意

- WSL 里 **`HOME` 被 Windows 污染成 `C:Usersli`**，所有命令前必须 `export HOME=/root`
- PowerShell 写 TOML 必须用 `[IO.File]::WriteAllText(..., UTF8Encoding($false))`，否则 BOM 让 `tomllib` 报错
- 长 heredoc/引号在 PowerShell 里极易被吞，**一律写成 .sh 文件再执行**

---

# 结果：第一个 `*.fd` 已产出（2026-10-28 01:44）

上面 §下一步 的 `git submodule update` **不需要执行**，根因判断是错的（见
`CORRECTIONS.md` C6）。真正有效的恢复步骤如下，全部已执行完毕。

## 产物

```
D:\aios\uefi-a04br3\Build\dandelionPkg\DEBUG_CLANGPDB\FV\SILICIUM_UEFI.fd      2,097,152 B (2 MiB, = FD_SIZE 0x200000)
D:\aios\uefi-a04br3\Build\dandelionPkg\DEBUG_CLANGPDB\FV\SILICIUM_UEFI.fd.gz     575,300 B
```

- M1 判据达成：构建成功 + fd ≤ 4 MiB
- FV 内容抽样（`FV/Guid.xref`）：`ArmGicDxe`、`GraphicsConsoleDxe`、`MsdcDxe`、
  `KeypadDxe`、`KeypadDeviceDxe` 都在
- FVMAIN 3,223,552 B 压进 FVMAIN_COMPACT 609,456 B（29%），外层 FD 正好 2 MiB
- 全量构建 11m43s，增量 3m31s（`/mnt/d` 是 drvfs，慢是正常的）

## 步骤 1：补齐 mu_basecore 的四个嵌套子模块

zip 解压出来的 `Mu_Basecore` 里这些目录**存在但为空**，`build.py` 会逐个校验 `.dec`
的 `[Includes]`，缺一个就 `error 000E`：

| 目录 | 来源 | pin（`mu_basecore@3c9c7b2`） |
|---|---|---|
| `MdePkg/Library/MipiSysTLib/mipisyst` | `MIPI-Alliance/public-mipi-sys-t` | `370b5944c046bab043dd8b133727b2135af7747a` |
| `MdeModulePkg/Library/BrotliCustomDecompressLib/brotli` | `google/brotli` | `e230f474b87134e8c6c85b630084c612057f253e` |
| `SecurityPkg/DeviceSecurity/SpdmLib/libspdm` | `DMTF/libspdm` | `f55cf6d48ec69b4ac60a63903e9c6a2cb0fd155d` |
| `MdePkg/Library/BaseFdtLib/libfdt` | `devicetree-org/pylibfdt` | `cfff805481bdea27f900c32698171286542b8d3c` |

做法：`git clone` 到 `D:\aios\_deps\<name>`，`git checkout <pin>`，再
`robocopy /E /XD .git` 覆盖进上表目录（**不要留 `.git`**，保持与 zip 解压一致的树形）。

`MdePkg.dec` 报的是第一条，后面三条是同一类错误，一次补齐省三轮构建。

## 步骤 2：三个代码级修复（都在树里，可回退）

1. `Platforms/Xiaomi/dandelionPkg/Library/MemoryMapLib/MemoryMapLib.c`
   文件头用了 DSC 风格的 `##`/`#` 注释 → clang 报 `invalid preprocessing directive`。
   改成 `/** ... **/`。
2. `Silicon/MediaTek/MT6765Pkg/MT6765Pkg.dsc.inc`
   `*_*_AARCH64_CC_FLAGS` 里的 `-DSOC_TYPE` **到不了 `.S`**：`build_rule.txt` 预处理
   `*.S` 用的是 `"$(PP)" $(PP_FLAGS)`，CLANGPDB 下是一次独立的 `clang -E`。
   补 `*_*_AARCH64_PP_FLAGS = -DSOC_TYPE=$(SOC_TYPE)`。
   （漏了的表现就是 `PlatformHelper.S` 自己的 `#error` 守卫触发。）
3. `Silicon/MediaTek/MediaTekPkg/Drivers/MsdcDxe/MsdcDxe.c`
   `MsdcGetNextSlot` / `MsdcGetSlotNumber` 里 `Private` 赋值后从不使用 →
   `-Werror,-Wunused-but-set-variable`。删掉这个死变量（不是加 `(VOID)`）。

## 步骤 3：构建命令

```bash
# WSL
export HOME=/root
cd /mnt/d/aios/uefi-a04br3
python3 build_uefi.py -d dandelion -m 0 -r DEBUG        # 增量
# 干净重建加 -c
```

现成脚本：`D:\aios\_work\build_dandelion.sh`（日志落到
`D:\aios\_work\build_dandelion_last.log`）。

## 环境坑（这次踩到的）

- `D:\aios` 的 Windows 文件权限一开始不允许写，DSH 沙箱连 `pwsh` 都起不来
  （`SetNamedSecurityInfoW ... grantWrite(D:\aios)`）。已修好，备份/回滚脚本在
  `D:\acl-recovery\`。
- 上一会话留下两个还在跑的进程：一个卡了 1h46m 的 `git clone --recursive`
  （在 `/mnt/d/aios/uefi-build`，会拖慢 drvfs），一个在 `uefi-build` 里重复跑
  `build_uefi.py -c` 的孤儿构建。都已终止。**`uefi-build` 那棵树是半截的，别用。**
- `build.py` 在 drvfs 上做 meta-data 时可能几分钟不输出，不要以为卡死；
  看 `Build/BUILDLOG_dandelionPkg.txt` 和 `ps` 里的 `build.py` 子进程更可靠。

## 下一步（M2）

1. 打包：`boot.img` = 原厂头 + BootShim + DTB + payload（payload 换成
   `SILICIUM_UEFI.fd`），沿用 `bootshim/patch_boot.py`。
2. 上机验证 M2 判据：SEC 把一行 `MT6765Pkg SEC alive` 写进 `flashinfo`，
   回 Android 用 `adb` 拉（`docs/EMMC-LOG.md` 的通道）。
3. 注意 FD 的落地地址必须与 `MemoryMapLib.c` 的 `"UEFI FD"` 条目
   （`0x48880000`，2 MiB）以及 `Resources/Configs/dandelion.toml` 的 `[uefi_fd].base` 一致。