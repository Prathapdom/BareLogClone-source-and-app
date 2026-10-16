param([string]$Destination = (Join-Path $PSScriptRoot 'Assets'))
# Draws the BareLog icon: log lines on a dark tile, the newest line highlighted, with a "follow" arrow.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Path $Destination -Force | Out-Null

function New-IconBitmap([int]$size) {
    $bmp = New-Object Drawing.Bitmap($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([Drawing.Color]::Transparent)
    $s = [single]$size
    $r = [single]([Math]::Max(2, $s * 0.2))
    $path = New-Object Drawing.Drawing2D.GraphicsPath
    $m = [single]($s * 0.04); $w = $s - 2 * $m
    $path.AddArc($m, $m, $r, $r, 180, 90); $path.AddArc($m + $w - $r, $m, $r, $r, 270, 90)
    $path.AddArc($m + $w - $r, $m + $w - $r, $r, $r, 0, 90); $path.AddArc($m, $m + $w - $r, $r, $r, 90, 90)
    $path.CloseFigure()
    $bg = New-Object Drawing.Drawing2D.LinearGradientBrush((New-Object Drawing.PointF(0, 0)), (New-Object Drawing.PointF(0, $s)), [Drawing.ColorTranslator]::FromHtml('#26303F'), [Drawing.ColorTranslator]::FromHtml('#121821'))
    $g.FillPath($bg, $path)
    $thick = [single]([Math]::Max(1.5, $s * 0.075))
    $grey = New-Object Drawing.Pen([Drawing.ColorTranslator]::FromHtml('#8A97A8'), $thick)
    $grey.StartCap = $grey.EndCap = [Drawing.Drawing2D.LineCap]::Round
    $accent = New-Object Drawing.Pen([Drawing.ColorTranslator]::FromHtml('#4CC2FF'), $thick)
    $accent.StartCap = $accent.EndCap = [Drawing.Drawing2D.LineCap]::Round
    $warn = New-Object Drawing.Pen([Drawing.ColorTranslator]::FromHtml('#FFC43C'), $thick)
    $warn.StartCap = $warn.EndCap = [Drawing.Drawing2D.LineCap]::Round
    $x0 = [single]($s * 0.22)
    $g.DrawLine($grey, $x0, [single]($s * 0.27), [single]($s * 0.70), [single]($s * 0.27))
    $g.DrawLine($warn, $x0, [single]($s * 0.42), [single]($s * 0.56), [single]($s * 0.42))
    $g.DrawLine($grey, $x0, [single]($s * 0.57), [single]($s * 0.64), [single]($s * 0.57))
    $g.DrawLine($accent, $x0, [single]($s * 0.74), [single]($s * 0.50), [single]($s * 0.74))
    # Follow arrow on the right, pointing down at the newest line.
    $ax = [single]($s * 0.76)
    $g.DrawLine($accent, $ax, [single]($s * 0.40), $ax, [single]($s * 0.76))
    $g.DrawLine($accent, [single]($ax - $s * 0.11), [single]($s * 0.65), $ax, [single]($s * 0.76))
    $g.DrawLine($accent, [single]($ax + $s * 0.11), [single]($s * 0.65), $ax, [single]($s * 0.76))
    $grey.Dispose(); $accent.Dispose(); $warn.Dispose(); $bg.Dispose(); $path.Dispose(); $g.Dispose()
    return $bmp
}

# Multi-resolution ICO with PNG-compressed entries (supported since Windows Vista).
$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$images = foreach ($size in $sizes) {
    $bmp = New-IconBitmap $size
    $ms = New-Object IO.MemoryStream
    $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png)
    if ($size -eq 256) { $bmp.Save((Join-Path $Destination 'logo-256.png'), [Drawing.Imaging.ImageFormat]::Png) }
    $bmp.Dispose()
    , $ms.ToArray()
}
$out = New-Object IO.MemoryStream
$bw = New-Object IO.BinaryWriter($out)
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$images[$i].Length); $bw.Write([UInt32]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $bw.Write($img) }
$bw.Flush()
[IO.File]::WriteAllBytes((Join-Path $Destination 'app.ico'), $out.ToArray())
$bw.Dispose()
