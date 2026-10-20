# Signs an appx package (podium-w10m's and gecko-w10m's tools/sign-appx.ps1, for Meglet).
#
# Signing lives in PowerShell rather than in build-appx.sh because msys2's bash
# hands its children an environment with TEMP and TMP empty; signtool unpacks
# and repacks an appx through a temp directory, and without one it fails with
# "This file format cannot be signed because it is not recognized" -- which
# reads like a corrupt package but is a missing temp dir.
#
# The signer's subject must be exactly the manifest's Publisher (CN=Computershik)
# or the package won't install, so a certificate with another subject is
# refused here rather than on the phone. -Certificate is used when it exists
# (tools/build-appx.sh picks one the user's phones already trust: Meglet's own,
# else Podium's, else Gecko's); only when it does not exist is a self-signed
# certificate made for that Publisher and exported there.
#
# -CerOut receives the public half of whatever signed the package: the
# certificate a phone has to trust before it installs it.
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [Parameter(Mandatory = $true)][string]$Certificate,
    [string]$Subject = '',
    [string]$Password = $(if ($env:MEGLET_PFX_PASSWORD) { $env:MEGLET_PFX_PASSWORD } else { 'meglet' }),
    [string]$CerOut = '',
    [string]$SdkBin = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64'
)

$ErrorActionPreference = 'Stop'

# The first temp directory we can actually write to.
$candidates = New-Object System.Collections.ArrayList
foreach ($v in @($env:TEMP, $env:TMP)) { if ($v) { [void]$candidates.Add($v) } }
if ($env:LOCALAPPDATA) { [void]$candidates.Add((Join-Path $env:LOCALAPPDATA 'Temp')) }
[void]$candidates.Add((Join-Path (Split-Path -Parent $Package) 'signtemp'))
$temp = $null
foreach ($c in $candidates) {
    try {
        if (-not (Test-Path $c)) { New-Item -ItemType Directory -Path $c -Force | Out-Null }
        $probe = Join-Path $c ([System.IO.Path]::GetRandomFileName())
        [System.IO.File]::WriteAllText($probe, 'x')
        Remove-Item $probe -Force
        $temp = $c
        break
    } catch { }
}
if (-not $temp) { throw 'no writable temp directory found' }
$env:TEMP = $temp
$env:TMP = $temp

$signtool = Join-Path $SdkBin 'signtool.exe'
if (-not (Test-Path $signtool)) { throw "signtool not found at $signtool" }
if (-not (Test-Path $Package)) { throw "package not found: $Package" }

if (-not (Test-Path $Certificate)) {
    if (-not $Subject) { throw "no certificate at $Certificate, and no -Subject to make one for" }
    Write-Output "    creating a self-signed certificate ($Subject)"
    $cert = New-SelfSignedCertificate -Type Custom -Subject $Subject `
        -KeyUsage DigitalSignature -FriendlyName 'Meglet W10M' `
        -CertStoreLocation 'Cert:\CurrentUser\My' `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
    $secure = ConvertTo-SecureString -String $Password -Force -AsPlainText
    Export-PfxCertificate -Cert $cert -FilePath $Certificate -Password $secure | Out-Null
    Write-Output "    thumbprint $($cert.Thumbprint); exported $Certificate"
}

# Read from the file, never imported into a store.
$signer = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($Certificate, $Password)
if ($Subject -and $signer.Subject -ne $Subject) {
    throw "the certificate's subject is '$($signer.Subject)', the manifest's Publisher is '$Subject': the package would not install"
}
Write-Output "    signer $($signer.Subject), thumbprint $($signer.Thumbprint), valid to $($signer.NotAfter.ToString('yyyy-MM-dd'))"

# Start-Process with explicit redirection: from a nested msys2 bash the call
# operator's stdout can vanish, taking signtool's diagnostics with it.
$outFile = Join-Path $env:TEMP 'meglet-signtool-out.txt'
$errFile = Join-Path $env:TEMP 'meglet-signtool-err.txt'
$proc = Start-Process -FilePath $signtool -Wait -NoNewWindow -PassThru `
    -ArgumentList @('sign', '/fd', 'SHA256', '/f', "`"$Certificate`"", '/p', $Password, "`"$Package`"") `
    -RedirectStandardOutput $outFile -RedirectStandardError $errFile
foreach ($f in @($outFile, $errFile)) {
    if (Test-Path $f) {
        Get-Content $f | Where-Object { $_.Trim() } | ForEach-Object { Write-Output "    | $_" }
        Remove-Item $f -Force
    }
}
if ($proc.ExitCode -ne 0) { throw "signtool exited with $($proc.ExitCode)" }

# Ask the package itself rather than trusting an exit code through bash: a
# signed appx carries AppxSignature.p7x.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($Package)
try {
    $signed = $null -ne ($zip.Entries | Where-Object { $_.FullName -eq 'AppxSignature.p7x' })
} finally {
    $zip.Dispose()
}
if (-not $signed) { throw "package is not signed: $Package" }
Write-Output "    signature present (AppxSignature.p7x)"

if ($CerOut) {
    $bytes = $signer.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert)
    [System.IO.File]::WriteAllBytes($CerOut, $bytes)
    Write-Output "    certificate to trust: $CerOut"
}
