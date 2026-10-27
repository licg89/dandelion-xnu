# R1：GIC 两安全态，实验计划（含回滚）

> **2026-10-27 重大更正（E0 结果）**
>
> **「非安全态拿不到中断」这个结论是错的。** Android 在同一颗芯片、同样非安全态下
> 正常工作：
>
> ```
>            CPU0    CPU1  ...  CPU7
> 3:        14328   13395  ...  11431   GICv3 30 Level  arch_timer
> ```
>
> Android 的 dmesg 也确认了 redistributor 布局与我们读到的一致：
>
> ```
> GICv3: CPU0: found redistributor 0   region 0:0x000000000c100000
> GICv3: CPU1: found redistributor 1   region 0:0x000000000c120000
> ...
> GICv3: CPU7: found redistributor 103 region 0:0x000000000c1e0000
> ```
>
> 所以 `0xc100000` 是对的，**我们探针在那一帧读到 `GICR_TYPER = 0` 是访问方式的问题**，
> 不是硬件不可用。R1 从「可能无解的固件墙」降级为「我们的配置/访问方式有 bug」。
>
> 下面第 1 节的表格保留作历史记录，其中「SPI/PPI 都收不到」的**解释**是错的：
> 它们收不到，是因为我们那一侧的配置不对，而不是硬件不给。

---

## 0. E0 结果（2026-10-27 实测）

### 0.0 E0.1：中断通路是通的（推翻「收不到」）

```
e01 typer64 0x0 lo 0x0 hi 0x0                    ← TYPER 读不出（但该帧其他寄存器正常）
e01 igroupr0 0x0  isenabler0 0x0  ispender0 0x1000000
e01 ipriority 0x0 icfgr1 0x0 waker 0x0
e01 igroupr w 0x48000000  r 0x0   group1 bits DROPPED
e01 after enable  isenabler0 0x48000000  ispender0 0x49000000
e01 hppir1 0x1b                                   ← 0x1b = 27
```

| 观察 | 含义 |
|---|---|
| `ISENABLER0` 回读 `0x48000000` | PPI 27/30 使能**成功** |
| `ISPENDR0` 回读 `0x49000000` | PPI 27/30 强制 pending **成功** |
| **`HPPIR1 = 0x1b`(27)** | **中断确实到达 CPU 接口** |
| `IGROUPR0` 写后回读 0 | 只是 NS 视角的 RAZ/WI；实际分组已是 Group 1（安全侧设过），**丢写无害** |
| `TYPER` 三种读法都是 0 | CPU0 那一帧的 TYPER 确实读不出，但**不影响功能**（其他寄存器都正常） |

**结论：GIC → CPU 接口的整条通路是好的。** 之前所有「NS 收不到中断」的判断，
是因为测试走的是**定时器自己到期**这条路，而不是 `ISPENDR` 强制 pending。

### 0.0.1 剩下的唯一问题：定时器不产生中断

- 用 `ISPENDR0` 强制 pending → `HPPIR1` 立刻看到 27 ✓
- 靠 `CNTV`/`CNTP` 到期（`ISTATUS=1`）→ `HPPIR` 仍是 1020 ✗

所以缺的只是 **arch timer → redistributor** 这一段。候选原因（按可能性）：

1. MTK 对 timer 到 GIC 的中断线有额外门控（类似 `GIC_SYNC_DCM`，dmesg 里 Android 打开了它）
2. arm/arm 时序：需要「先 arm timer，再 enable PPI」，或反之
3. `ISPENDR0` 里那个残留的 **bit24** 才是被真正路由的定时器 INTID（DTB 说 27/30，
   Android `/proc/interrupts` 说 30 —— 但值得实测确认）

下一步实验（E0.2，纯 shim）：使能 PPI 30 → arm CNTP 让它在 1 ms 内到期 →
轮询 `GICR_ISPENDR0` 和 `ICC_HPPIR1_EL1`，同时打印 `CNTP_CTL` 与
`GICR_IPRIORITYR`/`ICFGR1` 的相关位。**ISPENDR0 有无 bit30 是分水岭**：
有 → GIC 侧没问题、是 CPU 接口/优先级；没有 → 定时器到 GIC 这段不通。

---

## 0.1 历史：E0 的原始结果

```
boot mpidr 0x80000000  affinity 0x0
frame 0xc100000 typer 0x0          aff 0x0   <- self   ← 两次读仍为 0
frame 0xc120000 typer 0x100000100  aff 0x1
frame 0xc140000 typer 0x200000200  aff 0x2
frame 0xc160000 typer 0x300000300  aff 0x3
frame 0xc180000 typer 0x10000000400 aff 0x100
frame 0xc1a0000 typer 0x10100000500 aff 0x101
frame 0xc1c0000 typer 0x10200000600 aff 0x102
frame 0xc1e0000 typer 0x10300000710 aff 0x103
```

- 启动 CPU 亲和度 = `0x0`，`<- self` 正确标在 `0xc100000`
- 布局与 Linux 完全一致 → 地址没错
- **唯一异常**：CPU0 那一帧的 `GICR_TYPER` 两次读都是 0，而其它 7 帧正常

结论：不是地址错，是**我们读/配这一帧的方式**有问题。下一步就查它。

---

## 0.1 下一步（取代原来的 E1）

按代价排序：

1. **在 self 帧上重做一次最小配置并逐项回读**（shim，零风险）：
   - 用 32 位读替代 `dgic_read64` 读 `GICR_TYPER`（高/低半字分开），确认是否为
     64 位访问在该帧上的问题
   - 打印 `GICR_IGROUPR0` / `GICR_ISENABLER0` / `GICR_ISPENDR0` / `GICR_IPRIORITYR`
     的原始值，以及**写入后**的回读
   - 重点：`IGROUPR0` 之前写 `0xffffffff` 回读 0。若真是「NS 对 Group 0 位只读」，
     那 Android 也应该是 0 —— 但 Android 能用 PPI，说明这些位在 Android 启动时
     已经是 **Group 1**。**那就意味着有人在我们之前把它们设成 Group 1 了，
     我们要找的是「为什么我们这次没被设置」。**
2. **确认 PPI 是否真的 pending**：`GICR_ISPENDR0` 在 `CNTP_CTL.ISTATUS=1` 之后应该
   有 bit30。若没有 → 定时器到 redistributor 这一段不通；若有但 `HPPIR` 仍 1020
   → 是 CPU 接口/优先级/分组的问题。
3. **对比 Android 的启动参数**：`dmesg | grep -i gic` 已有；再看 LK 是否根据
   启动目标做不同配置（`fastboot flash boot` + 正常启动，与 Android 相同路径）。

只有在上面都排除后，才考虑动 EL3/LK。

---

## 1. 早期记录（部分解释已被 E0 推翻）

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
