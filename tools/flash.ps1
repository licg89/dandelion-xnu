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
  $Image = Join-Path $repo "artifacts\boot-uefi-cmd0.img"
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
  y 1186  white strip   PS_AND: bits that stayed high during the hand-sent CMD1
  y 1200  cyan strip    PS_OR:  bits that were high at some point during it
  x 400..560 y 1197     verdict patch
  y 1216  dots          blue = block devices (x 8), green = volumes (x 380)
  y 1400/1448/1496      stage cells.  16..23 are MsdcDxe's; this driver does
                        not paint them any more, so they stay the M4 evidence.
  text area             PS before=.. DAT=FF CMD=1 CD=1 / VEMC=.. en=N ANA=..
                        MUX strap=.. mode=N m0=.. m1=.. / DIN 122..133=....
                        PAD0=.. PAD1=..
                        C0 I=.. cl=.. f=.. l=.. kc=..   (CMD0, no response)
                        PING a I=.. R=.. cl=.. f=.. l=.. kc=..
                        PING b I=.. R=.. cl=.. f=.. l=.. kc=..
                        CARD ANSWERED | NO ANSWER

Reading it, in this order:

  0. Power is settled: PS = DAT=0xFF CMD=1, VEMC on at 3.0 V.  Do not reopen it.
     The mux is settled too: m0 reads 22222200 and survives a reboot.

  1. `cl=` `f=` `l=` come from GPIO122, the CMD pad itself, sampled while the
     command is on the wire -- not from MSDC_PS.  PS masks CMD while the
     controller drives it (IMG_5744: cl=0 there, while the same pin toggled).
       cl=0            nothing went out at all -> host side (SDC_CMD write,
                       clock gate, controller reset), not the card.
       cl>0, l-f small the command frame went out and nothing came back.
       l ~ 2*(f..l of C0)  the card DID answer and this controller failed to
                       sample it -> MSDC_PAD_TUNE0 / EMMC50_CFG0, a different bug.

  2. C0 is the transmitter self-check: CMD0 has no response, so its f..l is the
     pure 48-bit frame.  Compare CMD1's range against it.

  3. `kc=` how often the CLK pad read 0 in the same window.  A number near 0 or
     near 30000 means the clock was not running; anything in between is fine.

  4. PING a/b: CMDRDY is bit 8 of I.  If either sets it the card is alive and
     R= is its OCR (bit 31 set = powered up).  b repeats CMD1 with a plain OCR
     argument, so a rejection of bit 30/31 is ruled out.

Full legend: docs\POWER-BEFORE-AFTER.md   Context: docs\RESUME-1029.md
"@ | Write-Host
