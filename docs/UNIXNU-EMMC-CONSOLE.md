# 把 eMMC 日志通道并入 UniXNU-dandelion

目标：**用 eMMC 取代 UART**，解掉 `licg89/UniXNU-dandelion` README 里写的
「no UART to debug / kernels crash immediately with no way to see why」。

本文件是一份集成方案 + 已实现部分的清单。已验证的部分都注明证据。

---

## 1. 为什么是 eMMC，而不是别的

dandelion 没有可用的调试串口，USB 设备模式在 LK 之后被关掉（见 `docs/DEAD-ENDS.md` §7）。
剩下唯一能在「内核崩掉之后」仍留下证据的介质就是 eMMC。

分区选择：`flashinfo`（`mmcblk0p43`，16 MiB，Android 不使用）。

实测数据（`/sys/class/block/mmcblk0p43/start`）：

| 项 | 值 |
|---|---|
| 起始 LBA | 122109919 |
| 大小 | 32768 扇区 = 16 MiB |
| 前 64 KiB | MTK 的 `DOWNLOAD INFORMATION!!` 记录 |
| +1 MiB 起 | 全 0x00（可用，**不是** 0xff） |

---

## 2. 设计（已验证跑通的那一套）

```
从第一条 printf 开始
   ↓  每个字符
RAM ring（32 KiB，环形，丢最老的）
   ↓  VM 可用之后（UniXNU 里就是 unixnu_board_init 的时机）
一次 bring-up：ml_io_map(MSDC0) → 卡状态确认 → GPT 找 flashinfo → 挑空闲区
   ↓
写 header（512 B）+ 回灌 ring + 之后逐行流式写入
   ↓  每个 '\n' 立刻刷一个 512 B 扇区（补零重写，索引不前进）
   ↓  每 8 个扇区刷新一次 header 的 text_bytes
回 Android → adb dd → 主机解码
```

**关键点，全部有真机依据：**

| 设计 | 理由 |
|---|---|
| 逐行刷扇区 | 「内核死前最后一行」必须在介质上；只在扇区满时写会丢最后 ≤511 字节 |
| header 最后写 / magic 最后写 | 没有 magic = 写入没完成，本身就是信号 |
| 写前确认目标区间是全 0x00 或全 0xff | 防止写错 LBA 破坏别的东西 |
| 写完逐字节读回 | 「写成功」必须等于「已验证」 |
| 每次传输前重新武装时钟 | MTK 的 MSDC 空闲时会自关门控 |
| 失败时 CMD12 → 整卡重初始化 | 否则一次瞬时错误会让日志永久静音 |
| 不做多块 CMD25 | 单块 CMD24 没有 CMD12 状态机，控制台本来就是涓流 |

---

## 3. 在 UniXNU 里的落点

UniXNU 的板级钩子已经在 `bsd_init` 里被调用（a04br3 的 `unixnu_board_init`），
这正是「VM 已就绪、可以映射 MMIO」的时机。

### 3.1 XNU 侧补丁

新增 `patches/xnu/native-emmc-console.patch`（命名跟现有 `native-*` 一致），内容：

1. `osfmk/kern/printf.c`：在 `_cnputc` 的镜像点追加一个字符钩子
   （UniXNU 已有 16550 的同类钩子，参照它加一个 `XNU_EMMC_CONSOLE` 分支）
2. 新文件 `osfmk/arm64/emmc_console.c`（本仓库 `bootshim/msdc.c` 的内核版）：
   - `emmc_console_putc()`：ring 缓冲 / 流式写
   - `emmc_console_start()`：bring-up + GPT + 回灌 ring
   - 全部轮询、无中断、无 DMA

### 3.2 Kconfig / defconfig

```make
config XNU_EMMC_CONSOLE
	bool "Polled-PIO eMMC console for boards without a debug UART"
	depends on ARM64
	help
	  Streams the kernel console to a reserved region of an eMMC partition
	  using polled PIO.  The log survives a panic or a watchdog reset, which
	  is what a board without UART needs.  Read it back with
	  scripts/read-emmc-log.py after booting Android.

config XNU_EMMC_CONSOLE_PARTITION
	string "Partition name to write the log into"
	default "flashinfo"

config XNU_EMMC_CONSOLE_OFFSET
	hex "Byte offset inside that partition"
	default 0x200000
```

### 3.3 board.toml

```toml
[emmc-console]
enabled         = true
partition       = "flashinfo"
offset          = 0x200000        # +2 MiB，避开 shim/其它用途
kernel-arg      = "dandelion-emmc-log"
flush-on-newline = true
```

### 3.4 与 NativeMsdc 的冲突（必须处理）

UniXNU 的 `NativeMsdc` 一旦启动就拥有 MSDC0。两个写者会打架。按阶段处理：

| 阶段 | eMMC console | NativeMsdc |
|---|---|---|
| bsd_init 之前 | 只往 RAM ring 里写 | 未启动 |
| `unixnu_board_init` | **独占**：bring-up + 回灌 + 流式 | 未启动 |
| NativeMsdc 启动时 | 停止写，只保留 ring | 接管 |
| NativeMsdc 可用后 | 改为经由它追加（可选，二期） | 拥有 |

一期只需要「独占到 NativeMsdc 启动」，就足以覆盖 bring-up 阶段的所有崩溃。

---

## 4. 日志格式（主机解码器已写好）

`bootshim/log_block.h`：

```
block 0      header, 512 B，显式字节偏移（不是 C struct，见下）
block 1..N   文本槽，每槽 64 KiB
```

header 用字节偏移而不是结构体，因为内核里写到这段内存时如果用了 packed struct，
会产生非对齐的 64 位访问；KASAN/arm64 下不值得冒这个险。

字段：`magic "DLXLOG01"`, `version`, `header_bytes`, `boot`, `slot_count`,
`slot_size`, `next_slot`, `text_bytes`, `cntpct`, `cntfrq`, …
（完整偏移表见 `bootshim/log_block.h` 顶部注释）

---

## 5. 主机侧

```bash
# 回到 Android 后
adb shell su -c "dd if=/dev/block/by-name/flashinfo bs=4096 skip=512 count=128" > dump.bin
python3 scripts/read-emmc-log.py dump.bin        # --scan 忽略 text_bytes，扫到第一个空扇区
```

`skip=512` = 2 MiB（对应默认 offset），`count=128` = 512 KiB（整个日志区）。

建议把这个脚本放进 UniXNU 的 `scripts/`，与 `a04br3-screenshot.py` 并列。

---

## 6. 已经验证 / 还需要做的

### 已验证（真机，非推断）

- 轮询 PIO 写单块：`write rc 0x0 verify rc 0x0` 逐字节读回一致
- GPT 查找 `flashinfo`：`122109919 + 32768 (gpt)`，与 `/sys` 完全一致
- 高位 LBA 读取可信：备份 GPT 头（LBA 122142719）能读出 `EFI PART`
- 日志格式 + 主机解码器：内核日志 `DLXLOG01` 解出 3371 行 / 270 KB
- 内核从第一条 printf 起就被记录（`[step]` 标记、`ulock: buckets ready` 等）
- 写失败后的 CMD12 / 整卡重初始化恢复路径

### 还需要做

| 项 | 说明 |
|---|---|
| 去掉 shim 依赖 | 现在是 shim 通过固定 PA 递 LBA；UniXNU 版必须自己在内核里做 bring-up |
| 换 `ml_io_map` | 现在用 `start.s` 里的 bootstrap Device 映射（VA `0x41030000`）；UniXNU 里应在板级钩子用 `ml_io_map` |
| NativeMsdc 交接 | 见 §3.4 |
| 4 KiB 页 | UniXNU 的 `native-4k-*` 系列已经是 4 KiB，与我们的实现一致 |
| 上游化 | 按 UniXNU 的 PR 习惯拆成 `patches/xnu/` 一个补丁 + 一个 defconfig 选项 |

---

## 7. 验收标准

1. `unixnu build package` 通过，镜像里带 `XNU_EMMC_CONSOLE`
2. 刷入后故意让内核在 `bsd_init` 前 panic（或在 `unixnu_board_init` 里 `panic()`）
3. 进 Android 拉 `flashinfo + 2 MiB`，`read-emmc-log.py --scan` 能打印出 panic 前的完整日志
4. 重复 3 次，`boot` 计数递增，内容不串

达到这四条，`UniXNU-dandelion` 就可以把 README 里的
“blocked by lack of UART” 划掉。
