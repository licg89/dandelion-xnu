# eMMC 日志通道（已打通，2026-10-27）

## 一句话

shim 的日志现在写进 `flashinfo` 分区，回到 Android 用 adb 拉出来就能读，
**不需要拍照、不需要 USB、不需要手机配合**。

已验证（真机）：

```
python tools/read_log_block.py out/fi_log.bin --header
magic         DLXLOG01
version       1
boot          3
text_bytes    5159
note          shim msdc pio
```

正文里包含完整的 GIC inventory、MTK sysirq / sys_timer 寄存器、平台信息、
以及 MSDC 自己的报告 —— 就是以前必须在屏幕上拍下来的那些。

## 完整循环

```powershell
# 1) 打 probe 镜像并刷入（probe = 诊断 + 写日志）
cd D:\aios\dandelion-xnu
python bootshim\patch_boot.py out\boot_backup.img bootshim\out\Image.probe `
    out\stock_boot.dtb out\boot_probe_msdc.img
fastboot flash boot out\boot_probe_msdc.img ; fastboot reboot

# 2) 等它跑完（GIC 探测约 1 分钟 + 120 s 面板保持），然后它会自己回到 fastboot
#    实测：约 4 分钟后 fastboot devices 就能看到设备

# 3) 刷回原厂 boot，进 Android
fastboot flash boot out\boot_backup.img ; fastboot reboot
adb wait-for-device

# 4) 拉分区并解码（日志在 +1 MiB 处）
adb shell su -c "dd if=/dev/block/by-name/flashinfo bs=4096 skip=256 count=256 of=/sdcard/fi_log.bin"
adb pull /sdcard/fi_log.bin out\fi_log.bin
python tools\read_log_block.py out\fi_log.bin --header
```

`skip=256` = 1 MiB（256 × 4096）。`count=256` = 1 MiB，够覆盖 512 KiB 的槽位区。

## 格式

`bootshim/log_block.h`，`tools/read_log_block.py` 解码。

```
block 0      header, 512 B（最后写，magic 缺 = 写入没完成）
block 1..N   文本槽，每槽 64 KiB，按顺序
```

头是显式字节偏移，不是 C 结构体：shim 在 MMU 关时所有访问都是 Device-nGnRnE，
**非对齐的 64 位 store 会触发对齐异常**，packed 结构体会正好制造这种访问
（`cntpct` 在 0x24）。所以用 `put_le32/put_le64` 逐字节拼。

## 关键实现点（bootshim/msdc.c）

| 点 | 做法 | 为什么 |
|---|---|---|
| 控制器状态 | **继承**，不 CMD0/CMD1/重调 | LK 刚用这块 eMMC 加载本镜像：时钟、pad、卡都在 TRAN |
| 时钟 | 只改 `CKDIV=260`（≈385 kHz），`CKMOD` 不动 | 慢时钟下采样点不可能错；CKMOD/BUSWIDTH 必须与卡一致 |
| 总线宽度 | 不写，读 `EXT_CSD[183]` 与 `SDC_CFG` 比对 | 不一致就放弃（会让写落到错线上） |
| 数据通路 | `MSDC_CFG.PIO=1`，FIFO 空时才补 | MediaTek 自家驱动的策略 |
| 主循环等待 | 全部有 deadline（`CNTPCT_EL0`） | shim 死等 = 砖 |
| 写前闸门 | 目标区间必须全 0xff **或** 全 0x00 | 见下 |
| 写完 | 逐字节读回比对 + `CMD6 FLUSH_CACHE`（若 cache 开） | "写成功" 必须等于 "已验证" |

## 实测数据（真机）

```
MSDC0 version 0x20170426   eco 0x0
cfg in   0x2200199   (MODE=1 CKDRVEN=1 CKSTB=1 CKDIV=1 CKMOD=2)
cfg out  0x2210499   (CKDIV=260, CKMOD 保持 2)
sdc in   0x9020000   (BUSWIDTH=2 → 8-bit)
status   0x900       (R1: TRAN + READY_FOR_DATA)
ext csd  rev 8  part 0  bus 6 (8-bit DDR)  hs 1  cache 0
ext sectors 122142720
gpt      flashinfo @ 122109919 + 32768  (与 /sys 实测完全一致)
backup gpt header @ 0x747bfff 读到 EFI PART  →  高 LBA 读取可信
```

## 踩过的坑

1. **`flashinfo + 1 MiB` 不是 0xff，是 0x00。**
   `docs/NIGHT-LOG-1027.md` 里的「+0x10000 之后全 0xff」只看了前 128 KiB。
   现在 `span_is_free()` 接受全 0xff 或全 0x00，并扫描（每 64 KiB 探一个扇区）
   找第一个完整空闲区间，而不是假定固定地址。
2. **`log_block.h` 原来的 C 结构体不能用**：packed 会在 MMU 关时产生非对齐
   64 位访问；不 packed 则和 Python 解码器的 `"<8sIIIIIIIQQQII64s"` 对不上
   （C 会在 8s + 7×u32 后插 4 字节 padding）。改成显式偏移。
3. **每次开机写两遍**：第一遍写下 bring-up 报告，第二遍把「第一遍的结果」
   也写进去，所以拿到的日志是自描述的（含 `commit rc`）。
   第二遍通过头里的 magic 认领同一槽位（`slot reused`），boot 计数不会重复加。

## 内核侧（同日实现，待真机验证）

`dandelion_console_putc()` 现在有三个去处：trace ring、framebuffer、eMMC。

分工：

| 阶段 | 谁做 | 为什么 |
|---|---|---|
| 卡 bring-up、GPT 查找、挑空闲区 | **shim** | 继承 LK 的卡状态只有一次机会，而 shim 是唯一还能在面板上报告失败的阶段 |
| 把 LBA 交给内核 | `struct dandelion_emmc_handoff` @ `0x5FD00000`（`DANDELION.h`） | 两边共用同一个头文件，布局不会漂 |
| 流式写日志 | **内核** `printf.c` | 只保留写的一半：`ml_io_map` + 单块 CMD24 |

要点：

- 内核日志固定放在 `flashinfo + 2 MiB`（`DANDELION_EMMC_KLOG_OFFSET = 4096` 扇区），
  与 shim 的 `+1 MiB` 槽位不重叠
- `dandelion_emmc_enable()` 在 `arm_vm_init()` 的 `pmap_bootstrap()` 之后调用
  —— 这是 `ml_io_map()` 可用的最早时刻。**在那之前打印的所有内容保存在
  16 KiB 的 RAM ring 里**，enable 时一次性补写，所以 `arm_init` / `arm_vm_init`
  的输出不会丢
- 只用单块 CMD24（每报文一个扇区）。多块要维护 CMD12 状态机，而控制台本来就是涓流
- **每写完一行就刷一次**当前扇区（补零重写整个扇区，索引不前进）。代价是每行一次
  ~1 ms 的写，换来「内核死前最后一行一定在介质上」
- 头里的 `text_bytes` 每 8 个扇区刷新一次；内核猝死时它必然滞后，
  所以解码要用 `read_log_block.py --scan`：忽略 `text_bytes`，扫到第一个全 0x00 /
  全 0xFF 扇区为止
- 缓冲区是普通内存、FIFO 是设备 → 每次交给 FIFO 前 `dc civac` + `dsb sy`
