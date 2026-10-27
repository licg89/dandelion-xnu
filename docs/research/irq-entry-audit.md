# IRQ-entry audit — XNU (xnu-8796.141.3 + dandelion/MT6762G port)

**Question:** why can the *first* interrupt taken immediately after `spllo()` kill the kernel,
and what is the smallest set of concrete fixes?

**Tree audited (read-only):** `D:\aios\xnu` (patched, uncommitted), with the port's own sources in
`D:\aios\dandelion-xnu`. Nothing was committed or modified in either tree; this file is the only
artifact written.

**Method:** static reading of the vector → C path, plus the measurements already on record
(`dandelion-xnu/docs/R1-GIC-PLAN.md`, `FINDINGS-1027-EARLYBOOT.md`, `FINDINGS-2026-10-27.md`,
`DEAD-ENDS.md`) and the on-device register dumps quoted there. No phone commands were run.

---

## 0. Four corrections to the premises, before anything else

These matter because two of them change which bugs are even reachable.

| Premise as given | What the tree actually contains | Consequence |
|---|---|---|
| "A spurious guard was just added to the **IRQ** branch of `sleh.c`" | The guard is in the **FIQ** branch, `osfmk/arm64/sleh.c:1947-1962` (`#if defined(DANDELION)` inside `sleh_fiq`). `sleh_irq`'s DANDELION block (`sleh.c:1860-1883`) has **no** spurious check at all. | If the first interrupt dies in the IRQ path, the new guard does nothing for it. The guard's own `dandelion_gic_eoi(iar)` is a no-op for `iar >= 1020` (`dandelion_gic.h:764-769`), so the guard cannot clear anything either. |
| "this port routes the timer as IRQ 30" | `DANDELION_TIMER_IRQ` is **27** (`dandelion-xnu/pexpert/pexpert/arm64/DANDELION.h:107`). The measured `ICC_HPPIR1_EL1 = 0x1e` (30) is the **physical** timer PPI, produced by the shim's E0.2 test, not by XNU. XNU's decrementer is the **virtual** timer: `ml_set_decrementer` writes `CNTV_TVAL_EL0` (`osfmk/arm64/machine_routines.c:2038`). | 27 is the number that matters at runtime. Any INTID other than 27 that arrives is dropped **without de-asserting its source** — see §4.1. |
| "`pexpert/pexpert/arm64/dandelion_gic.h` is in the tree" | It is **not** in `D:\aios\xnu`. Neither is `DANDELION.h`. They are copied in at build time by `dandelion-xnu/tools/ci/apply-board.sh:28-30`. | The authoritative header values are the ones in `dandelion-xnu`. `xnu/pexpert/pexpert/arm64/board_config.h` only contains the `#ifdef ARM64_BOARD_CONFIG_DANDELION → #include <pexpert/arm64/DANDELION.h>` block (patched, lines 100-111). |
| "`machine_slot` must be initialised" | `machine_slot` does not exist in this kernel. The only hit in the whole tree is `osfmk/mach/mach_types.defs:406` (`machine_slot_data_t`). Its historical role is played by `CpuDataEntries[]` + `processor_array[]` + `cpu_data_t.processor`. | See §2.3. |

Second-order consequence of the `GIC_SPURIOUS_IRQ` value: it is **1023** in both Apple's
`VMAPPLE.h:59` and the port's `DANDELION.h:84`, but architecturally 1020-1023 are all special and
`dgic_ack()` already normalises anything `>= 1020` down to 1020 (`dandelion_gic.h:757-760`).
So `intid < GIC_SPURIOUS_IRQ` (`sleh.c:1876`) lets 1020, 1021 and 1022 through as if they were real
interrupt IDs. Use `< 1020`.

---

## 1. The exact path of an IRQ on this port

### 1.1 Vector table (already installed long before any of this)

`VBAR_EL1` is programmed in `start.s`, **before the MMU is enabled**:

```
osfmk/arm64/start.s:860   adrp  x0, EXT(ExceptionVectorsBase)@page
osfmk/arm64/start.s:861   add   x0, x0, EXT(ExceptionVectorsBase)@pageoff
osfmk/arm64/start.s:862   add   x0, x0, x22      // Convert exception vector address to KVA
osfmk/arm64/start.s:863   sub   x0, x0, x23
osfmk/arm64/start.s:865   MSR_VBAR_EL1_X0
```

The table itself is `locore.s:223-288`. Each entry is a 128-byte slot branching to a "long" handler;
with `__ARM_KERNEL_PROTECT__` undefined for this board the macro degenerates to a plain branch
(`locore.s:110-128`, the `#else b $0` arm).

Kernel-mode IRQ therefore enters at:

```
locore.s:230  Lel1_sp0_irq_vector:  BRANCH_TO_KVA_VECTOR el1_sp0_irq_vector_long, 1
locore.s:322  el1_sp0_irq_vector_long:
locore.s:323      EL1_SP0_VECTOR
locore.s:324      SWITCH_TO_INT_STACK
locore.s:325-327  ... fleh_irq ... b fleh_dispatch64
```

There is a second, fatal-if-reached case: if PSTATE.SP was 1 (SP_EL1 selected) when the IRQ hit,
`el1_sp1_irq_vector_long` (`locore.s:364-368`) ends at `fleh_irq_sp1`
(`locore.s:769-774`) → `panic_with_thread_kernel_state("IRQ exception taken while SP1 selected")`.
The kernel runs on SP_EL0, so SP0 is the normal case; this panic is the *diagnostic* you get if the
interrupt lands while the CPU is on the exception stack (i.e. a nested exception). Worth knowing:
it prints, so it is not the silent-death case.

### 1.2 What the vector does before any C code runs

**(a) Build the exception frame on the *interrupted* stack** (`locore.s:296-304`):

```
msr   SPSel, #0                    // select SP_EL0 (kernel stack)
sub   sp, sp, ARM_CONTEXT_SIZE     // carve arm_context_t
stp   x0, x1, [sp, SS64_X0]        // first two GPRs
add   x0, sp, ARM_CONTEXT_SIZE
str   x0, [sp, SS64_SP]            // the interrupted SP
INIT_SAVED_STATE_FLAVORS sp, w0, w1
mov   x0, sp                       // arm_context_t * -> x0
```

`INIT_SAVED_STATE_FLAVORS` (`osfmk/arm64/exception_asm.h:118-126`) stamps
`SS_FLAVOR/SS_COUNT = ARM_SAVED_STATE64/COUNT` and `NS_FLAVOR/NS_COUNT = ARM_NEON_SAVED_STATE64/COUNT`.
That is what makes `ASSERT_CONTEXT_SANITY`/`is_saved_state64()` work later (`sleh.c:92-101`).

**(b) Switch to the per-CPU interrupt stack** (`exception_asm.h:240-245`):

```
SWITCH_TO_INT_STACK:
    mrs  x1, TPIDR_EL1              // thread pointer
    ldr  x1, [x1, ACT_CPUDATAP]     // thread->machine.CpuDatap
    ldr  x1, [x1, CPU_ISTACKPTR]    // cpu_data->istackptr
    mov  sp, x1
```

Note the ordering: the *frame* stays on the interrupted kernel stack; only the C handler runs on
the interrupt stack. Both are built inside the first ~10 instructions of the vector, so a bad
`TPIDR_EL1` or a zero `istackptr` faults here, with the faulting SP still bad — the classic
self-recursive early-boot exception loop that `sleh.c:564-580` warns about in a comment:

> *"If an exception is raised in early boot before the initial thread has been set up, then calling
> `get_preemption_level()` in the SLEH will trigger an infinitely-recursing exception."*

**(c) Complete the register save** — `fleh_dispatch64` (`locore.s:505-576`) runs
`SPILL_REGISTERS KERNEL_MODE` (`exception_asm.h:144-227`), which stores x2-x28, fp, lr, all 32
Q registers, FPSR, FPCR, `ELR_EL1 → SS64_PC`, `SPSR_EL1 → SS64_CPSR`, `FAR_EL1 → SS64_FAR`,
`ESR_EL1 → SS64_ESR`, then `br x22` to `fleh_irq`. On a user-mode exception it additionally zeroes
x2-x28 and fp/lr before the branch (`locore.s:513-549`) — not our case.

**(d) `fleh_irq`** (`locore.s:677-693`) runs `BEGIN_INTERRUPT_HANDLER`
(`locore.s:629-645`), which is itself a per-CPU data walk:

```
mrs  x22, TPIDR_EL1
ldr  x23, [x22, ACT_CPUDATAP]
ldr  w1, [x23, CPU_STAT_IRQ]        // cpu_stat.irq_ex_cnt++
...
ldr  w1, [x22, ACT_PREEMPT_CNT]     // thread->machine.preemption_count++
str  x0, [x23, CPU_INT_STATE]       // cpu_data->cpu_int_state = &arm_context_t
PUSH_FRAME; bl sleh_irq; POP_FRAME
END_INTERRUPT_HANDLER               // locore.s:658-673
```

`END_INTERRUPT_HANDLER` clears `cpu_int_state`, decrements `ACT_PREEMPT_CNT` (branching to
`preempt_underflow` if it would go negative), restores `sp = thread->machine.kstackptr`, and
`sevl`. Then `exception_return_dispatch` (`locore.s:796-800`) → `return_to_kernel`
(`locore.s:806-820`) → `exception_return` → `eret` with `sp = x21`.

One detail that matters for the audit: `return_to_kernel` tests the **saved** CPSR
(`tbnz w0, #DAIF_IRQF_SHIFT, exception_return`, `locore.s:807`). The interrupted context had I set
(because `spllo()` had not yet executed its `msr daifclr` when the interrupt was taken), so the AST
path is skipped and the return is a straight `eret`. No dependency there.

### 1.3 Layout that must be correct

`arm_saved_state64` (`osfmk/mach/arm/thread_status.h:486-500`):

```
x[29] 0..231 | fp 232 | lr 240 | sp 248 | pc 256 | cpsr 264 | (pad 268)
far 272 | esr 280 | exception 284            -> 288 bytes
```

`arm_saved_state` adds the 16-byte `arm_state_hdr_t` (`thread_status.h:512-522`), and
`arm_context_t` = `{ arm_saved_state_t ss; arm_neon_saved_state_t ns; }`
(`thread_status.h:1028-1032`) → `ARM_CONTEXT_SIZE = sizeof(arm_context_t)` = 832 bytes with the
current `ns` layout (`thread_status.h:968-977`: 32×Q + fpsr + fpcr, plus its own 8-byte header).
The assembler gets every offset from `genassym.c:147-216`, so the C and asm views cannot drift —
`SS64_X0..X28, SS64_FP/LR/SP/PC/CPSR/FAR/ESR`, `NS64_Q*`, `NS_FLAVOR`, `NS_COUNT`, `ARM_CONTEXT_SIZE`.

Per-CPU offsets used by the vector are `genassym.c:121-138, 262-275`: `TH_KSTACKPTR`,
`ACT_CONTEXT`, `ACT_PREEMPT_CNT`, `ACT_CPUDATAP`, `CPU_ISTACKPTR`, `CPU_INTSTACK_TOP`,
`CPU_INT_STATE`, `CPU_STAT_IRQ`, `CPU_STAT_IRQ_WAKE`, `CPU_PENDING_AST`, `CPU_NUMBER_GS`,
`CPU_USER_DEBUG`.

### 1.4 The C side

`osfmk/arm64/sleh.c:1849-1903`:

```
1850  sleh_irq(arm_saved_state_t *state)
1852      cpu_data_t * cdp __unused = getCpuDatap();
1858      sleh_interrupt_handler_prologue(state, DBG_INTR_TYPE_OTHER);
1860  #if defined(DANDELION)
1871          uint32_t intid = dandelion_gic_ack();
1873          if (intid == DANDELION_TIMER_IRQ) {
1874                  cdp->cpu_decrementer = -1; /* Large */
1875                  rtclock_intr(TRUE);
1876          } else if (intid < GIC_SPURIOUS_IRQ && cdp->interrupt_handler) {
1877-1880          cdp->interrupt_handler(cdp->interrupt_target, ...);
1881          }
1882          dandelion_gic_eoi(intid);
1883      }
1884  #elif USE_APPLEARMSMP
1885      PE_handle_ext_interrupt();
1886  #else
1888      cdp->interrupt_handler(...);          // upstream Apple path for non-SMP platforms
1889  #endif
1894      entropy_collect();
1897      sleh_interrupt_handler_epilogue();
1898  #if MACH_ASSERT
1899      if (preemption_level != sleh_get_preemption_level())
1900          panic("irq handler %p changed preemption level from %d to %d", ...);
1902  #endif
```

The prologue (`sleh.c:2095-2113`) does `task_vtimer_check(current_thread())` (inline, `sleh.c:552-562`),
computes the PC (unslides it for kernel exceptions), and emits
`KDBG_RELEASE(MACHDBG_CODE(DBG_MACH_EXCP_INTR,0)|DBG_FUNC_START, 0, pc, is_user, type)`.
The epilogue (`sleh.c:2115-2122`) does `kperf_interrupt()` (KPERF only) and the matching KDBG end.

`dandelion_gic_ack()`/`dandelion_gic_eoi()` are `pexpert/arm/pe_fiq.c:70-86`; they are thin
wrappers that **return 1020 / do nothing when `dandelion_gic_ready == 0`**. The real work is
`dgic_ack`/`dgic_eoi` in `dandelion_gic.h:744-782`: `ICC_IAR1_EL1` / `ICC_EOIR1_EL1` for
`g->group == DGIC_GROUP1`, masked to 24 bits, everything `>= 1020` collapsed to 1020, and EOI
skipped for `>= 1020`.

Then the timer:

```
rtclock_intr(TRUE)          osfmk/arm/rtclock.c:322-370
  cdp = getCpuDatap(); cdp->cpu_stat.timer_cnt++; SCHED_STATS_INC(timer_pop_count);
  assert(!ml_get_interrupts_enabled());
  abstime = mach_absolute_time();
  if (cdp->cpu_idle_pop) {...}
  regs = cdp->cpu_int_state;                       // set by BEGIN_INTERRUPT_HANDLER
  if (abstime >= cdp->rtcPop) KDBG_RELEASE(DECR_TRAP_LATENCY ...);
  timer_intr(user_mode, pc)  osfmk/arm/arm_timer.c:66-118
      mytimer = &cpu_data_ptr->rtclock_timer;
      if (idle_timer_deadline && ...) { idle_timer_notify(...) }
      if (mytimer->deadline <= abstime) { mytimer->deadline = timer_queue_expire(...); }
      processor = PERCPU_GET_RELATIVE(processor, cpu_data, cpu_data_ptr);
      (void)running_timers_expire(processor, abstime);      // kern/timer_call.c:2098
      cpu_data_ptr->rtcPop = EndOfAllTime;
      timer_resync_deadlines();                             // arm_timer.c:146-185
          -> setPop(deadline)  (rtclock.c:390-406) -> ml_set_decrementer()
             -> CNTV_TVAL_EL0 = dec    (machine_routines.c:2028-2040, no tbd funcs on this port)
```

That last write is the only thing in the entire path that *de-asserts the timer's level*.
It is reached only through `timer_resync_deadlines()` → `setPop()`.

### 1.5 State that must already be valid at the first interrupt

| Required by | State | Set where | Valid at `spllo()`? |
|---|---|---|---|
| vector dispatch | `VBAR_EL1` | `start.s:860-865` | yes |
| `SWITCH_TO_INT_STACK`, `BEGIN_INTERRUPT_HANDLER`, `getCpuDatap()` | `TPIDR_EL1` = `thread_t` | `arm_init.c:470 machine_set_current_thread(thread)`; re-established on every context switch | yes |
| same | `thread->machine.CpuDatap` | `arm_init.c:468` | yes |
| `END_INTERRUPT_HANDLER`, `SWITCH_TO_KERN_STACK` | `thread->machine.kstackptr` | `thread_bootstrap()` + `thread_machine_init_template()` (`startup.c:546`) | yes |
| frame allocation `sub sp,#832` | SP_EL0 on a mapped kernel stack | thread context | yes |
| `SWITCH_TO_INT_STACK` | `cpu_data->istackptr`, `intstack_top` | `arm_init.c:456-457` (`&intstack_top`) | yes |
| `BEGIN_INTERRUPT_HANDLER` | `cpu_stat.irq_ex_cnt`, `cpu_int_state` | `cpu_init()` (`arm64/cpu.c:676-739`) / zeroed BSS | yes |
| `rtclock_intr` | `cpu_stat`, `rtcPop`, `rtclock_timer.deadline` | `cpu_init()` sets `rtclock_timer.deadline = EndOfAllTime` (`cpu.c:687-688`), `cpu_data_init()` (`arm_init.c:378`) | yes |
| `ml_set_decrementer` | `cpu_set_decrementer_func` (NULL ⇒ CNTV) , `cpu_decrementer` | `cpu_timebase_init(TRUE)` (`cpu.c:958-984`, `arm_init.c:665`) | yes |
| `timer_intr` | `processor` slot (`PERCPU_GET_RELATIVE`) | `processor_bootstrap()` → `processor_init()` (`kern/processor.c:233`, `244-313`) before `arm_init.c:499` | yes |
| `sleh_irq` DANDELION branch | `dandelion_gic` (mode/group) + `dandelion_gic_ready` | `PE_init_cpu()` → `pe_init_fiq()` (`arm_init.c:676`, `pe_identify_machine.c:443-451`, `pe_fiq.c:88-132`) | yes (if it returned `DGIC_OK`) |
| `sleh_irq` fallback | `cdp->interrupt_handler` | only `ml_install_interrupt_handler()` (`machine_routines.c:800-821`), i.e. an interrupt-controller **kext** | **no — always NULL on this port** |
| `sleh_interrupt_handler_prologue` | `current_thread()->task` | `kernel_thread_create` (`startup.c:604`) | yes |
| `entropy_collect` | `PERCPU_GET(entropy_cpu_data)` | `entropy_init` (`__startup_func`) or zeroed static | yes (harmless if not yet run: BSS) |
| MACH_ASSERT paths | `assert()`s are live in this build (proved by the on-device `zalloc.c` assertf, `zalloc.c:6668-6675`) | — | yes |

**Answer to the "is a first interrupt here survivable on Apple" question: yes, by design.**
Apple's own boot sequence unmasks at exactly this point (`startup.c:732` asserts interrupts are
still off *before* `STARTUP_SUB_EARLY_BOOT`; `startup.c:745-753` then unmasks), and
`zalloc.c:6668` makes it *illegal* to allocate from an interrupt-disabled context once
`startup_phase >= STARTUP_SUB_EARLY_BOOT`. Everything the entry path needs is built in `arm_init()`
hundreds of milliseconds earlier. The difference from Apple is not "interrupts must not be enabled
this early" — it is that on Apple something is **registered to consume** the interrupt
(`ml_install_interrupt_handler` from the platform kext, plus the Apple timer FIQ machinery in
`fiq_context_init`), whereas on this port `cdp->interrupt_handler` is NULL forever and the single
hard-coded INTID is the only thing that can be consumed.

---

## 2. What is *actually* fragile here (rather than uninitialised)

`getCpuDatap()` (`osfmk/arm/cpu_data.h:123`) is just `current_thread()->machine.CpuDatap`, and
`current_thread()` is a `TPIDR_EL1` read (`cpu_data.h:55-69`). Both are valid from
`arm_init.c:467-470` onward, i.e. long before `STARTUP_SUB_EARLY_BOOT`. **"Uninitialised per-CPU
data" is therefore *not* the fault** — unless the very first exception is itself raised before
`machine_set_current_thread()` (not the case here).

The real gaps are:

1. **Nothing owns the interrupt.** `cdp->interrupt_handler == NULL` (no kext is ever loaded; the
   port ships none, `pe_fiq.c` file comment lines 22-27). `sleh_irq`'s only real consumer is an
   equality test against one INTID.
2. **No source de-assertion for anything else.** The arch-timer PPI is *level*-sensitive in this
   port's configuration — the shim programs `GICR_ICFGR0/1 = 0` (`dandelion_gic.h:622-624`) and
   `dgic_enable_ppi` explicitly clears both ICFGR bits for `irq >= 16`
   (`dandelion_gic.h:726-735`). A level-sensitive line that is acked and EOIed without its source
   being cleared re-fires immediately and forever.
3. **The first interrupt is guaranteed to exist and to carry no deadline.** `fiq_context_init(TRUE)`
   at `arm_init.c:680` calls `_enable_virtual_timer()` (`machine_routines.c:2242-2250`):

   ```c
   uint64_t cntvctl = CNTV_CTL_EL0_ENABLE;              /* ENABLE=1, IMASK=0 */
   __builtin_arm_wsr64("CNTV_CTL_EL0", cntvctl);
   __builtin_arm_wsr64("CNTP_CTL_EL0", CNTP_CTL_EL0_IMASKED);
   ```

   It enables the virtual timer **without programming `TVAL`/`CVAL`**. The shim's last act before
   hand-off left `CNTV_TVAL = freq/500` (`bootshim/gic_probe.c:387-413`) and PPI 27 enabled
   (`gic_prepare_kernel_timer`, `gic_probe.c:286-312`; called from `shim.c:368`). So from
   `arm_init.c:680` onwards `CNTV_CTL.ISTATUS == 1` and PPI 27 is asserted with *no* kernel
   deadline behind it. At `spllo()` the IRQ is taken on the very next instruction.
   This is normal XNU on Apple (the CNTV enable is common code) and is survivable *provided the
   handler runs and reaches `setPop()`*, which reprograms `CNTV_TVAL` and drops the level.
4. `spllo()` clears **I, F and A** at once: `DAIFSC_STANDARD_DISABLE`
   (`osfmk/arm64/proc_reg.h:493`) = `ASYNCF|IRQF|FIQF`, applied in
   `ml_set_interrupts_enabled_with_debug` (`machine_routines_common.c:1074`). So an FIQ is
   unmasked at the same instant as an IRQ — relevant to §4.3.

---

## 3. Does the DANDELION handling match Apple's own GICv3 platform?

Apple's public GICv3 support exists only for `APPLEVIRTUALPLATFORM`/`VMAPPLE`
(`HAS_GIC_V3` is defined exclusively by `VMAPPLE.h:40`; the port re-defines it in
`DANDELION.h:77`, which is itself a hazard — see §4.6).

### 3.1 What Apple's `pe_fiq.c` sets up (`git show HEAD:pexpert/arm/pe_fiq.c`)

```c
// Configure timers and legacy FIQ to be group 0
gicr_write32(GICR_IGROUPR0, 0x81FFFFFF);      // bits 25..30 = 0 = Group 0
// Enable PPI 27
gicr_write32(GICR_ISENABLER0, (1 << 27));
uint64_t icc_sre = __builtin_arm_rsr64("ICC_SRE_EL1");  icc_sre |= ICC_SRE_SRE;  ...
__builtin_arm_wsr64("ICC_BPR0_EL1", 0);
__builtin_arm_wsr64("ICC_PMR_EL1", 0xFF);
uint64_t icc_ctlr = ...("ICC_CTLR_EL1");  icc_ctlr &= ~ICC_CTLR_EOIMODE;  ...   // EOImode = 0
gicd_ctlr |= GICD_CTLR_ENABLEGRP0;  gicd_write32(GICD_CTLR, gicd_ctlr);
__builtin_arm_wsr64("ICC_IGRPEN0_EL1", 1);
```

The decisive line is `GICR_IGROUPR0 = 0x81FFFFFF`: in GICv3 a `1` bit means Group 1, so INTIDs
**25..30 are Group 0** — the architectural-timer PPIs, including PPI 27 — and everything else
(SGIs, PPIs 16-24, 31, all SPIs) is Group 1. Apple therefore receives the timer as **FIQ** and all
device interrupts as **IRQ**, with the CPU interface configured for **Group 0 with IAR0/EOIR0**.

### 3.2 What Apple's exception handlers do

FIQ (`sleh.c`, upstream, `#if APPLEVIRTUALPLATFORM` at the pre-patch line 1920):

```
uint64_t iar = __builtin_arm_rsr64("ICC_IAR0_EL1");      // group 0
...
    cdp->cpu_decrementer = -1; /* Large */
    INTERRUPT_MASKED_DEBUG_START(rtclock_intr, DBG_INTR_TYPE_TIMER);
    rtclock_intr(TRUE);
    INTERRUPT_MASKED_DEBUG_END();
...
if (iar != GIC_SPURIOUS_IRQ) {
    __builtin_arm_wsr64("ICC_EOIR0_EL1", iar);
    __builtin_arm_isb(ISB_SY);
}
```

IRQ (`sleh.c`, upstream): **no GIC code at all**. `USE_APPLEARMSMP` → `PE_handle_ext_interrupt()`
→ `gCPUIC->externalInterrupt()` (`iokit/Kernel/arm/AppleARMSMP.cpp:387-391`), i.e. the closed
platform kext owns ack/eoi/dispatch; otherwise `cdp->interrupt_handler(...)` — the kext registered
with `ml_install_interrupt_handler`.

### 3.3 What our branch skips, concretely

| Apple does | DANDELION branch | Assessment |
|---|---|---|
| Classifies by *state* (`ml_get_timer_pending()`, `sleh.c:1933`) and treats any remaining FIQ as the timer | Classifies by exact INTID equality | Fragile: any other INTID is silently swallowed. Apple's approach fails *safe*, ours fails *loud-and-fatal* (§4.1) |
| `INTERRUPT_MASKED_DEBUG_START/END(rtclock_intr, DBG_INTR_TYPE_TIMER)` around the timer | absent (`sleh.c:1871-1882`) | With `SCHED_HYGIENE_DEBUG` off (this build) it is inert; treat as a missing hygiene step, not the killer |
| Group-0 register pair (`IAR0`/`EOIR0`) matching the group it enabled in `pe_fiq.c` | `ICC_IAR1/EOIR1` matching `ICC_IGRPEN1` — **consistent** | correct as written; the bug would be a *mismatch*, see §4.3 |
| `ICC_CTLR_EL1 &= ~EOIMODE` (read-modify-write, only EOImode touched) | `ICC_CTLR_EL1 = 0` (`dandelion_gic.h:685`), observed readback `0x400` → EOImode = 0, CBPR = 0 | equivalent in effect; the single `EOIR1` write is the right EOI for EOImode=0 (no `ICC_DIR_EL1` needed). *Not* the bug |
| — | No ack **loop**: one `IAR` read per entry | If two lines are pending, the second is taken as a second entry; acceptable, but see §4.1 for the "no clear → storm" interaction |
| — | No spurious guard in the IRQ branch at all | §4.2 |
| — | No group-enable readback at unmask time | §4.5 |
| — | `GIC_SPURIOUS_IRQ` is 1023, not 1020 | §0 |

So: **ack/eoi register selection is right, EOImode is right, priority mask is right. The
substantive divergence is that Apple's GICv3 code never has to cope with an interrupt whose source
it does not know, because a kext with a dispatcher table sits behind `cdp->interrupt_handler`, and
the only kernel-owned line (the timer) is *guaranteed* to be reprogrammed by `rtclock_intr`.**

---

## 4. Ranked candidate faults

Ranking logic: the hold-interrupts experiment produced a **clean panic with output**
(`FINDINGS-1027-EARLYBOOT.md` §第三轮), which proves `kprintf` is live at this point in boot.
Therefore any candidate that ends in a `panic()` would have printed. The observed death is
**silent**, so livelock / recursive-fault candidates outrank panic candidates.

### 4.1 — Most likely: no source de-assertion for any INTID that is not exactly 27 ⇒ level-triggered IRQ storm

**Mechanism.** PPI 27 (or any other line) is level-sensitive. `sleh.c:1871-1882` acks it, and:
- if `intid == 27` → dispatch → `timer_resync_deadlines()` → `setPop()` → `CNTV_TVAL` write drops
  the level (self-healing);
- if `intid != 27` → **only** `dandelion_gic_eoi(intid)` runs. The source is untouched, the level
  is still high, the GIC re-asserts the same INTID, the CPU takes the IRQ again immediately, and
  the loop never ends. No output, no panic, no return — exactly the observed signature.

The same loop occurs for `intid == 27` if the dispatch path cannot reach `setPop()`.

**Evidence.**
- The equality test is the only consumer: `sleh.c:1873`; `cdp->interrupt_handler` is NULL
  (`pe_fiq.c` file comment; no kext; the port's `DandelionPlatformExpert.cpp` never calls
  `ml_install_interrupt_handler`).
- Level-sensitivity: `dandelion_gic.h:622-624` (`ICFGR0/ICFGR1 = 0`) and
  `dandelion_gic.h:726-735`.
- The measured "second candidate INTID" exists and is *not* 27: `R1-GIC-PLAN.md` §0.0.2
  `e02 CNTP30 ... hppir1 0x1e`; `GICR_ISENABLER0` read `0x48000000` (bits 27 **and** 30) in E0.1.
  The shim's hand-off only clears/enables 27 (`gic_probe.c:298-303`), but the E0.2 test in the same
  boot *armed and expired* CNTP — if anything re-enables 30 (or leaves it enabled), the kernel sees
  INTID 30, which `sleh.c:1873` will never match.
- The port's own note that the DT is ambiguous between 27 and 30: `R1-GIC-PLAN.md:63-65`.

**Fix (do all four; they are cheap and independent).**

```c
/* osfmk/arm64/sleh.c, replacing the body of the #if defined(DANDELION) block in sleh_irq */

uint32_t hppir = __builtin_arm_rsr64("ICC_HPPIR1_EL1") & 0xffffffu;  /* non-destructive */
uint32_t intid = dandelion_gic_ack();

/*
 * Stop the level-sensitive timer lines from re-asserting *before* we EOI.
 * CNTV_TVAL/CNTV_CTL and CNTP_CTL are architected EL1 sysregs: no MMIO, so this
 * is legal here (an MMU-on GICR write is what stalls the bus - DEAD-ENDS #4).
 */
if (intid == DANDELION_TIMER_IRQ || intid == DANDELION_TIMER_IRQ_PHYS || intid >= 1020u) {
        uint32_t ctl = (uint32_t)__builtin_arm_rsr64("CNTV_CTL_EL0") & 3u;
        if (ctl & 1u) {                                  /* was enabled: mask it */
                __builtin_arm_wsr64("CNTV_CTL_EL0", ctl | 2u);
        }
        __builtin_arm_wsr64("CNTP_CTL_EL0", 0x3u);       /* disable + mask phys timer */
        __builtin_arm_isb(ISB_SY);
}

if (intid == DANDELION_TIMER_IRQ) {
        cdp->cpu_decrementer = -1; /* Large */
        INTERRUPT_MASKED_DEBUG_START(rtclock_intr, DBG_INTR_TYPE_TIMER);
        rtclock_intr(TRUE);
        INTERRUPT_MASKED_DEBUG_END();
} else if (intid < 1020u && cdp->interrupt_handler) {     /* was: < GIC_SPURIOUS_IRQ (1023) */
        cdp->interrupt_handler(...);
} else if (intid < 1020u) {
        /*
         * Nobody owns this line. EOI it so the CPU interface is not left with an
         * active interrupt, but make it visible and bounded: a level line we cannot
         * clear will come straight back.
         */
        dandelion_irq_unexpected(intid);                  /* see below */
}
dandelion_gic_eoi(intid);
```

Two notes on that snippet. (a) INTID 30 is masked off (`CNTP_CTL = 0x3`) but deliberately **not**
dispatched: once §5 has shown which timer actually fires, either drop the physical timer entirely or
add `|| intid == DANDELION_TIMER_IRQ_PHYS` to the `rtclock_intr` arm — do not leave a case where a
real tick is swallowed. (b) `cdp` is already in scope at `sleh.c:1852`; `hppir` is emitted only by the
§5 instrument, so drop it here if you are not taking the instrumentation.

plus a bounded-storm guard, because the mask above cannot help an SPI:

```c
/* pexpert/arm/pe_fiq.c */
static uint32_t dandelion_irq_budget;   /* reset to 0 whenever a *dispatched* IRQ completes */
void
dandelion_irq_unexpected(uint32_t intid)
{
        if (++dandelion_irq_budget == 1 || (dandelion_irq_budget % 1024u) == 0) {
                kprintf("DANDELION: unexpected INTID %u (hppir1 %u) after %u in a row\n",
                    intid, (unsigned)(__builtin_arm_rsr64("ICC_HPPIR1_EL1") & 0xffffffu),
                    dandelion_irq_budget);
        }
}
```

and in `DANDELION.h`, name the physical PPI so the intent is explicit:

```c
#define DANDELION_TIMER_IRQ        27   /* CNTV -> PPI 27, what XNU's decrementer drives   */
#define DANDELION_TIMER_IRQ_PHYS   30   /* CNTP -> PPI 30, only ever asserted by probes    */
```

### 4.2 — Second: the ack itself returns 1020 (nothing is ever EOIed) ⇒ same storm, one level up

**Mechanism.** `dandelion_gic_ack()` returns 1020 without touching hardware when
`dandelion_gic_ready == 0` (`pe_fiq.c:70-77`), and `dandelion_gic_eoi(1020)` is a no-op
(`dandelion_gic.h:764-769`). The interrupt is therefore never acknowledged *and* never deactivated:
the CPU interface still has it pending, the level is still high, and the CPU re-enters forever.

This is reachable whenever the **shim** made interrupts deliverable but the **kernel's** bookkeeping
disagrees — and they are independent state. `gic_probe.c:286-312` enables PPI 27 and
`dgic_cpu_init(..., 1)` enables `ICC_IGRPEN1_EL1` in the shim; `pe_init_fiq()` merely redoes the
CPU interface and sets `dandelion_gic_ready = 1` *only* if `dgic_cpuif_init()` returns `DGIC_OK`
(`pe_fiq.c:104-114`). Any change that makes that function return `DGIC_ERR_SRE` (-5) or fault
earlier would produce exactly this: delivery works, handler is blind.

**Evidence.** `pe_fiq.c:70-86`; `dandelion_gic.h:632-709` (three return points, one of which is
`DGIC_ERR_SRE`); `dandelion_gic.h:757-760` (normalisation to 1020); and the historical fact that
this exact code path previously killed the machine by reading `ICC_IGRPEN0_EL1`
(`FINDINGS-2026-10-27.md` §1, `dandelion_gic.h:695-704`).

**Fix.** Make "ready" a hardware fact, not a software flag, and never return a value that causes a
silent no-op:

```c
/* pexpert/arm/pe_fiq.c */
uint32_t
dandelion_gic_ack(void)
{
        if (!dandelion_gic_ready) {
                /* Fall back to the raw register: a wrong INTID is far better than 1020,
                 * which makes the caller skip the EOI and livelock. */
                uint32_t raw = (uint32_t)(__builtin_arm_rsr64("ICC_IAR1_EL1") & 0xffffffu);
                if (!dandelion_gic_ack_warned) {
                        dandelion_gic_ack_warned = 1;
                        kprintf("DANDELION: ack with !ready, raw IAR1=%u\n", raw);
                }
                return raw;
        }
        return dgic_ack(&dandelion_gic);
}

void
dandelion_gic_eoi(uint32_t intid)
{
        if (!dandelion_gic_ready) {
                if (intid < 1020u) {
                        __builtin_arm_wsr64("ICC_EOIR1_EL1", (uint64_t)intid);
                        __builtin_arm_isb(ISB_SY);
                }
                return;
        }
        dgic_eoi(&dandelion_gic, intid);
}
```

plus, in `pe_init_fiq()`, verify instead of trusting: read back `ICC_IGRPEN1_EL1` and `ICC_PMR_EL1`
and only then set `dandelion_gic_ready`, printing the readback either way.

### 4.3 — Third: an FIQ arrives (Group 0) and the FIQ path acks the wrong group ⇒ FIQ storm

**Mechanism.** `spllo()` unmasks F **and** I (`proc_reg.h:493`). If any Group-0 interrupt is
delivered to this PE — the secure/physical timer PPI 29, a secure SPI, an EL3-routed line, or
anything the secure firmware left in Group 0 and enabled — the CPU takes `sleh_fiq`. The DANDELION
FIQ branch immediately does `dandelion_gic_ack()` → **`ICC_IAR1_EL1`** (`dandelion_gic.h:752-756`,
`group == DGIC_GROUP1`). For a Group-0 interrupt `IAR1` returns spurious (1020) → the new guard
`sleh.c:1958-1962` returns after `dandelion_gic_eoi(1020)`, which is a no-op → **the Group-0
interrupt is neither acked nor deactivated nor cleared** → FIQ re-fires forever.

Note also that the *pre-existing* upstream shape would be worse: had the port relied on
`#elif APPLEVIRTUALPLATFORM || HAS_GIC_V3` (`sleh.c:1963-1965`), the FIQ path would read
`ICC_IAR0_EL1` — architecturally UNDEFINED for a non-secure EL1 on a two-security-state GIC. That is
exactly the failure documented in `FINDINGS-2026-10-27.md` §1.

**Evidence.** `sleh.c:1947-1962`; `dandelion_gic.h:744-782`; `pe_fiq.c:99-114` (this port *only
ever* enables `ICC_IGRPEN1_EL1` for Group 1, so a Group-0 FIQ can only come from firmware state the
port does not control); `GICD_TYPER.SecurityExtn == 1` measured (`R1-GIC-PLAN.md` §1).

**Fix.** Make the exception handler group-aware and self-consistent — one function, one decision:

```c
/* osfmk/arm64/sleh.c */
#if defined(DANDELION)
        /* Which group did we actually enable? Never ack a group we did not enable. */
        if (__builtin_arm_rsr64("ICC_IGRPEN1_EL1") & 1u) {          /* Group 1 -> IAR1/EOIR1 */
                iar = dandelion_gic_ack();
        }
        /* Group 0 belongs to EL3 here (SecurityExtn == 1): reading IAR0/EOIR0 is
         * UNDEFINED. A Group-0 FIQ therefore cannot be serviced from EL1 - report
         * it once and halt rather than livelock in the vector. */
        if (iar >= 1020u) {
                dandelion_fiq_unexpected(/*hppir0 read is illegal*/ 0);
                sleh_interrupt_handler_epilogue();
                return;
        }
```

and, to keep the machine alive rather than loop, make the "cannot service" case loud:

```c
static void
dandelion_fiq_unexpected(uint32_t iar)
{
        static int once;
        if (!once++) {
                kprintf("DANDELION: unserviceable FIQ (iar1=%u, igrpen1=%llu, hppir1=%llu)\n",
                    iar,
                    __builtin_arm_rsr64("ICC_IGRPEN1_EL1"),
                    __builtin_arm_rsr64("ICC_HPPIR1_EL1") & 0xffffffu);
        }
}
```

(The `ICC_HPPIR0_EL1` read is deliberately *not* attempted: it is the UNDEFINED access that already
cost this port a day.)

### 4.4 — Fourth: the dispatch is reached and dies *inside* the timer path

**Mechanism.** If the INTID really is 27 (most likely, per §2.3), the death is downstream:
`rtclock_intr` → `timer_intr` → `timer_queue_expire` / `running_timers_expire` /
`timer_resync_deadlines` → `setPop` → `ml_set_decrementer`. Candidates inside, all silent-capable:

- `timer_intr` at `arm_timer.c:96-106` runs **expired timer callouts with interrupts disabled
  inside the IRQ**; a callout that blocks, takes a contended mutex, or frees memory trips
  `zalloc.c:6668` (`startup_phase >= STARTUP_SUB_EARLY_BOOT` and interrupts-check) — that path
  *does* print, so it would have been visible.
- `ml_set_decrementer` asserts `ml_get_interrupts_enabled() == FALSE`
  (`machine_routines.c:2032`) — true in the handler, fine.
- `ml_set_interrupts_enabled_with_debug` asserts `getCpuDatap()->cpu_int_state == NULL` whenever it
  is asked to *enable* interrupts (`machine_routines_common.c:1049`). Inside an IRQ,
  `cpu_int_state != NULL`, so **any nested `splx(TRUE)`-style unmask from the timer path is an
  instant assert**. `timer_resync_deadlines` does `spl_t s = splclock(); ... splx(s);`
  (`arm_timer.c:151,184`) — safe only because `splclock()` returned "already disabled". If any spl
  on this port is stubbed to a constant, this becomes a panic that *can* print.
- If `setPop()` is never reached (`timer_resync_deadlines` skips it when
  `deadline != EndOfAllTime && deadline == 0`), the level never drops and this collapses back into
  §4.1.

**Evidence.** The chain is fully quoted in §1.4; `fiq_context_init(TRUE)` at `arm_init.c:680`
guarantees the first interrupt carries no deadline (§2.3), which means the *first* pass through
`timer_intr` skips `timer_queue_expire` entirely and goes straight to
`rtcPop = EndOfAllTime; timer_resync_deadlines();` — the one code path in the kernel that must
convert "no timers" into a `CNTV_TVAL` write, and the only reason the level ever drops.

**Fix.** Do not rely on the interrupt to arm the decrementer for the first time. Program a known
deadline before unmasking, so the first IRQ has a real deadline behind it:

```c
/* osfmk/arm/arm_init.c, right after fiq_context_init(TRUE) */
#if defined(DANDELION)
        /*
         * fiq_context_init() enables CNTV without programming TVAL, so ISTATUS is
         * already 1 on this SoC (the shim left a compare value in the past). Give the
         * decrementer a real deadline before interrupts are ever unmasked, so the
         * first tick is a normal timer pop rather than a level that nothing owns.
         */
        {
                extern void setPop(uint64_t);      /* osfmk/arm/rtclock.h */
                setPop(mach_absolute_time() + 1000000ULL);   /* ~1 ms in timebase units */
        }
#endif
```

and add the two trace points that make §4.4 vs §4.1 decidable in one boot (see §5).

### 4.5 — Fifth: nothing re-checks the group/priority state at the moment of unmask

**Status: probably not the fault, but it is the cheapest invariant to add.** Measured state is
consistent: `ICC_SRE_EL1 = 1`, `ICC_PMR_EL1` writes `0xff` reads `0xf0`, `ICC_CTLR_EL1 = 0x400`,
`ICC_IGRPEN1_EL1 = 1` (`R1-GIC-PLAN.md:316-317`), PPI priority `0xa0` (`dandelion_gic.h:155`,
written by `dgic_fill_priorities`, `dandelion_gic.h:621`). `ICC_CTLR_EL1 = 0x400` means
`EOImode = 0` (bit 1 clear) and only 4 priority bits implemented — hence the `0xf0` PMR readback,
and hence `0xa0` is a legal, masking-passing priority. So priority drop, `EOImode`, and PMR are
**not** the killer.

What is missing is the *check*. `spllo()` (`startup.c:752`) enables interrupts with no assertion
that the CPU interface is actually ready for them.

Note a structural problem the fix has to work around: **`dandelion_gic_ready` and
`dandelion_gic.group` are `static` in `pexpert/arm/pe_fiq.c` (`pe_fiq.c:60-62`)**, so `sleh.c`
— the file that has to make the ack/eoi decisions — cannot see them; the only accessor exported
today is `dandelion_gic_uses_group0()` (`pe_fiq.c:64-68`, declared in `DANDELION.h:197-199`).
Add the two accessors first:

```c
/* pexpert/arm/pe_fiq.c */
int dandelion_gic_is_ready(void)   { return dandelion_gic_ready; }
int dandelion_gic_group(void)      { return dandelion_gic.group; }   /* DGIC_GROUP0/1 */
```

then the pre-unmask check, in `pe_fiq.c` (so it can read the statics) and called from
`startup.c` immediately before `spllo()`:

```c
#if defined(DANDELION)
void
dandelion_check_irq_ready(void)
{
        uint64_t sre = __builtin_arm_rsr64("ICC_SRE_EL1");
        uint64_t pmr = __builtin_arm_rsr64("ICC_PMR_EL1");
        uint64_t en1 = __builtin_arm_rsr64("ICC_IGRPEN1_EL1");
        uint64_t ctlr = __builtin_arm_rsr64("ICC_CTLR_EL1");
        uint64_t hppir = __builtin_arm_rsr64("ICC_HPPIR1_EL1") & 0xffffffu;
        kprintf("DANDELION: pre-unmask sre=%llu pmr=%llu igrpen1=%llu ctlr=%#llx "
            "hppir1=%llu ready=%d group=%d\n",
            sre, pmr, en1, ctlr, hppir, dandelion_gic_ready, dandelion_gic.group);
        if (!(sre & 1) || !(en1 & 1)) {
                kprintf("DANDELION: CPU interface not armed - NOT unmasking\n");
                /* keep DAIF masked; diagnose rather than die */
        }
}
#endif
```

`ICC_HPPIR1_EL1` printed *before* the unmask is the single most valuable number in this whole audit:
it is exactly "which INTID is about to hit us".

### 4.6 — Sixth: latent traps that are not today's fault but will bite

1. **`HAS_GIC_V3 = 1` in the board header** (`DANDELION.h:77`). Apple defines it *only* for
   `VMAPPLE` (`VMAPPLE.h:40`), and the upstream code guards Group-0 register accesses with
   `#elif APPLEVIRTUALPLATFORM || HAS_GIC_V3` (`sleh.c:1963, 2013`). The DANDELION `#if` currently
   shadows both, so nothing breaks today — but any future `#if HAS_GIC_V3` outside those two sites
   silently inherits Group-0/FIQ semantics. Prefer a separate `HAS_DANDELION_GIC` and keep
   `HAS_GIC_V3` undefined.
2. **`ICC_CTLR_EL1 = 0`** (`dandelion_gic.h:685`) vs Apple's read-modify-write. Same effect here,
   but it also clears `CBPR`; on a GIC where preemption is wanted later, this needs revisiting.
3. **`intid < GIC_SPURIOUS_IRQ` (1023)** — see §0.
4. **`entropy_collect()` before the epilogue** (`sleh.c:1894`): `PERCPU_GET(entropy_cpu_data)`
   plus `assert(sample_count <= ENTROPY_MAX_SAMPLE_COUNT)` (`entropy.c:232`). Safe while the
   per-CPU base is valid; it is a *third* per-CPU lookup on a path that already did two.
5. **`sleh_fiq`'s classification runs before the ack** (`sleh.c:1933` `ml_get_timer_pending()`
   reads `CNTV_CTL.ISTATUS`). With the guard added *after* the prologue but *before* any EOI, a
   spurious FIQ now costs a full prologue/epilogue round trip (KDBG + `task_vtimer_check`) per
   iteration — enough to make a storm look like a hang rather than a reset.

### 4.7 — Explicitly ruled out

| Candidate | Why it is not it |
|---|---|
| **Uninitialised per-CPU data / `getCpuDatap()`** | `TPIDR_EL1`, `CpuDatap`, `istackptr`, `cpu_stat`, `rtclock_timer` are all built in `arm_init()` (`arm_init.c:456-470`, `378`, `cpu.c:687-688`, `665`) — see the table in §1.5 |
| **`CpuDataEntries`** | `arm_init.c:462-465` for the master CPU, before anything can run on it |
| **`cpu_decrementer`** | `cpu.c:984` (`0x7FFFFFFF`) then `sleh.c:1874` (`-1`); it is only a bookkeeping mirror of `CNTV_TVAL`, read by `ml_get_decrementer` |
| **`VBAR_EL1` order** | set in `start.s:865`, before the MMU is even on, and never changed; nothing to do with the unmask. (`__ARM_KERNEL_PROTECT__` is not defined for this board, so the `exc_vectors_table` indirection at `locore.s:189-209` is not used) |
| **EOImode needing a second `ICC_DIR_EL1`** | readback `ICC_CTLR_EL1 = 0x400` → EOImode = 0 → single `EOIR1` is correct |
| **Priority mask blocking delivery** | PMR reads `0xf0`, PPI priority `0xa0`; and delivery demonstrably happens |
| **`icc_sre` not set at unmask** | `ICC_SRE_EL1 = 1` measured, and `dgic_cpuif_init` forces bit 0 before touching anything else (`dandelion_gic.h:656-664`) |
| **A `machine_slot` hole** | the array does not exist in this kernel |

---

## 5. The one experiment that separates the top candidates

**Goal:** decide in a single boot between

- **C1** "the INTID that arrives is not consumable (mismatch / spurious / wrong group) → the level
  is never cleared → storm" (§4.1, §4.2, §4.3), and
- **C2** "the INTID is 27 and the death is inside the timer dispatch" (§4.4).

**Instrument (one hunk, `osfmk/arm64/sleh.c`, top of the DANDELION block in `sleh_irq`):**

```c
{
        static volatile uint32_t irq_n;
        uint32_t n = ++irq_n;

        /* (1) Read the pending INTID *without* consuming it. */
        uint32_t hppir = (uint32_t)(__builtin_arm_rsr64("ICC_HPPIR1_EL1") & 0xffffffu);

        /* (2) Consume it. */
        uint32_t intid = dandelion_gic_ack();

        /* (3) Record everything, cheaply, before anything can go wrong.
         *     ready/group need the accessors added in 4.5. */
        if (n <= 4) {
                kprintf("[irq%u] hppir1=%u iar1=%u ready=%d grp=%d sre=%llu pmr=%llu "
                    "ctlr=%#llx en1=%llu cntv_ctl=%llu cntv_tval=%llu\n",
                    n, hppir, intid, dandelion_gic_is_ready(), dandelion_gic_group(),
                    __builtin_arm_rsr64("ICC_SRE_EL1"),
                    __builtin_arm_rsr64("ICC_PMR_EL1"),
                    __builtin_arm_rsr64("ICC_CTLR_EL1"),
                    __builtin_arm_rsr64("ICC_IGRPEN1_EL1"),
                    __builtin_arm_rsr64("CNTV_CTL_EL0"),
                    __builtin_arm_rsr64("CNTV_TVAL_EL0"));
        }
        /* (4) Belt and braces: survives a hang, read back by the shim next boot. */
        dandelion_trace_irq(n, hppir, intid);

        /* (5) Remove the level so the experiment cannot livelock, whatever happens next. */
        {
                uint64_t ctl = __builtin_arm_rsr64("CNTV_CTL_EL0");
                if (ctl & 1u) {
                        __builtin_arm_wsr64("CNTV_CTL_EL0", ctl | 2u);   /* IMASK */
                }
                __builtin_arm_wsr64("CNTP_CTL_EL0", 0x3u);               /* phys: off */
                __builtin_arm_isb(ISB_SY);
        }

        /* (6) then the existing dispatch + EOI, unchanged. */
```

Add `dandelion_trace_irq()` next to `dandelion_trace_mark()` in `osfmk/kern/printf.c` (the port's
existing persistent channel) so the record survives even if the machine never prints again; it is
the same mechanism that already proved where the boot dies.

**How to read the result** — the log line before the hang *is* the answer:

| Observed | Verdict |
|---|---|
| `[irq1] ... iar1=27 ...` and then `[step] early_boot: spllo returned` | **C1**, and specifically that the storm was the killer: the timer path did re-arm (`setPop`) and stopping the level was enough. Fix = §4.1's mask-before-EOI, permanently. |
| `[irq1] ... iar1=27 ...` and **no** further output | **C2**: the dispatch itself is fatal. The last number in the line tells you which stage: `cntv_ctl=5` means CNTV was enabled and expired with `TVAL=…` already in the past; you then bisect inside `rtclock_intr` with two more `dandelion_trace_mark()` calls (before/after `timer_intr`, before/after `timer_resync_deadlines`). |
| `[irq1] ... iar1=30 ...` | **C1 with the 27/30 mismatch**: §4.1 fix (dispatch 27 *and* 30, or make 30 unassertable by masking CNTP) is the fix. |
| `[irq1] ... iar1=1020 hppir1=1020 ...` | **C1/§4.2**: the ack is blind (`ready=0`) or the line is not in Group 1. `ready` and `en1` in the same line say which. |
| `[irq1] ... iar1=1020 hppir1=27 ...` | The interrupt *is* pending but not ackable from Group 1 → it is a **Group 0** line, i.e. §4.3; `hppir1=27` with `iar1=1020` cannot otherwise happen. |
| `[irq1]` repeated with `n` climbing rapidly (or only `[irq1]` then silence, with the trace ring full of `n` up to thousands) | **C1** confirmed as a storm; the INTID field names the line. |
| No `[irq1]` line at all, still silent | The exception never reaches `sleh_irq` (or the first `kprintf` cannot run) → the fault is in the vector or in the very first per-CPU instruction; re-check `TPIDR_EL1`/`istackptr` with a `DANDELION_STAGE_*`-style marker written by a 6-instruction inline stub at the top of `fleh_irq` (before `PUSH_FRAME`). |

Cheapest possible variant of the same experiment, if a full rebuild is expensive: keep everything
except step (3)'s `kprintf`, and print only `hppir1` and `iar1` from the **pre-unmask** hook of §4.5.
That single line (`DANDELION: pre-unmask ... hppir1=27`) already discriminates "which INTID is
armed" from "the handler cannot consume it", though it cannot separate §4.1 from §4.4.

---

## 6. Recommended order of work

1. §4.5's pre-unmask print run **once** — costs one boot, gives `hppir1` (the armed INTID) and the
   five CPU-interface registers in a single line, and cannot itself cause a storm.
2. §5's instrument (steps 1-5) — one boot, decides C1 vs C2 definitively.
3. Apply §4.1 (mask-before-EOI + dispatch 27/30 + `< 1020` + storm budget) and §4.2 (ack/eoi without
   the `ready` short-circuit). These are safe regardless of the outcome and remove the whole
   "silent livelock" class.
4. Apply §4.4's `setPop()` before unmask so the first tick has a deadline, and keep
   `INTERRUPT_MASKED_DEBUG_START/END` symmetry with Apple's FIQ path.
5. Only then revisit §4.3 (FIQ/wrong-group) — it is the least likely but the most expensive to
   debug later, because its failure mode is identical in appearance.

**Do not** "fix" any of this by writing `GICD_*`/`GICR_*` from the kernel: `DEAD-ENDS.md` §4 records
that an MMU-on write to that block stalls the bus and kills the boot silently — the same symptom
being diagnosed here. Every fix above uses only `ICC_*`/`CNT*_EL0` system registers or the shim.
