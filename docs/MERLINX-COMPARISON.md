# 对照 merlinx（MT6768/MT6769Z，Redmi Note 9）：我们缺什么、我们有什么

来源：`Dracen-W/UniXNU-merlinx` 分支 `merlinx-work` 的 `docs/devices/merlinx.md`，
2026-10-30 读取。**它是目前最接近我们的参考实现** —— 同为小米、同为 MT676x 家族、
同一套 MTK LK 引导链、同样没有 UART。

> 先纠正本文档早前两处错误判断：
> 1. 我说过「MTK 上 iOS 用户态不现实」——**错**。merlinx 的 XNU 已经
>    `initproc 和 launchd exec 完成`；
> 2. 我说过「Mali GPU 没戏」——**错**。`hakios-mt6768`（MT6768, Infinix Hot 20）
>    的 `device.conf` 明写 `gpu works Mali-G52 MC2, panfrost and Metal`。

---

## 1. 他们已经到什么位置

| 层 | merlinx 的状态（原文摘要） |
|---|---|
| 硬件 | panel、PMIC 按键、p33 boot 分区、bootloader 行为都已测量 |
| UEFI | `merlinxPkg` 能从源码构建并从 p33 启动 |
| BootShim | **修好了自覆盖**：320 字节的源 shim 在加载固件卷之前先搬走 copy/clean 代码块 |
| Boot menu | 自写 GOP 菜单能显示；音量下滚动，音量上加电源都能选中 |
| Boot image | **64 MiB p33 镜像**：修好的 BootShim + 3 MiB 固件卷 + 裁剪的 DTB + ramdisk |
| 存储 | `NativeEmmc` 枚举出 userdata；OS 与 SystemOS 卷出现 |
| UniXNU/XNU | XNU 到达 root 与 SystemOS cache；**4 KiB 内核**用 `CONFIG_XNU_NATIVE_CS_HASH_CACHE` 处理原来的 16 KiB CodeDirectory；**initproc 与 launchd exec 完成** |
| 内核显示 | 内核通过 GOP framebuffer 画 `ROOT`/`VOL`/`CACHE`/`EXEC`/`LAUNCHD` 状态行 |
| Data | **未验证**；仓库不含 Data seed，可写 `/private/var` 与 launchd 服务图待办 |
| Userland | launchd exec 已确认；userland 服务、显示归属与渲染未验证 |

## 2. 我们（dandelion, MT6765）与它的真实差异

架构同源，**数值全不同**。这一列才是关键，不能照抄。

| 维度 | merlinx (MT6768/6769Z) | dandelion (MT6765) | 备注 |
|---|---|---|---|
| CPU | 2×A75 + 6×A53 | **4×A53，无大核** | 集群/调频、`cpus=` 参数不同 |
| GPU | Mali-G52 MC2 | **Mali-G72 MP1** | panfrost 两者都支持，但电源域/寄存器不同 |
| 面板 | FT8719 1080×2340，**scanout 从屏幕中间开始** | 720×1600，**stride 2944（736 px）** | 两边都要按自己量出来的值定位 |
| 存储 | MSDC0 eMMC | MSDC0 eMMC @ `0x11230000` | 同 IP；**我们已有实测可用的读写** |
| 引导链 | MTK LK + BootShim | MTK LK + BootShim | 同路 |
| `fastboot boot` | **不可用**（`usb_read failed e00002ed`） | **不可用** | 两边都只能 `fastboot flash boot` |
| UART | **没有** | **没有** | 见 §4 |
| boot 分区 | 64 MiB (p33) | 64 MiB（我们的 `boot_backup.img` 就是 64 MiB） | 我们现在只用了 16 MiB，有空间 |

## 3. 构建流程（他们的原话）

```sh
./unixnu defconfig merlinx_defconfig -D .
./unixnu build kernel
./unixnu build payload
./unixnu build bootimg --uefi ../uefi-merlinx --dtb <merlinx.dtb>
```

固件卷在 `Build/merlinxPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd`，boot image 把它
和 BootShim、裁剪的 DTB、ramdisk 打包，**pad 到 64 MiB** 写进 p33。

### 3.1 关键的一句：payload 步骤的输入「不分发」

> The payload step consumes the board trust cache, platform identity and other
> inputs described by the device repository. **These inputs are not distributed.**

也就是说，要复现到「XNU 跑起来」，除了代码还需要：

| 输入 | 说明 | 我们有没有 |
|---|---|---|
| **board trust cache** | 来自 IPSW 的 trustcache，按设备取 | ❌ 没有 |
| **platform identity** | 设备身份 blob（`UNIXNU_UNIQUE_CHIP_ID` 一类） | ❌ 没有 |
| **Data seed** | APFS Data 卷镜像，可用他们的脚本在 Linux 生成 | ⚠️ 可生成（脚本 + `data-seed.json`） |

Data seed 那部分**在 Linux 上可行**：`scripts/make-data-seed-linux.py` 用 `mkapfs`/`apfsck`
造一个「大小写敏感、无加密、无角色、只有空目录和偏好文件」的 APFS Data 卷，
输出 `data-linux.dbr` + 含 APFS UUID 的 JSON（UUID 给 `CONFIG_XNU_OWNER_DATA_VOLUME_UUID` 用）。

## 4. 我们的独有优势：日志通道

merlinx 文档里有两句话值得抄下来：

> There is no UART on this device. **Early boot evidence comes from the panel and
> from flashing test images to p33.**

以及他们的 Goals 第 5 条：

> Add a persistent log or recovery path without relying on UART.

**这一条我们已经做到了**：`flashinfo` 里的 64 KiB 环形日志 + 2 秒周期 flush +
`adb pull` + `tools/extract_log_block.py`。今晚 M6 的每一格进展都是靠它读出来的，
而参考实现还在把「不依赖 UART 的持久日志」当目标。

这是可以反向贡献给对方的东西，也是我们继续往前的最强工具。

## 5. 我们的缺口清单（按重要性）

| # | 缺口 | 现状 | 下一步 |
|---|---|---|---|
| 1 | ADT 的 `window-size` | `bootshim/adt.c` 不生成它，DarwinBoot 报 `Out of Resources` | 补属性，拿到绿条 |
| 2 | **XNU 侧 eMMC 驱动** | 只有 UEFI 侧 `DandelionEmmcDxe` | 对照 `NativeEmmc` / `NativeMsdc` 写 MT6765 版；寄存器行为我们已全部实测 |
| 3 | 设备仓库格式 | `licg89/UniXNU-dandelion` 是 line A 旧格式，缺 `package/`、`unixnu.lock` | 迁移到 `merlinx` 的形态，改用 `./unixnu build payload` |
| 4 | trust cache / platform identity | 没有，且上游**不分发** | 先搞清楚来源；这是最硬的一关 |
| 5 | Data seed | 没有 | 用 `make-data-seed-linux.py` 生成 |
| 6 | boot image 尺寸 | 16 MiB（分区有 64 MiB） | 需要放 ramdisk 时再扩到 64 MiB |

## 6. 结论

- **可以实现**：同代 MTK 上 XNU 到 launchd 已经有既成事实；
- **不能照抄**：CPU/GPU/面板/时钟/分区表全部要用 dandelion 自己的数；
- **最硬的一关不是驱动，是 trust cache 与 platform identity** —— 上游明确不分发；
- **我们的日志通道比参考实现更完整**，应该继续用它把每一格推进下去。
