# measure_beacon.ps1 - time the watchdog beacon without a serial console.
#
# The dandelion has no UART and the UEFI tree cannot write to eMMC yet, so the
# only observable is "how long the device stays off the USB bus between two
# fastboot windows".  Our FD does not initialise USB, so:
#
#   LK fastboot visible  ->  jump to the FD (USB goes away)  ->  watchdog reset
#   -> LK again ...
#
# The *gap* between two consecutive fastboot windows is therefore the FD's own
# run time, which maps straight onto the beacon table in
# docs/HANDOVER-UEFI-1028.md:
#
#   ~10 s  FD never executed (LK's own watchdog)
#   ~20 s  SEC entry reached, died before PlatformPeim
#   ~30 s  past memory init, died before DXE
#    ~2 s  DXE core + dispatcher + FV alive
#   no loop at all -> the FD did not run (Android came up instead)
#
# Usage:
#   .\tools\measure_beacon.ps1                 # watch for 180 s
#   .\tools\measure_beacon.ps1 -Seconds 240

param(
    [int]$Seconds = 180,
    [int]$PollMs = 400
)

$ErrorActionPreference = 'SilentlyContinue'
$fb = 'C:\Users\li\AppData\Local\Android\Sdk\platform-tools\fastboot.exe'
$adb = 'C:\Users\li\AppData\Local\Android\Sdk\platform-tools\adb.exe'

function Test-Fastboot {
    $job = Start-Job -ScriptBlock { param($exe) & $exe devices 2>&1 } -ArgumentList $fb
    $null = Wait-Job $job -Timeout 3
    $out = Receive-Job $job
    Remove-Job $job -Force
    return [bool]($out -match '\tfastboot')
}

function Test-Adb {
    $job = Start-Job -ScriptBlock { param($exe) & $exe devices 2>&1 } -ArgumentList $adb
    $null = Wait-Job $job -Timeout 3
    $out = Receive-Job $job
    Remove-Job $job -Force
    return [bool]($out -match '\tdevice$')
}

$start = Get-Date
$last = $null
$state = $null
$transitions = @()

Write-Host ("watching for {0}s, poll every {1}ms" -f $Seconds, $PollMs)

while (((Get-Date) - $start).TotalSeconds -lt $Seconds) {
    $now = [math]::Round(((Get-Date) - $start).TotalSeconds, 2)
    if (Test-Fastboot) { $s = 'fastboot' }
    elseif (Test-Adb)  { $s = 'android' }
    else               { $s = 'gone' }

    if ($s -ne $state) {
        $transitions += [pscustomobject]@{ t = $now; state = $s }
        $line = "{0,8:N2}s  {1}" -f $now, $s
        if ($last -ne $null -and $s -eq 'fastboot' -and $state -eq 'gone') {
            $line += ("   <- FD ran for ~{0:N1}s before the reset" -f ($now - $last))
        }
        if ($s -eq 'fastboot' -or $s -eq 'android') { $last = $now }
        Write-Host $line
        $state = $s
    }
    Start-Sleep -Milliseconds $PollMs
}

Write-Host ''
Write-Host '--- summary ---'
if ($transitions.Count -eq 0) {
    Write-Host 'no USB activity at all (device off / not plugged in)'
    return
}

$windows = @()
for ($i = 1; $i -lt $transitions.Count; $i++) {
    if ($transitions[$i].state -eq 'fastboot' -and $transitions[$i - 1].state -eq 'gone') {
        $windows += ($transitions[$i].t - $transitions[$i - 1].t)
    }
}

if ($windows.Count -eq 0) {
    Write-Host 'saw no second fastboot window -> the FD did not reset the device,'
    Write-Host 'or the device booted Android instead (state list above).'
} else {
    Write-Host ("fastboot windows: {0}" -f $windows.Count)
    Write-Host ("FD run time: min {0:N1}s  max {1:N1}s  avg {2:N1}s" -f `
        ($windows | Measure-Object -Minimum).Minimum, `
        ($windows | Measure-Object -Maximum).Maximum, `
        ($windows | Measure-Object -Average).Average)
}
