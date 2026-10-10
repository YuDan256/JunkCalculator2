param(
    [string]$ExePath = '',
    [string]$TestsDir = '',
    [string]$ModulesDir = ''
)

# Requires PowerShell 7+ for reliable native stdin/stdout handling.
if ($PSVersionTable.PSVersion.Major -lt 7) {
    if (Get-Command pwsh -ErrorAction SilentlyContinue) {
        & pwsh -NoProfile -File $PSCommandPath -ExePath $ExePath -TestsDir $TestsDir -ModulesDir $ModulesDir
        exit $LASTEXITCODE
    }
    Write-Warning 'PowerShell 7 (pwsh) not found; REPL piping and stderr capture may be unreliable.'
}

$ErrorActionPreference = 'Continue'

$Root = Split-Path -Parent $PSScriptRoot
$Exe  = if ($ExePath)  { $ExePath }  else { Join-Path $Root 'bin\JunkCalculator2.exe' }
if (-not $TestsDir)   { $TestsDir   = Join-Path $Root 'bin\tests' }
if (-not $ModulesDir) { $ModulesDir = Join-Path $Root 'bin\modules' }
$Results = Join-Path $PSScriptRoot 'results'

$TempRoot = Join-Path $env:TEMP ('jc2-jit-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $TempRoot -Force | Out-Null
New-Item -ItemType Directory -Path $Results -Force | Out-Null

Write-Host "Exe: $Exe"
Write-Host "Temp: $TempRoot"

function Invoke-JC2Once {
    param([string[]]$RunArgs, [string]$WorkDir)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Push-Location $WorkDir
    try {
        $output = & $Exe @RunArgs 2>&1
        $code = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    $sw.Stop()
    [pscustomobject]@{
        Output   = ($output -join "`n")
        ExitCode = $code
        Ms       = $sw.Elapsed.TotalMilliseconds
    }
}

function Get-Median {
    param([double[]]$Values)
    $s = $Values | Sort-Object
    $n = $s.Count
    if ($n -eq 0) { return 0.0 }
    $mid = [int][math]::Floor($n / 2)
    if ($n % 2 -eq 1) { return $s[$mid] }
    return ($s[$mid - 1] + $s[$mid]) / 2.0
}

$oracleRows   = New-Object System.Collections.Generic.List[object]
$benchRows    = New-Object System.Collections.Generic.List[object]
$edgeRows     = New-Object System.Collections.Generic.List[object]

# ---------- 1. Oracle: existing test suite (exclude GUI window test) ----------
Write-Host "`n=== 1/5 Oracle tests ==="
$testFiles = Get-ChildItem -Path $TestsDir -Recurse -Filter '*.jc2' |
    Where-Object {
        $_.Name -ne 'test_window.jc2' -and
        $_.FullName -notmatch '\\jit-tests\\'
    } |
    Sort-Object FullName
$tempTests = Join-Path $TempRoot 'tests'
$tempModules = Join-Path $TempRoot 'modules'
Copy-Item $TestsDir $tempTests -Recurse -Force
Copy-Item $ModulesDir $tempModules -Recurse -Force

$oracleFail = 0
foreach ($t in $testFiles) {
    $rel = $t.FullName.Substring($TestsDir.Length + 1)
    $tempCopy = Join-Path $tempTests $rel
    $r0 = Invoke-JC2Once @($tempCopy) $tempTests
    $r1 = Invoke-JC2Once @($tempCopy, '--jit') $tempTests
    $same = ($r0.ExitCode -eq $r1.ExitCode) -and ($r0.Output -eq $r1.Output)
    if (-not $same) { $oracleFail++ }
    $oracleRows.Add([pscustomobject]@{
        Test     = $rel
        ExitInt  = $r0.ExitCode
        ExitJit  = $r1.ExitCode
        StdoutEq = ($r0.Output -eq $r1.Output)
        Pass     = $same
        MsInt    = [math]::Round($r0.Ms, 1)
        MsJit    = [math]::Round($r1.Ms, 1)
    })
    Write-Host ("{0,-45} pass={1,-5} int={2}ms jit={3}ms" -f $rel, $same, [math]::Round($r0.Ms,1), [math]::Round($r1.Ms,1))
}

# ---------- 2. Benchmarks ----------
Write-Host "`n=== 2/5 Benchmarks (3 runs, median) ==="
$benchFiles = Get-ChildItem -Path $PSScriptRoot -Filter 'bench*.jc2' | Sort-Object Name
foreach ($b in $benchFiles) {
    $intTimes = @()
    $jitTimes = @()
    $intOut = $null; $jitOut = $null
    $intCode = -1; $jitCode = -1
    for ($k = 0; $k -lt 3; $k++) {
        $ri = Invoke-JC2Once @($b.FullName) $TempRoot
        $rj = Invoke-JC2Once @($b.FullName, '--jit') $TempRoot
        $intTimes += $ri.Ms; $jitTimes += $rj.Ms
        $intOut = $ri.Output; $jitOut = $rj.Output
        $intCode = $ri.ExitCode; $jitCode = $rj.ExitCode
        Write-Host ("{0} round {1}: int={2}ms jit={3}ms" -f $b.Name, $k, [math]::Round($ri.Ms,1), [math]::Round($rj.Ms,1))
    }
    $mi = Get-Median $intTimes
    $mj = Get-Median $jitTimes
    $speedup = if ($mj -gt 0) { $mi / $mj } else { 0.0 }
    $same = ($intCode -eq $jitCode) -and ($intOut -eq $jitOut)
    $benchRows.Add([pscustomobject]@{
        Bench      = $b.Name
        IntMedianMs = [math]::Round($mi, 1)
        JitMedianMs = [math]::Round($mj, 1)
        SpeedupX   = [math]::Round($speedup, 2)
        Correct    = $same
        ExitInt    = $intCode
        ExitJit    = $jitCode
    })
    Write-Host ("{0}: int={1}ms jit={2}ms speedup={3}x correct={4}" -f $b.Name, [math]::Round($mi,1), [math]::Round($mj,1), [math]::Round($speedup,2), $same)
}

# ---------- 3. Edge scenarios ----------
Write-Host "`n=== 3/5 Edge scenarios ==="
$edgeFiles = Get-ChildItem -Path $PSScriptRoot -Filter 'edge*.jc2' | Where-Object { $_.Name -ne 'edge08_repl_target.jc2' } | Sort-Object Name
$edgeFail = 0
foreach ($e in $edgeFiles) {
    $r0 = Invoke-JC2Once @($e.FullName) $TempRoot
    $r1 = Invoke-JC2Once @($e.FullName, '--jit') $TempRoot
    $same = ($r0.ExitCode -eq $r1.ExitCode) -and ($r0.Output -eq $r1.Output)
    if (-not $same) { $edgeFail++ }
    $edgeRows.Add([pscustomobject]@{
        Edge     = $e.Name
        ExitInt  = $r0.ExitCode
        ExitJit  = $r1.ExitCode
        StdoutEq = ($r0.Output -eq $r1.Output)
        Pass     = $same
    })
    Write-Host ("{0,-32} pass={1}" -f $e.Name, $same)
}

# ---------- 4. Toolchain & introspection ----------
Write-Host "`n=== 4/5 Toolchain & introspection ==="
$toolSamples = Join-Path $Results 'samples'
New-Item -ItemType Directory -Path $toolSamples -Force | Out-Null

$target = Join-Path $PSScriptRoot 'edge08_repl_target.jc2'
$hir = Invoke-JC2Once @($target, '--jit', '--hir') $TempRoot
$hirOk = ($hir.Output -match 'digraph HIR') -and
         ($hir.Output -match 'OSR HIR Graph \(Unoptimized\)') -and
         ($hir.Output -match 'OSR HIR Graph \(Optimized\)') -and
         ($hir.Output -match 'OSREntry') -and
         ($hir.Output -match 'Guard') -and
         ($hir.Output -match 'Phi') -and
         ($hir.Output -match 'FrameState')
$hir.Output | Out-File -FilePath (Join-Path $toolSamples 'tool_hir.txt') -Encoding utf8
Write-Host ("--hir: ok={0} nodes={1}" -f $hirOk, ([regex]::Matches($hir.Output, 'node\d+ \[label').Count))

$mc = Invoke-JC2Once @($target, '--jit', '--mc') $TempRoot
$mcOk = ($mc.Output -match 'OSR Machine Code') -and ($mc.Output -match 'push rbp') -and ($mc.Output -match 'Size:')
$mc.Output | Out-File -FilePath (Join-Path $toolSamples 'tool_mc.txt') -Encoding utf8
Write-Host ("--mc: ok={0} bytes-header-present={1}" -f $mcOk, ($mc.Output -match 'Size:'))

$prof = Invoke-JC2Once @($target, '--jit', '--profile') $TempRoot
$profOk = ($prof.Output -match 'Profiler Results')
$prof.Output | Out-File -FilePath (Join-Path $toolSamples 'tool_profile.txt') -Encoding utf8
Write-Host ("--profile: ok={0}" -f $profOk)

# REPL toggle session
$replInput = @(
    '/jit on'
    '/hir on'
    '/mc on'
    'f(n) = { s=0; i=0; while (i < n) { s += i; i += 1 }; s }'
    'r = f(2000000)'
    '/jit off'
    '/exit'
) -join "`n"
$replInput += "`n"
Push-Location $TempRoot
$replOut = $replInput | & $Exe -q 2>&1
Pop-Location
$replText = ($replOut -join "`n")
$replOk = ($replText -match 'JIT Compilation enabled\.') -and
          ($replText -match 'OSR HIR Graph') -and
          ($replText -match 'OSR Machine Code') -and
          ($replText -match 'JIT Compilation disabled\.')
$replText | Out-File -FilePath (Join-Path $toolSamples 'tool_repl.txt') -Encoding utf8
Write-Host ("REPL /jit /hir /mc toggles: ok={0}" -f $replOk)

# -e vs --eval alias
$e1 = Invoke-JC2Once @('-e', '1+2*3', '--jit') $TempRoot
$e2 = Invoke-JC2Once @('--eval', '1+2*3', '--jit') $TempRoot
$evalOk = ($e1.Output -eq $e2.Output) -and ($e1.ExitCode -eq $e2.ExitCode)
Write-Host ("-e vs --eval: ok={0} out='{1}'" -f $evalOk, ($e1.Output -replace "`n", ' '))

# Debug introspection probes
$probeCmds = @(
    @{ Name = 'p1_nojit_basic';       Args = @('-e', 'f(x)=x*x; f(3); __dbg_is_jitted(f)') },
    @{ Name = 'p2_jit_hotloop';       Args = @('-e', 'f(n) = { s=0; i=0; while (i<n) { s += i; i += 1 }; s }; f(5000000); __dbg_is_jitted(f)', '--jit') },
    @{ Name = 'p3_jit_hotfunc_calls'; Args = @('-e', 'f(x)=x*x; g(n) = { s=0; i=0; while (i<n) { s += f(i); i += 1 }; s }; g(2000000); __dbg_is_jitted(f)', '--jit') },
    @{ Name = 'p4_typefb_hotloop';    Args = @('-e', 'f(n) = { s=0; i=0; while (i<n) { s += i; i += 1 }; s }; f(5000000); __dbg_type_feedback(f)', '--jit') },
    @{ Name = 'p5_typefb_hotfunc';    Args = @('-e', 'f(x)=x*x; g(n) = { s=0; i=0; while (i<n) { s += f(i); i += 1 }; s }; g(2000000); __dbg_type_feedback(f)', '--jit') },
    @{ Name = 'p6_typefb_poly';       Args = @('-e', 'f(x)=x+1; g2(x)=x*2; h(n) = { s=0; i=0; while (i<n) { fn = if (i % 2 == 0) { f } else { g2 }; s += fn(i); i += 1 }; s }; h(1000000); __dbg_type_feedback(f)', '--jit') }
)
$probeLines = New-Object System.Collections.Generic.List[string]
foreach ($p in $probeCmds) {
    $r = Invoke-JC2Once $p.Args $TempRoot
    $probeLines.Add(('### {0} | exit={1}' -f $p.Name, $r.ExitCode))
    $probeLines.Add($r.Output)
    Write-Host ("probe {0}: exit={1} out='{2}'" -f $p.Name, $r.ExitCode, ($r.Output -replace "`n", ' '))
}
$probeLines | Out-File -FilePath (Join-Path $toolSamples 'probes.txt') -Encoding utf8

# JIT runtime log capture (script mode, no /hir /mc)
$logRun = Invoke-JC2Once @($target, '--jit') $TempRoot
$jitLogLines = ($logRun.Output -split "`n") | Where-Object { $_ -match '\[JIT\]|OSR|Deopt|Tier 2' }
$jitLogLines | Out-File -FilePath (Join-Path $toolSamples 'jit_logs.txt') -Encoding utf8
Write-Host ("[JIT] log lines in plain script mode: {0}" -f $jitLogLines.Count)

# ---------- 5. Persist results ----------
$oracleRows | Export-Csv -Path (Join-Path $Results 'oracle_results.csv') -NoTypeInformation -Encoding utf8
$benchRows  | Export-Csv -Path (Join-Path $Results 'bench_results.csv')  -NoTypeInformation -Encoding utf8
$edgeRows   | Export-Csv -Path (Join-Path $Results 'edge_results.csv')   -NoTypeInformation -Encoding utf8

Write-Host "`n=== Summary ==="
Write-Host ("Oracle: {0}/{1} passed, {2} failed" -f ($oracleRows.Count - $oracleFail), $oracleRows.Count, $oracleFail)
Write-Host ("Bench: {0} benchmarks, {1} correctness mismatches" -f $benchRows.Count, @($benchRows | Where-Object { -not $_.Correct }).Count)
Write-Host ("Edge: {0}/{1} passed, {2} failed" -f ($edgeRows.Count - $edgeFail), $edgeRows.Count, $edgeFail)
Write-Host ("HIR ok={0}, MC ok={1}, Profile ok={2}, REPL ok={3}, eval alias ok={4}" -f $hirOk, $mcOk, $profOk, $replOk, $evalOk)
Write-Host "Results saved under: $Results"
