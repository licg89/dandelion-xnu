# 读到上游源码后的结论：`MSDC_INTEN` 被清掉，然后又在等它的状态位

> 2026-10-29。依据：IMG_5728 的探针读数 + 上游
> `MediaTekPkg/Drivers/MsdcDxe/MsdcDxe.c（上游 uefi-a04br3 仓库，不在本仓库内）` 原文。

## 1. 探针读数（只读，未写控制器）

```
INT   = 00000000
INTEN = 000BFFFF      <-- 很宽，包含 CMDRDY/CMDTMO/XFER_COMPL/DATTMO/DATCRCERR
R0    = 00000900      <-- R1: CURRENT_STATE = 4 (TRAN)
CFG   = 02000000      <-- 只有一个 bit，存疑，见第 4 节
cell 20 青 + cell 21 绿 = 探针跑完，MMIO 读没有挂住
```

## 2. 上游 `MsdcSendCmd` 里的两处关键代码

```c
  // Disable interrupts because we use polling way
  MsdcClrBits (Private, MSDC_INTEN, MSDC_INT_CMDSTS);

  MsdcWrite (Private, SDC_ARG, CommandBlk->CommandArgument);
  MsdcWrite (Private, SDC_CMD, RawCmd);

  Status = MsdcPollInterrupts (Private, MSDC_INT_CMDSTS, MSDC_INT_CMDRDY);
```

以及 `MsdcPioRead` 同样的开头：

```c
  MsdcClrBits (Private, MSDC_INTEN, MSDC_INT_DATSTS);
  while (TRUE) {
    MsdcRead (Private, MSDC_INT, &IntStatus);   // <-- 在等刚刚被关掉的那组位
```

**驱动先把要等的那组中断从 `INTEN` 里清掉，然后轮询 `MSDC_INT` 等它们置位。**

如果这颗 SoC 的 `MSDC_INT` 是 `raw & INTEN`（被 `INTEN` 门控），那么这个轮询**永远看不到任何东西**
—— 与实测的 `INT=00000000`、以及「每条命令都超时」完全一致。

## 3. 另一个上游缺陷

```c
EFI_STATUS
MsdcPollInterrupts (...)
{
  while (TRUE) {
    MsdcRead (Private, MSDC_INT, &IntStatus);
    if (IntStatus & (ExpectedInterrupts)) break;   // 没有超时
  }
```

上游版本**没有超时**。日志里的 `status=0x12` 来自本仓库补丁加的带超时版本；
也就是说补丁已经把「永远卡住」变成了「2 秒后报超时」，但**根因（INTEN 被清）没动**。

## 4. 待核实的疑点：`CFG = 02000000`

`MsdcInit` 里明确有：

```c
  MsdcSetBits (Private, MSDC_CFG, MSDC_CFG_MODE | MSDC_CFG_CCKPD | MSDC_CFG_PIO);
```

其中 `MSDC_CFG_MODE = BIT0`、`MSDC_CFG_CCKPD = BIT1`、`MSDC_CFG_PIO = BIT3`。
正常初始化后 `CFG` 至少应含 `0x0B`，不可能是 `0x02000000`。

`0x02000000` 恰好等于 `512 << SDC_CMD_BLK_SIZE_SHIFT(16)`，也就是 `SDC_CMD` 里的块长字段。
**所以很可能探针读错了窗口/偏移**（或该 host 根本不是被使用的那一个），
这一点必须复核，不能作为结论使用。

## 5. 建议的下一步（按顺序）

1. **复核探针**：把 `MSDC_CFG` / `MSDC_INTEN` 同时从 **MSDC0(0x11230000) 和 MSDC1(0x11240000)**
   各读一次并分行显示。若 MSDC0 的 `CFG` 不含 `BIT3` 而 MSDC1 含，说明 eMMC 在 MSDC1。
2. **不再清 `INTEN`**：把两处
   `MsdcClrBits (Private, MSDC_INTEN, MSDC_INT_CMDSTS/DATSTS);`
   改为「记录原值 → 轮询 → 恢复原值」，或直接改成 `MsdcSetBits`。
   这是最可能一次性解决 `DATTMO`/超时误判的改动。
3. 若第 2 步后 `INT` 出现 `CMDRDY`，但数据仍不动，才回到数据通路（PIO/DMA/宽度）。
