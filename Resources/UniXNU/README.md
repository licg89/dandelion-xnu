# Resources/UniXNU/ — 从公开源码在 Linux 上编出来的 UniXNU 产物

**M6 原本以为卡在「外部产物不存在」，这个判断是错的。** `UniXNU/UniXNU` 的
`package/UniXNUPkg` 用普通 EDK2 就能编，只需要 MdePkg（含 `BaseFdtLib/libfdt`、
`MipiSysTLib/mipisyst`）。**只有内核那一步需要 macOS + Xcode**；打包/驱动这一步在 Linux 上就够。

重建：`bash tools/build_unixnu.sh`（WSL 内跑，见脚本开头的依赖说明）。

| 文件 | 大小 | 是什么 |
|---|---|---|
| `UniXNU.efi` | 94,720 B | 启动管理器（UEFI 应用）。`package/UniXNUPkg/Application/UniXNU` |
| `UniXNUDxe.efi` | 14,336 B | DXE 驱动，装 UniXNU 的 loader 协议 |
| `UniXNURamDiskDxe.efi` | 15,360 B | 把 initrd / payload 卷变成 RAM 磁盘 |
| `HelloPayload.efi` | 19,968 B | 上游自带的最小 payload，用来验证加载路径 |
| `SampleDriver.efi` | 9,216 B | 上游示例驱动 |
| `XnuTest.efi` | 14,336 B | 上游自带的 XNU 测试应用 |

来源 commit：`UniXNU/UniXNU` `main`，2026-10-29 克隆；工具链 CLANGPDB / DEBUG / AARCH64，
EDK2 是 `/root/uefi-a04br3/Mu_Basecore`。

## 还没编的

- **`kernel.macho`** —— 需要 macOS + Xcode，是 `UniXNU.efi` 最终要启动的内核；
- **`DarwinBoot`** —— submodule `drivers/DarwinBoot`（`UniXNU/DarwinBoot`），尚未克隆与编译。

`UniXNU.efi` 在没有内核的情况下仍然可以运行并显示它自己的界面，这本身就是 M6 的一步：
先证明「固件能把 UniXNU 装起来并跑起来」，再谈内核。

## 怎么装到手机上

不需要动固件：`flashinfo` 分区 +2 MiB 是 payload 槽位，`PayloadBootDxe` 的查找顺序是
**FAT → flashinfo → FV**。

```powershell
python tools\put_flash_payload.py Resources\UniXNU\UniXNU.efi -o out\payload.bin
adb push out\payload.bin /sdcard/payload.bin
adb shell su -c "dd if=/sdcard/payload.bin of=/dev/block/by-name/flashinfo bs=512 seek=4096 conv=notrunc"
```
