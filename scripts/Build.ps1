#requires -Version 7.0
[CmdletBinding()]
param([switch]$Test)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
& cmake -S $root -B (Join-Path $root 'build') -A x64 -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed: $LASTEXITCODE" }
& cmake --build (Join-Path $root 'build') --config Release
if ($LASTEXITCODE -ne 0) { throw "C++ build failed: $LASTEXITCODE" }
if ($Test) {
    & ctest --test-dir (Join-Path $root 'build') -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Tests failed: $LASTEXITCODE" }
}
