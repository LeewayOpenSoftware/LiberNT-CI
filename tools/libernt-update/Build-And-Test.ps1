<#
.SYNOPSIS
    Host build and unit tests of the LiberNT Update stage 1 core (libservicing + libernt-update.exe).

.DESCRIPTION
    Standalone: it does not use the LiberNT build. It compiles the sources with llvm-mingw clang (the
    RosBE package) for x86_64 Windows, builds signed fixture packages with the DEVELOPMENT certificate,
    and runs the tests against a temporary Image target. Compilation and tests are reported separately.

    Needs: the development certificate (New-DevSigningCert.ps1), Windows PowerShell 5.1 (New-FileCatalog,
    Set-AuthenticodeSignature) and the RosBE llvm-mingw toolchain.
#>
[CmdletBinding()]
param(
    [string]$Clang = (Join-Path $env:LOCALAPPDATA 'RosBE\pkgs\20261015\llvm-mingw\bin\clang.exe'),
    [string]$OutDir = (Join-Path $env:TEMP 'libernt-update-host'),
    [string]$KeyDir = (Join-Path $env:USERPROFILE '.leeway-dev-signing'),
    [switch]$SkipFixtures
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..')).Path
$lib = Join-Path $repo 'base\system\servicing\libservicing'
$cli = Join-Path $repo 'base\applications\cmdutils\libernt-update'
$cer = Join-Path $here 'certs\Leeway-LiberNT-Update-DEV.cer'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

if (-not (Test-Path (Join-Path $KeyDir 'Leeway-LiberNT-Update-DEV.pfx')) -or -not (Test-Path $cer)) {
    & (Join-Path $here 'New-DevSigningCert.ps1') -KeyDir $KeyDir
}

# ---- 1. COMPILATION ---------------------------------------------------------
Write-Host '=== COMPILATION ==='
$srcs = 'sha256','util','json','target','manifest','verify','journal','store','pending','servicing' | ForEach-Object { Join-Path $lib "$_.c" }
$srcs += (Join-Path $repo 'base\system\servicing\pending\s1core.c')    # the portable core shared with the boot applier
$common = @('--target=x86_64-w64-mingw32', '-O1', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-sign-compare', '-municode')
$libs = @('-lwintrust', '-lcrypt32', '-ladvapi32')
$compileOk = $true

function Invoke-Cc([string[]]$cargs, [string]$what) {
    $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $out = & $Clang @cargs 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev
    if ($code -ne 0) { Write-Host "FAILED: $what"; $out | ForEach-Object { Write-Host "  $_" }; return $false }
    $warn = @($out | Where-Object { "$_" -match 'warning:' })
    Write-Host ("ok: {0} ({1} warnings)" -f $what, $warn.Count)
    $warn | ForEach-Object { Write-Host "  $_" }
    return $true
}

$exe = Join-Path $OutDir 'libernt-update.exe'
$tst = Join-Path $OutDir 'libservicing-tests.exe'
if (-not (Invoke-Cc ($common + $srcs + @((Join-Path $cli 'main.c'), '-o', $exe) + $libs) 'libernt-update.exe')) { $compileOk = $false }
if (-not (Invoke-Cc ($common + @('-DLN_TESTING') + $srcs + @((Join-Path $lib 'tests\tests.c'), '-o', $tst) + $libs + @('-lole32')) 'libservicing-tests.exe (with the test hooks)')) { $compileOk = $false }
Write-Host ("COMPILATION: " + $(if ($compileOk) { 'OK' } else { 'FAILED' }))
if (-not $compileOk) { exit 1 }

# ---- 2. FIXTURES --------------------------------------------------------------
$fx = Join-Path $OutDir 'fixtures'
if (-not $SkipFixtures) {
    Write-Host '=== FIXTURES ==='
    $mk = Join-Path $here 'New-FixturePackage.ps1'
    $good = @(@{ Target = 'Windows\System32\lnfixture.dll'; Content = 'version one' })
    & $mk -OutDir "$fx\good1" -PackageId ln.fixture -Version 1.0 -Items $good -KeyDir $KeyDir | Out-Null
    & $mk -OutDir "$fx\tampered-payload" -PackageId ln.fixture -Version 1.0 -Items $good -KeyDir $KeyDir -TamperPayloadAfterSigning | Out-Null
    & $mk -OutDir "$fx\tampered-manifest" -PackageId ln.fixture -Version 1.0 -Items $good -KeyDir $KeyDir -TamperManifestAfterSigning | Out-Null
    & $mk -OutDir "$fx\tampered-sig" -PackageId ln.fixture -Version 1.0 -Items $good -KeyDir $KeyDir -TamperManifestSigAfterSigning | Out-Null
    & $mk -OutDir "$fx\no-manifest-sig" -PackageId ln.fixture -Version 1.0 -Items $good -KeyDir $KeyDir -NoManifestSig | Out-Null
    & $mk -OutDir "$fx\junction" -PackageId ln.junction -Version 1.0 -KeyDir $KeyDir -Items @(@{ Target = 'Windows\System32\jn\evil.dll'; Content = 'evil' }) | Out-Null
    & $mk -OutDir "$fx\driver-cat" -PackageId ln.driver -Version 1.0 -KeyDir $KeyDir -Items @(@{ Target = 'Windows\System32\drivers\lndrv.sys'; Content = 'driver'; DriverStart = 3 }) | Out-Null
    & $mk -OutDir "$fx\driver-nocat" -PackageId ln.driver -Version 1.0 -KeyDir $KeyDir -Unsigned -Items @(@{ Target = 'Windows\System32\drivers\lndrv.sys'; Content = 'driver'; DriverStart = 3 }) | Out-Null
    & $mk -OutDir "$fx\unsigned" -PackageId ln.fixture -Version 1.0 -Items $good -KeyDir $KeyDir -Unsigned | Out-Null
    & $mk -OutDir "$fx\unsigned-catalog" -PackageId ln.fixture -Version 1.0 -Items $good -KeyDir $KeyDir -UnsignedCatalog | Out-Null
    & $mk -OutDir "$fx\kernel" -PackageId ln.kernel -Version 1.0 -Items @(@{ Target = 'Windows\System32\ntoskrnl.exe'; Content = 'no' }) -KeyDir $KeyDir | Out-Null
    & $mk -OutDir "$fx\twoitems" -PackageId ln.twoitems -Version 1.0 -KeyDir $KeyDir -Items @(
        @{ Target = 'Windows\System32\first.dll'; Content = 'first new' },
        @{ Target = 'Windows\System32\blocked\second.dll'; Content = 'second' }) | Out-Null
    & $mk -OutDir "$fx\missing-payload" -PackageId ln.fixture -Version 1.0 -Items $good -KeyDir $KeyDir | Out-Null
    Remove-Item -Force (Join-Path $fx 'missing-payload\payload\f0.bin')

    # a valid signature by a certificate that is NOT the pinned one (throwaway, never stored)
    $tmpKey = Join-Path $env:TEMP ('lnwrong-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $tmpKey | Out-Null
    try {
        $c = New-SelfSignedCertificate -Subject 'CN=Not The LiberNT Dev Cert' -Type CodeSigningCert -CertStoreLocation 'Cert:\CurrentUser\My'
        $pw = 'throwaway'
        Export-PfxCertificate -Cert $c -FilePath "$tmpKey\w.pfx" -Password (ConvertTo-SecureString $pw -AsPlainText -Force) | Out-Null
        [IO.File]::WriteAllText("$tmpKey\pw.txt", $pw)
        Remove-Item -Path ("Cert:\CurrentUser\My\" + $c.Thumbprint) -DeleteKey -ErrorAction SilentlyContinue
        & $mk -OutDir "$fx\wrongcert" -PackageId ln.fixture -Version 1.0 -Items $good -SigningPfx "$tmpKey\w.pfx" -SigningPasswordFile "$tmpKey\pw.txt" | Out-Null
    }
    finally { Remove-Item -Recurse -Force $tmpKey -ErrorAction SilentlyContinue }
}

# ---- 3. TESTS -------------------------------------------------------------------
Write-Host '=== TESTS ==='
& $tst $fx $cer
$rc = $LASTEXITCODE
Write-Host ("TESTS: " + $(if ($rc -eq 0) { 'PASSED' } else { 'FAILED (exit ' + $rc + ')' }))
exit $rc
