# artifacts/ — 可刷机的 UEFI 镜像

这两个文件是**这一版真正刷进手机的东西**，特意放进 git，好让一次刷机可以从
一个 checkout 完整复现。构建与打包的完整环境见
[`docs/UEFI-BUILD-AND-PACKAGING.md`](../docs/UEFI-BUILD-AND-PACKAGING.md)，
换机交接见 [`docs/HANDOVER-CURRENT.md`](../docs/HANDOVER-CURRENT.md)。

| 文件 | 大小 | sha256 |
|---|---|---|
| `SILICIUM_UEFI.fd` | 2,097,152 B（= `FD_SIZE` 0x200000） | `e56a3be2…10fff3f8` |
| `boot-uefi-console.img` | 16,099,328 B | `9731a982…7741e431` |

```
e56a3be2d681baff19b05d211a5ebdcefa84268f675cfb88fb01261110fff3f8  SILICIUM_UEFI.fd
9731a9828b8ec6cbd94a96c98566d837573395f8e8f1dce5892665ce7741e431  boot-uefi-console.img
```

## 这一版是什么（2026-10-28 18:26）

**自研 framebuffer 控制台版**：`DandelionConsoleDxe` 已经进了 FV，而
`GraphicsConsoleDxe` 已经彻底摘掉（`APRIORI.inc` 里那行注释掉 —— 只在 `DXE.inc`
里注释是没用的，APRIORI 是进 FV 的第二条路）。

FVMAIN 字节级校验：

```
GraphicsConsoleDxe  (CCCB0C28-…) : 0 次      <- 已不在 FV
DandelionConsoleDxe (B7E1F4A2-…) : 2 次      <- FFS 头 + UI 段
```

boot image 的 header：

```
magic=ANDROID!  hver=2  page=2048
kaddr=0x40080000  ksize=13907597     <- 只改了这一项
raddr=0x51B00000  rsize=2060079  tags=0x47880000
kernel 段 = gzip(SILICIUM_UEFI.fd) 572,928 B
```

`kaddr` 必须等于 `[uefi_fd].base`（`Resources/Configs/dandelion.toml` =
`0x40080000`）。**`0x48880000` 是错的**：LK 只在 `0x40080000` 执行 kernel 段，
5 组对照实验见 `docs/CORRECTIONS.md` C8。

## 上机应该看到什么

| 位置 | 内容 |
|---|---|
| 屏幕上部 | `PlatformProgressDxe` 的彩色诊断（`GOP FOUND`、`FB 7EC50000`、`STRIDE 02E0` …） |
| 中部 | 5 倍放大的 `S02E0` / `UEFI`（证明直接写屏通道正确） |
| y≈1200 / y≈1300 | 洋红 + 黄色两条实心条（`DandelionConsoleDxe` 入口自证） |
| 其余 | BDS/UiApp 的文字 —— **这一版应该不再有横纹** |

如果文字清晰 → M3.5 完成 → 进 M4（`MsdcDxe` + FAT + `UniXNU.efi`）。
如果仍是横纹 → 按 `HANDOVER-CURRENT.md` §5.2 强制让 `ConOut` 只指向我们的实例。

## 刷机与恢复

```powershell
adb reboot bootloader
fastboot flash boot D:\aios\dandelion-xnu\artifacts\boot-uefi-console.img
fastboot reboot
```

恢复（刷完卡住 / 黑屏 / 重启循环）：长按电源键 10 s 强制关机，然后
电源 + 音量下 进 fastboot：

```powershell
fastboot flash boot D:\aios\artifacts\boot_backup.img
fastboot reboot
```

> `boot_backup.img`（64 MiB 原厂 boot 分区 dump）**没有**进 git：太大，而且它是
> 本机设备的原厂内容。它留在 `D:\aios\artifacts\`，打包脚本的 `--stock`
> 默认值就指向它。

## 怎么重新生成

```bash
# 1. 构建（WSL，本地 EDK2 树；也可以直接等 CI 的 dandelion-fd artifact）
export HOME=/root
cd /mnt/d/aios/uefi-a04br3
python3 build_uefi.py -d dandelion -m 0 -r DEBUG

# 2. 打包 + 校验（读 toml 里的 base，自动断言 kernel_addr）
cd /mnt/d/aios/dandelion-xnu
python3 tools/package_uefi_bootimg.py --out artifacts/boot-uefi-console.img
```
