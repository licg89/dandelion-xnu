# Flash the current dandelion UEFI image, and say what to look for afterwards.
#
#   powershell -File tools\flash.ps1            # flash artifacts/boot-uefi-m5-dma.img
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
  $Image = "D:\aios\artifacts\boot_backup.img"
  if (-not (Test-Path $Image)) {
    throw "stock boot image not found at $Image"
  }
} elseif ([string]::IsNullOrEmpty($Image)) {
  $Image = Join-Path $repo "artifacts\boot-uefi-blockio.img"
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
  text area             10 lines, fixed position (rows 53..62 of 68):
                        PWR PS=.. DAT=.. CMD=.. CD=.. VEMC=.. vsel=.. w=.. f=..
                        H0 C=.. E=.. I=.. R=..          (controller leftovers)
                        H0 S=.. P=.. N=..               (SDC_CFG / PS / BLK_NUM)
                        C0 I=.. cl=.. f=.. l=.. kc=..   (CMD0: TX self-check)
                        C1 n=.. I=.. R=..               (CMD1 until OCR bit31)
                        C2 3=.. 2=.. 1=.. 0=..          (CID, R2)
                        C3 I=.. R=.. C7 I=.. R=..       (RCA, select)
                        C13 I=.. R=.. st=..             (4 = TRAN)
                        C8 I=.. sz=.. rv=.. bw=.. hs=.. (EXT_CSD, 512-byte read)
                        B1 I=.. S=..                    (LBA 1, GPT header)

Reading it, in this order:

  0. `C13 st=4` + `C8 I=...0100` + `B1 S=45464920` is the goal: commands, a
     128-bit response, a 512-byte data read and sector addressing all work, and
     what is left is the UEFI driver, not the board.

  1. `C0` is the transmitter self-check: CMD0 has no response, so its f..l is
     the pure 48-bit frame.  About 100 samples low in one run is healthy.

  2. `C1 n=..` is how many CMD1 attempts it took for OCR bit 31 (power-up done)
     to come back set.  `CARD NOT READY` instead means the card stayed busy.

  3. `C8` is the data phase.  Its `I` says which half failed: ...0100 XFER_COMPL
     (good), ...4000 DATTMO (the card never started sending), ...8000 DATCRCERR
     (data on the wire, sample window wrong).  `sz=` is SEC_COUNT in 512-byte
     sectors, i.e. the size of the eMMC.

  4. `B1 S=45464920` is "EFI " from the GPT header at LBA 1; anything else means
     the read worked but the addressing or the partition table did not.

  5. Any line whose `I` is 00000200 (CMDTMO) is the command that timed out; the
     line above it is where the sequence got that far.

Full legend: docs\POWER-BEFORE-AFTER.md   Context: docs\RESUME-1029.md
"@ | Write-Host
