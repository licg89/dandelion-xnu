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
