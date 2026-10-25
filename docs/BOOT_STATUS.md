# 启动进度（2026-10）

本文记录 Dandelion 移植当前的启动状态、已经打通的阶段，以及为什么停在这里。

## 现在能跑到哪里

QEMU（`-M virt,gic-version=3 -cpu cortex-a53`）和真机走同一条启动路径。控制台最后停在：

```
DANDELION: platform expert: configure
!BSD
Waiting on <dict ID="0"><key>String ID="1">IOProviderClass</key><string ID="1">IOMedia</string>
<key>Content Mask</key><string ID="2">apple_hfs</string></dict>
Still waiting for root device
DANDELION: no root filesystem can be mounted on this port (open-source XNU has no HFS+/APFS);
kernel stays up
Still waiting for root device
```

最后两行每 60 秒重复一次（`ROOTDEVICETIMEOUT`），屏幕会一直保持在这个状态。

也就是说：

- `PE_lockdown_iokit()` 完成，平台专家（`IOPanicPlatform` 回落到通用 `IOPlatformExpert`）启动成功；
- `kernel_bootstrap_thread` 走完 `vm_set_restrictions` / `kperf_init_early`，进入 `bsd_init()`；
- BSD 侧 `kminit` / `aio_init` / `pshm` / `psem` / `socketinit` / `domaininit`（inet、inet6、local、key）/ `skywalk` / `necp` / `netagent` / `memorystatus` / `sysctl_mib_init` / `bsd_autoconf` 全部执行；
- `bsd_init` 最后在 `setconf()` → `IOFindBSDRoot()` 里等待一个 `Apple_HFS` 的 `IOMedia`，每 60 秒打印一次 `Still waiting for root device`。

不再 panic，CPU 进入 idle 线程，机器稳定。

## 为什么停在「等根设备」

开源 XNU 树里**没有文件系统实现**（HFS+/APFS 都是闭源的），也没有内存盘（`md`）驱动。
`/chosen/memory-map` 的 `RAMDisk` 支持虽然还在（会建 `/dev/mdX`），但没有可挂载的文件系统，
所以 `vfs_mountroot()` 不可能成功。

这不是移植缺陷，是开源 XNU 的边界：**内核可以完整跑起来，但无法进入用户态**。

## 这一段修掉的阻塞点

| 阻塞点 | 现象 | 处理 | 提交 |
|--------|------|------|------|
| `ml_wait_max_cpus()` | 启动线程永久休眠，`bsd_init()` 不执行 | Apple 平台由平台 kext 调 `ml_set_max_cpus()`；本移植无 kext，DANDELION 单核直接标记已确定 | `4eb6114` |
| `pthread_init()` | `panic: pthread kernel extension not loaded` | 无 pthread kext 时把 3 个核心 shim 降级为空操作 | `395f093` |
| `tcp_tfo_init()` → `aes_encrypt_key128()` | `Kernel data abort`（`g_crypto_funcs` 为空，解引用 0x78） | DANDELION 关闭 TCP Fast Open，跳过 AES 密钥调度 | `e36af27` |

更早的修复见 `patches/xnu/non-apple-*.patch` 与 git 历史。

## 重新验证

```powershell
# 1. 构建（CI，需 workflow_dispatch + machine=dandelion）
# 2. 拉产物
powershell -NoProfile -ExecutionPolicy Bypass -File fetch_art.ps1 -Sha <sha>
# 3. QEMU 跑 60 秒并读屏
python qemu_check.py --kernel ...\mach_kernel --unstrip ...\mach.development.dandelion.unstripped `
    --seconds 60 --cmdline "-v serial=3 debug=0x14e keepsyms=1 cpus=1 -noprogress wdt=-1"
python fbdump.py 60; python fbcrop.py
```

## 打真机镜像 / 刷机

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File rebuild-device-image.ps1 -Kernel <mach_kernel>
fastboot flash boot out\boot_xnu.img
fastboot reboot
```

失败恢复：

```powershell
fastboot flash boot out\boot_backup.img
fastboot reboot
```

`rebuild-device-image.ps1` 会用 `xnu` 模式的 shim 打包，并在结束后把
`bootshim\out\Image` 还原成 `xnu-qemu` 版本，QEMU 环境不受影响。

## 已知的非致命问题

- 控制台字形有横向「重影」（每个字形画了两次），不影响阅读；
- `can't perform kext scan: no kext summary`、`IOResources: not registry member` 是无 kext 环境的正常告警；
- `wdt=-1` 目前用于规避 QEMU 下的「中断处理过久」panic，真机上可保留。
