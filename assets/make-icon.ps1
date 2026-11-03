# Makes the Mechano Reader icon from Eslam's logo (00-data\brand\mechano-reader-logo.png, 2026-11-02):
# the "M" mark (eye + zipper) left of the divider line is cut out, its white background and soft shadow are made
# transparent (flood fill from the border, so the white eye and zipper inside stay white), the edges are cleaned
# ("colour to alpha" against white), then it is centred on a square and saved as app.ico (16 ... 256 px, PNG
# entries) + app-256.png.
# Run again after changing the logo:  powershell -ExecutionPolicy Bypass -File assets\make-icon.ps1
Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'
$logo = Join-Path $PSScriptRoot '..\..\00-data\brand\mechano-reader-logo.png'
$out = Join-Path $PSScriptRoot 'app.ico'

if (-not ('IconCut' -as [type])) {
Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Collections.Generic; using System.Drawing; using System.Drawing.Imaging;
public static class IconCut {
    static bool White(Color c) { return Math.Min(c.R, Math.Min(c.G, c.B)) >= 238; }
    static bool Plain(Color c) {                      // white / light grey background or its soft shadow
        int mx = Math.Max(c.R, Math.Max(c.G, c.B)), mn = Math.Min(c.R, Math.Min(c.G, c.B));
        return mx - mn <= 22 && mx >= 150;
    }
    // Pass 1: pure white connected to the border. Pass 2: grey shadow touching that white, but only below
    // the mark's lowest coloured row band (the shadow under the legs), so the white eye / zipper stay.
    static void Flood(Color[,] px, bool[,] bg, int w, int h, Func<int, int, bool> ok, IEnumerable<Point> seeds) {
        var q = new Queue<Point>(seeds);
        while (q.Count > 0) {
            var p = q.Dequeue();
            if (p.X < 0 || p.Y < 0 || p.X >= w || p.Y >= h || bg[p.X, p.Y] || !ok(p.X, p.Y)) continue;
            bg[p.X, p.Y] = true;
            q.Enqueue(new Point(p.X + 1, p.Y)); q.Enqueue(new Point(p.X - 1, p.Y));
            q.Enqueue(new Point(p.X, p.Y + 1)); q.Enqueue(new Point(p.X, p.Y - 1));
        }
    }
    // The mark: everything left of the vertical divider (first column with a long thin grey line).
    public static Bitmap Cut(Bitmap src, int maxX) {
        int w = Math.Min(maxX, src.Width), h = src.Height;
        var px = new Color[w, h];
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) px[x, y] = src.GetPixel(x, y);
        // background = pure white connected to the border
        var bg = new bool[w, h];
        var seeds = new List<Point>();
        for (int x = 0; x < w; x++) { seeds.Add(new Point(x, 0)); seeds.Add(new Point(x, h - 1)); }
        for (int y = 0; y < h; y++) { seeds.Add(new Point(0, y)); seeds.Add(new Point(w - 1, y)); }
        Flood(px, bg, w, h, (x, y) => White(px[x, y]), seeds);
        // the soft grey shadow under the legs and the white gap between them (bottom 45 %), but never the
        // zipper's pull in the middle column (it is white too)
        int x0c = w, x1c = 0;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (!bg[x, y]) { x0c = Math.Min(x0c, x); x1c = Math.Max(x1c, x); }
        int cx = (x0c + x1c) / 2, half = Math.Max(2, (x1c - x0c) * 55 / 1000);
        int lowTop = h * 55 / 100;
        var s2 = new List<Point>();
        for (int y = lowTop; y < h; y++) for (int x = 0; x < w; x++) if (bg[x, y]) s2.Add(new Point(x, y));
        var bg2 = new bool[w, h];
        Func<Color, bool> shadow = c => {
            int mx = Math.Max(c.R, Math.Max(c.G, c.B)), mn = Math.Min(c.R, Math.Min(c.G, c.B));
            return mx - mn <= 45 && mx >= 120;                // greyish / faintly purple shadow
        };
        Flood(px, bg2, w, h, (x, y) => y >= lowTop && (bg[x, y] ||
            (Math.Abs(x - cx) > half ? (Plain(px[x, y]) || shadow(px[x, y])) : (shadow(px[x, y]) && !White(px[x, y]) && y > h * 93 / 100))), s2);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (bg2[x, y]) bg[x, y] = true;
        var res = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        int x0 = w, y0 = h, x1 = -1, y1 = -1;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            Color c = px[x, y];
            if (bg[x, y]) { res.SetPixel(x, y, Color.Transparent); continue; }
            bool edge = false;
            for (int dy = -2; dy <= 2 && !edge; dy++) for (int dx = -2; dx <= 2 && !edge; dx++) {
                int xx = x + dx, yy = y + dy;
                if (xx >= 0 && yy >= 0 && xx < w && yy < h && bg[xx, yy]) edge = true;
            }
            if (edge) {                                // colour to alpha against white: no white fringe
                int a = Math.Max(255 - c.R, Math.Max(255 - c.G, 255 - c.B));
                if (a < 8) { res.SetPixel(x, y, Color.Transparent); continue; }
                Func<int, int> un = v => Math.Max(0, Math.Min(255, 255 - (255 - v) * 255 / a));
                c = Color.FromArgb(a, un(c.R), un(c.G), un(c.B));
            }
            res.SetPixel(x, y, c);
            if (c.A > 40) { x0 = Math.Min(x0, x); y0 = Math.Min(y0, y); x1 = Math.Max(x1, x); y1 = Math.Max(y1, y); }
        }
        var crop = res.Clone(Rectangle.FromLTRB(x0, y0, x1 + 1, y1 + 1), PixelFormat.Format32bppArgb);
        res.Dispose();
        return crop;
    }
    public static int Divider(Bitmap src) {          // x of the thin vertical line between the mark and the text
        for (int x = src.Width / 4; x < src.Width * 3 / 4; x++) {
            int run = 0;
            for (int y = 0; y < src.Height; y++) {
                Color c = src.GetPixel(x, y);
                bool line = c.R < 230 && Math.Abs(c.R - c.G) < 30 && Math.Abs(c.G - c.B) < 40;
                bool sideWhite = x > 2 && src.GetPixel(x - 3, y).R > 240 && src.GetPixel(x + 3, y).R > 240;
                run = line && sideWhite ? run + 1 : 0;
                if (run > src.Height / 2) return x - 2;
            }
        }
        return src.Width / 2;
    }
}
"@
}

$src = [System.Drawing.Bitmap]::FromFile((Resolve-Path $logo))
$mark = [IconCut]::Cut($src, [IconCut]::Divider($src))
$src.Dispose()
Write-Host "Mark: $($mark.Width) x $($mark.Height)"

function Draw([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'; $g.SmoothingMode = 'AntiAlias'
    $margin = if ($s -le 24) { 0 } else { [Math]::Round($s * 0.03) }
    $box = $s - 2 * $margin
    $k = [Math]::Min($box / $mark.Width, $box / $mark.Height)
    $w = $mark.Width * $k; $h = $mark.Height * $k
    $g.DrawImage($mark, [float](($s - $w) / 2), [float](($s - $h) / 2), [float]$w, [float]$h)
    $g.Dispose()
    return $bmp
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 96, 128, 256
$pngs = foreach ($s in $sizes) {
    $b = Draw $s
    $ms = New-Object System.IO.MemoryStream
    $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose()
    , $ms.ToArray()
}
# ICO container: header, one directory entry per size, then the PNG data.
$fs = [System.IO.File]::Create($out)
$w = New-Object System.IO.BinaryWriter $fs
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $len = $pngs[$i].Length
    $w.Write([byte]($(if ($s -ge 256) { 0 } else { $s }))); $w.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $w.Write([byte]0); $w.Write([byte]0); $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$len); $w.Write([uint32]$offset); $offset += $len
}
foreach ($p in $pngs) { $w.Write($p) }
$w.Close()
# Also a 256 px PNG (README, about box) and the mark itself at full size.
$b = Draw 256; $b.Save((Join-Path $PSScriptRoot 'app-256.png'), [System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose()
$mark.Save((Join-Path $PSScriptRoot 'logo-mark.png'), [System.Drawing.Imaging.ImageFormat]::Png); $mark.Dispose()
Write-Host "Wrote $out"
