# artifacts/ — 可刷机的 UEFI 镜像

这里的文件是**这一版真正刷进手机的东西**，特意放进 git，好让一次刷机可以从一个
checkout 完整复现。构建与打包的完整环境见
[`docs/UEFI-BUILD-AND-PACKAGING.md`](../docs/UEFI-BUILD-AND-PACKAGING.md)，
换机交接见 [`docs/HANDOVER-CURRENT.md`](../docs/HANDOVER-CURRENT.md)。

| 文件 | 大小 | 说明 |
|---|---|---|
| `SILICIUM_UEFI.fd` | 2,097,152 B（= `FD_SIZE` 0x200000） | 当前构建产物 |
| `boot-uefi-cmd0.img` | 16,099,328 B | **当前上机版**：先 `CMD0` 再 `CMD1`；CMD 波形用 GPIO122 逐样本计数（`cl=/f=/l=/kc=`），M4/M5 其余内容不变 |
| `panel-preview.png` | 720×1600 | `tools/render_panel_preview.py` 渲染的仪表区，**不用刷机就能看布局** |

> 更早的几版（`boot-uefi-ping.img`、`boot-uefi-power.img`、`boot-uefi-m5-probe.img` …）
> 不再跟着仓库一起变大：它们仍在 git 历史里，需要时 `git log -- artifacts/` 取。
> 仓库里只保留当前版本。

sha256（**每次重建都会变**，只有和同一次构建的产物对得上才有意义）：

```
f4a0764546c3a53145aea39cd5f23689fe22fd2785c5c44b7b4a2ef20703ebc5  SILICIUM_UEFI.fd
85e8caae324aa66476d71da3eb6776013a90a8d45f3431a4c79e948ca98bbdb4  boot-uefi-cmd0.img
```

## 这一版是什么（2026-10-29 深夜，第三版）

上一版（`boot-uefi-mux.img`）得到的读数（IMG_5744）：

```
MUX strap=00094A40 mode=2 m0=22222200 m1=11222222
DIN 122..133=00000DFB
PAD0=00000000 PAD1=00000000
PING a I=00000200 R=00000000 cl=0 dA=00000DFA dO=00000DFF
```

三件事：

1. **复用是好的**，而且上一版的 `0xF` 是虚惊：每个引脚的 4 位模式字段里只有低 3 位是模式，
   第 4 位是保留位。按 vendor 的写法整寄存器写一次再读回就是 `0x22222200`。
2. **`MSDC_PS` 的 CMD 位（bit24）在控制器输出时被屏蔽**（`cl=0`），而同一个脚的 GPIO 输入
   （`dA`/`dO` 的 bit0）确实翻转——**命令确实上了线**，时钟（bit2 = GPIO124）也在跑。
3. `DIN=0xDFB`：12 个脚里只有 124（CLK）和 132 低，而 132 是 `DS`，vendor 的 pad 配置就是
   `dsl:pd50k`（下拉）。**RSTB 是高的，DAT 全高。**

所以现在的事实是：**有电、复位放开、时钟在跑、命令在线上，就是没有 R3。**

本版只做一件事：按规范的顺序问 —— 上一版只发了 `CMD1`，而 **`CMD1` 只在 idle 状态合法**，
被 LK 留在 TRAN（或卡在某个中间状态）的卡对它就是不理会，表现正好是"总线干净 + 超时"。
于是：

1. **先 `CMD0`（GO_IDLE_STATE，无应答）再 `CMD1`（SEND_OP_COND）**；
2. 采样改用 **GPIO122（CMD 脚本身）**，并给出时间结构：
   - `cl=` 读到 0 的样本数、`f=` / `l=` 第一次 / 最后一次读到 0 的迭代序号；
   - `kc=` CLK 脚读到 0 的样本数（时钟在跑 ≈ 中间值）；
3. `CMD0` 那行是**发射机自检**：它一定没有应答，所以它的 `f..l` 就是纯命令帧长度；
   `CMD1` 的 `l` 如果约等于两倍帧长 ⇒ **卡其实回了，是这个控制器采样不到**（另一类 bug，
   要去看 `MSDC_PAD_TUNE0` / `EMMC50_CFG0`）；
4. `PING b` 用不带 bit30/bit31 的朴素 OCR 再发一次 `CMD1`，把"参数被拒"也排除掉。

### 怎么看屏幕

**文字区是主证据**：

```
C0 I=........ cl=... f=.. l=... kc=...
PING a I=00000200 R=00000000 cl=... f=.. l=... kc=...
PING b I=........ R=... cl=... f=.. l=... kc=...
CARD ANSWERED | NO ANSWER
```

| 看到 | 结论 |
|---|---|
| `PING` 的 `I` 有 bit8（`...0100` 之类） | 卡回了，`R` 就是 OCR（bit31=1 表示上电完成） |
| `C0` 的 `cl=0` | 控制器连无应答的 `CMD0` 都没发出去 ⇒ 主机侧（`SDC_CMD` 写回 / 时钟门 / 控制器复位） |
| `C0` 的 `cl` 正常、`PING` 的 `l-f` 也差不多 | 命令帧发出去了，卡不回 ⇒ 卡有没有真拿到电 / 卡本身 |
| `PING` 的 `l` ≈ 2×(帧长) | **卡回了，主机采样不到** ⇒ `MSDC_PAD_TUNE0`(0xF0) / `EMMC50_CFG0` |
| `kc` 接近 0 或 30000 | 命令窗口里时钟没跑 ⇒ `MSDC_CFG.CKSTB` / topckgen |

三条位图条（y 1172 / 1186 / 1200）仍是最新一次 `CMD1` 的 `PS` / AND / OR 备份；
x 400..560 的方块是判读结论。逐条判读见 `docs/POWER-BEFORE-AFTER.md` §3。

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
fastboot flash boot D:\aios\dandelion-xnu\artifacts\boot-uefi-cmd0.img
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
python3 tools/package_uefi_bootimg.py --out artifacts/boot-uefi-cmd0.img
```
