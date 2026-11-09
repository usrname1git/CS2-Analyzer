#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DemoPath,
    [string]$ReferenceJson,
    [string]$AnalyzerPath = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\cs2-analyzer.exe')
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$validation = Join-Path $root ('results\public-validation-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$null = New-Item -ItemType Directory -Path $validation
$decodeRun = & (Join-Path $root 'Invoke-CS2Analysis.ps1') -InputPath $DemoPath -SteamId '76561198057057588' `
    -OutDir $validation -AnalyzerPath $AnalyzerPath -Platform Esportal -SuspectName 'Public-fixture'
$manifest = Get-Content -LiteralPath (Join-Path $decodeRun 'manifest.json') -Raw | ConvertFrom-Json
$decodedPath = $manifest.inputs[0].path
$actual = Get-Content -LiteralPath $decodedPath -Raw | ConvertFrom-Json -AsHashtable
if ($actual.checksum -ne 'efaa41a26b6045ac') {
    throw 'This opt-in regression expects esportal_6045888_2024_mirage.dem from the public upstream fixture set.'
}
$reference = if ($ReferenceJson) {
    Get-Content -LiteralPath $ReferenceJson -Raw | ConvertFrom-Json -AsHashtable
} else { $null }
if ($reference) {
    foreach ($table in @('rounds', 'kills', 'playerEconomies')) {
        $left = ConvertTo-Json -InputObject @($actual[$table]) -Depth 20 -Compress
        $right = ConvertTo-Json -InputObject @($reference[$table]) -Depth 20 -Compress
        if ($left -ne $right) { throw "Public reference mismatch: $table" }
    }
    foreach ($table in @('damages', 'shots')) {
        $field = if ($table -eq 'damages') { 'weaponUniqueId' } else { 'weaponId' }
        $rows = if ($table -eq 'shots') {
            @($actual[$table] | Sort-Object { $_['tick'] }, { $_['playerSteamId'] },
                { $_['weaponName'] }, { $_['x'] }, { $_['y'] }, { $_['z'] })
        } else { @($actual[$table]) }
        $projectileAliases = @{}
        $normalized = @($rows | ForEach-Object {
            $row = @{}
            foreach ($key in $_.Keys) { $row[$key] = $_[$key] }
            $row[$field] = 'redacted'
            if ($table -eq 'shots' -and $row.projectileId -ne 0) {
                $identity = "$($row.projectileId)"
                if (-not $projectileAliases.ContainsKey($identity)) {
                    $projectileAliases[$identity] = $projectileAliases.Count + 1
                }
                $row.projectileId = $projectileAliases[$identity]
            }
            $row
        })
        $left = ConvertTo-Json -InputObject $normalized -Depth 20 -Compress
        $right = ConvertTo-Json -InputObject @($reference[$table]) -Depth 20 -Compress
        # Hashtable order is not a semantic JSON comparison.
        $leftValue = $left | ConvertFrom-Json -AsHashtable
        $rightValue = $right | ConvertFrom-Json -AsHashtable
        if ($leftValue.Count -ne $rightValue.Count) { throw "Reference event count mismatch: $table" }
        for ($index = 0; $index -lt $leftValue.Count; $index++) {
            foreach ($key in $rightValue[$index].Keys) {
                if ($leftValue[$index][$key] -cne $rightValue[$index][$key]) {
                    throw "Public reference mismatch: $table row $index field $key"
                }
            }
        }
    }
}
$checked = @()
foreach ($steam in $actual.players.Keys) {
    $out = Join-Path $validation "player-$steam"
    & $AnalyzerPath --input $decodedPath --steam-id $steam --output $out --name 'Public fixture'
    if ($LASTEXITCODE -ne 0) { throw "Player import failed: $steam" }
    $report = Get-Content -LiteralPath (Join-Path $out 'analysis.json') -Raw | ConvertFrom-Json
    if (@($report.warnings).Count -ne 0) { throw "Unexpected player reconciliation warning: $steam" }
    $player = $actual.players[$steam]
    foreach ($pair in @(
        @('kills', 'killCount'), @('deaths', 'deathCount'), @('assists', 'assistCount'),
        @('headshots', 'headshotCount'), @('damage', 'healthDamage'),
        @('opening_kills', 'firstKillCount'), @('opening_deaths', 'firstDeathCount')
    )) {
        if ($report.summary.($pair[0]) -ne $player[$pair[1]]) {
            throw "Scoreboard mismatch: $steam $($pair[0])"
        }
        if ($reference -and $player[$pair[1]] -ne $reference.players[$steam][$pair[1]]) {
            throw "Golden scoreboard mismatch: $steam $($pair[1])"
        }
    }
    if ($report.summary.rounds -ne $actual.rounds.Count) { throw 'Full-match participation denominator mismatch.' }
    $checked += $steam
}
@{
    status='pass'
    decoder_version=$manifest.inputs[0].decoder_provenance.decoder_version
    demo_sha256=$manifest.inputs[0].decoder_provenance.demo_sha256
    checked_players=$checked.Count
    live_rounds=$actual.rounds.Count
    kills=$actual.kills.Count
    damage_events=$actual.damages.Count
    shots=$actual.shots.Count
    reference_checked=[bool]$reference
    reference_normalization='Upstream weapon ID redaction, shot sorting and first-appearance projectile aliases; player color is not a compared combat statistic.'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $validation 'verification.json') -Encoding utf8NoBOM
Write-Host "Public demo regression passed: $($checked.Count) players reconciled. Receipt: $validation\verification.json"
