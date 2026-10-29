# artifacts/ — 可刷机的 UEFI 镜像

这里的文件是**这一版真正刷进手机的东西**，特意放进 git，好让一次刷机可以从一个
checkout 完整复现。构建与打包的完整环境见
[`docs/UEFI-BUILD-AND-PACKAGING.md`](../docs/UEFI-BUILD-AND-PACKAGING.md)，
换机交接见 [`docs/HANDOVER-CURRENT.md`](../docs/HANDOVER-CURRENT.md)。

| 文件 | 大小 | 说明 |
|---|---|---|
| `SILICIUM_UEFI.fd` | 2,097,152 B（= `FD_SIZE` 0x200000） | 当前构建产物 |
| `boot-uefi-ping.img` | 16,099,328 B | **当前上机版**：手工 `CMD1` 探针 + 两种引脚复用模式 + `MSDC_PS`/MT6357 读数，M4/M5 其余内容不变 |
| `panel-preview.png` | 720×1600 | `tools/render_panel_preview.py` 渲染的仪表区，**不用刷机就能看布局** |

> 更早的几版（`boot-uefi-power.img`、`boot-uefi-m5-probe.img`、`boot-uefi-m5-dma.img` …）
> 不再跟着仓库一起变大：它们仍在 git 历史里，需要时 `git log -- artifacts/` 取。
> 仓库里只保留当前版本。

sha256（**每次重建都会变**，只有和同一次构建的产物对得上才有意义）：

```
791ad79e6ab31307816d133c8ff8946b1429fc015c671842090de77a94191f52  SILICIUM_UEFI.fd
202be26f79ea89fe945e55ee005a1f4663c7eabf574cc8e92f3299c245d8223f  boot-uefi-ping.img
```

## 这一版是什么（2026-10-29 深夜）

**上一版把「供电」这条分支砍掉了。** IMG_5738 的读数是 `PS=0x81FF0002`、
`VEMC en=1`；按 vendor 的 `MSDC_PS` 位定义（DAT 在 bits 23..16、CMD 在 bit 24），
这意味着 **DAT=0xFF、CMD=1、卡在位、轨开着**——卡有电、总线干净，却一句都不回。

所以这一版改问**卡能不能听见我们**（见 `docs/POWER-BEFORE-AFTER.md`）：

1. 手工发一条 `CMD1`（只要 CMD + CLK，任何状态都能回）；
2. **在命令期间用紧循环采样 `MSDC_PS`**，把 AND / OR 画成两条位图条：如果 CMD
   （bit 24）在 AND 里还是 1，说明控制器**没驱动过** CMD —— 主机侧问题，不是卡；
3. 没有应答就把 GPIO122..133 换成**另一个复用模式**再问一次（成功则保留新模式，
   后面的 EmmcDxe 扫描可能当次就跑通）；
4. 文字区打印 strap、两个 mode 寄存器、12 个脚的 GPIO 输入电平（`DIN 122..133`）。

只读 PMIC，不碰时钟/总线宽度；唯一的写是那条有界的 `CMD1` 和一次（会被恢复的）引脚复用切换。
顺带修掉上一版的两个解码错误：**VOSEL 在 `VEMC_ANA_CON0`（0x1C20）而不是 `LDO_VEMC_CON0`**，
以及"NO-PULLUP"这个建立在错误位定义上的判读名。

### 怎么看屏幕（三条位图条 + 判读块 + 底部格子）

**先看 y 1172 / 1186 / 1200 三条**：`P_before`（青）/ `CMD1` 期间 AND（白）/ OR（青）。
关键不是颜色，而是 **bit 24（x≈530 那一格）** 的组合：

| AND bit24 | OR bit24 | 含义 |
|---|---|---|
| 1 | 1 | 控制器没驱动 CMD ⇒ pad / 时钟 / 复用 |
| 0 | 1 | 命令上了线，卡不回 ⇒ 时钟或复位 |

右边 x 400..560 的实心方块是判读结论（绿=BUS OK、橙=DAT/CMD 不高、红=轨关、洋红=PWRAP 没应答）。
逐条判读见 `docs/POWER-BEFORE-AFTER.md` §6。

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
kernel 段 = gzip(SILICIUM_UEFI.fd) 606,720 B（FD 2,097,152 B）
```

`kaddr` 必须等于 `[uefi_fd].base`（`Resources/Configs/dandelion.toml` = `0x40080000`）。
**`0x48880000` 是错的**：LK 只在 `0x40080000` 执行 kernel 段，见
`docs/CORRECTIONS.md` C8。

## 刷机与恢复

```powershell
adb reboot bootloader
fastboot flash boot D:\aios\dandelion-xnu\artifacts\boot-uefi-ping.img
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
python3 tools/package_uefi_bootimg.py --out artifacts/boot-uefi-ping.img
```
