# artifacts/ — 可刷机的 UEFI 镜像

这里的文件是**这一版真正刷进手机的东西**，特意放进 git，好让一次刷机可以从一个
checkout 完整复现。构建与打包的完整环境见
[`docs/UEFI-BUILD-AND-PACKAGING.md`](../docs/UEFI-BUILD-AND-PACKAGING.md)，
换机交接见 [`docs/HANDOVER-CURRENT.md`](../docs/HANDOVER-CURRENT.md)。

| 文件 | 大小 | 说明 |
|---|---|---|
| `SILICIUM_UEFI.fd` | 2,097,152 B（= `FD_SIZE` 0x200000） | 当前构建产物 |
| `boot-uefi-clock.img` | 16,099,328 B | **当前上机版**：`CMD0` → `CMD1`(到 OCR bit31) → `CMD2/3/7/13` 走到 TRAN，然后 `CMD16/CMD8/CMD17` **真读 512 字节**（EXT_CSD + LBA 1）；整段日志压到 10 行 |
| `panel-preview.png` | 720×1600 | `tools/render_panel_preview.py` 渲染的仪表区，**不用刷机就能看布局** |

> 更早的几版（`boot-uefi-ping.img`、`boot-uefi-power.img`、`boot-uefi-m5-probe.img` …）
> 不再跟着仓库一起变大：它们仍在 git 历史里，需要时 `git log -- artifacts/` 取。
> 仓库里只保留当前版本。

sha256（**每次重建都会变**，只有和同一次构建的产物对得上才有意义）：

```
10ae0959856a19838f1e616e605301f8d40ba5aa8415d0d459ee114b2a12ef11  SILICIUM_UEFI.fd
31b89dc27d2bf5824c8eab458c192ef4341a36d4ad5f418a04bd34b57323ee1b  boot-uefi-clock.img
```

## 这一版是什么（2026-10-29 深夜，第十版）

上一版（`boot-uefi-r1.img`）的读数（IMG_5757）非常干净：

```
H0 C=0200F09B S=FF007FFC P=81FF0002 N=00000001
IO dir=000 do=000 din=00000DFF
IO ioc=00000000 f60=00000000 adv=0000E63F
A8 I=00000100 R=00000900 sz=00000000 ps=EF/FF gp=DF8/DFF f=00 c6=0000
B8 I=00000100 R=00000900 sz=00000000 ps=EF/FF gp=DF8/DFF f=00 c6=0100
C8 I=00000100 R=00000900 sz=00000000 ps=EF/FF gp=DF8/DFF f=00 c6=0100
B1 -  no data phase
```

- `R=00000900`：卡在 TRAN、READY_FOR_DATA、**没有错误位** —— 不是拒绝，是不说话；
- `gp=DF8/DFF`：整个等待窗口只有 CMD/CLK 动过，**没有一根 DAT**；
- `dir=000`：GPIO 没抢总线；
- `H0 C` 的 **bit1 = CKPDN**（时钟 pad 掉电）= **1**。

一张状态良好、总线干净、却一位不发的卡，配上“时钟 pad 掉电”，只剩一个解释：
**数据相位没有时钟**。卡没时钟移不出位；控制器没 sclk 也不数超时——所以连
`DATTMO` 都从没出现过。

这一版：

1. **开跑前软复位控制器**（`MSDC_CFG.RST`，自清零，再补回 `PIO` 和继承的
   `SDC_CFG=0xFF007FFC`）——LK 几分钟前刚用同一个控制器读过同一个卡，中间只
   经过 MsdcDxe 的一次失败传输；
2. **第 4 个尝试 `D8`**：1-bit + **`CKPDN` 清零**（时钟 pad 保持供电）；
3. 每个尝试新增 **`k=`**：等待窗口里 CLK 脚读到 0 的样本数（≈一半 = 时钟一直在跑，
   很小 = 响应结束就停了）。

### 怎么看屏幕

```
PWR PS=81FF0002 DAT=FF CMD=1 CD=1 VEMC=1 vsel=3 3000mV w=1 f=0 BUS OK
RST C=0200F09B
H0 C=.. S=.. P=.. N=..
IO dir=000 do=000 din=00000DFF
C0 I=.. cl=.. f=.. l=.. kc=..
C1 n=.. I=.. R=........
C2 3=.. 2=.. 1=.. 0=..
C3 I=.. R=.. C7 I=.. R=..
C13 I=.. R=.. st=..
A8 I=0100 R=........ sz=........ ps=../../.. gp=.../... f=.. k=..... c6=........
B8 ...（A8 失败才出现）
C8 ...（B8 失败才出现）
D8 ...（C8 失败才出现，CKPDN 已清零）
B1 I=.. S=..
```

| 看到 | 结论 |
|---|---|
| 任一行 `I=1100` + `sz` 非零 + `B1 S=45464920` | **数据通路打通** |
| 只有 `D8` 成功 | 就是 CKPDN / 时钟 pad |
| 都失败但 `k=` 很小 | 数据相位没时钟 |
| 都失败而 `k≈15000` | 时钟在跑，卡不发 → 查卡侧 |
| `R=` 带错误位 | 卡拒绝 |

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
fastboot flash boot D:\aios\dandelion-xnu\artifacts\boot-uefi-clock.img
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
python3 tools/package_uefi_bootimg.py --out artifacts/boot-uefi-clock.img
```
