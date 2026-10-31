# 用 QEMU + gdb 找内存破坏：需要什么，怎么验证

写于 2026-10-31 19:2x。目标不是"在 QEMU 里跑 iOS"，而是**在一个能设 watchpoint 的环境里
找出那个会漂移的堆破坏的写者**（见 `docs/NEXT-SESSION.md` §0.-7 ~ §0.-11）。

## 为什么换环境

刷机每轮只能看到"停在哪"，看不到"谁写的"，而且每轮要人手动断电进 fastboot。
QEMU 里可以无限重跑、单步、改内存、下 watchpoint——这正是定位 UAF 需要的。

## 三块必需品（缺一不可）

### 1. 一个 QEMU 用的 boot shim

QEMU 的 `-kernel` 只是把 ELF 放到某个物理地址并跳到入口，它不知道 XNU 的启动协议。
需要一个小 AArch64 程序（几百行）替 DarwinBoot 做这些事：

| 步骤 | 参照 |
|---|---|
| 把 `mach_kernel` 放到 PA `0x4D004000`（或任意，只要和 boot_args 一致） | `artifacts/README.md`、firmware log `kernel pa 0x4D004000` |
| 填 `boot_args`（`Revision`、`MemoryMap`、`CommandLine`、`deviceTreeP` 等） | `pexpert/arm64/boot.h`；DarwinBoot 的 `log/boot.h` 用法 |
| 填 ADT blob 到 `deviceTreeP`（可以直接嵌 `Resources/UniXNU/kernel.macho.adt`，6044 字节） | `tools/mk_adt.c` / `bootshim/adt.c` |
| 打开 MMU 前的准备：EL1、DAIF 屏蔽、`SCTLR_EL1` 合法值、cache 关 | 我们 firmware log 里实测过：`CurrentEL 1 SCTLR_EL1 0x30D0180D TCR_EL1 0x280803518` |
| `x0 = boot_args 的 VA`，跳到 `_start`（mach_kernel 的入口） | `entry pa 0x4D3CB488` 那一行日志 |

### 2. 一个 QEMU 能用的控制台

**这是关键前提**：现在内核的 `printf()` 只写两个地方——eMMC 日志通道（`flashinfo` 分区，
QEMU 里没有）和 framebuffer（QEMU `virt` 默认没有、也没有我们那块 panel）。
所以即使内核在 QEMU 里跑起来，我们也**看不到任何输出**。

解法：在 `osfmk/kern/printf.c` 的 DANDELION 输出路径里加一个 **PL011 UART sink**
（QEMU `virt` 的 PL011 在 PA `0x09000000`），由 boot-arg（例如 `-dl_uart`）或"检测到
PL011 的 ID 寄存器"来启用。这样 QEMU 里就有真正的串口控制台，而手机上的行为不变。

### 3. gdb 脚本

- `qemu-system-aarch64 -M virt -cpu cortex-a53 -m 2G -s -S -kernel <shim.elf>`；
- 连上后在 `zalloc_validate_element` 命中处下断点（或直接在 `shared.kalloc.16` 的
  zone 上设硬件 watchpoint——QEMU 支持 `watch` 命令）；
- 命中时 `bt` 就是写者的调用栈。

## 验收标准（按顺序）

| # | 验收 | 说明 |
|---|---|---|
| 1 | QEMU 里出现第一行内核输出（PL011） | 证明 shim + boot_args + ADT 都对 |
| 2 | 走到 `arm_init` / `arm_vm_init` | 证明 MMU/pmap 路径在 QEMU 里也成立 |
| 3 | 走到 `bsd_init: done` | 与手机上一致 |
| 4 | 复现 `shared.kalloc.16` 的 UAF 或在 `execve` 处停住 | **这一步才是目的**：能复现就能用 gdb 抓写者 |
| 5 | `bt` 给出写者函数名 | 结论 |

若第 4 步在 QEMU 里**不复现**，那本身也是重要信息：说明破坏与 MT6765 的某个设备/
时序相关，回到手机继续（但那时我们至少有了一个能跑的对照环境）。

## 现成的参照

- [UniXNU/ArmVirtPlatform](https://github.com/UniXNU/ArmVirtPlatform)：QEMU virt 的
  platform expert（不是 loader，但说明这条路他们在走）；
- HakiOS 的 `make qemu-open`（`hakios-xnu-kernel`）在 QEMU 里跑同一个内核、不含任何
  Apple 东西——它的 loader/脚本是最值得抄的部分；
- XNU 自带的 `MACHINE=bcm2837` 配置也曾在 QEMU raspi3 上被跑过，可作为"最小可行"对照。

## 现实提醒

- QEMU 的设备与 MT6765 完全不同：SCP、MSDC、DDP/Mali、PMIC 都没有。我们的 dandelion
  补丁里有不少按设备地址写死的探测（GIC、eMMC、framebuffer、trace ring），在 QEMU 里会
  读到 0 或 fault——**shim 里要把这些探测跳过或让它们安全失败**，否则内核在 QEMU 里
  根本到不了 `bsd_init`。这也是为什么第 1 步必须是"先让一行输出出来"。
- 这件事本身是几小时到一两天的工作量，但**不需要手机、不需要人守着**，而且做完之后
  定位 UAF 是一次 gdb 断点的事。
