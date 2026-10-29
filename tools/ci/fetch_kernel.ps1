# Wait for the latest build-xnu run to finish, then download and extract its
# mach_kernel artifact into Resources/UniXNU/.
#
#   powershell -File tools\ci\fetch_kernel.ps1                 # poll, then fetch
#   powershell -File tools\ci\fetch_kernel.ps1 -RunId 12345    # a specific run
#   powershell -File tools\ci\fetch_kernel.ps1 -NoWait         # fetch only if done
#
# Credentials come from tools/ci/gh.ps1, i.e. from whatever git-credential-manager
# has stored for https://github.com -- no gh auth login needed.
#
# Why this exists: XNU only builds on macOS, and build-xnu.yml builds it on a
# macos-14 runner and uploads mach_kernel as an artifact.  The workflow had never
# been dispatched, which is why M6 spent so long described as "blocked on external
# artefacts".  It is not blocked; it is a queue.
param(
    [string]$RunId,
    [int]$TimeoutMinutes = 180,
    [switch]$NoWait,
    [string]$Repo = "licg89/dandelion-xnu"
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\gh.ps1"

$api = "https://api.github.com/repos/$Repo"

if (-not $RunId) {
    $runs = Invoke-GHApi -Uri "$api/actions/runs?per_page=10"
    $run = $runs.workflow_runs | Where-Object { $_.name -eq "build-xnu" } | Select-Object -First 1
    if (-not $run) { Write-Output "no build-xnu run at all -- dispatch it first"; exit 2 }
    $RunId = $run.id
}

$deadline = (Get-Date).AddMinutes($TimeoutMinutes)
while ($true) {
    $run = Invoke-GHApi -Uri "$api/actions/runs/$RunId"
    $line = "{0}  run {1}  {2}  {3}" -f (Get-Date).ToString("HH:mm:ss"), $RunId, $run.status, $run.conclusion
    Write-Output $line
    if ($run.status -eq "completed") { break }
    if ($NoWait) { Write-Output "not finished yet"; exit 2 }
    if ((Get-Date) -gt $deadline) { Write-Output "timed out waiting"; exit 3 }
    Start-Sleep -Seconds 60
}

if ($run.conclusion -ne "success") {
    Write-Output "run concluded '$($run.conclusion)' -- read the job log before fetching"
    $jobs = Invoke-GHApi -Uri "$api/actions/runs/$RunId/jobs"
    foreach ($job in $jobs.jobs) {
        Write-Output ("  job {0}: {1}/{2}" -f $job.name, $job.status, $job.conclusion)
        foreach ($step in $job.steps) { Write-Output ("    - {0}: {1}/{2}" -f $step.name, $step.status, $step.conclusion) }
    }
    exit 4
}

$arts = Invoke-GHApi -Uri "$api/actions/runs/$RunId/artifacts"
$art = $arts.artifacts | Where-Object { $_.name -like "mach_kernel*" } | Select-Object -First 1
if (-not $art) { Write-Output "run succeeded but carries no mach_kernel artifact"; exit 5 }
Write-Output ("artifact {0} ({1} bytes)" -f $art.name, $art.size_in_bytes)

$token = Get-GHToken
$zip = Join-Path $env:TEMP "mach_kernel.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
& curl.exe -sSL -H "Authorization: Bearer $token" -o $zip "$api/actions/artifacts/$($art.id)/zip"
if ($LASTEXITCODE -ne 0) { Write-Output "download failed"; exit 6 }

$dest = Join-Path $env:TEMP "mach_kernel_art"
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
Expand-Archive -Path $zip -DestinationPath $dest -Force
Get-ChildItem $dest -Recurse -File | ForEach-Object { "  " + $_.FullName.Substring($dest.Length + 1) + "  " + $_.Length }

$kernel = Get-ChildItem $dest -Recurse -File -Filter "mach_kernel" | Select-Object -First 1
if (-not $kernel) { Write-Output "no file named mach_kernel in the artifact"; exit 7 }

$repoRoot = Split-Path (Split-Path $PSScriptRoot)
$out = Join-Path $repoRoot "Resources\UniXNU\mach_kernel"
Copy-Item $kernel.FullName $out -Force
Write-Output ("wrote {0} ({1} bytes)" -f $out, (Get-Item $out).Length)
Write-Output "next: bash tools/mk_unixnu_esp.sh, then push the image with adb"
