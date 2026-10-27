# R1：GIC 两安全态，实验计划（含回滚）

> 目标：让 dandelion 的非安全态内核**真的收到中断**。
> 这是全项目唯一的 go/no-go 风险：没有 tick 就进不了 iOS 用户态，
> 无论 UEFI 层做得多完整。

---

## 1. 已知事实（全部真机实测，不要重复验证）

| 观察 | 值 | 含义 |
|---|---|---|
| `GICD_TYPER` | `0x790414` | SecurityExtn=1（两安全态），672 条中断线 |
| `GICD_PIDR2` @0xFFE8 | `0x3b` | GICv3（GIC-500 系） |
| `GICD_CTLR` | `0x12` | bit1=EnableGrp1NS **已置位**，bit4=ARE_NS 已置位 |
| 写 `GICD_CTLR = 0x13` 回读 | `0x12` | bit0(EnableGrp0) 写不进 —— 正常，Group0 是 EL3 的 |
| `GICD_IGROUPR*` 写 0xffffffff 回读 | `0` | 分组位对 NS 只读（符合规范） |
| `GICR_IGROUPR0` 写 0xffffffff 回读 | `0` | 同上 |
| `GICR_ISENABLER0` 写 `1<<27` 回读 | `0x8000000` | **使能确实生效** |
| `ICC_SRE_EL1` | `1` | 系统寄存器接口已开 |
| `ICC_PMR/CTLR/IGRPEN1` | 可写 | CPU 接口正常 |
| `CNTP_CTL.ISTATUS` / `CNTV_CTL.ISTATUS` | 都到 1 | **定时器确实到期** |
| `HPPIR`/`IAR` | `0x3fc`(1020, spurious) | 中断没有到达 CPU 接口 |
| 强制 pending 一个 SPI（INTID 200） | `IAR` 仍 spurious | **SPI 也不行** |
| Android `/proc/interrupts` | `GICv3 30 arch_timer`、`GICv3 23 arm-pmu` | **同一颗芯片上 NS 内核确实收到 PPI 30** |

矛盾点只有一处：**同样是非安全态，Android 能收 PPI，我们收不到。**

---

## 2. 一个被忽略的异常（Experiment 0 的由来）

shim 扫描 redistributor 时打印的表：

```
frame 0xc100000  typer 0x0            waker 0x0   (awake)   ← affinity 0？
frame 0xc120000  typer 0x100000100    waker 0x0   (awake)   ← Aff=0x1
frame 0xc140000  typer 0x200000200    waker 0x0   (awake)   ← Aff=0x2
frame 0xc160000  typer 0x300000300                              ← Aff=0x3
frame 0xc180000  typer 0x10000000400                            ← Aff=0x100
frame 0xc1a0000  typer 0x10100000500                            ← Aff=0x101
frame 0xc1c0000  typer 0x10200000600                            ← Aff=0x102
frame 0xc1e0000  typer 0x10300000710                            ← Aff=0x103
```

MT6765 是 2 簇 × 4 核，affinity 应为 `0x0,0x1,0x2,0x3,0x100,0x101,0x102,0x103`。
**8 个帧里正好缺 affinity `0x0`，而第 0 帧（本应是它）TYPER 读出来是 0。**

如果启动 CPU 不是 affinity 0，或者它的 redistributor 不在 `0xc100000`，
那么我们此前所有 `GICR_WAKER` / `ISENABLER0` / `IGROUPR0` 的写入**都打在了别的核上** ——
「NS 收不到 PPI」这个结论就得推翻。

> 注意：shim 的 `dgic_find_redist()` 返回的 `self redist` 也是 `0xc100000`，
> 说明它是按 affinity 0 匹配的，但那一帧的 TYPER 是 0。这点本身就不自洽。

---

## 3. 实验（按 代价×收益 排序）

### E0 — 核对 redistributor 与 CPU 的对应关系（**零风险，先做这个**）

**方法**：shim 的 `probe` 模式里

1. 打印启动 CPU 的 `MPIDR_EL1`
2. 从 `gicr_base - 0x40000` 扫到 `gicr_base + 0x100000`（覆盖可能的偏移错误）
3. 每帧打印 `GICR_TYPER` 的 Aff3..Aff0 解码值、`Last` 位、`Processor_Number`
4. 对每帧 **连读两次** TYPER（排除首读时序问题）
5. 标出与 `MPIDR_EL1` 亲和度一致的那一帧
6. 在那一帧上重新做一次「使能 PPI 27 → 强制 pending → 读 IAR」

**判据**：
- 若找到 affinity 0 的帧且 PPI 能被收到 → **之前的结论作废**，问题只是地址算错
- 若确认 affinity 0 帧的 TYPER 恒为 0 → 该 redistributor 确实不可用，进入 E1

**风险**：无。只读寄存器 + 已有的 PPI 使能。

---

### E1 — 确认中断在哪一层被关（EL3 / ATF / preloader）

**方法**（任选，成本递增）：

1. **从 Android 侧取证**：`cat /proc/interrupts` 已知有效；再取
   `dmesg | grep -i gic`、`/sys/kernel/debug/…`（如果有 debugfs），
   看 Android 的 GIC 初始化是否打印了 redistributor 的 affinity 列表 ——
   **这能直接告诉我们 affinity 0 的帧在哪**。
2. **对比 DTB**：`stock_boot.dtb` 的 `/interrupt-controller@c000000` 里 `reg` 是
   `0x0c000000/0x40000` + `0x0c100000/0x200000`。若 E0 表明 affinity 0 不在
   `0xc100000`，那 DTB 与硬件不一致（MTK 常见），需要以实测为准。
3. **patch preloader/LK 读 EL3 侧状态**：只有在 E0/E1 前两步都失败后才做。

**风险**：第 3 步高（会动引导链）。**必须**先确认备份可用：

```
D:\aios\artifacts\backup\lk.img
D:\aios\artifacts\backup\lk2.img
D:\aios\artifacts\backup\mmcblk0boot0.img
D:\aios\artifacts\backup\dtbo.img / boot_para.img / seccfg.img
```

**回滚**：`fastboot flash boot out\boot_backup.img`；引导器级用
`mtkclient` 写回 `lk.img` / `mmcblk0boot0.img`（设备变砖时用 BROM 模式，已备份即可恢复）。

---

### E2 — patch LK（若 E1 指向 LK）

已知：公开的 MT6765 LK 源码（`svoboda18/lk`）里写 `GICD_CTLR = 0x16`
（ARE|EnGrp1S|EnGrp1NS），而我们实测是 `0x12`（bit2 = EnGrp1S 掉了）。
说明 **LK 之后还有东西（ATF/ROM）改写了它**，或者 LK 的那条路径没执行。

**方法**：在 LK 里把 GIC 初始化挪到更晚（跳转内核前最后一步），并打印回读值。

**风险**：中高。必须先备份 `lk.img`；有 `mtkclient` 可回滚。

---

### E3 — 换中断入口（若 E1/E2 证明 GIC 对 NS 永久关闭 PPI）

把 tick 从 GIC PPI 挪到 MTK 自家的中断汇聚层：

- `sysirq` @ `0x10200b00` 对 NS 可读（实测 `+0x8=0x3`、`+0x18=0x8`）
- MTK GPT @ `0x10017000`（`mediatek,mt6765-timer`）寄存器 NS 可读写，且在跑
- 思路：用 GPT 产生一个 **SPI**，走 `sysirq` 路径拿 tick

**风险**：低（只动内核侧），但工程量最大，且要看 `sysirq` 的父线是否仍在 GIC 上。

---

## 4. 明确不要做的

- **不要**在内核里（MMU 开）写 `GICD_CTLR` / `GICD_IGROUPR` / `GICR_IGROUPR0` ——
  会让总线卡死（`docs/DEAD-ENDS.md` §4）
- **不要**读 `ICC_IGRPEN0_EL1` / `ICC_BPR0_EL1` / `ICC_IAR0_EL1` ——
  两安全态下非安全态读它们是 **UNDEFINED**，内核会静默卡死
  （今天就是被这一条卡了很久，见 `docs/FINDINGS-2026-10-27.md`）
- **不要**再假设「a04br3 能跑，所以 dandelion 也能跑」——
  它是 GICv2 单安全态

---

## 5. 与外部证据的交叉验证

`mouseos/uefi-a04br3` 的 `Status.md`：

| 分组 | 设备 | 状态 |
|---|---|---|
| MT8167 | a04br3 | ✅ iPadOS 桌面（**GICv2**） |
| P65/G80（MT6768） | Redmi 9（lancelot） | ❌ Inactive，仅 WinPE，**One Core only** |
| G99（MT6789） | POCO M5（stone） | ❌ Inactive，全 `?` |

`lancelot` 的「One Core only」和我们的症状一致 —— **MTK GICv3 手机在 NS 下拿不到 PPI
很可能不是 dandelion 的特例**。这意味着 R1 若解不开，MTK 手机这条路整体是关的。

---

## 6. 进度记录

| 日期 | 事件 |
|---|---|
| 2026-10-26 | 发现两安全态；组寄存器只读；PPI/SPI 都收不到 |
| 2026-10-27 | 修掉 `ICC_IGRPEN0_EL1` UNDEFINED（内核得以越过 `arm_init`）；起草本计划；E0 待执行 |
