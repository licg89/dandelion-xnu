# 启动流程：这一步跑的是谁，屏幕上应该看到什么

这份文档回答一个具体问题：**刷进去之后，屏幕上的每个现象对应固件里的哪一步。**
顺序来自 `Platforms/Xiaomi/dandelionPkg/Include/APRIORI.inc`（决定调度顺序）；
"进不进 FV"由 `Include/DXE.inc` 与 `dandelion.fdf` 决定（`CORRECTIONS.md` C13）。

> 面板坐标：文字区 y 0..1087（68 行 × 90 列）；仪表区从 y 1088 往下。
> 任何证据都必须落在 0 ≤ y < 1600 之内 —— 这条踩过（见 `CORRECTIONS.md`）。

---

## 1. SEC / PEI（在 FD 被 LK 跳转之后）

| 阶段 | 做什么 | 屏幕上 |
|---|---|---|
| SEC | `Silicon/MediaTek/MT6765Pkg` 的 SEC：建立最初的执行环境 | 无（面板还是 LK 留下的内容） |
| PEI | `PlatformPeiLib`：内存、时钟、最小初始化 | 无 |
| 交接 | PEI 把控制权交给 DXE Core | 屏幕开始被 `DandelionConsoleDxe` 一次性擦干净（y 0..1391） |

## 2. DXE 早期（还没有控制台）

| 顺序 | 模块 | 作用 | 屏幕证据 |
|---|---|---|---|
| 1 | `PlatformProgressDxe` | 解除看门狗（`TOPRGU WDT_MODE`），画 y 1560 的白条 | 白条出现 = DXE 起来了 |
| 2 | `DxeMain` / `Pcd` / StatusCode / RuntimeDxe | Mu 的 DXE 内核与基础服务 | 文字区开始有 DEBUG 输出（此时 ConOut 还没有，输出会被丢弃） |
| 3 | `CpuDxe` / `ArmGicDxe` / `MetronomeDxe` / `TimerDxe` | CPU、GIC、定时器 | 无 |
| 4 | `GpioDxe` / `ClockDxe` / `PmicWrapperDxe` / `MT6358PmicDxe` | GPIO / 时钟 / PMIC 协议 | `MsdcImplLib` 的状态格 1/2/3（见下） |

## 3. eMMC 路径（M4，当前卡点）

| 顺序 | 模块 | 作用 | 面板证据 |
|---|---|---|---|
| 5 | `DiskIoDxe` / `PartitionDxe` | 块设备与分区层 | 无（还没有块设备） |
| 6 | `MsdcDxe` | MSDC0 控制器 + eMMC 识别（CMD0/1/2/3/7/6） | **状态格 0..23**（三排八格，y 1400/1448/1496）+ **读数条**（y 1244..1373） |
| 7 | `EmmcDxe` | 绑定 pass-thru，读 ext-CSD，发 BlockIo | 文字区 `= Dump Emmc Csd Register=` 与 `ddn …` 行；格 16..23 |
| 8 | `FatPkg` / `EnglishDxe` | FAT 文件系统 | `StorageBeaconDxe` 的 `FAT OK/NONE` |

**这一段的判读请直接看 [`ANALYSIS-EMMC-DATA-PATH.md`](ANALYSIS-EMMC-DATA-PATH.md)。**
一句话：命令全通、数据读 `DATTMO`；这一版先试 DMA，失败自动退回 PIO。

## 4. 控制台

| 顺序 | 模块 | 作用 | 面板证据 |
|---|---|---|---|
| 9 | `SimpleFbDxe` / `KeypadDeviceDxe` / `KeypadDxe` | GOP、按键 | 无 |
| 10 | `ConPlatformDxe` / `ConSplitterDxe` | 标准控制台聚合 | 无 |
| 11 | `DandelionConsoleDxe` | **我们自己的面板控制台**（2944 stride，不滚动），安装 ConOut + 64 KiB 日志协议 | y 1000/1040 两条自证条；文字区从此可读；ConSplitter 的模式协商靠 `PcdConOutColumn/Row = 90×68` 对齐，否则 ASSERT 死循环 |
| 12 | `StorageBeaconDxe` | 自己做 `ConnectController` 扫描（BDS 不做），统计 BlockIo/FAT；**先做供电探针**（`MSDC_PS` 两次 + MT6357 `VEMC/VMC/VIO18`） | y 1100/1140 横幅、y 1172/1186/1200 三条位图条、y 1216 点阵、格 4/5/6/7/22/23 |
| 13 | `PayloadBootDxe` | 发布设备树（配置表）→ 发布日志（配置表）→ 写日志到 flashinfo → 找 payload 并 `LoadImage`/`StartImage` | y 1570 横条：橙=找、绿=启动成功、红=没有；右端白块=DTB 已发布；左端白块=日志已写入 flash |

## 5. BDS（不在 APRIORI 里，但仍在 FV 里）

`BdsDxe` 会在普通调度阶段被拉起来。它在这块板子上**不能承担启动策略**
（ConSplitter 的控制台模式协商会死循环），所以：

- 它的输出照常显示在文字区（`BdsDxe: loading Boot0001 …` 那一屏就是它）；
- 但它最后只会走到 `No boot options` / `Boot Failure`；
- **真正的"启动下一个东西"是第 13 步的 `PayloadBootDxe`。**

## 6. 屏幕元素速查

| 位置 | 内容 | 谁画的 |
|---|---|---|
| y 0..1087 | 文字（最近 68 行，写满回到第一行覆盖） | `DandelionConsoleDxe` |
| y 40..47 | **状态带**：最后一次判读的颜色（当前是供电结论） | `MsdcImplLib` / `StorageBeaconDxe` |
| y 1000..1030 / 1040..1070 | 洋红 / 黄色自证条 | `DandelionConsoleDxe` 入口 |
| y 1100..1129 / 1140..1169 | 红色横幅（`BLOCK NONE` / `FAT NONE` 时为红） | `StorageBeaconDxe` |
| y 1172..1208 | **供电位图条**：P_before（青）/ P_after（黄）/ VEMC 低 16 位（洋红） | `StorageBeaconDxe` (`ProbePower`) |
| y 1216..1236 | 点阵：块设备数（蓝，x 8）/ 文件系统数（绿，x 380） | `StorageBeaconDxe` |
| y 1244..1373 | **永久读数条**（两列五行，`INTR=…` 等） | `MsdcImplLib` (`MsdcPanelValue`) |
| y 1400 / 1448 / 1496 | 三排八格状态（stage 0..23；22 = PS 已采，23 = 供电判读） | `MsdcImplLib` (`MsdcTrace`) |
| y 1560..1568 | 白色横条 | `PlatformProgressDxe` |
| y 1570..1596 | 满宽横条 + 两端白块 | `PayloadBootDxe` / `PayloadHello` |

## 7. 日志怎么拿（三种，按可用性排序）

1. **面板读数条**：不需要任何东西，永远在屏幕上（拍照即可）；
2. **内存日志 + 配置表**：payload 通过 `gDandelionLogTableGuid` 拿到 64 KiB 全文
   （`EfiRuntimeServicesData`，`ExitBootServices` 之后仍有效）；
3. **flash 里的日志**：M4 通了以后 `PayloadBootDxe` 会把日志写进 `flashinfo` 分区
   起点 +1 MiB 的空闲区（写前校验全 `0xFF`/`0x00`、写后回读比对）；
   用 `tools/extract_log_block.py` 从分区镜像里取回纯文本。
