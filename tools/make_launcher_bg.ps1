<#
  assets\launcher_bg.bmp: the launcher's background, made here from nothing
  (no game art): a dark red dusk over a city skyline in silhouette, darkening
  to the bottom where the launcher's controls sit. 800x800, 24-bit.

      powershell -ExecutionPolicy Bypass -File tools\make_launcher_bg.ps1
#>
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$out = Join-Path (Split-Path -Parent $PSScriptRoot) 'assets\launcher_bg.bmp'
$W = 800; $H = 800
$b = New-Object System.Drawing.Bitmap $W, $H, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$g = [System.Drawing.Graphics]::FromImage($b)
$g.SmoothingMode = 'AntiAlias'

# Sky: deep red at the horizon to near black above.
$sky = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.Rectangle 0, 0, $W, 520), ([System.Drawing.Color]::FromArgb(255, 10, 6, 12)), ([System.Drawing.Color]::FromArgb(255, 120, 18, 22)), 90
$g.FillRectangle($sky, 0, 0, $W, 520)
$g.FillRectangle((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 120, 18, 22))), 0, 520, $W, $H - 520)

# A low glow behind the skyline.
$glow = New-Object System.Drawing.Drawing2D.GraphicsPath
$glow.AddEllipse(-200, 360, $W + 400, 320)
$pg = New-Object System.Drawing.Drawing2D.PathGradientBrush $glow
$pg.CenterColor = [System.Drawing.Color]::FromArgb(150, 230, 70, 40)
$pg.SurroundColors = @([System.Drawing.Color]::FromArgb(0, 230, 70, 40))
$g.FillPath($pg, $glow)

# Stars, few and faint.
$rnd = New-Object System.Random 7
for ($i = 0; $i -lt 90; $i++) {
    $x = $rnd.Next(0, $W); $y = $rnd.Next(0, 300); $a = $rnd.Next(40, 160)
    $g.FillRectangle((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb($a, 255, 230, 220))), $x, $y, 1, 1)
}

# Two rows of buildings: a far one, lighter, and a near one, black, with lit windows.
function Skyline($base, $minH, $maxH, $color, $windows) {
    $x = -10
    while ($x -lt $W + 10) {
        $bw = $rnd.Next(28, 70); $bh = $rnd.Next($minH, $maxH)
        $g.FillRectangle((New-Object System.Drawing.SolidBrush $color), $x, $base - $bh, $bw, $bh + 400)
        if ($rnd.Next(0, 4) -eq 0) {   # a spire
            $g.FillRectangle((New-Object System.Drawing.SolidBrush $color), $x + $bw / 2 - 2, $base - $bh - 30, 4, 30)
        }
        if ($windows) {
            for ($wy = $base - $bh + 8; $wy -lt $base - 6; $wy += 9) {
                for ($wx = $x + 5; $wx -lt $x + $bw - 6; $wx += 8) {
                    if ($rnd.Next(0, 9) -eq 0) {
                        $g.FillRectangle((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(220, 255, 196, 120))), $wx, $wy, 3, 4)
                    }
                }
            }
        }
        $x += $bw + $rnd.Next(0, 6)
    }
}
Skyline 560 60 240 ([System.Drawing.Color]::FromArgb(255, 54, 12, 18)) $false
Skyline 620 40 300 ([System.Drawing.Color]::FromArgb(255, 8, 6, 10)) $true

# The bottom, under the launcher's controls: near black.
$fade = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.Rectangle 0, 560, $W, 240), ([System.Drawing.Color]::FromArgb(0, 0, 0, 0)), ([System.Drawing.Color]::FromArgb(255, 6, 4, 8)), 90
$g.FillRectangle($fade, 0, 560, $W, 240)
$g.FillRectangle((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 6, 4, 8))), 0, 798, $W, 2)

$g.Dispose()
$b.Save($out, [System.Drawing.Imaging.ImageFormat]::Bmp)
$b.Dispose()
"wrote $out"
