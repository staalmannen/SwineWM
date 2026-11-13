# Regenerates SwineWM's default wallpaper using Windows' built-in GDI+.
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
$drawingAssembly = [System.Drawing.Bitmap].Assembly.Location
Add-Type -ReferencedAssemblies $drawingAssembly -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;

public static class SwineWallpaper {
    static readonly Color Ink = Color.FromArgb(65, 43, 69);
    static readonly Color Pink = Color.FromArgb(255, 190, 211);
    static readonly Color PinkLight = Color.FromArgb(255, 218, 228);
    static readonly Color Rose = Color.FromArgb(245, 128, 164);
    static readonly Color Lip = Color.FromArgb(206, 48, 101);
    static readonly Color Gold = Color.FromArgb(255, 213, 135);

    static SolidBrush Brush(Color color) { return new SolidBrush(color); }

    static void Ellipse(Graphics g, Color color, float x, float y, float w, float h) {
        using (SolidBrush b = Brush(color)) g.FillEllipse(b, x, y, w, h);
    }

    static void OutlineEllipse(Graphics g, Color color, float width,
            float x, float y, float w, float h) {
        using (Pen p = new Pen(color, width)) {
            p.LineJoin = LineJoin.Round;
            g.DrawEllipse(p, x, y, w, h);
        }
    }

    static void Polygon(Graphics g, Color color, PointF[] points) {
        using (SolidBrush b = Brush(color)) g.FillPolygon(b, points);
    }

    static GraphicsPath RoundedRectangle(RectangleF rectangle, float radius) {
        float diameter = radius * 2;
        GraphicsPath path = new GraphicsPath();
        path.AddArc(rectangle.X, rectangle.Y, diameter, diameter, 180, 90);
        path.AddArc(rectangle.Right - diameter, rectangle.Y, diameter, diameter, 270, 90);
        path.AddArc(rectangle.Right - diameter, rectangle.Bottom - diameter,
            diameter, diameter, 0, 90);
        path.AddArc(rectangle.X, rectangle.Bottom - diameter, diameter, diameter, 90, 90);
        path.CloseFigure();
        return path;
    }

    static void Line(Graphics g, Color color, float width, PointF a, PointF b) {
        using (Pen p = new Pen(color, width)) {
            p.StartCap = LineCap.Round;
            p.EndCap = LineCap.Round;
            g.DrawLine(p, a, b);
        }
    }

    static void Star(Graphics g, float x, float y, float size, Color color) {
        PointF[] points = new PointF[8];
        for (int i = 0; i < points.Length; i++) {
            double angle = -Math.PI / 2 + i * Math.PI / 4;
            float radius = i % 2 == 0 ? size : size * 0.24f;
            points[i] = new PointF(x + (float)Math.Cos(angle) * radius,
                y + (float)Math.Sin(angle) * radius);
        }
        Polygon(g, color, points);
    }

    static void Heart(Graphics g, float x, float y, float size, Color color) {
        using (GraphicsPath path = new GraphicsPath()) {
            path.AddBezier(x, y + size * .34f, x - size * .18f, y - size * .08f,
                x - size * .62f, y + size * .05f, x - size * .48f, y + size * .42f);
            path.AddBezier(x - size * .48f, y + size * .42f, x - size * .34f,
                y + size * .67f, x - size * .08f, y + size * .83f, x, y + size);
            path.AddBezier(x, y + size, x + size * .08f, y + size * .83f,
                x + size * .34f, y + size * .67f, x + size * .48f, y + size * .42f);
            path.AddBezier(x + size * .48f, y + size * .42f, x + size * .62f,
                y + size * .05f, x + size * .18f, y - size * .08f, x, y + size * .34f);
            using (SolidBrush b = Brush(color)) g.FillPath(b, path);
        }
    }

    static void KissMark(Graphics g, float x, float y, float scale) {
        using (GraphicsPath path = new GraphicsPath()) {
            path.AddBezier(x, y + 8 * scale, x - 25 * scale, y - 14 * scale,
                x - 47 * scale, y + 3 * scale, x - 37 * scale, y + 23 * scale);
            path.AddBezier(x - 37 * scale, y + 23 * scale, x - 28 * scale,
                y + 43 * scale, x - 8 * scale, y + 48 * scale, x, y + 57 * scale);
            path.AddBezier(x, y + 57 * scale, x + 8 * scale, y + 48 * scale,
                x + 28 * scale, y + 43 * scale, x + 37 * scale, y + 23 * scale);
            path.AddBezier(x + 37 * scale, y + 23 * scale, x + 47 * scale,
                y + 3 * scale, x + 25 * scale, y - 14 * scale, x, y + 8 * scale);
            using (SolidBrush b = Brush(Lip)) g.FillPath(b, path);
            using (Pen p = new Pen(Color.FromArgb(255, 194, 206), 3 * scale))
                g.DrawBezier(p, x - 22 * scale, y + 10 * scale, x - 14 * scale,
                    y + 3 * scale, x - 7 * scale, y + 10 * scale, x - 2 * scale,
                    y + 13 * scale);
        }
    }

    static void Lipstick(Graphics g) {
        GraphicsState state = g.Save();
        g.TranslateTransform(1350, 505);
        g.RotateTransform(13);

        using (GraphicsPath caseShape = RoundedRectangle(new RectangleF(-44, 15, 88, 245), 13)) {
            using (SolidBrush b = Brush(Ink)) g.FillPath(b, caseShape);
            using (Pen p = new Pen(Color.FromArgb(140, 113, 141), 4))
                g.DrawPath(p, caseShape);
        }
        Ellipse(g, Gold, -47, 34, 94, 19);
        Ellipse(g, Color.FromArgb(126, 78, 111), -44, 225, 88, 25);
        Polygon(g, Lip, new PointF[] {
            new PointF(-38, 29), new PointF(-38, -66), new PointF(-20, -118),
            new PointF(10, -129), new PointF(38, -99), new PointF(38, 29)
        });
        Ellipse(g, Color.FromArgb(255, 155, 181), -19, -93, 14, 58);
        Line(g, Color.FromArgb(255, 231, 236), 5,
            new PointF(-72, -147), new PointF(-83, -174));
        Line(g, Color.FromArgb(255, 231, 236), 5,
            new PointF(-90, -128), new PointF(-117, -134));
        g.Restore(state);
    }

    public static void Generate(string output) {
        const int width = 1920;
        const int height = 1080;
        using (Bitmap bitmap = new Bitmap(width, height, PixelFormat.Format24bppRgb))
        using (Graphics g = Graphics.FromImage(bitmap)) {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;

            using (LinearGradientBrush background = new LinearGradientBrush(
                    new Rectangle(0, 0, width, height),
                    Color.FromArgb(43, 35, 60), Color.FromArgb(30, 31, 51), 35f))
                g.FillRectangle(background, 0, 0, width, height);

            Ellipse(g, Color.FromArgb(26, 245, 178, 205), 395, 100, 1130, 760);
            Star(g, 445, 295, 23, Gold);
            Star(g, 1473, 300, 18, Color.FromArgb(255, 220, 162));
            Star(g, 520, 610, 12, Color.FromArgb(255, 220, 162));
            Star(g, 1420, 665, 25, Gold);
            Star(g, 700, 180, 11, PinkLight);
            Star(g, 1235, 202, 12, PinkLight);
            Heart(g, 495, 440, 42, Color.FromArgb(215, 245, 128, 164));
            Heart(g, 1455, 490, 34, Color.FromArgb(190, 245, 128, 164));
            Ellipse(g, Color.FromArgb(175, 255, 218, 228), 360, 495, 13, 13);
            Ellipse(g, Color.FromArgb(175, 255, 218, 228), 1540, 400, 10, 10);
            Ellipse(g, Color.FromArgb(175, 255, 218, 228), 1290, 720, 12, 12);

            Ellipse(g, Color.FromArgb(80, 14, 12, 28), 635, 738, 650, 54);

            Polygon(g, Rose, new PointF[] {
                new PointF(688, 310), new PointF(650, 117), new PointF(840, 248),
                new PointF(893, 330)
            });
            Polygon(g, PinkLight, new PointF[] {
                new PointF(704, 271), new PointF(681, 158), new PointF(790, 246),
                new PointF(827, 292)
            });
            Polygon(g, Rose, new PointF[] {
                new PointF(1232, 310), new PointF(1270, 117), new PointF(1080, 248),
                new PointF(1027, 330)
            });
            Polygon(g, PinkLight, new PointF[] {
                new PointF(1216, 271), new PointF(1239, 158), new PointF(1130, 246),
                new PointF(1093, 292)
            });

            Ellipse(g, Pink, 635, 200, 650, 590);
            OutlineEllipse(g, Ink, 13, 635, 200, 650, 590);
            Ellipse(g, Color.FromArgb(255, 255, 214, 225), 720, 265, 186, 70);

            Ellipse(g, Rose, 722, 480, 128, 77);
            Ellipse(g, Rose, 1070, 480, 128, 77);

            using (Pen eye = new Pen(Ink, 15)) {
                eye.StartCap = LineCap.Round; eye.EndCap = LineCap.Round;
                eye.LineJoin = LineJoin.Round;
                g.DrawBezier(eye, 797, 393, 815, 425, 851, 425, 873, 393);
                g.DrawBezier(eye, 1037, 393, 1059, 425, 1095, 425, 1113, 393);
            }
            Line(g, Ink, 11, new PointF(799, 397), new PointF(777, 377));
            Line(g, Ink, 10, new PointF(824, 409), new PointF(817, 382));
            Line(g, Ink, 11, new PointF(1111, 397), new PointF(1133, 377));
            Line(g, Ink, 10, new PointF(1086, 409), new PointF(1093, 382));

            Ellipse(g, Rose, 812, 418, 296, 207);
            OutlineEllipse(g, Color.FromArgb(228, 86, 116), 10, 812, 418, 296, 207);
            Ellipse(g, Ink, 864, 488, 43, 64);
            Ellipse(g, Ink, 1013, 488, 43, 64);
            Ellipse(g, Color.FromArgb(255, 215, 225), 875, 499, 12, 18);
            Ellipse(g, Color.FromArgb(255, 215, 225), 1024, 499, 12, 18);

            KissMark(g, 1128, 473, .72f);
            Ellipse(g, Color.FromArgb(255, 227, 233), 1158, 493, 7, 7);
            Ellipse(g, Color.FromArgb(255, 227, 233), 1102, 514, 6, 6);

            Lipstick(g);
            Star(g, 1290, 353, 14, Gold);

            using (Font font = new Font("Segoe UI", 55f, FontStyle.Bold, GraphicsUnit.Pixel))
            using (SolidBrush text = Brush(Color.FromArgb(255, 239, 190, 209))) {
                StringFormat format = new StringFormat();
                format.Alignment = StringAlignment.Center;
                g.DrawString("SwineWM", font, text, new PointF(960, 884), format);
            }
            using (Font font = new Font("Segoe UI", 19f, FontStyle.Regular, GraphicsUnit.Pixel))
            using (SolidBrush text = Brush(Color.FromArgb(205, 215, 184, 203))) {
                StringFormat format = new StringFormat();
                format.Alignment = StringAlignment.Center;
                g.DrawString("A little oink of glam", font, text, new PointF(960, 952), format);
            }

            bitmap.Save(output, ImageFormat.Png);
        }
    }
}
'@

$outputPath = Join-Path $PSScriptRoot 'swinewm-default.png'
[SwineWallpaper]::Generate($outputPath)
Write-Output "Generated $outputPath"
