# Draws the package's logos into app/Megler/Assets from Megler's own icon (the
# iPhone app's dark variant, in the Megler repository beside this one), on the
# black the tiles and the splash screen have. Run once; the PNGs are committed.
param(
    [string]$Source = (Join-Path $PSScriptRoot '..\..\..\..\Megler\src\app\Megler\Assets.xcassets\AppIcon.appiconset\Icon-Dark.png'),
    [string]$OutDir = $PSScriptRoot
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
if (-not (Test-Path $Source)) { throw "Megler's icon not found at $Source (pass -Source)" }
$icon = [System.Drawing.Image]::FromFile((Resolve-Path $Source))

function Write-Logo([string]$name, [int]$width, [int]$height, [double]$iconFraction) {
    $bmp = New-Object System.Drawing.Bitmap $width, $height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Black)
    $side = [single]([Math]::Min($width, $height) * $iconFraction)
    $x = [single](($width - $side) / 2)
    $y = [single](($height - $side) / 2)
    $g.DrawImage($icon, $x, $y, $side, $side)
    $g.Dispose()
    $bmp.Save((Join-Path $OutDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Output "  $name ${width}x${height}"
}

Write-Logo 'StoreLogo.png' 50 50 0.92
Write-Logo 'Square44x44Logo.png' 44 44 0.92
Write-Logo 'Square150x150Logo.png' 150 150 0.66
Write-Logo 'Wide310x150Logo.png' 310 150 0.66
Write-Logo 'SplashScreen.png' 620 300 0.6
$icon.Dispose()
