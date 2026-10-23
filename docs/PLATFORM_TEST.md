# 真机平台测试负载（platform-test）

目标：在没有可访问串口的情况下，在 Redmi 9A 真机上验证平台专家对
GICv3 / 通用定时器 / UART / 看门狗 / 视频交接的假设。

参考项目（MT8167 + GICv2/AIC）的地址和中断模型与本机不同，因此所有结论
必须以本机为准。本测试提供两份独立证据：

1. **shim 从设备自己的 DTB 读出**中断控制器、定时器、UART、看门狗和 CPU
   节点，并与 `pexpert/pexpert/arm64/DANDELION.h` 中的硬编码值逐项比对；
2. **测试负载实际访问**这些寄存器，把结果画到 framebuffer 上；访问出错时
   shim 的异常向量会打印 ESR/FAR，从而定位到具体地址。

## 构建

```bash
cd bootshim
./build.sh platform-test
```

编译器自动选择：优先 `clang`（`--target=aarch64-none-elf` + `lld` +
`llvm-objcopy`），否则回退到 `aarch64-linux-gnu-gcc`。也可以显式指定：

```bash
CC=clang ./build.sh platform-test
CC=aarch64-linux-gnu-gcc ./build.sh platform-test
```

产物：

| 文件 | 说明 |
|------|------|
| `out/platform_test.elf` | 负载 ELF，链接在 `0x60000000` |
| `out/platform_test.bin` | 负载原始镜像（`.incbin` 进 shim） |
| `out/shim.elf` | 含负载的 shim ELF |
| `out/Image` | 与 `out/Image.platform-test` 相同的原始镜像 |
| `out/Image.platform-test` | 用于打包 boot.img 的 shim 镜像 |

`./build.sh`（不带参数）仍然生成原来的 wfe 测试内核版本，便于回退。

## 打包 / 刷入

沿用原来的 MTK boot.img 打包方式（需要原始 boot 镜像作模板和本机 DTB）：

```bash
cd bootshim
python mkbootimg_mtk.py /path/to/boot_dump.img \
    out/Image.platform-test /path/to/dandelion.dtb - \
    out/platform-test-boot.img
```

`-` 表示不使用额外 payload（ramdisk 保持空 cpio）。刷入方式与之前相同：

```bash
fastboot flash boot out/platform-test-boot.img
fastboot reboot
```

## 屏幕输出

### 1. shim 阶段：设备 DTB 的平台探测

```
--- PLATFORM (device DTB) ---
GIC  arm,gic-v3 = DANDELION.h
  base    0x000000000C000000
  size    0x0000000000040000
  base    0x000000000C100000
  size    0x0000000000200000
timer Hz 13000000
  intid   29
  intid   30
  intid   27
  intid   26
uart base 0x0000000011002000
  size    0x0000000000001000
wdt base  0x0000000010070000
  size    0x0000000000001000
cpus     8
  mpidr   0x0000000000000000
  ...
enable   psci
```

判读：

- `= DANDELION.h` / 绿色：DTB 与头文件一致。
- `!= DANDELION.h` / 红色：以屏幕上打印的 DTB 值为准，修改
  `DANDELION.h` 后重新构建。
- `probe: DTB parse failed`：解析器拒绝了 DTB，需要检查 shim 的 FDT 解析
  （先用 `tools/test_fdt_probe.c` 在主机上回归）。

### 2. 负载阶段：实际硬件访问

负载会清屏并依次显示：

```
=== DANDELION PLATFORM TEST ===
boot_args 0x...
EL       0x...0000000000000001
MPIDR    0x...
phys_base 0x...
mem_size  0x...

[1] UART
[2] video handoff
[3] watchdog (TOPRGU)
[4] generic timer
[5] GICv3

=== TEST FINISHED (no exception) ===
```

每一步的结果行（绿色为通过）：

- `CNTFRQ` 应为 13000000；`delta` 必须非零。
- `init rc` 必须为 0。
- `ack` 应为 0x3FF（1023，无中断）或 27（定时器 PPI 已挂起，同样是好消息）。
- `ppi en` / `ppi dis` 应为 0。

## 失败时怎么看

### 负载没有打印 `TEST FINISHED`

屏幕被 `EXCEPTION!` 覆盖，说明访问了非法/未映射的物理地址。看：

- `ESR_EL1` 的 EC 字段：
  - `0x24` / `0x25`：数据访问异常（读/写），`FAR_EL1` 就是出错地址。
    常见原因：GICR 基址错误（如误用 MT6761 的 `0x0C080000`）。
  - `0x18`：MSR/MRS 被 EL2 捕获，通常是 `ICC_*_EL1` 访问被 EL2 拦截。
  - `0x00`：未定义指令，可能该核没有实现 GICv3 系统寄存器接口。
- `FAR_EL1` 与 `DANDELION_GIC_BASE` / `DANDELION_GICR_BASE` 对比即可判断
  是哪个基址有问题。

### `init rc` 为负

- `-1`：`ICC_SRE_EL1.SRE` 写不进去（EL3 关闭了系统寄存器接口）。需要
  改用 GIC MMIO CPU 接口，或让 ATF 打开 SRE。
- `-2`：redistributor 一直处于 `ChildrenAsleep`。通常是 GICR 基址或帧
  stride 不对；先看 shim 阶段打印的 DTB `base`。

### shim 阶段就失败

说明问题在 EL2→EL1 之前，与平台专家无关。按原有诊断流程处理。

## 主机侧回归

FDT 探测逻辑可以脱离硬件测试：

```bash
python tools/make_test_dtb.py out/test.dtb
# Windows + Visual Studio:
cl /nologo /Ibootshim tools/test_fdt_probe.c bootshim/fdt.c \
   /Fe:out/test_fdt_probe.exe
out/test_fdt_probe.exe out/test.dtb
```

测试用一棵合成的 MT6765 设备树覆盖 GICv3、generic timer、两个 UART、
watchdog、8 个 CPU 节点，并检查截断输入会被拒绝。
