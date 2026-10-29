# CMD13 四点探针：判定 `MSDC_INT` 到底有没有工作

> 2026-10-29，依据 IMG_5727 的实测日志。这是当前 M4 的唯一阻塞点。
> 结论先行：**故障在「观测路径」，不在数据通路**。卡是活的。

---

## 1. 实测日志（IMG_5727，`boot-uefi-m5-probe.img`）

```
PayloadBoot: firmware log handed over
PayloadBoot: firmware log published as an EFI configuration table
PayloadBoot: looking for a payload (GPT, FV, ...)
PayloadBoot: no SimpleFileSystem handle, using the payload embedded in the FV
PAYLOAD OK - the LoadImage -> StartImage -> Success

COUT 0 5A x 64                      <-- ConOut 就是我们的控制台（mode 0, 90x100）
MSDC PASSTHRU
ddn data: cmd 8 failed: status=0x12
ddn data: cmd 8 r1=00000900 len=512
ddn data: stall int=00000000
ddn data: cmd 8 r1=00000900 len=512
ddn data: PIO read stalled with 512 bytes left
ddn data: cmd 9 failed: status=0x12
ddn data: cmd 9 r1=00000900 len=512
ddn data: PIO read stalled with 512 bytes left
ddn data: cmd 7 failed: status=0x12
ddn data: cmd 8 r1=00000900 len=512
ddn data: PIO read stalled with 512 bytes left
BLOCK NONE
FAT NONE
```

## 2. 三个事实，合起来指向一个结论

**(a) `r1 = 0x00000900` ⇒ 卡处于 TRAN（state 4）**

`R1` 的 bit 12..9 是 `CURRENT_STATE`，`0x900 >> 9 = 4` = TRAN。
所以「卡不响应」「总线宽度不匹配」「数据线没接」全部可以排除。

**(b) 顺序是「先报失败、后读到合法 R1」**

```
cmd 8 failed: status=0x12      <-- 判定超时
cmd 8 r1=00000900 len=512      <-- 但响应寄存器里有合法值
```

命令**实际成功了**，只是驱动「没等到完成事件」，于是判超时；之后直接读 `RESP0` 才拿到真值。
`status=0x12` 是 `EFI_TIMEOUT`，不是 CRC 错、不是命令错。

**(c) `stall int=00000000`**

等数据时读 `MSDC_INT`，**一个位都没有** —— 连 `DATTMO`（数据超时中断）都没有。
如果数据事务真的启动了，控制器自己的超时逻辑必然会置 `DATTMO`。

### 结论

- `MSDC_INT` 全程为 0，而 `RESP0` 有效 ⇒ **中断/状态寄存器没有把「完成」交出来**
- 于是每条命令都被误判为超时 ⇒ 驱动从未真正进入数据阶段
- `PIO read stalled with 512 bytes left` 是「**没开始**」，不是「传了 0 字节」
- `INTR=00000000` 与 `R1=0x900` 并不矛盾：它们来自两条不同的寄存器路径

**我们前几轮一直在追一个可能从未被走到的数据通路。**

---

## 3. 探针设计（不碰数据、不碰总线宽度）

用 `CMD13`（SEND_STATUS）：无数据、最安全、一定该有响应。

```
1. 进之前读一次：MSDC_INT / MSDC_INTEN / SDC_STS / MSDC_CFG
2. 发 CMD13（arg = RCA，通常 1<<16）
3. 轮询 MSDC_INT，最长 10 s（不要 2 s 就放弃）
4. 退出时读：MSDC_INT / SDC_RESP0 / SDC_STS
5. 五个值全部画进永久读数条，一格一个
```

### 寄存器偏移（**必须先用 `MsdcImplLib.c` 里已有的宏核对，不要照抄本文**）

| 名字 | 典型偏移 | 用途 |
|---|---|---|
| `MSDC_INT` | 0x0C | 中断状态（可能 W1C，读它本身就是证据） |
| `MSDC_INTEN` | 0x10 | 中断使能 |
| `MSDC_CFG` | 0x00 | 控制器配置 |
| `SDC_CFG` | 0x30 | 含 `BLK_LEN` |
| `SDC_CMD` | 0x34 | 含 `DTYP` |
| `SDC_STS` | 0x3C | busy / 状态 |
| `SDC_RESP0` | 0x40 | R1 |

> 参数与命令的写法直接复用 `MsdcImplLib.c` 里现成的 `MsdcRead` / `MsdcWrite` 与宏，
> 不要在新代码里重新定义偏移 —— 这是上一轮 `SCFG/BLKN` 显示不出来的同类风险。

## 4. 判读表

| 观察 | 含义 | 下一步 |
|---|---|---|
| `RESP0` 变了，`MSDC_INT` 仍为 0 | 状态寄存器路径错（偏移/屏蔽/时钟门控） | 对着 LK 的 MT6765 源码核 `MSDC_INT`/`MSDC_INTEN` |
| `MSDC_INT` 出现 `CMDRESP` | 观测正常，之前是等 2 秒太短 | 改成按控制器自身超时等待 |
| 两者都不动，`RESP0` 也不变 | 命令根本没发出去 | 查 `SDC_CMD` 写时序与 `SDC_STS` busy |

## 5. 实现位置（按风险从低到高）

1. **`StorageBeaconDxe`（我们自己的驱动，推荐起点）**
   它已经能读 MMIO 并画面板。先只做**纯读**：`MSDC_INT` / `MSDC_INTEN` / `SDC_STS`
   —— 纯读不可能挂住控制器，可以立刻上机验证「寄存器读数是否稳定」。
2. **`0002` 补丁里加 CMD13**（需要同时改 hunk 行数，风险高）
   若走这条，用脚本生成补丁而不是手改：`tools/gen_patch_0002.py` 已经是这个模式，
   照它再加一段即可。

## 6. 顺带记录的次要问题

日志末行疑似 `PayloadBoot: ... device tree has a bad magic ...`。
`0x47880000` 处的 DTB magic 应为 `0xd00dfeed`。这是 M5 的独立问题，等 eMMC 定了再查。

---

## 7. 不要重复的弯路

- ❌ 不要再怀疑时钟太快（已 400 kHz）
- ❌ 不要再怀疑 FIFO 阈值（已按 `RXCNT` 实到实排）
- ❌ 不要再怀疑中断残留（已全清）
- ❌ 不要再怀疑卡状态（`R1=0x900` 已证明 TRAN）
- ❌ 不要再怀疑总线宽度（8-bit 只有在有数据之后才有意义）
- ✅ **先把 `MSDC_INT` 是否工作证伪**，这是所有后续判断的前提
