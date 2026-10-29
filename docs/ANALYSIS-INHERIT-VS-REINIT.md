# 继承 vs 重新初始化：eMMC 数据阶段为什么不开始

> 结论一句话：**这块板子上唯一被真机证明能读写 eMMC 的实现，是一份「绝不重新初始化」的实现，
> 它就在本仓库里（`bootshim/msdc.c`）。我们四轮都反着做。**

状态：2026-10-29 20:3x，依据 IMG_5760 + `bootshim/msdc.c` + [EMMC-LOG.md](EMMC-LOG.md)。
对应更正：[CORRECTIONS.md](CORRECTIONS.md) C15。

---

## 1. 已经确定的事实（不再需要重复测量）

IMG_5760 的面板读数：

```
C0  I=00000100 C=0200F09B cl=99 f=305 l=412 kc=15039
C1  n=4 I=00000100 R=C0FF8080
C2  3=9B010059 2=32503036 1=34009DB2 0=3BB919C9
C3  I=00000100 R=00000500 C7 I=00000100 R=00000700
C13 I=00000100 R=00000900 st=4
PV  pre=00000900 a=00000900 b0=00000000 c=00400900 STALE
A8  I=0100 R=00000900 sz=00000000 ps=EF/FF gp=DF8/DFF f=00 k=14999 c6=4B890760 t=4 r=1
B8  I=0100 R=00000900 sz=00000000 ps=EF/FF gp=DF8/DFF f=00 k=15007 c6=00400800 t=4 r=1
C8  I=0100 R=00000900 sz=00000000 ps=EF/FF gp=DF8/DFF f=00 k=14998 c6=00400800 t=4 r=1
D8  I=0100 R=00000900 sz=00000000 ps=EF/FF gp=DF8/DFF f=00 k=14996 c6=4803EAE0 t=4 r=1
B1  -  no data phase
```

逐条读出来：

| 观察 | 结论 |
|---|---|
| `C1 R=C0FF8080`、`C2` 四个字、`C7`、`C13` 全部应答 | 命令通道、时钟、CMD 焊盘、控制器命令路径**全部正常**，包括 136 位长响应 |
| `C13 st=4` 且 `t=4 r=1` | 卡确实在 **TRAN**、`READY_FOR_DATA` 置位 |
| `R=00000900` 且无任何错误位 | 卡对数据命令**报告成功** |
| `ps=EF/FF`（`MSDC_PS[23:16]` 的 AND/OR） | 控制器**从没看到** DAT0..7 被拉低 |
| `gp=DF8/DFF`（GPIO 122..133 的 DIN） | 独立第二个窗口，同样只有 CLK/CMD 在动 |
| `k=14999`（CLK 焊盘低的采样数 / 30000） | **整个等待期间时钟一直在跑**（约 50% 占空比） |
| `A8 == B8 == C8 == D8` 逐字相同 | 位宽和时序**不是**变量 |
| `I=0100` = 只有 CMDRDY | 没有 `XFER_COMPL`、没有 `DATTMO`（3 s 内；DTOC≈385 s，本来也不该有） |

所以：「时钟没跑」「卡没被选中」「命令没到卡上」「位宽不匹配导致 CRC」这四类解释全部被排除。
剩下的只有一类：**主机在跟一张它以为的卡说话，而这张卡不是它以为的那张。**

## 2. `PV ... STALE` 是误读，不是新问题

判据选错了命令。`CMD10`（SEND_CID）按 JESD84-B51 **只在 stby 态合法**：

- TRAN 态下它非法 → 卡回 **48 位**错误响应，主机按 R2 等 136 位 → `RESP0` 收成 0（`b0=0`）；
- `ILLEGAL_COMMAND`（bit22）锁存在卡的状态里，由**下一条**命令报出来 → `c=00400900`。

`b0 != a` 已经证明 `RESP0` 跟着命令走，`c` 多出的 bit22 是第二条独立证据。
**结论：`RESP0` 是活的，本仓库所有 R1 读数可以相信。** 判据已改为只看 `b0 != a`。

## 3. 真正的答案：`bootshim/msdc.c` 就在仓库里，而且跑通过

`docs/EMMC-LOG.md` 记录（2026-10-27，真机验证）：

```
magic DLXLOG01   version 1   boot 3   text_bytes 5159   note shim msdc pio
```

也就是说 line A 已经在**同一台手机、同一个 MSDC0、同一张卡**上完成了
「读 GPT → 找 flashinfo → 写 512 字节扇区 → 逐字节读回校验 → CMD6 FLUSH_CACHE」。

它的文件头（`bootshim/msdc.c:1-28`）把策略写得很死：

> LK has just loaded *this image* through MSDC0, so the controller is clocked, the pads are
> muxed, the card is selected with RCA 1 and it is in the transfer state. We therefore
> **inherit rather than re-initialise: no CMD0, no CMD1, no re-tuning.**
> The only thing we change is the clock divider, and we slow it down.
> **CKMOD and `SDC_CFG.BUSWIDTH` are left exactly as LK set them**, because the card's own
> bus width lives in `EXT_CSD[183]` and the two must keep matching.

`host_bringup()`（`bootshim/msdc.c:669-715`）实际只做四件事：

1. 读 `MSDC_CFG`，**`CKSTB` 没置位就直接放弃**（时钟被门控时别的都不能碰）；
2. `MSDC_INTEN = 0`、`MSDC_INT = 0xFFFFFFFF`（纯轮询、清残留）；
3. `MSDC_CFG = (cfg & ~CKDIV_MASK) | MODE | PIO | CKDIV(260)`，然后**等 `CKSTB` 回来**；
4. `SDC_CFG` 只放宽 `DTOC`/`WRDTOC`，**`BUSWIDTH` 一位不动**；最后清 FIFO。

之后 `CMD13`（RCA=1）确认 TRAN → `CMD8` 读 EXT_CSD → `check_bus_width()` 校验
`SDC_CFG.BUSWIDTH == EXT_CSD[183]` → 才允许读扇区。

### 3.1 和我们的四件事逐条对照

| bootshim（真机通过） | UEFI 侧（IMG_5752..5760，全部失败） |
|---|---|
| 不碰 `MSDC_CFG.RST` | `ResetHost()` 先复位控制器 |
| 不发 CMD0/CMD1/CMD2/CMD3/CMD7 | 从 IDLE 一路重新初始化到 TRAN |
| `SDC_CFG.BUSWIDTH` 保持 LK 值，并校验等于 `EXT_CSD[183]` | `SetHostBusWidth(1)` / `(8)`，与卡的实际位宽无关 |
| 只改 `CKDIV`（260 ≈ 385 kHz），等 `CKSTB` | 保留继承的 833 kHz，不等 `CKSTB` |

关键在第二行：`EXT_CSD[183]`（`BUS_WIDTH`）和 `EXT_CSD[185]`（`HS_TIMING`）是**卡的配置，
CMD0 不会清掉它们**。卡被 LK 留在 8 位 HS 模式，我们重新初始化到 TRAN 之后它**仍然是 8 位 HS**，
而主机把自己设成 1 位 —— 这就是「主机在跟一张它以为的卡说话」的具体形态。

## 4. 这一轮改了什么

1. **`MsdcDxe` 从 `Include/DXE.inc` 和 `Include/APRIORI.inc` 整条移除。**
   只要它跑过，上面四件事就已经发生过，MSDC0 的状态不再是 LK 留下的。
2. **`StorageBeaconDxe` 的 Depex 改成 `TRUE`**（不再依赖被移除的 PassThru 生产者），
   它现在自己就是 eMMC 驱动，而不是叠在驱动上面的报告器。
3. **新增 `InheritProbe()`**，在入口第一句执行，逐字照抄 `host_bringup()` + `msdc_cmd()`
   + `data_read()`，打印四行：

   ```
   INH C=<继承的 MSDC_CFG> S=<继承的 SDC_CFG> k=<CKSTB> C'=<改完的 MSDC_CFG>
   INH C13 I=.. R=.. st=..
   INH C8  I=.. rev=.. bw=.. hs=.. sec=..
   INH B1  I=.. S=..
   ```

   `bw=` 是 `EXT_CSD[183]`，`hs=` 是 `EXT_CSD[185]` —— 这两个数第一次被读出来，
   它们决定后面主机应该把自己设成几位。`S=45464920` 就是 `"EFI "`，即 GPT 头。
4. **`A8/B8/C8/D8` 四轮阶梯缩成单条 `A8`**（它才是「和 LK 留下的一致」那一档），
   省下的三行给 `INH` 用；`MsdcDxe` 移除后它的 DEBUG 行也没了，行数反而更宽裕。
5. 继承路径**成功时不再跑** `ProbeCardAndMux()`（它从 CMD0 开始），
   免得同一次启动里刚找到能用的配置就把它砸掉。

## 5. 下一轮看什么

| `INH` 输出 | 含义 | 下一步 |
|---|---|---|
| `k=0` | `CKSTB` 没置位，时钟被门控 | 先解决时钟，其余免谈 |
| `INH C8 I=...1100`（含 `XFER_COMPL`） | **继承路径成功** | 把 `bootshim/msdc.c` 的读路径整体搬进一个 `DandelionEmmcDxe`，产出 `BlockIo`，M4 收尾 |
| `INH C8 I=0100` 但 `bw=`/`hs=` 有值 | EXT_CSD 没读到但卡应答了 | 用 `bw=`/`hs=` 把主机设成与卡一致，再读一次 |
| `INH C13 st!=4` | LK 没把卡留在 TRAN | 说明「继承」这条路的前提不成立，回到重新初始化，但要**同时**把 `BUSWIDTH` 设成 `EXT_CSD[183]` |

## 6. 教训

**同一个仓库里已经有跑通的实现时，先把它的策略抄过来，再谈新假设。**
这条比前面所有更正都贵：四轮刷机、四张照片、一整晚，答案一直躺在
`bootshim/msdc.c` 第 5–14 行，而我们每一轮都在它的第 6 行上做反面。

---

## 7. 第一次上机（IMG_5761）：看起来像卡死，其实是刷了个旧固件

`boot-uefi-inh.img` 刷上去之后，面板上只有 `DANDELION UEFI` + 进度条、
`SELFTEST 0123456789`、`ABCDEFGHIJKLMNOPQRSTUVWXYZ`、`DIRECT CALL 0123456789`、
`COUT 0 50x19`，然后是一行糊成一片的长文本，再往下是控制台自己那两条证明横条
（y 1000 洋红 / y 1040 黄），底部没有状态格子、没有 `BLOCK NONE` / `FAT NONE`、
没有 `PayloadBoot` 那几行。第一反应是"拿掉 `MsdcDxe` 把调度链弄断了"。

**两个判断都不成立**：那次刷机根本没有包含任何新代码。
`tools/package_uefi_bootimg.py` 的 `--fd` 默认值指向 `D:\aios\uefi-a04br3\Build\…`
（一份 2026-10-28 23:48 的旧副本），而构建是在 WSL 里做的、产物只写到
`D:\aios\artifacts\`。于是连刷两次的镜像 **byte 相同**（sha256 都是 `ba70223d…`），
面板显示的是前一天的固件。完整记录见 [CORRECTIONS.md](CORRECTIONS.md) C16。

> **判据**：`kernel payload:` 那个字节数在换了源码之后**必须变化**。
> 两次打包得到同一个数，就是没有换固件。

即使如此，这一版仍然值得保留 —— 它把"是调度链断了还是我们卡住了"这个问题
变成了一个能被一张照片回答的问题：**格子路标**。

| 格子 | 含义 |
|---|---|
| 0 | 入口点执行了（`StorageBeaconDxe` 真的被 dispatch） |
| 1 | `MSDC_CFG`/`SDC_CFG` 写完、`CKSTB` 等到了 —— 若为红，说明时钟被门控，直接返回 |
| 2 | `CMD13` 发完且打印完 |
| 3 | `CMD8`（EXT_CSD）读完；绿 = 数据相位成功，黄 = 命令成功但没数据 |
| 6 | `ProbePower()` 跑到了（原来的黄格子） |

配合 `INH` 四行，一张照片就能定位到"哪一句"而不是"哪一轮"。

> 附带修正：`InheritProbe()` 现在在 `CKSTB` 没回来时**直接返回**（`bootshim` 也是这么做的，
> 返回 `MSDC_ENOCLK`），不再在时钟被门控的情况下白等 200 ms + 3 s。

---

## 8. 结论（IMG_5762）：继承路径就是答案，M4 数据通路打通

```
COUT 0 50x19INH C=02200199 S=09020000 k=1 C'=02210499
INH C13 I=00000100 R=00000900 st=4
INH C8  I=00001100 rev=08 bw=06 hs=01 sec=0747C000
INH B1  I=00001100 S=45464920
INHERIT OK
```

逐条：

| 读数 | 含义 |
|---|---|
| `I=00001100` | CMDRDY **+ XFER_COMPL** —— 数据相位真的走完了。四轮重新初始化时这里永远是 `0100` |
| `S=45464920` | LBA 1 的 GPT 头，`"EFI "`。**512 字节读成功** |
| `bw=06` | `EXT_CSD[183]` = **8 位 DDR**。LK 一直把卡留在 8 位 DDR 高速模式 |
| `hs=01` | `EXT_CSD[185]` = high speed（不是 HS200/HS400） |
| `rev=08` | eMMC 5.1 |
| `sec=0747C000` | **122,159,104 扇区 = 62.5 GB**，与 `flashinfo` 的 LBA 122,109,919 一致 |
| `C=02200199` | 继承的 `MSDC_CFG`：`CKDIV=1`（满速）、`CKMOD=2`、`CKSTB=1`、`MODE=1`、`PIO=1` |
| `S=09020000` | 继承的 `SDC_CFG`：`[17:16]=2` → **主机也是 8 位**。和 `EXT_CSD[183]` 对上了 |
| `C'=02210499` | 我们改完的 `MSDC_CFG`：`CKDIV=0x104=260`，其余位（CKMOD/CCKPD/CKSTB）原样保留 |

**所以真正的错配是"主机把自己设成 1 位、卡还在 8 位 DDR"**，加上"控制器被复位、卡被重新初始化"。
`A8` 那一档虽然也把主机设成 8 位，但前面已经复位过控制器并从 IDLE 重新初始化过卡 ——
现在可以确定变量在**重新初始化**这一侧（或者复位），而不在总线位宽本身。

### 8.1 落地

新增 `Platforms/Xiaomi/dandelionPkg/Drivers/DandelionEmmcDxe/`：

- `EmmcInherit()`：`INTEN=0`、`INT` 全清、`SDC_CFG` 只放宽 `DTOC`/`WRDTOC`（**不动 BUSWIDTH**）、
  `CMD13` 确认 TRAN、`CMD8` 读 `EXT_CSD`、`CMD17` 读 LBA 1 校验 `"EFI "`；
- **时钟自校准**：依次试 `CKDIV = 26 / 52 / 104 / 260`（快→慢），
  每一档都"设时钟 → 等 `CKSTB` → 读 `EXT_CSD` → 读 LBA 1 校验签名"，
  第一个通过的留下。bootshim 固定 260（≈385 kHz）是因为它只写 5 KiB；
  FAT 要读几个 MB，所以这里要尽量快，但不能猜采样点；
- `ReadSectors`/`WriteSectors`：单块 `CMD17`/`CMD24`，多块 `CMD18`/`CMD25`，
  每次最多 128 块（64 KiB），中间用 64 KiB bounce buffer（BlockIo 调用方不保证对齐）；
- 发布 `EFI_BLOCK_IO_PROTOCOL` + `VENDOR_DEVICE_PATH`，`LastBlock = sec - 1`。

**这一版 `Media->ReadOnly = TRUE`。** 写路径（`CMD24`/`CMD25`）已经实现了，但没打开：
这是用户的日常手机，读通路先在真机上验证几轮，写通路再一步打开。

### 8.2 下一步

1. `BlockIo` → `PartitionDxe` → `FatPkg` 应该能自己绑上（`StorageBeaconDxe` 的
   `ConnectEverything()` 会做 connect sweep），面板上要出现 `BLOCK OK` / `FAT OK`；
2. 读通路稳定后把 `ReadOnly` 改成 `FALSE`，`PayloadBootDxe` 的 `FlushLogToFlash()` 就活了 ——
   **日志写进 `flashinfo`，直接 `adb pull`，不用再拍照**；
3. 然后 M6 的外部依赖照旧（缺 `UniXNU.efi` 等产物）。

---

## 9. IMG_5765：BlockIo 上线，`FAT NONE` 多半是真话

```
COUT 0 50x19EMMC OK div=26 sec=0747C000
INH C=02201A99 S=FF027FFC k=1 C'=02210499
INH C13 I=00000100 R=00000900 st=4
INH C8  I=00001100 rev=08 bw=06 hs=01 sec=0747C000
INH B1  I=00001100 S=45464920
INHERIT OK
BLOCK OK
FAT NONE
```

| 读数 | 含义 |
|---|---|
| `EMMC OK div=26` | 时钟自校准**第一档就通过** —— CKDIV=26，约 7.7 MHz，是四档里最快的 |
| `INH C=02201A99` | `MSDC_CFG` 的 CKDIV=`0x1A`=26 ✓，CKMOD/CCKPD/CKSTB 原样保留 |
| `INH C8 ... rev=08 bw=06 hs=01 sec=0747C000` | EXT_CSD 再读一次仍然正确 |
| `BLOCK OK` | `EFI_BLOCK_IO_PROTOCOL` 装上并被上游驱动认领 |
| `FAT NONE` | 没有挂上文件系统 |

**`FAT NONE` 大概不是 bug**：这台机器的 Android 分区全是 ext4/f2fs，EDK2 的 `FatPkg`
没有可以绑定的卷。要区分"分区没解析出来"和"这块盘上确实没有 FAT"，
需要知道 **BlockIo 句柄数量**：只有 1 个 = 只有我们自己那块整盘 media（`PartitionDxe` 没绑上）；
多于 1 个 = GPT 解析成功、分区都在。所以 `BLOCK OK` 这一行现在带计数：`BLOCK OK n=..`。

顺带一个有用的交叉验证：`PayloadBootDxe` 报告

```
PayloadBoot: log span at LBA 122111967 is not free, refusing
```

LBA 122111967 = `flashinfo`(122109919) + 2048，正是 line A 在 2026-10-27 写下 DLXLOG01
日志块的位置。**我们的读路径读到了那段真实数据**，而且 `PayloadBootDxe` 的免费检查按设计拒绝了。
这一版加 `SpanIsOurs()`：首扇区 magic 是 `DDNLOG01` 就允许覆盖（那是上一次启动自己的日志），
这样日志通道才能反复使用。

### 9.1 这一版另外打开的东西

- `Media->ReadOnly = FALSE`：唯一的写入者是 `PayloadBootDxe` 的日志路径，
  它只写"空的或自己的"区间，并且写完逐字节读回校验；
- 目标：**日志写进 `flashinfo`，`adb pull` 就能读，不用再拍照**。


