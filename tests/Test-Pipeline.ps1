#requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$AnalyzerPath,
    [Parameter(Mandatory)][string]$DecoderPath
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('CS2-Analyzer-test-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $temporary
function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Reject([scriptblock]$Action, [string]$Message) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    Assert $rejected $Message
}
try {
    $steam = '76561198000000001'
    $kill = @{
        roundNumber=1;tick=20;weaponName='AK-47';weaponType='rifle'
        killerSteamId=[uint64]$steam;victimSteamId=[uint64]'76561198000000002'
        killerSide=2;victimSide=3;killerName='<script>alert(1)</script>'
        victimName='=HYPERLINK("evil")';assisterSteamId=0;assisterSide=0;isHeadshot=$true
    }
    $fixture = @{
        checksum='synthetic-pipeline';game='CS2';mapName='de_fixture';source='faceit'
        demoFileName='fixture.dem';tickrate=64;winner=@{letter='A'}
        players=@{$steam=@{steamId=[uint64]$steam;name='Player';killCount=1;deathCount=0;assistCount=0;headshotCount=1;healthDamage=100}}
        rounds=@(@{number=1;teamASide=2;teamBSide=3;winnerSide=2;overtimeNumber=0})
        kills=@($kill);shots=@()
        damages=@(@{roundNumber=1;tick=20;attackerSteamId=[uint64]$steam
            victimSteamId=[uint64]'76561198000000002';attackerSide=2;victimSide=3
            healthDamage=100;weaponType='rifle';weaponName='AK-47'})
        playerEconomies=@(@{roundNumber=1;steamId=[uint64]$steam;playerSide=2})
    }
    $jsonPath = Join-Path $temporary 'input space & unicode-é.json'
    $fixture | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $jsonPath -Encoding utf8NoBOM
    $output = Join-Path $temporary 'direct'
    & $AnalyzerPath --input $jsonPath --input $jsonPath --steam-id $steam --output $output --name '<script>bad</script>'
    Assert ($LASTEXITCODE -eq 0) 'CLI must succeed'
    $analysis = Get-Content -LiteralPath (Join-Path $output 'analysis.json') -Raw | ConvertFrom-Json
    Assert ($analysis.summary.kills -eq 1 -and $analysis.summary.matches -eq 1) 'Duplicate must not inflate totals'
    Assert ($null -eq $analysis.summary.kd) 'Zero deaths must not yield invented finite K/D'
    $manifest = Get-Content -LiteralPath (Join-Path $output 'manifest.json') -Raw | ConvertFrom-Json
    Assert ($manifest.inputs[1].status -eq 'duplicate') 'Duplicate receipt'
    foreach ($file in $manifest.outputs) {
        $actual = (Get-FileHash -LiteralPath (Join-Path $output $file.path) -Algorithm SHA256).Hash.ToLowerInvariant()
        Assert ($actual -eq $file.sha256) "Output hash mismatch: $($file.path)"
    }
    $html = Get-Content -LiteralPath (Join-Path $output 'CS2-Analysis-report.html') -Raw
    Assert ($html.Contains('&lt;script&gt;bad&lt;/script&gt;')) 'Name must be escaped'
    Assert (-not $html.Contains('<script>bad')) 'No untrusted HTML injection'
    Assert (-not ($html -match 'src=["'']https?://')) 'Report must not need remote assets'
    Assert ($html.Contains('CHEATING NOT ASSESSED')) 'No automatic verdict'
    & $AnalyzerPath --input $jsonPath --steam-id $steam --output $output
    Assert ($LASTEXITCODE -ne 0) 'Existing output must not be overwritten'
    $fixture.kills[0].tick=21
    $conflict = Join-Path $temporary 'conflict.json'
    $fixture | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $conflict -Encoding utf8NoBOM
    & $AnalyzerPath --input $jsonPath --input $conflict --steam-id $steam --output (Join-Path $temporary 'conflicted')
    Assert ($LASTEXITCODE -ne 0) 'Conflicting same-checksum exports must fail'
    Assert (-not (Test-Path -LiteralPath (Join-Path $temporary 'conflicted'))) 'No completed report after failure'
    $broken = Join-Path $temporary 'broken.json'
    '{}' | Set-Content -LiteralPath $broken
    & $AnalyzerPath --input $broken --steam-id $steam --output (Join-Path $temporary 'broken-output')
    Assert ($LASTEXITCODE -ne 0) 'Invalid schema must fail'
    $outRoot = Join-Path $temporary 'wrapper'
    $cache = Join-Path $temporary 'cache'
    $run = & (Join-Path $root 'Invoke-CS2Analysis.ps1') -InputPath $jsonPath -SteamId $steam `
        -OutDir $outRoot -CacheDir $cache -AnalyzerPath $AnalyzerPath -SuspectName 'name & unicode-é' -Elo 3000 -Platform Faceit
    Assert (Test-Path -LiteralPath (Join-Path $run 'analysis.json')) 'JSON launcher output'
    $demo = Join-Path $temporary 'fake demo & unicode-é.dem'
    Copy-Item -LiteralPath $jsonPath -Destination $demo
    $run = & (Join-Path $root 'Invoke-CS2Analysis.ps1') -InputPath $demo -SteamId $steam `
        -OutDir $outRoot -CacheDir $cache -AnalyzerPath $AnalyzerPath -CsdaPath $DecoderPath -Platform Faceit
    $run2 = & (Join-Path $root 'Invoke-CS2Analysis.ps1') -InputPath $demo -SteamId $steam `
        -OutDir $outRoot -CacheDir $cache -AnalyzerPath $AnalyzerPath -CsdaPath $DecoderPath -Platform Faceit
    $manifest2 = Get-Content -LiteralPath (Join-Path $run2 'manifest.json') -Raw | ConvertFrom-Json
    Assert $manifest2.inputs[0].decoder_provenance.cache_reused 'Hash-bound decoder cache reuse'
    Assert ($manifest2.inputs[0].decoder_provenance.decoder_version -eq 'custom_binary') 'Do not claim fake decoder is CSDA'
    '{"test_exit":7}' | Set-Content -LiteralPath $demo
    Reject { & (Join-Path $root 'Invoke-CS2Analysis.ps1') -InputPath $demo -SteamId $steam `
        -OutDir $outRoot -CacheDir $cache -AnalyzerPath $AnalyzerPath -CsdaPath $DecoderPath } 'Decoder failure must propagate'
    '{"test_sleep_ms":10000}' | Set-Content -LiteralPath $demo
    Reject { & (Join-Path $root 'Invoke-CS2Analysis.ps1') -InputPath $demo -SteamId $steam `
        -OutDir $outRoot -CacheDir $cache -AnalyzerPath $AnalyzerPath -CsdaPath $DecoderPath -TimeoutSeconds 1 } 'Timeout must fail'
    Write-Host 'CLI, offline report, launcher, cache, decoder failure and timeout checks passed.'
}
finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force
}
