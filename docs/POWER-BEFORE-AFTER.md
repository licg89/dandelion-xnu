# 上电前 / 后探针：eMMC 到底有没有电

> 2026-10-29 晚，`docs/RESUME-1029.md` 第 5 节的落地实现。
> 代码在 `Platforms/Xiaomi/dandelionPkg/Drivers/StorageBeaconDxe/StorageBeaconDxe.c` 的
> `ProbePower()`；面板布局在 `tools/render_panel_preview.py` 里有同样的坐标，可离线预览。
> 这一轮只做这一件事：把「卡有没有电」从推理变成读数。

---

## 1. 问题、以及为什么 PS 一个数不够

前面的结论（`docs/RESUME-1029.md` 第 2、3 节）：卡对任何命令**零应答**，`CMD12` 拿到
`MSDC_INT_CMDTMO`（说明 `MSDC_INT` 本身正常），`MSDC_PS` 低 8 位 = `0x02`，
`MSDC_INTEN` 从没被配置过。

`PS` 低 8 位不是 `0xFF`，有两种完全不同的解释：

| 解释 | 对应的后续动作 |
|---|---|
| **卡/IO 没有电** | 修 PMIC（MT6357 的 VEMC / VMC） |
| **主机没开数据线上拉，或上拉所在的轨（VIO18/VMC）没电** | 修主机配置 / 另一条轨 |

只看 `PS` 分不出这两条。所以这一轮**同时**读「控制这条轨的寄存器」和「线的电平」，
并且前后各读一次电平 —— 如果中间什么都没变，那至少证明读数是稳定的；
如果变了，说明有东西在上电/掉电。

## 2. 屏幕上看哪里

### 2.1 位图条（BDS 擦不掉，拍照主证据）

| y | 颜色 | 内容 | 位宽 |
|---|---|---|---|
| 1172 | 青 | `P_before` = 进入驱动后**第一条**读到的 `MSDC_PS`（MSDC0 +0x08） | 32 位，bit0 在左 |
| 1186 | 黄 | `P_after` = 读完 PMIC 并等 50 ms 之后的 `MSDC_PS` | 32 位 |
| 1200 | 洋红 | `VEMC` = MT6357 `LDO_VEMC_CON0`（0x1988）低 16 位 | 16 位 |

`PS` 的语义：bit7..0 = DAT7..DAT0，bit8 = CMD。空闲、有电的总线低字节应该是 `0xFF`。

这三行也兼做「走到哪一步」的标记，因为它们各自在关键动作之后才画：

- 只有第一行、没有后两行 → 卡在读 PMIC 的那一刻（这是本驱动里唯一会阻塞的事务）；
- 三行都在 → 探针跑完了，结论看 §2.2 的判读块。

> **先看清楚一件事：这一版里没有任何代码会去打开这条轨**，所以 `P_before == P_after`
> 是**预期**的。它们的作用不是"对比出上电效果"，而是 (a) 证明读数稳定、不是随机噪声，
> (b) 和下面 `V` 的使能位互相对照。真正回答「卡有没有电」的是 `VEMC` 的 bit0 —— 这也是
> 为什么这一轮把 PMIC 寄存器一起读了：只看 `PS` 分不出"没电"和"没开上拉"。

### 2.2 判读块（右端一个方块，一眼定性）

`VEMC` 那一行右边（x 400..560，y 1197..1209）有一个实心方块，颜色就是结论：

| 颜色 | 含义 |
|---|---|
| 绿 | VEMC 读成功且使能位 = 1，`P_after` 低 8 位也是 `0xFF` |
| 橙 | VEMC 读成功且使能位 = 1，但 `P_after` 低 8 位不是 `0xFF`（轨开着、线不跟着高） |
| 红 | VEMC 读成功但**使能位 = 0**（轨是关的，预期分支） |
| 洋红 | PMIC wrapper 事务没完成 —— 先修 PWRAP，别急着下结论 |

同一个颜色还会刷到 y 40..47 的状态带（顶上有直方图坏点，所以两处都放）。

> **这一版不画 16..23 号状态格。** 那 8 个格子里 20/21 是 `MsdcDxe` 的 CMD10 与
> CMD8（eMMC 的 SEND_EXT_CSD），22 是**数据读结果**，23 是命令报错；而
> `StorageBeaconDxe` 在这些之后才跑，画上去就等于**擦掉 M4 的证据**。上一版把 20/21
> 当"探针开始/结束"用过，这次一起撤了。本驱动只画自己的 4/5/6/7。

### 2.3 状态带（y 40..47，最顶上那条）

和判读块同色（顶上有坏点，所以两处都放）：

| 颜色 | 含义 |
|---|---|
| 红 | `RAIL-OFF`：VEMC 使能位 = 0 |
| 橙 | `NO-PULLUP`：轨开着，但 `P_after` 低 8 位仍不是 `0xFF` |
| 绿 | `OK`：轨开着且总线被拉高（或 FAT 已经挂上，M4 完成时优先） |
| 洋红 | `NO-WRAP`：PMIC wrapper 没应答，**别急着下结论** |

### 2.4 文字区（会在 BDS 来的时候被覆盖，能拍就拍）

```
MSDC PASSTHRU
PS before=81FF0002 after=81FF0002
PWRAP init=00000001 failed=00000000
VEMC=00000501 en=1 vosel=5 3300mV
VMC=00000000 VIO18=00000101
POWER RAIL-OFF
```

`en` / `vosel` / `mV` 是从 `VEMC` 里解出来的，省得人眼算位。

> **一个免费的交叉校验**：`VIO18` 是屏幕/IO 域那条轨，屏幕既然亮着，`VIO18=0000...`
> 里 bit0 必须是 **1**。如果它读出来也是 0，说明这条路读到的**不是**我们以为的那颗
> PMIC 的寄存器（地址或 SPI 编码不对），那 `VEMC en=0` 就不能当作结论，先回去修路。

## 3. 这些地址凭什么可信

| 项 | 值 | 出处 |
|---|---|---|
| `PWRAP` 基址 | `0x1000D000` | 厂商 DT `pwrap@1000d000`、LK `PWRAP_BASE` |
| `WACS2_CMD / RDATA / VLDCLR / INIT_DONE2` | `0xC20 / 0xC24 / 0xC28 / 0x0A0` | LK `reg_PMIC_WRAP.h`（MT6765），与 `PmicWrapperImplLib.c` 选给 `SOC_TYPE == 1` 的完全一致 |
| 事务编码 | `(write << 31) \| ((addr >> 1) << 16) \| data`，FSM = `(RDATA >> 16) & 7`，idle = 0，数据有效 = 6 | `PmicWrapperDxe.c`（上游仓库）与 `PmicWrapperImplLib.c` 的 `ArbCapabilities = FALSE` |
| `LDO_VEMC_CON0` | `0x1988` | LK `upmu_hw.h`（MT6357）、mainline `include/linux/mfd/mt6357/registers.h` |
| `LDO_VMC_CON0` | `0x1AB0` | 同上 |
| `LDO_VIO18_CON0` | `0x1958` | 同上 |
| 使能位 | `CON0` bit 0 | mainline `mt6357-regulator.c`：每个 LDO 都是 `enable_mask = BIT(0)`，`enable_reg = *_CON0` |
| VEMC `VOSEL` | bits 10..8，7 档 | `mt6357-regulator.c`：`vsel_mask = 0x700`，`vemc_voltages[] = {…, 2.9, 3.0, …, 3.3}` V，所以 `5 = 3300 mV` |

顺带说明一件**必须记住**的事：仓库里的 PMIC 驱动是 MT6358 的，它的
`ldo_vemc.Con0 = 0x1B1C`，在 MT6357 上完全是另一个寄存器。所以**不**走
`MTK_PMIC_PROTOCOL` 的 `RegulatorIsEnabled("ldo_vemc")`：那条路读的是另一颗芯片的地址，
答案无论是什么都不说明 dandelion 的轨。

## 4. 判读表（照这张看照片）

| 画面 | 结论 | 下一步 |
|---|---|---|
| 只有 `P_before` 一条，下面两条没有 | 卡在读 PMIC 的那一刻（唯一的阻塞事务） | 查 PWRAP / `MSDC_PS` 的读是否真的返回了 |
| 三条都在、判读块**洋红**（`PWRAP failed≠0`） | PMIC wrapper 没应答 | 先修 PWRAP：基址、`INIT_DONE2`、`PmicWrapperDxe` 是否跑过 |
| 判读块**红**（`VEMC en=0`） | **轨确实是关的** —— RESUME §4 的假设被证实 | 写 MT6357 的 VEMC 上电（见 §6） |
| 判读块**绿**、`P_after` 低 8 位 = `0xFF` | 供电 OK，总线被拉高 | 转去查时钟 / CMD 线 / pad tune（RESUME §6 里那几条才轮得到） |
| 判读块**橙**、`P_after` 低 8 位 ≠ `0xFF` | 轨开着但线仍低 | 看 `VMC` / `VIO18` 的使能位（IO 域），再看主机是否开上拉；下一根探针做这个 |
| `P_before ≠ P_after` | 读的过程中有东西在上电/掉电 | 重复一次；先确认读数窗口，再谈结论 |

同时看一眼 16..23 格子（那是上一轮 `MsdcDxe` 留下的、和本轮无关的证据）：22 绿/红 =
数据读成/败，21 白 = 走过了 CMD8（SEND_EXT_CSD）。它们现在**不会再被本轮驱动覆盖**。

## 5. 这一轮**没有**做什么（避免踩上一轮的坑）

- **不写 PMIC**。开关一条轨需要 MT6357 的表在驱动里，不该由一段诊断代码猜地址去写。
  现在的实现只读，`PmicRead16()` 只有 WACS2 的读命令和 `VLDCLR` 确认。
- **不碰**总线宽度、`SDC_CFG`、时钟、DMA/PIO。唯一的写仍然是从 CMD12 那一轮留下来的
  一条有界 `CMD12`。
- **不用无界等待**。上游 `PmicWrapperDxe` 等 FSM 是 `while` 死等；这里每次事务上限
  10 ms（200 × 50 µs），失败就返回 `0xFFFFFFFF` 并把 `Failures` 加一 —— 这块板没有
  UART，不能让「PMIC 卡住」变成「屏幕一片黑、什么都不知道」。
- 面板上的旧位图条（`CFG`/`INTEN` 等）撤掉了，那几条本来就和横幅重叠、且问题已经答过；
  文字里仍然照旧打印，供日志取用。

## 6. 如果结论是「轨关着」（预期的分支）

最小改动，按顺序：

1. 给 dandelion 一个 MT6357 的 regulator 表（或在 `MT6358PmicDxe` 里按 `PcdPmicType`
   选表），最少要有 `ldo_vemc`：`Con0 = 0x1988`、使能 bit0、`VOSEL` bits 10..8、3.3 V 档。
2. `MsdcImplLib::PowerControl()` 在 MSDC0 打开它，**先写电压再置使能**，写完读回去确认
   `en = 1` 且 `vosel = 5`。
3. 打开之后立刻用同一段探针再看一次：`P_after` 低 8 位必须变成 `0xFF`。
   这一步是「上电成功」的判据，不要用「命令有没有通」来反推。
4. 只有第 3 步成立，才回去看 `CMD1` / `CMD2` / `CMD3` 和 `MSDC_INT`。

## 7. 复现

```powershell
# 1. 构建（CI），拿到 artifact 后打包
python bootshim\patch_boot.py ... --kernel-addr 0x40080000
# 2. 刷
powershell -File tools\flash.ps1 -Image artifacts\boot-uefi-*.img
# 3. 拍一张整屏照片（下半部分必须完整）
```

判读时**先说「我读到的是 X」，等确认后再分析** —— 这一轮之前吃过两次亏
（`docs/RESUME-1029.md` 第 8 节）。
