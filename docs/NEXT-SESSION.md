# 下一次开工从这里开始

写完时间：2026-10-30 03:2x，**更新：2026-10-30 晚（见 §0）**。手机已刷回 Android，仓库已推送。

---

## 0.0 2026-10-30 19:0x（比 §0 更新）：GIC late init 的读数自相矛盾

`[dl] gic late` 跑了，但**它自己的读数互相冲突**：

```
[dl] gic late: gicd typer=7904f4 ctlr=10 (bit0=EnableGrp1NS)
[dl] gic late: redist=fffffffa6d59a000 typer=0 waker=0
[dl] gic late: igroupr0=0 (bit27)
[dl] gic late: isenabler0=8000000 igroupr0=0 icfgr1=800000 hppir1=3ff
[dl] gic late: done, timer INTID 27 enabled
```

| 读数 | 说明 |
|---|---|
| `isenabler0=0x8000000` | bit27 写进去了 → **redistributor 帧是对的**（SGI_base+0x100） |
| `icfgr1=0x800000` | 电平触发，配置正确 |
| `igroupr0=0` | 写 bit27 后仍读 0 → 组别写不进去 |
| `ctlr=0x10` | ARE_NS 已设，**EnableGrp1NS（bit0）为 0** |
| `typer=0` | 在 SGI_base 读 TYPER（应在 RD_base），所以无意义 —— 不是错误，是偏移 |
| `hppir1=0x3ff` | Group 1 上没有 pending |

**两个假设，互斥：**

1. **PPI 27 是 Group 0（安全域）** —— LK 把 GIC 留在 secure 状态，EL1 非安全态改不了 PPI 组别。
   若成立，中断其实在 Group 0 上排队，**正确做法是把定时器当 FIQ 收**，而不是继续修 Group 1。
2. **分发器层面 Group 1 NS 没开**（`ctlr=0x10`）—— 若成立，写一次 `GICD_CTLR |= 1` 就能通，
   这是非安全态**唯一被允许**写的分发器位。

### 判定方法（已提交，`dcca362`）

在 late init 里加了三件事：

- 读 **`ICC_HPPIR0_EL1`** —— 如果它报 **27**，就是假设 1，方向转为 FIQ；
- 读 `ICC_IGRPEN1/IGRPEN0`，把 CPU 接口状态记全；
- **尝试写 `GICD_CTLR |= EnableGrp1NS` 并回读** —— 回读写不进去就说明分发器不归我们管。

块被插在 `hppir1` 打印**之前**，所以那条 `hppir1` 现在反映的是「尝试开 Group1 之后」的状态。

判读表：

| 下一次日志 | 结论 | 下一步 |
|---|---|---|
| `hppir0=1b`（27） | 定时器在 Group 0 | 改成 FIQ 路径（`pe_fiq`），放弃 Group 1 |
| `hppir0=3ff` + `gicd ctlr 10 -> 11` + `hppir1=1b` | Group 1 打开成功，tick 通了 | `bsd_init` 继续 → 进入根文件系统问题 |
| `hppir0=3ff` + `gicd ctlr 10 -> 10` | 分发器不归非安全态管 | 只能走 FIQ / Group 0 路线 |
| 打印停在 `gicd ctlr` 那行 | 写 GICD_CTLR 让总线卡死 | 回退这次写，保留只读部分 |

顺带：`resources/UniXNU/mach_kernel` 在 `ca5d76c` 里已换成**含 GIC late init 的构建**
（14,858,208 字节）——之前 `936477f` 构建失败、`68cdb45` 被取消，这个修复一直没进过内核。

---
## 0. 2026-10-30 晚（最新）：日志通道打通；内核卡在"没有定时器 tick"

**一句话：内核能跑到 `bsd_init` 的根挂载，但定时器中断从未到达过 CPU —— GIC 的重分发器没有使能
PPI 27。这是"所有超时永不到期、`IOSleep` 永不返回"的唯一根因。**

### 这一轮新确认的事实

| 事实 | 证据 |
|---|---|
| 内核日志通道确实在写 | `flashinfo` sector 30000 有 `DLXLOG01`，解出 **3412 行 / 154 KiB**（步骤见 [`LOG-CHANNEL.md`](LOG-CHANNEL.md)） |
| `kprintf()` 在本构建里被整体丢弃 | `debug=0x144` 缺 `DB_KPRT(0x8)` + DEVELOPMENT → `disable_serial_output`/`disable_kprintf_output` 均 true；`printf()` 正常 |
| `bsd_init` 停在 `setconf()` | 面板逐条 `bsd_init: calling X`，最后一条是 `calling setconf`，没有 `vfs_mountroot` |
| 定时器 tick 从未发生 | `[dl] timerprobe: before IOSleep(200)` 之后再无输出 |
| 不是 IRQ 被屏蔽 | `DAIF=0x200`（只有 D 位） |
| 不是 CPU 接口没开 | `ICC_SRE=1`、`PMR=0xf0`、`IGRPEN1=1` |
| 定时器确实到期并在拉线 | `CNTV_CTL=0x5`（ENABLE=1、ISTATUS=1），`CNTV_TVAL` 为负 |
| 但 GIC 里没有 pending | `ICC_HPPIR1=0x3ff`（1023） |
| 结论 | **重分发器没有使能 PPI 27**；交接的固件没做这一步 |

### 已实现的修法（`936477f`，`68cdb45` 修掉重复声明）

`pexpert/arm/pe_fiq.c` 新增 `dandelion_gic_late_init()`：

- `ml_io_map` 映射 GICD `0x0c000000`(+`0x40000`) 与 GICR `0x0c100000`(+`0x200000`)；
- **只读** GICD 并打印 TYPER/CTLR；**绝不写** `GICD_CTLR`/`GICD_IGROUPR`（EL3 拥有，非安全态写会让总线卡死）；
- 唤醒本 PE 的 redistributor；
- PPI 27 → Group 1、优先级 `0xa0`、电平触发、`GICR_ISENABLER0` 使能；
- 回读 `ISENABLER0`/`IGROUPR0`/`ICFGR1`/`ICC_HPPIR1` 并打印；
- 调用点在 `bsd_init` 的探针处（其它子系统都已就绪，卡死也能定位）。

### 换机器后的第一步

1. `git pull`，确认 CI 对 `68cdb45` 构建成功（之前 `936477f` 因为重复 `printf` 声明失败，已修）；
2. 按 [`LOG-CHANNEL.md`](LOG-CHANNEL.md) 的循环刷 `flashinfo` → 启动 → 进 Android 取日志；
3. 看 `[dl] gic late:` 之后的 `hppir1`：
   - `0x1b`（27）→ PPI 使能成功，`IOSleep` 应返回，`bsd_init` 继续；下一个卡点按日志定位；
   - `0x3ff`（1023）→ 写没生效：查 GICR 基址/stride 或 redistributor 是否仍 asleep；
   - 打印停在某条 `gic late:` → 那一步 MMIO 卡死，按该行排查；
4. tick 通了之后：应出现 `bsd_init: calling vfs_mountroot`、`[dl] root mount: fail errno=...`
   （最多 3 次后继续），随后是 `VFS_ROOT` 的 panic——那才是"没有根文件系统"的正题
   （开源 XNU 无 HFS+/APFS，需要决定挂内存盘/网络根，或先让内核停在可用状态）。

### 现场状态（交接清单）

| 项目 | 值 |
|---|---|
| 当前 `boot` 分区 | `artifacts/boot-uefi-redband.img`（我们的链） |
| 原厂 Android boot | `out/boot_backup.img`（取日志时临时刷它，取完刷回） |
| 当前 ESP | `out/unixnu-esp.img`（内核来自最新构建） |
| 旧探针镜像 | 已移到 `out/archive/`（**没删**），`out/` 只剩 `boot_backup.img` 与 `unixnu-esp.img` |
| `Resources/UniXNU/mach_kernel` | 最新构建，随仓库提交（下一台机器不用再等 CI） |
| 取内核脚本 | `tools/ci/waitfetch.py`（`fetch_kernel.ps1` 会挂住，改用这个） |

---

## 0.1  2026-10-30 深夜：`arm_vm_init` 的页表全建好了，最后被自己的 `phystokv()` 拦下

**先读这一节，再读别的。**

`IMG_5805` 读了整整 26 行新日志：`arm_init` 后半段（rtclock / kernel_startup_bootstrap /
timer_call_early_init / cpu_init / processor_bootstrap）全过，`arm_vm_init` 里
`boot_tte`、`bootstrap sanity`、`avail_start`、`invalid_tte`、`cpu_tte`、
`init_ptpages(virt)`、`el0 mappings`、`seg headers`、`page_size`、`pmap_slide`、
`prot init/done`、`low globals done`、`tbi done`、`physmap_init`、`ttb alternate`、
`keep identity map` 一路到最后一关，然后：

```
!!! PANIC: %s: illegal PA: 0x%llx; phys base 0x%llx, size 0x%llx
panic(cpu 0 caller 0xfffffff007991fe4): phystokv: illegal PA: 0x5fc00000;
phys base 0x4d004000, size 0x10000000 @arm_vm_init.c:396
```

凶手还是我们自己的调试代码：`arm_vm_init.c:2218` 的 `DANDELION_TRACE_ATTACH()`
会 `phystokv(0x5FC00000)`，而 trace 环故意放在内存窗口
（`0x4D004000..0x5D004000`）**外面**，`phystokv()` 对窗口外的 PA 直接 panic。
这次根本不需要搬指针：`arm_vm_init()` 保留了 bootstrap 的 V=P 映射
（`DANDELION_KEEP_IDENTITY_MAP`），环和 framebuffer 都还能按物理地址访问——
panic 文本本身就是这么画到面板上的。

本轮修复（`bd2c26b`，`patches/xnu/xnu-trace-attach.patch`）：把
`dandelion_trace_attach()` 改成彻底的空操作，指针留在 `DANDELION_TRACE_PA`，
并在注释里写明「将来若去掉 `DANDELION_KEEP_IDENTITY_MAP`，必须先把环搬进窗口」。

**下一个动作**：拍一张。看 `vm: keep identity map` 之后是否继续出现
`arm_vm_init: kva_active`。若又出现 `illegal PA`，先算该地址是否落在
`gPhysBase..gPhysBase+real_phys_size` 内。

另外记一条将来要改的：`real_phys_size = 0x10000000`，内核对 2/3 GB 内存只认 256 MiB，
以后要扩内存得动 `darwinboot-memory-size`（ADT，属固件侧）。

判读与背景见 [`M6-UNIXNU.md`](M6-UNIXNU.md) §19。

---

## 1. 已经确认的事实（不要再重新验证）

| 事实 | 证据 |
|---|---|
| 整条链走通了：LK → BootShim → UEFI → eMMC BlockIo → FAT → UniXNU → UniXNUDxe → DarwinBoot → 256 MiB 窗口 → ExitBootServices → 跳转 | [logs/boot-log-2026-10-30-handoff.txt](logs/boot-log-2026-10-30-handoff.txt) |
| 内核被放到 `pa 0x4D004000`，窗口 `0x10000000`，`CurrentEL 1`，`PARange 2 (40-bit)`，4 KiB granule | 同上 |
| `UniXNU.toml` 的 `args` 原样进了内核命令行 | 同上 |
| 固件把交接记录写对了，**内核全部前置条件满足** | [logs/boot-log-2026-10-30-emmc-handoff.txt](logs/boot-log-2026-10-30-emmc-handoff.txt) |

交接记录那一行的实际值（这是关键，别再猜）：

```
PayloadBoot: emmc handoff lba 122139919 sectors 512 div 26;
             msdc ver 0x20170426 (want 0x20170426) cfg 0x02210499 ckstb 1
```

`MSDC 版本寄存器 0x114 = 0x20170426` —— 和内核要求**逐位相同**。

## 2. 唯一的未解问题

**`flashinfo` 的 sector 30000 里没有 `DLXLOG01`。**

内核的 `dandelion_emmc_enable()` 在 `arm_vm_init` 里、`pmap_bootstrap` 之后才被调用。
它没写 → **XNU 在更早的地方就停了**（不是通道的问题，前置条件已经全满足）。

## 3. 下一轮的三个动作，按顺序

### 动作 1（最便宜）：读内核自己的面板探针

`patches/xnu/dandelion-early-trace.patch` 会在面板上画 240 px 宽的横条，
**x 从 480 到 720**：

| y | 颜色 | 含义 |
|---|---|---|
| 990 | 白 | 另一处标记 |
| 1000 | 蓝 | before the call |
| 1020 | 绿 | after the call |
| 1040 | 洋红 | mark returned |
| 1060 | 橙 | debug inherit ok |
| 1080 | **紫** | **startup→kprintf ok** |

这些 y 位置在 UniXNU 菜单文字下方、固件指示条（y 1092）上方，**应该是空白的**。

操作：刷 `artifacts/boot-uefi-redband.img`（或下次重建的最新镜像）→ 等横条变绿 →
**拍一张** → 只看 y 990~1080 这一段。

| 看到 | 结论 | 去动作 |
|---|---|---|
| 一条都没有 | 内核在 `arm_init` 之前就死了 | 2 |
| 有 1000/1020、没 1080 | 进了 `arm_init`，`kprintf` 还没活 | 2 |
| 有 1080 紫条 | `kprintf` 活了 | 3 |

### 动作 2：内核在早期就死了怎么办

`dandelion-early-trace.patch` 里 `arm_init.c` 的那个钩子在
`kernel_startup_initialize_upto(STARTUP_SUB_KPRINTF)` 之前。它之前的东西包括
`start.s` 的 bootstrap pmap、`arm_vm_init` 的前半段。要查的对象：

- `osfmk/arm64/start.s` 的 `DANDELION_KEEP_IDENTITY_MAP` 段——它把 MSDC0 的
  2 MiB 块（物理 `0x11200000`，虚地址 `0x41030000`）装进 V=P 表；这个映射错了，
  后面所有早期写入都会踩空；
- `arm_vm_init.c` 里那句 `panic("Unsupported memory configuration %lx", mem_size)`
  的判据——我们的 `mem_size` 是 `0xC0000000`（3 GiB），要确认它落在允许集合里；
- ADT 的 `/chosen` 是否给了它要的东西（`dram-base` / `dram-size` / `memory-map`
  的 `Kernel-Dandelion` 段——这些我们都有）。

### 动作 3：`kprintf` 活了但通道没起怎么办

那就是 `dandelion_emmc_enable()` 里 `printf` 之后的分支。已知能过的检查：
`handoff` 三个字段、`0x114 == 0x20170426`、`CKSTB`。剩下可能失败的是
`dl_emmc_write_header()`（第一次 CMD24 写失败）—— 那时候日志区会是**全零但通道
"应该"起来了**，和现在的现象一样，所以需要用探针区分。

---

## 4. 工具（都用这次学到的教训加固过）

| 工具 | 用法 | 注意 |
|---|---|---|
| `tools/extract_log_block.py` | `--lba 30512` | **必须给 `--lba`**：分区里有两个合法块（30512 是新的、32512 是旧布局残留），它现在会列出来并要求选择 |
| `tools/extract_xnu_log.py` | `python tools/extract_xnu_log.py out/fi.img` | 解内核的 `DLXLOG01` header（version/boot/slots/text_bytes/uptime） |
| `tools/patch_darwinboot.py` | 由 `build_darwinboot.sh` 自动调用 | 让 DarwinBoot 的诊断同时进 ConOut |
| `tools/patch_adt_emmc_log.py` | 幂等 | ADT 的 `emmc-log` 节点 + flashinfo 布局 |
| `tools/patch_emmc_handoff.py` | 幂等 | 交接记录（`PayloadBootDxe`） |

flashinfo 布局（三处必须一致：`bootshim/adt.c`、`tools/mk_unixnu_esp.sh`、`PayloadBootDxe.c`）：

```
0      .. 30000   FAT 卷（UniXNU.efi / EFI/UniXNU/* / kernel.macho 14.8 MB）
30000  .. 30512   XNU 日志通道，256 KiB
30512  .. 30640   固件日志块（DDNLOG01）
```

## 5. 值得先做的一个小加固

**把旧日志块（sector 32512..32640）在第一次使用时清零。** 它现在仍然
checksum 有效，任何一个只看 "找到的第一个/最后一个块" 的工具都会拿错它 ——
今晚因此浪费了三轮刷机，并且得出过一个错误结论（见 M6-UNIXNU.md §13）。

## 6. 三个反复踩过的坑

1. **复位的屏幕不清** → 「停在 UEFI 面板」既可能是旧画面也可能是新固件卡住。
   判据要用**像素**（`PaintBar` 的颜色）、不能用日志（DEBUG 只在之后有 flush 才落盘）。
2. **日志块残留** → 见 §5。
3. **同一个仓库里已有能用的实现时不要再写一份** —— `FindFlashInfo` 就是这么错的，
   而 `FlushLogToFlash` 里那份一直是对的。

## 7. 参考实现（别忘了它）

`Dracen-W/UniXNU-merlinx`（Redmi Note 9，MT6768/MT6769Z）已经跑到
`initproc 和 launchd exec 完成`。它的 [docs/devices/merlinx.md](https://github.com/Dracen-W/UniXNU-merlinx/blob/merlinx-work/docs/devices/merlinx.md) 有完整的 build/启动条件，
对照表在 [MERLINX-COMPARISON.md](MERLINX-COMPARISON.md)。
**注意：它的 `payload` 步骤需要 board trust cache 和 platform identity，上游不分发。**
