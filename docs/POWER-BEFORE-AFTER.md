# 供电 / 引脚 / eMMC 握手：从"没反应"到"能读扇区"

> 2026-10-29 深夜，第六代。前四代解决了"卡不回话"，第五代拿到第一声回话，
> **本代把卡走到 TRAN，并读出 EXT_CSD 和 LBA 1** —— 也就是 BlockIo 需要的全部
> 硬件能力。
> 代码：`Platforms/Xiaomi/dandelionPkg/Drivers/StorageBeaconDxe/StorageBeaconDxe.c`
> （`ProbePower()` / `ProbeCardAndMux()` / `ReadDataBlock()`）。

---

## 1. 已经测到的

### 1.1 供电：不是问题（IMG_5738 / IMG_5743）

`MSDC_PS` 的 DAT 在 bits 23..16、CMD 在 bit 24。DAT=0xFF、CMD=1、卡在位；
VEMC 3.0 V（`LDO_VEMC_CON0` en=1，`VEMC_ANA_CON0` vosel=3）。**不要再查供电。**

### 1.2 复用 / pad / 复位：都不是问题（IMG_5744）

`m0=0x22222200`（122..127 全 mode 2，跨重启保持）；`DIN=0xDFB` 只有 CLK 和
`DS`（132，vendor 就是下拉）为低 ⇒ **RSTB 高、DAT 全高**。

### 1.3 关键突破：`CMD0 → CMD1` 有应答（IMG_5747）

```
C0     I=00000100            cl=98  f=297 l=402  kc=14841
PING a I=00000100 R=00FF8080 cl=129 f=13  l=221  kc=15232
```

`I=0x100` = CMDRDY（此前一直是 `0x200` = CMDTMO）；`R=00FF8080` 是 R3 的 OCR。
时间结构：`C0` 帧跨 105 个样本，`PING a` 跨 208 ≈ 2× ⇒ 命令帧 + 响应帧。
**根因：卡不在 idle 状态，`CMD1` 对它是非法命令。**

### 1.4 卡上电完成（IMG_5750）

```
C0     I=00000100 cl=97  f=297 l=402 kc=14767
PING a I=00000100 R=00FF8080 cl=125 f=15 l=222 kc=15187
C1 n=1 I=00000100 R=C0FF8080
```

- `C1 n=1`：第一轮 `CMD1` 的 OCR 就带 **bit31=1**（上电完成）、bit30=1（扇区模式）。
  `n=1` 说明"重复 CMD1 直到 ready"的循环一轮就满足了。
- **`C1` 之后没有文字，不是失败**：文字区只有 68 行（y 0..1087），
  `DandelionConsoleDxe` 自己的自检把光标留在第 53 行，本驱动只剩 15 行预算；
  上一版要吃 21 行，于是 `C2..C13` 全被画到最后一行之外。
  这一版把旧诊断压成 3 行、整段只占 10 行。

## 2. 本版（V8）做什么

IMG_5752 把"命令通"和"数据不通"彻底分开了：

```
C13 I=00000100 R=00000900 st=4          ← 卡在 TRAN，选中
C8  I=00000100 sz=00000000 ...          ← 只有 CMDRDY：没有 XFER_COMPL，也没有 DATTMO
B1  I=00000100 S=00000000
```

两条原因，都在这版修：

1. **总线模式不匹配**。LK 把这颗 eMMC 按 8-bit HS200 拉起来（写了 `EXT_CSD[183]`/
   `[185]`），而这两个字节是**卡的配置，CMD0 不会清**。主机这边是 1-bit default
   （`SDC_CFG=0xFF007FFC`）。命令走 CMD 单根线，任何模式下时序都一样，所以命令
   全对；数据走 DAT，模式不一致就不来 —— 正好是"命令全通、FIFO 空、没有 DATTMO"。
   修法：TRAN 之后用 **CMD6 write-byte** 把 183(BUS_WIDTH)、185(HS_TIMING) 写回 0。
2. **`SDC_CMD` 的数据类型字段在 bit 11，不是 bit 9**。上一版写成了 bit9，结果是
   rsptyp 变成 5、dtype 还是 0 —— 控制器根本没开数据通路，而卡是**看不到主机的
   dtype 字段**的，`CMD8` 对它就是要发 512 字节，于是卡一直拉着 DAT0，后面所有
   命令跟着超时（IMG_5752 顶部 `cmd 7 failed: status=0x12` 就是这个）。

另外补两件卫生：`CMD16` 之后失败要发 **`CMD12 stop`**（LK 每次都发，Mu 驱动从不发，
所以一次失败之后看起来像"卡死了"，见 `docs/ANALYSIS-EMMC-DATA-PATH.md` §1.2）。

### 屏幕上看哪里

| 位置 | 内容 |
|---|---|
| y 1172 青条 | `P_before`（`MSDC_PS`） |
| y 1186 白条 | 采样版 `CMD0` 的 `PS_AND` |
| y 1200 青条 | 采样版 `CMD0` 的 `PS_OR` |
| x 400..560 / y 1197 | 供电判读块 |
| 文字区（主证据） | 见下 |

```
PWR PS=81FF0002 DAT=FF CMD=1 CD=1 VEMC=1 vsel=3 3000mV w=1 f=0 BUS OK
H0 C=0200F09B E=00000000 I=00000000 R=00000B00
H0 S=FF007FFC P=81FF0002 N=00000001
C0 I=.. cl=.. f=.. l=.. kc=..
C1 n=.. I=.. R=........
C2 3=.. 2=.. 1=.. 0=..
C3 I=.. R=.. C7 I=.. R=..
C13 I=.. R=.. st=..
C6 I=../../.. n=.. st=..           ← 新增：写回 1-bit / default speed
C8 I=.. sz=.. rv=.. bw=.. hs=.. c16=....
B1 I=.. S=..
stop I=..                          ← 只在 C8 失败时出现
```

## 3. 判读表（V8）

| 画面 | 结论 | 下一步 |
|---|---|---|
| `C13 st=4` + `C6 st=4` + `C8 I=...0100 sz=<非零>` + `B1 S=45464920` | **整条存储通路打通** | 把这套顺序搬进 `MsdcDxe`/`EmmcDxe` |
| `C8 I=...0100 sz=0` 且 `C6 st=4` | 模式改回来了但数据还不来 | 把主机也切成 8-bit（`SDC_CFG[17:16]=2`）再试 |
| `C8 I=...4000`（DATTMO） | 控制器开了通路，卡没发 | 模式/时序；或 `MSDC_IOCON` 采样边沿 |
| `C8 I=...8000`（DATCRCERR） | 数据在线上、采样窗口不对 | `MSDC_PAD_TUNE0` / `EMMC50_CFG0` |
| `C6 st≠4` | CMD6 没被接受或卡一直 busy | 看 `C6 I=`，可能要求 R1 而不是 R1b |
| `B1 S≠45464920` | 读到了但不是 GPT 头 | LBA 偏移 / 分区表不在 LBA1 |
| 某行 `I=00000200` | 那条命令超时 | 看它上一行走到哪 |


## 4. 这一版**没有**写什么

- 不写 PMIC、不改总线宽度、不改时钟分频、不做 DMA/tuning。
- 会写卡状态的只有规范里的初始化命令：`CMD0`、`CMD1`×n、`CMD2`、`CMD3`、`CMD7`、
  `CMD13`、`CMD16`、`CMD8`、`CMD17`；全部有界超时。
- 唯一的寄存器写：GPIO122..133 的 mode（写回它本来就该有的 mode 2）、
  `MSDC_CFG.PIO`（幂等）。

## 5. 复现

```powershell
powershell -File tools\flash.ps1              # 默认 artifacts\boot-uefi-blockio.img
# 拍一张整屏（下半部必须完整），先报"我读到的是 X"
```

判读时**先说「我读到的是 X」，等确认后再分析**。
