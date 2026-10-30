<#
.SYNOPSIS
    Builds (and signs) a LiberNT Update package: manifest.json, payload\, catalog.cat.

.DESCRIPTION
    The package layout is the S1 contract: manifest.json + catalog.cat + payload\. The catalog covers
    manifest.json and every payload file (CatalogVersion 2, SHA-256) and is signed with the
    DEVELOPMENT certificate from KeyDir (or with -SigningPfx for negative tests).

    Items: array of hashtables
        @{ Target = 'Windows\System32\foo.dll'; Content = 'text' }      replace/create with that content
        @{ Target = 'Windows\System32\old.dll'; Delete = $true }        delete

    Switches for negative fixtures:
        -Unsigned          no catalog.cat at all
        -UnsignedCatalog   a catalog that is not signed
        -SigningPfx/-SigningPasswordFile   sign with another certificate (the manifest then names that one)
        -TamperPayloadAfterSigning        change a payload byte after signing (payload hash no longer matches)
        -TamperManifestAfterSigning       rewrite manifest.json after signing (catalog no longer covers it)
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutDir,
    [Parameter(Mandatory)][string]$PackageId,
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][hashtable[]]$Items,
    [string]$Type = 'system-component',
    [string]$KeyDir = (Join-Path $env:USERPROFILE '.leeway-dev-signing'),
    [string]$SigningPfx,
    [string]$SigningPasswordFile,
    [switch]$Unsigned,
    [switch]$UnsignedCatalog,
    [switch]$TamperPayloadAfterSigning,
    [switch]$TamperManifestAfterSigning
)

$ErrorActionPreference = 'Stop'
if (-not $SigningPfx) { $SigningPfx = Join-Path $KeyDir 'Leeway-LiberNT-Update-DEV.pfx' }
if (-not $SigningPasswordFile) { $SigningPasswordFile = Join-Path $KeyDir 'pfx-password.txt' }
$utf8 = New-Object Text.UTF8Encoding($false)

if (Test-Path $OutDir) { Remove-Item -Recurse -Force $OutDir }
New-Item -ItemType Directory -Force -Path (Join-Path $OutDir 'payload') | Out-Null

$cert = New-Object Security.Cryptography.X509Certificates.X509Certificate2($SigningPfx, ([IO.File]::ReadAllText($SigningPasswordFile)))
$certSha = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($cert.RawData)).Replace('-', '').ToLower()

$payloads = @()
$n = 0
foreach ($it in $Items) {
    if ($it.Delete) {
        $payloads += [ordered]@{ action = 'delete'; target = $it.Target }
        continue
    }
    $rel = ('f{0}.bin' -f $n++)
    $file = Join-Path $OutDir ('payload\' + $rel)
    [IO.File]::WriteAllText($file, [string]$it.Content, $utf8)
    $sha = (Get-FileHash -Algorithm SHA256 -Path $file).Hash.ToLower()
    $payloads += [ordered]@{ path = $rel; target = $it.Target; sha256 = $sha }
}

$manifest = [ordered]@{
    schemaVersion     = 1
    packageId         = $PackageId
    releaseVersion    = $Version
    type              = $Type
    targetFamily      = 'LiberNT'
    architecture      = 'amd64'
    certificateSha256 = $certSha
    payloads          = $payloads
}
$manifestPath = Join-Path $OutDir 'manifest.json'
[IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 6), $utf8)

if (-not $Unsigned) {
    $tmpCat = Join-Path ([IO.Path]::GetTempPath()) ([Guid]::NewGuid().ToString('N') + '.cat')
    New-FileCatalog -Path $OutDir -CatalogFilePath $tmpCat -CatalogVersion 2 | Out-Null
    Move-Item $tmpCat (Join-Path $OutDir 'catalog.cat')
    if (-not $UnsignedCatalog) {
        Set-AuthenticodeSignature -FilePath (Join-Path $OutDir 'catalog.cat') -Certificate $cert -HashAlgorithm SHA256 | Out-Null
    }
}

if ($TamperPayloadAfterSigning) {
    $first = Get-ChildItem (Join-Path $OutDir 'payload') | Select-Object -First 1
    [IO.File]::AppendAllText($first.FullName, 'X', $utf8)
}
if ($TamperManifestAfterSigning) {
    $text = [IO.File]::ReadAllText($manifestPath)
    [IO.File]::WriteAllText($manifestPath, $text.Replace($Version, $Version + '1'), $utf8)
}

Write-Host "Package '$PackageId' $Version written to $OutDir (certificate sha256 $certSha)"
