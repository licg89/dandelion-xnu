# GIC forensics: how does non-secure get Group 1 on this SoC?

Scope: why `GICD_CTLR` reads `0x10` (`ARE_NS` set, `EnableGrp1NS` clear) at `bsd_init`, why the
timer PPI never becomes pending, and what - if anything - non-secure is allowed to do about it.
Everything here is either **measured on this phone**, **quoted from the architecture**, or **a
witness implementation**. Nothing is presented as measured that has not been booted yet.

Status line: the probe in `c2672e4` (`patches/xnu/zzz-dandelion-timer-probe.patch`) has **not been
run** when this was written. Its expected outcome is predicted below; the prediction is not a result.

How to read the citations:

| Tag | Meaning |
|---|---|
| **[M]** | measured on dandelion, in a log or probe kept in this repo |
| **[M-old]** | measured on dandelion in an earlier session; only the written record survives |
| **[SPEC]** | quoted architecture text (URL given) |
| **[IMPL]** | what an implementation does (Linux, EDK2, QEMU); evidence of intent, not of the architecture |
| **[INF]** | inference from the above; labelled so it can be attacked |

---

## 1. TL;DR - ranked conclusion

| # | Hypothesis | Verdict | One-line reason |
|---|---|---|---|
| **H1** | The kernel enables the wrong bit: with `ARE_NS==1` the NS group-1 enable is **bit 1**, and bit 0 is RES0. So `10 -> 10` is a correct read-back of an ignored write. | **Primary, untested on hardware** | [SPEC] NS view of `GICD_CTLR`; the repo already writes `\| 1u`; EDK2 and Linux both use bit 1 |
| **H2** | `EnableGrp1NS` was cleared *before* the kernel ran, by EDK2's `ArmGicDxe` ExitBootServices handler writing `0` to `GICD_CTLR`; `ARE_NS` survived because clearing it is UNPREDICTABLE, which is why the value is `0x10` and not `0x00`. | **Primary (explains the starting value)** | FV content list + `ArmGicDisableDistributor()` at `ArmGicCommonDxe.c:235`; `0x10` is exactly that footprint |
| **H3** | PPI 30 belongs to Group 0 / Group 1 Secure, so the tick should be taken as FIQ instead. | **Disfavoured** | The `GICR_ISENABLER0` bit-30 write *stuck*, which under `DS==0` only happens for an NS-owned INTID |
| **H4** | The `GICD_CTLR` write was dropped/read back too early because of `RWP`. | **Weak** | `RWP` tracks group-enable **1 -> 0** transitions, not 0 -> 1; the read itself had `RWP=0` |
| **H5** | `GICR_IGROUPR0` reading `0` means "non-secure cannot set the group", i.e. the redistributor is refusing us. | **Refuted as evidence** | NS reads/writes of that register are RAZ/WI when `DS==0` by architecture - it says nothing about PPI 30 |
| **H6** | We are programming the wrong redistributor (`GICR_TYPER` of the self frame reads `0`). | **Refuted** | `TYPER=0` *is* the correct value for the affinity-0 frame |
| **H7** | A MediaTek-specific gate between the timer and the redistributor (the `GIC_SYNC_DCM` idea) still swallows the PPI. | **Possible second fault, currently unsupported** | The same chain measured good at shim time; only worth pursuing if H1 lands and nothing arrives |

The single decisive experiment is **write bit 1 and then try to acknowledge INTID 30 through
`ICC_IAR1_EL1`**. Reading back `0x12` is corroboration; the acknowledge is the proof. The build
that does this (`c2672e4`, "Experiment A" + "Experiment B") is already pushed.

---

## 2. What is actually measured on this device

From `out/kernel-boot.log` (UTF-16; decode with `py -3.13`. Line numbers below are decoded line
numbers, not byte offsets):

```
3545 [dl] gic late: gicd typer=7904f4 ctlr=10 (bit0=EnableGrp1NS)
3547 [dl] gic late: redist=fffffff486f6f000 typer=0 waker=0
3549 [dl] gic late: igroupr0=0 (bit30)
3551 [dl] gic late: isenabler0=40000000 igroupr0=0 icfgr1=20000000
3553 [dl] gic late: ispender0=0 iactiver0=0 igrpen1=1
3555 [dl] gic late: gicd ctlr 10 -> 10
3557 [dl] gic late: hppir1=3ff
3559 [dl] gic late: done, timer INTID 30 enabled
3561 [dl] regs daif=200 ctl=2 tval=6e4f0425 sre=1 pmr=f0 igrp1=1
3563 [dl] regs hppir1=3ff(1023=spurious) rpr=ff
3565 [dl] timerprobe: before IOSleep(200)
3574 DANDELION: gic pidr2=0 arch=3 typer=0 gicr_typer=0
3576 DANDELION: gic ctlr 0 -> 0, igroupr 0 -> 0
```

Decoded, field by field:

| Value | Meaning | Tag |
|---|---|---|
| `GICD_TYPER = 0x7904f4` | `ITLinesNumber=0x14` -> 672 INTIDs; `SecurityExtn` (bit 10) **= 1** -> the *two* Security states are implemented | [M] |
| `GICD_CTLR = 0x10` | `ARE_NS=1` (bit 4), `EnableGrp1NS=0`, `RWP=0`; bit 6 reads 0, but the NS view has no `DS` bit at all (see §3.1), so `DS` is settled from `GICD_TYPER`, not from here | [M] |
| write `ctlr\|1` -> read `0x10` | the write changed nothing. `1` is bit 0 | [M] |
| `GICR_WAKER = 0` | `ProcessorSleep=0`, `ChildrenAsleep=0`: this PE's redistributor is awake | [M] |
| `GICR_ISENABLER0 = 0x40000000` | bit 30 write **took** | [M] |
| `GICR_IGROUPR0 = 0` (before and after write) | unchanged by an NS write of `1<<30` | [M] |
| `GICR_ICFGR1 = 0x20000000` | bits [29:28] = `0b10`: PPI 30 is level-sensitive | [M] |
| `GICR_ISPENDR0 = 0`, `IACTIVER0 = 0` | nothing pending, nothing active - see §9, this is uninformative here | [M] |
| `ICC_IGRPEN1_EL1 = 1`, `ICC_SRE_EL1 = 1`, `ICC_PMR_EL1 = 0xf0` | CPU interface is open for Group 1 | [M] |
| `DAIF = 0x200` | only `D` masked: IRQs are enabled | [M] |
| `ICC_HPPIR1_EL1 = 0x3ff` | the CPU interface has no Group 1 interrupt to give | [M] |
| `CNTV_CTL = 0x2` | the value logged as `ctl=2` is **`CNTV_CTL_EL0`** (`patches/xnu/non-apple-bsd-init-trace.patch:68`): `ENABLE=0`, `IMASK=1` | [M] |
| `GICR_TYPER = 0` (self frame) | correct: affinity 0, `Processor_Number` 0, `Last` 0 (see §9) | [M] |

Two structural facts about the run, both from the same log:

* the boot does **not** stop dead at `IOSleep(200)`: other kernel threads keep printing
  (`3574`-`3580`, the platform expert), and then the log simply ends. A missing tick parks the
  sleeping thread; it does not wedge the machine.
* two different readers of `GICD_CTLR` inside the same kernel disagree: `0x10` at `3545` and `0x00`
  at `3576`. The second one or its base is wrong; see §8.1. Only the `[dl]` reader (which maps
  `0x0c000000` with `ml_io_map`, `patches/xnu/dandelion-gic-mode.patch:268`) can be trusted.

---

## 3. The architecture that decides the three questions

### 3.1 The GIC is in the two-Security-state configuration (`DS == 0`)

`GICD_TYPER.SecurityExtn` bit [10]: *"Indicates whether the GIC implementation supports two Security
states: When GICD_CTLR.DS == 1, this field is RAZ."*
([GICD_TYPER](https://arm.jonpalmisc.com/latest_sysreg/ext-gicd_typer))

Measured value is 1, and a field that is RAZ when `DS==1` cannot read 1 when `DS==1`. Therefore
`DS==0` - the GIC is *not* in the "security disabled" single-state mode. **[M] + [SPEC] + [INF]**

This matters because every "GICv2-style" habit in this repo only works when `DS==1`. Do not try to
read `DS` from non-secure to double-check: the NS view of `GICD_CTLR` has no `DS` bit at all
(bits [30:5] are RES0, see next section) - this is also why the logged comment
`ctlr=10 (bit0=EnableGrp1NS)` cannot be justified from the value.

### 3.2 In the NS view, bit 1 is the Group 1 enable and bit 0 is RES0

`GICD_CTLR`, *"When access is Non-secure, in a system that supports two Security states"*
([GICD_CTLR](https://arm.jonpalmisc.com/latest_sysreg/ext-gicd_ctlr)):

> **EnableGrp1A, bit \[1\]:** If ARE_NS == 1, then this bit is a read/write alias of the Secure
> GICD_CTLR.EnableGrp1NS bit. If ARE_NS == 0, then this bit is RES0.

> **EnableGrp1, bit \[0\]:** If ARE_NS == 0, then this bit is a read/write alias of the Secure
> GICD_CTLR.EnableGrp1NS bit. If ARE_NS == 1, then this bit is RES0.

Measured `ARE_NS == 1`, therefore:

* writing bit 0 is **architecturally a no-op** - it is RES0, so the read-back must show the old value;
* writing bit 1 sets `EnableGrp1NS`, the actual "forward Group 1 non-secure interrupts" switch.

**[SPEC] + [M] + [INF]** `0x10 -> 0x10` is exactly what an ignored bit-0 write must produce, and it
is *not* evidence that "the distributor is not ours".

Witnesses that this is what working software does:

* Linux, `drivers/irqchip/irq-gic-v3.c`, `gic_dist_init()`:
  `val = GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1A | GICD_CTLR_ENABLE_G1;` ... `/* Enable
  distributor with ARE, Group1, and wait for it to drain */ writel_relaxed(val, ...)`.
  With `include/linux/irqchip/arm-gic-v3.h` (`GICD_CTLR_ENABLE_G1A (1U << 1)`,
  `GICD_CTLR_ENABLE_G1 (1U << 0)`, `GICD_CTLR_ARE_NS (1U << 4)`) that word is `0x13`:
  Linux writes **both** bits so the same code works whether the GIC has one or two Security states.
  ([irq-gic-v3.c](https://raw.githubusercontent.com/torvalds/linux/master/drivers/irqchip/irq-gic-v3.c),
  [arm-gic-v3.h](https://raw.githubusercontent.com/torvalds/linux/master/include/linux/irqchip/arm-gic-v3.h)) **[IMPL]**
* EDK2 in *our own firmware tree*, `ArmPkg/Drivers/ArmGicDxe/GicV3/ArmGicV3Dxe.c:689-694`:
  `if ((GicDistributorCtl & ARM_GIC_ICDDCR_ARE) != 0) { MmioOr32 (..., 0x2); } else { MmioOr32 (..., 0x1); }`
  - "if affinity routing is already on, set bit 1; otherwise bit 0". **[IMPL]**
* the repo's own newer driver already has the right constant:
  `iokit/Kernel/arm/DandelionPlatformExpert.cpp:208`
  `constexpr uint32_t kGicdCtlrEnableGrp1Ns = 1u << 1;`. The *older* path does not:
  `pexpert/pexpert/arm64/dandelion_gic.h:73-79` documents the GICv2/DS==1 mapping
  ("bit0 = EnableGrp1NS") and `:537-542` writes `ARE_NS | ENABLE0 | ENABLE1`. **[repo]**

### 3.3 `ARE_NS` cannot be cleared (this is why the value is `0x10`, not `0x00`)

Same register page, Secure view:

> **ARE_NS, bit \[5\]:** ... Changing the ARE_NS settings from 0 to 1 is UNPREDICTABLE except when
> GICD_CTLR.EnableGrp1 Non-secure == 0. **Changing the ARE_NS settings from 1 to 0 is
> UNPREDICTABLE.** If GICv2 backward compatibility for Non-secure state is not implemented, this
> field is RAO/WI.

The same rule is quoted against IHI 0069H.b §2.3.3 in a Xen patch that exists purely because of it:
*"On a system where firmware has already enabled Non-secure affinity routing, a zero write clears the
Non-secure view's GICD_CTLR.ARE_NS bit. Arm IHI 0069H.b, section 2.3.3 ("Changing affinity routing
enables"), states that changing GICD_CTLR.ARE_NS from 1 to 0 is UNPREDICTABLE."*
([xen/arm: gic-v3: Preserve ARE_NS when disabling the distributor](https://patchew.org/Xen/0bc584018c025ee9fa5557276b9f3914ca9205d4.1779200467.git.mykola._5Fkvach@epam.com/))

**[SPEC] + [IMPL]** Consequence: a "disable the distributor" write of `0` (which our firmware does,
§6) clears `EnableGrp1NS` but leaves `ARE_NS == 1` - a `0x10`. The measured value is that footprint
exactly.

### 3.4 What `RWP` does and does not guarantee

> **RWP, bit \[31\]:** Register Write Pending. ... 0b1: Register write in progress. The effects of
> previous register writes to the affected register fields are not guaranteed to be visible ...
> This field tracks writes to: GICD_CTLR\[2:0\], the Group Enables, **for transitions from 1 to 0
> only**; GICD_CTLR\[7:4\], the ARE bits, E1NWF bit and DS bit; GICD_ICENABLER<n>.

**[SPEC]** Two consequences for our probe:

* waiting for `RWP==0` is the right thing to do before believing a read-back, and it is what both
  Linux (`gic_dist_wait_for_rwp()`) and TF-A do;
* but a **0 -> 1** group-enable write is *not* tracked by `RWP`, so `RWP==0` does not prove that a
  0 -> 1 write has landed. It can only falsify H4 (a write still in flight), not confirm the write.
  The confirmation has to be behavioural: read the register again later, and - decisively - take the
  interrupt.

Measured context: the previous `10 -> 10` read already had `RWP==0` (`0x10`), and writes to
`GICR_ISENABLER0`/`GICR_ICFGR1` from the same function did take. So H4 was never strong.

### 3.5 Whether an INTID is Group 1 NS is decided by the Secure world

Distributor side, [GICD_IGROUPR<n>](https://arm.jonpalmisc.com/latest_sysreg/ext-gicd_igrouprn):

> When GICD_CTLR.DS==0, the register is RAZ/WI to Non-secure accesses.

and, for our INTID range:

> For SGIs and PPIs: When ARE is 1 for the Security state of an interrupt, the field for that
> interrupt is RES0 ... **Equivalent functionality is provided by GICR_IGROUPR0.**

**[SPEC]** for the distributor register; the redistributor's `GICR_IGROUPR0` is the same idea one
level down. QEMU's model of the redistributor spells out what that means in practice
([arm_gicv3_redist.c](https://raw.githubusercontent.com/qemu/qemu/master/hw/intc/arm_gicv3_redist.c)):

* `GICR_IGROUPR0`: an NS read returns `0` and an NS write is ignored whenever `DS==0`;
* `GICR_ISENABLER0` / `ISPENDR0` / `ICFGR1` / `IPRIORITYR` are masked for NS access by the *group
  bitmap itself* (`mask_group()` returns `cs->gicr_igroupr0`): bits for Group 0 / Group 1 Secure
  INTIDs read `0` and ignore writes. **[IMPL]**

That last line is what turns our own measurement into evidence: `GICR_ISENABLER0` bit 30 **stuck**
(`0x40000000`, `[M]`). Under the masking rule that can only happen if INTID 30 is already
Group 1 NS - i.e. LK/ATF assigned it, which is also what makes Android's `/proc/interrupts` line
`GICv3 30 Level arch_timer` possible (`docs/R1-GIC-PLAN.md:170`) [M-old]. Under a hypothetical
`DS==1` GIC the write would also stick, but §3.1 excludes `DS==1`.

Also note the flip side, which is a trap the repo already fell into once: because NS cannot see or
write the group state, **no NS read of `GICR_IGROUPR0` can ever tell us anything**. `0` is the only
value non-secure can observe on a two-state GIC, whether the INTID is Group 1 NS or Group 0.

### 3.6 Which INTID is the EL1 physical timer

PPI 30 (`INTID 16 + 14`) is the non-secure EL1 physical timer. On this device: Android's
`/proc/interrupts` reports `GICv3 30 Level arch_timer` on all eight CPUs
(`docs/R1-GIC-PLAN.md:170`), the SGI/PPI wiring matches the MT6765 device tree, and the shim
measured CNTP expiry producing `ispender` bit 30 and `hppir1 = 0x1e` (30)
(`docs/R1-GIC-PLAN.md:78-88`). [M-old]

The kernel takes it as Group 1 NS => IRQ, not FIQ, so `ICC_IAR1_EL1`/`ICC_EOIR1_EL1` is the correct
pair and no Group 0 register may be touched (`ICC_HPPIR0_EL1`, `ICC_IGRPEN0_EL1`, `ICC_IAR0_EL1`
trap as undefined from EL1 NS - that killed the kernel once,
`patches/xnu/dandelion-gic-mode.patch:362-369`, `docs/CORRECTIONS.md` C2).

---

## 4. Hypotheses, evidence, and how each would be settled

### H1 - the enable write targets the wrong bit (primary)

* **Claim.** `GICD_CTLR.EnableGrp1NS` is bit 1 in the NS view when `ARE_NS==1`; bit 0 is RES0. The
  kernel writes bit 0, so nothing is enabled and `10 -> 10` is a faithful read-back.
* **Evidence.** [SPEC] §3.2; [M] `ARE_NS=1` and `10 -> 10`; [IMPL] EDK2 `ArmGicV3Dxe.c:690-691`
  (bit 1 whenever `ARE` is on), Linux `gic_dist_init()` (`0x13`); [repo] the OLD code at
  `patches/xnu/dandelion-gic-mode.patch:386-391` wrote `ctlr | 1u`, and the comment above it
  ("EnableGrp1NS is the one distributor bit non-secure is allowed to write") shows the wrong bit
  was a deliberate choice, not a typo. **[M-old]** independent witness: the BootShim probe wrote
  `0x13` and read back `0x12` (`bootshim/gic_probe.c:837-841`, recorded in
  `docs/HANDOVER-DAY2.md:16`, `docs/NEXT-STEPS.md:66` and `:93`, `docs/R1-GIC-PLAN.md:161`) - bit 1
  stuck there, bit 0 did not.
* **Decisive experiment.** Write `ctlr | 0x2` (or `0x3`, as Linux does), poll `RWP`, read back, and
  then arm CNTP and try to ack INTID 30 via `ICC_IAR1_EL1`. `10 -> 12` is corroboration; `IAR1 == 30`
  is the proof.
* **Status.** Not yet run. Probe A+B in `c2672e4` is exactly this.

### H2 - the firmware cleared it before we arrived (primary, explains the starting value)

* **Claim.** The `0x10` was produced by a firmware "disable the distributor" write of `0`, which
  cannot clear `ARE_NS`, so it left `ARE_NS=1, EnableGrp1NS=0`.
* **Evidence.** See §6 for the full chain with file:line. Summary: `ArmGicDxe` is in our FV
  (`Platforms/Xiaomi/dandelionPkg/Include/DXE.inc:26`, included by `dandelion.fdf:89`, and present
  as an `ArmGicDxe` FFS entry in a build's `FVMAIN.inf`); its ExitBootServices handler calls
  `ArmGicDisableDistributor()` (`ArmGicV3Dxe.c:739`), which is
  `MmioWrite32 (GicDistributorBase + ARM_GIC_ICDDCR, 0x0)` (`ArmGicCommonDxe.c:235`).
* **Decisive experiment.** Cheap, from the firmware side: print `GICD_CTLR` in an
  `EVT_SIGNAL_EXIT_BOOT_SERVICES` notify registered at `TPL_CALLBACK` (runs after the GIC driver's
  `TPL_NOTIFY` handler), or simply in `PayloadBootDxe`'s existing `OnExitBootServices`
  (`Platforms/Xiaomi/dandelionPkg/Drivers/PayloadBootDxe/PayloadBootDxe.c:1095`). Expected `0x10`.
* **Status.** Not yet run; the mechanism is proven to *exist*, only the instance is inferred.

### H3 - PPI 30 is really a Group 0/1S interrupt, go FIQ (disfavoured)

* **Claim.** The timer belongs to the secure world; the tick must be taken as FIQ, and Group 1 work
  is pointless.
* **Why disfavoured.** Two independent contradictions: (a) `GICR_ISENABLER0` bit 30 write stuck (see
  §3.5) - under `DS==0` that is only possible for an NS-owned INTID; (b) at shim time, forcing
  `GICR_ISPENDR0` bit 30 made `ICC_HPPIR1_EL1` report 30 (`docs/R1-GIC-PLAN.md:78-88`), and
  `HPPIR1` only ever reports Group 1 - non-secure Group 1 from an NS read.
* **What would revive it.** The new `ICC_HPPIR1` value after a real CNTP expiry staying `0x3ff`
  *while* `GICR_ISPENDR0` bit 30 is set, together with `ICC_HPPIR0_EL1` (unreadable from NS) - i.e.
  it can no longer be tested directly; the practical test is whether the FIQ entry point ever sees
  INTID 30.
* **Status.** Effectively excluded by (a)+(b); do not spend a build on FIQ until H1's acknowledge
  test fails.

### H4 - `RWP` / stale read-back (weak)

Covered in §3.4. `RWP` does not track 0 -> 1 group writes, the earlier read had `RWP==0`, and other
writes in the same function landed. Keep the poll (it is free and it is what Linux does); do not
expect `spin > 0`.

### H5 - "`GICR_IGROUPR0` reads 0, therefore non-secure cannot set the group" (refuted as evidence)

It is true that the write is ignored - but by architecture, not by refusal, and the same register is
invisible to NS whatever the real group is (§3.5). The repo already corrected this once
(`docs/CORRECTIONS.md` C1, `docs/R1-GIC-PLAN.md:48`); this document confirms the mechanism and adds
the primary citation. **Do not use `IGROUPR0` as a diagnostic again.**

### H6 - wrong redistributor frame / `TYPER == 0` anomaly (refuted)

* `GICR_TYPER` for the frame whose affinity is 0 legitimately reads `0`: the affinity lives in
  `[63:32]` (`GICR_TYPER_AFFINITY`), the processor number in `[23:8]`, `Last` in bit 4
  ([arm-gic-v3.h](https://raw.githubusercontent.com/torvalds/linux/master/include/linux/irqchip/arm-gic-v3.h) -
  `GICR_TYPER_AFFINITY GENMASK_ULL(63, 32)`, `GICR_TYPER_CPU_NUMBER(r) (((r) >> 8) & 0xffff)`,
  `GICR_TYPER_LAST (1U << 4)`). Affinity 0 + CPU 0 + not-Last = `0x0`.
* The earlier scan supports this: frames read `0x100000100`, `0x200000200`, ... `0x10300000710`
  (`docs/R1-GIC-PLAN.md:109-117`), i.e. CPU1..CPU7 - so the *missing* affinities are exactly the
  expected ones, with CPU0's frame at `0x0c100000` reading zero. **[M-old]**
* Independent confirmation that the frame is ours: our own writes to `ISENABLER0`/`ICFGR1` in that
  frame read back as written **[M]**, `GICR_WAKER` reads `0` (awake) **[M]**, and
  `MPIDR_EL1` affinity is 0 (`docs/R1-GIC-PLAN.md:108`).

### H7 - a MediaTek gate between timer and redistributor (open, low priority)

The hypothesis that killed R1 originally (`docs/R1-GIC-PLAN.md:59-65`, the `GIC_SYNC_DCM` idea).
Counter-evidence: at shim time the full path CNTP -> `GICR_ISPENDR0` -> `ICC_HPPIR1_EL1 = 30`
measured good (`docs/R1-GIC-PLAN.md:73-88`), and the vendor Linux driver contains no MediaTek MMIO
in the GIC path at all (`docs/research/mtk6765-drivers.md:1101-1105`). It becomes the primary
suspect only if H1 lands (bit 1 set, read-back `0x12`) and INTID 30 still never reaches
`ICC_IAR1_EL1` while `GICR_ISPENDR0` bit 30 is set.

---

## 5. The experiment that is running now (`c2672e4`)

`patches/xnu/zzz-dandelion-timer-probe.patch` runs three sub-experiments in
`dandelion_gic_late_init()`:

* **A** (`:153-172`): write `before | DGICD_CTLR_ENABLE0 | DGICD_CTLR_ENABLE1` (= `0x3`), poll `RWP`,
  print `ctlr rwp-waited %x -> %x (spin rwp)`.
* **B** (`:187-201`): `dgic_timer_test()` - arm CNTP for 1 kHz, bounded `CNTPCT` spin, ack through
  `ICC_IAR1_EL1` - with `GICR_ISPENDR0` sampled immediately before and after.
* **C** (`:220-244`): SGI to self with `ICC_IGRPEN1=0`, then look for `GICR_ISPENDR0` bit 9.

Reading the outcome (this is the decision table; none of it is measured yet):

| Log | Reading |
|---|---|
| `ctlr rwp-waited 10 -> 12` **and** `cntp test=1` or `3` | H1 confirmed, H2 corroborated. The blocker is closed; next stop is `bsd_init` past `IOSleep`. |
| `10 -> 12` but `cntp test=2` (expired, not delivered) | H1 is right but something downstream is still broken: priority/PMR, the redistributor-to-CPU-interface leg, or H7. Next experiment: `GICR_ISPENDR0`/`ICC_HPPIR1` sampled in the same spin, then `ICC_IAR1` for a *forced* pending PPI 30 to separate "not pending" from "pending but not delivered" - the shim already proved the forced-pending leg works. |
| `10 -> 10` | H1 is wrong and bit 1 is being refused too. Then the surviving candidates are H2 in a nastier form (something re-clears it), or an implementation that ignores NS group-enable writes for a reason not documented here. Decisive next step: return to the shim (MMU off) and repeat *exactly* this write there, because the shim is known to have written `0x13` successfully; a difference between shim and kernel at the same address would then be about mapping/attributes, not about permissions. |
| `spin` large / `rwp=1` | A write really is in flight; investigate `GICD_ICENABLER`/`GICD_CTLR` writes earlier in the firmware handoff. |

Two caveats about sub-experiments, so nobody mis-reads a negative:

1. **B is only as good as the timer.** `dgic_timer_test()` (`pexpert/pexpert/arm64/dandelion_gic.h:871-928`)
   reports bit 1 from `CNTP_CTL.ISTATUS` and bit 0 from an `IAR1` match, so `test=2` (expired,
   not delivered) is a real end-to-end statement - but `test=0` only says "not within 20 ms".
2. **C can produce a false negative.** An SGI's delivery to the redistributor is gated by
   `GICR_NSACR` when the sender is non-secure (`gicr_ns_access()` in QEMU's model; if the SGI's
   group needs NS access and the field is < 2 the SGI is dropped). So `bit9=0` does **not** prove
   "Group 1 NS cannot be pended here"; `bit9=1` *is* proof that it can.

Also worth noting: Experiment A's write of `0x3` is what Linux does, so a successful `10 -> 12`
is the state Linux would have created on its own boot - see §7.

---

## 6. Who cleared `EnableGrp1NS`: the `ArmGicDxe` chain

1. `Platforms/Xiaomi/dandelionPkg/Include/DXE.inc:26` - `INF ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf`.
   `DXE.inc` is the FV *content* list (`docs/CORRECTIONS.md` C13), and `dandelion.fdf:89`
   `!include Include/DXE.inc` pulls it into `[FV.FvMain]`.
2. A real build agrees: `FV/FVMAIN.inf` of the dandelion build carries
   `.../Ffs/DE371F7C-DEC4-4D21-ADF1-593ABCC15882ArmGicDxe/DE371F7C-...ffs` among its 53 FFS entries.
   (Caveat, honestly: the only `FVMAIN.inf` left in this workspace is from the 2026-10-28 build tree
   `D:\aios\uefi-a04br3`, and `docs/CORRECTIONS.md` C16 warns that the packaged FD can come from a
   different tree. I could not re-verify the newest FD directly because `FVMAIN` is compressed: a
   GUID search for *known-present* drivers such as `PlatformProgressDxe` and `DandelionConsoleDxe`
   is equally fruitless in `artifacts/SILICIUM_UEFI.fd`, which reproduces C14's lesson rather than
   proving absence.)
3. `ArmGicDxe.c:77-80` picks the V3 path whenever the GICv3 system registers exist (they do - the
   kernel reads `ICC_SRE_EL1 = 1`), i.e. `GicV3DxeInitialize()`.
4. `GicV3DxeInitialize()` at `:807` does `MmioOr32 (GICD_CTLR, ARM_GIC_ICDDCR_ARE)` - harmless,
   `ARE_NS` is already set by LK - and at `:895` `ArmGicEnableDistributor()` which, because `ARE`
   is on, sets bit 1 (`:689-694`). So UEFI *enables* it while running.
5. `GicV3DxeInitialize()` registers the exit handler through
   `GicCommonInstallAndRegisterInterruptService(...)` (`:916`), which creates an
   `EVT_SIGNAL_EXIT_BOOT_SERVICES` notify at `TPL_NOTIFY` (`ArmGicCommonDxe.c:184-190`).
6. That handler, `GicV3ExitBootServicesEvent()` (`:718-740`), disables the interrupt interface
   (`:736`) and then calls `ArmGicDisableDistributor()` (`:739`) =
   `MmioWrite32 (GicDistributorBase + ARM_GIC_ICDDCR, 0x0)` = `ArmGicCommonDxe.c:235`.
7. Writing `0` to `GICD_CTLR` from NS with `DS==0` clears `EnableGrp1NS` and cannot clear
   `ARE_NS` (§3.3) => `0x10`. This is exactly what the kernel reads at `out/kernel-boot.log:3545`.

This is also why the shim-era value was `0x12`/`0x13`-ish and the kernel-era value is `0x10`: the
shim runs *before* UEFI. Nothing between LK and the shim writes a `0`; UEFI does.

Note the handler is not a bug in UEFI: a boot loader must not hand a payload an interrupt-enabled
machine. Linux does the same thing to itself, deliberately: `gic_dist_init()` starts with
`writel_relaxed(0, base + GICD_CTLR)` ("Disable the distributor") and then writes `0x13`. The
difference is only that Linux finishes the job.

---

## 7. Fix in firmware, or re-enable from EL1 non-secure?

**Re-enable from EL1 NS.** Reasons, in order of weight:

1. It is legal and expected. The NS view of bit 1 is, verbatim, "a read/write alias of the Secure
   GICD_CTLR.EnableGrp1NS bit" (§3.2). Linux does exactly this from EL1 NS on every boot; if it
   were forbidden, no non-secure OS could ever start its own tick on a two-state GIC.
2. The architectural state we find (`0x10`) is not a malfunction - it is the normal post-firmware
   state, and the OS is the component that is supposed to move it to `0x12`. A firmware-side fix
   would leave the same trap for the next payload (and for a future `ExitBootServices` in a
   different driver order).
3. Cost: one store, testable on the next boot with no UEFI rebuild, no reflash of `p1`, no risk to
   the log channel. Firmware changes here cost a full FD build plus the C16 "did I flash the right
   image" ritual.
4. Nothing in our chain writes `GICD_CTLR` after us, so the enable stays: the only other writer in
   the kernel is `dgic_distributor_init()` (`pexpert/pexpert/arm64/dandelion_gic.h:521-542`) and its
   only caller is `dgic_cpu_init()` (`:823-837`), which has **no call sites** anywhere in the tree
   (only `bootshim/gic_probe.c:724` calls the distributor init directly). After
   `gbs->ExitBootServices` the firmware is gone; the Secure world would have to take an interrupt to
   touch the register, and even then it cannot clear `ARE_NS`.

Firmware-side work is still worth exactly one line of belt-and-braces *later*, not now: if a second
`ExitBootServices` path ever appears (or if we add SMP and a CPU comes up through UEFI's PSCI
again), the same clear could repeat. A debug print in `PayloadBootDxe::OnExitBootServices`
(`PayloadBootDxe.c:1095`) is the cheap way to detect that, and it also settles H2.

One thing **not** to do: remove `ArmGicDxe` from the FV. UEFI needs it (TimerDxe's tick, the
`GPIO`/`Pmic` drivers' waits, BDS timeouts), and its session-scoped disable is correct behaviour.

---

## 8. Secondary defects found on the way (not the blocker)

### 8.1 A second GIC reader in the kernel reads zeros

`out/kernel-boot.log:3574-3576`:

```
DANDELION: gic pidr2=0 arch=3 typer=0 gicr_typer=0
DANDELION: gic ctlr 0 -> 0, igroupr 0 -> 0
```

Those values come from globals filled by the platform-expert path
(`patches/xnu/non-apple-platform-expert-fallback.patch:29-48`, printing
`dandelion_gic_ctlr_pre/post`). `0x00` for `GICD_CTLR` contradicts `0x10` measured 30 lines earlier
in the same boot, and `pidr2=0` contradicts `GICD_PIDR2 = 0x3b` measured in earlier sessions
(`docs/R1-GIC-PLAN.md:159`). So that reader is not looking at the distributor: its base defaults to
the *physical* constant `DANDELION_GIC_BASE` (`iokit/Kernel/arm/DandelionPlatformExpert.cpp:66`,
`:422-423`) unless `dandelion_platform_set_mmio_map()` installs kernel VAs, and it clearly was not
installed. Reading a physical address with the MMU on and no mapping returning `0` instead of
faulting needs to be understood, but nothing depends on it today.

Notable: that file has the **right** bit (`:208`, `:321`) and a real RWP poll (`:275-281`). If this
path is ever given a valid mapping it will program `0x12` correctly - and it will also, per
`:342-344`, clobber `GICR_ICENABLER0`/`IGROUPR0`/`IGRPMODR0` wholesale, so it must be reconciled with
`pe_fiq.c` rather than run alongside it.

### 8.2 Documentation drift on the timer INTID

`docs/CORRECTIONS.md` C5 states `DANDELION_TIMER_IRQ` must be 27 (`CNTV`) and that 30 "must be
explicitly masked". That is superseded: `pexpert/pexpert/arm64/DANDELION.h:107` is
`#define DANDELION_TIMER_IRQ 30` ("CNTP: what Linux uses on this GIC"), the patch
`572f300`/`fa0fda6` moved the kernel to CNTP/PPI 30, and the same header still carries a stale
comment at `:105` about "GIC PPI 11". C5 should be marked superseded when someone next edits
`CORRECTIONS.md`; it is not edited here because this document is the only file this task may write.

---

## 9. Claims that were believed, and are wrong

| Claim | Why it is wrong |
|---|---|
| "`10 -> 10` proves non-secure cannot open Group 1" | The write targeted bit 0, which is RES0 when `ARE_NS==1` (§3.2). The experiment never tested bit 1. |
| "`GICR_IGROUPR0` reads 0 after writing 1, so the group write is refused" | NS reads/writes of that register are RAZ/WI when `DS==0`; the value carries no information (§3.5). |
| "`GICR_ISPENDR0 = 0` means the timer PPI is not a Group 1 interrupt" | At that instant no timer was asserting: `ctl=2` is `CNTV_CTL` with `ENABLE=0, IMASK=1` (`non-apple-bsd-init-trace.patch:68`), and CNTP had not been armed in that code path at all. A pending bit can only be read while the source asserts (§3.5, level-triggered pending = latch OR line). |
| "The redistributor of CPU0 is broken, `GICR_TYPER` reads 0" | `0` is the correct value for affinity 0 / CPU number 0 / not last (§4 H6). |
| "Writing `GICD_CTLR` from the kernel hangs the bus" | Already corrected in `docs/CORRECTIONS.md` C2 / `docs/DEAD-ENDS.md` §4: the hang was `ICC_IGRPEN0_EL1`, a Group 0 *system* register, being undefined from EL1 NS. The current kernel writes `GICD_CTLR` and keeps logging (`out/kernel-boot.log:3555` then `3557`...), so MMIO to the distributor is not the problem. |
| "Writing `GICR_IGROUPR0` from the kernel stalls the bus" (still in `pexpert/pexpert/arm64/dandelion_gic.h:583-590`) | Measured false in the current boot: the kernel writes `1<<30` to it and the next three `[dl]` lines print normally (`out/kernel-boot.log:3549` -> `3551` -> `3553`). The write is *ignored* (§3.5), which is a different thing from hanging. |
| "Non-secure can never get the arch-timer PPI on this SoC" | Android on the same phone gets PPI 30 (`docs/R1-GIC-PLAN.md:170`), and the shim read `ICC_HPPIR1 = 0x1e` (`:78-88`). |

---

## 10. References

Architecture (quoted text):

* [GICD_CTLR, Distributor Control Register](https://arm.jonpalmisc.com/latest_sysreg/ext-gicd_ctlr) - NS-view bit map, `ARE_NS` UNPREDICTABLE rule, `RWP` semantics.
* [GICD_TYPER](https://arm.jonpalmisc.com/latest_sysreg/ext-gicd_typer) - `SecurityExtn` is RAZ when `DS==1`.
* [GICD_IGROUPR<n>](https://arm.jonpalmisc.com/latest_sysreg/ext-gicd_igrouprn) - RAZ/WI to NS when `DS==0`; SGI/PPI group lives in `GICR_IGROUPR0`.
* Arm IHI 0069H.b §2.3.3 "Changing affinity routing enables" and §12.9.4 (as cited by the Xen patch below).

Implementations (evidence of intent):

* Linux: [drivers/irqchip/irq-gic-v3.c](https://raw.githubusercontent.com/torvalds/linux/master/drivers/irqchip/irq-gic-v3.c) `gic_dist_init()`, and [include/linux/irqchip/arm-gic-v3.h](https://raw.githubusercontent.com/torvalds/linux/master/include/linux/irqchip/arm-gic-v3.h) for the bit names and `GICR_TYPER` fields.
* QEMU: [hw/intc/arm_gicv3_dist.c](https://raw.githubusercontent.com/qemu/qemu/master/hw/intc/arm_gicv3_dist.c), [hw/intc/arm_gicv3_redist.c](https://raw.githubusercontent.com/qemu/qemu/master/hw/intc/arm_gicv3_redist.c) - the NS access masks for `GICD_CTLR`, `GICR_IGROUPR0`, `GICR_ISENABLER0`, `GICR_ISPENDR0`.
* Xen: [xen/arm: gic-v3: Preserve ARE_NS when disabling the distributor](https://patchew.org/Xen/0bc584018c025ee9fa5557276b9f3914ca9205d4.1779200467.git.mykola._5Fkvach@epam.com/).

In this repo (path:line):

* `bootshim/gic_probe.c:837-847` - the `0x13` / `0x11` write-back probes.
* `docs/HANDOVER-DAY2.md:16`, `docs/NEXT-STEPS.md:66`, `docs/NEXT-STEPS.md:93`, `docs/R1-GIC-PLAN.md:161` - where `0x13 -> 0x12` is recorded ([M-old]; the raw evidence was a photo of the panel, there is no text log).
* `docs/R1-GIC-PLAN.md:73-88` - the shim's CNTP/CNTV -> `ISPENDR0` -> `HPPIR1 = 0x1e` measurement.
* `docs/R1-GIC-PLAN.md:109-117` - the redistributor frame scan.
* `docs/CORRECTIONS.md` C1 (group registers are RAZ/WI, not refused), C2 (the Group 0 system-register trap), C13 (DXE.inc is the FV content list), C14 (compressed FV defeats string/GUID searches), C16 (stale FD).
* `patches/xnu/dandelion-gic-mode.patch:268`, `:344-361`, `:362-379`, `:386-391` - the late init, the group/PPI programming, the Group 0 warning, the `| 1u` write ([M] for the current boot's output).
* `patches/xnu/zzz-dandelion-timer-probe.patch:132-249` - experiments A/B/C pushed as `c2672e4`.
* `patches/xnu/non-apple-bsd-init-trace.patch:66-83` - where `ctl=2` comes from (`CNTV_CTL_EL0`).
* `pexpert/pexpert/arm64/dandelion_gic.h:73-79`, `:521-542`, `:583-626`, `:823-837`, `:871-928` - register defs (with the GICv2 bit mapping), distributor init, redistributor init (and its stale "writing IGROUPR0 stalls the bus" comment), unused `dgic_cpu_init`, `dgic_timer_test`.
* `pexpert/pexpert/arm64/DANDELION.h:107` - `DANDELION_TIMER_IRQ 30`.
* `iokit/Kernel/arm/DandelionPlatformExpert.cpp:207-210`, `:275-324`, `:414-423` - the correct constants and RWP poll in the unused-but-correct path, and its mapping hook.
* `Platforms/Xiaomi/dandelionPkg/Include/DXE.inc:26`, `Platforms/Xiaomi/dandelionPkg/dandelion.fdf:89` - `ArmGicDxe` is in the FV.
* `D:\aios\uefi-build\Mu_Basecore\ArmPkg\Drivers\ArmGicDxe\` - `ArmGicV3Dxe.c:689-694`, `:718-740`, `:807`, `:892-895`, `:916`; `ArmGicCommonDxe.c:184-190`, `:228-236`.
* `out/kernel-boot.log` (UTF-16) lines 3545-3576 - the measured state in §2.

---

## 11. What would falsify H1

Any one of:

* `ctlr rwp-waited 10 -> 10` with `spin=0`, i.e. bit 1 also ignored;
* `10 -> 12` but `ICC_IAR1_EL1` never returns 30 for a `GICR_ISPENDR0`-confirmed pending PPI 30
  (that would move the fault to the redistributor-to-CPU-interface leg or to H7);
* a shim run (`bootshim/gic_probe.c` writes `0x13` there, MMU off) reading back something other
  than `0x12`, which would say the difference is about our mapping or cache attributes rather than
  about permissions.
