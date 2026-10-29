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

### 1.4 卡上电完成，并且顺序继续往下走（IMG_5750）

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

## 2. 本版（V7）做什么

在 `C1` 之后继续，并且真正**读数据**：

```
C0  I=.. cl=.. f=.. l=.. kc=..      发射机自检（CMD0 无应答）
C1  n=.. I=.. R=........            CMD1 到 OCR bit31
C2  3=.. 2=.. 1=.. 0=..             128 位 CID（R2）
C3  I=.. R=.. C7 I=.. R=..          RCA=1 被接受，卡被选中（R1b）
C13 I=.. R=.. st=..                 CURRENT_STATE，4 = TRAN
C8  I=.. sz=.. rv=.. bw=.. hs=..    EXT_CSD：容量（SEC_COUNT）/ 版本 / 总线 / 时序
B1  I=.. S=..                       LBA 1 前四个字节，45464920 = "EFI "
```

`C8` / `B1` 用的是控制器自己的 **polled PIO**：`SDC_CMD` 里带传输类型和块长，
`MSDC_FIFOCS` 的 RXCNT 告诉还有几个字节，`MSDC_RXDATA` 取字，`MSDC_INT`
的 XFER_COMPL 说传完了。序列与 `bootshim/msdc.c` 的 `CMDW()`/`data_read()`
对同一控制器做的一致。

### 屏幕上看哪里

| 位置 | 内容 |
|---|---|
| y 1172 青条 | `P_before`（`MSDC_PS`） |
| y 1186 白条 | 采样版 `CMD0` 的 `PS_AND` |
| y 1200 青条 | 采样版 `CMD0` 的 `PS_OR` |
| x 400..560 / y 1197 | 供电判读块 |
| 文字区（主证据） | 秒表见下 |

```
PWR PS=81FF0002 DAT=FF CMD=1 CD=1 VEMC=1 vsel=3 3000mV w=1 f=0 BUS OK
H0 C=0200F09B E=00000000 I=00000000 R=00000B00
H0 S=FF007FFC P=81FF0002 N=00000001
C0 I=.. cl=.. f=.. l=.. kc=..
C1 n=.. I=.. R=........
C2 3=.. 2=.. 1=.. 0=..
C3 I=.. R=.. C7 I=.. R=..
C13 I=.. R=.. st=..
C8 I=.. sz=.. rv=.. bw=.. hs=.. c16=....
B1 I=.. S=..
```

## 3. 判读表（V7）

| 画面 | 结论 | 下一步 |
|---|---|---|
| `C13 st=4` + `C8 I=...100` + `B1 S=45464920` | **整条存储通路打通**：命令、长响应、512 字节数据读、扇区寻址全有 | 把这套顺序搬进 `MsdcDxe`/`EmmcDxe`，BlockIo → FAT |
| `C13 st=4` 但 `C8 I=...4000`（DATTMO） | 命令通了，数据相位没起来 | 查 `SDC_CMD` 的 blklen/DTYP、`MSDC_CFG.PIO` |
| `C8 I=...8000`（DATCRCERR） | 数据在线上，采样窗口不对 | `MSDC_PAD_TUNE0` / `EMMC50_CFG0` |
| `C8 sz=00000000` | EXT_CSD 全零 | 读到的不是 EXT_CSD（RSPTYP / 状态） |
| `B1 S≠45464920` | 能读但内容不对 | 扇区模式 / LBA 偏移 / 分区表不在 LBA1 |
| `NOT IN TRAN` | `C13` 的状态不是 4 | 回看 `C2`/`C3`/`C7` 哪一步掉的 |
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
