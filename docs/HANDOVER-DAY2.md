# 当前状态与换机器接手（2026-10-27 凌晨）

> 目的：换一台电脑继续时，读完这一份就能接着干，不需要翻聊天记录。

## 1. 一句话状态

Boot shim 完全正常（探针、ADT、handoff 都验证过），内核编译正常（CI 每版都能过），
但**内核目前在跑起来后很快就重启**，而且**看不到它的日志**——这就是全部问题所在。

已确认的硬事实（都经过真机实测，别再重复验证）：

| 事实 | 证据 |
|---|---|
| GIC 是 **GICv3 / GIC-500** | `PIDR2@0xFFE8 = 0x3b`，`GICD_IIDR = 0x1143b` |
| **两安全态**（SecurityExtn=1） | `GICD_TYPER = 0x790414` |
| 非安全侧**写不进**组寄存器 | `GICD_CTLR` 写 0x13 读回 0x12；`GICD_IGROUPR` / `GICR_IGROUPR0` 写 0xffffffff 读回 0 |
| 定时器确实在跑 | `CNTP/CNTV` 的 ISTATUS 在轮询中读到 1 |
| 中断就是收不到 | 上述状态下 `HPPIR`/`IAR` 始终 spurious |
| Android 能收 | `/proc/interrupts`：`GICv3 30 arch_timer`、`GICv3 23 arm-pmu` |
| **内核碰 GIC MMIO 会挂死** | 见 §2 |
| LK 不改 GIC | 反汇编 `lk.img`：载荷里没有 GICD 写、没有对 `GIC_SYNC_DCM` 的引用 |
| `fastboot boot` 不可用 | 下载 OKAY 后 `Booting FAILED (Status read failed)` |
| USB 控制器可写、PHY 被 LK 关掉 | `POWER` 可回读 0x60，`DEVCTL` 恒 0，PHY @0x11cc0000 读 0 |
| UniXNU 在 MTK 上也没有串口/USB 日志 | 见 `docs/research/unixnu-usb-logs.md`，他们 dandelion(MT6765) 页写 "no UART, blocked" |

## 2. 为什么会从"跑到 configure: done"回退

**这是我这轮引入的回归，根因很具体：**

- 老内核（commit `1137e5f`，产物 run#91）的 `pe_init_fiq()` 里有个
  `if (gic_arch < 3) return;` 早退——因为 `PIDR2` 读错偏移，它**从未真正碰过 GIC**，
  所以能一路跑到 `bsd_init` → `configure: done`。
- 新内核（GIC 改造后）在 `pe_init_fiq()` 里真的去读写 distributor：
  `GICD_CTLR` / `GICD_IGROUPR` / `GICR_*`。
- 同样的访问：**shim 里（MMU 关）被静默丢弃，内核里（MMU 开）会把总线卡死**。
  于是内核在 `arm_init: cpu_timabase_init` 之后**一个字都不打印就死**，
  随后被看门狗复位 → 表现为"重启循环"。

**修复**（commit `60cae45`）：内核侧**完全不碰 GIC MMIO**，只配 `ICC_*` 系统寄存器；
distributor / redistributor 的配置全部留在 shim 里做。

## 3. 以后怎么避免这类回归

1. **铁律**：`pe_init_fiq()`（以及任何 MMU 打开后的代码）**不得**访问 GIC 的
   MMIO（`0x0C000000` / `0x0C100000` 区域），只能用 `ICC_*` 系统寄存器。
   代码里已加注释；`dgic_*` 的调用一律传 `program_distributor = 0`。
2. **保留一份"已知能跑"的对照镜像**：`D:\aios\artifacts\boot-oldkernel.img`
   （老内核 run#91 + 当前 shim）。它的作用是：一旦新内核什么都不打印，
   刷它就能立刻区分"shim 坏了"还是"内核坏了"——这次就是靠它定位的。
3. **每次只改一件事**，并且先验证基线：CI 出新内核 → 刷 → 确认能看到
   `arm_init:` 日志 → 再叠加改动。
4. **CI 产物保留**：`mach_kernel` 14 天过期，重要版本立刻下载到 `artifacts\`。

## 4. 换机器要带走的东西

代码：`git clone https://github.com/licg89/dandelion-xnu.git`（全部补丁、工具、文档都在里面）

**不在 git 里、必须自己拷的**（`artifacts\`）：

| 文件 | 大小 | 用途 |
|---|---|---|
| `boot_backup.img` | 64 MiB | 原始 boot（含设备 DTB），恢复 Android 的唯一手段 |
| `stock_boot.dtb` | 127 KiB | 从 boot_backup 里提取的设备 DTB，打包必须用 |
| `backup\lk.img` / `lk2.img` | 各 2 MiB | 引导器备份（将来改 LK 时要） |
| `backup\mmcblk0boot0.img` | 4 MiB | preloader 备份 |
| `backup\dtbo.img` / `boot_para.img` / `seccfg.img` | — | 其它关键分区备份 |
| `boot-oldkernel.img` | 27 MiB | 对照镜像（老内核，能到 `configure: done`） |
| `boot-icconly.img` | 27 MiB | 当前修复版（内核只配 ICC） |
| `payload91.bin` | 14 MiB | 老内核 payload（对照用） |

## 5. 环境准备（新机器）

```powershell
# 1) 仓库
git clone https://github.com/licg89/dandelion-xnu.git D:\aios\dandelion-xnu
git clone --depth 1 --branch xnu-8796.141.3 https://github.com/apple-oss-distributions/xnu.git D:\aios\xnu

# 2) XNU 树恢复（LF 行尾很重要：Windows 克隆默认 CRLF 会让补丁全部失败）
cd D:\aios\xnu
git config core.autocrlf false
git rm -r --cached . -q; git reset --hard -q     # 重新按 LF 检出

# 3) 打补丁 + 拷板级文件
bash D:\aios\dandelion-xnu\tools\ci\apply-patches.sh D:\aios\xnu
bash D:\aios\dandelion-xnu\tools\ci\apply-board.sh D:\aios\xnu

# 4) 工具：Git for Windows(bash)、Python3、Android platform-tools、WSL(clang+qemu)
#    本机验证过：WSL Ubuntu 里 qemu-system-aarch64 / clang / aarch64-linux-gnu-gcc 都可用
```

CI 手动触发：

```powershell
. D:\aios\dandelion-xnu\tools\ci\gh.ps1
$token = Get-GHToken
$headers = @{ Authorization = "Bearer $token"; "User-Agent" = "dandelion-xnu-ci";
             Accept = "application/vnd.github+json"; "X-GitHub-Api-Version" = "2022-11-28" }
$body = '{"ref":"main","inputs":{"machine":"dandelion","kernel":"DEVELOPMENT"}}'
Invoke-WebRequest -Uri "https://api.github.com/repos/licg89/dandelion-xnu/actions/workflows/build-xnu.yml/dispatches" `
  -Method POST -Headers $headers -Body $body -ContentType "application/json" -UseBasicParsing
```

## 6. 刷机与取证（不含拍照的路线）

```powershell
# 打包（用原始 boot 作模板，DTB 位置不变）
wsl -d Ubuntu bash -c "cd /mnt/d/aios/dandelion-xnu/bootshim && \
  python3 mkxnu.py <mach_kernel> -o /mnt/d/aios/artifacts/payload.bin \
    --cmdline '-v serial=3 debug=0x14e keepsyms=1 cpus=1 -noprogress wdt=-1' && \
  python3 patch_boot.py /mnt/d/aios/artifacts/boot_backup.img out/Image.xnu \
    /mnt/d/aios/artifacts/stock_boot.dtb /mnt/d/aios/artifacts/boot-test.img \
    /mnt/d/aios/artifacts/payload.bin"

fastboot flash boot D:\aios\artifacts\boot-test.img ; fastboot reboot
# 恢复 Android
fastboot flash boot D:\aios\artifacts\boot_backup.img ; fastboot reboot
```

MTK LK 不支持 `fastboot boot`，每次试验都要"长按电源 10 秒 → 音量下+电源 → flash"。

## 7. 下一步（按优先级）

1. **USB 串口日志**（唯一能摆脱拍照的路，且现在有了确切的寄存器序列）
   见 `docs/research/mtk-usb-phy.md` 与 `docs/USB_CONSOLE.md`：
   - PHY 是 MTK **a60810** @ `0x11cc0000`（不是通用 t-phy，不要照抄 Linux 偏移）
   - 关键写：`PHY+0x6C = 0x3F2F`（强制 device 模式）
   - 时钟：`infracfg_ao+0x84 = 1<<8`、`topckgen+0x104 |= 1<<8|1<<9`、
     `apmixed+0x14 |= 1<<4`、`pericfg+0x29` 软复位脉冲
   - **判据**：`0x11200060`(DEVCTL) 读回 `0x98`；PHY `0xF0` 非 0
   - 代码已写进 `bootshim/usb_cdc.c`（`usb_phy_bringup()`），**尚未真机验证**
   - 验证方式：刷 `usb` 模式镜像后看**主机是否出现新 USB 设备**（不需要拍照）
2. **eMMC 持久日志**（USB 不通时的备选）：`flashinfo`(16MiB raw) + polled MSDC 写入，
   Android 起来后 `adb shell su -c dd if=/dev/block/by-name/flashinfo` 读回。
   格式与解码器已就绪：`bootshim/log_block.h` + `tools/read_log_block.py`（已用合成 dump 验证）。
3. **内核重启原因**：在拿到日志之前不要猜。有了 USB 或 eMMC 通道后再定位。
4. **中断/tick**：被固件层挡住（见 §1），唯一实质路径是 patch LK/preloader；风险高，
   已有全部备份。

## 8. 已知坑

- Windows 上克隆 XNU 树必须先 `core.autocrlf false`，否则补丁全部应用失败。
- `patch_boot.py` 要求 payload 长度是 page(2048) 的倍数（mkxnu 已保证）。
- 内核控制台从 **y=860** 开始画，会覆盖 shim 的 `JUMPING` 等尾部输出——
  "看不到 JUMPING"不等于没跳转。
- QEMU 里 `pmemsave` 在这台机器上不可用（第三个参数被当表达式解析），
  所以 QEMU 侧读不到内核 trace ring；`tools/qemu/qemu_run.py --fb` 的解码路径也未跑通。
- `fastboot getvar all` 能看到分区表；`flashinfo` 16MiB 是空白可用区。
