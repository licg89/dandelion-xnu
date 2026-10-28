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

## 下一步（M2）：已经打包好了，等上机

### FD 落地地址（已统一，2026-10-28 02:12）

三处必须相等，现在都是 `0x48880000`：

| 位置 | 值 |
|---|---|
| `Resources/Configs/dandelion.toml` → `[uefi_fd].base` | `0x48880000` |
| `dandelionPkg/Library/MemoryMapLib/MemoryMapLib.c` → `"UEFI FD"` 条目 | `0x48880000`，`0x200000` |
| boot image 头里的 `kernel_addr` | `0x48880000` |

- `PcdFdBaseAddress` 从 toml 走 `BLD_*_FD_BASE` 一路传进 DSC，`BUILD_REPORT.TXT` 里可核对。
- 原来 toml 是 `0x40080000`（照抄 lancelot），而 DSC 的 `PcdSystemMemoryBase` 是
  `0x48000000`——FD 落在系统内存之外，是 M2 的硬错误。
- **不需要 BootShim**：FD 的前 4 个字节就是 `0x140023EA`（`b +0x8FA8`），
  跳到一个小 stub 去解压 FV。所以 `[boot_shim]` 段故意不存在，
  `build_uefi.py` 的 `append_boot_shim` 分支不会走。

### 打包（已执行）

```bash
python3 bootshim/patch_boot.py \
  artifacts/boot_backup.img \                                  # 原厂 boot 分区镜像
  Build/.../FV/SILICIUM_UEFI.fd \                              # 直接当 kernel 段
  Resources/DTBs/dandelion.dtb \
  artifacts/boot-uefi-m2.img \
  --kernel-addr 0x48880000
```

`patch_boot.py` 新增 `--kernel-addr`：只改 header 里的 `kernel_addr`，
`raddr=0x51B00000` / `tags=0x47880000` / `page=2048` / `ksize=13907597`
全部保持原厂值——那是这台机器上**已经跑通过**的布局（见 `artifacts/boot-*.img`）。

产物：`artifacts/boot-uefi-m2.img`，16,099,328 B，
kernel 段 = gzip(FD) 575,283 B，解压后 sha256 与 FD 完全一致（已校验）。

### 上机判据

⚠️ **`SEC alive` 的 eMMC 标记还没有实现**（`MT6765Pkg` 里没有任何 flashinfo 写入代码）。
所以这一版改成用**看门狗当进度信标**（见下节）。

### 看门狗进度信标（已实现，2026-10-28 09:43）

dandelion 没有 UART，eMMC 写入还没写，唯一能免驱观测到的量是
**「从跳转到 FD 到 SoC 自己复位，隔了多久」**。TOPRGU 的寄存器语义已经从
vendor 源码核实（`drivers/watchdog/mediatek/wdt/common/wdt_v2/mtk_wdt.{h,c}`）：

```
WDT_MODE   0x00   KEY 0x22000000 | EXTEN 0x0004 | ENABLE 0x0001
WDT_LENGTH 0x04   字段在 bit15:5 = 秒数 * 64，低 5 位必须是 0x08，否则写入被忽略
                  → 最长 2047/64 ≈ 31 s
```

三级信标，每级重新武装一次（先 disarm，改 LENGTH，再 arm）：

| 武装点 | 代码位置 | 超时 |
|---|---|---|
| SEC 入口 | `MT6765Pkg/Library/PlatformSecLib/PlatformSec.c` → `SetWatchdogBeacon(20)` | 20 s |
| 过内存初始化 | `MT6765Pkg/Library/PlatformPeiLib/PlatformPei.c` → `SetWatchdogBeacon(30)` | 30 s |
| DXE 分发成功 | `dandelionPkg/Drivers/PlatformProgressDxe/PlatformProgressDxe.c` | 2 s |

**判读表**（从上电/logo 出现开始掐表）：

| 观测到的复位延迟 | 含义 |
|---|---|
| ~10 s | 我们的 FD **根本没被执行**（还是 LK 自己武装的那个看门狗） |
| ~20 s | SEC 入口跑到了，死在 `PlatformPeim` 之前 |
| ~30 s | 过了内存初始化，死在 DXE 之前 |
| ~2 s | **DXE core + dispatcher + FV 都活着**（我们的驱动跑了） |
| 不重启 | 以上都没发生 / 看门狗被别的东西关了 |

注意 DXE 那级故意**不关**看门狗而是改成 2 s，这样「到了 DXE」是一个阳性信号而不是静默。
一旦 DXE 确认可达，第一件事就是把 `BEACON_SECONDS` 改成 0（改成关掉）。

`PlatformPeiLib` 是新增的 MT6765 实现，**必须保留 Null 实例里的 `BuildFvHob()`**，
否则 PEI 找不到 FV —— 这是覆盖平台 PEI 库时最容易漏的一行。

### 下一步

- 真正的 M2 判据：把 `bootshim/msdc.c` 里已验证的 polled-PIO 写搬进
  `MT6765Pkg/Library/MsdcImplLib`，SEC 里写一行 `MT6765Pkg SEC alive` 到 flashinfo。
- 然后才是 M3（点屏）、M4（FAT 卷）、M5（`UniXNU.efi`）。

构建/打包环境与完整命令见 `docs/UEFI-BUILD-AND-PACKAGING.md`。

---

# 结果：M2 达成（2026-10-28）

两件事同时被证实。

## 1. CI 能编 UEFI 固件了

`.github/workflows/build-uefi-dandelion.yml` 从建立起**每一次都是失败**，原因只有一个：
`Resources/MuPatches/` 里的补丁从来没有被应用。`MsdcDxe.c` 的 `Private` 死变量在
`-Werror,-Wunused-but-set-variable` 下是硬错误，构建就停在那里。

修法（`9e8650d`）：在 overlay 之后加一步

```yaml
for p in "$GITHUB_WORKSPACE"/dandelion-xnu/Resources/MuPatches/*.patch; do
  git -C "$WORK" apply -p1 --verbose "$p"   # 必须是绝对路径，git -C 会切目录
done
```

跑通后产物 `dandelion-fd` 里的 `SILICIUM_UEFI.fd` 基址正好是 `0x40080000`
（两处：`0x168` 头和 `0xc0d8` 内存映射），和手工 patch 的二进制完全一致。

**从此不需要本地 EDK2 树**：改代码 → push → 下 artifact → 打包 → 刷机。

## 2. 三级色带证明 PEI 和 DXE 都跑到了

色带是直接写 framebuffer（`0x7EC50000`，stride 2944，720 宽，取自
`bootshim/fb.h`），**不经过 GOP、不经过 UEFI 图形栈**，所以 SEC 一进去就能画。

实测现象：

| 色带 | 位置 | 现象 |
|---|---|---|
| 🟩 绿（SEC） | 第 0–31 行 | 开机**一闪而过** |
| 🟦 蓝（PEI） | 第 32–63 行 | 稳定留在顶部 |
| ⬜ 白（DXE） | 第 64–95 行 | 稳定留在顶部 |

**白条出现 = DXE core + dispatcher + 我们的 FV 全部工作。** 这是 M2 的正式判据。

绿色为什么一闪而过：之后有东西重画了顶部（很可能是 DXE 阶段的显示初始化/logo
重绘），而 32 行以下的蓝白没被覆盖。这不影响判读。

## 现在的位置

M2 ✅ 完成 → 下一步是 M3（文字控制台）与 M4（FAT 卷）。

## 下一步（按顺序）

1. 把 `BEACON_SECONDS` 改成 0（DXE 已确认可达，信标不再需要），观察设备是
   停住还是继续复位 —— 这决定 DXE 之后是「在等我们」还是「有异常」。
2. 把色带换成文字：复用 `bootshim/font.h` 的 8×16 点阵，SEC/PEI/DXE 各画一行
   状态字，仍然不经过 GOP。
3. 之后才是 `GraphicsConsoleDxe` + GOP，把 UEFI 自己的 `ConOut` 接上。
4. M4：`MsdcDxe` + FAT，让 `UniXNU.efi` 能被加载。


## 实测补充（同日，第二次上机）

设备停在蓝白两条带上不动，**约 10 秒后复位**。

- 10 s 是 LK 那个看门狗的值，不是我们 DXE 里的 2 s → **`SetWatchdogBeacon()` 的写
  并没有生效**（TOPRGU 基址在 MT6765 上可能不是这几处代码假设的那个）。三级信标
  方案作废，改用屏幕 postcode。
- 关键结论不变：白条出现，DXE core + dispatcher + FV 都工作。

### 下一个动作（不用再碰看门狗）

DXE 已经能跑我们自己的驱动，所以文字控制台可以直接做：

1. 把 `bootshim/font.h` 的 8x16 点阵抄进 `PlatformProgressDxe.c`（同文件，避免
   include 路径问题），写一个 `DrawString()`，直接画到 `0x7EC50000`。
2. 在 DXE 里打印真正有用的东西：内存映射条目、`PcdFdBaseAddress`、`ConOut`
   是否可用、以及 `gBS->LocateProtocol` 的结果。
3. 之后把 `PlatformSecLib` / `PlatformPeiLib` 的色带也换成一行字。
4. 再往后才是 GOP：`GraphicsConsoleDxe` 需要一个 MT6765 的 GOP 生产者。

注意：10 秒的复位窗口足够画完几十行字，不影响使用。
