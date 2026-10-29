# 换机交接

> 这份文件是**唯一入口**。换电脑后先读它，再读文末列出的历史文档。
> 所有内容都已推到 GitHub，`main` 分支即最新。
>
> **2026-10-29 深夜之后先读这一份**（它覆盖下面所有旧结论）：
> [`RESUME-1029.md`](RESUME-1029.md)（当前状态、决定性证据链、下一个动作、别再踩的坑），
> 配合 [`POWER-BEFORE-AFTER.md`](POWER-BEFORE-AFTER.md)（每一轮探针的读数与判读表）。
> 本文件其余小节是更早的环境/流程总览，**已过期的以 RESUME-1029.md 为准**。

---

## 1. 一句话现状

固件已能在 Redmi 9A（dandelion / MT6762G）真机上从 **SEC → PEI → DXE → BDS** 完整跑起来，
有自己的 framebuffer 控制台（文字清晰、不再滚动糊屏）、有面板上的永久数字读数条、
有 64 KiB 的内存日志（可写进 flash 再用脚本取回）、能**找 payload 并 LoadImage/StartImage**、
能**把设备树作为 EFI 配置表发布**。
**唯一还差的是 eMMC 的数据传输**：卡的**命令通路已全部验证**（`CMD0→CMD1(bit31)→CMD2→CMD3→CMD7→CMD13 st=4`，
`R1` 无错误位），但 `CMD8`/`CMD17` 的 512 字节数据相位里**卡不驱动任何 DAT 线**
（`ps=EF/FF`、`gp=DF8/DFF`、FIFO 空、时钟一直在跑）。三种总线模式、CKPDN 清零、控制器软复位均无效。
**当前正在做的事**：用 `CMD13` 读回卡的 `CURRENT_STATE`（`t=`）来区分“卡在发、主机没接住”
和“卡根本没开始”。细节见 [`RESUME-1029.md`](RESUME-1029.md)。

---

## 2. 里程碑

| 里程碑 | 状态 | 证据 |
|---|---|---|
| M1 编出 FD | ✅ | GitHub Actions `build-uefi-dandelion.yml` |
| M2 SEC / PEI / DXE | ✅ | 绿带 / 蓝带 / 屏幕文字 |
| M3 GOP + ConOut 到面板 | ✅ | `GOP FOUND` + `GOP 720 OK` |
| M3.5 UEFI 文字可读 | ✅ | `DandelionConsoleDxe`：2944 stride、不滚动、永久读数条 |
| M4 eMMC → BlockIo → FAT | 🔄 | **命令全通**（`C13 st=4`、`R1` 无错误）；数据相位卡不驱动 DAT，见 `RESUME-1029.md` §1 |
| M5 找 payload 并启动 | 🔄 | `PayloadBootDxe` + FV 内嵌 `PayloadHello`（真 `UniXNU.efi` 还没产出） |
| M6 DarwinBoot → XNU | ⬜ | 缺 `UniXNUDxe` / `DarwinBoot` / `UniXNURamDiskDxe` 源码，见 `M5-M6-PLAN.md` |

---

## 3. 关键实测数据（不要再猜）

| 项 | 值 | 来源 |
|---|---|---|
| FD 加载基址 | **`0x40080000`** | LK 只在这执行 kernel 段；5 组对照实验，见 `docs/CORRECTIONS.md` C8 |
| `PcdSystemMemoryBase` | `0x48000000` | DDR 窗口起点 |
| framebuffer 基址 | **`0x7EC50000`** | `bootshim/fb.h`，每轮开机都在用 |
| framebuffer stride | **`2944` 字节（736 像素）** | 同上；用 2944 写才清晰 |
| 分辨率 / 格式 | 720×1600 / 32bpp BGRA | 同上 |
| GOP 报告的 pitch | `0x02E0` = 736 | 屏幕实测（5 倍放大字） |
| 内核参数 | `raddr=0x51b00000`, `tags=0x47880000`, `page=2048` | 不要改 |

---

## 4. 最近一次上机的确切画面（2026-10-29 深夜，`boot-uefi-state.img`）

**这一节只列当前布局；数值与判读请看 [RESUME-1029.md](RESUME-1029.md) §2 和
[POWER-BEFORE-AFTER.md](POWER-BEFORE-AFTER.md) §3。**

屏幕从上到下：

1. y 0..~100：控制台**回绕到顶部**后打印的内容（PayloadBoot: 与 MsdcDxe 的 `ddn` 日志行）；
2. y ~760：`SELFTEST 0123456789` / `ABCDEFGHIJKLMNOPQRSTUVWXYZ` / `DIRECT CALL 0123456789`
   —— 控制台自检；
3. **y 848..1050：探针文字区（主证据，13 行，固定在第 53..65 行）**：

```
PWR PS=81FF0002 DAT=FF CMD=1 CD=1 VEMC=1 vsel=3 3000mV w=1 f=0 BUS OK
RST C=0200F09B
H0 C=0200F09B S=FF007FFC P=81FF0002 N=00000001
IO dir=000 do=000 din=00000DF7
C0 I=00000100 cl=.. f=.. l=.. kc=..
C1 n=4 I=00000100 R=C0FF8080
C2 3=9B010059 2=32503036 1=34009DB2 0=3BB919C9
C3 I=00000100 R=00000500 C7 I=00000100 R=00000700
C13 I=00000100 R=00000900 st=4
A8 I=0100 R=00000900 sz=00000000 ps=EF/FF gp=DF8/DFF f=00 k=14994 c6=00000000 t=4 r=1
B8 / C8 / D8 ...（前一行失败才出现）
B1 -  no data phase
```

4. y ~1100/1140：两条横幅（`BLOCK NONE` / `FAT NONE` 为红色）—— 没有 BlockIo / FAT；
5. y 1172/1186/1200：三条位图条（P_before / 采样 CMD0 的 AND / OR）+ x 400..560 的供电判读块；
6. y 1400/1448/1496：24 个阶段格子（16..23 属于 MsdcDxe/EmmcDxe，是 M4 的证据）；
7. y 1560：PlatformProgressDxe 的白条。

> 文字区**只有 68 行**且**不滚动**（满后回绕到顶部覆盖最早的行），本驱动的 13 行从第 53 行开始。
> 加诊断行必须先省掉别的行。
## 5. 下一步（按顺序做，都很小）

### 5.0 2026-10-28 晚：三个结构性发现（先看这一节）

这一小时里最值钱的不是某个 bug 的修法，而是三条关于**调度顺序**的事实。

#### (a) `BdsDxe` 在 `APRIORI.inc` 里 = 后面的驱动全部不跑

`BdsEntry` **永不返回**（它就是 boot manager，在等输入）。它原本排在
`APRIORI.inc` 第 36 行；一旦某个真控制台满足了它的依赖，它立刻被调度并接管，
于是排在 FV 主体里的 `PlatformProgressDxe`、`MsdcDxe`、`FatPkg`、
`StorageBeaconDxe` **一个都不会运行**。

现象很好认：屏幕上有我们自己控制台的 `SELFTEST` 行，但**没有** `PlatformProgressDxe`
的彩色诊断。

**处理**：把 `INF ... BdsDxe.inf` 从 `APRIORI.inc` 注释掉。它在 `DXE.inc`
末尾本来就有，那才是它该待的位置 —— 平台需要的所有东西都在它之前调度完。

#### (b) `SimpleTextInOutSerial` 是块"幽灵控制台"

这块板没有 UART，但 `SimpleTextInOutSerial` 会往 ConSplitter 里塞一个 **80×25**
的文本设备（诊断里 `COLS 0050 ROWS 0019` 就是它）。后果：

- BDS 的依赖被它先满足，于是 BDS 抢在真有显示能力的控制台之前跑完，
  把 `Volume Up` / `boot manager` / `failed to find an operating system`
  全打到不存在的串口上；
- ConSplitter 的文本模式被它拉成 80×25，和 720×1600 的面板完全不匹配。

**处理**：`APRIORI.inc` 和 `DXE.inc` 两处都注释掉。

#### (c) 我们的控制台装上了，但 `ConOut` 不往它送

`PlatformProgressDxe` 里那句走 `ConOut` 的探针**没有任何输出**，而同一时刻
用 `DandelionConsoleDxe` 自己的 `OutputString` 直接写的 `SELFTEST` /
`DIRECT CALL` 都**清晰可读**。

结论：渲染没问题，是 **ConSplitter 没把我们的设备加进它的 console 列表**。
当前正在用控制台入口的自检行定位：`COUT <mode> <cols>x<rows>` +
`VIA CONOUT ...`（前者是我们自己画的报告，后者是经 ConOut 回环）。

- 如果 `COUT` 报 `5A x 64`（90×100）且 `VIA CONOUT` 出现 → 已经接上了
- 如果 `COUT X` 或列行数是 80×25 → 还没接上，下一步是直接替换
  `gST->ConOut` 为我们的实例（bring-up 阶段的务实做法，之后再回头修 ConSplitter）

### 5.4 2026-10-29 凌晨：横纹的真因 + MSDC 标记为什么一直是空的

这两条都是**读代码读出来的**，不靠上机，价值比一次刷机大。

#### (a) MSDC 的所有标记都打在 ConOut 还不存在的时候

`MsdcImplLib.c` 的 `MsdcSay()` 第一件事就是

```c
if ((gST == NULL) || (gST->ConOut == NULL) || (Tag == NULL)) {
  return;      // ← 连状态带都不画
}
```

而 `MsdcDxe` 当时排在 `APRIORI.inc` 的**第 29 行**，`DandelionConsoleDxe` 在**第 91 行**。
所以构造函数的每一条标记都在 `gST->ConOut == NULL` 时返回，屏幕上只剩控制台自己画的
洋红。**那一整个刷机周期对 eMMC 一无所知，不是我猜错了阶段，是标记根本没画。**

处理（两层）：

1. `MsdcSay()` 改成**先写 framebuffer，再看有没有 ConOut**；
2. `MsdcDxe` 挪到 `DandelionConsoleDxe` **之后**（`APRIORI.inc`），这样文字标记也能落到
   一个活的控制台上。

并且把"最后一次覆盖的状态带"升级成**状态格子**：面板底部 y 1400..1480 有 8 个 80×80
格子，间距 88，第 i 个格子代表第 i 个阶段。**格子存在 = 阶段到达**，所以一张照片就是
一个完整状态向量，没有"最后写入者覆盖"的歧义：

| 格子 | 颜色 | 含义 |
|---|---|---|
| 0 | 黄 | `MsdcDxe` 构造函数跑到了 |
| 1 | 橙（红=失败） | GPIO122..133 复用设置完成 |
| 2 | 青（红=失败） | MSDC 时钟使能完成 |
| 3 | 白（红=缺协议） | power / PMIC 路径走完 |
| 4 | 紫 | 产生了 BlockIo（`StorageBeaconDxe`） |
| 5 | 绿 | 挂上了文件系统（`StorageBeaconDxe`） |

#### (b) 那片"白横纹"是我们自己的 `ClearScreen` 没清干净

`DandelionConsoleDxe.c` 的 `FillRow()` 原本是：

```c
Dst = base + Row * 16 * STRIDE;      // 只算出这一格的**第一行**
for (X = 0; X < WIDTH; X++) Dst[X] = Color;   // 也只填这一行
```

`ClearScreen()` 用 `for (Row = 0; Row < 100; Row++) FillRow (Row, bg)` 调用它，于是只清了
**扫描行 0, 16, 32, …, 1584**，每 16 行里剩下 15 行是上一阶段留下的旧内容。

这就解释了所有一直没解释通的现象：

- `PlatformProgressDxe` 在 y=24 写的 `DANDELION UEFI` **擦不掉**（24 不是 16 的倍数）；
- 中段那片横纹既不是 GOP、也不是 `GraphicsConsoleDxe`、也不是第二条控制台，
  而是 **PEI/上一帧留在 DRAM 里的老内容**，从来没有任何一次 clear 碰到过它；
- 我们把 `GraphicsConsoleDxe` 从 FV 里摘掉之后横纹"还在"，正是因为凶手不是它。

已修：`FillRow()` 现在填满 16 个扫描行（含隐藏的 16 像素补白）。
`ScrollUp()` 也一并正确了。

> 连带影响：clear 真的生效以后，`PlatformProgressDxe` 的头部诊断会被擦掉，
> 所以控制台入口现在自己写前两行 `DANDELION UEFI` / `CONSOLE OK 90x100`。

### 5.5 下一步动作（照这个顺序）

1. 用当前改动重新 build → package → `flash boot`；
2. 一张照片读底部 8 个格子：
   - 只到 0 → `MsdcDxe` 的 depex 没满足，或 `Start()` 提前返回
   - 到 1 红 → 没拿到 GPIO 协议
   - 到 2 红 → 没拿到 Clock 协议
   - 到 3 白 → 主机侧初始化全过，卡不应答（下一步查 CMD0/CMD1 与 pad tuning）
   - 到 4 紫 → **M4 达成**，接着看 5
3. 只有到 3 时，才去动上游 `MsdcDxe.c`（用 `Resources/MuPatches` 加 `MsdcTrace` 钩子），
   在 CMD0/CMD1/ACMD41 上打点。

### 5.6 2026-10-29 02:2x：先修好"能不能读到日志"，再修数据通路

这一轮把两件事分开做：**诊断通道**和**被诊断的东西**。

**(a) 诊断通道（这轮改的就是它）**

1. `ScrollUp()` 用 framebuffer 当 memmove 的源 → 这块屏读回不可靠 → 每滚一次就多一层
   错位重影，照片上半屏全是乱的（详见 `CORRECTIONS.md` C11）。现在**不滚动**：
   写满 68 行就 `WrapToTop()` 覆盖最旧一行，行首第一次写入前整行擦除。
2. 面板上只剩"最近 68 行"，所以关键诊断信息必须**短**：
   `ddn pio-read FAIL int=%08x left=%u` + `ddn  cfg=... fifo=... sts=... dcrc=...`
   两行，成功时只打前两次 `ddn pio-read ok len=%u fifo=%08x`。

**(b) 数据通路（这轮真正的修复尝试）**

上一张照片（`boot-uefi-pio-dbg.img`）里 cell 22 红、`BLOCK NONE`。

**验证镜像里到底有什么：不要用字符串扫 `*.fd`。** 这一轮试过
`IndexOf("pio-read")` / `IndexOf("BLOCK NONE")` / `IndexOf("DANDELION")`，结果全是 -1
—— 连我们自己控制台里一定存在的字符串都扫不到，因为 FV 里的模块是**压缩存储**的
（`_FVH` 在偏移 40，但模块正文不是明文）。唯一可靠的验证是：**构建完看构建树里的源文件**
（`tools/build-local.sh` 第 2 步之后 `grep` 一下），或者看 `BUILDLOG_dandelionPkg.txt`
里该模块的编译行。

**教训：只能改 `Resources/MuPatches`，不能手改构建树** —— 第 2 步的 `git checkout --`
会把手工改动擦掉，而"擦掉了"和"没擦掉"在压缩过的 FD 上肉眼/字符串都看不出来。

这轮 0002 里已经包含的三处（都对得上 `Platforms/Benesse/a04br3Pkg/Drivers/SdCardBlockIoDxe`
——本树里唯一在真机 MT6765 上跑通过数据传输的实现）：

| 改动 | 理由 |
|---|---|
| `MsdcPioRead` 整个重写：按 `MSDC_FIFOCS` 实到的字节数排水，不再要求攒够 128 字节，不再 `ASSERT`，错误时打印寄存器 | 旧函数要 `RxBytes >= 128` 才排一次；硬件阈值是 64，永远凑不满 → 一个字节都读不出，`XFER_COMPL` 到时报 `EFI_ABORTED` |
| 每条命令发出前 W1C 掉所有 latched `MSDC_INT` | `MSDC_INT` 是 latched 状态，旧代码只清它在等的那一位；上一条命令留下的 `DATCRCERR` 会被下一次数据读当成"立刻出错" |
| `MSDC_PATCH_BIT2 = 0x3489180F`、数据命令前重新置 `MSDC_CFG_PIO` + 清 FIFO、26 MHz、跳过 `HS_TIMING` | 见 patch 内注释；PIO 位只在 `MsdcReset()` 之前设过一次，复位后可能已被清掉 |

**(c) 刷完看什么**

1. 面板文字区是否**干净、无重影**（这才是 (a) 是否生效）；
2. 有没有 `ddn pio-read FAIL ...` / `ddn  cfg=...`：有就把这两行的四个寄存器贴出来，
   `int` 的 bit14(`0x4000`)=DATTMO（数据根本没来）、bit15(`0x8000`)=DATCRCERR（来了但采样错），
   `int` 里没有数据位而有别的位 → 就是 latched 残留；
3. `ddn pio-read ok` 出现 + `BLOCK OK` + `FAT OK` → M4 达成，直接进 M5。

### 5.7 2026-10-29 02:2x 实测：DATTMO，一个字节都没来

`boot-uefi-pagelog-pio2.img` 的照片上读到了这两行（第一次真正看到寄存器，不是猜）：

```
ddn pio-read FAIL int=00004000 left=512
ddn  cfg=0200049B fifo=00000000 sts=00100000 dcrc=00000000
```

| 值 | 含义 |
|---|---|
| `int=0x00004000` | **只有 DATTMO**（bit14）。没有 XFER_COMPL、没有 DATCRCERR、也没有 ACMD 任何位 |
| `fifo=0x00000000` | 512 字节的时间窗里，FIFO **一个字节都没进过** |
| `left=512` | 一个字节都没读走（和 fifo=0 自洽） |
| `dcrc=0x00000000` | 没有任何数据线的 CRC 状态令牌 → 根本没有数据传输 |
| `cfg=0x0200049B` | MODE=1、CKPDN=1、**PIO=1**（bit3，确认在 PIO 模式）、CKDRVEN=1、CKSTB=1（时钟稳）、CKDIV=4、SCLK_STOP_DDR=1 |
| `sts=0x00100000` | SDC_STS bit20（MTK 未公开位；SDCBUSY/CMDBUSY 都没置，即控制器不认为命令忙） |

**结论**：这不是采样点问题（那是 CRCERR），是**卡根本没有发出数据**。方向只剩两个：

1. **卡不在 Transfer 状态**：`SEND_EXT_CSD`(CMD8) 是寻址命令，卡只有在 Tran 才能回数据。
   如果 CMD7(SELECT_CARD) 的 RCA 不对，卡会停在 Stand-by，命令照样应答
   （CMDRDY 会置位，所以我们没在命令阶段失败），但不会送数据 → 正好 DATTMO + fifo=0。
2. **DAT0 根本没连到控制器**（pad 复用/驱动能力），也是 DATTMO + fifo=0。

所以下一版把**数据命令的 R1 存到读数条**（格子 6）：R1 的 bit12:9 是 CURRENT_STATE，
`4` = Tran。同时把 LK `msdc_init_hw()` 里我们漏掉的信号完整性寄存器补齐
（`MSDC_PAD_TUNE0/1`、`MSDC_DAT_RDDLY0..3`、`SDC_FIFO_CFG` 的 b24/b25），
并清掉 `SDC_CFG.SDIO`（eMMC 不该置这一位，LK 只在 SDIO 卡上置）。

> 还有一条现成的旁证：`bootshim/msdc.c`（线 A 的日志通道，**在本机真机上已经读写通过**）
> 的头注释写着「LK 刚用 MSDC0 载入了这个镜像，所以控制器已上时钟、pad 已复用、
> 卡已用 RCA=1 选中并处于 Transfer 状态 —— 所以我们不去重新初始化」。
> 这正好说明：**这条 eMMC 的硬件路径本身是好的**，UEFI 里失败的是"重新初始化"这一段，
> 而最可能差的就是卡状态（RCA/CMD7）。


### 5.1 把 `GraphicsConsoleDxe` 从 FV 里摘掉（**改的是 `APRIORI.inc`，不是 DSC**）

> 2026-10-28 实测更正：原先以为「DSC 里还在编，FV 可能仍包含它」——
> 不对。DSC 的 `[Components]` 只负责**编译**，往 FV 里放模块的是 FDF。
> 而 `GraphicsConsoleDxe` 进 FV 的路径是 `Include/APRIORI.inc` 的 APRIORI 列表
> （`DXE.inc` 里那行之前已经被注释，但 APRIORI 是第二条路）。
>
> 判定方法（不用猜）：构建后直接在 `Build/.../FV/FVMAIN.Fv` 里搜该驱动的文件 GUID
> `CCCB0C28-4B24-11d5-9A5A-0090273FC14D`。
>
> | APRIORI 那行 | FVMAIN 里搜到 GUID |
> |---|---|
> | 保留 | 1 次（两个 console 同时存在） |
> | 注释掉 | 0 次 |

- 改 `Platforms/Xiaomi/dandelionPkg/Include/APRIORI.inc`，把最后那条
  `INF MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf`
  注释掉（**不要**去动 DSC：它在 `SiliciumPkg.dsc.inc` 的 `[Components]` 里，
  是共享文件，编出来不放进 FV 也无害）
- 验证：CI 构建后确认 FV 中已无该 GUID

做完后 `ConSplitter` 只剩我们的控制台，BDS 的文字应立刻变清晰。

### 5.2 如果 5.1 还不够

在 `DandelionConsoleDxe` 里主动屏蔽：安装完协议后，
`gBS->LocateHandleBuffer(ByProtocol, &gEfiGraphicsOutputProtocolGuid, ...)` 并
`DisconnectController` 掉其他 console 设备；或直接把 `gST->ConOut` 指向我们的实例。

### 5.3 之后才是 M4

`MsdcDxe` + FAT，让 BDS 能找到 `\EFI\BOOT\BOOTAA64.EFI`（即 `UniXNU.efi`）。
硬骨头是 **dandelion 用 MT6357，仓库里继承的是 MT6358** 的 PMIC 驱动，
eMMC 的 `vmmc`/`vemmc` 上电时序对不上。

---

## 6. 构建 / 刷机流程（可直接复制）

### 6.1 换电脑后从零跑起来（2026-10-29 更新）

本地构建**不需要**在 Windows 上装 EDK2，只需要 WSL 里有一份 EDK2 树：

```powershell
# 1) WSL（默认发行版是 docker-desktop，必须显式指定 Ubuntu）
wsl -d Ubuntu -e bash -lc "uname -a"

# 2) EDK2 树：git clone 上游 fork 到 WSL 的 ext4（不要放 /mnt/d，9p 会卡死）
wsl -d Ubuntu -e bash -lc "cd ~ && git clone --recurse-submodules https://github.com/mouseos/uefi-a04br3.git"
#    目录名用 uefi-a04br3，tools/build-local.sh 默认就找 ~/uefi-a04br3

# 3) 一条命令构建（overlay 我们的包 → 重置并应用补丁 → 编 → 拷 FD 到 D:\aios\artifacts）
wsl -d Ubuntu -e bash -lc "bash /mnt/d/aios/dandelion-xnu/tools/build-local.sh"

# 4) 打包 + 刷机（脚本会打印镜像 hash 和"该拍哪里"）
python tools\package_uefi_bootimg.py --stock D:\aios\artifacts\boot_backup.img `
    --fd D:\aios\artifacts\SILICIUM_UEFI.fd --out D:\aios\artifacts\boot-uefi-test.img
powershell -File tools\flash.ps1 -Image D:\aios\artifacts\boot-uefi-test.img
```

构建前建议先跑三个自检（都不需要真机，几秒钟）：

```powershell
python tools\check_modules.py      # 三个模块清单是否一致
python tools\check_docs.py         # 文档引用是否都存在
wsl -d Ubuntu -e bash -lc "python3 /mnt/d/aios/dandelion-xnu/tools/check_patches.py"  # 补丁 + 要点标记
```

**也可以走 CI**：push 之后 ~6 分钟出 artifact `dandelion-fd`（见下），但本地构建更快、且
`build-local.sh` 与 CI 步骤一一对应，两者产物应当一致。

### 6.2 CI

```
push 改动（Silicon/MediaTek/MT6765Pkg/**、Platforms/Xiaomi/dandelionPkg/**、
Resources/** 或 workflow 文件）→ ~6 分钟 → artifact dandelion-fd
```

下载 artifact（需要 GitHub token）：

```powershell
$cred = "protocol=https`nhost=github.com`n`n" | git credential fill 2>$null
$tok = (($cred | Select-String "^password=") -replace "^password=","").Trim()
$h = @{ Authorization = "Bearer $tok"; Accept = "application/vnd.github+json" }
$r = Invoke-RestMethod "https://api.github.com/repos/licg89/dandelion-xnu/actions/workflows/build-uefi-dandelion.yml/runs?per_page=1" -Headers $h
# success 后：artifacts[0].archive_download_url → Expand-Archive → 找到 SILICIUM_UEFI.fd
```

### 6.3 打包与刷机

打包（**必须带 `--kernel-addr 0x40080000`**；`tools/package_uefi_bootimg.py` 会自动读 toml 并断言）：

```powershell
python tools\package_uefi_bootimg.py --stock D:\aios\artifacts\boot_backup.img `
    --fd D:\aios\artifacts\SILICIUM_UEFI.fd --out D:\aios\artifacts\boot_x.img
# 输出固定 16099328 字节
```

刷机（`fastboot boot` 在本机不可用，必须 `flash boot`）：

```powershell
powershell -File tools\flash.ps1 -Image D:\aios\artifacts\boot_x.img
# 或手动：adb reboot bootloader → fastboot flash boot … → fastboot reboot
```

**测试完必须恢复 Android**：

```powershell
powershell -File tools\flash.ps1 -Restore
# 或手动：fastboot flash boot D:\aios\artifacts\boot_backup.img
```

手动进 fastboot（死机时）：长按电源 10 秒关机 → 音量下 + 电源。

---

## 7. 踩过的坑（省时间用）

| 坑 | 说明 |
|---|---|
| FD 基址 | 用 `0x48880000` 时 FD **从不执行**；必须 `0x40080000` |
| CI 从未应用 `MuPatches` | 已修，用 `git -C "$WORK" apply`（绝对路径） |
| YAML 步骤名带冒号 | `- name: Dandelion: ...` 会让整个 workflow 解析失败、jobs 为空 |
| 新驱动必须登记两处 | DSC 的 `[Components]`（编译）**和** FDF 的 `DXE.inc`（进 FV），缺一个就构建失败或进不了 FV |
| APRIORI 也会往 FV 里放模块 | `Include/APRIORI.inc` 是进 FV 的第二条路；只注释 `DXE.inc` 摘不掉一个驱动 |
| INF 缺 `[Protocols]` | 用到的 GUID 不声明会 `lld-link: undefined symbol: gEfi...Guid` |
| `EFI_SIMPLE_TEXT_OUTPUT_MODE` | 没有 `Columns`/`Rows` 字段，要用 `QueryMode()` 取 |
| PowerShell 里写中文 | 容易乱码，文档请用 `write` 工具写 UTF-8，或脚本文件而非 here-string |
| `git apply` 补丁 | 手写 unified diff 容易坏（空行/空格），优先用 CI 里的 `sed` 改 |
| 屏幕判断 | 分辨率对 ≠ 显示对，一定要看 `FrameBufferBase` 和 `Pitch` |

---

## 8. 文件地图

| 文件 | 作用 |
|---|---|
| `Platforms/Xiaomi/dandelionPkg/Drivers/PlatformProgressDxe/` | 诊断驱动：直接写 framebuffer 的彩色文字 / 5 倍放大字 / GOP 与 ConOut 探测 |
| `Platforms/Xiaomi/dandelionPkg/Drivers/DandelionConsoleDxe/` | 自写 `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL`（8×16，90×100，stride 2944） |
| `Platforms/Xiaomi/dandelionPkg/Drivers/*/DandelionFont.h` | 8×16 点阵（由 `bootshim/font.h` 生成） |
| `Platforms/Xiaomi/dandelionPkg/Library/MemoryMapLib/MemoryMapLib.c` | 内存映射；`Display Reserved` = `0x7EC50000`（关键） |
| `Platforms/Xiaomi/dandelionPkg/Include/DXE.inc` | FV 里包含哪些 DXE 驱动 |
| `Platforms/Xiaomi/dandelionPkg/dandelion.dsc` | 组件、PCD、编译开关（`USE_CUSTOM_DISPLAY_DRIVER = 0`） |
| `Resources/Configs/dandelion.toml` | `[uefi_fd].base = 0x40080000` |
| `Resources/MuPatches/` | 上游 Mu 的补丁（CI 自动应用） |
| `.github/workflows/build-uefi-dandelion.yml` | 云端构建（含 `SimpleFbDxe` pitch `sed`） |
| `bootshim/fb.h` | 面板参数权威值 |
| `bootshim/patch_boot.py` | 打包成可刷的 boot.img |
| `tools/patch_fd_base.py` | 不重新编译就改 FD 基址 |
| `docs/CORRECTIONS.md` | FD 基址的 5 组对照实验（C7/C8） |
| `docs/HANDOVER-UEFI-1028.md` | 本轮详细日志（ConOut 实测、CI 修法） |
| `docs/NEXT-M3-GOP.md` | GOP 排查全过程 |

---

## 9. 上游与身份

- **基座**：`mouseos/uefi-a04br3`（Mu / Mu-Silicium），CI 克隆后叠加本仓库的
  `Platforms/Xiaomi/dandelionPkg`、`Silicon/MediaTek/MT6765Pkg`
- **不是** unixnu.com 那套。UniXNU 是本线的**下游**：`PayloadBootDxe` 已经能加载
  `\UniXNU.efi`（或 `\EFI\BOOT\BOOTAA64.EFI`），设备树也已经按 `gFdtTableGuid` 发布，
  所以 UniXNU 那一侧只需要产出 payload，不需要改我们的固件
- 设备序列号 `QS55XCPRCA9LDA5T`；工具在
  `C:\Users\li\AppData\Local\Android\Sdk\platform-tools\`

---

## 10. 接手后的第一个动作（2026-10-29 晚 更新）

```powershell
git pull
# 1) 先看两份文档：docs\RESUME-1029.md（现状 + 本轮做了什么）、
#    docs\POWER-BEFORE-AFTER.md（供电探针的坐标与判读表）
# 2) 三个自检（几秒钟，不需要真机）
python tools\check_modules.py
python tools\check_docs.py
# 3) 刷当前镜像，拍一张屏幕下半部
powershell -File tools\flash.ps1 -Image D:\aios\artifacts\boot-uefi-state.img
# 4) 按 POWER-BEFORE-AFTER.md §4 读：PING 的 cl= （CMD 有没有动）+ SET mode 的读回 + PAD0/PAD1
```

现在的卡点只有一个：**卡有电、总线正常，但对任何命令零应答**。
上一轮已经把「供电」排除（`VEMC en=1`、`PS DAT=0xFF CMD=1`），所以这一版直接问卡：
手工 `CMD1` + 命令期间采样 `MSDC_PS`，再换一个引脚复用模式重问一次。
