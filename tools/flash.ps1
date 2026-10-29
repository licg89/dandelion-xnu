# Flash the current dandelion UEFI image, and say what to look for afterwards.
#
#   powershell -File tools\flash.ps1            # flash artifacts/boot-uefi-mem.img
#   powershell -File tools\flash.ps1 -Image <path>   # flash something else
#   powershell -File tools\flash.ps1 -Restore        # put the stock boot image back
#   powershell -File tools\flash.ps1 -Check          # report only, flash nothing
#
# Why a script: the sequence (wait for fastboot, flash boot, reboot, then read a
# panel photograph against a table of expected values) is done dozens of times per
# bring-up session, and every step of it has been got wrong at least once --
# flashing while the phone is still booting Android, forgetting that `fastboot
# boot` is unsupported on MTK LK, or reading the wrong version of the panel
# legend.  The legend is printed here so it cannot drift from the image.
#
# Nothing in this script is destructive beyond the boot partition, and -Restore
# exists for exactly that reason.

param(
  [string]$Image = "",
  [switch]$Restore,
  [switch]$Check,
  [string]$Sdk = "C:\Users\li\AppData\Local\Android\Sdk\platform-tools"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$fastboot = Join-Path $Sdk "fastboot.exe"
$adb = Join-Path $Sdk "adb.exe"

if (-not (Test-Path $fastboot)) {
  throw "fastboot not found at $fastboot (pass -Sdk <path>)"
}

if ($Restore) {
  $Image = Join-Path $repo "out\boot_backup.img"
  if (-not (Test-Path $Image)) {
    throw "stock boot image not found at $Image (out\ is gitignored; copy it from the old machine)"
  }
} elseif ([string]::IsNullOrEmpty($Image)) {
  $Image = Join-Path $repo "artifacts\boot-uefi-mem.img"
}

if (-not (Test-Path $Image)) {
  throw "image not found: $Image"
}

$size = (Get-Item $Image).Length
$hash = (Get-FileHash $Image -Algorithm SHA256).Hash.ToLower()
Write-Host ""
Write-Host "image : $Image"
Write-Host "size  : $size bytes"
Write-Host "sha256: $hash"
Write-Host ""

# MTK LK cannot `fastboot boot`, so the phone has to be in fastboot already.
$devices = @(& $fastboot devices 2>&1 | Where-Object { $_ -match "\s+fastboot$" })
if ($devices.Count -eq 0) {
  Write-Host "No device in fastboot." -ForegroundColor Yellow
  Write-Host "  Power off (hold power ~10 s), then hold Power + Volume Down to enter fastboot."
  if (Test-Path $adb) {
    $adbDevices = @(& $adb devices 2>&1 | Where-Object { $_ -match "\s+device$" })
    if ($adbDevices.Count -gt 0) {
      Write-Host "  Phone is booted in Android; 'adb reboot bootloader' also works."
    }
  }
  exit 1
}

Write-Host "device: $($devices[0])"
Write-Host ""
if ($Check) {
  Write-Host "check only: device is in fastboot, nothing was written." -ForegroundColor Green
  exit 0
}

Write-Host "flashing boot ..."
& $fastboot flash boot $Image
if ($LASTEXITCODE -ne 0) {
  throw "fastboot flash failed"
}

Write-Host "rebooting ..."
& $fastboot reboot | Out-Null

if ($Restore) {
  Write-Host "restored the stock boot image." -ForegroundColor Green
  exit 0
}

@"

Flashed. The panel comes up in a few seconds and then stays put (the watchdog is
disarmed, and the console wraps instead of scrolling), so there is no rush.

What to photograph: ONE shot of the whole lower half -- it must include the three
thin strip rows, the verdict patch and the three rows of cells.

  y 1172  cyan strip    P_before: MSDC_PS, the driver's very first read
  y 1186  white strip   PS_AND: bits that stayed high during the sampled CMD0
  y 1200  cyan strip    PS_OR:  bits that were high at some point during it
  x 400..560 y 1197     verdict patch
  y 1216  dots          blue = block devices (x 8), green = volumes (x 380)
  y 1400/1448/1496      stage cells.  16..23 are MsdcDxe's; this driver does
                        not paint them any more, so they stay the M4 evidence.
  text area             13 lines, fixed position (rows 53..65 of 68):
                        PWR PS=.. DAT=.. CMD=.. CD=.. VEMC=.. vsel=.. w=.. f=..
                        RST C=..                        (MSDC_CFG after soft reset)
                        H0 C=.. S=.. P=.. N=..          (controller leftovers)
                        C0 I=.. cl=.. f=.. l=.. kc=..   (CMD0: TX self-check)
                        C1 n=.. I=.. R=..               (CMD1 until OCR bit31)
                        C2 3=.. 2=.. 1=.. 0=..          (CID, R2)
                        C3 I=.. R=.. C7 I=.. R=..       (RCA, select)
                        C13 I=.. R=.. st=..             (4 = TRAN)
                        PV pre=.. a=.. b0=.. c=.. LIVE  (does RESP0 follow commands?)
                        IO dir=000 do=000 din=FFF       (pins 122..133, GPIO windows)
                        IO ioc=.. f60=.. adv=..         (controller leftovers)
                        A8 I=.. R=.. sz=.. ps=../.. gp=.../... f=.. k=..... c6=.... t=. r=.
                        B8/C8/D8 ...                    (only if the line above failed)
                        D8 is the same 1-bit attempt with CKPDN cleared
                        B1 I=.. S=..                    (LBA 1, GPT header)

Reading it, in this order:

  0. Any A8/B8/C8 line with `I=...1100` and a non-zero `sz`, plus
     `B1 S=45464920`, is the goal: commands, a 128-bit response, a 512-byte data
     read and sector addressing all work, and what is left is the UEFI driver.

  1. `C0` is the transmitter self-check: CMD0 has no response, so its f..l is
     the pure 48-bit frame.  About 100 samples low in one run is healthy.

  2. `C1 n=..` is how many CMD1 attempts it took for OCR bit 31 (power-up done)
     to come back set.  `CARD NOT READY` instead means the card stayed busy.

  3. `PV` decides whether any R1 in this log can be believed.  It reads RESP0,
     sends CMD13 (R1), CMD10 (R2, the CID), then CMD13 again:

        PV pre=.. a=00000900 b0=3BB919C9 c=00000900 LIVE

     `b0` must be the card's CID low word (3BB919C9) and differ from `a`; `c` must
     return to `a`.  `STALE` instead means RESP0 is not tracking commands, and
     then every "R1 = 0x00000900" in this log is a leftover rather than an answer
     -- stop and fix the register path before reading anything else.

  4. A8/B8/C8 are three bus modes -- 8-bit with the card as LK left it, 8-bit
     default speed, then 1-bit -- and the first that works stops the ladder.
     `I=...1100` is XFER_COMPL (good); `...0100` alone means the command answered
     and no data ever started; `...4100` adds DATTMO, `...8100` DATCRCERR.
     `sz=` is SEC_COUNT in 512-byte sectors.  `ps=AA/BB` is MSDC_PS sampled
     through the whole wait, and `gp=CCC/DD` is the GPIO block's own view of the
     same pins: ps quiet with gp moving means the controller is not seeing the
     card, both quiet means the card is not sending.  `f=` is what the FIFO held.
     `dir=` / `do=` / `ies=` on the IO lines say whether anything else is driving
     the bus or whether the pads' input enables are off.

  4. `B1 S=45464920` is "EFI " from the GPT header at LBA 1; anything else means
     the read worked but the addressing or the partition table did not.

  5. Any line whose `I` is 00000200 (CMDTMO) is the command that timed out; the
     line above it is where the sequence got that far.

Full legend: docs\POWER-BEFORE-AFTER.md   Context: docs\RESUME-1029.md
"@ | Write-Host
