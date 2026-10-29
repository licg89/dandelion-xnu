# 供电 / 引脚探针：卡到底是不是"没电"

> 2026-10-29 晚。第一版（`docs/RESUME-1029.md` §5）测的是"上电前/后 DAT 电平"，
> 上机结果（IMG_5738）**否掉了"没电"这个假设**；本版改成"卡能不能听见我们"。
> 代码：`Platforms/Xiaomi/dandelionPkg/Drivers/StorageBeaconDxe/StorageBeaconDxe.c`
> （`ProbePower()` / `ProbeCardAndMux()`）；布局在 `tools/render_panel_preview.py` 里有同样坐标。

---

## 1. 第一轮测到了什么（IMG_5738，2026-10-29 晚）

```
MSDC PASSTHRU
PS before=81FF0002 after=81FF0002
PWRAP init=00000001 failed=00000000
VEMC=00000001 en=1 vosel=0 0mV        <- vosel 那两位是错的，见 §3
VMC=00000000 VIO18=00000001
POWER NO-PULLUP                        <- 这是按错误的位定义得出的，作废
H0 C=0200F09B E=00000000 I=00000000 R=00000B00
H1 C=0200789B E=00000000 I=00000000 R=00000000
H0 S=FF007FFC P=81FF0002 N=00000001
CMD12 I=00000200 R=00000000
```

三条独立读数合起来说明**卡是有电的**：

1. `MT6357 LDO_VEMC_CON0 = 0x0001` → 使能位（bit0）为 1，flash 轨是开的；
2. `VIO18 = 0x0001` → IO 域开着（屏幕亮着，交叉校验通过）；
3. `MSDC_PS = 0x81FF0002` → 按 vendor 的位定义，DAT7..0 = `0xFF`、CMD = 1、卡在位
   （见 §3）—— 总线是干净的、被拉高的空闲总线。

而 `CMD12 I=00000200 R=00000000`、`cmd 7 status=0x12`、ext-CSD 读 `DATTMO` 依旧：
**卡有电、在位、总线正常，却一句都不回。**

## 2. 本版（V3）测什么

既然"供电"已经被排除，剩下的可能是"卡根本没听见我们"：引脚复用选错、时钟没到 pad、
或者卡的 RSTB 被按住。这三件事都无法靠"读寄存器"区分，所以本版直接**问卡**：

1. 手工发一条 `CMD1`（SEND_OP_COND，只要 CMD + CLK，任何状态都能回）；
2. **在命令期间用紧循环采样 `MSDC_PS`**（不 Stall：命令相位只有 48 个时钟 ≈ 120 µs），
   记录 AND / OR。CMD 线（bit24）如果在 AND 里还是 1，说明控制器**从没驱动过 CMD**——
   那是主机侧（pad/时钟）问题，不是卡的问题；
3. 如果没有应答，把 GPIO122..133 的复用改成**另一个模式**（1 ↔ 2）再问一次；
   成功就保留新模式（这样后面的 EmmcDxe 扫描可能直接跑通）；
4. 同时打印 strap、两个 mode 寄存器、以及 12 个脚的 GPIO 输入电平（DIN）。

### 屏幕上看哪里

| 位置 | 内容 |
|---|---|
| y 1172 青条 | `P_before`：进驱动后第一条 `MSDC_PS` |
| y 1186 白条 | `PS_AND`：CMD1 期间**始终为高**的位 |
| y 1200 青条 | `PS_OR`：CMD1 期间**曾经为高**的位 |
| x 400..560 / y 1197 | 判读块（绿=BUS OK、橙=DAT/CMD 不高、红=轨关、洋红=PWRAP 没应答） |
| 文字区 | 见下面 |

关键看 **bit 24（CMD）在两行里的组合**：

| AND bit24 | OR bit24 | 含义 |
|---|---|---|
| 1 | 1 | 控制器**没有驱动** CMD：pad/时钟/复用问题（不是卡） |
| 0 | 1 | CMD 被拉低过：命令确实上了线，卡没回 ⇒ 时钟或复位 |

文字区（都在最后一次清屏之后，能拍就拍）：

```
MSDC PASSTHRU
PS before=81FF0002 after=81FF0002 DAT=FF CMD=1 CD=1
PWRAP init=00000001 failed=00000000
VEMC=00000001 en=1 ANA=XXXX vosel=N NNNNmV
VMC=00000000 VIO18=00000001
BUS OK
MUX strap=00000000 mode=2 m0=2222FF00 m1=00222222
DIN 122..133=0FFF
PING a I=00000200 R=00000000
PING b I=00000000 R=04FF8080
MUX now=1 / MUX ANSWERED - mode kept / MUX restored
```

`DIN 122..133` 里 bit0 = GPIO122 … bit11 = GPIO133。总线被拉高时应该接近 `0FFF`；
如果某一个脚是 0（尤其 131/133 这种候选 RSTB），那就是"被按住"的直接证据。

## 3. MSDC_PS 的真实位定义（这条错了会连累所有结论）

`docs/RESUME-1029.md` §3 曾把 `PS` 的低 8 位当成 DAT，于是 `0x02` 被读成"数据线没被拉高 ⇒ 没供电"。
vendor 头文件（`ComboA/msdc_reg.h`）和上游 `mtk-sd.c` 是一致的，位定义是：

| 位 | 名字 | 方向 |
|---|---|---|
| 0 | `CDEN` 卡检测使能 | RW |
| 1 | `CDSTS` 卡检测状态 | R |
| 15..12 | `CDDEBOUNCE` | RW |
| **23..16** | **`DAT[7:0]`** | R |
| **24** | **`CMD`** | R |
| 31 | `WP` 写保护 | R |

所以 `0x81FF0002` = DAT `0xFF` + CMD `1` + WP `1` + CDSTS `1`：**健康**。

同一段错误还带来两个连带错误，本版一并修掉：

- **`vosel` 不在 `LDO_VEMC_CON0` 里**。mainline `mt6357-regulator.c` 的宏是
  `MT6357_LDO(name, vreg, table, enreg, vosel, vosel_mask)`，`ldo-vemc` 的调用是
  `…, MT6357_LDO_VEMC_CON0, MT6357_VEMC_ANA_CON0, 0x700`：**使能**在 `CON0` bit0，
  **VOSEL** 在 `VEMC_ANA_CON0`（`0x1C20`）bits 10..8。从 `CON0` 里取只能得到 0。
- 判读块里"NO-PULLUP"这个名字本身就带着上一轮的误解，改成了 `BUS DAT-LOW / CMD-LOW`。

## 4. 这些地址凭什么可信

| 项 | 值 | 出处 |
|---|---|---|
| `PWRAP` 基址 | `0x1000D000` | 厂商 DT `pwrap@1000d000`、LK `PWRAP_BASE` |
| `WACS2_CMD / RDATA / VLDCLR / INIT_DONE2` | `0xC20 / 0xC24 / 0xC28 / 0x0A0` | LK `reg_PMIC_WRAP.h`（MT6765），与 `PmicWrapperImplLib.c` 的 `SOC_TYPE == 1` 一致 |
| 事务编码 | `(write << 31) \| ((addr >> 1) << 16) \| data`，FSM = `(RDATA >> 16) & 7`，idle = 0，数据有效 = 6 | `PmicWrapperDxe.c`（上游仓库） |
| `LDO_VEMC_CON0` / `LDO_VMC_CON0` / `LDO_VIO18_CON0` | `0x1988` / `0x1AB0` / `0x1958` | mainline `include/linux/mfd/mt6357/registers.h`、vendor `power_gs_6357` |
| `VEMC_ANA_CON0` | `0x1C20`（VOSEL bits 10..8，2/3/5 → 2.9/3.0/3.3 V） | `mt6357-regulator.c` + 同上的 registers.h |
| `Pinctrl` 基址 / mode 偏移 | `0x10005000` / `0x300`，每 8 脚一个寄存器、每脚 4 位 | `GpioImplLib.c`、vendor `msdc_cust.h`（0x3F0 / 0x400） |
| `CMD1` 的 `SDC_CMD` 编码 | `RSPTYP` 字段 = 3（R3），左移 7；`R1b` = 7、`R1` = 1 | 上游 `MsdcDxe.c` 的 `switch (ResponseType)` |

## 5. 这一版**没有**写什么

- **不写 PMIC**。轨已经是开的，没有理由动它。
- **只有两处写控制器/GPIO**：一条 `CMD1`（和之前那条 `CMD12` 一样是有界的、可忽略的），
  以及在两个候选 mode 之间切一次 GPIO122..133；两次都失败时**恢复成 strap 选的那个**。
- 不碰总线宽度、时钟分频、DMA/PIO。

## 6. 判读表（V3）

| 画面 | 结论 | 下一步 |
|---|---|---|
| `PING a` 有 `CMDRDY`（`I` 的 bit8=1）且 `R` 非零 | 卡本来就是活的，M4 卡在别处 | 看 `R` 的 OCR：bit31=1 表示上电完成；直接查识别流程 |
| `PING a/b` 都是 `I=00000200`、AND/OR 的 bit24 都是 1 | 控制器没驱动 CMD | 查 clock/pad（`MSDC_PAD_CTL*`、msdc0_top 0x11CD0000 是否映射） |
| 同上但 AND bit24 = 0、OR bit24 = 1 | 命令上了线，卡不回 | 查时钟是否真的到卡、RSTB（`DIN` 里找恒为 0 的脚） |
| `PING b` 在另一个 mode 下成功 | **引脚复用模式错了** | 把 `InitGpio()` 改成那个模式 |
| 判读块**红** | 轨被关了（与上一轮相反） | 回到 PMIC 分支 |
| 判读块**洋红** | PWRAP 没应答 | 先修 PWRAP |
| `DIN` 里有一个恒 0 的脚 | 该线被外部按住（候选 RSTB） | 按脚位去查 vendor DT 的 `pins_rst` |

## 7. 复现

```powershell
powershell -File tools\flash.ps1 -Image artifacts\boot-uefi-*.img
# 拍一张整屏（下半部必须完整），先报"我读到的是 X"
```

判读时**先说「我读到的是 X」，等确认后再分析** —— RESUME §8 的约定，这一轮又验证了一次它的价值。
