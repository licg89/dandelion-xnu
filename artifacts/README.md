# artifacts/ — 可刷机的 UEFI 镜像

这两个文件是**这一版真正刷进手机的东西**，特意放进 git，好让一次刷机可以从
一个 checkout 完整复现。构建与打包的完整环境见
[`docs/UEFI-BUILD-AND-PACKAGING.md`](../docs/UEFI-BUILD-AND-PACKAGING.md)。

| 文件 | 大小 | sha256 |
|---|---|---|
| `SILICIUM_UEFI.fd` | 2,097,152 B（= `FD_SIZE` 0x200000） | `4B790C1D…6CF4200D` |
| `boot-uefi-beacon.img` | 16,099,328 B | `33067AD3…964D9F8086` |

完整哈希：

```
4b790c1dfc0ff4f24a3ea156875452cdf3d8c22e0c8301626a9971666cf4200d  SILICIUM_UEFI.fd
33067ad3c246ae89864e05b48cb38d4cf77f8ff75458be1afd2b8b964d9f8086  boot-uefi-beacon.img
```

## 这一版是什么

`boot-uefi-beacon.img` = **看门狗进度信标**版本（2026-10-28 09:43 构建）。
因为 dandelion 没有 UART、eMMC 日志通道也还没写，固件唯一的可观测输出是
「从跳进 FD 到 SoC 自己复位隔了多久」：

| 复位延迟 | 含义 |
|---|---|
| ~10 s | FD 根本没被执行（还是 LK 自己的看门狗） |
| ~20 s | SEC 入口跑到了（`PlatformSecLib`），死在 `PlatformPeim` 之前 |
| ~30 s | 过了内存初始化（`PlatformPeiLib`），死在 DXE 之前 |
| ~2 s | **DXE core + dispatcher + FV 都活着**（`PlatformProgressDxe` 跑了） |
| 不重启 | 以上都没发生 |

boot image 的 header：

```
magic=ANDROID!  hver=2  page=2048
kaddr=0x48880000  ksize=13907597     <- 只改了这一项
raddr=0x51B00000  rsize=2060079  tags=0x47880000
kernel 段 = gzip(SILICIUM_UEFI.fd) 576,072 B
```

`kaddr` 必须等于 `[uefi_fd].base`（`Resources/Configs/dandelion.toml`）
以及 `MemoryMapLib.c` 的 `"UEFI FD"` 条目，三处都是 `0x48880000`。

## 刷机与恢复

```powershell
adb reboot bootloader
fastboot flash boot D:\aios\dandelion-xnu\artifacts\boot-uefi-beacon.img
fastboot reboot
```

恢复（刷完卡住 / 黑屏 / 重启循环）：长按电源键 10 s 强制关机，然后
电源 + 音量下 进 fastboot，刷回原厂 boot 分区：

```powershell
fastboot flash boot D:\aios\artifacts\boot_backup.img
```

> `boot_backup.img`（64 MiB 原厂 boot 分区 dump）**没有**进 git：太大，而且它是
> 本机设备的原厂内容，不是我们的产物。它留在 `D:\aios\artifacts\`，
> 打包脚本的 `--stock` 默认值就指向它。

## 怎么重新生成

```bash
# 1. 构建（WSL）
export HOME=/root
cd /mnt/d/aios/uefi-a04br3
python3 build_uefi.py -d dandelion -m 0 -r DEBUG

# 2. 打包 + 校验（读 toml 里的 base，自动断言 kernel_addr）
cd /mnt/d/aios/dandelion-xnu
python3 tools/package_uefi_bootimg.py --out artifacts/boot-uefi-beacon.img
```
