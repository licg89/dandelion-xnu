# XNU 集成指南

本仓库目前是**覆盖层**（overlay），不是完整的 XNU 源码树。下面说明如何把这些
文件放进 `xnu-8796.141.3` 并构建 Dandelion 内核。参考实现为
[mouseos/xnu_kernel_benesse_a04br3](https://github.com/mouseos/xnu_kernel_benesse_a04br3)。

## 1. 目录映射

| 本仓库 | XNU 树路径 | 说明 |
|--------|-----------|------|
| `pexpert/pexpert/arm64/DANDELION.h` | `pexpert/pexpert/arm64/DANDELION.h` | 板级特性开关 + 硬件地址 |
| `pexpert/pe_dandelion.h` | — | 转发头，仅用于本仓库的独立编译检查 |
| `iokit/Kernel/arm/DandelionPlatformExpert.cpp` | `iokit/Kernel/arm/DandelionPlatformExpert.cpp` | 平台专家 |
| `config/MASTER.arm64.dandelion` | `config/MASTER.arm64.dandelion` | 内核配置（当前是占位，见 §4） |
| `bootshim/` | — | 独立运行，产出 boot.img |
| `tools/check-platform-expert.sh` | — | aarch64 独立编译检查 |

`pexpert/pexpert/arm64/DANDELION.h` 会被 XNU 的
`pexpert/pexpert/arm64/board_config.h` 以 `<pexpert/arm64/DANDELION.h>`
包含（XNU 的 include 根是 `pexpert/`）。

## 2. makedefs/MakeInc.def

XNU 通过 machine config 选择板级头文件并附加编译参数。参考项目在
`makedefs/MakeInc.def` 中这样写：

```make
SUPPORTED_ARM64_MACHINE_CONFIGS = A04BR3 BCM2837 T6000 T8101 T8103 VMAPPLE
...
MACHINE_FLAGS_ARM64_A04BR3 = -DARM64_BOARD_CONFIG_A04BR3 -mcpu=cortex-a35
```

Dandelion 需要对应地加入：

```make
SUPPORTED_ARM64_MACHINE_CONFIGS = A04BR3 BCM2837 DANDELION T6000 T8101 T8103 VMAPPLE
...
MACHINE_FLAGS_ARM64_DANDELION = -DARM64_BOARD_CONFIG_DANDELION -mcpu=cortex-a53
```

## 3. pexpert/pexpert/arm64/board_config.h

在 Apple 的文件中加入（顺序与 A04BR3 块保持一致）：

```c
#ifdef ARM64_BOARD_CONFIG_DANDELION
#include <pexpert/arm64/DANDELION.h>

#define MAX_L2_CLINE                 6   /* Cortex-A53, 64 字节 L2 行 */
#define MAX_CPUS                     8
#define MAX_CPU_CLUSTERS             2
#define CORE_NCTRS                   8   /* 仅容量；KPC 未启用 */
#endif /* ARM64_BOARD_CONFIG_DANDELION */
```

`MAX_L2_CLINE` 是 L2 cache line 字节数的 log2（64 → 6）。MT6762G 的
Cortex-A53 两个 cluster 的行大小相同，所以取 6 即可。

## 4. 内核配置

XNU 的 `SETUP/config` 只识别 Apple 的 MASTER 语法
（`machine` / `makeoptions` / `options` / `files`），不识别
`CPU_TYPE`、`PAGE_SIZE`、`GIC_BASE` 这类键。仓库当前这个
`config/MASTER.arm64.dandelion` 是早期占位文件，**不能直接用于构建**。

集成时按参考项目的做法：

1. 以 Apple 的 `config/MASTER.arm64.MacOSX` 为模板复制为
   `config/MASTER.arm64.dandelion`；
2. 参照 `mouseos/.../config/MASTER.arm64.a04br3` 的差异调整：
   - `KERNEL_BASE` 去掉 `config_pmap_ppl`、加入 `config_pmap_ppl` 的替代
     （A04BR3 用 `config_pmap_ppl`，Dandelion 同样需要按 16K 页 + 39-bit VA
     选择 pmap 选项）；
   - 关闭 Apple PERFMON / KPC；
   - 关闭依赖 Apple 硬件的驱动；
3. 确认新增的 `options` 在 `config/MASTER`（machine independent）中有定义，
   否则会生成无意义的宏。

构建命令（machine config 需显式传入）：

```bash
make ARCH_CONFIGS=ARM64 KERNEL_CONFIGS=DEVELOPMENT MACHINE_CONFIGS=DANDELION
```

## 5. 平台专家接入点

平台专家默认使用物理地址，可以直接在 MMU 关闭的 shim 交接路径调用。进入
XNU 之后，设备寄存器需要先经 `ml_io_map()` 映射，再替换基址：

```c
/* pexpert/arm64/pe_arm64_init.c, PE_init_platform() 内，pmap 可用之后 */
dandelion_platform_set_mmio_map(
    (uint64_t)ml_io_map(DANDELION_UART_BASE, 0x1000),
    (uint64_t)ml_io_map(DANDELION_GIC_BASE, 0x40000),
    (uint64_t)ml_io_map(DANDELION_GICR_BASE, 0x200000),
    (uint64_t)ml_io_map(DANDELION_WDT_BASE, 0x1000));

dandelion_platform_expert_init((struct xnu_boot_args *)args);
```

注意：`struct xnu_boot_args` 是本仓库对 Apple `boot_args` 的镜像
（`bootshim/handoff.h`，`static_assert` 校验 size = 736、`device_tree`
偏移 96、`command_line` 偏移 108）。接入 XNU 时可直接改用
`<pexpert/boot.h>` 的 `boot_args`，两者布局一致。

其余接入点：

| 功能 | 调用位置 | 调用 |
|------|----------|------|
| 中断 ack/eoi | XNU arm64 中断分发 | `dandelion_gicv3_ack()` / `dandelion_gicv3_eoi()` |
| 中断使能/优先级/路由 | 中断控制器驱动 | `dandelion_gicv3_irq_enable()` 等 |
| 从核启动 | `start_cpu` 的 C 入口 | `dandelion_gicv3_cpu_up()` |
| 时间基准 | `machine_timebase` | `dandelion_timer_frequency()` / `dandelion_timer_ticks()` |
| 看门狗 | 平台初始化/定时器 | `dandelion_watchdog_pet()`（生产环境） |

中断在平台专家返回时保持屏蔽（`DAIF.I = 1`），由 XNU 安装异常向量、
初始化中断框架后再打开。

## 6. 与参考实现的主要差异

| 项目 | A04BR3 | Dandelion |
|------|--------|-----------|
| SoC | MT8167 / Cortex-A35 | MT6762G / Cortex-A53 |
| 中断控制器 | GICv2 + MediaTek AIC | GICv3（`0x0C000000` / `0x0C100000`） |
| CPU 接口 | GICC MMIO | `ICC_*_EL1` 系统寄存器 |
| 定时器 | ARM generic timer | 同上，13 MHz，虚拟定时器 INTID 27 |
| 页大小 | 16KB | 16KB |
| VA | 39-bit | 39-bit |

## 7. 尚未完成

- GICv3 中断控制器与 XNU 中断框架的完整适配（当前只有底层原语）
- 平台专家的调用时机与 `ml_io_map()` 映射（见 §5）
- ADT 属性与 XNU `pexpert` 期望的完整对齐（`/arm-io/gic`、`/chosen` 等）
- 从核启动（PSCI）与 SMP
- 存储（eMMC）、USB、显示驱动
- 完整内核构建与首启验证
