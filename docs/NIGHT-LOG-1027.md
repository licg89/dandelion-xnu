# 2026-10-27 夜班日志（03:00–03:30）


> **编码说明（2026-10-29 补）**：这份日志最初是 UTF-8 与 GBK 混着写的，已经无法无损解码；这里按 UTF-8 重写了一遍，个别字符是替换符。内容以
> [RESUME-1029.md](RESUME-1029.md) 与 [HANDOVER-CURRENT.md](HANDOVER-CURRENT.md) 为准。

手机状态：已刷回 Android（boot_backup.img），adb 在线，循环已停。用户睡觉中，明早需要：
手机可用 + 仓库里有一切结论。

## 本节新增事实（真机读取）

| 项 | 值 | 来源 |
|---|---|---|
| flashinfo 分区 | `mmcblk0p43` | `/dev/block/by-name/flashinfo` |
| flashinfo 起始 LBA | **122109919**（512B 扇区） | `/sys/class/block/mmcblk0p43/start` |
| flashinfo 大小 | 32768 扇区 = 16 MiB | 同上 |
| 第一个 64 KiB | **有数据**（`DOWNLOAD INFORMATION!!` + 0xfdff 填充） | dd 读取 |
| +0x10000 之后 | 全 0xff 填充（未用，安全） | dd 读取 |

→ MSDC 日志落点定案：**头 = flashinfo LBA + 2048 扇区**（即 +1 MiB 处），正文随后按 512B 块写。

## USB 第二次实验结果

修复四项（EP0 CSR0 位定义、EP0 用 CSR0/COUNT0 读状态、SERVICEDSETUP、EP1 TXFIFOSZ/TXFIFOADD）
+ PHY 基址改 0x11CC0800 之后刷机实测：**主机 58 秒内仍无任何新 USB 设备**。
说明还有缺失（候选：VUSB33 电没开、PERICFG 复位时序、或 shim 根本没跑到 USB 代码）。
已派 subagent 逐行对照 LK 序列审计 `usb_cdc.c`。

## 正在并行的四个 subagent

1. UniXNU 进度方法论（用户点名要看的 https://unixnu.com/zh/devices/ ）
2. MUSB 枚举失败根因审计（对照 lk-gic-usb.md §5 的逐条序列）
3. **MSDC PIO 写日志实现**（写进 bootshim，含构建验证）
4. LK 引导模式/USB/MSDC 交接状态（recovery 分区怎么选、MSDC 是否留在 RCA=1 传输态）

## 下一步（等 subagent 回来）

- 把 MSDC 写入器接进 probe/usb 模式，用今晚的 LBA 数据打包
- 刷机验证循环变成：刷 → 跑 → 进 Android → `adb pull` flashinfo → 解码，
  **全程不需要拍照，也不需要手机侧 USB**

## ������2026-10-27 ���죩

���ļ��+0x10000 ֮��ȫ 0xff ��䡹��**����**���������ɨ������ 15 MiB ��ȷ�ϣ�
flashinfo + 1 MiB ���� **0x00**��ȫ�㣩������ 0xff�������Ѹ�Ϊ���ֿ�̬�����ܡ�
��� docs/EMMC-LOG.md��

