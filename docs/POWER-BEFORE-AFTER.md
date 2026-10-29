# 供电 / 引脚 / eMMC 握手：从"没反应"到"卡回了"

> 2026-10-29 深夜，第五代。第一代否掉"没电"；第二代拿到 `MSDC_PS`/MT6357 读数；
> 第三代修正测量 bug；第四代把命令波形变成数字；**本代拿到第一个有效响应，并把卡
> 手工走到 TRAN**。
> 代码：`Platforms/Xiaomi/dandelionPkg/Drivers/StorageBeaconDxe/StorageBeaconDxe.c`
> （`ProbePower()` / `ProbeCardAndMux()`）。

---

## 1. 已经测到的

### 1.1 供电：不是问题（IMG_5738 / IMG_5743）

```
PS before=81FF0002 after=81FF0002 DAT=000000FF CMD=1 CD=1
VEMC=00000001 en=1 ANA=00000300 vosel=3 3000mV
```

`MSDC_PS` 的 DAT 在 bits 23..16、CMD 在 bit 24。DAT=0xFF、CMD=1、卡在位；VEMC 3.0 V。

### 1.2 复用 / pad / 复位：都不是问题（IMG_5744）

```
MUX strap=00094A40 mode=2 m0=22222200 m1=11222222
DIN 122..133=00000DFB
```

- `m0=22222200`：122..127 全是 mode 2，跨重启保持。IMG_5743 里 122/123 的 `0xF` 是
  4 位字段里保留的第 4 位，不是模式。
- `DIN=0xDFB`：只有 124（CLK）和 132 低；132 是 `DS`，vendor 的 pad 配置就是
  `dsl:pd50k` 下拉。**RSTB 高、DAT 全高、卡不在复位里。**

### 1.3 关键突破：`CMD0` 之后 `CMD1` 有应答（IMG_5747）

```
C0     I=00000100            cl=98  f=297 l=402  kc=14841
PING a I=00000100 R=00FF8080 cl=129 f=13  l=221  kc=15232
```

- `I=0x100` = **CMDRDY**（bit8），上一版是 `0x200` = CMDTMO。
- `R=00FF8080` = R3 的 OCR：**卡第一次开口**。
- 时间结构印证：`C0` 的帧跨 297..402（105 个样本，98 个低 —— CMD0 的帧几乎全 0）；
  `PING a` 跨 13..221（208 ≈ 2×105）⇒ **命令帧 + 响应帧**。
- `kc≈15k/30k` ⇒ 窗口里时钟一直在跑。

**根因：卡不在 idle 状态，`CMD1` 对它是非法命令。`CMD0 → CMD1` 就是缺的那一步。**

OCR 的 **bit31 = 0** ⇒ 卡还在内部上电流程里；规范要求**重复 `CMD1` 直到 bit31=1**。

## 2. 本版（V6）做什么

把卡手工走到 TRAN，一次把结论钉死：

```
C1  n=..  I=..  R=........      n = CMD1 重试次数（直到 OCR bit31=1）
C2  3=.. 2=.. 1=.. 0=..         128 位 CID（R2，长响应也能收）
C3  I=..  R=........            R6：status + 卡接受的 RCA（用 1，跟 LK 一致）
C7  I=..  R=........            R1b：卡被选中
C13 I=..  R=........ st=..      CURRENT_STATE，4 = TRAN
```

`st=4` 就意味着**硬件、控制器、时序、命令序列全部打通**，剩下的纯粹是驱动的事。

### 屏幕上看哪里

| 位置 | 内容 |
|---|---|
| y 1172 青条 | `P_before`（`MSDC_PS`） |
| y 1186 白条 | 采样版 `CMD1` 的 `PS_AND` |
| y 1200 青条 | 采样版 `CMD1` 的 `PS_OR` |
| x 400..560 / y 1197 | 判读块 |
| 文字区 | **主证据** |

```
MSDC PASSTHRU
PS before=81FF0002 after=81FF0002 DAT=000000FF CMD=1 CD=1
VEMC=.. en=1 ANA=.. vosel=3 3000mV
BUS OK
H0 C=0200F09B E=00000000 I=00000000 R=00000B00
H0 S=FF007FFC P=81FF0002 N=00000001
CMD12 I=00000200 R=00000000
MUX strap=00094A40 mode=2 m0=22222200 m1=11222222
DIN 122..133=00000DFB
C0 I=00000100 cl=.. f=.. l=.. kc=..
PING a I=00000100 R=00FF8080 cl=.. f=.. l=.. kc=..
C1 n=.. I=.. R=........
C2 3=.. 2=.. 1=.. 0=..
C3 I=.. R=..
C7 I=.. R=..
C13 I=.. R=.. st=..
```

## 3. 判读表（V6）

| 画面 | 结论 | 下一步 |
|---|---|---|
| `C13 ... st=4` | **卡在 TRAN**，全部打通 | 改驱动：同一套顺序；然后 EXT_CSD + 块读 |
| `C1 n=..` 后 `CARD NOT READY` | 卡一直 busy（bit31=0） | OCR 窗口 / VEMC 电压；或卡内部初始化慢，加长重试 |
| `C2` 四个字全 0 / `I` 只有 CMDTMO | 长响应（136 位）收不到 | 采样窗口 / `MSDC_PAD_TUNE0` |
| `C3` 的 `R` bit15..0 不是 status 正常值 | RCA 没被接受 | 换 RCA / 检查 CMD3 的 arg |
| `C7` 超时（R1b） | 选中失败或 busy 处理 | 查 `SDC_STS` / DAT0 busy 路径 |
| `C13` 的 `I` 超时 | 卡不在 TRAN | 回看 `C2`/`C3` 哪一步掉的 |

## 4. 这一版**没有**写什么

- 不写 PMIC、不碰总线宽度、不碰 DMA/PIO、不碰 tuning。
- 唯一的写：`CMD0` / `CMD1`(×n) / `CMD2` / `CMD3` / `CMD7` / `CMD13`，都有界；
  以及 GPIO122..133 的 mode 寄存器（写回它本来就该有的 mode 2）。

## 5. 复现

```powershell
powershell -File tools\flash.ps1              # 默认 artifacts\boot-uefi-init.img
# 拍一张整屏（下半部必须完整），先报"我读到的是 X"
```

判读时**先说「我读到的是 X」，等确认后再分析**。
