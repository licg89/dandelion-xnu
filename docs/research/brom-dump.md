# Dumping a raw partition from a Redmi 9A (MT6762G / MT6765) with no Android and no fastboot — mtkclient / BROM study

**Scope.** Can the host read the 16 MiB `flashinfo` partition (or any byte range) out of the phone using only
MediaTek BROM/preloader protocols, and does that give a practical "shim writes a log to flash, Windows reads it
back" loop?

**Date.** 2026-10-27. **Method.** Source reading of mtkclient `main` (config, exploit, DA, CLI), its docs, and
device-specific evidence from mtkclient issues + one third-party fix repository. **Nothing was run against the
phone.** No file outside `D:\aios\research\` was created or modified.

**Device under test.** Xiaomi Redmi 9A (`dandelion`, M2006C3LG), marketed as MT6762G / Helio G25; bootloader
unlocked; `flashinfo` = `mmcblk0p43`, 16 MiB
([HARDWARE_ANALYSIS.md](<D:/aios/dandelion-xnu/docs/HARDWARE_ANALYSIS.md>)); log layout already defined in
[mtk-msdc-pio.md](<D:/aios/research/mtk-msdc-pio.md>) §5.3 and decoded by
[msdc_log_decode.py](<D:/aios/research/msdc_log_decode.py>).

---

## 0. Verdict

**Yes — this works, and it is the right read-back path for this project — with one model-specific landmine
that has three documented workarounds.**

| Question | Answer |
|---|---|
| Can mtkclient dump `flashinfo` by name, no Android, no fastboot? | **Yes.** `python mtk.py r flashinfo flashinfo.bin` over BROM. 16 MiB, seconds of USB time. |
| Can it dump an arbitrary byte range? | **Yes**, two ways: partition-relative (`r … --offset/--length`) and absolute (`ro <offset> <len> <file>`). |
| Is MT6762/MT6765 a supported target? | **Yes, first-class.** BROM hwcode `0x766` → `MT6765/MT8768t` config, `var1=0x25` marked *confirmed*. The phone really reports `0x766` (see §2.2). |
| Does it need a vendor DA file? | **No.** Bundled `Loader/MTK_DA_V5.bin` (XFlash) + a **612-byte** kamakiri stub. It *does* need the phone's **preloader** (for its EMI/DRAM table) when entering via BROM. |
| Does it give a per-iteration loop? | **Yes:** ~45–75 s per cycle (≈15–30 s of that is holding buttons), versus ~3–5 min for the "flash stock boot → boot Android → `dd`" path. |
| The landmine | On 2 GB Micron LPDDR4X `dandelion`, stock DA1 halts at `Sending emi data …` (`DRAM setup failed: unpack requires a buffer of 12 bytes`) because of a 32-bit-vs-8-bit MR5 compare in `MTK_DA_V5.bin`. Three escapes: patched EMI block, `plstage`, or the DA-free `stage2` reader (§2.5, §3.5). |
| Physical actions per cycle | **3** (fastboot combo, force power-off, BROM combo + cable; drop the first if you flash over BROM). No screen photographing. |

---

## 1. mtkclient: what it is, how it connects, what it can do

### 1.1 What it is

A Python host tool (GPLv3) that speaks MediaTek's **BROM** (boot ROM USB protocol), **preloader** protocol, and
the **DA / XFlash / XML** download-agent protocols, plus several BROM exploits. It reads/writes eMMC and UFS,
edits `seccfg`, dumps BROM/preloader, and can mount partitions over FUSE.
Sources: [README.md](https://github.com/bkerler/mtkclient/blob/main/README.md),
[README-USAGE.md](https://github.com/bkerler/mtkclient/blob/main/README-USAGE.md).

### 1.2 The three connection states

| State | How the phone gets there | USB ID | What mtkclient does |
|---|---|---|---|
| **BROM** | Phone **off**, keys held, cable inserted (BROM runs before any flash content) | `0E8D:0003` "MediaTek USB Port" | Reads `hwcode` + target config, runs the exploit (kamakiri), uploads DA1/DA2, then all flash commands |
| **Preloader** | Phone **off**, cable inserted with **no keys**; the preloader's USB is up for ~1–2 s | `0E8D:2000/2001/201D/20FF/3000/6000` | Skips DRAM setup entirely (`connagent == b"preloader"` ⇒ `stage = 1`), uploads DA2 directly |
| **DA** | After the DA handshake, over the same pipe | still `0E8D:0003` (no re-enumeration); Windows may *also* show a "MediaTek DA USB VCOM" node | Partition I/O — this is where `r`/`ro` live |

IDs from [config/usb_ids.py](https://github.com/bkerler/mtkclient/blob/main/mtkclient/config/usb_ids.py);
BROM-vs-preloader branch from
[DA/xflash/xflash_lib.py L1111–1151](https://github.com/bkerler/mtkclient/blob/main/mtkclient/Library/DA/xflash/xflash_lib.py).

### 1.3 What a BROM session actually does (real Redmi 9A log)

From [issue #265](https://github.com/bkerler/mtkclient/issues/265) (identical to
[#229](https://github.com/bkerler/mtkclient/issues/229)), abbreviated:

```
Preloader - CPU: MT6765/MT8768t(Helio P35/G35)      HW code: 0x766      Target config: 0xe7
Preloader -   SBC enabled: True   SLA enabled: True   DAA enabled: True
Preloader -   Mem read auth: True  Mem write auth: True  Cmd 0xC8 blocked: True
Preloader - BROM mode detected.   ME_ID: …   SOC_ID: …
Preloader - [LIB]: Auth file is required. Use --auth option.
DaHandler - Device is protected.  DaHandler - Device is in BROM-Mode. Bypassing security.
PLTools - Loading payload from mt6765_payload.bin, 0x264 bytes
Exploitation - Kamakiri Run → Done sending payload...
DAXFlash - Uploading xflash stage 1 from MTK_DA_V5.bin
XFlashExt - Patching da1 ... / Security check patched / DA version anti-rollback patched /
            SBC patched to be disabled / Register read/write not allowed patched
Preloader - Jumping to 0x200000: ok.
DAXFlash - Successfully received DA sync
DAXFlash - Sending emi data ...          <-- the landmine is here (see §2.5)
```

So: **SBC + SLA + DAA are all on** (fuses), and mtkclient still gets in — because kamakiri patches the BROM's
DA-verification path in SRAM, and then mtkclient patches DA1/DA2 in memory (`SBC patched to be disabled`,
`anti-rollback patched`). **No signing keys, no Xiaomi auth, no vendor DA are involved.**

### 1.4 Command map (only the read side matters here)

From [mtk.py CMDS_HELP](https://github.com/bkerler/mtkclient/blob/main/mtk.py) and README-USAGE:

| Command | Meaning | Read-only? |
|---|---|---|
| `printgpt` | Print GPT (also proves the DA is up) | ✅ |
| `gpt <dir>` | Save GPT tables to a directory | ✅ |
| `r <part> <file> [--offset N] [--length N]` | Dump partition (or a sub-range) — **the command we want** | ✅ |
| `r gpt <file>` | Dump the GPT itself (0x16000 bytes) | ✅ |
| `rl <dir>` | Dump **every** partition — tens of GB, hours. Do not use for this loop | ✅ but expensive |
| `rf <file> [--offset N] [--length N]` | Dump the whole user area | ✅ but expensive |
| `ro <offset> <length> <file>` | Dump from an **absolute** flash byte offset | ✅ |
| `rs <sector> <count> <file>` | Dump from an absolute LBA | ✅ |
| `stage` / `plstage` | Send `stage2.bin` into SRAM (BROM / preloader respectively) | ✅ |
| `payload`, `dumpbrom`, `dumppreloader`, `peek`, `brute`, `crash` | BROM research tools | ✅ (but `crash` deliberately kills the session) |
| `gettargetconfig` | Print SBC/SLA/DAA flags | ✅ |
| `da efuse` | Read efuses | ✅ |
| `reset` | Clean exit: reset the phone | ✅ |
| `w`, `wl`, `wf`, `wo`, `e`, `es`, `ess`, `da seccfg` | **Writes/erases** — out of scope, brick territory (§3.7) | ❌ |

`r --offset` is **partition-relative** (added to the partition's start sector), verified in
[mtk_da_handler.py L368](https://github.com/bkerler/mtkclient/blob/main/mtkclient/Library/DA/mtk_da_handler.py);
`ro`/`rs` are absolute.

### 1.5 Windows installation

From [README-WINDOWS.md](https://github.com/bkerler/mtkclient/blob/main/README-WINDOWS.md):

```powershell
winget install Python.Python.3.12      # >= 3.9 required; MS-Store Python works
winget install Git.Git
git clone https://github.com/bkerler/mtkclient D:\tools\mtkclient
cd D:\tools\mtkclient
py -3 -m pip install -r requirements.txt
```

`requirements.txt` = `pyusb`, `pycryptodome`, `pycryptodomex`, `pyserial`, `pyside6`/`shiboken6` (GUI), `keystone`,
`capstone`, `unicorn`, `mfusepy` (FUSE only, needs [WinFsp](https://winfsp.dev/rel/)).
Prefer **git clone over `pip install mtkclient`**: the CLI was refactored in 2025/26 (`mtk.py` header says
"V2.1.4"), and the flash subcommands below are what `main` implements.

### 1.6 Drivers on Windows — VCOM vs libusb, and the UsbDk question

This is the part that eats evenings, so be precise about what the code actually does today:

* mtkclient talks **libusb** (`pyusb`), not the COM port. On Windows it loads the **bundled**
  `mtkclient\Windows\libusb-1.0.dll` ([usblib.py L136–143](https://github.com/bkerler/mtkclient/blob/main/mtkclient/Library/Connection/usblib.py)).
* A device is only openable if it is bound to **WinUSB/libusb0**, *or* if **UsbDk** is installed to detach it
  from its kernel driver. If Windows bound the stock **"MediaTek USB Port" / VCOM (usbser)** driver, libusb gets
  `Access denied` (errno 13); the code then retries with the libusb0 backend, and with libusb1 + `USE_USBDK`
  ([usblib.py L311–328](https://github.com/bkerler/mtkclient/blob/main/mtkclient/Library/Connection/usblib.py)).
* README-WINDOWS therefore says: install the **stock MTK port driver** and **[UsbDk](https://github.com/daynix/UsbDk/releases/)**,
  then check with `UsbDkController -n` that a `0x0E8D 0x0003` device is visible.
* **But** the current code *disables* the UsbDk option by default with the comment
  *"UsbDk enumeration is broken on this Windows build (26200); plain WinUSB/libusb0 enumeration works"*
  ([usblib.py L147–149](https://github.com/bkerler/mtkclient/blob/main/mtkclient/Library/Connection/usblib.py)).
  So on a recent Windows 11, the UsbDk path may simply not engage.

**Practical recipe (in this order):**

1. Plug the phone in BROM mode; in Device Manager note whether it appears as *MediaTek USB Port (COMx)*.
2. Try `python mtk.py gettargetconfig`. If you see `Access denied` / "Couldn't detect the device", then either
3. **(a)** install [UsbDk](https://github.com/daynix/UsbDk/releases/) (README's path; reboot; `UsbDkController -n`), or
4. **(b)** bind **WinUSB** to `0E8D:0003` with [Zadig](https://zadig.akeo.ie/) (Options → List All Devices →
   `MediaTek USB Port` → WinUSB). This is the most reliable on Win11 25H2. **Cost:** SP Flash Tool will no longer
   see it as a VCOM port; revert with Device Manager → Uninstall device → "Delete driver" if you need VCOM back.

Do not install both a WinUSB binding *and* expect the VCOM driver to work; they are mutually exclusive per device node.

### 1.7 How the phone must be placed

* **Power off first.** BROM is entered from a cold, powered-off state. If the shim hung the CPU, hold **Power
  ~10–20 s** until the panel goes dark (PMIC-level long-press), then release.
* **BROM mode:** per [README.md](https://github.com/bkerler/mtkclient/blob/main/README.md), *"power off device,
  press and hold either vol up + power or vol down + power and connect the phone; once detected, release."*
  On Redmi 9A/9C-class Xiaomi MT6765 phones the combo that most often works is **Vol Up + Vol Down held together
  while inserting the cable** (no Power key). Treat the three variants as a checklist and record which one worked
  for this unit; verify by watching for `Port - Device detected :)` / `BROM mode detected.`
* **Preloader mode:** cable in, **no keys**, and **start the mtkclient command first** — the window is ~1–2 s
  ("Status: Waiting for PreLoader VCOM, please reconnect mobile/iot device").
* **Test point** (last resort, *not* needed for this project): [issue #309](https://github.com/bkerler/mtkclient/issues/309)
  documents a unit that only entered BROM via test point after a botched preloader write.

---

## 2. Device support for MT6762 / MT6765

### 2.1 The chip table

Everything is table-driven in
[config/brom_config.py](https://github.com/bkerler/mtkclient/blob/main/mtkclient/config/brom_config.py)
(README: *"Chip details / configs → config/brom_config.py"*). Two entries matter:

| key (BROM hwcode) | name | dacode | DA mode | var1 | loader (payload) |
|---|---|---|---|---|---|
| `0x766` | **MT6765/MT8768t** (Helio P35/G35) | `0x6765` | `XFLASH` (5) | `0x25` *"confirmed"* | `mt6765_payload.bin` |
| `0x717` | MT6761/**MT6762**/MT3369/MT8766B/MT8761/AC8259/AC8257 | `0x6761` | `XFLASH` (5) | `0x25` | `mt6761_payload.bin` |

Both carry the same per-chip constants used by the exploit: `watchdog=0x10007000`, `brom_payload_addr=0x100A00`,
`da_payload_addr=0x201000`, `efuse_addr=0x11C50000`, `blacklist`, `cmd_handler`, `send_ptr`.
**MT6762G is covered twice over**: either as the `0x717` (MT6761/6762) row or as `0x766` — and the phone's own BROM
resolves the ambiguity (next section).

### 2.2 What this specific phone reports

Real dandelion logs ([#265](https://github.com/bkerler/mtkclient/issues/265),
[#229](https://github.com/bkerler/mtkclient/issues/229),
[fix repo](https://github.com/Hypercomputation/redmi-9a-dandelion-mtkclient-fix)) all show:

```
CPU: MT6765/MT8768t(Helio P35/G35)   HW code: 0x766   HW subcode: 0x8a00   HW Ver: 0xca00   SW Ver: 0x0
```

⇒ on a Redmi 9A the **`0x766` row is the one used** (`dacode 0x6765`, `mt6765_payload.bin`, `var1=0x25`). The
marketing name "MT6762G" is a binning of the same `mt6765` platform — consistent with the LK image in this repo
([pl.bin](<D:/aios/research/pl.bin>)), whose embedded strings contain `platform/mt6765/*.c` next to `MT6762G`,
`MT6765V/CA`, `MT6762V/WB` and `MT6762` in one platform table.

### 2.3 Payloads, DAs, and what is *not* needed

| Asset | Size | Role |
|---|---|---|
| `mtkclient/payloads/mt6765_payload.bin` | **612 B** | kamakiri BROM stub for this chip; run before DA upload |
| `mtkclient/payloads/stage2.bin` | **15,896 B** | standalone SRAM agent with its **own polled eMMC driver** (`msdc0`), no DRAM |
| `mtkclient/Loader/MTK_DA_V5.bin` | vendor | XFlash DA (DA1 in SRAM @0x200000 + DA2); patched in RAM at runtime |
| `mtkclient/Loader/MTK_DA_V6.bin` | vendor | **not** used here — the "V6" protocol is MT6781/6789/6855/6886/6895/6983/8985 only ([README](https://github.com/bkerler/mtkclient/blob/main/README.md)) |
| `mtkclient/Loader/Preloader/*` | 1.4–1.9 KB each | **EMI blocks only**, not full preloaders (see §2.4) |
| `Loader/MTK_AllInOne_DA_*.bin` | vendor | multi-chip bundles; `DAconfig.setup()` picks by `dacode` + `hwver`/`swver` |

**Which "campaign"?** kamakiri — a USB control-transfer attack on the BROM's `usb_ctrl_handler`
(`udev.ctrl_transfer(0xA1, 0, 0, var1, 0)` with `var1 = 0x25` for 0x766), see
[kamakiri.py L21–48](https://github.com/bkerler/mtkclient/blob/main/mtkclient/Library/Exploit/kamakiri.py).
`--ptype` accepts `amonet | kamakiri | kamakiri2 | carbonara` (per `mtk.py`; `hashimoto` and `heapbait` also exist in
the tree for other chips); **for 0x766 leave it alone**, the default (kamakiri) is the supported one and `var1` is
already confirmed in the table. `brute` exists for unknown chips; you do not need it.

**Does it need my DA?** No. **Does it need my preloader?** For BROM mode: *effectively yes* (§2.4/§2.5); for
preloader mode: no.

### 2.4 The preloader/EMI story (this is the crux for `dandelion`)

In BROM mode the DA1 has to bring DRAM up itself, so it must be handed a **DRAM config (EMI)** table. mtkclient's
options, in code order ([xflash_lib.py L1111–1149](https://github.com/bkerler/mtkclient/blob/main/mtkclient/Library/DA/xflash/xflash_lib.py)):

1. If a preloader **file** was given (`--preloader`), its EMI is extracted (`MTK_BLOADER_INFO_vNN`, 112-byte
   header + 10 × 160-byte entries = **1712 bytes**) and sent.
2. Else it walks `Loader/Preloader/*` and only accepts a bundled file **if the phone's eMMC CID prefix
   (`emmc_info.cid[:8]`) appears inside that file** — the blocks are CID-bound. If one matches and the DA accepts
   it, you see `Detected working preloader: <file>`.
3. Else: `No preloader given. Operation may fail due to missing dram setup.`

So the **bundled `preloader_redmi9a_Hydra_V2.bin` only auto-matches if its embedded CID prefix equals this
phone's eMMC CID prefix** (`DRAM config needed for: <hex>` in the log). Copies of these files are EMI-only 1712-byte
blobs, so `--preloader` accepts them directly as well as a full 243,240-byte `preloader_k62v1_64_bsp.bin`.

### 2.5 The model-specific landmine: DA1 rejects the 2 GB Micron entry

[issue #405](https://github.com/bkerler/mtkclient/issues/405) (Oct 2026) reverse-engineered it; the long-form
write-up is [Hypercomputation/redmi-9a-dandelion-mtkclient-fix](https://github.com/Hypercomputation/redmi-9a-dandelion-mtkclient-fix):

* `MTK_DA_V5.bin`'s DRAM-entry matcher (`0x2145dc…0x214658`) does `cmp r3, r2` with a **32-bit** EMI word against
  the **8-bit** MR5 vendor id read from the chip. Xiaomi's stock table stores multi-byte values there
  (`0xFF1301`, `0xFF01`), so the 2 GB / 1-rank entry is rejected, DA1 prints `[EMI] setting failed 0xffffffff`
  and **halts**; the host sees `DRAM setup failed: unpack requires a buffer of 12 bytes`.
* **Fix:** in the EMI block, entry 3, field `+0x98`: `01 ff 00 00` → `ff 00 00 00` (one 4-byte word). A patched
  EMI block is validated and MIT-licensed; I re-verified its table (see Appendix A): its only 2 GB / 1-rank entry
  now carries `MR5word = 0x000000FF`, which passes the 32-bit compare.
* [issue #229](https://github.com/bkerler/mtkclient/issues/229) shows the tool's own suggestion in the error text:
  *"Use mtk.py with --preloader preloader.bin !"* — **but per #405 the stock preloader does not help by itself**;
  the table itself must be patched (or one of the alternate paths below used).

**Three independent escapes, all verified by third parties on dandelion:**

| # | Path | Why it works | Evidence |
|---|---|---|---|
| A | BROM + `--preloader <patched EMI or patched preloader>` | DA1 accepts the 2 GB entry | #405 + fix repo (DA2 boots, `printgpt`, `r`, `da seccfg unlock` all work) |
| B | BROM + `plstage --preloader preloader_k62v1_64_bsp.bin` | a real preloader does EMI itself | [#309](https://github.com/bkerler/mtkclient/issues/309): *"PLStage loads preloader to RAM … After this, DA commands work perfectly"* |
| C | BROM + `stage` → `stage2.py` | no DA, no DRAM, own eMMC driver | fix repo §2: dumped GPT and `boot1` this way on dandelion |
| D | **preloader mode** (no keys) | `connagent == b"preloader"` ⇒ DA2 upload happens with **no EMI step at all** | code path §1.2/§2.4; `plstage` in preloader mode is #309's working case |

---

## 3. Concrete Windows command lines

All examples assume `cd D:\tools\mtkclient` and `py -3` (or an activated venv). Use a **real** preloader path;
`<PL>` below = `D:\aios\research\emi_fixed_v35.bin` (see Appendix A) or your own
`preloader_k62v1_64_bsp.bin`.

> **Argument-order trap.** `--preloader` must come **after** the subcommand. Placed before it, the subparser
> re-initialises the option to `None` and the file is silently ignored (documented by the fix-repo author, and it
> matches `add_common_flash_cmd(..., parents=[base])` in `mtk.py`). ✅ `mtk.py printgpt --preloader X` ❌ `mtk.py --preloader X printgpt`.

### 3.1 Driver-level preflight (no mtkclient involved)

```powershell
# is a MediaTek device present at all?
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'VID_0E8D' } |
    Select-Object Status,Class,FriendlyName,InstanceId | Format-Table -Auto

# if UsbDk is installed (README path), it should list 0E8D:0003
& 'C:\Program Files\UsbDk Runtime Library\UsbDkController.exe' -n

# COM-port view: BROM/preloader ports often show up here first
Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match 'MediaTek|MTK|PreLoader' } |
    Select-Object Name,DeviceID | Format-Table -Auto
```

### 3.2 (a) Verify detection

```powershell
# cheapest: does the BROM answer, and with which security flags?
py -3 mtk.py gettargetconfig

# stronger: full handshake + DA + GPT (this is the go/no-go test for the whole loop)
py -3 mtk.py printgpt --preloader "<PL>"
```

Expected good output: `BROM mode detected.`, `Kamakiri Run`, `DRAM setup passed.`, `Sending emi data succeeded.`,
`Uploading stage 2...`, `Successfully uploaded stage 2`, `DA Extensions successfully added`, then a `GPT Table:`
listing. I look for `flashinfo` in that table (dandelion: `mmcblk0p43`, 16 MiB).

Failure signatures worth memorising:

| Log | Meaning | Next move |
|---|---|---|
| `Couldn't detect the device` | not in BROM / driver not claimable | §3.1, §1.6 |
| `DRAM setup failed: unpack requires a buffer of 12 bytes` | the EMI landmine (§2.5) | use patched `--preloader`, or path B/C/D |
| `Preloader - [LIB]: Auth file is required. Use --auth option.` followed by `Bypassing security` | normal on this phone | ignore |
| `Detected working preloader: …` | bundled EMI matched by CID | good, continue |
| `Couldn't detect partition: flashinfo` | GPT not parsed / wrong mode | re-check `printgpt` |

### 3.3 (b) Dump a named partition

```powershell
# full 16 MiB partition
py -3 mtk.py r flashinfo D:\aios\artifacts\flashinfo.bin --preloader "<PL>"

# multiple partitions in one session (one DA upload, several files)
py -3 mtk.py r flashinfo,gpt D:\aios\artifacts\flashinfo.bin,D:\aios\artifacts\gpt.bin --preloader "<PL>"

# save the GPT separately, print it, and keep a copy of the partition table
py -3 mtk.py gpt D:\aios\artifacts --preloader "<PL>"

# clean exit (resets the phone)
py -3 mtk.py reset --preloader "<PL>"
```

`r` writes exactly `sectors × 512` bytes; `--length` overrides the size.

### 3.4 (c) Dump a byte range

```powershell
# partition-relative: header (4 KiB) + 1 MiB ring, i.e. flashinfo + 0x100000 .. + 0x201000
py -3 mtk.py r flashinfo D:\aios\artifacts\flashinfo_log.bin `
        --offset 0x100000 --length 0x110000 --preloader "<PL>"

# just the 4 KiB header
py -3 mtk.py r flashinfo D:\aios\artifacts\flashinfo_hdr.bin --offset 0x100000 --length 0x1000 --preloader "<PL>"

# absolute flash offset (from the GPT: first_lba * 512). Example only — read the real value first.
py -3 mtk.py ro 0x1A000000 0x110000 D:\aios\artifacts\flashinfo_log2.bin --preloader "<PL>"

# absolute LBA range
py -3 mtk.py rs 0xD0000 0x880 D:\aios\artifacts\flashinfo_lba.bin --preloader "<PL>"
```

`--offset`/`--length` are parsed base-16, so `0x…` or bare hex both work (`0x110000` = 1 MiB + 64 KiB).
Getting `first_lba` is free: `py -3 mtk.py r gpt gpt.bin` then parse LBA 2, or read the `printgpt` output.

**Cost matters:** 1.1 MiB ≈ one packet burst; 16 MiB ≈ a few seconds. Prefer the 1.1 MiB range for the loop and
keep a full 16 MiB dump for the archive.

### 3.5 Fallback with no DA and no DRAM (`stage2`)

Use this if the EMI path stays broken. It is slower (512-byte USB transfers) but it does not touch DA1/DRAM at all.

```powershell
# 1) BROM: put stage2.bin into SRAM and jump to it
py -3 mtk.py stage

# 2) read raw user-area bytes by absolute byte offset (hex)
py -3 stage2.py data --start 0x1A100000 --length 0x110000 --filename D:\aios\artifacts\flashinfo_log.bin

# also available: preloader, boot2, rpmb, memread, memwrite (DO NOT use memwrite here), reboot
py -3 stage2.py reboot
```

Notes: `stage2.py data` takes **byte** offsets (`start // 0x200` internally) and reads 512 bytes per transaction;
budget ~1–3 min for 16 MiB, ~5–10 s for 1.1 MiB. `mtk.py stage --stage2 <file> --stage2addr <addr>` can override
the payload if you ever need to.

### 3.6 Preloader-mode variant (try it first — it is free and sidesteps §2.5)

```powershell
# start THIS first, then plug the powered-off phone with no keys held
py -3 mtk.py printgpt
# good:  "Preloader - Detected … / BROM mode detected." then GPT without any "Sending emi data"
```

In preloader mode mtkclient skips the whole EMI/DRAM stage and goes from DA1 straight to stage 2
(`connagent == b"preloader"` ⇒ `stage = 1`), so there is no BROM exploit and no EMI to get wrong. If the 1–2 s
window is catchable on your unit, this is the **least risky** path.

### 3.7 Risks

| Risk | Trigger | Severity | Mitigation |
|---|---|---|---|
| **Brick by writing** (`w`, `wl`, `wf`, `wo`, `e`, `da seccfg`) | any write, especially `preloader`/`lk`/`seccfg` | **fatal** ([#309](https://github.com/bkerler/mtkclient/issues/309): dead phone after writing preloader to `boot1` only) | This project never writes. Read-only commands only. |
| **Anti-rollback** | flashing an older preloader/lk; ARB fuse on this unit is `anti: 1` (#309) | fatal/hard-brick | never write boot-chain partitions |
| **BROM hang after a failed EMI attempt** | `[EMI] setting failed 0xffffffff`, DA1 HALT | high: needs power cycle; #405 reports *"freeze the phone in BROM mode until the battery is physically disconnected"* (non-removable battery → back cover) | use a pre-validated EMI; prefer path B/C/D on the first run; never iterate EMI variants blind |
| **Wrong exploit `var1`** | forcing `--var1` / `brute` | medium | don't: `0x25` is confirmed for 0x766 |
| **Driver: `Access denied`** | VCOM driver owns the node, no UsbDk | low | §1.6; WinUSB via Zadig is the reliable Win11 25H2 route |
| **UsbDk enumeration broken** | Windows build 26200 | low | code already disables it; use WinUSB/libusb0 |
| **Two tools at once** (SP Flash Tool + mtkclient) | both claim the device | low | one at a time |
| **`rl` / `rf` on 64 GB** | fat fingers | low | only `r <part>` |
| **`stage2.py memwrite`** | exploring | fatal | don't; `data`/`preloader`/`reboot` only |
| **efuse reads** | `da efuse` | none (read) | safe; do it once and keep the output |

---

## 4. BROM readback vs. "boot Android and `dd`"

| | **BROM + mtkclient `r`** | **Android + `adb … dd`** (current fallback) | **fastboot only** |
|---|---|---|---|
| Works when the test image kills USB? | **Yes** — BROM is below the kernel | **No** — needs a healthy kernel + adbd | No read primitive |
| Needs Android installed/bootable? | No | Yes (Magisk root) | — |
| Needs a working `boot` image? | No | Yes — must reflash **stock boot** every iteration | Bootloader only |
| Per-iteration time | **≈45–75 s** (≈15–30 s of that is physical) | ≈3–5 min: 2× `fastboot flash boot`, Android boot ~30–40 s, `dd`+pull ~2 s | n/a |
| Flash writes per iteration | 1 (`fastboot flash boot` of the test image) | **2** (test image + stock boot) | 1 |
| Physical actions | force-off, BROM key combo + plug (and fastboot combo if flashing that way) | force-off, fastboot combo, power cycle, Android unlock/PIN sometimes, then fastboot again | force-off + fastboot combo |
| Data safety | read-only, no flash writes | read-only, no flash writes | n/a |
| Failure modes | driver/EMI/BROM-hang; needs the preloader fix once | adb auth, Magisk state, `su` path, MTP flakiness, USB re-enum on the hung image | — |
| Setup cost | ~1 evening once (drivers + EMI validation) | zero (already used) | — |
| Log freshness | always the last boot's log | same | — |

**Verdict.** BROM is **faster per iteration** (≈3–4×), **works in exactly the state that blocks you today**
(shim with no USB stack), and does **not** require reflashing Android's boot image. It is also *safer for the log
data*: no writes at all. The adb path remains useful as a zero-setup cross-check when the shim is healthy — keep
both; make BROM the primary.

Note that the whole loop can even be done **without fastboot**: `py -3 mtk.py w boot boot-test.img --preloader "<PL>"`
from the same BROM session (`w` writes a single partition only). Slower than fastboot (~28 MB over DA) and it is a
write, so it is optional — but it means the loop needs **no fastboot and no working LK UI** at all.

---

## 5. Recommended "one log cycle" for this project

Legend: 🖐 = physical action on the phone. Everything else is one host command.

### 5.1 One-time setup (do this once, offline of any boot experiment)

1. Install Python ≥3.9, git, `git clone` mtkclient, `pip install -r requirements.txt` (§1.5).
2. Driver: plug the phone in BROM mode, run `py -3 mtk.py gettargetconfig`; if it fails, install UsbDk **or** bind
   WinUSB (§1.6). ✅ Done when `gettargetconfig` prints `HW code: 0x766` and `SBC/SLA/DAA enabled`.
3. Obtain the preloader **for this exact phone**: `preloader_k62v1_64_bsp.bin` (243,240 B) from the matching MIUI
   fastboot ROM, *or* dump it from the phone's `boot1` with `stage2.py preloader --start 0x800 --length 0x3B628`
   (check for the `MTK_BLOADER_INFO` magic) — §3.5 / fix repo §2.
4. Make a working EMI: either apply the 1-word patch (entry 3, `+0x98`: `01 ff 00 00` → `ff 00 00 00`), or use the
   validated block kept at `D:\aios\research\emi_fixed_v35.bin` (Appendix A).
5. Validate: `py -3 mtk.py printgpt --preloader "<PL>"` must reach `Successfully uploaded stage 2` and print the GPT
   with `flashinfo`. **Do not start the log loop before this passes once.**
6. Record, for this unit: the working BROM key combo, and `flashinfo`'s `first_lba`/size from `printgpt`.
   Optional: `py -3 mtk.py da efuse` once, save the output.

### 5.2 Steady-state cycle

| # | Step | Command / action | Notes |
|---|---|---|---|
| 1 | 🖐 Enter fastboot | Phone off → hold **Vol Down + Power** → `FASTBOOT` | skip if you flash over BROM (step 1′) |
| 2 | Flash the test image | `fastboot flash boot D:\aios\artifacts\boot-test.img` | this is the only write in the cycle |
| 3 | Boot it | `fastboot reboot` | shim runs, writes the log to `flashinfo`, then dies/hangs |
| 4 | Wait for the hang | ~5–20 s, screen frozen/blank, no adb | trust the heartbeat convention if the panel still shows text |
| 5 | 🖐 Force power off | hold **Power 10–20 s** until dark | needed: BROM is only entered from a cold state |
| 6 | 🖐 Enter BROM | hold **Vol Up + Vol Down**, insert USB, release on `Port - Device detected :)` | the one step to tune per unit (§1.7) |
| 7 | Read the log | `py -3 mtk.py r flashinfo D:\aios\artifacts\flashinfo_log.bin --offset 0x100000 --length 0x110000 --preloader "<PL>"` | 1.1 MiB ≈ 1–2 s; full 16 MiB if you want the archive (`--length` omitted) |
| 8 | Exit | `py -3 mtk.py reset --preloader "<PL>"`, unplug | phone reboots; it will hang again in the test image — that's fine |
| 9 | Decode | `py -3 D:\aios\research\msdc_log_decode.py D:\aios\artifacts\flashinfo_log.bin --scan` | `--scan` finds `MSDCLOG1` anywhere, so a sub-range dump is fine; `--json out.jsonl` for machine-readable |
| 10 | Loop | back to step 1 | |

**Timing budget:** steps 2–3 ≈ 5–10 s; step 5 ≈ 10–20 s; step 6 ≈ 5–10 s; step 7 ≈ 10–20 s (handshake dominates);
step 9 < 1 s. **≈45–75 s wall clock, 4 host commands, 3 button interactions** (2 if you flash over BROM instead of
fastboot — see below).

**If step 7 fails with `DRAM setup failed`:** swap to §3.5 (`mtk.py stage` + `stage2.py data`) for that cycle;
nothing about the log or the flash changes, only the read transport.

**If you want zero fastboot:** replace steps 1–3 with, from the same BROM session,
`py -3 mtk.py w boot D:\aios\artifacts\boot-test.img --preloader "<PL>"` then `py -3 mtk.py reset --preloader "<PL>"`.
Then the cycle is: 🖐 BROM (plug), `w boot`, `reset`, wait for the hang, 🖐 power-off, 🖐 BROM again, `r flashinfo` —
**3 physical actions per iteration, exactly as with fastboot**, but with no fastboot dependency at all.

---

## 6. Showstoppers, unknowns, and what to verify first

**Not showstoppers (checked):**

* *"BROM disabled on this SoC revision"* — **no**. `dandelion` BROM answers with `HW code 0x766`, and kamakiri
  completes (`Done sending payload...`) on real units (#265, #229, fix repo).
* *"Secure boot fuses block reading"* — **no**. SBC/SLA/DAA are all enabled (`target config 0xe7`) and mtkclient
  patches DA1/DA2 in RAM (`SBC patched to be disabled`). Efuses are *readable*; nothing needs burning.
* *"Need the vendor DA / Xiaomi auth"* — **no**; bundled `MTK_DA_V5.bin` + 612-byte kamakiri stub.
* *"Need to photograph the screen"* — **no longer**; this path replaces that.

**Real risks / unknowns, in priority order:**

1. **The EMI/DRAM bug on 2 GB Micron units (§2.5).** Reproducible, root-caused, and fixable in one 4-byte field;
   worst case a failed attempt leaves BROM halted until the battery is disconnected. **Verify `printgpt` once
   before trusting the loop**, and prefer the pre-validated EMI block.
2. **BROM hang recovery on a sealed phone.** Redmi 9A's battery is not user-removable. Test the long-press
   power-off *before* you need it, and keep the "battery disconnect" option (back cover off) as the documented
   last resort (#405).
3. **eMMC-CID binding of the bundled EMI blocks.** The auto-search only accepts a bundled block whose embedded CID
   prefix matches your eMMC (§2.4). Don't rely on `Detected working preloader:` appearing; supply `--preloader`.
4. **Preloader-mode timing.** The window is ~1–2 s and needs the command already running. If your unit never
   exposes it, fall back to BROM + patched EMI. (Unverified for this unit.)
5. **Windows driver setup** is the most likely *time* sink: UsbDk may be inert on Win11 26200; WinUSB-via-Zadig
   works but breaks SP Flash Tool VCOM (§1.6).
6. **`flashinfo` head is not ours.** First 64 KiB may hold LK/preloader download metadata (the LK sources in
   `research/refs/lk-svoboda18/platform/mt6765/mt_get_dl_info.c` read a `DL_INFO` block out of `flashinfo`).
   Read-only dumps are unaffected; just don't interpret the first 64 KiB as log data, and don't "restore" over it.
   (Existing layout already starts the log at `+0x100000` — keep it.)
7. **GPT must be valid** for `r <name>` (it resolves the name to an LBA). It is; `printgpt` proves it. If the GPT
   were ever damaged, `ro`/`rs` + a known LBA still work, and `stage2.py data` works without any DA.
8. **Anti-rollback** (`anti: 1` on this unit) only matters if you *write* preloader/lk. Read path untouched.
9. **Speed of `stage2`** (512 B/transfer): fine for 1.1 MiB, ~1–3 min for 16 MiB. Plan around it if you must use it.
10. **`--preloader` placement** (§3, top) — silent failure mode, easy to lose an hour to.

---

## 7. Citations

mtkclient (primary sources):

* Repository: <https://github.com/bkerler/mtkclient>
* README (BROM entry procedure, V6 chip list): <https://github.com/bkerler/mtkclient/blob/main/README.md>
* Windows install + UsbDk: <https://github.com/bkerler/mtkclient/blob/main/README-WINDOWS.md>
* Usage / command reference: <https://github.com/bkerler/mtkclient/blob/main/README-USAGE.md>
* CLI definition (`r`, `rl`, `rf`, `ro`, `rs`, `stage`, `plstage`, `--preloader`): <https://github.com/bkerler/mtkclient/blob/main/mtk.py>
* Chip table (`0x766` MT6765, `0x717` MT6761/6762, `var1=0x25`): <https://github.com/bkerler/mtkclient/blob/main/mtkclient/config/brom_config.py>
* USB IDs (BROM `0x0E8D:0x0003`, preloader PIDs): <https://github.com/bkerler/mtkclient/blob/main/mtkclient/config/usb_ids.py>
* kamakiri exploit (`ctrl_transfer(0xA1,0,0,var1,0)`): <https://github.com/bkerler/mtkclient/blob/main/mtkclient/Library/Exploit/kamakiri.py>
* DA/EMI flow (`send_emi`, preloader-mode branch, CID-matched auto-search): <https://github.com/bkerler/mtkclient/blob/main/mtkclient/Library/DA/xflash/xflash_lib.py>
* Partition read (`--offset` is partition-relative): <https://github.com/bkerler/mtkclient/blob/main/mtkclient/Library/DA/mtk_da_handler.py>
* Windows USB backend / bundled libusb / UsbDk note: <https://github.com/bkerler/mtkclient/blob/main/mtkclient/Library/Connection/usblib.py>
* `stage2` SRAM agent + `data` command: <https://github.com/bkerler/mtkclient/blob/main/stage2.py>

Device-specific evidence (Redmi 9A / dandelion):

* #265 — BROM log, `HW code 0x766`, `DRAM setup failed`: <https://github.com/bkerler/mtkclient/issues/265>
* #229 — same failure + `Use mtk.py with --preloader preloader.bin !`: <https://github.com/bkerler/mtkclient/issues/229>
* #405 — root cause in DA1 (MR5 32-bit compare), BROM hang: <https://github.com/bkerler/mtkclient/issues/405>
* #309 — `PLStage` works, brick after preloader write, test point, `anti: 1`: <https://github.com/bkerler/mtkclient/issues/309>
* Fix repo (MIT): patched EMI, `preloader_fixed.bin`, `emi_fixed_v35.bin`, 1-word patch script, `stage2` dump of GPT/boot1:
  <https://github.com/Hypercomputation/redmi-9a-dandelion-mtkclient-fix>

Supporting tools/URLs:

* UsbDk: <https://github.com/daynix/UsbDk/releases/>
* Zadig (WinUSB binding): <https://zadig.akeo.ie/>
* WinFsp (only for `mtk fs`): <https://winfsp.dev/rel/>

In-repo context used for the log contract:

* [mtk-msdc-pio.md](<D:/aios/research/mtk-msdc-pio.md>) — `flashinfo` layout (`+0x100000` header, 1 MiB ring), adb read path
* [msdc_log_decode.py](<D:/aios/research/msdc_log_decode.py>) — decoder (`--scan`, `--json`)
* [HARDWARE_ANALYSIS.md](<D:/aios/dandelion-xnu/docs/HARDWARE_ANALYSIS.md>) — `flashinfo` = `mmcblk0p43`, 16 MiB
* [unixnu-usb-logs.md](<D:/aios/research/unixnu-usb-logs.md>) — prior ranking of read-back options (this study answers its question 5)

---

## Appendix A — measured assets (what I actually inspected)

Downloaded and byte-inspected while writing this (kept: the 1712-byte EMI block; everything else was scratch):

| File | Size | SHA-256 | Note |
|---|---|---|---|
| `research/emi_fixed_v35.bin` | 1,712 B | `662F738335982D3DA7521268CEE34F22D0389AD54C01C42503512FFD32A10DAB` | validated 2 GB Micron EMI, MIT, from the fix repo; starts with `MTK_BLOADER_INFO_v35`, 10 entries |
| `mtkclient/.../preloader_redmi9a_Hydra_V2.bin` | 1,712 B | (upstream) | bundled; also EMI-only; **different table** (120 of 1712 bytes differ), but it *does* contain a 2 GB / 1-rank / `MR5word=0x000000FF` entry at index 2 — so it may work, unverified |
| `mtkclient/.../preloader_k62v1_64_bsp*.bin` | 1,456 / 1,872 B | (upstream) | EMI-only stubs; one is LPDDR3-bound (`type=0x0003`) per the fix repo → wrong for this board |
| `mtkclient/payloads/mt6765_payload.bin` | 612 B | (upstream) | kamakiri stub |
| `mtkclient/payloads/stage2.bin` | 15,896 B | (upstream) | SRAM agent with own eMMC driver |

EMI entry dump of the validated block (offset `+0x00` type, `+0x50` rank0, `+0x58` rank1, `+0x98` MR5 word):

```
entry 0: type=0x0000 rank0=0x20000000 rank1=0x00000000 MR5word=0x00000000
entry 1: type=0x0001 rank0=0x80000000 rank1=0x80000000 MR5word=0x00FF1301   <- rejected by 32-bit compare
entry 2: type=0x0001 rank0=0x60000000 rank1=0x60000000 MR5word=0x00000001
entry 3: type=0x0001 rank0=0x80000000 rank1=0x00000000 MR5word=0x000000FF   <- 2 GB / 1 rank, patched, accepted
entry 4: type=0x0001 rank0=0x80000000 rank1=0x00000000 MR5word=0x00000013
entry 5: type=0x0001 rank0=0x100000000 rank1=0x00000000 MR5word=0x000000FF
entry 6: type=0x0001 rank0=0xC0000000 rank1=0xC0000000 MR5word=0x000000FF
entry 7: type=0x0001 rank0=0x80000000 rank1=0x100000000 MR5word=0x000000FF
entry 8: type=0x0001 rank0=0x60000000 rank1=0x60000000 MR5word=0x00000006
entry 9: type=0x0001 rank0=0xC0000000 rank1=0x00000000 MR5word=0x00000006
```

Use it only while the phone really is 2 GB / 1-rank / Micron (`MR5=0xFF`). If the module is Samsung/SK Hynix or the
capacity differs, patch *your* preloader's matching entry instead — do not mix tables.

`emi_fixed_v35.bin` is redistributed here under the MIT licence of
<https://github.com/Hypercomputation/redmi-9a-dandelion-mtkclient-fix> (Copyright (c) 2026 Hypercomputation).

## Appendix B — decision tree for one read

```
                         ┌──────────────────────────────┐
                 plug ──▶│ try PRELOADER mode (no keys) │
                         └──────────────┬───────────────┘
                    GPT printed ◀───────┴───────▶ no device in 1–2 s
                         │                              │
                         ▼                              ▼
                 r flashinfo (no EMI)        BROM mode (keys + plug)
                                                        │
                                          printgpt --preloader emi_fixed_v35.bin
                                                   │              │
                                     "stage 2 uploaded"     "DRAM setup failed"
                                                   │              │
                                                   ▼              ▼
                                          r flashinfo    plstage --preloader <pl>
                                                   │              │ fails
                                                   │              ▼
                                                   │        stage + stage2.py data
                                                   ▼              ▼
                                              decode ◀────────────┘
```
