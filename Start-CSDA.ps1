#requires -Version 7.0
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$exe = Join-Path $root 'build\Release\csda-service.exe'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if ([Security.Principal.WindowsPrincipal]::new($identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    & 'C:\Tools\TeamM2\wsudo.exe' -U -w 'C:\pwsh\pwsh.exe' -NoLogo -NoProfile -File $PSCommandPath
    if ($LASTEXITCODE) { throw 'Normal-user CSDA start failed.' }
    return
}
$listeners = @(Get-NetTCPConnection -State Listen -LocalPort 8091 -ErrorAction SilentlyContinue)
if ($listeners.Count) {
    if ($listeners.Count -ne 1 -or $listeners[0].LocalAddress -ne '127.0.0.1' -or
        (Get-Process -Id $listeners[0].OwningProcess).Path -ine $exe) {
        throw 'Port 8091 has an unexpected owner; nothing was stopped.'
    }
    $health = Invoke-RestMethod -Uri 'http://127.0.0.1:8091/healthz' -TimeoutSec 5
    if (-not $health.ready -or $health.role -ne 'csda-service-v1' -or $health.pid -ne $listeners[0].OwningProcess) {
        throw 'The existing CSDA backend is unready.'
    }
    Write-Output 'CSDA is already running: https://csda.autom8ed.me'
    return
}
$start = [Diagnostics.ProcessStartInfo]::new($exe)
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.WorkingDirectory = $root
$start.ArgumentList.Add('serve')
$start.ArgumentList.Add('--root')
$start.ArgumentList.Add($root)
$start.Environment.Clear()
foreach ($key in 'SystemRoot','WINDIR','SYSTEMDRIVE','USERPROFILE','APPDATA','LOCALAPPDATA') {
    $value = [Environment]::GetEnvironmentVariable($key)
    if (-not $value) { throw "Required Windows environment missing: $key" }
    $start.Environment[$key] = $value
}
$start.Environment['PATH'] = Join-Path $env:SystemRoot 'System32'
$start.Environment['TEMP'] = $env:TEMP
$start.Environment['TMP'] = $env:TEMP
$process = [Diagnostics.Process]::Start($start)
try {
    $deadline = [datetime]::UtcNow.AddSeconds(25)
    do {
        if ($process.HasExited) { throw 'CSDA exited during startup; inspect local\service-native.log.' }
        Start-Sleep -Milliseconds 250
        try {
            $health = Invoke-RestMethod -Uri 'http://127.0.0.1:8091/healthz' -TimeoutSec 1
            if ($health.ready -and $health.pid -eq $process.Id -and $health.role -eq 'csda-service-v1') {
                Write-Output 'CSDA started: https://csda.autom8ed.me'
                return
            }
        } catch [Net.Http.HttpRequestException], [Threading.Tasks.TaskCanceledException] {
            # Startup runs the real containment gate before opening the API.
        }
    } while ([datetime]::UtcNow -lt $deadline)
    if (-not $process.HasExited) { $process.Kill($true); $null = $process.WaitForExit(10000) }
    throw 'CSDA startup timed out; its created process tree was stopped.'
} finally { $process.Dispose() }
