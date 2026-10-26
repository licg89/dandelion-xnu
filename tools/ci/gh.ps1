# Minimal GitHub API helper using the credential already stored for
# https://github.com in Windows Credential Manager (via git-credential-manager).
# The token is never printed; only the API response is.

function Get-GHToken {
    $lines = @("protocol=https", "host=github.com", "", "") | & git credential fill 2>$null
    foreach ($line in $lines) {
        if ($line -like "password=*") { return $line.Substring(9) }
    }
    throw "no stored github credential"
}

function Invoke-GHApi {
    param(
        [Parameter(Mandatory = $true)][string]$Uri,
        [string]$Method = "GET",
        [object]$Body = $null,
        [switch]$Raw
    )
    $token = Get-GHToken
    $headers = @{
        Authorization          = "Bearer $token"
        "User-Agent"           = "dandelion-xnu-ci"
        Accept                 = "application/vnd.github+json"
        "X-GitHub-Api-Version" = "2022-11-28"
    }
    $args = @{ Uri = $Uri; Method = $Method; Headers = $headers; UseBasicParsing = $true }
    if ($null -ne $Body) {
        $args.Body = ($Body | ConvertTo-Json -Depth 6)
        $args.ContentType = "application/json"
    }
    $response = Invoke-WebRequest @args
    if ($Raw) { return $response.Content }
    return $response.Content | ConvertFrom-Json
}
