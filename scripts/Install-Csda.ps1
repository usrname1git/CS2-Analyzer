#requires -Version 7.0
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$lock = Get-Content -LiteralPath (Join-Path $root 'tools\csda.lock.json') -Raw | ConvertFrom-Json
$destination = Join-Path $root 'tools\csda'
if (Test-Path -LiteralPath $destination) {
    $receiptPath = Join-Path $destination 'installed.json'
    if (-not (Test-Path -LiteralPath $receiptPath)) { throw 'Existing CSDA directory lacks an installation receipt.' }
    $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    $exe = Join-Path $destination 'csda.exe'
    if ($receipt.version -ne $lock.version -or $receipt.archive_sha256 -ne $lock.sha256 -or
        -not (Test-Path -LiteralPath $exe) -or
        (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant() -ne $receipt.exe_sha256) {
        throw 'Existing CSDA installation differs from the lock or receipt; not overwriting it.'
    }
    Write-Host "Pinned CSDA already installed: $exe"
    return
}

$staging = Join-Path $root ('tools\csda-stage-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $staging
try {
    $archive = Join-Path $staging 'windows-x64.zip'
    Invoke-WebRequest -Uri $lock.url -OutFile $archive
    $hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $lock.sha256) { throw 'CSDA release archive SHA-256 does not match the pinned manifest.' }
    $expanded = Join-Path $staging 'expanded'
    Expand-Archive -LiteralPath $archive -DestinationPath $expanded
    $executables = @(Get-ChildItem -LiteralPath $expanded -Recurse -File -Filter 'csda.exe')
    if ($executables.Count -ne 1) { throw 'CSDA archive must contain exactly one csda.exe.' }
    $install = Join-Path $staging 'install'
    $null = New-Item -ItemType Directory -Path $install
    Copy-Item -LiteralPath $executables[0].FullName -Destination (Join-Path $install 'csda.exe')
    Copy-Item -LiteralPath (Join-Path $root 'THIRD_PARTY_NOTICES.md') -Destination $install
    @{
        version = $lock.version
        archive_sha256 = $hash
        exe_sha256 = (Get-FileHash -LiteralPath (Join-Path $install 'csda.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
        url = $lock.url
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $install 'installed.json') -Encoding utf8NoBOM
    Move-Item -LiteralPath $install -Destination $destination
    Write-Host "Installed pinned $($lock.version): $(Join-Path $destination 'csda.exe')"
}
finally {
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
}
