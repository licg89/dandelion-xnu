# 把 eMMC 日志通道并入 UniXNU-dandelion

目标：**用 eMMC 取代 UART**，解掉 `licg89/UniXNU-dandelion` README 里写的
「no UART to debug / kernels crash immediately with no way to see why」。

本文件的第 3 节已按 `mouseos/UniXNU`（未合并的 PR 分支）和
`mouseos/uefi-a04br3` 的**真实源码**核对过，不是设计猜测。

---

## 1. 为什么是 eMMC

dandelion 没有可用的调试串口；USB 设备模式在 LK 之后被关掉（`docs/DEAD-ENDS.md` §7）。
剩下唯一能在「内核崩掉之后」仍留下证据的介质就是 eMMC。

分区：`flashinfo`（`mmcblk0p43`，16 MiB，Android 不使用）。

实测（`/sys/class/block/mmcblk0p43/start`）：

| 项 | 值 |
|---|---|
| 起始 LBA | 122109919 |
| 大小 | 32768 扇区 = 16 MiB |
| 前 64 KiB | MTK 的 `DOWNLOAD INFORMATION!!` 记录 |
| +1 MiB 起 | 全 0x00（可用，**不是** 0xff） |

---

## 2. 设计（已在真机跑通）

```
从第一条 printf 开始
   ↓  每个字符
RAM ring（32 KiB 环形，丢最老的）
   ↓  serial_init() / unixnu_board_init()：VM 已可映射 MMIO
一次 bring-up：ml_io_map(MSDC0) → 卡状态确认 → 定位日志区
   ↓
写 header（512 B）+ 回灌 ring + 之后逐行流式写入
   ↓  每个 '\n' 立刻刷一个 512 B 扇区（补零重写，索引进位只在满时）
   ↓  每 8 个扇区刷新一次 header 的 text_bytes
回 Android → adb dd → 主机解码
```

| 设计 | 理由（每条都有真机依据） |
|---|---|
| 逐行刷扇区 | 「死前最后一行」必须在介质上；只在扇区满时写会丢最后 ≤511 字节 |
| magic 最后写 | 没有 magic = 写入没完成，本身就是信号 |
| 写前确认目标区间全是 0x00 或全 0xff | 防止写错 LBA |
| 写完逐字节读回 | 「写成功」必须等于「已验证」 |
| 每次传输前重新武装时钟 | MTK 的 MSDC 空闲时会自关门控 |
| 失败时 CMD12 → 整卡重初始化 | 否则一次瞬时错误让日志永久静音 |
| 只用单块 CMD24 | 没有 CMD12 状态机；控制台本来就是涓流 |

---

## 3. 在 UniXNU 里的落点（已对照真实源码）

### 3.1 关键发现：UniXNU 有现成的串口 vtable

`pexpert/arm/pe_serial.c`，来自 `mouseos/UniXNU` 的
`patches/xnu/native-ns16550-serial.patch`（尚未上游）：

```c
SECURITY_READ_ONLY_LATE(static struct pe_serial_functions) ns16550_serial_functions = {
	.init            = ns16550_init,
	.transmit_ready  = ns16550_tr0,
	.transmit_data   = ns16550_td0,
	.receive_ready   = ns16550_rr0,
	.receive_data    = ns16550_rd0,
	.device          = SERIAL_NS16550_UART
};
...
	ns16550_base_vaddr = ml_io_map(soc_base + *reg_prop, *(reg_prop + 1));
	if (ns16550_base_vaddr != 0) {
		register_serial_functions(&ns16550_serial_functions);
	}
```

设备由 **ADT 描述**选出：`arm-io` 下的 `uart0`、`compatible` 含 `ns16550a`、
`reg` / `reg-shift` / `reg-io-width`。

**结论：eMMC 日志不是「改 printf」，而是同一个 vtable 的另一个实现。**
`transmit_data(c)` 就是「写一个字符」，我们在里面按行刷扇区。
`kprintf`、`_cnputc` 都不用动。

### 3.2 补丁形状

新增 `patches/xnu/native-emmc-serial.patch`，两个文件：

| 文件 | 改动 |
|---|---|
| `pexpert/pexpert/arm/protos.h` | `serial_device_t` 加 `SERIAL_EMMC = 0x40` |
| `pexpert/arm/pe_serial.c` | `#if XNU_EMMC_SERIAL` 下加整套 vtable 实现 |

契约与 ns16550 一致，只是 **`.receive_*` 恒为 0**（只写不读）：

```c
static void         emmc_init(void);      /* 卡 bring-up + 写 header */
static unsigned int emmc_tr0(void);       /* 恒 1：我们有 512 B 缓冲 */
static void         emmc_td0(uint8_t c);  /* 攒字符，'\n' 时刷一个扇区 */
static unsigned int emmc_rr0(void) { return 0; }
static uint8_t      emmc_rd0(void) { return 0; }
```

`emmc_td0()` 内部就是本仓库已在真机验证过的那套：
攒满 512 B 或遇到 `'\n'` → `dc civac` → 单块 CMD24 → 逐字节读回。

### 3.3 ADT 需要提供什么

由设备仓库的 ADT 生成器（对应 a04br3 的 `make-a04br3-adt.py`）写一个
`arm-io/emmc-log` 节点：

| 属性 | 含义 | 建议值 |
|---|---|---|
| `reg` | MSDC0 物理基址 + 长度 | `0x11230000`, `0x10000` |
| `compatible` | 选择器 | `"dandelion,emmc-log"` |
| `log-lba` | 日志区起始扇区 | `GPT(flashinfo) + 4096`（+2 MiB） |
| `log-sectors` | 保留扇区数 | `1024`（512 KiB） |
| `clock-div` | 固件留下的 CKDIV | `260`（≈385 kHz） |

**关键决定：LBA 由 DT 给，内核不解 GPT。**
设备仓库在打包时就知道 `flashinfo` 的起始 LBA（a04br3 的 `make-a04br3-gpt.py`
也是自己排分区表），没必要让内核再实现 CMD17/CMD18。
内核侧只做「写单块」，代码量减半、风险也减半。

### 3.4 板级钩子（`bsd_init` 之后的持续日志）

用现成的 `patches/xnu/native-board-init-hook.patch`，原文：

```c
#if defined(XNU_BOARD_INIT) && XNU_BOARD_INIT
	/* Board hook: a device repository links an archive (XNU_EXTRA_LD_ARCHIVES)
	 * that defines unixnu_board_init().  It runs once, early in bsd_init, with
	 * IOKit, the scheduler and ml_io_map available, before the root mount. */
	extern void unixnu_board_init(void);
	unixnu_board_init();
#endif
```

设备仓库通过 `XNU_EXTRA_LD_ARCHIVES` 链入一个归档提供 `unixnu_board_init()`。
一期在这个函数里只需把 ring 里剩余字符刷完并保持流式。

### 3.5 与 NativeMsdc 的冲突

`NativeMsdc` 一旦启动就独占 MSDC0：

| 阶段 | eMMC serial | NativeMsdc |
|---|---|---|
| `serial_init()` 起 | 独占，逐行写 | 未启动 |
| `unixnu_board_init()` | 继续写 | 未启动 |
| NativeMsdc 启动时 | 停止写，只保留 ring | 接管 |
| NativeMsdc 可用后 | 可选：改为经它追加 | 拥有 |

一期只需「独占到 NativeMsdc 启动」，就覆盖了 bring-up 阶段的全部崩溃。

---

## 4. 日志格式（主机解码器已写好）

`bootshim/log_block.h`：

```
block 0      header, 512 B，显式字节偏移（不是 C struct）
block 1..N   文本槽，每槽 64 KiB
```

header 用字节偏移而不是结构体：packed struct 会产生非对齐 64 位访问，
在这类早期启动代码里不值得冒险。

字段：`magic "DLXLOG01"`、`version`、`header_bytes`、`boot`、`slot_count`、
`slot_size`、`next_slot`、`text_bytes`、`cntpct`、`cntfrq` …
（完整偏移表见 `bootshim/log_block.h` 顶部注释）

---

## 5. 主机侧

```bash
adb shell su -c "dd if=/dev/block/by-name/flashinfo bs=4096 skip=512 count=128" > dump.bin
python3 scripts/read-emmc-log.py dump.bin        # --scan: 忽略 text_bytes，扫到第一个空扇区
```

`skip=512` = 2 MiB（对应默认 offset），`count=128` = 512 KiB（整个日志区）。

建议放进 UniXNU 的 `scripts/`，与 `a04br3-screenshot.py` 并列。

---

## 6. 已验证 / 还需要做

### 已验证（真机，非推断）

- 轮询 PIO 单块写：`write rc 0x0 verify rc 0x0`，逐字节读回一致
- GPT 查找 `flashinfo`：`122109919 + 32768 (gpt)`，与 `/sys` 完全一致
- 高 LBA 读取可信：备份 GPT 头（LBA 122142719）能读出 `EFI PART`
- 日志格式 + 主机解码器：内核日志 `DLXLOG01` 解出 3371 行 / 270 KB
- 从第一条 printf 起即被记录（`[step]` 标记、`ulock: buckets ready` 等）
- 写失败后的 CMD12 / 整卡重初始化恢复路径

### 还需要做

| 项 | 说明 |
|---|---|
| 宿主化 | 现在是 shim 通过固定 PA 递 LBA + `start.s` 的 bootstrap Device 映射；UniXNU 版改为 ADT + `ml_io_map` |
| 生成补丁 | 对着 `mouseos/UniXNU` 的 `pe_serial.c` 生成合法 diff，需要一次 macOS 编译验证 |
| NativeMsdc 交接 | 见 §3.5 |
| 4 KiB 页 | UniXNU 的 `native-4k-*` 系列已是 4 KiB，与本实现一致 |
| 上游化 | 按 UniXNU 的 PR 习惯：一个 `patches/xnu/` 补丁 + 一个 defconfig 选项 |

---

## 7. 验收标准

1. `unixnu build package` 通过，镜像带 `CONFIG_XNU_EMMC_SERIAL=y`
2. 刷入后让内核在 `bsd_init` 前 panic（或在 `unixnu_board_init` 里 `panic()`）
3. 进 Android 拉 `flashinfo + 2 MiB`，`read-emmc-log.py --scan` 打印出 panic 前完整日志
4. 重复 3 次，`boot` 计数递增，内容不串

达到这四条，`UniXNU-dandelion` 就能把 README 里的
“blocked by lack of UART” 划掉。
