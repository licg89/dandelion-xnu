# 下一轮开工清单（2026-10-26 收尾时状态）

手机：已恢复原系统（Android）。仓库：`5c8de28` 已推送。
本地资产：`D:\aios\artifacts\` 下有 `boot_backup.img`（原始 boot，含设备 DTB）、
`stock_boot.dtb`、`boot-usb5.img`、`boot-probe3.img`、`boot-gic-newkernel.img`。

刷机循环（MTK LK 不支持 `fastboot boot`，实测 `Booting FAILED (Status read failed)`）：

```powershell
# 长按电源 10 秒 -> 音量下 + 电源 -> fastboot
fastboot flash boot D:\aios\artifacts\boot-xxx.img ; fastboot reboot
# 恢复 Android
fastboot flash boot D:\aios\artifacts\boot_backup.img ; fastboot reboot
```

打包命令（用原始 boot 作模板，保留头部和 DTB 位置）：

```bash
cd bootshim && python3 patch_boot.py /mnt/d/aios/artifacts/boot_backup.img \
    out/Image.<mode> /mnt/d/aios/artifacts/stock_boot.dtb \
    /mnt/d/aios/artifacts/boot-xxx.img /mnt/d/aios/artifacts/payload-device.bin
```

## 任务 A：USB CDC-ACM 日志通道（需要手机配合）

已知：MUSB `POWER` 可写（回读 `0x60`）、`topckgen` 可读（`0x300`）；
`DEVCTL=0`（无 VBUS）、`INTRUSB` 从无事件、`0x11CC0000`/`0x11280000` 读 0。
结论：LK 关掉了 USB PHY/时钟门控，必须自己上电。

步骤：

1. 参考 [u-boot 的 phy-mtk-tphy.c](https://raw.githubusercontent.com/u-boot/u-boot/master/drivers/phy/phy-mtk-tphy.c)
   （它就是用这颗 PHY + MUSB 做 fastboot 的，场景一致）取 u2 上电/PLL 序列；
   Linux 版 [phy-mtk-tphy.c](https://android-kvm.googlesource.com/linux/+/f71077a4d84bbe8c7b91b7db7c4ef815755ac5e3/drivers/phy/mediatek/phy-mtk-tphy.c)
   可对照 `U2PHY_*` 偏移与 `P2C_RG_SIF_U2PLL_FORCE_ON` 等位。
2. 在 `bootshim/usb_cdc.c` 的 `usb_cdc_init()` 里加上电序列，每步用
   `fb_puthex_raw()` 打点（面板能看到走到了哪一步）。
3. 判据：`usb ctl D=` 的 VBUS 位非 0 → 主机应出现新设备（Windows 上表现为 COM 口或未知设备）。
4. 枚举通过后：内核侧在 `osfmk/kern/printf.c` 的 `dandelion_console_putc()` 里
   把字符写进 EP1 FIFO（轮询 `TXCSR.TXPKTRDY`），约 30 行，即得实时 `kprintf`。

## 任务 B：SPI 定时器（决定内核能否有 tick）

背景（真机实测）：GIC 是 GICv3、**两安全态**；`GICR_IGROUPR0` 与
`GICD_CTLR.EnableGrp1NS` 在非安全侧只读 → **arch timer 的 PPI 27 对 NS 不可见**，
XNU 原生的 FIQ/Group0 路线在这颗芯片上不可能工作。

步骤：

1. **先验证 SPI 对 NS 可见**（一次刷机就能定论）：在 `bootshim/gic_probe.c` 里
   - 写 `GICD_IGROUPR[1] = 0xffffffff` 并回读（SPI 32..63 的分组是否可写）；
   - 选一个空闲 SPI（如 INTID 200）：设 `GICD_ISENABLER`、写 `GICD_ISPENDR` 强制 pending，
     然后读 `ICC_IAR1_EL1`，看能否拿到该 INTID。
   - 若拿到 → SPI 通道可用；拿不到 → 这颗芯片所有中断对 NS 都受限，需要换思路。
2. 配 MTK GPT：DTB 的 `timer@10017000`（`mediatek,mt6765-timer`），参考 Linux
   `drivers/clocksource/timer-mediatek.c` 的 `GPT_IRQ_EN` / `GPT_CTRL` / `GPT_COMPARE`。
3. 把该 SPI 接进 XNU：`sleh_irq` 已有 DANDELION 分支（ack → 若 INTID == 定时器 → `rtclock_intr`），
   把"定时器 INTID"从 `DANDELION_TIMER_IRQ`（27）换成这个 SPI 即可。

## 已确认的硬事实（不要再重复验证）

| 事实 | 证据 |
|---|---|
| GIC 是 GICv3（GIC-500 r1p1） | `PIDR2@0xFFE8 = 0x3b`，`GICD_IIDR = 0x1143b` |
| 两安全态 | `GICD_TYPER = 0x790414`，SecurityExtn=1 |
| PPI/组控制对 NS 只读 | `IGROUPR0` 写 0xffffffff 回读 0；`GICD_CTLR` 写 0x13 回读 0x12 |
| SPI 同样不行 | `GICD_IGROUPR[6]` 写 0xffffffff 回读 0；强制 pending INTID 200 后 `IAR` 仍是 spurious |
| 两个定时器 PPI 都收不到 | `CNTP`/INTID 30 与 `CNTV`/INTID 27 强制后 `HPPIR` 都是 `0x3fc`(1020) |
| 没有 NS 可写别名 | `0x0c000000` 与 `0x0c200000`（镜像）都 read-only；`0x0c400000`（`mediatek,gic_cpu`）是空块 |
| ICC 系统寄存器可用 | `ICC_SRE_EL1=1`、PMR/CTLR/IGRPEN1 可写、`CNTFRQ=13MHz`、`CNTVCT` 在跑 |
| `fastboot boot` 不可用 | 下载 OKAY 后 `Booting FAILED (Status read failed)` |
| MUSB 控制器可写、无 VBUS | `POWER=0x60`、`DEVCTL=0x00`、主机无枚举 |
| LK 内有 USB/GIC 线索 | `lk.img` 里能搜到 `mediatek,mt6765-usb20`、`GIC_SYNC_DCM`、`MP_GIC_RGU_SYNC_DCM` |

分区备份（`D:\aios\artifacts\backup\`，通过 Magisk root + `dd` 取得）：
`lk.img`、`lk2.img`、`dtbo.img`、`boot_para.img`、`seccfg.img`、`mmcblk0boot0.img`（preloader）。
有这些就能在改引导器失败时恢复。

## 关键矛盾与下一步方向

Android（非安全）能收中断，而我们同样在非安全态却一个都收不到。差异只能来自
**安全侧/引导器对这条启动路径的配置**。因此剩下的路都指向引导器。

### 本轮的完整证据链（2026-10-26 深夜）

| 观察 | 值 | 结论 |
|---|---|---|
| Android `/proc/interrupts` | `GICv3 30 arch_timer`、`GICv3 23 arm-pmu`，其余全走 `MT_SYSIRQ` | Android 确实从 GIC 收到 **PPI 30** |
| 我们的 `CNTP_CTL.ISTATUS`（轮询中采样） | `fired yes` | 定时器**真的到期了**，不是没启起来 |
| 我们的 `CNTV_CTL.ISTATUS` | `fired yes` | 同上 |
| 两者的 `HPPIR` | `0x3fc`(1020) | 中断没有到达 CPU 接口 |
| `GICD_CTLR` | `0x12`（bit0=0, bit1=1, bit4=1） | NS 视图里 EnableGrp1NS=0 |
| 写 `GICD_CTLR=0x13` 回读 | `0x12` | bit0 写不进 |
| `GICD_IGROUPR` / `GICR_IGROUPR0` | 写 0xffffffff 回读 0 | 分组位对 NS RAZ/WI（规范如此） |
| 三个基址（0xc000000 / 0xc200000 镜像 / 0xc400000） | 都是只读或空块 | 没有 NS 可写别名 |
| INFRACFG `BUS_DCM_CTRL`（0x10001070/74） | `0x40ff0603` / `0xb47f83e3`，bit9(GIC_SYNC_DCM) 已置位 | DCM 不是原因 |
| `sysirq` @0x10200b00 | 可读（`+0x8=0x3`, `+0x18=0x8`） | MTK sysirq 对 NS 可访问 |
| `sys_timer` @0x10017000 | `[0]=0x505`, `[4]=0x100`, `[8]` 是递增计数 | GPT 在跑，NS 可读 |
| Android 内核日志 | `[Power/dcm] [GIC_SYNC_DCM 0x00000200] state:1` | DCM 由内核开启（我们已确认已开） |
| `/dev/mem` | 节点可 mknod，但 `dd` 读不出数据 | 内核未开 DEVMEM，无法从 Android 侧读寄存器 |

**结论**：我们的定时器、CPU 接口（ICC）、redistributor 使能、优先级、DCM 全部正常，
唯一对不上的就是 **`GICD_CTLR.EnableGrp1NS` 与中断分组**——这两个位在 NS 侧写不进去，
而 Android 显然工作在它们已正确配置的状态下。差别必定来自**跳转前**（ATF/LK）或
**Android 内核启动时的特权操作**，而我们目前没有任何 NS 手段去读它。

### 候选突破口（按可行性）

0. **⚠️ 已排除：在核心里写 GIC 组寄存器会挂死总线**
   从 shim（MMU 关）写 `GICD_CTLR` / `GICD_IGROUPR` 只是被静默丢弃；
   从内核（MMU 开，`ml_io_map` 得到的 Device 映射）写同样的地址，
   **内核停在 `arm_init: cpu_timabase_init`**（比正常少走十几行）。
   所以这不是"内存属性不同导致写生效"的问题，而是**该访问会 stall**。
   代码里已加注释禁止再写，见 `pexpert/arm/pe_fiq.c`。
   这也意味着 **XNU 侧永远拿不到这两个寄存器**，Android 必然是在
   更早的特权阶段把它们配置好的。
1. **反汇编 `lk.img` 找 GIC 初始化**：已做常量扫描与 ADRP 交叉引用分析，
   结论是 LK 载荷里**没有** `IGROUPR`/`CTLR` 的直接写，也没有对
   `GIC_SYNC_DCM` 等字符串的引用（这些字符串在载荷外，属于内嵌 DTB）。
   因此 LK 很可能根本不碰 GIC —— 配置发生在 **preloader/ATF（EL3）**。
2. **patch LK/preloader**：唯一还能"在跳转前"动手的地方。`lk.img`/`lk2.img`
   已备份，`preloader`（`mmcblk0boot0`）也已备份。
3. **在 Android 内核里插桩**：需要匹配 vermagic 的模块，难度高。
4. **绕开 GIC**：`sysirq`(0x10200b00) 与 `sys_timer`(0x10017000) 对 NS 可读，
   但 sysirq 的父线仍在 GIC 上，且 `/proc/interrupts` 里看不到它的 GIC 父线，
   说明这层映射需要 MTK 私有信息才能接上。
