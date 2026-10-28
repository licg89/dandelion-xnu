# UEFI 线（B）构建与打包环境

> 这份文档回答两个问题：**换一台机器怎么把 `SILICIUM_UEFI.fd` 重新做出来**，
> 以及**怎么把它变成一个能 `fastboot flash boot` 的镜像**。
> 所有路径都是本机实测值（2026-10-28）。

## 0. 一图流

```
uefi-a04br3 (Mu-Silicium 树)          dandelion-xnu (本仓库)
  Silicon/MediaTek/MT6765Pkg   ←同步←   Silicon/MediaTek/MT6765Pkg
  Platforms/Xiaomi/dandelionPkg ←同步←  Platforms/Xiaomi/dandelionPkg
  Resources/Configs/dandelion.toml ←同步← Resources/Configs/dandelion.toml
        │
        │  build_uefi.py -d dandelion -m 0 -r DEBUG
        ▼
  Build/dandelionPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd   (2 MiB)
        │
        │  tools/package_uefi_bootimg.py   (内部调 bootshim/patch_boot.py)
        ▼
  artifacts/boot-uefi-beacon.img  (16 MB, kaddr=0x48880000)
        │
        │  fastboot flash boot
        ▼
      手机
```

## 1. 环境清单

| 项 | 值 | 备注 |
|---|---|---|
| 宿主 | Windows 10/11 | |
| Linux 侧 | WSL2 + Ubuntu | `/mnt/d` 是 drvfs，构建慢是正常的 |
| Python | 3.14.4（WSL 内 `python3`） | 需要 `tomllib`（3.11+） |
| 编译器 | clang 21.1.8 + lld 21.1.8 | `CLANGPDB` 工具链，`/usr/lib/llvm-21/bin` |
| EDK2 工具 | `edk2-pytool-library 0.23.17`、`edk2-pytool-extensions 0.31.3`、`GitPython 3.2.1` | `stuart` 由 `edk2-pytool-extensions` 提供 |
| 构建树 | `D:\aios\uefi-a04br3` | `mouseos/uefi-a04br3`（Mu-Silicium fork） |
| 打包脚本 | `D:\aios\dandelion-xnu\tools\package_uefi_bootimg.py` | 依赖 `bootshim/patch_boot.py` |

安装依赖（只需一次）：

```bash
export HOME=/root                      # 见 §6 坑 1
cd /mnt/d/aios/uefi-a04br3
python3 -m pip install -r pip-requirements.txt
```

### 构建树的依赖布局（重要）

`uefi-a04br3` 的依赖是 **zip 解压**进树的，不是 `git submodule` 检出。
`git submodule status` 会显示它们「未初始化」，**这是正常的，不要去 `git submodule update`**。

但有 **4 个嵌套子模块** 必须手工补，否则 `build.py` 会在 `.dec` 的 `[Includes]`
校验处报 `error 000E: File/directory not found in workspace`：

| 目录（相对 `Mu_Basecore/`） | 上游仓库 | pin |
|---|---|---|
| `MdePkg/Library/MipiSysTLib/mipisyst` | `MIPI-Alliance/public-mipi-sys-t` | `370b5944c046bab043dd8b133727b2135af7747a` |
| `MdeModulePkg/Library/BrotliCustomDecompressLib/brotli` | `google/brotli` | `e230f474b87134e8c6c85b630084c612057f253e` |
| `SecurityPkg/DeviceSecurity/SpdmLib/libspdm` | `DMTF/libspdm` | `f55cf6d48ec69b4ac60a63903e9c6a2cb0fd155d` |
| `MdePkg/Library/BaseFdtLib/libfdt` | `devicetree-org/pylibfdt` | `cfff805481bdea27f900c32698171286542b8d3c` |

补法（**不要留 `.git`**，保持和 zip 解压一致的树形）：

```bash
git clone <url> /tmp/dep && git -C /tmp/dep checkout <pin>
robocopy /tmp/dep <目标目录> /E /XD .git        # PowerShell
# 或 rsync -a --exclude .git /tmp/dep/ <目标目录>/
```

pin 的取法：`https://api.github.com/repos/microsoft/mu_basecore/contents/<path>?ref=3c9c7b2e211e62d2a954ad760a36fe05adc43aa3`
里的 `sha` 字段（`3c9c7b2` 就是 `Mu_Basecore` 自己的 gitlink）。

## 2. 构建

```bash
export HOME=/root
cd /mnt/d/aios/uefi-a04br3
python3 build_uefi.py -d dandelion -m 0 -r DEBUG      # 增量
python3 build_uefi.py -d dandelion -m 0 -r DEBUG -c   # 干净重建
```

现成包装脚本：`D:\aios\_work\build_dandelion.sh`（额外参数原样传给 `build_uefi.py`，
日志写 `D:\aios\_work\build_dandelion_last.log`）。

耗时（本机实测）：干净重建 6–12 min，增量 3–4 min。全在 drvfs 上跑，慢是正常的。

产物：

```
Build/dandelionPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd       2,097,152 B
Build/dandelionPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd.gz       ~576 KB
Build/BUILDLOG_dandelionPkg.txt                              完整日志
Build/dandelionPkg/DEBUG_CLANGPDB/BUILD_REPORT.TXT             PCD 生效值（核对地址用）
```

`build_uefi.py` 还会顺手产出 `Mu-dandelion.img`（mkbootimg 路径）。
**那条路现在没用**：它不写 `kernel_addr`，`[boot_image].base` 里的
`0x48800000` 只是为了满足 `base + kernel_offset == FD 基址` 的一致性，
真正刷机走 §3 的 `patch_boot.py`（保留原厂 header）。

## 3. 打包

```bash
cd /mnt/d/aios/dandelion-xnu
python3 tools/package_uefi_bootimg.py --out /mnt/d/aios/artifacts/boot-uefi-beacon.img
```

默认值（都能用参数覆盖）：

| 参数 | 默认值 |
|---|---|
| `--stock` | `D:\aios\artifacts\boot_backup.img`（原厂 boot 分区 64 MiB dump） |
| `--fd` | `D:\aios\uefi-a04br3\Build\dandelionPkg\DEBUG_CLANGPDB\FV\SILICIUM_UEFI.fd` |
| `--dtb` | `dandelion-xnu/Resources/DTBs/dandelion.dtb` |
| `--out` | `D:\aios\artifacts\boot-uefi.img` |

它做的事：

1. 从 `Resources/Configs/dandelion.toml` 读 `[uefi_fd].base`（当前 `0x48880000`）
2. 调 `bootshim/patch_boot.py <stock> <fd> <dtb> <out> --kernel-addr 0x48880000`
3. 重新读输出 header，断言 `kernel_addr == [uefi_fd].base`

### 为什么不需要 BootShim

`SILICIUM_UEFI.fd` 的前 4 个字节是 `EA 23 00 14` = `0x140023EA` = AArch64
`b +0x8FA8`，跳到 FV 内部一个小 stub 去解压。所以 LK 只要把 kernel 段解压到
`kernel_addr` 再跳过去就行，**没有 shim 这一步**（`dandelion.toml` 里也没有
`[boot_shim]` 段，`build_uefi.py` 的 `append_boot_shim` 分支不会走）。

### header 里唯一被改的字段

`patch_boot.py` 只改 `kernel_addr`，其余保持原厂值 —— 那是这台机器上
**已经跑通过**的布局（见 `artifacts/boot-*.img`，都是这套 `raddr`/`tags`）：

```
page=2048  hver=2  ksize=13907597  kaddr=0x48880000
raddr=0x51B00000  rsize=2060079  tags=0x47880000
```

## 4. 校验产物

```bash
python3 /mnt/d/aios/_work/verify_bootimg.py \
  /mnt/d/aios/artifacts/boot-uefi-beacon.img \
  /mnt/d/aios/uefi-a04br3/Build/dandelionPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd
```

应当看到 `sha256(解压后的 kernel 段) == sha256(FD)` 和 `MATCH`。

再核对 FD 的链接地址（**必须**是 `0x48880000`）：

```bash
grep -m2 PcdFdBaseAddress Build/dandelionPkg/DEBUG_CLANGPDB/BUILD_REPORT.TXT
# *F PcdFdBaseAddress : FIXED (UINT64) = 0x48880000
```

## 5. 刷机与恢复

```powershell
adb devices                       # 手机在 Android 里
adb reboot bootloader
fastboot devices                  # 确认进了 fastboot
fastboot flash boot D:\aios\artifacts\boot-uefi-beacon.img
fastboot reboot
```

**恢复**（刷完卡住、黑屏、重启循环时）：

```powershell
# 长按电源键 10 s 强制关机，然后 电源 + 音量下 进 fastboot
fastboot flash boot D:\aios\artifacts\boot_backup.img
fastboot reboot
```

## 6. 已知的坑

1. **`HOME` 被 Windows 污染成 `C:Usersli`** —— WSL 里每条命令前都要 `export HOME=/root`，
   否则 `stuart` 会去 `C:Usersli` 找配置。
2. **`.S` 的 `-D` 要走 `PP_FLAGS`** —— `build_rule.txt` 预处理 `*.S` 用的是
   `"$(PP)" $(PP_FLAGS)`，CLANGPDB 下是一次独立的 `clang -E`，看不见 `CC_FLAGS`。
   `MT6765Pkg.dsc.inc` 里两处都写了（`CC_FLAGS` 和 `PP_FLAGS`）。
3. **DXE 驱动必须有 `[Depex]`** —— 少了会报
   `error 4000: No [Depex] section ... for [DXE_DRIVER] module`，加 `[Depex] TRUE`。
4. **`FVMAIN` 快满了** —— 当前 3,239,936 B 里只剩 2,200 B。再加组件前先看
   `FVMAIN [...] total/used/free`；外层 `FVMAIN_COMPACT`（2 MiB）还有 ~1.4 MB 余量，
   所以 `FVMAIN` 变大本身不是问题，**但 FD 总大小必须 ≤ `FD_SIZE`(2 MiB)**。
5. **PowerShell 写文件要 UTF-8 无 BOM** —— `[IO.File]::WriteAllText($p,$s,(New-Object System.Text.UTF8Encoding($false)))`，
   TOML/DSC 带 BOM 会让 `tomllib` / 解析器报错。
6. **长 heredoc / 引号在 PowerShell 里会被吞** —— 复杂命令一律写成 `.sh` 文件再执行。
7. **`build.py` 可能几分钟不输出** —— 在 drvfs 上做 meta-data 时就这样，
   看 `Build/BUILDLOG_dandelionPkg.txt` 和 `ps` 里的 `build.py` 子进程，不要以为卡死。
8. **`D:\aios\uefi-build` 那棵树是半截的**（上一个会话 `git clone --recursive` 中断），
   不要用它构建；它里面还有 openssl/quiche/boringssl 的残留。
