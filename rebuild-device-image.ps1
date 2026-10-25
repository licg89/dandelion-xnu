# Rebuild out/boot_xnu.img (the fastboot-flashable image) from a mach_kernel.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File rebuild-device-image.ps1 `
#       -Kernel C:\Users\li\AppData\Local\Temp\opencode\xnu-log\dandelion-art\mach_kernel
#
# The device image needs the plain "xnu" shim (real dandelion ADT/framebuffer),
# not the "xnu-qemu" one used for QEMU.  build.sh keeps out/Image.<mode>, so
# out/Image is restored to the QEMU variant afterwards and QEMU runs keep
# working.
param(
    [Parameter(Mandatory = $true)][string]$Kernel,
    [string]$Cmdline = "-v serial=3 debug=0x14e keepsyms=1 cpus=1 -noprogress wdt=-1"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$bootshim = Join-Path $root "bootshim"
$out = Join-Path $root "out"

if (-not (Test-Path $Kernel)) { throw "kernel not found: $Kernel" }
if (-not (Test-Path (Join-Path $out "boot_backup.img"))) {
    throw "out\boot_backup.img is missing - refusing to build without a recovery image"
}

$env:PATH = "C:\Program Files\LLVM\bin;" + $env:PATH

Write-Host "== building device shim (xnu) =="
& "C:\Program Files\Git\bin\bash.exe" -lc "cd /d/aios/dandelion-xnu/bootshim && CC=clang bash build.sh xnu"
if ($LASTEXITCODE -ne 0) { throw "shim build failed" }

Push-Location $bootshim
try {
    Write-Host "== building payload =="
    python mkxnu.py $Kernel -o out/payload-dandelion.bin --cmdline $Cmdline
    if ($LASTEXITCODE -ne 0) { throw "mkxnu failed" }

    Write-Host "== packing boot image =="
    python mkbootimg_mtk.py ../out/boot_backup.img out/Image.xnu ../out/dandelion.dtb `
        out/payload-dandelion.bin ../out/boot_xnu.img
    if ($LASTEXITCODE -ne 0) { throw "mkbootimg failed" }
} finally {
    Pop-Location
}

# Restore out/Image to the QEMU shim so qemu_check.py keeps working.
if (Test-Path (Join-Path $out "Image.xnu-qemu")) {
    Copy-Item (Join-Path $out "Image.xnu-qemu") (Join-Path $out "Image") -Force
}

Get-Item (Join-Path $out "boot_xnu.img") | ForEach-Object { "boot_xnu.img: $($_.Length) bytes" }
