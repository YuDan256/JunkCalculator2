#Requires -Version 7.0
<#
.SYNOPSIS
    Generates the deterministic X.509 certificate test fixtures used by the JC2
    certificate chain validator tests (data\crypto_vectors\x509).

.DESCRIPTION
    Everything the generator needs lives in this file: the OpenSSL configuration
    is emitted next to the fixtures (openssl.cnf) and every certificate is issued
    with an explicit notBefore / notAfter.

    OpenSSL 3.2 has no -not_before / -not_after on `req -x509` (those appeared in
    3.4), therefore `openssl ca -startdate / -enddate` (YYMMDDHHMMSSZ) is used for
    every single certificate, including the self-signed roots (`-selfsign`).

    NOTE on OpenSSL 3.2.1: putting `rand_serial` into a CA section of the config
    file makes `openssl ca` ignore the serial file and emit a random serial.  That
    key is therefore deliberately absent and the per-CA serial file is seeded with
    a fixed value, which keeps serial numbers deterministic.

    Private keys are cached in keys\ and only regenerated with -ForceKeys, so the
    DER bytes are stable across runs.  ECDSA signature encodings are randomised by
    OpenSSL, so an EC certificate can differ by a couple of bytes when the script
    is re-run; manifest.md is always rewritten from the bytes actually on disk.

.PARAMETER OpenSSL
    Path to the openssl executable.

.PARAMETER ForceKeys
    Regenerate the private keys in keys\ even if they already exist.

.PARAMETER Check
    Do not generate anything.  Re-read manifest.md, compare every recorded byte
    count with the file on disk and re-run the core `openssl verify` commands.

.NOTES
    Only files below this directory are created or modified.
#>
[CmdletBinding()]
param(
    [string] $OpenSSL = 'C:\Program Files\Git\usr\bin\openssl.exe',
    [switch] $ForceKeys,
    [switch] $Check
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

$Root       = $PSScriptRoot
$RootFwd    = $Root.Replace('\', '/')
$KeysDir    = Join-Path $Root 'keys'
$CsrDir     = Join-Path $Root 'csr'
$WorkDir    = Join-Path $Root 'work'
$PemDir     = Join-Path $WorkDir 'pem'
$CaRoot     = Join-Path $WorkDir 'ca'
$ConfigPath = Join-Path $Root 'openssl.cnf'
$Manifest   = Join-Path $Root 'manifest.md'

# ---------------------------------------------------------------------------
# Fixed verification instant: 2024-06-01T00:00:00Z.
# Used with `openssl verify -attime` so no result depends on the wall clock.
# ---------------------------------------------------------------------------
$Attime     = [long] 1717200000
$AttimeText = '2024-06-01T00:00:00Z'

if (-not (Test-Path -LiteralPath $OpenSSL)) {
    throw "OpenSSL executable not found: $OpenSSL"
}

# ===========================================================================
# Fixture description tables
# ===========================================================================

# Ordered parent-first.  Kind drives the X.509 extension block (see Get-ExtLines).
# Issuer = $null means self-signed.
$Certs = @(
    # name                  key    CN                            SAN                                                  notBefore        notAfter         kind            issuer
    @{ Name='root_rsa';            Key='rsa'; Cn='JC2 Test RSA Root';            San='DNS:root-rsa.jc2.test';               NotBefore='200101000000Z'; NotAfter='350101000000Z'; Kind='root_ca';       Issuer=$null;            Role='root';         Expected='valid';   Reason='' }
    @{ Name='root_ec';             Key='ec';  Cn='JC2 Test EC Root';             San='DNS:root-ec.jc2.test';                NotBefore='200101000000Z'; NotAfter='350101000000Z'; Kind='root_ca';       Issuer=$null;            Role='root';         Expected='valid';   Reason='' }
    @{ Name='neg_pathlen_root';    Key='rsa'; Cn='JC2 Pathlen Root';             San='DNS:pathlen-root.jc2.test';           NotBefore='200101000000Z'; NotAfter='350101000000Z'; Kind='pathlen_root';  Issuer=$null;            Role='root';         Expected='invalid'; Reason='subordinate CA of a pathlen:0 root (path length constraint violated)' }
    @{ Name='neg_self_signed';     Key='rsa'; Cn='JC2 Untrusted Self-Signed';    San='DNS:selfsigned.example.com';          NotBefore='200101000000Z'; NotAfter='350101000000Z'; Kind='leaf';          Issuer=$null;            Role='leaf';         Expected='invalid'; Reason='self-signed certificate (CA:FALSE) that is not in the trust store' }
    @{ Name='inter_rsa';           Key='rsa'; Cn='JC2 Test RSA Intermediate';    San='DNS:inter-rsa.jc2.test';              NotBefore='200101000000Z'; NotAfter='350101000000Z'; Kind='inter_ca';      Issuer='root_rsa';       Role='intermediate'; Expected='valid';   Reason='' }
    @{ Name='inter_mixed';         Key='rsa'; Cn='JC2 Test Mixed Intermediate';  San='DNS:inter-mixed.jc2.test';            NotBefore='200101000000Z'; NotAfter='350101000000Z'; Kind='inter_ca';      Issuer='root_rsa';       Role='intermediate'; Expected='valid';   Reason='' }
    @{ Name='inter_ec';            Key='ec';  Cn='JC2 Test EC Intermediate';     San='DNS:inter-ec.jc2.test';               NotBefore='200101000000Z'; NotAfter='350101000000Z'; Kind='inter_ca';      Issuer='root_ec';        Role='intermediate'; Expected='valid';   Reason='' }
    @{ Name='neg_pathlen_inter';   Key='rsa'; Cn='JC2 Pathlen Intermediate';     San='DNS:pathlen-inter.jc2.test';          NotBefore='200101000000Z'; NotAfter='350101000000Z'; Kind='pathlen_inter'; Issuer='neg_pathlen_root'; Role='intermediate'; Expected='invalid'; Reason='subordinate CA of a pathlen:0 root (path length constraint violated)' }
    @{ Name='leaf_rsa';            Key='rsa'; Cn='example.com';                  San='DNS:example.com, DNS:www.example.com';NotBefore='200101000000Z'; NotAfter='350101000000Z'; Kind='leaf';          Issuer='inter_rsa';      Role='leaf';         Expected='valid';   Reason='' }
    @{ Name='leaf_wild';           Key='rsa'; Cn='*.wild.example.com';           San='DNS:*.wild.example.com';              NotBefore='200101000000Z'; NotAfter='350101000000Z'; Kind='leaf';          Issuer='inter_rsa';      Role='leaf';         Expected='valid';   Reason='' }
    @{ Name='neg_expired';         Key='rsa'; Cn='expired.example.com';          San='DNS:expired.example.com';             NotBefore='190101000000Z'; NotAfter='200101000000Z'; Kind='leaf';          Issuer='inter_rsa';      Role='leaf';         Expected='invalid'; Reason='certificate has expired' }
    @{ Name='neg_not_yet_valid';   Key='rsa'; Cn='notyet.example.com';           San='DNS:notyet.example.com';              NotBefore='360101000000Z'; NotAfter='400101000000Z'; Kind='leaf';          Issuer='inter_rsa';      Role='leaf';         Expected='invalid'; Reason='certificate is not yet valid' }
    @{ Name='neg_wrong_host';      Key='rsa'; Cn='other.example.com';            San='DNS:other.example.com';               NotBefore='200101000000Z'; NotAfter='350101000000Z'; Kind='leaf';          Issuer='inter_rsa';      Role='leaf';         Expected='invalid'; Reason='hostname mismatch (leaf is only valid for other.example.com)' }
    @{ Name='neg_ca_false';        Key='rsa'; Cn='JC2 Test Not-A-CA';            San='DNS:cafalse.jc2.test';                NotBefore='200101000000Z'; NotAfter='350101000000Z'; Kind='not_a_ca';      Issuer='root_rsa';       Role='intermediate'; Expected='invalid'; Reason='issuer certificate is not a CA (basicConstraints CA:FALSE, keyUsage lacks keyCertSign)' }
    @{ Name='leaf_ec';             Key='ec';  Cn='ec.example.com';               San='DNS:ec.example.com';                  NotBefore='200101000000Z'; NotAfter='350101000000Z'; Kind='leaf';          Issuer='inter_ec';       Role='leaf';         Expected='valid';   Reason='' }
    @{ Name='leaf_mixed';          Key='ec';  Cn='mixed.example.com';            San='DNS:mixed.example.com';               NotBefore='200101000000Z'; NotAfter='350101000000Z'; Kind='leaf';          Issuer='inter_mixed';    Role='leaf';         Expected='valid';   Reason='' }
    @{ Name='neg_pathlen_leaf';    Key='rsa'; Cn='pathlen.example.com';          San='DNS:pathlen.example.com';             NotBefore='200101000000Z'; NotAfter='350101000000Z'; Kind='leaf';          Issuer='neg_pathlen_inter'; Role='leaf';       Expected='invalid'; Reason='subordinate CA of a pathlen:0 root (path length constraint violated)' }
    @{ Name='leaf_under_cafalse';  Key='rsa'; Cn='under-cafalse.example.com';    San='DNS:under-cafalse.example.com';       NotBefore='200101000000Z'; NotAfter='350101000000Z'; Kind='leaf';          Issuer='neg_ca_false';   Role='leaf';         Expected='invalid'; Reason='issuer certificate is not a CA (basicConstraints CA:FALSE, keyUsage lacks keyCertSign)' }
    @{ Name='neg_wrong_root';      Key='ec';  Cn='wrongroot.example.com';        San='DNS:wrongroot.example.com';           NotBefore='200101000000Z'; NotAfter='350101000000Z'; Kind='leaf';          Issuer='root_ec';        Role='leaf';         Expected='invalid'; Reason='chain does not terminate at the requested trust anchor (root_rsa)' }
)

# Certificates that are pure DER surgery on top of the generated ones.
$SyntheticCerts = @(
    @{ Name='neg_tampered_sig'; Role='leaf';     Expected='invalid'; Reason='certificate signature failure (one byte of the signature flipped)';       Source='leaf_rsa';   Kind='tampered' }
    @{ Name='neg_truncated';    Role='leaf';     Expected='invalid'; Reason='DER parse error - the last 50 bytes of leaf_rsa.der are missing';        Source='leaf_rsa';   Kind='truncated' }
    @{ Name='neg_garbage';      Role='n/a';      Expected='invalid'; Reason='DER parse error - not a certificate (64 bytes of 0xDEADBEEF pattern)';    Source=$null;        Kind='garbage' }
)

# CA sections of openssl.cnf.  Every certificate that acts as an issuer needs one.
# Seed = deterministic first serial number handed to that CA.
$IssuerCa = @{}
$serialSeed = 0x1001
foreach ($c in $Certs) {
    if ($null -ne $c.Issuer -and -not $IssuerCa.ContainsKey($c.Issuer)) {
        $IssuerCa[$c.Issuer] = ('{0:X4}' -f $serialSeed)
        $serialSeed += 0x10
    }
}
# Self-signed certificates also need a CA section, because `openssl ca -selfsign`
# is what applies the fixed start/end dates.
foreach ($c in $Certs) {
    if ($null -eq $c.Issuer -and -not $IssuerCa.ContainsKey($c.Name)) {
        $IssuerCa[$c.Name] = ('{0:X4}' -f $serialSeed)
        $serialSeed += 0x10
    }
}

# ===========================================================================
# Helpers
# ===========================================================================

function Invoke-OpenSSL {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string[]] $Arguments,
        [switch] $AllowFailure
    )
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $raw  = & $OpenSSL @Arguments 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $saved
    }
    $text = (@($raw) | ForEach-Object { $_.ToString() }) -join "`n"
    if ($code -ne 0 -and -not $AllowFailure) {
        throw "openssl $($Arguments -join ' ') failed (exit $code):`n$text"
    }
    [pscustomobject]@{
        ExitCode = $code
        Output   = $text
        Command  = ('openssl ' + ($Arguments -join ' '))
    }
}

function Get-ExtLines {
    param([hashtable] $Cert)
    $lines = @()
    switch ($Cert.Kind) {
        'root_ca'       { $lines += 'basicConstraints = critical, CA:TRUE'
                          $lines += 'keyUsage = critical, keyCertSign, cRLSign' }
        'pathlen_root'  { $lines += 'basicConstraints = critical, CA:TRUE, pathlen:0'
                          $lines += 'keyUsage = critical, keyCertSign, cRLSign' }
        'inter_ca'      { $lines += 'basicConstraints = critical, CA:TRUE, pathlen:0'
                          $lines += 'keyUsage = critical, keyCertSign, cRLSign' }
        'pathlen_inter' { $lines += 'basicConstraints = critical, CA:TRUE'
                          $lines += 'keyUsage = critical, keyCertSign, cRLSign' }
        'not_a_ca'      { $lines += 'basicConstraints = critical, CA:FALSE'
                          $lines += 'keyUsage = critical, digitalSignature, keyEncipherment'
                          $lines += 'extendedKeyUsage = serverAuth' }
        'leaf'          { $lines += 'basicConstraints = critical, CA:FALSE'
                          if ($Cert.Key -eq 'rsa') {
                              $lines += 'keyUsage = critical, digitalSignature, keyEncipherment'
                          } else {
                              $lines += 'keyUsage = critical, digitalSignature'
                          }
                          $lines += 'extendedKeyUsage = serverAuth' }
        default         { throw "unknown extension kind '$($Cert.Kind)'" }
    }
    $lines += 'subjectKeyIdentifier = hash'
    $lines += 'authorityKeyIdentifier = keyid:always'
    $lines += ('subjectAltName = ' + $Cert.San)
    return $lines
}

function Write-TextFile {
    param([string] $Path, [string[]] $Lines)
    $dir = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    Set-Content -LiteralPath $Path -Value $Lines -Encoding ascii
}

function Get-PemPath { param([string] $Name) Join-Path $PemDir "$Name.pem" }
function Get-DerPath { param([string] $Name) Join-Path $Root "$Name.der" }

# ===========================================================================
# -Check mode: manifest <-> disk consistency + re-run of the verify commands
# ===========================================================================
if ($Check) {
    $failures = 0
    if (-not (Test-Path -LiteralPath $Manifest)) { throw "manifest not found: $Manifest" }

    Write-Host '== manifest byte counts vs. files on disk =='
    $rows = 0
    foreach ($line in Get-Content -LiteralPath $Manifest) {
        if ($line -match '^\|\s*([A-Za-z0-9_]+\.der)\s*\|\s*(\d+)\s*\|') {
            $rows++
            $name = $Matches[1]
            $want = [long] $Matches[2]
            $path = Join-Path $Root $name
            if (-not (Test-Path -LiteralPath $path)) {
                Write-Host ("  MISSING  {0}" -f $name); $failures++; continue
            }
            $got = (Get-Item -LiteralPath $path).Length
            if ($got -ne $want) {
                Write-Host ("  MISMATCH {0}: manifest={1} disk={2}" -f $name, $want, $got); $failures++
            }
        }
    }
    Write-Host ("  checked {0} rows" -f $rows)

    Write-Host ''
    Write-Host '== re-running openssl verify =='
    $checks = @(
        @{ T='RSA chain valid';            C=@('verify','-attime',$Attime,'-CAfile',(Get-PemPath 'root_rsa'),'-untrusted',(Get-PemPath 'inter_rsa'),(Get-DerPath 'leaf_rsa'));                                     E=0 }
        @{ T='RSA chain hostname';         C=@('verify','-attime',$Attime,'-CAfile',(Get-PemPath 'root_rsa'),'-untrusted',(Get-PemPath 'inter_rsa'),'-verify_hostname','example.com',(Get-DerPath 'leaf_rsa')); E=0 }
        @{ T='EC chain valid';             C=@('verify','-attime',$Attime,'-CAfile',(Get-PemPath 'root_ec'),'-untrusted',(Get-PemPath 'inter_ec'),(Get-DerPath 'leaf_ec'));                                        E=0 }
        @{ T='Mixed chain valid';          C=@('verify','-attime',$Attime,'-CAfile',(Get-PemPath 'root_rsa'),'-untrusted',(Get-PemPath 'inter_mixed'),(Get-DerPath 'leaf_mixed'));                                E=0 }
        @{ T='Wildcard hostname';          C=@('verify','-attime',$Attime,'-CAfile',(Get-PemPath 'root_rsa'),'-untrusted',(Get-PemPath 'inter_rsa'),'-verify_hostname','a.wild.example.com',(Get-DerPath 'leaf_wild')); E=0 }
        @{ T='Wildcard zero-label rejected'; C=@('verify','-attime',$Attime,'-CAfile',(Get-PemPath 'root_rsa'),'-untrusted',(Get-PemPath 'inter_rsa'),'-verify_hostname','wild.example.com',(Get-DerPath 'leaf_wild')); E=2 }
        @{ T='Wildcard too deep rejected'; C=@('verify','-attime',$Attime,'-CAfile',(Get-PemPath 'root_rsa'),'-untrusted',(Get-PemPath 'inter_rsa'),'-verify_hostname','a.b.wild.example.com',(Get-DerPath 'leaf_wild')); E=2 }
        @{ T='EC leaf hostname';           C=@('verify','-attime',$Attime,'-CAfile',(Get-PemPath 'root_ec'),'-untrusted',(Get-PemPath 'inter_ec'),'-verify_hostname','ec.example.com',(Get-DerPath 'leaf_ec')); E=0 }
        @{ T='Expired rejected';           C=@('verify','-attime',$Attime,'-CAfile',(Get-PemPath 'root_rsa'),'-untrusted',(Get-PemPath 'inter_rsa'),(Get-DerPath 'neg_expired'));                                  E=2 }
        @{ T='Not-yet-valid rejected';     C=@('verify','-attime',$Attime,'-CAfile',(Get-PemPath 'root_rsa'),'-untrusted',(Get-PemPath 'inter_rsa'),(Get-DerPath 'neg_not_yet_valid'));                           E=2 }
        @{ T='Wrong host rejected';        C=@('verify','-attime',$Attime,'-CAfile',(Get-PemPath 'root_rsa'),'-untrusted',(Get-PemPath 'inter_rsa'),'-verify_hostname','example.com',(Get-DerPath 'neg_wrong_host')); E=2 }
        @{ T='CA:FALSE issuer rejected';   C=@('verify','-attime',$Attime,'-CAfile',(Get-PemPath 'root_rsa'),'-untrusted',(Get-PemPath 'neg_ca_false'),(Get-DerPath 'leaf_under_cafalse')); E=2 }
        @{ T='pathlen violated';           C=@('verify','-attime',$Attime,'-CAfile',(Get-PemPath 'neg_pathlen_root'),'-untrusted',(Get-PemPath 'neg_pathlen_inter'),(Get-DerPath 'neg_pathlen_leaf'));            E=2 }
        @{ T='Untrusted self-signed';      C=@('verify','-attime',$Attime,'-CAfile',(Get-PemPath 'root_rsa'),(Get-DerPath 'neg_self_signed'));                                                                     E=2 }
        @{ T='Wrong trust anchor';         C=@('verify','-attime',$Attime,'-CAfile',(Get-PemPath 'root_rsa'),'-untrusted',(Get-PemPath 'inter_rsa'),(Get-DerPath 'neg_wrong_root'));                              E=2 }
        @{ T='Tampered signature';         C=@('verify','-attime',$Attime,'-CAfile',(Get-PemPath 'root_rsa'),'-untrusted',(Get-PemPath 'inter_rsa'),(Get-DerPath 'neg_tampered_sig'));                            E=2 }
        @{ T='Tampered DER still parses';  C=@('asn1parse','-inform','DER','-in',(Get-DerPath 'neg_tampered_sig'));                                                                                                E=0 }
        @{ T='Truncated DER fails';        C=@('x509','-inform','DER','-in',(Get-DerPath 'neg_truncated'),'-noout');                                                                                              E=1 }
        @{ T='Garbage DER fails';          C=@('x509','-inform','DER','-in',(Get-DerPath 'neg_garbage'),'-noout');                                                                                                E=1 }
    )
    foreach ($c in $checks) {
        $r = Invoke-OpenSSL -AllowFailure -Arguments $c.C
        $ok = ($r.ExitCode -eq $c.E)
        if (-not $ok) { $failures++ }
        Write-Host ("  [{0}] {1} (exit {2}, expected {3})" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $c.T, $r.ExitCode, $c.E)
        if (-not $ok) { Write-Host ("        $($r.Output)") }
    }
    Write-Host ''
    if ($failures -eq 0) { Write-Host 'CHECK OK'; exit 0 } else { Write-Host "CHECK FAILED ($failures problems)"; exit 1 }
}

# ===========================================================================
# 1. Directories
# ===========================================================================
foreach ($d in @($KeysDir, $CsrDir, $WorkDir, $PemDir, $CaRoot)) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}
if (Test-Path -LiteralPath $CaRoot) { Remove-Item -LiteralPath $CaRoot -Recurse -Force }
Get-ChildItem -LiteralPath $PemDir -File -ErrorAction SilentlyContinue | Remove-Item -Force
New-Item -ItemType Directory -Force -Path $CaRoot | Out-Null

foreach ($ca in $IssuerCa.Keys) {
    $dir = Join-Path $CaRoot "ca_$ca"
    New-Item -ItemType Directory -Force -Path (Join-Path $dir 'newcerts') | Out-Null
    New-Item -ItemType File -Force -Path (Join-Path $dir 'index.txt') | Out-Null
    Set-Content -LiteralPath (Join-Path $dir 'serial') -Value $IssuerCa[$ca] -Encoding ascii -NoNewline
}

# ===========================================================================
# 2. openssl.cnf
# ===========================================================================
$conf = @()
$conf += '# Generated by generate.ps1 -- do not edit by hand, re-run the script instead.'
$conf += '# NOTE: no "rand_serial" key on purpose (see the header of generate.ps1).'
$conf += ''
$conf += '[ ca ]'
$conf += 'default_ca = CA_default'
$conf += ''
$conf += '[ policy_any ]'
$conf += 'commonName             = supplied'
$conf += 'organizationName       = optional'
$conf += 'countryName            = optional'
$conf += 'stateOrProvinceName    = optional'
$conf += 'localityName           = optional'
$conf += 'organizationalUnitName = optional'
$conf += 'emailAddress           = optional'
$conf += ''

foreach ($ca in ($IssuerCa.Keys | Sort-Object)) {
    $caDir = "$RootFwd/work/ca/ca_$ca"
    $conf += "[ ca_$ca ]"
    $conf += "dir              = $caDir"
    $conf += "database         = $caDir/index.txt"
    $conf += "new_certs_dir    = $caDir/newcerts"
    $conf += "serial           = $caDir/serial"
    $conf += "certificate      = $caDir/ca.pem"
    $conf += "private_key      = $caDir/ca.key.pem"
    $conf += 'default_md       = sha256'
    $conf += 'policy           = policy_any'
    $conf += 'email_in_dn      = no'
    $conf += 'unique_subject   = no'
    $conf += 'copy_extensions  = none'
    $conf += 'default_days     = 365'
    $conf += 'default_crl_days = 30'
    $conf += 'name_opt         = ca_default'
    $conf += 'cert_opt         = ca_default'
    $conf += ''
}

# A minimal CA_default so that a stray `-name`-less invocation still fails loudly
# instead of silently picking up something unexpected.
$conf += '[ CA_default ]'
$conf += "dir              = $RootFwd/work/ca/ca_root_rsa"
$conf += 'database         = $dir/index.txt'
$conf += 'new_certs_dir    = $dir/newcerts'
$conf += 'serial           = $dir/serial'
$conf += 'certificate      = $dir/ca.pem'
$conf += 'private_key      = $dir/ca.key.pem'
$conf += 'default_md       = sha256'
$conf += 'policy           = policy_any'
$conf += 'unique_subject   = no'
$conf += 'copy_extensions  = none'
$conf += 'default_days     = 365'
$conf += ''

foreach ($c in $Certs) {
    $conf += "[ v3_$($c.Name) ]"
    $conf += (Get-ExtLines -Cert $c)
    $conf += ''
}
Write-TextFile -Path $ConfigPath -Lines $conf
Write-Host "wrote $ConfigPath"

# ===========================================================================
# 3. Private keys
# ===========================================================================
foreach ($c in $Certs) {
    $keyFile = Join-Path $KeysDir "$($c.Name).key.pem"
    if ((Test-Path -LiteralPath $keyFile) -and -not $ForceKeys) {
        Write-Host "key  $($c.Name) (kept)"
        continue
    }
    if ($c.Key -eq 'rsa') {
        Invoke-OpenSSL -Arguments @('genpkey','-algorithm','RSA','-pkeyopt','rsa_keygen_bits:2048','-out',$keyFile) | Out-Null
    } else {
        Invoke-OpenSSL -Arguments @('genpkey','-algorithm','EC','-pkeyopt','ec_paramgen_curve:P-256','-out',$keyFile) | Out-Null
    }
    Write-Host "key  $($c.Name) (generated)"
}

# ===========================================================================
# 4. CSRs
# ===========================================================================
foreach ($c in $Certs) {
    $keyFile = Join-Path $KeysDir "$($c.Name).key.pem"
    $csrFile = Join-Path $CsrDir  "$($c.Name).csr.pem"
    Invoke-OpenSSL -Arguments @('req','-new','-key',$keyFile,'-subj',"/O=JC2 Test Fixtures/CN=$($c.Cn)",'-out',$csrFile) | Out-Null
}

# ===========================================================================
# 5. Issue every certificate (parents first, fixed notBefore / notAfter)
# ===========================================================================
$isIssuer = @{}
foreach ($c in $Certs) {
    if ($null -ne $c.Issuer) { $isIssuer[$c.Issuer] = $true }
}

foreach ($c in $Certs) {
    $csrFile = Join-Path $CsrDir "$($c.Name).csr.pem"
    $pemFile = Get-PemPath $c.Name
    if ($null -eq $c.Issuer) {
        $caName = "ca_$($c.Name)"
        $args = @('ca','-config',$ConfigPath,'-name',$caName,'-selfsign',
                  '-keyfile',(Join-Path $KeysDir "$($c.Name).key.pem"),
                  '-in',$csrFile,'-extfile',$ConfigPath,'-extensions',"v3_$($c.Name)",
                  '-startdate',$c.NotBefore,'-enddate',$c.NotAfter,
                  '-batch','-notext','-out',$pemFile)
    } else {
        $caName = "ca_$($c.Issuer)"
        $args = @('ca','-config',$ConfigPath,'-name',$caName,
                  '-in',$csrFile,'-extfile',$ConfigPath,'-extensions',"v3_$($c.Name)",
                  '-startdate',$c.NotBefore,'-enddate',$c.NotAfter,
                  '-batch','-notext','-out',$pemFile)
    }
    Invoke-OpenSSL -Arguments $args | Out-Null
    Write-Host "cert $($c.Name) <- $(if ($null -eq $c.Issuer) { 'self' } else { $c.Issuer })"

    if ($isIssuer.ContainsKey($c.Name)) {
        $caDir = Join-Path $CaRoot "ca_$($c.Name)"
        Copy-Item -LiteralPath $pemFile -Destination (Join-Path $caDir 'ca.pem') -Force
        Copy-Item -LiteralPath (Join-Path $KeysDir "$($c.Name).key.pem") -Destination (Join-Path $caDir 'ca.key.pem') -Force
    }
}

# ===========================================================================
# 6. DER conversion
# ===========================================================================
foreach ($c in $Certs) {
    Invoke-OpenSSL -Arguments @('x509','-in',(Get-PemPath $c.Name),'-outform','DER','-out',(Get-DerPath $c.Name)) | Out-Null
}

# ===========================================================================
# 7. Negative fixtures produced by byte surgery
# ===========================================================================
$bytes = @{}

# 7a. neg_tampered_sig: flip every bit of the last byte of the signature BIT STRING.
#     The signatureValue is the final element of a Certificate, so its last byte is
#     the last byte of the file.  The DER framing is untouched.
$src = [System.IO.File]::ReadAllBytes((Get-DerPath 'leaf_rsa'))
$tampered = [byte[]]::new($src.Length)
[Array]::Copy($src, $tampered, $src.Length)
$lastIndex = $tampered.Length - 1
$before = $tampered[$lastIndex]
$tampered[$lastIndex] = $tampered[$lastIndex] -bxor 0xFF
[System.IO.File]::WriteAllBytes((Get-DerPath 'neg_tampered_sig'), $tampered)
$bytes['neg_tampered_sig'] = @{ Offset = $lastIndex; Before = $before; After = $tampered[$lastIndex] }

# 7b. neg_truncated: drop the last 50 bytes.
$truncated = [byte[]]::new($src.Length - 50)
[Array]::Copy($src, 0, $truncated, 0, $truncated.Length)
[System.IO.File]::WriteAllBytes((Get-DerPath 'neg_truncated'), $truncated)

# 7c. neg_garbage: fixed 64-byte non-DER blob (0xDE 0xAD 0xBE 0xEF x 16).
$garbage = [byte[]]::new(64)
for ($i = 0; $i -lt 64; $i++) {
    $garbage[$i] = @(0xDE, 0xAD, 0xBE, 0xEF)[$i % 4]
}
[System.IO.File]::WriteAllBytes((Get-DerPath 'neg_garbage'), $garbage)

# ===========================================================================
# 8. Untrusted bundles used by the verification commands
# ===========================================================================
function New-UntrustedBundle {
    param([string[]] $Names)
    $bundle = Join-Path $PemDir ('untrusted_' + ($Names -join '_') + '.pem')
    $text = ''
    foreach ($n in $Names) { $text += (Get-Content -LiteralPath (Get-PemPath $n) -Raw) }
    Set-Content -LiteralPath $bundle -Value $text -Encoding ascii -NoNewline
    return $bundle
}

# ===========================================================================
# 9. Read back everything we need for the manifest
# ===========================================================================
function Get-CertFacts {
    param([string] $DerPath)
    $facts = [ordered]@{
        Parsed  = $false
        Subject = 'n/a'; Issuer = 'n/a'; NotBefore = 'n/a'; NotAfter = 'n/a'
        San = 'n/a'; PubKey = 'n/a'; SigAlg = 'n/a'
    }
    $r = Invoke-OpenSSL -AllowFailure -Arguments @('x509','-inform','DER','-in',$DerPath,'-noout',
            '-subject','-issuer','-nameopt','RFC2253','-dates','-dateopt','iso_8601','-ext','subjectAltName')
    if ($r.ExitCode -ne 0) { return [pscustomobject] $facts }

    foreach ($line in ($r.Output -split "`n")) {
        $t = $line.TrimEnd()
        if     ($t -like 'subject=*')   { $facts.Subject   = $t.Substring(8) }
        elseif ($t -like 'issuer=*')    { $facts.Issuer    = $t.Substring(7) }
        elseif ($t -like 'notBefore=*') { $facts.NotBefore = $t.Substring(10) }
        elseif ($t -like 'notAfter=*')  { $facts.NotAfter  = $t.Substring(9) }
    }
    $m = [regex]::Match($r.Output, '(?s)Subject Alternative Name:\s*\r?\n\s*([^\r\n]+)')
    if ($m.Success) { $facts.San = $m.Groups[1].Value.Trim() }

    $t = Invoke-OpenSSL -AllowFailure -Arguments @('x509','-inform','DER','-in',$DerPath,'-noout','-text')
    if ($t.ExitCode -eq 0) {
        $m = [regex]::Match($t.Output, 'Signature Algorithm:\s*(\S+)')
        if ($m.Success) { $facts.SigAlg = $m.Groups[1].Value }
        if ($t.Output -match 'ASN1 OID:\s*prime256v1' -or $t.Output -match 'NIST CURVE:\s*prime256v1' -or $t.Output -match 'NIST CURVE:\s*P-256') {
            $facts.PubKey = 'ecdsa-p256'
        } elseif ($t.Output -match 'Public-Key:\s*\(2048 bit\)') {
            $facts.PubKey = 'rsa2048'
        }
    }
    $facts.Parsed = $true
    return [pscustomobject] $facts
}

# ===========================================================================
# 10. Verification suite
# ===========================================================================
# Chain definitions: ordered leaf -> ... -> root plus the trust anchor used.
$ChainDefs = @(
    @{ Id='1'; Title='RSA chain (RSA-2048 everywhere)';                        Trust='root_rsa';          Files=@('leaf_rsa','inter_rsa','root_rsa');                 Untrusted=@('inter_rsa');            Target='leaf_rsa';           Expected='valid';   Attime=$Attime }
    @{ Id='2'; Title='EC chain (P-256 everywhere)';                            Trust='root_ec';           Files=@('leaf_ec','inter_ec','root_ec');                    Untrusted=@('inter_ec');             Target='leaf_ec';            Expected='valid';   Attime=$Attime }
    @{ Id='3'; Title='Mixed chain (P-256 leaf, RSA-2048 intermediate + root)'; Trust='root_rsa';          Files=@('leaf_mixed','inter_mixed','root_rsa');             Untrusted=@('inter_mixed');          Target='leaf_mixed';         Expected='valid';   Attime=$Attime }
    @{ Id='4'; Title='Wildcard chain (SAN DNS:*.wild.example.com)';            Trust='root_rsa';          Files=@('leaf_wild','inter_rsa','root_rsa');                Untrusted=@('inter_rsa');            Target='leaf_wild';          Expected='valid';   Attime=$Attime }
    @{ Id='5'; Title='Expired leaf';                                           Trust='root_rsa';          Files=@('neg_expired','inter_rsa','root_rsa');              Untrusted=@('inter_rsa');            Target='neg_expired';        Expected='invalid'; Attime=$Attime }
    @{ Id='6'; Title='Not-yet-valid leaf';                                     Trust='root_rsa';          Files=@('neg_not_yet_valid','inter_rsa','root_rsa');        Untrusted=@('inter_rsa');            Target='neg_not_yet_valid';  Expected='invalid'; Attime=$Attime }
    @{ Id='7'; Title='Hostname mismatch leaf';                                 Trust='root_rsa';          Files=@('neg_wrong_host','inter_rsa','root_rsa');           Untrusted=@('inter_rsa');            Target='neg_wrong_host';     Expected='invalid'; Attime=$Attime; Hostname='example.com' }
    @{ Id='8'; Title='Leaf under a CA:FALSE certificate';                      Trust='root_rsa';          Files=@('leaf_under_cafalse','neg_ca_false','root_rsa');    Untrusted=@('neg_ca_false');         Target='leaf_under_cafalse'; Expected='invalid'; Attime=$Attime; Note='`neg_ca_false.der` 由 `root_rsa.der` 直接签发（不经中间 CA），这样链上唯一的缺陷就是签发者不是 CA，不会与 `pathlen:0` 之类的次要错误混在一起。' }
    @{ Id='9'; Title='pathlen:0 root with a subordinate CA';                   Trust='neg_pathlen_root';  Files=@('neg_pathlen_leaf','neg_pathlen_inter','neg_pathlen_root'); Untrusted=@('neg_pathlen_inter');   Target='neg_pathlen_leaf'; Expected='invalid'; Attime=$Attime }
    @{ Id='10'; Title='Self-signed certificate outside the trust store';       Trust='root_rsa';          Files=@('neg_self_signed');                                 Untrusted=@();                       Target='neg_self_signed';    Expected='invalid'; Attime=$Attime }
    @{ Id='11'; Title='Chain issued by the wrong root (root_ec, trusted via root_rsa)'; Trust='root_rsa'; Files=@('neg_wrong_root','root_ec');                        Untrusted=@('inter_rsa');            Target='neg_wrong_root';     Expected='invalid'; Attime=$Attime; Note='`neg_wrong_root.der` 由 `root_ec.der` 签发；这里故意只用 `root_rsa.der` 作信任根，因此链无法建立。' }
    @{ Id='12'; Title='Tampered signature (1 byte flipped in leaf_rsa)';       Trust='root_rsa';          Files=@('neg_tampered_sig','inter_rsa','root_rsa');         Untrusted=@('inter_rsa');            Target='neg_tampered_sig';   Expected='invalid'; Attime=$Attime }
)

function Invoke-VerifyCase {
    param([string] $Target, [string] $Trust, [string[]] $Untrusted, [long] $Attime, [string[]] $Extra = @())
    $a = @('verify','-attime',$Attime,'-CAfile',(Get-PemPath $Trust))
    if ($Untrusted.Count -gt 1) {
        $a += @('-untrusted', (New-UntrustedBundle -Names $Untrusted))
    } else {
        foreach ($u in $Untrusted) { $a += @('-untrusted',(Get-PemPath $u)) }
    }
    $a += $Extra
    $a += (Get-DerPath $Target)
    return Invoke-OpenSSL -AllowFailure -Arguments $a
}

$chainResults = @()
foreach ($ch in $ChainDefs) {
    $extra = @()
    if ($ch.ContainsKey('Hostname') -and $ch.Hostname) { $extra = @('-verify_hostname', $ch.Hostname) }
    $r = Invoke-VerifyCase -Target $ch.Target -Trust $ch.Trust -Untrusted $ch.Untrusted -Attime $ch.Attime -Extra $extra
    $chainResults += [pscustomobject]@{ Def = $ch; Result = $r }
}

# Extra hostname checks (RFC 6125 wildcard rules).
$hostChecks = @(
    @{ Target='leaf_rsa';       Ca='root_rsa'; Untrusted=@('inter_rsa'); Host='example.com';          ExpectExit=0; Note='exact SAN match' }
    @{ Target='leaf_rsa';       Ca='root_rsa'; Untrusted=@('inter_rsa'); Host='www.example.com';      ExpectExit=0; Note='second SAN entry' }
    @{ Target='leaf_rsa';       Ca='root_rsa'; Untrusted=@('inter_rsa'); Host='WWW.EXAMPLE.COM';      ExpectExit=0; Note='DNS names compare case-insensitively' }
    @{ Target='leaf_ec';        Ca='root_ec';  Untrusted=@('inter_ec');  Host='ec.example.com';       ExpectExit=0; Note='EC leaf SAN' }
    @{ Target='leaf_wild';      Ca='root_rsa'; Untrusted=@('inter_rsa'); Host='a.wild.example.com';   ExpectExit=0; Note='wildcard matches exactly one label' }
    @{ Target='leaf_wild';      Ca='root_rsa'; Untrusted=@('inter_rsa'); Host='wild.example.com';     ExpectExit=2; Note='wildcard must not match zero labels (RFC 6125)' }
    @{ Target='leaf_wild';      Ca='root_rsa'; Untrusted=@('inter_rsa'); Host='a.b.wild.example.com'; ExpectExit=2; Note='wildcard must not span two labels' }
    @{ Target='leaf_wild';      Ca='root_rsa'; Untrusted=@('inter_rsa'); Host='example.com';          ExpectExit=2; Note='unrelated host rejected' }
    @{ Target='neg_wrong_host'; Ca='root_rsa'; Untrusted=@('inter_rsa'); Host='example.com';          ExpectExit=2; Note='leaf is only valid for other.example.com' }
    @{ Target='neg_wrong_host'; Ca='root_rsa'; Untrusted=@('inter_rsa'); Host='other.example.com';    ExpectExit=0; Note='certificate itself is otherwise fine' }
    @{ Target='neg_expired';    Ca='root_rsa'; Untrusted=@('inter_rsa'); Host='expired.example.com';  ExpectExit=2; Note='hostname matches but the leaf is expired' }
)
$hostResults = @()
foreach ($h in $hostChecks) {
    $a = @('verify','-attime',$Attime,'-CAfile',(Get-PemPath $h.Ca))
    foreach ($u in $h.Untrusted) { $a += @('-untrusted',(Get-PemPath $u)) }
    $a += @('-verify_hostname',$h.Host,(Get-DerPath $h.Target))
    $r = Invoke-OpenSSL -AllowFailure -Arguments $a
    $hostResults += [pscustomobject]@{ Def = $h; Result = $r }
}

# Structural / parser checks for the byte-surgery fixtures.
$structuralChecks = @()
$structuralChecks += [pscustomobject]@{
    Title = 'neg_tampered_sig.der still parses as DER (openssl asn1parse)'
    Result = Invoke-OpenSSL -AllowFailure -Arguments @('asn1parse','-inform','DER','-in',(Get-DerPath 'neg_tampered_sig'))
    Expect = 'exit 0, full structure printed'
}
$structuralChecks += [pscustomobject]@{
    Title = 'neg_truncated.der is rejected by the parser'
    Result = Invoke-OpenSSL -AllowFailure -Arguments @('x509','-inform','DER','-in',(Get-DerPath 'neg_truncated'),'-noout')
    Expect = 'non-zero exit'
}
$structuralChecks += [pscustomobject]@{
    Title = 'neg_garbage.der is rejected by the parser'
    Result = Invoke-OpenSSL -AllowFailure -Arguments @('x509','-inform','DER','-in',(Get-DerPath 'neg_garbage'),'-noout')
    Expect = 'non-zero exit'
}
$structuralChecks += [pscustomobject]@{
    Title = 'Positive control: inter_rsa.der validates against root_rsa.der'
    Result = Invoke-OpenSSL -AllowFailure -Arguments @('verify','-attime',$Attime,'-CAfile',(Get-PemPath 'root_rsa'),(Get-DerPath 'inter_rsa'))
    Expect = 'exit 0'
}
$structuralChecks += [pscustomobject]@{
    Title = 'Positive control: neg_pathlen_root.der validates as a self-signed root on its own'
    Result = Invoke-OpenSSL -AllowFailure -Arguments @('verify','-attime',$Attime,'-CAfile',(Get-PemPath 'neg_pathlen_root'),(Get-DerPath 'neg_pathlen_root'))
    Expect = 'exit 0'
}
$structuralChecks += [pscustomobject]@{
    Title = 'Control: neg_pathlen_inter.der used as the END of the path is accepted'
    Result = Invoke-OpenSSL -AllowFailure -Arguments @('verify','-attime',$Attime,'-CAfile',(Get-PemPath 'neg_pathlen_root'),(Get-DerPath 'neg_pathlen_inter'))
    Expect = 'exit 0 - pathLenConstraint counts only the intermediate CA certificates that FOLLOW a certificate, so the path target itself is not counted. The violation therefore only shows up when the path continues below the intermediate (see Chain 9).'
}

$osslVersion = (Invoke-OpenSSL -Arguments @('version')).Output.Trim()

# ===========================================================================
# 11. manifest.md
# ===========================================================================
function Format-Cell { param([string] $s) ($s -replace '\|', '\|') }

$md = @()
$md += '# X.509 certificate test fixtures — manifest'
$md += ''
$md += 'Generated by `generate.ps1` (re-run it to regenerate; `generate.ps1 -Check` re-verifies this file against disk).'
$md += ''
$md += "- OpenSSL: ``$osslVersion``"
$md += "- Fixed verification instant: ``-attime $Attime`` = $AttimeText (UTC). Every ``openssl verify`` below uses it, so no result depends on the wall clock."
$md += '- All certificates: SHA-256 signatures, critical `basicConstraints`, critical `keyUsage`, and a `subjectAltName` with DNS names (never CN-only).'
$md += '- Every certificate is issued with explicit `-startdate` / `-enddate`; no validity date is relative to the current time.'
$md += '- Private keys live in `keys/` and are reused across runs (`-ForceKeys` regenerates them).'
$md += ''
$md += '## Certificate files'
$md += ''
$md += '| 文件 | 字节数 | 角色 | subject | issuer | SAN | notBefore | notAfter | 公钥类型 | 签名算法 | 期望校验结果 | 失败原因 |'
$md += '|---|---:|---|---|---|---|---|---|---|---|---|---|'

$derOwners = @{}
$roleByName = @{}
foreach ($c in $Certs) { $derOwners["$($c.Name).der"] = $c; $roleByName[$c.Name] = $c.Role }
foreach ($s in $SyntheticCerts) { $roleByName[$s.Name] = $s.Role }

$allDer = @()
$allDer += ($Certs | ForEach-Object { "$($_.Name).der" })
$allDer += ($SyntheticCerts | ForEach-Object { "$($_.Name).der" })

foreach ($der in $allDer) {
    $path = Join-Path $Root $der
    $size = (Get-Item -LiteralPath $path).Length
    $facts = Get-CertFacts -DerPath $path

    if ($derOwners.ContainsKey($der)) {
        $c = $derOwners[$der]
        $role = $c.Role; $expected = $c.Expected; $reason = $c.Reason
    } else {
        $s = $SyntheticCerts | Where-Object { "$($_.Name).der" -eq $der }
        $role = $s.Role; $expected = $s.Expected; $reason = $s.Reason
    }
    if ([string]::IsNullOrEmpty($reason)) { $reason = '—' }

    $md += ('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} | {11} |' -f `
        $der, $size, $role, (Format-Cell $facts.Subject), (Format-Cell $facts.Issuer), (Format-Cell $facts.San), `
        $facts.NotBefore, $facts.NotAfter, $facts.PubKey, $facts.SigAlg, $expected, (Format-Cell $reason))
}
$md += ''
$md += '### Notes on the table'
$md += ''
$md += '- `neg_tampered_sig.der` is a byte-for-byte copy of `leaf_rsa.der` with **one** byte changed: the last byte of the file (`0x{0:X2}` -> `0x{1:X2}`, offset {2}). That byte is inside the `signatureValue` BIT STRING, which is the final element of a `Certificate`, so the DER framing stays valid and the structure still parses.' -f $bytes['neg_tampered_sig'].Before, $bytes['neg_tampered_sig'].After, $bytes['neg_tampered_sig'].Offset
$md += '- `neg_truncated.der` is `leaf_rsa.der` with the last 50 bytes removed. `neg_garbage.der` is 64 fixed bytes: `DE AD BE EF` repeated 16 times.'
$md += '- `neg_ca_false.der` carries `basicConstraints=critical, CA:FALSE` and a `keyUsage` without `keyCertSign`, yet it signs `leaf_under_cafalse.der`.'
$md += '- `neg_pathlen_root.der` carries `basicConstraints=critical, CA:TRUE, pathlen:0` yet has a subordinate CA (`neg_pathlen_inter.der`).'
$md += '- `neg_self_signed.der` is a self-signed end-entity certificate (`CA:FALSE`) — a private/self-signed server certificate that is simply not in the trust store. OpenSSL reports error 18 `self-signed certificate`.'
$md += '- For `neg_tampered_sig.der`, `neg_truncated.der` and `neg_garbage.der` the subject/issuer/SAN/date columns are read back from the DER file itself; `n/a` means OpenSSL could not parse it (which is the expected outcome for the latter two).'
$md += ''
$md += '## Chains (leaf → intermediate(s) → root)'
$md += ''
$md += '路径按 leaf → intermediate(s) → root 排列；`root` 一项即该链校验时使用的信任根（`openssl verify -CAfile`）。'
$md += ''

foreach ($cr in $chainResults) {
    $ch = $cr.Def
    $r  = $cr.Result
    $md += "### Chain $($ch.Id) — $($ch.Title)"
    $md += ''
    $md += '| # | 文件 | 角色 |'
    $md += '|---:|---|---|'
    $i = 1
    foreach ($f in $ch.Files) {
        $desc = $Certs | Where-Object { $_.Name -eq $f } | Select-Object -First 1
        $baseRole = if ($null -ne $desc) { $desc.Role } elseif ($roleByName.ContainsKey($f)) { $roleByName[$f] } else { 'n/a' }
        $role = if ($f -eq $ch.Trust) { "$baseRole (trust anchor)" } else { $baseRole }
        $md += ('| {0} | {1}.der | {2} |' -f $i, $f, $role)
        $i++
    }
    $md += ''
    $md += ('- 信任根: `{0}.der`' -f $ch.Trust)
    if ($ch.ContainsKey('Note') -and $ch.Note) { $md += ('- 说明: {0}' -f $ch.Note) }
    $md += ('- 期望: **{0}**' -f $ch.Expected)
    $md += ('- 命令: `{0}`' -f $r.Command)
    $md += ('- 实测: exit {0}' -f $r.ExitCode)
    $md += ''
    $md += '```'
    $md += $r.Output
    $md += '```'
    $md += ''
}

$md += '## Hostname checks (`-verify_hostname`)'
$md += ''
$md += '| 叶子 DER | 主机名 | 期望 | 实测 exit | openssl 输出 |'
$md += '|---|---|---|---:|---|'
foreach ($hr in $hostResults) {
    $h = $hr.Def; $r = $hr.Result
    $verdict = if ($r.ExitCode -eq $h.ExpectExit) { 'PASS' } else { 'UNEXPECTED' }
    $out = ($r.Output -split "`n" | Where-Object { $_.Trim() -ne '' }) -join '<br>'
    $md += ('| {0}.der | {1} | {2} exit {3} ({4}) | {5} | {6} |' -f $h.Target, $h.Host, $(if ($h.ExpectExit -eq 0) { 'OK' } else { 'fail' }), $h.ExpectExit, $h.Note, $r.ExitCode, (Format-Cell $out))
}
$md += ''
$md += '通配符规则（OpenSSL 3.2 / RFC 6125）：`DNS:*.wild.example.com` 匹配 `a.wild.example.com`，但**不**匹配 `wild.example.com`（通配符必须恰好替换一个标签），也不匹配 `a.b.wild.example.com`（不能跨两个标签）。DNS 名比较不区分大小写。'
$md += ''
$md += '## Structural / parser checks'
$md += ''
foreach ($sc in $structuralChecks) {
    $md += ('### {0}' -f $sc.Title)
    $md += ''
    $md += ('- 期望: {0}' -f $sc.Expect)
    $md += ('- 命令: `{0}`' -f $sc.Result.Command)
    $md += ('- 实测: exit {0}' -f $sc.Result.ExitCode)
    $md += ''
    $md += '```'
    $md += $sc.Result.Output
    $md += '```'
    $md += ''
}

$md += '## Raw byte sizes'
$md += ''
$md += '供 C++ 测试直接引用（长度对不上即为夹具漂移）。'
$md += ''
$md += '| 文件 | 字节数 |'
$md += '|---|---:|'
foreach ($der in $allDer) {
    $md += ('| {0} | {1} |' -f $der, (Get-Item -LiteralPath (Join-Path $Root $der)).Length)
}
$md += ''
$md += '## Reproducibility notes'
$md += ''
$md += '- Re-running `generate.ps1` reuses `keys/*.pem` and rewrites every DER file plus this manifest, so the manifest can never disagree with the files on disk.'
$md += '- Serial numbers are deterministic: each CA section starts from a fixed seed in `work/ca/<ca>/serial` and `openssl ca` increments it per issuance. (`rand_serial` must **not** appear in the config: OpenSSL 3.2.1 then ignores the serial file and emits 20 random bytes.)'
$md += '- `work/` is scratch space: `work/pem/` holds the PEM copies used by the verification commands (and by `generate.ps1 -Check`), the CA databases under `work/ca/` are deleted once generation succeeds. The only private keys that persist are the ones in `keys/`.'
$md += '- RSA-2048 signature values are always 256 bytes, so RSA certificate sizes are perfectly stable. ECDSA signatures use a randomised nonce, so re-running with `-ForceKeys` can shift an EC certificate by 1–2 bytes; use `generate.ps1 -Check` to confirm this manifest still matches the files on disk.'
$md += ''

Set-Content -LiteralPath $Manifest -Value $md -Encoding utf8
Write-Host "wrote $Manifest"

# ===========================================================================
# 12. Self-check of the freshly written fixture set
# ===========================================================================
$bad = 0
foreach ($cr in $chainResults) {
    $ch = $cr.Def
    $want = if ($ch.Expected -eq 'valid') { 0 } else { 2 }
    if ($cr.Result.ExitCode -ne $want) {
        Write-Warning "chain $($ch.Id) expected exit $want but got $($cr.Result.ExitCode)"
        $bad++
    }
}
foreach ($hr in $hostResults) {
    if ($hr.Result.ExitCode -ne $hr.Def.ExpectExit) {
        Write-Warning "hostname check $($hr.Def.Target)/$($hr.Def.Host) expected exit $($hr.Def.ExpectExit) but got $($hr.Result.ExitCode)"
        $bad++
    }
}
if ($bad -ne 0) { throw "$bad verification expectation(s) not met - see the warnings above" }
Write-Host 'all chain and hostname expectations met'

# The CA databases are scratch state and contain a second copy of the CA private
# keys; drop them so the only private keys on disk are the ones in keys\.
Remove-Item -LiteralPath $CaRoot -Recurse -Force
Write-Host 'cleaned scratch CA databases under work/ca'
