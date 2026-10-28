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
  $Image = Join-Path $repo "artifacts\boot-uefi-m5-dma.img"
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

What to photograph: one shot of the lower third of the screen. Everything below is
permanent until the next reset.

  y 1244..1373  readout, two columns of five, big digits
  y 1570..1596  one full-width bar
  y 1400/1448/1496  three rows of eight stage cells

Reading it, in order of importance:

  1. The log, in the text area -- look for these lines:
       ddn data cmd=8 r1=XXXXXXXX len=512      the card's own answer
       ddn dma ok ...                           DATA ARRIVED -> M4 done
       ddn dma FAIL int=...                     DMA could not move data either
       ddn pio-read FAIL int=... left=...       PIO, after DMA gave up
       ddn  ps_or=... ps_and=...                did anything ever drive DAT0?
       ddn data cmd=8 refused, r1 has error...  the card refused the command

  2. INTR= in the readout:
       00001000  transfer complete
       00004000  data timeout (what it has always been)
       00008000  data CRC error

  3. R1  = the card status.  Low nibble of bits 12:9 is CURRENT_STATE;
       4 = Tran (normal).  Bit 8 is READY_FOR_DATA.

  4. ps_or vs ps_and (text area):
       equal      nothing ever drove the data lines -- card state, not wiring
       different  something did, so look at sampling / bus width

  5. The y 1570 bar:
       orange  still looking for a payload       green  payload started
       red     no payload / payload failed       white blocks at the ends:
               right = device tree published, left = log written to flash

  6. Stage cells (row A / B / C = stages 0-7 / 8-15 / 16-23).  Cell 22 is the
     512-byte data read: green = it worked, red = it did not.

Full legend: docs\BOOT-FLOW.md   eMMC analysis: docs\ANALYSIS-EMMC-DATA-PATH.md
"@ | Write-Host
