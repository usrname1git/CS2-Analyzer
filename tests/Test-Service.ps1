#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DemoPath,
    [string]$BaseUri = 'https://csda.autom8ed.me'
)
$ErrorActionPreference = 'Stop'
function Get-Sha256Utf8([string]$Text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))).Replace('-', '').ToLowerInvariant()
    }
    finally { $sha.Dispose() }
}
$root = Split-Path -Parent $PSScriptRoot
if ($BaseUri -ne 'https://csda.autom8ed.me') { throw 'Unexpected qualification endpoint.' }
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class CsdaTestCredential {
    [StructLayout(LayoutKind.Sequential)] struct Credential {
        public uint Flags, Type;
        public IntPtr Target, Comment;
        public long Written;
        public uint Size;
        public IntPtr Blob;
        public uint Persist, Count;
        public IntPtr Attributes, Alias, User;
    }
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool CredRead(string target, uint type, uint flags, out IntPtr value);
    [DllImport("advapi32.dll")] static extern void CredFree(IntPtr value);
    public static string Read(string target) {
        IntPtr pointer;
        if (!CredRead(target, 1, 0, out pointer)) throw new InvalidOperationException("Beta credential unavailable");
        try {
            var value = Marshal.PtrToStructure<Credential>(pointer);
            if (value.Size != 64) throw new InvalidOperationException("Invalid beta credential size");
            var bytes = new byte[value.Size];
            Marshal.Copy(value.Blob, bytes, 0, bytes.Length);
            try { return System.Text.Encoding.UTF8.GetString(bytes); }
            finally { Array.Clear(bytes, 0, bytes.Length); }
        } finally { CredFree(pointer); }
    }
}
'@
$hash = Get-Sha256Utf8 $root
$secret = [CsdaTestCredential]::Read("Local\CSDA.$($hash.Substring(0,24)).BetaAccess")
$handler = [Net.Http.HttpClientHandler]::new()
$handler.UseProxy = $false
$handler.AllowAutoRedirect = $false
$client = [Net.Http.HttpClient]::new($handler)
$client.Timeout = [TimeSpan]::FromSeconds(185)
$csrf = ''
function Assert($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Request([string]$Method, [string]$Path, $Body = $null, [string]$Origin = 'https://csda.autom8ed.me') {
    $request = [Net.Http.HttpRequestMessage]::new([Net.Http.HttpMethod]::new($Method), "$BaseUri$Path")
    $null = $request.Headers.TryAddWithoutValidation('Origin', $Origin)
    if ($csrf) { $null = $request.Headers.TryAddWithoutValidation('X-CSDA-CSRF', $csrf) }
    $request.Headers.ExpectContinue = $false
    if ($Body -is [IO.FileInfo]) {
        $request.Content = [Net.Http.StreamContent]::new([IO.File]::OpenRead($Body.FullName))
        $request.Content.Headers.ContentType = [Net.Http.Headers.MediaTypeHeaderValue]::new('application/octet-stream')
    } elseif ($null -ne $Body) {
        $request.Content = [Net.Http.StringContent]::new(($Body | ConvertTo-Json -Compress), [Text.Encoding]::UTF8, 'application/json')
    }
    $response = $null
    try {
        $response = $client.SendAsync($request).GetAwaiter().GetResult()
        return [pscustomobject]@{ Status = [int]$response.StatusCode
            Text = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult() }
    } finally { if ($response) { $response.Dispose() }; $request.Dispose() }
}
$id = ''
try {
    Assert ((Request GET '/').Status -eq 200) 'The HTTPS page is unavailable.'
    Assert ((Request POST '/api/jobs' @{}).Status -eq 401) 'Anonymous job creation was accepted.'
    Assert ((Request POST '/api/session' @{invite=$secret} 'https://invalid.example').Status -eq 403) 'Foreign-origin access was accepted.'
    $login = Request POST '/api/session' @{invite=$secret}
    $secret = $null
    Assert ($login.Status -eq 200) 'TLS beta login failed.'
    $csrf = ($login.Text | ConvertFrom-Json).csrf
    $created = Request POST '/api/jobs' @{}
    Assert ($created.Status -eq 201) 'TLS job creation failed.'
    $id = ($created.Text | ConvertFrom-Json).id
    $uploaded = Request PUT "/api/jobs/$id/demo" (Get-Item -LiteralPath $DemoPath)
    Assert ($uploaded.Status -eq 200) 'TLS streaming upload failed.'
    function Wait-State([string]$State) {
        for ($attempt=0; $attempt -lt 300; $attempt++) {
            $response = Request GET "/api/jobs/$id"
            Assert ($response.Status -eq 200) 'The private job became unavailable.'
            $job = $response.Text | ConvertFrom-Json
            if ($job.state -eq $State) { return $job }
            Assert ($job.state -notin 'failed','cleanup_pending') 'The worker failed; inspect private errors.log.'
            Start-Sleep -Milliseconds 500
        }
        throw 'TLS job timed out.'
    }
    $roster = Wait-State select
    Assert (@($roster.players).Count -eq 10) 'The public reference roster did not contain ten players.'
    Assert ((Request POST "/api/jobs/$id/analysis" @{steam_id='76561198057057588'}).Status -eq 202) 'TLS player selection failed.'
    $complete = Wait-State complete
    $result = Request GET "/api/jobs/$id/result"
    Assert ($result.Status -eq 200) 'The real result is unavailable.'
    $analysis = $result.Text | ConvertFrom-Json
    Assert ($analysis.summary.kills -eq 12 -and $analysis.summary.deaths -eq 14 -and
        $analysis.summary.rounds -eq 18 -and @($analysis.warnings).Count -eq 0) 'Public-fixture result differs from the native reference.'
    Assert (-not (Test-Path -LiteralPath (Join-Path $root "local\jobs\$id"))) 'Raw input survived publication.'
    $report = Request GET "/api/jobs/$id/report"
    $reportHash = Get-Sha256Utf8 $report.Text
    Assert ($report.Status -eq 200 -and $reportHash -eq $complete.artifacts.'report.html'.sha256) 'TLS report download changed bytes.'
    $receipt = [ordered]@{status='pass';at=[datetime]::UtcNow.ToString('o');endpoint=$BaseUri;
        route='public hostname from the hosting PC; not off-host upload proof';job=$complete;
        public_fixture_sha256=(Get-FileHash -LiteralPath $DemoPath).Hash;raw_removed=$true}
    $receipt | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $root 'local\public-route-qualification.json') -Encoding utf8NoBOM
    [pscustomobject]@{Status='pass';Endpoint=$BaseUri;Job=$id;Rounds=18;Kills=12;Deaths=14;
        InputBytes=$complete.upload_bytes;UploadMs=$complete.upload_ms;Phases=$complete.phases;RawRemoved=$true}|ConvertTo-Json -Depth 8
} finally {
    if ($id) {
        try { $null = Request DELETE "/api/jobs/$id" }
        catch { Write-Warning 'Qualification job deletion request failed; normal result expiry still applies.' }
    }
    $secret = $null
    $client.Dispose()
    $handler.Dispose()
}
