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
| GIC 是 GICv3 | `PIDR2@0xFFE8 = 0x3b`，`GICD_IIDR = 0x1143b`（GIC-500 r1p1） |
| 两安全态 | `GICD_TYPER = 0x790414`，SecurityExtn=1 |
| PPI/组控制对 NS 只读 | `IGROUPR0` 写 0xffffffff 回读 0；`GICD_CTLR` 写 0x13 回读 0x12 |
| ICC 系统寄存器可用 | `ICC_SRE_EL1=1`、PMR/CTLR/IGRPEN1 可写、`CNTFRQ=13MHz` |
| arch timer PPI 27 收不到 | 强制 pending 后 `IAR` 二十分钟仍是 spurious（1020/1023） |
| `fastboot boot` 不可用 | 下载 OKAY 后 `Booting FAILED (Status read failed)` |
| MUSB 控制器可写、无 VBUS | `POWER=0x60`、`DEVCTL=0x00`、主机无枚举 |
