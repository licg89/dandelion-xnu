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

## 2. 本版（V10）做什么

IMG_5754 的读数（三种模式全部试过）：

`
C13 I=00000100 R=00000900 st=4
A8  I=00000100 sz=00000000 ps=EF/FF f=00 c6=0000
B8  I=00000100 sz=00000000 ps=EF/FF f=00 c6=0100
C8  I=00000100 sz=00000000 ps=EF/FF f=00 c6=0100
B1  -  no data phase
`

ps=EF/FF 是 MSDC_PS 里 DAT[7:0] 的 AND/OR：**DAT0..7 一直是高的，只有 DAT4 抖过**。
一张真的在发 512 字节的卡不可能只驱动一根线，而命令、CID、选中全都正常 ——
所以数据总线上“没有人在发”。下一步要分清两件事：

1. 那点抖动在哪个**物理脚**上（MSDC_PS 的位序是 DAT[7:0]，但板的引脚编号要靠
   GPIO 那套窗口看）；
2. 主机是不是**根本没在看**（pad 的 IES 输入使能、pinctrl 方向位、GPIO 是否在驱动总线）。

所以这版加两行新的只读诊断：

`
IO dir=000 do=000 ies=0000000F              ← 122..133 方向位 / 输出值 / MSDC0 pad 输入使能
IO ioc=........ f60=........ adv=........   ← MSDC_IOCON / 0x60 / SDC_ADV_CFG0
`

以及每次数据尝试多打一组 **gp=AND/OR**：同一时刻、同一批脚，从 **GPIO 的 DIN**
读一遍（bit0 = 引脚 122）。两个独立窗口对齐后：

| ps（控制器） | gp（GPIO） | 结论 |
|---|---|---|
| FF/FF | FF/FFF | 卡真的没驱动任何数据线 |
| FF/FF | 有变化 | **控制器的输入通路**（pad IES / 复用），卡其实在发 |
| 有变化 | 有变化 | 数据到了控制器，问题在 FIFO / 收尾 |

dir= / do= 能直接抓到“GPIO 把数据脚配成输出、和卡抢总线”，
ies= 能抓到“输入使能被关掉”。

### 屏幕上看哪里

文字区（最多 13 行，第 53..65 行）：

`
PWR PS=81FF0002 DAT=FF CMD=1 CD=1 VEMC=1 vsel=3 3000mV w=1 f=0 BUS OK
H0 C=.. S=.. P=.. N=..
C0 I=.. cl=.. f=.. l=.. kc=..
C1 n=.. I=.. R=........
C2 3=.. 2=.. 1=.. 0=..
C3 I=.. R=.. C7 I=.. R=..
C13 I=.. R=.. st=..
IO dir=000 do=000 ies=........
IO ioc=........ f60=........ adv=........
A8 I=.. sz=.. ps=../.. gp=.../.. f=.. c6=....
B8 ...（A8 失败才出现）
C8 ...（B8 失败才出现）
B1 I=.. S=..
`

## 3. 判读表（V10）

| 画面 | 结论 | 下一步 |
|---|---|---|
| 任一行 I=...1100 + sz 非零 + B1 S=45464920 | **数据通路打通** | 按那一行的模式改驱动 |
| ps=FF/FF 且 gp=FFF/FFF | 卡没驱动任何数据线 | 查卡侧：EXT_CSD 可达性、PARTITION_ACCESS、卡状态 |
| ps=FF/FF 但 gp 有变化 | 控制器看不到卡 ⇒ **pad / 复用** | 看 ies= 与 dir=，必要时重写 pad 配置 |
| dir= 里数据脚为 1 | GPIO 在驱动总线，和卡抢 | 把方向改回输入 |
| ies= 的数据位为 0 | 输入使能没开 | 打开对应 pad 的 IES |
| = 非 0 | 字节到了 FIFO，是 drain / 中断的问题 | 看 XFER_COMPL 位 |
| B1 - | 三种模式都没成 | 按 ps=/gp= 分叉 |
| 某行 I=00000200 | 那条命令超时 | 看它上一行 |
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
