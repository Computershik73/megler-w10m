# Draws the package's logos into app/Megler/Assets: Megler's mark, a white M of
# round strokes with a green dot under its middle, on the black the tiles and
# the splash screen have. Run once; the PNGs are committed.
param(
    [string]$OutDir = $PSScriptRoot
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

# The mark in a square of side 1: the M's stroke, and the dot.
$strokePoints = @(@(0.14, 0.80), @(0.14, 0.20), @(0.50, 0.60), @(0.86, 0.20), @(0.86, 0.80))
$strokeWidth = 0.15
$dot = @(0.50, 0.86, 0.075)
$green = [System.Drawing.Color]::FromArgb(255, 0x3D, 0xDC, 0x84)

function Write-Logo([string]$name, [int]$width, [int]$height, [double]$markFraction) {
    $bmp = New-Object System.Drawing.Bitmap $width, $height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Black)
    $side = [Math]::Min($width, $height) * $markFraction
    $x0 = ($width - $side) / 2
    $y0 = ($height - $side) / 2
    $points = [System.Drawing.PointF[]]($strokePoints | ForEach-Object {
        New-Object System.Drawing.PointF ([single]($x0 + $_[0] * $side)), ([single]($y0 + $_[1] * $side))
    })
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), ([single]($strokeWidth * $side))
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $g.DrawLines($pen, $points)
    $pen.Dispose()
    $r = $dot[2] * $side
    $brush = New-Object System.Drawing.SolidBrush $green
    $g.FillEllipse($brush, [single]($x0 + $dot[0] * $side - $r), [single]($y0 + $dot[1] * $side - $r), [single](2 * $r), [single](2 * $r))
    $brush.Dispose()
    $g.Dispose()
    $bmp.Save((Join-Path $OutDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Output "  $name ${width}x${height}"
}

Write-Logo 'StoreLogo.png' 50 50 0.84
Write-Logo 'Square44x44Logo.png' 44 44 0.84
Write-Logo 'Square150x150Logo.png' 150 150 0.56
Write-Logo 'Wide310x150Logo.png' 310 150 0.56
Write-Logo 'SplashScreen.png' 620 300 0.5
