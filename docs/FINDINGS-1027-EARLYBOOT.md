# 2026-10-27 晚：canary 实验结果（真机，eMMC 日志取回）

## 结论

```
> PE_init_iokit
iBoot version:
[step] inner canary: entry
> early_boot
[step] phase store: before / after
[step] inner canary: ok
[step] inner canary: about to return
```

1. **`[canary] ok` 且 `about to return` 都出现** → `kernel_startup_initialize_upto()`
   内部**没有栈破坏**，「函数返回即死」的假设作废。
2. 死点从 `LOCKDOWN` **前进到 `early_boot` 之后**（或至少越过了 LOCKDOWN）。
3. `ICC_IGRPEN0_EL1` 修复 + 内核 eMMC 通道 = 这两条是本轮真正的推进。

## 取日志的正确姿势（重要，别再踩）

`adb shell ... > file` 和 `adb exec-out` 直出**都会被 CRLF 破坏**（实测 2 MiB 变 4.19 MB）。
必须走 base64：

```powershell
adb exec-out "su -c 'dd if=/dev/block/by-name/flashinfo bs=4096 skip=512 count=512 2>/dev/null | base64'" > klog.b64
$b64 = (Get-Content klog.b64 -Raw) -replace '\s',''
[IO.File]::WriteAllBytes("klog.bin", [Convert]::FromBase64String($b64))
python tools/read_log_block.py klog.bin --scan
```

- shim 日志：`skip=256`（+1 MiB）
- 内核日志：`skip=512`（+2 MiB）
- `dd ... of=/sdcard/x` 在 `su -c` 下**静默失败**，不要用

## 下一步

1. 在 `early_boot` 之后继续加打点，定位新的死点（`kernel_bootstrap()` 里
   `kernel_startup_initialize_upto(STARTUP_SUB_EARLY_BOOT)` 之后的语句）
2. UEFI 线 M1 并行推进（`docs/UEFI-MT6765PKG-DERIVATION.md`）

## 第二轮（三打点）结果：死点在 spllo() 内部

```n[step] early_boot phase returned
[step] early_boot: spllo next      <-- 到此为止，没有 spllo returned
```

spllo() 是启动序列里**第一次允许中断**的地方。所以死因不是栈、不是 phase 返回，
而是**开中断后立刻发生的异常入口**（有待中断的 IRQ/FIQ，而向量或处理路径尚未就绪）。

下一步：把 spllo() 临时改成不解除屏蔽（或先清 DAIF 前检查 VBAR_EL1/IAR），确认死点是否随之后移。


## 第三轮（hold interrupts）：确认死因是中断入口

```n[step] early_boot: spllo next
[step] early_boot: spllo SKIPPED (DANDELION_HOLD_INTERRUPTS)
[step] early_boot: spllo returned
[step] initialize_screen: enter
!!! PANIC: zalloc.c:675 Assertion failed: Calling {k,z}alloc from interrupt disabled context isn't allowed
```

结论：

1. **跳过 spllo 后启动确实越过了原来的死点**（走到 initialize_screen 内部），所以之前的挂死就是「解除 DAIF 屏蔽 → 有中断进来 → 异常入口/处理路径崩」。
2. 这一版拿到的是**干净的 panic 而不是静默死**：initialize_screen() 内部 zalloc 断言「中断关闭时不允许分配」——这是实验本身的副作用（我们故意保持屏蔽），不是新 bug。

下一步：把 DANDELION_HOLD_INTERRUPTS 置回 0，转去修**异常入口**本身（sleh 的 IRQ/FIQ 分支 + dandelion_gic_ack/eoi 在 spurious INTID 与未初始化 per-CPU 状态下的行为）。

