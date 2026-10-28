# 更正清单：被实测推翻的旧结论

> 本文件优先级高于其它文档的结论段落。凡与本文件冲突的旧说法，以本文件为准。

## C1. 「非安全态拿不到 GIC 中断，是固件墙」——**错**

旧结论（`docs/NEXT-STEPS.md`、`docs/FINDINGS-2026-10-26.md`、`docs/R1-GIC-PLAN.md` §1 的表格）：

> 非安全侧写不进 `GICD_CTLR.EnableGrp1NS` / 组寄存器，所以中断被固件锁死，
> 唯一出路是改 ATF/LK。

实测推翻（`docs/R1-GIC-PLAN.md` §0.0）：

```
e01 hppir1 0x1b                      强制 pending 后 CPU 接口立刻报告 INTID 27
e02 CNTP30 ctl 0x5 ispender 0x41000000 hppir1 0x1e
```

- `GICR_IGROUPR0` 写后回读 0 是**规范规定的 NS 视角 RAZ/WI**，分组本身早已是 Group 1
- `GICD_CTLR` 读 `0x12`：bit1 已经是 EnableGrp1NS（LK 写的是 `0x16`，非安全侧看到 `0x12`）
- 旧探针之所以"收不到"，是因为读了 **Group 0** 的 `HPPIR0/IAR0`（NS 不可用），
  且没清残留 pending

**结论：GIC 通路从 NS 到 CPU 接口全链正常。R1 不是阻塞项。**

## C2. 「内核碰 GIC MMIO 会卡死总线」——**错（归因错）**

旧结论（`docs/DEAD-ENDS.md` §4、`docs/NEXT-STEPS.md`）：

> 内核（MMU 开）写 `GICD_CTLR`/`GICD_IGROUPR` 会把总线卡死。

实际原因（`docs/EMMC-LOG.md`、commit `46d6e22`）：

- 真正的静默卡死是 **`ICC_IGRPEN0_EL1` 在两安全态 GIC 上对非安全态 UNDEFINED**
- 早期向量没装好，UNDEFINED 表现为"一个字都不打印"

**结论：不要读/写任何 Group 0 的系统寄存器；GICD/GICR 的 MMIO 访问本身不是禁忌。**

## C3. 「内核死在 `kernel_startup_initialize_upto()` 的返回处，栈/LR 被破坏」——**错**

旧结论（`docs/FINDINGS-2026-10-27.md` 尾部）：

> 死点在函数返回本身，最强假设是栈/保存的 LR 被破坏。

实测推翻（`docs/FINDINGS-1027-EARLYBOOT.md`）：

```
[canary] ok
[canary] about to return
[caller] upto(LOCKDOWN) returned      ← 调用者侧的标记也出现了
```

**结论：没有栈破坏，函数正常返回。**

## C4. 「第一次开中断即静默死 = 异常入口崩溃/向量问题」——**错**

旧结论（本轮中间判断）：

> 跳过 `spllo()` 就前进了，所以是异常入口崩。

实际原因（`docs/FINDINGS-1027-EARLYBOOT.md` 第四轮）：

- PPI 是**电平触发**，`arm_init` 打开 `CNTV` 时**没有 TVAL**，源电平永不撤销
- 处理函数 ack+EOI 之后电平立刻再次拉高 → **中断风暴/活锁**，表现为静默不前进
- 在 EOI 前屏蔽 `CNTV`/`CNTP` 即消失，启动推进到 **`sched_startup`**

**结论：这是「中断源未解除」的活锁，不是异常向量问题。**
`DANDELION_HOLD_INTERRUPTS` 那个实验开关只是用来分离症状的，不是修复。

## C5. 「`DANDELION_TIMER_IRQ` = 30」——**错**

- `pexpert/pexpert/arm64/DANDELION.h` 定义的是 **27**
- XNU 的 decrementer 走 **`CNTV`（虚拟定时器，INTID 27）**
- shim 探针里测到的 30 是 **`CNTP`（物理定时器）**

**结论：内核 dispatch 必须认 27；30 若进来必须被显式屏蔽或作为第二个源处理。**

## C6. 「`*.fd` 出不来是因为 submodule 目录布局，要先 `git submodule update`」——**错**

旧结论（`docs/HANDOVER-UEFI-1028.md` §已验证的事实、§下一步）：
> 依赖是 zip 解压进树的、不是 git submodule 检出，所以 stuart / build_uefi.py 建不起 workspace；
> 先跑 `git submodule update --init --recursive` 就能解决。

实测（2026-10-28 01:44，`SILICIUM_UEFI.fd` 产出后回看）：
- 顶层 `git submodule update` **根本不需要**：`Mu_Basecore` / `Common/Mu` 作为普通目录
  照样被 `PACKAGES_PATH` 接受，meta-data 阶段能正常解析
- 真正的错是 `MdePkg.dec(33)` 那条 `[Includes]`：
  `Library/MipiSysTLib/mipisyst/library/include` **目录是空的** ——
  `mu_basecore` 里 `MdePkg/Library/MipiSysTLib/mipisyst` 是一个**嵌套子模块**，
  zip 包不含它，所以目录在、内容空
- 同一个坑还有三个（`.dec` 的 `[Includes]` 会逐个校验）：`MdeModulePkg/.../brotli`、
  `SecurityPkg/.../libspdm`，以及作为源码用的 `MdePkg/Library/BaseFdtLib/libfdt`
- 「lancelot 报同样的错 ⇒ 问题在环境不在我们的包」：**结论对、归因错** ——
  环境确实有问题，但不是 workspace 布局，是四个没被填上的嵌套子模块

**结论：恢复方式是按 `mu_basecore@3c9c7b2` 的 pin 补齐这四个嵌套子模块，
不是重做顶层 submodule 检出。** 命令见 `HANDOVER-UEFI-1028.md` §结果。

## 仍然成立的旧结论（不要删）

- `fastboot boot` 不可用（MTK LK 限制）
- 从 Android 读物理内存不可行（无 `/dev/mem`，DEVMEM 未启用）
- pstore/ramoops **跨重启不保留**（preloader 每次重新训练 DRAM）
- QEMU 的 `pmemsave` 在本机不可用
- 只用开源 XNU 源码时，终点是 `waiting for root device`（无 APFS/HFS+/ext4/f2fs、无用户态）
- UniXNU 那条能进用户态的路需要 UEFI，且 MTK 手机无人验证成功

## C7. `[uefi_fd].base` 与 `MemoryMapLib.c` 的 "UEFI FD" 不一致（**已确认为真**）

实测两处源码：

```
Platforms/Xiaomi/dandelionPkg/Library/MemoryMapLib/MemoryMapLib.c:47
    {"UEFI FD", 0x48880000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},

MemoryMapLib.c:43 的注释原文
    //            "UEFI FD" here must equal [uefi_fd].base in

Resources/Configs/dandelion.toml
    [uefi_fd]
    base = 0x40080000      <-- 与上面的 0x48880000 矛盾
    size = 0x200000
```

`0x40080000` 是从 `lancelot.toml` 抄来的，不是 dandelion 的值。

**后果**：打包时 payload 会被放到一个和固件自己认为的地址不同的地方，现象是
黑屏或立刻重启，且没有任何日志可看（SEC 还没起来）。

**处理**：以 `MemoryMapLib.c` 的 `0x48880000` 为准，把 toml 改过来；
并且核对 `bootshim/patch_boot.py` 里 payload 的落地地址与之相同。
刷机前必须三处一致。


## C8. `0x48880000` 是错的：LK 只会在 `0x40080000` 执行 kernel 段（2026-10-28 刷机实测）

C7 的结论「以 `MemoryMapLib.c` 的 `0x48880000` 为准」**是错的**。C7 只核对了三处
*一致性*，没有验证 LK 到底把 kernel 放到哪。刷机实测：

| # | 实验 | 结果 |
|---|---|---|
| 1 | 原 FD（内部基址 `0x48880000`），`kernel_addr=0x48880000` | Redmi logo → 稳定 **~10 s** 复位，循环 |
| 2 | 同一 FD，`kernel_addr=0x40080000` | 同上（入口 stub 从错误地址解压） |
| 3 | **位置无关的 bootshim**（`shim.ld` 里 `. = 0`），`kernel_addr=0x48880000` | 同上，代码根本没跑 |
| 4 | 同一 bootshim，`kernel_addr=0x40080000` | ✅ 正常（线 A 一直如此） |
| 5 | FD 里两处 `0x48880000` 改成 `0x40080000`，`kernel_addr=0x40080000` | 复位时间 10 s → **~60 s** |

实验 3/4 是判定性的：同一个位置无关镜像，只改 `kernel_addr`，`0x40080000`
能跑、`0x48880000` 不能 → **LK 只认 `0x40080000`**。

实验 5 说明 SEC 真的执行了：LK 自己武装的 10 s 看门狗被改写，复位时间变成别的值。
**M2 一直卡在地址上，不是 SEC/PEI 的代码。**

**正确配置**（已改回）：

| 位置 | 值 |
|---|---|
| `Resources/Configs/dandelion.toml` → `[uefi_fd].base` | `0x40080000` |
| 同上 → `[boot_image].base` | `0x40000000`（+ `kernel_offset 0x80000`） |
| `MemoryMapLib.c` → `"UEFI FD"` | `0x40080000`，`0x200000` |
| `patch_boot.py --kernel-addr` | `0x40080000` |

`PcdSystemMemoryBase = 0x48000000` **不需要跟着改**：FD 镜像本身可以待在系统内存
之外，只有解压出来的 FVMAIN 和 DXE heap 需要在 `0x48xxxxxx` 里（它们本来就在）。

**教训**：`PcdFdBaseAddress` 必须等于 LK 真实的加载地址，这个值只能刷机测，
不能靠「哪份 toml 抄得更像」推。

**遗留**：`~60 s` 和信标表里的 20 s / 30 s / 2 s 都不吻合。要么是掐表误差
（PEI 的 30 s 加上 LK 阶段），要么是 WDT 的时钟倍率与 vendor 公式不符。
下一次用 `Seconds=6` 的单级信标就能一次定标。

---

## C9. 「面板中段那片白横纹是第二个 console / GOP 在抢输出」——**错**

**现象**：framebuffer 大约 y 880..1200 有一大片斜向白色条纹，看起来像用错误 pitch
（2880 而不是 2944）渲染出来的文字。为此先后摘掉了 `GraphicsConsoleDxe`、
`SimpleTextInOutSerial`，甚至整个 GOP，横纹都还在。

**真因**（2026-10-29 读代码，`DandelionConsoleDxe.c` 的 `FillRow()`）：

```c
Dst = base + Row * 16 * STRIDE;                // 只定位到这一格的**第一行**
for (X = 0; X < DANDELION_FB_WIDTH; X++) {     // 也只写这一行
  Dst[X] = Color;
}
```

`DandelionClearScreen()` 的写法是 `for (Row = 0; Row < 100; Row++) FillRow (Row, bg)`，
于是它只清了**扫描行 0, 16, 32, …, 1584**，每 16 行里有 15 行**从来没被清过**。
那片横纹是 PEI / 上一帧留在 DRAM 里的老内容，被面板原样扫描出来。

**反证**：`PlatformProgressDxe` 在 y=24 写的 `DANDELION UEFI` 一直擦不掉 ——
24 不是 16 的倍数；而它写在 y=1024 的白条（16 的倍数）确实被擦掉了。

**结论**：不是第二个渲染器，是**我们自己的清屏少清了 15/16**。
读屏不可靠的那几个月，有一半是这条 bug 造成的假象。

**已修**：`FillRow()` 填满 16 个扫描行（并连带补齐隐藏的 16 像素补白），
`ScrollUp()` 随之正确。

---

## C10. 「MSDC 一个阶段都没报到，说明 eMMC 走不到那一步」——**错**

**现象**：`boot-uefi-gpiofix.img` 上状态带（第 26..33 行）是洋红，即只有控制台报过到，
`MSDCDxe` 的 yellow / orange / cyan 一个都没出现。

**真因**：`MsdcImplLib.c` 的 `MsdcSay()` 开头就是

```c
if ((gST == NULL) || (gST->ConOut == NULL) || (Tag == NULL)) {
  return;
}
```

而当时 `MsdcDxe` 排在 `APRIORI.inc` 第 29 行、`DandelionConsoleDxe` 在第 91 行。
**构造函数执行时 `gST->ConOut` 还是 NULL**，所以每一条标记都在画状态带之前返回。

**教训**：**诊断代码自己依赖被诊断的东西**，是这个项目里反复出现的坑
（串口不存在、ConOut 还不存在、GOP 还没装……）。
在没有串口的板子上，任何"打点"都必须直写 framebuffer，并且**不许 early return**。

**已修**：
1. `MsdcSay()` 先写 framebuffer（阶段格子 + 状态带），再决定要不要写字；
2. `MsdcDxe` 挪到 `DandelionConsoleDxe` 之后；
3. 单一状态带升级成 **8 个固定位置的状态格子**（y 1400..1480，80×80，间距 88），
   "格子存在 = 阶段到达"，一张照片就是一个状态向量，不会互相覆盖。

## C11. 「面板上半屏那片重影乱码是第二个 console / 第二个渲染器」——**错**

**现象**：`boot-uefi-pio-dbg.img` 的照片里，文字区上半部分（y 0..700 左右）是一团
"同一个字叠印两遍"的重影，下半部分（`BLOCK NONE` / `= Dump Emmc Csd Register=`）却
干净清楚。于是很容易判断成"有两个东西在写同一块 framebuffer"。

**真因**：`DandelionConsoleDxe` 的 `ScrollUp()` 用

```c
CopyMem (FB_BASE, FB_BASE + 16 * STRIDE, (CLEAR_ROWS * 16 - 16) * STRIDE);
```

也就是**把 framebuffer 自己当 memmove 的源**。这块屏幕的 framebuffer 读回不可靠，
复制出来的每一行是"新旧内容混在一起"的错位像素，再被下一次写入叠上去 ——
滚动了 N 次之后，上半屏就是 N 层错位文字叠成的重影。下半屏从没被滚过，所以一直是对的。

**教训**：**任何可能只剩"从显存读回来"这一条路的代码，都要先怀疑读回**。
这块板子上 framebuffer 只保证"写得进"，不保证"读得出"。诊断通道本身要是坏了，
后面所有结论都会被它污染（这条和 C9、C10 是同一类坑的第三次）。

**已修**：不再滚动。整页写满后回到第 0 行覆盖最旧的一行（`WrapToTop()`），
每行第一次写入前先整行擦除（`mRowNeedsClear`）。代价是一张照片只能看到
"最近 68 行"（本来也就只看日志尾部），收益是**每一次渲染只画一遍**，
面板上出现的东西一定是当前内容，不再有重影。
另外 `SetCursorPosition()` 会把 row ≥ 68 的请求夹到 67，避免调用方把光标
放到 y ≥ 1088 的仪表区、覆盖掉状态格子。

## C12. 「读数条上的 `CFG` 和 `R1` 两行没有值，说明那两个寄存器没读到」——**错**

**现象**：`boot-uefi-readout.img` 的照片上，左列第 2 行只有 `CFG` 三个字，右列第 2 行
只有 `R1` 两个字，后面是黑的；同一张照片上其它 8 行都正常显示 `标签=8 位十六进制`。

**真因**：不是没读到，是**字符串在第二个字符之后就结束了**。`MsdcPanelValue()` 是这么拼的：

```c
for (Index = 0; Index < 4; Index++) {
  Text[Index] = mMsdcPanelLabel[Slot][Index];
}
Text[4] = '=';
```

它无条件拷 4 个字符。而标签表里写的是 `"CFG"` 和 `"R1"` —— C 字符串字面量自带结尾 `\0`，
于是 `Text[3]`（`CFG`）或 `Text[2]`（`R1`）被写成了 `\0`，`MsdcPanelText()` 一遇到 `\0`
就停下：**标签画出来了，值一个字都没画**。剩下 8 行标签本来就正好 4 个字符，所以只有这两行坏。

**教训**：**固定宽度的字段 + C 字符串 = 静默截断**。诊断代码里凡是"格式固定"的地方，
要么把标签补齐到固定宽度，要么断言长度；出问题时的表现（"标签在、值不在"）看起来
完全像"那个寄存器没读到"，非常容易把结论带偏。

**已修**：标签全部补齐成 4 字符（`"CFG "` → `"MCFG"`，`"R1"` → `"R1  "`），
并在表上方写清楚"必须正好 4 个字符"。
另外新增 `tools/render_panel_preview.py`：**在 PC 上把整个仪表区渲染成 PNG**，
这类布局/截断问题不用刷机就能看出来。


