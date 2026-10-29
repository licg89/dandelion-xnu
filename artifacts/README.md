# artifacts/ — 可刷机的 UEFI 镜像

这里的文件是**这一版真正刷进手机的东西**，特意放进 git，好让一次刷机可以从一个
checkout 完整复现。构建与打包的完整环境见
[`docs/UEFI-BUILD-AND-PACKAGING.md`](../docs/UEFI-BUILD-AND-PACKAGING.md)，
换机交接见 [`docs/HANDOVER-CURRENT.md`](../docs/HANDOVER-CURRENT.md)。

| 文件 | 大小 | 说明 |
|---|---|---|
| `SILICIUM_UEFI.fd` | 2,097,152 B（= `FD_SIZE` 0x200000） | 当前构建产物 |
| `boot-uefi-mux.img` | 16,099,328 B | **当前上机版**：`CMD1` 探针（`cl=` 计数、DIN 修正、两个复用模式按 vendor 方式写入并读回）、M4/M5 其余内容不变 |
| `panel-preview.png` | 720×1600 | `tools/render_panel_preview.py` 渲染的仪表区，**不用刷机就能看布局** |

> 更早的几版（`boot-uefi-ping.img`、`boot-uefi-power.img`、`boot-uefi-m5-probe.img` …）
> 不再跟着仓库一起变大：它们仍在 git 历史里，需要时 `git log -- artifacts/` 取。
> 仓库里只保留当前版本。

sha256（**每次重建都会变**，只有和同一次构建的产物对得上才有意义）：

```
cec9ebe24468c7191081014f2f9b2df8802c2816425fa5576979f5d02ebaa0a6  SILICIUM_UEFI.fd
959205c6f2cf1a15e9e38f150f3062cb2d78ac613ec640a15fbfe7a39da951ed  boot-uefi-mux.img
```

## 这一版是什么（2026-10-29 深夜，第二版）

上一版（`boot-uefi-mux.img`）得到的读数（IMG_5743）：

```
PS before=81FF0002 after=81FF0002 DAT=000000FF CMD=1 CD=1
VEMC=00000001 en=1 ANA=00000300 vosel=3 3000mV
MUX strap=00094A40 mode=2 m0=2222FF00 m1=11222222
PING a I=00000200 R=00000000
PING b I=00000200 R=00000000
```

三件事：卡有电（3.0 V）；pins 124..127 复用读回 mode 2，但 **122/123 读回 `0xF`**；
`DIN` 读出全 0（那是探针自己的 bug）。所以这一版：

1. **`cl=`**：命令期间 `CMD`（bit24）读到 0 的次数 —— 直接把"控制器有没有驱动 CMD"
   变成一个十进制数字，不再靠数像素（上一版的紧循环只有 ~100 µs，可能刚好错过 120 µs 的命令相位）；
2. **`dA=` / `dO=`**：命令期间 12 个脚 DIN 的 AND / OR，某个 bit 翻转就是有时钟；
3. **按 vendor 方式写复用**（整寄存器一次写 `0x22222200` / `0x222222`），写完**读回打印**，
   然后 ping；mode 2 和 mode 1 都试，成功的那个保留；
4. 打印 `PAD0=` / `PAD1=`（`MSDC_PAD_CTL0/1`）：pad 若没有驱动能力，CMD 也出不去。

DIN 的寻址也修了：数据寄存器是 `(pin/32)*0x10` + `bit = pin%32`（`GpioDxe.c` 的 `PIN_OFFSET`），
只有 mode 寄存器才是 `(pin/8)*0x10`、每脚 4 位。

### 怎么看屏幕

**文字区是主证据**（IMG_5743 证明它拍得很清楚）：

```
PING a I=00000000 R=00000000 cl=NNNN dA=.... dO=....
SET mode=2 m0=.... m1=....
PING b I=.... cl=....
SET mode=1 m0=.... m1=....
PING c I=.... cl=....
MUX ANSWERED - mode kept | MUX restored to N
```

| 看到 | 结论 |
|---|---|
| `PING x` 的 `I` 有 bit8（`...0100` 之类） | 卡回了，`R` 就是 OCR（bit31=1 表示上电完成） |
| `cl=` 一直是 0 | 控制器没驱动 CMD ⇒ 主机侧（pad / 时钟 / 复用写不进去） |
| `cl=` > 0 但仍 CMDTMO | 命令确实上了线，卡不回 ⇒ 时钟没到卡 或 RSTB |
| `SET mode=N` 后 `m0` 跟着变 | 复用写入有效，模式不是问题 |
| `SET mode=N` 后 `m0` 不变 | 复用寄存器写不进去（别名语义不对） |

三条位图条（y 1172 / 1186 / 1200）仍是最新一次 ping 的 `PS` / AND / OR 备份；
x 400..560 的方块是判读结论。逐条判读见 `docs/POWER-BEFORE-AFTER.md` §4。

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
fastboot flash boot D:\aios\dandelion-xnu\artifacts\boot-uefi-mux.img
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
python3 tools/package_uefi_bootimg.py --out artifacts/boot-uefi-mux.img
```
