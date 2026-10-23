# 翻译粒度：Cortex-A53 没有 16 KB granule

**结论：XNU 必须按 4 KiB 页构建，不能用 16 KiB 页。**

这一点决定了后续所有 pmap / 启动代码的写法，所以单独记录证据。

## 1. 硬件寄存器（真机实测）

`platform-test` 负载在真机上读到的 `ID_AA64MMFR0_EL1 = 0x1122`：

| 字段 | 位 | 值 | 含义 |
|------|----|----|------|
| PARange | [3:0] | 0x2 | 40-bit 物理地址 |
| ASIDBits | [7:4] | 0x2 | 16-bit ASID |
| BigEnd | [11:8] | 0x1 | 支持混合端序 |
| SNSMem | [15:12] | 0x1 | |
| **TGran16** | [23:20] | **0x0** | **不支持 16 KB granule** |
| TGran64 | [27:24] | 0x0 | 支持 64 KB granule |
| TGran4 | [31:28] | 0x0 | 支持 4 KB granule |

`MIDR_EL1 = 0x410FD034` → Implementer 0x41 (ARM)、PartNum 0xD03 → Cortex-A53 r0p4。

Cortex-A53 TRM (DDI 0500J) 第 6 章 "About the MMU" 原文：

> A translation granule of 4KB or 64KB.

也就是说 **A53 根本没有 16 KB 粒度**（16 KB 在 ARMv8 里是可选特性）。这一点与
参考项目 mouseos 的 MT8167 / **Cortex-A35** 不同：A35 支持 4K/16K/64K，
所以 A04BR3 的 16 KB 配置不能直接搬到 Dandelion。

第三个独立佐证：这台机器原厂 Android 内核（4.19.127，MTK 自带）的配置为

```
CONFIG_ARM64_4K_PAGES=y
# CONFIG_ARM64_16K_PAGES is not set
CONFIG_ARM64_VA_BITS_39=y
CONFIG_ARM64_VA_BITS=39
```

即 MTK 自己也是按 4 KiB 页 + 39-bit VA 构建的。

## 2. 真机实验（trap-test 负载）

`bootshim/trap_test.c` 用两种粒度分别建立 39-bit VA 恒等映射，只开 MMU、
不开 cache，结果：

| 粒度 | `TCR_EL1` 写入值 | `TCR_EL1` 回读值 | 结果 |
|------|------------------|------------------|------|
| 4 KiB (TG0=0) | `0x2_0099_3519` | `0x2_0099_3519` | **`MMU on`，4K MMU works** |
| 16 KiB (TG0=2) | `0x2_0099_B519` | `0x2_0099_3519` | 停在 `enabling mmu...`（静默死锁） |

两个关键事实：

1. **写 16 KiB 的 TG0 会被硬件丢掉**：bit 15 (TG0[1]) 读回始终是 0，
   其余字段（T0SZ=25、IRGN0/ORGN0=1、SH0=3、EPD1、IPS=2）都保留。
   这正是 "该粒度未实现时 TG0[1] 为 RES0" 的表现。
2. 16 KiB 的页表按 16 KiB 语义填写，但硬件按 4 KiB 语义遍历，
   取到的表项全为 0 → 第一条取指就 translation fault；
   异常向量表本身也在未映射页上 → 二次异常 → 静默死锁（屏幕上没有红字）。

同样的页表结构换成 4 KiB 粒度后一次就通，说明表、属性、MAIR、cache 维护
都没有问题，**唯一的变量就是粒度**。

## 3. 对 XNU 的影响

XNU 的 arm64 端口（iOS / Apple Silicon）默认是 16 KiB 页：

```c
#define __ARM_16K_PG__ 1
```

在 Dandelion 上必须改成 4 KiB：

- `pexpert/pexpert/arm64/DANDELION.h`：不定义 `__ARM_16K_PG__`，
  `DANDELION_PAGE_SIZE = 0x1000`；
- `config/MASTER.arm64.dandelion`：`PAGE_SIZE = 4096`；
- 39-bit VA (`T1SZ = 25`) 保持不变，但 4 KiB 粒度下各级索引位移变为
  L1=30、L2=21、L3=12（16 KiB 时是 36/25/14），pmap 里按 `ARM_PGSHIFT`
  推导的地方要逐处确认；
- `TCR_EL1_BOOT`：TTBR0 的 TG0 = 0（4 KiB）；TTBR1 的 TG1 编码不同，
  4 KiB 对应 `0b11`；
- 其余需要打的补丁可以参考 UniXNU 的 BCM2837 4 KiB 配置（同为 Cortex-A53）：
  `arm_init.c` 的页大小选择、代码签名 hash 的移位、user map 的 page shift、
  DMA/IOMMU 表对齐、`ptoa()`/`phystokv()` 的地址掩码、bootstrap 页表预算。

## 4. 下一步验证

在 4 KiB 粒度下补一个 **TTBR1（内核高半区）+ 39-bit VA** 的测试负载，
确认：

- `TCR_EL1.TG1 = 0b11` 能保持；
- `VM_MIN_KERNEL_ADDRESS` 附近的高地址映射可读写；
- 4K 粒度下 2 MB block / 4 KB page 混用正常。

通过之后再进入 XNU 源码树集成。
