# 从这里继续（2026-10-29 深夜，最后一次更新）

> **换电脑后第一份读的就是这一页。** 它记录：现在在哪、决定性证据、下一个动作、别再踩的坑。
> 同目录 [POWER-BEFORE-AFTER.md](POWER-BEFORE-AFTER.md) 是每一轮探针的完整判读表；
> [HANDOVER-CURRENT.md](HANDOVER-CURRENT.md) 是更早的环境/流程总览（部分小节已过期，
> 以本页为准）。

---

## ★ 最新结论（2026-10-29 22:2x）——**UniXNU 在我们的固件上有了自己的启动项**

一整条链在真机上成立，日志 + 面板双证（[M6-UNIXNU.md](M6-UNIXNU.md) §6）：

```
INHERIT OK
BLOCK OK n=44
FAT OK                                   ← flashinfo 里的 1 MiB FAT 卷
PayloadBoot: device tree 127044 bytes published
```

```
        ... dandelion   (UniXNU 0.1.0)
        config: \EFI\UniXNU\UniXNU.toml
      > 1. DarwinBoot (dandelion)  [efi]
        2. Return to firmware  [action]
        3. Restart  [action]
        "DarwinBoot (dandelion)" returned: Success
```

```
固件 → eMMC BlockIo → 分区 → FAT(flashinfo) → UniXNU.efi(flashinfo 槽)
   → \EFI\UniXNU\UniXNU.toml → DarwinBoot.efi → 返回 Success
```

**M6 的「加载 UniXNU」「给它启动项」两步都成立。** DarwinBoot 是 `returned` 而不是跳转，
因为它没有内核可跳 —— **绿条不是失败，是「真的走到跳转那一刻」的标志**。

### 下一步（按代价排序）

1. **定 80 vs 90 列**（§4 / §6.2）：UniXNU 与 XNU 都按 80 列排版，我们的控制台是 90。
   在看 XNU 输出之前必须先定，否则整屏都是断行；
2. **给 DarwinBoot 一个内核**：`kernel.macho` 需要 macOS + Xcode，或改用 Apple IPSW 的
   kernelcache（不需编译，但需要设备 profile）；
3. `UniXNURamDiskDxe` 的 initrd 路径（`/chosen` 里的 `linux,initrd-start/end`）——
   我们自己发布了设备树，往 `/chosen` 里加两个属性就能让它把 payload 卷挂起来。

---

## ★ 更早一版（2026-10-29 22:0x）——M6 不再是「外部依赖卡住」

`docs/UNIXNU-BUILD-NOTES.md` 说内核要 macOS，于是 M6 被记成卡在外部产物。**这个判断错了一半**：
内核确实要 macOS，但 **`UniXNU.efi`、`UniXNUDxe`、`UniXNURamDiskDxe`、`DarwinBoot` 全是普通 EDK2 包，
在 Linux 上就编出来了**，而且已经上机跑起来。

证据是固件自己写的日志
[logs/boot-log-2026-10-29-unixnu.txt](logs/boot-log-2026-10-29-unixnu.txt)（3448 字节，checksum ok）：

```
PayloadBoot: using the payload from flashinfo
UniXNU  (UniXNU 0.1.0)   built-in configuration   no boot entry found
        drivers 0 loaded / 0 failed    kits 0    acpi 0
  > 1. Return to firmware  [action]
    2. Restart  [action]
    3. Power off  [action]
    4. Tools  [action]
  Up/Down: select  Enter: boot  1-9: boot entry  Esc: stop timer
  Booting Return to firmware in 10 s ... 1 s
PayloadBoot: StartImage -> Success
```

也就是说：**固件 → BlockIo → flashinfo payload → LoadImage/StartImage → UniXNU 启动管理器**
整条链路通了，而且是 UniXNU 自己正常退出（不是崩）。

产物都在 `Resources/UniXNU/`，重建用 `tools/build_unixnu.sh` 和 `tools/build_darwinboot.sh`。
完整记录见 [M6-UNIXNU.md](M6-UNIXNU.md)。

| 还差 | 需要什么 |
|---|---|
| 启动项（`UniXNU.toml` 的 boot entry） | 现在能写 |
| payload 卷 | `UniXNURamDiskDxe` 内嵌镜像，或在 `flashinfo` 里建 FAT |
| `kernel.macho` | **macOS + Xcode**，唯一真正的外部依赖 |

先验链路、后补内核：DarwinBoot 拿到控制权会打 `DARWINBOOT: command line:` 并在顶部画**绿条**，
看到绿条就说明 `UniXNU → DarwinBoot → 跳转` 整条路是通的。

---

## ★ 更早一版（2026-10-29 21:4x）——日志可读之后，四个 bug 自己浮出来了

有了落盘日志（[logs/boot-log-2026-10-29-flash-payload.txt](logs/boot-log-2026-10-29-flash-payload.txt)，
26 行，之前面板一张照片只能看到 68 行里的尾巴）立刻抓到四件事：

```
CONSOLE OK (720 / 8)x68
BLOCK OK n=44
FAT NONE
PayloadBoot: device tree 127044 bytes published          ← 修好了，之前永远失败
PayloadBoot: replacing the previous log at LBA 122111967 ← 日志可覆盖
PayloadBoot: using the payload from flashinfo            ← payload 从介质加载
PAYLOAD OK - the load/start path works
```

1. **device tree 从来没有发布过。** 校验写的是 `*(volatile UINT32 *)Blob != 0xD00DFEED`，
   这是把字节 `D0 0D FE ED` **按小端**读成 `0xEDFE0DD0` —— 和常量恰好互为字节序颠倒，
   永远不等。日志里那句 `bad magic EDFE0DD0` 一直是对的，只是没人读得到。
   DarwinBoot / UniXNU 靠 `gFdtTableGuid` 找设备树，所以这是 M6 的硬前提。
2. **payload 可以放在 `flashinfo + 2 MiB`。** FV 只剩 ~1.4 MiB，`UniXNU.efi` 塞不进；
   手机分区是 ext4/f2fs，EDK2 读不了（`FAT NONE` 就是这个事实）。
   查找顺序 **FAT → flashinfo → FV**，工具是 `tools/put_flash_payload.py`。
3. **日志刷两次。** 第一次在 payload 之前（payload 可能卡住），第二次在 payload 返回之后，
   否则日志里永远没有"payload 从哪来、成没成"。
4. `CONSOLE OK 90x100` 是过期字面量（真实 90×68），改成运行时按 `QueryMode()` 同一批常量算。

### 读日志的循环（不再拍照）

```powershell
powershell -File tools\flash.ps1                       # 1) 刷 UEFI 跑一轮
# 2) 跑完按住 音量下+电源 进 fastboot，刷回原厂 boot 进 Android
fastboot flash boot D:\aios\artifacts\boot_backup.img ; fastboot reboot
# 3) 拉 flashinfo 并解码（拿到整个 64 KiB 环形日志）
adb shell su -c "dd if=/dev/block/by-name/flashinfo of=/sdcard/fi.img bs=4096"
adb pull /sdcard/fi.img out\fi.img
python tools\extract_log_block.py out\fi.img --out out\last-boot.log --verbose
```

### 仍然待办

| | |
|---|---|
| M6 DarwinBoot → XNU | ⬜ 缺外部产物（`UniXNU.efi` / `UniXNUDxe` / `DarwinBoot` / `UniXNURamDiskDxe`）。**投递通道已经通了**：产物一到，`tools/put_flash_payload.py` 推进 `flashinfo` 就能启动 |
| `flashinfo` 里建 FAT | 想要 `FAT OK` 就得有 FAT 卷；但 `flashinfo` 前 64 KiB 是 MTK 的 `DOWNLOAD INFORMATION` 记录，别动 |

---

## ★ 更早一版（2026-10-29 21:2x）——M4 完成，日志不再靠拍照

**eMMC 读写已全通，日志已经落盘。** 第一次成功取回的完整启动日志在
[logs/boot-log-2026-10-29-first.txt](logs/boot-log-2026-10-29-first.txt)
（`tools/extract_log_block.py` 从 `flashinfo` 解出，621 字节，checksum ok，LBA 122111967）。

```
EMMC OK div=26 sec=0747C000
INH C=02201A99 S=FF027FFC k=1 C'=02210499
INH C8  I=00001100 rev=08 bw=06 hs=01 sec=0747C000
INH B1  I=00001100 S=45464920
INHERIT OK
BLOCK OK n=44
FAT NONE
```

- **`div=26`** —— 时钟自校准第一档就过（CKDIV=26 ≈ 7.7 MHz，四档里最快）；
- **`BLOCK OK n=44`** —— GPT 解析成功：整盘 media + DiskIo 子句柄 + 各分区；
- **`FAT NONE` 是真话** —— 这台机器分区全是 ext4/f2fs，没有 FAT 卷可挂。
  **M4 的判据因此改成「eMMC → BlockIo → 分区表」**，已达成。

### 读日志的循环（以后不用拍照）

```powershell
powershell -File tools\flash.ps1                       # 1) 刷 UEFI 跑一轮
# 2) 跑完按住 音量下+电源 进 fastboot，刷回原厂 boot 进 Android
fastboot flash boot D:\aios\artifacts\boot_backup.img ; fastboot reboot
# 3) 拉 flashinfo 并解码（拿到的是整个 64 KiB 环形日志，不是面板最后 68 行）
adb shell su -c "dd if=/dev/block/by-name/flashinfo of=/sdcard/fi.img bs=4096"
adb pull /sdcard/fi.img out\fi.img
python tools\extract_log_block.py out\fi.img --out out\last-boot.log --verbose
```

### 仍然待办

| | |
|---|---|
| M6 DarwinBoot → XNU | ⬜ 缺外部产物（`UniXNU.efi` / `UniXNUDxe` / `DarwinBoot` / `UniXNURamDiskDxe`） |
| 给 `UniXNU.efi` 找个家 | 有产物后二选一：塞进 FV（还剩 ~1.4 MiB）、或 `flashinfo` 里划一段裸 LBA 由 `PayloadBootDxe` 直接读 |
| `flashinfo` 里建 FAT | 想要 `FAT OK` 就得有 FAT 卷；但 `flashinfo` 前 64 KiB 是 MTK 的 `DOWNLOAD INFORMATION` 记录，别动 |

---

## ★ 更早一版（2026-10-29 21:0x）——M4 数据通路打通

**IMG_5762 证明 eMMC 数据读成功**，方式是照抄仓库里已有的 `bootshim/msdc.c`
（line A 早就用它在这台手机上读写过 eMMC：[EMMC-LOG.md](EMMC-LOG.md)）：

```
COUT 0 50x19INH C=02200199 S=09020000 k=1 C'=02210499
INH C13 I=00000100 R=00000900 st=4
INH C8  I=00001100 rev=08 bw=06 hs=01 sec=0747C000
INH B1  I=00001100 S=45464920
INHERIT OK
```

- `I=00001100` = CMDRDY + **XFER_COMPL**（四轮重新初始化时永远是 `0100`）；
- `S=45464920` = LBA 1 的 GPT 头 `"EFI "`，**512 字节读成功**；
- `bw=06` = `EXT_CSD[183]` **8 位 DDR**，`hs=01` = high speed，`rev=08` = eMMC 5.1；
- `sec=0747C000` = **122,159,104 扇区 ≈ 62.5 GB**（`flashinfo` 的 LBA 122,109,919 落在里面）；
- `S=09020000` = `SDC_CFG` 的 `[17:16]=2`，**主机本来也是 8 位**。

所以真正的错配是「主机把自己设成 1 位、卡还在 8 位 DDR」＋「复位控制器、重新初始化卡」。
`bootshim/msdc.c` 的策略是**继承 LK 留下的控制器与卡状态，绝不重新初始化**，只改时钟分频。

**已落地**：`Platforms/Xiaomi/dandelionPkg/Drivers/DandelionEmmcDxe/` —— 继承 `MSDC0`、
**时钟自校准**（CKDIV `26/52/104/260` 依次试，每档读 LBA 1 校验 `"EFI "`）、
发布 `EFI_BLOCK_IO_PROTOCOL`。**这一版 `ReadOnly = TRUE`**：写路径（CMD24/CMD25）已实现但没打开，
因为这是日常手机，读通路先验证几轮。

本文档其余部分（尤其 §1、§3 那些"卡不发数据"的分析）**保留为历史**；
现在的判读以 [ANALYSIS-INHERIT-VS-REINIT.md](ANALYSIS-INHERIT-VS-REINIT.md) §8 为准，
打包踩过的坑（刷了两轮旧固件）见 [CORRECTIONS.md](CORRECTIONS.md) C16。

**下一个动作**：看面板上是否出现 `EMMC OK div=.. sec=..`、`BLOCK OK`、`FAT OK`。
出现 `FAT OK` 就是 M4 收尾；然后把 `ReadOnly` 打开，`PayloadBootDxe` 的
`FlushLogToFlash()` 就活了 —— **日志写进 `flashinfo`，`adb pull` 就能读，不用再拍照**。

---

## 0. 项目全貌（30 秒）

**目标**：把 Apple 的 XNU 移植到 **Xiaomi Redmi 9A（dandelion，MT6762G / MT6765）**，在真机启动到用户态。

**当前走「线 B」**（线 A 的纯开源 XNU 冻结在 sched_startup，**不要再碰**）：

`
LK 引导 → BootShim(gzip FD @0x40080000) → UEFI(Mu / mouseos/uefi-a04br3)
   → DXE → BDS → UniXNU.efi → DarwinBoot → XNU → IPSW 用户态
`

**硬约束**：这块板**没有 UART**、**没有 USB device mode** ⇒ 诊断只能靠**屏幕**（自写 framebuffer
控制台）和**写入 flash 的日志**（后者依赖 M4，还没有）。

**里程碑**：

| | 状态 |
|---|---|
| M1 编出 FD | ✅ |
| M2 SEC/PEI/DXE | ✅ |
| M3 GOP + ConOut 到面板 | ✅ |
| M3.5 UEFI 文字可读（自写控制台 90×68 文本区） | ✅ |
| M4 eMMC → BlockIo → FAT | 🔄 **命令全通；512 字节数据读拿不到：卡不驱动 DAT** |
| M5 加载 payload + 发布设备树 | ✅（PayloadBootDxe 实测跑通，已把设备树/日志作为配置表交给 payload） |
| M6 DarwinBoot → XNU | ⬜ 缺 UniXNU 产物（外部依赖） |

---

## 1. 一句话现状

eMMC 的**命令通路已经完全打通并逐条验证**：CMD0 → CMD1（到 OCR bit31）→ CMD2（CID）
→ CMD3/CMD7/CMD13（st=4，TRAN、已选中）→ CMD16/CMD6（切模式）/CMD12（stop）。

**卡在数据相位**：CMD8(SEND_EXT_CSD) / CMD17(READ_BLOCK) 命令都成功（CMDRDY，
R1=0x00000900，无错误位），但**没有任何一根 DAT 被驱动**：

- MSDC_PS 的 DAT[7:0] 全程为高（ps=EF/FF，只有 DAT4 轻微抖动）；
- GPIO 独立窗口 gp=DF8/DFF 显示整个等待窗口只有 CMD/CLK 在动；
- FIFO 空（f=00）、没有任何 XFER_COMPL/DATTMO/DATCRCERR；
- k≈15000/30000 ⇒ **时钟一直在跑**；
- 三种总线模式（8-bit 继承 / 8-bit default / 1-bit default）+ CKPDN 清零 + 控制器软复位，
  **结果完全一样**。

也就是说：主机侧该做的都做了，卡状态也正常（TRAN + READY_FOR_DATA），**它就是不发**。

---

## 2. 决定性证据链（按时间，全部有照片编号）

| 轮 | 镜像 | 关键读数 | 结论 |
|---|---|---|---|
| 1–3 | boot-uefi-power/mux/... | PS=81FF0002（DAT=FF CMD=1 CD=1）、VEMC en=1 3000mV、m0=22222200、DIN=0xDFB | **供电/引脚/复用/复位全部排除** |
| 4 | boot-uefi-cmd0.img | C0 I=00000100（此前一直是 0x200 CMDTMO）、PING a R=00FF8080 | 卡第一次回话；根因：**卡不在 idle，CMD1 非法** ⇒ 必须先 CMD0 |
| 5 | boot-uefi-init.img | C1 n=1 R=C0FF8080（bit31=1） | 上电完成；C2..C13 被控制台行数截断（非失败） |
| 6 | boot-uefi-mem.img | C13 st=4、C8 I=00000100 sz=0、B1 S=0 | 进 TRAN；数据相位不存在 |
| 7 | boot-uefi-1bit.img | C6 I=0100/0100 n=1 st=4、C8 I=0100 | CMD6 被接受；仍无数据 |
| 8 | boot-uefi-3modes.img | 三种模式 A8/B8/C8 全 ps=EF/FF | 模式不是原因 |
| 9 | boot-uefi-datwin.img | **崩溃**：读 0x10002010 触发 translation fault | **pad 块不在内存映射里**，别直接读 |
| 10 | boot-uefi-r1.img | R=00000900、gp=DF8/DFF、dir=000、ioc=0 f60=0 adv=0xE63F、CKPDN=1 | 卡无误、GPIO 不抢总线；主机配置与 LK 一致 |
| 11 | boot-uefi-clock.img | 软复位后同值；CKPDN 清零无效；k≈15000 | 时钟一直在跑、复位无效 |
| 12 | **boot-uefi-state.img** | 待拍 | 新增 t=/
=（失败后立刻 CMD13） |

### 2.1 已经确认的技术事实（可以直接拿去用）

- SDC_CMD 的数据类型字段在 **bit 11**（vendor：blklen<<16 | go_irq<<15 | stop<<14 | rw<<13 | dtype<<11 | rsptyp<<7 | opcode），
  CMD8 的正确命令字是 **0x02000888**。写成 bit9 会变成 rsptyp=5、dtype=0。
- SDC_CMD 的 **bit14 = stop**（CMD12 必须带）；SDC_CMD 里没有单独的块长寄存器，
  块长在 [27:16]，块数在 SDC_BLK_NUM(0x50)。
- PIO 数据通路：MSDC_CFG.PIO(bit3) + MSDC_FIFOCS(0x14)(RXCNT=[7:0], bit31=CLR) +
  MSDC_RXDATA(0x1C)；完成位 MSDC_INT.XFER_COMPL(bit12)。
- MSDC_PS：DAT 在 bits 23..16、CMD 在 bit 24、CDSTS bit1、WP bit31。
- SDC_CFG 继承值 = 0xFF007FFC（1-bit、DTOC=0xFF、WRDTOC=0x1FFF）；总线宽度在 [17:16]
  （0=1-bit，1=4-bit，2=8-bit）。
- MSDC_CFG 继承值 = 0x0200F09B（CKDIV=240 ≈ 416 kHz，PIO=1，CKSTB=1，**CKPDN(bit1)=1**）。
- 卡：CID = 9B010059 32503036 34009DB2 3BB919C9（PNM 解出来是 Y2P064，国产 eMMC），
  OCR = 0xC0FF8080（bit31=1 上电完成、bit30=1 扇区模式），RCA=1。
- **GPIO 引脚组**：122..133 共 12 个（CMD/CLK/DAT0..7/DS/RSTB），pinctrl 里
  DIR=0x0、DOUT=0x100、DIN=0x200、MODE=0x300，(pin/32)*0x10 按位、(pin/8)*0x10 每脚 4 bit。
  实测：**只有 CMD(122)、CLK(124) 以及 123 在动**，DAT 一概不动。

---

## 3. 下一步（两个动作，先证明寄存器可信，再看 t=）

### 3.1 先看 `PV` 行：`SDC_RESP0` 到底跟不跟着命令走

**刷 `artifacts/boot-uefi-mem.img`**（sha256 `c66cd965…`，2026-10-29 20:3x 本地构建），
`C13` 那行后面会多出一行：

```
PV pre=00000900 a=00000900 b0=3BB919C9 c=00000900 LIVE
```

它做的是：读一次 `RESP0` → 发 `CMD13`（R1）→ 发 `CMD10`（R2，取 CID）→ 再发 `CMD13`。

| 观察 | 含义 | 下一步 |
|---|---|---|
| `b0=3BB919C9` 且 `b0 != a`、`c == a`，行尾 **`LIVE`** | **RESP0 真的跟着命令走**，`R1=0x00000900` 可信 | 直接看 3.2 的 `t=` |
| 行尾 **`STALE`**（`b0 == a`） | RESP0 是残留，**整张判读表都要重估** | 先查寄存器窗口/时序（`CFG=0x02000000` 这个疑点就是同类），再谈数据相位 |

> 为什么必须先做这一步：`POWER-BEFORE-AFTER.md` 里每一行"R1=0x900 ⇒ TRAN"都建立在
> "RESP0 是本条命令的应答"这个未验证的前提上；`ANALYSIS-INTEN-GATED-POLL.md` 也已经
> 对同一批读数（`CFG=0x02000000`）提出过同样性质的怀疑。

### 3.2 再看 `t=`：卡到底有没有开始传

同一行尝试记录行尾的 **`t=`**（= `CURRENT_STATE`，来自失败后立刻发的 `CMD13`）：

```
A8 I=0100 R=00000900 sz=00000000 ps=EF/FF gp=DF8/DFF f=00 k=14994 c6=00000000 t=4 r=1
                                                                          ^^^^^^^
```

| t= | 含义 | 下一步 |
|---|---|---|
| **5** (DATA) | 卡**已经在发**数据、正在被时钟推，主机没接住 | 主机接收路径：用 `gDS->AddMemorySpace(EfiMemoryMappedIO, 0x10002000, 0x1000, EFI_MEMORY_UC)` + `SetMemorySpaceAttributes` 映射 pad 块，读/修 IES(0x10)、DRV(0x00)、PUPD(0x30)、R0(0x50)、R1(0x60)（vendor 的 pull-up 组合：PUPD=0x401, R0=0xBFE, R1=0x401） |
| **4** (TRAN) | 卡**根本没开始**传输 | 转卡侧：读完整 R1 位（ADDRESS_OUT_OF_RANGE / ILLEGAL_COMMAND）、试 CMD6 写 PARTITION_ACCESS(179)=0、对照 LK 留下的 EXT_CSD 线索；必要时试 CMD18 多块 / CMD23 |
| **0** | 连 CMD13 都没发出去 | 说明控制器仍停在数据相位（`SDC_STS.SDCBUSY`）⇒ 它确实在等数据，按 t=5 方向查 |

刷机（手机必须在 fastboot；MTK LK 不支持 `fastboot boot`）：

```powershell
powershell -File tools\flash.ps1        # 默认就是 boot-uefi-mem.img
# 手机关机：长按电源 ~10 s；然后 电源 + 音量下 进 fastboot
```

---

## 4. 日常工作流（每轮都这样，约 40 分钟）

```powershell
cd D:\aios\dandelion-xnu
# 1. 改代码
clang -fsyntax-only -std=c11 -I "C:\Users\li\AppData\Local\Temp\sbstub" -Wall -Wextra -Wno-unused-parameter 
  Platforms\Xiaomi\dandelionPkg\Drivers\StorageBeaconDxe\StorageBeaconDxe.c   # 需要本地 stub 头
python tools\check_docs.py
git add -A; git commit -m "..."; git push origin HEAD      # push 成功看 stderr 里的 old..new 行
# 2. 等 CI（~7 min），取 artifact
#    https://api.github.com/repos/licg89/dandelion-xnu/actions/runs
#    zip → dandelion-fd/dandelionPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd
# 3. 打包（必须保持 kaddr=0x40080000）
py -3.13 tools\package_uefi_bootimg.py --stock out\boot_backup.img 
  --fd artifacts\SILICIUM_UEFI.fd --out artifacts\boot-uefi-XXX.img
# 4. 提交产物 + 更新 artifacts\README.md / tools\flash.ps1 默认镜像名
# 5. 刷机、重启、拍照、判读（先说「我读到的是 X」，再分析）
```

GitHub API 用 git credential fill 取 token 后带 Bearer；匿名会被限流。

### 4.1 换电脑时必须手动带走的东西

下面这些**不在 git 里**（体积原因），缺了会卡住：

| 路径 | 大小 | 为什么需要 |
|---|---|---|
| `out\boot_backup.img` | 67 MB | **打包模板**：`tools\package_uefi_bootimg.py --stock` 要它；`flash.ps1 -Restore` 也用它恢复安卓。`out/` 在 `.gitignore` 里 |
| `C:\Users\li\AppData\Local\Android\Sdk\platform-tools\` | — | `adb` / `fastboot`（`tools\flash.ps1` 默认从这里找） |

**已经在 git 里、不用重建就能刷的**：

- `artifacts\boot-uefi-mem.img`（16 MB，当前上机镜像，sha256 `c66cd965…`）

另外 D:\aios\xnu（Apple xnu-8796.141.3 的本地移植，**线 A，已冻结**）**没有可推送的远端**：
已在那棵树里提交为分支 dandelion-port（5a023ec），并把整份改动的 patch 放进本仓库
[Resources/XnuPatches/0001-xnu-dandelion-port.patch](../Resources/XnuPatches/0001-xnu-dandelion-port.patch)：
新机器上按 [Resources/XnuPatches/README.md](../Resources/XnuPatches/README.md) 用 git am 恢复即可，不必拷 8 GB 源码。
- `artifacts\SILICIUM_UEFI.fd`（2 MB，同一个 CI 构建）
- 全部源码、`tools\`、`Resources\MuPatches\`、`.github\workflows\`

---

## 5. 坑（都踩过了，别再踩）

1. **控制台文字区只有 68 行**（y 0..1087），DandelionConsoleDxe 的自检把光标留在第 53 行，
   **本驱动只剩约 14 行预算**；超出的行会被裁掉（IMG_5750 就是这么丢掉 C2..C13 的）。
   每加一行诊断，就要从别处省一行。
2. **0x10002000（MSDC0 pad 块）不在内存映射里**，直接读 = 三级页表 fault + 整屏寄存器转储
   （IMG_5755）。要读就先 gDS->AddMemorySpace。
3. **MSDC_PS 的 CMD 位在控制器驱动 CMD 时被屏蔽**，测“有没有发出去”要用 GPIO122。
4. 命令字段：**dtype 在 bit 11**、CMD12 要 **bit14**；写成 bit9 会静默失效。
5. 数据失败后**要发 CMD12**（SDC_CMD bit14），否则卡一直占着 DAT0，下一轮看起来像“卡死了”。
6. git push 输出走 stderr，PowerShell 会报 exit 1；**看 old..new HEAD -> main 这行**才算成功。
7. tools\flash.ps1 输出被 Select-Object -First N 截断会中断脚本，**重启要单独发**。
8. 不要动：供电分支（VEMC/VIO18/PS）、复用分支（m0=22222200）——都已闭环。

---

## 6. 现在不要再做的事

- ❌ 查供电/引脚/复用/时钟 —— 全部排除（见 §2）。
- ❌ 再猜“总线宽度不匹配” —— 三种模式都试过（§1）。
- ❌ 再猜“没有时钟” —— k≈15000 证伪（IMG_5759）。
- ❌ 再猜“控制器没复位” —— 软复位无效（IMG_5759）。
- ❌ 碰线 A（sched_startup 冻结）。

---

## 7. 仓库里相关文件

| 文件 | 作用 |
|---|---|
| Platforms/Xiaomi/dandelionPkg/Drivers/StorageBeaconDxe/StorageBeaconDxe.c | 探针本体：ProbePower() / ProbeMsdcLeftovers() / ProbeCardAndMux() / ReadDataBlock() / ExtCsdAttempt() |
| docs/POWER-BEFORE-AFTER.md | 每轮探针的读数与判读表（最新：V13） |
| artifacts/boot-uefi-state.img | **当前上机镜像**（sha256 4703306c…） |
| tools/flash.ps1 | 刷机 + 屏幕判读图例（默认镜像名在这里） |
| tools/package_uefi_bootimg.py | 打包（--kernel-addr 0x40080000 不变） |
| docs/research/mtk-msdc-pio.md | **最权威的寄存器/命令字/时序参考**（vendor 注释原文 + 预计算命令字） |
| docs/ANALYSIS-EMMC-DATA-PATH.md | Mu / LK / shim 三份实现的差异对照 |
| Resources/MuPatches/0002-*.patch | Mu MsdcDxe 的 PIO 读 + 面板标记补丁 |
| bootshim/msdc.c | LK 侧 PIO 参考实现（同控制器，另一套写法） |
