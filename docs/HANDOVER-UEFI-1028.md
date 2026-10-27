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