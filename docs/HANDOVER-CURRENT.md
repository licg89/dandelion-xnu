# 换机交接（2026-10-28 最终状态）

> 这份文件是**唯一入口**。换电脑后先读它，再读文末列出的三份历史文档。
> 所有内容都已推到 GitHub，`main` 分支即最新。

---

## 1. 一句话现状

固件已能在 Redmi 9A（dandelion / MT6762G）真机上从 **SEC → PEI → DXE → BDS** 完整跑起来，
GOP 可用、framebuffer 参数全部实测确认；**唯一未完成的是让 UEFI 控制台的文字正常显示**
（现在能画，但按 720 的 pitch 画到 736 的面板上，糊成横纹）。

---

## 2. 里程碑

| 里程碑 | 状态 | 证据 |
|---|---|---|
| M1 编出 FD | ✅ | GitHub Actions `build-uefi-dandelion.yml` |
| M2 SEC / PEI / DXE | ✅ | 绿带 / 蓝带 / 屏幕文字 |
| M3 GOP + ConOut 到面板 | ✅ | `GOP FOUND` + `GOP 720 OK`，BDS 文字确实出现在面板上 |
| M3.5 UEFI 文字可读 | 🔄 | 见第 5 节，只差把 `GraphicsConsoleDxe` 摘掉 |
| M4 eMMC(FAT) + `UniXNU.efi` | ⬜ | 阻塞：MT6357 vs MT6358 的 PMIC/eMMC 上电时序 |

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

## 4. 最近一次上机的确切画面（2026-10-29 01:0x，`boot-uefi-gpiofix.img`）

屏幕从上到下：

1. 顶部一条**洋红实心带**（framebuffer 第 26..33 行，约 y 26..33）—— 状态带，
   洋红 = 只有我们的控制台报了到，**没有任何 MSDC 阶段覆盖它**
2. 白色 `DANDELION UEFI`（被状态带压掉一半）、绿色 `DXE OK`、黄色 `CONOUT NO`
   —— `PlatformProgressDxe` 直写 framebuffer 的诊断
3. 中间大片"白横纹" —— **不是**驱动画的，是旧内容没被清掉（见 5.4(b)）
4. 清晰可读的 `SELFTEST 0123456789` / `ABCDEFGHIJKLMNOPQRSTUVWXYZ` /
   `DIRECT CALL 0123456789` / `COUT 0 90x19` —— 我们自己的 `OutputString`
5. 底部**洋红实心条（y 1200..1280）+ 黄色实心条（y 1300..1380）** ——
   `DandelionConsoleDxe` 入口处的自证标记
6. 白条（y 1560..1591）—— `PlatformProgressDxe` 画的

读数结论：

- ✅ `DandelionConsoleDxe` 确实被调度、执行、安装协议（两条自证条都在）
- ✅ 我们的渲染通道完全正确（`SELFTEST` / `DIRECT CALL` 都是 1:1 清晰）
- ❌ 状态带是**洋红** ⇒ eMMC 一个阶段都没报 ⇒ 但这不是 eMMC 失败，
  而是 **MSDC 的标记全部打在 ConOut 还不存在的时候**（见 5.4(a)）
- ❌ 没有紫色 / 绿色 ⇒ 没有 BlockIo，也没有文件系统（M4 未达成）

---

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

构建在 **GitHub Actions**，本地不需要 EDK2：

```
push 改动（Silicon/MediaTek/MT6765Pkg/**、Platforms/Xiaomi/dandelionPkg/**、
Resources/** 或 workflow 文件）→ ~7 分钟 → artifact dandelion-fd
```

下载 artifact（需要 GitHub token）：

```powershell
$cred = "protocol=https`nhost=github.com`n`n" | git credential fill 2>$null
$tok = (($cred | Select-String "^password=") -replace "^password=","").Trim()
$h = @{ Authorization = "Bearer $tok"; Accept = "application/vnd.github+json" }
$r = Invoke-RestMethod "https://api.github.com/repos/licg89/dandelion-xnu/actions/workflows/build-uefi-dandelion.yml/runs?per_page=1" -Headers $h
# success 后：artifacts[0].archive_download_url → Expand-Archive → 找到 SILICIUM_UEFI.fd
```

打包（**必须带 `--kernel-addr 0x40080000`**）：

```powershell
python bootshim\patch_boot.py out\boot_backup.img <fd> Resources\DTBs\dandelion.dtb out\boot_x.img --kernel-addr 0x40080000
# 输出固定 16099328 字节
```

刷机（`fastboot boot` 在本机不可用，必须 `flash boot`）：

```powershell
adb reboot bootloader
fastboot flash boot out\boot_x.img
fastboot reboot
```

**测试完必须恢复 Android**：

```powershell
fastboot flash boot out\boot_backup.img
fastboot reboot
# 等 sys.boot_completed=1
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
- **不是** unixnu.com 那套。UniXNU 是本线的**下游**：等 M4 通了才轮到 `UniXNU.efi`
  作为 `\EFI\BOOT\BOOTAA64.EFI` 被 BDS 加载
- 设备序列号 `QS55XCPRCA9LDA5T`；工具在
  `C:\Users\li\AppData\Local\Android\Sdk\platform-tools\`

---

## 10. 接手后的第一个动作

```powershell
git pull
# 1) 把 dandelion.dsc 里的 GraphicsConsoleDxe 也注释掉
# 2) push → 等 CI → 下载 artifact → 打包 → flash
# 3) 看 BDS 文字是否变清晰（不再横纹）
```

如果清晰：M3.5 完成，直接进 M4（eMMC / FAT / UniXNU.efi）。
如果仍横纹：按 5.2 强制让 `ConOut` 只指向我们的实例。
