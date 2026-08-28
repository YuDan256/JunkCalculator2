# ============================================================================
# JC2 Formatter correctness verification script
#
# Usage:
#   .\verify_fmt.ps1 <file.jc2>      verify a single file
#   .\verify_fmt.ps1 <dir>           verify all .jc2 under a directory
#
# Three checks (hard correctness guarantees of the formatter):
#   1. Semantic equivalence - bytecode (jc2 -c -s, strip debug) MD5 is identical
#      before and after formatting (no code loss, no behavior change).
#   2. Idempotence - after formatting, `jc2 fmt --check` reports no diff.
#   3. Compilable - the formatted code still passes `-c` (syntax intact).
#
# The script formats each file but restores it afterwards (no source changes).
# ============================================================================

param(
    [Parameter(Mandatory = $true)][string]$Target
)

$ErrorActionPreference = 'Continue'

# --- locate jc2.exe ---
$exe = Join-Path $PSScriptRoot "out\build\x64-release\jc2.exe"
if (-not (Test-Path $exe)) {
    $exe = Join-Path $PSScriptRoot "out\build\x64-release\JunkCalculator2.exe"
}
if (-not (Test-Path $exe)) {
    Write-Error "jc2.exe not found. Build it under out\build\x64-release first."
    exit 1
}

# --- collect .jc2 files ---
$files = @()
if (Test-Path $Target -PathType Container) {
    $files = @(Get-ChildItem -Path $Target -Recurse -Filter "*.jc2" | Select-Object -ExpandProperty FullName)
} elseif (Test-Path $Target -PathType Leaf) {
    $files = @($Target)
} else {
    Write-Error "Path not found: $Target"
    exit 1
}

Write-Host ""
Write-Host "=== JC2 Formatter correctness verification ===" -ForegroundColor Cyan
Write-Host "Target: $Target   ($($files.Count) .jc2 file(s))" -ForegroundColor Cyan
Write-Host ""

$pass = 0
$fail = 0
$skip = 0
$work = Join-Path $env:TEMP ("fmt_verify_" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null

try {
    $i = 0
    foreach ($file in $files) {
        $i++
        $name = Split-Path $file -Leaf
        $original = [System.IO.File]::ReadAllText($file)
        $jcbPath = [System.IO.Path]::ChangeExtension($file, '.jcb')
        $tmpA = Join-Path $work "a_$i.jcb"
        $tmpB = Join-Path $work "b_$i.jcb"

        # 1. compile original -> bytecode A
        Remove-Item $jcbPath -ErrorAction SilentlyContinue
        & $exe -c $file -s 2>$null | Out-Null
        $compileA = ($LASTEXITCODE -eq 0) -and (Test-Path $jcbPath)
        if ($compileA) { Copy-Item $jcbPath $tmpA -Force }

        # 2. format in place
        & $exe fmt $file 2>$null | Out-Null

        # 2.5 original does not compile -> SKIP (not a formatter issue)
        if (-not $compileA) {
            [System.IO.File]::WriteAllText($file, $original)
            Write-Host ("[SKIP]  {0}   (original does not compile)" -f $name) -ForegroundColor Yellow
            $skip++
            continue
        }

        # 3. compile formatted -> bytecode B
        $compileB = $false
        if ($compileA) {
            Remove-Item $jcbPath -ErrorAction SilentlyContinue
            & $exe -c $file -s 2>$null | Out-Null
            $compileB = ($LASTEXITCODE -eq 0) -and (Test-Path $jcbPath)
            if ($compileB) { Copy-Item $jcbPath $tmpB -Force }
        }

        # 4. idempotence
        & $exe fmt $file --check 2>$null | Out-Null
        $idempotent = ($LASTEXITCODE -eq 0)

        # 5. verdict
        $reasons = @()
        if ($compileA -and $compileB) {
            $ha = (Get-FileHash $tmpA -Algorithm MD5).Hash
            $hb = (Get-FileHash $tmpB -Algorithm MD5).Hash
            if ($ha -ne $hb) { $reasons += "bytecode mismatch" }
        } else {
            $reasons += "compile failed"
        }
        if (-not $idempotent) { $reasons += "not idempotent" }

        if ($reasons.Count -eq 0) {
            Write-Host ("[PASS]  {0}" -f $name) -ForegroundColor Green
            $pass++
        } else {
            $reasonStr = $reasons -join ", "
            Write-Host ("[FAIL]  {0}   ({1})" -f $name, $reasonStr) -ForegroundColor Red
            $fail++
        }

        # 6. restore original + clean bytecode
        [System.IO.File]::WriteAllText($file, $original)
        Remove-Item $jcbPath -ErrorAction SilentlyContinue
    }
} finally {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host ("=== Result:  PASS {0} / FAIL {1} / SKIP {2} ===" -f $pass, $fail, $skip) -ForegroundColor Cyan
Write-Host ""
if ($fail -gt 0) { exit 1 } else { exit 0 }
