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

$env:PATH = "C:\Program Files\LLVM\bin;C:\Program Files\Git\usr\bin;" + $env:PATH

Write-Host "== building device shim (xnu) =="
& "C:\Program Files\Git\bin\bash.exe" -lc "cd /d/aios/dandelion-xnu/bootshim && CC=clang bash build.sh xnu"
if ($LASTEXITCODE -ne 0) { throw "shim build failed" }

Push-Location $bootshim
try {
    Write-Host "== building payload =="
    python mkxnu.py $Kernel -o out/payload-dandelion.bin --cmdline $Cmdline
    if ($LASTEXITCODE -ne 0) { throw "mkxnu failed" }

    # The boot partition holds a plain Android boot image v2: the kernel
    # section starts with gzip(shim) and has no MediaTek 512-byte section
    # header.  patch_boot.py keeps the stock header and kernel_size and only
    # replaces the sections, which is what the bootloader expects.
    Write-Host "== packing boot image =="
    python patch_boot.py ../out/boot_backup.img out/Image.xnu ../out/dandelion.dtb `
        ../out/boot_xnu.img out/payload-dandelion.bin
    if ($LASTEXITCODE -ne 0) { throw "patch_boot failed" }
} finally {
    Pop-Location
}

# Restore the shim's out/Image to the QEMU variant so qemu_check.py keeps working.
$qemuImage = Join-Path $bootshim "out\Image.xnu-qemu"
if (Test-Path $qemuImage) {
    Copy-Item $qemuImage (Join-Path $bootshim "out\Image") -Force
} else {
    Write-Warning "bootshim\out\Image.xnu-qemu is missing; run 'build.sh xnu-qemu' before using QEMU again"
}

Get-Item (Join-Path $out "boot_xnu.img") | ForEach-Object { "boot_xnu.img: $($_.Length) bytes" }
