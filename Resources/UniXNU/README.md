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
| `DarwinBoot.efi` | 26,112 B | XNU 加载器：接住 UniXNU 给的启动参数，打成 XNU 命令行，画绿条后跳内核（`UniXNU/DarwinBoot`，`tools/build_darwinboot.sh`） |
| `HelloPayload.efi` | 19,968 B | 上游自带的最小 payload，用来验证加载路径 |
| `SampleDriver.efi` | 9,216 B | 上游示例驱动 |
| `XnuTest.efi` | 14,336 B | 上游自带的 XNU 测试应用 |

来源 commit：`UniXNU/UniXNU` 与 `UniXNU/DarwinBoot` 的 `main`，2026-10-29 克隆；
工具链 CLANGPDB / DEBUG / AARCH64，EDK2 是 `/root/uefi-a04br3/Mu_Basecore`。

`DarwinBoot.inf` 依赖 `UniXNUPkg.dec`（loader 协议 GUID 在那里），所以
`tools/build_darwinboot.sh` 会把 `UniXNU/package` 一起放进 `PACKAGES_PATH`。

## 还没编的

- **`kernel.macho`** —— 需要 macOS + Xcode，是唯一真正的外部依赖；
- 启动项配置与 payload 卷：见 [`docs/M6-UNIXNU.md`](../../docs/M6-UNIXNU.md) §5。

`UniXNU.efi` 在没有内核的情况下**已经可以在真机上运行并显示菜单**（`no boot entry found`），
然后自己返回固件 —— 详见 [`docs/M6-UNIXNU.md`](../../docs/M6-UNIXNU.md)。

## 怎么装到手机上

不需要动固件：`flashinfo` 分区 +2 MiB 是 payload 槽位，`PayloadBootDxe` 的查找顺序是
**FAT → flashinfo → FV**。

```powershell
python tools\put_flash_payload.py Resources\UniXNU\UniXNU.efi -o out\payload.bin
adb push out\payload.bin /sdcard/payload.bin
adb shell su -c "dd if=/sdcard/payload.bin of=/dev/block/by-name/flashinfo bs=512 seek=4096 conv=notrunc"
```
