# 生态：别人是怎么把 iOS 跑在非 Apple ARM64 上的

写于 2026-10-31。这一页是用来纠正一个错误结论的：本会话早先写过"这条路到不了 iOS"，
**那是错的**。已经有项目在 MediaTek 手机上跑起了 iOS 16.5 自己的用户态（launchd →
backboardd → SpringBoard → app），内核就是同一代开源 XNU。

## 三个项目，各走一条路

| 项目 | 路线 | 现在的状态（其 README 自述） |
|---|---|---|
| [oosexgang/hakios-mt6768](https://github.com/oosexgang/hakios-mt6768) | 自带裸机 loader（`mtkloader`）+ XNU + 自己写的 MTK 驱动，iOS 系统镜像放进独立分区 | **iOS 16.5 用户态跑起来**：锁屏/主屏/控制中心/内置 app、触摸、按键、显示、Mali-G52（panfrost + Metal，CoreAnimation 能画壁纸）都 work；Wi-Fi 在做 |
| [oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel)（HakiOS 主仓） | 开源内核 + loader + 驱动 + 工具；用户态由**使用者自己**从 Apple 公开 IPSW 里展开 | POCO X5 Pro（SM7325）上锁屏/主屏/控制中心/内置 app 全 work，Metal 部分 work |
| [UniXNU](https://github.com/UniXNU/)（我们这条链的上游） | **UEFI** 路线：UniXNU + DarwinBoot + ArmCpu + GopFramebuffer + XnuPrelink + RamBlock + VerifiedRoot | 目标 "Run iOS and macOS on non-Apple ARM64 hardware through UEFI"；旗舰是小米平板 6S Pro（SM8550） |
| [mouseos](https://github.com/mouseos) | 同 UEFI 路线，MT8167 平板（a04br3）——本仓库的直系祖先 | `xnu_bootshim_benesse_a04br3` 自述 "watchdog boot loop, OS boot incomplete"：**他们没走过我们现在的进度** |

## 关键事实（都能在他们仓库里指到）

1. **用户态不是自己写的**。HakiOS 的三层模型写得很清楚：开源内核 + "你自己放上去的用户态"
   + 不支持的 Apple 服务。用户态从 Apple 公开 IPSW 展开，仓库里**不含任何 Apple 二进制**
   （`LEGAL.md`）。这是合法路径，也是我早先漏掉的一环。
2. **内核版本同代**：HakiOS 用 `xnu-8796.121.2`，我们用 `xnu-8796.141.3`，同一个大版本。
3. **MTK 上的根设备是 eMMC 分区**：`bsd/dev/redwood_emmc.c` 把 `xnusys` 分区做成只读块设备
   当根（boot-arg `emmc_root=1`），iOS 镜像约 2.5 GB，内核边用边读。
4. **MediaTek 也要自己写 loader**：hakios-mt6768 明确说 "The first MediaTek phone of the project;
   everything else in the organisation is Qualcomm"，并为此写了 `mtkloader`。**我们不需要**——
   我们已经有 UEFI 链（LK → BootShim → UEFI → UniXNU → DarwinBoot），这是我们的优势。
5. **他们的驱动是"每个 SoC 一组"的普通内核文件**（`bsd/dev/*.c` + `iokit/Kernel/*.cpp`），
   按 boot-arg 开关：`emmc_root=1`、`touchscreen=2`、`mali=1`、`buttons=2`、`scp_step=7`。
   这正是我们 `patches/xnu/zzz*` 之外该补的那一层。

## 对 dandelion（MT6765 / 8×A53 / Mali-G72）意味着什么

MT6765 与 MT6768 是同一代、同一家族：MSDC0 eMMC、MTK 显示通路、Mali（G72 vs G52，都在
panfrost 覆盖范围）、SCP 传感器中枢都同源。所以可复用的是**方法**，不是地址：

| # | 步骤 | 参照 | 验收信号 |
|---|---|---|---|
| 1 | 手写最小 arm64 Mach-O，证明 exec 通路（含签名门）通 | 我们自己在做 | `initproc` 起来、用户态写标记 |
| 2 | eMMC 只读块设备 + `emmc_root=1` 根设备 | `redwood_emmc.c` | `BSD root: <emmc 分区>`（我们已有内核内 eMMC 写日志的通道可复用） |
| 3 | iOS 用户态布局（用户自己的 IPSW） | HakiOS `sysimage/` 工具 | `load_init_program` 找到真的 `launchd` |
| 4 | 显示驱动进 XNU | UniXNU `GopFramebuffer` / HakiOS display | 屏幕由内核接管，SpringBoard 能画 |
| 5 | 触摸 | `redwood_tcm.c` 的类比 | 触摸事件进 `/dev` |
| 6 | GPU（Mali-G72） | `redwood_mali.c` + `RedwoodMali.cpp`（panfrost） | CoreAnimation 出画面 |

## 结论修正

- "**到不了 iOS**" 是错的。正确说法：**到 iOS 用户态需要三样我们还没有的东西** ——
  一个能放用户态的根设备（eMMC 分区）、显示/触摸/GPU 驱动、以及从 IPSW 展开用户态的工具链。
  这些都有人做过，路径公开，方法是可复用的。
- 我们现在的进度（内核完整启动、timer 100 Hz、根文件系统挂载、`bsd_init: done`）在
  MediaTek + UEFI 这条细分路线上**比公开的那些 MTK 尝试更靠前**（mouseos 的 MT8167 还停在
  watchdog boot loop）。缺的是"用户态 + 驱动"这两层，不是内核本身。
