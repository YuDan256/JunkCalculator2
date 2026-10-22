# tools/build.ps1 — build the configured JC2 tree
# Usage: pwsh tools/build.ps1 [-Config x64-release] [-Verbose]
# Keeps build orchestration outside jc2.exe so linking cannot self-lock.

param(
    [string]$Config = "x64-release",
    [switch]$Verbose
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "out\build\$Config"
if (-not (Test-Path $buildDir)) {
    throw "Build directory not found: $buildDir (configure it first)."
}

# A running REPL/LSP holds jc2.exe and blocks the linker.
Get-Process jc2 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot = $null
if (Test-Path $vswhere) {
    $vsRoot = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null | Select-Object -First 1
}
if (-not $vsRoot) { $vsRoot = "D:\Program Files\Microsoft Visual Studio\2022\Community" }
$vcvars = Join-Path $vsRoot "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found under: $vsRoot"
}

$sw = [Diagnostics.Stopwatch]::StartNew()
$lines = & cmd.exe /d /c "call `"$vcvars`" >nul 2>&1 && cmake --build `"$buildDir`" 2>&1"
$code = $LASTEXITCODE
$sw.Stop()

$interesting = $lines | Where-Object {
    $_ -match 'error C\d+|error LNK|fatal error|warning C\d+|ninja: error|FAILED:'
}

if ($Verbose -or $code -ne 0) {
    $lines | ForEach-Object { $_ }
} else {
    $interesting | ForEach-Object { $_ }
}

if ($code -eq 0) {
    Write-Host ("Build OK ({0:N1}s)" -f $sw.Elapsed.TotalSeconds) -ForegroundColor Green
} else {
    Write-Host ("Build FAILED (exit {0})" -f $code) -ForegroundColor Red
    exit $code
}
