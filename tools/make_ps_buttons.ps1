# Build assets\ps5_buttons.bmp / ps4_buttons.bmp: the game's button-prompt strip with
# PlayStation buttons -- white symbols as on the DualSense (-Style ps5), or the
# PS4's coloured ones (-Style ps4).
#
#   .\tools\make_ps_buttons.ps1 -Source <the strip as dumped by RECOMP_GPU_TEXDUMP> -Style ps5
#
# The source is the 512x64 strip the title draws every prompt from (texels hash
# 73E8ECF1, dumped as 73E8ECF1_512x64_fmt0F.bmp): 32-bit top-down BMP, alpha
# kept. Cells, left to right (x ranges from its alpha): Y-with-arrows 2-31,
# R trigger 40-71, A 83-106, X 120-143, Y 157-180, B 194-217, L trigger 228-260,
# White 272-295, Black 309-332, then four d-pad arrows (left as they are).
# xinput_device.c puts the left shoulder on Black and the right on White, so
# Black is L1 and White is R1. The output has the same size and layout;
# gpu_backend.c swaps it in for the original when Button prompts are set to
# PlayStation 5 / PlayStation 4.
#
# A source that is a PNG -- the strip upscaled for the texture pack, e.g. 2048x256
# -- is drawn over at its own scale and written as a PNG of that size:
#
#   .\tools\make_ps_buttons.ps1 -Source texpack\73E8ECF1.png -Style ps5 -Out texpack\73E8ECF1_ps5.png
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [ValidateSet("ps5", "ps4")][string]$Style = "ps5",
    [string]$Out
)
# ($PSScriptRoot is not set yet in param defaults under Windows PowerShell.)
if (-not $Out) { $Out = Join-Path $PSScriptRoot "..\assets\${Style}_buttons.bmp" }
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.IO;
using System.Runtime.InteropServices;

public static class PsButtons {
    const int W = 512, H = 64;

    static Bitmap Load(string path) {
        // An upscaled strip (texpack PNG) is read as it is.
        if (path.EndsWith(".png", StringComparison.OrdinalIgnoreCase)) {
            var png = new Bitmap(path);
            var copy = new Bitmap(png.Width, png.Height, PixelFormat.Format32bppArgb);
            using (var g = Graphics.FromImage(copy)) {
                g.CompositingMode = CompositingMode.SourceCopy;
                g.DrawImage(png, 0, 0, png.Width, png.Height);
            }
            png.Dispose();
            return copy;
        }
        byte[] b = File.ReadAllBytes(path);
        var bmp = new Bitmap(W, H, PixelFormat.Format32bppArgb);
        var d = bmp.LockBits(new Rectangle(0, 0, W, H), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        for (int y = 0; y < H; y++)
            Marshal.Copy(b, 54 + y * W * 4, d.Scan0 + y * d.Stride, W * 4);
        bmp.UnlockBits(d);
        return bmp;
    }

    static void Save(Bitmap bmp, string path) {
        if (path.EndsWith(".png", StringComparison.OrdinalIgnoreCase)) {
            bmp.Save(path, ImageFormat.Png);
            return;
        }
        var d = bmp.LockBits(new Rectangle(0, 0, W, H), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        byte[] px = new byte[W * H * 4];
        for (int y = 0; y < H; y++)
            Marshal.Copy(d.Scan0 + y * d.Stride, px, y * W * 4, W * 4);
        bmp.UnlockBits(d);
        byte[] hdr = new byte[54];
        hdr[0] = (byte)'B'; hdr[1] = (byte)'M';
        BitConverter.GetBytes(54 + px.Length).CopyTo(hdr, 2);
        hdr[10] = 54; hdr[14] = 40;
        BitConverter.GetBytes(W).CopyTo(hdr, 18);
        BitConverter.GetBytes(-H).CopyTo(hdr, 22);      // top-down
        hdr[26] = 1; hdr[28] = 32;
        using (var f = File.Create(path)) { f.Write(hdr, 0, 54); f.Write(px, 0, px.Length); }
    }

    static void Clear(Graphics g, RectangleF r, bool ellipse) {
        var old = g.CompositingMode;
        g.CompositingMode = CompositingMode.SourceCopy;
        using (var t = new SolidBrush(Color.Transparent)) {
            if (ellipse) g.FillEllipse(t, r); else g.FillRectangle(t, r);
        }
        g.CompositingMode = old;
    }

    // A round face button: dark body, light rim, the symbol in its colour
    // (PS4) or white (DualSense).
    static void Face(Graphics g, float cx, float cy, float d, string sym, bool white) {
        var r = new RectangleF(cx - d / 2, cy - d / 2, d, d);
        using (var body = new LinearGradientBrush(r, Color.FromArgb(255, 70, 70, 74),
                                                  Color.FromArgb(255, 14, 14, 16), 90f))
            g.FillEllipse(body, r);
        using (var rim = new Pen(Color.FromArgb(255, 200, 200, 205), d / 14f))
            g.DrawEllipse(rim, r);
        float s = d * 0.27f, w = d / 9f;
        Color c;
        switch (sym) {
            case "cross":    c = Color.FromArgb(255, 124, 178, 232); break;
            case "circle":   c = Color.FromArgb(255, 255, 102, 102); break;
            case "square":   c = Color.FromArgb(255, 230, 140, 200); break;
            default:         c = Color.FromArgb(255, 64, 226, 160); break;
        }
        if (white)
            c = Color.FromArgb(255, 240, 240, 244);
        using (var p = new Pen(c, w)) {
            p.StartCap = p.EndCap = LineCap.Round;
            p.LineJoin = LineJoin.Round;
            if (sym == "cross") {
                g.DrawLine(p, cx - s, cy - s, cx + s, cy + s);
                g.DrawLine(p, cx - s, cy + s, cx + s, cy - s);
            } else if (sym == "circle") {
                g.DrawEllipse(p, cx - s, cy - s, 2 * s, 2 * s);
            } else if (sym == "square") {
                g.DrawRectangle(p, cx - s * 0.9f, cy - s * 0.9f, 1.8f * s, 1.8f * s);
            } else {
                float h = s * 1.05f;
                g.DrawPolygon(p, new PointF[] {
                    new PointF(cx, cy - h), new PointF(cx + h * 1.05f, cy + h * 0.72f),
                    new PointF(cx - h * 1.05f, cy + h * 0.72f) });
            }
        }
    }

    // A shoulder or trigger: a rounded dark key with its label.
    static void Key(Graphics g, RectangleF r, string label) {
        float rad = r.Height * 0.32f;
        using (var path = new GraphicsPath()) {
            path.AddArc(r.X, r.Y, rad * 2, rad * 2, 180, 90);
            path.AddArc(r.Right - rad * 2, r.Y, rad * 2, rad * 2, 270, 90);
            path.AddArc(r.Right - rad * 2, r.Bottom - rad * 2, rad * 2, rad * 2, 0, 90);
            path.AddArc(r.X, r.Bottom - rad * 2, rad * 2, rad * 2, 90, 90);
            path.CloseFigure();
            using (var body = new LinearGradientBrush(r, Color.FromArgb(255, 70, 70, 74),
                                                      Color.FromArgb(255, 14, 14, 16), 90f))
                g.FillPath(body, path);
            using (var rim = new Pen(Color.FromArgb(255, 200, 200, 205), 2f))
                g.DrawPath(rim, path);
        }
        using (var f = new Font("Arial", r.Height * 0.42f, FontStyle.Bold, GraphicsUnit.Pixel))
        using (var sf = new StringFormat { Alignment = StringAlignment.Center,
                                           LineAlignment = StringAlignment.Center })
            g.DrawString(label, f, Brushes.White, r, sf);
    }

    public static void Make(string src, string dst, bool white) {
        using (var bmp = Load(src))
        using (var g = Graphics.FromImage(bmp)) {
            // The layout below is in the 512x64 strip's pixels; a larger
            // source is drawn at its own scale.
            g.ScaleTransform(bmp.Width / (float)W, bmp.Height / (float)H);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            const float cy = 18.5f;
            // Y with move arrows: a triangle button inside the arrows.
            // A little wider than the old button: an upscaled strip's glow spreads.
            Clear(g, new RectangleF(5.25f, 5, 23, 28), true);
            Face(g, 16.5f, cy, 20, "triangle", white);
            // Triggers.
            Clear(g, new RectangleF(39, 0, 34, 37), false);
            Key(g, new RectangleF(41, 3, 30, 31), "R2");
            Clear(g, new RectangleF(227, 0, 35, 37), false);
            Key(g, new RectangleF(229, 3, 31, 31), "L2");
            // Face buttons, centred on the old ovals.
            foreach (var c in new[] { new { x0 = 83, x1 = 106, s = "cross" },
                                      new { x0 = 120, x1 = 143, s = "square" },
                                      new { x0 = 157, x1 = 180, s = "triangle" },
                                      new { x0 = 194, x1 = 217, s = "circle" } }) {
                Clear(g, new RectangleF(c.x0 - 2, 0, c.x1 - c.x0 + 5, 37), false);
                Face(g, (c.x0 + c.x1) / 2f + 0.5f, cy, 24, c.s, white);
            }
            // White (right shoulder) and Black (left shoulder).
            Clear(g, new RectangleF(270, 0, 28, 37), false);
            Key(g, new RectangleF(271, 8, 26, 21), "R1");
            Clear(g, new RectangleF(307, 0, 28, 37), false);
            Key(g, new RectangleF(308, 8, 26, 21), "L1");
            Save(bmp, dst);
        }
    }
}
"@
$full = [System.IO.Path]::GetFullPath($Out)
[PsButtons]::Make((Resolve-Path $Source).Path, $full, $Style -eq "ps5")
"wrote $full"
