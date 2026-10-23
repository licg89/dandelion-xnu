# Dandelion XNU Port

**在 Xiaomi Redmi 9A (MT6762G/Helio G25) 上运行 Apple XNU 内核**

## 项目状态

| 组件 | 状态 | 说明 |
|------|------|------|
| Boot Shim | ✅ 完成 | EL2 入口、FDT 解析、ADT 生成、boot_args 构建、EL1 跳转 |
| Framebuffer 控制台 | ✅ 完成 | stride=2944, base=0x7ec50000, 736x1600 RGBA8888 |
| 测试内核加载 | ✅ 完成 | 嵌入 shim 的测试内核成功在 0x60000000 运行 |
| 设备 DTB 平台探测 | ✅ 完成 | shim 从设备自己的 DTB 读取 GICv3/定时器/UART/看门狗/CPU，并与 `DANDELION.h` 比对 |
| 真机平台测试负载 | ✅ 完成（待刷入） | 在 EL1 逐项调用平台专家并画到 framebuffer，见 [docs/PLATFORM_TEST.md](docs/PLATFORM_TEST.md) |
| XNU 内核适配 | 🚧 进行中 | 平台定义、配置、平台专家（GICv3/定时器/UART/看门狗）已完成并通过交叉编译；尚缺 XNU 源码树集成与完整内核构建 |
| UART 调试 | 🚧 部分实现 | MT6577 UART 驱动已实现（`0x11002000` @921600 8N1）；物理串口未引出，实际仍以 framebuffer 输出为主 |
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
| GICv3 Distributor | `0x0C000000` | 中断控制器 |
| GICv3 Redistributor | `0x0C100000` | 每 CPU 的 PPI/SGI 帧 |
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
├── bootshim/           # Boot shim（扁平目录）
│   ├── start.S         # AArch64 入口 + 异常向量 + EL2→EL1
│   ├── shim.c          # 主诊断逻辑 + 设备 DTB 平台探测
│   ├── fdt.c/h         # FDT 解析器 + fdt_probe_platform()
│   ├── fb.c/h          # Framebuffer 控制台
│   ├── adt.c           # Apple Device Tree 生成器
│   ├── handoff.c/h     # 内核加载 + boot_args 构建（Apple ABI）
│   ├── rng.c           # MT6762G TRNG（未验证）
│   ├── trng_probe.c    # TRNG 探测
│   ├── usb_cdc.c       # USB CDC-ACM 实验代码
│   ├── log.h           # 持久日志（pstore）
│   ├── pstore.h        # pstore 格式写入（PSTORE_ENABLED 可关闭）
│   ├── font.h          # 8x16 字体
│   ├── payload_start.S # 平台测试负载入口
│   ├── platform_test.c # 平台测试负载（UART/视频/WDT/定时器/GICv3）
│   ├── payload.ld      # 负载链接脚本（0x60000000）
│   ├── shim.ld         # 链接脚本
│   ├── build.sh        # 构建脚本（shim / platform-test）
│   ├── build_trng.sh   # TRNG 探测构建脚本
│   ├── mkpayload.py    # 测试 payload 生成器
│   ├── mkbootimg_mtk.py # MTK boot.img 打包
│   └── read_log.*      # pstore 日志读取工具
├── pexpert/
│   ├── pe_dandelion.h  # 转发头（独立编译检查用）
│   └── pexpert/arm64/
│       └── DANDELION.h # 平台定义 + XNU 特性开关（对应参考项目 A04BR3.h）
├── config/
│   └── MASTER.arm64.dandelion  # 内核配置（MACHINE_CONFIGS=DANDELION）
├── iokit/Kernel/arm/
│   └── DandelionPlatformExpert.cpp  # 平台专家：串口/GICv3/定时器/看门狗/视频
├── tools/
│   ├── check-platform-expert.sh     # aarch64 交叉编译检查
│   ├── make_test_dtb.py             # 合成 MT6765 DTB（主机回归测试用）
│   └── test_fdt_probe.c             # fdt_probe_platform() 主机侧测试
├── docs/
│   ├── HARDWARE_ANALYSIS.md         # 硬件分析详细报告
│   ├── PLATFORM_TEST.md             # 真机平台测试负载使用说明
│   └── XNU_INTEGRATION.md           # XNU 源码树集成步骤（MakeInc/board_config/hook）
├── README.md
└── .gitignore
```

## XNU 平台专家

`iokit/Kernel/arm/DandelionPlatformExpert.cpp` 实现了参考项目 A04BR3 仍标注为
待完成的 "platform interrupt/timer/serial wiring"，对应 Redmi 9A 的硬件：

| 模块 | 实现 |
|------|------|
| 早期控制台 | MT6577 UART @ `0x11002000`，沿用 LK 配置的 921600 8N1 |
| 中断控制器 | GICv3：distributor `0x0C000000`，redistributor `0x0C100000`（帧大小按 GICR_TYPER 判定） |
| 定时器 | ARM generic timer，13 MHz，虚拟定时器 INTID 27（GIC PPI 11） |
| 看门狗 | MTK TOPRGU @ `0x10007000`，提供 pet / disable / reset |
| 视频 | 用 LK framebuffer 参数填充 `boot_args.Video` |

入口为 `dandelion_platform_expert_init(boot_args *)`。默认直接使用物理地址，
可在 MMU 关闭的 shim 交接路径调用；接入 XNU 源码树后，可在 `PE_init_platform()`
中用 `dandelion_platform_set_mmio_map()` 传入 `ml_io_map()` 得到的虚拟地址。

注意：参考项目 A04BR3 使用 GICv2/AIC，而 Dandelion 是 GICv3，这是本移植相对
参考实现的主要差异。中断在平台专家返回时仍保持屏蔽（`DAIF.I=1`），由 XNU 安装
异常向量后再打开。

### 以设备自己的 DTB 为准

参考项目的地址不能直接照搬。shim 启动时会把设备 DTB 中的
`arm,gic-v3`、`arm,armv8-timer`、`mediatek,mt6577-uart`、
`mediatek,mt6589-wdt` 和 CPU 节点解析出来画到屏幕上，绿色表示与
`pexpert/pexpert/arm64/DANDELION.h` 一致；红色则以屏幕上打印的值为准。

要进一步验证寄存器真的可访问，构建并刷入平台测试负载（会实际初始化 GICv3、
读取定时器、关闭看门狗并把结果画到 framebuffer），见
[docs/PLATFORM_TEST.md](docs/PLATFORM_TEST.md)。

接入完整 XNU 源码树的步骤（`MakeInc.def`、`board_config.h`、`ml_io_map()` 调用
位置）见 [docs/XNU_INTEGRATION.md](docs/XNU_INTEGRATION.md)。

## 快速开始

### 前置条件
- Windows 10/11 + Git Bash
- 以下任一工具链：
  - **推荐**：LLVM/clang（带 aarch64 后端 + `ld.lld` + `llvm-objcopy`），无需 WSL
  - 或 WSL2 + `gcc-aarch64-linux-gnu`
- Python 3（打包 boot.img）
- ADB + Fastboot
- Xiaomi Redmi 9A (bootloader 已解锁)

### 构建 Boot Shim
```bash
cd bootshim
bash build.sh                 # 默认：wfe 测试内核
bash build.sh platform-test   # 真机平台测试负载（见 docs/PLATFORM_TEST.md）
```

`build.sh` 会自动选择编译器：有 `clang` 就用 clang，否则用
`aarch64-linux-gnu-gcc`。也可用 `CC=...` 显式指定。

### 编译检查平台专家
```bash
# 无需 WSL：clang 带 aarch64 后端即可
./tools/check-platform-expert.sh

# 或使用 Linux 交叉编译器
CC=aarch64-linux-gnu-g++ ./tools/check-platform-expert.sh
```

### 创建 boot.img
```bash
cd bootshim/out
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
