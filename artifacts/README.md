# artifacts/ — 可刷机的 UEFI 镜像

这里的文件是**这一版真正刷进手机的东西**，特意放进 git，好让一次刷机可以从一个
checkout 完整复现。构建与打包的完整环境见
[`docs/UEFI-BUILD-AND-PACKAGING.md`](../docs/UEFI-BUILD-AND-PACKAGING.md)，
换机交接见 [`docs/HANDOVER-CURRENT.md`](../docs/HANDOVER-CURRENT.md)。

| 文件 | 大小 | 说明 |
|---|---|---|
| `SILICIUM_UEFI.fd` | 2,097,152 B（= `FD_SIZE` 0x200000） | 当前构建产物 |
| `boot-uefi-blockio.img` | 16,099,328 B | **当前上机版**：`CMD0` → `CMD1`(到 OCR bit31) → `CMD2/3/7/13` 走到 TRAN，然后 `CMD16/CMD8/CMD17` **真读 512 字节**（EXT_CSD + LBA 1）；整段日志压到 10 行 |
| `panel-preview.png` | 720×1600 | `tools/render_panel_preview.py` 渲染的仪表区，**不用刷机就能看布局** |

> 更早的几版（`boot-uefi-ping.img`、`boot-uefi-power.img`、`boot-uefi-m5-probe.img` …）
> 不再跟着仓库一起变大：它们仍在 git 历史里，需要时 `git log -- artifacts/` 取。
> 仓库里只保留当前版本。

sha256（**每次重建都会变**，只有和同一次构建的产物对得上才有意义）：

```
29754419dfd99d4f9c84ec9445d6764fef471114d123b807d4a38c7be6cddeb5  SILICIUM_UEFI.fd
91d210971a374bac35d6dde93cf572994e434e0c23332347dfbed859a68e09a7  boot-uefi-blockio.img
```

## 这一版是什么（2026-10-29 深夜，第五版）

上一版（`boot-uefi-init.img`）把卡走到了 `CMD1` 有应答，读数（IMG_5750）：

```
C0     I=00000100 cl=97  f=297 l=402 kc=14767
PING a I=00000100 R=00FF8080 cl=125 f=15 l=222 kc=15187
C1 n=1 I=00000100 R=C0FF8080
```

- `C1 n=1`：第一轮 `CMD1` 的 OCR 就带 **bit31=1**（上电完成）、bit30=1（扇区模式）；
- **`C1` 之后没有字，不是失败**：文字区 68 行，`DandelionConsoleDxe` 的自检把光标留在
  第 53 行，本驱动只剩 15 行预算；上一版要吃 21 行，`C2..C13` 被画到最后一行之外。

本版两件事：

1. **日志压到 10 行**（供电一整行、去掉 H1/CMD12/PING），整段固定落在
   y 848..1008，绝不会被挤出屏幕；
2. **真的读数据**：`C13 st=4` 之后跑 `CMD16`(SET_BLOCKLEN 512)、
   `CMD8`(SEND_EXT_CSD，512 字节 PIO 读)、`CMD17`(LBA 1，即 GPT 头)。

### 怎么看屏幕

**文字区是主证据**：

```
PWR PS=81FF0002 DAT=FF CMD=1 CD=1 VEMC=1 vsel=3 3000mV w=1 f=0 BUS OK
H0 C=0200F09B E=00000000 I=00000000 R=00000B00
H0 S=FF007FFC P=81FF0002 N=00000001
C0 I=.. cl=.. f=.. l=.. kc=..
C1 n=.. I=.. R=........
C2 3=.. 2=.. 1=.. 0=..
C3 I=.. R=.. C7 I=.. R=..
C13 I=.. R=.. st=..
C8 I=.. sz=.. rv=.. bw=.. hs=.. c16=....
B1 I=.. S=..
```

| 看到 | 结论 |
|---|---|
| `C13 st=4` + `C8 I=...0100` + `B1 S=45464920` | **整条存储通路打通**（命令/长响应/512 字节数据读/扇区寻址），剩下的纯粹是驱动 |
| `C8 I=...4000`（DATTMO） | 命令通了、数据相位没起来 ⇒ 查 `SDC_CMD` 的 blklen/DTYP、`MSDC_CFG.PIO` |
| `C8 I=...8000`（DATCRCERR） | 数据在线上但采样窗口不对 ⇒ `MSDC_PAD_TUNE0` / `EMMC50_CFG0` |
| `C8 sz=00000000` | EXT_CSD 全零 ⇒ 读到的不是 EXT_CSD |
| `B1 S≠45464920` | 能读但内容不对 ⇒ 扇区模式 / LBA 偏移 / 分区表位置 |
| `NOT IN TRAN` | `C13` 状态不是 4 ⇒ 回看 `C2`/`C3`/`C7` |
| `CARD NOT READY` | 卡一直 busy ⇒ OCR 窗口 / VEMC / 重试次数 |
| 某行 `I=00000200` | 那条命令超时，看它上一行 |

三条位图条（y 1172 / 1186 / 1200）现在是采样版 `CMD0` 的 `PS` / AND / OR；
x 400..560 的方块是供电判读结论。逐条判读见 `docs/POWER-BEFORE-AFTER.md` §3。

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
kernel 段 = gzip(SILICIUM_UEFI.fd) 607,232 B（FD 2,097,152 B）
```

`kaddr` 必须等于 `[uefi_fd].base`（`Resources/Configs/dandelion.toml` = `0x40080000`）。
**`0x48880000` 是错的**：LK 只在 `0x40080000` 执行 kernel 段，见
`docs/CORRECTIONS.md` C8。

## 刷机与恢复

```powershell
adb reboot bootloader
fastboot flash boot D:\aios\dandelion-xnu\artifacts\boot-uefi-blockio.img
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
python3 tools/package_uefi_bootimg.py --out artifacts/boot-uefi-blockio.img
```
