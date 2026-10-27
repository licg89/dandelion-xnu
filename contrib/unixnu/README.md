# contrib/unixnu — 交给 UniXNU 移植使用的东西

这里放的是本仓库为 **`licg89/UniXNU-dandelion`** 准备的、可直接复用的部件。

| 文件 | 用途 |
|---|---|
| `patches/xnu/native-emmc-serial.patch` | 轮询 PIO 的 eMMC 控制台（XNU 侧），替代 UART |
| `contrib/unixnu/scripts/read-emmc-log.py` | 主机侧解码器 |

> 补丁本身放在 `patches/xnu/`（而不是这里），这样本仓库的 CI 会一直编译验证它；
> 对本移植而言它是惰性的 —— 只有当 ADT 里有 `emmc-log` 节点时才会注册。

## 为什么

`UniXNU-dandelion` 的 README 把唯一的阻塞写成：

> **Key Challenge**: No UART access makes debugging impossible. We've tried the
> direct boot approach but kernels crash immediately with no way to see why.

这个补丁把内核控制台写到 eMMC 的保留区，崩溃/复位后还能用 `adb` 取回来解码。

## 怎么接

完整说明见 `docs/UNIXNU-EMMC-CONSOLE.md`。要点：

1. **补丁形状**：UniXNU 已有串口 vtable（`pexpert/arm/pe_serial.c`），
   `native-emmc-serial` 是同一个 vtable 的另一个实现，`transmit_data()` 就是写字符。
   `kprintf` 不需要改。
2. **ADT 节点**（由设备仓库的 ADT 生成器写入）：

   | 属性 | 值 |
   |---|---|
   | `name` | `"emmc-log"` |
   | `reg` | `0x11230000`, `0x10000` |
   | `log-lba` | `GPT(flashinfo) + 4096`（+2 MiB） |
   | `log-sectors` | `1024` |
   | `clock-div` | `260` |

3. **开关**：`CONFIG_XNU_EMMC_SERIAL=y`（本仓库在 `DANDELION.h` 里直接
   `#define XNU_EMMC_SERIAL 1`）。
4. **取日志**：

   ```bash
   adb shell su -c "dd if=/dev/block/by-name/flashinfo bs=4096 skip=512 count=128" > dump.bin
   python3 scripts/read-emmc-log.py dump.bin --scan
   ```

## 状态

- 写路径、日志格式、失败恢复（CMD12 + 整卡重初始化）、主机解码器：
  **已在真机验证**
- 作为 UniXNU 补丁的形态（ADT 驱动、`ml_io_map`、串口 vtable）：**已生成并通过
  `git apply` 校验，等待在 UniXNU 的 macOS 构建里编译验证**
