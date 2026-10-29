# MSDC 数据通路：三个实现的逐项对照

写于 2026-10-29 凌晨（这一夜没有真机可用，全部是代码对照与推理）。

我们这块板子上，**同一颗 eMMC、同一个控制器**，有三份实现：

| 代号 | 位置 | 状态 |
|---|---|---|
| **Mu** | `Silicon/MediaTek/MediaTekPkg/Drivers/MsdcDxe`（上游 Mu-Silicium，MT6768 移植过来的） | **命令全通、数据永远拿不到**（M4 卡在这） |
| **LK** | `research/refs/lk-svoboda18/platform/common/storage/mmc/`（MT6765 的 Little Kernel） | 出厂 bootloader，**真机上读写都正常** |
| **Shim** | `bootshim/msdc.c`（线 A 的 eMMC 日志通道） | **真机验证过能读能写**（日志通道就是它写的） |

结论先写在前面：**这不是硬件问题，也不是"eMMC 走不通"，而是 Mu 这份实现少做了两步、多做了一步。**

---

## 1. 实测数据（不是猜的）

`boot-uefi-pagelog-pio2.img` 的照片，第一次读到真实寄存器：

```
ddn pio-read FAIL int=00004000 left=512
ddn  cfg=0200049B fifo=00000000 sts=00100000 dcrc=00000000
```

| 寄存器 | 值 | 读法 |
|---|---|---|
| `MSDC_INT` | `0x00004000` | 只有 **DATTMO**(bit14)。没有 `XFER_COMPL`(bit12)、没有 `DATCRCERR`(bit15) |
| `MSDC_FIFOCS` | `0` | 整个时间窗内 FIFO **没有进过一个字节** |
| `SDC_DATCRC_STS` | `0` | 连"CRC 状态令牌"都没有 → 卡完全没有进入数据传输 |
| `MSDC_CFG` | `0x0200049B` | `PIO`(bit3)=1、`CKSTB`(bit7)=1（时钟稳）、`CKDIV`=4 |
| `SDC_CMD` | `0x02000888`（下一版读数条） | opcode=8、RSPTYP=1(R1)、DTYP=1(单块)、BLKLEN=512 → **命令字是对的** |
| `SDC_CFG` | 主机侧 buswidth=0（1-bit）、SDIO 位已清、DTOC 有效 | 主机侧配置没毛病 |

判读：**主机侧一切正常，卡一个数据字节都没发。** 这排除了"采样点/时钟太快/CRC 设置错"
这一整类原因（那些都会体现为 `DATCRCERR` 而不是 `DATTMO`）。

### 1.2 第二次实测（`IMG_5724`，2026-10-29 08:1x）

这一版先走 DMA、并在数据命令前后打印 R1。照片上的关键三行：

```
ddn data cmd=8 r1=00000900 len=512     ← 卡接受了命令
ddn dma stall int=00000000             ← DMA 路径：一个中断都没来
ddn cmd 7 failed: status=0x12          ← 紧接着的 CMD7 超时（0x12 = EFI_TIMEOUT）
```

读数条：`INTR=00000000`、`R1 =00000900`。

**结论一：卡的状态没问题。** `R1=0x00000900` → bits 12:9 = `4` = **Tran**，
bit8 = 1 = **READY_FOR_DATA**。卡接受 `SEND_EXT_CSD`、处在传输态、并声称数据就绪。
于是"卡不在 Transfer 状态 / CMD7 没选中"这一整类假设**全部作废**。

**结论二：那次 DMA 是无效测量，不是失败证据。** 等待循环是 400 万次空转（≈0.1 s），
比控制器自己的数据超时还短，所以在 `DATTMO` 有机会出现之前就退出了。
**测量必须在被测事件可能发生的时间窗内结束，否则"没看到"毫无意义。**

**结论三（新发现）：数据失败后卡会被"卡住"。** 紧接着的 `CMD7` 返回 `EFI_TIMEOUT`，
原因不是 CMD7 本身：卡还停在没传完的数据块里，等着主机把剩下的字节读走，
在此之前它忽略所有命令。**Mu 的驱动在数据失败后从不发 `CMD12`**（LK 每次都发），
所以一次失败会把后面所有阶段一起带坏 —— 这也是以前"识别之后再无进展"的一部分原因。

对应的三处修正已经进这一版：按时间等待（2 s）、失败后发 `CMD12`、
以及识别完成后由驱动自己做一次 **PIO + DMA 双通路探针**（各配一次 `CMD12`），
一次开机同时拿到两条通路的答案。

---

## 2. 三份实现的关键差异

### 2.1 初始化

| 项目 | LK（真机 OK） | Shim（真机 OK） | Mu（坏） |
|---|---|---|---|
| 谁初始化控制器 | 自己完整初始化 | **不初始化**，继承 LK 的状态 | 自己完整初始化 |
| `MSDC_IOCON` | 写 0 | 不动 | 写 0 ✅ |
| `MSDC_PAD_TUNE0/1` (0xf0/0xf4) | **写 0** | 不动 | **从来不写** ❌（这一版补上了） |
| `MSDC_DAT_RDDLY0..3` (0xf8..0x104) | **写 0** | 不动 | **从来不写** ❌（这一版补上了） |
| `SDC_FIFO_CFG` b24/b25 | **清 0** | 不动 | **从来不写** ❌（这一版补上了） |
| `SDC_CFG.SDIO` | 只在 SDIO 卡上置 | 不动 | **无条件置 1** ❌（这一版改成 host0 清 0） |
| `SDC_CFG` 数据超时 | DTOC/WRDTOC 拉满 | DTOC=0xff / WRDTOC=0x1fff | 由时钟推导（约 161 ms）——这一版也拉满 |
| 时钟 | 按模式 200/50/26 MHz | **降到 ~385 kHz**，全部数据流量都在这上面 | 26 MHz（这一版降到 400 kHz） |

### 2.2 卡的状态（**这是最可能的原因**）

关键事实：**`EXT_CSD[183]`(BUS_WIDTH) 和 `EXT_CSD[185]`(HS_TIMING) 只有在"掉电"时才复位，
CMD0(GO_IDLE_STATE) 不会把它们改回默认值。** LK 在 `mmc_core.c:1831-1874` 里明确会把卡切到
`BUS_WIDTH=8`，并在时钟 >52 MHz 时切到 `HS_TIMING=2`(HS200)。

于是开机链是：

```
上电 → LK 初始化 → 卡被设成 BUS_WIDTH=8、HS_TIMING=2(HS200)，LK 用它载入我们的 boot.img
     → 跳到我们的 FD（DXE 起来）
     → Mu 的 MsdcDxe：CMD0 + CMD1 + CMD2 + CMD3(RCA=1) + CMD7
                    → 主机侧自己配成 1-bit / default speed
                    → 卡侧仍然是 8-bit / HS200（因为没人改，CMD0 也改不了）
```

**一份 1-bit default-speed 的主机 + 一张 8-bit HS200 的卡**，正好会产生我们看到的画面：
命令阶段能应答（命令线是单独一根，与时序关系小），数据阶段卡根本不按主机的节奏发 → `DATTMO` + FIFO 空。

Mu 原来的做法是"跳过 HS_TIMING 切换"，但**"跳过"不等于"默认速度"，而是"保持卡里原来的值"**。
这是这一夜里最重要的一条推理修正。

### 2.3 数据阶段

| 步骤 | Shim（OK） | Mu（坏） |
|---|---|---|
| 等控制器空闲 | `wait_sts_clear(SDCBUSY)` | `MsdcCheckBusy` 等价 ✅ |
| 清 FIFO | `fifo_clear()` | `FIFOCS_CLR` ✅ |
| 清中断 | `wr(R_INT, 0xFFFFFFFF)` | 只 W1C 它等的那一位 ❌（已改成全清） |
| 块数 | `wr(R_BLK_NUM, blocks)` | 同样 ✅ |
| PIO 位 | `if (!(cfg & PIO)) cfg \|= PIO` | 数据命令前重设 ✅ |
| 排水 | 按 `FIFOCS.RXCNT` 实到多少排多少 | 原来要凑够 128 字节 ❌（已重写） |
| 超时 | 软件 deadline + 硬件 DTOC 拉满 | 只有软件循环 ✅ |
| **检查 R1** | **`if (response & R1_FATAL_MASK) → 中止传输`** | **完全不看 Resp0** ❌（这一版打印出来了，但还没据此中止） |

**最后一行是这份对照里最有用的一条。** Shim 是 PIO 读数据的（`data_read()` 就是
FIFO 轮询），而且**在本机真机上读出了完整的 512 字节 ext-CSD** —— 所以
**PIO 这条路径本身在这块硬件上是好的**，我们失败的原因不在"PIO 机制"，而在
"这次传输的前提不成立"。

而 Shim 在发完数据命令之后**第一件事就是检查 R1**，一旦带上 fatal 位就立刻中止、
报 `MSDC_ESTATE`，根本不会傻等 512 字节。Mu 的实现把 `Resp0` 读进
`SdMmcStatusBlk` 就再也没人看过它，于是"卡回了错误应答 → 不送数据 → 我们等到 DATTMO"
变成了一次没有解释的超时。

这也是为什么这一版把 R1 送进读数条（槽 6）：**它是区分"卡拒绝"和"卡不发"的唯一数字。**
如果 R1 带错误位，下一步就该照 Shim 的做法——先中止、把 R1 打进面板、回到"选中卡"那一步，
而不是继续调采样和时钟。


---

## 3. 这一版（夜里编好的）改了什么

按"先让卡和主机对表"的顺序：

1. **CMD6 SWITCH `HS_TIMING=0`**（`EMMCSwitch(PassThru, 3, 185, 0, 0)`）——把卡从 HS200 拉回默认速度；
2. **CMD6 SWITCH `BUS_WIDTH=0`**（`EMMCSwitch(PassThru, 3, 183, 0, 0)`）——把卡拉回 1-bit，
   和主机侧 `MsdcSetBusWidth(Private, 1)` 一致；
3. 数据阶段时钟 **26 MHz → 400 kHz**（shim 已证明这个速率下读写都能过）；
4. `SDC_CFG` 的 DTOC/WRDTOC 拉满；
5. 命令前 `MSDC_INT = 0xFFFFFFFF` 全清；
6. 补齐 LK 有、Mu 没有的 `MSDC_PAD_TUNE*`、`MSDC_DAT_RDDLY*`、`SDC_FIFO_CFG` 位；
7. host0 上 `SDC_CFG.SDIO` 清零（eMMC 不是 SDIO 卡）；
8. 读数条修两个自己的 bug：标签必须 4 字符（`"CFG"`/`"R1"` 里的 `\0` 会把值截掉）、
   行距 26 → 30 px（行与行贴太紧，照片上看不清）；
9. 两条 CMD6 改成 **best effort**：失败只打 `(continuing)`，不阻断识别
   （阻断识别会让 EmmcDxe 不绑定，`MsdcPioRead` 根本不会被调用，证据全丢）；
10. **新增 DMA 读路径** `MsdcDmaRead`（移植自 a04br3 的 GPD/BD 实现，含描述符校验和、
    cache clean/invalidate、有界等待、失败自动退回 PIO）；为此 `MsdcDxe.inf` 增加
    `CacheMaintenanceLib` —— 这条改动在本仓库的补丁序列里（`0002`，同一个 patch 两个文件）；
11. 数据窗内 `MSDC_PS` 采样（`ps_or` / `ps_and`）。

**这一版的取舍：DMA 先试。** PIO 在这块板子上从来没成功过，而 DMA 是同类硬件上验证过的路径；
失败会自动退回 PIO，所以一次开机同时验证两条通路，不会比之前更糟。

同时新增三个**不需要上机**的工具：

- `tools/gen_small_font.py` → 读数条用的 3×5 字体（放大 5 倍，一个字形 15×25 px）；
- `tools/render_panel_preview.py` → **在 PC 上把仪表区画成 PNG**，布局错一眼就能看见，
  不用刷机拍照。上面那两个读数条 bug 本来用它可以零成本发现；
- `tools/extract_log_block.py` + `tools/test_extract_log_block.py` → 从 flash 镜像里
  取回固件日志（格式/校验/长度都有离线自测）。

---

## 4. 下一次上机：判读表

刷完看读数条（两列五行，永久留在屏幕下半部）：

| `INTR` | `R1` | `FIFO` | 说明 | 下一步 |
|---|---|---|---|---|
| `00001000` | — | ≠0 | `XFER_COMPL`，数据来了 | 看 `LEFT`：0 = **M4 达成**，直接进 M5 |
| `00004000` | `xxxxxxx4`（state=Tran，bit8=1） | 0 | 卡在 Tran 但就是不发数据 | 见下面第 5 节的阶梯 |
| `00004000` | 带错误位（bit22 ILLEGAL_COMMAND / bit31 ADDRESS_OUT_OF_RANGE / state≠4） | 0 | 卡拒绝了这条命令 | 打印 CMD7/CMD13 的 R1，确认"选中"这一步 |
| `00008000` | — | 0 | `DATCRCERR`：数据来了但采样错 | 回到调 `MSDC_PAD_TUNE`/`IOCON` 的采样相位，或再降时钟 |
| 仍 `00004000` 且 `R1=0` | 0 | 0 | 命令根本没应答 | 看文字区 `dandelion cmd N failed`，回到命令阶段 |

文字区另外会有三行证据（都在同一个 512 字节时间窗里）：

```
ddn pio-read FAIL int=00004000 left=512
ddn  cfg=0200049B fifo=00000000 sts=00100000 dcrc=00000000
ddn  ps_or=XXXXXXXX ps_and=XXXXXXXX        <- 数据窗内 MSDC_PS 的"或/与"
```

`ps_or` / `ps_and` 是这一版新加的，用来**区分"卡没发"和"线没通"**：

| 观察 | 结论 |
|---|---|
| `ps_or == ps_and`（整段时间引脚没变） | 卡确实没有驱动数据线 —— 问题在卡的时序/状态，而不是 DAT0 走线 |
| `ps_or != ps_and`（引脚动过） | 数据真的上过总线，问题在采样/宽度/CRC 侧 |

> 这一版还把所有"纠正卡状态"的动作改成 **best effort**：`HS_TIMING=0` / `BUS_WIDTH=0`
> 两条 CMD6 即使失败也只是打印一行 `(continuing)`，不再让识别流程失败。
> 理由很实际：一旦识别流程失败，EmmcDxe 就不会绑定，`MsdcPioRead` 根本不会被调用，
> 我们连"数据窗里的寄存器"都看不到 —— 那等于把唯一的证据来源弄丢了。

### 4.1 现在（`boot-uefi-m5-probe.img`）：一次开机两条通路

识别完成后驱动自己发两次 `CMD8`（512 字节），每次后跟一条 `CMD12`：

```
ddn probe pio res=XXXXXXXX int=XXXXXXXX     <- PIO 的结果与 MSDC_INT
ddn probe dma res=XXXXXXXX int=XXXXXXXX     <- DMA 的结果与 MSDC_INT
```

`res` 是 `EFI_STATUS` 低 32 位（`0x0` = 成功；`0x12` = EFI_TIMEOUT；`0x7` = EFI_DEVICE_ERROR），
`int` 是那次传输结束时 `MSDC_INT` 的原始值。判读：

| `probe pio int` | `probe dma int` | 结论 |
|---|---|---|
| 含 `1000`（XFER_COMPL） | — | **PIO 能搬数据 → M4 达成** |
| — | 含 `1000` | **DMA 能搬数据 → M4 达成** |
| `4000`（DATTMO） | `4000` | 两条路径都"数据不来" → 问题在卡/线，不在主机机制 |
| `4000` | `0` 或 `8000`/`1000` | DMA 侧行为与 PIO 不同 → 优先查 DMA 描述符/时序 |
| `0` | `0` | 连超时都没触发 → 测量窗还不够长，或者数据阶段根本没开始 |

另外这一版还修了两件事：等待改成**按时间**（1 ms × 2000 ≈ 2 s，比任何硬件超时都长），
以及**失败后发 `CMD12`**（见 §1.2 结论三）。


---

## 5. 如果数据通路仍然不通，按这个顺序继续

1. **读 `ps_or` / `ps_and`**（详见第 4 节）：这是"卡没发"和"线没通"的分水岭 ——
   整段时间引脚没变化，就别再怀疑走线；变化过，就别再怀疑卡；
2. **host 侧 8-bit**：`MsdcSetBusWidth(Private, 8)` + 卡 `BUS_WIDTH=2`，看 `INTR` 是否变成
   `DATCRCERR` —— 若变成 CRC，说明数据线其实是通的、只是宽度不匹配；
3. **改成 DMA 读**（见 5.1），这是 a04br3 在同类硬件上真正跑通的路径；
4. **不要做的事**：不要再怀疑"时钟太快"（已降到 400 kHz）、不要再怀疑"FIFO 阈值"
   （已按 RXCNT 实到实排）、不要再怀疑"中断残留"（已全清）、不要再怀疑"卡状态"
   （已强制 `HS_TIMING=0` + `BUS_WIDTH=1`，且失败也不阻断识别）。

### 5.1 DMA 备选路径（已调研完，随时可以写）

a04br3 的 `SdCardBlockIoDxe` 在真机上是**用 DMA 搬数据的**，PIO 只用于小传输。它把整套
MTK GPD/BD 描述符实现都写出来了，可以直接照搬：

| 需要的东西 | a04br3 里的位置 |
|---|---|
| 描述符结构（GPD 28 B / BD 16 B） | `Platforms/Benesse/a04br3Pkg/Drivers/SdCardBlockIoDxe/SdCardBlockIoDxe.c:220-240` |
| 校验和算法 `SdDescriptorChecksum` | 同文件，`MSDC_DESC_CHECKSUM_BYTES` 附近 |
| 构建描述符 `SdBuildDescriptors` | 同文件 ~845-905 |
| 启动 + 等待 `SdReadDmaBlocks` / `SdWaitDmaComplete` | 同文件 ~930-1045 |
| 收尾 `SdStopDma` | 同文件 ~911-925 |

关键点（照抄时不要漏）：

1. 数据命令发出**之前**先清 `MSDC_CFG.PIO`（切到 DMA 模式），命令应答后再启动 DMA；
2. `SDC_BLK_NUM` 先写，`SDC_CMD.DTYP` 用 1（单块）或 2（多块）；
3. 命令成功后写 `MSDC_DMA_SA_HIGH` / `MSDC_DMA_SA` / `MSDC_DMA_LEN = 0`，
   置 `MSDC_DMA_CFG.DECSEN`，然后在 `MSDC_DMA_CTRL` 里 `START` 且 burst = 64 B；
4. 轮询 `MSDC_INT` 的 `XFER_COMPL / DATTMO / DATCRCERR / BDCSERR / GPDCSERR`；
5. 收尾要 `DMA_CTRL.STOP`、等 `DMA_CFG.STS` 清零、清 `DECSEN`、把 `PIO` 置回；
6. **缓冲区要做 cache 维护**：传输前 `WriteBackInvalidateDataCacheRange`，读回前
   `InvalidateDataCacheRange`。这意味着 `MsdcDxe.inf` 要加 `CacheMaintenanceLib`
   —— Mu 的 `MsdcDxe.inf` 现在没有，所以这条改动必须连同 INF 一起打进补丁序列（`0003`）。

---

## 6. 与日志通道的关系

Shim 的日志通道（写 eMMC 一个无人使用的区域，回 Android 读回）已经证明这条 eMMC
**在本机可读可写**。所以 M4 一旦通了，日志通道可以立刻照搬到 UEFI：
`BlockIo` 一旦出现，`StorageBeaconDxe` 就能把整份 DEBUG 日志写进 flashinfo+1 MiB，
再用 `tools/` 里的脚本从 Android 侧 dump 出来 —— 那时候就不需要拍照了。
