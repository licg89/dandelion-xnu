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

## 三、还差什么（按顺序，参照系见 `docs/LANDSCAPE.md`）

前三项已经做完（tick 下线程调度、`vfs_mountroot` 返回 0、`bsd_init: done`）。
剩下的按"离 iOS 用户态还有多远"排：

| # | 事项 | 参照实现 | 判据 | 量级估计（工程日，非承诺） |
|---|---|---|---|---|
| 1 | 手写最小 arm64 Mach-O，证明 exec 通路（含签名门）通 | 我们自己在做（`tools/mk_init_macho.py`） | `initproc` 起来、用户态写标记 | 1–2 轮刷机 |
| 2 | **eMMC 只读块设备 + 根设备**（替代 4 KiB RAMDisk） | HakiOS `bsd/dev/redwood_emmc.c`，boot-arg `emmc_root=1` | `BSD root: <emmc 分区>` | 3–7 天（内核里已有 eMMC 写通道可复用） |
| 3 | **iOS 16.5 用户态布局**（用户自己从 Apple 公开 IPSW 展开） | HakiOS `sysimage/`、`userland/` | `load_init_program` 找到真 `launchd` | 1–2 周（需用户提供 IPSW） |
| 4 | 显示驱动进 XNU | UniXNU `GopFramebuffer` / HakiOS 显示驱动 | 内核接管屏幕 | 3–7 天（DSI0 数值已实测：0x14014000、720×1600、stride 2944） |
| 5 | 触摸 | **UniXNU `ShengNovatekTouch`**（我们是 Novatek，见下） | 触摸事件进 `/dev` | 2–5 天 |
| 6 | GPU（Mali-G72 MP1，panfrost 系） | HakiOS `redwood_mali.c` + `RedwoodMali.cpp`（G52） | CoreAnimation 出画面 | 2–4 周 |

**触摸控制器已确认为 Novatek**（不是 HakiOS 的 TD4160/OmniVision）：Android 侧
`getevent -pl` 报 `NVTCapacitiveTouchScreen`，dmesg 里是 `[NVT-ts]`；而 UniXNU 组织里
正好有 `ShengNovatekTouch`（NT36532E，SPI），是同族控制器 —— 这块比想象的好办。

**总计**：到"用户态程序跑起来"≈ 第 1 步 + 第 2 步（+第 3 步最小版）≈ 2–3 周量级；
到"iOS 桌面亮起来"≈ 6–10 周量级，瓶颈在第 3 步（IPSW 工具链）和第 6 步（GPU/Metal）。
第 4、5 步可与第 3 步并行。

第 2、3 步是"有没有用户态"的分水岭；4-6 是"能不能看见、能不能操作"。
**修正**：本会话早先写过"到不了 iOS"，那是错的 —— HakiOS 已经在 MT6768 上跑起 iOS 16.5
用户态，内核同为 xnu-8796.x；详见 `docs/LANDSCAPE.md`。

（历史备注）RAMDisk 交接那半步（ADT 的 `/chosen/memory-map/RAMDisk` 占位、ESP 里的 initrd、
`UniXNU.toml` 的 `rd=md0`）必须**一起刷**：DarwinBoot 对两半是 both-or-neither。

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
