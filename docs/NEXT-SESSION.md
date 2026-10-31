# 下一次开工从这里开始

写完时间：2026-10-30 03:2x，**更新：2026-10-30 晚（见 §0）**。手机已刷回 Android，仓库已推送。

---

## 0.-6 2026-10-31 11:5x（最新）：**XNU 完整启动完成**，卡在"没有可执行的 init"

**一句话：`bsd_init` 跑完了 —— devfs、siginit、bsd_utaskbootstrap、mountroot_post_hook 全过，
`bsd_init: done`，内核在做完 POST 之后尝试执行 `/sbin/launchd`。这一步失败的唯一原因是
我们给 mockfs 的根设备是 4 KiB 零页，不是可执行的 Mach-O。**

提交：`92253d8`（`zzz11`）。日志：`docs/logs/kernel-log-2026-10-31-bsd-init-done.txt`。

### 关键日志（原文，逐行）

```
DANDELION: no platform expert for secure BSD root; root verification unsupported on this port
bsd_init: calling VFS_ROOT
bsd_init: calling devfs_kernel_mount
bsd_init: calling siginit
bsd_init: calling bsd_utaskbootstrap
bsd_init: calling mountroot_post_hook
bsd_init: done
VM Swap Subsystem is ON
Failed to open swap file 2
vm_swap_create_file failed @ 222 secs
No POST boot-arg set.
bsd_init: bsd_do_post - done
load_init_program: attempting to load /usr/appleinternal/sbin/launchd.development
```

| 阶段 | 状态 |
|---|---|
| `devfs_kernel_mount` | ✅ 靠 `zzz11`：无 kext 时 `g_crypto_funcs` 是 NULL，`SHA256_Init` 一读就炸；改用内核自带的 corecrypto（`ccsha256_di` + `ccdigest_*`） |
| `siginit` / `bsd_utaskbootstrap` / `mountroot_post_hook` | ✅ |
| `bsd_init: done` | ✅ **内核初始化完整走完** |
| VM swap 打开 / 建 swap 文件失败 | ⚠️ 无害（无 swap 设备），`vm_swap_create_file failed @ 222 secs` 只是延迟 |
| `load_init_program` | ❌ 当前卡点：要执行 `/usr/appleinternal/sbin/launchd.development`，而 mockfs 的根设备是 4 KiB 零页 |

### 下一步

1. **给 mockfs 一个真的 arm64 Mach-O**。mockfs 把整个根设备当成一个可执行文件节点，
   所以 `Resources/UniXNU/kernel.macho.rd` 换成合法的静态 Mach-O（`MH_EXECUTE`，AArch64，
   `__TEXT` + `LC_UNIXTHREAD`）就是"用户态跑起来"这一步。
2. 先确认 loader 的每一道门（签名/entitlement/platform binary）在我们的补丁下是否都放开：
   `non-apple-trustedcache.patch`、`non-apple-closed-source.patch` 已经处理了一部分，
   剩下的用 `osfmk/kern/bsd_init.c` 的 `load_init_program()` + `kern_exec.c` 逐条核对。
3. 如果 `/sbin/launchd` 这条路成本高，看 `init=` 之类的 boot-arg 能不能直接指向我们的文件
   （`load_init_program` 先试 `/usr/appleinternal/sbin/launchd.development`，这个路径来自
   boot-arg/配置默认值，值得查清楚）。

---

## 0.-5 2026-10-31 11:3x：**根文件系统挂上了**，`VFS_ROOT` 也过了；现在卡在 `devfs_kernel_mount`

**一句话：这条链第一次有了可用的根文件系统 —— mockfs 从 DarwinBoot 交接的 RAMDisk
挂上 md0，`vfs_mountroot()` 返回 0，`[dl] root mount: success`，然后 `VFS_ROOT` 通过。
新的挡路点是紧接着的 `devfs_kernel_mount()` 数据访问异常。**

提交：`24f233c`（`zzz10`）、`8d3a1dc`（`zzz9`）、`e71706a`（`zzz5` + macos-15 CI 迁移）。
日志：`docs/logs/kernel-log-2026-10-31-vfs-root-devfs.txt`。

### 关键日志（原文）

```
[dl] timerprobe: busy-wait spans=18656319 elapsed=2600000 ticks=2600000 freq=13000000
[dl] IOFindBSDRoot: IOBSD resource absent after 200 ms (polls=1000000)
Added memory device md0/rmd0 (01000000/0D000000) at FFFFFFF007F07000 for 0000000000001000
BSD root: md0, major 1, minor 0
[dl] setRootMedia: no IOBlockStorageDriver class (no IOStorageFamily kext), treating the root as non-storage
bsd_init: vfs_mountroot
mockfs_mountroot: root device 4096 bytes
[dl] vfs_mountroot -> 0 in 89169 ticks (13000000 = 1 s)
[dl] root mount: success
DANDELION: no platform expert for secure BSD root; root verification unsupported on this port
bsd_init: calling VFS_ROOT
bsd_init: calling devfs_kernel_mount
panic(cpu 0 caller ...): Kernel data abort. at pc 0xfffffff007841980, lr 0xfffffff007322d74
   far: 0x0000000000000030   (null pointer + 0x30)
```

| 读数 | 结论 |
|---|---|
| `elapsed=2600000 ticks` = 200 ms | `zzz5`（写入器加中断保护）之后，**tick 运行下线程调度完全正常**，探针跑完 |
| `Added memory device md0/rmd0 … for 0x1000` | ADT 的 `RAMDisk` 占位 + `initrd` 交接成功，4096 字节正是 `kernel.macho.rd` |
| `mockfs_mountroot: root device 4096 bytes` | `zzz2` 的 `si_devsize` 填充生效 |
| `vfs_mountroot -> 0` / `root mount: success` | **根文件系统挂载成功** —— 本移植第一次 |
| `calling VFS_ROOT` 之后还有输出 | `VFS_ROOT` 成功，拿到了 `/` 的 vnode |
| `devfs_kernel_mount` 后 data abort，`far=0x30` | 新挡路点：`kernel_mount()`/`devfs_mount()` 里空指针 + 0x30 |

### 下一步

1. **`devfs_kernel_mount` 的空指针**。`far=0x30` 说明基址为 0 的对象上取偏移 0x30 的字段。
   已用 `tools/macho_sym.py` 把三个地址解析出来：

   | 日志里的值 | 符号 |
   |---|---|
   | `pc 0xfffffff007841980` | `_SHA256_Init+0x10` |
   | `lr 0xfffffff007322d74` | `_vm_kernel_addrhash_internal+0x6c` |
   | `caller 0xfffffff00797d8e0` | `_sleh_synchronous_sp1+0x0`（异常入口） |

   所以不是 devfs 自己炸：是挂载路径里某处调 `vm_kernel_addrhash()` 算 SHA-256，
   而 `SHA256_Init` 拿到的上下文指针是 0（`0x30` 正是 SHA-256 上下文里某个状态字段的偏移）。
   先查 `vm_kernel_addrhash()` 的调用者（`grep -rn vm_kernel_addrhash osfmk bsd`）为什么
   这时候需要哈希，再确认 `SHA256_Init` 在这个构建里是不是桩实现
   （`libkern/crypto/` 下被 `non-apple-crypto-rand.patch` 动过，corecrypto 的 sha2 是否真被链接）。
2. devfs 之后是 `load_init_program()`：它要从根设备执行 `/sbin/launchd`。mockfs 的
   file node 现在指向 4 KiB 零页（挂载够用、执行不够），需要换成真正的 arm64 Mach-O。
3. 之后再往上是用户态；**注意这条路到不了 iOS**（见 `docs/PROGRESS.md` 最后一节）。

---

## 0.-4 2026-10-31 01:3x：tick 稳定 100 Hz 已证实；探针跑完了；剩下的是"第一个 tick 之后线程不再被调度"

**一句话：定时器这条链彻底通了 —— 中断稳定 10 ms 一次、`rtclock_intr()` 每次都完整跑完、
GIC 探针也跑到了最后一行。新的挡路点不再是中断，而是内核在收到真实 tick 之后
不再把 `bsd_init` 线程调度回来。**

提交：`f9c917e`（`patches/xnu/zzz-dandelion-timer-probe.patch` + `zzz3`/`zzz4`）。
日志：`docs/logs/kernel-log-2026-10-31-tick100hz.txt`。

### 决定性读数（原文，逐行可查）

```
[dl] irq 1 intid=30 gap=2444428839 cntp_ctl=5 tval=ffec1873
[dl] rtc 1: enter
[dl] rtc 2: abstime ok
[dl] rtc 3: before timer_intr
[dl] rtc 4: after timer_intr
[dl] gic late: done, timer INTID 30 enabled        <- 探针跑完了
[dl] regs daif=200 ctl=2 tval=6e41d4f8 sre=1 pmr=f0 igrp1=1
[dl] irq 50  intid=30 gap=130013 cntp_ctl=5
[dl] irq 100 intid=30 gap=130012 cntp_ctl=5
[dl] irq 150 intid=30 gap=130013 cntp_ctl=5
```

| 读数 | 结论 |
|---|---|
| `gap=130012`，`CNTFRQ=13000000` | **10.0009 ms**，即稳定 **100 Hz tick**；decrementer 每次都被正确重装 |
| `cntp_ctl=5` | ENABLE=1、ISTATUS=1（到期并在拉线），与 10 ms tick 一致 |
| `rtc 1..4` 每次都有 | `rtclock_intr()` 内部（含 `timer_intr()`）**每次都完整跑完**，不是它挂 |
| 风暴熔断从未触发 | 不是中断风暴（间距从不小于 1000 ticks） |
| `done, timer INTID 30 enabled` / `regs …` | 探针**跑完了**，中断开着的情况下也没有 panic |

### 排除掉的三条（今晚逐一证伪，别再走回头路）

1. **不是 GIC 不转发** —— INTID 30 一次次到达并被 ack。
2. **不是 `rtclock_intr` 崩** —— 四个标记每次都齐。
3. **不是中断风暴** —— 间距稳定 130012。

### 新的挡路点：第一个 tick 之后 `bsd_init` 线程不再被调度

探针最后一行之后（`[dl] regs …`），本该接着是 `timerprobe: before busy-wait`，但没有；
而中断还在以 10 ms 节奏进（`irq 150`）。也就是说 CPU 在跑中断/别的线程，而
`bsd_init` 那条线程没有被调度回来。最可能是第一次真实 tick 触发了抢占，
之后线程没有被放回运行队列（或 `timer_call` 的 deadline 计算与我们的 CNTP 切换不一致）。

**下一步（按顺序）**：
1. 在 `thread_block`/`thread_continue`、`timer_call_*` 与 idle 路径上放环形缓冲标记
   （中断上下文一律用 `dandelion_trace_mark`，绝不能用 printf —— 见下条），看线程去了哪个队列。
2. 对照 `zz-dandelion-cntp-timer.patch`：`ml_set_decrementer` 已改 CNTP，
   但 `mach_absolute_time()` 走 CNTVCT。实测 `CNTVOFF==0`，理论上一致；
   仍需确认 `timer_call` 里 `setPop`/`ml_get_timebase` 用的是同一个计数器。
3. 一旦线程能被正常调度，`bsd_init` 会走到根挂载那半步（`docs/ROOTFS-STEP1.md`）。

### 今晚踩过并记录的坑（都在仓库里有对应改动）

| 坑 | 证据 | 处理 |
|---|---|---|
| 中断上下文里 `printf()` → `panic("Console I/O from interrupt-disabled context")`，且这行 panic 本身打不出来，看起来像"静默死亡" | `serial_console.c:168` | 中断上下文只用 `dandelion_trace_mark`（`zzz3`） |
| 探针里清 `ICC_IGRPEN1` 在 tick 已开的情况下会把内核搞停 | 58c66cc 探针停在 step C1 | 删掉 SGI 实验（`f9c917e`） |
| 补丁体里混入 U+FEFF → `use of undeclared identifier '\ufeff'` | CI run 41052633260 | `tools/ci/check_series.py` 推送前拦截 |
| `kfree_type(mockfs_fsnode_t, …)` 静态断言失败 | CI run 41068310201 | `zzz2` 改为 `kfree_type(struct mockfs_fsnode, …)`，mockfs 首次编过 |
| `waitfetch.py` 轮询未认证 → `KeyError: 'workflow_runs'` | 今晚一次实测 | 带 token |

---

## 0.-3 2026-10-30 23:5x：定时器中断**确实送达**，INTID 30；剩下的故障是 `rtclock_intr()`

**一句话：GIC/定时器那条链已经彻底通了 —— 内核收到了 INTID 30 的定时器中断。
之前三轮"内核哑掉"不是硬件问题，是我自己在中断处理程序里调了 `printf()`，
而这个内核会因此 panic。现在把这条诊断换成内存环形缓冲后，中断进来了，
下一位挡路者是 `rtclock_intr()`。**

提交：`9a52545`（`patches/xnu/zzz3-dandelion-irq-entry.patch`）。
日志：`docs/logs/kernel-log-2026-10-30-irq.txt`。

### 关键日志（原文）

```
[dl] gic step A: ctlr write
[dl] gic late: ctlr rwp-waited 10 -> 12 (spin=0 rwp=0)
[dl] gic step B: cntp
[dl] irq 1 intid=30 hppir1=3ff
[dl] irq1: timer; CNTP masked, rtclock_intr() skipped
[irq1] first irq seen
[dl] gic late: cntp test=0 (1=delivered 2=expired 3=both) ispender0 1000000 -> 1000000 freq=13000000
[dl] gic step C: sgi
[dl] gic step C1: igrpen1 off
[dl] irq 2 intid=30 hppir1=3ff          <- 到这里就停了
```

| 读数 | 结论 |
|---|---|
| `ctlr rwp-waited 10 -> 12` | `GICD_CTLR` bit 1 写进去了（NS 视图下的 Group 1 使能） |
| **`irq 1 intid=30`** | **定时器中断真的到达 CPU 接口并被 ack**：INTID 30 = EL1 非安全物理定时器 PPI，与 Android `/proc/interrupts` 的 `GICv3 30 Level arch_timer` 一致 |
| `irq1: timer; CNTP masked, rtclock_intr() skipped` | 第一个中断只做了 mask+EOI+返回，内核活着走完了实验 B、C 的入口 |
| `irq 2 intid=30` 之后没有输出 | **第二个中断走的是 `rtclock_intr(TRUE)` 那条路，机器停在那里** |

### 前三轮"静默死亡"的真正原因（重要，别再踩）

```
!!! PANIC: Console I/O from interrupt-disabled context @%s:%d
panic(cpu 0 caller ...): ... @serial_console.c:168
```

IRQ 处理程序运行在 `DAIF.I=1` 下，而这个内核**禁止在中断屏蔽上下文做 console I/O**。
我为了诊断在 handler 里加的 `printf()` 正好触发了这个 panic —— 而 panic 前那行
（本该解释一切的 `[dl] irq …`）永远打不出来，于是表现为"最后一行之后什么都没有"。
正确通道是 `dandelion_trace_mark()`（写内存环形缓冲，由 eMMC 日志通道事后落盘）。
**在中断上下文里绝不能用 `printf`/`IOLog`/`kprintf`。**

### 下一步（按顺序）

1. **`rtclock_intr()` 为什么死**。现在第一个中断故意跳过它，第二个中断就走它并停住。
   它内部会处理 `timer_call` 队列、`thread_wakeup`、以及重装 decrementer。
   先在它前后各放一个 `dandelion_trace_mark()`（**不能 printf**）定位到具体子步骤；
   如果它在 `ml_set_decrementer` 之后需要 `ml_delay` 之类会 block 的东西，那就是下一层原因。
2. 拿到 tick 之后：把 `zzz3` 里的 bisect 去掉（第一个中断也走 `rtclock_intr`），
   然后按 `docs/ROOTFS-STEP1.md` 走根文件系统那半步（`options MOCKFS` 已经进了
   `config/MASTER.arm64.dandelion`，`patches/xnu/zzz2-*.patch` 已经让 mockfs 编得过，
   本轮 CI 证明了这一点）。

---

## 0.-2 2026-10-30 22:xx：第三轮内核在探针中途哑掉，加步进标记；同时把根文件系统那半步做掉

### 异常：`4ac085d` 那次启动只打到 SGI 自检就再也没有输出

`round.ps1 collect-log` 拿回来的块里：

| 查询 | 结果 |
|---|---|
| `[dl] gic late: done` / `[dl] regs` / `[dl] timerprobe` / `[dl] hfs wait` / `[dl] parked` | **一条都没有**（全零命中） |
| `[irq1] first irq seen` | **没有**（第二轮有） |
| 块里最后一次真实写入的文本 | `[dl] gic late: sgi self-test …` |
| 日志头 `cntpct` | 2439002560 = **187 秒**，说明写入器在 187 s 时还活着 |

也就是说：`ctlr rwp-waited 10 -> 12` 又出现了（bit 1 依然写进去了），
但紧接在下一条 `printf` 之前内核就再没写过一个字节 —— 而日志头的时间戳还在往后走。
两个可能：(a) 内核在 `dgic_write32(redist, DGICR_ICPENDR0, …)` / 恢复 `ICC_IGRPEN1_EL1`
之后挂了；(b) eMMC 写入器被卡死（中断重入 / 锁），内核还活着但永远打不出字。

这一轮加了**步进标记**（`gic step A` / `B` / `C1` / `C2` / `C3`），每个实验前一行短输出，
所以下一次启动就能把死点从"一段代码"缩小到"一步"。同时把 `[irq1]` 这条一次性标记
当成第二个指标：它出现 = 真的收到过中断。

### 根文件系统：最小改动已经在树里了（`docs/ROOTFS-STEP1.md`）

- `md` 本来就在编（`bsd/conf/files:106` + `config/MASTER:483` 无标签）；
- **唯一**有 `vfc_mountroot` 的文件系统是 mockfs（`vfs/vfs_conf.c:205`，`#if MOCKFS`），
  所以 `config/MASTER.arm64.dandelion` 加了一行无标签 `options MOCKFS`；
- `patches/xnu/zzz2-dandelion-mockfs-size.patch`：mockfs 的 file node 大小来自
  `si_devsize`，而这棵树里只有 `spec_open()` 写它，mountroot 路径从不 open →
  exec 会读到 0 字节。补丁在 `mockfs_mountroot()` 里用 `DKIOCGETBLOCKSIZE/COUNT`
  自己填 `v_specdevsize`（= `v_specinfo->si_devsize`，`bsd/sys/specdev.h:101`）。
- 固化件那一半（ADT 里 `/chosen/memory-map/RAMDisk` 占位、ESP 里的 initrd、
  `UniXNU.toml` 的 `rd=md0` + `initrd`）在 `docs/ROOTFS-STEP1.md` §2.2，
  **必须一起刷**：DarwinBoot 对两半是 both-or-neither，只给一半返回 EFI_UNSUPPORTED。

---

## 0.-1 2026-10-30 21:3x（读这一节就够了）：tick 通了，内核走到"没有根文件系统"

**一句话：`GICD_CTLR` 该写的是 bit 1 不是 bit 0；写进去以后中断第一次被内核收到，
`bsd_init` 越过自己的探针一路跑到根挂载失败。挡路的已经不是 GIC 了，是根文件系统。**

提交：`690cb43`（内核，`patches/xnu/zzz-dandelion-timer-probe.patch`）+ 本文档所在提交。
日志原文：`docs/logs/kernel-log-2026-10-30-tick.txt`（3457 行，`round.ps1 collect-log` 一次拿全）。

### 这一轮的硬证据（每条都能在日志里指到行）

| 读数 | 结论 |
|---|---|
| `[dl] gic late: ctlr rwp-waited 10 -> 12 (spin=0 rwp=0)` | **bit 1（NS 视图下的 EnableGrp1A = EnableGrp1NS 别名）写进去了**。之前的 `10 -> 10` 是写 bit 0 —— ARE_NS==1 时 bit 0 是 RES0，写它必然是 no-op。旧结论"分发器不归非安全态管"是错的 |
| `[irq1] first irq seen` | **这颗内核有史以来第一次收到中断**，就出现在 bit 1 写成功之后 |
| `[dl] gic late: sgi self-test ispender0 1000000 -> 1000200 (bit9=1 spin=0)` | Group 1 NS 的中断能在这颗 redistributor 里挂起 → 帧是对的、组别通 |
| `[dl] gic late: cntp test=0 (1=delivered 2=expired 3=both) ispender0 1000000 -> 1000000` | 见下：`test=0` 不是"定时器没到期"，而是**到期后被内核的 IRQ handler 抢先收走**，探针再去 ack 时已经没有了。同一时刻的 `[irq1]` 就是它 |
| `cntpct=91a3e7e4 cntvct=91a3e7e4` | **CNTVOFF == 0**：虚拟计数器和物理计数器同源，所以 CNTP/CNTV 互换不会把 deadline 算错（`zz-dandelion-cntp-timer.patch` 可以放心留着） |
| `busy-wait spans=23053345 elapsed=2600000 ticks=2600000 freq=13000000` | CNTPCT 忙等精确等于 200 ms（2600000 = 13 MHz × 0.2 s），取代 IOSleep 的方案本身工作正常 |
| `bsd_init: calling setconf` → `IOFindBSDRoot: IOBSD resource absent after 200 ms` → `Still waiting for root device` → `DANDELION: no root filesystem …` | `bsd_init` 现在**越过了自己的探针**，`setconf()` 返回，根挂载循环开始，走到"没有根文件系统"这句 |
| `Waiting on <dict…Apple_HFS…>` 之后还能出现下一行 | 那条 `waitForService(60 s)` 的 deadline **到期返回了** —— 没有 tick 时它会永远挂着。这是 tick 在跑的第二条独立证据 |

日志到"no root filesystem"就结束了：`vfs_mountroot()` 里还有下一个等待（每轮 ~60 s），
我取日志时它还在里面。

### 还有待确认的一件事（下一轮第一件事）

tick 是否**持续**在跑，现在还只有上面两条间接证据。直接证明：在根挂载循环里
`IOSleep(1000)` 并打印 CNTPCT，返回一次就是 tick 在跑、且能唤醒线程。

### 顺带纠正的三条旧结论

1. `docs/GIC-FORENSICS.md` 把 bit 1 定为 H1 并给了规范原文与 Linux/EDK2 的旁证 —— 实测确认。
2. 之前"探针编译不过所以没测过"的真正原因是补丁体里混进一个 **U+FEFF**（clang 报
   `use of undeclared identifier '\ufeff'`）。修在 `tools/ci/ci_log.py`（打印全部编译器
   错误、且能打印这个字符）和新的 `tools/ci/check_series.py`（推送前本地跑整条补丁序列）。
3. 面板上 `DANDELION: gic ctlr 0 -> 0` 是**导出的静态变量初值**，不是真实读数（
   `non-apple-platform-expert-fallback.patch` 只打印变量）。别看它。

---

## 0.0 2026-10-30 19:0x（比 §0 旧）：GIC late init 的读数自相矛盾

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
