# artifacts/ — 可刷机的 UEFI 镜像

这里的文件是**这一版真正刷进手机的东西**，特意放进 git，好让一次刷机可以从一个
checkout 完整复现。构建与打包的完整环境见
[`docs/UEFI-BUILD-AND-PACKAGING.md`](../docs/UEFI-BUILD-AND-PACKAGING.md)，
换机交接见 [`docs/HANDOVER-CURRENT.md`](../docs/HANDOVER-CURRENT.md)。

| 文件 | 大小 | 说明 |
|---|---|---|
| `SILICIUM_UEFI.fd` | 2,097,152 B（= `FD_SIZE` 0x200000） | 当前构建产物 |
| `boot-uefi-r1.img` | 16,099,328 B | **当前上机版**：`CMD0` → `CMD1`(到 OCR bit31) → `CMD2/3/7/13` 走到 TRAN，然后 `CMD16/CMD8/CMD17` **真读 512 字节**（EXT_CSD + LBA 1）；整段日志压到 10 行 |
| `panel-preview.png` | 720×1600 | `tools/render_panel_preview.py` 渲染的仪表区，**不用刷机就能看布局** |

> 更早的几版（`boot-uefi-ping.img`、`boot-uefi-power.img`、`boot-uefi-m5-probe.img` …）
> 不再跟着仓库一起变大：它们仍在 git 历史里，需要时 `git log -- artifacts/` 取。
> 仓库里只保留当前版本。

sha256（**每次重建都会变**，只有和同一次构建的产物对得上才有意义）：

```
b8770cc1e9db1f196d48ea83ade69f62a099366f48a2244cda753d95a0204256  SILICIUM_UEFI.fd
9136f4cc4a1a601b532dbfbfaa3e4b3037f4225785726c02cabc151a442ea7b1  boot-uefi-r1.img
```

## 这一版是什么（2026-10-29 深夜，第九版）

上一版（`boot-uefi-datwin.img`）**把探针自己写崩了**：读 `0x10002010`
（MSDC0 pad 块，不在这个平台的内存映射里）触发三级页表 translation fault，
DxeCore 把寄存器转储打在屏幕上（IMG_5755，`X8 = 0x000000010002010`）。

这一版：

1. **不再读 pad 块**；输入使能改由 GPIO 窗口回答（`din=` 当前电平、`gp=` 整个
   等待窗口的 AND/OR）；
2. **打 CMD8/CMD17 的 R1**（`R=`）：卡“回答了但不发数据”和“回答里带错误位”
   在之前是分不清的；
3. 保留 `dir=` / `do=`（pinctrl 已映射），能抓到 GPIO 抢总线。

> 以后真要读 pad 块：先 `gDS->AddMemorySpace (EfiMemoryMappedIO, 0x10002000,
> 0x1000, EFI_MEMORY_UC)` 再加映射，**不要直接 load**。

已知结论（IMG_5754，三种模式都跑过）：`ps=EF/FF` —— 控制器看到的 DAT[7:0]
全程为高，只有 DAT4 抖过；命令、CID、选中全正常。

### 怎么看屏幕

```
PWR PS=81FF0002 DAT=FF CMD=1 CD=1 VEMC=1 vsel=3 3000mV w=1 f=0 BUS OK
H0 C=.. S=.. P=.. N=..
C0 I=.. cl=.. f=.. l=.. kc=..
C1 n=.. I=.. R=........
C2 3=.. 2=.. 1=.. 0=..
C3 I=.. R=.. C7 I=.. R=..
C13 I=.. R=.. st=..
IO dir=000 do=000 din=FFF
IO ioc=........ f60=........ adv=........
A8 I=.. R=.. sz=.. ps=../../.. gp=.../... f=.. c6=....
B8 ...（A8 失败才出现）
C8 ...（B8 失败才出现）
B1 I=.. S=..
```

| 看到 | 结论 |
|---|---|
| 任一行 `I=...1100` + `sz` 非零 + `B1 S=45464920` | **数据通路打通** |
| `R=` 里带错误位 | 卡拒绝发数据，按位查卡状态 |
| `ps` 安静 + `gp` 有变化 | 控制器看不到卡 ⇒ pad / 输入通路 |
| `ps`/`gp` 都安静 | 卡没驱动数据线 |
| 屏幕变成寄存器转储 | 探针踩了未映射地址 |

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
kernel 段 = gzip(SILICIUM_UEFI.fd) 607,744 B（FD 2,097,152 B）
```

`kaddr` 必须等于 `[uefi_fd].base`（`Resources/Configs/dandelion.toml` = `0x40080000`）。
**`0x48880000` 是错的**：LK 只在 `0x40080000` 执行 kernel 段，见
`docs/CORRECTIONS.md` C8。

## 刷机与恢复

```powershell
adb reboot bootloader
fastboot flash boot D:\aios\dandelion-xnu\artifacts\boot-uefi-r1.img
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
python3 tools/package_uefi_bootimg.py --out artifacts/boot-uefi-r1.img
```
