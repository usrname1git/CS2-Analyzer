#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InputPath,
    [Parameter(Mandatory)][ValidatePattern('^[1-9][0-9]{0,19}$')][string]$SteamId,
    [string]$OutDir = (Join-Path $PSScriptRoot 'results'),
    [string]$Platform = 'Unknown',
    [string]$SuspectName = 'Player',
    [ValidateRange(0, 100000)][int]$Elo = 0,
    [string]$AnalyzerPath = (Join-Path $PSScriptRoot 'build\Release\cs2-analyzer.exe'),
    [string]$CsdaPath = (Join-Path $PSScriptRoot 'tools\csda\csda.exe'),
    [string]$CacheDir = (Join-Path $PSScriptRoot 'cache'),
    [ValidateRange(1, 7200)][int]$TimeoutSeconds = 300,
    [switch]$Positions,
    [switch]$Recurse
)

$ErrorActionPreference = 'Stop'
function Get-Sha256Utf8([string]$Text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))).Replace('-', '').ToLowerInvariant()
    }
    finally { $sha.Dispose() }
}
[uint64]$null = [uint64]::Parse($SteamId)
if (-not (Test-Path -LiteralPath $AnalyzerPath -PathType Leaf)) {
    throw "Analyzer executable missing. Run scripts\Build.ps1 first: $AnalyzerPath"
}
$AnalyzerPath = (Resolve-Path -LiteralPath $AnalyzerPath).Path
$item = Get-Item -LiteralPath $InputPath
$files = if ($item.PSIsContainer) {
    @(Get-ChildItem -LiteralPath $item.FullName -File -Recurse:$Recurse |
        Where-Object { $_.Extension -in '.dem', '.json' } | Sort-Object FullName)
}
else { @($item) }
if ($files.Count -eq 0) { throw 'No .dem or CSDA .json input files found.' }
if (@($files | Where-Object { $_.Extension -notin '.dem', '.json' }).Count -gt 0) {
    throw 'Only .dem and CSDA .json inputs are supported.'
}
$OutDir = [IO.Path]::GetFullPath($OutDir)
$CacheDir = [IO.Path]::GetFullPath($CacheDir)
$null = New-Item -ItemType Directory -Path $OutDir -Force
$null = New-Item -ItemType Directory -Path $CacheDir -Force

function Invoke-BoundedProcess {
    param([string]$Executable, [string[]]$Arguments, [int]$LimitSeconds)
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.Encoding]::UTF8
    $start.StandardErrorEncoding = [Text.Encoding]::UTF8
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    $started = $false
    try {
        if (-not $process.Start()) { throw "Failed to start $Executable" }
        $started = $true
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($LimitSeconds * 1000)) {
            $process.Kill($true)
            if (-not $process.WaitForExit(10000)) { throw "Child PID $($process.Id) did not exit after termination." }
            throw "Process exceeded ${LimitSeconds}s; terminated created child PID $($process.Id) and its process tree."
        }
        @{
            ExitCode = $process.ExitCode
            Stdout = $stdout.GetAwaiter().GetResult()
            Stderr = $stderr.GetAwaiter().GetResult()
        }
    }
    finally {
        if ($started -and -not $process.HasExited) {
            $process.Kill($true)
            $null = $process.WaitForExit(10000)
        }
        $process.Dispose()
    }
}

$hasDemos = @($files | Where-Object Extension -EQ '.dem').Count -gt 0
$decoderHash = $null
$decoderVersion = 'not_used'
if ($hasDemos) {
    if (-not (Test-Path -LiteralPath $CsdaPath -PathType Leaf)) {
        throw "CSDA missing. Run scripts\Install-Csda.ps1 first or supply -CsdaPath: $CsdaPath"
    }
    $CsdaPath = (Resolve-Path -LiteralPath $CsdaPath).Path
    $decoderHash = (Get-FileHash -LiteralPath $CsdaPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $installedPath = Join-Path (Split-Path -Parent $CsdaPath) 'installed.json'
    if (Test-Path -LiteralPath $installedPath) {
        $installed = Get-Content -LiteralPath $installedPath -Raw | ConvertFrom-Json
        if ($installed.exe_sha256 -ne $decoderHash) { throw 'CSDA binary does not match its installation receipt.' }
        $decoderVersion = $installed.version
    }
    else {
        $decoderVersion = 'custom_binary'
        Write-Warning 'Custom CSDA executable: its SHA-256 is recorded, but release provenance is not established.'
    }
}

$receipts = [Collections.Generic.List[object]]::new()
$inputs = [Collections.Generic.List[string]]::new()
$source = $Platform.ToLowerInvariant()
$supportedSources = @('challengermode', 'ebot', 'esea', 'esl', 'esportal', 'faceit',
    'fastcup', '5eplay', 'perfectworld', 'popflash', 'valve')
if ($source -eq 'premier') { $source = 'valve' }
if ($source -notin $supportedSources) { $source = '' }
foreach ($file in $files) {
    if ($file.Extension -eq '.json') {
        $inputs.Add($file.FullName)
        $receipts.Add(@{
            mode = 'supplied-json'
            json_path = $file.FullName
            json_sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        })
        continue
    }
    $demoHash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $identity = "$demoHash|$decoderHash|$source|$Positions"
    $cacheKey = Get-Sha256Utf8 $identity
    $jsonPath = Join-Path $CacheDir "$cacheKey.json"
    $receiptPath = Join-Path $CacheDir "$cacheKey.receipt.json"
    $reusable = $false
    if ((Test-Path -LiteralPath $jsonPath) -and (Test-Path -LiteralPath $receiptPath)) {
        $prior = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
        $reusable = $prior.demo_sha256 -eq $demoHash -and $prior.decoder_sha256 -eq $decoderHash -and
            $prior.json_sha256 -eq (Get-FileHash -LiteralPath $jsonPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if (-not $reusable) { Write-Warning "Cache receipt mismatch; decoding again: $($file.Name)" }
    }
    if (-not $reusable) {
        $temporary = Join-Path $CacheDir "$cacheKey.$([guid]::NewGuid().ToString('N')).pending.json"
        [IO.File]::WriteAllText($temporary, '', [Text.UTF8Encoding]::new($false))
        try {
            $arguments = @("-demo-path=$($file.FullName)", "-output=$temporary", '-format=json', '-minify')
            if ($source) { $arguments += "-source=$source" }
            if ($Positions) { $arguments += '-positions' }
            Write-Host "Decoding: $($file.Name)"
            $result = Invoke-BoundedProcess -Executable $CsdaPath -Arguments $arguments -LimitSeconds $TimeoutSeconds
            $result.Stdout | Set-Content -LiteralPath (Join-Path $CacheDir "$cacheKey.stdout.txt") -Encoding utf8NoBOM
            $result.Stderr | Set-Content -LiteralPath (Join-Path $CacheDir "$cacheKey.stderr.txt") -Encoding utf8NoBOM
            if ($result.ExitCode -ne 0) { throw "CSDA failed ($($result.ExitCode)): $($result.Stderr) $($result.Stdout)" }
            if ((Get-Item -LiteralPath $temporary).Length -eq 0) { throw 'CSDA returned success but produced empty JSON.' }
            if ((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -ne $demoHash) {
                throw 'Demo changed while it was being decoded.'
            }
            Move-Item -LiteralPath $temporary -Destination $jsonPath -Force
        }
        finally {
            if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
        }
    }
    else { Write-Host "Verified cache: $($file.Name)" }
    $receipt = @{
        mode = 'decoded-demo'
        demo_path = $file.FullName
        demo_sha256 = $demoHash
        json_path = $jsonPath
        json_sha256 = (Get-FileHash -LiteralPath $jsonPath -Algorithm SHA256).Hash.ToLowerInvariant()
        decoder_version = $decoderVersion
        decoder_sha256 = $decoderHash
        source = $source
        positions = [bool]$Positions
        cache_reused = $reusable
    }
    $receipt | ConvertTo-Json | Set-Content -LiteralPath $receiptPath -Encoding utf8NoBOM
    $receipts.Add($receipt)
    $inputs.Add($jsonPath)
}

$safeName = ($SuspectName -replace '[^a-zA-Z0-9_-]', '_').Trim('_')
if (-not $safeName) { $safeName = 'Player' }
if ($safeName.Length -gt 40) { $safeName = $safeName.Substring(0, 40) }
$prefix = "CS2-Analysis-$safeName-" + (Get-Date -Format 'yyyyMMdd-HHmmss')
$runDirectory = Join-Path $OutDir ($prefix + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$provenance = Join-Path $CacheDir ('inputs-' + [guid]::NewGuid().ToString('N') + '.json')
ConvertTo-Json -InputObject @($receipts.ToArray()) -Depth 6 |
    Set-Content -LiteralPath $provenance -Encoding utf8NoBOM
try {
    $arguments = @('--steam-id', $SteamId, '--output', $runDirectory, '--name', $SuspectName,
        '--platform', $Platform, '--elo', "$Elo", '--prefix', $prefix, '--provenance', $provenance)
    foreach ($input in $inputs) { $arguments += @('--input', $input) }
    $result = Invoke-BoundedProcess -Executable $AnalyzerPath -Arguments $arguments -LimitSeconds $TimeoutSeconds
    if ($result.Stdout) { Write-Host $result.Stdout.TrimEnd() }
    if ($result.Stderr) { Write-Host $result.Stderr.TrimEnd() }
    if ($result.ExitCode -ne 0) { throw "Analyzer failed with exit code $($result.ExitCode)." }
    $manifestPath = Join-Path $runDirectory 'manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath)) { throw 'Analyzer returned success without a report manifest.' }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.status -ne 'complete') { throw 'Report manifest is not complete.' }
    $runDirectory
}
finally {
    if (Test-Path -LiteralPath $provenance) { Remove-Item -LiteralPath $provenance -Force }
}
