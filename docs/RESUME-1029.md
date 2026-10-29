# 从这里继续（2026-10-29 晚）

> 给下一个会话的第一份文件。读完这一页就该知道「现在在哪、下一步做什么、别再碰什么」。

---

## 0. 项目全貌（30 秒）

**目标**：把 Apple 的 XNU（macOS/iOS 内核）移植到 **Xiaomi Redmi 9A（dandelion，MediaTek MT6762G / MT6765）**，
让它能在真机上启动到用户态。

**当前走的是「线 B」**（线 A 的纯开源 XNU 已冻结在 `sched_startup`，不要再碰）：

```
LK 引导 → BootShim(gzip FD @0x40080000) → UEFI(Mu / mouseos/uefi-a04br3)
   → DXE → BDS → UniXNU.efi → DarwinBoot → XNU → IPSW 用户态
```

**硬约束（这决定了所有设计）**：
- 这块板**没有 UART**、也**没有 USB device mode** ⇒ 没有任何日志出口
- 所以一切诊断只能靠**屏幕**（我们自己写的 framebuffer 控制台）和**持久化到 flash 的日志**
- 因此本项目里「能看见」比「能跑」更重要：状态格子、位图条、彩色状态带都是为此而存在

**里程碑**：

| | 状态 |
|---|---|
| M1 编出 FD | ✅ |
| M2 SEC/PEI/DXE | ✅ |
| M3 GOP + ConOut 到面板 | ✅ |
| M3.5 UEFI 文字可读（自写控制台 90×100） | ✅ |
| M4 eMMC → BlockIo → FAT | 🔄 卡**零应答**，疑似未上电 |
| M5 加载 payload + 发布设备树 | ✅（`PayloadHello` 实测跑通） |
| M6 DarwinBoot → XNU | ⬜ 缺 UniXNU 产物（外部依赖） |

**日常工作流**（每一轮都是这样）：
```
改代码 → push 到 GitHub → Actions 构建(~8min) → 下载 artifact
      → python bootshim\patch_boot.py ... --kernel-addr 0x40080000
      → fastboot flash boot → 重启 → 拍照 → 判读 → 下一轮
```

**环境**：
- 仓库：`D:\aios\dandelion-xnu`
- 设备序列号：`QS55XCPRCA9LDA5T`
- `adb` / `fastboot`：`C:\Users\li\AppData\Local\Android\Sdk\platform-tools\`
- **Android 备份**：`out\boot_backup.img`（要恢复手机时刷它；测试期间一般不需要）
- 拍照：用户会把照片放在一个 HTTP 目录里，给 URL；图片要下载到本地再用读图工具看

---

## 1. 一句话现状

固件能跑到 BDS、能加载 payload、能显示文字；**M4 的真问题不是数据通路、也不是供电**：
卡有电（`VEMC en=1`）、在位、总线干净（`PS DAT=0xFF CMD=1`），却对任何命令零应答。
之前看到的 `R1=0x900`（TRAN）是 LK 交接前留在寄存器里的**过期值**。

下一步是问「卡能不能听见我们」：手工 `CMD1` + 命令期间采样 `PS`，再换一个引脚复用模式重问。
见 [`POWER-BEFORE-AFTER.md`](POWER-BEFORE-AFTER.md)。

---

## 2. 决定性证据链（按时间）

| 步骤 | 现象 | 结论 |
|---|---|---|
| 早期 | `cmd 7/8/9 failed: status=0x12`，`r1=00000900` | 曾误判为「卡在 TRAN、只是没等到完成」 |
| 探针 1 | `INT=00000000`，`INTEN=000BFFFF` | 曾误判为「INTEN 门控、轮询看不到事件」 |
| 双 host 探针 | `H0 C=0200F09B E=00000000 I=00000000 R=00000B00`<br>`H1 C=0200789B E=00000000 I=00000000 R=00000000` | eMMC 在 **H0**；H1 无卡 |
| CMD12 探针 | `S=FF007FFC  P=81FF0002  N=00000001`<br>`CMD12 I=00000200  R=00000000` | **决定性** |

## 3. 为什么说「卡没上电」（**已作废，2026-10-29 晚**）

> ❌ **这一节整体作废。** 它的前提是"`MSDC_PS` 低 8 位 = DAT0..7"，而真实位定义是
> DAT 在 bits 23..16、CMD 在 bit 24（vendor `ComboA/msdc_reg.h`、上游 `mtk-sd.c`）。
> 实测 `PS = 0x81FF0002` ⇒ DAT = `0xFF`、CMD = 1、卡在位：总线是干净的。
> 同时 `MT6357 LDO_VEMC_CON0 = 0x0001`（使能位 = 1）⇒ 轨是开的。
> **卡有电、在位、总线正常，仍然零应答。** 下一轮改问「卡能不能听见我们」，
> 见 [`POWER-BEFORE-AFTER.md`](POWER-BEFORE-AFTER.md) §2。
> 下面保留原文，只为记录这个错误是怎么被读出来的。

三个独立事实指向同一结论：

1. **`CMD12 I=0x200` = `MSDC_INT_CMDTMO`**
   控制器把命令发出去了，自己的超时也正常触发 ⇒ `MSDC_INT` 工作正常，
   「INTEN 门控」的假设**被否掉**。而 `R=0` ⇒ 卡**零应答**。

2. **`C = 0x0200F09B`，分频位 bit8..15 = `0xF0`**
   这仍是 ~400 kHz 的**初始化档位**。若识别真的走完，`MsdcSetMclk(50MHz)` 与
   `MsdcSetBusWidth(8)` 一定会改掉它和 `S`。
   ⇒ `EmmcIdentification` **从来没走过第一关**。

3. **`S = 0xFF007FFC`，bit17..16 = `00`**
   总线宽度仍是 1-bit，同样是「没进过识别流程」的证据。

4. **`P = 0x81FF0002`，低 8 位 = `0x02`**
   eMMC 空闲时 DAT0..7 应被上拉为**全 1**。DAT0 = 低，符合
   「卡 busy」或「卡没供电、上拉不成立」。

5. **`E = 0x00000000`**
   驱动从没配置过 `INTEN`（上游只在轮询前清位），说明它**没走到需要中断的阶段**。

**因此：`R1=0x00000900`、状态 4/5 这些都不是 UEFI 这条路径产生的，是 LK 的残留。**

## 4. 根因方向（**已作废，见 §3 的更正**）

`PowerControl()` 走 PMIC 的 `vmmc` / `vemmc` 轨：

- dandelion 用 **MT6357**
- 仓库继承的 PMIC 驱动是 **MT6358**
- LDO 编号 / 电压表对不上 ⇒ 写进去的是错的寄存器 ⇒ 卡一直没电

这就是项目一开始标记的已知阻塞点，现在有了**直接证据**。

## 5. 下一步实验（只做这一件）

> **2026-10-29 晚：这一节已经实现（两轮）。** 第一轮的结论是「卡有电、总线正常」，
> 见 [`POWER-BEFORE-AFTER.md`](POWER-BEFORE-AFTER.md) §1；现在的镜像在第二轮
> （手工 `CMD1` + 两个引脚复用模式）。代码在 `StorageBeaconDxe.c` 的
> `ProbePower()` / `ProbeCardAndMux()`；屏幕坐标与判读表都在同一份文档里。
> 实现时的两个决定：PMIC **直接 MMIO**（不走 `MTK_PMIC_PROTOCOL`，因为它的名字解析到
> MT6358 的地址，而且它等 FSM 是无界的），以及三组数都进位图条（y 1172 / 1186 / 1200）
> 加文字。下面是原始设计记录，保持原样。

目标：证明「上电前/后 DAT 线电平有没有变」。

1. 在 `StorageBeaconDxe`（我们自己的驱动，`Platforms/Xiaomi/dandelionPkg/Drivers/StorageBeaconDxe/`）
   里，**在读取任何东西之前**先读一次 `MSDC_PS`（offset 0x08），记为 `P_before`；
2. 读一次 PMIC 的 `vemmc` / `vmmc` 控制寄存器（走 `MTK_PMIC_PROTOCOL` 或直接 MMIO），记为 `V`；
3. 再读一次 `MSDC_PS`，记为 `P_after`；
4. 三个值都画到面板（位图条 + 文本）。

判据：

| 观察 | 结论 | 动作 |
|---|---|---|
| `P_after` 低 8 位 ≠ `0xFF` | ~~供电没成~~ 位定义读错了，低 8 位不是 DAT | 已作废，见 §3 更正 |
| `P_after` = `0xFF` 但命令仍无应答 | 供电 OK | 才轮到时钟 / pad tuning / CMD 线 |
| `V` 读出来是 `0xFFFFFFFF` 或 `0` | PMIC 窗口不对 | 先修 PMIC 的寄存器基址 |

## 6. 不要再走的弯路（每一条都有实测反证）

- ❌ 「卡没上电 / 供电没成」—— `VEMC en=1`、`PS DAT=0xFF CMD=1`，两路独立读数都否掉了（IMG_5738）
- ❌ 「DAT 线没被拉高」—— 同上，`PS` 的 DAT 在 bits 23..16，读到的是 `0xFF`
- ❌ 「卡在 DATA 态」—— 那个 `R=0xB00` 是 LK 残留
- ❌ 「`INTEN` 门控导致轮询失效」—— `CMD12` 已经证明 `INT` 正常报 `CMDTMO`
- ❌ 「PIO vs DMA 通路」—— 命令都没人应答，通路无从谈起
- ❌ 「总线宽度 8-bit 不匹配」—— 从未走到设置宽度那一步
- ❌ 「clock 太快」—— 现在就是 400 kHz 初始化档
- ❌ 「`GraphicsConsoleDxe` 的 pitch 导致横纹」—— 真因是 `FillRow` 只清了 1/16 行

## 7. 关键文件

| 文件 | 用途 |
|---|---|
| `Platforms/Xiaomi/dandelionPkg/Drivers/StorageBeaconDxe/StorageBeaconDxe.c` | **本轮已改这里**：`ProbePower()`（PS + MT6357 CON0/ANA）+ `ProbeCardAndMux()`（手工 CMD1 + 两种引脚复用模式） |
| `docs/POWER-BEFORE-AFTER.md` | 判读表：屏幕坐标、地址出处、`PS` 位定义更正、下一步分支 |
| `tools/render_panel_preview.py` | 不用刷机先看布局（三条位图条 + 判读块 + 一排点阵） |
| `Platforms/Xiaomi/dandelionPkg/Library/MemoryMapLib/MemoryMapLib.c` | `MSDC-0 0x11230000`、`MSDC-1 0x11240000`、`Display Reserved 0x7EC50000` |
| `Resources/MuPatches/0002-MsdcDxe-dandelion-panel-markers.patch` | 上游 eMMC 驱动的补丁（**改它要同步 hunk 行数，别手改**） |
| `docs/ANALYSIS-EMMC-DATA-PATH.md` | PIO/DMA 通路调研（现在优先级下降） |
| `docs/ANALYSIS-INTEN-GATED-POLL.md` | 上面被否掉的那个假设，留作记录 |
| `artifacts/boot-uefi-3modes.img` | 本轮镜像（CMD1 探针 + 两种引脚复用）；上一版 `boot-uefi-ping.img` 在 git 历史里 |

## 8. 工程约定（别破坏）

- CI：push 到 `Platforms/**`、`Silicon/**`、`Resources/**`、workflow 会触发；**只改 docs 不会触发**（需要一次无害的代码改动来触发）
- 三个检查器在 CI 里跑：`check_modules.py` / `check_patches.py` / `check_docs.py`。
  `check_docs.py` 会把 `` `Silicon/...` `` 这种代码跨度当成**本仓库路径**，引用上游文件时要写成
  `MediaTekPkg/Drivers/...（上游仓库）`。
- 刷机：`tools/flash.ps1 -Image artifacts\boot-uefi-*.img`；
  打包必须 `--kernel-addr 0x40080000`；`fastboot boot` 在本机不可用，只能 `flash boot`。
- 拍照判读：**先说「我读到的是 X」，等确认后再分析**（这一轮吃过两次亏）。
