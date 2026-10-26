# 交接说明（换机器继续开发）

本文件记录「在另一台电脑上继续这个项目」所需的一切。目标：clone 完就能接着干活。

## 1. 项目是什么

在 Xiaomi Redmi 9A（dandelion / MT6762G / Helio G25）上启动 Apple 开源 XNU（xnu-8796.141.3）。

- 通过 boot 分区的 shim 把 XNU 装进 RAM 并在 EL1 跳转（自定义 payload 格式，不是标准 Android boot image 的 ramdisk 引导）
- 没有物理串口，**所有诊断都靠 framebuffer 打印**
- XNU 内核本身**只在 CI 上编译**（macOS runner），本地只编 shim 和打包镜像

## 2. 目录布局

```
D:\aios\
  dandelion-xnu\      ← 主仓库（本项目全部内容，推到 GitHub）
  xnu\                ← XNU 源码树（本地 clone，未提交；靠 patches/xnu/*.patch 复现）
```

`xnu\` 不是 git 提交源。**唯一可信来源是 `dandelion-xnu/patches/xnu/*.patch`**。
新机器上重新 clone XNU 再打补丁即可，不要试图拷贝那棵 35 个改动文件的树。

## 3. 必须手动搬运的东西（被 .gitignore 排除）

`out/` 整个目录都在 .gitignore 里，以下文件**不在 git 中**：

| 文件 | 大小 | 重要性 |
|---|---|---|
| `out\boot_backup.img` | 64 MiB | **关键**。出厂 boot 分区备份，用来恢复 Android。丢了只能重刷整包 |
| `out\dandelion.dtb` / `out\stock_boot.dtb` | 277 KiB | 设备真实 DTB，比对平台参数用 |
| `out\boot_xnu.img` | 27 MiB | 当前测试镜像，可随时重新生成 |

至少把 `boot_backup.img` 和两个 dtb 拷到 U 盘 / 网盘。

## 4. 新机器一次性准备

必需：

- **Git for Windows**（提供 `bash.exe`，编 shim 要用）
- **Python 3**（`bootshim/check_payload.py` 校验 payload）
- **Android platform-tools**（`fastboot.exe`），并确认能识别设备：
  `fastboot devices` → 期望 `QS55XCPRCA9LDA5T  fastboot`
- **LLVM/Clang**（Windows 版），默认装在 `C:\Program Files\LLVM\`。
  既用来编 shim，也用它的 `llvm-objdump.exe` / `llvm-nm.exe` 反汇编排错
- **PowerShell 5.1+**（脚本是 .ps1）

不需要：macOS、Xcode、交叉编译器。内核编译交给 GitHub Actions。

## 5. 克隆与还原源码树

```powershell
git clone git@github.com:licg89/dandelion-xnu.git D:\aios\dandelion-xnu

git clone https://github.com/apple-oss-distributions/xnu.git D:\aios\xnu
cd D:\aios\xnu
git checkout xnu-8796.141.3

# 应用本项目全部补丁（按文件名排序，自动跳过已应用的）
"C:\Program Files\Git\bin\bash.exe" -lc "cd /d/aios/dandelion-xnu && tools/ci/apply-patches.sh /d/aios/xnu"
```

期望输出末尾是 `applied: non-apple-video-console-text.patch`，没有 `does not apply`。

**注意**：`xnu` 目录路径写死在部分脚本和我的习惯命令里。若新机器不用 `D:\aios`，
需要把所有 `D:\aios` 替换成实际路径（`git grep -n "D:\\\\aios"` 可查）。

## 6. GitHub 凭据（容易踩）

`tools/ci/gh.ps1` 通过 `git credential fill` 从 **Windows 凭据管理器**取 token，不存明文。

新机器上如果只配了 SSH remote 而没走过 HTTPS，`Get-GHToken` 会抛
`no stored github credential`。解决办法二选一：

1. 先用 HTTPS 做一次 clone/fetch，让 Git Credential Manager 记住 token
2. 或手动写入凭据：
   ```powershell
   "protocol=https`nhost=github.com`nusername=<你的账号>`npassword=<PAT>" | git credential approve
   ```
   PAT 需要 `repo` + `workflow` 权限。

## 7. 日常开发循环

```powershell
# 1) 改内核源码
#    注意：改的是 D:\aios\xnu 里的文件，改完后必须重新生成补丁（见第 8 节）

# 2) 等 CI 编内核（约 7-10 分钟）
. D:\aios\dandelion-xnu\tools\ci\gh.ps1
$token = Get-GHToken
$headers = @{ Authorization = "Bearer $token"; "User-Agent" = "dandelion-xnu-ci";
             Accept = "application/vnd.github+json"; "X-GitHub-Api-Version" = "2022-11-28" }
$body = '{"ref":"main","inputs":{"machine":"dandelion"}}'
Invoke-WebRequest -Uri "https://api.github.com/repos/licg89/dandelion-xnu/actions/workflows/build-xnu.yml/dispatches" `
  -Method POST -Headers $headers -Body $body -ContentType "application/json" -UseBasicParsing

# 3) 下载产物 + 重新打包 + 刷机
powershell -NoProfile -File D:\aios\dandelion-xnu\tools\ci\fetch_art.ps1 -Sha <commit>
powershell -NoProfile -File D:\aios\dandelion-xnu\rebuild-device-image.ps1 `
  -Kernel <产物目录>\mach_kernel
python D:\aios\dandelion-xnu\bootshim\check_payload.py `
  D:\aios\dandelion-xnu\out\boot_xnu.img <产物目录>\mach_kernel   # 必须 RESULT: identical

fastboot flash boot D:\aios\dandelion-xnu\out\boot_xnu.img
fastboot reboot

# 4) 恢复 Android
fastboot flash boot D:\aios\dandelion-xnu\out\boot_backup.img
```

`rebuild-device-image.ps1` 会顺带**重新编译 shim**（调 bash+clang），所以改了
`bootshim/` 下的东西也会进镜像。

进不了 fastboot 时：长按电源 10 秒 → 音量下 + 电源 → fastboot。

## 8. 补丁生成规则（最容易搞坏 CI）

补丁是唯一真相，但生成时极易出错。必须遵守：

1. **只对你改过的那几个文件生成 diff**，不要 `git diff` 全量（会把别的补丁的改动混进来）。
2. **必须 UTF-8 无 BOM、LF 换行**。
   PowerShell 的 `>` 会写成 UTF-16 LE，`git apply` 直接报 `unrecognized input`。
   正确写法：
   ```powershell
   cmd /c "cd /d D:\aios\xnu && git diff -- <文件列表> > <补丁路径>"
   $b = [System.IO.File]::ReadAllBytes($p)
   [System.IO.File]::WriteAllBytes($p, [byte[]]($b | Where-Object { $_ -ne 13 }))
   ```
3. 生成后**自检**：`git -C D:\aios\xnu apply --reverse --check <补丁>` 必须成功。
4. **一个文件只能归属一个补丁**。已有的归属：

   | 补丁 | 负责的文件 |
   |---|---|
   | `dandelion-early-trace.patch` | `osfmk/arm/arm_init.c`、`osfmk/arm64/arm_vm_init.c`、`osfmk/console/serial_console.c`、`osfmk/kern/{debug,printf,startup}.c`、`pexpert/arm/pe_fiq.c` |
   | `non-apple-platform-expert-fallback.patch` | `iokit/Kernel/IOPlatformExpert.cpp` |

   把 `IOPlatformExpert.cpp` 塞进 `dandelion-early-trace.patch` 会让后者排在前、
   抢先改掉同一段上下文，导致 `non-apple-platform-expert-fallback.patch` 应用失败
   （已经踩过一次）。

5. 提交前最好做一次全量演练（26 个补丁按序应用，0 失败）：
   ```bash
   # 在临时目录里用 git show HEAD:<path> 导出 pristine 文件，再按文件名顺序 git apply
   ```
   `tools/ci/apply-patches.sh` 的逻辑是：先 `--reverse --check`，成功则视为已应用并跳过；
   否则 `--check` 后 `apply`。

## 9. 当前进度与已知状态

已完成：

- shim：EL2→EL1 跳转、FDT 解析、ADT 合成、boot_args 构建、payload 加载
- XNU：`start.s` → `arm_init` 全流程 → `machine_startup` → `bsd_init`
- BSD 自检全部 PASSED、线程创建、IPC、IOKit 初始化、网络栈初始化
- framebuffer 滚动控制台 + 真实 `kprintf` 输出镜像（`_cnputc` 钩子）

当前停点：

```
bsd_init: calling setconf
DANDELION: platform expert: configure
```

即卡在 `IOPlatformExpert::configure()` 的 `createNub / attach / registerService` 里。

已确认 / 强推断：

- `arm_vm_init` 正常返回；之前「停在 arm_vm_init: return」是**文字被 Y 上限 1500 静默丢弃**，
  不是崩溃。已改为 1580 + 滚动。
- GIC：DTB 声明 `arm,gic-v3`，但 `ICC_BPR0_EL1` 在 EL1 触发 UNDEFINED。
  `ICC_SRE_EL1` 读回 1、shim 也已设 `ICC_SRE_EL2=0xf`，仍复现。
  加入「PIDR2.ArchRev < 3 就跳过 ICC 配置」后内核能一路跑到 bsd_init，
  **说明这颗 SoC 实际不是 GICv3**（很可能是 GIC-400/GICv2），
  `pe_fiq.c` 的 GICv3 路径对它不适用。
- 因此**目前没有任何中断源**，定时器不跳，任何带超时的阻塞等待都会永远挂住。

下一步优先级：

1. 拿到 `GICD_PIDR2`、`GICD_TYPER`、`GICR_TYPER` 的真实值（日志会滚走，
   已改为在 `IOPlatformExpert` 里二次打印，见 `dandelion_gic_*` 全局变量）
2. 若确认 GICv2：在 `pe_fiq.c` 里实现 GICv2 路径（GICC 内存映射，
   GICC_CTLR/PMR/BPR + GICD_ISENABLER 使能 PPI），替换现在的 GICv3 系统寄存器路径
3. 有定时器中断后再看 `configure()` 能否走完

## 10. 硬上限（要提前知道）

开源 XNU 没有 HFS+/APFS，也没有 ext4/f2fs，更没有 `md` 设备。
所以即使中断全部修好，最终也会停在 `waiting for root device`，
**无法进入用户态 / 起 Android**。这是该移植的结构性边界，不是调参能解决的。

## 11. 调试手段速查

| 位置 | 用途 |
|---|---|
| `osfmk/kern/printf.c` | `dandelion_fb_putc` / `dandelion_fb_scroll` / `dandelion_console_putc`；FB 常量 |
| `osfmk/kern/printf.c` `dandelion_probe_bar()` | 右侧彩条探针，不受文字滚动影响，二进制式回答「是否执行到」 |
| `DANDELION_TRACE("...")` | 打一行检查点（走 `dandelion_trace_mark`） |
| `kprintf("...")` | 现在会镜像到滚动区，推荐用于后续调试 |
| `osfmk/kern/debug.c` | panic hook，把 panic 格式串打到 FB |

屏幕坐标约定（`printf.c` 顶部宏）：

- 文字区：y 从 860 到 1580，行高 16，x 从 8 开始，满屏自动滚动
- 探针条：x 480–720，宽 240（滚动只动 x<472，不会擦掉探针）
- shim 日志占 y≈0–800，内核文字不会覆盖它

反汇编排错（确认某段代码有没有真的编进去）：

```powershell
& "C:\Program Files\LLVM\bin\llvm-nm.exe" -n <unstripped> | Select-String "_arm_vm_init|_pe_init_fiq"
& "C:\Program Files\LLVM\bin\llvm-objdump.exe" -d --start-address=0x... --stop-address=0x... <unstripped>
```

CI 产物里的 `obj/DEVELOPMENT_ARM64_DANDELION/mach.development.dandelion.unstripped`
是带符号的，排错用它而不是 `mach_kernel`。

## 12. 环境差异注意

- **PowerShell 不支持 `&&`**，用 `;`
- PowerShell 的 `>` 写 UTF-16 LE；二进制/补丁一律用 `cmd /c "... > file"` + 去 CR
- Windows 路径 vs bash 路径：bash 里是 `/d/aios/...`
- `fastboot` 若找不到，检查是否在 `PATH`；本机在
  `C:\Users\li\AppData\Local\Android\Sdk\platform-tools\fastboot.exe`
- 设备 USB 驱动：MTK 平台，Windows 需要能出 fastboot 接口；换机器后先 `fastboot devices` 验证
- CI runner 固定 `macos-14`，XNU tag 固定 `xnu-8796.141.3`（见 `.github/workflows/build-xnu.yml`）
