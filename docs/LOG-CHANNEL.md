# 内核日志通道：从 eMMC 读回，不再靠拍照

## 一句话

内核的 `printf()` 输出会落到 `flashinfo` 分区 **sector 30000** 的 `DLXLOG01` 块（512 扇区 = 256 KiB）。
进一次 Android，用 `adb` 把整块分区 `dd` 出来，在主机上解码即可——**一次拿到 3000+ 行**，
而不是面板上那 45 行。2026-10-30 晚实测：单次启动解出 **3412 行 / 154 KiB**。

## 位置

| 块 | 位置（相对 flashinfo） | 大小 | 写入者 | magic |
|---|---|---|---|---|
| 内核日志 | +30000 扇区（字节 15360000） | 512 扇区 = 256 KiB | XNU（`patches/xnu/native-emmc-serial.patch`） | `DLXLOG01` |
| 固件日志 | +30512 扇区 | 128 扇区 | `PayloadBootDxe` | `DDNLOG01` |
| 旧固件日志 | +32512 扇区 | 128 扇区 | 旧布局残留 | `DDNLOG01` |

`flashinfo` = `mmcblk0p43`，起始 LBA 122109919，共 16 MiB。前 30000 扇区是 FAT（ESP），
日志区在它之后，所以刷 ESP 不会覆盖日志。

## 完整循环（实测可用）

```powershell
cd D:\aios\dandelion-xnu

# 1) 换回原厂 boot，进 Android（我们的链镜像记在 artifacts\boot-uefi-redband.img）
fastboot flash boot out\boot_backup.img
fastboot reboot
adb wait-for-device

# 2) 拉整个 flashinfo（16 MiB，约 1 秒）
adb shell "su -c 'dd if=/dev/block/by-name/flashinfo of=/data/local/tmp/fi.img bs=1M'"
adb pull /data/local/tmp/fi.img out\fi.img

# 3) 切出内核日志块并解码（DLXLOG01 槽位格式）
py -3.13 -c "d=open(r'out/fi.img','rb').read(); open(r'out/klog.bin','wb').write(d[15360000:15360000+512*512])"
py -3.13 tools\read_log_block.py out\klog.bin --header --scan > out\kernel-boot.log

# 4) 固件日志（可选）
py -3.13 tools\extract_log_block.py out\fi.img --lba 30512 --out out\fw-boot.log

# 5) 刷回我们的链
fastboot flash boot artifacts\boot-uefi-redband.img
fastboot flash flashinfo out\unixnu-esp.img    # 需要时
fastboot reboot
```

## 两个解码工具的分工（别用错）

| 工具 | 认的 magic | 关键参数 | 用途 |
|---|---|---|---|
| `tools/read_log_block.py` | `DLXLOG01`（内核） | 输入必须是**从块头开始**的切片；`--scan` 忽略 `text_bytes`，遇到第一个全空扇区就停（内核崩了 header 不会再刷新） | 内核日志 |
| `tools/extract_log_block.py` | `DDNLOG01`（固件） | 必须给 `--lba 30512`；否则可能选到 32512 的旧块 | 固件日志 |

## 踩过的坑

1. **PowerShell 的 `>` 会把文件写成 UTF-16**，补丁和二进制切片都会坏。用 Python 写二进制，或 `Out-File -Encoding utf8`。
2. `adb shell su -c "dd ..."` 的嵌套引号会被拆坏，`dd` 会读到 0 字节。正确写法：`adb shell "su -c 'dd ...'"`。
3. 写 `/sdcard` 在 Magisk 命名空间下可能不可见；写 `/data/local/tmp` 稳定。
4. `tools/ci/fetch_kernel.ps1` 依赖 `Invoke-WebRequest`，在某些网络/代理下会挂住（`-NoWait` 也挂）。
   备用：`tools/ci/waitfetch.py`（`curl.exe` + `git credential fill`，实测稳定）。
5. 内核块 `text_bytes` 可能是过期值（内核死后不再刷新 header），**一定加 `--scan`**。

## 为什么日志里看不到 `kprintf()` 的行

`pexpert/arm/pe_kprintf.c`：

```c
if (debug_boot_arg & DB_KPRT)          /* 0x8 */
        disable_serial_output = false; /* 默认 true */
#if DEBUG
        disable_kprintf_output = false;
#elif DEVELOPMENT
        if (PE_parse_boot_argn("-enable_kprintf_spam", ...))
                disable_kprintf_output = false; /* 默认 true */
#endif
```

启动参数是 `-v keepsyms=1 debug=0x144`，`0x144 = 0x100|0x40|0x4`，**不含 `0x8`**；
而这是 DEVELOPMENT 构建 → `kprintf()` 被整体丢弃，`printf()` 仍然走控制台。

要让 `kprintf()` 出声：把参数改成 `debug=0x14c -enable_kprintf_spam`。
**但**输出量会大幅增加，256 KiB 的日志区可能被写满后停止（丢掉的正是尾部），
所以 bring-up 阶段更推荐：诊断语句直接用 `printf()`，或者用 `dandelion_trace_mark()`
（这两条路都实测可见）。
