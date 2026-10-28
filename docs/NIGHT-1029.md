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

## 早上第一件事：刷 `boot-uefi-m5-log.img`

```powershell
# 1) 进 fastboot：长按电源 10 s 关机 → 电源 + 音量下
cd D:\aios\dandelion-xnu
& "C:\Users\li\AppData\Local\Android\Sdk\platform-tools\fastboot.exe" flash boot D:\aios\artifacts\boot-uefi-m5-log.img
& "C:\Users\li\AppData\Local\Android\Sdk\platform-tools\fastboot.exe" reboot
```

镜像信息（这一版是唯一一次构建的产物，别和旧的混用）：

| 文件 | 大小 | sha256 |
|---|---|---|
| `SILICIUM_UEFI.fd` | 2,097,152 | `28a5a267…d9aa7` |
| `boot-uefi-m5-log.img` | 16,099,328 | `7fb490f0…d3964` |

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
| 右端白色小方块 | 设备树已作为 EFI 配置表发布 |

## 判读表（照这个看 `INTR` 和 `R1`）

| `INTR` | `R1` | 含义 | 下一步 |
|---|---|---|---|
| `1000` 附近 | — | `XFER_COMPL`：数据来了 | 看 `LEFT`=0 → **M4 达成** |
| `4000` | 低 4 位 = `…4`（state=Tran）且 bit8=1 | 卡在 Tran 却仍不发数据 | 只剩"数据线物理通路"：下一步试 host 侧 8-bit |
| `4000` | 带错误位（bit22 / state≠4） | 卡拒绝了命令 | 打印 CMD7/CMD13 的 R1，回到"选中"这一步 |
| `8000` | — | `DATCRCERR`：数据来了但采样错 | 调 `MSDC_PAD_TUNE`/`MSDC_IOCON` 采样相位 |
| 还是 `4000` 且 `R1=0` | — | 命令没应答 | 看文字区 `dandelion cmd N failed` |

**这一版对 M4 做了什么**（全部来自 `docs/ANALYSIS-EMMC-DATA-PATH.md` 的对照）：
卡被 CMD6 强制回 `HS_TIMING=0` + `BUS_WIDTH=1`（Android 上次把它留在 8-bit HS200，
而 CMD0 不会复位这两个字段），数据阶段时钟降到 ~400 kHz（线 A 已验证的速率），
`SDC_CFG` 数据超时拉满，命令前整寄存器 W1C，补上 LK 有而我们没有的
`MSDC_PAD_TUNE*`/`MSDC_DAT_RDDLY*`/`SDC_FIFO_CFG`。

---

## 这一夜具体做了什么

### 1. M4：把"怀疑"变成"可判读"
- 读数条 10 个槽位 + 文字区一行 `ddn data cmd=%u r1=%08x len=%u`；
- 修好读数条自己的两个 bug（标签短于 4 字符会把值截断；行距 26 px 太挤）；
- 三份实现的逐项对照写进 `docs/ANALYSIS-EMMC-DATA-PATH.md`，
  包括"为什么命令全通、数据全丢"的完整解释和下一次上机的判读表。

### 2. 不用刷机就能验证布局：`tools/render_panel_preview.py`
在 PC 上把整个仪表区（读数条 + 横幅 + 点阵 + 状态格 + 白条）渲染成 PNG。
上面那两个读数条 bug 本来用它就能零成本抓到。生成效果见
`docs/../artifacts/panel_preview.png`（也可随时重跑）。

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
