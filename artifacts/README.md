# artifacts/ — 可刷机的 UEFI 镜像

这里的文件是**这一版真正刷进手机的东西**，特意放进 git，好让一次刷机可以从一个
checkout 完整复现。构建与打包的完整环境见
[`docs/UEFI-BUILD-AND-PACKAGING.md`](../docs/UEFI-BUILD-AND-PACKAGING.md)，
换机交接见 [`docs/HANDOVER-CURRENT.md`](../docs/HANDOVER-CURRENT.md)。

| 文件 | 大小 | 说明 |
|---|---|---|
| `SILICIUM_UEFI.fd` | 2,097,152 B（= `FD_SIZE` 0x200000） | 当前构建产物 |
| `boot-uefi-pv.img` | 16,099,328 B | **当前上机版**：在 `boot-uefi-state.img` 基础上多了一行 **`PV`**——用 `CMD13 → CMD10 → CMD13` 证明 `SDC_RESP0` 真的跟着命令走（而不是留着上一条命令的残留） |
| `panel-preview.png` | 720×1600 | `tools/render_panel_preview.py` 渲染的仪表区，**不用刷机就能看布局** |

> 更早的几版（`boot-uefi-state.img`、`boot-uefi-blockio.img`、`boot-uefi-power.img` …）
> 不再跟着仓库一起变大：它们仍在 git 历史里，需要时 `git log -- artifacts/` 取。
> 仓库里只保留当前版本。

sha256（**每次重建都会变**，只有和同一次构建的产物对得上才有意义）：

```
27b7393f660818e74250a864a9078cd206421e1e58bd3c62133fa7fc54455d61  SILICIUM_UEFI.fd
c1baecb7ca36af2b5dcd6dc95f47186f18624a289fa6e78fd6b9428cc56ee1fd  boot-uefi-pv.img
```

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
