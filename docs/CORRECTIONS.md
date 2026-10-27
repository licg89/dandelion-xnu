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
