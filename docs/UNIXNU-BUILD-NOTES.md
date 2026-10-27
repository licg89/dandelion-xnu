# UniXNU 构建流程要点（读 https://unixnu.com/zh/building 整理）

## 1. 他们的编排方式

有一个 `unixnu` 命令行驱动，用 **defconfig** 描述设备，而不是手写一堆命令：

```sh
git clone --recurse-submodules https://github.com/UniXNU/UniXNU
git clone --recurse-submodules https://github.com/UniXNU/UniXNU-sheng
git clone --recurse-submodules https://github.com/UniXNU/uefi-sheng
cd UniXNU
./unixnu defconfig sheng_defconfig -D ../UniXNU-sheng
./unixnu fetch
./unixnu status --json          # 显示 profile / 版本 / 补丁顺序 / 输出目录
```

输出统一在 `out/<device>/<os_profile>/<config>/`（格式 2）。

**对我们的意义**：`dandelion` 若要走到底，最终应当是一个 `UniXNU-dandelion` 设备仓库 +
`uefi-dandelion` 固件仓库，而不是我们现在的散装脚本。短期不必照搬，但目录约定值得对齐。

## 2. 构建顺序（四步，前一步成功才做下一步）

| 步骤 | 命令 | 产物 | 平台要求 |
|---|---|---|---|
| 内核 | `./unixnu build kernel` | `<out>/kernel/kernel.macho` | **macOS + Xcode** |
| 包 | `./unixnu build package` | `AARCH64-DEBUG/UniXNU.efi` | 任意 |
| 固件 | `./unixnu build firmware` | UEFI 仓库里的 FD | **Linux 可编 EDK2** |
| 根卷/载荷/镜像 | `build userdata` / `build payload` / `bootimg` | `payload` FAT 卷、`boot.img` | userdata 需 macOS 的 `hdiutil`/`diskutil` |

## 3. 最关键的：固件 DSC/FDF 必须包含哪些模块

原文：

> 打包启动镜像前，运行 `./unixnu build firmware --uefi ../uefi-<device>` 构建设备固件。
> **DSC 和 FDF 必须包含 UniXNU、UniXNUDxe、DarwinBoot 和负载卷驱动。**
> Mu-Silicium 的 FDT initrd 路径使用共享的 `UniXNURamDiskDxe`，并需要
> **`DiskIoDxe` 和 FAT 支持**。

→ 我们现在的 `dandelionPkg` 只有基础平台模块，**这四类都还没有**：
1. `UniXNU.efi`（启动管理器，作为 DXE 驱动或从卷加载）
2. `UniXNUDxe`
3. `DarwinBoot`（XNU 加载器）
4. 负载卷驱动：`UniXNURamDiskDxe` + `DiskIoDxe` + FAT

**这是 `dandelionPkg` 与"能真正引导 XNU"的固件之间的全部差距**，也是下一步该补的清单。

## 4. CI 做法（可直接照搬）

- 内核步骤需要 **macOS runner**（调用 Xcode）；我们的 `build-xnu.yml` 已经是这条
- **Data seed 步骤需要 `hdiutil` / `diskutil`**，也只能在 macOS 上
- 他们的 macOS workflow **只发布脱敏文本日志，不上传镜像**；job 跑在 `macos-26`，
  Xcode 版本作为 workflow 输入（`DEVELOPER_DIR`）
- 板级需要平台种子时加仓库 secret `UNIXNU_UNIQUE_CHIP_ID`
- fork 仓库 → 启用 Actions → 选设备仓库/ref/defconfig/Xcode 版本/步骤 → 看 step summary

→ 我们的分工是对的：**内核在 macOS runner，固件（EDK2）在 Linux runner**。
新加的 `.github/workflows/build-uefi-dandelion.yml` 与之一致。

## 5. 对我们的行动清单（按顺序）

1. 先让 `MT6765Pkg` + `dandelionPkg` 在 Linux runner 上**编出 `.fd`**（已加 workflow）
2. 再往 `dandelionPkg` 的 DSC/FDF 里补 §3 的六个模块
   （`UniXNUDxe`、`DarwinBoot`、`UniXNURamDiskDxe`、`DiskIoDxe`、FAT、`UniXNU.efi` 入口）
3. 然后才谈"刷进去看 SEC 活不活"
4. 根卷/载荷阶段需要 macOS 工具，届时用 macOS runner
