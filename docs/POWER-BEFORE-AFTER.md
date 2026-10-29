# 供电 / 引脚 / CMD 探针：卡到底卡在哪一侧

> 2026-10-29 深夜，第四代。第一代否掉"没电"；第二代拿到 `MSDC_PS`/MT6357 读数；
> 第三代修正测量 bug 并把"命令有没有真的上线"变成数字；**本版把上一版的结论钉死，
> 并把剩下的唯一问题按规范顺序问一遍（CMD0 → CMD1）**。
> 代码：`Platforms/Xiaomi/dandelionPkg/Drivers/StorageBeaconDxe/StorageBeaconDxe.c`
> （`ProbePower()` / `ProbeCardAndMux()`）。

---

## 1. 已经测到的

### 1.1 供电：不是问题（IMG_5738 / IMG_5743）

```
PS before=81FF0002 after=81FF0002 DAT=000000FF CMD=1 CD=1
VEMC=00000001 en=1 ANA=00000300 vosel=3 3000mV
VMC=00000000 VIO18=00000001
BUS OK
```

`MSDC_PS` 的 DAT 在 **bits 23..16**、CMD 在 **bit 24**（vendor `msdc_reg.h`、上游
`mtk-sd.c` 一致）。`0x81FF0002` ⇒ DAT=0xFF、CMD=1、卡在位；VEMC 使能、3.0 V；
VIO18 使能。**这条分支已经钉死，不再重开。**

### 1.2 复用 / pad：也不是问题（IMG_5744）

```
MUX strap=00094A40 mode=2 m0=22222200 m1=11222222
DIN 122..133=00000DFB
PAD0=00000000 PAD1=00000000
```

- `m0=22222200`：pins 122..127 全是 mode 2。上一版看到的 `m0=2222FF00`（122/123 读回
  `0xF`）**是虚惊**：vendor 的掩码是 `0x777777`，每个引脚 4 位里只有低 3 位是模式，
  第 4 位（bit11/bit15）是保留位，当时恰好是 1。按 vendor 的写法整寄存器写一次，
  读回就是干净的 `0x22222200`，而且**跨重启保持**。
- `DIN 122..133=0xDFB`：12 个脚里只有 **124（CLK）** 和 **132** 是 0。
  132 低是**设计如此** —— vendor 的 pad 配置注释是 `cmd:pu10k, clk:pd50k, dat:pu10k,
  rstb:pu10k, dsl:pd50k`，`DS`（data strobe）本来就是下拉。
  **RSTB 是高的，DAT 全高，卡没有被按在复位里。**
- `PAD0/PAD1=0` **不作数**：MT6765 的 pad 配置不在控制器的 0xE0/0xE4，而在
  `MSDC0_IO_PAD_BASE = 0x10002000`（DRV/PUPD/R0/R1/IES）。控制器的这两个偏移读回 0
  只说明它在这个 IP 上不存在。

### 1.3 命令：确实上线了，卡不回

```
PING a I=00000200 R=00000000 cl=0 dA=00000DFA dO=00000DFF
```

- `cl=0` 是 `MSDC_PS` 的 CMD 位（bit24）**永远读到 1**；但同一时刻 12 个脚的 GPIO 输入
  AND/OR（`dA`/`dO`）显示 **bit0（GPIO122 = CMD）和 bit2（GPIO124 = CLK）在翻转**。
  ⇒ `MSDC_PS` 的 CMD 位在控制器输出时是**被屏蔽的**，不能用来判断"有没有驱动 CMD"；
  pad 的输入缓冲没被屏蔽。**上一版"CMD 可能没动"的判断因此作废。**
- 所以现在的事实是：**卡有电、复位已释放、时钟在跑、CMD 在动，但 R3 回不来（CMDTMO,
  RESP0=0）**。

## 2. 本版（V5）测什么

上一版只发 `CMD1`。**`CMD1` 只在 idle 状态合法**：如果卡被 LK 留在 TRAN，或者被一次
中断的传输卡在某个状态，它对 `CMD1` 就是不理——表现正好是"总线干净 + 超时"。所以本版：

1. **先 `CMD0`（GO_IDLE_STATE，无应答）再 `CMD1`**，按规范顺序来；
2. **把每一次命令的 CMD 波形变成数字**，用 GPIO122 逐样本采样：
   `cl=` 读到 0 的样本数、`f=`/`l=` 第一次/最后一次读到 0 的迭代序号、`kc=` CLK 脚读到 0 的样本数；
3. `kc` 落在 10%..90% 之间 ⇒ 时钟确实在命令窗口里跑；
4. `CMD0` 那一行是"发射机自检"：它一定没有应答，所以它的 `cl/f/l` 就是**纯命令帧**的形状；
5. `CMD1` 的 `f..l` 如果和 `CMD0` 差不多长 ⇒ 只有命令帧，**卡没回**；
   如果 `l` 差不多是 `f` 的两倍 ⇒ **卡其实回了**，是这个控制器采样不到（那就去查
   `MSDC_PAD_TUNE0` / `EMMC50_CFG0`），性质完全不同。

### 屏幕上看哪里

| 位置 | 内容 |
|---|---|
| y 1172 青条 | `P_before`（`MSDC_PS`，进入驱动的第一条读） |
| y 1186 白条 | 最后一次 `CMD1` 的 `PS_AND` |
| y 1200 青条 | 最后一次 `CMD1` 的 `PS_OR` |
| x 400..560 / y 1197 | 判读块（绿=BUS OK、橙=DAT/CMD 不高、红=轨关、洋红=PWRAP 没应答） |
| 文字区 | **主证据**，见下 |

```
MSDC PASSTHRU
PS before=81FF0002 after=81FF0002 DAT=000000FF CMD=1 CD=1
VEMC=00000001 en=1 ANA=00000300 vosel=3 3000mV
VMC=00000000 VIO18=00000001
BUS OK
H0 C=0200F09B E=00000000 I=00000000 R=00000B00
H0 S=FF007FFC P=81FF0002 N=00000001
CMD12 I=00000200 R=00000000
MUX strap=00094A40 mode=2 m0=22222200 m1=11222222
DIN 122..133=00000DFB
PAD0=00000000 PAD1=00000000
C0 I=........ cl=... f=.. l=... kc=...
PING a I=00000200 R=00000000 cl=... f=.. l=... kc=...
PING b I=........ R=... cl=... f=.. l=... kc=...
CARD ANSWERED | NO ANSWER
```

## 3. 判读表（V5）

| 画面 | 结论 | 下一步 |
|---|---|---|
| `PING a` 的 `I` 有 bit8（CMDRDY） | **卡回了** | `R` 是 OCR；bit31=1 表示上电完成 |
| `C0` 的 `cl` 是 0 | 控制器**根本没发**命令（连无应答的 CMD0 都没有帧） | 查 `SDC_CMD` 写回、时钟门、控制器复位位 |
| `C0` 的 `cl` 正常（约 150..250），`PING` 的 `l-f` 也差不多 | 命令帧发出去了，**卡不回** | 电源（VEMC 真到卡上了吗）/ 卡本身 |
| `PING` 的 `l` ≈ 2×(`f` 到 `C0.l`) | **卡回了，主机采样不到** | `MSDC_PAD_TUNE0`(0xF0) / `EMMC50_CFG0` / `MSDC_IOCON` |
| `kc` 接近 0 或接近 `30000` | 命令窗口里时钟没在跑（或恒低） | 时钟门 `MSDC_CFG.CKSTB` / topckgen |
| `kc` 在中间 | 时钟正常 | 排除这一项 |

## 4. 这一版**没有**写什么

- 不写 PMIC（轨已经确认开着；掉电重上留给"卡不回"这条分支的下一轮）。
- 不碰总线宽度、时钟分频、DMA/PIO、tuning。
- 唯一的写：一条 `CMD0` + 两条 `CMD1`（都有界），以及 GPIO122..133 的 mode 寄存器
  （写回它本来就该有的 mode 2）。

## 5. 复现

```powershell
powershell -File tools\flash.ps1              # 默认 artifacts\boot-uefi-cmd0.img
# 拍一张整屏（下半部必须完整），先报"我读到的是 X"
```

判读时**先说「我读到的是 X」，等确认后再分析**。
