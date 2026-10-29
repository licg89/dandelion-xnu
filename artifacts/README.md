# artifacts/ — 可刷机的 UEFI 镜像

这里的文件是**这一版真正刷进手机的东西**，特意放进 git，好让一次刷机可以从一个
checkout 完整复现。构建与打包的完整环境见
[`docs/UEFI-BUILD-AND-PACKAGING.md`](../docs/UEFI-BUILD-AND-PACKAGING.md)，
换机交接见 [`docs/HANDOVER-CURRENT.md`](../docs/HANDOVER-CURRENT.md)。

| 文件 | 大小 | 说明 |
|---|---|---|
| `SILICIUM_UEFI.fd` | 2,097,152 B（= `FD_SIZE` 0x200000） | 当前构建产物 |
| `boot-uefi-m5-dma.img` | 16,099,328 B | **当前上机版**：M4 卡状态修复 + DMA 读路径 + M5 装载器 + 设备树 + 完整日志（含写 flash） |
| `panel-preview.png` | 720×1600 | `tools/render_panel_preview.py` 渲染的仪表区，**不用刷机就能看布局** |

> 更早的几版（`boot-uefi-pagelog-pio2.img`、`boot-uefi-stagecells.img`、`boot-uefi-console.img`）
> 不再跟着仓库一起变大：它们仍在 git 历史里，需要时 `git log -- artifacts/` 取。
> 仓库里只保留当前版本。

sha256（**每次重建都会变**，只有和同一次构建的产物对得上才有意义）：

```
8fa1919b89fada7a70db72a5949633c34aec825cab8cdc9f65ad8fcbf984cb35  SILICIUM_UEFI.fd
58d5e5a3f19fb628039623397dd5027e45052365c1f423b3a45deb2c04409282  boot-uefi-m5-dma.img
```

## 这一版是什么（2026-10-29 02:2x）

**先修"能不能读到日志"，再修数据通路。** 两件事分开做：

1. **控制台不再滚动**（`DandelionConsoleDxe.c`）。旧 `ScrollUp()` 用 `CopyMem` 把
   framebuffer 自己当源，而这块屏的 framebuffer **读回不可靠** → 每滚一次就多一层
   错位重影，照片上半屏永远是一团乱码（`docs/CORRECTIONS.md` C11）。现在写满 68 行
   就回到第 0 行覆盖最旧的一行，行首第一次写入前整行擦除。
   **面板上出现的每一个字都是当前内容，不会再有重影。**
2. **PIO 数据读重写**（`Resources/MuPatches/0002`）。旧 `MsdcPioRead()` 要 FIFO 攒够
   128 字节才排一次（硬件阈值 64）→ 512 字节的 ext-CSD 读一个字节都读不出来；
   另外每条命令前没有清 latched `MSDC_INT`，上一条命令留下的 `DATCRCERR` 会被下一次
   数据读当成"立刻出错"。参照 `Platforms/Benesse/a04br3Pkg/Drivers/SdCardBlockIoDxe`
   （本树里唯一在真机 MT6765 上跑通过数据传输的实现）重写。
3. 失败时面板上会打出证据（两行，不会再被 90 列折行）：
   `ddn pio-read FAIL int=… left=…` / `ddn  cfg=… fifo=… sts=… dcrc=…`；
   成功时只打前两次 `ddn pio-read ok len=… fifo=…`。

### 怎么看屏幕（底部格子）

底部有 **三排 8 个 80×80 的格子**（y 1400 / 1448 / 1496，间距 88，左边距 8）。
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
| 22 | **绿/红** | **512 字节 PIO 数据读（M4 的关键格子）** |
| 23 | 红 | 识别之后有命令报错 |

另外：y 1100/1140 是 `StorageBeaconDxe` 的横幅，y 1190 是块设备/文件系统计数点；
y 1560 是 `PlatformProgressDxe` 的白条。

> 文字区只有 y 0..1087（68 行 × 90 列）。**不要**把任何证据画在 y ≥ 1088 之外又指望
> 它出现在文字区里；反过来，文字区里出现的任何东西都是当前内容（C11 之后）。

boot image 的 header：

```
magic=ANDROID!  hver=2  page=2048
kaddr=0x40080000  ksize=13907597     <- 只改了这一项
raddr=0x51B00000  rsize=2060079  tags=0x47880000
kernel 段 = gzip(SILICIUM_UEFI.fd) 570,880 B
```

`kaddr` 必须等于 `[uefi_fd].base`（`Resources/Configs/dandelion.toml` = `0x40080000`）。
**`0x48880000` 是错的**：LK 只在 `0x40080000` 执行 kernel 段，见
`docs/CORRECTIONS.md` C8。

## 刷机与恢复

```powershell
adb reboot bootloader
fastboot flash boot D:\aios\dandelion-xnu\artifacts\boot-uefi-pagelog-pio2.img
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
python3 tools/package_uefi_bootimg.py --out artifacts/boot-uefi-pagelog-pio2.img
```
