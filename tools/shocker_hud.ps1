<#
  The Shocker's HUD: <hash>_shocker.png in the texture pack, which
  gpu_backend.c uses in place of <hash>.png while he is worn.

      powershell -ExecutionPolicy Bypass -File tools\shocker_hud.ps1

  Made from the pack's own images (tools/hud_upscale.py), so every piece
  keeps its size and its slots: the game still draws its health fill into the
  bar, and gpu_backend.c draws his heat into the slot the web fluid had.

    health bar   e_healthbar_2_rows / _1_row / _circle: the chrome warmed to
                 brass, the red orb his gold; around the lower bar (the heat
                 gauge) his gauntlet, banded, riveted, a knuckle block at the end
    compass      compas_spider_arrow, compass_glow_01/02: his mask, not the spider
    height meter alt_sp_001..003 red to his yellow, alt_dest_001..003 blue to brown
#>
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class ShockerArt
{
    public static readonly Color Gold = Color.FromArgb(255, 255, 200, 30);
    public static readonly Color Brown = Color.FromArgb(255, 163, 65, 0);

    delegate int PixelFn(int a, int r, int g, int b);

    static void Each(Bitmap bmp, PixelFn fn)
    {
        var rc = new Rectangle(0, 0, bmp.Width, bmp.Height);
        var d = bmp.LockBits(rc, ImageLockMode.ReadWrite, PixelFormat.Format32bppArgb);
        int n = d.Stride / 4 * bmp.Height;
        var px = new int[n];
        Marshal.Copy(d.Scan0, px, 0, n);
        for (int i = 0; i < n; i++) {
            int p = px[i];
            px[i] = fn((p >> 24) & 255, (p >> 16) & 255, (p >> 8) & 255, p & 255);
        }
        Marshal.Copy(px, 0, d.Scan0, n);
        bmp.UnlockBits(d);
    }

    static int Argb(int a, double r, double g, double b)
    {
        Func<double, int> c = v => v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5);
        return (a << 24) | (c(r) << 16) | (c(g) << 8) | c(b);
    }

    /* Red pixels to his yellow, by how red and how bright they are. */
    public static void RedToYellow(Bitmap bmp)
    {
        Each(bmp, (a, r, g, b) => {
            int red = r - Math.Max(g, b);
            if (red < 24) return Argb(a, r, g, b);
            double v = r / 255.0, k = Math.Min(1.0, red / 90.0);
            double hi = Math.Min(g, b) / 255.0;          /* the highlight left white */
            return Argb(a, r + (255 * v - r) * k + hi * 40,
                           g + ((200 * v + hi * 55) - g) * k,
                           b + ((30 * v + hi * 120) - b) * k);
        });
    }

    /* Blue pixels to brown. */
    public static void BlueToBrown(Bitmap bmp)
    {
        Each(bmp, (a, r, g, b) => {
            int blue = b - Math.Max(r, g);
            if (blue < 16) return Argb(a, r, g, b);
            double v = Math.Min(1.6, b / 110.0), k = Math.Min(1.0, blue / 50.0);
            return Argb(a, r + (163 * v - r) * k, g + (72 * v - g) * k, b + (12 * v - b) * k);
        });
    }

    /* The health bar: grey chrome to brass, the red orb gold. Black stays. */
    public static void Warm(Bitmap bmp)
    {
        Each(bmp, (a, r, g, b) => {
            int mx = Math.Max(r, Math.Max(g, b)), mn = Math.Min(r, Math.Min(g, b));
            if (r - Math.Max(g, b) >= 24) {                 /* the orb */
                double v = r / 255.0, hi = Math.Min(g, b) / 255.0;
                return Argb(a, 255 * Math.Min(1, v * 1.25) + hi * 30, 175 * v + hi * 80, 25 * v + hi * 60);
            }
            if (mx < 18 || mx - mn > 40) return Argb(a, r, g, b);
            double l = (r + g + b) / 3.0;                    /* chrome */
            return Argb(a, l * 1.18 + 10, l * 0.92 + 2, l * 0.45);
        });
    }

    static PointF P(float cx, float cy, float w, float h, float x, float y)
    {
        return new PointF(cx + x * w, cy + y * h);
    }

    /* His mask, centred on cx, cy, h tall: gold, the brown cap coming to a
     * point between the eyes, the quilting, white slanted eyes, outlined. */
    public static void Mask(Graphics g, float cx, float cy, float h)
    {
        float w = h * 0.76f, ol = Math.Max(1.5f, h * 0.014f), ln = Math.Max(1f, h * 0.0075f);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        var oval = new GraphicsPath();
        oval.AddEllipse(cx - w / 2, cy - h / 2, w, h);
        using (var br = new SolidBrush(Gold)) g.FillPath(br, oval);
        var clip = g.Clip;
        g.SetClip(oval, CombineMode.Intersect);

        var cap = new GraphicsPath();
        cap.AddLine(P(cx, cy, w, h, -0.60f, -0.60f), P(cx, cy, w, h, -0.60f, -0.33f));
        cap.AddBezier(P(cx, cy, w, h, -0.60f, -0.33f), P(cx, cy, w, h, -0.20f, -0.31f),
                      P(cx, cy, w, h, -0.06f, -0.24f), P(cx, cy, w, h, 0.0f, -0.15f));
        cap.AddBezier(P(cx, cy, w, h, 0.0f, -0.15f), P(cx, cy, w, h, 0.06f, -0.25f),
                      P(cx, cy, w, h, 0.20f, -0.33f), P(cx, cy, w, h, 0.60f, -0.35f));
        cap.AddLine(P(cx, cy, w, h, 0.60f, -0.35f), P(cx, cy, w, h, 0.60f, -0.60f));
        cap.CloseFigure();
        using (var br = new SolidBrush(Brown)) g.FillPath(br, cap);

        using (var pen = new Pen(Color.Black, ln)) {
            pen.StartCap = pen.EndCap = LineCap.Round;
            g.DrawLine(pen, P(cx, cy, w, h, -0.13f, -0.25f), P(cx, cy, w, h, 0.24f, 0.44f));
            g.DrawLine(pen, P(cx, cy, w, h, 0.09f, -0.24f), P(cx, cy, w, h, -0.21f, 0.46f));
            g.DrawBezier(pen, P(cx, cy, w, h, -0.33f, -0.30f), P(cx, cy, w, h, -0.44f, -0.05f),
                              P(cx, cy, w, h, -0.40f, 0.20f), P(cx, cy, w, h, -0.30f, 0.38f));
            g.DrawBezier(pen, P(cx, cy, w, h, 0.33f, -0.31f), P(cx, cy, w, h, 0.45f, -0.05f),
                              P(cx, cy, w, h, 0.40f, 0.20f), P(cx, cy, w, h, 0.28f, 0.40f));
            g.DrawBezier(pen, P(cx, cy, w, h, -0.13f, -0.25f), P(cx, cy, w, h, -0.30f, -0.20f),
                              P(cx, cy, w, h, -0.42f, -0.12f), P(cx, cy, w, h, -0.50f, -0.04f));
            g.DrawBezier(pen, P(cx, cy, w, h, 0.09f, -0.24f), P(cx, cy, w, h, 0.25f, -0.18f),
                              P(cx, cy, w, h, 0.40f, -0.06f), P(cx, cy, w, h, 0.50f, 0.03f));
            g.DrawBezier(pen, P(cx, cy, w, h, -0.20f, -0.29f), P(cx, cy, w, h, -0.27f, -0.05f),
                              P(cx, cy, w, h, -0.26f, 0.20f), P(cx, cy, w, h, -0.20f, 0.45f));
            g.DrawBezier(pen, P(cx, cy, w, h, 0.20f, -0.30f), P(cx, cy, w, h, 0.28f, -0.05f),
                              P(cx, cy, w, h, 0.27f, 0.20f), P(cx, cy, w, h, 0.22f, 0.45f));
        }
        using (var pen = new Pen(Color.Black, ln * 1.4f)) g.DrawPath(pen, cap);
        g.Clip = clip;

        using (var pen = new Pen(Color.Black, ol * 0.8f)) {
            pen.LineJoin = LineJoin.Round;
            for (int side = -1; side <= 1; side += 2) {
                var eye = new PointF[] {
                    P(cx, cy, w, h, side * 0.305f, -0.090f), P(cx, cy, w, h, side * 0.090f, 0.012f),
                    P(cx, cy, w, h, side * 0.130f, 0.072f), P(cx, cy, w, h, side * 0.255f, -0.020f) };
                g.FillPolygon(Brushes.White, eye);
                g.DrawPolygon(pen, eye);
            }
        }
        using (var pen = new Pen(Color.Black, ol)) g.DrawPath(pen, oval);
    }

    /* Clear a disc to transparent, or fill it with a colour. */
    public static void Disc(Bitmap bmp, float cx, float cy, float r, Color fill, float keepTopY, float keepHalfW)
    {
        var rc = new Rectangle(0, 0, bmp.Width, bmp.Height);
        var d = bmp.LockBits(rc, ImageLockMode.ReadWrite, PixelFormat.Format32bppArgb);
        int n = d.Stride / 4 * bmp.Height, w = d.Stride / 4;
        var px = new int[n];
        Marshal.Copy(d.Scan0, px, 0, n);
        for (int y = 0; y < bmp.Height; y++)
            for (int x = 0; x < bmp.Width; x++) {
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                if (dx * dx + dy * dy > r * r) continue;
                if (y < keepTopY && Math.Abs(dx) < keepHalfW) continue;   /* the pointer */
                px[y * w + x] = fill.ToArgb();
            }
        Marshal.Copy(px, 0, d.Scan0, n);
        bmp.UnlockBits(d);
    }

    static GraphicsPath Round(float x, float y, float w, float h, float r)
    {
        var p = new GraphicsPath();
        p.AddArc(x, y, r * 2, r * 2, 180, 90);
        p.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
        p.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
        p.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
        p.CloseFigure();
        return p;
    }

    static void Plate(Graphics g, float x, float y, float w, float h, float r)
    {
        using (var path = Round(x, y, w, h, r))
        using (var br = new LinearGradientBrush(new RectangleF(x, y - 1, w, h + 2),
                   Color.FromArgb(255, 238, 196, 92), Color.FromArgb(255, 92, 52, 10), 90f)) {
            var blend = new ColorBlend(4);
            blend.Colors = new[] { Color.FromArgb(255, 250, 222, 130), Color.FromArgb(255, 214, 156, 40),
                                   Color.FromArgb(255, 150, 92, 18), Color.FromArgb(255, 70, 36, 6) };
            blend.Positions = new[] { 0f, 0.35f, 0.7f, 1f };
            br.InterpolationColors = blend;
            g.FillPath(br, path);
            using (var pen = new Pen(Color.FromArgb(255, 30, 14, 2), 2.2f)) g.DrawPath(pen, path);
        }
    }

    /* The gauntlet alone, red-hot, with a glow round it: gpu_backend.c fades
     * it in over the bar as his heat rises (shocker_heat.png). */
    public static Bitmap Hot()
    {
        var hot = new Bitmap(1024, 256, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(hot)) g.Clear(Color.Transparent);
        Gauntlet(hot);
        Each(hot, (a, r, g, b) => {
            if (a == 0) return 0;
            double l = (0.3 * r + 0.59 * g + 0.11 * b) / 255.0;     /* metal's light to heat */
            double R = 150 + 105 * Math.Min(1, l * 1.6), G = 20 + 230 * Math.Pow(l, 1.6), B = 10 + 170 * Math.Pow(l, 3);
            return Argb(a, R, G, B);
        });
        /* The glow: the gauntlet's shape, blurred by scaling it down and up. */
        var small = new Bitmap(128, 32, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(small)) {
            g.InterpolationMode = InterpolationMode.HighQualityBilinear;
            g.DrawImage(hot, 0, 0, 128, 32);
        }
        var glow = new Bitmap(1024, 256, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(glow)) {
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.DrawImage(small, -8, -8, 1040, 272);
        }
        small.Dispose();
        Each(glow, (a, r, g, b) => Argb(Math.Min(255, a * 2), 255, 90, 10));
        using (var g = Graphics.FromImage(glow)) g.DrawImage(hot, 0, 0, 1024, 256);
        hot.Dispose();
        /* The slot stays clear: the heat cells are drawn there. */
        Disc(glow, -1, -1, 0, Color.Transparent, 0, 0);
        var rc = new Rectangle(280, 177, 248, 21);
        using (var g = Graphics.FromImage(glow)) {
            g.CompositingMode = CompositingMode.SourceCopy;
            using (var br = new SolidBrush(Color.Transparent)) g.FillRectangle(br, rc);
        }
        return glow;
    }

    /* His gauntlet round the lower bar of e_healthbar_2_rows (1024x256 pack
     * image): a banded cuff over and under the slot, rivets, and a knuckle
     * block over the bar's slanted end. The slot itself (x 280..528,
     * y 176..198) is left as it is: gpu_backend.c draws the heat there. */
    public static void Gauntlet(Bitmap bmp)
    {
        using (var g = Graphics.FromImage(bmp)) {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            float x0 = 258, x1 = 552;
            Plate(g, x0, 164, x1 - x0, 13, 5);              /* over the slot */
            Plate(g, x0, 198, x1 - x0, 15, 5);              /* under it */
            using (var dark = new Pen(Color.FromArgb(255, 60, 30, 4), 2f))
            using (var lite = new Pen(Color.FromArgb(200, 255, 236, 160), 1f)) {
                for (float x = x0 + 22; x < x1 - 30; x += 30) {   /* bands */
                    g.DrawLine(dark, x, 165, x, 176);
                    g.DrawLine(lite, x + 2, 166, x + 2, 175);
                    g.DrawLine(dark, x, 199, x, 212);
                    g.DrawLine(lite, x + 2, 200, x + 2, 211);
                }
            }
            for (float x = x0 + 9; x < x1 - 20; x += 60) {      /* rivets */
                foreach (float y in new[] { 170.5f, 205.5f }) {
                    using (var br = new SolidBrush(Color.FromArgb(255, 70, 40, 8))) g.FillEllipse(br, x - 3.5f, y - 3.5f, 7, 7);
                    using (var br = new SolidBrush(Color.FromArgb(255, 255, 232, 150))) g.FillEllipse(br, x - 2f, y - 2.5f, 3, 3);
                }
            }
            /* Cuff at the wrist end, over the orb's edge. */
            Plate(g, 246, 158, 30, 62, 7);
            /* The knuckle block over the slanted end, quilted like his suit. */
            var block = Round(522, 150, 112, 74, 14);
            Plate(g, 522, 150, 112, 74, 14);
            var clip = g.Clip;
            g.SetClip(block, CombineMode.Intersect);
            using (var pen = new Pen(Color.FromArgb(255, 110, 44, 0), 2.2f)) {
                for (float k = -80; k < 200; k += 22) {
                    g.DrawLine(pen, 522 + k, 150, 522 + k + 74, 224);
                    g.DrawLine(pen, 522 + k + 74, 150, 522 + k, 224);
                }
            }
            g.Clip = clip;
            using (var pen = new Pen(Color.FromArgb(255, 30, 14, 2), 2.4f)) g.DrawPath(pen, block);
            for (int i = 0; i < 3; i++)                      /* knuckles */
                Plate(g, 618, 156 + i * 22, 26, 18, 8);
            /* The shock emitter on the back of the hand. */
            using (var br = new SolidBrush(Color.FromArgb(255, 40, 22, 4))) g.FillEllipse(br, 556, 170, 34, 34);
            using (var br = new LinearGradientBrush(new RectangleF(560, 174, 26, 26),
                       Color.FromArgb(255, 255, 250, 200), Color.FromArgb(255, 255, 150, 20), 60f))
                g.FillEllipse(br, 560, 174, 26, 26);
            using (var pen = new Pen(Color.FromArgb(255, 30, 14, 2), 2f)) g.DrawEllipse(pen, 560, 174, 26, 26);
        }
    }
}
'@

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$pack = Join-Path $root 'build\Release\texpack'
$outs = @((Join-Path $root 'texpack'), $pack)
$src = Join-Path $root 'cap\shocker_hud'

function Load($hash) {
    $p = Join-Path $pack "$hash.png"
    if (-not (Test-Path $p)) { $p = Join-Path $src "${hash}_src.png" }
    $i = [System.Drawing.Image]::FromFile($p)
    $b = New-Object System.Drawing.Bitmap $i.Width, $i.Height, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($b); $g.DrawImage($i, 0, 0, $i.Width, $i.Height); $g.Dispose(); $i.Dispose()
    return $b
}
function Save($b, $hash) {
    foreach ($d in $outs) { New-Item -ItemType Directory -Force $d | Out-Null; $b.Save((Join-Path $d "${hash}_shocker.png"), [System.Drawing.Imaging.ImageFormat]::Png) }
    "  ${hash}_shocker.png $($b.Width)x$($b.Height)"
    $b.Dispose()
}

# Health bar: two rows (health over the heat gauge), one row, the ring.
$b = Load '435057E8'; [ShockerArt]::Warm($b); [ShockerArt]::Gauntlet($b); Save $b '435057E8'
# The gauntlet red-hot, laid over that bar as he heats up (not a pack image:
# no hash in its name).
$b = [ShockerArt]::Hot()
foreach ($d in $outs) { $b.Save((Join-Path $d 'shocker_heat.png'), [System.Drawing.Imaging.ImageFormat]::Png) }
"  shocker_heat.png"; $b.Dispose()
$b = Load 'D14A7147'; [ShockerArt]::Warm($b); Save $b 'D14A7147'
$b = Load 'A45114FA'; [ShockerArt]::Warm($b); Save $b 'A45114FA'

# Compass: the arrow disc (512, its pointer at the top kept) and both glows.
$b = Load '8C8B650E'
[ShockerArt]::Disc($b, 256, 256, 210, [System.Drawing.Color]::Transparent, 84, 96)
$g = [System.Drawing.Graphics]::FromImage($b); [ShockerArt]::Mask($g, 256, 272, 330); $g.Dispose()
Save $b '8C8B650E'
foreach ($h in '4B624D90', '75521338') {
    $b = Load $h
    $grey = $b.GetPixel(128, 30)
    foreach ($p in @(@(128, 225), @(40, 128), @(216, 128))) { $c = $b.GetPixel($p[0], $p[1]); if ($c.GetBrightness() -gt $grey.GetBrightness()) { $grey = $c } }
    [ShockerArt]::Disc($b, 128, 128, 102, $grey, 0, 0)
    $g = [System.Drawing.Graphics]::FromImage($b); [ShockerArt]::Mask($g, 128, 130, 168); $g.Dispose()
    Save $b $h
}

# Height meter: his yellow for the red, brown for the blue.
foreach ($h in '7DD7A164', '1B0817D1', '7EDB6DC5') { $b = Load $h; [ShockerArt]::RedToYellow($b); Save $b $h }
foreach ($h in 'F1AC7DC5', '71AC7DC5', '3BAC7DC5') { $b = Load $h; [ShockerArt]::BlueToBrown($b); Save $b $h }
