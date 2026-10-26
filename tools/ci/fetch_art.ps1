param(
    [Parameter(Mandatory = $true)][string]$Sha,
    [string]$Machine = "dandelion",
    [string]$Dest = "C:\Users\li\AppData\Local\Temp\opencode\xnu-log\dandelion-art"
)
. "$PSScriptRoot\gh.ps1"

$runs = Invoke-GHApi -Uri "https://api.github.com/repos/licg89/dandelion-xnu/actions/runs?per_page=20"
$run = $runs.workflow_runs | Where-Object { $_.head_sha -like "$Sha*" -and $_.event -eq "workflow_dispatch" } | Select-Object -First 1
if (-not $run) { Write-Output "no dispatched run for $Sha"; exit 2 }
Write-Output "run $($run.id) status=$($run.status) conclusion=$($run.conclusion)"
if ($run.status -ne "completed") { exit 2 }
if ($run.conclusion -ne "success") { exit 3 }

$a = Invoke-GHApi -Uri "https://api.github.com/repos/licg89/dandelion-xnu/actions/runs/$($run.id)/artifacts"
$art = $a.artifacts | Where-Object { $_.name -like "*$Machine*" } | Select-Object -First 1
if (-not $art) { Write-Output "no artifact"; exit 4 }

$token = Get-GHToken
$zip = Join-Path $env:TEMP "art.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
& curl.exe -sSL -H "Authorization: Bearer $token" -o $zip "https://api.github.com/repos/licg89/dandelion-xnu/actions/artifacts/$($art.id)/zip"
if ($LASTEXITCODE -ne 0) { Write-Output "download failed"; exit 5 }

if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
New-Item -ItemType Directory -Path $Dest | Out-Null
Expand-Archive -Path $zip -DestinationPath $Dest -Force
Write-Output "extracted to $Dest"
Get-ChildItem $Dest -Recurse -File | ForEach-Object { "  " + $_.FullName.Substring($Dest.Length + 1) + "  " + $_.Length }
