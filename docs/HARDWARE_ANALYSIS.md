# 硬件分析报告

## 设备识别

```bash
adb shell getprop ro.product.model        # M2006C3LC
adb shell getprop ro.product.device        # dandelion
adb shell getprop ro.board.platform        # mt6765
adb shell getprop ro.hardware              # mt6762
adb shell cat /proc/cpuinfo                # MT6762G, 8× Cortex-A53
```

## CPU 详细信息

```
CurrentEL   = 0x8 (EL2)
MIDR_EL1    = 0x410FD034
  Implementer = 0x41 (ARM)
  Variant     = 0xF
  Architecture = 0xD (ARMv8)
  PartNum     = 0xD03 (Cortex-A53)
  Revision    = 0x4
MPIDR_EL1   = 0x80000000 (CPU 0, Cluster 0)
  Aff0 = 0 (CPU within cluster)
  Aff1 = 0 (Cluster ID)
SCTLR_EL1   = 0x30D00800 (MMU off, Cache off)
ID_AA64PFR0 = 0x1002222 (EL0/EL1 AArch64 only)
ID_AA64MMFR0 = 0x1122 (PARange=40bit, 4K/16K/64K pages)
```

### CPU 拓扑
- 8 核心 Cortex-A53
- 2 集群：Cluster 0 (CPU 0-3 @2.0GHz), Cluster 1 (CPU 4-7 @1.5GHz)
- MPIDR Aff0 编码：0x000-0x003 (cluster 0), 0x100-0x103 (cluster 1)

## 内存布局

```
DRAM 基址：0x40000000
DRAM 大小：4GB (0x100000000)
XNU 管理窗口：0x60000000 - 0x78000000 (384MB)
```

### 保留区域 (from FDT)
| 区域 | 地址范围 | 用途 |
|------|----------|------|
| Kernel code | 0x40080000-0x4195FFFF | Linux 内核代码 |
| Reserved | 0x41960000-0x4224FFFF | 保留 |
| Kernel data | 0x42250000-0x42A3CFFF | Linux 内核数据 |
| Reserved | 0x47880000-0x478C5FFF | 保留 |
| Persistent RAM | 0x47CFB000-0x47CFBFFF | Ramoops |
| Persistent RAM | 0x47D5FA00-0x47D5FBFF | Ramoops |
| Reserved | 0x51B00000-0x51CF6FFF | Ramdisk |
| Reserved | 0x78000000-0x7816FFFF | 保留 |
| Reserved | 0x7C000000-0x7C3FFFFF | 保留 |
| Reserved | 0x7FC00000-0x7FEFFFFF | TEE |
| Framebuffer | 0x7EC50000-0x7FBFFFFF | ~16MB |
| System RAM | 0xBF300000-0xBFBFFFFF | 高地址 RAM |

## 外设地址

| 外设 | 地址 | 兼容字符串 | IRQ |
|------|------|------------|-----|
| UART0 | 0x11002000 | mediatek,mt6577-uart | 219 |
| UART1 | 0x11003000 | mediatek,mt6577-uart | - |
| I2C0 | 0x11007000 | mediatek,mt6765-i2c | - |
| SPI0 | 0x1100A000 | mediatek,mt6765-spi | - |
| USB0 | 0x11200000 | mediatek,mt6765-musb | - |
| MSDC0 (eMMC) | 0x11230000 | mediatek,mt6765-mmc | 18 |
| GICv3 Distributor | 0x0C000000 | arm,gic-v3 | - |
| GICv3 Redistributor | 0x0C080000 | arm,gic-v3 | - |
| MTK Timer | 0x10017000 | mediatek,mt6765-timer | 194 |
| TRNG | 0x1020F000 | mediatek,trng | - |
| Topckgen | 0x10000000 | - | - |
| Watchdog | 0x10007000 | - | - |
| Display OVL0 | 0x1400B000 | - | - |
| Display RDMA0 | 0x1400D000 | - | - |
| Display DSI0 | 0x14014000 | - | - |

## Framebuffer 详细参数

通过内核 sysfs 确认：
```bash
/sys/devices/platform/mtkfb@0/graphics/fb0/stride     → 2944
/sys/devices/platform/mtkfb@0/graphics/fb0/virtual_size → 736,4800
/sys/devices/platform/mtkfb@0/graphics/fb0/bits_per_pixel → 32
```

- **物理基址**: 0x7EC50000 (from dmesg: `disphal_allocate_fb pa_start=0x7ec50000`)
- **逻辑宽度**: 736 像素 (720 可见 + 16 填充)
- **逻辑高度**: 4800 像素 (1600 可见 × 3 缓冲)
- **Stride**: 2944 字节/行 (736 × 4)
- **像素格式**: RGBA8888
- **可见区域**: 720×1600 @60Hz

### Stride 校准过程
1. 假设 stride=2880 (720×4) → 文字偏右 ~360 像素
2. 尝试 2944 → 竖线测试显示完全笔直 ✅
3. 确认 stride=2944 正确

## Boot Image 格式

### 原始 boot.img 分析
```
Magic: ANDROID!
Header version: 2
Page size: 2048
Kernel addr: 0x40080000
Ramdisk addr: 0x51B00000
Tags addr: 0x47880000
Kernel size: 13,907,597 (gzip compressed)
Ramdisk size: 2,060,079 (gzip compressed)
Cmdline: bootopt=64S3,32N2,64N2 androidboot.init_fatal_reboot_target=recovery kpti=off buildvariant=userdebug
```

### MTK 启动参数
- `bootopt=64S3,32N2,64N2` 必须位于 cmdline 开头 (offset 0x40)
- 64S3: 64-bit kernel, 32-bit LK, 32-bit preloader
- 32N2: 32-bit kernel params
- 64N2: 64-bit kernel entry

### 内核段格式
```
[gzip(shim)][pad to 512][DTB]
```
- LK 解压 gzip → shim 在 0x40080000
- DTB 紧跟解压后的 shim
- LK 传 DTB 指针给 shim (x0 = 0x47B80000)

### Ramdisk 段问题
- LK **覆盖** ramdisk 区域为启动参数结构
- 无法在 ramdisk 中放置 payload
- Second 段不被支持

## DTB 信息

```
DTB 地址：0x47B80000 (由 LK 设置)
DTB 大小：283,700 字节
DTB Magic: 0xD00DFEED ✅
```

### 内存节点
- 1 个内存区域：0x40000000 + 0x100000000 (4GB)

### 保留区域
- 16 个保留区域（TEE、firmware、display 等）

### Initrd
- 地址：0x51B00000
- 大小：由 LK 设置（原始 89 字节空 ramdisk）

## 分区布局

| 分区 | 设备 | 大小 | 用途 |
|------|------|------|------|
| boot_para | mmcblk0p1 | 512KB | 启动参数 |
| recovery | mmcblk0p2 | 16MB | Recovery |
| para | mmcblk0p3 | 512KB | 参数 |
| expdb | mmcblk0p4 | 10MB | 异常数据库 |
| gsort | mmcblk0p5 | - | 排序 |
| ffu | mmcblk0p6 | - | FFU |
| cust | mmcblk0p7 | - | 定制 |
| vbmeta_system | mmcblk0p8 | - | Verified boot |
| vbmeta_vendor | mmcblk0p9 | - | Verified boot |
| frp | mmcblk0p10 | - | FRP |
| nvcfg | mmcblk0p11 | - | NV 配置 |
| nvdata | mmcblk0p12 | - | NV 数据 |
| md_udc | mmcblk0p13 | - | Modem UDC |
| metadata | mmcblk0p14 | - | 元数据 |
| protect1 | mmcblk0p15 | - | 保护 1 |
| protect2 | mmcblk0p16 | - | 保护 2 |
| seccfg | mmcblk0p17 | - | 安全配置 |
| persist | mmcblk0p18 | - | 持久化 |
| sec1 | mmcblk0p19 | - | 安全 1 |
| proinfo | mmcblk0p20 | - | 产品信息 |
| efuse | mmcblk0p21 | - | eFuse |
| md1img | mmcblk0p22 | - | Modem 固件 |
| spmfw | mmcblk0p23 | - | SPM 固件 |
| scp1 | mmcblk0p24 | - | SCP 固件 1 |
| scp2 | mmcblk0p25 | - | SCP 固件 2 |
| sspm_1 | mmcblk0p26 | - | SSPM 1 |
| sspm_2 | mmcblk0p27 | - | SSPM 2 |
| gz1 | mmcblk0p28 | - | TrustZone 1 |
| gz2 | mmcblk0p29 | - | TrustZone 2 |
| nvram | mmcblk0p30 | - | NVRAM |
| lk | mmcblk0p31 | 384KB | Little Kernel |
| lk2 | mmcblk0p32 | 384KB | LK 备份 |
| boot | mmcblk0p33 | 64MB | 启动镜像 |
| logo | mmcblk0p34 | 8MB | 启动 Logo |
| dtbo | mmcblk0p35 | - | DTB Overlay |
| tee1 | mmcblk0p36 | - | TEE 1 |
| tee2 | mmcblk0p37 | - | TEE 2 |
| super | mmcblk0p38 | ~14GB | Super 分区 |
| vbmeta | mmcblk0p39 | - | Verified boot |
| cache | mmcblk0p40 | - | 缓存 |
| userdata | mmcblk0p41 | ~50GB | 用户数据 |
| otp | mmcblk0p42 | - | OTP |
| flashinfo | mmcblk0p43 | 16MB | Flash 信息 |

## 调试方法

### Framebuffer 控制台
- 所有调试信息输出到屏幕
- 需要拍照记录
- 分辨率：736×1600, stride=2944, RGBA8888

### Pstore 日志（未实现）
- 目标：写入 0x47C90000 (ramoops 区域)
- 问题：ramoops 驱动不识别我们的格式
- 内核配置：`ramoops.mem_address=0x47c90000 ramoops.console_size=0x40000`

### USB CDC-ACM（未实现）
- 目标：通过 USB 输出到电脑 COM 口
- 需要实现 MUSB 设备驱动
- MUSB 基址：0x11200000

## 下一步

1. 修复 pstore 格式或实现 USB CDC-ACM
2. 编译 XNU 内核 for dandelion
3. 适配 GICv3 中断控制器
4. 实现 UART 早期控制台驱动
5. 实现 eMMC 存储驱动
6. 实现显示驱动
