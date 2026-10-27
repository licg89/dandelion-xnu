# 死路清单（不要重复尝试）

每一条都是真机或有据可查的来源验证过的，**不要再花时间**。

## 1. pstore / ramoops 跨重启保留日志
- 现象：preloader 在每次开机时重新训练 DRAM（EMI），预留区内容被覆盖。
- 直接证据：shim 每次开机的 `XNU TRACE (previous boot)` 恒为 `none`；
  刷回 Android 后 `/sys/fs/pstore` 里读不到我们自己写的内容。
- 即便内核配置了 `CONFIG_PSTORE_RAM/CONSOLE=y`、preloader 预留了
  `0x47C90000/0xE0000`，在这台机器上**跨重启不保留**。
- 结论：pstore 只适合"同一次上电内"的场景，对我们（刷机→重启→读日志）无效。

## 2. `fastboot boot <img>`
- MTK LK 不支持：下载 `OKAY`，随后 `Booting FAILED (Status read failed (Too many links))`。
- 每次试验必须 `fastboot flash boot` + 手动进 fastboot。

## 3. 从 Android 侧读物理内存
- `/dev/mem` 节点不存在；`mknod /dev/mem c 1 1` 能建出来但 `dd` 读不到数据
  （内核未启用 DEVMEM）。
- 结论：无法从 Android 读我们内核的寄存器/内存。

## 4. 让内核去配置 GIC distributor（**归因已更正，见 CORRECTIONS.md C2**）

> 本节原文说「内核写 GICD 会卡死总线」，这是**错误归因**。
> 真正原因是 ICC_IGRPEN0_EL1 在两安全态 GIC 上对非安全态 UNDEFINED。
> 内核不碰 Group 0 的系统寄存器即可；GICD/GICR 的 MMIO 本身不是禁忌。
- shim 里（MMU 关）写 `GICD_CTLR` / `GICD_IGROUPR` 被静默丢弃；
- 内核里（MMU 开）同样的写**会卡死总线**，机器在
  `arm_init: cpu_timabase_init` 之后一个字节都不打印。
- 这是 2026-10-26 那次"能跑到 configure: done 后来不行了"的根因。
- 结论：GIC 的 distributor/redistributor 只在 shim 里配；内核只用 `ICC_*`。

## 5. 通过 LK 反汇编找 GIC 初始化
- `lk.img` 载荷里没有 `GICD_CTLR`/`IGROUPR` 的写，也没有对 `GIC_SYNC_DCM`
  等字符串的引用（那些字符串在追加的 DTB 里）。
- 后来拿到公开的 MT6765 LK 源码（`svoboda18/lk`），确认 LK **确实**写
  `GICD_CTLR = 0x16`（ARE|EnGrp1S|EnGrp1NS）——非安全侧读回 `0x12`，与我们实测一致。
  也就是说 Group1NS 一直是开的，**我们之前写 bit0 是写错了位**。
- 结论：不需要再反汇编 LK 找 GIC；缺的是 **GICR（PPI）侧**，LK 从不碰它。

## 6. QEMU 侧读内核 trace ring
- `pmemsave` 在这台机器的 QEMU 里不可用（文件名被当表达式解析：
  `invalid char 't' in expression`），拿不到 trace/fb dump。
- PL011 镜像输出是唯一可用的 QEMU 取证方式。

## 7. USB CDC 的第一版实现（已修，记录原因）
- `ep0_send()` 往 EP0 写 `0x0001`（那是 RXPKTRDY），而 EP0 的 CSR0 里发送位是
  `TXPKTRDY = 0x0002`；
- 用 `RXCSR(0x16)` 读 EP0 状态（EP0 应该用 CSR0 = `0x12`，计数在 `COUNT0 = 0x18`）；
- 从不清 `SERVICEDSETUP`，所以 SETUP 只处理一次；
- EP1 从未设置 `TXFIFOSZ`/`TXFIFOADD`，发送 FIFO 不存在，`TXPKTRDY` 永不落。
- 任何一条都足以"控制器可读可写但主机永不枚举"。
- **PHY 基址也错**：是 `0x11CC0800`（`USBSIF_BASE + 0x800`），不是 `0x11CC0000`。
  之前"PHY 寄存器全读 0"就是这个原因。

## 8. 认定「开源 XNU 进不了系统」的说法要修正（2026-10-27 更正）

**本条之前的内容有误，已重写。**

- UniXNU **确实在非 Apple ARM64 硬件上启动真正的 iOS**：用户态来自目标 IPSW 的
  OS / SystemOS 镜像。mouseos/UniXNU-a04br3（MT8167 平板）README 原文：
  “The hardware **reaches the iPadOS home screen** with four CPUs, eMMC DMA and the
  CPU framebuffer renderer.”
- 但那条路径**不是本仓库现在走的这条**。mouseos 早期也做过 boot shim 直启 XNU，
  三个仓库自我标注为 “Experimental; **watchdog boot loop, OS boot incomplete**”，
  现在只作为「MT8167 寄存器数值的参考资料」列在 Credits 里。
- 可用路径是 UniXNU 的六层栈：
  BootShim -> UEFI(Mu-Silicium fork) -> UniXNU.efi -> DarwinBoot -> XNU + 原版 kext -> IPSW 用户态。
  **dandelion 缺的是第 2 层（UEFI）**，不是内核补丁。
- 结论修正：**只做「boot shim + 开源 XNU」到不了用户态**（同一作者在同类硬件上验证过，
  走不通）；要到 iOS 必须补 UEFI 这一层。见 docs/UEFI-DANDELION-PLAN.md。
