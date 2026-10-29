# M6：UniXNU 已经在手机上跑起来了

状态：2026-10-29 22:0x。证据是固件自己写进 `flashinfo` 的日志
[logs/boot-log-2026-10-29-unixnu.txt](logs/boot-log-2026-10-29-unixnu.txt)（3448 字节，checksum ok）。

---

## 1. 「外部产物不存在」这个判断是错的

`docs/UNIXNU-BUILD-NOTES.md` 记的构建顺序是「内核 → 包 → 固件」，而内核那一步需要
macOS + Xcode，于是 M6 被记成「卡在外部依赖」。读源码之后发现这两件事是**解耦**的：

| 步骤 | 真正需要什么 | 状态 |
|---|---|---|
| `UniXNU.efi` + `UniXNUDxe.efi` + `UniXNURamDiskDxe.efi` | Linux + EDK2 | ✅ 已编出并上机 |
| `DarwinBoot.efi` | Linux + EDK2（自带 `DarwinBoot.dsc`） | ✅ 已编出 |
| `kernel.macho` | **macOS + Xcode** | ⬜ 唯一真正的外部依赖 |

`UniXNU/UniXNU` 的 `package/UniXNUPkg` 是普通 EDK2 包，只依赖 MdePkg
（含 `BaseFdtLib/libfdt`、`MipiSysTLib/mipisyst`），`tools/build_unixnu.sh` 一条命令重建。

> 坑：`make -C BaseTools/Source/C` 在这棵树上会因为 **brotli submodule 缺失**而失败
> （只有 `BrotliCompress` 用得到它）。非 FV 包只需要 `GenFw` 和 `GenSec`：
> `make -C $EDK2/BaseTools/Source/C -j$(nproc) GenFw GenSec`。

## 2. 上机结果

安装方式没有改一行固件 —— 用 M6 设计好的 payload 槽位（`flashinfo + 2 MiB`）：

```powershell
python tools\put_flash_payload.py Resources\UniXNU\UniXNU.efi -o out\payload.bin
adb push out\payload.bin /sdcard/payload.bin
adb shell su -c "dd if=/sdcard/payload.bin of=/dev/block/by-name/flashinfo bs=512 seek=4096 conv=notrunc"
```

刷 `boot-uefi-fdt.img` 之后，日志里出现（原文是被我们的 90 列控制台挤成一行的，
这里按 UniXNU 的 UI 语义还原）：

```
PayloadBoot: using the payload from flashinfo
UniXNU  (UniXNU 0.1.0)
        built-in configuration
        no boot entry found
        drivers 0 loaded / 0 failed
        kits 0    acpi 0
  > 1. Return to firmware  [action]
    2. Restart  [action]
    3. Power off  [action]
    4. Tools  [action]
  Up/Down: select    Enter: boot    1-9: boot entry    Esc: stop timer
  Booting Return to firmware in 10 s   ... 1 s
PayloadBoot: StartImage -> Success
```

读出来的四件事：

1. **`UniXNU 0.1.0` 真的执行了** —— 版本号、菜单、倒计时、按键提示都是它自己打的；
2. **`built-in configuration`** —— 它没有找到 `UniXNU.toml`，用了内建默认配置；
3. **`no boot entry found`** —— 没有任何启动项，因为没有内核、没有 payload 卷；
4. **`Return to firmware` 之后 `StartImage -> Success`** —— 它是**自己正常退出**的，
   不是崩溃。这顺带证明了我们这条 `LoadImage` + `StartImage` 路径对真实应用是通的。

## 3. 日志为什么能记下这些

`PayloadBootDxe` 现在刷两次日志：第一次在 payload 之前（payload 可能卡住，那次必须写），
第二次在 `StartImage` 返回之后。这一次两行都在，正好说明 UniXNU 返回了。
如果哪天它卡在菜单里不返回，日志会停在 `using the payload from flashinfo`——
那本身也是一个明确的信号。

## 4. 已知的呈现问题：80 列 vs 90 列

UniXNU 的 UI 假定的控制台宽度是 **80 列**，每行补齐到 80 个字符再输出，靠终端自动换行。
我们的 `DandelionConsoleDxe` 是 **90 列**（用满 720 px），所以永远不会在 80 处换行，
整屏就被挤成一行。

两个修法，各有代价：

| 方案 | 代价 |
|---|---|
| 把 `PcdConOutColumn` / `QueryMode` 改成 80 | UniXNU 排版正常；我们自己的日志每行少 10 个字符 |
| 保持 90，让 UniXNU 读 `QueryMode` 自适应 | 需要确认 UniXNU 是否真的读 `Mode` 再决定列数 |

不是 M6 的阻塞项，但一旦开始看 XNU 的输出就必须先解决——XNU 的控制台也是按列宽排版的。

## 5. 还差什么（按顺序）

1. ✅ `UniXNU.efi` 能跑（本文）；
2. ✅ `DarwinBoot.efi` 已编出（26,112 B，`tools/build_darwinboot.sh`）。坑：`DarwinBoot.inf` 依赖 `UniXNUPkg.dec`，`PACKAGES_PATH` 要带上 `UniXNU/package`；
3. ⬜ **给它一个启动项**：`UniXNU.toml` 里写一条 `boot entry`，指向 DarwinBoot + 内核；
4. ⬜ **payload 卷**：`UniXNURamDiskDxe` 可以从内嵌镜像做 RAM 盘，
   或者我们在 `flashinfo` 里建 FAT 卷（见 `docs/RESUME-1029.md` 的待办）；
5. ⬜ **`kernel.macho`**：唯一真正需要 macOS 的东西。

第 3、4 步可以先用一个**假内核**验证：DarwinBoot 收到控制权后会打
`DARWINBOOT: command line:` 并在顶部画一条**绿条**再跳转。只要看到绿条，
就说明「UniXNU → DarwinBoot → 跳转」整条链路是通的，内核可以最后再补。

---

## 6. 第二个里程碑：FAT 挂上了，UniXNU 读到了配置，DarwinBoot 起来了

上一节说 `no boot entry found` 是因为**没有卷可读**。这一轮把 1 MiB 的 FAT12 卷写进
`flashinfo` 开头（`tools/mk_unixnu_esp.sh`），里面放 `\EFI\UniXNU\` 四个文件，
于是同一次启动里三件事一起成立：

日志（`flashinfo+1 MiB`，[logs/boot-log-2026-10-29-fat-ok.txt](logs/boot-log-2026-10-29-fat-ok.txt)）：

```
INHERIT OK
BLOCK OK n=44
FAT OK                                    ← M4 最初的判据，第一次诚实地成立
PayloadBoot: device tree 127044 bytes published
PayloadBoot: firmware log published as an EFI configuration table
```

面板（[images/m6-unixnu-menu.png](images/m6-unixnu-menu.png)）：

```
        ... dandelion   (UniXNU 0.1.0)
        config: \EFI\UniXNU\UniXNU.toml          ← 我们的配置被读到了
      > 1. DarwinBoot (dandelion)  [efi]        ← 我们的启动项，3 秒后自动选中
        2. Return to firmware  [action]
        3. Restart  [action]
        ...
        "DarwinBoot (dandelion)" returned: Success   ← DarwinBoot 被加载、执行、返回
        drivers 0 loaded / 0 failed    kits 0    acpi 0
```

所以链路现在是：

```
固件 → DandelionEmmcDxe BlockIo → 44 个句柄 → 分区 → FAT（flashinfo 里的 1 MiB）
   → PayloadBootDxe 从 flashinfo 槽加载 UniXNU.efi
   → UniXNU 读到 \EFI\UniXNU\UniXNU.toml
   → 选中 DarwinBoot.efi → LoadImage/StartImage → 返回 Success
```

**M6 的「加载并启动 UniXNU」和「给它一个启动项」两步都成立。** 日志只有 686 字节
（第一次 flush），因为 UniXNU 起完 DarwinBoot 后回到自己的菜单等人按键，
`StartImage` 还没返回 —— 面板底部那条**橙色** payload 横条就是证据：
`PayloadBootDxe` 还停在「正在找 payload」的颜色里。

### 6.1 DarwinBoot 为什么是 returned 而不是跳转

`DarwinBoot.efi` 的职责是「接住参数 → 打 `DARWINBOOT: command line:` → 画绿条 →
关 MMU → 跳内核」。没有内核它就没有可跳的目标，于是正常返回。
所以**绿条不是失败**，它是「真的走到跳转那一刻」的标志，而下一次需要给它一个内核。

### 6.2 标题行左边被吃掉：改 80 列**没有**修好，列宽不是原因

面板上图里标题行是 `..u dandelion` / `.nfig:` —— 左边被吃掉。当时的假设是「UniXNU 按
80 列排版并补齐到 80，而我们是 90 列」。于是把 `DANDELION_COLS` 与 `PcdConOutColumn`
都改成 **80**（`DrawGlyph` 一直是 `Column * 8`，所以只影响换行位置、`QueryMode()` 和
光标夹取，渲染没动）。

**结果：截断一模一样（IMG_5769）。** 所以这个假设是错的。实测两个数据点：

| 行 | 完整文本 | 可见部分 | 少掉 |
|---|---|---|---|
| 1 | `UniXNU dandelion   (UniXNU 0.1.0)` | `..u dandelion   (UniXNU 0.1.0)` | ~6 字符（≈48 px） |
| 2 | `config: \EFI\UniXNU\unixnu.toml` | `.nfig: \EFI\UniXNU\unixnu.toml` | ~2 字符（≈16 px） |

**越长的那行少得越多** ⇒ 这些行的绘制起点 `x < 0`，而且偏移随行长变化 —— 典型的
「按一个比实际更宽的虚拟宽度右对齐」。而菜单项（`1. DarwinBoot (dandelion) [efi]`）是完整的，
说明只有标题/状态这一类行走了另一条排版路径。

由此可以确定：**UniXNU 的 UI 不是走我们的 ConOut**，它有自己的字体
（`UniXNULib/Font.c`）和绘制器，直接画 GOP。所以控制台列数改多少都不影响它；
真正的变量在 **GOP 模式的几何**：

- `HorizontalResolution = 720`（可见宽度）
- `PixelsPerScanLine = 736`（扫描行 = stride 2944 / 4）

差值正好 16 px = 2 个 8 px 字符，和行 2 丢失的量吻合。下一步该查的是
`GopFramebuffer` / `SimpleFbDxe` 报出去的这两个值，以及 UniXNU 用哪个算虚拟宽度 ——
如果它用 stride 当宽度再右对齐，长行就会被推到屏幕外。

不是功能阻塞项：菜单、启动项、`returned: Success` 都正常。但**在看 XNU 自己的控制台之前
必须解决**，因为 XNU 也会按它看到的宽度排版。
### 6.3 让日志能记到 UniXNU 自己的话

UniXNU 的 **UI** 不走我们的 ConOut（它有自己的字体、直接画 GOP），所以列宽改不动它。
但它的 **`UniXNULog` 走 ConOut** —— 也就是说，**只要它肯返回，它说的话就能进日志**。

问题在于它起完一个启动项后会停在菜单等人按键，`StartImage` 不返回，`PayloadBootDxe`
的第二次 flush 就永远轮不到。于是把配置改成：

- `[[entry]]` 第 1 项 = `action = "exit"`（超时 3 秒自己返回固件）→ 第二次 flush 生效；
- DarwinBoot 挪到第 2 项（它已经被证明能加载并返回 Success，再证明一次不值得用日志通道去换）；
- 新增 `[[driver]] path = "UniXNUDxe.efi"` —— 这正是 `Boot.c:60` 那句
  `xnu: no loader protocol installed; UniXNU does not provide one` 对应的驱动，
  面板上的 `drivers 0 loaded / 0 failed` 就是「一个都没配」的意思。

这一步只改 U 盘（FAT 卷），**不用重编固件**。
### 6.4 `UniXNUDxe` 要的是「固件卷里的 UniXNU.efi」

第一版把 `[[driver]] path = "UniXNUDxe.efi"` 挂上去之后，日志（第一次能记到 UniXNU 的话了）：

```
UniXNUDxe: application 9B665204-A133-47A2-9396-9E2B370A4E25 not found in any FV
config error: driver \EFI\UniXNU\UniXNUDxe.efi: start failed: Not Found
drivers 0 loaded / 1 failed    kits 0    acpi 0
```

那个 GUID 就是 **`UniXNU.inf` 的 `FILE_GUID`**，同时是 `UniXNUPkg.dec` 里的
`PcdUniXNUApplicationFile`。也就是说**上游的部署方式是「UniXNU 全家编进固件卷」**，
`UniXNUDxe` 自己去 FV 里按 GUID 找并拉起 `UniXNU.efi` —— 我们用 `flashinfo` 槽放 payload
只是权宜之计。

所以这一版把 `UniXNU.efi` 作为 **FFS 文件**塞进 `dandelion.fdf` 的 `[FV.FvMain]`：

```
FILE APPLICATION = 9B665204-A133-47A2-9396-9E2B370A4E25 {
  SECTION PE32 = dandelionPkg/UniXNU/UniXNU.efi
  SECTION UI = "UniXNU.efi"
}
```

它是 **application**，DXE 不会自动调度它，只会被按 GUID 找到再启动 —— 正是我们要的。
`UniXNUPkg` 本身不 vendoring 进仓库：它由 `tools/build_unixnu.sh` 在树外编译，
产物复制到 `Platforms/Xiaomi/dandelionPkg/UniXNU/`（和 `FdtBlob/dandelion.dtb` 同一个套路）。
### 6.5 结果：`drivers 1 loaded / 0 failed`

`UniXNU.efi` 进 FV 之后再跑一次，日志（[logs/boot-log-2026-10-29-unixnu-fv.txt](logs/boot-log-2026-10-29-unixnu-fv.txt)）：

```
PayloadBoot: using the payload from flashinfo
UniXNU dandelion  (UniXNU 0.1.0)
config: \EFI\UniXNU\unixnu.toml
drivers 1 loaded / 0 failed    kits 0    acpi 0       ← 之前是 0 loaded / 1 failed
> 1. Return to firmware  [action]
  2. DarwinBoot (dandelion)  [efi]
  3. Restart  [action]
Booting Return to firmware in 3 s ... 1 s
PayloadBoot: StartImage -> Success
```

`UniXNUDxe: application ... not found in any FV` 和 `config error:` 两行都消失了。
也就是说 **`UniXNUDxe` 已经能提供 XNU loader 协议**（`Boot.c` 里那句
`xnu: no loader protocol installed` 不再成立）—— 只是还没有内核可以交给它。

### 6.6 现在缺的只剩内核

```
固件 → eMMC BlockIo → 分区 → FAT(flashinfo) + UniXNU.efi(FV)
   → UniXNU → UniXNUDxe 装上 loader 协议 → DarwinBoot 可选
   → [内核]  ← 只剩这一格
```

两条路：

1. **`kernel.macho`**：需要 macOS + Xcode（`./unixnu build kernel`）；
2. **Apple IPSW 的 kernelcache**：不需要编译，但要给 `UniXNU-sheng` 那样的
   SoC/device profile 做一份 `mt6765` 的，工作量在「设备描述」而不是「编译」。
---

## 7. 内核不是「拿不到」，是「没去排过队」

前面几节把「内核需要 macOS」当成了硬阻塞。**大部分是错怪了它**：

仓库里一直有整套内核 CI —— `tools/ci/{apply-patches.sh,apply-board.sh,build-xnu.sh}`、
`patches/*.patch`、`config/MASTER.arm64.dandelion`、`.github/workflows/build-xnu.yml`
（`runs-on: macos-14`，跑完把 `mach_kernel` 当 artifact 传上去）。
**这个 workflow 从来没有被 dispatch 过**，所以「没有内核产物」不是因为拿不到，
是因为没人点它。

2026-10-29 22:47 第一次点起来（run `40894367790`，`machine=dandelion`、
`kernel=DEVELOPMENT`），前置步骤全部成功：

```
Set up job              completed/success
actions/checkout@v4     completed/success
Toolchain               completed/success      ← macos-14 / Xcode 版本在这里打出来
Cache XNU source        completed/success
Fetch XNU xnu-8796.141.3 completed/success
Reset XNU source        completed/success
Apply build patches     completed/success
Copy board files        completed/success
Build kernel            in_progress
Upload mach_kernel      pending
```

取件脚本：`tools/ci/fetch_kernel.ps1`（轮询 → 下载 artifact → 解出 `mach_kernel` →
落到 `Resources/UniXNU/mach_kernel`）。凭据直接走 `tools/ci/gh.ps1` 从
Windows 凭据管理器取，**不需要 `gh auth login`**。

```powershell
powershell -File tools\ci\fetch_kernel.ps1                 # 等最新一次 build-xnu
powershell -File tools\ci\fetch_kernel.ps1 -RunId 40894367790
```

### 7.1 存储布局已经为内核腾好位置

内核是几十 MB，原来 1 MiB 的 FAT 装不下。`flashinfo`（16 MiB，Android 不用）现在是：

```
0      .. 14 MiB   FAT 卷：\UniXNU.efi、\EFI\UniXNU\*（配置/驱动/DarwinBoot）、以及内核
15 MiB .. 15 MiB+  固件日志块（128 扇区）
```

`PayloadBootDxe` 的 `LOG_PARTITION_OFFSET` 从 2048 改成 30720（15 MiB），
`tools/mk_unixnu_esp.sh` 从 1 MiB 改成 14336 KiB。`\UniXNU.efi` 放在卷根 ——
那是 `PayloadBootDxe` 的第一个候选名，于是启动管理器走「FAT 优先」这条路，
不再依赖 `flashinfo + 2 MiB` 的 payload 槽（那块现在在卷里面了）。

`extract_log_block.py` 是按扇区边界扫 magic 的，所以日志换位置不需要同步改任何东西。
---

## 8. 走到哪儿了：DarwinBoot 真的在跑它的逻辑

加了 2 秒周期 flush 之后，日志从 679 → 2873 → 9623 → 9384 字节，payload 里发生的事第一次能完整读出来。
每一步的错误码都是 DarwinBoot 自己给的，一路在往前挪：

| 轮次 | DarwinBoot 的输出 | 说明 |
|---|---|---|
| 1 | （没有输出，`returned: Unsupported`） | 找不到 `gUniXNULoaderProtocolGuid` —— 提供者是 **DarwinBoot 自己**，得先把它作为驱动装起来 |
| 2 | `DARWINBOOT: loader v2 installed EL1` | 驱动入口跑到了，协议装上了（但那次卡在读内核） |
| 3 | 仍无输出，read bar 全暗 | 进度条判据写错了（只在跨 MiB 时重绘） |
| 4 | read bar **整条变绿** + `boot refused: Not Found` | **14.8 MB 内核在我们的驱动里读完了**；缺 `<kernel>.adt` |
| 5 | `boot refused: Out of Resources` | ADT 已经在卷里，过了 `ReadFile(.adt)`；卡在 `AllocateWindow()` |

现在的现场：

```
DARWINBOOT: loader v2 installed EL1
drivers 2 loaded / 0 failed
Booting XNU (dandelion) in 3 s / 2 s / 1 s
DARWINBOOT: boot refused: Out of Resources; firmware retained
```

### 8.1 下一个具体要补的东西：`window-size`

`bootshim/adt.c` 生成的 ADT 里有 `model` / `memory-map` / `cpus` / `timebase-frequency` /
`arm-io` / `uart0`，但**没有 `window-size`**。而 DarwinBoot 是这样用的：

```c
AdtNode (Tree, TreeSize, &Offset, 0, 3, &RamdiskOffset, &TrustCacheOffset,
         &AddonOffset, &WindowSize, &KeepPacked);
...
Status = AllocateWindow (&Kernel, WindowSize, &Address);   // WindowSize = 0 → Out of Resources
```

所以下一步就是在 ADT 里（`chosen` 节点，或 DarwinBoot 的 `AdtNode` 搜索的那个位置）
补 `window-size` —— 一个 32 位字节数，比如 256 MiB。顺带可能要补
`ramdisk` / `trustcache` / `addon` 的偏移（否则它们保持 `MAX_UINTN`；只有在 `KeepPacked` 时才致命）。
---

## 9. 生态现状核实：MTK 的边界到底在哪（2026-10-30 00:3x）

前面几节我两次把话说得不准，这里按仓库里的证据改正。

### 9.1 事实

`UniXNU/UniXNU` 的 `devices.toml` 里 **MTK 设备不止一台**：

```
MT8167  MT6833  MT6991  MT6765  MT6878  MT6768  MT6835  MT6993
```

其中 `MT6765` 那一条是 `name = "dandelion" / repository = "licg89/UniXNU-dandelion"`，
**就是我们自己的仓库**（licg89 = 用户本人），只是登记过，不是上游在维护。

真正的一手 MTK 实现在 **mouseos**（这套生态的作者）名下，而且做的就是 MTK：

| 仓库 | 说明（原文） |
|---|---|
| `uefi-a04br3` | UEFI firmware for Benesse TAB-A04-BR3 (**MT8167**) —— 我们的固件就是 fork 它的 |
| `UniXNU-a04br3` | UniXNU device support for a04br3 (MT8167) *(work in progress)* |
| `UniXNU-mt8167-common` | 共享 MT8167 支持 |
| `NativeMsdc` | **MediaTek MT8167 MSDC eMMC block storage driver for UniXNU** |
| `xnu_kernel_benesse_a04br3` | WIP: XNU kernel for a04br3 (MT8167). Experimental; **watchdog boot loop, OS boot incomplete** |
| `xnu_bootshim_benesse_a04br3` | 同上，**OS boot incomplete** |
| `xnu_device_benesse_a04br3` | 同上，**OS boot incomplete** |

社区还有 `Dracen-W/UniXNU-merlinx`（MT6768/6769z，无「已启动」声明）和
`stanveyjiang/UniXNU-camellia`（MT6833，无描述）。

### 9.2 所以正确的说法是

- **不是「MTK 不行」** —— 内核能被加载、能进 DarwinBoot，这条路在 MTK 上是通的（我们自己已经走到 `boot refused: Out of Resources`）；
- **也不是「MTK 已经启动了 iOS」** —— 参考实现（MT8167）在自己的仓库描述里写着 **"OS boot incomplete"**、watchdog boot loop；
- 真正被 UniXNU 跑到 userland 的是**高通**平台（sheng SM8550、umi SM8250、picasso 等）。

**因此本项目要瞄准的靶子是：让 XNU 在这台 MT6765 上真正跑起来（打出自己的早期输出）。
这件事在 MTK 上目前没人做到过 —— mouseos 的 MT8167 也停在 "OS boot incomplete"。**
这不是「iOS 桌面」，但它是这条路上一个真实、有边界、可能被跨过的台阶。

### 9.3 对我们最直接有用的两个上游资产

1. **`mouseos/NativeMsdc`** —— MT8167 的 MSDC eMMC 驱动。我们的 `DandelionEmmcDxe` 是手写的，
   在自己的硬件上已经验证过（`FAT OK`）；但长期应该对齐它的形态，因为「存储驱动」在上游是有位置的。
2. **`mouseos/UniXNU-a04br3` 的仓库结构** —— `board.toml configs drivers package patches scripts unixnu.lock`。
   对比我们自己的 `licg89/UniXNU-dandelion`（line A 时代，缺 `package/` 和 `unixnu.lock`），
   差别说明**我们的设备仓库还没迁移到当前 UniXNU 的格式**。

### 9.4 两条路，都要走

| 路线 | 内容 | 性质 |
|---|---|---|
| 快路 | 给 ADT 补 `window-size`，拿到绿条、看 XNU 是否打印 | 小时级，继续手工**
| 正路 | 把 `UniXNU-dandelion` 迁移到当前设备仓库格式，用 `./unixnu build payload` 生成 payload（内核 + `.adt` + kexts + 正确的 `window-size` 都在里面） | 天级，一劳永逸 |

`unixnu` 在 Linux 上是能跑内核那一步的（`scripts/linux/setup-toolchain.py`），
只有 `--skip-setup` 和 `seed-data-grow` 需要 macOS 的 `hdiutil`。
也就是说**「必须 macOS」这条限制比我先前说的更宽松**。