# artifacts/ — 可刷机的 UEFI 镜像

这里的文件是**这一版真正刷进手机的东西**，特意放进 git，好让一次刷机可以从一个
checkout 完整复现。构建与打包的完整环境见
[`docs/UEFI-BUILD-AND-PACKAGING.md`](../docs/UEFI-BUILD-AND-PACKAGING.md)，
换机交接见 [`docs/HANDOVER-CURRENT.md`](../docs/HANDOVER-CURRENT.md)。

| 文件 | 大小 | sha256 |
|---|---|---|
| `SILICIUM_UEFI.fd` | 2,097,152 B（= `FD_SIZE` 0x200000） | `c46ca9ca…b5c57b` |
| `boot-uefi-stagecells.img` | 16,099,328 B | `7dc21382…caf2e` |
| `boot-uefi-console.img` | 16,099,328 B（上一版，留作对照） | `9731a982…741e431` |

```
c46ca9ca5a02ea1691a0f8a3f06630b4be1ebf74961652d4731e1d4ddbb5c57b  SILICIUM_UEFI.fd
7dc213820b7d69cfa3b3fe000985eac47ca163eb77504a54b4941cfe1fcaaf2e  boot-uefi-stagecells.img
```

## 这一版是什么（2026-10-29 00:0x）

**状态格子版。** 相对上一版改了三处，每一处都对应一个查实的 bug：

1. `DandelionConsoleDxe.c` 的 `FillRow()` 以前只写 1 行像素，`ClearScreen()` 于是
   只清了扫描行 0,16,32,… —— 面板中段那片"白横纹"就是从来没被清掉的旧内容。
   现在清满 16 行。**这一版屏幕应该是干净的**，看得见的都是驱动故意画的。
2. `MsdcImplLib.c` 的 `MsdcSay()` 以前在 `gST->ConOut == NULL` 时直接 return，
   连状态带都不画；而 `MsdcDxe` 排在控制台**之前**，所以 MSDC 一个标记都到不了屏幕。
   现在先直写 framebuffer，再看有没有 ConOut。
3. `MsdcDxe` 挪到控制台之后（`APRIORI.inc`），新增 `Resources/MuPatches/0002`
   在卡识别路径里打点。

### 怎么看屏幕（底部格子）

底部有 **8 个 80×80 的格子**（y 1400..1480，间距 88）和第二排 5 个矮格子
（y 1488..1543）。**格子存在 = 那个阶段到达过**，所以一张照片就是完整状态向量：

| 格子 | 颜色 | 含义 |
|---|---|---|
| 0 | 黄 | `MsdcDxe` 库构造函数跑到了 |
| 1 | 橙（红=失败） | GPIO122..133 复用设置完成 |
| 2 | 青（红=失败） | MSDC 时钟使能完成 |
| 3 | 白（红=缺协议） | power / PMIC 路径走完 |
| 4 | 紫 | 产生了 BlockIo（`StorageBeaconDxe`）← **M4 达成** |
| 5 | 绿 | 挂上了文件系统 |
| 8 | 绿 | msdc0 两段 MMIO 区间都解析成功 |
| 9 | 黄 | 控制器复位完成（`MsdcReset` 返回了） |
| 10 | 品红 | 识别阶段真的有一条命令上了总线 |
| 11 | 青 | CMD1 有应答（OCR bit31 置位 = 卡上电了） |
| 12 | 白 | CMD2+CMD3 过，`CardType = EmmcCard`（EmmcDxe 能绑上） |

另外：顶部第 26..33 行是"最后阶段"状态带；y 1200..1280 / 1300..1380 是控制台的
两条自证条；y 1560 是 `PlatformProgressDxe` 的白条。

boot image 的 header：

```
magic=ANDROID!  hver=2  page=2048
kaddr=0x40080000  ksize=13907597     <- 只改了这一项
raddr=0x51B00000  rsize=2060079  tags=0x47880000
kernel 段 = gzip(SILICIUM_UEFI.fd) 570,880 B
```

`kaddr` 必须等于 `[uefi_fd].base`（`Resources/Configs/dandelion.toml` = `0x40080000`）。
**`0x48880000` 是错的**：LK 只在 `0x40080000` 执行 kernel 段，见
`docs/CORRECTIONS.md` C8。

## 刷机与恢复

```powershell
adb reboot bootloader
fastboot flash boot D:\aios\dandelion-xnu\artifacts\boot-uefi-stagecells.img
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
python3 tools/package_uefi_bootimg.py --out artifacts/boot-uefi-stagecells.img
```
