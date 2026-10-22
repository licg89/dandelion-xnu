# Dandelion XNU Port

**在 Xiaomi Redmi 9A (MT6762G/Helio G25) 上运行 Apple XNU 内核**

## 项目状态

| 组件 | 状态 | 说明 |
|------|------|------|
| Boot Shim | ✅ 完成 | EL2 入口、FDT 解析、ADT 生成、boot_args 构建、EL1 跳转 |
| Framebuffer 控制台 | ✅ 完成 | stride=2944, base=0x7ec50000, 736x1600 RGBA8888 |
| 测试内核加载 | ✅ 完成 | 嵌入 shim 的测试内核成功在 0x60000000 运行 |
| XNU 内核适配 | 🚧 进行中 | 平台定义、配置、GICv3 适配 |
| UART 调试 | ❌ 未实现 | 无物理 UART 访问，使用 framebuffer 替代 |
| pstore 日志 | ❌ 未实现 | ramoops 格式不兼容 |
| USB CDC-ACM | ❌ 未实现 | 实时串口输出方案 |

## 硬件参数

### 设备信息
- **设备**: Xiaomi Redmi 9A (dandelion/blossom)
- **型号**: M2006C3LC
- **SoC**: MediaTek MT6762G (Helio G25)
- **CPU**: 8× Cortex-A53 @ 2.0GHz (2 clusters × 4 cores)
- **GPU**: PowerVR GE8320
- **RAM**: 4GB LPDDR4X
- **存储**: 32/64GB eMMC 5.1
- **屏幕**: 6.53" IPS LCD, 720×1600 @60Hz (NT36525B)
- **系统**: LineageOS 13 (userdebug, bootloader 已解锁)

### 关键地址
| 组件 | 地址 | 说明 |
|------|------|------|
| DRAM 基址 | `0x40000000` | 4GB 连续内存 |
| Framebuffer | `0x7EC50000` | 16MB, stride=2944 |
| UART0 | `0x11002000` | mediatek,mt6577-uart, 921600 baud |
| GICv3 | `0x0C000000` | 中断控制器 |
| MTK Timer | `0x10017000` | 系统定时器 |
| TRNG | `0x1020F000` | 硬件随机数（shim 阶段不可用） |
| Topckgen | `0x10000000` | 时钟门控 |
| Watchdog | `0x10007000` | 看门狗复位 |

### 启动链
```
eMMC Boot Region → Preloader → LK (Little Kernel)
                                    ↓
                            加载 boot.img (Android v0 格式)
                                    ↓
                            解压 gzip(shim) → 0x40080000
                            加载 DTB → 0x47880000 (tags_addr)
                                    ↓
                            跳转 shim @ EL2
                                    ↓
                            shim 诊断 → EL2→EL1 降级
                                    ↓
                            解析 FDT → 生成 ADT → 构建 boot_args
                                    ↓
                            跳转到 XNU 内核 @ EL1
```

## 项目结构

```
dandelion-xnu/
── bootshim/           # Boot shim 源码（v1 目录的完整内容）
│   ├── v1/
│   │   ├── start.S     # AArch64 入口 + 异常向量 + EL2→EL1
│   │   ├── shim.c      # 主诊断逻辑
│   │   ├── fdt.c/h     # FDT 解析器
│   │   ├── fb.c/h      # Framebuffer 控制台
│   │   ├── adt.c       # Apple Device Tree 生成器
│   │   ├── handoff.c/h # 内核加载 + boot_args 构建
│   │   ├── rng.c       # MT6762G TRNG（未验证）
│   │   ├── log.h       # 持久日志（pstore）
│   │   ├── pstore.h    # pstore 格式写入
│   │   ├── font.h      # 8x16 字体
│   │   ├── shim.ld     # 链接脚本
│   │   ├── build.sh    # 构建脚本
│   │   ├── mkpayload.py # 测试 payload 生成器
│   │   └── out/        # 构建输出
│   ── docs/
│       └── BOOTIMG_FORMAT.md  # boot.img 格式分析
├── xnu/                # XNU 内核适配
│   ├── pexpert/
│   │   └── pe_dandelion.h    # 平台定义
│   ├── config/
│   │   └── MASTER.arm64.dandelion  # 内核配置
│   └── iokit/Kernel/arm/
│       └── DandelionPlatformExpert.cpp  # 平台专家（待实现）
├── docs/
│   ├── HARDWARE_ANALYSIS.md   # 硬件分析详细报告
│   ├── BOOT_FLOW.md          # 启动流程详解
│   ├── FRAMEBUFFER.md        # Framebuffer 参数确认过程
│   └── TODO.md               # 待办事项
├── README.md             # 本文件
└── .gitignore
```

## 快速开始

### 前置条件
- Windows 10/11 + Git Bash
- WSL2 with Ubuntu 22.04+
- AArch64 交叉编译工具链 (`gcc-aarch64-linux-gnu`)
- Android 构建工具 (`mkbootimg`, `dtc`)
- ADB + Fastboot
- Xiaomi Redmi 9A (bootloader 已解锁)

### 构建 Boot Shim
```bash
cd bootshim/v1
bash build.sh
```

### 创建 boot.img
```bash
cd bootshim/v1/out
gzip -n -9 -c Image > Image.gz
# 填充到 512 字节边界
python3 -c "import os; size=os.path.getsize('Image.gz'); padded=(size+511)&~511; d=open('Image.gz','rb').read(); open('Image.gz.padded','wb').write(d + b'\\x00'*(padded-size))"
# 追加 DTB
cat Image.gz.padded /path/to/dtb.dtb > kernel_with_dtb.bin
# 创建 boot.img
mkbootimg --kernel kernel_with_dtb.bin --ramdisk ramdisk_empty.bin \
  --header_version 0 --base 0x40000000 --kernel_offset 0x00080000 \
  --ramdisk_offset 0x11B00000 --tags_offset 0x07880000 \
  --pagesize 2048 --cmdline "bootopt=64S3,32N2,64N2" \
  -o dandelion-boot.img
```

### 刷入测试
```bash
# 备份原始 boot
adb shell su -c 'dd if=/dev/block/by-name/boot of=/sdcard/boot_backup.img'
adb pull /sdcard/boot_backup.img

# 刷入 shim
adb reboot bootloader
fastboot flash boot dandelion-boot.img
fastboot reboot

# 观察屏幕输出（framebuffer 控制台）
# 长按电源 15 秒强制重启
# 进 fastboot 恢复
fastboot flash boot boot_backup.img
fastboot reboot
```

## 关键发现

### Framebuffer 参数确认过程
1. 初始假设：720×1600, stride=2880 (720×4) → 文字偏移
2. 内核 sysfs 确认：stride=2944, virtual=736×4800
3. 但 LK 的 framebuffer 可能不同 → 用竖线测试校准
4. 最终确认：stride=2944 时竖线笔直 ✅

### boot.img 格式限制
- LK 内核段格式：`gzip(shim) + pad512 + DTB`（DTB 必须在最后）
- LK ramdisk 段：**会被 LK 覆盖**为启动参数结构
- LK second 段：**不被支持**（导致崩溃）
- payload 嵌入 gzip 流：导致 LK 崩溃

### 解决方案
- 测试内核直接嵌入 shim 二进制
- 生产内核需要研究 MTK boot 格式或自定义 LK

## 参考项目

- [mouseos/xnu_bootshim_benesse_a04br3](https://github.com/mouseos/xnu_bootshim_benesse_a04br3) - MT8167 boot shim
- [mouseos/xnu_device_benesse_a04br3](https://github.com/mouseos/xnu_device_benesse_a04br3) - MT8167 设备配置
- [mouseos/xnu_kernel_benesse_a04br3](https://github.com/mouseos/xnu_kernel_benesse_a04br3) - XNU 内核移植
- [UniXNU/UniXNU](https://github.com/UniXNU/UniXNU) - UEFI 方案 XNU 移植
- [licg89/UniXNU-dandelion](https://github.com/licg89/UniXNU-dandelion) - Redmi 9A UEFI 方案（未完成）
- [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) - Qualcomm 平台 iOS 移植参考

## 许可证

Boot shim 代码：MIT License
XNU 内核：Apple Public Source License 2.0

## 贡献者

- 硬件分析 + boot shim 开发：Claude Code + 用户协作
- 参考实现：yuma shimizu (mouseos)

## 免责声明

本项目仅用于教育和研究目的。在非 Apple 硬件上运行 iOS/macOS 可能违反 Apple EULA。风险自担。
