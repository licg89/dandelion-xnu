# artifacts/ — 可刷机的 UEFI 镜像

这里的文件是**这一版真正刷进手机的东西**，特意放进 git，好让一次刷机可以从一个
checkout 完整复现。构建与打包的完整环境见
[`docs/UEFI-BUILD-AND-PACKAGING.md`](../docs/UEFI-BUILD-AND-PACKAGING.md)，
换机交接见 [`docs/HANDOVER-CURRENT.md`](../docs/HANDOVER-CURRENT.md)。

| 文件 | 大小 | 说明 |
|---|---|---|
| `SILICIUM_UEFI.fd` | 2,097,152 B（= `FD_SIZE` 0x200000） | 当前构建产物 |
| `boot-uefi-rw.img` | 16,099,328 B | **当前上机版**：eMMC 读写全开。`BlockIo` 已能读 GPT（`BLOCK OK n=44`）；日志开始写进 `flashinfo`，`adb pull` + `tools/extract_log_block.py` 就能读，不用再拍照 |
| `panel-preview.png` | 720×1600 | `tools/render_panel_preview.py` 渲染的仪表区，**不用刷机就能看布局** |

> 更早的几版（`boot-uefi-inh.img`、`boot-uefi-pv.img`、`boot-uefi-state.img`、
> `boot-uefi-power.img` …）不再跟着仓库一起变大：它们仍在 git 历史里，需要时
> `git log -- artifacts/` 取。仓库里只保留当前版本。

sha256（**每次重建都会变**，只有和同一次构建的产物对得上才有意义）：

```
d49c738c08cf519057f924bbbe17e70cce52f4c857ddf345fd93c39315a5e7dc  SILICIUM_UEFI.fd
c66cd96552821c8bd5e897045788610e8ffdcdd4e49349eb2d4e272308c9ec8f  boot-uefi-rw.img
```

## 这一版是什么（2026-10-29 21:2x，第十四版）——eMMC 读写全开，日志落盘

上一版（`boot-uefi-blockio.img`）的读数（IMG_5765 / IMG_5766）：

```
COUT 0 50x19EMMC OK div=26 sec=0747C000
INH C=02201A99 S=FF027FFC k=1 C'=02210499
INH C13 I=00000100 R=00000900 st=4
INH C8  I=00001100 rev=08 bw=06 hs=01 sec=0747C000
INH B1  I=00001100 S=45464920
INHERIT OK
BLOCK OK n=44
FAT NONE
```

- `EMMC OK div=26` —— 时钟自校准**第一档就通过**（CKDIV=26 ≈ 7.7 MHz，四档里最快）；
- `BLOCK OK n=44` —— **GPT 解析成功**：整盘 media + DiskIo 子句柄 + 各分区，一共 44 个句柄；
- `FAT NONE` —— **是真话**：这台机器的 Android 分区全是 ext4/f2fs，没有 FAT 卷可挂。
  M4 因此按「eMMC → BlockIo → 分区表」判完成；FAT 只在这块盘上确实存在 FAT 卷时才有意义。

这一版：

- `Media->ReadOnly = FALSE`，写路径打开。唯一的写入者是 `PayloadBootDxe` 的日志路径，
  它只写"空的或它自己上一次写的"区间，写完逐字节读回校验；
- `SpanIsOurs()`：`flashinfo + 1 MiB` 那块被 line A 在 2026-10-27 写的 `DLXLOG01` 日志占着，
  `DDNLOG01` 与 `DLXLOG01` 两种 magic 都认，于是日志通道可以反复使用；
- `BLOCK OK n=..` 带句柄计数，把"分区没解析出来"和"这块盘确实没有 FAT"分开。

**读日志的循环**（不再需要拍照）：

```powershell
fastboot flash boot D:\aios\artifacts\boot_backup.img
fastboot reboot
adb wait-for-device
adb shell su -c "dd if=/dev/block/by-name/flashinfo of=/sdcard/fi.img"
adb pull /sdcard/fi.img out\fi.img
python tools\extract_log_block.py out\fi.img --out out\last-boot.log
```

## 这一版是什么（2026-10-29 21:0x，第十三版）——M4 数据通路打通

上一版（`boot-uefi-inh.img`）的读数（IMG_5762）把 eMMC 数据相位解决了：

```
COUT 0 50x19INH C=02200199 S=09020000 k=1 C'=02210499
INH C13 I=00000100 R=00000900 st=4
INH C8  I=00001100 rev=08 bw=06 hs=01 sec=0747C000
INH B1  I=00001100 S=45464920
INHERIT OK
```

`I=00001100` = CMDRDY + **XFER_COMPL**；`S=45464920` = LBA 1 的 `"EFI "`。
`bw=06` = `EXT_CSD[183]` **8 位 DDR**，`hs=01` = high speed，`sec=0747C000` = 122,159,104 扇区
（62.5 GB）。所以真正的错配一直是「主机把自己设成 1 位、卡还在 8 位 DDR」＋「复位控制器、
重新初始化卡」—— 而 `bootshim/msdc.c` 早就在同一台手机上用「继承」策略读写成功过。

这一版把那条路径变成驱动：新增 `Platforms/Xiaomi/dandelionPkg/Drivers/DandelionEmmcDxe/`，
继承 `MSDC0`、**时钟自校准**（CKDIV 26/52/104/260 依次试，读 LBA 1 校验签名）、
发布 `EFI_BLOCK_IO_PROTOCOL`（**这一版只读**，写路径实现了但没打开）。

完整判读见 [`docs/ANALYSIS-INHERIT-VS-REINIT.md`](../docs/ANALYSIS-INHERIT-VS-REINIT.md) §8，
打包踩过的坑见 [`docs/CORRECTIONS.md`](../docs/CORRECTIONS.md) C16。

## 这一版是什么（2026-10-29 20:3x，第十二版）

上一版（`boot-uefi-pv.img`）的读数（IMG_5760）回答了两件事，一件是误读，一件是真答案。

**误读**：`PV pre=00000900 a=00000900 b0=00000000 c=00400900 STALE`。判据选错了命令 ——
`CMD10`（SEND_CID）只在 **stby** 态合法，TRAN 态下卡回的是 48 位错误响应而主机在等 136 位，
所以 `b0=0`；`ILLEGAL_COMMAND` 锁存后由下一条 CMD13 报出来，所以 `c` 多了 bit22。
`b0 != a` 已经证明 `SDC_RESP0` **是活的**。反证：上一版没有 `PV` 这一行，同一位置的
`c6` 是 `00000900`；这一版 `PV` 先发了一次非法 CMD10，`c6` 就变成 `00400800`（同一个 bit22）。

**真答案**：`A8/B8/C8/D8` 四轮逐字相同，说明位宽/时序不是变量；`k≈15000/30000` 说明时钟
一直在跑。而 **line A 的 `bootshim/msdc.c` 早就在同一台手机上读写过 eMMC**
（[`docs/EMMC-LOG.md`](../docs/EMMC-LOG.md)：写入 5159 字节并读回校验通过），它的策略是
「继承 LK 留下的控制器与卡状态，**绝不重新初始化**」，而 UEFI 侧做的正是它的反面。

这一版改动：`MsdcDxe` 从 `Include/DXE.inc` + `Include/APRIORI.inc` 整条移除；
`StorageBeaconDxe` Depex 改 `TRUE`；新增 `InheritProbe()`；四轮阶梯缩成单条 `A8`。
完整判读见 [`docs/ANALYSIS-INHERIT-VS-REINIT.md`](../docs/ANALYSIS-INHERIT-VS-REINIT.md)，
更正见 [`docs/CORRECTIONS.md`](../docs/CORRECTIONS.md) C15。

## 这一版是什么（2026-10-29 深夜，第十一版）

上一版（`boot-uefi-clock.img`）的读数（IMG_5759）把三条主机侧假设全否了：

```
RST C=0200F09B                    软复位干净，回到同一配置
A8 I=0100 R=00000900 ps=EF/FF gp=DF8/DFF k=14994 c6=00000000
B8 I=0100 R=00000900 ps=EF/FF gp=DF8/DFF k=15061 c6=00000900
C8 I=0100 R=00000900 ps=EF/FF gp=DF8/DFF k=14933 c6=00000900
D8 I=0100 R=00000900 ps=EF/FF gp=DF8/DFF k=1504x c6=00000000
B1 -  no data phase
```

- 软复位没用；`CKPDN` 清零没用；
- `k≈15000/30000` ⇒ **时钟整个等待窗口都在跑**（“没时钟”也死了）；
- `c6=00000900` ⇒ CMD6 切换被接受、无 SWITCH_ERROR；`R=00000900` ⇒ 卡在
  TRAN + READY_FOR_DATA、无错误位。

**卡一直很好，就是不驱动 DAT。** 所以这版只问一个问题：**卡自己认为传输开始了没有？**
数据命令失败后立刻补一条 `CMD13`：

| 字段 | 含义 |
|---|---|
| `t=` | CURRENT_STATE：**4=TRAN**（卡没开始传）／**5=DATA**（开始了、正在被时钟推数据，丢在主机侧） |
| `r=` | READY_FOR_DATA |

这一个数字决定下一步：

- `t=5` ⇒ 主机接收路径（pad 输入使能 / OE）→ 下一步用 `AddMemorySpace` 映射
  `0x10002000` 读/修 pad；
- `t=4` ⇒ 卡确实没开始 → 转卡侧（`EXT_CSD` 可达性 / 分区访问 / 永久锁定）。
- `t=0` ⇒ CMD13 都没发出去，说明控制器还卡在数据相位（SDCBUSY），即它确实在等数据。

### 怎么看屏幕

```
PWR ... BUS OK
RST C=0200F09B
H0 C=.. S=.. P=.. N=..
IO dir=000 do=000 din=........
C0 I=.. cl=.. f=.. l=.. kc=..
C1 n=.. I=.. R=........
C2 3=.. 2=.. 1=.. 0=..
C3 I=.. R=.. C7 I=.. R=..
C13 I=.. R=.. st=..
A8 I=.. R=.. sz=.. ps=../.. gp=.../... f=.. k=..... c6=........ t=. r=.
B8 / C8 / D8 ...（前一行失败才出现）
B1 I=.. S=..
```

逐条判读见 `docs/POWER-BEFORE-AFTER.md` §3。

**再看底部格子**：三排 8 个 80×80（y 1400 / 1448 / 1496，间距 88，左边距 8）。
**格子存在 = 那个阶段到达过**，所以一张照片就是完整状态向量：

| 格子 | 颜色 | 含义 |
|---|---|---|
| 0 | 黄 | `MsdcDxe` 库构造函数跑到了 |
| 1 | 橙（红=失败） | GPIO122..133 复用设置完成 |
| 2 | 青（红=失败） | MSDC 时钟使能完成 |
| 3 | 白（红=缺协议） | power / PMIC 路径走完 |
| 4 | 紫 | 产生了 BlockIo（`StorageBeaconDxe`）← **M4 达成** |
| 5 | 绿 | 挂上了文件系统（FAT） |
| 6 | 黄 | `StorageBeaconDxe` 起来了（拿到了 pass-thru） |
| 7 | 红 | 一个块设备都没有 |
| 8 | 绿 | msdc0 两段 MMIO 区间都解析成功 |
| 9 | 黄 | 控制器复位完成（`MsdcReset` 返回了） |
| 10 | 品红 | 识别阶段真的有一条命令上了总线 |
| 11 | 青 | CMD1 有应答（OCR bit31 置位 = 卡上电了） |
| 12 | 白 | CMD2+CMD3 过，`CardType = EmmcCard`（EmmcDxe 能绑上） |
| 13 | 绿/红 | `CardSetBusMode`（CMD7 选择 + 总线时钟） |
| 14 | 青 | 走到了 pass-thru 安装 |
| 15 | 红 | 卡检测失败（host 1 = SD 槽，红是正常的） |
| 16 | 黄 | 有人调 `BuildDevicePath`（= EmmcDxe 来看我们了） |
| 17 | 青 | 识别之后又来了一条 pass-thru 命令 |
| 18/19/20/21 | 黄/青/橙/白 | 驱动模型侧发的 CMD9 / CMD7 / CMD10 / CMD8 |
| 22 | **绿/红** | **512 字节数据读（M4 的关键格子）** |
| 23 | 红 | 识别之后有命令报错 |

> **16..23 归 `MsdcDxe`/`EmmcDxe`。** 它们比 `StorageBeaconDxe` 先跑，所以任何"后来者"
> 往这些格子上画都会**擦掉 M4 的证据**——上一版就踩过（20/21 被当成探针开始/结束）。

另外：y 1100/1140 是 `StorageBeaconDxe` 的横幅，y 1172/1186/1200 是供电位图条，
y 1216 是块设备数（蓝，x 8）与文件系统数（绿，x 380）的点阵；
y 1560 是 `PlatformProgressDxe` 的白条。

> 文字区只有 y 0..1087（68 行 × 90 列）。**不要**把任何证据画在 y ≥ 1088 之外又指望
> 它出现在文字区里；反过来，文字区里出现的任何东西都是当前内容（C11 之后）。

boot image 的 header：

```
magic=ANDROID!  hver=2  page=2048
kaddr=0x40080000  ksize=13907597     <- 只改了这一项
raddr=0x51B00000  rsize=2060079  tags=0x47880000
kernel 段 = gzip(SILICIUM_UEFI.fd) 608,256 B（FD 2,097,152 B）
```

`kaddr` 必须等于 `[uefi_fd].base`（`Resources/Configs/dandelion.toml` = `0x40080000`）。
**`0x48880000` 是错的**：LK 只在 `0x40080000` 执行 kernel 段，见
`docs/CORRECTIONS.md` C8。

## 刷机与恢复

```powershell
adb reboot bootloader
fastboot flash boot D:\aios\dandelion-xnu\artifacts\boot-uefi-state.img
fastboot reboot
```

恢复（刷完卡住 / 黑屏 / 重启循环）：长按电源键 10 s 强制关机，然后 电源 + 音量下 进 fastboot，

```powershell
fastboot flash boot D:\aios\artifacts\boot_backup.img
fastboot reboot
```

> `boot_backup.img`（64 MiB 原厂 boot 分区 dump）**没有**进 git：太大，而且它是
> 本机设备的原厂内容。它留在 `D:\aios\artifacts\`，打包脚本的 `--stock` 默认值
> 就指向它。

## 怎么重新生成

```bash
# 1. 构建（WSL，本地 EDK2 树；也可以直接等 CI 的 dandelion-fd artifact）
#    树必须在 WSL 的 ext4 上，不要放 /mnt/d（9p 会卡死在 p9_client_rpc）
wsl -d Ubuntu -e bash /mnt/d/aios/dandelion-xnu/tools/build-local.sh
#    → 自动把 FD 拷到 D:\aios\artifacts\SILICIUM_UEFI.fd

# 2. 打包 + 校验（读 toml 里的 base，自动断言 kernel_addr）
cd /mnt/d/aios/dandelion-xnu
python3 tools/package_uefi_bootimg.py --out artifacts/boot-uefi-state.img
```
