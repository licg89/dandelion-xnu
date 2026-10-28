# 夜里做完了什么（2026-10-29 02:5x – 05:0x）

你睡觉这段时间没有真机可用，所以全部工作都挑**不需要刷机就能推进、而且一定有用**的：
把 M4 的判据从"猜"变成"读"，把 M5 的装载链路整条搭起来，把日志通道从"拍照"升级成
"可以带走的文本"。早上只要刷一个镜像、拍一张照片，就能接着往下走。

---

## 一句话现状

- **原始目标（第一个 `*.fd`）早就达成**，而且这一夜它变得更有用了：
  固件现在会（1）找 payload 并启动它，（2）发布设备树，（3）把整份固件日志交给 payload。
- **M4 只剩一个判读**：数据通路的怀疑对象已经从"一堆"收敛到两个，而且下一次上机会直接打印
  能区分它们的那一个数字（卡自己的 R1 里 `CURRENT_STATE`）。
- **M6 卡在外部输入**：`UniXNUDxe` / `DarwinBoot` / `UniXNURamDiskDxe` / `UniXNU.efi`
  这四样我们本地一个都没有，固件这一侧该准备的都准备好了。

---

## 早上第一件事：刷 `boot-uefi-m5-dma.img`

```powershell
# 1) 进 fastboot：长按电源 10 s 关机 → 电源 + 音量下
cd D:\aios\dandelion-xnu
& "C:\Users\li\AppData\Local\Android\Sdk\platform-tools\fastboot.exe" flash boot D:\aios\artifacts\boot-uefi-m5-dma.img
& "C:\Users\li\AppData\Local\Android\Sdk\platform-tools\fastboot.exe" reboot
```

镜像信息（这一版是唯一一次构建的产物，别和旧的混用）：

| 文件 | 大小 | sha256 |
|---|---|---|
| `SILICIUM_UEFI.fd` | 2,097,152 | `80a2d6a7…f7bae` |
| `boot-uefi-m5-dma.img` | 16,099,328 | `42752869…0bc88` |

恢复原厂：`fastboot flash boot D:\aios\artifacts\boot_backup.img`。

### 拍一张就行（这次真的随便拍）

屏幕下半部有**永久读数条**（两列五行，15×25 像素大字，写完不会消失）：

```
INTR=…   MCFG=…      LEFT=…   R1=…
FIFO=…   SSTS=…      SCMD=…   SCFG=…
DCRC=…               BLKN=…
```

再加上最底部 **y 1570 的一条满宽横条**（M5 用）：

| 颜色 | 含义 |
|---|---|
| 橙 | 正在找 payload |
| 绿 | payload 被 LoadImage + StartImage 且返回成功（**M5 链路通了**） |
| 红 | 没找到 / 启动失败（M4 没好时这是**正常**的） |
| **右端**白色小方块 | 设备树已作为 EFI 配置表发布 |
| **左端**白色小方块 | **固件日志已写进 flash 的 flashinfo 空闲区**（M4 通了才会出现） |

## 判读表（照这个看 `INTR` 和 `R1`）

**这一版的数据读先走 DMA，不是 PIO**（理由见下）。文字区会出现两类新行：

```
ddn dma ok len=512 blocks=1                  <- 数据来了，M4 直接达成
ddn dma FAIL int=… cfg=… ctrl=… ca=… fifo=…  <- DMA 也不行，附 DMA 寄存器
```

DMA 失败一次之后驱动会自动退回 PIO（`mDdnUseDma = FALSE`），所以后面的读仍然会打原来那三行
`ddn pio-read FAIL` / `ddn  cfg=…` / `ddn  ps_or=… ps_and=…`。
**一次开机就能同时拿到两条路径的证据。**

| 看到什么 | 含义 | 下一步 |
|---|---|---|
| `ddn dma ok` | **M4 达成**（数据通路是 DMA 通、PIO 不通） | 把 DMA 定为默认，写 FAT、进 M5 |
| `ddn dma FAIL` + PIO 仍 `4000` | 两条路径都拿不到数据 | 看 `ps_or`/`ps_and`：没变化 = 卡没发；有变化 = 采样/宽度 |
| `4000` + `R1` 低 4 位 `…4`（state=Tran）bit8=1 | 卡在 Tran 却仍不发数据 | 同上 |
| `4000` + `R1` 带错误位（bit22 / state≠4） | 卡拒绝了命令 | 打印 CMD7/CMD13 的 R1，回到"选中"这一步 |
| `8000` | `DATCRCERR`：数据来了但采样错 | 调采样相位 / 再降时钟 |

**为什么敢让 DMA 先上**：PIO 在这块板子上从来没搬动过一个字节（每次都是 `DATTMO` + FIFO 空），
而 `Platforms/Benesse/a04br3Pkg` 的 `SdCardBlockIoDxe` 是同一代 MTK 控制器上**真机跑通过**的实现，
它搬数据用的就是这套 GPD/BD 描述符 DMA。所以这是移植而不是发明；失败也有兜底（自动退回 PIO），
不会比现在更糟。配套的 cache 维护也一并加上了（描述符 clean、接收缓冲 invalidate）——
少了这两步，DMA 只会安静地失败。

**这一版对 M4 做了什么**（全部来自 `docs/ANALYSIS-EMMC-DATA-PATH.md` 的对照）：
卡被 CMD6 强制回 `HS_TIMING=0` + `BUS_WIDTH=1`（Android 上次把它留在 8-bit HS200，
而 CMD0 不会复位这两个字段；两条 CMD6 现在都是 best effort，失败不阻断识别），
数据阶段时钟降到 ~400 kHz（线 A 已验证的速率），`SDC_CFG` 数据超时拉满，
命令前整寄存器 W1C，补上 LK 有而我们没有的 `MSDC_PAD_TUNE*`/`MSDC_DAT_RDDLY*`/`SDC_FIFO_CFG`，
并新增 **DMA 读路径** + 数据窗内 **`MSDC_PS` 采样**。

---

## 这一夜具体做了什么

### 1. M4：把"怀疑"变成"可判读"，并补上第二条数据通路
- 读数条 10 个槽位 + 文字区 `ddn data cmd=%u r1=%08x len=%u`；
- 数据窗内 `MSDC_PS` 采样（`ps_or` / `ps_and`）—— 用来区分"卡没发"和"线没通"；
- 修好读数条自己的两个 bug（标签短于 4 字符会把值截断；行距 26 px 太挤）；
- 把"纠正卡状态"的 CMD6 改成 best effort：失败只打印 `(continuing)`，**不再阻断识别**
  （阻断识别 = EmmcDxe 不绑定 = 连证据都没有了）；
- **新增 DMA 读路径**（`MsdcDmaRead`，移植自 a04br3 的 GPD/BD 实现，含描述符校验和与
  cache 维护），DMA 失败自动退回 PIO —— 一次开机同时验证两条通路；
- 三份实现的逐项对照写进 `docs/ANALYSIS-EMMC-DATA-PATH.md`，
  包括"为什么命令全通、数据全丢"的完整解释和下一次上机的判读表。

### 2. 不用刷机就能验证布局：`tools/render_panel_preview.py`
在 PC 上把整个仪表区（读数条 + 横幅 + 点阵 + 状态格 + 白条）渲染成 PNG。
上面那两个读数条 bug 本来用它就能零成本抓到。生成效果见
`../artifacts/panel-preview.png`（也可随时重跑）。

### 3. M5：装载链路整条搭起来
- `PayloadBootDxe`：FAT 上找 `\UniXNU.efi` / `\EFI\BOOT\BOOTAA64.EFI` / `\xnu.efi`，
  读进内存 `LoadImage` + `StartImage`；找不到就退到 **FV 内嵌 payload**（按 GUID）；
- `PayloadHello`：40 行的自检应用，M4 没好也能证明"装/启"这一段是通的；
- **设备树**：`dandelion.dtb` 进 FV（`gFdtTableGuid`），启动前校验魔数
  `0xD00DFEED` 并 `InstallConfigurationTable` 发布 —— payload 不用知道 dtb 在哪。

### 4. 日志通道：从"拍照"到"可以带走的文本"
- `DandelionConsoleDxe` 现在维护一个 **64 KiB 环形文本日志**，记录**所有**经过控制台的输出
  （DEBUG 也在内，因为 DebugLib 被重定向到 ConOut）；
- 通过新协议 `gDandelionLogProtocolGuid` 导出（`Dump`：先问大小再拷走）；
- `PayloadBootDxe` 启动 payload 前把日志**复制到 `EfiRuntimeServicesData`**
  并以配置表 `gDandelionLogTableGuid` 发布 —— 因为 DarwinBoot 会在
  `ExitBootServices` 之后才读它，BootServicesData 那时已经没了。
- 下一步（等 M4）：把这 64 KiB 写进 eMMC 一个确认全 `0xFF` 的区域，
  回 Android 侧 dump 出来 —— `bootshim/msdc.c` 已经把"找一块空区域再写"的整套逻辑验证过了，
  照搬即可。

### 4b. 已经写好了：日志落到 flash（M4 一通就生效）

`PayloadBootDxe` 在启动 payload 之前会做第 4 件事：**把这份日志写进 flash**。
位置和做法完全照搬线 A 已经验证过的方案：

1. 读 LBA1 解析 GPT，**只认 `flashinfo` 分区**；
2. 目标位置 = 分区起点 + 1 MiB，长度 64 KiB；
3. **写之前先读回来判断是否空闲**：整段必须是全 `0xFF` 或全 `0x00`（厂商把这个分区
   清零过一次，所以是 `0x00`），否则一个字节都不写，直接放弃；
4. 写完再读回来比对 magic；
5. 成功就在 y 1570 横条**左端**画一个白色方块。

配套取回工具：`tools/extract_log_block.py`（已带离线自测
`tools/test_extract_log_block.py`，格式、校验和、CLI、长度全部验证过）：

```powershell
# 手机进 Android 后（需要 root）
adb shell su -c 'dd if=/dev/block/by-name/flashinfo of=/sdcard/flashinfo.img'
adb pull /sdcard/flashinfo.img
python tools\extract_log_block.py flashinfo.img --out last-boot.log --verbose
```

这样"日志"就不再是 68 行文字区的照片，而是**完整的、可 grep 的文本**。
在 M4 通之前这套代码是安全的空转：没有块设备就不会执行。

### 5. 文档
| 文件 | 内容 |
|---|---|
| `docs/ANALYSIS-EMMC-DATA-PATH.md` | Mu / LK / bootshim 三份实现对照 + 判读表 + 后续阶梯 |
| `docs/M5-M6-PLAN.md` | M5 已落地部分、缺什么；M6 需要的四个外部模块、要改哪三处 |
| `docs/CORRECTIONS.md` | C11（滚动重影的真因）、C12（读数条标签截断） |
| `docs/HANDOVER-CURRENT.md` | §5.6 / §5.7：这一轮的改动与实测寄存器 |

---

## 我踩到并记下来的两个坑（下次省时间）

1. **`APRIORI.inc` 只决定调度顺序，`Include/DXE.inc` 才决定进不进 FV。**
   `PayloadBootDxe` 编出来了、APRIORI 里也有，但 FV 里根本没有它；
   权威判据是 `Build/.../FV/FVMAIN.inf` 的文件列表。
2. **在压缩过的 FV 里用字符串搜模块是无效判据。** 之前用
   `IndexOf("pio-read") == -1` 推断"镜像里没有那段代码"，其实是 FV 压缩。
   正确做法是构建完看构建树里的源文件，或看 `BUILDLOG`。

---

## 接下来（按顺序）

1. 你刷 `boot-uefi-m5-log.img` + 一张照片 → 按判读表定 M4 的下一步（我预计 1–2 轮内出结果）；
2. M4 一通：`StorageBeacon` 立刻有 `BLOCK OK` + `FAT OK`，
   `PayloadBootDxe` 就能从 FAT 里读真 payload，同时把日志写进 eMMC（真正的日志通道）；
3. M5/M6 需要外部输入：`UniXNU` 的 `kernel.macho`（macOS）→ `UniXNU.efi`，
   以及 `uefi-<device>` 里的 `UniXNUDxe` / `DarwinBoot` / `UniXNURamDiskDxe`。
   这三处要改的地方已经在 `docs/M5-M6-PLAN.md` §3.1 写清楚了。
