# 进度：dandelion 上的 XNU 跑到哪了

最后更新：2026-10-31 02:0x。判据一律是日志原文（`docs/logs/`），不是"应该"。

## 一、整条链

| 阶段 | 状态 | 证据 |
|---|---|---|
| LK → BootShim | ✅ | `PayloadBoot: emmc handoff … msdc ver 0x20170426` |
| UEFI → eMMC BlockIo → FAT | ✅ | `PayloadBoot: FAT OK` / `found \UniXNU.efi` |
| UniXNU → UniXNUDxe → DarwinBoot | ✅ | `DARWINBOOT: loader v2 installed EL1` |
| ExitBootServices → 跳转内核 | ✅ | `DARWINBOOT: hand-off to the kernel`，`CurrentEL 1`，`pa 0x4D004000` |
| XNU `arm_init` / `arm_vm_init` / pmap / 256 MiB 窗口 | ✅ | 7000+ 行早期日志（`kernel-log-2026-10-30-handoff` 等） |
| 内核日志通道（eMMC `DLXLOG01`） | ✅ | 每次启动 3400+ 行，`tools/round.ps1 collect-log` 一次拿全 |
| **GIC：Group 1 使能** | ✅ 2026-10-31 | `ctlr rwp-waited 10 -> 12`（NS 视图下 bit 1 才是 Group 1 使能） |
| **定时器中断送达** | ✅ 2026-10-31 | `irq N intid=30`，INTID 30 = EL1 非安全物理定时器 PPI |
| **tick 稳定运行** | ✅ 2026-10-31 | `gap=130012 ticks ÷ 13 MHz = 10.0009 ms`，`irq 50/100/150` 一致 → 100 Hz |
| `rtclock_intr()` / `timer_intr()` | ✅ 2026-10-31 | 每次中断 `rtc 1..4` 四个标记齐全 |
| `bsd_init()` | ✅ 2026-10-31 | 探针跑完、`setconf()` 返回、`while` 循环进入并退到挂载后的代码 |
| 根设备出现（RAMDisk → md0） | ✅ 2026-10-31 | `Added memory device md0/rmd0 … for 0x1000`、`BSD root: md0, major 1, minor 0` |
| 根文件系统挂载（mockfs） | ✅ 2026-10-31 | `mockfs_mountroot: root device 4096 bytes`、`vfs_mountroot -> 0`、`[dl] root mount: success` |
| `VFS_ROOT`（拿到 `/` 的 vnode） | ✅ 2026-10-31 | 之后仍有输出：`bsd_init: calling devfs_kernel_mount` |
| `devfs_kernel_mount` | ❌ **当前卡点** | data abort，`far=0x30`，`pc=_SHA256_Init+0x10`，`lr=_vm_kernel_addrhash_internal+0x6c` |
| `load_init_program()` / launchd | ❌ | 依赖上一行；还需把 mockfs 的 file node 换成真正的 arm64 Mach-O |

## 二、2026-10-30 晚 → 31 凌晨这一轮解决了什么

1. **"GIC 写不进去"是写错了位**：`ARE_NS=1` 时 `GICD_CTLR` bit 0 是 RES0，
   Group 1 使能是 bit 1。写进去后 `10 -> 12`，中断立刻到达。
2. **"内核静默死亡"是我自己的诊断代码**：IRQ handler 里调 `printf()` 会
   `panic("Console I/O from interrupt-disabled context")`（`serial_console.c:168`），
   而那行 panic 本身打不出来，于是表现为"最后一行之后什么都没有"。中断上下文只能用
   `dandelion_trace_mark()`。
3. **tick 完全正常**：100 Hz，decrementer 每次正确重装，不是风暴。
4. **console 写入器不可重入**：`dandelion_emmc_putc()` 用共享的 512 字节扇区缓冲、
   遇到换行就 flush，没有锁；tick 一起来，中断上下文和线程上下文同时写就会把
   `dl_emmc_used`/`dl_emmc_index` 弄坏（还可能越界）。已用
   `ml_set_interrupts_enabled()` 包住两个写入器（`zzz5`）。
5. **mockfs 第一次编过**：`kfree_type()` 要的是类型不是指针 typedef（上游 bug）；
   file node 的 `si_devsize` 需要自己填。两个修复都在 `zzz2`。

## 三、还差什么（按顺序）

| # | 事项 | 判据 |
|---|---|---|
| 1 | 验证 `zzz5` 之后 `bsd_init` 线程能被调度回来 | 日志出现 `[dl] timerprobe: busy-wait spans=…`（这是探针最后一段） |
| 2 | `bsd_init` 走完根挂载循环 | `[dl] vfs_mountroot -> …`、`[dl] root mount: fail errno=…`、`[dl] parked N: IOSleep(1s) returned after ~13000000 ticks` |
| 3 | 根文件系统：`rd=md0` + mockfs | `Added memory device md0/rmd0`、`BSD root: md0`、`mockfs_mountroot: root device N bytes`、`[dl] root mount: success` |
| 4 | `load_init_program()` 执行 `/sbin/launchd` | 需要 mockfs 的 file node 指向一个真正的 Mach-O（`docs/ROOTFS-STEP1.md` §2.2） |

第 3 步的固化件部分（ADT 的 `/chosen/memory-map/RAMDisk` 占位、ESP 里的 initrd、
`UniXNU.toml` 的 `rd=md0` + `initrd`）必须**一起刷**：DarwinBoot 对两半是
both-or-neither，只给一半返回 `EFI_UNSUPPORTED`。

## 四、今晚的工具与教训（都已进仓库）

| 工具 | 作用 |
|---|---|
| `tools/round.ps1` | 一轮设备试验：刷 boot+flashinfo / 取日志并解码 / 状态 |
| `tools/ci/check_series.py` | 推送前把 39 条补丁按 CI 顺序打一遍，拦"打不上/脏字节" |
| `tools/ci/syntax_check.sh` | WSL clang 对 `pe_fiq.c` 做语法检查（其余文件需 macOS SDK，工具会标 n/a） |
| `tools/ci/ci_log.py` | 打印编译器原话（含那个 U+FEFF），并能在 Windows 控制台上打印它 |
| `tools/read_log_block.py --out` | 避免 PowerShell 重定向写出 UTF-16 |

**四条别忘**：中断上下文禁止 `printf`；tick 开着时不要动 `ICC_IGRPEN1`；
补丁体不能有 U+FEFF；`waitfetch.py` 的轮询要带 token。
