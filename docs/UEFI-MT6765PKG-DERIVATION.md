# MT6765Pkg 派生清单（从 MT6768Pkg）

来源：`D:\aios\uefi-a04br3`（`mouseos/uefi-a04br3`，Mu-Silicium fork）浅克隆。
目标：为 dandelion（MT6762G，Redmi 9A）产出 `Silicon/MediaTek/MT6765Pkg`。

## 1. 上游 MT6768Pkg 的实际内容（已核对，不是占位）

```
MT6768Pkg/MT6768Pkg.dsc.inc
MT6768Pkg/Library/PlatformSecLib/{PlatformSec.c, PlatformRegisters.h,
                                  AArch64/PlatformHelper.S, *.inf}
MT6768Pkg/Library/GpioImplLib/{GpioImplLib.c, *.inf}
MT6768Pkg/Library/ClockImplLib/{ClockImplLib.c, *.inf}
MT6768Pkg/Library/MsdcImplLib/{MsdcImplLib.c, *.inf}
MT6768Pkg/Library/PmicWrapperImplLib/{PmicWrapperImplLib.c, *.inf}
```

`.dsc.inc` 关键行：

```
USE_PHYSICAL_TIMER = 1
!if $(SOC_TYPE) == 0        # MT6768 (Helio P65)
!elseif $(SOC_TYPE) == 1    # MT6769T (Helio G80)
gArmPlatformTokenSpaceGuid.PcdCoreCount|8
gArmPlatformTokenSpaceGuid.PcdClusterCount|2
gArmTokenSpaceGuid.PcdGicDistributorBase|0x0C000000
gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x0C040000
```

## 2. 必须改的硬差异（有实测依据）

| 项 | MT6768Pkg | dandelion 实测 | 依据 |
|---|---|---|---|
| GIC redistributor | `0x0C040000` | **`0x0C100000`** | shim 探针 + Android dmesg `GICv3: CPU0: found redistributor 0 region 0:0x000000000c100000` |
| GIC distributor | `0x0C000000` | `0x0C000000` ✅ 相同 | `GICD_IIDR=0x1143b` 读自该地址 |
| Core/cluster | 8 / 2 | 8 / 2 ✅ 相同 | MT6762G = 8×A53，2 簇（affinity 0x0-0x3, 0x100-0x103） |
| PPI 号 | — | **INTID 30 = arch timer**（`CNTP`） | Android `/proc/interrupts: GICv3 30 arch_timer`；`USE_PHYSICAL_TIMER=1` 与之相符 |
| MSDC0 | — | **`0x11230000`** | 设备 DTB `msdc@11230000`；内核日志 `MSDC0 version 0x20170426` |
| 串口 | MT6768 板有 UART | **无 UART 焊盘** | 本项目实测，`docs/DEAD-ENDS.md` |
| PMIC | MT6359 系 | **MT6357 / PWRAP**（待确认） | 需读上游 `PmicWrapperImplLib.c` 的寄存器基址再定 |
| 页粒度 | iOS 期待 16 KiB | **4 KiB**（`docs/MMU_GRANULE.md`） | 这是本条路线最硬的风险 R3 |

**`0x0C040000 → 0x0C100000` 是最容易漏、且一旦漏掉就表现为"UEFI 起来但拿不到定时器中断"的一处。**

## 3. 文件级动作

```
cp -r Silicon/MediaTek/MT6768Pkg      Silicon/MediaTek/MT6765Pkg
mv MT6768Pkg.dsc.inc                  MT6765Pkg.dsc.inc
# 所有 .inf 里的 MT6768Pkg 名字 → MT6765Pkg
```

然后按表改 `.dsc.inc`：

```
!if $(SOC_TYPE) == 0        # MT6765  (Helio P22)
!elseif $(SOC_TYPE) == 1    # MT6762G (Helio G25, dandelion)
gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x0C100000   # ← 关键
```

每个库的重写程度：

| 库 | 动作 |
|---|---|
| `PlatformSecLib` | 复制后只改 DDR/时钟表（MT6765 的 EMI 参数不同），`PlatformHelper.S` 基本不变 |
| `ClockImplLib` | **重写**：MT6765 的 topckgen/infracfg 位布局与 MT6768 不同（我们已有 `0x10000000/0x10001000/0x1000C000` 的实测值可对照） |
| `GpioImplLib` | 复制 + 按 dandelion 的 GPIO 基址/分组修正；无 UART 引脚需求 |
| `MsdcImplLib` | 复制 + 基址 `0x11230000`；寄存器序列可复用我们 `bootshim/msdc.c` 里已验证的 polled-PIO 版本 |
| `PmicWrapperImplLib` | 按 MT6357 改写（PWRAP 基址与寄存器表） |

## 4. 里程碑（每步都有可自动验证的判据）

| 步 | 目标 | 判据（不需要拍照） |
|---|---|---|
| M1 | `MT6765Pkg` 能在 CI 里 `.dsc` 通过、产出 `*.fd` | 构建成功 + `fd` 大小合理（≤ 4 MiB） |
| M2 | SEC 阶段起来 | 串口无、所以用**我们现成的 eMMC 日志通道**：SEC 把一行 "MT6765Pkg SEC alive" 写进 `flashinfo`，回 Android `adb` 拉取 |
| M3 | UEFI 点屏 | 屏幕出现 UEFI 输出（唯一需要看屏幕的一步） |
| M4 | MSDC 块设备 + FAT 卷 | UEFI 里 `ls` 到固件卷；日志同样走 eMMC |
| M5 | `UniXNU.efi` 加载 | UEFI 打印版本号 + 我们自己的 `DTB` 被读到 |
| M6 | `DarwinBoot` 交给 XNU | XNU 打印 `arm_init:` 起的第一批日志（复用现有内核补丁链） |

M2 是关键设计点：**UEFI 里的日志必须能在没有 UART/USB 的情况下取出**，而这条通道我们今天已经验证过（`docs/EMMC-LOG.md`）。

## 5. 打包方式

沿用现有 `bootshim/patch_boot.py`：`boot.img` = 原厂头 + BootShim + DTB + payload（这里 payload 换成 UEFI `*.fd`）。
好处是仓库里已有的刷机/取证脚本全部可复用，不需要新的工具链。

## 6. 风险（按严重度）

1. **R3 4 KiB 页 vs 16 KiB** —— 架构性，影响 `DarwinBoot`/`XNU` 的映射假设
2. **MTK GICv3 手机无人验证** —— 上游 `lancelot`(MT6768) 只跑到 **One Core only**
3. **Clock/PMIC 库要重写** —— 没有 MT6765 的 UEFI 前例可抄，只能按内核驱动反推
4. **无 UART** —— 已用 eMMC 通道缓解，但 UEFI 阶段的内存布局与我们的 shim 不同，需重新验证写入点

## 7. 与本仓库现有工作的关系

- 复用：eMMC 日志通道、boot.img 打包、CI、GIC/时钟实测数据
- 弃用：`patches/xnu/` 里为 LK→XNU 直跳写的补丁（UEFI 路线下由 `DarwinBoot` 接管 `boot_args`）
- 保留：`docs/FINDINGS-2026-10-27.md` 的实测结论仍是 UEFI 路线的输入数据
