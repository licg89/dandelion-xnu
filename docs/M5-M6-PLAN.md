# M5 / M6：从"能刷进去"到"能交给 XNU"

写于 2026-10-29 凌晨。M4（eMMC 数据通路）还在收尾，但 M5/M6 里**不需要真机**的部分
今晚已经做了，这篇说明做了什么、还剩什么、以及需要什么输入。

---

## 1. M5 已经落地的部分

### 1.1 `PayloadBootDxe` —— 这个固件里唯一的"启动策略"

`BdsDxe` 虽然还在 FV 里（`Include/DXE.inc`），但它在这块板子上不可靠：
它一进来就会在 ConSplitter 的模式协商里死循环（`DandelionSetMode` 的注释、CORRECTIONS C10）。
所以"找 payload → LoadImage → StartImage"这件事由我们自己的驱动做：

| 顺序 | 来源 | 说明 |
|---|---|---|
| 1 | FAT 卷上的 `\UniXNU.efi` | 读完整个文件进内存再 `LoadImage`，这样不需要 DevicePath 相关的库 |
| 2 | FAT 卷上的 `\EFI\BOOT\BOOTAA64.EFI` | 标准可移动介质路径 |
| 3 | FAT 卷上的 `\xnu.efi` | 我们自己的短名字，方便手放 |
| 4 | **固件卷内嵌的 payload**（按文件 GUID） | M4 没好之前唯一能验证"装/启"链路的方式 |

结果画在 **y 1570..1596 的一条满宽横条**上（这一段没有任何别的驱动在用）：

| 颜色 | 含义 |
|---|---|
| 橙 `ff8000` | 开始找了 |
| 绿 `00ff00` | payload 被 StartImage 且返回 `EFI_SUCCESS` |
| 红 `ff0000` | 没找到，或 LoadImage/StartImage 失败 |
| 右端白色小方块 | **设备树已经作为 EFI 配置表发布**（见 1.2） |

### 1.2 `PayloadHello` —— 内嵌的自检 payload

一个 40 行的 UEFI 应用，做三件事：把同一条横条涂绿、往 ConOut 打一行、返回成功。
它被编进 FV（`FILE` 名 `7E1B3A5C-2C4D-4F6A-9B10-DA9D0E110001`），`PayloadBootDxe` 按 GUID 找到它。

**它的价值**：M4 没好、一个块设备都没有的情况下，也能证明
"FV → ReadSection(PE32) → LoadImage → StartImage → 应用真的跑了"这条链路是通的。
如果刷完横条是绿的，M5 就只剩"从 FAT 读文件"这一半没验证。

### 1.3 设备树已经进固件并发布

- `dandelion.dtb`（线 A 已经用它引导）被复制到
  `Platforms/Xiaomi/dandelionPkg/FdtBlob/dandelion.dtb`，
  在 `dandelion.fdf` 里作为 `FREEFORM` 文件嵌入，GUID 就是 `gFdtTableGuid`
  （`B1B621D5-F19C-41A5-830B-D9152C69AAE0`，DarwinBoot/UniXNU 已经在找的那个）。
- `PayloadBootDxe` 启动 payload **之前**把它读出来，校验魔数 `0xD00DFEED`，
  再 `InstallConfigurationTable`。

这样 payload 不需要知道 dtb 在哪个分区、哪个文件里 —— 它按标准方式查配置表即可。
顺便这也让"固件版本"和"设备树版本"绑在一起，不会再出现 dtb 与固件不一致的错配。

> 上传入链路的坑（记在这里，避免下次再踩）：**`APRIORI.inc` 只决定调度顺序，
> 决定"进不进 FV"的是 `Include/DXE.inc` 和 `dandelion.fdf`。** 今晚加
> `PayloadBootDxe` 时只写了 APRIORI，编出来了但 FV 里根本没有它；
> 判据是 `Build/.../FV/FVMAIN.inf` 的文件列表（它才是权威）。

---

## 2. M5 还缺什么

| 缺口 | 现在能做什么 | 需要什么 |
|---|---|---|
| FAT 读取路径没有验证过 | 代码已就位，等 M4 | eMMC 通（M4） |
| 还没有真 payload | `PayloadHello` 顶上 | **`UniXNU.efi`**：UniXNU 的 `./unixnu build package` 产物（需要 macOS/Xcode 那一步先产出 `kernel.macho`） |
| payload 需要的运行期服务 | GOP 有、ConOut 有、DTB 有、BlockIo/FAT 待 M4 | — |

---

## 3. M6：DarwinBoot → XNU 需要往 FV 里补的模块

`docs/UNIXNU-BUILD-NOTES.md` 引 UniXNU 原文档，明确写了 DSC/FDF 必须包含：
**UniXNU、UniXNUDxe、DarwinBoot、以及负载卷驱动（`UniXNURamDiskDxe` + `DiskIoDxe` + FAT）**。

对照我们现在的 `dandelionPkg`：

| 需要的模块 | 来源 | 现在的状态 |
|---|---|---|
| `DiskIoDxe` | `MdeModulePkg/Universal/Disk/DiskIoDxe` | ✅ 树里就有，**已经在 FV 里** |
| FAT | `FatPkg/EnhancedFatDxe` | ✅ 树里就有，**已经在 FV 里** |
| `UniXNURamDiskDxe` | `uefi-<device>`（UniXNU 的固件仓库） | ❌ 没有源码 |
| `UniXNUDxe` | 同上 | ❌ 没有源码 |
| `DarwinBoot` | `UniXNU-sheng` | ❌ 没有源码 |
| `UniXNU.efi` | `UniXNU`（`build package`） | ❌ 没有产物 |

**结论：M6 现在卡在"没有 UniXNU 的源码/产物"，不是卡在固件能力上。**
固件这一侧我们该做的准备（DTB 发布、LoadImage 链路、BlockIo/FAT、GOP、ConOut）今晚已经齐了。

### 3.1 拿到源码之后要改的三处（已经想清楚，直接照做）

1. `Resources/MuPatches` 或者 overlay：把 `uefi-<device>` 里的
   `UniXNUDxe`、`DarwinBoot`、`UniXNURamDiskDxe` 放到
   `Platforms/Xiaomi/dandelionPkg/Drivers/`（或者作为独立包 overlay 进树）；
2. `dandelion.dsc` `[Components]` + `Include/DXE.inc` **两处都要加**
   （今晚踩过的坑）；`DarwinBoot`/`UniXNURamDiskDxe` 通常是 DXE_DRIVER；
3. `PayloadBootDxe` 的候选名单里加 `\UniXNU.efi` 已经在第一位；
   如果 `UniXNU` 走的是"DXE 驱动 + 负载卷"路线而不是"从 FAT 加载的应用"，
   就把 `PayloadBootDxe` 降级成"只发布 DTB + 打印状态"，让 UniXNUDxe 自己接管。

### 3.2 交接信息清单（payload 需要的，逐项对照）

| 项 | 状态 | 备注 |
|---|---|---|
| 设备树 | ✅ 已作为配置表发布 | 魔数已校验 |
| 帧缓冲 | ✅ GOP 已装（`SimpleFbDxe`），另有一份"直写 2944 stride"的经验 | payload 用 GOP 即可 |
| 控制台 | ✅ 我们的 90×68 文本控制台 + 一条永久读数条 | — |
| 内存映射 | ✅ DXE 自己维护；`MemoryMapLib` 有 dandelion 的实测表 | 交接前要 `ExitBootServices` |
| 块设备 / FAT | ⏳ 等 M4 | UniXNURamDiskDxe 可以绕开它（用内存盘） |
| RTC / 复位 / 看门狗 | ✅ 都在 FV 里，看门狗已在 `PlatformProgressDxe` 里解除 | 交接给 XNU 前要确认 XNU 自己会接管 |

---

## 4. 今晚新增的、以后一直有用的东西

| 文件 | 作用 |
|---|---|
| `tools/gen_small_font.py` | 生成读数条用的 3×5 位图字体（放大 5 倍 = 15×25 px） |
| `tools/render_panel_preview.py` | **在 PC 上把整个仪表区渲染成 PNG**，布局错一眼就看见，不用刷机 |
| `tools/gen_patch_0002.py` | MSDC 的 400 行补丁由脚本生成，不再手改（手改的活不过一次构建） |
| `docs/ANALYSIS-EMMC-DATA-PATH.md` | Mu / LK / shim 三份实现的逐项对照 + 下一次上机的判读表 |
