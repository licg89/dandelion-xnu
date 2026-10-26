# USB 串口日志通道（进行中）

目标：让 shim（以及之后的内核）把日志从手机 USB 口实时吐出来，替代"拍照读屏"。

## 硬件事实（2026-10-26 真机实测）

| 项 | 值 | 来源 |
|---|---|---|
| USB 控制器 | `mediatek,mt6765-usb20`（Mentor MUSB 核），寄存器块 `0x11200000`，第二段 `0x11CC0000` | 设备 DTB |
| 中断 | GIC SPI 73 | 设备 DTB |
| MUSB `POWER` 写入 `SOFTCONN|HSEN` | 回读 `0x60` ✅ 控制器活着、可写 | `boot-usb3/4` 面板 |
| MUSB `DEVCTL` | `0x00`（VBUS 状态 = 0） | 同上 |
| MUSB `INTRUSB` | `0x00`，从未出现 reset/connect | 同上 |
| 主机侧 | 刷入后 20 秒内**没有任何新 USB 设备** | `Get-PnpDevice` |
| 显式 session 请求（`DEVCTL|=1`） | 无效，仍无枚举 | `boot-usb4` |

**结论**：LK 在交接前关闭了 USB PHY（和/或 USB 时钟门控），fastboot 用完即断电。
"复用 LK 已初始化的 USB"这个假设不成立，必须自己把 PHY 和时钟拉起来。

`fastboot boot` 在 MTK LK 上不可用（下载 `OKAY` 后 `Booting FAILED (Status read failed)`，
随机重启回原分区），所以每次试验都要 `fastboot flash boot` + 手动进 fastboot。

## 代码现状

- `bootshim/usb_cdc.c`：已重写为完整的 MUSB 设备模式实现
  - EP0 控制传输（GET_DESCRIPTOR / SET_ADDRESS / SET_CONFIGURATION / CDC 类请求）
  - CDC-ACM 描述符（device/config/string）
  - EP1 IN 作为日志输出端点
  - `usb_poll()` 轮询 INTRUSB/EP0 FIFO，`usb_cdc_putc()` 推字符
  - `usb_cdc_dump()` 两行寄存器摘要（`P/D/I/W` + 两个候选 PHY 地址）
- `bootshim/fb.c`：`-DUSB_CONSOLE` 时 `fb_putc()` 同时镜像到 USB
- `bootshim/build.sh`：新增 `usb` 构建模式；`probe` 模式（只跑探针后停在面板）
- `tools/qemu/gen_font.py` + `font8x16.py`：从 `printf.c` 抽取字库，供 QEMU framebuffer 解码

## 下一步（按顺序）

1. **读 PHY 状态**：刷 `boot-usb4.img`，看 `usb phy cc=` / `1128=` 两行值，
   确认哪个基址有响应、PHY 是否在掉电态。
2. **移植 MTK u2 PHY 初始化**：
   - 参考 Linux `drivers/phy/mediatek/phy-mtk-tphy.c` 的 u2 部分（上电、PLL、Tx/Rx 使能）
   - 时钟门控：`topckgen`/`infra` 里 USB0 相关 gate（LK 关闭）
   - 每步都往面板打点，直到 `DEVCTL` 的 VBUS 位非 0
3. **枚举**：PHY 起来后 `POWER.SOFTCONN` 应能让主机看到设备；
   若 Windows 不自动装 `usbser`，把 VID/PID 改成已知 CDC 设备或手动指定驱动
4. **内核阶段**：枚举成功后，内核只需在 `dandelion_console_putc()` 里把字符写进
   EP1 FIFO（轮询 `TXCSR.TXPKTRDY`），约 30 行补丁，即可实时拿到 `kprintf` 全文

## 备选方案（若 PHY 迟迟不通）

Android + `/dev/mem` 后验读取：shim 把日志写进设备树里的保留内存，
刷回 Android 后用 `adb shell su -c dd if=/dev/mem` 读出来，主机脚本解码。
需要先验证 `/dev/mem` 可读（root 且非 STRICT_DEVMEM）。
