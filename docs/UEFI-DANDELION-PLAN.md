# uefi-dandelion 移植路线图（MT6765Pkg）

目标：把 UniXNU 的六层栈补到 dandelion 上，使 iOS 的用户态可以在这台 Redmi 9A 上启动。
本文件只做**规划与风险**，不含实现。

---

## 1. 事实基线（都来自上游仓库，不是推断）

`mouseos/UniXNU-a04br3` README 原文：

> The hardware **reaches the iPadOS home screen** with four CPUs, eMMC DMA and the
> CPU framebuffer renderer.

`unixnu.com/zh/porting/` 的六层架构：

| 层 | 来源 | 交接 |
|---|---|---|
| 1 启动镜像 | 设备仓库 `unixnu bootimg`（BootShim + 固件卷 + DTB + payload ramdisk） | 原厂 bootloader 跳到 BootShim |
| 2 **UEFI 固件** | `uefi-<device>`（Mu-Silicium fork） | 图形、输入、块设备、文件系统服务 |
| 3 启动管理器 | UniXNU `package/UniXNUPkg` → `UniXNU.efi` | 加载驱动、ACPI、kit、设备树 |
| 4 XNU 加载器 | `drivers/DarwinBoot` | 拷贝 kernel/ADT/trust cache/ramdisk，构造 `boot_args` |
| 5 内核 | 固定 XNU 源码 + `patches/xnu/*.patch` + prelink | 原生驱动 + 原版 kext，然后 launchd |
| 6 用户态 | 目标 IPSW 的 `OS` / `SystemOS` | iOS 或 macOS |

**dandelion 现在只有第 1 层和第 5 层的一部分。缺的是第 2 层。**

### Mu-Silicium fork 里已有的东西

`mouseos/uefi-a04br3`（`build_uefi.py -d <codename>`，`Status.md` 列设备）：

```
Platforms/  Asus Benesse LG Lenovo Motorola Nintendo Nothing OnePlus Qualcomm Realme Samsung Xiaomi ZTE
Silicon/MediaTek/  MT6768Pkg  MT6789Pkg  MT8167Pkg  MediaTekPkg
Silicon/{Nvidia,Qualcomm,Samsung,Silicium}
```

**关键：`MT6768Pkg` 不是占位，是完整的 SoC 包**（已核对 `main` 分支）：

```
MT6768Pkg/MT6768Pkg.dsc.inc
MT6768Pkg/Library/PlatformSecLib        SEC 阶段
MT6768Pkg/Library/GpioImplLib
MT6768Pkg/Library/ClockImplLib
MT6768Pkg/Library/PmicWrapperImplLib    PWRAP（PMIC 封装接口）
MT6768Pkg/Library/MsdcImplLib           eMMC
```

它的 `.dsc.inc` 里已经是这样写的：

```make
!if $(SOC_TYPE) == 0     # MT6768  Helio P65
!elseif $(SOC_TYPE) == 1 # MT6769T  Helio G80
```

并且 PCD 已经设了 `PcdCoreCount=8` / `PcdClusterCount=2`、
`PcdGicDistributorBase=0x0C000000`、`PcdGicRedistributorsBase=0x0C040000`、
`USE_PHYSICAL_TIMER=1`、`-march=armv8.2-a+crypto+rcpc`。

**MT6768 与 MT6765 同属 MT676x 家族**：GICD 基址一致（`0x0C000000`），
核数/簇数一致（8 核 2 簇）。所以 `MT6765Pkg` 更可能是「加一个 `SOC_TYPE` 变体」，
而不是「从零写一个 SoC 包」——工作量比最初估计低一个数量级。

已知差异（需要逐项核对）：

| 项 | MT6768Pkg | dandelion 实测 |
|---|---|---|
| GICR 基址 | `0x0C040000` | `0x0C100000`（8 帧 × `0x20000`） |
| PMIC | MT6357 系（经 PWRAP） | MT6357 |
| 时钟树 | P65/G80 表 | G25 表（MSDCPLL 等取值不同） |
| GPU | Mali-G52 | PowerVR GE8320 |


---

## 2. dandelion 的板级数据（本仓库已实测/已确认）

| 项 | 值 | 来源 |
|---|---|---|
| SoC | MT6762G / MT6765，4×Cortex-A53 | 设备 DTB |
| 页大小能力 | 4 KiB / 64 KiB（`TGran16 == 0`） | `ID_AA64MMFR0_EL1`，见 `docs/MMU_GRANULE.md` |
| DRAM | `0x48000000` 起，`0x18000000`（384 MiB 窗口） | shim 实测 |
| GICD | `0x0C000000`，64 KiB | 设备 DTB |
| GICR | `0x0C100000`，2 MiB | 设备 DTB |
| **GIC 形态** | **GICv3 + 两安全态**（`SecurityExtn=1`） | `GICD_TYPER=0x790414`，`PIDR2=0x3b` |
| eMMC | MSDC0 `0x11230000`，8-bit DDR，eMMC 5.1 | 实测 `EXT_CSD rev 8` |
| UART | UART0 `0x11002000`（MT6577 兼容） | 设备 DTB |
| PMIC | MT6357，经 PWRAP `0x1000D000` | 设备 DTB |
| 显示 | DDP `0x14000000`，NT36525B MIPI DSI，720×1600 | 设备 DTB |
| 触控 | Novatek，I²C3 `0x1100F000` | 设备 DTB |
| 引导链 | preloader → LK（无 UEFI） | 实测 |

---

## 3. 阶段划分

每个阶段都有明确的「可观测的完成判据」。建议按顺序，不要跳。

### P0 — 建立调试通道（前置条件，最高优先级）

没有 UART 就没法做 UEFI 移植。**先做 `docs/UNIXNU-EMMC-CONSOLE.md`**，
让内核/固件能把日志留在 eMMC 上。

- 判据：故意 panic，仍能取回完整日志

### P1 — UEFI 能跑起来并点亮屏幕

1. `Silicon/MediaTek/MT6765Pkg`：时钟、GPIO、PWRAP、UART、GIC、timer 的最小子集
2. `Platforms/Xiaomi/dandelionPkg`：`.dsc`/`.fdf`/`.dec`、内存映射、DXE 顺序
3. BootShim 复用本仓库已有的做法（Android v2 boot image，kernel 段放 gzip 后的固件）
4. 显示：先 `GopFramebuffer`（CPU 画），DDP 驱动二期

- 判据：屏幕出现 UEFI 的 logo/文字，且（若能用 eMMC console）有 DXE 阶段日志

### P2 — 块设备 + 文件系统 + 设备树

1. eMMC 块设备（可复用 `NativeMsdc` 的轮询 PIO 思路，或写 UEFI 侧的 MSDC DXE 驱动）
2. FAT 分区访问（放 payload、DTB、驱动）
3. 生成 dandelion 的 ADT/ACPI

- 判据：UEFI 内能读出 boot 分区里的文件并在屏幕上列出

### P3 — 接入 UniXNU.efi + DarwinBoot

1. `UniXNU.efi` 能启动并列出启动项
2. `DarwinBoot` 能拷贝内核、构造 `boot_args`、跳转

- 判据：XNU 开始打印（此时 eMMC console 必须已经在工作）

### P4 — XNU 跑到 `bsd_init` 之后

用 UniXNU 的 `patches/xnu/` 序列（不是我们自己那套）替代本仓库的补丁集。
本仓库踩过的坑可以直接贡献：GIC 两安全态、`ICC_IGRPEN0_EL1` UNDEFINED、
eMMC 继承 LK 状态、CPU `cpu_timebase_init` 相关问题。

- 判据：`waiting for root device` 之前的所有阶段无崩溃

### P5 — 驱动

显示（NativeLegacyDisplay 路线）、触控（NativeHidInput）、电源/PMIC、GPU（PowerVR GE8320，
参考 a04br3 的 GE8300 工作）。

### P6 — 用户态

IPSW 用户态 + owner root 链（`CONFIG_XNU_OWNER_ROOT_AUDIT/TRUST`）+ launchd 策略。
这部分完全沿用 UniXNU 已有机制，不需要为 dandelion 重写。

---

## 4. 风险（按严重程度）

### R1. GIC 两安全态 —— **这是硬阻塞，必须先解决**

实测结论（本仓库，真机）：

- `GICD_TYPER.SecurityExtn = 1`：Group 0 属于 EL3
- 非安全态写 `GICD_CTLR`/`GICD_IGROUPR`/`GICR_IGROUPR0` 全部只读
- 定时器 PPI 27/30 强制 pending 后 `HPPIR` 仍是 spurious
- 但 Android 在同一颗芯片上**能收到** `GICv3 30 arch_timer`

也就是说：中断配置发生在**我们够不到的特权层**（preloader/ATF），或者 Android 有额外的
私有手段。**没有中断就没有 tick，iOS 用户态跑不起来。**

a04br3 是 GICv2 单安全态，没有这个问题；所以它成功不能推广到我们这里。
这是 dandelion 全项目的头号风险。

**但有一条强线索**：`MT6768Pkg` 的 UEFI 在 `0x0C000000` 上驱动 GICv3 redistributor，
说明在 MT676x 家族的**非安全态**下 GIC 是能被驱动的。如果 MT6768 同样是两安全态，
那它必然解决了我们卡住的那一点 —— 读它的实现（见 §5 第 2 条）是当前性价比最高的一步。


### R2. 没有 UART

P0 解决（eMMC console）。但如果 UEFI 阶段在「MMU 未开、eMMC 不可用」时崩溃，
仍然看不到任何东西。缓解：UEFI 早期用 framebuffer + 屏幕上的阶段性色块。

### R3. 4 KiB 页 vs iOS 的 16 KiB 预期

Cortex-A53 不支持 16 KiB 粒度（`TGran16 == 0`）。UniXNU 已经有 `native-4k-*`
补丁系列（`native-4k-bootstrap-capacity` / `native-4k-signature-hash-cache` /
`native-4k-user-abi`），说明这条路他们走通过（a04br3 也是 Cortex-A35，同样只有 4K/64K）。
风险可控，但用户态二进制/签名的适配要跟着他们的 4K 系列走。

### R4. LK 对镜像的限制

`fastboot boot` 在 MTK LK 上不可用（已实测），每次都要写 boot 分区。
UEFI 固件体积（几 MiB）需要放进 boot 分区或单独分区，需要规划分区表
（可参考 a04br3 的 `make-a04br3-gpt.py`：boot 扩到 128 MiB、重排 userdata）。

### R5. 上游合并

a04br3 需要的 UniXNU 基础改动（`XNU_NS16550_UART`、`XNU_BOARD_INIT`、
`XNU_NATIVE_MSDC`、MTK 镜像头等）当时是「以 PR 形式准备、尚未合并」。
dandelion 大概率需要同样的 PR，要有 fork 长期存在的准备。

---

## 5. 立刻可以做的三件事

1. **P0**：把 eMMC console 做成 UniXNU 补丁（见 `docs/UNIXNU-EMMC-CONSOLE.md`），
   解掉 `UniXNU-dandelion` README 里的 no-UART 阻塞
2. **读 `MT6768Pkg` 的 GIC 与时钟实现**（最高信息量的一件事）：
   - `MdeModulePkg`/`ArmPkg` 的 GIC 基址 PCD 指向 `0x0C000000`/`0x0C040000`，
     说明在 MT6768 上 UEFI（非安全态）**确实在操作 GICv3**
   - 如果 MT6768 也是两安全态，那它就必须绕过我们卡住的那道墙 ——
     看它有没有写 `GICR_WAKER`、怎么处理 `GICD_CTLR.ARE`、是否依赖 LK 留下的状态
   - `USE_PHYSICAL_TIMER=1` 也值得注意：用 CNTP 而不是 CNTV
   这一项的结论会**直接决定 R1 是否可解**
3. **R1 立项**：GIC 两安全态是唯一可能让整个项目归零的风险，应该在写 UEFI 之前
   就排出可行路径（patch LK / 找 EL3 入口 / 复现 Android 的配置方式）


---

## 6. 与本仓库现有工作的关系

| 本仓库已有 | 在 UniXNU 路线里的用途 |
|---|---|
| BootShim（Android v2 boot image + payload 窗口） | 直接复用于第 1 层 |
| eMMC 轮询 PIO 写 | P0 的调试通道；P2 的块设备参考 |
| GIC 两安全态全部实测数据 | R1 的输入；P4 的 XNU 补丁输入 |
| `xnu-8796.141.3` 补丁集（26 个） | 可对照 UniXNU 的 `patches/xnu/` 取长补短，但不建议继续独立维护 |
| CPU/内存/PMIC/显示实测数据 | `MT6765Pkg` 与 `dandelionPkg` 的输入 |

**不建议**继续在「boot shim + 纯开源 XNU」这条线上追求用户态 —— 同一作者在同类硬件上
验证过它走不通（`xnu_*_benesse_a04br3` 三个仓库自述 “watchdog boot loop,
OS boot incomplete”）。
