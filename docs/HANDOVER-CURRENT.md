# 换机交接（2026-10-28 最终状态）

> 这份文件是**唯一入口**。换电脑后先读它，再读文末列出的三份历史文档。
> 所有内容都已推到 GitHub，`main` 分支即最新。

---

## 1. 一句话现状

固件已能在 Redmi 9A（dandelion / MT6762G）真机上从 **SEC → PEI → DXE → BDS** 完整跑起来，
GOP 可用、framebuffer 参数全部实测确认；**唯一未完成的是让 UEFI 控制台的文字正常显示**
（现在能画，但按 720 的 pitch 画到 736 的面板上，糊成横纹）。

---

## 2. 里程碑

| 里程碑 | 状态 | 证据 |
|---|---|---|
| M1 编出 FD | ✅ | GitHub Actions `build-uefi-dandelion.yml` |
| M2 SEC / PEI / DXE | ✅ | 绿带 / 蓝带 / 屏幕文字 |
| M3 GOP + ConOut 到面板 | ✅ | `GOP FOUND` + `GOP 720 OK`，BDS 文字确实出现在面板上 |
| M3.5 UEFI 文字可读 | 🔄 | 见第 5 节，只差把 `GraphicsConsoleDxe` 摘掉 |
| M4 eMMC(FAT) + `UniXNU.efi` | ⬜ | 阻塞：MT6357 vs MT6358 的 PMIC/eMMC 上电时序 |

---

## 3. 关键实测数据（不要再猜）

| 项 | 值 | 来源 |
|---|---|---|
| FD 加载基址 | **`0x40080000`** | LK 只在这执行 kernel 段；5 组对照实验，见 `docs/CORRECTIONS.md` C8 |
| `PcdSystemMemoryBase` | `0x48000000` | DDR 窗口起点 |
| framebuffer 基址 | **`0x7EC50000`** | `bootshim/fb.h`，每轮开机都在用 |
| framebuffer stride | **`2944` 字节（736 像素）** | 同上；用 2944 写才清晰 |
| 分辨率 / 格式 | 720×1600 / 32bpp BGRA | 同上 |
| GOP 报告的 pitch | `0x02E0` = 736 | 屏幕实测（5 倍放大字） |
| 内核参数 | `raddr=0x51b00000`, `tags=0x47880000`, `page=2048` | 不要改 |

---

## 4. 最近一次上机的确切画面（重要）

屏幕从上到下：

1. 白 / 绿 / 黄 / 品红 / 橙色小字 —— `PlatformProgressDxe` 直写 framebuffer 的诊断
   （`DANDELION UEFI`、`DXE OK`、`CONOUT YES`、`FD 40080000`、`GOP FOUND`、
   `GOP 720 OK`、`MODE 0`、`FB 7EC50000`、`STRIDE 02E0`、`COLS 0050 ROWS 0019`、`CUR`）
2. 黄色 `S02E0` + 白色 `UEFI` —— 5 倍放大（每字 40×80），证明我们的写屏通道绝对正确
3. **一片白色横条纹** —— UEFI 控制台（BDS/UiApp）的文字，被错误 pitch 渲染成斜纹
4. **底部一条洋红 + 一条黄色实心条**（y≈1200 / y≈1300）—— `DandelionConsoleDxe`
   入口处的自证标记

**两条都出现** ⇒ `DandelionConsoleDxe` 确实被调度并执行了 `ClearScreen` + 安装协议。
但 BDS 的文字仍然是歪的 ⇒ `ConSplitter` 底下还有第二个 console 设备在抢输出，
几乎可以确定是 `GraphicsConsoleDxe` 仍在 FV 里。

> 注意：`COLS 0050 ROWS 0019`（80×25）是 `PlatformProgressDxe` 在**新控制台接管之前**
> 读到的旧状态，不代表新控制台失败。新控制台自己是 90×100。

---

## 5. 下一步（按顺序做，都很小）

### 5.1 把 `GraphicsConsoleDxe` 从 FV 里摘掉（**改的是 `APRIORI.inc`，不是 DSC**）

> 2026-10-28 实测更正：原先以为「DSC 里还在编，FV 可能仍包含它」——
> 不对。DSC 的 `[Components]` 只负责**编译**，往 FV 里放模块的是 FDF。
> 而 `GraphicsConsoleDxe` 进 FV 的路径是 `Include/APRIORI.inc` 的 APRIORI 列表
> （`DXE.inc` 里那行之前已经被注释，但 APRIORI 是第二条路）。
>
> 判定方法（不用猜）：构建后直接在 `Build/.../FV/FVMAIN.Fv` 里搜该驱动的文件 GUID
> `CCCB0C28-4B24-11d5-9A5A-0090273FC14D`。
>
> | APRIORI 那行 | FVMAIN 里搜到 GUID |
> |---|---|
> | 保留 | 1 次（两个 console 同时存在） |
> | 注释掉 | 0 次 |

- 改 `Platforms/Xiaomi/dandelionPkg/Include/APRIORI.inc`，把最后那条
  `INF MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf`
  注释掉（**不要**去动 DSC：它在 `SiliciumPkg.dsc.inc` 的 `[Components]` 里，
  是共享文件，编出来不放进 FV 也无害）
- 验证：CI 构建后确认 FV 中已无该 GUID

做完后 `ConSplitter` 只剩我们的控制台，BDS 的文字应立刻变清晰。

### 5.2 如果 5.1 还不够

在 `DandelionConsoleDxe` 里主动屏蔽：安装完协议后，
`gBS->LocateHandleBuffer(ByProtocol, &gEfiGraphicsOutputProtocolGuid, ...)` 并
`DisconnectController` 掉其他 console 设备；或直接把 `gST->ConOut` 指向我们的实例。

### 5.3 之后才是 M4

`MsdcDxe` + FAT，让 BDS 能找到 `\EFI\BOOT\BOOTAA64.EFI`（即 `UniXNU.efi`）。
硬骨头是 **dandelion 用 MT6357，仓库里继承的是 MT6358** 的 PMIC 驱动，
eMMC 的 `vmmc`/`vemmc` 上电时序对不上。

---

## 6. 构建 / 刷机流程（可直接复制）

构建在 **GitHub Actions**，本地不需要 EDK2：

```
push 改动（Silicon/MediaTek/MT6765Pkg/**、Platforms/Xiaomi/dandelionPkg/**、
Resources/** 或 workflow 文件）→ ~7 分钟 → artifact dandelion-fd
```

下载 artifact（需要 GitHub token）：

```powershell
$cred = "protocol=https`nhost=github.com`n`n" | git credential fill 2>$null
$tok = (($cred | Select-String "^password=") -replace "^password=","").Trim()
$h = @{ Authorization = "Bearer $tok"; Accept = "application/vnd.github+json" }
$r = Invoke-RestMethod "https://api.github.com/repos/licg89/dandelion-xnu/actions/workflows/build-uefi-dandelion.yml/runs?per_page=1" -Headers $h
# success 后：artifacts[0].archive_download_url → Expand-Archive → 找到 SILICIUM_UEFI.fd
```

打包（**必须带 `--kernel-addr 0x40080000`**）：

```powershell
python bootshim\patch_boot.py out\boot_backup.img <fd> Resources\DTBs\dandelion.dtb out\boot_x.img --kernel-addr 0x40080000
# 输出固定 16099328 字节
```

刷机（`fastboot boot` 在本机不可用，必须 `flash boot`）：

```powershell
adb reboot bootloader
fastboot flash boot out\boot_x.img
fastboot reboot
```

**测试完必须恢复 Android**：

```powershell
fastboot flash boot out\boot_backup.img
fastboot reboot
# 等 sys.boot_completed=1
```

手动进 fastboot（死机时）：长按电源 10 秒关机 → 音量下 + 电源。

---

## 7. 踩过的坑（省时间用）

| 坑 | 说明 |
|---|---|
| FD 基址 | 用 `0x48880000` 时 FD **从不执行**；必须 `0x40080000` |
| CI 从未应用 `MuPatches` | 已修，用 `git -C "$WORK" apply`（绝对路径） |
| YAML 步骤名带冒号 | `- name: Dandelion: ...` 会让整个 workflow 解析失败、jobs 为空 |
| 新驱动必须登记两处 | DSC 的 `[Components]`（编译）**和** FDF 的 `DXE.inc`（进 FV），缺一个就构建失败或进不了 FV |
| APRIORI 也会往 FV 里放模块 | `Include/APRIORI.inc` 是进 FV 的第二条路；只注释 `DXE.inc` 摘不掉一个驱动 |
| INF 缺 `[Protocols]` | 用到的 GUID 不声明会 `lld-link: undefined symbol: gEfi...Guid` |
| `EFI_SIMPLE_TEXT_OUTPUT_MODE` | 没有 `Columns`/`Rows` 字段，要用 `QueryMode()` 取 |
| PowerShell 里写中文 | 容易乱码，文档请用 `write` 工具写 UTF-8，或脚本文件而非 here-string |
| `git apply` 补丁 | 手写 unified diff 容易坏（空行/空格），优先用 CI 里的 `sed` 改 |
| 屏幕判断 | 分辨率对 ≠ 显示对，一定要看 `FrameBufferBase` 和 `Pitch` |

---

## 8. 文件地图

| 文件 | 作用 |
|---|---|
| `Platforms/Xiaomi/dandelionPkg/Drivers/PlatformProgressDxe/` | 诊断驱动：直接写 framebuffer 的彩色文字 / 5 倍放大字 / GOP 与 ConOut 探测 |
| `Platforms/Xiaomi/dandelionPkg/Drivers/DandelionConsoleDxe/` | 自写 `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL`（8×16，90×100，stride 2944） |
| `Platforms/Xiaomi/dandelionPkg/Drivers/*/DandelionFont.h` | 8×16 点阵（由 `bootshim/font.h` 生成） |
| `Platforms/Xiaomi/dandelionPkg/Library/MemoryMapLib/MemoryMapLib.c` | 内存映射；`Display Reserved` = `0x7EC50000`（关键） |
| `Platforms/Xiaomi/dandelionPkg/Include/DXE.inc` | FV 里包含哪些 DXE 驱动 |
| `Platforms/Xiaomi/dandelionPkg/dandelion.dsc` | 组件、PCD、编译开关（`USE_CUSTOM_DISPLAY_DRIVER = 0`） |
| `Resources/Configs/dandelion.toml` | `[uefi_fd].base = 0x40080000` |
| `Resources/MuPatches/` | 上游 Mu 的补丁（CI 自动应用） |
| `.github/workflows/build-uefi-dandelion.yml` | 云端构建（含 `SimpleFbDxe` pitch `sed`） |
| `bootshim/fb.h` | 面板参数权威值 |
| `bootshim/patch_boot.py` | 打包成可刷的 boot.img |
| `tools/patch_fd_base.py` | 不重新编译就改 FD 基址 |
| `docs/CORRECTIONS.md` | FD 基址的 5 组对照实验（C7/C8） |
| `docs/HANDOVER-UEFI-1028.md` | 本轮详细日志（ConOut 实测、CI 修法） |
| `docs/NEXT-M3-GOP.md` | GOP 排查全过程 |

---

## 9. 上游与身份

- **基座**：`mouseos/uefi-a04br3`（Mu / Mu-Silicium），CI 克隆后叠加本仓库的
  `Platforms/Xiaomi/dandelionPkg`、`Silicon/MediaTek/MT6765Pkg`
- **不是** unixnu.com 那套。UniXNU 是本线的**下游**：等 M4 通了才轮到 `UniXNU.efi`
  作为 `\EFI\BOOT\BOOTAA64.EFI` 被 BDS 加载
- 设备序列号 `QS55XCPRCA9LDA5T`；工具在
  `C:\Users\li\AppData\Local\Android\Sdk\platform-tools\`

---

## 10. 接手后的第一个动作

```powershell
git pull
# 1) 把 dandelion.dsc 里的 GraphicsConsoleDxe 也注释掉
# 2) push → 等 CI → 下载 artifact → 打包 → flash
# 3) 看 BDS 文字是否变清晰（不再横纹）
```

如果清晰：M3.5 完成，直接进 M4（eMMC / FAT / UniXNU.efi）。
如果仍横纹：按 5.2 强制让 `ConOut` 只指向我们的实例。
