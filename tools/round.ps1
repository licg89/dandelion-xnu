# One test round on the phone: flash the chain, then get the kernel log back out.
#
# Why a script: a round used to be done from memory, and every step has been got
# wrong at least once (flashing while Android is still booting, forgetting that
# MTK LK cannot `fastboot boot`, pulling flashinfo from the wrong partition, or
# decoding the kernel log block without --scan).  The physical reality this
# encodes is in docs/HANDOVER-1027.md:155 -- when the kernel hangs the phone does
# NOT return to fastboot by itself, so a human has to hold power ~10 s and then
# Power + Volume Down.  Every stage below says exactly when.
#
# usage:
#   powershell -File tools\round.ps1 wait-fastboot
#   powershell -File tools\round.ps1 flash-chain        # boot + flashinfo, then reboot
#   powershell -File tools\round.ps1 collect-log        # stock boot -> Android -> pull -> decode
#   powershell -File tools\round.ps1 collect-log -Reinstall   # ... and flash the chain back
#   powershell -File tools\round.ps1 status
#
# Everything it writes goes to out\ (gitignored).

param(
  [Parameter(Position = 0)]
  [ValidateSet("wait-fastboot", "flash-chain", "collect-log", "status")]
  [string]$Stage = "status",
  [switch]$Reinstall,
  [int]$TimeoutMinutes = 10,
  [string]$Sdk = "C:\Users\li\AppData\Local\Android\Sdk\platform-tools"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$adb = Join-Path $Sdk "adb.exe"
$fastboot = Join-Path $Sdk "fastboot.exe"
$out = Join-Path $repo "out"

function Say($text, $colour = "Gray") { Write-Host $text -ForegroundColor $colour }

function Test-Fastboot {
  $lines = @(& $fastboot devices 2>&1)
  return @($lines | Where-Object { $_ -match "\s+fastboot$" })
}

function Test-Android {
  $lines = @(& $adb devices 2>&1)
  return @($lines | Where-Object { $_ -match "\s+device$" })
}

function Wait-For($kind, $minutes) {
  $deadline = (Get-Date).AddMinutes($minutes)
  while ((Get-Date) -lt $deadline) {
    if ($kind -eq "fastboot") {
      $d = @(Test-Fastboot)
      if ($d.Count -gt 0) { return $d[0] }
    } else {
      $d = @(Test-Android)
      if ($d.Count -gt 0) { return $d[0] }
    }
    Start-Sleep -Seconds 3
  }
  throw "no device in $kind after $minutes minutes"
}

function Require-Fastboot {
  $d = @(Test-Fastboot)
  if ($d.Count -gt 0) { return $d[0] }
  if (@(Test-Android).Count -gt 0) {
    Say "Device is in Android; asking it to reboot into the bootloader ..." Yellow
    & $adb reboot bootloader | Out-Null
    return Wait-For fastboot 3
  }
  Say ""
  Say "No device in fastboot and none in Android." Yellow
  Say "On the phone: hold Power ~10 s to force it off, then hold Power + Volume Down" Yellow
  Say "until the fastboot screen appears.  Waiting up to $TimeoutMinutes minutes ..." Yellow
  return Wait-For fastboot $TimeoutMinutes
}

switch ($Stage) {
  "status" {
    $fb = @(Test-Fastboot)
    $an = @(Test-Android)
    if ($fb.Count -gt 0) { Say "fastboot: $($fb[0])" Green }
    if ($an.Count -gt 0) { Say "android : $($an[0])" Green }
    if ($fb.Count -eq 0 -and $an.Count -eq 0) { Say "nothing attached (hung boot? power cycle needed)" Yellow }
  }

  "wait-fastboot" {
    $d = Require-Fastboot
    Say "fastboot: $d" Green
  }

  "flash-chain" {
    $boot = Join-Path $repo "artifacts\boot-uefi-redband.img"
    $esp = Join-Path $out "unixnu-esp.img"
    foreach ($f in @($boot, $esp)) {
      if (-not (Test-Path $f)) { throw "missing $f" }
    }
    $d = Require-Fastboot
    Say "fastboot: $d"
    Say ("boot    : {0} bytes  {1}" -f (Get-Item $boot).Length, (Get-FileHash $boot -Algorithm SHA256).Hash.ToLower())
    Say ("flashinfo: {0} bytes  {1}" -f (Get-Item $esp).Length, (Get-FileHash $esp -Algorithm SHA256).Hash.ToLower())
    & $fastboot flash boot $boot
    if ($LASTEXITCODE -ne 0) { throw "fastboot flash boot failed" }
    & $fastboot flash flashinfo $esp
    if ($LASTEXITCODE -ne 0) { throw "fastboot flash flashinfo failed" }
    & $fastboot reboot | Out-Null
    Say ""
    Say "Flashed and rebooting into the chain.  Give it 60-90 s to reach the hang," Yellow
    Say "then hold Power ~10 s, then Power + Volume Down, to get back to fastboot" Yellow
    Say "for 'collect-log'." Yellow
  }

  "collect-log" {
    # The stock image has lived in two places over this project's life: out\ on
    # the old machine and ..\artifacts\ (the D:\aios\artifacts drop) today.
    $stock = Join-Path $out "boot_backup.img"
    if (-not (Test-Path $stock)) {
      $stock = Join-Path (Split-Path -Parent $repo) "artifacts\boot_backup.img"
    }
    if (-not (Test-Path $stock)) { throw "missing boot_backup.img in out\ or ..\artifacts\" }
    Say "stock boot: $stock"
    $d = Require-Fastboot
    Say "fastboot: $d"
    Say "flashing the stock boot image so Android comes up and the log can be read ..."
    & $fastboot flash boot $stock
    if ($LASTEXITCODE -ne 0) { throw "fastboot flash boot (stock) failed" }
    & $fastboot reboot | Out-Null

    $d = Wait-For android 4
    Say "android : $d" Green
    Say "waiting for the system to settle ..."
    & $adb wait-for-device | Out-Null
    Start-Sleep -Seconds 12

    Say "pulling flashinfo (16 MiB) ..."
    & $adb shell "su -c 'dd if=/dev/block/by-name/flashinfo of=/data/local/tmp/fi.img bs=1M'"
    if ($LASTEXITCODE -ne 0) { throw "dd on the device failed (is su available?)" }
    $fi = Join-Path $out "fi.img"
    & $adb pull /data/local/tmp/fi.img $fi
    if ($LASTEXITCODE -ne 0) { throw "adb pull failed" }
    & $adb shell "rm -f /data/local/tmp/fi.img" | Out-Null

    Say "cutting and decoding the kernel log block ..."
    $py = "py"
    & $py -3.13 -c "d=open(r'$fi','rb').read(); open(r'$out\klog.bin','wb').write(d[15360000:15360000+512*512])"
    # --out, not a shell redirect: PowerShell would write UTF-16 and every later
    # reader would have to strip the NUL bytes first (docs/LOG-CHANNEL.md pitfall 1).
    & $py -3.13 (Join-Path $repo "tools\read_log_block.py") (Join-Path $out "klog.bin") --header --scan --out (Join-Path $out "kernel-boot.log")

    Say "decoding the firmware log block (LBA 30512) ..."
    & $py -3.13 (Join-Path $repo "tools\extract_log_block.py") $fi --lba 30512 --out (Join-Path $out "fw-boot.log")

    $lines = @(Get-Content (Join-Path $out "kernel-boot.log") -Encoding utf8)
    Say ("kernel log: {0} lines -> out\kernel-boot.log" -f $lines.Count) Green

    if ($Reinstall) {
      Say ""
      Say "flashing the chain back for the next round ..." Yellow
      & $adb reboot bootloader | Out-Null
      $d = Wait-For fastboot 3
      & $fastboot flash boot (Join-Path $repo "artifacts\boot-uefi-redband.img")
      & $fastboot flash flashinfo (Join-Path $out "unixnu-esp.img")
      & $fastboot reboot | Out-Null
      Say "chain reinstalled." Green
    }
  }
}
