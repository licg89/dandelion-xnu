# 从这里继续（2026-10-29 深夜，最后一次更新）

> **换电脑后第一份读的就是这一页。** 它记录：现在在哪、决定性证据、下一个动作、别再踩的坑。
> 同目录 [POWER-BEFORE-AFTER.md](POWER-BEFORE-AFTER.md) 是每一轮探针的完整判读表；
> [HANDOVER-CURRENT.md](HANDOVER-CURRENT.md) 是更早的环境/流程总览（部分小节已过期，
> 以本页为准）。

---

## ★ 最新结论（2026-10-29 20:3x）——先读这一节

**答案一直在仓库里：line A 的 `bootshim/msdc.c` 已经在同一台手机上读写过 eMMC**
（[EMMC-LOG.md](EMMC-LOG.md)：往 `flashinfo` 写入 5159 字节并逐字节读回校验通过）。
它的策略是**「继承 LK 留下的控制器与卡状态，绝不重新初始化」**，而 UEFI 侧这几轮做的
恰好是它的反面：复位控制器、发 CMD0、从 IDLE 重新初始化卡、把主机位宽改掉。

`EXT_CSD[183]`（`BUS_WIDTH`）和 `EXT_CSD[185]`（`HS_TIMING`）是**卡的配置，CMD0 清不掉**，
所以重新初始化之后卡仍然是 LK 留下的 8 位 HS 模式，而主机已经把自己设成了 1 位 ——
这就是「命令全通、DAT 一根都不动」的具体形态。

完整判读见 [ANALYSIS-INHERIT-VS-REINIT.md](ANALYSIS-INHERIT-VS-REINIT.md)，
更正见 [CORRECTIONS.md](CORRECTIONS.md) C15。本轮改动：

- `MsdcDxe` 从 `Include/DXE.inc` + `Include/APRIORI.inc` **整条移除**；
- `StorageBeaconDxe` Depex 改 `TRUE`，新增 `InheritProbe()`（逐字照抄 `host_bringup()`），
  打印 `INH C=.. S=.. k=.. C'=..` / `INH C13 ..` / `INH C8 .. rev=.. bw=.. hs=.. sec=..` /
  `INH B1 .. S=..`；
- `A8/B8/C8/D8` 四轮阶梯缩成单条 `A8`；
- `PV ... STALE` 是误读（CMD10 在 TRAN 态非法，bit22 是锁存后由下一条 CMD13 报出的），
  `SDC_RESP0` **是活的**，本仓库所有 R1 都可信。

**下一个动作**：看 `INH` 四行。`INH C8 I=...1100`（含 `XFER_COMPL`）+ `INH B1 S=45464920`
就是成功，接下来把 `bootshim/msdc.c` 的读路径整体搬成一个 `DandelionEmmcDxe` 产出 `BlockIo`，
M4 收尾；判读表在 [ANALYSIS-INHERIT-VS-REINIT.md](ANALYSIS-INHERIT-VS-REINIT.md) §5。

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
| 6 | boot-uefi-blockio.img | C13 st=4、C8 I=00000100 sz=0、B1 S=0 | 进 TRAN；数据相位不存在 |
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

**刷 `artifacts/boot-uefi-inh.img`**（sha256 `ba70223d…`，2026-10-29 20:3x 本地构建），
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
powershell -File tools\flash.ps1        # 默认就是 boot-uefi-inh.img
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

- `artifacts\boot-uefi-inh.img`（16 MB，当前上机镜像，sha256 `ba70223d…`）

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
