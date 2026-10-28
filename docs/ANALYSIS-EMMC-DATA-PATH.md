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
| 清中断 | `wr(R_INT, 0xFFFFFFFF)` | 只 W1C 它等的那一位 ❌（已改成全清） |
| 块数 | `wr(R_BLK_NUM, blocks)` | 同样 ✅ |
| PIO 位 | `if (!(cfg & PIO)) cfg \|= PIO` | 数据命令前重设 ✅ |
| 排水 | 按 `FIFOCS.RXCNT` 实到多少排多少 | 原来要凑够 128 字节 ❌（已重写） |
| 超时 | 软件 deadline + 硬件 DTOC 拉满 | 只有软件循环 ✅ |

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
   行距 26 → 30 px（行与行贴太紧，照片上看不清）。

同时新增两个**不需要上机**的工具：

- `tools/gen_small_font.py` → 读数条用的 3×5 字体（放大 5 倍，一个字形 15×25 px）；
- `tools/render_panel_preview.py` → **在 PC 上把仪表区画成 PNG**，布局错一眼就能看见，
  不用刷机拍照。上面那两个读数条 bug 本来用它可以零成本发现。

---

## 4. 下一次上机：判读表

刷完看读数条（两列五行，永久留在屏幕下半部）：

| `INTR` | `R1` | `FIFO` | 说明 | 下一步 |
|---|---|---|---|---|
| `00001000` | — | ≠0 | `XFER_COMPL`，数据来了 | 看 `LEFT`：0 = **M4 达成**，直接进 M5 |
| `00004000` | `xxxxxxx4`（state=Tran，bit8=1） | 0 | 卡在 Tran 但就是不发数据 | 只剩"数据线物理通路"一类：下一步试 host 侧 8-bit（`SDC_CFG` buswidth=2 + 卡切 8-bit）看是否有数据 |
| `00004000` | 带错误位（bit22 ILLEGAL_COMMAND / bit31 ADDRESS_OUT_OF_RANGE / state≠4） | 0 | 卡拒绝了这条命令 | 打印 CMD7/CMD13 的 R1，确认"选中"这一步；必要时用 CMD3 返回的真实 RCA 而不是硬编码 1 |
| `00008000` | — | 0 | `DATCRCERR`：数据来了但采样错 | 回到调 `MSDC_PAD_TUNE`/`IOCON` 的采样相位，或再降时钟 |
| 仍 `00004000` 且 `R1=0` | 0 | 0 | 命令根本没应答 | 看文字区有没有 `dandelion cmd N failed`，回到命令阶段 |

另外文字区会多一行 `ddn data cmd=%u r1=%08x len=%u`，和读数条互相印证。

---

## 5. 如果数据通路仍然不通，按这个顺序继续

1. **host 侧 8-bit**：`MsdcSetBusWidth(Private, 8)` + 卡 `BUS_WIDTH=2`，看 `INTR` 是否变成 `DATCRCERR`
   —— 若变成 CRC，说明"数据线确实通了、只是宽度不匹配"，那就回到 1-bit 继续调；
2. **`MSDC_PS` 判线**：文字区已有 `ddn  ps=%08x`。`MSDC_PS` 是引脚电平现状，
   DAT0 为 bit0、CMD 为 bit8 一带；卡驱动数据时这些位会动。一直不变的只有两种可能：
   pad 复用错、卡确实没发；
3. **改走 DMA**：Mu 的 `MsdcDmaRead` 路径也在树里，`a04br3Pkg` 的
   `SdCardBlockIoDxe` 有完整的 MTK GPD/BD 描述符实现（`Platforms/Benesse/`），
   真要绕开 PIO 有现成参考；
4. **不要做的事**：不要再怀疑"时钟太快"（已降到 400 kHz）、不要再怀疑"FIFO 阈值"
   （已按 RXCNT 实到实排）、不要再怀疑"中断残留"（已全清）。

---

## 6. 与日志通道的关系

Shim 的日志通道（写 eMMC 一个无人使用的区域，回 Android 读回）已经证明这条 eMMC
**在本机可读可写**。所以 M4 一旦通了，日志通道可以立刻照搬到 UEFI：
`BlockIo` 一旦出现，`StorageBeaconDxe` 就能把整份 DEBUG 日志写进 flashinfo+1 MiB，
再用 `tools/` 里的脚本从 Android 侧 dump 出来 —— 那时候就不需要拍照了。
