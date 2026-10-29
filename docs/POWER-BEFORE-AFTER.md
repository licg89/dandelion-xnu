# 供电 / 引脚 / CMD1 探针：卡到底卡在哪一侧

> 2026-10-29 深夜，第三代。第一代（`RESUME-1029` §5）测"上电前/后"，否掉了"没电"；
> 第二代（`boot-uefi-ping.img`）手工发 `CMD1` 并换引脚复用模式，拿到了新线索；
> 本版修正两个测量 bug 并把"命令有没有真的上线"变成一个**数字**。
> 代码：`Platforms/Xiaomi/dandelionPkg/Drivers/StorageBeaconDxe/StorageBeaconDxe.c`
> （`ProbePower()` / `ProbeCardAndMux()`）。

---

## 1. 已经测到的（IMG_5738 / IMG_5743，2026-10-29 晚）

### 1.1 供电：不是问题

```
PS before=81FF0002 after=81FF0002 DAT=000000FF CMD=1 CD=1
PWRAP init=00000001 failed=00000000
VEMC=00000001 en=1 ANA=00000300 vosel=3 3000mV
VMC=00000000 VIO18=00000001
BUS OK
```

- `MSDC_PS` 的 DAT 在 **bits 23..16**、CMD 在 **bit 24**（vendor `ComboA/msdc_reg.h`
  与上游 `mtk-sd.c` 一致；`RESUME-1029` §3 把低 8 位当 DAT 是错的，该节已作废）：
  `0x81FF0002` ⇒ **DAT=0xFF、CMD=1、卡在位**，总线干净且被拉高；
- `MT6357 LDO_VEMC_CON0 = 0x0001`（使能位=1），`VEMC_ANA_CON0 = 0x0300`
  ⇒ VOSEL=3 ⇒ **3.0 V**（VOSEL 在 ANA_CON0，不在 CON0）；
- `VIO18 = 0x0001`：IO 域开着（屏幕亮着，交叉校验通过）。

**结论：卡有电、在位、总线正常。**

### 1.2 命令：还是没人回，而且 CMD 可能根本没动

```
MUX strap=00094A40 mode=2 m0=2222FF00 m1=11222222
DIN 122..133=00000000          <- 测量 bug，见 §3
PING a I=00000200 R=00000000    <- CMDTMO，RESP0=0
PING b I=00000200 R=00000000    <- 换成 mode 1 也一样
```

两条线索：

1. `m0=2222FF00`：pins **124..127 = mode 2**，但 **122/123 读回来是 `0xF`**。
   而 `GpioDxe.c` 的 `GpioSetMode()` 用的是 set/reset 别名寄存器 + **3 位**掩码
   （`0x7 << shift`，而每个引脚是 4 位），所以这两个脚的 4 位字段可能从来没被正确写过。
2. 两条位图条（AND / OR）在 bit24 上看起来一致 ⇒ **CMD 在整条命令期间没有变低过**。
   但当时那段紧循环只有约 100 µs，而命令相位是 48 个时钟（400 kHz 下 ≈ 120 µs），
   采样窗口**可能刚好不够**——所以本版把它换成计数，不再靠数像素。

## 2. 本版（V4）测什么

1. **修 `DIN` 的寻址**：数据寄存器是 `(pin/32)*0x10` + `bit = pin%32`
   （`GpioDxe.c` 的 `PIN_OFFSET`），不是 mode 寄存器那套 `(pin/8)*0x10`。
   上一版的 `DIN=00000000` 就是读错寄存器读出来的。
2. **`cl=`：命令期间 `CMD`（bit24）读到 0 的次数**。这是"控制器到底有没有驱动 CMD"
   的直接答案，一个十进制数字，不用看像素。
   同时打印 `dA=` / `dO=`（命令期间 12 个脚 DIN 的 AND / OR）：某个 bit 在 AND 里是 0、
   在 OR 里是 1，就是**有信号在翻转**（时钟）。
3. **按 vendor 的方式写复用**：整寄存器一次写（`0x22222200` → 0x3F0、`0x222222` → 0x400），
   写完**读回并打印**（`SET mode=N m0=.. m1=..`），然后 ping。两个模式都试。
4. 额外打印 `PAD0=` / `PAD1=`（`MSDC_PAD_CTL0/1`，0xE0/0xE4）：如果 pad 驱动配置是 0，
   CMD 也出不去。

### 屏幕上看哪里

| 位置 | 内容 |
|---|---|
| y 1172 青条 | `P_before`（`MSDC_PS`，进入驱动的第一条读） |
| y 1186 白条 | 最后一次 ping 的 `PS_AND` |
| y 1200 青条 | 最后一次 ping 的 `PS_OR` |
| x 400..560 / y 1197 | 判读块（绿=BUS OK、橙=DAT/CMD 不高、红=轨关、洋红=PWRAP 没应答） |
| 文字区 | 见下 |

文字区（这是主要证据，IMG_5743 证明它拍得很清楚）：

```
PS before=81FF0002 after=81FF0002 DAT=000000FF CMD=1 CD=1
VEMC=00000001 en=1 ANA=00000300 vosel=3 3000mV
MUX strap=00094A40 mode=2 m0=2222FF00 m1=11222222
DIN 122..133=0FFF
PAD0=22222222 PAD1=22222222
PING a I=00000000 R=00000000 cl=NNNN dA=.... dO=....
SET mode=2 m0=.... m1=....
PING b I=... cl=...
SET mode=1 m0=.... m1=....
PING c I=... cl=...
MUX ANSWERED - mode kept | MUX restored to N
```

## 3. 已经踩过的两个测量 bug（这轮修掉）

| bug | 现象 | 原因 |
|---|---|---|
| `PS` 位定义 | 把 `0x02` 读成"DAT0 低 ⇒ 没供电" | DAT 在 bits 23..16、CMD 在 bit 24；低 8 位是 CDEN/CDSTS |
| `DIN` 寻址 | `DIN 122..133=00000000`（可 PS 说这些线是高） | 数据寄存器按 32 脚一组（`(pin/32)*0x10`，bit `pin%32`），只有 mode 寄存器按 8 脚一组 |

第三个（未确认）：`GpioSetMode()` 的 3 位掩码让 pins 122/123 读回 `0xF`。本版改成整寄存器写入
并读回，能直接看到写生效了没有。

## 4. 判读表（V4）

| 画面 | 结论 | 下一步 |
|---|---|---|
| `PING x` 的 `I` 有 bit8（CMDRDY） | **卡回了**，M4 直接推进 | 看 `R`：bit31=1 表示上电完成 |
| `cl=` 一直是 0（两个模式都是） | 控制器**没驱动 CMD** ⇒ 主机侧 | 看 `PAD0/PAD1`；查 msdc0_top（0x11CD0000）是否映射、pad 宏是否被初始化 |
| `cl=` > 0，但 `I` 只有 CMDTMO | 命令确实上了线，卡不回 | 看 `dA/dO` 里有没有翻转的时钟脚；查 RSTB |
| `SET mode=N` 后 `m0` 跟着变 | 复用写是有效的，模式不是问题 | 回上一条 |
| `SET mode=N` 后 `m0` 不变 | 复用寄存器写不进去（地址/别名语义不对） | 查 GPIO 块的 mode 寄存器布局，别再用 GpioDxe 的别名写法 |
| `PAD0/PAD1` 里驱动位是 0 | pad 没有驱动能力 | 补 pad 配置（LK 的 `msdc_set_pad` 值） |

## 5. 这一版**没有**写什么

- 不写 PMIC（轨已经开着，VOSEL=3 → 3.0 V）。
- 不碰总线宽度、时钟分频、DMA/PIO。
- 唯一的写：一条有界的 `CMD1`（每个模式一次），以及 GPIO122..133 的 mode 寄存器
  （结束时恢复成 strap 选的那个模式）。

## 6. 复现

```powershell
powershell -File tools\flash.ps1            # 默认 boot-uefi-ping.img 系列
# 拍一张整屏（下半部必须完整），先报"我读到的是 X"
```

判读时**先说「我读到的是 X」，等确认后再分析** —— 这条约定这一轮又救了一次：
IMG_5738 的 `POWER NO-PULLUP` 就是先被读出来、再发现位定义错了。
